//
//  amdgpu_pm4.h — PM4 packet builders for GFX12.
//
//  PM4 (Packet format 4) is the command stream language the GPU's
//  command processor consumes. Each packet is a header DWORD plus a
//  variable-length payload. We only need a small subset for "Hello
//  PM4": NOP, WRITE_DATA, RELEASE_MEM (EOP fence).
//
//  Header layout (DWORD 0):
//      bits [31:30] = packet type (3 = PACKET3)
//      bits [29:16] = payload dword count minus one (= total - 2)
//      bits [15:8]  = opcode (NOP=0x10, WRITE_DATA=0x37, RELEASE_MEM=0x49)
//
//  Sources: PM4 packet definitions live in upstream
//      drivers/gpu/drm/amd/amdgpu/nvd.h
//      drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c: gfx_v12_0_ring_emit_fence
//
//  We hand-roll the headers here rather than vendor those headers
//  (each is hundreds of fields); the subset we need is tiny.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

constexpr uint32_t kPM4Type3 = 3u;   // PACKET3

constexpr uint32_t kPM4OpNop        = 0x10;
constexpr uint32_t kPM4OpWriteData  = 0x37;
constexpr uint32_t kPM4OpReleaseMem = 0x49;
constexpr uint32_t kPM4OpIndirectBuffer = 0x3f;
constexpr uint32_t kPM4OpSetUconfigReg = 0x79;
constexpr uint32_t kPM4UconfigStart = 0xc000;
constexpr uint32_t kPM4UconfigEnd = 0xc400;

// Build a PACKET3 header DWORD. `count` is the number of DWORDs
// that follow the header (NOT including the header itself), minus 1.
// e.g. RELEASE_MEM has 7 payload dwords → count_minus_1=6.
static inline uint32_t
pm4_header(uint32_t op, uint32_t count_minus_1)
{
    return (kPM4Type3 << 30)
         | ((op & 0xFF) << 8)
         | ((count_minus_1 & 0x3FFF) << 16);
}

// ---- NOP header (count=0). Must be followed by one payload DWORD. ----
static inline uint32_t pm4_nop(void) { return pm4_header(kPM4OpNop, 0); }

// Linux gfx_v12_0_ring_emit_ib_gfx: the IB control selects the application's
// VMID independently of the queue's own ring-fetch VMID. GFX does not use the
// compute-ring INDIRECT_BUFFER_VALID bit. Enforce its 32-byte IB alignment.
static inline uint32_t pm4_gfx_ib(uint32_t (&packet)[4], uint64_t address,
                                 uint32_t dwords, uint32_t vmid)
{
    if ((address & 31) || (address >> 48) || !dwords || (dwords & 7) ||
        dwords > 0xfffff || vmid > 15 ||
        address > (1ull << 48) - uint64_t(dwords) * 4) return 0;
    packet[0] = pm4_header(kPM4OpIndirectBuffer, 2);
    packet[1] = static_cast<uint32_t>(address);
    packet[2] = static_cast<uint32_t>(address >> 32);
    packet[3] = dwords | (vmid << 24);
    return 4;
}

// ---- WRITE_DATA: write N DWORDs to memory. Minimum form (1 DW data):
//   DW0: header
//   DW1: control word
//   DW2: dst addr lo
//   DW3: dst addr hi
//   DW4..: data
//
// Control bits (subset):
//   [27:24] = engine_sel  (0 = ME)
//   [22:20] = dst_sel     (5 = memory)
//   [16]    = wr_confirm
constexpr uint32_t kPM4WriteDataDstSelMemory = 5;
constexpr uint32_t kPM4WriteDataEngineME     = 0;

static inline uint32_t
pm4_write_data_control(uint32_t engine_sel, uint32_t dst_sel,
                       bool wr_confirm)
{
    uint32_t c = 0;
    c |= ((engine_sel & 0xF) << 24);
    c |= ((dst_sel    & 0x7) << 20);
    if (wr_confirm) c |= (1u << 16);
    return c;
}

// ---- RELEASE_MEM (EOP fence). 8 DWORDs total (header + 7 payload).
//
//   DW0: header (count=6, since count is payload-1 = 7-1 = 6)
//   DW1: event_type + event_index + cache flush + GCR flags
//   DW2: data_sel + int_sel
//   DW3: dst addr lo (must be qword-aligned)
//   DW4: dst addr hi
//   DW5: fence value lo
//   DW6: fence value hi
//   DW7: pad (0)
//
// We model the bits with constants:
constexpr uint32_t kPM4RMEventCacheFlushAndInvTS = 0x14;   // event_type
constexpr uint32_t kPM4RMEventIndexFence         = 0x05;   // event_index

// CACHE_POLICY: 0=NC, 1=WC, 2=??, 3=BYPASS — using BYPASS keeps
// the fence write coherent w.r.t. CPU readback.
constexpr uint32_t kPM4RMCachePolicyBypass = 0x3;

// GCR (Global Cache Refresh) bits — minimal: GL2_WB + SEQ
constexpr uint32_t kPM4RMGCRGL2WB  = (1u << 21);
constexpr uint32_t kPM4RMGCRSeq    = (1u << 22);

constexpr uint32_t kPM4RMDataSel32       = 1;
constexpr uint32_t kPM4RMDataSel64       = 2;
constexpr uint32_t kPM4RMIntSelNone      = 0;
constexpr uint32_t kPM4RMIntSelSendInt   = 2;

static inline uint32_t
pm4_release_mem_dw1(void)
{
    uint32_t v = 0;
    v |= (kPM4RMEventCacheFlushAndInvTS & 0x3F);            // [5:0]
    v |= ((kPM4RMEventIndexFence & 0xF) << 8);              // [11:8]
    v |= kPM4RMGCRGL2WB;
    v |= kPM4RMGCRSeq;
    v |= ((kPM4RMCachePolicyBypass & 0x3) << 25);
    return v;
}

static inline uint32_t
pm4_release_mem_dw2(uint32_t data_sel, uint32_t int_sel)
{
    uint32_t v = 0;
    v |= ((data_sel & 0x7) << 29);
    v |= ((int_sel  & 0x3) << 24);
    return v;
}

// Two-dword NOP followed by the eight-dword GFX12 RELEASE_MEM packet.
// Callers provide a dword-aligned address (qword-aligned for 64-bit writes).
static inline uint32_t
pm4_build_fence(uint32_t (&packet)[10], uint64_t address, uint64_t value,
                bool write64, bool interrupt)
{
    packet[0] = pm4_nop();
    packet[1] = 0;
    packet[2] = pm4_header(kPM4OpReleaseMem, 6);
    packet[3] = pm4_release_mem_dw1();
    packet[4] = pm4_release_mem_dw2(
        write64 ? kPM4RMDataSel64 : kPM4RMDataSel32,
        interrupt ? kPM4RMIntSelSendInt : kPM4RMIntSelNone);
    packet[5] = static_cast<uint32_t>(address);
    packet[6] = static_cast<uint32_t>(address >> 32);
    packet[7] = static_cast<uint32_t>(value);
    packet[8] = static_cast<uint32_t>(value >> 32);
    packet[9] = 0;
    return 10;
}

} // namespace amdgpu
