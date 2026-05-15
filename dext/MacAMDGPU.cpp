//
//  MacAMDGPU.cpp — Driver + UserClient implementation.
//
//  Phase 1A: load, claim, log config space + BARs, expose BARs to
//  userspace via CopyClientMemoryForType, answer Ping / GetIdentity /
//  GetBARInfo over ExternalMethod. No DMA, no MSI-X.
//
//  Pattern reference: qemu-vfio-apple/contrib/apple-vfio/
//      VFIOUserPCIDriver/VFIOUserPCIDriver.cpp (scottjg, 2026-03-18).
//  Narrowed: R9700 VID/DID only, single-client, no VFIO selectors.
//

#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <DriverKit/OSMetaClass.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include "MacAMDGPU.h"
#include "MacAMDGPUUserClient.h"

#include "amdgpu/amdgpu_init.h"
#include "amdgpu/amdgpu_ucode_psp.h"
#include "amdgpu/amdgpu_discovery.h"
#include "amdgpu/amdgpu_ih.h"
#include "amdgpu/amdgpu_cp.h"
#include "amdgpu/amdgpu_sdma.h"
#include "amdgpu/amdgpu_mes.h"
#include "amdgpu/amdgpu_ucode_extract.h"

#define MACAMDGPU_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu: " fmt, ##__VA_ARGS__)

//============================================================
// Selectors and memory types — keep in sync with userspace.
//============================================================
enum {
    kMacAMDGPUMethodPing              = 0,
    kMacAMDGPUMethodGetIdentity       = 1,
    kMacAMDGPUMethodGetBARInfo        = 2,
    kMacAMDGPUMethodSetupInterrupts   = 3,
    kMacAMDGPUMethodWaitInterrupt     = 4,  // async
    kMacAMDGPUMethodSetIRQMask        = 5,
    kMacAMDGPUMethodAllocateDMABuffer = 6,
    kMacAMDGPUMethodFreeDMABuffer     = 7,
    kMacAMDGPUMethodResetDevice       = 8,
    kMacAMDGPUMethodInitDevice        = 9,
    kMacAMDGPUMethodLoadFirmware      = 10,
    kMacAMDGPUMethodSetIPBase         = 11,
    kMacAMDGPUMethodGetIPBase         = 12,
    kMacAMDGPUMethodLoadDiscoveryBin  = 13,
    kMacAMDGPUMethodSubmitTestPM4     = 14,
    kMacAMDGPUMethodSDMACopyTest      = 15,
    kMacAMDGPUMethodBOAlloc           = 16,
    kMacAMDGPUMethodBOFree            = 17,
    kMacAMDGPUMethodBOGetInfo         = 18,
    kMacAMDGPUMethodSubmitIB          = 19,
    kMacAMDGPUMethodWaitFence         = 20,
    kMacAMDGPUMethodQueryInfo         = 21,
    kMacAMDGPUMethodMESAddQueue       = 22,
    kMacAMDGPUMethodGetDiagnostics    = 23,
    kMacAMDGPUMethodDumpTMR           = 24,
    kMacAMDGPUMethodDumpPSP           = 25,
    kMacAMDGPUMethodDumpCmdBuf        = 26,
};

// QueryInfo "info type" tags — input scalarInput[0]. Output shape
// is type-specific; we document each below.
enum {
    kMacAMDGPUInfoGFXVersion     = 1, // out[0]=major, [1]=minor, [2]=rev
    kMacAMDGPUInfoVRAMSizes      = 2, // out[0]=visible, [1]=total (bytes)
    kMacAMDGPUInfoIPVersions     = 3, // out[0]=GMC pack, [1]=SDMA pack, [2]=PSP pack, [3]=SMU pack
    kMacAMDGPUInfoBringupReached = 4, // out[0]=BringupStage (highest reached)
};

// Firmware type tags used by LoadFirmware. Pre-SOS components route
// through psp_bootloader_load_component; SOS is special (psp_load_sos).
enum {
    // Pre-SOS bootloader components (load via psp_bootloader_load_component).
    kMacAMDGPUFwTypeSOS         = 0,
    kMacAMDGPUFwTypeKDB         = 1,
    kMacAMDGPUFwTypeSPL         = 2,
    kMacAMDGPUFwTypeSysDrv      = 3,
    kMacAMDGPUFwTypeSocDrv      = 4,
    kMacAMDGPUFwTypeIntfDrv     = 5,
    kMacAMDGPUFwTypeDbgDrv      = 6,   // a.k.a. HAD on v14
    kMacAMDGPUFwTypeRASDrv      = 7,
    kMacAMDGPUFwTypeIPKeyMgrDrv = 8,
    kMacAMDGPUFwTypeTA          = 9,
    // Post-SOS IP firmware (load via psp_load_ip_fw through the ring).
    // Encoded as 0x100 + psp_gfx_fw_type so userspace doesn't collide
    // with the pre-SOS namespace.
    kMacAMDGPUFwTypeIP_SMU         = 0x100 + 18,
    kMacAMDGPUFwTypeIP_PPTABLE     = 0x100 + 73,
    kMacAMDGPUFwTypeIP_SDMA0       = 0x100 + 9,
    kMacAMDGPUFwTypeIP_SDMA1       = 0x100 + 10,
    kMacAMDGPUFwTypeIP_RLC_G       = 0x100 + 8,
    kMacAMDGPUFwTypeIP_CP_ME       = 0x100 + 1,
    kMacAMDGPUFwTypeIP_CP_PFP      = 0x100 + 2,
    kMacAMDGPUFwTypeIP_CP_MEC      = 0x100 + 4,
    kMacAMDGPUFwTypeIP_IMU_I       = 0x100 + 68,
    kMacAMDGPUFwTypeIP_IMU_D       = 0x100 + 69,
    kMacAMDGPUFwTypeIP_RS64_MES         = 0x100 + 76,
    kMacAMDGPUFwTypeIP_RS64_MES_STACK   = 0x100 + 77,
    kMacAMDGPUFwTypeIP_RS64_KIQ         = 0x100 + 78,
    kMacAMDGPUFwTypeIP_RS64_KIQ_STACK   = 0x100 + 79,
    // 0x200+ — multi-payload .bin files. Each value names a SOURCE
    // FILE; the dext expands it into N LOAD_IP_FW frames via
    // amdgpu_ucode_extract. Keep in sync with constants in
    // amdgpu_ucode_extract.cpp.
    kMacAMDGPUFwTypeFile_SDMA       = 0x200 + 0,
    kMacAMDGPUFwTypeFile_RLC        = 0x200 + 1,
    kMacAMDGPUFwTypeFile_IMU        = 0x200 + 2,
    kMacAMDGPUFwTypeFile_MES_UNI    = 0x200 + 3,
    kMacAMDGPUFwTypeFile_CP_PFP     = 0x200 + 4,
    kMacAMDGPUFwTypeFile_CP_ME      = 0x200 + 5,
    kMacAMDGPUFwTypeFile_CP_MEC     = 0x200 + 6,
};

enum {
    kMacAMDGPUMemoryTypeBAR0      = 0,
    kMacAMDGPUMemoryTypeBAR1      = 1,
    kMacAMDGPUMemoryTypeBAR2      = 2,
    kMacAMDGPUMemoryTypeBAR3      = 3,
    kMacAMDGPUMemoryTypeBAR4      = 4,
    kMacAMDGPUMemoryTypeBAR5      = 5,
    kMacAMDGPUMemoryTypeDMABuffer = 6,
    kMacAMDGPUMemoryTypeIRQState  = 7,
};

#define MACAMDGPU_MAX_IRQ_VECTORS    256
#define MACAMDGPU_IRQ_PENDING_WORDS  4   // 4 × 64 = 256 vectors
#define MACAMDGPU_MAX_DMA_SEGMENTS   32
#define MACAMDGPU_DMA_BUFFER_MAX     (1536ULL * 1024ULL * 1024ULL)
#define MACAMDGPU_MAX_BO             64
#define MACAMDGPU_BO_ALIGN           amdgpu::kASPageSize  // 16 KB

// Per-client BO sub-range allocator.
// Each BO is a [byte_offset, byte_offset+size) window inside the
// client's single DMABuffer. Userspace owns the mmap of the
// DMABuffer (Path A) so it can fill / read BOs directly via memcpy;
// the dext only validates the range and translates byte_offset →
// GPU bus address when stitching submissions. Allocation is bump-
// only for now; BOFree marks the entry free but doesn't reclaim
// space (free-list is a Phase 2 follow-up).
struct BOEntry {
    bool      in_use;
    uint64_t  byte_offset;
    uint64_t  size;
    uint32_t  generation;
};

// Bits 0..127 of irqPending track raw MSI-X vector firings (one bit
// per vector, capped at 128 — anything beyond would land in word 2+,
// reserved for IH-routed events below).
//
// Bits 128..255 are IH-routed events. When the dext drains the IH
// ring, each decoded entry's (client_id, src_id) maps to one of these
// bits. Userspace can WaitInterrupt() and then read irqPending to
// learn what kind of event arrived.
//
// Stable across the userspace ABI — don't renumber.
enum {
    kIRQBitGFXEOPFence   = 128,   // CP_EOP — fence value lands in entry's src_data
    kIRQBitGFXRASError   = 129,   // CP_ECC_ERROR
    kIRQBitVMFault       = 130,   // ATHUB UTCL2_FAULT
    kIRQBitSDMA0Trap     = 131,
    kIRQBitSDMA1Trap     = 132,
    kIRQBitIHOverflow    = 133,   // IH ring overflowed since last drain
    kIRQBitIHOther       = 134,   // catch-all for unrecognised entries
};

//============================================================
// Driver instance state.
//============================================================
struct MacAMDGPU_IVars {
    bool       pciOpen;
    IOService *openerUserClient;  // tracked so Open/Close entities match

    // Phase 1B: per-device bringup state shared across user clients.
    // Populated lazily when PCI is opened. Stages run on demand via
    // InitDevice selector.
    amdgpu::BringupContext bringup;
};

//
// UserClient state. DMA, MSI-X, IRQ shared page all live here so
// each userspace process gets isolated resources.
//
struct MacAMDGPUUserClient_IVars {
    bool claimed;

    // DMA — single contiguous buffer per client (Path A from apple-vfio).
    IOBufferMemoryDescriptor *dmaBuffer;
    IODMACommand             *dmaCommand;
    uint64_t                  dmaBufferSize;
    uint64_t                  dmaFlags;
    uint32_t                  dmaSegmentsCount;
    IOAddressSegment          dmaSegments[MACAMDGPU_MAX_DMA_SEGMENTS];

    // MSI-X
    bool                          interruptsSetUp;
    uint32_t                      numInterrupts;
    IOInterruptDispatchSource    *interruptSources[MACAMDGPU_MAX_IRQ_VECTORS];
    IODispatchQueue              *irqQueue;

    // IRQ shared-memory page (16 KB; first 64 B used). Layout:
    //   [0x00..0x1F]  irqPending[4]  — dext sets bits, client clears
    //   [0x20..0x3F]  irqEnabled[4]  — client writes, dext reads
    IOBufferMemoryDescriptor *irqSharedBuffer;
    volatile uint64_t         *irqPending;
    volatile uint64_t         *irqEnabled;
    OSAction                  *pendingInterruptNotify;  // outstanding WaitInterrupt

    // BO sub-range allocator (bump-only) — see BOEntry above.
    BOEntry   bos[MACAMDGPU_MAX_BO];
    uint64_t  boBumpOffset;
    uint32_t  boGenCounter;

