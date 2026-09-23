#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mtop {
struct Device {
    uint64_t registry = 0, build = 0, stage = 0, visible = 0, total = 0;
    uint32_t gfx[3]{};
    std::string error;
};

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
