# Native AMD GPU monitoring

`amdgpu_mtop` is a macOS rewrite with a terminal organization inspired by
amdgpu_top: device selection, GPU activity, memory, clocks and sensors. No upstream
UI implementation was copied. A straight build of amdgpu_top would not provide
telemetry here: its backend relies on `libdrm`, `libdrm_amdgpu`, Linux driver
interfaces, sysfs and process fdinfo. The new backend enumerates every
`MacAMDGPU` service and opens each by its IOKit registry identity.

## Implemented and pending

Build 193 adds software counters and graphs, current clocks and AC DPM ranges,
and a bounded one-second sensor sampler for an already-initialized GPU. The first live collection and concurrent-workload check passed; independent
sensor accuracy and an initialized idle/load comparison remain pending. Earlier build 176 rejected the
installed **104.76.0 / interface 0x33** before sending a metrics command. Build
193 supports an explicitly labeled Linux-compatible profile for that exact
release; it does not claim to establish a new 0x33 firmware schema.

| Statistic | Current monitor | Required source |
| --- | --- | --- |
| Device list and selection | Implemented for MacAMDGPU-bound cards | IOKit registry IDs |
| Responding build and initialization stage | Implemented | Observer selectors 43 and 21 |
| GPU IP label | Implemented as driver-reported configuration | QueryInfo GFX version; currently configured, not fresh hardware discovery |
| VRAM total and CPU-visible capacity | Implemented after initialization | GMC cached sizes |
| GFX activity | Implemented; hardware validation pending | SMU AverageGfxActivity, percent |
| UMC activity | Implemented; hardware validation pending | SMU AverageUclkActivity, percent |
| Media activity | Implemented; hardware validation pending | Maximum of two SMU VCN activity percentages |
| GFX/memory/fabric clocks | Implemented; hardware validation pending | SMU pre/post-deep-sleep averages, MHz |
| Current GFX/SOC/memory/fabric clocks | Implemented; hardware validation pending | SMU CurrClock[PPCLK_*], MHz |
| AC DPM minimum / maximum clocks | Implemented; hardware validation pending | Read-only GetMinDpmFreq / GetMaxDpmFreq replies, MHz; not active throttling caps |
| Socket power | Implemented; hardware validation pending | SMU AverageSocketPower, whole watts |
| Board power | Implemented; hardware validation pending | SMU AverageTotalBoardPower; confirm board reporting on hardware |
| Edge/hotspot/memory temperature | Implemented; hardware validation pending | SMU AvgTemperature, degrees Celsius |
| Fan speed | Implemented; hardware validation pending | SMU AvgFanRpm and AvgFanPwm |
| VRAM allocator used / free / largest span | Build 178 CPU-side observer accounting for visible and GPU-only pools | Both GMC allocators; fixed reservations/gaps separate |
| GTT allocated / free | Unavailable | Shared GART allocator accounting; aperture reservation differs from resident host memory |
| Per-process memory and engine use | Unavailable | Per-client BO/queue ownership and scheduler accounting |
| PCIe throughput, per-engine busy and ECC | Unavailable | Separate counters/interfaces, not inferred from this table |

UMC activity describes memory-controller work. It is not the fraction of VRAM
allocated and not a measured bandwidth in GB/s. Similarly, a current GRBM busy bit
is not a time-averaged GPU utilization percentage. The monitor keeps those
concepts separate. Total VRAM in QueryInfo is the driver's usable capacity after
firmware reservation and may be less than the board's marketed capacity.

## Build 193 first hardware capture

`build/tests/driver193-hardware/monitor-mailbox.jsonl` records 163 dashboard
samples, 156 fresh cached sensor reads and **16 distinct firmware captures**.
Capture intervals were 1.025–1.058 seconds (median 1.041), independently of the
100 ms UI refresh. Firmware version was 0x00684c00 and profile 0x33 throughout
fresh samples. Concurrent `mailbox-1024.log` records all 1024 operations passing
for the one-shot baseline and each active/hybrid DMA-mailbox batch size, with
completion retired and guards intact. Most of the sensor window covers the
15.65-second one-shot baseline; the subsequent persistent mailbox variants are
too short to treat these 16 captures as measurements of each variant.

