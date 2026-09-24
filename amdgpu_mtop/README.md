# amdgpu_mtop

A native macOS terminal monitor for GPUs bound to MacAMDGPU. Its GPU-only
layout takes inspiration from [amdgpu_top](https://github.com/Umio-Yasuno/amdgpu_top):
device selection, activity histories, memory, clocks and sensors, with a native
IOKit backend. It does not require Linux DRM, sysfs, Rust, HSA or a graphics
runtime.

Build 193 adds software counters, 60-second graph histories and bounded live
sensor sampling. **A live build-193 run recorded 156 fresh sensor reads in 163 monitor samples
while the 1024-operation mailbox test passed concurrently.** Those reads represent
16 distinct firmware captures; cached dashboard refreshes are not independent
sensor measurements. Independent sensor accuracy and an initialized idle/load
comparison remain pending. Older drivers still provide the fields
they support. Missing, failed, stale or unsupported values appear as
unavailable/null rather than fabricated zero readings.

## Build and controls

```sh
cmake -S amdgpu_mtop -B build/amdgpu_mtop -DCMAKE_BUILD_TYPE=Release
cmake --build build/amdgpu_mtop --parallel 4
build/amdgpu_mtop/amdgpu_mtop
```

- `h`: toggle **Slow 500 ms** (default) and **Fast 100 ms** dashboard refresh.
- `n` / `p`: select the next / previous GPU.
- `q` or `Ctrl-C`: quit and restore normal terminal input.

The dashboard adapts to terminal size. Histories cover the last 60 seconds with
peak values per time bucket; changing refresh speed does not change their time
axis. Dots identify missing samples. Software rates use actual monotonic elapsed
time and reset their baseline when the counter generation changes.

```sh
build/amdgpu_mtop/amdgpu_mtop --fast
build/amdgpu_mtop/amdgpu_mtop --list
build/amdgpu_mtop/amdgpu_mtop --device 0x10033939d --slow
build/amdgpu_mtop/amdgpu_mtop --json
build/amdgpu_mtop/amdgpu_mtop --json --watch --fast
```

Use a registry ID from `--list`; the example ID is not a permanent GPU identity.
Selection follows that registry entry rather than its enumeration index. Removal
leaves it disconnected until another card is selected; replugging creates a new
registry ID. JSON includes all matching devices and `selected_registry_id`.
Non-terminal output and `--json` default to one snapshot; `--watch` repeats at
the selected 100/500 ms cadence. Firmware sampling remains at most once per
second regardless of output cadence.

## What the values mean

The software panel shows driver-observed submissions, completions, failures,
pending work, AQL packet publication/consumption, and known completed copy
payload bytes by engine/direction. Successful CPU BAR uploads/readbacks have
separate byte counters. These counters persist across clean GPU stops for the
bound driver's lifetime, so short jobs remain visible after their client exits.
Unobserved outstanding work is retired on verified shutdown rather than counted
as completed; failed shutdown preserves it. Rejected calls do not count as
submissions. Copy bytes describe tracked payload, not all PCIe traffic.

Software pending time and submission rates are **not hardware GPU utilization**.
Persistent AQL packet consumption is also distinct from kernel completion.
Hardware GPU/UMC activity, when available, comes from the SMU metrics table.
UMC activity measures memory-controller work; it is not allocated VRAM or a
measured bandwidth value.

The **SMU REPORTED GFX** history uses a fixed **0–100%** scale. Earlier versions
scaled this percentage to the recent peak, making low activity appear full.
Unavailable activity now leaves this same chart unavailable; it is never replaced
by a submissions-rate chart. Driver-observed jobs/packets per second remain in
the separate WORK panel. JSON retains `gfx_activity_percent` and declares its
`gfx_activity_source` and `gfx_activity_scope`.

The percentage is **not calibrated productive GPU utilization**. In the
controlled driver 195 owned-idle diagnostic, all 11 observations over 10 seconds
had GRBM_STATUS 0x382c (GUI/ANY/CP busy clear), CP_STAT 0, zero queues and no
pending/new work; ten distinct fresh firmware sequences still reported97%
initially and 100% afterward. No resident signal mailbox was started. Stop
completed and stage0 was verified. See
`build/tests/driver 195-hardware/initialized-idle-diagnostic.jsonl`.

The IF 0x33 profile therefore displays **“idle can report 100%; workload utilization
unverified”** and JSON adds `gfx_activity_accuracy`. Raw firmware percentages
remain visible; the monitor does not invent zero or subtract a housekeeping
estimate. Resident service work can contribute in other sessions, but it does
not explain this controlled idle capture. The omitted Linux GFX clock-gating
initialization path is under investigation; it is not a confirmed cause.

The memory panel separates CPU-visible and GPU-only allocator capacity, used,
free and largest contiguous free span. Used bytes include rounded client and
driver allocations, including retained failed-work storage. “Outside pools”
identifies fixed reservations and gaps, not measured firmware memory use.
Allocator accounting is independent of the sensor profile and is unavailable
while the GPU is stopped. A valid empty pool reports zero.

Clock fields distinguish:

- **Raw/current MHz:** the firmware table's `CurrClock[]` for GFX, SOC, memory
  and fabric. Raw GFX stayed at 1000 MHz in the first run while its selected
  average changed; Linux's exported GPU-metrics `current_gfxclk` deliberately
  uses the selected average instead. Treat raw GFX as a firmware field, not an
  independently validated instantaneous clock.
- **Average MHz:** Linux's pre/post-deep-sleep GFX, memory and fabric averages.
- **Raw peak:** the highest sampled raw clock field seen by this monitor, not
  an independently measured maximum.
- **DPM minimum / AC DPM maximum:** firmware-advertised clock ranges, queried
  once per initialized session; these are not current throttling caps or a
  guarantee that the GPU will sustain the maximum.

Power, temperatures, fan speed and activity retain per-field validity. The
installed SMU 14.0.3 firmware reports interface **0x33**, while Linux's published
consumer uses **0x2e**. Build 193 implements an explicitly labeled Linux-compatible
profile only for live firmware version **0x00684c00 (104.76.0)**, using Linux's
backward-compatibility behavior and conservative plausibility checks. It does
not claim a newly verified 0x33 schema. Other firmware mismatches remain
unsupported before table transfer.

JSON uses explicit units and `null` for unavailable values. `software_stats`,
`vram_accounting` and `clocks` expose their separate counters, values and status.
Legacy top-level `gfx_clock_mhz`, `memory_clock_mhz` and `fabric_clock_mhz` are
averages; the nested `clocks` object uses `raw_current_mhz` and separate DPM limit keys.
`vram_used_bytes` remains null because total hardware occupancy, including
firmware-private memory, is not measured.

## Sampling and lifecycle

The tool requires driver build 172+ and permission to open its user client.
Cached information uses selectors 43/21, firmware metrics 47 (build 176+), software
counters 61 and clocks 62 (build 193+). On build 193+, the monitor requests bounded
sensor sampling through selector 63 at most once per second for each already
initialized device. The driver shares a one-second success cache across
observers. Fast 100 / Slow 500 ms refreshes read that cache and CPU-side counters;
they do not increase firmware request frequency.

The first qualified sample reads the firmware release, transfers metrics table 5
into retained staging and queries clock ranges. Subsequent samples transfer only
the metrics table. The existing mailbox operations have two-second timeouts and
BAR readback has an elapsed-time budget; a slow first request can delay a UI
refresh. A timeout or invalid table latches sampling off until verified reset,
preventing repeated timeout loops. Samples older than 2.5 seconds become stale.

The monitor never opens a new PCI hardware session, claims GPU ownership,
initializes, changes power policy, submits compute work or resets the device.
Sampling runs on the driver's existing serialized lifecycle queue, reuses its
reserved staging buffer and skips tracked pending raw submissions. Closing an
observer does not stop another client's session. Cards bound to Apple's driver,
or not bound to any driver, are outside this backend's coverage.

## Offline checks

```sh
bash scripts/test-amdgpu-mtop.sh
bash scripts/test-software-stats.sh
bash scripts/test-metrics.sh
bash scripts/test-metrics-rpc.sh
bash scripts/test-client-lifecycle.sh
bash scripts/test-vram-accounting.sh
```

The decoder tests require this repository's local `upstream/linux` reference.
These tests do not access the GPU. Driver integration, exact ABI details,
firmware provenance and validity rules are in
[GPU_MONITOR.md](../docs/GPU_MONITOR.md).
