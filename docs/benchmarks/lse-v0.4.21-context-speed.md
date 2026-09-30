# LSE v0.4.21 context speed tests

## Configuration

Local macOS gfx1201 GPU. LSE source `29fa0eaa634e33fa17dcf78fceb6704625e36d03`; compiler patches from mac-amdgpu `2621a96367df09e07b2de43a9bad8b5d953769c2`. Release build, Q4 target and Q8 DFlash2 drafter, seven proposals, BF16 paged K/V, FP32 attention accumulation, temperature 0.6, top-k 20, top-p 0.95, seed 1234, batch and ubatch 1024, configured K/V limit 262100.

Each context uses a saved code-review prompt with the exact input-token count shown below. Every request starts a fresh server with empty K/V. Cold runs start with an empty kernel cache; warm runs restart with that cache retained. No prompt-prefix reuse occurs. No profiling or perplexity run was performed. One cold and one warm observation per size; no statistical speedup claim.

All requests generated 128 tokens. The server's decode rate covers the 127 tokens after the first token; first-token work is included in prompt time. Acceptance is accepted proposals divided by tested proposals. Settings are identical within each cold/warm pair.

## Warm kernel cache

| Input tokens | Prefill tok/s | Decode tok/s | Acceptance | Prefill ms | Decode ms | JIT compiles |
|---:|---:|---:|---:|---:|---:|---:|
| 2,048 | 559.9 | 28.0 | 80.8% | 3657.7 | 4536.1 | 0 |
| 4,096 | 559.8 | 34.3 | 82.5% | 7317.4 | 3706.6 | 0 |
| 8,192 | 514.8 | 27.4 | 77.9% | 15913.5 | 4631.4 | 0 |
| 16,384 | 448.4 | 25.6 | 77.4% | 36542.8 | 4963.7 | 0 |

## Empty kernel cache

| Input tokens | Prefill tok/s | Decode tok/s | Acceptance | JIT compiles | Total compile ms |
|---:|---:|---:|---:|---:|---:|
| 2,048 | 494.3 | 19.8 | 80.8% | 322 | 2158.7 |
| 4,096 | 529.6 | 25.1 | 82.5% | 237 | 1851.4 |
| 8,192 | 490.5 | 19.2 | 77.9% | 279 | 2504.0 |
| 16,384 | 433.2 | 18.8 | 77.4% | 286 | 2395.8 |

## Checks and limits

All eight requests reported zero host groups and zero host fallbacks. Warm runs reported zero JIT compilations. Each cold/warm pair produced identical response text and acceptance counters. These are independent context sizes, not one growing conversation. There was no crash or allocation failure in this sweep, but it does not qualify contexts above 16K or long-running memory stability.

This sweep measures the current release only. It does not establish an end-to-end improvement over v0.4.20. The isolated attention comparison is documented separately in [Automatic LICM and joint decode attention](auto-licm-joint-decode-2026-09-29.md).

Local logs, response files, request hashes, exact commands and complete server timing counters are retained in `build/release/pi-performance/kv-context-growth/v0.4.21-*`. The harness is `run-release-context-speed.py` in the same directory.

## Reproducibility

- Server SHA-256: `2620fe49b1c2b50253b44773c1ec6287d0e19c4263e14a325e7ed7620796a46c`
- Compiler SHA-256: `63bbfe76c3ec1a3155465e2c17588d6367ab37f4089edce5845f51bae683ea16`
- 2,048-token request SHA-256: `4e231be2d668518e81d3357497b958d0a819aa841f5482d3e5325cfc143d6663`
- 4,096-token request SHA-256: `ca2b7daef4c9fb4fbf4916991cafcae5e1b5bb37a805230981e21bc91ecfb07f`
- 8,192-token request SHA-256: `6f93474be922cb2bccd838201588ae98f8b043ef14786660859fd76864eb8736`
- 16,384-token request SHA-256: `8a0cd22ff52674a66402ff254e595e647349e2278073e2af970356c56b2ac57c`

## Q4 baseline: MTP and DFlash2 disabled

The same release binary and saved requests were tested without either draft model. Server health reported both disabled, and every timing record reported `spec_method=none`. All other model, context, sampling and batch settings match the DFlash2 sweep above. Each request generated 128 tokens; decode rate covers 127 after the first token. One observation per condition.

| Input tokens | Cold prefill tok/s | Cold decode tok/s | Warm prefill tok/s | Warm decode tok/s | Warm DFlash2 decode tok/s | Observed DFlash2 / baseline |
|---:|---:|---:|---:|---:|---:|---:|
| 2,048 | 523.5 | 22.7 | 580.3 | 23.3 | 28.0 | 1.20x |
| 4,096 | 543.2 | 22.4 | 567.0 | 22.0 | 34.3 | 1.56x |
| 8,192 | 492.9 | 19.2 | 520.7 | 18.9 | 27.4 | 1.45x |
| 16,384 | 437.7 | 20.7 | 452.7 | 21.3 | 25.6 | 1.20x |

All eight baseline requests completed without a crash or allocation failure and reported zero host groups/fallbacks. Warm baseline requests compiled zero kernels. Cold/warm baseline output text matched at each size. Baseline and DFlash2 use the same requests and seed but can generate different sampled continuations, so the ratios are observations for these short requests rather than a universal acceleration factor.

Baseline records and logs use `v0.4.21-baseline-*` in the local results directory. The harness is `run-release-baseline-context-speed.py`. Both harnesses start each request in a new process with empty K/V; model loading is outside reported prompt and decode timings.

## MTP=3

The same binary and requests were tested with the local Q8 MTP model and `--mtp-depth 3`; DFlash2 remained disabled. Temperature, top-k/top-p, BF16 K/V, batch/ubatch and output cap are unchanged. Each run starts a fresh server and empty K/V. One observation per condition.

| Input tokens | Cold prefill tok/s | Cold decode tok/s | Warm prefill tok/s | Warm decode tok/s | Acceptance | Warm JIT compiles |
|---:|---:|---:|---:|---:|---:|---:|
| 2,048 | 487.5 | 27.8 | 556.8 | 33.9 | 82.7% | 0 |
| 4,096 | 515.2 | 26.4 | 541.0 | 31.9 | 79.0% | 0 |
| 8,192 | 469.7 | 29.9 | 495.6 | 36.9 | 91.9% | 0 |
| 16,384 | 416.1 | 26.0 | 436.1 | 32.1 | 89.0% | 0 |

All eight MTP requests completed with zero host groups/fallbacks. Warm runs compiled zero kernels. Cold/warm output and acceptance counters matched at each context size. Detailed records use `v0.4.21-mtp3-*`; the harness is `run-release-mtp3-context-speed.py` in the same local results directory.

## Warm-cache comparison of all three modes

| Input tokens | Baseline PP/s | Baseline TPS | MTP=3 PP/s | MTP=3 TPS | DFlash2 PP/s | DFlash2 TPS |
|---:|---:|---:|---:|---:|---:|---:|
| 2,048 | 580.3 | 23.3 | 556.8 | 33.9 | 559.9 | 28.0 |
| 4,096 | 567.0 | 22.0 | 541.0 | 31.9 | 559.8 | 34.3 |
| 8,192 | 520.7 | 18.9 | 495.6 | 36.9 | 514.8 | 27.4 |
| 16,384 | 452.7 | 21.3 | 436.1 | 32.1 | 448.4 | 25.6 |

These are matching input workloads and sampling settings, with mode-specific sampled output. They are not a growing conversation, a sustained-generation benchmark, or a version-to-version comparison. All three modes used the same server/compiler binary hashes recorded above. No perplexity tests were run.
