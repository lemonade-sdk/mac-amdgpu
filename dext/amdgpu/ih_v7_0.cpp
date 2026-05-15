//
//  ih_v7_0.cpp — Interrupt Handler v7_0 port (RDNA4).
//
//  Source: drivers/gpu/drm/amd/amdgpu/ih_v7_0.c +
//          drivers/gpu/drm/amd/amdgpu/amdgpu_ih.c
//
//  Coverage in this commit:
//    - ih_init               amdgpu_ih_ring_init (allocations only)
//    - ih_enable_ring        ih_v7_0_enable_ring (base/cntl/wptr regs)
//    - ih_toggle             ih_v7_0_toggle_interrupts
//    - ih_program_msi_storm  storm + flood + msi-storm config
//    - ih_drain              amdgpu_ih_process ring walk
//    - ih_init_full          orchestrator entry for BringupStage::IHInit
//
//  Deferred to next commit:
//    - Wiring ih_drain into the MSI-X handler in MacAMDGPU.cpp's
//      InterruptOccurred (currently the IH ring fills but nobody
//      walks it). That needs the dext-side path to know about the
//      driver-instance IHContext.
//    - Per-source dispatch table — for first PM4 we just log entries.
//    - Doorbell-based rptr advance (current implementation writes
//      IH_RB_RPTR via MMIO; slower but simpler).
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>

#include "amdgpu_ih.h"

#define IH_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.ih: " fmt, ##__VA_ARGS__)

namespace amdgpu {

// Compute log2 of ring size in dwords / 4 for IH_RB_CNTL.RB_SIZE.
// Upstream amdgpu_ih_ring_init expects ring size in bytes; the
// register field expects log2(ring_size_dwords).
static inline uint32_t log2u32(uint32_t v) {
    uint32_t r = 0;
    while (v >>= 1) r++;
    return r;
}

// ----- ih_init: allocate ring + wptr shadow -----
//
// Mirrors amdgpu_ih_ring_init(adev, ih, 256 KB, use_bus_addr=true).
// Two DART-mapped buffers:
//   1) ring buffer  — 256 KB, 16 KB-aligned
//   2) wptr shadow  — single AS page (the GPU writes the current
//                     wptr into the first 4 B; the rest is unused)
kern_return_t
ih_init(DeviceContext &dev, IHContext &ih)
{
    if (ih.ring_buf != nullptr) return kIOReturnSuccess;

    const uint32_t ring_size = kIHRingDefaultBytes;
    // Ring buffer.
    IOBufferMemoryDescriptor *ring = nullptr;
    kern_return_t r = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, ring_size, kASPageSize, &ring);
    if (r != kIOReturnSuccess || ring == nullptr) {
        IH_LOG("ring alloc failed: %#x", r);
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    ring->SetLength(ring_size);

    IODMACommandSpecification spec = {};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;
    IODMACommand *ring_dma = nullptr;
    r = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                             &spec, &ring_dma);
    if (r != kIOReturnSuccess || ring_dma == nullptr) {
        ring->release();
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    uint64_t flags = 0;
    uint32_t seg_count = 1;
    IOAddressSegment seg = {};
    r = ring_dma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                ring, 0, ring_size, &flags,
                                &seg_count, &seg);
    if (r != kIOReturnSuccess || seg_count != 1) {
        ring_dma->release();
        ring->release();
        return r != kIOReturnSuccess ? r : kIOReturnNotAligned;
    }
    IOAddressSegment ring_cpu = {};
    ring->GetAddressRange(&ring_cpu);

