#include "device_init.h"
#include <array>

namespace mac_hsa {
hsa_status_t initializeDevice(InitializationRPC &rpc, bool &claimed, uint64_t &capacity) {
    claimed = false; capacity = 0;
    std::array<uint64_t, 3> build{};
    auto status = rpc.scalar(43, {}, build);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (build[0] != 0x414d444750554142ull || build[1] != 1 || build[2] < 179)
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    std::array<uint64_t, 7> identity{};
    status = rpc.scalar(1, {}, identity);
    if (status != HSA_STATUS_SUCCESS) return status;
    claimed = true;
    if (identity[3] != 0x1002 || identity[4] != 0x7551 ||
        (identity[6] != 0xc0 && identity[6] != 0xc8))
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    uint64_t tag = 4, stage = UINT64_MAX;
    status = rpc.scalar(21, {&tag, 1}, {&stage, 1});
    if (status != HSA_STATUS_SUCCESS) return status;
    if (stage != 0) return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;

    const std::string suffix = identity[6] == 0xc8 ? "_kicker.bin" : ".bin";
    const std::vector<FirmwareFile> firmware{
        {0, "psp_14_0_3_sos" + suffix}, {9, "psp_14_0_3_ta" + suffix},
        {274, "smu_14_0_3" + suffix}, {512, "sdma_7_0_1.bin"},
        {516, "gc_12_0_1_pfp.bin"}, {517, "gc_12_0_1_me.bin"},
        {518, "gc_12_0_1_mec.bin"}, {515, "gc_12_0_1_uni_mes.bin"},
        {514, "gc_12_0_1_imu" + suffix}, {513, "gc_12_0_1_rlc" + suffix}};
    // Resolve every file before allocating DMA or resetting the endpoint.
    status = rpc.prepareFirmware(firmware);
    if (status != HSA_STATUS_SUCCESS) return status;
    const std::array<uint64_t, 2> dmaInput{32ull << 20, 16384};
    std::array<uint64_t, 2> dmaOutput{};
    status = rpc.scalar(6, dmaInput, dmaOutput);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (!dmaOutput[0]) return HSA_STATUS_ERROR;
    status = rpc.scalar(8, {}, {});
    if (status != HSA_STATUS_SUCCESS) return status;
    rpc.waitAfterReset();
    const auto advance = [&](uint64_t target) {
        uint64_t reached = 0;
        auto result = rpc.scalar(9, {&target, 1}, {&reached, 1});
        if (result != HSA_STATUS_SUCCESS) return result;
        return reached == target ? HSA_STATUS_SUCCESS : HSA_STATUS_ERROR;
    };
    status = advance(1); // IFWI wait + discovery, bounded in the driver
    if (status != HSA_STATUS_SUCCESS) return status;
    tag = 3;
    std::array<uint64_t, 4> ips{};
    status = rpc.scalar(21, {&tag, 1}, ips);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (ips[1] != 0x070001 || ips[2] != 0x0e0003 || ips[3] != 0x0e0003)
        return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    tag = 1;
    std::array<uint64_t, 3> gfx{};
    status = rpc.scalar(21, {&tag, 1}, gfx);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (gfx != std::array<uint64_t, 3>{12, 0, 1}) return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
    for (uint64_t target = 2; target <= 4; ++target) {
        status = advance(target);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    status = rpc.uploadFirmware(firmware.front());
    if (status != HSA_STATUS_SUCCESS) return status;
    for (uint64_t target = 5; target <= 7; ++target) {
        status = advance(target);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    for (size_t i = 1; i < firmware.size(); ++i) {
        status = rpc.uploadFirmware(firmware[i]);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    // Each firmware RPC waits for its PSP result/fence, and stage 8 validates
    // the completed load set. No arbitrary sleep substitutes for those acks.
    for (uint64_t target = 8; target <= 15; ++target) {
        status = advance(target);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    tag = 5;
    std::array<uint64_t, 15> accounting{};
    status = rpc.scalar(21, {&tag, 1}, accounting);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (accounting[0] != 1 || !(accounting[1] & 1) || !accounting[10]) return HSA_STATUS_ERROR;
    capacity = accounting[10];
    return HSA_STATUS_SUCCESS;
}
}
