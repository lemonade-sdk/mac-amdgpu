# GART port plan — unblock PSP ring submissions

`[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

## Why

PSP's ring-DMA path on RDNA4 (and almost certainly RDNA3/3.5) cannot read
VRAM-backed `cmd_buf`/`fence_buf`/`ring_mem` even though the bootloader path
can read VRAM-backed `fw_pri`. Upstream Linux puts these in **GTT (sysmem)**
and PSP routes them via the **GART** GPU IOMMU. We must do the same.

The PSP bootloader stages already worked (PSPInit, PSPLoadSOS, PSPRingCreate
all return ok). The wall is **the first ring submit** (TMRSetup) — PSP sees
our wptr update but ignores the frame, because the `cmd_buf` MC address we
hand it in the ring frame doesn't resolve through any path PSP trusts.

This isn't "later optimization." It's the only remaining blocker on the
PSP boot path.

## What GART is

GART = "Graphics Address Remapping Table" — a flat page-table the GPU's GMC
walks to translate **GPU-side MC addresses** in the GART range to
**host-physical addresses** (sysmem pages). It's the GPU's IOMMU for sysmem.

Layout on modern AMD:
- **Page table itself** lives in VRAM (small — ~few KB for the entries we
  need at boot). One 4 KB page table can map 512 × 4 KB = 2 MB. Allocate
  multiple to cover what we need.
- Each **PTE** is 8 bytes: `<host_phys_addr | flags>`.
- GMC's VM context 0 root register points at the page table base.
- Once active, any GMC access in the GART MC range walks the PTEs.

On Apple Silicon, the host-physical addresses we'll bind are **DART
bus addresses** of our sysmem `IOBufferMemoryDescriptor`s. So the chain is:
GPU → MC addr (GART range) → GMC walks PTE in VRAM → bus addr → DART → host
sysmem. PSP reads sysmem cleanly.

## Concrete tasks

### GART-1: Read upstream and document

- [ ] **GART-1a** Read `amdgpu_gart.c`/`.h` in full. Document the contract for
  `amdgpu_gart_init`, `amdgpu_gart_table_vram_alloc`, `amdgpu_gart_bind`,
  `amdgpu_gart_unbind`.
- [ ] **GART-1b** Read `gmc_v12_0_gart_enable` step-by-step. Document the
  exact register pokes: `regGCMC_VM_FB_LOCATION_BASE`,
  `regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32`, etc.
- [ ] **GART-1c** Document the PTE format on RDNA4. Mirrors
  `gmc_v12_0_get_vm_pte` / `amdgpu_gmc_set_pte_pde`.

### GART-2: Vendor structs + helpers

- [ ] **GART-2a** Create `dext/amdgpu/amdgpu_gart.h` with `GARTContext`
  struct and per-PTE helpers (`gart_make_pte(phys, flags)` etc.).
- [ ] **GART-2b** Add GC IP block registers we'll need to
  `dext/amdgpu/amdgpu_ip.h` (`namespace GCRegs { ... }` with
  `GCMC_VM_FB_LOCATION_BASE`, `GCMC_VM_AGP_BASE`,
  `GCVM_CONTEXT0_*`, etc., for RDNA4 specifically).

### GART-3: Allocate page table in VRAM

- [ ] **GART-3a** Add `kGARTPageTableVRAMOffset` constant in psp_v14_0.cpp's
  VRAM layout (after TMR — e.g. 0x600000). Decide on initial size — 4 KB
  (1 page = 2 MB mapped) is plenty for boot.
- [ ] **GART-3b** Zero-fill the page table via the existing
  `bar0_memset_vram` helper.

### GART-4: Implement `gart_bind`

- [ ] **GART-4a** Given a sysmem `IOBufferMemoryDescriptor` (already
  DMA-prepared, with a DART bus address), allocate the next free GART
  page-table slot.
- [ ] **GART-4b** Write the PTE: `((bus_addr & MASK) | flags)` to the
  page table at the right offset, via `bar0_memcpy_to_vram`.
- [ ] **GART-4c** Return the **GART MC address** for that slot — that's
  what we pass to PSP via the ring frame.

### GART-5: Implement `gart_enable`

- [ ] **GART-5a** Read upstream's `gmc_v12_0_gart_enable` register pokes,
  port one-by-one. Each WREG32 needs the GC IP base from discovery.
- [ ] **GART-5b** Validate by reading back key registers and confirming
  they latched.

### GART-6: Wire into bringup order

- [ ] **GART-6a** Add `BringupStage::GARTEnable` between `PSPInit` and
  `PSPLoadSOS` (or right after PSPRingCreate — confirm what's needed).
- [ ] **GART-6b** Refactor `psp_ring_create` / `psp_setup_tmr` to:
  - Allocate ring/cmd/fence/TMR as sysmem `IOBufferMemoryDescriptor`s.
  - `gart_bind` each to get GART MC addresses.
  - Pass those addresses to PSP.

### GART-7: Validate

- [ ] **GART-7a** Initialize GPU should now get past TMRSetup. fence_buf[0]
  should reflect our fence_value.
- [ ] **GART-7b** SMUInit should also work (SMU mailbox uses MMIO, not the
  PSP ring — but might depend on TMR being set up).

## Order of work this session

1. Read all of upstream `amdgpu_gart.c` + `gmc_v12_0.c gart_enable` first
   (GART-1a, GART-1b) so we don't make assumptions.
2. Stub out the new files with the bare struct + function signatures
   (GART-2).
3. Implement bottom-up: PTE format → gart_bind → gart_enable.
4. Wire into bringup. Test on hardware.

If we hit a chicken-and-egg in the bringup order (GMC init needs PSP, PSP
needs GMC for ring access), we'll document it and plan an alternative
sequencing — but start with the upstream order assuming it just works.
