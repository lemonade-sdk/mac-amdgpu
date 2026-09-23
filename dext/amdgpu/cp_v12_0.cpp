// GFX12 kernel GFX queue allocation, MES mapping and PM4 submission.
// Sources: amdgpu_ring.c and gfx_v12_0.c in the local Linux tree.

#include <os/log.h>
#include <string.h>
#include <time.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>

#include "amdgpu_cp.h"
#include "amdgpu_gfx_mqd.h"
#include "amdgpu_gmc.h"
#include "amdgpu_gfx.h"
#include "amdgpu_mes.h"
#include "amdgpu_vram_io.h"

#define CP_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.cp: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// Linux's ring->gpu_addr is a GPU VM address, not a PCI DMA address.
// Use visible VRAM until system-memory allocations have real GART mappings.
kern_return_t
cp_alloc_storage(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    if (cp.inited) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) return kIOReturnNotReady;
    VRAMAllocation ring{}, wb{};
    if (!gmc.vram_alloc.alloc(kCPRingDefaultBytes, kASPageSize, &ring))
        return kIOReturnNoMemory;
    if (!gmc.vram_alloc.alloc(kCPWBPageBytes, kASPageSize, &wb)) {
        gmc.vram_alloc.free(ring);
        return kIOReturnNoMemory;
    }
    IOBufferMemoryDescriptor *staging = nullptr;
    auto r = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, ring.size, kASPageSize, &staging);
    IOAddressSegment cpu{};
    if (r == kIOReturnSuccess && staging) {
        staging->SetLength(ring.size);
        staging->GetAddressRange(&cpu);
        if (!cpu.address || cpu.length < ring.size) r = kIOReturnNoMemory;
    } else if (r == kIOReturnSuccess) {
        r = kIOReturnNoMemory;
    }
    if (r == kIOReturnSuccess)
        r = vram_clear_verified(dev, ring.gpu_va - gmc.vram_start, ring.size);
    if (r == kIOReturnSuccess)
        r = vram_clear_verified(dev, wb.gpu_va - gmc.vram_start, wb.size);
    if (r != kIOReturnSuccess) {
        if (staging) staging->release();
        gmc.vram_alloc.free(wb);
        gmc.vram_alloc.free(ring);
        return r;
    }
    cp.ring_buf = staging;
    cp.ring_cpu = reinterpret_cast<void *>(cpu.address);
    memset(cp.ring_cpu, 0, ring.size);
    cp.ring_bus = ring.gpu_va;
    cp.ring_vram_off = ring.gpu_va - gmc.vram_start;
    cp.ring_size_dwords = kCPRingDefaultBytes / 4;
    cp.ring_ptr_mask = cp.ring_size_dwords - 1;
    cp.wb_bus = wb.gpu_va;
    cp.wb_vram_off = wb.gpu_va - gmc.vram_start;
    cp.wb_device = &dev;
    cp.fence_shadow = 0;
    cp.fence_cpu = &cp.fence_shadow;
    cp.rptr_gpu_addr = cp.wb_bus + kCPWBOffsetRptr;
    cp.wptr_gpu_addr = cp.wb_bus + kCPWBOffsetWptr;
    cp.fence_gpu_addr = cp.wb_bus + kCPWBOffsetFence;
    cp.wptr = cp.published_wptr = 0;
    cp.fence_counter = 0;
    cp.inited = true;
    CP_LOG("VRAM storage: ring=%#llx (%u dwords) wb=%#llx (rptr=%#llx wptr=%#llx fence=%#llx)",
           cp.ring_bus, cp.ring_size_dwords, cp.wb_bus,
           cp.rptr_gpu_addr, cp.wptr_gpu_addr, cp.fence_gpu_addr);
    return kIOReturnSuccess;
}

void
cp_release_storage(CPContext &cp)
{
    if (cp.ring_buf) cp.ring_buf->release();
    cp = {};
}

kern_return_t
cp_read_fence(const CPContext &cp, uint64_t *value)
{
    if (!cp.inited || !cp.wb_device) return kIOReturnNotReady;
    return vram_read_fence64(*cp.wb_device, cp.wb_vram_off + kCPWBOffsetFence, value);
}

bool
cp_read_cs_fence(void *context, uint64_t *value)
{
    if (!context) return false;
    return cp_read_fence(*static_cast<CPContext *>(context), value) == kIOReturnSuccess;
}

