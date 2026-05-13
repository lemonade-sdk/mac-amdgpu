# mac_amdgpu — Native AMD GPU Stack for macOS

A third-party, Metal-bypassing graphics + compute stack for AMD GPUs on macOS, built from the bare metal up. This replans the original 3-phase plan and adds a Phase 0 (gates), a hard-split Phase 1 (bringup vs. UAPI), and a Phase 2 strategy that's neither "fork RADV wholesale" nor "rebuild from scratch."

---

## 0. Reality check — read this first

The original plan is structurally right but understates four killers. Phase 0 exists to flush these out before a single line of dext code:

1. **DriverKit PCIe entitlement.** `com.apple.developer.driverkit.transport.pci` is gated by Apple — a manual request, not a checkbox. Without it, the dext won't load even on your own machine outside development mode. **Status: TBD — apply early, this can take weeks/months.**
2. **Mac platform.** Intel Macs (Mac Pro 7,1; iMac Pro; Intel mini with eGPU) use VT-d/IOMMU and the legacy IOPCIFamily kext story. Apple Silicon Macs use DART for DMA, and **PCIe device enumeration over Thunderbolt for non-Apple-blessed GPUs is essentially unsupported by macOS** — Apple has dropped AMD support on AS entirely. **The realistic target is Intel Mac + eGPU enclosure or Mac Pro 7,1 MPX/PCIe slot.** Apple Silicon is a research project, not a product.
3. **Target ASIC.** Every generation (GCN5/Vega, GFX10/RDNA1+2, GFX11/RDNA3, GFX12/RDNA4) has different register maps, firmware, packet formats, and microcode layout. **You pick one chip, you stay on it for the first year.** Recommendation: Navi 21/22/23 (RDNA2 — RX 6600/6700/6800) — widely available, well-documented in Linux, last gen Apple shipped drivers for, and supported by AMD's open firmware redistribution.
4. **Firmware redistribution.** AMD's firmware is redistributable under their license (see `LICENSE.amdgpu` in linux-firmware) but you must ship a copy with attribution. Don't extract from macOS — use linux-firmware blobs.

If any of the four can't be satisfied, the project re-shapes:
- No PCIe entitlement → headless dev only; can't distribute.
- AS-only → fallback to a Metal-backed compute path (defeats the purpose).
- Unable to commit to one ASIC → scope explosion.
- Firmware → blocked.

---

## 1. Architecture (1000-foot view)

```
┌─────────────────────────────────────────────────────────────┐
│  User App  (llama.cpp / vkcube / DXVK / native Vulkan game) │
└──────────────────────────┬──────────────────────────────────┘
                           │ Vulkan API
┌──────────────────────────▼──────────────────────────────────┐
│  amdvk-mac (Vulkan ICD, userspace)                          │
│    ├─ vk_*: dispatch tables, object lifetime, descriptors   │
│    ├─ compiler: SPIR-V → NIR → ACO → GFX10/11 binary        │
│    │            (vendored from mesa/src/amd + compiler/nir) │
│    ├─ packets: PM4/SDMA encoders (vendored mesa/src/amd)    │
│    ├─ winsys-mac: BO alloc, submit, fence, GART, GTT        │
│    └─ wsi-mac: IOSurface render-to-texture + CALayer        │
└──────────────────────────┬──────────────────────────────────┘
                           │ IOUserClient (custom protocol)
┌──────────────────────────▼──────────────────────────────────┐
│  amdgpu.dext (PCIDriverKit extension, userspace kernel-side)│
│    ├─ pci: BAR map, MSI-X, config space                     │
│    ├─ smu/psp: firmware load, power-up, clocks              │
│    ├─ atom: ATOMBIOS parse (clocks, voltage, displays)      │
│    ├─ gmc: GART/VM page tables, DART integration            │
│    ├─ ring: GFX/SDMA/compute ring submission                │
│    ├─ irq: interrupt routing, IH ring drain                 │
│    └─ uapi: IOUserClient — mirrors Linux DRM amdgpu UAPI    │
└──────────────────────────┬──────────────────────────────────┘
                           │ PCIe / Thunderbolt / DART
                  ┌────────▼─────────┐
                  │  AMD GPU (Navi 2X) │
                  └────────────────────┘
```

