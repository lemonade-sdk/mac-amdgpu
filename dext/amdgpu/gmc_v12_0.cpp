//
//  gmc_v12_0.cpp — GMC v12 controller foundation (no MMIO yet).
//
//  Sources:
//    drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c
//    drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c
//    drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c
//
//  This first cut implements:
//    - gmc_mc_init: fill in geometry (sizes, GPU VA layout)
//    - gmc_vram_alloc_init: stand up the bump allocator
//    - gmc_mmhub_offsets_init: populate MMHUB register offset table
//    - gmc_gfxhub_offsets_init: populate GFXHUB register offset table
//    - gmc_init: orchestrate the above
//
//  Deferred to the next chunk:
//    - Actual MMHUB/GFXHUB gart_enable register programming
//    - GART table allocation (uses sysmem-backed path, not the
//      upstream 512 MB VRAM allocator)
//    - VM fault handler hook (depends on IH)
//    - flush_gpu_tlb / set_fault_enable_default
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include "amdgpu_gmc.h"
#include "amdgpu_field_defs.h"

#define GMC_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.gmc: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// ----- Constants -----
// R9700 specs from the devcoredump in qemu-vfio-apple/traces/:
//   real vram size:    34208743424   (= 32 GiB)
//   visible vram size: 268435456     (= 256 MiB without ReBAR)
//   gtt size:          24663633920   (= ~22.97 GiB)
//
// We initialize visible_vram_size from dev.bar2VisibleVRAMSize at
// runtime; the total is hardcoded to 32 GB until we wire on-die
// discovery + mmRCC_CONFIG_MEMSIZE properly.
constexpr uint64_t kR9700TotalVRAMBytes  = 32ULL * 1024 * 1024 * 1024;
constexpr uint64_t kGARTInitialSize      = 256ULL * 1024 * 1024;   // 256 MB
constexpr uint64_t kGARTPageSize         = kASPageSize;            // 16 KB on AS

// ----- mc_init -----
//
// Port of gmc_v12_0_mc_init (line 727). Linux reads VRAM size from
// PCI BAR len + xgmi node size + MMHUB FB location. We can't yet
// (need on-die discovery), so:
//   - Use BAR2 size (already read at PCI open) for visible window
//   - Use hardcoded 32 GB for total VRAM
//   - Lay out VRAM linearly starting at GPU-VA 0
kern_return_t
gmc_mc_init(DeviceContext &dev, GMCContext &gmc)
{
    if (gmc.inited) return kIOReturnSuccess;

    gmc.real_vram_size = kR9700TotalVRAMBytes;
    if (dev.bar2VisibleVRAMSize > 0 &&
        dev.bar2VisibleVRAMSize <= gmc.real_vram_size) {
        gmc.visible_vram_size = dev.bar2VisibleVRAMSize;
    } else {
        // Fall back to 256 MB if BAR2 wasn't enumerated yet.
        gmc.visible_vram_size = 256ULL * 1024 * 1024;
    }

    // VRAM at GPU-VA 0..(real_vram_size). GART will sit above.
    gmc.vram_start = 0;
    gmc.vram_end   = gmc.real_vram_size - 1;

    // FB aperture mirrors VRAM in our layout.
    gmc.fb_start = gmc.vram_start;
    gmc.fb_end   = gmc.vram_end;

    // GART aperture starts immediately above VRAM. 256 MB initial
    // size — well under DART's 1.5 GB ceiling.
    gmc.gart_size  = kGARTInitialSize;
    gmc.gart_start = (gmc.vram_end + 1 + kGARTPageSize - 1)
                     & ~(kGARTPageSize - 1);
    gmc.gart_end   = gmc.gart_start + gmc.gart_size - 1;

    // VM manager params for GFX12: 4-level page tables (num_level=3
    // means depth 3, i.e. 4 levels counting root), 512-entry blocks
    // (block_size=9 means log2(512)=9), 48-bit VA.
    gmc.num_level   = 3;
    gmc.block_size  = 9;
    gmc.max_pfn     = (1ULL << 48) >> 12;   // 48-bit VA / 4 KB
    gmc.vram_base_offset = 0;

    // AGP aperture — unused on our config, set to non-overlapping
    // far-away range so any stray reads are recognisable.
    gmc.agp_start = 0xFFFFFFFF00000000ULL;
    gmc.agp_end   = 0xFFFFFFFFFFFFFFFFULL;

    GMC_LOG("mc_init: real_vram=%llu MB visible=%llu MB "
            "vram=[%#llx..%#llx] gart=[%#llx..%#llx]",
            gmc.real_vram_size  >> 20,
            gmc.visible_vram_size >> 20,
            gmc.vram_start, gmc.vram_end,
            gmc.gart_start, gmc.gart_end);

    gmc.inited = true;
    return kIOReturnSuccess;
}

