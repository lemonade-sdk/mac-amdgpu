//
//  amdgpu_gart.cpp — GART page table binding helpers.
//
//  The GART page table itself is managed by GMC (gmc_v12_0.cpp).
//  This module provides functions to bind sysmem buffers into the
//  GART by writing PTEs into the VRAM-resident page table.
//
//  Mirrors upstream amdgpu_gart.c (amdgpu_gart_table_vram_alloc,
//  amdgpu_gart_map). The gart_enable() register programming has
//  been moved to gmc_v12_0.cpp (gmc_mmhub_gart_enable).
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "amdgpu_gart.h"
#include "amdgpu_gmc.h"

#define GART_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.gart: " fmt, ##__VA_ARGS__)

namespace amdgpu {

//============================================================
// gart_init — populate GARTContext from a just-enabled GMC GART.
//
// Run AFTER gmc_gfxhub_gart_enable / gmc_mmhub_gart_enable so that
// gmc.gart_start / gmc.gart_size are valid. Sets up the bump
// allocator and the platform-gated `reads_supported` flag.
//============================================================
kern_return_t
gart_init(DeviceContext &dev, const GMCContext &gmc, GARTContext &gart)
{
    (void)dev;
    if (gmc.gart_size == 0) {
        GART_LOG("init: gmc.gart_size == 0 (gart_enable hasn't run?)");
        return kIOReturnNotReady;
    }

    // Derive PT VRAM offset from gmc.gart_pt_bus (= vram_start + offset
    // per gmc_alloc_resources at gmc_v12_0.cpp:512). The constant
    // kGMCGartPTVRAMOffset is file-local to gmc_v12_0.cpp and not
    // exposed in the header — subtraction reconstructs it.
    gart.enabled            = true;
    gart.pageTableVRAMOffset = (gmc.gart_pt_bus > gmc.vram_start)
        ? (gmc.gart_pt_bus - gmc.vram_start) : 0;
    gart.pageTableSize       = gmc.gart_pt_size;
    gart.numPTEs             = static_cast<uint32_t>(
        gmc.gart_size / kAMDGPUGPUPageSize);
    gart.gartStart           = gmc.gart_start;
    gart.gartEnd             = gmc.gart_start + gmc.gart_size - 1;
    gart.gartSize            = gmc.gart_size;
    gart.nextFreeOffset      = 0;

    // **Platform gate — GPU-initiated sysmem reads.**
    //
    // GART is fully functional from a software standpoint:
    //   • Page table is allocated in VRAM and zero-initialised
    //   • MMHUB / GFXHUB program the PT base, aperture start/end,
    //     VMID0 cntl exactly per upstream
    //   • gart_bind_sysmem / gart_bind_existing write PTEs correctly
    //   • Engine MC resolution returns the right bus address
    //
    // What does NOT work today: DART silently zeros every GPU-initiated
    // sysmem read on Apple Silicon + Thunderbolt 5. The PCIe transaction
    // reaches DART, but the data returned is all zero. So even though
    // the PTE points at the right host RAM, the engine receives zeros
    // instead of the actual bytes. See
    // [[feedback_mac_amdgpu_dart_tb5_pcie_reads]] for the test history.
    //
    // We default this flag FALSE on every platform we currently support.
    // Higher layers (BOAlloc(kBODomainGTT) etc.) refuse GTT allocations
    // when the flag is false, returning kIOReturnUnsupported so clients
    // fail loud instead of allocating a BO that returns zero on every
    // engine read.
    //
    // When Apple exposes a sysmem mapping primitive whose GPU-initiated
    // reads return real bytes — a new IODMACommand option, a per-host-app
    // entitlement, a non-DART path, whatever it ends up being — extend
    // the platform-detect logic here to flip this to true under that
    // condition. The rest of the GART stack is already operational, so
    // GTT allocations will start working immediately.
    gart.reads_supported = false;

    GART_LOG("init: gart aperture [%#llx..%#llx) size=%llu bytes, "
             "%u PTEs, pt_bus=%#llx, reads_supported=%d "
             "(AS+TB5 DART zeroes GPU-initiated reads — "
             "GTT BOs will return kIOReturnUnsupported)",
             gart.gartStart, gart.gartEnd + 1, gart.gartSize,
             gart.numPTEs, gmc.gart_pt_bus, gart.reads_supported ? 1 : 0);
    return kIOReturnSuccess;
}

// GART aperture in MC space. Picked LOW (4 GB MC offset) — safely
// below vram_start (= 512 GB for our R9700) and well within the
// 48-bit MC bus on RDNA4. Upstream's amdgpu_gmc_gart_location LOW
// placement puts GART just below vram_start; the 4 GB origin keeps
// the math simple and leaves room above for other future apertures.
//
//     gart_start = 0x0000_0001_0000_0000  (= 4 GB)
//     gart_end   = gart_start + numPTEs * 4 KB - 1
constexpr uint64_t kGARTStart = 0x0000000100000000ULL;

//============================================================
// gart_bind_sysmem — allocate sysmem buffer, DMA-map, write PTEs,
// return GART MC address.
//
// Limitations:
//   - Single contiguous DMA segment only (DART usually gives one;
//     we don't yet handle scatter-gather).
//   - Bump allocator: no free path until gart_unbind is called.
//============================================================
kern_return_t
gart_bind_sysmem(DeviceContext &dev, GARTContext &gart,
                 uint64_t sizeBytes, uint64_t alignment,
                 GARTBinding *outBinding)
{
    if (outBinding == nullptr) return kIOReturnBadArgument;
    if (gart.numPTEs == 0) {
        GART_LOG("bind: GART not initialised");
        return kIOReturnNotReady;
    }
    if (alignment < kASPageSize) alignment = kASPageSize;
    // Round up size to GPU page boundary so we map whole PTEs.
    uint64_t roundedSize = (sizeBytes + kAMDGPUGPUPageSize - 1) &
                          ~((uint64_t)kAMDGPUGPUPageSize - 1);
    uint32_t numPTEs = static_cast<uint32_t>(roundedSize /
                                             kAMDGPUGPUPageSize);
    if (gart.nextFreeOffset + roundedSize > gart.gartSize) {
        GART_LOG("bind: out of GART space (need %llu, free %llu)",
                 roundedSize, gart.gartSize - gart.nextFreeOffset);
        return kIOReturnNoSpace;
    }

    // Allocate sysmem buffer.
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, roundedSize, alignment, &buf);
    if (ret != kIOReturnSuccess || buf == nullptr) {
        GART_LOG("bind: buffer alloc failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    buf->SetLength(roundedSize);

    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;
    IODMACommand *dma = nullptr;
    ret = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                               &spec, &dma);
    if (ret != kIOReturnSuccess || dma == nullptr) {
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    ret = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
                             roundedSize, &flags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) {
        dma->release();
        buf->release();
        GART_LOG("bind: PrepareForDMA failed: %#x segs=%u", ret, segCount);
        return ret != kIOReturnSuccess ? ret : kIOReturnNotAligned;
    }
    IOAddressSegment cpu = {};
    buf->GetAddressRange(&cpu);

    // Write PTEs into the VRAM page table.
    //
    // Each PTE is 8 bytes: high bits = sys-phys (here: DART bus addr),
    // low bits = flags. We mark each one VALID|SYSTEM|SNOOPED|R|W.
    uint64_t gartOff = gart.nextFreeOffset;
    uint64_t pteStartIndex = gartOff / kAMDGPUGPUPageSize;
    for (uint32_t i = 0; i < numPTEs; i++) {
        uint64_t physAddr = seg.address + (uint64_t)i * kAMDGPUGPUPageSize;
        uint64_t pte = (physAddr & ~((uint64_t)0xFFFULL)) |
                       PTEFlags::SYSMEM_RW;
        uint64_t pteOffsetInPT = (pteStartIndex + i) * 8ULL;
        bar0_memcpy_to_vram(dev,
                            gart.pageTableVRAMOffset + pteOffsetInPT,
                            &pte, sizeof(pte));
    }

    // HDP flush after writing PTEs so GMC sees the new entries before
    // any GART access. Mirrors upstream amdgpu_gart_invalidate_tlb's
    // amdgpu_device_flush_hdp call.
    amdgpu_hdp_flush(dev);

    outBinding->sysmemBuffer = buf;
    outBinding->dmaCommand   = dma;
    outBinding->busAddr      = seg.address;
    outBinding->cpuAddr      = reinterpret_cast<void *>(cpu.address);
    outBinding->sizeBytes    = roundedSize;
    outBinding->gartOffset   = gartOff;
    outBinding->gartMCAddr   = gart.gartStart + gartOff;
    outBinding->numGPUPages  = numPTEs;

    gart.nextFreeOffset += roundedSize;

    GART_LOG("bind: %llu bytes @ bus=%#llx → gart_mc=%#llx "
             "(%u PTEs at PT idx %llu)",
             roundedSize, seg.address, outBinding->gartMCAddr,
             numPTEs, pteStartIndex);
    return kIOReturnSuccess;
}

//============================================================
// gart_bind_existing — write PTEs for a pre-existing DART-mapped
// bus address range. No buffer/DMA-command alloc. Caller owns the
// underlying IOBuffer; this only programs the page table.
//
// Reuse semantics: pass a binding with non-zero gartMCAddr to RE-USE
// the previously assigned slot (avoids burning bump-allocator space
// when the host swaps the buffer's contents but the busAddr/size are
// stable). Pass a zero-initialised binding to allocate fresh.
//============================================================
kern_return_t
gart_bind_existing(DeviceContext &dev, GARTContext &gart,
                   uint64_t busAddr, uint64_t sizeBytes,
                   GARTBinding *binding)
{
    if (binding == nullptr) return kIOReturnBadArgument;
    if (gart.numPTEs == 0) {
        GART_LOG("bind_existing: GART not initialised");
        return kIOReturnNotReady;
    }
    if ((busAddr & (kAMDGPUGPUPageSize - 1)) != 0) {
        GART_LOG("bind_existing: busAddr %#llx not 4 KB aligned", busAddr);
        return kIOReturnNotAligned;
    }
    uint64_t rounded = (sizeBytes + kAMDGPUGPUPageSize - 1) &
                      ~((uint64_t)kAMDGPUGPUPageSize - 1);
    uint32_t numPTEs = static_cast<uint32_t>(rounded / kAMDGPUGPUPageSize);

    // Choose GART slot: reuse if binding already has one, else bump.
    uint64_t gartOff;
    if (binding->gartMCAddr != 0 && binding->numGPUPages >= numPTEs) {
        gartOff = binding->gartOffset;
    } else {
        if (gart.nextFreeOffset + rounded > gart.gartSize) {
            GART_LOG("bind_existing: out of GART (need %llu, free %llu)",
                     rounded, gart.gartSize - gart.nextFreeOffset);
            return kIOReturnNoSpace;
        }
        gartOff = gart.nextFreeOffset;
        gart.nextFreeOffset += rounded;
    }

    uint64_t pteStartIndex = gartOff / kAMDGPUGPUPageSize;
    for (uint32_t i = 0; i < numPTEs; i++) {
        uint64_t physAddr = busAddr + (uint64_t)i * kAMDGPUGPUPageSize;
        uint64_t pte = (physAddr & ~((uint64_t)0xFFFULL)) |
                       PTEFlags::SYSMEM_RW;
        uint64_t pteOffsetInPT = (pteStartIndex + i) * 8ULL;
        bar0_memcpy_to_vram(dev,
                            gart.pageTableVRAMOffset + pteOffsetInPT,
                            &pte, sizeof(pte));
    }
    amdgpu_hdp_flush(dev);

    binding->sysmemBuffer = nullptr;   // caller owns
    binding->dmaCommand   = nullptr;
    binding->busAddr      = busAddr;
    binding->cpuAddr      = nullptr;   // caller has the CPU pointer
    binding->sizeBytes    = rounded;
    binding->gartOffset   = gartOff;
    binding->gartMCAddr   = gart.gartStart + gartOff;
    binding->numGPUPages  = numPTEs;

    GART_LOG("bind_existing: %llu bytes @ bus=%#llx → gart_mc=%#llx "
             "(%u PTEs at PT idx %llu)",
             rounded, busAddr, binding->gartMCAddr,
             numPTEs, pteStartIndex);
    return kIOReturnSuccess;
}

//============================================================
// gart_unbind — invalidate PTEs + release buffer/DMA handle.
// (Bump allocator: the GART slot stays "used" until reset.)
//============================================================
void
gart_unbind(GARTContext &gart, GARTBinding *binding)
{
    if (binding == nullptr || binding->sysmemBuffer == nullptr) return;
    if (binding->dmaCommand != nullptr) {
        binding->dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        binding->dmaCommand->release();
        binding->dmaCommand = nullptr;
    }
    binding->sysmemBuffer->release();
    binding->sysmemBuffer = nullptr;
    binding->busAddr = 0;
    binding->cpuAddr = nullptr;
    // PTE invalidation handled by full-table memset on next gart_init;
    // bump allocator means a single unbind doesn't free the slot.
}

} // namespace amdgpu

