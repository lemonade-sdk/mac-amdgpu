#pragma once
#include <stdint.h>

namespace amdgpu::CPRegs {
// gc_12_0_0_offset.h offsets must travel with their GC segment index.
struct Register { uint32_t offset; uint32_t baseIndex; };
constexpr Register CP_RB_WPTR_DELAY = {0x0F61, 0};
constexpr Register CP_RB0_BASE = {0x1DE0, 0};
constexpr Register CP_RB0_CNTL = {0x1DE1, 0};
constexpr Register CP_RB0_RPTR_ADDR = {0x1DE3, 0};
constexpr Register CP_RB0_RPTR_ADDR_HI = {0x1DE4, 0};
constexpr Register CP_DEVICE_ID = {0x1DEB, 0};
constexpr Register CP_RB_VMID = {0x1DF1, 0};
constexpr Register CP_RB0_WPTR = {0x1DF4, 0};
constexpr Register CP_RB0_WPTR_HI = {0x1DF5, 0};
constexpr Register CP_RB_DOORBELL_RANGE_LOWER = {0x1DFA, 0};
constexpr Register CP_RB_DOORBELL_RANGE_UPPER = {0x1DFB, 0};
constexpr Register CP_MEC_DOORBELL_RANGE_LOWER = {0x1DFC, 0};
constexpr Register CP_MEC_DOORBELL_RANGE_UPPER = {0x1DFD, 0};
constexpr Register CP_MAX_CONTEXT = {0x1E4E, 0};
constexpr Register CP_RB0_BASE_HI = {0x1E51, 0};
constexpr Register CP_RB_WPTR_POLL_ADDR_LO = {0x1E8B, 0};
constexpr Register CP_RB_WPTR_POLL_ADDR_HI = {0x1E8C, 0};
constexpr Register CP_RB_DOORBELL_CONTROL = {0x1E8D, 0};
constexpr Register CP_RB_ACTIVE = {0x1F40, 0};
constexpr Register CP_ME_CNTL = {0x0803, 1};
constexpr Register CP_MEC_RS64_CNTL = {0x2904, 1};
constexpr Register GRBM_GFX_CNTL = {0x0900, 1};
constexpr Register CP_STAT = {0x0f40, 0};
constexpr Register CP_RB0_RPTR = {0x0f60, 0};
constexpr Register CP_PFP_PRGRM_CNTR_START = {0x1e44, 0};
constexpr Register CP_PFP_PRGRM_CNTR_START_HI = {0x1e59, 0};
constexpr Register CP_ME_PRGRM_CNTR_START = {0x1e45, 0};
constexpr Register CP_ME_PRGRM_CNTR_START_HI = {0x1e79, 0};
constexpr Register CP_MEC_RS64_PRGRM_CNTR_START = {0x2900, 1};
constexpr Register CP_MEC_RS64_PRGRM_CNTR_START_HI = {0x2938, 1};
constexpr Register GCVM_L2_PROTECTION_FAULT_STATUS_LO32 = {0x15d0, 0};
constexpr Register GCVM_L2_PROTECTION_FAULT_STATUS_HI32 = {0x15d1, 0};
constexpr Register GCVM_L2_PROTECTION_FAULT_ADDR_LO32 = {0x15d2, 0};
constexpr Register GCVM_L2_PROTECTION_FAULT_ADDR_HI32 = {0x15d3, 0};
constexpr Register SCRATCH_REG0 = {0x2040, 1};
constexpr Register CP_CPC_STATUS = {0x0e24, 0};
constexpr Register CP_PFP_INSTR_PNTR = {0x0f45, 0};
constexpr Register CP_ME_INSTR_PNTR = {0x0f46, 0};
constexpr Register CP_GFX_HQD_ACTIVE = {0x1e80, 0};
constexpr Register CP_GFX_HQD_VMID = {0x1e81, 0};
constexpr Register CP_GFX_HQD_BASE = {0x1e86, 0};
constexpr Register CP_GFX_HQD_BASE_HI = {0x1e87, 0};
constexpr Register CP_GFX_HQD_RPTR = {0x1e88, 0};
constexpr Register CP_GFX_HQD_WPTR = {0x1e91, 0};
constexpr Register CP_GFX_HQD_WPTR_HI = {0x1e92, 0};
constexpr Register CP_GFX_HQD_MAPPED = {0x1e94, 0};
constexpr Register CP_GFX_RS64_INSTR_PNTR0 = {0x2a44, 1};
constexpr Register CP_GFX_RS64_INSTR_PNTR1 = {0x2a45, 1};
} // namespace amdgpu::CPRegs
