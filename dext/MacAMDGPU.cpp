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
#include <time.h>

#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSData.h>
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
#include "amdgpu/amdgpu_pci_rebar.h"
#include "amdgpu/amdgpu_client_lifecycle.h"
#include "amdgpu/amdgpu_buffer_io.h"
#include "amdgpu/amdgpu_vram_io.h"

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
    // v0.1.24 — runtime engine health snapshot + DPM toggle.
    kMacAMDGPUMethodLiveStatus        = 30,
    kMacAMDGPUMethodDisableSmuFeatures = 33,
    // v0.1.27: per-BO map.
    kMacAMDGPUMethodBOMap             = 36,
    // v0.1.25 — VRAM->VRAM SDMA copy smoke test (sysmem-free variant of
    // kMacAMDGPUMethodSDMACopyTest). GART-bound sysmem is fragile on
    // AS+TB5 (see feedback_mac_amdgpu_dart_tb5_pcie_reads), so we run
    // src+dst out of VRAM and verify via MM_INDEX/MM_DATA readback.
    kMacAMDGPUMethodSDMACopyVRAM      = 34,
    // v0.1.26 — first PM4 packet on KIQ. PACKET3_NOP + PACKET3_RELEASE_MEM
    // (fence write to a VRAM-resident slot). Confirms CP MEC firmware
    // is processing PM4 packets from the KIQ ring.
    kMacAMDGPUMethodCPKIQSmoke         = 35,
    // v0.1.28 — command-stream submission ABI. Userspace builds a CS
    // dword-by-dword via the dext and submits via the existing SubmitIB
    // selector (19), which now consumes a CS handle instead of a BO.
    // First pass: SDMA IP only; GFX/COMPUTE return kIOReturnUnsupported.
    kMacAMDGPUMethodCSCreate          = 37,
    kMacAMDGPUMethodCSWriteDwords     = 38,
    kMacAMDGPUMethodCSDestroy         = 39,
    // v0.1.29 — per-state GFXCLK soft-clamp. scalarInput[0] selects
    // a power state (0=auto, 1=low, 2=nominal, 3=high, 4=peak).
    kMacAMDGPUMethodSetPowerState      = 40,
    kMacAMDGPUMethodGetReBARInfo       = 41, // read-only PCIe capability query
    kMacAMDGPUMethodShutdownGPU        = 42, // reset, close PCI, discard session
    kMacAMDGPUMethodRuntimeBuild       = 43, // actual responding binary, no hardware access
    kMacAMDGPUMethodHostMemoryTest     = 44, // data-verified SDMA transfers through GART
    kMacAMDGPUMethodComputeTest        = 45, // fixed wave32 shader with full readback
    kMacAMDGPUMethodCollectMetrics     = 46, // owner-only one-shot SMU telemetry
    kMacAMDGPUMethodMetricsSnapshot    = 47, // cached CPU snapshot, observer only
    kMacAMDGPUMethodBOCopy             = 48, // bounded synchronous SDMA, owned BOs
    kMacAMDGPUMethodBOWrite            = 49, // verified BAR0 staging upload
    kMacAMDGPUMethodBORead             = 50, // BAR0 staging readback
};

// v0.1.28 — IP types accepted by CSCreate. Match the upstream
// AMDGPU_HW_IP_* layout where convenient (GFX=0, COMPUTE=1, SDMA=2 in
// libdrm); we renumber locally to keep SDMA at 0 because that's the
// only IP we ship in v0.1.28.
enum {
    kMacAMDGPUCSIPTypeSDMA    = 0,
    kMacAMDGPUCSIPTypeGFX     = 1,
    kMacAMDGPUCSIPTypeCompute = 2,
};

// v0.1.29 — Power state IDs (in sync with Swift host).
enum {
    kMacAMDGPUPowerStateAuto    = 0,
    kMacAMDGPUPowerStateLow     = 1,
    kMacAMDGPUPowerStateNominal = 2,
    kMacAMDGPUPowerStateHigh    = 3,
    kMacAMDGPUPowerStatePeak    = 4,
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
    // gc_<v>_toc.bin — table of contents that PSP parses to compute
    // the TMR layout for autoload-supported chips. Required BEFORE any
    // LOAD_IP_FW for SDMA/CP/MES (those firmwares live in TMR slots
    // PSP allocates from the TOC; without it PSP rejects them with
    // TEE_BAD_PARAMETERS = 0xFFFF0006). Mirrors upstream psp_load_toc
    // (amdgpu_psp.c:840), called from psp_tmr_init.
    kMacAMDGPUFwTypeFile_TOC        = 0x200 + 7,
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
    // v0.1.27: per-BO mappings. A successful BOMap(handle) returns a
    // memory_type value of (kMacAMDGPUMemoryTypeBOBase + bo_index);
    // userspace then calls IOConnectMapMemory64(conn, type, …) to
    // obtain the cpu_va. Using a high base keeps these distinct from
    // BAR / DMA / IRQ memory types.
    kMacAMDGPUMemoryTypeBOBase    = 0x10000,
};

#define MACAMDGPU_MAX_IRQ_VECTORS    256
#define MACAMDGPU_IRQ_PENDING_WORDS  4   // 4 × 64 = 256 vectors
#define MACAMDGPU_MAX_DMA_SEGMENTS   32
#define MACAMDGPU_DMA_BUFFER_MAX     (1536ULL * 1024ULL * 1024ULL)
#define MACAMDGPU_MAX_BO             64
#define MACAMDGPU_BO_ALIGN           amdgpu::kASPageSize  // 16 KB

// Per-client BO table entry.
//
// v0.1.27 upgrade: BOs now have an explicit `domain` (VRAM vs GTT) and
// own their underlying storage. Pre-v0.1.27 every BO was just a
// [byte_offset, byte_offset+size) carve-out of the client's single
// pre-allocated DMA buffer (a "sysmem"-only path). That still works —
// `kBODomainGTTLegacy` keeps the old semantics for SubmitIB /
// MESAddQueue which read `byte_offset` directly off the entry — but
// new BOs can also be backed by:
//
//   - kBODomainVRAM: an allocation from GMCContext::vram_alloc; the BO
//     lives inside the BAR0-LOW window (24 MB..256 MB) so userspace can
//     map it via BAR0. `vram_offset` is the offset from gmc.vram_start.
//
//   - kBODomainGTT:  a fresh IOBufferMemoryDescriptor + IODMACommand
//     allocated *per BO* and bound into the global GART. `gtt_buf`
//     holds the buffer, `gtt_dma` the DART mapping, `gtt_bus_addr` the
//     DART bus address, and `gpu_va` the GART MC address. This is the
//     domain that maps cleanly to Mesa's BUFFER_DOMAIN_GTT.
//
// `bo_handle` returned to userspace encodes (generation << 32) | index
// so stale handles from a closed BO can't reach a fresh allocation in
// the same slot.
enum {
    kBODomainGTTLegacy = 0,   // pre-v0.1.27: subrange of client DMA buffer
    kBODomainVRAM      = 1,   // VRAM-resident, BAR0-LOW mapped
    kBODomainGTT       = 2,   // sysmem, DART-pinned, GART-bound
    kBODomainDeviceVRAM = 3,  // GPU-only VRAM above the CPU-visible BAR0 window
};

struct BOEntry {
    bool      in_use;
    uint32_t  domain;          // kBODomain*
    uint64_t  size;            // user-visible size in bytes
    uint64_t  alignment;
    uint32_t  generation;

    // GTT-legacy: only byte_offset is used; gpu_va == 0.
    // VRAM:       gpu_va = gmc.vram_start + vram_offset; vram_offset
    //             is the offset from vram_start.
    // GTT (new):  gpu_va = GART MC address; gtt_bus_addr is the raw
    //             DART bus address; gtt_buf/gtt_dma own the storage.
    uint64_t  byte_offset;     // legacy: offset into client DMA buffer
    uint64_t  vram_offset;     // VRAM: offset from gmc.vram_start
    uint64_t  gpu_va;          // unified GPU MC address (or 0 for legacy)

    IOBufferMemoryDescriptor *gtt_buf;
    IODMACommand             *gtt_dma;
    uint64_t  gtt_bus_addr;
    amdgpu::GARTBinding gttBinding; // authoritative mapping/storage ownership

    // Cached cpu pointer for in-dext access (e.g. CP_DMA / IB staging).
    // Userspace gets its own mapping via IOConnectMapMemory64 against
    // the per-BO memory type id returned by BOMap.
    void     *cpu_addr;
};

// v0.1.28 — command-stream entry. One per CSCreate; freed by
// CSDestroy. The cpu_buffer is dext-owned scratch storage that
// userspace appends to via CSWriteDwords; SubmitIB walks the
// accumulated dwords and turns them into ring writes on the chosen
// IP. Handle layout: (index << 16) | generation.
#define MACAMDGPU_MAX_CS              16
#define MACAMDGPU_CS_CAPACITY_DW      1024
struct CSEntry {
    bool       in_use;
    uint32_t   ip_type;       // kMacAMDGPUCSIPType{SDMA,GFX,Compute}
    uint32_t   capacity_dw;   // capacity of cpu_buffer in dwords
    uint32_t   written_dw;    // dwords appended so far
    uint32_t  *cpu_buffer;    // dext-owned scratch (IONewZero(uint32_t, capacity))
    uint32_t   generation;
    uint32_t   last_fence;    // fence value emitted by the latest SubmitIB
    uint32_t   ip_instance;   // for SDMA: which engine (0 or 1)
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
struct MacAMDGPUUserClient_IVars;
struct MacAMDGPU_IVars {
    bool stopping;              // atomic: read by client/IRQ dispatch queues
    bool       pciOpen;
    bool       shutdownBlocked;  // failed shutdown: only status/retry allowed
    bool       shutdownInProgress; // atomic admission barrier for new clients
    uint32_t   connectedClients; // atomic, decremented only after Stop drains
    IOService *openerUserClient;  // tracked so Open/Close entities match
    IOPCIDevice *retainedPCI;     // outlives superclass Stop until final free
    MacAMDGPUUserClient_IVars *quarantinedClient; // backing retained after failed reset
    amdgpu::ClientSubmission submission;

    // Phase 1B: per-device bringup state shared across user clients.
    // Populated lazily when PCI is opened. Stages run on demand via
    // InitDevice selector.
    amdgpu::BringupContext bringup;

    // Serial queue protecting all mutations of shared per-device state
    // (bringup stages, bump allocators, ring wptrs, opener tracking).
    // Required for safe multi-UserClient / multi-session operation on the
    // same card (see approved multi-GPU + multi-session plan).
    IODispatchQueue *bringupQueue;   // created in Start, never null after success
};

//
// UserClient state. DMA, MSI-X, IRQ shared page all live here so
// each userspace process gets isolated resources.
//
struct MacAMDGPUUserClient_IVars {
    bool claimed;
    bool mappedBAR; // direct MMIO mappings cannot be revoked by this selector
    bool stopping;                 // atomic: also read by the IRQ queue
    uint32_t stopPendingSources;   // atomic cancellation countdown + submission sentinel
    IODispatchQueue *stopQueue;    // retained default queue for final cleanup
    IOService *stopProvider;       // retained until superclass Stop finishes
    MacAMDGPU *ownerDriver;        // keeps shared bringup state alive through teardown

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

    // v0.1.28 — command-stream table. cs_handle = (index << 16) | gen.
    // CSCreate allocates `cpu_buffer` (1024 dwords) per slot; CSDestroy
    // frees it; SubmitIB consumes the accumulated dwords and emits a
    // ring submission on the chosen IP (SDMA only in this version).
    CSEntry   cs[MACAMDGPU_MAX_CS];
    uint32_t  csGenCounter;

    // GART binding for the DMA buffer. Lazily populated on the first
    // LOAD_IP_FW submit; reused across subsequent submits (the host
    // streams different firmware bytes into the same DART-mapped
    // buffer, so the busAddr is stable). Reset on EnsureDMABuffer
    // when the buffer is reallocated.
    // Legacy bringup.gart binding handle. Retained until the legacy
    // amdgpu_gart.cpp parallel path is fully retired.
    amdgpu::GARTBinding dmaGartBinding;
    // Cached GMC GART MC address for the host's DMA buffer once it's
    // been bound into gmc.gart_pt_cpu. Reset to 0 when the DMA buffer
    // is freed.
    uint64_t            dmaGartMcAddr;
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

static inline uint32_t
mac_amdgpu_bo_handle_index(uint64_t handle)
{
    return static_cast<uint32_t>(handle & 0xFFFFFFFFull);
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

//
// CS handle helpers — v0.1.28. Handle layout matches spec:
// (index << 16) | (generation & 0xFFFF). Generation is bumped on
// destroy so stale handles map to a freed slot fail lookup.
//
static inline uint64_t
mac_amdgpu_cs_make_handle(uint32_t generation, uint32_t index)
{
    return (static_cast<uint64_t>(index & 0xFFFFu) << 16) |
           static_cast<uint64_t>(generation & 0xFFFFu);
}

static CSEntry *
mac_amdgpu_cs_lookup(MacAMDGPUUserClient_IVars *ivars, uint64_t handle)
{
    if (ivars == nullptr) return nullptr;
    uint32_t idx = static_cast<uint32_t>((handle >> 16) & 0xFFFFu);
    uint32_t gen = static_cast<uint32_t>(handle & 0xFFFFu);
    if (idx >= MACAMDGPU_MAX_CS) return nullptr;
    CSEntry *e = &ivars->cs[idx];
    if (!e->in_use || (e->generation & 0xFFFFu) != gen) return nullptr;
    return e;
}

static void
mac_amdgpu_cs_free_slot(CSEntry *e)
{
    if (e == nullptr) return;
    if (e->cpu_buffer != nullptr) {
        IOSafeDeleteNULL(e->cpu_buffer, uint32_t, e->capacity_dw);
    }
    e->in_use = false;
    e->ip_type = 0;
    e->capacity_dw = 0;
    e->written_dw = 0;
    e->last_fence = 0;
    e->ip_instance = 0;
}

//
// Resolve a BO's GPU MC address regardless of domain. Legacy
// (GTTLegacy) BOs return the bus address of their slice within the
// client DMA buffer; new VRAM/GTT BOs return their pre-computed gpu_va.
//
static uint64_t
mac_amdgpu_bo_gpu_addr(MacAMDGPUUserClient_IVars *ivars, BOEntry *e)
{
    if (ivars == nullptr || e == nullptr) return 0;
    if (e->domain == kBODomainGTTLegacy) {
        if (ivars->dmaSegmentsCount < 1) return 0;
        return ivars->dmaSegments[0].address + e->byte_offset;
    }
    return e->gpu_va;
}

//
// Resolve a BO's CPU address for in-dext readback (e.g. SubmitIB
// staging the IB into the CP ring). Returns nullptr if the BO has
// no CPU mapping handy (e.g. a VRAM BO with no live BAR0 mapping).
//
static void *
mac_amdgpu_bo_cpu_addr(MacAMDGPUUserClient_IVars *ivars, BOEntry *e)
{
    if (ivars == nullptr || e == nullptr) return nullptr;
    if (e->domain == kBODomainGTTLegacy) {
        if (ivars->dmaBuffer == nullptr) return nullptr;
        IOAddressSegment seg = {};
        if (ivars->dmaBuffer->GetAddressRange(&seg) != kIOReturnSuccess) {
            return nullptr;
        }
        return reinterpret_cast<void *>(seg.address + e->byte_offset);
    }
    return e->cpu_addr;
}

//
// Release the storage owned by a single BO entry, then mark it free.
// VRAM BOs return their range to gmc.vram_alloc; GTT BOs release the
// per-BO binding after verified reset/PCI isolation. Live BOFree instead
// invalidates its PTEs and both hub TLBs before completing DMA. Non-trailing
// aperture holes remain reserved until the session is reset.
//
// Forward decl only — implementation lives below the driver class
// definitions where bringup context fields are visible.
static void
mac_amdgpu_bo_release_all(MacAMDGPUUserClient_IVars *ivars,
                          IOService *driverService);
static void mac_amdgpu_release_quarantine(MacAMDGPU *driver);

//============================================================
// Helpers.
//============================================================
static IOPCIDevice *
mac_amdgpu_pci(IOService *service)
{
    if (service == nullptr) {
        return nullptr;
    }
    auto *driver = OSDynamicCast(MacAMDGPU, service);
    if (driver != nullptr && driver->ivars != nullptr &&
        driver->ivars->retainedPCI != nullptr) {
        return driver->ivars->retainedPCI;
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

// Standard capabilities occupy at most 48 aligned slots in config space.
// Reject malformed/cyclic chains rather than blocking the lifecycle queue.
static uint8_t
mac_amdgpu_find_pm_capability(IOPCIDevice *pci)
{
    uint8_t pointer = 0;
    pci->ConfigurationRead8(0x34, &pointer);
    uint64_t visited = 0;
    for (unsigned steps = 0; pointer != 0 && steps < 48; ++steps) {
        if (pointer < 0x40 || pointer > 0xFC || (pointer & 3)) return 0;
        const uint64_t bit = uint64_t(1) << ((pointer - 0x40) / 4);
        if (visited & bit) return 0;
        visited |= bit;
        uint16_t header = 0xFFFF;
        pci->ConfigurationRead16(pointer, &header);
        if (header == 0xFFFF) return 0;
        if ((header & 0xFF) == 1) return pointer <= 0xF8 ? pointer : 0;
        pointer = uint8_t(header >> 8);
    }
    return 0;
}

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
    if (driver->ivars->shutdownBlocked) return kIOReturnNotReady;
    if (driver->ivars->pciOpen)
        return driver->ivars->openerUserClient == opener ? kIOReturnSuccess : kIOReturnBusy;

    // Closing the opener releases client DMA mappings. Re-enabling bus
    // mastering on retained rings could expose those stale addresses.
    if (driver->ivars->bringup.reached != amdgpu::BringupStage::None)
        return kIOReturnNotReady;

    kern_return_t ret = pci->Open(opener, 0);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("ensure_open: PCI Open failed: %#x", ret);
        return ret;
    }
    driver->ivars->pciOpen = true;
    __atomic_store_n(&driver->ivars->openerUserClient, opener, __ATOMIC_RELEASE);

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
            break;
        case 2: bdev.bar2MemIndex = mi; bdev.bar2Size = sz; break; // doorbells
        case 5: bdev.bar5MemIndex = mi; break;       // MMIO register window
        default: break;
        }
    }
    MACAMDGPU_LOG("ensure_open: PCI opened, cmd=%#x, "
                  "BAR0(visible VRAM)=%llu B, BAR2(doorbell)=%llu B, "
                  "BAR5(registers)=512KB",
                  (unsigned)cmd, bdev.bar0Size, bdev.bar2Size);