// RPTR is a ring-relative 32-bit offset on GFX12; software WPTR is
// monotonic and 64-bit. Occupancy comparisons must use the ring mask.
static kern_return_t
cp_read_rptr(const CPContext &cp, uint32_t *value)
{
    if (!cp.inited || !cp.wb_device) return kIOReturnNotReady;
    return vram_read_fence32(*cp.wb_device, cp.wb_vram_off + kCPWBOffsetRptr, value);
}

// Stage PM4 on the CPU; commit uploads only new words into GPU VRAM.
uint32_t
cp_ring_write(CPContext &cp, const uint32_t *src, uint32_t dwords)
{
    if (!cp.ringReady || !cp.ring_cpu || src == nullptr || dwords == 0 ||
        cp.ring_size_dwords < 256 || (cp.ring_size_dwords & (cp.ring_size_dwords - 1)) ||
        cp.ring_ptr_mask != cp.ring_size_dwords - 1 ||
        cp.wptr > UINT64_MAX - dwords - 255) return 0;
    uint32_t rptr = 0;
    if (cp_read_rptr(cp, &rptr) != kIOReturnSuccess) return 0;
    const uint32_t pending = (static_cast<uint32_t>(cp.wptr) - rptr) & cp.ring_ptr_mask;
    const uint32_t padding = static_cast<uint32_t>(-(cp.wptr + dwords)) & 0xffu;
    if (cp.wptr < cp.published_wptr ||
        cp.wptr - cp.published_wptr + dwords + padding >= cp.ring_size_dwords ||
        uint64_t(pending) + dwords + padding >= cp.ring_size_dwords) return 0;
    if (dwords > cp.ring_size_dwords / 2) {
        CP_LOG("ring_write: %u dwords exceeds half-ring %u",
               dwords, cp.ring_size_dwords / 2);
        return 0;
    }
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    for (uint32_t i = 0; i < dwords; i++) {
        ring[cp.wptr & cp.ring_ptr_mask] = src[i];
        ++cp.wptr;
    }
    return dwords;
}

// ----- cp_emit_eop_fence: build NOP + RELEASE_MEM -----
//
// Returns the fence value the EOP write will deposit. Caller's
// responsibility to (a) kick the doorbell, (b) read the VRAM fence.
uint32_t
cp_emit_eop_fence(CPContext &cp)
{
    if (!cp.inited || cp.fence_counter == UINT32_MAX) return 0;

    uint32_t pkt[10];
    const uint32_t fence = ++cp.fence_counter;
    const uint32_t n = pm4_build_fence(pkt, cp.fence_gpu_addr, fence,
                                       /*write64=*/true, /*interrupt=*/true);

    if (cp_ring_write(cp, pkt, n) != n) return 0;
    CP_LOG("emitted EOP fence value %u (wptr=%llu)", fence, cp.wptr);
    return fence;
}

// Linux's default async GFX path loads v12_gfx_mqd through MES KIQ.
// Do not program legacy CP_RB0 registers as a second queue owner.
kern_return_t
cp_map_gfx_queue(DeviceContext &dev, GMCContext &gmc, CPContext &cp, MESContext &mes)
{
    if (!cp.inited || !cp.enginesStarted || !mes.uni_mes_active)
        return kIOReturnNotReady;
    if (cp.mqd_bus) return kIOReturnNotReady; // no replay over firmware-owned MQD
    VRAMAllocation allocation{};
    if (!gmc.vram_alloc.alloc(sizeof(GFXQueueDescriptor), kASPageSize, &allocation))
        return kIOReturnNoMemory;
    // Even a timed-out MAP may have reached hardware. Keep this allocation
    // through failure; the existing successful reset releases the GMC arena.
    cp.mqd_bus = allocation.gpu_va;
    GFXQueueDescriptor mqd{};
    if (!gfx_build_kernel_mqd(mqd, cp.mqd_bus, cp.ring_bus,
        cp.rptr_gpu_addr, cp.wptr_gpu_addr, cp.ring_size_dwords * 4, cp.doorbell_index))
        return kIOReturnBadArgument;
    auto r = vram_clear_verified(dev, cp.wb_vram_off, kCPWBPageBytes);
    if (r != kIOReturnSuccess) return r;
    cp.wptr = cp.published_wptr = 0;
    cp.fence_shadow = 0;
    r = vram_write_verified(dev, cp.mqd_bus - gmc.vram_start, &mqd, sizeof(mqd));
    if (r != kIOReturnSuccess) return r;
    amdgpu_hdp_flush(dev);
    r = mes_map_legacy_queue(dev, mes, kMESQueueType_GFX, 0, 0,
        cp.doorbell_index, cp.mqd_bus, cp.wptr_gpu_addr);
    if (r != kIOReturnSuccess) return r;
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
        CPRegs::CP_MAX_CONTEXT.baseIndex, CPRegs::CP_MAX_CONTEXT.offset), 7);
    WREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
        CPRegs::CP_DEVICE_ID.baseIndex, CPRegs::CP_DEVICE_ID.offset), 1);
    CP_LOG("GFX MQD mapped by KIQ: mqd=%#llx ring=%#llx doorbell=%u",
        cp.mqd_bus, cp.ring_bus, cp.doorbell_index);
    return kIOReturnSuccess;
}

