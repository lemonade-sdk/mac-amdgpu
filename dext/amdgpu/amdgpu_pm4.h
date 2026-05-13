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
//      bits [29:16] = opcode      (NOP=0, WRITE_DATA=0x37, RELEASE_MEM=0x49)
//      bits [15:0]  = count of dwords AFTER the header (= total - 1)
//
//  Sources: PM4 packet definitions live in upstream
//      drivers/gpu/drm/amd/include/v12_structs.h
//      drivers/gpu/drm/amd/amdgpu/{gfx,sdma}_v*_pkt.h
//
//  We hand-roll the headers here rather than vendor those headers
//  (each is hundreds of fields); the subset we need is tiny.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

constexpr uint32_t kPM4Type3 = 3u;   // PACKET3

constexpr uint32_t kPM4OpNop        = 0x00;
constexpr uint32_t kPM4OpWriteData  = 0x37;
constexpr uint32_t kPM4OpReleaseMem = 0x49;

// Build a PACKET3 header DWORD. `count` is the number of DWORDs
// that follow the header (NOT including the header itself), minus 1.
// e.g. RELEASE_MEM is 6 dwords of payload after the header → count=6.
static inline uint32_t
pm4_header(uint32_t op, uint32_t count_minus_1)
{
    return (kPM4Type3 << 30)
         | ((op & 0x3FFF) << 16)
         | (count_minus_1 & 0xFFFF);
}

// ---- NOP: payload-less header (count=0). 1 DWORD total. ----
static inline uint32_t pm4_nop(void) { return pm4_header(kPM4OpNop, 0); }

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
//   DW0: header (count=6, since count is total-1 = 7-1 = 6)
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
constexpr uint32_t kPM4RMGCRGL2WB  = (1u << 16);
constexpr uint32_t kPM4RMGCRSeq    = (1u << 20);

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

} // namespace amdgpu
