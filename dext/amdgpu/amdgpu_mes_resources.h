#pragma once
#include "amdgpu_ip.h"
#include "amdgpu_mes_packets.h"

namespace amdgpu {
// mes_v12_0_set_hw_resources supplies GC_HWIP, MMHUB_HWIP and OSSSYS_HWIP.
// GMC is a software grouping, not a discovered register block.
inline bool mes_set_register_bases(const IPBaseTable &ip, MES_SetHwResources &packet)
{
    if (!ip.isResolved(IPBlock::GC) || !ip.isResolved(IPBlock::GC, 1) ||
        !ip.isResolved(IPBlock::MMHUB) || !ip.isResolved(IPBlock::OSSSYS)) return false;
    for (int i = 0; i < 5; ++i) {
        // Unused segments remain zero in the firmware packet; do not export
        // the internal unresolved-address sentinel as an MMIO address.
        auto base = [&](IPBlock block) {
            const auto value = ip.getBase(block, i);
            return value == UINT32_MAX ? 0u : value;
        };
        packet.gc_base[i] = base(IPBlock::GC);
        packet.mmhub_base[i] = base(IPBlock::MMHUB);
        packet.osssys_base[i] = base(IPBlock::OSSSYS);
    }
    return true;
}
} // namespace amdgpu
