//
//  psp_v14_0.cpp — PSP v14 bootloader port.
//
//  Source: upstream/linux/drivers/gpu/drm/amd/amdgpu/psp_v14_0.c
//          (psp_v14_0_is_sos_alive, psp_v14_0_wait_for_bootloader,
//           psp_v14_0_bootloader_load_sos at lines 94/104/201)
//
//  Translations:
//      RREG32_SOC15(MP0, 0, reg)  → RREG32(dev, SOC15_REG_OFFSET(MP0,reg))
//      WREG32_SOC15(MP0, 0, reg)  → WREG32(dev, SOC15_REG_OFFSET(MP0,reg))
//      psp_wait_for(timeout)      → poll_reg(timeout_us)
//      psp->fw_pri_buf            → psp.fwPriCPUAddr
//      psp->fw_pri_mc_addr        → psp.fwPriBusAddr  (DART iova)
//      mdelay(20)                 → IOSleep(20)
//      kmemset(fw_pri_buf, 0, …)  → bzero(fwPriCPUAddr, …)
//
//  Behavioural deltas vs Linux:
//      - We don't do SR-IOV (skip vf paths in source file).
//      - We don't share PSP across multiple amdgpu_device instances.
//      - We allocate fw_pri ourselves (Linux uses amdgpu_bo_create).
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "amdgpu_psp.h"

#define PSP_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.psp: " fmt, ##__VA_ARGS__)

namespace amdgpu {

kern_return_t
psp_init(DeviceContext &dev, PSPContext &psp)
{
    if (psp.fwPriBuffer != nullptr) {
        return kIOReturnSuccess; // idempotent
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        PSP_LOG("MP0 IP base not resolved — IP discovery missing");
        return kIOReturnNotReady;
    }

    IOBufferMemoryDescriptor *buf = nullptr;
    // AS page size is 16 KB; DART rejects mappings that aren't
    // page-aligned. fw_pri is 1 MB so its size is already aligned.
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, kPSPFwPriBufSize, kASPageSize, &buf);
    if (ret != kIOReturnSuccess || buf == nullptr) {
        PSP_LOG("fw_pri buffer alloc failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ret = buf->SetLength(kPSPFwPriBufSize);
    if (ret != kIOReturnSuccess) {
        buf->release();
        return ret;
    }

    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;

    IODMACommand *cmd = nullptr;
    ret = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                               &spec, &cmd);
    if (ret != kIOReturnSuccess || cmd == nullptr) {
        buf->release();
        PSP_LOG("fw_pri IODMACommand::Create failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    ret = cmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                             buf, 0, kPSPFwPriBufSize,
                             &flags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) {
        if (cmd) cmd->release();
        buf->release();
        PSP_LOG("fw_pri PrepareForDMA failed: %#x segs=%u", ret, segCount);
        return ret != kIOReturnSuccess ? ret : kIOReturnNotAligned;
    }

    IOAddressSegment cpuSeg = {};
    buf->GetAddressRange(&cpuSeg);

    psp.fwPriBuffer     = buf;
    psp.fwPriDMACommand = cmd;
    psp.fwPriBusAddr    = seg.address;
    psp.fwPriCPUAddr    = reinterpret_cast<void *>(cpuSeg.address);
    psp.fwPriSize       = kPSPFwPriBufSize;
    psp.sosAlive        = false;

    PSP_LOG("fw_pri ready cpu=%p bus=%#llx size=%llu",
            psp.fwPriCPUAddr, psp.fwPriBusAddr, psp.fwPriSize);
    return kIOReturnSuccess;
}

void
psp_release(PSPContext &psp)
{
    if (psp.ringDMACommand != nullptr) {
        psp.ringDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.ringDMACommand->release();
        psp.ringDMACommand = nullptr;
    }
    if (psp.ringBuffer != nullptr) {
        psp.ringBuffer->release();
        psp.ringBuffer = nullptr;
    }
    psp.ringCPUAddr = nullptr;
    psp.ringBusAddr = 0;
    psp.ringSize    = 0;
    psp.ringCreated = false;

    if (psp.fwPriDMACommand != nullptr) {
        psp.fwPriDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.fwPriDMACommand->release();
        psp.fwPriDMACommand = nullptr;
    }
    if (psp.fwPriBuffer != nullptr) {
        psp.fwPriBuffer->release();
        psp.fwPriBuffer = nullptr;
    }
    psp.fwPriCPUAddr = nullptr;
    psp.fwPriBusAddr = 0;
    psp.fwPriSize    = 0;
    psp.sosAlive     = false;
}

bool
psp_is_sos_alive(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::MP0)) return false;
    uint32_t sol = RREG32(dev,
        SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_81));
    return sol != 0u;
}

