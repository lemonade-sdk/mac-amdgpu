//
//  amdgpu_cp.h — Command Processor / KIQ ring infrastructure for GFX12.
//
//  This first chunk gets the storage (ring buffer + MQD + write-back
//  page) and the abstraction for staging PM4 commands into a ring
//  in place. Actual MMIO programming of the HQD registers, doorbell
//  setup, and CP enable lands in the next chunk.
//
//  See docs/port_plans/HELLO_PM4.md for the full critical path.
//
//  Memory layout:
//      KIQ ring buffer  — 16 KB, **DART-mapped sysmem** (via GART
//                         once GMC is up). The CP fetches PM4 from
//                         here through GART translation. Sysmem
//                         (not VRAM) so we can write PM4 directly
//                         from the dext without needing an in-dext
//                         BAR2 mapping.
//      KIQ MQD          — 4 KB, **DART-mapped sysmem**. Memory
//                         Queue Descriptor; the CP reads it once at
//                         queue-create time to learn what's in the
//                         ring.
//      Write-back page  — 16 KB, **DART-mapped sysmem**. The CP
//                         writes back rptr/wptr and fence values
//                         here; the host reads from the same backing.
//

#pragma once

#include <stdint.h>

#ifdef __APPLE__
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#endif

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_vram.h"
#include "amdgpu_pm4.h"

