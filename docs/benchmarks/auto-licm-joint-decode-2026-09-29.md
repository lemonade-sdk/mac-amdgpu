# Automatic LICM and joint decode attention

## Scope

The compiler candidate runs the existing `licm` pass after vector-memory scalarization, view linearization, and canonicalization. Existing motion analysis governs aliases, conditional execution, and barriers. No masked-load speculation was added.

The attention candidate groups two query heads and four token rows in one workgroup. It applies to four through eight query rows, at least 8,192 table positions, and six query heads per KV head. The table remains the source of dispatch choices. Each group reads each K/V value once for its eight query/head combinations. The prior four-row tile reads it separately for each head.

## Matched measurements

Device: gfx1201. Shapes: batch 1, 24 query heads, four KV heads, head dimension 256, fragmented cache storage, causal attention. Both variants use the LICM candidate compiler. Each time is wall-clock duration of 32 queued partial-plus-merge dispatch pairs followed by synchronization, divided by 32; it includes submission overhead and is not a hardware-timestamp-only measurement. Compilation and input uploads are outside the interval.

| Storage | Live keys | Query rows | Current tile (ms) | Joint tile (ms) | Time reduction |
|---|---:|---:|---:|---:|---:|
| BF16 | 5,207 | 4 | 0.391070 | 0.356180 | 8.9% |
| BF16 | 5,207 | 8 | 0.656462 | 0.543939 | 17.1% |
| BF16 | 14,000 | 4 | 0.868470 | 0.717182 | 17.4% |
| BF16 | 14,000 | 7 | 1.651599 | 1.338173 | 19.0% |
| BF16 | 65,520 | 8 | 8.225427 | 6.510336 | 20.9% |
| FP8 | 14,000 | 8 | 2.205337 | 1.613495 | 26.8% |
| BF8 | 14,000 | 8 | 2.213812 | 1.610879 | 27.2% |
| FP16 | 5,207 | 7 | 0.612688 | 0.533405 | 12.9% |

Every output byte matched between variants in all eight cases. An independent FP64 reference also checked all output dimensions for the first and last query rows of heads 0, 5, 6, and 23. No perplexity run was performed; these transformations preserve the existing floating-point operation order.

## Resources

| Partial kernel | VGPRs | SGPRs | Modeled occupancy | Scratch spill bytes |
|---|---:|---:|---:|---:|
| BF16 four-row tile with LICM | 74 | 56 | 100% | 0 |
| BF16 two-head/four-row tile | 99 | 56 | 75% | 0 |
| FP8 two-head/four-row tile | 112 | 50 | 75% | 0 |

The joint tile uses 4,096 bytes of LDS. Occupancy is a compiler resource estimate, not a measured active-wave percentage. Greater K/V reuse outweighed the lower modeled occupancy in these measurements.

## LICM in isolation

- 28 existing LICM golden cases passed.
- Eight native masked FP16/BF16/FP8/BF8 attention checks passed.
- At 14,000 live keys and eight BF16 query rows, the existing tile measured 1.650544 ms with the released compiler and 1.653493 ms with LICM; output was bit-identical. This is not evidence of a speedup.
- The FP8 1,024-query/65,520-key prefill check measured 453.621417 ms versus the prior 457.441729 ms observation, with bit-identical output. This small difference is not claimed as a reliable performance gain.
- The standalone LICM pass hoisted one operation from the emitted joint-attention source. Masked invariant-load extraction remains separate future work.

## Regression coverage

- All nine short-attention dispatch/shape tests passed with the new grid and LDS requirements.
- The native joint test checks seven query rows, three batch slots, six query heads per KV head, reversed physical pages, invalid padded page-table entries, poisoned K/V tails, causal and sliding-window masks, and retained replay with all rows empty. It passed with two device dispatches and no host fallback.
- Ordinary single-token decode output remains bit-identical. Paired reruns of both variants ranged approximately 0.241–0.263 ms, with no consistent change.

## Limits

These are isolated attention results. End-to-end MTP/DFlash2 token rates have not been measured for this change. The two-head tile deliberately bounds register demand; it does not put all six heads and all eight token rows into one workgroup. No claim is made about explicit Infinity Cache residency or measured cache-hit counters.

The published v0.4.20 binaries predate this experiment.
