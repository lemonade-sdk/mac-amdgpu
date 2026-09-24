#pragma once

#include <stddef.h>
#include <stdint.h>

namespace amdgpu {

// Decodes the SMU 14.0.3 table used by Linux smu_v14_0_2_ppt.c.
// No MMIO, mailbox commands, allocations or assumptions about CPU mapping.
// Call only after a successful, coherent copy of the complete firmware table.
namespace metrics {
constexpr uint32_t kDriverInterface = 0x2e;
constexpr uint32_t kCompatibleInterface = 0x33;
constexpr uint32_t kCompatibleFirmware = 0x00684c00; // 104.76.0, pinned smu_14_0_3.bin.
constexpr bool linux_compatible(uint32_t interface, uint32_t firmware) {
    return interface == kCompatibleInterface && firmware == kCompatibleFirmware;
}
constexpr uint32_t kFirmwareTable = 5;
constexpr size_t kTableBytes = 412;
constexpr bool verified_interface(uint32_t version) { return version == kDriverInterface; }

enum Field : uint32_t {
    GfxActivityPercent, UmcActivityPercent, MediaActivityPercent,
    GfxClockMHz, MemoryClockMHz, SocClockMHz, FabricClockMHz,
    SocketPowerMilliwatts, BoardPowerMilliwatts,
    EdgeTemperatureMillicelsius, HotspotTemperatureMillicelsius,
    MemoryTemperatureMillicelsius, FanRPM, FanPWMPercent,
    GfxVoltageMillivolts, SocVoltageMillivolts, Count
};

struct Sample {
    uint64_t valid = 0;
    uint32_t firmwareCounter = 0;
    uint64_t value[Count]{};
    bool has(Field field) const { return (valid & (uint64_t(1) << field)) != 0; }
};

inline bool decode(const void *bytes, size_t length, uint32_t smuMajor,
                   uint32_t smuMinor, uint32_t smuRevision,
                   uint32_t driverInterface, Sample &sample, uint32_t firmwareVersion = 0) {
    sample = {};
    if (!bytes || length != kTableBytes || smuMajor != 14 || smuMinor != 0 ||
        smuRevision != 3 || (!verified_interface(driverInterface) &&
        !linux_compatible(driverInterface, firmwareVersion))) return false;
    const auto *p = static_cast<const uint8_t *>(bytes);
    auto u16 = [p](size_t i) -> uint32_t { return p[i] | (uint32_t(p[i + 1]) << 8); };
    auto u32 = [p](size_t i) -> uint32_t {
        return p[i] | (uint32_t(p[i + 1]) << 8) |
               (uint32_t(p[i + 2]) << 16) | (uint32_t(p[i + 3]) << 24);
    };
    bool allZero = true, allOnes = true;
    for (size_t i = 0; i < length; ++i) {
        allZero &= p[i] == 0;
        allOnes &= p[i] == 0xff;
    }
    if (allZero || allOnes) return false;
    auto set = [&sample](Field field, uint32_t raw, uint32_t maximum,
                         uint32_t scale = 1) {
        if (raw > maximum) return;
        sample.value[field] = uint64_t(raw) * scale;
        sample.valid |= uint64_t(1) << field;
    };
    // Offsets are checked against the pinned Linux firmware ABI by tests.
    // Firmware uint16 fields use 0xffff as unavailable; percentages outside
    // 0..100 are unavailable too, including the UCLK signed-underflow quirk.
    const uint32_t gfxActivity = u16(124), umcActivity = u16(126);
    set(GfxActivityPercent, gfxActivity, 100);
    set(UmcActivityPercent, umcActivity, 100);
    const uint32_t media0 = u16(128), media1 = u16(130);
    if (media0 <= 100 && media1 <= 100)
        set(MediaActivityPercent, media0 > media1 ? media0 : media1, 100);
    // Match Linux's pre/post deep-sleep average selection (busy > 5%).
    if (sample.has(GfxActivityPercent))
        set(GfxClockMHz, u16(gfxActivity <= 5 ? 48 : 46), 0xfffe);
    if (sample.has(UmcActivityPercent)) {
        set(MemoryClockMHz, u16(umcActivity <= 5 ? 56 : 54), 0xfffe);
        set(FabricClockMHz, u16(umcActivity <= 5 ? 52 : 50), 0xfffe);
    }
    set(SocClockMHz, u32(4), 0xfffe);
    // The raw table uses whole watts. Linux's sensor API shifts socket power
    // by eight to expose a separate Q8 value; do not apply Q8 twice here.
    set(SocketPowerMilliwatts, u16(136), 0xfffe, 1000);
    set(BoardPowerMilliwatts, u16(138), 0xfffe, 1000);
    set(EdgeTemperatureMillicelsius, u16(140), 0xfffe, 1000);
    set(HotspotTemperatureMillicelsius, u16(142), 0xfffe, 1000);
    set(MemoryTemperatureMillicelsius, u16(148), 0xfffe, 1000);
    set(FanRPM, u16(170), 0xfffe);
    set(FanPWMPercent, p[168], 100);
    set(GfxVoltageMillivolts, u16(108), 0xfffe);
    set(SocVoltageMillivolts, u16(110), 0xfffe);
    if (linux_compatible(driverInterface, firmwareVersion)) {
        // Conservative plausibility filters are safeguards, not proof of a new
        // schema. This profile uses Linux's backwards-compatible 0x2e layout.
        for (uint32_t i = 0; i < Count; ++i) {
            uint64_t max = UINT64_MAX;
            if (i >= GfxClockMHz && i <= FabricClockMHz) max = 10000;
            if (i == SocketPowerMilliwatts || i == BoardPowerMilliwatts) max = 2000000;
            if (i >= EdgeTemperatureMillicelsius && i <= MemoryTemperatureMillicelsius) max = 150000;
            if (i == FanRPM) max = 30000;
            if (i == GfxVoltageMillivolts || i == SocVoltageMillivolts) max = 2500;
            if (sample.value[i] > max) { sample.valid &= ~(uint64_t(1) << i); sample.value[i] = 0; }
        }
        // Require multiple independent anchors before publishing this profile.
        if (!sample.has(GfxActivityPercent) || !sample.has(GfxClockMHz) ||
            !sample.has(EdgeTemperatureMillicelsius) || !sample.has(SocketPowerMilliwatts)) {
            sample = {};
            return false;
        }
    }
    sample.firmwareCounter = u32(104);
    return true;
}
} // namespace metrics
} // namespace amdgpu
