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

// --- PSP GFX ring frame format ---
// Lays out exactly upstream `struct psp_gfx_rb_frame` (64 bytes) so
// the PSP firmware reads our writes without re-interpretation.
struct PSPGfxRBFrame {
    uint32_t cmd_buf_addr_lo;
    uint32_t cmd_buf_addr_hi;
    uint32_t cmd_buf_size;
    uint32_t fence_addr_lo;
    uint32_t fence_addr_hi;
    uint32_t fence_value;
    uint32_t sid_lo;
    uint32_t sid_hi;
    uint8_t  vmid;
    uint8_t  frame_type;
    uint8_t  reserved1[2];
    uint32_t reserved2[7];
};
static_assert(sizeof(PSPGfxRBFrame) == 64, "PSPGfxRBFrame must be 64 B");

constexpr uint32_t kPSPGfxCmdRespSize = 1024;  // upstream psp_gfx_cmd_resp
constexpr uint32_t kPSPFenceBufSize   = 16384; // AS page-aligned (only 4 B used)
constexpr uint32_t kPSPCmdBufSize     = 16384; // AS page-aligned (only 1 KB used)
constexpr uint32_t kPSPTMRDefaultSize = 4 * 1024 * 1024;  // 4 MB

// Layout of upstream union psp_gfx_commands embedded inside
// psp_gfx_cmd_resp at offset 28 (after buf_size/version/cmd_id +
// the 4 RBI-only response fields). Only the variants we use are
// modeled here; PSP doesn't care about the union slack.
struct PSPGfxCmdSetupTmr {
    uint32_t buf_phy_addr_lo;
    uint32_t buf_phy_addr_hi;
    uint32_t buf_size;
    uint32_t tmr_flags;         // bit0=sriov, bit1=virt_phy_addr (we set this)
    uint32_t system_phy_addr_lo;
    uint32_t system_phy_addr_hi;
};

struct PSPGfxCmdLoadIpFw {
    uint32_t fw_phy_addr_lo;
    uint32_t fw_phy_addr_hi;
    uint32_t fw_size;
    uint32_t fw_type;           // enum psp_gfx_fw_type (host-endian u32)
};

// Layout of the cmd_resp buffer's leading fields. Matches upstream
// `struct psp_gfx_cmd_resp` for the bits we actually write.
struct PSPGfxCmdRespHeader {
    uint32_t buf_size;
    uint32_t buf_version;       // PSP_GFX_CMD_BUF_VERSION = 1
    uint32_t cmd_id;
    uint32_t resp_buf_addr_lo;  // 0 for GPCOM ring
    uint32_t resp_buf_addr_hi;  // 0
    uint32_t resp_offset;       // 0
    uint32_t resp_buf_size;     // 0
    // cmd union starts at offset 28
};
constexpr uint32_t kPSPGfxCmdBufVersion = 1;

