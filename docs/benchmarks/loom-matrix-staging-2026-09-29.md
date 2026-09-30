# Automatic matrix operand staging

Loom now runs `stage-matrix-operands` in its default source optimization pipeline,
after private fragment promotion and before vector lowering. The pass matches
matrix operand producers. It does not inspect model names, attention names, or
LSE kernel names. LSE can express logical operand reads without specifying the
cooperative load schedule.

## Supported schedule

The first schedule handles RHS 16 × 16 matrix fragments with eight FP16 or BF16
elements per lane, 32-lane subgroups, and a 256 × 1 × 1 workgroup. It redistributes
independent scalar reads into contiguous eight-element loads. A padded LDS tile
restores the original lane layout before matrix execution. FP32 accumulation
order is unchanged.

The pass proves uniform barrier participation, independent payload components,
contiguous aligned loads, and predicates that remain constant across each vector
load. It checks the total workgroup storage budget. Already-coalesced patterns,
unsupported layouts, unsafe motion, and nonuniform vector tails remain unchanged.
This is a reusable transformation with one supported schedule, not a claim that
all kernels benefit from staging.

A selected schedule needs 4,352 bytes of LDS. Sequential operations of the same
operand type reuse that allocation. The compiler adds a barrier before each
consumer and before reuse. Compile reports record selection and rejection reasons.

## GPU measurement

Test: gfx1201, BF16 fragmented KV, 1,024 query rows, 65,520 live keys, capacity
65,536, 24 query heads, four KV heads, head width 256. Each timing is the average
of eight launches from the same harness. No model perplexity run was used.

| Implementation | Kernel time | VGPRs | Scratch spills |
|---|---:|---:|---:|
| Explicit LSE staging with existing cache policy | 360.172 ms | 136 | 0 |
| Logical LSE reads with automatic compiler staging | 316.626 ms | 120 | 0 |

The integrated compiler path took 12.1% less time. All output elements were
bit-identical. A sampled FP64 reference comparison had maximum absolute error
7.26163218e-7 for both paths. This is an isolated kernel result, not an HTTP
throughput claim.

The integrated code contains four `global_load_b128` instructions and 20 VOPD
components. Total LDS remains 29,120 bytes. Residency remains eight waves per
SIMD, limited by LDS. The pass does not pin data in L2 or Infinity Cache.

## Verification

- Nine compiler golden fixtures and parse/print round trips: FP16, BF16,
  already-contiguous access, insufficient LDS, incompatible workgroup dimensions,
  divergent participation, row tails, column tails, and shared staging lifetime.
- LSE attention selection and source emission tests passed.
- Native FP16/BF16 tests passed for widths 20 and 256, query counts 2, 3, and 7,
  causal/sliding masks, padded queries, permuted pages, and poisoned unused data.
- Equivalent packed FP8/BF8 correctness cases passed. Those formats do not use
  this new half-storage vector schedule.

The persistent compiler implementation is in
`patches/hrx/cooperative-matrix-operands.patch`. The macOS HRX build script applies
it to the pinned source. LSE release build scripts must apply the same patch and
include its hash in compiler provenance. Compiler image identity is part of the
LSE kernel cache key.

## Matched short-context HTTP check

A fresh-server comparison used the same 1,024-token prompt, 128 generated tokens,
Q4 target, Q8 DFlash2 draft, seven proposals, BF16 KV, temperature 0.6, top-k 20,
top-p 0.95, and an empty kernel cache for each binary.

| Implementation | Prefill | Decode |
|---|---:|---:|
| Explicit staging | 406.705 tok/s | 27.375 tok/s |
| Compiler staging | 410.247 tok/s | 27.788 tok/s |

Generated text was identical. Both runs accepted 102 proposals over 25 passes,
compiled 237 kernels, submitted 50,228 device groups, and had zero host fallbacks.
This single pair checks integration; it does not establish a short-context speedup.
