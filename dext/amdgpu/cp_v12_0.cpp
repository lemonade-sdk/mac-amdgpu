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
#include "amdgpu_gfx.h"

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

// ----- cp_alloc_storage: ring + MQD + write-back page (all sysmem) -----
kern_return_t
cp_alloc_storage(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    (void)gmc;
    if (cp.inited) return kIOReturnSuccess;

    void *cpu = nullptr;
    kern_return_t r;

    // KIQ ring — power-of-two, 16 KB-aligned, in DART sysmem. CP
    // fetches PM4 through GART translation; sysmem is the easy path
    // until we have a VRAM-resident allocator that's also CPU-writable
    // (which on AS requires a BAR2 mapping inside the dext).
    r = cp_alloc_dma_block(dev, kCPRingDefaultBytes,
                           &cp.ring_buf, &cp.ring_dma,
                           &cp.ring_bus, &cpu);
    if (r != kIOReturnSuccess) {
        CP_LOG("KIQ ring sysmem alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kCPRingDefaultBytes);
    cp.ring_cpu        = cpu;
    cp.ring_size_dwords = kCPRingDefaultBytes / 4;
    cp.ring_ptr_mask    = cp.ring_size_dwords - 1;

    // KIQ MQD — 4 KB sysmem. CP reads it once at queue-create and
    // doesn't touch it after.
    r = cp_alloc_dma_block(dev, kCPMQDBytes,
                           &cp.mqd_buf, &cp.mqd_dma,
                           &cp.mqd_bus, &cpu);
    if (r != kIOReturnSuccess) {
        CP_LOG("KIQ MQD sysmem alloc failed: %#x", r);
        return r;
    }
    memset(cpu, 0, kCPMQDBytes);
    cp.mqd_cpu = cpu;

    // Write-back page — sysmem, 16 KB. CP writes rptr/wptr/fence
    // values here; host reads from the same backing.
    r = cp_alloc_dma_block(dev, kCPWBPageBytes,
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
    cp.rptr_gpu_addr  = cp.wb_bus + kCPWBOffsetRptr;
    cp.wptr_gpu_addr  = cp.wb_bus + kCPWBOffsetWptr;
    cp.fence_gpu_addr = cp.wb_bus + kCPWBOffsetFence;

    cp.wptr           = 0;
    cp.fence_counter  = 0;
    cp.doorbell_index = 0;  // assigned in next chunk
    cp.inited         = true;

    CP_LOG("storage ok: ring bus=%#llx (%u dwords), mqd bus=%#llx, "
           "wb bus=%#llx (rptr=%#llx wptr=%#llx fence=%#llx)",
           cp.ring_bus, cp.ring_size_dwords,
           cp.mqd_bus, cp.wb_bus,
           cp.rptr_gpu_addr, cp.wptr_gpu_addr, cp.fence_gpu_addr);
    return kIOReturnSuccess;
}

void
cp_release_storage(CPContext &cp)
{
    auto teardown = [](IOBufferMemoryDescriptor *&b, IODMACommand *&d) {
        if (d != nullptr) {
            d->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            d->release(); d = nullptr;
        }
        if (b != nullptr) { b->release(); b = nullptr; }
    };
    teardown(cp.wb_buf, cp.wb_dma);
    teardown(cp.mqd_buf, cp.mqd_dma);
    teardown(cp.ring_buf, cp.ring_dma);
    cp.wb_bus = 0; cp.wb_cpu = nullptr;
    cp.mqd_bus = 0; cp.mqd_cpu = nullptr;
    cp.ring_bus = 0; cp.ring_cpu = nullptr;
    cp.rptr_cpu = nullptr; cp.wptr_cpu = nullptr; cp.fence_cpu = nullptr;
    cp.inited = false;
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
    if (dwords > cp.ring_size_dwords / 2) {
        CP_LOG("ring_write: %u dwords exceeds half-ring %u",
               dwords, cp.ring_size_dwords / 2);
        return 0;
    }
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    for (uint32_t i = 0; i < dwords; i++) {
        ring[cp.wptr] = src[i];
        cp.wptr = (cp.wptr + 1) & cp.ring_ptr_mask;
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

// ----- cp_hqd_program: write CP_RB0_* registers -----
//
// Mirrors gfx_v12_0_cp_gfx_resume (drivers/gpu/drm/amd/amdgpu/
// gfx_v12_0.c). On a fresh device the HQD registers are at reset
// defaults; this routine programs them to match our ring buffer
// location, size, and doorbell mapping so subsequent doorbell
// rings cause the CP to fetch + execute our PM4.
//
// Skips per-pipe GRBM_GFX_INDEX selection — we use the default
// pipe (RB0). Caller is responsible for taking the SRBM mutex if
// multi-context safety is required (Phase 2 concern).
kern_return_t
cp_hqd_program(const DeviceContext &dev, CPContext &cp)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    auto reg = [&](uint32_t off) {
        return SOC15_REG_OFFSET(dev, IPBlock::GC, off);
    };

    // Ring physical base — register holds bits [39:8] (256 B granularity).
    WREG32(dev, reg(CPRegs::CP_RB0_BASE),
           static_cast<uint32_t>(cp.ring_bus >> 8));
    WREG32(dev, reg(CPRegs::CP_RB0_BASE_HI),
           static_cast<uint32_t>(cp.ring_bus >> 40));

    // CP_RB0_CNTL: RB_BUFSZ = log2(ring_dwords) - 1 (per upstream
    // order_base_2(ring_size_bytes/8) where ring_size is bytes).
    // For 16 KB ring = 4096 dwords, BUFSZ = log2(4096/2) = 11.
    // (Upstream encodes "log2(elements) - 1" where element = 8 bytes.)
    uint32_t buf_log2 = 0;
    {
        uint32_t v = cp.ring_size_dwords / 2;  // upstream divides by 2
        while (v >>= 1) buf_log2++;
    }
    uint32_t cntl = (buf_log2 & 0x3F) | ((8u & 0x3F) << 8);  // BUFSZ + BLKSZ=8
    WREG32(dev, reg(CPRegs::CP_RB0_CNTL), cntl);

    // VMID 0 — all kernel-driver submits use system VMID for now.
    WREG32(dev, reg(CPRegs::CP_RB_VMID), 0);

    // Reset wptr to 0 (CP echoes; we also update software state).
    WREG32(dev, reg(CPRegs::CP_RB0_WPTR), 0);
    cp.wptr = 0;
    *cp.wptr_cpu = 0;
    *cp.rptr_cpu = 0;
    *cp.fence_cpu = 0;

    // RPTR write-back address — GPU writes its current read pointer
    // into our WB page at this offset so the CPU can poll it without
    // hitting MMIO.
    WREG32(dev, reg(CPRegs::CP_RB0_RPTR_ADDR),
           static_cast<uint32_t>(cp.rptr_gpu_addr & 0xFFFFFFFCu));
    WREG32(dev, reg(CPRegs::CP_RB0_RPTR_ADDR_HI),
           static_cast<uint32_t>(cp.rptr_gpu_addr >> 32));

    // Doorbell control — index << 2 per upstream encoding, plus
    // doorbell-enable bit (we set bit 30 to enable the doorbell).
    constexpr uint32_t kDoorbellEnable = (1u << 30);
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_CONTROL),
           kDoorbellEnable | ((cp.doorbell_index & 0xFFFFFF) << 2));
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_LOWER),
           (cp.doorbell_index & 0xFFFFFF) << 2);
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_UPPER),
           ((cp.doorbell_index + 1) & 0xFFFFFF) << 2);

    CP_LOG("HQD programmed: ring_bus=%#llx bufsz=%u doorbell=%u",
           cp.ring_bus, buf_log2, cp.doorbell_index);
    return kIOReturnSuccess;
}

