#include <hsa/hsa_ext_amd.h>
#include <cassert>
#include <cstdio>
#include <vector>

using iree_status_t = hsa_status_t;
struct iree_hal_amdgpu_libhsa_t {};
struct iree_hal_amdgpu_access_agent_list_t {uint32_t count;hsa_agent_t values[16];};
#define IREE_HAL_AMDGPU_MACOS_COARSE_HOST_ADAPTER 1
#define IREE_ASSERT_ARGUMENT(value) assert(value)
#define IREE_LIBHSA(value) value
#define IREE_RETURN_IF_ERROR(expression) do {const auto status=(expression);if(status)return status;} while(false)
#define IREE_STATUS_INVALID_ARGUMENT HSA_STATUS_ERROR_INVALID_ARGUMENT
static iree_status_t iree_make_status(iree_status_t status,const char *) {return status;}
static hsa_amd_pointer_info_t pointerInfo{};
static hsa_status_t pointerStatus=HSA_STATUS_SUCCESS,accessStatus=HSA_STATUS_SUCCESS;
static std::vector<uint64_t> grants;
static unsigned allowCalls=0;
static hsa_status_t iree_hsa_amd_pointer_info(const iree_hal_amdgpu_libhsa_t *,const void *,
    hsa_amd_pointer_info_t *out,void *(*allocator)(size_t),uint32_t *count,hsa_agent_t **agents) {
    assert(out->size==sizeof(*out) && !allocator && !count && !agents);
    if(pointerStatus)return pointerStatus;
    *out=pointerInfo;return HSA_STATUS_SUCCESS;
}
static hsa_status_t iree_hsa_agent_get_info(const iree_hal_amdgpu_libhsa_t *,hsa_agent_t agent,
    hsa_agent_info_t attribute,void *out) {
    assert(attribute==HSA_AGENT_INFO_DEVICE);
    if(agent.handle<1 || agent.handle>3)return HSA_STATUS_ERROR_INVALID_AGENT;
    *static_cast<hsa_device_type_t *>(out)=agent.handle==1 ? HSA_DEVICE_TYPE_CPU : HSA_DEVICE_TYPE_GPU;
    return HSA_STATUS_SUCCESS;
}
static hsa_status_t iree_hsa_amd_agents_allow_access(const iree_hal_amdgpu_libhsa_t *,uint32_t count,
    const hsa_agent_t *agents,const uint32_t *flags,const void *ptr) {
    assert(count && agents && !flags && ptr);++allowCalls;
    for(uint32_t i=0;i<count;++i)grants.push_back(agents[i].handle);
    return accessStatus;
}

// Compile the actual patched HRX function, not a duplicate implementation.
#include "hrx_access_policy_function.inc"

int main() {
    iree_hal_amdgpu_libhsa_t lib{};
    const iree_hal_amdgpu_access_agent_list_t both{2,{{2},{1}}},peer{3,{{2},{1},{3}}},cpu{1,{{1}}},invalid{2,{{2},{99}}};
    auto run=[&](const auto &list,hsa_status_t expected,std::vector<uint64_t> wanted,unsigned calls=1) {
        grants.clear();allowCalls=0;
        assert(iree_hal_amdgpu_access_allow_agent_list(&lib,&list,reinterpret_cast<void *>(0x10000))==expected);
        assert(grants==wanted && allowCalls==calls);
    };
    pointerInfo.size=sizeof(pointerInfo);pointerInfo.type=HSA_EXT_POINTER_TYPE_HSA;pointerInfo.agentOwner={2};
    // Device-only VRAM skips the CPU topology convenience grant.
    run(both,HSA_STATUS_SUCCESS,{2});
    // A denied peer request remains an error; the adapter cannot discard it.
    accessStatus=HSA_STATUS_ERROR_INVALID_ARGUMENT;
    run(peer,accessStatus,{2,3});
    accessStatus=HSA_STATUS_SUCCESS;
    run(cpu,HSA_STATUS_ERROR_INVALID_ARGUMENT,{},0);
    run(invalid,HSA_STATUS_ERROR_INVALID_AGENT,{},0);
    // A real host mapping must retain CPU access even with a GPU owner.
    pointerInfo.hostBaseAddress=reinterpret_cast<void *>(0x10000);
    run(both,HSA_STATUS_SUCCESS,{2,1});
    // DriverKit shared GTT is CPU-owned and remains accessible to both agents.
    pointerInfo.agentOwner={1};run(both,HSA_STATUS_SUCCESS,{2,1});
    // Unknown/imported pointers and anomalous CPU-only records are not guessed.
    pointerInfo.hostBaseAddress=nullptr;accessStatus=HSA_STATUS_ERROR_INVALID_ALLOCATION;
    run(both,accessStatus,{2,1});
    pointerInfo.type=HSA_EXT_POINTER_TYPE_UNKNOWN;pointerInfo.agentOwner={2};
    run(both,accessStatus,{2,1});
    pointerInfo.type=HSA_EXT_POINTER_TYPE_LOCKED;run(both,accessStatus,{2,1});
    pointerStatus=HSA_STATUS_ERROR_INVALID_AGENT;
    run(both,pointerStatus,{},0);
    puts("PASS: actual HRX macOS access adapter preserves host/peer grants, filters only unmapped VRAM CPU grants, and propagates errors.");
}
