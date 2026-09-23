//
//  amdgpu_mes.h — MicroEngine Scheduler (MES) v12_1 for RDNA4 / gfx1201.
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/mes_v12_0.c (gfx1201 reuses these
//          ops via mes_v12_1 alias)
//      drivers/gpu/drm/amd/amdgpu/amdgpu_mes.c
//      drivers/gpu/drm/amd/include/mes_v12_api_def.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h
//
//  Uni-MES runs the shared firmware image on SCHED pipe 0 and KIQ pipe 1.
//  Bootstrap KIQ through registers, configure its private resources, then
//  ask KIQ to map SCHED from its MQD before configuring the scheduler.
//

#pragma once

#include <stdint.h>

#ifdef __APPLE__
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#endif

#include "amdgpu_ip.h"
#include "amdgpu_mes_packets.h"
#include "amdgpu_mes_registers.h"
#include "amdgpu_regs.h"

namespace amdgpu {

struct GMCContext;  // forward

constexpr uint32_t kMaxMESPipes        = 2;     // SCHED + KIQ
constexpr uint32_t kMES_EOP_SIZE       = 2048;  // upstream MES_EOP_SIZE
constexpr uint32_t kMES_MQD_SIZE       = 4096;  // v12_compute_mqd + slop
constexpr uint32_t kMES_RING_SIZE      = 64 * 1024;
constexpr uint32_t kMES_CMD_BUF_SIZE   = 16 * 1024;
constexpr bool     kEnableUniMES       = true;  // RDNA4 default

enum class MESPipe : uint32_t {
    Sched = 0,
    KIQ   = 1,
};

//------------------------------------------------------------------
// MES register table — GC IP-block offsets from gc_12_0_0_offset.h.
//------------------------------------------------------------------


// CP_UNMAPPED_DOORBELL fields per gc_12_0_0_sh_mask.h:14084-14093.
#define CP_UNMAPPED_DOORBELL__ENABLE__SHIFT          0x0
#define CP_UNMAPPED_DOORBELL__ENABLE_MASK            0x00000001
#define CP_UNMAPPED_DOORBELL__PROC_LSB__SHIFT        0x8
#define CP_UNMAPPED_DOORBELL__PROC_LSB_MASK          0x00001F00

// CP_MES_DOORBELL_CONTROL1..5 share the same field layout —
// DOORBELL_OFFSET[27:2], DOORBELL_EN[30], DOORBELL_HIT[31].
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET__SHIFT 0x2
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET_MASK   0x0FFFFFFC
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_EN__SHIFT     0x1e
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_EN_MASK       0x40000000
#define CP_MES_DOORBELL_CONTROL1__DOORBELL_HIT_MASK      0x80000000

// CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN — required to route doorbell
// updated messages to MES for gfx queues.
#define CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN__SHIFT     0xf
#define CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN_MASK       0x00008000

//
// HQD register field shift/mask defs and defaults — gfx12.
//
#define CP_HQD_PQ_CONTROL__QUEUE_SIZE__SHIFT       0x0
#define CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK         0x0000003F
#define CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE__SHIFT  0x8
#define CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE_MASK    0x00003F00
#define CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR__SHIFT   0x1b
#define CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR_MASK     0x08000000
#define CP_HQD_PQ_CONTROL__UNORD_DISPATCH__SHIFT   0x1c
#define CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK     0x10000000
#define CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH__SHIFT  0x1d
#define CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH_MASK    0x20000000
#define CP_HQD_PQ_CONTROL__PRIV_STATE__SHIFT       0x1e
#define CP_HQD_PQ_CONTROL__PRIV_STATE_MASK         0x40000000
#define CP_HQD_PQ_CONTROL__KMD_QUEUE__SHIFT        0x1f
#define CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK          0x80000000

#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT  0x2
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK    0x0FFFFFFC
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN__SHIFT      0x1e
#define CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK        0x40000000

#define CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT 0x8
#define CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK   0x0003FF00

#define CP_HQD_VMID__VMID__SHIFT  0x0
#define CP_HQD_VMID__VMID_MASK    0x0000000F

#define CP_MQD_CONTROL__VMID__SHIFT  0x0
#define CP_MQD_CONTROL__VMID_MASK    0x0000000F

// Documented defaults from gfx_v12_0.c.
constexpr uint32_t kCP_HQD_PQ_CONTROL_DEFAULT      = 0x00308509u;
constexpr uint32_t kCP_HQD_PERSISTENT_STATE_DEFAULT= 0x0be05501u;
constexpr uint32_t kCP_MQD_CONTROL_DEFAULT         = 0x00000100u;
constexpr uint32_t kCP_HQD_EOP_CONTROL_DEFAULT     = 0x00000006u;

// CP_MES_CNTL — fields from gc_12_0_0_sh_mask.h
#define CP_MES_CNTL__MES_INVALIDATE_ICACHE__SHIFT 0x4
#define CP_MES_CNTL__MES_INVALIDATE_ICACHE_MASK   0x00000010
#define CP_MES_CNTL__MES_PIPE0_RESET__SHIFT       0x10
#define CP_MES_CNTL__MES_PIPE0_RESET_MASK         0x00010000
#define CP_MES_CNTL__MES_PIPE1_RESET__SHIFT       0x11
#define CP_MES_CNTL__MES_PIPE1_RESET_MASK         0x00020000
#define CP_MES_CNTL__MES_PIPE0_ACTIVE__SHIFT      0x1a
#define CP_MES_CNTL__MES_PIPE0_ACTIVE_MASK        0x04000000
#define CP_MES_CNTL__MES_PIPE1_ACTIVE__SHIFT      0x1b
#define CP_MES_CNTL__MES_PIPE1_ACTIVE_MASK        0x08000000
#define CP_MES_CNTL__MES_HALT__SHIFT              0x1e
#define CP_MES_CNTL__MES_HALT_MASK                0x40000000

// GRBM_GFX_CNTL — selects (ME, PIPE, QUEUE, VMID) for HQD writes.
// Also defined in amdgpu_gfx.h; guard so headers can be included in
// either order.
#ifndef GRBM_GFX_CNTL__PIPEID__SHIFT
#define GRBM_GFX_CNTL__PIPEID__SHIFT  0x0
#define GRBM_GFX_CNTL__PIPEID_MASK    0x00000003
#define GRBM_GFX_CNTL__MEID__SHIFT    0x2
#define GRBM_GFX_CNTL__MEID_MASK      0x0000000C
#define GRBM_GFX_CNTL__VMID__SHIFT    0x4
#define GRBM_GFX_CNTL__VMID_MASK      0x000000F0
#define GRBM_GFX_CNTL__QUEUEID__SHIFT 0x8
#define GRBM_GFX_CNTL__QUEUEID_MASK   0x00000700
#endif

//------------------------------------------------------------------
// Per-pipe state.
//------------------------------------------------------------------
struct MESInstance {
    bool inited;
    bool enabled;
    bool submission_pending; // retained after timeout until session reset
    uint64_t vram_base;       // MC base used to derive BAR0 offsets
    uint64_t published_wptr;  // last pointer exposed to the GPU

#ifdef __APPLE__
    IOBufferMemoryDescriptor *eop_buf;
    IODMACommand             *eop_dma;
    IOBufferMemoryDescriptor *mqd_buf;
    IODMACommand             *mqd_dma;
    IOBufferMemoryDescriptor *ring_buf;
    IODMACommand             *ring_dma;
    IOBufferMemoryDescriptor *cmd_buf;
    IODMACommand             *cmd_dma;
    IOBufferMemoryDescriptor *wb_buf;
    IODMACommand             *wb_dma;
#endif
    // *_bus are VRAM GPU addresses; *_cpu are CPU staging copies only.
    uint64_t  eop_bus;  void *eop_cpu;
    uint64_t  mqd_bus;  void *mqd_cpu;
    uint64_t  ring_bus; void *ring_cpu;
    uint64_t  cmd_bus;  void *cmd_cpu;
    uint64_t  wb_bus;   void *wb_cpu;   // rptr/wptr shadow page

    // Stashed from the MES firmware header (mes_uc_start_addr_lo/hi)
    // when LoadFirmware processes the matching IP fw type. Set this
    // before mes_enable() — mes_enable programs CP_MES_PRGRM_CNTR_START
    // with `uc_start_addr >> 2`.
    uint64_t  uc_start_addr;

    // Per-pipe ring write-back state derived during queue_init. The
    // GPU writes the ring's read-pointer + (optional) write-pointer
    // poll shadow into wb_bus + wb_rptr_offset / wb_wptr_offset.
    uint64_t  ring_rptr_gpu_addr;     // wb base + 0
    uint64_t  ring_wptr_gpu_addr;     // wb base + 0x40
    uint32_t  ring_size_dwords;
    uint32_t  doorbell_index;         // BAR2 dword doorbell index for this ring
    // SET_HW_RESOURCES_1 cleaner-shader fence buffer. Allocated lazily
    // by mes_set_hw_resources_1.  Matches mes->resource_1_gpu_addr in
    // upstream (mes_v12_0.c:721).
#ifdef __APPLE__
    IOBufferMemoryDescriptor *resource_1_buf;
    IODMACommand             *resource_1_dma;
#endif
    uint64_t  resource_1_bus;

    // Lazy-allocated by mes_set_hw_resources on first call. Lifetime
    // is tied to the MESContext (i.e. the driver instance).
#ifdef __APPLE__
    IOBufferMemoryDescriptor *sch_ctx_buf;
    IODMACommand             *sch_ctx_dma;
    IOBufferMemoryDescriptor *status_fence_buf;
    IODMACommand             *status_fence_dma;
#endif
    uint64_t  sch_ctx_bus;
    uint64_t  status_fence_bus;
};

struct MESContext {
    MESInstance pipe[kMaxMESPipes];
    bool        sched_ucode_loaded;   // MES code + data acknowledged by PSP
    bool        kiq_ucode_loaded;     // KIQ code + data acknowledged by PSP
    bool        uni_mes_active;

    // MES scheduler firmware version. Read from CP_MES_GP3_LO after
    // mes_v12_0_queue_init by upstream (mes_v12_0.c:1506). Only the
    // low 16 bits matter (AMDGPU_MES_VERSION_MASK = 0xffff). Used to
    // gate SET_HW_RESOURCES_1: upstream sends that frame only when
    // sched_version >= 0x4b (mes_v12_0_hw_init:1859).
    uint32_t  sched_version;
    uint32_t  kiq_version;


};

constexpr uint32_t kMES_VERSION_MASK     = 0x0000FFFFu;
constexpr uint32_t kMES_Resource1Bytes   = 4096;
constexpr uint32_t kMES_HwResources1MinSchedVersion = 0x4b;

//------------------------------------------------------------------
// Helper — GRBM_GFX_CNTL select. Mirrors soc21_grbm_select(adev,
// me, pipe, queue, vmid). me=3 picks MES; me=0/pipe=0/queue=0/vmid=0
// is the "deselect" state used after the GRBM-protected writes are
// done.
//------------------------------------------------------------------
static inline void
grbm_select(const DeviceContext &dev, uint32_t me, uint32_t pipe,
            uint32_t queue, uint32_t vmid)
{
    if (!dev.ip.isResolved(IPBlock::GC, 1)) return;
    uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
        MESRegs::GRBM_GFX_CNTL.baseIndex, MESRegs::GRBM_GFX_CNTL.offset);
    uint32_t v = 0;
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID,  pipe);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, MEID,    me);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, QUEUEID, queue);
    v = REG_SET_FIELD(v, GRBM_GFX_CNTL, VMID,    vmid);
    WREG32(dev, reg, v);
}

