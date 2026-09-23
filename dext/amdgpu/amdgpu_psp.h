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

#include <functional>
#include "amdgpu_regs.h"
#include "amdgpu_gart.h"

namespace amdgpu {

// FirmwareLoader — callback interface for psp_load_non_psp_fw.
// The caller implements get_payload() to provide firmware data for
// each fw_type. The loader is responsible for staging the firmware
// bytes into psp.fw_buf (VRAM) at the correct offset and returning
// the GPU bus address and size.
struct FirmwareLoader {
    // Given a fw_type, return (bus_addr, size) for the firmware payload.
    // Returns kIOReturnSuccess if the payload exists, kIOReturnUnsupported
    // if this fw_type is not present in the current firmware version.
    using GetPayloadFn = std::function<kern_return_t(uint32_t fw_type,
                                                      uint64_t &out_bus_addr,
                                                      uint32_t &out_size)>;
    GetPayloadFn get_payload;
};

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

    // PSP firmware sub-binary descriptors — mirrors upstream
    // `struct psp_bin_desc` in amdgpu_psp.h. Each sub-firmware
    // lives inside the same `psp_<chip>_sos.bin` file, addressed by
    // start_addr + size_bytes after the header parser has run.
    // Filled in by psp_parse_sos_microcode() on every fresh LoadFirmware.
    struct PSPSubBin {
        const uint8_t *start_addr;
        uint64_t       size_bytes;
        uint32_t       fw_version;
    };
    PSPSubBin sos;
    PSPSubBin sys;          // upstream "sys_drv"
    PSPSubBin kdb;
    PSPSubBin toc;
    PSPSubBin spl;
    PSPSubBin rl;
    PSPSubBin soc_drv;
    PSPSubBin intf_drv;
    PSPSubBin dbg_drv;      // a.k.a. had_drv on psp_v14
    PSPSubBin ras_drv;
    PSPSubBin ipkeymgr_drv;
    PSPSubBin spdm_drv;
    PSPSubBin sys_drv_aux;  // v1.3 only
    PSPSubBin sos_aux;      // v1.3 only

    // ASD (Authenticated Secure Display) TA descriptor — extracted from
    // psp_<chip>_ta.bin by psp_parse_ta_microcode(). Upstream calls
    // psp_asd_initialize between AUTOLOAD_RLC and psp_rl_load
    // (amdgpu_psp.c:3153); skipping it appears to leave PSP in a partial
    // state where the GC autoload state machine doesn't fire (v0.1.18
    // symptom). We pre-stage the ASD ucode into a dedicated VRAM slot
    // during LoadFirmware(TA) since the host DMA buffer gets overwritten
    // by the next LoadFirmware call before psp_asd_initialize runs.
    struct ASDContext {
        bool      parsed;         // psp_parse_ta_microcode populated this
        uint32_t  size_bytes;     // ASD ucode size from TA bin descriptor
        uint32_t  fw_version;
        uint64_t  ucode_mc_addr;  // VRAM MC address of staged ASD ucode
        uint64_t  ucode_vram_off; // BAR0-relative VRAM offset (for re-staging)
        uint32_t  session_id;     // PSP-assigned, post-load
        uint32_t  resp_status;    // last LOAD_ASD response status
    };
    ASDContext asd;

    // Private snapshot of `_sos.bin`. In particular RL is consumed only
    // after subsequent uploads have overwritten the host staging buffer.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *sosPackageBuffer;
#endif
    const uint8_t *sos_fw_blob;
    uint64_t       sos_fw_blob_size;
    bool           firmwareLoadComplete; // AUTOLOAD_RLC, ASD and RL acknowledged

    // Legacy fields used by code that hasn't been refactored to use
    // .sos.start_addr yet — kept until psp_load_sos is updated.
    const uint8_t *sosFirmware;
    uint64_t       sosFirmwareSize;

    bool sosAlive;

