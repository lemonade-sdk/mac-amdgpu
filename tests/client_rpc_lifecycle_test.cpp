#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>
using kern_return_t = int;
#include "../dext/amdgpu/amdgpu_client_lifecycle.h"
#include "../dext/amdgpu/amdgpu_software_stats.h"
constexpr int kIOReturnSuccess = 0, kIOReturnBusy = 1, kIOReturnBadArgument = 2,
              kIOReturnNotReady = 3, kIOReturnTimeout = 4, kIOReturnUnsupported = 5,
              kIOReturnNotOpen = 6, kIOReturnNoSpace = 7, kIOReturnNoResources = 8;
constexpr int kMacAMDGPUMethodSubmitIB = 19;
constexpr int kMacAMDGPUMethodHostMemoryTest = 44, kMacAMDGPUMethodHostWindow = 54;
constexpr int kMacAMDGPUMethodComputeTest = 45;
constexpr int kMacAMDGPUMethodWaitInterrupt = 4, kMacAMDGPUMethodWaitFence = 20;
constexpr int kMacAMDGPUMethodSubmitTestPM4 = 14, kMacAMDGPUMethodCPKIQSmoke = 35,
              kMacAMDGPUMethodSDMACopyTest = 15;
constexpr int kMacAMDGPUCSIPTypeSDMA = 0, kMacAMDGPUCSIPTypeGFX = 1,
              kMacAMDGPUCSIPTypeCompute = 2, MACAMDGPU_IRQ_PENDING_WORDS = 4;
