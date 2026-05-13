# "Hello PM4" port-order checklist

The minimum path from "PSP/SMU/GMC/IH/MES up" to "submitted one PM4
NOP packet, got an EOP fence back via IH ring, observed in
userspace." Distilled from upstream:

- `drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c` + `gfx_v12_1.c`
- `drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c` / `sdma_v7_1.c`
- `drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c`
- `mesa/src/amd/common/ac_pm4.*` (for the userspace packet builder)

## Sequence

| # | What                                  | Source                  | Mandatory | Cplx |
|---|---------------------------------------|-------------------------|-----------|------|
| 1 | RLC clear-state buffer init           | `gfx_v12_0_rlc_init` (746)    | yes | S |
| 2 | RLC resume                            | `gfx_v12_0_rlc_resume` (2071) | yes | S |
| 3 | Wait for PSP autoload (RLC/PFP/ME/MEC/MES) | `wait_for_rlc_autoload_complete` | yes | S |
| 4 | GFXHUB enable (TLB + system aperture) | `gfx_v12_0_gfxhub_enable` (3554) | yes | S |
| 5 | GFX ring buffer alloc                 | `amdgpu_ring_init` (226)      | yes | S |
| 6 | GFX constants init                    | `gfx_v12_0_constants_init` (1806) | yes | S |
| 7 | Doorbell + ring MMIO setup            | `gfx_v12_0_cp_gfx_resume` (2715) | yes | **M** |
| 8 | KIQ init (compute-side, kernel-mode)  | `gfx_v12_0_kiq_resume` (3461) | yes | M |
| 9 | CP enable + start                     | `gfx_v12_0_cp_gfx_start` (2774) | yes | S |
| — | Submit NOP + RELEASE_MEM              | userspace + ring buffer       | yes | S |
| — | Wait for fence in WB page             | ring->fence_cpu_addr poll     | yes | S |

## Memory requirements

| Object       | Size       | Where                  |
|--------------|------------|------------------------|
| Ring buffer  | 16 KB      | **GTT** (DART-mapped sysmem) |
| RLC CSB      | 4 KB       | VRAM                   |
| KIQ ring     | 4 KB       | VRAM                   |
| KIQ MQD      | 1 page     | VRAM                   |
| Write-back page | 4 KB    | sysmem                 |
| Fence buffer | 1 page     | WB page or VRAM        |

**The GFX ring lives in GTT, not VRAM.** Critical: this fits inside
our 1.5 GB DART budget. RLC CSB + KIQ ring/MQD are VRAM-only — that
means we *do* need a minimal VRAM allocator before "Hello PM4"
works. Without VRAM, we can attempt to put the KIQ ring in GTT too
but RLC CSB is harder to relocate.

## PM4 packet skeletons

### NOP (4 B header, no payload)
```
0xC0000000        // PACKET3 | NOP | count=0
```

### RELEASE_MEM EOP fence (8 dwords)
```
DW0: 0xC8000006   // PACKET3 | RELEASE_MEM | count=6
DW1: CACHE_FLUSH_AND_INV_TS_EVENT
     | (RELEASE_MEM_EVENT_INDEX_FENCE << 8)
     | RELEASE_MEM_GCR_SEQ | RELEASE_MEM_GCR_GL2_WB
     | (CACHE_POLICY_BYPASS << 25)
DW2: (DATA_SEL_64BIT_FENCE << 29) | (INT_SEL_SEND_INT << 24)
DW3: fence_gpu_addr_lo
DW4: fence_gpu_addr_hi
DW5: fence_value_lo
DW6: fence_value_hi
DW7: 0            // pad
```

### Doorbell kick
```
*(volatile u32 *)(BAR5 + (doorbell_index << 3)) = ring->wptr;
```

## Gotchas

- `order_base_2(ring_size / 8)` must be exact — ring size is a power
  of two. Off-by-one → hang.
- `GRBM_GFX_INDEX` selects which pipe/queue subsequent HQD register
  writes target. Wrong select silently scribbles the wrong queue's
  state.
- Doorbell stride is `<< 3` (×8 bytes) on GFX12.
- Fence GPU addr is **qword-aligned** (bit 0..2 must be zero).
- `INT_SEL = SEND_INTERRUPT` requires IH ring to be up; otherwise
  PSP eats the interrupt and the host never wakes.
- VMID 0 is shared system aperture — fine for first PM4, but every
  user-facing queue eventually wants its own VMID.

## SDMA v7_1 minimum

Same recipe as GFX, but simpler — SDMA packets are smaller (no cache
flush dance), the doorbell stride is the same `<< 3`. The HQD
registers live in `SDMA_QUEUE0_*` not `CP_RB0_*`. Add as Phase 1B
deferred-but-soon item; SDMA is the only way to copy VRAM↔system
without going through BAR2's tiny visible window, which we'll need
the moment we have to load anything bigger than ~256 MB.

## Total scope estimate

~1500 lines of port code across 6 files (RLC bits + CP bits + KIQ
bits + ring abstraction + PM4 builders + SDMA). Plus the
`amdgpu_irq_add_id` registration for EOP / VM-fault / RAS sources
so IH dispatch routes the fence interrupt back to userspace.

**Realistic effort if VRAM allocator is in hand:** 3–5 working
sessions of focused porting after PMFW + MES are wired.
