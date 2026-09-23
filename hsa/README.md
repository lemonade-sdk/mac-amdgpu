# macOS HSA runtime

A userspace HSA C ABI library connecting applications to MacAMDGPU through
IOKit. The intended path is LSE → HRX → this library → DriverKit → GPU.
The library executes inside the client process; the DriverKit extension owns
PCI access, firmware, DMA mappings and hardware queues.

This is an incomplete runtime, not an HSA-conformant implementation or a
working HRX backend. The initial implementation provides live discovery and
runtime lifecycle and CPU signal operations. It does not execute kernels. ABI version queries describe
the targeted HSA 1.2 interface, not conformance certification.

## Build and verify

```sh
bash scripts/test-hsa-runtime.sh
build/hsa/mac-hsa-info
```

The build uses Apple Clang, CMake, IOKit/CoreFoundation, and vendored core HSA
headers with their upstream license. No ROCm installation is needed for this
discovery library. The selected design is a focused, native HSA-compatible
interface for LSE’s pinned HRX backend, backed by DriverKit. ROCr is a source
reference; porting or shipping the full ROCr runtime is outside this approach. The C probe links the actual dylib and prints CPU/GPU names,
the responding driver build, cached bringup stage and VRAM sizes. Run it outside
a sandbox that denies IOKit user-client access. The installed DriverKit
extension must permit the client to connect.

The transport opens observer clients and uses only RuntimeBuild and QueryInfo.
It never initializes/resets the GPU, acquires PCI ownership or submits work.
No GPU attached is a valid CPU-only discovery result. An attached service that
cannot be opened or validated causes initialization to fail rather than silently
being hidden. Driver identity ABI 1 and build 172 or later are required.

Agent handles survive nested hsa_init/hsa_shut_down references and are never
reused within a process. Final shutdown closes observer clients; enumeration
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

No dispatch features or extensions are advertised. hsa_queue_create explicitly
rejects queue creation. This is intentional until hardware compute dispatch,
queue teardown and resource lifetime are implemented; there are no fake
completion signals or successful no-op dispatches.

## HRX integration work

LSE commit `b5637a7109d409c21f75586edb75e7631277bce8` pins HRX System to
`5927b0e0fafdefb5c8b41aa71bca8fd28791ad7c`. This revision requires 119 dynamic
HSA symbols; the current library supplies 48 of them, leaving 71 missing. Symbol presence
is not equivalent to full behavior: queue creation still rejects requests, and
signals currently require CPU-only consumers. It
creates hardware queues through `hsa_queue_create`, then casts those queues to
AMD's queue layout. Signals also have an AMD device-visible layout. The loader
requires the AMD loader extension, memory pools, signals and code objects.
The existing driver command/fence tests do not satisfy those contracts.

The separately reviewed HRX main revision
`437e789eaea207a036c197cf3398a6ca473d6534` requires 121 symbols (48 exported, 73 missing) and uses
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

This is the native transport foundation, not yet wired to HSA memory pools.
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
hardware retesting remains pending.

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
