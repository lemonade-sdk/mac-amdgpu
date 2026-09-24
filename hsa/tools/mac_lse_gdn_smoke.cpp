// Explicit GPU qualification of GDN recurrence and persistent state through LSE.
#include "lse/place/devices.hpp"
#include "lse/backends/hrx/loomc/loomc_compiler.hpp"
#include "lse/backends/hrx/loomc/loom_emitter.hpp"
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
void check(const Status& s, const char* why) {
  if (!s.ok()) throw std::runtime_error(std::string(why) + ": " + s.to_string());
}
struct Values {
  int batch, seq, heads, dim;
  std::vector<float> q, k, v, alpha, beta;
};
struct ResultValues { std::vector<float> out, state; };
ResultValues reference(const Values& x, std::vector<float> state) {
  ResultValues result{std::vector<float>(x.q.size()), std::move(state)};
  for (int b = 0; b < x.batch; ++b) for (int t = 0; t < x.seq; ++t)
    for (int h = 0; h < x.heads; ++h) for (int row = 0; row < x.dim; ++row) {
      const size_t scalar = (b * x.seq + t) * x.heads + h;
      const size_t vector = scalar * x.dim;
      const size_t base = ((b * x.heads + h) * x.dim + row) * x.dim;
      double sk = 0;
      for (int j = 0; j < x.dim; ++j) {
        result.state[base+j] *= x.alpha[scalar];
        sk += double(result.state[base+j]) * x.k[vector+j];
      }
      const float delta = float((double(x.v[vector+row]) - sk) * x.beta[scalar]);
      double output = 0;
      for (int j = 0; j < x.dim; ++j) {
        result.state[base+j] = std::fma(delta, x.k[vector+j], result.state[base+j]);
        output += double(result.state[base+j]) * x.q[vector+j];
      }
      result.out[vector+row] = float(output);
    }
  return result;
}
Values values(int dim, int seq, int heads, int seed, int batch = 1) {
  Values x{batch,seq,heads,dim,{},{},{},{},{}};
  x.q.resize(batch*seq*heads*dim); x.k.resize(x.q.size()); x.v.resize(x.q.size());
  x.alpha.resize(batch*seq*heads); x.beta.resize(x.alpha.size());
  for (size_t i = 0; i < x.q.size(); ++i) {
    x.q[i] = float(int((i*3+seed)%17)-8)/32;
    x.k[i] = float(int((i*5+seed)%13)-6)/64;
    x.v[i] = float(int((i*7+seed)%19)-9)/16;
  }
  for (size_t i = 0; i < x.alpha.size(); ++i) {
    x.alpha[i] = float(12+(i+seed)%4)/16;
    x.beta[i] = float(1+(i+seed)%5)/16;
  }
  return x;
}
bool near(float actual, float expected) {
  return std::isfinite(actual) && std::isfinite(expected) &&
         std::abs(actual-expected) <= 1e-5f + 1e-4f*std::abs(expected);
}
void checkReference() {
  Values x{1,1,1,16,std::vector<float>(16,1),std::vector<float>(16,1),
           std::vector<float>(16,1),{1},{0.5f}};
  auto r = reference(x, std::vector<float>(256));
  require(std::all_of(r.out.begin(),r.out.end(),[](float v){return v==8;}),"reference output sanity");
  require(std::all_of(r.state.begin(),r.state.end(),[](float v){return v==0.5f;}),"reference state sanity");
  x.beta[0]=0; x.alpha[0]=0.5f;
  auto carried = reference(x,r.state);
  require(carried.out[0]==4 && carried.state[0]==0.25f,"reference state carry sanity");
  require(!near(NAN,0) && !near(1,0) && near(0.5f,0.5f),"comparison sanity");
  std::puts("PASS CPU GDN oracle sanity, state carry and comparison checks; no GPU calls");
}
void compileOnly() {
  backend::LoomcCompiler compiler;
  require(compiler.available(), "Loom compiler unavailable");
  backend::DeviceInfo device;
  device.arch = "gfx1201"; device.wavefront_size = 32;
  device.max_threads_per_workgroup = 1024;
  auto input = [](Shape shape) {
    auto n = std::make_shared<Node>();
    n->shape = shape; n->dtype = DType::kF32; n->materialized = true;
    return Array(n);
  };
  size_t count = 0;
  for (int dim : {16, 32, 128}) for (int seq : {1, 6}) for (int mode : {0, 1, 2}) {
    const int heads = dim == 128 ? 48 : 2, batch = dim == 128 ? 1 : 2;
    auto q = input(Shape{batch, seq, heads, dim}), k = input(q.shape()), v = input(q.shape());
    auto alpha = input(Shape{batch, seq, heads}), beta = input(alpha.shape());
    auto state = input(Shape{batch, heads, dim, dim});
    Array next;
    auto out = gated_delta_step(q, k, v, alpha, beta, state, &next);
    std::vector<NodePtr> roots;
    if (mode != 1) roots.push_back(out.node());
    if (mode != 0) roots.push_back(next.node());
    const auto groups = Partitioner::partition(roots);
    require(!groups.empty(), "GDN compilation groups required");
    for (const auto& group : groups) {
      auto emitted = backend::LoomEmitter{}.emit(group, device);
      check(emitted.status(), "emit GDN fixture");
      auto object = compiler.compile(emitted->source, "gfx1201");
      check(object.status(), "compile GDN fixture");
      require(object->code.size() > 64, "missing GDN code");
      const auto* bytes = reinterpret_cast<const unsigned char*>(object->code.data());
      require(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F' &&
              (bytes[18] | (bytes[19] << 8)) == 224 && bytes[48] == 0x4e,
              "GDN fixture is not gfx1201 ELF");
    }
    std::printf("PASS compile GDN B%d T%d H%d D%d mode%d: %zu groups\n", batch, seq, heads, dim, mode, groups.size());
    ++count;
  }
  std::printf("PASS %zu GDN gfx1201 fixtures compiled; no GPU calls\n", count);
}
constexpr size_t guard = 256;
struct Guarded {
  backend::DeviceBuffer base;
  Array array;
  std::vector<std::byte> snapshot;
};
Guarded allocate(backend::IBackend& backend, Shape shape, const std::vector<float>* data) {
  Guarded g;
  const size_t bytes = shape.elem_count()*sizeof(float);
  g.snapshot.assign(bytes+2*guard,std::byte{0xa5});
  if (data) std::memcpy(g.snapshot.data()+guard,data->data(),bytes);
  else std::fill(g.snapshot.begin()+guard,g.snapshot.end()-guard,std::byte{0xff});
  auto allocation=backend.allocate(g.snapshot.size(),backend::MemoryClass::kDevice);
  check(allocation.status(),"allocate guarded buffer");g.base=allocation.release();
  check(backend.copy_h2d(g.snapshot.data(),g.base,g.snapshot.size(),0),"upload guards and data");
  auto view=g.base;view.offset+=guard;view.size_bytes=bytes;
  g.array=Array::from_buffer(view,shape,DType::kF32);
  return g;
}
std::vector<std::byte> read(backend::IBackend& backend,const Guarded& g) {
  std::vector<std::byte> bytes(g.snapshot.size());
  check(backend.copy_d2h(g.base,bytes.data(),bytes.size(),0),"read complete guarded allocation");
  return bytes;
}
void checkOutput(backend::IBackend& backend, Guarded& g, const std::vector<float>& expected, const char* label) {
  auto bytes=read(backend,g);
  for(size_t i=0;i<guard;++i) {
    require(bytes[i]==std::byte{0xa5} && bytes[bytes.size()-guard+i]==std::byte{0xa5},"output prefix/suffix guard changed");
  }
  double largest=0;
  for(size_t i=0;i<expected.size();++i) {
    float actual;std::memcpy(&actual,bytes.data()+guard+i*4,4);
    if(!near(actual,expected[i])) {
      std::fprintf(stderr,"%s mismatch at %zu: actual %.9g expected %.9g\n",label,i,actual,expected[i]);
      throw std::runtime_error("GDN numerical mismatch");
    }
    largest=std::max(largest,double(std::abs(actual-expected[i])));
  }
  g.snapshot=std::move(bytes);
  std::printf("  %s: %zu checked floats max_abs_error=%.9g, prefix/suffix guards intact\n",label,expected.size(),largest);
}
Guarded run(backend::IBackend& backend, Scheduler& scheduler, const Values& x,
            const Guarded& state, const ResultValues& expected, int mode) {
  const Shape vector{x.batch,x.seq,x.heads,x.dim},scalar{x.batch,x.seq,x.heads};
  auto q=allocate(backend,vector,&x.q),k=allocate(backend,vector,&x.k),v=allocate(backend,vector,&x.v);
  auto alpha=allocate(backend,scalar,&x.alpha),beta=allocate(backend,scalar,&x.beta);
  Array next;
  auto out=gated_delta_step(q.array,k.array,v.array,alpha.array,beta.array,state.array,&next);
  auto output=allocate(backend,out.shape(),nullptr),nextState=allocate(backend,next.shape(),nullptr);
  out.node()->buffer=output.array.node()->buffer; output.array=out;
  next.node()->buffer=nextState.array.node()->buffer; nextState.array=next;
  std::vector<NodePtr> roots;
  if(mode!=1) roots.push_back(out.node());
  if(mode!=0) roots.push_back(next.node());
  check(scheduler.eval(roots,false),"evaluate GPU GDN");
  check(backend.synchronize(),"retire GDN dispatch");
  const auto trace=scheduler.last_trace();
  require(trace.device_groups && trace.kernels_launched && !trace.host_groups && !trace.host_fallbacks,"GPU-only GDN required");
  // The current Loom partitioner evaluates both roots as separate kernels.
  // Fusion is a performance choice; both results and their guards are checked.
  std::printf("GDN dispatch trace: kernels=%u device=%u host=%u fallback=%u\n",
    trace.kernels_launched,trace.device_groups,trace.host_groups,trace.host_fallbacks);
  const Guarded* inputs[]={&q,&k,&v,&alpha,&beta,&state};
  for(const auto* input:inputs) require(read(backend,*input)==input->snapshot,"input or input guards changed");
  if(mode!=1) checkOutput(backend,output,expected.out,"output");
  else require(read(backend,output)==output.snapshot,"unused output changed");
  if(mode!=0) checkOutput(backend,nextState,expected.state,"state");
  else require(read(backend,nextState)==nextState.snapshot,"unused state changed");
  std::printf("PASS GDN B%d T%d H%d D%d mode=%s kernels=%u device=%u host=%u fallback=%u\n",
    x.batch,x.seq,x.heads,x.dim,mode==0?"output":mode==1?"state":"pair",trace.kernels_launched,trace.device_groups,trace.host_groups,trace.host_fallbacks);
  return nextState;
}
}
int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  if(argc!=2 || (std::strcmp(argv[1],"--run") && std::strcmp(argv[1],"--check-reference") && std::strcmp(argv[1],"--compile-only"))) {
    std::fprintf(stderr,"Usage: %s --check-reference | --compile-only | --run (real GPU GDN)\n",argv[0]);return 2;
  }
  try {
    checkReference();
    if(!std::strcmp(argv[1],"--check-reference")) return 0;
    if(!std::strcmp(argv[1],"--compile-only")) { compileOnly(); return 0; }
    setenv("LSE_REQUIRE_DEVICE_KERNELS","1",1);
    check(place::open_default_devices("hrx:0"),"open HRX GPU");
    auto* devices=place::default_devices();require(devices && devices->size()==1,"one GPU required");
    auto& backend=devices->device(devices->primary());
    require(backend.name()=="hrx" && backend.device_info().arch=="gfx1201","HRX gfx1201 required");
    auto* scheduler=default_scheduler();require(scheduler,"scheduler required");scheduler->set_dialect(Dialect::kLoom);
    for(int dim:{16,32,128}) {
      const int heads=dim==128?48:2;
      const int batch=dim==128?1:2;
      std::vector<float> initial(batch*heads*dim*dim);
      for(size_t i=0;i<initial.size();++i) initial[i]=float(int((i*11)%23)-11)/256;
      auto state=allocate(backend,Shape{batch,heads,dim,dim},&initial);
      auto prefill=values(dim,6,heads,3,batch);
      auto prefRef=reference(prefill,initial);
      for(int mode:{0,1}) (void)run(backend,*scheduler,prefill,state,prefRef,mode);
      auto carried=run(backend,*scheduler,prefill,state,prefRef,2);
      auto decode=values(dim,1,heads,13,batch);
      auto decRef=reference(decode,prefRef.state);
      for(int mode:{0,1,2}) (void)run(backend,*scheduler,decode,carried,decRef,mode);
    }
    check(scheduler->drain(),"drain GDN work");
    std::puts("PASS all 18 GPU GDN cases; real prefill state carried into decode; CPU reference, guards and GPU-only execution verified");
    return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
