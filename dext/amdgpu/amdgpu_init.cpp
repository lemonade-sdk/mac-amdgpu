//
//  amdgpu_init.cpp — Phase 1B bringup orchestrator.
//
//  Each stage gates the next. The orchestrator is a switch ladder
//  rather than a table-of-fns so that newly-added stages with
//  per-stage helper signatures aren't forced into a one-size-fits-all
//  shape.
//

#include <os/log.h>
#include <new>
#include "amdgpu_init.h"

#define INIT_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.init: " fmt, ##__VA_ARGS__)

namespace amdgpu {

void bringup_release_resources(BringupContext &ctx)
{
    smu_metrics_invalidate(ctx.metrics, kIOReturnNotReady);
    // PCI Close disables bus mastering before any DMA mapping is released.
    // Do not try to halt engines here: the device may already be unplugged.
    auto release_dma = [](IOBufferMemoryDescriptor *&buffer,
                          IODMACommand *&dma) {
        if (dma) {
            dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            dma->release();
            dma = nullptr;
        }
        if (buffer) {
            buffer->release();
            buffer = nullptr;
        }
    };
    for (auto &pipe : ctx.mes.pipe) {
        release_dma(pipe.eop_buf, pipe.eop_dma);
        release_dma(pipe.mqd_buf, pipe.mqd_dma);
        release_dma(pipe.ring_buf, pipe.ring_dma);
        release_dma(pipe.cmd_buf, pipe.cmd_dma);
        release_dma(pipe.wb_buf, pipe.wb_dma);
        release_dma(pipe.resource_1_buf, pipe.resource_1_dma);
        release_dma(pipe.sch_ctx_buf, pipe.sch_ctx_dma);
        release_dma(pipe.status_fence_buf, pipe.status_fence_dma);
    }
    // SDMA ring/write-back storage belongs to the GMC VRAM arena.
    cp_release_storage(ctx.cp);
    memory_transfer_release_after_reset(ctx.memoryTest);
    ih_release(ctx.ih);
    psp_release(ctx.psp);
    release_dma(ctx.psp.ringBuffer, ctx.psp.ringDMACommand);
    release_dma(ctx.psp.ringBinding.sysmemBuffer, ctx.psp.ringBinding.dmaCommand);
    release_dma(ctx.psp.cmdBinding.sysmemBuffer, ctx.psp.cmdBinding.dmaCommand);
    release_dma(ctx.psp.fenceBinding.sysmemBuffer, ctx.psp.fenceBinding.dmaCommand);
    gmc_release_resources(ctx.gmc);

    // Firmware views and VRAM allocations are borrowed pointers/offsets;
    // clearing them also removes stale stage and initialized flags.
    // Reinitialize in place: assignment from {} would put the enlarged VRAM
    // metadata arena in a large temporary on the lifecycle queue's stack.
    static_assert(__is_trivially_destructible(BringupContext));
    ctx.~BringupContext();
    new (&ctx) BringupContext{};
    INIT_LOG("released shared bringup resources after PCI/client teardown");
}

//============================================================
// NBIF v6.3.1 port — drivers/gpu/drm/amd/amdgpu/nbif_v6_3_1.c.
//
// Our R9700 (DID 0x7551 rev C0) reports NBIF v6.3.1 in discovery
// (hw_id=108, major/minor/rev=6.3.1). nbif_v6_3_1.c's funcs table is
// the one upstream wires into adev->nbio.funcs for this chip — NOT
// nbio_v7_11. The two have completely different doorbell-routing
// models: NBIO 7.11 uses regGDC0_BIF_CSDMA_DOORBELL_RANGE (PCIE_PORT
// indirect at BASE_IDX 3); NBIF 6.3.1 uses S2A (Slave-to-AXI) doorbell
// entries with explicit AWID/AWADDR fields, accessed via direct MMIO
// at BASE_IDX 2.
//
// Functions ported below mirror the nbio_funcs struct in
// nbif_v6_3_1.c:510. Each function has an IP_VERSION(7, 11, 4) branch
// (the "nbif_4_10" register variant at BASE_IDX 3) and a base branch
// (BASE_IDX 2). We use NBIORegs::IsIPVersion_7_11_4 — a runtime helper
// driven by the discovered IP version — to pick.
//
// Skipped from the upstream funcs table (not needed for our path
// today; each line is the reason):
//   .program_aspm    — TB5 link is fixed by host; we don't tune ASPM.
//   .program_ltr     — LTR negotiation is a host-bridge concern on AS.
//   .get_rom_offset  — VBIOS already parsed by host before bringup.
//   ras_err_event_*  — no IH/RAS handler chain wired up yet.
//============================================================

// Returns true if the chip is IP_VERSION(7, 11, 4) — the NBIO variant
// that uses the _nbif_4_10 register addresses (at BASE_IDX 3 via
// PCIE_PORT indirect). Other NBIF/NBIO versions use the base S2A
// doorbell entry registers (BASE_IDX 2, direct MMIO).
//
// Discovery populates dev.ip.version[NBIO] from the on-die binary, so
// this is a real runtime check now. Our R9700 reports NBIF 6.3.1 (the
// 6.3.1 driver family also handles 7.11.0-3); only 7.11.4 takes the
// alternate register path. [[feedback_mac_amdgpu_per_ip_version_offsets]]
static inline bool nbif_is_ip_7_11_4(const DeviceContext &dev)
{
    return dev.ip.isVersion(IPBlock::NBIO, 7, 11, 4);
}

//------------------------------------------------------------------
// nbif_v6_3_1_remap_hdp_registers — nbif_v6_3_1.c:60.
//
// Programs the two BIF_BX0_REMAP_HDP_*_FLUSH_CNTL registers so that
// writes to the rmmio_remap.reg_offset window (default 0x44000 inside
// BAR5) trigger an HDP flush. On NBIF 6.3.1 these regs live at offset
// 0x012d/0x012e BASE_IDX 2 — within Apple's 2 MB BAR5 mapping, direct
// MMIO. (NBIO 7.11 puts the same logical regs at 0x8E4D/0x8E4E
// BASE_IDX 5, which on AS requires SMN indirect access.)
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_remap_hdp_registers(DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/2)) {
        INIT_LOG("remap_hdp: NBIO BASE_IDX 2 unresolved");
        return kIOReturnNotReady;
    }

    constexpr uint32_t kRMMIORemapBase = 0x44000;
    constexpr uint32_t kHDPMemFlush    = 0x0;
    constexpr uint32_t kHDPRegFlush    = 0x4;

    const uint32_t mem_reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, NBIORegs::BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL);
    const uint32_t reg_reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, NBIORegs::BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL);

    WREG32(dev, mem_reg, kRMMIORemapBase + kHDPMemFlush);
    WREG32(dev, reg_reg, kRMMIORemapBase + kHDPRegFlush);
    INIT_LOG("remap_hdp: MEM_FLUSH @%#x = %#x, REG_FLUSH @%#x = %#x "
             "(BASE_IDX 2, direct MMIO)",
             mem_reg, kRMMIORemapBase + kHDPMemFlush,
             reg_reg, kRMMIORemapBase + kHDPRegFlush);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// nbif_v6_3_1_init_registers — nbif_v6_3_1.c:355.