The decoded sequence is internally plausible: edge/hotspot/memory temperatures
rose 40/54/38→60/83/56 C, selected average GFX rose 2721→3299 then fell to 3229 MHz,
socket/board power rose 196→about 300 W, and fan readings varied 872–938 RPM.
DPM queries independently returned GFX 500–3600 MHz, SOC 417–1476, memory 96–1258
and fabric 313–2400. These values support a working transfer and a coherent
Linux-compatible interpretation; they do not independently calibrate sensors.

Raw `CurrClock[]` remained GFX 1000/SOC 548/memory 96/fabric 1108 MHz. Linux's
`smu_v14_0_2_get_smu_metrics_data` exposes these raw fields, but
`smu_v14_0_2_get_gpu_metrics` explicitly assigns `current_gfxclk` from the
selected **average** GFX clock instead. The raw/average difference is therefore
not itself evidence of a shifted layout. The dashboard consequently presents selected average first, then **SMU raw**
and **raw peak**; JSON calls this field `raw_current_mhz`. Raw GFX must not be
presented as an independently verified instantaneous operating frequency. Socket and board power
were identical in all 16 captures; retain their firmware field names without
claiming independent board/socket measurement. The firmware metrics counter
varied around 1014–1048 rather than increasing monotonically; neither liveness
nor elapsed time is inferred from that counter.

GFX activity reported 96% initially then 100%, with UMC 0%. GFX activity is not
CU occupancy or shader throughput; the observed work alone does not validate
its percentage. A continuous nonzero activity reading after all owned queues
finish would merit initialization/firmware-idle investigation rather than
silently interpreting it as useful compute load. One transitional stage 12 read
was labeled unsupported before profile qualification; it had NotReady status,
not a failed table transfer.

A subsequent owner-retained idle/load check is recorded in
`monitor-idle-load-retry.jsonl` and `queue-idle-load-retry.log`. It held two empty
initialized queues for 12 seconds, then passed 192 dispatches with shared signals
and multiple producers. Sensor GFX remained 100% and power about 300 W even during
the empty-queue interval; average GFX drifted 3249→3189 MHz while thermal/fan
readings rose. This **does not establish good idle/load correlation**. The cause
remains unresolved: initialization could leave an engine busy, or the compatible
profile's activity/power semantics could differ. Do not claim independently
validated activity percentage or useful-compute utilization from these readings.

### Further acceptance design

Keep one owner initialized across the entire comparison; closing the final
owner resets the GPU and cannot provide an initialized-idle baseline. Record
at least 20 distinct one-second samples with no queued work, 20 under a sustained
known compute workload, then 20 after observed completion while retaining the
owner. Separately exercise a large known memory/copy workload to test UMC
response. Deduplicate by registry, reservation generation and sample sequence,
not UI refresh count. Mark exact workload start/finish timestamps and retain
raw/profile metadata. Compare medians and transitions in activity, selected
average clocks, power and temperatures; expect temperature lag, not immediate
step response. Do not require raw GFX `CurrClock` to equal its average, or infer
sensor correctness solely from broad plausibility. A persistent 100% idle GFX
reading, stationary power under substantially different workloads, or implausible
cross-field changes warrants investigation. Independent power/thermal evidence
or a matching Linux run would strengthen unit/accuracy qualification.

## Software work counters and graphs (build 193)

Selector **61**, with no input or scalar output, returns the version-1
456-byte `software_stats::Snapshot` from `amdgpu_software_stats.h`. It is an
observer endpoint: it never opens PCI, polls a GPU fence, submits work or sends
SMU messages. All access runs on the serialized lifecycle queue. For mapped
persistent AQL queues it reads only the pinned host metadata `read_dispatch_id`
with an acquire load; that mapping cannot be freed concurrently on this queue.

The snapshot includes a counter generation, sampling timestamp, epoch start
(`sessionStartNs`, despite totals spanning individual sessions), readiness flags,
participant count, mapped AQL queue count and queued/published/consumed/retired
packet counts. Each of SDMA0, SDMA1, GFX and bounded AQL dispatch has submitted,
observed-completed, failed, pending and retired work counts, cumulative software
pending time, and completed payload bytes by host→device, device→host,
device→device, host→host or unknown direction. Successful client BAR writes and
readbacks have separate CPU payload byte counters.

