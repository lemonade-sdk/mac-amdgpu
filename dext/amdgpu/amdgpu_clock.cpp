#include "amdgpu_clock.h"
#include "amdgpu_regs.h"
#include <os/log.h>
#define CLOCK_LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "mac.amdgpu.clock: " fmt, ##__VA_ARGS__)

namespace amdgpu {
bool gfx1201_timestamp_frequency_hz(const DeviceContext &dev, uint64_t &frequency,
                                  TimestampClockDiagnostics *diagnostics) {
    frequency = 0;
    TimestampClockDiagnostics local;
    auto &diag = diagnostics ? *diagnostics : local;
    diag = {};
    const auto nbio = dev.ip.getVersion(IPBlock::NBIO);
    const auto smuio = dev.ip.getVersion(IPBlock::SMUIO);
    const auto finish = [&](bool success) {
        CLOCK_LOG("ATOM timestamp: ok=%u stage=%u NBIO=%u.%u.%u SMUIO=%u.%u.%u "
                  "rom_offset=%#x last_read=%#x:%#x atom=%#x master=%#x gfx=%#x version=%#x hz=%llu",
                  unsigned(success), unsigned(diag.stage), nbio.major, nbio.minor, nbio.rev,
                  smuio.major, smuio.minor, smuio.rev, diag.romOffset, diag.readOffset,
                  diag.readValue, diag.atomOffset, diag.masterOffset, diag.gfxOffset,
                  diag.gfxVersion, (unsigned long long)frequency);
        return success;
    };
    // Match Linux amdgpu_discovery_set_nbio_ip_blocks. NBIO7.11.4 uses
    // nbif_v6_3_1_funcs too; it does not share the zero-offset 7.11.0-3 path.
    const bool nbif = (nbio.major == 6 && nbio.minor == 3 && nbio.rev == 1) ||
                     (nbio.major == 7 && nbio.minor == 11 && nbio.rev == 4);
    const bool zeroOffset = nbio.major == 7 && nbio.minor == 11 && nbio.rev <= 3;
    if (!dev.ip.isVersion(IPBlock::GC, 12, 0, 1) ||
        !dev.ip.isVersion(IPBlock::SMUIO, 14, 0, 2) || (!nbif && !zeroOffset))
        return finish(false);
    diag.stage = TimestampClockStage::RegisterBase;
    if (!dev.ip.isResolved(IPBlock::SMUIO)) return finish(false);
    const auto base = dev.ip.get(IPBlock::SMUIO);
    if (base > UINT32_MAX - clock_detail::kROMData) return finish(false);
    if (nbif) {
        const auto segment = clock_detail::kNBIFROMOffsetBase;
        if (!dev.ip.isResolved(IPBlock::NBIO, segment)) return finish(false);
        const auto nbioBase = dev.ip.getBase(IPBlock::NBIO, segment);
        if (nbioBase > (UINT32_MAX >> 2) - clock_detail::kNBIFROMOffsetControl)
            return finish(false);
        diag.stage = TimestampClockStage::ROMOffset;
        // BASE_IDX5 lives beyond BAR5: use the existing serialized SMN index
        // path, matching Linux's indirect register access for this address.
        diag.readOffset = nbioBase + clock_detail::kNBIFROMOffsetControl;
        diag.readValue = SMN_RREG32(dev, diag.readOffset);
        if (diag.readValue == UINT32_MAX) return finish(false);
        diag.romOffset = (diag.readValue & clock_detail::kNBIFROMOffsetMask) << 17;
    }
    const auto index = base + clock_detail::kROMIndex;
    const auto data = base + clock_detail::kROMData;
    diag.stage = TimestampClockStage::ROMIndex;
    const auto previousIndex = RREG32(dev, index);
    if (previousIndex == UINT32_MAX) return finish(false);
    const auto read = [&](uint32_t offset, uint32_t &value) {
        if (offset > UINT32_MAX - diag.romOffset) return false;
        WREG32(dev, index, diag.romOffset + offset);
        value = RREG32(dev, data);
        return value != UINT32_MAX;
    };
    const bool result = atom_gfx_timestamp_frequency_hz(read, frequency, &diag);
    WREG32(dev, index, previousIndex);
    return finish(result);
}
} // namespace amdgpu