//
// Clears RCC_DEV0_EPF2_STRAP2.STRAP_NO_SOFT_RESET_DEV0_F2 so the F2
// endpoint can take soft-reset signals. Without it, RAS-triggered
// resets get silently masked. BASE_IDX 2, direct MMIO.
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_init_registers(DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/2)) {
        INIT_LOG("nbif_init_registers: NBIO BASE_IDX 2 unresolved");
        return kIOReturnNotReady;
    }
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, NBIORegs::RCC_DEV0_EPF2_STRAP2);
    const uint32_t old_val = RREG32(dev, reg);
    const uint32_t new_val =
        old_val & ~NBIORegs::kSTRAP_NO_SOFT_RESET_DEV0_F2_MASK;
    if (old_val != new_val) {
        WREG32(dev, reg, new_val);
    }
    INIT_LOG("nbif_init_registers: RCC_DEV0_EPF2_STRAP2 @%#x %#x -> %#x "
             "(cleared NO_SOFT_RESET_F2)", reg, old_val, new_val);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// nbif_v6_3_1_ih_control — nbif_v6_3_1.c:272.
//
// IH dummy-page + interrupt config. NBIF 6.3.1 uses BIF_BX0_INTERRUPT_
// CNTL/_CNTL2 (BASE_IDX 2). Same offsets as the NBIO 7.11 BX1 variant
// — only the BX0 vs BX1 naming differs in the header.
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_ih_control(DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/2)) {
        INIT_LOG("nbif_ih_control: NBIO BASE_IDX 2 unresolved");
        return kIOReturnNotReady;
    }
    const uint32_t cntl2_reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, NBIORegs::INTERRUPT_CNTL2);
    const uint32_t cntl_reg  = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, NBIORegs::INTERRUPT_CNTL);

    // No dummy page yet — write 0.
    WREG32(dev, cntl2_reg, 0);
    const uint32_t cntl_old = RREG32(dev, cntl_reg);
    uint32_t cntl_new = cntl_old
        & ~NBIORegs::kINTERRUPT_CNTL_DUMMY_RD_OVERRIDE_MASK
        & ~NBIORegs::kINTERRUPT_CNTL_REQ_NONSNOOP_EN_MASK;
    WREG32(dev, cntl_reg, cntl_new);
    INIT_LOG("nbif_ih_control: INTERRUPT_CNTL2@%#x=0, INTERRUPT_CNTL@%#x "
             "%#x -> %#x", cntl2_reg, cntl_reg, cntl_old, cntl_new);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// nbif_v6_3_1_enable_doorbell_aperture — nbif_v6_3_1.c:203.
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_enable_doorbell_aperture(DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/2)) {
        INIT_LOG("doorbell_aperture: NBIO BASE_IDX 2 unresolved");
        return kIOReturnNotReady;
    }
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, NBIORegs::RCC_DOORBELL_APER_EN);
    uint32_t v = RREG32(dev, reg);
    if (enable) {
        v |= NBIORegs::kRCC_DOORBELL_APER_EN_BIT;
    } else {
        v &= ~NBIORegs::kRCC_DOORBELL_APER_EN_BIT;
    }
    WREG32(dev, reg, v);
    INIT_LOG("nbif_doorbell_aperture: APER_EN@%#x = %#x (enable=%d)",
             reg, v, enable ? 1 : 0);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// nbif_v6_3_1_enable_doorbell_selfring_aperture — nbif_v6_3_1.c:210.