Instrumentation counts successful publication, not rejected API calls. Linear
SDMA copies are counted inside the shared copy implementation, including general
BO copies and transfer smoke tests. GFX EOP and smoke fence submissions are
counted at their actual publication/completion points; raw CS completion is
counted by the existing owner's fence poll. The observer does not cause that
poll. Unknown raw packets do not receive invented payload byte counts. SDMA
direction classification requires complete source/destination ranges inside
known VRAM/GART bounds; legacy physical DMA addresses can remain unknown.
These are tracked work submissions, not every firmware control message or
every bootstrap ring test.

Completion means the relevant fence was observed. Copy payload totals do not
claim independent data verification or measure all PCIe traffic. Failure is
recorded once for a published operation that times out or loses readback;
repeated waits do not repeatedly count the same failure. Failed work remains
pending until a later verified completion or a verified shutdown retires it.
A failure can therefore later also have an observed completion. Retired means
completion was not observed before tracking ended; it is never counted as a
successful completion.

Counters persist for the entire bound driver lifetime, so a short-lived HSA
client's completed work is still visible after it closes. Successful Stop GPU
or last-participant reset closes pending intervals, records outstanding work as
retired and clears active queue tracking while preserving all totals and the
generation. Failed reset preserves outstanding state. Reattaching the driver
creates a new registry identity/counter epoch. Read indices going backwards or
beyond the published index mark queue sampling incomplete instead of wrapping
or manufacturing a huge rate. Counter overflow saturates and is flagged.

Pending time measures the union of software outstanding intervals per engine.
It includes time waiting for an owner to observe completion and is **not GPU
hardware utilization**. AQL read index advancement means packet consumption,
not kernel completion; persistent packet counts remain separate from bounded
AQL dispatch completion counters. Producer-writable shared metadata is not an
independent hardware measurement. Unmapped queues retire their last unobserved
packet range rather than claiming it completed.

The dashboard plots software submission/transfer rates, pending work and VRAM
allocator use. Its 60-second histories use monotonic sample times and fixed time
buckets; changing refresh cadence does not change the time axis. Press **h** to
switch between **Slow 500 ms** (default) and **Fast 100 ms**, or use `--slow` /
`--fast`. These refresh intervals apply only to observers and cached data, never
to SMU collection. Missing samples and generation changes cannot create a
negative or cross-session throughput spike. Hardware power, thermal, clock and
utilization fields retain their separate source/validity rules.

Offline verification: `scripts/test-software-stats.sh` exercises the real
observer RPC/queue snapshot and production SDMA copy body, plus saturation,
union intervals, queue reuse, direction classification and epoch reset. The
existing CP, raw-CS, BAR transfer and shutdown tests also assert their counter
effects, including preserved totals across verified reset and retention after
failed reset. None of these tests accesses GPU hardware.

## VRAM accounting (build 178)

QueryInfo selector **21**, tag **5**, returns 15 scalars. This is a CPU-only
snapshot on the existing serialized driver queue. It does not open PCI, read
VRAM, submit firmware messages or claim ownership. The standalone monitor only
requests this tag on build 178 or newer. Its availability is independent of the
SMU firmware-interface gate.

| Scalar | Meaning |
| --- | --- |
| 0 | ABI version, 1 |
| 1 | Flags; bit 0 means the complete accounting snapshot is valid |
| 2, 3 | Firmware-reported usable capacity, CPU-visible aperture, bytes |
| 4 | Usable bytes outside both allocator pools |
| 5–9 | Visible pool capacity, used, free, largest free span (bytes), allocation count |
| 10–14 | GPU-only pool capacity, used, free, largest free span (bytes), allocation count |

`amdgpu_vram_accounting.h` validates each pool's bounds and rejects overlap,
overflow and inconsistent accounting. `snapshot(ready, base, usable, visible,
lowAllocator, highAllocator)` returns version 1 and otherwise zero when not
ready. Integration supplies readiness only while GMC and full bringup are
initialized, PCI is open, and shutdown/stopping/blocking flags are clear. It
copies all 15 scalars and sets the returned scalar count explicitly. The
caller must invoke the helper on the same queue as allocator mutations.