// ----- VRAM allocator setup -----
//
// Constructs a bump allocator over the GPU-VA range
// [vram_start + (real - visible), vram_start + real). That is,
// the top `visible_vram_size` bytes of VRAM — which is what BAR2
// maps. Allocations from here are guaranteed CPU-mappable once a
// client maps BAR2 (the dext doesn't keep a long-lived BAR2 mapping
// itself; clients do that lazily, and the cpu_ptr field stays null
// from in-dext callers until we change that).
kern_return_t
gmc_vram_alloc_init(DeviceContext &dev, GMCContext &gmc)
{
    (void)dev;
    if (!gmc.inited) return kIOReturnNotReady;
    if (gmc.vram_alloc.is_inited()) return kIOReturnSuccess;

    // Top of VRAM (real) - visible_size = base of visible window.
    uint64_t visible_base = gmc.vram_start
                          + (gmc.real_vram_size - gmc.visible_vram_size);
    gmc.vram_alloc.init(visible_base, gmc.visible_vram_size, nullptr);
    GMC_LOG("vram_alloc: range [%#llx..%#llx) size=%llu MB",
            visible_base,
            visible_base + gmc.visible_vram_size,
            gmc.visible_vram_size >> 20);
    return kIOReturnSuccess;
}

// ----- MMHUB v4_1_0 offsets -----
//
// SOC15 register names from upstream mmhub_v4_1_0.c. The actual
// offsets come from upstream `asic_reg/mmhub/mmhub_4_1_0_offset.h`.
// Linux looks these up via the SOC15 macro at the call sites; we
// stash them here so the gart_enable port (next chunk) can write
// without having to plumb #include chains.
//
// Hub IP for MMHUB on RDNA4: lives in the GMC IP block per discovery.
// (Linux treats MMHUB as a sub-IP of GMC for register addressing.)
kern_return_t
gmc_mmhub_offsets_init(GMCContext &gmc)
{
    if (gmc.mmhub.inited) return kIOReturnSuccess;
    auto &h = gmc.mmhub;

    // Offsets from mmhub_4_1_0_offset.h (subset; full enable path
    // in the next chunk will reference more). These are the dword
    // offsets into the MMHUB register window.
    h.ctx0_pt_base_lo                  = 0x051B;  // regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
    h.ctx0_pt_base_hi                  = 0x051C;  // ..._HI32
    h.ctx0_pt_start_lo                 = 0x053B;  // regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32
    h.ctx0_pt_start_hi                 = 0x053C;
    h.ctx0_pt_end_lo                   = 0x055B;  // regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32
    h.ctx0_pt_end_hi                   = 0x055C;
    h.ctx0_cntl                        = 0x050B;  // regMMVM_CONTEXT0_CNTL
    h.ctx1_cntl                        = 0x050C;  // CONTEXT1_CNTL; per-VMID stride below
    h.vm_l2_cntl                       = 0x0584;  // regMMVM_L2_CNTL
    h.vm_l2_cntl2                      = 0x0585;
    h.vm_l2_cntl3                      = 0x0586;
    h.vm_l2_cntl4                      = 0x0587;
    h.vm_l2_cntl5                      = 0x0588;
    h.vm_l2_protection_fault_cntl      = 0x0592;
    h.vm_l1_tlb_cntl                   = 0x0583;
    h.vm_invalidate_eng0_req           = 0x056A;
    h.vm_invalidate_eng0_ack           = 0x057A;
    h.vm_invalidate_eng0_sem           = 0x056E;
    h.vm_invalidate_eng0_addr_range_lo32 = 0x0572;
    h.vm_invalidate_eng0_addr_range_hi32 = 0x0573;
    h.vm_system_aperture_low_addr      = 0x055D;
    h.vm_system_aperture_high_addr     = 0x055E;
    h.vm_system_aperture_default_addr_lo = 0x0561;
    h.vm_system_aperture_default_addr_hi = 0x0562;
    h.vm_context0_default_addr_lo      = 0x0563;
    h.vm_context0_default_addr_hi      = 0x0564;

    // Per-VMID stride in Linux: CONTEXT1_CNTL - CONTEXT0_CNTL = 1
    // for the CNTL register, and the page-table-base registers are
    // 2 apart. We use the more common single-CNTL stride here and
    // the enable path will compute the actual address per register.
    h.ctx_distance = 1;
    h.eng_distance = 4;  // VM_INVALIDATE_ENG{N}_* stride

    h.ip = IPBlock::GMC;   // MMHUB lives at the GMC base on RDNA4
    h.inited = true;
    GMC_LOG("mmhub offsets initialised (base IP=%u)", (unsigned)h.ip);
    return kIOReturnSuccess;
}

