//
//  cp_v12_0.cpp — CP/KIQ storage allocation + PM4 ring writer for GFX12.
//
//  This chunk's scope: storage and PM4 packet staging. The actual
//  HQD register programming, doorbell setup, and CP enable land in
//  the next chunk.
//
//  Sources:
//    drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c (cp_gfx_resume + kiq_resume)
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>

#include "amdgpu_cp.h"
#include "amdgpu_gmc.h"

#define CP_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.cp: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// Local copy of the alloc_dma_block helper from gmc_v12_0.cpp.
// Promote to shared utility when we have a third consumer.
static kern_return_t
cp_alloc_dma_block(DeviceContext &dev, uint64_t size,
                   IOBufferMemoryDescriptor **outBuf,
                   IODMACommand             **outDma,
                   uint64_t *outBus,
                   void    **outCpu)
{
    *outBuf = nullptr; *outDma = nullptr; *outBus = 0; *outCpu = nullptr;
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t r = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, size, kASPageSize, &buf);
    if (r != kIOReturnSuccess || buf == nullptr) {
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    buf->SetLength(size);
    IODMACommandSpecification spec = {};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;
    IODMACommand *dma = nullptr;
    r = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                             &spec, &dma);
    if (r != kIOReturnSuccess || dma == nullptr) {
        buf->release();
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t segCount = 1;
    IOAddressSegment seg = {};
    r = dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, buf, 0,
                           size, &flags, &segCount, &seg);
    if (r != kIOReturnSuccess || segCount != 1) {
        dma->release(); buf->release();
        return r != kIOReturnSuccess ? r : kIOReturnNotAligned;
    }
    IOAddressSegment cpu = {};
    buf->GetAddressRange(&cpu);
    *outBuf = buf;
    *outDma = dma;
    *outBus = seg.address;
    *outCpu = reinterpret_cast<void *>(cpu.address);
    return kIOReturnSuccess;
}

// ----- cp_alloc_storage: ring + MQD + write-back page -----
kern_return_t
cp_alloc_storage(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    if (cp.inited) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) return kIOReturnNotReady;

    // KIQ ring — power-of-two, 16 KB-aligned, in visible VRAM.
    if (!gmc.vram_alloc.alloc(kCPRingDefaultBytes, kASPageSize,
                              &cp.kiq_ring)) {
        CP_LOG("KIQ ring VRAM alloc failed (%llu free)",
               gmc.vram_alloc.bytes_free());
        return kIOReturnNoMemory;
    }
    cp.kiq_ring_size_dwords = kCPRingDefaultBytes / 4;
    cp.kiq_ring_ptr_mask    = cp.kiq_ring_size_dwords - 1;

    // KIQ MQD — 4 KB in VRAM. The CP reads MQD once at queue-create
    // and after that ignores it; alignment is just AS page rule.
    if (!gmc.vram_alloc.alloc(kCPMQDBytes, kASPageSize, &cp.kiq_mqd)) {
        CP_LOG("KIQ MQD VRAM alloc failed");
        return kIOReturnNoMemory;
    }

    // Write-back page in sysmem.
    void *cpu = nullptr;
    kern_return_t r = cp_alloc_dma_block(dev, kCPWBPageBytes,
                                          &cp.wb_buf, &cp.wb_dma,
                                          &cp.wb_bus, &cpu);
    if (r != kIOReturnSuccess) {
        CP_LOG("WB page alloc failed: %#x", r);
        return r;
    }
    auto *wb = static_cast<uint8_t *>(cpu);
    memset(wb, 0, kCPWBPageBytes);
    cp.wb_cpu        = cpu;
    cp.rptr_cpu      = reinterpret_cast<volatile uint32_t *>(wb + kCPWBOffsetRptr);
    cp.wptr_cpu      = reinterpret_cast<volatile uint32_t *>(wb + kCPWBOffsetWptr);
    cp.fence_cpu     = reinterpret_cast<volatile uint64_t *>(wb + kCPWBOffsetFence);
    cp.rptr_gpu_addr = cp.wb_bus + kCPWBOffsetRptr;
    cp.wptr_gpu_addr = cp.wb_bus + kCPWBOffsetWptr;
    cp.fence_gpu_addr = cp.wb_bus + kCPWBOffsetFence;

    cp.wptr          = 0;
    cp.fence_counter = 0;
    cp.doorbell_index = 0;  // assigned in next chunk
    cp.inited        = true;

    CP_LOG("storage ok: ring gpu=%#llx (%u dwords), mqd gpu=%#llx, "
           "wb bus=%#llx (rptr=%#llx wptr=%#llx fence=%#llx)",
           cp.kiq_ring.gpu_va, cp.kiq_ring_size_dwords,
           cp.kiq_mqd.gpu_va, cp.wb_bus,
           cp.rptr_gpu_addr, cp.wptr_gpu_addr, cp.fence_gpu_addr);
    return kIOReturnSuccess;
}

