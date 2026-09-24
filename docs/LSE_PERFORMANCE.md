# LSE performance baseline and comparison plan

LSE runs the local Qwen3.8-27B six-bit text checkpoint on the Radeon AI PRO
R9700 through Loom, HRX and the macOS HSA runtime. **Performance parity with
llama.cpp has not been demonstrated.** The current measurements establish a
working baseline and identify work to profile; they are not a matched benchmark
against another engine.

## Recorded local baseline

The driver 195 32-token run is preserved in:

- `build/tests/driver195-hardware/qwen-performance-baseline/preflight.json`
- `build/tests/driver195-hardware/qwen-performance-baseline/inference.log`
- `build/tests/driver195-hardware/qwen-performance-baseline/result.json`

These are local build artifacts, not files distributed with the repository.
The recorded command used `hrx:0`, the Loom dialect, KV capacity 128, greedy
sampling, MTP disabled, and the raw prompt `The capital of France is`.

| Measurement | Recorded result |
| --- | ---: |
| Prompt tokens | 5 |
| Prefill | 3.96 s; 1.26 tokens/s |
| Generated output tokens | 32 |
| Subsequent decode steps | 31 |
| Decode time/rate | 10.867 s; 2.85 tokens/s |
| Whole process elapsed | 25.312 s |
| Text weight loading | 8.499 s |
| Device groups | 83,057 |
| Host groups / CPU fallbacks | 0 / 0 |
| Process exit | 0; no timeout or interruption |

The first output token comes from prefill, so 32 output tokens contain 31
subsequent decode steps. The text weight loader bound 1,847 tensors into 11 slabs,
reserving 21.731 GiB. It explicitly excluded 333 vision tensors; this is a text-only
run. The run summary does not itself certify driver retirement: cleanup must also
be checked using driver state after the process exits.

This was a fresh process with cold graph construction, **not a wholly cold shader
cache**: the log records 111 disk JIT hits, 82,948 memory hits and zero shader
compilations. Its five-token prefill is not comparable to a warmed PP512 result.
Model loading and whole-process elapsed time must not be substituted for decode
time, or vice versa.

### Host preparation is a measured cost

The same run reports 67 step spans totaling 13,621.6 ms:

| Span | Time |
| --- | ---: |
| Partition | 2,109.9 ms |
| Schedule | 12.0 ms |
| Emit | 4,102.1 ms |
| JIT lookup | 1,346.3 ms |
| JIT compile | 0.0 ms |
| Bind | 33.8 ms |
| Submit | 44.3 ms |
| Host execution | 0.0 ms |
| Host wait | 5,891.2 ms |
| Readback | 52.0 ms |
| Unattributed | 30.0 ms |

Partition, schedule, emission, JIT lookup, binding and submission account for
approximately 7.65 s of host-side preparation versus 5.89 s in the host-wait span.
This supports profiling repeated graph preparation and cache lookup before
assuming GPU arithmetic is the only bottleneck. Host-wait time is **not** a
hardware measurement of GPU execution time: it can include completion handling
and scheduling delays. These spans cover the recorded step sequence, not just
its 31 decode steps, and must not be divided by 31 to claim a per-token breakdown.
No projected speedup follows from simply removing one column.

## Exact model and quantization identity

The local directory is:

```text
~/.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit
```

