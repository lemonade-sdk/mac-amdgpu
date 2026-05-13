# IH v7 port-order checklist

The interrupt handler ring inside the GPU: all IP blocks fan their
interrupts through here, and the host drains it. Upstream:
`drivers/gpu/drm/amd/amdgpu/ih_v7_0.c`.

We already have MSI-X plumbing in the dext (`IOInterruptDispatchSource`
+ shared `irqPending/irqEnabled` page). IH sits above that — when
MSI-X fires, we walk the IH ring entries and dispatch by client/source.

## Dependencies

- PSP/SMU/GMC up enough for the IH IP block (OSSSYS) to power on.
- DART mapping budget room for the rings (Ring0 ~256 KB + Ring1
  ~256 KB = ~512 KB, well under 1.5 GB ceiling).

## Steps

| # | Source                                | Touches          | Mem               | Cplx |
|---|---------------------------------------|------------------|-------------------|------|
| 1 | `amdgpu_ih_ring_init` (42)           | none             | sysmem 256K+8 (Ring0), 256K (Ring1) | S |
| 2 | `ih_v7_0_early_init` (578)           | none             | none              | S |
| 3 | `ih_v7_0_init_register_offset` (46)  | none             | none              | S |
| 4 | `ih_v7_0_sw_init` (587)              | none             | sysmem (alloc tracked) | M |
| 5 | `ih_v7_0_toggle_interrupts(false)`   | OSSSYS (disable) | none              | S |
| 6 | `ih_v7_0_enable_ring` (237)          | OSSSYS (ring base/cntl) | none      | M |
| 7 | `ih_v7_0_doorbell_rptr` (210)        | OSSSYS (doorbell) | none             | M |
| 8 | MSI storm + self-IRQ trigger config  | OSSSYS           | none              | S |
| 9 | (v7.1 only) retry CAM setup (398)    | OSSSYS           | none              | S/M |
| 10 | `ih_v7_0_toggle_interrupts(true)`   | OSSSYS           | none              | S |
| 11 | `amdgpu_irq_add_id` per src in GFX/SDMA/GMC drivers | none | none      | S |
| 12 | `amdgpu_ih_process` per IRQ          | OSSSYS (wptr/rptr) | reads ring     | M |

## Ring layout

Each entry is 8 dwords (32 B):

```
DW0: [7:0] client_id | [15:8] src_id | [23:16] ring_id | [27:24] vmid | [31] vmid_src
DW1: timestamp_lo
DW2: timestamp_hi (low 16) | timestamp_src (bit 31)
DW3: [15:0] pasid | [23:16] node_id
DW4..7: src_data[0..3]  — context-specific (EOP fence value, etc.)
```

## Critical interrupt sources for "Hello PM4"

For first-PM4 we only need:

| Client | Src | Meaning |
|---|---|---|
| `SOC21_IH_CLIENTID_GFX` (0x0a) | `GFX_12_0_0__SRCID__CP_EOP_INTERRUPT` (0xB5) | EOP fence — our PM4 RELEASE_MEM landed |
| `SOC21_IH_CLIENTID_GFX` (0x0a) | `GFX_12_0_0__SRCID__CP_ECC_ERROR` (0xC5) | RAS — useful to see if something blew up |
| `SOC21_IH_CLIENTID_ATHUB` (0x02) | `GFX_12_0_0__SRCID__UTCL2_FAULT` (0x00) | VM fault — if address translation broke |

## Hot points

- **16 KB page rule applies** — round the 256 KB rings + 8 B wptr
  shadow each up to 16 KB-aligned alloc. 256 KB is already aligned
  but the shadow needs its own page.
- **MSI-X already wired in the dext.** When IH ring becomes non-empty
  the GPU fires an MSI; we already have that path. The work here is
  the post-MSI ring walk + dispatch.
- **Doorbell-based rptr advance** is preferred over the MMIO write
  (saves a slow BAR write per interrupt) once doorbells are mapped.
- **IH_MSI_STORM_CTRL DELAY=3** is the upstream default — throttles
  pathological VM-fault floods. Use it.
- **Retry CAM** (step 9) only matters on v7.1; gfx1201 may or may not
  have it. Skip on first attempt; revisit if interrupts get dropped.

## Register handles (added to amdgpu_ip.h after porting)

All offsets relative to `IPBlock::OSSSYS`. Sample subset:

```
regIH_RB_BASE             0x0083
regIH_RB_BASE_HI          0x0084
regIH_RB_CNTL             0x0080
regIH_RB_RPTR             0x0081
regIH_RB_WPTR             0x0082
regIH_DOORBELL_RPTR       0x0087
regIH_RB_WPTR_ADDR_LO     0x0086
regIH_RB_WPTR_ADDR_HI     0x0085
regIH_STORM_CLIENT_LIST_CNTL 0x00AA
regIH_INT_FLOOD_CNTL      0x00AB
regIH_MSI_STORM_CTRL      0x00AC
```
