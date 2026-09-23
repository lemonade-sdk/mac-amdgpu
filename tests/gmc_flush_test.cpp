#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "amdgpu_ip.h"
#include "amdgpu_field_defs.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/mmhub/mmhub_4_1_0_offset.h"
using namespace amdgpu;
using kern_return_t=int;
enum { kIOReturnSuccess, kIOReturnNotReady, kIOReturnBadArgument, kIOReturnTimeout, kIOReturnNotAttached };
struct DeviceContext { IPBaseTable ip; };
struct GMCContext {};
struct HubContext {
    bool inited=true;
    IPBlock ip=IPBlock::MMHUB;
    uint32_t vm_invalidate_eng0_req=regMMVM_INVALIDATE_ENG0_REQ,vm_invalidate_eng0_ack=regMMVM_INVALIDATE_ENG0_ACK,
        vm_invalidate_eng0_sem=regMMVM_INVALIDATE_ENG0_SEM,
        vm_l2_bank_select_reserved_cid2=regMMVM_L2_BANK_SELECT_RESERVED_CID2,eng_distance=1;
};
static std::vector<uint32_t> events;
static unsigned polls;
static std::vector<uint32_t> writtenValues;
static bool semOK=true, ackOK=true, removed=false;
static uint32_t SOC15_REG_OFFSET(const DeviceContext &d,IPBlock b,uint32_t r) { return d.ip.getBase(b,0)+r; }
static uint32_t RREG32(const DeviceContext &,uint32_t r) { events.push_back(r); return 0x123; }
static void WREG32(const DeviceContext &,uint32_t r,uint32_t v) { events.push_back(r); writtenValues.push_back(v); }
static bool poll_reg(const DeviceContext &,uint32_t r,uint32_t,uint32_t,uint64_t,uint32_t *out) {
    events.push_back(r); ++polls; *out=removed?UINT32_MAX:0;
    return !removed && (polls==1?semOK:ackOK);
}
#define REG_SET_FIELD(v,r,f,x) (((v)&~r##__##f##_MASK)|(((x)<<r##__##f##__SHIFT)&r##__##f##_MASK))
#define GMC_LOG(...) do {} while(0)
#include "gmc_flush_under_test.inc"
int main() {
    DeviceContext d; GMCContext g; HubContext h;
    d.ip.setBase(IPBlock::MMHUB,0,0x1a000); d.ip.setBase(IPBlock::GC,0,0x1260);
    auto reset=[] { events.clear(); writtenValues.clear(); polls=0; semOK=ackOK=true; removed=false; };
    const uint32_t sem=0x1a000+h.vm_invalidate_eng0_sem+17, req=0x1a000+h.vm_invalidate_eng0_req+17, ack=0x1a000+h.vm_invalidate_eng0_ack+17, cid=0x1a000+h.vm_l2_bank_select_reserved_cid2;
    reset(); assert(gmc_flush_gpu_tlb(d,g,h,0,0)==0);
    assert(events==std::vector<uint32_t>({sem,req,ack,sem,cid,cid,cid}));
    assert(writtenValues==std::vector<uint32_t>({0xF80001u,0,0x2000123u}));
    reset(); semOK=false; assert(gmc_flush_gpu_tlb(d,g,h,0,0)==kIOReturnTimeout && events.size()==1);
    reset(); ackOK=false; assert(gmc_flush_gpu_tlb(d,g,h,0,0)==kIOReturnTimeout); assert(events[3]==sem);
    reset(); removed=true; assert(gmc_flush_gpu_tlb(d,g,h,0,0)==kIOReturnNotAttached && events.size()==1);
    reset(); assert(gmc_flush_gpu_tlb(d,g,h,16,0)==kIOReturnBadArgument && events.empty());
    reset(); h.ip=IPBlock::GC; assert(gmc_flush_gpu_tlb(d,g,h,0,0)==0 && events.size()==2 && polls==1);
    puts("GMC flush: MMHUB semaphore/request/ack/release/private invalidation, GFXHUB path and failures pass");
}