    // PSP runtime database — populated by psp_read_runtime_db() during
    // PSPInit. Mirrors upstream psp_sw_init's read of the runtime data
    // header at (mc_vram_size - PSP_RUNTIME_DB_OFFSET=0x100000).
    bool      runtimeDbRead;
    uint16_t  runtimeDbCookie;   // PSP_RUNTIME_DB_COOKIE_ID = 0x0ed5 expected
    uint16_t  runtimeDbVersion;
    uint32_t  bootCfgBitmask;    // BOOT_CFG_FEATURE_* flags
    uint32_t  scpmStatus;        // 0=disabled, 1=enabled, 2=enabled+err
    bool      scpmEnabled;

    // PSP command ring (km_ring). After the GART port, allocated as
    // sysmem via gart_bind_sysmem so PSP can route it through the GPU's
    // GMC. The GARTBinding owns the IOBufferMemoryDescriptor +
    // IODMACommand lifetime.
    // Pointer to the GART context owned by BringupContext. Set in
    // psp_init so PSP code can allocate GART-bound buffers without
    // taking a GART& parameter everywhere.
    GARTContext *gart;

    struct GARTBinding ringBinding;
    uint64_t  ringBusAddr;       // == ringBinding.gartMCAddr
    void     *ringCPUAddr;       // == ringBinding.cpuAddr
    uint64_t  ringSize;
    bool      ringCreated;
    // Legacy slots — no longer populated, kept for ABI compat with old
    // code that hasn't been refactored yet.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *ringBuffer;
    IODMACommand             *ringDMACommand;
#endif

    // PSP command buffer + fence buffer for ring-submitted commands.
    // Same GART-backed allocation pattern as ringBinding above.
    struct GARTBinding cmdBinding;
    struct GARTBinding fenceBinding;
    uint64_t  cmdBusAddr;
    void     *cmdCPUAddr;
    uint64_t  fenceBusAddr;
    void     *fenceCPUAddr;
    uint32_t  fenceCounter;
    // Legacy slots — no longer populated.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *cmdBuffer;
    IODMACommand             *cmdDMACommand;
    IOBufferMemoryDescriptor *fenceBuffer;
    IODMACommand             *fenceDMACommand;
#endif

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

    // ---- firmware.fw_buf-equivalent --------------------------------
    //
    // Upstream `amdgpu_ucode_create_bo` allocates ONE big buffer of
    // size `firmware.fw_size`, with EACH ucode getting its own MC
    // address inside it (`fw_buf_mc + cumulative PAGE-aligned offset`).
    // `psp_prep_load_ip_fw_cmd_buf` passes `ucode->mc_addr` (unique per
    // ucode) as `cmd_load_ip_fw.fw_phy_addr_lo/hi`. PSP then reads the
    // firmware bytes from that specific address and validates them.
    //
    // We used to pass `fwPriBusAddr` (single VRAM address) for EVERY
    // LOAD_IP_FW, which caused PSP to reject SDMA / CP_RS64 / MES with
    // TEE_BAD_PARAMETERS (0xFFFF0006) while SMU and IMU happened to
    // pass. Match upstream: bump-allocate a per-payload offset inside
    // a dedicated fw_buf region.
    //
    // fwBufVRAMOffset is the start of fw_buf within VRAM (BAR0
    // visible aperture). Each LoadFirmware bumps fwBufBumpOffset by
    // the page-aligned payload size and uses fwBufBaseMC +
    // (bump_offset before increment) as the LOAD_IP_FW address.
    uint64_t  fwBufVRAMOffset;   // legacy VRAM offset (unused after sysmem port)
    uint64_t  fwBufBaseMC;       // GART MC address PSP uses (post sysmem port)
    uint64_t  fwBufSize;         // total bytes reserved for fw_buf
    uint64_t  fwBufBumpOffset;   // next available offset within fw_buf

