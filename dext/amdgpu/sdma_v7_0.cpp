//
//  sdma_v7_0.cpp — SDMA v7_1 ring bringup for RDNA4 R9700 / gfx12.1.0.
//
//  File name kept as sdma_v7_0.cpp for git history; the implementation
//  actually mirrors sdma_v7_1.c (IP_VERSION(7,0,1)). All register
//  offsets and field shifts come from gc_12_1_0_offset.h /
//  gc_12_1_0_sh_mask.h via amdgpu_sdma.h.
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c
//          (sdma_v7_1_gfx_resume_instance, sdma_v7_1_inst_enable,
//           sdma_v7_1_inst_gfx_stop, sdma_v7_1_get_reg_offset)
//

#include <os/log.h>
#include <string.h>
#include <time.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>

#include "amdgpu_sdma.h"
#include "amdgpu_psp.h"
#include "amdgpu_gmc.h"
#include "amdgpu_vram_io.h"

#define SDMA_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.sdma: " fmt, ##__VA_ARGS__)

namespace amdgpu {

//------------------------------------------------------------------
// sdma_alloc_storage — ring + WB page for one instance.
//------------------------------------------------------------------
kern_return_t
sdma_alloc_storage(DeviceContext &dev, SDMAInstance &inst, GMCContext &gmc)
{
    if (inst.inited) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) {
        SDMA_LOG("instance %u: vram_alloc not ready", inst.instance);
        return kIOReturnNotReady;
    }
    // Keep command fetch and write-back in GPU-addressable VRAM. PCI DMA
    // addresses require a separate GART mapping before GPU VM use.
    VRAMAllocation ring_alloc{};
    if (!gmc.vram_alloc.alloc(kSDMARingDefaultBytes, kASPageSize, &ring_alloc)) {
        SDMA_LOG("instance %u: ring VRAM alloc failed (need %u, free=%llu)",
                 inst.instance, kSDMARingDefaultBytes,
                 (unsigned long long)gmc.vram_alloc.bytes_free());
        return kIOReturnNoMemory;
    }
    inst.ring_gpu_va     = ring_alloc.gpu_va;
    inst.ring_vram_off   = ring_alloc.gpu_va - gmc.vram_start;  // BAR0 offset
    inst.ring_size_dwords = kSDMARingDefaultBytes / 4;
    inst.ring_ptr_mask   = inst.ring_size_dwords - 1;
    VRAMAllocation wb_alloc{};
    if (!gmc.vram_alloc.alloc(kSDMAWBPageBytes, kASPageSize, &wb_alloc)) {
        gmc.vram_alloc.free(ring_alloc);
        return kIOReturnNoMemory;
    }
    inst.wb_bus = wb_alloc.gpu_va;
    inst.wb_vram_off = wb_alloc.gpu_va - gmc.vram_start;
    auto r = vram_clear_verified(dev, inst.ring_vram_off, ring_alloc.size);
    if (r == kIOReturnSuccess)
        r = vram_clear_verified(dev, inst.wb_vram_off, wb_alloc.size);
    if (r != kIOReturnSuccess) {
        gmc.vram_alloc.free(wb_alloc);
        gmc.vram_alloc.free(ring_alloc);
        return r;
    }
    inst.wb_device = &dev;
    inst.cs_fence_shadow = 0;
    inst.rptr_gpu_addr = inst.wb_bus;
    inst.wptr_poll_gpu_addr = inst.wb_bus + 0x40;

    inst.wptr           = 0;
    // Doorbell index — DWORD offset into the doorbell BAR (BAR2).
    // SOC21 layout (nv.c:580-583, amdgpu_doorbell.h:210-211):
    //     adev->doorbell_index.sdma_engine[0] = 0x100
    //     adev->doorbell_index.sdma_engine[1] = 0x10A
    // sdma_v7_1.c:1329-1330 assigns
    //     ring->doorbell_index = adev->doorbell_index.sdma_engine[i] << 1;
    // which is the DWORD offset written into the SDMA_QUEUE0_DOORBELL_OFFSET
    // register and consumed by the engine to filter doorbell traffic.
    // We shift by 1 here to match the upstream pattern.
    inst.doorbell_index = (dev.doorbell.index.sdma_engine[inst.instance] << 1);
    inst.inited         = true;

