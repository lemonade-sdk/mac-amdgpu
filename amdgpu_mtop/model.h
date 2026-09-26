#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "../dext/amdgpu/amdgpu_metrics_state.h"
#include "../dext/amdgpu/amdgpu_vram_accounting.h"
#include "../dext/amdgpu/amdgpu_software_stats.h"

namespace mtop {
struct Device {
    uint64_t registry = 0, build = 0, stage = 0, visible = 0, total = 0;
    uint32_t gfx[3]{};
    std::string error;
    bool telemetrySupported = false;
    std::string telemetryError;
    amdgpu::SMUMetricsSnapshot metrics{};
    bool accountingSupported = false;
    std::string accountingError;
    amdgpu::vram_accounting::Snapshot accounting{};
    bool softwareSupported = false;
    std::string softwareError;
    amdgpu::software_stats::Snapshot software{};
    bool clocksSupported = false;
    std::string clocksError;
    amdgpu::SMUClockSnapshot clocks{};
    // MMHUB PERFSTATUS UMC busy (build 198+, selector 68, observer only).
    struct MmhubPerfStatus {
        uint32_t status = 0;          // kern_return_t bit pattern from the driver
        uint32_t raw = 0;             // full PERFSTATUS register value
        uint64_t umcBusyQ8 = 0;       // cumulative UMC busy, 0.25% steps (1048575 = 100% of window)
        uint64_t collectedAtNs = 0;   // CLOCK_UPTIME_RAW sample time
    } mmhub{};
    bool specSupported = false;
    std::string specError;
    struct GfxSpec {
        uint32_t words[32]{};
        bool valid = false;
    } spec;
};
// QueryInfo tag-8 device spec: read the 32 dwords out, keep the ones that
// matter (all-zero is the pre-GC-discovery sentinel), record them. The full
// GFXSpecSnapshot lives in dext/amdgpu/amdgpu_gfx.h; we only need the scalar
// geometry here so the monitor does not drag in the driver header.
inline void readGfxSpec(Device &d, const uint32_t *words) {
    if (!words || !words[0]) return; // header == 0 means GC not yet resolved
    for (unsigned i = 0; i < 32; ++i) d.spec.words[i] = words[i];
    d.spec.valid = true;
    d.specSupported = true;
}

inline bool hasSoftware(const Device &d) {
    return d.error.empty() && d.softwareSupported && d.softwareError.empty() &&
        amdgpu::software_stats::valid(d.software);
}

inline bool hasAccounting(const Device &d) {
    return d.error.empty() && d.accountingSupported && d.accountingError.empty() &&
        amdgpu::vram_accounting::valid(d.accounting) &&
        (d.accounting.values[amdgpu::vram_accounting::Flags] & amdgpu::vram_accounting::kValid);
}

inline bool validSnapshot(const amdgpu::SMUMetricsSnapshot &s) {
    constexpr uint32_t flags = amdgpu::kSMUMetricsValid | amdgpu::kSMUMetricsFaulted |
                               amdgpu::kSMUMetricsStale | amdgpu::kSMUMetricsLinuxCompatible;
    constexpr uint64_t fields = (uint64_t(1) << amdgpu::metrics::Count) - 1;
    return s.version == amdgpu::kSMUMetricsSnapshotVersion && s.size == sizeof(s) &&
           !(s.flags & ~flags) && !(s.validFields & ~fields) &&
           (!(s.flags & amdgpu::kSMUMetricsValid) ||
            (s.status == 0 && s.validFields != 0 &&
             !(s.flags & (amdgpu::kSMUMetricsFaulted | amdgpu::kSMUMetricsStale))));
}

inline bool fresh(const Device &d, uint64_t now) {
    return d.telemetrySupported && d.telemetryError.empty() && validSnapshot(d.metrics) &&
        amdgpu::smu_metrics_profile_supported(d.metrics) &&
        (d.metrics.flags & amdgpu::kSMUMetricsValid) && now >= d.metrics.collectedAtNs &&
        now - d.metrics.collectedAtNs <= amdgpu::kSMUMetricsStaleAfterNs;
}

inline bool validClocks(const Device &d) {
    const auto &s=d.clocks;
    return d.error.empty() && d.clocksSupported && d.clocksError.empty() &&
        s.version==1 && s.size==sizeof(s) &&
        !(s.currentValid & ~15u) && !(s.limitsValid & ~15u);
}

inline bool freshClocks(const Device &d, uint64_t now) {
    const auto &s=d.clocks;
    return validClocks(d) &&
        (s.flags & amdgpu::kSMUMetricsValid) &&
        !(s.flags & (amdgpu::kSMUMetricsFaulted | amdgpu::kSMUMetricsStale)) &&
        now>=s.collectedAtNs && now-s.collectedAtNs<=amdgpu::kSMUMetricsStaleAfterNs;
}

inline bool interfaceMismatch(const Device &d) {
    return d.telemetrySupported && validSnapshot(d.metrics) &&
        d.metrics.driverInterface != 0 &&
        !amdgpu::smu_metrics_profile_supported(d.metrics);
}

// MMHUB PERFSTATUS UMC busy source (build 198+, selector 68). The dext samples
// the MMHUB PERFSTATUS register on its 1 Hz sensor cache and publishes it with
// selector 68 (observer only, like selector 63): out[0]=status, out[1]=raw
// register, out[2]=cumulative UMC busy Q8, out[3]=sample wall time
// (CLOCK_UPTIME_RAW). The counter integrates (busy fraction * 1e9 / 0.25% step)
// per second, so a full-busy window accumulates kMMHUBPerfStatusMaxQ8 per
// second and the register saturates; consumers take deltas and drop wrap /
// saturation regressions, the same way they treat pendingNs.
inline constexpr uint32_t kMMHUBPerfStatusSelector = 68;
inline constexpr uint32_t kMMHUBPerfStatusMinimumBuild = 198;
inline constexpr uint64_t kMMHUBPerfStatusMaxQ8 = 1048575ull; // 20-bit saturating counter
inline bool validMmhub(const Device &d, uint64_t now) {
    // The PERFSTATUS register is only meaningful once the GPU is fully
    // initialized; before stage 15 the driver reports the source unavailable.
    return d.error.empty() && d.build >= kMMHUBPerfStatusMinimumBuild &&
        d.stage == 15 && d.mmhub.status == 0 && d.mmhub.collectedAtNs <= now &&
        now - d.mmhub.collectedAtNs <= amdgpu::kSMUMetricsStaleAfterNs;
}
inline bool freshMmhub(const Device &d, uint64_t now) { return validMmhub(d, now); }
// Delta-based UMC busy percentage over the sample window. previousUmc carries
// the previous sample's cumulative counter and wall time (0/0 when the source
// was absent). A counter regression (saturation wrap or a new counter epoch)
// reports unavailable instead of a fake spike.
inline std::optional<double> mmhubUmcPercent(const Device &d,
                                             uint64_t previousUmcQ8, uint64_t previousUmcAtNs) {
    if (!validMmhub(d, d.mmhub.collectedAtNs)) return {};
    if (!previousUmcAtNs || previousUmcAtNs >= d.mmhub.collectedAtNs) return {};
    if (d.mmhub.umcBusyQ8 < previousUmcQ8) return {}; // saturated / wrapped: no window
    const uint64_t delta = d.mmhub.umcBusyQ8 - previousUmcQ8;
    const uint64_t elapsed = d.mmhub.collectedAtNs - previousUmcAtNs;
    if (elapsed < 1000000ull || elapsed > 2'000'000'000ull) return {};
    // The counter integrates (busy_fraction * 1e9 / 0.25% steps) per second,
    // so a full-busy window accumulates kMMHUBPerfStatusMaxQ8 per second.
    const double ratio = double(delta) / (double(kMMHUBPerfStatusMaxQ8) * double(elapsed / 1e9)) * 100.0;
    if (ratio < 0 || !std::isfinite(ratio)) return {};
    return std::clamp(ratio, 0.0, 100.0);
}

// Selection survives enumeration reordering and removal. Never silently
// replace a removed card with the next array index; replug gets a new ID.
struct Selection {
    std::optional<uint64_t> registry;
    void initialize(const std::vector<Device> &devices) {
        if (!registry && !devices.empty()) registry = devices.front().registry;
    }
    const Device *find(const std::vector<Device> &devices) const {
        for (const auto &d : devices) if (registry == d.registry) return &d;
        return nullptr;
    }
    void step(const std::vector<Device> &devices, bool next) {
        if (devices.empty()) return;
        const auto *current = find(devices);
        const size_t i = current ? size_t(current - devices.data()) : (next ? devices.size() - 1 : 0);
        registry = devices[(i + (next ? 1 : devices.size() - 1)) % devices.size()].registry;
    }
};
std::vector<Device> discover(std::string &error);
} // namespace mtop
