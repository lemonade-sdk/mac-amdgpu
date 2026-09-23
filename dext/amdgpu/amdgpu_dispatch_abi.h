#pragma once
#include <stdint.h>

namespace amdgpu {
// Native owner-only launch ABI. Code objects and kernarg layout are prepared
// by the userspace loader. All owner BOs remain live during this synchronous
// call; this VMID0 path is a trusted compute interface, not process isolation.
struct ComputeDispatchRequest {
    uint32_t version; // 1
    uint32_t flags;   // reserved, zero; wave32 / CU mode
    uint64_t codeHandle;
    uint64_t codeOffset;
    uint64_t codeBytes;
    uint32_t groups[3];
    uint32_t threads[3];
    uint32_t rsrc1;
    uint32_t rsrc2;
    uint32_t userSGPRCount;
    uint32_t timeoutUS; // 1..1,000,000
    uint32_t userSGPR[16];
    uint64_t buffers[16]; // referenced owner BO handles, unused entries zero
};
static_assert(sizeof(ComputeDispatchRequest) == 264, "dispatch wire ABI");

inline bool compute_dispatch_shape(const ComputeDispatchRequest &r)
{
    if (r.version != 1 || r.flags || !r.codeHandle || !r.codeBytes ||
        (r.codeBytes & 3) || (r.codeOffset & 255) || !r.timeoutUS ||
        r.timeoutUS > 1000000 || r.userSGPRCount > 16) return false;
    uint32_t threads = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!r.groups[i] || r.groups[i] > 0x7fffffffu ||
            !r.threads[i] || r.threads[i] > 1024) return false;
        threads *= r.threads[i];
        if (threads > 1024) return false;
    }
    // GFX12: VGPR allocation + FLOAT_MODE + FP16_OVFL. CU mode, no
    // privileged/debug/trap/ordered-memory flags. Scratch/dynamic VGPR and
    // exceptions need separate runtime backing and are not supported yet.
    if (r.rsrc1 & ~0x040ff03fu) return false;
    constexpr uint32_t rsrc2Mask = 0x00ff8000u | 0x00001800u | 0x00000780u | 0x3eu;
    if ((r.rsrc2 & ~rsrc2Mask) || ((r.rsrc2 >> 1) & 31) != r.userSGPRCount ||
        ((r.rsrc2 >> 11) & 3) == 3 || ((r.rsrc2 >> 15) & 511) > 128) return false;
    for (unsigned i = r.userSGPRCount; i < 16; ++i)
        if (r.userSGPR[i]) return false;
    return true;
}
}
