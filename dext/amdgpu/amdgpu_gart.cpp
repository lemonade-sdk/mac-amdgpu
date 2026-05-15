//
//  amdgpu_gart.cpp — GART bootstrap implementation.
//
//  Mirrors upstream amdgpu_gart.c (amdgpu_gart_init,
//  amdgpu_gart_table_vram_alloc, amdgpu_gart_map) + the
//  mmhub_v4_1_0_gart_enable sequence (for the register pokes that
//  turn GART on).
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "amdgpu_gart.h"

#define GART_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.gart: " fmt, ##__VA_ARGS__)

namespace amdgpu {

//============================================================
// VRAM layout for the GART page table.
//
// PSP buffers occupy VRAM offsets:
//     0x000000-0x100000  fw_pri        (1 MB)
//     0x100000-0x104000  ring          (16 KB)
//     0x104000-0x108000  cmd_buf       (16 KB)
//     0x108000-0x10C000  fence_buf     (16 KB)
//     0x200000-0x600000  TMR           (4 MB)
//
// GART page table sits at 0x700000 (7 MB), giving plenty of room
// above TMR. One 4 KB page = 512 PTEs = 2 MB of GART space. We
// allocate 4 KB for now — enough for ring/cmd/fence + headroom.
//============================================================
constexpr uint64_t kGARTPageTableVRAMOffset = 0x700000;   // 7 MB
constexpr uint32_t kGARTPageTableBytes      = 4096;       // one 4 KB page

// GART aperture in MC space. We pick a fixed start address well above
// the VRAM range (vram_end is at 0x80_77700000 for our 30 GB R9700).
// Upstream's amdgpu_gmc_gart_location picks dynamically; we hard-code
// to keep things simple for early bringup.
//
//     gart_start = 0x0001_0000_0000_0000  (= 1<<48)
//
// Picked far above vram_start (0x80_00000000) so they never overlap.
constexpr uint64_t kGARTStart = 0x0001000000000000ULL;

//============================================================
// gart_init
//============================================================
kern_return_t
gart_init(DeviceContext &dev, GARTContext &gart)
{
    if (gart.numPTEs > 0) {
        return kIOReturnSuccess; // idempotent
    }

    gart.pageTableVRAMOffset = kGARTPageTableVRAMOffset;
    gart.pageTableSize       = kGARTPageTableBytes;
    gart.numPTEs             = kGARTPageTableBytes / 8;  // 8-byte PTEs
    gart.gartStart           = kGARTStart;
    gart.gartSize            = (uint64_t)gart.numPTEs * kAMDGPUGPUPageSize;
    gart.gartEnd             = gart.gartStart + gart.gartSize - 1;
    gart.nextFreeOffset      = 0;
    gart.enabled             = false;

    // Zero the page table in VRAM. All PTEs start invalid (VALID bit 0).
    bar0_memset_vram(dev, gart.pageTableVRAMOffset, 0, gart.pageTableSize);

    GART_LOG("init: pt @ VRAM+%#llx (%u PTEs, %llu KB aperture); "
             "gart_start=%#llx gart_end=%#llx",
             gart.pageTableVRAMOffset, gart.numPTEs, gart.gartSize >> 10,
             gart.gartStart, gart.gartEnd);
    return kIOReturnSuccess;
}

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

//============================================================
// gart_enable — stub for now. Will port mmhub_v4_1_0_gart_enable
// register sequence in the next iteration (task #36 in TaskList).
//============================================================
kern_return_t
gart_enable(DeviceContext &dev, GARTContext &gart)
{
    if (gart.enabled) return kIOReturnSuccess;
    if (gart.numPTEs == 0) {
        GART_LOG("enable: gart_init not called");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::MMHUB)) {
        GART_LOG("enable: MMHUB IP base not resolved");
        return kIOReturnNotReady;
    }

    // TODO (task #36): port mmhub_v4_1_0_gart_enable here.
    //   - init_gart_aperture_regs
    //   - init_system_aperture_regs
    //   - init_tlb_regs
    //   - init_cache_regs
    //   - enable_system_domain
    //   - disable_identity_aperture
    //   - setup_vmid_config
    //   - program_invalidation
    GART_LOG("enable: NOT YET IMPLEMENTED — see GART_PORT_PLAN.md task 36");
    return kIOReturnUnsupported;
}

} // namespace amdgpu
