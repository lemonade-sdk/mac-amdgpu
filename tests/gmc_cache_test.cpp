#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include "amdgpu_ip.h"
#include "amdgpu_field_defs.h"
using namespace amdgpu;
#include "gmc_field_checks.inc"
struct DeviceContext {};
struct HubContext {
    IPBlock ip;
    uint32_t vm_l2_cntl=1, vm_l2_cntl2=2, vm_l2_cntl3=3, vm_l2_cntl4=4, vm_l2_cntl5=5;
};
static std::map<uint32_t,uint32_t> registers;
static uint32_t SOC15_REG_OFFSET(const DeviceContext &, IPBlock, uint32_t reg) { return reg; }
static uint32_t RREG32(const DeviceContext &, uint32_t reg) { return registers[reg]; }
static void WREG32(const DeviceContext &, uint32_t reg, uint32_t value) { registers[reg]=value; }
#define REG_SET_FIELD(v,r,f,x) (((v)&~r##__##f##_MASK)|(((x)<<r##__##f##__SHIFT)&r##__##f##_MASK))
#include "gmc_cache_under_test.inc"
int main() {
    DeviceContext dev;
    for (auto ip : {IPBlock::MMHUB, IPBlock::GC}) {
        registers.clear();
        HubContext hub{ip};
        hub_init_cache_regs(dev,hub);
        // Linux seeds CNTL5 with 0x3fe0 and clears only bits 0..4.
        // Bit 5 belongs to WALKER_PRIORITY_CLIENT_ID, not fragment size.
        assert(registers[hub.vm_l2_cntl5]==0x3fe0);
        assert(((registers[hub.vm_l2_cntl5]>>5)&0x1ff)==0x1ff);
        assert(registers[hub.vm_l2_cntl4]==1);
        assert(registers.size()==5);
    }
    puts("GMC cache: Linux field definitions and both hubs preserve page-walker priority");
}