    // Read PCI config space + try to wake device to D0.
    uint32_t bar0_cfg = 0, bar2_cfg = 0, bar2_cfg_hi = 0, bar5_cfg = 0;
    uint16_t status = 0;
    pci->ConfigurationRead32(0x10, &bar0_cfg);
    pci->ConfigurationRead32(0x18, &bar2_cfg);
    pci->ConfigurationRead32(0x1C, &bar2_cfg_hi);  // 64-bit BAR2 high dword
    pci->ConfigurationRead32(0x24, &bar5_cfg);
    pci->ConfigurationRead16(0x06, &status);
    MACAMDGPU_LOG("ensure_open: config — BAR0=%#010x BAR2=%#010x:%#010x "
                  "BAR5=%#010x status=%#06x cmd=%#06x",
                  bar0_cfg, bar2_cfg_hi, bar2_cfg, bar5_cfg, (unsigned)status,
                  (unsigned)cmd);

    // Cache BAR2's PCIe bus address — used by enable_doorbell_selfring_aperture
    // (port of nbio_v7_11.c:149). BAR2 LOW carries flag bits in [3:0] for a
    // 64-bit prefetchable memory BAR; mask them off. HIGH is the upper dword
    // verbatim. If config reads are unreliable on AS (the BAR config-reg
    // read quirk), the resulting base will be wrong but selfring's EN bit
    // may still be enough — fall back to base=0 if BAR2 LOW reads as 0.
    {
        const uint64_t bar2_phys_lo =
            static_cast<uint64_t>(bar2_cfg & 0xFFFFFFF0u);
        const uint64_t bar2_phys_hi =
            static_cast<uint64_t>(bar2_cfg_hi) << 32;
        bdev.doorbell.base = bar2_phys_hi | bar2_phys_lo;
        MACAMDGPU_LOG("ensure_open: doorbell.base (BAR2 bus addr) = %#llx",
                      (unsigned long long)bdev.doorbell.base);
    }

    // Look for the PM capability and force D0. The capability list
    // pointer is at config offset 0x34.
    const uint8_t cap_ptr = mac_amdgpu_find_pm_capability(pci);
    if (cap_ptr != 0) {
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

static kern_return_t
mac_amdgpu_admit_external(IOService *client, MacAMDGPU *driver,
                          IOPCIDevice *pci, uint64_t selector)
{
    if (selector == kMacAMDGPUMethodMESAddQueue) return kIOReturnUnsupported;
    if (selector == kMacAMDGPUMethodCollectMetrics) {
        // Sampling cannot acquire a new hardware session. It belongs to the
        // client that already initialized the GPU, not a monitoring observer.
        if (!driver->ivars->pciOpen || driver->ivars->openerUserClient != client)
            return kIOReturnNotOpen;
        return driver->ivars->submission.poll() ? kIOReturnSuccess : kIOReturnBusy;
    }
    const bool observer = selector == kMacAMDGPUMethodRuntimeBuild ||
                          selector == kMacAMDGPUMethodMetricsSnapshot ||
                          selector == kMacAMDGPUMethodPing ||
                          selector == kMacAMDGPUMethodQueryInfo;
    if (!observer && selector != kMacAMDGPUMethodShutdownGPU) {
        kern_return_t ownerRet = mac_amdgpu_ensure_open(client, driver, pci);
        if (ownerRet != kIOReturnSuccess) return ownerRet;
        // Raw packets have no BO list. Until a scheduler owns their references,
        // all other mutation/resource recycling waits for the one live fence.
        if (!driver->ivars->submission.poll() &&
            selector != kMacAMDGPUMethodWaitFence &&
            selector != kMacAMDGPUMethodBOGetInfo)
            return kIOReturnBusy;
    }
    return kIOReturnSuccess;
}

static void
mac_amdgpu_release_dma_state(MacAMDGPUUserClient_IVars *state)
{
    if (state == nullptr) {
        return;
    }
    if (state->dmaCommand != nullptr) {
        state->dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    }
    OSSafeReleaseNULL(state->dmaCommand);
    OSSafeReleaseNULL(state->dmaBuffer);
    state->dmaBufferSize    = 0;
    state->dmaFlags         = 0;
    state->dmaSegmentsCount = 0;
    memset(state->dmaSegments, 0,
           sizeof(state->dmaSegments));
    // The bus address goes away with the buffer; the GART slot stays
    // mapped to a stale address until something rewrites it. Clear the
    // binding's MC addr so the next LOAD_IP_FW path rebinds afresh.
    memset(&state->dmaGartBinding, 0,
           sizeof(state->dmaGartBinding));
    state->dmaGartMcAddr = 0;
}

static void
mac_amdgpu_release_dma_buffer(MacAMDGPUUserClient *client)
{
    if (client) mac_amdgpu_release_dma_state(client->ivars);
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

    // Firmware/rings may still reference the existing DMA address. Repeated
    // same-size requests are idempotent; replacement requires Shutdown GPU.
    if (driver->ivars->bringup.reached != amdgpu::BringupStage::None) {
        return client->ivars->dmaBuffer != nullptr &&
               client->ivars->dmaBufferSize == requestedSize
                   ? kIOReturnSuccess : kIOReturnBusy;
    }
    uint64_t alignment = 0, rounded = 0;
    if (!amdgpu::client_allocation_shape(requestedSize, requestedAlignment,
                                         amdgpu::kASPageSize, alignment, rounded))
        return kIOReturnBadArgument;
    mac_amdgpu_release_dma_buffer(client);

    // Apple Silicon page size is 16 KB. DART rejects mappings that
    // aren't page-aligned; coerce upward if the caller asked for less.

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
    // Called only after the Stop cancellation barrier. Partial setup owns
    // a queue/shared page too, even if interruptsSetUp was never published.
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
    if (pending != nullptr) {
        client->AsyncCompletion(pending, kIOReturnAborted, nullptr, 0);
        pending->release();
    }

    client->ivars->irqPending = nullptr;
    client->ivars->irqEnabled = nullptr;
    OSSafeReleaseNULL(client->ivars->irqSharedBuffer);
}

static void
mac_amdgpu_stop_source_drained(MacAMDGPUUserClient *client)
{
    if (__atomic_sub_fetch(&client->ivars->stopPendingSources, 1,
                           __ATOMIC_ACQ_REL) != 0) return;
    // Cleanup must share the default queue with ExternalMethod and Stop.
    // Running it on an IRQ callback queue would race newly rejected RPCs.
    IOService *provider = client->ivars->stopProvider;
    client->ivars->stopQueue->DispatchAsync(^{ client->FinishStop(provider); });
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
    if (client->ivars->irqQueue != nullptr) {
        return kIOReturnStillOpen;
    }

    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, client->GetProvider());
    if (driver == nullptr) return kIOReturnNotAttached;
    IOPCIDevice *pci = mac_amdgpu_pci(driver);
    if (pci == nullptr) return kIOReturnUnsupported;

    IODispatchQueue *queue = nullptr;
    // Use the owning driver's serial queue for IRQ delivery too. A handler
    // cannot remain in MMIO/IH work while root Stop closes PCI or an RPC
    // mutates the same ring state. Cancellation itself remains asynchronous.
    kern_return_t ret = client->CopyDispatchQueue(kIOServiceDefaultQueueName,
                                                  &queue);
    if (ret != kIOReturnSuccess || queue == nullptr) {
        MACAMDGPU_LOG("IRQ CopyDispatchQueue failed: %#x", ret);
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
        // Own even a partially configured source until the Stop barrier.
        client->ivars->interruptSources[i] = src;
        OSAction *action = nullptr;
        ret = client->CreateActionInterruptOccurred(sizeof(uint32_t),
                                                    &action);
        if (ret != kIOReturnSuccess || action == nullptr) {
            OSSafeReleaseNULL(action);
            MACAMDGPU_LOG("CreateAction v=%u: %#x", (unsigned)i, ret);
            break;
        }
        uint32_t *vref = (uint32_t *)action->GetReference();
        if (vref != nullptr) *vref = i;

        ret = src->SetHandler(action);
        if (ret != kIOReturnSuccess) {
            action->release();
            MACAMDGPU_LOG("SetHandler v=%u: %#x", (unsigned)i, ret);
            break;
        }
        // SetHandler retains the action; balance our CreateAction reference.
        action->release();
        ret = src->SetEnable(true);
        if (ret != kIOReturnSuccess) {
            MACAMDGPU_LOG("SetEnable v=%u: %#x", (unsigned)i, ret);
            break;
        }
        registered++;
    }

    client->ivars->numInterrupts   = registered;
    __atomic_store_n(&client->ivars->interruptsSetUp, registered > 0,
                     __ATOMIC_RELEASE);
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
    if (driver == nullptr || driver->ivars == nullptr) return kIOReturnNotAttached;
    if (driver->ivars->bringup.reached != amdgpu::BringupStage::None) {
        // A live reset needs engine quiescence, BO preservation, and a full
        // firmware/ring rebuild. FLR alone leaves initialized software state
        // pointing at hardware that has lost that state.
        MACAMDGPU_LOG("reset rejected after stage %u: reconnect for a fresh driver session",
                      (unsigned)driver->ivars->bringup.reached);
        return kIOReturnBusy;
    }
    IOPCIDevice *pci = mac_amdgpu_pci(driver);
    if (pci == nullptr) return kIOReturnUnsupported;

    if (driver->ivars->openerUserClient != client ||
        client->ivars->mappedBAR || client->ivars->irqQueue != nullptr)
        return kIOReturnBusy;

    // A failed function reset must not disturb other devices on the bridge.
    kern_return_t ret = pci->Reset(kIOPCIDeviceResetTypeFunctionReset,
                                   kIOPCIDeviceResetOptionNone);
    if (ret == kIOReturnSuccess) {
        MACAMDGPU_LOG("FLR ok");
        return ret;
    }
    MACAMDGPU_LOG("FLR failed: %#x; bridge reset is not permitted", ret);
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

    // Create the serial bringup protection queue early (before any
    // bringup or allocator work). This is the foundation for safe
    // multi-UserClient operation on the same card (per the approved plan).
    IODispatchQueue *bqueue = nullptr;
    kern_return_t qret = IODispatchQueue::Create("MacAMDGPUBringup", 0, 0, &bqueue);
    if (qret != kIOReturnSuccess || bqueue == nullptr) {
        IOSafeDeleteNULL(ivars, MacAMDGPU_IVars, 1);
        MACAMDGPU_LOG("IODispatchQueue::Create (MacAMDGPUBringup) failed: %#x", qret);
        return qret != kIOReturnSuccess ? qret : kIOReturnNoMemory;
    }
    ivars->bringupQueue = bqueue;
    qret = SetDispatchQueue(kIOServiceDefaultQueueName, bqueue);
    if (qret != kIOReturnSuccess) {
        bqueue->release();
        IOSafeDeleteNULL(ivars, MacAMDGPU_IVars, 1);
        MACAMDGPU_LOG("SetDispatchQueue (MacAMDGPUBringup) failed: %#x", qret);
        return qret;
    }

    IOPCIDevice *pci = mac_amdgpu_pci(this);
    if (pci == nullptr) {
        MACAMDGPU_LOG("provider is not an IOPCIDevice");
        ivars->bringupQueue->release();
        IOSafeDeleteNULL(ivars, MacAMDGPU_IVars, 1);
        return kIOReturnUnsupported;
    }

    pci->retain();
    ivars->retainedPCI = pci;

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
// MacAMDGPU::Stop — gate new work and stop bus mastering. Shared
// storage remains alive until retained user clients have drained.
//============================================================
kern_return_t
IMPL(MacAMDGPU, Stop)
{
    if (ivars != nullptr) {
        __atomic_store_n(&ivars->stopping, true, __ATOMIC_RELEASE);
        amdgpu::smu_metrics_invalidate(ivars->bringup.metrics, kIOReturnNotAttached);
        if (ivars->pciOpen && ivars->openerUserClient != nullptr) {
            // PCIDriverKit Close disables Bus Lead Enable and Memory Space
            // Enable. Do this before any DMA descriptors can be completed.
            ivars->retainedPCI->Close(ivars->openerUserClient, 0);
            ivars->pciOpen = false;
            __atomic_store_n(&ivars->openerUserClient, (IOService *)nullptr,
                             __ATOMIC_RELEASE);
            MACAMDGPU_LOG("driver Stop: PCI closed; shared storage retained until clients drain");
        }
    }
    return Stop(provider, SUPERDISPATCH);
}

void
MacAMDGPU::free()
{
    if (ivars != nullptr) {
        // Every successful UserClient Start retains this service until its
        // callbacks and cleanup finish. No live client can observe this free.
        mac_amdgpu_release_quarantine(this);
        amdgpu::bringup_release_resources(ivars->bringup);
        OSSafeReleaseNULL(ivars->bringupQueue);
        OSSafeReleaseNULL(ivars->retainedPCI);
        IOSafeDeleteNULL(ivars, MacAMDGPU_IVars, 1);
        MACAMDGPU_LOG("driver free: shared bringup storage released");
    }
    IOService::free();
}

//============================================================
// MacAMDGPU::NewUserClient — spawn one UserClient per IOServiceOpen.
//============================================================
kern_return_t
IMPL(MacAMDGPU, NewUserClient)
{
    if (ivars == nullptr || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnNotAttached;
    }
    if (__atomic_load_n(&ivars->shutdownInProgress, __ATOMIC_ACQUIRE))
        return kIOReturnBusy;
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

    auto *driver = OSDynamicCast(MacAMDGPU, provider);
    if (driver == nullptr) {
        MACAMDGPU_LOG("user client provider is not MacAMDGPU");
        return kIOReturnUnsupported;
    }

    if (driver->ivars == nullptr ||
        __atomic_load_n(&driver->ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnNotAttached;
    }

    // Serialize subsequent RPCs, IRQs and Stop with the owning driver's
    // lifecycle. IRQ cancellation still needs its asynchronous completion barrier.
    IODispatchQueue *ownerQueue = nullptr;
    ret = driver->CopyDispatchQueue(kIOServiceDefaultQueueName, &ownerQueue);
    if (ret != kIOReturnSuccess || ownerQueue == nullptr) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }
    ret = SetDispatchQueue(kIOServiceDefaultQueueName, ownerQueue);
    ownerQueue->release();
    if (ret != kIOReturnSuccess) return ret;

    ivars = IONewZero(MacAMDGPUUserClient_IVars, 1);
    if (ivars == nullptr) {
        return kIOReturnNoMemory;
    }
    __atomic_add_fetch(&driver->ivars->connectedClients, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&driver->ivars->shutdownInProgress, __ATOMIC_ACQUIRE)) {
        __atomic_sub_fetch(&driver->ivars->connectedClients, 1, __ATOMIC_ACQ_REL);
        IOSafeDeleteNULL(ivars, MacAMDGPUUserClient_IVars, 1);
        return kIOReturnBusy;
    }
    driver->retain();
    ivars->ownerDriver = driver;
    return kIOReturnSuccess;
}

//============================================================
// Release every BO owned by this user client. Used at UserClient
// Stop so that VRAM allocations return to gmc.vram_alloc and GTT
// IOBufferMemoryDescriptors release their DART pin. Safe to call on
// a partially-initialised ivars (skips entries with in_use == false).
//============================================================
static void
mac_amdgpu_bo_release_all(MacAMDGPUUserClient_IVars *ivars,
                          IOService *driverService)
{
    if (ivars == nullptr) return;
    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, driverService);
    for (uint32_t i = 0; i < MACAMDGPU_MAX_BO; i++) {
        BOEntry &e = ivars->bos[i];
        if (!e.in_use) continue;
        if (amdgpu::buffer_vram_domain(e.domain) && driver != nullptr &&
            driver->ivars != nullptr) {
            amdgpu::VRAMAllocation va = {};
            va.gpu_va    = e.gpu_va;
            va.size      = e.size;
            va.alignment = e.alignment;
            va.cpu_ptr   = nullptr;
            auto &gmc = driver->ivars->bringup.gmc;
            auto &allocator = e.domain == kBODomainVRAM ? gmc.vram_alloc : gmc.device_vram_alloc;
            allocator.free(va);
        }
        else if (e.domain == kBODomainGTT) {
            // This bulk path runs only after reset/PCI isolation.
            amdgpu::gart_release_after_reset(e.gttBinding);
        }
        e.in_use   = false;
        e.gtt_buf  = nullptr;
        e.gtt_dma  = nullptr;
        e.cpu_addr = nullptr;
    }
    ivars->boBumpOffset = 0;
}

static void
mac_amdgpu_release_client_storage(MacAMDGPUUserClient_IVars *state, MacAMDGPU *driver)
{
    mac_amdgpu_bo_release_all(state, driver);
    mac_amdgpu_release_dma_state(state);
    for (auto &cs : state->cs)
        if (cs.in_use) mac_amdgpu_cs_free_slot(&cs);
}

static void
mac_amdgpu_release_quarantine(MacAMDGPU *driver)
{
    auto *state = driver->ivars->quarantinedClient;
    if (!state) return;
    mac_amdgpu_release_client_storage(state, driver);
    IOSafeDeleteNULL(driver->ivars->quarantinedClient, MacAMDGPUUserClient_IVars, 1);
}

// Keep all DMA backing pinned until FLR has completed and bus mastering is
// verified off. Unlike the cold-start helper, this must never fall back to a
// bridge hot reset, which could disrupt other functions/devices on that link.
static kern_return_t
mac_amdgpu_quiesce_for_shutdown(IOPCIDevice *pci, uint64_t &phase)
{
    phase = 1; // validate a live endpoint and FLR support before changing it
    uint16_t vendor = 0xFFFF;
    pci->ConfigurationRead16(0, &vendor);
    if (vendor != 0x1002) return kIOReturnNotAttached;
    uint64_t cap = 0;
    kern_return_t ret = pci->FindPCICapability(kIOPCICapabilityIDPCIExpress, 0, &cap);
    if (ret != kIOReturnSuccess || cap < 0x40 || cap > 0xF4 || (cap & 3))
        return kIOReturnUnsupported;
    uint32_t deviceCaps = 0xFFFFFFFF;
    pci->ConfigurationRead32((uint32_t)cap + 4, &deviceCaps);
    if (deviceCaps == 0xFFFFFFFF) return kIOReturnNotAttached;
    if (!(deviceCaps & (1u << 28))) return kIOReturnUnsupported;

    phase = 2; // stop new PCI transactions, retaining every buffer
    uint16_t command = 0xFFFF;
    pci->ConfigurationRead16(4, &command);
    if (command == 0xFFFF) return kIOReturnNotAttached;
    pci->ConfigurationWrite16(4, command & ~uint16_t(4));
    command = 0xFFFF;
    pci->ConfigurationRead16(4, &command);
    if (command == 0xFFFF || (command & 4)) return kIOReturnNotReady;

    phase = 3; // drain in-flight PCIe transactions before issuing FLR
    const uint64_t startNS = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (;;) {
        uint16_t status = 0xFFFF;
        pci->ConfigurationRead16((uint32_t)cap + 0x0A, &status);
        if (status == 0xFFFF) return kIOReturnNotAttached;
        if (!(status & (1u << 5))) break; // PCI_EXP_DEVSTA_TRPND
        const uint64_t nowNS = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (nowNS - startNS >= 1000000000ull) return kIOReturnTimeout;
        IOSleep(1);
    }

    phase = 4; // reset purges engine queues before their storage is unpinned
    ret = pci->Reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);
    if (ret != kIOReturnSuccess) return ret;

