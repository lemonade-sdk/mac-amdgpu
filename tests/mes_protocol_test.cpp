#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <cstdlib>
#include "amdgpu_vram.h"
#include "amdgpu_mes_packets.h"
#include "amdgpu_mes_registers.h"
#include "amdgpu_mes_resources.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/mes_v12_api_def.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h"

using namespace amdgpu;
#include "mes_register_checks.inc"

static_assert(sizeof(MES_QueryStatus) == sizeof(MESAPI__QUERY_MES_STATUS));
static_assert(offsetof(MES_QueryStatus, api_status) == offsetof(MESAPI__QUERY_MES_STATUS, api_status));
static_assert(offsetof(MES_QueryStatus, timestamp) == offsetof(MESAPI__QUERY_MES_STATUS, timestamp));
static_assert(sizeof(MES_RemoveQueue) == sizeof(MESAPI__REMOVE_QUEUE));
#define CHECK_REMOVE(field) static_assert(offsetof(MES_RemoveQueue, field) == offsetof(MESAPI__REMOVE_QUEUE, field))
CHECK_REMOVE(doorbell_offset);
CHECK_REMOVE(gang_context_addr);
CHECK_REMOVE(api_status);
CHECK_REMOVE(pipe_id);
CHECK_REMOVE(queue_id);
CHECK_REMOVE(tf_addr);
CHECK_REMOVE(tf_data);
CHECK_REMOVE(queue_type);
CHECK_REMOVE(timestamp);
CHECK_REMOVE(gang_context_array_index);
#undef CHECK_REMOVE
static_assert(sizeof(MES_SetHwResources1) == sizeof(MESAPI_SET_HW_RESOURCES_1));
static_assert(sizeof(MES_SetHwResources) == sizeof(MESAPI_SET_HW_RESOURCES));
static_assert(offsetof(MES_SetHwResources, gc_base) == offsetof(MESAPI_SET_HW_RESOURCES, gc_base));
static_assert(offsetof(MES_SetHwResources, mmhub_base) == offsetof(MESAPI_SET_HW_RESOURCES, mmhub_base));
static_assert(offsetof(MES_SetHwResources, osssys_base) == offsetof(MESAPI_SET_HW_RESOURCES, osssys_base));
static_assert(offsetof(MES_SetHwResources, api_status) == offsetof(MESAPI_SET_HW_RESOURCES, api_status));
#define CHECK_R1(field) static_assert(offsetof(MES_SetHwResources1, field) == offsetof(MESAPI_SET_HW_RESOURCES_1, field))
CHECK_R1(api_status);
CHECK_R1(timestamp);
CHECK_R1(mes_debug_ctx_mc_addr);
CHECK_R1(mes_debug_ctx_size);
CHECK_R1(mes_kiq_unmap_timeout);
CHECK_R1(coop_sch_shared_mc_addr);
CHECK_R1(cleaner_shader_fence_mc_addr);

using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = 1, kIOReturnBadArgument = 2,
    kIOReturnNoSpace = 3, kIOReturnNotAttached = 4, kIOReturnInternalError = 5,
    kIOReturnTimeout = 6, kIOReturnIOError = 7, kIOReturnBusy = 8,
    kIOReturnNoMemory = 9;
