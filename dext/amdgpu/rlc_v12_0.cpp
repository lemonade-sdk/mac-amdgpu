//
//  rlc_v12_0.cpp — RLC clear-state alloc + autoload wait for GFX12.
//
//  Sources:
//    drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c:amdgpu_gfx_rlc_init_csb
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_wait_for_rlc_autoload_complete
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_get_csb_size
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_get_csb_buffer
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_init_csb
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_rlc_enable_srm
//    drivers/gpu/drm/amd/amdgpu/clearstate_gfx12.h (vendored below).
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include "amdgpu_rlc.h"
#include "amdgpu_gmc.h"   // GMCContext / VRAMBumpAllocator

#define RLC_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.rlc: " fmt, ##__VA_ARGS__)

namespace amdgpu {

//------------------------------------------------------------------
// Vendored gfx12_cs_data — direct port of upstream
// drivers/gpu/drm/amd/amdgpu/clearstate_gfx12.h:26-119.
//
// All initial values are 0 (matching upstream); we keep the named
// arrays separate so the size + register-index math matches
// upstream's get_csb_size / get_csb_buffer behaviour exactly.
//------------------------------------------------------------------
namespace {

// clearstate_gfx12.h:26-61  — 34 entries (mmPA_SC_VPORT_*).
const uint32_t kGfx12_SECT_CONTEXT_def_1[] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0
};
// clearstate_gfx12.h:63-66  — 2 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_2[] = { 0, 0 };
// clearstate_gfx12.h:68-70  — 1 entry.
const uint32_t kGfx12_SECT_CONTEXT_def_3[] = { 0 };
// clearstate_gfx12.h:72-79  — 6 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_4[] = { 0, 0, 0, 0, 0, 0 };
// clearstate_gfx12.h:81-93  — 11 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_5[] = { 0,0,0,0,0,0,0,0,0,0,0 };
// clearstate_gfx12.h:95-104 — 8 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_6[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// clearstate_gfx12.h:106-114 — extent table. {extent, reg_index, reg_count}.
const CSExtentDef kGfx12_SECT_CONTEXT_defs[] = {
    { kGfx12_SECT_CONTEXT_def_1, 0x0000a03e, 34 },
    { kGfx12_SECT_CONTEXT_def_2, 0x0000a0cc,  2 },
    { kGfx12_SECT_CONTEXT_def_3, 0x0000a0d8,  1 },
    { kGfx12_SECT_CONTEXT_def_4, 0x0000a0db,  6 },
    { kGfx12_SECT_CONTEXT_def_5, 0x0000a2e5, 11 },
    { kGfx12_SECT_CONTEXT_def_6, 0x0000a3c0,  8 },
    { nullptr,                   0,           0 }   // terminator
};

// clearstate_gfx12.h:116-119 — section table.
const CSSectionDef kGfx12_cs_data[] = {
    { kGfx12_SECT_CONTEXT_defs, kCSSection_CONTEXT },
    { nullptr,                  kCSSection_NONE    }
};

} // anonymous namespace

//------------------------------------------------------------------
// rlc_get_csb_size — port of gfx_v12_0_get_csb_size (gfx_v12_0.c:669).
//
// Counts dwords needed to encode the full clear-state buffer:
//   1 (clustercount header)
// + per extent: 2 (reg_count + reg_index) + reg_count * dword
//------------------------------------------------------------------
static uint32_t
rlc_get_csb_size(const CSSectionDef *cs_data)
{
    uint32_t count = 1;
    for (const CSSectionDef *sect = cs_data; sect->section != nullptr; ++sect) {
        if (sect->id == kCSSection_CONTEXT) {
            for (const CSExtentDef *ext = sect->section;
                 ext->extent != nullptr; ++ext) {
                count += 2 + ext->reg_count;
            }
        } else {
            return 0;
        }
    }
    return count;
}

