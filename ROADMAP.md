# mac_amdgpu — Native AMD GPU Stack for macOS (Apple Silicon, RDNA4)

A third-party, Metal-bypassing graphics + compute stack for AMD RDNA4 GPUs (gfx1201 — Radeon AI PRO R9700) on Apple Silicon M-series Macs (M5 Pro/Max with Thunderbolt 5), built from the bare metal up. Replans the original 3-phase plan into 0–4 phases with updated 2026 platform reality.

---

## 0. Platform & target — fixed

| Decision | Value | Notes |
|---|---|---|
| Mac platform | **Apple Silicon (M5 Pro/Max, M5 Ultra) — no Intel support** | TB5 controller on-die; PCIe eGPU enumeration over TB5/USB4 works. User's `qemu-vfio-apple` fork is the local working precedent (R9700 via VFIO passthrough through a PCIDriverKit dext into a Linux VM). TinyGPU is a public precedent. |
| Bus | **Thunderbolt 5 (~80 Gbps usable bidirectional)** | Bandwidth budget for Phase 3 copy-back is real (~10 GB/s each direction), much better than the TB3 era. |
| Target ASIC | **AMD Radeon AI PRO R9700 (RDNA4, gfx1201, 32 GB GDDR6 ECC)** | Workstation card, $1299, 96 TFLOPs FP16, 128 AI accelerators, 300 W TDP, DP 2.1a. PCIe Gen5 x16 native. |
| GPU IP blocks | GFX12 (gfx_v12_1), GMC v12, SDMA v7_1, MES v12_1, NBIO v7_11, PSP v14_0_3, SMU v14_0_3 | Linux source maps to these versioned drivers under `drivers/gpu/drm/amd/amdgpu/`. |
| Firmware blobs | `gc_12_0_1_*`, `psp_14_0_3_*`, `smu_14_0_3*`, `sdma_7_0_1` | All present in `upstream/linux-firmware/amdgpu/`. AMD redistributable license. |
| Host OS | macOS 26 (Sequoia successor) — pinned to a known-good build | DriverKit ABI churn risk; pin and document. |
| Vulkan API target | 1.3 + `VK_EXT_metal_surface` | Compute-first; graphics in Phase 3. |

---

## 1. Reality check — read before any code

1. **Local prior art: `~/Documents/qemu-vfio-apple`.** The user maintains a QEMU fork with **PCI passthrough on Apple Silicon via a PCIDriverKit dext** (`contrib/apple-vfio/VFIOUserPCIDriver/`). It already runs the R9700 (VID `0x1002`, DID `0x7551`) under VFIO into a Linux VM. This is the architectural reference for our own dext: same framework, same DMA semantics, same host signing posture. **Traces in `qemu-vfio-apple/traces/` (3.5 GB, May 2026) capture real R9700 init sequences via `apple_dext_config_*` events and `guest-trace-amdgpu.sh` ftrace dumps** — these are our ground truth instead of needing a separate Linux box.
2. **Tiny Corp's TinyGPU** is the public precedent for an Apple-signed dext for eGPUs on AS. Reference only; not reusable directly (closed binary, compute-only via tinygrad).
3. **DriverKit entitlement workflow.** Correct sequence: build the Xcode dext bundle → register bundle ID + vendor identity on Apple Developer portal → request `com.apple.developer.driverkit.transport.pci` against that bundle ID. Apple's review can take weeks. Do this in parallel with Phase 1A skeleton code. The user's qemu-vfio-apple project is already entitlement-aware (its README documents the same gate); reuse the same Apple Developer team.
4. **DART (Apple Silicon DMA).** On AS, PCIe DMA is mediated by DART — Apple's IOMMU. PCIDriverKit's `IODMACommand` abstracts this; pinning, alignment, and TLB invalidation semantics are AS-specific. The apple-vfio dext already handles this correctly — port the pattern.
5. **HVF limitation noted in qemu-vfio-apple:** HVF cannot decode all trapping load/store instructions on AS, so BAR MMIO can't be traced from the host hypervisor side. The user works around this by capturing from inside the guest with ftrace on `amdgpu_device_rreg/wreg`. Our native driver doesn't have this constraint (we own the MMIO) but the captured traces are the canonical reference for register sequences.
6. **MES, not bare MEC.** RDNA4 has graduated to MES (MicroEngine Scheduler)-managed queues. Linux uses `mes_v12_1.c` for queue lifecycle. Phase 1B is structured around MES queue ops, not raw KIQ/MEC ring programming.
7. **Firmware redistribution.** AMD firmware in `upstream/linux-firmware/amdgpu/` is permissive with attribution. Ship `NOTICE` + `LICENSE.amdgpu`.

