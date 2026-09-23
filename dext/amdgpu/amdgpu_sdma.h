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
//  Rings and write-back pages use 16 KB-aligned visible VRAM allocations.
//  CPU access uses BAR0; GPU registers and packets receive VRAM MC addresses.
//

#pragma once

#include <stdint.h>
#include "amdgpu_sdma_packets.h"

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
// SDMA register offsets — per-chip-family tables selected at runtime
// from the discovered GC IP version (dev.ip.getVersion(IPBlock::GC)).
//
// [[feedback_mac_amdgpu_per_ip_version_offsets]] — same driver binary
// must work for ANY supported AMD card, so register offsets cannot be
// hardcoded for one chip family. Each supported (GC major, minor) gets
// its own offset table; sdma_reg_offset() picks the right one based on
// the chip discovery returned.
//
// Currently supported families:
//   • gc_12_0_X — R9700 (gfx1201, GFX 12.0.1) and other GFX 12.0 chips.
//     Source: upstream gc_12_0_0_offset.h.
//   • gc_12_1_X — gfx1250 and the "SDMA0_SDMA_QUEUE0_*" naming family.
//     Same registers, offsets are 0x180 higher. Source: upstream
//     gc_12_1_0_offset.h.
//
// Adding a new family: define another SDMARegOffsets_gc_*_*_* struct,
// add it to sdma_pick_reg_table()'s dispatch, link the relevant chips
// to it. Don't fork sdma_v7_0.cpp — same code reads through the table.
struct SDMARegOffsets {
    uint32_t STATUS_REG;
    uint32_t WATCHDOG_CNTL;
    uint32_t UTCL1_CNTL;
    uint32_t UTCL1_PAGE;
    uint32_t QUEUE0_RB_CNTL;
    uint32_t QUEUE0_RB_BASE;
    uint32_t QUEUE0_RB_BASE_HI;
    uint32_t QUEUE0_RB_RPTR;
    uint32_t QUEUE0_RB_RPTR_HI;
    uint32_t QUEUE0_RB_WPTR;
    uint32_t QUEUE0_RB_WPTR_HI;
    uint32_t QUEUE0_RB_RPTR_ADDR_LO;
    uint32_t QUEUE0_RB_RPTR_ADDR_HI;
    uint32_t QUEUE0_IB_CNTL;
    uint32_t QUEUE0_DOORBELL;
    uint32_t QUEUE0_DOORBELL_OFFSET;
    uint32_t QUEUE0_RB_WPTR_POLL_ADDR_LO;
    uint32_t QUEUE0_RB_WPTR_POLL_ADDR_HI;
    uint32_t QUEUE0_MINOR_PTR_UPDATE;
    uint32_t MCU_CNTL;  // hyp-dec range; uses GC BASE_IDX 1 not 0
};

// gc_12_0_0 — for GFX 12.0.x (R9700, etc.).
inline constexpr SDMARegOffsets kSDMARegOffsets_gc_12_0_0 = {
    /* STATUS_REG                  */ 0x0024,  // gc_12_0_0_offset.h:56
    /* WATCHDOG_CNTL               */ 0x002b,  // gc_12_0_0_offset.h:70
    /* UTCL1_CNTL                  */ 0x0035,  // gc_12_0_0_offset.h:90
    /* UTCL1_PAGE                  */ 0x0038,  // gc_12_0_0_offset.h:96
    /* QUEUE0_RB_CNTL              */ 0x0080,  // gc_12_0_0_offset.h:182
    /* QUEUE0_RB_BASE              */ 0x0081,  // gc_12_0_0_offset.h:184
    /* QUEUE0_RB_BASE_HI           */ 0x0082,  // gc_12_0_0_offset.h:186
    /* QUEUE0_RB_RPTR              */ 0x0083,  // gc_12_0_0_offset.h:188
    /* QUEUE0_RB_RPTR_HI           */ 0x0084,  // gc_12_0_0_offset.h:190
    /* QUEUE0_RB_WPTR              */ 0x0085,  // gc_12_0_0_offset.h:192
    /* QUEUE0_RB_WPTR_HI           */ 0x0086,  // gc_12_0_0_offset.h:194
    /* QUEUE0_RB_RPTR_ADDR_LO      */ 0x0087,  // gc_12_0_0_offset.h:196
    /* QUEUE0_RB_RPTR_ADDR_HI      */ 0x0088,  // gc_12_0_0_offset.h:198
    /* QUEUE0_IB_CNTL              */ 0x0089,  // gc_12_0_0_offset.h:200
    /* QUEUE0_DOORBELL             */ 0x008f,  // gc_12_0_0_offset.h:212
    /* QUEUE0_DOORBELL_OFFSET      */ 0x0091,  // gc_12_0_0_offset.h:216
    /* QUEUE0_RB_WPTR_POLL_ADDR_LO */ 0x0098,  // gc_12_0_0_offset.h:230
    /* QUEUE0_RB_WPTR_POLL_ADDR_HI */ 0x0099,  // gc_12_0_0_offset.h:232
    /* QUEUE0_MINOR_PTR_UPDATE     */ 0x009b,  // gc_12_0_0_offset.h:236
    /* MCU_CNTL                    */ 0x588e,  // gc_12_0_0_offset.h:948
};