// Linux gfx_v12_0_config_gfx_rs64: required on the PSP-load path after
// RLC autoload, before CP resume. Loading bytes alone leaves PFP/ME reset.
kern_return_t
cp_configure_rs64(const DeviceContext &dev, const CPContext &cp)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    for (const auto &fw : cp.firmware)
        if (!fw.loaded || fw.address == 0 || (fw.address & 3)) return kIOReturnNotReady;
    auto reg = [&](CPRegs::Register r) {
        return SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, r.baseIndex, r.offset);
    };
    // Explicitly select ME/pipe/queue/VMID rather than inheriting MES state.
    const uint32_t select = reg(CPRegs::GRBM_GFX_CNTL);
    const CPRegs::Register pcLo[] = {CPRegs::CP_PFP_PRGRM_CNTR_START,
        CPRegs::CP_ME_PRGRM_CNTR_START, CPRegs::CP_MEC_RS64_PRGRM_CNTR_START};
    const CPRegs::Register pcHi[] = {CPRegs::CP_PFP_PRGRM_CNTR_START_HI,
        CPRegs::CP_ME_PRGRM_CNTR_START_HI, CPRegs::CP_MEC_RS64_PRGRM_CNTR_START_HI};
    // Linux configures both graphics pipes and all four MEC pipes, even
    // on ASICs whose firmware loader populates fewer active stack slots.
    constexpr uint32_t resetMasks[] = {0x000c0000u, 0x00300000u, 0x000f0000u};
    for (uint32_t engine = 0; engine < 3; ++engine) {
        const uint64_t entry = cp.firmware[engine].address;
        const uint32_t lo = static_cast<uint32_t>(entry >> 2);
        const uint32_t hi = static_cast<uint32_t>(entry >> 34);
        const uint32_t pipes = engine == 2 ? 4 : 2;
        for (uint32_t pipe = 0; pipe < pipes; ++pipe) {
            uint32_t selection = 0;
            selection = REG_SET_FIELD(selection, GRBM_GFX_CNTL, MEID, engine == 2 ? 1 : 0);
            selection = REG_SET_FIELD(selection, GRBM_GFX_CNTL, PIPEID, pipe);
            WREG32(dev, select, selection);
            WREG32(dev, reg(pcLo[engine]), lo);
            WREG32(dev, reg(pcHi[engine]), hi);
            // gfx12.0.0/1 has one graphics pipe and two MEC pipes
            // (gfx_v12_0_sw_init). Linux still writes all slots above, but does
            // not read them back. The unused
            // graphics slot returns 0xDEADBEEF on R9700; validate only active
            // slots, retaining the upstream write/reset sequence for all slots.
            if (pipe >= (engine == 2 ? 2u : 1u)) continue;
            const uint32_t readLo = RREG32(dev, reg(pcLo[engine]));
            const uint32_t readHi = RREG32(dev, reg(pcHi[engine]));
            if (readLo == UINT32_MAX && readHi == UINT32_MAX) return kIOReturnNotAttached;
            if (readLo != lo || readHi != hi) {
                WREG32(dev, select, 0);
                CP_LOG("RS64 PC readback failed: engine=%u pipe=%u expected=%#x:%#x got=%#x:%#x",
                       engine, pipe, hi, lo, readHi, readLo);
                return kIOReturnIOError;
            }
        }
        WREG32(dev, select, 0);
        const uint32_t control = reg(engine == 2 ? CPRegs::CP_MEC_RS64_CNTL : CPRegs::CP_ME_CNTL);
        const uint32_t before = RREG32(dev, control);
        if (before == UINT32_MAX) return kIOReturnNotAttached;
        WREG32(dev, control, before | resetMasks[engine]);
        WREG32(dev, control, before & ~resetMasks[engine]);
        const uint32_t after = RREG32(dev, control);
        if (after == UINT32_MAX) return kIOReturnNotAttached;
        if (after & resetMasks[engine]) return kIOReturnIOError;
        CP_LOG("RS64 configured: engine=%u entry=%#llx pc=%#x:%#x control=%#x -> %#x",
               engine, entry, hi, lo, before, after);
    }
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
        SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
            CPRegs::CP_MEC_RS64_CNTL.baseIndex, CPRegs::CP_MEC_RS64_CNTL.offset);

    // gfx_v12_0.c:2782-2803 — full RMW for every field.
    uint32_t data = RREG32(dev, r);
    if (data == UINT32_MAX) return kIOReturnNotAttached;
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
    const uint32_t check = RREG32(dev, r);
    if (check == UINT32_MAX) return kIOReturnNotAttached;
    if ((check & CP_MEC_RS64_CNTL__MEC_HALT_MASK) !=
        (data & CP_MEC_RS64_CNTL__MEC_HALT_MASK)) return kIOReturnIOError;

    // gfx_v12_0.c:2807 — short settling delay so MEC sees the
    // write before any queue programming follows.
    IOSleep(1);  // upstream uses udelay(50); IOSleep(1) is the
                 // coarsest granularity available in DriverKit.

    CP_LOG("CP_MEC_RS64_CNTL %s: value=%#010x",
           enable ? "enabled (MEC running)" : "disabled (MEC halted)",
           data);
    return kIOReturnSuccess;
}

