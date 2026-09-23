#include "amdgpu_metrics_state.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using kern_return_t = int32_t;
enum { kIOReturnSuccess = 0, kIOReturnNotReady = 1, kIOReturnNotAttached = 2,
       kIOReturnUnsupported = 3, kIOReturnBadArgument = 4, kIOReturnTimeout = 5,
       kIOReturnIOError = 6, kIOReturnBusy = 7 };

namespace amdgpu {
static uint64_t now = 1, readCost = 100;
static unsigned commands = 0, reads = 0;
static kern_return_t commandResult = kIOReturnSuccess;
static bool populate = true;
struct FakePCI {
    uint8_t bytes[65536]{};
    void MemoryRead32(uint8_t, uint64_t offset, uint32_t *word) {
        assert(commands == 1 && commandResult == kIOReturnSuccess);
        assert(offset + 4 <= sizeof(bytes));
        memcpy(word, bytes + offset, 4);
        now += readCost;
        ++reads;
    }
};
struct DeviceContext {
    FakePCI *pci;
    uint8_t bar0MemIndex = 0;
    uint64_t bar0Size = 65536;
    bool smuOnline = true, gmcReady = true, smuMessagePending = false;
};
static constexpr struct { uint32_t major, minor, rev; } kIP_SMU{14, 0, 3};
#define clock_gettime_nsec_np fake_clock
#undef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW 0
static uint64_t fake_clock(int) { return now; }
static bool vram_io_range(const DeviceContext &d, uint64_t off, uint64_t length) {
    return length && !(off & 3) && !(length & 3) && off <= d.bar0Size && length <= d.bar0Size - off;
}
static kern_return_t smu_transfer_table_smu_to_dram(const DeviceContext &d, uint32_t table) {
    assert(table == 5);
    ++commands;
    if (populate && commandResult == kIOReturnSuccess) {
        d.pci->bytes[104] = 9; // Firmware counter, with otherwise legitimate idle data.
        d.pci->bytes[124] = 27;
        d.pci->bytes[126] = 31;
        d.pci->bytes[136] = 75;
    }
    return commandResult;
}
#include "amdgpu_metrics_collection.inc"
} // namespace amdgpu

using namespace amdgpu;
static SMUMetricsContext ready() {
    SMUMetricsContext ctx{};
    ctx.driverInterface = 0x2e;
    assert(smu_metrics_reserve(ctx, 0, 0, 65536, 0, 65536, true, 19));
    ctx.addressProgrammed = ctx.setupComplete = true;
    commands = reads = 0;
    now = 1;
    readCost = 100;
    commandResult = kIOReturnSuccess;
    populate = true;
    return ctx;
}
int main() {
    FakePCI pci;
    DeviceContext dev{&pci};
    auto ctx = ready();
    assert(smu_collect_metrics(dev, ctx, true) == 0 && commands == 1 && reads == 103);
    assert(ctx.snapshot.generation == 19 && ctx.snapshot.sequence == 1);
    assert(ctx.snapshot.values[metrics::SocketPowerMilliwatts] == 75000);
    assert(ctx.snapshot.values[metrics::UmcActivityPercent] == 31);
    assert(ctx.snapshot.firmwareCounter == 9 && ctx.snapshot.flags == kSMUMetricsValid);
    const auto collected = ctx.snapshot.collectedAtNs;
    assert(smu_collect_metrics(dev, ctx, true) == 0 && commands == 1); // Shared cache interval.
    SMUMetricsSnapshot snap{};
    smu_metrics_snapshot(ctx, true, snap);
    assert(snap.validFields && snap.size == 192 && snap.version == 1);
    now = collected + kSMUMetricsStaleAfterNs + 1;
    smu_metrics_snapshot(ctx, true, snap);
    assert(!snap.validFields && snap.flags == kSMUMetricsStale && snap.status == kIOReturnNotReady);
    assert(snap.collectedAtNs == collected && commands == 1); // Observer never collects.
    smu_metrics_invalidate(ctx, kIOReturnNotAttached);
    assert(ctx.reserved && ctx.tableBytes == 65536 && !ctx.setupComplete);
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnNotReady && commands == 1);

    ctx = ready();
    commandResult = kIOReturnTimeout;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnTimeout && commands == 1 && !reads);
    assert(ctx.faulted && !ctx.collecting && ctx.reserved && !ctx.snapshot.validFields);
    commandResult = kIOReturnSuccess;
    now += 10000000000ull;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnTimeout && commands == 1);
    smu_metrics_snapshot(ctx, true, snap);
    assert(snap.status == kIOReturnTimeout && snap.flags == kSMUMetricsFaulted);

    ctx = ready(); dev.smuMessagePending = true;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnTimeout && !commands);
    dev.smuMessagePending = false;
    ctx = ready(); ctx.driverInterface = 0x2f;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnUnsupported && !commands);
    ctx = ready(); ctx.vramBacked = false;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnUnsupported && !commands);
    ctx = ready(); ctx.addressProgrammed = false;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnNotReady && !commands);
    ctx = ready(); ctx.tableVRAMOffset = UINT64_MAX - 3;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnBadArgument && !commands);
    ctx = ready(); ctx.collecting = true;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnBusy && !commands);
    ctx = ready();
    assert(smu_collect_metrics(dev, ctx, false) == kIOReturnNotReady && !commands);
    dev.pci = nullptr;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnNotAttached && !commands);
    dev.pci = &pci;
    ctx = ready(); populate = false; memset(pci.bytes, 0xff, sizeof(pci.bytes));
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnIOError && ctx.faulted);
    ctx = ready(); readCost = 110000000;
    assert(smu_collect_metrics(dev, ctx, true) == kIOReturnTimeout && reads == 1 && ctx.faulted);

    ctx = {};
    assert(!smu_metrics_reserve(ctx, UINT64_MAX - 1024, 0, 65536, 0, 65536, true, 1));
    assert(!smu_metrics_reserve(ctx, 0, UINT64_MAX - 1024, 65536, 0, 65536, true, 1));
    assert(!smu_metrics_reserve(ctx, 0, 0, 65535, 0, 65536, true, 1));
    assert(!smu_metrics_reserve(ctx, 0, 0, 65536, 1, 4096, true, 1));
    assert(!smu_metrics_reserve(ctx, 0, 0, 65536, 0, 4095, true, 1));
    assert(!ctx.reserved);
    assert(smu_metrics_reserve(ctx, 0, 0, 65536, 0, 65536, true, 1));
    assert(!smu_metrics_reserve(ctx, 0, 0, 65536, 0, 65536, true, 2));
}