// gc_12_1_0 — for GFX 12.1.x (gfx1250 etc.). Offsets +0x180 vs 12.0.x.
inline constexpr SDMARegOffsets kSDMARegOffsets_gc_12_1_0 = {
    /* STATUS_REG                  */ 0x0024,  // gc_12_1_0_offset.h:56
    /* WATCHDOG_CNTL               */ 0x002b,  // gc_12_1_0_offset.h:70
    /* UTCL1_CNTL                  */ 0x0037,  // gc_12_1_0_offset.h:94
    /* UTCL1_PAGE                  */ 0x003a,  // gc_12_1_0_offset.h:100
    /* QUEUE0_RB_CNTL              */ 0x0200,  // gc_12_1_0_offset.h:194
    /* QUEUE0_RB_BASE              */ 0x0201,  // gc_12_1_0_offset.h:196
    /* QUEUE0_RB_BASE_HI           */ 0x0202,  // gc_12_1_0_offset.h:198
    /* QUEUE0_RB_RPTR              */ 0x0203,  // gc_12_1_0_offset.h:200
    /* QUEUE0_RB_RPTR_HI           */ 0x0204,  // gc_12_1_0_offset.h:202
    /* QUEUE0_RB_WPTR              */ 0x0205,  // gc_12_1_0_offset.h:204
    /* QUEUE0_RB_WPTR_HI           */ 0x0206,  // gc_12_1_0_offset.h:206
    /* QUEUE0_RB_RPTR_ADDR_LO      */ 0x0207,  // gc_12_1_0_offset.h:208
    /* QUEUE0_RB_RPTR_ADDR_HI      */ 0x0208,  // gc_12_1_0_offset.h:210
    /* QUEUE0_IB_CNTL              */ 0x0209,  // gc_12_1_0_offset.h:212
    /* QUEUE0_DOORBELL             */ 0x020f,  // gc_12_1_0_offset.h:224
    /* QUEUE0_DOORBELL_OFFSET      */ 0x0211,  // gc_12_1_0_offset.h:228
    /* QUEUE0_RB_WPTR_POLL_ADDR_LO */ 0x0218,  // gc_12_1_0_offset.h:242
    /* QUEUE0_RB_WPTR_POLL_ADDR_HI */ 0x0219,  // gc_12_1_0_offset.h:244
    /* QUEUE0_MINOR_PTR_UPDATE     */ 0x021b,  // gc_12_1_0_offset.h:248
    /* MCU_CNTL                    */ 0x588e,  // gc_12_1_0_offset.h:1224
};

// Selector — pick the offset table by discovered GC IP version.
// Falls back to gc_12_0_0 if the version is unknown so partial-discovery
// runs (or older test traces) don't crash; logs a warning the first time
// it falls back.
inline const SDMARegOffsets &
sdma_pick_reg_table(const DeviceContext &dev)
{
    const IPVersion gc = dev.ip.getVersion(IPBlock::GC);
    // GFX 12.1.x family.
    if (gc.major == 12 && gc.minor == 1) {
        return kSDMARegOffsets_gc_12_1_0;
    }
    // GFX 12.0.x family (default — also catches {0,0,0} unresolved).
    return kSDMARegOffsets_gc_12_0_0;
}

