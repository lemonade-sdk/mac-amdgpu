#pragma once
#include <stdint.h>

namespace amdgpu::GFXRegs {
// GC offsets and segment indices from gc_12_0_0_offset.h.
struct Register { uint32_t offset; uint32_t baseIndex; };
constexpr Register GRBM_CNTL = {0x0da0, 0};
constexpr Register GRBM_GFX_CNTL = {0x0900, 1};
constexpr Register GRBM_GFX_INDEX = {0x2200, 1};
constexpr Register SH_MEM_BASES = {0x09e3, 1};
constexpr Register SH_MEM_CONFIG = {0x09e4, 1};
constexpr Register SPI_GDBG_PER_VMID_CNTL = {0x1f72, 0};
constexpr Register CC_RB_BACKEND_DISABLE = {0x13dd, 0};
constexpr Register CC_GC_SHADER_ARRAY_CONFIG = {0x100f, 0};
constexpr Register GRBM_CC_GC_SA_UNIT_DISABLE = {0x0fe9, 0};
constexpr Register GC_USER_SHADER_ARRAY_CONFIG = {0x5b90, 1};
constexpr Register GRBM_GC_USER_SA_UNIT_DISABLE = {0x5b92, 1};
constexpr Register GC_USER_RB_BACKEND_DISABLE = {0x5b94, 1};
constexpr Register GB_ADDR_CONFIG = {0x13de, 0};
} // namespace amdgpu::GFXRegs