// ----- GFXHUB v12_0 offsets -----
//
// Source: gfxhub_v12_0.c. GFXHUB sits in the GC IP block. The
// register names mirror MMHUB but live in the GC register window.
// Offsets from `asic_reg/gc/gc_12_0_0_offset.h`.
kern_return_t
gmc_gfxhub_offsets_init(GMCContext &gmc)
{
    if (gmc.gfxhub.inited) return kIOReturnSuccess;
    auto &h = gmc.gfxhub;

    h.ctx0_pt_base_lo                  = 0x265B;  // regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
    h.ctx0_pt_base_hi                  = 0x265C;
    h.ctx0_pt_start_lo                 = 0x267B;  // regGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32
    h.ctx0_pt_start_hi                 = 0x267C;
    h.ctx0_pt_end_lo                   = 0x269B;  // regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32
    h.ctx0_pt_end_hi                   = 0x269C;
    h.ctx0_cntl                        = 0x264B;  // regGCVM_CONTEXT0_CNTL
    h.ctx1_cntl                        = 0x264C;
    h.vm_l2_cntl                       = 0x26C4;  // regGCVM_L2_CNTL
    h.vm_l2_cntl2                      = 0x26C5;
    h.vm_l2_cntl3                      = 0x26C6;
    h.vm_l2_cntl4                      = 0x26C7;
    h.vm_l2_cntl5                      = 0x26C8;
    h.vm_l2_protection_fault_cntl      = 0x26D2;
    h.vm_l1_tlb_cntl                   = 0x26C3;
    h.vm_invalidate_eng0_req           = 0x26AA;
    h.vm_invalidate_eng0_ack           = 0x26BA;
    h.vm_invalidate_eng0_sem           = 0x26AE;
    h.vm_invalidate_eng0_addr_range_lo32 = 0x26B2;
    h.vm_invalidate_eng0_addr_range_hi32 = 0x26B3;
    h.vm_system_aperture_low_addr      = 0x269D;
    h.vm_system_aperture_high_addr     = 0x269E;
    h.vm_system_aperture_default_addr_lo = 0x26A1;
    h.vm_system_aperture_default_addr_hi = 0x26A2;
    h.vm_context0_default_addr_lo      = 0x26A3;
    h.vm_context0_default_addr_hi      = 0x26A4;

    h.ctx_distance = 1;
    h.eng_distance = 4;

    h.ip = IPBlock::GC;
    h.inited = true;
    GMC_LOG("gfxhub offsets initialised (base IP=%u)", (unsigned)h.ip);
    return kIOReturnSuccess;
}

// ============================================================
// Resource allocation: GART page table + dummy_page + mem_scratch.
//
// All three live in DART-mapped system memory at 16 KB alignment.
// GART page table size = gart_size / page_size * sizeof(pte).
// We use the AS 16 KB page so #PTEs = 256 MB / 16 KB = 16384,
// each 8 bytes → 128 KB page table. Tiny; pad up to 16 KB anyway.
// ============================================================

static kern_return_t
alloc_dma_block(DeviceContext &dev, uint64_t size,
                IOBufferMemoryDescriptor **outBuf,
                IODMACommand             **outDma,
                uint64_t *outBus,
                void    **outCpu)
{
    *outBuf = nullptr; *outDma = nullptr; *outBus = 0; *outCpu = nullptr;
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, size, kASPageSize, &buf);
    if (ret != kIOReturnSuccess || buf == nullptr) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    buf->SetLength(size);

    IODMACommandSpecification spec = {};
    spec.options = kIODMACommandSpecificationNoOptions;
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
    ret = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf,
                             0, size, &flags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) {
        dma->release();
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNotAligned;
    }
    IOAddressSegment cpu = {};
    buf->GetAddressRange(&cpu);

    *outBuf = buf;
    *outDma = dma;
    *outBus = seg.address;
    *outCpu = reinterpret_cast<void *>(cpu.address);
    return kIOReturnSuccess;
}

