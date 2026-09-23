#pragma once
#include "amdgpu_aql_abi.h"
#include "../../hsa/third_party/hsa/include/hsa/amd_hsa_signal.h"
#include <string.h>

namespace amdgpu {
// gfx1201 KFD compute MQD; offsets checked against Linux v12_compute_mqd.
struct AQLComputeMQD { uint32_t words[512]; };
namespace AQLMQDOff {
constexpr unsigned header=0, compute_pipelinestat_enable=11;
constexpr unsigned compute_static_thread_mgmt_se0=23, compute_static_thread_mgmt_se1=24;
constexpr unsigned compute_static_thread_mgmt_se2=26, compute_static_thread_mgmt_se3=27;
constexpr unsigned cp_mqd_base_addr_lo=128, cp_mqd_base_addr_hi=129;
constexpr unsigned cp_hqd_active=130, cp_hqd_persistent_state=132, cp_hqd_quantum=135;
constexpr unsigned cp_hqd_pq_base_lo=136, cp_hqd_pq_base_hi=137;
constexpr unsigned cp_hqd_pq_rptr_report_addr_lo=139, cp_hqd_pq_rptr_report_addr_hi=140;
constexpr unsigned cp_hqd_pq_wptr_poll_addr_lo=141, cp_hqd_pq_wptr_poll_addr_hi=142;
constexpr unsigned cp_hqd_pq_doorbell_control=143, cp_hqd_pq_control=145;
constexpr unsigned cp_hqd_ib_control=149, cp_hqd_hq_status0=160, cp_mqd_control=162;
constexpr unsigned cp_hqd_eop_base_addr_lo=165, cp_hqd_eop_base_addr_hi=166;
constexpr unsigned cp_hqd_eop_control=167, cp_hqd_aql_control=181;
}
// DWORD doorbell index inside the programmed MEC range, distinct from GFX,
// MES and SDMA. One bounded queue occupies MEC pipe 0, queue 0 while serialized.
constexpr uint32_t kAQLDoorbell = 0x80;
constexpr uint32_t kAQLStorageBytes = 16384, kAQLEOPOffset = 4096;
constexpr uint32_t kAQLRingOffset = 8192, kAQLRingBytes = 4096;
constexpr uint32_t kAQLCompletionOffset = 12288, kAQLInactiveOffset = 12352;
constexpr uint32_t kAQLMetadataOffset = 12416;
static_assert(kAQLMetadataOffset + sizeof(amd_queue_t) <= kAQLStorageBytes);
static_assert(sizeof(amd_signal_t) == 64 && sizeof(hsa_kernel_dispatch_packet_t) == 64);

inline bool aql_build_storage(void *storage, uint64_t base, uint64_t descriptor,
    uint64_t kernarg, const AQLDispatchRequest &r, const uint32_t masks[4], uint32_t cuCount) {
    if (!storage || !aql_dispatch_shape(r) || !base || (base & 16383) ||
        base > (1ull << 48) - kAQLStorageBytes || !descriptor || (descriptor & 63) ||
        descriptor >= (1ull << 48) || !kernarg || (kernarg & 15) ||
        kernarg >= (1ull << 48) || !cuCount || !masks) return false;
    memset(storage, 0, kAQLStorageBytes);
    auto *bytes = static_cast<uint8_t *>(storage);
    auto &mqd = *reinterpret_cast<AQLComputeMQD *>(bytes);
    auto *w = mqd.words;
    using namespace AQLMQDOff;
    w[header]=0xc0310800; w[compute_pipelinestat_enable]=1;
    w[compute_static_thread_mgmt_se0]=masks[0]; w[compute_static_thread_mgmt_se1]=masks[1];
    w[compute_static_thread_mgmt_se2]=masks[2]; w[compute_static_thread_mgmt_se3]=masks[3];
    const auto pair = [&](unsigned lo, uint64_t value) { w[lo]=uint32_t(value); w[lo+1]=uint32_t(value>>32); };
    pair(cp_mqd_base_addr_lo,base); w[cp_mqd_control]=0x100;
    w[cp_hqd_active]=1; w[cp_hqd_persistent_state]=0x5501; w[cp_hqd_quantum]=0x111;
    pair(cp_hqd_pq_base_lo,(base+kAQLRingOffset)>>8);
    pair(cp_hqd_pq_rptr_report_addr_lo,base+kAQLMetadataOffset+offsetof(amd_queue_t,read_dispatch_id));
    pair(cp_hqd_pq_wptr_poll_addr_lo,base+kAQLMetadataOffset+offsetof(amd_queue_t,write_dispatch_id));
    w[cp_hqd_pq_doorbell_control]=0x40000002 | (kAQLDoorbell<<2);
    w[cp_hqd_pq_control]=9 | (5<<8) | (1<<28) | (1<<27) | (2<<18) | (1<<14);
    w[cp_hqd_ib_control]=3<<20;
    // Do not enable PCIe system atomics (bit 29) on this host path.
    w[cp_hqd_hq_status0]=1<<14;
    pair(cp_hqd_eop_base_addr_lo,(base+kAQLEOPOffset)>>8);
    w[cp_hqd_eop_control]=9; w[cp_hqd_aql_control]=1;
    auto &metadata=*reinterpret_cast<amd_queue_t *>(bytes+kAQLMetadataOffset);
    metadata.hsa_queue.type=HSA_QUEUE_TYPE_SINGLE;
    metadata.hsa_queue.features=HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
    metadata.hsa_queue.base_address=reinterpret_cast<void *>(base+kAQLRingOffset);
    metadata.hsa_queue.size=kAQLRingBytes/64;
    metadata.read_dispatch_id_field_base_byte_offset=offsetof(amd_queue_t,read_dispatch_id);
    metadata.max_cu_id=cuCount-1;
    metadata.max_wave_id=31; // gfx12: 2 SIMD/CU, 16 waves/SIMD.
    metadata.queue_properties=AMD_QUEUE_PROPERTIES_IS_PTR64;
    metadata.queue_inactive_signal.handle=base+kAQLInactiveOffset;
    auto &done=*reinterpret_cast<amd_signal_t *>(bytes+kAQLCompletionOffset);
    done.kind=AMD_SIGNAL_KIND_USER; done.value=1;
    auto &inactive=*reinterpret_cast<amd_signal_t *>(bytes+kAQLInactiveOffset);
    inactive.kind=AMD_SIGNAL_KIND_USER;
    auto *packets=reinterpret_cast<hsa_kernel_dispatch_packet_t *>(bytes+kAQLRingOffset);
    for (unsigned i=0;i<kAQLRingBytes/64;++i) packets[i].header=HSA_PACKET_TYPE_INVALID;
    auto &p=packets[0];
    p.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    p.setup=3<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    p.workgroup_size_x=uint16_t(r.threads[0]); p.workgroup_size_y=uint16_t(r.threads[1]); p.workgroup_size_z=uint16_t(r.threads[2]);
    p.grid_size_x=r.groups[0]*r.threads[0]; p.grid_size_y=r.groups[1]*r.threads[1]; p.grid_size_z=r.groups[2]*r.threads[2];
    p.kernel_object=descriptor; p.kernarg_address=reinterpret_cast<void *>(kernarg);
    p.completion_signal.handle=base+kAQLCompletionOffset;
    return true;
}
}
