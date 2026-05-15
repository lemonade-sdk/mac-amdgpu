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
#include "amdgpu_ucode_psp.h"

#define PSP_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.psp: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// Read the GPU's vram_start MC address from MMHUB's
// regMMMC_VM_FB_LOCATION_BASE. Requires MMHUB IP base to be resolved
// via IP discovery first.
static uint64_t
psp_read_vram_start(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::MMHUB)) return 0;
    uint32_t mmhub_base = dev.ip.get(IPBlock::MMHUB);
    uint32_t raw = RREG32(dev,
        mmhub_base + MMHUBRegs::MMMC_VM_FB_LOCATION_BASE);
    return ((uint64_t)(raw & MMHUBRegs::kFBBaseMask))
           << MMHUBRegs::kFBBaseShift;
}

// VRAM layout for PSP-accessed buffers. All four live within the first
// few MB of VRAM (well inside the 256 MB visible BAR0 aperture). PSP
// reads via internal GMC route — the MC address it sees is
// `vram_start + offset`.
//
// We hardcode offsets for simplicity; later phases will manage VRAM
// allocation through a proper bump-or-buddy allocator.
static constexpr uint64_t kFwPriVRAMOffset = 0;          // [0, 1MB)
static constexpr uint64_t kRingVRAMOffset  = 0x100000;   // 1 MB
static constexpr uint64_t kCmdBufVRAMOffset = 0x104000;  // 1 MB + 16 KB
static constexpr uint64_t kFenceVRAMOffset = 0x108000;   // 1 MB + 32 KB
static constexpr uint64_t kTMRVRAMOffset   = 0x200000;   // 2 MB (size 4 MB)

kern_return_t
psp_init(DeviceContext &dev, PSPContext &psp)
{
    if (psp.fwPriSize != 0) {
        return kIOReturnSuccess; // idempotent
    }
    if (!dev.ip.isResolved(IPBlock::MP0)) {
        PSP_LOG("MP0 IP base not resolved — IP discovery missing");
        return kIOReturnNotReady;
    }
    if (!dev.ip.isResolved(IPBlock::MMHUB)) {
        PSP_LOG("MMHUB IP base not resolved — IP discovery missing");
        return kIOReturnNotReady;
    }

    // VRAM-backed fw_pri: PSP DMAs via the GMC internal path using an
    // MC address, NOT via PCIe with a DART-mapped sysmem bus address.
    // For us "allocation" is just picking a VRAM offset; CPU writes go
    // through the visible BAR0 aperture (256 MB on R9700).
    uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("psp_init: vram_start read back as 0 — MMHUB "
                "regMMMC_VM_FB_LOCATION_BASE not populated yet?");
        return kIOReturnNotReady;
    }

    psp.fwPriBuffer     = nullptr;  // no sysmem buffer — VRAM-backed
    psp.fwPriDMACommand = nullptr;
    psp.fwPriBusAddr    = vram_start + kFwPriVRAMOffset;  // GPU MC addr
    psp.fwPriCPUAddr    = nullptr;  // CPU side accesses via BAR0 aperture
    psp.fwPriSize       = kPSPFwPriBufSize;
    psp.sosAlive        = false;

    PSP_LOG("fw_pri ready VRAM-backed @ vram_off=%#llx mc=%#llx size=%llu "
            "(vram_start=%#llx)",
            (uint64_t)kFwPriVRAMOffset, psp.fwPriBusAddr,
            psp.fwPriSize, vram_start);
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
    // Match upstream psp_v14_0_is_sos_alive exactly: any non-zero
    // value indicates SOS booted. PSP writes a build/version stamp
    // here once SOS init completes (we've observed 0x022690e0).
    // Earlier code tightened to bit 31 to defend against cold-boot
    // garbage — but on a fresh power-up C2PMSG_81 is reliably zero
    // until SOS sets it, and upstream's broader check is required
    // to accept the real value PSP writes.
    return sol != 0;
}

