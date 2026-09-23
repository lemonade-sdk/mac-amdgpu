#include "transport.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <new>

namespace mac_hsa {
namespace {
class IOObject {
public:
    explicit IOObject(io_object_t value) : value_(value) {}
    ~IOObject() { if (value_) IOObjectRelease(value_); }
    IOObject(const IOObject &) = delete;
    IOObject &operator=(const IOObject &) = delete;
private:
    io_object_t value_;
};

class IOKitConnection final : public Connection {
public:
    io_connect_t port = IO_OBJECT_NULL;
    uint64_t registryID = 0;
    ~IOKitConnection() override { if (port) IOServiceClose(port); }

    hsa_status_t call(uint32_t selector, const uint64_t *input, uint32_t inputs,
                      uint64_t *output, uint32_t outputs) {
        uint32_t count = outputs;
        const auto result = IOConnectCallScalarMethod(port, selector, input, inputs,
                                                      output, &count);
        if (result == kIOReturnNoDevice || result == kIOReturnNotAttached ||
            result == MACH_SEND_INVALID_DEST) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (result != KERN_SUCCESS || count != outputs) return HSA_STATUS_ERROR;
        return HSA_STATUS_SUCCESS;
    }

    hsa_status_t read(DeviceSnapshot &snapshot) override {
        uint64_t identity[3]{};
        auto status = call(43, nullptr, 0, identity, 3);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (identity[0] != 0x414d444750554142ull || identity[1] != 1 || identity[2] < 172)
            return HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS;
        uint64_t gfx[3]{}, vram[2]{}, stage = 0, tag = 1;
        status = call(21, &tag, 1, gfx, 3);
        if (status != HSA_STATUS_SUCCESS) return status;
        tag = 2;
        status = call(21, &tag, 1, vram, 2);
        if (status != HSA_STATUS_SUCCESS) return status;
        tag = 4;
        status = call(21, &tag, 1, &stage, 1);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (gfx[0] > UINT32_MAX || gfx[1] > UINT32_MAX || gfx[2] > UINT32_MAX ||
            vram[0] > vram[1]) return HSA_STATUS_ERROR;
        snapshot = {registryID, identity[2], stage, vram[0], vram[1],
                    uint32_t(gfx[0]), uint32_t(gfx[1]), uint32_t(gfx[2])};
        return HSA_STATUS_SUCCESS;
    }
};
} // namespace

hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto matching = IOServiceNameMatching("MacAMDGPU");
    if (!matching) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        return HSA_STATUS_ERROR;
    IOObject iteratorOwner(iterator);
    try {
        for (io_service_t service; (service = IOIteratorNext(iterator));) {
            IOObject serviceOwner(service);
            auto connection = std::make_shared<IOKitConnection>();
            if (IORegistryEntryGetRegistryEntryID(service, &connection->registryID) != KERN_SUCCESS)
                return HSA_STATUS_ERROR;
            if (IOServiceOpen(service, mach_task_self(), 0, &connection->port) != KERN_SUCCESS)
                return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            DeviceSnapshot snapshot;
            const auto status = connection->read(snapshot);
            if (status != HSA_STATUS_SUCCESS) return status;
            connections.push_back(std::move(connection));
        }
    } catch (const std::bad_alloc &) {
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    return HSA_STATUS_SUCCESS;
}
} // namespace mac_hsa
