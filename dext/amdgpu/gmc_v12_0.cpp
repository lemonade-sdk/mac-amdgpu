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
// Offsets vendored from upstream
// drivers/gpu/drm/amd/include/asic_reg/mmhub/mmhub_4_1_0_offset.h.
// Audit #4 F4-F5: the previous values in this table were taken from
// the wrong IP generation (mmhub_v3 / older) and resulted in writes
// landing on unrelated registers. Every offset below is now cross-
// referenced to the upstream header.
//
// Hub IP for MMHUB on RDNA4: lives in the GMC IP block per discovery.
// (Linux treats MMHUB as a sub-IP of GMC for register addressing.)
kern_return_t
gmc_mmhub_offsets_init(GMCContext &gmc)
{
    if (gmc.mmhub.inited) return kIOReturnSuccess;
    auto &h = gmc.mmhub;

    // ---- CONTEXT0/1 page-table-base / start / end ----
    // mmhub_4_1_0_offset.h:1094-1101  (PAGE_TABLE_BASE_ADDR)
    // mmhub_4_1_0_offset.h:1158-1163  (PAGE_TABLE_START_ADDR)
    // mmhub_4_1_0_offset.h:1222-1227  (PAGE_TABLE_END_ADDR)
    h.ctx0_pt_base_lo  = 0x05cf;
    h.ctx0_pt_base_hi  = 0x05d0;
    h.ctx0_pt_start_lo = 0x05ef;
    h.ctx0_pt_start_hi = 0x05f0;
    h.ctx0_pt_end_lo   = 0x060f;
    h.ctx0_pt_end_hi   = 0x0610;
    h.ctx1_pt_start_lo = 0x05f1;
    h.ctx1_pt_start_hi = 0x05f2;
    h.ctx1_pt_end_lo   = 0x0611;
    h.ctx1_pt_end_hi   = 0x0612;

    // ---- CONTEXT[N]_CNTL ----
    // mmhub_4_1_0_offset.h:880-882
    h.ctx0_cntl = 0x0564;
    h.ctx1_cntl = 0x0565;

    // ---- L2 cache control ----
    // mmhub_4_1_0_offset.h:730-734 + 778 + 790
    h.vm_l2_cntl  = 0x04e4;
    h.vm_l2_cntl2 = 0x04e5;
    h.vm_l2_cntl3 = 0x04e6;
    h.vm_l2_cntl4 = 0x04fd;
    h.vm_l2_cntl5 = 0x0503;

    // ---- Protection-fault CNTL + default addr ----
    // mmhub_4_1_0_offset.h:746-765
    h.vm_l2_protection_fault_cntl              = 0x04ec;
    h.vm_l2_protection_fault_cntl2             = 0x04ed;
    h.vm_l2_protection_fault_default_addr_lo32 = 0x04f4;
    h.vm_l2_protection_fault_default_addr_hi32 = 0x04f5;

    // ---- Identity aperture + offset ----
    // mmhub_4_1_0_offset.h:766-777
    h.vm_l2_ctx1_identity_aperture_low_lo32  = 0x04f7;
    h.vm_l2_ctx1_identity_aperture_low_hi32  = 0x04f8;
    h.vm_l2_ctx1_identity_aperture_high_lo32 = 0x04f9;
    h.vm_l2_ctx1_identity_aperture_high_hi32 = 0x04fa;
    h.vm_l2_ctx_identity_physical_offset_lo32 = 0x04fb;
    h.vm_l2_ctx_identity_physical_offset_hi32 = 0x04fc;

    // ---- TLB / aperture ----
    // mmhub_4_1_0_offset.h:870-874
    h.vm_l1_tlb_cntl                       = 0x055b;
    h.vm_system_aperture_low_addr          = 0x0559;
    h.vm_system_aperture_high_addr         = 0x055a;
    // mmhub_4_1_0_offset.h:692-695
    h.vm_system_aperture_default_addr_lo   = 0x04c8;
    h.vm_system_aperture_default_addr_hi   = 0x04c9;

    // ---- AGP / FB location ----
    // mmhub_4_1_0_offset.h:860-869 + 690
    h.vm_agp_top           = 0x0556;
    h.vm_agp_bot           = 0x0557;
    h.vm_agp_base          = 0x0558;
    h.vm_fb_location_base  = 0x0554;
    h.vm_fb_location_top   = 0x0555;
    h.vm_fb_offset         = 0x04c7;

    // ---- Invalidation engines ----
    // mmhub_4_1_0_offset.h:914 / 950 / 986 / 1022-1024
    h.vm_invalidate_eng0_req             = 0x0587;
    h.vm_invalidate_eng0_ack             = 0x0599;
    h.vm_invalidate_eng0_sem             = 0x0575;
    h.vm_invalidate_eng0_addr_range_lo32 = 0x05ab;
    h.vm_invalidate_eng0_addr_range_hi32 = 0x05ac;

    // ---- Strides ----  (audit #4 F6)
    // mmhub_v4_1_0.c:487-493
    //   ctx_distance      = regMMVM_CONTEXT1_CNTL                         - regMMVM_CONTEXT0_CNTL
    //                     = 0x0565 - 0x0564 = 1
    //   ctx_addr_distance = regMMVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32    - regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
    //                     = 0x05d1 - 0x05cf = 2
    //   eng_distance      = regMMVM_INVALIDATE_ENG1_REQ                   - regMMVM_INVALIDATE_ENG0_REQ
    //                     = 0x0588 - 0x0587 = 1
    //   eng_addr_distance = regMMVM_INVALIDATE_ENG1_ADDR_RANGE_LO32       - regMMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32
    //                     = 0x05ad - 0x05ab = 2
    h.ctx_distance      = 1;
    h.ctx_addr_distance = 2;
    h.eng_distance      = 1;
    h.eng_addr_distance = 2;

    h.ip = IPBlock::MMHUB;   // MMHUB has its own IP entry in the discovery table
    h.inited = true;
    GMC_LOG("mmhub v4_1_0 offsets initialised (base IP=%u, ctx_dist=%u "
            "eng_dist=%u eng_addr_dist=%u)",
            (unsigned)h.ip, h.ctx_distance, h.eng_distance,
            h.eng_addr_distance);
    return kIOReturnSuccess;
}

