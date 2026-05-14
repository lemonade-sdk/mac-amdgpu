//
//  amdgpu_gfx.h — GFX top-level configuration.
//
//  Currently just covers gfx_v12_0_constants_init — the
//  GRBM/SH_MEM-config writes the CP expects to find programmed
//  before it'll accept queue submissions. Later this header will
//  grow setup_rb, get_cu_info, init_compute_vmid, and other gfx
//  housekeeping as needed.
//
//  Source: drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:1806
//          (gfx_v12_0_constants_init)
//

#pragma once

#include <stdint.h>
#include "amdgpu_ip.h"
#include "amdgpu_regs.h"

namespace amdgpu {

// GC register offsets that belong to top-level GFX init (as opposed
// to the CP HQD registers in amdgpu_cp.h).
namespace GFXRegs {
    constexpr uint32_t GRBM_CNTL        = 0x0DA0;
    constexpr uint32_t GRBM_GFX_CNTL    = 0x0900;
    constexpr uint32_t GRBM_GFX_INDEX   = 0x2200;
    constexpr uint32_t SH_MEM_BASES     = 0x09E3;
    constexpr uint32_t SH_MEM_CONFIG    = 0x09E4;
}

// GRBM_CNTL.READ_TIMEOUT — bits [11:0]. Upstream writes 0xFF as the
// safe default. Larger values give more tolerance for slow MMIO
// returns; 0xFF works on every gen.
constexpr uint32_t kGRBM_CNTL_READ_TIMEOUT_MASK  = 0x00000FFFu;
constexpr uint32_t kGRBM_CNTL_READ_TIMEOUT_VALUE = 0x000000FFu;

// SH_MEM_CONFIG composition — mirrors upstream's DEFAULT_SH_MEM_CONFIG:
//   (SH_MEM_ADDRESS_MODE_64        << ADDRESS_MODE__SHIFT      = 0 << 0)
//   (SH_MEM_ALIGNMENT_MODE_UNALIGNED << ALIGNMENT_MODE__SHIFT = 3 << 2)
//   (3                             << INITIAL_INST_PREFETCH__SHIFT = 3 << 0xE)
//
// Pre-computed: 0xC | 0xC000 = 0xC00C.
constexpr uint32_t kSH_MEM_ADDRESS_MODE_64_SHIFT          = 0x0;
constexpr uint32_t kSH_MEM_ALIGNMENT_MODE_UNALIGNED_SHIFT = 0x2;
constexpr uint32_t kSH_MEM_INITIAL_INST_PREFETCH_SHIFT    = 0xE;
constexpr uint32_t kSH_MEM_ADDRESS_MODE_64                = 0x0;
constexpr uint32_t kSH_MEM_ALIGNMENT_MODE_UNALIGNED       = 0x3;
constexpr uint32_t kSH_MEM_INITIAL_INST_PREFETCH          = 0x3;

constexpr uint32_t kDefaultSHMemConfig =
      (kSH_MEM_ADDRESS_MODE_64           << kSH_MEM_ADDRESS_MODE_64_SHIFT)
    | (kSH_MEM_ALIGNMENT_MODE_UNALIGNED  << kSH_MEM_ALIGNMENT_MODE_UNALIGNED_SHIFT)
    | (kSH_MEM_INITIAL_INST_PREFETCH     << kSH_MEM_INITIAL_INST_PREFETCH_SHIFT);

//
// Port of gfx_v12_0_constants_init (gfx_v12_0.c:1806).
//
// Minimal subset for first PM4:
//   - Set GRBM_CNTL.READ_TIMEOUT = 0xFF
//   - Write SH_MEM_CONFIG = DEFAULT for VMID 0 (kernel-driver vmid)
//   - Zero pa_sc_tile_steering_override (default)
//
// Deferred (not required for a NOP + RELEASE_MEM submit):
//   - gfx_v12_0_setup_rb (render backend harvest map)
//   - gfx_v12_0_get_cu_info (compute unit harvest)
//   - gfx_v12_0_get_tcc_info (texture cache harvest)
//   - per-VMID SH_MEM_BASES (only needed for user-mode queues
//     using non-VMID-0 contexts)
//   - gfx_v12_0_init_compute_vmid (compute-specific)
//
// All deferred items are required once we move beyond the kernel-
// driver test path and add real user-mode queues / compute pipelines.
//
kern_return_t gfx_constants_init(const DeviceContext &dev);

} // namespace amdgpu