kern_return_t
gmc_alloc_resources(DeviceContext &dev, GMCContext &gmc)
{
    if (gmc.gart_pt_buf != nullptr) return kIOReturnSuccess;
    if (!gmc.inited) return kIOReturnNotReady;

    // GART page table — 16 KB pages, 8 B PTE, padded to AS page.
    uint64_t pt_entries = gmc.gart_size / kASPageSize;
    uint64_t pt_bytes   = pt_entries * 8;
    if (pt_bytes < kASPageSize) pt_bytes = kASPageSize;
    pt_bytes = (pt_bytes + kASPageSize - 1) & ~(kASPageSize - 1);
    gmc.gart_pt_size = pt_bytes;

    void *cpu = nullptr; uint64_t bus = 0;
    kern_return_t r;
    r = alloc_dma_block(dev, pt_bytes, &gmc.gart_pt_buf,
                        &gmc.gart_pt_dma, &bus, &cpu);
    if (r != kIOReturnSuccess) {
        GMC_LOG("gart page table alloc failed: %#x", r);
        return r;
    }
    gmc.gart_pt_bus = bus;
    gmc.gart_pt_cpu = cpu;
    memset(cpu, 0, pt_bytes);  // all PTEs invalid initially

    // dummy_page — destination for protection-fault page redirects.
    r = alloc_dma_block(dev, kASPageSize, &gmc.dummy_page_buf,
                        &gmc.dummy_page_dma, &gmc.dummy_page_bus, &cpu);
    if (r != kIOReturnSuccess) {
        GMC_LOG("dummy_page alloc failed: %#x", r);
        return r;
    }

    // mem_scratch — used as default system aperture address.
    r = alloc_dma_block(dev, kASPageSize, &gmc.mem_scratch_buf,
                        &gmc.mem_scratch_dma, &gmc.mem_scratch_bus, &cpu);
    if (r != kIOReturnSuccess) {
        GMC_LOG("mem_scratch alloc failed: %#x", r);
        return r;
    }

    GMC_LOG("resources: gart_pt bus=%#llx (%llu B) "
            "dummy bus=%#llx scratch bus=%#llx",
            gmc.gart_pt_bus, pt_bytes,
            gmc.dummy_page_bus, gmc.mem_scratch_bus);
    return kIOReturnSuccess;
}

void
gmc_release_resources(GMCContext &gmc)
{
    auto teardown = [](IOBufferMemoryDescriptor *&b, IODMACommand *&d) {
        if (d != nullptr) {
            d->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            d->release(); d = nullptr;
        }
        if (b != nullptr) { b->release(); b = nullptr; }
    };
    teardown(gmc.mem_scratch_buf, gmc.mem_scratch_dma);
    teardown(gmc.dummy_page_buf, gmc.dummy_page_dma);
    teardown(gmc.gart_pt_buf, gmc.gart_pt_dma);
    gmc.gart_pt_bus = 0; gmc.gart_pt_cpu = nullptr; gmc.gart_pt_size = 0;
    gmc.dummy_page_bus = 0; gmc.mem_scratch_bus = 0;
}

// ============================================================
// MMHUB v4_1_0 gart_enable port.
//
// Sub-functions follow upstream mmhub_v4_1_0.c structure 1:1.
// Each is parameterized by the GMCContext so they can be reused
// by the GFXHUB twin (since the field layout is identical, just
// the register addresses change — see amdgpu_field_defs.h aliases).
// ============================================================

// init_gart_aperture_regs: write CONTEXT0 PT base, start, end.
static void
hub_init_gart_aperture_regs(const DeviceContext &dev,
                            const GMCContext &gmc, const HubContext &h)
{
    // PT base address (split lo/hi)
    uint64_t pt = gmc.gart_pt_bus;
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.ctx0_pt_base_lo),
           (uint32_t)(pt & 0xFFFFFFFFu));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.ctx0_pt_base_hi),
           (uint32_t)(pt >> 32));

    // PT start/end: encoded as page-frame numbers, with the
    // high half being the upper bits of the 44-bit VA.
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.ctx0_pt_start_lo),
           (uint32_t)(gmc.gart_start >> 12));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.ctx0_pt_start_hi),
           (uint32_t)(gmc.gart_start >> 44));

    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.ctx0_pt_end_lo),
           (uint32_t)(gmc.gart_end >> 12));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.ctx0_pt_end_hi),
           (uint32_t)(gmc.gart_end >> 44));
}