    // Reset restores configuration state. Verify the saved BM-off state was
    // restored, and that the endpoint is still present, before releasing DMA.
    phase = 5;
    vendor = 0xFFFF;
    pci->ConfigurationRead16(0, &vendor);
    command = 0xFFFF;
    pci->ConfigurationRead16(4, &command);
    if (vendor != 0x1002 || command == 0xFFFF || (command & 4))
        return kIOReturnNotReady;
    return kIOReturnSuccess;
}

static kern_return_t
mac_amdgpu_shutdown_gpu(MacAMDGPUUserClient *client, uint64_t &phase)
{
    auto *driver = client->ivars->ownerDriver;
    auto *state = driver->ivars;
    auto *pci = state->retainedPCI;
    phase = 0;
    __atomic_store_n(&state->shutdownInProgress, true, __ATOMIC_RELEASE);
    struct AdmissionGuard {
        bool *flag;
        ~AdmissionGuard() { __atomic_store_n(flag, false, __ATOMIC_RELEASE); }
    } admission { &state->shutdownInProgress };

    // Other clients may own DMA or access BAR mappings outside our RPC queue.
    // IRQ owners must close and let the existing cancellation barrier drain.
    if (__atomic_load_n(&state->connectedClients, __ATOMIC_ACQUIRE) != 1 ||
        (state->pciOpen && state->openerUserClient != client) ||
        client->ivars->mappedBAR || client->ivars->pendingInterruptNotify != nullptr)
        return kIOReturnBusy;
    for (auto *source : client->ivars->interruptSources)
        if (source != nullptr) return kIOReturnBusy;

    // A previous owner may have closed with shared rings retained. Reopen
    // exclusively for reset, without ensure_open (which enables bus mastering).
    state->shutdownBlocked = true;
    amdgpu::smu_metrics_invalidate(state->bringup.metrics, kIOReturnNotReady);
    if (!state->pciOpen) {
        kern_return_t ret = pci->Open(client, 0);
        if (ret != kIOReturnSuccess) return ret;
        state->pciOpen = true;
        __atomic_store_n(&state->openerUserClient, (IOService *)client, __ATOMIC_RELEASE);
    }
    kern_return_t ret = mac_amdgpu_quiesce_for_shutdown(pci, phase);
    if (ret != kIOReturnSuccess) {
        MACAMDGPU_LOG("Shutdown GPU failed at phase %llu: %#x; backing retained, retry permitted",
                      phase, ret);
        return ret;
    }
    // SDK Close is void; its documented contract disables BM and MEM. BM was
    // also explicitly verified off above while configuration access was open.
    pci->Close(client, 0);
    state->pciOpen = false;
    __atomic_store_n(&state->openerUserClient, (IOService *)nullptr, __ATOMIC_RELEASE);
    state->submission = {};
    mac_amdgpu_release_quarantine(driver);
    mac_amdgpu_bo_release_all(client->ivars, driver);
    for (auto &cs : client->ivars->cs)
        if (cs.in_use) mac_amdgpu_cs_free_slot(&cs);
    mac_amdgpu_release_dma_buffer(client);
    amdgpu::bringup_release_resources(state->bringup);
    state->shutdownBlocked = false;
    phase = 6;
    MACAMDGPU_LOG("Shutdown GPU complete: FLR, PCI closed, all session storage released; ready to reinitialize");
    return kIOReturnSuccess;
}

