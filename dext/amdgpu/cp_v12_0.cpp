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
// Mirrors gfx_v12_0_cp_gfx_resume (gfx_v12_0.c:2715) line-by-line —
// the writes happen in upstream's exact order. Audit-7 #3 added the
// missing fields: CP_RB_WPTR_DELAY, CP_RB0_WPTR_HI,
// CP_RB_WPTR_POLL_ADDR_{LO,HI}, CP_RB_ACTIVE=1, CP_MAX_CONTEXT,
// CP_DEVICE_ID=1, and the per-pipe GRBM_GFX_CNTL select that picks
// PIPE_ID0 (cp_gfx_switch_pipe). The RB_BUFSZ encoding also follows
// upstream: log2(ring_size_bytes/8), with RB_BLKSZ = BUFSZ - 2.
kern_return_t
cp_hqd_program(const DeviceContext &dev, CPContext &cp)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    auto reg = [&](uint32_t off) {
        return SOC15_REG_OFFSET(dev, IPBlock::GC, off);
    };

    // gfx_v12_0.c:2723 — CP_RB_WPTR_DELAY = 0.
    WREG32(dev, reg(CPRegs::CP_RB_WPTR_DELAY), 0);

    // gfx_v12_0.c:2726 — CP_RB_VMID = 0.
    WREG32(dev, reg(CPRegs::CP_RB_VMID), 0);

    // gfx_v12_0.c:2730 — cp_gfx_switch_pipe(adev, PIPE_ID0): write
    // GRBM_GFX_CNTL.PIPEID = 0. PIPEID field is bits [1:0]; we only
    // change PIPEID and leave other fields at their current values.
    // Use REG_SET_FIELD with the GRBM_GFX_CNTL field defs from
    // amdgpu_mes.h's MES_REG block (single canonical layout for the
    // GRBM select register).
    {
        const uint32_t grbm_reg = reg(GFXRegs::GRBM_GFX_CNTL);
        uint32_t v = RREG32(dev, grbm_reg);
        v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID, 0);
        WREG32(dev, grbm_reg, v);
    }

    // gfx_v12_0.c:2734-2737 — RB_BUFSZ = order_base_2(ring_size/8);
    // RB_BLKSZ = BUFSZ - 2. ring_size is in BYTES upstream; ours
    // tracked in dwords, so ring_size_bytes = ring_size_dwords * 4.
    // order_base_2(N) = ceil(log2(N)).
    auto order_base_2 = [](uint32_t x) -> uint32_t {
        uint32_t r = 0;
        while ((1u << r) < x) r++;
        return r;
    };
    const uint32_t rb_bufsz = order_base_2(cp.ring_size_dwords * 4u / 8u);
    {
        uint32_t tmp = 0;
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ,
                            (rb_bufsz >= 2) ? (rb_bufsz - 2) : 0);
        WREG32(dev, reg(CPRegs::CP_RB0_CNTL), tmp);
    }

    // gfx_v12_0.c:2741-2742 — initialize wptr lo + hi to 0.
    cp.wptr = 0;
    *cp.wptr_cpu = 0;
    *cp.rptr_cpu = 0;
    *cp.fence_cpu = 0;
    WREG32(dev, reg(CPRegs::CP_RB0_WPTR),    0);
    WREG32(dev, reg(CPRegs::CP_RB0_WPTR_HI), 0);

    // gfx_v12_0.c:2746-2748 — RPTR write-back address. The HI write
    // upstream masks to RB_RPTR_ADDR_HI_MASK.
    WREG32(dev, reg(CPRegs::CP_RB0_RPTR_ADDR),
           static_cast<uint32_t>(cp.rptr_gpu_addr & 0xFFFFFFFCu));
    WREG32(dev, reg(CPRegs::CP_RB0_RPTR_ADDR_HI),
           static_cast<uint32_t>(cp.rptr_gpu_addr >> 32) &
           CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);

    // gfx_v12_0.c:2751-2754 — WPTR poll address pair. Required for
    // wptr-poll-driven CP rings; missing in our previous implementation.
    // Audit-7 #3.
    WREG32(dev, reg(CPRegs::CP_RB_WPTR_POLL_ADDR_LO),
           static_cast<uint32_t>(cp.wptr_gpu_addr));
    WREG32(dev, reg(CPRegs::CP_RB_WPTR_POLL_ADDR_HI),
           static_cast<uint32_t>(cp.wptr_gpu_addr >> 32));

    // gfx_v12_0.c:2756 — mdelay(1) before re-writing CP_RB0_CNTL.
    // Upstream literally writes the same value twice with a 1 ms gap
    // — there's a CP-internal latch that requires the redundant write.
    IOSleep(1);
    {
        uint32_t tmp = 0;
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
        tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ,
                            (rb_bufsz >= 2) ? (rb_bufsz - 2) : 0);
        WREG32(dev, reg(CPRegs::CP_RB0_CNTL), tmp);
    }

    // gfx_v12_0.c:2759-2761 — ring base, low + high.
    {
        const uint64_t rb_addr = cp.ring_bus >> 8;
        WREG32(dev, reg(CPRegs::CP_RB0_BASE),
               static_cast<uint32_t>(rb_addr));
        WREG32(dev, reg(CPRegs::CP_RB0_BASE_HI),
               static_cast<uint32_t>(rb_addr >> 32));
    }

    // gfx_v12_0.c:2763 — CP_RB_ACTIVE = 1.  Audit-7 #3.
    WREG32(dev, reg(CPRegs::CP_RB_ACTIVE), 1);

    // gfx_v12_0.c:2690-2712 — cp_gfx_set_doorbell. Doorbell control +
    // range registers. Mirrors upstream's REG_SET_FIELD pattern.
    {
        const uint32_t db_reg = reg(CPRegs::CP_RB_DOORBELL_CONTROL);
        uint32_t v = RREG32(dev, db_reg);
        v = REG_SET_FIELD(v, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET,
                          cp.doorbell_index);
        v = REG_SET_FIELD(v, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
        WREG32(dev, db_reg, v);

        uint32_t lower = 0;
        lower = REG_SET_FIELD(lower, CP_RB_DOORBELL_RANGE_LOWER,
                              DOORBELL_RANGE_LOWER, cp.doorbell_index);
        WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_LOWER), lower);
        // Upstream writes the full mask to RANGE_UPPER — accept any
        // doorbell index in our window.
        WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_UPPER),
               CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);
    }

    // gfx_v12_0.c:2770 — switch to PIPE_ID0 (second switch — the
    // start/stop pattern; upstream brackets cp_gfx_start with two
    // switches even though they're idempotent for PIPE_ID0).
    {
        const uint32_t grbm_reg = reg(GFXRegs::GRBM_GFX_CNTL);
        uint32_t v = RREG32(dev, grbm_reg);
        v = REG_SET_FIELD(v, GRBM_GFX_CNTL, PIPEID, 0);
        WREG32(dev, grbm_reg, v);
    }

    // gfx_v12_0.c:2669-2671 — cp_gfx_start. CP_MAX_CONTEXT and
    // CP_DEVICE_ID. Upstream uses adev->gfx.config.max_hw_contexts - 1;
    // GFX12 hardcodes max_hw_contexts = 8 in gfx_v12_0_gpu_early_init,
    // so the field value is 7. CP_DEVICE_ID = 1 per upstream.
    // Audit-7 #3.
    WREG32(dev, reg(CPRegs::CP_MAX_CONTEXT), 8u - 1u);
    WREG32(dev, reg(CPRegs::CP_DEVICE_ID),  1);

    CP_LOG("HQD programmed: ring_bus=%#llx bufsz=%u doorbell=%u",
           cp.ring_bus, rb_bufsz, cp.doorbell_index);
    return kIOReturnSuccess;
}

