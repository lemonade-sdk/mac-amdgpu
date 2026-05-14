//
//  gfx_v12_0.cpp — GFX12 top-level configuration.
//
//  Currently: gfx_v12_0_constants_init only.
//
//  Source: drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:1806
//

#include <os/log.h>
#include "amdgpu_gfx.h"

#define GFX_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.gfx: " fmt, ##__VA_ARGS__)

namespace amdgpu {

kern_return_t
gfx_constants_init(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        GFX_LOG("constants_init: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    auto reg = [&](uint32_t off) {
        return SOC15_REG_OFFSET(dev, IPBlock::GC, off);
    };

    // 1) GRBM_CNTL.READ_TIMEOUT = 0xFF. Sets the MMIO read timeout
    //    register so the GRBM doesn't bail too early on a slow path.
    {
        uint32_t v = RREG32(dev, reg(GFXRegs::GRBM_CNTL));
        v = (v & ~kGRBM_CNTL_READ_TIMEOUT_MASK) | kGRBM_CNTL_READ_TIMEOUT_VALUE;
        WREG32(dev, reg(GFXRegs::GRBM_CNTL), v);
    }

    // 2) SH_MEM_CONFIG for the kernel-driver VMID (0). Default
    //    config: 64-bit address mode + unaligned alignment + initial
    //    instruction prefetch of 3.
    //
    //    Upstream loops over all VMIDs and selects each via
    //    soc24_grbm_select(adev, me=0, pipe=0, queue=0, vmid=i)
    //    before writing SH_MEM_BASES/SH_MEM_CONFIG. For first PM4
    //    we only touch VMID 0 — the kernel context. User VMIDs are
    //    Phase 2 (when we wire up user-mode queues + compute).
    WREG32(dev, reg(GFXRegs::SH_MEM_CONFIG), kDefaultSHMemConfig);

    // pa_sc_tile_steering_override = 0 — Linux explicitly sets the
    // value on adev->gfx.config but doesn't WREG32 it during
    // constants_init. The register write happens elsewhere (in
    // setup_rb when harvested config is known). We skip both for
    // now; default is 0.

    GFX_LOG("constants_init: GRBM_CNTL.READ_TIMEOUT=%#x, "
            "SH_MEM_CONFIG=%#x (vmid 0)",
            kGRBM_CNTL_READ_TIMEOUT_VALUE, kDefaultSHMemConfig);
    return kIOReturnSuccess;
}

} // namespace amdgpu
