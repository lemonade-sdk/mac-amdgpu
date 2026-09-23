#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotAttached = 1,
    kIOReturnUnsupported = 2, kIOReturnNotReady = 3, kIOReturnTimeout = 4,
    kIOReturnBusy = 5;
constexpr int kIOPCICapabilityIDPCIExpress = 0x10,
    kIOPCIDeviceResetTypeFunctionReset = 1, kIOPCIDeviceResetOptionNone = 0;
static uint64_t timeNS;
static uint64_t test_clock(int) { return timeNS; }
static void IOSleep(unsigned ms) { timeNS += uint64_t(ms) * 1000000; }
#define clock_gettime_nsec_np test_clock
#undef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW 0
#define MACAMDGPU_LOG(...) do {} while (0)
struct IOService { void release() {} };
struct IODispatchQueue { void release() {} };
#define SUPERDISPATCH 0
#define OSDynamicCast(type, value) static_cast<type *>(value)
#define IOSafeDeleteNULL(value, type, count) do { delete value; value = nullptr; } while (0)
#include "../dext/amdgpu/amdgpu_client_lifecycle.h"
static std::vector<std::string> events;
static bool metricsInvalidated;
struct IOPCIDevice {
    uint16_t command = 6;
    bool absent = false, supportsFLR = true, refusesBM = false;
    bool failsReset = false, reenabledBM = false;
    uint64_t pendingUntil = 0;
    bool resetDone = false, closed = false;
    unsigned resets = 0;
    IOService *expectedOwner = nullptr;
    void ConfigurationRead16(uint32_t reg, uint16_t *out) {
        if (absent) { *out = 0xFFFF; return; }
        if (reg == 0) *out = 0x1002;
        else if (reg == 4) *out = command;
        else { assert(reg == 0x4A); *out = timeNS < pendingUntil ? 0x20 : 0; }
    }
    void ConfigurationRead32(uint32_t reg, uint32_t *out) {
        assert(reg == 0x44); *out = supportsFLR ? 1u << 28 : 0;
    }
    void ConfigurationWrite16(uint32_t reg, uint16_t value) {
        assert(reg == 4); assert(!(value & 4));
        events.push_back("disable BM");
        if (!refusesBM) command = value;
    }
    int FindPCICapability(int cap, int, uint64_t *offset) {
        assert(cap == 0x10); *offset = 0x40; return 0;
    }
    int Reset(int type, int options) {
        assert(metricsInvalidated);
        assert(type == kIOPCIDeviceResetTypeFunctionReset && options == 0);
        assert(!(command & 4)); assert(timeNS >= pendingUntil);
        assert(std::find(events.begin(), events.end(), "free DMA") == events.end());
        ++resets; events.push_back("reset");
        if (failsReset) return kIOReturnTimeout;
        resetDone = true;
        if (reenabledBM) command |= 4;
        return 0;
    }
    int Open(IOService *client, int) { assert(client == expectedOwner); events.push_back("open"); return 0; }
    void Close(IOService *client, int) {
        assert(client == expectedOwner);
        assert(metricsInvalidated);
        command &= ~6; closed = true; events.push_back("close");
    }
};
struct CS { bool in_use = true; };
constexpr uint32_t kBODomainGTT = 2;
struct TestBinding { bool mapped = true; };
static bool retireSucceeds = true, unmapSucceeds=true;
struct BO { bool in_use = false; uint32_t domain = kBODomainGTT; TestBinding gttBinding;
    void *gtt_buf = nullptr, *gtt_dma = nullptr, *cpu_addr = nullptr; };