    // GART binding for the DMA buffer. Lazily populated on the first
    // LOAD_IP_FW submit; reused across subsequent submits (the host
    // streams different firmware bytes into the same DART-mapped
    // buffer, so the busAddr is stable). Reset on EnsureDMABuffer
    // when the buffer is reallocated.
    amdgpu::GARTBinding dmaGartBinding;
};

//
// BO helpers — pack/unpack handle, lookup entry from handle.
//
static inline uint64_t
mac_amdgpu_bo_make_handle(uint32_t generation, uint32_t index)
{
    return (static_cast<uint64_t>(generation) << 32) |
           (static_cast<uint64_t>(index) & 0xFFFFFFFFull);
}

static BOEntry *
mac_amdgpu_bo_lookup(MacAMDGPUUserClient_IVars *ivars, uint64_t handle)
{
    if (ivars == nullptr) return nullptr;
    uint32_t idx = static_cast<uint32_t>(handle & 0xFFFFFFFFull);
    uint32_t gen = static_cast<uint32_t>(handle >> 32);
    if (idx >= MACAMDGPU_MAX_BO) return nullptr;
    BOEntry *e = &ivars->bos[idx];
    if (!e->in_use || e->generation != gen) return nullptr;
    return e;
}

//============================================================
// Helpers.
//============================================================
static IOPCIDevice *
mac_amdgpu_pci(IOService *service)
{
    if (service == nullptr) {
        return nullptr;
    }
    return OSDynamicCast(IOPCIDevice, service->GetProvider());
}

static const char *
mac_amdgpu_bar_type_string(uint8_t barType)
{
    switch (barType) {
    case kPCIBARTypeM32:   return "mem32";
    case kPCIBARTypeM64:   return "mem64";
    case kPCIBARTypeM32PF: return "mem32-prefetch";
    case kPCIBARTypeM64PF: return "mem64-prefetch";
    case kPCIBARTypeIO:    return "io";
    default:               return "unknown";
    }
}

//============================================================
// Resource management — DMA, MSI-X, IRQ shared page, reset.
//
// All static so they don't pollute the IIG-generated dispatch.
//============================================================

// Open the PCI device (idempotent) and populate the bringup
// DeviceContext from BAR info + enable Memory Space + Bus Master.
// Called from every selector that touches MMIO/config so the order
// of selector calls doesn't matter.
static kern_return_t
mac_amdgpu_ensure_open(IOService *opener, MacAMDGPU *driver,
                       IOPCIDevice *pci)
{
    if (driver == nullptr || driver->ivars == nullptr || pci == nullptr) {
        return kIOReturnNotReady;
    }
    if (driver->ivars->pciOpen) return kIOReturnSuccess;

    kern_return_t ret = pci->Open(opener, 0);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("ensure_open: PCI Open failed: %#x", ret);
        return ret;
    }
    driver->ivars->pciOpen = true;
    driver->ivars->openerUserClient = opener;

    uint16_t cmd = 0;
    pci->ConfigurationRead16(0x04, &cmd);
    uint16_t wanted = cmd | 0x0006;  // bit 1 = MEM, bit 2 = BUSMASTER
    if (wanted != cmd) {
        pci->ConfigurationWrite16(0x04, wanted);
        pci->ConfigurationRead16(0x04, &cmd);
    }

    auto &bdev = driver->ivars->bringup.device;
    bdev.pci = pci;
    bdev.psoCAlive = false;
    bdev.smuOnline = false;
    bdev.gmcReady  = false;
    // Wire the GART pointer so PSP code can allocate GART-bound buffers
    // without taking gart& as a parameter through every call.
    driver->ivars->bringup.psp.gart = &driver->ivars->bringup.gart;
    for (uint8_t bar = 0; bar < 6; bar++) {
        uint8_t  mi = 0;
        uint64_t sz = 0;
        uint8_t  ty = 0;
        if (pci->GetBARInfo(bar, &mi, &sz, &ty) != kIOReturnSuccess) continue;
        switch (bar) {
        case 0:
            // BAR0 is the visible-VRAM/framebuffer aperture on Bonaire+.
            bdev.bar0MemIndex         = mi;
            bdev.bar0Size             = sz;
            bdev.bar2VisibleVRAMSize  = sz;  // visible VRAM window size
            break;
        case 2: bdev.bar2MemIndex = mi; break;       // doorbell BAR
        case 5: bdev.bar5MemIndex = mi; break;       // MMIO register window
        default: break;
        }
    }
    MACAMDGPU_LOG("ensure_open: PCI opened, cmd=%#x, "
                  "BAR0(visible VRAM)=%llu B, BAR2(doorbell)=2MB, "
                  "BAR5(registers)=512KB",
                  (unsigned)cmd, bdev.bar0Size);

    // Read PCI config space + try to wake device to D0.
    uint32_t bar0_cfg = 0, bar2_cfg = 0, bar5_cfg = 0;
    uint16_t status = 0;
    pci->ConfigurationRead32(0x10, &bar0_cfg);
    pci->ConfigurationRead32(0x18, &bar2_cfg);
    pci->ConfigurationRead32(0x24, &bar5_cfg);
    pci->ConfigurationRead16(0x06, &status);
    MACAMDGPU_LOG("ensure_open: config — BAR0=%#010x BAR2=%#010x "
                  "BAR5=%#010x status=%#06x cmd=%#06x",
                  bar0_cfg, bar2_cfg, bar5_cfg, (unsigned)status,
                  (unsigned)cmd);

    // Look for the PM capability and force D0. The capability list
    // pointer is at config offset 0x34.
    uint8_t cap_ptr = 0;
    pci->ConfigurationRead8(0x34, &cap_ptr);
    while (cap_ptr != 0 && cap_ptr != 0xFF) {
        uint16_t cap_hdr = 0;
        pci->ConfigurationRead16(cap_ptr, &cap_hdr);
        uint8_t cap_id = cap_hdr & 0xFF;
        if (cap_id == 0x01) {  // PCI Power Management Capability
            uint16_t pmcsr = 0;
            pci->ConfigurationRead16(cap_ptr + 4, &pmcsr);
            uint8_t state = pmcsr & 0x3;
            MACAMDGPU_LOG("ensure_open: PM cap at %#x, PMCSR=%#x "
                          "(power state D%u)", cap_ptr, (unsigned)pmcsr,
                          (unsigned)state);
            if (state != 0) {
                pmcsr = (pmcsr & ~0x3u);  // state bits → 0 (D0)
                pci->ConfigurationWrite16(cap_ptr + 4, pmcsr);
                IOSleep(10);
                pci->ConfigurationRead16(cap_ptr + 4, &pmcsr);
                MACAMDGPU_LOG("ensure_open: forced D0, PMCSR now %#x",
                              (unsigned)pmcsr);
            }
            break;
        }
        cap_ptr = (cap_hdr >> 8) & 0xFF;
    }

    // MMIO sanity probe — read several BAR0 dwords. On a healthy
    // R9700 in D0, RCC_CONFIG_MEMSIZE (dword 0xDE3) returns
    // vram_size_mb (~32768). All-zero reads = device isn't decoding
    // MMIO yet (D-state, BAR not programmed, or PCIe link issue).
    auto rd = [&](uint32_t dw) -> uint32_t {
        uint32_t v = 0xFFFFFFFFu;
        pci->MemoryRead32(bdev.bar0MemIndex,
                          static_cast<uint64_t>(dw) * 4ULL, &v);
        return v;
    };
    MACAMDGPU_LOG("ensure_open: mmio probe — "
                  "BAR0[0x0000]=%#x BAR0[0x0001]=%#x BAR0[0xDE3]=%#x "
                  "BAR0[0x16A00]=%#x BAR0[0x16061]=%#x",
                  rd(0x0000), rd(0x0001), rd(0x0DE3), rd(0x16A00),
                  rd(0x16061));
    return kIOReturnSuccess;
}

static void
mac_amdgpu_release_dma_buffer(MacAMDGPUUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) {
        return;
    }
    if (client->ivars->dmaCommand != nullptr) {
        client->ivars->dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    }
    OSSafeReleaseNULL(client->ivars->dmaCommand);
    OSSafeReleaseNULL(client->ivars->dmaBuffer);
    client->ivars->dmaBufferSize    = 0;
    client->ivars->dmaFlags         = 0;
    client->ivars->dmaSegmentsCount = 0;
    memset(client->ivars->dmaSegments, 0,
           sizeof(client->ivars->dmaSegments));
    // The bus address goes away with the buffer; the GART slot stays
    // mapped to a stale address until something rewrites it. Clear the
    // binding's MC addr so the next LOAD_IP_FW path rebinds afresh.
    memset(&client->ivars->dmaGartBinding, 0,
           sizeof(client->ivars->dmaGartBinding));
}

static kern_return_t
mac_amdgpu_allocate_dma_buffer(MacAMDGPUUserClient *client,
                               uint64_t requestedSize,
                               uint64_t requestedAlignment)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }
    if (requestedSize == 0 || requestedSize > MACAMDGPU_DMA_BUFFER_MAX) {
        return kIOReturnBadArgument;
    }

    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, client->GetProvider());
    if (driver == nullptr) {
        return kIOReturnNotAttached;
    }
    IOPCIDevice *pci = mac_amdgpu_pci(driver);
    if (pci == nullptr) {
        return kIOReturnUnsupported;
    }

    mac_amdgpu_release_dma_buffer(client);

    // Apple Silicon page size is 16 KB. DART rejects mappings that
    // aren't page-aligned; coerce upward if the caller asked for less.
    uint64_t alignment = requestedAlignment < amdgpu::kASPageSize
                           ? amdgpu::kASPageSize : requestedAlignment;

    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, requestedSize, alignment, &buf);
    if (ret != kIOReturnSuccess || buf == nullptr) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ret = buf->SetLength(requestedSize);
    if (ret != kIOReturnSuccess) {
        buf->release();
        return ret;
    }

    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;

    IODMACommand *cmd = nullptr;
    ret = IODMACommand::Create(pci, kIODMACommandCreateNoOptions,
                               &spec, &cmd);
    if (ret != kIOReturnSuccess || cmd == nullptr) {
        buf->release();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    uint64_t dmaFlags = 0;
    uint32_t segCount = MACAMDGPU_MAX_DMA_SEGMENTS;
    ret = cmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                             buf, 0, requestedSize,
                             &dmaFlags, &segCount,
                             client->ivars->dmaSegments);
    if (ret != kIOReturnSuccess) {
        cmd->release();
        buf->release();
        return ret;
    }

    client->ivars->dmaBuffer        = buf;
    client->ivars->dmaCommand       = cmd;
    client->ivars->dmaBufferSize    = requestedSize;
    client->ivars->dmaFlags         = dmaFlags;
    client->ivars->dmaSegmentsCount = segCount;

    MACAMDGPU_LOG("dma alloc size=%llu align=%llu segs=%u first=%#llx/%#llx",
                  requestedSize, alignment, (unsigned)segCount,
                  segCount > 0 ? client->ivars->dmaSegments[0].address : 0,
                  segCount > 0 ? client->ivars->dmaSegments[0].length  : 0);
    return kIOReturnSuccess;
}

