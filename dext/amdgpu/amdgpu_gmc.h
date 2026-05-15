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

#ifdef __APPLE__
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#endif

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
    uint32_t  ctx1_pt_start_lo;  // CONTEXT1 PT start/end — programmed
    uint32_t  ctx1_pt_start_hi;  // per-VMID in setup_vmid_config.
    uint32_t  ctx1_pt_end_lo;
    uint32_t  ctx1_pt_end_hi;
    uint32_t  ctx0_cntl;
    uint32_t  ctx1_cntl;
    uint32_t  vm_l2_cntl;
    uint32_t  vm_l2_cntl2;
    uint32_t  vm_l2_cntl3;
    uint32_t  vm_l2_cntl4;
    uint32_t  vm_l2_cntl5;
    uint32_t  vm_l2_protection_fault_cntl;
    uint32_t  vm_l2_protection_fault_cntl2;
    uint32_t  vm_l2_protection_fault_default_addr_lo32;
    uint32_t  vm_l2_protection_fault_default_addr_hi32;
    // Identity aperture — closed during gart_enable
    uint32_t  vm_l2_ctx1_identity_aperture_low_lo32;
    uint32_t  vm_l2_ctx1_identity_aperture_low_hi32;
    uint32_t  vm_l2_ctx1_identity_aperture_high_lo32;
    uint32_t  vm_l2_ctx1_identity_aperture_high_hi32;
    uint32_t  vm_l2_ctx_identity_physical_offset_lo32;
    uint32_t  vm_l2_ctx_identity_physical_offset_hi32;
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
    // AGP / FB location regs (MMHUB only really; GFXHUB shadows them).
    uint32_t  vm_agp_base;
    uint32_t  vm_agp_top;
    uint32_t  vm_agp_bot;
    uint32_t  vm_fb_location_base;
    uint32_t  vm_fb_location_top;
    uint32_t  vm_fb_offset;

    // Strides between per-VMID/per-engine register blocks. Upstream
    // computes these as `regCONTEXT1_X - regCONTEXT0_X` in the hub
    // init function. On RDNA4 these are:
    //   ctx_distance       = 1   (CONTEXT1_CNTL - CONTEXT0_CNTL)
    //   ctx_addr_distance  = 2   (CONTEXT1_PT_BASE_ADDR_LO32 - CONTEXT0_PT_BASE_ADDR_LO32)
    //   eng_distance       = 1   (INVALIDATE_ENG1_REQ - INVALIDATE_ENG0_REQ)
    //   eng_addr_distance  = 2   (INVALIDATE_ENG1_ADDR_RANGE_LO32 - INVALIDATE_ENG0_ADDR_RANGE_LO32)
    // Audit #4 F6: previously eng_distance was hard-coded to 4 — that's
    // wrong. The REQ/ACK/SEM stride is 1; only the ADDR_RANGE stride is 2.
    uint32_t  ctx_distance;
    uint32_t  ctx_addr_distance;
    uint32_t  eng_distance;
    uint32_t  eng_addr_distance;

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

    // VM manager parameters (mirrors upstream adev->vm_manager).
    uint32_t  num_level;            // 3 for GFX12 (4-level PTs)
    uint32_t  block_size;           // 9 — log2 of entries per block
    uint64_t  max_pfn;              // (1 << 48) / 4 KB for GFX12
    uint64_t  vram_base_offset;     // GPU-VA base of FB in physical address space

    // Bump allocator over the visible VRAM aperture (top-down).
    VRAMBumpAllocator vram_alloc;

    // Resources allocated at gart_init time. All DART-mapped sysmem.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *gart_pt_buf;
    IODMACommand             *gart_pt_dma;
    IOBufferMemoryDescriptor *dummy_page_buf;
    IODMACommand             *dummy_page_dma;
    IOBufferMemoryDescriptor *mem_scratch_buf;
    IODMACommand             *mem_scratch_dma;
#endif
    uint64_t  gart_pt_bus;          // GPU-side bus address of GART page table
    void     *gart_pt_cpu;
    uint64_t  gart_pt_size;         // page table size in bytes
    uint64_t  dummy_page_bus;       // for protection-fault redirect
    uint64_t  mem_scratch_bus;      // default aperture address

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

// Allocate GART page table + dummy_page + mem_scratch in DART-mapped
// system memory. ~128 KB + 16 KB + 16 KB total — well under DART
// ceiling. Mirrors amdgpu_gart_table_ram_alloc + the dummy_page +
// mem_scratch allocations Linux does in gmc_v12_0_sw_init.
kern_return_t gmc_alloc_resources(DeviceContext &dev, GMCContext &gmc);

// Free everything alloc_resources allocated.
void gmc_release_resources(GMCContext &gmc);

// Hub-level gart_enable: actual register programming. Each is a
// port of upstream mmhub_v4_1_0_gart_enable / gfxhub_v12_0_gart_enable
// composed of the six sub-functions Linux structures it into:
//   init_gart_aperture_regs   — CONTEXT0 PT base/start/end
//   init_system_aperture_regs — AGP, system aperture, default page
//   init_tlb_regs             — L1 TLB on, MTYPE_UC, advanced model
//   init_cache_regs           — L2 cache fields
//   enable_system_domain      — CONTEXT0 enable
//   disable_identity_aperture — close the identity range
//   setup_vmid_config         — VMID 1..14 contexts
//   program_invalidation      — invalidation engines 0..17
kern_return_t gmc_mmhub_gart_enable(DeviceContext &dev, GMCContext &gmc);
kern_return_t gmc_gfxhub_gart_enable(DeviceContext &dev, GMCContext &gmc);

// HDP flush — port of amdgpu_device_flush_hdp. Writes the HDP
// memory-read-cache invalidate register. Used after writing PTEs
// or after gart_enable so the GPU sees fresh state.
kern_return_t gmc_hdp_flush(DeviceContext &dev);

// flush_gpu_tlb — uses the invalidation engines hub_program_invalidation
// armed. Writes a request to VM_INVALIDATE_ENG0_REQ, polls ACK.
// vmid: 0..15 (0 = system, 1..14 = user, 15 = all)
// flush_type: 0 = legacy, 1 = light-weight, 2 = heavyweight
kern_return_t gmc_flush_gpu_tlb(DeviceContext &dev, const GMCContext &gmc,
                                const HubContext &hub,
                                uint32_t vmid, uint32_t flush_type);

// set_fault_enable_default — port of mmhub_v4_1_0_set_fault_enable_default
// + gfxhub_v12_0_set_fault_enable_default. Programs
// MMVM_L2_PROTECTION_FAULT_CNTL / GCVM_L2_PROTECTION_FAULT_CNTL. Called
// after gart_enable with value=true.
kern_return_t gmc_set_fault_enable_default(DeviceContext &dev,
                                           const HubContext &hub, bool value);

// Top-level GMCInit stage entry — runs the chain.
kern_return_t gmc_init(DeviceContext &dev, GMCContext &gmc);

} // namespace amdgpu
