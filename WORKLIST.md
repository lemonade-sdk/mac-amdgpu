# WORKLIST

Granular tasks. Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` deferred.

Phase numbers match `ROADMAP.md`. Target hardware fixed: Apple Silicon M5 Pro/Max/Ultra + TB5 + AMD Radeon AI PRO R9700 (gfx1201, RDNA4).

Last sync against `git log`: commit `413b539` (Phase 1B chunk 9 — TMR + PMFW orchestrator wiring).

---

## Phase 0 — Gates

- [x] 000  Project skeleton dirs (`dext`, `winsys`, `vk`, `docs`, `scripts`, `firmware`, `Host`)
- [x] 001  `git init` local repo on `main`
- [x] 002  Clone Linux kernel (sparse: amdgpu + DRM uapi + scheduler + ttm)
- [x] 003  Clone Mesa userspace driver tree
- [x] 004  Clone linux-firmware (sparse: `amdgpu/`)
- [x] 005  Verify GFX12/RDNA4 firmware blobs present (`gc_12_0_1_*`, `psp_14_0_3_*`, `smu_14_0_3*`, `sdma_7_0_1`)
- [x] 006  Verify Linux source has gfx_v12_1 / gmc_v12 / mes_v12_1 / psp_v14_0 / sdma_v7_1 / nbio_v7_11
- [x] 007  Read `qemu-vfio-apple/contrib/apple-vfio/VFIOUserPCIDriver/` end-to-end
- [x] 008  Document apple-vfio dext architecture → `docs/APPLE_VFIO_NOTES.md`
- [x] 009  Inspect `qemu-vfio-apple/traces/` — devcoredump confirms gfx1201/SDMA v7.0.1/HDP v7.0.0; visible VRAM 256 MB
- [-] 010  Generic trace parser (`scripts/parse_trace.py`) — deferred, low value vs current chunks
- [-] 011  Init-sequence reconstruction from trace — superseded by direct Linux source port
- [-] 012  Extend `guest-trace-amdgpu.sh` filters — deferred until we want to bit-compare a specific failure
- [-] 013  Cross-ref register writes vs upstream — done implicitly as part of every port commit (cited in commit msgs)
- [x] 014  Hardware in hand: R9700 + M5 + TB5 (per qemu-vfio-apple use)
- [-] 015  `firmware/MANIFEST.md` — deferred until we ship; tracked in commit messages for now
- [-] 016  `NOTICE` + `LICENSE.amdgpu` — same, deferred to first shippable build
- [x] 017  macOS 26 Tahoe + DriverKit SDK 25.4 pinned (project.yml + scripts/build.sh)
- [x] 018  Debug strategy: `os_log` channel `mac.amdgpu.*` + `log show --predicate 'subsystem == "mac.amdgpu"'`
- [x] 019  Apple Developer team confirmed: `YBQ9BU6Q6F`, bundle prefix `com.geramyloveless` (matches qemu-vfio-apple)
- [x] 020  Capture reference IP discovery binary from a real GPU → `docs/reference/gfx1151_discovery.bin` (Strix Halo, pulled via SSH; format is shared with gfx1201)
- [x] 021  Port plan documents for next 4 subsystems → `docs/port_plans/{GMC_v12,IH_v7,MES_v12_1,HELLO_PM4}.md`
- [x] 022  Capture DART/AS hard architectural limits → `docs/AS_DART_LIMITS.md`

---

## Phase 1A — Dext skeleton + entitlement

- [x] 100  Xcode workspace generated via `project.yml` + `scripts/build.sh` (xcodegen)
- [x] 101  `amdgpu.dext` target with PCIDriverKit framework, links against PCIDriverKit.framework
- [x] 102  Info.plist IOPCIPrimaryMatch `0x75511002&0xFFFFFFFF`, IOPCITunnelCompatible:true, IOProbeScore:10000
- [x] 103  Entitlements file `dext/MacAMDGPU.entitlements` declares transport.pci scoped to R9700
- [x] 104  Team identity `YBQ9BU6Q6F`, automatic signing
- [x] 105  Build empty dext bundle → bundle ID `com.geramyloveless.MacAMDGPUHost.MacAMDGPU` registered
- [ ] 106  **Submit entitlement request** in Apple Developer portal — *user action, not yet done*
- [x] 107  `Start()` opens IOPCIDevice, logs VID/DID/class/cmd/status/header
- [x] 108  Map BAR0 (registers) via _CopyDeviceMemoryWithIndex
- [x] 109  Map BAR2 (visible VRAM aperture) — exposed via CopyClientMemoryForType
- [x] 110  Map BAR5 (doorbells)
- [x] 111  Enable PCI bus master + Memory Space (lazy on first BAR map)
- [x] 112  Allocate up to 256 MSI-X vectors via IOInterruptDispatchSource
- [x] 113  DART/DMA path — `IODMACommand::PrepareForDMA` with 16 KB alignment
- [-] 114  DART round-trip test via SDMA — deferred until SDMA is up; CPU↔DMABuffer round-trip already proven
- [x] 115  `MacAMDGPUUserClient` subclass with Ping/GetIdentity/GetBARInfo/SetupInterrupts/WaitInterrupt/SetIRQMask/AllocateDMABuffer/FreeDMABuffer/ResetDevice/InitDevice/LoadFirmware/SetIPBase/GetIPBase/LoadDiscoveryBin selectors
- [x] 116  `scripts/macamdgpu_ping.swift` exercises all selectors; arm64 Mach-O, compiles + runs
- [x] 117  IRQ shared 16 KB page (`irqPending[4]` + `irqEnabled[4]`) atomic, lock-free
- [x] 118  os_log subsystem `mac.amdgpu.*` with per-module categories (`.psp`, `.smu`, `.disc`, `.init`)
- [x] 119  IOProbeScore=10000 + conflict warning vs VFIOUserPCIDriver in `dext/README.md`
- [x] 120  Host app (SwiftUI) `Host/MacAMDGPUHostApp.swift` with `OSSystemExtensionRequest` activation flow
- [x] 121  Dext embedded inside host app at `Contents/Library/SystemExtensions/MacAMDGPU.dext`
- [x] 122  FLR + hot-reset fallback via `IOPCIDevice::Reset`

**Milestone 1A:** ✅ dext + host app both build clean. Loads after user installs + Apple grants the transport.pci entitlement.

---

## Phase 1B — Hardware bringup (RDNA4 / gfx1201)

Cite Linux source file in commit messages.

### 1B.0 IP discovery
- [x] 150  Port IP discovery parser (`amdgpu_discovery.c` header + IPDS walker) → `dext/amdgpu/amdgpu_discovery.cpp`
- [x] 151  LoadDiscoveryBinary selector — host uploads a captured binary via DMABuffer, dext parses
- [ ] 152  On-die discovery read — RREG32(mmRCC_CONFIG_MEMSIZE) → BAR2 read of binary at `(vram_size_MB << 20) - 0x100000`. (Manual SetIPBase or LoadDiscoveryBinary works in the meantime.)
- [ ] 153  Cross-validate parser against `docs/reference/gfx1151_discovery.bin` — standalone parser test

### 1B.1 PSP v14 — done
- [x] 160  Port PSP v14 bootloader wait (`psp_v14_0_wait_for_bootloader`)
- [x] 161  Port `psp_v14_0_is_sos_alive` (C2PMSG_81 check)
- [x] 162  Port `psp_v14_0_bootloader_load_sos` — `psp_14_0_3_sos.bin` via C2PMSG_35/36 protocol
- [x] 163  Port `psp_v14_0_bootloader_load_component` (generic) — KDB/SPL/SysDrv/SocDrv/IntfDrv/HADDrv/RASDrv/IPKeyMgrDrv wrappers exposed via LoadFirmware selector
- [x] 164  Port `psp_v14_0_ring_create` — 4 KB km_ring inside 16 KB DART-aligned buffer; non-SR-IOV path
- [x] 165  Port `psp_ring_cmd_submit` — gfx_rb_frame builder + cmd_buf + fence_buf + wptr (C2PMSG_67) + poll fence
- [x] 166  Port `psp_setup_tmr` — 4 MB DART-mapped TMR with `virt_phy_addr=1` flag
- [x] 167  Port `psp_load_ip_fw` — generic LOAD_IP_FW for SMU/SDMA/RLC/CP/IMU/MES firmware

### 1B.2 SMU v14_0_3 — primitives done, depends on PMFW
- [x] 170  Port `smu_cmn_send_smc_msg_with_param` (MP1 C2PMSG_66/82/90)
- [x] 171  Port `smu_cmn_wait_for_response`
- [x] 172  `smu_test_message`, `smu_get_version` wrappers (PPSMC msgs 0x01, 0x02)
- [ ] 173  End-to-end: LoadFirmware(PMFW=0x112) → InitDevice(SMUInit) → SMU TestMessage responds. *Currently gated on entitlement + IP bases.*
- [ ] 174  Port `smu_v14_0_init_pptable_microcode` + pptable upload via PSP ring
- [ ] 175  Port `smu_v14_0_setup_pptable` + `smu_v14_0_init_smc_tables` (driver tables, tool table, allowed mask)
- [ ] 176  Enable basic clocks via SMU + verify telemetry readback (gfx clock, mem clock)

### 1B.3 GMC v12 — pending (see `docs/port_plans/GMC_v12.md`)
- [ ] 180  Port `gmc_v12_0_mc_init` — VRAM size detect (mmRCC_CONFIG_MEMSIZE)
- [ ] 181  Allocate dummy_page + mem_scratch (system memory, 16 KB aligned)
- [ ] 182  Port `mmhub_v4_1_0_init` — populate MMHUB register offset table
- [ ] 183  Port `gfxhub_v12_0_init` — populate GFXHUB register offset table
- [ ] 184  Port `amdgpu_gmc_get_vram_info` — ATOM table parse for VRAM type + width + vendor
- [ ] 185  VM manager config (num_level=3, block_size=9, 48-bit VA)
- [ ] 186  Register VM fault + ECC interrupt IDs with our IH dispatch table
- [ ] 187  **Minimal VRAM allocator** — even a 16 MB bump allocator carved off the top of visible VRAM lets RLC CSB + KIQ MQD land. Required by `GMC_v12.md` step 10.
- [ ] 188  Port `gmc_v12_0_gart_init` — GART in **system memory** (Path B from `GMC_v12.md`), 256 MB initial
- [ ] 189  Port `gmc_v12_0_vram_gtt_location` — gart_start/end + fb_start/end + agp_start/end
- [ ] 190  Port `mmhub_v4_1_0_gart_enable` — write MMHUB context0 PT base, L1/L2 TLB cntls, per-VMID context (14×)
- [ ] 191  Port `gfxhub_v12_0_gart_enable` — same for GFXHUB
- [ ] 192  Port HDP flush
- [ ] 193  Port `set_fault_enable_default`
- [ ] 194  Port `flush_gpu_tlb`
- [ ] 195  Sanity test: map a sysmem page through GART, GPU-side read via SDMA → CPU compare

### 1B.4 IH v7 — pending (see `docs/port_plans/IH_v7.md`)
- [ ] 200  Allocate Ring0 IH (256 KB sysmem, 16 KB aligned) + wptr shadow (16 KB)
- [ ] 201  Allocate Ring1 IH (256 KB sysmem) — dGPU only
- [ ] 202  Port `ih_v7_0_init_register_offset` — OSSSYS regIH_RB_* table
- [ ] 203  Port `ih_v7_0_toggle_interrupts(false)` — disable before configuring
- [ ] 204  Port `ih_v7_0_enable_ring` — write BASE/BASE_HI/CNTL/WPTR_ADDR
- [ ] 205  Port `ih_v7_0_doorbell_rptr` — IH_DOORBELL_RPTR + Ring1 client cfg
- [ ] 206  Port MSI storm + flood control (IH_MSI_STORM_CTRL, IH_INT_FLOOD_CNTL)
- [ ] 207  Port `ih_v7_0_toggle_interrupts(true)` — enable + force-update trigger
- [ ] 208  Port `amdgpu_ih_process` — wptr read (shadow first, MMIO fallback), entry walk (8 dword stride), per-source dispatch
- [ ] 209  Hook MSI-X handler in dext to call `amdgpu_ih_process` and signal `irqPending`/AsyncCompletion to userspace

### 1B.5 GFX12 + first PM4 — pending (see `docs/port_plans/HELLO_PM4.md`)
- [ ] 220  Port `gfx_v12_0_rlc_init` — RLC clear-state buffer (4 KB **VRAM**; needs minimal allocator)
- [ ] 221  Port `gfx_v12_0_rlc_resume`
- [ ] 222  Wait for RLC autoload complete (CP_MES_INSTR_PNTR poll)
- [ ] 223  Port `gfx_v12_0_gfxhub_enable` — GFXHUB enable + UTCL1 fault enable + TLB flush
- [ ] 224  Allocate GFX ring buffer (16 KB **GTT**; sysmem-backed)
- [ ] 225  Allocate write-back page (rptr + wptr + fence offsets, 4 KB sysmem)
- [ ] 226  Port `gfx_v12_0_constants_init` — tile mode tables + cache configs
- [ ] 227  Port `gfx_v12_0_cp_gfx_resume` — CP_RB0_BASE/BASE_HI/CNTL/WPTR/RPTR_ADDR + doorbell control
- [ ] 228  Port `gfx_v12_0_kiq_resume` — KIQ ring + MQD (VRAM); register init via GRBM_GFX_INDEX select
- [ ] 229  Port `gfx_v12_0_cp_gfx_start` — set CP_ME_CNTL ME0_ACTIVE=1
- [ ] 230  PM4 packet builders (NOP, WRITE_DATA, RELEASE_MEM) → `dext/amdgpu/amdgpu_pm4.h`
- [ ] 231  Submit NOP via doorbell, observe CP_RB0_RPTR advance
- [ ] 232  Submit WRITE_DATA + RELEASE_MEM EOP fence; observe fence_value in WB page
- [ ] 233  **"Hello GFX12" milestone** — IH ring delivers EOP IRQ, dispatch reaches userspace via WaitInterrupt
- [ ] 234  Register EOP/RAS/VM-fault interrupt source handlers via `amdgpu_irq_add_id` analog

### 1B.6 MES v12_1 — pending (see `docs/port_plans/MES_v12_1.md`)
- [ ] 250  LoadFirmware: uni_mes (`gc_12_0_1_uni_mes.bin`) or split mes+mes1 via `psp_load_ip_fw`
- [ ] 251  Port `mes_v12_1_sw_init` — allocate EOP (2 KB VRAM) + MQD (4 KB VRAM) + shared cmd buf
- [ ] 252  Port `mes_v12_1_enable` — CP_MES_CNTL pipeline reset + activate
- [ ] 253  Port `mes_v12_1_queue_init` — SCHED ring HQD register programming via GRBM select
- [ ] 254  Port `mes_v12_1_set_hw_resources` — MESAPI_SET_HW_RSRC payload (vmid masks, HQD masks, IP bases)
- [ ] 255  Port `mes_v12_1_submit_pkt_and_poll_completion` — analogous to PSP ring submit
- [ ] 256  Port `mes_v12_1_query_sched_status` — verify scheduler responds
- [ ] 257  Port `mes_v12_1_add_hw_queue` — MESAPI_ADD_QUEUE for the first user-facing GFX queue
- [ ] 258  Port `mes_v12_1_init_aggregated_doorbell` — 5 priority levels

### 1B.7 SDMA v7_1 — pending
- [ ] 270  Port `sdma_v7_0_init_microcode` (via LoadFirmware → PSP LOAD_IP_FW for SDMA0/SDMA1)
- [ ] 271  Allocate SDMA ring (16 KB GTT)
- [ ] 272  Port `sdma_v7_0_gfx_resume` — SDMA0_QUEUE0_RB_* + doorbell
- [ ] 273  Submit SDMA copy packet; sysmem→sysmem bit-compare
- [ ] 274  Submit sysmem→VRAM copy via GART; validate via BAR2 readback within visible window

### 1B.8 Compute queue + UAPI
- [ ] 290  Create compute queue via MES ADD_QUEUE (queue_type=COMPUTE)
- [ ] 291  UAPI selector: `alloc_bo(size, domain, flags) → handle` (sysmem domain for now)
- [ ] 292  UAPI selector: `free_bo(handle)`
- [ ] 293  UAPI selector: `map_bo(handle) → memory type id` (returns id for IOConnectMapMemory64)
- [ ] 294  UAPI selector: `submit_cs(queue_id, ib_handle, ib_size_dw, deps[]) → fence_handle`
- [ ] 295  UAPI selector: `wait_fence(handle, timeout_ns) → status`
- [ ] 296  UAPI selector: `query_info(type) → blob` (heap sizes, queue ids, asic id, gfx_version)
- [ ] 297  UAPI selector: `bo_export(handle) → mach_port` (for IOSurface bridging later)
- [ ] 298  Stress test: 10k consecutive submits + fence waits
- [ ] 299  Power cycle: D0 → D3 → D0 without PSP reset

**Milestone 1B:** programmable R9700 via custom UAPI under macOS.

---

## Phase 2A — winsys-mac (unchanged, after Phase 1B)

(see `ROADMAP.md` §4 — same as before)

- [ ] 200..211  *(retained — moved into 2A section in ROADMAP)*

---

## Phase 2B — Vulkan ICD, 2C compute validation, 3A graphics, 3B WSI, 3C optimization, 4 polish

*(unchanged from previous WORKLIST; see `ROADMAP.md` §4 for full list)*

---

## Cross-cutting

- [x] 600  Bit-compare register sequences against ftrace captures — done implicitly per port commit
- [ ] 601  CI: load dext, ping, alloc BO, submit NOP per-commit (after Phase 1B Hello GFX12)
- [ ] 602  ASAN/UBSAN builds of userspace ICD (Phase 2)
- [ ] 603  Memory leak audit (Phase 2)
- [ ] 604  `docs/PORTING_NOTES.md` — divergences from Linux + rationales
- [x] 605  `docs/APPLE_VFIO_NOTES.md` (reading notes on qemu-vfio-apple dext)
- [x] 606  `docs/AS_DART_LIMITS.md` (DART 1.5 GB ceiling / 64k mapping cap / 16 KB pages / 10× MMIO penalty / BAR config-reg synthesis)
- [x] 607  `docs/port_plans/{GMC_v12,IH_v7,MES_v12_1,HELLO_PM4}.md` (sourced from research agents)