// Program GFX and MEC doorbell windows. CPContext uses DWORD indices;
// the ASIC MEC window arguments use QWORD slots, converted to byte offsets.
kern_return_t
cp_set_doorbell_range(const DeviceContext &dev, const CPContext &cp,
                      uint32_t mec_first_doorbell,
                      uint32_t mec_last_doorbell)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    auto reg = [&](CPRegs::Register r) {
        return SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, r.baseIndex, r.offset);
    };

    // DWORD doorbell indices become byte offsets in range registers.
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_LOWER),
           cp.doorbell_index << 2);
    WREG32(dev, reg(CPRegs::CP_RB_DOORBELL_RANGE_UPPER),
           (cp.doorbell_index + 2u) << 2);

    // gfx_v12_0.c:2963-2966 — MEC window. Same encoding as GFX
    // (× 8 byte offset).  Audit-7 #4.
    WREG32(dev, reg(CPRegs::CP_MEC_DOORBELL_RANGE_LOWER),
           (mec_first_doorbell * 2u) << 2);
    WREG32(dev, reg(CPRegs::CP_MEC_DOORBELL_RANGE_UPPER),
           (mec_last_doorbell  * 2u) << 2);

    CP_LOG("doorbell ranges: GFX=[%u..%u] MEC=[%u..%u]",
           cp.doorbell_index, cp.doorbell_index + 2u,
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
    const uint32_t r = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
        CPRegs::CP_ME_CNTL.baseIndex, CPRegs::CP_ME_CNTL.offset);

    // gfx_v12_0.c:2335-2339 — RMW.
    uint32_t tmp = RREG32(dev, r);
    if (tmp == UINT32_MAX) return kIOReturnNotAttached;
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT,  enable ? 0 : 1);
    tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
    WREG32(dev, r, tmp);
    const uint32_t check = RREG32(dev, r);
    if (check == UINT32_MAX) return kIOReturnNotAttached;
    constexpr uint32_t haltMask = CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK;
    if ((check & haltMask) != (tmp & haltMask)) return kIOReturnIOError;
    // gfx_v12_0_cp_gfx_enable waits for CP_STAT to acknowledge idle.
    const uint32_t statReg = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
        CPRegs::CP_STAT.baseIndex, CPRegs::CP_STAT.offset);
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint32_t stat;
    while ((stat = RREG32(dev, statReg)) != 0) {
        if (stat == UINT32_MAX) return kIOReturnNotAttached;
        if ((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) >= 1000000000ull) {
            CP_LOG("CP %s timed out: CP_STAT=%#x", enable ? "enable" : "halt", stat);
            return kIOReturnTimeout;
        }
        IOSleep(1);
    }

    CP_LOG("CP_ME_CNTL %s (ME_HALT=%u PFP_HALT=%u, value=%#010x)",
           enable ? "running" : "halted",
           enable ? 0u : 1u, enable ? 0u : 1u, tmp);
    return kIOReturnSuccess;
}