//------------------------------------------------------------------
// API
//------------------------------------------------------------------

// Allocate EOP/MQD/ring/cmd_buf for one pipe. Idempotent.
kern_return_t mes_alloc_storage(DeviceContext &dev, MESInstance &inst, GMCContext &gmc, MESPipe pipe);

// CP_MES_CNTL enable/disable for both uni-MES pipes. Writes
// PRGRM_CNTR_START/_HI from inst.uc_start_addr (caller must have
// set this before calling, else mes_enable returns NotReady).
kern_return_t mes_enable(const DeviceContext &dev,
                         MESContext &mes, bool enable);

// Stash the uc_start_addr that was just parsed out of the MES
// firmware header. Called from LoadFirmware after a successful
// psp_load_ip_fw for RS64_MES / RS64_KIQ / uni_mes.
//
// `pipe` selects which MESInstance to set on. `uc_start_addr` is
// the absolute value from `mes_uc_start_addr_lo | (hi << 32)`
// (NOT pre-shifted; mes_enable does the >> 2 itself).
kern_return_t mes_set_uc_start_addr(MESContext &mes, MESPipe pipe,
                                    uint64_t uc_start_addr);

//------------------------------------------------------------------
// MES API wire format — abridged subset of mes_v12_api_def.h
//------------------------------------------------------------------
//------------------------------------------------------------------
// mes_submit_pkt — port of mes_v12_0_submit_pkt_and_poll_completion.
//
// `pkt` must point to a buffer of exactly kMES_API_FRAME_DWORDS
// dwords. `api_status_off_dw` is the dword offset *inside* `pkt`
// where the embedded MES_API_Status starts. We patch fence_addr +
// fence_value into that slot, write the whole frame into the
// scheduler ring, append a QUERY_SCHEDULER_STATUS that chains a
// second fence on the ring's own fence area, kick the ring's
// doorbell, then wait for the query fence before checking API status.
//
// Returns success only after both the query fence and API status report
// completion within a measured timeout_us interval.
//------------------------------------------------------------------
kern_return_t mes_submit_pkt(const DeviceContext &dev, MESContext &mes,
                             MESPipe pipe,
                             const uint32_t *pkt,
                             uint32_t api_status_off_dw,
                             uint64_t timeout_us);

