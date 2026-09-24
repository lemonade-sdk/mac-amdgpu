#include "runtime_state.h"
#include "code_object.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <fcntl.h>
#include <unistd.h>

static unsigned allocated = 0, freed = 0, uploaded = 0;
static bool failAllocation = false, failUpload = false, shortAllocation = false;
static bool failCodeSync = false;
static unsigned synchronizedUploads = 0;
static std::vector<uint8_t> lastUpload;
namespace mac_hsa {
struct TestConnection final : Connection {
    bool supportsBuffers() const override { return true; }
    hsa_status_t read(DeviceSnapshot &snapshot) override {
        snapshot = {1, 180, 15, 256ull << 20, 32ull << 30, 12, 0, 1}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateBuffer(uint64_t size, DeviceBuffer &buffer) override {
        if (failAllocation) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        buffer = {++allocated, 0x8010000000ull, shortAllocation ? 1 : size}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &) override {
        uint64_t now;
        // A backend may reenter the runtime: final release must not hold its lock.
        hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now);
        ++freed; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t writeBuffer(const DeviceBuffer &, uint64_t offset, const void *data, size_t size) override {
        assert(offset == 0); ++uploaded;
        if (failUpload) return HSA_STATUS_ERROR;
        auto bytes = static_cast<const uint8_t *>(data);
        lastUpload.assign(bytes, bytes + size); return HSA_STATUS_SUCCESS;
    }
    hsa_status_t invalidateCodeCaches() override {
        assert(uploaded > synchronizedUploads && !failUpload && !lastUpload.empty());
        ++synchronizedUploads;
        return failCodeSync ? HSA_STATUS_ERROR : HSA_STATUS_SUCCESS;
    }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    connections.push_back(std::make_shared<TestConnection>()); return HSA_STATUS_SUCCESS;
}
}
static hsa_agent_t gpu;
static hsa_executable_t executable() {
    hsa_executable_t result{};
    assert(hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, nullptr, &result) == 0);
    return result;
}
int main(int argc, char **argv) {
    assert(argc>=2 && argc<=4);
    std::ifstream stream(argv[1], std::ios::binary);
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(stream), {}};
    mac_hsa::CodeObject expected;
    assert(mac_hsa::parseCodeObject(file, expected));
    hsa_ven_amd_loader_1_03_pfn_t loader{};
    assert(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 1, sizeof(loader), &loader) == HSA_STATUS_ERROR_NOT_INITIALIZED);
    assert(hsa_init() == 0);
    assert(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 1, sizeof(loader), &loader) == 0);
    // An older or oversized caller receives exactly the supported prefix.
    std::array<uint8_t, sizeof(loader) + 8> table;
    table.fill(0xa5);
    assert(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 1, 3, table.data()) == 0);
    assert(!std::memcmp(table.data(), &loader, 3) && table[3] == 0xa5);
    assert(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 1, table.size(), table.data()) == 0);
    assert(!std::memcmp(table.data(), &loader, sizeof(loader)) && table[sizeof(loader)] == 0xa5);
    assert(hsa_iterate_agents([](hsa_agent_t agent, void *) {
        hsa_device_type_t type;
        assert(hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) == 0);
        if (type == HSA_DEVICE_TYPE_GPU) gpu = agent;
        return HSA_STATUS_SUCCESS;
    }, nullptr) == 0 && gpu.handle);
    hsa_code_object_reader_t reader{};
    assert(hsa_code_object_reader_create_from_memory(nullptr, 1, &reader) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_code_object_reader_create_from_memory(file.data(), file.size(), &reader) == 0);
    std::fill(file.begin(), file.end(), 0); // reader owns a copy
    auto exec = executable();
    hsa_loaded_code_object_t loaded{};
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, &loaded) == 0 && loaded.handle);
    assert(lastUpload == expected.image && allocated == 1 && uploaded == 1 && freed == 0);
    assert(synchronizedUploads == 1);
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_VARIABLE_ALREADY_DEFINED);
    assert(allocated == 1);
    hsa_executable_t owner{};
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_EXECUTABLE, &owner) == 0 && owner.handle == exec.handle);
    uint64_t loadBase = 0;
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_BASE, &loadBase) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    hsa_executable_symbol_t symbol{};
    assert(hsa_executable_get_symbol_by_name(exec, "vector_add.kd", &gpu, &symbol) == 0);
    uint64_t address = 1;
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &address) == 0 && address == 0);
    uint32_t validation = UINT32_MAX;
    assert(hsa_executable_validate_alt(exec, nullptr, &validation) == 0 && validation == 0);
    assert(hsa_executable_freeze(exec, nullptr) == 0);
    assert(hsa_executable_freeze(exec, nullptr) == HSA_STATUS_ERROR_FROZEN_EXECUTABLE);
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_FROZEN_EXECUTABLE);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &address) == 0);
    assert(address == 0x8010000000ull + expected.kernels[0].descriptor);
    const void *descriptor = nullptr;
    assert(loader.hsa_ven_amd_loader_query_host_address(reinterpret_cast<void *>(address), &descriptor) == 0);
    assert(!std::memcmp(descriptor, lastUpload.data() + expected.kernels[0].descriptor, 64));
    const void *same = nullptr;
    assert(loader.hsa_ven_amd_loader_query_host_address(descriptor, &same) == 0 && same == descriptor);
    assert(loader.hsa_ven_amd_loader_query_host_address(reinterpret_cast<void *>(0x8010000000ull + expected.image.size()), &same) == HSA_STATUS_ERROR_INVALID_ARGUMENT && !same);
    assert(loader.hsa_ven_amd_loader_query_executable(reinterpret_cast<void *>(address), &owner) == 0 && owner.handle == exec.handle);
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_BASE, &loadBase) == 0 && loadBase == 0x8010000000ull);
    uint64_t loadSize = 0;
    int64_t loadDelta = 0;
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_SIZE, &loadSize) == 0 && loadSize == lastUpload.size());
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_DELTA, &loadDelta) == 0 && loadDelta == int64_t(loadBase - expected.virtualBase));
    struct Iteration { hsa_loaded_code_object_t handle; unsigned calls = 0; } iteration{loaded};
    assert(loader.hsa_ven_amd_loader_executable_iterate_loaded_code_objects(exec,
        [](hsa_executable_t e, hsa_loaded_code_object_t l, void *p) {
            auto &state = *static_cast<Iteration *>(p); ++state.calls;
            assert(l.handle == state.handle.handle);
            hsa_executable_t owner{};
            assert(hsa_ven_amd_loader_loaded_code_object_get_info(l,
                HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_EXECUTABLE, &owner) == 0 && owner.handle == e.handle);
            return HSA_STATUS_INFO_BREAK;
        }, &iteration) == HSA_STATUS_INFO_BREAK && iteration.calls == 1);
    assert(loader.hsa_ven_amd_loader_iterate_executables([](hsa_executable_t e, void *p) {
        assert(e.handle == static_cast<hsa_executable_t *>(p)->handle);
        uint32_t validation;
        assert(hsa_executable_validate_alt(e, nullptr, &validation) == 0);
        return HSA_STATUS_INFO_BREAK;
    }, &exec) == HSA_STATUS_INFO_BREAK);
    size_t segmentCount = 0;
    assert(loader.hsa_ven_amd_loader_query_segment_descriptors(nullptr, &segmentCount) == 0 && segmentCount >= 3);
    std::vector<hsa_ven_amd_loader_segment_descriptor_t> segments(segmentCount + 1);
    ++segmentCount;
    assert(loader.hsa_ven_amd_loader_query_segment_descriptors(segments.data(), &segmentCount) == HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    --segmentCount;
    assert(loader.hsa_ven_amd_loader_query_segment_descriptors(segments.data(), &segmentCount) == 0);
    for (size_t i = 0; i < segmentCount; ++i) {
        const auto &segment = segments[i];
        assert(segment.executable.handle == exec.handle && segment.agent.handle == gpu.handle);
        if (segment.code_object_storage_type == HSA_VEN_AMD_LOADER_CODE_OBJECT_STORAGE_TYPE_MEMORY) {
            assert(segment.code_object_storage_size == file.size());
            assert(!std::memcmp(segment.code_object_storage_base, "\177ELF", 4));
        }
    }
    uint32_t uriSize = 0;
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_URI_LENGTH, &uriSize) == 0);
    std::vector<char> uri(uriSize + 2, '!');
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_URI, uri.data()) == 0);
    assert(std::string(uri.data()).starts_with("memory://") && uri[uriSize] == 0 && uri[uriSize + 1] == '!');
    uint32_t value;
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE, &value) == 0 && value == 12);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_ALIGNMENT, &value) == 0 && value == 8);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH, &value) == 0 && value == 13);
    char name[14]; std::memset(name, '!', sizeof(name));
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME, name) == 0);
    assert(std::memcmp(name, "vector_add.kd", 13) == 0 && name[13] == '!');
    assert(hsa_code_object_reader_destroy(reader) == 0);
    assert(hsa_code_object_reader_destroy(reader) == HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER);
    // Reader destruction leaves storage and translated descriptor pointers alive.
    assert(!std::memcmp(descriptor, expected.image.data() + expected.kernels[0].descriptor, 64));
    assert(hsa_executable_destroy(exec) == 0 && freed == 1);
    assert(loader.hsa_ven_amd_loader_query_host_address(reinterpret_cast<void *>(address), &descriptor) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_SIZE, &loadSize) == HSA_STATUS_ERROR_INVALID_CODE_OBJECT);
    segmentCount = 0;
    assert(loader.hsa_ven_amd_loader_query_segment_descriptors(nullptr, &segmentCount) == 0 && segmentCount == 0);
    assert(hsa_executable_destroy(exec) == HSA_STATUS_ERROR_INVALID_EXECUTABLE);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &address) == HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL);
    // Embedded file code objects: pread must preserve the caller's file offset.
    char temporary[] = "/tmp/mac-hsa-loader-XXXXXX";
    const int fd = mkstemp(temporary); assert(fd >= 0);
    stream.clear(); stream.seekg(0);
    std::vector<uint8_t> original{std::istreambuf_iterator<char>(stream), {}};
    std::array<uint8_t, 128> prefix{};
    assert(write(fd, prefix.data(), prefix.size()) == ssize_t(prefix.size()));
    assert(write(fd, original.data(), original.size()) == ssize_t(original.size()));
    assert(lseek(fd, 17, SEEK_SET) == 17);
    assert(loader.hsa_ven_amd_loader_code_object_reader_create_from_file_with_offset_size(fd, prefix.size(), original.size(), &reader) == 0);
    assert(lseek(fd, 0, SEEK_CUR) == 17); close(fd);
    exec = executable();
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, &loaded) == 0);
    assert(hsa_code_object_reader_destroy(reader) == 0);
    int retainedFD = -1;
    assert(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded,
        HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_CODE_OBJECT_STORAGE_FILE, &retainedFD) == 0);
    char magic[4]; assert(pread(retainedFD, magic, 4, prefix.size()) == 4 && !std::memcmp(magic, "\177ELF", 4));
    assert(hsa_executable_destroy(exec) == 0 && fcntl(retainedFD, F_GETFD) == -1);
    unlink(temporary);
    assert(loader.hsa_ven_amd_loader_code_object_reader_create_from_file_with_offset_size(-1, 0, 1, &reader) == HSA_STATUS_ERROR_INVALID_FILE);
    // Rejected images and backend failures leave no published symbols or buffers.
    assert(hsa_code_object_reader_create_from_memory(file.data(), file.size(), &reader) == 0);
    exec = executable();
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_INVALID_CODE_OBJECT);
    assert(hsa_code_object_reader_destroy(reader) == 0);
    stream.clear(); stream.seekg(0); file.assign(std::istreambuf_iterator<char>(stream), {});
    assert(hsa_code_object_reader_create_from_memory(file.data(), file.size(), &reader) == 0);
    failAllocation = true;
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    failAllocation = false; shortAllocation = true;
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    shortAllocation = false; failUpload = true;
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR);
    failUpload = false;
    failCodeSync = true;
    loaded = {};
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, &loaded) == HSA_STATUS_ERROR);
    assert(!loaded.handle);
    failCodeSync = false;
    assert(allocated == freed);
    assert(hsa_executable_get_symbol_by_name(exec, "vector_add.kd", &gpu, &symbol) == HSA_STATUS_ERROR_INVALID_SYMBOL_NAME);
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == 0);
    // Two images cannot publish an ambiguous address-only loader translation.
    auto collision = executable();
    assert(hsa_executable_load_agent_code_object(collision, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    assert(hsa_executable_destroy(collision) == 0);
    assert(hsa_init() == 0 && hsa_shut_down() == 0 && allocated == freed + 1);
    assert(hsa_shut_down() == 0 && allocated == freed);
    assert(hsa_executable_freeze(exec, nullptr) == HSA_STATUS_ERROR_NOT_INITIALIZED);
    assert(hsa_init() == 0);
    assert(hsa_executable_freeze(exec, nullptr) == HSA_STATUS_ERROR_INVALID_EXECUTABLE);
    assert(hsa_code_object_reader_destroy(reader) == HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER);
    assert(hsa_shut_down() == 0);
    if (argc>=3) {
        // The pinned HRX helper library is a generic-v1 image with many kernels,
        // not the single-target/single-symbol shader used above.
        assert(hsa_init()==0);
        std::ifstream helperStream(argv[2],std::ios::binary);
        const std::vector<uint8_t> helperBytes{std::istreambuf_iterator<char>(helperStream),{}};
        mac_hsa::CodeObject helperObject;
        assert(mac_hsa::parseCodeObject(helperBytes,helperObject) && helperObject.kernels.size()>=17);
        assert(hsa_iterate_agents([](hsa_agent_t a,void *) {
            hsa_device_type_t type;assert(hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type)==0);
            if (type==HSA_DEVICE_TYPE_GPU) gpu=a;
            return HSA_STATUS_SUCCESS;
        },nullptr)==0);
        assert(hsa_code_object_reader_create_from_memory(helperBytes.data(),helperBytes.size(),&reader)==0);
        exec=executable();
        assert(hsa_executable_load_agent_code_object(exec,gpu,reader,nullptr,&loaded)==0);
        assert(hsa_executable_freeze(exec,nullptr)==0);
        assert(hsa_code_object_reader_destroy(reader)==0);
        for (const auto &helper:helperObject.kernels) {
            assert(hsa_executable_get_symbol_by_name(exec,helper.symbol.c_str(),&gpu,&symbol)==0);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&address)==0);
            assert(address==0x8010000000ull+helper.descriptor);
            assert(loader.hsa_ven_amd_loader_query_host_address(reinterpret_cast<void *>(address),&descriptor)==0);
            assert(!std::memcmp(descriptor,helperObject.image.data()+helper.descriptor,64));
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&value)==0 && value==helper.kernargSize);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&value)==0 && value==helper.privateSize);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&value)==0 && value==helper.groupSize);
        }
        assert(hsa_executable_destroy(exec)==0 && hsa_shut_down()==0 && allocated==freed);
    }
    if (argc==4) {
        // Exercise the exact native image passed through HRX's public loader.
        // Its bindings occupy 0/8/16 and affine constants occupy 24/28.
        assert(hsa_init()==0);
        std::ifstream computeStream(argv[3],std::ios::binary);
        const std::vector<uint8_t> computeBytes{std::istreambuf_iterator<char>(computeStream),{}};
        mac_hsa::CodeObject computeObject;
        assert(mac_hsa::parseCodeObject(computeBytes,computeObject) && computeObject.kernels.size()==2);
        assert(hsa_iterate_agents([](hsa_agent_t a,void *) {
            hsa_device_type_t type;assert(hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type)==0);
            if (type==HSA_DEVICE_TYPE_GPU) gpu=a;
            return HSA_STATUS_SUCCESS;
        },nullptr)==0);
        assert(hsa_code_object_reader_create_from_memory(computeBytes.data(),computeBytes.size(),&reader)==0);
        exec=executable();
        assert(hsa_executable_load_agent_code_object(exec,gpu,reader,nullptr,&loaded)==0);
        assert(hsa_executable_freeze(exec,nullptr)==0);
        assert(hsa_code_object_reader_destroy(reader)==0);
        unsigned seen=0;
        for (const auto &kernel:computeObject.kernels) {
            const bool affine=kernel.symbol=="hrx_vector_affine.kd";
            assert(affine || kernel.symbol=="hrx_matmul_16.kd");
            const unsigned bit=affine ? 1 : 2;
            assert(!(seen&bit));seen|=bit;
            assert(kernel.name==(affine ? "hrx_vector_affine" : "hrx_matmul_16"));
            assert(kernel.kernargSize==(affine ? 32u : 24u) && kernel.kernargAlignment==8);
            assert(kernel.privateSize==0 && kernel.groupSize==0 && !kernel.dynamicStack);
            assert(kernel.properties&(1u<<10)); // AMD_KERNEL_CODE_PROPERTIES_ENABLE_WAVEFRONT_SIZE32
            assert(hsa_executable_get_symbol_by_name(exec,kernel.symbol.c_str(),&gpu,&symbol)==0);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&address)==0);
            assert(address==0x8010000000ull+kernel.descriptor);
            assert(loader.hsa_ven_amd_loader_query_host_address(reinterpret_cast<void *>(address),&descriptor)==0);
            assert(!std::memcmp(descriptor,computeObject.image.data()+kernel.descriptor,64));
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&value)==0 && value==kernel.kernargSize);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_ALIGNMENT,&value)==0 && value==8);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&value)==0 && value==0);
            assert(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&value)==0 && value==0);
        }
        assert(seen==3);
        assert(hsa_executable_destroy(exec)==0 && hsa_shut_down()==0 && allocated==freed);
        puts("HRX native compute: both gfx1201 exports load/freeze with exact kernarg ABI and wave32 descriptors");
    }
    puts("HSA executable: copied readers, real ELF upload, symbols, freeze, rejection, failure cleanup and shutdown pass");
}
