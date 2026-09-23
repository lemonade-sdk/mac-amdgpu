#pragma once
#include <stdint.h>
#include <stddef.h>

namespace amdgpu {
// Native owner-only launch ABI. Code objects and kernarg layout are prepared
// by the userspace loader. All owner BOs remain live during this synchronous
// call; this VMID0 path is a trusted compute interface, not process isolation.
struct ComputeDispatchRequest {
    uint32_t version; // 1 = original 264-byte prefix; 2 = full descriptor registers
    uint32_t flags;   // reserved, zero; wave32, v1 CU mode / v2 CU or WGP mode
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
    uint32_t rsrc3; // version 2: instruction prefetch only; no shared VGPRs
    uint32_t reserved;
};
constexpr uint32_t kComputeDispatchV1Bytes = 264;
static_assert(offsetof(ComputeDispatchRequest, rsrc3) == kComputeDispatchV1Bytes, "legacy dispatch wire ABI");
static_assert(sizeof(ComputeDispatchRequest) == 272, "version 2 dispatch wire ABI");

inline bool compute_dispatch_shape(const ComputeDispatchRequest &r)
{
    if ((r.version != 1 && r.version != 2) || r.flags || r.reserved || !r.codeHandle || !r.codeBytes ||
        (r.codeBytes & 3) || (r.codeOffset & 255) || !r.timeoutUS ||
        r.timeoutUS > 1000000 || r.userSGPRCount > 16) return false;
    uint32_t threads = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!r.groups[i] || r.groups[i] > 0x7fffffffu ||
            !r.threads[i] || r.threads[i] > 1024) return false;
        threads *= r.threads[i];
        if (threads > 1024) return false;
    }
    // GFX12: VGPR allocation + FLOAT_MODE + FP16_OVFL. Version 2 also
    // preserves WGP_MODE, MEM_ORDERED and FWD_PROGRESS from AMDHSA metadata.
    // No privileged/debug/trap flags. Scratch/dynamic VGPR and
    // exceptions need separate runtime backing and are not supported yet.
    const uint32_t rsrc1Mask = r.version == 2 ? 0xe40ff03fu : 0x040ff03fu;
    if ((r.rsrc1 & ~rsrc1Mask) || (r.rsrc3 & ~0x00000ff0u) ||
        (r.version == 1 && r.rsrc3)) return false;
    constexpr uint32_t rsrc2Mask = 0x00ff8000u | 0x00001800u | 0x00000780u | 0x3eu;
    if ((r.rsrc2 & ~rsrc2Mask) || ((r.rsrc2 >> 1) & 31) != r.userSGPRCount ||
        ((r.rsrc2 >> 11) & 3) == 3 || ((r.rsrc2 >> 15) & 511) > 128) return false;
    for (unsigned i = r.userSGPRCount; i < 16; ++i)
        if (r.userSGPR[i]) return false;
    return true;
}
}
