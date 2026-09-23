#include "model.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <cstdio>

namespace mtop {
namespace {
struct Object {
    io_object_t value = IO_OBJECT_NULL;
    ~Object() { if (value) IOObjectRelease(value); }
};
struct Connection {
    io_connect_t value = IO_OBJECT_NULL;
    ~Connection() { if (value) IOServiceClose(value); }
};
std::string failure(const char *operation, kern_return_t kr) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s: 0x%08x", operation, unsigned(kr));
    return buffer;
}
bool call(Connection &connection, uint32_t selector, const uint64_t *in,
          uint32_t inCount, uint64_t *out, uint32_t outCount, Device &device) {
    uint32_t actual = outCount;
    auto kr = IOConnectCallScalarMethod(connection.value, selector, in, inCount, out, &actual);
    if (kr != KERN_SUCCESS) device.error = failure("Driver query", kr);
    else if (actual != outCount) device.error = "Unexpected driver response size";
    return device.error.empty();
}
void read(io_service_t service, Device &d) {
    Connection connection;
    auto kr = IOServiceOpen(service, mach_task_self(), 0, &connection.value);
    if (kr != KERN_SUCCESS) { d.error = failure("IOServiceOpen", kr); return; }
    uint64_t identity[3]{};
    if (!call(connection, 43, nullptr, 0, identity, 3, d)) return;
    if (identity[0] != 0x414d444750554142ull || identity[1] != 1 || identity[2] < 172) {
        d.error = "Unsupported driver observer ABI (requires build 172+)";
        return;
    }
    d.build = identity[2];
    uint64_t gfx[3]{}, vram[2]{}, tag = 1;
    if (!call(connection, 21, &tag, 1, gfx, 3, d)) return;
    tag = 2;
    if (!call(connection, 21, &tag, 1, vram, 2, d)) return;
    tag = 4;
    if (!call(connection, 21, &tag, 1, &d.stage, 1, d)) return;
    if (vram[0] > vram[1] || gfx[0] > UINT32_MAX || gfx[1] > UINT32_MAX || gfx[2] > UINT32_MAX) {
        d.error = "Invalid driver information";
        return;
    }
    for (unsigned i = 0; i < 3; ++i) d.gfx[i] = uint32_t(gfx[i]);
    d.visible = vram[0];
    d.total = vram[1];
    if (d.build >= 176) {
        size_t bytes = sizeof(d.metrics);
        kr = IOConnectCallStructMethod(connection.value, 47, nullptr, 0, &d.metrics, &bytes);
        if (kr == kIOReturnUnsupported) {
            d.telemetryError = "Driver does not provide the metrics endpoint";
        } else if (kr != KERN_SUCCESS) {
            d.telemetryError = failure("Metrics snapshot", kr);
        } else if (bytes != sizeof(d.metrics) || !validSnapshot(d.metrics)) {
            d.telemetryError = "Invalid metrics snapshot ABI";
        } else {
            d.telemetrySupported = true;
        }
    }
}
} // namespace

std::vector<Device> discover(std::string &error) {
    error.clear();
    std::vector<Device> devices;
    auto match = IOServiceNameMatching("MacAMDGPU");
    if (!match) { error = "Unable to allocate IOKit matching dictionary"; return devices; }
    Object iterator;
    auto kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator.value);
    if (kr != KERN_SUCCESS) { error = failure("Device enumeration", kr); return devices; }
    for (io_service_t service; (service = IOIteratorNext(iterator.value));) {
        Object owner{service};
        Device device;
        kr = IORegistryEntryGetRegistryEntryID(service, &device.registry);
        if (kr != KERN_SUCCESS) {
            error = failure("Registry identity", kr);
            continue;
        }
        read(service, device);
        devices.push_back(std::move(device));
    }
    std::sort(devices.begin(), devices.end(), [](const Device &a, const Device &b) {
        return a.registry < b.registry;
    });
    return devices;
}
} // namespace mtop