// Convenience wrapper — sends MES_SCH_API_QUERY_SCHEDULER_STATUS to
// the given pipe. Returns kIOReturnSuccess on a successful echo.
kern_return_t mes_query_sched_status(const DeviceContext &dev,
                                     MESContext &mes, MESPipe pipe);

// SET_HW_RESOURCES bus addresses (scheduler context + status fence)
// live alongside the regular MES storage. Allocated lazily by
// mes_set_hw_resources on first call, kept alive thereafter.
constexpr uint32_t kMES_SchCtxBytes      = 4096;
constexpr uint32_t kMES_StatusFenceBytes = 4096;

// Doorbell offsets handed to MES for the 5 priority levels.
// We carve a 5-slot window starting at 0x100 (well clear of the
// CP/SDMA/MES SCHED slots we already use).
constexpr uint32_t kMES_AggregatedDoorbellsBase = 0x100;

//------------------------------------------------------------------
// Wire-format structs for the MES API messages.
//
// Natural alignment matches upstream's Linux/x86_64 layout exactly
// (alignof(uint64_t) == 8 on both platforms), so as long as we
// keep field order + types verbatim from mes_v12_api_def.h the
// resulting byte layout is bit-compatible. The trailing
// `padding[…]` arrays round each frame up to API_FRAME_SIZE_IN_DWORDS.
//------------------------------------------------------------------

