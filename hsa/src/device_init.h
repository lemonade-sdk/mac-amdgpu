#pragma once
#include "transport.h"
#include <span>

namespace mac_hsa {
struct FirmwareFile { uint64_t type; std::string name; };
class InitializationRPC {
public:
    virtual ~InitializationRPC() = default;
    virtual hsa_status_t scalar(uint32_t selector, std::span<const uint64_t> input,
                                std::span<uint64_t> output) = 0;
    virtual hsa_status_t prepareFirmware(const std::vector<FirmwareFile> &files) = 0;
    virtual hsa_status_t uploadFirmware(const FirmwareFile &file) = 0;
    virtual void waitAfterReset() = 0;
};
// Caller serializes the session. Acquiring identity must succeed before reset;
// a stage cached by another owner is never treated as this session's readiness.
hsa_status_t initializeDevice(InitializationRPC &rpc, bool &claimed, uint64_t &capacity, bool allowInitialize = true);
}