// ----- cp_compute_enable: program CP_MEC_RS64_CNTL -----
//
// Direct port of gfx_v12_0_cp_compute_enable (gfx_v12_0.c:2778).
// Brings the MEC out of reset by clearing PIPE{0..3}_RESET and
// MEC_INVALIDATE_ICACHE, then sets PIPE{0..3}_ACTIVE and clears
// MEC_HALT. Disable path inverts all bits.
//
// Audit-7 #2 — without this, MEC pipes stay in reset and any compute
// queue (KCQ / KIQ if not MES-driven) goes nowhere.
kern_return_t
cp_compute_enable(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t r =
        SOC15_REG_OFFSET(dev, IPBlock::GC, CPRegs::CP_MEC_RS64_CNTL);

    // gfx_v12_0.c:2782-2803 — full RMW for every field.
    uint32_t data = RREG32(dev, r);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_INVALIDATE_ICACHE,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE0_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE1_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE2_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE3_RESET,
                         enable ? 0 : 1);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE0_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE1_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE2_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_PIPE3_ACTIVE,
                         enable ? 1 : 0);
    data = REG_SET_FIELD(data, CP_MEC_RS64_CNTL, MEC_HALT,
                         enable ? 0 : 1);
    WREG32(dev, r, data);

    // gfx_v12_0.c:2807 — short settling delay so MEC sees the
    // write before any queue programming follows.
    IOSleep(1);  // upstream uses udelay(50); IOSleep(1) is the
                 // coarsest granularity available in DriverKit.

    CP_LOG("CP_MEC_RS64_CNTL %s: value=%#010x",
           enable ? "enabled (MEC running)" : "disabled (MEC halted)",
           data);
    return kIOReturnSuccess;
}

