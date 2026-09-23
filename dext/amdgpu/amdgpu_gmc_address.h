#pragma once

#include <stdint.h>

namespace amdgpu {

// GFX12 VRAM PDE address transform (gmc_v12_0_get_vm_pde).
// Callers must supply an address inside VRAM. SYSTEM PTEs use DMA
// addresses directly and must never pass through this transform.
constexpr uint64_t gfx12_vram_walker_address(uint64_t mcAddress,
                                            uint64_t vramStart,
                                            uint64_t vramBaseOffset)
{
    return mcAddress - vramStart + vramBaseOffset;
}

// Linux AMDGPU_GART_PLACEMENT_LOW for a fixed-size aperture. Reject
// insufficient space instead of overlapping VRAM or entering the VA hole.
inline bool gfx12_gart_location_low(uint64_t fbStart, uint64_t fbEnd,
                                    uint64_t size, uint64_t &start)
{
    constexpr uint64_t limit = 1ULL << 47; // AMDGPU_GMC_HOLE_START, 48-bit VA
    constexpr uint64_t alignment = 1ULL << 32;
    if (!size || (size & 0xfff) || fbStart > fbEnd || fbEnd >= limit)
        return false;
    const uint64_t candidate = fbStart >= size ? 0 :
        (fbEnd + 1 + alignment - 1) & ~(alignment - 1);
    if (candidate >= limit || size > limit - candidate)
        return false;
    start = candidate;
    return true;
}

} // namespace amdgpu
