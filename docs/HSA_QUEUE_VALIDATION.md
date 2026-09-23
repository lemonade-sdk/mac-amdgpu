# Persistent HSA queues and shared signals

Driver 187 supports seven persistent legacy compute queues across two MEC pipes
with four queues per pipe, matching Linux's gfx1201 topology. Flat slots 1–7 map
to `(pipe = slot / 4, queue = slot % 4)`. Slot 0 remains reserved for bounded AQL operations, including the runtime's
GPU atomic signal executor. MES KIQ maps and removes these queues; this is not
the MES scheduler's GFX queue path. Queue capacity is shared across clients.

The runtime implements `hsa_queue_create`, queue indices, CPU doorbell stores,
inactivation and destruction using shared AMD queue metadata and a shared AQL
ring. Queue handles identify their owner and lifetime. Driver-side BO free is
rejected while firmware owns that queue's ring or metadata. Client departure
unmaps its queues before retiring its storage. An uncertain map or unmap retains
backing and blocks normal operation until verified reset.

Callers must retain all executable, data, kernarg and signal allocations until
submitted work completes or the queue is successfully removed. Raw VMID0 queues
are a trusted-development interface, not isolation between mutually untrusted
applications. Scratch allocation, dynamic queue resource updates, device-side
enqueue, hardware error callbacks and complete profiling support are unfinished.

## Signal implementation

AMD signal handles point to the real 64-byte ABI at identical CPU/GPU virtual
addresses. GPU code accesses the value directly; CPU HSA API updates dispatch a
system-scope atomic kernel on reserved queue 0. CPU HSA loads and direct AMD ABI
loads read the shared value. All GPU-backed updates use sequentially consistent
GPU atomics, including APIs that request weaker ordering.

This routing is necessary because native CPU atomic RMWs racing with GPU atomic
RMWs lost updates on the tested Apple Silicon/Thunderbolt path. GPU shader
release publication and CPU acquire observation passed. The GPU-backed HSA API
also passed sequential stores, arithmetic, bitwise operations, exchange, CAS,
wraparound and waits. These results do not establish general fine-grained host
memory or arbitrary native CPU/GPU atomics.

One executor pools 256 signals in one shared arena. GPU signal IPC and signals
shared by distinct GPU devices are not implemented. Every CPU value-changing
HSA call currently incurs a bounded GPU dispatch and MES map/unmap; throughput
has not been characterized. CPU-only signals continue to use CPU atomics.
Failure of the executor stops all its signal waits without inventing completion.

## Hardware procedure

Install driver 187 or newer using the host app. Verify the running driver, not only the
installed registration. The tools initialize a fresh GPU session or join an
already initialized session. They refuse to submit persistent work to older
builds. Run one test at a time, and do not replace the driver during a test.

```sh
cmake -S hsa -B build/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/hsa --parallel 4
bash scripts/test-hsa-code-objects.sh
bash scripts/build-signal-test.sh
build/hsa/mac-hsa-info
build/hsa/mac-hsa-signal-api-test --run
build/hsa/mac-hsa-queue-test --run build/tests/hsa-code-object.hsaco
build/hsa/mac-hsa-queue-signal-test --run build/tests/hsa-signal-object.hsaco
python3 scripts/test-hsa-multi-process.py
```

Required results:

- Signal API operations return correct values and preserve direct AMD ABI reads.
- A 64-packet ring executes 192 dispatches with matching read/write indices and
  all 16 KiB of each result/input/guard allocation intact.
- Queue 0's barrier remains pending until queue 1 decrements its dependency.
- A CPU HSA store releases a pending GPU barrier through the reserved executor.
- Two shader queues plus CPU HSA additions produce exactly 131,136 increments,
  with at least one CPU API call issued before shader completion.
- Two CP completions and 64 CPU HSA additions to a shared signal produce exactly
  64, without losing either decrement; adjacent signals remain intact.
- Both queues are successfully removed before any referenced allocation is freed.
- All seven persistent slots can be created; an eighth fails without poisoning
  the session. Four CPU producers complete 256 barrier packets through one ring.
- Two processes each hold two queues. After the first completes and exits, the
  second executes through its existing queues and tears down successfully.

All these checks passed on installed driver 187. Each process verified 192 shader
dispatches and all 16 KiB of input/output/guard memory after every dispatch.
The combined session also crossed the SDMA ring boundary that previously caused
a timeout. SDMA write pointers now remain monotonic; only ring storage offsets
wrap. CPU/shader additions produced exactly 131,136 and CPU/CP completion updates
produced exactly 64. At least one CPU HSA operation was issued while GPU work was
pending in each concurrency check. Clean final teardown returned the GPU to stage 0.

Completion timeouts are bounded at two seconds. A failed removal retains memory
for session reset. A passing shader-only test is insufficient to declare CP
completion atomics or full HRX compatibility working. Kernel dispatch capability
is exposed only for gfx1201 with driver 187 or newer: seven device-wide queues,
64–4096 packets and multi-producer support. The HRX readiness audit still fails
until fine-grained memory integration and end-to-end inference are verified.
