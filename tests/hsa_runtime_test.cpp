#include <hsa/hsa_ven_amd_loader.h>
#include <hsa/hsa_ext_amd.h>
#include "transport.h"
#include "mac_hsa.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <thread>

static unsigned discovered, closed;
static hsa_status_t discoveryStatus = HSA_STATUS_SUCCESS;
static hsa_status_t readStatus = HSA_STATUS_SUCCESS;
static bool present = true;
static hsa_status_t propertyStatus=HSA_STATUS_SUCCESS;
static uint64_t gpuTimestampFrequency=100000000;
namespace mac_hsa {
struct TestConnection final : Connection {
    hsa_status_t properties(DeviceProperties &properties) override {
        properties={0x7551,0xc0,0x500,0,64,4,1,gpuTimestampFrequency,32,32};
        return propertyStatus;
    }
    ~TestConnection() override { ++closed; }
    hsa_status_t read(DeviceSnapshot &snapshot) override {
        if (readStatus != HSA_STATUS_SUCCESS) return readStatus;
        snapshot = {0xabc, 173, 15, 256ull << 20, 32ull << 30, 12, 0, 1};
        return HSA_STATUS_SUCCESS;
    }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    ++discovered;
    if (present) connections.push_back(std::make_shared<TestConnection>());
    return discoveryStatus;
}
}
static std::vector<hsa_agent_t> observed;
static hsa_status_t collect(hsa_agent_t agent, void *) {
    // Reentrant API calls must not deadlock under the enumeration lock.
    assert(hsa_init() == HSA_STATUS_SUCCESS);
    char name[64];
    std::memset(name, 0xff, sizeof(name));
    assert(hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name) == HSA_STATUS_SUCCESS);
    assert(name[63] == 0);
    assert(hsa_shut_down() == HSA_STATUS_SUCCESS);
    observed.push_back(agent);
    return HSA_STATUS_SUCCESS;
}
int main() {
    uint64_t timestamp = 0;
    assert(hsa_shut_down() == HSA_STATUS_ERROR_NOT_INITIALIZED);
    assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &timestamp) == HSA_STATUS_ERROR_NOT_INITIALIZED);
    discoveryStatus = HSA_STATUS_ERROR;
    assert(hsa_init() == HSA_STATUS_ERROR && closed == 1);
    assert(hsa_iterate_agents(collect, nullptr) == HSA_STATUS_ERROR_NOT_INITIALIZED);
    discoveryStatus = HSA_STATUS_SUCCESS;
    assert(hsa_init() == HSA_STATUS_SUCCESS);
    assert(hsa_init() == HSA_STATUS_SUCCESS && discovered == 2);
    assert(hsa_iterate_agents(nullptr, nullptr) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_iterate_agents(collect, nullptr) == HSA_STATUS_SUCCESS && observed.size() == 2);
    const auto cpu = observed[0], gpu = observed[1];
    uint32_t property=99;
    assert(hsa_agent_get_info(cpu,HSA_AGENT_INFO_NODE,&property)==0 && property==0);
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_NODE,&property)==0 && property==1);
    hsa_agent_t nearest{};
    assert(hsa_agent_get_info(gpu,hsa_agent_info_t(HSA_AMD_AGENT_INFO_NEAREST_CPU),&nearest)==0 && nearest.handle==cpu.handle);
    char uuid[21];std::memset(uuid,'!',sizeof(uuid));
    assert(hsa_agent_get_info(gpu,hsa_agent_info_t(HSA_AMD_AGENT_INFO_UUID),uuid)==0 && !std::strcmp(uuid,"GPU-XX") && uuid[20]=='!');
    const std::pair<uint32_t,uint32_t> properties[]={
        {HSA_AMD_AGENT_INFO_CHIP_ID,0x7551},{HSA_AMD_AGENT_INFO_ASIC_REVISION,0xc0},
        {HSA_AMD_AGENT_INFO_BDFID,0x500},{HSA_AMD_AGENT_INFO_DOMAIN,0},
        {HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT,64},{HSA_AMD_AGENT_INFO_NUM_SHADER_ENGINES,4},
        {HSA_AMD_AGENT_INFO_NUM_SHADER_ARRAYS_PER_SE,1},{HSA_AMD_AGENT_INFO_MAX_WAVES_PER_CU,32},
        {HSA_AGENT_INFO_WAVEFRONT_SIZE,32}};
    for (const auto &[attribute,expected]:properties) {
        assert(hsa_agent_get_info(gpu,hsa_agent_info_t(attribute),&property)==0 && property==expected);
    }
    uint64_t frequency=99;
    assert(hsa_agent_get_info(gpu,hsa_agent_info_t(HSA_AMD_AGENT_INFO_TIMESTAMP_FREQUENCY),&frequency)==0 && frequency==100000000);
    gpuTimestampFrequency=0;frequency=99;
    assert(hsa_agent_get_info(gpu,hsa_agent_info_t(HSA_AMD_AGENT_INFO_TIMESTAMP_FREQUENCY),&frequency)==HSA_STATUS_ERROR && frequency==99);
    gpuTimestampFrequency=100000000;propertyStatus=HSA_STATUS_ERROR;property=99;
    assert(hsa_agent_get_info(gpu,hsa_agent_info_t(HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT),&property)==HSA_STATUS_ERROR && property==99);
    propertyStatus=HSA_STATUS_SUCCESS;
    unsigned count = 0;
    assert(hsa_iterate_agents([](hsa_agent_t, void *p) {
        ++*static_cast<unsigned *>(p); return HSA_STATUS_INFO_BREAK;
    }, &count) == HSA_STATUS_INFO_BREAK && count == 1);
    assert(hsa_iterate_agents([](hsa_agent_t, void *) { return HSA_STATUS_ERROR; }, nullptr) == HSA_STATUS_ERROR);
    uint32_t features = UINT32_MAX;
    assert(hsa_agent_get_info(gpu, HSA_AGENT_INFO_FEATURE, &features) == HSA_STATUS_SUCCESS && !features);
    assert(hsa_agent_get_info(gpu, HSA_AGENT_INFO_QUEUES_MAX, &features) == HSA_STATUS_SUCCESS && !features);
    char name[64];
    assert(hsa_agent_get_info(gpu, HSA_AGENT_INFO_NAME, name) == HSA_STATUS_SUCCESS && !std::strcmp(name, "gfx1201"));
    assert(hsa_agent_get_info({UINT64_MAX}, HSA_AGENT_INFO_NAME, name) == HSA_STATUS_ERROR_INVALID_AGENT);
    assert(hsa_agent_get_info(cpu, HSA_AGENT_INFO_NAME, nullptr) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    hsa_isa_t isa{};
    assert(hsa_agent_iterate_isas(cpu, [](hsa_isa_t, void *) { assert(false); return HSA_STATUS_ERROR; }, nullptr) == 0);
    assert(hsa_agent_iterate_isas(gpu, [](hsa_isa_t value, void *out) {
        *static_cast<hsa_isa_t *>(out) = value;
        uint32_t length = 0;
        assert(hsa_isa_get_info_alt(value, HSA_ISA_INFO_NAME_LENGTH, &length) == 0);
        std::vector<char> name(length + 1, '!');
        assert(hsa_isa_get_info_alt(value, HSA_ISA_INFO_NAME, name.data()) == 0);
        assert(std::strcmp(name.data(), "amdgcn-amd-amdhsa--gfx1201") == 0 && name.back() == '!');
        return HSA_STATUS_INFO_BREAK;
    }, &isa) == HSA_STATUS_INFO_BREAK);
    uint16_t dimensions[4] = {0, 0, 0, 0xabcd};
    assert(hsa_isa_get_info_alt(isa, HSA_ISA_INFO_WORKGROUP_MAX_DIM, dimensions) == 0);
    assert(dimensions[0] == 1024 && dimensions[1] == 1024 && dimensions[2] == 1024 && dimensions[3] == 0xabcd);
    assert(hsa_isa_get_info_alt({cpu.handle}, HSA_ISA_INFO_NAME, name) == HSA_STATUS_ERROR_INVALID_ISA);
    mac_hsa_device_info_t info{};
    assert(mac_hsa_agent_get_driver_info(gpu, &info, sizeof(info) - 1) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(mac_hsa_agent_get_driver_info(cpu, &info, sizeof(info)) == HSA_STATUS_ERROR_INVALID_AGENT);
    assert(mac_hsa_agent_get_driver_info(gpu, &info, sizeof(info)) == HSA_STATUS_SUCCESS);
    assert(info.driver_build == 173 && info.total_vram_bytes == 32ull << 30 && info.gfx_revision == 1);
    // Live transport failures propagate instead of returning cached success.
    readStatus = HSA_STATUS_ERROR_INVALID_AGENT;
    assert(hsa_agent_get_info(gpu, HSA_AGENT_INFO_NAME, name) == readStatus);
    assert(mac_hsa_agent_get_driver_info(gpu, &info, sizeof(info)) == readStatus);
    readStatus = HSA_STATUS_SUCCESS;
    hsa_queue_t *queue = reinterpret_cast<hsa_queue_t *>(1);
    assert(hsa_queue_create(gpu, 64, HSA_QUEUE_TYPE_MULTI, nullptr, nullptr, 0, 0, &queue)
           == HSA_STATUS_ERROR_INVALID_QUEUE_CREATION && !queue);
    assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &timestamp) == HSA_STATUS_SUCCESS);
    uint64_t later = 0;
    assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &later) == HSA_STATUS_SUCCESS && later >= timestamp);
    uint8_t extensions[128]; std::memset(extensions, 255, sizeof(extensions));
    assert(hsa_system_get_info(HSA_SYSTEM_INFO_EXTENSIONS, extensions) == HSA_STATUS_SUCCESS);
    assert(extensions[HSA_EXTENSION_AMD_LOADER / 8] == (1u << (HSA_EXTENSION_AMD_LOADER % 8)));
    uint16_t minor = 99; bool supported = true;
    assert(hsa_system_major_extension_supported(HSA_EXTENSION_AMD_LOADER, 1, &minor, &supported)
           == HSA_STATUS_SUCCESS && minor == 3 && supported);
    hsa_ven_amd_loader_1_03_pfn_t loader{};
    assert(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 1, sizeof(loader), &loader) == 0);
    assert(loader.hsa_ven_amd_loader_query_host_address && loader.hsa_ven_amd_loader_iterate_executables);
    assert(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 2, sizeof(loader), &loader)
           == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    // Parallel reference users cannot prematurely close the observer client.
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([gpu] {
        for (unsigned j = 0; j < 200; ++j) {
            assert(hsa_init() == HSA_STATUS_SUCCESS);
            hsa_device_type_t type;
            assert(hsa_agent_get_info(gpu, HSA_AGENT_INFO_DEVICE, &type) == HSA_STATUS_SUCCESS);
            assert(type == HSA_DEVICE_TYPE_GPU);
            assert(hsa_shut_down() == HSA_STATUS_SUCCESS);
        }
    });
    for (auto &thread : workers) thread.join();
    assert(discovered == 2 && closed == 1);
    assert(hsa_shut_down() == HSA_STATUS_SUCCESS && closed == 1);
    assert(hsa_shut_down() == HSA_STATUS_SUCCESS && closed == 2);
    assert(hsa_init() == HSA_STATUS_SUCCESS);
    assert(hsa_agent_get_info(gpu, HSA_AGENT_INFO_NAME, name) == HSA_STATUS_ERROR_INVALID_AGENT);
    assert(hsa_isa_get_info_alt(isa, HSA_ISA_INFO_NAME, name) == HSA_STATUS_ERROR_INVALID_ISA);
    assert(hsa_shut_down() == HSA_STATUS_SUCCESS);
    present = false; observed.clear();
    assert(hsa_init() == HSA_STATUS_SUCCESS);
    assert(hsa_iterate_agents(collect, nullptr) == HSA_STATUS_SUCCESS && observed.size() == 1);
    assert(hsa_shut_down() == HSA_STATUS_SUCCESS);
    puts("HSA: lifecycle, live discovery, callbacks, stale handles, failure cleanup, dispatch rejection and concurrent references pass");
}
