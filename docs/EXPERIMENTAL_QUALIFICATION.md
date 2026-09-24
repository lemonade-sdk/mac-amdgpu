# R9700 experimental optimization qualification

The allocation-owner lifetime fix passes correctness and performance checks.
The two-pass Q6 prefill and two-iteration INT8 prefetch candidates remain on
experimental branches because they are slower than the current default.

## Matched model measurements

Qwen3.8-27B-MLX-6bit, R9700/gfx1201 on Apple Silicon, Loom, MTP disabled,
512 input / 129 output tokens, KV1024, flush64 and 64 microsecond polling.
Every process receives two warmup requests followed by three measured requests.
All measured requests have zero JIT compilations and zero disk-cache hits.
The HSA code-cache fix and current Mac same-queue HRX runtime are identical
across every arm. The measured rates exclude warmup.

| Candidate | Prompt tokens/s | Decode tokens/s | Decision |
| --- | ---: | ---: | --- |
| Current default | 88.57 | 17.46 | Baseline |
| Allocation-owner lifetimes | 88.63 | 17.45 | Publish correctness fix; throughput unchanged |
| Two-pass centered Q6 prefill | 76.72 | 17.47 | Keep experimental: 13.4% slower prefill |
| Two-iteration Q6 INT8 prefetch | 88.61 | 16.32 | Keep experimental: 6.5% slower decode |

Baseline and lifetime results are medians of six measured requests each, from
both baseline-before-candidate and candidate-before-baseline ordering. The
arithmetic experiments each have three measured requests. All thirty responses
are identical and all six servers shut down cleanly. These results establish
neither a new arithmetic speedup nor matched llama.cpp or HIPC parity.

## What is being published

An inplace result can alias an earlier temporary allocation through one or more
views. The planner now follows that ownership chain, including inplace inputs
other than input zero, so a later reader keeps the original allocation alive.
Malformed alias cycles disable slot reuse instead of guessing a lifetime.

The regression reproduces premature reuse on the old implementation and passes
16 combinations of aliases, view sizes, escaped roots and inplace input indices
with the fix. It also checks a malformed cycle. Five fresh CPU-only Mac suites
pass: graph, CPU backend, pointwise fusion, scheduler epilogues and inplace
ownership. The new ownership test is included in the Mac CI test list.

The Mac build pins mac-amdgpu `6dd8c2a74d61c12e7cf5f568296bd9c4b6fe7c45`,
which includes automatic qualified same-queue dependency barriers and the HSA
executable code-cache synchronization fix. The latter fixes the shared-buffer
comparison's failure when changing executable shapes; it is not an INT8
performance improvement.

## Long-request check

The baseline and lifetime-fix servers each complete two requests of exactly
1,024 input and 1,024 output tokens, KV2048, MTP disabled. All four responses
are byte-identical and both servers exit cleanly. This is a correctness and
repeatability check; compilation during these requests excludes them from
steady-state throughput claims. Evidence: `{baseline,owner}-1k1k/result.json`
and `long-comparison.log` in the same local qualification directory.

## Experiments that did not pass

Two-pass Q6 passes the unchanged 0.005 full-logit relative-L2 gate on the fixed
code, math and story contexts, with all 248,320 logits finite, matching argmax,
and bit-identical repeats. Its numerical suite passes 37 cases with nine
repeats. Correctness does not compensate for its measured prefill slowdown.

The INT8 prefetch candidate passes its 71-case numerical suite and the corrected
same-buffer FP32/INT8 comparison. Its full-model speed screen fails. The complete
multi-context, early/late decode-logit gate remains unqualified; matching this
one benchmark response does not replace it. The released `LSE_HRX_INT8=1`
policy continues to cover the existing Q4 path, not this Q6 experiment.

## Reproduction and evidence

The local driver checkout preserves `build/qualification-196/`:

- `performance-summary.json`: all per-request rates and combined medians.
- `qualification-inputs.json` and `binaries.json`: runtime/source/binary hashes.
- `{baseline,owner}-perf-{1,2}/result.json`: matched forward/reverse cohorts.
- `{two-pass,int8-prefetch2}-perf-1/result.json`: experimental speed screens.
- `host-tests.log`: fresh host regression results.
- `run-performance.py`: serial benchmark driver, invoked with explicit `--run`.

Arithmetic sources remain at `perf/q6-centered-repair-model` (`f592e58`) and
`perf/q6-int8-prefetch2` (`bd91750`). Allocation-owner source was qualified from
`testing/inplace-owner-lifetimes` (`345050b`). Frozen controls are preserved;
rebuilding an executable or runtime invalidates its recorded identity.
