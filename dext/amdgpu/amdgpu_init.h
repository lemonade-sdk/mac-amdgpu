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
//      8 SMUInit       — after PSP has LoadFirmware(SMU); mailbox handshake
//      9 IMUInit       — after PSP has LoadFirmware(IMU_I/D)
//     10 RLCInit       — after PSP has loaded the RLC sub-bins
//     11 CPInit        — after RS64 firmwares loaded
//     12 MESInit       — after CP_MES + CP_MES_DATA loaded
//     13 GFXInit       — first PM4 submit
//     14 SDMAInit      — after SDMA TH0
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
#include "amdgpu_discovery.h"
#include "amdgpu_imu.h"
#include "amdgpu_gfx.h"

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
    SMUInit       = 8,   // upstream phase2: SMU.hw_init
    IMUInit       = 9,
    RLCInit       = 10,
    CPInit        = 11,
    MESInit       = 12,
    GFXInit       = 13,
    SDMAInit      = 14,
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
    IMUContext    imu;         // IMU microcode-loaded gate
    GFXConfig     gfx;         // gfx_constants_init harvest + caps

    BringupStage  reached;   // highest stage that completed
};

//
// Drive bringup up to and including `target`. Returns
// kIOReturnSuccess if all stages up to target are now done.
// Subsequent calls with a higher target pick up where we left off.
//
kern_return_t bringup_to(BringupContext &ctx, BringupStage target);

//
// IP discovery — hardcoded R9700 values; sets the IP base table.
// TODO(phase1b): replace with on-die discovery-binary read.
//
kern_return_t bringup_ip_discovery(BringupContext &ctx);

} // namespace amdgpu
