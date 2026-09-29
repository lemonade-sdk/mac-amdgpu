# Active gfx1201 ISA audit — 2026-09-28

## Scope and provenance

The DFlash BF16 two-turn profile contains **357 distinct active entries**, 137,867 timed dispatches and **14,237.4294 ms** summed GPU execution. The profile includes startup, prefill and decode; its totals are not per-token timings. Its server SHA is `635ccc66422d2fb769f8f98730c6d52c37e5d6414136775f68f10870d62b50b1`.

Every entry was matched to its exact captured Loom source and verified against `source-corpus.json`. All 357 sources compiled and disassembled offline for the initial inventory. The subsequent compiler qualification below includes a native correctness suite; no performance benchmark has been completed for the compiler correction. The original private code-object cache had been removed, so these are fresh compilations of the captured sources, not preserved objects from the timed run.

Compiler: canonical `LoomcCompiler`, prepared-low/CFG pipeline, current accepted source `b0b93ad`. Loaded `libloomc.0.1.0.dylib` SHA256: `2790b84de9e02834d7297a08425e7136c55eb8cffaa6d5558e8629722a13ffbf`. Summed compiler time was 2.672 s. LLVM disassembly used `--mcpu=gfx1201`.

Persistent evidence is under `build/release/pi-performance/dflash-bf16-wave-profile/isa-audit/`: `manifest.json`, `all-active.csv`, `isa-metrics.json`, candidate records and the 2.18 MB native-object/disassembly archive. Entry mappings include the actual profile shapes and GPU durations. The script counts native instructions, load widths, matrix/dot operations, barriers, backward branches and conservative pairing candidates.

## Baseline compiler inventory

| Native fact | Result |
|---|---:|
| Verified source / compiled / disassembled entries | 357 / 357 / 357 |
| Wave32 | 357 |
| WGP-mode descriptors | 357 |
| Queried private bytes per lane | 0 for all 357 |
| Scratch load/store instructions | 0 |
| Allocator vector/scalar spill counts | Unreported for all 357 |
| Highest VGPR allocation | 186 registers per lane |
| Highest static LDS allocation | 56,064 bytes per workgroup |
| Static native instructions | 157,582 |
| VOPD instructions, each encoding two component operations | 2,459 across 157 entries |
| WMMA instructions | 656 across 48 entries |
| Integer DOT4 instructions | 624 across 30 entries |
| DOT2 / `v_pk_*` instructions | 0 / 0 |
| Barrier signal / wait instructions | 316 / 316 |

Workgroup sizes are 32, 64, 128, 256, 512 or 1024 threads; **wave32 means 32 lanes per wave**, independently of workgroup size. Neither register/LDS allocations nor WGP mode establish actual occupancy. Zero private bytes and no scratch accesses provide no evidence of scratch traffic in these objects; unreported spill counts remain unknown.

These are static counts. Backward branches and execution masks prevent treating them as dynamic operation or byte totals. WMMA is already present in the expensive contractions and prefill attention, so VOPD percentage alone is not a useful utilization score. Q4 matrix instructions accumulate integers, then restore scales and accumulate in FP32; BF16 attention matrix instructions accumulate in FP32.

## Concrete opportunities

### 1. Q4 prefill step/sum LDS layout

The dominant up entry `lse_loom_8341997943815054630` (M512/N17408/K5120) consumed **3,557.274 ms** in the capture; down `lse_loom_5046229324040367664` consumed **1,570.697 ms**. Both have 110 VGPRs, 6,656 B LDS, zero private bytes, 16 WMMA instructions, 43 VOPD instructions and three barrier pairs.

Their bodies retain **72 scalar 4-byte LDS loads plus 16 8-byte loads**, and 63 `s_wait_dscnt` instructions. Step/sum reads are separate: up addresses `0x1e0c`/`0x1e28` load offsets 6144/6148; `0x1e44`/`0x1e4c` load offsets 6400/6404. No paired-address LDS load is emitted. Interleaving these FP32 terms is a focused way to ask for paired loads; it must preserve arithmetic order and demonstrate a native win. This audit does not establish that the LDS reads are the dominant stall.

### 2. gfx12 same-source VOPD legality