---

## 2. Architecture (1000-foot view)

```
┌─────────────────────────────────────────────────────────────┐
│  User App  (llama.cpp / vkcube / DXVK / native Vulkan game) │
└──────────────────────────┬──────────────────────────────────┘
                           │ Vulkan API
┌──────────────────────────▼──────────────────────────────────┐
│  amdvk-mac (Vulkan ICD, userspace)                          │
│    ├─ vk_*: dispatch, object lifetime, descriptors          │
│    ├─ compiler: SPIR-V → NIR → ACO → gfx1201 binary         │
│    │            (vendored from mesa/src/amd + compiler/nir) │
│    ├─ packets: PM4/SDMA encoders for GFX12/SDMA7            │
│    ├─ winsys-mac: BO alloc, submit, fence, GART, GTT        │
│    └─ wsi-mac: VK_EXT_metal_surface + IOSurface copyback    │
└──────────────────────────┬──────────────────────────────────┘
                           │ IOUserClient (custom protocol)
┌──────────────────────────▼──────────────────────────────────┐
│  amdgpu.dext (PCIDriverKit, Apple Silicon, userspace-kernel)│
│    ├─ pci: BAR map, MSI-X, config space (TB5 endpoint)      │
│    ├─ dart: DMA through Apple's IOMMU                       │
│    ├─ psp: PSP v14 bringup, SOS load, firmware verification │
│    ├─ smu: SMU v14_0_3 — clocks, power, telemetry           │
│    ├─ gmc: GMC v12 — VRAM detect, GART, VM page tables      │
│    ├─ mes: MES v12_1 — queue creation, scheduler            │
│    ├─ gfx/sdma: GFX12 + SDMA7 ring submission via MES       │
│    ├─ irq: IH ring drain, MSI-X dispatch                    │
│    └─ uapi: IOUserClient — mirrors Linux amdgpu UAPI subset │
└──────────────────────────┬──────────────────────────────────┘
                           │ PCIe Gen5 / Thunderbolt 5 / DART
                  ┌────────▼─────────┐
                  │ Radeon AI PRO R9700│
                  │   (gfx1201, 32GB) │
                  └────────────────────┘
```

Split mirrors Linux DRM/Mesa split: dext owns hardware state and queue lifecycle; ICD owns memory policy, command building, and shaders. Mesa's userspace compiler + packet code is reusable; only winsys + WSI are rewritten.

---

## 3. The RADV question — answered

> Fork RADV directly, or build a leaner bespoke compute-only ICD from scratch?

**Neither. Vendor what's not Linux-coupled; rewrite only the kernel-facing layers.**

| Component | Strategy | Source |
|---|---|---|
| ACO compiler | Vendor as-is | `mesa/src/amd/compiler/` |
| NIR + SPIR-V→NIR | Vendor as-is | `mesa/src/compiler/{nir,spirv}` |
| PM4/SDMA encoders | Vendor as-is | `mesa/src/amd/common/` |
| Register definitions (incl. gfx1201) | Vendor as-is | `mesa/src/amd/registers/` |
| `addrlib` (tiling/swizzle) | Vendor as-is | `mesa/src/amd/addrlib/` |
| Vulkan dispatch (`vk_*`) | Vendor scaffolding | `mesa/src/vulkan/runtime/` |
| RADV state tracker | Reference; port compute subset | `mesa/src/amd/vulkan/` |
| `winsys/amdgpu/*` (libdrm) | Throw away; rewrite | N/A |
| WSI (X11/Wayland) | Throw away; rewrite as Metal/IOSurface WSI | N/A |

