//
//  amdgpu_sdma.h — System DMA engine v7_0 (RDNA4 / gfx1201).
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h
//
//  RDNA4 has two SDMA instances, both living inside the GC IP block
//  (per amdgpu_discovery: SDMA0/SDMA1 ⇒ GC_HWIP for SOC15 base
//  resolution). Each instance has three queues (QUEUE0..2); we only
//  drive QUEUE0 for the initial bringup.
//
//  Register addressing (sdma_v7_0_get_reg_offset):
//      • Most registers: base = GC base[0], add SDMA1_REG_OFFSET=0x600
//        for instance 1.
//      • "Hyp dec" range 0x5880..0x589a (MCU_CNTL etc.) uses
//        GC base[1] with SDMA1_HYP_DEC_REG_OFFSET=0x20 per instance.
//
//  On Apple Silicon the SDMA ring buffers live in DART-mapped sysmem
//  (16 KB-aligned), addressed by GPU through GART once GMC is up.
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

struct GMCContext;  // forward — SDMA shares GART with GFX

constexpr uint32_t kSDMAInstanceCount     = 2;
constexpr uint32_t kSDMARingDefaultBytes  = 16 * 1024;   // 4096 dwords
constexpr uint32_t kSDMAWBPageBytes       = 16 * 1024;   // AS page granular

// SDMA1 sits 0x600 dwords past SDMA0 in the GC[0] base. The hyp-dec
// range uses the GC[1] base and a 0x20 stride per instance.
constexpr uint32_t kSDMA1_REG_OFFSET             = 0x600;
constexpr uint32_t kSDMA1_HYP_DEC_REG_OFFSET     = 0x20;
constexpr uint32_t kSDMA0_HYP_DEC_REG_START      = 0x5880;
constexpr uint32_t kSDMA0_HYP_DEC_REG_END        = 0x589a;

//------------------------------------------------------------------
// SDMA0 register offsets (relative to GC IP base for non-hyp-dec,
// or relative to GC[1] base for hyp-dec). All values are dword
// offsets, taken verbatim from gc_12_0_0_offset.h.
//------------------------------------------------------------------
namespace SDMARegs {
    constexpr uint32_t STATUS_REG                  = 0x0024;
    constexpr uint32_t WATCHDOG_CNTL               = 0x002b;
    constexpr uint32_t UTCL1_CNTL                  = 0x0035;
    constexpr uint32_t UTCL1_PAGE                  = 0x0038;
    constexpr uint32_t QUEUE0_RB_CNTL              = 0x0080;
    constexpr uint32_t QUEUE0_RB_BASE              = 0x0081;
    constexpr uint32_t QUEUE0_RB_BASE_HI           = 0x0082;
    constexpr uint32_t QUEUE0_RB_RPTR              = 0x0083;
    constexpr uint32_t QUEUE0_RB_RPTR_HI           = 0x0084;
    constexpr uint32_t QUEUE0_RB_WPTR              = 0x0085;
    constexpr uint32_t QUEUE0_RB_WPTR_HI           = 0x0086;
    constexpr uint32_t QUEUE0_RB_RPTR_ADDR_LO      = 0x0087;
    constexpr uint32_t QUEUE0_RB_RPTR_ADDR_HI      = 0x0088;
    constexpr uint32_t QUEUE0_IB_CNTL              = 0x0089;
    constexpr uint32_t QUEUE0_DOORBELL             = 0x008f;
    constexpr uint32_t QUEUE0_DOORBELL_OFFSET      = 0x0091;
    constexpr uint32_t QUEUE0_RB_WPTR_POLL_ADDR_LO = 0x0098;
    constexpr uint32_t QUEUE0_RB_WPTR_POLL_ADDR_HI = 0x0099;
    constexpr uint32_t QUEUE0_MINOR_PTR_UPDATE     = 0x009b;
    // Hyp-dec range
    constexpr uint32_t MCU_CNTL                    = 0x588e;
}