static void
mac_amdgpu_release_all_interrupts(MacAMDGPUUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) return;
    if (!client->ivars->interruptsSetUp) return;

    for (uint32_t i = 0; i < client->ivars->numInterrupts; i++) {
        IOInterruptDispatchSource *src = client->ivars->interruptSources[i];
        if (src != nullptr) {
            // Cancel is async — release inside the completion block so the
            // source outlives any in-flight handler.
            src->Cancel(^{ src->release(); });
            client->ivars->interruptSources[i] = nullptr;
        }
    }
    if (client->ivars->irqQueue != nullptr) {
        client->ivars->irqQueue->release();
        client->ivars->irqQueue = nullptr;
    }
    client->ivars->interruptsSetUp = false;
    client->ivars->numInterrupts   = 0;

    if (client->ivars->irqPending != nullptr) {
        for (int i = 0; i < MACAMDGPU_IRQ_PENDING_WORDS; i++) {
            __atomic_store_n(&client->ivars->irqPending[i], 0,
                             __ATOMIC_RELEASE);
        }
    }

    OSAction *pending = __atomic_exchange_n(
        &client->ivars->pendingInterruptNotify, nullptr, __ATOMIC_ACQ_REL);
    if (pending != nullptr) pending->release();

    client->ivars->irqPending = nullptr;
    client->ivars->irqEnabled = nullptr;
    OSSafeReleaseNULL(client->ivars->irqSharedBuffer);
}

static uint32_t
mac_amdgpu_query_vector_count(IOPCIDevice *pci, bool *outUsingMSIX)
{
    if (outUsingMSIX != nullptr) *outUsingMSIX = false;
    if (pci == nullptr) return 1;

    uint64_t capOffset = 0;
    uint16_t msgCtrl   = 0;

    if (pci->FindPCICapability(kIOPCICapabilityIDMSIX, 0,
                               &capOffset) == kIOReturnSuccess) {
        pci->ConfigurationRead16((uint32_t)(capOffset + 2), &msgCtrl);
        if (outUsingMSIX != nullptr) *outUsingMSIX = true;
        return (msgCtrl & 0x07FFu) + 1u;
    }
    if (pci->FindPCICapability(kIOPCICapabilityIDMSI, 0,
                               &capOffset) == kIOReturnSuccess) {
        uint8_t mmc;
        pci->ConfigurationRead16((uint32_t)(capOffset + 2), &msgCtrl);
        mmc = (msgCtrl >> 1) & 0x7;
        return 1u << mmc;
    }
    return 1;
}

static kern_return_t
mac_amdgpu_setup_interrupts(MacAMDGPUUserClient *client)
{
    if (client == nullptr || client->ivars == nullptr) {
        return kIOReturnBadArgument;
    }
    if (client->ivars->interruptsSetUp) {
        return kIOReturnStillOpen;
    }

    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, client->GetProvider());
    if (driver == nullptr) return kIOReturnNotAttached;
    IOPCIDevice *pci = mac_amdgpu_pci(driver);
    if (pci == nullptr) return kIOReturnUnsupported;

    IODispatchQueue *queue = nullptr;
    kern_return_t ret = IODispatchQueue::Create("MacAMDGPUIRQ", 0, 0, &queue);
    if (ret != kIOReturnSuccess || queue == nullptr) {
        MACAMDGPU_LOG("IODispatchQueue::Create failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    client->ivars->irqQueue = queue;

    // 16 KB shared page (only first 64 B used; round to 16 K for alignment).
    IOBufferMemoryDescriptor *irqBuf = nullptr;
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
                                           16384, 16384, &irqBuf);
    if (ret != kIOReturnSuccess || irqBuf == nullptr) {
        queue->release();
        client->ivars->irqQueue = nullptr;
        MACAMDGPU_LOG("IRQ shared buffer alloc failed: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    IOAddressSegment seg = {};
    ret = irqBuf->GetAddressRange(&seg);
    if (ret != kIOReturnSuccess || seg.address == 0) {
        irqBuf->release();
        queue->release();
        client->ivars->irqQueue = nullptr;
        return kIOReturnNoMemory;
    }
    auto *shared = reinterpret_cast<volatile uint64_t *>(seg.address);
    client->ivars->irqSharedBuffer = irqBuf;
    client->ivars->irqPending      = shared;
    client->ivars->irqEnabled      = shared + MACAMDGPU_IRQ_PENDING_WORDS;
    for (int i = 0; i < MACAMDGPU_IRQ_PENDING_WORDS; i++) {
        __atomic_store_n(&client->ivars->irqPending[i], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&client->ivars->irqEnabled[i], ~0ULL,
                         __ATOMIC_RELEASE);
    }
    client->ivars->pendingInterruptNotify = nullptr;

    bool usingMSIX = false;
    uint32_t requested = mac_amdgpu_query_vector_count(pci, &usingMSIX);
    if (requested == 0 || requested > MACAMDGPU_MAX_IRQ_VECTORS) {
        requested = MACAMDGPU_MAX_IRQ_VECTORS;
    }

    ret = pci->ConfigureInterrupts(usingMSIX ?
                                   kIOInterruptTypePCIMessagedX :
                                   kIOInterruptTypePCIMessaged,
                                   1, requested, 0);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("ConfigureInterrupts(%s, n=%u) failed: %#x",
                      usingMSIX ? "MSI-X" : "MSI",
                      (unsigned)requested, ret);
        if (usingMSIX) {
            usingMSIX = false;
            requested = 1;
            ret = pci->ConfigureInterrupts(kIOInterruptTypePCIMessaged,
                                           1, requested, 0);
            if (ret != kIOReturnSuccess) {
                MACAMDGPU_LOG("MSI fallback also failed: %#x", ret);
            }
        }
    } else {
        MACAMDGPU_LOG("ConfigureInterrupts(%s) n=%u",
                      usingMSIX ? "MSI-X" : "MSI", (unsigned)requested);
    }

    uint32_t registered = 0;
    for (uint32_t i = 0; i < requested; i++) {
        IOInterruptDispatchSource *src = nullptr;
        ret = IOInterruptDispatchSource::Create(pci, i, queue, &src);
        if (ret != kIOReturnSuccess || src == nullptr) {
            MACAMDGPU_LOG("Create source v=%u/%u: %#x",
                          (unsigned)i, (unsigned)requested, ret);
            break;
        }
        OSAction *action = nullptr;
        ret = client->CreateActionInterruptOccurred(sizeof(uint32_t),
                                                    &action);
        if (ret != kIOReturnSuccess || action == nullptr) {
            src->release();
            MACAMDGPU_LOG("CreateAction v=%u: %#x", (unsigned)i, ret);
            break;
        }
        uint32_t *vref = (uint32_t *)action->GetReference();
        if (vref != nullptr) *vref = i;

        ret = src->SetHandler(action);
        if (ret != kIOReturnSuccess) {
            action->release();
            src->release();
            MACAMDGPU_LOG("SetHandler v=%u: %#x", (unsigned)i, ret);
            break;
        }
        ret = src->SetEnable(true);
        if (ret != kIOReturnSuccess) {
            action->release();
            src->release();
            MACAMDGPU_LOG("SetEnable v=%u: %#x", (unsigned)i, ret);
            break;
        }
        client->ivars->interruptSources[i] = src;
        registered++;
    }

    client->ivars->numInterrupts   = registered;
    client->ivars->interruptsSetUp = (registered > 0);
    MACAMDGPU_LOG("registered %u/%u %s vectors",
                  (unsigned)registered, (unsigned)requested,
                  usingMSIX ? "MSI-X" : "MSI");
    return registered > 0 ? kIOReturnSuccess : kIOReturnNotFound;
}

static kern_return_t
mac_amdgpu_reset_device(MacAMDGPUUserClient *client)
{
    if (client == nullptr) return kIOReturnBadArgument;
    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, client->GetProvider());
    if (driver == nullptr) return kIOReturnNotAttached;
    IOPCIDevice *pci = mac_amdgpu_pci(driver);
    if (pci == nullptr) return kIOReturnUnsupported;

    // FLR first; fall back to upstream-port hot reset.
    kern_return_t ret = pci->Reset(kIOPCIDeviceResetTypeFunctionReset,
                                   kIOPCIDeviceResetOptionNone);
    if (ret == kIOReturnSuccess) {
        MACAMDGPU_LOG("FLR ok");
        return ret;
    }
    MACAMDGPU_LOG("FLR failed: %#x — trying hot reset", ret);
    ret = pci->Reset(kIOPCIDeviceResetTypeHotReset,
                     kIOPCIDeviceResetOptionNone);
    MACAMDGPU_LOG("hot reset: %#x", ret);
    return ret;
}

//============================================================
// MacAMDGPU::Start — log device, enumerate BARs, register service.
//============================================================
kern_return_t
IMPL(MacAMDGPU, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("super Start failed: %#x", ret);
        return ret;
    }

    ivars = IONewZero(MacAMDGPU_IVars, 1);
    if (ivars == nullptr) {
        return kIOReturnNoMemory;
    }

    IOPCIDevice *pci = mac_amdgpu_pci(this);
    if (pci == nullptr) {
        MACAMDGPU_LOG("provider is not an IOPCIDevice");
        IOSafeDeleteNULL(ivars, MacAMDGPU_IVars, 1);
        return kIOReturnUnsupported;
    }

    uint8_t bus = 0, device = 0, function = 0;
    uint16_t vendorID = 0xFFFF, deviceID = 0xFFFF;
    uint32_t classRev = 0;
    pci->GetBusDeviceFunction(&bus, &device, &function);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vendorID);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &deviceID);
    pci->ConfigurationRead32(kIOPCIConfigurationOffsetRevisionID, &classRev);

    uint16_t cmd = 0, status = 0;
    uint8_t  headerType = 0;
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
    pci->ConfigurationRead16(kIOPCIConfigurationOffsetStatus, &status);
    pci->ConfigurationRead8(kIOPCIConfigurationOffsetHeaderType, &headerType);

    MACAMDGPU_LOG("matched %02x:%02x.%u vendor=%04x device=%04x "
                  "class=%06x rev=%02x cmd=%04x status=%04x header=%02x",
                  (unsigned)bus, (unsigned)device, (unsigned)function,
                  (unsigned)vendorID, (unsigned)deviceID,
                  (unsigned)(classRev >> 8) & 0xFFFFFFu,
                  (unsigned)(classRev & 0xFFu),
                  (unsigned)cmd, (unsigned)status, (unsigned)headerType);

    for (uint8_t bar = 0; bar < 6; bar++) {
        uint8_t  memoryIndex = 0;
        uint64_t barSize = 0;
        uint8_t  barType = 0;
        kern_return_t barRet = pci->GetBARInfo(bar, &memoryIndex,
                                                &barSize, &barType);
        if (barRet == kIOReturnSuccess) {
            MACAMDGPU_LOG("BAR%u memoryIndex=%u size=%llu type=%{public}s",
                          (unsigned)bar, (unsigned)memoryIndex, barSize,
                          mac_amdgpu_bar_type_string(barType));
        }
    }

    RegisterService();
    MACAMDGPU_LOG("RegisterService done");
    return kIOReturnSuccess;
}

//============================================================
// MacAMDGPU::Stop — release ivars (PCI Open/Close lives on
// UserClient lifetime, see below).
//============================================================
kern_return_t
IMPL(MacAMDGPU, Stop)
{
    if (ivars != nullptr && ivars->pciOpen) {
        MACAMDGPU_LOG("WARNING: PCI still open in driver Stop "
                      "(opener=%p) — should have been closed by UserClient",
                      (void *)ivars->openerUserClient);
        ivars->pciOpen = false;
        ivars->openerUserClient = nullptr;
    }
    IOSafeDeleteNULL(ivars, MacAMDGPU_IVars, 1);
    return Stop(provider, SUPERDISPATCH);
}

