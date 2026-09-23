//
//  amdgpu_gart.h — GART (Graphics Address Remapping Table) binding helpers.
//
//  The GART page table itself is managed by GMC (gmc_v12_0.cpp).
//  This module provides functions to bind sysmem buffers into the
//  GART by writing PTEs into the VRAM-resident page table.
//
//  Layout on RDNA4:
//      - 4 KB GPU page granularity (regardless of host CPU page size — note
//        Apple Silicon CPU is 16 KB pages, so each CPU page = 4 GPU PTEs).
//      - PTE is 8 bytes: high bits = host phys addr (DART bus on AS), low
//        bits = flags (VALID, SYSTEM, R/W, etc.).
//
//  Mirrors upstream `amdgpu_gart.c` (amdgpu_gart_table_vram_alloc,
//  amdgpu_gart_map). The gart_enable() register programming has been
//  moved to gmc_v12_0.cpp (gmc_mmhub_gart_enable).
//

#pragma once

#include <stdint.h>
#ifdef __APPLE__
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#endif

#include "amdgpu_regs.h"
#include "amdgpu_ip.h"
#include "amdgpu_gart_allocator.h"

namespace amdgpu {

struct GARTContext;
struct GMCContext;

// One BO bound into GART. Tracks the sysmem buffer + the GART MC address
// PSP (or any other GPU IP) should use to reach it.
struct GARTBinding {
#ifdef __APPLE__
    IOBufferMemoryDescriptor *sysmemBuffer; // sysmem buffer being bound
    IODMACommand             *dmaCommand;   // DART-mapped DMA handle
#endif
    uint64_t  busAddr;       // DART-mapped bus address (1 segment for now)
    void     *cpuAddr;       // CPU pointer for writes/reads
    uint64_t  sizeBytes;
    uint64_t  reservationID; // rejects stale/copy unbind after range reuse
    uint64_t  gartOffset;    // byte offset into the GART aperture
    uint64_t  gartMCAddr;    // gart_start + gartOffset — what PSP uses
    uint32_t  numGPUPages;   // number of 4 KB PTEs used
    GARTContext *owner;
    bool ready;             // PTE readback and both hub invalidations succeeded
    bool dmaPrepared;
};

struct GARTContext {
    bool        enabled;

    // Page-table storage: in VRAM. Accessed CPU-side via the BAR0
    // aperture using bar0_memcpy_to_vram / bar0_memset_vram (see
    // amdgpu_regs.h). One 4 KB page = 512 PTEs = 2 MB of GART space.
    uint64_t    pageTableVRAMOffset;  // VRAM offset of the table
    uint64_t    pageTableSize;        // bytes (one page = 4096 = 512 PTEs)
    uint32_t    numPTEs;              // pageTableSize / 8

    // GART address space layout in MC space.
    uint64_t    gartStart;       // MC address where GART aperture begins
    uint64_t    gartEnd;         // gartStart + (numPTEs * GPU_PAGE_SIZE) - 1
    uint64_t    gartSize;        // numPTEs * GPU_PAGE_SIZE

    // GMC owns the shared range allocator, including firmware reservations.
    GARTApertureAllocator *allocator;
    GMCContext *gmc;

    // GTT allocations stay gated until a data-verified GPU host-memory
    // read passes. This is independent of BAR2 doorbell delivery.
    bool        reads_supported;
};

//
// gart_bind_sysmem — allocate a sysmem buffer, DART-map it, bind into
// GART, return the GART MC address to pass to PSP. The IODMACommand is
// stashed in the GARTBinding so we can release it later.
//
// alignment must be a power of two; allocations round to at least 16 KiB.
// On a mapping/cleanup failure, outBinding can retain resources. The caller
// must keep it alive until gart_unbind succeeds or verified session reset.
//
kern_return_t gart_bind_sysmem(DeviceContext &dev, GARTContext &gart,
                               uint64_t sizeBytes, uint64_t alignment,
                               GARTBinding *outBinding);

//
// Caller must first retire every GPU job referencing this binding. Invalidates
// PTEs and both hub TLBs before completing DMA and releasing owned storage.
// Failure retains storage/reservation for retry or verified reset. Successful
// teardown reclaims the range regardless of allocation order.
//
kern_return_t gart_unbind(DeviceContext &dev, GARTContext &gart, GARTBinding *binding);
// Only after verified GPU reset/PCI isolation; performs no GPU register access.
void gart_release_after_reset(GARTBinding &binding);

//
// gart_bind_existing — bind an EXISTING bus address range into GART.
// Used when the host's DMA buffer is already DART-mapped (e.g. the
// shared firmware-staging buffer the user client owns) and we just
// need PSP to be able to read it via a GMC MC address.
//
// Writes PTEs at the next free GART slot. The caller retains ownership of the
// DMA mapping, including on failure if binding.numGPUPages is nonzero. Retain
// that mapping until unbind or verified reset. Ranges must cover whole 4 KiB
// pages. Zero is a valid GART MC address.
//
// An acknowledged identical binding is idempotent. Replacing a live or failed
// binding requires unbind first; pass a zero-init binding to allocate fresh.
//
kern_return_t gart_bind_existing(DeviceContext &dev, GARTContext &gart,
                                 uint64_t busAddr, uint64_t sizeBytes,
                                 GARTBinding *binding);

//
// gart_init — populate GARTContext from the just-enabled GMC GART
// aperture. Run AFTER gmc_gfxhub_gart_enable / gmc_mmhub_gart_enable
// (so gmc.gart_start, gmc.gart_size, gmc.gart_pt_bus are valid). After
// this returns, gart_bind_sysmem / gart_bind_existing / gart_unbind
// are operational.
//
// GTT BO allocations remain gated by reads_supported until data-verified
// GPU host-memory transfers pass. Prior failures do not distinguish DART
// behavior from incomplete GPU page-table programming or engine addressing.
// This facade shares GMC's range allocator; reinitializing it does not
// clear the page table or reclaim bindings. Teardown/invalidation must be
// completed before general GTT allocation can be enabled.
//
kern_return_t gart_init(DeviceContext &dev, GMCContext &gmc,
                        GARTContext &gart);

} // namespace amdgpu