bool
psp_wait_for_bootloader(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::MP0)) return false;
    const uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                          MP0Regs::C2PMSG_35);
    // Upstream Linux psp_v14_0_wait_for_bootloader loops 10x against
    // psp_wait_for (each call uses adev->usec_timeout, default 100 ms;
    // bumped 10x for emulation/passthrough modes). Total budget on
    // bare-metal is ~1 second; with the 10x emulation bump it's ~10 s.
    //
    // TB5 hotplug effectively counts as "passthrough" — the PSP
    // bootloader takes longer to come up than on PCIe-attached cards.
    // Use a 10-second budget to match upstream's worst case.
    PSP_LOG("wait_for_bootloader: polling C2PMSG_35 (reg=%#x) for "
            "bit 31 set, up to 10 sec", reg);
    const uint64_t kBudgetUs = 10ULL * 1000000;
    uint32_t v = 0;
    bool ok = poll_reg(dev, reg,
                       kPSPBootloaderReadyBit, kPSPBootloaderReadyBit,
                       kBudgetUs, &v);
    if (!ok) {
        PSP_LOG("bootloader wait timeout — C2PMSG_35=%#010x "
                "(expected bit 31 set)", v);
    } else {
        PSP_LOG("bootloader ready — C2PMSG_35=%#010x", v);
    }
    return ok;
}

