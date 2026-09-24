#include "amdgpu_software_stats.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
using kern_return_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBadArgument=2,kIOReturnNoSpace=3,kIOReturnTimeout=4;
constexpr uint32_t kSDMACopyLinearMaxBytes=4*1024*1024;
constexpr uint32_t SDMA_OP_COPY=1,SDMA_SUBOP_COPY_LINEAR=0;
#define SDMA_PKT_HEADER_OP(x) (x)
#define SDMA_PKT_HEADER_SUB_OP(x) (x)
#define SDMA_PKT_HEADER_CPV(x) (x)
#define SDMA_LOG(...) ((void)0)
#define CLOCK_UPTIME_RAW 0
using namespace amdgpu;
static uint64_t now=100;
static uint64_t clock_gettime_nsec_np(int) {return now;}
static void IOSleep(unsigned ms) {now+=uint64_t(ms)*1000000;}
struct DeviceContext {software_stats::Counters *softwareStats;};
struct SDMAInstance {bool inited=true,enabled=true;unsigned instance=0;uint64_t wb_bus=0x8000000000;};
static bool ringOK=true,complete=true;
static int kickResult=0,clearResult=0,readResult=0;
static int sdma_clear_fence(const DeviceContext &,SDMAInstance &,unsigned) {return clearResult;}
static uint32_t sdma_fence_header() {return 0;}
static unsigned sdma_ring_write(const DeviceContext &,SDMAInstance &,uint32_t *,unsigned count) {return ringOK?count:0;}
static int sdma_kick_doorbell(const DeviceContext &,SDMAInstance &) {return kickResult;}
static int sdma_read_fence(const DeviceContext &,SDMAInstance &,unsigned,uint32_t *out) {*out=complete?0xDEC0FFEEu:0;return readResult;}
#include "software_sdma_copy.inc"
int main() {
    using namespace software_stats;
    Counters stats;stats.reset(now);stats.gartBase=0x100000000;stats.gartBytes=1ull<<30;
    stats.vramBase=0x8000000000;stats.vramBytes=32ull<<30;
    DeviceContext dev{&stats};SDMAInstance sdma;
    auto copy=[&](uint32_t bytes=4096) {return sdma_copy_linear_test(dev,sdma,stats.gartBase,stats.vramBase,bytes,1000);};
    assert(copy(0)==kIOReturnBadArgument && !stats.data.engines[SDMA0].submitted);
    ringOK=false;assert(copy()==kIOReturnNoSpace && !stats.data.engines[SDMA0].submitted);ringOK=true;
    kickResult=kIOReturnNotReady;assert(copy()==kIOReturnNotReady && !stats.data.engines[SDMA0].submitted);kickResult=0;
    assert(copy()==0 && stats.data.engines[SDMA0].completed==1 && stats.data.engines[SDMA0].bytes[HostToDevice]==4096);
    sdma.instance=1;complete=false;
    assert(copy()==kIOReturnTimeout && stats.data.engines[SDMA1].submitted==1);
    assert(stats.data.engines[SDMA1].failed==1 && stats.data.engines[SDMA1].pending==1);
    assert(stats.data.engines[SDMA1].bytes[HostToDevice]==0);
    assert(valid(stats.snapshot(now,true)));
    puts("Production SDMA copy accounting: rejected calls zero, completed payload exact, timeout retained and engines distinct pass");
}