// ----- GFXHUB v12_0 offsets -----
//
// Source: gfxhub_v12_0.c + gc/gc_12_0_0_offset.h. GFXHUB sits in the
// GC IP block (its registers are part of the GC register window).
//
// Audit #4 F4-F5: previous offsets were from the wrong IP gen.
// Every offset below cites the exact gc_12_0_0_offset.h line number.
kern_return_t
gmc_gfxhub_offsets_init(GMCContext &gmc)
{
    if (gmc.gfxhub.inited) return kIOReturnSuccess;
    auto &h = gmc.gfxhub;

    // ---- CONTEXT0/1 PT base / start / end ----
    // gc_12_0_0_offset.h:3106-3110 (PT_BASE_ADDR)
    // gc_12_0_0_offset.h:3170-3172 (PT_START_ADDR)
    // gc_12_0_0_offset.h:3234-3236 (PT_END_ADDR)
    h.ctx0_pt_base_lo  = 0x168f;
    h.ctx0_pt_base_hi  = 0x1690;
    h.ctx0_pt_start_lo = 0x16af;
    h.ctx0_pt_start_hi = 0x16b0;
    h.ctx0_pt_end_lo   = 0x16cf;
    h.ctx0_pt_end_hi   = 0x16d0;
    // CONTEXT1 PT regs are CONTEXT0 + ctx_addr_distance (=2).
    h.ctx1_pt_start_lo = 0x16b1;
    h.ctx1_pt_start_hi = 0x16b2;
    h.ctx1_pt_end_lo   = 0x16d1;
    h.ctx1_pt_end_hi   = 0x16d2;

    // ---- CONTEXT[N]_CNTL ----
    // gc_12_0_0_offset.h:2892-2894
    h.ctx0_cntl = 0x1624;
    h.ctx1_cntl = 0x1625;

    // ---- L2 cache control ----
    // gc_12_0_0_offset.h:2766-2770 + 2814 + 2826
    h.vm_l2_cntl  = 0x15c4;
    h.vm_l2_cntl2 = 0x15c5;
    h.vm_l2_cntl3 = 0x15c6;
    h.vm_l2_cntl4 = 0x15dd;
    h.vm_l2_cntl5 = 0x15e3;

    // ---- Protection-fault CNTL + default addr ----
    // gc_12_0_0_offset.h:2782-2800
    h.vm_l2_protection_fault_cntl              = 0x15cc;
    h.vm_l2_protection_fault_cntl2             = 0x15cd;
    h.vm_l2_protection_fault_default_addr_lo32 = 0x15d4;
    h.vm_l2_protection_fault_default_addr_hi32 = 0x15d5;

    // ---- Identity aperture + offset ----
    // gc_12_0_0_offset.h:2802-2813
    h.vm_l2_ctx1_identity_aperture_low_lo32   = 0x15d7;
    h.vm_l2_ctx1_identity_aperture_low_hi32   = 0x15d8;
    h.vm_l2_ctx1_identity_aperture_high_lo32  = 0x15d9;
    h.vm_l2_ctx1_identity_aperture_high_hi32  = 0x15da;
    h.vm_l2_ctx_identity_physical_offset_lo32 = 0x15db;
    h.vm_l2_ctx_identity_physical_offset_hi32 = 0x15dc;

    // ---- TLB / aperture ----
    // gc_12_0_0_offset.h:2882-2886
    h.vm_l1_tlb_cntl                       = 0x161b;
    h.vm_system_aperture_low_addr          = 0x1619;
    h.vm_system_aperture_high_addr         = 0x161a;
    // gc_12_0_0_offset.h:2728-2730
    h.vm_system_aperture_default_addr_lo   = 0x15a8;
    h.vm_system_aperture_default_addr_hi   = 0x15a9;

    // ---- AGP / FB location ----
    // gc_12_0_0_offset.h:2872-2880 + 2726
    h.vm_agp_top           = 0x1616;
    h.vm_agp_bot           = 0x1617;
    h.vm_agp_base          = 0x1618;
    h.vm_fb_location_base  = 0x1614;
    h.vm_fb_location_top   = 0x1615;
    h.vm_fb_offset         = 0x15a7;

    // ---- Invalidation engines ----
    // gc_12_0_0_offset.h:2926 + 2962 + 2998 + 3034-3036
    h.vm_invalidate_eng0_req             = 0x1647;
    h.vm_invalidate_eng0_ack             = 0x1659;
    h.vm_invalidate_eng0_sem             = 0x1635;
    h.vm_invalidate_eng0_addr_range_lo32 = 0x166b;
    h.vm_invalidate_eng0_addr_range_hi32 = 0x166c;

    // ---- Strides ----  (audit #4 F6)
    // gfxhub_v12_0.c:494-500:
    //   ctx_distance      = regGCVM_CONTEXT1_CNTL                         - regGCVM_CONTEXT0_CNTL
    //                     = 0x1625 - 0x1624 = 1
    //   ctx_addr_distance = regGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32    - regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32
    //                     = 0x1691 - 0x168f = 2
    //   eng_distance      = regGCVM_INVALIDATE_ENG1_REQ                   - regGCVM_INVALIDATE_ENG0_REQ
    //                     = 0x1648 - 0x1647 = 1
    //   eng_addr_distance = regGCVM_INVALIDATE_ENG1_ADDR_RANGE_LO32       - regGCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32
    //                     = 0x166d - 0x166b = 2
    h.ctx_distance      = 1;
    h.ctx_addr_distance = 2;
    h.eng_distance      = 1;
    h.eng_addr_distance = 2;

    h.ip = IPBlock::GC;
    h.inited = true;
    GMC_LOG("gfxhub v12_0 offsets initialised (base IP=%u, ctx_dist=%u "
            "eng_dist=%u eng_addr_dist=%u)",
            (unsigned)h.ip, h.ctx_distance, h.eng_distance,
            h.eng_addr_distance);
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

// init_system_aperture_regs — port of:
//   mmhub_v4_1_0_init_system_aperture_regs  (mmhub_v4_1_0.c:152)
//   gfxhub_v12_0_init_system_aperture_regs  (gfxhub_v12_0.c:158)
// Programs AGP base/top/bot, system aperture low/high, default addr,
// protection-fault default addr, and PFCNTL2 PTE_READ_RETRY bit.
//
// Audit #4 F21/F22 fix: previously the protection-fault default addr
// registers were never programmed; now they are pointed at dummy_page.
static void
hub_init_system_aperture_regs(const DeviceContext &dev,
                              const GMCContext &gmc, const HubContext &h)
{
    // Program the AGP BAR: base = 0, bot = agp_start>>24, top = agp_end>>24
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_agp_base), 0);
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_agp_bot),
           (uint32_t)(gmc.agp_start >> 24));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_agp_top),
           (uint32_t)(gmc.agp_end >> 24));

    // System aperture low/high = min(fb_start, agp_start) >> 18, ditto high.
    uint64_t aperture_low  = gmc.fb_start;
    uint64_t aperture_high = gmc.fb_end;
    if (gmc.agp_start < aperture_low)  aperture_low  = gmc.agp_start;
    if (gmc.agp_end   > aperture_high) aperture_high = gmc.agp_end;

    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_system_aperture_low_addr),
           (uint32_t)(aperture_low >> 18));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip, h.vm_system_aperture_high_addr),
           (uint32_t)(aperture_high >> 18));

    // Default page address (mem_scratch as system-aperture default).
    uint64_t def = gmc.mem_scratch_bus - gmc.vram_start
                 + gmc.vram_base_offset;
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip,
                                 h.vm_system_aperture_default_addr_lo),
           (uint32_t)(def >> 12));
    WREG32(dev, SOC15_REG_OFFSET(dev, h.ip,
                                 h.vm_system_aperture_default_addr_hi),
           (uint32_t)(def >> 44));

    // Protection-fault default address — dummy_page  (F21/F22).
    // mmhub_v4_1_0.c:185-188 / gfxhub_v12_0.c:182-185.
    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip,
                            h.vm_l2_protection_fault_default_addr_lo32),
           (uint32_t)(gmc.dummy_page_bus >> 12));
    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip,
                            h.vm_l2_protection_fault_default_addr_hi32),
           (uint32_t)(gmc.dummy_page_bus >> 44));

    // PFCNTL2 ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY = 1
    // mmhub_v4_1_0.c:190-193 / gfxhub_v12_0.c:187-188.
    uint32_t pfc2_reg = SOC15_REG_OFFSET(dev, h.ip,
                                         h.vm_l2_protection_fault_cntl2);
    uint32_t pfc2 = RREG32(dev, pfc2_reg);
    pfc2 = REG_SET_FIELD(pfc2, MMVM_L2_PROTECTION_FAULT_CNTL2,
                         ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
    WREG32(dev, pfc2_reg, pfc2);
}

