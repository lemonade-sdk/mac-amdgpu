# Native AMD GPU monitoring

`amdgpu_mtop` is a macOS rewrite with a terminal organization inspired by
amdgpu_top: device selection, GPU activity, memory, clocks and sensors. No upstream
UI implementation was copied. A straight build of amdgpu_top would not provide
telemetry here: its backend relies on `libdrm`, `libdrm_amdgpu`, Linux driver
interfaces, sysfs and process fdinfo. The new backend enumerates every
`MacAMDGPU` service and opens each by its IOKit registry identity.

## Implemented and pending

| Statistic | Current monitor | Required source |
| --- | --- | --- |
| Device list and selection | Implemented for MacAMDGPU-bound cards | IOKit registry IDs |
| Responding build and initialization stage | Implemented | Observer selectors 43 and 21 |
| GPU IP label | Implemented as driver-reported configuration | QueryInfo GFX version; currently configured, not fresh hardware discovery |
| VRAM total and CPU-visible capacity | Implemented after initialization | GMC cached sizes |
| GFX activity | Unavailable pending telemetry endpoint | SMU AverageGfxActivity, percent |
| UMC activity | Unavailable pending telemetry endpoint | SMU AverageUclkActivity, percent |
| Media activity | Unavailable pending telemetry endpoint | Maximum of two SMU VCN activity percentages |
| GFX/memory/fabric clocks | Unavailable pending telemetry endpoint | SMU pre/post-deep-sleep averages, MHz |
| SOC clock | Unavailable pending telemetry endpoint | SMU CurrClock[PPCLK_SOCCLK], MHz |
| Socket power | Unavailable pending telemetry endpoint | SMU AverageSocketPower, whole watts |
| Board power | Unavailable pending telemetry endpoint | SMU AverageTotalBoardPower; confirm board reporting on hardware |
| Edge/hotspot/memory temperature | Unavailable pending telemetry endpoint | SMU AvgTemperature, degrees Celsius |
| Fan speed | Unavailable pending telemetry endpoint | SMU AvgFanRpm and AvgFanPwm |
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
Current initialization logs the firmware interface version without retaining
it; that must be fixed before enabling this decoder on a device.

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

## Required driver integration

No new selector is allocated by this foundation; coordinate its number with
the driver ABI when integration is ready.

1. Retain the SMU driver-table MC address, VRAM offset, capacity, firmware
   interface version and validity in the per-device context. Initialization
   already reserves 64 KiB from `PSPContext::fwBuf` and programs the address,
   but currently discards its coordinates. Do not derive them later from the
   current bump pointer, which may advance for other firmware allocations.
2. Treat that 64 KiB as firmware-owned shared staging until a confirmed reset.
   It must not overlap later PSP payloads or be reused by an allocator. A failure
   after programming either half of its address cannot free it while SMU might
   still DMA. Record the reservation before issuing the first address message.
3. Perform telemetry collection on the same serialized device queue and under
   the same mailbox ownership rules as every other SMU operation. The complete
   table-transfer command, acknowledgement, HDP coherency operation and copy
   must be one serialized transaction. An unrelated table command cannot reuse
   staging between the acknowledgement and copy. Do not expose arbitrary table
   transfer or address-setting commands to a monitor client.
4. Only after successful initialization and exact ABI validation, issue existing
   `smu_transfer_table_smu_to_dram(dev, 5)` with argument zero. It is a firmware
   telemetry request and memory write, even though the public monitoring API is
   read-only. Wait for successful firmware acknowledgement before reading the
   bytes; no fixed sleep substitutes for this. A timeout retains the destination,
   invalidates the sample and suppresses repeated collection until recovery.
5. Invalidate/serialize the CPU read path as required by the working BAR0 mapping
   and ported HDP helpers. Use the verified VRAM readback accessor, bounds checked
   against the reserved slot and visible aperture. Do not assume direct CPU
   `memcpy` from VRAM is coherent on arm64. Copy all 412 bytes to owned CPU storage
   before another table operation can use staging.
6. Decode and publish a fixed-size, versioned cached snapshot with a validity
   mask, host monotonic collection time, firmware counter, last error and reset
   generation. A read-only observer selector should only copy this snapshot;
   it must not call `ensure_open`, acquire the hardware session or trigger SMU
   commands. Register it in every admission/lifecycle allowlist that already
   treats selectors 21 and 43 as observers.
7. Start with at most one sample per second per initialized device, shared by
   all observers. No polling during bringup, shutdown, detach, reset, failed
   mailbox recovery or suspended state. Stop/invalidate collection before
   teardown. Return not-ready/unavailable with timestamps instead of presenting
   a cached sample as current. Repeated host timestamps alone do not prove the
   firmware is still updating; compare counters without assuming their unit or
   increment rate.
8. Extend the standalone transport and dashboard after that endpoint passes a
   bounded hardware test. Read-only monitoring must coexist with the host's
   owning connection, and disconnecting the monitor must leave the GPU running.

Device selection uses the registry ID for the active attachment, never “the
first matching GPU” after the user has selected one. Reordering does not switch
the selected card. Removal and reconnect produce an explicit disconnected/new
attachment state. A blocked/erroring card must not erase other cards' results.

## References

- [amdgpu_top primary repository and backend dependencies](https://github.com/Umio-Yasuno/amdgpu_top)
- [Linux AMDGPU GPU metrics documentation](https://docs.kernel.org/gpu/amdgpu/thermal.html#gpu-metrics)
- Local Linux reference commit: `1f63dd8ca0dc05a8272bb8155f643c691d29bb11`.

The UI/backend foundation was compiled and tested offline. No firmware table
request, GPU reset, power-policy change or installation is part of these tests.
