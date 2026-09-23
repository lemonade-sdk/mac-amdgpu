#pragma once
#include "amdgpu_vram.h"
#include "amdgpu_regs.h"

namespace amdgpu {
struct GMCContext;
struct CPContext;
struct GFXConfig;
struct ComputeTest {
    bool active;
    VRAMAllocation storage;
};
struct ComputeTestResult {
    uint32_t stage; // 0=preflight, 1=allocate, 2=upload, 3=cache, 4=registers,
                    // 5=shader, 6=verify, 7=complete
    uint32_t mismatches;
    uint32_t firstMismatch;
    uint32_t fence;
    uint64_t gpuAddress;
};
kern_return_t compute_test(DeviceContext &dev, GMCContext &gmc, CPContext &cp,
    const GFXConfig &gfx, ComputeTest &test, uint32_t seed, ComputeTestResult &result);
}
