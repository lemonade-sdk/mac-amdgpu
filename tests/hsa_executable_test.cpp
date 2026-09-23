#include "runtime_state.h"
#include "code_object.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

static unsigned allocated = 0, freed = 0, uploaded = 0;
static bool failAllocation = false, failUpload = false, shortAllocation = false;
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
    assert(argc == 2);
    std::ifstream stream(argv[1], std::ios::binary);
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(stream), {}};
    mac_hsa::CodeObject expected;
    assert(mac_hsa::parseCodeObject(file, expected));
    assert(hsa_init() == 0);
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
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == HSA_STATUS_ERROR_VARIABLE_ALREADY_DEFINED);
    assert(allocated == 1);
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
    uint32_t value;
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE, &value) == 0 && value == 12);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_ALIGNMENT, &value) == 0 && value == 8);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH, &value) == 0 && value == 13);
    char name[14]; std::memset(name, '!', sizeof(name));
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_NAME, name) == 0);
    assert(std::memcmp(name, "vector_add.kd", 13) == 0 && name[13] == '!');
    assert(hsa_code_object_reader_destroy(reader) == 0);
    assert(hsa_code_object_reader_destroy(reader) == HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER);
    assert(hsa_executable_destroy(exec) == 0 && freed == 1);
    assert(hsa_executable_destroy(exec) == HSA_STATUS_ERROR_INVALID_EXECUTABLE);
    assert(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &address) == HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL);
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
    assert(allocated == freed);
    assert(hsa_executable_get_symbol_by_name(exec, "vector_add.kd", &gpu, &symbol) == HSA_STATUS_ERROR_INVALID_SYMBOL_NAME);
    assert(hsa_executable_load_agent_code_object(exec, gpu, reader, nullptr, nullptr) == 0);
    assert(hsa_init() == 0 && hsa_shut_down() == 0 && allocated == freed + 1);
    assert(hsa_shut_down() == 0 && allocated == freed);
    assert(hsa_executable_freeze(exec, nullptr) == HSA_STATUS_ERROR_NOT_INITIALIZED);
    assert(hsa_init() == 0);
    assert(hsa_executable_freeze(exec, nullptr) == HSA_STATUS_ERROR_INVALID_EXECUTABLE);
    assert(hsa_code_object_reader_destroy(reader) == HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER);
    assert(hsa_shut_down() == 0);
    puts("HSA executable: copied readers, real ELF upload, symbols, freeze, rejection, failure cleanup and shutdown pass");
}
