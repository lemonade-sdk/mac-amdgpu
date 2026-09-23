#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "amdgpu_sdma_packets.h"
#include "amdgpu_vram.h"
#include "amdgpu_client_lifecycle.h"

namespace linux_sdma {
#include "../upstream/linux/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h"
}
using namespace amdgpu;
using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = 1,
    kIOReturnBadArgument = 2, kIOReturnNoSpace = 3, kIOReturnTimeout = 4,
    kIOReturnNotAttached = 5, kIOReturnIOError = 6, kIOReturnNoMemory = 7;
struct FakePCI {
    alignas(8) uint8_t vram[0x20000]{};
    bool removed = false, dropWrites = false;
    uint64_t doorbellOffset = 0, doorbellValue = 0;
    unsigned kicks = 0;
    void MemoryWrite32(unsigned bar, uint64_t offset, uint32_t value) {
        assert(bar == 0 && offset + 4 <= sizeof(vram));
        if (!dropWrites) memcpy(vram + offset, &value, 4);
    }
    void MemoryRead32(unsigned bar, uint64_t offset, uint32_t *value) {
        assert(bar == 0 && offset + 4 <= sizeof(vram));
        if (removed) *value = UINT32_MAX;
        else memcpy(value, vram + offset, 4);
    }
    void MemoryRead64(unsigned bar, uint64_t offset, uint64_t *value) {
        assert(bar == 0 && offset + 8 <= sizeof(vram));
        if (removed) *value = UINT64_MAX;
        else memcpy(value, vram + offset, 8);
    }
    void MemoryWrite64(unsigned bar, uint64_t offset, uint64_t value) {
        assert(bar == 1); ++kicks; doorbellOffset = offset; doorbellValue = value;
    }
};
enum class IPBlock { GC };
struct DeviceContext {
    FakePCI *pci;
    unsigned bar0MemIndex = 0, bar2MemIndex = 1;
    uint64_t bar0Size = 0x20000, bar2Size = 0x200000;
    bool doorbell_works = false;
    struct { bool isResolved(IPBlock) const { return true; } } ip;
    struct { struct { unsigned sdma_engine[2] = {0x100,0x10a}; } index; } doorbell;
};
#include "amdgpu_vram_io.h"
constexpr uint64_t kASPageSize = 16384;
constexpr unsigned kSDMARingDefaultBytes = 16384, kSDMAWBPageBytes = 16384;
struct SDMAInstance {
    unsigned instance = 0;
    bool inited = false, enabled = false;
    uint64_t ring_gpu_va = 0, ring_vram_off = 0;
    unsigned ring_size_dwords = 0, ring_ptr_mask = 0;
    uint64_t wb_bus = 0, wb_vram_off = 0;
    const DeviceContext *wb_device = nullptr;
    uint32_t cs_fence_shadow = 0;
    uint64_t rptr_gpu_addr = 0, wptr_poll_gpu_addr = 0;
    unsigned wptr = 0, doorbell_index = 0;
};
struct GMCContext { VRAMBumpAllocator vram_alloc; uint64_t vram_start = 0x8000000000; };
static void amdgpu_hdp_flush(const DeviceContext &) {}
struct Regs { unsigned QUEUE0_RB_WPTR = 10, QUEUE0_RB_WPTR_HI = 11; };
static Regs sdma_regs(const DeviceContext &) { return {}; }
static unsigned sdma_reg_offset(const DeviceContext &, unsigned instance, unsigned offset) { return instance * 100 + offset; }
static unsigned mmioWrites;
static void WREG32(const DeviceContext &, unsigned, uint32_t) { ++mmioWrites; }
#define SDMA_LOG(...) do {} while (0)
#include "sdma_allocation_under_test.inc"
#include "sdma_wb_under_test.inc"
static uint32_t emitted[12];
static unsigned emitted_count;
static uint64_t time_ns;
static volatile uint32_t *test_fence;
static bool complete_fence;
static uint32_t sdma_ring_write(const DeviceContext &, SDMAInstance &,
                                const uint32_t *pkt, uint32_t n) {
    assert(n == 12 || n == 4);
    memcpy(emitted, pkt, n * 4);
    emitted_count = n;
    return n;
}
static uint64_t test_clock(int) { return time_ns; }
static void IOSleep(unsigned ms) {
    time_ns += uint64_t(ms) * 1000000;
    if (complete_fence && time_ns >= 3000000) *test_fence = emitted[emitted_count-1];
}
#define SDMA_LOG(...) do {} while (0)
#define clock_gettime_nsec_np test_clock
#define CLOCK_UPTIME_RAW 0
#include "sdma_copy_under_test.inc"
#undef clock_gettime_nsec_np

