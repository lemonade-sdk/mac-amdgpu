#pragma once
#include "amdgpu_dispatch_abi.h"
#include "amdgpu_vram.h"
#include "amdgpu_regs.h"

namespace amdgpu {
struct GMCContext;
struct CPContext;
struct GFXConfig;
struct ComputeLaunch {
    VRAMAllocation ib;
    bool retained;
};
struct ComputeLaunchResult {
    uint32_t fence;
    uint32_t stage; // 0=preflight, 1=upload, 2=submit/wait, 3=complete
};
kern_return_t compute_launch(DeviceContext &dev, GMCContext &gmc, CPContext &cp,
    const GFXConfig &gfx, ComputeLaunch &launch, uint64_t codeVA,
    const ComputeDispatchRequest &request, ComputeLaunchResult &result);
}