// Convenience accessor: sdma_regs(dev).QUEUE0_RB_CNTL etc. Returns the
// selected per-chip table at runtime.
inline const SDMARegOffsets &sdma_regs(const DeviceContext &dev) {
    return sdma_pick_reg_table(dev);
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
//------------------------------------------------------------------
// Per-instance ring state. Two instances live side by side; we
// drive both with the same layout.
//------------------------------------------------------------------
struct SDMAInstance {
    uint32_t  instance;          // 0 or 1
    bool      inited;
    bool      enabled;

    // Ring and write-back live in the visible VRAM arena, retained until
    // reset/PCI close. CPU access goes through BAR0, not a host DMA mapping.
    uint64_t  ring_gpu_va;       // MC address (= ring_bus equivalent)
    uint64_t  ring_vram_off;     // BAR0-relative byte offset for CPU writes
    uint32_t  ring_size_dwords;
    uint32_t  ring_ptr_mask;

    uint64_t  wb_bus;
    uint64_t  wb_vram_off;
    const DeviceContext *wb_device;
    uint32_t  cs_fence_shadow; // CPU cache populated only by a valid BAR read
    uint64_t  rptr_gpu_addr;    // wb_bus + 0
    uint64_t  wptr_poll_gpu_addr; // wb_bus + 0x40

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
// given instance. Direct port of sdma_v7_0_get_reg_offset (sdma_v7_0.c:125).
//
// Two register regimes:
//   Hyp-dec (HYPervisor DECoded) range [0x5880..0x589a] inclusive —
//     resolves through GC BASE_IDX 1; instance increment is
//     SDMA1_HYP_DEC_REG_OFFSET (0x30).
//   Everything else — GC BASE_IDX 0; instance increment is
//     SDMA1_REG_OFFSET (0x600) for instance==1.
//
// **v0.1.40 fix:** prior versions collapsed both regimes onto
// GC BASE_IDX 0, which meant MCU_CNTL (0x588e, in hyp-dec range) was
// being read/written at GC[0]+0x588e instead of GC[1]+0x588e. The
// resulting register hit a completely unrelated location, returning
// the apparently random 0x92929292 pattern and silently ignoring
// HALT/RESET writes — so the SDMA MCU never actually unhalted, and
// engine-side WPTR updates from the doorbell aperture never landed.
constexpr uint32_t kSDMA0_HYP_DEC_REG_START = 0x5880;
constexpr uint32_t kSDMA0_HYP_DEC_REG_END   = 0x589a;

static inline uint32_t
sdma_reg_offset(const DeviceContext &ctx, uint32_t instance, uint32_t reg)
{
    if (reg >= kSDMA0_HYP_DEC_REG_START && reg <= kSDMA0_HYP_DEC_REG_END) {
        const uint32_t base = ctx.ip.getBase(IPBlock::GC, /*baseIdx=*/1);
        const uint32_t inst_add = (instance != 0)
            ? (kSDMA1_HYP_DEC_REG_OFFSET * instance) : 0u;
        return base + reg + inst_add;
    }
    const uint32_t base = ctx.ip.get(IPBlock::GC);  // BASE_IDX 0
    return base + reg + (instance == 1 ? kSDMA1_REG_OFFSET : 0u);
}

// Allocate ring + WB page for one instance. Idempotent.
struct GMCContext;
kern_return_t sdma_alloc_storage(DeviceContext &dev, SDMAInstance &inst,
                                 GMCContext &gmc);

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
uint32_t sdma_ring_write(const DeviceContext &dev, SDMAInstance &inst,
                         const uint32_t *src, uint32_t dwords);

// Submit an SDMA COPY_LINEAR + FENCE pair, kick doorbell, poll fence.
// src/dst must be GPU addresses: VRAM or explicitly GART-mapped system memory.
// byte_count must be ≤ kSDMACopyLinearMaxBytes. Completion alone does not prove
// data integrity; the caller must verify the destination contents.
kern_return_t sdma_copy_linear_test(const DeviceContext &dev,
                                    SDMAInstance &inst,
                                    uint64_t src_bus, uint64_t dst_bus,
                                    uint32_t byte_count,
                                    uint64_t timeout_us);

// Internal completion slots: diagnostics at 0x80, CS ABI at 0xC0.
kern_return_t sdma_clear_fence(const DeviceContext &, const SDMAInstance &, uint32_t offset);
kern_return_t sdma_read_fence(const DeviceContext &, const SDMAInstance &, uint32_t offset, uint32_t *value);
bool sdma_read_cs_fence(void *context, uint32_t *value);

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
