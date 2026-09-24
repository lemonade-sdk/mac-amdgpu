#include "amdgpu_aql.h"
#include "amdgpu_aql_packets.h"
#include "amdgpu_gmc.h"
#include "amdgpu_mes.h"
#include "amdgpu_gfx.h"
#include "amdgpu_vram_io.h"
#include "amdgpu_software_stats.h"
#include <DriverKit/IOLib.h>
#include <time.h>

namespace amdgpu {
kern_return_t aql_launch(DeviceContext &dev, GMCContext &gmc, MESContext &mes,
    const GFXConfig &gfx, AQLLaunch &launch, uint64_t descriptorVA, uint64_t kernargVA,
    const AQLDispatchRequest &request, AQLLaunchResult &result) {
    result={}; result.completion=UINT64_MAX;
    if (launch.retained) return kIOReturnBusy;
    if (!aql_dispatch_shape(request)) return kIOReturnBadArgument;
    const auto v=dev.ip.version[static_cast<int>(IPBlock::GC)];
    if (v.major!=12 || v.minor!=0 || v.rev!=1) return kIOReturnUnsupported;
    if (!dev.pci || !mes.uni_mes_active || !mes.pipe[1].enabled || !mes.pipe[1].inited ||
        mes.pipe[1].submission_pending || !gmc.vram_alloc.is_inited() ||
        !gfx.max_scratch_waves_per_cu ||
        dev.bar2Size < uint64_t(kAQLDoorbell)*4+8) return kIOReturnNotReady;
    uint32_t masks[4];
    if (!gfx12_compute_masks(gfx.max_shader_engines,gfx.max_sh_per_se,gfx.num_active_cus,
        gfx.active_cu_bitmap,masks)) return kIOReturnNotReady;
    if (!gmc.vram_alloc.alloc(kAQLStorageBytes,kAQLStorageBytes,&launch.storage)) return kIOReturnNoMemory;
    const uint64_t base=launch.storage.gpu_va;
    alignas(64) uint8_t staging[kAQLStorageBytes];
    kern_return_t status=kIOReturnBadArgument;
    if (base>=gmc.vram_start && aql_build_storage(staging,base,descriptorVA,kernargVA,request,masks,gfx.num_active_cus,gfx.max_scratch_waves_per_cu)) {
        result.stage=1;
        status=vram_write_verified(dev,base-gmc.vram_start,staging,sizeof(staging));
    }
    if (status!=kIOReturnSuccess) { gmc.vram_alloc.free(launch.storage); launch={}; return status; }
    amdgpu_hdp_flush(dev);
    // Once mapping is attempted firmware may own MQD/ring/storage even when
    // its acknowledgement is lost. Only a complete dispatch AND unmap can
    // release it here; every uncertain path requires verified device reset.
    launch.retained=true;
    result.stage=2;
    status=mes_map_legacy_queue(dev,mes,1,0,0,kAQLDoorbell,base,
        base+kAQLMetadataOffset+offsetof(amd_queue_t,write_dispatch_id));
    if (status!=kIOReturnSuccess) return status;
    result.stage=3;
    const uint64_t writeIndex=1;
    status=vram_write_verified(dev,base-gmc.vram_start+kAQLMetadataOffset+
        offsetof(amd_queue_t,write_dispatch_id),&writeIndex,sizeof(writeIndex));
    if (status!=kIOReturnSuccess) return status;
    amdgpu_hdp_flush(dev);
    // ROCr's 64-bit AQL doorbell receives the last published packet index.
    dev.pci->MemoryWrite64(dev.bar2MemIndex,uint64_t(kAQLDoorbell)*4,0);
    software_stats::PublishedWork work(dev.softwareStats, software_stats::AQL,
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    const uint64_t deadline=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)+uint64_t(request.timeoutUS)*1000;
    do {
        status=vram_read_fence64(dev,base-gmc.vram_start+kAQLCompletionOffset+8,&result.completion);
        if (status!=kIOReturnSuccess) return status;
        status=vram_read_fence64(dev,base-gmc.vram_start+kAQLInactiveOffset+8,&result.inactive);
        if (status!=kIOReturnSuccess) return status;
        if (result.inactive) return kIOReturnIOError;
        if (!result.completion) break;
        if (result.completion!=1) return kIOReturnIOError;
        IOSleep(1);
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW)<deadline);
    if (result.completion) return kIOReturnTimeout;
    work.complete(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    result.stage=4;
    status=mes_unmap_legacy_queue(dev,mes,1,0,0,kAQLDoorbell);
    if (status!=kIOReturnSuccess) return status;
    amdgpu_hdp_flush(dev);
    status=vram_read_fence64(dev,base-gmc.vram_start+kAQLMetadataOffset+
        offsetof(amd_queue_t,read_dispatch_id),&result.readIndex);
    if (status!=kIOReturnSuccess || result.readIndex!=1)
        return status==kIOReturnSuccess ? kIOReturnIOError : status;
    gmc.vram_alloc.free(launch.storage); launch={}; result.stage=5;
    return kIOReturnSuccess;
}

// CP requests scratch only after prior scratch users on this queue have retired.
// Mirror ROCr's non-async HandleInsufficientScratch path; no CPU/GPU atomic RMW
// is needed because CP waits for a release store of zero to the inactive signal.
static kern_return_t aql_queue_allocate_scratch(DeviceContext &dev,GMCContext &gmc,const GFXConfig &gfx,
    PersistentAQLQueue &q,amd_queue_t &metadata,uint32_t laneBytes) {
    uint32_t aligned=0,waves=0;uint64_t bytes=0;
    const auto ip=dev.ip.version[static_cast<int>(IPBlock::GC)];
    if (!aql_scratch_geometry(ip,laneBytes,gfx.num_active_cus,gfx.max_shader_engines,
        gfx.max_scratch_waves_per_cu,aligned,waves,bytes)) return kIOReturnBadArgument;
    // Retain enough slots for every legal 1024-thread workgroup, including
    // wave32. Reducing occupancy in whole 32-wave steps preserves that bound.
    VRAMAllocation replacement{};
    for (;waves>=kAQLScratchMinimumWavesPerEngine;waves-=kAQLScratchMinimumWavesPerEngine) {
        bytes=uint64_t(aligned)*64*waves*gfx.max_shader_engines;
        if (bytes<=UINT32_MAX && gmc.device_vram_alloc.alloc(bytes,16384,&replacement)) break;
    }
    if (!replacement.size) return kIOReturnNoMemory;
    if (!aql_scratch_metadata(ip,metadata,replacement.gpu_va,bytes,aligned,
        gfx.max_shader_engines,waves)) {
        gmc.device_vram_alloc.free(replacement);return kIOReturnBadArgument;
    }
    if (q.scratch.size) gmc.device_vram_alloc.free(q.scratch);
    q.scratch=replacement;
    return kIOReturnSuccess;
}

kern_return_t aql_queue_open(DeviceContext &dev,GMCContext &gmc,MESContext &mes,const GFXConfig &gfx,
    PersistentAQLQueue &q,uint64_t ringVA,uint64_t metadataVA,void *metadataCPU,uint32_t packets,uint32_t slot) {
    if (q.mapped || q.retained || q.storage.size) return kIOReturnBusy;
    if (!metadataCPU || (reinterpret_cast<uintptr_t>(metadataCPU)&63) || slot<1 || slot>kPersistentAQLQueues)
        return kIOReturnBadArgument;
    const auto version=dev.ip.version[static_cast<int>(IPBlock::GC)];
    if (version.major!=12 || version.minor!=0 || version.rev!=1) return kIOReturnUnsupported;
    const uint32_t doorbell=kAQLDoorbell+slot*2;
    if (!dev.pci || !mes.uni_mes_active || !mes.pipe[1].inited || !mes.pipe[1].enabled ||
        mes.pipe[1].submission_pending || !gfx.max_scratch_waves_per_cu ||
        dev.bar2Size<uint64_t(doorbell)*4+8) return kIOReturnNotReady;
    uint32_t masks[4];
    if (!gfx12_compute_masks(gfx.max_shader_engines,gfx.max_sh_per_se,gfx.num_active_cus,
        gfx.active_cu_bitmap,masks)) return kIOReturnNotReady;
    auto &metadata=*static_cast<amd_queue_t *>(metadataCPU);
    if (__atomic_load_n(&metadata.write_dispatch_id,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&metadata.read_dispatch_id,__ATOMIC_ACQUIRE) ||
        metadata.hsa_queue.size!=packets || reinterpret_cast<uintptr_t>(metadata.hsa_queue.base_address)!=ringVA ||
        metadata.read_dispatch_id_field_base_byte_offset!=offsetof(amd_queue_t,read_dispatch_id) ||
        metadata.caps || metadata.queue_properties!=AMD_QUEUE_PROPERTIES_IS_PTR64)
        return kIOReturnBadArgument;
    if (metadata.scratch_wave64_lane_byte_size>kAQLMaxPrivateBytes) return kIOReturnBadArgument;
    if (!gmc.vram_alloc.alloc(kAQLStorageBytes,kAQLStorageBytes,&q.storage)) return kIOReturnNoMemory;
    alignas(64) uint8_t staging[kAQLStorageBytes]{};
    auto &mqd=*reinterpret_cast<AQLComputeMQD *>(staging);
    const uint64_t base=q.storage.gpu_va;
    if (base<gmc.vram_start || !aql_build_mqd(mqd,base,ringVA,metadataVA,packets,doorbell,masks)) {
        gmc.vram_alloc.free(q.storage);q.storage={};return kIOReturnBadArgument;
    }
    auto &inactive=*reinterpret_cast<amd_signal_t *>(staging+kAQLInactiveOffset);
    inactive.kind=AMD_SIGNAL_KIND_USER;
    const auto requestedScratch=metadata.scratch_wave64_lane_byte_size;
    // Never trust user-provided scratch GPU addresses or resource encodings.
    metadata.compute_tmpring_size=0;
    memset(metadata.scratch_resource_descriptor,0,sizeof(metadata.scratch_resource_descriptor));
    metadata.scratch_backing_memory_location=0;metadata.scratch_wave64_lane_byte_size=0;
    metadata.group_segment_aperture_base_hi=kAQLGroupApertureHi;
    metadata.private_segment_aperture_base_hi=kAQLPrivateApertureHi;
    if (requestedScratch) {
        const auto scratchStatus=aql_queue_allocate_scratch(dev,gmc,gfx,q,metadata,requestedScratch);
        if (scratchStatus!=kIOReturnSuccess) {
            gmc.vram_alloc.free(q.storage);q.storage={};return scratchStatus;
        }
    }
    metadata.max_cu_id=gfx.num_active_cus-1;metadata.max_wave_id=gfx.max_scratch_waves_per_cu-1;
    metadata.queue_inactive_signal.handle=base+kAQLInactiveOffset;
    auto status=vram_write_verified(dev,base-gmc.vram_start,staging,sizeof(staging));
    if (status!=kIOReturnSuccess) {
        if (q.scratch.size) gmc.device_vram_alloc.free(q.scratch);q.scratch={};
        gmc.vram_alloc.free(q.storage);q.storage={};return status;
    }
    q.metadataVA=metadataVA;q.metadataCPU=metadataCPU;q.packets=packets;q.slot=slot;
    q.retained=true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);amdgpu_hdp_flush(dev);
    status=mes_map_legacy_queue(dev,mes,1,gfx1201_compute_pipe(slot),gfx1201_compute_queue(slot),doorbell,base,
        metadataVA+offsetof(amd_queue_t,write_dispatch_id));
    if (status==kIOReturnSuccess) {q.mapped=true;q.retained=false;}
    return status;
}
bool aql_queue_cpu_read_index(const PersistentAQLQueue &q, uint64_t &index) {
    if (!q.mapped || !q.metadataCPU) return false;
    const auto &metadata = *static_cast<const amd_queue_t *>(q.metadataCPU);
    index = __atomic_load_n(&metadata.read_dispatch_id, __ATOMIC_ACQUIRE);
    return true;
}

