#include "runtime_state.h"
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <array>
#include <cstdio>

namespace mac_hsa::detail {
namespace {
struct Cache { uint64_t id; hsa_agent_t agent; uint8_t level; uint32_t size; std::string name; };
std::vector<Cache> caches;
bool cacheDiscovery = false;
struct LockRange { uintptr_t begin, end; size_t size; };
std::map<uintptr_t, uint64_t> lockedPages;
std::map<uintptr_t, std::vector<LockRange>> lockedRanges;

hsa_status_t lockHost(void *pointer, size_t size, hsa_agent_t *requested, int count,
    const hsa_amd_memory_pool_t *poolHandle, uint32_t flags, void **out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer), page = hostPageSize();
    if (!pointer || !size || count < 0 || (bool(count) != bool(requested)) || flags ||
        size > UINTPTR_MAX - address || address + size > UINTPTR_MAX - (page - 1))
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (poolHandle) {
        const auto pool = findPool(poolHandle->handle);
        if (!pool || pool->connection) return hsa_status_t(HSA_STATUS_ERROR_INVALID_MEMORY_POOL);
    }
    // Ordinary host addresses are not GPU addresses on this transport.
    for (int i = 0; i < count; ++i) {
        const auto agent = findAgent(requested[i]);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (agent->connection) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    if (!count) for (const auto &agent : agents)
        if (agent.connection) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    const auto allocation = findAllocation(pointer);
    if (allocation && (allocation->connection || size > allocation->size - (address - uintptr_t(allocation->base))))
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto next = allocations.lower_bound(address);
    if (!allocation && next != allocations.end() && size > next->first - address) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const uintptr_t begin = address & ~(page - 1), end = (address + size + page - 1) & ~(page - 1);
    if (end - begin > 256ull << 20) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        auto pages = lockedPages;
        auto ranges = lockedRanges;
        ranges[address].push_back({begin, end, size});
        for (auto p = begin; p < end; p += page) {
            auto &refs = pages[p];
            if (refs == UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            ++refs;
        }
        for (auto p = begin; p < end; p += page) {
            if (lockedPages.contains(p)) continue;
            if (mlock(reinterpret_cast<void *>(p), page)) {
                for (auto undo = begin; undo < p; undo += page)
                    if (!lockedPages.contains(undo)) munlock(reinterpret_cast<void *>(undo), page);
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            }
        }
        lockedPages.swap(pages); lockedRanges.swap(ranges);
        *out = pointer; return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
}
void clearCaches() { caches.clear(); cacheDiscovery = false; }
void clearHostLocks() {
    for (const auto &[page, refs] : lockedPages) { (void)refs; munlock(reinterpret_cast<void *>(page), hostPageSize()); }
    lockedPages.clear(); lockedRanges.clear();
}
bool describeHostLock(const void *pointer, hsa_amd_pointer_info_t &info) {
    const auto address = uintptr_t(pointer);
    for (const auto &[base, ranges] : lockedRanges) {
        if (base > address) break;
        for (const auto &range : ranges) if (address - base < range.size) {
            info.type = HSA_EXT_POINTER_TYPE_LOCKED;
            info.hostBaseAddress = info.agentBaseAddress = reinterpret_cast<void *>(base);
            info.sizeInBytes = range.size; info.registered = true;
            info.global_flags = HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED;
            for (const auto &agent : agents) if (!agent.connection) { info.agentOwner = agent.handle; break; }
            return true;
        }
    }
    return false;
}
}
using namespace mac_hsa::detail;
extern "C" {
hsa_status_t hsa_agent_iterate_caches(hsa_agent_t handle,
    hsa_status_t (*callback)(hsa_cache_t, void *), void *data) {
    std::vector<hsa_cache_t> snapshot;
    try {
        {
            std::lock_guard lock(runtimeMutex);
            if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            const auto agent = findAgent(handle);
            if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
            if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            // GPU cache topology must come from discovery; do not invent sizes.
            if (agent->connection) return HSA_STATUS_SUCCESS;
            if (!cacheDiscovery) {
                std::vector<Cache> discovered;
                for (uint8_t level = 1; level <= 3; ++level) {
                    char key[32]; std::snprintf(key, sizeof(key), level == 1 ? "hw.l%udcachesize" : "hw.l%ucachesize", level);
                    uint64_t bytes = 0; size_t length = sizeof(bytes);
                    if (sysctlbyname(key, &bytes, &length, nullptr, 0) || !bytes || bytes > UINT32_MAX) continue;
                    if (lastHandle == UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
                    discovered.push_back({++lastHandle, handle, level, uint32_t(bytes), "Mac CPU L" + std::to_string(level) + " data cache"});
                }
                caches.swap(discovered); cacheDiscovery = true;
            }
            for (const auto &cache : caches) if (cache.agent.handle == handle.handle) snapshot.push_back({cache.id});
        }
        for (const auto cache : snapshot) {
            const auto status = callback(cache, data);
            if (status != HSA_STATUS_SUCCESS) return status;
        }
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_cache_get_info(hsa_cache_t handle, hsa_cache_info_t attribute, void *value) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    for (const auto &cache : caches) if (cache.id == handle.handle) {
        switch (attribute) {
        case HSA_CACHE_INFO_NAME_LENGTH: return writeValue(value, uint32_t(cache.name.size()));
        case HSA_CACHE_INFO_NAME: std::memcpy(value, cache.name.c_str(), cache.name.size() + 1); return HSA_STATUS_SUCCESS;
        case HSA_CACHE_INFO_LEVEL: return writeValue(value, cache.level);
        case HSA_CACHE_INFO_SIZE: return writeValue(value, cache.size);
        default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        }
    }
    return HSA_STATUS_ERROR_INVALID_CACHE;
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_lock(void *pointer, size_t size,
    hsa_agent_t *agents, int count, void **out) {
    return lockHost(pointer, size, agents, count, nullptr, 0, out);
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_lock_to_pool(void *pointer, size_t size,
    hsa_agent_t *agents, int count, hsa_amd_memory_pool_t pool, uint32_t flags, void **out) {
    return lockHost(pointer, size, agents, count, &pool, flags, out);
}
HSA_API_EXPORT hsa_status_t hsa_amd_memory_unlock(void *pointer) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = lockedRanges.find(reinterpret_cast<uintptr_t>(pointer));
    if (found == lockedRanges.end()) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto range = found->second.back();
    for (auto p = range.begin; p < range.end; p += hostPageSize()) {
        auto page = lockedPages.find(p);
        if (--page->second == 0) { munlock(reinterpret_cast<void *>(p), hostPageSize()); lockedPages.erase(page); }
    }
    found->second.pop_back();
    if (found->second.empty()) lockedRanges.erase(found);
    return HSA_STATUS_SUCCESS;
}
}
