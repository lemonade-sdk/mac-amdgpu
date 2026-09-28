# HRX system tests

Four bounded benchmark categories for the working macOS gfx1201 HRX/HSA stack:

| Category | Measurement |
|---|---|
| Kernel compilation | Actual LSE Loom compilation of complete Q4 M1/M512 kernels; first invocation and seven uncached warm calls, source lines/bytes and code-object bytes. |
| Host → device | Sustained mapped GTT → DEVICE_LOCAL VRAM copies across eight distinct 12 MiB regions. |
| Device → host | Sustained VRAM → mapped GTT copies across eight distinct 12 MiB regions. |
| Launch and communication latency | HRX recording/issue/completion, bare HSA issue/completion and GPU CP interval, 8-byte directional copy completion, and persistent CPU/GPU mailbox round trips. |

The distinct transfer working set is **96 MiB per endpoint**. One host endpoint is reused for checked readback after its source is verified; this fits the current 256 MiB GART aperture. Direct public HRX allocations avoid stream-pool slab reservations. Each batch visits eight different regions once. Full payloads, source preservation and 4 KiB guards are verified outside timing. The original 4/16/64 MiB repeated-region sweep is also retained as a diagnostic; its cached logical copy rates are not physical bus throughput.

## Prerequisites

- macOS/arm64, installed gfx1201 driver and a working HSA runtime.
- Existing **Ninja LSE build** with `lse`, `liblse_*` archives, HRX and Loom enabled. Its compile/link commands supply matching headers, definitions and static libraries for the compiler fixture.
- HRX source/build containing `hrx_runtime.h`, `libhrx.dylib` and `libloomc.dylib`.
- AMDGPU LLVM tools (`clang`, `clang++`, `llvm-readelf`, `llvm-objdump`), `ld.lld`, Python 3, Ninja and Xcode command line tools.
- Exclusive GPU use during live measurements. Stop inference and other compute workloads first.

Build output, binaries, code objects, emitted sources, raw CSV and reports default to ignored `build/benchmarks/hrx-system-tests`. No model loading or perplexity test is involved.

## Build and run

```sh
python3 tools/hrx_system_tests/build.py \
  --lse-build build/lse-macos-adapter \
  --hrx-source build/hrx-macos-source \
  --hrx-build build/hrx-macos-adapter \
  --runtime-dir build/hsa-code-cache-fix/runtime-default \
  --llvm-bin /opt/homebrew/opt/llvm@21/bin

python3 tools/hrx_system_tests/run.py --run
python3 tools/hrx_system_tests/report.py
```

`build.py` is offline. It builds all native fixtures and measures compilation without GPU initialization. To verify only compilation/linking, add `--skip-compile-measurements`; that does not execute the compiler benchmark or timed compile loops.

All entry points support `--help`. Pass the same `--output-dir` to build, run and report. `--runtime-dir` chooses the live HSA library; loaded HRX/Loom/HSA library paths and hashes are recorded. `--lld` selects a linker when it is absent from the LLVM directory and `PATH`. The native fixture is linked using the LSE build's matching compiler configuration; `--llvm-bin` selects the standalone host/GPU toolchain.

Live GPU access requires the explicit `--run` flag. Each native process has a default 180-second outer deadline, and waits inside the fixture are bounded. Failure stops the sequence. Timeout termination does not establish GPU retirement; inspect the log and driver state before another live run.

For only the distinct-region transfer check:

```sh
python3 tools/hrx_system_tests/run-disjoint.py --run
```

To summarize existing checked data without GPU work:

```sh
python3 tools/hrx_system_tests/run.py --summarize-only --output-dir build/benchmarks/hrx-system-tests
python3 tools/hrx_system_tests/report.py --output-dir build/benchmarks/hrx-system-tests
```

`report.py --report-dir PATH` writes Markdown and Discord text somewhere else while reading existing results. Reports derive their date, host, GPU identity and results from artifacts. The source scripts contain no fixed measurement date or user home path.

## Timing definitions and wait policy

- **Compile:** first invocation per fresh compiler instance, then seven calls to `compile(source)` that bypass the JIT disk-cache layer. M512 follows M1 in the same process; first invocation is not a second cold process. Synthetic Q4 fixtures use LSE’s dispatch tables; the build metadata records that selection.
- **Bandwidth:** host monotonic time from recording eight copies through flush and completed retirement. Allocation, initialization and full validation are excluded. GB/s is decimal; MiB is binary. The rate measures this HRX buffer-copy path, without claiming SDMA-only or theoretical peak bandwidth.
- **HRX issue:** recording plus flush return. **HRX completion:** recording, flush and observed timeline completion. One checked 4-byte store uses WG32, with 32 warmups and 200 samples.
- **Bare HSA:** publication/doorbell return and active completion polling are timed separately. Fresh completion signals pair raw CP start/end timestamps with their dispatches. CP ticks are divided by the live GPU timestamp frequency.
- **Tiny transfers:** 32 warmups and 256 individual 8-byte copies per direction, including record/flush/completion.
- **Pure communication:** one persistent lane publishes sequence-numbered 8-byte requests/replies through release/acquire ownership; no mixed CPU/GPU read-modify-write. Each initiator measures 128 warmups and 1,024 round trips in each of three trials. CPU→GPU→CPU uses a host monotonic clock; GPU→CPU→GPU uses `MSG_RTN_GET_REALTIME` at the live agent frequency. Initial launch is excluded. Polling and responder turnaround remain included.

The current runtime's default is **`MAC_HSA_BLOCKED_POLL_US` unset → 32 us blocked polling**. An explicit integer from 10 through 1,000 overrides it; malformed or out-of-range values use 32 us. HRX timeline completion uses that policy. Bare HSA completion and mailbox loops actively poll. The environment setting and effective interval are saved in `wait-policy.json`; changing it creates a different completion-latency configuration.

The benchmark runner explicitly selects 32 us when no valid interval was requested, so an older copied runtime also receives the selected policy. It records both the caller setting and the applied interval. Use the rebuilt runtime for new measurements (`--runtime-dir build/hsa`). Historical results retain their recorded interval, including the original 1,000 us benchmark; report generation uses that artifact metadata.

CPU and GPU clocks are not calibrated against one another. These tests establish round trips and API-to-completion intervals, not exact one-way physical CPU→GPU or GPU→CPU latency. Neither cross-domain subtraction nor RTT/2 estimates are used. Percentiles use linear interpolation at `(n−1)×p`.

## Files

- `build.py`: offline fixtures and compiler measurement.
- `run.py`, `run-disjoint.py`: bounded live workload and JSON/CSV summaries.
- `report.py`: Markdown report and Discord table under 2,000 characters.
- `common.py`: paths, library identities and statistics.
- `compile.cpp`, `native.cpp`, `bandwidth-disjoint.cpp`, `latency.cl`: native fixtures.

The accepted measurements are preserved in the [frozen benchmark report](../../docs/benchmarks/hrx-2026-09-27/HRX-BENCHMARK-REPORT.md). Binaries and generated code objects belong in release assets; generated artifacts are not tracked with this source directory.