constexpr uint32_t kMES_PriorityLevels = 5;  // AMD_PRIORITY_NUM_LEVELS

// SET_HW_RESOURCES flag bit positions (mirrors upstream packed
// bitfield in mes_v12_api_def.h:275-298). Layout (LSB→MSB):
//   bit  0  : disable_reset
//   bit  1  : use_different_vmid_compute
//   bit  2  : disable_mes_log
//   bit  3  : apply_mmhub_pgvm_invalidate_ack_loss_wa
//   bit  4  : apply_grbm_remote_register_dummy_read_wa
//   bit  5  : second_gfx_pipe_enabled
//   bit  6  : enable_level_process_quantum_check
//   bit  7  : legacy_sch_mode
//   bit  8  : disable_add_queue_wptr_mc_addr
//   bit  9  : enable_mes_event_int_logging
//   bit 10  : enable_reg_active_poll
//   bit 11  : use_disable_queue_in_legacy_uq_preemption
//   bit 12  : send_write_data
//   bit 13  : os_tdr_timeout_override
//   bit 14  : use_rs64mem_for_proc_gang_ctx
//   bit 15  : halt_on_misaligned_access
//   bit 16  : use_add_queue_unmap_flag_addr
//   bit 17  : enable_mes_sch_stb_log
//   bit 18  : limit_single_process
//   bit 19..20 : unmapped_doorbell_handling (2 bits) — upstream
//                writes value 1 (basic version) per mes_v12_0.c:792
//   bit 21  : enable_mes_fence_int
//   bit 22  : enable_lr_compute_wa
constexpr uint32_t kSetHwRsrcFlag_disable_reset                      = 1u <<  0;
constexpr uint32_t kSetHwRsrcFlag_use_different_vmid_compute         = 1u <<  1;
constexpr uint32_t kSetHwRsrcFlag_disable_mes_log                    = 1u <<  2;
constexpr uint32_t kSetHwRsrcFlag_enable_level_process_quantum_check = 1u <<  6;
constexpr uint32_t kSetHwRsrcFlag_enable_reg_active_poll             = 1u << 10;
constexpr uint32_t kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC   = 1u << 19;

