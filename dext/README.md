# `dext/` — MacAMDGPU PCIDriverKit Extension

Phase 1A skeleton. Loads against AMD R9700 (VID `0x1002`, DID `0x7551`) on Apple Silicon over Thunderbolt 5, logs config space + BARs, and exposes a minimal IPC surface (Ping / GetIdentity / GetBARInfo) plus BAR0–5 mapping via `IOConnectMapMemory64`. No DMA, no MSI-X yet.

## Files

| File | Role |
|---|---|
| `Info.plist` | `IOPCIPrimaryMatch` for R9700, `IOPCITunnelCompatible: true`, declares `MacAMDGPU` driver class + `MacAMDGPUUserClient` |
| `MacAMDGPU.entitlements` | `com.apple.developer.driverkit.transport.pci` scoped to the R9700 VID/DID |
| `MacAMDGPU.iig` | Driver class interface — `Start`, `Stop`, `NewUserClient` |
| `MacAMDGPUUserClient.iig` | UserClient interface — `ExternalMethod`, `CopyClientMemoryForType` |
| `MacAMDGPU.cpp` | All logic for both classes |

The two `.iig` files are processed by Apple's IIG (Interface Generator) tool during the build; it emits C++ glue that the `.cpp` slots into.

## Building

This is **not buildable as a standalone Makefile** — DriverKit dexts must be built inside Xcode. Until we publish an `.xcodeproj`, follow these manual setup steps:

### 1. Create the Xcode project

1. Open Xcode 26.3+. **File → New → Project → macOS → DriverKit Driver**.
2. Save as `mac_amdgpu.xcodeproj` somewhere outside this directory (e.g. `~/Library/Developer/Xcode/Projects/mac_amdgpu/`) so the project metadata doesn't pollute our git tree.
3. Product name: `MacAMDGPU`. Language: C++. The template will create a target.

### 2. Wire in the source files

Delete the auto-generated `MacAMDGPU.cpp`, `MacAMDGPU.iig`, etc. Right-click the target in Xcode → **Add Files to "MacAMDGPU"…**, select these from the `dext/` directory in this repo (use **Reference**, not copy):

- `Info.plist` — set in target → **General → Custom macOS Application Target Properties**, or just drop this as the bundle's Info.plist via build settings (`INFOPLIST_FILE`).
- `MacAMDGPU.entitlements` — assign in target → **Signing & Capabilities → All → Code Signing Entitlements**.
- `MacAMDGPU.iig`
- `MacAMDGPUUserClient.iig`
- `MacAMDGPU.cpp`

### 3. Signing

