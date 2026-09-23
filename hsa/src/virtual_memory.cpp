#include "runtime_state.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <limits>

namespace mac_hsa::detail {
size_t hostPageSize() { static const size_t size = size_t(getpagesize()); return size; }
namespace {
struct Reservation {
    void *base = nullptr;
    size_t size = 0;
    ~Reservation() { if (base) munmap(base, size); }
};
struct Backing {
    int fd = -1;
    size_t size = 0;
    hsa_agent_t owner{};
    void *pinned = nullptr;
    ~Backing() {
        if (pinned) { munlock(pinned, size); munmap(pinned, size); }
        if (fd >= 0) close(fd);
    }
};
struct Mapping {
    std::shared_ptr<Reservation> reservation;
    std::shared_ptr<Backing> storage;
    void *base = nullptr;
    size_t size = 0;
    ~Mapping() {
        // Keep the address reserved after removing physical backing. No other
        // thread can claim a hole between unmap and the next mapping.
        if (base) mmap(base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    }
};
std::map<uintptr_t, std::shared_ptr<Reservation>> reservations;
std::unordered_map<uint64_t, std::shared_ptr<Backing>> handles;
std::map<uintptr_t, std::shared_ptr<Allocation>> mappings;
bool aligned(size_t size) { return size && size % hostPageSize() == 0; }
std::shared_ptr<Reservation> reservationFor(uintptr_t address, size_t size) {
    auto it = reservations.upper_bound(address);
    if (it == reservations.begin()) return {};
    --it;
    const auto offset = address - it->first;
    return offset <= it->second->size && size <= it->second->size - offset ? it->second : nullptr;
}
bool overlaps(uintptr_t address, size_t size) {
    const auto next = allocations.lower_bound(address);
    if (next != allocations.end() && next->first - address < size) return true;
    return bool(findAllocation(reinterpret_cast<void *>(address)));
}
int sharedFile(size_t size) {
    char name[32];
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        std::snprintf(name, sizeof(name), "/mhv%08x%08x", arc4random(), arc4random());
        const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) continue;
        shm_unlink(name); // only descriptor/mapping references retain this storage
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) || ftruncate(fd, off_t(size))) { close(fd); return -1; }
        return fd;
    }
    return -1;
}
}
void clearVirtualMemory() { mappings.clear(); handles.clear(); reservations.clear(); }
}
using namespace mac_hsa::detail;
extern "C" {
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_address_reserve_align(void **out, size_t size,
    uint64_t hint, uint64_t alignment, uint64_t flags) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!alignment) alignment = hostPageSize();
    if (!aligned(size) || alignment < hostPageSize() || (alignment & (alignment - 1)) ||
        alignment > SIZE_MAX || size > SIZE_MAX - alignment || hint % hostPageSize() ||
        flags & ~uint64_t(HSA_AMD_VMEM_ADDRESS_NO_REGISTER)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    try {
        auto reservation = std::make_shared<Reservation>();
        const size_t bytes = size + size_t(alignment) - hostPageSize();
        void *raw = mmap(reinterpret_cast<void *>(hint), bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (raw == MAP_FAILED) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(raw);
        const uintptr_t address = (begin + alignment - 1) & ~(alignment - 1);
        const size_t prefix = address - begin, suffix = bytes - prefix - size;
        if (prefix) munmap(raw, prefix);
        if (suffix) munmap(reinterpret_cast<void *>(address + size), suffix);
        reservation->base = reinterpret_cast<void *>(address); reservation->size = size;
        // GPU MC addresses are opaque process pointers too; never overlap them.
        if (overlaps(address, size)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        reservations.emplace(address, reservation);
        *out = reservation->base;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_address_free(void *base, size_t size) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = reservations.find(reinterpret_cast<uintptr_t>(base));
    if (found == reservations.end()) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (size != found->second->size) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (found->second.use_count() != 1) return HSA_STATUS_ERROR_RESOURCE_FREE;
    reservations.erase(found); return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_handle_create(hsa_amd_memory_pool_t poolHandle,
    size_t size, hsa_amd_memory_type_t type, uint64_t flags, hsa_amd_vmem_alloc_handle_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    const auto pool = findPool(poolHandle.handle);
    if (!pool) return hsa_status_t(HSA_STATUS_ERROR_INVALID_MEMORY_POOL);
    if (!aligned(size) || flags || size > uint64_t(INT64_MAX) ||
        (type != MEMORY_TYPE_NONE && type != MEMORY_TYPE_PINNED)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    // GPU aliases require process GPU page tables, not a host mmap of MC addresses.
    if (pool->connection) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (size > pool->capacity || lastHandle == UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        auto backing = std::make_shared<Backing>();
        backing->fd = sharedFile(size);
        if (backing->fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        backing->size = size; backing->owner = pool->owner;
        if (type == MEMORY_TYPE_PINNED) {
            void *pointer = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, backing->fd, 0);
            if (pointer == MAP_FAILED) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            if (mlock(pointer, size)) { munmap(pointer, size); return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
            backing->pinned = pointer;
        }
        const auto id = ++lastHandle;
        handles.emplace(id, std::move(backing)); out->handle = id;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_handle_release(hsa_amd_vmem_alloc_handle_t handle) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    return handles.erase(handle.handle) ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR_INVALID_ALLOCATION;
}
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_map(void *base, size_t size, size_t offset,
    hsa_amd_vmem_alloc_handle_t handle, uint64_t flags) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto address = reinterpret_cast<uintptr_t>(base);
    const auto found = handles.find(handle.handle);
    if (found == handles.end() || !aligned(size) || address % hostPageSize() || offset || flags ||
        size != found->second->size) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto reservation = reservationFor(address, size);
    if (!reservation || overlaps(address, size)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    try {
        auto mapping = std::make_shared<Mapping>();
        mapping->reservation = reservation; mapping->storage = found->second; mapping->size = size;
        auto allocation = std::make_shared<Allocation>();
        allocation->base = base; allocation->size = size; allocation->owner = found->second->owner;
        allocation->type = HSA_EXT_POINTER_TYPE_HSA_VMEM; allocation->backing = mapping;
        allocation->access = HSA_ACCESS_PERMISSION_NONE;
        // Allocate bookkeeping before replacing only our own reserved pages.
        mappings.emplace(address, allocation);
        try { allocations.emplace(address, allocation); }
        catch (...) { mappings.erase(address); throw; }
        if (mmap(base, size, PROT_NONE, MAP_SHARED | MAP_FIXED, found->second->fd, 0) == MAP_FAILED) {
            allocations.erase(address); mappings.erase(address); return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        }
        mapping->base = base;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_unmap(void *base, size_t size) {
    try { reapCopyJobs(); }
    catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto address = reinterpret_cast<uintptr_t>(base);
    const auto found = mappings.find(address);
    if (found == mappings.end()) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (size != found->second->size) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (found->second.use_count() != 2) return HSA_STATUS_ERROR_RESOURCE_FREE; // async copy pins
    // Replace first so a failed OS operation leaves the mapping tracked.
    if (mmap(base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
        return HSA_STATUS_ERROR;
    std::static_pointer_cast<Mapping>(found->second->backing)->base = nullptr;
    allocations.erase(address); mappings.erase(found); return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_vmem_set_access(void *base, size_t size,
    const hsa_amd_memory_access_desc_t *descriptors, size_t count) {
    try { reapCopyJobs(); }
    catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = mappings.find(reinterpret_cast<uintptr_t>(base));
    if (found == mappings.end()) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (size != found->second->size || !count || !descriptors) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    int protection = -1;
    hsa_access_permission_t selected = found->second->access;
    for (size_t i = 0; i < count; ++i) {
        const auto agent = findAgent(descriptors[i].agent_handle);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        const auto permission = descriptors[i].permissions;
        if (permission < HSA_ACCESS_PERMISSION_NONE || permission > HSA_ACCESS_PERMISSION_RW)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (agent->connection && permission != HSA_ACCESS_PERMISSION_NONE) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!agent->connection) {
            if (protection != -1) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            protection = ((permission & HSA_ACCESS_PERMISSION_RO) ? PROT_READ : 0) |
                         ((permission & HSA_ACCESS_PERMISSION_WO) ? PROT_WRITE : 0);
            selected = permission;
        }
    }
    if (found->second.use_count() != 2) return HSA_STATUS_ERROR_RESOURCE_FREE;
    if (protection != -1 && mprotect(base, size, protection)) return HSA_STATUS_ERROR;
    found->second->access = selected;
    return HSA_STATUS_SUCCESS;
}
}
