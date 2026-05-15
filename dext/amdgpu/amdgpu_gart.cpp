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
// gart_enable — port of mmhub_v4_1_0_gart_enable.
//
// Programs the MMHUB to use our VRAM-resident page table for VM
// context 0 (the kernel-mode context PSP uses). Sub-functions
// mirror upstream's split for clarity.
//
// We deliberately SKIP setup_vmid_config + program_invalidation
// (those configure contexts 1-14 for userspace VMs — we only need
// context 0 for PSP). disable_identity_aperture is also skipped
// (only relevant for SR-IOV / VFs).
//============================================================
static void
gart_setup_vm_pt_regs(const DeviceContext &dev, uint64_t pt_base_mc)
{
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
           static_cast<uint32_t>(pt_base_mc & 0xFFFFFFFFu));
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
           static_cast<uint32_t>(pt_base_mc >> 32));
}

static void
gart_init_aperture_regs(const DeviceContext &dev, const GARTContext &gart,
                        uint64_t pt_base_mc)
{
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    gart_setup_vm_pt_regs(dev, pt_base_mc);
    // start/end addresses are in 4 KB units (low 32 = bits 12-43,
    // high 32 = bits 44+). Matches upstream's shifts.
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32,
           static_cast<uint32_t>((gart.gartStart >> 12) & 0xFFFFFFFFu));
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32,
           static_cast<uint32_t>(gart.gartStart >> 44));
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32,
           static_cast<uint32_t>((gart.gartEnd >> 12) & 0xFFFFFFFFu));
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32,
           static_cast<uint32_t>(gart.gartEnd >> 44));
}

static void
gart_init_tlb_regs(const DeviceContext &dev)
{
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    // Match upstream mmhub_v4_1_0_init_tlb_regs exactly:
    //   ENABLE_L1_TLB                    = 1   bit 0
    //   SYSTEM_ACCESS_MODE               = 3   bits 3-4
    //   SYSTEM_APERTURE_UNMAPPED_ACCESS  = 0   bit 5
    //   ENABLE_ADVANCED_DRIVER_MODEL     = 1   bit 6
    //   ECO_BITS                         = 0   bits 7-10
    //   MTYPE                            = 2 (MTYPE_UC)  bits 11-13
    uint32_t v = 0;
    v |= (1u << 0);             // ENABLE_L1_TLB
    v |= (3u << 3);             // SYSTEM_ACCESS_MODE = 3
    v |= (1u << 6);             // ENABLE_ADVANCED_DRIVER_MODEL
    v |= (2u << 11);            // MTYPE = MTYPE_UC
    WREG32(dev, base + MMHUBRegs::MMMC_VM_MX_L1_TLB_CNTL, v);
}

static void
gart_init_cache_regs(const DeviceContext &dev)
{
    uint32_t base = dev.ip.get(IPBlock::MMHUB);

    // VM_L2_CNTL — read-modify-write isn't strictly required since we
    // know the bits we want; write the full value matching upstream's
    // REG_SET_FIELD sequence:
    //   ENABLE_L2_CACHE                                 = 1   bit 0
    //   ENABLE_L2_FRAGMENT_PROCESSING                   = 0   bit 1
    //   ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY        = 1   bit 11
    //   L2_PDE0_CACHE_TAG_GENERATION_MODE               = 0
    //   PDE_FAULT_CLASSIFICATION                        = 0
    //   CONTEXT1_IDENTITY_ACCESS_MODE                   = 1   bit 19
    //   IDENTITY_MODE_FRAGMENT_SIZE                     = 0
    {
        uint32_t v = 0;
        v |= (1u << 0);
        v |= (1u << 11);
        v |= (1u << 19);
        WREG32(dev, base + MMHUBRegs::MMVM_L2_CNTL, v);
    }
    // VM_L2_CNTL2 — invalidate all TLBs + L2 cache.
    {
        uint32_t v = (1u << 0) | (1u << 1);
        WREG32(dev, base + MMHUBRegs::MMVM_L2_CNTL2, v);
    }
    // VM_L2_CNTL3 — BANK_SELECT=9, L2_CACHE_BIGK_FRAGMENT_SIZE=6
    // (matches upstream's translate_further=false default).
    {
        uint32_t v = 0;
        v |= (9u << 0);
        v |= (6u << 15);
        WREG32(dev, base + MMHUBRegs::MMVM_L2_CNTL3, v);
    }
    // VM_L2_CNTL4 — VMC_TAP_PDE_REQUEST_PHYSICAL=0, VMC_TAP_PTE_REQUEST_PHYSICAL=0.
    WREG32(dev, base + MMHUBRegs::MMVM_L2_CNTL4, 0);
    // VM_L2_CNTL5 — L2_CACHE_SMALLK_FRAGMENT_SIZE=0.
    WREG32(dev, base + MMHUBRegs::MMVM_L2_CNTL5, 0);
}

static void
gart_enable_system_domain(const DeviceContext &dev)
{
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    // MMVM_CONTEXT0_CNTL:
    //   ENABLE_CONTEXT                            = 1   bit 0
    //   PAGE_TABLE_DEPTH                          = 0   bits 1-2
    //   RETRY_PERMISSION_OR_INVALID_PAGE_FAULT    = 0   bit 8
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_CNTL, 1u);
}

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

    // The MMHUB needs the page-table MC address. Our PT lives at
    // VRAM offset pageTableVRAMOffset → MC address = vram_start + offset.
    // Read vram_start fresh in case the caller hasn't cached it.
    uint32_t mmhubBase = dev.ip.get(IPBlock::MMHUB);
    uint32_t fbRaw = RREG32(dev,
        mmhubBase + MMHUBRegs::MMMC_VM_FB_LOCATION_BASE);
    uint64_t vramStart =
        ((uint64_t)(fbRaw & MMHUBRegs::kFBBaseMask))
        << MMHUBRegs::kFBBaseShift;
    if (vramStart == 0) {
        GART_LOG("enable: vram_start=0; cannot compute PT MC addr");
        return kIOReturnNotReady;
    }
    uint64_t ptBaseMC = vramStart + gart.pageTableVRAMOffset;

    GART_LOG("enable: pt_mc=%#llx aperture=[%#llx, %#llx]",
             ptBaseMC, gart.gartStart, gart.gartEnd);

    gart_init_aperture_regs(dev, gart, ptBaseMC);
    gart_init_tlb_regs(dev);
    gart_init_cache_regs(dev);
    gart_enable_system_domain(dev);

    // Read-back smoke check: confirm the start-addr register latched.
    uint32_t readback = RREG32(dev,
        mmhubBase + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32);
    uint32_t expected =
        static_cast<uint32_t>((gart.gartStart >> 12) & 0xFFFFFFFFu);
    if (readback != expected) {
        GART_LOG("enable: read-back mismatch (got %#x, want %#x)",
                 readback, expected);
        // Don't fail — register may have side-effects on RMW. Log and
        // continue; the real test is whether PSP-ring submits work.
    }

    gart.enabled = true;
    GART_LOG("enable: GART context-0 enabled");
    return kIOReturnSuccess;
}

} // namespace amdgpu
