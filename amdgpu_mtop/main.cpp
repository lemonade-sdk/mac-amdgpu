#include "model.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
struct Terminal {
    termios original{};
    bool active = false;
    explicit Terminal(bool enabled) {
        if (!enabled || tcgetattr(STDIN_FILENO, &original)) return;
        termios raw = original;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        active = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~Terminal() { if (active) tcsetattr(STDIN_FILENO, TCSANOW, &original); }
};
std::string id(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
std::string gib(uint64_t value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << double(value) / (1ull << 30) << " GiB";
    return out.str();
}
std::string accountingValue(const mtop::Device &d, amdgpu::vram_accounting::Field field) {
    return mtop::hasAccounting(d) ? std::to_string(d.accounting.values[field]) : "null";
}
std::string accountingSize(const mtop::Device &d, amdgpu::vram_accounting::Field field) {
    return mtop::hasAccounting(d) ? gib(d.accounting.values[field]) : "unavailable";
}
std::string quote(const std::string &value);
void accountingJson(const mtop::Device &d) {
    using namespace amdgpu::vram_accounting;
    std::cout << ",\"vram_accounting\":{\"status\":"
              << quote(mtop::hasAccounting(d) ? "available" :
                       (d.accountingSupported || !d.accountingError.empty() ? "unavailable" : "unsupported"))
              << ",\"error\":" << (d.accountingError.empty() ? "null" : quote(d.accountingError));
    constexpr const char *names[] = {"usable_bytes", "cpu_visible_bytes", "excluded_bytes",
        "visible_pool_capacity_bytes", "visible_pool_used_bytes", "visible_pool_free_bytes",
        "visible_pool_largest_free_span_bytes", "visible_pool_allocation_count",
        "device_pool_capacity_bytes", "device_pool_used_bytes", "device_pool_free_bytes",
        "device_pool_largest_free_span_bytes", "device_pool_allocation_count"};
    static_assert(sizeof(names) / sizeof(names[0]) == Count - UsableBytes);
    for (unsigned i = UsableBytes; i < Count; ++i)
        std::cout << ',' << quote(names[i - UsableBytes]) << ':' << accountingValue(d, Field(i));
    std::cout << '}';
}
std::string quote(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
std::string metric(const mtop::Device &d, amdgpu::metrics::Field field,
                   uint32_t scale = 1, const char *unit = "", bool jsonMode = false) {
    if (!mtop::fresh(d, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) ||
        !(d.metrics.validFields & (uint64_t(1) << field)))
        return jsonMode ? "null" : "unavailable";
    std::ostringstream out;
    if (scale == 1) out << d.metrics.values[field];
    else out << std::fixed << std::setprecision(1) << double(d.metrics.values[field]) / scale;
    if (!jsonMode) out << unit;
    return out.str();
}
std::string telemetryStatus(const mtop::Device &d) {
    if (!d.telemetrySupported) return d.telemetryError.empty() ? "not_implemented" : "unavailable";
    if (mtop::interfaceMismatch(d)) return "unsupported_firmware_interface";
    if (d.metrics.flags & amdgpu::kSMUMetricsFaulted) return "faulted";
    if (mtop::fresh(d, clock_gettime_nsec_np(CLOCK_UPTIME_RAW))) return "fresh";
    if ((d.metrics.flags & amdgpu::kSMUMetricsStale) || d.metrics.validFields) return "stale";
    return "unavailable";
}
std::string telemetryReason(const mtop::Device &d) {
    if (!d.telemetryError.empty()) return d.telemetryError;
    if (mtop::interfaceMismatch(d))
        return "SMU firmware interface " + id(d.metrics.driverInterface) +
            "; verified metrics layout is " + id(amdgpu::metrics::kDriverInterface) +
            ". Collection disabled before requesting firmware data.";
    return {};
}
void json(const std::vector<mtop::Device> &devices, const mtop::Selection &selection,
          const std::string &error) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::cout << "{\"schema\":1,\"timestamp_unix_ms\":" << now << ",\"selected_registry_id\":"
              << (selection.registry ? quote(id(*selection.registry)) : "null")
              << ",\"error\":" << (error.empty() ? "null" : quote(error)) << ",\"devices\":[";
    bool first = true;
    for (const auto &d : devices) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"registry_id\":" << quote(id(d.registry)) << ",\"error\":"
                  << (d.error.empty() ? "null" : quote(d.error));
        if (d.error.empty()) {
            std::cout << ",\"driver_build\":" << d.build << ",\"stage\":" << d.stage
                      << ",\"vram_total_bytes\":" << (d.total ? std::to_string(d.total) : "null")
                      << ",\"vram_cpu_visible_bytes\":" << (d.visible ? std::to_string(d.visible) : "null");
        }
        accountingJson(d);
        using namespace amdgpu::metrics;
        const auto reason = telemetryReason(d);
        std::cout << ",\"telemetry_status\":" << quote(telemetryStatus(d))
                  << ",\"telemetry_error\":" << (reason.empty() ? "null" : quote(reason));
        if (d.telemetrySupported)
            std::cout << ",\"telemetry_driver_status\":" << d.metrics.status
                      << ",\"firmware_interface\":" << d.metrics.driverInterface
                      << ",\"verified_firmware_interfaces\":[" << amdgpu::metrics::kDriverInterface << ']'
                      << ",\"sample_generation\":" << d.metrics.generation
                      << ",\"sample_sequence\":" << d.metrics.sequence
                      << ",\"sample_uptime_ns\":" << d.metrics.collectedAtNs
                      << ",\"firmware_metrics_counter\":" << d.metrics.firmwareCounter;
        std::cout << ",\"gfx_activity_percent\":" << metric(d, GfxActivityPercent, 1, "", true)
                  << ",\"umc_activity_percent\":" << metric(d, UmcActivityPercent, 1, "", true)
                  << ",\"media_activity_percent\":" << metric(d, MediaActivityPercent, 1, "", true)
                  << ",\"vram_used_bytes\":null,\"gtt_used_bytes\":null"
                  << ",\"gfx_clock_mhz\":" << metric(d, GfxClockMHz, 1, "", true)
                  << ",\"memory_clock_mhz\":" << metric(d, MemoryClockMHz, 1, "", true)
                  << ",\"soc_clock_mhz\":" << metric(d, SocClockMHz, 1, "", true)
                  << ",\"fabric_clock_mhz\":" << metric(d, FabricClockMHz, 1, "", true)
                  << ",\"socket_power_watts\":" << metric(d, SocketPowerMilliwatts, 1000, "", true)
                  << ",\"board_power_watts\":" << metric(d, BoardPowerMilliwatts, 1000, "", true)
                  << ",\"edge_temperature_celsius\":" << metric(d, EdgeTemperatureMillicelsius, 1000, "", true)
                  << ",\"hotspot_temperature_celsius\":" << metric(d, HotspotTemperatureMillicelsius, 1000, "", true)
                  << ",\"memory_temperature_celsius\":" << metric(d, MemoryTemperatureMillicelsius, 1000, "", true)
                  << ",\"fan_rpm\":" << metric(d, FanRPM, 1, "", true) << '}';
    }
    std::cout << "]}\n";
}
void dashboard(const std::vector<mtop::Device> &devices, const mtop::Selection &selection,
               const std::string &error, bool interactive, bool listOnly) {
    if (interactive) std::cout << "\033[H\033[2J";
    std::cout << "amdgpu_mtop | macOS DriverKit | " << devices.size() << " device(s)\n";
    if (!error.empty()) std::cout << "Enumeration error: " << error << '\n';
    if (devices.empty()) std::cout << "No GPUs bound to MacAMDGPU are visible.\n";
    for (const auto &d : devices) {
        std::cout << (selection.registry == d.registry ? " > " : "   ") << id(d.registry);
        if (d.error.empty()) std::cout << "  gfx" << d.gfx[0] << '.' << d.gfx[1] << '.' << d.gfx[2]
                                     << "  build " << d.build << "  stage " << d.stage;
        else std::cout << "  " << d.error;
        std::cout << '\n';
    }
    if (listOnly) return;
    const auto *d = selection.find(devices);
    if (!d) {
        if (selection.registry) std::cout << "Selected device " << id(*selection.registry) << " is disconnected.\n";
    } else if (d->error.empty()) {
        using namespace amdgpu::metrics;
        auto row = [](const std::string &left, const std::string &right) {
            std::cout << ' ' << std::left << std::setw(29) << left << right << '\n';
        };
        std::cout << '\n';
        row("GPU ACTIVITY", "MEMORY");
        row("GFX     " + metric(*d, GfxActivityPercent, 1, "%"), "VRAM usable       " + (d->total ? gib(d->total) : "unavailable"));
        row("UMC     " + metric(*d, UmcActivityPercent, 1, "%"), "CPU-visible VRAM  " + (d->visible ? gib(d->visible) : "unavailable"));
        row("Media   " + metric(*d, MediaActivityPercent, 1, "%"), "Outside pools     " + accountingSize(*d, amdgpu::vram_accounting::ExcludedBytes));
        row("", "GTT used          unavailable");
        std::cout << "\n VRAM ALLOCATORS               capacity / used / free / largest free span\n";
        using namespace amdgpu::vram_accounting;
        row("CPU-visible pool", accountingSize(*d, VisibleCapacity) + " / " +
            accountingSize(*d, VisibleUsed) + " / " + accountingSize(*d, VisibleFree) +
            " / " + accountingSize(*d, VisibleLargestSpan));
        row("GPU-only pool", accountingSize(*d, DeviceCapacity) + " / " +
            accountingSize(*d, DeviceUsed) + " / " + accountingSize(*d, DeviceFree) +
            " / " + accountingSize(*d, DeviceLargestSpan));
        if (!d->accountingError.empty()) std::cout << " " << d->accountingError << '\n';
        std::cout << '\n';
        row("CLOCKS", "SENSORS");
        row("GFX     " + metric(*d, GfxClockMHz, 1, " MHz"),
            "Socket power      " + metric(*d, SocketPowerMilliwatts, 1000, " W"));
        row("Memory  " + metric(*d, MemoryClockMHz, 1, " MHz"),
            "Board power       " + metric(*d, BoardPowerMilliwatts, 1000, " W"));
        row("SOC     " + metric(*d, SocClockMHz, 1, " MHz"),
            "Edge / hotspot    " + metric(*d, EdgeTemperatureMillicelsius, 1000, " C") +
            " / " + metric(*d, HotspotTemperatureMillicelsius, 1000, " C"));
        row("Fabric  " + metric(*d, FabricClockMHz, 1, " MHz"),
            "Memory temp       " + metric(*d, MemoryTemperatureMillicelsius, 1000, " C"));
        row("", "Fan RPM           " + metric(*d, FanRPM));
        std::cout << "\n Telemetry: " << telemetryStatus(*d);
        if (d->telemetrySupported) std::cout << "  sample " << d->metrics.sequence
                                           << "  driver status " << id(d->metrics.status);
        const auto reason = telemetryReason(*d);
        if (!reason.empty()) std::cout << "\n " << reason;
        std::cout << "\n Pool use includes rounded client and driver allocations, not all firmware memory.\n"
                     " Outside pools includes fixed reservations/gaps; UMC activity measures controller work.\n";
    }
    if (interactive) std::cout << "\n [n] next GPU  [p] previous GPU  [q] quit  | refresh 1 s\n";
}
bool number(const char *text, uint64_t &value) {
    if (!*text || *text == '-' || *text == '+') return false;
    char *end = nullptr;
    errno = 0;
    value = std::strtoull(text, &end, 0);
    return !errno && end && !*end;
}
} // namespace

