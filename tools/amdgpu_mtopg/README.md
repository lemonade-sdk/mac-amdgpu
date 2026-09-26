# amdgpu_mtopg

macOS SwiftUI app: the 2D companion to the terminal `amdgpu_mtop` monitor.
Same driver, same telemetry, real vector drawing instead of braille.

## What it shows

- **GPU CORE LOAD** — live 60 s rolling line/area chart (SwiftUI Canvas,
  10 Hz) of driver dispatch-in-flight utilization (software_stats selector
  61), with grid, axis labels and the current % in the panel header.
  The SMU AverageGfxActivity field is incoherent on this host and is never
  plotted.
- **UMC MEMORY ACTIVITY** — second chart, same treatment. Source is the
  MMHUB PERFSTATUS hardware PERFCTR delta (selector 68, driver build 198+).
  The SMU UmcActivityPercent field is in the same unqualified firmware
  table as AverageGfxActivity (incoherent: it moves at verified idle and
  reads 0% under real traffic), so it is never plotted. Blank with a
  caption while no coherent source has samples.
- **VRAM / GTT** — driver CPU allocator pools (query tag 5).
- **Clocks (SMU)** — GFX/SOC/MEMORY/FABRIC current MHz with DPM min–max
  (selector 62).
- **Sensors (SMU)** — power, edge/hotspot temperature, fan (selector 47).
- **Engines** — per-engine dispatch-in-flight busy strip (SDMA0/SDMA1/GFX/
  AQL from selector 61; VCN/JPEG have no observer counter and say so).

Every panel is captioned with its source; no value is displayed from a
source that is not actually producing samples.

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
| 21 | QueryInfo tags 1 (gfx version), 2 (VRAM), 4 (stage), 5 (VRAM accounting), 8 (GFXSpec chip geometry) |
| 47 | SMU metrics snapshot (192 B) |
| 61 | software_stats snapshot (456 B) — dispatch-in-flight counters |
| 62 | SMU clock snapshot (96 B) |
| 68 | MMHUB PERFSTATUS UMC busy (4 x u64; build 198+ only) |

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