namespace amdgpu {

struct GMCContext;   // forward — we ask its VRAM allocator for storage

// GC register offsets for the CP HQD registers we touch.
// Sourced from gc_12_0_0_offset.h. Used by cp_hqd_program / cp_enable.
namespace CPRegs {
    // gc_12_0_0_offset.h: regCP_RB_WPTR_DELAY  = 0x0f61
    constexpr uint32_t CP_RB_WPTR_DELAY           = 0x0F61;
    // gc_12_0_0_offset.h: regCP_RB0_BASE = 0x1DE0
    constexpr uint32_t CP_RB0_BASE                = 0x1DE0;
    constexpr uint32_t CP_RB0_CNTL                = 0x1DE1;
    constexpr uint32_t CP_RB0_RPTR_ADDR           = 0x1DE3;
    constexpr uint32_t CP_RB0_RPTR_ADDR_HI        = 0x1DE4;
    // gc_12_0_0_offset.h: regCP_DEVICE_ID = 0x1deb
    constexpr uint32_t CP_DEVICE_ID               = 0x1DEB;
    constexpr uint32_t CP_RB_VMID                 = 0x1DF1;
    constexpr uint32_t CP_RB0_WPTR                = 0x1DF4;
    // gc_12_0_0_offset.h: regCP_RB0_WPTR_HI = 0x1df5
    constexpr uint32_t CP_RB0_WPTR_HI             = 0x1DF5;
    constexpr uint32_t CP_RB_DOORBELL_RANGE_LOWER = 0x1DFA;
    constexpr uint32_t CP_RB_DOORBELL_RANGE_UPPER = 0x1DFB;
    // gc_12_0_0_offset.h: regCP_MEC_DOORBELL_RANGE_{LOWER,UPPER} = 0x1dfc/0x1dfd
    constexpr uint32_t CP_MEC_DOORBELL_RANGE_LOWER = 0x1DFC;
    constexpr uint32_t CP_MEC_DOORBELL_RANGE_UPPER = 0x1DFD;
    // gc_12_0_0_offset.h: regCP_MAX_CONTEXT = 0x1e4e
    constexpr uint32_t CP_MAX_CONTEXT             = 0x1E4E;
    constexpr uint32_t CP_RB0_BASE_HI             = 0x1E51;
    // gc_12_0_0_offset.h: regCP_RB_WPTR_POLL_ADDR_LO/_HI = 0x1e8b/0x1e8c
    constexpr uint32_t CP_RB_WPTR_POLL_ADDR_LO    = 0x1E8B;
    constexpr uint32_t CP_RB_WPTR_POLL_ADDR_HI    = 0x1E8C;
    constexpr uint32_t CP_RB_DOORBELL_CONTROL     = 0x1E8D;
    // gc_12_0_0_offset.h: regCP_RB_ACTIVE = 0x1f40
    constexpr uint32_t CP_RB_ACTIVE               = 0x1F40;
    constexpr uint32_t CP_ME_CNTL                 = 0x0803;
    // gc_12_0_0_offset.h: regCP_MEC_RS64_CNTL = 0x2904 (BASE_IDX=1, we still
    // use single-base-table; offset is the same since IPBaseTable.get(GC) is
    // the BASE_IDX=0 entry's value — BASE_IDX semantics covered by
    // amdgpu_regs.h's note).
    constexpr uint32_t CP_MEC_RS64_CNTL           = 0x2904;
}

// CP_ME_CNTL bit positions per gc_12_0_0_sh_mask.h:13868-13889.
//   PFP_HALT__SHIFT = 0x1a (26) → MASK 0x04000000
//   ME_HALT__SHIFT  = 0x1c (28) → MASK 0x10000000
// (Previous values for ME_HALT in this header were WRONG — fixed per
// upstream sh_mask. Audit-7 #1.)
#define CP_ME_CNTL__PFP_HALT__SHIFT 0x1a
#define CP_ME_CNTL__PFP_HALT_MASK   0x04000000
#define CP_ME_CNTL__ME_HALT__SHIFT  0x1c
#define CP_ME_CNTL__ME_HALT_MASK    0x10000000

// CP_MEC_RS64_CNTL bit positions per gc_12_0_0_sh_mask.h:15886-15909.
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE__SHIFT 0x4
#define CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE_MASK   0x00000010
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET__SHIFT       0x10
#define CP_MEC_RS64_CNTL__MEC_PIPE0_RESET_MASK         0x00010000
#define CP_MEC_RS64_CNTL__MEC_PIPE1_RESET__SHIFT       0x11
#define CP_MEC_RS64_CNTL__MEC_PIPE1_RESET_MASK         0x00020000
#define CP_MEC_RS64_CNTL__MEC_PIPE2_RESET__SHIFT       0x12
#define CP_MEC_RS64_CNTL__MEC_PIPE2_RESET_MASK         0x00040000
#define CP_MEC_RS64_CNTL__MEC_PIPE3_RESET__SHIFT       0x13
#define CP_MEC_RS64_CNTL__MEC_PIPE3_RESET_MASK         0x00080000
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE__SHIFT      0x1a
#define CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK        0x04000000
#define CP_MEC_RS64_CNTL__MEC_PIPE1_ACTIVE__SHIFT      0x1b
#define CP_MEC_RS64_CNTL__MEC_PIPE1_ACTIVE_MASK        0x08000000
#define CP_MEC_RS64_CNTL__MEC_PIPE2_ACTIVE__SHIFT      0x1c
#define CP_MEC_RS64_CNTL__MEC_PIPE2_ACTIVE_MASK        0x10000000
#define CP_MEC_RS64_CNTL__MEC_PIPE3_ACTIVE__SHIFT      0x1d
#define CP_MEC_RS64_CNTL__MEC_PIPE3_ACTIVE_MASK        0x20000000
#define CP_MEC_RS64_CNTL__MEC_HALT__SHIFT              0x1e
#define CP_MEC_RS64_CNTL__MEC_HALT_MASK                0x40000000

// CP_RB0_CNTL fields — gfx_v12_0.c:2735 writes RB_BUFSZ and RB_BLKSZ.
#define CP_RB0_CNTL__RB_BUFSZ__SHIFT 0x0
#define CP_RB0_CNTL__RB_BUFSZ_MASK   0x0000003F
#define CP_RB0_CNTL__RB_BLKSZ__SHIFT 0x8
#define CP_RB0_CNTL__RB_BLKSZ_MASK   0x00003F00

// CP_RB_DOORBELL_CONTROL field shifts (upstream gfx_v12_0.c uses
// REG_SET_FIELD on these). DOORBELL_OFFSET[27:2], DOORBELL_EN[30].
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT 0x2
#define CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK   0x0FFFFFFC
#define CP_RB_DOORBELL_CONTROL__DOORBELL_EN__SHIFT     0x1e
#define CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK       0x40000000

// CP_RB_DOORBELL_RANGE_LOWER.DOORBELL_RANGE_LOWER + RANGE_UPPER mask.
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER__SHIFT 0x2
#define CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_MASK   0x0FFFFFFC
#define CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK   0x0FFFFFFC

// CP_RB_RPTR_ADDR_HI bit mask (upstream gfx_v12_0.c:2748 uses the
// `RB_RPTR_ADDR_HI` field mask to keep only the low 16 bits).
#define CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK 0x0000FFFF

// Default ring size — 4 KB matches Linux's KIQ ring default for
// GFX12 (one page; smaller than the upstream 16 KB only because the
// GPU's CP can address any power-of-two ≥ 4 KB).
constexpr uint32_t kCPRingDefaultBytes = 16 * 1024;
constexpr uint32_t kCPMQDBytes         = 4 * 1024;
constexpr uint32_t kCPWBPageBytes      = 16 * 1024;   // AS page-aligned

// Write-back layout (host + GPU agree on these offsets within
// the 16 KB write-back page):
constexpr uint32_t kCPWBOffsetRptr  = 0x000;
constexpr uint32_t kCPWBOffsetWptr  = 0x040;
constexpr uint32_t kCPWBOffsetFence = 0x080;   // 8 B, qword-aligned

struct CPContext {
    bool             inited;

    // KIQ ring (sysmem, GART-mapped)
#ifdef __APPLE__
    IOBufferMemoryDescriptor *ring_buf;
    IODMACommand             *ring_dma;
#endif
    uint64_t          ring_bus;        // GPU-side bus address (== GART iova)
    void             *ring_cpu;        // CPU-side write target
    uint32_t          ring_size_dwords;
    uint32_t          ring_ptr_mask;   // ring_size_dwords - 1

    // KIQ MQD (sysmem)
#ifdef __APPLE__
    IOBufferMemoryDescriptor *mqd_buf;
    IODMACommand             *mqd_dma;
#endif
    uint64_t          mqd_bus;
    void             *mqd_cpu;

