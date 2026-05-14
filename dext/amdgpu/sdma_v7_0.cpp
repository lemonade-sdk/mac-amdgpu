//
//  sdma_v7_0.cpp — SDMA v7_0 ring bringup for RDNA4 / gfx1201.
//
//  Sources:
//      drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c
//          (sdma_v7_0_gfx_resume_instance, sdma_v7_0_enable,
//           sdma_v7_0_gfx_stop, sdma_v7_0_get_reg_offset)
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>

#include "amdgpu_sdma.h"
#include "amdgpu_psp.h"
#include "amdgpu_gmc.h"

#define SDMA_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.sdma: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// Local DMA helper — same pattern as cp_alloc_dma_block. Promote
// once a fourth consumer shows up.
static kern_return_t
sdma_alloc_dma_block(DeviceContext &dev, uint64_t size,
                     IOBufferMemoryDescriptor **outBuf,
                     IODMACommand             **outDma,
                     uint64_t *outBus, void **outCpu)
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

//------------------------------------------------------------------
// sdma_alloc_storage — ring + WB page for one instance.
//------------------------------------------------------------------
kern_return_t
sdma_alloc_storage(DeviceContext &dev, SDMAInstance &inst)
{
    if (inst.inited) return kIOReturnSuccess;
    void *cpu = nullptr;

    kern_return_t r = sdma_alloc_dma_block(dev, kSDMARingDefaultBytes,
                                           &inst.ring_buf, &inst.ring_dma,
                                           &inst.ring_bus, &cpu);
    if (r != kIOReturnSuccess) {
        SDMA_LOG("instance %u: ring alloc failed: %#x", inst.instance, r);
        return r;
    }
    memset(cpu, 0, kSDMARingDefaultBytes);
    inst.ring_cpu         = cpu;
    inst.ring_size_dwords = kSDMARingDefaultBytes / 4;
    inst.ring_ptr_mask    = inst.ring_size_dwords - 1;

    r = sdma_alloc_dma_block(dev, kSDMAWBPageBytes,
                             &inst.wb_buf, &inst.wb_dma,
                             &inst.wb_bus, &cpu);
    if (r != kIOReturnSuccess) {
        SDMA_LOG("instance %u: WB page alloc failed: %#x",
                 inst.instance, r);
        inst.ring_dma->release(); inst.ring_buf->release();
        inst.ring_dma = nullptr; inst.ring_buf = nullptr;
        return r;
    }
    memset(cpu, 0, kSDMAWBPageBytes);
    inst.wb_cpu             = cpu;
    inst.rptr_cpu           = reinterpret_cast<volatile uint32_t *>(cpu);
    inst.rptr_gpu_addr      = inst.wb_bus + 0;
    inst.wptr_poll_gpu_addr = inst.wb_bus + 0x40;

    inst.wptr           = 0;
    // Doorbell index — first PM4 hardcodes per-instance slots in the
    // user's doorbell page. SDMA0 = slot 0x10, SDMA1 = slot 0x12
    // (matches upstream doorbell_index.sdma_engine[0/1] for SOC21).
    inst.doorbell_index = (inst.instance == 0) ? 0x10 : 0x12;
    inst.inited         = true;

    SDMA_LOG("instance %u: ring %u dwords @ bus %#llx, "
             "wb @ bus %#llx, doorbell slot %#x",
             inst.instance, inst.ring_size_dwords,
             (unsigned long long)inst.ring_bus,
             (unsigned long long)inst.wb_bus,
             inst.doorbell_index);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_engine_halt — write SDMA0_MCU_CNTL.HALT.
//------------------------------------------------------------------
kern_return_t
sdma_engine_halt(const DeviceContext &dev, uint32_t instance, bool halt)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        SDMA_LOG("engine_halt: GC IP base not resolved");
        return kIOReturnNotReady;
    }
    const uint32_t reg = sdma_reg_offset(dev, instance, SDMARegs::MCU_CNTL);
    uint32_t v = RREG32(dev, reg);
    v = REG_SET_FIELD(v, SDMA0_MCU_CNTL, HALT, halt ? 1u : 0u);
    v = REG_SET_FIELD(v, SDMA0_MCU_CNTL, RESET, 0u);
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
        sdma_reg_offset(dev, instance, SDMARegs::QUEUE0_RB_CNTL);
    const uint32_t ib_cntl_reg =
        sdma_reg_offset(dev, instance, SDMARegs::QUEUE0_IB_CNTL);

    uint32_t rb_cntl = RREG32(dev, rb_cntl_reg);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 0);
    WREG32(dev, rb_cntl_reg, rb_cntl);

    uint32_t ib_cntl = RREG32(dev, ib_cntl_reg);
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_QUEUE0_IB_CNTL, IB_ENABLE, 0);
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
// sdma_gfx_resume_instance — port of sdma_v7_0_gfx_resume_instance.
//
// Programs all the QUEUE0 registers (RB_CNTL/BASE/WPTR/RPTR/DOORBELL),
// unhalts the engine via MCU_CNTL, enables the ring + IB queue.
// Skipped from upstream:
//   • SR-IOV branches (we never run SR-IOV)
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

    // 1) Initial RB_CNTL — set RB_SIZE, set RB_PRIV.
    uint32_t rb_bufsz = order_base_2(inst.ring_size_dwords);
    uint32_t rb_cntl = RREG32(dev, reg(SDMARegs::QUEUE0_RB_CNTL));
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_SIZE, rb_bufsz);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_PRIV, 1);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_CNTL), rb_cntl);

    // 2) Reset RPTR/WPTR.
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_RPTR),    0);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_RPTR_HI), 0);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_WPTR),    0);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_WPTR_HI), 0);

    // 3) WPTR poll address (shadow in WB page; engine doesn't use
    //    when WPTR_POLL_ENABLE=0, but we still program it).
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_WPTR_POLL_ADDR_LO),
           static_cast<uint32_t>(inst.wptr_poll_gpu_addr));
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_WPTR_POLL_ADDR_HI),
           static_cast<uint32_t>(inst.wptr_poll_gpu_addr >> 32));

    // 4) RPTR write-back address — engine deposits read-pointer here.
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_RPTR_ADDR_HI),
           static_cast<uint32_t>(inst.rptr_gpu_addr >> 32));
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_RPTR_ADDR_LO),
           static_cast<uint32_t>(inst.rptr_gpu_addr & 0xFFFFFFFC));

    // 5) Enable RPTR writeback. MCU_WPTR_POLL_ENABLE on per upstream
    //    bare-metal path; WPTR_POLL_ENABLE off (driver kicks doorbell).
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL,
                            RPTR_WRITEBACK_ENABLE, 1);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL,
                            WPTR_POLL_ENABLE, 0);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL,
                            MCU_WPTR_POLL_ENABLE, 1);

    // 6) Ring base — RB_BASE is bus_addr >> 8, BASE_HI is >> 40.
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_BASE),
           static_cast<uint32_t>(inst.ring_bus >> 8));
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_BASE_HI),
           static_cast<uint32_t>(inst.ring_bus >> 40));

    inst.wptr = 0;

    // 7) MINOR_PTR_UPDATE handshake — set 1 before writing WPTR,
    //    write WPTR, then clear MINOR_PTR_UPDATE.
    WREG32(dev, reg(SDMARegs::QUEUE0_MINOR_PTR_UPDATE), 1);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_WPTR),    inst.wptr << 2);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_WPTR_HI), 0);

    // 8) Doorbell config — enable doorbell + offset = doorbell_index.
    uint32_t doorbell        = RREG32(dev, reg(SDMARegs::QUEUE0_DOORBELL));
    uint32_t doorbell_offset = RREG32(dev, reg(SDMARegs::QUEUE0_DOORBELL_OFFSET));
    doorbell        = REG_SET_FIELD(doorbell,
                                    SDMA0_QUEUE0_DOORBELL, ENABLE, 1);
    doorbell_offset = REG_SET_FIELD(doorbell_offset,
                                    SDMA0_QUEUE0_DOORBELL_OFFSET, OFFSET,
                                    inst.doorbell_index);
    WREG32(dev, reg(SDMARegs::QUEUE0_DOORBELL),        doorbell);
    WREG32(dev, reg(SDMARegs::QUEUE0_DOORBELL_OFFSET), doorbell_offset);

    // 9) Clear MINOR_PTR_UPDATE after wptr.
    WREG32(dev, reg(SDMARegs::QUEUE0_MINOR_PTR_UPDATE), 0);

    // 10) Watchdog: 100ms per unit, usec_timeout/100000 floored at 1.
    //     For now we just write 1 (we don't carry a usec_timeout var).
    {
        uint32_t v = RREG32(dev, reg(SDMARegs::WATCHDOG_CNTL));
        v = REG_SET_FIELD(v, SDMA0_WATCHDOG_CNTL, QUEUE_HANG_COUNT, 1);
        WREG32(dev, reg(SDMARegs::WATCHDOG_CNTL), v);
    }

    // 11) UTCL1 RESP_MODE=3, REDO_DELAY=9.
    {
        uint32_t v = RREG32(dev, reg(SDMARegs::UTCL1_CNTL));
        v = REG_SET_FIELD(v, SDMA0_UTCL1_CNTL, RESP_MODE,  3);
        v = REG_SET_FIELD(v, SDMA0_UTCL1_CNTL, REDO_DELAY, 9);
        WREG32(dev, reg(SDMARegs::UTCL1_CNTL), v);
    }

    // 12) UTCL1_PAGE — clean read+write policy bits, set L2 defaults
    //     (CACHE_READ_POLICY_L2__DEFAULT = 0 → bits [13:12] = 0,
    //      CACHE_WRITE_POLICY_L2__DEFAULT = 0 → bits [15:14] = 0).
    {
        uint32_t v = RREG32(dev, reg(SDMARegs::UTCL1_PAGE));
        v &= 0xFF0FFFu;
        WREG32(dev, reg(SDMARegs::UTCL1_PAGE), v);
    }

    // 13) Unhalt engine via MCU_CNTL.
    sdma_engine_halt(dev, i, false);

    // 14) Enable the ring + IB.
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_QUEUE0_RB_CNTL, RB_ENABLE, 1);
    WREG32(dev, reg(SDMARegs::QUEUE0_RB_CNTL), rb_cntl);

    uint32_t ib_cntl = RREG32(dev, reg(SDMARegs::QUEUE0_IB_CNTL));
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_QUEUE0_IB_CNTL, IB_ENABLE, 1);
    WREG32(dev, reg(SDMARegs::QUEUE0_IB_CNTL), ib_cntl);

    inst.enabled = true;
    SDMA_LOG("instance %u: gfx_resume done (rb_cntl=%#x, ib_cntl=%#x)",
             i, rb_cntl, ib_cntl);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// sdma_ring_write — same shape as cp_ring_write.
