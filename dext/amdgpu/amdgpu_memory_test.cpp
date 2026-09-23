#include "amdgpu_memory_test.h"
#include "amdgpu_gmc.h"
#include "amdgpu_sdma.h"
#include "amdgpu_vram_io.h"

namespace amdgpu {
static constexpr uint32_t kTransferBytes = 16384;
static uint32_t memory_test_word(uint32_t index, uint32_t seed)
{
    return (index * 0x9e3779b9u) ^ seed ^ 0xa53cc35au;
}

void memory_transfer_release_after_reset(MemoryTransferTest &test)
{
    gart_release_after_reset(test.host);
    if (test.staging) test.staging->release();
    test = {}; // VRAM arena is discarded with the enclosing GMC session.
}

kern_return_t memory_transfer_test(DeviceContext &dev, GMCContext &gmc,
    GARTContext &gart, SDMAInstance &sdma, MemoryTransferTest &test,
    uint32_t seed, MemoryTransferResult &result)
{
    result = {};
    result.firstMismatch = UINT32_MAX;
    if (test.active) return kIOReturnBusy;
    if (!gart.enabled || !sdma.inited || !sdma.enabled) return kIOReturnNotReady;
    test.active = true;
    result.stage = 1;
    auto r = gart_bind_sysmem(dev, gart, 2 * kTransferBytes, kASPageSize, &test.host);
    if (r != kIOReturnSuccess) return r;
    result.hostGPUAddress = test.host.gartMCAddr;
    if (!gmc.vram_alloc.alloc(kTransferBytes, kASPageSize, &test.vram)) return kIOReturnNoMemory;
    result.vramGPUAddress = test.vram.gpu_va;
    r = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
        kTransferBytes, kASPageSize, &test.staging);
    if (r != kIOReturnSuccess || !test.staging) return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    r = test.staging->SetLength(kTransferBytes);
    if (r != kIOReturnSuccess) return r;
    IOAddressSegment cpu{};
    r = test.staging->GetAddressRange(&cpu);
    if (r != kIOReturnSuccess || !cpu.address || cpu.length < kTransferBytes)
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    auto *words = reinterpret_cast<uint32_t *>(cpu.address);
    for (uint32_t i = 0; i < kTransferBytes / 4; ++i) words[i] = memory_test_word(i, seed);

    result.stage = 2;
    // DriverKit's documented CPU access to a prepared DMA mapping. Keep
    // the separate staging descriptor alive through both transfer legs.
    r = test.host.dmaCommand->PerformOperation(kIODMACommandPerformOperationOptionWrite,
        0, kTransferBytes, 0, test.staging);
    if (r != kIOReturnSuccess) return r;
    r = test.host.dmaCommand->PerformOperation(kIODMACommandPerformOperationOptionZero,
        kTransferBytes, kTransferBytes, 0, nullptr);
    if (r != kIOReturnSuccess) return r;
    const uint64_t offset = test.vram.gpu_va - gmc.vram_start;
    r = vram_clear_verified(dev, offset, kTransferBytes);
    if (r != kIOReturnSuccess) return r;
    amdgpu_hdp_flush(dev);
    result.stage = 3;
    r = sdma_copy_linear_test(dev, sdma, test.host.gartMCAddr,
        test.vram.gpu_va, kTransferBytes, 100000);
    if (r != kIOReturnSuccess) return r;
    result.stage = 4;
    for (uint32_t i = 0; i < kTransferBytes / 4; ++i) {
        uint32_t observed = UINT32_MAX;
        dev.pci->MemoryRead32(dev.bar0MemIndex, offset + i * 4, &observed);
        if (observed != memory_test_word(i, seed)) {
            if (!result.mismatches) result.firstMismatch = i * 4;
            ++result.mismatches;
        }
    }
    if (result.mismatches) return kIOReturnIOError;

    // Distinct pattern for the reverse leg prevents stale source data from
    // masquerading as a successful GPU write to the second host region.
    seed ^= 0x5a96e1c3u;
    for (uint32_t i = 0; i < kTransferBytes / 4; ++i) words[i] = memory_test_word(i, seed);
    r = vram_write_verified(dev, offset, words, kTransferBytes);
    if (r != kIOReturnSuccess) return r;
    amdgpu_hdp_flush(dev);
    result.stage = 5;
    r = sdma_copy_linear_test(dev, sdma, test.vram.gpu_va,
        test.host.gartMCAddr + kTransferBytes, kTransferBytes, 100000);
    if (r != kIOReturnSuccess) return r;
    result.stage = 6;
    memset(words, 0, kTransferBytes);
    r = test.host.dmaCommand->PerformOperation(kIODMACommandPerformOperationOptionRead,
        kTransferBytes, kTransferBytes, 0, test.staging);
    if (r != kIOReturnSuccess) return r;
    for (uint32_t i = 0; i < kTransferBytes / 4; ++i) {
        if (words[i] != memory_test_word(i, seed)) {
            if (!result.mismatches) result.firstMismatch = i * 4;
            ++result.mismatches;
        }
    }
    if (result.mismatches) return kIOReturnIOError;
    result.stage = 7;
    r = gart_unbind(dev, gart, &test.host);
    if (r != kIOReturnSuccess) return r;
    test.staging->release();
    gmc.vram_alloc.free(test.vram);
    test = {};
    result.stage = 8;
    // A controlled transfer does not yet enable general GTT BO allocation.
    return kIOReturnSuccess;
}
}