// init_tlb_regs — enable L1 TLB, MTYPE=UC, advanced driver model.
// Port of mmhub_v4_1_0_init_tlb_regs (mmhub_v4_1_0.c:196) and
//          gfxhub_v12_0_init_tlb_regs (gfxhub_v12_0.c:192).
static void
hub_init_tlb_regs(const DeviceContext &dev, const HubContext &h)
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

// disable_identity_aperture — port of
//   mmhub_v4_1_0_disable_identity_aperture (mmhub_v4_1_0.c:279)
//   gfxhub_v12_0_disable_identity_aperture (gfxhub_v12_0.c:275)
// Closes the identity aperture so CONTEXT1 access is fully translated.
// Constants from upstream: low=0xFFFFFFFF/0xF, high=0/0, offset=0/0.
static void
hub_disable_identity_aperture(const DeviceContext &dev, const HubContext &h)
{
    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_ctx1_identity_aperture_low_lo32),
           0xFFFFFFFFu);
    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_ctx1_identity_aperture_low_hi32),
           0x0000000Fu);

    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_ctx1_identity_aperture_high_lo32),
           0);
    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_ctx1_identity_aperture_high_hi32),
           0);

    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_ctx_identity_physical_offset_lo32),
           0);
    WREG32(dev,
           SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_ctx_identity_physical_offset_hi32),
           0);
}