    SDMA_LOG("instance %u: ring %u dwords @ bus %#llx, "
             "wb @ bus %#llx, doorbell slot %#x",
             inst.instance, inst.ring_size_dwords,
             (unsigned long long)inst.ring_gpu_va,
             (unsigned long long)inst.wb_bus,
             inst.doorbell_index);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_engine_halt — write SDMA0_SDMA_MCU_CNTL.HALT.
//------------------------------------------------------------------
kern_return_t
sdma_engine_halt(const DeviceContext &dev, uint32_t instance, bool halt)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("engine_halt: GC IP base not resolved");
        return kIOReturnNotReady;
    }
    const uint32_t reg = sdma_reg_offset(dev, instance, sdma_regs(dev).MCU_CNTL);
    uint32_t v = RREG32(dev, reg);
    v = REG_SET_FIELD(v, SDMA0_SDMA_MCU_CNTL, HALT, halt ? 1u : 0u);
    v = REG_SET_FIELD(v, SDMA0_SDMA_MCU_CNTL, RESET, 0u);
    WREG32(dev, reg, v);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_gfx_stop_instance — clear RB_ENABLE + IB_ENABLE on QUEUE0.
// Mirrors the per-instance body of sdma_v7_0_gfx_stop.
//------------------------------------------------------------------
kern_return_t
sdma_gfx_stop_instance(const DeviceContext &dev, uint32_t instance)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t rb_cntl_reg =
        sdma_reg_offset(dev, instance, sdma_regs(dev).QUEUE0_RB_CNTL);
    const uint32_t ib_cntl_reg =
        sdma_reg_offset(dev, instance, sdma_regs(dev).QUEUE0_IB_CNTL);

    uint32_t rb_cntl = RREG32(dev, rb_cntl_reg);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    WREG32(dev, rb_cntl_reg, rb_cntl);

    uint32_t ib_cntl = RREG32(dev, ib_cntl_reg);
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_SDMA_QUEUE0_IB_CNTL, IB_ENABLE, 0);
    WREG32(dev, ib_cntl_reg, ib_cntl);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// order_base_2 — log2 of a power-of-two. SDMA RB_SIZE expects the
// ring size as log2(dwords).
//------------------------------------------------------------------
static inline uint32_t order_base_2(uint32_t x)
{
    uint32_t r = 0;
    while ((1u << r) < x) r++;
    return r;
}

