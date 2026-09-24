#include "amdgpu_shared_buffers.h"
#include "amdgpu_bo_limits.h"
#include "amdgpu_vram.h"
#include "amdgpu_client_lifecycle.h"
#include <cassert>
#include <cstdio>
#include <vector>
using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNoResources = 2, kIOReturnBusy = 3, kIOReturnNotReady = 4, kIOReturnNoMemory = 5, kIOReturnNoSpace = 6, kIOReturnUnsupported = 7;
constexpr int kMacAMDGPUMethodBOAlloc = 16;
constexpr uint64_t MACAMDGPU_BO_ALIGN = 16384;
constexpr int kMacAMDGPUMethodBOExport = 52, kMacAMDGPUMethodBOImport = 53, kMacAMDGPUMethodBOFree = 17;
constexpr uint32_t kBODomainGTTLegacy = 0;
constexpr uint32_t kBODomainVRAM = 1, kBODomainGTT = 2, kBODomainDeviceVRAM = 3, MACAMDGPU_MAX_BO = amdgpu::kMaxClientBOs;
namespace amdgpu {
bool buffer_vram_domain(uint32_t domain) { return domain == 1 || domain == 3; }
struct GARTBinding { void *sysmemBuffer = nullptr, *dmaCommand = nullptr, *cpuAddr = nullptr;
    uint64_t busAddr = 0, gartMCAddr = 0, sizeBytes = 0; uint32_t numGPUPages = 0; };
struct GART { uint32_t numPTEs = 1; bool reads_supported = true; };
static bool partialBind;
int gart_bind_sysmem(int &, GART &, uint64_t, uint64_t, GARTBinding *binding) {
    if (partialBind) { binding->sysmemBuffer = reinterpret_cast<void *>(1); binding->numGPUPages = 4; }
    return kIOReturnNoSpace;
}
int gart_unbind(int &, GART &, GARTBinding *) { return 0; }
void gart_release_after_reset(GARTBinding &b) { b = {}; }
}
struct BOEntry {
    uint32_t sharedIndex = 0; bool in_use = false; uint32_t domain = 3;
    uint64_t gpu_va = 0, size = 16384, alignment = 16384, vram_offset = 0;
    uint32_t generation = 1; void *gtt_buf = nullptr, *gtt_dma = nullptr, *cpu_addr = nullptr;
    amdgpu::GARTBinding gttBinding{};
    uint64_t byte_offset = 0, gtt_bus_addr = 0;
};
struct MacAMDGPUUserClient_IVars { BOEntry *boPages[amdgpu::kMaxBOPages]{}; uint32_t boGenCounter = 1; uint64_t boBumpOffset = 0;
    void *dmaBuffer = nullptr; uint32_t dmaSegmentsCount = 0; uint64_t dmaBufferSize = 0;
    struct { uint64_t address = 0; } dmaSegments[1];
};
static bool allocationFails;
static unsigned tableAllocations, tableDeletes;
static BOEntry *newTable(size_t count) {
    assert(count == amdgpu::kBOEntriesPerPage);
    ++tableAllocations;
    return allocationFails ? nullptr : new BOEntry[count]{};
}
#define IONewZero(type, count) newTable(count)
#define IOSafeDeleteNULL(pointer, type, count) do { if (pointer) ++tableDeletes; delete[] pointer; pointer=nullptr; } while (0)
#include "bo_table_under_test.inc"
static unsigned pageCount(const MacAMDGPUUserClient_IVars &client) {
    unsigned count = 0; for (auto *page : client.boPages) if (page) ++count; return count;
}

