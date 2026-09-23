#pragma once
#include <stdint.h>

namespace amdgpu {
// Bounded, owner-only VMID0 dispatch. Descriptor and kernarg reside in owned
// VRAM BOs. This is not a persistent HSA queue or an isolation boundary.
struct AQLDispatchRequest {
    uint32_t version, flags;
    uint64_t codeHandle, descriptorOffset;
    uint64_t kernargHandle, kernargOffset, kernargBytes;
    uint32_t groups[3], threads[3];
    uint32_t timeoutUS, reserved;
    uint64_t buffers[16];
};
static_assert(sizeof(AQLDispatchRequest) == 208);
inline bool aql_dispatch_shape(const AQLDispatchRequest &r) {
    if (r.version != 1 || r.flags || r.reserved || !r.codeHandle ||
        (r.descriptorOffset & 63) || !r.kernargHandle || (r.kernargOffset & 15) ||
        r.kernargBytes > 4 * 1024 * 1024 || !r.timeoutUS || r.timeoutUS > 1000000)
        return false;
    uint64_t threads = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!r.groups[i] || !r.threads[i] || r.threads[i] > 1024 ||
            uint64_t(r.groups[i]) * r.threads[i] > UINT32_MAX) return false;
        threads *= r.threads[i];
    }
    return threads <= 1024;
}
}
