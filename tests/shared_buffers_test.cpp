#include "amdgpu_shared_buffers.h"
#include <cassert>
#include <cstdio>
#include <vector>
using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNoResources = 2, kIOReturnBusy = 3;
constexpr int kMacAMDGPUMethodBOExport = 52, kMacAMDGPUMethodBOImport = 53, kMacAMDGPUMethodBOFree = 17;
constexpr uint32_t kBODomainVRAM = 1, kBODomainGTT = 2, kBODomainDeviceVRAM = 3, MACAMDGPU_MAX_BO = 64;
namespace amdgpu {
struct VRAMAllocation { uint64_t gpu_va, size, alignment; void *cpu_ptr; };
bool buffer_vram_domain(uint32_t domain) { return domain == 1 || domain == 3; }
int gart_unbind(int &, int &, int *) { return 0; }
void gart_release_after_reset(int &) {}
}
struct BOEntry {
    uint32_t sharedIndex = 0; bool in_use = false; uint32_t domain = 3;
    uint64_t gpu_va = 0, size = 16384, alignment = 16384, vram_offset = 0;
    uint32_t generation = 1; void *gtt_buf = nullptr, *gtt_dma = nullptr, *cpu_addr = nullptr;
    int gttBinding = 0;
};
struct MacAMDGPUUserClient_IVars { BOEntry bos[MACAMDGPU_MAX_BO]; uint32_t boGenCounter = 1; uint64_t boBumpOffset = 0; };
static uint64_t mac_amdgpu_bo_make_handle(uint32_t gen, uint32_t index) { return uint64_t(gen) << 32 | index; }
static BOEntry *mac_amdgpu_bo_lookup(MacAMDGPUUserClient_IVars *client, uint64_t handle) {
    const auto slot = uint32_t(handle);
    if (slot >= MACAMDGPU_MAX_BO) return nullptr;
    auto &entry = client->bos[slot];
    return entry.in_use && entry.generation == handle >> 32 ? &entry : nullptr;
}
struct Allocator {
    std::vector<uint64_t> freed;
    void free(const amdgpu::VRAMAllocation &a) { freed.push_back(a.gpu_va); }
};
struct State {
    amdgpu::SharedBuffers sharedBuffers;
    bool shutdownBlocked = false;
    struct {
        struct { uint64_t vram_start = 0x8000000000; Allocator vram_alloc, device_vram_alloc; } gmc;
        int device = 0, gart = 0;
        struct {void *owner=nullptr;uint64_t ringHandle=0,metadataHandle=0;} aqlQueues[7];
    } bringup;
};
struct IOService {};
struct MacAMDGPU : IOService { State *ivars; };
#define OSDynamicCast(type, value) static_cast<type *>(value)
struct Args { uint64_t *scalarInput, *scalarOutput; uint32_t scalarInputCount, scalarOutputCount; };
static int call(MacAMDGPU *driver, MacAMDGPUUserClient_IVars *ivars, uint32_t selector, Args *arguments) {
    switch (selector) {
#include "shared_buffer_rpc_under_test.inc"
    default: return kIOReturnBadArgument;
    }
}
#include "shared_buffer_release_under_test.inc"
int main() {
    State state; MacAMDGPU driver; driver.ivars = &state;
    MacAMDGPUUserClient_IVars owner, first, second;
    owner.bos[0].in_use = true; owner.bos[0].gpu_va = 0x8010000000;
    uint64_t input[3]{1ull << 32, 0x12345678, 0x98765432}, output[3]{};
    Args args{input, output, 3, 3};
    assert(call(&driver, &owner, 52, &args) == 0 && output[2] == 16384);
    const uint64_t firstToken = output[0], secondToken = output[1];
    input[1] = 42; input[2] = 99;
    assert(call(&driver, &owner, 52, &args) == 0 && output[0] == firstToken && output[1] == secondToken);
    input[0] = firstToken; input[1] = secondToken; input[2] = 16385;
    assert(call(&driver, &first, 53, &args) == kIOReturnBadArgument);
    input[2] = 16384;
    assert(call(&driver, &first, 53, &args) == 0 && output[1] == 0x8010000000);
    const uint64_t firstHandle = output[0];
    assert(call(&driver, &second, 53, &args) == 0);
    const uint64_t secondHandle = output[0];
    // Closing the exporter or a quarantined owner's eventual cleanup does not
    // free imported VRAM. Only the last actual BO reference releases it.
    mac_amdgpu_bo_release_all(&owner, &driver);
    assert(state.bringup.gmc.device_vram_alloc.freed.empty());
    mac_amdgpu_bo_release_all(&first, &driver);
    assert(!mac_amdgpu_bo_lookup(&first, firstHandle));
    assert(state.bringup.gmc.device_vram_alloc.freed.empty());
    input[0] = secondHandle; args.scalarInputCount = 1;
    state.bringup.aqlQueues[0]={&second,secondHandle,0};
    assert(call(&driver,&second,17,&args)==kIOReturnBusy && second.bos[0].in_use);
    state.bringup.aqlQueues[0]={};
    assert(call(&driver, &second, 17, &args) == 0);
    assert(state.bringup.gmc.device_vram_alloc.freed == std::vector<uint64_t>{0x8010000000});
    assert(call(&driver, &second, 17, &args) == kIOReturnBadArgument);
    input[0] = firstToken; input[1] = secondToken; input[2] = 16384; args.scalarInputCount = 3;
    assert(call(&driver, &second, 53, &args) == kIOReturnBadArgument);
    // Collision, table exhaustion and refcount overflow never steal a reference.
    amdgpu::SharedBuffers table;
    assert(!table.publish(0, 0, 1, 1, 1));
    for (uint32_t i = 0; i < table.capacity; ++i) assert(table.publish(i + 1, 7, 1, 1, 1) == i + 1);
    assert(!table.publish(1, 7, 1, 1, 1) && !table.publish(999, 7, 1, 1, 1));
    table.entries[0].references = UINT32_MAX;
    assert(!table.retain(1));
    puts("Shared GPU buffers: production export/import/free/close paths, stale tokens, balanced references and exhaustion pass");
}
