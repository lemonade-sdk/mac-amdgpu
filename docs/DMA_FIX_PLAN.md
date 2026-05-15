# DMA fix plan — eliminate slow BAR0 MMIO staging

Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` deferred.

## Where we are (v0.0.36)

- IP discovery + PSP bootloader + SOS load + PSP ring create all **work** on the R9700.
- All PSP-accessed buffers (`fw_pri`, `ring`, `cmd`, `fence`, `tmr`) are currently
  **VRAM-backed**, accessed CPU-side via per-dword `pci->MemoryWrite32` through the
  BAR0 visible aperture. This works because PSP expects an MC address routed by GMC;
  passing a DART-mapped sysmem bus address fails silently.
- The cost: **~10× slower per-dword writes vs native PCIe** on AS+TB5. Each MMIO
  write takes ~10 µs; staging 1 MB of `fw_pri` per-dword takes ~2.6 seconds. Each
  PSP ring submit currently times out at 10 s because something is still wrong with
  the cmd/fence path — needs verification with fresh dext spawn, but the underlying
  slowness is a real problem regardless.
- The user constraint: **don't permanently park ring/cmd/fence in VRAM** — small
  RDNA3/3.5 cards (8 GB VRAM) shouldn't waste capacity on driver-control buffers.
  Upstream Linux uses GTT (sysmem) + GART for these — that's the right target.

## The four research findings

(Pulled from agents a20147d2797554fc4 + adb0251b8c3d91621 and memory feedback files
`feedback_mac_amdgpu_dma_strategy.md`, `feedback_mac_amdgpu_fw_pri_mc_addr.md`.)

1. **`IOMemoryDescriptor::CreateMapping` + `GetAddress`** — short-term speedup. Map
   the BAR0 aperture once, get a CPU pointer, use native `memcpy`. **10–50×**
   expected. Same pattern qemu-vfio-apple uses successfully
   (`VFIOUserPCIDriver.cpp:1000–1012`). Zero architectural change.

2. **Bootstrap GART before PSP** — long-term architecture. Move
   `fw_pri`/`ring`/`cmd`/`fence` back to sysmem with GART addresses; PSP routes via
   GMC like upstream. Matches the user's "no VRAM waste" requirement. Substantial
   work: needs GMC init reordered ahead of PSP.

3. **SDMA-based copy** — post-SOS only. Use the GPU's SDMA engine to copy
   sysmem→VRAM via the GPU's internal bus. Useful for workload data later, not the
   PSP boot path.

4. **`MemoryWrite64` instead of `MemoryWrite32`** — quick win, ~30%. Half the MMIO
   calls. Zero architectural risk.

## Task list (ordered by priority)

### Quick wins (today / this session)

- [x] **DMA-1**  Swap `bar0_memcpy_to_vram` and `bar0_memset_vram` to use
  `MemoryWrite64` where the offset + remaining bytes allow it; fall back to
  `MemoryWrite32` for the last partial dword. Bump version, rebuild, test.
  *Expected: ~30% staging speedup. Risk: low. Files: `dext/amdgpu/amdgpu_regs.h`.
  Done in v0.0.36 — half the MMIO calls, expecting ~30–50% speedup.*

- [ ] **DMA-2**  Audit why TMR/SMU/etc. still time out after v0.0.35→v0.0.36
  changes. Confirm the dext running is actually v0.0.36 (kill+respawn), confirm
  `C2PMSG_64` value matches what we wrote, dump first 16 dwords of the cmd_buf in
  VRAM after a submit attempt to see if PSP wrote a response.
  *Files: `dext/MacAMDGPU.cpp` (extend `DumpPSP` or add `DumpCmdBuf` selector).*

### Big-bang speedup

- [ ] **DMA-3**  Prototype `IOMemoryDescriptor::CreateMapping` on the BAR0 aperture.
  - **Caveat found while implementing:** qemu-vfio-apple's `CreateMapping` calls
    at `VFIOUserPCIDriver.cpp:1000–1012` operate on **sysmem DMA chunks** (created
    via `CreateMemoryDescriptorFromClient`), not on BAR memory. They still use
    `MemoryRead/Write32` for BAR access — same as us. The research agent
    overstated the precedent.
  - PCIDriverKit *does* expose `_CopyDeviceMemoryWithIndex(memoryIndex, &md, ...)`
    which returns an `IOMemoryDescriptor*` for a BAR (see
    `PCIDriverKit.framework/Headers/IOPCIDevice.iig:580`). But it's an internal
    method (`_`-prefixed) and unclear whether `CreateMapping` on the returned
    IOMD will yield a CPU-readable virtual address (BAR memory needs cache-control
    attributes typically managed via Mach VM, not DriverKit).
  - **Next step**: write a small probe — call `_CopyDeviceMemoryWithIndex(bar0)`,
    `CreateMapping(0, 0, 0, 0, 0, &map)`, dereference `map->GetAddress()`. If we
    get a valid pointer that reads/writes pass through to BAR0, we win. If we
    crash or get an error, fall back to plan B (sysmem + GART, DMA-4..9).

  *Expected: 10–50× speedup ON BAR0 OPS IF the probe succeeds. Files:
  `dext/MacAMDGPU.cpp`, `dext/amdgpu/amdgpu_regs.h`. Risk: high — undocumented
  territory.*

- [ ] **DMA-3a**  Same `CreateMapping` treatment for **BAR5** (the register window).
  Register reads/writes happen on every `RREG32`/`WREG32`, including hot poll loops.
  This alone should make `psp_wait_for_bootloader` and fence polls dramatically
  faster.

  *Files: `dext/amdgpu/amdgpu_regs.h`.*

- [ ] **DMA-3b**  Add a host-UI button + diagnostic to bench `bar0_memcpy_to_vram`
  before/after the CreateMapping switch — write a 1 MB pattern, time it, report
  MB/s. Sanity check that the speedup actually materialized.

  *Files: `dext/MacAMDGPU.cpp`, `Host/MacAMDGPUHostApp.swift`.*

### Architecture — GART + sysmem for control buffers

- [ ] **DMA-4**  Read upstream `amdgpu_gart.c` / `gmc_v12_0_gart_*` in full;
  document the minimum bootstrap sequence (alloc GART page table in VRAM, set up
  GMC registers, call `gart_enable`).

  *Files: `docs/PORTING_NOTES.md` (add a `GART bootstrap` section).*

- [ ] **DMA-5**  Port `amdgpu_gart_init` + `amdgpu_gart_table_vram_alloc` +
  `amdgpu_gart_bind` into `dext/amdgpu/amdgpu_gart.{h,cpp}`. GART page table itself
  stays in VRAM (small, ~few KB), accessed via the new BAR0 CPU mapping from DMA-3.

  *Risk: medium — chicken-and-egg with PSP. The page table has to live in VRAM
  before GART is up, which requires CPU-writable VRAM — we have that via BAR0.*

- [ ] **DMA-6**  Port `gmc_v12_0_gart_enable` (writes the GART base register, sets
  up VM context 0 page-table pointers). Mirrors upstream
  `gmc_v12_0_gart_enable`.

- [ ] **DMA-7**  Insert a `BringupStage::GARTEnable` between `PSPInit` and
  `PSPLoadSOS` in `amdgpu_init.cpp`. Allocate the GART page table at a small VRAM
  offset (say after `fw_pri`/ring/cmd/fence), enable GART.

- [ ] **DMA-8**  Refactor PSP `fw_pri` from VRAM-backed back to sysmem
  (`IOBufferMemoryDescriptor`) but bind it through GART. The GPU-MC address PSP
  receives is the GART address (not the DART bus address). Pass that to PSP via
  C2PMSG_36.

  *This is the payoff: 1 MB `fw_pri` staging becomes a sysmem memcpy (microseconds)
  instead of a BAR0 MMIO loop.*

- [ ] **DMA-9**  Same refactor for `ring` (16 KB sysmem + GART), `cmd_buf`
  (16 KB), `fence_buf` (16 KB), `tmr` (4 MB).

- [ ] **DMA-10**  Once DMA-3 to DMA-9 are stable, re-bench the full `Initialize
  GPU` run. Pre-PSP staging should drop from seconds to milliseconds.

### Post-SOS (deferred, future phase)

- [-] **DMA-11**  Port `amdgpu_copy_buffer` (SDMA COPY_LINEAR packet) for bulk
  sysmem→VRAM moves of workload data (model weights, etc.) after SDMA init.
  *Only applies once SOS is alive + SDMA ring is up — different problem space than
  the PSP boot path.*

## Side-quests discovered while doing the above

- [ ] **SQ-1**  Bench: is the 10× slowdown only on writes, or also on reads? Our
  fence poll does `RBAR2_32(...)` (a read) every 1 ms — if reads are equally slow,
  the fence poll itself is part of the latency.

- [ ] **SQ-2**  Investigate whether DriverKit lets us map BAR0 as **write-combined**
  rather than the default cached mode. WC writes coalesce in the CPU's
  store buffer and burst out — even without CreateMapping, this could collapse
  per-dword writes into 64 B PCIe TLPs.

- [ ] **SQ-3**  Once GART is up, deprecate the `MemoryWrite64` / per-dword path
  entirely; only the (very small) PSP register pokes still need
  `pci->MemoryWrite32` to BAR5.

## How this maps back to `WORKLIST.md`

This is a focused sub-plan for the Phase 1B PSP bringup blocker. When tasks here
close, the corresponding WORKLIST item should be checked off too — specifically the
PSP bootloader probe (currently `[~]`), TMRSetup, SMUInit, etc.

## Memory references

- `feedback_mac_amdgpu_dma_strategy.md` — both research findings summarized
- `feedback_mac_amdgpu_fw_pri_mc_addr.md` — why MC addresses are required
- `feedback_mac_amdgpu_bar5_registers.md` — BAR layout (BAR0 = visible VRAM aperture)
- `feedback_mac_amdgpu_no_divergence.md` — match upstream amdgpu when porting