    // Write-back page (sysmem)
#ifdef __APPLE__
    IOBufferMemoryDescriptor *wb_buf;
    IODMACommand             *wb_dma;
#endif
    uint64_t          wb_bus;
    void             *wb_cpu;

    // Convenience pointers into the WB page (CPU-side):
    volatile uint32_t *rptr_cpu;
    volatile uint32_t *wptr_cpu;
    volatile uint64_t *fence_cpu;

    // GPU-side addresses derived from wb_bus
    uint64_t  rptr_gpu_addr;
    uint64_t  wptr_gpu_addr;
    uint64_t  fence_gpu_addr;

    // Software wptr — what the host has committed but not yet kicked.
    uint32_t  wptr;
    uint32_t  fence_counter;

    // Doorbell index (allocated separately in the next chunk).
    uint32_t  doorbell_index;
};

// Allocate KIQ ring + MQD + write-back page. Idempotent.
// Requires GMCContext.vram_alloc to be ready and the IOPCIDevice
// to be available via DeviceContext for the WB-page DMA mapping.
kern_return_t cp_alloc_storage(DeviceContext &dev,
                               GMCContext &gmc, CPContext &cp);

// Free everything cp_alloc_storage allocated. VRAMBumpAllocator
// doesn't currently support free (bump-only), so the VRAM regions
// stay reserved; only the sysmem WB page is torn down.
void cp_release_storage(CPContext &cp);

// Append PM4 dwords to the ring at the current software wptr.
// Wraps modulo the ring size. Returns the number of dwords written
// (or 0 if `dwords` would overflow the ring). Does NOT kick the
// doorbell — caller does that after all packets are staged.
uint32_t cp_ring_write(CPContext &cp, const uint32_t *src,
                       uint32_t dwords);

// Build a NOP+RELEASE_MEM packet pair into the ring. Returns the
// fence value the EOP write will deposit at fence_gpu_addr; caller
// should kick the doorbell then poll *fence_cpu for that value.
uint32_t cp_emit_eop_fence(CPContext &cp);

// Program the GFX ring's HQD registers (CP_RB0_BASE/BASE_HI/CNTL,
// RPTR_ADDR/HI, WPTR, VMID, doorbell range). Mirrors
// gfx_v12_0_cp_gfx_resume's register sequence — minus the per-pipe
// GRBM_GFX_INDEX selection since we're targeting RB0 on the default
// pipe. Caller must have populated CPContext via cp_alloc_storage.
//
// Picks doorbell index from cp.doorbell_index (caller sets — for
// first PM4 we hardcode 0 inside cp_init_full).
kern_return_t cp_hqd_program(const DeviceContext &dev, CPContext &cp);

// Toggle CP_ME_CNTL.{ME_HALT,PFP_HALT}. After cp_enable(true) the
// CP can fetch + execute from the GFX ring; before, the ring is
// dormant. Mirrors upstream gfx_v12_0_cp_gfx_enable
// (gfx_v12_0.c:2332) — both halts must drop together.
kern_return_t cp_enable(const DeviceContext &dev, bool enable);

// Toggle CP_MEC_RS64_CNTL — bring all 4 MEC pipes in/out of reset
// and active. Mirrors upstream gfx_v12_0_cp_compute_enable
// (gfx_v12_0.c:2778).
kern_return_t cp_compute_enable(const DeviceContext &dev, bool enable);

// Program CP_RB_DOORBELL_RANGE_{LOWER,UPPER} for GFX and
// CP_MEC_DOORBELL_RANGE_{LOWER,UPPER} for compute. Mirrors upstream
// gfx_v12_0_cp_set_doorbell_range (gfx_v12_0.c:2954).
kern_return_t cp_set_doorbell_range(const DeviceContext &dev,
                                    const CPContext &cp,
                                    uint32_t mec_first_doorbell,
                                    uint32_t mec_last_doorbell);

// Write the GFX ring's doorbell (BAR5 + doorbell_index<<3) with the
// current software wptr. The actual register-level write goes through
// IOPCIDevice::MemoryWrite32 on dev.bar5MemIndex.
kern_return_t cp_kick_doorbell(const DeviceContext &dev,
                               const CPContext &cp);

// End-to-end test: emit NOP+RELEASE_MEM, kick doorbell, poll fence.
// Returns kIOReturnSuccess if the fence value materialised at
// *fence_cpu within timeout_us microseconds. Designed for the
// SubmitTestPM4 selector — sanity-checks the entire submit path
// once HQD + CP enable have run.
kern_return_t cp_submit_eop_test(const DeviceContext &dev,
                                 CPContext &cp,
                                 uint64_t timeout_us,
                                 uint32_t *outFence);

// Top-level CPInit stage entry — alloc storage + (if IP base is
// resolved) program HQD + enable CP. Idempotent.
kern_return_t cp_init_full(DeviceContext &dev,
                           GMCContext &gmc, CPContext &cp);

} // namespace amdgpu