struct MES_AddQueue {
    MES_Header_Wire header;
    uint32_t process_id;
    uint64_t page_table_base_addr;
    uint64_t process_va_start;
    uint64_t process_va_end;
    uint64_t process_quantum;
    uint64_t process_context_addr;
    uint64_t gang_quantum;
    uint64_t gang_context_addr;
    uint32_t inprocess_gang_priority;
    uint32_t gang_global_priority_level;
    uint32_t doorbell_offset;
    uint32_t _pad0;             // align mqd_addr to 8
    uint64_t mqd_addr;
    uint64_t wptr_addr;
    uint64_t h_context;
    uint64_t h_queue;
    uint32_t queue_type;
    uint32_t gds_base;          // mes_v12_api_def.h:357 — was MISSING.
    uint32_t gds_size;          // union with kfd_queue_size
    uint32_t gws_base;
    uint32_t gws_size;
    uint32_t oa_mask;
    // Natural u64 alignment pads to 144 here. trap_handler_addr at
    // offset 144 (dw 36) — matches upstream's offset (compiler also
    // inserts the same 4-byte pad after `oa_mask` upstream).
    uint64_t trap_handler_addr;
    uint32_t vm_context_cntl;
    uint32_t flags;             // packed bitfield
    // api_status (u64 first field) is naturally aligned — vm_context_cntl
    // + flags = 8 bytes; sum-since-trap_handler_addr u64 = 16 bytes; the
    // running offset is 8-aligned.
    MES_API_Status api_status;
    uint64_t tma_addr;
    uint32_t sch_id;
    uint32_t _pad2;             // align timestamp to 8
    uint64_t timestamp;
    uint32_t process_context_array_index;
    uint32_t gang_context_array_index;
    uint32_t pipe_id;
    uint32_t queue_id;
    uint32_t alignment_mode_setting;
    uint32_t full_sh_mem_config_data;
    // Tail pad rounds to 64 dwords (256 bytes). After full_sh_mem_config_data
    // the running offset is 216; we need 40 more bytes = 10 dwords.
    uint32_t pad[10];
};
static_assert(sizeof(MES_AddQueue) == 64 * 4,
              "MES_AddQueue must be 64 dwords");