//============================================================
// MacAMDGPU::NewUserClient — spawn one UserClient per IOServiceOpen.
//============================================================
kern_return_t
IMPL(MacAMDGPU, NewUserClient)
{
    if (type != 0) {
        MACAMDGPU_LOG("unsupported user-client type %u", (unsigned)type);
        return kIOReturnUnsupported;
    }

    IOService *clientService = nullptr;
    kern_return_t ret = Create(this, "MacAMDGPUUserClientProperties",
                               &clientService);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("Create UserClient failed: %#x", ret);
        return ret;
    }

    IOUserClient *typed = OSDynamicCast(IOUserClient, clientService);
    if (typed == nullptr) {
        clientService->release();
        MACAMDGPU_LOG("created service is not an IOUserClient");
        return kIOReturnUnsupported;
    }

    *userClient = typed;
    return kIOReturnSuccess;
}

//============================================================
// MacAMDGPUUserClient::Start
//============================================================
kern_return_t
IMPL(MacAMDGPUUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("user client super Start failed: %#x", ret);
        return ret;
    }

    if (OSDynamicCast(MacAMDGPU, provider) == nullptr) {
        MACAMDGPU_LOG("user client provider is not MacAMDGPU");
        return kIOReturnUnsupported;
    }

    ivars = IONewZero(MacAMDGPUUserClient_IVars, 1);
    if (ivars == nullptr) {
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

//============================================================
// MacAMDGPUUserClient::Stop
//============================================================
kern_return_t
IMPL(MacAMDGPUUserClient, Stop)
{
    // Order matters: release interrupts and DMA before closing PCI so
    // any outstanding kernel state has somewhere to drain to.
    mac_amdgpu_release_all_interrupts(this);
    mac_amdgpu_release_dma_buffer(this);

    // Release PCI Open if this client opened it. The Open/Close entities
    // must match per IOPCIFamily.
    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, GetProvider());
    if (driver != nullptr && driver->ivars != nullptr &&
        driver->ivars->pciOpen &&
        driver->ivars->openerUserClient == (IOService *)this) {
        IOPCIDevice *pci = mac_amdgpu_pci(driver);
        if (pci != nullptr) {
            pci->Close(this, 0);
        }
        driver->ivars->pciOpen = false;
        driver->ivars->openerUserClient = nullptr;
        MACAMDGPU_LOG("PCI closed on UserClient Stop");
    }

    IOSafeDeleteNULL(ivars, MacAMDGPUUserClient_IVars, 1);
    return Stop(provider, SUPERDISPATCH);
}

