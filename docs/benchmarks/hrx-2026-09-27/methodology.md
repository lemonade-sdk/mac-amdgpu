# HRX benchmark definitions

## 1. Kernel compilation

Measure the LSE Loom compiler that produces gfx1201 code objects for HRX.
Record input source bytes, total and nonblank lines, source SHA-256, output
code-object bytes and compile duration. Report the first compile separately
from repeated uncached compilation in the same process. Compiler timings do
not include model loading, a cached kernel lookup or GPU execution.

## 2. Host to device transfer

Measure retained HRX copies from mapped coherent host/GTT memory into
DEVICE_LOCAL VRAM. Sweep 4, 16 and 64 MiB payloads, with eight copies per batch
and three measured batches per size after warmup. Time record, flush and
completion with the host monotonic clock. Report decimal GB/s from actual
payload bytes. The headline comes from a separate distinct-region check using
eight 12 MiB regions per endpoint (96 MiB aggregate working set) to avoid reusing
a small cached region. The 256 MiB GART/shared window bounds mapped host memory. Allocate, seed and validate full payloads and guards outside timing.

## 3. Device to host transfer

Use the same sweep and timing boundary from DEVICE_LOCAL VRAM into mapped
host/GTT memory. Verify every output byte and guards after completion. This
records the current HRX transfer route, including its submission overhead;
it does not establish the theoretical link maximum.

## 4. Launch and communication latency

Separate HRX dispatch recording, flush and completed minimal-kernel round
trip. Report median, P95 and minimum with sample counts. Measure completed
8-byte copies in each direction to show small-transfer API latency.

Use a persistent GPU mailbox for CPU-initiated CPU→GPU→CPU and GPU-initiated
GPU→CPU→GPU round trips. Each elapsed interval uses one clock domain. Separate
CPU and GPU ownership of mailbox words avoids unsupported mixed CPU/GPU
read-modify-write atomics. Report GPU tick frequency with the results. These
round trips include both directions and polling. They do not establish exact
one-way latency; dividing them by two would assume symmetric paths.

## Measurement conditions

No language-model workload runs during these tests. The existing GPU monitor
remains running. Benchmarks are serialized, bounded by process and wait
deadlines, and retain backing allocations until GPU retirement is confirmed.
Raw samples, runtime identity and library hashes accompany the report.