bool
psp_wait_for_bootloader(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::MP0)) return false;
    const uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                          MP0Regs::C2PMSG_35);
    // Linux retries up to 10 times against psp->adev->usec_timeout
    // (default ~100ms each). We do a single 1-second budget.
    const uint64_t kBudgetUs = 1000000;
    uint32_t v = 0;
    bool ok = poll_reg(dev, reg,
                       kPSPBootloaderReadyBit, kPSPBootloaderReadyBit,
                       kBudgetUs, &v);
    if (!ok) {
        PSP_LOG("bootloader wait timeout — C2PMSG_35=%#010x", v);
    }
    return ok;
}

kern_return_t
psp_load_sos(DeviceContext &dev, PSPContext &psp)
{
    if (psp.fwPriBuffer == nullptr || psp.fwPriCPUAddr == nullptr) {
        PSP_LOG("load_sos: psp_init not called");
        return kIOReturnNotReady;
    }
    if (psp.sosFirmware == nullptr || psp.sosFirmwareSize == 0) {
        PSP_LOG("load_sos: SOS firmware not provided");
        return kIOReturnBadArgument;
    }
    if (psp.sosFirmwareSize > psp.fwPriSize) {
        PSP_LOG("load_sos: SOS firmware too large (%llu > %llu)",
                psp.sosFirmwareSize, psp.fwPriSize);
        return kIOReturnNoSpace;
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        PSP_LOG("load_sos: MP0 IP base unresolved");
        return kIOReturnNotReady;
    }

    if (psp_is_sos_alive(dev)) {
        PSP_LOG("load_sos: SOS already alive — skipping");
        psp.sosAlive = true;
        return kIOReturnSuccess;
    }

    if (!psp_wait_for_bootloader(dev)) {
        return kIOReturnTimeout;
    }

    // Stage the SOS image in fw_pri.
    memset(psp.fwPriCPUAddr, 0, psp.fwPriSize);
    memcpy(psp.fwPriCPUAddr, psp.sosFirmware, psp.sosFirmwareSize);

    const uint32_t regAddr = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_36);
    const uint32_t regCmd  = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_35);
    const uint32_t regSOL  = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_81);

    // Capture pre-load SOL value — protocol completes when SOL CHANGES.
    uint32_t solBefore = RREG32(dev, regSOL);

    // PSP expects the buffer address as (mc_addr >> 20).
    WREG32(dev, regAddr, static_cast<uint32_t>(psp.fwPriBusAddr >> 20));
    WREG32(dev, regCmd,  PSPBootloaderCmd::LoadSOSDrv);

    // Linux comments: "there might be handshake issue with hardware
    // which needs delay" → mdelay(20).
    IOSleep(20);

    // Wait for SOL to change from its captured value. We poll with a
    // 5-second budget; SOS bringup is normally well under 1 second.
    const uint64_t kBudgetUs = 5 * 1000000;
    uint32_t v = solBefore;
    uint64_t elapsed = 0;
    const uint64_t kStepUs = 1000;
    while (elapsed < kBudgetUs) {
        v = RREG32(dev, regSOL);
        if (v != solBefore && v != 0) {
            psp.sosAlive = true;
            PSP_LOG("SOS alive — C2PMSG_81 %#010x → %#010x after %llu µs",
                    solBefore, v, elapsed);
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed += kStepUs;
    }

    PSP_LOG("SOS load timeout — C2PMSG_81 stayed %#010x", v);
    return kIOReturnTimeout;
}

