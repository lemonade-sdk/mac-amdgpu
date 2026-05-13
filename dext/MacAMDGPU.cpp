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
#include "amdgpu/amdgpu_discovery.h"

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
};

// Firmware type tags used by LoadFirmware. Pre-SOS components route
// through psp_bootloader_load_component; SOS is special (psp_load_sos).
enum {
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
};

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
            arguments->scalarOutputCount < 6) {
            return kIOReturnBadArgument;
        }
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
        arguments->scalarOutput[5] = (classRev >> 8) & 0xFFFFFFu;
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
        if (!driver->ivars->pciOpen) {
            // Bringup needs the device context populated, which happens
            // on first BAR map. Force the client to map BAR0 first.
            return kIOReturnNotOpen;
        }
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
            psp.sosFirmware     = bin;
            psp.sosFirmwareSize = fwSize;
            kern_return_t ret = amdgpu::psp_load_sos(dev, psp);
            psp.sosFirmware = nullptr;
            psp.sosFirmwareSize = 0;
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
// InterruptOccurred — MSI-X fires, runs on irqQueue. Set pending
// bit; if there's a pending WaitInterrupt OSAction, wake it.
//
// Lock-free path: client maps the IRQ shared page and polls
// irqPending; or calls WaitInterrupt for an async wait.
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
