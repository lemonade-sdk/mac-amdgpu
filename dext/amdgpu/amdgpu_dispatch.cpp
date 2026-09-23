#include "amdgpu_dispatch.h"
#include "amdgpu_compute_packets.h"
#include "amdgpu_gmc.h"
#include "amdgpu_cp.h"
#include "amdgpu_gfx.h"
#include "amdgpu_vram_io.h"

namespace amdgpu {
kern_return_t compute_launch(DeviceContext &dev, GMCContext &gmc, CPContext &cp,
    const GFXConfig &gfx, ComputeLaunch &launch, uint64_t codeVA,
    const ComputeDispatchRequest &request, ComputeLaunchResult &result)
{
    result = {};
    if (launch.retained) return kIOReturnBusy;
    if (!dev.pci || !cp.inited || !cp.ringReady || !gmc.vram_alloc.is_inited())
        return kIOReturnNotReady;
    const auto v = dev.ip.version[static_cast<int>(IPBlock::GC)];
    if (v.major != 12 || v.minor != 0 || v.rev != 1) return kIOReturnUnsupported;
    if (!gfx.num_active_cus || gfx.max_shader_engines != 4 || gfx.max_sh_per_se != 1)
        return kIOReturnNotReady;
    uint32_t masks[4]{};
    for (unsigned i = 0; i < 4; ++i) masks[i] = gfx.active_cu_bitmap[i][0];
    uint32_t packets[kComputeSmokePacketCapacity]{};
    const uint32_t count = compute_dispatch_packets(packets, codeVA, request, masks);
    if (!count) return kIOReturnBadArgument;
    uint32_t padded = (count + 7) & ~7u;
    if (padded - count == 1) padded += 8;
    if (padded > kComputeSmokePacketCapacity) return kIOReturnNoSpace;
    if (padded != count) packets[count] = pm4_header(kPM4OpNop, padded - count - 2);
    if (!gmc.vram_alloc.alloc(kASPageSize, kASPageSize, &launch.ib)) return kIOReturnNoMemory;
    result.stage = 1;
    kern_return_t status = kIOReturnBadArgument;
    if (launch.ib.gpu_va >= gmc.vram_start)
        status = vram_write_verified(dev, launch.ib.gpu_va - gmc.vram_start, packets, padded * 4);
    if (status == kIOReturnSuccess) {
        uint32_t ib[4]{};
        if (!pm4_gfx_ib(ib, launch.ib.gpu_va, padded, 0)) status = kIOReturnBadArgument;
        else {
            amdgpu_hdp_flush(dev);
            result.stage = 2;
            const auto previousWptr = cp.wptr;
            if (cp_ring_write(cp, ib, 4) != 4) status = kIOReturnNoSpace;
            else status = cp_submit_eop_test(dev, cp, request.timeoutUS, &result.fence);
            // Even an unpublished staged IB could be kicked by a subsequent
            // submission. Retain it and all owner BOs until reset on failure.
            if (status != kIOReturnSuccess && cp.wptr != previousWptr) {
                launch.retained = true;
                return status;
            }
        }
    }
    gmc.vram_alloc.free(launch.ib);
    launch = {};
    if (status == kIOReturnSuccess) result.stage = 3;
    return status;
}
}