    // wptr shadow — single AS page.
    IOBufferMemoryDescriptor *wbuf = nullptr;
    r = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
                                         kASPageSize, kASPageSize, &wbuf);
    if (r != kIOReturnSuccess || wbuf == nullptr) {
        ring_dma->release(); ring->release();
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    wbuf->SetLength(kASPageSize);

    IODMACommand *wdma = nullptr;
    r = IODMACommand::Create(dev.pci, kIODMACommandCreateNoOptions,
                             &spec, &wdma);
    if (r != kIOReturnSuccess || wdma == nullptr) {
        wbuf->release();
        ring_dma->release(); ring->release();
        return r != kIOReturnSuccess ? r : kIOReturnNoMemory;
    }
    uint64_t wflags = 0;
    uint32_t wsegs = 1;
    IOAddressSegment wseg = {};
    r = wdma->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                            wbuf, 0, kASPageSize, &wflags, &wsegs, &wseg);
    if (r != kIOReturnSuccess || wsegs != 1) {
        wdma->release(); wbuf->release();
        ring_dma->release(); ring->release();
        return r != kIOReturnSuccess ? r : kIOReturnNotAligned;
    }
    IOAddressSegment wcpu = {};
    wbuf->GetAddressRange(&wcpu);

    // Stash everything.
    ih.ring_buf         = ring;
    ih.ring_dma         = ring_dma;
    ih.ring_bus         = seg.address;
    ih.ring_cpu         = reinterpret_cast<void *>(ring_cpu.address);
    ih.ring_size_bytes  = ring_size;
    ih.ring_size_dwords = ring_size / 4;
    ih.ptr_mask         = ih.ring_size_dwords - 1;
    ih.wptr_shadow_buf  = wbuf;
    ih.wptr_shadow_dma  = wdma;
    ih.wptr_shadow_bus  = wseg.address;
    ih.wptr_shadow_cpu  =
        reinterpret_cast<volatile uint32_t *>(wcpu.address);
    ih.rptr             = 0;
    ih.entries_processed = 0;
    ih.overflows_seen   = 0;

    // Zero the buffers so a stale value doesn't get read as wptr.
    memset(ih.ring_cpu, 0, ring_size);
    *ih.wptr_shadow_cpu = 0;

    ih.inited = true;
    IH_LOG("ring alloc ok: ring bus=%#llx (%u B), wptr_shadow bus=%#llx",
           ih.ring_bus, ring_size, ih.wptr_shadow_bus);
    return kIOReturnSuccess;
}

// ----- ih_release: teardown -----
void
ih_release(IHContext &ih)
{
    if (ih.wptr_shadow_dma != nullptr) {
        ih.wptr_shadow_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        ih.wptr_shadow_dma->release();
        ih.wptr_shadow_dma = nullptr;
    }
    if (ih.wptr_shadow_buf != nullptr) {
        ih.wptr_shadow_buf->release();
        ih.wptr_shadow_buf = nullptr;
    }
    if (ih.ring_dma != nullptr) {
        ih.ring_dma->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        ih.ring_dma->release();
        ih.ring_dma = nullptr;
    }
    if (ih.ring_buf != nullptr) {
        ih.ring_buf->release();
        ih.ring_buf = nullptr;
    }
    ih.ring_bus = 0; ih.ring_cpu = nullptr;
    ih.wptr_shadow_bus = 0; ih.wptr_shadow_cpu = nullptr;
    ih.inited = false; ih.enabled = false;
}

// ----- ih_toggle: flip RB_ENABLE in IH_RB_CNTL -----
kern_return_t
ih_toggle(const DeviceContext &dev, IHContext &ih, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;
    const uint32_t reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                          IHRegs::IH_RB_CNTL);
    uint32_t v = RREG32(dev, reg);
    if (enable) v |=  (1u << kIH_RB_CNTL__RB_ENABLE__SHIFT);
    else        v &= ~(1u << kIH_RB_CNTL__RB_ENABLE__SHIFT);
    WREG32(dev, reg, v);
    return kIOReturnSuccess;
}