//============================================================
// ExternalMethod selector dispatcher.
//============================================================
kern_return_t
MacAMDGPUUserClient::ExternalMethod(uint64_t selector,
                                    IOUserClientMethodArguments *arguments,
                                    const IOUserClientMethodDispatch *dispatch,
                                    OSObject *target,
                                    void *reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;

    if (arguments == nullptr) {
        return kIOReturnBadArgument;
    }

    MacAMDGPU  *driver = OSDynamicCast(MacAMDGPU, GetProvider());
    IOPCIDevice *pci    = mac_amdgpu_pci(driver);
    if (driver == nullptr || pci == nullptr) {
        return kIOReturnNotAttached;
    }

    switch (selector) {

    case kMacAMDGPUMethodPing: {
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        // Echo a magic value the caller can verify against.
        arguments->scalarOutput[0] = 0xA117AB1Eu;  // "AMDGPU live"-ish
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetIdentity: {
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 7) {
            return kIOReturnBadArgument;
        }
        // Config-space reads need the PCI device to be Open()'d.
        kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
        if (openRet != kIOReturnSuccess) return openRet;
        uint8_t  bus = 0, dev = 0, fn = 0;
        uint16_t vid = 0xFFFF, did = 0xFFFF;
        uint32_t classRev = 0;
        pci->GetBusDeviceFunction(&bus, &dev, &fn);
        pci->ConfigurationRead16(kIOPCIConfigurationOffsetVendorID, &vid);
        pci->ConfigurationRead16(kIOPCIConfigurationOffsetDeviceID, &did);
        pci->ConfigurationRead32(kIOPCIConfigurationOffsetRevisionID,
                                 &classRev);
        arguments->scalarOutput[0] = bus;
        arguments->scalarOutput[1] = dev;
        arguments->scalarOutput[2] = fn;
        arguments->scalarOutput[3] = vid;
        arguments->scalarOutput[4] = did;
        arguments->scalarOutput[5] = (classRev >> 8) & 0xFFFFFFu;  // class+prog_if
        arguments->scalarOutput[6] = classRev & 0xFFu;             // revision
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetBARInfo: {
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }
        uint64_t barIndex = arguments->scalarInput[0];
        if (barIndex >= 6) {
            return kIOReturnBadArgument;
        }
        uint8_t  memoryIndex = 0;
        uint64_t barSize = 0;
        uint8_t  barType = 0;
        kern_return_t ret = pci->GetBARInfo((uint8_t)barIndex, &memoryIndex,
                                            &barSize, &barType);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        arguments->scalarOutput[0] = memoryIndex;
        arguments->scalarOutput[1] = barSize;
        arguments->scalarOutput[2] = barType;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetDiagnostics: {
        // Returns: cfg (cmd/status, BAR0..5 low+high), PM cap, BAR0/2/5
        // MMIO probes so the host can see exactly what each BAR reads.
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 16) {
            return kIOReturnBadArgument;
        }
        kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
        if (openRet != kIOReturnSuccess) return openRet;

        uint16_t cmd = 0, status = 0;
        uint32_t bar0_lo = 0, bar0_hi = 0;
        uint32_t bar2_lo = 0, bar2_hi = 0, bar5_cfg = 0;
        pci->ConfigurationRead16(0x04, &cmd);
        pci->ConfigurationRead16(0x06, &status);
        pci->ConfigurationRead32(0x10, &bar0_lo);
        pci->ConfigurationRead32(0x14, &bar0_hi);  // 64-bit BAR0 high dword
        pci->ConfigurationRead32(0x18, &bar2_lo);
        pci->ConfigurationRead32(0x1C, &bar2_hi);  // 64-bit BAR2 high dword
        pci->ConfigurationRead32(0x24, &bar5_cfg);

        uint8_t  pm_cap_ptr = 0;
        uint16_t pmcsr      = 0xFFFFu;
        {
            uint8_t cp = 0;
            pci->ConfigurationRead8(0x34, &cp);
            while (cp != 0 && cp != 0xFF) {
                uint16_t hdr = 0;
                pci->ConfigurationRead16(cp, &hdr);
                if ((hdr & 0xFF) == 0x01) {
                    pm_cap_ptr = cp;
                    pci->ConfigurationRead16(cp + 4, &pmcsr);
                    break;
                }
                cp = (hdr >> 8) & 0xFF;
            }
        }

        auto rdBar = [&](uint8_t memIdx, uint64_t byteOff) -> uint32_t {
            uint32_t v = 0xDEADBEEFu;  // distinct from 0 and 0xFFFFFFFF
            pci->MemoryRead32(memIdx, byteOff, &v);
            return v;
        };
        auto &bdev = driver->ivars->bringup.device;
        arguments->scalarOutput[0]  = ((uint64_t)status << 16) | cmd;
        arguments->scalarOutput[1]  = bar0_lo;
        arguments->scalarOutput[2]  = bar0_hi;
        arguments->scalarOutput[3]  = bar2_lo;
        arguments->scalarOutput[4]  = bar2_hi;
        arguments->scalarOutput[5]  = bar5_cfg;
        arguments->scalarOutput[6]  = ((uint64_t)pm_cap_ptr << 16) | pmcsr;
        arguments->scalarOutput[7]  = bdev.bar0Size;
        arguments->scalarOutput[8]  = bdev.bar2VisibleVRAMSize;
        // BAR5 is the MMIO register window on Bonaire+ AMDGPUs.
        // BAR0 is the framebuffer aperture (returns 0 pre-VRAM-setup).
        arguments->scalarOutput[9]  = rdBar(bdev.bar5MemIndex, 0x0000);
        arguments->scalarOutput[10] = rdBar(bdev.bar5MemIndex, 0x0004);
        arguments->scalarOutput[11] = rdBar(bdev.bar5MemIndex, 0x0DE3 * 4);
        arguments->scalarOutput[12] = rdBar(bdev.bar0MemIndex, 0x0000);
        arguments->scalarOutput[13] = rdBar(bdev.bar0MemIndex, 0x0004);
        // MP0_C2PMSG_33 (IFWI status) — read from BAR5 (the register
        // window), dword 0x0061 = byte 0x184. Bit 31 set = IFWI complete.
        arguments->scalarOutput[14] =
            rdBar(bdev.bar5MemIndex, (uint64_t)0x0061 * 4ULL);
        // memIdx values packed in case PCIDriverKit numbered them
        // differently from the BAR numbers (compacted indices).
        arguments->scalarOutput[15] =
            ((uint64_t)bdev.bar5MemIndex << 16) |
            ((uint64_t)bdev.bar2MemIndex << 8)  |
             (uint64_t)bdev.bar0MemIndex;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodDumpTMR: {
        // Read 16 dwords (64 bytes) from VRAM at the upstream
        // discovery TMR location: (vram_size << 20) - 1 MB. Uses
        // mmMM_INDEX/MM_DATA to reach offsets beyond the visible
        // BAR0 aperture. Output goes straight to scalarOutput so the
        // host UI can see it without depending on os_log delivery.
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 16) {
            return kIOReturnBadArgument;
        }
        kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
        if (openRet != kIOReturnSuccess) return openRet;

        auto &bdev = driver->ivars->bringup.device;

        // Read mmRCC_CONFIG_MEMSIZE (BAR5 dword 0x0DE3) for vram size in MB.
        uint32_t vram_mb = 0;
        pci->MemoryRead32(bdev.bar5MemIndex,
                          (uint64_t)0x0DE3 * 4ULL, &vram_mb);
        uint64_t vram_bytes = (uint64_t)vram_mb << 20;
        // DISCOVERY_TMR_OFFSET is 64 KB in upstream amdgpu_discovery.h.
        uint64_t tmr_offset = (vram_bytes > 0x10000)
                              ? (vram_bytes - 0x10000) : 0;

        auto rd_vram = [&](uint64_t pos) -> uint32_t {
            uint32_t idx = ((uint32_t)pos) | 0x80000000u;
            pci->MemoryWrite32(bdev.bar5MemIndex,
                               (uint64_t)0x0 * 4ULL, idx);  // MM_INDEX
            pci->MemoryWrite32(bdev.bar5MemIndex,
                               (uint64_t)0x6 * 4ULL,
                               (uint32_t)(pos >> 31));      // MM_INDEX_HI
            uint32_t v = 0xDEADBEEFu;
            pci->MemoryRead32(bdev.bar5MemIndex,
                              (uint64_t)0x1 * 4ULL, &v);    // MM_DATA
            return v;
        };

        for (int i = 0; i < 16; i++) {
            arguments->scalarOutput[i] =
                rd_vram(tmr_offset + (uint64_t)i * 4ULL);
        }
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodDumpPSP: {
        // Returns SOC15-resolved PSP register reads + MMHUB vram_start.
        // Outputs:
        //   [0] MP0 IP base (or 0xFFFFFFFF if unresolved)
        //   [1] C2PMSG_33 (IFWI ready, bit 31 = 1 when ready)
        //   [2] C2PMSG_35 (bootloader ready, bit 31 = 1 when ready)
        //   [3] C2PMSG_36 (firmware buffer address — host-written)
        //   [4] C2PMSG_64 (PSP ring base low — host-written)
        //   [5] C2PMSG_81 (sOS sign-of-life, bit 31 set when alive)
        //   [6] PSP ring create state (0=not created, 1=created)
        //   [7] MMHUB IP base (or 0xFFFFFFFF if unresolved)
        //   [8] regMMMC_VM_FB_LOCATION_BASE raw value
        //   [9] regMMMC_VM_FB_LOCATION_TOP raw value
        //   [10] computed vram_start (FB_LOCATION_BASE.FB_BASE << 24)
        //   [11] C2PMSG_67 (PSP ring wptr — dwords; non-zero ⇒ PSP saw our kick)
        //   [12] MMMC_VM_FB_OFFSET raw value (controls GMC vram_base_offset)
        //   [13] MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 read-back (after gart_enable)
        //   [14] MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32 read-back
        //   [15] MMVM_CONTEXT0_CNTL read-back (bit 0 = ENABLE_CONTEXT)
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 16) {
            return kIOReturnBadArgument;
        }
        kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
        if (openRet != kIOReturnSuccess) return openRet;
        auto &dev = driver->ivars->bringup.device;
        if (!dev.ip.isResolved(amdgpu::IPBlock::MP0)) {
            arguments->scalarOutput[0] = 0xFFFFFFFFu;
            for (int i = 1; i < 11; i++) arguments->scalarOutput[i] = 0;
            return kIOReturnSuccess;
        }
        uint32_t mp0_base = dev.ip.get(amdgpu::IPBlock::MP0);
        arguments->scalarOutput[0] = mp0_base;
        arguments->scalarOutput[1] = amdgpu::RREG32(dev, mp0_base + 0x0061);
        arguments->scalarOutput[2] = amdgpu::RREG32(dev, mp0_base + 0x0063);
        arguments->scalarOutput[3] = amdgpu::RREG32(dev, mp0_base + 0x0064);
        arguments->scalarOutput[4] = amdgpu::RREG32(dev, mp0_base + 0x0080);
        arguments->scalarOutput[5] = amdgpu::RREG32(dev, mp0_base + 0x0091);
        arguments->scalarOutput[6] = (uint64_t)driver->ivars->bringup.psp.ringCreated;

        if (dev.ip.isResolved(amdgpu::IPBlock::MMHUB)) {
            uint32_t mmhub_base = dev.ip.get(amdgpu::IPBlock::MMHUB);
            uint32_t fb_base_raw = amdgpu::RREG32(dev,
                mmhub_base + amdgpu::MMHUBRegs::MMMC_VM_FB_LOCATION_BASE);
            uint32_t fb_top_raw  = amdgpu::RREG32(dev,
                mmhub_base + amdgpu::MMHUBRegs::MMMC_VM_FB_LOCATION_TOP);
            uint32_t fb_off_raw  = amdgpu::RREG32(dev,
                mmhub_base + amdgpu::MMHUBRegs::MMMC_VM_FB_OFFSET);
            uint64_t vram_start =
                ((uint64_t)(fb_base_raw & amdgpu::MMHUBRegs::kFBBaseMask))
                << amdgpu::MMHUBRegs::kFBBaseShift;
            uint32_t pt_lo = amdgpu::RREG32(dev,
                mmhub_base + amdgpu::MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
            uint32_t pt_hi = amdgpu::RREG32(dev,
                mmhub_base + amdgpu::MMHUBRegs::MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32);
            uint32_t ctx0_cntl = amdgpu::RREG32(dev,
                mmhub_base + amdgpu::MMHUBRegs::MMVM_CONTEXT0_CNTL);
            arguments->scalarOutput[7]  = mmhub_base;
            arguments->scalarOutput[8]  = fb_base_raw;
            arguments->scalarOutput[9]  = fb_top_raw;
            arguments->scalarOutput[10] = vram_start;
            arguments->scalarOutput[12] = fb_off_raw;
            arguments->scalarOutput[13] = pt_lo;
            arguments->scalarOutput[14] = pt_hi;
            arguments->scalarOutput[15] = ctx0_cntl;
        } else {
            arguments->scalarOutput[7]  = 0xFFFFFFFFu;
            arguments->scalarOutput[8]  = 0;
            arguments->scalarOutput[9]  = 0;
            arguments->scalarOutput[10] = 0;
            arguments->scalarOutput[12] = 0;
            arguments->scalarOutput[13] = 0;
            arguments->scalarOutput[14] = 0;
            arguments->scalarOutput[15] = 0;
        }
        // C2PMSG_67 lives at dword offset 0x0083 — the "_67" is the
        // register's logical name in PSP's spec, not its register-file
        // offset. (C2PMSG_N actual offset = 0x40 + N.)
        arguments->scalarOutput[11] = amdgpu::RREG32(dev, mp0_base + 0x0083);
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodDumpCmdBuf: {
        // Read 16 dwords from the VRAM-backed PSP control buffers via
        // the BAR0 aperture. ring/cmd/fence live at fixed VRAM offsets
        // (kRingVRAMOffset / kCmdBufVRAMOffset / kFenceVRAMOffset from
        // psp_v14_0.cpp — kept in sync with the constants there).
        //   [0..3]  cmd_buf[0..3]     (header: buf_size, version, cmd_id, ...)
        //   [4..7]  cmd_buf[64..76]   (cmd-specific payload start)
        //   [8..11] cmd_buf[864..876] (response status region)
        //   [12]    fence_buf[0]      (PSP-written fence value)
        //   [13..15] ring_mem[0..8]   (first dwords of the ring frame)
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 16) {
            return kIOReturnBadArgument;
        }
        kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
        if (openRet != kIOReturnSuccess) return openRet;
        auto &psp = driver->ivars->bringup.psp;
        if (!psp.ringCreated) {
            for (int i = 0; i < 16; i++) arguments->scalarOutput[i] = 0;
            return kIOReturnSuccess;  // ring not created yet
        }
        // VRAM offsets must match psp_v14_0.cpp constants.
        constexpr uint64_t kRingVRAMOff   = 0x100000;
        constexpr uint64_t kCmdBufVRAMOff = 0x104000;
        constexpr uint64_t kFenceVRAMOff  = 0x108000;
        auto &devCtx = driver->ivars->bringup.device;
        auto cmdU32   = [&](uint64_t off) -> uint32_t {
            return amdgpu::RBAR2_32(devCtx, kCmdBufVRAMOff + off);
        };
        auto fenceU32 = [&](uint64_t off) -> uint32_t {
            return amdgpu::RBAR2_32(devCtx, kFenceVRAMOff + off);
        };
        auto ringU32  = [&](uint64_t off) -> uint32_t {
            return amdgpu::RBAR2_32(devCtx, kRingVRAMOff + off);
        };
        arguments->scalarOutput[0]  = cmdU32(0);
        arguments->scalarOutput[1]  = cmdU32(4);
        arguments->scalarOutput[2]  = cmdU32(8);
        arguments->scalarOutput[3]  = cmdU32(12);
        arguments->scalarOutput[4]  = cmdU32(64);
        arguments->scalarOutput[5]  = cmdU32(68);
        arguments->scalarOutput[6]  = cmdU32(72);
        arguments->scalarOutput[7]  = cmdU32(76);
        arguments->scalarOutput[8]  = cmdU32(864);
        arguments->scalarOutput[9]  = cmdU32(868);
        arguments->scalarOutput[10] = cmdU32(872);
        arguments->scalarOutput[11] = cmdU32(876);
        arguments->scalarOutput[12] = fenceU32(0);
        arguments->scalarOutput[13] = ringU32(0);
        arguments->scalarOutput[14] = ringU32(4);
        arguments->scalarOutput[15] = ringU32(8);
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSetupInterrupts:
        return mac_amdgpu_setup_interrupts(this);

    case kMacAMDGPUMethodWaitInterrupt: {
        // Async: stash the completion OSAction; InterruptOccurred wakes it.
        if (arguments->completion == nullptr) {
            return kIOReturnBadArgument;
        }
        if (ivars == nullptr || !ivars->interruptsSetUp) {
            return kIOReturnNotReady;
        }

        // Fast path: if any enabled+pending bit is already set, complete now.
        for (int i = 0; i < MACAMDGPU_IRQ_PENDING_WORDS; i++) {
            uint64_t en = __atomic_load_n(&ivars->irqEnabled[i],
                                          __ATOMIC_ACQUIRE);
            uint64_t pe = __atomic_load_n(&ivars->irqPending[i],
                                          __ATOMIC_ACQUIRE);
            if (en & pe) {
                arguments->completion->retain();
                AsyncCompletion(arguments->completion,
                                kIOReturnSuccess, nullptr, 0);
                arguments->completion->release();
                return kIOReturnSuccess;
            }
        }

        // No event yet — install the completion as pending.
        arguments->completion->retain();
        OSAction *prev = __atomic_exchange_n(&ivars->pendingInterruptNotify,
                                             arguments->completion,
                                             __ATOMIC_ACQ_REL);
        if (prev != nullptr) prev->release();
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSetIRQMask: {
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < MACAMDGPU_IRQ_PENDING_WORDS) {
            return kIOReturnBadArgument;
        }
        if (ivars == nullptr || ivars->irqEnabled == nullptr) {
            return kIOReturnNotReady;
        }
        for (int i = 0; i < MACAMDGPU_IRQ_PENDING_WORDS; i++) {
            __atomic_store_n(&ivars->irqEnabled[i],
                             arguments->scalarInput[i], __ATOMIC_RELEASE);
        }
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodAllocateDMABuffer: {
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 2 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 2) {
            return kIOReturnBadArgument;
        }
        uint64_t size  = arguments->scalarInput[0];
        uint64_t align = arguments->scalarInput[1];
        kern_return_t ret = mac_amdgpu_allocate_dma_buffer(this, size, align);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        arguments->scalarOutput[0] = ivars->dmaSegmentsCount;
        arguments->scalarOutput[1] = ivars->dmaSegmentsCount > 0 ?
            ivars->dmaSegments[0].address : 0;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodFreeDMABuffer:
        mac_amdgpu_release_dma_buffer(this);
        return kIOReturnSuccess;

    case kMacAMDGPUMethodResetDevice:
        return mac_amdgpu_reset_device(this);

    case kMacAMDGPUMethodInitDevice: {
        // scalarInput[0] = target BringupStage; scalarOutput[0] = reached.
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        // Lazy open + populate DeviceContext so any order of selector
        // calls works.
        kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
        if (openRet != kIOReturnSuccess) return openRet;
        auto stage = (amdgpu::BringupStage)arguments->scalarInput[0];
        kern_return_t ret = amdgpu::bringup_to(driver->ivars->bringup,
                                                  stage);
        arguments->scalarOutput[0] =
            (uint64_t)driver->ivars->bringup.reached;
        return ret;
    }

    case kMacAMDGPUMethodLoadFirmware: {
        // scalarInput[0] = fw type; [1] = size in bytes; the firmware
        // bytes are sourced from this client's DMABuffer (must be
        // allocated and contain the payload before this call).
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }
        uint64_t fwType = arguments->scalarInput[0];
        uint64_t fwSize = arguments->scalarInput[1];

        if (ivars == nullptr || ivars->dmaBuffer == nullptr) {
            MACAMDGPU_LOG("LoadFirmware: no DMABuffer on this client");
            return kIOReturnNotReady;
        }
        if (fwSize == 0 || fwSize > ivars->dmaBufferSize) {
            return kIOReturnBadArgument;
        }
        IOAddressSegment seg = {};
        if (ivars->dmaBuffer->GetAddressRange(&seg) != kIOReturnSuccess) {
            return kIOReturnInternalError;
        }

        auto &dev = driver->ivars->bringup.device;
        auto &psp = driver->ivars->bringup.psp;
        const uint8_t *bin = reinterpret_cast<const uint8_t *>(seg.address);

        switch (fwType) {
        case kMacAMDGPUFwTypeSOS: {
            // Ensure PSPInit ran so fw_pri is allocated.
            auto &br = driver->ivars->bringup;
            if (br.reached < amdgpu::BringupStage::PSPInit) {
                kern_return_t pir = amdgpu::bringup_to(br,
                    amdgpu::BringupStage::PSPInit);
                if (pir != kIOReturnSuccess) return pir;
            }

            // Parse the AMD firmware header (v1.x or v2.x) and populate
            // every sub-bin descriptor in psp (sos, kdb, sys, soc_drv,
            // intf_drv, dbg_drv, ras_drv, ipkeymgr_drv, etc.). Mirrors
            // upstream amdgpu_psp.c psp_init_sos_microcode.
            kern_return_t parseRet = amdgpu::psp_parse_sos_microcode(
                psp, bin, fwSize);
            if (parseRet != kIOReturnSuccess) {
                MACAMDGPU_LOG("LoadFirmware(SOS): parse failed kr=%#x",
                              parseRet);
                return parseRet;
            }
            MACAMDGPU_LOG("LoadFirmware(SOS): parsed package — "
                          "SOS=%llu KDB=%llu SYS=%llu SOC_DRV=%llu "
                          "INTF_DRV=%llu DBG_DRV=%llu RAS_DRV=%llu "
                          "IPKEYMGR=%llu SPL=%llu",
                          psp.sos.size_bytes, psp.kdb.size_bytes,
                          psp.sys.size_bytes, psp.soc_drv.size_bytes,
                          psp.intf_drv.size_bytes, psp.dbg_drv.size_bytes,
                          psp.ras_drv.size_bytes,
                          psp.ipkeymgr_drv.size_bytes,
                          psp.spl.size_bytes);

            // Load each sub-firmware in the upstream order, then SOS.
            kern_return_t ret = amdgpu::psp_load_sos_package(dev, psp);
            if (ret == kIOReturnSuccess) {
                if (br.reached < amdgpu::BringupStage::PSPLoadSOS) {
                    br.reached = amdgpu::BringupStage::PSPLoadSOS;
                }
            }
            return ret;
        }
        case kMacAMDGPUFwTypeKDB:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadKeyDatabase);
        case kMacAMDGPUFwTypeSPL:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadTosSPLTable);
        case kMacAMDGPUFwTypeSysDrv:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadSysDrv);
        case kMacAMDGPUFwTypeSocDrv:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadSocDrv);
        case kMacAMDGPUFwTypeIntfDrv:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadIntfDrv);
        case kMacAMDGPUFwTypeDbgDrv:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadHADDrv);
        case kMacAMDGPUFwTypeRASDrv:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadRASDrv);
        case kMacAMDGPUFwTypeIPKeyMgrDrv:
            return amdgpu::psp_bootloader_load_component(
                dev, psp, bin, fwSize, amdgpu::PSPBootloaderCmd::LoadIPKeyMgrDrv);
        default:
            // Post-SOS IP firmware path. Two host fwType encodings:
            //   0x100..0x1FF  single-payload IP firmware (legacy);
            //                 psp_fw_type = hostFwType - 0x100.
            //   0x200..0x2FF  per-file multi-payload (e.g. rlc.bin emits
            //                 up to 13 LOAD_IP_FW frames). The extractor
            //                 (`amdgpu_ucode_extract`) decodes the file
            //                 header version and emits all required
            //                 (fw_type, offset, size) tuples for us to
            //                 loop over.
            //
            // Mirrors upstream amdgpu_ucode_init_single_fw +
            // psp_get_fw_type + psp_execute_ip_fw_load, called per-IP
            // from psp_load_non_psp_fw.
            if ((fwType >= 0x100 && fwType < 0x200) ||
                (fwType >= 0x200 && fwType < 0x300)) {

                if (ivars->dmaSegmentsCount < 1) return kIOReturnNotReady;
                auto &gart = driver->ivars->bringup.gart;
                if (!gart.enabled) {
                    return kIOReturnNotReady;
                }

                // Decode the .bin into one-or-more LOAD_IP_FW payloads.
                amdgpu::UcodePayload payloads[amdgpu::kMaxUcodePayloadsPerFile];
                uint32_t count = amdgpu::amdgpu_ucode_extract(
                    fwType, bin, fwSize, payloads);
                if (count == 0) {
                    MACAMDGPU_LOG("LoadFirmware: extractor returned 0 payloads "
                                  "for fwType=%#llx size=%llu",
                                  fwType, fwSize);
                    return kIOReturnUnsupported;
                }

                // Bind the DMA buffer's bus addr range into GART once
                // (slot is reused across submits since the busAddr is
                // stable for the lifetime of the DMA buffer). The
                // per-payload GPU bus address = gartMCAddr + payload
                // offset_bytes.  Mirrors upstream's GTT-domain-BO flow:
                // amdgpu_ucode_init_bo allocates ONE big BO, copies all
                // payloads into it, and each (info->mc_addr, info->ucode_size)
                // points into that single BO.
                kern_return_t br = amdgpu::gart_bind_existing(
                    dev, gart,
                    ivars->dmaSegments[0].address,
                    ivars->dmaBufferSize,
                    &ivars->dmaGartBinding);
                if (br != kIOReturnSuccess) {
                    return br;
                }

                // Side-effect bookkeeping that used to live in the
                // single-payload path. Captures MES start addresses
                // from the MES file header (independent of any
                // particular payload), and flips sdma microcode_loaded
                // when the SDMA payload submits successfully.
                bool sdma_loaded_this_call = false;

                // MES start address extraction — pre-loop because it's
                // a property of the .bin file, not a per-payload value.
                // Upstream: amdgpu_mes.c:708-713 stashes
                // adev->mes.uc_start_addr[pipe] from the header before
                // PSP load. The KIQ pipe is not exposed on R9700 yet —
                // mes_v12_0 uses one file per pipe so we only see the
                // SCHED pipe via uni_mes/mes.bin.
                //
                // We trigger on the FILE-typed path (0x200+3 = MES_UNI)
                // and on the legacy CP_MES (0x100+33) single-payload
                // path that also loads a uni_mes-format file.
                if (fwType == kMacAMDGPUFwTypeFile_MES_UNI ||
                    fwType == 0x100ULL + amdgpu::PSPGfxFwType::CP_MES) {
                    if (fwSize >= sizeof(amdgpu::mes_firmware_header_v1_0)) {
                        auto *mhdr = reinterpret_cast<
                            const amdgpu::mes_firmware_header_v1_0 *>(bin);
                        uint64_t uc_addr =
                            static_cast<uint64_t>(mhdr->mes_uc_start_addr_lo) |
                            (static_cast<uint64_t>(mhdr->mes_uc_start_addr_hi) << 32);
                        amdgpu::mes_set_uc_start_addr(
                            driver->ivars->bringup.mes,
                            amdgpu::MESPipe::Sched, uc_addr);
                    }
                }

                // Submit each (fw_type, offset, size) as a separate
                // LOAD_IP_FW frame. Bail on first error — upstream
                // psp_load_non_psp_fw does the same (a failing IP fw
                // load aborts the whole bringup; we mirror that here
                // so subsequent failing loads don't mask the first).
                MACAMDGPU_LOG("LoadFirmware(fwType=%#llx): extractor produced "
                              "%u payload(s)", fwType, count);
                for (uint32_t i = 0; i < count; i++) {
                    uint64_t fwBusAddr =
                        ivars->dmaGartBinding.gartMCAddr + payloads[i].offset_bytes;
                    MACAMDGPU_LOG("  payload[%u]: fw_type=%u offset=%u size=%u",
                                  i, payloads[i].fw_type,
                                  payloads[i].offset_bytes,
                                  payloads[i].size_bytes);
                    kern_return_t r = amdgpu::psp_load_ip_fw(
                        dev, psp, fwBusAddr,
                        payloads[i].size_bytes, payloads[i].fw_type);
                    if (r != kIOReturnSuccess) {
                        MACAMDGPU_LOG("LoadFirmware(fwType=%#llx) payload[%u] "
                                      "(fw_type=%u) FAILED kr=%#x",
                                      fwType, i, payloads[i].fw_type, r);
                        return r;
                    }
                    if (payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA0 ||
                        payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA1 ||
                        payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA_UCODE_TH0) {
                        sdma_loaded_this_call = true;
                    }
                }
                if (sdma_loaded_this_call) {
                    driver->ivars->bringup.sdma.microcode_loaded = true;
                }
                return kIOReturnSuccess;
            }
            MACAMDGPU_LOG("LoadFirmware: fw type %llu not yet implemented",
                          fwType);
            return kIOReturnUnsupported;
        }
    }

    case kMacAMDGPUMethodSetIPBase: {
        // One-time bootstrap from userspace until we have on-die
        // discovery wired up. scalarInput[0]=block id, [1]=base offset.
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }
        uint64_t blockId = arguments->scalarInput[0];
        uint64_t base    = arguments->scalarInput[1];
        if (blockId >= (uint64_t)amdgpu::IPBlock::Count) {
            return kIOReturnBadArgument;
        }
        driver->ivars->bringup.device.ip.set(
            (amdgpu::IPBlock)blockId, (uint32_t)base);
        MACAMDGPU_LOG("IP base block=%llu set to %#llx", blockId, base);
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetIPBase: {
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 2) {
            return kIOReturnBadArgument;
        }
        uint64_t blockId = arguments->scalarInput[0];
        if (blockId >= (uint64_t)amdgpu::IPBlock::Count) {
            return kIOReturnBadArgument;
        }
        auto block = (amdgpu::IPBlock)blockId;
        arguments->scalarOutput[0] = driver->ivars->bringup.device.ip.get(block);
        arguments->scalarOutput[1] =
            driver->ivars->bringup.device.ip.isResolved(block) ? 1 : 0;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSubmitTestPM4: {
        // scalarInput[0] = timeout_us
        // scalarOutput[0] = fence value if observed, 0 on timeout
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        if (!driver->ivars->pciOpen) return kIOReturnNotOpen;
        uint64_t timeout_us = arguments->scalarInput[0];
        if (timeout_us == 0) timeout_us = 1000000;  // 1 s default
        uint32_t fence = 0;
        kern_return_t r = amdgpu::cp_submit_eop_test(
            driver->ivars->bringup.device,
            driver->ivars->bringup.cp,
            timeout_us, &fence);
        arguments->scalarOutput[0] = fence;
        return r;
    }

    case kMacAMDGPUMethodSDMACopyTest: {
        // scalarInput[0] = SDMA instance (0 or 1)
        // scalarInput[1] = src byte offset within DMABuffer
        // scalarInput[2] = dst byte offset within DMABuffer
        // scalarInput[3] = byte count
        // scalarInput[4] = timeout_us
        // scalarOutput[0] = kIOReturn from sdma_copy_linear_test
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 5 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        if (ivars == nullptr || ivars->dmaBuffer == nullptr ||
            ivars->dmaSegmentsCount < 1) {
            return kIOReturnNotReady;
        }
        uint64_t inst    = arguments->scalarInput[0];
        uint64_t src_off = arguments->scalarInput[1];
        uint64_t dst_off = arguments->scalarInput[2];
        uint64_t count   = arguments->scalarInput[3];
        uint64_t to_us   = arguments->scalarInput[4];
        if (inst >= amdgpu::kSDMAInstanceCount || count == 0 ||
            src_off + count > ivars->dmaBufferSize ||
            dst_off + count > ivars->dmaBufferSize) {
            return kIOReturnBadArgument;
        }
        // First segment bus base — we already enforce single-segment
        // mappings, so contiguous offsets are valid bus addresses.
        const uint64_t bus_base = ivars->dmaSegments[0].address;
        kern_return_t r = amdgpu::sdma_copy_linear_test(
            driver->ivars->bringup.device,
            driver->ivars->bringup.sdma.instance[inst],
            bus_base + src_off, bus_base + dst_off,
            static_cast<uint32_t>(count),
            to_us ? to_us : 1000000ull);
        arguments->scalarOutput[0] = static_cast<uint64_t>(r);
        return r;
    }

    case kMacAMDGPUMethodLoadDiscoveryBin: {
        // scalarInput[0] = size in bytes of the discovery binary
        // currently sitting at the start of this client's DMABuffer.
        // scalarOutput[0] = 1 if parse ok, 0 otherwise
        // scalarOutput[1] = number of IPs recognised (GC/MP0/MP1/...)
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 2) {
            return kIOReturnBadArgument;
        }
        if (ivars == nullptr || ivars->dmaBuffer == nullptr) {
            return kIOReturnNotReady;
        }
        uint64_t size = arguments->scalarInput[0];
        if (size == 0 || size > ivars->dmaBufferSize) {
            return kIOReturnBadArgument;
        }
        IOAddressSegment seg = {};
        if (ivars->dmaBuffer->GetAddressRange(&seg) != kIOReturnSuccess) {
            return kIOReturnInternalError;
        }
        amdgpu::DiscoveryParseResult res{};
        kern_return_t r = amdgpu::discovery_parse(
            reinterpret_cast<const uint8_t *>(seg.address),
            size, driver->ivars->bringup.device, &res);
        arguments->scalarOutput[0] = res.ok ? 1 : 0;
        arguments->scalarOutput[1] = res.num_ips_total;
        if (res.ok) {
            MACAMDGPU_LOG("discovery loaded: gc v%u.%u.%u, %u dies, %u ips",
                          res.ip_version_major, res.ip_version_minor,
                          res.ip_version_rev, res.num_dies, res.num_ips_total);
        } else {
            MACAMDGPU_LOG("discovery parse failed: %{public}s", res.err);
        }
        return r;
    }

    case kMacAMDGPUMethodBOAlloc: {
        // scalarInput[0] = size in bytes
        // scalarOutput[0] = handle (or 0 on failure)
        // scalarOutput[1] = bus address of the BO's first byte
        // scalarOutput[2] = byte offset within the client DMABuffer
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }
        if (ivars == nullptr || ivars->dmaBuffer == nullptr ||
            ivars->dmaSegmentsCount < 1) {
            return kIOReturnNotReady;
        }
        uint64_t size = arguments->scalarInput[0];
        if (size == 0) return kIOReturnBadArgument;
        // Round size up to a 16 KB multiple (AS DART page granularity).
        size = (size + MACAMDGPU_BO_ALIGN - 1) & ~(MACAMDGPU_BO_ALIGN - 1);
        // Bump-align the cursor too.
        ivars->boBumpOffset = (ivars->boBumpOffset + MACAMDGPU_BO_ALIGN - 1)
                            & ~(MACAMDGPU_BO_ALIGN - 1);
        if (ivars->boBumpOffset + size > ivars->dmaBufferSize) {
            return kIOReturnNoSpace;
        }
        // Find a free slot.
        uint32_t idx = MACAMDGPU_MAX_BO;
        for (uint32_t i = 0; i < MACAMDGPU_MAX_BO; i++) {
            if (!ivars->bos[i].in_use) { idx = i; break; }
        }
        if (idx == MACAMDGPU_MAX_BO) return kIOReturnNoResources;

        BOEntry &e = ivars->bos[idx];
        e.in_use      = true;
        e.byte_offset = ivars->boBumpOffset;
        e.size        = size;
        e.generation  = ++ivars->boGenCounter;
        ivars->boBumpOffset += size;

        const uint64_t handle = mac_amdgpu_bo_make_handle(e.generation, idx);
        arguments->scalarOutput[0] = handle;
        arguments->scalarOutput[1] =
            ivars->dmaSegments[0].address + e.byte_offset;
        arguments->scalarOutput[2] = e.byte_offset;
        MACAMDGPU_LOG("BOAlloc: handle=%#llx off=%#llx size=%llu",
                      handle, e.byte_offset, size);
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOFree: {
        // scalarInput[0] = handle
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr) return kIOReturnBadArgument;
        e->in_use = false;
        // size + byte_offset stay set so a stale handle keeps failing.
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOGetInfo: {
        // scalarInput[0] = handle
        // scalarOutput[0] = bus address
        // scalarOutput[1] = byte offset within the DMABuffer
        // scalarOutput[2] = size
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr || ivars->dmaSegmentsCount < 1) {
            return kIOReturnBadArgument;
        }
        arguments->scalarOutput[0] =
            ivars->dmaSegments[0].address + e->byte_offset;
        arguments->scalarOutput[1] = e->byte_offset;
        arguments->scalarOutput[2] = e->size;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSubmitIB: {
        // scalarInput[0] = ib BO handle
        // scalarInput[1] = ib size in dwords
        // scalarInput[2] = (reserved — queue id, must be 0 for now)
        // scalarOutput[0] = fence_value to wait on (returns 0 on err)
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 3 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr) return kIOReturnBadArgument;
        uint64_t ib_dw = arguments->scalarInput[1];
        if (ib_dw == 0 || ib_dw * 4 > e->size) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarInput[2] != 0) return kIOReturnUnsupported;
        if (!driver->ivars->pciOpen) return kIOReturnNotOpen;

        // The BO bytes live in our DMABuffer; map a CPU pointer via
        // GetAddressRange to copy them into the CP ring.
        IOAddressSegment seg = {};
        if (ivars->dmaBuffer->GetAddressRange(&seg) != kIOReturnSuccess) {
            return kIOReturnInternalError;
        }
        const uint32_t *ib_words = reinterpret_cast<const uint32_t *>(
            seg.address + e->byte_offset);

        auto &cp = driver->ivars->bringup.cp;
        // Stage the IB body into the ring, then append a fence so we
        // know when the CP has drained it.
        uint32_t wrote = amdgpu::cp_ring_write(cp, ib_words,
                                               static_cast<uint32_t>(ib_dw));
        if (wrote != ib_dw) return kIOReturnNoSpace;

        uint32_t fence = amdgpu::cp_emit_eop_fence(cp);
        kern_return_t r = amdgpu::cp_kick_doorbell(
            driver->ivars->bringup.device, cp);
        if (r != kIOReturnSuccess) return r;

        arguments->scalarOutput[0] = fence;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodMESAddQueue: {
        // scalarInput[0] = queue_type (0=GFX, 1=COMPUTE, 2=SDMA)
        // scalarInput[1] = doorbell_offset
        // scalarInput[2] = mqd BO handle (must be allocated by BOAlloc)
        // scalarInput[3] = wptr BO handle (or 0 for none)
        // scalarInput[4] = inprocess priority (0..4)
        // scalarOutput[0] = kIOReturn from mes_add_hw_queue
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 5 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        BOEntry *mqd_bo  = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[2]);
        BOEntry *wptr_bo = arguments->scalarInput[3] == 0
                          ? nullptr
                          : mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[3]);
        if (mqd_bo == nullptr) return kIOReturnBadArgument;
        const uint64_t bus_base = ivars->dmaSegments[0].address;

        amdgpu::MESAddQueueInput in{};
        in.process_id    = 1;
        in.queue_type    = static_cast<uint32_t>(arguments->scalarInput[0]);
        in.doorbell_offset = static_cast<uint32_t>(arguments->scalarInput[1]);
        in.mqd_addr      = bus_base + mqd_bo->byte_offset;
        in.wptr_addr     = wptr_bo ? (bus_base + wptr_bo->byte_offset) : 0;
        in.inprocess_gang_priority    =
            static_cast<uint32_t>(arguments->scalarInput[4]);
        in.gang_global_priority_level =
            static_cast<uint32_t>(arguments->scalarInput[4]);
        in.page_table_base_addr = 0;  // VMID 0 GART path
        in.gang_context_addr    = 0;
        in.process_context_addr = 0;
        in.pipe_id  = 0;
        in.queue_id = 0;
        in.flags    = 0;

        kern_return_t r = amdgpu::mes_add_hw_queue(
            driver->ivars->bringup.device,
            driver->ivars->bringup.mes,
            in);
        arguments->scalarOutput[0] = static_cast<uint64_t>(r);
        return r;
    }

    case kMacAMDGPUMethodQueryInfo: {
        // scalarInput[0] = info type tag
        // scalarOutput[N] = type-specific payload
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        const uint64_t infoType = arguments->scalarInput[0];
        auto &b = driver->ivars->bringup;
        switch (infoType) {
        case kMacAMDGPUInfoGFXVersion:
            if (arguments->scalarOutputCount < 3) return kIOReturnBadArgument;
            arguments->scalarOutput[0] = amdgpu::kIP_GFX.major;
            arguments->scalarOutput[1] = amdgpu::kIP_GFX.minor;
            arguments->scalarOutput[2] = amdgpu::kIP_GFX.rev;
            return kIOReturnSuccess;
        case kMacAMDGPUInfoVRAMSizes:
            if (arguments->scalarOutputCount < 2) return kIOReturnBadArgument;
            arguments->scalarOutput[0] = b.gmc.visible_vram_size;
            arguments->scalarOutput[1] = b.gmc.real_vram_size;
            return kIOReturnSuccess;
        case kMacAMDGPUInfoIPVersions: {
            if (arguments->scalarOutputCount < 4) return kIOReturnBadArgument;
            auto pack = [](amdgpu::IPVersion v) {
                return (uint64_t(v.major) << 16) | (uint64_t(v.minor) << 8) | v.rev;
            };
            arguments->scalarOutput[0] = pack(amdgpu::kIP_GMC);
            arguments->scalarOutput[1] = pack(amdgpu::kIP_SDMA);
            arguments->scalarOutput[2] = pack(amdgpu::kIP_PSP);
            arguments->scalarOutput[3] = pack(amdgpu::kIP_SMU);
            return kIOReturnSuccess;
        }
        case kMacAMDGPUInfoBringupReached:
            arguments->scalarOutput[0] = static_cast<uint64_t>(b.reached);
            return kIOReturnSuccess;
        default:
            return kIOReturnUnsupported;
        }
    }

    case kMacAMDGPUMethodWaitFence: {
        // scalarInput[0] = fence_value to wait for
        // scalarInput[1] = timeout_us
        // scalarOutput[0] = observed fence value
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 2 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint64_t target  = arguments->scalarInput[0];
        uint64_t to_us   = arguments->scalarInput[1];
        if (to_us == 0) to_us = 1000000ull;

        auto &cp = driver->ivars->bringup.cp;
        if (cp.fence_cpu == nullptr) return kIOReturnNotReady;

        const uint64_t step_us = 50;
        uint64_t elapsed = 0;
        while (elapsed < to_us) {
            uint64_t observed = *cp.fence_cpu;
            if (observed >= target) {
                arguments->scalarOutput[0] = observed;
                return kIOReturnSuccess;
            }
            uint32_t scratch = 0;
            for (int i = 0; i < 1000; i++) {
                scratch ^= static_cast<uint32_t>(*cp.fence_cpu);
            }
            (void)scratch;
            elapsed += step_us;
        }
        arguments->scalarOutput[0] = *cp.fence_cpu;
        return kIOReturnTimeout;
    }

    default:
        return kIOReturnUnsupported;
    }
}

