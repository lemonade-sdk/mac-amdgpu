# Native AMD GPU monitoring

`amdgpu_mtop` is a macOS rewrite with a terminal organization inspired by
amdgpu_top: device selection, GPU activity, memory, clocks and sensors. No upstream
UI implementation was copied. A straight build of amdgpu_top would not provide
telemetry here: its backend relies on `libdrm`, `libdrm_amdgpu`, Linux driver
interfaces, sysfs and process fdinfo. The new backend enumerates every
`MacAMDGPU` service and opens each by its IOKit registry identity.

## Implemented and pending

Build 176 integrates one-shot firmware collection and cached observer reads.
Live firmware validation is pending; no periodic collection is enabled.

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
| SOC clock | Implemented; hardware validation pending | SMU CurrClock[PPCLK_SOCCLK], MHz |
| Socket power | Implemented; hardware validation pending | SMU AverageSocketPower, whole watts |
| Board power | Implemented; hardware validation pending | SMU AverageTotalBoardPower; confirm board reporting on hardware |
| Edge/hotspot/memory temperature | Implemented; hardware validation pending | SMU AvgTemperature, degrees Celsius |
| Fan speed | Implemented; hardware validation pending | SMU AvgFanRpm and AvgFanPwm |
| VRAM allocated / free | Unavailable; not a firmware load percentage | Driver allocation accounting, with reserved storage separately identified |
| GTT allocated / free | Unavailable | Shared GART allocator accounting; aperture reservation differs from resident host memory |
| Per-process memory and engine use | Unavailable | Per-client BO/queue ownership and scheduler accounting |
| PCIe throughput, per-engine busy and ECC | Unavailable | Separate counters/interfaces, not inferred from this table |

UMC activity describes memory-controller work. It is not the fraction of VRAM
allocated and not a measured bandwidth in GB/s. Similarly, a current GRBM busy bit
is not a time-averaged GPU utilization percentage. The monitor keeps those
concepts separate. Total VRAM in QueryInfo is the driver's usable capacity after
firmware reservation and may be less than the board's marketed capacity.

## Firmware decoder

`dext/amdgpu/amdgpu_metrics.h` is a pure decoder; it issues no commands and does
not map or access GPU memory. It accepts exactly 412 bytes matching the pinned
Linux `SmuMetricsExternal_t`, SMU IP **14.0.3**, and driver interface **0x2e**.
Different interface versions are rejected until their layouts are verified.
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
owner selector 46, observer selector 47 and lifecycle invalidation. Collection
is explicit: neither initialization nor the standalone monitor triggers it.

Collection validates runtime readiness, retained VRAM staging, exact firmware
interface, previous mailbox completion and BAR bounds before its single table-5
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
| status / flags | Last operation status and valid/faulted/stale bits |
| generation / sequence | Reservation's uptime timestamp and successful collection count |
| collectedAtNs / attemptedAtNs | Last successful snapshot and attempted collection, CLOCK_UPTIME_RAW nanoseconds |
| driverInterface / firmwareCounter | Firmware ABI and raw metrics counter |
| validFields / values[16] | Per-field validity and normalized integer values in metrics::Field order |

The generation identifies this boot/session's reservation, not a persistent
hardware serial number. Cached samples should be keyed by registry ID and
generation. The monitor also independently checks version, size, validity flags
and sample age before displaying values.

## Build 176 driver API and lifecycle

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

`amdgpu_mtop` reads 47 on driver build 176+ and shows unavailable for missing,
invalid, failed or stale samples. It never sends 46. The host's Sample Metrics
button performs a one-shot 46 call and immediately reads 47, preserving valid
values in its log. A standalone monitor must read within 2.5 seconds to show a
fresh sample; refreshing its UI does not implicitly refresh firmware.

First hardware validation should use one owner collection and inspect the
returned firmware interface, status, validity and values before any periodic
sampling is introduced. A genuine unsupported interface remains unsupported;
do not loosen layout gating to make numbers appear.

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
Until those barriers are implemented and tested, explicit one-shot collection
is the supported integration path.

The collection, decoder, reservation and actual selector tests passed under
ASan/UBSan. Admission tests verify that cached observers do not open PCI and
collection cannot claim or steal ownership. Shutdown tests verify invalidation
before reset/Close and preservation when shutdown is rejected before starting.
Modified driver and SMU sources passed DriverKit arm64 syntax checks. The
standalone monitor build and identity/validity tests passed. No firmware table
request, GPU reset, power-policy change or installation was performed by these
tests.