Result is **"RADV-mac"** — ~5–10% original code, ~90% Mesa, line drawn at the kernel boundary. Compute-only is a build flag (skip render passes, skip graphics pipeline, skip WSI), not a separate codebase.

---

## 4. Phased roadmap

### Phase 0 — Gates

| # | Task | Done when |
|---|---|---|
| 0.1 | Read `qemu-vfio-apple/contrib/apple-vfio/VFIOUserPCIDriver/` end-to-end | Architecture, IIG interfaces, entitlements, Start/Stop lifecycle understood. |
| 0.2 | Index `qemu-vfio-apple/traces/` by trace event type | Catalog of `apple_dext_config_*`, `vfio_*`, ftrace amdgpu sequences, devcoredumps. |
| 0.3 | Replay a representative trace into a parser; reconstruct R9700 init register sequence | `docs/INIT_SEQUENCE.md` derived from real captures. |
| 0.4 | Confirm hardware: R9700 + M5 Pro/Max/Ultra + TB5 enclosure in hand | Already true (qemu-vfio-apple runs on this rig). |
| 0.5 | Pull firmware blobs for gfx1201, record versions in `firmware/MANIFEST.md` | Manifest committed. |
| 0.6 | Author NOTICE + LICENSE.amdgpu | Files committed. |
| 0.7 | Decide debug strategy (`os_log` + IOUserClient debug method + cross-ref vs qemu-vfio-apple traces) | `docs/DEBUG.md` written. |
| 0.8 | Pin macOS build + capture DriverKit SDK version | `docs/HARDWARE.md` written. |
| 0.9 | Confirm Apple Developer team + entitlement plan (reuse qemu-vfio-apple's team or new one) | Decision recorded. |

**Gate to Phase 1:** 0.1, 0.2, 0.3.

### Phase 1A — Dext skeleton + PCI plumbing + entitlement

Build the Xcode bundle first so the entitlement request has a target. Apple's review runs in parallel with code.

- 1A.1 Create Xcode workspace `mac_amdgpu.xcworkspace` + `amdgpu.dext` target (PCIDriverKit). Mirror layout of `qemu-vfio-apple/contrib/apple-vfio/VFIOUserPCIDriver/`.
- 1A.2 Info.plist `IOKitPersonalities` matching VID `0x1002` / DID `0x7551` (R9700) — confirmed from trace `value=0x75511002` at config offset 0x0.
- 1A.3 Build, codesign with team identity → bundle ID registered on Apple Developer portal.
- 1A.4 **Submit entitlement request** for `com.apple.developer.driverkit.transport.pci` against bundle ID — parallel to 1A.5+.
- 1A.5 `Start()` — open `IOPCIDevice` over TB5, log VID/DID/config space; compare against `qemu-vfio-apple/traces/linux-pre-driver-init.txt`.
- 1A.6 Map BARs — BAR0 (visible VRAM aperture), BAR2 (VRAM doorbell/aperture), **BAR5 (register window — Bonaire+ AMD layout, confirmed on R9700)**. Cross-check sizes against config space reads from the captured traces.
- 1A.7 Enable bus master + MSI-X; allocate N vectors via `IOInterruptDispatchSource`.
- 1A.8 DART/DMA validation — `IODMACommand` allocate, write, GPU SDMA-copy back, bit-compare. Pattern after `VFIOUserPCIDriver.cpp` DMA paths.
- 1A.9 `IOUserClient` subclass + stub method table.
- 1A.10 Userspace test (`scripts/dext_ping.swift`) — open service, ping round-trip.

**Milestone 1A:** dext loads, claims R9700, DMA round-trip works, userspace can ping. No hardware activity beyond config space + DMA validation.

### Phase 1B — Hardware bringup (RDNA4-specific)

Port from `upstream/linux/drivers/gpu/drm/amd/amdgpu/` against the gfx1201/v12_1/v7/v14 versioned files. Cite Linux source files in commit messages. Cross-check every register write against the ftrace captures in `qemu-vfio-apple/traces/` (those are the exact RREG32/WREG32 sequences amdgpu emits when bringing up our R9700 on our actual hardware).

The bringup orchestrator (`dext/amdgpu/amdgpu_init.{h,cpp}`) drives a 14-stage ladder via the `InitDevice(target_stage)` selector. On real R9700 hardware (v0.0.58, commit `c8d224e`), **8 stages are green** — PSP ring is fully end-to-end (FB_FW_RESERV queries + LOAD_IP_FW frames all return real responses from PSP), SMU PMFW is loaded via the ring, SMU mailbox `TestMessage` echoes, GMC + MMHUB are configured with correct register offsets + GFX12 `IS_PTE` PTE bit + NBIO HDP `remap_hdp_registers` programmed. The 6-day silent-drop bug was `PSP_RING_TYPE__KM = 1` instead of upstream's `2`.

| #  | Stage              | Source                            | Status (on R9700) |
|---|---|---|---|
| 0  | None               | —                                 | ✅ |
| 1  | IPDiscovery        | on-die VRAM read via MM_INDEX/MM_DATA (TMR offset 64 KB); parser ported with multi-`BASE_IDX` support | ✅ |
| 2  | PSPInit            | `psp_init` fw_pri buffer alloc    | ✅ |
| 3  | PSPLoadSOS         | `psp_v14_0_bootloader_load_sos`; `psp_init_sos_microcode` v1+v2 header autodetect + sub-firmware extraction | ✅ |
| 4  | PSPRingCreate      | `psp_v14_0_ring_create` (PSP_RING_TYPE__KM = **2**, GPCOM ring), `psp_query_fw_reservation` (FB_FW_RESERV_ADDR + EXT) | ✅ |
| 5  | TMRSetup           | `psp_setup_tmr` (skip path — `boot_time_tmr` on psp_v14_0_3) | ✅ |
| 6  | SMUInit            | `smu_test_message` over MP1 BASE_IDX 1 mailbox, after `LoadFirmware(SMU)` | ✅ |
| 7  | GMCInit            | full `gmc_v12_0_*` + `mmhub_v4_1_0_*` + `gfxhub_v12_0_*` register sequence | ✅ |
| 8  | IMUInit            | `imu_v12_0_*` (PSP LOAD_IP_FW for IMU_I + IMU_D) | ⛔ stub; needs IMU fw loading |
| 9  | RLCInit            | `gfx_v12_0_rlc_init` + `_resume`  | ⛔ returns `kIOReturnUnsupported` until RLC sub-bins load via PSP |
| 10 | CPInit             | `gfx_v12_0_cp_gfx_resume`         | ⛔ needs PFP_HALT clear + CP_MEC_RS64_CNTL + RS64 PFP/ME/MEC firmware |
| 11 | MESInit            | `mes_v12_1_*`                     | ⛔ needs CP_MES + CP_MES_DATA firmware, queue_init reorder |
| 12 | IHInit             | `ih_v7_0_*`                       | ⛔ several RB_CNTL field shifts wrong (audit #6), runs too late in stage order |
| 13 | GFXInit            | First-PM4 path                    | ⛔ stub |
| 14 | SDMAInit           | `sdma_v7_0_*` → `sdma_v7_1_*`     | ⛔ register offsets are sdma_v7_0 family; R9700 needs sdma_v7_1 from `gc_12_1_0_offset.h` |

### Phase 1B.0b — Multi-component firmware parsing (current critical-path blocker)

LoadFirmware uses a simple "skip 32-byte common header" extraction
that works for SMU PMFW. PSP returns `0xFFFF0006 = TEE_ERROR_BAD_PARAMETERS`
for SDMA / RLC / uni_mes because those firmwares have per-IP header
layouts and may need multiple LOAD_IP_FW frames per file:

- **SDMA v3** (sdma_v7_1): ucode at `sdma3_hdr->ucode_offset_bytes`, not the common header's field.
- **RLC**: sub-binaries for `RLC_G` / `RLC_IRAM` / `RLC_DRAM` / `RLC_P` / `RLC_V` / `RESTORE_LIST` — each is a separate LOAD_IP_FW frame at a different `fw_type` and offset within the file.
- **uni_mes**: split into `CP_MES` (ucode) + `CP_MES_DATA` (data) — two frames per file at different offsets.
- **CP RS64**: `gc_<v>_pfp.bin` / `me.bin` / `mec.bin` each split into ucode + P0..P3 stack variants.
- **IMU**: `IMU_I` (instruction) + `IMU_D` (data) from `gc_<v>_imu.bin`.

Once parsing lands and RLC/IMU autoload chain completes, `RLCInit` → `CPInit` → `MESInit` → `GFXInit` unblock. SDMA needs a separate `sdma_v7_1` register-offset port (audit #6).

**Pre-SOS bootloader components** (KDB/SPL/SysDrv/SocDrv/IntfDrv/HADDrv/RASDrv/IPKeyMgrDrv) all load via `psp_bootloader_load_component` exposed through `LoadFirmware(type=1..8, size)`. Post-SOS IP firmware (PMFW, RLC, CP, IMU, MES, SDMA) loads via `psp_load_ip_fw` exposed through `LoadFirmware(type=0x100+psp_gfx_fw_type, size)`.

The bringup contract:
1. Userspace calls `LoadDiscoveryBinary` (or `SetIPBase` per block) so the IP base table is resolved.
2. `InitDevice(target=PSPLoadSOS)` after host has called `LoadFirmware(SOS, size)`.
3. `InitDevice(target=PSPRingCreate)` once SOS is alive.
4. `InitDevice(target=TMRSetup)` after the ring is up.
5. `LoadFirmware(PMFW=0x112, size)` → SMU comes online.
6. `InitDevice(target=SMUInit)` — TestMessage now answers; SMU version logged.
7. Subsequent stages (GMC/IMU/RLC/CP/MES/IH/GFX/SDMA) — each gates on the prior being done; each consumes one or more firmwares.

**Milestone 1B:** programmable AMD R9700 under macOS via custom UAPI.

### Phase 2A — winsys-mac

- 2A.1 Define `winsys.h` shape matching Mesa's `radeon_winsys.h` (so RADV's expectations swap cleanly).
- 2A.2 BO alloc/free/map/unmap over IOUserClient.
- 2A.3 BO domain selector (VRAM / GTT / system).
- 2A.4 CS context, IB build via `ac_pm4`, submit path packs PM4 for dext.
- 2A.5 Fence + timeline semaphore — wrap kernel fence handles.
- 2A.6 BO export/import via mach port (for IOSurface bridging in Phase 3).
- 2A.7 `query_info` — surface heap sizes, queue counts, asic id (gfx1201).
- 2A.8 Standalone test: hand-rolled PM4 stream submitted via winsys → fence completes.

### Phase 2B — Vulkan ICD

- 2B.1 Meson cross-file for macOS arm64; build infra.
- 2B.2 Vendor `mesa/src/util/` with macOS shims for Linux-only bits (`futex` → `os_unfair_lock`, `inotify` → no-op, etc.).
- 2B.3 Vendor `mesa/src/compiler/{nir,spirv}`.
- 2B.4 Vendor `mesa/src/amd/{common,registers,addrlib,compiler}`.
- 2B.5 Vendor `mesa/src/vulkan/runtime`.
- 2B.6 Port `radv_instance` / `radv_physical_device` against our winsys; expose gfx1201.
- 2B.7 Port `radv_device` — strip graphics/render-pass/WSI initially.
- 2B.8 Port command pool + command buffer.
- 2B.9 Port compute pipeline (`radv_pipeline_compute.c`).
- 2B.10 Port descriptor set + pipeline layout.
- 2B.11 Port buffer, image (compute-usable storage), memory binding.
- 2B.12 Port fence, binary + timeline semaphore.
- 2B.13 SPIR-V → NIR → ACO → gfx1201 ISA path.
- 2B.14 ICD JSON installed to `/usr/local/share/vulkan/icd.d/amdvk_mac_icd.json`.
- 2B.15 `vulkaninfo` enumerates the AMD device.

### Phase 2C — Compute validation

- 2C.1 Hand-rolled "vkAdd" — two buffers, compute sum, readback.
- 2C.2 `vkcube --compute`.
- 2C.3 `dEQP-VK.compute.basic.*` — target 80% pass.
- 2C.4 `dEQP-VK.api.compute.*` — collect pass/fail diff vs Linux R9700 reference.
- 2C.5 Build llama.cpp with `GGML_VULKAN=ON`.
- 2C.6 Load Phi-3 / Llama-3-8B GGUF, single forward pass, output bit-similar to CPU reference.
- 2C.7 Benchmark tokens/sec vs Linux reference; expect within 10–20%.

**Milestone 2:** llama.cpp inference on R9700 under macOS, zero Metal.

### Phase 3A — Graphics pipeline

- 3A.1 Re-enable RADV render pass + framebuffer code.
- 3A.2 Re-enable RADV graphics pipeline (vertex → fragment).
- 3A.3 Image tiling via addrlib for GFX12 color + depth swizzle.
- 3A.4 Offscreen `vkcube` (full graphics, BO readback, visual-diff against Linux).

### Phase 3B — WSI for macOS

- 3B.1 Implement `VK_EXT_metal_surface`.
- 3B.2 `VkSurfaceKHR` backed by `CAMetalLayer*` (handed in from app).
- 3B.3 Swapchain images allocated as IOSurface-exportable BOs in R9700 VRAM.
- 3B.4 Present routine:
  1. GFX12 renders to VRAM IOSurface.
  2. SDMA v7 copies VRAM → system-memory IOSurface (TB5 upstream).
  3. Hand system IOSurface to `CAMetalLayer.contents` (or via Metal `MTLTexture` import from IOSurface).
- 3B.5 VSync via `CVDisplayLink`.
- 3B.6 `vkcube` (full graphics, on-screen).
- 3B.7 Triple buffering with in-flight present pipeline.

### Phase 3C — Optimization

- 3C.1 Profile copy-back — confirm TB5 upstream is saturated.
- 3C.2 Tile-delta copy (addrlib tile coords, only changed tiles).
- 3C.3 Pipeline submit + copy + present with N in-flight frames.
- 3C.4 Targets: 1080p @ 120 fps for `vkmark`; 4K @ 60 fps for `vkQuake`. (TB5 budget is forgiving.)
- 3C.5 Latency: input-to-photon < 16 ms p99 at 1080p.

### Phase 4 — Polish + distribution

- 4.1 Multi-queue (graphics + compute + transfer) concurrent submits.
- 4.2 Power management — idle clocks, boost under load via SMU v14.
- 4.3 Hot-plug — eGPU disconnect/reconnect graceful handling.
- 4.4 Notarized signed dext build.
- 4.5 Brew tap formula + `pkg` installer with TinyGPU-style setup UX.
- 4.6 Public README + install guide.

---

## 5. Risks & contingencies

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| DriverKit PCIe entitlement denied | Low (TinyGPU + qemu-vfio-apple precedents) | Project-killer | Apply against the dext bundle ID early; appeal with TinyGPU and qemu-vfio-apple as precedents. |
| qemu-vfio-apple dext architecture proves insufficient pattern for full amdgpu lifecycle | Medium | Phase 1A rework | Treat apple-vfio dext as "PCI plumbing reference," not "everything"; the amdgpu lifecycle (MES, fences, queues) is added on top, not inherited. |
| Apple changes DriverKit ABI mid-development | Medium | Phase 1 rework | Pin macOS version; maintain a test VM/snapshot. |
| Firmware microcode signature mismatch / PSP rejection | Medium | Phase 1B stall | Match `linux-firmware` git SHA to validated Linux config; if PSP refuses, escalate via `psp_v14_0_3_sos_kicker.bin` path. |
| RDNA4 GFX12 register/packet drift from Linux | Medium-High | Phase 1B stall | Bit-compare every submit against umr traces from Linux R9700. |
| MES v12 behavioral differences from older MES | Medium | Phase 1B rework | Treat MES queue ops as "Linux-equivalent" not "documented" — diff against `mes_v12_1.c` line-by-line. |
| TB5 upstream bandwidth caps framerate at 4K HDR | Low | Phase 3 may need quality cap | Accept 4K @ 60 fps as realistic ceiling; tile-delta to improve. |
| Mesa license mix (MIT + various) for distribution | Low | Minor | Track per-file licenses; ship NOTICE. |
| Single-developer scope (~2–4 years calendar) | High | Burnout | Compute-first ordering — ship llama.cpp Vulkan before graphics. |

---

## 6. Out of scope (explicit)

- Display output direct from R9700 ports. (All scanout via copy-back to Mac's iGPU and CoreAnimation.)
- Non-RDNA4 ASICs for the first release. (gfx1201 only until Phase 4.)
- Non-AMD GPUs.
- OpenGL/OpenCL frontends. (Vulkan only.)
- Multiple ASIC families simultaneously.

---

## 7. References

- **qemu-vfio-apple (local prior art)** — `~/Documents/qemu-vfio-apple/contrib/apple-vfio/VFIOUserPCIDriver/`: working AS PCIDriverKit dext + IIG interfaces. `~/Documents/qemu-vfio-apple/traces/`: 3.5 GB of real R9700 captures (config space, ftrace amdgpu RREG/WREG, devcoredumps). `guest-trace-amdgpu.sh` is the capture tool to extend.
- Linux amdgpu — `upstream/linux/drivers/gpu/drm/amd/amdgpu/` (gfx_v12_*, gmc_v12_*, mes_v12_*, psp_v14_0, smu_v14_0, sdma_v7_*, nbio_v7_11)
- Mesa userspace — `upstream/mesa/src/amd/`, `upstream/mesa/src/compiler/`, `upstream/mesa/src/vulkan/`
- AMD firmware — `upstream/linux-firmware/amdgpu/` (gc_12_0_1_*, psp_14_0_3_*, smu_14_0_3*, sdma_7_0_1)
- TinyGPU (public prior art) — https://docs.tinygrad.org/tinygpu/ and https://github.com/tinygrad
- Apple PCIDriverKit — https://developer.apple.com/documentation/pcidriverkit
- DriverKit transport entitlements — https://developer.apple.com/documentation/bundleresources/entitlements/com_apple_developer_driverkit_transport_pci
- AMD R9700 specs — https://www.amd.com/en/products/graphics/workstations/radeon-ai-pro/ai-9000-series/amd-radeon-ai-pro-r9700.html
- Phoronix R9700 Linux review — https://www.phoronix.com/review/amd-radeon-ai-pro-r9700
- GPUOpen ISA — https://gpuopen.com/documentation/amd-isa-documentation/
- Mesa ACO docs — `upstream/mesa/src/amd/compiler/README.md`

See `WORKLIST.md` for tracked granular tasks.
