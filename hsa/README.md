# macOS HSA runtime

A userspace HSA C ABI library connecting applications to MacAMDGPU through
IOKit. The intended path is LSE → HRX → this library → DriverKit → GPU.
The library executes inside the client process; the DriverKit extension owns
PCI access, firmware, DMA mappings and hardware queues.

This is an incomplete runtime, not an HSA-conformant implementation or a
working HRX backend. It provides live discovery, runtime lifecycle, CPU signals, software queues,
and CPU/GPU memory allocation and copies. It does not execute HSA kernels. ABI version queries describe
the targeted HSA 1.2 interface, not conformance certification.

## Build and verify

```sh
bash scripts/test-hsa-runtime.sh
build/hsa/mac-hsa-info
```

The build uses Apple Clang, CMake, IOKit/CoreFoundation, and vendored core HSA
headers with their upstream license. No ROCm installation is needed for this
library. The selected design is a focused, native HSA-compatible
interface for LSE’s pinned HRX backend, backed by DriverKit. ROCr is a source
reference; porting or shipping the full ROCr runtime is outside this approach. The C probe links the actual dylib and prints CPU/GPU names,
the responding driver build, cached bringup stage and VRAM sizes. Run it outside
a sandbox that denies IOKit user-client access. The installed DriverKit
extension must permit the client to connect.

Discovery and agent queries use temporary observer clients and only
RuntimeBuild/QueryInfo. GPU pool capacity queries and first allocation lazily
acquire a session, initialize firmware, and retain that connection until final
shutdown. Device copies use the synchronous DriverKit BO/SDMA APIs.
No GPU attached is a valid CPU-only discovery result. An attached service that
cannot be opened or validated causes initialization to fail rather than silently
being hidden. Driver identity ABI 1 and build 172 or later are required.

Agent handles survive nested hsa_init/hsa_shut_down references and are never
reused within a process. Final shutdown cancels/joins copy workers and closes the device session; enumeration
callbacks run outside the runtime lock. Live GPU info queries propagate
transport failures rather than returning stale cached success. Reinitializing
after final shutdown rebuilds the device list.

## Implemented surface

- hsa_init, hsa_shut_down, hsa_iterate_agents.
- hsa_agent_get_info for names, vendor, device type, feature flags, maximum
  queue count, machine model, profile, ABI version, extension mask and unknown
  cache-size reporting.
- hsa_system_get_info for ABI version, monotonic nanosecond timestamp/frequency,
  endianness, machine model and extension mask.
- hsa_status_string and extension-support queries.
- Core signal creation/destruction, loads/stores (including silent stores),
  arithmetic/bitwise atomics, exchange/CAS and condition waits in all required
  ordering variants. AMD signal creation and wait-any/wait-all are also present.
  Signals currently support explicit CPU consumers; GPU consumers, IPC signals
  and unrestricted consumers while a GPU is present return an allocation error
  until coherent GPU-visible backing is implemented. No GPU completion is faked.
- mac_hsa_agent_get_driver_info, a separate diagnostic ABI exposing the live
  DriverKit snapshot. Bringup stage is historical and is not a readiness claim.

CPU pools are fine-grained for CPU access only; device pools are coarse-grained
VRAM with no CPU mapping. Access queries do not claim CPU/GPU coherent memory.
Software queues allocate real AMD-layout queue metadata and AQL packet storage,
but the application is responsible for consuming them. Hardware kernel queues
remain separate.

No dispatch features or extensions are advertised. hsa_queue_create explicitly
rejects queue creation. This is intentional until hardware compute dispatch,
queue teardown and resource lifetime are implemented; there are no fake
completion signals or successful no-op dispatches.

## HRX integration work

LSE commit `b5637a7109d409c21f75586edb75e7631277bce8` pins HRX System to
`5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c`. This revision requires 119 dynamic
HSA symbols; the current library supplies 90 of them, leaving 29 missing. Symbol presence
is not equivalent to full behavior: queue creation still rejects requests, and
signals currently require CPU-only consumers. It
creates hardware queues through `hsa_queue_create`, then casts those queues to
AMD's queue layout. Signals also have an AMD device-visible layout. The loader
requires the AMD loader extension, memory pools, signals and code objects.
The existing driver command/fence tests do not satisfy those contracts.