// ----- ih_enable_ring: program IH_RB_BASE/CNTL/WPTR_ADDR -----
//
// On enable we:
//   1. Write the ring physical bus address (40-bit split lo/hi)
//   2. Construct IH_RB_CNTL with RB_SIZE = log2(ring_dwords)
//      + WPTR_WRITEBACK_ENABLE + MC_SPACE = 4 (IOMMU) + RPTR_REARM
//      + MC_SNOOP + WPTR_OVERFLOW_ENABLE
//   3. Write wptr shadow address (40-bit split)
//   4. Reset rptr/wptr to 0
//
// We don't touch RB_ENABLE here; ih_toggle(true) does that as the
// final step.
kern_return_t
ih_enable_ring(const DeviceContext &dev, IHContext &ih)
{
    if (!ih.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;

    const uint32_t base_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_BASE);
    const uint32_t basehi_reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_BASE_HI);
    const uint32_t cntl_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_CNTL);
    const uint32_t rptr_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_RPTR);
    const uint32_t wptr_reg   = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_WPTR);
    const uint32_t wptr_lo    = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_WPTR_ADDR_LO);
    const uint32_t wptr_hi    = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                                 IHRegs::IH_RB_WPTR_ADDR_HI);

    // The IH_RB_BASE register expects bits [39:8] of the ring's bus
    // address (8-byte granularity per upstream code). Higher bits
    // go into IH_RB_BASE_HI.
    WREG32(dev, base_reg,   (uint32_t)((ih.ring_bus >> 8) & 0xFFFFFFFFu));
    WREG32(dev, basehi_reg, (uint32_t)((ih.ring_bus >> 40) & 0xFFu));

    // RB_CNTL: mirror ih_v7_0_rb_cntl (ih_v7_0.c:187-208) +
    // ih_v7_0_enable_ring's RPTR_REARM line (252). Field shifts
    // taken from osssys_7_0_0_sh_mask.h.
    //   RB_SIZE             = log2(ring_dwords)        bits[5:1]
    //   MC_SPACE            = 2 (bus_addr)             bits[31:28]
    //   WPTR_OVERFLOW_CLEAR = 1                        bit[31] (self-clearing)
    //   WPTR_OVERFLOW_ENABLE= 1                        bit[16]
    //   WPTR_WRITEBACK_ENABLE = 1                      bit[8]
    //   MC_SNOOP            = 1                        bit[20]
    //   RPTR_REARM          = 1 (we use MSI)           bit[21]
    //   MC_RO / MC_VMID     = 0
    uint32_t cntl = 0;
    cntl |= (log2u32(ih.ring_size_dwords) << kIH_RB_CNTL__RB_SIZE__SHIFT);
    cntl |= (kIH_RB_CNTL_MC_SPACE_BUS_ADDR << kIH_RB_CNTL__MC_SPACE__SHIFT);
    cntl |= (1u << kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT);
    cntl |= (1u << kIH_RB_CNTL__WPTR_OVERFLOW_ENABLE__SHIFT);
    cntl |= (1u << kIH_RB_CNTL__WPTR_WRITEBACK_ENABLE__SHIFT);
    cntl |= (1u << kIH_RB_CNTL__MC_SNOOP__SHIFT);
    cntl |= (1u << kIH_RB_CNTL__RPTR_REARM__SHIFT);
    // RB_ENABLE stays 0 here; ih_toggle(true) sets it later.
    WREG32(dev, cntl_reg, cntl);

    // wptr shadow address — bits [39:2] (4-byte aligned).
    WREG32(dev, wptr_lo, (uint32_t)(ih.wptr_shadow_bus & 0xFFFFFFFCu));
    WREG32(dev, wptr_hi, (uint32_t)(ih.wptr_shadow_bus >> 32) & 0xFFu);

    // Reset pointers.
    WREG32(dev, rptr_reg, 0);
    WREG32(dev, wptr_reg, 0);
    ih.rptr = 0;
    *ih.wptr_shadow_cpu = 0;

    IH_LOG("ring enabled: size=%u dwords, cntl=%#010x", ih.ring_size_dwords, cntl);
    return kIOReturnSuccess;
}

// ----- ih_program_msi_storm: storm/flood control -----
//
// Mirrors the upstream MSI storm config — throttle MSI delivery to
// avoid VM-fault storms melting the host. Linux default DELAY=3.
kern_return_t
ih_program_msi_storm(const DeviceContext &dev, const IHContext &ih)
{
    (void)ih;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return kIOReturnNotReady;
    // IH_MSI_STORM_CTRL: bits [1:0] = DELAY (3 = max throttle)
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                 IHRegs::IH_MSI_STORM_CTRL), 0x3);
    // IH_INT_FLOOD_CNTL: leave at default (Linux writes 0; just clear it)
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                 IHRegs::IH_INT_FLOOD_CNTL), 0);
    // IH_STORM_CLIENT_LIST_CNTL: enable storm protection for client 18
    // (matches upstream default; bit-position derivation in upstream).
    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                 IHRegs::IH_STORM_CLIENT_LIST_CNTL),
           (1u << 18));
    return kIOReturnSuccess;
}