kern_return_t
psp_bootloader_load_component(DeviceContext &dev, PSPContext &psp,
                              const uint8_t *bin, uint64_t binSize,
                              uint32_t bl_cmd)
{
    if (psp.fwPriCPUAddr == nullptr) {
        PSP_LOG("load_component(cmd=%#x): psp_init not called", bl_cmd);
        return kIOReturnNotReady;
    }
    if (bin == nullptr || binSize == 0 || binSize > psp.fwPriSize) {
        return kIOReturnBadArgument;
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        return kIOReturnNotReady;
    }

    // If SOS is already alive the bootloader window has closed —
    // these components only matter pre-SOS.
    if (psp_is_sos_alive(dev)) {
        PSP_LOG("load_component(cmd=%#x): SOS already alive — skip", bl_cmd);
        return kIOReturnSuccess;
    }

    if (!psp_wait_for_bootloader(dev)) {
        return kIOReturnTimeout;
    }

    memset(psp.fwPriCPUAddr, 0, psp.fwPriSize);
    memcpy(psp.fwPriCPUAddr, bin, binSize);

    const uint32_t regAddr = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_36);
    const uint32_t regCmd  = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_35);

    WREG32(dev, regAddr, static_cast<uint32_t>(psp.fwPriBusAddr >> 20));
    WREG32(dev, regCmd,  bl_cmd);

    if (!psp_wait_for_bootloader(dev)) {
        PSP_LOG("load_component(cmd=%#x): post-load wait timed out", bl_cmd);
        return kIOReturnTimeout;
    }
    PSP_LOG("load_component(cmd=%#x): ok", bl_cmd);
    return kIOReturnSuccess;
}

kern_return_t
psp_ring_create(DeviceContext &dev, PSPContext &psp)
{
    if (psp.ringCreated) {
        return kIOReturnSuccess;
    }
    if (!psp.sosAlive) {
        PSP_LOG("ring_create: SOS not alive yet");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        return kIOReturnNotReady;
    }

    // PSP wants a 4 KB ring per its protocol; DART on AS needs a
    // 16 KB-aligned + sized backing buffer. Allocate 16 KB, tell PSP
    // the usable area is 4 KB via C2PMSG_71. Trailing 12 KB unused.
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, kPSPKMRingBufSize, kASPageSize, &buf);
    if (ret != kIOReturnSuccess || buf == nullptr) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ret = buf->SetLength(kPSPKMRingBufSize);
    if (ret != kIOReturnSuccess) {
        buf->release();
        return ret;
    }

    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;

    IODMACommand *cmd = nullptr;
    ret = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                               &spec, &cmd);
    if (ret != kIOReturnSuccess || cmd == nullptr) {
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    ret = cmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                             buf, 0, kPSPKMRingBufSize,
                             &flags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) {
        cmd->release();
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNotAligned;
    }
    IOAddressSegment cpuSeg = {};
    buf->GetAddressRange(&cpuSeg);

    psp.ringBuffer      = buf;
    psp.ringDMACommand  = cmd;
    psp.ringBusAddr     = seg.address;
    psp.ringCPUAddr     = reinterpret_cast<void *>(cpuSeg.address);
    psp.ringSize        = kPSPKMRingSize;

    const uint32_t reg64 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_64);
    const uint32_t reg69 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_69);
    const uint32_t reg70 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_70);
    const uint32_t reg71 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_71);

    // 1. Wait for SOS ready to accept ring creation.
    uint32_t v = 0;
    if (!poll_reg(dev, reg64, kPSPMboxRespMask, kPSPMboxRespFlag,
                  5 * 1000000, &v)) {
        PSP_LOG("ring_create: SOS not ready — C2PMSG_64=%#010x", v);
        psp.ringDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.ringDMACommand->release();
        psp.ringDMACommand = nullptr;
        psp.ringBuffer->release();
        psp.ringBuffer = nullptr;
        return kIOReturnTimeout;
    }

    // 2. Program ring address (low + high) + size, then kick.
    WREG32(dev, reg69, static_cast<uint32_t>(psp.ringBusAddr & 0xFFFFFFFFu));
    WREG32(dev, reg70, static_cast<uint32_t>(psp.ringBusAddr >> 32));
    WREG32(dev, reg71, static_cast<uint32_t>(psp.ringSize));
    WREG32(dev, reg64, kPSPRingTypeKM << 16);

    IOSleep(20);

    // 3. Wait for response flag.
    if (!poll_reg(dev, reg64, kPSPMboxRespMask, kPSPMboxRespFlag,
                  5 * 1000000, &v)) {
        PSP_LOG("ring_create: response wait timed out — C2PMSG_64=%#010x", v);
        return kIOReturnTimeout;
    }

    psp.ringCreated = true;
    PSP_LOG("ring_created bus=%#llx size=%llu (response=%#010x)",
            psp.ringBusAddr, psp.ringSize, v);
    return kIOReturnSuccess;
}

} // namespace amdgpu
