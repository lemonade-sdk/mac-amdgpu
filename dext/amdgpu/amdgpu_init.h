//
//  amdgpu_init.h — Phase 1B bringup orchestrator.
//
//  Drives the firmware-load + IP-init sequence in upstream-faithful order.
//  Reference: drivers/gpu/drm/amd/amdgpu/amdgpu_device.c
//             amdgpu_device_ip_init  (line 2299)
//
//  Upstream sequence:
//    - COMMON.hw_init                       (we fold into IPDiscovery)
//    - GMC.hw_init                          (inline before phase1)
//    - phase1: COMMON + IH hw_init          (we explicitly do IH here)
//    - amdgpu_device_fw_loading             (PSP.hw_init + SMU fw load)
//    - phase2: SMU/GFX/MES/SDMA/... hw_init
//
//  Stage numbering (audit #9 #1, #4, #7):
//      0 None
//      1 IPDiscovery   — on-die discovery + IP base table + NBIO HDP remap
//      2 IHInit        — interrupt handler ring (phase1, before PSP)
//      3 GMCInit       — MC init, GART page tables, MMHUB+GFXHUB enable,
//                        HDP+TLB flush, fault-redirect to dummy_page.
//                        Must complete BEFORE PSP so the SOS doesn't
//                        stomp on our L2/TLB state.
//      4 PSPInit       — allocate PSP fw_pri DMA buffer in VRAM
//      5 PSPLoadSOS    — bootloader handshake → SOS firmware load
//      6 PSPRingCreate — KM ring + FB_FW_RESERV query
//      7 TMRSetup      — psp_setup_tmr (skip path for 14_0_3)
//      8 PSPFwLoad     — load_non_psp_fw: SMU→IMU→RLC→autoload→CP→SDMA→MES
//      9 SMUInit       — after PSP has LoadFirmware(SMU); mailbox handshake
//     10 IMUInit       — after PSP has LoadFirmware(IMU_I/D)
//     11 RLCInit       — after PSP has loaded the RLC sub-bins
//     12 CPInit        — RS64 setup, GFXHUB and constants; queue halted
//     13 MESInit       — after CP_MES + CP_MES_DATA loaded
//     14 GFXInit       — resume legacy GFX queue after MES
//     15 SDMAInit      — after SDMA TH0
//
//  Each stage either runs to completion or returns an error. The
//  orchestrator is idempotent — repeating a stage that's already
//  done is a no-op.
//

#pragma once

#include "amdgpu_psp.h"
#include "amdgpu_smu.h"
#include "amdgpu_gmc.h"
#include "amdgpu_ih.h"
#include "amdgpu_rlc.h"
#include "amdgpu_cp.h"
#include "amdgpu_sdma.h"
#include "amdgpu_mes.h"
#include "amdgpu_gart.h"
#include "amdgpu_memory_test.h"
#include "amdgpu_discovery.h"
#include "amdgpu_imu.h"
#include "amdgpu_gfx.h"

// Forward declaration — defined in amdgpu_ip.h
struct DoorbellState;

namespace amdgpu {

// Numbering follows the upstream amdgpu_device_ip_init order.
// Audit #9: GMCInit MUST run before PSPInit (phase2 sees GART up).
//           IHInit MUST run before PSPInit (phase1 in upstream).
enum class BringupStage : uint32_t {
    None          = 0,
    IPDiscovery   = 1,
    IHInit        = 2,   // upstream phase1: IH.hw_init
    GMCInit       = 3,   // upstream pre-phase1: GMC.hw_init inline
    PSPInit       = 4,   // upstream fw_loading: PSP.hw_init
    PSPLoadSOS    = 5,
    PSPRingCreate = 6,
    TMRSetup      = 7,
    PSPFwLoad     = 8,   // upstream fw_loading: psp_load_non_psp_fw
    SMUInit       = 9,   // upstream phase2: SMU.hw_init
    IMUInit       = 10,
    RLCInit       = 11,
    CPInit        = 12,
    MESInit       = 13,
    GFXInit       = 14,
    SDMAInit      = 15,
};

//
// Aggregate context for the bringup. Lives on the driver instance
// (not per-UserClient) so multiple clients see consistent state.
//
struct BringupContext {
    DeviceContext device;
    PSPContext    psp;
    GMCContext    gmc;
    IHContext     ih;
    RLCContext    rlc;
    CPContext     cp;
    SDMAContext   sdma;
    MESContext    mes;
    GARTContext   gart;        // GART page-table state + bindings (DMA fix)
    MemoryTransferTest memoryTest;
    IMUContext    imu;         // IMU microcode-loaded gate
    GFXConfig     gfx;         // gfx_constants_init harvest + caps
    DoorbellState doorbell;    // BAR2 doorbell index map + state

    BringupStage  reached;   // highest stage that completed
};

//
// Drive bringup up to and including `target`. Returns
// kIOReturnSuccess if all stages up to target are now done.
// Subsequent calls with a higher target pick up where we left off.
//
kern_return_t bringup_to(BringupContext &ctx, BringupStage target);

// Final CPU-side teardown only: PCI must already be closed and every
// user-client/interrupt callback drained. Never accesses GPU registers.
// Safe after partial initialization and safe to call more than once.
void bringup_release_resources(BringupContext &ctx);

//
// IP discovery — hardcoded R9700 values; sets the IP base table.
// TODO(phase1b): replace with on-die discovery-binary read.
//
kern_return_t bringup_ip_discovery(BringupContext &ctx);

//
// Doorbell init — mirrors upstream amdgpu_doorbell_init
// (amdgpu_doorbell_mgr.c:193). Records BAR2 base/size and
// populates the doorbell_index map with ASIC-specific values.
//
// On Apple Silicon BAR2 is accessed via IOPCIDevice::MemoryRead32/
// Write32 (not as a linear mapping), so base=0. The size is read
// from PCI config space. The doorbell_index map is ASIC-specific
// and hardcoded for RDNA4 (gfx1201).
//
// Returns kIOReturnSuccess on success, kIOReturnNotReady if BAR2
// is not mapped.
//
kern_return_t doorbell_init(DeviceContext &dev, DoorbellState &db);

//
// psp_load_all_fw — orchestrates loading all non-PSP firmware
// (SMU, IMU, RLC, CP, SDMA, MES) through the PSP ring.
//
// This is the dext-side equivalent of upstream's psp_load_non_psp_fw
// (amdgpu_psp.c:3051). It calls psp_load_non_psp_fw with a
// FirmwareLoader callback that reads firmware from the host's
// DMABuffer.
//
// The caller must have already loaded all firmware files into the
// DMABuffer via the host-side LoadFirmware selector. This function
// just needs to know where to find each firmware's payloads.
//
// Returns kIOReturnSuccess on success, or the first error encountered.
//
kern_return_t psp_load_all_fw(DeviceContext &dev, PSPContext &psp,
                              const FirmwareLoader &loader);

} // namespace amdgpu
