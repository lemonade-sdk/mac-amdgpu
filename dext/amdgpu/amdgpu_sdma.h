//
//  amdgpu_sdma.h — System DMA engine v7_1 (RDNA4 R9700 / gfx12.1.0).
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_offset.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h
//
//  RDNA4 (IP_VERSION SDMA(7,0,1)) dispatches to the sdma_v7_1 path —
//  see sdma_v7_1.c:33-34 (includes gc_12_1_0_*). The 7,0,0 codepath
//  in sdma_v7_0.c uses the older gc_12_0_0_* headers; we drive R9700
//  so we must use the 12_1_0 offsets. Audit #6 P0-1: every QUEUE0
//  register on the 12_1_0 layout sits 0x180 past its 12_0_0 location
//  (RB_CNTL 0x80 → 0x200, RB_BASE 0x81 → 0x201, …), and the hyp-dec
//  per-instance stride is 0x30 (not 0x20).
//
//  RDNA4 has two SDMA instances, both living inside the GC IP block
//  (per amdgpu_discovery: SDMA0/SDMA1 ⇒ GC_HWIP for SOC15 base
//  resolution). Each instance has three queues (QUEUE0..2); we only
//  drive QUEUE0 for the initial bringup.
//
//  Register addressing (sdma_v7_1_get_reg_offset @ sdma_v7_1.c:117):
//      • Most registers: base = GC base[0], add SDMA1_REG_OFFSET=0x600
//        for instance 1.
//      • Hyp-dec range (internal_offset >= SDMA0_SDMA_IDX_0_END=0x450)
//        uses GC base[1] with SDMA1_HYP_DEC_REG_OFFSET=0x30 per
//        instance (sdma_v7_1.c:49-51).
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
// range uses the GC[1] base and a 0x30 stride per instance.
// Values per sdma_v7_1.c:49-51.
constexpr uint32_t kSDMA1_REG_OFFSET             = 0x600;
constexpr uint32_t kSDMA1_HYP_DEC_REG_OFFSET     = 0x30;
// The hyp-dec boundary moved on 12_1_0: any internal_offset
// >= SDMA0_SDMA_IDX_0_END (0x450) is routed through GC base[1].
// MCU_CNTL (0x588e), other hyp registers all live above this.
constexpr uint32_t kSDMA0_SDMA_IDX_0_END         = 0x450;

//------------------------------------------------------------------
// SDMA0 register offsets (relative to GC IP base for non-hyp-dec,
// or relative to GC[1] base for hyp-dec). All values are dword
// offsets, taken verbatim from
//   drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_offset.h
// (regSDMA0_SDMA_* names — note the extra "SDMA_" infix vs the
// older 12_0_0 layout).
//------------------------------------------------------------------
namespace SDMARegs {
    // gc_12_1_0_offset.h line numbers cited per register.
    constexpr uint32_t STATUS_REG                  = 0x0024; // line 56  regSDMA0_SDMA_STATUS_REG
    constexpr uint32_t WATCHDOG_CNTL               = 0x002b; // line 70  regSDMA0_SDMA_WATCHDOG_CNTL
    constexpr uint32_t UTCL1_CNTL                  = 0x0037; // line 94  regSDMA0_SDMA_UTCL1_CNTL
    constexpr uint32_t UTCL1_PAGE                  = 0x003a; // line 100 regSDMA0_SDMA_UTCL1_PAGE
    constexpr uint32_t QUEUE0_RB_CNTL              = 0x0200; // line 194 regSDMA0_SDMA_QUEUE0_RB_CNTL
    constexpr uint32_t QUEUE0_RB_BASE              = 0x0201; // line 196 regSDMA0_SDMA_QUEUE0_RB_BASE
    constexpr uint32_t QUEUE0_RB_BASE_HI           = 0x0202; // line 198 regSDMA0_SDMA_QUEUE0_RB_BASE_HI
    constexpr uint32_t QUEUE0_RB_RPTR              = 0x0203; // line 200 regSDMA0_SDMA_QUEUE0_RB_RPTR
    constexpr uint32_t QUEUE0_RB_RPTR_HI           = 0x0204; // line 202 regSDMA0_SDMA_QUEUE0_RB_RPTR_HI
    constexpr uint32_t QUEUE0_RB_WPTR              = 0x0205; // line 204 regSDMA0_SDMA_QUEUE0_RB_WPTR
    constexpr uint32_t QUEUE0_RB_WPTR_HI           = 0x0206; // line 206 regSDMA0_SDMA_QUEUE0_RB_WPTR_HI
    constexpr uint32_t QUEUE0_RB_RPTR_ADDR_LO      = 0x0207; // line 208 regSDMA0_SDMA_QUEUE0_RB_RPTR_ADDR_LO
    constexpr uint32_t QUEUE0_RB_RPTR_ADDR_HI      = 0x0208; // line 210 regSDMA0_SDMA_QUEUE0_RB_RPTR_ADDR_HI
    constexpr uint32_t QUEUE0_IB_CNTL              = 0x0209; // line 212 regSDMA0_SDMA_QUEUE0_IB_CNTL
    constexpr uint32_t QUEUE0_DOORBELL             = 0x020f; // line 224 regSDMA0_SDMA_QUEUE0_DOORBELL
    constexpr uint32_t QUEUE0_DOORBELL_OFFSET      = 0x0211; // line 228 regSDMA0_SDMA_QUEUE0_DOORBELL_OFFSET
    constexpr uint32_t QUEUE0_RB_WPTR_POLL_ADDR_LO = 0x0218; // line 242 regSDMA0_SDMA_QUEUE0_RB_WPTR_POLL_ADDR_LO
    constexpr uint32_t QUEUE0_RB_WPTR_POLL_ADDR_HI = 0x0219; // line 244 regSDMA0_SDMA_QUEUE0_RB_WPTR_POLL_ADDR_HI
    constexpr uint32_t QUEUE0_MINOR_PTR_UPDATE     = 0x021b; // line 248 regSDMA0_SDMA_QUEUE0_MINOR_PTR_UPDATE
    // Hyp-dec range (>= 0x450)
    constexpr uint32_t MCU_CNTL                    = 0x588e; // line 1224 regSDMA0_SDMA_MCU_CNTL
}

