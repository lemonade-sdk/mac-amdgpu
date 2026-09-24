#include <cassert>
#include <cstdio>
#include <cstdint>
#include "../dext/amdgpu/amdgpu_client_lifecycle.h"

using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBusy = 1,
              kIOReturnBadArgument = 2, kIOReturnUnsupported = 3, kIOReturnNotOpen = 4,
              kIOReturnNotReady = 5, kIOReturnNotAttached = 6;
enum { kMacAMDGPUMethodRuntimeBuild, kMacAMDGPUMethodPing,
       kMacAMDGPUMethodQueryInfo, kMacAMDGPUMethodShutdownGPU,
       kMacAMDGPUMethodWaitFence, kMacAMDGPUMethodBOGetInfo,
       kMacAMDGPUMethodBOFree, kMacAMDGPUMethodSubmitIB, kMacAMDGPUMethodMESAddQueue,
       kMacAMDGPUMethodClockSnapshot, kMacAMDGPUMethodSampleCachedSensors,
       kMacAMDGPUMethodCollectMetrics, kMacAMDGPUMethodMetricsSnapshot, kMacAMDGPUMethodSoftwareSnapshot,
       kMacAMDGPUMethodLoadFirmware, kMacAMDGPUMethodSetIPBase,
       kMacAMDGPUMethodLoadDiscoveryBin, kMacAMDGPUMethodResetDevice,
       kMacAMDGPUMethodSetupInterrupts, kMacAMDGPUMethodAtomicRequesterExperiment,
       kMacAMDGPUMethodSetPowerState, kMacAMDGPUMethodDisableSmuFeatures };
struct IOService {};
static unsigned openCalls;
#define OSDynamicCast(type, pointer) static_cast<type *>(pointer)
#define MACAMDGPU_LOG(...) do {} while (0)
struct IOPCIDevice {
    IOService *openedBy = nullptr;
    int openResult = 0;
    uint32_t identity=0x75511002,classRev=0x030000c0;
    unsigned identityReads=0,closes=0;
    void Close(IOService *client,int) {assert(client==openedBy);openedBy=nullptr;++closes;}
    void ConfigurationRead32(uint32_t reg,uint32_t *out) {
        assert(openedBy && openResult==0); // identity may never be read before successful Open
        assert(reg==0 || reg==8);++identityReads;*out=reg==0 ? identity : classRev;
    }
    int Open(IOService *client, int) { ++openCalls; openedBy = client; return openResult; }
    uint8_t head = 0;
    uint16_t headers[256] = {};
    unsigned reads = 0;
    void ConfigurationRead8(uint32_t reg, uint8_t *out) { assert(reg == 0x34); *out = head; }
    void ConfigurationRead16(uint32_t reg, uint16_t *out) {
        assert(reg < 256); ++reads; *out = headers[reg];
    }
};
#include "client_pm_cap_under_test.inc"
namespace amdgpu { enum class BringupStage { None, SDMAInit }; }
struct State {
    bool pciOpen = false, shutdownBlocked = false;
    uint16_t deviceID=0;uint8_t revision=0;
    struct {
        amdgpu::BringupStage reached = amdgpu::BringupStage::None;
        struct { bool smuOnline = false; } device;
    } bringup;
    amdgpu::ClientSessions sessions;
    amdgpu::ClientSubmission submission;
};
struct ClientState { bool claimed = false; };
struct MacAMDGPUUserClient : IOService { ClientState *ivars; };
struct MacAMDGPU : IOService { State *ivars; };
#include "client_open_under_test.inc"
#include "client_admission_under_test.inc"
constexpr uint32_t kBODomainGTTLegacy = 0;
struct BOEntry { uint32_t domain = 0; uint64_t size = 4096, byte_offset = 0; };
struct LegacyState {
    BOEntry bo;
    void *dmaBuffer = nullptr;
    uint32_t dmaSegmentsCount = 0;
    uint64_t dmaBufferSize = 4096;
};
struct LegacyArgs { uint32_t scalarInputCount = 3; uint64_t scalarInput[3] = {42, 4, 0}; };
static BOEntry *mac_amdgpu_bo_lookup(LegacyState *state, uint64_t handle) {
    return handle == 42 ? &state->bo : nullptr;
}
static int validate_legacy(LegacyState *ivars, LegacyArgs *arguments) {
#include "client_legacy_validation_under_test.inc"
    return 0;
}

