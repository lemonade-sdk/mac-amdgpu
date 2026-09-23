# Status

**v0.1.73 — reusable, aligned GART reservations (hardware validation pending).**
GMC and GTT now share a bounded range allocator. Successful unbind reclaims
non-trailing holes, GPU addresses honor the requested alignment, and ownership
IDs reject stale unbinds after address reuse. Publication/invalidation failures
retain their reservations. All 19 regression suites, the signed Debug build
and strict signature verification pass. Runtime 172 remains the hardware-tested
baseline below; general GTT allocation is still gated.

The current target is AI compute and model inference. HRX integration is under
investigation; shader dispatch and a working inference runtime remain required.

**v0.1.72 — checked GART teardown and two-way host-memory diagnostic.**
The new Host Memory Copy button verifies distinct 16 KiB patterns from host
memory to VRAM and back through SDMA/GART. It uses DriverKit's prepared-DMA
access API, checks every word, and retains failed-test storage until Stop GPU.
Bindings verify PTE writes, invalidate both hubs, and clear/invalidate PTEs
before releasing DMA mappings. GART/transfer, client lifecycle, shutdown,
GMC flush and CP startup tests pass, as do the signed build and signature
verification. Verified runtime 172 passes five two-way host-memory tests with
zero mismatches, including mapping reuse and a software restart with firmware
reload. CP register/fence, GFX CS and VRAM SDMA copy also pass; all 23 captured
GFXHUB fault snapshots are zero. General GTT remains disabled and arbitrary
freed aperture holes are not yet reclaimed. These bounded tests do not yet
establish long-running stability or shader execution.

**v0.1.71 — one allocation cursor for the GART aperture.**
Firmware bindings and GTT bindings now reserve disjoint page-table entries
through the same GMC-owned cursor. Reinitializing the GART facade preserves
existing reservations. A production-path regression reproduces the prior
overlap and checks interleaved bindings, owned buffers and exhaustion. GART
binding/CP startup tests, the signed Debug build and signature verification
pass. GTT stays disabled pending mapping teardown and data-verified host-memory
transfers; this correction alone does not establish GPU access to system RAM.
Verified runtime 171 initializes, passes GFX CS and a correct 4 KB SDMA copy,
and stops successfully.

**v0.1.70 — GFX command-stream submission through the kernel queue.**
CSCreate(GFX), CSWriteDwords, SubmitIB and WaitFence now use the MES-mapped
kernel graphics queue and VRAM completion fence. The host adds GFX CS Smoke.
One outstanding raw submission retains the existing resource lifetime rules;
compute queues and general scheduling remain unfinished. Client lifecycle/RPC
and CP ring/startup tests, the signed Debug build and signature verification
pass. Verified runtime 170 passes 19 GFX CS submissions through a ring wrap,
SDMA CS, a data-verified 4 KB SDMA copy, and CP scratch/fence diagnostics with
zero captured GFXHUB faults. Software restart reloads firmware and another GFX
CS passes; final Stop GPU succeeds. This proves NOP/fence submission through
the API, not shader execution or general compute workloads.

**v0.1.69 — preserve CP capacity across hardware ring wraps.**
Verified runtime 168 executes CP register writes and memory fences, passes SDMA
copy and CS/WaitFence, and repeats CP/SDMA successfully after software restart.
Its ninth CP test stops before submission because hardware RPTR wraps to zero
while software WPTR remains monotonic. 169 computes occupancy modulo ring size
and separately bounds unpublished words, preserving the guard against overwriting
unconsumed work. A regression reproduces the old rejection, then passes 80
completed submissions across repeated wraps and rejects a stalled full ring.
Verified runtime 169 passes 25 consecutive CP scratch/fence tests across three
full ring revolutions, reaching WPTR 12800 with zero GFXHUB fault snapshots.
SDMA then copies 4096 bytes with zero mismatches, CS/WaitFence passes, and Stop
GPU succeeds. This validates the wrap correction, not long-running stability.

