//
//  amdgpu_ih.h — Interrupt Handler (IH) v7_0 ring + drain interface.
//
//  See docs/port_plans/IH_v7.md for the full port-order checklist.
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/ih_v7_0.c
//      drivers/gpu/drm/amd/amdgpu/amdgpu_ih.c
//      drivers/gpu/drm/amd/include/ivsrcid/*
//
//  Hardware model: every IP block on the GPU posts interrupts into
//  a unified ring buffer in system memory (the IH ring). When the
//  ring becomes non-empty the GPU fires an MSI-X line to the host,
//  the host walks the ring entries, and dispatches each to a
//  per-(client_id, src_id) handler.
//
//  Entry format on RDNA4 / GFX12 (32 bytes = 8 dwords):
//      DW0: [7:0] client_id  [15:8] src_id  [23:16] ring_id
//           [27:24] vmid     [31] vmid_src
//      DW1: timestamp_lo
//      DW2: [15:0] timestamp_hi  [31] timestamp_src
//      DW3: [15:0] pasid  [23:16] node_id
//      DW4..7: src_data[0..3] — meaning is per-source-handler
//
//  We allocate the ring in DART-mapped system memory at 16 KB
//  alignment (per the AS DART constraint). Default 256 KB capacity
//  matches upstream amdgpu_ih_ring_init's default.
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

constexpr uint32_t kIHRingDefaultBytes = 256 * 1024;
constexpr uint32_t kIHEntryDwords      = 8;
constexpr uint32_t kIHEntryBytes       = kIHEntryDwords * 4;

// Single decoded IH entry — what handlers receive from the dispatcher.
struct IHEntry {
    uint8_t  client_id;     // OSSSYS / GFX / SDMA / etc.
    uint8_t  src_id;
    uint16_t ring_id;
    uint8_t  vmid;
    uint8_t  vmid_src;
    uint64_t timestamp;
    uint16_t pasid;
    uint8_t  node_id;
    uint32_t src_data[4];   // raw src_data[0..3]
};

// Per-ring state — we only run one ring (Ring0) for first PM4.
struct IHContext {
    bool      inited;
    bool      enabled;

    // Ring buffer — system memory, DART-mapped, 16 KB-aligned.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *ring_buf;
    IODMACommand             *ring_dma;
    IOBufferMemoryDescriptor *wptr_shadow_buf;
    IODMACommand             *wptr_shadow_dma;
#endif
    uint64_t  ring_bus;
    void     *ring_cpu;
    uint32_t  ring_size_bytes;  // power of two
    uint32_t  ring_size_dwords;
    uint32_t  ptr_mask;         // (ring_size_dwords - 1)
    uint64_t  wptr_shadow_bus;
    volatile uint32_t *wptr_shadow_cpu;  // GPU writes wptr here
    uint32_t  rptr;              // host's read pointer (dword index)

    // Counters
    uint64_t  entries_processed;
    uint64_t  overflows_seen;
};

// IH v7 has known interrupt source IDs we care about for "Hello PM4".
// Full list in drivers/gpu/drm/amd/include/ivsrcid/{gfx,sdma,vmc}/*.h
namespace IHSourceID {
    // GFX clients
    constexpr uint8_t CLIENT_GFX          = 0x0A;   // SOC21_IH_CLIENTID_GFX
    constexpr uint8_t CLIENT_ATHUB        = 0x02;
    constexpr uint8_t CLIENT_VMC          = 0x07;
    constexpr uint8_t CLIENT_SDMA0        = 0x0E;
    constexpr uint8_t CLIENT_SDMA1        = 0x0F;

    // GFX sources (within CLIENT_GFX)
    constexpr uint8_t SRC_CP_EOP          = 0xB5;   // 181
    constexpr uint8_t SRC_CP_ECC_ERROR    = 0xC5;   // 197
    constexpr uint8_t SRC_UTCL2_FAULT     = 0x00;   // (with CLIENT_ATHUB)

    // SDMA sources
    constexpr uint8_t SRC_SDMA_TRAP       = 0xE0;   // 224
}

// Register offsets (OSSSYS IP block — IHv7).
// From drivers/gpu/drm/amd/include/asic_reg/oss/osssys_*_offset.h.
namespace IHRegs {
    constexpr uint32_t IH_RB_BASE                  = 0x0083;
    constexpr uint32_t IH_RB_BASE_HI               = 0x0084;
    constexpr uint32_t IH_RB_CNTL                  = 0x0080;
    constexpr uint32_t IH_RB_RPTR                  = 0x0081;
    constexpr uint32_t IH_RB_WPTR                  = 0x0082;
    constexpr uint32_t IH_RB_WPTR_ADDR_LO          = 0x0086;
    constexpr uint32_t IH_RB_WPTR_ADDR_HI          = 0x0085;
    constexpr uint32_t IH_DOORBELL_RPTR            = 0x0087;
    constexpr uint32_t IH_STORM_CLIENT_LIST_CNTL   = 0x00AA;
    constexpr uint32_t IH_INT_FLOOD_CNTL           = 0x00AB;
    constexpr uint32_t IH_MSI_STORM_CTRL           = 0x00AC;
}

// IH_RB_CNTL bit positions (from upstream IH_RB_CNTL field defs).
constexpr uint32_t kIH_RB_CNTL__RB_ENABLE__SHIFT       = 0;
constexpr uint32_t kIH_RB_CNTL__RB_SIZE__SHIFT         = 1;
constexpr uint32_t kIH_RB_CNTL__WPTR_WRITEBACK__SHIFT  = 8;
constexpr uint32_t kIH_RB_CNTL__ENABLE_INTR__SHIFT     = 0;   // not the same; alias
constexpr uint32_t kIH_RB_CNTL__MC_SPACE__SHIFT        = 13;
constexpr uint32_t kIH_RB_CNTL__RPTR_REARM__SHIFT      = 4;
constexpr uint32_t kIH_RB_CNTL__MC_SNOOP__SHIFT        = 16;
constexpr uint32_t kIH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT = 7;
constexpr uint32_t kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT  = 31;

// MC_SPACE values: 0 = invalid, 1 = MC, 2 = MC translated, 4 = IOMMU
constexpr uint32_t kIH_RB_CNTL_MC_SPACE_IOMMU = 4;

//
// API
//

// Allocate ring buffer + wptr shadow. Idempotent.
kern_return_t ih_init(DeviceContext &dev, IHContext &ih);

// Free everything ih_init allocated.
void ih_release(IHContext &ih);

// Toggle IH_RB_CNTL.RB_ENABLE. Safe to call any time.
kern_return_t ih_toggle(const DeviceContext &dev, IHContext &ih,
                        bool enable);

// Program the ring registers + enable. Called once after init.
kern_return_t ih_enable_ring(const DeviceContext &dev, IHContext &ih);

// Configure storm + flood control + MSI storm throttle.
kern_return_t ih_program_msi_storm(const DeviceContext &dev,
                                   const IHContext &ih);

// Drain the ring — process every entry between rptr and the current
// wptr. Caller-provided dispatcher is invoked per entry with the
// decoded form. Returns the number of entries processed.
typedef void (*IHDispatchFn)(const IHEntry &entry, void *user);

uint32_t ih_drain(const DeviceContext &dev, IHContext &ih,
                  IHDispatchFn dispatch, void *user);

// Top-level IHInit stage entry.
kern_return_t ih_init_full(DeviceContext &dev, IHContext &ih);

} // namespace amdgpu