// ----- cp_enable: toggle CP_ME_CNTL.ME_HALT -----
kern_return_t
cp_enable(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t r = SOC15_REG_OFFSET(dev, IPBlock::GC, CPRegs::CP_ME_CNTL);
    uint32_t v = RREG32(dev, r);
    if (enable) v &= ~kCP_ME_CNTL__ME_HALT_MASK;   // halt=0 → running
    else        v |=  kCP_ME_CNTL__ME_HALT_MASK;
    WREG32(dev, r, v);
    CP_LOG("CP ME_HALT %s (CP_ME_CNTL=%#010x)",
           enable ? "cleared (CP running)" : "set (CP halted)", v);
    return kIOReturnSuccess;
}

// ----- cp_kick_doorbell: write wptr to BAR5 doorbell slot -----
//
// On GFX12 the doorbell stride is 8 bytes (one u64 per slot). We
// write the low 32 bits — the dext bypasses the userspace
// IOConnectMapMemory64 BAR5 mapping and pokes the doorbell directly
// through PCIDriverKit's MemoryWrite32 on bar5MemIndex.
kern_return_t
cp_kick_doorbell(const DeviceContext &dev, const CPContext &cp)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (dev.pci == nullptr) return kIOReturnNotAttached;
    *cp.wptr_cpu = cp.wptr;
    const uint64_t off = static_cast<uint64_t>(cp.doorbell_index) * 8;
    dev.pci->MemoryWrite32(dev.bar5MemIndex, off, cp.wptr);
    return kIOReturnSuccess;
}