int main() {
    IOPCIDevice config;
    config.head = 0x40; config.headers[0x40] = 0x4405; config.headers[0x44] = 1;
    assert(mac_amdgpu_find_pm_capability(&config) == 0x44 && config.reads == 2);
    config.reads = 0; config.headers[0x44] = 0x4005;
    assert(mac_amdgpu_find_pm_capability(&config) == 0 && config.reads == 2); // cycle
    config.head = 0x41; config.reads = 0;
    assert(mac_amdgpu_find_pm_capability(&config) == 0 && config.reads == 0);
    config.head = 0xFC; config.headers[0xFC] = 1;
    assert(mac_amdgpu_find_pm_capability(&config) == 0); // PMCSR extends beyond config
    config.head = 0x40; config.headers[0x40] = 0xFFFF;
    assert(mac_amdgpu_find_pm_capability(&config) == 0);
    config.reads = 0;
    for (unsigned i = 0x40; i <= 0xFC; i += 4)
        config.headers[i] = uint16_t(((i + 4) & 255) << 8) | 5;
    assert(mac_amdgpu_find_pm_capability(&config) == 0 && config.reads == 48);
    uint64_t alignment, rounded;
    assert(amdgpu::client_allocation_shape(1, 1, 16384, alignment, rounded));
    assert(alignment == 16384 && rounded == 16384);
    assert(amdgpu::client_allocation_shape(32769, 20000, 16384, alignment, rounded));
    assert(alignment == 32768 && rounded == 65536);
    assert(!amdgpu::client_allocation_shape(1, UINT64_MAX, 16384, alignment, rounded));
    assert(!amdgpu::client_allocation_shape(UINT64_MAX, 16384, 16384, alignment, rounded));
    assert(!amdgpu::client_allocation_shape(0, 16384, 16384, alignment, rounded));
    assert(amdgpu::client_allocation_shape(1, uint64_t(1) << 63, 16384, alignment, rounded));
    assert(!amdgpu::client_subrange(UINT64_MAX - 15, 32, UINT64_MAX));
    assert(amdgpu::client_subrange(4096, 4096, 8192));
    assert(!amdgpu::client_subrange(4097, 4096, 8192));
    assert(amdgpu::client_wait_ns(UINT64_MAX) == 1000000000);
    assert(amdgpu::client_wait_ns(UINT64_MAX, true) == 1000000000);
    assert(amdgpu::client_wait_ns(250, true) == 250000);
    assert(amdgpu::client_wait_ns(0) == 1000000000);
    LegacyState legacy; LegacyArgs legacyArgs;
    assert(validate_legacy(&legacy, &legacyArgs) == kIOReturnBadArgument); // null DMA
    legacy.dmaBuffer = &legacy; legacy.dmaSegmentsCount = 1;
    assert(validate_legacy(&legacy, &legacyArgs) == 0);
    legacy.bo.domain = 1; // a VRAM BO cannot be interpreted as a DMA slice
    assert(validate_legacy(&legacy, &legacyArgs) == kIOReturnBadArgument);
    legacy.bo.domain = kBODomainGTTLegacy;
    legacyArgs.scalarInput[1] = UINT64_MAX / 4 + 1;
    assert(validate_legacy(&legacy, &legacyArgs) == kIOReturnBadArgument);
    legacyArgs.scalarInput[1] = 4; legacy.bo.byte_offset = UINT64_MAX - 15;
    assert(validate_legacy(&legacy, &legacyArgs) == kIOReturnBadArgument);

    State state; MacAMDGPU driver; driver.ivars = &state; IOPCIDevice pci;
    ClientState ownerState, observerState;
    MacAMDGPUUserClient owner, observer;
    owner.ivars = &ownerState; observer.ivars = &observerState;
    auto call = [&](IOService &client, uint64_t selector) {
        return mac_amdgpu_admit_external(&client, &driver, &pci, selector);
    };
    // A power button pressed before bringup leaves the app connected, but must
    // not prevent a different runtime client from acquiring bootstrap ownership.
    assert(call(observer, kMacAMDGPUMethodSetPowerState) == kIOReturnNotReady);
    assert(call(observer, kMacAMDGPUMethodDisableSmuFeatures) == kIOReturnNotReady);
    assert(openCalls == 0 && !observerState.claimed && !state.pciOpen);
    assert(state.sessions.participants == 0 && !state.sessions.initializationClient);
    assert(call(observer, kMacAMDGPUMethodMESAddQueue) == kIOReturnUnsupported && openCalls == 0);
    assert(call(observer, kMacAMDGPUMethodSampleCachedSensors) == 0 && openCalls == 0);
    assert(call(observer, kMacAMDGPUMethodClockSnapshot) == 0 && openCalls == 0);
    assert(call(observer, kMacAMDGPUMethodRuntimeBuild) == 0 && openCalls == 0);
    assert(call(observer, kMacAMDGPUMethodMetricsSnapshot) == 0 && openCalls == 0);
    assert(call(owner, kMacAMDGPUMethodCollectMetrics) == kIOReturnNotOpen && openCalls == 0);
    assert(call(owner, kMacAMDGPUMethodBOFree) == 0 && state.sessions.initializationClient == &owner && pci.openedBy == &driver);
    const uint64_t monitorSelectors[] = {
        kMacAMDGPUMethodRuntimeBuild, kMacAMDGPUMethodQueryInfo,
        kMacAMDGPUMethodMetricsSnapshot, kMacAMDGPUMethodClockSnapshot,
        kMacAMDGPUMethodSoftwareSnapshot, kMacAMDGPUMethodSampleCachedSensors
    };
    auto observeWithoutLease = [&] {
        const auto participants = state.sessions.participants;
        const auto initializer = state.sessions.initializationClient;
        const auto exclusive = state.sessions.exclusiveClient;
        const auto opens = openCalls;
        for (auto selector : monitorSelectors) {
            const auto expected = selector == kMacAMDGPUMethodSampleCachedSensors &&
                                  state.submission.pending ? kIOReturnBusy : kIOReturnSuccess;
            assert(call(observer, selector) == expected);
        }
        assert(!observerState.claimed && state.sessions.participants == participants);
        assert(state.sessions.initializationClient == initializer);
        assert(state.sessions.exclusiveClient == exclusive && openCalls == opens);
    };
    observeWithoutLease(); // monitoring during initialization
    assert(call(observer, kMacAMDGPUMethodSubmitIB) == kIOReturnBusy);
    unsigned before = openCalls;
    assert(call(owner, kMacAMDGPUMethodCollectMetrics) == 0 && openCalls == before);
    assert(call(observer, kMacAMDGPUMethodCollectMetrics) == kIOReturnNotOpen && openCalls == before);
    assert(call(observer, kMacAMDGPUMethodMetricsSnapshot) == 0 && openCalls == before);
    assert(call(observer, kMacAMDGPUMethodQueryInfo) == 0 && openCalls == before);

    volatile uint32_t fence = 0;
    const auto first = state.submission.beginSDMA(&fence);
    assert(first == 1 && !state.submission.poll());
    observeWithoutLease(); // cached monitoring during a pending submission
    before = openCalls;
    assert(call(owner, kMacAMDGPUMethodCollectMetrics) == kIOReturnBusy && openCalls == before);
    assert(call(observer, kMacAMDGPUMethodMetricsSnapshot) == 0 && state.submission.pending);
    assert(call(observer, kMacAMDGPUMethodSampleCachedSensors) == kIOReturnBusy && state.submission.pending && openCalls == before);
    assert(call(observer, kMacAMDGPUMethodSoftwareSnapshot) == 0 && state.submission.pending && openCalls == before);
    assert(state.submission.beginSDMA(&fence) == 0); // no overlapping submission
    assert(call(owner, kMacAMDGPUMethodBOFree) == kIOReturnBusy);
    assert(call(owner, kMacAMDGPUMethodSubmitIB) == kIOReturnBusy);
    assert(call(owner, kMacAMDGPUMethodWaitFence) == 0);
    assert(call(owner, kMacAMDGPUMethodShutdownGPU) == 0);
    assert(call(observer, kMacAMDGPUMethodPing) == 0);
    fence = first;
    assert(call(owner, kMacAMDGPUMethodBOFree) == 0);
    auto second = state.submission.beginSDMA(&fence);
    assert(second == 2 && fence == 0 && state.submission.completed == first);
    fence = first; // stale completion cannot satisfy a resubmission
    assert(!state.submission.poll());
    fence = second;
    assert(state.submission.poll() && state.submission.completed == second);
    state.submission.issued = UINT32_MAX;
    assert(state.submission.beginSDMA(&fence) == 0); // never reuse a sequence

    volatile uint64_t cpFence = 7;
    assert(state.submission.beginCP(&cpFence));
    assert(!state.submission.poll()); // partial submit has no valid fence
    state.submission.expected = 8;
    assert(!state.submission.poll());
    cpFence = 8;
    assert(state.submission.poll());
    {
        State shared; MacAMDGPU root; root.ivars = &shared; IOPCIDevice endpoint;
        ClientState aState, bState; MacAMDGPUUserClient a, b;
        a.ivars = &aState; b.ivars = &bState;
        assert(mac_amdgpu_ensure_open(&a, &root, &endpoint) == 0);
        assert(endpoint.openedBy == &root && shared.sessions.participants == 1);
        assert(mac_amdgpu_ensure_open(&b, &root, &endpoint) == kIOReturnBusy);
        shared.bringup.reached = amdgpu::BringupStage::SDMAInit;
        shared.bringup.device.smuOnline = true;
        assert(mac_amdgpu_ensure_open(&b, &root, &endpoint) == 0);
        assert(shared.sessions.participants == 2 && !shared.sessions.initializationClient);
        assert(mac_amdgpu_admit_external(&b, &root, &endpoint, kMacAMDGPUMethodSetPowerState) == 0);
        assert(mac_amdgpu_admit_external(&b, &root, &endpoint, kMacAMDGPUMethodDisableSmuFeatures) == 0);
        assert(mac_amdgpu_admit_external(&a, &root, &endpoint, kMacAMDGPUMethodBOFree) == 0);
        assert(mac_amdgpu_admit_external(&b, &root, &endpoint, kMacAMDGPUMethodBOFree) == 0);
        assert(mac_amdgpu_admit_external(&a, &root, &endpoint, kMacAMDGPUMethodSubmitIB) == kIOReturnBusy);
        assert(mac_amdgpu_admit_external(&a, &root, &endpoint, kMacAMDGPUMethodSetupInterrupts) == kIOReturnBusy);
        assert(mac_amdgpu_admit_external(&a, &root, &endpoint, kMacAMDGPUMethodLoadFirmware) == kIOReturnBusy);
        assert(!shared.sessions.canReset(&a, aState.claimed));
        shared.sessions.detach(&b, bState.claimed);
        assert(mac_amdgpu_admit_external(&a, &root, &endpoint, kMacAMDGPUMethodSubmitIB) == 0);
        assert(shared.sessions.exclusiveClient == &a);
        assert(mac_amdgpu_ensure_open(&b, &root, &endpoint) == kIOReturnBusy);
        assert(mac_amdgpu_admit_external(&b, &root, &endpoint, kMacAMDGPUMethodMetricsSnapshot) == 0);
        shared.sessions.detach(&a, aState.claimed);
        assert(shared.sessions.participants == 0 && !shared.sessions.exclusiveClient);
        shared.pciOpen = false; shared.bringup.reached = amdgpu::BringupStage::None;
        endpoint.openResult = kIOReturnNotOpen;
        assert(mac_amdgpu_ensure_open(&b, &root, &endpoint) == kIOReturnNotOpen);
        assert(!bState.claimed && shared.sessions.participants == 0 && !shared.sessions.initializationClient);
    }
    // Access-denied sentinels and wrong devices must close PCI and unwind a
    // newly attached client; they must never become a successful cached ID.
    for(unsigned invalid=0;invalid<5;++invalid) {
        State checked;checked.deviceID=UINT16_MAX;checked.revision=UINT8_MAX;
        MacAMDGPU root;root.ivars=&checked;IOPCIDevice endpoint;
        ClientState clientState;MacAMDGPUUserClient client;client.ivars=&clientState;
        if(invalid==0) endpoint.identity=UINT32_MAX;
        if(invalid==1) endpoint.identity=0x00001002;
        if(invalid==2) endpoint.identity=0x75511234;
        if(invalid==3) endpoint.classRev=UINT32_MAX;
        if(invalid==4) endpoint.identity=0xffff1002;
        const auto status=mac_amdgpu_ensure_open(&client,&root,&endpoint);
        assert(status==(invalid==0 || invalid==3 || invalid==4 ? kIOReturnNotAttached : kIOReturnUnsupported));
        assert(!checked.pciOpen && !checked.deviceID && !checked.revision && endpoint.closes==1);
        assert(!endpoint.openedBy && !clientState.claimed && checked.sessions.participants==0 && !checked.sessions.initializationClient);
        assert(endpoint.identityReads==2);
        endpoint.identity=0x75511002;endpoint.classRev=0x030000c0;
        assert(mac_amdgpu_ensure_open(&client,&root,&endpoint)==0);
        assert(checked.pciOpen && checked.deviceID==0x7551 && checked.revision==0xc0 && endpoint.identityReads==4);
        assert(mac_amdgpu_ensure_open(&client,&root,&endpoint)==0 && endpoint.identityReads==4);
    }
    {
        State checked;MacAMDGPU root;root.ivars=&checked;IOPCIDevice endpoint;
        ClientState clientState;MacAMDGPUUserClient client;client.ivars=&clientState;
        endpoint.identity=0x744c1002; // general PCI admission is not an R9700-only policy
        assert(mac_amdgpu_ensure_open(&client,&root,&endpoint)==0 && checked.deviceID==0x744c);
    }
    puts("Client lifecycle: checked allocation, timeout cap, observer/owner admission, pending-work gate and unique latched fences pass");
}
