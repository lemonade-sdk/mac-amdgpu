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
// RREG32 / WREG32 — 32-bit MMIO over BAR0.
//
// `reg` is the absolute BAR0-relative dword offset (post-SOC15
// resolution). PCIDriverKit's MemoryRead32/Write32 take a byte
// offset, so we shift left by 2.
//============================================================
#ifdef __APPLE__
static inline uint32_t
RREG32(const DeviceContext &ctx, uint32_t reg)
{
    uint32_t value = 0xFFFFFFFFu;
    ctx.pci->MemoryRead32(ctx.bar0MemIndex,
                          static_cast<uint64_t>(reg) * 4ULL, &value);
    return value;
}

static inline void
WREG32(const DeviceContext &ctx, uint32_t reg, uint32_t value)
{
    ctx.pci->MemoryWrite32(ctx.bar0MemIndex,
                           static_cast<uint64_t>(reg) * 4ULL, value);
}

// Absolute BAR0 read — bypasses SOC15 indirection. Used by the
// bootstrap path (IP discovery) to read mmRCC_CONFIG_MEMSIZE +
// mmDRIVER_SCRATCH_{0,1,2} before any IP bases are resolved.
static inline uint32_t
RREG32_abs(const DeviceContext &ctx, uint32_t reg_dword_offset)
{
    uint32_t value = 0xFFFFFFFFu;
    ctx.pci->MemoryRead32(ctx.bar0MemIndex,
                          static_cast<uint64_t>(reg_dword_offset) * 4ULL,
                          &value);
    return value;
}

// 32-bit read from BAR2 at the given **byte** offset. BAR2 maps the
// visible VRAM aperture; offsets larger than ctx.bar2VisibleVRAMSize
// are not valid (the IP discovery TMR must live in the visible
// window or the on-die path bails — see amdgpu_discovery::discover_ips_on_die).
static inline uint32_t
RBAR2_32(const DeviceContext &ctx, uint64_t byte_offset)
{
    uint32_t value = 0;
    ctx.pci->MemoryRead32(ctx.bar2MemIndex, byte_offset, &value);
    return value;
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
