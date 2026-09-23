# HSA behavior status

LSE's pinned HRX resolves **119/119 symbols**. This does **not** mean 119 fully
working GPU operations or a runtime ready for HRX inference. The seven
platform-unsupported APIs below deliberately return errors. Several other
families currently support host behavior only.

The 29 symbols added with build 181 have these behaviors:

| Family | Symbols | Implemented behavior and limits |
| --- | ---: | --- |
| Cache enumeration/info | 2 | Queries available macOS CPU data-cache sizes; GPU cache enumeration is empty until driver topology exposes it. |
| Host lock/unlock/lock-to-pool | 3 | Real `mlock`/`munlock`, native page sizes, overlapping-lock references, CPU agents only. GPU access fails; a host pointer is never passed off as a GPU address. |
| Virtual-memory reserve/free, handle create/release, map/unmap/access | 7 | Real host mappings, aligned reservations, physical aliases, access protection, pinned backing when requested and retained lifetimes. GPU VA aliases are unsupported. The system-wide GPU virtual-memory capability remains false. |
| GPU memory IPC create/attach/detach | 3 | Driver 181 uses a random 128-bit sharing token and per-client BO references. Full allocation, one explicitly selected GPU agent; CPU/peer-GPU mappings are rejected. Repeated imports share a process-local pointer with balanced detach references. Importing over an existing owned MC-address allocation is rejected until distinct process GPU VAs exist. |
| Signal IPC create/attach | 2 | CPU-only shared atomic signal backing and process-local waits. All core CPU signal operations use the shared value. Cross-process updates, exporter destruction, repeated attachment and abrupt peer death are tested. These signals are not GPU-visible. |
| Queue profiling/info/CU-mask/priority | 4 | Profiling updates the AMD queue ABI flag. The other three reject software queues, consistent with the local ROCr HostQueue contract; persistent hardware queues now return their owning agent. Physical doorbell IDs, CU affinity and priority changes remain unsupported; persistent dispatch still awaits hardware validation. |
| System-event registration | 1 | One handler per runtime session, reset on final shutdown; callback delivery occurs outside the runtime lock and preserves callback status. Delivery is tested with injected events; DriverKit hardware-fault notifications are not connected yet. |
| Interop map/unmap and portable DMA-BUF export/close | 4 | Unsupported platform paths. No Linux DMA-BUF namespace is available through this transport. Failures preserve export outputs and never close unrelated caller descriptors. |
| SVM attribute get/set and prefetch | 3 | Unsupported until GPU fault servicing and shared virtual-memory migration exist. Calls fail without changing attributes or falsely decrementing completion signals. SVM capability queries return false. |

The existing executable loader accepts a bounded subset of linked gfx1201 ELF
images. AMD loader extension 1.03 now provides all seven table entries:
address translation, segment/executable/object enumeration, object metadata,
and embedded-file readers. Original storage and relocated descriptor copies
remain alive until executable destruction, even after reader destruction.
General global linking and production hardware queue support remain incomplete. Build 182 passed equal-address CPU/GPU host-buffer transfers, but
concurrent CPU/GPU atomic updates failed. Build 183 adds explicit coarse shared
allocations and native synchronous dispatch of frozen HSA-loaded kernels.
Both VRAM and direct shared-memory shader tests passed on build 183. These are
separate native APIs, not HSA fine-grained pools, signals or AQL queues.
Build 184 adds a bounded native AQL dispatch path with MES queue removal;
both VRAM and shared-memory hardware tests passed with verified queue removal. Public
HSA queue creation is implemented for driver 185 with seven device-wide slots,
64–4096 packets per queue, per-client ownership and verified MES removal before
storage release. Hardware validation of persistent queues is pending installation.
Agent dispatch capability remains disabled until it passes.

GPU-visible AMD signal storage now uses a shared per-device arena. CPU HSA signal
updates execute a small system-scope GPU atomic kernel; CPU loads observe that
same value. Stores, add/subtract, bitwise operations, exchange, compare-and-swap,
signed wraparound and waits passed on driver 184. This avoids mixing native CPU
RMWs with GPU RMWs, which lost updates on this PCIe path. GPU signal creation is
limited to one GPU consumer domain and 256 live signals per connection. Signal
IPC remains CPU-only. Failure of the GPU atomic executor wakes all affected
waiters and rejects further signals from that executor. Queue-to-queue completion,
barriers and concurrent shader/CP/CPU-HSA updates are not yet hardware-verified.
The [hardware procedure](../docs/HSA_QUEUE_VALIDATION.md) tests these separately.
No HRX/LSE inference has run.

## Validation

- Driver 184 executed two AQL dispatches in each of VRAM and shared host-memory
  modes: 128 and 256 correct results, all 16 KiB verified, GPU completion 1 → 0
  and firmware-acknowledged queue removal after every call.

- Driver 183 executed HSA-loaded `vector_add.kd` in VRAM and shared host memory.
  Each mode verified 128 and 256 results, all 16 KiB of input/output/guard bytes,
  increasing GPU fences and successful cleanup. This is native synchronous
  dispatch, not hardware HSA AQL or HRX inference.
- The loader extension passed on the actual GPU with driver 181: a linked
  kernel uploaded/froze successfully and its translated descriptor matched
  kernarg metadata after reader destruction.
- Driver 181 passed 16 KiB host → VRAM → host, zero mismatches, with DMA
  unmapping complete. This validates the existing PerformOperation path.
- Build 182 adds direct CPU-mapping checks before allowing GTT allocation,
  an immutable session GART window, fixed CPU mappings that fail on address
  collisions, and GTT/VRAM SDMA copies. `mac-hsa-shared-test --run` verifies
  64,003 unaligned bytes each way plus the complete 128 KiB shared allocation
  and VRAM guards. This passed on hardware. GPU-only decrement and carry also
  passed, but concurrent CPU/GPU additions lost updates and timed out. Cached
  PCIe capabilities lack host AtomicOp completion and Thunderbolt routing.
  No coherent GPU-signal capability was enabled.

- Nine ASan/UBSan runtime suites, including native executable dispatch,
  shared allocation lifetime, host virtual memory and
  separate-process IPC signal updates. A SIGKILL test verifies cleanup by a
  surviving attachment.
- Production driver export/import/free and client-close code is extracted into
  the shared-buffer regression test. It checks final-reference release, stale
  tokens, generation/size validation and exhausted tables.
- Actual driver 181 hardware test: all 16,384 bytes matched before and after
  the exporting process exited without HSA cleanup. The importer then wrote
  and read back a new pattern. Final detach/shutdown returned the GPU to stage 0.

```sh
bash scripts/test-hsa-runtime.sh
bash scripts/test-shared-buffers.sh
build/hsa/mac-hsa-ipc-test --run
python3 hsa/tools/audit_hrx.py --hrx upstream/hrx-lse-pin \
  --library build/hsa/libhsa-runtime64.dylib --require-runtime-ready
```

The last command intentionally fails while runtime requirements remain unmet,
even though all dynamic symbols resolve. The host virtual-memory tests need
permission to create macOS shared-memory objects outside a workspace sandbox.

IPC signals use mode-0600 files in the user's macOS temporary directory and
kernel-held file locks. A surviving attachment removes the name after the final
release, including after a peer dies. If every participant dies without cleanup,
an orphan temporary file can remain until a stale attach rejects/removes it or
OS/user temporary-file cleanup removes it; no GPU
storage or pinned pages are retained by that file. This local IPC format is not
binary-compatible with Linux ROCr sharing tokens.
