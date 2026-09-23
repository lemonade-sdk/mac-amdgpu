#pragma once
#include <stdint.h>

namespace amdgpu {

constexpr uint32_t SDMA_OP_NOP    = 0;
constexpr uint32_t SDMA_OP_COPY   = 1;
constexpr uint32_t SDMA_OP_FENCE  = 5;
constexpr uint32_t SDMA_OP_TRAP   = 6;
constexpr uint32_t SDMA_OP_TIMESTAMP = 13;

constexpr uint32_t SDMA_SUBOP_COPY_LINEAR = 0;

static inline uint32_t SDMA_PKT_HEADER_OP(uint32_t op)         { return (op & 0xff); }
static inline uint32_t SDMA_PKT_HEADER_SUB_OP(uint32_t sub_op) { return ((sub_op & 0xff) << 8); }
static inline uint32_t SDMA_PKT_HEADER_CPV(uint32_t v)         { return ((v & 0x1) << 19); }

// COPY_LINEAR permits 0x400000 bytes; the field stores byte_count - 1.
// (HW counter is a 22-bit field, byte_count - 1).
constexpr uint32_t kSDMACopyLinearMaxBytes = 0x00400000u;

// Linux SDMA fences use the uncached memory type (MTYPE=3).
constexpr uint32_t sdma_fence_header()
{
    return 5u | (3u << 16);
}

// WDOORBELL64 indices count dwords, even for a 64-bit write.
constexpr uint64_t sdma_doorbell_byte_offset(uint32_t dword_index)
{
    return uint64_t(dword_index) * 4;
}

} // namespace amdgpu
