// Actual scheduler qualification for ordinary FP32 pointwise chain fusion.
#include "lse/place/devices.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using namespace lse;
using namespace lse::graph;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void check(const Status& status, const char* why) {
  if (!status.ok()) throw std::runtime_error(std::string(why)+": "+status.to_string());
}
enum class Chain { kSigmoidGate, kDecay, kMlpGate };
const char* name(Chain chain) {
  switch (chain) {
    case Chain::kSigmoidGate: return "sigmoid-mul";
    case Chain::kDecay: return "softplus-mul-neg-exp";
    case Chain::kMlpGate: return "silu-mul";
  }
  return "invalid";
}
bool near(float actual, float expected) {
  return std::isfinite(actual) && std::isfinite(expected) &&
         std::abs(actual-expected) <= 3e-6f+3e-5f*std::abs(expected);
}
// Independent host formulas; explicitly round each intermediate to FP32,
// matching the materialized control while allowing device transcendental error.
std::vector<std::vector<float>> reference(Chain chain,
    const std::vector<float>& x, const std::vector<float>& multiplier) {
  require(x.size()==multiplier.size(), "reference shape mismatch");
  std::vector<std::vector<float>> steps(chain==Chain::kDecay?4:2,
                                        std::vector<float>(x.size()));
  for (size_t i=0; i<x.size(); ++i) {
    const double input=x[i];
    if (chain==Chain::kDecay) {
      steps[0][i]=float(std::log1p(std::exp(input)));
      steps[1][i]=steps[0][i]*multiplier[i];
      steps[2][i]=-steps[1][i];
      steps[3][i]=float(std::exp(double(steps[2][i])));
    } else {
      const double sigmoid=1.0/(1.0+std::exp(-input));
      steps[0][i]=float(chain==Chain::kMlpGate?input*sigmoid:sigmoid);
      steps[1][i]=steps[0][i]*multiplier[i];
    }
  }
  return steps;
}
void checkReference() {
  const std::vector<float> zero{0}, two{2};
  require(reference(Chain::kSigmoidGate,zero,two).back()[0]==1,
          "sigmoid CPU oracle sanity");
  require(near(reference(Chain::kDecay,zero,two).back()[0],0.25f),
          "decay CPU oracle sanity");
  require(reference(Chain::kMlpGate,zero,two).back()[0]==0,
          "MLP CPU oracle sanity");
  require(!near(NAN,0) && !near(INFINITY,0) && !near(1,0) && near(0.25f,0.25f),
          "comparison rejection sanity");
  std::puts("PASS CPU pointwise references and finite/tolerance checks; no GPU calls");
}
constexpr size_t guard=256;
struct Guarded {
  backend::DeviceBuffer base;
  Array array;
  std::vector<std::byte> snapshot;
};
Guarded allocate(backend::IBackend& backend, Shape shape,
                 const std::vector<float>* values=nullptr) {
  Guarded g;
  const size_t bytes=shape.elem_count()*sizeof(float);
  g.snapshot.assign(bytes+2*guard,std::byte{0xa5});
  if (values) {
    require(values->size()==shape.elem_count(),"input shape mismatch");
    std::memcpy(g.snapshot.data()+guard,values->data(),bytes);
  } else {
    // Quiet NaN poison catches any output element the dispatch fails to write.
    std::fill(g.snapshot.begin()+guard,g.snapshot.end()-guard,std::byte{0xff});
  }
  auto allocation=backend.allocate(g.snapshot.size(),backend::MemoryClass::kDevice);
  check(allocation.status(),"allocate guarded pointwise buffer");
  g.base=allocation.release();
  check(backend.copy_h2d(g.snapshot.data(),g.base,g.snapshot.size(),0),"upload guarded buffer");
  auto view=g.base;view.offset+=guard;view.size_bytes=bytes;
  g.array=Array::from_buffer(view,shape,DType::kF32);
  return g;
}
std::vector<std::byte> read(backend::IBackend& backend, const Guarded& g) {
  std::vector<std::byte> bytes(g.snapshot.size());
  check(backend.copy_d2h(g.base,bytes.data(),bytes.size(),0),"read guarded allocation");
  return bytes;
}
std::vector<float> checkOutput(backend::IBackend& backend, const Guarded& g,
                              const std::vector<float>& expected) {
  const auto bytes=read(backend,g);
  for (size_t i=0; i<guard; ++i)
    require(bytes[i]==std::byte{0xa5} && bytes[bytes.size()-guard+i]==std::byte{0xa5},
            "output prefix/suffix guard changed");
  std::vector<float> actual(expected.size());
  std::memcpy(actual.data(),bytes.data()+guard,actual.size()*sizeof(float));
  for (size_t i=0; i<actual.size(); ++i) if (!near(actual[i],expected[i])) {
    std::fprintf(stderr,"pointwise mismatch [%zu]: expected %.9g actual %.9g\n",
                 i,expected[i],actual[i]);
    throw std::runtime_error("pointwise CPU reference mismatch");
  }
  return actual;
}
struct Result { std::vector<float> values; unsigned groups=0, kernels=0; };
Result run(backend::IBackend& backend, Scheduler& scheduler, Shape shape,
           Chain chain, bool fused, const std::vector<float>& x,
           const std::vector<float>& multiplier,
           const std::vector<std::vector<float>>& expected) {
  auto input=allocate(backend,shape,&x), other=allocate(backend,shape,&multiplier);
  std::vector<Array> nodes;
  if (chain==Chain::kDecay) {
    nodes.push_back(softplus(input.array));
    nodes.push_back(nodes.back()*other.array);
    nodes.push_back(neg(nodes.back()));
    nodes.push_back(exp(nodes.back()));
  } else {
    nodes.push_back(chain==Chain::kMlpGate?silu(input.array):sigmoid(input.array));
    nodes.push_back(nodes.back()*other.array);
  }
  std::vector<Guarded> outputs;
  outputs.reserve(nodes.size());
  for (auto& node:nodes) {
    require(node.valid() && node.shape()==shape && node.dtype()==DType::kF32,
            "pointwise output shape/dtype changed");
    auto output=allocate(backend,shape);
    node.node()->buffer=output.array.node()->buffer;
    output.array=node;outputs.push_back(std::move(output));
  }
  Result result;
  auto evaluate=[&](const Array& output) {
    const NodePtr roots[]={output.node()};
    check(scheduler.eval(roots,false),"evaluate actual pointwise scheduler");
    check(backend.synchronize(),"retire pointwise dispatch");
    const auto trace=scheduler.last_trace();
    for (const auto& reason:trace.fallback_reasons)
      std::fprintf(stderr,"fallback: %s\n",reason.c_str());
    require(trace.device_groups==1 && trace.kernels_launched==1 &&
            !trace.host_groups && !trace.host_fallbacks,
            "expected one actual GPU group/kernel, with no CPU fallback");
    result.groups+=trace.device_groups;result.kernels+=trace.kernels_launched;
  };
  if (fused) evaluate(nodes.back());
  else for (const auto& node:nodes) evaluate(node);
  require(read(backend,input)==input.snapshot && read(backend,other)==other.snapshot,
          "input data or input prefix/suffix guard changed");
  for (size_t i=0; i<outputs.size(); ++i) {
    if (fused && i+1<outputs.size()) {
      require(read(backend,outputs[i])==outputs[i].snapshot,
              "fused intermediate backing or guards were written");
    } else {
      auto values=checkOutput(backend,outputs[i],expected[i]);
      if (i+1==outputs.size()) result.values=std::move(values);
    }
  }
  require(result.groups==(fused?1u:unsigned(nodes.size())),"wrong group reduction");
  return result;
}
}
int main(int argc, char** argv) {
  if (argc!=2 || (std::strcmp(argv[1],"--run") && std::strcmp(argv[1],"--check-reference"))) {
    std::fprintf(stderr,"Usage: %s --check-reference | --run (actual LSE/Loom/HRX pointwise fusion)\n",argv[0]);
    return 2;
  }
  std::setvbuf(stdout,nullptr,_IONBF,0);
  try {
    checkReference();
    if (!std::strcmp(argv[1],"--check-reference")) return 0;
    setenv("LSE_REQUIRE_DEVICE_KERNELS","1",1);
    check(place::open_default_devices("hrx:0"),"open explicit HRX GPU");
    auto* devices=place::default_devices();
    require(devices && devices->size()==1,"one HRX GPU required");
    auto& backend=devices->device(devices->primary());
    require(backend.name()=="hrx" && backend.device_info().arch=="gfx1201","HRX gfx1201 required");
    auto* scheduler=default_scheduler();require(scheduler,"scheduler required");
    scheduler->set_dialect(Dialect::kLoom);
    size_t cases=0, checked=0;
    for (const auto shape:{Shape{1,1},Shape{3,17},Shape{2,129},Shape{1,17408}})
      for (const auto chain:{Chain::kSigmoidGate,Chain::kDecay,Chain::kMlpGate}) {
        std::vector<float> x(shape.elem_count()),multiplier(x.size());
        for (size_t i=0; i<x.size(); ++i) {
          x[i]=float(int((i*37+13)%161)-80)/8;
          multiplier[i]=chain==Chain::kDecay?float(1+(i*7)%12)/8:
                                                 float(int((i*11)%33)-16)/8;
        }
        const auto expected=reference(chain,x,multiplier);
        const auto fused=run(backend,*scheduler,shape,chain,true,x,multiplier,expected);
        const auto separate=run(backend,*scheduler,shape,chain,false,x,multiplier,expected);
        require(fused.groups<separate.groups,"scheduler did not reduce GPU groups");
        for (size_t i=0; i<fused.values.size(); ++i)
          require(near(fused.values[i],separate.values[i]),"fused/materialized numerical mismatch");
        std::printf("PASS %s shape=%s FP32: %zu outputs, scheduler groups %u -> %u, kernels %u -> %u; CPU reference, fused/control agreement, all buffer guards verified\n",
          name(chain),shape.to_string().c_str(),x.size(),separate.groups,fused.groups,
          separate.kernels,fused.kernels);
        checked+=x.size();++cases;
      }
    check(scheduler->drain(),"drain pointwise work");
    std::printf("PASS %zu GPU pointwise cases / %zu fused outputs; no CPU fallback\n",cases,checked);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;
  }
}