int main(int argc, char **argv) {
    bool jsonMode = false, listOnly = false, watch = false;
    mtop::Selection selected;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << "amdgpu_mtop [--list] [--device REGISTRY_ID] [--json] [--watch]\n"
                         "Native macOS monitor for GPUs bound to MacAMDGPU (build 172+).\n"
                         "Terminal mode refreshes each second; n/p switch devices, q quits.\n"
                         "Non-terminal output is one snapshot unless --watch is specified.\n"
                         "Observer only: never initializes, resets or changes the GPU.\n"
                         "Reads cached dynamic telemetry from driver 176+ when collected by its owner.\n"
                         "Reads CPU-side VRAM allocator accounting from driver 178+.\n";
            return 0;
        } else if (arg == "--json" || arg == "-J") jsonMode = true;
        else if (arg == "--list") listOnly = true;
        else if (arg == "--watch") watch = true;
        else if (arg == "--device" && i + 1 < argc) {
            uint64_t value;
            if (!number(argv[++i], value)) { std::cerr << "Invalid registry ID\n"; return 2; }
            selected.registry = value;
        } else { std::cerr << "Unknown or incomplete option: " << arg << '\n'; return 2; }
    }
    const bool interactive = !jsonMode && !listOnly && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    Terminal terminal(interactive);
    if (interactive && !terminal.active) { std::cerr << "Unable to configure terminal\n"; return 1; }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    int result = 0;
    while (running) {
        std::string error;
        const auto devices = mtop::discover(error);
        selected.initialize(devices);
        result = error.empty() ? 0 : 1;
        if (const auto *device = selected.find(devices); device && !device->error.empty()) result = 1;
        if (selected.registry && !selected.find(devices)) result = 1;
        if (jsonMode) json(devices, selected, error);
        else dashboard(devices, selected, error, interactive, listOnly);
        std::cout.flush();
        if (listOnly || (!interactive && !watch)) break;
        fd_set descriptors;
        FD_ZERO(&descriptors);
        if (interactive) FD_SET(STDIN_FILENO, &descriptors);
        timeval timeout{1, 0};
        const int ready = select(interactive ? STDIN_FILENO + 1 : 0, &descriptors, nullptr, nullptr, &timeout);
        if (ready > 0 && interactive) {
            char key;
            if (read(STDIN_FILENO, &key, 1) == 1) {
                if (key == 'q') break;
                if (key == 'n' || key == 'p') selected.step(devices, key == 'n');
            }
        } else if (ready < 0 && errno != EINTR) {
            std::cerr << "Input wait failed\n"; return 1;
        }
    }
    return result;
}
