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
    // gc_12_0_0_offset.h: regRLC_CSIB_ADDR_LO/_HI/_LENGTH = 0x0987/0x0988/0x0989.
    constexpr uint32_t RLC_CSIB_ADDR_LO         = 0x0987;
    constexpr uint32_t RLC_CSIB_ADDR_HI         = 0x0988;
    constexpr uint32_t RLC_CSIB_LENGTH          = 0x0989;
    // gc_12_0_0_offset.h: regRLC_SRM_CNTL = 0x4c80.
    constexpr uint32_t RLC_SRM_CNTL             = 0x4C80;
    // CP_ME_CNTL, CP_RB0_* etc. live here too — added as we port them.
}

constexpr uint32_t kRLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK
    = 0x80000000u;

// RLC_SRM_CNTL fields — gc_12_0_0_sh_mask.h:20333-20340.
#define RLC_SRM_CNTL__SRM_ENABLE__SHIFT     0x0
#define RLC_SRM_CNTL__SRM_ENABLE_MASK       0x00000001
#define RLC_SRM_CNTL__AUTO_INCR_ADDR__SHIFT 0x1
#define RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK   0x00000002

// Default clear-state buffer size. Upstream computes it from a
// per-asic cs_section_def table; 16 KB is plenty for first PM4.
// gfx12_cs_data (clearstate_gfx12.h:107-113) totals:
//   1 (clustercount)
// + (2 + 34) + (2 + 2) + (2 + 1) + (2 + 6) + (2 + 11) + (2 + 8)
// = 75 dwords (~300 B). 16 KB has plenty of headroom for future
// upstream table extensions.
constexpr uint32_t kRLCClearStateDefaultBytes = 16 * 1024;

// A single contiguous-register cluster in the CSB:
//   - first dword:  reg_count
//   - second dword: reg_index (start register, GC offset)
//   - then reg_count × initial-value dwords.
struct CSExtentDef {
    const uint32_t *extent;  // initial values
    uint32_t        reg_index;
    uint32_t        reg_count;
};

struct CSSectionDef {
    const CSExtentDef *section;  // null-terminated array (extent==nullptr)
    uint32_t           id;       // SECT_CONTEXT = 1, SECT_NONE = 0
};

constexpr uint32_t kCSSection_CONTEXT = 1;
constexpr uint32_t kCSSection_NONE    = 0;

struct RLCContext {
    bool             inited;
    bool             bootload_complete;
    bool             csb_populated;     // gfx12_cs_data filled in?
    bool             csib_programmed;   // RLC_CSIB_ADDR_* written?
    bool             srm_enabled;       // RLC_SRM_CNTL.SRM_ENABLE set?

    // CSB allocation in VRAM (via GMC's VRAM bump allocator).
    VRAMAllocation   clear_state;
    uint32_t         clear_state_dwords;   // total dwords written to CSB

    // Cached VRAM-mapped CSB CPU pointer. Populated by rlc_alloc_csb
    // (BAR0 aperture write target) so rlc_setup_csb_buffer can stream
    // dwords without re-mapping. Real driver path uses
    // amdgpu_bo_kmap; here we use the BAR0 dext aperture writes.
    uint64_t         csb_vram_byte_offset;   // VRAM byte offset of CSB
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
// Fill the allocated CSB with the gfx12_cs_data cluster table.
// Mirrors upstream gfx_v12_0_get_csb_buffer (gfx_v12_0.c:688) +
// gfx_v12_0_init_csb (gfx_v12_0.c:1905). After filling, programs
// regRLC_CSIB_ADDR_HI/_LO and regRLC_CSIB_LENGTH so RLC knows
// where the save/restore table lives.
//
// Audit-7 #7.
//
kern_return_t rlc_setup_csb_buffer(const DeviceContext &dev,
                                   GMCContext &gmc, RLCContext &rlc);

//
// Set RLC_SRM_CNTL.{AUTO_INCR_ADDR, SRM_ENABLE} to enable the
// Save-Restore-Machine. Direct port of
// gfx_v12_0_rlc_enable_srm (gfx_v12_0.c:1967).
//
// Audit-7 #7.
//
kern_return_t rlc_enable_srm(const DeviceContext &dev, RLCContext &rlc);

//
// Top-level RLCInit stage entry. Calls rlc_alloc_csb then
// rlc_wait_for_autoload_complete then rlc_setup_csb_buffer then
// rlc_enable_srm.
//
kern_return_t rlc_init_full(const DeviceContext &dev,
                            GMCContext &gmc, RLCContext &rlc);

} // namespace amdgpu
