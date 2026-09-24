# rocprofmac

`rocprofmac` is an experimental low-overhead dispatch profiler for this DriverKit HSA runtime. The initial executable is a bounded qualification workload, not yet an arbitrary LSE process launcher. The initial R9700/gfx1201 qualification passed on driver 195.

It enables CP dispatch profiling on an unused AQL queue, submits 32 kernels with distinct completion signals, and reads GPU-written `start_ts` and `end_ts` after the batch finishes. There is no timestamp-marker kernel or per-kernel completion wait. Labels and host submission spans are buffered, then written as Chrome trace JSON after verified queue retirement. Two separate profiled queue lifetimes follow an unprofiled control; every dispatch checks its entire data slice, including unchanged input and surrounding canaries.

## Verified result

The unprofiled control and two profiled batches each completed 32 dispatches with exact outputs, unchanged inputs/full-slice guards and acknowledged queue retirement. All 64 profiled dispatches returned ordered nonzero CP timestamps at the device-reported 100 MHz. Logs: `build/tests/driver195-hardware/rocprofmac.log` and `rocprofmac.json`. This small sequential qualification is not a controlled overhead benchmark. The HRX integration and full Qwen model capture also passed; CPU Time Profiler recording remains unverified.

## Build and qualification

```sh
cmake -S hsa -B build/rocprofmac -DCMAKE_BUILD_TYPE=Debug
cmake --build build/rocprofmac --target rocprofmac hsa-hardware-queue-test -j 6
ctest --test-dir build/rocprofmac -R hsa-hardware-queue-lifecycle --output-on-failure
# Explicit hardware opt-in; requires gfx1201, driver 195+, and the existing fixture:
build/rocprofmac/rocprofmac --run build/tests/hsa-code-object.hsaco build/rocprofmac-trace.json
```

Running without arguments only prints help. The tool has bounded batch waits and a process deadline. A failed queue removal keeps all potentially referenced memory alive until process exit. It does not install a driver, change PCIe configuration, or run concurrently with another qualification workload.

## Timestamp contract

The new `mac_hsa_dispatch_timestamps` query returns raw **GPU clock** ticks and the device-reported frequency. The caller retains a fresh completion signal, submits it on the specified queue with SYSTEM release scope, then leaves it unchanged until readout. The query requires completion zero and nonzero ordered timestamps; missing timestamps fail rather than substitute host elapsed time. The runtime verifies GPU ownership but cannot prove which queue last used an arbitrary signal.

The trace separates host and GPU lanes, with independent zero origins. They are **not correlated**: do not infer launch latency by subtracting timestamps across them. Adjacent GPU dispatch intervals may overlap. A positive interval between adjacent dispatches is a queue gap; it does not identify bandwidth, cache misses, occupancy, or a particular scheduling bottleneck. Measured model profiling overhead is reported below; it is workload- and runtime-dependent. No SDMA timestamps or hardware counters are claimed.

The standard `hsa_amd_profiling_get_dispatch_time` remains unsupported: that API promises HSA system-clock timestamps, and ROCr converts device timestamps using a calibrated clock model. Returning raw ticks there would violate its contract.

## Why this follows rocprof's low-overhead path

ROCr's `AqlQueue::SetProfiling` sets `AMD_QUEUE_PROPERTIES_ENABLE_PROFILING`; it suspends/remaps only if packets have already been submitted. This runtime permits changing the bit only before any write-index reservation or doorbell kick. Used queues reject property changes; an idempotent request is harmless. Rewriting indices to zero does not bypass the rule.

The existing 64-byte AMD signal ABI already contains hardware timestamp fields at offsets 32 and 40. Static assertions verify those offsets against the vendored public AMD header. GPU completion's SYSTEM release and CPU acquire establish readout ordering on the existing validated DMA mapping.

Sources:

