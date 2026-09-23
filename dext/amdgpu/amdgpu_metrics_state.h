#pragma once

#include "amdgpu_metrics.h"

namespace amdgpu {

enum SMUMetricsFlags : uint32_t {
    kSMUMetricsValid = 1u << 0,
    kSMUMetricsFaulted = 1u << 1,
    kSMUMetricsStale = 1u << 2,
};

// Fixed-width observer payload. The caller must validate version and size.
// Values use the units declared by metrics::Field; validFields is authoritative.
struct SMUMetricsSnapshot {
    uint32_t version;
    uint32_t size;
    uint32_t status; // kern_return_t bit pattern, including the latest error.
    uint32_t flags;
    uint64_t generation;
    uint64_t sequence;
    uint64_t collectedAtNs; // CLOCK_UPTIME_RAW, last successful complete sample.
    uint64_t attemptedAtNs;
    uint32_t driverInterface;
    uint32_t firmwareCounter;
    uint64_t validFields;
    uint64_t values[metrics::Count];
};
static_assert(sizeof(SMUMetricsSnapshot) == 192);

struct SMUMetricsContext {
    // Reserved inside PSP's persistent firmware arena, not separately owned.
    // Keep these coordinates after partial address programming or a timeout.
    uint64_t tableMC;
    uint64_t tableVRAMOffset;
    uint64_t tableBytes;
    uint32_t driverInterface;
    bool reserved;
    bool vramBacked;
    bool addressProgrammed;
    bool setupComplete;
    bool collecting;
    bool faulted;
    SMUMetricsSnapshot snapshot;
};

constexpr uint32_t kSMUMetricsSnapshotVersion = 1;
constexpr uint64_t kSMUMetricsMinIntervalNs = 1000000000ull;
constexpr uint64_t kSMUMetricsStaleAfterNs = 2500000000ull;

// Validate before changing the firmware bump pointer or sending address words.
// A zero MC address is allowed; ownership is recorded by reserved, not address.
inline bool smu_metrics_reserve(SMUMetricsContext &ctx, uint64_t arenaMC,
    uint64_t arenaVRAM, uint64_t arenaBytes, uint64_t slot, uint64_t bytes,
    bool vramBacked, uint64_t generation) {
    if (ctx.reserved || !bytes || (slot & 4095) || (bytes & 4095) ||
        slot > arenaBytes || bytes > arenaBytes - slot ||
        slot > UINT64_MAX - arenaMC || bytes > UINT64_MAX - (arenaMC + slot) ||
        slot > UINT64_MAX - arenaVRAM || bytes > UINT64_MAX - (arenaVRAM + slot))
        return false;
    ctx.tableMC = arenaMC + slot;
    ctx.tableVRAMOffset = arenaVRAM + slot;
    ctx.tableBytes = bytes;
    ctx.reserved = true;
    ctx.vramBacked = vramBacked;
    ctx.snapshot.version = kSMUMetricsSnapshotVersion;
    ctx.snapshot.size = sizeof(SMUMetricsSnapshot);
    ctx.snapshot.generation = generation;
    ctx.snapshot.driverInterface = ctx.driverInterface;
    return true;
}

} // namespace amdgpu
