#include "runtime_state.h"
#include "mac_hsa.h"
#include "synchronization_policy.h"
#include <algorithm>
#include <array>
#include <limits>
#include <system_error>

namespace mac_hsa::detail {
void reapCopyJobs() {
    std::vector<std::unique_ptr<CopyJob>> retired;
    {
        std::lock_guard lock(runtimeMutex);
        // Allocate retirement slots before changing the job list.
        retired.reserve(copyJobs.size());
        for (auto &job : copyJobs)
            if (job->done.load(std::memory_order_acquire)) retired.push_back(std::move(job));
        std::erase(copyJobs, nullptr);
    }
    // Completed job captures may release GPU buffers and reenter HSA.
}
Pool *findPool(uint64_t handle) {
    for (auto &pool : pools) if (pool.handle == handle) return &pool;
    return nullptr;
}
std::shared_ptr<Allocation> findAllocation(const void *pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    auto it = allocations.upper_bound(address);
    if (it == allocations.begin()) return {};
    --it;
    return address - it->first < it->second->size ? it->second : nullptr;
}
namespace {
const size_t granule = hostPageSize();
uint32_t poolFlags(const Pool &pool) {
    if (pool.sharedHost) return HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED |
                               HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT;
    return pool.connection ? HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED :
                             HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED;
}
bool poolAccessible(const Pool &pool, const Agent &agent) {
    return pool.owner.handle == agent.handle.handle ||
        (pool.sharedHost && agent.connection == pool.connection);
}
bool validRange(const void *pointer, size_t size, const std::shared_ptr<Allocation> &allocation, bool write = false) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    if (!pointer || size > UINTPTR_MAX - address) return false;
    if (allocation)
        return (allocation->access & (write ? HSA_ACCESS_PERMISSION_WO : HSA_ACCESS_PERMISSION_RO)) &&
            size <= allocation->size - (address - reinterpret_cast<uintptr_t>(allocation->base));
    // Host pointers from the OS allocator are accepted. Reject a range that
    // crosses into a known allocation instead of bypassing its bounds check.
    const auto next = allocations.lower_bound(address);
    return next == allocations.end() || size <= next->first - address;
}
hsa_status_t poolSnapshot(uint64_t handle, Pool &out, hsa_status_t invalidPool) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto pool = findPool(handle);
    if (!pool) return invalidPool;
    out = *pool;
    return HSA_STATUS_SUCCESS;
}
hsa_status_t poolCapacity(const Pool &pool, void *value) {
    uint64_t capacity = pool.capacity;
    if (pool.connection) {
        const auto status = pool.sharedHost ? pool.connection->sharedMemoryCapacity(capacity) :
                                             pool.connection->memoryCapacity(capacity);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    return writeValue(value, size_t(capacity));
}
hsa_status_t allocate(uint64_t poolHandle, size_t size, uint32_t flags, void **out,
                      hsa_status_t invalidPool) {
    Pool pool{};
    const auto status = poolSnapshot(poolHandle, pool, invalidPool);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!size || flags) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const size_t alignment = pool.connection ? 16384 : granule;
    if (size > SIZE_MAX - (alignment - 1)) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    const auto rounded = (size + alignment - 1) & ~(alignment - 1);
    try {
        auto allocation = std::make_shared<Allocation>();
        allocation->globalFlags = poolFlags(pool);
        if (pool.sharedHost) {
            uint64_t capacity = 0;
            auto result = pool.connection->sharedMemoryCapacity(capacity);
            if (result != HSA_STATUS_SUCCESS) return result;
            if (rounded > capacity) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
            allocation->connection = pool.connection;
            result = pool.connection->allocateSharedBuffer(rounded, allocation->shared);
            allocation->buffer = allocation->shared.device;
            if (result != HSA_STATUS_SUCCESS) return result;
            allocation->base = allocation->shared.host;
            allocation->size = allocation->buffer.size;
            const auto address = reinterpret_cast<uintptr_t>(allocation->base);
            if (!address || address != allocation->buffer.address || !allocation->buffer.handle ||
                allocation->size < rounded || allocation->size > UINTPTR_MAX - address)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        } else if (pool.connection) {
            const auto result = pool.connection->allocateBuffer(rounded, allocation->buffer);
            if (result != HSA_STATUS_SUCCESS) return result;
            allocation->connection = pool.connection;
            allocation->base = reinterpret_cast<void *>(allocation->buffer.address);
            if (!allocation->buffer.handle || !allocation->base || allocation->buffer.size < rounded ||
                allocation->buffer.address > UINTPTR_MAX - allocation->buffer.size)
                return HSA_STATUS_ERROR;
            allocation->size = allocation->buffer.size;
        } else {
            if (rounded > pool.capacity) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
            if (posix_memalign(&allocation->base, alignment, rounded))
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            allocation->size = rounded;
        }
        allocation->owner = pool.owner;
        {
            std::lock_guard lock(runtimeMutex);
            if (!references || !findPool(poolHandle)) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            const auto address = reinterpret_cast<uintptr_t>(allocation->base);
            // Distinct GPUs can expose identical MC addresses. Until per-process
            // GPU VAs are implemented, reject collisions instead of aliasing them.
            if (findAllocation(allocation->base)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            const auto next = allocations.lower_bound(address);
            if (next != allocations.end() && next->first - address < allocation->size)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            allocations.emplace(address, allocation);
        }
        *out = allocation->base;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
bool accessibleAgent(hsa_agent_t agent, const std::shared_ptr<Allocation> &allocation) {
    if (!agent.handle) return true;
    const auto found = findAgent(agent);
    if (!found) return false;
    return allocation && allocation->connection ? found->connection == allocation->connection ||
        (allocation->shared.host && !found->connection) : !found->connection;
}
hsa_status_t copyBytes(void *dst, const void *src, size_t size,
    const std::shared_ptr<Allocation> &destination, const std::shared_ptr<Allocation> &source) {
    // Shared GTT pointers are real host mappings. Use their CPU mapping when
    // either endpoint is host-visible; ownership transitions remain the caller's
    // responsibility for coarse-grained storage.
    const auto srcGPU = source && !source->shared.host ? source->connection : nullptr;
    const auto dstGPU = destination && !destination->shared.host ? destination->connection : nullptr;
    if (!srcGPU && !dstGPU) {
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memmove(dst, src, size);
        std::atomic_thread_fence(std::memory_order_release);
        return HSA_STATUS_SUCCESS;
    }
    const auto srcOffset = source ? reinterpret_cast<uintptr_t>(src) - reinterpret_cast<uintptr_t>(source->base) : 0;
    const auto dstOffset = destination ? reinterpret_cast<uintptr_t>(dst) - reinterpret_cast<uintptr_t>(destination->base) : 0;
    if (srcGPU && !dstGPU) return srcGPU->readBuffer(source->buffer, srcOffset, dst, size);
    if (!srcGPU) return dstGPU->writeBuffer(destination->buffer, dstOffset, src, size);
    if (srcGPU == dstGPU) {
        if (src == dst) return HSA_STATUS_SUCCESS;
        const bool overlap = source->buffer.handle == destination->buffer.handle &&
            srcOffset < dstOffset + size && dstOffset < srcOffset + size;
        if (!overlap) {
            // The bounded driver copy RPC accepts at most 4 MiB. Keep both BOs
            // retained throughout all chunks and propagate the first failure;
            // retrying through another engine cannot prove the first completed.
            constexpr size_t maxCopy = 4 * 1024 * 1024;
            for (size_t offset = 0; offset < size;) {
                const auto bytes = std::min(maxCopy, size - offset);
                const auto status = srcGPU->copyBuffers(source->buffer, srcOffset + offset,
                    destination->buffer, dstOffset + offset, bytes);
                if (status != HSA_STATUS_SUCCESS) return status;
                offset += bytes;
            }
            return HSA_STATUS_SUCCESS;
        }
    }
    std::array<uint8_t, 4096> staging;
    const bool backwards = srcGPU == dstGPU && source->buffer.handle == destination->buffer.handle &&
        dstOffset > srcOffset && dstOffset < srcOffset + size;
    for (size_t completed = 0; completed < size;) {
        const auto bytes = std::min(staging.size(), size - completed);
        const auto offset = backwards ? size - completed - bytes : completed;
        auto status = srcGPU->readBuffer(source->buffer, srcOffset + offset, staging.data(), bytes);
        if (status != HSA_STATUS_SUCCESS) return status;
        status = dstGPU->writeBuffer(destination->buffer, dstOffset + offset, staging.data(), bytes);
        if (status != HSA_STATUS_SUCCESS) return status;
        completed += bytes;
    }
    return HSA_STATUS_SUCCESS;
}
} // namespace
} // namespace mac_hsa::detail

using namespace mac_hsa::detail;
extern "C" {
HSA_API_EXPORT hsa_status_t mac_hsa_memory_get_sync_capabilities(hsa_agent_t handle,
    const void *pointer,uint32_t *flags) {
    std::shared_ptr<Allocation> allocation;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!pointer || !flags) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto *agent=findAgent(handle);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        allocation=findAllocation(pointer);
        if (!allocation || allocation->connection!=agent->connection)
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    }
    mac_hsa::DeviceSnapshot snapshot;
    if (allocation->connection) {
        const auto status=allocation->connection->read(snapshot);
        if (status!=HSA_STATUS_SUCCESS) return status;
    }
    const auto path=!allocation->connection ? mac_hsa::MemoryPath::HostOnly :
        allocation->shared.host ? mac_hsa::MemoryPath::DriverKitShared : mac_hsa::MemoryPath::DeviceVRAM;
    *flags=mac_hsa::synchronizationCapabilities(path,allocation->connection ? &snapshot : nullptr);
    return HSA_STATUS_SUCCESS;
}
hsa_status_t mac_hsa_memory_allocate_shared(hsa_agent_t agent, size_t size, void **out) {
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    std::shared_ptr<mac_hsa::Connection> connection;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto found = findAgent(agent);
        if (!found || !found->connection) return HSA_STATUS_ERROR_INVALID_AGENT;
        connection = found->connection;
    }
    if (!size || size > SIZE_MAX - 16383) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    try {
        auto allocation = std::make_shared<Allocation>();
        allocation->connection = connection; allocation->owner = agent;
        const auto status = connection->allocateSharedBuffer(size, allocation->shared);
        if (status != HSA_STATUS_SUCCESS) return status;
        allocation->buffer = allocation->shared.device;
        allocation->base = allocation->shared.host;
        allocation->size = allocation->buffer.size;
        const auto address = reinterpret_cast<uintptr_t>(allocation->base);
        if (!address || address != allocation->buffer.address || !allocation->buffer.handle ||
            allocation->size < size || allocation->size > UINTPTR_MAX - address)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        {
            std::lock_guard lock(runtimeMutex);
            if (!references || !findAgent(agent)) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            if (findAllocation(allocation->base)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            const auto next = allocations.lower_bound(address);
            if (next != allocations.end() && next->first - address < allocation->size)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            allocations.emplace(address, allocation);
        }
        *out = allocation->base;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_agent_iterate_regions(hsa_agent_t agent,
    hsa_status_t (*callback)(hsa_region_t, void *), void *data) {
    std::vector<hsa_region_t> snapshot;
    try {
        {
            std::lock_guard lock(runtimeMutex);
            if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            if (!findAgent(agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
            if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            for (const auto &pool : pools)
                if (pool.owner.handle == agent.handle) snapshot.push_back({pool.handle});
        }
        for (const auto region : snapshot) {
            const auto status = callback(region, data);
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
HSA_API_EXPORT hsa_status_t hsa_amd_agent_iterate_memory_pools(hsa_agent_t agent,
    hsa_status_t (*callback)(hsa_amd_memory_pool_t, void *), void *data) {
    std::vector<hsa_amd_memory_pool_t> snapshot;
    try {
        {
            std::lock_guard lock(runtimeMutex);
            if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            if (!findAgent(agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
            if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            for (const auto &pool : pools)
                if (pool.owner.handle == agent.handle) snapshot.push_back({pool.handle});
        }
        for (const auto pool : snapshot) {
            const auto status = callback(pool, data);
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_region_get_info(hsa_region_t region, hsa_region_info_t attribute, void *value) {
    Pool storage{};
    const auto status = poolSnapshot(region.handle, storage, HSA_STATUS_ERROR_INVALID_REGION);
    if (status != HSA_STATUS_SUCCESS) return status;
    const auto pool = &storage;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_REGION_INFO_SEGMENT: return writeValue(value, HSA_REGION_SEGMENT_GLOBAL);
    case HSA_REGION_INFO_GLOBAL_FLAGS: return writeValue(value, poolFlags(*pool));
    case HSA_REGION_INFO_SIZE:
    case HSA_REGION_INFO_ALLOC_MAX_SIZE: return poolCapacity(*pool, value);
    case HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED: return writeValue(value, true);
    case HSA_REGION_INFO_RUNTIME_ALLOC_GRANULE:
    case HSA_REGION_INFO_RUNTIME_ALLOC_ALIGNMENT: return writeValue(value, pool->connection ? size_t(16384) : granule);
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_pool_get_info(hsa_amd_memory_pool_t handle,
    hsa_amd_memory_pool_info_t attribute, void *value) {
    Pool storage{};
    const auto status = poolSnapshot(handle.handle, storage, hsa_status_t(HSA_STATUS_ERROR_INVALID_MEMORY_POOL));
    if (status != HSA_STATUS_SUCCESS) return status;
    const auto pool = &storage;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_AMD_MEMORY_POOL_INFO_SEGMENT: return writeValue(value, HSA_AMD_SEGMENT_GLOBAL);
    case HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS:
        return writeValue(value, poolFlags(*pool));
    case HSA_AMD_MEMORY_POOL_INFO_SIZE:
    case HSA_AMD_MEMORY_POOL_INFO_ALLOC_MAX_SIZE: return poolCapacity(*pool, value);
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED: return writeValue(value, true);
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE:
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_REC_GRANULE:
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALIGNMENT: return writeValue(value, pool->connection ? size_t(16384) : granule);
    case HSA_AMD_MEMORY_POOL_INFO_LOCATION: return writeValue(value, pool->connection && !pool->sharedHost ? HSA_AMD_MEMORY_POOL_LOCATION_GPU : HSA_AMD_MEMORY_POOL_LOCATION_CPU);
    case HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL: {
        std::lock_guard lock(runtimeMutex);
        return writeValue(value, std::all_of(agents.begin(), agents.end(),
            [&](const Agent &agent) { return poolAccessible(*pool, agent); }));
    }
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
HSA_API_EXPORT hsa_status_t hsa_amd_agent_memory_pool_get_info(hsa_agent_t agent,
    hsa_amd_memory_pool_t handle, hsa_amd_agent_memory_pool_info_t attribute, void *value) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = findAgent(agent);
    if (!found) return HSA_STATUS_ERROR_INVALID_AGENT;
    const auto pool = findPool(handle.handle);
    if (!pool) return hsa_status_t(HSA_STATUS_ERROR_INVALID_MEMORY_POOL);
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS:
        return writeValue(value, !poolAccessible(*pool, *found) ? HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED :
                                                    HSA_AMD_MEMORY_POOL_ACCESS_ALLOWED_BY_DEFAULT);
    case HSA_AMD_AGENT_MEMORY_POOL_INFO_NUM_LINK_HOPS:
        return writeValue(value, uint32_t(poolAccessible(*pool, *found) && agent.handle != pool->owner.handle));
    case HSA_AMD_AGENT_MEMORY_POOL_INFO_LINK_INFO: {
        if (!poolAccessible(*pool, *found) || agent.handle == pool->owner.handle)
            return HSA_STATUS_SUCCESS; // zero-hop array: do not write caller storage
        hsa_amd_memory_pool_link_info_t link{};
        link.link_type = HSA_AMD_LINK_INFO_TYPE_PCIE;
        // PCIe/Thunderbolt host AtomicOp interoperability is not established. Unknown
        // bandwidth/latency stay zero instead of inventing a direct PCIe rate.
        return writeValue(value, link);
    }
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
hsa_status_t hsa_memory_allocate(hsa_region_t region, size_t size, void **out) {
    return allocate(region.handle, size, 0, out, HSA_STATUS_ERROR_INVALID_REGION);
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_pool_allocate(hsa_amd_memory_pool_t pool,
    size_t size, uint32_t flags, void **out) {
    return allocate(pool.handle, size, flags, out, hsa_status_t(HSA_STATUS_ERROR_INVALID_MEMORY_POOL));
}
hsa_status_t hsa_memory_free(void *pointer) {
    std::shared_ptr<Allocation> retired;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!pointer) return HSA_STATUS_SUCCESS;
        const auto found = allocations.find(reinterpret_cast<uintptr_t>(pointer));
        if (found == allocations.end()) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (found->second->type != HSA_EXT_POINTER_TYPE_HSA) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        retired = std::move(found->second);
        allocations.erase(found);
    }
    return retired.use_count() == 1 ? retired->release() : HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_pool_free(void *pointer) {
    return hsa_memory_free(pointer);
}
HSA_API_EXPORT hsa_status_t hsa_amd_agents_allow_access(uint32_t count, const hsa_agent_t *handles,
    const uint32_t *flags, const void *pointer) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!count || !handles || flags || !pointer) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto allocation = findAllocation(pointer);
    if (!allocation) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    for (uint32_t i = 0; i < count; ++i) {
        const auto agent = findAgent(handles[i]);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!accessibleAgent(handles[i], allocation)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_memory_copy(void *dst, const void *src, size_t size) {
    std::shared_ptr<Allocation> destination, source;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!size) return HSA_STATUS_SUCCESS;
        destination = findAllocation(dst); source = findAllocation(src);
        if (!validRange(dst, size, destination, true) || !validRange(src, size, source))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return copyBytes(dst, src, size, destination, source);
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_fill(void *pointer, uint32_t value, size_t count) {
    std::shared_ptr<Allocation> allocation;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!count) return HSA_STATUS_SUCCESS;
        allocation = findAllocation(pointer);
        if (!allocation) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        if (count > SIZE_MAX / sizeof(value) || reinterpret_cast<uintptr_t>(pointer) % alignof(uint32_t) ||
            !validRange(pointer, count * sizeof(value), allocation, true))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    if (allocation->connection && !allocation->shared.host) {
        std::array<uint32_t, 1024> values; values.fill(value);
        uint64_t offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(allocation->base);
        while (count) {
            const auto words = std::min(count, values.size());
            const auto status = allocation->connection->writeBuffer(allocation->buffer, offset, values.data(), words * 4);
            if (status != HSA_STATUS_SUCCESS) return status;
            count -= words; offset += words * 4;
        }
    } else {
        std::fill_n(static_cast<uint32_t *>(pointer), count, value);
        std::atomic_thread_fence(std::memory_order_release);
    }
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_pointer_info(const void *pointer, hsa_amd_pointer_info_t *info,
    void *(*alloc)(size_t), uint32_t *count, hsa_agent_t **accessible) {
    hsa_amd_pointer_info_t result{};
    bool known = false;
    hsa_agent_t otherAccess{};
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!pointer || !info || info->size < offsetof(hsa_amd_pointer_info_t, agentBaseAddress))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        result.size = std::min<uint32_t>(info->size, sizeof(result));
        result.type = HSA_EXT_POINTER_TYPE_UNKNOWN;
        if (auto allocation = findAllocation(pointer)) {
            known = true;
            result.type = allocation->type;
            result.agentBaseAddress = allocation->base;
            result.hostBaseAddress = allocation->shared.host ? allocation->shared.host :
                (allocation->connection ? nullptr : allocation->base);
            result.sizeInBytes = allocation->size;
            result.agentOwner = allocation->owner;
            result.userData = allocation->userData;
            result.global_flags = allocation->globalFlags ? allocation->globalFlags :
                (allocation->connection ? HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED : HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED);
            result.registered = bool(allocation->connection);
            if (allocation->shared.host)
                for (const auto &agent : agents)
                    if (agent.handle.handle != allocation->owner.handle && accessibleAgent(agent.handle, allocation)) {
                        otherAccess = agent.handle; break;
                    }
        } else known = describeHostLock(pointer, result);
    }
    // The caller's allocator may reenter HSA. Do not call it under runtimeMutex.
    if (accessible) *accessible = nullptr;
    const uint32_t agentCount = known ? 1 + bool(otherAccess.handle) : 0;
    if (count) *count = agentCount;
    if (known && alloc && count && accessible) {
        auto array = static_cast<hsa_agent_t *>(alloc(sizeof(hsa_agent_t) * agentCount));
        if (!array) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        *array = result.agentOwner;
        if (otherAccess.handle) array[1] = otherAccess;
        *accessible = array;
    }
    std::memcpy(info, &result, result.size);
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_pointer_info_set_userdata(const void *pointer, void *data) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto allocation = findAllocation(pointer);
    if (!allocation) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    allocation->userData = data;
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_async_copy(void *dst, hsa_agent_t dstAgent,
    const void *src, hsa_agent_t srcAgent, size_t size, uint32_t count,
    const hsa_signal_t *dependencies, hsa_signal_t completion) {
    try { reapCopyJobs(); }
    catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!size) return HSA_STATUS_SUCCESS;
    if (!dst || !src || (count && !dependencies) || !completion.handle)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto completed = signals.find(completion.handle);
    if (completed == signals.end()) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    const auto destination = findAllocation(dst), source = findAllocation(src);
    if (!accessibleAgent(dstAgent, destination) || !accessibleAgent(srcAgent, source))
        return HSA_STATUS_ERROR_INVALID_AGENT;
    if (!validRange(dst, size, destination, true) || !validRange(src, size, source))
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    try {
        std::vector<std::shared_ptr<mac_hsa::Signal>> waiting;
        waiting.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto found = signals.find(dependencies[i].handle);
            if (found == signals.end()) return HSA_STATUS_ERROR_INVALID_SIGNAL;
            waiting.push_back(found->second);
        }
        if (copyJobs.size() >= 64) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        copyJobs.reserve(copyJobs.size() + 1);
        auto job = std::make_unique<CopyJob>();
        auto jobPointer = job.get();
        job->worker = std::jthread([=, waiting = std::move(waiting), signal = completed->second]
            (std::stop_token stop) {
            bool failed = false;
            for (const auto &dependency : waiting) {
                while (dependency->value().load(std::memory_order_acquire) != 0) {
                    if (stop.stop_requested() || !dependency->alive.load() || !signal->alive.load()) {
                        failed = true; break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                if (failed) break;
            }
            failed |= stop.stop_requested() || !signal->alive.load();
            // Capture both allocation owners through the actual copy, including
            // when the application has already removed their public handles.
            (void)destination; (void)source;
            if (!failed) failed = copyBytes(dst, src, size, destination, source) != HSA_STATUS_SUCCESS;
            if (failed) signal->value().store(-1, std::memory_order_release);
            else signal->value().fetch_sub(1, std::memory_order_release);
            signal->changed.notify_all();
            jobPointer->done.store(true, std::memory_order_release);
        });
        copyJobs.push_back(std::move(job));
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
      catch (const std::system_error &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
} // extern C