    // Sysmem backing for fw_buf — matches upstream amdgpu_ucode_create_bo
    // which allocates fw_buf in GTT (AMDGPU_GEM_DOMAIN_GTT) on the
    // default non-sriov/non-debug path. PSP reads the firmware bytes via
    // GMC, which resolves the GART MC address to sysmem through the
    // page table programmed by gmc_init.
#ifdef __APPLE__
    IOBufferMemoryDescriptor *fwBufSysmemBuffer;
    IODMACommand             *fwBufSysmemDMA;
#endif
    void     *fwBufSysmemCPU;    // CPU-side ptr for memcpy of payloads
    uint64_t  fwBufSysmemBusAddr;// DART-mapped bus addr (input to GART bind)
};

//
// psp_init — allocate the fw_pri DMA buffer + populate PSPContext.
// Idempotent. Returns kIOReturnSuccess on success.
//
kern_return_t psp_init(DeviceContext &dev, PSPContext &psp);

// psp_read_runtime_db — port of upstream psp_get_runtime_db_entry +
// psp_sw_init runtime DB read (amdgpu_psp.c:376-449, 471-503). Reads
// the PSP runtime data header at (vram_size - 0x100000) and fills in
// boot_cfg_bitmask + scpm_status if the cookie matches.
kern_return_t psp_read_runtime_db(DeviceContext &dev, PSPContext &psp,
                                  uint64_t vram_size_bytes);

//
// psp_setup_fw_buf_sysmem — allocate firmware staging buffer in sysmem
// and bind it into GART. Matches upstream amdgpu_ucode_create_bo which
// allocates fw_buf in GTT (AMDGPU_GEM_DOMAIN_GTT) on the non-sriov path
// (amdgpu_ucode.c:1148-1166). After this call psp.fwBufBaseMC is the
// GART MC address PSP should use in LOAD_IP_FW.fw_phy_addr.
//
// Requires GMC to be initialised (gmc.gart_start populated, page table
// allocated, MMHUB GART enabled). Call after GMCInit, on or before the
// first IP firmware load.
//
// Idempotent — second call is a no-op.
//
struct GMCContext;
kern_return_t psp_setup_fw_buf_sysmem(DeviceContext &dev, PSPContext &psp,
                                      GMCContext &gmc);

//
// psp_parse_sos_microcode — port of upstream amdgpu_psp.c
// `psp_init_sos_microcode`. Walks the AMD firmware header in `fw_data`
// (raw bytes of `psp_<chip>_sos.bin`), auto-detects v1 vs v2 layout,
// and populates psp.sos / psp.kdb / psp.sys / etc. with pointers into
// fw_data plus per-sub-binary sizes.
//
// Copies fw_data into driver-owned storage retained through psp_release;
// later host uploads cannot invalidate the sub-bin pointers. Returns
// kIOReturnSuccess on a recognised header, kIOReturnUnsupported on
// an unknown header_version_major, kIOReturnBadArgument on bad input.
//
kern_return_t psp_parse_sos_microcode(PSPContext &psp,
                                      const uint8_t *fw_data,
                                      uint64_t fw_size);

