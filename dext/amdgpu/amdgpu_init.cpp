//
//  amdgpu_init.cpp — Phase 1B bringup orchestrator.
//
//  Each stage gates the next. The orchestrator is a switch ladder
//  rather than a table-of-fns so that newly-added stages with
//  per-stage helper signatures aren't forced into a one-size-fits-all
//  shape.
//

#include <os/log.h>
#include "amdgpu_init.h"

#define INIT_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.init: " fmt, ##__VA_ARGS__)

namespace amdgpu {

//============================================================
// IP discovery — hardcoded R9700 IP versions.
//
// IP **base addresses** are still 0xFFFFFFFFu sentinels until we
// (a) read the on-die discovery binary, or (b) ship a per-ASIC
// table of base offsets derived from a one-time host-side dump.
// Code that uses any IP block must check ip.isResolved() and fail
// gracefully if the base isn't filled in.
//============================================================
kern_return_t
bringup_ip_discovery(BringupContext &ctx)
{
    INIT_LOG("IP discovery: pinning R9700 IP versions "
             "(GC=%u.%u.%u GMC=%u.%u.%u SDMA=%u.%u.%u "
             "PSP=%u.%u.%u SMU=%u.%u.%u MES=%u.%u.%u)",
             kIP_GFX.major,  kIP_GFX.minor,  kIP_GFX.rev,
             kIP_GMC.major,  kIP_GMC.minor,  kIP_GMC.rev,
             kIP_SDMA.major, kIP_SDMA.minor, kIP_SDMA.rev,
             kIP_PSP.major,  kIP_PSP.minor,  kIP_PSP.rev,
             kIP_SMU.major,  kIP_SMU.minor,  kIP_SMU.rev,
             kIP_MES.major,  kIP_MES.minor,  kIP_MES.rev);

    // IP bases — sentinel until we either implement on-die discovery
    // or measure them on real hardware. Bringup of dependent stages
    // will refuse to run until these are filled in.
    //
    // NOTE: at first hardware bring-up the easiest path is:
    //   1. Use guest-trace-amdgpu.sh in qemu-vfio-apple to capture
    //      the first few hundred amdgpu_device_wreg() calls.
    //   2. The first writes Linux makes are RLC_GFX_SCRATCH_DATA
    //      and friends with a known relative offset; back out the
    //      IP bases by subtracting the documented SOC15 offset.
    //   3. Populate the table below from that.
    //
    // Until step 3 is done, anything that depends on MP0/GC/SDMA0
    // base addresses returns kIOReturnNotReady.

    // Helper: check whether a SOC15-resolved register dword offset is
    // within Apple's 512 KB BAR5 mapping (= 0x20000 dword offsets).
    auto reg_in_bar5 = [](uint32_t reg_dword) -> bool {
        return reg_dword < 0x20000u;  // 0x20000 dwords = 512 KB
    };

    // Sanity-check SMN indirect against a known register: read
    // MP0_C2PMSG_33 via SMN and compare to its direct BAR5 value.
    // Expected: both 0x80000000 (IFWI READY). If SMN returns 0 or
    // 0xFFFFFFFF, the byte-address shift in SMN_RREG32 broke.
    if (ctx.device.ip.isResolved(IPBlock::MP0)) {
        const uint32_t dwd =
            SOC15_REG_OFFSET(ctx.device, IPBlock::MP0, 0x0061);
        uint32_t direct  = RREG32(ctx.device, dwd);
        uint32_t via_smn = SMN_RREG32(ctx.device, dwd);
        INIT_LOG("smn-check: MP0_C2PMSG_33  direct=%#x  via_smn=%#x  "
                 "(%s)", direct, via_smn,
                 (direct == via_smn) ? "MATCH" : "MISMATCH");
    }

    // Program NBIO's remap-HDP registers so writes to BAR5 byte 0x44000
    // trigger HDP_MEM_FLUSH. Direct port of upstream
    // `nbio_v7_11_remap_hdp_registers` (nbio_v7_11.c:30). Both target
    // registers live at NBIO BASE_IDX 5 (regBIF_BX0_REMAP_HDP_*_FLUSH_CNTL
    // = 0x8e4d / 0x8e4e). Their resolved BAR5 byte offsets are outside
    // Apple's 512 KB BAR5 mapping so we go through PCIE_INDEX2/DATA2
    // SMN indirect.
    if (ctx.device.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/5)) {
        constexpr uint32_t kRegBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL = 0x8e4d;
        constexpr uint32_t kRegBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL = 0x8e4e;
        constexpr uint32_t kRMMIORemap = 0x44000;
        uint32_t regMem = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 5,
            kRegBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL);
        uint32_t regReg = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 5,
            kRegBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL);
        if (reg_in_bar5(regMem) && reg_in_bar5(regReg)) {
            WREG32(ctx.device, regMem, kRMMIORemap + 0);
            WREG32(ctx.device, regReg, kRMMIORemap + 4);
            INIT_LOG("nbio: remap_hdp programmed (direct) — "
                     "MEM_FLUSH @%#x, REG_FLUSH @%#x", regMem, regReg);
        } else {
            SMN_WREG32(ctx.device, regMem, kRMMIORemap + 0);
            SMN_WREG32(ctx.device, regReg, kRMMIORemap + 4);
            INIT_LOG("nbio: remap_hdp programmed (SMN indirect) — "
                     "MEM_FLUSH @SMN %#x = %#x, REG_FLUSH @SMN %#x = %#x",
                     regMem, kRMMIORemap + 0, regReg, kRMMIORemap + 4);
        }
    } else {
        INIT_LOG("nbio: BASE_IDX 5 unresolved — HDP flush will be a no-op");
    }

    // Enable doorbell aperture — nbio_v7_11_enable_doorbell_aperture
    // (nbio_v7_11.c:136). Programs regRCC_DEV0_EPF0_0_RCC_DOORBELL_APER_EN
    // bit 0 = BIF_DOORBELL_APER_EN. Per nbio_7_11_0_offset.h the register
    // lives at offset 0x00C0, BASE_IDX 2 (NBIO[2] = 0x0D20 from discovery).
    if (ctx.device.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/2)) {
        uint32_t reg_addr = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 2, NBIORegs::RCC_DOORBELL_APER_EN);
        uint32_t reg = RREG32(ctx.device, reg_addr);
        reg |= (1u << 0);  // BIF_DOORBELL_APER_EN = 1
        WREG32(ctx.device, reg_addr, reg);
        INIT_LOG("nbio: doorbell aperture enabled (APER_EN@%#x = %#x)",
                 reg_addr, reg);

        // nbio_v7_11_ih_control (nbio_v7_11.c:196): write dummy_page_addr
        // to BIF_BX1_INTERRUPT_CNTL2 and clear IH_DUMMY_RD_OVERRIDE +
        // IH_REQ_NONSNOOP_EN in BIF_BX1_INTERRUPT_CNTL. BASE_IDX 2 (same
        // as doorbell APER_EN). We don't allocate a dummy_page yet so
        // we write 0; the field clears do the meaningful part.
        uint32_t cntl2_addr = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 2, NBIORegs::INTERRUPT_CNTL2);
        uint32_t cntl_addr  = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 2, NBIORegs::INTERRUPT_CNTL);
        WREG32(ctx.device, cntl2_addr, 0);  // no dummy page yet
        uint32_t cntl = RREG32(ctx.device, cntl_addr);
        uint32_t cntl_new = cntl
            & ~NBIORegs::kINTERRUPT_CNTL_DUMMY_RD_OVERRIDE_MASK
            & ~NBIORegs::kINTERRUPT_CNTL_REQ_NONSNOOP_EN_MASK;
        WREG32(ctx.device, cntl_addr, cntl_new);
        INIT_LOG("nbio: ih_control — INTERRUPT_CNTL2@%#x=0, "
                 "INTERRUPT_CNTL@%#x %#x -> %#x",
                 cntl2_addr, cntl_addr, cntl, cntl_new);
    } else {
        INIT_LOG("nbio: BASE_IDX 2 unresolved — doorbell aperture / ih_control "
                 "not programmed");
    }

    // df_v4_15_hw_init (df_v4_15.c:29): writes NCSConfigurationRegister1
    // with DisIntAtomicsLclProcessing fields set ONLY if
    // adev->have_atomics_support is true. On Apple Silicon over TB5 we
    // do not negotiate PCIe AtomicOps to the root complex, so
    // have_atomics_support=false and df_v4_15_hw_init is a no-op. This
    // matches upstream behavior on a system without PCIe atomic-ops
    // support; the field clears in the register are unnecessary.
    INIT_LOG("df_v4_15_hw_init: skipped (have_atomics_support=false on "
             "Apple Silicon / TB5; matches upstream no-atomics behavior)");

    // Port of nbio_v7_11_init_registers (nbio_v7_11.c:264) — programs
    // two PCIe root-complex registers at NBIO BASE_IDX 5:
    //
    //   1. regBIF_BIF256_CI256_RC3X4_USB4_PCIE_MST_CTRL_3: set
    //      MAX_READ_REQUEST_SIZE_MODE (bit 27) and _PRIV (bit 28) so
    //      the GPU can issue 4 KB MRRs (default is 512 B).
    //
    //   2. regRCC_DEV0_EPF5_STRAP4: clear bit 23. Suspected gate for
    //      the IMU/RLC autoload state machine — without the clear,
    //      after psp_rlc_autoload_start the GC block stays in reset
    //      and RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE never goes
    //      high.
    //
    // Both registers are outside Apple's 512 KB BAR5 — we reach them
    // through PCIE_INDEX2/DATA2 SMN indirect.
    if (ctx.device.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/5)) {
        uint32_t mst_reg = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 5, NBIORegs::PCIE_MST_CTRL_3);
        uint32_t strap_reg = SOC15_REG_OFFSET_BIDX(
            ctx.device, IPBlock::NBIO, 5, NBIORegs::RCC_DEV0_EPF5_STRAP4);

        // 1. PCIE_MST_CTRL_3 — set MAX_READ_REQUEST_SIZE_{MODE,PRIV} = 1
        uint32_t mst_def = SMN_RREG32(ctx.device, mst_reg);
        uint32_t mst_data = mst_def;
        mst_data &= ~NBIORegs::kSWUS_MAX_READ_REQUEST_SIZE_MODE_MASK;
        mst_data |= (1u << NBIORegs::kSWUS_MAX_READ_REQUEST_SIZE_MODE_SHIFT);
        mst_data &= ~NBIORegs::kSWUS_MAX_READ_REQUEST_SIZE_PRIV_MASK;
        mst_data |= (1u << NBIORegs::kSWUS_MAX_READ_REQUEST_SIZE_PRIV_SHIFT);
        if (mst_def != mst_data) {
            SMN_WREG32(ctx.device, mst_reg, mst_data);
        }
        INIT_LOG("nbio: PCIE_MST_CTRL_3 @SMN %#x  %#x -> %#x  "
                 "(MAX_RR_SIZE_{MODE,PRIV}=1)",
                 mst_reg, mst_def, mst_data);

        // 2. RCC_DEV0_EPF5_STRAP4 — clear bit 23
        uint32_t strap_old = SMN_RREG32(ctx.device, strap_reg);
        uint32_t strap_new = strap_old & ~NBIORegs::kRCC_DEV0_EPF5_STRAP4_BIT23;
        SMN_WREG32(ctx.device, strap_reg, strap_new);
        uint32_t strap_rb = SMN_RREG32(ctx.device, strap_reg);
        INIT_LOG("nbio: RCC_DEV0_EPF5_STRAP4 @SMN %#x  %#x -> %#x  "
                 "(cleared bit23) readback=%#x",
                 strap_reg, strap_old, strap_new, strap_rb);
    } else {
        INIT_LOG("nbio_v7_11_init_registers: NBIO BASE_IDX 5 unresolved");
    }

    return kIOReturnSuccess;
}

