#include "amdgpu_atomic_diagnostics.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
using kern_return_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnNotReady=2,kIOReturnIOError=3;
constexpr unsigned kBODomainGTT=2,kIOPCICapabilityIDPCIExpress=0x10;
struct PCI {
    bool valid=true; unsigned reads=0;
    int FindPCICapability(unsigned,unsigned,uint64_t *cap) {*cap=0x80;return valid ? 0 : 1;}
    void ConfigurationRead16(uint64_t offset,uint16_t *out) {
        ++reads;*out=offset==0x80 ? 0x10 : offset==0x82 ? 2 : 0x40;
    }
    void ConfigurationRead32(uint64_t,uint32_t *out) {++reads;*out=0xe0;}
};
namespace amdgpu {
enum class BringupStage {None,SDMAInit};
struct Device {PCI *pci;};
struct GART {bool enabled=true;uint64_t pageTableVRAMOffset=0x700000,pageTableSize=4096;};
struct Binding {
    bool ready=true,dmaPrepared=true;GART *owner=nullptr;
    void *sysmemBuffer=nullptr,*dmaCommand=nullptr;
    uint64_t sizeBytes=16384,gartMCAddr=0x110000000,busAddr=0x81000000,gartOffset=0;
};
struct PersistentAQLQueue {
    void *owner=nullptr;uint64_t handle=9;bool mapped=true,retained=false;
    struct {uint64_t gpu_va=0x8001800000,size=16384;} storage;
};
struct Hub {bool inited=true;IPBlock ip=IPBlock::GC;uint32_t ctx0_pt_base_lo=1,ctx0_pt_base_hi=2,ctx0_cntl=3;};
unsigned reads=0;bool readFailure=false;uint64_t actualPTE=0x81000000|PTEFlags::SYSMEM_RW;
int vram_read_fence64(const Device &,uint64_t offset,uint64_t *out) {
    assert(offset>=0x700000 && offset<0x701000);++reads;*out=actualPTE;return readFailure ? kIOReturnIOError : 0;
}
int vram_read_fence32(const Device &,uint64_t offset,uint32_t *out) {
    assert(offset==0x1800000+160*4);++reads;*out=1u<<29;return 0;
}
uint32_t SOC15_REG_OFFSET(const Device &,IPBlock,uint32_t reg) {return reg;}
uint32_t RREG32(const Device &,uint32_t reg) {++reads;return reg==1 ? 0x700001 : reg==2 ? 0 : 0x3fffc01;}
}
struct Buffer {
    unsigned domain=kBODomainGTT;void *gtt_buf=nullptr,*gtt_dma=nullptr,*cpu_addr=nullptr;
    uint64_t size=16384,gpu_va=0x110000000,gtt_bus_addr=0x81000000;
    amdgpu::Binding gttBinding;
};
struct Client {bool claimed=true;Buffer buffer;};
Buffer *mac_amdgpu_bo_lookup(Client *client,uint64_t handle) {return handle==5 ? &client->buffer : nullptr;}
struct State {
    bool pciOpen=true,stopping=false,shutdownBlocked=false,shutdownInProgress=false;
    struct {
        amdgpu::Device device{};amdgpu::GART gart;
        struct {bool inited=true;} gfx;
        struct {uint64_t vram_start=0x8000000000;amdgpu::Hub gfxhub;} gmc;
        amdgpu::BringupStage reached=amdgpu::BringupStage::SDMAInit;
        std::array<amdgpu::PersistentAQLQueue,1> aqlQueues;
    } bringup;
};
struct Driver {State *ivars;};
struct Args {
    uint64_t *scalarInput,*scalarOutput;unsigned scalarInputCount=4,scalarOutputCount=16;
    void *structureInput=nullptr,*structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    size_t structureOutputMaximumSize=0;
};
int call(Driver *driver,Client *ivars,Args *arguments) {
    auto &b=driver->ivars->bringup;
    switch(arguments->scalarInput[0]) {
#include "atomic_diagnostics_rpc.inc"
    default:return kIOReturnBadArgument;
    }
}
int main() {
    using namespace amdgpu::atomic_diag;
    Snapshot s{},unchanged{};std::memset(&unchanged,0xa5,sizeof(unchanged));
    for (uint64_t offset:{0ull,4096ull,8192ull,16376ull}) {
        assert(mapping_addresses(0x110000000,0x81000000,16384,8192,0x700000,4096,offset,s));
        assert(s.values[PTEVRAMOffset]==0x700010+(offset/4096)*8);
        assert(s.values[PTEExpected]==(((0x81000000+offset)&~uint64_t(4095))|amdgpu::PTEFlags::SYSMEM_RW));
    }
    for (unsigned bad=0;bad<8;++bad) {
        s=unchanged;
        uint64_t gpu=0x110000000,dma=0x81000000,size=16384,gart=0,table=0x700000,tableSize=4096,offset=0;
        switch(bad) {
        case 0:offset=1;break;case 1:offset=size;break;case 2:dma=1ull<<44;break;
        case 3:dma=(1ull<<44)-4096;break;case 4:gpu=(1ull<<47)-4096;break;
        case 5:gart=4096*512;break;case 6:table=UINT64_MAX-7;gart=4096;break;
        case 7:size=16383;break;
        }
        assert(!mapping_addresses(gpu,dma,size,gart,table,tableSize,offset,s));
        assert(std::memcmp(&s,&unchanged,sizeof(s))==0);
    }
    State state;PCI pci;state.bringup.device.pci=&pci;Driver driver{&state};Client client,other;
    auto &bo=client.buffer;auto &binding=bo.gttBinding;
    bo.gtt_buf=binding.sysmemBuffer=&client;bo.gtt_dma=binding.dmaCommand=&state;bo.cpu_addr=&bo;
    binding.owner=&state.bringup.gart;state.bringup.aqlQueues[0].owner=&client;
    uint64_t input[4]={7,5,0,9};Snapshot output{};Args args{input,output.values};
    assert(call(&driver,&client,&args)==0 && args.scalarOutputCount==16 && snapshot_valid(output,input[2]+bo.gpu_va));
    assert(output.values[ValidFields]==15 && output.values[PCIeDeviceControl2]==0x40);
    assert(output.values[MQDBackingHQStatus0]==1u<<29 && output.values[CPUCacheAttributes]==UINT64_MAX);
    // A valid but unexpectedly different PTE is evidence, not a fabricated expected readback.
    amdgpu::actualPTE^=0x1000;assert(call(&driver,&client,&args)==0);
    assert(output.values[PTEActual]!=output.values[PTEExpected]);amdgpu::actualPTE^=0x1000;
    pci.valid=false;state.bringup.gmc.gfxhub.inited=false;
    assert(call(&driver,&client,&args)==0 && output.values[ValidFields]==3 && snapshot_valid(output,bo.gpu_va));
    pci.valid=true;state.bringup.gmc.gfxhub.inited=true;
    for (unsigned bad=0;bad<12;++bad) {
        output=unchanged;Args broken=args;const unsigned before=amdgpu::reads+pci.reads;
        switch(bad) {
        case 0:broken.scalarInputCount=3;break;case 1:broken.scalarOutputCount=15;break;
        case 2:broken.structureInput=&state;break;case 3:client.claimed=false;break;
        case 4:state.shutdownBlocked=true;break;case 5:input[1]=6;break;
        case 6:binding.ready=false;break;case 7:binding.dmaPrepared=false;break;
        case 8:binding.owner=nullptr;break;case 9:state.bringup.aqlQueues[0].owner=&other;break;
        case 10:state.bringup.aqlQueues[0].retained=true;break;case 11:input[2]=16384;break;
        }
        assert(call(&driver,&client,&broken)!=0 && std::memcmp(&output,&unchanged,sizeof(output))==0);
        assert(amdgpu::reads+pci.reads==before);
        client.claimed=true;state.shutdownBlocked=false;input[1]=5;input[2]=0;
        binding.ready=binding.dmaPrepared=true;binding.owner=&state.bringup.gart;
        state.bringup.aqlQueues[0].owner=&client;state.bringup.aqlQueues[0].retained=false;
    }
    amdgpu::readFailure=true;assert(call(&driver,&client,&args)==kIOReturnIOError);
    assert(std::memcmp(&output,&unchanged,sizeof(output))==0);
    puts("Atomic diagnostics: production RPC ownership/readiness/bounds, real PTE mismatch, optional unknowns and 44-bit DMA boundaries passed");
}
