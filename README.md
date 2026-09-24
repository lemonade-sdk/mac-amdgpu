# mac_amdgpu

**Working AMD GPU compute and AI inference on Apple Silicon macOS.**

mac_amdgpu is a native PCIDriverKit driver and userspace HSA runtime. It runs
real workloads on the **AMD Radeon AI PRO R9700** (`gfx1201`) connected to an
Apple Silicon Mac over Thunderbolt 5. The driver initializes the GPU, loads
firmware, manages memory and compute queues, and executes GPU kernels.

With [Lemon Seed Engine (LSE)](https://github.com/Geramy/LSE),
**Loom → HRX → HSA → mac_amdgpu now runs Qwen3.8-27B-MLX-6bit text generation
and HTTP chat on the R9700**. The validated inference runs require GPU execution
and complete with clean runtime shutdown. This is a working compute stack with
ongoing performance and compatibility development.

Start with the [local build and chat instructions](LOCAL_RUN.md), or the
[detailed LSE reproduction guide](docs/LSE_QUICKSTART.md) for pinned dependencies
and validation commands.

## What works today

- **GPU initialization and firmware:** discovery, firmware loading, memory
  setup, CP/MES compute queues and SDMA on the R9700.
- **GPU memory and transfers:** verified SDMA copies, VRAM and shared host
  allocations, cross-process buffer sharing, and dynamic allocation bookkeeping.
  Tests include 1,024 simultaneous VRAM buffers and HRX pooled allocations
  alongside a guarded 2 GiB buffer. [Allocation details](docs/BUFFER_CAPACITY.md)
- **HSA execution:** native gfx1201 code objects, persistent AQL queues,
  scratch/LDS allocation and reuse, barriers and completion signals. All seven
  application queue slots, multiple CPU producers and two processes sharing
  the GPU have passed hardware tests. All 119 entry points required by the
  pinned HRX build resolve. [API behavior](hsa/API_STATUS.md)
- **CPU/GPU coordination:** GPU-mediated HSA signal operations and a persistent
  DMA mailbox support the validated shared-memory path. Ownership-transfer
  testing passed one million round trips with every payload checked.
  [Signal service](docs/ATOMIC_MAILBOX_SERVICE.md)
- **AI inference:** Qwen 27B Q6 generation, resident-model HTTP completion/chat,
  and conversation-state reset. The tested model operations include quantized
  projections, convolution, recurrent layers, paged KV caches and attention.
  Independent MLX references match exact prompt and generated token IDs on
  two qualified fixtures. [Inference validation](docs/LSE_PERFORMANCE.md)
- **Measured optimizations:** Q6 weights are reused across prompt rows using
  LDS; shared HIP/Loom operand selection chooses the accepted tiled BF16 path
  for four measured large projection shapes. Native FP8/BF8 conversion and Q6
  residual matrix kernels also run correctly on qualified ordinary inputs,
  but did not beat tiled BF16. Decode attention shares query/key scores;
  eligible single-device decode selects its submission batch size from measured
  execution times. [Operand evidence](docs/LOOM_HIP_PARITY.md)
- **GPU monitoring:** [amdgpu_mtop](amdgpu_mtop/README.md) provides history
  graphs, device switching, clocks, temperatures, power, fan readings, driver
  work counters, VRAM accounting and JSON output. Press **h** for 0.1 s / 0.5 s
  screen refresh. Firmware sensors update at up to 1 Hz; unreliable raw activity
  readings are identified explicitly. [Monitor details](docs/GPU_MONITOR.md)
- **Controlled lifecycle:** Stop/Restart GPU drains work and uses a verified
  reset before releasing session resources. Other processes can continue using
  their existing queues when a participating process exits.
- **GPU dispatch timing:** [rocprofmac](tools/rocprofmac/README.md) reads
  hardware start/end timestamps from completion signals. Its R9700 qualification
  passed 64 guarded dispatches, followed by actual HRX and Qwen model captures,
  without marker kernels. All 110,154 events in the first model capture match
  host submission counts. Three warm requests measured 12.58 TPS with profiling
  off and 12.22 TPS with it on, with identical text; this sequential comparison
  is workload-specific. The tool also includes a macOS CPU sampling helper.

## Measured Qwen performance

**HIPC and Loom have different performance.** The earlier HIPC
(`--dialect hip`) result is recalled at approximately **34 decode tokens/s**;
its log, model, quantization, context and MTP settings still need to be recovered
for a matched comparison. It is not a measured macOS Loom result.

The local Qwen3.8-27B-MLX-6bit checkpoint runs through GPU kernels using **Loom**.
Cooperative RMS normalization and four-column Q6 decode use shared HIP/Loom code
and policy. GPU inference passes repeated-text and clean-shutdown checks,
including full **1,024-input/1,024-output** requests.

The rebuilt default, using FP32 for the two M256 projection shapes whose BF16
profile failed accuracy qualification, measured:

| Workload | Prompt processing | Decode |
| --- | ---: | ---: |
| 512 input / 129 output, KV1024 | **88.82 tokens/s** | **16.67 tokens/s** |

This is one measured request after two warmups, with MTP disabled, flush64 and
64 µs polling. All three responses matched, the measured request compiled no new
shaders, and the server shut down cleanly. Output count includes the first token
from prefill, leaving 128 timed decode steps. A separate run using the prior HSA
library measured 88.68 PP/s and 16.68 TPS on the same rebuilt server.

**rocprofmac works on this model.** A matched GPU timestamp capture preserved
all generated text and added about 1.0% prefill time and 3.7% decode time in this
comparison. The two feed-forward projection shapes account for 67.3% of summed
prefill kernel time, making matrix-kernel optimization the main prefill priority.

Experimental prefill kernels reached higher throughput but failed the unchanged
accuracy limit; they are withheld from automatic selection. The four-column
INT8 decode candidate passes its GPU numerical suite and remains experimental
pending full-model quality and performance checks. Historical measurements,
profiling conditions, and [current evidence](docs/LSE_PERFORMANCE.md) are kept
separately. Matched llama.cpp performance parity remains an active goal.
See the [reproduction command](LOCAL_RUN.md#experimental-resident-benchmark).

## Current scope

The working hardware configuration is **Apple Silicon + Thunderbolt 5 + R9700**.
Other AMD cards need their own firmware, initialization and queue validation.
The project currently provides a compute path for applications such as LSE;
Metal/display integration and Mesa/Vulkan support are separate future work.

- **Inference coverage is expanding:** short-context generation and repeated
  HTTP requests work. Longer contexts, longer generations, MTP and additional
  models/quantizations are being qualified. Qwen Q6 has completed an exact
  1,024-input / 1,024-output run with KV capacity 2,048; throughput optimization continues.
- **HSA support targets real application needs:** the pinned HRX interface works,
  while full HSA conformance, general executable linking and full hardware profiling
  remain incomplete. Unsupported APIs report errors rather than simulated success.
- **Shared signals work through the mediated path:** native simultaneous CPU/GPU
  atomic read-modify-write is not supported on the tested connection. The driver
  uses the validated ownership-transfer/mailbox design instead.
  [Atomic test results](docs/PCIE_ATOMIC_TEST_POLICY.md)
- **Telemetry is firmware-specific:** clocks and other readings are available
  for the tested SMU 14.0.3 profile. Its raw GFX activity can report 100% while
  engine registers show idle, so that value is not presented as verified usage.
- **Platform constraints remain:** no public Apple BAR-resizing API has been
  identified. The existing assigned BAR windows are sufficient for the working
  compute path. Stop/Restart requires a responsive device and link; replacing
  the installed dext still uses macOS system-extension activation.

## Upcoming

- Reduce kernel launches through additional correct Loom fusion, including
  paired recurrent output/state computation.
- Optimize longer prompts and generations; KV-cache growth now passes the
  1,024-input / 1,024-output workload.
- Improve throughput using measured tile, register, LDS and workgroup choices.
- Expand model accuracy checks, multi-client coverage and hardware compatibility.
- Optimize kernels using LSE dispatch traces, expand profiler measurements,
  improve activity measurement and validate IRQ-assisted wakeups.

Detailed engineering results live in [PROGRESS.md](PROGRESS.md). The README
summarizes current capabilities; it is not a running release log.

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

## HRX status regression check

After building the patched HRX adapter, run this host-only check from the project
root. It compiles the actual HRX/IREE status conversion with ASan/UBSan and checks
all 16 public status codes in both directions, including callback `ABORTED`
propagation. It does not initialize HSA, open the GPU, or run device work.

```sh
python3 scripts/test-hrx-status-roundtrip.py \
  --source build/hrx-macos-source \
  --build build/hrx-macos-adapter
```

Both paths must name the matching patched source and configured build; the
build must contain `compile_commands.json` and the built IREE base library.

The allocator regression check also runs entirely on the CPU:

```sh
bash scripts/test-hrx-tlsf-pool.sh
```

It exercises the actual HRX slab allocator: reuse beyond its recent-slab hints,
dependency ordering, fragmentation, and concurrent allocation with trimming.
The adapter searches all existing slabs before growing the pool, avoiding
premature exhaustion when reusable memory remains in older slabs.

The Loom floating-mode regression is also host-only. It calls the actual binary
descriptor writer and assembly metadata printer for every supported processor
profile, checking that both preserve FP32 and FP16/64 subnormals and use the
same rounding modes:

```sh
source scripts/amdgpu-llvm-env.sh
python3 scripts/test-loom-denorm-descriptor.py \
  --hrx-source build/hrx-macos-source \
  --hrx-build build/hrx-macos-adapter
```

Use matching patched source and built HRX dependencies. This check writes only
its own test outputs; it neither changes the compiler libraries nor opens HSA.

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
