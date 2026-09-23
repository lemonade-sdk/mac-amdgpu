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
#include "amdgpu_psp.h"   // PSPContext (for fwBuf bump allocator in smc_hw_setup)

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
    if (!dev.ip.isResolved(IPBlock::MP1, /*baseIdx=*/1)) {
        if (outResp) *outResp = 0;
        return false;
    }
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                               MP1Regs::C2PMSG_90);
    // Upstream uses `adev->usec_timeout * 20 = 100 ms * 20 = 2 s` here.
    const uint64_t kBudgetUs = 2 * 1000000;
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    while (true) {
        const uint32_t cur = RREG32(dev, reg);
        if (outResp) *outResp = cur;
        if (cur == UINT32_MAX) return false;
        if (cur != 0) return true;
        if ((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000 >= kBudgetUs)
            return false;
        IOSleep(1);
    }
}

kern_return_t
smu_send_msg_with_param(const DeviceContext &dev,
                        uint32_t msgId, uint32_t param,
                        uint32_t *outReturn)
{
    // SMU mailbox registers `regMP1_SMN_C2PMSG_*` declare BASE_IDX 1 in
    // upstream mp_14_0_2_offset.h — NOT BASE_IDX 0. Using base[0] (the
    // historical default) routes the writes to a completely different
    // physical register and SMU never responds. Confirmed against
    // upstream `smu_v14_0_send_msg_with_param`.
    if (!dev.ip.isResolved(IPBlock::MP1, /*baseIdx=*/1)) {
        SMU_LOG("MP1 BASE_IDX 1 not resolved — SMU mailbox unreachable");
        return kIOReturnNotReady;
    }

    const uint32_t regMsg   = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                                    MP1Regs::C2PMSG_66);
    const uint32_t regParam = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                                    MP1Regs::C2PMSG_82);
    const uint32_t regResp  = SOC15_REG_OFFSET_BIDX(dev, IPBlock::MP1, 1,
                                                    MP1Regs::C2PMSG_90);

    // An earlier timed-out command still owns the mailbox. Do not replace
    // its parameter/message until firmware acknowledges it (Linux pre-poll).
    // The initial message skips this check, like SMU_FW_INIT in Linux.
    uint32_t previous = 0;
    if (dev.smuMessagePending && !smu_wait_for_response(dev, &previous))
        return previous == UINT32_MAX ? kIOReturnNotAttached : kIOReturnTimeout;
    dev.smuMessagePending = true;

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
        return resp == UINT32_MAX ? kIOReturnNotAttached : kIOReturnTimeout;
    }
    dev.smuMessagePending = false;

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
    if (outReturn) *outReturn = RREG32(dev, regParam);
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

//
// SMU table-transfer wrappers — see amdgpu_smu.h for shape.
// Linux equivalents live in smu_v14_0.c (smu_v14_0_set_driver_table_location,
// smu_v14_0_set_tool_table_location, smu_v14_0_transfer_table_*).
//
kern_return_t
smu_set_driver_dram_addr(const DeviceContext &dev, uint64_t bus_addr)
{
    kern_return_t r = smu_send_msg_with_param(
        dev, PPSMCTable::SetDriverDramAddrHigh,
        static_cast<uint32_t>(bus_addr >> 32), nullptr);
    if (r != kIOReturnSuccess) return r;
    return smu_send_msg_with_param(
        dev, PPSMCTable::SetDriverDramAddrLow,
        static_cast<uint32_t>(bus_addr & 0xFFFFFFFFu), nullptr);
}

kern_return_t
smu_set_tools_dram_addr(const DeviceContext &dev, uint64_t bus_addr)
{
    kern_return_t r = smu_send_msg_with_param(
        dev, PPSMCTable::SetToolsDramAddrHigh,
        static_cast<uint32_t>(bus_addr >> 32), nullptr);
    if (r != kIOReturnSuccess) return r;
    return smu_send_msg_with_param(
        dev, PPSMCTable::SetToolsDramAddrLow,
        static_cast<uint32_t>(bus_addr & 0xFFFFFFFFu), nullptr);
}

kern_return_t
smu_transfer_table_dram_to_smu(const DeviceContext &dev, uint32_t table_id)
{
    return smu_send_msg_with_param(dev,
                                   PPSMCTable::TransferTableDram2Smu,
                                   table_id, nullptr);
}