// ----- cp_set_doorbell_range: program GFX + MEC doorbell windows -----
//
// Direct port of gfx_v12_0_cp_set_doorbell_range (gfx_v12_0.c:2954).
// Both ranges accept any in-window doorbell. We pick conservative
// caller-set bounds: GFX gets [cp.doorbell_index, cp.doorbell_index+1)
// (one slot); MEC (compute) gets the full 0..0xFFFFFFFC window so
// MES-assigned compute doorbells fall inside it.
//
// Audit-7 #4 — without CP_MEC_DOORBELL_RANGE_*, compute queues built
// via MES SET_HW_RESOURCES (or KIQ MAP_QUEUES) will be rejected by
// the CP doorbell aperture filter and never wake up the MEC.
kern_return_t
cp_set_doorbell_range(const DeviceContext &dev, const CPContext &cp,
                      uint32_t mec_first_doorbell,
                      uint32_t mec_last_doorbell)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    auto reg = [&](uint32_t off) {
        return SOC15_REG_OFFSET(dev, IPBlock::GC, off);
    };

    // gfx_v12_0.c:2957-2960 — GFX ring window. Upstream encodes the
    // doorbell index as `(idx * 2) << 2` (GFX12 8-byte doorbell
    // stride × shift-into-DOORBELL_RANGE_LOWER field). Total
    // multiplier: × 8 (byte offset). Same formula for the MEC.
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_LOWER),
           (cp.doorbell_index * 2u) << 2);
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_UPPER),
           ((cp.doorbell_index + 1u) * 2u) << 2);

    // gfx_v12_0.c:2963-2966 — MEC window. Same encoding as GFX
    // (× 8 byte offset).  Audit-7 #4.
    WREG32(dev, reg(CPRegs::CP_MEC_DOORBELL_RANGE_LOWER),
           (mec_first_doorbell * 2u) << 2);
    WREG32(dev, reg(CPRegs::CP_MEC_DOORBELL_RANGE_UPPER),
           (mec_last_doorbell  * 2u) << 2);

    CP_LOG("doorbell ranges: GFX=[%u..%u] MEC=[%u..%u]",
           cp.doorbell_index, cp.doorbell_index + 1u,
           mec_first_doorbell, mec_last_doorbell);
    return kIOReturnSuccess;
}