namespace amdgpu {
enum class BringupStage { None, SDMAInit };
struct TestAQLQueue {void *owner=nullptr;};
struct Bringup { TestAQLQueue aqlQueues[7]; int gmc=0,mes=0; bool initialized = true, metrics = true; BringupStage reached = BringupStage::SDMAInit;
    int device = 0, gart = 0; };
static int aql_queue_close(int &,int &,int &,TestAQLQueue &queue) {
    events.push_back("unmap AQL");
    if (!unmapSucceeds) return kIOReturnNotReady;
    queue={};return 0;
}
static int gart_unbind(int &, int &, TestBinding *binding) {
    events.push_back("unbind");
    if (!retireSucceeds) return kIOReturnNotReady;
    binding->mapped = false;
    return 0;
}
static void smu_metrics_invalidate(bool &valid, int status) {
    assert(status == kIOReturnNotReady);
    valid = false;
    metricsInvalidated = true;
}
}
struct ClientState;
struct DriverState {
    bool shutdownInProgress = false, shutdownBlocked = false, pciOpen = true;
    uint32_t connectedClients = 1;
    amdgpu::ClientSessions sessions;
    IOPCIDevice *retainedPCI = nullptr;
    amdgpu::Bringup bringup;
    amdgpu::ClientSubmission submission;
    ClientState *quarantinedClient = nullptr;
};
struct MacAMDGPU : IOService { DriverState *ivars; };
struct ClientState {
    MacAMDGPU *ownerDriver;
    bool claimed = false;
    ClientState *quarantineNext = nullptr;
    bool mappedBAR = false;
    void *pendingInterruptNotify = nullptr;
    void *interruptSources[2] = {};
    CS cs[2];
    BO bos[2];
    IODispatchQueue *stopQueue = nullptr;
    IOService *stopProvider = nullptr;
};
using MacAMDGPUUserClient_IVars = ClientState;
struct MacAMDGPUUserClient : IOService {
    ClientState *ivars;
    void FinishStop(IOService *provider);
    void Stop(IOService *, int) { events.push_back("super stop"); }
};
static void mac_amdgpu_bo_release_all(ClientState *c, MacAMDGPU *d) {
    assert(c->ownerDriver == d && d->ivars->retainedPCI->closed);
    events.push_back("free BO");
}
static void mac_amdgpu_cs_free_slot(CS *cs) { cs->in_use = false; }
static void mac_amdgpu_release_dma_buffer(MacAMDGPUUserClient *c) {
    assert(c->ivars->ownerDriver->ivars->retainedPCI->closed);
    events.push_back("free DMA");
}
namespace amdgpu {
static void bringup_release_resources(Bringup &ctx) {
    ctx.initialized = false; events.push_back("free shared");
}
}
static void mac_amdgpu_release_all_interrupts(MacAMDGPUUserClient *) { events.push_back("drain IRQ"); }
static void mac_amdgpu_release_client_storage(ClientState *, MacAMDGPU *driver) {
    (void)driver;
    events.push_back("free client");
}
#include "retire_client_under_test.inc"
#include "release_quarantine_under_test.inc"
#include "shutdown_under_test.inc"
#include "finish_stop_under_test.inc"
struct Fixture {
    IOPCIDevice pci;
    DriverState state;
    MacAMDGPU driver;
    ClientState clientState;
    MacAMDGPUUserClient client;
    uint64_t phase = 99;
    Fixture() {
        events.clear(); timeNS = 0;
        metricsInvalidated = false;
        driver.ivars = &state; client.ivars = &clientState;
        clientState.ownerDriver = &driver;
        state.retainedPCI = &pci; pci.expectedOwner = &driver;
        assert(state.sessions.attach(&client, clientState.claimed, true));
        retireSucceeds = true;
    }
    int stop() { return mac_amdgpu_shutdown_gpu(&client, phase); }
    void retained() {
        assert(state.bringup.initialized && !pci.closed);
        assert(!state.shutdownInProgress);
        assert(std::find(events.begin(), events.end(), "free DMA") == events.end());
    }
};
int main() {
    {
        // Observers do not count as reset-blocking application participants.
        Fixture f; f.state.connectedClients = 20;
        assert(f.stop() == 0 && f.state.sessions.participants == 0);
    }
    for (int retirementMode : {0, 1, 2, 3}) {
        const bool healthy = retirementMode == 0;
        Fixture f; IODispatchQueue queue;
        f.client.ivars = new ClientState(f.clientState);
        f.client.ivars->stopQueue = &queue;
        MacAMDGPUUserClient peer;
        auto *peerState = new ClientState(f.clientState);
        peerState->claimed = false; peerState->stopQueue = &queue;
        peer.ivars = peerState;
        assert(f.state.sessions.attach(&peer, peerState->claimed, true));
        f.state.connectedClients = 2;
        f.state.shutdownBlocked = retirementMode == 1;
        f.state.submission.pending = retirementMode == 2;
        f.client.ivars->bos[0].in_use = true; // exercise real live GTT unbind
        retireSucceeds = retirementMode != 3;
        f.client.FinishStop(&f.driver);
        assert(f.state.sessions.participants == 1 && f.state.connectedClients == 1);
        assert(f.pci.resets == 0 && !f.pci.closed && f.state.bringup.initialized);
        assert(!metricsInvalidated); // closing one peer does not invalidate live telemetry
        if (healthy) {
            assert(!f.state.quarantinedClient);
            assert(std::count(events.begin(), events.end(), "free client") == 1);
            assert(std::find(events.begin(), events.end(), "unbind") <
                   std::find(events.begin(), events.end(), "free client"));
            peer.FinishStop(&f.driver);
            assert(f.pci.resets == 1 && f.pci.closed && !f.state.bringup.initialized);
        } else {
            assert(f.state.quarantinedClient && !f.state.quarantinedClient->quarantineNext);
            f.pci.failsReset = true;
            peer.FinishStop(&f.driver);
            assert(f.pci.closed && f.state.sessions.participants == 0);
            assert(f.state.quarantinedClient && f.state.quarantinedClient->quarantineNext);
            assert(std::count(events.begin(), events.end(), "free client") == 0);
            // A later observer recovers only after all application peers retired.
            f.client.ivars = &f.clientState; f.clientState.claimed = false;
            f.pci.failsReset = false; f.pci.closed = false;
            assert(f.stop() == 0 && !f.state.quarantinedClient);
            assert(std::count(events.begin(), events.end(), "free client") == 2);
        }
    }

    for (bool closeBootstrapFirst : {false, true}) {
        Fixture f; IODispatchQueue queue;
        f.client.ivars = new ClientState(f.clientState);
        f.client.ivars->stopQueue = &queue;
        MacAMDGPUUserClient observer;
        observer.ivars = new ClientState(f.clientState);
        observer.ivars->claimed = false; observer.ivars->stopQueue = &queue;
        f.state.connectedClients = 2;
        if (closeBootstrapFirst) {
            f.state.bringup.reached = amdgpu::BringupStage::None;
            f.state.sessions.initializationClient = &f.client;
            f.client.FinishStop(&f.driver);
            assert(f.pci.resets == 1 && f.pci.closed);
            assert(!f.state.sessions.initializationClient && f.state.sessions.participants == 0);
            observer.FinishStop(&f.driver);
            assert(f.pci.resets == 1);
        } else {
            observer.FinishStop(&f.driver);
            assert(f.state.sessions.participants == 1 && f.state.pciOpen);
            assert(f.pci.resets == 0 && f.state.bringup.initialized && !metricsInvalidated);
            f.client.FinishStop(&f.driver);
            assert(f.pci.resets == 1);
        }
        assert(f.state.connectedClients == 0);
    }

    // Another participant remains: owned queues must unmap before GTT backing
    // can be retired. An unmap failure retains the entire departing client.
    for (bool fail:{false,true}) {
        Fixture f;IODispatchQueue queue;
        f.client.ivars=new ClientState(f.clientState);f.client.ivars->stopQueue=&queue;
        assert(f.state.sessions.attach(&f.client,f.client.ivars->claimed,true));
        f.state.sessions.participants=2;f.state.connectedClients=2;
        f.state.bringup.aqlQueues[0].owner=f.client.ivars;
        unmapSucceeds=!fail;
        f.client.FinishStop(&f.driver);
        auto unmapped=std::find(events.begin(),events.end(),"unmap AQL");
        assert(unmapped!=events.end() && !f.pci.closed && !f.pci.resets);
        if (fail) {
            assert(f.state.quarantinedClient && f.state.shutdownBlocked);
            assert(std::find(events.begin(),events.end(),"free client")==events.end());
            delete f.state.quarantinedClient;f.state.quarantinedClient=nullptr;
        } else assert(unmapped<std::find(events.begin(),events.end(),"free client"));
        unmapSucceeds=true;
    }

    // Ordinary owner exit uses the reset barrier before freeing its storage.
    for (bool resetFails : {false, true}) {
        Fixture f; IODispatchQueue queue;
        f.client.ivars = new ClientState(f.clientState);
        f.client.ivars->stopQueue = &queue;
        f.pci.failsReset = resetFails;
        assert(f.state.sessions.claimExclusive(&f.client, f.client.ivars->claimed));
        f.state.submission.pending = true;
        f.client.FinishStop(&f.driver);
        assert(f.client.ivars == nullptr && f.state.connectedClients == 0);
        assert(f.pci.closed && !f.state.pciOpen);
        if (resetFails) {
            assert(f.state.shutdownBlocked && f.state.quarantinedClient);
            assert(std::find(events.begin(), events.end(), "free client") == events.end());
            // A fresh client can retry reset; only then is old backing released.
            f.client.ivars = &f.clientState; f.clientState.claimed = false; f.state.connectedClients = 1;
            f.pci.failsReset = false; f.pci.closed = false;
            assert(f.stop() == 0 && !f.state.quarantinedClient);
        } else {
            assert(!f.state.quarantinedClient && !f.state.bringup.initialized);
            auto closed = std::find(events.begin(), events.end(), "close");
            auto freed = std::find(events.begin(), events.end(), "free client");
            assert(closed < freed);
        }
    }

    {
        Fixture f; f.pci.pendingUntil = 5000000;
        assert(f.stop() == 0 && f.phase == 6 && timeNS == 5000000);
        assert(!f.state.pciOpen && !f.state.shutdownBlocked && !f.state.shutdownInProgress);
        assert(!f.state.bringup.initialized && f.state.sessions.participants == 0);
        assert((events == std::vector<std::string>{"disable BM", "reset", "close", "free BO", "free DMA", "free shared"}));
    }
    for (int reason = 0; reason < 5; ++reason) {
        Fixture f;
        if (reason == 0) f.state.sessions.participants = 2;
        if (reason == 1) f.clientState.mappedBAR = true;
        if (reason == 2) f.clientState.interruptSources[1] = &f;
        if (reason == 3) f.clientState.pendingInterruptNotify = &f;
        if (reason == 4) f.state.sessions.exclusiveClient = &f.driver;
        assert(f.stop() == kIOReturnBusy && f.phase == 0);
        assert(f.state.bringup.metrics && !metricsInvalidated);
        assert(events.empty() && !f.state.shutdownBlocked); f.retained();
    }
    {
        Fixture f; f.pci.pendingUntil = 2000000000;
        assert(f.stop() == kIOReturnTimeout && f.phase == 3 && timeNS == 1000000000);
        assert(f.pci.resets == 0 && f.state.shutdownBlocked && !(f.pci.command & 4)); f.retained();
        timeNS = 2000000000;
        assert(f.stop() == 0 && f.phase == 6); // retry drains and resets safely
    }
    {
        Fixture f; f.pci.failsReset = true;
        assert(f.stop() == kIOReturnTimeout && f.phase == 4 && f.pci.resets == 1);
        f.retained();
        f.pci.failsReset = false;
        assert(f.stop() == 0 && f.pci.resets == 2);
    }
    {
        Fixture f; f.pci.supportsFLR = false;
        assert(f.stop() == kIOReturnUnsupported && f.phase == 1);
        assert(events.empty()); f.retained();
    }
    {
        Fixture f; f.pci.absent = true;
        assert(f.stop() == kIOReturnNotAttached && f.phase == 1);
        assert(events.empty()); f.retained();
    }
    {
        Fixture f; f.pci.refusesBM = true;
        assert(f.stop() == kIOReturnNotReady && f.phase == 2 && f.pci.resets == 0); f.retained();
    }
    {
        Fixture f; f.pci.reenabledBM = true;
        assert(f.stop() == kIOReturnNotReady && f.phase == 5); f.retained();
    }
    {
        Fixture f; f.state.pciOpen = false;
        f.pci.command = 0; // SDK Close disabled DMA when the previous client exited
        assert(f.stop() == 0 && events.front() == "open");
    }
    puts("Shutdown lifecycle: DMA/FLR/Close/free ordering, transaction drain, failure retention, admission checks and retry pass");
}