//------------------------------------------------------------------
uint32_t
sdma_ring_write(SDMAInstance &inst, const uint32_t *src, uint32_t dwords)
{
    if (!inst.inited || dwords == 0) return 0;
    if (dwords > inst.ring_size_dwords) return 0;
    auto *ring = static_cast<uint32_t *>(inst.ring_cpu);
    for (uint32_t i = 0; i < dwords; i++) {
        ring[(inst.wptr + i) & inst.ring_ptr_mask] = src[i];
    }
    inst.wptr = (inst.wptr + dwords) & inst.ring_ptr_mask;
    return dwords;
}

//------------------------------------------------------------------
// sdma_kick_doorbell — BAR5 write at (doorbell_index * 8).
// Wptr is the byte offset (dword wptr << 2).
//------------------------------------------------------------------
kern_return_t
sdma_kick_doorbell(const DeviceContext &dev, const SDMAInstance &inst)
{
    if (!inst.inited) return kIOReturnNotReady;
    const uint64_t offs = static_cast<uint64_t>(inst.doorbell_index) * 8ull;
    const uint32_t v = inst.wptr << 2;
    dev.pci->MemoryWrite32(dev.bar5MemIndex, offs, v);
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
    auto *wb_bytes = static_cast<volatile uint8_t *>(inst.wb_cpu);
    volatile uint32_t *fence_cpu =
        reinterpret_cast<volatile uint32_t *>(wb_bytes + 0x80);
    *fence_cpu = 0;
    const uint64_t fence_gpu = inst.wb_bus + 0x80;
    const uint32_t fence_value = 0xCAFEC0DEu;

    uint32_t pkt[4];
    pkt[0] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[1] = static_cast<uint32_t>(fence_gpu);
    pkt[2] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[3] = fence_value;

    if (sdma_ring_write(inst, pkt, 4) != 4) {
        SDMA_LOG("instance %u: ring_test ring_write failed",
                 inst.instance);
        return kIOReturnNoSpace;
    }
    kern_return_t r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    // Poll the WB landing slot.
    const uint64_t step_us = 50;
    uint64_t elapsed = 0;
    while (elapsed < timeout_us) {
        if (*fence_cpu == fence_value) {
            SDMA_LOG("instance %u: ring_test ok in ~%llu us",
                     inst.instance, (unsigned long long)elapsed);
            return kIOReturnSuccess;
        }
        // crude busy-wait — refine when we have a real ns delay
        uint32_t scratch = 0;
        for (int i = 0; i < 1000; i++) { scratch ^= *fence_cpu; }
        (void)scratch;
        elapsed += step_us;
    }
    SDMA_LOG("instance %u: ring_test timeout (last=%#x)",
             inst.instance, *fence_cpu);
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

    auto *wb_bytes = static_cast<volatile uint8_t *>(inst.wb_cpu);
    volatile uint32_t *fence_cpu =
        reinterpret_cast<volatile uint32_t *>(wb_bytes + 0x80);
    *fence_cpu = 0;
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
    pkt[n++] = SDMA_PKT_HEADER_OP(SDMA_OP_FENCE);
    pkt[n++] = static_cast<uint32_t>(fence_gpu);
    pkt[n++] = static_cast<uint32_t>(fence_gpu >> 32);
    pkt[n++] = fence_value;

    if (sdma_ring_write(inst, pkt, n) != n) {
        SDMA_LOG("copy_linear_test: ring_write failed");
        return kIOReturnNoSpace;
    }
    kern_return_t r = sdma_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    const uint64_t step_us = 50;
    uint64_t elapsed = 0;
    while (elapsed < timeout_us) {
        if (*fence_cpu == fence_value) {
            SDMA_LOG("copy_linear_test ok: %u bytes %#llx -> %#llx in ~%llu us",
                     byte_count,
                     (unsigned long long)src_bus,
                     (unsigned long long)dst_bus,
                     (unsigned long long)elapsed);
            return kIOReturnSuccess;
        }
        uint32_t scratch = 0;
        for (int i = 0; i < 1000; i++) { scratch ^= *fence_cpu; }
        (void)scratch;
        elapsed += step_us;
    }
    SDMA_LOG("copy_linear_test: timeout (last fence=%#x)", *fence_cpu);
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

    // 1) Stop both queues defensively.
    sdma_gfx_stop_instance(dev, 0);
    sdma_gfx_stop_instance(dev, 1);
    sdma_engine_halt(dev, 0, true);
    sdma_engine_halt(dev, 1, true);

    // 2) Allocate storage.
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        sdma.instance[i].instance = i;
        kern_return_t r = sdma_alloc_storage(dev, sdma.instance[i]);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("init_full: instance %u alloc failed: %#x", i, r);
            return r;
        }
    }

    if (!sdma.microcode_loaded) {
        SDMA_LOG("init_full: microcode not yet loaded; storage allocated, "
                 "deferring gfx_resume until LoadFirmware(SDMA0/SDMA1)");
        return kIOReturnSuccess;
    }

    // 3) gfx_resume each.
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        kern_return_t r = sdma_gfx_resume_instance(dev, sdma.instance[i]);
        if (r != kIOReturnSuccess) {
            SDMA_LOG("init_full: instance %u resume failed: %#x", i, r);
            return r;
        }
    }

    // 4) Ring test on each. Failure is logged but doesn't kill the
    //    init — userspace can re-run via a selector.
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
        sdma_ring_test(dev, sdma.instance[i], /*timeout_us=*/100000);
    }

    return kIOReturnSuccess;
}

} // namespace amdgpu
