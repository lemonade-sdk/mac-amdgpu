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
#endif

namespace MacAMDGPU {

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
    // Conservative ~50 µs sleep per iteration. Real implementation
    // should use IOSleep or absolute time, but for Phase 1B we keep
    // it simple. PSP bootloader waits often complete in <1 ms.
    const uint64_t kStepUs = 50;
    uint64_t elapsed = 0;
    uint32_t v = 0;
    while (true) {
        v = RREG32(ctx, reg);
        if ((v & mask) == expected) {
            if (outValue) *outValue = v;
            return true;
        }
        if (elapsed >= timeout_us) {
            if (outValue) *outValue = v;
            return false;
        }
        // IOSleep is in milliseconds; we approximate by spinning briefly.
        // TODO(phase1b): replace with absolute-time-based wait.
        for (volatile int i = 0; i < 1000; i++) { /* busy */ }
        elapsed += kStepUs;
    }
}
#endif

} // namespace MacAMDGPU