//
// Programs the SELFRING aperture BASE/CNTL so the GPU's internal
// blocks can target the BAR2 doorbell window via their own GPA
// (Guest Physical Address) path. soc24_common_hw_init calls this
// BEFORE the regular doorbell aperture. Skipping it leaves the
// engine-side path gated even when APER_EN is set.
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_enable_doorbell_selfring_aperture(DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::NBIO, /*baseIdx=*/2)) {
        INIT_LOG("nbif_selfring_aperture: NBIO BASE_IDX 2 unresolved");
        return kIOReturnNotReady;
    }
    const uint32_t cntl_reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2,
        NBIORegs::BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL);
    const uint32_t low_reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2,
        NBIORegs::BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW);
    const uint32_t hi_reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2,
        NBIORegs::BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH);

    uint32_t tmp = 0;
    if (enable) {
        tmp |= (1u << NBIORegs::kDOORBELL_SELFRING_GPA_APER_EN_SHIFT)
                & NBIORegs::kDOORBELL_SELFRING_GPA_APER_EN_MASK;
        tmp |= (1u << NBIORegs::kDOORBELL_SELFRING_GPA_APER_MODE_SHIFT)
                & NBIORegs::kDOORBELL_SELFRING_GPA_APER_MODE_MASK;
        tmp |= (0u << NBIORegs::kDOORBELL_SELFRING_GPA_APER_SIZE_SHIFT)
                & NBIORegs::kDOORBELL_SELFRING_GPA_APER_SIZE_MASK;

        const uint32_t base_lo =
            static_cast<uint32_t>(dev.doorbell.base & 0xFFFFFFFFull);
        const uint32_t base_hi =
            static_cast<uint32_t>((dev.doorbell.base >> 32) & 0xFFFFFFFFull);
        WREG32(dev, low_reg, base_lo);
        WREG32(dev, hi_reg,  base_hi);
        INIT_LOG("nbif_selfring: BASE = %#010x:%#010x (doorbell.base=%#llx)",
                 base_hi, base_lo, (unsigned long long)dev.doorbell.base);
    }
    WREG32(dev, cntl_reg, tmp);
    INIT_LOG("nbif_selfring: CNTL @%#x = %#x (EN=%d MODE=1 SIZE=0)",
             cntl_reg, tmp, enable ? 1 : 0);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// Helper: write a single S2A_DOORBELL_ENTRY_n_CTRL register. Picks
