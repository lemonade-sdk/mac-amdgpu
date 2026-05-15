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
    // ptr_mask is in BYTES — (ring_size_bytes - 1). Matches upstream
    // amdgpu_ih.c:52 (`ih->ptr_mask = ih->ring_size - 1`). rptr is a
    // byte offset; the HW wptr from the shadow is also bytes.
    uint32_t  ptr_mask;
    uint64_t  wptr_shadow_bus;
    volatile uint32_t *wptr_shadow_cpu;  // GPU writes wptr here
    // host read pointer, in BYTES — advances 32 per dispatched entry.
    uint32_t  rptr;

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
// From drivers/gpu/drm/amd/include/asic_reg/oss/osssys_7_0_0_offset.h.
namespace IHRegs {
    constexpr uint32_t IH_RB_CNTL                  = 0x0080; // line 112
    constexpr uint32_t IH_RB_RPTR                  = 0x0081; // line 114
    constexpr uint32_t IH_RB_WPTR                  = 0x0082; // line 116
    constexpr uint32_t IH_RB_BASE                  = 0x0083; // line 118
    constexpr uint32_t IH_RB_BASE_HI               = 0x0084; // line 120
    constexpr uint32_t IH_RB_WPTR_ADDR_HI          = 0x0085; // line 122
    constexpr uint32_t IH_RB_WPTR_ADDR_LO          = 0x0086; // line 124
    constexpr uint32_t IH_DOORBELL_RPTR            = 0x0087; // line 126
    constexpr uint32_t IH_STORM_CLIENT_LIST_CNTL   = 0x00aa; // line 150
    constexpr uint32_t IH_INT_FLOOD_CNTL           = 0x00d5; // line 192
    constexpr uint32_t IH_MSI_STORM_CTRL           = 0x00f1; // line 222
    constexpr uint32_t IH_CHICKEN                  = 0x018a; // line 262
}

// IH_RB_CNTL bit positions per osssys_7_0_0_sh_mask.h lines 170-185.
constexpr uint32_t kIH_RB_CNTL__RB_ENABLE__SHIFT             = 0x00; // line 170
constexpr uint32_t kIH_RB_CNTL__RB_SIZE__SHIFT               = 0x01; // line 171
constexpr uint32_t kIH_RB_CNTL__WPTR_WRITEBACK_ENABLE__SHIFT = 0x08; // line 172
constexpr uint32_t kIH_RB_CNTL__RB_FULL_DRAIN_ENABLE__SHIFT  = 0x09; // line 173
constexpr uint32_t kIH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT  = 0x10; // line 177
constexpr uint32_t kIH_RB_CNTL__ENABLE_INTR__SHIFT           = 0x11; // line 178
constexpr uint32_t kIH_RB_CNTL__MC_SWAP__SHIFT               = 0x12; // line 179
constexpr uint32_t kIH_RB_CNTL__MC_SNOOP__SHIFT              = 0x14; // line 180
constexpr uint32_t kIH_RB_CNTL__RPTR_REARM__SHIFT            = 0x15; // line 181
constexpr uint32_t kIH_RB_CNTL__MC_RO__SHIFT                 = 0x16; // line 182
constexpr uint32_t kIH_RB_CNTL__MC_VMID__SHIFT               = 0x18; // line 183
constexpr uint32_t kIH_RB_CNTL__MC_SPACE__SHIFT              = 0x1c; // line 184
constexpr uint32_t kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT   = 0x1f; // line 185

// IH_RB_WPTR fields (osssys_7_0_0_sh_mask.h:206-207). The HW puts
// the overflow flag in bit 0 of IH_RB_WPTR — NOT bit 31 of either
// IH_RB_WPTR or IH_RB_CNTL.
constexpr uint32_t kIH_RB_WPTR__RB_OVERFLOW__SHIFT           = 0x00; // line 206
constexpr uint32_t kIH_RB_WPTR__OFFSET__SHIFT                = 0x02; // line 207

// MC_SPACE values (ih_v7_0.c:192): 2 = bus_addr / sysmem ring,
// 4 = GPUVA. We always run with use_bus_addr=true on AS so the IH
// ring is DART-mapped sysmem and the engine should treat the
// programmed RB_BASE as a system bus address.
constexpr uint32_t kIH_RB_CNTL_MC_SPACE_BUS_ADDR = 2;
constexpr uint32_t kIH_RB_CNTL_MC_SPACE_GPUVA    = 4;

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
