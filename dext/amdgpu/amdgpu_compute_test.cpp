#include "amdgpu_compute_test.h"
#include "amdgpu_compute_packets.h"
#include "amdgpu_gmc.h"
#include "amdgpu_cp.h"
#include "amdgpu_gfx.h"
#include "amdgpu_vram_io.h"

namespace amdgpu {
static uint32_t compute_input(uint32_t lane, uint32_t seed)
{
    return (lane * 0x9e3779b9u) ^ seed ^ 0x7b19d35au;
}
static uint32_t compute_guard(uint32_t word)
{
    return 0xd39c8a71u ^ (word * 0x01010101u);
}
kern_return_t compute_test(DeviceContext &dev, GMCContext &gmc, CPContext &cp,
    const GFXConfig &gfx, ComputeTest &test, uint32_t seed, ComputeTestResult &result)
{
    result = {};
    result.firstMismatch = UINT32_MAX;
    if (test.active) return kIOReturnBusy;
    if (!dev.pci || !cp.inited || !cp.ringReady || !gmc.vram_alloc.is_inited())
        return kIOReturnNotReady;
    const auto version = dev.ip.version[static_cast<int>(IPBlock::GC)];
    if (version.major != 12 || version.minor != 0 || version.rev != 1)
        return kIOReturnUnsupported;
    if (!gfx.num_active_cus || gfx.max_shader_engines != 4 || gfx.max_sh_per_se != 1)
        return kIOReturnNotReady;
    uint32_t masks[4]{};
    for (uint32_t i = 0; i < 4; ++i) masks[i] = gfx.active_cu_bitmap[i][0];
    // One allocation keeps code, input, output and guards alive together.
    // On any started-test failure, only reset/session destruction releases it.
    result.stage = 1;
    test.active = true;
    if (!gmc.vram_alloc.alloc(kASPageSize, kASPageSize, &test.storage))
        return kIOReturnNoMemory;
    const uint64_t codeVA = test.storage.gpu_va;
    const uint64_t dataVA = codeVA + 4096;
    result.gpuAddress = dataVA;
    uint32_t packets[kComputeSmokePacketCapacity]{};
    const auto count = compute_smoke_packets(packets, codeVA, dataVA, seed, masks);
    if (!count) return kIOReturnBadArgument;
    if (codeVA < gmc.vram_start) return kIOReturnBadArgument;
    const uint64_t offset = codeVA - gmc.vram_start;
    if (!vram_io_range(dev, offset, test.storage.size)) return kIOReturnBadArgument;
    result.stage = 2;
    auto r = vram_clear_verified(dev, offset, test.storage.size);
    if (r != kIOReturnSuccess) return r;
    r = vram_write_verified(dev, offset, kComputeSmokeCode, sizeof(kComputeSmokeCode));
    if (r != kIOReturnSuccess) return r;
    uint32_t words[kComputeSmokeDataBytes / 4];
    for (uint32_t i = 0; i < kComputeSmokeDataBytes / 4; ++i) words[i] = compute_guard(i);
    for (uint32_t i = 0; i < kComputeSmokeLanes; ++i) {
        words[i] = compute_input(i, seed);
        words[kComputeSmokeOutputOffset / 4 + i] = ~(words[i] + seed);
    }
    r = vram_write_verified(dev, offset + 4096, words, sizeof(words));
    if (r != kIOReturnSuccess) return r;
    amdgpu_hdp_flush(dev);
    result.stage = 3;
    if (cp_ring_write(cp, packets, count) != count) return kIOReturnNoSpace;
    r = cp_submit_eop_test(dev, cp, 100000, &result.fence);
    if (r != kIOReturnSuccess) return r;
    result.stage = 4;
    for (uint32_t i = 0; i < kComputeSmokeLanes; ++i)
        words[kComputeSmokeOutputOffset / 4 + i] = compute_input(i, seed) + seed;
    for (uint32_t i = 0; i < kComputeSmokeDataBytes / 4; ++i) {
        uint32_t observed = ~words[i];
        dev.pci->MemoryRead32(dev.bar0MemIndex, offset + 4096 + i * 4, &observed);
        if (observed != words[i]) {
            if (!result.mismatches) result.firstMismatch = i * 4;
            ++result.mismatches;
        }
    }
    if (result.mismatches) return kIOReturnIOError;
    gmc.vram_alloc.free(test.storage);
    test = {};
    result.stage = 5;
    return kIOReturnSuccess;
}
}