namespace amdgpu { constexpr unsigned kSDMAInstanceCount = 2; enum class BringupStage { None, SDMAInit }; }
struct OSAction { unsigned references = 1; void retain() { ++references; } void release() { --references; } };
struct Args {
    OSAction *completion = nullptr;
    uint64_t *scalarInput = nullptr, *scalarOutput = nullptr;
    unsigned scalarInputCount = 0, scalarOutputCount = 0;
    void *structureInput = nullptr, *structureInputDescriptor = nullptr, *structureOutputDescriptor = nullptr;
    uint64_t structureOutputMaximumSize = 0;
};
struct CSEntry {
    uint32_t last_fence = 0, ip_type = 0, ip_instance = 0;
    uint32_t *cpu_buffer = nullptr;
    uint32_t written_dw = 0;
};
struct ClientState {
    bool interruptsSetUp = true;
    uint64_t irqEnabled[4] = {~0ull, ~0ull, ~0ull, ~0ull}, irqPending[4] = {};
    OSAction *pendingInterruptNotify = nullptr;
    CSEntry cs;
    void *dmaBuffer = nullptr;
    unsigned dmaSegmentsCount = 0;
    uint64_t dmaBufferSize = 4096;
    struct { uint64_t address = 0; } dmaSegments[1];
};
struct DriverState {
    amdgpu::software_stats::Counters softwareStats;
    amdgpu::software_stats::Engine rawStatsEngine{};
    bool rawStatsFailureRecorded=false;
    amdgpu::ClientSubmission submission;
    bool pciOpen = true, shutdownBlocked = false;
    struct {
        int device = 0, mes = 0, gmc = 0;
        int gfx = 0;
        amdgpu::BringupStage reached = amdgpu::BringupStage::SDMAInit;
        struct { bool hostWindowConfigured = false, reads_supported = false;
            uint64_t gartStart = 0, gartSize = 1ull << 28; } gart;
        struct { bool active = false; } memoryTest;
        struct { bool active = false; } computeTest;
        struct { struct {
            bool inited = true, enabled = true;
            uint32_t wptr = 0, cs_fence_shadow = 0;
            uint64_t wb_bus = 0;
        } instance[2]; } sdma;
        struct {
            volatile uint64_t *fence_cpu = nullptr;
            uint64_t wptr = 0;
            uint32_t fence_counter = 0;
            bool ringReady = true;
        } cp;
    } bringup;
};
static int diagnosticResult;
static bool advanceDiagnosticWptr;
static uint64_t diagnosticTimeout;
static std::vector<uint32_t> submittedWords;
static unsigned kicks;
static bool appendOK = true, emitOK = true, cpReadOK = true;
static int kickResult;
static uint64_t gpuCPFence;
namespace amdgpu {
template<class GART> int gart_configure_host_window(int &, GART &gart, uint64_t base) {
    if (advanceDiagnosticWptr) { gart.hostWindowConfigured = true; gart.gartStart = base; }
    return diagnosticResult;
}
void cp_log_control(int &, const char *) {}
struct ComputeTestResult {
    uint32_t stage = 0, mismatches = 0, firstMismatch = UINT32_MAX, fence = 0;
    uint64_t gpuAddress = 0;
};
template<class CP, class Test>
int compute_test(int &, int &, CP &, int &, Test &test, uint32_t, ComputeTestResult &result) {
    test.active = advanceDiagnosticWptr;
    result.stage = test.active ? 3 : 0;
    result.mismatches = 5;
    return diagnosticResult;
}
struct MemoryTransferResult {
    uint32_t stage = 0, mismatches = 0, firstMismatch = UINT32_MAX;
    uint64_t hostGPUAddress = 0, vramGPUAddress = 0;
};
template<class GART, class SDMA, class Test>
int memory_transfer_test(int &, int &, GART &, SDMA &, Test &test,
                          uint32_t, MemoryTransferResult &result) {
    test.active = advanceDiagnosticWptr;
    result.stage = test.active ? 3 : 0;
    result.mismatches = 5;
    return diagnosticResult;
}
static bool cp_read_cs_fence(void *, uint64_t *out) { *out = gpuCPFence; return cpReadOK; }
template<class CP> uint32_t cp_ring_write(CP &cp, const uint32_t *p, uint32_t n) {
    if (!appendOK) return 0;
    submittedWords.insert(submittedWords.end(), p, p+n); cp.wptr += n; return n;
}
template<class CP> uint32_t cp_emit_eop_fence(CP &cp) {
    if (!emitOK) return 0;
    cp.wptr += 10; return ++cp.fence_counter;
}
template<class Dev, class CP> int cp_kick_doorbell(Dev &, CP &) { ++kicks; return kickResult; }
template<class Dev, class SDMA> int sdma_clear_fence(Dev &, SDMA &, unsigned) { return 0; }
static bool sdma_read_cs_fence(void *, uint32_t *) { return false; }
static uint32_t sdma_fence_header() { return 5; }
template<class Dev, class SDMA> uint32_t sdma_ring_write(Dev &, SDMA &, const uint32_t *, uint32_t n) { return n; }
template<class Dev, class SDMA> int sdma_kick_doorbell(Dev &, SDMA &) { return 0; }
template<class Dev, class CP>
int cp_submit_eop_test(Dev &, CP &cp, uint64_t timeout, uint32_t *fence) {
    diagnosticTimeout = timeout; *fence = 0;
    if (advanceDiagnosticWptr) cp.wptr += 10;
    return diagnosticResult;
}
template<class Dev, class CP, class MES, class GMC>
int cp_kiq_smoke_test(Dev &, CP &cp, MES &, GMC &, uint32_t, uint32_t,
                     uint64_t *, uint64_t *, uint32_t *) {
    if (advanceDiagnosticWptr) cp.wptr += 10;
    return diagnosticResult;
}
template<class Dev, class SDMA>
int sdma_copy_linear_test(Dev &, SDMA &instance, uint64_t, uint64_t, uint32_t, uint64_t timeout) {
    diagnosticTimeout = timeout;
    if (advanceDiagnosticWptr) instance.wptr += 12;
    return diagnosticResult;
}
}
struct Driver { DriverState *ivars; };
static CSEntry *mac_amdgpu_cs_lookup(ClientState *state, uint64_t handle) { return handle == 1 ? &state->cs : nullptr; }
static uint64_t nowNS;
static unsigned sleeps, completeAfter;
static volatile uint32_t *signalSlot;
static uint32_t signalValue;
static uint64_t test_clock(int) { return nowNS; }
static void IOSleep(unsigned ms) {
    nowNS += uint64_t(ms) * 1000000;
    if (++sleeps == completeAfter && signalSlot) *signalSlot = signalValue;
}
#define clock_gettime_nsec_np test_clock
#ifdef CLOCK_UPTIME_RAW
#undef CLOCK_UPTIME_RAW
#endif
#define CLOCK_UPTIME_RAW 0
#define MACAMDGPU_LOG(...) do {} while (0)
using MacAMDGPU_IVars = DriverState;
#include "client_software_raw_under_test.inc"
struct Client {
    ClientState *ivars;
    Driver *driver;
    unsigned completions = 0;
    void AsyncCompletion(OSAction *, int, void *, unsigned) { ++completions; }
    int call(int selector, Args *arguments) {
        switch (selector) {
#include "client_rpc_lifecycle_under_test.inc"
#include "client_cs_submit_under_test.inc"
        default: return kIOReturnUnsupported;
        }
    }
};
int main() {
    ClientState clientState; DriverState state; Driver driver{&state}; Client client{&clientState, &driver};
    OSAction first, second; Args args; args.completion = &first;
    assert(client.call(kMacAMDGPUMethodWaitInterrupt, &args) == 0);
    assert(first.references == 2 && clientState.pendingInterruptNotify == &first);
    args.completion = &second;
    assert(client.call(kMacAMDGPUMethodWaitInterrupt, &args) == kIOReturnBusy);
    assert(first.references == 2 && second.references == 1 && client.completions == 0);
    // The original waiter remains owned; Stop/IRQ can complete it normally.
    clientState.pendingInterruptNotify = nullptr; first.release();
    clientState.irqPending[0] = 1;
    assert(client.call(kMacAMDGPUMethodWaitInterrupt, &args) == 0);
    assert(client.completions == 1 && second.references == 1);

    uint32_t fence = 0, cpuCache = 0;
    auto reader = [](void *context, uint32_t *value) {
        *value = *static_cast<uint32_t *>(context);
        return *value != UINT32_MAX;
    };
    auto sequence = state.submission.beginSDMA(&cpuCache, reader, &fence);
    clientState.cs.last_fence = sequence;
    uint64_t input[2] = {1, 2000000}, output = 99;
    args = {}; args.scalarInput = input; args.scalarInputCount = 2;
    args.scalarOutput = &output; args.scalarOutputCount = 1;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    assert(nowNS == 2000000 && output == 1 && state.submission.pending);
    nowNS = 0; sleeps = 0; input[1] = UINT64_MAX;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    assert(nowNS == 1000000000 && sleeps == 1000);
    nowNS = 0; sleeps = 0; completeAfter = 3; signalSlot = &fence; signalValue = sequence;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == 0);
    assert(nowNS == 3000000 && output == 0 && !state.submission.pending);
    // Completion remains valid after another submission clears the shared slot.
    fence = 0;
    assert(state.submission.beginSDMA(&cpuCache, reader, &fence) == sequence + 1);
    nowNS = 0;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == 0 && nowNS == 0);

    volatile uint64_t cpFence = 0; state.submission = {};
    assert(state.submission.beginCP(&cpFence));
    state.submission.expected = state.submission.lastCPFence = 7;
    state.bringup.cp.fence_cpu = &cpFence; input[0] = 7;
    nowNS = 0; sleeps = 0; completeAfter = 0; signalSlot = nullptr;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    assert(nowNS == 1000000000); // legacy microseconds cannot overflow conversion
    cpFence = 7;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == 0 && output == 7);
    input[0] = 8;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnBadArgument);

    // Submit through the actual GFX CS RPC, then wait on its CS handle.
    // A CPU shadow alone must not complete a GPU-owned fence.
    state.submission = {}; cpFence = 99; gpuCPFence = 0;
    nowNS = 0; state.softwareStats.reset(nowNS);
    uint32_t pm4[] = {0xc0001000, 0};
    clientState.cs = {0, kMacAMDGPUCSIPTypeGFX, 0, pm4, 2};
    input[0] = 1; input[1] = 2000000; output = 99;
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == 0);
    assert(output == 1 && kicks == 1 && state.submission.pending);
    assert(state.softwareStats.data.engines[amdgpu::software_stats::GFX].submitted == 1);
    assert(submittedWords == std::vector<uint32_t>({0xc0001000, 0}));
    const auto firstGFX = clientState.cs.last_fence;
    assert(firstGFX == 1 && state.submission.expected == firstGFX);
    nowNS = 0;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    assert(output == 1 && nowNS == 2000000 && state.submission.pending);
    gpuCPFence = UINT64_MAX;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    gpuCPFence = firstGFX; cpReadOK = false;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    cpReadOK = true;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == 0 && output == 0);
    assert(!state.submission.pending && state.submission.completedCPFence == firstGFX);
    const auto &firstCounts = state.softwareStats.data.engines[amdgpu::software_stats::GFX];
    assert(firstCounts.completed == 1 && firstCounts.failed == 1 && firstCounts.pending == 0);
    // Three timed-out polls counted one failed operation, then one completion.

    // New jobs reuse WB storage without erasing an older handle's completion.
    auto oldCS = clientState.cs;
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == 0);
    assert(clientState.cs.last_fence == firstGFX + 1 && state.submission.pending);
    const auto newCS = clientState.cs;
    clientState.cs = oldCS; nowNS = 0;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == 0 && nowNS == 0);
    clientState.cs = newCS;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);
    gpuCPFence = newCS.last_fence;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == 0);
    // SDMA completion cannot satisfy a newer GFX fence.
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == 0);
    state.submission.completed = UINT32_MAX;
    assert(client.call(kMacAMDGPUMethodWaitFence, &args) == kIOReturnTimeout);

    // Failures never publish a success handle and retain submission ownership.
    for (int failure = 0; failure < 3; ++failure) {
        const auto priorSubmissions = state.softwareStats.data.engines[amdgpu::software_stats::GFX].submitted;
        state.submission = {}; output = 99; kicks = 0;
        appendOK = failure != 0; emitOK = failure != 1;
        kickResult = failure == 2 ? kIOReturnTimeout : 0;
        assert(client.call(kMacAMDGPUMethodSubmitIB, &args) ==
               (failure == 2 ? kIOReturnTimeout : kIOReturnNoSpace));
        assert(state.submission.pending && output == 99);
        assert(kicks == (failure == 2 ? 1u : 0u));
        assert(state.softwareStats.data.engines[amdgpu::software_stats::GFX].submitted == priorSubmissions);
    }
    state.submission = {}; appendOK = emitOK = true; kickResult = 0; kicks = 0;
    clientState.cs.ip_instance = 1;
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == kIOReturnBadArgument);
    clientState.cs.ip_instance = 0; state.bringup.cp.ringReady = false;
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == kIOReturnNotReady);
    state.bringup.cp.ringReady = true; state.bringup.cp.fence_counter = UINT32_MAX;
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == kIOReturnNoResources);
    assert(!state.submission.pending && !kicks);
    clientState.cs.ip_type = kMacAMDGPUCSIPTypeCompute;
    assert(client.call(kMacAMDGPUMethodSubmitIB, &args) == kIOReturnUnsupported);
    // Failed memory-test RPCs preserve output diagnostics and require reset
    // only after the test has taken ownership of a session.
    uint64_t memoryInputs[1] = {42}, memoryOutputs[6] = {};
    args.scalarInput = memoryInputs; args.scalarInputCount = 1;
    args.scalarOutput = memoryOutputs; args.scalarOutputCount = 6;
    state.shutdownBlocked = false; diagnosticResult = kIOReturnTimeout;
    advanceDiagnosticWptr = false;
    assert(client.call(kMacAMDGPUMethodHostMemoryTest, &args) == 0);
    assert(memoryOutputs[0] == kIOReturnTimeout && !state.shutdownBlocked);
    advanceDiagnosticWptr = true;
    assert(client.call(kMacAMDGPUMethodHostMemoryTest, &args) == 0);
    assert(memoryOutputs[1] == 3 && memoryOutputs[2] == 5 && state.shutdownBlocked);
    state.shutdownBlocked = false; advanceDiagnosticWptr = false;
    assert(client.call(kMacAMDGPUMethodComputeTest, &args) == 0);
    assert(memoryOutputs[0] == kIOReturnTimeout && !state.shutdownBlocked);
    advanceDiagnosticWptr = true;
    assert(client.call(kMacAMDGPUMethodComputeTest, &args) == 0);
    assert(memoryOutputs[1] == 3 && memoryOutputs[2] == 5 && state.shutdownBlocked);
    // A failed aperture register publication blocks further submissions; a
    // rejected request or a read-only query must not poison the session.
    args.scalarOutputCount = 6; memoryInputs[0] = 0; state.shutdownBlocked = false;
    assert(client.call(kMacAMDGPUMethodHostWindow, &args) == 0 && memoryOutputs[0] == 0 && memoryOutputs[1] == (1ull << 28));
    memoryInputs[0] = 1ull << 36; advanceDiagnosticWptr = false; diagnosticResult = kIOReturnBadArgument;
    assert(client.call(kMacAMDGPUMethodHostWindow, &args) == kIOReturnBadArgument && !state.shutdownBlocked);
    advanceDiagnosticWptr = true; diagnosticResult = kIOReturnTimeout;
    assert(client.call(kMacAMDGPUMethodHostWindow, &args) == kIOReturnTimeout && state.shutdownBlocked);
    state.shutdownBlocked = false; diagnosticResult = 0;
    assert(client.call(kMacAMDGPUMethodHostWindow, &args) == 0 && memoryOutputs[0] == memoryInputs[0]);
    args.structureInput = &args;
    assert(client.call(kMacAMDGPUMethodHostWindow, &args) == kIOReturnBadArgument);
    args.structureInput = nullptr;
    // Failed diagnostics that queued packets require reset. Preflight failures
    // do not poison the session, and caller timeouts remain capped.
    uint64_t diagnosticInputs[5] = {0, 0, 16, 4, UINT64_MAX};
    uint64_t diagnosticOutputs[6] = {};
    args.scalarInput = diagnosticInputs; args.scalarInputCount = 5;
    args.scalarOutput = diagnosticOutputs; args.scalarOutputCount = 6;
    clientState.dmaBuffer = &clientState; clientState.dmaSegmentsCount = 1;
    for (int selector : {kMacAMDGPUMethodSubmitTestPM4, kMacAMDGPUMethodCPKIQSmoke,
                         kMacAMDGPUMethodSDMACopyTest}) {
        state.shutdownBlocked = false; advanceDiagnosticWptr = false;
        diagnosticResult = kIOReturnNotReady;
        client.call(selector, &args);
        assert(!state.shutdownBlocked);
        advanceDiagnosticWptr = true; diagnosticResult = kIOReturnTimeout;
        client.call(selector, &args);
        assert(state.shutdownBlocked);
        if (selector != kMacAMDGPUMethodCPKIQSmoke) assert(diagnosticTimeout == 1000000);
    }
    puts("Client RPC lifecycle: IRQ ownership, deadlines, GFX CS submit/wait, completion isolation and failure retention pass");
}
