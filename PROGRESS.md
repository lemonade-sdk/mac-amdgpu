# Progress review — 2026-09-22

## Observed system and baseline

- macOS 26.6.2 (25G83), Xcode 26.6 (17F113), DriverKit SDK 25.5.
- Active IORegistry service: `MacAMDGPU`, R9700 `1002:7551`, revision C0,
  PCI `05:00.0`. A live user-client probe answered successfully.
- OS-assigned apertures: BAR0 268435456 bytes (256 MiB, memory index 0),
  BAR2 2097152 bytes (2 MiB, memory index 1), BAR5 524288 bytes
  (512 KiB, memory index 2).
- The initial live probe reported stage 0; selector 41 was unsupported.
- A subsequent user-supplied v0.1.49 log confirmed all 15 stages on this OS,
  followed by a 4096-byte VRAM copy with 144/1024 mismatched dwords.
  CP KIQ returned timeout (`0xe00002d6`) despite the old host printing OK.
  SDMA command-stream NOP/fence submission reported success.
- That log showed FB_OFFSET=0 and GART root `0x8000700001`, directly
  confirming that the old driver omitted the VRAM physical transform.
  The corrected root for this configuration is `0x00700001`.
- The initial working tree did not compile: `gmc_gart_location` was called
  before declaration. Pre-existing edits in the driver, GART, and GMC files
  were retained or completed; unrelated worktrees were left untouched.

The top-level README described v0.1.22, whereas the source was v0.1.49.
Later commits contain VRAM SDMA copy, KIQ smoke, BO, and SDMA CS paths.
Their existence is not evidence that all paths pass on this macOS release.
The `winsys/` and `vk/` directories contain no implementation.

## Apple API and ReBAR findings

