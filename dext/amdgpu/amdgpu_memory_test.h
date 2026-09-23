#pragma once
#include "amdgpu_gart.h"
#include "amdgpu_vram.h"

namespace amdgpu {
struct SDMAInstance;
// Retained by BringupContext, including failed mappings or GPU submissions.
struct MemoryTransferTest {
    bool active;
    GARTBinding host;
    VRAMAllocation vram;
    IOBufferMemoryDescriptor *staging;
};
struct MemoryTransferResult {
    uint32_t stage;          // 1=allocate, 2=upload, 3=GPU read, 4=verify VRAM,
                             // 5=GPU write, 6=verify host, 7=unbind, 8=complete,
                             // 9/10=direct CPU→GPU/verify, 11/12=GPU→CPU/verify
    uint32_t mismatches;
    uint32_t firstMismatch;  // byte offset, UINT32_MAX if none
    uint64_t hostGPUAddress;
    uint64_t vramGPUAddress;
};
kern_return_t memory_transfer_test(DeviceContext &dev, GMCContext &gmc,
    GARTContext &gart, SDMAInstance &sdma, MemoryTransferTest &test,
    uint32_t seed, MemoryTransferResult &result);
void memory_transfer_release_after_reset(MemoryTransferTest &test);
}