//------------------------------------------------------------------
// Field shift / mask defs — minimal subset for ring resume.
// Mirrors upstream gc_12_0_0_sh_mask.h naming so REG_SET_FIELD can
// be used directly.
//------------------------------------------------------------------
#define SDMA0_QUEUE0_RB_CNTL__RB_ENABLE__SHIFT                    0x0
#define SDMA0_QUEUE0_RB_CNTL__RB_ENABLE_MASK                      0x00000001
#define SDMA0_QUEUE0_RB_CNTL__RB_SIZE__SHIFT                      0x1
#define SDMA0_QUEUE0_RB_CNTL__RB_SIZE_MASK                        0x0000003E
#define SDMA0_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE__SHIFT             0x8
#define SDMA0_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE_MASK               0x00000100
#define SDMA0_QUEUE0_RB_CNTL__MCU_WPTR_POLL_ENABLE__SHIFT         0xb
#define SDMA0_QUEUE0_RB_CNTL__MCU_WPTR_POLL_ENABLE_MASK           0x00000800
#define SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT        0xc
#define SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK          0x00001000
#define SDMA0_QUEUE0_RB_CNTL__RB_PRIV__SHIFT                      0x17
#define SDMA0_QUEUE0_RB_CNTL__RB_PRIV_MASK                        0x00800000

#define SDMA0_QUEUE0_IB_CNTL__IB_ENABLE__SHIFT                    0x0
#define SDMA0_QUEUE0_IB_CNTL__IB_ENABLE_MASK                      0x00000001

#define SDMA0_QUEUE0_DOORBELL__ENABLE__SHIFT                      0x1c
#define SDMA0_QUEUE0_DOORBELL__ENABLE_MASK                        0x10000000

#define SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET__SHIFT               0x2
#define SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK                 0x0FFFFFFC

#define SDMA0_MCU_CNTL__HALT__SHIFT                               0x0
#define SDMA0_MCU_CNTL__HALT_MASK                                 0x00000001
#define SDMA0_MCU_CNTL__RESET__SHIFT                              0x1
#define SDMA0_MCU_CNTL__RESET_MASK                                0x00000002

#define SDMA0_UTCL1_CNTL__REDO_DELAY__SHIFT                       0x0
#define SDMA0_UTCL1_CNTL__REDO_DELAY_MASK                         0x0000001F
#define SDMA0_UTCL1_CNTL__RESP_MODE__SHIFT                        0x9
#define SDMA0_UTCL1_CNTL__RESP_MODE_MASK                          0x00000600

#define SDMA0_WATCHDOG_CNTL__QUEUE_HANG_COUNT__SHIFT              0x0
#define SDMA0_WATCHDOG_CNTL__QUEUE_HANG_COUNT_MASK                0x000000FF

//------------------------------------------------------------------
// SDMA opcode + helpers — sdma_pkt_open.h subset for NOP / FENCE /
// TRAP. Enough to emit a ring test that the host can wait on.
//------------------------------------------------------------------
constexpr uint32_t SDMA_OP_NOP    = 0;
constexpr uint32_t SDMA_OP_FENCE  = 5;
constexpr uint32_t SDMA_OP_TRAP   = 6;
constexpr uint32_t SDMA_OP_TIMESTAMP = 13;

static inline uint32_t SDMA_PKT_HEADER_OP(uint32_t op) { return (op & 0xff); }

//------------------------------------------------------------------
// Per-instance ring state. Two instances live side by side; we
// drive both with the same layout.
//------------------------------------------------------------------
struct SDMAInstance {
    uint32_t  instance;          // 0 or 1
    bool      inited;
    bool      enabled;

#ifdef __APPLE__
    // Ring buffer — sysmem, 16 KB-aligned, GART-fetched.
    IOBufferMemoryDescriptor *ring_buf;
    IODMACommand             *ring_dma;
    // Write-back page (rptr/fence shadow).
    IOBufferMemoryDescriptor *wb_buf;
    IODMACommand             *wb_dma;
#endif
    uint64_t  ring_bus;
    void     *ring_cpu;
    uint32_t  ring_size_dwords;
    uint32_t  ring_ptr_mask;