Used bytes are the allocator's charged sizes, including alignment rounding.
They include client BOs and driver allocations such as rings, writeback storage,
MQDs and smoke-test storage in that pool. Retained failed-work storage remains
charged until actually freed or the allocator is reset. Build 194 reserves free-list metadata against a live-allocation bound so valid
frees cannot exhaust it; see [BUFFER_CAPACITY.md](BUFFER_CAPACITY.md). These are allocation
accounts, not a measure of GPU accesses, resident host memory, bandwidth or
total hardware occupancy. Per-client attribution is not part of this ABI.

On the current small BAR layout, the low 24 MiB fixed bootstrap arena is outside
the visible allocator, and the high pool leaves its final MiB reserved. Any
alignment gaps are also excluded. The low arena covers the driver's fixed PSP
firmware/ring/command/fence/TMR and GMC storage; the retained SMU table resides
inside that firmware arena and is not counted twice. These exclusions describe
reserved address ranges, not how much firmware actually uses. Firmware's own
top reservation already excluded from GMC usable capacity has unknown size
relative to installed physical VRAM and is not guessed. The snapshot computes
actual excluded bytes from the pool layout rather than assuming 25 MiB for
every BAR configuration.

Largest free span reports the raw contiguous range. It is not an allocation
guarantee: requested alignment, rounded size and available free-list metadata
can still limit an allocation. An absent GPU-only pool with a full BAR is a
valid zero-sized pool. Valid zero usage displays zero; stopped, unavailable,
malformed or unsupported accounting displays unavailable/null. Capacity and
each pool's used/free values are published together in `vram_accounting` JSON;
the old `vram_used_bytes` stays null because it represented unmeasured total
occupancy. UMC activity is unchanged and remains separately gated telemetry.

`scripts/test-vram-accounting.sh` checks rounding, >4 GiB allocations, fragmented
free ranges, coalescing, duplicate frees, allocation metadata bounds, full-BAR
layouts, invalid bounds and stopped-state suppression under ASan/UBSan. The
monitor's synthetic renderer test checks real zero versus null and does not
connect to hardware.

## Firmware decoder

`dext/amdgpu/amdgpu_metrics.h` is a pure decoder; it issues no commands and does
not map or access GPU memory. It accepts exactly 412 bytes matching the pinned
Linux `SmuMetricsExternal_t`, SMU IP **14.0.3**, and driver interface **0x2e**.
The additional Linux-compatible profile requires interface **0x33** and live
firmware version **0x00684c00**, with separate profile flag and plausibility
filters. Other mismatches are rejected before table transfer.
`smu_smc_hw_setup` now accepts an optional `SMUMetricsContext` to retain the
firmware interface and staging coordinates before programming either address
half. Build 176 passes the per-device context from the SMU bringup stage.

The local Linux mapping is:

- `drivers/gpu/drm/amd/pm/swsmu/smu14/smu_v14_0_2_ppt.c` maps `SMU_TABLE_SMU_METRICS`
  to `TABLE_SMU_METRICS`, and converts the fields into `gpu_metrics_v1_3`.
- `drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu14_driver_if_v14_0.h` defines
  firmware table ID **5**, `SmuMetrics_t`, and `SmuMetricsExternal_t`.
- `drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c` sends `TransferTableSmu2Dram`, waits
  for acknowledgement, invalidates the HDP read path and copies the table.

### Interface 0x33 investigation