// init_cache_regs — L2 cache control. Port of
//   mmhub_v4_1_0_init_cache_regs   (mmhub_v4_1_0.c:216)
//   gfxhub_v12_0_init_cache_regs   (gfxhub_v12_0.c:212)
//
// CNTL3/4/5 are programmed from per-IP DEFAULT seeds, then have a few
// fields overridden via REG_SET_FIELD — that matches upstream exactly.
// MMHUB and GFXHUB use different DEFAULT constants:
//   MMHUB CNTL3 default = 0x80100007
//   GFXHUB CNTL3 default = 0x80120007
//   both CNTL4 default   = 0x000000c1
//   both CNTL5 default   = 0x00003fe0
static void
hub_init_cache_regs(const DeviceContext &dev, const HubContext &h)
{
    uint32_t reg, tmp;

    // ---- L2_CNTL ----
    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl);
    tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_CACHE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL,
                        L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL2 ----
    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl2);
    tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL3 ----
    // Upstream regMMVM_L2_CNTL3_DEFAULT  = 0x80100007 (MMHUB)
    //          regGCVM_L2_CNTL3_DEFAULT  = 0x80120007 (GFXHUB)
    // translate_further == false → BANK_SELECT=9, BIGK_FRAGMENT=6.
    const uint32_t kCNTL3_DEFAULT = (h.ip == IPBlock::GC)
                                  ? 0x80120007u   // GFXHUB
                                  : 0x80100007u;  // MMHUB
    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl3);
    tmp = kCNTL3_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, BANK_SELECT, 9);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL4 ----
    // Upstream regMMVM_L2_CNTL4_DEFAULT = 0x000000c1.
    // Audit #4 F12 fix: VMC_TAP_PDE/PTE_REQUEST_PHYSICAL shifts were
    // wrong in the field-defs (0x1/0x2 → 0x6/0x7).
    constexpr uint32_t kCNTL4_DEFAULT = 0x000000c1u;
    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl4);
    tmp = kCNTL4_DEFAULT;
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
    WREG32(dev, reg, tmp);

    // ---- L2_CNTL5 ----
    // Upstream regMMVM_L2_CNTL5_DEFAULT = 0x00003fe0.
    constexpr uint32_t kCNTL5_DEFAULT = 0x00003fe0u;
    reg = SOC15_REG_OFFSET(dev, h.ip, h.vm_l2_cntl5);
    tmp = kCNTL5_DEFAULT;
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