The split is deliberate: the dext owns hardware state and ring submission; the userspace ICD owns memory management policy, command building, and shaders. This mirrors Linux's split (DRM kernel + Mesa userspace) and lets us reuse Mesa's userspace compiler & packet code with only the winsys layer rewritten.

---

## 2. The RADV question — answered

> Would you fork RADV directly, or build a leaner bespoke compute-only ICD from scratch?

**Neither. Vendor the parts that don't depend on Linux; rewrite the parts that do.**

A clean "fork RADV" loses you a year fighting the Linux/DRM coupling in RADV's `winsys/amdgpu/`, sync primitives (drm syncobj), CS submit ioctls, and `radv_device.c`'s mountain of feature toggles. A "build from scratch" loses you two years re-implementing ACO, NIR-to-ACO, register encoders, packet builders, descriptor encoding for GFX10/11, and SPIR-V translation — code that already exists and is correct.

The pragmatic line:

| Component | Strategy | Source |
|---|---|---|
| ACO compiler | **Vendor as-is** | `mesa/src/amd/compiler/` |
| NIR + SPIR-V→NIR | **Vendor as-is** | `mesa/src/compiler/nir`, `mesa/src/compiler/spirv` |
| PM4/SDMA packet encoders | **Vendor as-is** | `mesa/src/amd/common/` |
| Register definitions | **Vendor as-is** | `mesa/src/amd/registers/` |
| `addrlib` (tiling/swizzle) | **Vendor as-is** | `mesa/src/amd/addrlib/` |
| Vulkan dispatch (`vk_*`) | **Vendor scaffolding** | `mesa/src/vulkan/runtime/` |
| RADV state tracker | **Reference only — rewrite minimal subset** | `mesa/src/amd/vulkan/` |
| `winsys/amdgpu/*` (libdrm calls) | **Throw away — rewrite** | N/A |
| WSI (X11/Wayland) | **Throw away — rewrite as Metal/IOSurface WSI** | N/A |

What this gives you is **"RADV-mac"** — a Vulkan driver that's ~5–10% original code, ~90% Mesa, with the line drawn at "anything that calls into the kernel." Plumbing only. The compute-only subset is then a build flag (skip graphics pipeline, skip render passes, skip WSI), not a separate codebase.

**Risk this hedges:** building compute-only from scratch means you eventually re-do all this work for graphics in Phase 3. The vendored path means Phase 3 is mostly "implement the WSI extension and wire up render passes" — already-supported code paths in the vendored RADV.

---

## 3. Phased roadmap

### Phase 0 — Gates (weeks, not months)

| # | Task | Done when |
|---|---|---|
| 0.1 | Apply for `com.apple.developer.driverkit.transport.pci` entitlement | Apple approves or denies |
| 0.2 | Pick target ASIC + acquire hardware | RX 6600/6700/6800 or similar Navi 2x in hand |
| 0.3 | Pick target Mac platform | Intel Mac (Mac Pro 7,1 or eGPU-capable Intel) on macOS 14/15 |
| 0.4 | Stand up Linux reference rig (same GPU) | Vanilla Ubuntu + amdgpu + vkcube + llama.cpp Vulkan working |
| 0.5 | Capture a "known-good" trace | `umr` / `radeontool` dumps of register init + a vkcube submission |
| 0.6 | Audit AMD firmware license + ship attribution | NOTICE / LICENSE files committed |
| 0.7 | Decide on debug strategy | JTAG/serial via Thunderbolt? `vmwrite` instrumentation? `os_log` to host? |

**Gate to Phase 1: 0.1, 0.2, 0.3, 0.4 all green.**

### Phase 1 — Dext bringup (the hard part)

Split into 1A (skeleton + PCI) and 1B (power-up + rings). 1A is ~weeks; 1B is ~months.

