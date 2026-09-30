# Automatic staging for FP8 and BF8 operands

The default Loom `stage-matrix-operands` pass now handles converted operands,
including FP8 (E4M3) and BF8 (E5M2). It recognizes packed words plus per-vector
scales, and direct byte storage. Selection uses the source expression and access
pattern. It does not use model names or attention kernel names.

## Compiler changes

The pass redistributes the load and conversion expressions across the workgroup.
It retains the original byte decoding, scale multiplication, masks and fragment
addressing. Matrix operands and FP32 accumulation retain their original types
and evaluation order.

For uniform masks, the compiler constructs the complete eight-element run in one
branch. It proves which expressions have equal values across those elements and
shares their results within that branch. Eight packed values use two 32-bit word
loads and one scale load. Fragment pointer calculations are shared when their
addresses are equal. This also works when a vector crosses a fragment boundary.
Nonuniform tails retain their per-element masks.

The supported matrix fragment geometry remains RHS 16 x 16, eight FP16/BF16
payload elements per lane, wave32, and a 256 x 1 x 1 workgroup. FP8/BF8 storage is
decoded into those operands. This change does not change the selected matrix
instruction to a native FP8 matrix instruction.

## Measurements

Same workload for both compilers: gfx1201, 1,024 queries, 65,520 live keys,
capacity 65,536, 24 query heads, four KV heads, width 256, fragmented storage.
Each result averages eight GPU launches. The same saved logical source and data
were used for each format. The previous compiler is the FP16/BF16 migration
compiler; it does not stage these packed operand expressions.

| Storage | Previous compiler | Extended compiler | Less kernel time |
|---|---:|---:|---:|
| FP8 | 559.632 ms | 457.442 ms | 18.3% |
| BF8 | 557.701 ms | 458.607 ms | 17.8% |

All output bytes match for each before/after pair. Sampled FP64 reference checks
had maximum absolute errors of 6.39082026e-7 (FP8) and 8.65855784e-7 (BF8).
These are isolated attention timings, not end-to-end token-rate measurements.

The final FP8 kernel uses 120 VGPRs, 46 SGPRs, and no scratch spills. Compared
with the first unshared staging candidate, static global-load instructions fell
from 134 to 68. That first candidate took 668.752 ms and was not selected for
publication. Residency remains eight waves per SIMD, limited by LDS.

## Verification

- Fifteen compiler selection/rejection fixtures, with text round trips.
- Explicit packed FP8/BF8 tests require exactly two word reads and one scale read.
- Tests cover direct FP8/BF8 byte storage, FP32 conversion, masks, LDS budgets,
  shared staging lifetime, and rejection of loop-carried value dependencies.
- Final native FP8/BF8 poisoned-tail tests pass for widths 20/256, queries 2/3/7,
  and causal/sliding masks with permuted page tables.
- FP16/BF16 regression cases preserve their behavior.

Implementation: `patches/hrx/cooperative-matrix-operands.patch`. Both LSE release
builds apply the same pinned patch and include its hash in compiler provenance.
