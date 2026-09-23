#pragma once
#include "amdgpu_ip.h"

namespace amdgpu {
enum class ScratchSRDLayout : uint8_t { Gfx9, Gfx10, Gfx11, Gfx12 };
struct ScratchArchitectureCapabilities {
    uint8_t gfxMajor;
    uint16_t waveSizeUnitBytes;
    uint8_t wavesBits, waveSizeBits, srdAddressBits, srdRecordBits;
    // Legacy WAVES counts are per XCC; GFX11+ counts are per shader engine.
    bool wavesPerEngine;
    ScratchSRDLayout srdLayout;
};
// ROCr AqlQueue constructor, FillComputeTmpRingSize* and SQ_BUF_RSRC unions.
// These are architecture properties, not an assertion that our queue/MQD
// implementation supports every member of the architecture family.
constexpr ScratchArchitectureCapabilities kScratchArchitectures[] = {
    {9, 1024, 12, 13, 48, 32, false, ScratchSRDLayout::Gfx9},
    {10,1024, 12, 13, 48, 32, false, ScratchSRDLayout::Gfx10},
    {11, 256, 12, 15, 48, 32, true,  ScratchSRDLayout::Gfx11},
    {12, 256, 12, 18, 48, 32, true,  ScratchSRDLayout::Gfx12},
};
constexpr const ScratchArchitectureCapabilities *scratch_architecture(IPVersion ip) {
    for (const auto &capability:kScratchArchitectures)
        if (capability.gfxMajor==ip.major) return &capability;
    return nullptr;
}
constexpr bool aql_scratch_layout_supported(IPVersion ip) {
    return ip.major==12 && ip.minor==0 && ip.rev==1;
}
}