//------------------------------------------------------------------
// rlc_get_csb_buffer — port of gfx_v12_0_get_csb_buffer
// (gfx_v12_0.c:688). Fills `buffer` with the CSB layout RLC expects:
//
//     buffer[0]            = clustercount
//     for each extent:
//         buffer[count++]  = reg_count
//         buffer[count++]  = reg_index
//         buffer[count..]  = extent initial values
//
// `buffer` must point to at least rlc_get_csb_size dwords.
//------------------------------------------------------------------
static void
rlc_get_csb_buffer(const CSSectionDef *cs_data, uint32_t *buffer)
{
    if (cs_data == nullptr || buffer == nullptr) return;

    uint32_t count = 1;
    uint32_t clustercount = 0;
    for (const CSSectionDef *sect = cs_data; sect->section != nullptr; ++sect) {
        if (sect->id == kCSSection_CONTEXT) {
            for (const CSExtentDef *ext = sect->section;
                 ext->extent != nullptr; ++ext) {
                clustercount++;
                buffer[count++] = ext->reg_count;
                buffer[count++] = ext->reg_index;
                for (uint32_t i = 0; i < ext->reg_count; i++) {
                    buffer[count++] = ext->extent[i];
                }
            }
        } else {
            return;
        }
    }
    buffer[0] = clustercount;
}

kern_return_t
rlc_alloc_csb(GMCContext &gmc, RLCContext &rlc)
{
    if (rlc.inited && rlc.clear_state.gpu_va != 0) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) return kIOReturnNotReady;

    // Pre-compute the CSB size from the (now-vendored) cs_data table
    // so we don't over-allocate. Upstream amdgpu_gfx_rlc_init_csb
    // (amdgpu_rlc.c:128) does the same — it queries
    // get_csb_size, multiplies by 4, and allocates that many bytes.
    rlc.clear_state_dwords = rlc_get_csb_size(kGfx12_cs_data);
    const uint64_t csb_bytes_needed =
        static_cast<uint64_t>(rlc.clear_state_dwords) * 4u;
    const uint64_t alloc_bytes =
        (csb_bytes_needed < kRLCClearStateDefaultBytes)
            ? kRLCClearStateDefaultBytes  // pad up to page granularity
            : ((csb_bytes_needed + kASPageSize - 1) & ~(kASPageSize - 1));

    if (!gmc.vram_alloc.alloc(alloc_bytes, kASPageSize, &rlc.clear_state)) {
        RLC_LOG("CSB VRAM alloc failed (need %llu bytes, free=%llu)",
                static_cast<unsigned long long>(alloc_bytes),
                gmc.vram_alloc.bytes_free());
        return kIOReturnNoMemory;
    }

    // Store the VRAM byte offset (relative to vram_start) so
    // rlc_setup_csb_buffer can stream dwords via the BAR0 aperture.
    if (rlc.clear_state.gpu_va >= gmc.vram_start) {
        rlc.csb_vram_byte_offset = rlc.clear_state.gpu_va - gmc.vram_start;
    } else {
        rlc.csb_vram_byte_offset = 0;
    }

    RLC_LOG("CSB alloc: gpu_va=%#llx size=%llu (csb_dwords=%u)",
            rlc.clear_state.gpu_va, rlc.clear_state.size,
            rlc.clear_state_dwords);
    rlc.inited = true;
    return kIOReturnSuccess;
}

