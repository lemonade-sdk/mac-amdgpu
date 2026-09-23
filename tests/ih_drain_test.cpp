#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "amdgpu_ip.h"
using namespace amdgpu;
struct IOBufferMemoryDescriptor;
struct IODMACommand;
#include "ih_types_under_test.inc"
using IHDispatchFn=void (*)(const IHEntry &,void *);
struct DeviceContext { IPBaseTable ip; };
static uint32_t readValue=0, writeCount=0;
static uint32_t RREG32(const DeviceContext &,uint32_t) { return readValue; }
static void WREG32(const DeviceContext &,uint32_t,uint32_t) { ++writeCount; }
static uint32_t SOC15_REG_OFFSET(const DeviceContext &d,IPBlock b,uint32_t r) { return d.ip.getBase(b,0)+r; }
#define IH_LOG(...) do {} while(0)
#include "ih_drain_under_test.inc"
static void collect(const IHEntry &e,void *p) { static_cast<std::vector<uint32_t> *>(p)->push_back(e.src_data[0]); }
int main() {
    DeviceContext dev; dev.ip.setBase(IPBlock::OSSSYS,0,0x10a0);
    uint32_t ring[32]={}; for(unsigned i=0;i<4;++i) ring[i*8+4]=i;
    uint32_t shadow=64;
    IHContext ih{}; ih.inited=ih.enabled=true; ih.ring_cpu=ring; ih.ring_size_bytes=sizeof(ring); ih.ptr_mask=sizeof(ring)-1; ih.wptr_shadow_cpu=&shadow;
    std::vector<uint32_t> entries;
    assert(ih_drain(dev,ih,collect,&entries)==2 && entries==std::vector<uint32_t>({0,1}));
    shadow=32; entries.clear(); assert(ih_drain(dev,ih,collect,&entries)==3 && entries==std::vector<uint32_t>({2,3,0}));
    for(uint32_t bad:{2u,4u,31u,UINT32_MAX}) { shadow=bad; auto old=ih.rptr, writes=writeCount; assert(ih_drain(dev,ih,collect,&entries)==0 && ih.rptr==old && writeCount==writes); }
    shadow=1; readValue=UINT32_MAX; auto writes=writeCount; assert(ih_drain(dev,ih,collect,&entries)==0 && writeCount==writes);
    shadow=1; readValue=65; entries.clear(); assert(ih_drain(dev,ih,collect,&entries)==3 && entries==std::vector<uint32_t>({3,0,1}));
    ih.rptr=2; assert(ih_drain(dev,ih,collect,&entries)==0);
    puts("IH: entry decode, wrap, overflow and malformed/disconnected pointers pass");
}