//------------------------------------------------------------------
// sdma_gfx_resume_instance — port of sdma_v7_1_gfx_resume_instance
// (sdma_v7_1.c:456-604, restore=false branch).
//
// Programs all the QUEUE0 registers (RB_CNTL/BASE/WPTR/RPTR/DOORBELL),
// unhalts the engine via MCU_CNTL, enables the ring + IB queue.
// Skipped from upstream:
//   • SR-IOV branches (we never run SR-IOV)
//   • __BIG_ENDIAN swap-enable fields (Apple Silicon is LE)
//   • amdgpu_ring_test_helper (we have our own sdma_ring_test)
//   • nbio.funcs->sdma_doorbell_range (set elsewhere when we wire
//     up the doorbell aperture; not needed for first NOP test)
//------------------------------------------------------------------
kern_return_t
sdma_gfx_resume_instance(const DeviceContext &dev, SDMAInstance &inst)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    const uint32_t i = inst.instance;
    auto reg = [&](uint32_t r) { return sdma_reg_offset(dev, i, r); };

    // Mirrors upstream sdma_v7_0_gfx_resume_instance (sdma_v7_1.c:456-604).
    // Each step logs the register name + value so the dext log reads
    // like Linux dyndbg=+p amdgpu when bringup runs.
    SDMA_LOG("SDMA%u gfx_resume: starting (ring_gpu_va=%#llx, rb_size=%u dwords, "
             "rptr_addr=%#llx, doorbell_slot=%#x)",
             i, (unsigned long long)inst.ring_gpu_va,
             inst.ring_size_dwords,
             (unsigned long long)inst.rptr_gpu_addr,
             inst.doorbell_index);

    // 1) Initial RB_CNTL — set RB_SIZE, set RB_PRIV.
    uint32_t rb_bufsz = order_base_2(inst.ring_size_dwords);
    uint32_t rb_cntl = RREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_CNTL));
    SDMA_LOG("SDMA%u  RB_CNTL initial = %#010x (programming RB_SIZE=%u, RB_PRIV=1)",
             i, rb_cntl, rb_bufsz);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_SIZE, rb_bufsz);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_PRIV, 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_CNTL), rb_cntl);

    // 2) Reset RPTR/WPTR.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR),    0);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR_HI), 0);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR),    0);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_HI), 0);

    // 3) WPTR poll address (shadow in WB page; engine doesn't use
    //    when WPTR_POLL_ENABLE=0, but we still program it).
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_POLL_ADDR_LO),
           static_cast<uint32_t>(inst.wptr_poll_gpu_addr));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_POLL_ADDR_HI),
           static_cast<uint32_t>(inst.wptr_poll_gpu_addr >> 32));

    // 4) RPTR write-back address — engine deposits read-pointer here.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR_ADDR_HI),
           static_cast<uint32_t>(inst.rptr_gpu_addr >> 32));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_RPTR_ADDR_LO),
           static_cast<uint32_t>(inst.rptr_gpu_addr & 0xFFFFFFFC));

    // 5) Enable RPTR writeback + MCU_WPTR_POLL.
    //
    // WPTR_POLL_ENABLE — only enabled on SR-IOV (upstream line 526).
    // Bare-metal sets it to 0 (matches us); changing write-back placement
    // does not require enabling the SR-IOV polling mode.
    //
    // MCU_WPTR_POLL_ENABLE — the SDMA microcontroller (MCU) uses this
    // to watch for WPTR updates. Upstream sets this to 1 UNCONDITIONALLY
    // (line 530), bare-metal AND SR-IOV. **Earlier we incorrectly set it
    // to 0 thinking it was a sysmem-polling enable; it is not — it gates
    // the MCU's ability to react to doorbell-delivered WPTR updates.**
    // Without it, the doorbell aperture write arrives at the engine,
    // but the MCU never picks up the new WPTR → RB_WPTR register stays
    // at 0 and no packets are processed. (v0.1.33-v0.1.37 symptom.)
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL,
                            RPTR_WRITEBACK_ENABLE, 1);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL,
                            WPTR_POLL_ENABLE, 0);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL,
                            MCU_WPTR_POLL_ENABLE, 1);

    // 6) Ring base — RB_BASE is bus_addr >> 8, BASE_HI is >> 40.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_BASE),
           static_cast<uint32_t>(inst.ring_gpu_va >> 8));
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_BASE_HI),
           static_cast<uint32_t>(inst.ring_gpu_va >> 40));
    SDMA_LOG("SDMA%u  RB_BASE = %#010x:%#010x (ring_gpu_va >> 8 / >> 40)",
             i,
             static_cast<uint32_t>(inst.ring_gpu_va >> 40),
             static_cast<uint32_t>(inst.ring_gpu_va >> 8));

    inst.wptr = 0;

    // 7) MINOR_PTR_UPDATE handshake — set 1 before writing WPTR,
    //    write WPTR, then clear MINOR_PTR_UPDATE.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_MINOR_PTR_UPDATE), 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR),    inst.wptr << 2);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_WPTR_HI), 0);

    // 8) Doorbell config — enable doorbell + offset = doorbell_index.
    uint32_t doorbell        = RREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL));
    uint32_t doorbell_offset = RREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL_OFFSET));
    doorbell        = REG_SET_FIELD(doorbell,
                                    SDMA0_SDMA_QUEUE0_DOORBELL, ENABLE, 1);
    doorbell_offset = REG_SET_FIELD(doorbell_offset,
                                    SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET, OFFSET,
                                    inst.doorbell_index);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL),        doorbell);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_DOORBELL_OFFSET), doorbell_offset);
    SDMA_LOG("SDMA%u  DOORBELL=%#x DOORBELL_OFFSET=%#x",
             i, doorbell, doorbell_offset);

    // 9) Clear MINOR_PTR_UPDATE after wptr.
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_MINOR_PTR_UPDATE), 0);

    // 10) Watchdog: 100ms per unit, usec_timeout/100000 floored at 1.
    //     For now we just write 1 (we don't carry a usec_timeout var).
    {
        uint32_t v = RREG32(dev, reg(sdma_regs(dev).WATCHDOG_CNTL));
        v = REG_SET_FIELD(v, SDMA0_SDMA_WATCHDOG_CNTL, QUEUE_HANG_COUNT, 1);
        WREG32(dev, reg(sdma_regs(dev).WATCHDOG_CNTL), v);
    }

    // 11) UTCL1 RESP_MODE=3, REDO_DELAY=9.
    {
        uint32_t v = RREG32(dev, reg(sdma_regs(dev).UTCL1_CNTL));
        v = REG_SET_FIELD(v, SDMA0_SDMA_UTCL1_CNTL, RESP_MODE,  3);
        v = REG_SET_FIELD(v, SDMA0_SDMA_UTCL1_CNTL, REDO_DELAY, 9);
        WREG32(dev, reg(sdma_regs(dev).UTCL1_CNTL), v);
    }

    // 12) UTCL1_PAGE — clean read+write policy bits, set L2 defaults
    //     (CACHE_READ_POLICY_L2__DEFAULT = 0 → bits [13:12] = 0,
    //      CACHE_WRITE_POLICY_L2__DEFAULT = 0 → bits [15:14] = 0).
    {
        uint32_t v = RREG32(dev, reg(sdma_regs(dev).UTCL1_PAGE));
        v &= 0xFF0FFFu;
        WREG32(dev, reg(sdma_regs(dev).UTCL1_PAGE), v);
    }

    // 13) Unhalt engine via MCU_CNTL.
    SDMA_LOG("SDMA%u  unhalting MCU (writing MCU_CNTL.HALT=0, RESET=0)", i);
    sdma_engine_halt(dev, i, false);

    // 14) Enable the ring + IB.
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_SDMA_QUEUE0_RB_CNTL, RB_ENABLE, 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_RB_CNTL), rb_cntl);

    uint32_t ib_cntl = RREG32(dev, reg(sdma_regs(dev).QUEUE0_IB_CNTL));
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_SDMA_QUEUE0_IB_CNTL, IB_ENABLE, 1);
    WREG32(dev, reg(sdma_regs(dev).QUEUE0_IB_CNTL), ib_cntl);

    inst.enabled = true;
    SDMA_LOG("SDMA%u: gfx_resume done — RB_CNTL=%#010x IB_CNTL=%#010x "
             "(RB_ENABLED on QUEUE 0, IB_ENABLED, engine running)",
             i, rb_cntl, ib_cntl);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_ring_write — stage dwords into the VRAM-resident ring via
