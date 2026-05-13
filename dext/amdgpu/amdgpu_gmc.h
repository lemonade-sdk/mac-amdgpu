//
//  amdgpu_gmc.h — GMC v12 / MMHUB v4_1_0 / GFXHUB v12_0 interface.
//
//  Ports a subset of:
//      drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c
//      drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c
//      drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c
//
//  See docs/port_plans/GMC_v12.md for the full 17-step checklist.
//  This first cut covers steps 1, 3, 4, 5, 6 (info gather + offset
//  table population, no MMIO programming yet) and stands up the
//  VRAM bump allocator.
//

#pragma once

#include <stdint.h>
#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_vram.h"

namespace amdgpu {

// Per-hub register offset table — MMHUB and GFXHUB have parallel
// register layouts so we keep a single struct shape and populate
// it differently per hub. Linux's `struct amdgpu_vmhub` is the
// upstream analogue.
//
// All offsets are SOC15-style relative-to-IP-base dword indices.
// To form a final BAR0 register offset:
//     dword_off = SOC15_REG_OFFSET(dev, IPBlock::<hub>, hub.<field>)
struct HubContext {
    bool      inited;

    uint32_t  ctx0_pt_base_lo;   // GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 (or MMHUB equiv)
    uint32_t  ctx0_pt_base_hi;
    uint32_t  ctx0_pt_start_lo;
    uint32_t  ctx0_pt_start_hi;
    uint32_t  ctx0_pt_end_lo;
    uint32_t  ctx0_pt_end_hi;
    uint32_t  ctx0_cntl;
    uint32_t  ctx1_cntl;
    uint32_t  vm_l2_cntl;
    uint32_t  vm_l2_cntl2;
    uint32_t  vm_l2_cntl3;
    uint32_t  vm_l2_cntl4;
    uint32_t  vm_l2_cntl5;
    uint32_t  vm_l2_protection_fault_cntl;
    uint32_t  vm_l1_tlb_cntl;
    uint32_t  vm_invalidate_eng0_req;
    uint32_t  vm_invalidate_eng0_ack;
    uint32_t  vm_invalidate_eng0_sem;
    uint32_t  vm_invalidate_eng0_addr_range_lo32;
    uint32_t  vm_invalidate_eng0_addr_range_hi32;
    uint32_t  vm_system_aperture_low_addr;
    uint32_t  vm_system_aperture_high_addr;
    uint32_t  vm_system_aperture_default_addr_lo;
    uint32_t  vm_system_aperture_default_addr_hi;
    uint32_t  vm_context0_default_addr_lo;
    uint32_t  vm_context0_default_addr_hi;

    // Stride between per-VMID context registers (e.g. CONTEXT0_CNTL
    // vs CONTEXT1_CNTL). Used to walk all 14 user VMIDs.
    uint32_t  ctx_distance;
    uint32_t  eng_distance;

    IPBlock   ip;        // which IP this hub lives in (GC for GFXHUB, GMC/MMHUB for MMHUB)
};

// GMC controller state. Populated by gmc_init().
struct GMCContext {
    bool      inited;

    // Memory geometry. We hardcode R9700 32 GB total VRAM for now
    // (confirmed via devcoredump real_vram_size=34208743424). The
    // visible window comes from dev.bar2VisibleVRAMSize at runtime.
    uint64_t  real_vram_size;       // 32 GB on R9700
    uint64_t  visible_vram_size;    // ~256 MB / 1 GB depending on ReBAR
    uint64_t  vram_start;           // GPU-VA base of VRAM
    uint64_t  vram_end;

    // GART aperture (DART-mapped system memory). Initial 256 MB to
    // stay well under DART's 1.5 GB total ceiling.
    uint64_t  gart_size;
    uint64_t  gart_start;
    uint64_t  gart_end;

    // AGP/FB apertures
    uint64_t  fb_start;
    uint64_t  fb_end;
    uint64_t  agp_start;
    uint64_t  agp_end;

    // Bump allocator over the visible VRAM aperture (top-down).
    VRAMBumpAllocator vram_alloc;

    // Hub register offset tables.
    HubContext mmhub;     // MMHUB v4_1_0
    HubContext gfxhub;    // GFXHUB v12_0
};

// mc_init — port of gmc_v12_0_mc_init (gmc_v12_0.c:727). Fills in
// real_vram_size / visible_vram_size / vram_start / vram_end. For
// the first cut we trust dev.bar2VisibleVRAMSize and hardcode the
// total to 32 GB (R9700). A future revision will read VRAM size
// from the discovery binary or mmRCC_CONFIG_MEMSIZE.
kern_return_t gmc_mc_init(DeviceContext &dev, GMCContext &gmc);

// vram_alloc_init — initialize the bump allocator over the visible
// VRAM window. Must run after mc_init.
kern_return_t gmc_vram_alloc_init(DeviceContext &dev, GMCContext &gmc);

// Populate MMHUB v4_1_0 hub offsets. No MMIO — pure offset table init.
// Source: mmhub_v4_1_0_init (drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c:464)
kern_return_t gmc_mmhub_offsets_init(GMCContext &gmc);

// Populate GFXHUB v12_0 hub offsets. No MMIO.
// Source: gfxhub_v12_0_init (drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c)
kern_return_t gmc_gfxhub_offsets_init(GMCContext &gmc);

// Top-level GMCInit stage entry — runs the chain.
kern_return_t gmc_init(DeviceContext &dev, GMCContext &gmc);

} // namespace amdgpu
