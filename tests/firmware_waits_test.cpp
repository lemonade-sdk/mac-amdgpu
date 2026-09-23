#include "amdgpu_ip.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <utility>
using namespace amdgpu;
using kern_return_t = int;
enum { kIOReturnSuccess, kIOReturnNotReady, kIOReturnNotAttached, kIOReturnTimeout,
       kIOReturnIOError, kIOReturnError, kIOReturnUnsupported, kIOReturnBusy, kIOReturnInternalError };
#undef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW 0
#define clock_gettime_nsec_np fake_clock
static uint64_t now, readCost;
static unsigned reads;
static std::vector<uint32_t> replies;
static std::vector<std::pair<uint32_t,uint32_t>> writes;
struct DeviceContext { IPBaseTable ip; mutable bool smuMessagePending=false; };
static uint64_t clock_gettime_nsec_np(int) { return now; }
static void IOSleep(uint64_t ms) { now+=ms*1000000; }
static uint32_t RREG32(const DeviceContext &, uint32_t) {
    now+=readCost;
    return replies[reads<replies.size()?reads++:replies.size()-1];
}
static void WREG32(const DeviceContext &, uint32_t reg, uint32_t v) { writes.emplace_back(reg,v); }
static uint32_t SOC15_REG_OFFSET_BIDX(const DeviceContext &d, IPBlock b, unsigned i, uint32_t off) { return d.ip.getBase(b,i)+off; }
#define SMU_LOG(...) do {} while(0)
#include "firmware_waits_under_test.inc"
static void reset(std::vector<uint32_t> r, uint64_t cost=0) { replies=r; now=0; reads=0; readCost=cost; writes.clear(); }
int main() {
    DeviceContext d; d.ip.setBase(IPBlock::MP1,1,0x16000);
    uint32_t out=0;
    reset({0,0x80000000}); assert(poll_reg(d,0,0x80000000,0x80000000,2000,&out));
    reset({UINT32_MAX}); assert(!poll_reg(d,0,0x80000000,0x80000000,100000,&out) && reads==1);
    assert(poll_psp_response(d,0,0x80000000,0x80000000,100000,&out)==kIOReturnNotAttached);
    reset({0x80000011}); assert(poll_psp_response(d,0,0x80000000,0x80000000,100000,&out)==kIOReturnIOError);
    reset({0x80000000}); assert(poll_psp_response(d,0,0x80000000,0x80000000,100000,&out)==0);
    reset({0,0,0},7000000); assert(!poll_reg(d,0,1,1,10000,&out)); assert(reads==2 && now==15000000); // MMIO latency counted
    reset({0}); assert(poll_psp_response(d,0,1,1,0,&out)==kIOReturnTimeout && reads==1);
    reset({0,0,0},700000000); assert(smu_send_msg_with_param(d,1,7,&out)==kIOReturnTimeout);
    assert(d.smuMessagePending && writes.size()==3 && reads==3);
    reset({0},700000000); assert(smu_send_msg_with_param(d,2,8,&out)==kIOReturnTimeout && writes.empty());
    reset({1,1,42}); assert(smu_send_msg_with_param(d,2,8,&out)==0 && out==42 && !d.smuMessagePending);
    assert(writes.size()==3);
    reset({SMUResp::UnknownCmd}); out=123; assert(smu_send_msg_with_param(d,9,0,&out)==kIOReturnUnsupported && out==123);
    reset({UINT32_MAX}); assert(smu_send_msg_with_param(d,1,0,&out)==kIOReturnNotAttached && d.smuMessagePending);
    reset({UINT32_MAX}); assert(smu_send_msg_with_param(d,1,0,&out)==kIOReturnNotAttached && writes.empty());
    puts("Firmware waits: elapsed MMIO time, removal, PSP errors, SMU timeout ownership and successful-only output pass");
}