The installed `PCIDriverKit/IOPCIDevice.iig` exposes `FindPCICapability`,
configuration reads/writes, and `GetBARInfo`. No public BAR-resource resize
or bridge-window allocation operation appears in that interface.
`kIOPCIExpressCapabilityIDResizableBAR` is already the negated extended
capability ID (`-0x15`); it is a discovery constant, not an enable operation.
See Apple's [IOPCIDevice](https://developer.apple.com/documentation/pcidriverkit/iopcidevice),
[GetBARInfo](https://developer.apple.com/documentation/pcidriverkit/iopcidevice/getbarinfo),
and [ReBAR ID](https://developer.apple.com/documentation/pcidriverkit/kiopciexpresscapabilityidresizablebar).

The [Xcode 26.6 notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-26_6-release-notes)
and available [Xcode 27 notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-27-release-notes)
did not identify a public ReBAR allocation API. Only the installed 25.5
DriverKit SDK was inspected locally; this is not a binary audit of an
uninstalled Xcode 27 SDK. The installed DMA specification exposes no new
sysmem-read enable flag.

Linux's local `amdgpu_device_resize_fb_bar` checks root-bus resources,
disables decoding, tears down doorbells, calls `pci_resize_resource`,
and restores mappings/decoding. Writing the endpoint's size field by
itself does not perform these host-side allocations. A larger GPU-supported
size therefore does not prove that macOS can assign it through the TB chain.
The [Linux ReBAR implementation](https://github.com/torvalds/linux/blob/master/drivers/pci/rebar.c)
and [register definitions](https://github.com/torvalds/linux/blob/master/include/uapi/linux/pci_regs.h)
provide the capability layout used by the new read-only parser.

A small BAR limits CPU-visible VRAM, not the GPU's physical memory capacity.
The practical path can use CPU-visible staging buffers and SDMA transfers
to GPU-only VRAM, but a separate allocator/transfer ABI is still needed to
use the rest of VRAM. ReBAR does not substitute for functioning GART/DMA or
compute queues.

## Changes in v0.1.50

1. Fixed compilation and extracted checked GFX12 GART LOW-placement math.
   Tests cover zero-based GART, 4 GiB alignment, invalid layouts, and VA-hole
   exhaustion. Binding reuse no longer treats GPU address zero as unallocated.
2. Preserved the pending VRAM root transform: `mc - vram_start +
   vram_base_offset`, then VALID. This follows local
   `amdgpu_gmc_pd_addr` and `gmc_v12_0_get_vm_pde`.
3. Moved system-aperture scratch into reserved VRAM after the page table
   (`0x780000`, 16 KiB), and applied the same physical address transform.
   The protection-fault dummy page remains DMA-mapped sysmem. The original
   code applied a VRAM transform to a DMA bus address; the pending edit
   removed that transform but still lacked VRAM backing for scratch.
4. Made resource initialization idempotent for a VRAM-backed table, checked
   its reserved/visible bounds, and validated dummy-page DMA/CPU mappings.
   Added calculated addresses and MMHUB register/PTE readback to GMC logs.
5. Read total VRAM from `RCC_CONFIG_MEMSIZE`, rather than assuming 32 GiB.
   Visible VRAM is `min(actual BAR0 length, physical VRAM length)`.
6. Removed the duplicate memory-mapping initialization path. Both entry
   orders use the BAR0 framebuffer, BAR2 doorbell, BAR5 register assignment.
   BAR2 now has its own size field; diagnostics no longer label BAR0's size
   as BAR2's size. The host correctly decodes GetBARInfo's memory index/type.
7. Added selector 41 and host BARs output for supported ReBAR sizes,
   selected size, and actual OS mapping. The bounded v1 parser rejects
   invalid entries, duplicate BAR indexes, truncated blocks, unsupported
   versions, and failed reads. It never changes BAR sizing.
8. Kept GTT reads and doorbell capability gates false independently.
   Removed unsupported claims that their failures share a proven DART cause.
   Added a status-only CLI that performs no engine initialization or submits.

The VRAM SDMA test now checks the staged source before submission, reports
upload mismatches separately, and offers full 64-bit addresses in its extended
reply. CP KIQ diagnostics preserve timeout results in the scalar payload;
the host requires both a successful call and the expected fence before saying OK.

## Evidence corrections and remaining blockers

The historical Apple-feedback draft misidentifies selector 34 as a sysmem
copy. The current host's SDMA Copy button allocates both source and
destination in VRAM. The old host displayed addresses truncated to 32 bits;
v0.1.50 prints the full addresses.
A completed fence with mismatched data proves neither a successful copy
nor a specific Apple IOMMU fault. The draft now carries this correction.

Hardware bringup and the corrected GART root have now been verified with
v0.1.50. VRAM copy correctness still fails; GART reads remain unverified.
The GTT gate remains off. Existing GART code still has
separate bump allocators in GMC and GART, lacks a complete reusable mapping
lifecycle/TLB-invalidation path, and cannot simply be enabled for clients.
The pre-existing serial queue was only created/released. The lifecycle revision
now binds shared driver/client dispatch to it; hardware validation is pending.
BO reclamation, validated command submission,
MES compute queue execution, and a Mesa winsys remain further work.

## Validation

- `bash scripts/test-memory-layout.sh`: passed with address and undefined
  behavior sanitizers.
- The host and extension build together using Xcode 26.6 / DriverKit 25.5.
  Existing host concurrency warnings remain under Swift 5 mode; moving the
  project to Swift 6 requires a separate concurrency cleanup.
- The signed v0.1.50 host and extension were re-signed with the documented
  minimal DriverKit entitlements and pass `codesign --verify --deep --strict`.
  The installable app is `build/review/Build/Products/Debug/MacAMDGPUHost.app`.
- The v0.1.50 app was copied to `/Applications/MacAMDGPUHost.app`.
  The host reported activation complete and installed v0.1.50 (150) enabled.
  The previous app is preserved at
  `build/review/MacAMDGPUHost-before-0.1.50.app`.
- After the user completed installation and authorized tests, v0.1.50 was
  tested live. Results and firmware readiness timing are recorded below.
  Future installation/activation remains with the user.
- The status probe compiled and queried the existing attached driver;
  observed values are listed above. This baseline does not test the new code.

## v0.1.50 hardware results — 2026-09-22 UTC

- 23:22:36–39: all 15 initialization stages succeeded.
- MMHUB readback: root `0x00700001`, GART start `0`, end `0xffff`,
  scratch register `0x780`, PTE0 `0`. Root/scratch match the intended VRAM
  walker transform; these register checks do not establish working GART reads.
- ReBAR: BAR0 supports 256 MiB through 32768 MiB; selected and OS-assigned
  size are both 256 MiB. BAR2 supports 2–256 MiB; selected/assigned 2 MiB.
  BAR5 has no ReBAR entry. No size controls were changed.
- Reported VRAM capacity: 34208743424 bytes (32624 MiB); visible 256 MiB.
- 23:22:43: 4096-byte SDMA copy completed its fence but failed readback:
  184/1024 dwords differed, first at byte `0xe0`. Source upload verification
  found zero mismatches. Source `0x800180c000`, destination `0x8001810000`.
- 23:26:44: repeat after about four minutes still failed: 640/1024 dwords,
  first at `0x40`; source upload again had zero mismatches. Source
  `0x8001818000`, destination `0x800181c000`. Extra idle time did not resolve
  the failure. Different buffers and mismatch counts do not isolate its cause.
- CP KIQ: timeout `0xe00002d6`, expected `0xdeadbeef`, observed unchanged
  poison `0xcafebabe` at `0x8001814000`. Nominal timeout 100 ms; log
  timestamps span about 119 ms. Doorbell capability remains false.
- SDMA CS create/write four NOPs/submit/wait/destroy succeeded.
- VRAM BO allocation, info round-trip, and free succeeded. GTT allocation
  returned NotReady (`0xe00002d8`), consistent with the disabled gate.
- Subsequent live status and ping remained responsive. BAR2 diagnostics now
  correctly report 2097152 bytes rather than BAR0's 268435456 bytes.

### Firmware completion waits

The driver waits for completion signals rather than using only an upload delay:

| Operation | Configured wait |
| --- | --- |
| Host after FLR | 150 ms before discovery |
| PSP bootloader readiness | Up to 10 s; 1 ms sleeps between register probes |
| SOS load | 20 ms handshake delay, then up to 5 s for changed, nonzero sign-of-life |
| PSP ring firmware command | Up to 1 s for its fence, then validate response status |
| RLC/graphics autoload | Up to 10 s for CP_STAT=0 and BOOTLOAD_COMPLETE |

These loops count requested sleep intervals, not a monotonic deadline;
MMIO and scheduling overhead can extend actual wall time. In this run SOS
reported ready at 23:22:38.720 after the 20 ms delay plus eight 1 ms polling
intervals. PSP command fences completed. At 23:22:38.963, RLC's first check
already read CP_STAT=0 and BOOTLOAD_STATUS=`0x8000003f`. The first SDMA copy
began approximately 4.8 seconds later. No firmware wait timeout was observed.

The installed host's PSP dump incorrectly labels a nonzero C2PMSG_81 with
bit 31 clear as NOT ALIVE. Both the driver's `psp_is_sos_alive` and local
Linux `psp_v14_0_is_sos_alive` use nonzero as the condition. The host source
display and selector comment are corrected after this test; that display
correction is included in the subsequently built v0.1.51, not yet installed.
The general Diagnostics IFWI
line also uses a bootstrap register alias and is not authoritative after
IP discovery; the resolved PSP dump reports C2PMSG_33=`0x80000000`.

## External discussion review and v0.1.51

Reviewed `docs/AMDGPU_OSX_QUESTIONS_EXTERNAL_OTHER_PARTY.md` against the
local Linux driver, current port, and v0.1.50 hardware results. The file mixes
questions, hypotheses, later corrections, and advice; each proposed change
must be checked against the applicable ASIC and Linux implementation.

### Changes and reasons

- **IFWI discovery readiness:** use absolute BAR5 dword address `0x16061`
  for C2PMSG_33, matching Linux `amdgpu_discovery_read_binary_from_mem`.
  The old code polled `0x63`, an IP-relative C2PMSG_35 offset without its
  MP0 base. This conflated IFWI completion with the later bootloader
  command-ready handshake and polled the wrong absolute location.
  General Diagnostics now reads the same corrected IFWI address.
  The existing two-second poll and MEMSIZE fallback remain as in Linux.
- **PM4 encoding:** fix the shared header to put opcode in bits 15:8 and
  payload-count-minus-one in 29:16, and correct NOP opcode to `0x10`.
  Correct RELEASE_MEM GL2_WB/SEQ bits to 21/22 using Linux `nvd.h`.
- **CP smoke stream:** use a shared builder for the normal EOP path and
  smoke test. Emit a two-dword NOP followed by the complete eight-dword
  RELEASE_MEM, including Linux's cache controls and destination selection.
  Previously the smoke NOP consumed the RELEASE_MEM header as its payload,
  and RELEASE_MEM itself lacked its final dword. This is a confirmed
  encoding defect, but not proof of the only CP execution failure.
- **PSP status display:** use a nonzero C2PMSG_81 value for SOS alive,
  matching the driver and Linux, instead of incorrectly requiring bit 31.
- **Regression coverage:** `tests/pm4_packet_test.cpp` compares packet
  fields directly with local Linux `nvd.h`/`soc24_enum.h`, checks a fixed
  expected command stream, parses packet boundaries, and covers both
  32-bit/no-interrupt and 64-bit/interrupt fences.

### Advice already implemented or requiring separate work

- PSP bootloader commands are followed by SOS ring commands. Current
  firmware loads already use that handoff, and the tested firmware set
  matches this card's discovered PSP 14.0.3 / GFX 12.0.1 versions.
  The document's RX 9060 / PSP 14.0.2 correction does not change this card.
- The SOS ring frame and key PSP command payloads already have explicit
  packing and size assertions. Blanket packing changes are not justified
  by the document's compiler-alignment hypothesis.
- `mes_add_hw_queue` already submits normal ADD_QUEUE to `MESPipe::Sched`.
  Linux separately routes legacy queue mapping through KIQ when uni-MES is
  enabled; a successful scheduler command alone does not prove our CP ring
  is mapped, fetchable, or correctly kicked. That remains unverified.
- Linux's [GPUVM fault decoding guide](https://docs.kernel.org/gpu/amdgpu/debugging.html#decoding-a-gpuvm-page-fault)
  identifies the useful next evidence: hub, faulting GPU address, VMID,
  client, permission bits, and read/write direction. Current failures have
  not yet been tied to a captured fault. No Apple IOMMU cause is established.
- The document's adapter-reset recovery advice matters after a CP hang.
  A new initialization/reset is needed before testing the corrected stream;
  repeated submissions into the old potentially stalled queue cannot
  validate this fix. No reset was performed during this document review.
- DCN/DML2 and shader-ISA portability concern future graphics integration;
  they do not repair the current SDMA copy failure.

### Validation and handoff

Both sanitizer-backed packet and memory-layout/ReBAR test suites pass.
The Xcode Debug host/extension build succeeds; existing Swift concurrency
warnings remain. Signed v0.1.51 (151) passes deep, strict signature verification.
The prepared app is `build/review/Build/Products/Debug/MacAMDGPUHost.app`.
At the user's request, the build was launched at 23:34:28 UTC and displays
bundled v0.1.51 alongside installed v0.1.50 with the install/relaunch button.
The user then installed the app into Applications. The existing relaunch
flow exited normally at 23:35:00 without leaving the replacement host open;
no matching crash report was found. The host was reopened from Applications
at 23:36:13. Installed host and bundled extension signatures verify correctly.
Activation failed with OSSystemExtensionErrorDomain code 4: sysextd reported
two v0.1.50 records, one `terminating_for_upgrade_via_delegate` and one
`activated_enabled`. Recovery subsequently succeeded without a system reboot:
the user reset extension registration and terminated the stale driver process.
Installer source was unchanged through v0.1.51.

## v0.1.51 hardware retest and lifecycle review

After recovery, v0.1.51 completed all 15 stages at 23:47:56 UTC. The corrected
CP packet still timed out with its fence unchanged. At 23:49:04, a 4096-byte
SDMA copy completed its fence but failed 256/1024 dwords, first at byte 0x40;
the source upload verified correctly. Packet encoding was a real defect but
did not fully explain CP execution failure. Firmware delay alone does not
explain the repeated SDMA failures.

At 23:49:14, driver Stop left PCI open; the kernel forcibly closed the opener
and disabled memory decoding/bus mastering. A new service registered at
23:49:32, while the host retained an invalid old connection. The stale process
and registry transitions do not establish a kernel deadlock or corrupted
driver binary. Terminating the identified driver processes and restarting the
GPU is the current recovery procedure; no system reboot was needed.

The lifecycle revision under validation:

- Skips activation of an already enabled identical version/build and prevents
  overlapping host install/remove requests. The copy/relaunch flow verifies a
  distinct replacement process before exiting the original host.
- Serializes shared driver operations and rejects new work during Stop.
- Closes the actual PCI opener before releasing DMA backing. Interrupt-source
  cancellation must finish before client storage and superclass Stop.
- Retains shared bringup state until all clients have finished, then releases
  PSP, GMC, IH, CP, SDMA, and MES CPU/DMA resources without touching unplugged
  hardware.
- Rejects reset of initialized/partially initialized software state. A complete
  in-place GPU reset requires engine quiescence and state reconstruction, which
  are not yet implemented; resetting hardware while retaining completed stage
  flags can otherwise incorrectly skip initialization.

The v0.1.52 (152) host/extension build succeeds and passes deep, strict signature
verification. Memory-layout/ReBAR and Linux PM4 regression tests pass. A
sanitizer-backed cancellation-barrier harness passed zero/one/128 sources,
concurrent completions, and failed-drain retention. The prepared app launched
at 23:59:55 UTC, showing bundled 152 and installed 151; installation remains
with the user.

The final revision also rejects GPU operations after an initialized session's
PCI opener closes. Merely reopening PCI could otherwise resume engines with
released client DMA mappings. Cached status remains readable; a fresh GPU
attachment is required. If both interrupt cancellation and disable/drain fail,
the driver retains backing and logs the blocked source instead of freeing live
memory. Hardware acceptance must verify normal Stop completion and final
shared-resource release, including unplug/reconnect and partial initialization.

## Hardware acceptance sequence

### v0.1.52 live results (September 23 UTC / September 22 local)

- At 00:03:14 the installed host relaunched from `/Applications` and activated
  v0.1.52. Fresh GPU attachment subsequently answered Ping.
- At 00:04:33 all stage calls returned success. At 00:04:51 repeated Initialize
  skipped reset and retained the current session, as intended.
- Those successful stage returns did not establish full initialization: kernel
  logs show MES SET_HW_RESOURCES and QUERY_SCHED_STATUS timeouts were swallowed.
  Earlier SMU power-source/feature-mask errors and PSP status values also need
  investigation. They do not yet identify the fan-failure cause.
- At 00:04:57 SDMA copied 4096 bytes with a completed fence but 160/1024 incorrect
  dwords. Source verification passed. At 00:05:16 an independent read-only BAR0
  mapping found the same 160 destination mismatches, no source mismatches and
  no remaining poison values. First bad range began at 0xe0 with zero values.
  Matching results weaken an explanation confined to MM_INDEX reads.
- The secondary readback client completed Stop without closing the primary
  client's PCI session or releasing shared resources. Both host and dext
  remained running. The user reported full-speed fans and power-cycled the GPU;
  further hardware access stopped. No particular command is established as the
  trigger, and there was no matching process crash report.

v0.1.53 returns the MES failures to the orchestrator, following the local
Linux `mes_v12_0_hw_init` checks. The host now stops on firmware-load errors
and failed later stages, returns a real initialization result, and displays
failure instead of unconditional green completion. The Xcode build and deep,
strict signature verification passed; v0.1.53 was launched at 00:09:16 UTC for
the user to install. No additional GPU probes or submissions were performed
until the user confirmed a fresh power cycle and installation.

At 00:09:54 UTC, installed v0.1.53 answered Ping. One initialization attempt
at 00:10:02 reached CPInit, then returned MESInit timeout `0xe00002d6` at
00:10:03. The host displayed initialization failure and stopped before
GFXInit/SDMAInit. This validates failure propagation, not scheduler execution.
No engine smoke tests followed that timeout; the GPU was left idle.

### v0.1.54 firmware lifetime correction

The user reported full-speed fans again after the v0.1.53 initialization stopped
at MESInit. Firmware/initialization review found an earlier concrete defect:

- `psp_parse_sos_microcode` left all sub-binary pointers in the host DMA upload
  buffer. `Host.loadFirmware` overwrites that same buffer for each later file.
- The installed SOS file's REG_LIST occupies `[0x69f00, 0x6a5b0)`, 1712 bytes.
  Replaying the real Host upload order changes 1067 of those bytes beginning
  with the ME upload. The pointer submitted later therefore no longer refers
  to the register-list bytes in the SOS package.
- Original RL SHA256: `1bbad6e895075aca5d27f60fbc148a610a256781c175ce48f33a72e7ead71d6a`.
  Reused-buffer RL SHA256: `c91b51d19b6b61f2bd712173f7ef6fad4e28f5bd495e36ec9e053ffeffc6b2be`.
- The captured PSP REG_LIST response was `0x11`. Corrupted input is a concrete
  explanation to test; that response and the fan symptom are not yet proven
  to share this cause. Prior comments calling the rejection an Apple quirk
  lacked evidence and have been removed.

The parser now copies SOS into driver-owned storage and publishes sub-bin
views only after validating the package. Replacement failure preserves the
existing package; final PSP teardown releases ownership. The register-list
upload verifies VRAM bytes against the retained source before submission.
ASD and REG_LIST failures abort like Linux `psp_load_fw`; PSPFwLoad requires
their completed chain rather than checking only SOS alive.

The new firmware-only Host path stops at stage 8. Continuing from that verified
checkpoint uses the existing session without resetting or uploading firmware
again. This allows observation before later SMU/CP/MES setup. It does not imply
that firmware-loaded engines are dormant; RLC autoload already occurred.

`test-psp-firmware-lifetime.sh` compiles the production snapshot/parser functions
with only the DriverKit allocation boundary mocked. It replays the real files,
destroys caller storage, checks retained RL, exercises malformed/truncated
packages and allocation failure, and verifies replacement ownership. It passes
ASAN/UBSAN, as do the existing memory-layout and PM4 suites. The signed Xcode
build succeeds and deep/strict signature verification passes. The prepared
v0.1.54 app was launched at 00:20:26 UTC for user installation. Hardware
validation is pending; no probes or submissions were performed during this
offline review.

Other firmware-protocol differences remain for separate investigation: the
current NotifyPowerSource parameter labels `1` as AC while Linux's firmware
enum maps AC to `0`; allowed-mask commands are sent despite SCPM being enabled,
where Linux skips them. The logged SCPM value `0x40000000` is outside the named
0/1/2 enum and needs raw database/layout verification before interpretation.
These were not changed in v0.1.54, to isolate the confirmed payload corruption.

#### v0.1.54 hardware firmware checkpoint

After user installation and GPU power-on, the installed extension reported
v0.1.54/154 activated and enabled. At 00:23:36 UTC on September 23, the Host's
Load Firmware Only path completed stage 8. Kernel logs confirm private SOS
retention (435632 bytes), successful RLC autoload and ASD acknowledgement,
verification of all 1712 retained register-list bytes in VRAM, and successful
LOAD_IP_FW type 67 at fence 26. The previous REG_LIST `0x11` rejection did not
recur. This validates the lifetime correction on hardware, but does not yet
establish the cause of the full-speed fan condition or validate MES/SDMA.
The session was left at the firmware checkpoint without later SMU/CP/MES setup
or engine smoke tests, pending the user's physical fan observation. Captured
logs: `/tmp/mac-amdgpu-154-firmware.log`.

The user subsequently confirmed normal fans at the firmware checkpoint. At
00:24:56 UTC, Initialize GPU continued the same session without reset/reupload.
SMU/IMU/RLC/CP stages returned success, then MESInit failed with `0xe00002d6`.
The first SET_HW_RSRC submission timed out from 17:24:56.050 to .074 local time
(about 24 ms despite a nominal 2-second timeout). No engine smoke tests followed.
Post-continuation fan state is not yet confirmed. Continuation logs are retained
in `/tmp/mac-amdgpu-154-continued-init.log`.

### v0.1.55 MES startup and submission corrections

Comparison with local Linux `mes_v12_0.c`, `mes_v12_api_def.h` and
`gc_12_0_0_offset.h` found these protocol defects:

- MES startup, scheduler-version, GRBM queue-select, RLC scheduler routing and
  aggregated-doorbell registers used GC segment 0 instead of segment 1.
  HQD/MQD registers correctly belong to segment 0. A typed offset/segment pair
  now preserves that distinction. For the discovered bases, CP_MES_CNTL moves
  from erroneous dword address `0x3a67` to `0xc807`.
- The chained QUERY_SCHEDULER_STATUS completion address was at byte 240,
  while AMD's ABI puts it at byte 8. Submission now waits for the chained
  query fence, then checks the original API's status, as Linux does.
- SET_HW_RESOURCES_1 added unnecessary padding after api_status, shifting
  subsequent fields by eight bytes. Its layout now matches the upstream union.
- HQD was configured with the ASIC qword slot rather than its dword index.
  The corrected index and BAR2 write now identify the same doorbell. WPTR
  is a monotonic 64-bit dword count, not a wrapped byte count, and its polled
  memory shadow is published before the doorbell write.
- Resource packets omitted all register segments beyond segment 0. The first
  five discovered segments are now supplied as in Linux. The oversubscription
  timer also follows the upstream firmware-version condition.
- Submission's timeout counted busy-loop iterations as microseconds. It now
  measures CLOCK_UPTIME_RAW and sleeps between completion polls.

The MES regression suite compiles production submission/ring functions with
mocked PCI, clock and completion boundaries. It checks packet sizes/offsets and
every MES register's offset/segment directly against the local AMD headers,
then exercises delayed success, firmware error, actual timeout accounting,
write-pointer publication and wraparound. Hardware acceptance remains pending;
these corrections do not yet establish functioning MES execution or resolve
all GPU initialization prerequisites.

MES protocol tests pass under ASAN/UBSAN, along with the firmware-lifetime,
memory-layout/ReBAR and PM4 suites. The signed Xcode build and deep/strict
signature verification pass. v0.1.55 was opened at 00:32:45 UTC for user
installation; no GPU test was run with this build. The next hardware session
requires a fresh attachment because v0.1.54 stopped during MES initialization.

#### v0.1.55 hardware initialization result

After the user completed installation/power cycling, the extension list showed
only v0.1.55/155 activated and enabled. At 00:39:44 UTC on September 23,
Load Firmware Only completed stage 8 and PSP again verified/accepted all 1712
register-list bytes. At 00:39:48 UTC, continuation without reset completed all
15 stages. MES now reports scheduler version `0x0102708b` (masked `0x708b`)
instead of zero. SET_HW_RSRC, SET_HW_RSRC_1 and QUERY_SCHEDULER_STATUS each
completed with their chained query fence and successful original API status,
after 1133, 1131 and 1136 microseconds respectively. Both SDMA instances passed
their initialization ring-fence tests. These are observed GPU acknowledgements,
not merely successful setup-function returns.

Capture: `/tmp/mac-amdgpu-155-init.log`. No SDMA copy, CP NOP or CS smoke test
has yet followed this run. Post-initialization fan observation and sustained
stability are pending; neither copy integrity nor general CP execution is
established by these initialization results.

The user described the fans as somewhat normal after initialization. One
4096-byte SDMA copy at 00:41:01 UTC completed its fence, but 128/1024 dwords
differed beginning at byte `0xe0`; the source upload had zero mismatches.
Source/destination GPU addresses were `0x800180c000` and `0x8001810000`.
No further GPU commands followed. The user then reported fans rising/falling
and subsequently dropping; this was not labeled a confirmed firmware crash.
Capture: `/tmp/mac-amdgpu-155-copy.log`.

### v0.1.56 SDMA protocol correction

The copy-header CPV helper used bit 28 instead of bit 19 defined by
`sdma_v6_0_0_pkt_open.h`, which Linux's `sdma_v7_0.c` includes for SDMA 7.0.x.
The copy packet now matches Linux's eight-dword COPY_LINEAR stream. Fences
use MTYPE=3 (uncached), matching Linux's SDMA fence emitter. BAR2 doorbell
writes now multiply the already-dword-based queue index by four, rather than
eight; the existing MMIO fallback remains until SDMA-only doorbell delivery
is validated. MES's successful BAR2 submissions disprove the earlier blanket
claim that this platform cannot deliver doorbells, but do not independently
validate SDMA routing.

The copy wait now measures CLOCK_UPTIME_RAW and returns an actual submission
duration instead of reporting zero. The regression suite compiles the
production copy routine with simulated ring/fence/clock boundaries and checks
the full emitted stream against AMD's packet macros, including 64-bit addresses,
uncached fences, invalid lengths and timeout accounting. These are confirmed
protocol corrections; neither the observed data mismatch nor fan fluctuation
has yet been conclusively attributed to them. No speculative cache-control or
fan-setting changes were included.

The SDMA and MES protocol suites, firmware-lifetime, memory-layout/ReBAR and
PM4 tests pass. The signed Xcode build and deep/strict signature verification
pass. The v0.1.56 app was opened at 00:46:48 UTC and visibly reports bundled
0.1.56/156 versus installed 0.1.55/155. Hardware testing is pending installation
and a fresh GPU attachment.

The user highlighted ARM64 portability. Remaining investigation must distinguish
GPU-defined packet/register layouts from CPU memory ordering, DMA visibility,
16 KB host pages versus GPU page sizes, alignment and Apple IOMMU semantics.
Passing native ARM64 unit tests does not establish device-memory ordering or
DMA coherency. MES now fetches its system-memory ring and writes completions,
so earlier blanket assumptions that GPU system-memory reads cannot work on this
platform must not be treated as established facts.

#### v0.1.56 hardware result

After the user reported GPU power-on, the extension list confirmed one active
v0.1.56/156 instance. Initialization at 00:47:22–23 UTC on September 23 completed
all 15 stages. MES again reported `0x0102708b` and acknowledged its three commands
in 1014, 1098 and 1127 microseconds.

One SDMA copy at 00:47:41 UTC passed: 4096 bytes, zero source-upload mismatches
and zero destination mismatches across 1024 dwords. The Host measured 1155
microseconds for submission/completion; the internal fence poll measured 1137
microseconds. Source/destination were `0x800180c000` and `0x8001810000`.
This validates copy integrity for this transfer after the packet/fence/doorbell
corrections, not sustained throughput or all memory paths.

One CP KIQ test at 00:47:58 UTC still timed out (`0xe00002d6`): expected
`0xdeadbeef`, observed initial poison `0xcafebabe`, target `0x8001814000`.
The driver logged ten PM4 dwords submitted at ring WPTR 0→10. No further GPU
submissions followed. CP queue execution remains the next unresolved engine
path. Capture: `/tmp/mac-amdgpu-156-tests.log`.

Subsequent Host log entries show a second 4096-byte copy at 00:48:17 UTC
passing with zero mismatches (1149 microseconds), another CP timeout at
00:48:23, a signaled SDMA CS smoke fence at 00:48:26, and VRAM BO allocation,
query and free succeeding at 00:48:39. GTT allocation remained unsupported
(`0xe00002d8`). These actions were observed in the user's Host log.

After the user reported fluctuating fans, Live Status at 00:50:27 issued fresh
GetRunningSmuFeaturesLow/High mailbox requests and returned `0x38fffcfb` /
`0x0488f19e`, with no mailbox errors logged. SMU was responding at that time.
SDMA0 RPTR/WPTR both read `0x90` and the engines reported idle. This is not a
whole-GPU health verdict: the existing Live Status handler discards SMU return
codes, its broad ALIVE label can be satisfied by static registers, and CP
execution still fails. PSP's nonzero sOS marker and bootloader handshake values
alone do not prove current runtime command processing.

The retained first copy was independently checked through a read-only BAR0
mapping using `macamdgpu_vram_readback 0x180c000 0x1810000 4096`: zero source
mismatches, zero destination mismatches and zero remaining poison across all
1024 dwords. This confirms the original MM_INDEX/DATA readback through another
access path, minutes after completion. The initial sandbox-denied connection
was rerun successfully with approved access. Firmware/status capture:
`/tmp/mac-amdgpu-156-health.log`.

### v0.1.57 — CP register segments, ring commits and reset-based lifecycle

Source review found CP_ME_CNTL, CP_MEC_RS64_CNTL and GRBM_GFX_CNTL incorrectly
addressed using GC segment 0 instead of segment 1. GFX constants also selected
shader/VMID registers and read user harvest masks through the wrong segment.
Both register tables now carry offset plus base index, checked against every
corresponding macro in local gc_12_0_0_offset.h. GRBM_CC_GC_SA_UNIT_DISABLE also
had the wrong offset (0x0cfd, corrected to 0x0fe9). Enable operations check halt
readback; GFX CP enable/halt waits up to one second for CP_STAT to become zero
and propagates failures rather than claiming success.

The legacy GFX RB0 ring is not a compute KIQ created by MES. Its exported
selector/function names remain ABI-compatible, but the host label now says
CP GFX Fence. The ring now follows Linux's 256-DWORD commit padding with its
special one-DWORD NOP, monotonic 64-bit WPTR, 64-bit published shadow, and
DWORD doorbell indexing. A release barrier precedes notification. Capacity
checks include padding so a stalled consumer cannot be overwritten. The fence
smoke test measures wall-clock time and logs hardware RPTR/WPTR, WB RPTR,
CP_STAT and CP_ME_CNTL on timeout. These source corrections are not yet a
hardware execution result or proof that all CP prerequisites are satisfied.

Selector 42 returns [operation status, phase] for reset-based shutdown. It
requires a sole connected client and no active BAR mapping or interrupt source.
Admission blocks new clients while shutdown proceeds. It verifies AMD endpoint
presence and FLR support, clears/verifies Bus Master Enable, waits up to one
second for PCIe Transactions Pending to clear, and invokes the DriverKit
function reset (no bridge-reset fallback). Reset success, endpoint presence,
and restored BM-off state are required before PCI Close and resource release.
The SDK controls the duration of the blocking Reset call. Failures preserve
session storage and restrict calls to status/retry. A closed previous session
can reopen exclusively for reset without enabling bus mastering. Successful
shutdown clears bringup context and invalidates BO/CS handles. DMA buffer
replacement/free is also refused while live initialization may still reference it.

Host Stop GPU runs the blocking RPC off the UI actor, reports the failure phase,
and closes its user client after success. Restart GPU then performs fresh
initialization and firmware loading. This is reset-based teardown, not firmware
suspend, enclosure power-off or guaranteed recovery of a hung PCIe link.
Binary hot replacement continues through OSSystemExtensionRequest activation;
macOS can return willCompleteAfterReboot. Stop/Restart does not itself unload
or replace a driver executable. The installer was not changed in this revision.

Validation: Xcode Debug build succeeded. Seven focused suites pass: memory
layout/ReBAR, PM4 packets, actual-firmware PSP snapshot lifetime, MES protocol,
SDMA packets, CP ring/registers, and shutdown lifecycle. New production-function
tests use ASAN/UBSAN and simulate wraparound, full/stalled rings, halt timeout,
failed register readback, absent devices, transaction drain, failed reset,
BM-disable failure, other clients/BAR/IRQ rejection and retained-buffer retry.
Existing unused-function and Swift concurrency warnings remain. Hardware CP
execution and Stop/Restart require testing after v0.1.57 installation.

The final build and deep/strict signature verification passed. At 01:05:07 UTC
on September 23 the new host was opened and visibly reported bundled
0.1.57/157 versus installed 0.1.56/156, with Stop GPU, Restart GPU and
CP GFX Fence controls. Installation is left to the user. The current installed
156 driver does not implement selector 42; new lifecycle controls require 157
to be activated first.

Acceptance sequence for this version:
1. Verify installed and bundled versions are both 0.1.57/157.
2. Initialize, verify SDMA Copy remains zero-mismatch, then run CP GFX Fence.
3. On timeout retain CP register diagnostics; do not treat initialization as
   successful CP execution.
4. Stop GPU must reach phase 6, then Initialize must rebuild all firmware/queues
   and pass the copy check without a physical power cycle. Repeat via Restart GPU.
5. Before subsequent binary replacement, Stop GPU, then Install Driver. Honor
   macOS's actual activation result; do not claim no-reboot replacement in advance.

#### v0.1.57 hardware acceptance result

On September 23, activation of 157 completed at 01:05:16 UTC. Initialization
at 01:05:53 completed all 15 stages; CP halt/unhalt readbacks now reflect the
correct register and MES acknowledged commands. SDMA Copy at 01:05:58 passed
4096 bytes with zero source/destination mismatches in 1150 microseconds.

CP GFX Fence at 01:06:20 still failed after 100167 microseconds, preserving
poison 0xcafebabe instead of 0xdeadbeef at 0x8001814000. Commit advanced WPTR
to 256 (including padding). Hardware RPTR and WB RPTR stayed zero, hardware
WPTR read 0x100, CP_STAT=0x80008000 (PFP_BUSY and CP_BUSY per Linux masks),
and CP_ME_CNTL=0x0114a000 (ME/PFP halt bits clear). This narrows the failure
to execution/fetch progress rather than a missing WPTR register update; it
does not yet identify its cause. Capture: /tmp/mac-amdgpu-157-first-tests.log.

Stop GPU at 01:07:22 completed FLR, PCI Close and resource release. The user
also confirmed shutdown worked. Initialize at 01:07:46–47 rebuilt all 15
stages, and SDMA Copy at 01:07:52 passed zero mismatches in 1149 microseconds.
The combined Restart GPU button at 01:07:57 performed another successful
shutdown and completed fresh initialization at 01:07:58. Its follow-up copy
at 01:09:41 again passed zero mismatches in 1156 microseconds. The user had
selected Low power state at 01:09:16, observed in the host log. Both restarts
used software only, with no physical power cycle. These are two successful
session-reset cycles, not a guarantee of recovery from every hardware fault
or validation of binary replacement without a reboot. GPU remains initialized.
Reset/cleanup/reinitialization capture: /tmp/mac-amdgpu-157-restart-tests.log;
that capture precedes the final 01:09:41 copy, whose result was read from the
host UI. CP fetch/firmware prerequisites remain the next unresolved work.

### v0.1.58 — release CP RS64 firmware pipes after PSP load

The v157 timeout readback CP_ME_CNTL=0x0114a000 includes PFP_PIPE0_RESET
(0x00040000) and ME_PIPE0_RESET (0x00100000). The halt bits were clear but
both active graphics firmware pipes remained in reset. Local Linux
`gfx_v12_0_hw_init` calls `gfx_v12_0_config_gfx_rs64` specifically for PSP
firmware loading after RLC autoload completes and before CP resume. This
entire step was absent from the port. It programs PFP and ME entry PCs for
two graphics pipes, MEC entry PCs for four pipes, then pulses and clears
those engines' respective reset bits. The configuration pipe counts match
that Linux function even though fewer stack payloads are loaded for GFX12.

The port now validates the v2.0 firmware header and payload bounds, retains
entry addresses by value (not pointers into the reused upload buffer), and
marks each family ready only after all its PSP payloads acknowledge. The
three bundled gc_12_0_1 CP files specify 0x0007000000003000; the programmed
PC encoding is high 0x1c000 / low 0xc00. CPInit configures these PCs while
engines are halted, selects ME/pipe explicitly and restores selection zero,
and verifies entry and reset-bit readback before continuing. Missing firmware
or failed readback is fatal to this initialization stage. No timing guess or
fixed delay substitutes for the existing RLC autoload completion check.

The CP suite now reads the actual PFP/ME/MEC files, simulates upload-buffer
reuse, validates malformed header rejection, and compares every ordered
register write with the Linux startup sequence. It starts from the observed
157 reset state and requires its reset bits to clear while preserving unrelated
control bits. Missing firmware, failed reads/writes, existing ring padding and
pointer wrap/capacity/timeout checks pass under ASAN/UBSAN. PSP firmware-lifetime
and shutdown regression suites also pass. Xcode build succeeds; hardware fence
execution is pending installation of 0.1.58/158. This fixes an observed startup
omission; successful PM4 execution must still be demonstrated on hardware.

Deep/strict signature verification passed. The existing 157 session was stopped
successfully at 01:16:30–31 UTC without powering off the enclosure, then the
158 host was opened at 01:16:36. UI confirms bundled 0.1.58/158 versus installed
0.1.57/157. Installation is left to the user before live CP testing.

### Further hardware acceptance

1. Install and activate the newly signed extension; verify its version and
   selector 41 response. ReBAR queries must leave OS-assigned BAR sizes unchanged.
2. Initialize GPU from the host and retain the complete stage log. Compare
   the new GART root/scratch register readback with calculated walker addresses.
3. Run SDMA Copy; require both a completed fence and zero mismatched dwords.
4. Separately test raw DMA and GART-mapped host-memory reads with nonzero
   source patterns and poisoned destinations. Capture PTEs, root registers,
   DMA addresses, faults, and CPU readback. Keep mappings alive through completion.
5. Enable GTT only after repeated data verification; validate BAR2 doorbells
   separately. Then complete compute queue submission and Mesa integration.

## v0.1.58 actual attachment and v0.1.59 follow-up

Activation at 01:16:43 UTC registered 158 but left the 157 executable running.
The 01:17 CP test was therefore not a test of 158. Process paths and executable
hashes confirmed this; kernelmanagerd delegated retirement of the replaced
process. Stop GPU succeeded at 01:19:41 and the host closed. After the user
terminated only the retired process, process 24420 ran the installed 158 path
and systemextensionsctl showed only 158 active/enabled. No physical GPU cycle
was needed for this handoff. Installed extension properties alone cannot prove
which executable answers a user client.

The first verified 158 run at 01:25:15–16 acknowledged PFP/ME/MEC firmware at
entry 0x0007000000003000. CPInit stopped with kIOReturnIOError: PFP pipe 0 PC
readback passed, but pipe 1 returned 0xDEADBEEF for both registers. The local
Linux gfx_v12_0_sw_init defines one graphics pipe and two MEC pipes for
12.0.0/1. Its gfx_v12_0_config_gfx_rs64 writes two/four slots without PC
readback. The 159 correction retains that write/reset sequence while limiting
PC validation to active slots. The CP regression model now reproduces unreadable
inactive graphics/MEC slots. Active-slot write failures remain fatal.
Stop GPU succeeded again at 01:27:19 following partial initialization.

159 also centralizes the build/marketing version in project.yml. Both bundle
plists and the compiled driver identity derive from those build settings.
Runtime identity is queried without opening PCI or touching GPU registers;
the host must match the responding build before starting hardware work.

Validation: the signed 159 Debug build succeeds and strict deep signature
verification passes. Host and embedded driver both report 0.1.59/159. The CP
and shutdown sanitizer suites pass. The new runtime-identity suite exercises
production response parsing and probe ownership, including malformed/unsupported
replies, mismatches, temporary-client closure and preservation of active clients;
the extracted driver selector passes ASAN/UBSAN without PCI helpers.

The 159 host opened at 01:30:59 UTC while the actual driver remained 158.
Verify Running reported the legacy driver as unverified; Initialize GPU at
01:31:30 aborted at the connection gate before reset/allocation/firmware work.
The GPU remains stopped. Installing 159, verifying its runtime reply and
retesting CP execution are pending. Automatic shutdown before activation is
implemented but the complete 158-to-159 installation path has not yet been
exercised. The app does not forcibly terminate a stuck retired process.

## v0.1.59 hardware results and v0.1.60 ordering correction

The 158-to-159 activation at 01:32:30 automatically stopped the old session
successfully. Runtime verification correctly reported a pending handoff until
the user physically cycled the GPU; it then verified build 159 at 01:33:22.
This confirms both automatic pre-upgrade shutdown and the stale-runtime guard,
but not seamless binary replacement.

The verified 159 driver initialized all 15 stages at 01:34:55–56. All three
RS64 firmware PCs passed active-pipe checks and their reset masks cleared.
CP_ME_CNTL was 0x0100a000 immediately after enable, but 0x1500a000 at the
01:35:01 fence timeout: PFP_HALT and ME_HALT were set again, with resets clear.
RPTR and writeback RPTR remained zero, WPTR was 0x100, CP_STAT was 0x80000000.
The expected 0xDEADBEEF fence remained 0xCAFEBABE after 100267 microseconds.
SDMA copied 4096 bytes at 01:35:23 with zero upload/readback mismatches in
1157 microseconds. Stop GPU succeeded at 01:40:15.

The local Linux gfx_v12_0_hw_init configures RS64, GFXHUB and constants before
cp_resume, whose MES KIQ setup precedes legacy gfx queue resume. Our stage 12
previously resumed CP, then stage 13 started MES and stage 14 first programmed
GFXHUB. 160 separates CP preparation from resume without renumbering the ABI:
stage 12 prepares firmware, GFXHUB and constants; stage 13 initializes MES;
stage 14 programs the legacy ring and clears halt. Prepared firmware cannot
be replaced without resetting the session. Additional snapshots before/after
MES and before submission will distinguish initialization from later halting.
The precise cause of the observed re-halt is not yet proven.

The startup test extracts the production preparation/resume and orchestrator
cases. It verifies ordering, no early usable queue, prerequisite failure gates
and idempotent resume under ASAN/UBSAN. It and the existing CP ring suite pass.
The signed Debug build succeeds; strict deep signature verification passes;
host and embedded driver both report 0.1.60/160. Hardware execution is pending.


## v0.1.61 — offline Linux comparison and lifecycle hardening

Verified runtime 160 failed CP preparation at the unresolved GMC slot; it did
not reach the queue test. Build 161 corrects the GC/MMHUB selection, RLC resume
order and MMHUB invalidation, hardens discovery and actual firmware deadlines,
and closes client ownership/fence/teardown gaps. Explicit deactivation now
prepares safe shutdown. The copy/relaunch installer remains unchanged.

All 14 offline suites and the host/dext Debug build pass. No activation or live
GPU test was performed while the device was detached. CP execution, full MES/KIQ,
GTT, topology parsing and sustained stability remain open. See the
[full review, firmware wait budgets and acceptance plan](docs/DRIVER_REVIEW_2026-09-22.md).


## v0.1.61 hardware result and v0.1.62 correction

At 02:48:23 UTC on September 23, runtime161 was directly verified. FLR and IFWI
readiness succeeded; MEMSIZE reported 32624 MiB. Binary and IPDS checksums passed.
The parser walked IPDS v3 / die0 / 46 IPs and logged correct GC, HDP, MMHUB, MP0,
MP1, NBIO and OSSSYS bases, then returned `invalid IP base-address array`.
No firmware upload or CP/SDMA test occurred. Log: `/tmp/mac-amdgpu-161-init.log`.

The new zero-base prohibition had no Linux counterpart: upstream advances by
`struct_size(ip, base_address, ip->num_base_address)` and preserves version
metadata even with no bases. 162 removes that prohibition, retaining span and
checksum checks. The same prior error also covered a span overflow, so the next
live parse remains necessary to establish the exact offending hardware record.
New error logging identifies die/IP/hw_id/base-count/span on any bounds failure.

The added test inserts a zero-base SDMA version record between valid GC/MMHUB
records. It failed on 161 and passes on 162, across IPDS v3/v4 and both supported
base widths. The full discovery suite (ASAN/UBSAN) and signed Debug build pass.
At 02:50:27 UTC, Stop GPU on161 verified reset, closed PCI, released resources
and closed the user client. No physical power cycle was used.

## v0.1.62 live result — discovery passes, MES completion times out

The 161-to-162 replacement completed registration, but runtime verification
continued to report 161 while macOS listed it as terminating for upgrade via
delegate. Stop GPU had completed reset, PCI close and resource release before
replacement; closing the host did not retire the old driver. A targeted TERM
attempt did not execute because sudo required a password. After the user power
cycled the enclosure, only 162 remained registered and its runtime identity was
verified at 02:57:05 UTC on September 23. This distinguishes session shutdown
from detachment of the still-present PCI device; it does not prove a deadlock.

Initialization at 02:57:09 UTC passed discovery, PSP firmware acknowledgements,
SMU, IMU, RLC autoload and CP preparation, including GFXHUB enable. MES reported
scheduler version 0x102708b, then its first SET_HW_RESOURCES submission timed out
after 2000262 microseconds with both API and chained-query completion slots zero.
This does not distinguish command fetch failure from firmware processing or
completion-write failure. MES storage still uses direct IODMACommand addresses
(ring 0x82080000), and the incomplete uni-MES KIQ path remains an audit gap.
No GFX/SDMA initialization or engine smoke test followed the failed stage.
Log: `/tmp/mac-amdgpu-162-init.log`.

At 02:57:45 UTC, Stop GPU succeeded: function reset, PCI close, session resource
release and user-client close. The enclosure remained powered. The host's
Remove Driver action already submits a SystemExtensions deactivation request
after this shutdown sequence. Apple's documented deactivation contract permits
completion after a Mac reboot; it is not an unconditional immediate unload.

## v0.1.63 — MES VRAM command and completion storage

The Linux comparison confirms MES queue fields receive BO GPU addresses, not
raw host DMA addresses. In 162, MES used IODMACommand addresses such as
0x82080000 directly, with no GART binding; the configured GART is only
[0, 0x10000000), and the framebuffer aperture starts at 0x8000000000. This is an
addressing gap, but the 162 timeout alone does not identify the failing access.

163 allocates EOP, MQD, command ring, staging-command storage, write-back page,
scheduler context, query fence and cleaner-shader fence in the existing visible
VRAM allocator. CPU descriptors are staging copies, with no DMA mapping. MQD
and newly appended ring words are uploaded and checked through BAR0 before
HDP flush and doorbell publication; completion polling reads the actual VRAM
fences. Only CPU-owned completion/WPTR slots are overwritten, preserving other
GPU write-back state. Session reset and PCI-close cleanup retain the existing
ownership boundary and reclaim the VRAM arena with the rest of GMC.

Submissions reserve both API/query frames, reject pointer overflow and partial
staging, and retain a pending latch after a timeout or publication failure.
Timeout logs include HQD ACTIVE/RPTR/WPTR/doorbell, MES control/program counter,
and GFXHUB fault status/address. The installed DriverKit PCI read/write methods
return void, so upload readback and the all-ones completion sentinel are used
instead of pretending there is a transport return code.

Linux's MES ring implementation requires doorbells (its non-doorbell path is
BUG()), so no speculative MMIO fallback was added. The missing uni-MES KIQ
initialization and firmware-mediated scheduler mapping remain a separate gap;
this change isolates memory placement before altering that sequence. CP's own
system-memory storage is also unchanged. Neither MES nor CP is declared fixed.

The production MES allocation/submission tests pass under ASAN/UBSAN, including
raw-address rejection, bounds/overflow, ring wraparound, lost writes preventing
doorbells, successful/failed completion, timeout reuse blocking, removal and
allocation rollback. Memory-layout and shutdown-lifecycle suites also pass.
Hardware validation of 163 is pending installation and direct runtime identity.

## v0.1.63 live result — MES completes; SDMA startup fence is the next blocker

Runtime build 163 was directly verified after installation and GPU reconnection.
At 03:10:30 UTC on September 23, discovery, firmware loading, SMU, IMU, RLC and
CP preparation passed. MES storage was in VRAM (ring 0x800180c000). Its
SET_HW_RESOURCES, SET_HW_RESOURCES_1 and scheduler query completed in 1152,
1140 and 1148 microseconds respectively. Each submit checked its API completion
after its chained query fence. This validates the MES bootstrap command/fence
path and doorbell delivery for this session, not arbitrary scheduled queues.

GFX queue resume passed, clearing ME/PFP halt bits. CP's ring still resides at
raw PCI DMA address 0x82058000; no CP packet execution was tested. SDMA0 and
SDMA1 ring setup passed, with VRAM rings at 0x8001830000 and 0x8001834000, but
write-back buffers still at raw PCI DMA addresses 0x82070000 and 0x82078000.
SDMA0's startup fence remained zero through its 100 ms timeout, so stage 15
failed and no copy, CS or CP smoke tests were submitted. This result does not
distinguish failed execution from a bad completion destination. The SDMA
write-back/fence address path is the next memory-placement candidate to review.
Log: `/tmp/mac-amdgpu-163-init.log`.

At 03:11:10 UTC, Stop GPU completed function reset, PCI close, session resource
release and user-client close. The enclosure remains powered and the host is
open with the GPU stopped. No source-code changes or new build were made during
this hardware test; only the recorded results were updated.

## v0.1.64 — SDMA write-back storage and completion readers

Both SDMA instances now allocate their ring and write-back page from visible
VRAM. Raw host DMA write-back addresses and their IOBuffer/IODMACommand storage
are removed. RPTR and WPTR register addresses derive from the VRAM allocation;
the byte-valued WPTR shadow is uploaded and read back before doorbell/MMIO
publication. Pre-publication allocation failure returns both VRAM spans.
Successful allocations remain owned by the GMC arena until reset/PCI close.

Startup and copy tests clear only the diagnostic fence at +0x80 and poll it
through BAR0. CS submission uses +0xC0, with the shared ClientSubmission latch
calling the SDMA VRAM reader before examining its CPU cache. Every admission
check and WaitFence path therefore reads the same completion source. Failed
reads retain pending work; UINT32_MAX is reserved for PCI read failure and is
never issued as a CS sequence. Earlier completed CS results remain latched when
the shared slot is reused. CP completion handling is unchanged.

The checked VRAM I/O helpers introduced for MES are shared in
`amdgpu_vram_io.h`; MES behavior is unchanged. SDMA's existing doorbell/MMIO
fallback remains in place. Failed startup additionally snapshots the failing
engine's hardware status and RPTR/WPTR. This build does not claim SDMA execution
or data integrity until both startup fences and a readback-verified copy pass.

Production-function tests cover both instances' allocations, Linux packet
addresses, WPTR units/publication, fence-slot isolation, copy/startup deadlines,
CS completion through the VRAM callback, stale/all-ones rejection, lost uploads
and allocation rollback. SDMA, MES, client-lifecycle and shutdown-lifecycle
suites pass under ASAN/UBSAN. Hardware validation awaits installation of 164.

### Hardware result — 2026-09-23 03:21–03:24 UTC

Runtime build 164 was directly verified before initialization. All 15 stages
completed, including three MES command acknowledgements and both SDMA startup
fences. SDMA0 copied 4096 bytes from 0x8001840000 to 0x8001844000 with zero
source-upload mismatches and zero destination mismatches across 1024 dwords.
The copy test reported 1186 microseconds; its internal fence completed after
1133 microseconds. CSCreate/WriteDwords/SubmitIB/WaitFence/CSDestroy also passed
using SDMA0 and the new VRAM completion reader. This verifies the tested copy
and submission paths, not sustained workloads or doorbell-only operation.

The subsequent CP GFX fence test timed out after 100411 microseconds:
expected 0xdeadbeef, observed unchanged 0xcafebabe at 0x8001848000. CP's WPTR
was 256 while hardware and write-back RPTR remained zero. ME_CNTL changed from
0x0100a000 before submission to 0x1500a000 at timeout, and CP_STAT became
0x80021000. CP ring/MQD/write-back storage still uses raw PCI DMA addresses
0x82058000/0x82060000/0x82068000 without corresponding GPU mappings. This is
the next addressing path to correct; the test does not establish it as the
only remaining CP issue. Log: `/tmp/mac-amdgpu-164-tests.log`.

At 03:24:45 UTC, Stop GPU reported successful function reset, PCI close,
session resource release and user-client close. The enclosure remains powered
and the app is open with the GPU stopped. No source-code changes or new build
were made during this hardware test; only the recorded results were updated.

## v0.1.65 — CP GPU addressing and completion

Linux's `amdgpu_ring_init` supplies `ring->gpu_addr` from its BO/GART path;
`gfx_v12_0_cp_gfx_resume` programs that GPU address into RB0 along with GPU
write-back addresses. Our CP path instead programmed IODMACommand addresses
without corresponding GPU mappings. The new path allocates the command ring
and write-back page from visible VRAM and retains a CPU-only staging buffer.
The unused MQD allocation is removed because the legacy RB0 path never binds it.

Ring capacity now reads the GPU's 32-bit RPTR through BAR0. Commit uploads and
verifies only newly staged words, handles wraparound and 256-dword padding,
then verifies the 64-bit WPTR shadow before doorbell/MMIO notification. It
checks aperture bounds and pointer overflow. GPU-written fence and RPTR slots
are never overwritten by a whole write-back-page upload during submission.

Diagnostic and legacy submission fences now read VRAM. ClientSubmission uses
a CP reader callback before comparing its CPU cache, preserving completion
latching and pending-work exclusion. All-ones/failed reads cannot complete a
job. The GFX smoke test verifies its poison value, frees its target after
successful completion, and retains it after a submission failure until reset.
Timeout logging includes the GFXHUB protection fault status/address.

Production-function tests cover VRAM allocation and rollback, register address
programming against Linux, ring wrap across the 32-bit pointer boundary,
failed packet and WPTR uploads without notification, GPU-owned slot preservation,
stale CPU cache rejection, completion latching, delayed success, real timeout,
and removal with submitted memory retained. CP ring/startup, PM4 packets,
client lifecycle/RPC, shutdown lifecycle and SDMA suites pass under ASAN/UBSAN.
The signed Debug build and strict signature verification pass; host and dext
report 0.1.65/165. The candidate app is open for installation. Hardware CP
execution remains unverified until runtime 165 is installed and directly checked.

### Hardware result — 2026-09-23 03:38–03:39 UTC

Runtime 165 was directly verified before testing. Initialization completed all
15 stages, including both SDMA startup fences. CP now programs ring address
0x8001800000 and write-back address 0x8001804000 in VRAM. Nevertheless, the GFX
fence timed out after 101032 microseconds: expected 0xdeadbeef, observed poison
0xcafebabe at 0x8001848000. RPTR and WB_RPTR remained zero, WPTR was 256,
CP_STAT was 0x80021000, and ME_CNTL changed from 0x0100a000 before submission
to 0x1500a000 at timeout. Correcting CP storage addresses alone did not resolve
command execution. Driver log: `/tmp/mac-amdgpu-165-tests.log`.

The new GFXHUB snapshot reported status high/low 0/0x00040b5d and address
high/low 0/0. Local Linux `gc_12_0_0_sh_mask.h` decodes the low status as
MORE_FAULTS=1, WALKER_ERROR=6, PERMISSION_FAULTS=5, MAPPING_ERROR=1, CID=5,
RW=1 and VMID=0. `gfxhub_v12_0.c` names CID 5 CPC. There was no pre-submit
fault snapshot, so this does not prove the fault originated from this GFX
submission or identify the first fault. The next investigation should isolate
its onset around initialization and submission before changing another path.

Stop GPU succeeded at 03:39:04. Reinitialization at 03:39:36–37 completed
without a physical power cycle. SDMA then copied 4096 bytes from 0x8001848000
to 0x800184c000 with zero upload/readback mismatches in 1200 microseconds,
and CS submission/WaitFence passed at 03:39:47. Final Stop GPU at 03:39:53
reported successful reset, PCI close, resource release and user-client close.
The enclosure remains powered and the app is open with the GPU stopped.

## v0.1.66 — isolate the failing CP phase

The GFX diagnostic first runs the local Linux `gfx_v12_0_ring_test_ring`
sequence: poison SCRATCH_REG0 with 0xcafedead, submit SET_UCONFIG_REG with
0xdeadbeef, then poll the register against a monotonic deadline. Only a
successful register test proceeds to the existing VRAM RELEASE_MEM fence.
This distinguishes CP fetch/dispatch from event/fence completion. The host
reports a preflight/register failure when no fence target was allocated.

CP control snapshots now include the GFXHUB fault status/address and decoded
client/VMID/RW/more-faults fields, RB base and pointers, PFP/ME instruction
pointers and CPC status. Existing checkpoints bracket MES and submission;
new checkpoints bracket HQD programming and GFX/MEC enable. These reads do not
clear the fault latch. Hardware evidence is needed to establish fault onset.

Tests compare the register offsets/base indices and packet constants against
the local Linux headers, exercise the emitted scratch packet, and verify a
scratch timeout does not allocate or submit a memory fence. Existing VRAM,
fence, startup and client-lifecycle checks pass. The signed Debug build and
strict signature verification pass; bundle versions are 0.1.66/166. The app
is open for user installation, with the prior GPU session stopped. No runtime
166 hardware test has been performed yet.

An additional read-only comparison of 90 locally defined memory-controller
field masks/shifts found `MMVM_L2_CNTL5__L2_CACHE_SMALLK_FRAGMENT_SIZE_MASK`
is 0x3f locally versus 0x1f in both Linux MMHUB and GFXHUB definitions. The
extra bit overlaps WALKER_PRIORITY_CLIENT_ID bit 5, so setting fragment size
zero changes the default 0x3fe0 to 0x3fc0. This needs a regression and a fix
in the next candidate; it is not part of the already built 166 and is not
established as the cause of the CP fault.


## v0.1.66 hardware result and v0.1.67 uni-MES bootstrap

Verified runtime 166 initialized all stages at 03:49:15 UTC on September 23.
GFXHUB fault status was zero before MES and 0x00040b5d immediately afterward
(CPC, VMID 0, write, address zero), before legacy HQD/GFX/MEC enable. The CP
scratch-register test timed out after 100043 us with 0xcafedead unchanged;
no RELEASE_MEM fence was submitted. Stop GPU completed reset and PCI close at
03:50:24. The legacy PFP/ME instruction-pointer readings are not sufficient to
infer the execution state of RS64 firmware.

Linux mes_v12_0_kiq_hw_init starts both uni-MES pipes, directly bootstraps KIQ,
configures its private resources, and uses KIQ ADD_QUEUE with queue type SCHQ
to map SCHED. Our earlier path routed RLC to pipe 1 while starting only pipe 0
and directly activating its HQD. 167 implements the two-pipe bootstrap with
separate VRAM contexts/fences and distinct qword doorbell slots. Aggregated
doorbells now follow scheduler resource setup. Each API error stops the chain;
submitted allocations remain alive until the existing reset/PCI-close cleanup.

The MQD header was incorrectly written at dword 80 rather than 0; the pipeline
statistics/thread-mask fields also used incorrect offsets. The descriptor now
matches v12_compute_mqd and includes CP_MQD_CONTROL 0x100 and IB_CONTROL
0x00300000. A production-code test compares the full MQD and MAP_QUEUE frame
against Linux structures, verifies both pipe enables and KIQ-only direct HQD
activation, checks private resource addresses, and injects failures at all six
bootstrap messages and at MQD upload. Firmware readiness is published only after
PSP acknowledges the scheduler/KIQ code and data payloads.

The local SMALLK_FRAGMENT_SIZE mask included bit 5, which belongs to walker
priority. Correcting 0x3f to Linux's 0x1f keeps both hubs' L2_CNTL5 at 0x3fe0
instead of 0x3fc0. The cache regression first failed against the old mask and
passes with the corrected definitions, including checks against Linux fields.
Hardware acceptance of 167, fault clearance, and CP execution remain pending.

167 validation: MES startup/protocol, GMC cache/flush, CP startup/ring, shutdown,
firmware waits, client lifecycle, runtime identity, and SDMA suites pass under
their existing sanitizer/test configurations. The signed Debug build and strict
signature verification pass; both host and dext report bundle build 167. The
candidate app is open for user installation. The installed driver remains 166,
and its GPU session was safely stopped before opening the candidate.


## v0.1.67 hardware results and v0.1.68 GFX queue mapping

Runtime 167 was verified directly at 04:04:14 UTC on September 23. Initialization
completed at 04:04:21. Both MES pipes run; KIQ resources, KIQ's SCHED mapping,
scheduler resources, and query all acknowledge in about 1.1 ms. Every new MES
checkpoint and the post-GFX/MEC snapshots report zero GFXHUB faults. This resolves
the reproducible MES-startup CPC write fault seen on 166.

At 04:05:06, the CP scratch test still timed out (101110 us, 0xcafedead unchanged,
no memory-fence submission). This time fault status becomes 0x00000d3c only after
submission: CPG client 6, VMID 0, read, address zero. ME/PFP halt again and RPTR
remains zero. Software Stop GPU succeeds at 04:05:27. No further engine submissions
were made in that faulted session. Logs: /tmp/mac-amdgpu-167-init.log.

Linux defaults amdgpu_async_gfx_ring to 1 and maps its kernel graphics queues
through KIQ. 168 follows gfx_v12_0_cp_resume / cp_async_gfx_ring_resume instead
of the legacy direct CP_RB0 setup. MEC/GFX enable precedes the MES stage; a
verified VRAM v12_gfx_mqd is then mapped by a KIQ ADD_QUEUE (GFX, map_legacy_kq).
The driver marks the ring ready only after acknowledgement. The MQD remains
reserved until successful reset/PCI close, including mapping timeouts, and
mapping cannot be replayed on that allocation. Kernel shadow/CSA addresses stay
zero, matching amdgpu_ring_to_mqd_prop. General user queues remain disabled.

CP commits use only the mapped queue's doorbell; the prior duplicate legacy
MMIO WPTR write is removed. SDMA behavior is unchanged. Diagnostics now include
the actual GFX HQD base/active/mapped/pointers and RS64 instruction pointers.
The standalone MQD test compiles Linux's actual initializer as a reference and
compares all 2048 bytes across ring sizes, high addresses and doorbell indices.
Production-path tests cover mapping requests, failed uploads/maps with retained
storage, no replay, no legacy WPTR writes, and ordered startup/failure gates.
The additional MES firmware test proves neither pipe becomes ready before all
four payload acknowledgements, including failed replacement and missing payloads.
Hardware acceptance of 168 remains pending.

168 validation: CP startup/ring, MES startup/protocol/firmware gate, GFX MQD,
shutdown, client lifecycle, and SDMA suites pass. The signed Debug build and
strict signature verification pass; host and dext bundle versions both read
168. The candidate is open for user installation. The installed driver is
still 167; its GPU session remains stopped. No installer code changed.


## v0.1.68 CP execution confirmed; v0.1.69 ring-wrap accounting

Runtime 168 was verified directly and initialized at 04:14:58–59 UTC on
September 23. CP's scratch-register packet and poisoned VRAM RELEASE_MEM fence
both passed at 04:15:03 (fence 0xdeadbeef, 1871 us). Eight consecutive diagnostic
runs passed, with fence durations 1863–1922 us. SDMA copied 4096 bytes with zero
mismatches (1187 us), and CS/WaitFence passed between CP runs. GFXHUB fault
registers remained zero; ME/PFP stayed unhalted and RS64 PCs were nonzero.

The ninth diagnostic at 04:15:40 returned kIOReturnNoSpace before publishing a
packet. The snapshot showed hardware RPTR=0 and monotonic WPTR=0x1000, precisely
one 4096-dword ring revolution. Our raw subtraction interpreted that empty ring
as full. 169 masks the difference by ring_ptr_mask in both append and commit,
and independently bounds unpublished staging. The one-slot reservation still
prevents full/empty ambiguity. A new production-path regression fails on 168
at this exact state, passes 80 completion cycles across repeated wraps after
the correction, fills the ring without simulated GPU progress, rejects the next
fetch block without mutation, and resumes only after consumption.

Software Restart GPU completed reset, reopened PCI, and reloaded firmware at
04:16:35–36. CP passed again at 04:16:56 (1875 us), followed by a correct 4096-byte
SDMA copy at 04:17:01 (1191 us). Final Stop GPU completed at 04:17:06. No physical
power cycle was used for this restart test. The host log also records a user
Low power selection at 04:16:07, before restart. Primary logs are
/tmp/mac-amdgpu-168-tests.log and /tmp/mac-amdgpu-168-restart.log.

This proves basic CP command execution and memory completion, not general
compute dispatch, long-running stability, host-memory GART access, or display/
Metal integration. Hardware verification beyond the first ring revolution is
pending on 169.

169 validation: CP ring and startup suites pass, as do the signed Debug build
and strict signature verification. Host and dext bundle versions both read 169.
At 04:35:50 UTC the candidate was opened for user installation; its window
reports bundled 169 and installed 168. The previous GPU session is stopped.
No installer code changed. Repeated hardware submissions across ring wraps
remain the next acceptance check after installation and runtime verification.

## v0.1.69 hardware wrap validation; v0.1.70 GFX CS submission

Runtime 169 was verified directly at 04:45:04 UTC on September 23 and initialized
at 04:45:11–12. Twenty-five consecutive CP scratch-register and poisoned-memory
fence tests pass from 04:45:15 through 04:46:13, with fence times 1771–1934 us.
The final WPTR is 12800 (three complete 4096-dword revolutions plus 512 words).
GFXHUB fault snapshots remain zero, including after the third wrap. At 04:46:17
SDMA copies 4096 bytes with zero source-upload/readback mismatches (1211 us).
SDMA CS/WaitFence passes at 04:46:25. The host records a user Low power selection
at 04:46:00 during the test sequence. Stop GPU succeeds at 04:47:55, resetting the
function, closing PCI and releasing resources. Log: /tmp/mac-amdgpu-169-tests.log.

170 enables the GFX branch of the CS handle API using the existing mapped
kernel graphics queue. It appends the caller's PM4 and a unique RELEASE_MEM
fence, then publishes the ring once. WaitFence reads GPU VRAM through the
existing completion tracker, latches completion independently of SDMA, and
retains older completed results when the shared write-back slot is reused.
The existing owner/admission rule still permits only one outstanding raw
submission and blocks resource recycling while pending. Failed append, fence
emission or publication retains ownership until completion or verified reset.
This is a trusted developer API, not isolated user queues or compute dispatch.

The host adds GFX CS Smoke alongside the existing SDMA smoke test. It sends two
complete PACKET3 NOPs through CSWriteDwords and checks the appended GPU fence
through the public WaitFence call. The RPC test executes the production SubmitIB
and WaitFence branches, checking the payload, stale/unavailable read rejection,
latched completion across reuse, cross-engine completion isolation, failures,
invalid instance, unready queue and exhausted fence counter.

Client lifecycle/RPC and CP ring/startup suites pass. The separate build/gfx-cs
Debug build and strict nested signature verification pass; both bundles report
170. The 169 build remains preserved in build/review. Hardware acceptance of
GFX CS on 170 is pending. No installer code changed.

## v0.1.70 hardware API acceptance

Runtime 170 was verified directly at 04:49:41 UTC on September 23, initialized
at 04:49:45–46, and passed GFX CSCreate/CSWriteDwords/SubmitIB/WaitFence/CSDestroy
at 04:49:50. Seventeen more consecutive GFX submissions passed through 04:50:16,
crossing the 4096-dword ring boundary. SDMA CS passed at 04:50:21, followed by
another GFX CS at 04:50:25: nineteen GFX submissions before restart. SDMA copied
4096 bytes with zero upload/readback mismatches at 04:50:29 (1194 us), and CP's
scratch/poisoned-memory fence diagnostic passed at 04:50:37 (1884 us).

Software Restart GPU completed reset, PCI reopen, firmware reload and all stages
at 04:50:42–43. A fresh GFX CS passed at 04:50:54, with its new session fence
counter starting at 1. The captured driver log contains CP fence emissions
1 through 19 followed by 1 after restart, and fifteen zero GFXHUB fault snapshots.
Final Stop GPU succeeded at 04:51:42. The host records user Low power selection
at 04:51:07. Log: /tmp/mac-amdgpu-170-tests.log. No physical power cycle occurred
during the software restart test. Application PM4 NOP/fence submission is now
demonstrated; shader execution, compute queues, host-memory transfer and display
integration remain unproven.

## v0.1.71 shared GART aperture allocation

The GMC firmware binder advanced gmc.gart_bump_offset, but gart_init created an
independent nextFreeOffset at zero for GTT bindings into the same page table.
The GART facade now references GMC's sole cursor. Both existing-buffer and
owned-buffer bindings advance that cursor, and repeated gart_init does not
rewind it. The context lives inside the same stable BringupContext as GMC;
the full session reset clears both contexts together.

The new test-gart-binding.sh extracts the actual GART init/bind functions and
GMC binder, interleaves all three binding paths, checks every PTE for four
16 KiB buffers, reinitializes the facade, and verifies aperture exhaustion.
Against the prior independent-cursor implementation, it fails because the GTT
mapping reuses address zero instead of the next 16 KiB slot. It passes with
the shared cursor. CP startup tests and the separate build/memory Debug build
pass; the app and dext are signed and strict nested signature verification
passes. The source version is 171; the installed runtime remains 170.

GTT remains disabled. This fixes overlapping allocation, not the still-needed
unbind/invalidation/reclamation, complete binding error handling, or hardware
host-memory read/write validation. GART/GTT comments and refusal messages no
longer attribute unverified read failures conclusively to DART. No installer
code changed.

## v0.1.72 checked GART lifetime and host-memory transfer probe

The live 171 baseline was verified at 05:12:19 UTC on September 23, initialized
at 05:12:26, passed GFX CS at 05:12:31, and copied 4096 bytes with SDMA at
05:12:36 (1192 us, zero source-upload/readback mismatches). Stop GPU succeeded
at 05:12:41. This checks the established engine paths after the shared-cursor
change; 171 still has no host-memory hardware test.

172 checks GART geometry, aperture/table bounds, whole GPU pages, allocation
rounding and the 48-bit physical-address limit of GFX12 PTEs. The owned-buffer
binder requests a 48-bit DMA mapping and checks SetLength, CPU range, DMA
segment count and coverage. It records ownership before publishing PTEs,
verifies each write, flushes HDP, then requires MMHUB and GFXHUB TLB ACKs.
An identical acknowledged external binding is idempotent; replacement or reuse
of a failed binding is rejected until unbound or reset.

Live unbind now clears VALID and SYSTEM by zeroing/verifying the PTEs, flushes
both hubs, and only then completes DMA and releases the buffer. A failed clear,
TLB ACK or CompleteDMA retains ownership and reservation. The BO entry stores
its full binding, including failed allocation state, so Stop can release it
after verified reset/PCI isolation. Successful trailing frees reclaim the
shared cursor; arbitrary non-trailing holes still wait for full reset. The
older GMC firmware-only mapping helper is not used by the new diagnostic and
still requires a separate lifetime/API review before re-enabling that path.

Host Memory Copy (selector 44) allocates 32 KiB of mapped host memory and a
16 KiB VRAM destination. DriverKit IODMACommand::PerformOperation writes a
changing pattern into the first host region and zeros the second. SDMA copies
host to VRAM; every word is read through BAR0 and compared. A distinct pattern
is then uploaded to VRAM and copied to the second host region, read through
PerformOperation and compared in full. Eight 4 KiB PTEs are exercised across
the two 16 KiB host regions. The final phase unbinds and releases storage.
Any started-test failure retains storage in BringupContext, rejects replay,
and requires Stop GPU. Success does not automatically enable general GTT BOs.

The GART test compiles production bind/unbind and transfer functions with
simulated DMA, page translation and VRAM. It covers both patterns and all
pages, whole-buffer comparisons, stale/corrupt data, each copy/API failure,
invalid ranges, shared allocation, TLB/PTE/CompleteDMA failure retention,
cleanup ordering, and reset-only release without MMIO. Client RPC tests cover
preserved failure diagnostics and recovery admission. GART/transfer, client
lifecycle/RPC, shutdown, GMC flush and CP startup suites pass. The separate
build/memory-test Debug build, signing and strict nested verification pass;
both host and dext report 172. Hardware transfer acceptance remains pending.
No installer code changed.

## v0.1.72 hardware host-memory acceptance

Runtime 172 was verified directly at 05:14:52 UTC on September 23 and initialized
at 05:17:53–54. Host Memory Copy passed at 05:18:10, 05:18:53, 05:18:54 and
05:18:55: each run checked distinct 16 KiB patterns in both directions, reported
zero mismatches and completed unbind/DMA release. Each reused host GPU address
zero and VRAM GPU address 0x8001878000 with a different seed. The user selected
Low power at 05:18:33. GFX CS passed at 05:18:43; CP register-write and memory
fence passed at 05:19:06 (1871 us). The 4096-byte VRAM SDMA copy passed at
05:19:14 (1188 us, zero source-upload/readback mismatches).

Software Restart GPU reset the function, closed PCI and released the session
at 05:19:23–24, then reloaded firmware and completed initialization at 05:19:25.
Host Memory Copy passed again at 05:19:37, followed by GFX CS at 05:19:48.
No enclosure power cycle was performed during this test. The driver capture
contains five successful stage-8 transfer results and 23 zero GFXHUB fault
snapshots. Log: /tmp/mac-amdgpu-172-tests.log. The app and GPU were left online.

This establishes data-verified GPU reads and writes through the prepared-DMA
GART path on this Mac, including mapping reuse and software session restart.
It does not establish application CPU mapping coherence, arbitrary GTT BO
lifetimes, shader execution or long-running stability. General GTT stays
disabled pending allocator/lifetime work; no driver or installer code changed
during this hardware acceptance run.

## v0.1.73 reusable GART aperture reservations

Replaced the shared bump cursor with GMC-owned, bounded first-fit reservations.
Successful unbind reclaims any range after verified PTE clearing, both hub TLB
acknowledgements and DMA completion. Failed publication or teardown retains its
reservation. Monotonic IDs plus exact range checks reject stale unbinds after
address reuse. Reinitializing the facade preserves existing allocations and
rejects changed aperture geometry. The unused legacy firmware binding helper
shares the allocator and pins its reservations until full session reset; its
separate DMA ownership/publication review remains outstanding.

Owned bindings now align the absolute GPU address, as well as requesting aligned
host storage. A preceding 4 KiB external binding can no longer misalign a 16 KiB
GTT BO. The allocator checks bounds, integer overflow, metadata exhaustion and
ID exhaustion without recycling live or stale ownership IDs. Its 128-record
limit is explicit; it returns no-space when the table is full.

The production-path GART tests cover non-tail reuse, preserved firmware PTEs,
stale unbind rejection without MMIO, and mixed 4/16 KiB alignment. An independent
page occupancy model checks 10,000 mixed allocation/free operations, first-fit
placement, absolute alignment, overlap, bounds, ID reuse and exhaustion. All
19 scripts/test-*.sh suites pass, including existing failure retention and
two-way transfer tests. The build/gart-allocator Debug app and dext both report
173, and strict nested signature verification passes. Hardware acceptance of
173 is pending. No installer behavior changed in this revision.

The requested product scope is AI compute and model inference. Display output
and desktop graphics are not required for that target. HRX integration is now
being investigated; no claim of working shader dispatch or inference is made.

## v0.1.73 hardware acceptance and initial HSA discovery

The responding runtime was verified as 173 at 05:30:00 UTC on September 23.
Initialization completed at 05:30:19–20; the two-way 16 KiB Host Memory Copy
passed at 05:30:30 with zero mismatches and successful unbind. GFX CS passed
at 05:31:02. These runs exercise the new allocator's normal mapping/teardown
path; non-trailing reuse and stale reservation rejection are validated offline.
The user selected Low power at 05:30:52.

The new hsa/ library implements a core HSA C ABI discovery/lifecycle foundation
using the public header revision pinned by reviewed HRX System commit
437e789eaea207a036c197cf3398a6ca473d6534. It opens observer clients and calls only
RuntimeBuild and QueryInfo. It does not reset, claim or submit to the device.
Runtime references, agent lifetime, reentrant callbacks, live query failures,
initialization failure cleanup and parallel reference users are covered by an
address/undefined-sanitized test. A C program links the built dylib to verify
the public ABI and inspect the installed driver.

The live probe discovered gfx1201, driver 173, bringup stage 15, visible VRAM
268435456 bytes and reported total VRAM 34208743424 bytes. It returned success
and closed its observer connection. GFX CS then passed at 05:40:10, demonstrating
that observer shutdown did not end the app-owned GPU session in this run.

HRX's dynamic table currently requires 121 symbols. The library exports 8 of
those; the symbol audit reports 113 missing and exits nonzero. HSA queue creation
is explicitly rejected, dispatch feature flags remain zero and no extensions
are advertised. This is not HSA conformance, an HRX-loadable backend or shader
execution. General memory allocation/copy, compute dispatch, signals, AQL,
executable/code-object handling and AMD loader tables remain required for the
requested HRX/LSE inference target. The user intends compute-only operation.


## v0.1.74 fixed compute shader diagnostic

Added Compute Smoke (selector 45), a fixed gfx1201 wave32 load/add/store shader
submitted through the existing kernel GFX queue. This is a compute instruction
execution test, not an AQL queue, executable loader or general dispatch API.
The host supplies a changing seed; 32 output words must equal input plus seed.
Input words and the rest of the 4 KiB data area must remain unchanged. Output
starts as the bitwise complement of the expected result, so an EOP fence alone
cannot pass the test. The function waits up to 100 ms for a unique CP fence.

The packet setup follows Mesa's GFX12 compute preamble and direct dispatch
sequence, including CU harvest masks, three user SGPRs, wave32 and no LDS or
scratch. Legacy MEM_ORDERED/high flags remain clear as in radeonsi's GFX12
setup. Linux's full ACQUIRE_MEM cache sequence precedes the shader; a compute
partial flush and another cache flush precede the final EOP fence. The shader
uses explicit GFX12 load/store waits. An offline LLVM assembler check verifies
the embedded instruction bytes against the assembly source.

A retained 16 KiB VRAM allocation owns both code and data. Any started-test
failure blocks normal operations and retains storage until the existing reset
path destroys the session arena. Success releases storage only after fence
completion and complete data/guard verification. Tests cover packet fields
against Linux definitions, ordering, shader bytes, bad address/architecture
preflight, poisoned output, input/guard corruption, upload/submission failures,
timeout, replay rejection and RPC diagnostic preservation.

All 21 scripts/test-*.sh suites pass. The signed Debug host and extension are
174 / 0.1.74; the build is in build/compute-smoke and strict signature validation
passes. Hardware acceptance remains pending. Installer behavior is unchanged.

Local LM Studio contains a Qwen3.8-27B-MLX-6bit checkpoint (group size 64,
affine, about 21.21 GiB of weights), suitable as a later LSE acceptance target
based on LSE's dense Qwen family and 6-bit group-affine source support. This is
format compatibility only: device allocation for the full model, runtime and
context memory, kernel execution and inference correctness remain unverified.


## v0.1.74 first compute attempt and recovery

Build 174 was verified through the responding driver at 06:00:59 UTC on
September 23. Initialization passed at 06:01:08. Compute Smoke at 06:01:17 timed
out after 100 ms waiting for EOP fence 1 (observed 0), status 0xe00002d6 at stage 3.
No output readback ran; zero reported mismatches was not a correctness result.
Storage remained retained and subsequent normal operations were blocked.
Log: /tmp/mac-amdgpu-174-compute.log.

Stop GPU completed a function reset, PCI close and session release at 06:01:47.
Reinitialization passed at 06:02:23, followed by GFX CS at 06:02:30. No enclosure
power cycle was needed for this recovery. Shader execution remains unverified.

## v0.1.75 compute checkpoints and native monitor foundation

The fixed test now verifies three submissions separately: initial cache
preparation, compute register setup, and shader execution/flush. Each has a
100 ms fence deadline. A failed phase logs the existing CP controls, queue state
and GFXHUB fault snapshot. The host distinguishes unperformed readback from a
zero-mismatch comparison. Tests inject failure at each phase and continue to
cover retained storage, poisoning and guard corruption. Compute and client
lifecycle tests pass, as do the signed Debug build and strict signatures.
Hardware acceptance of 175 is pending; installer behavior is unchanged.

Added standalone amdgpu_mtop, a macOS terminal dashboard organized like
amdgpu_top with per-attachment registry identity selection, n/p switching,
list/JSON/watch modes and observer-only IOKit transport. Its first live JSON
snapshot found registry 0x100339c27, build 174, stage 15, total VRAM 34208743424
and CPU-visible VRAM 268435456 bytes. Dynamic statistics are explicitly null
until firmware collection is integrated. Offline selection and SMU decoder
checks pass; multiple physical GPUs have not been tested. The decoder is
version-gated to SMU 14.0.3 interface 0x2e and checked against local Linux table
layout definitions; passing these tests does not verify firmware telemetry.


## v0.1.75 hardware checkpoints and recovery

Verified responding build 175 at 06:12:36 UTC on September 23; initialization
completed at 06:13:44. Compute Smoke at 06:13:59 completed the cache and register
phases (fences 1 and 2), then timed out waiting for shader fence 3. GFXHUB fault
0x00301281 reported CID 9, VMID 3, read access at page 0x08001878, corresponding
to shader code address 0x8001878000. The GFX HQD itself reported VMID 0. Data at
0x8001879000 was not checked. Log: /tmp/mac-amdgpu-175-compute.log.

Stop GPU completed at 06:14:44 with FLR, PCI close and session release. No
enclosure power cycle was needed. The fault suggests application VMID selection
was missing from direct ring submission; it does not establish that all other
shader setup is correct.

## v0.1.76 explicit IB context and bounded telemetry

The fixed compute test now submits each phase through a GFX INDIRECT_BUFFER
packet carrying VMID 0, matching Linux gfx_v12_0_ring_emit_ib_gfx. Queue-fetch
VMID alone did not select the observed shader memory context. Each IB has a
separate retained VRAM slot, 32-byte alignment, eight-dword length padding and
verified upload before publication. GFX packets omit the compute-ring-only
VALID bit. Offline packet and compute tests check the actual IB contents,
VMID, padding, data guards, upload failure and retained storage. Hardware
acceptance of the change remains pending.

SMU setup retains the existing 64 KiB firmware-table staging reservation before
programming its address. Owner selector 46 performs one bounded table-5
transfer and readback after firmware acknowledgement. Observer selector 47
returns a 192-byte cached snapshot with per-field validity, monotonic timestamps,
generation and error state. Collection failure disables retries until reset;
failed or older-than-2.5-second samples remain unavailable. The monitor never
issues table transfers. No telemetry timer is enabled in this build.


Build 176 passed all 24 scripts/test-*.sh regression suites, the signed Debug
build and independent strict signature verification for host and extension.
The candidate app opened at 06:28:39 UTC on September 23 and reports bundled
176, installed 175, awaiting installation. No installer implementation changed.

The HSA integration target is LSE's pinned HRX revision
5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c (119 required symbols, 8 currently
exported), rather than the separately reviewed newer main revision. The chosen
approach is our own focused HSA-compatible interface backed by DriverKit,
using Linux KFD and ROCr as references. No full ROCr port is being integrated.


## v0.1.76 hardware compute acceptance

Responding build 176 was verified at 06:30:39 UTC on September 23. Initialization
completed at 06:31:02. The user selected Low power at 06:31:04. Compute Smoke
passed at 06:31:40 (seed 0xaf40545f, fence 3) and 06:32:04 (seed 0x1ba92b2c,
fence 6). Both reached complete with zero mismatches across all 32 outputs,
unchanged inputs and all surrounding guard words. Both used data VA
0x8001879000 and released their allocation, verifying sequential reuse.
The first run's three fences completed in about 1.14 ms each. Log:
/tmp/mac-amdgpu-176-hardware.log. This establishes fixed shader execution;
it does not establish HSA AQL queues, arbitrary kernels or inference.

Sample Metrics returned Unsupported at 06:31:26, before any table transfer.
SMU setup reports interface 0x33; the verified Linux table schema is 0x2e.
The monitor now labels this unsupported_firmware_interface and reports both
versions. No verified 0x33 schema was found in the reviewed AMD/Linux sources.
The mismatch is not evidence of a firmware crash.

## v0.1.77 GPU-only model buffers

Added a separate GPU-only VRAM allocator above the CPU-visible BAR0 window,
limited by the firmware-reported usable VRAM size and retaining a final 1 MiB
reservation. The existing visible allocator continues to own bootstrap and
staging storage above the low 24 MiB firmware reservation. BO domain 3 selects
the GPU-only pool; all releases return storage to its original allocator.

Owner selector 48 copies between owned VRAM BO subranges through SDMA, capped
at 4 MiB and a 100 ms completion wait. Overlapping and overflowing ranges are
rejected. A failure after ring publication blocks mutation and retains backing
until reset. Selectors 49/50 upload/read at most 4 KiB of dword-aligned data in
owned visible staging BOs. They never address GPU-only memory through BAR0.
The public interface exposes no raw CPU pointer for these buffers.

The host's Large VRAM Test allocates 22 GiB plus two visible staging BOs and
checks round-trip transfers at offsets 0, 1 GiB and 22 GiB minus 4 KiB using
changing data and poisoned destination words. This samples addressability and
copy correctness; it does not touch every byte or verify model execution.
All 25 regression suites and the Debug build pass. Hardware acceptance remains
pending. Installer behavior is unchanged.


The live amdgpu_mtop observer reported build 176/stage15 and explicitly identified
firmware IF0x33 versus verified0x2e, with dynamic values unavailable. Stop GPU
then succeeded at 06:40:20 UTC: FLR, PCI close and all session resources released.

Version 177 host and extension passed independent strict signature validation.
The signed candidate opened at 06:41:15 UTC with bundled177/installed176;
installation and hardware validation remain pending.


## v0.1.77 hardware buffer acceptance

Responding build 177 verified at 06:42:40 UTC on September 23. Initialization
completed at 06:42:49. Large VRAM Test at 06:42:58 allocated two visible staging
BOs at 0x8001878000 and 0x800187c000, and one 23622320128-byte (22 GiB)
GPU-only BO at 0x8010000000. Verified 4096-byte upload/copy/download round-trips
at device-BO offsets 0, 0x40000000 and 0x57ffff000, then released all three BOs.
The last region's GPU address is 0x858ffff000, well above BAR0's visible range.
Only those sampled regions were touched; the full allocation was not filled.

Compute Smoke afterward at 06:43:27 passed all 32 results, inputs and guards,
seed 0x368ac04f, fence 3, data address 0x8001879000. The shader reused the
released visible staging range successfully. The session remains initialized.
Driver log: /tmp/mac-amdgpu-177-hardware.log.

These results establish owned-buffer staging, SDMA access beyond BAR0 and
continued fixed shader execution. They do not establish general kernel
dispatch, HSA signals/AQL queues, code-object loading or HRX/LSE inference.


## Build 178: native dispatch and live allocation accounting

Added owner-only selector 51 with versioned code-BO/range validation,
workgroup dimensions, user SGPRs and bounded fence waits. General launches
reuse the verified VMID0 GFX indirect-buffer path. Failed staged submissions
retain their IB and all owner storage until reset. Added a host dispatch test
with independently assembled code and explicit kernargs, plus packet, RPC and
allocation-lifetime regressions. All 27 suites and the signed Debug build pass.

Build 178 was installed and runtime-verified at 2026-09-23 07:00:12 UTC.
Initialization completed at 07:00:48. The first four-workgroup dispatch at
07:01:08 completed fence 1 (1136 microseconds polling interval reported), but
readback had 128 mismatched words. This is not a successful data test. Two
16 KiB BOs were retained by the host for recovery; the completed IB was freed.
Compiling the equivalent OpenCL kernel with LLVM 21.1.8 showed gfx1201 uses
ttmp9 for workgroup X, while the handwritten shader incorrectly read s2.
The compiler also emits instruction-delay handling missing from that shader.
The follow-up replaces the test sequence with compiler-verified instructions.

amdgpu_mtop's new observer QueryInfo tag 5 successfully reported real allocator
accounting: usable VRAM 34,208,743,424 bytes; visible pool capacity 243,269,632;
GPU-only pool 33,939,259,392; excluded fixed reservations/gaps 26,214,400.
Visible usage rose from 491,520 bytes / 24 allocations after initialization to
524,288 bytes / 26 allocations after the retained test buffers. GPU-only usage
remained zero. Firmware interface 0x33 remains unsupported by the 0x2e metrics
decoder; no utilization, frequency, power or fan values are fabricated.


## Build 179: compiler-verified gfx1201 test kernel

The native dispatch test kernel now matches LLVM 21.1.8 OpenCL output exactly.
The regression compiles the source independently, obtains the `vector_add`
symbol offset/size, and compares that code against both the assembly fixture
and bytes uploaded by the host. This catches architecture-specific ABI and
instruction scheduling changes that merely assembling handwritten code cannot.
The code uses ttmp9 for workgroup X and preserves the compiler's s_delay_alu /
s_wait_alu instructions. The host reports fence, seed, first bad byte and
expected/observed/initial words on a mismatch. Selector 51 and the driver-side
queue/resource protocol are unchanged; retesting on hardware is required.


## HSA CPU signal family

Implemented 41 signal entry points together: core create/destroy; all required
load/store/silent-store, add/subtract/bitwise, exchange/CAS and wait variants;
AMD create and wait-any/wait-all. Forty are additional symbols in LSE's pinned
HRX dynamic table, moving the export audit from 8/119 to 48/119 (71 missing).
This is export coverage, not 48 fully integrated GPU functions. Core queue
creation still rejects requests; signal creation with GPU consumers or implicit
all-agent consumers while a GPU exists fails until shared signal mapping is
implemented. CPU-only consumers are supported with real atomics and waits.

ASan/UBSan tests verify operation results/order variants, signed wraparound,
40,000 concurrent increments, release/acquire data publication, blocking/active
waits, silent stores, multi-signal waits and cleanup. Dynamic symbol checks
verify the actual dylib exports all 41 signal entry points. AMD signal layout
is checked against the original headers. The vendored headers now match LSE's
pinned HRX header revision cc2b5f429de4d1cb2be96ed10e6f45246e408d0e rather than
the separately examined newer HRX main. No ROCr implementation was imported.

Stop GPU completed at 2026-09-23 07:08:02 UTC, releasing build-178 retained
storage through reset and PCI close. The signed build-179 app is open for the
next installation. Hardware retesting is deferred while runtime software work
continues; the completed-fence/bad-data result from 178 is still the latest
authoritative native-dispatch result.


## Build 179 hardware acceptance — 2026-09-23 15:23 UTC

Installed runtime identity verified build 179. Initialization completed all
stages at 15:23:11 UTC. At 15:23:38, Dispatch Test passed four workgroups
(128 results, fence 1, seed 0x25d2b85b) and eight workgroups (256 results,
fence 2, seed 0x265c6eea), including all inputs and guards. The host then
released code, kernargs and data. This supersedes the build-178 bad-data result.
It validates the native caller-uploaded compute interface, not HSA queues or
HRX inference.


## Native HSA memory and software queues — 2026-09-23

Added host/device pool and region APIs, allocation/access metadata, checked
synchronous copies, dependency-gated asynchronous copies, fill and pointer info.
Added CPU software queues with pinned AMD metadata layout, invalid initial
packets, monotonically assigned IDs, and the complete index atomic family.
The pinned HRX audit advanced from 48 to 79 exported functions (40 missing).
This is symbol coverage, not HSA conformance or GPU queue completion.

Temporary observer probes replace persistent idle connections, removing a
source of Host Stop Busy errors. Lazy native sessions reproduce the verified
R9700 firmware ordering; no reset occurs after failed ownership acquisition.
GPU buffers use the existing driver BO ABI and 4 KiB visible staging with
unaligned-edge preservation. Transfer failures fault the session and retain
staging. No driver or installer changes were needed for this memory milestone.

At 15:48 UTC, Host Stop completed, then the actual dylib's memory test initialized
the GPU independently, reported 33,939,259,392 bytes in the device pool, verified
12,003 payload bytes and all 4,381 surrounding guards, observed asynchronous
completion, and freed both device buffers. After final HSA shutdown a separate
observer reported driver179/stage0. An earlier live run while Host owned the
GPU returned OutOfResources before initialization, as expected for build179.

Five ASan/UBSan suites pass, covering callbacks, lifetimes, cancellation, packet
publication, atomic contention, exact firmware transcript and each injected
initialization failure, plus production transport staging and owner conflicts.
GPU-visible atomics, hardware AQL queues, executable loading and HRX/LSE
inference remain unfinished. The next lifecycle change must let multiple
clients share one driver-owned GPU session without resetting each other's work.


## Build 180: shared sessions and HSA executable loading — 2026-09-23

PCI ownership moved to the root driver. Per-client participant references
separate initialization, shared ready sessions, and legacy exclusive mappings.
Firmware upload/reset require the initialization or sole-client lease. Closing
one healthy client retires only its resources while peers remain. Last-client
close performs verified quiesce/reset before global release. Faulted closes
retain a list of quarantined clients until a successful reset; observer clients
neither block teardown nor take initialization ownership.

All 27 existing regression scripts and the signed Debug app build passed before
installation. RuntimeBuild verified 180 on the live device. Two HSA processes
shared the GPU: the second completed its full memory test and exited, after
which the first verified all 12,003 payload bytes and 4,381 guards. The device
remained at stage 15 between client exits and returned to stage 0 after the last.
A second two-client test terminated the first with SIGINT without HSA cleanup;
the remaining client still verified all data/guards and shut down normally.
This covers abrupt idle-process exit, not hung in-flight GPU execution.

HSA gained gfx1201 ISA enumeration and nine executable APIs backed by a bounded
ELF64/MessagePack parser, kernel descriptor checks, supported relocation
processing and GPU BO upload. The pinned HRX symbol audit is now 90/119, with
29 missing. Parser checks include every fixture truncation, 1,000 mutations,
invalid layouts, overflow and descriptor-changing relocations. Six runtime
ASan/UBSan suites pass, including backend failure cleanup and reentrant release.
A hardware executable test uploaded the linked LLVM kernel, froze the executable
and resolved vector_add.kd at 0x8010000580 with 12-byte kernargs. No HSA kernel
was dispatched; GPU-visible signals, AQL queues and loader extensions remain.

Installing the ELF linker also upgraded Homebrew's default LLVM. Regression
shader generation now selects the retained LLVM 21.1.8 keg and its compatible
versioned Z3 library through a local environment helper, preserving the tested
instruction bytes without changing global library symlinks.


## Build 181: remaining HRX symbols and GPU IPC hardware acceptance — 2026-09-23

Added all 29 previously unresolved dynamic symbols. The pinned HRX audit now
reports 119/119 exported. Seven Linux DMA-BUF/SVM paths deliberately return
errors; this is not full runtime readiness. The audit now prints platform and
runtime limitations, and --require-runtime-ready fails while tracked GPU/HRX
requirements remain incomplete. hsa/API_STATUS.md records each added family.

Implemented native-page host allocations, virtual-memory reservations and
aliases, pinned backing, host access protections, reference-counted overlapping
locks, CPU cache queries, CPU IPC signals, GPU memory IPC, queue profiling flags,
software-queue control rejection and system-event registration/dispatch. GPU
hardware event notification wiring remains pending. Completion-job captures
are now retired outside the runtime mutex to avoid reentrant BO-release deadlock.

Driver selectors 52/53 export/import device VRAM using random capabilities and
a driver-owned reference table. Bulk client cleanup and BOFree free the physical
allocation only on the last reference, including references held in quarantine.
Imports require an initialized device and cannot reset/reinitialize a stopped
GPU using a stale sharing token. Driver 181 and the app built, were signed with
unchanged entitlements, and passed strict signature verification. Installer code
was unchanged. The user installed the build; RuntimeBuild verified 181.

Hardware HSA IPC passed: exporter uploaded 16,384 patterned bytes, importer
attached twice and verified the entire allocation, exporter exited without HSA
cleanup, and importer verified every byte again. Imported writes and readback
also passed. Balanced detach and final shutdown returned the GPU to stage 0.

Eight HSA ASan/UBSan suites pass, including separate-process shared signal
updates and cleanup after a SIGKILLed peer. All 28 non-runtime regression scripts
pass, including extracted production driver export/import/free/close paths.
GPU-visible signals, hardware AQL queues, shared host/GPU mappings, AMD loader
extension support and actual HRX inference remain incomplete.


## Build 182: AMD loader and equal-address host-memory transport — 2026-09-23

Implemented the complete AMD loader 1.03 extension table: host address queries,
segment/executable/loaded-object enumeration, loaded metadata and embedded-file
readers. ELF load segments are retained, including file/BSS distinctions.
Executable lifetimes retain source storage and descriptor translations after
reader destruction. File readers duplicate descriptors and use bounded pread
without changing the caller's file position. Loader enumeration serializes
executable mutations while allowing metadata callbacks outside the runtime lock.
Stale handles, allocation failures, address collisions and truncated/oversized
extension table requests are covered by sanitizer tests.

On installed build 181, the real loader probe uploaded vector_add.kd,
queried descriptor 0x8010000580 through the extension after destroying its reader,
and verified kernarg metadata and the 16 KiB GPU allocation. This proves loader
behavior, not kernel dispatch. A separate host-memory test passed 16 KiB each
way with zero mismatches and completed DMA unmapping at 2026-09-23T17:51:13Z.
The earlier unverified assertion that the platform could not perform host reads
is not a current blocker.

Build 182 extends the memory test with independent direct-CPU read/write patterns,
without PerformOperation, before enabling GTT BO allocation. SDMA copy accepts
owned GTT or VRAM BOs. Selector 54 establishes one immutable process-mappable
GART address window per GPU session, refuses live reservations, verifies both
hubs and retains a failed register update for reset recovery. The transport maps
GTT at the exact GPU address and refuses CPU address collisions without replacing
existing memory. Its diagnostic checks 64,003 unaligned bytes each way plus all
128 KiB shared bytes and device guard bytes. Hardware acceptance is recorded
below; shared HSA pools, GPU-visible atomic signals and hardware
AQL queues are not advertised yet. HRX inference has not run.

The app/dext build and strict signature verification passed with unchanged
installer code and entitlements. Eight HSA sanitizer suites and 29 driver
regression scripts pass, including window programming/fault retention, direct
DMA data verification, mapping collision cleanup and loader lifetimes.

### Build 182 installed: shared transfers pass, concurrent atomics fail

The live observer reported driver 182 and stage 0 before testing. The shared
transport then initialized the GPU itself and allocated 131,072 bytes at
CPU/GPU address `0x110000000`. Both unaligned 64,003-byte transfers, every shared
byte and the VRAM guards passed; mappings were released successfully.

Added a bounded raw-SDMA atomic diagnostic using ROCr's GFX12.0.1 BlitSdmaV5
ADD64 packet (operation 47, no GFX12.5 scope fields). It validates owned shared
storage, alignment, bounds and a maximum of 64 packets per batch, waits for the
driver's real VRAM fence and retains backing on uncertain submission/timeout.
It uses the existing exclusive raw-command API and needs no driver reinstall.
Regression coverage checks packet fields, signed decrement, carry, invalid
arguments, forged buffers and timeout retention.

On hardware, GPU-only decrement 1 to 0 and carry `0xffffffff` to `0x100000000`
passed. Concurrent CPU/GPU updates failed: CPU reported 245,885,253 increments,
seven completed SDMA batches accounted for 448 increments, the eighth batch
timed out, and the observed word was 245,716,216. No HSA GPU-signal capability
was enabled. Cleanup subsequently returned the driver to stage 0; the user
then initialized it again in the app. No further GPU submissions were made.

Added a read-only IORegistry ancestry diagnostic. The tested root port
DeviceCapabilities2 is `0xc1f`, without host AtomicOp 32/64 completion; both
Intel bridge ports are `0x10800`, without routing support. AMD bridge ports
are `0x330840`, with routing. These cached capabilities fail Linux's normal
AtomicOp path checks. They do not prove the precise timeout mechanism, nor
rule out other communication protocols. Shared DMA transfer success must not
be advertised as concurrent HSA signal atomicity. The diagnostic tests cover
missing capabilities, endpoint/root distinctions and multiple GPU paths.
## Build 183: HSA executable launches and shared shader data — 2026-09-23

Connected frozen HSA executable symbols to native selector-51 compute dispatch.
The runtime preserves the kernel's entry, resource registers and kernarg layout,
retains all referenced allocations/code through a real GPU fence, and rejects
unsupported scratch/LDS/preload/dynamic-stack or implicit-SGPR layouts. It does
not advertise hardware AQL queues or synthesize GPU signal completion.

LLVM's gfx1201 descriptor uses WGP_MODE, MEM_ORDERED, FWD_PROGRESS and instruction
prefetch. Added a 272-byte version-2 request to preserve these fields; the
original 264-byte version-1 format remains supported. Version 2 accepts ready
GART-bound data BOs while code stays in VRAM. Reserved/privileged and unsupported
resource fields are rejected. Prefetch is emitted to COMPUTE_PGM_RSRC3, and
CU-mode paired workgroup policy is not applied to WGP mode.

Added an explicit coarse shared allocator with identical CPU/GPU addresses,
correct unmap/free lifetime, CPU/GPU pointer-info access and allocation pins.
CPU access is allowed between completed GPU operations; concurrent system
atomics remain unsupported. The new kernel probe tests HSA loading followed by
two actual native launches, in VRAM or direct shared-memory mode, with 128/256
outputs and all 16 KiB inputs/guards verified. Installer code and entitlements
are unchanged.

Nine HSA sanitizer suites pass, including public executable/buffer destruction
during an in-flight mocked dispatch, old/new wire formats, malformed completion
retention and shared mapping lifetime. All 29 driver regression scripts passed,
the driver/app build succeeded, and both strict driver and deep app signature
verification passed. Both bundles identify build 183. Stop GPU completed its
reset/PCI close/resource release before opening the new app for installation.

After installation, the live observer verified build 183 at stage 0. The new
native test initialized automatically, loaded/froze `vector_add.kd`, destroyed
its reader and launched twice. VRAM mode at `0x8010004000` passed 128/256
computed results and all 16,384 input/output/guard bytes, with fences 1 and 2.
Shared mode at CPU/GPU `0x110000000` passed the same checks through direct CPU
stores and shader writes/readback, also with fences 1 and 2. Both tools exited
successfully after cleanup. No hardware AQL queue or HRX inference is claimed.


## Build 184 — bounded AQL lifecycle (hardware passed)

- Added a gfx1201 compute MQD and ROCr-compatible packet/queue/signal storage
  builder, with Linux structure offsets and register-mask checks.
- Reserved MEC pipe 0/queue 0 from MES scheduling, using an unused doorbell
  inside the existing MEC range. AQL publication uses packet indices: shadow
  1, first doorbell 0.
- Added uni-MES KIQ REMOVE_QUEUE using the Linux packet layout. Both API and
  trailing query acknowledgements are required. Failed mapping, publication,
  completion or unmap keeps GPU backing until verified reset.
- Added selector 55 and `mac_hsa_executable_dispatch_aql`, retaining executable,
  kernarg and data allocations through the bounded call. Completion is a
  GPU-only VRAM signal, not a coherent CPU/GPU HSA signal.
- Added `--aql` and `--aql-shared` kernel test modes with full result/guard
  checks. Both modes passed on installed driver 184.
- Replaced the README release history with working, unfinished and upcoming
  capability lists. No installer behavior changed.
- Validation: 30/30 driver regression scripts and nine HSA ASan/UBSan suites
  passed. The extended IOKit transport test also passed all seven malformed or
  failed AQL reply cases, the minimum-build gate and faulted-session retention.
  Xcode build and strict app/driver signature verification passed. Installed
  driver 184 was verified directly before hardware testing. Stop GPU completed
  before opening the new host app for installation.
- Hardware: `--aql` passed in VRAM at `0x8010004000`; `--aql-shared`
  passed at the identical CPU/GPU address `0x110000000`. Each mode verified
  128 and 256 computed outputs and all 16,384 input/output/guard bytes. Every
  dispatch observed completion 0, inactive/error signal 0, read index 1 and
  successful MES removal before queue storage was freed. Queues were recreated
  between dispatches. Public HSA queues and HRX inference remain incomplete.

## Build 185 — persistent queues and GPU-backed signal operations

- Added owned persistent AQL create/kick/destroy RPCs, seven legacy compute
  slots, monotonic doorbell publication and shared AMD queue metadata. Reserved
  MEC pipe 0 from the MES scheduler; bounded AQL retains slot 0.
- Client cleanup removes its queues before freeing memory while other clients
  continue. Failed mapping or removal retains backing until reset. Queue-owned
  ring/metadata BOs cannot be freed, and queue resource exhaustion does not
  fault an otherwise healthy runtime connection.
- Added public HSA queue creation, shared read/write indices, CPU doorbell
  hooks, inactivation/destruction, owning-agent queries and kick-error callbacks.
- Added pooled shared AMD signal storage and an embedded gfx1201 atomic kernel.
  CPU HSA value changes execute on the GPU, while CPU loads observe shared
  values. A failed executor wakes all affected signal waiters; new signals
  cannot reuse that faulted executor.
- Hardware on installed driver 184: shader-only atomic adds produced 4,096;
  release publication verified 1,024 payloads with live CPU observation. Native
  concurrent CPU/GPU RMWs failed (2,671,460 observed versus 2,704,434 expected).
  The GPU-backed HSA API then passed stores, arithmetic, bitwise operations,
  exchange, CAS success/failure, signed wraparound, waits and direct ABI reads.
- Added persistent hardware tests for 192 dispatches, ring wraparound, two-queue
  barriers, CPU signal release, concurrent GPU/CPU-HSA updates and shared CP
  completion decrements. These tests await installation of build 185; they are
  not recorded as passed. No HRX inference is claimed.
- Validation: ten HSA ASan/UBSan suites passed, including queue lifetime,
  reentrant error callbacks, signal fault propagation, malformed RPC replies
  and resource exhaustion. Driver packet, shared-buffer and shutdown tests
  passed; a new production-RPC test covers shapes, ownership, stale handles,
  seven-slot exhaustion and failed-unmap retention. Xcode build and strict
  code-signature verification passed for host and driver 185. Stop GPU completed
  successfully before the new host app was opened for user installation.

## Builds 186–187 — shared queue topology and SDMA ring wrap

- Driver 185 passed two persistent queues, 192 verified dispatches, barrier
  dependencies and concurrent shader/CP/CPU-HSA signal updates. A two-process
  test then exposed the fourth persistent slot targeting a nonexistent HQD.
  Linux's gfx1201 branch has two compute pipes with four queues each, rather
  than the larger topology used by other variants. Driver 186 maps flat slots
  1–7 across those two pipes and reserves both pipes from MES scheduling.
- With four queues mapped successfully, driver 186 exposed a separate SDMA
  timeout at the first 4096-DWORD ring wrap. Driver 187 keeps the write pointer
  as a monotonic 64-bit count and masks only the storage offset. Added regression
  coverage for repeated wraps, the 32-bit boundary and byte-pointer overflow.
- Installed driver 187 passed all seven queue slots, graceful eighth-queue
  exhaustion, 192 dispatches through a 64-packet ring and 256 barrier packets
  submitted by four CPU producers. Every dispatch verified all 16 KiB of input,
  output and guard bytes. Queue-to-queue dependencies and CPU HSA signal release
  passed.
- Two processes held four queues simultaneously. The first completed and
  exited; the surviving process continued through its existing queues, including
  ring wraps and concurrent producers. Both cleaned up successfully. This also
  crossed the SDMA boundary that had failed on 186.
- Concurrent shader and CPU HSA additions produced exactly 131,136. Two CP
  completion decrements plus 64 CPU HSA additions produced exactly 64. At least
  one CPU API call was issued while GPU work was pending in each concurrency
  test. The full GPU-backed signal API test passed again.
- Runtime discovery now reports kernel dispatch, seven device-wide queues,
  64–4096 packet rings and multi-producer support only for gfx1201 with driver
  187 or newer. Older builds and other GFX variants remain gated. The read-only
  observer verified these capabilities and final cleanup at stage 0.
- Validation: all 30 driver regression scripts and ten HSA ASan/UBSan suites
  passed. Driver 187 built and passed strict code-signature verification before
  installation. No manual GPU power cycle was needed during the passing tests.
  General GPU-accessible HSA host pools, scratch/LDS resources and HRX inference
  remain incomplete; symbol resolution alone is not runtime readiness.


## Builds 188–189 — HRX host adapter and resource candidate

- Added public CPU-owned coarse/kernarg pools backed by prepared DriverKit DMA
  buffers, truthful access/pointer metadata, and bounded native SDMA copies for
  same-GPU VRAM allocations. No fine-grained CPU/GPU atomic contract is advertised.
- Persistent queues now allocate/grow/reuse scratch, service the CP inactive
  signal, and expose LDS/private apertures. Queue workers stop before normal
  destruction returns; fault handling wakes waiters without fabricating completion.
- Replaced the hardcoded CU geometry with validated GC_INFO discovery, and added
  PCI/topology properties plus a bounded ATOM ROM timestamp-frequency reader.
- The executable loader accepts compatible gfx12-generic-v1 images, including
  all 17 real HRX helper kernels, with checked relocations and descriptor lifetime.
- Built the pinned HRX library and Loom compiler using a tracked macOS adapter
  applied to a disposable source copy. The explicit adapter supports one GPU and
  host-published AQL queues; it rejects unsupported device enqueue, instrumentation,
  hostcalls and PM4 replay. Hardware profiling enablement returns an error.
- Added actual HRX memory operations and executable dispatch checks: 4,093 FP32
  affine outputs and a 16×16 FP32 matrix product, with full readback and guards.
- Added CPU-only/GPU-only add and CAS controls, followed by native CPU/GPU
  contention experiments: three trials of 10 million
  64-bit additions per agent, then shared 64-bit CAS-lock contention. They require
  overlap, exact counts, intact guards and completed GPU work, with deadlines.
  Shader disassembly verifies system-scope 64-bit atomic instructions. Cached
  PCIe capability checks distinguish known-missing bits from unavailable registers;
  they do not substitute for measured interoperability. CPU mapping uses Apple's
  default cache policy, not a verified cache-inhibited mapping.
- Validation: 33 driver/ABI regression scripts, 12 HSA ASan/UBSan suites and nine
  PCIe diagnostic tests passed. Xcode build and strict app/driver signature checks
  passed. Stop GPU completed before opening the new host for installation.
  New resource, HRX and native contention hardware results remain pending;
  driver 187's earlier results do not validate these additions.
- Built the pinned LSE CLI against native HRX/Loom through a reproducible patch,
  including kqueue, Darwin sockets and portable argument reflection. Eight CPU
  suites and a readiness/TCP regression passed. Explicit kernel-archive linking
  fixed numerical CPU graph failures; the broad graph suite is 118/124, with six
  remaining device-first/GPU-launch-count assertions in unchanged upstream paths.
  No model inference has run.
- Installed driver 188 initialized successfully and reported the actual R9700
  topology: four shader engines, two arrays each, eight CUs per array (64 total).
  The first hardware property test exposed a stale one-array validation gate.
  Review then found the same assumption in queue admission and CU masks; these
  are corrected for build 189 using Linux's per-array compact-mask packing.
  The ATOM clock query also rejected actual NBIO 6.3.1; its ROM-offset handling
  now matches Linux NBIF bank selection, with bounded diagnostic logging. No new HRX or atomic dispatch
  result was produced by the stopped run.


## Driver 189 hardware results and driver 190 corrections

- Driver 189 passed discovered 64-CU topology and 100 MHz clock properties,
  public coarse/kernarg pools, GPU-backed HSA signal operations, all seven
  persistent queue slots, concurrent producers and two-process queue sharing.
- Scratch/LDS dispatch retired but had 272 wrong output words per queue with
  intact guards. Review found GFX12 scratch wave size encoded in 1024-byte units
  instead of ROCr's 256-byte units. Driver 190 changes the example TMPRING value
  from 0x11200 to 0x44200. Scratch units, register widths, SRD layout and
  per-engine wave policy now come from a capability table selected by discovered
  GC IP. The table records GFX9/10/11/12 properties; unimplemented queue layouts
  and unknown families remain rejected. Resource probes isolate IDs, scratch
  and LDS. Hardware validation remains pending.
- Native single-agent controls passed 10 million additions and 1 million CAS
  lock cycles each. Mixed addition completed 10 million operations per processor
  but returned 10,169,561 instead of 20 million. Mixed CAS stopped after six
  completed CPU cycles and an ownership error during the seventh; GPU completed
  one million and the payload was 1,000,007. This incomplete CAS trial does not
  represent two million completed operations. Queue cleanup returned to stage 0.
- Actual HRX startup exposed missing product-name and XNACK capability queries.
  The runtime now supplies gfx1201 identification, XNACK=false and dma-buf=false;
  a source audit covers 50 pinned HRX query attributes, including explicit
  optional exceptions. Real HRX compute and model inference are still unverified.
- Driver 190 exposes owned shared-buffer PTE/DMA and saved MQD diagnostics plus
  live endpoint PCIe controls and GFXHUB state. CPU cache attributes remain
  unknown; no AtomicOp or cache-policy registers are changed. GTT and firmware
  DMA allocations/bindings now honor Linux GFX12's 44-bit limit. Earlier DMA
  addresses already fit that limit, so this does not explain the atomic failure.
- Added serialized atomic handoff, returned-old sums, unequal CPU/GPU counts,
  staggered phase checkpoints and host timestamps. These test current mapping
  and configuration; failed contention does not prove hardware impossibility.
- Candidate 190 passed 34 driver/ABI scripts, 12 HSA ASan/UBSan suites, nine
  PCIe diagnostic tests, the HRX query audit, Xcode build and strict signature
  verification. Installation and new hardware validation remain pending.


## Driver 190 installed validation

- Live R9700 readback confirmed DEVCAP2=0x0073099f, DEVCTL2=0x0000:
  AtomicOp Requester Enable is off. Actual shared PTE matched expected,
  saved MQD policy stayed 0x4000 and GFXHUB PTBASE was 0x700001. CPU cache
  attributes and live upstream bridge controls remain unknown.
- All 32 serialized native atomic handoff rounds passed. Unequal-count tests
  completed but returned 1,191,166 (CPU 10M/GPU 1M) and 10,025,715 (CPU 1M/GPU 10M),
  each expecting 11M. Staggered A returned 6,124,524; its 2.5M CPU prefix and 2.5M
  CPU tail were exact, while the middle phase returned 1,124,524 of 6M.
  Returning-add also lost updates with failed old-value sums. Single-agent
  controls, data guards, completion and cleanup passed. See the atomic policy
  document for exact timing/count interpretation; no PCI configuration changed.
- The architecture-selected scratch fix passed two queues through initial
  allocation, growth and reuse, with all output and guard bytes checked.
- Actual HRX initialization exposed an adapter access-list assumption: CPU
  access was requested for GPU-only VRAM. Filtering that CPU request only for
  confirmed unmapped VRAM fixes startup without weakening runtime permissions.
- Actual HRX then passed streams, 64 KiB copy/fill, 4,093 exact FP32 affine outputs,
  256 exact FP32 matmul outputs, unchanged inputs/full guards and shutdown.
  No model inference result is implied.


## Driver 191 requester experiment candidate

- Added a dedicated endpoint-only AtomicOp Requester Enable begin/end API.
  The transition requires an initialized sole participant, exclusive lease,
  no queues/submissions, no BAR/IRQ ownership and no pending PCIe transactions.
  It saves recovery state before a 16-bit RMW of bit 6 and verifies readback.
- Explicit end and driver/client shutdown restore the original requester bit
  while preserving unrelated bits. Shutdown restores after transaction drain
  and before reset, then verifies after reset. Failed restoration retains
  backing/recovery state and blocks submissions; physical unplug can prevent
  rollback. No upstream configuration or ordinary capability advertisement changes.
- The explicit A/B harness reuses its allocations, kernel and completion signal,
  retires and recreates identical queues between settings, and compares PTE/DMA,
  full saved HQ_STATUS0, GFXHUB and mapping policy. It runs the exact staggered
  CPU 10M/GPU 1M algorithm OFF then ON. Restoration is checked against the
  independently saved OFF register, not just two fields of the restore reply.
- Shader source/binary hashes and CPU trial code match the measured driver 190
  baseline. No PTE, cache, shader ordering or allocation changes are included.
- Validation: 36 driver/ABI scripts and 12 HSA ASan/UBSan suites pass. The metrics
  regression harness needed the new selector enum to compile its extracted
  admission code. Xcode build and strict app/dext signature checks pass.
- Build 0.1.91 (191) was installed and verified through the live driver query.
  OFF trials completed, but the begin RPC returned NotReady (0xe00002d8)
  before reaching the register write; there is no ON result. The diagnostic
  retry measured 6,133,742 / 11,000,000, with exact single-agent controls,
  prefix and tail, retired GPU completion and intact guards. The driver
  returned to stage 0 after cleanup.

## Driver 192 PCI identity correction

- A direct HSA identity query on driver 191 returned chip 0xffff and revision
  0xff with success, although live configuration reads identified 1002:7551.
  Start had cached identity before opening the PCI device; the new requester
  experiment gate rejected that stale value. Identity must be read and
  validated after successful PCI Open before publishing it to HSA.
- Added raw IOReturn reporting for failed requester RPCs so transport failures
  cannot be mistaken for register readback or experiment results.
- Atomic shader, allocation, PTE, cache, CPU ordering and trial algorithm remain
  unchanged.
- Validation: client admission/open, requester, shutdown and atomic diagnostics
  regression scripts pass, including invalid-read close/unwind and another AMD
  device ID. All 12 HSA ASan/UBSan suites pass with host Mach API access; the
  virtual-memory suite is blocked inside the restricted sandbox. Xcode build
  and strict signed host/dext verification pass. Build 192 was opened.
- Installed driver 192 reports corrected HSA chip 0x7551 and revision 0xc0.
  The complete staggered A/B experiment confirmed DEVCTL2 0x0000 → 0x0040
  readback, unchanged mapping/queue policy and restoration to 0x0000.
  OFF returned 6,131,574 / 11,000,000; ON returned 6,133,395 / 11,000,000.
  Both single-agent controls, 2.5M prefix and 2.5M tail were exact; the middle
  counter deltas were 1,131,574 and 1,133,395 / 6,000,000. Both workloads
  completed with overlap, retired completion and intact guards. Enabling the
  endpoint requester alone did not fix concurrent RMW under this configuration.
  Cleanup returned to stage 0; amdgpu_mtop detects build 192. Detailed timing
  and limitations are in docs/PCIE_ATOMIC_TEST_POLICY.md.
- A separate SYSTEM-scope release/acquire ownership fixture passed 1,000 smoke
  rounds then 1,000,000 full CPU→GPU→CPU rounds in one persistent dispatch.
  Both directions validated all 64 words per payload, with zero errors, intact
  data/kernarg guards, turn 2,000,000, retired completion and no timeout.
  Elapsed exchange time was 222.591321 s (4,492.5 round trips/s including
  validation/polling). No config changes; Requester Enable remained off.
  Driver returned to stage 0. This supports the tested ownership-transfer
  path, without advertising fine-grained pools or concurrent native RMW.


## GPU-owned atomic mailbox and monitor candidate

R9700/gfx1201 hardware runs on driver 192 passed both 128- and 1,024-operation streams through all six persistent DMA mailbox configurations (active/hybrid polling; batch 1/8/64). All old-value checks, final values, completion sequences, dispatch retirement and guards passed. The longer run measured 42,708 operations/s for active individual requests and 162,426 operations/s at batch 64; existing one-shot HSA measured 66 operations/s with exact validation. These measurements exclude startup and do not compare IRQ delivery. The original PCIe capability audit correctly selected the fallback based on absent Intel bridge routing and Apple root completion. The native mixed RMW capability remains disabled.

Actual LSE→Loom→HRX Q6 projection produced 51 exact outputs with four unchanged input buffers/guards and zero host groups/fallbacks. Process teardown subsequently aborted in a recursive_mutex operation; this is not yet a clean end-to-end pass. The identified destruction-order issue is being corrected before model inference.

Monitor candidate 193 builds and passes signature verification. It adds persistent software counters, bounded 1 Hz firmware sampling for the exact qualified SMU 14.0.3/0x33 firmware profile, current clocks and AC DPM limits. The terminal dashboard has 60-second graphs and h toggles 100 ms/500 ms refresh. Driver-side and four monitor offline test suites pass; live sensor values await installation and hardware validation.

Driver 193 hardware follow-up: mailbox 1,024 passed all six variants with the monitor concurrently sampling; 156 fresh sensor snapshots were collected in 163 dashboard samples. Firmware reported 0x00684c00/interface 0x33; current clocks, AC DPM limits, load, power, temperature and fan fields populated, and software counters recorded completed work. Independent sensor accuracy remains unverified. The LSE shutdown-order fix then passed the actual Q6 workload and normal process exit (status 0): 51 exact outputs, all four input guards unchanged, one GPU kernel and zero host fallbacks. The observed evaluation time was 0.027 s, not an inference throughput measurement. Logs are under `build/tests/driver193-hardware/`.


## Paged buffer metadata and LSE model qualification

Driver 195 replaces the one-time 4096-entry BO table allocation with 64-entry pages. Empty pages are reclaimed only after backing release; live entries never move, and client-wide handle generations survive page recreation. Hardware validation passed three rounds with 1024 simultaneous 16 KiB device buffers, every byte checked, 512 alternating frees/reallocations, 128 consecutive frees/reallocations and clean teardown. Allocator regression tests verify the 8,191-live-allocation bound and full span recovery after fragmentation, fixing the old silent free-range loss.

Actual HRX pooling passed 96 simultaneously live small buffers with boundary canaries, reuse and a 2 GiB allocation tested at 0, 1 GiB and 2 GiB-4 KiB. Actual LSE Q6 still passes 51 exact outputs with no host fallback. Six LSE causal convolution cases passed 680 exact outputs across short/long sequences, zero padding and nonzero history; input guards and normal shutdown passed. The focused Loom Bind-alias fix and source-only regressions were committed to Geramy/LSE main as efee3fb.

The actual Qwen 27B 6-bit text path loaded 1,847 tensors (20.355 GiB payload; 11 slabs reserving 21.731 GiB). The first load took 33.2 seconds; a cache-warm retry took 14.2 seconds. 333 vision tensors are intentionally excluded by LSE's text-only path. After the convolution fix, strict GPU execution stopped at the next missing Loom template, repeat. No model token or PP/s/TP/s qualification is claimed. Both failed inference attempts exited normally and left the driver at clean stage 0. Logs are in build/tests/driver194-hardware and driver195-hardware.

## Qwen GPU generation through Loom and HRX

Loom repeat now preserves original element bits and passes nine actual GPU cases, including Qwen tensor shapes. Convolution-tail updates pass four cases and 408 exact outputs. GDN records per-lane recurrent state as typed mutable IR scalars, with collision-free SSA names and CSE preservation for raw store operands. Eighteen GPU cases pass independent numerical checks and prefix/suffix guards, including actual Qwen D128/H48 state carried directly from prefill into decode. Loom currently computes output/state in two kernels; this is distinct from the HIP fused path.

Typed runtime extents let paged attention and KV writes read dispatch metadata without embedding C expressions in Loom. Four GPU metadata cases passed two exact cache images and 64 attention outputs each, maximum error 5.96046e-08, unchanged input guards and no host fallback. The permanent offline compiler audit covers 60 operator/composition cases producing 192 gfx1201 groups. These include partial RoPE and simultaneous recurrent state/convolution-tail roots.

The complete Qwen3.8-27B-MLX-6bit one-token test produced "Paris" for "The capital of France is": 5,030 GPU groups, zero host groups/fallbacks, normal exit and driver stage 0. The follow-up generated 16 tokens, continuing with correct capitals for France and Germany, with 42,785 GPU groups and zero host fallback. Its five-token prefill took 3.36 seconds; the 15 subsequent decode tokens ran at approximately 3.0 tokens/s. The old printed 3.21 tokens/s counted the prefill-produced token in the decode numerator; this accounting is being corrected. Both model runs passed normal teardown. Logs: build/tests/driver195-hardware/qwen-loom-operators-fixed and qwen-sixteen-tokens.

These results qualify short GPU-only CLI generation on this checkpoint, not general model accuracy, quantization support or long-running HTTP service. Server shutdown and metric accounting are the next validation targets.


## Qwen HTTP chat, session isolation and shutdown

Loom flash attention now reads bounded runtime KV metadata, initializes padded queries, skips unused V-cache loads and resolves deduplicated input bindings. Five actual GPU cases passed numerical outputs, exact cache images and guards, including Qwen D256 and two attention windows; maximum error was 2.38e-7. The permanent compile-only audit now passes 198 cases and 846 groups across prompt widths through 128. Logs: build/tests/driver195-hardware/lse-flash.log and build/tests/lse-long-prefill-coverage.log.

The repeated-request test exposed shared writable recurrent state: scheduler-interned zero constants became carry outputs during retained-program folding. GDN state and convolution tails now allocate private buffers. Restart detaches state from the preceding computation graph and clears retained passes while preserving model weights and compiled kernels. Every mixer tracks its sequence cursor, including GDN-only models and one-token prompts. Two focused CPU regressions cover ownership, carry writes, cursor advance/rewind and restart; existing unsupported MTP replay cases are not qualified by this work.

The rebuilt native server passed five interleaved requests on Qwen3.8-27B-MLX-6bit: The → France → The → Germany chat → The. All three identical greedy prompts returned the same four tokens; France returned Paris and chat answered Berlin. GPU execution was required, with KV128 and MTP disabled. The 23-token chat prompt took 7.089 seconds (3.244 prompt tokens/s); 28 subsequent decode tokens took 10.626 seconds (2.635 tokens/s). All five HTTP requests succeeded, timing/count checks passed, SIGINT completed with exit 0 and a separate HSA discovery probe verified driver 195 at stage 0. Results: build/tests/driver195-hardware/lse-server-owned-state/result.json. These short runs do not establish long-context performance or independent full-model accuracy.

CLI/HTTP decode rates now exclude the first token produced by prefill. Server shutdown stops the listener once, drains in-flight work and joins the watchdog before normal destruction. The bounded grace-expiry path exits with failure without freeing potentially active GPU backing. Reproduction passed 17 selected host suites, 17 CLI-option cases, six runtime lifetime/strict-mode cases and Darwin socket/kqueue checks. The new scripts/run-lse-server-smoke.py reproduces session isolation, metrics and graceful shutdown; LOCAL_RUN.md has the user-facing server and chat commands.

## Loom inference performance, accuracy and calibration

The repeated resident benchmark now records binary/runtime hashes, exact request counts, warmup and measured samples. On Qwen3.8-27B-MLX-6bit, KV128, greedy decoding and no MTP, the five-token France prompt improved from a median 2.853677 decode tokens/s to 6.082109 with source caching, pointwise fusion and explicit flush64/poll64 settings. A same-runtime control confirmed that reducing blocked HSA polling from 1000 to 64 microseconds improved throughput; the runtime default remains 1000 microseconds. Disabling periodic flushes entirely was slower (5.593398 tokens/s). These are HTTP generation wall times including surrounding host work, not GPU timestamp or matched llama-bench results.

Q6 activation panels now rotate their shared-memory indexing to avoid bank conflicts. The gfx1201 prefill schedule reuses decoded weights across two or four rows without narrowing FP32 accumulators; single-row decode rotates only privately staged panels. Other architecture, dtype, indexed and caller-staged configurations retain their existing path. All 49 multirow and eight single-row hardware correctness cases passed, with unchanged inputs and guards. Four large prefill projections retained exact whole-output hashes while improving about 2.9–8.7 times; the checked single-row K5120/N17408 projection improved from 0.577947 to 0.326997 ms. These host dispatch/retirement timings are kernel comparisons, not model-wide speedup claims.

The combined Q6 changes then passed four full-model requests (one warmup and three measured), with identical output to the preceding implementation. Median decode reached 10.118556 tokens/s and warm PP5 reached 14.179178 tokens/s. A separate exact64-token prompt passed four repeated requests with median warm prefill 54.007206 tokens/s and 32 subsequent decode steps at 6.750825 tokens/s. The lower longer-context decode rate remains an optimization target. Normal exits and separate driver stage0 checks passed. Evidence is under build/tests/driver195-hardware/qwen-q6-combined-flush64-poll64 and qwen-q6-pp64-flush64-poll64.

An independent Apple Metal MLX run of the same fully hashed checkpoint exactly matched all five prompt IDs and all 33 generated IDs from the R9700 CLI. This covers one greedy continuation, not broad model accuracy. The reference script records native precision, top-ten logits and token margins; Apple Metal timing is not an R9700 comparison. The CLI's optional --token-ids diagnostic emits existing vectors only after generation. See docs/LSE_PERFORMANCE.md for provenance and comparison limits.

Native Loom calibration now executes validated stream and touch kernels when COMGR is unavailable. It checks every output and guard before publishing a rate and preserves backing if GPU retirement fails. Actual calibration measured a 512MiB streaming read at 416.829GB/s and host-observed amortized launch cost of 5.978 microseconds. Cache residency is unverified; matrix-rate measurements remain unknown. Profile identity includes compiler, batching and bounded polling policy. The calibration CLI subsequently produced the same33-token continuation with zero CPU fallback and clean shutdown.

The isolated HSA wait-policy build passed all18 software suites, including default/short polling and unnotified DMA-like stores. The shared-memory test required normal macOS shared-memory access outside the sandbox. The combined Q6 adapter passed24 selected host suites, server CLI checks, six runtime-lifetime checks and Darwin socket/kqueue checks. Driver195 remains installed; these changes do not replace the dext.

The 64-token continuation also now exactly matches 64 prompt IDs and 33 generated IDs from an independent float32 MLX reference. Native BF16 MLX diverged at an exact 18.375-logit tie between China and South; converting only floating parameters/scales/biases while preserving packed Q6 integer weight identity resolves this observed split. This remains two-prompt accuracy evidence, not broad model qualification.

- Automatic single-device decode batching now qualifies ordinary warm decode
  samples and selects stable improvements without repeating model work.
  Verified median 10.127645 tokens/s with 64 µs polling and 6.967066 with the
  default 1000 µs polling; all output text identical and clean shutdown.
- Reproducible HRX compute-copy benchmark verifies payloads, guards and source
  preservation. Repeated 16 MiB buffers measured H2D 6.79 GB/s, D2H 7.04 GB/s,
  D2D 290.28 GB/s payload; cache residency is not established, so these are not
  claims of raw link or physical DRAM bandwidth.
- Monitor percentage graphs now retain a fixed 0–100 scale and clear stale
  headlines. Owned-idle testing found SMU interface 0x33 reporting 100% despite
  idle GRBM/CP and no queues; the monitor labels this raw counter explicitly.

- Eight-row Q6 prefill reuse preserves exact output hashes across 63 fixtures
  and reduces large matrix dispatch times by 19–21%. Full-model PP64 improved
  from 54.01 to 63.12 prompt tokens/s with identical response text and clean
  shutdown; decode remained approximately 6.76 tokens/s before attention changes.
