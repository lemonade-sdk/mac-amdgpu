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
#include "amdgpu_vram_io.h"
#include "amdgpu_gmc_address.h"

#define GART_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.gart: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// GFX12 PTE physical addresses occupy bits 47:12 (gmc_v12_0.c).
static bool gart_bus_range(uint64_t address, uint64_t bytes)
{
    constexpr uint64_t limit = uint64_t(1) << 48;
    return bytes && !(address & 4095) && !(bytes & 4095) &&
        address < limit && bytes <= limit - address;
}

static bool gart_table_range(const DeviceContext &dev, const GARTContext &gart,
                              uint64_t offset, uint64_t bytes)
{
    return gart.enabled && gart.gmc && gart.allocator && bytes &&
        !(offset & 4095) && !(bytes & 4095) &&
        offset <= gart.gartSize && bytes <= gart.gartSize - offset &&
        offset / 4096 <= gart.pageTableSize / 8 &&
        bytes / 4096 <= gart.pageTableSize / 8 - offset / 4096 &&
        vram_io_range(dev, gart.pageTableVRAMOffset, gart.pageTableSize);
}

static kern_return_t gart_invalidate(DeviceContext &dev, const GARTContext &gart)
{
    if (!gart.gmc) return kIOReturnNotReady;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    amdgpu_hdp_flush(dev);
    auto r = gmc_flush_gpu_tlb(dev, *gart.gmc, gart.gmc->mmhub, 0, 0);
    if (r != kIOReturnSuccess) {
        GART_LOG("MMHUB invalidation failed: %#x — retaining mapping", r);
        return r;
    }
    r = gmc_flush_gpu_tlb(dev, *gart.gmc, gart.gmc->gfxhub, 0, 0);
    if (r != kIOReturnSuccess)
        GART_LOG("GFXHUB invalidation failed: %#x — retaining mapping", r);
    return r;
}

kern_return_t
gart_init(DeviceContext &dev, GMCContext &gmc, GARTContext &gart)
{
    gart.enabled = false;
    if (!gmc.inited || !gmc.mmhub.inited || !gmc.gfxhub.inited ||
        !gmc.gart_size || (gmc.gart_size & 4095) ||
        (gmc.gart_start & 4095) ||
        gmc.gart_size / 4096 > UINT32_MAX ||
        gmc.gart_pt_bus < gmc.vram_start ||
        gmc.gart_size / 4096 > gmc.gart_pt_size / 8 ||
        gmc.gart_start > UINT64_MAX - gmc.gart_size ||
        !vram_io_range(dev, gmc.gart_pt_bus - gmc.vram_start, gmc.gart_pt_size))
        return kIOReturnNotReady;

    if (!gmc.gart_allocator.init(gmc.gart_start, gmc.gart_size))
        return kIOReturnNotReady;
    gart.enabled = true;
    gart.pageTableVRAMOffset = gmc.gart_pt_bus - gmc.vram_start;
    gart.pageTableSize = gmc.gart_pt_size;
    gart.numPTEs = static_cast<uint32_t>(gmc.gart_size / 4096);
    gart.gartStart = gmc.gart_start;
    gart.gartEnd = gmc.gart_start + gmc.gart_size - 1;
    gart.gartSize = gmc.gart_size;
    gart.allocator = &gmc.gart_allocator;
    gart.gmc = &gmc;
    gart.reads_supported = false;
    gart.hostWindowConfigured = false;
    GART_LOG("init: aperture [%#llx..%#llx), %u PTEs; host-memory transfers unverified",
             gart.gartStart, gart.gartEnd + 1, gart.numPTEs);
    return kIOReturnSuccess;
}

kern_return_t
gart_configure_host_window(DeviceContext &dev, GARTContext &gart, uint64_t base)
{
    if (!gart.enabled || !gart.gmc || !gart.allocator) return kIOReturnNotReady;
    if (gart.hostWindowConfigured) return kIOReturnSuccess;
    auto &gmc = *gart.gmc;
    if (!gfx12_host_window_valid(base, gart.gartSize, gmc.fb_start, gmc.fb_end))
        return kIOReturnBadArgument;
    if (gart.allocator->bytes_used()) return kIOReturnBusy;
    // Mark the mutation before publishing registers. On any failure the caller
    // must quarantine this session; rolling back cannot prove GPU inactivity.
    gart.hostWindowConfigured = true;
    gart.reads_supported = false;
    gart.gartStart = gmc.gart_start = base;
    gart.gartEnd = gmc.gart_end = base + gart.gartSize - 1;
    gart.allocator->base = base; // no reservations exist; preserve generation IDs
    return gmc_program_gart_window(dev, gmc);
}

