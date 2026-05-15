//
//  amdgpu_regs.h — Register-access primitives over BAR0.
//
//  Higher layers (PSP, SMU, GMC, GFX, …) call RREG32/WREG32 with
//  SOC15 offsets that combine an IP base (resolved at runtime) with
//  a fixed register offset (compile-time). This file owns:
//
//      • the BAR0 base pointer (set once by MacAMDGPU::Start)
//      • the IPBaseTable (filled in from IP discovery or hardcoded)
//      • SOC15_REG_OFFSET — combine IP base + reg offset
//      • RREG32 / WREG32 — 32-bit MMIO over BAR0
//      • a simple poll-until-bit helper for PSP / SMU mailboxes
//
//  Phase 1B uses these directly. Phase 2 will move register access
//  into a thin host-side wrapper so userspace ICD code reuses the
//  same SOC15 abstractions.
//

#pragma once

#include <stdint.h>
#include "amdgpu_ip.h"

#ifdef __APPLE__
#include <PCIDriverKit/IOPCIDevice.h>
#include <DriverKit/IOLib.h>
#endif

//============================================================
// REG_SET_FIELD — mirrors upstream's drm/amd/include/soc15_common.h
// macro. Reads a register-value, clears the field, ORs in the new
// value shifted into place.
//
//     uint32_t tmp = RREG32(dev, reg);
//     tmp = REG_SET_FIELD(tmp, MMMC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
//     WREG32(dev, reg, tmp);
//
// Requires reg##__##field##__SHIFT and reg##__##field##_MASK to
// be defined for every (reg, field) pair touched. See
// amdgpu_field_defs.h for the vendored definitions we need.
//============================================================
#define REG_SET_FIELD(value, reg, field, val) \
    ((((value) & ~(reg##__##field##_MASK))) | \
     ((((uint32_t)(val)) << (reg##__##field##__SHIFT)) & \
      (reg##__##field##_MASK)))

namespace amdgpu {

//============================================================
// Per-device runtime state — owned by the driver instance,
// passed by pointer into the bringup orchestration.
//============================================================
struct DeviceContext {
#ifdef __APPLE__
    IOPCIDevice *pci;
    uint8_t      bar0MemIndex;
    uint8_t      bar2MemIndex;
    uint8_t      bar5MemIndex;
    uint64_t     bar0Size;
    uint64_t     bar2VisibleVRAMSize;  // small-BAR on AS
#endif
    IPBaseTable  ip;
    bool         psoCAlive;
    bool         smuOnline;
    bool         gmcReady;
};

//============================================================
// SOC15_REG_OFFSET — combine IP base + reg offset.
//
// Linux's macro takes a HW IP enum + instance + reg name and
// expands to a full register absolute offset. We do the same.
//============================================================
static inline uint32_t
SOC15_REG_OFFSET(const DeviceContext &ctx, IPBlock block, uint32_t reg)
{
    return ctx.ip.get(block) + reg;
}

//============================================================
// RREG32 / WREG32 — 32-bit MMIO over the register BAR.
//
// IMPORTANT: For all AMD GPUs >= CHIP_BONAIRE (gfx7 and later —
// every modern card including RDNA3/3.5/4), the MMIO register
// window is in BAR5, NOT BAR0. See upstream amdgpu_device.c
// amdgpu_device_init():
//     if (asic_type >= CHIP_BONAIRE) {
//         adev->rmmio_base = pci_resource_start(pdev, 5);  // BAR5
//     } else {
//         adev->rmmio_base = pci_resource_start(pdev, 2);
//     }
// BAR0 on these chips is the framebuffer/VRAM aperture and
// returns 0 for all reads until PSP sets up VRAM backing.
//
// `reg` is the BAR5-relative dword offset (post-SOC15 resolution).
// PCIDriverKit's MemoryRead/Write32 take a byte offset, so we
// shift left by 2.
//============================================================
#ifdef __APPLE__
static inline uint32_t
RREG32(const DeviceContext &ctx, uint32_t reg)
{
    uint32_t value = 0xFFFFFFFFu;
    ctx.pci->MemoryRead32(ctx.bar5MemIndex,
                          static_cast<uint64_t>(reg) * 4ULL, &value);
    return value;
}

static inline void
WREG32(const DeviceContext &ctx, uint32_t reg, uint32_t value)
{
    ctx.pci->MemoryWrite32(ctx.bar5MemIndex,
                           static_cast<uint64_t>(reg) * 4ULL, value);
}

// Absolute register read — bypasses SOC15 indirection. Used by the
// bootstrap path (IP discovery) to read mmRCC_CONFIG_MEMSIZE +
// mmDRIVER_SCRATCH_{0,1,2} + mmMP0_C2PMSG_33 before any IP bases
// are resolved. Same BAR as RREG32 (BAR5 register window).
static inline uint32_t
RREG32_abs(const DeviceContext &ctx, uint32_t reg_dword_offset)
{
    uint32_t value = 0xFFFFFFFFu;
    ctx.pci->MemoryRead32(ctx.bar5MemIndex,
                          static_cast<uint64_t>(reg_dword_offset) * 4ULL,
                          &value);
    return value;
}

// 32-bit read from BAR0 at the given **byte** offset. BAR0 maps the
// visible VRAM aperture (it's the framebuffer BAR on Bonaire+); the
// IP discovery TMR lives in this window when not in sysmem AND when
// the total VRAM fits inside the visible aperture.
static inline uint32_t
RBAR2_32(const DeviceContext &ctx, uint64_t byte_offset)
{
    uint32_t value = 0;
    ctx.pci->MemoryRead32(ctx.bar0MemIndex, byte_offset, &value);
    return value;
}

// 32-bit write to BAR0 at the given **byte** offset. BAR0 is the
// visible-VRAM aperture, so this writes to VRAM at the corresponding
// VRAM offset. Used for staging firmware buffers PSP will read via
// its internal GMC path.
static inline void
WBAR0_32(const DeviceContext &ctx, uint64_t byte_offset, uint32_t value)
{
    ctx.pci->MemoryWrite32(ctx.bar0MemIndex, byte_offset, value);
}

// memcpy-equivalent for staging a buffer into VRAM via BAR0. Writes
// 32-bit dwords; src must be 4-byte aligned in length (rounded up if
// not). Slow (each dword is an MMIO write — ~10x slower on AS than
// native PCIe) but correct.
static inline void
bar0_memcpy_to_vram(const DeviceContext &ctx, uint64_t vram_byte_offset,
                    const void *src, uint64_t size_bytes)
{
    const uint32_t *p = static_cast<const uint32_t *>(src);
    uint64_t dwords = (size_bytes + 3) >> 2;
    for (uint64_t i = 0; i < dwords; i++) {
        WBAR0_32(ctx, vram_byte_offset + i * 4ULL, p[i]);
    }
}

// memset-equivalent for the BAR0 aperture — zero-fills size_bytes of
// VRAM starting at the given offset.
static inline void
bar0_memset_vram(const DeviceContext &ctx, uint64_t vram_byte_offset,
                 uint32_t pattern, uint64_t size_bytes)
{
    uint64_t dwords = (size_bytes + 3) >> 2;
    for (uint64_t i = 0; i < dwords; i++) {
        WBAR0_32(ctx, vram_byte_offset + i * 4ULL, pattern);
    }
}

// Read a single dword from anywhere in VRAM via the
// mmMM_INDEX / mmMM_INDEX_HI / mmMM_DATA register pair.
//
// Mirrors upstream amdgpu_device_mm_access():
//     WREG32(mmMM_INDEX, (pos & 0xffffffff) | 0x80000000);
//     WREG32(mmMM_INDEX_HI, pos >> 31);
//     return RREG32(mmMM_DATA);
//
// Required to reach VRAM offsets above the visible BAR0 aperture —
// e.g. the IP discovery TMR sits at (vram_size - 1 MB), which is at
// ~30 GB for a 32 GB card with only 256 MB visible. Same register
// layout on all chips since bif_3_0 — see asic_reg/bif/bif_5_1_d.h.
static inline uint32_t
RVRAM32_via_mm(const DeviceContext &ctx, uint64_t pos)
{
    constexpr uint32_t mmMM_INDEX     = 0x0;
    constexpr uint32_t mmMM_DATA      = 0x1;
    constexpr uint32_t mmMM_INDEX_HI  = 0x6;
    WREG32(ctx, mmMM_INDEX, ((uint32_t)pos) | 0x80000000u);
    WREG32(ctx, mmMM_INDEX_HI, (uint32_t)(pos >> 31));
    return RREG32(ctx, mmMM_DATA);
}
#endif

//============================================================
// poll_reg — read a register until (value & mask) == expected,
// or timeout_us microseconds elapse. Returns the last value
// observed regardless of success.
//
// All PSP / SMU mailbox waits go through here.
//============================================================
#ifdef __APPLE__
static inline bool
poll_reg(const DeviceContext &ctx, uint32_t reg,
         uint32_t mask, uint32_t expected,
         uint64_t timeout_us, uint32_t *outValue = nullptr)
{
    // 1 ms IOSleep between probes. On Apple Silicon each MMIO read
    // over the DART path is ~10× the latency of a native PCI read,
    // so the previous "busy spin 1000 RREG32 = 50 µs" approximation
    // actually burned tens of ms per iteration and stretched a
    // nominal 5 s timeout into minutes while pegging a CPU. IOSleep
    // yields the dispatcher and gives a deterministic wall-clock
    // timeout.
    const uint64_t kStepMs = 1;
    uint64_t elapsed_us = 0;
    uint32_t v = 0;
    while (true) {
        v = RREG32(ctx, reg);
        if ((v & mask) == expected) {
            if (outValue) *outValue = v;
            return true;
        }
        if (elapsed_us >= timeout_us) {
            if (outValue) *outValue = v;
            return false;
        }
        IOSleep(kStepMs);
        elapsed_us += kStepMs * 1000;
    }
}
#endif

} // namespace amdgpu