// ----- cp_submit_eop_test: end-to-end submit + fence wait -----
kern_return_t
cp_submit_eop_test(const DeviceContext &dev, CPContext &cp,
                   uint64_t timeout_us, uint32_t *outFence)
{
    if (outFence) *outFence = 0;
    if (!cp.inited) return kIOReturnNotReady;

    uint32_t fence = cp_emit_eop_fence(cp);
    if (fence == 0) return kIOReturnInternalError;

    kern_return_t r = cp_kick_doorbell(dev, cp);
    if (r != kIOReturnSuccess) return r;

    const uint64_t kStep = 1000;
    uint64_t elapsed = 0;
    while (elapsed < timeout_us) {
        uint64_t observed = *cp.fence_cpu;
        // Linux uses a 32-bit fence in the low half on simple paths.
        if ((observed & 0xFFFFFFFFu) == fence) {
            if (outFence) *outFence = fence;
            CP_LOG("EOP fence %u observed after %llu µs", fence, elapsed);
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed += 1000;
        (void)kStep;
    }
    CP_LOG("EOP fence %u timeout (observed=%llu)", fence, *cp.fence_cpu);
    return kIOReturnTimeout;
}

// ----- cp_init_full: BringupStage::CPInit entry -----
kern_return_t
cp_init_full(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    kern_return_t r = cp_alloc_storage(dev, gmc, cp);
    if (r != kIOReturnSuccess) return r;

    // Pin doorbell index 0 for the GFX ring on first PM4 — the
    // simplest possible assignment. Real driver would allocate from
    // a doorbell ID pool.
    cp.doorbell_index = 0;

    // Skip MMIO programming if IP base isn't resolved (e.g. user
    // hasn't loaded the discovery binary yet). Storage stays staged.
    if (!dev.ip.isResolved(IPBlock::GC)) {
        CP_LOG("CP storage staged; GC IP base unresolved — HQD/CP enable deferred");
        return kIOReturnSuccess;
    }

    // Halt CP while we reprogram registers (safety, matches upstream).
    cp_enable(dev, false);

    // GFX top-level constants (GRBM_CNTL.READ_TIMEOUT, SH_MEM_CONFIG)
    // must be programmed before the CP starts fetching from the ring.
    r = gfx_constants_init(dev);
    if (r != kIOReturnSuccess) {
        CP_LOG("gfx_constants_init failed: %#x", r);
        return r;
    }

    r = cp_hqd_program(dev, cp);
    if (r != kIOReturnSuccess) return r;
    cp_enable(dev, true);

    CP_LOG("CP ready: GFX ring active at doorbell %u", cp.doorbell_index);
    return kIOReturnSuccess;
}

} // namespace amdgpu
