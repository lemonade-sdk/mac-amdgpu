// Loaded at run time to reproduce vendor-library global destruction ordering.
#include <cstdio>
#include <cstdlib>
namespace {
bool live=false,stopped=false;
struct Runtime {
  Runtime() {live=true;}
  ~Runtime() {
    if(!stopped) std::abort();
    live=false;
    std::puts("runtime-destroyed");
  }
} runtime;
}
extern "C" bool fixture_alive() {return live && !stopped;}
extern "C" void fixture_shutdown() {
  if(!live || stopped) std::abort();
  stopped=true;
  std::puts("accelerator-shutdown");
}