The local Linux reference and the reviewed primary sources in both
[AMD's ROCm/amdgpu implementation](https://raw.githubusercontent.com/ROCm/amdgpu/master/drivers/gpu/drm/amd/pm/swsmu/smu14/smu_v14_0_2_ppt.c)
and [upstream Linux](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/amd/pm/swsmu/smu14/smu_v14_0_2_ppt.c)
still declare `SMU14_DRIVER_IF_VERSION_SMU_V14_0_2` as 0x2e and use
`smu14_driver_if_v14_0.h`. AMD's corresponding public firmware-interface header
contains the same metrics fields used by this decoder. The reviewed interface
history advances 0x26 to 0x2e; it does not establish a separate 0x33 layout.

Linux's `smu_cmn_check_fw_version` reads and logs the driver and firmware
interface versions, then returns success without requiring equality. The older
[Linux v6.18 SMU14 implementation](https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/amd/pm/swsmu/smu14/smu_v14_0.c#L253-L266)
explicitly states that newer firmware is designed for backward compatibility;
the newer common implementation no longer includes that explanation. This is
primary-source evidence of intended compatibility, not evidence that the
version mismatch itself represents an error. It corrects the earlier assessment
that no explicit compatibility statement had been found. The reviewed statement
does not specifically name interface 0x33, table 5, or individual metrics-field
offsets and units. An exact 0x33 metrics schema or table-specific compatibility
guarantee remains unverified.

The 2026-09-23 offline review also confirmed that the vendored
`firmware/smu_14_0_3.bin` matches the local linux-firmware copy byte-for-byte:
333236 bytes, header microcode version `0x00684c00` (104.76.0), SHA-256
`3221ef2ddb341570eeb727e1e16f170bfb2ea1230be4a7eb248d0b183fbfbf15`.
The firmware binary header identifies the release, not the SMU metrics layout;
WHENCE lists the file without declaring a 0x33 table schema. Neither is used
as a substitute for layout validation.

Build 193 follows that source-backed compatibility behavior for one pinned
profile, not for arbitrary newer firmware. Before any table transfer it reads
`GetSmuVersion` (**0x02**) once per initialized context. SMU IP must be 14.0.3;
interface 0x33 must return **0x00684c00**. A different release remains unsupported
and no table request is sent. A failed version request latches collection off.
Successful samples carry `kSMUMetricsLinuxCompatible` (bit 3) separately from
normal validity; consumers must preserve that qualification in their output.
The firmware release is exposed in selector62's clock metadata.

The collection reuses the retained 64 KiB staging reservation, sends one
`TransferTableSmu2Dram` (**0x12**, parameter **5**), waits for acknowledgement,
and reads exactly 412 bytes via 103 BAR0 dword reads. It never changes the
staging address while collecting. The table keeps Linux's documented field
layout and units. The compatible profile additionally suppresses implausible
clocks above 10000 MHz, temperatures above 150 C, power above 2000 W, fan speed
above 30000 RPM and voltage above 2500 mV. GFX activity/average clock, edge
temperature and socket power must remain valid independent anchors. These
broad checks catch malformed data; they are not proof of a separately verified
schema. No live result is claimed by the offline tests.

Current clocks are the table's uint32 `CurrClock[]`, in order GFX, SOC, UCLK,
FCLK; they are distinct from the pre/post-deep-sleep averages. Clock limits use
Linux `smu_v14_0_get_dpm_ultimate_freq`'s **GetMinDpmFreq 0x1d** and
**GetMaxDpmFreq 0x1e**, each parameter `PPCLK << 16`, once per initialized
context. The maximum is explicitly the firmware-advertised **AC DPM maximum**,
not an inferred AC/DC state, active cap, observed peak or guaranteed boost.
Each valid range requires `0 < minimum <= maximum <= 10000` MHz. Unsupported
queries leave that range unavailable; a timeout stops further commands and
latches all collection off. `GetDpmFreqByIndex(... | 0xff)` is a DPM level-count
query and is not used as a current-clock measurement.

The decoder reads little-endian bytes without unaligned casts. It uses Linux's
5% busy threshold to select pre/post-deep-sleep averages. It converts whole-watt
power to milliwatts and degree temperatures to millicelsius. Linux's separate
sensor API shifts socket power left eight bits to Q8; that conversion must not
be applied to the raw table or confused with its watts.

Validity is per field. Out-of-range activity, 0xffff fields and an all-zero or
all-ones table are unavailable. Linux clamps the signed-underflow UCLK activity
quirk to zero; the monitor instead marks that field and its dependent average
clock selection unavailable, so invalid firmware data cannot look like idle.
Zero can be valid for an individual fan/clock/activity field. Firmware's metrics
counter is retained but does not alone establish liveness or time units.

`tests/metrics_test.cpp` asserts the production offsets and table size against
the actual Linux header. It exercises conversions, deep-sleep selection,
unavailable values, wrong interface/IP, truncation and unaligned snapshots under
ASan/UBSan. The tests do not validate that this installed PMFW exports the table.

## Staging and ownership rules

The 64 KiB SMU driver table is retained inside the PSP firmware arena before
either address half is programmed. Later PSP uploads advance their bump pointer
without changing that reservation. A partial programming failure or collection
timeout never recycles the destination while firmware may still write it.

The whole request, acknowledgement and CPU copy run on the driver's serialized
queue. No other table operation can reuse staging between acknowledgement and
readback. The monitor has no arbitrary mailbox or table-transfer API. It only
receives copied CPU snapshots through its observer connection.

Device selection uses the registry ID for the active attachment, never “the
first matching GPU” after the user has selected one. Reordering does not switch
the selected card. Removal and reconnect produce an explicit disconnected/new
attachment state. A blocked/erroring card must not erase other cards' results.

## References

- [amdgpu_top primary repository and backend dependencies](https://github.com/Umio-Yasuno/amdgpu_top)
- [Linux AMDGPU GPU metrics documentation](https://docs.kernel.org/gpu/amdgpu/thermal.html#gpu-metrics)
- Local Linux reference commit: `1f63dd8ca0dc05a8272bb8155f643c691d29bb11`.

## Collection backend and snapshot ABI

`amdgpu_metrics.cpp` implements `smu_collect_metrics`, `smu_metrics_snapshot`
and `smu_metrics_invalidate`. Build 176 connects them to the per-device context,
owner selector 46, observer selector 47 and lifecycle invalidation. Initialization does not collect. Build 193's monitor uses selector63 at most
once per second, while selectors47/62 remain cached CPU-only reads.

Collection validates runtime readiness, retained VRAM staging, qualified firmware
profile, previous mailbox completion and BAR bounds before its single table-5
request. It waits for firmware acknowledgement through the existing bounded SMU
mailbox primitive. It then reads exactly 103 dwords through `MemoryRead32`, with
a 100 ms elapsed readback budget. The budget cannot interrupt a single PCI API
call; it stops further reads after a call returns over budget. This ASIC's
`hdp_v7_0_funcs` has no `invalidate_hdp` callback, so Linux's generic invalidation
call is a no-op here. No speculative HDP register writes are added.

Successful collection can be shared for one second. Transfer failure, invalid
table data, readback timeout or an already pending SMU command latches collection
off until the enclosing context is reset after a verified device reset. The
firmware destination remains reserved. The observer path only copies CPU state
and marks samples older than 2.5 seconds stale; it never refreshes firmware.
Errors clear validity and values while preserving the last success timestamp.

`SMUMetricsSnapshot` in `amdgpu_metrics_state.h` is a 192-byte, version-1 payload:

| Field | Meaning |
| --- | --- |
| version / size | Both must match the consumer's ABI |
| status / flags | Last operation status and valid/faulted/stale/Linux-compatible-profile bits |
| generation / sequence | Reservation's uptime timestamp and successful collection count |
| collectedAtNs / attemptedAtNs | Last successful snapshot and attempted collection, CLOCK_UPTIME_RAW nanoseconds |
| driverInterface / firmwareCounter | Firmware ABI and raw metrics counter |
| validFields / values[16] | Per-field validity and normalized integer values in metrics::Field order |

The generation identifies this boot/session's reservation, not a persistent
hardware serial number. Cached samples should be keyed by registry ID and
generation. The monitor also independently checks version, size, validity flags
and sample age before displaying values.

## Driver API and lifecycle

`BringupContext.metrics` retains the state, and SMU initialization passes it to
`smu_smc_hw_setup(ctx.device, ctx.psp, &ctx.metrics)`.

- **46 — CollectMetrics:** no input, three scalar outputs: operation status
  (kern_return_t bit pattern), successful sample sequence, and field validity
  mask. The RPC returns success when it can return those operation results;
  malformed arguments and admission failures return an RPC error. Collection
  requires the existing owning user client, full SDMAInit stage, PCI open and
  no shutdown/stopping. It never acquires a new PCI session. Pending submitted
  work returns busy rather than introducing another firmware operation.
- **47 — MetricsSnapshot:** no inputs or scalar outputs, a 192-byte struct
  response through IOConnectCallStructMethod. The driver validates output size
  and rejects descriptor output. It returns OSData created from a CPU snapshot;
  DriverKit owns and releases that OSData. This selector is an observer and is
  allowed through closed-session and shutdown-blocked checks without opening
  PCI. The user's own stopping/provider-detached checks still apply.

Root Stop, owner-client Stop/FinishStop, explicit Shutdown GPU before reset and
bringup resource release all invalidate telemetry before ownership or storage
is lost. The firmware reservation stays intact until the existing lifecycle
allows the enclosing PSP arena to be released after reset or completed detach.
Failed shutdown leaves the cached sample unavailable and retains the backing.
An observer closing does not invalidate or stop the owner's session.

- **62 — ClockSnapshot (build193):** no inputs/scalar outputs, 96-byte version-1
  `SMUClockSnapshot`. It contains status/profile flags, generation, collection
  timestamp, interface/firmware version, current/range validity masks and four
  MHz values each for current/minimum/AC maximum. Current clocks expire with
  the metrics sample; static ranges remain available while the session stays
  ready. Shutdown suppresses both. This endpoint never accesses hardware.
- **63 — SampleCachedSensors (build193):** same arguments and three output
  scalars as46, but available to observers only as an operation on an existing
  initialized session. It never opens PCI, claims ownership, initializes,
  resets or changes power policy. It returns Busy for tracked pending raw
  submissions without polling their fences. Readiness and shutdown checks run
  before every firmware request; the collector and teardown share the existing
  serialized dispatch queue. Closing this observer cannot stop the GPU.

The monitor attempts63 at most once per second per registry identity while
stage15 is reported, then reads47 and62. The driver also shares a one-second
success cache across observers. UI Fast100/Slow500ms affects the cache/software
refresh cadence only. The first sample may include one firmware-version query,
one table transfer and up to eight DPM range queries; each mailbox command has
the existing two-second bound. Subsequent samples issue only the table transfer.
An individual synchronous PCI read cannot be interrupted by the 100 ms copy
budget. A timeout or invalid table latches collection off until verified reset,
preventing repeated mailbox timeouts during refresh. A busy sample retains
cached values until their normal2.5s expiry. Sensor unavailability does not hide
software counters or allocator accounting.

The host's Sample Metrics button still performs one owner46 call then47 and
retains results in its log. Hardware acceptance should inspect that first
sample's profile, status, validity and plausible values before relying on the
continuous monitor. No hardware verification was performed by the offline tests.

## Timer integration audit (not implemented)

The current driver and user clients use the driver's default `bringupQueue`,
with user-client `Start` copying that queue via `CopyDispatchQueue` and
`SetDispatchQueue`. SMU collection must use that same queue, keeping the entire
transfer and BAR copy serialized with other firmware commands and teardown.

The installed DriverKit 25.5 `IOTimerDispatchSource.h` supports `Create(queue)`,
`SetHandler`, `WakeAtTime(kIOTimerClockUptimeRaw, deadline, leeway)` and
`Cancel(completion)`. `WakeAtTime` must be scheduled from the timer's target
queue. A future callback should rearm only after successful collection and
after rechecking lifecycle generation and readiness. Do not rearm after an
error, shutdown or removal. Retain the driver/context until cancellation's
completion has confirmed all pending callbacks finished. Disabling alone
without its completion is not a release barrier.

Root `Stop` and owner-client close currently have separate teardown paths.
Both must account for a telemetry timer in their completion/drain barriers;
adding only a timer pointer to the driver and freeing it in `Stop` is insufficient.
The cancelled source cannot be reactivated; a new session creates a new one.
No timer is added. The bounded selector63 sampler uses the already serialized
RPC path, so there are no telemetry timer callbacks to drain during teardown.

The collection, decoder, reservation and actual selector tests passed under
ASan/UBSan. Admission tests verify that cached observers do not open PCI and
collection cannot claim or steal ownership. Shutdown tests verify invalidation
before reset/Close and preservation when shutdown is rejected before starting.
Modified driver and SMU sources passed DriverKit arm64 syntax checks. The
standalone monitor build and identity/validity tests passed. No firmware table
request, GPU reset, power-policy change or installation was performed by these
tests.
