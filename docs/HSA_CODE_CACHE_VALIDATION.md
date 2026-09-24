# Executable upload and GPU code-cache synchronization

The Mac HSA loader now completes GPU code-cache synchronization after uploading
an executable and before publishing its symbols. Previously, a destroyed code
object's allocation could be reused without invalidating cached instructions.
The direct Q6 FP32/INT8 comparison then failed when switching matrix shapes,
although the same shape passed in isolation.

ROCr performs the corresponding step in `RegionMemory::Freeze()` in
`amd_loader_context.cpp`, calling `GpuAgent::InvalidateCodeCaches()` after the
upload. Its GFX12 implementation uses `ACQUIRE_MEM` and waits for completion.
The reference sources are under
`upstream/rocm-systems/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/`.

## Implementation

`hsa/src/executable.cpp` calls `Connection::invalidateCodeCaches()` after
relocation and upload. Failure prevents symbol publication and returns a load
error. The IOKit transport uses the existing synchronous native-dispatch API:
the driver brackets a one-instruction `s_endpgm` launch with full-range
`ACQUIRE_MEM` packets (`GCR_CNTL=0xc3b1`) and waits for its completion fence.

One 16 KiB utility allocation is retained per connection and reclaimed when
the connection closes. Normal executable allocations are still released.
Synchronization runs at code upload, not for every inference dispatch. This
uses the existing gfx1201 native-dispatch path; no driver ABI, firmware,
shader arithmetic, numerical tolerance or atomic capability changes are needed.

## Controlled R9700 results

The unchanged direct fixture alternates FP32 and INT8 kernels against identical
input buffers, verifies every output, checks input/output guards and repeated
hashes, and releases executable objects between three shapes:
`N17408 K5120`, `N5120 K17408`, and `N5120 K6144`.

| Configuration | Result |
| --- | --- |
| Previous runtime, second shape alone | Pass |
| Previous runtime, all shapes | Second shape FP32 fails: `0` instead of `19.8689175` |
| Retain code objects across shapes | All shapes pass |
| Retain only data buffers | Fails on third shape |
| Allocate/upload the utility kernel, omit synchronization dispatch | Same second-shape failure: `0` instead of `19.8689175` |
| Synchronize, original variant, two independent runs | All 240 dispatches pass |
| Synchronize, two-iteration INT8 prefetch variant | All 120 dispatches pass |

The allocation-only control rules out merely moving the code allocations as
the explanation for the fix. It is an isolated diagnostic build; no production
switch bypasses synchronization. All successful runs use normal resource
release, unchanged oracles and bit-identical per-variant output hashes.

The host suite passes all 18 tests, including ASan/UBSan executable-loader
tests. A new fault-injection case verifies that failed cache synchronization
leaves no published symbol or loaded handle and releases the image allocation.
The virtual-memory test requires access to macOS shared memory outside the
filesystem sandbox.

An inference smoke test with the current default HRX library also passes:
both Qwen3.8-27B MLX Q6 responses (512 input / 129 output tokens) exactly
match the previously qualified default, and the server exits cleanly.
The second response decodes at 17.48 tokens/s, but still compiles three kernels
during prefill; this run is a correctness check, not a new throughput baseline.
Results are in `build/tests/driver195-hardware/code-cache-fix-model/result.json`.

## Local evidence and reproduction

Preserved artifacts are under `build/hsa-code-cache-fix/`; the exact comparison
uses `runtime-frozen/`, with only HSA replaced from the original comparison
runtime. `allocation-only-control/` is the negative control and must not be used
as an application runtime. The fixture is
`build/perf-q6-int8-shared-buffers/build/shared-buffers`.

The frozen fixed HSA library SHA256 is
`1a80cb4b515ac978882caa35f89a191f7f0bc20768b7e4a6c2341800654fff01`;
the unchanged fixture SHA256 is
`f18877fb0dd0b8620f2d29a5d055e3a30c6dd0ee5dcf4f181418e7fed4229e98`.
The ordinary local `build/hsa` library has also been rebuilt from the fixed
source. Rebuild that target when updating an existing checkout; installing a
host app alone does not update a separately built HSA runtime.

```sh
source scripts/amdgpu-llvm-env.sh
DYLD_LIBRARY_PATH="$PWD/build/hsa-code-cache-fix/runtime-frozen" \
  MAC_HSA_BLOCKED_POLL_US=64 \
  build/perf-q6-int8-shared-buffers/build/shared-buffers --run original
# Repeat with --run prefetch2, serially.
```

Logs under `build/tests/driver195-hardware/`:

- `q6-int8-shared-r3-all.log`: original failure.
- `q6-int8-shared-r3-isolated-k17408.log`: isolated shape passes.
- `q6-int8-shared-retain-code.log` / `q6-int8-shared-retain-data.log`: lifetime controls.
- `q6-int8-shared-allocation-only-control.log`: allocation-only negative control.
- `q6-int8-shared-cache-fix-{original,repeat,prefetch2}.log`: successful fixed runs.

This resolves the numerical failure blocking the shared-buffer comparison.
Full-model quality and performance gates still apply before promoting the
experimental INT8 or two-pass Q6 implementations.
