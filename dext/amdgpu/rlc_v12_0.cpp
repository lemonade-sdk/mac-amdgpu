//
//  rlc_v12_0.cpp — RLC clear-state alloc + autoload wait for GFX12.
//
//  Sources:
//    drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c:amdgpu_gfx_rlc_init_csb
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_wait_for_rlc_autoload_complete
//

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include "amdgpu_rlc.h"
#include "amdgpu_gmc.h"   // GMCContext / VRAMBumpAllocator

#define RLC_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.rlc: " fmt, ##__VA_ARGS__)

namespace amdgpu {

kern_return_t
rlc_alloc_csb(GMCContext &gmc, RLCContext &rlc)
{
    if (rlc.inited && rlc.clear_state.gpu_va != 0) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) return kIOReturnNotReady;

    // Allocate the CSB out of visible VRAM. Top-down bump allocator
    // hands back a 16 KB-aligned region.
    if (!gmc.vram_alloc.alloc(kRLCClearStateDefaultBytes, kASPageSize,
                              &rlc.clear_state)) {
        RLC_LOG("CSB VRAM alloc failed (need %u bytes, free=%llu)",
                kRLCClearStateDefaultBytes, gmc.vram_alloc.bytes_free());
        return kIOReturnNoMemory;
    }
    rlc.clear_state_dwords = kRLCClearStateDefaultBytes / 4;

    // CSB content stays zeroed for now. Linux fills it with a
    // per-asic register/value table (gfx12_cs_data) via
    // amdgpu_gfx_rlc_setup_csb_buffer. Required for power-gating
    // state save/restore; not required for a simple NOP/RELEASE_MEM
    // submission. TODO(phase1b-pg): port cs_data + setup_csb_buffer.

    RLC_LOG("CSB alloc: gpu_va=%#llx size=%llu",
            rlc.clear_state.gpu_va, rlc.clear_state.size);
    rlc.inited = true;
    return kIOReturnSuccess;
}

kern_return_t
rlc_wait_for_autoload_complete(const DeviceContext &dev, RLCContext &rlc)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        RLC_LOG("GC IP base not resolved");
        return kIOReturnNotReady;
    }

    const uint32_t cp_stat_reg =
        SOC15_REG_OFFSET(dev, IPBlock::GC, GCRegs::CP_STAT);
    const uint32_t bootload_reg =
        SOC15_REG_OFFSET(dev, IPBlock::GC, GCRegs::RLC_RLCS_BOOTLOAD_STATUS);

    // 5-second budget — cold-boot autoload can take seconds the
    // first time PSP loads everything.
    const uint64_t kBudgetUs = 5 * 1000000;
    uint64_t elapsed = 0;
    uint32_t cp_stat = 0xFFFFFFFFu, bs = 0;
    while (elapsed < kBudgetUs) {
        cp_stat = RREG32(dev, cp_stat_reg);
        bs      = RREG32(dev, bootload_reg);
        if (cp_stat == 0 &&
            (bs & kRLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK)) {
            rlc.bootload_complete = true;
            RLC_LOG("RLC autoload complete (cp_stat=%#010x bootload=%#010x "
                    "after %llu µs)", cp_stat, bs, elapsed);
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed += 1000;
    }
    RLC_LOG("RLC autoload timeout (cp_stat=%#010x bootload=%#010x)",
            cp_stat, bs);
    return kIOReturnTimeout;
}

kern_return_t
rlc_init_full(const DeviceContext &dev, GMCContext &gmc, RLCContext &rlc)
{
    kern_return_t r;
    r = rlc_alloc_csb(gmc, rlc);
    if (r != kIOReturnSuccess) return r;
    r = rlc_wait_for_autoload_complete(dev, rlc);
    if (r != kIOReturnSuccess) return r;
    return kIOReturnSuccess;
}

} // namespace amdgpu
