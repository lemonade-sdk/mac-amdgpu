#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "amdgpu_clock.h"
#include "amdgpu_ip.h"
#include "atomfirmware.h"
#include "smuio/smuio_14_0_2_offset.h"
#include "nbif/nbif_6_3_1_offset.h"
#include "nbif/nbif_6_3_1_sh_mask.h"
using namespace amdgpu;
static_assert(offsetof(atom_rom_header_v2_2, masterdatatable_offset) == clock_detail::kATOMMasterDataPointer);
static_assert(offsetof(atom_master_data_table_v2_1, listOfdatatables) +
              offsetof(atom_master_list_of_data_tables_v2_1, gfx_info) == clock_detail::kATOMGFXInfoPointer);
static_assert(offsetof(atom_gfx_info_v3_0, golden_tsc_count_lower_refclk) == clock_detail::kATOMGoldenTSCReference);
static_assert(regROM_INDEX == clock_detail::kROMIndex && regROM_DATA == clock_detail::kROMData);
static_assert(regROM_INDEX_BASE_IDX == 0 && regROM_DATA_BASE_IDX == 0);
static_assert(regREGS_ROM_OFFSET_CTRL == clock_detail::kNBIFROMOffsetControl);
static_assert(regREGS_ROM_OFFSET_CTRL_BASE_IDX == clock_detail::kNBIFROMOffsetBase);
static_assert(REGS_ROM_OFFSET_CTRL__ROM_OFFSET_MASK == clock_detail::kNBIFROMOffsetMask);
static_assert(REGS_ROM_OFFSET_CTRL__ROM_OFFSET__SHIFT == 0);

static void put(std::vector<uint8_t> &rom, size_t offset, uint32_t value, unsigned count) {
    for (unsigned i=0;i<count;++i) rom.at(offset+i)=uint8_t(value>>(8*i));
}
static std::vector<uint8_t> fixture() {
    std::vector<uint8_t> rom(2048, 0);
    put(rom,0,0xaa55,2);rom[2]=4;
    std::memcpy(rom.data()+0x30," 761295520",10);
    put(rom,0x48,0x81,2);
    put(rom,0x81,36,2);rom[0x83]=2;rom[0x84]=2;
    std::memcpy(rom.data()+0x85,"ATOM",4);put(rom,0x81+0x20,0x123,2);
    put(rom,0x123,74,2);rom[0x125]=2;rom[0x126]=1;
    put(rom,0x123+32,0x205,2);
    put(rom,0x205,44,2);rom[0x207]=3;
    put(rom,0x205+40,10000,4);
    return rom;
}
namespace amdgpu {
struct DeviceContext {IPBaseTable ip;};
static std::vector<uint8_t> hardwareROM;
static uint32_t romIndex, writes, reads, romOffsetControl, indirectReads;
static uint32_t RREG32(const DeviceContext &dev,uint32_t reg) {
    ++reads;
    const auto base=dev.ip.get(IPBlock::SMUIO);
    if (reg==base+clock_detail::kROMIndex) return romIndex;
    assert(reg==base+clock_detail::kROMData && romIndex%4==0);
    const uint32_t imageOffset=(romOffsetControl&clock_detail::kNBIFROMOffsetMask)<<17;
    if (romIndex<imageOffset) return UINT32_MAX;
    const auto offset=romIndex-imageOffset;
    if (offset>=hardwareROM.size() || hardwareROM.size()-offset<4) return UINT32_MAX;
    uint32_t value=0;
    for(unsigned i=0;i<4;++i) value|=uint32_t(hardwareROM[offset+i])<<(8*i);
    romIndex+=4;return value;
}
static uint32_t SMN_RREG32(const DeviceContext &dev,uint32_t reg) {
    assert(reg==dev.ip.getBase(IPBlock::NBIO,clock_detail::kNBIFROMOffsetBase)+clock_detail::kNBIFROMOffsetControl);
    ++indirectReads;return romOffsetControl;
}
static void WREG32(const DeviceContext &dev,uint32_t reg,uint32_t value) {
    assert(reg==dev.ip.get(IPBlock::SMUIO)+clock_detail::kROMIndex);
    ++writes;romIndex=value;
}
}
template<class... T> void discardClockLog(T...) {}
#define CLOCK_LOG(...) discardClockLog(__VA_ARGS__)
#include "gpu_clock_reader_under_test.inc"

