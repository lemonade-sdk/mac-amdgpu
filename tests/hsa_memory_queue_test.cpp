#include "runtime_state.h"
#include <hsa/amd_hsa_queue.h>
#include <cassert>
#include <cstdio>
#include <array>

static bool gpuBuffers = false, failCopy = false, sharedBuffers = false, secondGPU = false;
static unsigned sharedAllocations = 0, sharedFrees = 0;
static unsigned gpuFrees = 0, nativeCopies = 0;
static std::vector<size_t> nativeCopySizes;
namespace mac_hsa {
struct TestConnection final : Connection {
    uint64_t next = 0;
    std::map<uint64_t, std::vector<uint8_t>> buffers;
    bool supportsBuffers() const override { return gpuBuffers; }
    bool supportsSharedBuffers() const override { return sharedBuffers; }
    hsa_status_t sharedMemoryCapacity(uint64_t &size) override { size = 65536; return HSA_STATUS_SUCCESS; }
    hsa_status_t allocateSharedBuffer(uint64_t bytes, SharedBuffer &buffer) override {
        void *pointer = nullptr;
        assert(!posix_memalign(&pointer, 16384, bytes));
        std::memset(pointer, 0, bytes);
        buffer = {{++next, uint64_t(pointer), bytes}, pointer, 1};
        ++sharedAllocations;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override {
        uint64_t now;
        assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now) == HSA_STATUS_SUCCESS ||
               !mac_hsa::detail::references);
        std::free(buffer.host); ++sharedFrees;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t memoryCapacity(uint64_t &size) override { size = 16 << 20; return HSA_STATUS_SUCCESS; }
    hsa_status_t allocateBuffer(uint64_t size, DeviceBuffer &buffer) override {
        const auto handle = ++next;
        buffer = {handle, 0x8001000000ull + handle * 0x1000000, size};
        buffers.emplace(handle, std::vector<uint8_t>(size, 0x91));
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t copyBuffers(const DeviceBuffer &src, uint64_t so,
        const DeviceBuffer &dst, uint64_t dso, size_t size) override {
        ++nativeCopies;nativeCopySizes.push_back(size);
        assert(size && size <= 4 * 1024 * 1024);
        assert(so <= src.size && size <= src.size-so && dso <= dst.size && size <= dst.size-dso);
        assert(src.handle != dst.handle || so+size <= dso || dso+size <= so);
        if (failCopy) return HSA_STATUS_ERROR;
        std::memcpy(buffers.at(dst.handle).data()+dso,buffers.at(src.handle).data()+so,size);
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        uint64_t now;
        // Closing hardware storage must never happen under runtimeMutex.
        hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now);
        ++gpuFrees;
        assert(buffers.erase(buffer.handle) == 1);
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t readBuffer(const DeviceBuffer &buffer, uint64_t offset, void *out, size_t size) override {
        if (failCopy) return HSA_STATUS_ERROR;
        assert(offset + size <= buffers.at(buffer.handle).size());
        std::memcpy(out, buffers.at(buffer.handle).data() + offset, size); return HSA_STATUS_SUCCESS;
    }
    hsa_status_t writeBuffer(const DeviceBuffer &buffer, uint64_t offset, const void *in, size_t size) override {
        if (failCopy) return HSA_STATUS_ERROR;
        assert(offset + size <= buffers.at(buffer.handle).size());
        std::memcpy(buffers.at(buffer.handle).data() + offset, in, size); return HSA_STATUS_SUCCESS;
    }
    hsa_status_t read(DeviceSnapshot &snapshot) override {
        snapshot = {1, 179, 15, 256ull << 20, 32ull << 30, 12, 0, 1};
        return HSA_STATUS_SUCCESS;
    }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    connections.push_back(std::make_shared<TestConnection>());
    if (secondGPU) connections.push_back(std::make_shared<TestConnection>());
    return HSA_STATUS_SUCCESS;
}
}
static std::vector<hsa_agent_t> agents;
static hsa_amd_memory_pool_t pool;
static hsa_region_t region;
static hsa_signal_t signal(int64_t value) {
    hsa_signal_t result;
    assert(hsa_signal_create(value, 1, &agents[0], &result) == HSA_STATUS_SUCCESS);
    return result;
}
static void *allocate(size_t size) {
    void *result = nullptr;
    assert(hsa_amd_memory_pool_allocate(pool, size, 0, &result) == HSA_STATUS_SUCCESS);
    return result;
}
static void wait(hsa_signal_t s, int64_t value = 0) {
    assert(hsa_signal_wait_scacquire(s, HSA_SIGNAL_CONDITION_EQ, value, 2000000000,
                                   HSA_WAIT_STATE_BLOCKED) == value);
}
int main() {
    assert(hsa_init() == HSA_STATUS_SUCCESS);
    assert(hsa_iterate_agents([](hsa_agent_t a, void *) { agents.push_back(a); return HSA_STATUS_SUCCESS; }, nullptr) == 0);
    assert(agents.size() == 2);
    assert(hsa_amd_agent_iterate_memory_pools(agents[0], [](hsa_amd_memory_pool_t p, void *) {
        pool = p;
        size_t capacity;
        assert(hsa_amd_memory_pool_get_info(p, HSA_AMD_MEMORY_POOL_INFO_SIZE, &capacity) == 0 && capacity);
        return HSA_STATUS_INFO_BREAK;
    }, nullptr) == HSA_STATUS_INFO_BREAK);
    assert(hsa_agent_iterate_regions(agents[0], [](hsa_region_t r, void *) {
        region = r; bool allowed = false;
        assert(hsa_region_get_info(r, HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED, &allowed) == 0 && allowed);
        return HSA_STATUS_SUCCESS;
    }, nullptr) == 0);
    bool all = true;
    assert(hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL, &all) == 0 && !all);
    hsa_amd_memory_pool_access_t access;
    assert(hsa_amd_agent_memory_pool_get_info(agents[1], pool, HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access) == 0);
    assert(access == HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED);
    unsigned gpuPools = 0;
    assert(hsa_amd_agent_iterate_memory_pools(agents[1], [](hsa_amd_memory_pool_t, void *p) {
        ++*static_cast<unsigned *>(p); return HSA_STATUS_SUCCESS;
    }, &gpuPools) == 0 && !gpuPools);

