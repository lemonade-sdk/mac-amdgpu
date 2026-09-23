#pragma once

#include <hsa/hsa.h>
#include <memory>
#include <vector>
#include <string>
#include "../../dext/amdgpu/amdgpu_dispatch_abi.h"
#include "../../dext/amdgpu/amdgpu_aql_abi.h"

namespace mac_hsa {

constexpr uint64_t kPersistentQueueDriverBuild=187;

struct DeviceSnapshot {
    uint64_t registryID = 0;
    uint64_t build = 0;
    uint64_t stage = 0;
    uint64_t visibleVRAM = 0;
    uint64_t totalVRAM = 0;
    uint32_t gfxMajor = 0, gfxMinor = 0, gfxRevision = 0;
};

inline bool supportsPersistentQueues(const DeviceSnapshot &snapshot) {
    return snapshot.build >= kPersistentQueueDriverBuild &&
        snapshot.gfxMajor == 12 && snapshot.gfxMinor == 0 && snapshot.gfxRevision == 1;
}

struct DeviceBuffer {
    uint64_t handle = 0, address = 0, size = 0;
};
struct SharedBuffer { DeviceBuffer device; void *host = nullptr; uint32_t memoryType = 0; };
struct BufferToken { uint64_t registryID = 0, token[2]{}, size = 0; };
static_assert(sizeof(BufferToken) == 32);

class Connection {
public:
    virtual ~Connection() = default;
    virtual hsa_status_t read(DeviceSnapshot &snapshot) = 0;
    virtual bool supportsBuffers() const { return false; }
    virtual hsa_status_t memoryCapacity(uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t allocateBuffer(uint64_t, DeviceBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t freeBuffer(const DeviceBuffer &) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t readBuffer(const DeviceBuffer &, uint64_t, void *, size_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t writeBuffer(const DeviceBuffer &, uint64_t, const void *, size_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t allocateSharedBuffer(uint64_t, SharedBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t freeSharedBuffer(const SharedBuffer &) { return HSA_STATUS_ERROR; }
    // Diagnostic only: raw SDMA submission claims an exclusive client lease.
    virtual hsa_status_t testSharedAtomicAdd(const SharedBuffer &, uint64_t, int64_t, uint32_t) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t createQueue(const SharedBuffer &, const SharedBuffer &, uint32_t, uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t kickQueue(uint64_t, uint64_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t destroyQueue(uint64_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &, uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &, uint64_t &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t copyBuffers(const DeviceBuffer &, uint64_t, const DeviceBuffer &, uint64_t, size_t) { return HSA_STATUS_ERROR; }
    virtual hsa_status_t exportBuffer(const DeviceBuffer &, BufferToken &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    virtual hsa_status_t importBuffer(const BufferToken &, DeviceBuffer &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
};

// Only opens observer clients and calls RuntimeBuild/QueryInfo. No ownership,
// initialization, queue submission, PCI configuration or reset is performed.
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections);

} // namespace mac_hsa
