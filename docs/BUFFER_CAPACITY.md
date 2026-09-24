# Buffer capacity and reclamation

Build 194 raises the per-user-client buffer-object handle limit from 64 to 4096.
This accommodates independently retained executable images alongside inference
weight and transient pools. It is a handle/metadata bound, not a promise that
4096 buffers of any size or domain fit in GPU memory.

Build 195 replaces build 194's whole-table allocation with stable pages of
64 entries. A fixed 64-pointer directory costs 512 bytes; each allocated page
costs 11776 bytes (11.5 KiB) on arm64, because each entry is 184 bytes. Zero-buffer
clients and observers allocate no pages. Metadata grows only when all existing
pages lack a free slot, up to 64 pages/4096 entries (736 KiB plus the directory).
Finding a slot scans at most 4096 entries; generation/index lookup remains O(1).

The final successfully freed buffer in a page causes that page to be reclaimed
immediately. No warm page is retained. Other pages and live entry addresses never
move. Ordinary allocation failure and failed import-reference retention reclaim
an otherwise-empty new page too. Page growth failure returns NoMemory without
publishing a handle or changing live entries. Invalid imports do not allocate
metadata or retain a shared reference. Full capacity or exhausted generation
returns NoResources.

Handles keep the existing `(generation << 32) | index` ABI. The monotonically
increasing per-client generation never wraps; reaching UINT32_MAX prevents new
allocations/imports. Freeing a slot, releasing/recreating a page or importing
another reference cannot make a stale handle valid again. A failed ordinary
allocation releases its slot. A partially published GTT mapping retains its
slot and backing until the existing reset/isolation cleanup allows reclamation.

Remaining pages are destroyed by the existing resource-release path only after
their entries can be retired safely. Normal retirement first unbinds live GTT
mappings and closes owned queues; uncertain DMA/reset failures quarantine the
entire client state, including every page containing retained backing. Export/import reference counting
continues to free VRAM only when the final reference is released.

## VRAM allocator metadata

Each visible/GPU-only allocator now permits at most 8191 live allocations and
has 8192 free-range nodes. Coalesced free ranges over a contiguous pool are at
most the number of live allocations plus one. That bound reserves enough
metadata to return every valid allocation, including nonadjacent frees after
heavy fragmentation. New allocations fail before crossing the live-allocation
bound. The former 256-node pool could silently discard an isolated freed range
when metadata was exhausted; raising only the BO table limit would expose that
leak much more often.

The metadata is 196664 bytes per allocator on arm64. It consumes driver memory,
not reserved VRAM. Releasing the bringup context reinitializes it in place after
resource cleanup, avoiding the roughly 402 KiB stack temporary that ordinary
aggregate assignment would create with the larger allocator arrays.

Other independent bounds remain: the global GART reservation table has 128
records, available VRAM and alignment still limit allocations, and the shared
export registry has its own capacity. The 4096 BO limit does not raise these
limits. HRX/LSE pooling reduces tensor BO count; cached executable BOs remain
individually retained until their runtime releases them. The full-model peak
handle count still requires measurement.

## Offline verification

`test-shared-buffers.sh` runs the production allocation/import/free/lookup and
bulk-release bodies under ASan/UBSan. Build 195 also checks one-page-at-a-time
growth, complete middle-page reclamation, stable neighboring entry addresses,
page recreation with stale-handle rejection, growth failure and import-reference
overflow rollback. It fills all 4096 slots, verifies exhaustion
without reference leaks, frees/reuses a middle slot, rejects stale/out-of-range
handles, preserves generation across page recreation, rejects generation wrap,
injects metadata allocation failure, and checks ordinary versus partially
published GTT allocation failures. Existing shutdown tests cover successful
retirement, failed reset retention and quarantine cleanup.

`test-vram-accounting.sh` fills the 8191 allocation bound, rejects one extra
allocation without state changes, frees 4096 nonadjacent allocations, checks
exact accounting, reuses a freed range and recovers the entire contiguous pool.
It also exercises alignment splits. These tests do not touch GPU hardware.

## Build 195 hardware verification

The installed build 195 passed `mac-hsa-memory-test --capacity` in three rounds.
Each round held 1024 live device buffers and verified all 16384 bytes of every
buffer. The run freed and reallocated 512 alternating buffers plus 128 consecutive
buffers, then exited with exact data and clean teardown. This exercises buffer
counts above the former 64 limit, fragmented VRAM reuse and empty metadata-page
reclamation/recreation. Evidence is retained locally in
`build/tests/driver195-hardware/buffer-capacity-1024.log`.

The same installed driver passed six actual LSE/Loom/HRX convolution cases:
680 exact GPU outputs across zero-padding and retained-history variants, all
input buffers/guards unchanged, and zero host/fallback groups. The local log is
`build/tests/driver195-hardware/lse-convolution.log`. These focused checks do not
establish a full-model inference result or independently validate all 4096 live
handles on hardware; full 4096 exhaustion remains covered by offline tests.