// the base or _nbif_4_10 variant based on IP version. The _nbif_4_10
// path lives at BASE_IDX 3 (outside Apple's BAR5) and needs PCIE_PORT
// indirect; the base path is direct MMIO at BASE_IDX 2.
//------------------------------------------------------------------
static uint32_t
nbif_read_s2a_entry(DeviceContext &dev, int entry)
{
    static const uint32_t base_offsets[6] = {
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_1_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_4_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_5_CTRL,
    };
    static const uint32_t nbif_4_10_offsets[6] = {
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_1_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_4_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_5_CTRL_nbif_4_10,
    };
    if (entry < 0 || entry >= 6) return 0;

    if (nbif_is_ip_7_11_4(dev)) {
        const uint32_t reg = SOC15_REG_OFFSET_BIDX(
            dev, IPBlock::NBIO, 3, nbif_4_10_offsets[entry]);
        return PCIE_PORT_RREG32(dev, reg);
    }
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, base_offsets[entry]);
    return RREG32(dev, reg);
}

static void
nbif_write_s2a_entry(DeviceContext &dev, int entry, uint32_t value)
{
    static const uint32_t base_offsets[6] = {
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_1_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_4_CTRL,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_5_CTRL,
    };
    static const uint32_t nbif_4_10_offsets[6] = {
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_1_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_4_CTRL_nbif_4_10,
        NBIORegs::GDC_S2A0_S2A_DOORBELL_ENTRY_5_CTRL_nbif_4_10,
    };
    if (entry < 0 || entry >= 6) return;

    if (nbif_is_ip_7_11_4(dev)) {
        const uint32_t reg = SOC15_REG_OFFSET_BIDX(
            dev, IPBlock::NBIO, 3, nbif_4_10_offsets[entry]);
        PCIE_PORT_WREG32(dev, reg, value);
        return;
    }
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(
        dev, IPBlock::NBIO, 2, base_offsets[entry]);
    WREG32(dev, reg, value);
}

