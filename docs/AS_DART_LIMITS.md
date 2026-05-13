# Apple Silicon + DART hard limits

Lessons distilled from scottjg's 2026-05-05 eGPU-Mac-gaming post
(<https://scottjg.com/posts/2026-05-05-egpu-mac-gaming/>) and our
own reading of qemu-vfio-apple's source + traces. These are the
**non-negotiable** constraints that shape any third-party eGPU
driver on Apple Silicon.

## 1. DART (Apple's IOMMU) — three hard ceilings

DART is the IOMMU sitting between the PCIe tunnel and system memory
on Apple Silicon. PCIDriverKit's `IODMACommand` calls into it. Its
limits are **architectural, not bugs** — there is no workaround,
only mitigation.

| Limit | Value | Mitigation |
|---|---|---|
| Total mapped bytes (system memory exposed to GPU) | **~1.5 GB** | Coalesce, recycle aggressively, push everything possible into VRAM instead |
| Mapping count | **~64 k entries** | Cluster small buffers (qemu-vfio-apple uses 4 KB → 256 KB clusters; 4× count reduction) |
| Address/alignment control | **None** | DART picks the IOVA. We must accept whatever bus address `PrepareForDMA` returns; cannot pre-allocate fixed iova ranges |

### Implications for mac_amdgpu

- **llama.cpp working set must fit under 1.5 GB system-mapped.** Model
  weights themselves go in VRAM (R9700 has 32 GB), so this is workable
  if we manage the upload buffers tightly. Some workloads (large
  context windows, KV cache) will be the first to push this.
- **GART page tables, the PSP fw_pri buffer, the PSP ring, ring buffers
  for GFX/SDMA/MES queues, IH ring, and IOSurface scanout copies all
  consume the 1.5 GB budget.** Plan the budget explicitly per phase.
- **64k mapping cap → no fine-grained BO model.** If we end up with a
  Linux-style "every BO is its own DMA mapping" architecture and the
  user runs a Vulkan game with tens of thousands of buffers, we hit
  the cap. Phase 2 winsys must allocate BOs in buckets / pools.

## 2. Apple Silicon page size

- **16 KB**, not 4 KB. DART rejects mappings that aren't 16 KB-aligned.
- Use `amdgpu::kASPageSize = 16384` as the alignment everywhere DMA
  is involved. **Even for things the GPU side only requires 4 KB
  granularity for** (e.g. PSP km_ring) — we allocate at 16 KB
  alignment and waste the slack, because the alternative is a hard
  refusal at `PrepareForDMA`.

## 3. BAR MMIO performance — ~10× slower than native

`hv_vm_map()` has no flag for write-combining (Device-nGnRE ordering),
so prefetchable BAR writes from userspace dexts go through a slower
path. Quoting scottjg: *"hv_vm_map() has no flags to configure this."*

| Path | Relative throughput |
|---|---|
| Native Linux + amdgpu | 1× (baseline) |
| Apple Silicon dext via BAR map | ~0.1× (~10× slower writes) |
| Apple Silicon dext via trap-and-emulate (worst) | ~0.003× (~300× slower) |

### Implications

- **Every WREG32 has real cost.** Low-frequency paths (PSP/SMU mailboxes,
  ring create) are fine. High-frequency paths (per-frame ring submission,
  per-submit fence emit) need to *batch* writes into the ring memory and
  then trigger a single doorbell, not poke MMIO repeatedly.
- **Compute / graphics submission must minimize MMIO.** Ring writes go
  into VRAM/system memory; only the doorbell hits MMIO. We are already
  on this path because that's how amdgpu submission works on Linux too,
  but the AS BAR overhead makes the doorbell-per-submit pattern even
  more important.

## 4. HVF mapping panic (qemu-only context — not us)

For completeness: mapping PCI BARs with `HV_MEMORY_EXEC` panics the
host. We don't use HVF in mac_amdgpu (no VM involved), so this is
not our problem. But it's a useful precedent: AS PCIDriverKit
mappings deserve careful permission flags. Our equivalent: don't
ever try to set `kIOMapAnywhere | kIOMapDefaultCache` on a BAR
descriptor without ensuring it's marked Device memory (PCIDriverKit
does the right thing here by default — just don't override).

## 5. BAR-register-value masking (config space read quirk)

Already documented in `docs/APPLE_VFIO_NOTES.md`. Reading config-space
BAR registers (offsets `0x10..0x24`) returns DART-translated values
with bogus PCI type bits in the low bits. The fix is to **synthesize**
the return value from `IOPCIDevice::GetBARInfo` instead of passing
the raw read through. Any future ConfigRead selector or
amdgpu-userspace path that asks for BAR registers must apply this
substitution.

The "**we can't modify the hardware, so we mask in the read path**"
pattern from qemu-vfio-apple.

## 6. What this means for the project's ceiling

Combined: small visible BAR (256 MB without ReBAR, ~1 GB with the
partial ReBAR you mentioned), DART 1.5 GB system mapping ceiling,
DART 64k mapping cap, 10× slower BAR writes. These are the four
walls of the room. Within them, we can build the entire stack. The
strategies that follow are:

1. **Keep working set small** — design around 1 GB BAR + 1.5 GB
   DART, not the 32 GB of VRAM the card has.
2. **Use SDMA for any VRAM access that doesn't fit in BAR** — SDMA
   reads/writes happen from VRAM to VRAM (or VRAM to GART-mapped
   system memory) without consuming visible BAR.
3. **Coalesce DMA mappings** — every BO group, every PSP firmware
   load, gets one DART mapping, not one per object.
4. **Batch MMIO** — ring-based submission everywhere, doorbells only.

## 7. Open question: TSO / ACTLR_EL1

scottjg's post mentions Apple Silicon TSO requires macOS 15+ and
`ACTLR_EL1` bit manipulation for x86-emulated workloads. This is
DXVK/Rosetta-relevant (Phase 3 graphics with Windows games), not
Phase 1B. Park it on the worklist for the WSI / graphics phase.