kern_return_t
psp_load_sos(DeviceContext &dev, PSPContext &psp)
{
    if (psp.fwPriSize == 0) {
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

    // Stage the SOS image in fw_pri (VRAM via BAR0 aperture).
    // NOTE: we don't memset the full 1 MB like upstream — each BAR0
    // dword write is ~10x slower on AS, so a full 1 MB memset takes
    // ~2.6 sec. Just zero exactly what the firmware doesn't cover.
    // For SOS specifically, since this is the final load and the file
    // is large, skip the trailing zero entirely; PSP should not read
    // past sosFirmwareSize when the descriptor reports it.
    bar0_memcpy_to_vram(dev, kFwPriVRAMOffset,
                        psp.sosFirmware, psp.sosFirmwareSize);

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
    if (psp.fwPriSize == 0) {
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

    // Stage the sub-firmware in fw_pri (VRAM via BAR0 aperture).
    // Skip the full-1MB memset (too slow on AS — see psp_load_sos).
    bar0_memcpy_to_vram(dev, kFwPriVRAMOffset, bin, binSize);

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

    // VRAM-backed ring (same reason as fw_pri — PSP reads via GMC,
    // needs an MC address, not a DART bus address). 16 KB allocation
    // sized to AS page granularity; PSP uses only the first 4 KB.
    uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("ring_create: vram_start = 0; MMHUB not ready");
        return kIOReturnNotReady;
    }

    psp.ringBuffer      = nullptr;  // no sysmem buffer
    psp.ringDMACommand  = nullptr;
    psp.ringBusAddr     = vram_start + kRingVRAMOffset;
    psp.ringCPUAddr     = nullptr;
    psp.ringSize        = kPSPKMRingSize;

    // Zero the ring memory in VRAM (small — 16 KB = 4096 dwords).
    bar0_memset_vram(dev, kRingVRAMOffset, 0, kPSPKMRingBufSize);

    const uint32_t reg64 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_64);
    const uint32_t reg69 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_69);
    const uint32_t reg70 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_70);
    const uint32_t reg71 = SOC15_REG_OFFSET(dev, IPBlock::MP0,
                                            MP0Regs::C2PMSG_71);

    // Diagnostic: read C2PMSG_64 + C2PMSG_81 (SOS sign-of-life)
    // before we touch anything.
    uint32_t v = 0;
    uint32_t reg81 = SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_81);
    uint32_t pre64 = RREG32(dev, reg64);
    uint32_t pre81 = RREG32(dev, reg81);
    PSP_LOG("ring_create entry: C2PMSG_64=%#010x C2PMSG_81=%#010x sosAlive=%d",
            pre64, pre81, (int)psp.sosAlive);

    // 1. Wait for SOS ready to accept ring creation. Always do this
    //    (Linux does it unconditionally) — the previous skip-on-
    //    warm-SOS optimisation was based on bad assumptions.
    if (!poll_reg(dev, reg64, kPSPMboxRespMask, kPSPMboxRespFlag,
                  5 * 1000000, &v)) {
        PSP_LOG("ring_create: SOS not ready — C2PMSG_64=%#010x "
                "(masked=%#010x, want %#010x)",
                v, v & kPSPMboxRespMask, kPSPMboxRespFlag);
        psp.ringDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.ringDMACommand->release();
        psp.ringDMACommand = nullptr;
        psp.ringBuffer->release();
        psp.ringBuffer = nullptr;
        return kIOReturnTimeout;
    }
    PSP_LOG("ring_create: SOS ready, C2PMSG_64=%#010x", v);

    // 2. Program ring address (low + high) + size, then kick.
    WREG32(dev, reg69, static_cast<uint32_t>(psp.ringBusAddr & 0xFFFFFFFFu));
    WREG32(dev, reg70, static_cast<uint32_t>(psp.ringBusAddr >> 32));
    WREG32(dev, reg71, static_cast<uint32_t>(psp.ringSize));
    WREG32(dev, reg64, kPSPRingTypeKM << 16);
    PSP_LOG("ring_create: wrote ring addr=%#llx size=%u type=%u (kick=%#x)",
            psp.ringBusAddr, (unsigned)psp.ringSize,
            kPSPRingTypeKM, kPSPRingTypeKM << 16);

    IOSleep(20);

    // 3. Wait for response flag.
    if (!poll_reg(dev, reg64, kPSPMboxRespMask, kPSPMboxRespFlag,
                  5 * 1000000, &v)) {
        uint32_t now64 = RREG32(dev, reg64);
        uint32_t now81 = RREG32(dev, reg81);
        PSP_LOG("ring_create: response wait timed out — "
                "final C2PMSG_64=%#010x C2PMSG_81=%#010x",
                now64, now81);
        return kIOReturnTimeout;
    }

    psp.ringCreated = true;
    PSP_LOG("ring_created mc=%#llx size=%llu (response=%#010x)",
            psp.ringBusAddr, psp.ringSize, v);

    // VRAM-backed cmd + fence buffers. Same reasoning as ring/fw_pri:
    // PSP needs an MC address it can route via GMC.
    psp.cmdBuffer       = nullptr;
    psp.cmdDMACommand   = nullptr;
    psp.cmdBusAddr      = vram_start + kCmdBufVRAMOffset;
    psp.cmdCPUAddr      = nullptr;

    psp.fenceBuffer     = nullptr;
    psp.fenceDMACommand = nullptr;
    psp.fenceBusAddr    = vram_start + kFenceVRAMOffset;
    psp.fenceCPUAddr    = nullptr;
    psp.fenceCounter    = 0;

    // Zero both regions in VRAM.
    bar0_memset_vram(dev, kCmdBufVRAMOffset, 0, kPSPCmdBufSize);
    bar0_memset_vram(dev, kFenceVRAMOffset,  0, kPSPFenceBufSize);

    PSP_LOG("cmd_buf mc=%#llx fence_buf mc=%#llx",
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
    if (!psp.ringCreated) {
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

    // 2. Locate the frame slot offset within the ring (VRAM-relative).
    uint32_t frame_slot;
    if ((wptr_dw % ringSizeDw) == 0) {
        frame_slot = 0;
    } else {
        frame_slot = wptr_dw / frameSizeDw;
    }
    uint32_t max_slot = ringSizeBytes / sizeof(PSPGfxRBFrame);
    if (frame_slot >= max_slot) {
        PSP_LOG("ring_cmd_submit: wptr %u out of range", wptr_dw);
        return kIOReturnInternalError;
    }
    uint64_t frame_vram_off = kRingVRAMOffset
                            + (uint64_t)frame_slot * sizeof(PSPGfxRBFrame);

    // 3. Stage the command buffer in VRAM via BAR0.
    bar0_memcpy_to_vram(dev, kCmdBufVRAMOffset, cmd, cmdSize);

    // 4. Bump fence counter, zero the fence dword, build the frame.
    uint32_t fence_index = ++psp.fenceCounter;
    WBAR0_32(dev, kFenceVRAMOffset, 0);

    PSPGfxRBFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(psp.cmdBusAddr & 0xFFFFFFFFu);
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(psp.cmdBusAddr >> 32);
    frame.cmd_buf_size    = kPSPGfxCmdRespSize;
    frame.fence_addr_lo   = static_cast<uint32_t>(psp.fenceBusAddr & 0xFFFFFFFFu);
    frame.fence_addr_hi   = static_cast<uint32_t>(psp.fenceBusAddr >> 32);
    frame.fence_value     = fence_index;
    bar0_memcpy_to_vram(dev, frame_vram_off, &frame, sizeof(frame));

    // 5. Advance and publish wptr (in dwords).
    wptr_dw = (wptr_dw + frameSizeDw) % ringSizeDw;
    WREG32(dev, regWptr, wptr_dw);

    // 6. Wait for PSP to write the fence value in VRAM (read via BAR0).
    // PSP should respond in ~1-10 ms when working. Linux uses
    // adev->usec_timeout ≈ 100ms-1s. We use 1 second — long enough
    // to absorb startup jitter, short enough that an actual failure
    // doesn't waste 80 seconds across 8 stages.
    const uint64_t kBudgetUs = 1 * 1000000;
    uint64_t elapsed = 0;
    uint32_t observed = 0;
    while (elapsed < kBudgetUs) {
        observed = RBAR2_32(dev, kFenceVRAMOffset);
        if (observed == fence_index) break;
        IOSleep(1);
        elapsed += 1000;
    }
    if (observed != fence_index) {
        PSP_LOG("ring_cmd_submit: fence timeout (expected %u, got %u)",
                fence_index, observed);
        return kIOReturnTimeout;
    }

    // 7. Read response status from cmd buffer in VRAM.
    // Upstream psp_gfx_cmd_resp.resp lives at offset 864 (psp_gfx_if.h).
    const uint32_t kRespStatusOffset = 864;
    uint32_t resp_status = RBAR2_32(dev,
        kCmdBufVRAMOffset + kRespStatusOffset);
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
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }

    // VRAM-backed TMR — pass PSP an MC address. We don't need to
    // CPU-touch the TMR; PSP and IP firmwares own it.
    uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("setup_tmr: vram_start=0; MMHUB not ready");
        return kIOReturnNotReady;
    }
    psp.tmrBuffer     = nullptr;
    psp.tmrDMACommand = nullptr;
    psp.tmrBusAddr    = vram_start + kTMRVRAMOffset;
    psp.tmrCPUAddr    = nullptr;
    psp.tmrSize       = kPSPTMRDefaultSize;

    // Build SETUP_TMR command in a scratch cmd_resp-sized buffer.
    // We construct this on the dext stack and rely on the ring submit
    // path to BAR0-copy it into the VRAM cmd_buf.
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
    tmr->tmr_flags          = 0;  // VRAM-backed, not virt_phy
    tmr->system_phy_addr_lo = 0;
    tmr->system_phy_addr_hi = 0;

    uint32_t resp = 0;
    kern_return_t ret = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                            kPSPGfxCmdRespSize, &resp);
    if (ret != kIOReturnSuccess) {
        PSP_LOG("SETUP_TMR submit failed: %#x (resp=%#x)", ret, resp);
        return ret;
    }
    psp.tmrSetUp = true;
    PSP_LOG("SETUP_TMR ok — TMR at mc=%#llx (vram_off=%#llx) size=%llu",
            psp.tmrBusAddr, (uint64_t)kTMRVRAMOffset, psp.tmrSize);
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