// Commit the ring as in amdgpu_ring_commit/gfx_v12_0_ring_set_wptr_gfx.
// Hardware pointers count DWORDs; BAR2 offsets use DWORD doorbell indices.
kern_return_t
cp_kick_doorbell(const DeviceContext &dev, CPContext &cp)
{
    if (!cp.inited) return kIOReturnNotReady;
    if (dev.pci == nullptr) return kIOReturnNotAttached;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    if (!cp.ringReady || !cp.ring_cpu || !cp.wb_device)
        return kIOReturnNotReady;
    if (cp.ring_size_dwords < 256 || (cp.ring_size_dwords & (cp.ring_size_dwords - 1)) ||
        cp.ring_ptr_mask != cp.ring_size_dwords - 1 ||
        cp.wptr < cp.published_wptr || cp.wptr > UINT64_MAX - 255)
        return kIOReturnBadArgument;
    const uint64_t db_off = uint64_t(cp.doorbell_index) * 4;
    if (db_off > dev.bar2Size || 8 > dev.bar2Size - db_off)
        return kIOReturnBadArgument;
    uint32_t rptr = 0;
    auto r = cp_read_rptr(cp, &rptr);
    if (r != kIOReturnSuccess) return r;
    // amdgpu_ring_commit pads GFX12 submissions to 256 DWORDs.
    const uint32_t padding = static_cast<uint32_t>(-cp.wptr) & 0xffu;
    const uint32_t pending = (static_cast<uint32_t>(cp.wptr) - rptr) & cp.ring_ptr_mask;
    if (uint64_t(pending) + padding >= cp.ring_size_dwords ||
        cp.wptr - cp.published_wptr + padding >= cp.ring_size_dwords)
        return kIOReturnNoSpace;
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    for (uint32_t i = 0; i < padding; ++i)
        ring[cp.wptr++ & cp.ring_ptr_mask] = pm4_header(kPM4OpNop, 0x3fff);
    // Read back every new packet word, including wraparound, before making
    // the WPTR visible. Never upload the WB page over GPU-owned completions.
    uint64_t cursor = cp.published_wptr;
    while (cursor < cp.wptr) {
        const uint32_t index = uint32_t(cursor) & cp.ring_ptr_mask;
        const uint64_t remaining = cp.wptr - cursor;
        const uint64_t contiguous = cp.ring_size_dwords - index;
        const uint64_t count = remaining < contiguous ? remaining : contiguous;
        r = vram_write_verified(dev, cp.ring_vram_off + uint64_t(index) * 4,
                                ring + index, count * 4);
        if (r != kIOReturnSuccess) return r;
        cursor += count;
    }
    const uint64_t v = cp.wptr;
    r = vram_write_verified(dev, cp.wb_vram_off + kCPWBOffsetWptr, &v, sizeof(v));
    if (r != kIOReturnSuccess) return r;
    amdgpu_hdp_flush(dev);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    cp.published_wptr = cp.wptr;
    dev.pci->MemoryWrite64(dev.bar2MemIndex, db_off, v);

    // A MES-mapped queue is notified through its assigned doorbell, as in
    // gfx_v12_0_ring_set_wptr_gfx. Legacy MMIO WPTR writes can target a
    // different/unmapped queue; never publish the same work through both.
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

    const uint64_t start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t elapsed = 0, observed = 0;
    while (elapsed < timeout_us) {
        r = cp_read_fence(cp, &observed);
        if (r != kIOReturnSuccess) return r;
        // Linux uses a 32-bit fence in the low half on simple paths.
        if ((observed & 0xFFFFFFFFu) == fence) {
            if (outFence) *outFence = fence;
            CP_LOG("EOP fence %u observed after %llu µs", fence, elapsed);
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start_ns) / 1000;
    }
    CP_LOG("EOP fence %u timeout (observed=%llu)", fence, observed);
    return kIOReturnTimeout;
}

