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
#include <DriverKit/IOUserServer.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include "MacAMDGPU.h"
#include "MacAMDGPUUserClient.h"

#define MACAMDGPU_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu: " fmt, ##__VA_ARGS__)

//============================================================
// Selectors and memory types — keep in sync with userspace.
//============================================================
enum {
    kMacAMDGPUMethodPing       = 0,
    kMacAMDGPUMethodGetIdentity = 1,
    kMacAMDGPUMethodGetBARInfo = 2,
};

enum {
    kMacAMDGPUMemoryTypeBAR0 = 0,
    kMacAMDGPUMemoryTypeBAR1 = 1,
    kMacAMDGPUMemoryTypeBAR2 = 2,
    kMacAMDGPUMemoryTypeBAR3 = 3,
    kMacAMDGPUMemoryTypeBAR4 = 4,
    kMacAMDGPUMemoryTypeBAR5 = 5,
};

//============================================================
// Driver instance state.
//============================================================
struct MacAMDGPU_IVars {
    bool       pciOpen;
    IOService *openerUserClient;  // tracked so Open/Close entities match
};

struct MacAMDGPUUserClient_IVars {
    bool claimed;  // this client called Claim and owns the PCI Open
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
