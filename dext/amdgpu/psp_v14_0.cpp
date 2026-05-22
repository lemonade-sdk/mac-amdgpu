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
#include <DriverKit/IOMemoryMap.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "amdgpu_psp.h"
#include "amdgpu_ucode_psp.h"
#include "amdgpu_gmc.h"

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
// firmware.fw_buf mirror — unique per-payload addresses for LOAD_IP_FW.
// GART PT lives at 0x00800000 (~8 MB, 128 KB long); place fw_buf well
// past that. 8 MB span is plenty for all gfx12 ucodes + stacks.
static constexpr uint64_t kFwBufVRAMOffset = 0x01000000; // 16 MB
static constexpr uint64_t kFwBufSize       = 0x00800000; // 8 MB

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

    // firmware.fw_buf mirror: dedicated VRAM region for per-payload
    // LOAD_IP_FW staging. Each LoadFirmware bumps fwBufBumpOffset by
    // its payload size (PAGE-aligned) and passes
    // `fwBufBaseMC + (old bump offset)` as cmd.fw_phy_addr — matching
    // upstream's `firmware.fw_buf_mc + per-ucode-offset` scheme.
    psp.fwBufVRAMOffset = kFwBufVRAMOffset;   // legacy; sysmem path overrides
    psp.fwBufBaseMC     = vram_start + kFwBufVRAMOffset;  // placeholder
    psp.fwBufSize       = kFwBufSize;
    psp.fwBufBumpOffset = 0;
    psp.fwBufSysmemBuffer  = nullptr;
    psp.fwBufSysmemDMA     = nullptr;
    psp.fwBufSysmemCPU     = nullptr;
    psp.fwBufSysmemBusAddr = 0;

    PSP_LOG("fw_pri ready VRAM-backed @ vram_off=%#llx mc=%#llx size=%llu "
            "(vram_start=%#llx); fw_buf @ vram_off=%#llx mc=%#llx size=%#llx "
            "(VRAM placeholder — psp_setup_fw_buf_sysmem replaces with GART)",
            (uint64_t)kFwPriVRAMOffset, psp.fwPriBusAddr,
            psp.fwPriSize, vram_start,
            (uint64_t)kFwBufVRAMOffset, psp.fwBufBaseMC,
            (uint64_t)kFwBufSize);
    return kIOReturnSuccess;
}

//============================================================
// psp_read_runtime_db — port of upstream psp_get_runtime_db_entry +
// the calls in psp_sw_init (amdgpu_psp.c:376-449, 471-503).
//
// Reads the PSP runtime data header at (vram_size - 0x100000), then
// walks the directory looking for PSP_RUNTIME_ENTRY_TYPE_BOOT_CONFIG
// (0x5) and PSP_RUNTIME_ENTRY_TYPE_PPTABLE_ERR_STATUS (0x6). Populates
// psp.bootCfgBitmask, psp.scpmStatus, psp.scpmEnabled.
//
// For psp_v14_0_3 the runtime DB is informational — IFWI POSTs the
// card so we don't actually need to drive memory training. We still
// read it so we have parity with upstream and can log non-default
// boot config flags if present.
//============================================================
kern_return_t
psp_read_runtime_db(DeviceContext &dev, PSPContext &psp,
                    uint64_t vram_size_bytes)
{
    if (psp.runtimeDbRead) return kIOReturnSuccess;  // idempotent

    constexpr uint64_t kPSP_RUNTIME_DB_OFFSET    = 0x100000ULL;
    constexpr uint16_t kPSP_RUNTIME_DB_COOKIE_ID = 0x0ed5;
    constexpr uint32_t kMAX_ENTRY_COUNT          = 0x40;
    constexpr uint32_t kENTRY_TYPE_BOOT_CONFIG   = 0x5;
    constexpr uint32_t kENTRY_TYPE_PPTABLE_ERR   = 0x6;

    if (vram_size_bytes <= kPSP_RUNTIME_DB_OFFSET) {
        PSP_LOG("runtime_db: vram_size %llu too small", vram_size_bytes);
        return kIOReturnNotReady;
    }

    const uint64_t db_pos = vram_size_bytes - kPSP_RUNTIME_DB_OFFSET;

    // Header: uint16 cookie + uint16 version (4 bytes).
    uint32_t hdr_dw = RVRAM32_via_mm(dev, db_pos);
    uint16_t cookie  = static_cast<uint16_t>(hdr_dw & 0xFFFFu);
    uint16_t version = static_cast<uint16_t>(hdr_dw >> 16);

    psp.runtimeDbCookie  = cookie;
    psp.runtimeDbVersion = version;
    psp.runtimeDbRead    = true;

    if (cookie != kPSP_RUNTIME_DB_COOKIE_ID) {
        PSP_LOG("runtime_db: cookie %#x at VRAM+%#llx (expected %#x) — "
                "runtime DB absent",
                cookie, db_pos, kPSP_RUNTIME_DB_COOKIE_ID);
        return kIOReturnSuccess;  // not an error — db just doesn't exist
    }

    // Directory: uint16 entry_count, then entry_list[N] of 8-byte entries.
    // entry_count lives in low 16 of the first dword after the header.
    const uint64_t dir_pos = db_pos + 4ULL;
    uint32_t dir_first_dw = RVRAM32_via_mm(dev, dir_pos);
    uint16_t entry_count = static_cast<uint16_t>(dir_first_dw & 0xFFFFu);

    if (entry_count >= kMAX_ENTRY_COUNT) {
        PSP_LOG("runtime_db: invalid entry_count=%u (max %u)",
                entry_count, kMAX_ENTRY_COUNT);
        return kIOReturnInvalid;
    }

    PSP_LOG("runtime_db: cookie=%#x ver=%#x entry_count=%u (db @ VRAM+%#llx)",
            cookie, version, entry_count, db_pos);

    // entry_list starts 4 bytes after dir_pos (after the entry_count u16
    // + 2-byte pad — directory header is itself dword-aligned).
    // Each entry: u32 type, u16 offset, u16 size = 8 bytes.
    const uint64_t entry_list_pos = dir_pos + 4ULL;

    for (uint16_t i = 0; i < entry_count; i++) {
        const uint64_t e_pos = entry_list_pos + (uint64_t)i * 8ULL;
        uint32_t e_type   = RVRAM32_via_mm(dev, e_pos);
        uint32_t e_offsz  = RVRAM32_via_mm(dev, e_pos + 4ULL);
        uint16_t e_offset = static_cast<uint16_t>(e_offsz & 0xFFFFu);
        uint16_t e_size   = static_cast<uint16_t>(e_offsz >> 16);

        if (e_type == kENTRY_TYPE_BOOT_CONFIG && e_size >= 4) {
            uint32_t bitmask = RVRAM32_via_mm(dev, db_pos + e_offset);
            psp.bootCfgBitmask = bitmask;
            PSP_LOG("runtime_db: BOOT_CONFIG bitmask=%#x", bitmask);
        } else if (e_type == kENTRY_TYPE_PPTABLE_ERR && e_size >= 4) {
            uint32_t status = RVRAM32_via_mm(dev, db_pos + e_offset);
            psp.scpmStatus = status;
            psp.scpmEnabled = (status != 0);
            PSP_LOG("runtime_db: PPTABLE_ERR scpm_status=%u (enabled=%d)",
                    status, psp.scpmEnabled ? 1 : 0);
        }
    }
    return kIOReturnSuccess;
}

