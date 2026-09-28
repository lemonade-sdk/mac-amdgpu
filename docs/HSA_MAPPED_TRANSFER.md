# Mapped staging for HSA transfers

The IOKit transport reuses one mapped 4 MiB host allocation per connection for
CPU/GPU transfers. CPU bytes are copied directly through that mapping, followed
by the existing synchronous SDMA copy. An aligned 4 MiB upload needs one copy
submission instead of 1,024 write RPCs and 1,024 copy submissions.

Partial-dword writes preserve neighboring bytes with read/modify/write. The
mapped path uses the same CPU fences and shared-mapping validation as the
existing shared-buffer API. `copyRaw` still requires successful completion;
failed copies fault the connection and retain storage until owner shutdown.
Executable code-cache invalidation is unchanged.

Older drivers and clean allocation or mapping declines use the existing 4 KiB
RPC path. Coherence-proof and cleanup failures propagate rather than selecting
fallback. The allocation consumes 4 MiB of shared host/GART capacity, is
serialized by the existing connection mutex, and does not grow with the number
of loaded executables. It does not duplicate model weights or consume 4 MiB of
the device VRAM pool. A shared pool filled to its exact capacity still needs this
additional headroom.

## Correctness and build checks

- The ASan/UBSan mocked transport test passes, including exact full-buffer and
  guard comparisons across the 4 MiB boundary, partial/last-byte writes,
  mapping reuse, clean fallback, and coherence/cleanup/copy fault retention.
- A native driver-204 check passes all nine byte-transfer cases using one
  8 MiB + 16 KiB GPU allocation. It compares every data and guard byte, checks
  that CPU input is unchanged, and confirms allocation release and owner
  shutdown. No shader dispatch is needed for this check.
- The private library replaces only the transport object; the other 14 runtime
  objects and all 140 exported symbol names are unchanged. The shared-allocation
  body, `copyRaw`, and `invalidateCodeCaches` have byte-identical source proofs.
- The normal `build/hsa` targets build successfully, and the focused
  `hsa-transport-test` CTest passes. The canonical library is byte-identical to
  the library used by both native and HTTP checks.

## Ordered HTTP comparison

One baseline-then-candidate pair used the same canonical server executable,
Qwen3.8-27B Q4 target, DFlash2 Q8 draft, depth 3, configured KV limit 262,100,
and the same two-turn workload. The actual loaded candidate HSA path and hash
were recorded from the server's dyld log. Both assistant results are exactly
equal, with identical speculative acceptance and dispatch counts; neither arm
uses host groups or host fallback.

| Measurement | Previous transfer | Mapped transfer |
| --- | ---: | ---: |
| First prompt tokens | 5,207 | 5,207 |
| First prefill | 18.711 s | 14.541 s |
| First prefill throughput | 278.28 tokens/s | 358.09 tokens/s |
| First decode, 102 timed tokens | 24.87 tokens/s | 31.26 tokens/s |
| Second decode, 159 timed tokens | 31.28 tokens/s | 31.35 tokens/s |
| First-request JIT compile count/time | 7 / 246.2 ms | 0 / 0 ms |

The first prefill is 4.170 s shorter, with 28.7% higher throughput. The second
request's decode rate is effectively unchanged. This is one ordered pair;
compilation differs by 246.2 ms, and process/cache order can affect the result.
It is evidence for startup improvement, not a repeated steady-state speed claim.
The process-total scheduler JIT-lookup span across both requests falls from
7.618 s to 2.547 s; that span includes executable loading and is not compiler
time alone.

## Frozen build control

A second control used the frozen prepatch build instead of the previously
installed library. The server executable and two-turn workload were unchanged;
both control and mapped arms compiled zero new kernels. Both assistant results
remain exactly equal.

| Measurement | Frozen prepatch build | Mapped transfer |
| --- | ---: | ---: |
| First prefill | 18.295 s | 14.541 s |
| First prefill throughput | 284.61 tokens/s | 358.09 tokens/s |
| First decode | 25.23 tokens/s | 31.26 tokens/s |
| Second decode | 31.30 tokens/s | 31.35 tokens/s |
| New JIT compiles | 0 | 0 |
| Process-total JIT-lookup span, both requests | 7.457 s | 2.547 s |

The frozen prepatch library has SHA256
`bfbfe0537074f5b1f8895adc007499fc6ef81898e9c876e61ae97b56c0ede827`.
It and the mapped candidate use the same 14 unchanged runtime objects; only the
transport object differs. The original installed baseline has SHA256
`3394ac3e3d14b6b588bd5c8175fb1705179d93ca9c19215568cfcccdfe07ffc1` and
lacks an equivalent object manifest, so the second control provides the
transport-only build comparison. This additional single control supports the
startup improvement without the original compile-count mismatch; it is still
not a repeated or randomized benchmark.

Only aggregate results are recorded here. Local HTTP evidence is in
`build/release/pi-performance/{attention-gdn,mapped-upload,upload-control}/`, including
`metrics-{1,2}.json`, `server.log`, and candidate `hsa-identity.json`.

## Identity and reproduction

The qualified canonical artifact is `build/hsa/libhsa-runtime64.0.1.0.dylib`,
SHA256 `b7f8216e32fa6ab0e5b2fce87c518ce67c3fffa4c628cceece983c53e3cea90d`.
Source, object, library, native and apply/reverse-check manifests are preserved
in `/private/tmp/lse-hsa-upload`. The source patch changes only the IOKit
transport and its existing focused test.

Rebuild and run the CPU fixture from the repository root:

```sh
cmake --build build/hsa --target hsa-runtime64 hsa-transport-test --parallel 4
ctest --test-dir build/hsa -R '^hsa-transport-test$' --output-on-failure
```

A rebuild does not update `/Library/MacAMDGPU/runtime`. Existing server processes
keep their loaded library until restarted. Install the reviewed canonical
artifact through the runtime deployment process, then verify the actual loaded
path and hash on the next launch; setting a library search variable alone does
not establish which image was loaded.