//------------------------------------------------------------------
// Field shift / mask defs — minimal subset for ring resume.
// Mirrors upstream gc_12_1_0_sh_mask.h naming so REG_SET_FIELD can
// be used directly. Note the "SDMA0_SDMA_*" infix — the 12_1_0
// layout renamed every SDMA0_ macro to SDMA0_SDMA_ (sdma_v7_1.c
// uses these spellings throughout).
//------------------------------------------------------------------
// gc_12_1_0_sh_mask.h lines cited per shift below.
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_ENABLE__SHIFT                    0x0      // line 985
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_ENABLE_MASK                      0x00000001
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_SIZE__SHIFT                      0x1      // line 986
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_SIZE_MASK                        0x0000003E
#define SDMA0_SDMA_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE__SHIFT             0x8      // line 987
#define SDMA0_SDMA_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE_MASK               0x00000100
#define SDMA0_SDMA_QUEUE0_RB_CNTL__MCU_WPTR_POLL_ENABLE__SHIFT         0xb      // line 990
#define SDMA0_SDMA_QUEUE0_RB_CNTL__MCU_WPTR_POLL_ENABLE_MASK           0x00000800
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT        0xc      // line 991
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK          0x00001000
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_PRIV__SHIFT                      0x17     // line 994
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_PRIV_MASK                        0x00800000

#define SDMA0_SDMA_QUEUE0_IB_CNTL__IB_ENABLE__SHIFT                    0x0      // line 1032
#define SDMA0_SDMA_QUEUE0_IB_CNTL__IB_ENABLE_MASK                      0x00000001

#define SDMA0_SDMA_QUEUE0_DOORBELL__ENABLE__SHIFT                      0x1c     // line 1058
#define SDMA0_SDMA_QUEUE0_DOORBELL__ENABLE_MASK                        0x10000000

#define SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET__OFFSET__SHIFT               0x2      // line 1068
#define SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK                 0x0FFFFFFC

#define SDMA0_SDMA_MCU_CNTL__HALT__SHIFT                               0x0      // line 3719
#define SDMA0_SDMA_MCU_CNTL__HALT_MASK                                 0x00000001
#define SDMA0_SDMA_MCU_CNTL__RESET__SHIFT                              0x1      // line 3720
#define SDMA0_SDMA_MCU_CNTL__RESET_MASK                                0x00000002

#define SDMA0_SDMA_UTCL1_CNTL__REDO_DELAY__SHIFT                       0x0      // line 393
#define SDMA0_SDMA_UTCL1_CNTL__REDO_DELAY_MASK                         0x0000001F
#define SDMA0_SDMA_UTCL1_CNTL__RESP_MODE__SHIFT                        0x9      // line 395
#define SDMA0_SDMA_UTCL1_CNTL__RESP_MODE_MASK                          0x00000600

