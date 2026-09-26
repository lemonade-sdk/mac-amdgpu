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

// SQ per-SE/SH performance counters (GC, BASE_IDX 1) — the driver-owned
// SQ-busy source. Mirrors upstream gc_12_0_0_offset.h: each counter's
// SELECT register takes a PERF_SEL event id (writing it starts the counter;
// RDNA has no separate START) and the matching _LO register is the cumulative
// readout. Under a GRBM broadcast select, a read of any one SH returns the sum
// across all SEs — the whole-device "cycles the SQ was busy" number we want.
// Event ids from upstream include/soc24_enum.h: SQG_PERF_SEL_CYCLES = 0x0e
// (total shader cycles, denominator), SQG_PERF_SEL_BUSY_CYCLES = 0x0f (busy
// cycles, numerator). SQ_PERFCOUNTER0/1 are dedicated to this feature and are
// not touched by any other dext path (gfx_get_spec never selects SQ counters).
constexpr Register SQ_PERFCOUNTER0_SELECT = {0x39c0, 1};
constexpr Register SQ_PERFCOUNTER1_SELECT = {0x39c1, 1};
constexpr Register SQ_PERFCOUNTER0_LO = {0x31c0, 1};
constexpr Register SQ_PERFCOUNTER1_LO = {0x31c2, 1};

// SQ_PERFCOUNTERx_SELECT.PERF_SEL is the 9-bit event id in [8:0].
constexpr uint32_t kSqPerfSelBusyCycles = 0x0fu; // SQG_PERF_SEL_BUSY_CYCLES
constexpr uint32_t kSqPerfSelCycles     = 0x0eu; // SQG_PERF_SEL_CYCLES
} // namespace amdgpu::GFXRegs
