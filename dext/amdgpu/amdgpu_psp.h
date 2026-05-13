//
//  amdgpu_psp.h — PSP v14 bootloader interface.
//
//  Ports the subset of Linux's psp_v14_0.c that we need for Phase 1B
//  bringup. Specifically:
//      psp_init           — set up the fw_pri DMA buffer + state
//      psp_is_sos_alive   — check sign-of-life register
//      psp_wait_for_bootloader  — poll C2PMSG_35 bit31
//      psp_load_sos       — copy SOS binary to fw_pri, kick bootloader
//
//  Subsequent components (KDB, SPL, SysDrv, SocDrv, IntfDrv, RASDrv,
//  IPKeyMgrDrv) load through the same protocol with different bl_cmd
//  values. Those land in commits after we have SOS up.
//
//  The ring-based protocol (used after SOS is alive — for SMU bringup,
//  firmware loads, etc.) is a separate port from psp_v14_0_ring_*.
//

#pragma once

#include <stdint.h>

#ifdef __APPLE__
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#endif

#include "amdgpu_regs.h"

namespace amdgpu {

struct PSPContext {
    // Primary firmware buffer — PSP reads each binary from here.
    // Must be DMA-mappable; allocated via IOBufferMemoryDescriptor
    // + IODMACommand::PrepareForDMA. Size = PSP_1_MEG.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *fwPriBuffer;
    IODMACommand             *fwPriDMACommand;
#endif
    uint64_t  fwPriBusAddr;   // GPU-visible bus address (post-DART)
    void     *fwPriCPUAddr;   // CPU-side ptr for memcpy of firmware
    uint64_t  fwPriSize;

    // SOS firmware blob (handed in via LoadFirmware selector).
    // For other bootloader components (KDB, SysDrv, etc.) we pass
    // the binary directly through psp_bootloader_load_component and
    // don't keep a long-lived pointer.
    const uint8_t *sosFirmware;
    uint64_t       sosFirmwareSize;

    bool sosAlive;

    // PSP command ring (km_ring). Allocated via psp_ring_create.
    // The ring lives in system memory mapped through DART (we don't
    // yet have a VRAM allocator). 4 KB matches Linux's default.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *ringBuffer;
    IODMACommand             *ringDMACommand;
#endif
    uint64_t  ringBusAddr;
    void     *ringCPUAddr;
    uint64_t  ringSize;
    bool      ringCreated;

    // PSP command buffer + fence buffer for ring-submitted commands.
    // The command buffer holds a single psp_gfx_cmd_resp struct (1 KB);
    // the fence buffer is a small 64 B word that PSP writes after the
    // command completes. Both DART-mapped.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *cmdBuffer;
    IODMACommand             *cmdDMACommand;
    IOBufferMemoryDescriptor *fenceBuffer;
    IODMACommand             *fenceDMACommand;
#endif
    uint64_t  cmdBusAddr;
    void     *cmdCPUAddr;
    uint64_t  fenceBusAddr;
    void     *fenceCPUAddr;
    uint32_t  fenceCounter;