kern_return_t
rlc_wait_for_autoload_complete(const DeviceContext &dev, RLCContext &rlc)
{
    if (!dev.ip.isResolved(IPBlock::GC, 0) || !dev.ip.isResolved(IPBlock::GC, 1)) {
        RLC_LOG("GC IP base not resolved");
        return kIOReturnNotReady;
    }

    // CP_STAT lives at GC BASE_IDX 0 (gc_12_0_0_offset.h:2131).
    // RLC_RLCS_BOOTLOAD_STATUS lives at GC BASE_IDX 1 (line 6862).
    const uint32_t cp_stat_reg =
        SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0, GCRegs::CP_STAT);
    const uint32_t bootload_reg =
        SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1,
                              GCRegs::RLC_RLCS_BOOTLOAD_STATUS);

    // Log initial register state for triage.
    {
        uint32_t init_cp = RREG32(dev, cp_stat_reg);
        uint32_t init_bs = RREG32(dev, bootload_reg);
        // Cross-checks to verify BAR5 GC base resolution:
        //   GRBM_STATUS at GC[0] + 0x0DA4
        //   RLC_CGCG_CGLS_CTRL at GC[1] + 0x4C49
        //   BOOTLOAD_STATUS read at GC[0] + 0x4E7C (the OLD wrong address)
        const uint32_t grbm_reg =
            SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0, 0x0DA4);
        const uint32_t cgcg_reg =
            SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1, 0x4C49);
        const uint32_t bootload_b0 =
            SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0,
                                  GCRegs::RLC_RLCS_BOOTLOAD_STATUS);
        uint32_t grbm = RREG32(dev, grbm_reg);
        uint32_t cgcg = RREG32(dev, cgcg_reg);
        uint32_t bs_b0 = RREG32(dev, bootload_b0);
        RLC_LOG("autoload poll start: CP_STAT[GC0]=%#010x(@%#x) "
                "BOOTLOAD_STATUS[GC1]=%#010x(@%#x) "
                "BOOTLOAD_STATUS[GC0(wrong)]=%#010x(@%#x) "
                "GRBM_STATUS[GC0]=%#010x(@%#x) "
                "CGCG_CGLS_CTRL[GC1]=%#010x(@%#x)",
                init_cp, cp_stat_reg,
                init_bs, bootload_reg,
                bs_b0, bootload_b0,
                grbm, grbm_reg,
                cgcg, cgcg_reg);
    }

    // 10-second budget — Apple Silicon cold-boot autoload can be slow.
    const uint64_t kBudgetUs = 10 * 1000000;
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t elapsed = 0;
    uint32_t cp_stat = 0xFFFFFFFFu, bs = 0;
    while (elapsed < kBudgetUs) {
        cp_stat = RREG32(dev, cp_stat_reg);
        bs      = RREG32(dev, bootload_reg);
        if (cp_stat == UINT32_MAX || bs == UINT32_MAX) return kIOReturnNotAttached;
        elapsed = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000;
        if (cp_stat == 0 &&
            (bs & kRLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK)) {
            rlc.bootload_complete = true;
            RLC_LOG("RLC autoload complete (cp_stat=%#010x bootload=%#010x "
                    "after %llu µs)", cp_stat, bs, elapsed);
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000;
    }
    RLC_LOG("RLC autoload timeout (cp_stat=%#010x bootload=%#010x "
            "after %llu ms) — SRM enabled=%u",
            cp_stat, bs, elapsed / 1000,
            rlc.srm_enabled ? 1 : 0);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// rlc_setup_csb_buffer — fill the CSB content from gfx12_cs_data,
// then program RLC_CSIB_ADDR_HI/_LO + RLC_CSIB_LENGTH.
//
// Mirrors:
//   gfx_v12_0_get_csb_buffer    (gfx_v12_0.c:688) — fills the buffer
//   gfx_v12_0_init_csb          (gfx_v12_0.c:1905) — writes CSIB regs
//
// We can't (easily) kmap VRAM from the dext to memcpy into it — but
// the BAR0 aperture maps onto the visible VRAM window, so we stream
// dwords via WBAR0_32. The CSB lives inside the bump allocator's
// region which is within the BAR0-visible aperture.
//
// Audit-7 #7.
//------------------------------------------------------------------
kern_return_t
rlc_setup_csb_buffer(const DeviceContext &dev, GMCContext &gmc,
                     RLCContext &rlc)
{
    if (!rlc.inited || rlc.clear_state.gpu_va == 0) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    (void)gmc;

    if (!rlc.csb_populated) {
        // Build the CSB content into a small CPU-side buffer first,
        // then push it into VRAM via the BAR0 aperture.
        constexpr uint32_t kMaxCsbDwords = 1024;  // 4 KB — plenty for gfx12
        uint32_t cpu[kMaxCsbDwords];
        memset(cpu, 0, sizeof(cpu));

        if (rlc.clear_state_dwords > kMaxCsbDwords) {
            RLC_LOG("CSB size %u exceeds staging buffer %u",
                    rlc.clear_state_dwords, kMaxCsbDwords);
            return kIOReturnNoMemory;
        }
        rlc_get_csb_buffer(kGfx12_cs_data, cpu);

        // Stream into VRAM via BAR0 aperture writes.
#ifdef __APPLE__
        bar0_memcpy_to_vram(dev, rlc.csb_vram_byte_offset,
                            cpu,
                            static_cast<uint64_t>(rlc.clear_state_dwords) * 4u);
        // Drain the HDP write cache so RLC sees the CSB content.
        amdgpu_hdp_flush(dev);
#endif
        rlc.csb_populated = true;

        // Log first cluster for triage: dword[0] = cluster count.
        RLC_LOG("CSB filled: clustercount=%u, total_dwords=%u",
                cpu[0], rlc.clear_state_dwords);
    }

    // Program RLC_CSIB_ADDR_HI/_LO/_LENGTH per gfx_v12_0_init_csb
    // (gfx_v12_0.c:1909-1913). All three live at GC BASE_IDX 1
    // (gc_12_0_0_offset.h:7030/7032/7034).
    const uint64_t csb_gpu_va = rlc.clear_state.gpu_va;
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1,
                                      GCRegs::RLC_CSIB_ADDR_HI),
           static_cast<uint32_t>(csb_gpu_va >> 32));
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1,
                                      GCRegs::RLC_CSIB_ADDR_LO),
           static_cast<uint32_t>(csb_gpu_va & 0xFFFFFFFCu));
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1,
                                      GCRegs::RLC_CSIB_LENGTH),
           rlc.clear_state_dwords);

    rlc.csib_programmed = true;
    RLC_LOG("CSIB programmed: ADDR=%#llx LENGTH=%u",
            csb_gpu_va, rlc.clear_state_dwords);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// rlc_enable_srm — port of gfx_v12_0_rlc_enable_srm (gfx_v12_0.c:1967).