// init_system_aperture_regs: AGP, system aperture low/high, default,
// L2 protection-fault default.
static void
hub_init_system_aperture_regs(const DeviceContext &dev,
                              const GMCContext &gmc, const HubContext &h)
{
    uint64_t aperture_low  = gmc.fb_start;     // agp is in non-overlapping range
    uint64_t aperture_high = gmc.fb_end;

    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_system_aperture_low_addr),
           (uint32_t)(aperture_low >> 18));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_system_aperture_high_addr),
           (uint32_t)(aperture_high >> 18));

    // Default page address — mem_scratch.
    uint64_t def = gmc.mem_scratch_bus - gmc.vram_start
                 + gmc.vram_base_offset;
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip,
                                 h.vm_system_aperture_default_addr_lo),
           (uint32_t)(def >> 12));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip,
                                 h.vm_system_aperture_default_addr_hi),
           (uint32_t)(def >> 44));
}

// init_tlb_regs: enable L1 TLB, MTYPE=UC, advanced driver model.
static void
hub_init_tlb_regs_mmhub(const DeviceContext &dev, const HubContext &h)
{
    uint32_t reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l1_tlb_cntl);
    uint32_t tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL,
                        ENABLE_ADVANCED_DRIVER_MODEL, 1);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL,
                        SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ECO_BITS, 0);
    tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, MTYPE, MMHUB_MTYPE_UC);
    WREG32(dev, reg, tmp);
}

// init_cache_regs (MMHUB variant): L2 cache enable + assorted bits.
static void
hub_init_cache_regs_mmhub(const DeviceContext &dev, const HubContext &h)
{
    uint32_t reg, tmp;

    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl);
    tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WREG32(dev, reg, tmp);

    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl2);
    tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32(dev, reg, tmp);

    // CNTL3: bank_select=9, BIGK fragment size=6 for non-translate-further
    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl3);
    tmp = 0;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WREG32(dev, reg, tmp);

    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl4);
    tmp = 0;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WREG32(dev, reg, tmp);

    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl5);
    tmp = 0;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE, 0);
    WREG32(dev, reg, tmp);
}

// enable_system_domain: turn on CONTEXT0 with PT depth 0 (no PT for
// CONTEXT0 — it covers the system/FB aperture).
static void
hub_enable_system_domain(const DeviceContext &dev, const HubContext &h)
{
    uint32_t reg = SOC15_REG_OFFSET(dev, h.ip, h.ctx0_cntl);
    uint32_t tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT0_CNTL,
                        RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
    WREG32(dev, reg, tmp);
}

// setup_vmid_config: per-VMID CONTEXT1..14 enable with fault bits.
static void
hub_setup_vmid_config(const DeviceContext &dev, const GMCContext &gmc,
                      const HubContext &h)
{
    for (int i = 0; i <= 14; i++) {
        // CONTEXT(i+1)_CNTL = CONTEXT1_CNTL + i * ctx_distance
        uint32_t cntl_reg = SOC15_REG_OFFSET(dev, h.ip, h.ctx1_cntl)
                            + i * h.ctx_distance;
        uint32_t tmp = RREG32(dev, cntl_reg);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            PAGE_TABLE_DEPTH, gmc.num_level);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE,
                            gmc.block_size - 9);
        // no-retry on fault to suppress storms
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 1);
        WREG32(dev, cntl_reg, tmp);

        // Per-VMID PT start/end addresses — fresh contexts get full range.
        // (Linux uses ctx_addr_distance; we collapse to ctx_distance.)
        // Currently leaving CONTEXT1..14 PT start/end zeroed.
    }
}

// program_invalidation: arm invalidation engines 0..17 over the
// full address range.
static void
hub_program_invalidation(const DeviceContext &dev, const HubContext &h)
{
    for (int i = 0; i < 18; i++) {
        WREG32(dev,
               SOC15_REG_OFFSET(dev, h.ip,
                                h.vm_invalidate_eng0_addr_range_lo32)
               + i * h.eng_distance, 0xFFFFFFFFu);
        WREG32(dev,
               SOC15_REG_OFFSET(dev, h.ip,
                                h.vm_invalidate_eng0_addr_range_hi32)
               + i * h.eng_distance, 0x1Fu);
    }
}

