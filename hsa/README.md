# macOS HSA runtime

A userspace HSA C ABI library connecting applications to MacAMDGPU through
IOKit. The intended path is LSE → HRX → this library → DriverKit → GPU.
The library executes inside the client process; the DriverKit extension owns
PCI access, firmware, DMA mappings and hardware queues.

This is an incomplete runtime, not an HSA-conformant implementation or a
working HRX backend. The initial implementation provides live discovery and
runtime lifecycle. It does not execute kernels. ABI version queries describe
the targeted HSA 1.2 interface, not conformance certification.

## Build and verify

```sh
bash scripts/test-hsa-runtime.sh
build/hsa/mac-hsa-info
```

The build uses Apple Clang, CMake, IOKit/CoreFoundation, and vendored core HSA
headers with their upstream license. No ROCm installation is needed for this
discovery library. The C probe links the actual dylib and prints CPU/GPU names,
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
- mac_hsa_agent_get_driver_info, a separate diagnostic ABI exposing the live
  DriverKit snapshot. Bringup stage is historical and is not a readiness claim.

No dispatch features or extensions are advertised. hsa_queue_create explicitly
rejects queue creation. This is intentional until hardware compute dispatch,
queue teardown and resource lifetime are implemented; there are no fake
completion signals or successful no-op dispatches.

## HRX integration work

The reviewed HRX System source is commit
`437e789eaea207a036c197cf3398a6ca473d6534`. Its dynamic loader resolves a broad
HSA symbol table and requests the AMD loader extension. It currently creates
compute queues through hsa_amd_queue_create, allocates HSA memory pools and
uses HSA signals and code objects. The existing driver command/fence tests do
not satisfy those contracts.

Track the missing symbol surface with:

```sh
python3 hsa/tools/audit_hrx.py --hrx upstream/hrx-system \
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
No shader execution, HRX workload or model inference has passed yet.
