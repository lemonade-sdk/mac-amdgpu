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
    constexpr uint32_t CP_RB0_BASE                = 0x1DE0;
    constexpr uint32_t CP_RB0_CNTL                = 0x1DE1;
    constexpr uint32_t CP_RB0_RPTR_ADDR           = 0x1DE3;
    constexpr uint32_t CP_RB0_RPTR_ADDR_HI        = 0x1DE4;
    constexpr uint32_t CP_RB_VMID                 = 0x1DF1;
    constexpr uint32_t CP_RB0_WPTR                = 0x1DF4;
    constexpr uint32_t CP_RB_DOORBELL_RANGE_LOWER = 0x1DFA;
    constexpr uint32_t CP_RB_DOORBELL_RANGE_UPPER = 0x1DFB;
    constexpr uint32_t CP_RB0_BASE_HI             = 0x1E51;
    constexpr uint32_t CP_RB_DOORBELL_CONTROL     = 0x1E8D;
    constexpr uint32_t CP_ME_CNTL                 = 0x0803;
}

// CP_ME_CNTL bit positions (gfx_v12_0.c REG_SET_FIELDs imply these).
// ME_HALT lives at bit 21 in gfx12; setting 1 halts ME0, 0 runs it.
// PFP_HALT at bit 18, MEC_HALT at bit 22 (we don't touch those here).
constexpr uint32_t kCP_ME_CNTL__ME_HALT__SHIFT = 21;
constexpr uint32_t kCP_ME_CNTL__ME_HALT_MASK   = (1u << 21);

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

// Toggle CP_ME_CNTL.ME_HALT. After cp_enable(true) the CP can fetch
// + execute from the GFX ring; before, the ring is dormant.
kern_return_t cp_enable(const DeviceContext &dev, bool enable);

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
