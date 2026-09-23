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
    uint32_t masks[4]{};
    if (!gfx12_compute_masks(gfx.max_shader_engines,gfx.max_sh_per_se,gfx.num_active_cus,
        gfx.active_cu_bitmap,masks)) return kIOReturnNotReady;
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
    uint32_t dispatchOffset = 0;
    const auto count = compute_smoke_packets(packets, codeVA, dataVA, seed, masks, &dispatchOffset);
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
    // Verify cache preparation and register programming independently before
    // launching the shader. A timeout now identifies the first uncompleted
    // phase instead of attributing every stalled packet to shader execution.
    uint32_t ibSlot = 0;
    auto submit = [&](uint32_t start, uint32_t length) -> kern_return_t {
        // Dispatch must enter through an IB with an explicit application VMID,
        // as in Linux's GFX ring submission. HQD VMID alone only establishes
        // the queue's memory context; direct shader packets inherited VMID3
        // during the first hardware test and faulted on instruction fetch.
        uint32_t ib[kComputeSmokePacketCapacity + 8]{};
        for (uint32_t i = 0; i < length; ++i) ib[i] = packets[start + i];
        uint32_t padded = (length + 7) & ~7u;
        if (padded - length == 1) padded += 8; // NOP needs header + payload.
        if (padded != length) ib[length] = pm4_header(kPM4OpNop, padded - length - 2);
        const uint64_t ibOffset = 8192 + uint64_t(ibSlot++) * 1024;
        auto status = vram_write_verified(dev, offset + ibOffset, ib, padded * 4);
        if (status != kIOReturnSuccess) return status;
        amdgpu_hdp_flush(dev);
        uint32_t launch[4]{};
        if (!pm4_gfx_ib(launch, codeVA + ibOffset, padded, 0)) return kIOReturnBadArgument;
        if (cp_ring_write(cp, launch, 4) != 4) return kIOReturnNoSpace;
        return cp_submit_eop_test(dev, cp, 100000, &result.fence);
    };
    result.stage = 3;
    r = submit(0, kComputeSmokeAcquireDwords);
    if (r != kIOReturnSuccess) return r;
    result.stage = 4;
    r = submit(kComputeSmokeAcquireDwords, dispatchOffset - kComputeSmokeAcquireDwords);
    if (r != kIOReturnSuccess) return r;
    result.stage = 5;
    r = submit(dispatchOffset, count - dispatchOffset);
    if (r != kIOReturnSuccess) return r;
    result.stage = 6;
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
    result.stage = 7;
    return kIOReturnSuccess;
}
}
