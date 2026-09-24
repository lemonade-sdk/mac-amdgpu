// Actual LSE owner/registry paths, with a dlopened mock runtime and CPU backend.
#include "lse/backend/backend.hpp"
#include "lse/backends/cpu/cpu_backend.hpp"
#include "lse/graph/graph.hpp"
#ifndef LSE_LIFETIME_STANDALONE
#include "lse/place/devices.hpp"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
namespace {
const char *runtimePath=nullptr;
bool (*alive)()=nullptr;
void (*stop)()=nullptr;
bool ownerDestroyed=false;
struct ProbeBackend : lse::backend::CpuBackend {
  static constexpr std::string_view kName="lifecycle";
  static void prepare_runtime() {
    static const bool prepared=[] {
      void *handle=dlopen(runtimePath,RTLD_NOW|RTLD_LOCAL);
      if(!handle) std::abort();
      alive=reinterpret_cast<bool(*)()>(dlsym(handle,"fixture_alive"));
      stop=reinterpret_cast<void(*)()>(dlsym(handle,"fixture_shutdown"));
      if(!alive || !stop || !alive()) std::abort();
      std::atexit([] {
        if(!ownerDestroyed || !alive()) std::abort();
        stop();
      });
      return true;
    }();
    (void)prepared;
  }
  ~ProbeBackend() {
    if(!alive || !alive() || ownerDestroyed) std::abort();
    ownerDestroyed=true;
    std::puts("backend-destroyed");
  }
};
}
LSE_REGISTER_BACKEND("lifecycle", ProbeBackend)
int main(int argc,char **argv) {
  if(argc!=3) return 2;
  runtimePath=argv[1];
  setenv("LSE_POOL","lifecycle:0",1);
  setenv("LSE_BACKEND","lifecycle",1);
#ifndef LSE_LIFETIME_STANDALONE
  if(!std::strcmp(argv[2],"explicit") &&
     !lse::place::open_default_devices("lifecycle:0").ok()) return 3;
#endif
  if(!lse::graph::default_scheduler()) return 4;
  if(!alive || !alive()) return 5;
  if(!std::strcmp(argv[2],"strict")) {
    setenv("LSE_REQUIRE_DEVICE_KERNELS","1",1);
    auto value=lse::graph::Array::full(lse::Shape{4},lse::DType::kF32,2);
    auto status=value.eval();
    if(status.ok() || status.message().find("GPU-only execution required") == std::string::npos ||
       value.node()->materialized) return 6;
    unsetenv("LSE_REQUIRE_DEVICE_KERNELS");
    if(!value.eval().ok() || !value.node()->materialized) return 7;
  }
  std::puts("main-completed");
}
