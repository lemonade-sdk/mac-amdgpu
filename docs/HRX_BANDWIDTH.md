# HRX bandwidth on macOS

This test uses the real pinned HRX C API and the MacAMDGPU HSA adapter. It does
not use HIP. It measures transfers independently of model loading or inference.

## Reproduce

Build the HSA runtime and pinned HRX libraries using the normal project build,
then build this utility:

```sh
bash scripts/build-hrx-bandwidth.sh
```

The build runs `--check-host`, which validates the pattern, poison, and guard
oracle without opening the GPU. Stop other GPU workloads before running:

```sh
MAC_HSA_BLOCKED_POLL_US=64 python3 scripts/run-hrx-bandwidth.py \
  --hsa-library-dir build/hsa-wait-perf
```

The isolated runtime build is described in [LOCAL_RUN.md](../LOCAL_RUN.md#experimental-resident-benchmark); a freshly rebuilt normal HSA runtime also supports the override.

The wrapper records binary/runtime hashes, writes `build/tests/hrx-bandwidth.log` and imposes a 180-second process
deadline. `--log PATH`, `--timeout-seconds 10..600`, and `--hsa-library-dir PATH`
are optional. The runtime directory defaults to `build/hsa`; the example selects
the runtime used for the validated 64-microsecond polling results below. The
wrapper records executable/runtime paths, SHA-256 hashes, and polling setting. A process
termination does not establish GPU retirement; inspect the driver before another
workload if the wrapper reports a timeout. The direct executable is also available:

```sh
DYLD_LIBRARY_PATH="$PWD/build/hsa" \
  build/hrx-macos-adapter/mac-hrx-bandwidth --run
```

Only `--run` opens the GPU. The utility uses one device and returns an error if
multiple devices are enumerated. Normal completion waits for stream retirement,
unmaps and releases buffers, shuts down HRX, and prints `clean-shutdown`.

## What is measured

The main sweep transfers 4 KiB, 64 KiB, 1 MiB, and 16 MiB in each direction:

- **H2D:** a reusable mapped host buffer to a `DEVICE_LOCAL` buffer.
- **D2H:** a `DEVICE_LOCAL` buffer to a reusable mapped host buffer.
- **D2D:** between two distinct `DEVICE_LOCAL` buffers.

This uses `hrx_stream_copy_buffer`, matching the GPU transfer portion of LSE's
reusable staging path. It excludes the application's CPU memcpy into/out of that
staging memory. CPU accesses occur before submission or after confirmed GPU
retirement; the test requires no concurrent CPU/GPU read-modify-write atomic.

Each size and direction gets one warm-up followed by three measured batches of
eight copies. The reported summary uses the median batch time. Timing uses the
host steady clock and includes recording, flush, and completion waiting. Allocation,
initialization, poisoning, and verification are outside the timed interval. The
stream timeline wait has a 30-second timeout. Initialization and the convenience
APIs do not expose equivalent finite timeouts; the process wrapper bounds those.

Rates use **decimal payload GB/s**: `payload_bytes × copies / seconds / 1e9`.
Gb/s is eight times that number. D2D additionally reports twice the payload rate
as the nominal read-plus-write traffic convention. This second number is not a
hardware memory-controller measurement. Copies reuse the same buffers, cache
residency is unverified, and small batches are dominated by submission/waiting
latency. These are effective HRX API rates, not raw VRAM or PCIe link bandwidth.
In particular, repeated 1 MiB copies can report more payload per second than the
physical host link can carry: cached reuse prevents treating that as link speed.

The pinned HRX path uses **compute blit kernels, not SDMA**. It records one AQL
COPY packet in `aql_command_buffer.c`, lowers it through
`aql_block_processor.c`, and calls `device_buffer_copy_emplace` in
`runtime/src/iree/hal/drivers/amdgpu/device/blit.c`. The test's aligned addresses
and sizes select `device_buffer_copy_block_x16`, which copies with 16-byte vector
loads/stores. The source explicitly reserves SDMA emission for a separate
queue-specific wrapper.

Every trial uses a changed deterministic byte pattern and a poisoned destination.
Verification checks the entire payload, prefix/suffix guards, and unchanged source
contents. Each buffer has 4 KiB guards at both ends. Allocation size is queried
before access. A failed retirement retains the HRX objects for process/driver
teardown instead of freeing potentially live storage.

A separate 1 MiB test calls `hrx_stream_copy_h2d` and
`hrx_stream_copy_d2h` directly. These convenience APIs have different overhead:
the pinned implementation chunks H2D into 63 KiB synchronous transfers and D2H
into up to 4 MiB synchronous transfers. Their measurements include that behavior;
they are not interchangeable with reusable mapped-buffer results.

## Measured result

Driver 195, Radeon AI PRO R9700/gfx1201, September 23, 2026 local time.
The final repository utility passed through `scripts/run-hrx-bandwidth.py` using
`build/hsa-wait-perf` with **64 microseconds of blocked-wait polling**. Runtime
SHA-256: `244f3943c333a307a65e62a9992683d65c1b4a089ce66114b4952bc4ae33c1b0`.
The run exited 0 with a clean shutdown. Every payload, guard, allocation-capacity,
and unchanged-source check passed.

| Payload per copy | H2D GB/s | D2H GB/s | D2D payload GB/s |
| --- | ---: | ---: | ---: |
| 4 KiB | 0.1656 | 0.1691 | 0.1585 |
| 64 KiB | 2.6569 | 2.6835 | 2.6766 |
| 1 MiB | 22.8521 | 22.5677 | 43.5207 |
| 16 MiB | 6.7917 | 7.0377 | 290.2789 |

At 16 MiB, the effective host-copy rates correspond to **54.33 Gb/s H2D** and
**56.30 Gb/s D2H**. The D2D read-plus-write convention gives **580.56 GB/s**;
its eight-copy batch took about 0.462 ms including host overhead. Reused buffers
and unmeasured cache residency mean neither this D2D figure nor the 1 MiB host
figures establish raw memory/link bandwidth.

The separate single-copy 1 MiB convenience results were **0.3138 GB/s H2D**
(3.34 ms) and **0.2875 GB/s D2H** (3.65 ms). These differences expose actual
API staging/chunking costs; they do not indicate different physical link speeds.

The final hardware artifact is
`build/tests/driver195-hardware/hrx-bandwidth-repro.log`. Its first JSON record
identifies the exact executable and HSA library. The original prototype artifact,
`build/hrx-bandwidth/run.log`, used an older `build/hsa` library with fixed
**1,000-microsecond polling**. That older binary ignored
`MAC_HSA_BLOCKED_POLL_US=64`; recording the environment setting alone did not
establish the runtime's effective policy. It measured 6.74/6.75 GB/s host copies
and 104.40 GB/s D2D at 16 MiB. This is not a controlled one-change A/B: the final
utility also added allocation-capacity queries and host-oracle checks, while
retaining the same timed copy loop. The comparison illustrates why effective
API throughput must identify both runtime and waiting behavior.

An earlier attempt to extend the host-mapped working buffers to 128 MiB returned
`HSA_STATUS_ERROR_OUT_OF_RESOURCES` during allocation and shut down cleanly. It
is preserved in `build/hrx-bandwidth/large-allocation-run.log`. The final utility
stays within the validated 16 MiB payload size; this is a test bound, not a claim
about the maximum GPU-local allocation size.
