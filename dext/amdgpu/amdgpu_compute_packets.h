#pragma once
#include "amdgpu_pm4.h"

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
constexpr uint32_t kComputeSmokePacketCapacity = 192;
constexpr uint32_t kComputeSmokeAcquireDwords = 8;

// Register byte addresses and fields follow Mesa gfx12_init_compute_preamble_state
// and si_emit_dispatch_packets; packets are executed on the kernel GFX queue.
// This is a fixed diagnostic program, not an HSA AQL queue or arbitrary launcher.
inline uint32_t compute_smoke_packets(uint32_t (&out)[kComputeSmokePacketCapacity],
    uint64_t codeVA, uint64_t dataVA, uint32_t seed, const uint32_t (&cuMask)[4],
    uint32_t *dispatchOffset = nullptr)
{
    if (dispatchOffset) *dispatchOffset = 0;
    if ((codeVA & 255) || (codeVA >> 48) || (dataVA & 3) ||
        dataVA > ((1ull << 48) - kComputeSmokeDataBytes) ||
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
    // Three VGPRs fit one allocation granule. Match radeonsi's GFX12 CU mode:
    // MEM_ORDERED is only enabled on older generations. No legacy high flags.
    // Round-to-nearest and preserve fp32 denormals (no FP ops here).
    sh(0xb848, 0x000c0000);
    sh(0xb84c, 3u << 1); // three user SGPRs, TID X only, no TGID/LDS/scratch
    sh(0xb8a0, 0);
    sh(0xb860, 0); // no scratch ring
    sh(0xb854, 1u << 24); // Mesa CU_GROUP_COUNT(2 - 1) for one-wave groups
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
    sh(0xb81c, kComputeSmokeLanes); sh(0xb820, 1); sh(0xb824, 1);
    sh(0xb900, static_cast<uint32_t>(dataVA));
    sh(0xb904, static_cast<uint32_t>(dataVA >> 32));
    sh(0xb908, seed);
    if (dispatchOffset) *dispatchOffset = n;
    out[n++] = pm4_header(0x15, 3) | 2; // DISPATCH_DIRECT, compute
    out[n++] = 1; out[n++] = 1; out[n++] = 1;
    out[n++] = 0x8045; // enable, start 000, order mode, wave32
    // ACQUIRE_MEM alone does not wait for shader completion. Drain compute
    // before flushing its stores; the caller appends a unique EOP fence.
    out[n++] = pm4_header(0x46, 0);
    out[n++] = 0x407; // CS_PARTIAL_FLUSH, event index 4
    sync();
    return n;
}
}
