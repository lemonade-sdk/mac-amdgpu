#pragma once

#include <stdint.h>

namespace amdgpu {

// PCIe Resizable BAR capability v1, as used by Linux pci_rebar_find_pos
// and pci_rebar_get_possible_sizes. This module only reads config space.
struct ReBARInfo {
    uint32_t capability;
    uint32_t control;
    uint32_t supportedSizes; // bit n = 2^(20+n) bytes
    uint64_t selectedBytes;
};

enum class ReBARResult { Found, NotFound, UnsupportedVersion, Malformed, ReadError };

template <typename Read32>
ReBARResult read_rebar(uint64_t offset, uint32_t bar, Read32 read,
                       ReBARInfo &result)
{
    result = {};
    if (bar >= 6 || offset < 0x100 || offset > 0xff4 || (offset & 3))
        return ReBARResult::Malformed;
    uint32_t header = 0, firstControl = 0;
    if (!read(offset, header) || !read(offset + 8, firstControl))
        return ReBARResult::ReadError;
    if (header == 0xffffffff || (header & 0xffff) != 0x15)
        return ReBARResult::Malformed;
    if (((header >> 16) & 0xf) != 1)
        return ReBARResult::UnsupportedVersion;
    const uint32_t count = (firstControl >> 5) & 7; // actual count, not count-1
    const uint64_t end = offset + 4 + count * 8;
    const uint32_t next = header >> 20;
    if (count == 0 || count > 6 || end > 0x1000 ||
        (next && ((next & 3) || next < 0x100 ||
                  (next >= offset && next < end))))
        return ReBARResult::Malformed;

    uint32_t seen = 0;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t cap = 0, control = 0;
        if (!read(offset + 4 + i * 8, cap) ||
            !read(offset + 8 + i * 8, control))
            return ReBARResult::ReadError;
        const uint32_t index = control & 7;
        const uint32_t sizes = cap >> 4;
        const uint32_t selected = (control >> 8) & 0x1f;
        if (cap == 0xffffffff || control == 0xffffffff || index >= 6 ||
            (seen & (1u << index)) || selected >= 28 ||
            !(sizes & (1u << selected)))
            return ReBARResult::Malformed;
        seen |= 1u << index;
        if (index == bar) {
            result = {cap, control, sizes, 1ULL << (20 + selected)};
            found = true;
        }
    }
    return found ? ReBARResult::Found : ReBARResult::NotFound;
}

} // namespace amdgpu