//
// Audit-7 #7.
//------------------------------------------------------------------
kern_return_t
rlc_enable_srm(const DeviceContext &dev, RLCContext &rlc)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    // regRLC_SRM_CNTL is at GC BASE_IDX 1 (gc_12_0_0_offset.h:6467).
    const uint32_t reg =
        SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 1, GCRegs::RLC_SRM_CNTL);

    // gfx_v12_0.c:1972-1975 — read, OR-in AUTO_INCR + SRM_ENABLE, write back.
    uint32_t tmp = RREG32(dev, reg);
    tmp |= RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK;
    tmp |= RLC_SRM_CNTL__SRM_ENABLE_MASK;
    WREG32(dev, reg, tmp);

    rlc.srm_enabled = true;
    RLC_LOG("RLC_SRM_CNTL = %#x (SRM enabled, auto-incr addr)", tmp);
    return kIOReturnSuccess;
}

kern_return_t
rlc_init_full(const DeviceContext &dev, GMCContext &gmc, RLCContext &rlc)
{
    kern_return_t r;

    // Guard: if PSP rejected the RLC sub-bin LOAD_IP_FW frames we
    // CANNOT touch RLC registers — programming RLC_CSIB/RLC_SRM_CNTL
    // against a firmware-less RLC drops the GPU off the PCIe bus.
    if (!rlc.microcode_loaded) {
        RLC_LOG("init_full: RLC microcode not loaded into PSP "
                "(LOAD_IP_FW failed earlier) — refusing to touch "
                "RLC hardware; the link would drop");
        return kIOReturnNotReady;
    }

    // Match upstream gfx_v12_0_hw_init + gfx_v12_0_rlc_resume(PSP):
    //   1. wait_for_rlc_autoload_complete    (poll CP_STAT==0 + BOOTLOAD_COMPLETE)
    //   2. gfx_v12_0_init_csb                (fill CSB + program RLC_CSIB_*)
    //   3. gfx_v12_0_rlc_enable_srm          (RLC_SRM_CNTL AUTO_INCR + SRM_ENABLE)
    //
    // Upstream does NOT touch RLC_CGCG_CGLS_CTRL on the PSP path —
    // clock-gating is programmed later in set_clockgating_state with
    // proper field masking. The PSP+IMU autoload runs autonomously
    // after psp_rlc_autoload_start (GFX_CMD_ID_AUTOLOAD_RLC); the
    // driver's job is just to wait for completion, then set up CSB+SRM.

    // Autoload was checked before RS64/GFXHUB preparation.
    r = rlc.bootload_complete ? kIOReturnSuccess : rlc_wait_for_autoload_complete(dev, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("autoload wait failed: %#x", r);
        return r;
    }

    // 2. Allocate CSB in VRAM.
    r = rlc_alloc_csb(gmc, rlc);
    if (r != kIOReturnSuccess) return r;

    // 3. Fill CSB content + program RLC_CSIB_ADDR_HI/_LO/_LENGTH
    //    (== upstream gfx_v12_0_init_csb).
    r = rlc_setup_csb_buffer(dev, gmc, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("setup_csb_buffer failed: %#x", r);
        return r;
    }

    // 4. Enable SRM + AUTO_INCR_ADDR (== upstream gfx_v12_0_rlc_enable_srm).
    r = rlc_enable_srm(dev, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("enable_srm failed: %#x", r);
        return r;
    }

    return kIOReturnSuccess;
}

} // namespace amdgpu