// Helper: bake an S2A_DOORBELL_PORTn_* field set into a u32. Identical
// layout across all eight ports, so we just use the shared field
// shifts/masks.
static inline uint32_t
nbif_build_s2a(uint32_t cur, bool enable, uint32_t awid,
               uint32_t range_offset, uint32_t range_size,
               uint32_t awaddr_31_28)
{
    uint32_t v = cur;
    v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_ENABLE_MASK)
        | ((enable ? 1u : 0u) << NBIORegs::kS2A_DOORBELL_PORT_ENABLE_SHIFT);
    v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_AWID_MASK)
        | ((awid << NBIORegs::kS2A_DOORBELL_PORT_AWID_SHIFT)
           & NBIORegs::kS2A_DOORBELL_PORT_AWID_MASK);
    v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_RANGE_OFFSET_MASK)
        | ((range_offset << NBIORegs::kS2A_DOORBELL_PORT_RANGE_OFFSET_SHIFT)
           & NBIORegs::kS2A_DOORBELL_PORT_RANGE_OFFSET_MASK);
    v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_RANGE_SIZE_MASK)
        | ((range_size << NBIORegs::kS2A_DOORBELL_PORT_RANGE_SIZE_SHIFT)
           & NBIORegs::kS2A_DOORBELL_PORT_RANGE_SIZE_MASK);
    v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_AWADDR_31_28_VALUE_MASK)
        | ((awaddr_31_28 << NBIORegs::kS2A_DOORBELL_PORT_AWADDR_31_28_VALUE_SHIFT)
           & NBIORegs::kS2A_DOORBELL_PORT_AWADDR_31_28_VALUE_MASK);
    return v;
}

//------------------------------------------------------------------
// nbif_v6_3_1_sdma_doorbell_range — nbif_v6_3_1.c:98.
//
// Programs S2A_DOORBELL_ENTRY_2 (SDMA) with PORT2_AWID=0xe and
// PORT2_AWADDR_31_28_VALUE=0x3 — the AXI ID + upper address bits the
// SDMA cluster listens for. Only `instance == 0` actually writes the
// register (the ENTRY covers all SDMA queues via RANGE_OFFSET/SIZE).
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_sdma_doorbell_range(DeviceContext &dev,
                                int instance,
                                bool use_doorbell,
                                int doorbell_index,
                                int doorbell_size)
{
    if (instance != 0) return kIOReturnSuccess;

    uint32_t v = nbif_read_s2a_entry(dev, /*entry=*/2);
    const uint32_t old_value = v;

    if (use_doorbell) {
        v = nbif_build_s2a(v, /*enable=*/true, /*awid=*/0xe,
                           static_cast<uint32_t>(doorbell_index),
                           static_cast<uint32_t>(doorbell_size),
                           /*awaddr_31_28=*/0x3);
    } else {
        v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_RANGE_SIZE_MASK);
    }

    nbif_write_s2a_entry(dev, /*entry=*/2, v);

    INIT_LOG("nbif_sdma_doorbell_range[%d]: ENTRY_2_CTRL "
             "%#x -> %#x  (use=%d AWID=0xe AWADDR=0x3 OFFSET=%#x SIZE=%d)",
             instance, old_value, v,
             use_doorbell ? 1 : 0, doorbell_index, doorbell_size);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// nbif_v6_3_1_ih_doorbell_range — nbif_v6_3_1.c:233.