#define SDMA0_SDMA_WATCHDOG_CNTL__QUEUE_HANG_COUNT__SHIFT              0x0      // line 289
#define SDMA0_SDMA_WATCHDOG_CNTL__QUEUE_HANG_COUNT_MASK                0x000000FF

//------------------------------------------------------------------
// SDMA opcode + helpers — sdma_pkt_open.h subset for NOP / FENCE /
// TRAP. Enough to emit a ring test that the host can wait on.
//------------------------------------------------------------------
constexpr uint32_t SDMA_OP_NOP    = 0;
constexpr uint32_t SDMA_OP_COPY   = 1;
constexpr uint32_t SDMA_OP_FENCE  = 5;
constexpr uint32_t SDMA_OP_TRAP   = 6;
constexpr uint32_t SDMA_OP_TIMESTAMP = 13;

constexpr uint32_t SDMA_SUBOP_COPY_LINEAR = 0;

static inline uint32_t SDMA_PKT_HEADER_OP(uint32_t op)         { return (op & 0xff); }
static inline uint32_t SDMA_PKT_HEADER_SUB_OP(uint32_t sub_op) { return ((sub_op & 0xff) << 8); }
static inline uint32_t SDMA_PKT_HEADER_CPV(uint32_t v)         { return ((v & 0x1) << 28); }

// COPY_LINEAR max byte count is 0x400000 - 1 on RDNA4
// (HW counter is a 22-bit field, byte_count - 1).
constexpr uint32_t kSDMACopyLinearMaxBytes = 0x00400000u;

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
    // DWORD offset into the doorbell BAR (BAR2). Programmed into
    // SDMA_QUEUE0_DOORBELL_OFFSET. SOC21 default: 0x200 for SDMA0,
    // 0x214 for SDMA1 (= sdma_engine[i] << 1).
    uint32_t  doorbell_index;
};

struct SDMAContext {
    SDMAInstance instance[kSDMAInstanceCount];
    bool         microcode_loaded;
};

//------------------------------------------------------------------
// API
//------------------------------------------------------------------

// Compute the absolute BAR5 dword offset for an SDMA register on a
// given instance. Mirrors sdma_v7_1_get_reg_offset() (sdma_v7_1.c:117).
// Hyp-dec range is "internal_offset >= SDMA0_SDMA_IDX_0_END (0x450)"
// and uses GC[1] base — for now we treat that as identical to GC[0]
// since on R9700 they live in the same SMN window; refine when the
// IP discovery walker grows multi-instance support.
static inline uint32_t
sdma_reg_offset(const DeviceContext &ctx, uint32_t instance, uint32_t reg)
{
    const uint32_t base = ctx.ip.get(IPBlock::GC);
    if (reg >= kSDMA0_SDMA_IDX_0_END) {
        // GC base[1] in upstream; collapsed to base[0] until IP
        // discovery supplies a separate hyp-dec base.
        return base + reg + (instance != 0
                             ? kSDMA1_HYP_DEC_REG_OFFSET * instance
                             : 0u);
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
// offset, so wptr_dword << 2). Writes to BAR2 dev.bar2MemIndex at
// (doorbell_index * 4) — see amdgpu_mm_wdoorbell @
// amdgpu_doorbell_mgr.c:59 for the upstream byte-stride.
kern_return_t sdma_kick_doorbell(const DeviceContext &dev,
                                 const SDMAInstance &inst);

// Append dwords to the ring at the current software wptr; wraps.
// Returns the number of dwords actually written (0 on overflow).
uint32_t sdma_ring_write(SDMAInstance &inst,
                         const uint32_t *src, uint32_t dwords);

// Submit an SDMA COPY_LINEAR + FENCE pair, kick doorbell, poll fence.
// Used by item 195: GART sanity test. src/dst are GPU-visible bus
// addresses (either DART iovas for sysmem or GART iovas for mapped
// sysmem-as-VRAM aliases). byte_count must be ≤ kSDMACopyLinearMaxBytes.
kern_return_t sdma_copy_linear_test(const DeviceContext &dev,
                                    SDMAInstance &inst,
                                    uint64_t src_bus, uint64_t dst_bus,
                                    uint32_t byte_count,
                                    uint64_t timeout_us);

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