kern_return_t
gmc_mmhub_gart_enable(DeviceContext &dev, GMCContext &gmc)
{
    if (!gmc.mmhub.inited) return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0) return kIOReturnNotReady;
    if (!dev.ip.isResolved(gmc.mmhub.ip)) {
        GMC_LOG("mmhub_gart_enable: IP base not resolved (block=%u)",
                (unsigned)gmc.mmhub.ip);
        return kIOReturnNotReady;
    }

    hub_init_gart_aperture_regs(dev, gmc, gmc.mmhub);
    hub_init_system_aperture_regs(dev, gmc, gmc.mmhub);
    hub_init_tlb_regs_mmhub(dev, gmc.mmhub);
    hub_init_cache_regs_mmhub(dev, gmc.mmhub);
    hub_enable_system_domain(dev, gmc.mmhub);
    hub_setup_vmid_config(dev, gmc, gmc.mmhub);
    hub_program_invalidation(dev, gmc.mmhub);

    GMC_LOG("mmhub_gart_enable done");
    return kIOReturnSuccess;
}

kern_return_t
gmc_gfxhub_gart_enable(DeviceContext &dev, GMCContext &gmc)
{
    if (!gmc.gfxhub.inited) return kIOReturnNotReady;
    if (gmc.gart_pt_bus == 0) return kIOReturnNotReady;
    if (!dev.ip.isResolved(gmc.gfxhub.ip)) {
        GMC_LOG("gfxhub_gart_enable: IP base not resolved (block=%u)",
                (unsigned)gmc.gfxhub.ip);
        return kIOReturnNotReady;
    }

    // GFXHUB field layouts mirror MMHUB exactly (see field_defs aliases),
    // and our helpers use the MMHUB-style names that REG_SET_FIELD
    // resolves to the same SHIFT/MASK constants — so reusing the
    // helpers is correct.
    hub_init_gart_aperture_regs(dev, gmc, gmc.gfxhub);
    hub_init_system_aperture_regs(dev, gmc, gmc.gfxhub);
    hub_init_tlb_regs_mmhub(dev, gmc.gfxhub);
    hub_init_cache_regs_mmhub(dev, gmc.gfxhub);
    hub_enable_system_domain(dev, gmc.gfxhub);
    hub_setup_vmid_config(dev, gmc, gmc.gfxhub);
    hub_program_invalidation(dev, gmc.gfxhub);

    GMC_LOG("gfxhub_gart_enable done");
    return kIOReturnSuccess;
}