//
// psp_setup_fw_buf_sysmem — port of upstream amdgpu_ucode_create_bo's
// default branch (amdgpu_ucode.c:1148-1166): allocate fw_buf in sysmem
// (Linux GTT, Mac DART-mapped IOBufferMemoryDescriptor), then bind it
// into GART so PSP sees a single contiguous MC-routable address space
// inside which each LOAD_IP_FW payload gets a unique offset.
//
// Replaces the VRAM-placeholder fw_buf set up by psp_init. After this
// runs, psp.fwBufBaseMC is the GART MC address — that's the address
// passed into LOAD_IP_FW.fw_phy_addr.
//
kern_return_t
psp_setup_fw_buf_sysmem(DeviceContext &dev, PSPContext &psp, GMCContext &gmc)
{
    if (psp.fwBufSysmemBuffer != nullptr) {
        return kIOReturnSuccess;  // idempotent
    }
    if (psp.fwBufSize == 0) {
        PSP_LOG("setup_fw_buf_sysmem: psp_init not called");
        return kIOReturnNotReady;
    }
    if (gmc.gart_start == 0) {
        PSP_LOG("setup_fw_buf_sysmem: GMC GART not enabled "
                "(gart_start=0) — bring up GMC first");
        return kIOReturnNotReady;
    }

    // Allocate the staging buffer as sysmem, kASPageSize-aligned (DART
    // requirement on Apple Silicon — see [[mac_amdgpu_as_dart_limits]]).
    // Mirrors upstream amdgpu_bo_create_kernel(adev->firmware.fw_size,
    // PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT, ...).
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t r = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, psp.fwBufSize, kASPageSize, &buf);
    if (r != kIOReturnSuccess || buf == nullptr) {
        PSP_LOG("setup_fw_buf_sysmem: buffer alloc failed: %#x", r);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    buf->SetLength(psp.fwBufSize);

    // DART-map: pin to a GPU-bus address. We require a SINGLE segment
    // because GART is set up to expect contiguous-PCI-addressed regions
    // (upstream's create_bo + amdgpu_ucode_init_bo make the same
    // assumption for the kernel-allocated firmware BO).
    //
    // maxAddressBits = 32 — force DART to allocate an IOVA that fits in
    // 32 bits so the AMD GPU's PCIe DMA can definitely reach it. With
    // maxAddressBits=64 DART placed our buffer at IOVA 0x82060000
    // (~2 GB), and PSP returned resp=0x11 even though our PT walk and
    // memcpy looked correct end-to-end — likely the GPU's PCIe path on
    // this Mac can't issue reads above some threshold below 0x82000000.
    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 32;
    IODMACommand *dma = nullptr;
    r = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions, &spec, &dma);
    if (r != kIOReturnSuccess || dma == nullptr) {
        buf->release();
        PSP_LOG("setup_fw_buf_sysmem: IODMACommand::Create failed: %#x", r);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    r = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
                           psp.fwBufSize, &flags, &segCount, &seg);
    if (r != kIOReturnSuccess || segCount != 1) {
        dma->release();
        buf->release();
        PSP_LOG("setup_fw_buf_sysmem: PrepareForDMA failed: %#x segs=%u",
                r, segCount);
        return r != kIOReturnSuccess ? r : kIOReturnNotAligned;
    }

    // Get a CPU-side mapping with cache INHIBITED. On Apple Silicon,
    // DART does not snoop CPU caches for TB5-attached devices — so any
    // CPU writes that linger in L1/L2 are invisible to the GPU's PCIe
    // reads (v0.1.12 self-test confirmed: writing 0xDEADBEEF via the
    // default cacheable mapping, then reading via MMHUB GMC, returned
    // 0s — the cached writes never made it to DRAM before the GPU
    // read). kIOMemoryMapCacheModeInhibit forces every CPU access
    // straight to DRAM, bypassing the cache.
    IOMemoryMap *uncachedMap = nullptr;
    r = buf->CreateMapping(
        kIOMemoryMapCacheModeInhibit,
        /*address  =*/ 0,
        /*offset   =*/ 0,
        /*length   =*/ psp.fwBufSize,
        /*alignment=*/ 0,
        &uncachedMap);
    if (r != kIOReturnSuccess || uncachedMap == nullptr) {
        dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma->release();
        buf->release();
        PSP_LOG("setup_fw_buf_sysmem: CreateMapping(uncached) kr=%#x", r);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    const uint64_t cpu_uncached_addr = uncachedMap->GetAddress();

    // Bind the whole bus range into GART. After this, GMC-routable MC
    // address `gart_mc + i*kASPageSize` resolves to sysmem bus address
    // `seg.address + i*kASPageSize`. Mirrors upstream's per-ucode
    // amdgpu_bo_gpu_offset(ucode->bo) which returns the GART-mapped
    // MC address for GTT-domain BOs.
    uint64_t gart_mc = 0;
    r = gmc_bind_existing(dev, gmc, seg.address, psp.fwBufSize, &gart_mc);
    if (r != kIOReturnSuccess) {
        dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma->release();
        buf->release();
        PSP_LOG("setup_fw_buf_sysmem: gmc_bind_existing failed: %#x", r);
        return r;
    }

    psp.fwBufSysmemBuffer  = buf;
    psp.fwBufSysmemDMA     = dma;
    psp.fwBufSysmemCPU     = reinterpret_cast<void *>(cpu_uncached_addr);
    psp.fwBufSysmemBusAddr = seg.address;
    psp.fwBufBaseMC        = gart_mc;          // <- now GART, not VRAM
    psp.fwBufBumpOffset    = 0;

    PSP_LOG("setup_fw_buf_sysmem: %llu B @ bus=%#llx → gart_mc=%#llx, "
            "cpu=%p (replaces VRAM-backed placeholder)",
            psp.fwBufSize, psp.fwBufSysmemBusAddr, psp.fwBufBaseMC,
            psp.fwBufSysmemCPU);

    // GART self-test: write a sentinel via the CPU pointer, flush the
    // CPU cache, then read it back via the GART MC address using
    // RVRAM32_via_mm (which goes through MMHUB GMC — the same walk
    // PSP performs for LOAD_IP_FW.fw_phy_addr).
    //
    // Three outcomes:
    //   - readback = sentinel : MMHUB GART walk works end-to-end.
    //     PSP's resp=0x11 on GART addresses is a PSP-side policy
    //     check (fw_phy_addr must be VRAM-resident on this chip).
    //   - readback = 0        : MMHUB walks but reads 0 — PTE
    //     translates but DART/sysmem doesn't return our data.
    //   - readback = 0xFFFFFFFF : walk faulted / aborted.
    //
    // Mirrors the diagnostic style of the smn-probe; results inform
    // whether to keep chasing GART or switch fw_buf to VRAM permanently.
    {
        uint32_t *cpu_dw = reinterpret_cast<uint32_t *>(psp.fwBufSysmemCPU);
        constexpr uint32_t kSentinel0 = 0xDEADBEEFu;
        constexpr uint32_t kSentinel1 = 0xCAFEBABEu;
        cpu_dw[0] = kSentinel0;
        cpu_dw[1] = kSentinel1;

        // Read the sentinels back THROUGH THE SAME CPU MAPPING — if
        // this returns 0, the writes never made it past the CPU side
        // (cache-inhibit mapping silently fell back to cacheable, or
        // is pointing at the wrong physical pages). If it returns the
        // sentinels, the writes ARE in DRAM and the MMHUB→DART read
        // is what's broken.
        uint32_t cpu_rb0 = cpu_dw[0];
        uint32_t cpu_rb1 = cpu_dw[1];

        // Read back via MMHUB GMC (same path PSP uses).
        uint32_t rb0 = RVRAM32_via_mm(dev, gart_mc + 0);
        uint32_t rb1 = RVRAM32_via_mm(dev, gart_mc + 4);

        PSP_LOG("gart-selftest: cpu-readback {%#x, %#x}  "
                "mmhub-readback {%#x, %#x}",
                cpu_rb0, cpu_rb1, rb0, rb1);
        PSP_LOG("gart-selftest: wrote {%#x, %#x} @ cpu=%p, "
                "readback via MMHUB @ gart_mc=%#llx → {%#x, %#x}  (%s)",
                kSentinel0, kSentinel1, psp.fwBufSysmemCPU,
                gart_mc, rb0, rb1,
                (rb0 == kSentinel0 && rb1 == kSentinel1)
                    ? "MATCH — GART walk works, PSP rejects GART addr"
                    : (rb0 == 0 && rb1 == 0)
                        ? "ZEROS — walk OK, DART/sysmem read returns 0"
                        : (rb0 == 0xFFFFFFFFu)
                            ? "ABORT — walk faulted"
                            : "MISMATCH");
    }
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
} __attribute__((packed));
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
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdSetupTmr) == 24,
              "PSPGfxCmdSetupTmr must be 24 B (matches upstream "
              "psp_gfx_cmd_setup_tmr)");

struct PSPGfxCmdLoadIpFw {
    uint32_t fw_phy_addr_lo;
    uint32_t fw_phy_addr_hi;
    uint32_t fw_size;
    uint32_t fw_type;           // enum psp_gfx_fw_type (host-endian u32)
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdLoadIpFw) == 16,
              "PSPGfxCmdLoadIpFw must be 16 B (matches upstream "
              "psp_gfx_cmd_load_ip_fw)");

