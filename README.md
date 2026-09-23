# Status

**v0.1.77 — GPU-only buffers and bounded transfers.**
A separate allocator covers usable VRAM above BAR0, keeping firmware and visible
staging storage separate. Owner-only buffer upload/readback and SDMA copy APIs
validate handles, ranges and transfer limits. Large VRAM Test reserves 22 GiB
and checks 4 KiB transfers at three offsets, including its end; it does not
validate every byte. All 25 regression suites and the Debug build pass.
Hardware acceptance of these new APIs is pending.

**v0.1.76 — compute shader execution verified.**
Two consecutive runs passed all 32 shader results, input words and guards,
with changing seeds, fences 3 and 6, and successful allocation reuse. Explicit
VMID 0 in Linux-style GFX indirect-buffer submission fixed the observed shader
instruction-fetch fault. Firmware telemetry is unavailable: the board reports
interface 0x33, while the verified decoder covers 0x2e. No telemetry transfer
is issued for an unverified interface.

**v0.1.75 — compute fault isolated to shader instruction fetch.**
Cache preparation and register-programming fences passed. Shader completion
timed out with a GFXHUB VMID-3 fault at the shader code address, while the queue
itself used VMID 0. Output was not checked. Stop GPU recovered through function
reset and session release without an enclosure power cycle.

The native [amdgpu_mtop monitor](amdgpu_mtop/README.md) enumerates attached
MacAMDGPU devices with GPU switching and JSON output. Live discovery and VRAM
capacity queries work; dynamic firmware metrics require a verified interface-0x33 layout.

The current target is AI compute and model inference. The initial [HSA runtime](hsa/README.md)
discovers the live GPU through IOKit and passes lifecycle tests. General dispatch, queues, signals, executable loading and HRX/LSE inference
remain required; the fixed diagnostic is not an HSA dispatch interface.

Older release history and detailed hardware results are in [PROGRESS.md](PROGRESS.md).

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

The next milestones are general compute dispatch, the HSA interface for HRX,
and data-verified inference workloads. Mesa winsys integration remains unimplemented.

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
Other RDNA4 / gfx1201 cards should work with the same firmware images.

## Hardware requirements

- Apple Silicon Mac. Developed on M5 Pro / Max / Ultra; M1+ likely fine.
- Thunderbolt 5 to an external GPU enclosure. TB4 may work but is untested.
- AMD GPU: Radeon AI PRO R9700 (`0x1002:0x7551`). Other RDNA4 / gfx1201
  cards should work with the same firmware blobs.
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
