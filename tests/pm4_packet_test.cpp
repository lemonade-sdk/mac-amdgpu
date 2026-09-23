#include "amdgpu_pm4.h"
#include "amdgpu_ip.h"
#include "../upstream/linux/drivers/gpu/drm/amd/amdgpu/nvd.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/soc24_enum.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main()
{
    using namespace amdgpu;
    static_assert(kPM4OpSetUconfigReg == PACKET3_SET_UCONFIG_REG);
    static_assert(kPM4UconfigStart == PACKET3_SET_UCONFIG_REG_START);
    static_assert(kPM4UconfigEnd == PACKET3_SET_UCONFIG_REG_END);
    assert(pm4_header(kPM4OpSetUconfigReg, 1) == uint32_t(PACKET3(PACKET3_SET_UCONFIG_REG, 1)));
    assert(BootstrapRegs::MP0_C2PMSG_33 == 0x16061);
    assert(pm4_header(kPM4OpNop, 0) == uint32_t(PACKET3(PACKET3_NOP, 0)));
    assert(pm4_header(kPM4OpWriteData, 3) == uint32_t(PACKET3(PACKET3_WRITE_DATA, 3)));
    assert(pm4_header(kPM4OpReleaseMem, 6) == uint32_t(PACKET3(PACKET3_RELEASE_MEM, 6)));
    assert(pm4_header(0x1ff, 0x7fff) == uint32_t(PACKET3(0x1ff, 0x7fff)));
    assert(pm4_release_mem_dw1() ==
        (PACKET3_RELEASE_MEM_GCR_SEQ | PACKET3_RELEASE_MEM_GCR_GL2_WB |
         PACKET3_RELEASE_MEM_CACHE_POLICY(3) |
         PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
         PACKET3_RELEASE_MEM_EVENT_INDEX(5)));

    // Fixed expected stream from the Linux GFX12 fence emission sequence.
    // The NOP payload must not consume the following RELEASE_MEM header.
    uint32_t packet[10]{};
    const uint32_t expected[] = {
        0xc0001000, 0, 0xc0064900, 0x06600514, 0x20000000,
        0x01814000, 0x00000080, 0xdeadbeef, 0, 0,
    };
    const auto count = pm4_build_fence(packet, 0x8001814000ULL,
                                      0xdeadbeef, false, false);
    assert(count == 10);
    for (unsigned i = 0; i < count; ++i) assert(packet[i] == expected[i]);
    unsigned cursor = 0;
    for (uint32_t opcode : {uint32_t(PACKET3_NOP), uint32_t(PACKET3_RELEASE_MEM)}) {
        assert(CP_PACKET_GET_TYPE(packet[cursor]) == PACKET_TYPE3);
        assert(CP_PACKET3_GET_OPCODE(packet[cursor]) == opcode);
        cursor += CP_PACKET_GET_COUNT(packet[cursor]) + 2;
        assert(cursor <= count);
    }
    assert(cursor == count);

    pm4_build_fence(packet, 0x8001814000ULL, 0x12345678deadbeefULL, true, true);
    assert(packet[4] == (PACKET3_RELEASE_MEM_DATA_SEL(2) |
                         PACKET3_RELEASE_MEM_INT_SEL(2)));
    assert(packet[7] == 0xdeadbeef && packet[8] == 0x12345678);
    puts("GFX12 PM4 packet tests passed against local Linux definitions");
}