// Layout of the cmd_resp buffer's leading fields. Matches upstream
// `struct psp_gfx_cmd_resp` for the bits we actually write. Per the
// AMD freedesktop thread (Patryk Simon's struct-packing trap finding,
// [[reference_amd_freedesktop_thread]]): explicitly mark __packed
// because Clang on macOS doesn't apply implicit packing the way Linux
// GCC + __le32 does. All-uint32_t means the static_assert should hold
// either way, but make it explicit + checked so it can't regress.
struct PSPGfxCmdRespHeader {
    uint32_t buf_size;
    uint32_t buf_version;       // PSP_GFX_CMD_BUF_VERSION = 1
    uint32_t cmd_id;
    uint32_t resp_buf_addr_lo;  // 0 for GPCOM ring
    uint32_t resp_buf_addr_hi;  // 0
    uint32_t resp_offset;       // 0
    uint32_t resp_buf_size;     // 0
    // cmd union starts at offset 28
} __attribute__((packed));
static_assert(sizeof(PSPGfxCmdRespHeader) == 28,
              "PSPGfxCmdRespHeader must be 28 B (cmd union starts at "
              "this offset in upstream psp_gfx_cmd_resp)");
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

    // VRAM-backed ring/cmd/fence — nothing to free on the CPU side;
    // VRAM allocator (when we have one) will reclaim the offsets.
    psp.ringCPUAddr  = nullptr;
    psp.ringBusAddr  = 0;
    psp.ringSize     = 0;
    psp.cmdCPUAddr   = nullptr;
    psp.cmdBusAddr   = 0;
    psp.fenceCPUAddr = nullptr;
    psp.fenceBusAddr = 0;
    psp.ringCreated  = false;

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

    // Sysmem-backed fw_buf (GART-bound) — release the IODMACommand +
    // IOBufferMemoryDescriptor pair. The GART page-table entries stay
    // until the next gmc reset; for now that's a non-issue because the
    // GMC bump allocator never reclaims slots either.
    if (psp.fwBufSysmemDMA != nullptr) {
        psp.fwBufSysmemDMA->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        psp.fwBufSysmemDMA->release();
        psp.fwBufSysmemDMA = nullptr;
    }
    if (psp.fwBufSysmemBuffer != nullptr) {
        psp.fwBufSysmemBuffer->release();
        psp.fwBufSysmemBuffer = nullptr;
    }
    psp.fwBufSysmemCPU     = nullptr;
    psp.fwBufSysmemBusAddr = 0;
    psp.fwBufBaseMC        = 0;
    psp.fwBufBumpOffset    = 0;
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

    // GART is now brought up by `BringupStage::GMCInit` which runs
    // BEFORE `PSPLoadSOS` per Agent D's reorder (audit #9 #1). The
    // legacy inline `if (psp.gart && !psp.gart->enabled)` shortcut
    // here is dead code on the new flow; the bringup orchestrator
    // guarantees GMC ran first.

    if (!psp_wait_for_bootloader(dev)) {
        return kIOReturnTimeout;
    }

    // Stage the SOS image in fw_pri (VRAM via BAR0 aperture).
    // Upstream psp_v14_0_bootloader_load_sos (psp_v14_0.c:217) memsets
    // the entire 1 MB fw_pri buffer to zero before memcpy — same as
    // for every other bootloader component (line 143). Without this,
    // stale bytes from prior loads sit at fw_pri[sosFirmwareSize..1MB]
    // and PSP may hash/validate past the declared size, rejecting
    // with status 0x11 (signature/size mismatch). Our own psp_load_toc
    // code already documents this exact failure mode. The ~2.6 sec
    // BAR0-memset cost is acceptable once at init time.
    bar0_memset_vram(dev, kFwPriVRAMOffset, 0, psp.fwPriSize);
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
    // Upstream psp_v14_0_bootloader_load_component (psp_v14_0.c:143)
    // memsets the entire 1 MB fw_pri buffer to zero before memcpy.
    // Skipping the memset leaves stale bytes from prior bootloader
    // loads at fw_pri[binSize..1MB]; PSP may hash/validate beyond the
    // declared size and reject with status 0x11 (signature/size
    // mismatch) — same failure mode our psp_load_toc code documents.
    bar0_memset_vram(dev, kFwPriVRAMOffset, 0, psp.fwPriSize);
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

    // PSP ring/cmd/fence live in VRAM addressed via the FB aperture
    // (MC = vram_start + vram_offset) — NOT via GART. PSP's internal
    // fetch path for RB frames bypasses MMHUB/GFXHUB and dereferences
    // the address as a raw VRAM physical offset. Confirmed by upstream
    // amdgpu_bo_fb_aper_addr() at amdgpu_object.c:1493 + psp_update_
    // gpu_addresses() at amdgpu_psp.c:2475-2486. GART remains up — it's
    // still needed for future runtime GTT-domain BOs.
    uint64_t vram_start = psp_read_vram_start(dev);
    if (vram_start == 0) {
        PSP_LOG("ring_create: vram_start=0 — MMHUB not ready");
        return kIOReturnNotReady;
    }

    psp.ringBusAddr = vram_start + kRingVRAMOffset;
    psp.ringCPUAddr = nullptr;  // VRAM-backed; CPU access via BAR0
    psp.ringSize    = kPSPKMRingSize;
    // Zero the full ring buffer in VRAM via BAR0.
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

    // 3. Wait for response flag. poll_psp_response surfaces PSP-side
    // error statuses (bit 31 set + non-zero low 16) as kIOReturnIOError
    // instead of letting us spin until timeout.
    kern_return_t pr = poll_psp_response(dev, reg64,
                                         kPSPMboxRespMask, kPSPMboxRespFlag,
                                         5 * 1000000, &v);
    if (pr != kIOReturnSuccess) {
        uint32_t now81 = RREG32(dev, reg81);
        const char *why = (pr == kIOReturnIOError) ? "ERROR" : "TIMEOUT";
        PSP_LOG("ring_create: response wait %{public}s — "
                "final C2PMSG_64=%#010x (status=%#x) C2PMSG_81=%#010x",
                why, v, v & 0xFFFFu, now81);
        return pr;
    }

    psp.ringCreated = true;
    PSP_LOG("ring_created mc=%#llx size=%llu (response=%#010x)",
            psp.ringBusAddr, psp.ringSize, v);

    // VRAM-backed cmd + fence buffers (FB-aperture MC addresses).
    psp.cmdBusAddr   = vram_start + kCmdBufVRAMOffset;
    psp.cmdCPUAddr   = nullptr;  // VRAM-backed; CPU access via BAR0
    psp.fenceBusAddr = vram_start + kFenceVRAMOffset;
    psp.fenceCPUAddr = nullptr;
    psp.fenceCounter = 0;
    bar0_memset_vram(dev, kCmdBufVRAMOffset,  0, kPSPCmdBufSize);
    bar0_memset_vram(dev, kFenceVRAMOffset, 0, kPSPFenceBufSize);

    PSP_LOG("cmd_buf mc=%#llx (vram_off=%#llx) fence_buf mc=%#llx (vram_off=%#llx)",
            psp.cmdBusAddr, (uint64_t)kCmdBufVRAMOffset,
            psp.fenceBusAddr, (uint64_t)kFenceVRAMOffset);
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

    // 2. Locate the frame slot by VRAM byte offset (ring is in VRAM).
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
    const uint64_t frameVRAMOff = kRingVRAMOffset +
        (uint64_t)frame_slot * sizeof(PSPGfxRBFrame);

    // 3. Stage the command buffer in VRAM via BAR0.
    bar0_memcpy_to_vram(dev, kCmdBufVRAMOffset, cmd, cmdSize);

    // 4. Bump fence counter, zero the fence dword in VRAM, build the
    //    frame in a stack buffer and copy it into the ring slot.
    uint32_t fence_index = ++psp.fenceCounter;
    WBAR0_32(dev, kFenceVRAMOffset, 0);

    // Upstream amdgpu_psp.c:3485-3492 — memset frame to zero, then set
    // ONLY these 5 fields. cmd_buf_size, sid, vmid, frame_type, all
    // reserved fields stay zero. Setting cmd_buf_size to a non-zero
    // value (we used to write 0x400) makes PSP silently drop the
    // frame — match upstream exactly.
    PSPGfxRBFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(psp.cmdBusAddr >> 32);
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(psp.cmdBusAddr & 0xFFFFFFFFu);
    frame.fence_addr_hi   = static_cast<uint32_t>(psp.fenceBusAddr >> 32);
    frame.fence_addr_lo   = static_cast<uint32_t>(psp.fenceBusAddr & 0xFFFFFFFFu);
    frame.fence_value     = fence_index;
    bar0_memcpy_to_vram(dev, frameVRAMOff, &frame, sizeof(frame));

    // 5a. HDP flush — drain host-side write buffers so PSP sees our
    //     ring frame + cmd_buf. Upstream calls amdgpu_device_flush_hdp
    //     here in psp_ring_cmd_submit.
    amdgpu_hdp_flush(dev);

    // 5b. Advance and publish wptr (in dwords).
    wptr_dw = (wptr_dw + frameSizeDw) % ringSizeDw;
    WREG32(dev, regWptr, wptr_dw);

    // 6. Wait for PSP to write the fence value into VRAM. Read it back
    //    via the BAR0 aperture — BAR0 reads bypass any CPU cache and go
    //    straight to PCIe / VRAM, so we always see the latest value.
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

    // 7. Read response status from cmd buffer in VRAM via BAR0.
    //    Upstream psp_gfx_cmd_resp.resp.status lives at offset 864
    //    (psp_gfx_if.h struct layout).
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
// psp_setup_tmr — port of upstream psp_tmr_load (amdgpu_psp.c:916).
//
// IMPORTANT — on psp_v14_0 IP_VERSION(14,0,2) and (14,0,3) (R9700 and
// related), upstream's `psp_skip_tmr` returns true because the chip
// defaults both `boot_time_tmr = true` and `autoload_supported = true`
// (amdgpu_psp.c:173-174, no override for 14,0,2/3 at lines 257-261).
// PSP's SOS sets up TMR itself during boot, before the driver's ring
// is even created. Sending SETUP_TMR after SOS boot is silently
// dropped by PSP (the symptom we hit on v0.0.47/48: ring submit lands
// in C2PMSG_67 but fence_buf never gets written).
//
// So for v14_0_2/3 we skip the submit entirely. Tracked state still
// flips to tmrSetUp = true so downstream LOAD_IP_FW commands are
// allowed to proceed — PSP knows where its TMR is, we don't need to.
//
kern_return_t
psp_setup_tmr(DeviceContext &dev, PSPContext &psp)
{
    if (psp.tmrSetUp) return kIOReturnSuccess;
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }

    // psp_v14_0_x: boot_time_tmr + autoload → skip SETUP_TMR. SOS owns
    // the TMR allocation. But upstream's psp_tmr_init (amdgpu_psp.c:881-890)
    // ALSO calls psp_load_toc when `psp->toc.start_addr` is non-null —
    // and on psp_v14_0_3 it IS non-null because parse_sos_bin_descriptor
    // extracts the PSP_FW_TYPE_PSP_TOC sub-bin from psp_14_0_3_sos.bin.
    // Without that LOAD_TOC submit, PSP doesn't know the per-IP TMR slot
    // layout, so it rejects every TMR-resident LOAD_IP_FW (SDMA/CP_RS64/
    // MES/RLC) with TEE_BAD_PARAMETERS (0xFFFF0006) and AUTOLOAD_RLC
    // returns TEE_ERROR_ITEM_NOT_FOUND (0xFFFF0007). SMU + IMU pass
    // because they bypass TMR slotting.
    if (kIP_PSP.major == 14) {
        psp.tmrBusAddr = 0;  // SOS-managed; we have no MC address
        psp.tmrCPUAddr = nullptr;
        psp.tmrSize    = 0;

        if (psp.toc.start_addr != nullptr && psp.toc.size_bytes > 0) {
            uint32_t tmr_size = 0, toc_resp = 0;
            kern_return_t tr = psp_load_toc_subbin(
                dev, psp,
                psp.toc.start_addr,
                static_cast<uint32_t>(psp.toc.size_bytes),
                &tmr_size, &toc_resp);
            if (tr != kIOReturnSuccess) {
                PSP_LOG("setup_tmr: LOAD_TOC sub-bin FAILED kr=%#x "
                        "resp=%#x (toc_size=%llu); TMR-resident "
                        "LOAD_IP_FW will likely reject",
                        tr, toc_resp, psp.toc.size_bytes);
                return tr;
            }
            PSP_LOG("setup_tmr: LOAD_TOC sub-bin ok — PSP reported "
                    "tmr_size=%u (%#x) from %llu-byte sub-bin",
                    tmr_size, tmr_size, psp.toc.size_bytes);
        } else {
            PSP_LOG("setup_tmr: NO TOC sub-bin in SOS package "
                    "(toc.start_addr=%p size=%llu) — skipping LOAD_TOC; "
                    "expect TMR-resident LOAD_IP_FW to fail",
                    psp.toc.start_addr, psp.toc.size_bytes);
        }

        psp.tmrSetUp = true;
        PSP_LOG("setup_tmr: SKIP SETUP_TMR (psp_v14_0_%u — boot_time_tmr "
                "by SOS, see psp_skip_tmr in upstream amdgpu_psp.c:906)",
                (unsigned)kIP_PSP.rev);
        return kIOReturnSuccess;
    }

    // Legacy / other PSP families that DO accept SETUP_TMR from the
    // driver — port of upstream psp_setup_tmr (amdgpu_psp.c:825 sets
    // virt_phy_addr = 1 unconditionally; system_phy_addr is the
    // CPU-visible BAR phys address of the TMR BO).
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

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 — upstream's
    // acquire_psp_cmd_buf memsets the whole struct and never writes
    // these fields; PSP firmware on v14_0_3 may treat nonzero values
    // as version mismatch. Only cmd_id is set.
    hdr->cmd_id      =PSPGfxCmd::SETUP_TMR;

    auto *tmr = reinterpret_cast<PSPGfxCmdSetupTmr *>(cmd_buf + 28);
    tmr->buf_phy_addr_lo    = static_cast<uint32_t>(psp.tmrBusAddr & 0xFFFFFFFFu);
    tmr->buf_phy_addr_hi    = static_cast<uint32_t>(psp.tmrBusAddr >> 32);
    tmr->buf_size           = static_cast<uint32_t>(psp.tmrSize);
    tmr->tmr_flags          = 0x2;  // bit 1 = virt_phy_addr per upstream
    tmr->system_phy_addr_lo = static_cast<uint32_t>(psp.tmrBusAddr & 0xFFFFFFFFu);
    tmr->system_phy_addr_hi = static_cast<uint32_t>(psp.tmrBusAddr >> 32);

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
    // Upstream `psp_prep_load_ip_fw_cmd_buf` does NOT enforce 4 KB
    // alignment on fw_phy_addr — it just stuffs whatever address came
    // out of `amdgpu_bo_gpu_offset(bo) + ucode_array_offset_bytes`.
    // The BO is page-aligned, but ucode_array_offset_bytes is typically
    // 32 (= sizeof(common_firmware_header)), so the resulting address
    // is dword-aligned, not 4 KB. PSP accepts that. Our previous 4 KB
    // alignment check rejected every real LOAD_IP_FW call with
    // kIOReturnNotAligned. Keep only the minimal dword-alignment check.
    if (fwBusAddr & 0x3) {
        return kIOReturnNotAligned;
    }

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 — upstream's
    // acquire_psp_cmd_buf memsets the whole struct and never writes
    // these fields; PSP firmware on v14_0_3 may treat nonzero values
    // as version mismatch. Only cmd_id is set.
    hdr->cmd_id      =PSPGfxCmd::LOAD_IP_FW;

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

