#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>
#include <fstream>
#include <iterator>
#include <string>
#include <cstdlib>
#include <cstring>
#include "amdgpu_vram.h"
#include "amdgpu_software_stats.h"
#include "amdgpu_client_lifecycle.h"
#include "amdgpu_cp_firmware.h"
#include "amdgpu_cp_registers.h"
#include "amdgpu_gfx_registers.h"
#include "amdgpu_pm4.h"
#include "amdgpu_gfx_mqd.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h"
using namespace amdgpu;
#include "cp_register_checks.inc"
using kern_return_t = int;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnNoSpace=2,
    kIOReturnNotAttached=3, kIOReturnIOError=4, kIOReturnTimeout=5,
    kIOReturnNoMemory=6, kIOReturnBadArgument=7, kIOReturnInternalError=8;
constexpr uint64_t kASPageSize=16384;
enum class IPBlock { GC };
struct FakeIP { bool isResolved(IPBlock) const { return true; } };
struct FakePCI {
    alignas(8) uint8_t vram[0x40000]{};
    uint64_t offset = 0, value = 0;
    unsigned doorbells = 0, writes = 0;
    bool dropWrites = false, removed = false;
    uint64_t dropOffset = UINT64_MAX;
    void MemoryWrite32(uint32_t bar, uint64_t o, uint32_t v) {
        assert(bar == 0 && o + 4 <= sizeof(vram));
        ++writes;
        if (!dropWrites && o != dropOffset) memcpy(vram + o, &v, 4);
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
    FakePCI *pci; FakeIP ip;
    uint32_t bar0MemIndex=0, bar2MemIndex=1;
    uint64_t bar0Size=0x40000, bar2Size=0x200000;
    bool doorbell_works=false;
    software_stats::Counters *softwareStats=nullptr;
};
#include "amdgpu_vram_io.h"
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
#include "cp_context_under_test.inc"
static std::map<uint32_t,uint32_t> registers;
struct Write { uint32_t reg, value, selection; };
static std::vector<Write> writes;
static uint64_t now;
static bool disconnected=false, ignoreWrites=false;
static uint32_t SOC15_REG_OFFSET_BIDX(const DeviceContext &, IPBlock, int b, uint32_t r) { return r == regSCRATCH_REG0 ? 0xc040 : 0x10000 + b*0x10000 + r; }
static uint32_t RREG32(const DeviceContext &, uint32_t r) {
    if (disconnected) return UINT32_MAX;
    const auto selection = registers[0x20000+regGRBM_GFX_CNTL];
    const bool unusedGraphicsPipe = (selection & GRBM_GFX_CNTL__MEID_MASK) == 0 &&
        (selection & GRBM_GFX_CNTL__PIPEID_MASK) != 0;
    if (unusedGraphicsPipe && (r == 0x10000+regCP_PFP_PRGRM_CNTR_START ||
        r == 0x10000+regCP_PFP_PRGRM_CNTR_START_HI ||
        r == 0x10000+regCP_ME_PRGRM_CNTR_START ||
        r == 0x10000+regCP_ME_PRGRM_CNTR_START_HI)) return 0xdeadbeef;
    if ((selection & GRBM_GFX_CNTL__MEID_MASK) == (1u << GRBM_GFX_CNTL__MEID__SHIFT) &&
        (selection & GRBM_GFX_CNTL__PIPEID_MASK) >= 2 &&
        (r == 0x20000+regCP_MEC_RS64_PRGRM_CNTR_START ||
         r == 0x20000+regCP_MEC_RS64_PRGRM_CNTR_START_HI)) return 0xdeadbeef;
    return registers[r];
}
static void WREG32(const DeviceContext &, uint32_t r, uint32_t v) { writes.push_back({r,v,registers[0x20000+regGRBM_GFX_CNTL]}); if (!ignoreWrites) registers[r]=v; }
static void amdgpu_hdp_flush(const DeviceContext &) {}
static uint64_t test_clock(int) { return now; }
static FakePCI *activePCI;
static CPContext *activeCP;
struct MESContext { bool uni_mes_active=true; };
constexpr uint32_t kMESQueueType_GFX=0;
static unsigned mapCalls=0;
static int mapError=0;
static int mes_map_legacy_queue(const DeviceContext &dev, MESContext &, unsigned type,
    unsigned pipe, unsigned queue, unsigned doorbell, uint64_t mqd, uint64_t wptr) {
    ++mapCalls;
    assert(type==0 && pipe==0 && queue==0 && doorbell==0);
    const auto *words=reinterpret_cast<const uint32_t *>(dev.pci->vram+(mqd-0x8000000000ull));
    assert(words[GFXMQDOff::cp_mqd_base_addr]==uint32_t(mqd));
    assert(words[GFXMQDOff::cp_rb_wptr_poll_addr_lo]==uint32_t(wptr));
    return mapError;
}
static void cp_log_control(const DeviceContext &, const char *) {}
static bool completeFence = false, completeSmoke = false, removeOnSleep = false;
static uint64_t smokeAddress = 0;
static uint32_t smokeValue = 0xdeadbeef;
static bool completeScratch = true;
static unsigned smokeTicks = 0;
static void IOSleep(unsigned ms) {
    now += uint64_t(ms)*1000000;
    if (completeScratch && registers[0xc040] == 0xcafedead && now >= 3000000) {
        registers[0xc040] = 0xdeadbeef;
        const uint32_t rptr = uint32_t(activeCP->published_wptr);
        memcpy(activePCI->vram + activeCP->wb_vram_off, &rptr, 4);
    }
    if (removeOnSleep && smokeAddress) activePCI->removed = true;
    if (completeSmoke && smokeAddress && ++smokeTicks >= 3)
        memcpy(activePCI->vram + (smokeAddress - 0x8000000000ull), &smokeValue, 4);
    if (completeFence && now >= 3000000) {
        uint64_t value = activeCP->fence_counter;
        memcpy(activePCI->vram + activeCP->wb_vram_off + kCPWBOffsetFence, &value, 8);
    }
}
#define REG_SET_FIELD(v,r,f,x) (((v) & ~r##__##f##_MASK) | (((x) << r##__##f##__SHIFT) & r##__##f##_MASK))
template<class... Args> static void test_log(Args...) {}
#define CP_LOG(...) test_log(__VA_ARGS__)
#define clock_gettime_nsec_np test_clock
#include "cp_under_test.inc"
int main() {
    FakePCI pci;
    DeviceContext dev{&pci, {}};
    GMCContext gmc;
    gmc.vram_alloc.init(gmc.vram_start + 0x10000, 0x30000);
    CPContext cp{};
    assert(cp_alloc_storage(dev,gmc,cp)==0);
    assert(cp.ring_bus==gmc.vram_start+0x10000 && cp.wb_bus==gmc.vram_start+0x14000);
    const auto used = gmc.vram_alloc.bytes_used();
    assert(cp_alloc_storage(dev,gmc,cp)==0 && gmc.vram_alloc.bytes_used()==used);
    MESContext mapMes;
    assert(cp_map_gfx_queue(dev,gmc,cp,mapMes)==kIOReturnNotReady && mapCalls==0);
    cp.enginesStarted=true;
    assert(cp_map_gfx_queue(dev,gmc,cp,mapMes)==0 && mapCalls==1);
    assert(cp.mqd_bus==gmc.vram_start+0x18000);
    assert(cp_map_gfx_queue(dev,gmc,cp,mapMes)==kIOReturnNotReady && mapCalls==1);
    assert(!registers.contains(0x10000+regCP_RB0_BASE));
    cp.ringReady=true; cp.doorbell_index=0x40;
    auto *ring=static_cast<uint32_t *>(cp.ring_cpu);
    auto &rptr=*reinterpret_cast<uint32_t *>(pci.vram+cp.wb_vram_off);
    auto &shadow=*reinterpret_cast<uint64_t *>(pci.vram+cp.wb_vram_off+kCPWBOffsetWptr);
    auto &gpuFence=*reinterpret_cast<uint64_t *>(pci.vram+cp.wb_vram_off+kCPWBOffsetFence);
    activePCI=&pci; activeCP=&cp;
    uint32_t pkt[10];
    pm4_build_fence(pkt,0x8001800000ull,0xdeadbeef,false,false);
    assert(cp_ring_write(cp,pkt,10)==10);
    assert(cp_kick_doorbell(dev,cp)==0);
    assert(cp.wptr==256 && shadow==256 && pci.value==256 && pci.offset==0x100);
    for (int i=10;i<256;++i) assert(ring[i]==0xffff1000u);
    // Hardware 168 wraps RPTR at 4096 dwords while WPTR stays monotonic.
    // Reproduce the ninth smoke-test failure at the first wrap, then keep
    // consuming work over several more wraps without resetting the queue.
    cp.wptr=cp.published_wptr=4096; rptr=0;
    for (unsigned i=0;i<80;++i) {
        const auto next=cp.wptr+256;
        assert(cp_ring_write(cp,pkt,10)==10);
        assert(cp_kick_doorbell(dev,cp)==0);
        assert(cp.wptr==next && shadow==next && pci.value==next);
        rptr=uint32_t(cp.wptr)&cp.ring_ptr_mask;
    }
    // With no GPU progress, reserve a slot and reject the next fetch block
    // before any word is overwritten or any doorbell is rung.
    for (unsigned i=0;i<15;++i) {
        assert(cp_ring_write(cp,pkt,10)==10);
        assert(cp_kick_doorbell(dev,cp)==0);
    }
    const auto fullWptr=cp.wptr;
    const auto fullKicks=pci.doorbells;
    assert(cp_ring_write(cp,pkt,10)==0);
    assert(cp.wptr==fullWptr && pci.doorbells==fullKicks);
    rptr=uint32_t(cp.wptr)&cp.ring_ptr_mask;
    assert(cp_ring_write(cp,pkt,10)==10 && cp_kick_doorbell(dev,cp)==0);
    // Cross both the storage wrap and 32-bit hardware pointer boundary.
    cp.wptr=cp.published_wptr=(1ull<<32)-6; rptr=static_cast<uint32_t>(cp.wptr);
    assert(cp_ring_write(cp,pkt,10)==10);
    assert(ring[4090]==pkt[0] && ring[3]==pkt[9]);
    assert(cp_kick_doorbell(dev,cp)==0);
    assert(cp.wptr==(1ull<<32)+256 && shadow==cp.wptr);
    assert(!registers.contains(0x10000+regCP_RB0_WPTR_HI));
    assert(!registers.contains(0x10000+regCP_RB0_WPTR));
    assert(memcmp(pci.vram+cp.ring_vram_off+4090*4,pkt,6*4)==0);
    assert(memcmp(pci.vram+cp.ring_vram_off,pkt+6,4*4)==0);
    // GPU-owned fence/RPTR slots survive packet/WPTR publication.
    gpuFence=123; rptr=uint32_t(cp.wptr);
    assert(cp_ring_write(cp,pkt,10)==10);
    assert(cp_kick_doorbell(dev,cp)==0 && gpuFence==123);
    // Failed command or pointer upload must not notify the engine.
    rptr=uint32_t(cp.wptr);
    assert(cp_ring_write(cp,pkt,10)==10);
    const auto kicks=pci.doorbells;
    pci.dropWrites=true;
    assert(cp_kick_doorbell(dev,cp)==kIOReturnIOError && pci.doorbells==kicks);
    pci.dropWrites=false;
    assert(cp_kick_doorbell(dev,cp)==0);
    rptr=uint32_t(cp.wptr);
    assert(cp_ring_write(cp,pkt,10)==10);
    const auto published=cp.published_wptr;
    const auto beforeShadowFailure=pci.doorbells;
    pci.dropOffset=cp.wb_vram_off+kCPWBOffsetWptr;
    assert(cp_kick_doorbell(dev,cp)==kIOReturnIOError);
    assert(pci.doorbells==beforeShadowFailure && cp.published_wptr==published);
    pci.dropOffset=UINT64_MAX;
    assert(cp_kick_doorbell(dev,cp)==0);
    rptr=uint32_t(cp.wptr);
    pci.removed=true;
    assert(cp_ring_write(cp,pkt,10)==0);
    assert(cp_kick_doorbell(dev,cp)==kIOReturnNotAttached);
    pci.removed=false;
    const auto validWptr=cp.wptr;
    cp.wptr=UINT64_MAX-1;
    assert(cp_ring_write(cp,pkt,10)==0);
    assert(cp_kick_doorbell(dev,cp)==kIOReturnBadArgument);
    cp.wptr=validWptr;
    dev.bar2Size=0x100;
    assert(cp_kick_doorbell(dev,cp)==kIOReturnBadArgument);
    dev.bar2Size=0x200000;
    // Completion must come from VRAM, never from a stale CPU cache.
    ClientSubmission submission;
    assert(submission.beginCP(cp.fence_cpu,cp_read_cs_fence,&cp));
    submission.expected=7; cp.fence_shadow=7; gpuFence=6;
    assert(!submission.poll() && cp.fence_shadow==6);
    gpuFence=7; pci.removed=true;
    assert(!submission.poll() && submission.completedCPFence==0);
    pci.removed=false;
    assert(submission.poll() && submission.completedCPFence==7);
    gpuFence=UINT64_MAX;
    assert(submission.beginCP(cp.fence_cpu,cp_read_cs_fence,&cp));
    submission.expected=8;
    assert(!submission.poll() && submission.completedCPFence==7);
    gpuFence=8;
    assert(submission.poll() && submission.completedCPFence==8);
    // Actual fence emission/polling: delayed success, timeout, and removal.
    software_stats::Counters accounting;
    dev.softwareStats = &accounting;
    for (bool complete : {false,true}) {
        rptr=uint32_t(cp.wptr); gpuFence=0; now=0; completeFence=complete;
        accounting.reset(now);
        uint32_t observed=0;
        assert(cp_submit_eop_test(dev,cp,100000,&observed)==(complete?0:kIOReturnTimeout));
        assert(now==(complete?3000000:100000000));
        assert(observed==(complete?cp.fence_counter:0));
        const auto &counts = accounting.data.engines[software_stats::GFX];
        assert(counts.submitted == 1 && counts.completed == unsigned(complete));
        assert(counts.failed == unsigned(!complete) && counts.pending == unsigned(!complete));
        assert(software_stats::valid(accounting.snapshot(now, true)));
    }
    dev.softwareStats = nullptr;
    completeFence=false;
    MESContext mes;
    uint64_t elapsed=0;
    uint32_t observed=0;
    // A failed register test must not allocate or submit a memory fence.
    rptr=uint32_t(cp.wptr); now=0; completeScratch=false;
    const auto beforeScratch=gmc.vram_alloc.bytes_used();
    const auto beforeScratchWptr=cp.wptr;
    assert(cp_kiq_smoke_test(dev,cp,mes,gmc,smokeValue,100000,&elapsed,&smokeAddress,&observed)==kIOReturnTimeout);
    assert(elapsed==100000 && observed==0xcafedead && smokeAddress==0);
    assert(gmc.vram_alloc.bytes_used()==beforeScratch && cp.wptr==beforeScratchWptr+256);
    const auto scratchIndex=uint32_t(beforeScratchWptr)&cp.ring_ptr_mask;
    assert(ring[scratchIndex]==0xc0017900 && ring[scratchIndex+1]==0x40 && ring[scratchIndex+2]==0xdeadbeef);
    completeScratch=true;
    rptr=uint32_t(cp.wptr); now=0; completeSmoke=true; smokeTicks=0;
    auto before=gmc.vram_alloc.bytes_used();
    assert(cp_kiq_smoke_test(dev,cp,mes,gmc,smokeValue,100000,&elapsed,&smokeAddress,&observed)==0);
    assert(elapsed==3000 && observed==smokeValue && gmc.vram_alloc.bytes_used()==before);
    assert(cp_kiq_smoke_test(dev,cp,mes,gmc,0xcafebabe,100000,&elapsed,&smokeAddress,&observed)==kIOReturnBadArgument);
    assert(gmc.vram_alloc.bytes_used()==before);
    rptr=uint32_t(cp.wptr); now=0; completeSmoke=false;
    assert(cp_kiq_smoke_test(dev,cp,mes,gmc,smokeValue,100000,&elapsed,&smokeAddress,&observed)==kIOReturnTimeout);
    assert(elapsed==100000 && observed==0xcafebabe && gmc.vram_alloc.bytes_used()==before+kASPageSize);
    rptr=uint32_t(cp.wptr); now=0; removeOnSleep=true;
    assert(cp_kiq_smoke_test(dev,cp,mes,gmc,smokeValue,100000,&elapsed,&smokeAddress,&observed)==kIOReturnNotAttached);
    assert(gmc.vram_alloc.bytes_used()==before+2*kASPageSize);
    removeOnSleep=false; pci.removed=false;
    // A stalled GPU must not allow queued data to be overwritten.
    cp.wptr=3840; rptr=0;
    assert(cp_ring_write(cp,pkt,10)==0);
    cp.ringReady=false;
    assert(cp_ring_write(cp,pkt,10)==0 && cp_kick_doorbell(dev,cp)==kIOReturnNotReady);
    assert(cp_enable(dev,false)==0);
    assert(registers[0x20000+regCP_ME_CNTL]==0x14000000);
    assert(!registers.contains(0x10000+regCP_ME_CNTL));
    assert(cp_enable(dev,true)==0);
    assert(cp_compute_enable(dev,false)==0);
    assert(registers[0x20000+regCP_MEC_RS64_CNTL] & CP_MEC_RS64_CNTL__MEC_HALT_MASK);
    assert(!registers.contains(0x10000+regCP_MEC_RS64_CNTL));
    assert(cp_compute_enable(dev,true)==0);
    now=0; registers[0x10000+regCP_STAT]=1;
    assert(cp_enable(dev,false)==kIOReturnTimeout && now==1000000000ull);
    disconnected=true;
    assert(cp_enable(dev,true)==kIOReturnNotAttached);
    assert(cp_compute_enable(dev,true)==kIOReturnNotAttached);
    disconnected=false; ignoreWrites=true;
    assert(cp_enable(dev,true)==kIOReturnIOError);
    ignoreWrites=false; registers.clear(); writes.clear();
    assert(cp_configure_rs64(dev,cp)==kIOReturnNotReady && writes.empty());
    unsigned f=0;
    for (const char *family : {"pfp", "me", "mec"}) {
        std::ifstream file(std::string("firmware/gc_12_0_1_")+family+".bin", std::ios::binary);
        assert(file.good());
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        uint64_t entry=0;
        assert(cp_parse_firmware_start(bytes.data(),bytes.size(),entry));
        assert(entry==0x0007000000003000ull);
        cp.firmware[f++]={entry,true};
        // Value must survive reuse of the upload buffer for another firmware.
        std::fill(bytes.begin(),bytes.end(),0xa5);
        assert(!cp_parse_firmware_start(bytes.data(),bytes.size(),entry));
        assert(entry==0);
    }
    // The observed 157 control value leaves both active graphics pipes reset.
    registers[0x20000+regCP_ME_CNTL]=0x1514a000;
    registers[0x20000+regCP_MEC_RS64_CNTL]=0x400f0010;
    assert(cp_configure_rs64(dev,cp)==0);
    assert(registers[0x20000+regCP_ME_CNTL]==0x1500a000);
    assert(registers[0x20000+regCP_MEC_RS64_CNTL]==0x40000010);
    assert(registers[0x20000+regGRBM_GFX_CNTL]==0);
    const uint32_t lows[]={regCP_PFP_PRGRM_CNTR_START,regCP_ME_PRGRM_CNTR_START,regCP_MEC_RS64_PRGRM_CNTR_START};
    const uint32_t highs[]={regCP_PFP_PRGRM_CNTR_START_HI,regCP_ME_PRGRM_CNTR_START_HI,regCP_MEC_RS64_PRGRM_CNTR_START_HI};
    const uint32_t masks[]={CP_ME_CNTL__PFP_PIPE0_RESET_MASK|CP_ME_CNTL__PFP_PIPE1_RESET_MASK,
        CP_ME_CNTL__ME_PIPE0_RESET_MASK|CP_ME_CNTL__ME_PIPE1_RESET_MASK,
        CP_MEC_RS64_CNTL__MEC_PIPE0_RESET_MASK|CP_MEC_RS64_CNTL__MEC_PIPE1_RESET_MASK|
        CP_MEC_RS64_CNTL__MEC_PIPE2_RESET_MASK|CP_MEC_RS64_CNTL__MEC_PIPE3_RESET_MASK};
    // Independently reconstruct Linux config_gfx_rs64's ordered register writes.
    size_t cursor=0;
    uint32_t control=0x1514a000;
    auto expect=[&](uint32_t reg,uint32_t value) {
        assert(cursor<writes.size());
        if (writes[cursor].reg!=reg || writes[cursor].value!=value)
            fprintf(stderr,"write %zu: got %#x=%#x expected %#x=%#x\n",cursor,writes[cursor].reg,writes[cursor].value,reg,value);
        assert(writes[cursor].reg==reg && writes[cursor].value==value);
        ++cursor;
    };
    for (unsigned e=0;e<3;++e) {
        unsigned pipes=e==2?4:2;
        for (unsigned pipe=0;pipe<pipes;++pipe) {
            const uint32_t sel=(pipe<<GRBM_GFX_CNTL__PIPEID__SHIFT)|((e==2?1u:0u)<<GRBM_GFX_CNTL__MEID__SHIFT);
            expect(0x20000+regGRBM_GFX_CNTL,sel);
            expect((e==2?0x20000:0x10000)+lows[e],0xc00);
            expect((e==2?0x20000:0x10000)+highs[e],0x1c000);
        }
        expect(0x20000+regGRBM_GFX_CNTL,0);
        if(e==2) control=0x400f0010;
        const uint32_t ctrlReg=0x20000+(e==2?regCP_MEC_RS64_CNTL:regCP_ME_CNTL);
        expect(ctrlReg,control|masks[e]);
        control &= ~masks[e];
        expect(ctrlReg,control);
    }
    assert(cursor==writes.size());
    disconnected=true;
    assert(cp_configure_rs64(dev,cp)==kIOReturnNotAttached);
    disconnected=false; ignoreWrites=true;
    registers[0x10000+regCP_PFP_PRGRM_CNTR_START]=0;
    assert(cp_configure_rs64(dev,cp)==kIOReturnIOError);
    assert(registers[0x20000+regCP_ME_CNTL] & CP_ME_CNTL__ME_HALT_MASK);
    uint64_t invalid=123;
    assert(!cp_parse_firmware_start(nullptr,0,invalid) && invalid==0);
    gfx_firmware_header_v2_0 bad{};
    bad.header.header_version_major=2;
    bad.header.header_size_bytes=sizeof(bad);
    bad.header.size_bytes=sizeof(bad);
    bad.ucode_size_bytes=16; bad.data_size_bytes=16;
    bad.header.ucode_array_offset_bytes=UINT32_MAX;
    assert(!cp_parse_firmware_start(reinterpret_cast<const uint8_t *>(&bad),sizeof(bad),invalid));
    cp_release_storage(cp);
    assert(!cp.inited && !cp.ring_buf && !cp.fence_cpu);
    // VRAM remains reserved until the reset/close owner resets GMC.
    assert(gmc.vram_alloc.bytes_used()==used+3*kASPageSize);
    // Failed map/upload retains firmware-visible descriptor backing until
    // reset, blocks replay, and cannot expose a ready command ring.
    ignoreWrites=false;
    for (bool uploadFailure : {false,true}) {
        GMCContext trial;
        trial.vram_alloc.init(trial.vram_start+0x10000,0x30000);
        CPContext pending{};
        assert(cp_alloc_storage(dev,trial,pending)==0);
        pending.enginesStarted=true;
        const auto beforeMaps=mapCalls;
        mapError=kIOReturnTimeout;
        pci.dropWrites=uploadFailure;
        // Poison the target so a dropped upload cannot match an older MQD.
        memset(pci.vram+0x18000,0xa5,sizeof(GFXQueueDescriptor));
        assert(cp_map_gfx_queue(dev,trial,pending,mapMes)==
            (uploadFailure?kIOReturnIOError:kIOReturnTimeout));
        assert(!pending.ringReady && pending.mqd_bus);
        assert(mapCalls==beforeMaps+(uploadFailure?0:1));
        assert(cp_map_gfx_queue(dev,trial,pending,mapMes)==kIOReturnNotReady);
        assert(trial.vram_alloc.bytes_used()==3*kASPageSize);
        cp_release_storage(pending);
        assert(trial.vram_alloc.bytes_used()==3*kASPageSize);
        pci.dropWrites=false; mapError=0;
    }
    GMCContext failing;
    failing.vram_alloc.init(failing.vram_start+0x10000,0x4000);
    assert(cp_alloc_storage(dev,failing,cp)==kIOReturnNoMemory);
    assert(failing.vram_alloc.bytes_used()==0 && !cp.inited);
    failing.vram_alloc.init(failing.vram_start+0x10000,0x8000);
    IOBufferMemoryDescriptor::failCreate=true;
    assert(cp_alloc_storage(dev,failing,cp)==kIOReturnNoMemory);
    assert(failing.vram_alloc.bytes_used()==0 && !cp.inited);
    IOBufferMemoryDescriptor::failCreate=false; pci.removed=true;
    assert(cp_alloc_storage(dev,failing,cp)==kIOReturnIOError);
    assert(failing.vram_alloc.bytes_used()==0 && !cp.inited);
    puts("CP: VRAM storage/publication/completion/rollback, real firmware entries, Linux RS64 pipe startup, register segments, ring padding/wrap, capacity, halt acknowledgement and timeout pass");
}
