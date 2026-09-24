# rocprofmac

`rocprofmac` is an experimental low-overhead dispatch profiler for this DriverKit HSA runtime. The initial executable is a bounded qualification workload, not yet an arbitrary LSE process launcher. The initial R9700/gfx1201 qualification passed on driver 195.

It enables CP dispatch profiling on an unused AQL queue, submits 32 kernels with distinct completion signals, and reads GPU-written `start_ts` and `end_ts` after the batch finishes. There is no timestamp-marker kernel or per-kernel completion wait. Labels and host submission spans are buffered, then written as Chrome trace JSON after verified queue retirement. Two separate profiled queue lifetimes follow an unprofiled control; every dispatch checks its entire data slice, including unchanged input and surrounding canaries.

## Verified result

The unprofiled control and two profiled batches each completed 32 dispatches with exact outputs, unchanged inputs/full-slice guards and acknowledged queue retirement. All 64 profiled dispatches returned ordered nonzero CP timestamps at the device-reported 100 MHz. Logs: `build/tests/driver195-hardware/rocprofmac.log` and `rocprofmac.json`. This small sequential qualification is not a controlled overhead benchmark. General LSE profiling integration and CPU sampling execution remain pending.

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

The trace separates host and GPU lanes, with independent zero origins. They are **not correlated**: do not infer launch latency by subtracting timestamps across them. Adjacent GPU dispatch intervals may overlap. A positive interval between adjacent dispatches is a queue gap; it does not identify bandwidth, cache misses, occupancy, or a particular scheduling bottleneck. Profiling perturbation has not been measured. No SDMA timestamps or hardware counters are claimed.

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

macOS already provides sampled CPU stacks through Instruments Time Profiler. The bounded wrapper preserves each command argument and requires explicit execution:

```sh
# Dry run prints the exact argv; add --run to record.
python3 scripts/rocprofmac-cpu.py --seconds 30 --output build/cpu.trace -- /absolute/path/to/lse [arguments]
# Attach to one existing process instead of starting another GPU workload:
python3 scripts/rocprofmac-cpu.py --run --seconds 30 --output build/cpu.trace --attach PID
xcrun xctrace export --input build/cpu.trace --toc --output build/cpu-toc.xml
open build/cpu.trace
```

A `.trace` includes sampled CPU stacks, not GPU utilization. Retain matching binaries/debug symbols for useful names. Recording is permission-dependent and has not yet been exercised as part of this qualification. The installed `xcrun xctrace help record` confirms `Time Profiler`, bounded recording, launch/attach and explicit environment options. Apple's [Instruments help](https://developer.apple.com/library/archive/documentation/AnalysisTools/Conceptual/instruments_help-collection/) describes Time Profiler's low-overhead CPU sampling; its [command-line recording example](https://developer.apple.com/videos/play/wwdc2022/10106/) documents the xctrace workflow.

The GPU qualification trace currently contains CPU submission spans. LSE's existing opt-in phase measurements can identify JIT/partition/bind/wait costs; wiring those labels, transfers and waits into a common buffered trace is a subsequent integration step. Neither those wall spans nor CPU samples replace CP dispatch timestamps. GPU-to-host clock calibration is still needed before combining them on a shared time axis.
