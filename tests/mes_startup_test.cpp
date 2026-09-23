#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <array>
#include <map>
#include <algorithm>
#include "amdgpu_ip.h"
#include "amdgpu_vram.h"
#include "amdgpu_mes_packets.h"
#include "amdgpu_mes_registers.h"
#include "amdgpu_mes_resources.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/mes_v12_api_def.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/v12_structs.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_0_default.h"
using kern_return_t = int;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnBadArgument=2,
    kIOReturnNoMemory=3, kIOReturnIOError=4, kIOReturnNotAttached=5;
constexpr unsigned kIOMemoryDirectionOutIn=0;
struct IOAddressSegment { uint64_t address, length; };
struct IODMACommand {};
struct IOBufferMemoryDescriptor {
    void *data;
    uint64_t size;
    static inline std::vector<IOBufferMemoryDescriptor *> allocated;
    static int Create(unsigned, uint64_t bytes, uint64_t, IOBufferMemoryDescriptor **out) {
        *out=new IOBufferMemoryDescriptor{calloc(1,bytes),bytes};
        allocated.push_back(*out); return 0;
    }
    void SetLength(uint64_t bytes) { assert(bytes==size); }
    void GetAddressRange(IOAddressSegment *out) { *out={uint64_t(data),size}; }
    void release() { allocated.erase(std::find(allocated.begin(),allocated.end(),this)); free(data); delete this; }
};
#define REG_SET_FIELD(value, reg, field, val) \
    (((value) & ~(reg##__##field##_MASK)) | (((uint32_t(val)) << reg##__##field##__SHIFT) & reg##__##field##_MASK))
namespace amdgpu {
struct FakePCI {
    std::vector<uint8_t> vram=std::vector<uint8_t>(0x200000);
    bool drop=false;
    void MemoryWrite32(uint32_t, uint64_t off, uint32_t v) {
        assert(off+4<=vram.size()); if (!drop) memcpy(vram.data()+off,&v,4);
    }
    void MemoryRead32(uint32_t, uint64_t off, uint32_t *v) {
        assert(off+4<=vram.size()); memcpy(v,vram.data()+off,4);
    }
    void MemoryRead64(uint32_t, uint64_t off, uint64_t *v) {
        assert(off+8<=vram.size()); memcpy(v,vram.data()+off,8);
    }
};
struct DeviceContext {
    FakePCI *pci;
    IPBaseTable ip;
    DoorbellState doorbell;
    uint64_t bar0Size=0x200000;
    uint32_t bar0MemIndex=0;
};
struct GMCContext { uint64_t vram_start=0x8000000000; VRAMBumpAllocator vram_alloc; };
struct PSPContext {};
struct Write { uint32_t selection, reg, value; };
static std::vector<Write> writes;
static uint32_t selected;
static uint32_t SOC15_REG_OFFSET_BIDX(const DeviceContext &dev, IPBlock ip, unsigned base, uint32_t off) {
    return dev.ip.getBase(ip,base)+off;
}
static uint32_t RREG32(const DeviceContext &dev, uint32_t reg) {
    if (reg==dev.ip.getBase(IPBlock::GC,1)+MESRegs::CP_MES_GP3_LO.offset) return 0x0102708b;
    return 0;
}
static void WREG32(const DeviceContext &, uint32_t reg, uint32_t value) {
    writes.push_back({selected,reg,value});
    if (reg==0xa000+MESRegs::GRBM_GFX_CNTL.offset) selected=value;
}
static void amdgpu_hdp_flush(const DeviceContext &) {}
static void IOSleep(unsigned ms) { assert(ms==1); }
}
#include "amdgpu_vram_io.h"
#include "mes_header_under_test.inc"
using namespace amdgpu;
struct Submission { MESPipe pipe; uint32_t statusDW; std::array<uint32_t,64> words; };
static std::vector<Submission> submissions;
static size_t failSubmission=SIZE_MAX;
namespace amdgpu {
static void mes_log_queue_state(const DeviceContext &, const MESInstance &, uint32_t) {}
kern_return_t mes_submit_pkt(const DeviceContext &, MESContext &, MESPipe pipe,
    const uint32_t *pkt, uint32_t status, uint64_t timeout) {
    assert(timeout==2000000);
    Submission s{pipe,status,{}}; memcpy(s.words.data(),pkt,256); submissions.push_back(s);
    return submissions.size()==failSubmission ? 99 : 0;
}
kern_return_t mes_query_sched_status(const DeviceContext &dev, MESContext &mes, MESPipe pipe) {
    MES_QueryStatus pkt{}; pkt.header.u32All=mes_api_header(1,11,64);
    return mes_submit_pkt(dev,mes,pipe,reinterpret_cast<const uint32_t *>(&pkt),2,2000000);
}
}
#define MES_LOG(...) do {} while(0)
#include "mes_startup_under_test.inc"
#include "mes_mqd_checks.inc"
static_assert(sizeof(MES_AddQueue)==sizeof(MESAPI__ADD_QUEUE));
#define CHECK_ADD(field) static_assert(offsetof(MES_AddQueue,field)==offsetof(MESAPI__ADD_QUEUE,field))
CHECK_ADD(pipe_id); CHECK_ADD(queue_id); CHECK_ADD(mqd_addr); CHECK_ADD(wptr_addr);
CHECK_ADD(doorbell_offset); CHECK_ADD(queue_type); CHECK_ADD(api_status);
static_assert(kMESQueueType_SCHQ==MES_QUEUE_TYPE_SCHQ);

static void checkMQD(const FakePCI &pci, const MESInstance &inst) {
    v12_compute_mqd expected{};
    expected.header=0xc0310800;
    expected.compute_pipelinestat_enable=1;
    expected.compute_static_thread_mgmt_se0=expected.compute_static_thread_mgmt_se1=
    expected.compute_static_thread_mgmt_se2=expected.compute_static_thread_mgmt_se3=0xffffffff;
    expected.compute_misc_reserved=7;
    expected.cp_mqd_base_addr_lo=uint32_t(inst.mqd_bus);
    expected.cp_mqd_base_addr_hi=uint32_t(inst.mqd_bus>>32);
    expected.cp_hqd_active=1;
    expected.cp_hqd_persistent_state=regCP_HQD_PERSISTENT_STATE_DEFAULT;
    expected.cp_hqd_pq_base_lo=uint32_t(inst.ring_bus>>8);
    expected.cp_hqd_pq_base_hi=uint32_t(inst.ring_bus>>40);
    expected.cp_hqd_pq_rptr_report_addr_lo=uint32_t(inst.wb_bus);
    expected.cp_hqd_pq_rptr_report_addr_hi=uint32_t(inst.wb_bus>>32);
    expected.cp_hqd_pq_wptr_poll_addr_lo=uint32_t(inst.wb_bus+0x40);
    expected.cp_hqd_pq_wptr_poll_addr_hi=uint32_t(inst.wb_bus>>32);
    expected.cp_hqd_pq_doorbell_control=0x40000000|(inst.doorbell_index<<2);
    // 16K dwords, 1024-dword RPTR block; privileged KMD queue, no RPTR updates.
    expected.cp_hqd_pq_control=0xd830890d;
    expected.cp_mqd_control=regCP_MQD_CONTROL_DEFAULT;
    expected.cp_hqd_ib_control=regCP_HQD_IB_CONTROL_DEFAULT;
    expected.cp_hqd_eop_base_addr_lo=uint32_t(inst.eop_bus>>8);
    expected.cp_hqd_eop_base_addr_hi=uint32_t(inst.eop_bus>>40);
    expected.cp_hqd_eop_control=8;
    expected.reserved_184=1<<15;
    assert(memcmp(inst.mqd_cpu,&expected,sizeof(expected))==0);
    assert(memcmp(pci.vram.data()+inst.mqd_bus-inst.vram_base,&expected,sizeof(expected))==0);
}
static void cleanup() {
    while (!IOBufferMemoryDescriptor::allocated.empty()) IOBufferMemoryDescriptor::allocated.back()->release();
    writes.clear(); submissions.clear(); selected=0;
}
int main() {
    FakePCI pci; DeviceContext dev{&pci,{}, {}};
    dev.ip.setBase(IPBlock::GC,0,0x1260); dev.ip.setBase(IPBlock::GC,1,0xa000);
    dev.ip.setBase(IPBlock::MMHUB,0,0x1a000); dev.ip.setBase(IPBlock::OSSSYS,0,0x10a0);
    dev.doorbell.index.mes_ring0=0x20;
    PSPContext psp; MESContext mes{}; GMCContext gmc;
    gmc.vram_alloc.init(gmc.vram_start+0x4000,0x1fc000);
    assert(mes_init_full(dev,psp,gmc,mes)==kIOReturnNotReady && writes.empty());
    mes_set_uc_start_addr(mes,MESPipe::Sched,0x100001000);
    assert(mes_init_full(dev,psp,gmc,mes)==kIOReturnNotReady && writes.empty());
    mes_set_uc_start_addr(mes,MESPipe::KIQ,0x100001000);
    assert(mes_init_full(dev,psp,gmc,mes)==0);
    assert(mes.pipe[0].enabled && mes.pipe[1].enabled && selected==0);
    assert(mes.pipe[0].doorbell_index==0x40 && mes.pipe[1].doorbell_index==0x42);
    assert(submissions.size()==6);
    const unsigned opcodes[]={0,19,2,0,19,11};
    for (size_t i=0;i<6;++i) {
        assert(submissions[i].pipe==(i<3?MESPipe::KIQ:MESPipe::Sched));
        assert(((submissions[i].words[0]>>4)&255)==opcodes[i]);
    }
    auto kiq=*reinterpret_cast<const MES_SetHwResources *>(submissions[0].words.data());
    assert(!kiq.vmid_mask_gfxhub && !kiq.compute_hqd_mask[0] && !kiq.aggregated_doorbells[0]);
    assert(kiq.g_sch_ctx_gpu_mc_ptr==mes.pipe[1].sch_ctx_bus);
    assert(kiq.query_status_fence_gpu_mc_ptr==mes.pipe[1].status_fence_bus);
    assert(mes.pipe[0].sch_ctx_bus!=mes.pipe[1].sch_ctx_bus);
    assert(mes.pipe[0].status_fence_bus!=mes.pipe[1].status_fence_bus);
    assert(mes.pipe[0].resource_1_bus!=mes.pipe[1].resource_1_bus);
    MESAPI__ADD_QUEUE mapped{};
    mapped.header.type=1; mapped.header.opcode=MES_SCH_API_ADD_QUEUE; mapped.header.dwsize=64;
    mapped.doorbell_offset=0x40; mapped.mqd_addr=mes.pipe[0].mqd_bus;
    mapped.wptr_addr=mes.pipe[0].ring_wptr_gpu_addr;
    mapped.queue_type=MES_QUEUE_TYPE_SCHQ; mapped.map_legacy_kq=1;
    assert(memcmp(&mapped,submissions[2].words.data(),256)==0);
    assert(submissions[2].statusDW*4==offsetof(MESAPI__ADD_QUEUE,api_status));
    for (const auto &inst:mes.pipe) checkMQD(pci,inst);
    unsigned pcWrites=0, activeWrites=0;
    for (const auto &w:writes) {
        if (w.reg==0xa000+MESRegs::CP_MES_PRGRM_CNTR_START.offset) { ++pcWrites; assert(w.value==0x40000400); }
        if (w.reg==0x1260+MESRegs::CP_HQD_ACTIVE.offset) { ++activeWrites; assert(w.selection==13); }
    }
    assert(pcWrites==2 && activeWrites==1); // no direct SCHED activation
    // GFX kernel mapping uses the same ABI with queue type GFX, on KIQ.
    assert(mes_map_legacy_queue(dev,mes,kMESQueueType_GFX,0,0,0,0x8000100000,0x8000200040)==0);
    mapped.queue_type=MES_QUEUE_TYPE_GFX; mapped.doorbell_offset=0;
    mapped.mqd_addr=0x8000100000; mapped.wptr_addr=0x8000200040;
    assert(submissions.back().pipe==MESPipe::KIQ);
    assert(memcmp(&mapped,submissions.back().words.data(),256)==0);
    const auto before=submissions.size();
    assert(mes_map_legacy_queue(dev,mes,4,0,0,0,0x8000100000,0x8000200040)==kIOReturnBadArgument);
    assert(mes_map_legacy_queue(dev,mes,0,0,0,1,0x8000100000,0x8000200040)==kIOReturnBadArgument);
    assert(mes_map_legacy_queue(dev,mes,0,0,0,0,0,0x8000200040)==kIOReturnBadArgument);
    mes.pipe[1].resource_1_bus=0;
    assert(mes_map_legacy_queue(dev,mes,0,0,0,0,0x8000100000,0x8000200040)==kIOReturnNotReady);
    assert(submissions.size()==before);
    cleanup();
    // Every rejected bootstrap message stops the chain before later work.
    for (failSubmission=1;failSubmission<=6;++failSubmission) {
        mes={}; gmc={}; gmc.vram_alloc.init(gmc.vram_start+0x4000,0x1fc000);
        mes_set_uc_start_addr(mes,MESPipe::Sched,0x1000);
        mes_set_uc_start_addr(mes,MESPipe::KIQ,0x1000);
        assert(mes_init_full(dev,psp,gmc,mes)==99);
        assert(submissions.size()==failSubmission);
        cleanup();
    }
    // A failed MQD upload cannot send MAP_QUEUE or activate an HQD.
    mes={}; gmc={}; gmc.vram_alloc.init(gmc.vram_start+0x4000,0x1fc000);
    assert(mes_alloc_storage(dev,mes.pipe[0],gmc,MESPipe::Sched)==0);
    mes.pipe[0].enabled=true;
    pci.drop=true;
    assert(mes_queue_init(dev,mes,MESPipe::Sched)==kIOReturnIOError);
    assert(submissions.empty() && writes.empty()); cleanup();
    puts("MES startup: dual-pipe enable, Linux MQD/ADD_QUEUE bytes, private resources, KIQ mapping and failure gates pass");
}