// the BAR0 framebuffer aperture. Engine fetches them from the same
// addresses via the FB aperture (GMC-routable).
//------------------------------------------------------------------
uint32_t
sdma_ring_write(const DeviceContext &dev, SDMAInstance &inst,
                const uint32_t *src, uint32_t dwords)
{
    if (!inst.inited || !src || dwords == 0) return 0;
    if (!inst.ring_size_dwords || (inst.ring_size_dwords & (inst.ring_size_dwords-1)) ||
        inst.ring_ptr_mask!=inst.ring_size_dwords-1 || dwords>inst.ring_size_dwords ||
        inst.wptr>(UINT64_MAX>>2)-dwords) return 0;
    for (uint32_t i = 0; i < dwords; i++) {
        uint32_t slot = (inst.wptr + i) & inst.ring_ptr_mask;
        WBAR0_32(dev, inst.ring_vram_off + slot * 4u, src[i]);
    }
    // Ensure HDP write buffers drain so the engine sees the new
    // packets when it processes the doorbell.
    amdgpu_hdp_flush(dev);
    // Linux publishes a monotonic 64-bit byte count. Masking this counter
    // stalls SDMA at the first ring wrap; only the storage index is masked.
    inst.wptr += dwords;
    return dwords;
}

kern_return_t
sdma_clear_fence(const DeviceContext &dev, const SDMAInstance &inst, uint32_t offset)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (offset != 0x80 && offset != 0xC0) return kIOReturnBadArgument;
    const uint32_t zero = 0;
    return vram_write_verified(dev, inst.wb_vram_off + offset, &zero, 4);
}

