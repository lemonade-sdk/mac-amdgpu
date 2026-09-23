# macOS HRX implementation candidate

Driver 189 and the companion HSA runtime add the contracts needed by an explicit
macOS adapter for the pinned HRX host AQL path. The implementation has software
coverage; hardware results from driver 187 do not validate these new paths.

## Implemented candidate

- Public CPU-owned coarse/kernarg HSA pools backed by prepared DriverKit DMA
  buffers, equal CPU/GPU addresses, truthful access and pointer metadata, and
  capacity from the negotiated GART window. CPU-only fine pools do not falsely
  advertise GPU access. Same-GPU VRAM copies use bounded SDMA chunks.
- Dynamic queue scratch allocation, CP inactive-signal servicing, growth/reuse,
  LDS/private apertures, retained backing on uncertain removal, and error
  callbacks that wake signal waiters without inventing completion. Background
  workers stop before normal queue destruction returns.
- GC_INFO discovery-based CU geometry, waves and resource limits, replacing the
  hardcoded CU count. GPU timestamp frequency comes from the bounded ATOM ROM
  clock-table reader; an unknown or invalid table returns an error.
- gfx1201 and compatible gfx12-generic-v1 executable loading, including the
  actual 17 built-in HRX kernels, checked ELF relocations and descriptor lifetimes.
- A reproducible build of pinned HRX revision
  `5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c` using a tracked adapter patch applied
  to a disposable build copy. The vendored upstream checkout is unchanged.

The adapter uses one GPU, CPU-published AQL queues and dispatch/completion
ownership transitions for host buffers. It does not claim general fine-grained
memory, SVM or GPU virtual-memory aliases. GPU-driven enqueue, ASAN/TSAN/feedback,
hostcalls and PM4 replay are rejected. Hardware profiling is unavailable until
the driver can safely refresh CP queue properties. This is an explicit port,
not full ROCr/HSA conformance or unmodified HRX support.

## Build without GPU submissions

```sh
bash scripts/test-hsa-code-objects.sh
bash scripts/build-resource-test.sh
bash scripts/build-signal-test.sh
bash scripts/build-atomic-contention-test.sh
cmake -S hsa -B build/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/hsa --parallel 4
ctest --test-dir build/hsa --output-on-failure
bash scripts/build-hrx-macos.sh
bash scripts/test-hrx-abi.sh
bash scripts/build-lse-macos.sh
python3 scripts/test-hrx-hardware.py
```

The final command only lists the hardware procedure. Host virtual-memory tests
need permission to use macOS shared-memory facilities. `mac-hrx-smoke
--check-library` loads the actual HRX dylib and checks its status ABI without
initializing HSA or submitting work.

## Hardware validation

After installing driver 189, run:

```sh
python3 scripts/test-hrx-hardware.py --run
```

The runner checks the responding driver, then tests topology/clock queries,
GPU-backed signals, public memory pools and persistent queues, scratch/LDS,
concurrent signals, two processes sharing the GPU, and actual HRX
initialization/streams/upload/copy/fill, executable loading, a 4,093-element
FP32 affine kernel and a 16×16 FP32 matrix multiply. Compute results and full
input/output guards are checked through readback before shutdown. Each stage logs to its
own file and a failure stops later GPU work. No model inference result is implied
by these small HRX compute checks.

Native atomics run last as a separate experiment. CPU-only and GPU-only controls
first verify add and CAS independently using the same mapping and operations; a
failed control stops before mixed execution. Three mixed trials each request
10,000,000 native CPU additions plus 10,000,000 system-scope shader additions,
expecting exactly 20,000,000. A separate 64-bit CPU/GPU CAS-lock test checks a
protected counter, mutual-exclusion evidence and data guards. Progress/abort
fields and deadlines distinguish wrong results from incomplete work, queue
faults and timeouts. `--skip-atomics` omits this experiment when isolating HRX.
The explicit native test does not route CPU operations through the working HSA
GPU-atomic executor. Each phase has a 60-second deadline and a three-second
cancellation grace period; a timeout cannot count as success. The DMA buffer is
16 KiB aligned. Its CPU mapping uses Apple’s default cache policy (not independently
verified as uncached); the GART mapping uses SYSTEM/SNOOPED/UC attributes.
No cache-policy or fence substitution is made merely to assume interoperability.

For queue firmware policy, mapping details and result interpretation, see
[the atomic experiment notes](PCIE_ATOMIC_TEST_POLICY.md).

## Atomic capability evidence

```sh
python3 scripts/check-pcie-atomics.py --json
```

This reads cached PCIe topology only, with separate known-missing and unknown
states. It reports routing, 32/64/128-bit completion, requester enable and egress
blocking where properties are available. It neither changes configuration nor
proves Thunderbolt forwarding or live cache/atomic behavior.

The observed cache reports no root-port 32/64-bit completion and no routing on
two intermediate bridge ports. Earlier native SDMA/shader contention lost
updates. These facts do not prove that every Apple/AMD mapping or configuration
lacks interoperability; the new experiments test the actual operation, mapping
and scope. A pass establishes only the tested case and requires further study
before broader HSA capability claims.

Apple's kernel atomic guarantees apply to devices participating in the platform
coherency architecture; their separate PCI BAR warning does not by itself
exclude DMA-backed host memory. Metal synchronization guarantees also do not
automatically establish an external DriverKit GPU's participation.

- [Apple kernel atomics](https://developer.apple.com/documentation/kernel/libkern/atomic_operations)
- [Apple DriverKit interrupt delivery](https://developer.apple.com/documentation/driverkit/iointerruptdispatchsource)
- [AMD hardware atomic behavior](https://rocm.docs.amd.com/en/docs-6.4.2/reference/gpu-atomics-operation.html)
- [Linux PCI AtomicOp path requirements](https://docs.kernel.org/6.14/driver-api/pci/pci.html#c.pci_enable_atomic_ops_to_root)

## LSE host port

The tracked LSE adapter targets revision `b5637a7109d409c21f75586edb75e7631277bce8`.
It builds the native CLI against the actual HRX and Loom libraries, with kqueue,
Darwin socket setup, native whole-archive linking and portable aggregate argument
binding in place of the unavailable reflection extension. Multi-GPU spanning
groups and Linux abstract Unix sockets return explicit unsupported errors.

Eight CPU-only suites pass, as do readiness/wake/EOF handling and a 1 MiB local
TCP transfer. Linking the shared kernel archive into the CPU graph test fixes
missing kernel registration and retained-overwrite numerical failures. That
broader test reports 118/124: six remaining assertions assume device-first aliasing,
program replay or GPU launch counts while this test uses the CPU interpreter.
The scheduler/interpreter paths involved are unchanged from the pinned upstream.
These results do not establish HRX execution or model inference on the GPU.