    const size_t hostGranule = mac_hsa::detail::hostPageSize();
    auto src = static_cast<uint32_t *>(allocate(hostGranule - 1));
    auto dst = static_cast<uint32_t *>(allocate(hostGranule));
    assert(reinterpret_cast<uintptr_t>(src) % 4096 == 0);
    assert(hsa_amd_agents_allow_access(1, &agents[0], nullptr, src) == 0);
    assert(hsa_amd_agents_allow_access(1, &agents[1], nullptr, src) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_memory_fill(src, 0x12345678, 1024) == 0);
    assert(hsa_amd_memory_fill(src + 1, 0, hostGranule / 4) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_memory_fill(reinterpret_cast<char *>(src) + 1, 0, 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_memory_copy(dst, src, 4096) == 0 && dst[1023] == 0x12345678);
    assert(hsa_memory_copy(dst + 1, src, hostGranule) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_memory_copy(dst, src, SIZE_MAX) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_memory_pool_free(src + 1) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    void *invalid = reinterpret_cast<void *>(1);
    assert(hsa_amd_memory_pool_allocate(pool, SIZE_MAX, 0, &invalid) == HSA_STATUS_ERROR_INVALID_ALLOCATION && !invalid);
    assert(hsa_amd_memory_pool_allocate(pool, 16, 1, &invalid) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_memory_pool_allocate({UINT64_MAX}, 16, 0, &invalid) == hsa_status_t(HSA_STATUS_ERROR_INVALID_MEMORY_POOL));

    hsa_amd_pointer_info_t info{}; info.size = sizeof(info);
    assert(hsa_amd_pointer_info_set_userdata(src, dst) == 0);
    uint32_t count = 0; hsa_agent_t *accessible = nullptr;
    assert(hsa_amd_pointer_info(src + 5, &info, [](size_t n) -> void * {
        uint64_t now; assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now) == 0);
        return std::malloc(n);
    }, &count, &accessible) == 0);
    assert(info.type == HSA_EXT_POINTER_TYPE_HSA && info.hostBaseAddress == src && info.sizeInBytes == hostGranule);
    assert(info.userData == dst && !info.registered && count == 1 && accessible[0].handle == agents[0].handle);
    std::free(accessible);
    std::memset(&info, 0xa5, sizeof(info)); info.size = 8;
    assert(hsa_amd_pointer_info(src, &info, nullptr, nullptr, nullptr) == 0);
    const auto bytes = reinterpret_cast<const unsigned char *>(&info);
    for (size_t i = 8; i < sizeof(info); ++i) assert(bytes[i] == 0xa5);
    int local = 0; info.size = sizeof(info);
    assert(hsa_amd_pointer_info(&local, &info, nullptr, &count, nullptr) == 0 && info.type == HSA_EXT_POINTER_TYPE_UNKNOWN && !count);