The separately reviewed HRX main revision
`437e789eaea207a036c197cf3398a6ca473d6534` requires 121 symbols (90 exported, 31 missing) and uses
`hsa_amd_queue_create`. Keep these baselines distinct; LSE's pinned revision
and its patches are the initial integration target.

The ROCr runtime reference is `ROCm/rocm-systems`, commit
`820ea79c1848e1291204e7f7e56ec68bd049704e`, under `projects/rocr-runtime`.
Its `amd_aql_queue.cpp`, signal implementations, executable loader and driver
interface complement Linux AMDGPU/KFD's hardware queue, memory and teardown
contracts. Linux KFD's GFX12 MQD setup uses AQL packet slots and queue-specific
pointer units; the existing PM4 GFX queue cannot be advertised as an AQL queue.
Host/GPU pointer identity, coherent atomic visibility, shared queue metadata,
completion signals and teardown must be implemented and tested together.

Track the missing symbol surface with:

```sh
python3 hsa/tools/audit_hrx.py --hrx upstream/hrx-lse-pin \
  --library build/hsa/libhsa-runtime64.dylib
```

This audit returns nonzero while symbols are missing. Passing it will be
necessary but insufficient: each function and extension must work correctly.
The required milestones are:

1. General buffer allocation, copies and retirement through the DriverKit ABI,
   including separate CPU/GPU addresses and memory visibility rules.
2. Data-verified compute shader dispatch with the correct GFX12 register,
   executable, kernarg, scratch/LDS and queue setup.
3. HSA signals, memory pools, AQL queues, barriers and completion semantics.
   GPU-visible atomic coherence cannot be inferred from the SDMA copy test.
4. Code-object loading, relocation, executable metadata and AMD loader tables.
5. HRX loading and device creation, a vector kernel with exact readback, then
   LSE inference with reference output comparisons.

Do not report kernel-dispatch support merely to pass HRX device enumeration.
Driver build 176 passed two fixed wave32 load/add/store shader tests with
complete result/guard verification and storage reuse. General HSA dispatch,
an HRX workload and model inference have not passed yet.


## Driver buffer ABI (build 177)

This transport now backs the HSA device memory pool.
Calls require the existing owning driver connection; observers cannot acquire
that initialized session simply by requesting a copy.

| Selector | Inputs | Result / constraints |
| --- | --- | --- |
| 16 BOAlloc | size, domain, alignment, flags=0 | handle, GPU address, CPU address=0 for VRAM |
| 17 BOFree | handle | Returns storage only when no submission is pending |
| 18 BOGetInfo | handle | GPU address, offset, size, alignment, domain |
| 48 BOCopy | source handle, source offset, destination handle, destination offset, bytes | One scalar operation status; at most 4 MiB and 100 ms; overlap rejected |
| 49 BOWrite | handle, offset, bytes plus input structure | Verified upload into domain-1 staging; 4-byte alignment, at most 4096 bytes |
| 50 BORead | handle, offset, bytes | Output structure from domain-1 staging; same alignment/size limit |

Domain 1 uses CPU-visible VRAM above the fixed firmware reservation. Domain 3
uses a separate GPU-only range above BAR0, excluding the final MiB of reported
usable VRAM. Domain 3 cannot be mapped or accessed by BOWrite/BORead. To upload
larger device buffers, write a visible staging BO and copy its contents to a
subrange of the device BO; reverse the sequence to download. Keep staging alive
until completion. A published transfer failure retains allocations and blocks
normal mutation until Stop/reset. Calls are serialized and synchronous; this is
not yet an asynchronous HSA copy implementation. Build 177 passed 4 KiB
round-trips at offsets 0, 1 GiB and 22 GiB minus 4 KiB in one 22 GiB allocation,
followed by successful buffer release and the fixed compute test. This does
not verify every byte, arbitrary copy lengths, throughput or HSA semantics.