void
psp_release(PSPContext &psp)
{
    if (psp.tmrDMACommand != nullptr) {
        psp.tmrDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.tmrDMACommand->release();
        psp.tmrDMACommand = nullptr;
    }
    if (psp.tmrBuffer != nullptr) {
        psp.tmrBuffer->release();
        psp.tmrBuffer = nullptr;
    }
    psp.tmrCPUAddr = nullptr;
    psp.tmrBusAddr = 0;
    psp.tmrSize    = 0;
    psp.tmrSetUp   = false;

    if (psp.fenceDMACommand != nullptr) {
        psp.fenceDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.fenceDMACommand->release();
        psp.fenceDMACommand = nullptr;
    }
    if (psp.fenceBuffer != nullptr) {
        psp.fenceBuffer->release();
        psp.fenceBuffer = nullptr;
    }
    psp.fenceCPUAddr = nullptr;
    psp.fenceBusAddr = 0;
    if (psp.cmdDMACommand != nullptr) {
        psp.cmdDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.cmdDMACommand->release();
        psp.cmdDMACommand = nullptr;
    }
    if (psp.cmdBuffer != nullptr) {
        psp.cmdBuffer->release();
        psp.cmdBuffer = nullptr;
    }
    psp.cmdCPUAddr = nullptr;
    psp.cmdBusAddr = 0;

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
    //
    // When the GPU is warm-booting and SOS is already alive (e.g.
    // we re-loaded the dext without power-cycling the card), the
    // C2PMSG_64 mailbox is in the post-command state from whatever
    // ran before us, not the SOS-idle state. The "ready" check then
    // never fires and we wait the full timeout. Short-circuit it
    // when sosAlive is already set — SOS is always idle/ready once
    // it's running.
    uint32_t v = 0;
    if (!psp.sosAlive) {
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
    } else {
        PSP_LOG("ring_create: SOS already alive — skipping ready check");
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

    // Allocate cmd + fence buffers — both DART-mapped, 16 KB aligned.
    // The fence is just a 4 B word that PSP writes when a command
    // completes; the cmd buffer holds one psp_gfx_cmd_resp at a time.
    {
        IOBufferMemoryDescriptor *cb = nullptr;
        ret = IOBufferMemoryDescriptor::Create(
            kIOMemoryDirectionOutIn, kPSPCmdBufSize, kASPageSize, &cb);
        if (ret != kIOReturnSuccess || cb == nullptr) {
            PSP_LOG("cmd buffer alloc failed: %#x", ret);
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        cb->SetLength(kPSPCmdBufSize);
        IODMACommand *cdma = nullptr;
        IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                             &spec, &cdma);
        IOAddressSegment cseg = {};
        uint64_t cflags = 0;
        uint32_t csegc = 1;
        cdma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, cb, 0,
                            kPSPCmdBufSize, &cflags, &csegc, &cseg);
        IOAddressSegment cpu = {};
        cb->GetAddressRange(&cpu);
        psp.cmdBuffer     = cb;
        psp.cmdDMACommand = cdma;
        psp.cmdBusAddr    = cseg.address;
        psp.cmdCPUAddr    = reinterpret_cast<void *>(cpu.address);
    }
    {
        IOBufferMemoryDescriptor *fb = nullptr;
        ret = IOBufferMemoryDescriptor::Create(
            kIOMemoryDirectionOutIn, kPSPFenceBufSize, kASPageSize, &fb);
        if (ret != kIOReturnSuccess || fb == nullptr) {
            PSP_LOG("fence buffer alloc failed: %#x", ret);
            return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
        }
        fb->SetLength(kPSPFenceBufSize);
        IODMACommand *fdma = nullptr;
        IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                             &spec, &fdma);
        IOAddressSegment fseg = {};
        uint64_t fflags = 0;
        uint32_t fsegc = 1;
        fdma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, fb, 0,
                            kPSPFenceBufSize, &fflags, &fsegc, &fseg);
        IOAddressSegment cpu = {};
        fb->GetAddressRange(&cpu);
        psp.fenceBuffer     = fb;
        psp.fenceDMACommand = fdma;
        psp.fenceBusAddr    = fseg.address;
        psp.fenceCPUAddr    = reinterpret_cast<void *>(cpu.address);
        psp.fenceCounter    = 0;
        // Clear so the first fence value comparison doesn't false-match.
        *reinterpret_cast<volatile uint32_t *>(psp.fenceCPUAddr) = 0;
    }
    PSP_LOG("cmd_buf bus=%#llx fence_buf bus=%#llx",
            psp.cmdBusAddr, psp.fenceBusAddr);
    return kIOReturnSuccess;
}

