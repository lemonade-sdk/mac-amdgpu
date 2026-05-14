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
}

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
#endif
    uint64_t  eop_bus;  void *eop_cpu;
    uint64_t  mqd_bus;  void *mqd_cpu;
    uint64_t  ring_bus; void *ring_cpu;
    uint64_t  cmd_bus;  void *cmd_cpu;

    // Stashed from the MES firmware header (mes_uc_start_addr_lo/hi)
    // when LoadFirmware processes the matching IP fw type. Set this
    // before mes_enable() — mes_enable programs CP_MES_PRGRM_CNTR_START
    // with `uc_start_addr >> 2`.
    uint64_t  uc_start_addr;
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

// MESInit stage entry — alloc storage for every pipe we plan to
// drive, then (if microcode is already loaded) call mes_enable.
struct PSPContext;
kern_return_t mes_init_full(DeviceContext &dev, PSPContext &psp,
                            GMCContext &gmc, MESContext &mes);

} // namespace amdgpu