kern_return_t
sdma_read_fence(const DeviceContext &dev, const SDMAInstance &inst,
                 uint32_t offset, uint32_t *value)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (offset != 0x80 && offset != 0xC0) return kIOReturnBadArgument;
    return vram_read_fence32(dev, inst.wb_vram_off + offset, value);
}

bool
sdma_read_cs_fence(void *context, uint32_t *value)
{
    const auto *inst = static_cast<const SDMAInstance *>(context);
    return inst && inst->wb_device &&
        sdma_read_fence(*inst->wb_device, *inst, 0xC0, value) == kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_kick_doorbell — write the new wptr into the BAR2 doorbell
// aperture.
//
// WDOORBELL64 takes a dword index, so the BAR2 byte offset is index * 4.
// SDMA's WPTR value is a byte count (unlike MES's dword count).
//------------------------------------------------------------------
kern_return_t
sdma_kick_doorbell(const DeviceContext &dev, const SDMAInstance &inst)
{
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    if (inst.wptr>(UINT64_MAX>>2)) return kIOReturnBadArgument;
    const uint64_t v = static_cast<uint64_t>(inst.wptr) << 2;

    if (!dev.pci) return kIOReturnNotAttached;
    const uint64_t offs = sdma_doorbell_byte_offset(inst.doorbell_index);
    if (offs > dev.bar2Size || 8 > dev.bar2Size - offs) return kIOReturnBadArgument;
    const auto upload = vram_write_verified(dev, inst.wb_vram_off + 0x40, &v, 8);
    if (upload != kIOReturnSuccess) return upload;

    amdgpu_hdp_flush(dev);

    // Publish WPTR before ringing the same dword index programmed in the queue.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    dev.pci->MemoryWrite64(dev.bar2MemIndex, offs, v);

    // Retain the existing MMIO fallback until SDMA doorbell-only delivery
    // is validated. MES's successful BAR2 submissions do not validate SDMA routing.
    if (!dev.doorbell_works) {
        const uint32_t wptr_reg = sdma_reg_offset(
            dev, inst.instance, sdma_regs(dev).QUEUE0_RB_WPTR);
        const uint32_t wptr_hi_reg = sdma_reg_offset(
            dev, inst.instance, sdma_regs(dev).QUEUE0_RB_WPTR_HI);
        WREG32(dev, wptr_reg,    static_cast<uint32_t>(v));
        WREG32(dev, wptr_hi_reg, static_cast<uint32_t>(v >> 32));
    }
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_ring_test — emit FENCE writing a known value to the WB page
// at offset 0x80 then poll for it. Doesn't depend on the host CPU
// having a coherent view; we rely on the engine's writeback.
//
// FENCE packet format (sdma_pkt_open.h):
//   DW0: header   (OP=FENCE)
//   DW1: addr_lo  (dword-aligned bus addr to write)
//   DW2: addr_hi
//   DW3: data     (the fence value)
//------------------------------------------------------------------
kern_return_t
sdma_ring_test(const DeviceContext &dev, SDMAInstance &inst,
               uint64_t timeout_us)
{
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;

    // Use an unused part of the WB page (offset 0x80) as the fence
    // landing slot. Pre-clear it.
    auto r = sdma_clear_fence(dev, inst, 0x80);
    if (r != kIOReturnSuccess) return r;
    uint32_t observed = 0;
    const uint64_t fence_gpu = inst.wb_bus + 0x80;
    const uint32_t fence_value = 0xCAFEC0DEu;

    uint32_t pkt[4];
    pkt[0] = sdma_fence_header();
    pkt[1] = static_cast<uint32_t>(fence_gpu);
    pkt[2] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[3] = fence_value;

    if (sdma_ring_write(dev, inst, pkt, 4) != 4) {
        SDMA_LOG("instance %u: ring_test ring_write failed",
                 inst.instance);
        return kIOReturnNoSpace;
    }
    r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    while (true) {
        r = sdma_read_fence(dev, inst, 0x80, &observed);
        if (r != kIOReturnSuccess) return r;
        if (observed == fence_value) {
            SDMA_LOG("instance %u: ring_test completed", inst.instance);
            return kIOReturnSuccess;
        }
        if ((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000 >= timeout_us) break;
        IOSleep(1);
    }
    SDMA_LOG("instance %u: ring_test timeout (last=%#x)",
             inst.instance, observed);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_copy_linear_test — emit COPY_LINEAR + FENCE, kick doorbell,
// poll. Item 195 (GART sanity): proves the engine actually services
// reads + writes through whatever bus address space the caller hands
// it (sysmem DART, or GART iova when GMC remaps sysmem-as-VRAM).
//
// Packet shape (sdma_v7_0_emit_copy_buffer in upstream):
//   DW0 = OP_COPY | (SUBOP_COPY_LINEAR << 8) | CPV(1)
//   DW1 = byte_count - 1
//   DW2 = parameters  (0 = no endian swap)
//   DW3 = src lo
//   DW4 = src hi
//   DW5 = dst lo
//   DW6 = dst hi
//   DW7 = 0 (CPV byte)
//------------------------------------------------------------------
kern_return_t
sdma_copy_linear_test(const DeviceContext &dev, SDMAInstance &inst,
                      uint64_t src_bus, uint64_t dst_bus,
                      uint32_t byte_count, uint64_t timeout_us)
{
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (byte_count == 0 || byte_count > kSDMACopyLinearMaxBytes) {
        return kIOReturnBadArgument;
    }

    auto r = sdma_clear_fence(dev, inst, 0x80);
    if (r != kIOReturnSuccess) return r;
    uint32_t observed = 0;
    const uint64_t fence_gpu   = inst.wb_bus + 0x80;
    const uint32_t fence_value = 0xDEC0FFEEu;

    uint32_t pkt[12];
    uint32_t n = 0;
    // COPY_LINEAR
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY)
             | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR)
             | SDMA_PKT_HEADER_CPV(1);
    pkt[n++] = byte_count - 1;
    pkt[n++] = 0;                          // endian swap params = 0
    pkt[n++] = static_cast<uint32_t>(src_bus);
    pkt[n++] = static_cast<uint32_t>(src_bus >> 32);
    pkt[n++] = static_cast<uint32_t>(dst_bus);
    pkt[n++] = static_cast<uint32_t>(dst_bus >> 32);
    pkt[n++] = 0;                          // CPV byte
    // FENCE — engine writes fence_value to fence_gpu after the copy.
    pkt[n++] = sdma_fence_header();
    pkt[n++] = static_cast<uint32_t>(fence_gpu);
    pkt[n++] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[n++] = fence_value;

    if (sdma_ring_write(dev, inst, pkt, n) != n) {
        SDMA_LOG("copy_linear_test: ring_write failed");
        return kIOReturnNoSpace;
    }
    r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    const uint64_t start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t elapsed_us = 0;
    do {
        elapsed_us = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start_ns) / 1000;
        r = sdma_read_fence(dev, inst, 0x80, &observed);
        if (r != kIOReturnSuccess) return r;
        if (observed == fence_value) {
            SDMA_LOG("copy_linear_test fence complete: %u bytes %#llx -> %#llx after %llu us",
                     byte_count, (unsigned long long)src_bus,
                     (unsigned long long)dst_bus, (unsigned long long)elapsed_us);
            return kIOReturnSuccess;
        }
        if (elapsed_us >= timeout_us) break;
        IOSleep(1);
    } while (true);
    SDMA_LOG("copy_linear_test: timeout after %llu us (last fence=%#x)",
             (unsigned long long)elapsed_us, observed);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// sdma_init_full — top-level SDMAInit stage entry.
//
// Caller must have done PSP bringup + SMU + GMC + IH + RLC + CP
// first; this stage just adds the two SDMA engines on top.
//
// Order:
//   1) Stop any running queues (in case of warm reset).
//   2) Allocate ring + WB for each instance.
//   3) PSP-load SDMA0 + SDMA1 microcode (firmware bytes uploaded
//      separately via the LoadFirmware selector — caller arranges
//      that before calling). If `sdma.microcode_loaded` is false
//      we skip the gfx_resume step and just log so userspace can
//      retry after firmware upload.
//   4) gfx_resume_instance on each.
//   5) sdma_ring_test on each.
//------------------------------------------------------------------
// sdma_log_status — read and log per-instance SDMA_STATUS_REG.
// Mirrors what `sdma_v7_0_wait_for_idle` reads in upstream
// (sdma_v7_0.c:1478). Use this as a quick "is this engine alive"
// check from any diagnostic path. Bits of interest:
//   [0]   IDLE         — engine is idle
//   [1]   REG_IDLE     — register interface idle
//   [4]   RB_EMPTY     — queue 0 ring buffer empty
//   [8]   RB_CMD_IDLE  — ring command unit idle
//   [9]   RB_CMD_FULL  — ring command unit full (back-pressure)
//   [12]  IB_CMD_IDLE  — IB command unit idle
//   [16]  MC_WR_IDLE   — memory controller writes idle
//   [17]  SRBM_IDLE    — SRBM idle
//   [18]  CONTEXT_EMPTY
//   [19]  DELTA_RPTR_FULL
//   [24]  PREV_CMD_IDLE
//   [25]  PREV_HASHTAG_VALID
//   [29]  REG_CG_REQ
//   [30]  REG_CG_GRANT
void
sdma_log_status(const DeviceContext &dev, uint32_t inst)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("status[%u]: GC IP not resolved", inst);
        return;
    }
    uint32_t status = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).STATUS_REG));
    uint32_t rb_rptr = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).QUEUE0_RB_RPTR));
    uint32_t rb_wptr = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).QUEUE0_RB_WPTR));
    uint32_t rb_cntl = RREG32(dev,
        sdma_reg_offset(dev, inst, sdma_regs(dev).QUEUE0_RB_CNTL));
    // STATUS_REG bit positions per gc_12_0_0_sh_mask.h:142-161.
    SDMA_LOG("SDMA%u status: STATUS_REG=%#010x RB_CNTL=%#010x "
             "rptr=%#x wptr=%#x  "
             "(idle=%u rb_empty=%u rb_full=%u ib_idle=%u srbm_idle=%u)",
             inst, status, rb_cntl, rb_rptr, rb_wptr,
             (status >> 0)  & 1,   // IDLE
             (status >> 2)  & 1,   // RB_EMPTY
             (status >> 3)  & 1,   // RB_FULL
             (status >> 6)  & 1,   // IB_CMD_IDLE
             (status >> 14) & 1);  // SRBM_IDLE
}