static bool parse(const std::vector<uint8_t> &rom,uint64_t &hz) {
    return atom_gfx_timestamp_frequency_hz([&](uint32_t offset,uint32_t &value) {
        assert(offset%4==0);
        if (offset>rom.size() || rom.size()-offset<4) return false;
        value=0;for(unsigned i=0;i<4;++i) value|=uint32_t(rom[offset+i])<<(8*i);
        return true;
    },hz);
}
static void rejects(const std::vector<uint8_t> &rom) {
    uint64_t hz=123;assert(!parse(rom,hz) && hz==0);
}
int main() {
    auto good=fixture();uint64_t hz=0;
    assert(parse(good,hz) && hz==100000000);
    for (const auto version:{0x0003u,0x0103u,0x0602u}) {
        auto rom=good;put(rom,0x207,version,2);
        assert(parse(rom,hz) && hz==100000000);
    }
    auto rom=good;std::memcpy(rom.data()+0x85,"MOTA",4);assert(parse(rom,hz));
    for (const auto offset:{0u,0x30u,0x85u}) {rom=good;rom[offset]^=1;rejects(rom);}
    rom=good;rom[2]=0;rejects(rom);
    rom=good;rom[2]=1;rejects(rom); // gfx table outside declared ROM
    for (const auto offset:{0x48u,0x81u+0x20u,0x123u+32u}) {
        rom=good;put(rom,offset,0,2);rejects(rom);
        rom=good;put(rom,offset,2047,2);rejects(rom);
    }
    for (const auto offset:{0x81u,0x123u,0x205u}) {
        rom=good;put(rom,offset,4,2);rejects(rom);
        rom=good;put(rom,offset,2048,2);rejects(rom);
    }
    rom=good;put(rom,0x207,0x0502,2);rejects(rom);
    rom=good;put(rom,0x205+40,0,4);rejects(rom);
    rom=good;put(rom,0x205+40,UINT32_MAX,4);rejects(rom);
    for(size_t n=0;n<good.size();++n) {
        // It is valid to omit unread ROM tail; truncation before the clock isn't.
        if(n<0x205+44) rejects(std::vector<uint8_t>(good.begin(),good.begin()+n));
    }
    DeviceContext dev;
    dev.ip.setVersion(IPBlock::GC,{12,0,1});
    dev.ip.setVersion(IPBlock::NBIO,{7,11,3});
    dev.ip.setVersion(IPBlock::SMUIO,{14,0,2});dev.ip.set(IPBlock::SMUIO,0x5a0);
    hardwareROM=good;romIndex=0x100;
    assert(gfx1201_timestamp_frequency_hz(dev,hz) && hz==100000000 && romIndex==0x100);
    assert(reads && writes && writes<40);
    hardwareROM[0]=0;romIndex=0x104;
    assert(!gfx1201_timestamp_frequency_hz(dev,hz) && !hz && romIndex==0x104);
    hardwareROM=good;reads=writes=0;
    TimestampClockDiagnostics diag;
    // Actual R9700 discovery uses NBIO6.3.1, BASE_IDX5=0x04040000.
    // Cover both NBIF selectors, nonzero image banks and unrelated bits.
    dev.ip.setBase(IPBlock::NBIO,5,0x04040000);
    for (const auto version:{IPVersion{6,3,1},IPVersion{7,11,4}}) {
        dev.ip.setVersion(IPBlock::NBIO,version);
        for(uint32_t bank:{0u,1u,127u}) {
            romOffsetControl=0xabcd0000|bank;romIndex=0x1234;
            const auto before=indirectReads;
            assert(gfx1201_timestamp_frequency_hz(dev,hz,&diag) && hz==100000000);
            assert(diag.stage==TimestampClockStage::Complete && diag.romOffset==(bank<<17));
            assert(indirectReads==before+1 && romIndex==0x1234);
        }
    }
    romOffsetControl=UINT32_MAX;romIndex=0x1240;
    assert(!gfx1201_timestamp_frequency_hz(dev,hz,&diag) && !hz);
    assert(diag.stage==TimestampClockStage::ROMOffset && romIndex==0x1240);
    dev.ip.setBase(IPBlock::NBIO,5,UINT32_MAX);romOffsetControl=0;
    assert(!gfx1201_timestamp_frequency_hz(dev,hz,&diag) && diag.stage==TimestampClockStage::RegisterBase);
    dev.ip.setBase(IPBlock::NBIO,5,0x04040000);
    dev.ip.setVersion(IPBlock::NBIO,{6,3,2});
    assert(!gfx1201_timestamp_frequency_hz(dev,hz,&diag) && diag.stage==TimestampClockStage::IPVersion);
    dev.ip.setVersion(IPBlock::NBIO,{6,3,1});
    hardwareROM[0]=0;
    assert(!gfx1201_timestamp_frequency_hz(dev,hz,&diag) && diag.stage==TimestampClockStage::ROMHeader && diag.readOffset==0);
    hardwareROM=good;hardwareROM[0x207]=4;
    assert(!gfx1201_timestamp_frequency_hz(dev,hz,&diag) && diag.stage==TimestampClockStage::GFXVersion && diag.gfxVersion==4);
    hardwareROM=good;reads=writes=0;
    dev.ip.setVersion(IPBlock::SMUIO,{14,0,1});
    assert(!gfx1201_timestamp_frequency_hz(dev,hz) && !hz && !reads && !writes);
    dev.ip.setVersion(IPBlock::SMUIO,{14,0,2});dev.ip.set(IPBlock::SMUIO,UINT32_MAX);
    assert(!gfx1201_timestamp_frequency_hz(dev,hz) && !reads && !writes);
    puts("GPU clock: Linux ATOM layout, bounded ROM parsing, clock units, NBIF6.3.1 ROM banks, failure stages and index restoration pass");
}
