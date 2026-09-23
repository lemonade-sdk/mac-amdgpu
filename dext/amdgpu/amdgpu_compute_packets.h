#pragma once
#include "amdgpu_pm4.h"
#include "amdgpu_dispatch_abi.h"

namespace amdgpu {
// Assembled from tests/shaders/compute_smoke_gfx1201.s using LLVM 21.1.8.
// scripts/test-compute-smoke.sh verifies the bytes against the assembler.
constexpr uint32_t kComputeSmokeCode[] = {
    0x30020082, 0xee050000, 0x00000002, 0x00000001, 0xbfc00000,
    0x4a040402, 0xee068000, 0x01000000, 0x00010001, 0xbfc10000,
    0xbfb00000,
};
constexpr uint32_t kComputeSmokeLanes = 32;
constexpr uint32_t kComputeSmokeOutputOffset = 256;
constexpr uint32_t kComputeSmokeDataBytes = 4096;
constexpr uint32_t kComputeSmokePacketCapacity = 256;
constexpr uint32_t kComputeSmokeAcquireDwords = 8;

// Register byte addresses and fields follow Mesa gfx12_init_compute_preamble_state
// and si_emit_dispatch_packets; packets are executed on the kernel GFX queue.
// The userspace loader supplies resources and register arguments; the driver
// chooses the active CU mask and always enters through a GFX IB with VMID0.
inline uint32_t compute_dispatch_packets(uint32_t (&out)[kComputeSmokePacketCapacity],
    uint64_t codeVA, const ComputeDispatchRequest &r, const uint32_t (&cuMask)[4],
    uint32_t *dispatchOffset = nullptr)
{
    if (dispatchOffset) *dispatchOffset = 0;
    if (!compute_dispatch_shape(r) || (codeVA & 255) || (codeVA >> 48) ||
        r.codeBytes > (1ull << 48) - codeVA ||
        !(cuMask[0] | cuMask[1] | cuMask[2] | cuMask[3])) return 0;
    for (auto mask : cuMask) if (mask & 0xffff0000u) return 0;
    uint32_t n = 0;
    auto sh = [&](uint32_t byteReg, uint32_t value) {
        out[n++] = pm4_header(0x76, 1) | 2; // SET_SH_REG, shader type compute
        out[n++] = (byteReg - 0xb000) / 4;
        out[n++] = value;
    };
    auto sync = [&]() {
        // Linux gfx_v12_0_emit_mem_sync, including instruction invalidation.
        out[n++] = pm4_header(0x58, 6);
        out[n++] = 0;
        out[n++] = 0xffffffff;
        out[n++] = 0xffffff;
        out[n++] = 0;
        out[n++] = 0;
        out[n++] = 10;
        out[n++] = 0xc3b1;
    };
    sync(); // Upload may reuse addresses cached by an earlier dispatch.
    sh(0xb82c, 0); // profiling disabled
    sh(0xb830, static_cast<uint32_t>(codeVA >> 8));
    sh(0xb834, static_cast<uint32_t>(codeVA >> 40));
    sh(0xb838, 0); sh(0xb83c, 0); // no AQL dispatch-packet pointer
    sh(0xb840, 0); sh(0xb844, 0); // no scratch
    sh(0xb848, r.rsrc1);
    sh(0xb84c, r.rsrc2);
    sh(0xb8a0, r.rsrc3);
    sh(0xb860, 0); // no scratch ring
    const uint32_t waves = (r.threads[0] * r.threads[1] * r.threads[2] + 31) / 32;
    // Mesa uses paired one-wave groups in CU mode. Larger groups stand alone.
    const bool wgpMode = (r.rsrc1 & (1u << 29)) != 0;
    sh(0xb854, (waves == 1 && !wgpMode ? 1u << 24 : 0) | (waves % 4 == 0 ? 1u << 22 : 0));
    const uint32_t seRegs[] = {0xb858, 0xb85c, 0xb864, 0xb868};
    for (uint32_t i = 0; i < 4; ++i) sh(seRegs[i], cuMask[i]);
    sh(0xb88c, 0); // SE8
    for (uint32_t i = 0; i < 4; ++i) {
        sh(0xb890 + i * 4, 0); // USER_ACCUM
        sh(0xb8ac + i * 4, 0); // SE4..7
    }
    sh(0xb8bc, 0); // no dispatch interleave
    sh(0xb9f4, 0); // no tunneling
    sh(0xb810, 0); sh(0xb814, 0); sh(0xb818, 0); // START_X/Y/Z
    sh(0xb81c, r.threads[0]); sh(0xb820, r.threads[1]); sh(0xb824, r.threads[2]);
    // Clear unused register arguments so a shorter launch cannot inherit them.
    for (uint32_t i = 0; i < 16; ++i) sh(0xb900 + i * 4, r.userSGPR[i]);
    if (dispatchOffset) *dispatchOffset = n;
    out[n++] = pm4_header(0x15, 3) | 2; // DISPATCH_DIRECT, compute
    out[n++] = r.groups[0]; out[n++] = r.groups[1]; out[n++] = r.groups[2];
    out[n++] = 0x8045; // enable, start 000, order mode, wave32
    // ACQUIRE_MEM alone does not wait for shader completion. Drain compute
    // before flushing its stores; the caller appends a unique EOP fence.
    out[n++] = pm4_header(0x46, 0);
    out[n++] = 0x407; // CS_PARTIAL_FLUSH, event index 4
    sync();
    return n;
}
inline uint32_t compute_smoke_packets(uint32_t (&out)[kComputeSmokePacketCapacity],
    uint64_t codeVA, uint64_t dataVA, uint32_t seed, const uint32_t (&cuMask)[4],
    uint32_t *dispatchOffset = nullptr)
{
    if (dispatchOffset) *dispatchOffset = 0;
    if ((dataVA & 3) || dataVA > ((1ull << 48) - kComputeSmokeDataBytes)) return 0;
    ComputeDispatchRequest r{};
    r.version = 1; r.codeHandle = 1; r.codeBytes = sizeof(kComputeSmokeCode);
    r.timeoutUS = 100000;
    r.groups[0] = r.groups[1] = r.groups[2] = 1;
    r.threads[0] = kComputeSmokeLanes; r.threads[1] = r.threads[2] = 1;
    r.rsrc1 = 0xc0000; r.rsrc2 = 3u << 1; r.userSGPRCount = 3;
    r.userSGPR[0] = static_cast<uint32_t>(dataVA);
    r.userSGPR[1] = static_cast<uint32_t>(dataVA >> 32); r.userSGPR[2] = seed;
    return compute_dispatch_packets(out, codeVA, r, cuMask, dispatchOffset);
}

}
