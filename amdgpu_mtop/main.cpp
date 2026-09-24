#include "model.h"
#include "history.h"
#include <map>
#include <sys/ioctl.h>
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
        if (active) std::cout<<"\033[?1049h\033[?25l";
    }
    ~Terminal() { if (active) {std::cout<<"\033[0m\033[?25h\033[?1049l"<<std::flush;tcsetattr(STDIN_FILENO, TCSANOW, &original);} }
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
void softwareJson(const mtop::Device &d);
void clocksJson(const mtop::Device &d);
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
    if (d.metrics.flags & amdgpu::kSMUMetricsFaulted) return "faulted";
    if (mtop::interfaceMismatch(d)) return "unsupported_firmware_interface";
    if (mtop::fresh(d, clock_gettime_nsec_np(CLOCK_UPTIME_RAW))) return "fresh";
    if ((d.metrics.flags & amdgpu::kSMUMetricsStale) || d.metrics.validFields) return "stale";
    return "unavailable";
}
std::string telemetryProfile(const mtop::Device &d) {
    if (mtop::validClocks(d) && amdgpu::metrics::linux_compatible(d.clocks.driverInterface,d.clocks.firmwareVersion))
        return "linux-compatible";
    if (amdgpu::metrics::verified_interface(d.metrics.driverInterface)) return "verified-interface";
    return "unqualified";
}
std::string telemetryReason(const mtop::Device &d) {
    if (!d.telemetryError.empty()) return d.telemetryError;
    if (d.metrics.flags & amdgpu::kSMUMetricsFaulted) return "Sensor collection faulted; reset required before retry.";
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
        softwareJson(d);
        clocksJson(d);
        using namespace amdgpu::metrics;
        const auto reason = telemetryReason(d);
        std::cout << ",\"telemetry_status\":" << quote(telemetryStatus(d))
                  << ",\"telemetry_error\":" << (reason.empty() ? "null" : quote(reason))
                  << ",\"telemetry_profile\":" << quote(telemetryProfile(d));
        if (d.telemetrySupported)
            std::cout << ",\"telemetry_driver_status\":" << d.metrics.status
                      << ",\"firmware_interface\":" << d.metrics.driverInterface
                      << ",\"verified_firmware_interfaces\":[" << amdgpu::metrics::kDriverInterface << ']'
                      << ",\"sample_generation\":" << d.metrics.generation
                      << ",\"sample_sequence\":" << d.metrics.sequence
                      << ",\"sample_uptime_ns\":" << d.metrics.collectedAtNs
                      << ",\"firmware_metrics_counter\":" << d.metrics.firmwareCounter;
        std::cout << ",\"gfx_activity_percent\":" << metric(d, GfxActivityPercent, 1, "", true)
                  << ",\"gfx_activity_source\":\"SMU AverageGfxActivity\""
                  << ",\"gfx_activity_scope\":\"firmware-reported activity; not CU occupancy or productive workload utilization\""
                  << ",\"gfx_activity_accuracy\":" << quote(d.metrics.driverInterface==amdgpu::metrics::kCompatibleInterface &&
                      (d.metrics.flags&amdgpu::kSMUMetricsLinuxCompatible) ?
                      "idle_100_percent_observed; workload_utilization_unverified" : "not_independently_calibrated")
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
using Histories=std::map<uint64_t,mtop::History>;
std::optional<mtop::RateCounters> rateCounters(const mtop::Device &d) {
    if (!mtop::hasSoftware(d) || (d.software.flags&amdgpu::software_stats::Saturated)) return {};
    mtop::RateCounters out{d.software.generation,d.software.sampledAtNs,d.software.publishedPackets,0};
    for (const auto &engine:d.software.engines) {
        if (UINT64_MAX-out.submitted<engine.submitted) return {};
        out.submitted+=engine.submitted;
        for (const auto bytes:engine.bytes) {
            if (UINT64_MAX-out.completedBytes<bytes) return {};
            out.completedBytes+=bytes;
        }
    }
    return out;
}
std::optional<double> allocatedGiB(const mtop::Device &d) {
    using namespace amdgpu::vram_accounting;
    if (!mtop::hasAccounting(d)) return {};
    return (double(d.accounting.values[VisibleUsed])+double(d.accounting.values[DeviceUsed]))/1073741824.0;
}
std::optional<double> hardwareValue(const mtop::Device &d,amdgpu::metrics::Field field,uint64_t now) {
    if (!mtop::fresh(d,now) || !(d.metrics.validFields&(uint64_t(1)<<field))) return {};
    return double(d.metrics.values[field]);
}
std::string decimal(std::optional<double> value,unsigned places=1) {
    if (!value) return "--";
    std::ostringstream out;out<<std::fixed<<std::setprecision(places)<<*value;return out.str();
}
std::optional<double> clockValue(const mtop::Device &d,unsigned index,uint64_t now,unsigned kind=0) {
    if (index>=4 || (kind ? !mtop::validClocks(d) || d.stage!=15 : !mtop::freshClocks(d,now))) return {};
    const auto &s=d.clocks;
    if (!((kind ? s.limitsValid : s.currentValid)&(1u<<index))) return {};
    return kind==1 ? s.minimumMHz[index] : kind==2 ? s.maximumACMHz[index] : s.currentMHz[index];
}
void clocksJson(const mtop::Device &d) {
    const auto now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    std::cout<<",\"clocks\":{\"status\":"<<quote(mtop::freshClocks(d,now) ? "fresh" : "unavailable")
        <<",\"error\":"<<(d.clocksError.empty() ? "null" : quote(d.clocksError))
        <<",\"range_query_status\":"<<d.clocks.status<<",\"firmware_version\":"<<d.clocks.firmwareVersion;
    const char *names[]={"gfx","soc","memory","fabric"};
    for(unsigned i=0;i<4;++i) {
        std::cout<<','<<quote(names[i])<<":{\"raw_current_mhz\":";
        for(unsigned kind=0;kind<3;++kind) {
            if(kind) std::cout<<(kind==1 ? ",\"dpm_min_mhz\":" : ",\"ac_dpm_max_mhz\":");
            const auto value=clockValue(d,i,now,kind);
            std::cout<<(value ? decimal(value,0) : "null");
        }
        std::cout<<'}';
    }
    std::cout<<'}';
}
void softwareJson(const mtop::Device &d) {
    using namespace amdgpu::software_stats;
    std::cout<<",\"software_stats\":{\"status\":"<<quote(mtop::hasSoftware(d) ? "available" :
        d.softwareSupported ? "unavailable" : "unsupported")<<",\"error\":"
        <<(d.softwareError.empty() ? "null" : quote(d.softwareError));
    if (mtop::hasSoftware(d)) {
        const auto &s=d.software;
        std::cout<<",\"generation\":"<<s.generation<<",\"sample_uptime_ns\":"<<s.sampledAtNs
            <<",\"session_start_ns\":"<<s.sessionStartNs<<",\"runtime_ready\":"<<((s.flags&RuntimeReady) ? "true" : "false")
            <<",\"queue_sample_incomplete\":"<<((s.flags&QueueSampleIncomplete) ? "true" : "false")
            <<",\"saturated\":"<<((s.flags&Saturated) ? "true" : "false")
            <<",\"participants\":"<<s.participants<<",\"active_queues\":"<<s.activeQueues
            <<",\"queued_packets\":"<<s.queuedPackets<<",\"published_packets\":"<<s.publishedPackets
            <<",\"retired_packets\":"<<s.retiredPackets<<",\"consumed_packets\":"<<s.consumedPackets<<",\"cpu_upload_bytes\":"<<s.cpuUploadBytes
            <<",\"cpu_readback_bytes\":"<<s.cpuReadbackBytes<<",\"engines\":[";
        const char *names[]={"SDMA0","SDMA1","GFX","AQL"};
        for (unsigned i=0;i<EngineCount;++i) {
            if (i) std::cout<<',';
            const auto &e=s.engines[i];
            std::cout<<"{\"name\":"<<quote(names[i])<<",\"submitted\":"<<e.submitted
                <<",\"retired\":"<<e.retired<<",\"completed\":"<<e.completed<<",\"failed\":"<<e.failed<<",\"pending\":"<<e.pending
                <<",\"software_pending_ns\":"<<e.pendingNs<<",\"host_to_device_bytes\":"<<e.bytes[HostToDevice]
                <<",\"device_to_host_bytes\":"<<e.bytes[DeviceToHost]<<",\"device_to_device_bytes\":"<<e.bytes[DeviceToDevice]
                <<",\"host_to_host_bytes\":"<<e.bytes[HostToHost]<<",\"unclassified_copy_bytes\":"<<e.bytes[Unknown]<<'}';
        }
        std::cout<<']';
    }
    std::cout<<'}';
}
void dashboard(const std::vector<mtop::Device> &devices, const mtop::Selection &selection,
               const std::string &error, bool interactive, bool listOnly,
               const Histories *histories=nullptr,mtop::RefreshMode mode={},
               unsigned columns=100,unsigned rows=40,uint64_t now=0) {
    if (!now) now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    columns=std::clamp(columns,24u,200u);rows=std::max(8u,rows);
    std::vector<std::string> lines;
    auto line=[&](std::string value) {lines.push_back(std::move(value));};
    line("amdgpu_mtop | "+std::to_string(devices.size())+" GPU(s) | "+mode.label());
    line(std::string(columns,'='));
    if (!error.empty()) line("Enumeration: "+error);
    if (devices.empty()) line("Waiting for a GPU bound to MacAMDGPU...");
    for (const auto &d:devices) {
        line(std::string(selection.registry==d.registry ? "> " : "  ")+id(d.registry)+
            (d.error.empty() ? "  gfx"+std::to_string(d.gfx[0])+"."+std::to_string(d.gfx[1])+"."+std::to_string(d.gfx[2])+
             "  build "+std::to_string(d.build)+"  "+(d.stage==15 ? "initialized" : "stage "+std::to_string(d.stage)) : "  "+d.error));
    }
    if (!listOnly) {
        const auto *d=selection.find(devices);
        if (!d && selection.registry) line("Selected GPU disconnected: "+id(*selection.registry));
        if (d && d->error.empty()) {
            using namespace amdgpu::vram_accounting;
            line("Sensors: "+telemetryProfile(*d)+" | firmware sampling 1 s | "+telemetryStatus(*d));
            const mtop::History empty;
            const auto it=histories ? histories->find(d->registry) : Histories::const_iterator{};
            const auto &history=histories && it!=histories->end() ? it->second : empty;
            for (const auto index:{0u,2u}) {
                const auto current=clockValue(*d,index,now);
                const auto average=hardwareValue(*d,index==0 ? amdgpu::metrics::GfxClockMHz : amdgpu::metrics::MemoryClockMHz,now);
                const auto peak=index==0 ? history.peakGfxClockMHz : history.peakMemoryClockMHz;
                line(std::string(index==0 ? "GFX" : "MEM")+" MHz: avg "+decimal(average,0)+
                    " | SMU raw "+decimal(current,0)+" | raw peak "+decimal(peak,0));
                line("  DPM min "+decimal(clockValue(*d,index,now,1),0)+
                    " | AC DPM max "+decimal(clockValue(*d,index,now,2),0)+" MHz");
            }
            if (mtop::hasSoftware(*d)) {
                uint64_t pending=0,failed=0,completed=0;
                for (const auto &e:d->software.engines) {pending+=e.pending;failed+=e.failed;completed+=e.completed;}
                line("WORK  pending "+std::to_string(pending)+"  completed "+std::to_string(completed)+"  failed "+std::to_string(failed));
                line("WORK  submissions "+decimal(history.points.empty() ? std::optional<double>{} : history.points.back().submissionsPerSecond)+
                    " jobs + packets/s (driver-observed)");
                line("AQL   queues "+std::to_string(d->software.activeQueues)+"  queued packets "+std::to_string(d->software.queuedPackets)+
                    "  consumed "+std::to_string(d->software.consumedPackets)+"  clients "+std::to_string(d->software.participants));
                if (d->software.flags&amdgpu::software_stats::QueueSampleIncomplete) line("AQL queue snapshot incomplete; rates may omit unobserved packets.");
            } else line("Software counters: "+(d->softwareError.empty() ? std::string("requires driver 193+") : d->softwareError));
            const unsigned height=rows>=32 ? 4 : 2;
            auto chart=[&](const std::string &title,const std::string &unit,std::optional<double> mtop::ActivityPoint::*field,
                           std::optional<double> fixedScale={}) {
                const auto values=history.buckets(columns-2,now,field);
                double maximum=1;for (auto value:values) if(value) maximum=std::max(maximum,*value);
                if (fixedScale) maximum=*fixedScale;
                auto current=history.points.empty() ? std::optional<double>{} : history.points.back().*field;
                // A stale or failed firmware read must not retain a previous
                // numeric headline, even while historical samples remain visible.
                if (field==&mtop::ActivityPoint::gfxPercent)
                    current=hardwareValue(*d,amdgpu::metrics::GfxActivityPercent,now);
                line(title+"  "+decimal(current)+" "+unit+
                    (fixedScale ? "  [scale 0.."+decimal(maximum,0)+"]" : "  [peak scale "+decimal(maximum)+"]"));
                for (const auto &row:mtop::graph(values,height,maximum)) line("|"+row+"|");
                line("+"+std::string(columns-2,'-')+"+");
            };
            // Activity is an absolute percentage, never normalized to the
            // recent peak and never replaced with a software submission rate.
            chart("SMU REPORTED GFX","%",&mtop::ActivityPoint::gfxPercent,100.0);
            if (d->metrics.driverInterface==amdgpu::metrics::kCompatibleInterface &&
                (d->metrics.flags&amdgpu::kSMUMetricsLinuxCompatible))
                line("SMU activity: idle can report 100%; workload utilization unverified.");
            else line("SMU activity is firmware-reported; not CU occupancy or useful work.");
            chart("COMPLETED COPIES","MiB/s",&mtop::ActivityPoint::transferMiBPerSecond);
            line("History: last 60 s; peak per column; dots = no sample. Rates are driver-observed.");
            if (mtop::hasAccounting(*d)) {
                const auto &a=d->accounting.values;
                const double used=double(a[VisibleUsed])+double(a[DeviceUsed]);
                const double capacity=double(a[VisibleCapacity])+double(a[DeviceCapacity]);
                line("VRAM POOLS "+mtop::meter(used,capacity,std::min(30u,columns>50 ? columns-48 : 8u))+
                    " "+decimal(used/1073741824.0)+" / "+decimal(capacity/1073741824.0)+" GiB");
            } else line("VRAM pools: initialize GPU to expose allocation accounting.");
            line("CPU-visible: "+accountingSize(*d,VisibleCapacity)+" / "+accountingSize(*d,VisibleUsed)+" / "+accountingSize(*d,VisibleFree)+" / "+accountingSize(*d,VisibleLargestSpan));
            line("GPU-only:    "+accountingSize(*d,DeviceCapacity)+" / "+accountingSize(*d,DeviceUsed)+" / "+accountingSize(*d,DeviceFree)+" / "+accountingSize(*d,DeviceLargestSpan));
            if (rows>=30) line("Pool values: capacity / used / free / largest span. Raw clock peaks since monitor start.");
            if (rows>=36) chart("ALLOCATED VRAM","GiB",&mtop::ActivityPoint::allocatedGiB);
            using namespace amdgpu::metrics;
            if (mtop::fresh(*d,now)) {
                line("HARDWARE  GFX "+metric(*d,GfxActivityPercent,1,"%")+"  UMC "+metric(*d,UmcActivityPercent,1,"%")+
                    "  clock "+metric(*d,GfxClockMHz,1," MHz")+"  power "+metric(*d,SocketPowerMilliwatts,1000," W"));
                line("SENSORS   edge "+metric(*d,EdgeTemperatureMillicelsius,1000," C")+"  hotspot "+metric(*d,HotspotTemperatureMillicelsius,1000," C")+
                    "  fan "+metric(*d,FanRPM,1," RPM"));
            } else line("Hardware load / clocks / power / temperature: "+telemetryStatus(*d));
            line("Software pending work is not hardware GPU utilization; consumed packets may still execute.");
        }
    }
    const std::string footer="[h] fast/slow  [n/p] GPU  [q] quit | "+std::string(mode.label());
    if (interactive) {
        std::cout<<"\033[H";
        const size_t limit=rows-1;
        for (size_t i=0;i<std::min(lines.size(),limit);++i) {
            const bool graphRow=!lines[i].empty() && lines[i][0]=='|';
            std::cout<<"\033[K"<<(graphRow ? "\033[36m" : i==0 ? "\033[1;32m" : "")<<lines[i].substr(0,columns)<<"\033[0m\n";
        }
        std::cout<<"\033[K\033[1m"<<footer.substr(0,columns)<<"\033[0m\033[J";
    } else {
        for (const auto &value:lines) std::cout<<value<<'\n';
        if (!listOnly) std::cout<<footer<<'\n';
    }
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
    mtop::RefreshMode mode;
    Histories histories;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << "amdgpu_mtop [--list] [--device REGISTRY_ID] [--json] [--watch] [--fast | --slow]\n"
                         "Native macOS monitor for GPUs bound to MacAMDGPU (build 172+).\n"
                         "Slow: 0.5 s (default), fast: 0.1 s; h toggles, n/p select GPU, q quits.\n"
                         "Non-terminal output is one snapshot unless --watch is specified.\n"
                         "Observer: never initializes, resets or changes power policy.\n"
                         "Driver 193+: bounded sensor collection at 1 Hz and software activity counters.\n"
                         "Reads CPU-side VRAM allocator accounting from driver 178+.\n";
            return 0;
        } else if (arg == "--json" || arg == "-J") jsonMode = true;
        else if (arg == "--list") listOnly = true;
        else if (arg == "--watch") watch = true;
        else if (arg == "--fast") mode.fast = true;
        else if (arg == "--slow") mode.fast = false;
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
        const auto refreshStart=std::chrono::steady_clock::now();
        std::string error;
        const auto devices = mtop::discover(error);
        selected.initialize(devices);
        const auto sampledAt=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (const auto &device:devices) {
            auto &history=histories[device.registry];
            history.add(sampledAt,rateCounters(device),allocatedGiB(device),hardwareValue(device,amdgpu::metrics::GfxActivityPercent,sampledAt));
            history.clocks(clockValue(device,0,sampledAt),clockValue(device,2,sampledAt));
        }
        for (auto i=histories.begin();i!=histories.end();) {
            const bool present=std::any_of(devices.begin(),devices.end(),[&](const auto &d){return d.registry==i->first;});
            if (!present) i->second.previous.reset();
            if (!present && !i->second.points.empty() && sampledAt-i->second.points.back().timeNs>mtop::History::windowNs) i=histories.erase(i);
            else ++i;
        }
        result = error.empty() ? 0 : 1;
        if (const auto *device = selected.find(devices); device && !device->error.empty()) result = 1;
        if (selected.registry && !selected.find(devices)) result = 1;
        if (jsonMode) json(devices, selected, error);
        else {
            winsize dimensions{};
            if (ioctl(STDOUT_FILENO,TIOCGWINSZ,&dimensions)) dimensions={};
            dashboard(devices,selected,error,interactive,listOnly,&histories,mode,
                dimensions.ws_col ? dimensions.ws_col : 100,dimensions.ws_row ? dimensions.ws_row : 40,sampledAt);
        }
        std::cout.flush();
        if (listOnly || (!interactive && !watch)) break;
        fd_set descriptors;
        FD_ZERO(&descriptors);
        if (interactive) FD_SET(STDIN_FILENO, &descriptors);
        const auto remaining=std::max<int64_t>(0,mode.milliseconds()*1000-
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-refreshStart).count());
        timeval timeout{0, static_cast<suseconds_t>(remaining)};
        const int ready = select(interactive ? STDIN_FILENO + 1 : 0, &descriptors, nullptr, nullptr, &timeout);
        if (ready > 0 && interactive) {
            char key;
            if (read(STDIN_FILENO, &key, 1) == 1) {
                if (key == 'q') break;
                if (key == 'h') mode.toggle();
                if (key == 'n' || key == 'p') selected.step(devices, key == 'n');
            }
        } else if (ready < 0 && errno != EINTR) {
            std::cerr << "Input wait failed\n"; return 1;
        }
    }
    return result;
}