// setup_vmid_config — per-VMID CONTEXT1..15 enable with fault bits.
// Port of:
//   mmhub_v4_1_0_setup_vmid_config  (mmhub_v4_1_0.c:305)
//   gfxhub_v12_0_setup_vmid_config  (gfxhub_v12_0.c:298)
//
// Audit #4 F16: the original implementation only programmed CONTEXT_CNTL
// (and got the bit positions wrong via hand-coded shifts in
// amdgpu_gart.cpp). This now matches upstream:
//   - Uses REG_SET_FIELD with the corrected MMVM_CONTEXT1_CNTL field
//     defs (RANGE=0xb DUMMY=0xd PDE0=0xf VALID=0x11 READ=0x13 WRITE=0x15
//     EXECUTE=0x17, PAGE_TABLE_BLOCK_SIZE=0x4, RETRY=0x8).
//   - Programs CONTEXT[i+1]_PAGE_TABLE_START/END with 0 / (max_pfn-1).
//   - Uses ctx_addr_distance (=2) for PT addr regs and ctx_distance (=1)
//     for the CNTL reg — these are different strides on RDNA4.
static void
hub_setup_vmid_config(const DeviceContext &dev, const GMCContext &gmc,
                      const HubContext &h)
{
    uint32_t cntl_base = SOC15_REG_OFFSET(dev, h.ip, h.ctx1_cntl);
    uint32_t st_lo_base = SOC15_REG_OFFSET(dev, h.ip, h.ctx1_pt_start_lo);
    uint32_t st_hi_base = SOC15_REG_OFFSET(dev, h.ip, h.ctx1_pt_start_hi);
    uint32_t en_lo_base = SOC15_REG_OFFSET(dev, h.ip, h.ctx1_pt_end_lo);
    uint32_t en_hi_base = SOC15_REG_OFFSET(dev, h.ip, h.ctx1_pt_end_hi);

    const uint64_t end_pfn = (gmc.max_pfn > 0) ? (gmc.max_pfn - 1) : 0;

    for (int i = 0; i <= 14; i++) {
        uint32_t cntl_reg = cntl_base + i * h.ctx_distance;
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
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            PAGE_TABLE_BLOCK_SIZE,
                            gmc.block_size - 9);
        // no-retry on fault (matches upstream's !amdgpu_noretry default = 1).
        tmp = REG_SET_FIELD(tmp, MMVM_CONTEXT1_CNTL,
                            RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 1);
        WREG32(dev, cntl_reg, tmp);

        // Per-VMID PT start = 0, end = max_pfn - 1. Use ctx_addr_distance
        // for these (=2 on RDNA4), NOT ctx_distance.
        WREG32(dev, st_lo_base + i * h.ctx_addr_distance, 0);
        WREG32(dev, st_hi_base + i * h.ctx_addr_distance, 0);
        WREG32(dev, en_lo_base + i * h.ctx_addr_distance,
               (uint32_t)(end_pfn & 0xFFFFFFFFu));
        WREG32(dev, en_hi_base + i * h.ctx_addr_distance,
               (uint32_t)(end_pfn >> 32));
    }
}