// MES_AddQueue flags bitfield encoding (bit position).
// Mirrors mes_v12_api_def.h:369-385 packed bitfield order:
//   bit  0    : paging
//   bits 1..4 : debug_vmid (4 bits)
//   bit  5    : program_gds
//   bit  6    : is_gang_suspended
//   bit  7    : is_tmz_queue
//   bit  8    : map_kiq_utility_queue
//   bit  9    : is_kfd_process
//   bit 10    : trap_en
//   bit 11    : is_aql_queue
//   bit 12    : skip_process_ctx_clear
//   bit 13    : map_legacy_kq
//   bit 14    : exclusively_scheduled
//   bit 15    : is_long_running
//   bit 16    : is_dwm_queue
constexpr uint32_t kAddQueueFlag_paging                = 1u <<  0;
constexpr uint32_t kAddQueueFlag_program_gds           = 1u <<  5;
constexpr uint32_t kAddQueueFlag_is_gang_suspended     = 1u <<  6;
constexpr uint32_t kAddQueueFlag_is_tmz_queue          = 1u <<  7;
constexpr uint32_t kAddQueueFlag_map_kiq_utility_queue = 1u <<  8;
constexpr uint32_t kAddQueueFlag_is_kfd_process        = 1u <<  9;
constexpr uint32_t kAddQueueFlag_trap_en               = 1u << 10;
constexpr uint32_t kAddQueueFlag_is_aql_queue          = 1u << 11;
constexpr uint32_t kAddQueueFlag_skip_process_ctx_clear= 1u << 12;
constexpr uint32_t kAddQueueFlag_map_legacy_kq         = 1u << 13;
constexpr uint32_t kAddQueueFlag_exclusively_scheduled = 1u << 14;
constexpr uint32_t kAddQueueFlag_is_long_running       = 1u << 15;
constexpr uint32_t kAddQueueFlag_is_dwm_queue          = 1u << 16;

// MES queue types (mirrors enum MES_QUEUE_TYPE).
constexpr uint32_t kMESQueueType_GFX     = 0;
constexpr uint32_t kMESQueueType_COMPUTE = 1;
constexpr uint32_t kMESQueueType_SDMA    = 2;
constexpr uint32_t kMESQueueType_SCHQ    = 3;

//------------------------------------------------------------------
// Inputs for the high-level helpers.
//------------------------------------------------------------------
struct MESSetHwResourcesInput {
    uint32_t vmid_mask_mmhub;
    uint32_t vmid_mask_gfxhub;
    uint32_t compute_hqd_mask[8];
    uint32_t gfx_hqd_mask[2];
    uint32_t sdma_hqd_mask[2];
    uint32_t aggregated_doorbells[kMES_PriorityLevels];
    // gc_base / mmhub_base / osssys_base copied from DeviceContext.
};

struct MESAddQueueInput {
    uint32_t process_id;
    uint64_t mqd_addr;
    uint64_t wptr_addr;          // GPU bus address of wptr shadow
    uint32_t doorbell_offset;
    uint32_t queue_type;         // kMESQueueType_*
    uint32_t pipe_id;
    uint32_t queue_id;
    uint32_t inprocess_gang_priority;       // 0..4 (AMD_PRIORITY_LEVEL_*)
    uint32_t gang_global_priority_level;    // 0..4
    uint64_t gang_context_addr;             // 4 KB sysmem allocated by user
    uint64_t process_context_addr;          // 4 KB sysmem allocated by user
    uint64_t page_table_base_addr;          // GART PD base — VMID 0 path
    uint32_t flags;                         // bitwise-OR of kAddQueueFlag_*
};