    uint64_t  wb_bus;
    void     *wb_cpu;
    uint64_t  rptr_gpu_addr;    // wb_bus + 0
    uint64_t  wptr_poll_gpu_addr; // wb_bus + 0x40
    volatile uint32_t *rptr_cpu;

    uint32_t  wptr;             // software wptr (dword index)
    uint32_t  doorbell_index;   // BAR5 doorbell slot
};

struct SDMAContext {
    SDMAInstance instance[kSDMAInstanceCount];
    bool         microcode_loaded;
};

//------------------------------------------------------------------
// API
//------------------------------------------------------------------

// Compute the absolute BAR0 dword offset for an SDMA register on a
// given instance. Mirrors sdma_v7_0_get_reg_offset(). Hyp-dec lookup
// uses GC[1] base — for now we treat that as identical to GC[0]
// since on R9700 they live in the same SMN window; refine when the
// IP discovery walker grows multi-instance support.
static inline uint32_t
sdma_reg_offset(const DeviceContext &ctx, uint32_t instance, uint32_t reg)
{
    const uint32_t base = ctx.ip.get(IPBlock::GC);
    if (reg >= kSDMA0_HYP_DEC_REG_START && reg <= kSDMA0_HYP_DEC_REG_END) {
        return base + reg + (instance * kSDMA1_HYP_DEC_REG_OFFSET);
    }
    return base + reg + (instance == 1 ? kSDMA1_REG_OFFSET : 0u);
}

// Allocate ring + WB page for one instance. Idempotent.
kern_return_t sdma_alloc_storage(DeviceContext &dev, SDMAInstance &inst);

// Halt/unhalt one engine via SDMA0_MCU_CNTL.HALT.
kern_return_t sdma_engine_halt(const DeviceContext &dev,
                               uint32_t instance, bool halt);

// Stop the GFX queue: clear RB_ENABLE + IB_ENABLE on QUEUE0.
kern_return_t sdma_gfx_stop_instance(const DeviceContext &dev,
                                     uint32_t instance);

// Port of sdma_v7_0_gfx_resume_instance(restore=false).
// Programs QUEUE0 HQD + unhalts the engine + enables the ring.
kern_return_t sdma_gfx_resume_instance(const DeviceContext &dev,
                                       SDMAInstance &inst);

// Kick QUEUE0's doorbell with the current software wptr (byte
// offset, so wptr_dword << 2). Writes to BAR5 dev.bar5MemIndex at
// (doorbell_index * 8).
kern_return_t sdma_kick_doorbell(const DeviceContext &dev,
                                 const SDMAInstance &inst);

// Append dwords to the ring at the current software wptr; wraps.
// Returns the number of dwords actually written (0 on overflow).
uint32_t sdma_ring_write(SDMAInstance &inst,
                         const uint32_t *src, uint32_t dwords);

// End-to-end ring test: emit FENCE + TRAP, poll the fence word.
// Returns kIOReturnSuccess if fence materialised, else the
// last-observed return code.
kern_return_t sdma_ring_test(const DeviceContext &dev,
                             SDMAInstance &inst,
                             uint64_t timeout_us);

// Top-level SDMAInit stage entry. Resolves IP base, asks PSP to
// load the SDMA0/SDMA1 microcode, allocates per-instance storage,
// runs gfx_resume on each. Idempotent.
struct PSPContext;
kern_return_t sdma_init_full(DeviceContext &dev,
                             PSPContext &psp,
                             GMCContext &gmc,
                             SDMAContext &sdma);

} // namespace amdgpu
