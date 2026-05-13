//
//  amdgpu_init.h — Phase 1B bringup orchestrator.
//
//  Drives the firmware-load + IP-init sequence in dependency order:
//
//      0 IPDiscovery   — resolve IP base addresses (hardcoded for R9700
//                        until we read the on-die discovery table)
//      1 PSPInit       — allocate PSP fw_pri DMA buffer
//      2 PSPLoadSOS    — bootloader → SOS firmware load
//      3 SMUInit       — (stub) SMU v14_0_3 mailbox handshake
//      4 GMCInit       — (stub) VRAM detect, GART aperture setup
//      5 IMUInit       — (stub) image management unit
//      6 RLCInit       — (stub) RLC bringup
//      7 CPInit        — (stub) command processor firmware
//      8 MESInit       — (stub) MES v12_1 queue manager
//      9 IHInit        — (stub) interrupt handler ring
//     10 GFXInit       — (stub) GFX queue create via MES
//     11 SDMAInit      — (stub) SDMA v7_1 ring
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

namespace amdgpu {

enum class BringupStage : uint32_t {
    None          = 0,
    IPDiscovery   = 1,
    PSPInit       = 2,
    PSPLoadSOS    = 3,
    PSPRingCreate = 4,
    TMRSetup      = 5,
    SMUInit       = 6,
    GMCInit       = 7,
    IMUInit       = 8,
    RLCInit       = 9,
    CPInit        = 10,
    MESInit       = 11,
    IHInit        = 12,
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
