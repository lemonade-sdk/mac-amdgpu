#include "amdgpu_metrics_state.h"
#include <cassert>
#include <cstdint>
#include <cstring>

using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNoMemory = 2,
    kIOReturnNotReady = 3, kIOReturnUnsupported = 4, kIOReturnTimeout = 5;
enum { kMacAMDGPUMethodRuntimeBuild, kMacAMDGPUMethodPing, kMacAMDGPUMethodQueryInfo,
    kMacAMDGPUMethodShutdownGPU, kMacAMDGPUMethodGetBARInfo,
    kMacAMDGPUMethodCollectMetrics = 46, kMacAMDGPUMethodMetricsSnapshot = 47 };
namespace amdgpu {
enum class BringupStage { None, SDMAInit };
static unsigned collections, snapshots;
static int collectionResult;
static bool lastReady;
static int smu_collect_metrics(int, SMUMetricsContext &ctx, bool ready) {
    ++collections; lastReady = ready;
    ctx.snapshot.sequence = 42;
    ctx.snapshot.validFields = collectionResult == 0 && ready ? 5 : 0;
    return collectionResult;
}
static void smu_metrics_snapshot(const SMUMetricsContext &ctx, bool ready, SMUMetricsSnapshot &out) {
    ++snapshots; lastReady = ready;
    out = ctx.snapshot;
    out.size = sizeof(out); out.version = 1;
    if (!ready) { out.validFields = 0; out.status = kIOReturnNotReady; }
}
}
static bool allocationFails;
struct OSData {
    uint8_t bytes[sizeof(amdgpu::SMUMetricsSnapshot)];
    static OSData *withBytes(const void *bytes, size_t length) {
        assert(length == sizeof(amdgpu::SMUMetricsSnapshot));
        if (allocationFails) return nullptr;
        auto *data = new OSData;
        memcpy(data->bytes, bytes, length);
        return data;
    }
};
struct Args {
    uint64_t *scalarInput = nullptr, *scalarOutput = nullptr;
    unsigned scalarInputCount = 0, scalarOutputCount = 0;
    void *structureInput = nullptr, *structureInputDescriptor = nullptr;
    void *structureOutputDescriptor = nullptr;
    OSData *structureOutput = nullptr;
    uint64_t structureOutputMaximumSize = 0;
};
struct State {
    bool pciOpen = true, stopping = false, shutdownBlocked = false, shutdownInProgress = false;
    struct {
        amdgpu::BringupStage reached = amdgpu::BringupStage::SDMAInit;
        amdgpu::SMUMetricsContext metrics{};
        int device = 0;
    } bringup;
};
struct Driver { State *ivars; };
static int lifecycle(Driver *driver, uint64_t selector) {
#include "metrics_allowlist_under_test.inc"
    return 0;
}
static int call(Driver *driver, uint64_t selector, Args *arguments) {
    const auto gate = lifecycle(driver, selector);
    if (gate) return gate;
    switch (selector) {
#include "metrics_rpc_under_test.inc"
    default: return kIOReturnUnsupported;
    }
}
int main() {
    State state; Driver driver{&state}; Args args;
    uint64_t out[3]{};
    args.scalarOutput = out; args.scalarOutputCount = 3;
    assert(call(&driver, 46, &args) == 0 && amdgpu::collections == 1 && amdgpu::lastReady);
    assert(out[0] == 0 && out[1] == 42 && out[2] == 5 && args.scalarOutputCount == 3);
    amdgpu::collectionResult = kIOReturnTimeout;
    assert(call(&driver, 46, &args) == 0 && out[0] == kIOReturnTimeout && !out[2]);
    const auto collected = amdgpu::collections;
    args.scalarInputCount = 1;
    assert(call(&driver, 46, &args) == kIOReturnBadArgument && amdgpu::collections == collected);
    args.scalarInputCount = 0; args.scalarOutputCount = 2;
    assert(call(&driver, 46, &args) == kIOReturnBadArgument);
    args.scalarOutputCount = 3; args.structureInput = &state;
    assert(call(&driver, 46, &args) == kIOReturnBadArgument);

    args = {}; args.structureOutputMaximumSize = sizeof(amdgpu::SMUMetricsSnapshot);
    assert(call(&driver, 47, &args) == 0 && args.structureOutput);
    assert(amdgpu::snapshots == 1 && amdgpu::collections == collected);
    delete args.structureOutput; args.structureOutput = nullptr;
    allocationFails = true;
    assert(call(&driver, 47, &args) == kIOReturnNoMemory && !args.structureOutput);
    allocationFails = false;
    args.structureOutputMaximumSize--;
    assert(call(&driver, 47, &args) == kIOReturnBadArgument);
    args.structureOutputMaximumSize++; args.structureOutputDescriptor = &state;
    assert(call(&driver, 47, &args) == kIOReturnBadArgument);
    args.structureOutputDescriptor = nullptr; args.scalarOutputCount = 1;
    assert(call(&driver, 47, &args) == kIOReturnBadArgument);
    args.scalarOutputCount = 0; args.scalarInputCount = 1;
    assert(call(&driver, 47, &args) == kIOReturnBadArgument);
    args.scalarInputCount = 0; args.structureInputDescriptor = &state;
    assert(call(&driver, 47, &args) == kIOReturnBadArgument);
    args.structureInputDescriptor = nullptr;

    for (unsigned mode = 0; mode < 3; ++mode) {
        state = {};
        if (mode == 0) state.shutdownBlocked = true;
        if (mode == 1) state.pciOpen = false;
        if (mode == 2) state.shutdownInProgress = true;
        assert(call(&driver, 47, &args) == 0 && !amdgpu::lastReady);
        amdgpu::SMUMetricsSnapshot s{};
        memcpy(&s, args.structureOutput->bytes, sizeof(s));
        delete args.structureOutput; args.structureOutput = nullptr;
        assert(s.validFields == 0 && s.status == kIOReturnNotReady);
        if (mode < 2) assert(lifecycle(&driver, 46) == kIOReturnNotReady);
    }
    assert(amdgpu::collections == collected); // All cached reads stayed CPU-only.
}