// Match Linux gfx_v12_0_ring_test_ring before requiring RELEASE_MEM.
// SET_UCONFIG_REG writes a register, so this isolates command fetch/dispatch
// from the event and GPU-memory completion path used by the fence test.
static kern_return_t
cp_scratch_test(const DeviceContext &dev, CPContext &cp, uint64_t timeout_us,
                uint64_t *outElapsed, uint32_t *outObserved)
{
    if (outElapsed) *outElapsed = 0;
    if (outObserved) *outObserved = 0;
    if (!cp.ringReady || !dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t scratch = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
        CPRegs::SCRATCH_REG0.baseIndex, CPRegs::SCRATCH_REG0.offset);
    if (scratch < kPM4UconfigStart || scratch >= kPM4UconfigEnd)
        return kIOReturnBadArgument;
    constexpr uint32_t poison = 0xcafedead, expected = 0xdeadbeef;
    WREG32(dev, scratch, poison);
    const uint32_t initial = RREG32(dev, scratch);
    if (initial == UINT32_MAX) return kIOReturnNotAttached;
    if (initial != poison) return kIOReturnIOError;
    const uint32_t packet[] = {
        pm4_header(kPM4OpSetUconfigReg, 1), scratch - kPM4UconfigStart, expected
    };
    cp_log_control(dev, "before scratch submission");
    if (cp_ring_write(cp, packet, 3) != 3) return kIOReturnNoSpace;
    const uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    auto r = cp_kick_doorbell(dev, cp);
    if (r != kIOReturnSuccess) return r;
    for (;;) {
        const uint32_t observed = RREG32(dev, scratch);
        const uint64_t elapsed = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1000;
        if (outElapsed) *outElapsed = elapsed;
        if (outObserved) *outObserved = observed;
        if (observed == UINT32_MAX) return kIOReturnNotAttached;
        if (observed == expected) {
            CP_LOG("GFX scratch test passed: reg=%#x value=%#x elapsed=%llu us",
                   scratch, observed, elapsed);
            cp_log_control(dev, "after scratch completion");
            return kIOReturnSuccess;
        }
        if (elapsed >= timeout_us) {
            CP_LOG("GFX scratch test timed out: reg=%#x expected=%#x observed=%#x elapsed=%llu us",
                   scratch, expected, observed, elapsed);
            cp_log_control(dev, "scratch timeout");
            return kIOReturnTimeout;
        }
        IOSleep(1);
    }
}

// Kernel GFX PM4 test. The exported name remains for selector compatibility.
kern_return_t
cp_kiq_smoke_test(DeviceContext &dev,
                  CPContext &cp,
                  MESContext &mes,
                  GMCContext &gmc,
                  uint32_t expected_fence_value,
                  uint32_t timeout_us,
                  uint64_t *out_elapsed_us,
                  uint64_t *out_fence_gpu_va,
                  uint32_t *out_observed_fence)
{
    if (out_elapsed_us)     *out_elapsed_us     = 0;
    if (out_fence_gpu_va)   *out_fence_gpu_va   = 0;
    if (out_observed_fence) *out_observed_fence = 0;

    (void)mes;
    if (!cp.ringReady) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    if (expected_fence_value == 0xCAFEBABE || expected_fence_value == UINT32_MAX)
        return kIOReturnBadArgument;

    const auto scratchResult = cp_scratch_test(dev, cp, timeout_us,
                                               out_elapsed_us, out_observed_fence);
    if (scratchResult != kIOReturnSuccess) return scratchResult;
    cp_log_control(dev, "before GFX fence submission");
    CP_LOG("cp_kiq_smoke: starting (expected=%#x, timeout=%u us)",
           expected_fence_value, timeout_us);

    // Allocate a distinct poisoned fence target inside visible VRAM.
    // Retain it after a failed submission until the session reset.
    VRAMAllocation fence_alloc{};
    if (!gmc.vram_alloc.alloc(64, kASPageSize, &fence_alloc)) {
        CP_LOG("cp_kiq_smoke: VRAM fence alloc failed");
        return kIOReturnNoMemory;
    }
    const uint64_t fence_gpu_va    = fence_alloc.gpu_va;
    const uint64_t fence_vram_off  = fence_gpu_va - gmc.vram_start;
    if (out_fence_gpu_va) *out_fence_gpu_va = fence_gpu_va;
    CP_LOG("cp_kiq_smoke: fence_target @ gpu_va=%#llx vram_off=%#llx",
           (unsigned long long)fence_gpu_va,
           (unsigned long long)fence_vram_off);

    // (3) Pre-fill the fence dword with 0xCAFEBABE so a "no write"
    // outcome is distinguishable from accidental zero.
    const uint32_t poison = 0xCAFEBABE;
    auto upload = vram_write_verified(dev, fence_vram_off, &poison, sizeof(poison));
    if (upload != kIOReturnSuccess) {
        gmc.vram_alloc.free(fence_alloc);
        return upload;
    }
    // HDP flush so the GPU sees the pre-fill (paranoid — RELEASE_MEM
    // overwrites it anyway, but keeps the readback clean).
    amdgpu_hdp_flush(dev);

    // (4) Use the same GFX12 encoding as the normal EOP fence path.
    uint32_t pkt[10];
    const uint32_t n = pm4_build_fence(pkt, fence_gpu_va,
                                       expected_fence_value,
                                       /*write64=*/false, /*interrupt=*/false);

    const uint64_t start_wptr = cp.wptr;
    if (cp_ring_write(cp, pkt, n) != n) {
        gmc.vram_alloc.free(fence_alloc);
        return kIOReturnNoSpace;
    }
    CP_LOG("GFX queue: %u PM4 dwords, wptr=%llu -> %llu", n, start_wptr, cp.wptr);
    const uint64_t start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    // Publish verified ring words and WPTR shadow, then notify its doorbell.
    {
        kern_return_t kr = cp_kick_doorbell(dev, cp);
        if (kr != kIOReturnSuccess) {
            CP_LOG("cp_kiq_smoke: cp_kick_doorbell failed: %#x", kr);
            return kr;
        }
    }
    CP_LOG("cp_kiq_smoke: doorbell rung (slot=%#x new_wptr=%llu doorbell_works=%d)",
           cp.doorbell_index, cp.wptr, dev.doorbell_works ? 1 : 0);

    uint64_t elapsed_us = 0;
    uint32_t observed = 0;
    for (;;) {
        auto readResult = vram_read_fence32(dev, fence_vram_off, &observed);
        if (readResult != kIOReturnSuccess) return readResult;
        elapsed_us = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start_ns) / 1000;
        if (observed == expected_fence_value) {
            if (out_observed_fence) *out_observed_fence = observed;
            if (out_elapsed_us) *out_elapsed_us = elapsed_us;
            CP_LOG("GFX queue fence expected=%#x observed=%#x in %llu us",
                   expected_fence_value, observed, elapsed_us);
            gmc.vram_alloc.free(fence_alloc);
            return kIOReturnSuccess;
        }
        if (elapsed_us >= timeout_us) break;
        IOSleep(1);
    }
    auto read = [&](CPRegs::Register r) {
        return RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, r.baseIndex, r.offset));
    };
    uint32_t wb_rptr = UINT32_MAX;
    (void)cp_read_rptr(cp, &wb_rptr);
    CP_LOG("GFX HQD timeout: RPTR=%#x WPTR=%#x:%#x WB_RPTR=%#x CP_STAT=%#x ME_CNTL=%#x",
           read(CPRegs::CP_GFX_HQD_RPTR), read(CPRegs::CP_GFX_HQD_WPTR_HI),
           read(CPRegs::CP_GFX_HQD_WPTR), wb_rptr, read(CPRegs::CP_STAT), read(CPRegs::CP_ME_CNTL));

    CP_LOG("GFXHUB fault snapshot: status=%#x:%08x address=%#x:%08x",
           read(CPRegs::GCVM_L2_PROTECTION_FAULT_STATUS_HI32),
           read(CPRegs::GCVM_L2_PROTECTION_FAULT_STATUS_LO32),
           read(CPRegs::GCVM_L2_PROTECTION_FAULT_ADDR_HI32),
           read(CPRegs::GCVM_L2_PROTECTION_FAULT_ADDR_LO32));
    if (out_observed_fence) *out_observed_fence = observed;
    if (out_elapsed_us)     *out_elapsed_us     = elapsed_us;
    CP_LOG("cp_kiq_smoke: fence wait TIMEOUT expected=%#x observed=%#x "
           "after %llu us",
           expected_fence_value, observed, elapsed_us);
    return kIOReturnTimeout;
}

