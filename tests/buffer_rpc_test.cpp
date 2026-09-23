#include "amdgpu_buffer_io.h"
#include <cassert>
#include <cstring>
#include <vector>
using kern_return_t = int;
constexpr int kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnNotReady=2,
    kIOReturnNoMemory=3, kIOReturnTimeout=4, kIOReturnIOError=5, kIOReturnNotAttached=6;
constexpr int kMacAMDGPUMethodBOCopy=48, kMacAMDGPUMethodBOWrite=49, kMacAMDGPUMethodBORead=50;
constexpr int kBODomainVRAM=1;
struct OSData {
    std::vector<uint8_t> data;
    static OSData *withBytes(const void *p, size_t n) {
        auto *result=new OSData; result->data.resize(n); memcpy(result->data.data(),p,n); return result;
    }
    size_t getLength() const { return data.size(); }
    const void *getBytesNoCopy() const { return data.data(); }
};
struct PCI {
    uint32_t words[8192]{};
    unsigned reads=0,writes=0;
    void MemoryRead32(uint8_t, uint64_t offset,uint32_t *out) {
        assert(offset < sizeof(words)); *out=words[offset/4]; ++reads;
    }
    void MemoryWrite32(uint8_t,uint64_t offset,uint32_t value) {
        assert(offset < sizeof(words)); words[offset/4]=value; ++writes;
    }
    void MemoryRead64(uint8_t,uint64_t offset,uint64_t *value) {
        assert(offset + 8 <= sizeof(words)); memcpy(value, reinterpret_cast<uint8_t *>(words)+offset,8);
    }
};
namespace amdgpu {
struct DeviceContext { PCI *pci; uint64_t bar0Size=32768; uint8_t bar0MemIndex=0; };
}
#include "amdgpu_vram_io.h"
namespace amdgpu {
enum class BringupStage { None, SDMAInit };
struct SDMA { uint32_t wptr=0; };
static unsigned copies, flushes;
static int copyStatus;
static bool publish=true;
static uint64_t copySource,copyDestination;
static int sdma_copy_linear_test(DeviceContext &,SDMA &sdma,uint64_t src,uint64_t dst,
    uint32_t bytes,uint64_t timeout) {
    assert(bytes<=kBufferCopyMaxBytes && timeout==100000);
    ++copies; copySource=src; copyDestination=dst;
    if(publish) sdma.wptr+=12;
    return copyStatus;
}
static void amdgpu_hdp_flush(DeviceContext &) { ++flushes; }
}
struct BO { bool used=true; uint32_t domain=1; uint64_t gpu_va=0,size=16384; };
static BO entries[3]{{true,1,0,16384},{true,1,16384,16384},{true,3,0x10000000,22ull<<30}};
static BO *mac_amdgpu_bo_lookup(int *,uint64_t handle) {
    return handle>=1 && handle<=3 && entries[handle-1].used ? &entries[handle-1] : nullptr;
}
struct State {
    bool shutdownBlocked=false;
    struct {
        amdgpu::DeviceContext device;
        amdgpu::BringupStage reached=amdgpu::BringupStage::SDMAInit;
        struct { uint64_t vram_start=0; } gmc;
        struct { amdgpu::SDMA instance[1]; } sdma;
    } bringup;
};
struct Driver { State *ivars; };
struct Args {
    uint64_t *scalarInput=nullptr,*scalarOutput=nullptr;
    uint32_t scalarInputCount=0,scalarOutputCount=0;
    OSData *structureInput=nullptr,*structureOutput=nullptr;
    void *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
    uint64_t structureOutputMaximumSize=0;
};
static int call(Driver *driver,PCI *pci,int *ivars,uint64_t selector,Args *arguments) {
    switch(selector) {
#include "buffer_rpc_under_test.inc"
    default:return kIOReturnBadArgument;
    }
}
int main() {
    PCI pci; State state{}; state.bringup.device.pci=&pci; Driver driver{&state}; int client=0;
    uint64_t input[5]{1,0,3,(22ull<<30)-4096,4096},output[1]{};
    Args args; args.scalarInput=input; args.scalarInputCount=5; args.scalarOutput=output; args.scalarOutputCount=1;
    assert(call(&driver,&pci,&client,48,&args)==0 && output[0]==0 && amdgpu::copies==1);
    assert(amdgpu::copySource==0 && amdgpu::copyDestination==0x10000000ull+(22ull<<30)-4096);
    input[3]=(22ull<<30)-4095;
    assert(call(&driver,&pci,&client,48,&args)==kIOReturnBadArgument && amdgpu::copies==1);
    input[3]=UINT64_MAX;
    assert(call(&driver,&pci,&client,48,&args)==kIOReturnBadArgument);
    input[2]=1; input[3]=4;
    assert(call(&driver,&pci,&client,48,&args)==kIOReturnBadArgument); // overlap
    input[2]=3; input[3]=0; input[4]=0;
    assert(call(&driver,&pci,&client,48,&args)==kIOReturnBadArgument);
    input[4]=4096; entries[2].used=false;
    assert(call(&driver,&pci,&client,48,&args)==kIOReturnBadArgument);
    entries[2].used=true; amdgpu::copyStatus=kIOReturnTimeout;
    assert(call(&driver,&pci,&client,48,&args)==0 && output[0]==kIOReturnTimeout && state.shutdownBlocked);
    state.shutdownBlocked=false; amdgpu::publish=false;
    assert(call(&driver,&pci,&client,48,&args)==0 && !state.shutdownBlocked);
    std::vector<uint32_t> words(1024);
    for(unsigned i=0;i<words.size();++i) words[i]=i^0x12345678;
    auto *upload=OSData::withBytes(words.data(),4096);
    input[0]=1;input[1]=0;input[2]=4096;
    args={};args.scalarInput=input;args.scalarInputCount=3;args.structureInput=upload;
    assert(call(&driver,&pci,&client,49,&args)==0 && pci.writes==1024 && pci.reads==1024);
    args.structureInput=nullptr;args.structureOutputMaximumSize=4096;
    assert(call(&driver,&pci,&client,50,&args)==0 && args.structureOutput);
    assert(memcmp(args.structureOutput->getBytesNoCopy(),words.data(),4096)==0);
    delete args.structureOutput; args.structureOutput=nullptr;
    const auto reads=pci.reads;
    input[0]=3;
    assert(call(&driver,&pci,&client,50,&args)==kIOReturnBadArgument && pci.reads==reads);
    input[0]=1; input[1]=2;
    assert(call(&driver,&pci,&client,50,&args)==kIOReturnBadArgument);
    input[1]=0;input[2]=4097;
    assert(call(&driver,&pci,&client,50,&args)==kIOReturnBadArgument);
    delete upload;
}
