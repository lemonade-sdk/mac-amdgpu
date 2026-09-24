// Explicit GPU qualification for causal convolution padding and retained history.
#include "lse/place/devices.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include <bit>
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
struct Input {lse::graph::Array array;std::vector<std::byte> original;};
}
int main(int argc,char **argv) {
  if(argc!=2 || std::strcmp(argv[1],"--run")) {
    std::fprintf(stderr,"Usage: %s --run (actual LSE/Loom/HRX causal convolution)\n",argv[0]);return 2;
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
    auto upload=[&](const void *data,size_t bytes,Shape shape,DType dtype) {
      Input input;input.original.assign(bytes+256,std::byte{0xa5});
      std::memcpy(input.original.data(),data,bytes);
      auto allocation=backend.allocate(input.original.size(),backend::MemoryClass::kDevice);
      check(allocation.status(),"allocate guarded input");
      auto buffer=allocation.release();
      check(backend.copy_h2d(input.original.data(),buffer,input.original.size(),0),"upload guarded input");
      input.array=Array::from_buffer(buffer,shape,dtype);return input;
    };
    constexpr int batch=2,channels=17,kernel=4;
    for(const int seq:{1,2,7}) for(const bool history:{false,true}) {
      std::vector<float> x(batch*seq*channels),tail(batch*(kernel-1)*channels),
                         weights(channels*kernel),bias(channels),reference(x.size(),0);
      std::vector<uint16_t> packedWeights(weights.size()),packedBias(bias.size());
      for(size_t i=0;i<x.size();++i) x[i]=float(int(i%19)-9)/8;
      for(size_t i=0;i<tail.size();++i) tail[i]=float(int((i*7)%23)-11)/4;
      for(int c=0;c<channels;++c) {
        bias[c]=float(c%5-2)/4;
        packedBias[c]=uint16_t(std::bit_cast<uint32_t>(bias[c])>>16);
        for(int j=0;j<kernel;++j) {
          weights[c*kernel+j]=float((c*7+j*3)%11-5)/16;
          packedWeights[c*kernel+j]=uint16_t(std::bit_cast<uint32_t>(weights[c*kernel+j])>>16);
        }
      }
      for(int b=0;b<batch;++b) for(int t=0;t<seq;++t) for(int c=0;c<channels;++c) {
        float sum=bias[c];
        for(int j=0;j<kernel;++j) {
          const int source=t-(kernel-1-j);
          const float value=source>=0?x[(b*seq+source)*channels+c]:
            history?tail[(b*(kernel-1)+source+kernel-1)*channels+c]:0;
          sum+=value*weights[c*kernel+j];
        }
        reference[(b*seq+t)*channels+c]=sum;
      }
      auto xi=upload(x.data(),x.size()*4,Shape{batch,seq,channels},DType::kF32);
      auto wi=upload(packedWeights.data(),packedWeights.size()*2,Shape{channels,kernel},DType::kBF16);
      auto bi=upload(packedBias.data(),packedBias.size()*2,Shape{channels},DType::kBF16);
      auto ti=upload(tail.data(),tail.size()*4,Shape{batch,kernel-1,channels},DType::kF32);
      auto output=history?causal_conv1d(xi.array,wi.array,bi.array,ti.array):causal_conv1d(xi.array,wi.array,bi.array);
      require(output.valid() && output.shape()==Shape{batch,seq,channels} && output.dtype()==DType::kF32,"output contract");
      std::vector<float> actual(reference.size());
      check(output.to_host(actual.data(),actual.size()*4),"evaluate causal convolution");
      auto trace=scheduler->last_trace();
      require(trace.device_groups && trace.kernels_launched && !trace.host_groups && !trace.host_fallbacks,"GPU-only execution required");
      for(size_t i=0;i<actual.size();++i) if(actual[i]!=reference[i]) {
        std::fprintf(stderr,"mismatch T%d history%d index%zu expected=%g actual=%g\n",seq,history,i,reference[i],actual[i]);
        throw std::runtime_error("convolution numerical mismatch");
      }
      for(auto *input:{&xi,&wi,&bi,&ti}) {
        std::vector<std::byte> actualBytes(input->original.size());
        check(backend.copy_d2h(input->array.node()->buffer,actualBytes.data(),actualBytes.size(),0),"read guarded input");
        require(actualBytes==input->original,"input/guard changed");
      }
      check(backend.synchronize(),"retire convolution");
      std::printf("PASS: B2 T%d C17 K4 history=%d, %zu exact outputs; four inputs/guards unchanged; GPU groups=%u host=%u fallback=%u\n",
        seq,history,actual.size(),trace.device_groups,trace.host_groups,trace.host_fallbacks);
    }
    for(const int seq:{1,2,3,7}) {
      std::vector<float> tail(batch*3*channels),x(batch*seq*channels),expected(tail.size());
      for(size_t i=0;i<tail.size();++i) tail[i]=float(i+10000);
      for(size_t i=0;i<x.size();++i) x[i]=float(i+20000);
      for(int b=0;b<batch;++b) for(int t=0;t<3;++t) for(int c=0;c<channels;++c) {
        const int source=seq+t;
        expected[(b*3+t)*channels+c]=source<3?tail[(b*3+source)*channels+c]:x[(b*seq+source-3)*channels+c];
      }
      auto ti=upload(tail.data(),tail.size()*4,Shape{batch,3,channels},DType::kF32);
      auto xi=upload(x.data(),x.size()*4,Shape{batch,seq,channels},DType::kF32);
      auto output=conv_tail(ti.array,xi.array);std::vector<float> actual(expected.size());
      check(output.to_host(actual.data(),actual.size()*4),"evaluate conv tail");
      const auto trace=scheduler->last_trace();
      require(trace.device_groups && trace.kernels_launched && !trace.host_groups && !trace.host_fallbacks,"GPU-only conv tail required");
      require(actual==expected,"convolution tail mismatch");
      for(auto *input:{&ti,&xi}) {
        std::vector<std::byte> unchanged(input->original.size());
        check(backend.copy_d2h(input->array.node()->buffer,unchanged.data(),unchanged.size(),0),"read conv-tail guards");
        require(unchanged==input->original,"conv-tail input/guard changed");
      }
      check(backend.synchronize(),"retire conv tail");
      std::printf("PASS conv_tail B2 T%d C17 L3: %zu exact outputs; two inputs/guards unchanged; GPU groups=%u host=%u fallback=%u\n",seq,actual.size(),trace.device_groups,trace.host_groups,trace.host_fallbacks);
    }
    return 0;
  } catch(const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
