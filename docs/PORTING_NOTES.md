# Porting notes

Significant divergences between the upstream Linux amdgpu driver and
this dext port. The bullets here capture *why* we deviated, not what —
the what is obvious from the code.

Last reviewed: Phase 1B chunk 28.

---

## Apple Silicon DART vs Linux IOMMU

Apple's DART sits between the GPU and host memory. The hard limits
that shape every allocator and DMA path in this dext are documented
fully in `docs/AS_DART_LIMITS.md`. The short list:

- **16 KB page size**, not 4 KB. Every buffer handed to the GPU must
  be 16 KB-aligned + 16 KB-multiple in size. Every alloc in the dext
  uses `kASPageSize` (16384) and rounds up.
- **1.5 GB ceiling** on the total DART-mapped sysmem footprint per
  PCI device. We track this loosely via `MACAMDGPU_DMA_BUFFER_MAX`
  (1536 MB) on the per-client DMABuffer.
- **64k mapping cap** on the number of distinct DART mappings.
  Translation: prefer large contiguous buffers over many small ones.
  Our BO sub-range allocator hands out windows inside one DMABuffer
  per client rather than one DART mapping per BO.
- **BAR writes are ~10× slower** than reads. We avoid per-register
  read-modify-write loops on the hot path where we can.
- **BAR config-register reads need masking** — qemu-vfio-apple's dext
  synthesises some PCI config register values that the host's
  PCIDriverKit returns garbage for. We don't hit this in our path
  yet; called out in case we do.

## VBIOS / ATOM tables

Linux pulls VBIOS via the PCI ROM BAR and runs an ATOM bytecode
interpreter over it. On Apple Silicon eGPU, the ROM BAR isn't reliably
enumerated and there's no clean PCIDriverKit path to read it.

What we did instead:
- VRAM size comes from `mmRCC_CONFIG_MEMSIZE` (already exposed via the
  bootstrap-register reads in `amdgpu_discovery::discover_ips_on_die`).
- VRAM type / width / vendor (item 184) — currently hardcoded for
  R9700 in the IP version table. Long-term plan is to read GDDR
  config off the SMU once PMFW is alive.
- IP discovery binary comes from the on-die TMR via the same path
  Linux uses for ASICs without VBIOS reliance (`amdgpu_discovery.c`
  `get_tmr_info`-style read of `(vram_size << 20) - 0x100000` via
  BAR2). Implemented in `discover_ips_on_die`.

## IP base resolution

Linux walks a runtime IP discovery binary to fill in per-IP base
register offsets. We do the same (`amdgpu_discovery.cpp`) but with two
fallbacks:

1. **On-die path** (preferred) — `mmRCC_CONFIG_MEMSIZE` + BAR2 read +
   the existing parser. Self-bootstrapping, no user action needed.
2. **LoadDiscoveryBin selector** — userspace uploads a captured binary
   to the DMABuffer; dext parses it directly. Useful when on-die is
   blocked (e.g. PSP put the binary in sysmem via `DRIVER_SCRATCH_*`
   which AS PCIDriverKit doesn't let us map directly).

The hardcoded IP version constants in `amdgpu_ip.h` (`kIP_GFX`,
`kIP_SDMA`, etc.) are version pins for R9700 only — they're verified
against the discovered binary on every boot but never consulted as a
base-address source.

## CP_RB0 direct path vs MES-scheduled queues

Upstream amdgpu funnels every gfx/compute submission through MES once
the scheduler is live. We keep a direct CP_RB0 path open *in addition*
to MES so that:

- `SubmitTestPM4` (Hello PM4) works without MES being up — useful as
  a smoke test before/after firmware loads.
- `SubmitIB` from the BO UAPI uses CP_RB0 today; switching to MES
  ADD_QUEUE-managed queues is a Phase 2A item.

MES SET_HW_RESOURCES is configured to leave `gfx_hqd_mask[0] = 0xFE`
(HQDs 1..7 owned by MES, HQD 0 reserved). That keeps both paths alive.

## Firmware loading

Linux loads firmware either through PSP (`psp_load_ip_fw`) or
directly via `AMDGPU_FW_LOAD_DIRECT` writes to IC/DC memory. We only
implement the PSP path — DIRECT requires writing into IP-private
instruction memory which is harder to validate without traces.

Side effect: MES needs the firmware header parsed to extract
`mes_uc_start_addr`. Linux gets that for free from the firmware-loader
infrastructure; we do it inline in the LoadFirmware selector when
fwType is RS64_MES or RS64_KIQ. The byte offsets (dword 16/17 inside
the firmware blob) come from `mes_firmware_header_v1_0` in
`amdgpu_ucode.h`.

## Doorbell allocation

Linux uses a per-device IDA for doorbells. We use a static layout for
Phase 1B:

```
CP_RB0          slot 0x00
SDMA0           slot 0x10
SDMA1           slot 0x12
MES SCHED       slot 0x20
MES KIQ         slot 0x22
aggregated[0..4] slots 0x100 .. 0x104
```

User queues created via MES ADD_QUEUE get whatever the caller passes
in `doorbell_offset`. There's no allocator yet — first conflict will
be caught by visual inspection of os_log output. Real bitmap allocator
is a Phase 2A item.

## SRBM / GRBM mutex

Linux uses a sleeping mutex around `soc21_grbm_select` writes. We
don't have a sleeping mutex in DriverKit user-mode dext context, and
our dispatcher is single-threaded per UserClient. We document this
contract in `mes_queue_init` and `mes_enable` and otherwise call
`grbm_select` without locking.

The contention case (two clients racing GRBM_GFX_CNTL) would corrupt
each other's HQD writes. We're single-client per IOServiceOpen so
this doesn't bite today. Adding a real lock is a Phase 2A item.

## Power management

PMFW (item 173, 174–176) is partially ported — mailbox primitives
work, but pptable upload + clock control isn't wired up. We rely on
whatever clocks the GPU comes up in. Real workloads will need
`smu_v14_0_init_smc_tables` + `smu_v14_0_set_clocks` ported.

## Not ported

For completeness, the upstream pieces we explicitly chose *not* to
port:

- KFD (compute kernel mode driver) — userspace ICD does its own
  queue management via MES ADD_QUEUE.
- VCN (video codec) — out of scope for the first product.
- Display engine — R9700 has no display outputs of interest; we
  treat it as a compute/render device only.
- TTM buffer object manager — replaced with our BO sub-range
  allocator on top of a per-client DMABuffer.
- DRM scheduler — replaced with userspace-driven ring submission.
- IOMMU code paths — DART is the IOMMU and PCIDriverKit handles it.
