# Interpreting the native CPU/GPU atomic experiment

`mac-hsa-atomic-contention-test` measures the currently configured DMA mapping,
queue firmware policy and interconnect. It cannot establish that all possible
configurations of this GPU or ARM64 host support or lack mixed-agent atomics.

## Queue firmware policy and shader scope are distinct

The current `aql_build_mqd` writes `CP_HQD_HQ_STATUS0 = 0x00004000`:
bit 14 is set and bit 29 is clear. The test prints this **candidate source policy**,
not a live MQD register readback. Compare the installed driver build with the
candidate before interpreting the result.

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
or proof of Thunderbolt/DART handling and CPU coherence. Neither diagnostic
changes PCI configuration, MQD atomic policy or memory cache policy. There is no
blind "enable atomics" override.

## Mapping and result limits

The buffer uses DriverKit DMA backing, default CPU mapping cache policy, equal
CPU/GPU virtual addresses, and the driver's system/snooped GPU PTE policy with
gfx12 MTYPE=2 (UC). Actual CPU cache attributes and live PTE contents are not
independently queried. The public allocation remains coarse-grained; this test
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