// program_invalidation — arm invalidation engines 0..17 over the full
// address range. Port of:
//   mmhub_v4_1_0_program_invalidation (mmhub_v4_1_0.c:355)
//   gfxhub_v12_0_program_invalidation (gfxhub_v12_0.c:347)
//
// Audit #4 F6: must use eng_addr_distance (=2) for the ADDR_RANGE
// regs, NOT eng_distance (=1). Previously we used a single combined
// stride of 4 which was wrong for both.
static void
hub_program_invalidation(const DeviceContext &dev, const HubContext &h)
{
    uint32_t lo_base = SOC15_REG_OFFSET(dev, h.ip,
                                        h.vm_invalidate_eng0_addr_range_lo32);
    uint32_t hi_base = SOC15_REG_OFFSET(dev, h.ip,
                                        h.vm_invalidate_eng0_addr_range_hi32);
    for (int i = 0; i < 18; i++) {
        WREG32(dev, lo_base + i * h.eng_addr_distance, 0xFFFFFFFFu);
        WREG32(dev, hi_base + i * h.eng_addr_distance, 0x1Fu);
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

    // Order mirrors mmhub_v4_1_0_gart_enable (mmhub_v4_1_0.c:368).
    hub_init_gart_aperture_regs(dev, gmc, gmc.mmhub);
    hub_init_system_aperture_regs(dev, gmc, gmc.mmhub);
    hub_init_tlb_regs(dev, gmc.mmhub);
    hub_init_cache_regs(dev, gmc.mmhub);
    hub_enable_system_domain(dev, gmc.mmhub);
    hub_disable_identity_aperture(dev, gmc.mmhub);
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

    // GFXHUB field layouts mirror MMHUB exactly (see GCVM_* → MMVM_*
    // aliases in amdgpu_field_defs.h); REG_SET_FIELD resolves to the
    // same SHIFT/MASK constants. Order mirrors gfxhub_v12_0_gart_enable
    // (gfxhub_v12_0.c:360).
    hub_init_gart_aperture_regs(dev, gmc, gmc.gfxhub);
    hub_init_system_aperture_regs(dev, gmc, gmc.gfxhub);
    hub_init_tlb_regs(dev, gmc.gfxhub);
    hub_init_cache_regs(dev, gmc.gfxhub);
    hub_enable_system_domain(dev, gmc.gfxhub);
    hub_disable_identity_aperture(dev, gmc.gfxhub);
    hub_setup_vmid_config(dev, gmc, gmc.gfxhub);
    hub_program_invalidation(dev, gmc.gfxhub);

    GMC_LOG("gfxhub_gart_enable done");
    return kIOReturnSuccess;
}

// gmc_set_fault_enable_default — port of:
//   mmhub_v4_1_0_set_fault_enable_default  (mmhub_v4_1_0.c:415)
//   gfxhub_v12_0_set_fault_enable_default  (gfxhub_v12_0.c:417)
//
// Audit #4 F23: must be called after gart_enable so the VM L2 actually
// redirects faults to the dummy_page we set up.
kern_return_t
gmc_set_fault_enable_default(DeviceContext &dev,
                             const HubContext &hub, bool value)
{
    if (!hub.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(hub.ip)) return kIOReturnNotReady;

    uint32_t reg = SOC15_REG_OFFSET(dev, hub.ip,
                                    hub.vm_l2_protection_fault_cntl);
    uint32_t tmp = RREG32(dev, reg);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        NACK_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                        EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
    if (!value) {
        tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                            CRASH_ON_NO_RETRY_FAULT, 1);
        tmp = REG_SET_FIELD(tmp, MMVM_L2_PROTECTION_FAULT_CNTL,
                            CRASH_ON_RETRY_FAULT, 1);
    }
    WREG32(dev, reg, tmp);
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
    // Mirrors gmc_v12_0_gart_enable (gmc_v12_0.c:1019).
    if (dev.ip.isResolved(IPBlock::HDP)) {
        gmc_hdp_flush(dev);
    }

    // set_fault_enable_default — wire the L2 protection-fault unit to
    // redirect faults to dummy_page rather than crash the engines.
    // Audit #4 F23. gmc_v12_0.c:1023.
    if (dev.ip.isResolved(gmc.mmhub.ip)) {
        kern_return_t fr = gmc_set_fault_enable_default(dev, gmc.mmhub, true);
        if (fr != kIOReturnSuccess) {
            GMC_LOG("mmhub set_fault_enable_default failed: %#x", fr);
        }
    }

    // Flush TLBs on both hubs (vmid=0, legacy flush type). This
    // ensures the page tables are picked up before the first GPU
    // memory access through GART.
    // Mirrors gmc_v12_0_gart_enable (gmc_v12_0.c:1024).
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
// armed in hub_program_invalidation. Port of:
//   gmc_v12_0_flush_gpu_tlb / gmc_v12_0_flush_vm_hub  (gmc_v12_0.c:213+305)
//   mmhub_v4_1_0_get_invalidate_req                   (mmhub_v4_1_0.c:67)
//   gfxhub_v12_0_get_invalidate_req                   (gfxhub_v12_0.c:61)
//
// Audit #4 F24: the original implementation hard-coded shifts of
// 0,4..11 which were wrong — the correct request-word shifts are
// 0 (PER_VMID, 16-bit field), 16 (FLUSH_TYPE), 19-23 (L2/L1 invalidates),
// 24 (CLEAR_PFS). Now built via REG_SET_FIELD so the SHIFT/MASK pairs
// in amdgpu_field_defs.h are the single source of truth.
//
// Upstream also targets engine 17 for GART flushes (not engine 0) to
// avoid contention with on-the-fly per-submit flushes; we match that.
// ============================================================
kern_return_t
gmc_flush_gpu_tlb(DeviceContext &dev, const GMCContext &gmc,
                  const HubContext &hub, uint32_t vmid, uint32_t flush_type)
{
    (void)gmc;
    if (!hub.inited || !dev.ip.isResolved(hub.ip)) return kIOReturnNotReady;

    // Build the request word via REG_SET_FIELD — same pattern as
    // {mmhub,gfxhub}_v*_get_invalidate_req upstream.
    uint32_t req = 0;
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ,
                        PER_VMID_INVALIDATE_REQ, 1u << vmid);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ,
                        FLUSH_TYPE, flush_type & 0x7);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PTES, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE0, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE1, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE2, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ, INVALIDATE_L1_PTES, 1);
    req = REG_SET_FIELD(req, MMVM_INVALIDATE_ENG0_REQ,
                        CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0);

    // Upstream gmc_v12_0_flush_vm_hub uses engine 17 for GART flushes
    // (gmc_v12_0.c:221: "Use register 17 for GART").
    constexpr unsigned kFlushEng = 17;

    const uint32_t req_reg = SOC15_REG_OFFSET(dev, hub.ip,
                                              hub.vm_invalidate_eng0_req)
                           + kFlushEng * hub.eng_distance;
    const uint32_t ack_reg = SOC15_REG_OFFSET(dev, hub.ip,
                                              hub.vm_invalidate_eng0_ack)
                           + kFlushEng * hub.eng_distance;

    WREG32(dev, req_reg, req);

    // Poll ack bit for this vmid. Upstream loops until usec_timeout
    // (default 100 ms).
    uint32_t expected = (1u << vmid);
    uint32_t value = 0;
    if (!poll_reg(dev, ack_reg, expected, expected,
                  /*timeout_us*/ 100000, &value)) {
        GMC_LOG("flush_gpu_tlb: ack timeout (req=%#x ack=%#x ip=%u eng=%u)",
                req, value, (unsigned)hub.ip, kFlushEng);
        return kIOReturnTimeout;
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
