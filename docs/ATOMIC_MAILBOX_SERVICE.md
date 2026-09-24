# CPU/GPU atomic synchronization service

The runtime treats CPU-local atomics, GPU-local atomics, ordered ownership transfer, and native mixed CPU/GPU read-modify-write as separate capabilities. `mac_hsa_memory_get_sync_capabilities` reports the policy for a tracked allocation and its actual device/mapping path. It does not enable PCIe configuration or initialize hardware.

Before device initialization, the IOKit transport captures the original cached PCIe topology evidence. Unknown or missing root completion, bridge routing, or original requester configuration cannot qualify native mixed atomic operations. Setting Requester Enable in an explicit experiment does not change this snapshot. Even a fully advertised path needs a qualified implementation for its mapping, atomic width, and scope; the current native mixed RMW flag remains false.

Host-only HSA signals use ARM64 atomics. CPU calls that update a GPU-visible HSA signal already route through `gpu_signals.cpp`: a bounded GPU kernel performs the update and returns the old value. GPU shaders and the CPU's HSA API updates therefore share the GPU atomic domain. The CPU observes the signal through acquire loads. Store, exchange, add, subtract, AND, OR, XOR, and both CAS outcomes use this path. It does not intercept arbitrary C++ atomic instructions or arbitrary system-scope shader atomics.

The current shared pool remains coarse-grained. The ownership capability describes the specifically tested release/acquire protocol on DriverKit shared DMA mappings. It is not a fine-grained pool advertisement or a general native atomics guarantee.

## Experimental DMA mailbox

`signal_mailbox.h` and `signal_mailbox_gfx1201.cl` implement a bounded, single-producer request/completion protocol. The CPU writes operation/operand/compare requests, then releases a request sequence. A persistent GPU dispatch acquires the sequence, performs native GPU atomic operations on a canonical HSA signal, stores every old value, then releases a completion sequence. The CPU acquires that exact completion before consuming results or reusing request storage. A batch contains 1–64 ordered operations. Multiple CPU producers must serialize access to a mailbox.

Separate 128-byte regions hold request sequence, completion sequence, ready, abort, and state. Payload and results use relaxed atomic accesses under ownership. The kernel bounds idle waiting; the host bounds each run, publishes abort on failure, and waits for dispatch completion. Queue unmap must succeed before buffers, signals, or code are released. A timeout is an indeterminate operation outcome: never retry the operation blindly, because it may have executed before completion was lost.

The persistent service is the default for the qualified gfx1201 DriverKit shared-memory profile on driver193 or newer. Other profiles retain the one-shot executor. `MAC_HSA_SIGNAL_BACKEND=one-shot` explicitly selects the established path; `mailbox` permits earlier experimental ownership-capable builds190–192. Neither choice enables native mixed RMW.

## Build and validation

```sh
bash scripts/build-signal-mailbox-test.sh
cmake -S hsa -B build/hsa
cmake --build build/hsa --target mac-hsa-signal-mailbox-bench hsa-signal-mailbox-test hsa-synchronization-policy-test
ctest --test-dir build/hsa -R 'hsa-signal-mailbox-protocol|hsa-synchronization-policy' --output-on-failure
```

The software peer test performs no GPU access. It checks old values for every operation, unsigned 64-bit wraparound, successful/failed CAS, batching, active/hybrid waits, malformed input, sequence errors, failed GPU state, timeout, cancellation, and unchanged results on failure. Shader build assertions check system-scope atomics and ownership acquire/release lowering.

An explicitly authorized hardware run, after initialization:

```sh
build/hsa/mac-hsa-signal-mailbox-bench --run build/tests/hsa-signal-mailbox.hsaco 128
# Increase the count only after the small run passes:
build/hsa/mac-hsa-signal-mailbox-bench --run build/tests/hsa-signal-mailbox.hsaco 1024
```

The same operation stream runs through the existing HSA one-shot path and six mailbox variants: active or hybrid polling, each with batch sizes 1, 8, and 64. Every returned value, final target value, completion sequence, kernel completion, and data/kernarg guard is checked. Reported timings include validation overhead; batch amortization is shown explicitly. Hybrid polling spins for 50 microseconds, then sleeps for 10 microseconds between acquire checks. These are candidate parameters, not a claim of optimal latency.

Mailbox IRQ wakeup is unavailable in the current transport and is reported separately as unsupported. A future MSI/IRQ may wake the CPU, but the acquired completion sequence remains authoritative; a wakeup alone can never imply completion. This benchmark does not claim any strategy is fastest before measurement, and it does not compare against a fabricated IRQ path.

## Hardware results

On the R9700/gfx1201 with driver192, both 128- and 1,024-operation streams passed all six modes. Every operation result, final target, completion sequence, dispatch retirement and guard check passed. The original topology audit reported missing bridge routing and root completion prerequisites; native mixed RMW remained disabled.

