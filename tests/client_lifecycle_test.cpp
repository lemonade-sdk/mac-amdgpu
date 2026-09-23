#include <cassert>
#include <cstdio>
#include <cstdint>
#include "../dext/amdgpu/amdgpu_client_lifecycle.h"

using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBusy = 1,
              kIOReturnBadArgument = 2, kIOReturnUnsupported = 3;
enum { kMacAMDGPUMethodRuntimeBuild, kMacAMDGPUMethodPing,
       kMacAMDGPUMethodQueryInfo, kMacAMDGPUMethodShutdownGPU,
       kMacAMDGPUMethodWaitFence, kMacAMDGPUMethodBOGetInfo,
       kMacAMDGPUMethodBOFree, kMacAMDGPUMethodSubmitIB, kMacAMDGPUMethodMESAddQueue };
struct IOService {};
struct IOPCIDevice {
    uint8_t head = 0;
    uint16_t headers[256] = {};
    unsigned reads = 0;
    void ConfigurationRead8(uint32_t reg, uint8_t *out) { assert(reg == 0x34); *out = head; }
    void ConfigurationRead16(uint32_t reg, uint16_t *out) {
        assert(reg < 256); ++reads; *out = headers[reg];
    }
};
#include "client_pm_cap_under_test.inc"
struct State { IOService *owner = nullptr; amdgpu::ClientSubmission submission; };
struct MacAMDGPU { State *ivars; };
static unsigned openCalls;
static int mac_amdgpu_ensure_open(IOService *client, MacAMDGPU *driver, IOPCIDevice *) {
    ++openCalls;
    if (driver->ivars->owner && driver->ivars->owner != client) return kIOReturnBusy;
    driver->ivars->owner = client;
    return 0;
}
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

    State state; MacAMDGPU driver{&state}; IOPCIDevice pci;
    IOService owner, observer;
    auto call = [&](IOService &client, uint64_t selector) {
        return mac_amdgpu_admit_external(&client, &driver, &pci, selector);
    };
    assert(call(observer, kMacAMDGPUMethodMESAddQueue) == kIOReturnUnsupported && openCalls == 0);
    assert(call(observer, kMacAMDGPUMethodRuntimeBuild) == 0 && openCalls == 0);
    assert(call(owner, kMacAMDGPUMethodBOFree) == 0 && state.owner == &owner);
    assert(call(observer, kMacAMDGPUMethodSubmitIB) == kIOReturnBusy);
    unsigned before = openCalls;
    assert(call(observer, kMacAMDGPUMethodQueryInfo) == 0 && openCalls == before);

    volatile uint32_t fence = 0;
    const auto first = state.submission.beginSDMA(&fence);
    assert(first == 1 && !state.submission.poll());
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
    puts("Client lifecycle: checked allocation, timeout cap, observer/owner admission, pending-work gate and unique latched fences pass");
}
