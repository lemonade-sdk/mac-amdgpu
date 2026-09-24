# Status

This driver and HSA runtime connect AMD GPUs on Apple Silicon to
[Lemon Seed Engine (LSE)](https://github.com/Geramy/LSE/tree/main) through native HRX/Loom.
See the [LSE reproduction guide](docs/LSE_QUICKSTART.md) for pinned dependencies,
build commands and GPU validation, or the shorter [local run guide](LOCAL_RUN.md).
Qwen 27B Q6 text generation and repeated HTTP chat now run on the GPU. Independent MLX references match exact token IDs on five-token and 64-token prompts (native and float32 reference precision respectively). Experimental batching and host-wait settings reached median 10.12 decode tokens/s on the short workload; see the [measurements and limits](docs/LSE_PERFORMANCE.md) and [reproduction command](LOCAL_RUN.md#experimental-resident-benchmark). Eligible single-device decode now measures its submission interval automatically; blocked polling defaults to 1000 µs. The automatic policy reached 6.97 tokens/s with default polling and 10.13 tokens/s with a 64 µs polling override. Longer contexts and broader accuracy remain under qualification.

## Working

- GPU discovery, firmware loading and initialization on the Radeon AI PRO R9700 (`gfx1201`) over Thunderbolt.
- PCI identity reported through HSA: device `0x7551`, revision `0xc0`, verified after opening the PCI device.
- Verified SDMA transfers, VRAM allocations and cross-process GPU-buffer sharing.
- Loading linked gfx1201 HSA code objects, freezing executables and resolving kernel descriptors.
- Actual HRX initialization, streams, 64 KiB copy/fill, 4,093-result FP32 vector compute and 16×16 FP32 matrix multiplication, with exact results and full input/output guards.
- Actual LSE→Loom→HRX affine Q6 projection: 51 exact outputs, four unchanged input buffers/guards, no CPU fallback and clean runtime shutdown.
- Actual LSE causal convolution: six zero-padding/history cases passed 680 exact GPU outputs. Four convolution-tail cases passed another 408 exact outputs. Input guards and normal shutdown passed with no CPU fallback.
- Actual LSE repeat: nine GPU cases passed exact byte comparisons, including Qwen-shaped tensors, integer values, BF16 and floating-point bit patterns, with unchanged input guards and no CPU fallback.
- Actual LSE GDN recurrence: 18 GPU cases passed numerical checks, including Qwen's 48 heads of width 128 and GPU state carried from prefill into decode. Paged KV/attention passed exact cache-image and numerical output checks across four runtime metadata cases.
- Qwen3.8-27B-MLX-6bit text inference through LSE→Loom→HRX, with strict GPU execution and clean shutdown. The five-token France prompt and 33 generated token IDs exactly match an independent Apple Metal MLX run of the same checkpoint. Three measured resident requests reached median 10.12 decode tokens/s with explicit flush64/poll64 overrides. A 64-token prompt reached 63.12 prompt tokens/s and 6.76 decode tokens/s; its 33 generated IDs match float32 MLX exactly. These KV128 workloads do not establish PP512/TG128 parity.
- HTTP completion/chat on the same resident Qwen model passed five interleaved requests, identical repeated-prompt output, and graceful shutdown to driver stage 0. The 23-token chat prompt measured 3.24 prompt tokens/s; 28 subsequent decode tokens measured 2.63 tokens/s. GPU execution was required throughout. Recurrent state uses private writable buffers and resets between conversations.
- Loom flash attention passed five GPU cases with numerical output checks, exact cache images and guards, including Qwen D256 and multi-window attention. The offline compiler audit passes 198 cases producing 846 groups.
- Large allocation workloads: 1,024 simultaneous VRAM buffers passed full data and fragmentation/reuse checks. HRX passed 96 pooled buffers alongside a guarded 2 GiB allocation. Driver buffer bookkeeping grows and shrinks in 64-entry pages, with a 4,096-handle limit per client; see [capacity and lifetime limits](docs/BUFFER_CAPACITY.md).
- Native synchronous compute: HSA-loaded kernels passed with 128 and 256 results in both VRAM and shared host memory, including every byte of the input/output guards.
- Bounded hardware AQL dispatch: kernels passed in VRAM and shared host memory, with firmware-acknowledged queue removal and recreation between launches.
- Persistent HSA compute queues: all seven slots, ring wraparound, four CPU producers, shared completion/barrier signals and two processes sharing the GPU passed hardware tests. One process can exit while the other continues on its existing queues.
- Scratch/LDS compute on gfx1201: two independent queues passed full data/guard checks through initial scratch allocation, growth and reuse.
- Public CPU-owned coarse/kernarg HSA pools with GPU access and identical CPU/GPU addresses. CPU access is allowed between completed GPU operations.
- Experimental CPU↔GPU release/acquire ownership transfer: **1,000,000 round trips / 2,000,000 transfers** passed with every 512-byte payload checked both ways, no errors, intact guards and completed dispatch. This validates the tested mapping; it does not advertise general fine-grained atomic support.
- GPU-mediated HSA signals use the validated persistent DMA mailbox by default on the qualified gfx1201 shared-memory path. Stores, arithmetic, bitwise operations, exchange, CAS, waits, concurrent callers, idle restart and all seven application queue slots passed hardware checks. `MAC_HSA_SIGNAL_BACKEND=one-shot` retains the bounded fallback. Native mixed CPU/GPU RMW remains unsupported; see the [service policy](docs/ATOMIC_MAILBOX_SERVICE.md).
- Atomic mailbox benchmark: individual requests reached about 43,000 operations/s; batches of 64 reached 162,000 operations/s, including exact return-value and guard validation. Production synchronous signal calls use individual requests. IRQ wakeup is not implemented.
- HSA host services, CPU signals and software queues. All 119 entry points required by LSE’s pinned HRX resolve; the [behavior status](hsa/API_STATUS.md) explains their limits.
- [amdgpu_mtop](amdgpu_mtop/README.md) terminal dashboard with history graphs, device switching, driver work counters, VRAM accounting and JSON output. `h` toggles Fast (0.1 s) and Slow (0.5 s). Hardware testing received fresh clocks, load, power, temperatures and fan readings while the atomic workload passed; sensor sampling is limited to 1 Hz.
- Stop/Restart GPU through transaction draining and verified reset; recovery still depends on a responsive device and link.

## Not working yet

- General fine-grained CPU/GPU atomic interoperability. The controlled staggered test returned **6,131,574 with Requester Enable OFF** and **6,133,395 with it ON**, versus **11,000,000 expected**. Single-agent controls passed, the bit change was read back and the original value was restored. This result applies to the tested mapping and queue configuration; see the [experiment details](docs/PCIE_ATOMIC_TEST_POLICY.md).
- Long-context/long-generation qualification, broader independent accuracy comparison, macOS MTP validation and broad model/quantization coverage. Short GPU-only CLI and repeated HTTP requests pass on the local Qwen 27B Q6 checkpoint with KV128 and MTP disabled; exact independent token agreement currently covers two 33-token continuations, with the reference precision recorded.
- Full HSA conformance and general executable linking. The gfx12-generic HRX helper loader passes software tests. Hardware profiling and some platform-specific APIs explicitly return errors.
- General firmware telemetry support beyond the tested SMU 14.0.3 firmware profile; independent sensor accuracy and idle/load behavior need further validation.
- Larger PCIe BAR allocation through a public Apple API, and Mesa/Vulkan integration.

## Upcoming

- Measure signal-service batching and IRQ-assisted wakeup options while keeping concurrent native cross-agent RMW unsupported.
- Extend idle/load and concurrent-client validation of the monitor.
- Extend the [combined HRX validation suite](docs/HRX_MACOS_VALIDATION.md) beyond the verified small compute workloads.
- Measure longer prompt/generation workloads and improve inference throughput, extending the verified repeated-request HTTP path.
- Finish the firmware telemetry path for amdgpu_mtop.

Detailed changes and hardware results are in [PROGRESS.md](PROGRESS.md).

**Stop GPU** disables PCI bus mastering, drains pending transactions and requires
a successful function-level reset before closing PCI and freeing session storage.
**Restart GPU** follows that with fresh initialization and firmware loading.
Failures retain backing and block normal operations until a successful retry;
other clients, BAR mappings or interrupt sources must first be closed.
This leaves the enclosure powered. It cannot guarantee recovery of an
unresponsive device/link. Binary replacement still uses **Install Driver** and
macOS may defer activation until reboot; Stop/Restart does not replace the dext.

Xcode 26.6 / DriverKit 25.5 provides PCI configuration access and BAR queries,
but no public PCI resource-resizing operation was found in its headers. A
ReBAR capability ID alone cannot allocate larger Thunderbolt bridge windows.
The driver does not write ReBAR size controls.

The next inference milestones are longer context, independent accuracy comparison
and throughput improvement. Mesa winsys integration remains unimplemented.

Software checks and the non-submitting status probe:

```sh
bash scripts/test-memory-layout.sh
bash scripts/test-pm4-packets.sh
bash scripts/test-psp-firmware-lifetime.sh
swiftc -module-cache-path build/ModuleCache -framework IOKit \
  scripts/macamdgpu_status.swift -o build/macamdgpu_status
build/macamdgpu_status
```

# mac_amdgpu

A third-party native PCIDriverKit-based driver for AMD GPUs on macOS Tahoe
(26.x) running on Apple Silicon. The goal is to talk to a discrete AMD GPU
directly over PCIe — bypassing Metal entirely — by porting the relevant
slices of the Linux `amdgpu` kernel driver into a DriverKit system extension.

Primary target hardware is the **AMD Radeon AI PRO R9700** (RDNA4, gfx1201,
PCI `0x1002:0x7551`) connected via Thunderbolt 5 to an Apple Silicon Mac.
Hardware validation currently covers this card. IP discovery selects device
geometry and architecture properties; other cards still require their own
firmware, initialization and queue validation before support is claimed.

## Hardware requirements

- Apple Silicon Mac. Developed on M5 Pro / Max / Ultra; M1+ likely fine.
- Thunderbolt 5 to an external GPU enclosure. TB4 may work but is untested.
- AMD GPU: Radeon AI PRO R9700 (`0x1002:0x7551`). Other cards are unverified.
- macOS **Tahoe 26.2** or newer.
- **SIP disabled.** The development entitlements we use require it. To
  disable: boot to Recovery (hold the power button on Apple Silicon),
  Utilities → Terminal, then:

  ```
  csrutil disable
  reboot
  ```

## Software requirements

- Xcode 26 or newer, with DriverKit SDK 25.4+.
- Command Line Tools: `xcode-select --install`.
- `xcodegen` (`brew install xcodegen`).
- A paid Apple Developer Program membership with one specific entitlement
  request granted (see below).
- AMD microcode blobs — vendored in [`firmware/`](firmware/) already.

## Apple Developer Portal setup

1. Create an **App ID** for the host app: `<your-prefix>.MacAMDGPUHost`
   (e.g. `com.yourname.MacAMDGPUHost`). Enable the **System Extension**
   capability.

2. Create an **App ID** for the dext:
   `<your-prefix>.MacAMDGPUHost.MacAMDGPU`. The dext bundle id MUST be a
   child of the host bundle id with the suffix `.MacAMDGPU`. Enable the
   **DriverKit** capability, then under "Configure" enable both of:

   - **DriverKit Transport (PCI)** — request either
     `0xFFFFFFFF&0x00000000` (full wildcard) or `0x1002:0x7551`
     (R9700-specific) as the `IOPCIPrimaryMatch`. Apple grants the
     development entitlement through the portal request flow, usually
     within a few hours.
   - **DriverKit Allow Any UserClient Access** — also Apple-granted, also
     quick.

3. Download the two **Development** provisioning profiles for these App IDs.
   Xcode automatic signing pulls them down for you if you sign in.

4. Note your 10-character **Team ID** from Apple Developer → Membership.

## Team ID and bundle ID renaming

The repo ships pinned to team `YBQ9BU6Q6F` and bundle prefix
`com.geramyloveless`. To use your own:

- Override the team ID via environment for builds:

  ```
  export XCODE_TEAM_ID=YOURTEAMID
  ```

- Change the bundle prefix by editing:
  - `project.yml` — search for `com.geramyloveless` and replace.
  - `Host/MacAMDGPUHostApp.swift` — the `dextBundleIdentifier` constant.

  The host bundle id and the dext bundle id MUST form a parent/child pair
  where the dext id is exactly `<host id>.MacAMDGPU`.

## Firmware blob setup

The repo vendors AMD microcode for the GFX11 (RDNA3) and GFX12 (RDNA4)
families directly under [`firmware/`](firmware/). Files are copied
verbatim from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git);
their license (`firmware/LICENSE.amdgpu`) and provenance metadata
(`firmware/WHENCE.amdgpu`) ship alongside.

For the R9700 specifically, the bring-up flow uses:

| File                    | Purpose                          |
| ----------------------- | -------------------------------- |
| `psp_14_0_3_sos.bin`    | PSP SOS bootloader               |
| `smu_14_0_3.bin`        | Power management (PMFW)          |
| `sdma_7_0_1.bin`        | SDMA microcode                   |
| `gc_12_0_1_rlc.bin`     | RLC microcode                    |
| `gc_12_0_1_imu.bin`     | Image Management Unit            |
| `gc_12_0_1_pfp.bin`     | CP — Prefetch Parser             |
| `gc_12_0_1_me.bin`      | CP — Micro Engine                |
| `gc_12_0_1_mec.bin`     | CP — Microcode Engine Compute    |
| `gc_12_0_1_uni_mes.bin` | MES scheduler (unified)          |

Other RDNA3/4 cards use their own IP subversion prefix
(`gc_11_0_0_*` for the RX 7900 family, etc.). See
[`firmware/README.md`](firmware/README.md) for the full mapping.

At runtime the host app reads firmware from its own bundle —
xcodegen's `project.yml` adds the repo's `firmware/` directory to
the host target as a "Copy Files: Resources" build phase, so the
binaries land at `MacAMDGPUHost.app/Contents/Resources/firmware/`
during the build and no manual selection is needed. The
**Pick Firmware Folder…** button exists only as an override.

## Build + install

```
git clone git@github.com:lemonade-sdk/mac-amdgpu.git
cd mac-amdgpu
export XCODE_TEAM_ID=YOURTEAMID
scripts/build.sh
```

`scripts/build.sh` runs `xcodegen` then `xcodebuild`, producing
`MacAMDGPUHost.app` under DerivedData. The script prints the
`BUILT_PRODUCTS_DIR` path at the end.

### Post-build resign (required)

The dext must be re-signed with **minimal** entitlements so AMFI honors
`allow-any-userclient-access` (see "Code-signing gotchas" below). Apple
Development signing normally bakes in `get-task-allow` and
`application-identifier`, which silently disqualify restricted DriverKit
entitlements. You must strip them by re-signing the dext post-build:

```bash
BUILT=$(xcodebuild -showBuildSettings -scheme MacAMDGPUHost -configuration Debug \
  | awk '/ BUILT_PRODUCTS_DIR / {print $3}')
DEXT="$BUILT/MacAMDGPUHost.app/Contents/Library/SystemExtensions/com.yourprefix.MacAMDGPUHost.MacAMDGPU.dext"

cat > /tmp/dext_entitlements.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.developer.driverkit</key><true/>
  <key>com.apple.developer.driverkit.allow-any-userclient-access</key><true/>
  <key>com.apple.developer.driverkit.transport.pci</key>
  <array>
    <dict><key>IOPCIPrimaryMatch</key><string>0xFFFFFFFF&amp;0x00000000</string></dict>
  </array>
</dict>
</plist>
EOF

codesign --force --sign "Apple Development: Your Name (XXXXXXXXXX)" \
  -o library,runtime \
  --entitlements /tmp/dext_entitlements.plist \
  "$DEXT"

codesign --force --sign "Apple Development: Your Name (XXXXXXXXXX)" \
  --entitlements Host/Host.entitlements \
  "$BUILT/MacAMDGPUHost.app"
```

### Install + activate

```
sudo systemextensionsctl developer on
cp -R "$BUILT/MacAMDGPUHost.app" /Applications/
open /Applications/MacAMDGPUHost.app
```

The host app submits an activation request on launch. macOS will prompt to
approve in **System Settings → General → Login Items & Extensions →
Driver Extensions** — toggle MacAMDGPU on. Afterwards,
`systemextensionsctl list` should show the entry as `* * activated enabled`.

## Run the bring-up

The host app window has a row of diagnostic buttons
(**Identity** / **BARs** / **Diagnostics** / **Dump PSP** /
**Dump TMR** / **Dump CmdBuf**) and a single **Initialize GPU**
button that runs every bring-up stage in order, prints the result
of each stage, and bails out the moment any stage fails.

Firmware is auto-loaded from `Contents/Resources/firmware/` inside
the host app bundle — `xcodegen` adds the repo's `firmware/`
directory there. The **Pick Firmware Folder…** button exists only
to override that for testing.

Repeat these checks after installing a changed driver:

```
IPDiscovery   ok
IHInit        ok
GMCInit       ok
PSPInit       ok
PSPLoadSOS    ok
PSPRingCreate ok
TMRSetup      ok
PSPFwLoad     ok
SMUInit       ok
IMUInit       ok
RLCInit       ok
CPInit        ok
MESInit       ok
GFXInit       ok
SDMAInit      ok
```

For granular dext-side diagnostics, tail the dext log:

```
log stream --predicate 'eventMessage CONTAINS "mac.amdgpu"'
```

## Entitlement reference

| Entitlement | Bundle | What it does |
| --- | --- | --- |
| `com.apple.developer.system-extension.install` | host | Required to call `OSSystemExtensionRequest`. Granted automatically by Apple when you enable System Extension capability on the host App ID. |
| `com.apple.developer.driverkit` | dext | Required for any DriverKit dext. Apple-granted. |
| `com.apple.developer.driverkit.transport.pci` | dext | Permits attaching to PCI devices matching the listed `IOPCIPrimaryMatch`. **Requires Apple to approve the specific match scope via the portal.** Wildcard is fine for development; production needs a narrower scope. |
| `com.apple.developer.driverkit.allow-any-userclient-access` | dext | Lets any client app open the user client. Without it, every client app needs `com.apple.developer.driverkit.userclient-access` listing this dext. Apple-granted. |

## Code-signing gotchas

These four surprises eat most of an afternoon if you don't know about them.

1. **Sign the dext with `-o library,runtime`** (flags `0x12000`). Without
   the `library` flag, AMFI silently strips restricted DriverKit
   entitlements and `IOServiceOpen` returns
   `kIOReturnNotPermitted (0xe00002e2)`.

2. **The dext's signed entitlements must not contain `get-task-allow` or
   `application-identifier`.** Apple Development signing normally bakes
   those in. Re-sign the dext post-build with a minimal entitlements plist
   (see the resign script above).

3. **The host app must not have hardened runtime enabled.**
   `IOServiceOpen` against your own dext returns `kIOReturnNotPermitted`
   if hardened runtime is on.

4. **The dext bundle's filename must equal its `CFBundleIdentifier`**
   (Apple bug FB15590713). `project.yml` pins
   `PRODUCT_NAME = PRODUCT_BUNDLE_IDENTIFIER` on the dext target to
   enforce this. If you rename the bundle, both must change together.

## Troubleshooting

- **`kr=0xe00002e2 kIOReturnNotPermitted` from `IOServiceOpen`** — re-sign
  the dext with the minimal entitlements (gotchas #1 and #2), and confirm
  hardened runtime is off on the host (#3).
- **"Extension not found in App bundle"** — check that the dext filename
  matches its bundle id (gotcha #4), and that the dext is at
  `Contents/Library/SystemExtensions/<bundle-id>.dext` inside the host app.
- **"no policy, cannot allow apps outside /Applications"** in the `sysextd`
  log — the host app isn't in `/Applications/`, or its bundle name doesn't
  match the dext bundle name pattern. Copy the app to `/Applications/` and
  retry.
- **Live debugging:**

  ```
  log stream --predicate 'process == "sysextd" OR (eventMessage CONTAINS "DK:")'
  ```

## Project layout

- `Host/` — SwiftUI host app that drives the dext.
- `dext/` — the DriverKit system extension (C++ inside an IOService).
- `hsa/` — userspace HSA runtime, IOKit transport, discovery probe and HRX API audit.
- `project.yml` — xcodegen spec; regenerates `MacAMDGPU.xcodeproj`.
- `scripts/` — build, install, and ping/test helpers.
- `docs/` — local bringup audits and porting plans. Gitignored.
- `upstream/` — vendored Linux + Mesa source for reference. Gitignored.
- `firmware/` — AMD GPU microcode (GFX11 + GFX12 families), copied
  from linux-firmware. Tracked in the repo.
