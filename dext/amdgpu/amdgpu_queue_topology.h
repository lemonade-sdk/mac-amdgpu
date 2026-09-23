#pragma once
#include <stdint.h>

namespace amdgpu {
// Linux gfx_v12_0_sw_init uses this topology for GC 12.0.0/12.0.1.
// The four-pipe/eight-queue default applies to other versions, not gfx1201.
constexpr uint32_t kGFX1201ComputePipes=2;
constexpr uint32_t kGFX1201QueuesPerPipe=4;
constexpr unsigned kPersistentAQLQueues=kGFX1201ComputePipes*kGFX1201QueuesPerPipe-1;
constexpr uint32_t gfx1201_compute_pipe(uint32_t slot) {return slot/kGFX1201QueuesPerPipe;}
constexpr uint32_t gfx1201_compute_queue(uint32_t slot) {return slot%kGFX1201QueuesPerPipe;}

// Linux mqd_symmetrically_map_cu_mask represents each shader array in a
// 16-bit half. Hardware remaps harvested CUs: enable the compact low N bits,
// not the physical fuse bitmap. GFX12 enables CUs in complete WGP pairs.
inline bool gfx12_compute_masks(uint32_t engines,uint32_t arrays,uint32_t cuCount,
    const uint32_t (&bitmap)[4][2],uint32_t (&masks)[4]) {
    if (engines!=4 || !arrays || arrays>2 || !cuCount) return false;
    uint32_t result[4]{},total=0;
    for (unsigned se=0;se<4;++se) {
        for (unsigned sh=0;sh<arrays;++sh) {
            const unsigned count=__builtin_popcount(bitmap[se][sh]);
            if (count>16 || (count&1)) return false;
            const uint32_t enabled=(1u<<count)-1;
            result[se]|=enabled<<(sh*16);total+=count;
        }
    }
    if (total!=cuCount) return false;
    for (unsigned se=0;se<4;++se) masks[se]=result[se];
    return true;
}
}