//
// psp_load_sos_package — port of psp_v14_0's full pre-SOS sequence.
// Loads KDB → SPL → SYS_DRV → SOC_DRV → INTF_DRV → HAD/DBG_DRV →
// RAS_DRV → IPKEYMGR_DRV (each via psp_bootloader_load_component),
// then loads SOS via psp_load_sos. Skips any sub-bin whose size is 0
// (e.g. SPL on chips that don't ship one). Caller must have already
// run psp_parse_sos_microcode to populate the sub-bin pointers.
//
kern_return_t psp_load_sos_package(DeviceContext &dev, PSPContext &psp);

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
    // Legacy SDMA instances (sdma_v4-style packaging). RDNA4 uses
    // SDMA_UCODE_TH0 instead — see below.
    constexpr uint32_t SDMA0     = 9;
    constexpr uint32_t SDMA1     = 10;
    // RDNA4 (sdma_v7_1) packs both engines into one RS64 firmware that
    // PSP loads ONCE with TH0=71. Per upstream amdgpu_sdma_init_microcode.
    constexpr uint32_t SDMA_UCODE_TH0 = 71;
    constexpr uint32_t RLC_G     = 8;
    constexpr uint32_t CP_ME     = 1;
    constexpr uint32_t CP_PFP    = 2;
    constexpr uint32_t CP_MEC    = 4;
    // GFX12 / RDNA4 RS64 CP firmwares.
    constexpr uint32_t RS64_PFP      = 87;
    constexpr uint32_t RS64_ME       = 88;
    constexpr uint32_t RS64_MEC      = 89;
    constexpr uint32_t RS64_PFP_P0   = 90;
    constexpr uint32_t RS64_PFP_P1   = 91;
    constexpr uint32_t RS64_ME_P0    = 92;
    constexpr uint32_t RS64_ME_P1    = 93;
    constexpr uint32_t RS64_MEC_P0   = 94;
    constexpr uint32_t RS64_MEC_P1   = 95;
    constexpr uint32_t RS64_MEC_P2   = 96;
    constexpr uint32_t RS64_MEC_P3   = 97;
    constexpr uint32_t IMU_I     = 68;
    constexpr uint32_t IMU_D     = 69;
    // Standalone MES (legacy / non-uni packaging).
    constexpr uint32_t RS64_MES        = 76;
    constexpr uint32_t RS64_MES_STACK  = 77;
    constexpr uint32_t RS64_KIQ        = 78;
    constexpr uint32_t RS64_KIQ_STACK  = 79;
    // PSP-embedded Register List (REG_LIST). Loaded via psp_rl_load AFTER
    // AUTOLOAD_RLC — PSP's autoload state machine waits for this firmware
    // to arrive before completing GC bringup. psp.rl is populated from
    // the v2 SOS package by psp_parse_sos_microcode (PSP_FW_TYPE_PSP_RL).
    constexpr uint32_t REG_LIST        = 67;
    // uni_mes packaging: ucode + data loaded as two LOAD_IP_FW frames.
    constexpr uint32_t CP_MES          = 33;
    constexpr uint32_t CP_MES_DATA     = 34;
    // Note: upstream `enum psp_gfx_fw_type` calls this MES_STACK (=34);
    // we use CP_MES_DATA as a clearer name for the uni_mes data half.
    constexpr uint32_t MES_STACK       = 34;   // alias of CP_MES_DATA
    // KIQ pipe variants — psp_gfx_if.h:285-286. Same uni_mes.bin bytes
    // as CP_MES/CP_MES_DATA but tagged differently so PSP places them
    // in the KIQ TMR slot. Required on RDNA4 because enable_uni_mes=1
    // is the default and mes_v12_0_early_init registers both pipes.
    constexpr uint32_t CP_MES_KIQ      = 81;
    constexpr uint32_t MES_KIQ_STACK   = 82;

    // RLC sub-firmwares (v2.1+). All emitted from a single rlc.bin
    // file when the relevant rlc_firmware_header_v2_x has non-zero
    // size for that sub-bin. From psp_gfx_if.h.
    constexpr uint32_t RLC_RESTORE_LIST_GPM_MEM  = 20;
    constexpr uint32_t RLC_RESTORE_LIST_SRM_MEM  = 21;
    constexpr uint32_t RLC_RESTORE_LIST_SRM_CNTL = 22;
    constexpr uint32_t RLC_V                     = 7;   // psp_gfx_if.h:216
    constexpr uint32_t RLC_P                     = 25;
    constexpr uint32_t RLC_IRAM                  = 26;
    constexpr uint32_t RLC_DRAM_BOOT             = 48;
    // RLC v2.4 tap delays.
    constexpr uint32_t GLOBAL_TAP_DELAYS         = 27;
    constexpr uint32_t SE0_TAP_DELAYS            = 28;
    constexpr uint32_t SE1_TAP_DELAYS            = 29;
    constexpr uint32_t SE2_TAP_DELAYS            = 65;
    constexpr uint32_t SE3_TAP_DELAYS            = 66;
}

