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
    if (!value) return "unavailable";
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << double(value) / (1ull << 30) << " GiB";
    return out.str();
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
        std::cout << ",\"telemetry_status\":\"not_implemented\",\"gfx_activity_percent\":null,"
                     "\"umc_activity_percent\":null,\"vram_used_bytes\":null,\"gtt_used_bytes\":null,"
                     "\"gfx_clock_mhz\":null,\"memory_clock_mhz\":null,\"socket_power_watts\":null,"
                     "\"board_power_watts\":null,\"edge_temperature_celsius\":null,"
                     "\"hotspot_temperature_celsius\":null,\"memory_temperature_celsius\":null,"
                     "\"fan_rpm\":null}";
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
        std::cout << "\n GPU ACTIVITY                MEMORY\n"
                     " GFX     unavailable         VRAM total        " << gib(d->total) << '\n'
                  << " UMC     unavailable         CPU-visible VRAM  " << gib(d->visible) << '\n'
                  << " Media   unavailable         VRAM used         unavailable\n"
                     "                             GTT used          unavailable\n"
                     "\n CLOCKS                      SENSORS\n"
                     " GFX     unavailable         Socket power      unavailable\n"
                     " Memory  unavailable         Board power       unavailable\n"
                     " SOC     unavailable         Edge / hotspot    unavailable\n"
                     " Fabric  unavailable         Memory temp       unavailable\n"
                     "                             Fan RPM           unavailable\n"
                     "\n Dynamic telemetry awaits the driver metrics endpoint.\n"
                     " UMC activity measures memory-controller work; VRAM used measures allocations.\n";
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
                         "Dynamic telemetry is unavailable until its driver endpoint is implemented.\n";
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