- [ROCr AQL queue implementation](https://github.com/ROCm/rocm-systems/blob/develop/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp), `AqlQueue::SetProfiling`.
- [ROCr GPU agent implementation](https://github.com/ROCm/rocm-systems/blob/develop/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp), `TranslateTime`.
- [rocprofiler-sdk queue interception](https://github.com/ROCm/rocm-systems/blob/develop/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/hsa/queue.cpp), enabling queue profiling and collecting completed dispatch timestamps.
- [Official SDK installation](https://rocmdocs.amd.com/projects/rocprofiler-sdk/en/latest/install/install.html) and [SDK architecture](https://rocmdocs.amd.com/projects/rocprofiler-sdk/en/latest/what-is-rocprofiler-sdk.html).

The published rocprofiler SDK targets the Linux ROCm stack. A full macOS port would need runtime API-table interception and Linux dependency replacements; counters, thread trace, and PC sampling additionally depend on services not exposed by this DriverKit runtime. The narrow CP-timestamp path can be qualified independently of those components. It is not a claim that the complete rocprof3 application runs on macOS.

## CPU sampling

An explicit alternative, Apple's `/usr/bin/sample`, is verified on a bounded CPU-only fixture: 852 snapshots resolved the expected nested functions and source lines, and both sampler and target exited successfully. It snapshots thread stacks, **including waiting threads**; sample counts are not CPU-time percentages. Attach to your existing LSE server without launching a second GPU workload:

```sh
python3 tools/rocprofmac/cpu.py --tool sample --run --seconds 10 \
  --output build/lse-stacks.txt --attach YOUR_LSE_PID
```

The wrapper never kills the attached process and does not silently fall back between tools. `sample` requires an explicit positive PID and a new `.txt` output path. Evidence: `build/tests/rocprofmac-cpu-native.sample.txt`.

An eight-second attach also completed during actual Qwen long-context decode.
On the inference request thread, 4,992 of 6,899 stack observations were waiting
for GPU completion. Active body stacks concentrated in emission/cache identity
construction, hashing and allocation; no Loom compiler stack appeared. These
are observations of one thread, not process CPU-time percentages. The existing
`LSE_TIME_STEPS=1` spans help distinguish replay from first-use compilation.
Evidence: `build/tests/driver195-hardware/qwen-rms-production-cpu.txt` and the
matching `qwen-rms-production-1k1k/server.log`. The workload's repeated-text
check failed separately; successful sampling does not qualify model accuracy.


macOS already provides sampled CPU stacks through Instruments Time Profiler. The bounded wrapper preserves each command argument and requires explicit execution:

```sh
# Dry run prints the exact argv; add --run to record.
python3 tools/rocprofmac/cpu.py --seconds 30 --output build/cpu.trace -- /absolute/path/to/lse [arguments]
# Attach to one existing process instead of starting another GPU workload:
python3 tools/rocprofmac/cpu.py --run --seconds 30 --output build/cpu.trace --attach PID
xcrun xctrace export --input build/cpu.trace --toc --output build/cpu-toc.xml
open build/cpu.trace
```

A `.trace` includes sampled CPU stacks, not GPU utilization. Retain matching binaries/debug symbols for useful names. Recording is permission-dependent. Native xctrace attempts timed out before launching even a CPU-only fixture, so Time Profiler recording remains unverified; help and dry-run validation do not establish recording support. Developer authorization is enabled and templates list successfully. Startup logs report a LaunchServices connection failure and a TCC ListenEvent denial; their causal role is not established. No security settings were changed. On timeout the wrapper stops xctrace, but does not claim the launched target retired and never kills an attached process. The installed `xcrun xctrace help record` confirms `Time Profiler`, bounded recording, launch/attach and explicit environment options. Apple's [Instruments help](https://developer.apple.com/library/archive/documentation/AnalysisTools/Conceptual/instruments_help-collection/) describes Time Profiler's low-overhead CPU sampling; its [command-line recording example](https://developer.apple.com/videos/play/wwdc2022/10106/) documents the xctrace workflow.

The GPU qualification trace contains CPU submission spans, and HRX exports host queue events. LSE's existing opt-in phase measurements identify JIT/partition/bind/wait costs; its buffered dispatch summary supplies operation and shape labels to the exporter. Neither those wall spans nor CPU samples replace CP dispatch timestamps. GPU-to-host clock calibration is still needed before combining them on a shared time axis.

## Experimental HRX / LSE integration

The separate `testing/rocprofmac` HRX candidate at `build/hrx-testing-rocprofmac` reuses HRX's existing per-dispatch completion slots and batched timestamp harvest. It latches CP profiling during queue creation only when `HRX_PROFILE_FILE` and `HRX_PROFILE_MODE=dispatch` are set. Collection end does not rewrite a used queue's CP property; normal queue destruction retires it. With profiling absent, queue setup and stream synchronization keep their existing behavior.

The candidate records executable metadata, CPU queue events and raw GPU dispatch intervals. PM4 queue ranges, counters, thread traces and host/GPU correlation remain rejected. Raw completion slots live in device memory and are initialized once by a GPU kernel; completed dispatch timestamps are harvested in command-buffer batches. There is no timestamp-marker kernel for each model dispatch. Ready events are flushed at existing application stream synchronization points to bound the 64K event ring. A single unsynchronized batch exceeding that capacity fails explicitly.

The HRX candidate passed the real 4,093-element affine and 256-element matrix workloads, including inputs, guards and shutdown. Both dispatches appeared in the capture with GPU timestamps (5.56 and 5.32 microseconds in that run). A full Qwen model server then completed two requests of 64 input and 33 output tokens with matching text and clean shutdown.

The model capture contains 110,154 GPU dispatches and 6,416 host events. Queue/event IDs are unique, and dispatch counts match the independent LSE host summary for all 95 kernel exports. Its GPU envelope is 13.6336 seconds; first-to-last host submission spans 13.6489 seconds, supporting the reported 100 MHz scale without pretending to calibrate the clocks. The 5.6517-second sum of dispatch durations and 7.9819 seconds of positive gaps include a 5.7987-second setup gap before model computation. They must not be mistaken for one request's generation time or a GPU utilization percentage.

Three sequential profile-off requests followed by three profile-on requests, using the same candidate libraries and 64-input/33-output workload, measured:

| Median rate | Profile off | Profile on | Observed change |
| --- | ---: | ---: | ---: |
| Prefill tokens/s | 87.29354 | 86.524477 | -0.88% |
| Decode tokens/s | 12.578807 | 12.217472 | -2.87% |

The corresponding decode time increase is 2.96%. This includes capture buffering/export work at existing synchronization boundaries. Run order and thermal drift were not randomized, so these are observed results rather than a universal overhead guarantee. Evidence is under `build/tests/driver195-hardware/rocprofmac-model`, `rocprofmac-model-off`, and `rocprofmac-model-on3`; the first model's raw capture, JSONL, summary and server log support the count/clock audit. No hardware counters, memory-bandwidth attribution or occupancy measurements are provided.

A newer capture of the accurate FP32-M256 default on 512 input / 129 output
tokens passed with all six profile-off/on responses identical. After two warmups,
the single measured request changed from **88.82 PP/s / 16.67 TPS** to
**87.94 PP/s / 16.08 TPS**, or **1.00% / 3.69%** more elapsed time. The trace
separates 3,334 prefill dispatches from 213,632 decode dispatches and verifies
every export count against the host summary. See [current results and phase
rankings](../../docs/LSE_PERFORMANCE.md). These observations are workload-specific.

Use a profiling-capable HSA build in both arms. The older local
`build/hsa-wait-perf` binary with SHA prefix `244f3943` predates hardware queue
profiling and rejects enablement even though the current source implements it.
That failure was reproduced on an unused queue, without submitting a kernel.
The qualified `7d9b8af9` build supports the unused-queue operation and preserves
the 64 µs polling override. Keep the selected libraries frozen through the
comparison; do not rebuild between its off/on arms.

```sh
# Explicit hardware command for the small HRX correctness workload first:
DYLD_LIBRARY_PATH="$PWD/build/rocprofmac-build" \
  HRX_PROFILE_FILE="$PWD/build/hrx-dispatch.ireeprof" HRX_PROFILE_MODE=dispatch \
  build/hrx-rocprofmac-build/mac-hrx-smoke --run

# For an existing LSE binary, use this same candidate HRX and HSA at launch:
# DYLD_LIBRARY_PATH="$PWD/build/hrx-rocprofmac-build/libhrx/src/libhrx:$PWD/build/rocprofmac-build" \
# HRX_PROFILE_FILE="$PWD/build/model.ireeprof" HRX_PROFILE_MODE=dispatch \
# LSE_PROFILE_DISPATCH=submit LSE_TIME_SPANS=1 <existing GPU-only LSE command>
```

`LSE_PROFILE_DISPATCH=submit` provides buffered export-name, operation and shape labels without forced serialization. Avoid `serial` when measuring the batched inference path. Compare matched profile-on/off runs before making an overhead claim.

Convert a completed capture offline:

```sh
build/hrx-rocprofmac-build/runtime/src/iree/tools/iree-profile/iree-profile \
  export --format=ireeperf-jsonl --output=build/model.jsonl build/model.ireeprof
python3 tools/rocprofmac/export.py build/model.jsonl \
  --trace build/model-trace.json --summary build/model-summary.json \
  --lse-log build/model.log
```

The converter refuses missing/failed session endings, absent frequencies or invalid dispatch stamps. It labels gaps as intervals with **no captured dispatch**: they may include copies, timestamp harvest, submission delay or other work, and are not GPU idle measurements. Summed kernel durations are not GPU busy time when kernels overlap. Each device clock and the host clock retain independent origins.
