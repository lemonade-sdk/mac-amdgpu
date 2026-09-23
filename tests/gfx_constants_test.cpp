#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include "amdgpu_ip.h"
#include "amdgpu_gfx_registers.h"
using kern_return_t=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnUnsupported=2;
namespace amdgpu {struct DeviceContext {IPBaseTable ip;};}
#include "gfx_config_under_test.inc"
#define REG_SET_FIELD(value,reg,field,val) (((value)&~reg##__##field##_MASK)|((uint32_t(val)<<reg##__##field##__SHIFT)&reg##__##field##_MASK))
template<class... T> void discardLog(T...) {}
#define GFX_LOG(...) discardLog(__VA_ARGS__)
namespace amdgpu {
static std::map<uint32_t,uint32_t> registers;
static uint32_t selectedSE=0,selectedSA=0,selectedVMID=0,writeCount=0;
static uint32_t inactiveWGP[4][2]{},vmidBases[16]{};
static uint32_t SOC15_REG_OFFSET_BIDX(const DeviceContext &dev,IPBlock block,int baseIndex,uint32_t offset) {
    return dev.ip.getBase(block,baseIndex)+offset;
}
static uint32_t address(const DeviceContext &dev,GFXRegs::Register reg) {
    return SOC15_REG_OFFSET_BIDX(dev,IPBlock::GC,reg.baseIndex,reg.offset);
}
static uint32_t RREG32(const DeviceContext &dev,uint32_t reg) {
    if(reg==address(dev,GFXRegs::CC_GC_SHADER_ARRAY_CONFIG)) {
        assert(selectedSE<4 && selectedSA<2);return inactiveWGP[selectedSE][selectedSA]<<16;
    }
    return registers[reg];
}
static void WREG32(const DeviceContext &dev,uint32_t reg,uint32_t value) {
    ++writeCount;registers[reg]=value;
    if(reg==address(dev,GFXRegs::GRBM_GFX_INDEX)) {selectedSE=(value>>16)&15;selectedSA=(value>>8)&3;}
    if(reg==address(dev,GFXRegs::GRBM_GFX_CNTL)) selectedVMID=(value>>4)&15;
    if(reg==address(dev,GFXRegs::SH_MEM_BASES)) vmidBases[selectedVMID]=value;
}
}
#include "gfx_constants_under_test.inc"
using namespace amdgpu;
int main() {
    DeviceContext dev;dev.ip.setBase(IPBlock::GC,0,0x1260);dev.ip.setBase(IPBlock::GC,1,0xa000);
    auto &gc=dev.ip.gfx;
    // R9700 build188 hardware log: max_se=4 max_sh=2 max_be=4
    // max_cu_per_sh=8, active_cus=64. Exercise both arrays independently.
    gc.valid=true;gc.max_shader_engines=4;gc.max_sh_per_se=2;gc.max_cu_per_sh=8;
    gc.max_backends_per_se=4;gc.wave_front_size=32;gc.max_waves_per_simd=16;
    gc.max_scratch_slots_per_cu=32;gc.lds_size_kib=64;
    registers[address(dev,GFXRegs::GB_ADDR_CONFIG)]=1;
    GFXConfig cfg{};
    assert(gfx_constants_init(dev,cfg)==0 && cfg.inited && cfg.num_active_cus==64);
    assert(cfg.max_sh_per_se==2 && cfg.max_cu_per_sh==8 && cfg.num_rbs==16);
    assert(cfg.max_scratch_waves_per_cu==32 && cfg.lds_size_bytes==65536);
    assert(cfg.wave_front_size==32 && cfg.max_waves_per_simd==16);
    for(unsigned se=0;se<4;++se) for(unsigned sa=0;sa<2;++sa)
        assert(cfg.active_cu_bitmap[se][sa]==0xff);
    for(unsigned i=0;i<16;++i) assert(vmidBases[i]==0x10002);
    // Harvest only SA1: neither flattening to SA0 nor counting an engine as
    // one array may lose the other array's active CUs.
    inactiveWGP[0][1]=1u<<3;
    assert(gfx_constants_init(dev,cfg)==0 && cfg.num_active_cus==62);
    assert(cfg.active_cu_bitmap[0][0]==0xff && cfg.active_cu_bitmap[0][1]==0x3f);
    registers[address(dev,GFXRegs::GRBM_CC_GC_SA_UNIT_DISABLE)]=1u<<(8+7);
    assert(gfx_constants_init(dev,cfg)==0 && cfg.num_active_cus==54 && cfg.num_rbs==14);
    assert(cfg.active_cu_bitmap[3][0]==0xff && cfg.active_cu_bitmap[3][1]==0);
    // Keep wider-array synthetic coverage so future 1-SA/16-CU discovery
    // data cannot regress the upper-WGP masking logic.
    memset(inactiveWGP,0,sizeof(inactiveWGP));
    registers[address(dev,GFXRegs::GRBM_CC_GC_SA_UNIT_DISABLE)]=0;
    gc.max_sh_per_se=1;gc.max_cu_per_sh=16;
    assert(gfx_constants_init(dev,cfg)==0 && cfg.num_active_cus==64);
    // A disabled upper WGP removes both CUs; the former hardcoded 8-CU limit
    // would miss the entire upper half of this synthetic 16-CU shader array.
    inactiveWGP[0][0]=1u<<7;
    assert(gfx_constants_init(dev,cfg)==0 && cfg.num_active_cus==62);
    assert(cfg.active_cu_bitmap[0][0]==0x3fff);
    registers[address(dev,GFXRegs::GRBM_CC_GC_SA_UNIT_DISABLE)]=1u<<(8+1);
    assert(gfx_constants_init(dev,cfg)==0 && cfg.num_active_cus==46 && cfg.active_cu_bitmap[1][0]==0);
    gc.max_scratch_slots_per_cu=24;
    assert(gfx_constants_init(dev,cfg)==0 && cfg.max_scratch_waves_per_cu==24);
    const auto original=gc;
    for(unsigned scenario=0;scenario<5;++scenario) {
        gc=original;
        if(scenario==0) gc.valid=false;
        if(scenario==1) gc.max_shader_engines=5;
        if(scenario==2) gc.max_sh_per_se=3;
        if(scenario==3) gc.max_cu_per_sh=33;
        if(scenario==4) gc.max_backends_per_se=16;
        writeCount=0;cfg={};
        assert(gfx_constants_init(dev,cfg)==kIOReturnUnsupported && !writeCount && !cfg.inited);
    }
    puts("GFX constants: R9700 4x2x8 CUs, per-array/upper-WGP/SA harvest, scratch/LDS limits and all VMID apertures pass");
}
