// Explicit GPU opt-in. Runs the pinned LSE graph -> Loom -> HRX path.
#include "lse/place/devices.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
void check(const lse::Status &s,const char *what) {
  if(!s.ok()) throw std::runtime_error(std::string(what)+": "+s.to_string());
}
void require(bool b,const char *what) {if(!b) throw std::runtime_error(what);}
uint16_t bf16Bits(float x) {return static_cast<uint16_t>(std::bit_cast<uint32_t>(x)>>16);}
struct Input {lse::graph::Array array;std::vector<std::byte> original;};
}
int main(int argc,char **argv) {
  if(argc!=2 || std::strcmp(argv[1],"--run")) {
    std::fprintf(stderr,"Usage: %s --run (explicit LSE/HRX GPU Q6 test; no model files)\n",argv[0]);return 2;
  }
  std::setvbuf(stdout,nullptr,_IONBF,0);
  try {
    using namespace lse;using namespace lse::graph;
    check(place::open_default_devices("hrx:0"),"open explicit HRX-only pool");
    auto *devices=place::default_devices();require(devices && devices->size()==1,"one HRX device required");
    auto &backend=devices->device(devices->primary());
    require(backend.name()=="hrx","refusing CPU backend fallback");
    require(backend.device_info().arch=="gfx1201","fixture requires gfx1201");
    auto *scheduler=default_scheduler();require(scheduler,"missing scheduler");
    scheduler->set_dialect(Dialect::kLoom);
    constexpr int m=3,k=128,n=17,groups=k/64,lanes=k*6/32;
    std::vector<float> x(m*k),reference(m*n,0);
    std::vector<uint32_t> packed(n*lanes,0);
    std::vector<uint16_t> scales(n*groups),biases(n*groups);
    auto code=[](int row,int column) {return static_cast<uint32_t>((row*17+column*13+7)%64);};
    auto scale=[](int row,int group) {return float((row+group)%3+1)/16;};
    auto bias=[](int row,int group) {return float((row+group)%5-2);};
    for(int i=0;i<m*k;++i) x[i]=float(i%9-4)/8;
    for(int row=0;row<n;++row) {
      for(int group=0;group<groups;++group) {scales[row*groups+group]=bf16Bits(scale(row,group));biases[row*groups+group]=bf16Bits(bias(row,group));}
      for(int j=0;j<k;++j) {
        const unsigned bit=static_cast<unsigned>(j*6),word=bit/32,shift=bit%32;
        packed[row*lanes+word]|=code(row,j)<<shift;
        if(shift>26) packed[row*lanes+word+1]|=code(row,j)>>(32-shift);
      }
    }
    for(int row=0;row<m;++row) for(int out=0;out<n;++out)
      for(int j=0;j<k;++j) reference[row*n+out]+=x[row*k+j]*(float(code(out,j))*scale(out,j/64)+bias(out,j/64));
    auto upload=[&](const void *data,size_t bytes,Shape shape,DType dtype) {
      Input input;input.original.assign(bytes+256,std::byte{0xa5});
      std::memcpy(input.original.data(),data,bytes);
      auto buffer=backend.allocate(input.original.size(),backend::MemoryClass::kDevice);
      check(buffer.status(),"allocate guarded input");
      auto owned=buffer.release();
      check(backend.copy_h2d(input.original.data(),owned,input.original.size(),0),"upload input");
      input.array=Array::from_buffer(owned,shape,dtype);return input;
    };
    auto xi=upload(x.data(),x.size()*4,Shape{m,k},DType::kF32);
    auto pi=upload(packed.data(),packed.size()*4,Shape{n,lanes},DType::kU32);
    auto si=upload(scales.data(),scales.size()*2,Shape{n,groups},DType::kBF16);
    auto bi=upload(biases.data(),biases.size()*2,Shape{n,groups},DType::kBF16);
    std::printf("LSE Q6 smoke on %s (%s): M=%d K=%d N=%d, affine6bit group64, BF16 scales/biases\n",
      std::string(backend.name()).c_str(),backend.device_info().arch.c_str(),m,k,n);
    const auto started=std::chrono::steady_clock::now();
    auto y=quant_linear(xi.array,pi.array,si.array,bi.array,6,64);
    require(y.valid() && y.shape()==Shape{m,n} && y.dtype()==DType::kF32,
      "wrong Q6 result shape or dtype");
    std::vector<float> actual(reference.size());check(y.to_host(actual.data(),actual.size()*sizeof(float)),"LSE Q6 evaluation");
    const auto trace=scheduler->last_trace();
    for(const auto &reason:trace.fallback_reasons) std::fprintf(stderr,"fallback: %s\n",reason.c_str());
    require(trace.device_groups && trace.kernels_launched && !trace.host_groups && !trace.host_fallbacks,
      "computation did not remain on the GPU");
    size_t mismatches=0;
    for(size_t i=0;i<reference.size();++i) if(actual[i]!=reference[i]) {
      if(mismatches++<8) std::fprintf(stderr,"Q6 mismatch [%zu]: expected=%g actual=%g\n",i,reference[i],actual[i]);
    }
    for(auto *input:{&xi,&pi,&si,&bi}) {
      std::vector<std::byte> readback(input->original.size());
      check(backend.copy_d2h(input->array.node()->buffer,readback.data(),readback.size(),0),"read guarded input");
      require(readback==input->original,"input data or tail guards changed");
    }
    check(backend.synchronize(),"final synchronization");
    require(!mismatches,"Q6 numerical mismatch");
    std::printf("PASS: actual LSE/Loom/HRX Q6 projection, %zu exact outputs; four input buffers/guards unchanged; device-groups=%u kernels=%u host-groups=%u fallback=%u elapsed=%.3fs\n",
      reference.size(),trace.device_groups,trace.kernels_launched,trace.host_groups,trace.host_fallbacks,
      std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count());
    return 0;
  } catch(const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