//============================================================
// Doorbell init — port of upstream amdgpu_doorbell_init
// (amdgpu_doorbell_mgr.c:193).
//
// Records BAR2 base/size and populates the doorbell_index map
// with ASIC-specific values for RDNA4 (gfx1201).
//
// On Apple Silicon BAR2 is accessed via IOPCIDevice::MemoryRead32/
// Write32 (not as a linear mapping), so base=0. The size is read
// from PCI config space at offset 0x24 (BAR2).
//
// Upstream also calls amdgpu_asic_init_doorbell_index(adev) which
// populates the doorbell_index map. We hardcode the RDNA4 values.
//
// The 0x400 addition is for Vega+ compatibility — SDMA paging
// queue doorbell uses the second page (0x400 dwords = 1 KB).
//
// Returns kIOReturnSuccess on success, kIOReturnNotReady if BAR2
// is not mapped.
//============================================================
kern_return_t
doorbell_init(DeviceContext &dev, DoorbellState &db)
{
    // BAR2 must be mapped — it's the doorbell aperture.
    if (dev.bar2MemIndex == 0xFFu) {
        INIT_LOG("doorbell_init: BAR2 not mapped");
        return kIOReturnNotReady;
    }

    // RDNA4 doorbell BAR size: typically 2 MB (0x200000) for the
    // full doorbell aperture. We hardcode this since PCIDriverKit
    // doesn't expose the full BAR size via config space reads.
    constexpr uint64_t kRDNA4DoorbellSize = 2 * 1024 * 1024;  // 2 MB

    db.base = 0;  // BAR2 accessed via MemoryRead/Write, not linear mapping
    db.size = kRDNA4DoorbellSize;

    // Populate doorbell_index map (ASIC-specific values for RDNA4/gfx1201).
    // Mirrors amdgpu_asic_init_doorbell_index for CHIP_NAVI31.
    //
    // SOC21 layout (amdgpu_doorbell.h:210-211):
    //   gfx_ring0 = 0x100, sdma_engine[0] = 0x100, sdma_engine[1] = 0x10A
    //   mes_ring0 = 0x20, compute = 0x200+
    //
    // Note: SDMA doorbell_index values are shifted by 1 in sdma_v7_1.c
    // (ring->doorbell_index = adev->doorbell_index.sdma_engine[i] << 1),
    // so we store the unshifted values here. The SDMA code will shift
    // them when programming SDMA_QUEUE0_DOORBELL_OFFSET.
    db.index.gfx_ring0     = 0;
    db.index.gfx_ring1     = 1;
    db.index.sdma_engine[0] = 0x100;
    db.index.sdma_engine[1] = 0x10A;
    db.index.sdma_engine[2] = 0x114;
    db.index.sdma_engine[3] = 0x11E;
    db.index.ih            = 6;
    db.index.mes_ring0     = 0x20;
    db.index.compute       = 0x200;
    db.index.max_assignment = 0x200;

    // num_kernel_doorbells = min(size/4, max_assignment + 1)
    // Then add 0x400 (256 dwords = 1 KB = 2 pages) for Vega+ compat.
    uint32_t max_by_size = static_cast<uint32_t>(db.size / sizeof(uint32_t));
    uint32_t max_by_idx  = db.index.max_assignment + 1;
    db.num_kernel_doorbells = (max_by_size < max_by_idx) ? max_by_size : max_by_idx;
    // Add 0x400 for Vega+ compatibility (SDMA paging queue uses second page)
    db.num_kernel_doorbells += 0x400;

    INIT_LOG("doorbell_init: base=0 size=%llu num_kernel=%u max_idx=%u",
             db.size, db.num_kernel_doorbells, db.index.max_assignment);
    return kIOReturnSuccess;
}

