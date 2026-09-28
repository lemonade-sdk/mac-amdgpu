# HRX compile, transfer and latency benchmarks

Measured 2026-09-27 (Pacific time). All four test categories completed with checked outputs and clean GPU retirement.

**System:** MacBook Pro (Mac17,6), Apple M5 Max, 128 GB memory; macOS 26.6.2 (25G83); gfx1201, driver 204, 31.86 GiB reported VRAM.

**Clock:** GPU timestamp frequency 100,000,000 Hz. Host intervals use a monotonic clock. No engine workload was running; amdgpu_mtopg remained open.

**Source:** mac_amdgpu `fde050c1b8a98a73ffa3cdc8ea2c1c95fe8a6531`; LSE `0f2ad2977d4ed54edaab04fdb50041c399042f25`; Homebrew clang version 21.1.8.

## Discord copy/paste

```text
HRX / gfx1201 | M5 Max / 128 GB | macOS 26.6.2 | driver 204
Test                       | Median      | Details
---------------------------+-------------+------------------------
Loom compile: Q4 M1        | 4.638 ms    | 400 lines / 19.86 KiB
Loom compile: Q4 M512      | 17.966 ms   | 1,733 lines / 95.34 KiB
Host -> VRAM sustained     | 6.040 GB/s  | max 6.067 GB/s; 96 MiB
VRAM -> Host sustained     | 6.628 GB/s  | max 6.631 GB/s; 96 MiB
HRX kernel issue           | 67.896 us   | P95 146.415 us
HRX kernel completed       | 1571.625 us | P95 1645.956 us
Bare HSA kernel completed  | 182.853 us  | P95 314.846 us
Minimal kernel: GPU time   | 5.080 us    | P95 5.762 us
Host -> VRAM: 8 B complete | 1556.479 us | P95 1657.937 us
VRAM -> Host: 8 B complete | 1573.229 us | P95 1685.011 us
CPU -> GPU -> CPU mailbox  | 12.583 us   | P95 14.375 us
GPU -> CPU -> GPU mailbox  | 10.680 us   | P95 11.120 us
Compile: warm uncached. GB/s: decimal. Mailbox: round trips, not one-way.
HRX completed timings include the default 1000 us blocked-poll policy.
```

## 1. Kernel compile time

Actual LSE Loom source-to-gfx1201-code-object compilation, with seven repeated uncached compilations per kernel. The synthetic Q4 fixtures use explicit INT8 selection. First call is per compiler instance; the M512 case runs later in the same process. Model loading, disk-cache lookup and GPU execution are excluded.

| Kernel | Lines | Nonblank lines | Source bytes | Code-object bytes | First call | Warm median | Warm P95 |
|---|---:|---:|---:|---:|---:|---:|---:|
| q4_m1 | 400 | 400 | 20,332 | 9,208 | 20.392 ms | 4.638 ms | 4.713 ms |
| q4_m512 | 1,733 | 1,733 | 97,629 | 13,304 | 18.979 ms | 17.966 ms | 18.189 ms |

Both representative Q4 kernels use WG256; grid X is 2,176 for M1 and 1,088 for M512, with (N,K)=(17,408,5,120). The timing measures compilation of each complete emitted kernel.

The separate OpenCL latency fixture has 59 total lines / 57 nonblank lines, 2,106 source bytes and 10,992 code-object bytes. Its external Clang+LLD process time is 148.727 ms first call and 52.560 ms warm median (three calls); these process-startup timings differ from the in-process Loom measurements.

## 2. Host to device and 3. Device to host

Headline transfer rates use eight distinct 12 MiB regions per endpoint (96 MiB aggregate working set), with fresh patterns per trial. One warmup precedes three timed batches per direction. Buffers are mapped coherent host/GTT memory and DEVICE_LOCAL VRAM. Timing includes record, flush and completed retirement; allocation, seeding and full payload/guard validation are outside timing.

| Direction | Working set | Regions/batch | Median GB/s | Minimum GB/s | Maximum GB/s | Trials |
|---|---:|---:|---:|---:|---:|---:|
| H2D | 96 MiB | 8 × 12 MiB | 6.040 | 6.026 | 6.067 | 3 |
| D2H | 96 MiB | 8 × 12 MiB | 6.628 | 6.602 | 6.631 | 3 |

The driver currently exposes a 256 MiB GART/shared-memory aperture. The distinct-region test uses one 96 MiB mapped host endpoint, reused after completion for checked readback, plus small guards and runtime overhead. A requested 512 MiB endpoint could not be allocated; those setup failures occurred before timed transfers and are preserved with the raw evidence. No driver changes were made.

### Original repeated-region size sweep

