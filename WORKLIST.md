# WORKLIST

Granular tasks. Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` deferred.

Phase numbers match `ROADMAP.md`. Target hardware fixed: Apple Silicon M5 Pro/Max/Ultra + TB5 + AMD Radeon AI PRO R9700 (gfx1201, RDNA4).

---

## Phase 0 — Gates

- [x] 000  Create project skeleton dirs (`dext`, `winsys`, `vk`, `docs`, `scripts`, `firmware`)
- [x] 001  `git init` local repo on `main`
- [x] 002  Clone Linux kernel (sparse: `drivers/gpu/drm/amd`, `include/uapi/drm`, `include/drm`, scheduler, ttm)
- [x] 003  Clone Mesa userspace driver tree
- [x] 004  Clone linux-firmware (sparse: `amdgpu/`)
- [x] 005  Verify GFX12/RDNA4 firmware blobs present in `linux-firmware/amdgpu/` (`gc_12_0_1_*`, `psp_14_0_3_*`, `smu_14_0_3*`, `sdma_7_0_1`) — confirmed
- [x] 006  Verify Linux source has gfx_v12_1, gmc_v12, mes_v12_1, psp_v14_0, sdma_v7_1, nbio_v7_11 — confirmed
- [ ] 007  Evaluate TinyGPU UAPI — does it expose enough for a Vulkan compute ICD? (Read tinygrad runtime code, talk to TC if needed)
- [ ] 008  Acquire AMD Radeon AI PRO R9700
- [ ] 009  Acquire M5 Max or M5 Ultra Mac + TB5 PCIe enclosure rated for 300W
- [ ] 010  Stand up Linux reference rig (Ubuntu 24.04.3 + kernel 6.17+ + R9700 + amdgpu)
- [ ] 011  Install ROCm 7.0.2+ and Mesa 25.2.8+ on Linux rig
- [ ] 012  Install `umr` on Linux rig
- [ ] 013  Capture `umr` register dump at idle (baseline, archive in `docs/traces/`)
- [ ] 014  Capture `umr` submit trace of vkcube on Linux rig
- [ ] 015  Capture `umr` submit trace of `llama.cpp` forward pass on Linux rig
- [ ] 016  Validate llama.cpp Vulkan backend with R9700 on Linux (tokens/sec baseline)
- [ ] 017  Write `firmware/MANIFEST.md` — exact linux-firmware git SHA + file list for gfx1201
- [ ] 018  Author `NOTICE` + `LICENSE.amdgpu` (AMD's firmware license text)
- [ ] 019  Pin macOS version (likely macOS 26 latest) + DriverKit SDK version; write `docs/HARDWARE.md`
- [ ] 020  Decide debug strategy: `os_log` channel, IOUserClient debug method, Linux umr cross-ref; write `docs/DEBUG.md`

**Gate to Phase 1:** 007 (TinyGPU decision), 008, 009, 010, 016.

---

## Phase 1A — Dext skeleton + entitlement (run in parallel)

- [ ] 100  Create Xcode workspace `mac_amdgpu.xcworkspace`
- [ ] 101  Add `amdgpu.dext` target with PCIDriverKit framework
- [ ] 102  Info.plist `IOKitPersonalities` with VID `0x1002` / DID `0x7550` (or wildcard for gfx1201 family)
- [ ] 103  Entitlements file referencing `com.apple.developer.driverkit.transport.pci`
- [ ] 104  Configure team identity + code signing
- [ ] 105  Build empty dext bundle → register bundle ID + vendor identity on Apple Developer portal
- [ ] 106  **Submit entitlement request** against the registered bundle ID (PARALLEL — Apple review can run while we code)
- [ ] 107  Implement `Start()` — open `IOPCIDevice`, log VID/DID/revision/config space
- [ ] 108  Map BAR0 (registers) via `IOMemoryMap`; sanity-check register read
- [ ] 109  Map BAR2 (VRAM aperture) — log size, verify against R9700 spec (32 GB)
- [ ] 110  Map BAR5 (doorbells)
- [ ] 111  Enable PCI bus master + MSI-X
- [ ] 112  Allocate N MSI-X vectors via `IOInterruptDispatchSource`
- [ ] 113  DART/DMA path — `IODMACommand` for a system-memory buffer
- [ ] 114  DART validation test: write pattern to system buffer, SDMA-copy via raw register poke (later replace with proper SDMA submit), read back, bit-compare
- [ ] 115  Subclass `IOUserClient`, method table with `Ping` stub
- [ ] 116  Write `scripts/dext_ping.swift` — open service, call `Ping`, verify
- [ ] 117  Stub IH ring memory allocation (don't process yet)
- [ ] 118  Add `os_log` category, surface via `log show --predicate 'subsystem == "mac.amdgpu"'`

**Milestone 1A:** dext loads, claims R9700, DMA round-trip works, userspace pings.

---

## Phase 1B — Hardware bringup (RDNA4 / gfx1201)

Cite Linux source file in every commit message (e.g. `psp_v14_0.c:psp_v14_0_bootloader_load_sos`).

- [ ] 150  Port IP discovery (`amdgpu_discovery.c`) — read IP discovery table from VBIOS / on-die
- [ ] 151  Verify reported IP versions match gfx_v12_1 / gmc_v12_0 / psp_v14_0_3 / smu_v14_0_3
- [ ] 152  Port PSP v14 bootloader load (`psp_v14_0.c`) — load `psp_14_0_3_sos.bin`
- [ ] 153  Verify SOS signature, hand off to PSP
- [ ] 154  Load PSP `ta.bin` (trusted apps)
- [ ] 155  Port SMU v14_0_3 mailbox handshake (`smu_v14_0.c`, `smu_v14_0_2_ppt.c`)
- [ ] 156  Enable basic clocks via SMU; verify telemetry readback
- [ ] 157  Port GMC v12 (`gmc_v12_0.c`) — VRAM size detect
- [ ] 158  Implement GART page tables in DART-mapped system memory
- [ ] 159  Implement GFX12 VM page table format (PTE layout differs from GFX11 — verify `gmc_v12_0_emit_flush_gpu_tlb`)
- [ ] 160  Map a test page through GART; verify CPU↔GPU access
- [ ] 161  Load IMU firmware (`gc_12_0_1_imu.bin`)
- [ ] 162  Load RLC firmware (`gc_12_0_1_rlc.bin`) + bringup
- [ ] 163  Load CP firmware (`gc_12_0_1_pfp.bin`, `gc_12_0_1_me.bin`, `gc_12_0_1_mec.bin`)
- [ ] 164  Load MES firmware (`gc_12_0_1_mes.bin`, `gc_12_0_1_mes1.bin`) — primary scheduler on RDNA4
- [ ] 165  Port NBIO v7_11 setup (`nbio_v7_11.c`) — interrupt routing, BIF
- [ ] 166  Port IH ring init (`ih_v7_0.c`)
- [ ] 167  Drain IH ring on MSI-X interrupt → dispatch by source ID
- [ ] 168  Port MES v12_1 init (`mes_v12_0.c`) — schedule init queue
- [ ] 169  Create GFX queue via MES API — allocate ring in VRAM, map doorbell
- [ ] 170  Write first PM4 `NOP`; ring doorbell; confirm CP advances (read CP_RB_RPTR)
- [ ] 171  Write PM4 `WRITE_DATA` to known VRAM offset; read back
- [ ] 172  Implement EOP fence (`RELEASE_MEM` packet) — emit, interrupt fires, userspace sees fence value
- [ ] 173  **"Hello GFX12" milestone**: full submit + fence round-trip from userspace test
- [ ] 174  Port SDMA v7_1 (`sdma_v7_0.c`) — async copy engine
- [ ] 175  SDMA validate: system→VRAM→system bit-compare via DART
- [ ] 176  Create compute queue via MES — separate ring
- [ ] 177  UAPI: `alloc_bo(size, domain, flags) → handle`
- [ ] 178  UAPI: `free_bo(handle)`
- [ ] 179  UAPI: `map_bo(handle) → cpu_ptr` (via IOMemoryDescriptor share)
- [ ] 180  UAPI: `submit_cs(queue_id, ib, deps[]) → fence_handle`
- [ ] 181  UAPI: `wait_fence(handle, timeout_ns) → status`
- [ ] 182  UAPI: `query_info(type) → blob` (heap sizes, queue ids, asic id, gfx_version)
- [ ] 183  UAPI: `bo_export(handle) → mach_port` (for IOSurface bridging)
- [ ] 184  Stress test: 10k consecutive submits + fence waits, no leak/hang
- [ ] 185  Power cycle test: idle → load → idle, no PSP reset

**Milestone 1B:** programmable R9700 via custom UAPI under macOS.

---

## Phase 2A — winsys-mac

- [ ] 200  Define `winsys/winsys.h` shape matching `mesa/src/amd/common/ac_winsys.h` and `mesa/src/amd/vulkan/radv_radeon_winsys.h`
- [ ] 201  Implement `winsys_create` (opens dext IOUserClient)
- [ ] 202  Implement `bo_create` / `bo_destroy` / `bo_map` / `bo_unmap`
- [ ] 203  Implement BO domain selector (VRAM / GTT / system)
- [ ] 204  Implement CS context create/destroy
- [ ] 205  Implement CS IB chunk + Mesa `ac_pm4` integration
- [ ] 206  Implement CS submit — package for IOUserClient, dispatch
- [ ] 207  Implement fence wait (single + multi)
- [ ] 208  Implement timeline semaphore via fence wait-multi
- [ ] 209  Implement `query_info` — surface heap sizes, queue counts, gfx1201 asic id
- [ ] 210  Implement BO export/import via mach port (for IOSurface bridging in Phase 3)
- [ ] 211  Unit test: hand-rolled PM4 stream submits + fences via winsys (no Vulkan yet)

---

## Phase 2B — Vulkan ICD

- [ ] 250  Set up meson cross-file for macOS arm64; build infrastructure
- [ ] 251  Vendor `mesa/src/util/` + write macOS shims (`futex` → `os_unfair_lock`, etc.) in `vk/util_shims/`
- [ ] 252  Vendor `mesa/src/compiler/{nir,spirv}`
- [ ] 253  Vendor `mesa/src/amd/{common,registers,addrlib,compiler}`
- [ ] 254  Vendor `mesa/src/vulkan/runtime/`
- [ ] 255  Port `radv_instance` + `radv_physical_device` (point at our winsys)
- [ ] 256  Expose gfx1201 properties: vendor `0x1002`, device `0x7550`, driver name
- [ ] 257  Port `radv_device` — strip render-pass/WSI/graphics for compute-only build
- [ ] 258  Port command pool + command buffer
- [ ] 259  Port compute pipeline (`radv_pipeline_compute.c`)
- [ ] 260  Port descriptor set + pipeline layout
- [ ] 261  Port buffer, image (compute-usable storage only), memory binding
- [ ] 262  Port fence + binary + timeline semaphore
- [ ] 263  Port shader module + SPIR-V → NIR → ACO → gfx1201 ISA
- [ ] 264  Author ICD JSON; install to `/usr/local/share/vulkan/icd.d/amdvk_mac_icd.json`
- [ ] 265  `vulkaninfo` enumerates the AMD device + reports gfx1201 features

---

## Phase 2C — Compute validation

- [ ] 300  Hand-rolled "vkAdd" — two buffers, compute shader sum, readback
- [ ] 301  `vkcube --compute` runs
- [ ] 302  Run `dEQP-VK.compute.basic.*` — target 80% pass
- [ ] 303  Run `dEQP-VK.api.compute.*` — collect pass/fail diff vs Linux R9700 reference
- [ ] 304  Build llama.cpp with `-DGGML_VULKAN=ON`
- [ ] 305  Load Phi-3 GGUF, single forward pass
- [ ] 306  Output match within tolerance vs Linux reference
- [ ] 307  Benchmark tokens/sec, gap vs Linux ≤ 20%
- [ ] 308  Load Llama-3-8B GGUF, sustained throughput test

**Milestone 2:** llama.cpp Vulkan inference on R9700 under macOS, zero Metal.

---

## Phase 3A — Graphics pipeline

- [ ] 350  Re-enable RADV render pass + framebuffer code
- [ ] 351  Re-enable RADV graphics pipeline (vertex → fragment)
- [ ] 352  Port image tiling via addrlib for GFX12 color
- [ ] 353  Port depth/stencil tiling
- [ ] 354  Offscreen `vkcube` — render to BO, readback, visual-diff against Linux

---

## Phase 3B — WSI

- [ ] 400  Implement `VK_EXT_metal_surface`
- [ ] 401  `VkSurfaceKHR` backed by `CAMetalLayer*`
- [ ] 402  Swapchain images allocated in R9700 VRAM as IOSurface-exportable BOs
- [ ] 403  Render path: GFX12 to VRAM IOSurface
- [ ] 404  Present path: SDMA v7 VRAM→system IOSurface copy (TB5 upstream)
- [ ] 405  Hand system IOSurface to `CAMetalLayer.contents` or via Metal `MTLTexture` import from IOSurface
- [ ] 406  VSync via `CVDisplayLink`
- [ ] 407  `vkcube` (on-screen, full graphics)
- [ ] 408  Triple-buffer + N in-flight frames

---

## Phase 3C — Optimization

- [ ] 450  Profile copy-back, confirm TB5 upstream saturated
- [ ] 451  Tile-delta copy via addrlib tile coords
- [ ] 452  Pipeline submit + copy + present overlapping
- [ ] 453  Targets: 1080p @ 120 fps `vkmark`; 4K @ 60 fps `vkQuake`
- [ ] 454  Input-to-photon latency < 16 ms p99 at 1080p

---

## Phase 4 — Polish

- [ ] 500  Multi-queue (graphics + compute + transfer) concurrent submits
- [ ] 501  SMU v14 power management — idle clocks, boost under load
- [ ] 502  Hot-plug eGPU graceful handling
- [ ] 503  Notarized signed dext build
- [ ] 504  pkg installer with TinyGPU-style UX
- [ ] 505  Brew tap
- [ ] 506  Public README + install guide

---

## Cross-cutting

- [ ] 600  Bit-compare every PM4 stream against umr trace from Linux rig
- [ ] 601  CI: load dext, ping, alloc BO, submit NOP per-commit
- [ ] 602  ASAN/UBSAN builds of userspace ICD
- [ ] 603  Memory leak audit (BO lifetime, fence cleanup)
- [ ] 604  `docs/PORTING_NOTES.md` — every divergence from Linux amdgpu, with rationale