int main() {
    assert(SDMA_PKT_HEADER_CPV(1) == SDMA_PKT_COPY_LINEAR_HEADER_CPV(1));
    assert(sdma_fence_header() == (SDMA_PKT_FENCE_HEADER_OP(5) | SDMA_PKT_FENCE_HEADER_MTYPE(3)));
    assert(sdma_doorbell_byte_offset(0x200) == 0x800);
    FakePCI pci;
    DeviceContext dev{};
    dev.pci = &pci;
    GMCContext gmc;
    gmc.vram_alloc.init(gmc.vram_start + 0x4000, 0x10000);
    SDMAInstance inst;
    assert(sdma_alloc_storage(dev, inst, gmc) == kIOReturnSuccess);
    assert(inst.ring_gpu_va == gmc.vram_start + 0x4000);
    assert(inst.wb_bus == gmc.vram_start + 0x8000);
    assert(inst.wb_vram_off == 0x8000 && inst.rptr_gpu_addr == inst.wb_bus);
    assert(inst.wptr_poll_gpu_addr == inst.wb_bus + 0x40);
    assert(inst.wb_device == &dev && inst.inited);
    SDMAInstance inst1;
    inst1.instance = 1;
    assert(sdma_alloc_storage(dev, inst1, gmc) == 0);
    assert(inst1.wb_bus != inst.wb_bus && inst1.doorbell_index == 0x214);
    inst1.enabled = true;
    inst.enabled = true;
    test_fence = reinterpret_cast<volatile uint32_t *>(pci.vram + inst.wb_vram_off + 0x80);
    for (bool complete : {false, true}) {
        complete_fence = complete;
        time_ns = 0;
        auto r = sdma_copy_linear_test(dev, inst, 0x800180c000ull, 0x8001810000ull, 4096, 100000);
        assert(r == (complete ? kIOReturnSuccess : kIOReturnTimeout));
        assert(time_ns == (complete ? 3000000 : 100000000));
        assert(emitted_count == 12);
        const uint32_t expected[] = {
            SDMA_PKT_COPY_LINEAR_HEADER_OP(1) | SDMA_PKT_COPY_LINEAR_HEADER_SUB_OP(0) | SDMA_PKT_COPY_LINEAR_HEADER_CPV(1),
            4095, 0, 0x180c000, 0x80, 0x1810000, 0x80, 0,
            SDMA_PKT_FENCE_HEADER_OP(5) | SDMA_PKT_FENCE_HEADER_MTYPE(3),
            0x8080, 0x80, 0xDEC0FFEE
        };
        assert(memcmp(emitted, expected, sizeof(expected)) == 0);
    }
    for (bool complete : {false,true}) {
        complete_fence=complete; time_ns=0;
        auto r=sdma_ring_test(dev,inst,100000);
        assert(r==(complete?kIOReturnSuccess:kIOReturnTimeout));
        assert(time_ns==(complete?3000000:100000000));
        assert(emitted_count==4 && emitted[3]==0xCAFEC0DEu);
    }
    emitted_count = 0;
    assert(sdma_copy_linear_test(dev, inst, 0, 0, 0, 100000) == kIOReturnBadArgument);
    assert(sdma_copy_linear_test(dev, inst, 0, 0, kSDMACopyLinearMaxBytes + 1, 100000) == kIOReturnBadArgument);
    assert(emitted_count == 0);
    uint32_t *rptr = reinterpret_cast<uint32_t *>(pci.vram + inst.wb_vram_off);
    uint32_t *otherFence = reinterpret_cast<uint32_t *>(pci.vram + inst.wb_vram_off + 0xC0);
    *rptr = 12; *otherFence = 77;
    assert(sdma_clear_fence(dev, inst, 0x80) == 0);
    assert(*rptr == 12 && *otherFence == 77);
    test_fence = reinterpret_cast<uint32_t *>(pci.vram + inst1.wb_vram_off + 0x80);
    time_ns = 0; complete_fence = true;
    assert(sdma_ring_test(dev, inst1, 100000) == 0);
    assert(emitted[1] == uint32_t(inst1.wb_bus + 0x80) && emitted[2] == 0x80);
    test_fence = reinterpret_cast<uint32_t *>(pci.vram + inst.wb_vram_off + 0x80);
    // Publish the byte WPTR in GPU memory before ringing the BAR2 doorbell.
    inst.wptr = 37;
    mmioWrites = 0;
    assert(sdma_kick_doorbell(dev, inst) == 0);
    uint64_t shadow = 0;
    memcpy(&shadow, pci.vram + inst.wb_vram_off + 0x40, 8);
    assert(shadow == 148 && pci.doorbellValue == 148 && pci.doorbellOffset == 0x800);
    assert(mmioWrites == 2);
    dev.doorbell_works = true; mmioWrites = 0;
    assert(sdma_kick_doorbell(dev, inst) == 0 && mmioWrites == 0);
    const auto kicks = pci.kicks;
    pci.dropWrites = true; ++inst.wptr;
    assert(sdma_kick_doorbell(dev, inst) == kIOReturnIOError && pci.kicks == kicks);
    *test_fence = 9;
    emitted_count = 0;
    assert(sdma_ring_test(dev, inst, 100000) == kIOReturnIOError);
    assert(emitted_count == 0 && pci.kicks == kicks);
    pci.dropWrites = false;

    // Exercise the production CS callback through the shared submission latch.
    ClientSubmission submission;
    assert(sdma_clear_fence(dev, inst, 0xC0) == 0);
    const auto first = submission.beginSDMA(&inst.cs_fence_shadow, sdma_read_cs_fence, &inst);
    assert(first == 1 && !submission.poll());
    uint32_t *csFence = reinterpret_cast<uint32_t *>(pci.vram + inst.wb_vram_off + 0xC0);
    *csFence = first;
    pci.removed = true;
    assert(!submission.poll() && submission.completed == 0);
    pci.removed = false;
    assert(submission.poll() && submission.completed == first);
    assert(sdma_clear_fence(dev, inst, 0xC0) == 0);
    const auto second = submission.beginSDMA(&inst.cs_fence_shadow, sdma_read_cs_fence, &inst);
    *csFence = first;
    assert(!submission.poll() && submission.completed == first);
    *csFence = second;
    assert(submission.poll() && submission.completed == second);
    submission.issued = UINT32_MAX - 1;
    assert(submission.beginSDMA(&inst.cs_fence_shadow, sdma_read_cs_fence, &inst) == 0);
    pci.removed = true;
    uint32_t observed = 0;
    assert(sdma_read_fence(dev, inst, 0x80, &observed) == kIOReturnNotAttached);
    pci.removed = false;
    assert(sdma_read_fence(dev, inst, 0x40, &observed) == kIOReturnBadArgument);

    // Pre-publication allocation failures return their VRAM spans.
    GMCContext failing;
    failing.vram_alloc.init(failing.vram_start + 0x10000, 0x8000);
    SDMAInstance other;
    pci.removed = true;
    assert(sdma_alloc_storage(dev, other, failing) == kIOReturnIOError);
    assert(!other.inited && failing.vram_alloc.bytes_used() == 0);
    pci.removed = false;
    failing.vram_alloc.init(failing.vram_start + 0x10000, 0x4000);
    assert(sdma_alloc_storage(dev, other, failing) == kIOReturnNoMemory);
    assert(!other.inited && failing.vram_alloc.bytes_used() == 0);
    puts("SDMA: Linux packets, both VRAM WB pages, publication, CS callbacks, stale/removal rejection and timeout pass");
}