The baseline compiler's `loom/src/loom/target/arch/amdgpu/planning/vopd_plan.c` accepts source-bank compatibility only when `register % 4` differs. It rejects **identical same-size VGPR sources**, which are legal on gfx12. [AMD RDNA4 ISA, §7.8](https://docs.amd.com/api/khub/documents/uQpkEvk3pv~kfAb2x~j4uw/content), [LLVM's gfx12 `AllowSameVGPR` checks](https://www.llvm.org/docs/doxygen/GCNVOPDUtils_8cpp_source.html).

An actual M4 down body, `lse_loom_5881251037008486095`, leaves these independent FP32 accumulations separate:

```asm
v_fma_f32 v3, v7, v8, v3
v_fma_f32 v4, v7, v9, v4
```

The equivalent `v_dual_fmac_f32` pair assembles for gfx1201. The conservative scan found **1,886 nonoverlapping encoding-valid candidate pairs, including 56 arithmetic pairs**; most others are register copies. Every proposed pair assembled with LLVM MC. The scan excludes literals, scalar sources, modifiers and DPP; requires opposite destination parity, bank compatibility and no cross-dependency; and permits ALU-delay recalculation. It is an opportunity list, not a replacement scheduler or a measured speedup. The narrow correction below fixes this legality restriction; it does not implement every pairing proposed by the scan.

### Qualified narrow compiler correction

`patches/hrx/gfx12-vopd-identical-source.patch` adds a generated RDNA4 capability and permits an identical source only when both component readers admit the same single 32-bit physical VGPR. This includes packed DOT2 sources. Distinct same-bank sources, opposite-destination-parity requirements, dependencies, modifiers and unsupported wider operands retain their existing checks. gfx11 retains its prior restriction.

The patch is based on HRX `5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c`. It applies cleanly to that revision plus `symbolic-memo-touched-reset.patch`, without requiring the macOS transport patch. Its SHA256 is `14f7ca4af800e5fdc00105b0587a6cb10b7d3287587d11cef00f87dd89e74b8e`.

- Eight authored planner cases passed: same-source FMA, ordered SUB and packed DOT2 positives; distinct same-bank, destination parity, forward dependency and reverse alias negatives; and gfx11 same-source rejection. The existing gfx12 source-orientation case also passed with its updated direct-pair expectation. All 28 target-table tests passed.
- All **357 exact captured sources** compiled and disassembled with the private candidate. Static VOPD instructions increased **2,459 → 2,494** across **20 entries**. Register/LDS/private resource facts were unchanged; all reported private bytes remain zero. The previously illustrated M4 down entry remains unchanged, so the correction does not prove that its hand-identified FMA pair is formed.
- The combined native runtime correctness suite passed **80/80** with compiler SHA256 `3480ea13db1af8a7cdd05c0fffd3d5440eec6e85913d9832e3e14e589f693269`. The loader trace confirms that candidate library and HSA SHA256 `b7f8216e32fa6ab0e5b2fce87c518ce67c3fffa4c628cceece983c53e3cea90d`. Host references were enabled for fixtures that compare device and host results; this is not a claim that every fixture forbids host execution.

Native proof: `build/release/pi-performance/vopd-runtime/native-host-reference.log` and `identity-host-reference.json`. The candidate used an empty private cache per process. Corpus objects, disassembly and comparisons are under `/private/tmp/loom-vopd/corpus/`. Performance remains **unmeasured**; additional legal static pairs do not establish a throughput gain.

### 3. Register-pressure outliers are narrowly scoped

The 186-VGPR entries are prefill `gdn_rate_q4` tails `lse_loom_1533985215442264651` and `lse_loom_15903415484645545292`, totaling **29.989 ms**. The 56,064-B LDS outlier `lse_loom_5817587412685894498` totals **21.613 ms**. They merit resource-aware attention if their workload grows, but together account for only about 0.36% of this capture. The heavy M512 Q4 entries are the higher-priority experiment.

No critical native resource defect was found in the inspected corpus. Hardware stall, cache-hit and achieved-occupancy counters were not collected; a count of ALU delays or waits is not their measured cost. RDNA4 L1 does not perform actual caching, so an L1 tuning proposal would be misplaced. [RGP event-window documentation](https://gpuopen.com/manuals/rgp_manual/events_windows/).

## Linked SwiGLU assessment before cleanup

This assessment describes commit `b0b93ad7c3dc47be16872203db5bb6dac1a61e9d`, before the requested inactive-code cleanup.

The [hidden loop and its sole barrier](https://github.com/Geramy/LSE/blob/b0b93ad7c3dc47be16872203db5bb6dac1a61e9d/src/kernels/linked.cpp#L180-L188) are separate: the barrier follows the completed loop. [emit_dot](https://github.com/Geramy/LSE/blob/b0b93ad7c3dc47be16872203db5bb6dac1a61e9d/include/lse/kernels/vec_mem.hpp#L52-L79) contains no barrier. N2176 assigns nine outputs to 128 lanes and eight to the others; all then reach the barrier. The reviewed source therefore does **not** substantiate the proposed divergent-barrier diagnosis.

The hidden tensor was a global [env::InOut binding](https://github.com/Geramy/LSE/blob/b0b93ad7c3dc47be16872203db5bb6dac1a61e9d/src/kernels/linked.cpp#L151-L158), despite the opening LDS comment; the input was global too. Gate/up used two complete dot loops, so an interleaved loop could reuse an input load and expose independent FP32 accumulations. However, [linked_kernel_for](https://github.com/Geramy/LSE/blob/b0b93ad7c3dc47be16872203db5bb6dac1a61e9d/src/kernels/linked.cpp#L421-L432) declined SwiGLU/RMS, and [is_linear_node](https://github.com/Geramy/LSE/blob/b0b93ad7c3dc47be16872203db5bb6dac1a61e9d/src/kernels/linked.cpp#L27-L33) excluded quantized contractions. This body was not the active Q4 M512 or activation-panel route. Staging full target input plus hidden would require `4*(5120+17408)=90,112 B`, exceeding 64 KiB per workgroup. No enabling or performance claim follows from this proposal.

The inactive SwiGLU/RMS bodies, matchers and bindings have now been removed for the v0.4.12 working tree; `linked.cpp` retains only GDN forwarding. Five focused host suites passed after removal, covering GDN pairing, scheduling, pointwise fusion, graph behavior and scheduler epilogues; the GDN kernel body was unchanged.