**v0.1.68 — map the kernel graphics queue through MES KIQ.**
Runtime 167 completes the two-pipe MES bootstrap without GFXHUB faults, but its
first legacy GFX command causes a CPG read fault at address zero. 168 follows
Linux's default asynchronous GFX path: enable MEC/GFX before KIQ bootstrap,
upload a complete v12_gfx_mqd to VRAM, require KIQ's mapping acknowledgement,
and submit through the assigned doorbell. The legacy MMIO WPTR fallback is
removed from CP. The descriptor is checked byte-for-byte against Linux's
initializer, including failure retention and startup ordering tests. Nine
focused suites, the signed Debug build, and signature verification pass.
Verified runtime 168 passes eight CP scratch/fence tests (about 1.9 ms per fence),
a 4096-byte SDMA copy with zero mismatches, and SDMA CS/WaitFence. GFXHUB faults
stay zero. The ninth CP test is rejected by software capacity accounting at the
first ring wrap; no command is submitted. Software Restart GPU succeeds, followed
by another passing CP test and correct SDMA copy. Final Stop GPU succeeds.

**v0.1.67 — complete the uni-MES KIQ bootstrap.**
Runtime 166 failed CP's register-write test without submitting a memory fence.
The GFXHUB address-zero fault first appeared during MES startup, before the
legacy graphics queue was enabled. 167 starts both MES pipes, configures private
KIQ resources, and maps SCHED through KIQ instead of activating it directly.
It corrects the MQD header/thread-mask offsets and control defaults, publishes
MES firmware readiness only after all four PSP payload acknowledgements, and
preserves page-walker priority when configuring the GMC cache. Startup snapshots
now bracket the MES substeps. Verified runtime 167 acknowledges all six MES
bootstrap messages and has no GFXHUB fault through initialization. Its CP scratch
test still times out after 101110 us; a new CPG/VMID0 read fault (0x00000d3c,
address zero) first appears after submission. No memory fence was submitted.
Stop GPU succeeds. The MES firmware acknowledgement regression also passes.

**v0.1.66 — isolate CP command execution from fence completion.**
The CP diagnostic now runs Linux's SCRATCH_REG0 register-write test before
submitting RELEASE_MEM. A scratch failure stops the test before memory-fence
allocation/submission. Startup snapshots bracket MES, HQD programming and
GFX/MEC enable, and include GFXHUB faults, queue pointers and instruction
pointers. Focused tests, the signed build and signature verification pass.
Runtime 166 initializes but fails the scratch test after 100 ms. The target
remains 0xcafedead; no memory fence is submitted. The address-zero CPC/VMID0
fault first appears during MESInit. Stop GPU succeeds.

**v0.1.65 — CP command ring and write-back storage use VRAM.**
164 passed SDMA but CP never advanced RPTR. CP still used PCI DMA addresses
without GPU mappings. 165 places its ring and write-back page in visible VRAM,
verifies new commands and the WPTR shadow before notification, and reads RPTR
and completion fences through BAR0. The unused legacy MQD allocation is removed.
Timeouts retain submitted memory until reset and now report GFXHUB fault state.
Focused tests and the signed Debug build pass. Verified runtime 165 initializes,
but CP still times out with RPTR zero and PFP/ME halted. The timeout snapshot
reports GFXHUB fault status 0x00040b5d (CPC, VMID 0, address zero); its onset is
not yet isolated. Software Stop/reinitialize recovers the session, after which
SDMA's 4096-byte copy and CS/WaitFence pass. CP execution remains unproven.

**v0.1.64 — SDMA write-back and completion fences use VRAM.**
163 passed MES but timed out on SDMA0's startup fence. 164 moves both SDMA
write-back pages into visible VRAM and updates startup, copy and CS/WaitFence
polling to read the GPU-written slots through BAR0. The write-pointer shadow is
verified before notification, and failed/all-ones reads cannot complete a CS.
The existing SDMA doorbell/MMIO fallback is preserved. Focused tests pass.
Verified runtime 164 completes all initialization stages, both SDMA startup
fences, a 4096-byte copy with zero mismatches, and CS submission/WaitFence.
The CP fence still times out with RPTR zero and PFP/ME halted again. Stop GPU
then succeeds. CP memory placement remains a separate task.

