//
//  amdgpu_rlc.h — RLC (RISC-LC) subsystem for GFX12 (RDNA4).
//
//  RLC is the run-load-and-clear engine that owns the per-context
//  shader engine state, power gating, and clock gating sequencing.
//  In Linux it's split between amdgpu_rlc.c (CSB allocation) and
//  gfx_v12_0.c (RLC start/wait/microcode load).
//
//  For "Hello PM4" the minimum we need:
//      1. Allocate the clear-state buffer (CSB) in VRAM.
//      2. Wait for the RLC microcode autoload chain to complete
//         (the PSP loads RLC + PFP + ME + MEC + MES microcode in
//         sequence; we poll regRLC_RLCS_BOOTLOAD_STATUS bit31).
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c
//      drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c
//      docs/port_plans/HELLO_PM4.md (steps 1-3)
//

#pragma once

#include <stdint.h>
#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_vram.h"

namespace amdgpu {

struct GMCContext;   // forward — we need its vram_alloc

// GC register offsets (gc_12_0_0_offset.h subset).
namespace GCRegs {
    constexpr uint32_t CP_STAT                  = 0x0F40;
    constexpr uint32_t RLC_RLCS_BOOTLOAD_STATUS = 0x4E7C;
    // CP_ME_CNTL, CP_RB0_* etc. live here too — added as we port them.
}

constexpr uint32_t kRLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK
    = 0x80000000u;

// Default clear-state buffer size. Upstream computes it from a
// per-asic cs_section_def table; 16 KB is plenty for first PM4
// (the actual GFX12 CSB on R9700 is ~3-4 KB based on Linux's
// gfx12_cs_data table). Bump if we add power gating later.
constexpr uint32_t kRLCClearStateDefaultBytes = 16 * 1024;

struct RLCContext {
    bool             inited;
    bool             bootload_complete;

    // CSB allocation in VRAM (via GMC's VRAM bump allocator).
    VRAMAllocation   clear_state;
    uint32_t         clear_state_dwords;
};

//
// Allocate the clear-state buffer in VRAM. Returns kIOReturnSuccess
// or kIOReturnNoMemory if the VRAM allocator is empty.
//
kern_return_t rlc_alloc_csb(GMCContext &gmc, RLCContext &rlc);

//
// Poll regCP_STAT == 0 && regRLC_RLCS_BOOTLOAD_STATUS bit 31 == 1.
// Linux uses a per-microsecond loop with adev->usec_timeout (default
// 100 ms). We use 5 seconds since cold-boot autoload can be slow.
//
kern_return_t rlc_wait_for_autoload_complete(const DeviceContext &dev,
                                             RLCContext &rlc);

//
// Top-level RLCInit stage entry. Calls rlc_alloc_csb then
// rlc_wait_for_autoload_complete.
//
kern_return_t rlc_init_full(const DeviceContext &dev,
                            GMCContext &gmc, RLCContext &rlc);

} // namespace amdgpu