// ----- GMCInit orchestrator entry -----
kern_return_t
gmc_init(DeviceContext &dev, GMCContext &gmc)
{
    kern_return_t r;
    r = gmc_mc_init(dev, gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_vram_alloc_init(dev, gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_mmhub_offsets_init(gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_gfxhub_offsets_init(gmc);
    if (r != kIOReturnSuccess) return r;
    r = gmc_alloc_resources(dev, gmc);
    if (r != kIOReturnSuccess) return r;

    // MMIO programming gated on IP bases being resolved (set via
    // SetIPBase or LoadDiscoveryBin from userspace).
    if (dev.ip.isResolved(gmc.mmhub.ip)) {
        r = gmc_mmhub_gart_enable(dev, gmc);
        if (r != kIOReturnSuccess) {
            GMC_LOG("mmhub_gart_enable failed: %#x", r);
            return r;
        }
    } else {
        GMC_LOG("skipping mmhub_gart_enable — IP base unresolved");
    }
    if (dev.ip.isResolved(gmc.gfxhub.ip)) {
        r = gmc_gfxhub_gart_enable(dev, gmc);
        if (r != kIOReturnSuccess) {
            GMC_LOG("gfxhub_gart_enable failed: %#x", r);
            return r;
        }
    } else {
        GMC_LOG("skipping gfxhub_gart_enable — IP base unresolved");
    }

    // HDP flush — invalidate HDP cache so the GPU sees the page-
    // table writes we just made via the BAR.
    if (dev.ip.isResolved(IPBlock::HDP)) {
        gmc_hdp_flush(dev);
    }

    // Flush TLBs on both hubs (vmid=0, legacy flush type). This
    // ensures the page tables are picked up before the first GPU
    // memory access through GART.
    if (dev.ip.isResolved(gmc.mmhub.ip)) {
        gmc_flush_gpu_tlb(dev, gmc, gmc.mmhub, /*vmid*/ 0, /*type*/ 0);
    }
    if (dev.ip.isResolved(gmc.gfxhub.ip)) {
        gmc_flush_gpu_tlb(dev, gmc, gmc.gfxhub, /*vmid*/ 0, /*type*/ 0);
    }

    dev.gmcReady = true;
    GMC_LOG("GMCInit complete");
    return kIOReturnSuccess;
}

// ============================================================
// HDP flush — delegates to amdgpu_hdp_flush in amdgpu_regs.h, which
// is the proper port of upstream amdgpu_hdp_generic_flush
// (amdgpu_hdp.c:48-54): write 0 to remap-HDP register + readback
// NBIO's get_memsize. hdp_v7_0_funcs (hdp_v7_0.c:128-132) only
// hooks .flush_hdp = amdgpu_hdp_generic_flush; there is no
// invalidate_hdp on this asic, and the prior implementation here
// wrote to a bogus regHDP_MEM_COHERENCY_FLUSH_CNTL=0x230C that does
// not exist in gc_12_0_0_offset.h or hdp_7_0_0_offset.h. Audit #6
// step 8.
// ============================================================
kern_return_t
gmc_hdp_flush(DeviceContext &dev)
{
    amdgpu_hdp_flush(dev);
    return kIOReturnSuccess;
}

// ============================================================
// gmc_flush_gpu_tlb — invalidate TLB entries using the engines we
// armed in hub_program_invalidation. Mirrors gmc_v12_0_flush_gpu_tlb.
//
// The protocol: write a request word to VM_INVALIDATE_ENG0_REQ with
// the vmid + flush type packed into specific bit positions, then
// poll VM_INVALIDATE_ENG0_ACK until the corresponding bit is set.
// ============================================================
kern_return_t
gmc_flush_gpu_tlb(DeviceContext &dev, const GMCContext &gmc,
                  const HubContext &hub, uint32_t vmid, uint32_t flush_type)
{
    (void)gmc;
    if (!hub.inited || !dev.ip.isResolved(hub.ip)) return kIOReturnNotReady;

    // Request word layout (from gmc_v12_0_get_invalidate_req):
    //   bit  0      = PER_VMID_INVALIDATE_REQ shifted left by vmid
    //   bits 4..5   = FLUSH_TYPE
    //   bit 6       = INVALIDATE_L2_PTES
    //   bit 7       = INVALIDATE_L2_PDE0
    //   bit 8       = INVALIDATE_L2_PDE1
    //   bit 9       = INVALIDATE_L2_PDE2
    //   bit 10      = INVALIDATE_L1_PTES
    //   bit 11      = INVALIDATE_L1_PDES
    //   bit 12      = CLEAR_PROTECTION_FAULT_STATUS_ADDR
    uint32_t req = 0;
    req |= (1u << vmid);                  // PER_VMID bit
    req |= ((flush_type & 0x3) << 4);     // FLUSH_TYPE
    req |= (1u << 6);   // INVALIDATE_L2_PTES
    req |= (1u << 7);   // INVALIDATE_L2_PDE0
    req |= (1u << 8);   // INVALIDATE_L2_PDE1
    req |= (1u << 9);   // INVALIDATE_L2_PDE2
    req |= (1u << 10);  // INVALIDATE_L1_PTES
    req |= (1u << 11);  // INVALIDATE_L1_PDES

    const uint32_t req_reg = SOC15_REG_OFFSET(dev, hub.ip,
                                              hub.vm_invalidate_eng0_req);
    const uint32_t ack_reg = SOC15_REG_OFFSET(dev, hub.ip,
                                              hub.vm_invalidate_eng0_ack);

    WREG32(dev, req_reg, req);

    // Poll ack bit for this vmid.
    uint32_t expected = (1u << vmid);
    uint32_t value = 0;
    if (!poll_reg(dev, ack_reg, expected, expected,
                  /*timeout_us*/ 100000, &value)) {
        GMC_LOG("flush_gpu_tlb: ack timeout (req=%#x ack=%#x ip=%u)",
                req, value, (unsigned)hub.ip);
        return kIOReturnTimeout;
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
