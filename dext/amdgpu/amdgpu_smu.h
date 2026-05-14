//
//  amdgpu_smu.h — SMU v14_0_3 mailbox interface.
//
//  Ports the SMU mailbox primitives from upstream Linux:
//      drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c
//        smu_cmn_wait_for_response
//        smu_cmn_send_smc_msg_with_param
//        smu_cmn_send_smc_msg
//
//  Pre-requisites that must be true before SMU mailbox is usable:
//      - PSP SOS loaded (psp_load_sos succeeded)
//      - PSP km_ring created (psp_ring_create succeeded)
//      - PMFW loaded via PSP ring command (not yet ported — Phase 1B
//        next chunks)
//
//  Until PMFW is loaded the SMU silicon doesn't respond to messages.
//  smu_test_message() returns kIOReturnNotReady if MP1 is unresolved
//  and a timeout if SMU never answers.
//

#pragma once

#include <stdint.h>
#include "amdgpu_regs.h"

namespace amdgpu {

//
// smu_send_msg_with_param — port of smu_cmn_send_smc_msg_with_param.
// Synchronous. Returns kIOReturnSuccess if SMU responded with OK
// (0x01), kIOReturnDeviceError for other non-zero responses, and
// kIOReturnTimeout if the SMU never answered.
//
//   msgId      — PPSMC message id (PPSMC::TestMessage, etc.)
//   param      — message parameter; 0 for messages that don't use it
//   outReturn  — out param for SMU's reply payload (read from C2PMSG_82
//                after the response is received); nullable
//
kern_return_t smu_send_msg_with_param(const DeviceContext &dev,
                                      uint32_t msgId,
                                      uint32_t param,
                                      uint32_t *outReturn);

//
// smu_send_msg — convenience wrapper, no parameter, no return read.
//
kern_return_t smu_send_msg(const DeviceContext &dev, uint32_t msgId);

//
// smu_test_message — port of the basic ping. PPSMC::TestMessage with
// param=0xABCD0001; SMU echoes back param+1. Used to verify the
// mailbox is live before any other SMU traffic.
//
kern_return_t smu_test_message(const DeviceContext &dev,
                               uint32_t *outEcho);

//
// smu_get_version — read SMU PMFW version number.
//
kern_return_t smu_get_version(const DeviceContext &dev, uint32_t *outVer);

//
// PPSMC IDs for the driver-table transfer protocol. We use these
// to point SMU at a DRAM-resident copy of the SMU driver table so
// the SMU can DMA tool/config tables in and out of our sysmem.
// Subset of smu_v14_0_2_ppsmc.h — full file is per-ASIC.
//
namespace PPSMCTable {
    constexpr uint32_t SetDriverDramAddrHigh = 0x0E;
    constexpr uint32_t SetDriverDramAddrLow  = 0x0F;
    constexpr uint32_t SetToolsDramAddrHigh  = 0x10;
    constexpr uint32_t SetToolsDramAddrLow   = 0x11;
    constexpr uint32_t TransferTableSmu2Dram = 0x12;
    constexpr uint32_t TransferTableDram2Smu = 0x13;
}

// PSP firmware type id for the pptable blob (GFX_FW_TYPE_PPTABLE).
constexpr uint32_t kPSP_FW_TYPE_PPTABLE = 73;

//
// smu_set_driver_dram_addr — point SMU at a DRAM-resident driver
// table buffer. Caller owns a single DART-mapped sysmem buffer
// (e.g. 64 KB, 16 KB-aligned) and passes its bus address.
// Splits the address across the LOW/HIGH PPSMC messages.
//
// This is the equivalent of upstream's amdgpu_table_setup flow for
// smu14: see smu_v14_0.c smu_v14_0_set_tool_table_location +
// the driver_pptable / driver_table allocations in init_smc_tables.
// We don't allocate the upstream zoo of tables; we expose the
// primitive and let userspace (or a future port chunk) decide which
// tables to upload via TransferTableDram2Smu.
//
kern_return_t smu_set_driver_dram_addr(const DeviceContext &dev,
                                       uint64_t bus_addr);

// Same shape for the SMU tool (telemetry) DRAM area.
kern_return_t smu_set_tools_dram_addr(const DeviceContext &dev,
                                      uint64_t bus_addr);

//
// smu_transfer_table_dram_to_smu — instruct SMU to consume the
// driver table at the current DRAM addr. `table_id` is the SMU's
// internal table index (asic-specific subset of `SMU_TABLE_*`).
//
kern_return_t smu_transfer_table_dram_to_smu(const DeviceContext &dev,
                                             uint32_t table_id);

//
// smu_transfer_table_smu_to_dram — opposite direction. SMU writes
// the current table contents into our DRAM buffer (telemetry
// snapshots, OD readback, etc.).
//
kern_return_t smu_transfer_table_smu_to_dram(const DeviceContext &dev,
                                             uint32_t table_id);

} // namespace amdgpu
