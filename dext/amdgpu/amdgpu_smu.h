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

} // namespace amdgpu