The longer run measured:

| Path | Batch | Operations/s | Microseconds/operation |
| --- | ---: | ---: | ---: |
| Existing one-shot HSA | 1 | 66 | 15,259 |
| Active polling | 1 | 42,708 | 23.42 |
| Active polling | 8 | 124,038 | 8.06 |
| Active polling | 64 | 162,426 | 6.16 |
| Hybrid polling | 1 | 42,748 | 23.39 |
| Hybrid polling | 8 | 116,949 | 8.55 |
| Hybrid polling | 64 | 156,528 | 6.39 |

These include exact validation, exclude service startup, and are not inference throughput or an IRQ comparison. Batching amortizes the handoff; it does not make an individual synchronous request take six microseconds. Logs: `build/tests/driver192-hardware/mailbox-128.log` and `mailbox-1024.log`. The qualified integration and its lifecycle validation are described below.

## Qualified HSA integration

On gfx1201 DriverKit shared DMA mappings with driver193+, the persistent service handles CPU HSA updates to GPU-visible signals by default. Set `MAC_HSA_SIGNAL_BACKEND=one-shot` to use the established executor. Explicit `mailbox` remains available for experimental builds190–192 that support the ownership protocol. Unknown override values conservatively select one-shot. The service uses the same native GPU atomic domain as the benchmark, with a validated slot index selecting one of the context's 256 AMD signal records. Loads remain CPU acquire observations; this does not intercept arbitrary pointer atomics.

A single internal queue is mapped lazily, reused across updates, and retired after 50 milliseconds idle. Public queue creation reclaims it while holding a lease that prevents a concurrent signal update from taking the freed slot before mapping completes. If seven public queues already occupy the device, service mapping returns out-of-resources before any request publication, so the signal operation uses reserved one-shot queue0. The advertised seven public slots are unchanged. This preference is scoped to this process/connection; other processes still compete for the driver's global slots.

The service serializes CPU producers. It bounds startup at 250 milliseconds, each request at 100 milliseconds, and completion retirement at 100 milliseconds before attempting verified unmap. After publication, an error or timeout invalidates the context; it never retries the operation using another backend. Failed unmap retains code, arguments, mailbox, metadata, ring, and the borrowed signal arena. Last-signal destruction joins the idle worker and verifies retirement before freeing storage. The HSA wait path observes background service failure and stops waiting on invalidated signals.

The qualified runtime is built in `build/hsa`. For isolated development or regression work, a separate build directory remains supported:

```sh
bash scripts/build-signal-mailbox-service.sh
cmake -S hsa -B build/hsa
cmake --build build/hsa -j 4
ctest --test-dir build/hsa --output-on-failure
```

The new sanitized runtime test exercises real HSA API routing against a software GPU peer: hot reuse, four concurrent producers, all seven public queue slots, unpublished fallback, idle release/restart, final destruction, lost acknowledgement after an applied RMW, no replay, and failed-unmap retention. Production shader metadata/codegen is checked separately. This does not replace hardware validation of the integration.

To repeat hardware checks, serialize GPU testing and use:

```sh
build/hsa/mac-hsa-signal-api-test --run
build/hsa/mac-hsa-queue-signal-test --run build/tests/hsa-signal-object.hsaco
```

Signal API, concurrent queue signal, and lifecycle hardware runs passed on driver193. The trace-enabled run independently verified four mailbox lifetimes, 192 hot requests in the first lifetime, retirement for public queue creation and idle expiration, both final-context retirements, one completed reserved fallback, and seven available public slots after the final context. All retired completions were confirmed; the trace checker passed. Log: `build/tests/driver193-hardware/signal-service-trace.log`. The default path was then tested on driver194 with `MAC_HSA_SIGNAL_BACKEND` unset and passed the same complete trace/lifecycle checks: `build/tests/driver194-hardware/signal-service-default.log`. The qualified sources were promoted to `build/hsa`; the rollback override remains available. A trace-enabled verification of the default path uses:

```sh
DYLD_LIBRARY_PATH="$PWD/build/hsa" \
MAC_HSA_SIGNAL_TRACE=1 \
build/hsa/mac-hsa-signal-service-test --run > signal-service-lifecycle.log 2>&1
python3 scripts/check-signal-service-trace.py signal-service-lifecycle.log
```

The checker rejects missing mailbox execution, missing reserved fallback, unconfirmed retirement, absent idle/reclaim/shutdown paths, and unretired queue lifetimes. The final hardware phase creates all seven public queues again after the last signal context is destroyed. Trace is disabled by default and adds no per-request logging in the normal path.

The production service handles one synchronous HSA update per request; benchmark batch64 throughput is not its expected per-call performance. Its idle lease and hybrid polling thresholds still need workload measurements.
