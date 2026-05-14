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
//  This first chunk just gets storage + enable shaped:
//      • EOP / MQD / ring / cmd buffers per MES pipe (sysmem GTT).
//      • CP_MES_CNTL transitions for enable/disable.
//      • Hook for LoadFirmware to stash mes_uc_start_addr from the
//        firmware header so mes_enable() can program PRGRM_CNTR_START.
//
//  Deferred to later chunks:
//      • mes_v12_1_queue_init — HQD program for the MES scheduler
//        ring (GRBM_GFX_CNTL select + CP_HQD_* writes).
//      • mes_v12_1_set_hw_resources — the SET_HW_RESOURCES API
//        message that tells MES which VMIDs/HQDs/doorbells it owns.
//      • mes_v12_1_add_hw_queue + the rest of the API surface.
//
//  Uni-MES note: on RDNA4 we run with enable_uni_mes = true so a
//  single firmware blob (`gc_12_0_1_uni_mes.bin`) drives both the
//  scheduler and KIQ duties; only pipe 0 (SCHED) is programmed.
//

#pragma once

#include <stdint.h>

#ifdef __APPLE__
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#endif

#include "amdgpu_ip.h"
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
namespace MESRegs {
    constexpr uint32_t GRBM_GFX_CNTL              = 0x0900;
    constexpr uint32_t CP_MES_PRGRM_CNTR_START    = 0x2800;
    constexpr uint32_t CP_MES_INTR_ROUTINE_START  = 0x2801;
    constexpr uint32_t CP_MES_CNTL                = 0x2807;
    constexpr uint32_t CP_MES_PIPE0_PRIORITY      = 0x2809;
    constexpr uint32_t CP_MES_INSTR_PNTR          = 0x2813;
    constexpr uint32_t CP_MES_MSCRATCH_HI         = 0x2814;
    constexpr uint32_t CP_MES_MSCRATCH_LO         = 0x2815;
    constexpr uint32_t CP_MES_PRGRM_CNTR_START_HI = 0x289d;
    constexpr uint32_t CP_MES_IC_BASE_LO          = 0x5850;
    constexpr uint32_t CP_MES_IC_BASE_HI          = 0x5851;
    constexpr uint32_t CP_MES_DC_BASE_LO          = 0x5854;
    constexpr uint32_t CP_MES_DC_BASE_HI          = 0x5855;

    // HQD registers (for mes_queue_init).
    constexpr uint32_t CP_MQD_BASE_ADDR             = 0x1fa9;
    constexpr uint32_t CP_MQD_BASE_ADDR_HI          = 0x1faa;
    constexpr uint32_t CP_HQD_ACTIVE                = 0x1fab;
    constexpr uint32_t CP_HQD_VMID                  = 0x1fac;
    constexpr uint32_t CP_HQD_PERSISTENT_STATE      = 0x1fad;
    constexpr uint32_t CP_HQD_PQ_BASE               = 0x1fb1;
    constexpr uint32_t CP_HQD_PQ_BASE_HI            = 0x1fb2;
    constexpr uint32_t CP_HQD_PQ_RPTR_REPORT_ADDR   = 0x1fb4;
    constexpr uint32_t CP_HQD_PQ_RPTR_REPORT_ADDR_HI= 0x1fb5;
    constexpr uint32_t CP_HQD_PQ_WPTR_POLL_ADDR     = 0x1fb6;
    constexpr uint32_t CP_HQD_PQ_WPTR_POLL_ADDR_HI  = 0x1fb7;
    constexpr uint32_t CP_HQD_PQ_DOORBELL_CONTROL   = 0x1fb8;
    constexpr uint32_t CP_HQD_PQ_CONTROL            = 0x1fba;
    constexpr uint32_t CP_HQD_EOP_BASE_ADDR         = 0x1fce;
    constexpr uint32_t CP_HQD_EOP_BASE_ADDR_HI      = 0x1fcf;
    constexpr uint32_t CP_HQD_EOP_CONTROL           = 0x1fd0;
    constexpr uint32_t CP_HQD_PQ_WPTR_LO            = 0x1fdf;
    constexpr uint32_t CP_HQD_PQ_WPTR_HI            = 0x1fe0;
    constexpr uint32_t CP_MQD_CONTROL               = 0x1fcb;
}

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
#define GRBM_GFX_CNTL__PIPEID__SHIFT  0x0
#define GRBM_GFX_CNTL__PIPEID_MASK    0x00000003
#define GRBM_GFX_CNTL__MEID__SHIFT    0x2
#define GRBM_GFX_CNTL__MEID_MASK      0x0000000C
#define GRBM_GFX_CNTL__VMID__SHIFT    0x4
#define GRBM_GFX_CNTL__VMID_MASK      0x000000F0
#define GRBM_GFX_CNTL__QUEUEID__SHIFT 0x8
#define GRBM_GFX_CNTL__QUEUEID_MASK   0x00000700