Its `config.json` declares `Qwen3_5ForConditionalGeneration` / `qwen3_5`, 64
layers, hidden size 5120, intermediate size 17408, 24 query heads, 4 KV heads,
attention head width 256, rotary width 64, and Gated DeltaNet value heads 48 of
width 128. The architecture name in LSE's log does not rename the checkpoint to
Qwen3.5: Qwen3.8 uses that architecture family. The publisher provides the
[MLX six-bit checkpoint](https://huggingface.co/lmstudio-community/Qwen3.8-27B-MLX-6bit).

The local quantization is **MLX affine 6-bit, group size 64**, with packed weights,
scales and biases. GGUF Q6_K has a different representation, using 256-element
superblocks with subgroup scales. Unsloth UD-Q6_K_XL also mixes tensor precision.
Equal nominal bit counts do not imply identical weights, arithmetic, memory
traffic, quality or performance. The [GGUF publisher lists the different
Qwen3.8 variants](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF).

A comparison using a GGUF derived from the same original checkpoint is a useful
**engine-and-quantization comparison**, not an isolated driver comparison.
Converting an already quantized MLX checkpoint by dequantizing and requantizing
also changes its weights. An exact-weight cross-engine test would require a
verified representation-preserving path; none is qualified here. Pin and hash
both models instead of treating their directory names as sufficient identity.

## External reference, not an achieved target

A benchmark author reports the following for **one ASRock R9700 Creator 32GB**,
Windows 11, OCuLink/DEG1, Vulkan with full GPU offload, Flash Attention enabled,
Q8_0 KV, and llama.cpp build 10612 (`758443071`):

| Checkpoint/format | Test | Author-reported rate |
| --- | --- | ---: |
| Qwen3.8-27B UD-Q6_K_XL | PP512 | 655.10 tokens/s |
| Qwen3.8-27B UD-Q6_K_XL | TG128 | 22.72 tokens/s |

The author separately reports MTP experiments; those are not the TG128 baseline.
The post describes a 128K context configuration but does not supply enough raw
benchmark output, model hashes and repetition details to reproduce every setting.
These are first-hand published measurements, not independently reproduced here.
[Original benchmark report](https://www.reddit.com/r/LocalLLM/comments/1vzcuwa/r9700_32gb_qwen3827b_q4_vs_q6_and_some_mtp_testing/)

Our single-card macOS/Thunderbolt MLX affine Q6 run differs in quantization, host,
backend, KV type, context/workload, cache state and timing boundaries. Dividing
these figures by our five-token or31-step measurements would not establish a
valid performance ratio. A separate [Qwen3.8 Q6_K experiment with pinned software
and model hash](https://github.com/ggml-org/llama.cpp/discussions/27082) uses **two**
R9700s; its results must not be presented as a single-card baseline.

## Reproducible qualification

Start with workloads inside the currently qualified KV128 capacity. For example,
measure 16-, 32- and 64-token prefills, then 32 subsequent decode steps beginning at
a 64-token context. That means requesting 33 output tokens from LSE and checking
that EOS did not reduce the actual number. PP512/TG128 requires separately
qualifying a larger KV capacity first; it is not covered by the short run above.

For every comparison, preserve:

1. **Model identity:** publisher/revision, configuration and tokenizer hashes,
   shard/GGUF hashes, tensor quantization and exclusions. Use the same original
   checkpoint and explicitly identify any conversion or mixed precision.
2. **Workload:** exact prompt token IDs including special tokens, input length,
   context depth, actual decode steps, batch size and concurrency. Keep MTP off.
   Use raw completions to avoid different chat templates, or record the expanded
   template and tokens. Record EOS and truncation rather than silently accepting
   shorter runs.
3. **Timing boundaries:** separate loading, tokenization, graph construction/JIT,
   prefill, first-token latency, subsequent decode, sampling and end-to-end time.
   `llama-bench` excludes tokenization and sampling; LSE generation timing
   includes some surrounding host work. Align boundaries or label the difference.
   [llama-bench documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/llama-bench/README.md)
4. **Warmup/cache:** report cold-process and warm-resident results separately.
   Warm each tested shape before timing. Preserve compiled-kernel caches but
   clear sequence/KV state between independent prompts; prefix reuse is a
   separate test. Record memory/disk JIT hits and compile counts. Repeat at least
   five measured samples and retain each sample, not only a fastest result.
5. **Memory/execution:** record KV capacity and element type, recurrent-state
   precision, batch/ubatch sizes, all-layer device offload, CPU fallback counts,
   and allocation failures. LSE currently uses FP32 state/KV buffers; a llama.cpp
   F16 or Q8 KV result has another explicit difference.
6. **Platform:** driver/runtime/compiler/engine revisions and binary hashes,
   single-card identity, OS, CPU, PCIe/Thunderbolt topology, negotiated link,
   BAR aperture, power/clock settings and thermal state. Do not silently compare
   a slotted card or two-card tensor split to this eGPU.
7. **Correctness/lifecycle:** retain output tokens and errors, require finite
   results, no forbidden host fallback, successful process exit and verified
   driver retirement. A fast failed/shortened run is not a sample.

### Baseline tooling route

The inspected local llama.cpp checkout is
`~/Documents/Development/llama.cpp` at
`a94d563ed801d1da1b8c2432946de07d0231bb3d`. No built `llama-bench` or Qwen GGUF was
found in the inspected model/tool locations. LM Studio's installed llama.cpp
backend 2.37.0 declares Apple/Metal and GGUF support; it does not execute through
this R9700 HSA/HRX driver. Our current HSA runtime alone does not provide a
llama.cpp HIP backend on macOS.

A practical R9700 reference therefore uses a separately recorded Linux HIP or
Windows/Linux Vulkan installation and a pinned Qwen3.8 GGUF. On a Linux host
with ROCm and a single R9700, the inspected llama.cpp build interface is:

```bash
HIPCXX="$(hipconfig -l)/clang" HIP_PATH="$(hipconfig -R)" \
cmake -S /path/to/llama.cpp -B build/llama-r9700 \
  -DGGML_HIP=ON -DGPU_TARGETS=gfx1201 -DCMAKE_BUILD_TYPE=Release
cmake --build build/llama-r9700 --target llama-bench -j8

HIP_VISIBLE_DEVICES=0 build/llama-r9700/bin/llama-bench \
  -m /path/to/pinned-Qwen3.8-27B-Q6_K.gguf \
  -ngl 99 -sm none -fa on -ctk f16 -ctv f16 \
  -p 16,32,64 -n 0 -b 64 -ub 64 -r 5 -o json

HIP_VISIBLE_DEVICES=0 build/llama-r9700/bin/llama-bench \
  -m /path/to/pinned-Qwen3.8-27B-Q6_K.gguf \
  -ngl 99 -sm none -fa on -ctk f16 -ctv f16 \
  -p 0 -n 32 -d 64 -b 64 -ub 64 -r 5 -o json
```

These commands are a proposed reference procedure, **not runs performed here**.
They explicitly choose F16 KV and GGUF Q6_K, so do not claim exact quantization or
KV parity. Save the exact engine's `--help`, build metadata and raw JSON with each
run; CLI capabilities can change across revisions.

For the local benchmark runner, prefer a resident-model sequence with explicit
warmup, fresh sequence state, exact token counts and per-sample structured results.
Capture driver state before and after, preserve stderr and span/JIT counters, and
reject timeouts, fallback or early termination. Keep cold-start loading and
steady-state inference in distinct result fields. Cross-engine numeric comparison
should remain disabled until the required workload and timing metadata match.


## Warm resident benchmark runner

The loopback runner performs one untimed warmup request followed by measured
requests on the same loaded model. Each request starts a fresh sequence; outputs
and usage counts must match exactly under greedy sampling. It records individual
samples, medians, binary/config/prompt hashes and normal shutdown in `result.json`.

```sh
LSE_TIME_SPANS=1 python3 scripts/run-lse-server-smoke.py --run \
  --benchmark-repeats 5 --generated-tokens 33 \
  --log-dir build/tests/qwen-resident-benchmark
build/hsa/mac-hsa-info
```

The default France prompt has five tokens on this checkpoint. Use `--prompt-file`
for another exact text fixture, `--kv-len` for its total context capacity, and
`--request-timeout` for longer measurements. The returned prompt count is the
actual tokenizer result, not a count inferred from characters. A 33-token output
cap requests 32 subsequent decode steps. EOS can produce fewer; the summary's
`all_requested_decode_steps_completed` must be true for a fixed-length comparison.
`qualified_http_gpu` alone only establishes successful deterministic requests and
server exit. Longer settings require their own numerical/lifecycle qualification.

The initial three-sample resident baseline is in
`build/tests/driver195-hardware/qwen-resident-performance-baseline/result.json`:
32 decode steps per sample, rates 2.853677, 2.850951 and 2.857027 tokens/s
(median 2.853677). All four requests, including warmup, returned identical output;
server exit was zero and a separate discovery probe reported driver 195 stage 0.
Warm five-token prompt processing had median 2.214130 tokens/s. The warmup was
excluded from those medians. This is a local optimization baseline, not PP512 or
TG128 parity evidence.

### Controlled batching and host-wait experiments

The following driver 195 experiments use that same five-token prompt, checkpoint,
KV128, HRX/Loom path, greedy sampling and disabled MTP. Each row has one excluded
warmup and three measured requests, each producing **33 tokens / 32 decode steps**.
All requests returned exactly the same output across these runs, exited normally,
and were followed by a driver check reporting stage 0. These are short resident
workloads; they do not qualify PP512, TG128, or long-context performance.

| Configuration | Flush interval | Blocked poll interval | Median decode tokens/s | Median warm prompt tokens/s | Raw result directory under `build/tests/driver195-hardware/` |
| --- | ---: | ---: | ---: | ---: | --- |
| Original server, standard runtime | 16 (default) | 1000 µs (default) | 2.853677 | 2.214130 | `qwen-resident-performance-baseline/` |
| Source-cache server, standard runtime | 16 (default) | 1000 µs (default) | 2.848566 | 2.215652 | `qwen-cache-performance/` |
| Original server, unchanged binary | 64 (override) | 1000 µs (default) | 4.600654 | 2.520894 | `qwen-flush64-baseline/` |
| Source-cache server, experimental runtime control | 64 (override) | 1000 µs (explicit override) | 4.612161 | 2.501338 | `qwen-cache-flush64-poll1000-control/` |
| Same source-cache server and experimental runtime | 64 (override) | 64 µs (override) | 5.947544 | 6.483711 | `qwen-cache-flush64-poll64/` |

Each directory contains `result.json`, the four `response-*.json` files, and
`server.log`. Full per-sample rates, prompt/model hashes, command arguments and
environment values are retained there. In particular:

- The original server SHA-256 is
  `f0242463cf5057b71294f65ea501d6c2f491415ed735a26fbfccfb1ee4755633` in
  both the default and flush-64 rows. The batching override is
  `LSE_FLUSH_INTERVAL=64`; it changes when queued dispatches are submitted.
- The source-cache server SHA-256 is
  `3d6ecdf75ef0928c2b765e0f25ee94a89513497da1aa4d5fa077660b34e7a114`.
  Both polling rows use exactly the same
  `build/hsa-wait-perf/libhsa-runtime64.dylib`, SHA-256
  `244f3943c333a307a65e62a9992683d65c1b4a089ce66114b4952bc4ae33c1b0`.
  Their controlled difference is `MAC_HSA_BLOCKED_POLL_US=1000` versus `64`.
  This changes host polling cadence, preserving wait conditions, timeout
  clamping and memory ordering. The requested sleep interval is not a guaranteed
  operating-system wakeup latency.
- All rows record `LSE_REQUIRE_DEVICE_KERNELS=1` and `LSE_TIME_SPANS=1`.
  The source-cache/default-flush run additionally records `LSE_TIME_STEPS=1`;
  account for that diagnostic difference when comparing host costs.

Source caching alone showed **no decode throughput gain** in this measurement:
2.848566 versus 2.853677 tokens/s. Across all four requests, including warmup,
the accumulated emission span fell from 17,089.5 ms to 1,911.5 ms. Over the same
span sequence, host wait increased from 24,365.3 ms to 36,827.9 ms, while total
step time stayed near 52 seconds. CPU preparation overlaps queued GPU execution;
moving time between preparation and waiting does not imply an equal wall-clock
saving. Neither host span measures GPU execution time directly.

The same-binary polling control supports attributing the measured 4.612161 to
5.947544 tokens/s change to the shorter host wait cadence for this workload.
The warm prompt rate of 6.483711 tokens/s covers only five prompt tokens; it must
not be advertised as PP512. These settings remain **experimental overrides**:
the standard flush default is still 16 and the standard blocked-poll default is
still 1000 µs. No default promotion or broader workload qualification is implied.
Q6 kernel and pointwise-fusion performance are not qualified by this table.

### Whole-token submission versus a 64-dispatch interval

A later comparison includes the pointwise-fusion candidate and holds both
executables fixed: server SHA-256
`83dd028bf99456372171be747e71fef1e1c09a0abfc3ea65145b8660a97f3633` and the
experimental HSA SHA-256 `244f3943c333a307a65e62a9992683d65c1b4a089ce66114b4952bc4ae33c1b0`.
Both use `MAC_HSA_BLOCKED_POLL_US=64`, the same five-token prompt and 33-token
output request, one excluded warmup and three measured requests.

| Submission setting | Median decode tokens/s | Median warm prompt tokens/s | Raw directory under `build/tests/driver195-hardware/` |
| --- | ---: | ---: | --- |
| `LSE_FLUSH_INTERVAL=0` (no periodic dispatch-count flush; work submitted at required synchronization) | 5.593398 | 6.249660 | `qwen-pointwise-flush0-poll64/` |
| `LSE_FLUSH_INTERVAL=64` | 6.082109 | 6.549340 | `qwen-pointwise-flush64-poll64/` |

Whole-token batching was slower in this comparison. Removing periodic submission
is therefore not a measured improvement for this workload. Each directory retains
the per-request `result.json`, responses and `server.log`; both runs completed
normally with matching output. This comparison isolates submission interval in
the fusion candidate, not the effect of fusion against the earlier server.
These measurements do not change the standard defaults.

### Independent MLX correctness reference

`build/tests/qwen-mlx-reference/result.json` records a successful independent run
of the **same local MLX affine Q6 checkpoint** using Apple Metal MLX 0.32.0 and
MLX-LM 0.31.3. The opt-in runner is `scripts/lse-mlx-reference.py`; the result
includes full checkpoint/config/tokenizer SHA-256 hashes, prompt IDs, each
generated ID and top-10 raw logits, and the installed model implementation hash.
It used the raw France prompt, greedy argmax, no chat template, no MTP and no KV
quantization, producing all 33 requested tokens without EOS termination.

Its prompt IDs are `[760, 6511, 314, 9338, 369]`. The independent comparison now
passes **all five prompt IDs and all 33 generated IDs exactly**, as well as the
decoded text, spacing and newlines. Evidence is retained in
`build/tests/driver195-hardware/qwen-exact-token-ids.log` and
`build/tests/driver195-hardware/qwen-token-comparison.json`; the LSE process exited
zero and the subsequent driver check reported stage 0. This establishes the
tested greedy sequence, not general logit agreement. MLX uses its native intermediate
precision whereas this LSE path uses FP32, so raw logits also need an explicit
tolerance and attention to near-tied choices. Apple Metal timing is an accuracy
reference diagnostic, not an R9700 throughput measurement or llama.cpp parity
result.


## Q6 panel optimization and longer-prompt evidence

The gfx1201 Q6 path now reuses decoded weights across prefill rows and rotates
privately staged FP32 activation panels. CPU and guarded GPU tests preserve
FP32 accumulation order; unsupported layouts and caller-owned panels retain
their existing path. Large-row single-token tiling was separately tested and
rejected because it was slower; it is not part of this result.

The same flush64/poll64 settings, KV128, no MTP, one resident warmup and three
measured requests gave:

| Prompt tokens | Generated tokens / subsequent decode steps | Warm median PP/s | Median decode tokens/s |
| ---: | ---: | ---: | ---: |
| 5 | 33 / 32 | 14.179178 | 10.118556 |
| 64 | 33 / 32 | 54.007206 | 6.750825 |

All requests used strict device execution, matched repeated output, exited
normally and were followed by a separate stage0 check. The short prompt's output
also matches the preceding implementation. Evidence directories are
`build/tests/driver195-hardware/qwen-q6-combined-flush64-poll64` and
`qwen-q6-pp64-flush64-poll64`; they record executable/runtime hashes and timing
boundaries. These rates differ by context and are not PP512/TG128 parity.

The 64-token prompt exposed a native MLX BF16 tie at generated index 9: tokens
5440 (China) and 4725 (South) both had raw logit 18.375. Native MLX chose South;
LSE's FP32 path chose China, so the subsequent sequences diverged. A second
independent run cast only floating parameters/scales/biases to float32, preserving
every packed integer weight object, shape and affine 6-bit/group64 module setting.
That reference exactly matches all 64 prompt IDs and 33 generated IDs from LSE.
Internal fusion/accumulation may still differ between engines; this is evidence
for the two tested continuations, not universal numerical equivalence.

The extended reference script records its precision audit and deterministic
top-k tie ordering. Evidence: `build/tests/qwen-mlx-reference-pp64/result.json`,
`build/tests/qwen-mlx-reference-pp64-f32/result.json`, and
`build/tests/driver195-hardware/qwen-pp64-token-comparison.json`.

### Automatic decode submission selection

Eligible macOS gfx1201 single-device, non-MTP decode now measures 16/64/256/0
submission intervals during ordinary model work. It never repeats a decode
step. Cold compilation, CPU fallback and actual repartitioning invalidate a
sample; stable medians must improve on the baseline by at least 5%. Explicit
`LSE_FLUSH_INTERVAL` wins, and `LSE_AUTO_BATCH=0` disables selection. Prefill and
other workloads retain baseline behavior. Decisions are local to the process,
model, context bucket and dispatch signature.

The same short France workload, KV128, one resident warmup and three measured
requests produced identical text and clean server shutdown:

| Poll interval | Selected flush interval | Median decode tokens/s | Evidence directory |
|---|---|---|---|
| 64 µs override | 64 | 10.127645 | `qwen-adaptive-stable-poll64` |
| 1000 µs default | 0 | 6.967066 | `qwen-adaptive-default-poll` |

Directories are under `build/tests/driver195-hardware/`. Selection is measured,
not a fixed promise that one interval is best for every context or polling
policy. Whole-step host timing includes dispatch, synchronization and sampling;
it is not a GPU timestamp or a comparable llama-bench kernel rate.

### Eight-row Q6 prefill reuse

For gfx1201 FP32-activation/BF16-affine Q6 prompts of at least 32 rows, the
validated schedule now uses eight rows per 512-K tile. This retains 16 KiB LDS
and each lane's original FP32 FMA sequence. The isolated qualification passed
63 full-output cases with identical baseline hashes and preserved guards/input
bytes. Four large matrix shapes improved host dispatch-to-retirement times by
19–21%; compiled VGPR use rose from 55 to 88 without scratch spills.

The full-model PP64 fixture with fixed flush64/poll64, KV128 and 33 generated
tokens reached median **63.122215 prompt tokens/s**, versus 54.007206 before.
Median decode was 6.764287 tokens/s (expected to remain unchanged by this
prefill-only change). All four requests returned identical text and the server
exited normally; the driver was independently checked at stage0. Evidence:
`build/tests/driver195-hardware/qwen-rows8-pp64/` and `q6-rows8-*` logs.