// ----- cp_enable: toggle CP_ME_CNTL.{ME_HALT,PFP_HALT} -----
//
// Direct port of gfx_v12_0_cp_gfx_enable (gfx_v12_0.c:2332).
// Both PFP and ME halts must drop together — clearing only ME_HALT
// leaves PFP_HALT set, so the PFP never fetches packets into the ME's
// queue and ME_HALT=0 looks active but the ring is dead.
//
// Audit-7 #1 — previously this routine cleared only ME_HALT_MASK,
// which is also at the wrong bit position. Now matches upstream
// REG_SET_FIELD pattern, with bit positions sourced from
// gc_12_0_0_sh_mask.h: PFP_HALT=0x1a, ME_HALT=0x1c.
kern_return_t
cp_enable(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t r = SOC15_REG_OFFSET(dev, IPBlock::GC, CPRegs::CP_ME_CNTL);

    // gfx_v12_0.c:2335-2339 — RMW.
    uint32_t tmp = RREG32(dev, r);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT,  enable ? 0 : 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
    WREG32(dev, r, tmp);

    CP_LOG("CP_ME_CNTL %s (ME_HALT=%u PFP_HALT=%u, value=%#010x)",
           enable ? "running" : "halted",
           enable ? 0u : 1u, enable ? 0u : 1u, tmp);
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
//
// Mirrors gfx_v12_0_cp_resume (gfx_v12_0.c:3484) for the PSP-load
// path, with the legacy-direct-load branches stripped (PSP autoload
// chains the firmware itself).
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

    // gfx_v12_0_cp_resume:3490 — halt + halt-compute first. Matches
    // upstream's idle-before-program pattern (audit-7 #1 and #2).
    cp_enable(dev, false);
    cp_compute_enable(dev, false);

    // GFX top-level constants (GRBM_CNTL.READ_TIMEOUT, SH_MEM_CONFIG)
    // must be programmed before the CP starts fetching from the ring.
    // Upstream calls gfx_v12_0_constants_init in hw_init before
    // cp_resume; we still call it here to keep CPInit self-contained.
    r = gfx_constants_init(dev);
    if (r != kIOReturnSuccess) {
        CP_LOG("gfx_constants_init failed: %#x", r);
        return r;
    }

    // gfx_v12_0_cp_resume:3503 — cp_set_doorbell_range BEFORE
    // KIQ/KCQ/MES resume. We default MEC window to [0x10, 0x100) to
    // cover the MES SCHED + KIQ + KFD-style compute doorbells we
    // hand out later. (cp_v12_0 only owns RB0; MES code owns the
    // compute slots so picks the actual mec doorbell map.)
    r = cp_set_doorbell_range(dev, cp,
                              /*mec_first_doorbell=*/0x10,
                              /*mec_last_doorbell =*/0x100);
    if (r != kIOReturnSuccess) {
        CP_LOG("cp_set_doorbell_range failed: %#x", r);
        return r;
    }

    // Program the GFX HQD (cp_gfx_resume) — upstream gfx_v12_0.c:3522.
    r = cp_hqd_program(dev, cp);
    if (r != kIOReturnSuccess) return r;

    // gfx_v12_0_cp_resume implicitly enables CP via cp_gfx_start
    // (gfx_v12_0.c:2674) which calls cp_gfx_enable. We do the same
    // here: bring ME + PFP out of halt, plus the compute MEC pipes
    // (audit-7 #2).
    cp_enable(dev, true);
    cp_compute_enable(dev, true);

    CP_LOG("CP ready: GFX ring active at doorbell %u, MEC pipes active",
           cp.doorbell_index);
    return kIOReturnSuccess;
}

} // namespace amdgpu
