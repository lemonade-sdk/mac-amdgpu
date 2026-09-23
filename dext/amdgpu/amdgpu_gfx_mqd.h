#pragma once
#include <stdint.h>
#include <string.h>

namespace amdgpu {
// v12_gfx_mqd, initialized as gfx_v12_0_gfx_mqd_init for a kernel queue.
// Unused user-queue shadow/CSA/fence fields remain zero, as in
// amdgpu_ring_to_mqd_prop. Firmware owns the uploaded descriptor after mapping.
struct GFXQueueDescriptor { uint32_t words[512]; };
namespace GFXMQDOff {
constexpr uint32_t cp_mqd_base_addr = 128;
constexpr uint32_t cp_mqd_base_addr_hi = 129;
constexpr uint32_t cp_gfx_hqd_active = 130;
constexpr uint32_t cp_gfx_hqd_vmid = 131;
constexpr uint32_t cp_gfx_hqd_queue_priority = 134;
constexpr uint32_t cp_gfx_hqd_quantum = 135;
constexpr uint32_t cp_gfx_hqd_base = 136;
constexpr uint32_t cp_gfx_hqd_base_hi = 137;
constexpr uint32_t cp_gfx_hqd_rptr = 138;
constexpr uint32_t cp_gfx_hqd_rptr_addr = 139;
constexpr uint32_t cp_gfx_hqd_rptr_addr_hi = 140;
constexpr uint32_t cp_rb_wptr_poll_addr_lo = 141;
constexpr uint32_t cp_rb_wptr_poll_addr_hi = 142;
constexpr uint32_t cp_rb_doorbell_control = 143;
constexpr uint32_t cp_gfx_hqd_cntl = 145;
constexpr uint32_t cp_gfx_hqd_wptr = 149;
constexpr uint32_t cp_gfx_hqd_wptr_hi = 150;
constexpr uint32_t cp_gfx_mqd_control = 162;
}

static inline bool gfx_build_kernel_mqd(GFXQueueDescriptor &mqd,
    uint64_t mqdAddress, uint64_t ringAddress, uint64_t rptrAddress,
    uint64_t wptrAddress, uint32_t ringBytes, uint32_t doorbell)
{
    if (!mqdAddress || (mqdAddress & 255) || !ringAddress || (ringAddress & 255) ||
        !rptrAddress || (rptrAddress & 3) || !wptrAddress || (wptrAddress & 7) ||
        ringBytes < 1024 || (ringBytes & (ringBytes - 1)) ||
        (doorbell & 1) || doorbell > 0x03fffffeu) return false;
    uint32_t log2 = 0;
    for (uint32_t n = ringBytes / 4; n > 1; n >>= 1) ++log2;
    const uint32_t bufsz = log2 - 1;
    memset(&mqd, 0, sizeof(mqd));
    auto *w = mqd.words;
    w[GFXMQDOff::cp_mqd_base_addr] = uint32_t(mqdAddress);
    w[GFXMQDOff::cp_mqd_base_addr_hi] = uint32_t(mqdAddress >> 32);
    w[GFXMQDOff::cp_gfx_mqd_control] = 0x100; // VMID 0, PRIV_STATE, cached
    w[GFXMQDOff::cp_gfx_hqd_quantum] = 0xa01; // Linux default, QUANTUM_EN
    w[GFXMQDOff::cp_gfx_hqd_base] = uint32_t(ringAddress >> 8);
    w[GFXMQDOff::cp_gfx_hqd_base_hi] = uint32_t(ringAddress >> 40);
    w[GFXMQDOff::cp_gfx_hqd_rptr_addr] = uint32_t(rptrAddress);
    w[GFXMQDOff::cp_gfx_hqd_rptr_addr_hi] = uint32_t(rptrAddress >> 32) & 0xffff;
    w[GFXMQDOff::cp_rb_wptr_poll_addr_lo] = uint32_t(wptrAddress);
    w[GFXMQDOff::cp_rb_wptr_poll_addr_hi] = uint32_t(wptrAddress >> 32) & 0xffff;
    w[GFXMQDOff::cp_gfx_hqd_cntl] = 0x00f00000 | bufsz | ((bufsz - 2) << 8);
    w[GFXMQDOff::cp_rb_doorbell_control] = 0x40000000 | (doorbell << 2);
    w[GFXMQDOff::cp_gfx_hqd_active] = 1;
    return true;
}
} // namespace amdgpu
