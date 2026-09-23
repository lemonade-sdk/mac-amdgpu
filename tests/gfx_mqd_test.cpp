#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "amdgpu_gfx_mqd.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/v12_structs.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h"
using namespace amdgpu;
#include "gfx_mqd_offsets.inc"
static_assert(sizeof(GFXQueueDescriptor)==sizeof(v12_gfx_mqd));
#define REG_SET_FIELD(value,reg,field,val) (((value)&~reg##__##field##_MASK)|((uint32_t(val)<<reg##__##field##__SHIFT)&reg##__##field##_MASK))
static uint32_t lower_32_bits(uint64_t v) { return uint32_t(v); }
static uint32_t upper_32_bits(uint64_t v) { return uint32_t(v>>32); }
static uint32_t order_base_2(uint32_t n) { unsigned r=0; while ((1u<<r)<n) ++r; return r; }
struct amdgpu_mqd_prop {
    uint64_t mqd_gpu_addr,hqd_base_gpu_addr,rptr_gpu_addr,wptr_gpu_addr;
    uint32_t queue_size,doorbell_index;
    bool use_doorbell=true,tmz_queue=false,kernel_queue=true;
    uint64_t shadow_addr=0,csa_addr=0,fence_address=0;
};
#include "linux_gfx_mqd_reference.inc"
int main() {
    for (uint64_t base: {0x8000000000ull,0xffff00000000ull})
    for (uint32_t bytes: {1024u,16384u,65536u})
    for (uint32_t doorbell: {0u,0x40u,0x3fffffeu}) {
        amdgpu_mqd_prop prop{base+0x10000,base+0x20000,base+0x30000,base+0x30040,bytes,doorbell};
        v12_gfx_mqd linux{}; GFXQueueDescriptor ours{};
        assert(linux_gfx_mqd_init(&linux,&prop)==0);
        assert(gfx_build_kernel_mqd(ours,prop.mqd_gpu_addr,prop.hqd_base_gpu_addr,
            prop.rptr_gpu_addr,prop.wptr_gpu_addr,bytes,doorbell));
        assert(memcmp(&ours,&linux,sizeof(ours))==0);
    }
    GFXQueueDescriptor mqd{};
    assert(!gfx_build_kernel_mqd(mqd,0,0x2000,0x3000,0x3040,16384,0));
    assert(!gfx_build_kernel_mqd(mqd,0x1001,0x2000,0x3000,0x3040,16384,0));
    assert(!gfx_build_kernel_mqd(mqd,0x1000,0x2001,0x3000,0x3040,16384,0));
    assert(!gfx_build_kernel_mqd(mqd,0x1000,0x2000,0x3001,0x3040,16384,0));
    assert(!gfx_build_kernel_mqd(mqd,0x1000,0x2000,0x3000,0x3044,16384,0));
    assert(!gfx_build_kernel_mqd(mqd,0x1000,0x2000,0x3000,0x3040,16380,0));
    assert(!gfx_build_kernel_mqd(mqd,0x1000,0x2000,0x3000,0x3040,16384,1));
    assert(!gfx_build_kernel_mqd(mqd,0x1000,0x2000,0x3000,0x3040,16384,0x4000000));
    puts("GFX MQD: byte-for-byte Linux kernel descriptor across sizes/addresses/doorbells; malformed input rejection pass");
}
