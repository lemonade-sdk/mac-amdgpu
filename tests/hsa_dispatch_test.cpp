#include "mac_hsa.h"
#include "runtime_state.h"
#include <cassert>
#include <fstream>
#include <functional>
#include <iterator>

static std::map<uint64_t, std::vector<uint8_t>> storage;
static uint64_t nextBuffer, launches;
static uint64_t expectedDataAddress;
static unsigned sharedLive = 0;
static std::function<void()> duringDispatch;
namespace mac_hsa {
struct DispatchConnection : Connection {
    bool supportsBuffers() const override { return true; }
    hsa_status_t read(DeviceSnapshot &s) override {
        s = {1, 182, 15, 256ull << 20, 32ull << 30, 12, 0, 1}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateBuffer(uint64_t bytes, DeviceBuffer &out) override {
        const auto handle = ++nextBuffer;
        const auto rounded = (bytes + 16383) & ~uint64_t(16383);
        storage[handle].resize(rounded);
        out = {handle, 0x8000000000ull + handle * 0x100000, rounded}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        uint64_t now;
        hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now); // no runtime lock held
        assert(storage.erase(buffer.handle) == 1); return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateSharedBuffer(uint64_t bytes, SharedBuffer &out) override {
        if (posix_memalign(&out.host, 16384, bytes)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        out.device = {++nextBuffer, reinterpret_cast<uintptr_t>(out.host), bytes};
        storage[out.device.handle].resize(bytes); ++sharedLive;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override {
        assert(sharedLive && storage.erase(buffer.device.handle) == 1);
        std::free(buffer.host); --sharedLive; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t writeBuffer(const DeviceBuffer &buffer, uint64_t offset, const void *data, size_t bytes) override {
        assert(offset <= buffer.size && bytes <= buffer.size - offset);
        std::memcpy(storage.at(buffer.handle).data() + offset, data, bytes); return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatch(const amdgpu::ComputeDispatchRequest &r, uint64_t &fence) override {
        assert(amdgpu::compute_dispatch_shape(r) && r.codeOffset % 256 == 0 && r.codeBytes);
        assert(r.groups[0] == 4 && r.groups[1] == 1 && r.groups[2] == 1);
        assert(r.threads[0] == 32 && r.threads[1] == 1 && r.threads[2] == 1);
        assert(r.version == 2 && r.rsrc1 == 0xe00f0000 && r.rsrc2 == 0x84 && r.rsrc3 == 0x10 && r.userSGPRCount == 2);
        assert(storage.size() == 3); // code, data, per-launch kernargs
        assert((uint64_t(r.userSGPR[1]) << 32 | r.userSGPR[0]) == 0x8000000000ull + r.buffers[1] * 0x100000);
        const auto &args = storage.at(r.buffers[1]);
        uint64_t pointer; uint32_t seed;
        std::memcpy(&pointer, args.data(), 8); std::memcpy(&seed, args.data() + 8, 4);
        assert(pointer == expectedDataAddress && seed == 0x12345678);
        if (duringDispatch) duringDispatch();
        assert(storage.contains(r.codeHandle) && storage.contains(r.buffers[0]) && storage.contains(r.buffers[1]));
        fence = ++launches; return HSA_STATUS_SUCCESS;
    }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &out) {
    out.push_back(std::make_shared<DispatchConnection>()); return HSA_STATUS_SUCCESS;
}
}
int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    const uint32_t groups[] = {4, 1, 1}, threads[] = {32, 1, 1};
    uint64_t fence = 123;
    assert(mac_hsa_executable_dispatch({}, nullptr, 0, groups, threads, nullptr, 0, &fence) == HSA_STATUS_ERROR_NOT_INITIALIZED && !fence);
    assert(hsa_init() == 0);
    hsa_agent_t gpu{};
    assert(hsa_iterate_agents([](hsa_agent_t a, void *out) {
        hsa_device_type_t type; assert(hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &type) == 0);
        if (type == HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(out) = a;
        return HSA_STATUS_SUCCESS;
    }, &gpu) == 0 && gpu.handle);
    hsa_amd_memory_pool_t pool{};
    assert(hsa_amd_agent_iterate_memory_pools(gpu, [](hsa_amd_memory_pool_t p, void *out) {
        *static_cast<hsa_amd_memory_pool_t *>(out) = p; return HSA_STATUS_SUCCESS;
    }, &pool) == 0 && pool.handle);
    hsa_code_object_reader_t reader{}; hsa_executable_t executable{}; hsa_executable_symbol_t symbol{};
    assert(hsa_code_object_reader_create_from_memory(bytes.data(), bytes.size(), &reader) == 0);
    assert(hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr, &executable) == 0);
    assert(hsa_executable_load_agent_code_object(executable, gpu, reader, nullptr, nullptr) == 0);
    assert(hsa_code_object_reader_destroy(reader) == 0);
    assert(hsa_executable_get_symbol_by_name(executable, "vector_add.kd", &gpu, &symbol) == 0);
    uint8_t args[12]{};
    assert(mac_hsa_executable_dispatch(symbol, args, 12, groups, threads, nullptr, 0, &fence) == HSA_STATUS_ERROR_INVALID_EXECUTABLE);
    assert(hsa_executable_freeze(executable, nullptr) == 0);
    void *data = nullptr;
    assert(hsa_amd_memory_pool_allocate(pool, 16384, 0, &data) == 0);
    uint64_t address = reinterpret_cast<uintptr_t>(data); uint32_t seed = 0x12345678;
    expectedDataAddress = address;
    std::memcpy(args, &address, 8); std::memcpy(args + 8, &seed, 4);
    const void *buffers[] = {data};
    assert(mac_hsa_executable_dispatch(symbol, args, 11, groups, threads, buffers, 1, &fence) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    const void *cpuBuffers[] = {args};
    assert(mac_hsa_executable_dispatch(symbol, args, 12, groups, threads, cpuBuffers, 1, &fence) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    const uint32_t invalidGroups[] = {0, 1, 1};
    assert(mac_hsa_executable_dispatch(symbol, args, 12, invalidGroups, threads, buffers, 1, &fence) == HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    assert(storage.size() == 2 && !launches);
    assert(mac_hsa_executable_dispatch(symbol, args, 12, groups, threads, buffers, 1, &fence) == 0 && fence == 1);
    assert(hsa_memory_free(data) == 0);
    assert(mac_hsa_memory_allocate_shared({}, 16384, &data) == HSA_STATUS_ERROR_INVALID_AGENT && !data);
    assert(mac_hsa_memory_allocate_shared(gpu, 0, &data) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    assert(mac_hsa_memory_allocate_shared(gpu, 16384, &data) == 0 && data && sharedLive == 1);
    expectedDataAddress = reinterpret_cast<uintptr_t>(data);
    std::memcpy(args, &expectedDataAddress, 8); buffers[0] = data;
    hsa_amd_pointer_info_t info{}; info.size = sizeof(info);
    hsa_agent_t *accessible = nullptr; uint32_t accessibleCount = 0;
    assert(hsa_amd_pointer_info(data, &info, std::malloc, &accessibleCount, &accessible) == 0);
    assert(info.hostBaseAddress == data && info.agentBaseAddress == data && accessibleCount == 2);
    assert(info.global_flags == HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED);
    assert(hsa_amd_agents_allow_access(accessibleCount, accessible, nullptr, data) == 0);
    std::free(accessible);
    duringDispatch = [&] {
        assert(hsa_memory_free(data) == 0);
        assert(hsa_executable_destroy(executable) == 0);
        assert(storage.size() == 3); // public destruction cannot recycle in-flight BOs
    };
    assert(mac_hsa_executable_dispatch(symbol, args, 12, groups, threads, buffers, 1, &fence) == 0 && fence == 2);
    assert(storage.empty() && !sharedLive);
    assert(mac_hsa_executable_dispatch(symbol, args, 12, groups, threads, nullptr, 0, &fence) == HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL && !fence);
    assert(hsa_shut_down() == 0);
    puts("HSA native dispatch: descriptor arguments, bounds and in-flight executable/buffer retention passed");
}