// Build + submit SET_HW_RESOURCES with private storage for the selected pipe.
// Lazy-allocates the scheduler context + query-status fence buffers
// on first call and stashes them on MESInstance.
kern_return_t mes_set_hw_resources(DeviceContext &dev, MESContext &mes, GMCContext &gmc,
                                   const MESSetHwResourcesInput &in, MESPipe pipe);

// Map a kernel GFX/compute/scheduler queue through the configured uni-MES KIQ.
// Backing must remain alive until reset or a completed unmap.
kern_return_t mes_map_legacy_queue(const DeviceContext &dev, MESContext &mes,
    uint32_t queueType, uint32_t pipe, uint32_t queue, uint32_t doorbell,
    uint64_t mqdAddress, uint64_t wptrAddress);

// Success means the REMOVE_QUEUE API and trailing scheduler query completed.
// On failure the caller must retain all queue backing until a verified reset.
kern_return_t mes_unmap_legacy_queue(const DeviceContext &dev, MESContext &mes,
    uint32_t queueType, uint32_t pipe, uint32_t queue, uint32_t doorbell);

// Build + submit an ADD_QUEUE message. Pipes always SCHED.
kern_return_t mes_add_hw_queue(const DeviceContext &dev, MESContext &mes,
                               const MESAddQueueInput &in);

// Program CP_MES_DOORBELL_CONTROL{1..5} with the 5 aggregated
// doorbell offsets + CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN.
// Doorbell offsets are bytes (already shifted via OFFSET field write).
kern_return_t mes_init_aggregated_doorbell(const DeviceContext &dev,
                                           const uint32_t doorbells[5]);

// Port of mes_v12_0_kiq_setting (mes_v12_0.c:1728). Writes RLC_CP_SCHEDULERS
// with the (me, pipe, queue) encoding for the KIQ ring + scheduler-active
// bit so RLC routes IRQs to the correct queue.
kern_return_t mes_kiq_setting(const DeviceContext &dev,
                              uint32_t me, uint32_t pipe, uint32_t queue);

// Port of mes_v12_0_enable_unmapped_doorbell_handling (mes_v12_0.c:863).
// Sets CP_UNMAPPED_DOORBELL.PROC_LSB = 0xd + ENABLE = 1 so the CP
// forwards doorbell writes to queues MES hasn't yet mapped.
kern_return_t mes_enable_unmapped_doorbell_handling(const DeviceContext &dev,
                                                    bool enable);

// Port of mes_v12_0_set_hw_resources_1 (mes_v12_0.c:711). Sent
// AFTER set_hw_resources, gated on sched_version >= 0x4b. Lazily
// allocates the cleaner_shader_fence buffer.
kern_return_t mes_set_hw_resources_1(DeviceContext &dev, MESContext &mes, GMCContext &gmc, MESPipe pipe);

// Read MES scheduler version from CP_MES_GP3_LO after mes_enable
// (mes_v12_0.c:1505). Stashed on mes.sched_version.
kern_return_t mes_read_sched_version(const DeviceContext &dev,
                                     MESContext &mes, MESPipe pipe);

// Build and verify the v12_compute_mqd in VRAM. Bootstrap the KIQ HQD
// through registers; map SCHED through the running, configured KIQ.
// Both pipes must already be enabled, with storage allocated.
kern_return_t mes_queue_init(const DeviceContext &dev,
                             MESContext &mes, MESPipe pipe);

// MESInit stage entry — enable both pipes, configure KIQ, map SCHED,
// then require scheduler resource/query acknowledgements.
struct PSPContext;
kern_return_t mes_init_full(DeviceContext &dev, PSPContext &psp,
                            GMCContext &gmc, MESContext &mes);

} // namespace amdgpu