The initial sweep reuses the same region for eight copies per batch. Its 4 MiB H2D rate is cache-sensitive and is not used as a physical transfer headline. All outputs and guards pass, but logical copied bytes can exceed physical bus traffic. AMD lists 64 MiB Infinity Cache and 8 MiB L2 for the R9700; the 96 MiB distinct working set exceeds their sum. [AMD hardware specifications](https://rocmdocs.amd.com/en/docs-7.2.4/reference/gpu-arch-specs.html).

| Direction | Payload/copy | Copies/batch | Median GB/s | Minimum GB/s | Maximum GB/s | Trials |
|---|---:|---:|---:|---:|---:|---:|
| H2D | 4 MiB | 8 | 21.226 | 12.559 | 21.949 | 3 |
| H2D | 16 MiB | 8 | 6.310 | 6.308 | 6.499 | 3 |
| H2D | 64 MiB | 8 | 6.664 | 6.657 | 6.678 | 3 |
| D2H | 4 MiB | 8 | 5.468 | 5.430 | 5.982 | 3 |
| D2H | 16 MiB | 8 | 6.790 | 6.787 | 6.836 | 3 |
| D2H | 64 MiB | 8 | 7.056 | 7.054 | 7.062 | 3 |

The headline uses the distinct-region batch median. These are current HRX buffer-copy path measurements, not a theoretical bus limit or an SDMA-only measurement. GB/s = 1,000,000,000 bytes/s; MiB = 1,048,576 bytes.

## 4. Kernel launch and communication latency

| Measurement | Median us | P95 us | Minimum us | Samples | Clock |
|---|---:|---:|---:|---:|---|
| HRX dispatch recording | 2.438 | 4.631 | 1.209 | 200 | host_steady |
| HRX recording + flush return | 67.896 | 146.415 | 46.625 | 200 | host_steady |
| HRX dispatch to observed completion | 1571.625 | 1645.956 | 1075.500 | 200 | host_steady |
| HRX wait after issue returns | 1507.917 | 1519.388 | 1017.000 | 200 | host_steady |
| Bare HSA packet publication + doorbell return | 68.208 | 212.417 | 53.084 | 200 | host_steady |
| Bare HSA dispatch to observed completion | 182.853 | 314.846 | 148.791 | 200 | host_steady |
| Minimal kernel CP interval | 5.080 | 5.762 | 4.080 | 200 | gpu_agent |
| 8-byte H2D copy to completion | 1556.479 | 1657.937 | 1070.417 | 256 | host_steady |
| 8-byte D2H copy to completion | 1573.229 | 1685.011 | 1121.333 | 256 | host_steady |
| CPU→GPU→CPU persistent mailbox RTT | 12.583 | 14.375 | 6.208 | 3,072 | host_steady |
| GPU→CPU→GPU persistent mailbox RTT | 10.680 | 11.120 | 3.560 | 3,072 | gpu_agent |

The original latency run uses the default blocking wait policy: `MAC_HSA_BLOCKED_POLL_US` is unset, giving a 1,000 us HSA blocked-poll interval. HRX timeline completion uses that blocked wait; its roughly 1.5 ms completion observation includes the wait policy. Bare HSA completion and mailbox tests actively poll. No wait-related MAC_HSA/IREE/HRX/LSE environment override was set.

HRX/HSA kernel tests use one wave and a checked 4-byte store to shared host memory; 32 warmups precede 200 measured dispatches. Small-copy tests use 32 warmups and 256 samples per direction. GPU CP timing comes from a separate bare-HSA profiled test and does not include host submission or completion polling.

Pure communication uses one persistent mailbox kernel: 128 warmup round trips and 1,024 measured round trips per trial, three trials per initiator. It excludes a fresh kernel launch or bulk-copy command per round trip. Payload sequence, reply contents, guards and kernel retirement are checked. GPU realtime intervals and CPU intervals each remain within one clock domain.

**Exact one-way CPU→GPU and GPU→CPU latency is not established by these tests.** Both mailbox measurements include the outward request, response and polling. Their difference reflects initiation, memory paths and timing boundaries; dividing either by two would assume symmetric directions.

P95 uses linear interpolation at sample position (n-1)×0.95. Small completion tests and active mailbox polling expose different runtime paths. No profiler or model inference runs concurrently.

## Reproduction and raw evidence

Use the portable [benchmark tools](../../../tools/hrx_system_tests/README.md) for build and bounded live-run commands. The sources below preserve the measured fixture identities. Machine-specific repository prefixes in metadata are replaced with relative paths; numeric results and hashes are unchanged. Raw measured data:

- [compile.csv](compile.csv), [bandwidth.csv](bandwidth.csv), [bandwidth-disjoint.csv](bandwidth-disjoint.csv), [latency.csv](latency.csv)
- [results.json](results.json), [environment.json](environment.json), [gpu.json](gpu.json)
- [build-metadata.json](build-metadata.json), [opencl-compile.json](opencl-compile.json), [wait-policy.json](wait-policy.json)
- [Distinct-region source](bandwidth-disjoint.cpp), [distinct-region results](bandwidth-disjoint.json)
- [Compiler harness](compile.cpp), [native harness](native.cpp), [latency kernels](latency.cl)

### Kernel source identities

| Source | SHA-256 |
|---|---|
| [q4_m1.loom](q4_m1.loom) | `572ac498697e34da9affc9507aa5d4edd5e29232cac5019be84b03fbfeb14917` |
| [q4_m512.loom](q4_m512.loom) | `2f8843ef4151de2c2c8f90fa26afa3ea724177928c1390222afc10d8b38a0da4` |
| [latency.cl](latency.cl) | `3d710428553285cbe7b95458e9c0ba57898722fd8e8b7b0b57cb80ae33efaaaa` |
