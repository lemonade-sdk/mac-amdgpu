#pragma once
#include <stdint.h>

namespace amdgpu::MESRegs {

// GC offsets and segment indices from gc_12_0_0_offset.h.
struct Register { uint32_t offset; uint32_t baseIndex; };
constexpr Register GRBM_GFX_CNTL = {0x0900, 1};
constexpr Register CP_MES_PRGRM_CNTR_START = {0x2800, 1};
constexpr Register CP_MES_INTR_ROUTINE_START = {0x2801, 1};
constexpr Register CP_MES_CNTL = {0x2807, 1};
constexpr Register CP_MES_PIPE0_PRIORITY = {0x2809, 1};
constexpr Register CP_MES_INSTR_PNTR = {0x2813, 1};
constexpr Register CP_MES_MSCRATCH_HI = {0x2814, 1};
constexpr Register CP_MES_MSCRATCH_LO = {0x2815, 1};
constexpr Register CP_MES_PRGRM_CNTR_START_HI = {0x289d, 1};
constexpr Register CP_MES_IC_BASE_LO = {0x5850, 1};
constexpr Register CP_MES_IC_BASE_HI = {0x5851, 1};
constexpr Register CP_MES_DC_BASE_LO = {0x5854, 1};
constexpr Register CP_MES_DC_BASE_HI = {0x5855, 1};
constexpr Register CP_MQD_BASE_ADDR = {0x1fa9, 0};
constexpr Register CP_MQD_BASE_ADDR_HI = {0x1faa, 0};
constexpr Register CP_HQD_ACTIVE = {0x1fab, 0};
constexpr Register CP_HQD_VMID = {0x1fac, 0};
constexpr Register CP_HQD_PERSISTENT_STATE = {0x1fad, 0};
constexpr Register CP_HQD_PQ_BASE = {0x1fb1, 0};
constexpr Register CP_HQD_PQ_BASE_HI = {0x1fb2, 0};
constexpr Register CP_HQD_PQ_RPTR = {0x1fb3, 0};
constexpr Register CP_HQD_PQ_RPTR_REPORT_ADDR = {0x1fb4, 0};
constexpr Register CP_HQD_PQ_RPTR_REPORT_ADDR_HI = {0x1fb5, 0};
constexpr Register CP_HQD_PQ_WPTR_POLL_ADDR = {0x1fb6, 0};
constexpr Register CP_HQD_PQ_WPTR_POLL_ADDR_HI = {0x1fb7, 0};
constexpr Register CP_HQD_PQ_DOORBELL_CONTROL = {0x1fb8, 0};
constexpr Register CP_HQD_PQ_CONTROL = {0x1fba, 0};
constexpr Register CP_HQD_EOP_BASE_ADDR = {0x1fce, 0};
constexpr Register CP_HQD_EOP_BASE_ADDR_HI = {0x1fcf, 0};
constexpr Register CP_HQD_EOP_CONTROL = {0x1fd0, 0};
constexpr Register CP_HQD_PQ_WPTR_LO = {0x1fdf, 0};
constexpr Register CP_HQD_PQ_WPTR_HI = {0x1fe0, 0};
constexpr Register CP_MQD_CONTROL = {0x1fcb, 0};
constexpr Register CP_HQD_GFX_CONTROL = {0x1e9f, 0};
constexpr Register CP_UNMAPPED_DOORBELL = {0x0880, 1};
constexpr Register CP_MES_DOORBELL_CONTROL1 = {0x283c, 1};
constexpr Register CP_MES_DOORBELL_CONTROL2 = {0x283d, 1};
constexpr Register CP_MES_DOORBELL_CONTROL3 = {0x283e, 1};
constexpr Register CP_MES_DOORBELL_CONTROL4 = {0x283f, 1};
constexpr Register CP_MES_DOORBELL_CONTROL5 = {0x2840, 1};
constexpr Register RLC_CP_SCHEDULERS = {0x098A, 1};
constexpr Register CP_MES_MSCRATCH_LO_OFFSET = {0x2815, 1};
constexpr Register CP_MES_MSCRATCH_HI_OFFSET = {0x2814, 1};
constexpr Register CP_MES_GP3_LO = {0x2849, 1};

constexpr Register GCVM_L2_PROTECTION_FAULT_STATUS_LO32 = {0x15d0, 0};
constexpr Register GCVM_L2_PROTECTION_FAULT_STATUS_HI32 = {0x15d1, 0};
constexpr Register GCVM_L2_PROTECTION_FAULT_ADDR_LO32 = {0x15d2, 0};
constexpr Register GCVM_L2_PROTECTION_FAULT_ADDR_HI32 = {0x15d3, 0};

} // namespace amdgpu::MESRegs