//
// psp_ring_cmd_submit — direct port of upstream's algorithm
// (drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c:3449 psp_ring_cmd_submit
//  + amdgpu_psp.c:705 psp_cmd_submit_buf).
//
// Flow:
//   1. Read current wptr from C2PMSG_67.
//   2. Compute write_frame slot in ring memory based on wptr.
//   3. memcpy `cmd` into cmd_buf at psp.cmdCPUAddr.
//   4. Build a 64-byte psp_gfx_rb_frame in the ring slot pointing
//      to cmdBusAddr + fenceBusAddr with our incremented fence value.
//   5. Update wptr (in dwords) and write back to C2PMSG_67.
//   6. Poll *fenceCPUAddr until it equals the fence value or timeout.
//   7. Read response status from cmd_buf+offsetof(resp.status) — at
//      offset 864 per upstream psp_gfx_cmd_resp layout.
//
kern_return_t
psp_ring_cmd_submit(DeviceContext &dev, PSPContext &psp,
                    const void *cmd, uint32_t cmdSize,
                    uint32_t *outRespStatus)
{
    if (!psp.ringCreated || psp.cmdCPUAddr == nullptr ||
        psp.fenceCPUAddr == nullptr) {
        return kIOReturnNotReady;
    }
    if (cmd == nullptr || cmdSize != kPSPGfxCmdRespSize) {
        return kIOReturnBadArgument;
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        return kIOReturnNotReady;
    }

    const uint32_t ringSizeBytes = psp.ringSize;
    const uint32_t ringSizeDw    = ringSizeBytes / 4;
    const uint32_t frameSizeDw   = sizeof(PSPGfxRBFrame) / 4;
    const uint32_t regWptr = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                              MP0Regs::C2PMSG_67);

    // 1. Get current wptr.
    uint32_t wptr_dw = RREG32(dev, regWptr);

    // 2. Locate the frame slot in ring memory.
    PSPGfxRBFrame *ring_start =
        reinterpret_cast<PSPGfxRBFrame *>(psp.ringCPUAddr);
    PSPGfxRBFrame *ring_end =
        ring_start + (ringSizeBytes / sizeof(PSPGfxRBFrame)) - 1;
    PSPGfxRBFrame *frame;
    if ((wptr_dw % ringSizeDw) == 0) {
        frame = ring_start;
    } else {
        frame = ring_start + (wptr_dw / frameSizeDw);
    }
    if (frame < ring_start || frame > ring_end) {
        PSP_LOG("ring_cmd_submit: wptr %u out of range", wptr_dw);
        return kIOReturnInternalError;
    }

    // 3. Stage the command buffer.
    memset(psp.cmdCPUAddr, 0, kPSPCmdBufSize);
    memcpy(psp.cmdCPUAddr, cmd, cmdSize);

    // 4. Bump fence counter and build the frame.
    uint32_t fence_index = ++psp.fenceCounter;
    *reinterpret_cast<volatile uint32_t *>(psp.fenceCPUAddr) = 0;

    memset(frame, 0, sizeof(*frame));
    frame->cmd_buf_addr_lo = static_cast<uint32_t>(psp.cmdBusAddr & 0xFFFFFFFFu);
    frame->cmd_buf_addr_hi = static_cast<uint32_t>(psp.cmdBusAddr >> 32);
    frame->cmd_buf_size    = kPSPGfxCmdRespSize;
    frame->fence_addr_lo   = static_cast<uint32_t>(psp.fenceBusAddr & 0xFFFFFFFFu);
    frame->fence_addr_hi   = static_cast<uint32_t>(psp.fenceBusAddr >> 32);
    frame->fence_value     = fence_index;

    // 5. Advance and publish wptr (in dwords).
    wptr_dw = (wptr_dw + frameSizeDw) % ringSizeDw;
    WREG32(dev, regWptr, wptr_dw);

    // 6. Wait for PSP to write the fence value.
    auto *fence_ptr = reinterpret_cast<volatile uint32_t *>(psp.fenceCPUAddr);
    const uint64_t kBudgetUs = 10 * 1000000;  // 10 s — same order as Linux
    uint64_t elapsed = 0;
    const uint64_t kStep = 100;
    while (elapsed < kBudgetUs) {
        if (*fence_ptr == fence_index) break;
        IOSleep(1);
        elapsed += 1000;
        (void)kStep;
    }
    if (*fence_ptr != fence_index) {
        PSP_LOG("ring_cmd_submit: fence timeout (expected %u, got %u)",
                fence_index, *fence_ptr);
        return kIOReturnTimeout;
    }

    // 7. Read response status from the cmd buffer.
    // Upstream psp_gfx_cmd_resp.resp lives at offset 864 (see psp_gfx_if.h).
    const uint32_t kRespStatusOffset = 864;
    uint32_t resp_status = *reinterpret_cast<const uint32_t *>(
        static_cast<const uint8_t *>(psp.cmdCPUAddr) + kRespStatusOffset);
    if (outRespStatus) *outRespStatus = resp_status;
    if (resp_status != 0) {
        PSP_LOG("ring_cmd_submit: PSP returned status %#x", resp_status);
        return kIOReturnError;
    }
    PSP_LOG("ring_cmd_submit: ok (fence=%u)", fence_index);
    return kIOReturnSuccess;
}

