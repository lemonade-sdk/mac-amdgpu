//
//  smu_v14_0.cpp — SMU v14_0_3 mailbox primitives.
//
//  Source: upstream/linux/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c
//      smu_cmn_wait_for_response          (line ~125)
//      smu_cmn_send_smc_msg_with_param    (line ~162)
//      smu_cmn_send_smc_msg               (line ~186)
//
//  Note: real Linux path runs through a msg_ctl abstraction
//  (smu_msg_v1_send_msg / smu_msg_v1_wait_response) so different
//  SMU generations can share code. We collapse to direct register
//  pokes here because we only target SMU v14_0_3.
//

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include "amdgpu_smu.h"

#define SMU_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.smu: " fmt, ##__VA_ARGS__)

namespace amdgpu {

//
// smu_wait_for_response — port of smu_cmn_wait_for_response.
// Polls C2PMSG_90 until non-zero or timeout. Returns the latched
// response value via *outResp (zero on timeout).
//
static bool
smu_wait_for_response(const DeviceContext &dev, uint32_t *outResp)
{
    if (!dev.ip.isResolved(IPBlock::MP1)) {
        if (outResp) *outResp = 0;
        return false;
    }
    const uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::MP1,
                                          MP1Regs::C2PMSG_90);
    // Linux uses adev->usec_timeout, default ~1 second.
    const uint64_t kBudgetUs = 1 * 1000000;
    uint32_t v = 0;
    bool ok = poll_reg(dev, reg, 0xFFFFFFFFu, 0u, /*invert below*/
                       0, &v);
    (void)ok;
    // poll_reg's mask/expected semantics don't fit "wait for non-zero"
    // — do it explicitly instead.
    uint32_t cur = 0;
    uint64_t elapsed = 0;
    const uint64_t kStep = 1000;  // 1 ms
    while (elapsed < kBudgetUs) {
        cur = RREG32(dev, reg);
        if (cur != 0) {
            if (outResp) *outResp = cur;
            return true;
        }
        IOSleep(1);
        elapsed += kStep;
    }
    if (outResp) *outResp = 0;
    return false;
}

kern_return_t
smu_send_msg_with_param(const DeviceContext &dev,
                        uint32_t msgId, uint32_t param,
                        uint32_t *outReturn)
{
    if (!dev.ip.isResolved(IPBlock::MP1)) {
        SMU_LOG("MP1 IP base not resolved");
        return kIOReturnNotReady;
    }

    const uint32_t regMsg   = SOC15_REG_OFFSET(dev, IPBlock::MP1,
                                               MP1Regs::C2PMSG_66);
    const uint32_t regParam = SOC15_REG_OFFSET(dev, IPBlock::MP1,
                                               MP1Regs::C2PMSG_82);
    const uint32_t regResp  = SOC15_REG_OFFSET(dev, IPBlock::MP1,
                                               MP1Regs::C2PMSG_90);

    // 1. Clear any stale response.
    WREG32(dev, regResp, 0);
    // 2. Stage the parameter.
    WREG32(dev, regParam, param);
    // 3. Kick.
    WREG32(dev, regMsg, msgId);

    // 4. Wait for SMU to write a non-zero response.
    uint32_t resp = 0;
    if (!smu_wait_for_response(dev, &resp)) {
        SMU_LOG("msg=%#x param=%#x timeout (no response)", msgId, param);
        return kIOReturnTimeout;
    }

    // 5. Read return value if caller asked.
    if (outReturn != nullptr) {
        *outReturn = RREG32(dev, regParam);
    }

    if (resp != SMUResp::OK) {
        SMU_LOG("msg=%#x param=%#x resp=%#x (not OK)", msgId, param, resp);
        // Translate to a useful errno-ish thing.
        switch (resp) {
        case SMUResp::Failed:           return kIOReturnError;
        case SMUResp::UnknownCmd:       return kIOReturnUnsupported;
        case SMUResp::CmdRejectedPrereq:return kIOReturnNotReady;
        case SMUResp::CmdRejectedBusy:  return kIOReturnBusy;
        default:                        return kIOReturnInternalError;
        }
    }
    return kIOReturnSuccess;
}

kern_return_t
smu_send_msg(const DeviceContext &dev, uint32_t msgId)
{
    return smu_send_msg_with_param(dev, msgId, 0, nullptr);
}

kern_return_t
smu_test_message(const DeviceContext &dev, uint32_t *outEcho)
{
    // SMU echoes back param+1.
    const uint32_t param = 0xABCD0001u;
    uint32_t echoed = 0;
    kern_return_t ret = smu_send_msg_with_param(dev, PPSMC::TestMessage,
                                                param, &echoed);
    if (outEcho) *outEcho = echoed;
    if (ret != kIOReturnSuccess) return ret;
    if (echoed != param + 1) {
        SMU_LOG("test_message: bad echo (sent %#x got %#x)",
                param, echoed);
        return kIOReturnInternalError;
    }
    SMU_LOG("test_message: ok (echo %#x)", echoed);
    return kIOReturnSuccess;
}

kern_return_t
smu_get_version(const DeviceContext &dev, uint32_t *outVer)
{
    uint32_t v = 0;
    kern_return_t ret = smu_send_msg_with_param(dev, PPSMC::GetSmuVersion,
                                                0, &v);
    if (outVer) *outVer = v;
    if (ret == kIOReturnSuccess) {
        SMU_LOG("smu_version: %u.%u.%u.%u",
                (v >> 24) & 0xFF, (v >> 16) & 0xFF,
                (v >>  8) & 0xFF,  v        & 0xFF);
    }
    return ret;
}

} // namespace amdgpu
