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
#include "amdgpu_regs.h"

namespace MacAMDGPU {

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

} // namespace MacAMDGPU