- **Signing & Capabilities → Team:** your Apple Developer team (same one you use for `qemu-vfio-apple` if that's where the entitlement will live).
- **Bundle Identifier:** something like `com.yourteam.MacAMDGPU` — keep the team prefix consistent.
- **Automatically manage signing:** on (for now). Manual signing once the entitlement is granted.

Capabilities to enable (some appear after the entitlement is granted):
- DriverKit
- DriverKit Transport — PCI
- DriverKit Allow Any UserClient Access

Until Apple grants `com.apple.developer.driverkit.transport.pci` against this bundle ID, builds will fail with a signing error. Workarounds:
- Build for **My Mac (Apple Development)** with `SDKROOT=driverkit` and accept the signing-error warning to at least get a compiled .dext for local testing in **Developer Mode** (`sudo systemextensionsctl developer on`).
- Or strip the entitlement from `MacAMDGPU.entitlements` temporarily to compile-only; the binary won't load against real hardware without it.

### 4. Host app

DriverKit dexts must be packaged inside a regular macOS host app, and the host app must live in `/Applications` for `systemextensionsctl` to stage the dext. Pattern from `qemu-vfio-apple/contrib/apple-vfio/VFIOUserHostApp/`:

- Add a second target: **macOS App**, SwiftUI, name e.g. `MacAMDGPUHost`.
- Embed the dext in the host app: target → **General → Frameworks, Libraries, and Embedded Content** → drag the dext target product.
- Bundle ID: `com.yourteam.MacAMDGPUHost`; dext bundle ID should be `com.yourteam.MacAMDGPUHost.MacAMDGPU` (parent/child).
- Host app's `Info.plist` needs `NSSystemExtensionUsageDescription`.
- Code in the host app calls `OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: …)` to stage the dext.

A minimal host app is on the worklist (Phase 1A.105) — not yet written.

### 5. Loading + verifying

> **Conflict warning — qemu-vfio-apple's VFIOUserPCIDriver.**
> If the qemu-vfio-apple dext is already installed and active, it
> matches **every PCI device** via `IOPCIPrimaryMatch=0xFFFFFFFF&0x00000000`,
> which means it can claim the R9700 first. Symptoms: our dext
> appears as `[activated waiting]` in `systemextensionsctl list`
> but `ioreg -lw0 -c IOPCIDevice` shows the R9700 attached to
> `VFIOUserPCIDriver` instead of `MacAMDGPU`. Resolutions, in
> order of preference:
>
> 1. **Don't have both running.** Stop qemu-vfio-apple's host app
>    before loading ours, or uninstall the VFIO dext temporarily:
>    `systemextensionsctl uninstall <team-id> com.example.VFIOUserHostApp.VFIOUserPCIDriver`.
> 2. **Rely on probe score.** Our Info.plist sets `IOProbeScore=10000`,
>    above the default. A more-specific `IOPCIPrimaryMatch`
>    (exact VID/DID, mask all-ones) also wins ties, so this should
>    be self-resolving in practice — but verify with `ioreg`.
> 3. **Narrow apple-vfio.** Edit its Info.plist to exclude
>    `0x75511002&0xFFFFFFFF` (or any AMD VID) so it never sees the
>    R9700.

```bash
# Move app into place
sudo cp -R ~/Library/Developer/Xcode/DerivedData/MacAMDGPUHost-*/Build/Products/Debug/MacAMDGPUHost.app /Applications/

# Allow staging from non-default location (during development only)
sudo systemextensionsctl developer on

# Run the host app once — it triggers activation
open /Applications/MacAMDGPUHost.app

# In System Settings → Privacy & Security → "Allow" the blocked extension.

# Verify the dext is staged
systemextensionsctl list
# Expect a line for com.yourteam.MacAMDGPUHost.MacAMDGPU [activated enabled]

# Verify the R9700 actually bound to OUR dext, not VFIOUserPCIDriver
ioreg -lw0 -c IOPCIDevice | grep -A2 "vendor-id.*1002.*device-id.*7551"
# Look for "IOUserClientClass" pointing to MacAMDGPUUserClient (good)
# vs. VFIOUserPCIDriverUserClient (qemu-vfio-apple won the match).

# Plug in the eGPU with the R9700 if not already; verify match
log stream --predicate 'eventMessage CONTAINS "mac.amdgpu"' &
# Expect: "mac.amdgpu: matched 05:00.0 vendor=1002 device=7551 …"

# Smoke-test from userspace
swiftc -framework IOKit -framework CoreFoundation \
    ../scripts/macamdgpu_ping.swift -o /tmp/macamdgpu_ping
/tmp/macamdgpu_ping
```

Expected output:
```
opening MacAMDGPU service…
service: connected (handle=<n>)
ping:    0xA117AB1E
identity: 1002:7551 class=030000 bus=05 dev=00 fn=0
BAR0:    memIdx=0 size=524288 type=mem64-prefetch     # 512 KB MMIO regs
BAR2:    memIdx=2 size=268435456 type=mem64-prefetch  # 256 MB visible VRAM (small BAR)
BAR5:    memIdx=5 size=524288 type=mem64              # doorbells
BAR0 mapped: vaddr=0x… size=524288
BAR0[0x0]=0x…  BAR0[0x4]=0x…
ok.
```

If BAR2 reports >256 MB, ReBAR is enabled (the user noted ~1 GB is achievable on AS). Either way, we are in **small-BAR** territory — see `docs/SMALL_BAR.md` (forthcoming) for the moving-window / SDMA strategy.

## Deliberate omissions in this commit

- No DMA path (`IOBufferMemoryDescriptor` + `IODMACommand`).
- No MSI-X (`IOInterruptDispatchSource`, IRQ shared page).
- No FLR / reset.
- No power management.
- No PSP / SMU / GMC / MES — all "below the line" amdgpu work begins in Phase 1B.

These all land in subsequent commits, each gated on the previous one being verified on hardware.

## Pattern source

`qemu-vfio-apple/contrib/apple-vfio/VFIOUserPCIDriver/VFIOUserPCIDriver.cpp` is the architectural template — see `docs/APPLE_VFIO_NOTES.md` for what we mirror, what we tighten, and what we drop.