kern_return_t
sdma_init_full(DeviceContext &dev,
               PSPContext &psp,
               GMCContext &gmc,
               SDMAContext &sdma)
{
    (void)psp;
    (void)gmc;

    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("init_full: GC IP base not resolved");
        return kIOReturnNotReady;
    }
    SDMA_LOG("init_full: starting SDMA bringup (instances=%u, "
             "microcode_loaded=%d)",
             kSDMAInstanceCount, sdma.microcode_loaded);

    // 1) Bare-metal start path — port of sdma_v7_0_start (sdma_v7_0.c:837).
    //    Upstream does NOT halt the MCU here in bare-metal mode; PSP
    //    autoload has already loaded SDMA microcode and started the MCU,
    //    so we'd be stomping a live engine. Just unhalt (idempotent if
    //    already running) to mirror upstream line 862:
    //        sdma_v7_0_enable(adev, true);   // unhalt the MEs
    //
    //    v0.1.40 fix uncovered this: when sdma_reg_offset was using the
    //    wrong base for hyp-dec range, our previous "defensive stop"
    //    silently failed (writing HALT=1 to a different register). After
    //    fixing the base, the halt actually landed and tore down the
    //    PSP-loaded MCU between unhalt → ring program → re-unhalt. Now
    //    we follow upstream's bare-metal path and never halt.
    //
    //    Clearing RB_ENABLE+IB_ENABLE on QUEUE0 IS still safe: the queue
    //    control bits are at GC[0] (regular range) and the MCU just sees
    //    "queue disabled until ring is programmed", same as a fresh boot.
    SDMA_LOG("init_full: step 1/4 — clear RB/IB enable, unhalt MCU "
             "(bare-metal path, PSP-autoloaded MCU stays live)");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        sdma_gfx_stop_instance(dev, i);
        sdma_engine_halt(dev, i, /*halt=*/false);
        sdma_log_status(dev, i);
    }

    // 2) Allocate storage.
    SDMA_LOG("init_full: step 2/4 — allocate ring + WB per instance");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        sdma.instance[i].instance = i;
        kern_return_t r = sdma_alloc_storage(dev, sdma.instance[i], gmc);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("SDMA%u init_full: storage alloc failed: %#x", i, r);
            return r;
        }
    }

    if (!sdma.microcode_loaded) {
        SDMA_LOG("init_full: microcode_loaded=false — storage allocated, "
                 "deferring gfx_resume + ring_test until "
                 "LoadFirmware(SDMA0/SDMA1) completes");
        return kIOReturnNotReady;
    }

    // 3) gfx_resume each. Mirrors upstream sdma_v7_0_gfx_resume.
    SDMA_LOG("init_full: step 3/4 — gfx_resume each instance "
             "(program RB_BASE/CNTL/WPTR/RPTR, doorbell, watchdog, "
             "unhalt MCU, enable RB+IB)");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        kern_return_t r = sdma_gfx_resume_instance(dev, sdma.instance[i]);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("SDMA%u init_full: gfx_resume failed: %#x", i, r);
            sdma_log_status(dev, i);
            return r;
        }
        // Linux dyndbg pattern: "SDMA %d use_doorbell being set to: [yes]"
        SDMA_LOG("SDMA%u: use_doorbell=yes (slot %#x), engine unhalted",
                 i, sdma.instance[i].doorbell_index);
        sdma_log_status(dev, i);
    }

    // 4) Match Linux amdgpu_ring_test_helper: a failed ring must not
    //    publish a completed SDMA stage. Reset before retrying failed work.
    SDMA_LOG("init_full: step 4/4 — sdma_ring_test on each instance "
             "(submit FENCE pkt, watch WB write)");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        kern_return_t r = sdma_ring_test(dev, sdma.instance[i], /*timeout_us=*/100000);
        if (r != kIOReturnSuccess) {
            sdma_log_status(dev, i);
            return r;
        }
    }

    SDMA_LOG("init_full: done — final per-instance status:");
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        sdma_log_status(dev, i);
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