kern_return_t
psp_query_fw_reservation(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }
    auto submit_one = [&](uint32_t cmd_id, const char *name) -> kern_return_t {
        uint8_t cmd_buf[kPSPGfxCmdRespSize];
        memset(cmd_buf, 0, sizeof(cmd_buf));
        auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
        // buf_size/buf_version left at 0 (upstream behavior).
        hdr->cmd_id      = cmd_id;
        uint32_t resp = 0;
        kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                              kPSPGfxCmdRespSize, &resp);
        if (r == kIOReturnSuccess) {
            // Pull reserve_base_address + reserve_size out of the response
            // buffer. Layout: psp_gfx_uresp_fw_reserve_info lives inside
            // psp_gfx_resp.uresp (offset +64 within resp). resp sits at
            // +864 in cmd_buf. Field order is hi-then-lo (unusual).
            constexpr uint32_t kUrespOff = 864 + 64;
            uint32_t addr_hi = RBAR2_32(dev, kCmdBufVRAMOffset + kUrespOff + 0);
            uint32_t addr_lo = RBAR2_32(dev, kCmdBufVRAMOffset + kUrespOff + 4);
            uint32_t rsv_sz  = RBAR2_32(dev, kCmdBufVRAMOffset + kUrespOff + 8);
            uint64_t addr    = ((uint64_t)addr_hi << 32) | addr_lo;
            PSP_LOG("query_fw_reservation: %{public}s ok (resp=%#x) "
                    "addr=%#llx size=%#x",
                    name, resp, addr, rsv_sz);
            return kIOReturnSuccess;
        }
        // PSP_ERR_UNKNOWN_COMMAND (0x100) means SOS too old to know this
        // cmd — upstream treats it as success+(addr=0,size=0). For us,
        // the ring submit itself was processed; only the chip's response
        // says "I don't know this command", which is fine.
        if (r == kIOReturnError && (resp & 0xFFFFu) == 0x100) {
            PSP_LOG("query_fw_reservation: %{public}s — SOS doesn't "
                    "implement (resp=%#x); ignoring", name, resp);
            return kIOReturnSuccess;
        }
        PSP_LOG("query_fw_reservation: %{public}s FAILED kr=%#x resp=%#x",
                name, r, resp);
        return r;
    };
    kern_return_t r1 = submit_one(PSPGfxCmd::FB_FW_RESERV_ADDR,
                                  "FB_FW_RESERV_ADDR");
    if (r1 != kIOReturnSuccess) return r1;
    kern_return_t r2 = submit_one(PSPGfxCmd::FB_FW_RESERV_EXT_ADDR,
                                  "FB_FW_RESERV_EXT_ADDR");
    return r2;
}