kern_return_t
smu_transfer_table_smu_to_dram(const DeviceContext &dev, uint32_t table_id)
{
    return smu_send_msg_with_param(dev,
                                   PPSMCTable::TransferTableSmu2Dram,
                                   table_id, nullptr);
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

//============================================================
// smu_smc_hw_setup — minimal port of upstream smu_smc_hw_setup
// (amdgpu_smu.c:1662). v0.1.20 hypothesis: PMFW must enable DPM
// features for the IMU autoload state machine to fire after
// AUTOLOAD_RLC. PSP-side LOAD_IP_FW for SMU brings PMFW up; this
// function then completes the SMU<->driver handshake.
//
// Sequence (matches upstream order, minimal subset):
//   1. GetDriverIfVersion         — sanity-check IF version.
//   2. SetDriverDramAddrHigh+Low  — point SMU at a 64 KB driver_table
//                                    region (VRAM, reached via GMC).
//   3. RunDcBtc                   — boot-time calibration.
//   4. SetAllowedFeaturesMaskLow  — 0xFFFFFFFF
//      SetAllowedFeaturesMaskHigh — 0xFFFFFFFF
//   5. EnableAllSmuFeatures       — master DPM enable.
//   6. GetRunningSmuFeaturesLow+High — log what came up.
//
// Driver-table allocation: bump 64 KB out of psp.fwBuf (VRAM slot
// allocator already used by ASD + LOAD_IP_FW per-payload staging).
// SMU is on-die and reads via the same GMC PSP uses — the VRAM
// MC address resolves.
//============================================================

kern_return_t
smu_smc_hw_setup(DeviceContext &dev, PSPContext &psp)
{
    constexpr uint64_t kDriverTableSize  = 0x10000;   // 64 KB
    constexpr uint64_t kFwBufAlign       = 0x1000;    // PAGE_SIZE

    // 1. Sanity-check the SMU driver IF version. Upstream logs but does
    //    NOT abort on version mismatch on most chips — we mirror that.
    {
        uint32_t if_ver = 0;
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::GetDriverIfVersion, 0, &if_ver);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: GetDriverIfVersion FAILED kr=%#x", r);
            return r;
        }
        SMU_LOG("smc_hw_setup: SMC IF version = %#x", if_ver);
    }

    // 2. Allocate driver_table from psp.fwBuf (VRAM slot allocator).
    //    SMU stores tool/metric/dpm tables here when we request transfers.
    //    Upstream uses amdgpu_bo_create_kernel for this; we reuse our
    //    existing per-payload bump allocator for the same VRAM region.
    if (psp.fwBufSize == 0) {
        SMU_LOG("smc_hw_setup: psp.fwBuf not initialized");
        return kIOReturnNotReady;
    }
    uint64_t slot_off = psp.fwBufBumpOffset;
    uint64_t slot_sz  = (kDriverTableSize + kFwBufAlign - 1) & ~(kFwBufAlign - 1);
    if (slot_off + slot_sz > psp.fwBufSize) {
        SMU_LOG("smc_hw_setup: fwBuf exhausted (want %llu @ %llu, cap %llu)",
                slot_sz, slot_off, psp.fwBufSize);
        return kIOReturnNoSpace;
    }
    uint64_t driver_table_mc = psp.fwBufBaseMC + slot_off;
    psp.fwBufBumpOffset = slot_off + slot_sz;

    SMU_LOG("smc_hw_setup: driver_table @ mc=%#llx size=%llu",
            driver_table_mc, slot_sz);

    // 3. Send SetDriverDramAddrHigh + Low. Upstream calls these
    //    unconditionally in smu_v14_0_set_driver_table_location (line 641).
    {
        uint32_t hi = static_cast<uint32_t>(driver_table_mc >> 32);
        uint32_t lo = static_cast<uint32_t>(driver_table_mc & 0xFFFFFFFFu);
        kern_return_t r;

        r = smu_send_msg_with_param(dev, PPSMC::SetDriverDramAddrHigh, hi, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetDriverDramAddrHigh(%#x) FAILED kr=%#x", hi, r);
            return r;
        }
        r = smu_send_msg_with_param(dev, PPSMC::SetDriverDramAddrLow, lo, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetDriverDramAddrLow(%#x) FAILED kr=%#x", lo, r);
            return r;
        }
        SMU_LOG("smc_hw_setup: SetDriverDramAddr ok (hi=%#x lo=%#x)", hi, lo);
    }

    // 3.5 v0.1.29 — UseDefaultPPTable. The full pptable-from-VBIOS
    //     parser is documented as a follow-up; for now we ask PMFW to
    //     fall back to the IFWI-baked default powerplay table. This is
    //     what populates the fan curve + chip-specific DPM tables that
    //     PMFW otherwise leaves zeroed (which is why the fan defaults
    //     to MAX after EnableAllSmuFeatures).
    //
    //     Best-effort like SetAllowedFeaturesMask{Low,High}: if this
    //     PMFW build doesn't expose the message (UnknownCmd / 0xFE),
    //     log and continue. RunDcBtc still runs; the chip still boots.
    {
        kern_return_t r = smu_send_msg(dev, PPSMC::UseDefaultPPTable);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: UseDefaultPPTable non-fatal kr=%#x — "
                    "PMFW may already have applied its IFWI default. "
                    "Full VBIOS pptable parse is deferred.", r);
        } else {
            SMU_LOG("smc_hw_setup: UseDefaultPPTable ok — IFWI default "
                    "pptable applied");
        }
    }

    // 4. RunDcBtc — boot-time calibration. Upstream: smu_v14_0.c:1558.
    //    No parameter; SMUResp::OK (1) acknowledges success.
    {
        kern_return_t r = smu_send_msg(dev, PPSMC::RunDcBtc);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: RunDcBtc FAILED kr=%#x", r);
            return r;
        }
        SMU_LOG("smc_hw_setup: RunDcBtc ok");
    }

    // 4.5 v0.1.29 — NotifyPowerSource(AC). Upstream amdgpu_smu.c:1662
    //     smu_smc_hw_setup calls smu_notify_display_change /
    //     smu_set_power_source after RunDcBtc with the current power
    //     source. POWER_SOURCE_AC is 0 in smu14_driver_if_v14_0.h. Some
    //     PMFW builds don't expose this message — same non-fatal pattern
    //     as SetAllowedFeaturesMask{Low,High} above.
    {
        kern_return_t r = smu_send_msg_with_param(
            dev, PPSMC::NotifyPowerSource, /*POWER_SOURCE_AC*/ 0, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: NotifyPowerSource(AC) non-fatal kr=%#x", r);
        } else {
            SMU_LOG("smc_hw_setup: NotifyPowerSource(AC) ok");
        }
    }

    // 5. Set allowed features mask. v0.1.20 test result: PMFW returns
    //    UnknownCmd (0xFE) for both SetAllowedFeaturesMaskLow and
    //    SetAllowedFeaturesMaskHigh on this firmware build, even though
    //    upstream smu_v14_0_2_ppt.c's message map registers them.
    //
    //    Theory: the deployed SMU 14.0.3 PMFW (version 0.104.76.0) uses
    //    a baked-in default allow-mask from IFWI and doesn't expose the
    //    runtime mask-set messages. We skip these and try
    //    EnableAllSmuFeatures directly — if PMFW honors its IFWI default,
    //    DPM features still come up.
    //
    //    Best-effort: log the failure but don't abort. Re-evaluate if
    //    EnableAllSmuFeatures also fails.
    {
        kern_return_t r;
        r = smu_send_msg_with_param(dev, PPSMC::SetAllowedFeaturesMaskLow,
                                    0xFFFFFFFFu, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetAllowedFeaturesMaskLow non-fatal "
                    "kr=%#x — proceeding to EnableAllSmuFeatures with "
                    "PMFW default mask", r);
        }
        r = smu_send_msg_with_param(dev, PPSMC::SetAllowedFeaturesMaskHigh,
                                    0xFFFFFFFFu, nullptr);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: SetAllowedFeaturesMaskHigh non-fatal "
                    "kr=%#x", r);
        }
    }

    // 6. EnableAllSmuFeatures — THE master DPM switch.
    //    Upstream calls smu_system_features_control(smu, true), which
    //    sends this message. After this, PMFW starts driving GFX/SOC
    //    clocks out of bootup-idle.
    //
    //    This is the message we MOST want to succeed. If PMFW also
    //    returns UnknownCmd here, we'd know feature control is wholly
    //    PMFW-internal on this chip and the autoload state machine must
    //    be unblocked by some other means.
    {
        kern_return_t r = smu_send_msg(dev, PPSMC::EnableAllSmuFeatures);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: EnableAllSmuFeatures FAILED kr=%#x — "
                    "if UnknownCmd, PMFW feature control is autoload-"
                    "internal on this chip", r);
            return r;
        } else {
            SMU_LOG("smc_hw_setup: EnableAllSmuFeatures ok");
        }
    }

    // 7. Read back which features actually came online. Pure diagnostic
    //    — upstream stores into smu->smu_feature.supported_bits, we just
    //    log. Failures here are non-fatal (some old PMFW silently drops
    //    GetRunningSmuFeatures*).
    {
        uint32_t lo = 0, hi = 0;
        kern_return_t r;
        r = smu_send_msg_with_param(dev, PPSMC::GetRunningSmuFeaturesLow,
                                    0, &lo);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: GetRunningSmuFeaturesLow FAILED kr=%#x — "
                    "(non-fatal)", r);
        }
        r = smu_send_msg_with_param(dev, PPSMC::GetRunningSmuFeaturesHigh,
                                    0, &hi);
        if (r != kIOReturnSuccess) {
            SMU_LOG("smc_hw_setup: GetRunningSmuFeaturesHigh FAILED kr=%#x — "
                    "(non-fatal)", r);
        }
        SMU_LOG("smc_hw_setup: running features = %#x_%#x", hi, lo);
    }

    SMU_LOG("smc_hw_setup: ok");
    return kIOReturnSuccess;
}

} // namespace amdgpu
