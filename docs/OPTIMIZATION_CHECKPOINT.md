# Optimization checkpoint: 0.1.96

## Published default

The Mac HRX adapter now uses a standard AQL barrier for an eligible same-queue
dependency. Eligibility requires the exact physical queue, a published latest
device-only epoch, and all earlier host actions drained. Cross-queue, failed,
future and otherwise unproven dependencies retain software deferral. There is
no experimental environment switch and no change to native CPU/GPU atomic support.

The matched off/on/on/off comparison measured 16.6678 versus 17.4067 TPS
(+4.43%), with twenty identical responses and no compilation or disk-cache
misses in twelve measured requests. The final ordinary-default runtime measured
**88.9301 PP/s and 17.5059 TPS**, median of three requests after two warmups,
512 input / 129 output, KV1024, MTP disabled, flush64/poll64. All five responses
match the control and shutdown succeeds. This does not establish llama.cpp parity.

Final default hardware checks pass ring wrap (8,193 steps), alternating physical
queues (513 steps), transitive host callbacks (257 steps), and intentional
ABORTED propagation. Separate tests establish actual AQL, notification and
kernarg backpressure. Host resolver/packet tests pass ASan/UBSan on both Mac
and non-Mac policies. The build runs these checks without submitting GPU work.

Version 0.1.96/build 196 includes this userspace runtime improvement; firmware and
GPU initialization sequences are unchanged. The host app builds and its
development signature verifies. The runtime measurements used the installed
driver 195. Installing a new host bundle alone does not rebuild LSE's HRX library;
use `scripts/build-hrx-macos.sh` when building from source.

## Saved experimental branches

These branches are not merged into the LSE default:

- [Two-pass Q6 model selection](https://github.com/Geramy/LSE/tree/perf/q6-centered-repair-model):
  shared HIP/Loom graph expansion, separate values/flags and out-of-place repair.
  All 37 numerical cases pass nine repeats, with hashes identical to the previous
  accurate implementation. Automatic model selection passes the fixed code
  context at relative logit L2 0.0000495955, below the unchanged 0.005 gate.
  Math/story model checks and compilation-free full-model timing remain pending.
- [Inplace allocation-owner lifetimes](https://github.com/Geramy/LSE/tree/testing/inplace-owner-lifetimes):
  isolated planner fix; the original fails and the candidate passes 16 alias
  scenarios, including nonzero inplace inputs, nested views and escaped roots.
  Malformed cycles disable recycling. Four existing scheduler suites pass.
  This patch is independent of the out-of-place matrix experiment.
- [Two-iteration INT8 prefetch](https://github.com/Geramy/LSE/tree/perf/q6-int8-prefetch2):
  all 71 numerical cases pass nine repeats with hashes matching the prior INT8
  candidate. Performance remains unqualified. Earlier variants regress badly
  on large FFN shapes. The direct same-input-buffer failure after switching
  shapes is resolved by synchronizing GPU code caches after executable upload
  in the Mac HSA loader. The original test passes twice and the prefetch variant
  passes once, across all three shapes, with unchanged numerical checks and
  normal executable release. This corrects a loader bug; it does not establish
  an INT8 performance improvement. See [code-cache validation](HSA_CODE_CACHE_VALIDATION.md).

## Local continuation evidence

All paths are relative to the mac_amdgpu checkout. Preserve frozen comparisons.

- `build/hrx-prefix-current/model-comparison.json`: complete off/on/on/off cohort.
- `build/hrx-prefix-default/final-qualification.json`: final default traces/model.
- `build/perf-q6-centered-repair-model/run-quality.py`: remaining fixed-context
  model checks, explicit `--run`; use the qualified runtime directory
  `build/rocprofmac-fp32-default/runtime-profile-ready`.
- `build/perf-q6-int8-shared-buffers`: preserved failures plus r3 diagnostic,
  which prints first mismatch details and supports isolated shape runs.
- `build/tests/driver195-hardware`: raw model, numerical and dependency evidence.
- `build/release196/Build/Products/Debug/MacAMDGPUHost.app`: signed local app.

Run one GPU experiment at a time. Further promotion requires full-model quality
and actual speed improvements; passing a small numerical test is insufficient.