//
// psp_load_toc — port of upstream `psp_load_toc` (amdgpu_psp.c:840).
// Stages the gc_<v>_toc.bin file in fw_pri (VRAM via BAR0) and submits
// GFX_CMD_ID_LOAD_TOC. PSP parses the TOC, validates, and writes the
// total TMR size needed for autoload back into the cmd_buf response.
// Must run AFTER ring_create + FB_FW_RESERV queries but BEFORE any
// LOAD_IP_FW for SDMA / CP_RS64 / MES (those firmwares occupy TMR
// slots whose layout PSP only knows once the TOC is parsed).
//
kern_return_t
psp_load_toc(DeviceContext &dev, PSPContext &psp,
             const uint8_t *tocBin, uint32_t tocSize,
             uint32_t *outTmrSize, uint32_t *outRespStatus)
{
    if (!psp.ringCreated) {
        PSP_LOG("load_toc: ring not created");
        return kIOReturnNotReady;
    }
    if (tocBin == nullptr || tocSize < sizeof(common_firmware_header)) {
        return kIOReturnBadArgument;
    }

    // Parse common header — upstream psp_init_toc_microcode pulls
    // payload offset + size from `header.ucode_array_offset_bytes` and
    // `header.ucode_size_bytes`, then psp_load_toc only copies/submits
    // that PAYLOAD (NOT the whole file). Sending the file as-is gets
    // rejected by PSP with status 0x11 because the signature & size
    // don't match what was signed.
    auto *hdr = reinterpret_cast<const common_firmware_header *>(tocBin);
    uint32_t payload_offset = hdr->ucode_array_offset_bytes;
    uint32_t payload_size   = hdr->ucode_size_bytes;
    if (payload_offset == 0 ||
        (uint64_t)payload_offset + payload_size > tocSize) {
        PSP_LOG("load_toc: header bad — off=%u size=%u file=%u",
                payload_offset, payload_size, tocSize);
        return kIOReturnBadArgument;
    }
    if (payload_size > psp.fwPriSize) {
        PSP_LOG("load_toc: payload %u B > fw_pri %llu B",
                payload_size, psp.fwPriSize);
        return kIOReturnNoSpace;
    }

    // 1. Stage the TOC PAYLOAD ONLY in fw_pri via BAR0. Mirrors
    //    upstream psp_copy_fw(psp, psp->toc.start_addr, psp->toc.size_bytes)
    //    — which zeros the WHOLE fw_pri buffer first (amdgpu_psp.c:4191)
    //    THEN memcpys the payload. Without the zero, stale bytes from the
    //    prior SOS sub-bin loads sit at fw_pri[payload_size..]; PSP reads
    //    or hashes past the declared toc_size and rejects with status 0x11
    //    (signature/size mismatch).
    bar0_memset_vram(dev, kFwPriVRAMOffset, 0, psp.fwPriSize);
    bar0_memcpy_to_vram(dev, kFwPriVRAMOffset,
                        tocBin + payload_offset, payload_size);
    amdgpu_hdp_flush(dev);

    // 2. Build LOAD_TOC frame.
    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *cmd_hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 (upstream behavior).
    cmd_hdr->cmd_id      =PSPGfxCmd::LOAD_TOC;

    // psp_gfx_cmd_load_toc layout (psp_gfx_if.h):
    //   uint32_t toc_phy_addr_lo;  // +0
    //   uint32_t toc_phy_addr_hi;  // +4
    //   uint32_t toc_size;         // +8 — PAYLOAD size, NOT file size
    auto *toc = reinterpret_cast<uint32_t *>(cmd_buf + 28);
    toc[0] = static_cast<uint32_t>(psp.fwPriBusAddr & 0xFFFFFFFFu);
    toc[1] = static_cast<uint32_t>(psp.fwPriBusAddr >> 32);
    toc[2] = payload_size;

    uint32_t resp = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp);
    if (outRespStatus) *outRespStatus = resp;
    if (r != kIOReturnSuccess) {
        PSP_LOG("LOAD_TOC FAILED kr=%#x resp=%#x (payload off=%u size=%u)",
                r, resp, payload_offset, payload_size);
        return r;
    }

    // 3. Read tmr_size from response. psp_gfx_resp.tmr_size lives at
    //    offset 16 (per psp_gfx_if.h response struct).
    uint32_t tmr_size = RBAR2_32(dev, kCmdBufVRAMOffset + 16);
    if (outTmrSize) *outTmrSize = tmr_size;
    PSP_LOG("LOAD_TOC ok — PSP reported tmr_size=%u (%#x)",
            tmr_size, tmr_size);
    return kIOReturnSuccess;
}

//
// psp_load_toc_subbin — submit GFX_CMD_ID_LOAD_TOC using the TOC
// SUB-BINARY that lives INSIDE psp_<chip>_sos.bin (v2 descriptor
// PSP_FW_TYPE_PSP_TOC). Unlike `psp_load_toc` which takes a separate
// gc_<v>_toc.bin file and slices its common_firmware_header, this
// helper expects `subBin/subSize` to ALREADY be the payload bytes —
// the SOS parser (`psp_parse_sos_microcode`) extracts those.
//
// Mirrors upstream `psp_tmr_init`'s branch (amdgpu_psp.c:881-890):
//   if (psp->toc.start_addr && psp->toc.size_bytes && psp->fw_pri_buf)
//       psp_load_toc(psp, &tmr_size);
// where `psp_load_toc` body (amdgpu_psp.c:840-859) is:
//   psp_copy_fw(psp, psp->toc.start_addr, psp->toc.size_bytes);
//   psp_prep_load_toc_cmd_buf(cmd, psp->fw_pri_mc_addr, psp->toc.size_bytes);
//   psp_cmd_submit_buf(...)
//
// PSP reads the TOC, builds the TMR slot map for SDMA/CP_RS64/MES/RLC,
// and returns tmr_size. Without this, every TMR-resident LOAD_IP_FW
// gets rejected with TEE_BAD_PARAMETERS, and AUTOLOAD_RLC returns
// TEE_ERROR_ITEM_NOT_FOUND (0xFFFF0007) — the symptom we observe.
//
kern_return_t
psp_load_toc_subbin(DeviceContext &dev, PSPContext &psp,
                   const uint8_t *subBin, uint32_t subSize,
                   uint32_t *outTmrSize, uint32_t *outRespStatus)
{
    if (!psp.ringCreated) {
        PSP_LOG("load_toc_subbin: ring not created");
        return kIOReturnNotReady;
    }
    if (subBin == nullptr || subSize == 0) {
        PSP_LOG("load_toc_subbin: empty (subBin=%p subSize=%u)",
                subBin, subSize);
        return kIOReturnBadArgument;
    }
    if (subSize > psp.fwPriSize) {
        PSP_LOG("load_toc_subbin: payload %u B > fw_pri %llu B",
                subSize, psp.fwPriSize);
        return kIOReturnNoSpace;
    }

    // Mirror upstream psp_copy_fw: zero whole 1 MB fw_pri, memcpy
    // payload at offset 0.
    bar0_memset_vram(dev, kFwPriVRAMOffset, 0, psp.fwPriSize);
    bar0_memcpy_to_vram(dev, kFwPriVRAMOffset, subBin, subSize);
    amdgpu_hdp_flush(dev);

    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *cmd_hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 (upstream behavior).
    cmd_hdr->cmd_id      =PSPGfxCmd::LOAD_TOC;

    auto *toc = reinterpret_cast<uint32_t *>(cmd_buf + 28);
    toc[0] = static_cast<uint32_t>(psp.fwPriBusAddr & 0xFFFFFFFFu);
    toc[1] = static_cast<uint32_t>(psp.fwPriBusAddr >> 32);
    toc[2] = subSize;

    uint32_t resp = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp);
    if (outRespStatus) *outRespStatus = resp;
    if (r != kIOReturnSuccess) {
        PSP_LOG("LOAD_TOC (sub-bin) FAILED kr=%#x resp=%#x "
                "(size=%u, fw_pri_mc=%#llx)",
                r, resp, subSize, psp.fwPriBusAddr);
        return r;
    }

    uint32_t tmr_size = RBAR2_32(dev, kCmdBufVRAMOffset + 16);
    if (outTmrSize) *outTmrSize = tmr_size;
    PSP_LOG("LOAD_TOC (sub-bin) ok — PSP reported tmr_size=%u (%#x)",
            tmr_size, tmr_size);
    return kIOReturnSuccess;
}

