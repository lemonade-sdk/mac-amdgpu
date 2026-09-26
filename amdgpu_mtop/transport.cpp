#include "model.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <cstdio>
#include <map>
#include <time.h>

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
    if (d.build >= 193 && d.stage == 15) {
        // Per-registry rate limiting is independent of the dashboard refresh.
        // The driver also shares a one-second cache across all observers.
        static std::map<uint64_t, uint64_t> attempts;
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        auto &last = attempts[d.registry];
        if (!last || now < last || now - last >= 1000000000ull) {
            last = now;
            uint64_t result[3]{};
            uint32_t count = 3;
            // Errors remain in the cached sensor snapshot; Busy simply skips
            // this sample. Never affect the independent software counters.
            (void)IOConnectCallScalarMethod(connection.value, 63, nullptr, 0, result, &count);
        }
    }
    if (d.build >= amdgpu::software_stats::kMinimumBuild) {
        size_t bytes = sizeof(d.software);
        kr = IOConnectCallStructMethod(connection.value, amdgpu::software_stats::kSelector,
                                        nullptr, 0, &d.software, &bytes);
        if (kr != KERN_SUCCESS) d.softwareError = failure("Software counters", kr);
        else if (bytes != sizeof(d.software) || !amdgpu::software_stats::valid(d.software))
            d.softwareError = "Invalid software counter ABI";
        else d.softwareSupported = true;
    }
    if (d.build >= 193) {
        size_t bytes = sizeof(d.clocks);
        kr = IOConnectCallStructMethod(connection.value, amdgpu::kSMUClockSelector,
                                        nullptr, 0, &d.clocks, &bytes);
        if (kr != KERN_SUCCESS) d.clocksError = failure("Clock snapshot", kr);
        else if (bytes != sizeof(d.clocks) || d.clocks.version != 1 ||
                 d.clocks.size != sizeof(d.clocks) ||
                 (d.clocks.currentValid & ~15u) || (d.clocks.limitsValid & ~15u))
            d.clocksError = "Invalid clock snapshot ABI";
        else d.clocksSupported = true;
    }
    // Read-only observer device spec (GFXSpecSnapshot): chip geometry for the
    // header. kIOReturnNotReady is normal while the GPU is stopped (stage 0).
    // The driver returns the 32 dwords as uint64 scalar outputs.
    {
        uint64_t tag8 = 8, specWords64[32]{};
        uint32_t specCount = 32;
        if (IOConnectCallScalarMethod(connection.value, 21, &tag8, 1, specWords64, &specCount) == KERN_SUCCESS &&
            specCount == 32) {
            uint32_t words[32]{};
            for (int i = 0; i < 32; ++i) words[i] = (uint32_t)specWords64[i];
            if (words[0] != 0) mtop::readGfxSpec(d, words);
        }
    }
    if (d.build >= 178) {
        tag = 5;
        uint32_t count = amdgpu::vram_accounting::Count;
        kr = IOConnectCallScalarMethod(connection.value, 21, &tag, 1,
                                       d.accounting.values, &count);
        if (kr != KERN_SUCCESS) d.accountingError = failure("VRAM accounting", kr);
        else if (count != amdgpu::vram_accounting::Count ||
                 !amdgpu::vram_accounting::valid(d.accounting))
            d.accountingError = "Invalid VRAM accounting ABI";
        else d.accountingSupported = true;
    }
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
    // MMHUB PERFSTATUS UMC busy accumulator (build 198+, selector 68).
    // Observer-only read of the driver's sensor cache; never opens PCI or
    // reconfigures the counter. A non-success status (or a pre-198 driver)
    // leaves the source unavailable and the TUI falls back to the SMU field.
    if (d.build >= mtop::kMMHUBPerfStatusMinimumBuild) {
        uint64_t result[4]{};
        uint32_t count = 4;
        (void)IOConnectCallScalarMethod(connection.value, mtop::kMMHUBPerfStatusSelector,
                                        nullptr, 0, result, &count);
        if (count == 4) {
            d.mmhub.status = uint32_t(result[0]);
            d.mmhub.raw = uint32_t(result[1]);
            d.mmhub.umcBusyQ8 = result[2] & mtop::kMMHUBPerfStatusMaxQ8;
            d.mmhub.collectedAtNs = result[3];
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
