#pragma once

#include <stdint.h>

namespace amdgpu {
struct DeviceContext;

// Caller serializes SMUIO register access. Read the gfx1201 GPU timestamp
// frequency from ATOM firmware; return false/zero for unknown or invalid data.
enum class TimestampClockStage : uint32_t {
    IPVersion, RegisterBase, ROMOffset, ROMIndex, ROMHeader, VendorSignature,
    ATOMHeader, MasterTable, GFXTable, GFXVersion, ReferenceClock, Complete
};
struct TimestampClockDiagnostics {
    TimestampClockStage stage = TimestampClockStage::IPVersion;
    uint32_t romOffset = 0, readOffset = 0, readValue = 0;
    uint32_t atomOffset = 0, masterOffset = 0, gfxOffset = 0, gfxVersion = 0;
};
bool gfx1201_timestamp_frequency_hz(const DeviceContext &dev, uint64_t &frequency,
                                  TimestampClockDiagnostics *diagnostics = nullptr);

namespace clock_detail {
// Linux nbif_6_3_1_offset.h / sh_mask.h. Discovery NBIO6.3.1 and
// NBIO7.11.4 select this function table; older NBIO7.11 has no ROM offset.
constexpr uint32_t kNBIFROMOffsetControl = 0xcc23;
constexpr int kNBIFROMOffsetBase = 5;
constexpr uint32_t kNBIFROMOffsetMask = 0x7f;
constexpr uint32_t kROMIndex = 0x00e4;
constexpr uint32_t kROMData = 0x00e5;
constexpr uint32_t kATOMHeaderPointer = 0x48;
constexpr uint32_t kATOMMasterDataPointer = 0x20;
constexpr uint32_t kATOMGFXInfoPointer = 4 + 14 * 2;
constexpr uint32_t kATOMGoldenTSCReference = 40;

// The callback reads one little-endian dword at an aligned ROM byte offset.
// Bounds are checked before every read, including nested table lengths. This
// permits small indexed MMIO reads instead of allocating/copying the whole ROM.
template<class ReadDword> class ROMReader {
public:
    explicit ROMReader(ReadDword &read, TimestampClockDiagnostics &diag) : read_(read), diag_(diag) {}
    uint32_t limit = 4;
    bool read(uint32_t offset, uint32_t count, uint32_t &out) {
        out = 0;
        if (!count || count > 4 || offset > limit || count > limit - offset) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto aligned = (offset + i) & ~uint32_t(3);
            if (cachedOffset_ != aligned) {
                diag_.readOffset = aligned;
                const bool success = read_(aligned, cachedValue_);
                diag_.readValue = cachedValue_;
                if (!success) return false;
                cachedOffset_ = aligned;
            }
            out |= ((cachedValue_ >> (((offset + i) & 3) * 8)) & 255u) << (i * 8);
        }
        return true;
    }
    bool table(uint32_t offset, uint32_t required, uint32_t &version) {
        uint32_t size = 0;
        return offset && read(offset, 2, size) && size >= required &&
            offset <= limit && size <= limit - offset && read(offset + 2, 2, version);
    }
private:
    ReadDword &read_;
    TimestampClockDiagnostics &diag_;
    uint32_t cachedOffset_ = UINT32_MAX, cachedValue_ = 0;
};
} // namespace clock_detail

// Linux references: amdgpu_bios.c ROM signature/header, atomfirmware.h data
// table layout, amdgpu_atomfirmware_get_clock_info() GFX v3/v2.6 reference clock.
// The reference is in 10 kHz; amdgpu_kms.c exports xclk*10 kHz and ROCr converts
// that clock to Hz. This is the timestamp clock, not a dynamic shader frequency.
template<class ReadDword>
bool atom_gfx_timestamp_frequency_hz(ReadDword read, uint64_t &frequency,
                                    TimestampClockDiagnostics *diagnostics = nullptr) {
    using namespace clock_detail;
    frequency = 0;
    TimestampClockDiagnostics local;
    auto &diag = diagnostics ? *diagnostics : local;
    diag.stage = TimestampClockStage::ROMHeader;
    ROMReader<ReadDword> rom(read, diag);
    uint32_t signature = 0, units = 0;
    if (!rom.read(0, 2, signature) || signature != 0xaa55 ||
        !rom.read(2, 1, units) || !units) return false;
    rom.limit = units * 512;
    diag.stage = TimestampClockStage::VendorSignature;
    constexpr char vendor[] = " 761295520";
    for (uint32_t i = 0; i < sizeof(vendor) - 1; ++i) {
        uint32_t byte = 0;
        if (!rom.read(0x30 + i, 1, byte) || byte != uint32_t(vendor[i])) return false;
    }
    uint32_t header = 0, version = 0, master = 0, gfx = 0;
    diag.stage = TimestampClockStage::ATOMHeader;
    if (!rom.read(kATOMHeaderPointer, 2, header)) return false;
    diag.atomOffset = header;
    if (!rom.table(header, kATOMMasterDataPointer + 2, version) ||
        !rom.read(header + 4, 4, signature) ||
        (signature != 0x4d4f5441 && signature != 0x41544f4d)) return false;
    diag.stage = TimestampClockStage::MasterTable;
    if (!rom.read(header + kATOMMasterDataPointer, 2, master)) return false;
    diag.masterOffset = master;
    if (!rom.table(master, kATOMGFXInfoPointer + 2, version)) return false;
    diag.stage = TimestampClockStage::GFXTable;
    if (!rom.read(master + kATOMGFXInfoPointer, 2, gfx)) return false;
    diag.gfxOffset = gfx;
    if (!rom.table(gfx, kATOMGoldenTSCReference + 4, version)) return false;
    diag.gfxVersion = version;
    diag.stage = TimestampClockStage::GFXVersion;
    const auto major = version & 255u, minor = version >> 8;
    if (major != 3 && !(major == 2 && minor == 6)) return false;
    diag.stage = TimestampClockStage::ReferenceClock;
    uint32_t reference = 0;
    if (!rom.read(gfx + kATOMGoldenTSCReference, 4, reference) ||
        !reference || reference == UINT32_MAX) return false;
    frequency = uint64_t(reference) * 10000;
    diag.stage = TimestampClockStage::Complete;
    return true;
}
} // namespace amdgpu