// ----- ih_drain: walk the ring and dispatch entries -----
//
// Mirrors amdgpu_ih_process(). Reads the current wptr (preferring
// the shadow over MMIO), then for each 8-dword entry between
// (rptr, wptr) decodes and dispatches.
//
// Returns the number of entries processed.
uint32_t
ih_drain(const DeviceContext &dev, IHContext &ih,
         IHDispatchFn dispatch, void *user)
{
    if (!ih.inited || !ih.enabled) return 0;
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) return 0;

    // Prefer shadow (avoids a slow BAR read each interrupt).
    uint32_t wptr = (ih.wptr_shadow_cpu != nullptr)
        ? __atomic_load_n(ih.wptr_shadow_cpu, __ATOMIC_ACQUIRE)
        : RREG32(dev,
                 SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                  IHRegs::IH_RB_WPTR));
    // Overflow bit: bit 31 of wptr indicates the GPU's write pointer
    // wrapped past rptr. Linux logs + clears.
    if (wptr & (1u << 31)) {
        ih.overflows_seen++;
        // Clear overflow in IH_RB_CNTL bit 31.
        uint32_t cntl_reg = SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                             IHRegs::IH_RB_CNTL);
        uint32_t cntl = RREG32(dev, cntl_reg);
        cntl |= (1u << kIH_RB_CNTL__WPTR_OVERFLOW_CLEAR__SHIFT);
        WREG32(dev, cntl_reg, cntl);
        IH_LOG("ring overflow seen (count=%llu)", ih.overflows_seen);
        wptr &= ~(1u << 31);
    }

    wptr &= ih.ptr_mask;
    uint32_t count = 0;
    auto *ring = static_cast<const uint32_t *>(ih.ring_cpu);
    while (ih.rptr != wptr) {
        // 8 dwords per entry.
        const uint32_t *e = &ring[ih.rptr];
        IHEntry decoded{};
        decoded.client_id  =  e[0]        & 0xFF;
        decoded.src_id     = (e[0] >> 8)  & 0xFF;
        decoded.ring_id    = (e[0] >> 16) & 0xFF;
        decoded.vmid       = (e[0] >> 24) & 0xF;
        decoded.vmid_src   = (e[0] >> 31) & 0x1;
        decoded.timestamp  = ((uint64_t)(e[2] & 0xFFFF) << 32) | e[1];
        decoded.pasid      =  e[3]        & 0xFFFF;
        decoded.node_id    = (e[3] >> 16) & 0xFF;
        decoded.src_data[0] = e[4];
        decoded.src_data[1] = e[5];
        decoded.src_data[2] = e[6];
        decoded.src_data[3] = e[7];

        if (dispatch != nullptr) dispatch(decoded, user);

        ih.rptr = (ih.rptr + kIHEntryDwords) & ih.ptr_mask;
        count++;
    }

    if (count > 0) {
        // Advance hardware rptr.
        WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::OSSSYS,
                                     IHRegs::IH_RB_RPTR),
               ih.rptr);
        ih.entries_processed += count;
    }
    return count;
}

// ----- ih_init_full: BringupStage::IHInit orchestrator entry -----
kern_return_t
ih_init_full(DeviceContext &dev, IHContext &ih)
{
    if (!dev.ip.isResolved(IPBlock::OSSSYS)) {
        IH_LOG("OSSSYS IP base not resolved");
        return kIOReturnNotReady;
    }
    kern_return_t r;
    r = ih_init(dev, ih);
    if (r != kIOReturnSuccess) return r;
    r = ih_toggle(dev, ih, false);     // safety: disable before config
    if (r != kIOReturnSuccess) return r;
    r = ih_enable_ring(dev, ih);
    if (r != kIOReturnSuccess) return r;
    r = ih_program_msi_storm(dev, ih);
    if (r != kIOReturnSuccess) return r;
    r = ih_toggle(dev, ih, true);
    if (r != kIOReturnSuccess) return r;
    ih.enabled = true;
    IH_LOG("IH ring online (ring=%#llx wptr_shadow=%#llx)",
           ih.ring_bus, ih.wptr_shadow_bus);
    return kIOReturnSuccess;
}

} // namespace amdgpu
