//
//  amdgpu_ip.h — IP version pinning + IP base-address table.
//
//  In upstream Linux, IP base addresses come from the runtime IP
//  discovery table on the GPU (see amdgpu_discovery.c) because one
//  driver supports dozens of ASICs. We support exactly one ASIC
//  (Radeon AI PRO R9700, gfx1201) so we pin the versions at compile
//  time and either:
//      (a) hardcode the base offsets once we know them, or
//      (b) read them from the discovery binary on first init and
//          stash them in the global IPBaseTable.
//
//  Initial commit uses placeholder 0xFFFFFFFFu sentinels for any
//  base address we haven't read off real hardware yet. The PSP /
//  SMU / GFX bringup code asserts the bases are filled before use,
//  so a missing entry fails loud rather than reading register 0.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

// IP versions on Radeon AI PRO R9700 (gfx1201) — confirmed from
// the devcoredump in qemu-vfio-apple/traces/:
//
//   HWIP: GC[1][0]:    v12.0.1.0.0   → gfx_v12_1
//   HWIP: HDP[2][0]:   v7.0.0.0.0
//   HWIP: SDMA0[3][0]: v7.0.1.0.0    → sdma_v7_1
//   HWIP: SDMA1[4][0]: v7.0.1.0.0
//
// PSP / SMU / MES inferred from linux-firmware blob versions
// (psp_14_0_3_*, smu_14_0_3*, gc_12_0_1_mes*):
//
//   PSP:  v14.0.3
//   SMU:  v14.0.3
//   MES:  v12.0.1
//   NBIO: v7.11
//
struct IPVersion {
    uint8_t major;
    uint8_t minor;
    uint8_t rev;
};

constexpr IPVersion kIP_GFX  = { 12, 0, 1 };
constexpr IPVersion kIP_GMC  = { 12, 0, 0 };
constexpr IPVersion kIP_SDMA = { 7,  0, 1 };
constexpr IPVersion kIP_PSP  = { 14, 0, 3 };
constexpr IPVersion kIP_SMU  = { 14, 0, 3 };
constexpr IPVersion kIP_MES  = { 12, 0, 1 };
constexpr IPVersion kIP_NBIO = { 7,  11, 0 };
constexpr IPVersion kIP_IH   = { 7,  0, 0 };
constexpr IPVersion kIP_HDP  = { 7,  0, 0 };

// Hardware IP enum mirrors Linux amdgpu_ip_block.h.
enum class IPBlock : uint8_t {
    GC = 0,    // graphics + compute
    HDP,       // host data path
    SDMA0,
    SDMA1,
    MP0,       // PSP lives here
    MP1,       // SMU lives here
    NBIO,
    OSSSYS,    // IH lives here
    GMC,
    Count,
};

// IP base addresses in BAR0-relative dword offsets. Each entry is
// the SOC15 "instance 0" base; SOC15 register references are added
// on top.
//
// Sentinel: 0xFFFFFFFFu means "not yet read from discovery — must
// fill in before using this IP block."
struct IPBaseTable {
    uint32_t base[(int)IPBlock::Count];

    constexpr IPBaseTable() : base{} {
        for (int i = 0; i < (int)IPBlock::Count; i++) {
            base[i] = 0xFFFFFFFFu;
        }
    }

    bool isResolved(IPBlock block) const {
        return base[(int)block] != 0xFFFFFFFFu;
    }
    uint32_t get(IPBlock block) const { return base[(int)block]; }
    void set(IPBlock block, uint32_t b) { base[(int)block] = b; }
};

// SOC15 register offsets from upstream Linux —
// drivers/gpu/drm/amd/include/asic_reg/mp/mp_14_0_2_offset.h:
//
//   regMPASP_SMN_C2PMSG_35  = 0x0063  (bootloader cmd / status)
//   regMPASP_SMN_C2PMSG_36  = 0x0064  (binary fw_pri_mc_addr >> 20)
//   regMPASP_SMN_C2PMSG_64  = 0x0080  (ring create cmd)
//   regMPASP_SMN_C2PMSG_69  = 0x0085  (ring low addr)
//   regMPASP_SMN_C2PMSG_70  = 0x0086  (ring high addr)
//   regMPASP_SMN_C2PMSG_71  = 0x0087  (ring size)
//   regMPASP_SMN_C2PMSG_81  = 0x0091  (SOS sign-of-life)
//   regMPASP_SMN_C2PMSG_101 = 0x00A5  (ring destroy, SR-IOV)
//   regMPASP_SMN_C2PMSG_102 = 0x00A6  (SR-IOV ring low)
//   regMPASP_SMN_C2PMSG_103 = 0x00A7  (SR-IOV ring high)
//
// All added to the MP0 IP base.
namespace MP0Regs {
    constexpr uint32_t C2PMSG_35  = 0x0063;
    constexpr uint32_t C2PMSG_36  = 0x0064;
    constexpr uint32_t C2PMSG_64  = 0x0080;
    constexpr uint32_t C2PMSG_69  = 0x0085;
    constexpr uint32_t C2PMSG_70  = 0x0086;
    constexpr uint32_t C2PMSG_71  = 0x0087;
    constexpr uint32_t C2PMSG_81  = 0x0091;
    constexpr uint32_t C2PMSG_101 = 0x00A5;
    constexpr uint32_t C2PMSG_102 = 0x00A6;
    constexpr uint32_t C2PMSG_103 = 0x00A7;
}

// PSP bootloader commands —
// drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h (enum psp_bootloader_cmd).
namespace PSPBootloaderCmd {
    constexpr uint32_t LoadKeyDatabase  = 0x80000;
    constexpr uint32_t LoadTosSPLTable  = 0x10000000;
    constexpr uint32_t LoadSysDrv       = 0x10000;
    constexpr uint32_t LoadSocDrv       = 0xB0000;
    constexpr uint32_t LoadIntfDrv      = 0xD0000;
    constexpr uint32_t LoadHADDrv       = 0xC0000;
    constexpr uint32_t LoadRASDrv       = 0xE0000;
    constexpr uint32_t LoadIPKeyMgrDrv  = 0x110000;
    constexpr uint32_t LoadSOSDrv       = 0x20000;
}

constexpr uint32_t kPSPBootloaderReadyBit = 0x80000000u;
constexpr uint32_t kPSPFwPriBufSize       = 1024u * 1024u;  // PSP_1_MEG

// PSP GFX command frame flags — from
// drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h.
constexpr uint32_t kPSPGfxCmdStatusMask   = 0x0000FFFFu;
constexpr uint32_t kPSPGfxCmdResponseMask = 0x80000000u;
constexpr uint32_t kPSPGfxFlagResponse    = 0x80000000u;

// Combined response handshake. C2PMSG_64 should latch
// (val & MASK) == FLAG once the bootloader finishes.
constexpr uint32_t kPSPMboxRespFlag = kPSPGfxFlagResponse;
constexpr uint32_t kPSPMboxRespMask = kPSPGfxCmdResponseMask
                                    | kPSPGfxCmdStatusMask;

// psp_ring_type — only KM is used outside SR-IOV.
constexpr uint32_t kPSPRingTypeKM = 1;

// 4 KB matches Linux's psp_ring_init default for km_ring.
constexpr uint32_t kPSPKMRingSize = 0x1000;

} // namespace amdgpu