    auto dependency = signal(1), completed = signal(2);
    dst[0] = 0;
    assert(hsa_amd_memory_async_copy(dst, agents[0], src, agents[0], 4096, 1, &dependency, completed) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    assert(dst[0] == 0 && hsa_signal_load_relaxed(completed) == 2);
    src[0] = 0xabcdef;
    hsa_signal_store_screlease(dependency, 0);
    wait(completed, 1); assert(dst[0] == 0xabcdef);
    assert(hsa_amd_memory_async_copy(dst, agents[1], src, agents[0], 16, 0, nullptr, completed) == HSA_STATUS_ERROR_INVALID_AGENT);
    assert(hsa_amd_memory_async_copy(dst, agents[0], src, agents[0], 16, 0, nullptr, {UINT64_MAX}) == HSA_STATUS_ERROR_INVALID_SIGNAL);
    // Accepted work owns storage even if its public allocation is freed.
    hsa_signal_store_relaxed(dependency, 1);
    assert(hsa_amd_memory_async_copy(dst, agents[0], src, agents[0], 4096, 1, &dependency, completed) == 0);
    assert(hsa_amd_memory_pool_free(src) == 0);
    hsa_signal_store_screlease(dependency, 0);
    wait(completed); assert(dst[0] == 0xabcdef);

    auto doorbell = signal(-1);
    hsa_queue_t *queue = nullptr;
    assert(hsa_soft_queue_create(region, 3, HSA_QUEUE_TYPE_MULTI, 0, doorbell, &queue) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_soft_queue_create(region, 64, HSA_QUEUE_TYPE_MULTI, HSA_QUEUE_FEATURE_AGENT_DISPATCH, doorbell, &queue) == 0);
    assert(queue->size == 64 && queue->type == HSA_QUEUE_TYPE_MULTI && queue->doorbell_signal.handle == doorbell.handle);
    assert(reinterpret_cast<uintptr_t>(queue) % 64 == 0 && reinterpret_cast<uintptr_t>(queue->base_address) % 4096 == 0);
    for (unsigned i = 0; i < queue->size; ++i)
        assert(static_cast<uint16_t *>(queue->base_address)[i * 32] == HSA_PACKET_TYPE_INVALID);
    const auto amd = reinterpret_cast<amd_queue_t *>(queue);
    assert(amd->write_dispatch_id == 0 && amd->read_dispatch_id == 0);
    assert(amd->queue_properties & AMD_QUEUE_PROPERTIES_IS_PTR64);
    const std::array adds{hsa_queue_add_write_index_relaxed, hsa_queue_add_write_index_scacquire,
                          hsa_queue_add_write_index_screlease, hsa_queue_add_write_index_scacq_screl};
    const std::array cases{hsa_queue_cas_write_index_relaxed, hsa_queue_cas_write_index_scacquire,
                           hsa_queue_cas_write_index_screlease, hsa_queue_cas_write_index_scacq_screl};
    for (size_t i = 0; i < adds.size(); ++i) {
        hsa_queue_store_write_index_relaxed(queue, UINT64_MAX);
        assert(adds[i](queue, 1) == UINT64_MAX && hsa_queue_load_write_index_relaxed(queue) == 0);
        assert(cases[i](queue, 0, 7) == 0 && hsa_queue_load_write_index_scacquire(queue) == 7);
        assert(cases[i](queue, 3, 9) == 7 && amd->write_dispatch_id == 7);
    }
    hsa_queue_store_write_index_relaxed(queue, 0);
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) workers.emplace_back([=] {
        for (unsigned i = 0; i < 10000; ++i) hsa_queue_add_write_index_scacq_screl(queue, 1);
    });
    for (auto &worker : workers) worker.join();
    assert(hsa_queue_load_write_index_relaxed(queue) == 40000);
    // CPU packet publication through release/acquire queue indices. The runtime
    // does not consume software queues; this application thread is the consumer.
    hsa_queue_store_write_index_relaxed(queue, 0);
    hsa_queue_store_read_index_relaxed(queue, 0);
    std::thread consumer([=] {
        for (uint64_t i = 0; i < 2000; ++i) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (hsa_queue_load_write_index_scacquire(queue) <= i) assert(std::chrono::steady_clock::now() < deadline);
            assert(static_cast<uint64_t *>(queue->base_address)[(i % 64) * 8 + 1] == i * 17);
            hsa_queue_store_read_index_screlease(queue, i + 1);
        }
    });
    for (uint64_t i = 0; i < 2000; ++i) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (i - hsa_queue_load_read_index_scacquire(queue) >= 64) assert(std::chrono::steady_clock::now() < deadline);
        static_cast<uint64_t *>(queue->base_address)[(i % 64) * 8 + 1] = i * 17;
        hsa_queue_store_write_index_screlease(queue, i + 1);
    }
    consumer.join(); assert(hsa_queue_load_read_index_relaxed(queue) == 2000);
    assert(hsa_queue_inactivate(queue) == 0 && hsa_queue_inactivate(queue) == 0);
    assert(hsa_queue_destroy(queue) == 0 && hsa_queue_destroy(queue) == HSA_STATUS_ERROR_INVALID_QUEUE);
    assert(hsa_signal_load_relaxed(doorbell) == -1);

    // Final shutdown must not hang on an unsatisfied copy dependency. A nested
    // shutdown leaves the job alive; final shutdown cancels and joins it.
    hsa_signal_store_relaxed(dependency, 1);
    hsa_signal_store_relaxed(completed, 1);
    assert(hsa_amd_memory_async_copy(dst, agents[0], dst, agents[0], 16, 1, &dependency, completed) == 0);
    assert(hsa_init() == 0 && hsa_shut_down() == 0);
    assert(hsa_signal_load_relaxed(completed) == 1);
    const auto start = std::chrono::steady_clock::now();
    assert(hsa_shut_down() == 0);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    assert(hsa_init() == 0);
    assert(hsa_region_get_info(region, HSA_REGION_INFO_SIZE, &count) == HSA_STATUS_ERROR_INVALID_REGION);
    assert(hsa_amd_memory_pool_free(dst) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    assert(hsa_shut_down() == 0);
    gpuBuffers = true; agents.clear();
    assert(hsa_init() == 0);
    assert(hsa_iterate_agents([](hsa_agent_t a, void *) { agents.push_back(a); return HSA_STATUS_SUCCESS; }, nullptr) == 0);
    hsa_amd_memory_pool_t devicePool{};
    assert(hsa_amd_agent_iterate_memory_pools(agents[1], [](hsa_amd_memory_pool_t p, void *out) {
        *static_cast<hsa_amd_memory_pool_t *>(out) = p; return HSA_STATUS_SUCCESS;
    }, &devicePool) == 0 && devicePool.handle);
    size_t granule;
    assert(hsa_amd_memory_pool_get_info(devicePool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE, &granule) == 0 && granule == 16384);
    assert(hsa_amd_agent_memory_pool_get_info(agents[0], devicePool, HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access) == 0 && access == HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED);
    void *deviceA, *deviceB;
    assert(hsa_amd_memory_pool_allocate(devicePool, 16384, 0, &deviceA) == 0);
    assert(hsa_amd_memory_pool_allocate(devicePool, 16384, 0, &deviceB) == 0);
    std::vector<uint8_t> input(12003), output(16384);
    for (size_t i = 0; i < input.size(); ++i) input[i] = uint8_t(i * 79);
    auto interior = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(deviceA) + 3);
    assert(hsa_memory_copy(interior, input.data(), input.size()) == 0);
    assert(hsa_memory_copy(deviceB, deviceA, 16384) == 0);
    assert(hsa_memory_copy(output.data(), deviceB, output.size()) == 0);
    assert(std::memcmp(output.data() + 3, input.data(), input.size()) == 0);
    for (size_t i = 0; i < 3; ++i) assert(output[i] == 0x91);
    for (size_t i = input.size() + 3; i < output.size(); ++i) assert(output[i] == 0x91);
    assert(nativeCopies == 1 && nativeCopySizes.back() == 16384);
    assert(hsa_memory_copy(output.data(), deviceA, output.size()) == 0);
    std::memmove(output.data()+4,output.data(),12003);
    assert(hsa_memory_copy(reinterpret_cast<void *>(uintptr_t(deviceA)+4),deviceA,12003) == 0);
    std::vector<uint8_t> overlap(output.size());
    assert(hsa_memory_copy(overlap.data(),deviceA,overlap.size()) == 0 && overlap == output);
    assert(nativeCopies == 1); // overlapping ranges never sent to SDMA
    failCopy = true;
    assert(hsa_memory_copy(deviceB,deviceA,16384) == HSA_STATUS_ERROR && nativeCopies == 2);
    failCopy = false;
    assert(hsa_amd_memory_fill(deviceA, 0x76543210, 4096) == 0);
    info.size = sizeof(info);
    assert(hsa_amd_pointer_info(deviceA, &info, nullptr, nullptr, nullptr) == 0);
    assert(info.agentBaseAddress == deviceA && !info.hostBaseAddress && info.registered);
    completed = signal(1);
    assert(hsa_amd_memory_async_copy(output.data(), agents[0], deviceA, agents[1], output.size(), 0, nullptr, completed) == 0);
    wait(completed);
    uint32_t firstWord; std::memcpy(&firstWord, output.data(), 4); assert(firstWord == 0x76543210);
    hsa_signal_store_relaxed(completed, 1); failCopy = true;
    assert(hsa_amd_memory_async_copy(output.data(), agents[0], deviceA, agents[1], output.size(), 0, nullptr, completed) == 0);
    wait(completed, -1);
    assert(hsa_amd_memory_pool_free(deviceA) == 0);
    assert(hsa_shut_down() == 0 && gpuFrees == 2);
    // CPU-owned GPU-backed pools carry coarse/kernarg semantics without claiming
    // native CPU/GPU atomic interoperability. Exercise discovery and ownership
    // exactly as HRX does before requesting host-visible allocations.
    failCopy = false; sharedBuffers = true;
    for (bool multiple : {false, true}) {
        secondGPU = multiple; agents.clear();
        assert(hsa_init() == 0);
        assert(hsa_iterate_agents([](hsa_agent_t a, void *) { agents.push_back(a); return HSA_STATUS_SUCCESS; }, nullptr) == 0);
        std::vector<hsa_amd_memory_pool_t> hostPools;
        assert(hsa_amd_agent_iterate_memory_pools(agents[0], [](hsa_amd_memory_pool_t p, void *out) {
            uint32_t flags = 0;
            assert(hsa_amd_memory_pool_get_info(p, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags) == 0);
            static_cast<std::vector<hsa_amd_memory_pool_t> *>(out)->push_back(p);
            return HSA_STATUS_SUCCESS;
        }, &hostPools) == 0);
        assert(hostPools.size() == (multiple ? 3 : 2));
        unsigned visits = 0;
        assert(hsa_agent_iterate_regions(agents[0], [](hsa_region_t r, void *out) {
            uint32_t flags;
            assert(hsa_region_get_info(r, HSA_REGION_INFO_GLOBAL_FLAGS, &flags) == 0);
            ++*static_cast<unsigned *>(out); return HSA_STATUS_SUCCESS;
        }, &visits) == 0 && visits == hostPools.size());
        visits = 0;
        assert(hsa_amd_agent_iterate_memory_pools(agents[0], [](hsa_amd_memory_pool_t, void *out) {
            ++*static_cast<unsigned *>(out); return HSA_STATUS_INFO_BREAK;
        }, &visits) == HSA_STATUS_INFO_BREAK && visits == 1);
        uint32_t flags = 0;
        bool all = true;
        assert(hsa_amd_memory_pool_get_info(hostPools[0], HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags) == 0);
        assert(flags == HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED);
        assert(hsa_amd_memory_pool_get_info(hostPools[0], HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL, &all) == 0 && !all);
        if (!multiple) {
            hsa_amd_memory_pool_t vram{};
            assert(hsa_amd_agent_iterate_memory_pools(agents[1],[](hsa_amd_memory_pool_t p,void *out) {
                *static_cast<hsa_amd_memory_pool_t *>(out)=p;return HSA_STATUS_SUCCESS;
            },&vram)==0);
            void *a=nullptr,*b=nullptr;
            const size_t bytes=5*1024*1024+3;
            assert(hsa_amd_memory_pool_allocate(vram,bytes,0,&a)==0);
            assert(hsa_amd_memory_pool_allocate(vram,bytes,0,&b)==0);
            std::vector<uint8_t> input(bytes),output(bytes);
            for(size_t i=0;i<bytes;++i) input[i]=uint8_t(i*71+3);
            assert(hsa_memory_copy(a,input.data(),bytes)==0);
            nativeCopySizes.clear();
            assert(hsa_memory_copy(b,a,bytes)==0);
            assert((nativeCopySizes==std::vector<size_t>{4*1024*1024,1024*1024+3}));
            assert(hsa_memory_copy(output.data(),b,bytes)==0 && input==output);
            assert(hsa_memory_free(a)==0 && hsa_memory_free(b)==0);
        }
        const auto sharedPool = hostPools[1];
        assert(hsa_amd_memory_pool_get_info(sharedPool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags) == 0);
        assert(flags == (HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED | HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT));
        assert(hsa_amd_memory_pool_get_info(sharedPool, HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL, &all) == 0 && all == !multiple);
        hsa_amd_memory_pool_location_t location;
        assert(hsa_amd_memory_pool_get_info(sharedPool, HSA_AMD_MEMORY_POOL_INFO_LOCATION, &location) == 0 && location == HSA_AMD_MEMORY_POOL_LOCATION_CPU);
        size_t capacity = 0;
        assert(hsa_amd_memory_pool_get_info(sharedPool, HSA_AMD_MEMORY_POOL_INFO_ALLOC_MAX_SIZE, &capacity) == 0 && capacity == 65536);
        assert(hsa_region_get_info({sharedPool.handle}, HSA_REGION_INFO_ALLOC_MAX_SIZE, &capacity) == 0 && capacity == 65536);
        assert(hsa_amd_agent_memory_pool_get_info(agents[1], sharedPool, HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access) == 0 && access == HSA_AMD_MEMORY_POOL_ACCESS_ALLOWED_BY_DEFAULT);
        uint32_t hops = 0;
        assert(hsa_amd_agent_memory_pool_get_info(agents[1], sharedPool, HSA_AMD_AGENT_MEMORY_POOL_INFO_NUM_LINK_HOPS, &hops) == 0 && hops == 1);
        hsa_amd_memory_pool_link_info_t link{};
        assert(hsa_amd_agent_memory_pool_get_info(agents[1], sharedPool, HSA_AMD_AGENT_MEMORY_POOL_INFO_LINK_INFO, &link) == 0);
        assert(link.link_type == HSA_AMD_LINK_INFO_TYPE_PCIE && !link.atomic_support_32bit && !link.atomic_support_64bit);
        void *host = nullptr;
        const auto before = sharedAllocations;
        assert(hsa_amd_memory_pool_allocate(sharedPool, 65537, 0, &host) == HSA_STATUS_ERROR_INVALID_ALLOCATION && !host);
        assert(sharedAllocations == before);
        assert(hsa_amd_memory_pool_allocate(sharedPool, 16381, 0, &host) == 0);
        assert(uintptr_t(host) % 16384 == 0);
        assert(hsa_amd_agents_allow_access(2, agents.data(), nullptr, host) == 0);
        info = {}; info.size = sizeof(info); accessible = nullptr;
        assert(hsa_amd_pointer_info(static_cast<uint8_t *>(host) + 8, &info, std::malloc, &count, &accessible) == 0);
        assert(info.agentOwner.handle == agents[0].handle && info.hostBaseAddress == host && info.agentBaseAddress == host);
        assert(info.global_flags == flags && info.registered && info.sizeInBytes == 16384 && count == 2);
        assert(accessible[0].handle == agents[0].handle && accessible[1].handle == agents[1].handle);
        std::free(accessible);
        if (multiple) {
            assert(hsa_amd_agent_memory_pool_get_info(agents[2], sharedPool, HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access) == 0 && access == HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED);
            assert(hsa_amd_agents_allow_access(1, &agents[2], nullptr, host) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
            hops = 99;
            assert(hsa_amd_agent_memory_pool_get_info(agents[2], sharedPool, HSA_AMD_AGENT_MEMORY_POOL_INFO_NUM_LINK_HOPS, &hops) == 0 && hops == 0);
            link.min_latency = 0xfeed;
            assert(hsa_amd_agent_memory_pool_get_info(agents[2], sharedPool, HSA_AMD_AGENT_MEMORY_POOL_INFO_LINK_INFO, &link) == 0 && link.min_latency == 0xfeed);
        }
        assert(hsa_amd_memory_fill(host, 0xfedcba98, 4096) == 0);
        assert(static_cast<uint32_t *>(host)[4095] == 0xfedcba98);
        assert(hsa_memory_copy(static_cast<uint8_t *>(host) + 3, input.data(), input.size()) == 0);
        assert(std::memcmp(static_cast<uint8_t *>(host) + 3, input.data(), input.size()) == 0);
        assert(hsa_memory_copy(output.data(), host, 16384) == 0);
        assert(std::memcmp(output.data(), host, 16384) == 0);
        completed = signal(1);
        assert(hsa_amd_memory_async_copy(output.data(), agents[0], host, agents[1], 16384, 0, nullptr, completed) == 0);
        wait(completed);
        assert(hsa_amd_memory_pool_free(host) == 0);
        assert(hsa_shut_down() == 0 && sharedAllocations == sharedFrees);
    }
    puts("HSA: host pools, bounds, pointer ABI, async copy ordering/lifetime, software queue atomics/publication and cancellation pass");
}