//------------------------------------------------------------------
// Per-pipe state.
//------------------------------------------------------------------
struct MESInstance {
    bool inited;
    bool enabled;

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
    uint32_t  doorbell_index;         // BAR5 doorbell slot for this ring
};

struct MESContext {
    MESInstance pipe[kMaxMESPipes];
    bool        sched_ucode_loaded;   // RS64_MES (76) seen via LoadFirmware
    bool        kiq_ucode_loaded;     // RS64_KIQ  (78) seen — N/A for uni
    bool        uni_mes_active;
};

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
    if (!dev.ip.isResolved(IPBlock::GC)) return;
    uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::GC, MESRegs::GRBM_GFX_CNTL);
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
kern_return_t mes_alloc_storage(DeviceContext &dev, MESInstance &inst);

// CP_MES_CNTL enable/disable for uni_mes pipe 0. Writes
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
constexpr uint32_t kMES_API_FRAME_DWORDS = 64;          // every msg is 64 dw
constexpr uint32_t kMES_API_TYPE_SCHEDULER = 1;

// MES_SCH_API_OPCODE subset (the ones we care about for first PM4):
namespace MESSchOp {
    constexpr uint32_t SET_HW_RSRC               = 0;
    constexpr uint32_t SET_SCHEDULING_CONFIG     = 1;
    constexpr uint32_t ADD_QUEUE                 = 2;
    constexpr uint32_t REMOVE_QUEUE              = 3;
    constexpr uint32_t QUERY_SCHEDULER_STATUS    = 11;
    constexpr uint32_t SET_HW_RSRC_1             = 19;
}

// MES_API_HEADER bit layout — type[3:0], opcode[11:4], dwsize[19:12],
// reserved[31:20].
static inline uint32_t
mes_api_header(uint32_t type, uint32_t opcode, uint32_t dwsize)
{
    return (type   & 0xFu)
         | ((opcode & 0xFFu) << 4)
         | ((dwsize & 0xFFu) << 12);
}

//------------------------------------------------------------------
// Per-call API status footprint that lives inside every MES message.
// The dext sets `fence_addr` to a 64-bit GPU-side WB slot and
// `fence_value` to 1; MES writes that value into the slot once the
// API completes. (Failure encoding lives in the high 32 bits — see
// upstream comment in mes_v12_api_def.h.)
//------------------------------------------------------------------
struct MES_API_Status {
    uint64_t fence_addr;
    uint64_t fence_value;
};

//------------------------------------------------------------------
// mes_submit_pkt — port of mes_v12_0_submit_pkt_and_poll_completion.
//
// `pkt` must point to a buffer of exactly kMES_API_FRAME_DWORDS
// dwords. `api_status_off_dw` is the dword offset *inside* `pkt`
// where the embedded MES_API_Status starts. We patch fence_addr +
// fence_value into that slot, write the whole frame into the
// scheduler ring, append a QUERY_SCHEDULER_STATUS that chains a
// second fence on the ring's own fence area, kick the ring's
// doorbell, then poll the status slot.
//
// Returns kIOReturnSuccess if the status slot latches lower-32 == 1
// (the MES success indicator) within timeout_us.
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

// Program the MES SCHED pipe's HQD registers. Mirrors upstream's
// mes_v12_0_queue_init_register + the field defaults from
// mes_v12_0_mqd_init. Caller must have run mes_alloc_storage on
// the matching pipe. Does NOT depend on the MES microcode being
// loaded — the writes happen via GRBM_GFX_CNTL select to the MES
// pipe and program the queue state for when MES is later activated.
//
// We also stash the same values into the MQD memory at the
// upstream v12_compute_mqd byte offsets so that MES, once
// running, sees a consistent picture if it re-loads context.
kern_return_t mes_queue_init(const DeviceContext &dev,
                             MESContext &mes, MESPipe pipe);

// MESInit stage entry — alloc storage for every pipe we plan to
// drive, then (if microcode is already loaded) call mes_enable.
struct PSPContext;
kern_return_t mes_init_full(DeviceContext &dev, PSPContext &psp,
                            GMCContext &gmc, MESContext &mes);

} // namespace amdgpu
