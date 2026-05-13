# GMC v12 port-order checklist

Distilled from a research agent reading upstream Linux's
`drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c` and friends. The work
needed to land VRAM size detection + GART aperture + VM page table
programming for gfx1201 / Radeon AI PRO R9700 on macOS DriverKit.

End state: GPU can translate addresses (VMID 0 system aperture +
GART), TLB primed, fault redirection enabled.

## Dependencies before starting

- IP discovery has populated `IPBlock::GMC` / `IPBlock::GC` /
  `IPBlock::MMHUB` (the MMHUB IP shares its base with GMC).
- PSP SOS + ring + TMR up (already done).
- PMFW load complete and SMU alive enough to gate clocks.
- IH ring active (so VM-fault interrupts route somewhere).

## Steps (port order)

| # | Source                              | Touches               | Mem        | Cplx |
|---|-------------------------------------|-----------------------|------------|------|
| 1 | `gmc_v12_0.c:gmc_v12_0_mc_init` (727) | MMHUB read            | none       | S    |
| 2 | dummy_page + mem_scratch alloc      | none                  | sysmem <1M | S    |
| 3 | `mmhub_v4_1_0.c:mmhub_v4_1_0_init` (464) | none              | none       | S    |
| 4 | `gfxhub_v12_0.c:gfxhub_v12_0_init`  | none                  | none       | S    |
| 5 | `amdgpu_gmc_get_vram_info` (828)    | none (ATOM only)      | none       | S    |
| 6 | VM size config (`amdgpu_vm_adjust_size`) | none             | none       | S    |
| 7 | irq id registration                 | none                  | none       | S    |
| 8 | dma_set_mask_and_coherent (44-bit)  | none                  | none       | S    |
| 9 | `amdgpu_bo_init`                    | none                  | none       | M    |
| 10 | `gmc_v12_0_gart_init` (776)        | none                  | **see below** | M |
| 11 | `gmc_v12_0_vram_gtt_location` (692) | MMHUB read           | none       | S    |
| 12 | `mmhub_v4_1_0_gart_enable` (368)   | MMHUB writes (many)   | none       | **L** |
| 13 | `gfxhub_v12_0_gart_enable` (360)   | GFXHUB / GC writes (many) | none   | **L** |
| 14 | flush_hdp                           | HDP                   | none       | S    |
| 15 | `set_fault_enable_default` (415)    | MMHUB                 | none       | S    |
| 16 | flush_gpu_tlb                       | MMHUB invalidate eng. | none       | S    |
| 17 | umc init_registers (optional)       | UMC (ECC)             | none       | S    |

## Hot points

- **Step 10 substitution required.** Upstream allocates the GART
  page table in VRAM (≈512 MB). We don't have a VRAM allocator yet
  and 512 MB blows half our 1.5 GB DART budget. Use
  `amdgpu_gart_table_ram_alloc()` path → GART table in system memory,
  DART-mapped, 16 KB-aligned. Smaller GART (e.g. 256 MB) is feasible
  while we boot; expand once VRAM allocator lands.
- **Steps 12–13 each touch dozens of registers.** Batching is fine
  (no in-between fences needed), but the order within each function
  matters — port the function body verbatim, not "approximately."
- **VM addressing on v12:** 4-level page tables, 48-bit VA, 512-entry
  block size, `num_level = 3`, `block_size = 9`. Don't reuse v11
  constants.
- **Per-VMID context init in 12 / 13 is a loop of 14 VMIDs** (1..14
  on top of VMID 0 = kernel). Each VMID needs its own
  `regGCVM_CONTEXT*_CNTL`. Don't skip.

## Notes on flush_gpu_tlb

Has a vmid + flush_type signature. For initial bring-up call
`flush_gpu_tlb(adev, 0, AMDGPU_MMHUB0(0), 0)` — invalidate all
TLB entries, vmid 0, flush_type 0 (legacy).
