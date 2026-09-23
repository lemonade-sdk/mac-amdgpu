#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "../dext/amdgpu/amdgpu_metrics_state.h"

namespace mtop {
struct Device {
    uint64_t registry = 0, build = 0, stage = 0, visible = 0, total = 0;
    uint32_t gfx[3]{};
    std::string error;
    bool telemetrySupported = false;
    std::string telemetryError;
    amdgpu::SMUMetricsSnapshot metrics{};
};

inline bool validSnapshot(const amdgpu::SMUMetricsSnapshot &s) {
    constexpr uint32_t flags = amdgpu::kSMUMetricsValid | amdgpu::kSMUMetricsFaulted |
                               amdgpu::kSMUMetricsStale;
    constexpr uint64_t fields = (uint64_t(1) << amdgpu::metrics::Count) - 1;
    return s.version == amdgpu::kSMUMetricsSnapshotVersion && s.size == sizeof(s) &&
           !(s.flags & ~flags) && !(s.validFields & ~fields) &&
           (!(s.flags & amdgpu::kSMUMetricsValid) ||
            (s.status == 0 && s.validFields != 0 &&
             !(s.flags & (amdgpu::kSMUMetricsFaulted | amdgpu::kSMUMetricsStale))));
}

inline bool fresh(const Device &d, uint64_t now) {
    return d.telemetrySupported && d.telemetryError.empty() && validSnapshot(d.metrics) &&
        amdgpu::metrics::verified_interface(d.metrics.driverInterface) &&
        (d.metrics.flags & amdgpu::kSMUMetricsValid) && now >= d.metrics.collectedAtNs &&
        now - d.metrics.collectedAtNs <= amdgpu::kSMUMetricsStaleAfterNs;
}

inline bool interfaceMismatch(const Device &d) {
    return d.telemetrySupported && validSnapshot(d.metrics) &&
        d.metrics.driverInterface != 0 &&
        !amdgpu::metrics::verified_interface(d.metrics.driverInterface);
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