//============================================================
// psp_parse_sos_microcode — port of upstream amdgpu_psp.c
// psp_init_sos_microcode. Auto-detects v1 vs v2 header and
// populates psp.sos / psp.kdb / psp.sys / etc. with pointers
// into the input firmware blob.
//============================================================

static void
set_sub_bin(PSPContext::PSPSubBin &dst,
            const uint8_t *base, uint32_t offset_bytes,
            uint32_t size_bytes, uint32_t fw_version)
{
    dst.start_addr = (size_bytes > 0) ? (base + offset_bytes) : nullptr;
    dst.size_bytes = size_bytes;
    dst.fw_version = fw_version;
}

kern_return_t
psp_parse_sos_microcode(PSPContext &psp,
                        const uint8_t *fw_data, uint64_t fw_size)
{
    // Reset every sub-bin so a re-parse doesn't leave stale pointers.
    psp.sos = psp.sys = psp.kdb = psp.toc = psp.spl = psp.rl =
        psp.soc_drv = psp.intf_drv = psp.dbg_drv = psp.ras_drv =
        psp.ipkeymgr_drv = psp.spdm_drv = psp.sys_drv_aux =
        psp.sos_aux = PSPContext::PSPSubBin{};

    if (fw_data == nullptr || fw_size < sizeof(common_firmware_header)) {
        PSP_LOG("parse_sos: fw_data null or too small (%llu)", fw_size);
        return kIOReturnBadArgument;
    }

    auto *hdr = reinterpret_cast<const common_firmware_header *>(fw_data);
    uint32_t ucode_off = hdr->ucode_array_offset_bytes;
    if (ucode_off > fw_size) {
        PSP_LOG("parse_sos: ucode_array_offset_bytes %#x exceeds fw_size %llu",
                ucode_off, fw_size);
        return kIOReturnBadArgument;
    }
    const uint8_t *ucode_base = fw_data + ucode_off;

    PSP_LOG("parse_sos: hdr ver %u.%u, ip %u.%u, ucode_size=%u, "
            "ucode_off=%#x, total=%u",
            hdr->header_version_major, hdr->header_version_minor,
            hdr->ip_version_major, hdr->ip_version_minor,
            hdr->ucode_size_bytes, ucode_off, hdr->size_bytes);

    psp.sos_fw_blob      = fw_data;
    psp.sos_fw_blob_size = fw_size;

    switch (hdr->header_version_major) {
    case 1: {
        // v1.0: only sos. v1.1/1.2: + kdb (and maybe toc). v1.3: +spl/rl/aux.
        // Note: v1 layouts pack sub-bins WITHIN the ucode region — the
        // start address is `ucode_base + sub.offset_bytes`. Upstream
        // does the same.
        auto *h10 = reinterpret_cast<const psp_firmware_header_v1_0 *>(fw_data);
        // The "sys" sub-bin doesn't exist as a separate field on v1.0 —
        // upstream's psp_init_sos_base_fw fills sys.start_addr from
        // sos.offset_bytes (sys precedes sos in the ucode region for
        // legacy chips). We mirror that here.
        set_sub_bin(psp.sys, ucode_base,
                    0,
                    h10->sos.offset_bytes,  // sys spans [0, sos_off)
                    hdr->ucode_version);
        set_sub_bin(psp.sos, ucode_base,
                    h10->sos.offset_bytes,
                    h10->sos.size_bytes,
                    h10->sos.fw_version);

        if (hdr->header_version_minor >= 1) {
            auto *h11 = reinterpret_cast<const psp_firmware_header_v1_1 *>(fw_data);
            set_sub_bin(psp.toc, ucode_base,
                        h11->toc.offset_bytes,
                        h11->toc.size_bytes, h11->toc.fw_version);
            set_sub_bin(psp.kdb, ucode_base,
                        h11->kdb.offset_bytes,
                        h11->kdb.size_bytes, h11->kdb.fw_version);
        }
        if (hdr->header_version_minor == 2) {
            // v1.2 redefines: kdb instead of toc
            auto *h12 = reinterpret_cast<const psp_firmware_header_v1_2 *>(fw_data);
            set_sub_bin(psp.kdb, ucode_base,
                        h12->kdb.offset_bytes,
                        h12->kdb.size_bytes, h12->kdb.fw_version);
        }
        if (hdr->header_version_minor == 3) {
            auto *h13 = reinterpret_cast<const psp_firmware_header_v1_3 *>(fw_data);
            set_sub_bin(psp.spl, ucode_base,
                        h13->spl.offset_bytes,
                        h13->spl.size_bytes, h13->spl.fw_version);
            set_sub_bin(psp.rl, ucode_base,
                        h13->rl.offset_bytes,
                        h13->rl.size_bytes, h13->rl.fw_version);
            set_sub_bin(psp.sys_drv_aux, ucode_base,
                        h13->sys_drv_aux.offset_bytes,
                        h13->sys_drv_aux.size_bytes,
                        h13->sys_drv_aux.fw_version);
            set_sub_bin(psp.sos_aux, ucode_base,
                        h13->sos_aux.offset_bytes,
                        h13->sos_aux.size_bytes,
                        h13->sos_aux.fw_version);
        }
        return kIOReturnSuccess;
    }
    case 2: {
        // v2.0/v2.1: flexible array of psp_fw_bin_desc tagged by fw_type.
        // Iterate and route each desc by fw_type. v2.1 has an extra
        // `psp_aux_fw_bin_index` field we don't currently need (only
        // relevant for chips with auxiliary SOS variants we don't support
        // yet).
        auto *h20 = reinterpret_cast<const psp_firmware_header_v2_0 *>(fw_data);
        const psp_fw_bin_desc *bin = h20->psp_fw_bin;
        uint32_t count = h20->psp_fw_bin_count;
        if (hdr->header_version_minor == 1) {
            auto *h21 = reinterpret_cast<const psp_firmware_header_v2_1 *>(fw_data);
            bin = h21->psp_fw_bin;
            // We don't currently route psp_aux_fw_bin_index; leave the
            // aux loading for when we encounter a chip that needs it.
        }
        if (count > 64) {
            PSP_LOG("parse_sos: implausible bin_count=%u", count);
            return kIOReturnBadArgument;
        }
        for (uint32_t i = 0; i < count; i++) {
            const auto &d = bin[i];
            switch (d.fw_type) {
            case PSP_FW_TYPE_PSP_SOS:
                set_sub_bin(psp.sos, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SYS_DRV:
                set_sub_bin(psp.sys, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_KDB:
                set_sub_bin(psp.kdb, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_TOC:
                set_sub_bin(psp.toc, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SPL:
                set_sub_bin(psp.spl, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_RL:
                set_sub_bin(psp.rl, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SOC_DRV:
                set_sub_bin(psp.soc_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_INTF_DRV:
                set_sub_bin(psp.intf_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_DBG_DRV:
                set_sub_bin(psp.dbg_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_RAS_DRV:
                set_sub_bin(psp.ras_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_IPKEYMGR_DRV:
                set_sub_bin(psp.ipkeymgr_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            case PSP_FW_TYPE_PSP_SPDM_DRV:
                set_sub_bin(psp.spdm_drv, ucode_base, d.offset_bytes, d.size_bytes, d.fw_version);
                break;
            default:
                PSP_LOG("parse_sos: ignoring unknown fw_type=%u "
                        "(offset=%#x size=%u)",
                        d.fw_type, d.offset_bytes, d.size_bytes);
                break;
            }
        }
        return kIOReturnSuccess;
    }
    default:
        PSP_LOG("parse_sos: unsupported header_version_major=%u",
                hdr->header_version_major);
        return kIOReturnUnsupported;
    }
}

//============================================================
// psp_load_sos_package — port of psp_v14_0_hw_init's pre-SOS
// loader sequence. Loads each non-empty sub-firmware via the
// bootloader interface in the upstream order, then loads SOS.
//============================================================
kern_return_t
psp_load_sos_package(DeviceContext &dev, PSPContext &psp)
{
    if (psp_is_sos_alive(dev)) {
        PSP_LOG("load_sos_package: SOS already alive — skipping");
        psp.sosAlive = true;
        return kIOReturnSuccess;
    }

    struct Step {
        const char *name;
        const PSPContext::PSPSubBin *bin;
        uint32_t bl_cmd;
    } steps[] = {
        // Order taken from psp_v14_0_hw_init / psp_hw_start in upstream.
        // We skip any step whose bin->size_bytes is 0 (e.g. SPL on
        // chips that don't ship one).
        { "KDB",        &psp.kdb,         PSP_BL__LOAD_KEY_DATABASE  },
        { "SPL",        &psp.spl,         PSP_BL__LOAD_TOS_SPL_TABLE },
        { "SYS_DRV",    &psp.sys,         PSP_BL__LOAD_SYSDRV        },
        { "SOC_DRV",    &psp.soc_drv,     PSP_BL__LOAD_SOCDRV        },
        { "INTF_DRV",   &psp.intf_drv,    PSP_BL__LOAD_INTFDRV       },
        { "DBG/HAD_DRV",&psp.dbg_drv,     PSP_BL__LOAD_HADDRV        },
        { "RAS_DRV",    &psp.ras_drv,     PSP_BL__LOAD_RASDRV        },
        { "IPKEYMGR",   &psp.ipkeymgr_drv,PSP_BL__LOAD_IPKEYMGRDRV   },
    };
    for (auto &s : steps) {
        if (s.bin->size_bytes == 0 || s.bin->start_addr == nullptr) {
            PSP_LOG("load_sos_package: skip %{public}s (not present)", s.name);
            continue;
        }
        PSP_LOG("load_sos_package: %{public}s (%llu bytes) → bl_cmd=%#x",
                s.name, s.bin->size_bytes, s.bl_cmd);
        kern_return_t r = psp_bootloader_load_component(
            dev, psp, s.bin->start_addr, s.bin->size_bytes, s.bl_cmd);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_sos_package: %{public}s FAILED kr=%#x",
                    s.name, r);
            return r;
        }
    }

    if (psp.sos.size_bytes == 0 || psp.sos.start_addr == nullptr) {
        PSP_LOG("load_sos_package: SOS sub-bin missing from package");
        return kIOReturnBadArgument;
    }
    PSP_LOG("load_sos_package: SOS (%llu bytes) → final load",
            psp.sos.size_bytes);
    // Stash into the legacy slots for now (until psp_load_sos is
    // refactored to read psp.sos directly).
    psp.sosFirmware     = psp.sos.start_addr;
    psp.sosFirmwareSize = psp.sos.size_bytes;
    kern_return_t r = psp_load_sos(dev, psp);
    psp.sosFirmware     = nullptr;
    psp.sosFirmwareSize = 0;
    return r;
}

} // namespace amdgpu
