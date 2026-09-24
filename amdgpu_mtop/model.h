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
};

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