//============================================================
// MacAMDGPUUserClient::Stop
//============================================================
kern_return_t
IMPL(MacAMDGPUUserClient, Stop)
{
    if (ivars == nullptr) return Stop(provider, SUPERDISPATCH);
    if (__atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnSuccess;
    }
    kern_return_t ret = CopyDispatchQueue(kIOServiceDefaultQueueName,
                                          &ivars->stopQueue);
    if (ret != kIOReturnSuccess || ivars->stopQueue == nullptr) {
        MACAMDGPU_LOG("Stop cannot obtain completion queue: %#x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }
    __atomic_store_n(&ivars->stopping, true, __ATOMIC_RELEASE);
    if (ivars->ownerDriver && ivars->ownerDriver->ivars &&
        ivars->ownerDriver->ivars->openerUserClient == this)
        amdgpu::smu_metrics_invalidate(ivars->ownerDriver->ivars->bringup.metrics,
                                     kIOReturnNotReady);
    retain();
    provider->retain();
    ivars->stopProvider = provider;
    // Count every source before issuing any cancellation, including setup
    // failures. The sentinel prevents synchronous callbacks completing Stop
    // while the submission loop still accesses ivars.
    uint32_t count = 0;
    for (uint32_t i = 0; i < MACAMDGPU_MAX_IRQ_VECTORS; ++i) {
        if (ivars->interruptSources[i] != nullptr) ++count;
    }
    __atomic_store_n(&ivars->stopPendingSources, count + 1, __ATOMIC_RELEASE);
    MACAMDGPU_LOG("UserClient Stop: draining %u interrupt sources", count);
    auto *client = this;
    for (uint32_t i = 0; i < MACAMDGPU_MAX_IRQ_VECTORS; ++i) {
        IOInterruptDispatchSource *src = ivars->interruptSources[i];
        if (src == nullptr) continue;
        auto drained = ^{
            client->ivars->interruptSources[i] = nullptr;
            src->release();
            mac_amdgpu_stop_source_drained(client);
        };
        ret = src->Cancel(drained);
        if (ret != kIOReturnSuccess) {
            MACAMDGPU_LOG("Stop Cancel vector %u failed %#x; trying disable/drain", i, ret);
            ret = src->SetEnableWithCompletion(false, drained);
            if (ret != kIOReturnSuccess) {
                // Neither API promised quiescence. Keep the source and its
                // client/provider/backing alive rather than cause a use-after-free.
                MACAMDGPU_LOG("Stop blocked: vector %u cannot drain (%#x); backing retained", i, ret);
            }
        }
    }
    mac_amdgpu_stop_source_drained(this); // drop submission sentinel
    return kIOReturnSuccess;
}

void
MacAMDGPUUserClient::FinishStop(IOService *provider)
{
    // All source callbacks have finished. No new RPC may access resources
    // after stopping was published, and this runs on their default queue.
    mac_amdgpu_release_all_interrupts(this);

    MacAMDGPU *driver = OSDynamicCast(MacAMDGPU, provider);
    bool quarantine = false;
    bool resetComplete = false;
    if (driver != nullptr && driver->ivars != nullptr &&
        driver->ivars->pciOpen && driver->ivars->openerUserClient == this) {
        auto *state = driver->ivars;
        uint64_t phase = 0;
        amdgpu::smu_metrics_invalidate(state->bringup.metrics, kIOReturnNotReady);
        // Root Stop already isolates an unplugged/terminated provider. For a
        // live client close, flush engine work before recycling VRAM or DMA.
        kern_return_t stopped = mac_amdgpu_quiesce_for_shutdown(state->retainedPCI, phase);
        resetComplete = stopped == kIOReturnSuccess;
        state->retainedPCI->Close(this, 0);
        state->pciOpen = false;
        __atomic_store_n(&state->openerUserClient, (IOService *)nullptr, __ATOMIC_RELEASE);
        if (!resetComplete) {
            state->shutdownBlocked = true;
            // A retry client cannot own new storage while shutdownBlocked.
            quarantine = state->quarantinedClient == nullptr;
            MACAMDGPU_LOG("client close: reset failed at phase %llu (%#x); PCI closed, backing quarantined", phase, stopped);
        } else {
            state->submission = {};
            mac_amdgpu_release_quarantine(driver);
        }
    }

    if (!quarantine) mac_amdgpu_release_client_storage(ivars, driver);
    if (resetComplete) {
        amdgpu::bringup_release_resources(driver->ivars->bringup);
        driver->ivars->shutdownBlocked = false;
    }

    IODispatchQueue *completionQueue = ivars->stopQueue;
    MacAMDGPU *ownerDriver = ivars->ownerDriver;
    __atomic_sub_fetch(&ownerDriver->ivars->connectedClients, 1, __ATOMIC_ACQ_REL);
    if (quarantine) {
        ownerDriver->ivars->quarantinedClient = ivars;
        ivars->ownerDriver = nullptr;
        ivars->stopQueue = nullptr;
        ivars->stopProvider = nullptr;
        ivars = nullptr;
    } else {
        IOSafeDeleteNULL(ivars, MacAMDGPUUserClient_IVars, 1);
    }
    MACAMDGPU_LOG("UserClient Stop: callbacks drained; completing superclass Stop");
    Stop(provider, SUPERDISPATCH);
    provider->release();
    ownerDriver->release();
    completionQueue->release();
    release();
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

    if (ivars == nullptr || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnNotAttached;
    }

    MACAMDGPU_LOG("ExternalMethod entry: sel=%llu args=%p", selector, arguments);
    if (arguments == nullptr) {
        MACAMDGPU_LOG("ExternalMethod: arguments==nullptr → BadArgument");
        return kIOReturnBadArgument;
    }

    MacAMDGPU  *driver = OSDynamicCast(MacAMDGPU, GetProvider());
    IOPCIDevice *pci    = mac_amdgpu_pci(driver);
    if (driver == nullptr || driver->ivars == nullptr || pci == nullptr ||
        __atomic_load_n(&driver->ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnNotAttached;
    }

    if (driver->ivars->shutdownBlocked &&
        selector != kMacAMDGPUMethodRuntimeBuild &&
        selector != kMacAMDGPUMethodMetricsSnapshot &&
        selector != kMacAMDGPUMethodShutdownGPU &&
        selector != kMacAMDGPUMethodPing && selector != kMacAMDGPUMethodQueryInfo)
        return kIOReturnNotReady;

    // A completed stage is historical after the PCI owner has closed.
    // Keep cached status readable, but require a fresh attachment before
    // any operation can reuse rings or DMA addresses from that session.
    if (!driver->ivars->pciOpen &&
        driver->ivars->bringup.reached != amdgpu::BringupStage::None &&
        selector != kMacAMDGPUMethodRuntimeBuild &&
        selector != kMacAMDGPUMethodMetricsSnapshot &&
        selector != kMacAMDGPUMethodShutdownGPU &&
        selector != kMacAMDGPUMethodPing &&
        selector != kMacAMDGPUMethodQueryInfo &&
        selector != kMacAMDGPUMethodGetBARInfo) {
        return kIOReturnNotReady;
    }

    kern_return_t admission = mac_amdgpu_admit_external(this, driver, pci, selector);
    if (admission != kIOReturnSuccess) return admission;

    switch (selector) {

    case kMacAMDGPUMethodCollectMetrics: {
        if (arguments->scalarInputCount != 0 || !arguments->scalarOutput ||
            arguments->scalarOutputCount < 3 || arguments->structureInput ||
            arguments->structureInputDescriptor || arguments->structureOutputDescriptor ||
            arguments->structureOutputMaximumSize != 0)
            return kIOReturnBadArgument;
        auto &state = *driver->ivars;
        const bool ready = state.pciOpen && !state.stopping &&
            !state.shutdownBlocked && !state.shutdownInProgress &&
            state.bringup.reached == amdgpu::BringupStage::SDMAInit;
        const auto status = amdgpu::smu_collect_metrics(state.bringup.device,
                                                       state.bringup.metrics, ready);
        arguments->scalarOutput[0] = static_cast<uint32_t>(status);
        arguments->scalarOutput[1] = state.bringup.metrics.snapshot.sequence;
        arguments->scalarOutput[2] = state.bringup.metrics.snapshot.validFields;
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodMetricsSnapshot: {
        if (arguments->scalarInputCount != 0 || arguments->scalarOutputCount != 0 ||
            arguments->structureInput || arguments->structureInputDescriptor ||
            arguments->structureOutputDescriptor ||
            arguments->structureOutputMaximumSize < sizeof(amdgpu::SMUMetricsSnapshot))
            return kIOReturnBadArgument;
        auto &state = *driver->ivars;
        const bool ready = state.pciOpen && !state.stopping &&
            !state.shutdownBlocked && !state.shutdownInProgress &&
            state.bringup.reached == amdgpu::BringupStage::SDMAInit;
        amdgpu::SMUMetricsSnapshot snapshot{};
        amdgpu::smu_metrics_snapshot(state.bringup.metrics, ready, snapshot);
        // IOUserClient owns and releases the created OSData output object.
        arguments->structureOutput = OSData::withBytes(&snapshot, sizeof(snapshot));
        return arguments->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    case kMacAMDGPUMethodRuntimeBuild: {
        if (arguments->scalarInputCount != 0 || arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) return kIOReturnBadArgument;
        arguments->scalarOutput[0] = 0x414D444750554142ull; // AMDGPUAB
        arguments->scalarOutput[1] = 1; // runtime identity ABI
        arguments->scalarOutput[2] = MACAMDGPU_BUILD_VERSION;
        arguments->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

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

    case kMacAMDGPUMethodGetReBARInfo: {
        // in[0] = BAR index; out = capability offset, raw capability,
        // raw control, supported-size mask, selected bytes, OS-assigned bytes.
        // Do not write BAR/ReBAR registers: resizing also needs the host
        // PCI allocator to update bridge windows and memory descriptors.
        if (!arguments->scalarInput || arguments->scalarInputCount != 1 ||
            arguments->scalarInput[0] >= 6 || !arguments->scalarOutput ||
            arguments->scalarOutputCount < 6)
            return kIOReturnBadArgument;
        kern_return_t ret = mac_amdgpu_ensure_open(this, driver, pci);
        if (ret != kIOReturnSuccess) return ret;
        uint64_t offset = 0;
        // Apple's extended-capability constant already contains -0x15.
        ret = pci->FindPCICapability(
            uint32_t(kIOPCIExpressCapabilityIDResizableBAR), 0, &offset);
        if (ret != kIOReturnSuccess) return ret;
        if (offset == 0) return kIOReturnNotFound;
        amdgpu::ReBARInfo info{};
        auto read = [&](uint64_t address, uint32_t &value) {
            value = 0xffffffff;
            pci->ConfigurationRead32(address, &value);
            return value != 0xffffffff; // SDK reports failed reads as all ones
        };
        auto result = amdgpu::read_rebar(offset,
            uint32_t(arguments->scalarInput[0]), read, info);
        switch (result) {
        case amdgpu::ReBARResult::ReadError: return kIOReturnIOError;
        case amdgpu::ReBARResult::Malformed: return kIOReturnBadMedia;
        case amdgpu::ReBARResult::NotFound: return kIOReturnNotFound;
        case amdgpu::ReBARResult::UnsupportedVersion: return kIOReturnUnsupported;
        case amdgpu::ReBARResult::Found: break;
        }
        uint8_t memoryIndex = 0, barType = 0;
        uint64_t assignedBytes = 0;
        ret = pci->GetBARInfo(uint8_t(arguments->scalarInput[0]),
                             &memoryIndex, &assignedBytes, &barType);
        if (ret != kIOReturnSuccess) return ret;
        arguments->scalarOutput[0] = offset;
        arguments->scalarOutput[1] = info.capability;
        arguments->scalarOutput[2] = info.control;
        arguments->scalarOutput[3] = info.supportedSizes;
        arguments->scalarOutput[4] = info.selectedBytes;
        arguments->scalarOutput[5] = assignedBytes;
        arguments->scalarOutputCount = 6;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodGetDiagnostics: {
        // Returns: cfg (cmd/status, BAR0..5 low+high), PM cap, BAR0/2/5
        // MMIO probes so the host can see exactly what each BAR reads.
        MACAMDGPU_LOG("GetDiagnostics entry: scalarInput=%p inCount=%u "
                      "scalarOutput=%p outCount=%u",
                      arguments->scalarInput,
                      (unsigned)arguments->scalarInputCount,
                      arguments->scalarOutput,
                      (unsigned)arguments->scalarOutputCount);
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 16) {
            MACAMDGPU_LOG("GetDiagnostics: rejecting — outScalar=%p "
                          "outCount=%u (need >=16)",
                          arguments->scalarOutput,
                          (unsigned)arguments->scalarOutputCount);
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
        arguments->scalarOutput[8]  = bdev.bar2Size;
        // BAR5 is the MMIO register window on Bonaire+ AMDGPUs.
        // BAR0 is the framebuffer aperture (returns 0 pre-VRAM-setup).
        arguments->scalarOutput[9]  = rdBar(bdev.bar5MemIndex, 0x0000);
        arguments->scalarOutput[10] = rdBar(bdev.bar5MemIndex, 0x0004);
        arguments->scalarOutput[11] = rdBar(bdev.bar5MemIndex, 0x0DE3 * 4);
        arguments->scalarOutput[12] = rdBar(bdev.bar0MemIndex, 0x0000);
        arguments->scalarOutput[13] = rdBar(bdev.bar0MemIndex, 0x0004);
        // MP0_C2PMSG_33 (IFWI status) — read from BAR5 (the register
        // window), using the same absolute alias as discovery.
        arguments->scalarOutput[14] =
            rdBar(bdev.bar5MemIndex,
                  (uint64_t)amdgpu::BootstrapRegs::MP0_C2PMSG_33 * 4ULL);
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
        //   [5] C2PMSG_81 (sOS sign-of-life, nonzero when alive)
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

    case kMacAMDGPUMethodLiveStatus: {
        // v0.1.24 — runtime engine health snapshot. Lets userspace
        // verify "is the dext + GPU still alive" post-bringup without
        // re-running stages. Reads live engine status regs through
        // SOC15 + asks SMU which DPM features are currently running.
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 12) {
            return kIOReturnBadArgument;
        }
        auto &bdev = driver->ivars->bringup.device;
        // Defaults: 0 means "unreadable".
        for (uint32_t i = 0; i < 12; i++) arguments->scalarOutput[i] = 0;
        if (bdev.ip.isResolved(amdgpu::IPBlock::GC)) {
            arguments->scalarOutput[0] = amdgpu::RREG32(bdev,
                SOC15_REG_OFFSET_BIDX(bdev, amdgpu::IPBlock::GC, 0,
                                      0x0DA4));               // regGRBM_STATUS
            arguments->scalarOutput[1] = amdgpu::RREG32(bdev,
                SOC15_REG_OFFSET_BIDX(bdev, amdgpu::IPBlock::GC, 0,
                                      amdgpu::GCRegs::CP_STAT));
            arguments->scalarOutput[2] = amdgpu::RREG32(bdev,
                SOC15_REG_OFFSET_BIDX(bdev, amdgpu::IPBlock::GC, 1,
                                      amdgpu::GCRegs::RLC_RLCS_BOOTLOAD_STATUS));
            arguments->scalarOutput[3] = amdgpu::RREG32(bdev,
                amdgpu::sdma_reg_offset(bdev, 0, amdgpu::sdma_regs(bdev).STATUS_REG));
            arguments->scalarOutput[4] = amdgpu::RREG32(bdev,
                amdgpu::sdma_reg_offset(bdev, 1, amdgpu::sdma_regs(bdev).STATUS_REG));
            // v0.1.32 — SDMA0 RPTR/WPTR + MCU_CNTL so we can SEE whether
            // doorbell delivery is updating the engine's wptr (if rptr
            // is stuck at 0 after a submit, the doorbell isn't landing).
            arguments->scalarOutput[8] = amdgpu::RREG32(bdev,
                amdgpu::sdma_reg_offset(bdev, 0, amdgpu::sdma_regs(bdev).QUEUE0_RB_RPTR));
            arguments->scalarOutput[9] = amdgpu::RREG32(bdev,
                amdgpu::sdma_reg_offset(bdev, 0, amdgpu::sdma_regs(bdev).QUEUE0_RB_WPTR));
            arguments->scalarOutput[10] = amdgpu::RREG32(bdev,
                amdgpu::sdma_reg_offset(bdev, 0, amdgpu::sdma_regs(bdev).QUEUE0_RB_CNTL));
            arguments->scalarOutput[11] = amdgpu::RREG32(bdev,
                amdgpu::sdma_reg_offset(bdev, 0, amdgpu::sdma_regs(bdev).MCU_CNTL));
        }
        if (bdev.ip.isResolved(amdgpu::IPBlock::MP1, /*baseIdx=*/1) &&
            bdev.smuOnline) {
            uint32_t lo = 0, hi = 0;
            (void)amdgpu::smu_send_msg_with_param(bdev,
                amdgpu::PPSMC::GetRunningSmuFeaturesLow, 0, &lo);
            (void)amdgpu::smu_send_msg_with_param(bdev,
                amdgpu::PPSMC::GetRunningSmuFeaturesHigh, 0, &hi);
            arguments->scalarOutput[5] = lo;
            arguments->scalarOutput[6] = hi;
        }
        arguments->scalarOutput[7] = driver->ivars->bringup.reached
                                     == amdgpu::BringupStage::SDMAInit ? 1 : 0;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodDisableSmuFeatures: {
        // v0.1.24 — turn DPM off via PMFW. Without a chip-specific
        // fan curve (smu_set_default_dpm_table), PMFW defaults fan to
        // MAX when DPM is enabled. Sending DisableAllSmuFeatures parks
        // it and the fan drops to idle. Reverse by re-running
        // Initialize GPU (smc_hw_setup re-sends EnableAllSmuFeatures).
        auto &bdev = driver->ivars->bringup.device;
        if (!bdev.smuOnline) return kIOReturnNotReady;
        if (arguments->scalarOutput != nullptr &&
            arguments->scalarOutputCount >= 1) {
            arguments->scalarOutput[0] = 0;
        }
        kern_return_t r = amdgpu::smu_send_msg(bdev,
            amdgpu::PPSMC::DisableAllSmuFeatures);
        if (arguments->scalarOutput != nullptr &&
            arguments->scalarOutputCount >= 1) {
            arguments->scalarOutput[0] = static_cast<uint64_t>(r);
        }
        MACAMDGPU_LOG("DisableSmuFeatures: kr=%#x", r);
        return r;
    }

    case kMacAMDGPUMethodSetPowerState: {
        // v0.1.29 — per-state GFXCLK soft-clamp via PMFW.
        //
        // Maps a coarse "power state" (auto / low / nominal / high / peak)
        // to a pair of SetSoftMin/MaxByFreq PMFW messages on PPCLK_GFXCLK
        // (clk_id=0 on v14). Encoding per upstream
        // smu_v14_0_set_soft_freq_limited_range (smu_v14_0.c:1099):
        //   param = (clk_id << 16) | freq_mhz
        // with clk_id=0 == PPCLK_GFXCLK.
        //
        // scalarOutput[0] = kIOReturn of the first failing message,
        // 0 on full success.
        if (arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        auto &bdev = driver->ivars->bringup.device;
        if (!bdev.smuOnline) return kIOReturnNotReady;

        const uint64_t state = arguments->scalarInput[0];
        if (arguments->scalarOutput != nullptr &&
            arguments->scalarOutputCount >= 1) {
            arguments->scalarOutput[0] = 0;
        }

        // Build (min_mhz, max_mhz). Zero means "skip that side".
        // 0xFFFF means "PMFW pick" (passed through unchanged into the
        // low 16 bits of the param).
        uint32_t min_mhz = 0, max_mhz = 0;
        bool valid = true;
        switch (state) {
        case kMacAMDGPUPowerStateAuto:
            min_mhz = 0;       // SetSoftMin(0)   → unclamp lower bound
            max_mhz = 0xFFFFu; // SetSoftMax(FFFF)→ PMFW pick
            break;
        case kMacAMDGPUPowerStateLow:
            min_mhz = 0;
            max_mhz = 200;
            break;
        case kMacAMDGPUPowerStateNominal:
            // Same as auto.
            min_mhz = 0;
            max_mhz = 0xFFFFu;
            break;
        case kMacAMDGPUPowerStateHigh:
            min_mhz = 1500;
            max_mhz = 0;       // leave max alone
            break;
        case kMacAMDGPUPowerStatePeak:
            min_mhz = 2400;
            max_mhz = 2400;
            break;
        default:
            valid = false;
            break;
        }
        if (!valid) {
            MACAMDGPU_LOG("SetPowerState: bad state=%llu", state);
            return kIOReturnBadArgument;
        }

        const uint32_t clk_id = amdgpu::PPCLK::GFXCLK; // 0
        kern_return_t firstErr = kIOReturnSuccess;

        // SetSoftMaxByFreq first (upstream order).
        if (max_mhz != 0) {
            uint32_t param = (clk_id << 16) | (max_mhz & 0xFFFFu);
            kern_return_t r = amdgpu::smu_send_msg_with_param(
                bdev, amdgpu::PPSMC::SetSoftMaxByFreq, param, nullptr);
            MACAMDGPU_LOG("SetPowerState: SetSoftMaxByFreq(GFXCLK,%u) kr=%#x",
                          max_mhz, r);
            if (r != kIOReturnSuccess && firstErr == kIOReturnSuccess) {
                firstErr = r;
            }
        }
        // Then SetSoftMinByFreq.
        if (min_mhz != 0 ||
            state == kMacAMDGPUPowerStateAuto ||
            state == kMacAMDGPUPowerStateNominal) {
            uint32_t param = (clk_id << 16) | (min_mhz & 0xFFFFu);
            kern_return_t r = amdgpu::smu_send_msg_with_param(
                bdev, amdgpu::PPSMC::SetSoftMinByFreq, param, nullptr);
            MACAMDGPU_LOG("SetPowerState: SetSoftMinByFreq(GFXCLK,%u) kr=%#x",
                          min_mhz, r);
            if (r != kIOReturnSuccess && firstErr == kIOReturnSuccess) {
                firstErr = r;
            }
        }

        if (arguments->scalarOutput != nullptr &&
            arguments->scalarOutputCount >= 1) {
            arguments->scalarOutput[0] =
                static_cast<uint64_t>(firstErr);
        }
        MACAMDGPU_LOG("SetPowerState: state=%llu min=%u max=%u kr=%#x",
                      state, min_mhz, max_mhz, firstErr);
        return firstErr;
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

        if (ivars->pendingInterruptNotify != nullptr) return kIOReturnBusy;

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
        if (driver->ivars->bringup.reached != amdgpu::BringupStage::None)
            return kIOReturnBusy;
        mac_amdgpu_release_dma_buffer(this);
        return kIOReturnSuccess;

    case kMacAMDGPUMethodResetDevice:
        return mac_amdgpu_reset_device(this);

    case kMacAMDGPUMethodShutdownGPU: {
        if (arguments->scalarInputCount != 0 || arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 2) return kIOReturnBadArgument;
        uint64_t phase = 0;
        const kern_return_t ret = mac_amdgpu_shutdown_gpu(this, phase);
        // Transport succeeds so the failure phase reaches the host even when
        // the operation itself failed. [status, phase], phase 6 means complete.
        arguments->scalarOutput[0] = (uint32_t)ret;
        arguments->scalarOutput[1] = phase;
        return kIOReturnSuccess;
    }

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
        auto &bringup = driver->ivars->bringup;
        const auto previousPSPFence = bringup.psp.fenceCounter;
        const auto previousCPWptr = bringup.cp.wptr;
        uint32_t previousSDMAWptr[amdgpu::kSDMAInstanceCount] = {};
        for (uint32_t i = 0; i < amdgpu::kSDMAInstanceCount; ++i)
            previousSDMAWptr[i] = bringup.sdma.instance[i].wptr;
        kern_return_t ret = amdgpu::bringup_to(bringup, stage);
        bool submitted = bringup.psp.fenceCounter != previousPSPFence ||
                         bringup.cp.wptr != previousCPWptr;
        for (uint32_t i = 0; i < amdgpu::kSDMAInstanceCount; ++i)
            submitted |= bringup.sdma.instance[i].wptr != previousSDMAWptr[i];
        if (ret != kIOReturnSuccess && (ret == kIOReturnTimeout || submitted)) {
            driver->ivars->shutdownBlocked = true;
            MACAMDGPU_LOG("initialization failed after possible submission; Stop GPU required before retry");
        }
        arguments->scalarOutput[0] =
            (uint64_t)driver->ivars->bringup.reached;
        return ret;
    }

    case kMacAMDGPUMethodLoadFirmware: {
        const uint32_t previousFence = driver->ivars->bringup.psp.fenceCounter;
        const kern_return_t firmwareResult = [&]() -> kern_return_t {

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
        psp.firmwareLoadComplete = false;

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
        case kMacAMDGPUFwTypeTA: {
            // Parse psp_<chip>_ta.bin, stage the ASD ucode into a
            // dedicated fwBuf VRAM slot. psp.asd will hold the staged
            // MC address + size; psp_asd_initialize uses those after
            // AUTOLOAD_RLC ack-s. Upstream order — amdgpu_psp.c:3153:
            //     psp_load_non_psp_fw → psp_asd_initialize → psp_rl_load
            // Skipping ASD leaves PSP in a partial state where the GC
            // autoload state machine doesn't fire (v0.1.18 symptom).
            auto &br = driver->ivars->bringup;
            if (br.reached < amdgpu::BringupStage::PSPInit) {
                kern_return_t pir = amdgpu::bringup_to(br,
                    amdgpu::BringupStage::PSPInit);
                if (pir != kIOReturnSuccess) return pir;
            }
            if (psp.fwBufSize == 0) {
                MACAMDGPU_LOG("LoadFirmware(TA): fwBuf not initialized");
                return kIOReturnNotReady;
            }
            kern_return_t r = amdgpu::psp_parse_ta_microcode(
                dev, psp, bin, fwSize);
            if (r != kIOReturnSuccess) {
                MACAMDGPU_LOG("LoadFirmware(TA): parse failed kr=%#x", r);
                return r;
            }
            if (psp.asd.parsed) {
                MACAMDGPU_LOG("LoadFirmware(TA): ASD staged size=%u "
                              "mc=%#llx fw_version=%#x",
                              psp.asd.size_bytes, psp.asd.ucode_mc_addr,
                              psp.asd.fw_version);
            } else {
                MACAMDGPU_LOG("LoadFirmware(TA): no ASD sub-bin found "
                              "(parser returned success) — proceeding "
                              "without ASD");
            }
            return kIOReturnSuccess;
        }
        case kMacAMDGPUFwTypeFile_TOC: {
            // gc_<v>_toc.bin — required BEFORE any IP firmware load on
            // autoload-supported chips (psp_v14_0_3 / R9700). PSP parses
            // the TOC to compute TMR layout; without this it rejects
            // SDMA/CP/MES LOAD_IP_FW with TEE_BAD_PARAMETERS (0xFFFF0006).
            // Mirrors upstream psp_load_toc (amdgpu_psp.c:840) called
            // from psp_tmr_init.
            uint32_t tmr_size = 0;
            uint32_t toc_resp = 0;
            kern_return_t r = amdgpu::psp_load_toc(
                dev, psp, bin, static_cast<uint32_t>(fwSize),
                &tmr_size, &toc_resp);
            if (r == kIOReturnSuccess) {
                MACAMDGPU_LOG("LoadFirmware(TOC): PSP needs tmr_size=%u",
                              tmr_size);
            } else {
                MACAMDGPU_LOG("LoadFirmware(TOC): PSP rejected — "
                              "kr=%#x resp_status=%#x (file=%llu B)",
                              r, toc_resp, fwSize);
            }
            return r;
        }
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
                auto &psp = driver->ivars->bringup.psp;
                if (psp.fwPriBusAddr == 0 || psp.fwPriSize == 0) {
                    return kIOReturnNotReady;
                }
                // fw_buf VRAM-backed (psp_setup_fw_buf_sysmem ported but
                // dormant): GART-bound sysmem path landed every
                // LOAD_IP_FW into resp=0x11 even after PerformOperation
                // cache flush + maxAddressBits=32 + MTYPE@bit54 fix —
                // PSP can't read this Mac's DART-mapped IOVA via the
                // PT walk for unknown reasons. VRAM path unblocks
                // SMU/IMU; SDMA/CP/MES/RLC stay failing pending a
                // different attack.

                // PSP transfers RS64 microcode but Linux's later hw_init must
                // still configure its entry PCs and release the pipe resets.
                int cpFirmwareIndex = -1;
                switch (fwType) {
                case kMacAMDGPUFwTypeFile_CP_PFP: cpFirmwareIndex = 0; break;
                case kMacAMDGPUFwTypeFile_CP_ME:  cpFirmwareIndex = 1; break;
                case kMacAMDGPUFwTypeFile_CP_MEC: cpFirmwareIndex = 2; break;
                default: break;
                }
                uint64_t cpEntryAddress = 0;
                if (cpFirmwareIndex >= 0) {
                    auto &cp = driver->ivars->bringup.cp;
                    if (cp.firmwarePrepared || cp.ringReady) return kIOReturnBusy;
                    cp.firmware[cpFirmwareIndex] = {};
                    if (!amdgpu::cp_parse_firmware_start(bin, fwSize, cpEntryAddress))
                        return kIOReturnBadArgument;
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

                // Side-effect bookkeeping that used to live in the
                // single-payload path. Captures MES start addresses
                // from the MES file header (independent of any
                // particular payload), and flips sdma microcode_loaded
                // when the SDMA payload submits successfully.
                bool sdma_loaded_this_call = false;

                uint64_t mesEntryAddress = 0;
                uint32_t mesPayloadMask = 0;
                const bool isMESPackage = fwType == kMacAMDGPUFwTypeFile_MES_UNI ||
                    fwType == 0x100ULL + amdgpu::PSPGfxFwType::CP_MES;
                if (isMESPackage) {
                    auto &mes = driver->ivars->bringup.mes;
                    mes.sched_ucode_loaded = mes.kiq_ucode_loaded = false;
                    if (fwSize < sizeof(amdgpu::mes_firmware_header_v1_0))
                        return kIOReturnBadArgument;
                    const auto *hdr = reinterpret_cast<const amdgpu::mes_firmware_header_v1_0 *>(bin);
                    mesEntryAddress = uint64_t(hdr->mes_uc_start_addr_lo) |
                        (uint64_t(hdr->mes_uc_start_addr_hi) << 32);
                    if (!mesEntryAddress || (mesEntryAddress & 3)) return kIOReturnBadArgument;
                }

                // Submit each payload as its own LOAD_IP_FW frame. Stage
                // bytes into a unique slot inside psp.fwBuf (VRAM) via
                // BAR0; address handed to PSP is `fwBufBaseMC + slot_off`
                // — a VRAM MC address the FB aperture resolves directly.
                // (psp_setup_fw_buf_sysmem ports the upstream GART path
                // but PSP rejects sysmem fw_phy_addr on this Mac for
                // reasons we haven't pinned down; VRAM unblocks SMU/IMU.)
                MACAMDGPU_LOG("LoadFirmware(fwType=%#llx): extractor produced "
                              "%u payload(s)", fwType, count);
                constexpr uint64_t kFwBufAlign = 0x1000; // PAGE_SIZE
                for (uint32_t i = 0; i < count; i++) {
                    if ((uint64_t)payloads[i].offset_bytes +
                            payloads[i].size_bytes > fwSize) {
                        return kIOReturnBadArgument;
                    }
                    uint64_t slot_off = psp.fwBufBumpOffset;
                    uint64_t slot_sz  = (payloads[i].size_bytes +
                                         kFwBufAlign - 1) & ~(kFwBufAlign - 1);
                    if (slot_off + slot_sz > psp.fwBufSize) {
                        MACAMDGPU_LOG("LoadFirmware: fw_buf exhausted "
                                      "(want %llu @ %llu, cap %llu)",
                                      slot_sz, slot_off, psp.fwBufSize);
                        return kIOReturnNoSpace;
                    }
                    uint64_t slot_vram_off = psp.fwBufVRAMOffset + slot_off;
                    uint64_t fwBusAddr     = psp.fwBufBaseMC      + slot_off;
                    // If psp_setup_fw_buf_sysmem succeeded earlier in
                    // PSPInit, fwBufSysmemCPU != nullptr and fwBufBaseMC
                    // points at the GART MC address of the sysmem
                    // staging buffer. Match upstream amdgpu_ucode_init_bo
                    // by memcpy'ing directly into the CPU-mapped sysmem
                    // buffer instead of streaming via BAR0 to VRAM.
                    // The buffer is DART-pinned + GART-bound so PSP
                    // can read it through the GMC walk.
                    if (psp.fwBufSysmemCPU != nullptr) {
                        memcpy(
                            static_cast<uint8_t *>(psp.fwBufSysmemCPU) +
                                slot_off,
                            bin + payloads[i].offset_bytes,
                            payloads[i].size_bytes);
                    } else {
                        amdgpu::bar0_memcpy_to_vram(
                            dev, slot_vram_off,
                            bin + payloads[i].offset_bytes,
                            payloads[i].size_bytes);
                        amdgpu::amdgpu_hdp_flush(dev);
                    }
                    psp.fwBufBumpOffset = slot_off + slot_sz;
                    MACAMDGPU_LOG("  payload[%u]: fw_type=%u src_off=%u size=%u "
                                  "→ vram_off=%#llx mc=%#llx",
                                  i, payloads[i].fw_type,
                                  payloads[i].offset_bytes,
                                  payloads[i].size_bytes,
                                  slot_vram_off, fwBusAddr);

                    // Read-back diagnostic for the first payload of
                    // each .bin: compare source dwords vs what the
                    // GPU sees at the same VRAM offset via the
                    // MM_INDEX/MM_DATA register pair (which reads
                    // through GMC — the same path PSP uses for
                    // LOAD_IP_FW.fw_phy_addr). If src == gpu_read,
                    // PSP's rejection is protocol-level (not a data
                    // path bug). If they differ, the data isn't
                    // reaching the GPU's view of memory.
                    if (i == 0) {
                        const uint32_t *s =
                            reinterpret_cast<const uint32_t *>(
                                bin + payloads[i].offset_bytes);
                        uint32_t g0 = amdgpu::RVRAM32_via_mm(dev, slot_vram_off + 0);
                        uint32_t g1 = amdgpu::RVRAM32_via_mm(dev, slot_vram_off + 4);
                        uint32_t g2 = amdgpu::RVRAM32_via_mm(dev, slot_vram_off + 8);
                        uint32_t g3 = amdgpu::RVRAM32_via_mm(dev, slot_vram_off + 12);
                        MACAMDGPU_LOG("  readback fw_type=%u @ vram_off=%#llx: "
                                      "src=%08x %08x %08x %08x  "
                                      "gpu=%08x %08x %08x %08x",
                                      payloads[i].fw_type, slot_vram_off,
                                      s[0], s[1], s[2], s[3],
                                      g0, g1, g2, g3);
                    }
                    kern_return_t r = amdgpu::psp_load_ip_fw(
                        dev, psp, fwBusAddr,
                        payloads[i].size_bytes, payloads[i].fw_type);
                    if (r != kIOReturnSuccess) {
                        MACAMDGPU_LOG("LoadFirmware(fwType=%#llx) payload[%u] "
                                      "(fw_type=%u) FAILED kr=%#x",
                                      fwType, i, payloads[i].fw_type, r);
                        return r;
                    }
                    if (isMESPackage) {
                        switch (payloads[i].fw_type) {
                        case amdgpu::PSPGfxFwType::CP_MES: mesPayloadMask |= 1; break;
                        case amdgpu::PSPGfxFwType::CP_MES_DATA: mesPayloadMask |= 2; break;
                        case amdgpu::PSPGfxFwType::CP_MES_KIQ: mesPayloadMask |= 4; break;
                        case amdgpu::PSPGfxFwType::MES_KIQ_STACK: mesPayloadMask |= 8; break;
                        }
                    }
                    if (payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA0 ||
                        payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA1 ||
                        payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA_UCODE_TH0) {
                        sdma_loaded_this_call = true;
                    }
                    if (payloads[i].fw_type == amdgpu::PSPGfxFwType::IMU_D) {
                        driver->ivars->bringup.imu.microcode_loaded = true;
                    }
                    // RLC_G is LAST per upstream enum order; trigger
                    // AUTOLOAD_RLC immediately after it acks. (IFWI-only
                    // fallback was tested in 0.0.91: AUTOLOAD_RLC without
                    // sub-bin loads returned TEE_ERROR_ITEM_NOT_FOUND
                    // = 0xFFFF0007 — proves SOS does NOT have firmware
                    // pre-loaded from IFWI. LOAD_IP_FW must succeed.)
                    if (payloads[i].fw_type == amdgpu::PSPGfxFwType::RLC_G) {
                        kern_return_t a = amdgpu::psp_rlc_autoload_start(
                            dev, psp);
                        if (a != kIOReturnSuccess) {
                            MACAMDGPU_LOG("rlc_autoload_start FAILED kr=%#x", a);
                            return a;
                        }
                        driver->ivars->bringup.rlc.microcode_loaded = false;

                        // Upstream amdgpu_psp.c:3153 — psp_asd_initialize
                        // runs BETWEEN psp_load_non_psp_fw (which ends
                        // with AUTOLOAD_RLC) and psp_rl_load. Hypothesis
                        // (v0.1.19): loading the first TA transitions
                        // PSP from "loading mode" into "system ready",
                        // and only then does the GC autoload state
                        // machine actually fire. v0.1.18 skipped this
                        // call and BOOTLOAD_STATUS stayed at 0 forever.
                        //
                        // psp_asd_initialize is a no-op (returns success)
                        // if the host didn't send the TA bin, so this
                        // is safe to add unconditionally.
                        kern_return_t asd =
                            amdgpu::psp_asd_initialize(dev, psp);
                        if (asd != kIOReturnSuccess) {
                            MACAMDGPU_LOG("psp_asd_initialize FAILED kr=%#x "
                                          "(resp=%#x) — autoload may stay "
                                          "stuck",
                                          asd, psp.asd.resp_status);
                            return asd;
                        }

                        // Upstream amdgpu_psp.c:3159 follows
                        // psp_asd_initialize with psp_rl_load. PSP's
                        // GC autoload state machine may also wait for
                        // REG_LIST (fw_type=67). psp.rl is populated
                        // from the v2 SOS package by
                        // psp_parse_sos_microcode. psp_rl_load is
                        // required when present. Keep its SOS package
                        // alive through all intervening firmware uploads.
                        kern_return_t rl =
                            amdgpu::psp_rl_load(dev, psp);
                        if (rl != kIOReturnSuccess) {
                            MACAMDGPU_LOG("psp_rl_load FAILED kr=%#x — firmware initialization stopped", rl);
                            return rl;
                        }
                        driver->ivars->bringup.rlc.microcode_loaded = true;
                        psp.firmwareLoadComplete = true;
                    }
                }
                if (isMESPackage) {
                    if (mesPayloadMask != 0xf) return kIOReturnNotReady;
                    auto &mes = driver->ivars->bringup.mes;
                    amdgpu::mes_set_uc_start_addr(mes, amdgpu::MESPipe::Sched, mesEntryAddress);
                    amdgpu::mes_set_uc_start_addr(mes, amdgpu::MESPipe::KIQ, mesEntryAddress);
                }
                if (cpFirmwareIndex >= 0) {
                    driver->ivars->bringup.cp.firmware[cpFirmwareIndex] =
                        {cpEntryAddress, true};
                    MACAMDGPU_LOG("CP RS64 firmware %d acknowledged; entry=%#llx",
                                  cpFirmwareIndex, cpEntryAddress);
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

        }();
        if (firmwareResult != kIOReturnSuccess &&
            (firmwareResult == kIOReturnTimeout ||
             driver->ivars->bringup.psp.fenceCounter != previousFence)) {
            driver->ivars->shutdownBlocked = true;
            MACAMDGPU_LOG("firmware command failed after possible submission; Stop GPU required before retry");
        }
        return firmwareResult;
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
        timeout_us = amdgpu::client_wait_ns(timeout_us, true) / 1000;
        const auto previousWptr = driver->ivars->bringup.cp.wptr;
        uint32_t fence = 0;
        kern_return_t r = amdgpu::cp_submit_eop_test(
            driver->ivars->bringup.device,
            driver->ivars->bringup.cp,
            timeout_us, &fence);
        if (r != kIOReturnSuccess && driver->ivars->bringup.cp.wptr != previousWptr)
            driver->ivars->shutdownBlocked = true;
        arguments->scalarOutput[0] = fence;
        return r;
    }

    case kMacAMDGPUMethodCPKIQSmoke: {
        // v0.1.26 — first PM4 packet on KIQ. Builds NOP +
        // RELEASE_MEM(fence=0xDEADBEEF) targeting a VRAM-resident
        // fence slot, kicks the doorbell, polls the slot.
        //
        // Out scalars:
        //   [0] kIOReturn
        //   [1] elapsed_us
        //   [2] expected (0xDEADBEEF)
        //   [3] observed_fence
        //   [4] fence_gpu_va lo32
        //   [5] fence_gpu_va hi32
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 6) {
            return kIOReturnBadArgument;
        }
        if (!driver->ivars->pciOpen) return kIOReturnNotOpen;

        const uint32_t expected   = 0xDEADBEEFu;
        const uint32_t timeout_us = 100000;  // 100 ms
        uint64_t elapsed_us       = 0;
        uint64_t fence_gpu_va     = 0;
        uint32_t observed         = 0;

        const auto previousWptr = driver->ivars->bringup.cp.wptr;
        kern_return_t r = amdgpu::cp_kiq_smoke_test(
            driver->ivars->bringup.device,
            driver->ivars->bringup.cp,
            driver->ivars->bringup.mes,
            driver->ivars->bringup.gmc,
            expected, timeout_us,
            &elapsed_us, &fence_gpu_va, &observed);
        if (r != kIOReturnSuccess && driver->ivars->bringup.cp.wptr != previousWptr)
            driver->ivars->shutdownBlocked = true;

        arguments->scalarOutput[0] = static_cast<uint64_t>(r);
        arguments->scalarOutput[1] = elapsed_us;
        arguments->scalarOutput[2] = expected;
        arguments->scalarOutput[3] = observed;
        arguments->scalarOutput[4] = fence_gpu_va & 0xFFFFFFFFull;
        arguments->scalarOutput[5] = (fence_gpu_va >> 32) & 0xFFFFFFFFull;
        // Preserve diagnostics on timeout: failed RPCs may discard scalars.
        // The operation status is carried in output[0], as for SDMACopyVRAM.
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodHostMemoryTest: {
        if (!arguments->scalarInput || arguments->scalarInputCount < 1 ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 6)
            return kIOReturnBadArgument;
        if (!driver->ivars->pciOpen) return kIOReturnNotOpen;
        auto &b = driver->ivars->bringup;
        amdgpu::MemoryTransferResult result{};
        const auto r = amdgpu::memory_transfer_test(b.device, b.gmc, b.gart,
            b.sdma.instance[0], b.memoryTest,
            static_cast<uint32_t>(arguments->scalarInput[0]), result);
        if (r != kIOReturnSuccess && b.memoryTest.active)
            driver->ivars->shutdownBlocked = true;
        arguments->scalarOutput[0] = static_cast<uint32_t>(r);
        arguments->scalarOutput[1] = result.stage;
        arguments->scalarOutput[2] = result.mismatches;
        arguments->scalarOutput[3] = result.firstMismatch;
        arguments->scalarOutput[4] = result.hostGPUAddress;
        arguments->scalarOutput[5] = result.vramGPUAddress;
        MACAMDGPU_LOG("host-memory test: status=%#x stage=%u mismatches=%u first=%#x host_gpu=%#llx vram_gpu=%#llx",
            r, result.stage, result.mismatches, result.firstMismatch,
            result.hostGPUAddress, result.vramGPUAddress);
        return kIOReturnSuccess; // preserve diagnostics even when operation failed
    }

    case kMacAMDGPUMethodComputeTest: {
        if (!arguments->scalarInput || arguments->scalarInputCount != 1 ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 6)
            return kIOReturnBadArgument;
        if (!driver->ivars->pciOpen) return kIOReturnNotOpen;
        auto &b = driver->ivars->bringup;
        amdgpu::ComputeTestResult result{};
        const auto r = amdgpu::compute_test(b.device, b.gmc, b.cp, b.gfx,
            b.computeTest, static_cast<uint32_t>(arguments->scalarInput[0]), result);
        if (r != kIOReturnSuccess && result.stage >= 3)
            amdgpu::cp_log_control(b.device, "compute diagnostic failure");
        if (r != kIOReturnSuccess && b.computeTest.active)
            driver->ivars->shutdownBlocked = true;
        arguments->scalarOutput[0] = static_cast<uint32_t>(r);
        arguments->scalarOutput[1] = result.stage;
        arguments->scalarOutput[2] = result.mismatches;
        arguments->scalarOutput[3] = result.firstMismatch;
        arguments->scalarOutput[4] = result.fence;
        arguments->scalarOutput[5] = result.gpuAddress;
        MACAMDGPU_LOG("compute test: status=%#x stage=%u mismatches=%u first=%#x fence=%u gpu=%#llx",
            r, result.stage, result.mismatches, result.firstMismatch, result.fence, result.gpuAddress);
        return kIOReturnSuccess;
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
            count > UINT32_MAX ||
            !amdgpu::client_subrange(src_off, count, ivars->dmaBufferSize) ||
            !amdgpu::client_subrange(dst_off, count, ivars->dmaBufferSize)) {
            return kIOReturnBadArgument;
        }
        // First segment bus base — we already enforce single-segment
        // mappings, so contiguous offsets are valid bus addresses.
        const uint64_t bus_base = ivars->dmaSegments[0].address;
        const auto previousWptr = driver->ivars->bringup.sdma.instance[inst].wptr;
        kern_return_t r = amdgpu::sdma_copy_linear_test(
            driver->ivars->bringup.device,
            driver->ivars->bringup.sdma.instance[inst],
            bus_base + src_off, bus_base + dst_off,
            static_cast<uint32_t>(count),
            amdgpu::client_wait_ns(to_us, true) / 1000);
        if (r != kIOReturnSuccess && driver->ivars->bringup.sdma.instance[inst].wptr != previousWptr)
            driver->ivars->shutdownBlocked = true;
        arguments->scalarOutput[0] = static_cast<uint64_t>(r);
        return r;
    }

    case kMacAMDGPUMethodSDMACopyVRAM: {
        // v0.1.25 — VRAM->VRAM SDMA copy smoke test.
        //
        // GART-bound sysmem is structurally fragile on AS+TB5 (see
        // feedback_mac_amdgpu_dart_tb5_pcie_reads), so this variant
        // allocates both src and dst from the VRAM bump allocator and
        // verifies the result via MM_INDEX/MM_DATA — entirely
        // bus-aperture independent. Proves SDMA can fetch, copy, and
        // write through the GMC walk.
        //
        // Input:
        //   scalarInput[0] = byte count (default 4096 if 0; cap 16 KB)
        //   scalarInput[1] = SDMA instance (0 or 1; default 0)
        // Output (count=7):
        //   [0] kIOReturn from sdma_copy_linear_test (0 = ok)
        //   [1] elapsed_us (0 — no monotonic clock plumbed yet)
        //   [2] mismatched dword count
        //   [3] first mismatched dword byte-offset (0 if all ok)
        //   [4] bytes copied
        //   [5] src.gpu_va & 0xFFFFFFFF
        //   [6] dst.gpu_va & 0xFFFFFFFF
        // Extended reply (when caller offers 12 words):
        //   [7] source readback mismatches before submit, [8] first bad offset
        //   [9] format marker 0x53444d41, [10..11] full source/dest MC addresses
        if (arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 7) {
            return kIOReturnBadArgument;
        }
        if (driver == nullptr || driver->ivars == nullptr) {
            return kIOReturnNotReady;
        }
        auto &bringup = driver->ivars->bringup;
        auto &dev     = bringup.device;
        auto &gmc     = bringup.gmc;
        auto &sdma    = bringup.sdma;
        if (!gmc.vram_alloc.is_inited()) {
            MACAMDGPU_LOG("SDMACopyVRAM: vram_alloc not initialised "
                          "(GMC stage hasn't run)");
            return kIOReturnNotReady;
        }
        uint64_t bytes_in =
            (arguments->scalarInput != nullptr &&
             arguments->scalarInputCount >= 1)
                ? arguments->scalarInput[0]
                : 0;
        uint64_t inst_in  =
            (arguments->scalarInput != nullptr &&
             arguments->scalarInputCount >= 2)
                ? arguments->scalarInput[1]
                : 0;
        if (bytes_in == 0) bytes_in = 4096;
        // Stack buffer cap — keep it modest so we don't blow the dext
        // stack and stay well within the 16 KB AS page.
        constexpr uint64_t kMaxBytes = 16384;
        if (bytes_in > kMaxBytes) bytes_in = kMaxBytes;
        // 4-byte align — SDMA COPY_LINEAR works in bytes but our
        // readback walks dwords.
        bytes_in &= ~uint64_t(3);
        if (bytes_in == 0) return kIOReturnBadArgument;
        if (inst_in >= amdgpu::kSDMAInstanceCount) {
            return kIOReturnBadArgument;
        }
        auto &sdma_inst = sdma.instance[inst_in];
        if (!sdma_inst.inited || !sdma_inst.enabled) {
            MACAMDGPU_LOG("SDMACopyVRAM: SDMA%llu not ready "
                          "(inited=%d enabled=%d)",
                          (unsigned long long)inst_in,
                          (int)sdma_inst.inited,
                          (int)sdma_inst.enabled);
            return kIOReturnNotReady;
        }

        // Allocate two VRAM slots (16 KB-aligned by the bump
        // allocator's AS-page floor). These are not freed — the bump
        // allocator has no free; they persist until dext unload, which
        // is fine for a smoke test that runs once.
        amdgpu::VRAMAllocation src{};
        amdgpu::VRAMAllocation dst{};
        if (!gmc.vram_alloc.alloc(bytes_in, amdgpu::kASPageSize, &src)) {
            MACAMDGPU_LOG("SDMACopyVRAM: VRAM alloc for src failed "
                          "(bytes=%llu)", (unsigned long long)bytes_in);
            return kIOReturnNoSpace;
        }
        if (!gmc.vram_alloc.alloc(bytes_in, amdgpu::kASPageSize, &dst)) {
            MACAMDGPU_LOG("SDMACopyVRAM: VRAM alloc for dst failed "
                          "(bytes=%llu)", (unsigned long long)bytes_in);
            return kIOReturnNoSpace;
        }
        const uint64_t src_vram_off = src.gpu_va - gmc.vram_start;
        const uint64_t dst_vram_off = dst.gpu_va - gmc.vram_start;

        // Build a known pattern: incrementing dwords 0xCAFE0000..n-1.
        uint32_t pattern_buf[kMaxBytes / 4];
        const uint32_t n_dwords = static_cast<uint32_t>(bytes_in / 4);
        for (uint32_t i = 0; i < n_dwords; i++) {
            pattern_buf[i] = 0xCAFE0000u + i;
        }

        // Stage src; pre-poison dst so a no-op would be visible.
        amdgpu::bar0_memcpy_to_vram(dev, src_vram_off,
                                    pattern_buf, bytes_in);
        amdgpu::bar0_memset_vram(dev, dst_vram_off, 0xDEADBEEFu,
                                 bytes_in);
        amdgpu::amdgpu_hdp_flush(dev);

        MACAMDGPU_LOG("SDMACopyVRAM: SDMA%llu  bytes=%llu  "
                      "src.gpu_va=%#llx (vram_off=%#llx)  "
                      "dst.gpu_va=%#llx (vram_off=%#llx)",
                      (unsigned long long)inst_in,
                      (unsigned long long)bytes_in,
                      (unsigned long long)src.gpu_va,
                      (unsigned long long)src_vram_off,
                      (unsigned long long)dst.gpu_va,
                      (unsigned long long)dst_vram_off);

        // Verify the upload through an independent read path before
        // blaming SDMA for a mismatch. Do not submit corrupt source data.
        uint32_t source_mismatched = 0, source_first_bad = 0;
        for (uint32_t i = 0; i < n_dwords; ++i) {
            const uint32_t observed = amdgpu::RVRAM32_via_mm(
                dev, src_vram_off + uint64_t(i) * 4);
            if (observed != pattern_buf[i]) {
                if (source_mismatched == 0) {
                    source_first_bad = i * 4;
                    MACAMDGPU_LOG("SDMACopyVRAM: source upload mismatch at %#x "
                                  "expected=%#x observed=%#x",
                                  source_first_bad, pattern_buf[i], observed);
                }
                ++source_mismatched;
            }
        }
        const uint64_t copy_start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        const auto previousWptr = sdma_inst.wptr;
        kern_return_t r = source_mismatched ? kIOReturnIOError :
            amdgpu::sdma_copy_linear_test(dev, sdma_inst,
                src.gpu_va, dst.gpu_va, static_cast<uint32_t>(bytes_in),
                /*timeout_us=*/100000ull);
        if (r != kIOReturnSuccess && sdma_inst.wptr != previousWptr)
            driver->ivars->shutdownBlocked = true;

        const uint64_t copy_elapsed_us =
            (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - copy_start_ns) / 1000;

        // Readback regardless of fence status — even a partial copy
        // tells us whether the engine touched dst at all.
        uint32_t mismatched = 0;
        uint32_t first_bad_off = 0;
        bool first_bad_set = false;
        for (uint32_t i = 0; i < n_dwords; i++) {
            uint32_t g = amdgpu::RVRAM32_via_mm(
                dev, dst_vram_off + uint64_t(i) * 4);
            if (g != pattern_buf[i]) {
                if (!first_bad_set) {
                    first_bad_off = i * 4;
                    first_bad_set = true;
                }
                mismatched++;
            }
        }
        MACAMDGPU_LOG("SDMACopyVRAM: result kr=%#x mismatched=%u/%u "
                      "first_bad_off=%#x",
                      r, mismatched, n_dwords, first_bad_off);

        arguments->scalarOutput[0] = static_cast<uint64_t>(r);
        arguments->scalarOutput[1] = copy_elapsed_us;
        arguments->scalarOutput[2] = static_cast<uint64_t>(mismatched);
        arguments->scalarOutput[3] = static_cast<uint64_t>(first_bad_off);
        arguments->scalarOutput[4] = bytes_in;
        arguments->scalarOutput[5] = src.gpu_va & 0xFFFFFFFFull;
        arguments->scalarOutput[6] = dst.gpu_va & 0xFFFFFFFFull;
        if (arguments->scalarOutputCount >= 12) {
            arguments->scalarOutput[7] = source_mismatched;
            arguments->scalarOutput[8] = source_first_bad;
            arguments->scalarOutput[9] = 0x53444d41;
            arguments->scalarOutput[10] = src.gpu_va;
            arguments->scalarOutput[11] = dst.gpu_va;
            arguments->scalarOutputCount = 12;
        } else {
            arguments->scalarOutputCount = 7;
        }
        // Always return success at the IOConnect layer; the actual
        // SDMA kr lives in scalarOutput[0] so the host can decode it.
        return kIOReturnSuccess;
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

    case kMacAMDGPUMethodBOCopy: {
        // [src handle, src offset, dst handle, dst offset, bytes]. The owning
        // connection and shared submission gate serialize copies with frees.
        if (!arguments->scalarInput || arguments->scalarInputCount != 5 ||
            !arguments->scalarOutput || arguments->scalarOutputCount < 1 ||
            arguments->structureInput || arguments->structureInputDescriptor ||
            arguments->structureOutputDescriptor || arguments->structureOutputMaximumSize)
            return kIOReturnBadArgument;
        auto *source = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        auto *destination = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[2]);
        if (!source || !destination || !amdgpu::buffer_vram_domain(source->domain) ||
            !amdgpu::buffer_vram_domain(destination->domain)) return kIOReturnBadArgument;
        uint64_t src = 0, dst = 0;
        const auto bytes = arguments->scalarInput[4];
        if (!amdgpu::buffer_copy_ranges(source->gpu_va, source->size, arguments->scalarInput[1],
            destination->gpu_va, destination->size, arguments->scalarInput[3], bytes, src, dst))
            return kIOReturnBadArgument;
        auto &b = driver->ivars->bringup;
        if (b.reached != amdgpu::BringupStage::SDMAInit) return kIOReturnNotReady;
        auto &sdma = b.sdma.instance[0];
        const auto previousWptr = sdma.wptr;
        amdgpu::amdgpu_hdp_flush(b.device);
        const auto status = amdgpu::sdma_copy_linear_test(b.device, sdma, src, dst,
                                                        static_cast<uint32_t>(bytes), 100000);
        // An unsuccessful published copy may still access either allocation.
        // Retain all owner storage and permit only recovery/status operations.
        if (status != kIOReturnSuccess && sdma.wptr != previousWptr)
            driver->ivars->shutdownBlocked = true;
        arguments->scalarOutput[0] = static_cast<uint32_t>(status);
        arguments->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOWrite:
    case kMacAMDGPUMethodBORead: {
        // Bounded, dword-aligned access to an owned CPU-visible staging BO.
        // High VRAM is never passed to a BAR accessor.
        const bool write = selector == kMacAMDGPUMethodBOWrite;
        if (!arguments->scalarInput || arguments->scalarInputCount != 3 ||
            arguments->scalarOutputCount || arguments->structureInputDescriptor ||
            arguments->structureOutputDescriptor) return kIOReturnBadArgument;
        const auto offset = arguments->scalarInput[1], bytes = arguments->scalarInput[2];
        if (!bytes || bytes > amdgpu::kBufferIOChunkBytes || ((offset | bytes) & 3))
            return kIOReturnBadArgument;
        if (write ? (!arguments->structureInput ||
                     arguments->structureInput->getLength() != bytes || arguments->structureOutputMaximumSize)
                  : (arguments->structureInput || arguments->structureOutputMaximumSize < bytes))
            return kIOReturnBadArgument;
        auto *entry = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        if (!entry || entry->domain != kBODomainVRAM ||
            !amdgpu::client_subrange(offset, bytes, entry->size)) return kIOReturnBadArgument;
        auto &b = driver->ivars->bringup;
        uint64_t gpu = 0;
        if (!amdgpu::buffer_gpu_range(entry->gpu_va, entry->size, offset, bytes, gpu) ||
            gpu < b.gmc.vram_start ||
            !amdgpu::vram_io_range(b.device, gpu - b.gmc.vram_start, bytes))
            return kIOReturnBadArgument;
        const auto barOffset = gpu - b.gmc.vram_start;
        if (write) {
            const auto status = amdgpu::vram_write_verified(b.device, barOffset,
                arguments->structureInput->getBytesNoCopy(), bytes);
            if (status == kIOReturnSuccess) amdgpu::amdgpu_hdp_flush(b.device);
            return status;
        }
        uint32_t data[amdgpu::kBufferIOChunkBytes / 4]{};
        for (uint64_t i = 0; i < bytes / 4; ++i)
            pci->MemoryRead32(b.device.bar0MemIndex, barOffset + i * 4, data + i);
        arguments->structureOutput = OSData::withBytes(data, bytes);
        return arguments->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    case kMacAMDGPUMethodBOAlloc: {
        // Dual ABI for back-compat with the pre-v0.1.27 single-input
        // callers (scripts/macamdgpu_ping.swift):
        //
        //   Legacy:  scalarInputCount == 1
        //     in[0] = size in bytes
        //     out[0] = handle, out[1] = bus address (DART) within client
        //              DMA buffer, out[2] = byte offset within DMA buffer
        //     Domain forced to kBODomainGTTLegacy.
        //
        //   v0.1.27: scalarInputCount >= 4
        //     in[0] = size, in[1] = domain (1=VRAM, 2=GTT),
        //     in[2] = alignment, in[3] = flags (reserved, must be 0)
        //     out[0] = handle, out[1] = gpu_va, out[2] = cpu_addr (or 0
        //              if not in-dext-mapped).
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }
        if (ivars == nullptr) return kIOReturnNotReady;

        const bool legacy = (arguments->scalarInputCount < 4);
        uint64_t size      = arguments->scalarInput[0];
        uint32_t domain    = legacy
                              ? (uint32_t)kBODomainGTTLegacy
                              : (uint32_t)arguments->scalarInput[1];
        uint64_t alignment = legacy ? (uint64_t)MACAMDGPU_BO_ALIGN
                                    : arguments->scalarInput[2];
        uint64_t flags     = legacy ? 0ULL : arguments->scalarInput[3];
        if (!legacy && arguments->scalarInput[1] > kBODomainDeviceVRAM)
            return kIOReturnBadArgument;
        if (size == 0) return kIOReturnBadArgument;
        if (flags != 0) return kIOReturnUnsupported;
        uint64_t rounded_size = 0;
        if (!amdgpu::client_allocation_shape(size, alignment, MACAMDGPU_BO_ALIGN,
                                             alignment, rounded_size))
            return kIOReturnBadArgument;

        // Find a free slot in the table.
        uint32_t idx = MACAMDGPU_MAX_BO;
        for (uint32_t i = 0; i < MACAMDGPU_MAX_BO; i++) {
            if (!ivars->bos[i].in_use) { idx = i; break; }
        }
        if (idx == MACAMDGPU_MAX_BO) return kIOReturnNoResources;

        BOEntry &e = ivars->bos[idx];
        // Zero everything except the generation counter (preserved across
        // the freed slot to detect stale handles).
        e.in_use       = true;
        e.domain       = domain;
        e.size         = rounded_size;
        e.alignment    = alignment;
        e.byte_offset  = 0;
        e.vram_offset  = 0;
        e.gpu_va       = 0;
        e.gtt_buf      = nullptr;
        e.gtt_dma      = nullptr;
        e.gttBinding   = {};
        e.gtt_bus_addr = 0;
        e.cpu_addr     = nullptr;
        e.generation   = ++ivars->boGenCounter;

        kern_return_t allocRet = kIOReturnSuccess;

        if (domain == kBODomainGTTLegacy) {
            if (ivars->dmaBuffer == nullptr || ivars->dmaSegmentsCount < 1) {
                allocRet = kIOReturnNotReady;
                goto bo_alloc_fail;
            }
            // Bump-align the legacy cursor.
            if (ivars->boBumpOffset > UINT64_MAX - (alignment - 1)) {
                allocRet = kIOReturnNoSpace;
                goto bo_alloc_fail;
            }
            const uint64_t alignedOffset = (ivars->boBumpOffset + alignment - 1)
                                           & ~(alignment - 1);
            if (!amdgpu::client_subrange(alignedOffset, rounded_size, ivars->dmaBufferSize)) {
                allocRet = kIOReturnNoSpace;
                goto bo_alloc_fail;
            }
            e.byte_offset = alignedOffset;
            ivars->boBumpOffset = alignedOffset + rounded_size;
        }
        else if (amdgpu::buffer_vram_domain(domain)) {
            auto &gmc = driver->ivars->bringup.gmc;
            auto &allocator = domain == kBODomainVRAM ? gmc.vram_alloc : gmc.device_vram_alloc;
            if (!allocator.is_inited()) {
                allocRet = kIOReturnNotReady;
                goto bo_alloc_fail;
            }
            amdgpu::VRAMAllocation va = {};
            if (!allocator.alloc(rounded_size, alignment, &va)) {
                allocRet = kIOReturnNoSpace;
                goto bo_alloc_fail;
            }
            e.gpu_va      = va.gpu_va;
            e.vram_offset = va.gpu_va - gmc.vram_start;
            e.size        = va.size;
            e.alignment   = va.alignment;
            // CPU pointer remains null. Visible buffers use BOWrite/BORead;
            // device-only buffers are transferred through BOCopy.
        }
        else if (domain == kBODomainGTT) {
            // Allocate per-BO sysmem + DART-pin + bind into GART. Mirrors
            // amdgpu::gart_bind_sysmem but keeps the IOBufferMemoryDescriptor
            // owned by this BO entry so BOFree releases it.
            auto &gart = driver->ivars->bringup.gart;
            if (gart.numPTEs == 0) {
                MACAMDGPU_LOG("BOAlloc(GTT): GART not yet initialized "
                              "(gfxhub_gart_enable hasn't run) — "
                              "returning kIOReturnNotReady");
                allocRet = kIOReturnNotReady;
                goto bo_alloc_fail;
            }
            // Require data-verified GPU host-memory transfers and complete
            // mapping teardown before exposing GTT BOs. Earlier zero readback
            // does not establish which address-translation layer failed.
            if (!gart.reads_supported) {
                MACAMDGPU_LOG("BOAlloc(GTT): refused — gart.reads_supported "
                              "= false; GPU host-memory transfers "
                              "remain unverified. Use "
                              "kBODomainVRAM instead. Returning "
                              "kIOReturnUnsupported.");
                allocRet = kIOReturnUnsupported;
                goto bo_alloc_fail;
            }
            auto &binding = e.gttBinding;
            kern_return_t r = amdgpu::gart_bind_sysmem(
                driver->ivars->bringup.device, gart,
                rounded_size, alignment, &binding);
            if (r != kIOReturnSuccess) {
                if (binding.sysmemBuffer || binding.numGPUPages) {
                    // A partially published mapping must survive until Stop.
                    // Keep the slot in use even though no handle is returned.
                    driver->ivars->shutdownBlocked = true;
                    return r;
                }
                allocRet = r;
                goto bo_alloc_fail;
            }
            e.gtt_buf      = binding.sysmemBuffer;
            e.gtt_dma      = binding.dmaCommand;
            e.gtt_bus_addr = binding.busAddr;
            e.gpu_va       = binding.gartMCAddr;
            e.cpu_addr     = binding.cpuAddr;
            e.size         = binding.sizeBytes;
        }
        else {
            allocRet = kIOReturnBadArgument;
            goto bo_alloc_fail;
        }

        {
            const uint64_t handle = mac_amdgpu_bo_make_handle(e.generation, idx);
            arguments->scalarOutput[0] = handle;
            if (legacy) {
                arguments->scalarOutput[1] =
                    ivars->dmaSegments[0].address + e.byte_offset;
                arguments->scalarOutput[2] = e.byte_offset;
            } else {
                arguments->scalarOutput[1] = mac_amdgpu_bo_gpu_addr(ivars, &e);
                arguments->scalarOutput[2] =
                    reinterpret_cast<uint64_t>(e.cpu_addr);
            }
            MACAMDGPU_LOG("BOAlloc: handle=%#llx domain=%u size=%llu "
                          "gpu_va=%#llx",
                          handle, domain, e.size,
                          mac_amdgpu_bo_gpu_addr(ivars, &e));
            return kIOReturnSuccess;
        }

    bo_alloc_fail:
        e.in_use = false;
        return allocRet;
    }

    case kMacAMDGPUMethodBOFree: {
        // scalarInput[0] = handle
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint64_t handle = arguments->scalarInput[0];
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, handle);
        if (e == nullptr) return kIOReturnBadArgument;

        // Release domain-specific storage.
        if (amdgpu::buffer_vram_domain(e->domain)) {
            auto &gmc = driver->ivars->bringup.gmc;
            amdgpu::VRAMAllocation va = {};
            va.gpu_va    = e->gpu_va;
            va.size      = e->size;
            va.alignment = e->alignment;
            va.cpu_ptr   = nullptr;
            auto &allocator = e->domain == kBODomainVRAM ? gmc.vram_alloc : gmc.device_vram_alloc;
            allocator.free(va);
        }
        else if (e->domain == kBODomainGTT) {
            const auto r = amdgpu::gart_unbind(driver->ivars->bringup.device,
                driver->ivars->bringup.gart, &e->gttBinding);
            if (r != kIOReturnSuccess) {
                driver->ivars->shutdownBlocked = true;
                return r;
            }
        }
        // kBODomainGTTLegacy: bump cursor stays where it is so existing
        // submits in flight don't get clobbered; the table slot is freed.

        e->in_use      = false;
        e->gtt_buf     = nullptr;
        e->gtt_dma     = nullptr;
        e->cpu_addr    = nullptr;
        // size / offset / gpu_va stay set so a stale handle keeps failing
        // until the slot is re-allocated (generation bump).
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOGetInfo: {
        // scalarInput[0] = handle
        // Outputs (5 fields, but only 3 required by pre-v0.1.27 callers):
        //   out[0] = gpu_va  (legacy: bus address within client DMA buffer)
        //   out[1] = legacy byte_offset (0 for VRAM/GTT)
        //   out[2] = size
        //   out[3] = alignment  (only filled if scalarOutputCount >= 5)
        //   out[4] = domain | (is_mapped << 8)
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 3) {
            return kIOReturnBadArgument;
        }
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr) return kIOReturnBadArgument;

        arguments->scalarOutput[0] = mac_amdgpu_bo_gpu_addr(ivars, e);
        arguments->scalarOutput[1] = e->byte_offset;
        arguments->scalarOutput[2] = e->size;
        if (arguments->scalarOutputCount >= 4) {
            arguments->scalarOutput[3] = e->alignment;
        }
        if (arguments->scalarOutputCount >= 5) {
            uint64_t is_mapped = (e->cpu_addr != nullptr) ? 1ULL : 0ULL;
            arguments->scalarOutput[4] =
                static_cast<uint64_t>(e->domain) | (is_mapped << 8);
        }
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodBOMap: {
        // scalarInput[0]  = handle
        // scalarOutput[0] = memory_type to pass to IOConnectMapMemory64
        // scalarOutput[1] = size (for caller's convenience)
        //
        // DriverKit can't synthesise a userspace VA from inside the dext;
        // the canonical pattern is to vend an IOMemoryDescriptor through
        // CopyClientMemoryForType and let userspace call
        // IOConnectMapMemory64(conn, memory_type) to obtain the cpu_va.
        // BOMap therefore returns a memory_type encoding the BO's table
        // index; CopyClientMemoryForType below decodes it and hands back
        // the underlying descriptor.
        //
        // Today only kBODomainGTT BOs can be mapped (they own an
        // IOBufferMemoryDescriptor we can return). kBODomainVRAM would
        // need a BAR0-aperture descriptor — out of scope for v0.1.27
        // until we re-export a BAR0 subrange descriptor per BO. Returns
        // kIOReturnUnsupported for those.
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint64_t handle = arguments->scalarInput[0];
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, handle);
        if (e == nullptr) return kIOReturnBadArgument;
        if (e->domain == kBODomainGTTLegacy) {
            // Legacy BOs are already mapped via the client DMABuffer
            // memory type — caller should map kMacAMDGPUMemoryTypeDMABuffer
            // and use byte_offset. Surface that contract explicitly.
            arguments->scalarOutput[0] = kMacAMDGPUMemoryTypeDMABuffer;
            if (arguments->scalarOutputCount >= 2) {
                arguments->scalarOutput[1] = e->size;
            }
            return kIOReturnSuccess;
        }
        if (e->domain != kBODomainGTT) {
            // VRAM domain mapping needs BAR0-aperture re-export — not
            // wired up for v0.1.27.
            return kIOReturnUnsupported;
        }
        if (e->gtt_buf == nullptr) return kIOReturnNotReady;
        uint32_t idx = mac_amdgpu_bo_handle_index(handle);
        arguments->scalarOutput[0] = kMacAMDGPUMemoryTypeBOBase + idx;
        if (arguments->scalarOutputCount >= 2) {
            arguments->scalarOutput[1] = e->size;
        }
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodSubmitIB: {
        // v0.1.28 — repurposed as the CS submission entrypoint.
        //   scalarInput[0] = cs_handle (from CSCreate)
        //   scalarOutput[0] = fence_handle (== cs_handle for now;
        //                     one-fence-per-CS in this revision)
        //
        // Legacy callers that pass (bo_handle, ib_dw, 0) are still
        // accepted via the BO fallback path below — kept so that any
        // pre-v0.1.28 test code keeps compiling. New code MUST use the
        // CS path.
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        if (!driver->ivars->pciOpen) return kIOReturnNotOpen;

        // First try the new CS-handle path.
        CSEntry *cs = mac_amdgpu_cs_lookup(ivars, arguments->scalarInput[0]);
        if (cs != nullptr) {
            if (cs->written_dw == 0) return kIOReturnBadArgument;
            switch (cs->ip_type) {
            case kMacAMDGPUCSIPTypeSDMA: {
                // Append the user CS dwords directly into the SDMA
                // ring as an inline submission, then close with a
                // FENCE packet so WaitFence can poll a VRAM WB slot.
                // Inline VRAM packets avoid depending on the unverified
                // GART-bound system-memory IB path.
                auto &b = driver->ivars->bringup;
                if (cs->ip_instance >= amdgpu::kSDMAInstanceCount) {
                    return kIOReturnBadArgument;
                }
                auto &inst = b.sdma.instance[cs->ip_instance];
                if (!inst.inited || !inst.enabled) return kIOReturnNotReady;

                // Pre-clear our fence slot at WB+0xC0 (0x80 belongs to
                // sdma_ring_test/copy_linear_test) and emit user
                // dwords + FENCE.
                const auto clear = amdgpu::sdma_clear_fence(b.device, inst, 0xC0);
                if (clear != kIOReturnSuccess) return clear;
                const uint32_t fence_value = driver->ivars->submission.beginSDMA(
                    &inst.cs_fence_shadow, amdgpu::sdma_read_cs_fence, &inst);
                if (!fence_value) return kIOReturnNoResources;
                const uint64_t fence_gpu   = inst.wb_bus + 0xC0;
                cs->last_fence = fence_value;
                // The device sequence is never reused within this session.

                uint32_t wrote = amdgpu::sdma_ring_write(
                    b.device, inst, cs->cpu_buffer, cs->written_dw);
                if (wrote != cs->written_dw) return kIOReturnNoSpace;

                uint32_t pkt[4];
                pkt[0] = amdgpu::sdma_fence_header();
                pkt[1] = static_cast<uint32_t>(fence_gpu);
                pkt[2] = static_cast<uint32_t>(fence_gpu >> 32);
                pkt[3] = fence_value;
                if (amdgpu::sdma_ring_write(b.device, inst, pkt, 4) != 4) {
                    return kIOReturnNoSpace;
                }
                kern_return_t r = amdgpu::sdma_kick_doorbell(b.device, inst);
                if (r != kIOReturnSuccess) return r;

                cs->last_fence = fence_value;
                // fence_handle == cs_handle in this revision; userspace
                // passes it straight to WaitFence.
                arguments->scalarOutput[0] = arguments->scalarInput[0];
                return kIOReturnSuccess;
            }
            case kMacAMDGPUCSIPTypeGFX: {
                if (cs->ip_instance != 0) return kIOReturnBadArgument;
                auto &b = driver->ivars->bringup;
                auto &cp = b.cp;
                if (!cp.ringReady) return kIOReturnNotReady;
                if (cp.fence_counter == UINT32_MAX) return kIOReturnNoResources;
                // Use the MES-mapped kernel GFX queue. Raw developer PM4 has
                // no BO reference list, so admission keeps every allocation
                // alive and blocks other mutations until this fence completes.
                if (!driver->ivars->submission.beginCP(
                        cp.fence_cpu, amdgpu::cp_read_cs_fence, &cp))
                    return kIOReturnBusy;
                if (amdgpu::cp_ring_write(cp, cs->cpu_buffer, cs->written_dw)
                        != cs->written_dw) return kIOReturnNoSpace;
                const uint32_t fence = amdgpu::cp_emit_eop_fence(cp);
                if (!fence) return kIOReturnNoSpace;
                driver->ivars->submission.expected = fence;
                cs->last_fence = fence;
                // On upload/kick failure the submission remains pending;
                // only verified completion or StopGPU can release resources.
                const auto r = amdgpu::cp_kick_doorbell(b.device, cp);
                if (r != kIOReturnSuccess) return r;
                arguments->scalarOutput[0] = arguments->scalarInput[0];
                return kIOReturnSuccess;
            }
            case kMacAMDGPUCSIPTypeCompute:
                // Compute needs its own queue and dispatch setup.
                return kIOReturnUnsupported;
            default:
                return kIOReturnBadArgument;
            }
        }

        // Legacy BO-handle fallback (pre-v0.1.28 callers).
        if (arguments->scalarInputCount < 3) return kIOReturnBadArgument;
        BOEntry *e = mac_amdgpu_bo_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr) return kIOReturnBadArgument;
        uint64_t ib_dw = arguments->scalarInput[1];
        if (e->domain != kBODomainGTTLegacy || ivars->dmaBuffer == nullptr ||
            ivars->dmaSegmentsCount == 0 || ib_dw == 0 || ib_dw > UINT32_MAX ||
            ib_dw > e->size / sizeof(uint32_t) ||
            !amdgpu::client_subrange(e->byte_offset, e->size, ivars->dmaBufferSize)) {
            return kIOReturnBadArgument;
        }
        if (arguments->scalarInput[2] != 0) return kIOReturnUnsupported;

        IOAddressSegment seg = {};
        if (ivars->dmaBuffer->GetAddressRange(&seg) != kIOReturnSuccess) {
            return kIOReturnInternalError;
        }
        const uint32_t *ib_words = reinterpret_cast<const uint32_t *>(
            seg.address + e->byte_offset);

        auto &cp = driver->ivars->bringup.cp;
        if (cp.fence_counter == UINT32_MAX) return kIOReturnNoResources;
        if (!cp.ringReady || !driver->ivars->submission.beginCP(
                cp.fence_cpu, amdgpu::cp_read_cs_fence, &cp))
            return kIOReturnNotReady;
        uint32_t wrote = amdgpu::cp_ring_write(cp, ib_words,
                                               static_cast<uint32_t>(ib_dw));
        if (wrote != ib_dw) return kIOReturnNoSpace;

        uint32_t fence = amdgpu::cp_emit_eop_fence(cp);
        if (fence == 0) return kIOReturnNoSpace;
        driver->ivars->submission.expected = fence;
        driver->ivars->submission.lastCPFence = fence;
        kern_return_t r = amdgpu::cp_kick_doorbell(
            driver->ivars->bringup.device, cp);
        if (r != kIOReturnSuccess) return r;

        arguments->scalarOutput[0] = fence;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodMESAddQueue:
        // Persistent user queues require queue removal and retained MQD/wptr
        // BO references. Raw CS fencing does not establish that lifetime.
        return kIOReturnUnsupported;

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
        // v0.1.28 — repurposed for CS-handle-based fences. Spec:
        //   scalarInput[0] = fence_handle (== cs_handle for now)
        //   scalarInput[1] = timeout_ns
        //   scalarOutput[0] = status (0=signaled, 1=timeout)
        //
        // Monotonic timeout is capped at one second so removal and Stop
        // cannot be blocked by an arbitrary caller-supplied duration.
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 2 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        uint64_t fence_handle = arguments->scalarInput[0];
        uint64_t to_ns        = arguments->scalarInput[1];
        if (to_ns == 0) to_ns = 1000000000ull;  // 1 s default

        // CS-handle path.
        CSEntry *cs = mac_amdgpu_cs_lookup(ivars, fence_handle);
        if (cs != nullptr) {
            if (cs->last_fence == 0) {
                // Nothing was submitted on this CS — treat as immediate
                // signal so caller can shortcut empty-CS waits.
                arguments->scalarOutput[0] = 0;
                return kIOReturnSuccess;
            }
            switch (cs->ip_type) {
            case kMacAMDGPUCSIPTypeSDMA: {
                auto &b = driver->ivars->bringup;
                if (cs->ip_instance >= amdgpu::kSDMAInstanceCount) {
                    arguments->scalarOutput[0] = 1;
                    return kIOReturnBadArgument;
                }
                auto &inst = b.sdma.instance[cs->ip_instance];
                if (!inst.inited) return kIOReturnNotReady;
                const uint64_t duration = amdgpu::client_wait_ns(to_ns);
                const uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                for (;;) {
                    driver->ivars->submission.poll();
                    if (cs->last_fence <= driver->ivars->submission.completed) {
                        arguments->scalarOutput[0] = 0;
                        return kIOReturnSuccess;
                    }
                    if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started >= duration) break;
                    IOSleep(1);
                }
                arguments->scalarOutput[0] = 1;  // timeout
                return kIOReturnTimeout;
            }
            case kMacAMDGPUCSIPTypeGFX: {
                if (cs->ip_instance != 0) return kIOReturnBadArgument;
                if (!driver->ivars->bringup.cp.ringReady) return kIOReturnNotReady;
                const uint64_t duration = amdgpu::client_wait_ns(to_ns);
                const uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                for (;;) {
                    driver->ivars->submission.poll();
                    if (cs->last_fence <= driver->ivars->submission.completedCPFence) {
                        arguments->scalarOutput[0] = 0;
                        return kIOReturnSuccess;
                    }
                    if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started >= duration) break;
                    IOSleep(1);
                }
                arguments->scalarOutput[0] = 1;
                return kIOReturnTimeout;
            }
            case kMacAMDGPUCSIPTypeCompute:
                return kIOReturnUnsupported;
            default:
                return kIOReturnBadArgument;
            }
        }

        // Legacy fence-value path (pre-v0.1.28 callers): treat
        // scalarInput[0] as a CP fence target and scalarInput[1] as
        // timeout_us (the original shape). Documented as deprecated;
        // remove once macamdgpu_ping is rewired to the CS ABI.
        uint64_t target = fence_handle;
        if (target == 0 || target != driver->ivars->submission.lastCPFence)
            return kIOReturnBadArgument;
        const uint64_t duration = amdgpu::client_wait_ns(arguments->scalarInput[1], true);
        auto &cp = driver->ivars->bringup.cp;
        if (cp.fence_cpu == nullptr) return kIOReturnNotReady;
        const uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (;;) {
            driver->ivars->submission.poll();
            uint64_t observed = driver->ivars->submission.completedCPFence;
            if (observed >= target) {
                arguments->scalarOutput[0] = observed;
                return kIOReturnSuccess;
            }
            if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started >= duration) break;
            IOSleep(1);
        }
        arguments->scalarOutput[0] = *cp.fence_cpu;
        return kIOReturnTimeout;
    }

    case kMacAMDGPUMethodCSCreate: {
        // v0.1.28 — allocate a CS slot + scratch buffer.
        //   scalarInput[0] = ip_type (kMacAMDGPUCSIPType*)
        //   scalarInput[1] = ip_instance (optional; SDMA engine 0 or 1)
        //   scalarOutput[0] = cs_handle (== 0 on failure)
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1 ||
            arguments->scalarOutput == nullptr ||
            arguments->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }
        const uint32_t ip_type =
            static_cast<uint32_t>(arguments->scalarInput[0]);
        const uint32_t ip_inst = arguments->scalarInputCount >= 2
            ? static_cast<uint32_t>(arguments->scalarInput[1])
            : 0;
        if (ip_type != kMacAMDGPUCSIPTypeSDMA &&
            ip_type != kMacAMDGPUCSIPTypeGFX &&
            ip_type != kMacAMDGPUCSIPTypeCompute) {
            return kIOReturnBadArgument;
        }
        // Find a free slot.
        int idx = -1;
        for (uint32_t i = 0; i < MACAMDGPU_MAX_CS; i++) {
            if (!ivars->cs[i].in_use) { idx = static_cast<int>(i); break; }
        }
        if (idx < 0) {
            arguments->scalarOutput[0] = 0;
            return kIOReturnNoResources;
        }
        CSEntry *e = &ivars->cs[idx];
        e->cpu_buffer = IONewZero(uint32_t, MACAMDGPU_CS_CAPACITY_DW);
        if (e->cpu_buffer == nullptr) {
            arguments->scalarOutput[0] = 0;
            return kIOReturnNoMemory;
        }
        e->in_use      = true;
        e->ip_type     = ip_type;
        e->ip_instance = ip_inst;
        e->capacity_dw = MACAMDGPU_CS_CAPACITY_DW;
        e->written_dw  = 0;
        e->last_fence  = 0;
        e->generation  = ++ivars->csGenCounter;
        // Wrap generation so we never emit a zero gen for an in-use
        // slot (a zero-gen handle is reserved for "invalid").
        if ((e->generation & 0xFFFFu) == 0) {
            e->generation = ++ivars->csGenCounter;
        }
        arguments->scalarOutput[0] =
            mac_amdgpu_cs_make_handle(e->generation,
                                      static_cast<uint32_t>(idx));
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodCSWriteDwords: {
        // v0.1.28 — append up to 8 dwords per call.
        //   scalarInput[0]    = cs_handle
        //   scalarInput[1..8] = dwords to append (low 32 bits used)
        //   scalarInput[9]    = count (1..8)
        // No output on success beyond kIOReturnSuccess.
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 10) {
            return kIOReturnBadArgument;
        }
        CSEntry *e = mac_amdgpu_cs_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr) return kIOReturnBadArgument;
        const uint64_t count_in = arguments->scalarInput[9];
        if (count_in == 0 || count_in > 8) return kIOReturnBadArgument;
        const uint32_t count = static_cast<uint32_t>(count_in);
        if (e->written_dw + count > e->capacity_dw) {
            return kIOReturnNoSpace;
        }
        for (uint32_t i = 0; i < count; i++) {
            e->cpu_buffer[e->written_dw + i] =
                static_cast<uint32_t>(arguments->scalarInput[1 + i]);
        }
        e->written_dw += count;
        return kIOReturnSuccess;
    }

    case kMacAMDGPUMethodCSDestroy: {
        // v0.1.28 — free a CS slot.
        //   scalarInput[0] = cs_handle
        if (arguments->scalarInput == nullptr ||
            arguments->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }
        CSEntry *e = mac_amdgpu_cs_lookup(ivars, arguments->scalarInput[0]);
        if (e == nullptr) return kIOReturnBadArgument;
        mac_amdgpu_cs_free_slot(e);
        return kIOReturnSuccess;
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
    if (ivars == nullptr || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnNotAttached;
    }
    if (ivars->ownerDriver == nullptr || ivars->ownerDriver->ivars == nullptr ||
        __atomic_load_n(&ivars->ownerDriver->ivars->stopping, __ATOMIC_ACQUIRE)) {
        return kIOReturnNotAttached;
    }
    if (ivars->ownerDriver->ivars->shutdownBlocked) return kIOReturnNotReady;
    if (memory == nullptr || options == nullptr) {
        return kIOReturnBadArgument;
    }

    auto *ownerState = ivars->ownerDriver->ivars;
    if (ownerState->pciOpen && ownerState->openerUserClient != this) return kIOReturnBusy;
    if (!ownerState->submission.poll()) return kIOReturnBusy;

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

    // v0.1.27 — per-BO mapping. BOMap returns (kMacAMDGPUMemoryTypeBOBase
    // + bo_index); userspace then calls IOConnectMapMemory64(type) which
    // routes here.
    if (type >= kMacAMDGPUMemoryTypeBOBase &&
        type <  kMacAMDGPUMemoryTypeBOBase + MACAMDGPU_MAX_BO) {
        if (ivars == nullptr) return kIOReturnNotReady;
        uint32_t idx = static_cast<uint32_t>(type - kMacAMDGPUMemoryTypeBOBase);
        BOEntry &e = ivars->bos[idx];
        if (!e.in_use || e.domain != kBODomainGTT ||
            e.gtt_buf == nullptr) {
            return kIOReturnBadArgument;
        }
        e.gtt_buf->retain();
        *options = 0;
        *memory  = e.gtt_buf;
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

    // Use the same BAR0 framebuffer / BAR2 doorbell setup regardless of
    // whether the client maps memory or calls a selector first.
    kern_return_t openRet = mac_amdgpu_ensure_open(this, driver, pci);
    if (openRet != kIOReturnSuccess) return openRet;

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
    ivars->mappedBAR = true;
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
    if (ivars == nullptr || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&ivars->interruptsSetUp, __ATOMIC_ACQUIRE)) return;

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
        if (__atomic_load_n(&driver->ivars->stopping, __ATOMIC_ACQUIRE)) return;
        auto &bringup = driver->ivars->bringup;
        if (bringup.ih.enabled && bringup.ih.inited) {
            // Only the opener client receives IH events for now.
            // Never borrow another client's ivars: its Stop barrier only
            // drains its own sources, not this client's IRQ queue.
            if (__atomic_load_n(&driver->ivars->openerUserClient,
                                __ATOMIC_ACQUIRE) == (IOService *)this) {
                IHDispatchCtx dctx{ ivars };
                uint32_t n = amdgpu::ih_drain(bringup.device, bringup.ih,
                                              &mac_amdgpu_ih_dispatch,
                                              &dctx);
                if (bringup.ih.overflows_seen > 0) {
                    mac_amdgpu_set_irq_bit(ivars, kIRQBitIHOverflow);
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
