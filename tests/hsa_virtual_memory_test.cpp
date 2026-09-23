#include "runtime_state.h"
#include <cassert>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace mac_hsa {
struct TestConnection final : Connection {
    bool supportsBuffers() const override { return true; }
    hsa_status_t read(DeviceSnapshot &s) override { s = {1, 180, 15, 1, 1, 12, 0, 1}; return HSA_STATUS_SUCCESS; }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    connections.push_back(std::make_shared<TestConnection>()); return HSA_STATUS_SUCCESS;
}
}
static hsa_agent_t cpu, gpu;
static hsa_amd_memory_pool_t pool;
int main() {
    assert(hsa_init() == 0);
    assert(hsa_iterate_agents([](hsa_agent_t a, void *) {
        hsa_device_type_t type;
        assert(hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &type) == 0);
        (type == HSA_DEVICE_TYPE_CPU ? cpu : gpu) = a; return HSA_STATUS_SUCCESS;
    }, nullptr) == 0);
    assert(hsa_amd_agent_iterate_memory_pools(cpu, [](hsa_amd_memory_pool_t p, void *) {
        pool = p; return HSA_STATUS_SUCCESS;
    }, nullptr) == 0);
    size_t page;
    assert(hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE, &page) == 0);
    assert(page == size_t(getpagesize()));
    void *base = nullptr;
    assert(hsa_amd_vmem_address_reserve_align(&base, page * 3, 0, page * 8, 0) == 0);
    assert(uintptr_t(base) % (page * 8) == 0);
    hsa_amd_vmem_alloc_handle_t handle{};
    assert(hsa_amd_vmem_handle_create(pool, page, MEMORY_TYPE_NONE, 0, &handle) == 0);
    const hsa_amd_memory_access_desc_t cpuRW{HSA_ACCESS_PERMISSION_RW, cpu};
    const hsa_amd_memory_access_desc_t gpuRW{HSA_ACCESS_PERMISSION_RW, gpu};
    for (size_t i = 0; i < 3; ++i) {
        auto pointer = static_cast<char *>(base) + i * page;
        assert(hsa_amd_vmem_map(pointer, page, 0, handle, 0) == 0);
        assert(hsa_amd_memory_fill(pointer, 1, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
        assert(hsa_amd_vmem_set_access(pointer, page, &gpuRW, 1) == HSA_STATUS_ERROR_INVALID_AGENT);
        assert(hsa_amd_vmem_set_access(pointer, page, &cpuRW, 1) == 0);
    }
    assert(hsa_amd_vmem_map(base, page, 0, handle, 0) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_vmem_map(static_cast<char *>(base) + 3 * page, page, 0, handle, 0) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_vmem_address_free(base, page * 3) == HSA_STATUS_ERROR_RESOURCE_FREE);
    assert(hsa_amd_vmem_handle_release(handle) == 0);
    assert(hsa_amd_vmem_handle_release(handle) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    // HRX's triple-mapped ring shape: writing across a logical wrap aliases
    // the same physical pages, even after releasing the allocation handle.
    auto bytes = static_cast<unsigned char *>(base);
    for (size_t i = 0; i < page; ++i) bytes[page + i] = uint8_t(i * 31 + 9);
    for (size_t i = 0; i < page; ++i) assert(bytes[i] == bytes[page + i] && bytes[i] == bytes[2 * page + i]);
    hsa_amd_pointer_info_t info{}; info.size = sizeof(info);
    assert(hsa_amd_pointer_info(base, &info, nullptr, nullptr, nullptr) == 0);
    assert(info.type == HSA_EXT_POINTER_TYPE_HSA_VMEM && info.sizeInBytes == page);
    assert(hsa_memory_free(base) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    const hsa_amd_memory_access_desc_t cpuRO{HSA_ACCESS_PERMISSION_RO, cpu};
    assert(hsa_amd_vmem_set_access(base, page, &cpuRO, 1) == 0);
    assert(hsa_amd_memory_fill(base, 0, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    uint32_t sample;
    assert(hsa_memory_copy(&sample, base, sizeof(sample)) == 0);
    assert(hsa_memory_copy(base, &sample, sizeof(sample)) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    hsa_signal_t dep{}, done{};
    assert(hsa_signal_create(1, 1, &cpu, &dep) == 0 && hsa_signal_create(1, 1, &cpu, &done) == 0);
    assert(hsa_amd_memory_async_copy(&sample, cpu, base, cpu, 4, 1, &dep, done) == 0);
    assert(hsa_amd_vmem_unmap(base, page) == HSA_STATUS_ERROR_RESOURCE_FREE);
    hsa_signal_store_screlease(dep, 0);
    assert(hsa_signal_wait_scacquire(done, HSA_SIGNAL_CONDITION_EQ, 0, 1000000000, HSA_WAIT_STATE_BLOCKED) == 0);
    hsa_status_t status;
    for (unsigned retry = 0; (status = hsa_amd_vmem_unmap(base, page)) == HSA_STATUS_ERROR_RESOURCE_FREE && retry < 100; ++retry)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(status == 0);
    for (size_t i = 1; i < 3; ++i) assert(hsa_amd_vmem_unmap(bytes + page * i, page) == 0);
    assert(hsa_amd_vmem_address_free(base, page) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_vmem_address_free(base, page * 3) == 0);
    assert(hsa_amd_vmem_address_free(base, page * 3) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    // Overlapping locks and duplicate input pointers require balanced unlocks.
    void *host = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(host != MAP_FAILED);
    void *locked = nullptr;
    assert(hsa_amd_memory_lock(host, page, &gpu, 1, &locked) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && !locked);
    assert(hsa_amd_memory_lock(host, page, &cpu, 1, &locked) == 0 && locked == host);
    assert(hsa_amd_memory_lock_to_pool(host, page * 2, &cpu, 1, pool, 0, &locked) == 0);
    assert(hsa_amd_memory_lock(static_cast<char *>(host) + 3, page, &cpu, 1, &locked) == 0);
    info.size = sizeof(info);
    assert(hsa_amd_pointer_info(static_cast<char *>(host) + 3, &info, nullptr, nullptr, nullptr) == 0);
    assert(info.type == HSA_EXT_POINTER_TYPE_LOCKED && info.registered);
    assert(hsa_amd_memory_unlock(host) == 0 && hsa_amd_memory_unlock(host) == 0);
    assert(hsa_amd_memory_unlock(host) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_memory_unlock(static_cast<char *>(host) + 3) == 0);
    munmap(host, page * 2);
    unsigned count = 0;
    assert(hsa_agent_iterate_caches(cpu, [](hsa_cache_t c, void *p) {
        ++*static_cast<unsigned *>(p); uint8_t level = 0; uint32_t size = 0, length = 0;
        assert(hsa_cache_get_info(c, HSA_CACHE_INFO_LEVEL, &level) == 0 && level >= 1 && level <= 3);
        assert(hsa_cache_get_info(c, HSA_CACHE_INFO_SIZE, &size) == 0 && size);
        assert(hsa_cache_get_info(c, HSA_CACHE_INFO_NAME_LENGTH, &length) == 0 && length);
        std::vector<char> name(length + 2, '!');
        assert(hsa_cache_get_info(c, HSA_CACHE_INFO_NAME, name.data()) == 0 && name[length] == 0 && name[length + 1] == '!');
        return HSA_STATUS_SUCCESS;
    }, &count) == 0);
    assert(hsa_cache_get_info({UINT64_MAX}, HSA_CACHE_INFO_LEVEL, &count) == HSA_STATUS_ERROR_INVALID_CACHE);
    // Final shutdown retires unreleased mappings and handles safely.
    assert(hsa_amd_vmem_address_reserve_align(&base, page, 0, 0, 0) == 0);
    assert(hsa_amd_vmem_handle_create(pool, page, MEMORY_TYPE_NONE, 0, &handle) == 0);
    assert(hsa_amd_vmem_map(base, page, 0, handle, 0) == 0);
    assert(hsa_shut_down() == 0);
    assert(hsa_init() == 0);
    assert(hsa_amd_vmem_handle_release(handle) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    assert(hsa_shut_down() == 0);
    std::printf("HSA host services: triple aliases, protections, lifetime, overlapping locks and %u CPU caches pass\n", count);
}
