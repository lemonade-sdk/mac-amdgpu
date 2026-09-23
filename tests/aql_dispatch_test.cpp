#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "amdgpu_aql_packets.h"
#include "amdgpu_vram.h"
#include "amdgpu_ip.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/v12_structs.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h"
using namespace amdgpu;
#include "aql_mqd_offsets.inc"
static_assert(sizeof(AQLComputeMQD)==sizeof(v12_compute_mqd));
using kern_return_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnBusy=1,kIOReturnBadArgument=2,
    kIOReturnUnsupported=3,kIOReturnNotReady=4,kIOReturnNoMemory=5,
    kIOReturnIOError=6,kIOReturnTimeout=7,kIOReturnNotAttached=8;
static unsigned mode, maps, unmaps, bells, writes;
static uint64_t now;
static bool persistent=false;
static unsigned persistentSlot=1;
struct PCI {
    alignas(64) uint8_t memory[16384]{};
    void MemoryWrite32(unsigned bar,uint64_t offset,uint32_t value) {
        assert(bar==0 && offset+4<=sizeof(memory)); ++writes;
        if (mode!=1 && !(mode==3 && maps)) memcpy(memory+offset,&value,4);
    }
    void MemoryRead32(unsigned bar,uint64_t offset,uint32_t *value) {
        assert(bar==0 && offset+4<=sizeof(memory)); memcpy(value,memory+offset,4);
    }
    void MemoryRead64(unsigned bar,uint64_t offset,uint64_t *value) {
        assert(bar==0 && offset+8<=sizeof(memory)); memcpy(value,memory+offset,8);
        if (mode==8 && bells) *value=UINT64_MAX;
    }
    void MemoryWrite64(unsigned bar,uint64_t offset,uint64_t value) {
        if (persistent) {assert(bar==1 && offset==uint64_t(0x80+persistentSlot*2)*4);++bells;return;}
        assert(bar==1 && offset==0x200 && value==0); ++bells;
        uint64_t wptr; memcpy(&wptr,memory+kAQLMetadataOffset+offsetof(amd_queue_t,write_dispatch_id),8);
        assert(wptr==1); // doorbell 0 publishes first packet, shadow counts 1
        auto *completion=reinterpret_cast<amd_signal_t *>(memory+kAQLCompletionOffset);
        auto *inactive=reinterpret_cast<amd_signal_t *>(memory+kAQLInactiveOffset);
        auto *queue=reinterpret_cast<amd_queue_t *>(memory+kAQLMetadataOffset);
        completion->value=mode==4 ? 1 : mode==9 ? 2 : 0;
        if (mode==5) inactive->value=2;
        queue->read_dispatch_id=mode==7 ? 0 : 1;
    }
};
namespace amdgpu {
struct DeviceContext { PCI *pci; uint64_t bar0Size=16384,bar2Size=0x200000; unsigned bar0MemIndex=0,bar2MemIndex=1; IPBaseTable ip{}; };
struct GMCContext { uint64_t vram_start=0x8000000000; VRAMBumpAllocator vram_alloc; };
struct MESInstance { bool enabled=true,inited=true,submission_pending=false; };
struct MESContext { bool uni_mes_active=true; MESInstance pipe[2]; };
struct GFXConfig { unsigned max_shader_engines=4,max_sh_per_se=1,num_active_cus=32; uint32_t active_cu_bitmap[4][2]={{255,0},{255,0},{255,0},{255,0}}; };
#include "aql_context.inc"
static int mes_map_legacy_queue(DeviceContext &dev,MESContext &,unsigned type,unsigned pipe,unsigned queue,
    unsigned doorbell,uint64_t base,uint64_t wptr) {
    if (persistent) {
        assert(type==1 && pipe==0 && queue==persistentSlot && doorbell==0x80+persistentSlot*2);
        assert(wptr==0x110000000ull+offsetof(amd_queue_t,write_dispatch_id));
        ++maps;return mode==2 ? kIOReturnTimeout : 0;
    }
    assert(type==1 && pipe==0 && queue==0 && doorbell==0x80 && base==0x8000000000);
    assert(wptr==base+kAQLMetadataOffset+offsetof(amd_queue_t,write_dispatch_id));
    const auto *q=reinterpret_cast<const amd_queue_t *>(dev.pci->memory+kAQLMetadataOffset);
    assert(!q->write_dispatch_id && !q->read_dispatch_id);
    ++maps; return mode==2 ? kIOReturnTimeout : 0;
}
static int mes_unmap_legacy_queue(DeviceContext &,MESContext &,unsigned type,unsigned pipe,unsigned queue,unsigned doorbell) {
    assert(type==1 && !pipe && queue==(persistent ? persistentSlot : 0) && doorbell==0x80+queue*2); ++unmaps;
    return mode==6 ? kIOReturnTimeout : 0;
}
static void amdgpu_hdp_flush(DeviceContext &) {}
static uint64_t test_clock(int) { return now; }
static void IOSleep(unsigned ms) { now+=uint64_t(ms)*1000000; }
}
#include "amdgpu_vram_io.h"
#define clock_gettime_nsec_np test_clock
#define CLOCK_UPTIME_RAW 0
namespace amdgpu {
#include "aql_launch.inc"
}
#undef clock_gettime_nsec_np
int main() {
    AQLDispatchRequest r{}; r.version=1;r.codeHandle=1;r.kernargHandle=2;r.kernargBytes=12;r.timeoutUS=100000;
    r.groups[0]=4;r.groups[1]=r.groups[2]=1;r.threads[0]=32;r.threads[1]=r.threads[2]=1;
    alignas(64) uint8_t staging[16384]; const uint32_t masks[]={255,63,15,3};
    const uint64_t base=0x8000000000,descriptor=base+0x10000000,kernarg=base+0x10004000;
    assert(aql_build_storage(staging,base,descriptor,kernarg,r,masks,22));
    const auto &m=*reinterpret_cast<const v12_compute_mqd *>(staging);
    assert(m.header==0xc0310800 && m.compute_static_thread_mgmt_se2==15);
    assert(m.cp_hqd_persistent_state==(CP_HQD_PERSISTENT_STATE__PRELOAD_REQ_MASK | (0x55<<CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE__SHIFT)));
    assert(m.cp_hqd_quantum==(CP_HQD_QUANTUM__QUANTUM_EN_MASK | CP_HQD_QUANTUM__QUANTUM_SCALE_MASK | (1<<CP_HQD_QUANTUM__QUANTUM_DURATION__SHIFT)));
    assert(m.cp_hqd_pq_control==(9 | (5<<CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE__SHIFT) |
        CP_HQD_PQ_CONTROL__NO_UPDATE_RPTR_MASK | CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK |
        (2<<CP_HQD_PQ_CONTROL__SLOT_BASED_WPTR__SHIFT) | CP_HQD_PQ_CONTROL__QUEUE_FULL_EN_MASK));
    assert(m.cp_hqd_pq_doorbell_control==(CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK |
        CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_BIF_DROP_MASK | (0x80<<CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT)));
    assert(m.cp_hqd_ib_control==(3<<CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE__SHIFT));
    assert(m.cp_hqd_hq_status0==(1u<<14) && m.cp_hqd_vmid==0 && m.cp_hqd_active==1);
    assert(m.cp_hqd_eop_control==9 && m.cp_hqd_aql_control==1 && m.cp_mqd_control==CP_MQD_CONTROL__PRIV_STATE_MASK);
    const auto &q=*reinterpret_cast<const amd_queue_t *>(staging+kAQLMetadataOffset);
    assert(q.queue_properties==2 && q.caps==0 && q.read_dispatch_id_field_base_byte_offset==offsetof(amd_queue_t,read_dispatch_id));
    assert(q.hsa_queue.size==64 && q.max_cu_id==21);
    const auto &p=*reinterpret_cast<const hsa_kernel_dispatch_packet_t *>(staging+kAQLRingOffset);
    assert(p.kernel_object==descriptor && reinterpret_cast<uintptr_t>(p.kernarg_address)==kernarg);
    assert(p.grid_size_x==128 && p.workgroup_size_x==32 && p.setup==3 && p.header==0x1402);
    assert(p.completion_signal.handle==base+kAQLCompletionOffset);
    for (unsigned i=1;i<64;++i) {
        const auto *slot=reinterpret_cast<const uint32_t *>(staging+kAQLRingOffset+64*i);
        assert(slot[0]==HSA_PACKET_TYPE_INVALID);
        for (unsigned j=1;j<16;++j) assert(!slot[j]);
    }
    for (unsigned bad=0;bad<10;++bad) {
        auto invalid=r;
        switch (bad) {
        case 0: invalid.version=2;break;case 1: invalid.flags=1;break;
        case 2: invalid.descriptorOffset=32;break;case 3: invalid.kernargOffset=8;break;
        case 4: invalid.timeoutUS=1000001;break;case 5: invalid.reserved=1;break;
        case 6: invalid.threads[2]=1024;break;case 7: invalid.groups[0]=UINT32_MAX;break;
        case 8: invalid.kernargBytes=4*1024*1024+1;break;case 9: invalid.codeHandle=0;break;
        }
        assert(!aql_dispatch_shape(invalid));
    }
    assert(!aql_build_storage(staging,base+4,descriptor,kernarg,r,masks,22));
    assert(!aql_build_storage(staging,base,descriptor+4,kernarg,r,masks,22));
    assert(!aql_build_storage(staging,base,descriptor,kernarg+4,r,masks,22));
    for (mode=0;mode<=9;++mode) {
        PCI pci; DeviceContext dev{&pci}; dev.ip.version[0]={12,0,1};
        GMCContext gmc; gmc.vram_alloc.init(base,16384); MESContext mes; GFXConfig gfx;
        maps=unmaps=bells=writes=0; now=0;
        AQLLaunch launch{}; AQLLaunchResult result{};
        const auto status=aql_launch(dev,gmc,mes,gfx,launch,descriptor,kernarg,r,result);
        if (!mode) assert(status==0 && result.stage==5 && !result.completion && result.readIndex==1 && maps==1 && unmaps==1 && bells==1);
        else assert(status!=0);
        if (mode>=2) {
            assert(launch.retained && gmc.vram_alloc.bytes_used()==16384);
            const auto before=writes;
            assert(aql_launch(dev,gmc,mes,gfx,launch,descriptor,kernarg,r,result)==kIOReturnBusy && writes==before);
        } else assert(!launch.retained && gmc.vram_alloc.bytes_used()==0);
        if (mode==4) assert(now==100000000);
        if (mode>=2 && mode<=5) assert(unmaps==0);
    }
    persistent=true;
    for (persistentSlot=1;persistentSlot<=7;++persistentSlot) {
        for (mode=0;mode<=6;++mode) {
            if (mode==3 || mode==4 || mode==5) continue;
            PCI pci;DeviceContext dev{&pci};dev.ip.version[0]={12,0,1};
            GMCContext gmc;gmc.vram_alloc.init(base,16384);MESContext mes;GFXConfig gfx;
            alignas(64) amd_queue_t metadata{};
            metadata.hsa_queue.base_address=reinterpret_cast<void *>(0x110004000ull);metadata.hsa_queue.size=64;
            metadata.queue_properties=2;metadata.read_dispatch_id_field_base_byte_offset=offsetof(amd_queue_t,read_dispatch_id);
            PersistentAQLQueue q{};maps=unmaps=bells=writes=0;
            auto status=aql_queue_open(dev,gmc,mes,gfx,q,0x110004000,0x110000000,&metadata,64,persistentSlot);
            if (mode==1) {assert(status!=0 && !q.retained && !gmc.vram_alloc.bytes_used() && !maps);continue;}
            if (mode==2) {assert(status!=0 && q.retained && gmc.vram_alloc.bytes_used()==16384);continue;}
            assert(status==0 && q.mapped && !q.retained && maps==1);
            assert(aql_queue_kick(dev,q,0)==kIOReturnBadArgument && !bells);
            metadata.write_dispatch_id=70;metadata.read_dispatch_id=64;
            assert(aql_queue_kick(dev,q,69)==0 && bells==1);
            assert(aql_queue_kick(dev,q,63)==0 && bells==1); // stale multi-producer doorbell
            assert(aql_queue_kick(dev,q,70)==kIOReturnBadArgument && bells==1);
            assert(aql_queue_kick(dev,q,UINT64_MAX)==kIOReturnBadArgument && bells==1);
            status=aql_queue_close(dev,gmc,mes,q);
            if (mode==6) {
                assert(status==kIOReturnTimeout && q.retained && q.mapped && gmc.vram_alloc.bytes_used()==16384);
                assert(aql_queue_kick(dev,q,69)==kIOReturnNotReady);
            } else assert(status==0 && !q.mapped && !gmc.vram_alloc.bytes_used() && unmaps==1);
        }
    }
    puts("AQL: Linux MQD/register layout, ROCr packet/metadata, publication, completion, unmap and failure retention passed");
}