kern_return_t aql_queue_kick(DeviceContext &dev,PersistentAQLQueue &q,uint64_t lastPacket) {
    if (!q.mapped || q.retained || !q.metadataCPU || !dev.pci) return kIOReturnNotReady;
    if (lastPacket==UINT64_MAX) return kIOReturnBadArgument;
    auto &metadata=*static_cast<amd_queue_t *>(q.metadataCPU);
    const auto reserved=__atomic_load_n(&metadata.write_dispatch_id,__ATOMIC_ACQUIRE);
    if (lastPacket>=reserved) return kIOReturnBadArgument;
    // Producers can ring out of order. A stale wakeup must not move hardware
    // WPTR backwards; the largest published packet index remains authoritative.
    if (q.published && lastPacket<=q.lastDoorbell) return kIOReturnSuccess;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);amdgpu_hdp_flush(dev);
    dev.pci->MemoryWrite64(dev.bar2MemIndex,uint64_t(kAQLDoorbell+q.slot*2)*4,lastPacket);
    q.lastDoorbell=lastPacket;q.published=true;
    return kIOReturnSuccess;
}
kern_return_t aql_queue_service(DeviceContext &dev,GMCContext &gmc,const GFXConfig &gfx,
    PersistentAQLQueue &q,const void *ringCPU,uint64_t &inactive) {
    inactive=0;
    if (!q.mapped || q.retained || !q.metadataCPU || !ringCPU ||
        q.storage.gpu_va<gmc.vram_start) return kIOReturnNotReady;
    const uint64_t signalOffset=q.storage.gpu_va-gmc.vram_start+kAQLInactiveOffset+8;
    auto status=vram_read_fence64(dev,signalOffset,&inactive);
    if (status!=kIOReturnSuccess || !inactive) return status;
    // Scratch requests may encode wave32 in bit 10. Never clear unrelated
    // packet, allocation, ISA or memory-fault errors as if they were recoverable.
    if (!(inactive&0x401) || (inactive&~uint64_t(0x401))) return kIOReturnIOError;
    auto &metadata=*static_cast<amd_queue_t *>(q.metadataCPU);
    const uint64_t read=__atomic_load_n(&metadata.read_dispatch_id,__ATOMIC_ACQUIRE);
    const uint64_t write=__atomic_load_n(&metadata.write_dispatch_id,__ATOMIC_ACQUIRE);
    if (write<=read) return kIOReturnIOError;
    // MULTI producers may reserve beyond ring capacity while waiting for space.
    const uint64_t available=(write-read<q.packets ? write-read : q.packets);
    const auto *ring=static_cast<const hsa_kernel_dispatch_packet_t *>(ringCPU);
    uint32_t required=0;
    for (uint64_t cursor=read;cursor-read<available;++cursor) {
        const auto &packet=ring[cursor&(q.packets-1)];
        const auto header=__atomic_load_n(&packet.header,__ATOMIC_ACQUIRE);
        const auto type=header&255;
        // A producer may have reserved a later slot without publishing it.
        if (type==HSA_PACKET_TYPE_INVALID) break;
        if (type!=HSA_PACKET_TYPE_KERNEL_DISPATCH || !packet.private_segment_size) continue;
        required=packet.private_segment_size;break;
    }
    if (!required || required>kAQLMaxPrivateBytes) return kIOReturnBadArgument;
    status=aql_queue_allocate_scratch(dev,gmc,gfx,q,metadata,required);
    if (status!=kIOReturnSuccess) return status;
    __atomic_thread_fence(__ATOMIC_RELEASE);amdgpu_hdp_flush(dev);
    const uint64_t zero=0;
    status=vram_write_verified(dev,signalOffset,&zero,sizeof(zero));
    amdgpu_hdp_flush(dev);
    if (status==kIOReturnSuccess) inactive=0;
    return status;
}
kern_return_t aql_queue_close(DeviceContext &dev,GMCContext &gmc,MESContext &mes,PersistentAQLQueue &q) {
    if (q.retained) return kIOReturnNotReady;
    if (!q.mapped) return kIOReturnBadArgument;
    q.retained=true;
    const auto status=mes_unmap_legacy_queue(dev,mes,1,gfx1201_compute_pipe(q.slot),gfx1201_compute_queue(q.slot),kAQLDoorbell+q.slot*2);
    if (status!=kIOReturnSuccess) return status;
    if (q.scratch.size) gmc.device_vram_alloc.free(q.scratch);
    gmc.vram_alloc.free(q.storage);q={};
    return kIOReturnSuccess;
}

}