#### 1A — PCI plumbing
- 1A.1 Xcode project: `amdgpu.dext`, PCIDriverKit target, Info.plist with VID/DID match.
- 1A.2 `IOPCIDevice` open, config space read (verify ASIC ID against expected).
- 1A.3 Map BAR0 (registers), BAR2 (VRAM aperture), BAR5 (doorbells).
- 1A.4 MSI-X allocation via `IOInterruptDispatchSource`.
- 1A.5 DART/IOMMU setup — `IODMACommand` for DMA-able buffers.
- 1A.6 `IOUserClient` subclass with a stub method table; round-trip a ping from userspace.

**Milestone 1A:** dext loads, claims the device, userspace can ping it. No hardware activity beyond config space reads.

#### 1B — Hardware initialization

Port from `linux/drivers/gpu/drm/amd/amdgpu/`. Stay generation-locked.

- 1B.1 ATOMBIOS parser (`amdgpu_atombios.c`) — read VBIOS from ROM, extract clock/voltage tables.
- 1B.2 SMU firmware load + handshake (`amdgpu_psp.c`, `smu_v11_0.c` for Navi). PSP loads SOS, then drives SMU.
- 1B.3 GMC init (`gmc_v10_0.c`) — VRAM size detect, GART aperture setup, page table format.
- 1B.4 GFX firmware load (CP/ME/MEC/RLC microcode from `linux-firmware/amdgpu/navi*`).
- 1B.5 IH (interrupt handler) ring init — drain into a per-source dispatch table.
- 1B.6 GFX ring init — KIQ + GFX queue, doorbell mapping, ring buffer in VRAM.
- 1B.7 First PM4 packet: `NOP` → `WRITE_DATA` → fence. **"Hello GFX" milestone.**
- 1B.8 SDMA ring — async DMA copies (you'll need this for VRAM uploads).
- 1B.9 Compute ring (MEC) — required for any compute submit.
- 1B.10 BO management UAPI — `alloc_bo(size, domain)`, `map_bo(handle)`, `submit_cs(ring, packets, deps)`.

**Milestone 1B:** userspace test program submits a PM4 `WRITE_DATA` to GFX ring, signals a fence, kernel-mode IH fires, userspace sees the fence value. **You now have a programmable AMD GPU on macOS.**

### Phase 2 — Vulkan compute ICD

#### 2A — winsys-mac

Replace Mesa's `winsys/amdgpu/` (which calls `libdrm_amdgpu`) with one that calls your IOUserClient.

- 2A.1 BO struct + alloc/free over IOUserClient.
- 2A.2 CS context, dependency tracking, fence/sync primitive.
- 2A.3 Submit path — pack PM4 from Mesa's `ac_pm4` builder, ship to dext.
- 2A.4 Query info — `amdgpu_query_info` equivalent (GPU info, heap sizes, queue counts).

#### 2B — Build the ICD

Vendor Mesa subtrees into `vk/`. Build with meson cross-file for macOS.

- 2B.1 Vendor `src/amd/{common,registers,addrlib,compiler}` and `src/compiler/{nir,spirv}`.
- 2B.2 Vendor `src/vulkan/runtime` (the `vk_*` scaffolding).
- 2B.3 Port `src/amd/vulkan` minimal subset — skip render passes, skip graphics pipeline, keep:
  - device/physical-device/instance
  - queue, command buffer
  - compute pipeline, descriptor set, pipeline layout
  - buffer, image (compute-usable only), memory
  - fence, semaphore (timeline + binary)
  - SPIR-V shader module + ACO compile path
- 2B.4 Install as `MoltenVK`-style ICD JSON (`/usr/local/share/vulkan/icd.d/amdvk_mac_icd.json`).

#### 2C — Compute validation

- 2C.1 `vkEnumeratePhysicalDevices` returns AMD GPU.
- 2C.2 `vkcube` compute path or a hand-rolled "add two buffers" compute shader runs.
- 2C.3 Vulkan CTS compute subset (`dEQP-VK.compute.*`) — aim for 80%+, not 100%.
- 2C.4 `llama.cpp` Vulkan backend loads weights, runs a forward pass, output matches CPU reference.

**Milestone 2:** llama.cpp running on an AMD GPU on a Mac with zero Metal involvement.

### Phase 3 — Graphics + presentation

#### 3A — Graphics pipeline
- 3A.1 Re-enable RADV's graphics state tracker (render pass, framebuffer, GFX pipeline).
- 3A.2 Color/depth attachments — tiling via addrlib.
- 3A.3 Validate with `vkcube` (full graphics), `vkmark`, `vkQuake`.

#### 3B — WSI: VK_EXT_metal_surface
- 3B.1 Implement `VkSurfaceKHR` backed by `CAMetalLayer`.
- 3B.2 Swapchain images allocated in AMD VRAM as IOSurface-exportable BOs.
- 3B.3 Present routine:
  1. Render to VRAM IOSurface.
  2. SDMA copy → system memory IOSurface (PCIe upstream).
  3. Hand IOSurface to `CAMetalLayer` via `CALayer.contents` or Metal interop.
- 3B.4 VSync via `CVDisplayLink` callback driving present queue.

#### 3C — Optimization
- 3C.1 Profile the copy-back — saturate PCIe upstream bandwidth (~3 GB/s TB3, ~6 GB/s TB4, ~16 GB/s PCIe 4.0 x16).
- 3C.2 Tile-based incremental upload (only changed tiles) — addrlib helps.
- 3C.3 Pipeline submit and copy with multiple in-flight frames.
- 3C.4 Triple buffer, latency targets: < 16 ms at 1080p, < 33 ms at 4K.

**Milestone 3:** native Vulkan game (DXVK-wrapped Windows game or Linux native via Wine) at usable framerate.

### Phase 4 — Polish + distribution

- 4.1 Multi-queue (graphics + compute + transfer) concurrency.
- 4.2 Power management — clock scaling under load, idle clocks.
- 4.3 Hot-plug handling for eGPU disconnect/reconnect.
- 4.4 Signed dext distribution (Developer ID + notarization).
- 4.5 Brew tap or pkg installer.

---

## 4. Risks & contingencies

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| DriverKit PCIe entitlement denied | Medium | Project-killer | Apply Phase 0.1. Fallback: kext-based (deprecated, but functional pre-Sequoia) or research-only. |
| Apple silently drops PCIDriverKit class | Low | Project-killer | Pin macOS version. Maintain Sonoma VM. |
| Firmware microcode signature mismatch | Medium | Phase 1B stall | Match linux-firmware version to ASIC PSP version exactly. |
| PM4 packet difference between Linux & macOS reality | High | Phase 1B stall | Capture umr traces on Linux first; bit-compare submission. |
| PCIe upstream bandwidth crushes framerate | High | Phase 3 unusable | Tile delta copies; accept 60 fps@1080p as ceiling for TB3. |
| Mesa license (MIT + others) vs distribution | Low | Minor | Track per-file licenses; ship NOTICE. |
| Single-developer scope (~3–5 years calendar) | High | Burnout | Compute-first ordering — ship llama.cpp Vulkan before touching graphics. |

---

## 5. Out of scope (explicit)

- Apple Silicon. (Different DART semantics, no public PCIe eGPU enumeration, no upstream effort.)
- Display output direct from AMD GPU. (No DCN driver. All scanout via copy-back to Mac's iGPU.)
- DisplayPort/HDMI output ports on the AMD card. (Same reason.)
- Non-AMD GPUs.
- OpenGL/OpenCL frontends. (Vulkan only. ICD shape only.)
- Multiple ASIC families simultaneously. (One ASIC, period, until Phase 4.)

---

## 6. References

- Linux kernel amdgpu — `upstream/linux/drivers/gpu/drm/amd/amdgpu/`
- Mesa userspace — `upstream/mesa/src/amd/`, `upstream/mesa/src/compiler/`, `upstream/mesa/src/vulkan/`
- AMD firmware — `upstream/linux-firmware/amdgpu/`
- Apple PCIDriverKit — https://developer.apple.com/documentation/pcidriverkit
- DriverKit transport entitlements — https://developer.apple.com/documentation/bundleresources/entitlements/com_apple_developer_driverkit_transport_pci
- GPUOpen ISA docs — https://gpuopen.com/documentation/amd-isa-documentation/
- Mesa ACO — `upstream/mesa/src/amd/compiler/README.md`
- AMD register reference (auto-gen) — `upstream/mesa/src/amd/registers/`

See `WORKLIST.md` for tracked, granular tasks.