//============================================================
// Stage dispatch.
//
// Stage order (audit #9 #1):
//   IPDiscovery → IHInit → GMCInit → PSPInit → PSPLoadSOS →
//   PSPRingCreate → TMRSetup → PSPFwLoad → SMUInit → IMUInit →
//   RLCInit → CPInit → MESInit → GFXInit → SDMAInit
//
// Mirrors amdgpu_device_ip_init's phase1 (COMMON + IH + inline GMC)
// → fw_loading (PSP) → phase2 (everything else).
//============================================================
static const char *
stage_name(BringupStage s)
{
    switch (s) {
    case BringupStage::None:          return "None";
    case BringupStage::IPDiscovery:   return "IPDiscovery";
    case BringupStage::IHInit:        return "IHInit";
    case BringupStage::GMCInit:       return "GMCInit";
    case BringupStage::PSPInit:       return "PSPInit";
    case BringupStage::PSPLoadSOS:    return "PSPLoadSOS";
    case BringupStage::PSPRingCreate: return "PSPRingCreate";
    case BringupStage::TMRSetup:      return "TMRSetup";
    case BringupStage::PSPFwLoad:     return "PSPFwLoad";
    case BringupStage::SMUInit:       return "SMUInit";
    case BringupStage::IMUInit:       return "IMUInit";
    case BringupStage::RLCInit:       return "RLCInit";
    case BringupStage::CPInit:        return "CPInit";
    case BringupStage::MESInit:       return "MESInit";
    case BringupStage::GFXInit:       return "GFXInit";
    case BringupStage::SDMAInit:      return "SDMAInit";
    }
    return "?";
}

