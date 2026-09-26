#include "model.h"
#include "history.h"
#include "render.h"
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
std::string utilizationSource(const mtop::Device &d) {
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
std::string historyLine(const std::string &label,const std::string &unit,
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
               mtop::Graphics graphics=mtop::Graphics::Text,mtop::Frame *frame=nullptr,bool demo=false) {
    if (!now) now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    columns=std::clamp(columns,24u,200u);rows=std::max(8u,rows);
    std::vector<std::string> lines;
    struct PixelChart {unsigned row,height;std::vector<uint8_t> png;};
    std::vector<PixelChart> images;
    auto tint=[&](const std::string&text,const char*code){return interactive ? std::string(code)+text+"\033[0m" : text;};
    auto line=[&](std::string value) {lines.push_back(std::move(value));};
    line(tint(std::string("amdgpu_mtop")+(demo?" DEMO (synthetic)":"")+"  |  "+std::to_string(devices.size())+" GPU(s)  |  "+mode.label()+
        (graphics==mtop::Graphics::Kitty ? "  |  PIXEL / Kitty" : graphics==mtop::Graphics::ITerm ? "  |  PIXEL / iTerm" : "  |  TEXT / block sparkline"),"\033[1;36m"));
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
            const mtop::History empty;
            const auto it=histories ? histories->find(d->registry) : Histories::const_iterator{};
            const auto &history=histories && it!=histories->end() ? it->second : empty;
            using namespace amdgpu::metrics;
            // Header: chip identity/geometry + current clocks, temperature, sensors.
            std::string chip=std::string("gfx")+std::to_string(d->gfx[0])+'.'+std::to_string(d->gfx[1])+'.'+std::to_string(d->gfx[2]);
            if (d->spec.valid) chip+="  "+std::to_string(d->spec.words[12])+" CUs  "+std::to_string(d->spec.words[4])+" SE";
            const auto tempNow=temperatureValue(*d,now);
            line(tint("GPU  "+chip,"\033[1m")+
                "  "+(d->stage==15 ? "initialized" : "stage "+std::to_string(d->stage))+
                "  |  "+tint("GFX "+decimal(hardwareValue(*d,GfxClockMHz,now),0)+" MHz","\033[1;36m")+
                "  "+tint("MEM "+decimal(hardwareValue(*d,MemoryClockMHz,now),0)+" MHz","\033[1;35m")+
                "  "+tint("TEMP "+decimal(tempNow,0)+" C","\033[1;33m")+
                "  |  sensors "+telemetryStatus(*d));
            line("DPM min / AC max  GFX "+decimal(clockValue(*d,0,now,1),0)+" / "+decimal(clockValue(*d,0,now,2),0)+
                "   MEM "+decimal(clockValue(*d,2,now,1),0)+" / "+decimal(clockValue(*d,2,now,2),0)+
                " MHz | raw GFX "+decimal(clockValue(*d,0,now),0)+"  MEM "+decimal(clockValue(*d,2,now),0)+
                " | observed min/max GFX "+decimal(history.minimumGfxClockMHz,0)+" / "+decimal(history.peakGfxClockMHz,0));
            line(tint("POWER  "+metric(*d,SocketPowerMilliwatts,1000," W"),"\033[1;33m")+
                "    FAN "+metric(*d,FanRPM,1," RPM")+"    UMC activity "+metric(*d,UmcActivityPercent,1,"%"));
            if (mtop::hasAccounting(*d)) {
                const auto &a=d->accounting.values;
                const double used=double(a[VisibleUsed])+double(a[DeviceUsed]);
                const double capacity=double(a[VisibleCapacity])+double(a[DeviceCapacity]);
                line("VRAM allocated "+mtop::meter(used,capacity,std::min(24u,columns>65?columns-60:8u))+
                    " "+decimal(used/1073741824.0)+" / "+decimal(capacity/1073741824.0)+" GiB (driver pools)");
                line("  CPU-visible used "+accountingSize(*d,VisibleUsed)+" / "+accountingSize(*d,VisibleCapacity)+
                    "    GPU-only used "+accountingSize(*d,DeviceUsed)+" / "+accountingSize(*d,DeviceCapacity));
            } else line("VRAM allocation: unavailable until GPU initialization");
            if (mtop::hasSoftware(*d)) {
                uint64_t pending=0,failed=0;
                for (const auto &e:d->software.engines) {pending+=e.pending;failed+=e.failed;}
                line("WORK  queues "+std::to_string(d->software.activeQueues)+"  pending jobs "+std::to_string(pending)+
                    "  queued packets "+std::to_string(d->software.queuedPackets)+"  failed "+std::to_string(failed)+
                    "  clients "+std::to_string(d->software.participants)+
                    ((d->software.flags&amdgpu::software_stats::QueueSampleIncomplete)?" (partial snapshot)":""));
            } else line("Work counters: "+(d->softwareError.empty() ? std::string("unavailable") : d->softwareError));
            // Live sparkline history rows (60 s window, 60-column fixed scale;
            // refresh speed never changes the time axis). Pixel protocols add
            // a small RGB chart per row; the block sparkline stays as fallback.
            const unsigned pixelRows=interactive && graphics!=mtop::Graphics::Text ? 5 : 0;
            const unsigned sparkColumns=60;
            const auto utilizationBuckets=history.buckets(sparkColumns,now,&mtop::ActivityPoint::busyPercent);
            const auto vramBuckets=history.buckets(sparkColumns,now,&mtop::ActivityPoint::allocatedGiB);
            const auto tempBuckets=history.buckets(sparkColumns,now,&mtop::ActivityPoint::temperatureC);
            const auto smuBuckets=history.buckets(sparkColumns,now,&mtop::ActivityPoint::gfxPercent);
            const auto copyBuckets=history.buckets(sparkColumns,now,&mtop::ActivityPoint::transferMiBPerSecond);
            const auto vramCeiling=capacityGiB(*d).value_or(1.0);
            if (pixelRows) {
                images.push_back({unsigned(lines.size()+1),1,mtop::chartPNG(utilizationBuckets,480,18,100)});
                images.push_back({unsigned(lines.size()+2),1,mtop::chartPNG(vramBuckets,480,18,vramCeiling,{230,181,74})});
                images.push_back({unsigned(lines.size()+3),1,mtop::chartPNG(tempBuckets,480,18,100,{240,120,90})});
                images.push_back({unsigned(lines.size()+4),1,mtop::chartPNG(smuBuckets,480,18,100,{120,160,200})});
                images.push_back({unsigned(lines.size()+5),1,mtop::chartPNG(copyBuckets,480,18,0.0,{140,200,140})});
                for(unsigned i=0;i<5;++i)line("");
            }
            line(tint(historyLine("GPU UTIL  ","%",utilizationBuckets,
                history.points.empty() ? std::optional<double>{} : history.points.back().busyPercent,100.0),
                "\033[1;36m"));
            line(historyLine("VRAM USED ","GiB",vramBuckets,
                history.points.empty() ? std::optional<double>{} : history.points.back().allocatedGiB,vramCeiling));
            line(historyLine("TEMP      ","C",tempBuckets,tempNow,100.0));
            line("60 s history; sensor capture 1 Hz | util: "+utilizationSource(*d));
            line(tint(historyLine("SMU GFX   ","%",smuBuckets,
                hardwareValue(*d,GfxActivityPercent,now),100.0)+
                "   (raw AverageGfxActivity, uncalibrated)","\033[90m"));
            if (d->metrics.driverInterface==amdgpu::metrics::kCompatibleInterface &&
                (d->metrics.flags&amdgpu::kSMUMetricsLinuxCompatible))
                line("SMU activity: idle can report 100%; the GPU UTIL row is a driver in-flight delta.");
            else line("SMU activity is firmware-reported; the GPU UTIL row is a driver in-flight delta.");
        }
    }
    const std::string footer="[h] fast/slow  [n/p] GPU  [q] quit | "+std::string(mode.label());
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
                         "Native macOS monitor for GPUs bound to MacAMDGPU (build 172+).\n"
                         "Slow: 0.1 s (default), fast: 0.01 s; h toggles, n/p select GPU, q quits.\n"
                         "Live 60 s sparkline histories: GPU utilization (driver in-flight delta, 0..100%),\n"
                         "VRAM used GiB, temperature C, plus the raw SMU AverageGfxActivity for reference.\n"
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
            amdgpu::software_stats::Counters counters;counters.reset(t);d.software=counters.snapshot(t,true);d.softwareSupported=true;
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
                dimensions.ws_col ? dimensions.ws_col : 100,dimensions.ws_row ? dimensions.ws_row : 40,sampledAt,graphics,&frame,demo);
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
                if (key == 'h') {mode.toggle();if(sampler)sampler->period(mode.milliseconds());}
                if (key == 'n' || key == 'p') selected.step(devices, key == 'n');
            }
        } else if (ready < 0 && errno != EINTR) {
            std::cerr << "Input wait failed\n"; return 1;
        }
    }
    return result;
}