// ----- CP preparation (stage 12) and queue resume (stage 14) -----
//
// Mirrors gfx_v12_0_cp_resume (gfx_v12_0.c:3484) for the PSP-load
// path, with the legacy-direct-load branches stripped (PSP autoload
// chains the firmware itself).
kern_return_t
cp_prepare_firmware(DeviceContext &dev, GMCContext &gmc, CPContext &cp)
{
    if (cp.firmwarePrepared) return kIOReturnSuccess;
    kern_return_t r = cp_alloc_storage(dev, gmc, cp);
    if (r != kIOReturnSuccess) return r;

    // Pin doorbell index for the GFX ring from the doorbell_index map.
    // gfx_ring0 = 0 for RDNA4 (gfx1201). Real driver would allocate
    // from a doorbell ID pool.
    cp.doorbell_index = dev.doorbell.index.gfx_ring0 << 1;

    // Skip MMIO programming if IP base isn't resolved (e.g. user
    // hasn't loaded the discovery binary yet). Storage stays staged.
    if (!dev.ip.isResolved(IPBlock::GC)) {
        CP_LOG("CP storage staged; GC IP base unresolved — HQD/CP enable deferred");
        return kIOReturnNotReady;
    }

    // Quiesce the legacy ring before replacing its backing addresses.
    // This is our initialization guard, not Linux's PSP-load resume sequence.
    r = cp_enable(dev, false);
    if (r != kIOReturnSuccess) return r;
    r = cp_compute_enable(dev, false);
    if (r != kIOReturnSuccess) return r;

    // RLCInit already acknowledged autoload. Configure the firmware PCs and
    // latch them through pipe reset, as Linux's PSP hw_init requires.
    r = cp_configure_rs64(dev, cp);
    if (r != kIOReturnSuccess) return r;

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

    cp.firmwarePrepared = true;
    cp_log_control(dev, "prepared; queue remains halted");
    return kIOReturnSuccess;
}