//============================================================
// CopyClientMemoryForType — return a BAR descriptor for mapping.
//
// On first call we Open() the PCI device so _CopyDeviceMemoryWithIndex
// will succeed. Open lives until UserClient::Stop.
//============================================================
kern_return_t
IMPL(MacAMDGPUUserClient, CopyClientMemoryForType)
{
    if (memory == nullptr || options == nullptr) {
        return kIOReturnBadArgument;
    }

    // Non-BAR memory types — DMA buffer and IRQ shared page.
    if (type == kMacAMDGPUMemoryTypeDMABuffer) {
        if (ivars == nullptr || ivars->dmaBuffer == nullptr) {
            return kIOReturnNotReady;
        }
        ivars->dmaBuffer->retain();
        *options = 0;
        *memory  = ivars->dmaBuffer;
        return kIOReturnSuccess;
    }
    if (type == kMacAMDGPUMemoryTypeIRQState) {
        if (ivars == nullptr || ivars->irqSharedBuffer == nullptr) {
            return kIOReturnNotReady;
        }
        ivars->irqSharedBuffer->retain();
        *options = 0;
        *memory  = ivars->irqSharedBuffer;
        return kIOReturnSuccess;
    }

    if (type > kMacAMDGPUMemoryTypeBAR5) {
        return kIOReturnUnsupported;
    }

    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, GetProvider());
    if (driver == nullptr || driver->ivars == nullptr) {
        return kIOReturnNotAttached;
    }
    IOPCIDevice *pci = mac_amdgpu_pci(driver);
    if (pci == nullptr) {
        return kIOReturnUnsupported;
    }

    // Ensure PCI is open (lazy first-touch).
    if (!driver->ivars->pciOpen) {
        kern_return_t ret = pci->Open(this, 0);
        if (ret != kIOReturnSuccess) {
            MACAMDGPU_LOG("PCI Open failed in CopyClientMemoryForType: %#x",
                          ret);
            return ret;
        }
        driver->ivars->pciOpen = true;
        driver->ivars->openerUserClient = (IOService *)this;

        // Enable Memory Space + Bus Master in PCI command register.
        uint16_t cmd = 0;
        pci->ConfigurationRead16(0x04, &cmd);
        uint16_t wanted = cmd | 0x0006;  // bit 1 = MEM, bit 2 = BUSMASTER
        if (wanted != cmd) {
            pci->ConfigurationWrite16(0x04, wanted);
            pci->ConfigurationRead16(0x04, &cmd);
        }
        MACAMDGPU_LOG("PCI opened, command=%#x", (unsigned)cmd);

        // Populate per-device bringup context for Phase 1B.
        auto &bdev = driver->ivars->bringup.device;
        bdev.pci = pci;
        bdev.psoCAlive = false;
        bdev.smuOnline = false;
        bdev.gmcReady  = false;
        for (uint8_t bar = 0; bar < 6; bar++) {
            uint8_t  mi = 0;
            uint64_t sz = 0;
            uint8_t  ty = 0;
            if (pci->GetBARInfo(bar, &mi, &sz, &ty) != kIOReturnSuccess) {
                continue;
            }
            switch (bar) {
            case 0: bdev.bar0MemIndex = mi; bdev.bar0Size = sz; break;
            case 2: bdev.bar2MemIndex = mi; bdev.bar2VisibleVRAMSize = sz;
                    break;
            case 5: bdev.bar5MemIndex = mi; break;
            default: break;
            }
        }
        MACAMDGPU_LOG("bringup ctx: BAR0=%llu B BAR2(visible VRAM)=%llu B",
                      bdev.bar0Size, bdev.bar2VisibleVRAMSize);
    }

    uint8_t barIndex = (uint8_t)type;
    uint8_t  memoryIndex = 0;
    uint64_t barSize = 0;
    uint8_t  barType = 0;
    kern_return_t ret = pci->GetBARInfo(barIndex, &memoryIndex,
                                        &barSize, &barType);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("GetBARInfo BAR%u failed: %#x",
                      (unsigned)barIndex, ret);
        return ret;
    }

    IOMemoryDescriptor *barMem = nullptr;
    // opener must be the IOService that called Open()
    ret = pci->_CopyDeviceMemoryWithIndex(memoryIndex, &barMem,
                                          driver->ivars->openerUserClient);
    if (ret != kIOReturnSuccess || barMem == nullptr) {
        MACAMDGPU_LOG("_CopyDeviceMemoryWithIndex BAR%u failed: %#x",
                      (unsigned)barIndex, ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }

    MACAMDGPU_LOG("returning BAR%u descriptor size=%llu",
                  (unsigned)barIndex, barSize);
    *options = 0;
    *memory  = barMem;
    return kIOReturnSuccess;
}