//
// psp_setup_tmr — allocates a DART-backed system-memory buffer for
// the TMR and submits GFX_CMD_ID_SETUP_TMR via the PSP ring.
// Idempotent. Linux normally puts the TMR in VRAM; we use system
// memory + the `virt_phy_addr` flag until we have a VRAM allocator.
//
kern_return_t
psp_setup_tmr(DeviceContext &dev, PSPContext &psp)
{
    if (psp.tmrSetUp) return kIOReturnSuccess;
    if (!psp.ringCreated || psp.cmdCPUAddr == nullptr) {
        return kIOReturnNotReady;
    }

    // Allocate TMR backing buffer (system memory, DART-mapped).
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, kPSPTMRDefaultSize, kASPageSize, &buf);
    if (ret != kIOReturnSuccess || buf == nullptr) {
        PSP_LOG("tmr buffer alloc failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    buf->SetLength(kPSPTMRDefaultSize);

    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;
    IODMACommand *dma = nullptr;
    ret = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                               &spec, &dma);
    if (ret != kIOReturnSuccess || dma == nullptr) {
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    ret = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
                             kPSPTMRDefaultSize, &flags, &segCount, &seg);
    if (ret != kIOReturnSuccess || segCount != 1) {
        dma->release();
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNotAligned;
    }
    IOAddressSegment cpu = {};
    buf->GetAddressRange(&cpu);
    psp.tmrBuffer     = buf;
    psp.tmrDMACommand = dma;
    psp.tmrBusAddr    = seg.address;
    psp.tmrCPUAddr    = reinterpret_cast<void *>(cpu.address);
    psp.tmrSize       = kPSPTMRDefaultSize;

    // Build SETUP_TMR command in a scratch cmd_resp-sized buffer.
    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    hdr->buf_size    = sizeof(cmd_buf);
    hdr->buf_version = kPSPGfxCmdBufVersion;
    hdr->cmd_id      = PSPGfxCmd::SETUP_TMR;

    auto *tmr = reinterpret_cast<PSPGfxCmdSetupTmr *>(cmd_buf + 28);
    tmr->buf_phy_addr_lo    = static_cast<uint32_t>(psp.tmrBusAddr & 0xFFFFFFFFu);
    tmr->buf_phy_addr_hi    = static_cast<uint32_t>(psp.tmrBusAddr >> 32);
    tmr->buf_size           = static_cast<uint32_t>(psp.tmrSize);
    tmr->tmr_flags          = 0x2;  // virt_phy_addr=1 — DART buffer is its own phys
    tmr->system_phy_addr_lo = tmr->buf_phy_addr_lo;
    tmr->system_phy_addr_hi = tmr->buf_phy_addr_hi;

    uint32_t resp = 0;
    ret = psp_ring_cmd_submit(dev, psp, cmd_buf, kPSPGfxCmdRespSize, &resp);
    if (ret != kIOReturnSuccess) {
        PSP_LOG("SETUP_TMR submit failed: %#x (resp=%#x)", ret, resp);
        return ret;
    }
    psp.tmrSetUp = true;
    PSP_LOG("SETUP_TMR ok — TMR at bus=%#llx size=%llu",
            psp.tmrBusAddr, psp.tmrSize);
    return kIOReturnSuccess;
}

//
// psp_load_ip_fw — submit a GFX_CMD_ID_LOAD_IP_FW with the firmware
// already staged in a DART-mapped buffer. PSP reads the bytes,
// validates the signature, copies them into the TMR + then to the
// target IP, and asserts the IP's reset. Returns kIOReturnSuccess
// only if PSP's response status is 0.
//
kern_return_t
psp_load_ip_fw(DeviceContext &dev, PSPContext &psp,
               uint64_t fwBusAddr, uint32_t fwSize, uint32_t fwType)
{
    if (!psp.tmrSetUp) {
        PSP_LOG("load_ip_fw(type=%u): TMR not set up — call psp_setup_tmr first",
                fwType);
        return kIOReturnNotReady;
    }
    if (fwBusAddr == 0 || fwSize == 0) {
        return kIOReturnBadArgument;
    }
    // 4 KB alignment is the PSP protocol requirement (independent of
    // AS 16 KB DART pages — we satisfy both because all our DART-mapped
    // buffers come in at 16 KB-aligned bus addresses).
    if (fwBusAddr & 0xFFF) {
        return kIOReturnNotAligned;
    }

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    hdr->buf_size    = sizeof(cmd_buf);
    hdr->buf_version = kPSPGfxCmdBufVersion;
    hdr->cmd_id      = PSPGfxCmd::LOAD_IP_FW;

    auto *load = reinterpret_cast<PSPGfxCmdLoadIpFw *>(cmd_buf + 28);
    load->fw_phy_addr_lo = static_cast<uint32_t>(fwBusAddr & 0xFFFFFFFFu);
    load->fw_phy_addr_hi = static_cast<uint32_t>(fwBusAddr >> 32);
    load->fw_size        = fwSize;
    load->fw_type        = fwType;

    uint32_t resp = 0;
    kern_return_t ret = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                            kPSPGfxCmdRespSize, &resp);
    if (ret != kIOReturnSuccess) {
        PSP_LOG("LOAD_IP_FW(type=%u, size=%u, bus=%#llx) failed: %#x resp=%#x",
                fwType, fwSize, fwBusAddr, ret, resp);
        return ret;
    }
    PSP_LOG("LOAD_IP_FW(type=%u, size=%u) ok", fwType, fwSize);
    return kIOReturnSuccess;
}

} // namespace amdgpu
