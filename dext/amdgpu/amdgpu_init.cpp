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

    (void)ctx; // currently a no-op (versions are compile-time consts)
    return kIOReturnSuccess;
}

//============================================================
// Stage dispatch.
//============================================================
static const char *
stage_name(BringupStage s)
{
    switch (s) {
    case BringupStage::None:          return "None";
    case BringupStage::IPDiscovery:   return "IPDiscovery";
    case BringupStage::PSPInit:       return "PSPInit";
    case BringupStage::PSPLoadSOS:    return "PSPLoadSOS";
    case BringupStage::PSPRingCreate: return "PSPRingCreate";
    case BringupStage::TMRSetup:      return "TMRSetup";
    case BringupStage::SMUInit:       return "SMUInit";
    case BringupStage::GMCInit:       return "GMCInit";
    case BringupStage::IMUInit:       return "IMUInit";
    case BringupStage::RLCInit:       return "RLCInit";
    case BringupStage::CPInit:        return "CPInit";
    case BringupStage::MESInit:       return "MESInit";
    case BringupStage::IHInit:        return "IHInit";
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
    case BringupStage::PSPInit:
        return psp_init(ctx.device, ctx.psp);
    case BringupStage::PSPLoadSOS:
        return psp_load_sos(ctx.device, ctx.psp);
    case BringupStage::PSPRingCreate:
        return psp_ring_create(ctx.device, ctx.psp);
    case BringupStage::TMRSetup:
        return psp_setup_tmr(ctx.device, ctx.psp);
    case BringupStage::SMUInit: {
        uint32_t echo = 0;
        kern_return_t r = smu_test_message(ctx.device, &echo);
        if (r == kIOReturnSuccess) {
            uint32_t ver = 0;
            (void)smu_get_version(ctx.device, &ver);
            ctx.device.smuOnline = true;
        }
        return r;
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
    // Still-stubs — return Unsupported until ported.
    case BringupStage::IMUInit:
    case BringupStage::MESInit:
    case BringupStage::GFXInit:
        INIT_LOG("stage %{public}s: NOT YET IMPLEMENTED", stage_name(s));
        return kIOReturnUnsupported;
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
