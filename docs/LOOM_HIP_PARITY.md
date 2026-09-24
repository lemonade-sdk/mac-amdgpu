# Loom and HIP backend parity

LSE's model graphs call shared operations. Quantized storage interpretation,
packing, dequantization, tiling, and instruction selection belong in shared
kernel/operand code; HIP and Loom supply the corresponding lowering. A hardware
instruction table alone is not an implemented model execution path.

The working source is `~/Documents/Development/LemonSeed-Engine`.
`build/lse-macos-source` remains a compatibility symlink. The publication checkout
is `build/lse-publish`; isolated qualification candidates are under `build/perf-*`.
Changes in a candidate directory have not automatically been promoted to the
working or published engine.

## Confirmed reference behavior

- HIP exposes gfx1201 FP16, BF16, INT8, FP8, and BF8 WMMA spellings through
  `src/backends/hrx/hipc/hip_sources.cpp`. Shared matrix descriptors, lane maps,
  and operand widths live in `include/lse/math.hpp` and
  `include/lse/kernels/wmma.hpp`.
- `LSE_HRX_INT8=1` explicitly enables the existing Q4 activation-quantized
  dot/WMMA paths. Q6/Q8 do not become INT8 merely because the GPU supports it.
  Q6 now has shared staged BF16 and residual FP8/BF8 matrix implementations;
  accepted accuracy and matched timing records determine automatic selection.
- The validated Q6 path retains MLX affine six-bit storage and dequantizes
  within the kernel. Its current accumulation is FP32. No expanded full-size
  weight tensor is written back to VRAM. Eight-row prompt reuse shares decoded
  weights while retaining the original arithmetic sequence.
- A 128-bit access can cover 21 complete Q6 codes and two additional bits.
  Continuous packing requires correct boundary handling; alignment, row tails,
  and scale-group boundaries still apply. Wider accesses reduce instruction
  count, while six-bit storage reduces payload bytes versus eight-bit storage.
- AMD's [workgroup-staged example](https://github.com/ROCm/hrx-demos/blob/main/kernels/qwen3_vl/linear_fp8_block_scaled_bf16_wmma_m128n128_4wave_workgroup_staged.loom)
  targets gfx1100 and converts FP8 storage to BF16 workgroup tiles for BF16
  WMMA. It is a useful staging example, not a gfx1201 Q6 kernel to copy unchanged.

## Qualified implementation

The shared implementation is promoted to LSE main. Its Mac adapter
is synchronized with `~/Documents/Development/LemonSeed-Engine`. Experimental
changes use testing branches; a successful replacement removes superseded code
rather than retaining permanent old/new implementation switches. An explicit
user-facing precision capability, such as optional INT8 activation conversion,
is a separate policy.

| Area | R9700 evidence | Scope |
|---|---|---|
| Existing kernel epilogues | 15 complete-output GPU cases match materialized results bit for bit, including replay and guards | Existing primitive fusion; no precision change |
| Paired recurrence | All 32 complete-buffer hashes match the separate-output/state baseline | Same recurrence and GPU-carried state, fewer launches |
| Matrix instructions | 36 numerical/replay cases pass for F16, BF16, I8, mixed signedness, FP8 and BF8 | Includes tails, offsets, unchanged inputs and output guards |
| Matrix calibration | F16/BF16/I8 production probes pass full output/guard checks; cost model consumes all three measured rates | Completion-wall rates, not GPU timestamp measurements; FP8/BF8 remain unmeasured |
| KV buffer growth | Four guarded opaque-HRX copy sizes pass; full model completes 1,024 input plus 1,024 output tokens with KV2048 | Single local device, checked owner and stream retirement |
| INT8 policy | Both flag states pass 12 guarded Q4 numerical/replay cases; shared HIP/Loom selection and cache tests pass | Explicit opt-in; Q6/Q8 remain floating-point |
| FP8/BF8 conversion | Both formats pass all 256 decode bytes, rounding edges, exceptions, replay and guards | OCP formats on gfx1201, matching HIP conversion semantics |
| Automatic Q6 operands | Tiled BF16 wins four matched large projection shapes; final model returns identical fixture text | 87.28 PP/s and 12.61 TPS; unknown shapes/M1 retain the existing path |

The combined short model fixture matches the validated baseline text. The long
request and timings are recorded in [performance results](LSE_PERFORMANCE.md).
The repeat was interrupted at user request and is not counted as another long
qualification pass.

The accepted Q6 matrix path stages workgroup tiles in LDS and accumulates in
FP32. The E4M3 residual candidate also runs the actual model successfully on the
64-token logit fixture, but was slower than staged BF16. BF8 failed two
cancellation-safe numerical budgets. Neither is forced as the automatic winner.
The obsolete manual Q6 matrix modes are removed. A separate tiny-input GPU/CPU
discrepancy remains recorded in the performance report. GPU timestamp
instrumentation and decode arithmetic experiments remain outside this promotion.

## Device-side dispatch boundary

The pinned HRX source includes GPU-side AQL queue helpers. The current macOS HSA
runtime maps queue packets and metadata into shared CPU/GPU address space, but
its public doorbell is a host `Signal::storeHook` that calls `kickQueue`; the
DriverKit endpoint performs the hardware doorbell write. The periodic queue
service handles inactive/scratch requests rather than polling a device-published
doorbell. This path does not yet provide qualified GPU-originated child dispatch.

Device-side scheduling would require its own accessible notification path,
publication/completion ordering and queue-lifetime tests. It is a separate
capability from CPU batching and from fusing kernels to eliminate intermediate
memory traffic. This is an implementation boundary, not a claim that the GPU
cannot enqueue kernels.

See [performance conditions and results](LSE_PERFORMANCE.md) for the current
validated throughput and the limits of comparison with external llama.cpp runs.
