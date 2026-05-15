# WORKLIST

Granular tasks. Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` deferred.

Phase numbers match `ROADMAP.md`. Target hardware fixed: Apple Silicon M5 Pro/Max/Ultra + TB5 + AMD Radeon AI PRO R9700 (gfx1201, RDNA4).

Last sync against `git log` (v0.0.58, commit `c8d224e`): PSP ring is end-to-end working on real R9700. 8 bringup stages green (`IPDiscovery → PSPInit → PSPLoadSOS → PSPRingCreate → TMRSetup → SMUInit → GMCInit`). The 6-day silent-drop bug was `PSP_RING_TYPE__KM = 1` (= upstream's `__UM` / RBI ring) instead of `2` (= `__KM` / GPCOM); fixed via the two-agent line-by-line audit pattern. SMU PMFW loaded via PSP ring, SMU mailbox responsive, GMC + MMHUB configured with correct register offsets + GFX12 `IS_PTE` bit + NBIO HDP `remap_hdp_registers` programmed.

**Current blocker (RLCInit returns `kIOReturnUnsupported`):** the LoadFirmware path uses a simple "skip 32-byte common header" extractor that works for SMU PMFW (single-component) but PSP returns `0xFFFF0006 = TEE_ERROR_BAD_PARAMETERS` for SDMA / RLC / uni_mes because those use multi-component firmware layouts (sdma_v3_header, RLC sub-binaries for RLC_G/IRAM/DRAM/P/V/RESTORE_LIST, uni_mes split into CP_MES ucode + CP_MES_DATA at different offsets). Each needs the per-IP header parser from upstream `amdgpu_ucode_init_single_fw`.

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
- [x] 108  Map BAR0 (visible VRAM aperture) via _CopyDeviceMemoryWithIndex — used for VRAM read/write via MM_INDEX/MM_DATA at runtime
- [x] 109  Map BAR2 (VRAM doorbell/aperture) — exposed via CopyClientMemoryForType
- [x] 110  Map BAR5 (**register window — Bonaire+ AMD layout**, confirmed on R9700; not BAR0)
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
- [x] 152  On-die discovery read — RREG32(mmRCC_CONFIG_MEMSIZE) → VRAM read of binary at `(vram_size_MB << 20) - 0x10000` (64 KB TMR offset, **was 1 MB — upstream uses 64 KB**, fixed in commit `8fcc609`) via MM_INDEX/MM_DATA. Confirmed on R9700 hardware: parser auto-detects gfx1201 + `psp_v14_0_3` + `smu_v14_0_3` + `sdma_v7_0_1`. IP bases populated automatically by `InitDevice(IPDiscovery)`.
- [~] 153  Cross-validate parser against `docs/reference/gfx1151_discovery.bin` — exercised via `kMacAMDGPUMethodLoadDiscoveryBin` (selector 13) and `scripts/macamdgpu_ping.swift --load-discovery docs/reference/gfx1151_discovery.bin` (which then calls `showIPBases` to dump every populated IP base). Standalone host-only parser test would need the dext code compiled for macOS instead of DriverKit — deferred until we add a unit-test harness.

### 1B.1 PSP v14 — bootloader + SOS + ring all confirmed on R9700
- [x] 160  Port PSP v14 bootloader wait (`psp_v14_0_wait_for_bootloader`) — confirmed on hw
- [x] 161  Port `psp_v14_0_is_sos_alive` (C2PMSG_81 check; bit 31 must be set per commit `0cfed7f`) — confirmed on hw
- [x] 162  Port `psp_v14_0_bootloader_load_sos` — `psp_14_0_3_sos.bin` via C2PMSG_35/36 protocol — **loads + runs on R9700** (commit `d53c775`)
- [x] 163  Port `psp_v14_0_bootloader_load_component` (generic) — KDB/SPL/SysDrv/SocDrv/IntfDrv/HADDrv/RASDrv/IPKeyMgrDrv wrappers exposed via LoadFirmware selector
- [x] 164  Port `psp_v14_0_ring_create` — 4 KB km_ring inside 16 KB DART-aligned buffer; non-SR-IOV path — **ring created on R9700** (PSPRingCreate stage 4 reached)
- [~] 165  Port `psp_ring_cmd_submit` — gfx_rb_frame builder + cmd_buf + fence_buf + wptr (C2PMSG_67) + poll fence. wptr writes land cleanly (verified via DumpPSP — advances by 0x10 per submit), but PSP doesn't process frames whose cmd_buf is VRAM-backed. **Blocked on GART** — see 1B.0a + `docs/GART_PORT_PLAN.md`.
- [!] 166  Port `psp_setup_tmr` — 4 MB TMR with `virt_phy_addr=1` flag — **blocked on GART** (first ring submit; needs GART-routable MC for cmd_buf)
- [x] 167  Port `psp_load_ip_fw` — generic LOAD_IP_FW for SMU/SDMA/RLC/CP/IMU/MES firmware
- [x] 168  Port `psp_init_sos_microcode` — v1 + v2 header autodetect + sub-firmware extraction (KDB/SPL/SYS/SOC/INTF/DBG/RAS/IPKEYMGR/SOS) — commit `d53c775`
- [x] 169  Vendor upstream firmware structs — `common_firmware_header`, `psp_firmware_header_v1/v2`, `psp_fw_bin_desc` — commit `d53c775`
- [x] 169a Port `amdgpu_is_kicker_fw` + `kicker_device_list`; expose PCI revision in Identity — commit `3694bd3`

### 1B.0a GART bootstrap — done (v0.0.51..0.0.58)

PSP ring/cmd/fence stayed in VRAM (FB aperture; PSP fetches via internal path), and the host's DMA staging buffer is bound into GART for LOAD_IP_FW payloads. GART is needed for the firmware-load path, not for the ring frames themselves.

- [x] 140  GART-1: read upstream `amdgpu_gart.c` + `gmc_v12_0_gart_enable`; PTE format + register pokes documented
- [x] 141  GART-2: `dext/amdgpu/amdgpu_gart.h` + MMHUB IP registers — correct offsets confirmed against `mmhub_4_1_0_offset.h`
- [x] 142  GART-3: page table in VRAM at `kGARTPageTableVRAMOffset` (64 KB → 32 MB GART space), zero-filled via `bar0_memset_vram`
- [x] 143  GART-4: `gart_bind_sysmem` + `gart_bind_existing(busAddr, size)` — PTE write via `bar0_memcpy_to_vram` with `IS_PTE` bit
- [x] 144  GART-5: `gart_enable` ports the full `mmhub_v4_1_0_gart_enable` sub-call tree
- [x] 145  GART-6: inline-bootstrapped from `psp_load_sos` (proper `GMCInit`-before-PSP reorder is task 195)
- [x] 146  GART-7: validated on hw — PSP ring submits work end-to-end after the `KM=2` fix in commit `c8d224e`
- [x] 147  DMA-1: `MemoryWrite64` instead of per-dword `MemoryWrite32` for BAR-aperture VRAM writes
- [x] 147a DMA-1b: drop PSP fence-wait timeout 10 s → 1 s + bail-on-first-stage-failure
- [x] 147b DMA-2: DumpCmdBuf + DumpPSP selectors for ring debugging
- [-] 148  DMA-3: probe `IOMemoryDescriptor::CreateMapping` on BAR0 — deferred; current BAR0 path is only on the bringup hot path (PSP ring frames during init), not runtime
- [-] 149  SQ-2: write-combined mapping on BAR0 — deferred; same reason

### 1B.0b Post-SOS firmware multi-component parsing — current critical-path blocker

LoadFirmware currently does `(common_header.ucode_array_offset_bytes, common_header.ucode_size_bytes)` extraction. Works for SMU PMFW. Fails for SDMA/RLC/uni_mes with `TEE_ERROR_BAD_PARAMETERS` because their layouts have per-IP headers.

- [ ] 130  Vendor per-IP firmware headers: `sdma_firmware_header_v3_0`, `rlc_firmware_header_v2_0..v2_4`, `mes_firmware_header_v1_0`, `imu_firmware_header_v1_0`, `gfx_firmware_header_v2_0`, `psp_firmware_header_v2_0` extensions
- [ ] 131  Per-IP extractor: `amdgpu_ucode_extract(fwBytes, fw_size, gfxFwType) → (ucode_offset, ucode_size)`. Implements upstream's `amdgpu_ucode_init_single_fw` switch table for: SMU/SDMA TH0/IMU_I/IMU_D/RLC_G/RLC_IRAM/RLC_DRAM/RLC_P/RLC_V/RLC_RESTORE_LIST/CP_MES/CP_MES_DATA/RS64_PFP/RS64_ME/RS64_MEC/RS64 stack variants P0..P3
- [ ] 132  Per-IP "multi-frame" expansion: one host LoadFirmware(SDMA) → one LOAD_IP_FW frame (TH0=71). One host LoadFirmware(RLC) → multiple frames (G, IRAM, DRAM, …). One host LoadFirmware(uni_mes) → CP_MES + CP_MES_DATA. One LoadFirmware(CP) → PFP + ME + MEC + P0..P3 stacks (multiple files per IP)
- [ ] 133  Host swift: add per-IP wrapper functions `loadFirmwareRLC(file) { dext-side expansion }`; align the host queue with upstream's `psp_load_non_psp_fw` order (SMU → IMU → RLC → CP_MES → CP RS64 → SDMA)
- [ ] 134  IMU loading (IMU_I + IMU_D files via `gc_<v>_imu.bin`). RLC autoload chains the rest once IMU is up
- [ ] 135  Validate on hw: fence advances per frame, resp.status = 0 across the whole chain, `RLCInit` returns success (microcode_loaded flag flips)

### 1B.2 SMU v14_0_3 — primitives done, depends on PMFW
- [x] 170  Port `smu_cmn_send_smc_msg_with_param` (MP1 C2PMSG_66/82/90)
- [x] 171  Port `smu_cmn_wait_for_response`
- [x] 172  `smu_test_message`, `smu_get_version` wrappers (PPSMC msgs 0x01, 0x02)
- [ ] 173  End-to-end: LoadFirmware(PMFW=0x112) → InitDevice(SMUInit) → SMU TestMessage responds. *Currently gated on entitlement + IP bases.*
- [~] 174  Port `smu_v14_0_init_pptable_microcode` + pptable upload — chunk 29. LoadFirmware now accepts `kMacAMDGPUFwTypeIP_PPTABLE` (0x149 = 0x100 + GFX_FW_TYPE_PPTABLE 73) so userspace can upload a pptable blob through the standard psp_load_ip_fw path. SMU then ingests it via PPSMC table-transfer protocol (see 175). Upstream's pptable-from-VBIOS path is not implemented (no ATOM on AS).
- [~] 175  Port `smu_v14_0_setup_pptable` + `smu_v14_0_init_smc_tables` — chunk 29. Implemented as the underlying PPSMC primitives: `smu_set_driver_dram_addr` (msg 0x0E/0x0F), `smu_set_tools_dram_addr` (msg 0x10/0x11), `smu_transfer_table_dram_to_smu` (msg 0x13), `smu_transfer_table_smu_to_dram` (msg 0x12). The upstream tables zoo (driver_pptable / overdrive / combo / max_sustainable_clocks etc.) is not allocated kernel-side — userspace ICD is expected to allocate a single sysmem region, point SMU at it via set_driver_dram_addr, and drive per-table transfers as needed. Same protocol as upstream just without the in-kernel struct wrappers.
- [ ] 176  Enable basic clocks via SMU + verify telemetry readback (gfx clock, mem clock)

### 1B.3 GMC v12 — pending (see `docs/port_plans/GMC_v12.md`)
- [x] 180  Port `gmc_v12_0_mc_init` — VRAM size detect (mmRCC_CONFIG_MEMSIZE)
- [x] 181  Allocate dummy_page + mem_scratch (system memory, 16 KB aligned)
- [x] 182  Port `mmhub_v4_1_0_init` — populate MMHUB register offset table
- [x] 183  Port `gfxhub_v12_0_init` — populate GFXHUB register offset table
- [ ] 184  Port `amdgpu_gmc_get_vram_info` — ATOM table parse for VRAM type + width + vendor
- [x] 185  VM manager config (num_level=3, block_size=9, 48-bit VA)
- [x] 186  Register VM fault + ECC interrupt IDs with our IH dispatch table — done implicitly: `mac_amdgpu_ih_dispatch` routes (CLIENT_ATHUB, SRC_UTCL2_FAULT) → kIRQBitVMFault, (CLIENT_GFX, SRC_CP_ECC_ERROR) → kIRQBitGFXRASError, (CLIENT_SDMA*, SRC_SDMA_TRAP) → kIRQBitSDMA{0,1}Trap (chunk 14).
- [x] 187  **Minimal VRAM allocator** — even a 16 MB bump allocator carved off the top of visible VRAM lets RLC CSB + KIQ MQD land. Required by `GMC_v12.md` step 10.
- [x] 188  Port `gmc_v12_0_gart_init` — GART in **system memory** (Path B from `GMC_v12.md`), 256 MB initial
- [x] 189  Port `gmc_v12_0_vram_gtt_location` — gart_start/end + fb_start/end + agp_start/end
- [x] 190  Port `mmhub_v4_1_0_gart_enable` — write MMHUB context0 PT base, L1/L2 TLB cntls, per-VMID context (14×)
- [x] 191  Port `gfxhub_v12_0_gart_enable` — same for GFXHUB
- [x] 192  Port HDP flush
- [-] 193  Port `set_fault_enable_default` — the per-VMID CONTEXT*_CNTL writes in hub_setup_vmid_config already enable faults; explicit toggle is suspend/resume territory.
- [x] 194  Port `flush_gpu_tlb`
- [~] 195  Sanity test: SDMA copy via `kMacAMDGPUMethodSDMACopyTest` — chunk 21. Codebase path complete (sysmem→sysmem same-buffer DMA copy + FENCE poll); untested on real hw.

### 1B.4 IH v7 — pending (see `docs/port_plans/IH_v7.md`)
- [x] 200  Allocate Ring0 IH (256 KB sysmem, 16 KB aligned) + wptr shadow (16 KB)
- [x] 201  Allocate Ring1 IH (256 KB sysmem) — dGPU only
- [x] 202  Port `ih_v7_0_init_register_offset` — OSSSYS regIH_RB_* table
- [x] 203  Port `ih_v7_0_toggle_interrupts(false)` — disable before configuring
- [x] 204  Port `ih_v7_0_enable_ring` — write BASE/BASE_HI/CNTL/WPTR_ADDR
- [x] 205  Port `ih_v7_0_doorbell_rptr` — IH_DOORBELL_RPTR + Ring1 client cfg
- [x] 206  Port MSI storm + flood control (IH_MSI_STORM_CTRL, IH_INT_FLOOD_CNTL)
- [x] 207  Port `ih_v7_0_toggle_interrupts(true)` — enable + force-update trigger
- [x] 208  Port `amdgpu_ih_process` — wptr read (shadow first, MMIO fallback), entry walk (8 dword stride), per-source dispatch
- [x] 209  Hook MSI-X handler in dext to call `amdgpu_ih_process` and signal `irqPending`/AsyncCompletion to userspace
- [x] 210  Per-source dispatch table (CP_EOP / UTCL2_FAULT / SDMA_TRAP / RAS) — `mac_amdgpu_ih_dispatch` translates client_id+src_id → kIRQBit* in irqPending

### 1B.5 GFX12 + first PM4 — pending (see `docs/port_plans/HELLO_PM4.md`)
- [x] 220  Port `gfx_v12_0_rlc_init` — RLC clear-state buffer (4 KB **VRAM**; needs minimal allocator) — chunk 15
- [x] 221  Port `gfx_v12_0_rlc_resume` — chunk 15
- [x] 222  Wait for RLC autoload complete (CP_MES_INSTR_PNTR poll) — chunk 15
- [x] 223  Port `gfx_v12_0_gfxhub_enable` — GFXHUB enable + UTCL1 fault enable + TLB flush — done as part of gmc_init in chunk 11
- [x] 224  Allocate GFX ring buffer (16 KB **GTT**; sysmem-backed) — chunk 16 (`cp_alloc_storage`)
- [x] 225  Allocate write-back page (rptr + wptr + fence offsets, 4 KB sysmem) — chunk 16
- [x] 226  Port `gfx_v12_0_constants_init` — GRBM_CNTL.READ_TIMEOUT + SH_MEM_CONFIG(VMID 0) — chunk 19 (`gfx_constants_init`)
- [x] 227  Port `gfx_v12_0_cp_gfx_resume` — CP_RB0_BASE/BASE_HI/CNTL/WPTR/RPTR_ADDR + doorbell control — chunk 17 (`cp_hqd_program`)
- [~] 228  Port `gfx_v12_0_kiq_resume` — deferred; using direct CP_RB0 path. MES-managed queue setup is Phase 1B chunk 18+.
- [x] 229  Port `gfx_v12_0_cp_gfx_start` — set CP_ME_CNTL ME0_ACTIVE=1 — chunk 17 (`cp_enable`)
- [x] 230  PM4 packet builders (NOP, WRITE_DATA, RELEASE_MEM) → `dext/amdgpu/amdgpu_pm4.h` — chunk 16
- [x] 231  Submit NOP via doorbell, observe CP_RB0_RPTR advance — chunk 17 (`cp_emit_eop_fence` + SubmitTestPM4 selector)
- [x] 232  Submit WRITE_DATA + RELEASE_MEM EOP fence; observe fence_value in WB page — chunk 17 (same)
- [~] 233  **"Hello GFX12" milestone** — codebase path complete including automatic on-die IP discovery — bringup is fully self-bootstrapping on hardware; the milestone is still UNTESTED on real hw. See chunk 17 commit `8a04754` and chunk 18 commit `34470dd`.
- [x] 234  Register EOP/RAS/VM-fault interrupt source handlers via `amdgpu_irq_add_id` analog — chunk 14 (`mac_amdgpu_ih_dispatch`)

### 1B.6 MES v12_1 — sw_init + enable landed (chunk 22)
- [~] 250  LoadFirmware: uni_mes (`gc_12_0_1_uni_mes.bin`) — psp_load_ip_fw path works for RS64_MES (76); LoadFirmware now parses the firmware header and stashes `mes_uc_start_addr` on the SCHED pipe so mes_enable can program CP_MES_PRGRM_CNTR_START. **Caller still has to upload the actual firmware.**
- [x] 251  Port `mes_v12_1_sw_init` — `mes_alloc_storage` allocates EOP (2K), MQD (4K), ring (64K), and cmd buf (16K) per pipe in DART-mapped sysmem — chunk 22.
- [x] 252  Port `mes_v12_1_enable` — CP_MES_CNTL pipeline reset + PRGRM_CNTR_START + PIPE0_ACTIVE — chunk 22 (`mes_enable`, uni_mes pipe 0 only).
- [x] 253  Port `mes_v12_1_queue_init` — SCHED ring HQD register programming via GRBM select + matching MQD-in-memory population at upstream v12_compute_mqd byte offsets — chunk 23 (`mes_queue_init`).
- [x] 254  Port `mes_v12_1_set_hw_resources` — chunk 28. Packed wire-format struct `MES_SetHwResources` (64 dw, validated by static_assert), lazy-allocs sch_ctx + status_fence buffers (4 KB sysmem each), submits with sensible defaults (VMID 0 kernel-reserved, GFX HQD0 reserved for direct CP path, all SDMA/compute HQDs handed to MES, disable_reset + disable_mes_log + enable_reg_active_poll + enable_level_process_quantum_check + use_different_vmid_compute flags set).
- [x] 255  Port `mes_v12_1_submit_pkt_and_poll_completion` → `mes_submit_pkt` — chunk 26. Patches embedded `MES_API_Status` fence_addr/value, writes the 64-dword frame to the SCHED ring, chains a QUERY_SCHEDULER_STATUS frame for fence acknowledgement, kicks the BAR5 doorbell, polls the status slot.
- [x] 256  Port `mes_v12_1_query_sched_status` → `mes_query_sched_status` — chunk 26. Thin wrapper around `mes_submit_pkt` with the QUERY opcode.
- [x] 257  Port `mes_v12_1_add_hw_queue` — chunk 28. Packed `MES_AddQueue` (64 dw), exposed as `mes_add_hw_queue(dev, mes, input)`. User selector `kMacAMDGPUMethodMESAddQueue` (22) maps BO handles to MQD/wptr GPU addresses + submits.
- [x] 258  Port `mes_v12_1_init_aggregated_doorbell` — chunk 28. Writes CP_MES_DOORBELL_CONTROL1..5 with the 5 priority doorbells starting at `kMES_AggregatedDoorbellsBase = 0x100`, then CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN. Called automatically at the end of `mes_init_full`.

### 1B.7 SDMA v7_1 — ring bringup landed (chunk 20)
- [x] 270  Port `sdma_v7_0_init_microcode` — generic LoadFirmware path now flips `sdma.microcode_loaded` after PSP LOAD_IP_FW for SDMA0 (`0x109`) / SDMA1 (`0x10A`).
- [x] 271  Allocate SDMA ring + WB page (16 KB sysmem, 16 KB-aligned) per instance via `sdma_alloc_storage` — chunk 20.
- [x] 272  Port `sdma_v7_0_gfx_resume_instance` — RB_CNTL/BASE/RPTR/WPTR/DOORBELL + MCU_CNTL unhalt + RB_ENABLE/IB_ENABLE — chunk 20 (`sdma_gfx_resume_instance`).
- [x] 273  SDMA FENCE-packet ring test — `sdma_ring_test` runs inside `sdma_init_full`; selector `kMacAMDGPUMethodSDMACopyTest` (15) exposes a COPY_LINEAR + FENCE smoke from userspace — chunk 21.
- [ ] 274  Submit sysmem→VRAM copy via GART; validate via BAR2 readback within visible window

### 1B.8 Compute queue + UAPI
- [x] 290  Create compute queue via MES ADD_QUEUE — chunk 28. `kMacAMDGPUMethodMESAddQueue` (selector 22) builds a `MES_AddQueue` frame from caller-supplied (queue_type, doorbell_offset, mqd BO handle, wptr BO handle, priority) and submits via `mes_add_hw_queue`. UNTESTED on real hw (gated on MES microcode being live + SET_HW_RESOURCES succeeding).
- [x] 291  UAPI selector: `BOAlloc(size) → handle, bus_addr, byte_offset` — chunk 24. Per-client bump allocator over the existing DMABuffer; userspace mmap of the DMABuffer gives direct CPU access to BO bytes.
- [x] 292  UAPI selector: `BOFree(handle)` — chunk 24 (bump-only, doesn't reclaim).
- [~] 293  Map BO — N/A in current design; userspace maps the DMABuffer once via `kMacAMDGPUMemoryTypeDMABuffer` and the BOAlloc byte_offset selects the window inside it. `BOGetInfo(handle)` returns bus_addr + byte_offset + size.
- [x] 294  UAPI selector: `SubmitIB(ib_handle, ib_size_dw, queue=0) → fence_value` — chunk 24. Copies BO bytes into the CP_RB0 ring via `cp_ring_write`, emits an EOP fence, kicks the doorbell.
- [x] 295  UAPI selector: `WaitFence(target_value, timeout_us) → observed_value` — chunk 24. Polls `cp.fence_cpu` from the WB page.
- [x] 296  UAPI selector: `QueryInfo(type) → blob` — chunk 25. Currently exposes GFX_VERSION, VRAM sizes (visible + total), packed IP versions for GMC/SDMA/PSP/SMU, and the highest BringupStage reached.
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
- [x] 604  `docs/PORTING_NOTES.md` — divergences from Linux + rationales — chunk 28. Covers DART vs Linux IOMMU, VBIOS/ATOM unavailability, IP base resolution, dual CP_RB0 + MES path, firmware loading, doorbell layout, SRBM mutex, PM gaps, and the upstream subsystems we chose not to port.
- [x] 605  `docs/APPLE_VFIO_NOTES.md` (reading notes on qemu-vfio-apple dext)
- [x] 606  `docs/AS_DART_LIMITS.md` (DART 1.5 GB ceiling / 64k mapping cap / 16 KB pages / 10× MMIO penalty / BAR config-reg synthesis)
- [x] 607  `docs/port_plans/{GMC_v12,IH_v7,MES_v12_1,HELLO_PM4}.md` (sourced from research agents)
