#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "amdgpu_metrics.h"
#include "smu14_driver_if_v14_0.h"

using namespace amdgpu::metrics;
static_assert(sizeof(SmuMetricsExternal_t) == kTableBytes);
static_assert(TABLE_SMU_METRICS == kFirmwareTable);
static_assert(offsetof(SmuMetrics_t, AverageGfxclkFrequencyPreDs) == 46);
static_assert(offsetof(SmuMetrics_t, AverageGfxclkFrequencyPostDs) == 48);
static_assert(offsetof(SmuMetrics_t, AverageFclkFrequencyPreDs) == 50);
static_assert(offsetof(SmuMetrics_t, AverageFclkFrequencyPostDs) == 52);
static_assert(offsetof(SmuMetrics_t, AverageMemclkFrequencyPreDs) == 54);
static_assert(offsetof(SmuMetrics_t, AverageMemclkFrequencyPostDs) == 56);
static_assert(offsetof(SmuMetrics_t, MetricsCounter) == 104);
static_assert(offsetof(SmuMetrics_t, AvgVoltage) == 108);
static_assert(offsetof(SmuMetrics_t, AverageGfxActivity) == 124);
static_assert(offsetof(SmuMetrics_t, AverageUclkActivity) == 126);
static_assert(offsetof(SmuMetrics_t, AverageVcn0ActivityPercentage) == 128);
static_assert(offsetof(SmuMetrics_t, Vcn1ActivityPercentage) == 130);
static_assert(offsetof(SmuMetrics_t, AverageSocketPower) == 136);
static_assert(offsetof(SmuMetrics_t, AverageTotalBoardPower) == 138);
static_assert(offsetof(SmuMetrics_t, AvgTemperature) + TEMP_EDGE * 2 == 140);
static_assert(offsetof(SmuMetrics_t, AvgTemperature) + TEMP_HOTSPOT * 2 == 142);
static_assert(offsetof(SmuMetrics_t, AvgTemperature) + TEMP_MEM * 2 == 148);
static_assert(offsetof(SmuMetrics_t, AvgFanPwm) == 168);
static_assert(offsetof(SmuMetrics_t, AvgFanRpm) == 170);
static_assert(PPCLK_SOCCLK * 4 == 4);
static_assert(SVI_PLANE_VDD_GFX == 0 && SVI_PLANE_VDD_SOC == 1);

int main() {
    SmuMetricsExternal_t wire{};
    auto &m = wire.SmuMetrics;
    m.MetricsCounter = 17;
    m.AverageGfxActivity = 80;
    m.AverageUclkActivity = 42;
    m.AverageVcn0ActivityPercentage = 2;
    m.Vcn1ActivityPercentage = 9;
    m.AverageGfxclkFrequencyPreDs = 2100;
    m.AverageGfxclkFrequencyPostDs = 300;
    m.AverageMemclkFrequencyPreDs = 1200;
    m.AverageMemclkFrequencyPostDs = 96;
    m.AverageFclkFrequencyPreDs = 1800;
    m.AverageFclkFrequencyPostDs = 100;
    m.CurrClock[PPCLK_SOCCLK] = 1300;
    m.AverageSocketPower = 75;
    m.AverageTotalBoardPower = 96;
    m.AvgTemperature[TEMP_EDGE] = 45;
    m.AvgTemperature[TEMP_HOTSPOT] = 63;
    m.AvgTemperature[TEMP_MEM] = 57;
    m.AvgFanPwm = 25;
    m.AvgFanRpm = 1120;
    m.AvgVoltage[SVI_PLANE_VDD_GFX] = 900;
    m.AvgVoltage[SVI_PLANE_VDD_SOC] = 1000;
    Sample s;
    auto read = [&] { return decode(&wire, sizeof(wire), 14, 0, 3, 0x2e, s); };
    assert(read() && s.valid == (uint64_t(1) << Count) - 1);
    assert(s.firmwareCounter == 17 && s.value[GfxActivityPercent] == 80);
    assert(s.value[UmcActivityPercent] == 42 && s.value[MediaActivityPercent] == 9);
    assert(s.value[GfxClockMHz] == 2100 && s.value[MemoryClockMHz] == 1200);
    assert(s.value[FabricClockMHz] == 1800 && s.value[SocClockMHz] == 1300);
    assert(s.value[SocketPowerMilliwatts] == 75000 && s.value[BoardPowerMilliwatts] == 96000);
    assert(s.value[EdgeTemperatureMillicelsius] == 45000);
    assert(s.value[HotspotTemperatureMillicelsius] == 63000);
    assert(s.value[MemoryTemperatureMillicelsius] == 57000);
    assert(s.value[FanRPM] == 1120 && s.value[FanPWMPercent] == 25);
    assert(s.value[GfxVoltageMillivolts] == 900 && s.value[SocVoltageMillivolts] == 1000);
    assert(decode(&wire, sizeof(wire), 14, 0, 3, 0x33, s, kCompatibleFirmware));
    assert(!decode(&wire, sizeof(wire), 14, 0, 3, 0x33, s, kCompatibleFirmware + 1));
    m.AvgTemperature[TEMP_EDGE] = 200;
    assert(!decode(&wire, sizeof(wire), 14, 0, 3, 0x33, s, kCompatibleFirmware) && !s.valid);
    m.AvgTemperature[TEMP_EDGE] = 45;
    m.AverageTotalBoardPower = 3000;
    assert(decode(&wire, sizeof(wire), 14, 0, 3, 0x33, s, kCompatibleFirmware) && !s.has(BoardPowerMilliwatts));
    m.AverageTotalBoardPower = 96;
    m.AverageGfxActivity = 5;
    m.AverageUclkActivity = 0;
    assert(read() && s.value[GfxClockMHz] == 300 && s.value[MemoryClockMHz] == 96);
    assert(s.value[FabricClockMHz] == 100 && s.has(UmcActivityPercent));
    m.AverageUclkActivity = 0xfffe; // Linux's signed-underflow quirk.
    m.AverageGfxActivity = 101;
    m.AverageSocketPower = 0xffff;
    m.AvgFanPwm = 255;
    assert(read() && !s.has(UmcActivityPercent) && !s.has(GfxActivityPercent));
    assert(!s.has(MemoryClockMHz) && !s.has(GfxClockMHz));
    assert(!s.has(SocketPowerMilliwatts) && !s.has(FanPWMPercent));
    for (size_t n = 0; n < sizeof(wire); ++n)
        assert(!decode(&wire, n, 14, 0, 3, 0x2e, s) && s.valid == 0);
    assert(!decode(&wire, sizeof(wire), 14, 0, 2, 0x2e, s));
    assert(!decode(&wire, sizeof(wire), 14, 0, 3, 0x2f, s));
    assert(!decode(&wire, sizeof(wire), 14, 0, 3, 0x33, s) && !s.valid);
    assert(!decode(nullptr, sizeof(wire), 14, 0, 3, 0x2e, s));
    std::memset(&wire, 0, sizeof(wire));
    assert(!read() && !s.valid);
    std::memset(&wire, 0xff, sizeof(wire));
    assert(!read() && !s.valid);
    // Unaligned DMA snapshots must not require aligned C struct loads.
    uint8_t unaligned[kTableBytes + 1]{};
    unaligned[105] = 1;
    assert(decode(unaligned + 1, kTableBytes, 14, 0, 3, 0x2e, s));
    assert(s.firmwareCounter == 1);
}