//
// psp_rlc_autoload_start — port of upstream `psp_rlc_autoload_start`
// (amdgpu_psp.c:3434). Submits a cmd-id-only frame
// (GFX_CMD_ID_AUTOLOAD_RLC = 0x21) telling SOS that all GFX firmware
// has been pre-staged; SOS then drives the per-IP autoload sequence.
// Caller MUST have already loaded all RS64 CP / MES / IMU / RLC
// sub-bins ending with RLC_G — this command kicks off the actual
// engine bringup PSP-side.
//
kern_return_t
psp_rlc_autoload_start(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) {
        return kIOReturnNotReady;
    }
    uint8_t cmd_buf[kPSPGfxCmdRespSize];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    // buf_size/buf_version intentionally left at 0 — upstream's
    // acquire_psp_cmd_buf memsets the whole struct and never writes
    // these fields; PSP firmware on v14_0_3 may treat nonzero values
    // as version mismatch. Only cmd_id is set.
    hdr->cmd_id      =PSPGfxCmd::AUTOLOAD_RLC;

    uint32_t resp = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf,
                                          kPSPGfxCmdRespSize, &resp);
    if (r != kIOReturnSuccess) {
        PSP_LOG("rlc_autoload_start: FAILED kr=%#x resp=%#x", r, resp);
        return r;
    }
    PSP_LOG("rlc_autoload_start: ok (resp=%#x)", resp);
    return kIOReturnSuccess;
}

//
// psp_rl_load — direct port of upstream `psp_rl_load`
// (amdgpu_psp.c:1152). Submits the PSP-embedded Register List firmware
// via LOAD_IP_FW with fw_type=GFX_FW_TYPE_REG_LIST=67. The RL bytes
// were extracted from the v2 SOS package by psp_parse_sos_microcode
// into psp.rl (PSP_FW_TYPE_PSP_RL sub-bin descriptor).
//
// CRITICAL for autoload: upstream calls this right after
// psp_load_non_psp_fw (which ended with AUTOLOAD_RLC). PSP's autoload
// state machine waits for REG_LIST to arrive before completing GC
// bringup — without it, BOOTLOAD_STATUS never transitions and GC
// stays in reset even though all other firmware loads acknowledged.
//
// Mirrors upstream byte-for-byte:
//   memset(psp->fw_pri_buf, 0, PSP_1_MEG);
//   memcpy(psp->fw_pri_buf, psp->rl.start_addr, psp->rl.size_bytes);
//   cmd.cmd_load_ip_fw.fw_phy_addr = psp->fw_pri_mc_addr;
//   cmd.cmd_load_ip_fw.fw_size     = psp->rl.size_bytes;
//   cmd.cmd_load_ip_fw.fw_type     = GFX_FW_TYPE_REG_LIST;
//
// We use the same fwPri buffer that worked for SOS/bootloader loading
// (vram_start + 0, 1 MB). Sysmem-via-GART is unavailable on Apple
// Silicon (see [[feedback_mac_amdgpu_dart_tb5_pcie_reads]]).
//
kern_return_t
psp_rl_load(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) return kIOReturnNotReady;
    if (psp.rl.start_addr == nullptr || psp.rl.size_bytes == 0) {
        PSP_LOG("rl_load: psp.rl absent (size=%llu, ptr=%p) — skipping "
                "(SOS package may not include RL)",
                (unsigned long long)psp.rl.size_bytes, psp.rl.start_addr);
        return kIOReturnSuccess;
    }
    if (psp.fwPriSize == 0) {
        PSP_LOG("rl_load: fwPri not initialized");
        return kIOReturnNotReady;
    }
    if (psp.rl.size_bytes > psp.fwPriSize) {
        PSP_LOG("rl_load: RL %llu > fwPri %llu — too large",
                (unsigned long long)psp.rl.size_bytes,
                (unsigned long long)psp.fwPriSize);
        return kIOReturnNoMemory;
    }

    // memset fwPri to zero (PSP_1_MEG = 1 MB region), then memcpy
    // RL bytes into it. fwPri lives at VRAM offset 0, so we use
    // bar0_memset_vram + bar0_memcpy_to_vram for both ops.
    bar0_memset_vram(dev, kFwPriVRAMOffset, 0, psp.fwPriSize);
    bar0_memcpy_to_vram(dev, kFwPriVRAMOffset,
                        psp.rl.start_addr, psp.rl.size_bytes);
    amdgpu_hdp_flush(dev);

    // Submit LOAD_IP_FW with fw_phy_addr = fwPriBusAddr, fw_type = REG_LIST.
    kern_return_t r = psp_load_ip_fw(dev, psp, psp.fwPriBusAddr,
                                     static_cast<uint32_t>(psp.rl.size_bytes),
                                     PSPGfxFwType::REG_LIST);
    if (r != kIOReturnSuccess) {
        PSP_LOG("rl_load: LOAD_IP_FW(REG_LIST, size=%llu, mc=%#llx) "
                "FAILED kr=%#x",
                (unsigned long long)psp.rl.size_bytes,
                psp.fwPriBusAddr, r);
        return r;
    }
    PSP_LOG("rl_load: ok (size=%llu, mc=%#llx)",
            (unsigned long long)psp.rl.size_bytes, psp.fwPriBusAddr);
    return kIOReturnSuccess;
}

//============================================================
// psp_parse_ta_microcode — port of upstream parse_ta_v2_microcode
// (amdgpu_psp.c:3947) for the ASD sub-binary specifically.
//
// psp_<chip>_ta.bin layout (v2.0):
//   common_firmware_header (32 B)
//   uint32_t ta_fw_bin_count
//   psp_fw_bin_desc ta_fw_bin[ta_fw_bin_count]  (16 B each)
//   ucode_array starts at common_firmware_header.ucode_array_offset_bytes
//
// Each ta_fw_bin descriptor:
//   fw_type     — ta_fw_type enum (TA_FW_TYPE_PSP_ASD = 1, etc.)
//   fw_version
//   offset_bytes — relative to ucode_array_offset_bytes
//   size_bytes
//
// We walk the array looking for TA_FW_TYPE_PSP_ASD, BAR0-stage the
// ASD ucode into a dedicated fwBuf VRAM slot, and store the resulting
// MC address + size in psp.asd. psp_asd_initialize later builds a
// LOAD_ASD cmd referencing that slot.
//============================================================

kern_return_t
psp_parse_ta_microcode(DeviceContext &dev, PSPContext &psp,
                       const uint8_t *ta_data, uint64_t ta_size)
{
    // Reset the ASD context — fresh parse, no stale pointers.
    psp.asd = PSPContext::ASDContext{};

    if (ta_data == nullptr || ta_size < sizeof(common_firmware_header)) {
        PSP_LOG("parse_ta: ta_data null or too small (%llu)", ta_size);
        return kIOReturnBadArgument;
    }
    auto *hdr = reinterpret_cast<const common_firmware_header *>(ta_data);
    if (hdr->ucode_array_offset_bytes > ta_size) {
        PSP_LOG("parse_ta: ucode_array_offset %#x > ta_size %llu",
                hdr->ucode_array_offset_bytes, ta_size);
        return kIOReturnBadArgument;
    }

    PSP_LOG("parse_ta: hdr ver %u.%u, ip %u.%u, ucode_size=%u, "
            "ucode_off=%#x, total=%u",
            hdr->header_version_major, hdr->header_version_minor,
            hdr->ip_version_major, hdr->ip_version_minor,
            hdr->ucode_size_bytes,
            hdr->ucode_array_offset_bytes, hdr->size_bytes);

    // R9700 ships v2.0 TA packages. v1.0 would need a different parser
    // (legacy named-field layout) — we don't support those chips here.
    if (hdr->header_version_major != 2) {
        PSP_LOG("parse_ta: unsupported header_version_major=%u "
                "(only v2.0 supported)", hdr->header_version_major);
        return kIOReturnUnsupported;
    }

    auto *h20 = reinterpret_cast<const ta_firmware_header_v2_0 *>(ta_data);
    uint32_t count = h20->ta_fw_bin_count;
    if (count == 0 || count > 64) {
        PSP_LOG("parse_ta: implausible ta_fw_bin_count=%u", count);
        return kIOReturnBadArgument;
    }
    // Make sure the descriptor array fits inside the file.
    uint64_t arr_end = sizeof(ta_firmware_header_v2_0) +
                       (uint64_t)count * sizeof(psp_fw_bin_desc);
    if (arr_end > ta_size) {
        PSP_LOG("parse_ta: bin array (count=%u) exceeds ta_size", count);
        return kIOReturnBadArgument;
    }

    const uint8_t *ucode_base = ta_data + hdr->ucode_array_offset_bytes;
    const psp_fw_bin_desc *bin = h20->ta_fw_bin;

    // Iterate. Log every descriptor for visibility; only stash ASD.
    bool found_asd = false;
    for (uint32_t i = 0; i < count; i++) {
        PSP_LOG("parse_ta: desc[%u] fw_type=%u version=%#x offset=%u size=%u",
                i, bin[i].fw_type, bin[i].fw_version,
                bin[i].offset_bytes, bin[i].size_bytes);
        if (bin[i].fw_type != TA_FW_TYPE_PSP_ASD) continue;
        if (bin[i].size_bytes == 0) {
            PSP_LOG("parse_ta: ASD descriptor has zero size — skip");
            continue;
        }
        // Boundary check the ASD slice lives inside the file.
        if ((uint64_t)hdr->ucode_array_offset_bytes +
            bin[i].offset_bytes + bin[i].size_bytes > ta_size) {
            PSP_LOG("parse_ta: ASD ucode (off=%u + size=%u) exceeds ta_size",
                    bin[i].offset_bytes, bin[i].size_bytes);
            return kIOReturnBadArgument;
        }
        const uint8_t *asd_src = ucode_base + bin[i].offset_bytes;
        uint32_t asd_size      = bin[i].size_bytes;

        // Page-align the slot size to match the fwBuf bump allocator.
        constexpr uint64_t kFwBufAlign = 0x1000;
        uint64_t slot_off = psp.fwBufBumpOffset;
        uint64_t slot_sz  = (asd_size + kFwBufAlign - 1) & ~(kFwBufAlign - 1);
        if (slot_off + slot_sz > psp.fwBufSize) {
            PSP_LOG("parse_ta: fw_buf exhausted (want %llu @ %llu, cap %llu)",
                    slot_sz, slot_off, psp.fwBufSize);
            return kIOReturnNoSpace;
        }
        uint64_t slot_vram_off = psp.fwBufVRAMOffset + slot_off;
        uint64_t slot_mc_addr  = psp.fwBufBaseMC     + slot_off;

        // Stage ASD ucode into the slot. Use sysmem path if available
        // (matches LOAD_IP_FW path used for IP firmwares), otherwise
        // stream via BAR0 to VRAM.
        if (psp.fwBufSysmemCPU != nullptr) {
            memcpy(static_cast<uint8_t *>(psp.fwBufSysmemCPU) + slot_off,
                   asd_src, asd_size);
        } else {
            bar0_memcpy_to_vram(dev, slot_vram_off, asd_src, asd_size);
            amdgpu_hdp_flush(dev);
        }
        psp.fwBufBumpOffset = slot_off + slot_sz;

        psp.asd.parsed         = true;
        psp.asd.size_bytes     = asd_size;
        psp.asd.fw_version     = bin[i].fw_version;
        psp.asd.ucode_mc_addr  = slot_mc_addr;
        psp.asd.ucode_vram_off = slot_vram_off;
        psp.asd.session_id     = 0;
        psp.asd.resp_status    = 0;

        PSP_LOG("parse_ta: ASD staged (size=%u, mc=%#llx, vram_off=%#llx, "
                "fw_version=%#x)",
                asd_size, slot_mc_addr, slot_vram_off, bin[i].fw_version);
        found_asd = true;
        break;
    }

    if (!found_asd) {
        PSP_LOG("parse_ta: no ASD descriptor in TA bin (parsed %u entries) "
                "— psp_asd_initialize will be a no-op", count);
        // Not a fatal error — return success. psp_asd_initialize sees
        // !psp.asd.parsed and skips.
        return kIOReturnSuccess;
    }
    return kIOReturnSuccess;
}

