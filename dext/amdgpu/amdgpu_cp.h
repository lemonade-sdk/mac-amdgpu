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
//      KIQ ring buffer  — 4 KB, **VRAM** (carved from VRAMBumpAllocator).
//                         The CP reads PM4 from here.
//      KIQ MQD          — 4 KB, **VRAM**. Memory Queue Descriptor;
//                         the CP reads it once at queue-create time
//                         to learn what's in the ring.
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

    // KIQ ring (VRAM)
    VRAMAllocation   kiq_ring;
    uint32_t         kiq_ring_size_dwords;
    uint32_t         kiq_ring_ptr_mask;     // ring_size_dwords - 1

    // KIQ MQD (VRAM)
    VRAMAllocation   kiq_mqd;

    // Write-back page (DART-mapped sysmem)
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

// Top-level CPInit stage entry — alloc storage only for now.
// Actual ring activation lands in the next chunk.
kern_return_t cp_init_full(DeviceContext &dev,
                           GMCContext &gmc, CPContext &cp);

} // namespace amdgpu