constexpr unsigned kMaxMESPipes = 2;
enum class MESPipe : uint32_t { Sched, KIQ };
struct FakePCI {
    alignas(8) uint8_t vram[0x40000]{};
    uint64_t offset = 0, value = 0;
    unsigned doorbells = 0, writes = 0;
    bool dropWrites = false, removed = false;
    void MemoryWrite32(uint32_t bar, uint64_t o, uint32_t v) {
        assert(bar == 0 && o + 4 <= sizeof(vram));
        ++writes;
        if (!dropWrites) memcpy(vram + o, &v, 4);
    }
    void MemoryRead32(uint32_t bar, uint64_t o, uint32_t *v) {
        assert(bar == 0 && o + 4 <= sizeof(vram));
        if (removed) *v = UINT32_MAX;
        else memcpy(v, vram + o, 4);
    }
    void MemoryRead64(uint32_t bar, uint64_t o, uint64_t *v) {
        assert(bar == 0 && o + 8 <= sizeof(vram));
        if (removed) *v = UINT64_MAX;
        else memcpy(v, vram + o, 8);
    }
    void MemoryWrite64(uint32_t bar, uint64_t o, uint64_t v) {
        assert(bar == 1);
        offset = o; value = v; ++doorbells;
    }
};
struct DeviceContext {
    FakePCI *pci;
    uint32_t bar0MemIndex = 0, bar2MemIndex = 1;
    uint64_t bar0Size = 0x40000, bar2Size = 0x200000;
};
#include "amdgpu_vram_io.h"
static void amdgpu_hdp_flush(const DeviceContext &) {}
struct MESInstance {
    bool inited = true, enabled = true, submission_pending = false;
    void *ring_cpu, *wb_cpu;
    uint32_t ring_size_dwords = 256, doorbell_index = 0x40;
    uint64_t vram_base = 0x8000000000, published_wptr = 0;
    uint64_t sch_ctx_bus=0x8000040000, resource_1_bus=0x8000044000;
    uint64_t ring_bus = vram_base + 0x10000, wb_bus = vram_base + 0x20000;
};
static unsigned snapshots = 0;
static void mes_log_queue_state(const DeviceContext &, const MESInstance &, uint32_t) {
    ++snapshots;
}
struct IODMACommand {};
struct IOAddressSegment { uint64_t address = 0, length = 0; };
constexpr unsigned kIOMemoryDirectionOutIn = 0;
struct IOBufferMemoryDescriptor {
    void *data = nullptr;
    uint64_t size = 0;
    static inline bool failCreate = false;
    static int Create(unsigned, uint64_t bytes, uint64_t, IOBufferMemoryDescriptor **out) {
        if (failCreate) return kIOReturnNoMemory;
        *out = new IOBufferMemoryDescriptor;
        (*out)->data = calloc(1, bytes);
        (*out)->size = bytes;
        return kIOReturnSuccess;
    }
    void SetLength(uint64_t bytes) { assert(bytes == size); }
    void GetAddressRange(IOAddressSegment *segment) {
        segment->address = reinterpret_cast<uint64_t>(data);
        segment->length = size;
    }
    void release() { free(data); delete this; }
};
struct GMCContext {
    uint64_t vram_start = 0x8000000000;
    VRAMBumpAllocator vram_alloc;
};
#include "mes_allocation_under_test.inc"
struct MESContext { MESInstance pipe[2]; bool uni_mes_active=true; };
static uint64_t test_time_ns;
static volatile uint64_t *test_wb;
static int completion_mode;
static uint64_t test_clock(int) { return test_time_ns; }
static void IOSleep(unsigned ms) {
    test_time_ns += uint64_t(ms) * 1000000;
    if (completion_mode && test_time_ns >= 5000000) {
        test_wb[0xd0 / 8] = 1;
        test_wb[0xc0 / 8] = completion_mode == 1 ? 1 : 0x123400000000ull;
    }
}
#define MES_LOG(...) do {} while (0)
#define clock_gettime_nsec_np test_clock
#define CLOCK_UPTIME_RAW 0
#include "mes_submission_under_test.inc"
#undef clock_gettime_nsec_np

