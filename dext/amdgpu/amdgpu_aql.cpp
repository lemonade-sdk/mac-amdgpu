#include "amdgpu_aql.h"
#include "amdgpu_aql_packets.h"
#include "amdgpu_gmc.h"
#include "amdgpu_mes.h"
#include "amdgpu_gfx.h"
#include "amdgpu_vram_io.h"
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
        gfx.max_shader_engines!=4 || gfx.max_sh_per_se!=1 || !gfx.num_active_cus ||
        dev.bar2Size < uint64_t(kAQLDoorbell)*4+8) return kIOReturnNotReady;
    if (!gmc.vram_alloc.alloc(kAQLStorageBytes,kAQLStorageBytes,&launch.storage)) return kIOReturnNoMemory;
    const uint64_t base=launch.storage.gpu_va;
    alignas(64) uint8_t staging[kAQLStorageBytes];
    uint32_t masks[4];
    for (unsigned i=0;i<4;++i) masks[i]=gfx.active_cu_bitmap[i][0];
    kern_return_t status=kIOReturnBadArgument;
    if (base>=gmc.vram_start && aql_build_storage(staging,base,descriptorVA,kernargVA,request,masks,gfx.num_active_cus)) {
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
}
