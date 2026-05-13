# WORKLIST

Granular task list. Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` deferred.

Each task has an ID for cross-reference. Phase numbers match `ROADMAP.md`. Add a `→` annotation when a task spawns sub-work, and a `(see WORKLIST.md#NNN)` link from the spawning task.

---

## Phase 0 — Gates

- [x] 000  Create project skeleton dirs (`dext`, `winsys`, `vk`, `docs`, `scripts`, `firmware`)
- [x] 001  `git init` local repo on `main`
- [x] 002  Clone Linux kernel (sparse: `drivers/gpu/drm/amd`, `include/uapi/drm`, `include/drm`, scheduler, ttm)
- [x] 003  Clone Mesa userspace driver tree
- [x] 004  Clone linux-firmware (sparse: `amdgpu/`)
- [ ] 005  Apply for `com.apple.developer.driverkit.transport.pci` entitlement (Apple Developer portal)
- [ ] 006  Decide & document target Mac platform (recommend: Intel Mac Pro 7,1 or Intel mini + eGPU enclosure on macOS 14/15)
- [ ] 007  Decide & document target ASIC (recommend: Navi 22/23 — RX 6600/6700 series)
- [ ] 008  Acquire target hardware (GPU + Mac + eGPU enclosure if applicable)
- [ ] 009  Stand up Linux reference rig — vanilla Ubuntu 24.04, amdgpu, vkcube working
- [ ] 010  Install `umr` debug tool on Linux rig
- [ ] 011  Capture umr register dump of GPU at idle (baseline)
- [ ] 012  Capture umr submit trace of `vkcube` (reference command stream)
- [ ] 013  Validate llama.cpp Vulkan backend on Linux rig with target GPU
- [ ] 014  Pull firmware blobs for target ASIC; record exact versions in `firmware/MANIFEST.md`
- [ ] 015  Author `NOTICE` + `LICENSE` files for vendored firmware + Mesa
- [ ] 016  Decide debug strategy (`os_log` channel + `IOUserClient` debug method)
- [ ] 017  Write `docs/HARDWARE.md` — pinning the target chip + Mac + macOS version

**Gate to Phase 1:** 005, 006, 007, 008, 009.

---

## Phase 1A — Dext skeleton + PCI plumbing

- [ ] 100  Create Xcode workspace `mac_amdgpu.xcworkspace` with `amdgpu.dext` target
- [ ] 101  Configure PCIDriverKit target — Info.plist `IOKitPersonalities` with VID `0x1002` / DID `<asic>`
- [ ] 102  Add entitlements file referencing `com.apple.developer.driverkit.transport.pci`
- [ ] 103  Sign with provisioning profile bound to the entitlement
- [ ] 104  Implement `Start()` — open IOPCIDevice, read config space, log device ID
- [ ] 105  Map BAR0 (MMIO registers) via `IOMemoryMap`
- [ ] 106  Map BAR2 (VRAM aperture) — note size limits on Intel Mac IOMMU
- [ ] 107  Map BAR5 (doorbells) — verify 4 KB pages
- [ ] 108  Configure PCI bus master + MSI-X
- [ ] 109  Allocate N MSI-X vectors via `IOInterruptDispatchSource`
- [ ] 110  Subclass `IOUserClient`, define method table, expose to userspace
- [ ] 111  Write minimal userspace test (`scripts/dext_ping.swift`) — open service, call `Ping`, verify response
- [ ] 112  Add `IOAddressSegment` + `IODMACommand` plumbing for DMA-able buffers
- [ ] 113  Stub IH ring memory allocation (don't process yet)
- [ ] 114  Add `os_log` category for the dext, surface via `log show` filter

**Milestone 1A:** dext loads, claims device, userspace ping works.

---

## Phase 1B — Hardware bringup

Port order matters — each step depends on prior steps. Cite the Linux source file you're porting from in commit messages.

- [ ] 150  Port ATOMBIOS reader (`amdgpu_atombios.c`) — read ROM, parse header
- [ ] 151  Parse FirmwareInfo table — clocks, memory, asic limits
- [ ] 152  Parse PowerPlay table — initial clock state
- [ ] 153  Port PSP bringup (`psp_v11_0.c`, `amdgpu_psp.c`) — load SOS firmware
- [ ] 154  Port SMU bringup (`smu_v11_0.c`) — handshake, enable basic clocks
- [ ] 155  Port GMC v10 (`gmc_v10_0.c`) — VRAM size detect via SMC
- [ ] 156  Implement GART page tables in system memory (DMA-mapped)
- [ ] 157  Implement VM page table format (Navi 2x — PDE/PTE layout)
- [ ] 158  Map GART aperture; write a test page; read back via BAR2
- [ ] 159  Load GFX firmware (CP_PFP / CP_ME / CP_MEC / RLC) from linux-firmware blobs
- [ ] 160  Initialize KIQ (Kernel Interface Queue) on MEC
- [ ] 161  Allocate GFX ring buffer in VRAM (typically 1–4 MB)
- [ ] 162  Map doorbell for GFX queue
- [ ] 163  Port IH (Interrupt Handler) ring init (`navi10_ih.c`)
- [ ] 164  Drain IH ring on MSI-X — dispatch by source ID
- [ ] 165  Write PM4 `NOP` packet, signal via doorbell, confirm CP advances
- [ ] 166  Write PM4 `WRITE_DATA` to a known VRAM offset; read back via SDMA or BAR2
- [ ] 167  Implement EOP fence — `RELEASE_MEM` packet + interrupt → userspace
- [ ] 168  Hello-PM4 milestone: full submit + fence round-trip
- [ ] 169  Port SDMA ring (`sdma_v5_0.c`) — async copy engine
- [ ] 170  Validate SDMA: system→VRAM→system copy, bit-compare
- [ ] 171  Port MEC compute queue init — separate ring for compute submits
- [ ] 172  Implement BO alloc UAPI (`alloc_bo`, `free_bo`, `map_bo`, `get_pages`)
- [ ] 173  Implement CS submit UAPI (`submit_cs(ring, ib, deps[])`)
- [ ] 174  Implement fence wait UAPI (`wait_fence(handle, timeout_ns)`)
- [ ] 175  Implement info query UAPI (`info(type)` — heap, queues, asic id)
- [ ] 176  Stress test: 10k consecutive submits + fence waits, no leak/hang
- [ ] 177  Power-down + power-up cycle without crash

**Milestone 1B:** programmable AMD GPU under macOS via custom UAPI.

---

## Phase 2A — winsys-mac

- [ ] 200  Define `winsys.h` — match Mesa's `radeon_winsys.h` shape so RADV swaps cleanly
- [ ] 201  Implement `winsys_create` — opens dext IOUserClient
- [ ] 202  Implement `bo_create` / `bo_destroy` / `bo_map` / `bo_unmap`
- [ ] 203  Implement BO domain selector (VRAM / GTT / system)
- [ ] 204  Implement CS context create/destroy
- [ ] 205  Implement CS chunk + IB build (call into Mesa's `ac_pm4`)
- [ ] 206  Implement CS submit — package for IOUserClient, dispatch
- [ ] 207  Implement fence — wrap kernel fence handle
- [ ] 208  Implement timeline semaphore via fence wait-multi
- [ ] 209  Implement `query_info` — surface heap sizes, queue counts, asic id
- [ ] 210  Implement BO export/import via shared mach port (for IOSurface bridging later)
- [ ] 211  Unit test winsys with a hand-rolled PM4 stream (no Vulkan yet)

---

## Phase 2B — Vulkan ICD

- [ ] 250  Set up meson build with macOS cross-file
- [ ] 251  Vendor `mesa/src/util/` (with macOS shims for the small Linux-only bits)
- [ ] 252  Vendor `mesa/src/compiler/nir`, `mesa/src/compiler/spirv`
- [ ] 253  Vendor `mesa/src/amd/common`, `mesa/src/amd/registers`, `mesa/src/amd/addrlib`
- [ ] 254  Vendor `mesa/src/amd/compiler` (ACO)
- [ ] 255  Vendor `mesa/src/vulkan/runtime` (vk_* scaffolding)
- [ ] 256  Port `radv_instance` / `radv_physical_device` (point at our winsys)
- [ ] 257  Port `radv_device` — strip graphics/render-pass/WSI for now
- [ ] 258  Port command pool + command buffer
- [ ] 259  Port compute pipeline (`radv_pipeline_compute.c`)
- [ ] 260  Port descriptor set / pipeline layout
- [ ] 261  Port buffer, image (compute-usable storage images), memory binding
- [ ] 262  Port fence, semaphore (binary + timeline)
- [ ] 263  Port shader module + SPIR-V → NIR → ACO → ISA
- [ ] 264  Write ICD JSON; install to `/usr/local/share/vulkan/icd.d/`
- [ ] 265  `vulkaninfo` enumerates the AMD device

---

## Phase 2C — Compute validation

- [ ] 300  Hand-rolled "vkAdd" — two buffers, compute shader sum, readback
- [ ] 301  `vkcube --compute` runs
- [ ] 302  Run `dEQP-VK.api.compute.*` subset — collect pass/fail
- [ ] 303  Run `dEQP-VK.compute.basic.*` — target 80% pass
- [ ] 304  Build llama.cpp with Vulkan backend (`-DGGML_VULKAN=ON`)
- [ ] 305  Load TinyLlama / Phi-2 GGUF, single forward pass
- [ ] 306  Output vs CPU reference within tolerance
- [ ] 307  Benchmark tokens/sec vs Linux reference rig

**Milestone 2:** llama.cpp inference on AMD GPU under macOS, no Metal.

---

## Phase 3A — Graphics pipeline

- [ ] 350  Re-enable RADV render pass code
- [ ] 351  Re-enable RADV graphics pipeline (vertex through fragment)
- [ ] 352  Port framebuffer + attachment handling
- [ ] 353  Port image tiling via addrlib for color + depth
- [ ] 354  `vkcube` (full graphics, offscreen render to BO)
- [ ] 355  Readback rendered BO to file; visual-diff against Linux reference

---

## Phase 3B — WSI for macOS

- [ ] 400  Implement `VK_EXT_metal_surface` extension
- [ ] 401  `VkSurfaceKHR` backed by `CAMetalLayer*`
- [ ] 402  Swapchain — allocate images as IOSurface-exportable BOs in VRAM
- [ ] 403  Render path: GFX ring renders to VRAM IOSurface
- [ ] 404  Present path: SDMA copy VRAM → system-memory IOSurface (upstream PCIe)
- [ ] 405  Hand system IOSurface to `CAMetalLayer.contents` (or via Metal texture import)
- [ ] 406  VSync via `CVDisplayLink`
- [ ] 407  `vkcube` (full graphics, on-screen)
- [ ] 408  Triple buffering with in-flight present pipeline

---

## Phase 3C — Optimization

- [ ] 450  Profile copy-back — confirm PCIe upstream is saturated
- [ ] 451  Implement tile-delta copy (addrlib tile coords)
- [ ] 452  Pipeline submit + copy + present with N in-flight frames
- [ ] 453  Target: 1080p @ 60 fps for `vkmark`; 4K @ 30 fps for `vkQuake`
- [ ] 454  Latency measurement: input-to-photon < 33 ms p99 at 1080p

---

## Phase 4 — Polish

- [ ] 500  Multi-queue (graphics + compute + transfer) concurrent submits
- [ ] 501  Power management — idle clocks, boost under load
- [ ] 502  Hot-plug — eGPU disconnect/reconnect graceful handling
- [ ] 503  Notarized signed dext build
- [ ] 504  Brew tap formula
- [ ] 505  Public README + install guide
- [ ] 506  Bug tracker triage cadence

---

## Cross-cutting

- [ ] 600  Continuous bit-compare against Linux umr traces for PM4 streams
- [ ] 601  Per-commit smoke test in CI: load dext, ping, alloc BO, submit NOP
- [ ] 602  ASAN/UBSAN build of userspace ICD
- [ ] 603  Memory leak audit (BO lifetime, fence cleanup)
- [ ] 604  Documentation: `docs/PORTING_NOTES.md` — every divergence from Linux amdgpu
