# Loom symbolic memo reset cost

Measured on 2026-09-28 for the pinned macOS HRX compiler. The second 5610-token
request retained an 8192-token physical KV pool. Its small prefill shapes needed
six new Flash code objects; the source/cache identity correctly changed with
the pool and table extents.

The loaded compiler clears its full symbolic memo table whenever branch facts
change. Each entry includes 17 condition-refined fact payloads. The backport in
`patches/hrx/symbolic-memo-touched-reset.patch` follows newer upstream Loom's
practice of tracking populated entries. Reset clears only their expression and
fact validity tags. It keeps the resets before and after each conditional proof;
allocation errors propagate before a memo result becomes valid.

## Matched cold compilation

Each arm compiled the identical saved source in a fresh process, with other
compilers idle. Every resulting code object was byte-identical across arms.
These are one matched pair per source, rather than a repeated timing study.

| Query rows | Original seconds | Backport seconds | Code bytes |
|---:|---:|---:|---:|
| M2 | 13.433 | 7.715 | 140,280 |
| M8 | 13.573 | 7.830 | 144,376 |
| M32 | 22.332 | 13.905 | 181,240 |
| M64 | 22.459 | 14.038 | 181,240 |
| M128 | 22.608 | 13.971 | 181,240 |
| M256 | 22.661 | 14.083 | 181,240 |
| **Total** | **117.065** | **71.542** | |

The six-source total fell **38.89%** (1.636×). Fresh memo
allocation and other proof work remain; this change does not eliminate cold JIT
cost.

## Verification and provenance

- Loaded build: `build/hrx-macos-adapter/CMakeCache.txt` points to
  `build/hrx-macos-source`, HRX pin
  `5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c`.
- Backport reference: upstream Loom at
  `437e789eaea207a036c197cf3398a6ca473d6534` uses touched-entry reset. Its broader
  value-domain/proof refactor was not copied into the pin.
- Existing symbolic-expression suite: **30/30** passed in both versions.
  Three added cases passed: sparse/reset reuse, fact-only true/false/restored
  scope isolation, and failed tracking allocation without memo publication.
- Candidate rebuilt all **77** objects depending on the changed context header
  and relinked **17** private archive copies, preserving matching internal ABI.
- Original library SHA256:
  `ebbb7cc3da1db6b7204b003b41afc6f01c5a355ff945553f34c89e2f41ee7a15`.
- Private candidate library SHA256:
  `1e39bf934d7c9437a3f79d1f7bfbcd12fe5c985ddd60c9f00fd9f337ab46e2e5`.
- Saved sources, code objects, test logs, timing JSON, build commands and hashes:
  `build/pp-optimization/loom-symbolic-memo-20260928/`.

`scripts/build-hrx-macos.sh` applies the transport adapter and the memo backport
individually, accepting an exact already-applied patch on incremental builds.