//============================================================
// IH dispatch glue.
//
// When MSI-X fires we drain the IH ring (if the IH subsystem is
// active) and translate each entry's (client_id, src_id) to one
// of the kIRQBit* event bits, raising it in the calling client's
// irqPending bitmap. This lets a userspace caller block on
// WaitInterrupt and then read irqPending to find out *what* event
// fired, not just *that one* fired.
//
// Routes only to the primary client (driver->openerUserClient).
// Phase 1B keeps a single client per device — multi-client fan-out
// is a Phase 2+ concern.
//============================================================
struct IHDispatchCtx {
    MacAMDGPUUserClient_IVars *ivars;   // primary client's IVars
};

static void
mac_amdgpu_set_irq_bit(MacAMDGPUUserClient_IVars *iv, uint32_t bit_index)
{
    if (iv == nullptr || iv->irqPending == nullptr) return;
    if (bit_index >= MACAMDGPU_MAX_IRQ_VECTORS) return;
    uint32_t word = bit_index / 64;
    uint64_t bit  = 1ULL << (bit_index % 64);
    if (!(__atomic_load_n(&iv->irqEnabled[word], __ATOMIC_ACQUIRE) & bit)) {
        return;
    }
    __atomic_fetch_or(&iv->irqPending[word], bit, __ATOMIC_RELEASE);
}

