#include <cassert>
#include <cstdio>
#include <cstdint>
#include <map>
#include <vector>
#include "amdgpu_ip.h"
#include "amdgpu_gmc_address.h"
using namespace amdgpu;
using kern_return_t = int;
enum { kIOReturnSuccess, kIOReturnNotReady, kIOReturnIOError, kIOReturnTimeout };
struct DeviceContext { IPBaseTable ip; };
struct HubContext {
    bool inited = true;
    IPBlock ip;
    uint32_t ctx0_pt_base_lo = 0, ctx0_pt_base_hi = 1, ctx0_pt_start_lo = 2,
        ctx0_pt_start_hi = 3, ctx0_pt_end_lo = 4, ctx0_pt_end_hi = 5;
};
struct GMCContext {
    HubContext mmhub{true, IPBlock::MMHUB}, gfxhub{true, IPBlock::GC};
    uint64_t vram_start = 0x8000000000, vram_base_offset = 0, gart_pt_bus = 0x8000700000;
    uint64_t gart_start = 0, gart_end = 0;
};
static std::map<uint32_t, uint32_t> registers;
static std::vector<IPBlock> flushed;
static uint32_t failedRegister = UINT32_MAX;
static IPBlock failedHub = IPBlock::MMHUB;
static bool timeout;
static uint32_t SOC15_REG_OFFSET(const DeviceContext &d, IPBlock ip, uint32_t offset) { return d.ip.getBase(ip, 0) + offset; }
static void WREG32(const DeviceContext &, uint32_t reg, uint32_t value) { registers[reg] = value; }
static uint32_t RREG32(const DeviceContext &, uint32_t reg) { return registers.at(reg) ^ (reg == failedRegister ? 1u : 0u); }
static int gmc_flush_gpu_tlb(DeviceContext &, GMCContext &, const HubContext &hub, unsigned vmid, unsigned type) {
    assert(!vmid && !type); flushed.push_back(hub.ip);
    return timeout && failedHub == hub.ip ? kIOReturnTimeout : 0;
}
#define GMC_LOG(...) do { (void)pt_vram_rel; } while (0)
#include "gmc_window_under_test.inc"
int main() {
    DeviceContext dev; GMCContext gmc;
    dev.ip.setBase(IPBlock::MMHUB, 0, 0x1a000); dev.ip.setBase(IPBlock::GC, 0, 0x12000);
    constexpr uint64_t size = 256ull << 20, fb = 512ull << 30;
    assert(gfx12_host_window_valid(64ull << 30, size, fb, fb + (32ull << 30) - 1));
    for (auto base : {0ull, (1ull << 32) - 1, (64ull << 30) + 4096, fb, (1ull << 47)})
        assert(!gfx12_host_window_valid(base, size, fb, fb + (32ull << 30) - 1));
    for (uint64_t base : {64ull << 30, 1ull << 44, (1ull << 47) - size}) {
        gmc.gart_start = base; gmc.gart_end = base + size - 1;
        registers.clear(); flushed.clear();
        assert(gmc_program_gart_window(dev, gmc) == 0 && registers.size() == 12 && flushed.size() == 2);
        for (uint32_t hub : {0x1a000, 0x12000}) {
            assert(registers.at(hub) == 0x700001 && registers.at(hub + 1) == 0);
            assert(registers.at(hub + 2) == uint32_t(base >> 12));
            assert(registers.at(hub + 3) == uint32_t(base >> 44));
            assert(registers.at(hub + 4) == uint32_t((base + size - 1) >> 12));
            assert(registers.at(hub + 5) == uint32_t((base + size - 1) >> 44));
        }
    }
    for (uint32_t hub : {0x1a000, 0x12000}) {
        for (uint32_t index = 2; index < 6; ++index) {
            failedRegister = hub + index;
            assert(gmc_program_gart_window(dev, gmc) == kIOReturnIOError);
        }
    }
    failedRegister = UINT32_MAX; timeout = true;
    for (auto hub : {IPBlock::MMHUB, IPBlock::GC}) {
        failedHub = hub; assert(gmc_program_gart_window(dev, gmc) == kIOReturnTimeout);
    }
    puts("GART host window: range/VRAM validation, full-width page bounds, both hubs, register readback and invalidation failures pass");
}
