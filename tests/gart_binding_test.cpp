#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "amdgpu_ip.h"
#include "amdgpu_vram.h"
#include "amdgpu_gart_allocator.h"
#include "amdgpu_gmc_address.h"
using namespace amdgpu;
using kern_return_t = int;
enum { kIOReturnSuccess, kIOReturnNotReady, kIOReturnBadArgument,
       kIOReturnNoSpace, kIOReturnNoMemory, kIOReturnNotAligned,
       kIOReturnBusy, kIOReturnIOError, kIOReturnTimeout, kIOReturnNotAttached };
constexpr unsigned kIOMemoryDirectionOutIn = 0,
    kIODMACommandSpecificationNoOptions = 0, kIODMACommandCreateNoOptions = 0,
    kIODMACommandPrepareForDMANoOptions = 0, kIODMACommandCompleteDMANoOptions = 0;
constexpr unsigned kIODMACommandPerformOperationOptionWrite = 2,
    kIODMACommandPerformOperationOptionRead = 1, kIODMACommandPerformOperationOptionZero = 4;
static std::vector<std::string> events;
static unsigned liveBuffers, liveDMA, writes;
static bool badReadback, badCPU, badLength, failPrepare;
static unsigned segments = 1;
static uint64_t segmentBytes = UINT64_MAX, segmentBase = 0x90000000;
static int completeResult, mmhubResult, gfxhubResult;
static unsigned performCount, failPerformAt;
struct IOAddressSegment { uint64_t address = 0, length = 0; };
struct IODMACommandSpecification { uint64_t options = 0, maxAddressBits = 0; };
struct IOBufferMemoryDescriptor {
    std::vector<uint8_t> bytes;
    static int Create(unsigned, uint64_t size, uint64_t, IOBufferMemoryDescriptor **out) {
        *out = new IOBufferMemoryDescriptor; (*out)->bytes.resize(size); ++liveBuffers; return 0;
    }
    int SetLength(uint64_t size) { assert(size == bytes.size()); return badLength ? kIOReturnIOError : 0; }
    int GetAddressRange(IOAddressSegment *out) {
        out->address = reinterpret_cast<uint64_t>(bytes.data()); out->length = bytes.size();
        return badCPU ? kIOReturnIOError : 0;
    }
    void release() { events.push_back("buffer-release"); --liveBuffers; delete this; }
};
static IOBufferMemoryDescriptor *preparedBuffer;
struct IODMACommand {
    static int Create(void *, unsigned, IODMACommandSpecification *spec, IODMACommand **out) {
        assert(spec->maxAddressBits == 48); *out = new IODMACommand; ++liveDMA; return 0;
    }
    int PrepareForDMA(unsigned, IOBufferMemoryDescriptor *buffer, uint64_t, uint64_t size,
                      uint64_t *, uint32_t *count, IOAddressSegment *out) {
        *count = segments; *out = {segmentBase, std::min(size, segmentBytes)};
        if (!failPrepare) preparedBuffer = buffer;
        return failPrepare ? kIOReturnIOError : 0;
    }
    int CompleteDMA(unsigned) { events.push_back("complete"); return completeResult; }
    int PerformOperation(unsigned option, uint64_t offset, uint64_t count,
                         uint64_t dataOffset, IOBufferMemoryDescriptor *data) {
        if (++performCount == failPerformAt) return kIOReturnIOError;
        assert(preparedBuffer && offset + count <= preparedBuffer->bytes.size());
        auto *mapped = preparedBuffer->bytes.data() + offset;
        if (option == kIODMACommandPerformOperationOptionZero) {
            assert(!data); std::memset(mapped, 0, count);
        } else {
            assert(data && dataOffset + count <= data->bytes.size());
            if (option == kIODMACommandPerformOperationOptionWrite)
                std::memcpy(mapped, data->bytes.data() + dataOffset, count);
            else {
                assert(option == kIODMACommandPerformOperationOptionRead);
                std::memcpy(data->bytes.data() + dataOffset, mapped, count);
            }
        }
        return 0;
    }
    void release() { events.push_back("dma-release"); --liveDMA; delete this; }
};
constexpr uint64_t kGMCGartPTVRAMOffset = 0x700000;
static uint64_t entries[512]{};
constexpr uint64_t testVRAMOffset = 0x1800000;
static uint8_t vramData[16384];
static uint8_t *barPointer(uint64_t offset) {
    if (offset >= testVRAMOffset && offset - testVRAMOffset < sizeof(vramData))
        return vramData + (offset - testVRAMOffset);
    assert(offset >= kGMCGartPTVRAMOffset && offset - kGMCGartPTVRAMOffset < sizeof(entries));
    return reinterpret_cast<uint8_t *>(entries) + (offset - kGMCGartPTVRAMOffset);
}
struct FakePCI {
    void MemoryWrite32(unsigned, uint64_t offset, uint32_t word) {
        std::memcpy(barPointer(offset), &word, 4);
        ++writes; events.push_back("write");
    }
    void MemoryRead32(unsigned, uint64_t offset, uint32_t *out) {
        std::memcpy(out, barPointer(offset), 4);
        if (badReadback) *out ^= 1;
        events.push_back("read");
    }
    void MemoryRead64(unsigned index, uint64_t offset, uint64_t *out) {
        uint32_t lo, hi; MemoryRead32(index, offset, &lo); MemoryRead32(index, offset+4, &hi);
        *out = uint64_t(hi) << 32 | lo;
    }
};
struct DeviceContext { FakePCI *pci; uint64_t bar0Size = 0x10000000; uint8_t bar0MemIndex = 0; };
struct GMCContext {
    bool inited = true;
    uint64_t vram_start = 0x8000000000, gart_pt_bus = 0x8000700000;
    uint64_t fb_start = 0x8000000000, fb_end = 0x87ffffffff, gart_end = 0xffff;
    uint64_t gart_start = 0, gart_size = 0x10000, gart_pt_size = 4096;
    GARTApertureAllocator gart_allocator;
    VRAMBumpAllocator vram_alloc;
    struct Hub { bool inited = true; bool gfx = false; } mmhub, gfxhub{true, true};
};
struct GARTContext;
#include "gart_context_under_test.inc"
#include "amdgpu_vram_io.h"
static void bar0_memcpy_to_vram(DeviceContext &dev, uint64_t offset, const void *src, uint64_t size) {
    assert(size == 8); uint32_t words[2]; std::memcpy(words, src, 8);
    dev.pci->MemoryWrite32(0, offset, words[0]); dev.pci->MemoryWrite32(0, offset+4, words[1]);
}
static void amdgpu_hdp_flush(DeviceContext &) { events.push_back("hdp"); }
static int gmc_flush_gpu_tlb(DeviceContext &, const GMCContext &, const GMCContext::Hub &hub, unsigned vmid, unsigned type) {
    assert(vmid == 0 && type == 0); events.push_back(hub.gfx ? "gfx" : "mm");
    return hub.gfx ? gfxhubResult : mmhubResult;
}
#define GART_LOG(...) do {} while (0)
#define GMC_LOG(...) do {} while (0)
static int windowStatus = 0;
static unsigned windowCalls = 0;
static int gmc_program_gart_window(DeviceContext &, GMCContext &) { ++windowCalls; return windowStatus; }
#include "gart_binding_under_test.inc"
#include "memory_test_context.inc"
struct SDMAInstance { bool inited = true, enabled = true; };
static unsigned copies, failCopyAt, corruptCopyAt;
static bool failUnbind;
static uint8_t *gpuPointer(uint64_t address) {
    constexpr uint64_t vramBase = 0x8000000000;
    if (address >= vramBase + testVRAMOffset && address < vramBase + testVRAMOffset + sizeof(vramData))
        return vramData + (address - vramBase - testVRAMOffset);
    assert(address / 4096 < 512);
    const auto pte = entries[address / 4096];
    assert((pte & PTEFlags::SYSMEM_RW) == PTEFlags::SYSMEM_RW);
    const auto bus = (pte & 0x0000fffffffff000ull) + address % 4096;
    assert(preparedBuffer && bus >= segmentBase && bus - segmentBase < preparedBuffer->bytes.size());
    return preparedBuffer->bytes.data() + (bus - segmentBase);
}
static int sdma_copy_linear_test(DeviceContext &, SDMAInstance &, uint64_t src,
                                 uint64_t dst, uint32_t bytes, uint64_t timeout) {
    assert(timeout == 100000 && bytes == sizeof(vramData));
    if (++copies == failCopyAt) return kIOReturnTimeout;
    for (unsigned i = 0; i < bytes; ++i) *gpuPointer(dst+i) = *gpuPointer(src+i);
    if (copies == corruptCopyAt) *gpuPointer(dst + bytes - 4) ^= 1;
    if (copies == 2 && failUnbind) gfxhubResult = kIOReturnTimeout;
    return 0;
}
#include "memory_transfer_under_test.inc"
static size_t event(const char *name) {
    auto found = std::find(events.begin(), events.end(), name);
    assert(found != events.end()); return found - events.begin();
}
int main() {
    FakePCI pci; DeviceContext dev{&pci}; GMCContext gmc; GARTContext gart{};
    uint64_t firmware = UINT64_MAX;
    assert(gmc_bind_existing(dev, gmc, 0x81000000, 16384, &firmware) == 0);
    assert(firmware == 0 && gmc.gart_allocator.bytes_used() == 16384);
    assert(gart_init(dev, gmc, gart) == 0 && !gart.reads_supported);
    GARTBinding external{};
    assert(gart_bind_existing(dev, gart, 0x82000000, 16384, &external) == 0);
    assert(external.gartMCAddr == 16384 && external.ready);
    for (unsigned i = 0; i < 4; ++i) {
        assert(entries[i] == ((0x81000000ull + i * 4096) | PTEFlags::SYSMEM_RW));
        assert(entries[4+i] == ((0x82000000ull + i * 4096) | PTEFlags::SYSMEM_RW));
    }
    uint64_t later = UINT64_MAX;
    assert(gmc_bind_existing(dev, gmc, 0x83000000, 16384, &later) == 0 && later == 32768);
    assert(gart_init(dev, gmc, gart) == 0);
    GARTBinding owned{};
    events.clear();
    assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) == 0);
    assert(owned.gartMCAddr == 49152 && gmc.gart_allocator.bytes_used() == gmc.gart_size && owned.ready);
    assert(event("read") < event("hdp") && event("hdp") < event("mm") && event("mm") < event("gfx"));
    const auto before = writes;
    assert(gart_bind_existing(dev, gart, external.busAddr, external.sizeBytes, &external) == 0);
    assert(writes == before); // idempotence does not rewrite a live mapping
    assert(gart_bind_existing(dev, gart, 0x84000000, external.sizeBytes, &external) == kIOReturnBusy);
    assert(gart_bind_existing(dev, gart, owned.busAddr, owned.sizeBytes, &owned) == kIOReturnBadArgument);
    GARTBinding full{};
    assert(gart_bind_existing(dev, gart, 0x84000000, 4096, &full) == kIOReturnNoSpace);
    assert(gmc_bind_existing(dev, gmc, 0x84000000, 16384, &later) == kIOReturnNoSpace);

    // Invalidate every PTE and both hubs before unpinning or freeing memory.
    events.clear();
    assert(gart_unbind(dev, gart, &owned) == 0);
    assert(!liveBuffers && !liveDMA && !owned.owner && gmc.gart_allocator.bytes_used() == 49152);
    assert(event("read") < event("hdp") && event("gfx") < event("complete"));
    assert(event("complete") < event("dma-release") && event("dma-release") < event("buffer-release"));
    for (unsigned i = 12; i < 16; ++i) assert(entries[i] == 0);
    assert(gart_unbind(dev, gart, &external) == 0);
    assert(gmc.gart_allocator.bytes_used() == 32768); // non-tail hole is reclaimed

    // Reuse a non-tail hole without touching either pinned firmware range.
    assert(gart_bind_existing(dev, gart, 0x85000000, 16384, &external) == 0);
    assert(external.gartOffset == 16384);
    auto stale = external;
    assert(gart_unbind(dev, gart, &external) == 0);
    assert(gart_bind_existing(dev, gart, 0x86000000, 16384, &external) == 0);
    events.clear();
    assert(gart_unbind(dev, gart, &stale) == kIOReturnBadArgument);
    assert(events.empty() && external.ready);
    for (unsigned i = 0; i < 4; ++i) {
        assert(entries[i] == ((0x81000000ull + i * 4096) | PTEFlags::SYSMEM_RW));
        assert(entries[8+i] == ((0x83000000ull + i * 4096) | PTEFlags::SYSMEM_RW));
    }
    assert(gart_unbind(dev, gart, &external) == 0);

    // Zero is a valid MC address; invalid sizes/addresses never mutate state.
    gmc.gart_allocator = {}; std::memset(entries, 0, sizeof(entries));
    assert(gart_init(dev, gmc, gart) == 0);
    for (uint64_t size : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
        GARTBinding invalid{}; events.clear();
        assert(gart_bind_existing(dev, gart, 0x81000000, size, &invalid) == kIOReturnBadArgument);
        assert(events.empty() && !invalid.owner && gmc.gart_allocator.bytes_used() == 0);
    }
    for (uint64_t base : {uint64_t(1), uint64_t(1) << 48, UINT64_MAX - 4095}) {
        GARTBinding invalid{};
        assert(gart_bind_existing(dev, gart, base, 4096, &invalid) == kIOReturnBadArgument);
    }
    assert(gart_bind_sysmem(dev, gart, UINT64_MAX, 16384, &owned) == kIOReturnBadArgument);
    assert(gart_bind_sysmem(dev, gart, 4096, 16385, &owned) == kIOReturnBadArgument);
    assert(gart_bind_existing(dev, gart, 0x81000000, 4096, &external) == 0 && external.gartMCAddr == 0);
    assert(gart_unbind(dev, gart, &external) == 0 && gmc.gart_allocator.bytes_used() == 0);

    // A 4 KiB external mapping must not misalign the next 16 KiB owned BO.
    assert(gart_bind_existing(dev, gart, 0x81000000, 4096, &external) == 0);
    assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) == 0);
    assert(owned.gartMCAddr == 16384);
    assert(gart_unbind(dev, gart, &owned) == 0);
    assert(gart_unbind(dev, gart, &external) == 0);

    // Every publication failure retains storage and the reservation. Failed
    // invalidation also retains the mapping; a successful retry then releases.
    for (unsigned failure = 0; failure < 3; ++failure) {
        badReadback = failure == 0;
        mmhubResult = failure == 1 ? kIOReturnTimeout : 0;
        gfxhubResult = failure == 2 ? kIOReturnTimeout : 0;
        assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) != 0);
        assert(owned.owner == &gart && !owned.ready && liveBuffers == 1 && liveDMA == 1);
        assert(gmc.gart_allocator.bytes_used() == 16384);
        assert(gart_unbind(dev, gart, &owned) != 0);
        assert(liveBuffers == 1 && liveDMA == 1 && gmc.gart_allocator.bytes_used() == 16384);
        badReadback = false; mmhubResult = gfxhubResult = 0;
        assert(gart_unbind(dev, gart, &owned) == 0);
        assert(!liveBuffers && !liveDMA && !gmc.gart_allocator.bytes_used());
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        segments = failure == 0 ? 2 : 1;
        segmentBytes = failure == 1 ? 4096 : UINT64_MAX;
        badCPU = failure == 2; badLength = failure == 3; failPrepare = failure == 4;
        assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) != 0);
        assert(!liveBuffers && !liveDMA && !owned.owner && !gmc.gart_allocator.bytes_used());
    }
    segments = 1; segmentBytes = UINT64_MAX; badCPU = badLength = failPrepare = false;
    assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) == 0);
    completeResult = kIOReturnIOError;
    assert(gart_unbind(dev, gart, &owned) == kIOReturnIOError);
    assert(liveBuffers == 1 && liveDMA == 1 && gmc.gart_allocator.bytes_used() == 16384);
    completeResult = 0;
    assert(gart_unbind(dev, gart, &owned) == 0);
    // Device isolation permits release even when MMIO cannot acknowledge.
    assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) == 0);
    badReadback = true; events.clear(); gart_release_after_reset(owned);
    assert(!liveBuffers && !liveDMA && !owned.owner);
    assert(std::find(events.begin(), events.end(), "read") == events.end());
    badReadback = false;
    // Run the production two-way test against simulated DMA and GPU page
    // translation. Every 4 KiB PTE is exercised in both 16 KiB regions.
    SDMAInstance sdma; MemoryTransferTest transfer{}; MemoryTransferResult result{};
    auto fresh = [&] {
        gmc.gart_allocator = {};
        gmc.vram_alloc.init(gmc.vram_start + testVRAMOffset, sizeof(vramData));
        std::memset(entries, 0, sizeof(entries));
        assert(gart_init(dev, gmc, gart) == 0);
        copies = failCopyAt = corruptCopyAt = performCount = failPerformAt = 0;
        failUnbind = false; mmhubResult = gfxhubResult = 0;
    };
    fresh();
    assert(memory_transfer_test(dev, gmc, gart, sdma, transfer, 0x12345678, result) == 0);
    assert(result.stage == 8 && result.mismatches == 0 && result.firstMismatch == UINT32_MAX);
    assert(copies == 4 && performCount == 3 && !transfer.active && gart.reads_supported);
    assert(!liveBuffers && !liveDMA && !gmc.gart_allocator.bytes_used() && !gmc.vram_alloc.bytes_used());
    assert(memory_transfer_test(dev, gmc, gart, sdma, transfer, 0x76543210, result) == 0);
    for (unsigned failure = 0; failure < 12; ++failure) {
        fresh();
        if (failure < 2) failCopyAt = failure + 1;
        else if (failure < 4) corruptCopyAt = failure - 1;
        else if (failure == 4) failUnbind = true;
        else if (failure < 8) failPerformAt = failure - 4;
        else if (failure < 10) failCopyAt = failure - 5;
        else corruptCopyAt = failure - 7;
        assert(memory_transfer_test(dev, gmc, gart, sdma, transfer, 0x98765432, result) != 0);
        assert(!gart.reads_supported);
        assert(transfer.active && liveBuffers == 2 && liveDMA == 1);
        assert(gmc.vram_alloc.bytes_used() == 16384 && gmc.gart_allocator.bytes_used() == 32768);
        const unsigned stages[] = {3,5,4,6,7,2,2,6,9,11,10,12};
        assert(result.stage == stages[failure]);
        if (failure == 2 || failure == 3) {
            assert(result.mismatches == 1 && result.firstMismatch == 16380);
            if (failure == 2) assert(copies == 1); // don't proceed past a bad GPU read
        }
        assert(memory_transfer_test(dev, gmc, gart, sdma, transfer, 7, result) == kIOReturnBusy);
        memory_transfer_release_after_reset(transfer);
        assert(!liveBuffers && !liveDMA && !transfer.active);
    }
    fresh();
    assert(gart_configure_host_window(dev, gart, 0) == kIOReturnBadArgument);
    assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) == 0);
    assert(gart_configure_host_window(dev, gart, 1ull << 36) == kIOReturnBusy && !windowCalls);
    assert(gart_unbind(dev, gart, &owned) == 0);
    gart.reads_supported = true;
    assert(gart_configure_host_window(dev, gart, 1ull << 36) == 0 && windowCalls == 1);
    assert(gart.hostWindowConfigured && !gart.reads_supported && gmc.gart_start == (1ull << 36));
    assert(gart.allocator->base == gart.gartStart && gmc.gart_end == gart.gartEnd);
    assert(gart_configure_host_window(dev, gart, 1ull << 37) == 0 && windowCalls == 1 && gart.gartStart == (1ull << 36));
    assert(gart_bind_sysmem(dev, gart, 16384, 16384, &owned) == 0 && owned.gartMCAddr == (1ull << 36));
    assert(gart_unbind(dev, gart, &owned) == 0);
    gart.hostWindowConfigured = false; windowStatus = kIOReturnTimeout;
    assert(gart_configure_host_window(dev, gart, 1ull << 37) == kIOReturnTimeout && gart.hostWindowConfigured);
    puts("GART: shared allocation, checked PTE ranges/publication, both-hub invalidation, DMA cleanup ordering and failure retention pass");
    puts("Host transfer: two 16 KiB patterns, every PTE, full readback, both copy failures, mismatches, API/cleanup failures and reset retention pass");
}
