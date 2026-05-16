# STATUS

Driver runs on a real R9700, brings PSP up, loads SMU PMFW and IMU
microcode through PSP's ring, gets the SMU mailbox responding, and
clears the IMUInit stage. Still can't render anything — RLC/CP/MES/SDMA
firmware loads are the next blocker (PSP rejects them with
`TEE_ERROR_BAD_PARAMETERS` because the per-IP header offsets in those
files differ from the simple cases that already work).

**What works (v0.0.63 on real R9700 over Thunderbolt 5):**

- Card detection + dext attach over TB5
- Identity reads (VID `0x1002`, DID `0x7551`, rev `0xC0`)
- MMIO via BAR5 (registers) + BAR0 (framebuffer aperture)
- IP discovery from VRAM via MM_INDEX/MM_DATA with full multi-`BASE_IDX`
  support — `gfx_12_0_1` / `psp_14_0_3` / `smu_14_0_3` / `sdma_7_0_1` /
  `mes_12_0_1` / `nbio_7_11` auto-detected
- 9 bringup stages green:
  `IPDiscovery → IHInit → GMCInit → PSPInit → PSPLoadSOS →
  PSPRingCreate → TMRSetup → SMUInit → IMUInit`
- PSP SOS firmware upload + run (kicker / non-kicker selection
  matches upstream `kicker_device_list`)
- PSP ring (GPCOM / km_ring) create + first-frame submit, `FB_FW_RESERV`
  queries return real responses, fence increments per submit
- `LOAD_IP_FW` over the PSP ring for SMU PMFW + IMU_I + IMU_D —
  firmware bytes copied into `fw_pri` (VRAM) via BAR0 aperture and
  handed to PSP at its VRAM MC address, matching upstream's
  `psp_execute_ip_fw_load` + `psp_copy_fw`
- SMU mailbox responsive (`TestMessage` echoes, `GetVersion` works)
  over MP1 BASE_IDX 1
- GMC + MMHUB + GFXHUB bringup with the full
  `mmhub_v4_1_0_gart_enable` / `gfxhub_v12_0_gart_enable` register
  sequences, GFX12 PTE format with `IS_PTE` bit, NBIO HDP
  `remap_hdp_registers` programmed
- GART page table in VRAM at MC `vram_start + 0x800000` (matches
  upstream `amdgpu_gart_table_vram_alloc`), CONTEXT0 enabled with
  PT base + START/END + flush via engine 17

**What's next:**

- **RLC / CP / MES / SDMA firmware parsing.** Each file uses a
  different per-IP header layout (`rlc_firmware_header_v2_0..v2_4`,
  `mes_firmware_header_v1_0` ucode+data, `gfx_firmware_header_v2_0`
  for RS64 PFP/ME/MEC + P0..P3 stacks, `sdma_firmware_header_v3_0`).
  The per-IP extractor in `dext/amdgpu/amdgpu_ucode_extract.cpp` is
  in but each file's payloads still bounce off PSP with
  `0xFFFF0006 = TEE_ERROR_BAD_PARAMETERS` — wrong offsets/sizes for
  the actual firmware bytes. Need to cross-check field-by-field
  against upstream `amdgpu_ucode_init_single_fw`.
- RLC autoload start, CP RS64 PFP/ME/MEC programming, MES
  `set_hw_resources` + queue init.
- GFXHUB gart_enable re-run after RLC autoload.
- First PM4 packet on a GFX12 compute queue.

**To use it:** install the host app, click **Initialize GPU** in the
test UI, watch each bring-up stage print. Expected output today:
stages 1–9 green, stage 10 (`RLCInit`) times out because RLC
microcode never loads.

  
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

A successful run today gets through `IPDiscovery → PSPInit →
PSPLoadSOS → PSPRingCreate → TMRSetup` plus an interleaved
LoadFirmware of `smu_<v>.bin` via the PSP ring, then `SMUInit →
GMCInit`. The next blocker is `RLCInit`, which returns
`kIOReturnUnsupported` until SDMA / RLC / uni_mes microcode parsing
is finished (see `docs/audit/00_SUMMARY.md`).

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
- `project.yml` — xcodegen spec; regenerates `MacAMDGPU.xcodeproj`.
- `scripts/` — build, install, and ping/test helpers.
- `docs/` — porting notes, Apple-VFIO reference notes, AS/DART limit
  cheat-sheets, and the phase-by-phase port plans.
- `upstream/` — vendored Linux + Mesa source for reference. Gitignored.
- `firmware/` — AMD GPU microcode (GFX11 + GFX12 families), copied
  from linux-firmware. Tracked in the repo.

For granular task state see [`WORKLIST.md`](WORKLIST.md). For the phase
plan see [`ROADMAP.md`](ROADMAP.md). For the divergences-from-Linux
summary see [`docs/PORTING_NOTES.md`](docs/PORTING_NOTES.md).