static kern_return_t gart_bind_range(DeviceContext &dev, GARTContext &gart,
                   uint64_t busAddr, uint64_t sizeBytes, uint64_t alignment, GARTBinding *binding)
{
    if (!binding || binding->sysmemBuffer || binding->dmaCommand)
        return kIOReturnBadArgument;
    if (!gart.enabled || !gart.allocator) return kIOReturnNotReady;
    if (!gart_bus_range(busAddr, sizeBytes) || alignment < 4096 ||
        (alignment & (alignment - 1))) return kIOReturnBadArgument;
    if (binding->owner || binding->numGPUPages) {
        if (binding->owner != &gart || !binding->ready ||
            binding->busAddr != busAddr || binding->sizeBytes != sizeBytes ||
            (binding->gartMCAddr & (alignment - 1)) ||
            !gart.allocator->owns(binding->reservationID, binding->gartOffset, sizeBytes))
            return kIOReturnBusy;
        return kIOReturnSuccess;
    }
    uint64_t offset;
    unsigned slot;
    if (!gart.allocator->find(sizeBytes, alignment, offset, slot) ||
        !gart_table_range(dev, gart, offset, sizeBytes)) return kIOReturnNoSpace;
    GARTReservation reservation{};
    if (!gart.allocator->reserve(sizeBytes, alignment, reservation)) return kIOReturnNoSpace;

    // Reserve and publish ownership before the first PTE write. A transport
    // or TLB timeout may leave a partially visible mapping; never recycle it.
    binding->owner = &gart;
    binding->reservationID = reservation.id;
    binding->busAddr = busAddr;
    binding->sizeBytes = sizeBytes;
    binding->gartOffset = offset;
    binding->gartMCAddr = gart.gartStart + offset;
    binding->numGPUPages = static_cast<uint32_t>(sizeBytes / 4096);
    binding->ready = false;
    GART_LOG("bind: DMA=%#llx bytes=%llu -> GPU=%#llx PT_offset=%#llx PTE_flags=%#llx",
        busAddr, sizeBytes, binding->gartMCAddr,
        gart.pageTableVRAMOffset + offset / 4096 * 8, PTEFlags::SYSMEM_RW);
    for (uint32_t i = 0; i < binding->numGPUPages; ++i) {
        const uint64_t pte = (busAddr + uint64_t(i) * 4096) | PTEFlags::SYSMEM_RW;
        const auto r = vram_write_verified(dev,
            gart.pageTableVRAMOffset + (offset / 4096 + i) * 8, &pte, sizeof(pte));
        if (r != kIOReturnSuccess) {
            GART_LOG("PTE upload/readback failed at page %u: %#x — retaining mapping", i, r);
            return r;
        }
    }
    const auto r = gart_invalidate(dev, gart);
    if (r != kIOReturnSuccess) return r;
    binding->ready = true;
    return kIOReturnSuccess;
}

kern_return_t
gart_bind_existing(DeviceContext &dev, GARTContext &gart,
                   uint64_t busAddr, uint64_t sizeBytes, GARTBinding *binding)
{
    return gart_bind_range(dev, gart, busAddr, sizeBytes, 4096, binding);
}

