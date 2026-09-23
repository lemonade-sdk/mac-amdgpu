#include "runtime_state.h"
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
        const auto status = pool.connection->memoryCapacity(capacity);
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
        if (pool.connection) {
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
    return allocation && allocation->connection ? agent.handle == allocation->owner.handle : !found->connection;
}
hsa_status_t copyBytes(void *dst, const void *src, size_t size,
    const std::shared_ptr<Allocation> &destination, const std::shared_ptr<Allocation> &source) {
    const auto srcGPU = source ? source->connection : nullptr;
    const auto dstGPU = destination ? destination->connection : nullptr;
    if (!srcGPU && !dstGPU) { std::memmove(dst, src, size); return HSA_STATUS_SUCCESS; }
    const auto srcOffset = source ? reinterpret_cast<uintptr_t>(src) - reinterpret_cast<uintptr_t>(source->base) : 0;
    const auto dstOffset = destination ? reinterpret_cast<uintptr_t>(dst) - reinterpret_cast<uintptr_t>(destination->base) : 0;
    if (srcGPU && !dstGPU) return srcGPU->readBuffer(source->buffer, srcOffset, dst, size);
    if (!srcGPU) return dstGPU->writeBuffer(destination->buffer, dstOffset, src, size);
    std::array<uint8_t, 4096> staging;
    for (size_t offset = 0; offset < size;) {
        const auto bytes = std::min(staging.size(), size - offset);
        auto status = srcGPU->readBuffer(source->buffer, srcOffset + offset, staging.data(), bytes);
        if (status != HSA_STATUS_SUCCESS) return status;
        status = dstGPU->writeBuffer(destination->buffer, dstOffset + offset, staging.data(), bytes);
        if (status != HSA_STATUS_SUCCESS) return status;
        offset += bytes;
    }
    return HSA_STATUS_SUCCESS;
}
} // namespace
} // namespace mac_hsa::detail

using namespace mac_hsa::detail;
extern "C" {
hsa_status_t hsa_agent_iterate_regions(hsa_agent_t agent,
    hsa_status_t (*callback)(hsa_region_t, void *), void *data) {
    hsa_region_t region{};
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!findAgent(agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        for (const auto &pool : pools)
            if (pool.owner.handle == agent.handle) region.handle = pool.handle;
    }
    return region.handle ? callback(region, data) : HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_agent_iterate_memory_pools(hsa_agent_t agent,
    hsa_status_t (*callback)(hsa_amd_memory_pool_t, void *), void *data) {
    hsa_amd_memory_pool_t result{};
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!findAgent(agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        for (const auto &pool : pools)
            if (pool.owner.handle == agent.handle) result.handle = pool.handle;
    }
    return result.handle ? callback(result, data) : HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_region_get_info(hsa_region_t region, hsa_region_info_t attribute, void *value) {
    Pool storage{};
    const auto status = poolSnapshot(region.handle, storage, HSA_STATUS_ERROR_INVALID_REGION);
    if (status != HSA_STATUS_SUCCESS) return status;
    const auto pool = &storage;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_REGION_INFO_SEGMENT: return writeValue(value, HSA_REGION_SEGMENT_GLOBAL);
    case HSA_REGION_INFO_GLOBAL_FLAGS: return writeValue(value, uint32_t(pool->connection ? HSA_REGION_GLOBAL_FLAG_COARSE_GRAINED : HSA_REGION_GLOBAL_FLAG_FINE_GRAINED));
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
        return writeValue(value, uint32_t(pool->connection ? HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED : HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED));
    case HSA_AMD_MEMORY_POOL_INFO_SIZE:
    case HSA_AMD_MEMORY_POOL_INFO_ALLOC_MAX_SIZE: return poolCapacity(*pool, value);
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED: return writeValue(value, true);
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE:
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_REC_GRANULE:
    case HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALIGNMENT: return writeValue(value, pool->connection ? size_t(16384) : granule);
    case HSA_AMD_MEMORY_POOL_INFO_LOCATION: return writeValue(value, pool->connection ? HSA_AMD_MEMORY_POOL_LOCATION_GPU : HSA_AMD_MEMORY_POOL_LOCATION_CPU);
    case HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL: {
        std::lock_guard lock(runtimeMutex);
        return writeValue(value, agents.size() == 1);
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
        return writeValue(value, agent.handle != pool->owner.handle ? HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED :
                                                    HSA_AMD_MEMORY_POOL_ACCESS_ALLOWED_BY_DEFAULT);
    case HSA_AMD_AGENT_MEMORY_POOL_INFO_NUM_LINK_HOPS:
        if (agent.handle != pool->owner.handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        return writeValue(value, uint32_t(0));
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
        if (handles[i].handle != allocation->owner.handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
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
    if (allocation->connection) {
        std::array<uint32_t, 1024> values; values.fill(value);
        uint64_t offset = reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(allocation->base);
        while (count) {
            const auto words = std::min(count, values.size());
            const auto status = allocation->connection->writeBuffer(allocation->buffer, offset, values.data(), words * 4);
            if (status != HSA_STATUS_SUCCESS) return status;
            count -= words; offset += words * 4;
        }
    } else std::fill_n(static_cast<uint32_t *>(pointer), count, value);
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_pointer_info(const void *pointer, hsa_amd_pointer_info_t *info,
    void *(*alloc)(size_t), uint32_t *count, hsa_agent_t **accessible) {
    hsa_amd_pointer_info_t result{};
    bool known = false;
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
            result.hostBaseAddress = allocation->connection ? nullptr : allocation->base;
            result.sizeInBytes = allocation->size;
            result.agentOwner = allocation->owner;
            result.userData = allocation->userData;
            result.global_flags = allocation->connection ? HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED : HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED;
            result.registered = bool(allocation->connection);
        } else known = describeHostLock(pointer, result);
    }
    // The caller's allocator may reenter HSA. Do not call it under runtimeMutex.
    if (accessible) *accessible = nullptr;
    if (count) *count = known ? 1 : 0;
    if (known && alloc && count && accessible) {
        auto array = static_cast<hsa_agent_t *>(alloc(sizeof(hsa_agent_t)));
        if (!array) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        *array = result.agentOwner;
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
