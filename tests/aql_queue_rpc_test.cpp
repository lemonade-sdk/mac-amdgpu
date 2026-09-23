#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
using kern_return_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnBusy=3,kIOReturnNoResources=4,kIOReturnTimeout=5;
constexpr uint32_t kBODomainGTT=2;
constexpr unsigned kMacAMDGPUMethodAQLQueueCreate=56,kMacAMDGPUMethodAQLQueueKick=57,kMacAMDGPUMethodAQLQueueDestroy=58;
#define MACAMDGPU_LOG(...) ((void)0)
namespace amdgpu {
constexpr unsigned kPersistentAQLQueues=7;
enum class BringupStage {None,SDMAInit};
struct PersistentAQLQueue {
    void *owner=nullptr;uint64_t handle=0,ringHandle=0,metadataHandle=0;bool retained=false;
};
unsigned opened=0,kicked=0,closed=0;int failure=0;bool retainFailure=false;
int aql_queue_open(int,int,int,int,PersistentAQLQueue &q,uint64_t ring,uint64_t metadata,void *cpu,uint32_t packets,unsigned slot) {
    assert(ring && metadata && cpu && packets==64 && slot>=1 && slot<=7);++opened;
    q.retained=retainFailure;return failure;
}
int aql_queue_kick(int,PersistentAQLQueue &,uint64_t packet) {assert(packet==9);++kicked;return 0;}
int aql_queue_close(int,int,int,PersistentAQLQueue &q) {
    ++closed;if (failure) {q.retained=true;return failure;}q={};return 0;
}
}
struct Buffer {
    uint32_t domain=kBODomainGTT;struct {bool ready=true;} gttBinding;
    uint64_t size=16384,gpu_va=0x110000000ull;void *cpu_addr=nullptr;
};
struct Client {std::array<Buffer,16> buffers;};
Buffer *mac_amdgpu_bo_lookup(Client *owner,uint64_t handle) {return handle && handle<=16 ? &owner->buffers[handle-1] : nullptr;}
struct State {
    bool shutdownBlocked=false;uint64_t nextAQLHandle=0;
    struct {
        int device=0,gmc=0,mes=0,gfx=0;amdgpu::BringupStage reached=amdgpu::BringupStage::SDMAInit;
        amdgpu::PersistentAQLQueue aqlQueues[7];
    } bringup;
};
struct Driver {State *ivars;};
struct Args {
    uint64_t *scalarInput,*scalarOutput;uint32_t scalarInputCount=3,scalarOutputCount=2;
    void *structureInput=nullptr,*structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    size_t structureOutputMaximumSize=0;
};
int call(Driver *driver,Client *ivars,unsigned selector,Args *arguments) {
    switch (selector) {
#include "aql_queue_rpc.inc"
    default:return kIOReturnBadArgument;
    }
}
int main() {
    State state;Driver driver{&state};Client first,second;
    std::array<uint32_t,4096> ring{};
    for (unsigned i=0;i<64;++i) ring[i*16]=1;
    for (auto *client:{&first,&second}) for (auto &buffer:client->buffers) buffer.cpu_addr=ring.data();
    uint64_t in[3]={1,2,64},out[2]{};Args args{in,out};
    auto create=[&](Client &owner) {args.scalarInputCount=3;args.scalarOutputCount=2;return call(&driver,&owner,56,&args);};
    auto action=[&](Client &owner,unsigned selector,uint64_t handle) {
        in[0]=handle;in[1]=9;args.scalarInputCount=selector==57 ? 2 : 1;args.scalarOutputCount=1;
        return call(&driver,&owner,selector,&args);
    };
    assert(create(first)==0 && !out[0] && out[1]==1);
    assert(create(first)==kIOReturnBusy && amdgpu::opened==1);
    assert(action(second,57,1)==kIOReturnBadArgument && !amdgpu::kicked);
    assert(action(second,58,1)==kIOReturnBadArgument && !amdgpu::closed);
    assert(action(first,57,1)==0 && !out[0] && amdgpu::kicked==1);
    assert(action(first,58,1)==0 && amdgpu::closed==1);
    assert(action(first,58,1)==kIOReturnBadArgument);
    in[0]=1;in[1]=2;
    // Nonces continue across queue destruction and across client owners.
    assert(create(second)==0 && out[1]==2);
    assert(action(first,57,2)==kIOReturnBadArgument);
    assert(action(second,58,2)==0);
    for (uint64_t i=0;i<7;++i) {
        in[0]=i*2+1;in[1]=i*2+2;assert(create(first)==0 && out[1]==i+3);
    }
    in[0]=15;in[1]=16;assert(create(second)==kIOReturnNoResources);
    for (uint64_t i=0;i<7;++i) assert(action(first,58,i+3)==0);
    in[0]=1;in[1]=2;state.nextAQLHandle=UINT64_MAX;assert(create(first)==kIOReturnNoResources);
    state.nextAQLHandle=9;
    for (unsigned malformed=0;malformed<8;++malformed) {
        auto broken=args;broken.scalarInputCount=3;broken.scalarOutputCount=2;
        switch(malformed) {
        case 0:broken.scalarInput=nullptr;break;case 1:broken.scalarInputCount=2;break;
        case 2:broken.scalarOutput=nullptr;break;case 3:broken.scalarOutputCount=1;break;
        case 4:broken.structureInput=&state;break;case 5:broken.structureInputDescriptor=&state;break;
        case 6:broken.structureOutputDescriptor=&state;break;case 7:broken.structureOutputMaximumSize=1;break;
        }
        assert(call(&driver,&first,56,&broken)==kIOReturnBadArgument);
    }
    in[0]=0;assert(create(first)==kIOReturnBadArgument);in[0]=1;
    for (uint64_t count:{0,63,65,8192}) {in[2]=count;assert(create(first)==kIOReturnBadArgument);}in[2]=64;
    auto &buffer=first.buffers[0];buffer.domain=1;assert(create(first)==kIOReturnBadArgument);buffer.domain=2;
    buffer.gttBinding.ready=false;assert(create(first)==kIOReturnBadArgument);buffer.gttBinding.ready=true;
    buffer.size=4095;assert(create(first)==kIOReturnBadArgument);buffer.size=16384;
    ring[16]=2;assert(create(first)==kIOReturnBadArgument);ring[16]=1;
    state.bringup.reached=amdgpu::BringupStage::None;assert(create(first)==kIOReturnNotReady);
    state.bringup.reached=amdgpu::BringupStage::SDMAInit;
    for (bool retained:{false,true}) {
        amdgpu::failure=kIOReturnTimeout;amdgpu::retainFailure=retained;
        assert(create(first)==0 && out[0]==kIOReturnTimeout && !out[1]);
        assert(state.shutdownBlocked==retained);
        assert(bool(state.bringup.aqlQueues[0].owner)==retained);
    }
    state={};amdgpu::failure=0;amdgpu::retainFailure=false;
    assert(create(first)==0 && out[1]==1);amdgpu::failure=kIOReturnTimeout;
    assert(action(first,58,1)==0 && out[0]==kIOReturnTimeout && state.shutdownBlocked);
    assert(state.bringup.aqlQueues[0].owner==&first);
    puts("Persistent queue RPC: shapes, client ownership, stale handles, seven-slot capacity, BO validation and failure retention pass");
}
