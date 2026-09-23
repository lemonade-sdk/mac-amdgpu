#pragma once
#include <stdint.h>
#include <string.h>
#include "amdgpu_ucode_psp.h"

namespace amdgpu {

// Retain values, not pointers into the host's reusable firmware upload buffer.
struct CPFirmwareStart {
    uint64_t address = 0;
    bool loaded = false; // Published only after every PSP payload acknowledges.
};

static inline bool cp_parse_firmware_start(const uint8_t *bin, uint64_t size,
                                           uint64_t &address)
{
    address = 0;
    if (!bin || size < sizeof(gfx_firmware_header_v2_0)) return false;
    gfx_firmware_header_v2_0 hdr;
    memcpy(&hdr, bin, sizeof(hdr));
    const auto &h = hdr.header;
    if (h.header_version_major != 2 || h.header_version_minor != 0 ||
        h.header_size_bytes < sizeof(hdr) || h.header_size_bytes > h.size_bytes ||
        h.size_bytes > size) return false;
    auto fits = [&](uint32_t offset, uint32_t bytes) {
        return bytes != 0 && offset >= h.header_size_bytes &&
               uint64_t(offset) + bytes <= h.size_bytes;
    };
    if (!fits(h.ucode_array_offset_bytes, hdr.ucode_size_bytes) ||
        !fits(hdr.data_offset_bytes, hdr.data_size_bytes)) return false;
    const uint64_t entry = uint64_t(hdr.ucode_start_addr_lo) |
                           (uint64_t(hdr.ucode_start_addr_hi) << 32);
    if (entry == 0 || (entry & 3)) return false;
    address = entry;
    return true;
}

} // namespace amdgpu