//============================================================
// psp_asd_initialize — port of upstream psp_asd_initialize +
// psp_ta_load (amdgpu_psp.c:1225, 1380) for the ASD case.
//
// Builds a GFX_CMD_ID_LOAD_ASD (0x4) cmd_buf pointing at the
// VRAM-staged ASD ucode (set up by psp_parse_ta_microcode) and
// submits via the PSP ring. Critical for autoload state machine:
// upstream calls this between psp_rlc_autoload_start and
// psp_rl_load (amdgpu_psp.c:3153). Skipping it appears to leave
// PSP in a partial state where the GC autoload chain doesn't fire
// — this was the v0.1.18 symptom (BOOTLOAD_STATUS stuck at 0).
//
// PSP_ASD_SHARED_MEM_SIZE = 0 in upstream (amdgpu_psp.h:68), so
// the cmd_buf shared-mem fields stay zero.
//
// Returns kIOReturnSuccess if PSP acks with resp_status == 0, or
// if no TA bin was loaded (no-op). Failure cases are logged.
//============================================================

kern_return_t
psp_asd_initialize(DeviceContext &dev, PSPContext &psp)
{
    if (!psp.ringCreated) {
        PSP_LOG("asd_initialize: PSP ring not created");
        return kIOReturnNotReady;
    }
    if (!psp.asd.parsed || psp.asd.size_bytes == 0) {
        PSP_LOG("asd_initialize: no ASD staged (parsed=%d, size=%u) — "
                "skipping (host did not send TA bin)",
                psp.asd.parsed, psp.asd.size_bytes);
        return kIOReturnSuccess;
    }

    // Build the LOAD_ASD command in a 1 KB cmd_resp-sized buffer matching
    // upstream's `struct psp_gfx_cmd_resp` layout. The host-side
    // cmd_buf VRAM at kCmdBufVRAMOffset is the one PSP reads — we
    // stage there directly via BAR0.
    constexpr uint32_t kCmdRespSize = 1024;
    uint8_t cmd_buf[kCmdRespSize];
    memset(cmd_buf, 0, kCmdRespSize);

    // Header (matches PSPCmdRespHeader at offsets 0..15):
    //   +0  buf_size       (left 0 — informational only, see [[rb_frame_minimal]])
    //   +4  buf_version    (0)
    //   +8  cmd_id         = PSPGfxCmd::LOAD_ASD = 4
    //   +12 resp_buf_addr  (we use the same cmd_buf for response — 0)
    auto *hdr = reinterpret_cast<PSPGfxCmdRespHeader *>(cmd_buf);
    hdr->cmd_id = PSPGfxCmd::LOAD_ASD;

    // psp_gfx_cmd_load_ta lives at offset 28 (after the 7-dword header).
    // Layout (psp_gfx_if.h:129):
    //   uint32_t app_phy_addr_lo
    //   uint32_t app_phy_addr_hi
    //   uint32_t app_len
    //   uint32_t cmd_buf_phy_addr_lo  (0 — no shared mem for ASD)
    //   uint32_t cmd_buf_phy_addr_hi  (0)
    //   uint32_t cmd_buf_len          (0 = PSP_ASD_SHARED_MEM_SIZE)
    constexpr uint32_t kCmdLoadTaOffset = 28;
    uint32_t *cmd_words =
        reinterpret_cast<uint32_t *>(cmd_buf + kCmdLoadTaOffset);
    cmd_words[0] = static_cast<uint32_t>(psp.asd.ucode_mc_addr & 0xFFFFFFFFu);
    cmd_words[1] = static_cast<uint32_t>((psp.asd.ucode_mc_addr >> 32) & 0xFFFFFFFFu);
    cmd_words[2] = psp.asd.size_bytes;
    cmd_words[3] = 0;  // cmd_buf_phy_addr_lo
    cmd_words[4] = 0;  // cmd_buf_phy_addr_hi
    cmd_words[5] = 0;  // cmd_buf_len (PSP_ASD_SHARED_MEM_SIZE == 0)

    PSP_LOG("asd_initialize: submitting LOAD_ASD (mc=%#llx, size=%u)",
            psp.asd.ucode_mc_addr, psp.asd.size_bytes);

    uint32_t resp_status = 0;
    kern_return_t r = psp_ring_cmd_submit(dev, psp, cmd_buf, kCmdRespSize,
                                          &resp_status);
    psp.asd.resp_status = resp_status;
    if (r != kIOReturnSuccess) {
        PSP_LOG("asd_initialize: psp_ring_cmd_submit FAILED kr=%#x "
                "resp_status=%#x", r, resp_status);
        return r;
    }

    // The response status sits at offset 864 of the cmd_buf (we already
    // verified via psp_ring_cmd_submit). Session ID lives further down
    // at the start of psp_gfx_resp.session_id — per upstream
    // psp_gfx_if.h, the resp union starts at offset 864 and session_id
    // is the first dword: offset 864.
    //
    // But psp_ring_cmd_submit reads resp_status from offset 864 too.
    // session_id is at offset 868 (next dword after status, since
    // psp_gfx_resp = { uint32_t status; uint32_t session_id; ... }).
    constexpr uint32_t kRespSessionIdOffset = 864 + 4;
    psp.asd.session_id = RBAR2_32(dev,
        kCmdBufVRAMOffset + kRespSessionIdOffset);

    PSP_LOG("asd_initialize: ok (session_id=%#x, resp=%#x)",
            psp.asd.session_id, resp_status);
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

//============================================================
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
// Upstream order (AMDGPU_UCODE_ID_* enum, amdgpu_ucode.h:515-529):
//   1. SMU (GFX_FW_TYPE_SMU = 18)
//   2. IMU_I (68) + IMU_D (69)
//   3. RLC sub-bins in order:
//      a. GLOBAL_TAP_DELAYS (v2.4 only)
//      b. SE0-3_TAP_DELAYS (v2.4 only)
//      c. RLC_RESTORE_LIST_SRM_CNTL (v2.1+)
//      d. RLC_RESTORE_LIST_GPM_MEM (v2.1+)
//      e. RLC_RESTORE_LIST_SRM_MEM (v2.1+)
//      f. RLC_IRAM (v2.2+)
//      g. RLC_DRAM (v2.2+)
//      h. RLC_P (v2.3+)
//      i. RLC_V (v2.3+)
//      j. RLC_G (always — LAST, triggers autoload)
//   4. psp_rlc_autoload_start() ← CRITICAL
//   5. CP_RS64: PFP + stacks, ME + stacks, MEC + stacks
//   6. SDMA (GFX_FW_TYPE_SDMA_UCODE_TH0 = 71)
//   7. MES: CP_MES + CP_MES_DATA
//
// Returns kIOReturnSuccess on success, or the first error encountered.
//
kern_return_t
psp_load_non_psp_fw(DeviceContext &dev, PSPContext &psp,
                    const FirmwareLoader &loader)
{
    kern_return_t r;
    uint64_t fwBusAddr = 0;
    uint32_t fwSize = 0;

    PSP_LOG("load_non_psp_fw: starting firmware load sequence");

    // 1. Load SMU firmware
    r = loader.get_payload(PSPGfxFwType::SMU, fwBusAddr, fwSize);
    if (r != kIOReturnSuccess) {
        PSP_LOG("load_non_psp_fw: SMU load failed kr=%#x", r);
        return r;
    }
    r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, PSPGfxFwType::SMU);
    if (r != kIOReturnSuccess) {
        PSP_LOG("load_non_psp_fw: SMU LOAD_IP_FW failed kr=%#x", r);
        return r;
    }
    PSP_LOG("load_non_psp_fw: SMU loaded (size=%u)", fwSize);

    // 2. Load IMU firmware (I + D)
    r = loader.get_payload(PSPGfxFwType::IMU_I, fwBusAddr, fwSize);
    if (r == kIOReturnSuccess) {
        r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, PSPGfxFwType::IMU_I);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: IMU_I LOAD_IP_FW failed kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: IMU_I loaded (size=%u)", fwSize);
    }
    r = loader.get_payload(PSPGfxFwType::IMU_D, fwBusAddr, fwSize);
    if (r == kIOReturnSuccess) {
        r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, PSPGfxFwType::IMU_D);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: IMU_D LOAD_IP_FW failed kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: IMU_D loaded (size=%u)", fwSize);
    }

    // 3. Load RLC sub-bins in correct upstream enum order.
    //    Each sub-bin is loaded via LOAD_IP_FW. RLC_G must be LAST
    //    because upstream calls psp_rlc_autoload_start() immediately
    //    after RLC_G loads (amdgpu_psp.c:3113-3121).
    {
        struct RLCSubBin {
            uint32_t fw_type;
            const char *name;
        } bins[] = {
            { PSPGfxFwType::GLOBAL_TAP_DELAYS,    "GLOBAL_TAP_DELAYS" },
            { PSPGfxFwType::SE0_TAP_DELAYS,       "SE0_TAP_DELAYS" },
            { PSPGfxFwType::SE1_TAP_DELAYS,       "SE1_TAP_DELAYS" },
            { PSPGfxFwType::SE2_TAP_DELAYS,       "SE2_TAP_DELAYS" },
            { PSPGfxFwType::SE3_TAP_DELAYS,       "SE3_TAP_DELAYS" },
            { PSPGfxFwType::RLC_RESTORE_LIST_SRM_CNTL, "RLC_RESTORE_LIST_SRM_CNTL" },
            { PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM,  "RLC_RESTORE_LIST_GPM_MEM" },
            { PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM,  "RLC_RESTORE_LIST_SRM_MEM" },
            { PSPGfxFwType::RLC_IRAM,             "RLC_IRAM" },
            { PSPGfxFwType::RLC_DRAM_BOOT,        "RLC_DRAM_BOOT" },
            { PSPGfxFwType::RLC_P,                "RLC_P" },
            { PSPGfxFwType::RLC_V,                "RLC_V" },
            { PSPGfxFwType::RLC_G,                "RLC_G" },
        };

        for (auto &bin : bins) {
            r = loader.get_payload(bin.fw_type, fwBusAddr, fwSize);
            if (r == kIOReturnUnsupported) {
                // This sub-bin is not present in this firmware version.
                PSP_LOG("load_non_psp_fw: %s not present (v2.x variant)",
                        bin.name);
                continue;
            }
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s get_payload failed kr=%#x",
                        bin.name, r);
                return r;
            }
            r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, bin.fw_type);
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s LOAD_IP_FW failed kr=%#x",
                        bin.name, r);
                return r;
            }
            PSP_LOG("load_non_psp_fw: %s loaded (size=%u)", bin.name, fwSize);

            // CRITICAL: After RLC_G, trigger autoload.
            // Upstream: amdgpu_psp.c:3113-3121
            if (bin.fw_type == PSPGfxFwType::RLC_G) {
                r = psp_rlc_autoload_start(dev, psp);
                if (r != kIOReturnSuccess) {
                    PSP_LOG("load_non_psp_fw: rlc_autoload_start FAILED kr=%#x",
                            r);
                    return r;
                }
                PSP_LOG("load_non_psp_fw: RLC autoload started");
            }
        }
    }

    // 4. Load CP RS64 firmware (PFP, ME, MEC + per-pipe stacks).
    //    Each CP file emits: ucode + 2-4 stack payloads.
    {
        struct CPSubBin {
            uint32_t fw_type;
            const char *name;
        } bins[] = {
            { PSPGfxFwType::RS64_PFP,       "RS64_PFP" },
            { PSPGfxFwType::RS64_PFP_P0,    "RS64_PFP_P0" },
            { PSPGfxFwType::RS64_PFP_P1,    "RS64_PFP_P1" },
            { PSPGfxFwType::RS64_ME,        "RS64_ME" },
            { PSPGfxFwType::RS64_ME_P0,     "RS64_ME_P0" },
            { PSPGfxFwType::RS64_ME_P1,     "RS64_ME_P1" },
            { PSPGfxFwType::RS64_MEC,       "RS64_MEC" },
            { PSPGfxFwType::RS64_MEC_P0,    "RS64_MEC_P0" },
            { PSPGfxFwType::RS64_MEC_P1,    "RS64_MEC_P1" },
            { PSPGfxFwType::RS64_MEC_P2,    "RS64_MEC_P2" },
            { PSPGfxFwType::RS64_MEC_P3,    "RS64_MEC_P3" },
        };

        for (auto &bin : bins) {
            r = loader.get_payload(bin.fw_type, fwBusAddr, fwSize);
            if (r == kIOReturnUnsupported) {
                PSP_LOG("load_non_psp_fw: %s not present",
                        bin.name);
                continue;
            }
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s get_payload failed kr=%#x",
                        bin.name, r);
                return r;
            }
            r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, bin.fw_type);
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s LOAD_IP_FW failed kr=%#x",
                        bin.name, r);
                return r;
            }
            PSP_LOG("load_non_psp_fw: %s loaded (size=%u)", bin.name, fwSize);
        }
    }

    // 5. Load SDMA firmware (single RS64 TH0 payload for RDNA4).
    r = loader.get_payload(PSPGfxFwType::SDMA_UCODE_TH0, fwBusAddr, fwSize);
    if (r == kIOReturnUnsupported) {
        PSP_LOG("load_non_psp_fw: SDMA not present");
    } else if (r != kIOReturnSuccess) {
        PSP_LOG("load_non_psp_fw: SDMA get_payload failed kr=%#x", r);
        return r;
    } else {
        r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize,
                           PSPGfxFwType::SDMA_UCODE_TH0);
        if (r != kIOReturnSuccess) {
            PSP_LOG("load_non_psp_fw: SDMA LOAD_IP_FW failed kr=%#x", r);
            return r;
        }
        PSP_LOG("load_non_psp_fw: SDMA loaded (size=%u)", fwSize);
    }

    // 6. Load MES firmware (CP_MES + CP_MES_DATA for uni_mes packaging).
    {
        struct MESBin {
            uint32_t fw_type;
            const char *name;
        } bins[] = {
            { PSPGfxFwType::CP_MES,       "CP_MES" },
            { PSPGfxFwType::CP_MES_DATA,  "CP_MES_DATA" },
        };

        for (auto &bin : bins) {
            r = loader.get_payload(bin.fw_type, fwBusAddr, fwSize);
            if (r == kIOReturnUnsupported) {
                PSP_LOG("load_non_psp_fw: %s not present",
                        bin.name);
                continue;
            }
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s get_payload failed kr=%#x",
                        bin.name, r);
                return r;
            }
            r = psp_load_ip_fw(dev, psp, fwBusAddr, fwSize, bin.fw_type);
            if (r != kIOReturnSuccess) {
                PSP_LOG("load_non_psp_fw: %s LOAD_IP_FW failed kr=%#x",
                        bin.name, r);
                return r;
            }
            PSP_LOG("load_non_psp_fw: %s loaded (size=%u)", bin.name, fwSize);
        }
    }

    PSP_LOG("load_non_psp_fw: all firmware loaded successfully");
    return kIOReturnSuccess;
}

} // namespace amdgpu