// GFX command IDs (subset). Full list in upstream psp_gfx_if.h.
namespace PSPGfxCmd {
    constexpr uint32_t LOAD_TA               = 1;
    constexpr uint32_t UNLOAD_TA             = 2;
    constexpr uint32_t INVOKE_CMD            = 3;
    constexpr uint32_t LOAD_ASD              = 4;   // ASD = Authenticated Secure Display
    constexpr uint32_t SETUP_TMR             = 5;
    constexpr uint32_t LOAD_IP_FW            = 6;
    constexpr uint32_t LOAD_TOC              = 0x20;
    constexpr uint32_t AUTOLOAD_RLC          = 0x21;
    constexpr uint32_t FB_FW_RESERV_ADDR     = 0x50;
    constexpr uint32_t FB_FW_RESERV_EXT_ADDR = 0x51;
}

//
// psp_load_toc — port of upstream `psp_load_toc` (amdgpu_psp.c:840).
// REQUIRED for autoload-supported chips (psp_v14_0_2/3 with R9700).
// Without TOC parsed, PSP rejects every SDMA/CP/MES LOAD_IP_FW with
// TEE_BAD_PARAMETERS = 0xFFFF0006 because it doesn't know the TMR
// slot layout those firmwares are signed against.
//
// The TOC bin (gc_<v>_toc.bin) is itself a common-header firmware blob;
// upstream just copies the WHOLE FILE into fw_pri_buf and sends
// GFX_CMD_ID_LOAD_TOC. PSP reads it, validates, computes total TMR
// size needed, and writes that back as `resp.uresp.fw_reserve_info`...
// actually `cmd_buf_mem->resp.tmr_size` per upstream line 854.
//
kern_return_t psp_load_toc(DeviceContext &dev, PSPContext &psp,
                           const uint8_t *tocBin, uint32_t tocSize,
                           uint32_t *outTmrSize,
                           uint32_t *outRespStatus = nullptr);

// psp_load_toc_subbin — same submit as psp_load_toc, but takes the TOC
// SUB-BINARY (already payload, no common_firmware_header) that lives
// inside psp_<chip>_sos.bin. Mirrors what upstream psp_tmr_init does:
// it parses the TOC out of the SOS package via parse_sos_bin_descriptor
// (sets psp->toc.start_addr/size_bytes), then calls psp_load_toc which
// just psp_copy_fw's those bytes verbatim.
kern_return_t psp_load_toc_subbin(DeviceContext &dev, PSPContext &psp,
                                  const uint8_t *subBin, uint32_t subSize,
                                  uint32_t *outTmrSize,
                                  uint32_t *outRespStatus = nullptr);

//
// psp_rlc_autoload_start — port of upstream `psp_rlc_autoload_start`
// (amdgpu_psp.c:3434). After all GFX firmware has loaded
// (last = RLC_G), submit GFX_CMD_ID_AUTOLOAD_RLC so PSP starts
// the autoload sequence and bootstraps GFX/SDMA/MES/CP engines.
// Upstream calls this from `psp_load_non_psp_fw` immediately after
// the RLC_G `psp_execute_ip_fw_load` returns success.
//
kern_return_t psp_rlc_autoload_start(DeviceContext &dev, PSPContext &psp);

// psp_rl_load — port of upstream `psp_rl_load` (amdgpu_psp.c:1152).
// Submits the PSP-embedded Register List firmware via LOAD_IP_FW with
// fw_type=GFX_FW_TYPE_REG_LIST. Must be called AFTER psp_rlc_autoload_start
// — PSP's GC autoload state machine waits for REG_LIST to arrive before
// completing GC bringup.
kern_return_t psp_rl_load(DeviceContext &dev, PSPContext &psp);

//
// psp_parse_ta_microcode — port of upstream `parse_ta_v2_microcode`
// (amdgpu_psp.c:3947). Walks the v2.0 TA package header in `ta_data`
// (raw bytes of `psp_<chip>_ta.bin`), iterates ta_fw_bin descriptors,
// locates TA_FW_TYPE_PSP_ASD, BAR0-stages the ASD ucode into a
// dedicated VRAM slot inside fw_buf, and populates psp.asd with the
// staged ucode_mc_addr + size_bytes.
//
// The caller (LoadFirmware(kMacAMDGPUFwTypeTA)) provides the transient
// bin pointer; this function copies the ASD slice into VRAM before
// returning, so the bin pointer can be reused. psp.asd.parsed becomes
// true on success.
//
// On v1.0 TA headers (legacy ASICs not on our path) this returns
// kIOReturnUnsupported. R9700 ships v2.0 TA packages.
//
kern_return_t psp_parse_ta_microcode(DeviceContext &dev, PSPContext &psp,
                                     const uint8_t *ta_data,
                                     uint64_t ta_size);

