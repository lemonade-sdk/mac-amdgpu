// Explicit GPU qualification for consecutive-axis repeat and exact storage copies.
#include "lse/place/devices.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace {
void require(bool value,const char *why) {if(!value) throw std::runtime_error(why);}
void check(const lse::Status &status,const char *why) {
  if(!status.ok()) throw std::runtime_error(std::string(why)+": "+status.to_string());
}
}
int main(int argc,char **argv) {
  if(argc!=2 || std::strcmp(argv[1],"--run")) {
    std::fprintf(stderr,"Usage: %s --run (actual LSE/Loom/HRX repeat)\n",argv[0]);return 2;
  }
  std::setvbuf(stdout,nullptr,_IONBF,0);
  setenv("LSE_REQUIRE_DEVICE_KERNELS","1",1);
  try {
    using namespace lse;using namespace lse::graph;
    check(place::open_default_devices("hrx:0"),"open HRX GPU");
    auto *devices=place::default_devices();require(devices && devices->size()==1,"one GPU required");
    auto &backend=devices->device(devices->primary());
    require(backend.name()=="hrx" && backend.device_info().arch=="gfx1201","HRX gfx1201 required");
    auto *scheduler=default_scheduler();require(scheduler,"scheduler required");
    scheduler->set_dialect(Dialect::kLoom);
    struct Case {Shape shape;int axis,count;DType dtype;};
    const Case cases[]={
      {Shape{2,3,5},0,2,DType::kF32}, {Shape{2,3,5},1,3,DType::kF32},
      {Shape{2,3,5},-1,3,DType::kF32}, {Shape{2,3,5},1,1,DType::kF32},
      {Shape{2,3,5},-1,3,DType::kU32}, {Shape{2,3,5},1,3,DType::kI32},
      {Shape{2,3,5},1,3,DType::kBF16}, {Shape{1,6,16,128},2,3,DType::kF32},
      {Shape{1,4,7,256},1,6,DType::kF32}};
    for(const auto &c:cases) {
      const size_t elementBytes=c.dtype==DType::kBF16?2:4;
      const size_t elements=c.shape.elem_count(),bytes=elements*elementBytes;
      std::vector<std::byte> original(bytes+256,std::byte{0xa5});
      for(size_t i=0;i<elements;++i) {
        // Includes integers beyond f32 precision and signed zero/NaN payloads.
        const uint32_t value=c.dtype==DType::kBF16?uint32_t(0x8000u+(i*997u)%32768u):
          i%5==0?0x80000000u:i%5==1?0x7fc00001u+uint32_t(i):0xfedcba98u+uint32_t(i*7919u);
        std::memcpy(original.data()+i*elementBytes,&value,elementBytes);
      }
      auto allocation=backend.allocate(original.size(),backend::MemoryClass::kDevice);
      check(allocation.status(),"allocate guarded input");auto buffer=allocation.release();
      check(backend.copy_h2d(original.data(),buffer,original.size(),0),"upload input");
      auto input=Array::from_buffer(buffer,c.shape,c.dtype);
      auto output=repeat(input,c.count,c.axis);
      require(output.valid() && output.dtype()==c.dtype,"output contract");
      const size_t axis=c.axis<0?c.shape.rank()+c.axis:c.axis;
      size_t inner=1;for(size_t d=axis+1;d<c.shape.rank();++d) inner*=c.shape.dim(d);
      const size_t inAxis=c.shape.dim(axis),outer=elements/(inner*inAxis);
      std::vector<std::byte> expected(elements*c.count*elementBytes);
      for(size_t o=0;o<outer;++o) for(size_t a=0;a<inAxis;++a) for(int r=0;r<c.count;++r) {
        const size_t src=(o*inAxis+a)*inner;
        const size_t dst=(o*inAxis*c.count+a*c.count+r)*inner;
        std::memcpy(expected.data()+dst*elementBytes,original.data()+src*elementBytes,inner*elementBytes);
      }
      std::vector<std::byte> actual(expected.size());
      check(output.to_host(actual.data(),actual.size()),"evaluate repeat");
      const auto trace=scheduler->last_trace();
      require(trace.device_groups && trace.kernels_launched && !trace.host_groups && !trace.host_fallbacks,"GPU-only execution required");
      if(actual!=expected) {
        for(size_t i=0;i<actual.size();++i) if(actual[i]!=expected[i]) {
          std::fprintf(stderr,"mismatch axis%d count%d dtype%d byte%zu expected%02x actual%02x\n",c.axis,c.count,int(c.dtype),i,unsigned(expected[i]),unsigned(actual[i]));break;
        }
        throw std::runtime_error("repeat exact storage mismatch");
      }
      std::vector<std::byte> unchanged(original.size());
      check(backend.copy_d2h(input.node()->buffer,unchanged.data(),unchanged.size(),0),"read guarded input");
      require(unchanged==original,"input/guard changed");
      check(backend.synchronize(),"retire repeat");
      std::printf("PASS repeat axis%d count%d dtype%d: %zu exact output bytes; input/guard unchanged; GPU groups=%u host=%u fallback=%u\n",
        c.axis,c.count,int(c.dtype),actual.size(),trace.device_groups,trace.host_groups,trace.host_fallbacks);
    }
    return 0;
  } catch(const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