**v0.1.63 — MES bootstrap storage uses GPU-addressable VRAM.**
Runtime 162 reached MES but never completed its first resource command. Its MES
buffers held direct PCI DMA addresses with no corresponding GPU mapping. 163
places all internal MES buffers in visible VRAM, verifies packet/MQD writes
before publishing the doorbell, and reads completion fences from VRAM. A timeout
retains the pending submission until reset and records HQD pointers, MES control
and GFXHUB fault registers. Focused tests cover VRAM placement, upload failure,
ring wraparound, completion, timeout and removal. Verified runtime 163 completes
both MES resource commands and its scheduler query in about 1.1 ms each. GFX
queue resume passes, then SDMA0's startup fence times out. Stop GPU succeeds.
No CP fence or SDMA copy test followed the failed initialization; the incomplete
uni-MES KIQ path and CP execution remain open.

**v0.1.62 — accept discovery records without register bases.**
Verified runtime 161 stopped in IP discovery before firmware upload. Its new
validator rejected a base-address array after the valid GC/MMHUB/MP0/MP1 records.
Linux permits zero-base records that still describe an IP version; 161 wrongly
rejected them. 162 accepts these records while preserving checksums, complete
record bounds and atomic publication. A production-parser regression reproduces
the old rejection and now passes for v3/v4 and 32/64-bit records. Build passes.
Verified runtime 162 now passes discovery, firmware loading and CP preparation.
MES times out on its first SET_HW_RESOURCES command after two seconds; GFX/SDMA
initialization and engine tests were not reached. Stop GPU then completed reset,
PCI close and resource release. CP execution remains unproven.

**v0.1.61 — offline review candidate; hardware validation pending.**
The Linux comparison found GFXHUB using an undiscovered GMC slot, MES receiving
incorrect MMHUB bases, premature RLC resume, and missing MMHUB invalidation steps.
161 corrects these, validates discovery data, measures firmware waits with a
monotonic clock, and hardens client ownership, fences and shutdown. Failed work
now requires Stop GPU before retrying; unfinished MES user queues are disabled.
All 14 offline suites and the Debug build pass. No live GPU tests were run during
the review. See [the full review and remaining gaps](docs/DRIVER_REVIEW_2026-09-22.md).

**160 hardware result:** verified runtime 160 stopped at stage 12 because
GFXHUB checked the missing GMC slot. CP was not tested. The last successful
SDMA copy was on 159; CP execution remains unproven on 161.

**v0.1.60 — prepare GFXHUB and MES before resuming the legacy graphics queue.**
Verified build 159 initializes all 15 stages and copies 4096 bytes through SDMA
with zero mismatches. CP still times out: its reset bits now clear correctly,
but PFP/ME halt bits are set again by the time the fence test times out.
The source starts CP before GFXHUB and MES, unlike Linux's ordering.
160 keeps the queue halted during stage 12 (firmware/GFXHUB/constants), runs
MES in stage 13, then programs and enables the legacy queue in stage 14.
Control snapshots bracket MES and precede submission to narrow down the halt
transition. Startup ordering/failure tests and the signed build pass; CP execution
on 160 was not reached: the stage-12 prerequisite failed as described above.

**v0.1.59 — verify the running driver during upgrades; correct CP checks for inactive pipes.**
The host now distinguishes the installed bundle from the driver answering its
user client. An old or unverified driver cannot run initialization or engine
tests; Stop GPU remains available for compatible older drivers. Replacement
activation first stops the attached GPU session, and completion is followed by
bounded runtime-build checks. These checks do not force macOS to terminate an
old process. The copy-to-Applications installer is unchanged.

**v0.1.58 hardware finding:** macOS initially enabled 158 while the old 157
process remained attached as “terminating for upgrade via delegate.” The tests
in that interval exercised 157, not 158. After Stop GPU, closing the app, and
terminating only the retired process, 158 attached without a GPU power cycle.
Its first real initialization acknowledged all three CP firmware entry points,
then stopped because our added readback checked unused graphics pipe 1, which
returned `0xDEADBEEF`. Linux defines one graphics pipe and two MEC pipes on
GC 12.0.1, while writing two/four startup slots without reading them back.
159 preserves those writes and reset pulses, but validates only active slots.
The verified 159 CP fence still timed out; see 160 above.


