# amdgpu_mtopg

macOS SwiftUI app: the 2D companion to the terminal `amdgpu_mtop` monitor.
Same driver, same telemetry, real vector drawing instead of braille.

## What it shows

- **GPU CORE LOAD** — with driver build 203+, a 60 s chart of the rolling
  fraction of `GRBM_STATUS.GUI_ACTIVE` samples (selector 71). This estimates
  GFX active time at the 10 Hz poll cadence, not CU occupancy or shader busy
  cycles. Older drivers use GFX submitted-packet rate from selector 61 scaled
  to the peak observed in this session; that fallback is an activity proxy.
- **UMC MEMORY ACTIVITY** — unavailable on this GPU. The decoded SMU 0x33
  UmcActivityPercent field has reported activity at idle and near zero under
  verified traffic, so it appears only in diagnostic text. The former
  selector 68 MMHUB PERFSTATUS address is unmapped on gfx1201; build 199+
  reports it unavailable rather than a live UMC busy counter.
- **VRAM / GTT** — driver CPU allocator pools (query tag 5).
- **Clocks (SMU)** — raw firmware `CurrClock[]` for SOC, memory and fabric,
  plus advertised AC DPM min–max. GFX shows the fresh SMU average at both
  initialized idle and load, clearly labeled as an average; its raw
  `CurrClock[]` field remains visible below. On the uncalibrated 0x33 profile,
  raw GFX has stayed at 1000 MHz during load, while its average has risen
  above 3 GHz at idle. Neither should be mistaken for a separately verified
  instantaneous core frequency. An average outside the advertised DPM
  maximum is flagged.
  DPM levels are advertised AC operating states; deep-sleep averages can fall
  below their minimum.
- **Sensors (SMU)** — firmware GFX activity, socket/board power,
  edge/hotspot temperature and fan. The installed 0x33 firmware profile is not
  independently calibrated: GFX activity has read 100% at initialized idle
  and decoded power has stayed near 300 W while workload activity changed.
  The decoded activity and power fields remain visible in the original meter
  bars with concise `SMU raw` labels, separate from the
  hardware-sampled GPU Load chart.
  An enclosure AC wattmeter can check the idle-to-load input-power change,
  though its reading includes PSU and enclosure losses and is not GPU board
  power.
  [Linux SMU 14.0.2](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/swsmu/smu14/smu_v14_0_2_ppt.c)
  maps GPU load to `AverageGfxActivity` and average socket power to
  `AverageSocketPower` under driver interface 0x2e. This GPU's firmware
  advertises 0x33, and the idle/load readings have not established an
  equivalent calibration.
- **Engines** — per-engine submitted-packet rates scaled to observed peaks
  (SDMA0/SDMA1/GFX/AQL from selector 61). HSA dispatches can be outside these
  counters; an empty row means no packet observed by this endpoint, not that
  the GPU was idle. VCN/JPEG have no observer counter.

Readouts identify their source; no value is displayed from a
source that is not actually producing samples.

SMU values require an initialized GPU session (driver stage 15). The monitor
calls the bounded observer sensor sampler (selector 63) at most once per
second, then reads the cached metrics and clock snapshots. At stage 0 the
SMU readings are unavailable.

## Build

```sh
tools/amdgpu_mtopg/build.sh            # -> tools/amdgpu_mtopg/build/amdgpu_mtopg.app
tools/amdgpu_mtopg/build.sh --clean    # rebuild from scratch
```

Plain `swiftc` against the macOS SDK; system frameworks only (SwiftUI,
AppKit, IOKit, CoreFoundation). The bundle is adhoc code-signed
(`codesign --force -s -`), no entitlements, no network.

## Run

```sh
open tools/amdgpu_mtopg/build/amdgpu_mtopg.app
```

Requires a driver build >= 172 installed (the MacAMDGPU system extension).
Esc or window close quits; the IOKit iterator and per-sample connections
are released on exit (the connection is opened and closed every refresh,
never held).

## Data source

The same read-only IOKit observer client the terminal monitor uses
(`amdgpu_mtop/transport.cpp`): `IOServiceGetMatchingServices("MacAMDGPU")`,
per-refresh `IOServiceOpen` / `IOConnectCallScalarMethod` /
`IOConnectCallStructMethod` / `IOServiceClose`. Selector protocol:

| selector | payload |
|---|---|
| 43 | identity: magic, 1, driver build |
| 21 | QueryInfo tags 1 (gfx version), 2 (VRAM), 4 (stage), 5 (VRAM accounting) |
| 47 | SMU metrics snapshot (192 B) |
| 61 | software_stats snapshot (456 B) — per-engine dispatch counters |
| 62 | SMU clock snapshot (96 B) |
| 63 | bounded SMU sensor-cache refresh (3 x u64; stage 15) |
| 68 | unavailable MMHUB UMC source on gfx1201 (4 x u64; build 198+) |
| 69 | workload SQ busy-cycle slot (5 x u64; build 200+) |
| 70 | driver SQ busy-cycle sample (5 x u64; build 200+) |
| 71 | passive GRBM_STATUS sample (3 x u64; build 203+) |
| 72 | cached, allowlisted raw SMU fields for schema diagnostics (208 B struct; working build 204+) |
| 73 | GFXSpec chip geometry (128 B struct; working build 204+) |

The Swift struct endpoints are decoded from raw byte buffers at the C
offsets (Swift's layout of C++ mirror structs is not reliable); the C
sizes are asserted by the dext headers.

## Files

- `Sources/GPUDriver.swift` — IOKit transport + ABI + validators.
- `Sources/MonitorModel.swift` — rolling history, honest rate math, snapshot.
- `Sources/Views.swift` — SwiftUI Canvas charts + panels.
- `Sources/App.swift` — app/delegate + 10 Hz sampler thread.
- `build.sh` — build + bundle + sign.

MIT, same license as the repository.
