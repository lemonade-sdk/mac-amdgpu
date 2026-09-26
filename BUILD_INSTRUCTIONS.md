# Building MacAMDGPU and its tools

This is the macOS DriverKit AMD-GPU driver and its companion tools. Three
things to build:

1. **The driver** — the `MacAMDGPU` DriverKit system extension (dext) + its
   `MacAMDGPUHost` activation app. This is what exposes the GPU to userspace.
2. **amdgpu_mtopg** — the SwiftUI 2D GPU monitor (reads the driver over IOKit).
3. **amdgpu_mtop** — the terminal braille monitor (same telemetry, TUI).

The driver is the only one that needs Apple Developer signing, a paid program
membership, and a live AMD GPU. The monitors are plain Swift apps that read the
driver; they build with no GPU and no signing ceremony.

---

## 1. The driver (dext + host app)

### What is required (accessories)

| Item | Why | Where |
|---|---|---|
| **Apple Silicon Mac**, macOS Tahoe 26.2+ | DriverKit + the HSA/HRX host path | — |
| **SIP disabled** (`csrutil disable` from Recovery) | The development DriverKit entitlements require it | one-time |
| **Xcode 26+** with DriverKit SDK 25.4+ | Builds + signs the dext | App Store / Xcode |
| **Command Line Tools** (`xcode-select --install`) | `xcrun`, code-signing tools | — |
| **xcodegen** (`brew install xcodegen`) | Generates the `.xcodeproj` from `project.yml` | Homebrew |
| **Paid Apple Developer Program membership** | Code-signing + provisioning | developer.apple.com |
| **Two App IDs + 2 Development profiles** (host + dext) | DriverKit is Apple-granted via a portal request | see below |
| **AMD microcode firmware blobs** | The dext loads these at bring-up | vendored in [`firmware/`](firmware/) |
| **The AMD GPU** (Radeon AI PRO R9700, `0x1002:0x7551`) over Thunderbolt 5 | The actual device | — |

The firmware blobs in [`firmware/`](firmware/) are already vendored (see
`firmware/WHENCE.amdgpu` for provenance). You do not need to fetch them.

### Apple Developer Portal setup (one-time)

1. **Host App ID** `<prefix>.MacAMDGPUHost` — enable the **System Extension**
   capability.
2. **dext App ID** `<prefix>.MacAMDGPUHost.MacAMDGPU` (MUST be a child of the
   host id, suffix exactly `.MacAMDGPU`) — enable **DriverKit**, then under
   Configure enable both:
   - **DriverKit Transport (PCI)** — request `0xFFFFFFFF&0x00000000` (wildcard)
     or `0x1002:0x7551` (R9700-only) as `IOPCIPrimaryMatch`.
   - **DriverKit Allow Any UserClient Access**.
3. Download the two **Development** provisioning profiles (Xcode automatic
   signing fetches them if you're signed in).
4. Note your 10-char **Team ID** (Apple Developer → Membership).

The repo is pinned to team `YBQ9BU6Q6F` / bundle prefix `com.geramyloveless`.
To use your own: `export XCODE_TEAM_ID=YOURTEAMID` for the team, and replace
`com.geramyloveless` in `project.yml` + `Host/MacAMDGPUHostApp.swift`
(`dextBundleIdentifier`) for the bundle prefix — keeping the host/dext
parent/child pair intact.

### Build

```bash
git clone git@github.com:lemonade-sdk/mac-amdgpu.git
cd mac-amdgpu
export XCODE_TEAM_ID=YOURTEAMID
scripts/build.sh            # xcodegen + xcodebuild -> MacAMDGPUHost.app
```

`scripts/build.sh` runs `xcodegen` (from `project.yml`) then `xcodebuild`,
producing `MacAMDGPUHost.app` under DerivedData and printing the
`BUILT_PRODUCTS_DIR`.

> **Note:** `project.yml` carries the driver version
> (`CURRENT_PROJECT_VERSION`). Bump it when you change the dext ABI so the
> userspace tools can gate on a minimum build.

### Post-build resign (required)

The dext must be re-signed with **minimal** entitlements so AMFI honors
`allow-any-userclient-access`. Apple Development signing normally bakes in
`get-task-allow` and `application-identifier`, which silently disqualify
restricted DriverKit entitlements. Strip them by re-signing the dext post-build:

```bash
BUILT=$(xcodebuild -showBuildSettings -scheme MacAMDGPUHost -configuration Debug \
  | awk '/ BUILT_PRODUCTS_DIR / {print $3}')
DEXT="$BUILT/MacAMDGPUHost.app/Contents/Library/SystemExtensions/<prefix>.MacAMDGPUHost.MacAMDGPU.dext"

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

```bash
sudo systemextensionsctl developer on
cp -R "$BUILT/MacAMDGPUHost.app" /Applications/
open /Applications/MacAMDGPUHost.app
```

The host app submits an activation request on launch. macOS prompts to approve
in **System Settings → General → Login Items & Extensions → Driver Extensions**
— toggle MacAMDGPU on. Afterwards:

```bash
systemextensionsctl list | grep -i macamdgpu
# expect: ... .MacAMDGPU (0.1.NN/NN) ... [activated enabled]
```

A driver *upgrade* (same team + bundle id) usually activates automatically on
launch without a fresh prompt; a first install needs the manual toggle.

### Verify

```bash
build/hsa-wait-perf/mac-hsa-info
# expect: registry=... driver=NN stage=0 GFX=12.0.1
```

---

## 2. amdgpu_mtopg (SwiftUI 2D monitor)

No GPU, no signing ceremony — plain `swiftc` against the macOS SDK, system
frameworks only (SwiftUI, AppKit, IOKit, CoreFoundation), adhoc-signed, no
entitlements, no network.

```bash
tools/amdgpu_mtopg/build.sh            # -> tools/amdgpu_mtopg/build/amdgpu_mtopg.app
tools/amdgpu_mtopg/build.sh --clean    # rebuild from scratch
open tools/amdgpu_mtopg/build/amdgpu_mtopg.app
```

Requires a driver build **≥ 172** installed (for the core read-only telemetry);
the **UMC memory-activity chart's primary source** (MMHUB PERFSTATUS,
selector 68) needs driver **≥ 198**. On a pre-198 driver it falls back to the
SMU `UmcActivityPercent` field and says so. Esc or closing the window quits;
the IOKit connection is opened and closed every refresh, never held.

It reads the same driver selectors as the terminal monitor (43 identity, 21
QueryInfo, 47 SMU metrics, 61 software_stats, 62 clocks, 68 MMHUB UMC) — see
`tools/amdgpu_mtopg/README.md` for the per-panel source table.

---

## 3. amdgpu_mtop (terminal TUI)

The braille terminal companion to mtopg, same driver, same telemetry. See
`amdgpu_mtop/` for its build/run. It needs the same installed driver (≥ 172).

---

## Version gating

The userspace tools gate on the driver build (selector 43 returns the build).
Current driver version is in `project.yml` (`CURRENT_PROJECT_VERSION`). A tool
that needs a newer sensor (e.g. the MMHUB UMC sensor, build 198) checks the
returned build before trusting that source and falls back + captions otherwise.

## Related

- [LSE build](https://github.com/Geramy/LSE/blob/main/BUILD_INSTRUCTIONS.md) —
  the inference engine that runs on this driver.
- [LSE quickstart](docs/LSE_QUICKSTART.md) — driver + LSE together.
