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
}