    // TMR (Trusted Memory Region) — PSP-owned region used as the
    // staging area for IP firmware loads. Linux puts this in VRAM,
    // but PSP also accepts a system-memory region for early bringup
    // (signaled via cmd_setup_tmr.tmr_flags.virt_phy_addr = 1). We
    // do the latter until we have a VRAM allocator.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *tmrBuffer;
    IODMACommand             *tmrDMACommand;
#endif
    uint64_t  tmrBusAddr;
    void     *tmrCPUAddr;
    uint64_t  tmrSize;
    bool      tmrSetUp;
};

//
// psp_init — allocate the fw_pri DMA buffer + populate PSPContext.
// Idempotent. Returns kIOReturnSuccess on success.
//
kern_return_t psp_init(DeviceContext &dev, PSPContext &psp);

//
// psp_release — free fw_pri buffer.
//
void psp_release(PSPContext &psp);

//
// psp_is_sos_alive — port of psp_v14_0_is_sos_alive.
// Returns true if MP0 C2PMSG_81 is non-zero (SOS has reported in).
//
bool psp_is_sos_alive(const DeviceContext &dev);

//
// psp_wait_for_bootloader — port of psp_v14_0_wait_for_bootloader.
// Polls MP0 C2PMSG_35 bit 31 for up to 10 × bootloader timeout.
// Returns true on success.
//
bool psp_wait_for_bootloader(const DeviceContext &dev);

//
// psp_load_sos — port of psp_v14_0_bootloader_load_sos.
// Copies the SOS image into fw_pri, kicks the bootloader, polls
// for SOS alive. Idempotent: returns success early if SOS is already
// alive. Caller must have set psp.sosFirmware + size before calling.
//
kern_return_t psp_load_sos(DeviceContext &dev, PSPContext &psp);

//
// psp_bootloader_load_component — port of psp_v14_0_bootloader_load_component.
// Copies a binary into fw_pri, writes the address + command, and
// waits for the bootloader ready bit. Used for the pre-SOS components:
//   PSPBootloaderCmd::LoadKeyDatabase   → KDB
//   PSPBootloaderCmd::LoadTosSPLTable   → SPL
//   PSPBootloaderCmd::LoadSysDrv        → SysDrv
//   PSPBootloaderCmd::LoadSocDrv        → SocDrv
//   PSPBootloaderCmd::LoadIntfDrv       → IntfDrv
//   PSPBootloaderCmd::LoadHADDrv        → DbgDrv (renamed to HAD in v14)
//   PSPBootloaderCmd::LoadRASDrv        → RASDrv
//   PSPBootloaderCmd::LoadIPKeyMgrDrv   → IPKeyMgrDrv
// Returns immediately with success if SOS is already alive (the
// caller is too late — these only matter before SOS).
//
kern_return_t psp_bootloader_load_component(DeviceContext &dev,
                                            PSPContext &psp,
                                            const uint8_t *bin,
                                            uint64_t binSize,
                                            uint32_t bl_cmd);

//
// psp_ring_create — port of psp_v14_0_ring_create (non-SR-IOV path).
// Allocates a 4 KB DMA-backed buffer in system memory, programs its
// address+size+type into PSP via C2PMSG_69..71+64, waits for bit 31
// in C2PMSG_64 to come back set. After this, PSP commands can be
// submitted by writing into the ring + ringing the doorbell.
//
// Ring type is hardcoded to PSP_RING_TYPE__KM (1) — the kernel-mode
// ring. We don't use UM ring (userspace-mode, SR-IOV-only).
//
kern_return_t psp_ring_create(DeviceContext &dev, PSPContext &psp);

//
// psp_ring_cmd_submit — port of upstream psp_ring_cmd_submit +
// psp_cmd_submit_buf. Synchronously submits a single PSP GFX command
// frame, waits for the fence to come back. Returns the PSP response
// status via *outRespStatus (PSP convention: 0 = success).
//
// The caller fills `cmd` (a psp_gfx_cmd_resp-sized buffer); we memcpy
// it into psp.cmdCPUAddr, then build a psp_gfx_rb_frame in the ring
// that points to it, then bump the wptr.
//
// cmdSize is the user-provided struct size — must match the upstream
// sizeof(psp_gfx_cmd_resp) = 1024 bytes.
//
kern_return_t psp_ring_cmd_submit(DeviceContext &dev, PSPContext &psp,
                                  const void *cmd, uint32_t cmdSize,
                                  uint32_t *outRespStatus);

//
// psp_setup_tmr — port of psp_setup_tmr/_v2 from upstream
// drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c. Allocates a TMR buffer
// in DART-mapped system memory (idempotent) and submits a
// GFX_CMD_ID_SETUP_TMR via the PSP ring. Required before any
// LOAD_IP_FW submission.
//
// Default TMR size is 4 MB which is enough for SMU + RLC + CP + MES
// + SDMA + IH staging; AMD's bootloader on some ASICs negotiates a
// smaller size via LOAD_TOC, but for the initial port we statically
// size it.
//
kern_return_t psp_setup_tmr(DeviceContext &dev, PSPContext &psp);

//
// psp_load_ip_fw — port of psp_load_ip_fw. Submits a
// GFX_CMD_ID_LOAD_IP_FW for a single firmware image. The caller
// stages the firmware bytes into `fwSysAddr` (CPU pointer to a
// DART-mapped buffer; the caller passes the corresponding GPU bus
// address as `fwBusAddr`). PSP copies the firmware into the TMR
// then into the target IP's memory and asserts the IP's reset.
//
// fwType: one of the PSP_GFX_FW_TYPE_* values (e.g. SMU=18).
//
// The DMA buffer for the firmware bytes only needs to live for the
// duration of this call; PSP reads and copies before returning.
//
kern_return_t psp_load_ip_fw(DeviceContext &dev, PSPContext &psp,
                             uint64_t fwBusAddr, uint32_t fwSize,
                             uint32_t fwType);

// Subset of psp_gfx_fw_type — full enum in upstream
// drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (208).
namespace PSPGfxFwType {
    constexpr uint32_t SMU       = 18;   // PMFW
    constexpr uint32_t SDMA0     = 9;
    constexpr uint32_t SDMA1     = 10;
    constexpr uint32_t RLC_G     = 8;
    constexpr uint32_t CP_ME     = 1;
    constexpr uint32_t CP_PFP    = 2;
    constexpr uint32_t CP_MEC    = 4;
    constexpr uint32_t IMU_I     = 68;
    constexpr uint32_t IMU_D     = 69;
    constexpr uint32_t RS64_MES        = 76;
    constexpr uint32_t RS64_MES_STACK  = 77;
    constexpr uint32_t RS64_KIQ        = 78;
    constexpr uint32_t RS64_KIQ_STACK  = 79;
}

// GFX command IDs (subset). Full list in upstream psp_gfx_if.h.
namespace PSPGfxCmd {
    constexpr uint32_t SETUP_TMR   = 5;
    constexpr uint32_t LOAD_IP_FW  = 6;
}

} // namespace amdgpu