**v0.1.57 tested — software Stop/Restart and SDMA copy pass; CP still stalls.**
CP and its GFX setup now use Linux's GC register segment indices. Submissions
pad to the 256-DWORD fetch boundary, publish a 64-bit write pointer, reject
ring overflow, and report real elapsed timeout plus hardware pointers.
The test previously labeled KIQ is the legacy GFX RB0 path; the host now calls
it **CP GFX Fence**. The hardware fence still times out: WPTR advances to 256 while RPTR remains
zero. CP_STAT reports PFP/CP busy. Software shutdown followed by full
reinitialization passed, as did the combined Restart GPU button; each was
followed by a verified 4096-byte SDMA copy with zero mismatches. No physical
power cycle was used during those two restart tests.

**Stop GPU** disables PCI bus mastering, drains pending transactions and requires
a successful function-level reset before closing PCI and freeing session storage.
**Restart GPU** follows that with fresh initialization and firmware loading.
Failures retain backing and block normal operations until a successful retry;
other clients, BAR mappings or interrupt sources must first be closed.
This leaves the enclosure powered. It cannot guarantee recovery of an
unresponsive device/link. Binary replacement still uses **Install Driver** and
macOS may defer activation until reboot; Stop/Restart does not replace the dext.
The installer/copy-to-Applications implementation was not changed for this release.


**v0.1.56 tested — SDMA copy now passes; CP execution still fails.**
The v0.1.55 copy fence completed but 128/1024 destination dwords differed,
despite a verified source upload. The SDMA copy CPV flag now uses bit 19 from
AMD's packet header (previously bit 28); fences use Linux's uncached memory
type, and doorbell writes use the programmed dword index times four. The copy
timeout and returned duration now measure elapsed time. The fresh hardware run
completed all 15 initialization stages and copied 4096 bytes with zero source
or destination mismatches in 1155 microseconds. The subsequent CP KIQ fence
test still timed out with its target unchanged. Fan speed was reported
fluctuating and then dropping on the earlier v0.1.55 run; sustained stability
remains unresolved.

**v0.1.55 hardware initialization passed — MES now acknowledges commands.**
MES register accesses now carry AMD's GC segment indices, including segment 1
for startup, queue selection and scheduler-version reads. MES commands use
the upstream completion layout, doorbell index units and monotonic dword write
pointer, publish its memory shadow, and measure the timeout with a monotonic
clock. Resource packets include the discovered register segments and corrected
SET_HW_RESOURCES_1 offsets. The hardware run at 00:39:48 UTC completed all 15
stages: MES reported firmware version `0x0102708b` and acknowledged all three
initialization commands in approximately 1.1 ms each. Both SDMA ring-fence tests
passed. The subsequent SDMA copy failed data verification; CP execution remains
untested on this build. Successful initialization alone does not establish
copy integrity or sustained stability.

**v0.1.54 tested through the firmware checkpoint — preserve PSP firmware across uploads.**
The SOS register-list descriptor previously referenced the host's reusable
upload buffer. Replaying the actual firmware files overwrites 1067 of its 1712
bytes before REG_LIST submission. The driver now retains a private SOS package,
verifies register-list staging in VRAM, and stops on ASD/REG_LIST errors.
PSPFwLoad requires completion of that chain. A new **Load Firmware Only** button
stops at this checkpoint; **Initialize GPU** can continue without a reset.
The production parser's lifetime regression test and Xcode build pass.
The hardware run at 00:23:36 UTC verified all 1712 register-list bytes in VRAM
and PSP accepted REG_LIST (type 67, fence 26), replacing the earlier `0x11`
rejection. The user confirmed normal fans at stage 8. Continuing without a
reset at 00:24:56 UTC still failed MESInit; no engine smoke tests followed.
Whether the register-list fix resolves the reported fan failure after later
initialization remains unverified.

**v0.1.53 tested — stop on failed initialization.**
The v0.1.52 hardware run exposed MES scheduler timeouts hidden by successful
stage returns. v0.1.53 propagates required MES setup/status failures and missing
microcode, stops host firmware loading on errors, and reports failed initialization
in red. This corrects success reporting; it does not fix MES execution or the
reported full-speed fan condition. A fresh hardware run now stops at MESInit
with timeout `0xe00002d6` and displays failure before GFX/SDMA initialization.