## Native compute dispatch (build 178)

Selector 51 accepts the 264-byte version-1 `ComputeDispatchRequest` in
`dext/amdgpu/amdgpu_dispatch_abi.h`, no input scalars, and three output scalars:
operation status, completed GPU fence, and stage (0 preflight, 1 upload,
2 submit/wait, 3 complete). A transport success does not imply GPU success.
The code handle/range and every nonzero referenced buffer handle must belong
to the owning connection and use a VRAM domain. Code entries are 256-byte
aligned. The request supplies group counts, local dimensions (at most 1024
threads total), resource registers and up to 16 user SGPRs. Timeout is 1 through
1,000,000 microseconds. Unused SGPR words and reserved flags must be zero.

This initial transport supports gfx1201 wave32/CU-mode launches, workgroup IDs
and up to 64 KiB LDS. Scratch, dynamic VGPR allocation, traps, wave64 and
privileged modes are rejected. The caller must supply correct machine code,
register allocation and argument layout. VMID0 and caller-supplied instructions
make this a trusted single-owner interface, not GPU process isolation. Buffer
handles validate declared resources; they do not constrain addresses embedded
in instructions or arguments.

Calls execute on the serial lifecycle queue and return only after completion
or bounded failure. All owner BOs remain alive during the call. A failed
staged/published submission retains the command IB and blocks mutation until
Stop/reset, including failures that precede the doorbell. Successful calls free
the IB. The HSA runtime does not yet call this selector or advertise dispatch.
The host Dispatch Test uploads a separate kernarg-loading shader and checks
four/eight workgroups, changing arguments and all data/guard words. Build 178 completed the first
fence but failed its 128-word output check. Build 179 corrects the test kernel
to compiler-generated gfx1201 workgroup IDs and dependency instructions;
hardware passed both launches on 2026-09-23 at 15:23 UTC: 128 and 256
outputs plus all input/guard words, fences 1 and 2, followed by buffer release.

## Signal implementation and verification

Signal storage follows the pinned AMD 64-byte layout and alignment, checked
against upstream declarations. Host atomic operations access the value at byte
8 using lock-free 64-bit atomics. Waiters retain the object without holding the
runtime mutex; blocked waits also poll to observe silent/direct atomic stores.
Final runtime shutdown wakes waiters and releases outstanding signal storage.
The CPU tests cover every operation variant, wraparound, concurrent increments,
release/acquire publication of ordinary data, all wait conditions, timeouts,
consumer validation, AMD attributes, multi-signal waits, and lifetime cleanup.
The test script checks dynamic exports from the actual dylib, since a function
compiled into a unit test can still be hidden from HRX's dynamic loader.

The vendored declarations now match LSE's exact pinned header revision
`cc2b5f429de4d1cb2be96ed10e6f45246e408d0e`. Only ABI headers are copied; the
runtime implementation remains native. These CPU tests do not establish
CPU/GPU atomic coherence, device-side signal access, AQL completion semantics
or an HRX workload. Those remain hardware/integration acceptance requirements.


## Memory and software queues

The memory family implements region/pool enumeration and attributes, aligned
CPU allocation, device-only VRAM allocation, ownership/access queries, pointer
metadata, fill, synchronous copy and dependency-gated asynchronous copy. GPU
addresses are opaque to the CPU: copies resolve allocation ownership and use
DriverKit staging rather than dereferencing those addresses. Unknown pointers
are treated as caller-owned host pointers. Known allocations have bounds checks
and remain alive during asynchronous operations. Distinct GPU allocations that
would collide in the process pointer namespace are rejected until separate GPU
virtual address spaces exist.