static uint64_t mac_amdgpu_bo_gpu_addr(MacAMDGPUUserClient_IVars *, BOEntry *e) { return e->gpu_va; }
#define MACAMDGPU_LOG(...) do {} while (0)
struct Allocator : amdgpu::VRAMBumpAllocator {
    std::vector<uint64_t> freed;
    void free(const amdgpu::VRAMAllocation &a) { freed.push_back(a.gpu_va); VRAMBumpAllocator::free(a); }
};
struct State {
    amdgpu::SharedBuffers sharedBuffers;
    bool shutdownBlocked = false;
    struct {
        struct { uint64_t vram_start = 0x8000000000; Allocator vram_alloc, device_vram_alloc; } gmc;
        int device = 0; amdgpu::GART gart;
        struct {void *owner=nullptr;uint64_t ringHandle=0,metadataHandle=0;} aqlQueues[7];
    } bringup;
};
struct IOService {};
struct MacAMDGPU : IOService { State *ivars; };
#define OSDynamicCast(type, value) static_cast<type *>(value)
struct Args { uint64_t *scalarInput, *scalarOutput; uint32_t scalarInputCount, scalarOutputCount; };
static int call(MacAMDGPU *driver, MacAMDGPUUserClient_IVars *ivars, uint32_t selector, Args *arguments) {
    switch (selector) {
#include "bo_alloc_under_test.inc"
#include "shared_buffer_rpc_under_test.inc"
    default: return kIOReturnBadArgument;
    }
}
#include "shared_buffer_release_under_test.inc"
int main() {
    State state; MacAMDGPU driver; driver.ivars = &state;
    MacAMDGPUUserClient_IVars owner, first, second;
    assert(!pageCount(owner) && !pageCount(first) && !pageCount(second) && !tableAllocations);
    assert(!mac_amdgpu_bo_lookup(&owner, 1ull << 32));
    uint32_t slot;
    assert(mac_amdgpu_bo_find_free_slot(&owner, slot) == 0 && slot == 0);
    mac_amdgpu_bo_entry(&owner, 0)->in_use = true; mac_amdgpu_bo_entry(&owner, 0)->gpu_va = 0x8010000000;
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
    assert(call(&driver,&second,17,&args)==kIOReturnBusy && mac_amdgpu_bo_entry(&second, 0)->in_use);
    state.bringup.aqlQueues[0]={};
    assert(call(&driver, &second, 17, &args) == 0);
    assert(state.bringup.gmc.device_vram_alloc.freed == std::vector<uint64_t>{0x8010000000});
    assert(call(&driver, &second, 17, &args) == kIOReturnBadArgument);
    input[0] = firstToken; input[1] = secondToken; input[2] = 16384; args.scalarInputCount = 3;
    assert(call(&driver, &second, 53, &args) == kIOReturnBadArgument);
    mac_amdgpu_bo_release_all(&second, &driver);
    assert(!pageCount(owner) && !pageCount(first) && !pageCount(second) && tableDeletes == 3);

    // Exercise all 4096 real import slots and direct generation/index lookup.
    MacAMDGPUUserClient_IVars exporter, full;
    assert(mac_amdgpu_bo_find_free_slot(&exporter, slot) == 0);
    mac_amdgpu_bo_entry(&exporter, 0)->in_use = true; mac_amdgpu_bo_entry(&exporter, 0)->gpu_va = 0x8010000000;
    input[0] = 1ull << 32; input[1] = 71; input[2] = 72;
    assert(call(&driver, &exporter, 52, &args) == 0);
    const auto token0 = output[0], token1 = output[1];
    input[0] = token0; input[1] = token1; input[2] = 16384;
    allocationFails = true;
    const auto references = state.sharedBuffers.entries[mac_amdgpu_bo_entry(&exporter, 0)->sharedIndex - 1].references;
    assert(call(&driver, &full, 53, &args) == kIOReturnNoMemory && !pageCount(full) && full.boGenCounter == 1);
    assert(state.sharedBuffers.entries[mac_amdgpu_bo_entry(&exporter, 0)->sharedIndex - 1].references == references);
    allocationFails = false;
    auto &exportRecord = state.sharedBuffers.entries[mac_amdgpu_bo_entry(&exporter, 0)->sharedIndex - 1];
    exportRecord.references = UINT32_MAX;
    assert(call(&driver, &full, 53, &args) == kIOReturnNoResources && !pageCount(full));
    assert(full.boGenCounter == 1 && exportRecord.references == UINT32_MAX);
    exportRecord.references = references;
    std::vector<uint64_t> handles;
    for (uint32_t i = 0; i < MACAMDGPU_MAX_BO; ++i) {
        assert(call(&driver, &full, 53, &args) == 0);
        handles.push_back(output[0]);
        assert(mac_amdgpu_bo_handle_index(output[0]) == i);
        assert(mac_amdgpu_bo_lookup(&full, output[0]) == mac_amdgpu_bo_entry(&full, i));
        assert(pageCount(full) == (i / amdgpu::kBOEntriesPerPage) + 1);
    }
    assert(pageCount(full) == amdgpu::kMaxBOPages);
    auto *stable = mac_amdgpu_bo_lookup(&full, handles[128]);
    const auto generation = full.boGenCounter;
    assert(call(&driver, &full, 53, &args) == kIOReturnNoResources && full.boGenCounter == generation);
    assert(state.sharedBuffers.entries[mac_amdgpu_bo_entry(&exporter, 0)->sharedIndex - 1].references == MACAMDGPU_MAX_BO + 1);
    assert(!mac_amdgpu_bo_lookup(&full, (uint64_t(generation) << 32) | MACAMDGPU_MAX_BO));
    input[0] = handles[2048]; args.scalarInputCount = 1;
    assert(call(&driver, &full, 17, &args) == 0 && !mac_amdgpu_bo_lookup(&full, handles[2048]));
    input[0] = token0; input[1] = token1; input[2] = 16384; args.scalarInputCount = 3;
    assert(call(&driver, &full, 53, &args) == 0 && uint32_t(output[0]) == 2048);
    assert(output[0] != handles[2048] && !mac_amdgpu_bo_lookup(&full, handles[2048]));
    // Free a complete middle page; an adjacent live page/entry must not move.
    const auto deletesBefore = tableDeletes;
    for (uint32_t i = 64; i < 128; ++i) {
        input[0] = handles[i]; args.scalarInputCount = 1;
        assert(call(&driver, &full, 17, &args) == 0);
        assert(mac_amdgpu_bo_lookup(&full, handles[128]) == stable);
        assert(pageCount(full) == amdgpu::kMaxBOPages - (i == 127 ? 1 : 0));
    }
    assert(tableDeletes == deletesBefore + 1 && !full.boPages[1]);
    input[0] = token0; input[1] = token1; input[2] = 16384; args.scalarInputCount = 3;
    allocationFails = true;
    const auto generationBeforeGrowth = full.boGenCounter;
    assert(call(&driver, &full, 53, &args) == kIOReturnNoMemory && !full.boPages[1]);
    assert(full.boGenCounter == generationBeforeGrowth && mac_amdgpu_bo_lookup(&full, handles[128]) == stable);
    allocationFails = false;
    assert(call(&driver, &full, 53, &args) == 0 && uint32_t(output[0]) == 64);
    assert(!mac_amdgpu_bo_lookup(&full, handles[64]) && mac_amdgpu_bo_lookup(&full, handles[128]) == stable);
    const auto pagesBeforeReuse = pageCount(full);
    assert(call(&driver, &full, 53, &args) == 0 && uint32_t(output[0]) == 65 && pageCount(full) == pagesBeforeReuse);
    const auto oldHandle = output[0];
    mac_amdgpu_bo_release_all(&full, &driver);
    assert(!pageCount(full) && !mac_amdgpu_bo_lookup(&full, oldHandle));
    assert(call(&driver, &full, 53, &args) == 0 && (output[0] >> 32) > generation);
    assert(!mac_amdgpu_bo_lookup(&full, handles[0])); // Recreated table does not reset generation.
    full.boGenCounter = UINT32_MAX;
    const auto heldRefs = state.sharedBuffers.entries[mac_amdgpu_bo_entry(&exporter, 0)->sharedIndex - 1].references;
    assert(call(&driver, &full, 53, &args) == kIOReturnNoResources && full.boGenCounter == UINT32_MAX);
    assert(state.sharedBuffers.entries[mac_amdgpu_bo_entry(&exporter, 0)->sharedIndex - 1].references == heldRefs);
    mac_amdgpu_bo_release_all(&full, &driver);
    mac_amdgpu_bo_release_all(&exporter, &driver);
    assert(!pageCount(full) && !pageCount(exporter));
    // Actual allocation RPC: failure rolls back the slot, while a partially
    // published GTT mapping keeps its slot/storage until reset-only cleanup.
    MacAMDGPUUserClient_IVars allocating;
    uint64_t allocateInput[4]{16384, kBODomainVRAM, 16384, 0};
    Args allocArgs{allocateInput, output, 4, 3};
    state.bringup.gmc.vram_alloc.init(0x8000000000, 2 * 16384);
    allocationFails = true;
    assert(call(&driver, &allocating, 16, &allocArgs) == kIOReturnNoMemory && !pageCount(allocating));
    allocationFails = false;
    allocateInput[0] = 3 * 16384;
    assert(call(&driver, &allocating, 16, &allocArgs) == kIOReturnNoSpace && !mac_amdgpu_bo_entry(&allocating, 0));
    const auto failedGeneration = allocating.boGenCounter;
    allocateInput[0] = 16384;
    assert(call(&driver, &allocating, 16, &allocArgs) == 0 && uint32_t(output[0]) == 0);
    assert((output[0] >> 32) > failedGeneration);
    allocateInput[1] = kBODomainGTT;
    assert(call(&driver, &allocating, 16, &allocArgs) == kIOReturnNoSpace && !mac_amdgpu_bo_entry(&allocating, 1)->in_use);
    amdgpu::partialBind = true;
    assert(call(&driver, &allocating, 16, &allocArgs) == kIOReturnNoSpace);
    assert(mac_amdgpu_bo_entry(&allocating, 1)->in_use && mac_amdgpu_bo_entry(&allocating, 1)->gttBinding.sysmemBuffer && state.shutdownBlocked);
    assert(mac_amdgpu_bo_find_free_slot(&allocating, slot) == 0 && slot == 2);
    mac_amdgpu_bo_release_all(&allocating, &driver);
    assert(!pageCount(allocating) && state.bringup.gmc.vram_alloc.bytes_used() == 0);
    // Collision, table exhaustion and refcount overflow never steal a reference.
    amdgpu::SharedBuffers table;
    assert(!table.publish(0, 0, 1, 1, 1));
    for (uint32_t i = 0; i < table.capacity; ++i) assert(table.publish(i + 1, 7, 1, 1, 1) == i + 1);
    assert(!table.publish(1, 7, 1, 1, 1) && !table.publish(999, 7, 1, 1, 1));
    table.entries[0].references = UINT32_MAX;
    assert(!table.retain(1));
    puts("Shared GPU buffers: production export/import/free/close paths, stale tokens, balanced references and exhaustion pass");
}