kern_return_t
gart_bind_sysmem(DeviceContext &dev, GARTContext &gart,
                 uint64_t sizeBytes, uint64_t alignment, GARTBinding *outBinding)
{
    if (!outBinding || outBinding->owner || outBinding->numGPUPages ||
        outBinding->sysmemBuffer || outBinding->dmaCommand) return kIOReturnBadArgument;
    if (!gart.enabled || !gart.allocator) return kIOReturnNotReady;
    if (alignment < kASPageSize) alignment = kASPageSize;
    if ((alignment & (alignment - 1)) || !sizeBytes ||
        sizeBytes > UINT64_MAX - (alignment - 1)) return kIOReturnBadArgument;
    const uint64_t rounded = (sizeBytes + alignment - 1) & ~(alignment - 1);
    uint64_t offset;
    unsigned slot;
    if (!gart.allocator->find(rounded, alignment, offset, slot) ||
        !gart_table_range(dev, gart, offset, rounded)) return kIOReturnNoSpace;

    IOBufferMemoryDescriptor *buf = nullptr;
    auto r = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn, rounded, alignment, &buf);
    if (r != kIOReturnSuccess || !buf) return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    r = buf->SetLength(rounded);
    if (r != kIOReturnSuccess) { buf->release(); return r; }
    IOAddressSegment cpu{};
    r = buf->GetAddressRange(&cpu);
    if (r != kIOReturnSuccess || !cpu.address || cpu.length < rounded) {
        buf->release();
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    IODMACommandSpecification spec{};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 48; // the GFX12 PTE format, not the CPU address width
    IODMACommand *dma = nullptr;
    r = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions, &spec, &dma);
    if (r != kIOReturnSuccess || !dma) {
        buf->release(); return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t count = 1;
    IOAddressSegment segment{};
    r = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
                           rounded, &flags, &count, &segment);
    if (r != kIOReturnSuccess) { dma->release(); buf->release(); return r; }
    if (count != 1 || segment.length < rounded || !gart_bus_range(segment.address, rounded)) {
        // Preserve a successfully prepared mapping if CompleteDMA fails.
        outBinding->sysmemBuffer = buf;
        outBinding->dmaCommand = dma;
        outBinding->dmaPrepared = true;
        const auto cleanup = gart_unbind(dev, gart, outBinding);
        return cleanup != kIOReturnSuccess ? cleanup : kIOReturnNotAligned;
    }
    r = gart_bind_range(dev, gart, segment.address, rounded, alignment, outBinding);
    outBinding->sysmemBuffer = buf;
    outBinding->dmaCommand = dma;
    outBinding->dmaPrepared = true;
    outBinding->cpuAddr = reinterpret_cast<void *>(cpu.address);
    if (r != kIOReturnSuccess && !outBinding->numGPUPages) {
        const auto cleanup = gart_unbind(dev, gart, outBinding);
        return cleanup != kIOReturnSuccess ? cleanup : r;
    }
    // Do not drop the buffer on a failed PTE upload or TLB invalidation.
    // The caller owns this result on failure as well as on success.
    return r;
}

kern_return_t
gart_unbind(DeviceContext &dev, GARTContext &gart, GARTBinding *binding)
{
    if (!binding) return kIOReturnBadArgument;
    if (binding->numGPUPages) {
        if (binding->owner != &gart || !gart.allocator ||
            !gart.allocator->owns(binding->reservationID, binding->gartOffset, binding->sizeBytes) ||
            binding->sizeBytes / 4096 != binding->numGPUPages ||
            !gart_table_range(dev, gart, binding->gartOffset, binding->sizeBytes))
            return kIOReturnBadArgument;
        binding->ready = false;
        // On GFX12 SYSTEM must also be clear for an invalid PTE.
        const auto r = vram_clear_verified(dev,
            gart.pageTableVRAMOffset + binding->gartOffset / 4096 * 8,
            uint64_t(binding->numGPUPages) * 8);
        if (r != kIOReturnSuccess) return r;
        const auto flushed = gart_invalidate(dev, gart);
        if (flushed != kIOReturnSuccess) return flushed;
    }
    if (binding->dmaCommand && binding->dmaPrepared) {
        const auto r = binding->dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        if (r != kIOReturnSuccess) return r;
        binding->dmaPrepared = false;
    }
    if (binding->dmaCommand) binding->dmaCommand->release();
    if (binding->sysmemBuffer) binding->sysmemBuffer->release();
    if (binding->numGPUPages)
        gart.allocator->release(binding->reservationID, binding->gartOffset, binding->sizeBytes);
    *binding = {};
    return kIOReturnSuccess;
}

void
gart_release_after_reset(GARTBinding &binding)
{
    // The caller has isolated the device; no GPU translation may be used now.
    if (binding.dmaCommand) {
        if (binding.dmaPrepared)
            binding.dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        binding.dmaCommand->release();
    }
    if (binding.sysmemBuffer) binding.sysmemBuffer->release();
    binding = {};
}

} // namespace amdgpu
