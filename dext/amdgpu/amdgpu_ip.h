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
    MMHUB,     // owns regMMMC_VM_FB_LOCATION_BASE — needed to find vram_start
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

    // IIG's IONewZero zero-fills the parent struct without running
    // C++ ctors, so base[] arrives as 0 instead of 0xFFFFFFFFu.
    // Treat *both* as "unresolved". A real IP base is never 0 on a
    // PCIDriverKit-mapped BAR0 (SMN registers start in the 0x40000+
    // range after the front-end SMUIO block).
    bool isResolved(IPBlock block) const {
        uint32_t b = base[(int)block];
        return b != 0xFFFFFFFFu && b != 0u;
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
    constexpr uint32_t C2PMSG_67  = 0x0083;  // PSP ring wptr (non-SR-IOV)
    constexpr uint32_t C2PMSG_69  = 0x0085;
    constexpr uint32_t C2PMSG_70  = 0x0086;
    constexpr uint32_t C2PMSG_71  = 0x0087;
    constexpr uint32_t C2PMSG_81  = 0x0091;
    constexpr uint32_t C2PMSG_101 = 0x00A5;
    constexpr uint32_t C2PMSG_102 = 0x00A6;
    constexpr uint32_t C2PMSG_103 = 0x00A7;
}

// SMU (MP1) mailbox registers from mp_14_0_2_offset.h. SMU is the
// PMFW on RDNA4 — handles clocks, voltages, power management, link
// training. Mailbox protocol (smu_cmn_send_smc_msg_with_param):
//   1. WREG32(C2PMSG_90, 0)            — clear response slot
//   2. WREG32(C2PMSG_82, param)        — parameter
//   3. WREG32(C2PMSG_66, msg_id)       — kicks SMU
//   4. poll C2PMSG_90 != 0             — response = status code
//   5. RREG32(C2PMSG_82)               — read return value
namespace MP1Regs {
    constexpr uint32_t C2PMSG_66 = 0x0082;  // message id (host → SMU)
    constexpr uint32_t C2PMSG_82 = 0x0092;  // parameter / return value
    constexpr uint32_t C2PMSG_90 = 0x009A;  // response (SMU → host)
}

// MMHUB registers — owns the VRAM/framebuffer location in MC space.
// Used to compute the GPU-MC address of any VRAM offset so PSP /
// other firmware can DMA-read from it via the GMC internal path.
//
// Offsets from upstream asic_reg/mmhub/mmhub_4_1_0_offset.h (RDNA4).
// Older NBIO families (4_1_0, etc) use the same offset but different
// IP BASE_IDX — we use BASE_IDX 0 which is what discovery reports.
//
// `regMMMC_VM_FB_LOCATION_BASE & 0x00FFFFFF` << 24 = vram_start MC addr.
namespace MMHUBRegs {
    constexpr uint32_t MMMC_VM_FB_LOCATION_BASE = 0x0554;
    constexpr uint32_t MMMC_VM_FB_LOCATION_TOP  = 0x0555;
    constexpr uint32_t MMMC_VM_FB_OFFSET        = 0x0556;
    constexpr uint32_t kFBBaseMask = 0x00FFFFFFu;  // low 24 bits
    constexpr uint32_t kFBBaseShift = 24;          // <<24 to get MC addr

    // GART setup registers from mmhub_4_1_0_offset.h (RDNA4 NBIO 7_11
    // family). All BASE_IDX 0 — same IP base as MMMC_VM_FB_LOCATION_BASE.
    // Used by gart_enable to point the GPU's GMC at our page table and
    // define the GART aperture in MC space.
    constexpr uint32_t MMVM_CONTEXT0_CNTL                       = 0x0564;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32  = 0x05cf;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32  = 0x05d0;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32 = 0x05ef;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32 = 0x05f0;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32   = 0x060f;
    constexpr uint32_t MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32   = 0x0610;
    constexpr uint32_t MMMC_VM_AGP_BASE                         = 0x055c;
    constexpr uint32_t MMMC_VM_AGP_BOT                          = 0x055d;
    constexpr uint32_t MMMC_VM_AGP_TOP                          = 0x055e;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_LOW_ADDR         = 0x0559;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR        = 0x055a;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB = 0x055f;
    constexpr uint32_t MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB = 0x0560;
    constexpr uint32_t MMMC_VM_MX_L1_TLB_CNTL                   = 0x055b;
    constexpr uint32_t MMVM_L2_CNTL                             = 0x04e4;
    constexpr uint32_t MMVM_L2_CNTL2                            = 0x04e5;
    constexpr uint32_t MMVM_L2_CNTL3                            = 0x04e6;
    constexpr uint32_t MMVM_L2_CNTL4                            = 0x04e7;
    constexpr uint32_t MMVM_L2_CNTL5                            = 0x04e8;
}

// PTE flag bits — drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h.
// PTEs are 64-bit: high bits = host physical address (page-aligned),
// low bits = flags. For GART-mapped sysmem we use VALID|SYSTEM|R|W.
namespace PTEFlags {
    constexpr uint64_t VALID     = (1ULL << 0);
    constexpr uint64_t SYSTEM    = (1ULL << 1);  // sysmem (not VRAM)
    constexpr uint64_t SNOOPED   = (1ULL << 2);
    constexpr uint64_t TMZ       = (1ULL << 3);
    constexpr uint64_t EXECUTABLE = (1ULL << 4);
    constexpr uint64_t READABLE  = (1ULL << 5);
    constexpr uint64_t WRITEABLE = (1ULL << 6);
    constexpr uint64_t FRAG_4K   = 0;
    // Standard sysmem mapping for PSP-readable buffers.
    constexpr uint64_t SYSMEM_RW = VALID | SYSTEM | SNOOPED |
                                   READABLE | WRITEABLE;
}

