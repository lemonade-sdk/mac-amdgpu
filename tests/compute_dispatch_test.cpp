#include <cassert>
#include <cstring>
#include <cstdio>
#include <map>
#include "amdgpu_compute_packets.h"
#include "amdgpu_vram.h"
#include "amdgpu_ip.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h"
using kern_return_t = int;
constexpr int kIOReturnSuccess=0, kIOReturnBusy=1, kIOReturnNotReady=2,
    kIOReturnUnsupported=3, kIOReturnBadArgument=4, kIOReturnNoSpace=5,
    kIOReturnNoMemory=6, kIOReturnIOError=7, kIOReturnTimeout=8, kIOReturnNotAttached=9;
static unsigned mode, submits, writes;
struct PCI {
    uint32_t memory[4096]{};
    void MemoryWrite32(unsigned, uint64_t off, uint32_t value) {
        assert(off < sizeof(memory)); ++writes;
        if (mode != 1) memory[off/4]=value;
    }
    void MemoryRead32(unsigned, uint64_t off, uint32_t *value) {
        assert(off < sizeof(memory)); *value=memory[off/4];
    }
    void MemoryRead64(unsigned, uint64_t off, uint64_t *value) {
        assert(off+8 <= sizeof(memory)); memcpy(value, reinterpret_cast<char *>(memory)+off, 8);
    }
};
namespace amdgpu {
struct DeviceContext { PCI *pci; uint64_t bar0Size=16384; unsigned bar0MemIndex=0; IPBaseTable ip{}; };
struct GMCContext { uint64_t vram_start=0x8000000000; VRAMBumpAllocator vram_alloc; };
struct CPContext { bool inited=true, ringReady=true; uint64_t wptr=0; };
struct GFXConfig {
    unsigned num_active_cus=64, max_shader_engines=4, max_sh_per_se=2;
    uint32_t active_cu_bitmap[4][2]={{255,255},{255,255},{255,255},{255,255}};
};
#include "dispatch_context.inc"
static void amdgpu_hdp_flush(DeviceContext &) {}
static uint32_t cp_ring_write(CPContext &cp, const uint32_t *ib, uint32_t count) {
    assert(count==4 && ((ib[0] >> 8) & 255)==0x3f);
    assert(!(ib[3] & 0xfff00000)); // no inherited VMID or compute-ring VALID bit
    assert((ib[1] & 31)==0 && (ib[3] & 7)==0);
    if (mode==2) return 0;
    cp.wptr+=4;
    return mode==4 ? 2 : 4; // retain even a partial staged submission
}
static int cp_submit_eop_test(DeviceContext &dev, CPContext &, uint64_t timeout, uint32_t *fence) {
    assert(timeout==100000); ++submits;
    unsigned masks=0;
    for (unsigned i=0;i+2<4096 && (dev.pci->memory[i]>>30)==3;) {
        const auto header=dev.pci->memory[i];
        if (((header>>8)&255)==0x76) {
            const auto reg=(dev.pci->memory[i+1]+0x2c00)*4;
            if (reg==0xb858 || reg==0xb85c || reg==0xb864 || reg==0xb868) {
                assert(dev.pci->memory[i+2]==0x00ff00ff);++masks;
            }
        }
        i+=((header>>16)&0x3fff)+2;
    }
    assert(masks==4);
    if(mode==3) return kIOReturnTimeout;
    *fence=submits; return 0;
}
}
#include "amdgpu_vram_io.h"
namespace amdgpu {
#include "dispatch_launch.inc"
}
int main() {
    using namespace amdgpu;
    ComputeDispatchRequest request{};
    request.version=1; request.codeHandle=1; request.codeBytes=256; request.timeoutUS=100000;
    request.groups[0]=19; request.groups[1]=3; request.groups[2]=2;
    request.threads[0]=32; request.threads[1]=4; request.threads[2]=1;
    request.userSGPRCount=2; request.rsrc1=0xc0000;
    request.rsrc2=(2<<1) | COMPUTE_PGM_RSRC2__TGID_X_EN_MASK;
    request.userSGPR[0]=0x1804000; request.userSGPR[1]=0x80;
    assert(compute_dispatch_shape(request));
    const uint32_t masks[4]={0xffff,0,0xff,1};
    uint32_t packets[kComputeSmokePacketCapacity]{};
    const auto n=compute_dispatch_packets(packets,0x8001800000,request,masks);
    assert(n && n <= kComputeSmokePacketCapacity);
    std::map<unsigned,unsigned> regs;
    unsigned dispatches=0, syncs=0, partials=0;
    for(unsigned i=0;i<n;) {
        const auto size=((packets[i]>>16)&0x3fff)+2;
        assert(i+size<=n);
        switch((packets[i]>>8)&255) {
        case 0x76: assert(size==3 && !dispatches); regs[(packets[i+1]+0x2c00)*4]=packets[i+2]; break;
        case 0x58: assert(size==8 && packets[i+7]==0xc3b1); ++syncs; break;
        case 0x15:
            assert(syncs==1 && size==5);
            for(unsigned j=0;j<3;++j) assert(packets[i+1+j]==request.groups[j]);
            assert(packets[i+4]==0x8045); ++dispatches; break;
        case 0x46: assert(dispatches==1 && packets[i+1]==0x407); ++partials; break;
        default: assert(false);
        }
        i+=size;
    }
    assert(dispatches==1 && syncs==2 && partials==1);
    assert(regs.at(0xb854)==COMPUTE_RESOURCE_LIMITS__SIMD_DEST_CNTL_MASK);
    assert(regs.at(0xb81c)==32 && regs.at(0xb820)==4 && regs.at(0xb824)==1);
    assert(regs.at(0xb84c)==request.rsrc2);
    for(unsigned i=0;i<16;++i) assert(regs.at(0xb900+4*i)==request.userSGPR[i]);
    for(unsigned failure=0;failure<12;++failure) {
        auto invalid=request;
        switch(failure) {
        case 0: invalid.version=3; break;
        case 1: invalid.flags=1; break;
        case 2: invalid.codeOffset=4; break;
        case 3: invalid.codeBytes=3; break;
        case 4: invalid.threads[2]=1024; break;
        case 5: invalid.groups[0]=0; break;
        case 6: invalid.rsrc1|=COMPUTE_PGM_RSRC1__PRIV_MASK; break;
        case 7: invalid.rsrc2|=COMPUTE_PGM_RSRC2__SCRATCH_EN_MASK; break;
        case 8: invalid.userSGPRCount=17; break;
        case 9: invalid.timeoutUS=1000001; break;
        case 10: invalid.userSGPR[15]=1; break;
        case 11: invalid.rsrc2|=129<<COMPUTE_PGM_RSRC2__LDS_SIZE__SHIFT; break;
        }
        assert(!compute_dispatch_packets(packets,0x8001800000,invalid,masks));
    }
    assert(compute_dispatch_packets(packets,(1ull<<48)-256,request,masks));
    assert(!compute_dispatch_packets(packets,1ull<<48,request,masks));
    auto v2=request;
    v2.version=2; v2.rsrc1=0xe00f0000; v2.rsrc3=0x10;
    assert(compute_dispatch_shape(v2));
    const auto v2Count=compute_dispatch_packets(packets,0x8001000000,v2,masks);
    bool resources=false, prefetch=false;
    for (unsigned i=0;i<v2Count;) {
        const auto size=((packets[i]>>16)&0x3fff)+2;
        if (((packets[i]>>8)&255)==0x76) {
            const auto reg=(packets[i+1]+0x2c00)*4;
            if (reg==0xb848) { assert(packets[i+2]==v2.rsrc1); resources=true; }
            if (reg==0xb8a0) { assert(packets[i+2]==v2.rsrc3); prefetch=true; }
        }
        i+=size;
    }
    assert(resources && prefetch);
    for (unsigned bad=0;bad<5;++bad) {
        auto invalid=v2;
        if (bad==0) invalid.version=1;
        if (bad==1) invalid.rsrc3|=COMPUTE_PGM_RSRC3__SHARED_VGPR_CNT_MASK;
        if (bad==2) invalid.rsrc3|=COMPUTE_PGM_RSRC3__GLG_EN_MASK;
        if (bad==3) invalid.rsrc1|=COMPUTE_PGM_RSRC1__PRIV_MASK;
        if (bad==4) invalid.reserved=1;
        assert(!compute_dispatch_shape(invalid));
    }
    for(mode=0;mode<=4;++mode) {
        PCI pci; DeviceContext dev{&pci}; dev.ip.version[0]={12,0,1};
        GMCContext gmc; gmc.vram_alloc.init(gmc.vram_start,16384); CPContext cp; GFXConfig gfx;
        ComputeLaunch launch{}; ComputeLaunchResult result{};
        const auto status=compute_launch(dev,gmc,cp,gfx,launch,0x8001000000,request,result);
        if(mode==0) assert(status==0 && result.stage==3 && result.fence && !launch.retained);
        else assert(status!=0);
        if(mode>=3) {
            assert(launch.retained && gmc.vram_alloc.bytes_used()==16384);
            const auto before=writes;
            assert(compute_launch(dev,gmc,cp,gfx,launch,0x8001000000,request,result)==kIOReturnBusy);
            assert(writes==before);
        } else assert(!launch.retained && gmc.vram_alloc.bytes_used()==0);
    }
    puts("General dispatch register oracle, validation and submission lifetime passed");
}