//
// psp_asd_initialize — port of upstream `psp_asd_initialize`
// (amdgpu_psp.c:1225) + `psp_ta_load` (amdgpu_psp.c:1380) for the
// ASD case. Builds a GFX_CMD_ID_LOAD_ASD (0x4) cmd_buf pointing at
// the VRAM-staged ASD ucode (populated by psp_parse_ta_microcode)
// and submits via the PSP ring. PSP responds with the session_id
// which we stash in psp.asd.session_id (not currently used post-load,
// but kept for parity with upstream).
//
// PSP_ASD_SHARED_MEM_SIZE = 0 in upstream — ASD load does NOT allocate
// a shared memory region (unlike other TAs). cmd_buf_phy_addr / len
// are zero.
//
// Returns success ONLY if psp.asd.parsed && PSP resp_status == 0.
// If psp.asd.parsed is false, returns kIOReturnSuccess as a no-op
// (a missing TA bin shouldn't block bringup — log + skip).
//
// MUST be called AFTER psp_rlc_autoload_start and BEFORE psp_rl_load,
// matching upstream's order in psp_load_fw (amdgpu_psp.c:3153).
//
kern_return_t psp_asd_initialize(DeviceContext &dev, PSPContext &psp);

//
// psp_load_non_psp_fw — port of upstream amdgpu_psp.c:3051
//
// Loads all non-PSP firmware (SMU, IMU, RLC, CP, SDMA, MES) through
// the PSP ring in the correct upstream order. Mirrors the
// psp_load_non_psp_fw function from Linux's amdgpu_psp.c.
//
// The caller provides firmware data via the FirmwareLoader callback.
// Each callback receives a fw_type and returns (bus_addr, size) for
// the firmware payload. The caller is responsible for staging the
// firmware bytes into psp.fw_buf (VRAM) at the correct offset.
//
// Upstream order (AMDGPU_UCODE_ID_* enum):
//   1. SMU → 2. IMU_I + IMU_D → 3. RLC sub-bins (tap_delays → restore_list
//      → iram/dram → p/v → G) → 4. psp_rlc_autoload_start() →
//      5. CP_RS64 (PFP+stacks, ME+stacks, MEC+stacks) → 6. SDMA → 7. MES
//
kern_return_t psp_load_non_psp_fw(DeviceContext &dev, PSPContext &psp,
                                   const FirmwareLoader &loader);

//
// psp_query_fw_reservation — port of upstream `psp_update_fw_reservation`
// (amdgpu_psp.c:1040). For psp_v14_0_2/3 with SOS firmware
// >= 0x3a0e14, upstream sends GFX_CMD_ID_FB_FW_RESERV_ADDR +
// _EXT_ADDR via the ring right after ring_create and BEFORE any
// LOAD_IP_FW. Skipping this pair MAY leave PSP in a half-handshake
// state where it accepts wptr changes but discards every subsequent
// submit (one of three audit theories for the silent-drop bug we hit
// in v0.0.47..0.0.52).
//
// Each is a cmd_id-only frame (no payload). Response carries
// reserve_base_address + reserve_size which we don't actually use
// (the host doesn't manage VRAM allocation yet). PSP responds with
// PSP_ERR_UNKNOWN_COMMAND on older SOS, which we silently swallow.
//
// Returns kIOReturnSuccess if the ring submits go through (regardless
// of PSP's resp.status, since UNKNOWN_COMMAND is acceptable).
//
kern_return_t psp_query_fw_reservation(DeviceContext &dev, PSPContext &psp);

} // namespace amdgpu
