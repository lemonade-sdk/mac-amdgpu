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

namespace amdgpu {

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
    uint64_t  gartOffset;    // dword offset into the GART aperture
    uint64_t  gartMCAddr;    // gart_start + gartOffset — what PSP uses
    uint32_t  numGPUPages;   // number of 4 KB PTEs used
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

    // Bump-allocator state — next free GART offset (in bytes).
    uint64_t    nextFreeOffset;

    // Platform gate: are GPU-initiated reads through GART → DART → sysmem
    // actually returning real bytes? On Apple Silicon + TB5 the answer is
    // currently NO — see [[feedback_mac_amdgpu_dart_tb5_pcie_reads]].
    // GART itself is fully programmed (PTEs get written correctly, MC
    // resolution works), but DART silently zeros every GPU-initiated read
    // of mapped sysmem. The PTE points at the right host RAM, the engine
    // just never receives the actual data.
    //
    // We keep this defaulting to FALSE on the dext's current platforms.
    // Higher layers gate GTT BO allocations on this flag and return
    // kIOReturnUnsupported when it's false, so clients fail fast instead
    // of silently allocating a BO that returns zeros on every read.
    //
    // When Apple exposes a sysmem mapping primitive whose GPU-initiated
    // reads return real data (a new IODMACommand option, an entitlement,
    // a non-DART path — whatever it ends up being), the platform-detect
    // code in gart_init / gart_post_enable can flip this to true and the
    // GTT path automatically becomes a first-class allocation domain.
    // No other driver changes needed; the rest of the GART stack is
    // already operational.
    bool        reads_supported;
};

//
// gart_bind_sysmem — allocate a sysmem buffer, DART-map it, bind into
// GART, return the GART MC address to pass to PSP. The IODMACommand is
// stashed in the GARTBinding so we can release it later.
//
// alignment must be a multiple of kASPageSize (16 KB) for DART to accept
// the mapping.
//
kern_return_t gart_bind_sysmem(DeviceContext &dev, GARTContext &gart,
                               uint64_t sizeBytes, uint64_t alignment,
                               GARTBinding *outBinding);

//
// gart_unbind — release a binding (CompleteDMA, release buffer, mark
// PTEs invalid).
//
void gart_unbind(GARTContext &gart, GARTBinding *binding);

//
// gart_bind_existing — bind an EXISTING bus address range into GART.
// Used when the host's DMA buffer is already DART-mapped (e.g. the
// shared firmware-staging buffer the user client owns) and we just
// need PSP to be able to read it via a GMC MC address.
//
// Writes PTEs at the next free GART slot. The caller retains ownership
// of the underlying IOBufferMemoryDescriptor / IODMACommand — this
// function doesn't take a reference. PTEs stay live until the GART is
// reset (GMC re-zero's the page table) or the binding is
// overwritten by another bind at the same offset.
//
// Idempotent across re-binds of the same buffer: if the same busAddr/
// size is re-bound, you can pass the previous binding back in to reuse
// its `gartOffset` (avoids bumping the allocator); pass a zero-init
// binding to allocate a fresh slot.
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
// Also sets `gart.reads_supported` based on platform detection. On
// AS+TB5 today this stays false — DART zeros every GPU-initiated
// sysmem read (see [[feedback_mac_amdgpu_dart_tb5_pcie_reads]]). Higher
// layers gate GTT BO allocations on this flag and fail fast.
//
// Future: when Apple exposes a working sysmem-mapping primitive,
// extend the platform-detect block here to set reads_supported=true
// under that condition. No other GART code needs to change.
//
struct GMCContext;
kern_return_t gart_init(DeviceContext &dev, const GMCContext &gmc,
                        GARTContext &gart);

} // namespace amdgpu