void
cp_log_control(const DeviceContext &dev, const char *phase)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return;
    auto read = [&](CPRegs::Register r) {
        return RREG32(dev, SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, r.baseIndex, r.offset));
    };
    CP_LOG("%{public}s: ME_CNTL=%#x MEC_CNTL=%#x CP_STAT=%#x",
           phase, read(CPRegs::CP_ME_CNTL), read(CPRegs::CP_MEC_RS64_CNTL), read(CPRegs::CP_STAT));
    const uint32_t faultLo = read(CPRegs::GCVM_L2_PROTECTION_FAULT_STATUS_LO32);
    CP_LOG("%{public}s: GFXHUB fault=%#x:%08x addr=%#x:%08x cid=%u vmid=%u rw=%u more=%u",
           phase, read(CPRegs::GCVM_L2_PROTECTION_FAULT_STATUS_HI32), faultLo,
           read(CPRegs::GCVM_L2_PROTECTION_FAULT_ADDR_HI32),
           read(CPRegs::GCVM_L2_PROTECTION_FAULT_ADDR_LO32),
           (faultLo >> 9) & 0x1ffu, (faultLo >> 20) & 0xfu,
           (faultLo >> 18) & 1u, faultLo & 1u);
    CP_LOG("%{public}s: RB_BASE=%#x:%08x RPTR=%#x WPTR=%#x:%08x VMID=%#x "
           "PFP_PC=%#x ME_PC=%#x CPC_STATUS=%#x",
           phase, read(CPRegs::CP_RB0_BASE_HI), read(CPRegs::CP_RB0_BASE),
           read(CPRegs::CP_RB0_RPTR), read(CPRegs::CP_RB0_WPTR_HI),
           read(CPRegs::CP_RB0_WPTR), read(CPRegs::CP_RB_VMID),
           read(CPRegs::CP_PFP_INSTR_PNTR), read(CPRegs::CP_ME_INSTR_PNTR),
           read(CPRegs::CP_CPC_STATUS));
    CP_LOG("%{public}s: GFX_HQD base=%#x:%08x active=%#x mapped=%#x vmid=%#x RPTR=%#x WPTR=%#x:%08x RS64_PC0=%#x PC1=%#x",
        phase, read(CPRegs::CP_GFX_HQD_BASE_HI), read(CPRegs::CP_GFX_HQD_BASE),
        read(CPRegs::CP_GFX_HQD_ACTIVE), read(CPRegs::CP_GFX_HQD_MAPPED),
        read(CPRegs::CP_GFX_HQD_VMID), read(CPRegs::CP_GFX_HQD_RPTR),
        read(CPRegs::CP_GFX_HQD_WPTR_HI), read(CPRegs::CP_GFX_HQD_WPTR),
        read(CPRegs::CP_GFX_RS64_INSTR_PNTR0), read(CPRegs::CP_GFX_RS64_INSTR_PNTR1));

}

// Linux async CP resume enables MEC/GFX before the MES KIQ bootstrap.
// Firmware, GFXHUB, constants and RLC must already be initialized.
kern_return_t
cp_start_engines(const DeviceContext &dev, CPContext &cp)
{
    if (!cp.firmwarePrepared) return kIOReturnNotReady;
    if (cp.enginesStarted) return kIOReturnSuccess;
    auto r = cp_compute_enable(dev, true);
    if (r != kIOReturnSuccess) return r;
    r = cp_enable(dev, true);
    if (r != kIOReturnSuccess) return r;
    cp.enginesStarted = true;
    cp_log_control(dev, "async engines enabled before MES");
    return kIOReturnSuccess;
}

kern_return_t
cp_init_full(DeviceContext &dev, GMCContext &gmc, CPContext &cp, MESContext &mes)
{
    if (cp.ringReady) return kIOReturnSuccess;
    if (!cp.firmwarePrepared || !cp.enginesStarted) return kIOReturnNotReady;
    cp_log_control(dev, "before GFX MQD mapping");
    const auto r = cp_map_gfx_queue(dev, gmc, cp, mes);
    cp_log_control(dev, "after GFX MQD mapping");
    if (r != kIOReturnSuccess) return r;
    cp.ringReady = true;
    CP_LOG("CP ready: MES-mapped GFX queue at doorbell %u", cp.doorbell_index);
    return kIOReturnSuccess;
}

} // namespace amdgpu
