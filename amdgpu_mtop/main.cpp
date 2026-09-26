#include "model.h"
#include "history.h"
#include "render.h"
#include "tui.h"
#include "sampler.h"
#include <memory>
#include <fstream>
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
    // Read one logical key. Plain bytes are returned as-is (Ctrl-C=3,
    // Ctrl-D=4); ESC sequences (arrows) are consumed with a short timeout
    // so a bare ESC never hangs the refresh loop.
    int readKey() {
        unsigned char first{};
        if (read(STDIN_FILENO, &first, 1) != 1) return -1;
        if (first != 27) return first;
        unsigned char buffer[2]{};
        size_t used = 0;
        for (int i = 0; i < 2; ++i) {
            fd_set waiters;
            FD_ZERO(&waiters);
            FD_SET(STDIN_FILENO, &waiters);
            timeval timeout{0, 1500};
            const int ready = select(STDIN_FILENO + 1, &waiters, nullptr, nullptr, &timeout);
            if (ready <= 0) break;
            if (read(STDIN_FILENO, buffer + used, 1) != 1) break;
            ++used;
        }
        if (!used) return first;
        if (buffer[0] == '[' || buffer[0] == 'O') return buffer[1]; // A..D arrows
        return first;
    }
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
[[maybe_unused]] std::string accountingSize(const mtop::Device &d, amdgpu::vram_accounting::Field field) {
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
std::optional<double> hardwareValue(const mtop::Device &d,amdgpu::metrics::Field field,uint64_t now) {
    if (!mtop::fresh(d,now) || !(d.metrics.validFields&(uint64_t(1)<<field))) return {};
    return double(d.metrics.values[field]);
}
std::optional<double> temperatureValue(const mtop::Device &d,uint64_t now) {
    // Hotspot first, then edge, then memory; 1000 mC per 1 C.
    for (const auto field:std::array<amdgpu::metrics::Field,3>{amdgpu::metrics::HotspotTemperatureMillicelsius,
            amdgpu::metrics::EdgeTemperatureMillicelsius,amdgpu::metrics::MemoryTemperatureMillicelsius}) {
        if (const auto value=hardwareValue(d,field,now)) return *value/1000.0;
    }
    return {};
}
// True delta-based utilization from driver activity telemetry: software
// pendingNs (union of dispatch-in-flight intervals) is a cumulative busy
// counter maintained by the single-writer lifecycle queue; dividing its delta
// by the monotonic elapsed time is a fraction of the window the engines were
// in flight, clamped to 100%. This is the fallback source; a reachable
// hardware busy counter would take precedence (none is exposed by the
// current observer surface: the SMU table carries only the uncalibrated
// AverageGfxActivity average, documented as "idle can report 100%").
std::optional<mtop::RateCounters> rateCounters(const mtop::Device &d) {
    if (!mtop::hasSoftware(d) || (d.software.flags&amdgpu::software_stats::Saturated)) return {};
    mtop::RateCounters out{d.software.generation,d.software.sampledAtNs,d.software.publishedPackets,0};
    for (unsigned i=0;i<amdgpu::software_stats::EngineCount;++i)
        out.enginePendingNs[i]=d.software.engines[i].pendingNs;
    for (const auto &engine:d.software.engines) {
        if (UINT64_MAX-out.pendingNs<engine.pendingNs) return {};
        out.pendingNs+=engine.pendingNs;
        if (UINT64_MAX-out.submitted<engine.submitted) return {};
        out.submitted+=engine.submitted;
        for (unsigned direction=0;direction<amdgpu::software_stats::DirectionCount;++direction) {
            const auto bytes=engine.bytes[direction];
            if(UINT64_MAX-out.directions[direction]<bytes) return {};
            out.directions[direction]+=bytes;
            if (UINT64_MAX-out.completedBytes<bytes) return {};
            out.completedBytes+=bytes;
        }
    }
    return out;
}
// Per-engine dispatch-in-flight busy fraction over the same sample window as
// the aggregate GPU UTIL row. The driver keeps separate pendingNs counters per
// engine (SDMA0, SDMA1, GFX, AQL); this is the finest engine granularity the
// current driver exposes. First sample after a baseline has no window.
std::optional<double> engineBusyPercent(const mtop::Device &d, unsigned engine,
                                        const mtop::History &history) {
    if (!mtop::hasSoftware(d) || (d.software.flags&amdgpu::software_stats::Saturated) ||
        engine>=amdgpu::software_stats::EngineCount) return {};
    const auto &previous=history.previousBusy;
    if (!previous || previous->generation!=d.software.generation ||
        previous->timeNs>=d.software.sampledAtNs ||
        d.software.engines[engine].pendingNs<previous->enginePendingNs[engine]) return {};
    const uint64_t delta=d.software.engines[engine].pendingNs-previous->enginePendingNs[engine];
    const uint64_t elapsed=d.software.sampledAtNs-previous->timeNs;
    if (elapsed>2'000'000'000ull) return {}; // stale gap: no rate across it
    return mtop::busyRatio(delta,0,elapsed);
}
[[maybe_unused]] std::string utilizationSource(const mtop::Device &d) {
    if (!mtop::hasSoftware(d)) return "unavailable";
    if (d.software.flags&amdgpu::software_stats::Saturated) return "saturated";
    return "driver dispatch-in-flight telemetry (software_stats pendingNs delta / wall window)";
}
// True delta-based utilization, computed by the monitor from the driver's
// cumulative in-flight telemetry: pendingNs is the union of dispatch-in-
// flight intervals, maintained by the single-writer lifecycle queue; its delta
// over the previous sample divided by the monotonic wall interval is the
// fraction of that window the engines were in flight, clamped to 100%.
// The first sample after a baseline has no window and reports unavailable.
// A hardware busy counter would be preferred, but the observer surface only
// exposes the SMU AverageGfxActivity average, which is documented as
// uncalibrated ("idle can report 100%") — see utilizationSource().
std::optional<double> busyPercent(const mtop::Device &d,
                                  const mtop::History &history) {
    const auto counters=rateCounters(d);
    if (!counters) return {};
    const auto &previous=history.previousBusy;
    const bool reset=!previous || previous->generation!=counters->generation ||
        previous->timeNs>counters->timeNs || previous->pendingNs>counters->pendingNs ||
        counters->timeNs-previous->timeNs>2'000'000'000ull;
    if (reset || !previous) return {};
    return mtop::busyRatio(counters->pendingNs,previous->pendingNs,counters->timeNs-previous->timeNs);
}
std::optional<double> allocatedGiB(const mtop::Device &d) {
    using namespace amdgpu::vram_accounting;
    if (!mtop::hasAccounting(d)) return {};
    return (double(d.accounting.values[VisibleUsed])+double(d.accounting.values[DeviceUsed]))/1073741824.0;
}
std::optional<double> capacityGiB(const mtop::Device &d) {
    using namespace amdgpu::vram_accounting;
    if (!mtop::hasAccounting(d)) return {};
    return (double(d.accounting.values[VisibleCapacity])+double(d.accounting.values[DeviceCapacity]))/1073741824.0;
}
std::string decimal(std::optional<double> value,unsigned places=1) {
    if (!value) return "--";
    std::ostringstream out;out<<std::fixed<<std::setprecision(places)<<*value;return out.str();
}
// Rust-amdgpu_top-style history row: label, sparkline, current value,
// min/max/avg. Fixed 0..ceiling scale for percentage metrics.
[[maybe_unused]] std::string historyLine(const std::string &label,const std::string &unit,
                        const std::vector<std::optional<double>> &values,
                        std::optional<double> current,double ceiling) {
    const auto minimum=mtop::stats(values);
    std::ostringstream out;
    out<<label<<" ";
    out.fill(' ');out<<std::setw(34)<<mtop::sparkline(values,ceiling);
out.fill(' ');
    out<<"  "<<decimal(current)<<" ";
    if (unit!="") out<<unit<<" ";
    if (minimum) out<<"min "<<decimal(minimum->at(0),0)<<"  avg "<<decimal(minimum->at(2),0)<<"  max "<<decimal(minimum->at(1),0);
    return out.str();
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
               unsigned columns=100,unsigned rows=40,uint64_t now=0,
               mtop::Graphics graphics=mtop::Graphics::Text,mtop::Frame *frame=nullptr,bool demo=false,
               const mtop::tui::Ui *ui=nullptr) {
    if (!now) now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    columns=std::clamp(columns,24u,200u);rows=std::max(8u,rows);
    std::vector<std::string> lines;
    struct PixelChart {unsigned row,height;std::vector<uint8_t> png;};
    std::vector<PixelChart> images;
    auto tint=[&](const std::string&text,const char*code){return interactive ? std::string(code)+text+"\033[0m" : text;};
    auto line=[&](std::string value) {lines.push_back(std::move(value));};
    const auto sort=ui ? ui->sort : mtop::tui::Sort{};
    const bool engines=ui ? ui->enginesVisible : true;
    using namespace mtop::tui;
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
            using namespace amdgpu::metrics;
            const mtop::History empty;
            const auto it=histories ? histories->find(d->registry) : Histories::const_iterator{};
            const auto &history=histories && it!=histories->end() ? it->second : empty;
            const auto utilizationBuckets=history.buckets(120,now,&mtop::ActivityPoint::busyPercent);
            const auto vramBuckets=history.buckets(60,now,&mtop::ActivityPoint::allocatedGiB);
            const auto tempBuckets=history.buckets(60,now,&mtop::ActivityPoint::temperatureC);
            const auto smuBuckets=history.buckets(60,now,&mtop::ActivityPoint::gfxPercent);
            const auto copyBuckets=history.buckets(60,now,&mtop::ActivityPoint::transferMiBPerSecond);
            const auto vramCeiling=capacityGiB(*d).value_or(1.0);
            const auto utilNow=history.points.empty() ? std::optional<double>{} : history.points.back().busyPercent;
                        const auto pwr=hardwareValue(*d,SocketPowerMilliwatts,now);
            const auto pwrCap=hardwareValue(*d,BoardPowerMilliwatts,now);
            const auto edgeT=hardwareValue(*d,EdgeTemperatureMillicelsius,now);
            const auto hotT=hardwareValue(*d,HotspotTemperatureMillicelsius,now);
            const auto fan=hardwareValue(*d,FanRPM,now);
            const auto sclk=hardwareValue(*d,GfxClockMHz,now);
            const auto mclk=hardwareValue(*d,MemoryClockMHz,now);
            const double pwrW=pwr ? *pwr/1000.0 : 0.0;
            const double pwrWCap=pwrCap ? *pwrCap/1000.0 : 0.0;
                        const auto edgeTc=edgeT ? *edgeT/1000.0 : 0.0;
            const auto hotTc=hotT ? *hotT/1000.0 : 0.0;
            const auto fanRpm=fan ? *fan : 0.0;
            const auto sclkMhz=sclk ? *sclk : 0.0;
            const auto mclkMhz=mclk ? *mclk : 0.0;
            const auto visibleCap=hasAccounting(*d) ? double(d->accounting.values[VisibleCapacity])/(1ull<<30) : 0.0;
            const auto deviceCap=hasAccounting(*d) ? double(d->accounting.values[DeviceCapacity])/(1ull<<30) : 0.0;
            const auto visibleUsed=hasAccounting(*d) ? double(d->accounting.values[VisibleUsed])/(1ull<<30) : 0.0;
            const auto deviceUsed=hasAccounting(*d) ? double(d->accounting.values[DeviceUsed])/(1ull<<30) : 0.0;
            const double vramTotal=visibleCap+deviceCap;
            const double vramUsed=visibleUsed+deviceUsed;
            const double vramPct=vramTotal>0 ? vramUsed/vramTotal : 0.0;
            const double gttPct=visibleCap>0 ? visibleUsed/visibleCap : 0.0;
            const unsigned pixelRows=interactive && graphics!=mtop::Graphics::Text ? 5 : 0;
                        if (pixelRows) {
                images.push_back({unsigned(lines.size()+1),1,mtop::chartPNG(utilizationBuckets,480,18,100)});
                images.push_back({unsigned(lines.size()+2),1,mtop::chartPNG(vramBuckets,480,18,vramCeiling,{230,181,74})});
                images.push_back({unsigned(lines.size()+3),1,mtop::chartPNG(tempBuckets,480,18,100,{240,120,90})});
                images.push_back({unsigned(lines.size()+4),1,mtop::chartPNG(smuBuckets,480,18,100,{120,160,200})});
                images.push_back({unsigned(lines.size()+5),1,mtop::chartPNG(copyBuckets,480,18,0.0,{140,200,140})});
                for(unsigned i=0;i<5;++i)line("");
            }
            // ===== Section 1: Header =====
            std::string header="AMDGPU gfx"+std::to_string(d->gfx[0])+'.'+std::to_string(d->gfx[1])+'.'+std::to_string(d->gfx[2]);
            if (d->spec.valid) header+="  "+std::to_string(d->spec.words[12])+" CUs / "+std::to_string(d->spec.words[4])+" SEs";
            header+="  "+dim("build "+std::to_string(d->build)+"  "+(d->stage==15 ? "initialized" : "stage "+std::to_string(d->stage)));
            header+="  "+dim("sensors "+telemetryStatus(*d));
            if (demo) header+="  "+dim("DEMO (synthetic)");
            line(tint(top(columns,header),kBorder));
            line("");
            // ===== Section 2: GPU CORE LOAD (Braille) =====
            line(tint(top(columns,bright("GPU CORE LOAD ")+(utilNow ? "["+decimal(*utilNow,0)+"%]" : dim("[ n/a ]"))),kBorder));
            const size_t cells=columns>10 ? columns-10 : 10;
            const size_t xCount=std::min<size_t>(utilizationBuckets.size(), cells*2);
            std::vector<std::optional<double>> plot;
            for(size_t i=0;i<xCount;++i)
                plot.push_back(utilizationBuckets[utilizationBuckets.size()-xCount+i]);
            auto br=braille(plot,5,100.0);
            const std::string labels[]={"100%","75%","50%","25%","0%"};
            for(size_t r=0;r<5;++r)
                line(cell(columns,bright(labels[r])+"  "+dim(br.rows[r])));
            line(cell(columns,dim(xaxis(cells))));
            line("");
            // ===== VRAM / GTT meters =====
            line(tint(top(columns,bright("VRAM USAGE ")+(vramTotal>0 ? "["+decimal(vramUsed,1)+" / "+decimal(vramTotal,1)+" GB -- "+decimal(vramPct*100.0,1)+"%]" : dim("[ n/a ]"))),kBorder));
            const unsigned barW=std::min(columns-24u, 40u);
            line(cell(columns,"  "+gradientBar(vramUsed, barW)+"  VRAM: "+decimal(vramUsed,1)+" GB / "+decimal(vramPct*100.0,0)+"%"));
            line(cell(columns,"  "+gradientBar(visibleUsed, barW)+"  GTT:   "+decimal(visibleUsed,1)+" GB / "+decimal(gttPct*100.0,0)+"%"));
            line("");
            line(cell(columns,bright("POWER & SENSOR STATE")));
            const unsigned pwrBarW=std::min(columns-32u, 18u);
            line(cell(columns,"  Pwr: "+(pwr ? decimal(pwrW,0)+"W / "+decimal(pwrWCap,0)+"W ["+gradientBar(pwrW,pwrBarW)+"]" : dim("n/a"))+
                "  Temp: "+(edgeT ? decimal(edgeTc,0)+"\xc2\xb0"+"C (Junc: "+(hotT?decimal(hotTc,0):"n/a")+"\xc2\xb0"+"C)" : dim("n/a"))));
            const unsigned fanBarW=std::min(columns-32u, 18u);
            line(cell(columns,"  Fan: "+(fan ? decimal(fanRpm,0)+" RPM  ["+gradientBar(fanRpm/200.0,fanBarW)+"]" : dim("n/a"))+
                "  Clock: SCLK "+(sclk?decimal(sclkMhz,0):"n/a")+"MHz MCLK "+(mclk?decimal(mclkMhz,0):"n/a")+"MHz"));
            line("");
            // ===== Section 3: GRBM / GRBM2 =====
            const auto sdma0=mtop::hasSoftware(*d) ? engineBusyPercent(*d,amdgpu::software_stats::SDMA0,history) : std::optional<double>{};
            const auto gfxEng=mtop::hasSoftware(*d) ? engineBusyPercent(*d,amdgpu::software_stats::GFX,history) : std::optional<double>{};
            const auto aqlEng=mtop::hasSoftware(*d) ? engineBusyPercent(*d,amdgpu::software_stats::AQL,history) : std::optional<double>{};
            auto engineBar=[&](std::optional<double> v){
                return v ? gradientBar(*v/100.0,16)+" "+decimal(*v,0)+"%" : dim("n/a (no in-flight sample window yet)");
            };
            line(tint(mid(columns,bright("PERFORMANCE COUNTERS (GRBM / GRBM2)")),kBorder));
            if (engines) {
                line(cell(columns,bright("  [GRBM Status]")+"    "+dim("source: driver dispatch-in-flight per engine (selector 61)")));
                line(cell(columns,"  Graphics Pipe (GFX) : "+(utilNow ? gradientBar(*utilNow/100.0,16)+" "+decimal(*utilNow,0)+"%" : dim("n/a (no in-flight sample window yet)"))));
                line(cell(columns,"  Compute Engine 0    : "+engineBar(gfxEng)));
                line(cell(columns,"  Compute Engine 1    : "+(aqlEng ? engineBar(aqlEng) : dim("n/a (AQL compute queue in-flight)"))));
                line(cell(columns,"  SDMA Engine (DMA)   : "+engineBar(sdma0)+"  "+dim("(SDMA0; SDMA1 not observed)")));
                line(cell(columns,"  VCN (Video Decode)  : "+dim("n/a (driver exposes no VCN counter; would need MMIO PERFSTATUS or SMU media field)")));
                line(cell(columns,"  JPEG Engine         : "+dim("n/a (driver exposes no JPEG counter)")));
                line("");
                line(cell(columns,bright("  [GRBM2 Status]")+"    "+dim("source: none (MMHUB/GFXHUB PERFSTATUS + L2 counters not exposed by driver)")));
                line(cell(columns,"  Command Processor (CPF) : "+dim("n/a (MMHUB_PERFSTATUS not exposed)")));
                line(cell(columns,"  Texture Cache (TCC)     : "+dim("n/a (MMHUB_L2 hit/miss counters not exposed)")));
                line(cell(columns,"  Depth Block (DB)        : "+dim("n/a (GRBM select + MMIO read not exposed)")));
                line(cell(columns,"  Color Block (CB)        : "+dim("n/a (GRBM select + MMIO read not exposed)")));
                line(cell(columns,"  Shader Pipe (SPI)       : "+dim("n/a (per-SPI activity not exposed)")));
                line(cell(columns,"  Primitive Assembly (PA) : "+dim("n/a (per-PA activity not exposed)")));
            } else {
                line(cell(columns,dim("  (hidden; press r to toggle)")));
            }
            line("");
            // ===== Section 4: GPU PROCESSES =====
            line(tint(mid(columns,bright("GPU PROCESSES (fdinfo)")+"  [sort: "+sort.label()+"]"),kBorder));
            line(cell(columns,dim("  PID     USER       PROCESS NAME            CPU%    GPU%   GFX/COMP   MEDIA     VRAM USAGE      VRAM BAR")));
            line(cell(columns,dim("  "+std::string(columns>24?columns-4:20,'-'))));
            line(cell(columns,dim("  n/a     n/a        per-process GPU accounting not exposed by macOS driver")));
            line(cell(columns,dim("  Driver exposes total VRAM pools + dispatch-in-flight + SMU sensors only.")));
            line(cell(columns,dim("  Per-PID GPU%, engine breakdown, VRAM require a driver-side per-client counter.")));
            line(cell(columns,dim("  See mtop-report.md for the exact missing query.")));
            line("");
            // ===== Footer =====
            const std::string footer="[q] Quit  [h] Interval: "+std::to_string(mode.milliseconds())+"ms"+
                "  [p] Sort PID  [m] Sort VRAM  [g] Sort GPU  [r] Toggle GRBM";
            if (interactive) {
                lines.resize(std::min<size_t>(lines.size(),rows-1));
                while(lines.size()<rows-1)lines.emplace_back();
                lines.push_back(tint(footer,"\033[1m"));
                mtop::Frame temporary;
                if(frame && graphics==mtop::Graphics::ITerm)frame->previous.clear();
                std::string output=(frame?*frame:temporary).update(lines,columns,rows);
                if(graphics==mtop::Graphics::Kitty)
                    for(unsigned i=0;i<3;++i)output+="\033_Ga=d,d=I,i="+std::to_string(5001+i)+",q=2\033\\";
                for(size_t i=0;i<images.size();++i) {
                    const auto &image=images[i];
                    if(image.row+image.height<=rows)
                        output+=mtop::pixelImage(graphics,image.png,5001+unsigned(i),image.row,columns-3,image.height);
                }
                std::cout<<output;
            } else {
                for (const auto &value:lines) std::cout<<value<<'\n';
                if (!listOnly) std::cout<<footer<<'\n';
            }
        }
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
    bool jsonMode = false, listOnly = false, watch = false, demo = false;
    std::string graphicsOption="auto",previewPNG;
    mtop::Selection selected;
    mtop::RefreshMode mode;
    Histories histories;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout << "amdgpu_mtop [--list] [--device REGISTRY_ID] [--json] [--watch] [--fast | --slow] [--graphics auto|kitty|iterm|text] [--demo]\n"
                         "Native macOS btop-style monitor for GPUs bound to MacAMDGPU (build 172+).\n"
                         "Slow: 0.1 s (default), fast: 0.01 s; h toggles, arrows select GPU, q quits.\n"
                         "Hotkeys: m sort VRAM, g sort GPU, p sort PID, r toggle GRBM engines.\n"
                         "Live 60 s Braille GPU-utilization history (driver in-flight delta, 0..100%),\n"
                         "VRAM/GTT meters, power/temperature/fan/clock sensors, GRBM/GRBM2 breakdown,\n"
                         "per-process table (n/a when the driver exposes no per-client counters).\n"
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
        else if (arg == "--demo") demo = true;
        else if (arg == "--preview-png" && i+1<argc) previewPNG=argv[++i];
        else if (arg == "--graphics" && i+1<argc) {
            graphicsOption=argv[++i];
            if(graphicsOption!="auto" && graphicsOption!="kitty" && graphicsOption!="iterm" && graphicsOption!="text") {std::cerr<<"Unknown graphics protocol\n";return 2;}
        }
        else if (arg == "--device" && i + 1 < argc) {
            uint64_t value;
            if (!number(argv[++i], value)) { std::cerr << "Invalid registry ID\n"; return 2; }
            selected.registry = value;
        } else { std::cerr << "Unknown or incomplete option: " << arg << '\n'; return 2; }
    }
    if(!previewPNG.empty()) {
        std::vector<std::optional<double>> values(320);
        for(size_t i=0;i<values.size();++i) if(i<110 || i>130)values[i]=30+25*std::sin(double(i)/15)+12*std::cos(double(i)/6);
        const auto png=mtop::chartPNG(values,960,180,100);
        std::ofstream file(previewPNG,std::ios::binary);file.write(reinterpret_cast<const char*>(png.data()),png.size());
        return file ? 0 : 1;
    }
    const bool interactive = !jsonMode && !listOnly && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    Terminal terminal(interactive);
    if (interactive && !terminal.active) { std::cerr << "Unable to configure terminal\n"; return 1; }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const auto env=[](const char*name){const char*v=std::getenv(name);return std::string(v?v:"");};
    auto program=env("TERM_PROGRAM");
    unsigned major=0,minor=0;
    if(program=="iTerm.app" && std::sscanf(env("TERM_PROGRAM_VERSION").c_str(),"%u.%u",&major,&minor)==2 &&
        (major>3 || (major==3 && minor>=7)))program="kitty";
    const auto graphics=mtop::graphicsFor(graphicsOption,program,env("TERM"),!env("TMUX").empty() || !env("STY").empty());
    mtop::Frame frame;
    mtop::tui::Ui uiState;
    std::unique_ptr<mtop::Sampler> sampler;
    if(interactive && !demo)sampler=std::make_unique<mtop::Sampler>(mode.milliseconds());
    uint64_t sequence=0;
    int result = 0;
    while (running) {
        const auto refreshStart=std::chrono::steady_clock::now();
        std::string error;
        std::vector<mtop::Device> devices;
        bool newSample=true;
        if(sampler) {
            auto sample=sampler->latest();devices=std::move(sample.devices);error=std::move(sample.error);
            newSample=sample.sequence!=sequence;sequence=sample.sequence;
        } else if(!demo) devices=mtop::discover(error);
        if(demo) {
            mtop::Device d;d.registry=0x100000001;d.build=195;d.stage=15;d.gfx[0]=12;d.gfx[2]=1;
            const auto t=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            d.telemetrySupported=true;d.metrics.version=amdgpu::kSMUMetricsSnapshotVersion;d.metrics.size=sizeof(d.metrics);
            d.metrics.flags=amdgpu::kSMUMetricsValid;d.metrics.driverInterface=amdgpu::metrics::kDriverInterface;
            d.metrics.collectedAtNs=t;d.metrics.validFields=(1ull<<amdgpu::metrics::Count)-1;
            d.metrics.values[amdgpu::metrics::GfxActivityPercent]=45;d.metrics.values[amdgpu::metrics::GfxClockMHz]=1820;
            d.metrics.values[amdgpu::metrics::MemoryClockMHz]=1250;d.metrics.values[amdgpu::metrics::SocketPowerMilliwatts]=105000;
            d.clocksSupported=true;d.clocks.version=1;d.clocks.size=sizeof(d.clocks);d.clocks.flags=amdgpu::kSMUMetricsValid;
            d.clocks.collectedAtNs=t;d.clocks.currentValid=d.clocks.limitsValid=5;d.clocks.currentMHz[0]=2050;d.clocks.currentMHz[2]=1250;
            d.clocks.minimumMHz[0]=500;d.clocks.maximumACMHz[0]=2900;d.clocks.minimumMHz[2]=100;d.clocks.maximumACMHz[2]=1250;
            d.metrics.values[amdgpu::metrics::UmcActivityPercent]=31;
            d.metrics.values[amdgpu::metrics::FanRPM]=1240;
            d.metrics.values[amdgpu::metrics::EdgeTemperatureMillicelsius]=46000;
            d.metrics.values[amdgpu::metrics::HotspotTemperatureMillicelsius]=58000;
            d.metrics.values[amdgpu::metrics::MemoryTemperatureMillicelsius]=54000;
            amdgpu::software_stats::Counters counters;counters.reset(t);
            // Demo: seed per-engine dispatch-in-flight history so the GRBM
            // panel shows the driver's real per-engine counters (GFX/AQL
            // in-flight), distinct from the aggregate busy percentage.
            for(unsigned i=0;i<60;++i) {
                const uint64_t ts=t-(59-i)*100000000ull;
                const uint64_t gfxNs=uint64_t(20+15*std::sin(i/13.0))*10000000;
                const uint64_t aqlNs=uint64_t(10+8*std::sin(i/19.0))*10000000;
                if(counters.begin(amdgpu::software_stats::GFX,ts))
                    counters.complete(amdgpu::software_stats::GFX,ts+gfxNs,gfxNs,amdgpu::software_stats::HostToDevice);
                if(counters.begin(amdgpu::software_stats::AQL,ts))
                    counters.complete(amdgpu::software_stats::AQL,ts+aqlNs,aqlNs,amdgpu::software_stats::DeviceToDevice);
            }
            d.software=counters.snapshot(t,true);d.softwareSupported=true;
            d.software.activeQueues=2;d.software.participants=1;
            amdgpu::VRAMBumpAllocator visible,device;
            visible.init(24ull<<20,232ull<<20);device.init(256ull<<20,31ull<<30);
            d.accounting=amdgpu::vram_accounting::snapshot(true,0,32ull<<30,256ull<<20,visible,device);d.accountingSupported=true;
            d.accounting.values[amdgpu::vram_accounting::DeviceUsed]=22ull<<30;
            d.accounting.values[amdgpu::vram_accounting::DeviceFree]=9ull<<30;
            d.accounting.values[amdgpu::vram_accounting::DeviceLargestSpan]=9ull<<30;
            d.accounting.values[amdgpu::vram_accounting::DeviceCount]=1;
            devices={d};
            auto &h=histories[d.registry];
            if(h.points.empty())for(unsigned i=0;i<600;++i) {
                mtop::ActivityPoint p;p.timeNs=t-(599-i)*100000000ull;p.gfxPercent=40+30*std::sin(i/25.0);
                p.busyPercent=30+25*std::sin(i/17.0)+10*std::sin(i/4.0);
                p.transferMiBPerSecond=200+160*std::sin(i/37.0);
                p.allocatedGiB=18+6*std::sin(i/29.0);p.temperatureC=52+8*std::sin(i/41.0);
                h.points.push_back(p);
            }
            h.clocks(2250,1250);h.clocks(700,1000);
            h.previousBusy=mtop::RateCounters{1,t,0,0};
            h.previous=mtop::RateCounters{1,t,0,0};
            newSample=false;
        }
        selected.initialize(devices);
        const auto sampledAt=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if(newSample)for (const auto &device:devices) {
            auto &history=histories[device.registry];
            const auto counters=rateCounters(device);
            const auto busy=busyPercent(device,history);
            history.add(sampledAt,counters,allocatedGiB(device),hardwareValue(device,amdgpu::metrics::GfxActivityPercent,sampledAt),
                        temperatureValue(device,sampledAt));
            history.previousBusy=counters;
            if (busy) history.points.back().busyPercent=busy;
            history.clocks(clockValue(device,0,sampledAt),clockValue(device,2,sampledAt));
        }
        for (auto i=histories.begin();i!=histories.end();) {
            const bool present=std::any_of(devices.begin(),devices.end(),[&](const auto &d){return d.registry==i->first;});
            if (!present) i->second.previous.reset();
            if (!present) i->second.previousBusy.reset();
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
                dimensions.ws_col ? dimensions.ws_col : 100,dimensions.ws_row ? dimensions.ws_row : 40,sampledAt,graphics,&frame,demo,&uiState);
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
            const int key=terminal.readKey();
            if (key > 0) {
                if (key == 'q') break;
                if (key == 'h') {mode.toggle();if(sampler)sampler->period(mode.milliseconds());}
                // 'p' is PID sort per the spec footer; GPU navigation is the
                // Up/Down arrows (key 'A'/'B').
                if (key == 'A') selected.step(devices, false);
                if (key == 'B') selected.step(devices, true);
                uiState.onKey(char(key));
            }
        } else if (ready < 0 && errno != EINTR) {
            std::cerr << "Input wait failed\n"; return 1;
        }
    }
    return result;
}
