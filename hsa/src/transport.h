#pragma once

#include <hsa/hsa.h>
#include <memory>
#include <vector>

namespace mac_hsa {

struct DeviceSnapshot {
    uint64_t registryID = 0;
    uint64_t build = 0;
    uint64_t stage = 0;
    uint64_t visibleVRAM = 0;
    uint64_t totalVRAM = 0;
    uint32_t gfxMajor = 0, gfxMinor = 0, gfxRevision = 0;
};

class Connection {
public:
    virtual ~Connection() = default;
    virtual hsa_status_t read(DeviceSnapshot &snapshot) = 0;
};

// Only opens observer clients and calls RuntimeBuild/QueryInfo. No ownership,
// initialization, queue submission, PCI configuration or reset is performed.
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections);

} // namespace mac_hsa