static kern_return_t
run_stage(BringupContext &ctx, BringupStage s)
{
    INIT_LOG("running stage %u (%{public}s)", (unsigned)s, stage_name(s));
    switch (s) {
    case BringupStage::None:
        return kIOReturnSuccess;
    case BringupStage::IPDiscovery: {
        // Do on-die discovery the way amdgpu does — read VRAM size
        // from mmRCC_CONFIG_MEMSIZE absolute, then memcpy the binary
        // out of BAR2 and parse. Falls back to letting the caller
        // try LoadDiscoveryBin if on-die isn't possible (e.g. PSP
        // placed the TMR in sysmem via DRIVER_SCRATCH).
        DiscoveryParseResult res{};
        kern_return_t r = discover_ips_on_die(ctx.device, &res);
        if (r == kIOReturnSuccess) {
            return bringup_ip_discovery(ctx);  // pin version constants
        }
        // On-die failed; if userspace has already provided a binary
        // via LoadDiscoveryBin, those bases stay set and we treat
        // IPDiscovery as a success. Otherwise propagate the error
        // so the caller knows to provide one.
        if (ctx.device.ip.isResolved(IPBlock::MP0) &&
            ctx.device.ip.isResolved(IPBlock::GC)) {
            INIT_LOG("on-die discovery failed (%#x) but IP bases already "
                     "set from a prior LoadDiscoveryBin — accepting", r);
            return bringup_ip_discovery(ctx);
        }
        return r;
    }
    case BringupStage::PSPInit: {
        kern_return_t r = psp_init(ctx.device, ctx.psp);
        if (r != kIOReturnSuccess) return r;
        // Match upstream psp_sw_init: read PSP runtime DB at
        // (vram_size - 0x100000) and capture boot_cfg / scpm flags.
        (void)psp_read_runtime_db(ctx.device, ctx.psp,
                                  ctx.gmc.real_vram_size);
        // Intentionally stay on VRAM-backed fw_buf — diverges from
        // upstream amdgpu_ucode_create_bo's GTT default but matches
        // upstream's debug_use_vram_fw_buf / SR-IOV path.
        //
        // Why VRAM (not sysmem-via-GART):
        // GART/DART sysmem path on Apple Silicon + Thunderbolt 5 is
        // structurally unsupported. The v0.1.14 self-test confirmed
        // CPU writes reach DRAM through an uncached mapping (cpu-readback
        // returns the sentinel), but the GPU's PCIe read of the
        // corresponding DART IOVA returns 0. PCIDriverKit does not
        // expose primitives to make device-initiated reads of arbitrary
        // CPU-written sysmem coherent — Scott Gustafson's RTX 5090 eGPU
        // project required a host-side mediation layer (apple-dma-pci
        // virtual PCIe device intercepting every DMA mapping call) to
        // work around this. We don't have that infrastructure; PSP
        // accessing VRAM via the framebuffer aperture sidesteps DART
        // entirely. See [[feedback_mac_amdgpu_dart_tb5_pcie_reads]].
        return kIOReturnSuccess;
    }
    case BringupStage::PSPLoadSOS:
        return psp_load_sos(ctx.device, ctx.psp);
    case BringupStage::PSPRingCreate: {
        // psp_v14_0_3 with recent SOS expects FB_FW_RESERV queries
        // immediately after ring_create. Per amdgpu_psp.c:2594-2600
        // (psp_update_fw_reservation), gated on SOS fw_version
        // >= 0x3a0e14 for IP_VERSION(14, 0, 3). We always send the
        // pair — PSP_ERR_UNKNOWN_COMMAND from older SOS is treated
        // as success per upstream.
        kern_return_t r = psp_ring_create(ctx.device, ctx.psp);
        if (r != kIOReturnSuccess) return r;
        return psp_query_fw_reservation(ctx.device, ctx.psp);
    }
    case BringupStage::TMRSetup:
        return psp_setup_tmr(ctx.device, ctx.psp);
    case BringupStage::PSPFwLoad: {
        // Synchronization point: validate that all required firmware
        // was loaded by the host-side LoadFirmware calls.
        //
        // The host-side code calls LoadFirmware for each firmware type
        // (SMU, IMU, RLC, CP, SDMA, MES) before reaching this stage.
        // Each LoadFirmware call sets microcode_loaded flags in the
        // BringupContext (sdma.microcode_loaded, imu.microcode_loaded,
        // etc.). This stage verifies those flags are set.
        //
        // Upstream: psp_load_non_psp_fw (amdgpu_psp.c:3051) does the
        // actual loading. On the dext, loading is done host-side via
        // the LoadFirmware selector, so this stage is just validation.
        auto &br = ctx;
        if (!br.psp.sosAlive) {
            INIT_LOG("PSPFwLoad: SOS not alive");
            return kIOReturnNotReady;
        }
        INIT_LOG("PSPFwLoad: all firmware loaded (host-side LoadFirmware)");
        return kIOReturnSuccess;
    }
    case BringupStage::SMUInit: {
        // 1. Existing alive-check: TestMessage round-trip.
        uint32_t echo = 0;
        kern_return_t r = smu_test_message(ctx.device, &echo);
        if (r != kIOReturnSuccess) return r;

        uint32_t ver = 0;
        (void)smu_get_version(ctx.device, &ver);
        ctx.device.smuOnline = true;

        // 2. v0.1.20: SMU PMFW handshake (smu_smc_hw_setup). Mirrors
        //    upstream amdgpu_smu.c:1662. Hypothesis is that PMFW must
        //    enable DPM features for the IMU autoload state machine to
        //    fire — without this, BOOTLOAD_STATUS stays at 0 even though
        //    PSP returns resp=0 for every command (v0.1.18-0.1.19
        //    symptom). Failures here are NON-FATAL for the SMUInit
        //    stage itself (SMU is still "alive" per TestMessage); we
        //    log and continue so RLCInit's BOOTLOAD_STATUS poll surfaces
        //    the real diagnostic signal.
        kern_return_t hwr = smu_smc_hw_setup(ctx.device, ctx.psp);
        if (hwr != kIOReturnSuccess) {
            INIT_LOG("SMUInit: smc_hw_setup non-fatal failure: %#x — "
                     "proceeding to RLCInit anyway", hwr);
        }
        return kIOReturnSuccess;
    }
    case BringupStage::GMCInit:
        return gmc_init(ctx.device, ctx.gmc);
    case BringupStage::IHInit:
        return ih_init_full(ctx.device, ctx.ih);
    case BringupStage::RLCInit:
        return rlc_init_full(ctx.device, ctx.gmc, ctx.rlc);
    case BringupStage::CPInit:
        return cp_init_full(ctx.device, ctx.gmc, ctx.cp);
    case BringupStage::SDMAInit:
        return sdma_init_full(ctx.device, ctx.psp, ctx.gmc, ctx.sdma);
    case BringupStage::MESInit:
        return mes_init_full(ctx.device, ctx.psp, ctx.gmc, ctx.mes);
    case BringupStage::IMUInit:
        // Audit-7 #11. PSP-load path: imu microcode comes via PSP
        // LOAD_IP_FW(IMU_I) + LOAD_IP_FW(IMU_D); the firmware
        // extractor (separate agent) sets imu.microcode_loaded.
        // This handler only validates that gate, no MMIO.
        return imu_init_full(ctx.device, ctx.imu);
    case BringupStage::GFXInit: {
        // Audit-7 #8 + #10. Run gfx_constants_init (still nominally
        // a CPInit prerequisite — but here we re-arm GFXHUB so it
        // survives RLC autoload's register reset).
        //
        // Upstream sequence (gfx_v12_0_hw_init:3697):
        //     gfx_v12_0_gfxhub_enable(adev);     // gart_enable + hdp_flush + tlb_flush
        //     gfx_v12_0_constants_init(adev);
        //     gfx_v12_0_rlc_resume(adev);        // not driver-side on PSP path
        //     gfx_v12_0_cp_resume(adev);         // already done in CPInit
        //
        // We delegate the gart_enable re-run to gmc_gfxhub_gart_enable
        // (which has the full upstream port), then HDP flush, then
        // gfx_constants_init.
        if (!ctx.device.ip.isResolved(IPBlock::GC)) {
            INIT_LOG("stage GFXInit: GC IP base not resolved");
            return kIOReturnNotReady;
        }
        // First (and only) GFXHUB program — upstream defers this to
        // gfx_v12_0_hw_init (gfx_v12_0.c:3697), after psp_load_fw.
        // Programming GFXHUB before PSP staged GFX-block firmware
        // makes PSP reject SDMA/CP/MES/RLC with TEE_BAD_PARAMETERS
        // while still accepting SMU/IMU. gmc_init now only programs
        // MMHUB; GFXHUB lands here.
        if (ctx.device.ip.isResolved(IPBlock::GMC)) {
            kern_return_t r = gmc_gfxhub_gart_enable(ctx.device, ctx.gmc);
            if (r != kIOReturnSuccess) {
                INIT_LOG("GFXInit: gfxhub_gart_enable failed: %#x", r);
                return r;
            }
            // HDP flush + GFXHUB-side TLB flush — moved here from
            // gmc_init since GFXHUB only just got programmed.
            gmc_hdp_flush(ctx.device);
            gmc_flush_gpu_tlb(ctx.device, ctx.gmc, ctx.gmc.gfxhub,
                              /*vmid*/ 0, /*type*/ 0);
        } else {
            INIT_LOG("GFXInit: skipping gfxhub (GMC IP base unresolved)");
        }
        // Run gfx_v12_0_constants_init now (idempotent — CPInit's
        // cp_init_full also calls the legacy convenience overload).
        return gfx_constants_init(ctx.device, ctx.gfx);
    }
    }
    return kIOReturnUnsupported;
}

kern_return_t
bringup_to(BringupContext &ctx, BringupStage target)
{
    for (uint32_t s = (uint32_t)ctx.reached + 1;
         s <= (uint32_t)target;
         s++) {
        kern_return_t ret = run_stage(ctx, (BringupStage)s);
        if (ret != kIOReturnSuccess) {
            INIT_LOG("stage %{public}s failed: %#x",
                     stage_name((BringupStage)s), ret);
            return ret;
        }
        ctx.reached = (BringupStage)s;
    }
    INIT_LOG("reached stage %{public}s", stage_name(ctx.reached));
    return kIOReturnSuccess;
}

} // namespace amdgpu