**v0.1.52 installed — GPU and driver lifecycle fixes.**
The signed build closes PCI before releasing DMA backing, serializes driver,
client and interrupt dispatch, and drains callbacks before final cleanup.
Repeated Initialize no longer resets hardware underneath completed software
state. Closed PCI sessions require a fresh attachment before GPU operations.
Build, packet/layout tests and signature verification pass. Install/relaunch,
fresh attachment, repeated-Initialize protection, and a secondary client's
normal Stop were observed working. Full unplug/teardown acceptance is pending.

**v0.1.51 tested — firmware discovery and GFX12 command-packet fixes.**
The installed build reaches all 15 initialization stages. CP still times out,
and the latest SDMA copy failed 256/1024 dwords despite a verified source upload.
The firmware and packet fixes are validated in source/tests but do not resolve
all engine failures. Lifecycle fixes for PCI close, callback draining, shared
resource cleanup, activation, and repeated initialization are under validation.

**v0.1.50 — memory-controller corrections and ReBAR diagnostics.**
The driver attaches to the R9700 on Apple Silicon. Earlier hardware sessions
reached all 15 firmware/IP bringup stages and demonstrated SDMA ring activity.
These milestones do not yet establish reliable compute or graphics operation.

The September 22 v0.1.50 hardware run reached all 15 stages and confirmed
the corrected GART root. ReBAR reports BAR0 support up to 32 GiB, with
macOS assigning 256 MiB; BAR2 is 2 MiB and BAR5 is 512 KiB. SDMA command
submission/fences and VRAM BO allocation pass. VRAM copies still fail data
verification (184/1024 mismatched dwords, then 640/1024 after four minutes),
despite verified source uploads. CP KIQ times out; GTT remains disabled.
See [the progress review](PROGRESS.md) for evidence, changes, and remaining work.

This revision:

- Fixes the pending GART placement compile error and uses checked LOW placement.
- Preserves the GFX12 VRAM page-table root address correction and gives the
  system-aperture scratch page VRAM backing, matching the Linux address model.
- Reads VRAM capacity from hardware and limits CPU-visible allocations to
  `min(BAR0 size, VRAM size)`.
- Unifies PCI initialization for API calls and memory mapping, so call order
  cannot mistake the doorbell aperture for VRAM.
- Adds read-only ReBAR supported/selected/OS-assigned size diagnostics to
  the host's **BARs** button (selector 41).
- Keeps GTT allocations and BAR2 doorbell kicks gated independently pending
  hardware verification; the MMIO SDMA write-pointer fallback remains enabled.

Xcode 26.6 / DriverKit 25.5 provides PCI configuration access and BAR queries,
but no public PCI resource-resizing operation was found in its headers. A
ReBAR capability ID alone cannot allocate larger Thunderbolt bridge windows.
The driver does not write ReBAR size controls.

The next hardware acceptance checks are data-verified VRAM copies, then
separately tested GART host-memory reads and compute queues.
Mesa winsys integration remains unimplemented.

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

A historically successful v0.1.22 run cleared every bring-up stage;
repeat these checks after installing a changed driver:

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

The session leading to v0.1.22 is documented under
[`docs/audit-2026-05-22/`](docs/audit-2026-05-22/) — synthesis report,
per-agent forensic audits, and the two version plans
(`PLAN_v0.1.19_psp_asd_initialize.md`,
`PLAN_v0.1.20_smu_hw_setup.md`) that drove the EnableAllSmuFeatures
and BAR0-mapped-LOW-VRAM fixes.

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
- `docs/` — bringup audits and version plans. `audit-2026-05-22/`
  holds the synthesis + per-agent forensic reports that produced
  v0.1.19–v0.1.22.
- `upstream/` — vendored Linux + Mesa source for reference. Gitignored.
- `firmware/` — AMD GPU microcode (GFX11 + GFX12 families), copied
  from linux-firmware. Tracked in the repo.