static void
mac_amdgpu_ih_dispatch(const amdgpu::IHEntry &entry, void *user)
{
    auto *ctx = static_cast<IHDispatchCtx *>(user);
    if (ctx == nullptr || ctx->ivars == nullptr) return;

    uint32_t bit = kIRQBitIHOther;
    if (entry.client_id == amdgpu::IHSourceID::CLIENT_GFX) {
        if (entry.src_id == amdgpu::IHSourceID::SRC_CP_EOP) {
            bit = kIRQBitGFXEOPFence;
        } else if (entry.src_id == amdgpu::IHSourceID::SRC_CP_ECC_ERROR) {
            bit = kIRQBitGFXRASError;
        }
    } else if (entry.client_id == amdgpu::IHSourceID::CLIENT_ATHUB &&
               entry.src_id == amdgpu::IHSourceID::SRC_UTCL2_FAULT) {
        bit = kIRQBitVMFault;
    } else if (entry.client_id == amdgpu::IHSourceID::CLIENT_SDMA0 &&
               entry.src_id == amdgpu::IHSourceID::SRC_SDMA_TRAP) {
        bit = kIRQBitSDMA0Trap;
    } else if (entry.client_id == amdgpu::IHSourceID::CLIENT_SDMA1 &&
               entry.src_id == amdgpu::IHSourceID::SRC_SDMA_TRAP) {
        bit = kIRQBitSDMA1Trap;
    }
    mac_amdgpu_set_irq_bit(ctx->ivars, bit);
}

//============================================================
// InterruptOccurred — MSI-X fires, runs on irqQueue. Set pending
// bit; if there's a pending WaitInterrupt OSAction, wake it.
//
// Lock-free path: client maps the IRQ shared page and polls
// irqPending; or calls WaitInterrupt for an async wait.
//
// Phase 1B addition: after raising the raw-vector bit, also drain
// the IH ring (if it's online). Each ring entry maps to one of the
// kIRQBit* event bits, also raised in irqPending. This is what
// surfaces EOP fences from a PM4 submit to userspace.
//============================================================
void
IMPL(MacAMDGPUUserClient, InterruptOccurred)
{
    (void)count;
    (void)time;
    if (ivars == nullptr || !ivars->interruptsSetUp) return;

    uint32_t vector = UINT32_MAX;
    uint32_t *vref = (uint32_t *)action->GetReference();
    if (vref != nullptr) vector = *vref;
    if (vector >= ivars->numInterrupts) return;

    uint32_t word = vector / 64;
    uint64_t bit  = 1ULL << (vector % 64);

    // Drop the interrupt if client masked this vector.
    if (!(__atomic_load_n(&ivars->irqEnabled[word], __ATOMIC_ACQUIRE) & bit)) {
        return;
    }

    __atomic_fetch_or(&ivars->irqPending[word], bit, __ATOMIC_RELEASE);

    // Drain IH ring if active. The ring is per-device (lives on the
    // driver) so we route only to the primary opener client.
    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, GetProvider());
    if (driver != nullptr && driver->ivars != nullptr) {
        auto &bringup = driver->ivars->bringup;
        if (bringup.ih.enabled && bringup.ih.inited) {
            // Only the opener client receives IH events for now.
            auto *opener = OSDynamicCast(MacAMDGPUUserClient,
                                         driver->ivars->openerUserClient);
            if (opener != nullptr && opener->ivars != nullptr) {
                IHDispatchCtx dctx{ opener->ivars };
                uint32_t n = amdgpu::ih_drain(bringup.device, bringup.ih,
                                              &mac_amdgpu_ih_dispatch,
                                              &dctx);
                if (bringup.ih.overflows_seen > 0) {
                    mac_amdgpu_set_irq_bit(opener->ivars, kIRQBitIHOverflow);
                }
                (void)n;
            }
        }
    }

    // Wake any outstanding WaitInterrupt caller.
    OSAction *notify = __atomic_exchange_n(&ivars->pendingInterruptNotify,
                                           nullptr, __ATOMIC_ACQ_REL);
    if (notify != nullptr) {
        AsyncCompletion(notify, kIOReturnSuccess, nullptr, 0);
        notify->release();
    }
}

//============================================================
// AsyncCompletion — IIG dispatch target. The interesting work
// happens inside ExternalMethod's WaitInterrupt case and inside
// InterruptOccurred above; this override exists only because
// IOUserClient demands it.
//============================================================
void
IMPL(MacAMDGPUUserClient, AsyncCompletion)
{
    (void)action;
    (void)status;
    (void)asyncData;
    (void)asyncDataCount;
}
