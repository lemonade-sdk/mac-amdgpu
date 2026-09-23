# Interpreting the native CPU/GPU atomic experiment

`mac-hsa-atomic-contention-test` measures the currently configured DMA mapping,
queue firmware policy and interconnect. It cannot establish that all possible
configurations of this GPU or ARM64 host support or lack mixed-agent atomics.

## Queue firmware policy and shader scope are distinct

The current `aql_build_mqd` writes `CP_HQD_HQ_STATUS0 = 0x00004000`:
bit 14 is set and bit 29 is clear. Driver 190 adds a read-only snapshot of the
owned queue's MQD backing image. This is not a live selected HQD register.
The snapshot also reads the owned buffer's actual GART PTE, DMA address,
GFXHUB context and endpoint Device Capabilities 2 / Device Control 2.

AMD's original GFX11 RS64 firmware change describes bit 29 as an acknowledgement
that PCIe atomics are supported. Setting it makes CP firmware use atomic
operations; leaving it clear lets firmware perform its own read/modify/write
fallback. GFX12's Linux KFD MQD initializer uses the same conditional bit based
on `amdgpu_amdkfd_have_atomics_support()`. This is evidence about CP firmware
operations, not proof that the bit globally disables shader atomics. The test
shader still contains native `global_atomic_add_u64` and
`global_atomic_cmpswap_b64` instructions with `SCOPE_SYS`.
See [AMD's firmware patch discussion](https://www.spinics.net/lists/amd-gfx/msg90787.html)
and local `upstream/linux/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c`.

PCIe Device Control 2 **AtomicOp Requester Enable** is another control. Linux's
`pci_enable_atomic_ops_to_root()` checks upstream routing, applicable upstream
egress blocking, and root completion capabilities before setting the endpoint
requester bit. AMD requests both 32-bit and 64-bit completion support. A GPU's
own completer capability does not establish support for GPU requests targeting
host RAM. See [Linux's PCI implementation](https://github.com/torvalds/linux/blob/master/drivers/pci/pci.c)
and local `amdgpu_device.c`.

`scripts/check-pcie-atomics.py` reports cached registry capabilities and controls,
preserving unavailable values as unknown. Cached results are not live readback
or proof of Thunderbolt/DART handling and CPU coherence. The default diagnostics do not change PCI configuration, MQD atomic policy or
memory cache policy. The separate driver 191 requester experiment below is
explicitly opt-in and does not qualify the upstream path.

## Mapping and result limits

The buffer uses DriverKit DMA backing, default CPU mapping cache policy, equal
CPU/GPU virtual addresses, and the driver's system/snooped GPU PTE policy with
gfx12 MTYPE=2 (UC). Driver 190 reads the actual PTE and compares it with the
expected encoding. Actual CPU cache/MAIR attributes remain unknown. The public allocation remains coarse-grained; this test
deliberately probes beyond that advertised contract.

AMD documents that gfx12 system-scope shader atomics may reach PCIe, and an
unsupported PCIe atomic may become a load/operate/store sequence that is atomic
only from GPU waves' perspective. Therefore GPU-only success is insufficient
to establish CPU/GPU interoperability. See [AMD hardware atomics documentation](https://rocm.docs.amd.com/en/docs-6.4.2/reference/gpu-atomics-operation.html).

The experiment first requires CPU-only and GPU-only add and CAS controls to pass
on the same mapped words. Mixed trials then require exact final counts, intact
guards, lock ownership/inverse consistency and observed progress overlap in
both directions. Timeout or failed completion is incomplete; correct counts
without overlap are inconclusive. A failed control stops further trials.

Defaults are 10 million adds and 1 million lock acquisitions per participating
processor, four single-agent controls, and three pairs of mixed trials. Each
phase has a 60-second host deadline and up to three seconds of GPU abort grace;
use a 700-second outer process deadline. GPU loops and retries are bounded.
Failed queue removal retains storage until reset. No successful test enables
general HSA fine-grain or PCIe atomic capabilities automatically.

## Measured result on driver 189

On 2026-09-23, the R9700 gfx1201 experiment used a 16 KiB DMA allocation at
equal CPU/GPU VA `0x110000000`, with the mapping and queue policy described
above. CPU-only controls returned exactly 10,000,000 additions and 1,000,000
CAS-protected increments. GPU-only controls returned the same exact values
with intact guards and completed dispatches.

The first mixed addition trial completed 10,000,000 operations on each processor
but returned **10,169,561**, rather than **20,000,000**. Both processors observed
overlapping progress (39,063 CPU observation batches and 84 GPU batches), all
guards remained intact, and GPU completion retired normally.

The first mixed CAS trial detected a CPU lock-ownership error after six completed
CPU acquisitions. The GPU completed 1,000,000 acquisitions; the protected value
was 1,000,007. That phase was incomplete, and the tool stopped before trials two
and three. It is not a completed two-million-operation result.

Queue removal and runtime shutdown succeeded; a read-only probe confirmed the
responding driver had returned to stage 0. These measurements demonstrate
failed native mixed-agent atomic interoperability under this configuration.
They do not identify actual PCIe transaction types or prove that every mapping
or platform configuration is incapable of interoperability. GPU-backed HSA
signal operations remain a separate, passing implementation.


## Driver 190 experiments awaiting installation

The new read-only query uses the installed SDK's
`FindPCICapability(kIOPCICapabilityIDPCIExpress, 0, &offset)` with a `uint64_t`
offset after the PCI provider has been opened. Endpoint completer bits do not
replace the upstream-path checks required before enabling AtomicOp requests.
No AtomicOp configuration bit is changed by this candidate.

The shader fixture verifies zero scratch/LDS and system-scope 64-bit atomic
instructions. `--return-add` forces the returning instruction and checks the
sum of returned old values in addition to the final counter. `--handoff` runs
32 serialized CPU/GPU rounds covering add, exchange and both successful and
failed CAS, including old-value and expected-value semantics.

Requested asymmetric cases (each also runs its single-agent controls):

```sh
build/hsa/mac-hsa-atomic-contention-test --run build/tests/hsa-atomic-contention.hsaco --add-only --cpu-iterations 10000000 --gpu-iterations 1000000 --trials 1 --timeout-seconds 120
build/hsa/mac-hsa-atomic-contention-test --run build/tests/hsa-atomic-contention.hsaco --add-only --cpu-iterations 1000000 --gpu-iterations 10000000 --trials 1 --timeout-seconds 120
```

Each expects 11,000,000. Add `--stagger` to run the first CPU quarter alone,
release the GPU and pace the middle CPU half from GPU progress, then wait for
GPU completion before the last CPU quarter. For CPU10M/GPU1M, checkpoints
are 2,500,000 before GPU release, 8,500,000 before the CPU tail and 11,000,000
at completion. Reversing counts gives 250,000, 10,750,000 and 11,000,000.
These are phase totals, not exact counts of physically overlapping RMWs.
Bidirectional progress observations are required; GPU progress must observe
CPU progress beyond the serialized prefix in the staggered case.

Logs include CPU start/end timestamps and host-observed GPU start/end markers
on one steady clock. Observation latency remains; they are not correlated GPU
hardware timestamps. Use a 400-second outer deadline per add-only invocation.
A completed mismatch may be investigated after confirming queue cleanup;
a timeout or incomplete dispatch requires recovery before more submissions.


## Measured result on driver 190

Live endpoint snapshot on 2026-09-23: PCIe capability offset `0x64`,
`DEVCAP2=0x0073099f`, `DEVCTL2=0x0000`. Requester Enable was off. The owned
buffer's actual GART PTE `0x8080000082058077` matched its expected encoding;
DART address was `0x82058000`, saved MQD HQ_STATUS0 was `0x4000`, and GFXHUB
PTBASE was `0x700001`. Actual CPU cache/MAIR attributes remain unknown.
No configuration bits changed during these tests.

All 32 serialized native CPU/GPU handoff rounds passed add, exchange and
successful/failed CAS checks. Single-agent addition controls passed in every
case below; mixed phases completed both requested workloads with intact guards
and observed progress overlap, but failed exact result checks:

| Mixed test | CPU operations | GPU operations | Expected | Observed |
| --- | ---: | ---: | ---: | ---: |
| A | 10,000,000 | 1,000,000 | 11,000,000 | 1,191,166 |
| B | 1,000,000 | 10,000,000 | 11,000,000 | 10,025,715 |
| Staggered A | 10,000,000 | 1,000,000 | 11,000,000 | 6,124,524 |
| Returning-add | 1,000,000 | 1,000,000 | 2,000,000 | 1,045,879 |

Staggered A recorded an exact 2,500,000 CPU prefix, 1,124,524 increments
in the middle phase versus 6,000,000 expected, and an exact 2,500,000 CPU tail.
The deficit of 4,875,476 was already present before the tail and remained
unchanged afterward. CPU timestamps were 0.000188–2.646470 seconds; host-observed
GPU markers were 0.013828–2.603420 seconds. These timestamps include observation
lag and do not count individual physically overlapping RMW operations.

The returning-add mixed sum of returned old values was 570,895,470,261 versus
1,999,999,000,000 expected. Each single-agent returned-old sum was exactly
499,999,500,000. Failure therefore also occurs with the returning GPU atomic
instruction under the current configuration.

These results support a boundary between tested serialized visibility and
concurrent heterogeneous RMW serialization. They do not identify the physical
cache/transaction mechanism or prove failure under every configuration.
Cached bridge controls remain unknown; the endpoint register is a live read.
Logs and baseline artifact hashes are under `build/tests/driver190-hardware/`.


## Driver 191 endpoint requester A/B experiment

The explicit `--requester-ab` mode keeps the existing non-returning shader,
CPU ordering, allocation type, mapping attributes and stagger algorithm. It
fixes CPU/GPU operation counts to 10,000,000 / 1,000,000 and one mixed trial
per setting, with the same single-agent controls before each mixed trial.

It requires live Requester Enable OFF for the baseline. After completed work
and confirmed queue removal, a dedicated driver API claims exclusive session
participation, saves Device Control 2, changes only endpoint bit 6 using a
16-bit read-modify-write and verifies readback. It does not alter any bridge,
MQD atomic-policy bit, PTE or advertised HSA capability. This is an explicitly
unqualified experiment despite the missing cached path prerequisites.

The ON phase reuses the same shared allocations, kernel and signal, recreating
the queue with identical settings. It checks full saved HQ_STATUS0, PTE/DMA/VA,
GFXHUB context, requested CPU mapping policy and endpoint registers against the
baseline. The recreated MQD storage address is logged separately. Completed
counter mismatches permit comparison; incomplete dispatch, failed controls or
guard damage stop the experiment.

After queue removal, explicit end restores only the original requester bit,
preserving unrelated control bits. Driver shutdown/client exit also restores
before reset after disabling bus mastering and draining transactions, then
verifies the bit after reset. Failed restoration blocks submissions and retains
recovery state. Software cannot guarantee restoration after physical unplug or
a driver-process crash.

```sh
build/hsa/mac-hsa-atomic-contention-test --run build/tests/hsa-atomic-contention.hsaco --requester-ab --timeout-seconds 120
```

Use a 700-second outer deadline. Completion of the A/B procedure is reported
separately from exact-count success. No Requester Enable ON hardware result has
yet been recorded; installation of driver 191 is required.
