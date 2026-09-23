#include "device_init.h"
#include <cassert>
#include <cstdio>
#include <array>

struct RPC final : mac_hsa::InitializationRPC {
    std::vector<std::string> events;
    size_t failAt = SIZE_MAX;
    uint64_t build = 179;
    bool busy = false, kicker = false, stale = false, wrongIP = false, shortStage = false;
    hsa_status_t event(const std::string &name) {
        events.push_back(name);
        return events.size() == failAt ? HSA_STATUS_ERROR : HSA_STATUS_SUCCESS;
    }
    hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> input, std::span<uint64_t> out) override {
        auto name = std::to_string(selector);
        if (input.size()) name += ":" + std::to_string(input[0]);
        const auto status = event(name);
        if (status != HSA_STATUS_SUCCESS) return status;
        switch (selector) {
        case 43: assert(out.size() == 3); out[0] = 0x414d444750554142ull; out[1] = 1; out[2] = build; break;
        case 1:
            if (busy) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            assert(out.size() == 7); out[3] = 0x1002; out[4] = 0x7551; out[6] = kicker ? 0xc8 : 0xc0; break;
        case 21:
            if (input[0] == 4) out[0] = stale ? 15 : 0;
            else if (input[0] == 3) { assert(out.size() == 4); out[1] = wrongIP ? 0x070000 : 0x070001; out[2] = out[3] = 0x0e0003; }
            else if (input[0] == 1) { assert(out.size() == 3); out[0] = 12; out[1] = 0; out[2] = 1; }
            else { assert(input[0] == 5 && out.size() == 15); out[0] = out[1] = 1; out[10] = 31ull << 30; }
            break;
        case 6: assert(input.size() == 2 && input[0] == 32ull << 20 && input[1] == 16384); out[0] = 1; out[1] = 0x80000000; break;
        case 8: assert(input.empty() && out.empty()); break;
        case 9: assert(out.size() == 1); out[0] = shortStage ? 0 : input[0]; break;
        default: assert(false);
        }
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t prepareFirmware(const std::vector<mac_hsa::FirmwareFile> &files) override {
        assert(files.size() == 10 && files.front().type == 0 && files.back().type == 513);
        assert(files.front().name == (kicker ? "psp_14_0_3_sos_kicker.bin" : "psp_14_0_3_sos.bin"));
        return event("prepare");
    }
    hsa_status_t uploadFirmware(const mac_hsa::FirmwareFile &file) override { return event("fw:" + std::to_string(file.type)); }
    void waitAfterReset() override { events.push_back("wait150"); }
};
int main() {
    RPC rpc; bool claimed; uint64_t capacity;
    assert(mac_hsa::initializeDevice(rpc, claimed, capacity) == 0 && claimed && capacity == 31ull << 30);
    const std::vector<std::string> expected{
        "43", "1", "21:4", "prepare", "6:33554432", "8", "wait150", "9:1", "21:3", "21:1",
        "9:2", "9:3", "9:4", "fw:0", "9:5", "9:6", "9:7", "fw:9", "fw:274", "fw:512",
        "fw:516", "fw:517", "fw:518", "fw:515", "fw:514", "fw:513", "9:8", "9:9", "9:10",
        "9:11", "9:12", "9:13", "9:14", "9:15", "21:5"};
    assert(rpc.events == expected);
    for (size_t i = 1; i <= expected.size(); ++i) {
        if (expected[i - 1] == "wait150") continue;
        RPC failure; failure.failAt = i;
        assert(mac_hsa::initializeDevice(failure, claimed, capacity) != 0);
        assert(failure.events.size() == i && !capacity);
        assert(claimed == (i > 2));
    }
    RPC busy; busy.busy = true;
    assert(mac_hsa::initializeDevice(busy, claimed, capacity) == HSA_STATUS_ERROR_OUT_OF_RESOURCES && !claimed);
    assert((busy.events == std::vector<std::string>{"43", "1"}));
    RPC stale; stale.stale = true;
    assert(mac_hsa::initializeDevice(stale, claimed, capacity) != 0 && claimed && stale.events.size() == 3);
    RPC shared; shared.build = 180; shared.stale = true;
    assert(mac_hsa::initializeDevice(shared, claimed, capacity) == 0 && claimed && capacity == 31ull << 30);
    const std::vector<std::string> sharedExpected{"43", "1", "21:4", "21:3", "21:1", "21:5"};
    assert(shared.events == sharedExpected);
    for (size_t i = 1; i <= sharedExpected.size(); ++i) {
        RPC failure; failure.build = 180; failure.stale = true; failure.failAt = i;
        assert(mac_hsa::initializeDevice(failure, claimed, capacity) != 0 && !capacity);
        assert(failure.events.size() == i);
    }
    RPC kicker; kicker.kicker = true;
    assert(mac_hsa::initializeDevice(kicker, claimed, capacity) == 0);
    RPC wrong; wrong.wrongIP = true;
    assert(mac_hsa::initializeDevice(wrong, claimed, capacity) != 0 && wrong.events.back() == "21:3");
    RPC shortStage; shortStage.shortStage = true;
    assert(mac_hsa::initializeDevice(shortStage, claimed, capacity) != 0 && shortStage.events.back() == "9:1");
    puts("HSA: exact firmware/stage transcript, every injected failure, owner Busy, stale stage, kicker and IP validation pass");
}
