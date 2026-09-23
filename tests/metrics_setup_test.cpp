#include "amdgpu_ip.h"
#include "amdgpu_metrics_state.h"
#include <cassert>
#include <cstdint>
#include <vector>

using kern_return_t = int32_t;
enum { kIOReturnSuccess, kIOReturnBusy, kIOReturnNoSpace, kIOReturnNotReady, kIOReturnTimeout };
namespace amdgpu {
struct DeviceContext { bool smuMessagePending = false; };
struct PSPContext {
    uint64_t fwBufSize = 2 * 65536, fwBufBumpOffset = 4096;
    uint64_t fwBufBaseMC = 0x8000000000ull, fwBufVRAMOffset = 0x200000;
    void *fwBufSysmemBuffer = nullptr;
};
static std::vector<uint32_t> messages;
static uint32_t failMessage = UINT32_MAX, interfaceVersion = 0x2e;
static SMUMetricsContext *active;
#undef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW 0
#define clock_gettime_nsec_np fake_clock
static uint64_t fake_clock(int) { return 99; }
#define SMU_LOG(...) do {} while (0)
static kern_return_t smu_send_msg_with_param(DeviceContext &dev, uint32_t message,
    uint32_t, uint32_t *out) {
    messages.push_back(message);
    if (message == PPSMC::SetDriverDramAddrHigh || message == PPSMC::SetDriverDramAddrLow) {
        // Reservation must already be recorded before firmware can DMA there.
        if (active) assert(active->reserved && active->tableBytes == 65536);
    }
    if (message == failMessage) { dev.smuMessagePending = true; return kIOReturnTimeout; }
    if (out) *out = message == PPSMC::GetDriverIfVersion ? interfaceVersion : 0;
    dev.smuMessagePending = false;
    return kIOReturnSuccess;
}
static kern_return_t smu_send_msg(DeviceContext &dev, uint32_t message) {
    return smu_send_msg_with_param(dev, message, 0, nullptr);
}
#include "metrics_setup_under_test.inc"
} // namespace amdgpu
using namespace amdgpu;
int main() {
    DeviceContext dev;
    PSPContext psp;
    SMUMetricsContext ctx{};
    active = &ctx;
    assert(smu_smc_hw_setup(dev, psp, &ctx) == kIOReturnSuccess);
    assert(ctx.reserved && ctx.vramBacked && ctx.addressProgrammed && ctx.setupComplete);
    assert(ctx.driverInterface == 0x2e && ctx.snapshot.generation == 99);
    assert(ctx.tableMC == 0x8000001000ull && ctx.tableVRAMOffset == 0x201000);
    assert(psp.fwBufBumpOffset == 4096 + 65536);
    psp.fwBufBumpOffset += 4096; // Later firmware allocations cannot move the retained table.
    const auto count = messages.size();
    assert(smu_smc_hw_setup(dev, psp, &ctx) == kIOReturnBusy && messages.size() == count);
    assert(ctx.tableVRAMOffset == 0x201000);

    for (uint32_t failure : {PPSMC::SetDriverDramAddrHigh, PPSMC::SetDriverDramAddrLow}) {
        ctx = {}; psp = {}; dev = {}; messages.clear(); failMessage = failure;
        assert(smu_smc_hw_setup(dev, psp, &ctx) == kIOReturnTimeout);
        assert(ctx.reserved && !ctx.addressProgrammed && !ctx.setupComplete);
        assert(psp.fwBufBumpOffset == 4096 + 65536); // Never recycle a partially programmed destination.
    }
    ctx = {}; psp = {}; dev = {}; messages.clear(); failMessage = UINT32_MAX;
    psp.fwBufBaseMC = UINT64_MAX - 4096;
    assert(smu_smc_hw_setup(dev, psp, &ctx) == kIOReturnNoSpace);
    assert(!ctx.reserved && psp.fwBufBumpOffset == 4096 && messages.size() == 1);
    ctx = {}; psp = {}; dev = {}; interfaceVersion = 0x2f;
    psp.fwBufSysmemBuffer = reinterpret_cast<void *>(1);
    assert(smu_smc_hw_setup(dev, psp, &ctx) == kIOReturnSuccess);
    assert(ctx.driverInterface == 0x2f && !ctx.vramBacked && ctx.setupComplete);
    // Unknown IF may still bring up; collection separately refuses to decode.
    ctx = {}; psp = {}; dev = {}; active = nullptr;
    assert(smu_smc_hw_setup(dev, psp, nullptr) == kIOReturnSuccess);
}