void
cp_release_storage(CPContext &cp)
{
    if (cp.wb_dma != nullptr) {
        cp.wb_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        cp.wb_dma->release(); cp.wb_dma = nullptr;
    }
    if (cp.wb_buf != nullptr) {
        cp.wb_buf->release(); cp.wb_buf = nullptr;
    }
    cp.wb_bus = 0; cp.wb_cpu = nullptr;
    cp.rptr_cpu = nullptr; cp.wptr_cpu = nullptr; cp.fence_cpu = nullptr;
    cp.inited = false;
    // KIQ ring + MQD are bump-allocated; not freed.
}

// ----- cp_ring_write: stage PM4 dwords into the ring -----
//
// The KIQ ring buffer lives in VRAM. We can't write to VRAM via
// CPU directly until we have a BAR2 mapping in the dext (clients
// do their own). For staging from inside the dext we'd need
// dext-side BAR2 mapping — TODO when we actually exercise this on
// hardware. For now, the function pretends to write and tracks
// the wptr.
//
// When the next chunk wires this up to userspace, the userspace
// client will map BAR2 + KIQ ring page, write PM4 dwords directly
// via the visible VRAM aperture, then call a "kick doorbell"
// selector that updates wptr and writes the doorbell register.
uint32_t
cp_ring_write(CPContext &cp, const uint32_t *src, uint32_t dwords)
{
    if (!cp.inited || src == nullptr || dwords == 0) return 0;
    if (dwords > cp.kiq_ring_size_dwords / 2) {
        CP_LOG("ring_write: %u dwords exceeds half-ring %u",
               dwords, cp.kiq_ring_size_dwords / 2);
        return 0;
    }
    // Without a dext-side BAR2 mapping we can only advance the
    // wptr counter — the actual ring memory writes happen from
    // userspace once we expose BAR2 + a kick-doorbell selector.
    // The wptr accounting still has to be correct so userspace
    // and dext agree on where the next packet goes.
    for (uint32_t i = 0; i < dwords; i++) {
        cp.wptr = (cp.wptr + 1) & cp.kiq_ring_ptr_mask;
        (void)src[i];   // placeholder for the actual write
    }
    return dwords;
}

// ----- cp_emit_eop_fence: build NOP + RELEASE_MEM -----
//
// Returns the fence value the EOP write will deposit. Caller's
// responsibility to (a) kick the doorbell, (b) poll *fence_cpu.
uint32_t
cp_emit_eop_fence(CPContext &cp)
{
    if (!cp.inited) return 0;

    uint32_t pkt[16];
    uint32_t n = 0;

    // 1) NOP — sanity warm-up.
    pkt[n++] = pm4_nop();

    // 2) RELEASE_MEM with INT_SEL_SEND_INT so the IH ring sees it.
    uint32_t fence = ++cp.fence_counter;
    pkt[n++] = pm4_header(kPM4OpReleaseMem, 6);  // 7 payload DWORDs - 1
    pkt[n++] = pm4_release_mem_dw1();
    pkt[n++] = pm4_release_mem_dw2(kPM4RMDataSel64, kPM4RMIntSelSendInt);
    pkt[n++] = static_cast<uint32_t>(cp.fence_gpu_addr & 0xFFFFFFFCu);
    pkt[n++] = static_cast<uint32_t>(cp.fence_gpu_addr >> 32);
    pkt[n++] = fence;       // fence_value lo
    pkt[n++] = 0;           // fence_value hi (we use 32-bit values for now)
    pkt[n++] = 0;           // pad

    cp_ring_write(cp, pkt, n);
    CP_LOG("emitted EOP fence value %u (wptr=%u)", fence, cp.wptr);
    return fence;
}

// ----- cp_init_full: BringupStage::CPInit entry -----
kern_return_t
cp_init_full(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    kern_return_t r = cp_alloc_storage(dev, gmc, cp);
    if (r != kIOReturnSuccess) return r;
    CP_LOG("CP storage staged; HQD/doorbell programming pending next chunk");
    return kIOReturnSuccess;
}

} // namespace amdgpu
