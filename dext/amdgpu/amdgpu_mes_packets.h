#pragma once

#include <stddef.h>
#include <stdint.h>

namespace amdgpu {

constexpr uint32_t kMES_API_FRAME_DWORDS = 64;
constexpr uint32_t kMES_API_TYPE_SCHEDULER = 1;

namespace MESSchOp {
    constexpr uint32_t SET_HW_RSRC = 0;
    constexpr uint32_t SET_SCHEDULING_CONFIG = 1;
    constexpr uint32_t ADD_QUEUE = 2;
    constexpr uint32_t REMOVE_QUEUE = 3;
    constexpr uint32_t QUERY_SCHEDULER_STATUS = 11;
    constexpr uint32_t SET_HW_RSRC_1 = 19;
}

constexpr uint32_t mes_api_header(uint32_t type, uint32_t opcode, uint32_t dwsize)
{
    return (type & 0xfu) | ((opcode & 0xffu) << 4) | ((dwsize & 0xffu) << 12);
}

struct MES_API_Status {
    uint64_t fence_addr;
    uint64_t fence_value;
};

struct MES_Header_Wire {
    uint32_t u32All;
};

// Linux mes_v12_api_def.h uses 8-byte packing. Query completion is at
// byte 8, not at the end of the 256-byte API frame.
struct MES_QueryStatus {
    MES_Header_Wire header;
    uint32_t subopcode;
    MES_API_Status api_status;
    uint64_t timestamp;
    uint32_t data[20];
    uint32_t padding[36];
};

struct MES_SetHwResources {
    MES_Header_Wire header;
    uint32_t vmid_mask_mmhub;
    uint32_t vmid_mask_gfxhub;
    uint32_t gds_size;
    uint32_t paging_vmid;
    uint32_t compute_hqd_mask[8];
    uint32_t gfx_hqd_mask[2];
    uint32_t sdma_hqd_mask[2];
    uint32_t aggregated_doorbells[5];
    uint64_t g_sch_ctx_gpu_mc_ptr;
    uint64_t query_status_fence_gpu_mc_ptr;
    uint32_t gc_base[8];
    uint32_t mmhub_base[8];
    uint32_t osssys_base[8];
    MES_API_Status api_status;
    uint32_t flags;
    uint32_t oversubscription_timer;
    uint64_t doorbell_info;
    uint64_t event_intr_history_gpu_mc_ptr;
    uint64_t timestamp;
    uint32_t os_tdr_timeout_in_sec;
    uint32_t pad[1];  // bring total to 64 dw = 256 bytes
};
static_assert(sizeof(MES_SetHwResources) == 64 * 4,
              "MES_SetHwResources must be 64 dwords");

struct MES_SetHwResources1 {
    MES_Header_Wire header;
    MES_API_Status api_status;
    uint64_t timestamp;
    uint32_t flags;
    uint64_t mes_debug_ctx_mc_addr;
    uint32_t mes_debug_ctx_size;
    uint32_t mes_kiq_unmap_timeout;
    uint64_t coop_sch_shared_mc_addr;
    uint64_t cleaner_shader_fence_mc_addr;
    uint32_t padding[46];
};

static_assert(sizeof(MES_QueryStatus) == 256);
static_assert(offsetof(MES_QueryStatus, api_status) == 8);
static_assert(sizeof(MES_SetHwResources1) == 256);
static_assert(offsetof(MES_SetHwResources1, timestamp) == 24);
static_assert(offsetof(MES_SetHwResources1, cleaner_shader_fence_mc_addr) == 64);

} // namespace amdgpu
