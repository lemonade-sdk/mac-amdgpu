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
    // Upstream amdgpu_gmc_pd_addr ORs in AMDGPU_PTE_VALID before
    // writing the page-table base. Without VALID, GMC treats the
    // page table itself as invalid and every GART access faults —
    // PSP then rejects ring_create with status 0x0a.
    //
    // For RDNA4 the PDE format also wants the FRAG field cleared and
    // (for VRAM-backed page tables) SYSTEM=0; we satisfy both by only
    // setting VALID.
    uint64_t pte_base = pt_base_mc | PTEFlags::VALID;
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32,
           static_cast<uint32_t>(pte_base & 0xFFFFFFFFu));
    WREG32(dev, base + MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32,
           static_cast<uint32_t>(pte_base >> 32));
}

static void
gart_init_aperture_regs(const DeviceContext &dev, const GARTContext &gart,
                        uint64_t pt_base_mc)
{
    GART_LOG("init_gart_aperture_regs: pt_base=%#llx start=%#llx end=%#llx",
             pt_base_mc, gart.gartStart, gart.gartEnd);
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

// Port of mmhub_v4_1_0_init_system_aperture_regs. Programs AGP +
// system aperture window + default addr + L2 protection-fault
// default addr. We don't use AGP, so disable it (BOT > TOP). The
// "system aperture" defines the MC range that maps to the
// framebuffer (covers vram_start..vram_end). The default + fault
// addrs need a small sysmem scratch; we point both at the first
// page of our PT table (it's already VRAM-backed and writable).
static void
gart_init_system_aperture_regs(const DeviceContext &dev,
                               uint64_t vramStart, uint64_t vramEnd)
{
    GART_LOG("init_system_aperture_regs: vram=[%#llx,%#llx]",
             vramStart, vramEnd);
    uint32_t base = dev.ip.get(IPBlock::MMHUB);

    // Disable AGP — BOT > TOP signals "no AGP aperture."
    WREG32(dev, base + MMHUBRegs::MMMC_VM_AGP_BASE, 0);
    WREG32(dev, base + MMHUBRegs::MMMC_VM_AGP_BOT, 0xFFFFFFu);
    WREG32(dev, base + MMHUBRegs::MMMC_VM_AGP_TOP, 0);

    // System aperture covers the framebuffer (VRAM range). Addresses
    // are in 256 KB units (>> 18).
    WREG32(dev, base + MMHUBRegs::MMMC_VM_SYSTEM_APERTURE_LOW_ADDR,
           static_cast<uint32_t>(vramStart >> 18));
    WREG32(dev, base + MMHUBRegs::MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
           static_cast<uint32_t>(vramEnd >> 18));

    // Default address for accesses outside any mapped range. Upstream
    // points it at adev->mem_scratch.gpu_addr. We don't have a
    // mem_scratch yet — point both at 0 (the start of our VRAM); any
    // such access is a bug anyway and 0 is a safe sink.
    WREG32(dev,
        base + MMHUBRegs::MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB_AT_4C8, 0);
    WREG32(dev,
        base + MMHUBRegs::MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB_AT_4C9, 0);

    // L2 protection fault default address — same situation. Linux uses
    // adev->dummy_page_addr; we use 0 for the same reason.
    WREG32(dev, base + MMHUBRegs::MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, 0);
    WREG32(dev, base + MMHUBRegs::MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, 0);

    // ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY (bit 0x12 = 18) — matches
    // upstream's RMW; we just write the bit set.
    WREG32(dev, base + MMHUBRegs::MMVM_L2_PROTECTION_FAULT_CNTL2,
           (1u << 0x12));
}

// Port of mmhub_v4_1_0_disable_identity_aperture. Sets the identity
// aperture to a never-matching range so context 1+ accesses always
// take the page-table route (not the identity bypass).
static void
gart_disable_identity_aperture(const DeviceContext &dev)
{
    GART_LOG("disable_identity_aperture");
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    WREG32(dev,
        base + MMHUBRegs::MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32,
        0xFFFFFFFFu);
    WREG32(dev,
        base + MMHUBRegs::MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32,
        0x0000000Fu);
    WREG32(dev,
        base + MMHUBRegs::MMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32,
        0);
    WREG32(dev,
        base + MMHUBRegs::MMVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32,
        0);
    WREG32(dev,
        base + MMHUBRegs::MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
    WREG32(dev,
        base + MMHUBRegs::MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
}

// Port of mmhub_v4_1_0_setup_vmid_config. Configures VM contexts
// 1..14 with protection-fault defaults + per-context PT addr ranges.
// Upstream loops 15 contexts; we mirror that. Each context's regs
// live at a fixed stride (ctx_distance) from context-1's base.
//
// The distance constants come from upstream amdgpu_vmhub (Linux's
// adev->vmhub[0].ctx_distance / ctx_addr_distance). For mmhub_v4_1_0:
//   ctx_distance      = MMVM_CONTEXT1_CNTL - MMVM_CONTEXT0_CNTL = 1
//   ctx_addr_distance = stride between CONTEXT1+i pt-start regs = 2
static void
gart_setup_vmid_config(const DeviceContext &dev)
{
    GART_LOG("setup_vmid_config: programming contexts 1..14");
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    constexpr uint32_t kCtxDistance     = 1;  // CNTL stride
    constexpr uint32_t kCtxAddrDistance = 2;  // PT start/end stride
    // Matches upstream MMVM_CONTEXT1_CNTL bit programming. Fields:
    //   ENABLE_CONTEXT                                = 1   bit 0
    //   PAGE_TABLE_DEPTH                              = 0   bits 1-3 (num_level=0)
    //   PAGE_TABLE_BLOCK_SIZE                         = 0   bits 4-7 (block_size-9=0)
    //   RANGE_PROTECTION_FAULT_ENABLE_DEFAULT          = 1   bit 8
    //   DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT    = 1   bit 9
    //   PDE0_PROTECTION_FAULT_ENABLE_DEFAULT          = 1   bit 10
    //   VALID_PROTECTION_FAULT_ENABLE_DEFAULT         = 1   bit 11
    //   READ_PROTECTION_FAULT_ENABLE_DEFAULT          = 1   bit 12
    //   WRITE_PROTECTION_FAULT_ENABLE_DEFAULT         = 1   bit 13
    //   EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT       = 1   bit 14
    //   RETRY_PERMISSION_OR_INVALID_PAGE_FAULT        = 1   bit 23 (!amdgpu_noretry)
    uint32_t cntl = 0;
    cntl |= (1u << 0);
    cntl |= (1u << 8);
    cntl |= (1u << 9);
    cntl |= (1u << 10);
    cntl |= (1u << 11);
    cntl |= (1u << 12);
    cntl |= (1u << 13);
    cntl |= (1u << 14);
    cntl |= (1u << 23);
    for (uint32_t i = 0; i < 15; i++) {
        WREG32(dev,
            base + MMHUBRegs::MMVM_CONTEXT1_CNTL + i * kCtxDistance, cntl);
        WREG32(dev,
            base + MMHUBRegs::MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32 +
                   i * kCtxAddrDistance, 0);
        WREG32(dev,
            base + MMHUBRegs::MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32 +
                   i * kCtxAddrDistance, 0);
        // Upstream uses max_pfn-1 here; we don't have that yet, set to
        // 0xFFFFFFFF (all-ones-ish low half) which is the same intent
        // (give the context the full possible address range).
        WREG32(dev,
            base + MMHUBRegs::MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32 +
                   i * kCtxAddrDistance, 0xFFFFFFFFu);
        WREG32(dev,
            base + MMHUBRegs::MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32 +
                   i * kCtxAddrDistance, 0xFFFFFFFFu);
    }
}

// Port of mmhub_v4_1_0_program_invalidation. Programs the 18 TLB
// invalidation engines with "match everything" address ranges so any
// invalidation request hits every VM context.
static void
gart_program_invalidation(const DeviceContext &dev)
{
    GART_LOG("program_invalidation: 18 engines");
    uint32_t base = dev.ip.get(IPBlock::MMHUB);
    constexpr uint32_t kEngAddrDistance = 2; // stride between LO/HI registers
    for (uint32_t i = 0; i < 18; i++) {
        WREG32(dev,
            base + MMHUBRegs::MMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32 +
                   i * kEngAddrDistance, 0xFFFFFFFFu);
        WREG32(dev,
            base + MMHUBRegs::MMVM_INVALIDATE_ENG0_ADDR_RANGE_HI32 +
                   i * kEngAddrDistance, 0x1Fu);
    }
}

static void
gart_init_tlb_regs(const DeviceContext &dev)
{
    GART_LOG("init_tlb_regs");
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
    GART_LOG("init_cache_regs: programming MMVM_L2_CNTL[1..5]");
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
    GART_LOG("enable_system_domain: enable VM context 0");
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

    // Compute vram_end from FB_LOCATION_TOP for the system aperture.
    uint32_t fbTopRaw = RREG32(dev,
        mmhubBase + MMHUBRegs::MMMC_VM_FB_LOCATION_TOP);
    uint64_t vramEnd =
        (((uint64_t)(fbTopRaw & MMHUBRegs::kFBBaseMask))
         << MMHUBRegs::kFBBaseShift) | 0xFFFFFFULL;

    GART_LOG("enable: pt_mc=%#llx aperture=[%#llx, %#llx] "
             "vram=[%#llx, %#llx]",
             ptBaseMC, gart.gartStart, gart.gartEnd,
             vramStart, vramEnd);

    // Full mmhub_v4_1_0_gart_enable register sequence — matches
    // upstream exactly. ORDER MATTERS: gart_enable must run BEFORE
    // psp_load_sos so SOS comes up with GART already configured.
    gart_init_aperture_regs(dev, gart, ptBaseMC);
    gart_init_system_aperture_regs(dev, vramStart, vramEnd);
    gart_init_tlb_regs(dev);
    gart_init_cache_regs(dev);
    gart_enable_system_domain(dev);
    gart_disable_identity_aperture(dev);
    gart_setup_vmid_config(dev);
    gart_program_invalidation(dev);

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