// AMDGPU GPU page size is fixed at 4 KB regardless of CPU page size.
// Apple Silicon CPU is 16 KB pages so each CPU page maps 4 GPU PTEs.
constexpr uint32_t kAMDGPUGPUPageSize  = 4096;
constexpr uint32_t kAMDGPUGPUPageShift = 12;

// PPSMC messages — drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/
// smu_v14_0_2_ppsmc.h. Tiny subset; expand as we wire up features.
namespace PPSMC {
    constexpr uint32_t TestMessage         = 0x01;
    constexpr uint32_t GetSmuVersion       = 0x02;
    constexpr uint32_t GetDriverIfVersion  = 0x03;
}

// Linux SMU mailbox response codes — smu_msg_v1_decode_response().
namespace SMUResp {
    constexpr uint32_t OK              = 0x01;
    constexpr uint32_t Failed          = 0xFF;
    constexpr uint32_t UnknownCmd      = 0xFE;
    constexpr uint32_t CmdRejectedPrereq = 0xFD;
    constexpr uint32_t CmdRejectedBusy = 0xFC;
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

// ============================================================
// Bootstrap registers — absolute BAR0 dword offsets that don't
// require any IP base to be resolved.
//
// These are the chicken-and-egg solver: amdgpu_discovery.c reads
// them via plain RREG32(mm...) (no SOC15 wrapper) to bootstrap the
// IP base table. On RDNA4 / gfx1201 the same legacy absolute
// aliases still resolve to the right physical registers.
//
// Sources:
//   drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c:140-148
//
// mmRCC_CONFIG_MEMSIZE  : VRAM size in MB.
// mmDRIVER_SCRATCH_0    : TMR offset LO (PSP-written; sysmem-TMR override)
// mmDRIVER_SCRATCH_1    : TMR offset HI
// mmDRIVER_SCRATCH_2    : TMR size in bytes (0 if no sysmem override)
//
// Discovery binary location (per amdgpu_discovery_get_tmr_info):
//   if DRIVER_SCRATCH_2 != 0:
//       offset = (DRIVER_SCRATCH_1 << 32) | DRIVER_SCRATCH_0
//       size   = DRIVER_SCRATCH_2
//   else:
//       offset = (vram_size_mb << 20) - DISCOVERY_TMR_OFFSET
//       size   = DISCOVERY_TMR_SIZE
// ============================================================
namespace BootstrapRegs {
    // BAR0-absolute dword offsets — these are the upstream "legacy
    // aliases" that work pre-IP-discovery. Same offset across NBIO
    // 6_1 / 7_0 / 7_4 / 7_11. They become valid only AFTER IFWI
    // init completes (poll MP0_C2PMSG_33 bit 31).
    constexpr uint32_t RCC_CONFIG_MEMSIZE = 0x0DE3;
    constexpr uint32_t DRIVER_SCRATCH_0   = 0x0094;
    constexpr uint32_t DRIVER_SCRATCH_1   = 0x0095;
    constexpr uint32_t DRIVER_SCRATCH_2   = 0x0096;

    // MP0 IFWI handshake register. Upstream amdgpu_discovery.c
    // amdgpu_discovery_get_tmr_info() polls this for bit 31 set for
    // up to 2 seconds before reading MEMSIZE — required on USB4/TB
    // hotplug because IFWI init isn't complete when the OS sees the
    // device. Offset matches mp_11_5_0 / mp_12_0_0 / mp_14_0_2
    // ASIC headers: dword 0x0061 (byte 0x184).
    constexpr uint32_t MP0_C2PMSG_33      = 0x0061;
    constexpr uint32_t kIFWIReadyMask     = 0x80000000u;
    constexpr uint32_t kIFWIReadyValue    = 0x80000000u;
    constexpr uint64_t kIFWITimeoutMs     = 2000;
}

// Upstream constants from amdgpu_discovery.h:
//     DISCOVERY_TMR_OFFSET = (64 << 10)   = 64 KB
//     DISCOVERY_TMR_SIZE   = (10 << 10)   = 10 KB (actual binary)
// We allocate 64 KB to round to the AS page size (16 KB) with room.
constexpr uint64_t kDiscoveryTMROffset = 0x10000;    // 64 KB (matches upstream)
constexpr uint32_t kDiscoveryTMRSize   = 0x10000;    // 64 KB allocation
                                                     // (binary itself ≈ 10 KB)

// **** Apple Silicon page size is 16 KB, not 4 KB. ****
//
// All DMA-mappable buffers handed to the GPU through DART must be
// aligned to 16 KB or DART will reject the mapping (qemu-vfio-apple
// has explicit "rejecting DMA mapping" logging for the multi-segment
// fallback case that fires when this is wrong). Use kASPageSize for
// alignment everywhere, even when the GPU side only needs 4 KB
// granularity — the cost is a few extra zero bytes per allocation,
// the failure mode otherwise is a hard refusal at PrepareForDMA.
constexpr uint64_t kASPageSize = 16384;

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

// PSP's protocol uses a 4 KB ring (matches Linux psp_ring_init).
// On AS we still allocate the underlying buffer at 16 KB alignment
// + size to satisfy the page granularity, and tell PSP that the
// usable area is 4 KB via C2PMSG_71. The trailing 12 KB is unused.
constexpr uint32_t kPSPKMRingSize    = 0x1000;
constexpr uint32_t kPSPKMRingBufSize = 16384;

} // namespace amdgpu