Each device session serializes transfers through a retained 16 KiB visible
staging BO. Transfers currently use at most 4 KiB per chunk; partial dwords
use read/modify/write to preserve neighbors. GPU-to-GPU copies currently pass
through CPU staging. This is correctness plumbing, not a throughput claim.
A failed submission faults the session and retains staging until driver teardown.
The CPU completion signal is decremented with release ordering only after a
successful copy; asynchronous failures set a negative value. It is not a
GPU-visible signal implementation. Final shutdown cancels pending dependencies,
joins workers outside the runtime lock, then retires allocations and the session.

Software queues implement create/destroy/inactivate and all required read/write
index load/store/add/CAS ordering variants. Packet headers start invalid and
queue IDs are never reused. The caller supplies and retains its doorbell signal.
These CPU queues do not advertise a GPU packet processor.

`initializeDevice` validates build 179 or newer, acquires PCI through GetIdentity,
and follows the tested R9700 firmware sequence. All firmware files are read
before reset. The default firmware directory is the installed host app's
`Contents/Resources/firmware`; `MAC_AMDGPU_FIRMWARE_DIR` overrides it. C0 and C8
firmware selection is explicit. Every stage and firmware RPC must acknowledge
success before the next is issued. Build 180 can join an already initialized
session after GetIdentity acquires a participant reference and live IP and
allocator queries succeed; this path performs no reset or firmware upload.
Build 179 still requires exclusive ownership.

After Stop GPU in the host app, run:

```sh
build/hsa/mac-hsa-memory-test --run
```

On 2026-09-23 this test acquired and initialized the actual GPU, reported a
33,939,259,392-byte device pool, and passed 12,003 unaligned payload bytes plus
4,381 guards through fill, upload, device copy, and asynchronous download.
After freeing buffers and final shutdown, a separate observer probe confirmed
bringup stage 0. The ownership-busy test also correctly refused access while
the host app still owned the GPU.

Five ASan/UBSan suites cover lifecycle, CPU signals, memory/software queues,
every initialization failure point, and the production IOKit transport with
mocked calls. They check temporary observer closure, single concurrent
initialization, firmware map/unmap, staged transfer guards and failure retention.
The export test checks the built dylib, not just unit-test linkage.


## Shared sessions and executable loading (build 180)

On 2026-09-23, two separate HSA processes shared the actual GPU. The first
held live buffers while the second allocated, copied, verified every byte,
and exited. The first then passed the same 12,003 payload and 4,381 guard-byte
checks. A separate run terminated the first process with SIGINT, bypassing HSA
cleanup; the second still passed. The final client exit returned the device to
stage 0. These runs validate idle-client departure, not recovery from a hung
in-flight kernel or process isolation for arbitrary trusted-VMID0 code.

To reproduce, run `build/hsa/mac-hsa-memory-test --hold` in one terminal, run
`--run` in another, then press Enter in the first. Build 180 needs no Host Stop
before attaching to an initialized shared session.

ISA enumeration reports gfx1201. Nine executable APIs provide copied code-object
readers, executable creation/destruction, agent loading, freeze, validation and
kernel symbol lookup/info. The loader accepts bounded ELF64 AMDHSA ET_DYN images
for gfx1201, checks load segments, MessagePack metadata and kernel descriptors,
then applies ABS64/RELATIVE64 relocations and uploads into owned GPU storage.
Unresolved imports, TLS, other relocation types and descriptor-changing
relocations are rejected. AMD loader extension tables and general global-symbol
linking remain unfinished. Symbol coverage does not establish HSA conformance.

`bash scripts/test-hsa-runtime.sh` builds a linked fixture using LLVM/LLD and
runs six ASan/UBSan suites plus parser truncation/mutation checks. The shader
fixture remains pinned to the available LLVM 21.1.8 compiler; an explicit
`AMDGPU_LLVM_BIN` overrides that selection.

```sh
build/hsa/mac-hsa-executable-test build/tests/hsa-code-object.hsaco vector_add.kd
```

This hardware test passed loading/freeze and resolved `vector_add.kd` at
`0x8010000580`, with 12-byte kernargs. It did not execute the kernel. Native
compute dispatch has a separate hardware result; HSA queue dispatch is not yet
connected to it.
