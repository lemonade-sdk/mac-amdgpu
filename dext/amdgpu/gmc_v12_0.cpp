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
#include "amdgpu_gmc.h"

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

    GMC_LOG("GMCInit foundation complete (MMIO programming + GART "
            "alloc pending — see docs/port_plans/GMC_v12.md)");
    dev.gmcReady = true;  // partial — actual hardware enable still pending
    return kIOReturnSuccess;
}

} // namespace amdgpu