//
// Programs S2A_DOORBELL_ENTRY_1 (IH) with PORT1_AWID=0x0,
// PORT1_AWADDR=0x0, PORT1_RANGE_SIZE=2.
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_ih_doorbell_range(DeviceContext &dev, bool use_doorbell,
                              int doorbell_index)
{
    uint32_t v = nbif_read_s2a_entry(dev, /*entry=*/1);
    const uint32_t old_value = v;

    if (use_doorbell) {
        v = nbif_build_s2a(v, /*enable=*/true, /*awid=*/0x0,
                           static_cast<uint32_t>(doorbell_index),
                           /*range_size=*/2,
                           /*awaddr_31_28=*/0x0);
    } else {
        v = (v & ~NBIORegs::kS2A_DOORBELL_PORT_RANGE_SIZE_MASK);
    }

    nbif_write_s2a_entry(dev, /*entry=*/1, v);
    INIT_LOG("nbif_ih_doorbell_range: ENTRY_1_CTRL %#x -> %#x "
             "(use=%d AWID=0x0 OFFSET=%#x SIZE=2)",
             old_value, v, use_doorbell ? 1 : 0, doorbell_index);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// nbif_v6_3_1_gc_doorbell_init — nbif_v6_3_1.c:192.
//
// Writes magic constants to S2A_DOORBELL_ENTRY_0 (GFX/HQD) and
// ENTRY_3 (MES/compute fence). Values are the upstream-baked ENABLE +
// AWID + RANGE + AWADDR combo for these engines:
//
//   ENTRY_0 = 0x30000007  →  AWADDR=0x3, DROP_EN=0, NEED_DEDUCT=0,
//                            64BIT_SUPPORT_DIS=0, RANGE_SIZE=0,
//                            RANGE_OFFSET=0, FENCE=0, AWID=0x3, EN=1
//   ENTRY_3 = 0x3000000d  →  AWADDR=0x3, AWID=0x6, EN=1, others 0
//
// These two ENTRIES define the routing for the GFX ring and the
// compute/MES doorbells. Without them, CP doorbells go nowhere even
// after enable_doorbell_aperture(true).
//------------------------------------------------------------------
static kern_return_t
nbif_v6_3_1_gc_doorbell_init(DeviceContext &dev)
{
    nbif_write_s2a_entry(dev, /*entry=*/0, 0x30000007u);
    nbif_write_s2a_entry(dev, /*entry=*/3, 0x3000000Du);
    INIT_LOG("nbif_gc_doorbell_init: ENTRY_0=0x30000007 ENTRY_3=0x3000000d "
             "(GFX/HQD + MES routing baked in)");
    return kIOReturnSuccess;
}

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
    // doorbell_init populates dev.doorbell.index.sdma_engine[], gfx_ring0,
    // etc. with the ASIC-specific doorbell indices. SDMA and the NBIO
    // routing-range programming below both read from those — without this
    // call they all read 0 (IIG IONewZero skips C++ ctors, so the
    // DoorbellIndex member's in-class default initializers never run).
    //
    // v0.1.33: previously doorbell_init was defined but never invoked,
    // which meant every doorbell-index lookup returned 0 and the engine
    // was being programmed to listen at doorbell 0 while we were writing
    // to BAR2 offset (0 * 8) — accidentally matching, but with no NBIO
    // routing the writes still went into a black hole.
    {
        kern_return_t dr = doorbell_init(ctx.device, ctx.device.doorbell);
        if (dr != kIOReturnSuccess) {
            INIT_LOG("doorbell_init failed: %#x — continuing but doorbells "
                     "will be misrouted", dr);
        }
    }

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

    // ---- NBIF v6.3.1 bringup sequence -----------------------------
    // Mirrors upstream's actual ordering across soc24_common_hw_init
    // (soc24.c:466) and soc24_common_late_init (soc24.c:436):
    //
    //   hw_init:
    //     1. init_registers          (RCC_DEV0_EPF2_STRAP2)
    //     2. remap_hdp_registers
    //     3. enable_doorbell_aperture(true)
    //
    //   late_init (after GMC sw_init, in case BAR2 resize moves the
    //   doorbell aperture):
    //     4. enable_doorbell_selfring_aperture(true)
    //
    //   Additional NBIF setup that lives outside soc24:
    //     5. ih_control               (BX0 INTERRUPT_CNTL clears, called
    //                                  from amdgpu_irq.c during IH init)
    //     6. gc_doorbell_init         (S2A ENTRY_0 + ENTRY_3, called from
    //                                  gfx_v12_0_late_init)
    //     7. sdma_doorbell_range      (S2A ENTRY_2, called from
    //                                  sdma_v7_0_gfx_resume)
    //
    // We run all of this here at IPDiscovery time since we don't run a
    // separate late_init phase — doorbell.base is already valid (BAR2
    // PCIe addr captured in MacAMDGPU::ensure_open) and we don't resize
    // BAR2 on AS, so the "late" reasoning doesn't apply.
    //
    // Order matters: SELFRING goes AFTER the doorbell aperture is up,
    // per the upstream late_init comment "Enable selfring doorbell
    // aperture late because doorbell BAR aperture will change if resize
    // BAR successfully in gmc sw_init" (soc24.c:448).
    //
    // ih_doorbell_range (S2A ENTRY_1) is deferred — we don't process IH
    // events yet; will wire it in alongside ih_v7_0_irq_init.
    (void)nbif_v6_3_1_init_registers(ctx.device);
    (void)nbif_v6_3_1_remap_hdp_registers(ctx.device);
    (void)nbif_v6_3_1_enable_doorbell_aperture(ctx.device, /*enable=*/true);
    (void)nbif_v6_3_1_ih_control(ctx.device);
    (void)nbif_v6_3_1_gc_doorbell_init(ctx.device);
    {
        constexpr int kSdmaInstances = 2;
        constexpr int kSdmaDoorbellRange = 20;
        const int low_doorbell_index = static_cast<int>(
            ctx.device.doorbell.index.sdma_engine[0] << 1);
        const int total_size = kSdmaDoorbellRange * kSdmaInstances;
        (void)nbif_v6_3_1_sdma_doorbell_range(
            ctx.device, /*instance=*/0, /*use_doorbell=*/true,
            low_doorbell_index, total_size);
    }
    // SELFRING last — matches soc24_common_late_init.
    (void)nbif_v6_3_1_enable_doorbell_selfring_aperture(
        ctx.device, /*enable=*/true);

    // df_v4_15_hw_init: have_atomics_support=false on AS+TB5, no-op
    // (matches upstream non-atomics path).
    INIT_LOG("df_v4_15_hw_init: skipped (no PCIe atomics on AS/TB5)");

    // Legacy NBIO 7.11 init_registers compat block — programs PCIE_MST_
    // CTRL_3 + RCC_DEV0_EPF5_STRAP4 via SMN. nbif_v6_3_1 has its own
    // (smaller) init_registers above. These NBIO 7.11 writes at BASE_IDX 5
    // are no-ops on a true NBIF 6.3.1 chip (the addresses don't exist),
    // but the v0.1.0-0.1.22 bringup chain has empirically relied on them
    // — keep them gated by NBIO BASE_IDX 5 resolution so they're silently
    // skipped if discovery doesn't populate that slot.
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

    // db.base is set by MacAMDGPU::ensure_open from PCI config — preserve it
    // here. (Apple's PCIDriverKit hides the BAR2 host VA, but the PCIe bus
    // address comes from config space and is what NBIO SELFRING aperture
    // needs.) If config reads failed, base stays 0 and SELFRING falls back
    // to base=0 + EN bit only.
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
        if (!br.psp.firmwareLoadComplete || !br.rlc.microcode_loaded ||
            !br.imu.microcode_loaded || !br.sdma.microcode_loaded) {
            INIT_LOG("PSPFwLoad: firmware chain incomplete (including AUTOLOAD_RLC/ASD/REG_LIST)");
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

        // A successful ping proves communication, not completed setup.
        // Required SMU setup failures must stop initialization here.
        kern_return_t hwr = smu_smc_hw_setup(ctx.device, ctx.psp, &ctx.metrics);
        if (hwr != kIOReturnSuccess)
            INIT_LOG("SMUInit: required hardware setup failed: %#x", hwr);
        return hwr;
    }
    case BringupStage::GMCInit:
        return gmc_init(ctx.device, ctx.gmc);
    case BringupStage::IHInit:
        return ih_init_full(ctx.device, ctx.ih);
    case BringupStage::RLCInit:
        if (!ctx.rlc.microcode_loaded) return kIOReturnNotReady;
        return rlc_wait_for_autoload_complete(ctx.device, ctx.rlc);
    case BringupStage::SDMAInit:
        return sdma_init_full(ctx.device, ctx.psp, ctx.gmc, ctx.sdma);
    case BringupStage::MESInit: {
        const auto started = cp_start_engines(ctx.device, ctx.cp);
        if (started != kIOReturnSuccess) return started;
        cp_log_control(ctx.device, "before MES initialization");
        const auto r = mes_init_full(ctx.device, ctx.psp, ctx.gmc, ctx.mes);
        cp_log_control(ctx.device, "after MES initialization");
        return r;
    }
    case BringupStage::GFXInit:
        return cp_init_full(ctx.device, ctx.gmc, ctx.cp, ctx.mes);
    case BringupStage::IMUInit:
        // Audit-7 #11. PSP-load path: imu microcode comes via PSP
        // LOAD_IP_FW(IMU_I) + LOAD_IP_FW(IMU_D); the firmware
        // extractor (separate agent) sets imu.microcode_loaded.
        // This handler only validates that gate, no MMIO.
        return imu_init_full(ctx.device, ctx.imu);
    case BringupStage::CPInit: {
        // Linux gfx_v12_0_hw_init orders RS64 setup -> GFXHUB -> constants
        // before cp_resume. Async resume enables engines, bootstraps MES KIQ,
        // then maps the GFX queue. Preserve the public stage numbers.
        kern_return_t prepared = cp_prepare_firmware(ctx.device, ctx.gmc, ctx.cp);
        if (prepared != kIOReturnSuccess) return prepared;
        if (!ctx.device.ip.isResolved(IPBlock::GC)) {
            INIT_LOG("CP preparation: GC IP base not resolved");
            return kIOReturnNotReady;
        }
        // First (and only) GFXHUB program — upstream defers this to
        // gfx_v12_0_hw_init (gfx_v12_0.c:3697), after psp_load_fw.
        // Programming GFXHUB before PSP staged GFX-block firmware
        // makes PSP reject SDMA/CP/MES/RLC with TEE_BAD_PARAMETERS
        // while still accepting SMU/IMU. gmc_init now only programs
        // MMHUB; GFXHUB lands here, after RLC and before MES/CP queue resume.
        if (ctx.device.ip.isResolved(ctx.gmc.gfxhub.ip)) {
            kern_return_t r = gmc_gfxhub_gart_enable(ctx.device, ctx.gmc);
            if (r != kIOReturnSuccess) {
                INIT_LOG("CP preparation: gfxhub_gart_enable failed: %#x", r);
                return r;
            }
            // HDP flush + GFXHUB-side TLB flush — moved here from
            // gmc_init since GFXHUB only just got programmed.
            r = gmc_hdp_flush(ctx.device);
            if (r != kIOReturnSuccess) return r;
            r = gmc_flush_gpu_tlb(ctx.device, ctx.gmc, ctx.gmc.gfxhub,
                                  /*vmid*/ 0, /*type*/ 0);
            if (r != kIOReturnSuccess) return r;

            // GART page table / aperture are programmed — now populate
            // the GARTContext so the BO allocator can hand out GTT BOs
            // (gated until host-memory transfers and general binding
            // lifetime/reclamation are validated).
            kern_return_t gr = gart_init(ctx.device, ctx.gmc, ctx.gart);
            if (gr != kIOReturnSuccess) {
                INIT_LOG("CP preparation: gart_init failed: %#x — GTT BOs "
                         "will return kIOReturnNotReady", gr);
                // Non-fatal: VRAM BOs still work, only GTT path lost.
            }
        } else {
            INIT_LOG("CP preparation: GFXHUB register block unresolved");
            return kIOReturnNotReady;
        }
        // Linux enables RLC CSB/SRM only after GFXHUB and constants.
        kern_return_t constants = gfx_constants_init(ctx.device, ctx.gfx);
        if (constants != kIOReturnSuccess) return constants;
        return rlc_init_full(ctx.device, ctx.gmc, ctx.rlc);
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