int main() {
    // Hardware discovery never supplies the abstract GMC block. Firmware must
    // receive the MMHUB addresses anyway, with unused segments zeroed.
    IPBaseTable ip;
    ip.setBase(IPBlock::GC, 0, 0x1260);
    ip.setBase(IPBlock::GC, 1, 0xa000);
    ip.setBase(IPBlock::GC, 2, 0x1c000);
    ip.setBase(IPBlock::GC, 3, 0x2402c00);
    ip.setBase(IPBlock::MMHUB, 0, 0x1a000);
    ip.setBase(IPBlock::MMHUB, 1, 0x2408800);
    ip.setBase(IPBlock::OSSSYS, 0, 0x10a0);
    ip.setBase(IPBlock::OSSSYS, 1, 0x240a000);
    MES_SetHwResources resources{};
    assert(!ip.isResolved(IPBlock::GMC));
    assert(mes_set_register_bases(ip, resources));
    const uint32_t gcBases[5]={0x1260,0xa000,0x1c000,0x2402c00,0};
    const uint32_t mmBases[5]={0x1a000,0x2408800,0,0,0};
    const uint32_t osBases[5]={0x10a0,0x240a000,0,0,0};
    assert(memcmp(resources.gc_base,gcBases,sizeof(gcBases))==0);
    assert(memcmp(resources.mmhub_base,mmBases,sizeof(mmBases))==0);
    assert(memcmp(resources.osssys_base,osBases,sizeof(osBases))==0);
    for (auto block : {IPBlock::GC,IPBlock::MMHUB,IPBlock::OSSSYS}) {
        auto missing=ip;
        missing.setBase(block,0,0);
        assert(!mes_set_register_bases(missing, resources));
    }
    alignas(8) uint32_t ring[256] = {};
    alignas(8) uint64_t wb[512] = {};
    FakePCI pci;
    DeviceContext dev{&pci};
    MESContext mes{};
    mes.pipe[0].ring_cpu = ring;
    mes.pipe[0].wb_cpu = wb;
    test_wb = reinterpret_cast<uint64_t *>(pci.vram + 0x20000);
    MES_QueryStatus pkt{};
    pkt.header.u32All = mes_api_header(1, 11, 64);
    MESAPI__QUERY_MES_STATUS upstream{};
    upstream.header.type = 1;
    upstream.header.opcode = 11;
    upstream.header.dwsize = 64;
    assert(memcmp(&pkt, &upstream, sizeof(pkt)) == 0);
    for (int mode = 0; mode < 3; ++mode) {
        memset(wb, 0, sizeof(wb));
        memset(pci.vram, 0, sizeof(pci.vram));
        mes.pipe[0].published_wptr = 0;
        mes.pipe[0].submission_pending = false;
        test_time_ns = 0;
        completion_mode = mode;
        const auto r = mes_submit_pkt(dev, mes, MESPipe::Sched,
            reinterpret_cast<const uint32_t *>(&pkt), 2, 2000000);
        assert(r == (mode == 0 ? kIOReturnTimeout : mode == 1 ? kIOReturnSuccess : kIOReturnInternalError));
        assert(test_time_ns == (mode == 0 ? 2000000000ull : 5000000ull));
        assert(pci.offset == 0x100 && pci.value == 128); // dwords, not bytes
        assert(mes.pipe[0].submission_pending == (mode == 0));
        assert(memcmp(pci.vram + 0x10000, ring, 128 * 4) == 0);
        assert(test_wb[0x40 / 8] == 128);
        if (mode == 0) {
            const unsigned before = pci.doorbells;
            assert(mes_submit_pkt(dev, mes, MESPipe::Sched,
                reinterpret_cast<const uint32_t *>(&pkt), 2, 2000000) == kIOReturnBusy);
            assert(pci.doorbells == before);
        }
        assert(wb[0x40 / 8] == 128); // hardware WPTR shadow published
        uint64_t query_addr;
        memcpy(&query_addr, ring + 64 + 2, 8);
        assert(query_addr == mes.pipe[0].wb_bus + 0xd0);
    }
    // Byte-for-byte REMOVE_QUEUE oracle and actual KIQ submission wrapper.
    MES_RemoveQueue remove{};
    MESAPI__REMOVE_QUEUE linuxRemove{};
    linuxRemove.header.type=1; linuxRemove.header.opcode=3; linuxRemove.header.dwsize=64;
    linuxRemove.unmap_legacy_queue=1; linuxRemove.queue_type=static_cast<MES_QUEUE_TYPE>(1);
    linuxRemove.pipe_id=2; linuxRemove.queue_id=7; linuxRemove.doorbell_offset=0x80;
    assert(mes_build_legacy_unmap(remove,1,2,7,0x80));
    assert(memcmp(&remove,&linuxRemove,sizeof(remove))==0);
    assert(!mes_build_legacy_unmap(remove,3,0,0,0));
    assert(!mes_build_legacy_unmap(remove,1,4,0,0));
    assert(!mes_build_legacy_unmap(remove,1,0,8,0));
    assert(!mes_build_legacy_unmap(remove,1,0,0,1));
    assert(!mes_build_legacy_unmap(remove,1,0,0,0x4000000));
    mes.pipe[1]=mes.pipe[0];
    for (int mode=0;mode<3;++mode) {
        memset(wb,0,sizeof(wb)); memset(pci.vram,0,sizeof(pci.vram));
        mes.pipe[1].published_wptr=0; mes.pipe[1].submission_pending=false;
        test_time_ns=0; completion_mode=mode;
        const auto r=mes_unmap_legacy_queue(dev,mes,1,2,7,0x80);
        assert(r==(mode==0 ? kIOReturnTimeout : mode==1 ? kIOReturnSuccess : kIOReturnInternalError));
        assert(test_time_ns==(mode==0 ? 500000000ull : 5000000ull));
        assert(mes.pipe[1].submission_pending==(mode==0));
        MES_RemoveQueue published; memcpy(&published,pci.vram+0x10000,sizeof(published));
        assert(published.api_status.fence_addr==mes.pipe[1].wb_bus+0xc0);
        published.api_status={};
        assert(memcmp(&published,&linuxRemove,sizeof(published))==0);
    }
    const auto beforeUnmap=pci.doorbells;
    mes.uni_mes_active=false;
    assert(mes_unmap_legacy_queue(dev,mes,1,0,0,0x80)==kIOReturnNotReady);
    assert(pci.doorbells==beforeUnmap);
    mes.uni_mes_active=true;
    // A wrapped storage offset must not wrap the monotonic hardware pointer.
    wb[0x80 / 8] = (1ull << 32) + 252;
    mes.pipe[0].published_wptr = wb[0x80 / 8];
    const uint32_t words[] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(mes_ring_write(mes.pipe[0], words, 8) == 8);
    assert(ring[252] == 1 && ring[3] == 8);
    assert(wb[0x80 / 8] == (1ull << 32) + 260);
    assert(mes_kick_doorbell(dev, mes.pipe[0]) == 0);
    assert(pci.value == (1ull << 32) + 260);
    assert(memcmp(pci.vram + 0x10000 + 252 * 4, words, 4 * 4) == 0);
    assert(memcmp(pci.vram + 0x10000, words + 4, 4 * 4) == 0);
    assert(snapshots == 2);

    // Lost BAR writes must be detected before the queue is published.
    const unsigned before = pci.doorbells;
    pci.dropWrites = true;
    assert(mes_ring_write(mes.pipe[0], words, 8) == 8);
    assert(mes_kick_doorbell(dev, mes.pipe[0]) == kIOReturnIOError);
    assert(pci.doorbells == before);

    // Reject raw PCI addresses and malformed/overflowing submissions before
    // any device writes. No half-written command pair may reach the GPU.
    dev.bar2Size = 0x200000;
    auto invalid = mes.pipe[0];
    invalid.ring_bus = 0x82080000;
    assert(mes_kick_doorbell(dev, invalid) == kIOReturnBadArgument);
    const unsigned writesBefore = pci.writes;
    mes.pipe[0].published_wptr = wb[0x80 / 8] = UINT64_MAX - 63;
    assert(mes_submit_pkt(dev, mes, MESPipe::Sched,
        reinterpret_cast<const uint32_t *>(&pkt), 2, 2000000) == kIOReturnNoSpace);
    assert(mes_ring_write(mes.pipe[0], words, 64) == 0);
    mes.pipe[0].published_wptr = wb[0x80 / 8] = 0;
    mes.pipe[0].ring_size_dwords = 64;
    assert(mes_submit_pkt(dev, mes, MESPipe::Sched,
        reinterpret_cast<const uint32_t *>(&pkt), 2, 2000000) == kIOReturnBadArgument);
    assert(pci.writes == writesBefore && pci.doorbells == before);
    pci.dropWrites = false;
    pci.removed = true;
    uint64_t fence = 0;
    assert(vram_read_fence64(dev, 0x200d0, &fence) == kIOReturnNotAttached);
    pci.removed = false;
    assert(vram_write_verified(dev, UINT64_MAX - 3, words, 8) == kIOReturnBadArgument);
    assert(vram_write_verified(dev, dev.bar0Size - 4, words, 8) == kIOReturnBadArgument);
    assert(vram_write_verified(dev, 3, words, 8) == kIOReturnBadArgument);
    assert(vram_read_fence64(dev, 0x200d4, &fence) == kIOReturnBadArgument);
    dev.bar2Size = 0x104;
    assert(mes_kick_doorbell(dev, mes.pipe[0]) == kIOReturnBadArgument);
    assert(pci.doorbells == before);

    // Actual allocation helper must return MC addresses in visible VRAM,
    // retain only a staging descriptor, and unwind pre-publication failures.
    GMCContext gmc;
    gmc.vram_alloc.init(gmc.vram_start + 0x4000, 0xc000);
    IOBufferMemoryDescriptor *buffer = nullptr;
    IODMACommand *dma = nullptr;
    void *cpu = nullptr;
    uint64_t gpu = 0;
    assert(mes_alloc_vram_block(dev, gmc, 2048, &buffer, &dma, &gpu, &cpu) == 0);
    assert(gpu == gmc.vram_start + 0x4000 && cpu && buffer && !dma);
    assert(gmc.vram_alloc.bytes_used() == 16384);
    buffer->release();
    IOBufferMemoryDescriptor::failCreate = true;
    assert(mes_alloc_vram_block(dev, gmc, 4096, &buffer, &dma, &gpu, &cpu) == kIOReturnNoMemory);
    assert(gmc.vram_alloc.bytes_used() == 16384 && !buffer && !cpu && !gpu);
    IOBufferMemoryDescriptor::failCreate = false;
    pci.removed = true;
    assert(mes_alloc_vram_block(dev, gmc, 4096, &buffer, &dma, &gpu, &cpu) == kIOReturnIOError);
    assert(gmc.vram_alloc.bytes_used() == 16384 && !buffer && !cpu && !gpu);
    puts("MES protocol: upstream layouts/registers, VRAM allocation/publication, wraparound, failures, completion and timeout pass");
}
