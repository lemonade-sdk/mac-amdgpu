// Standalone owner diagnostic. The normal HSA library has no diagnostic access.
#define MAC_HSA_IDLE_DIAGNOSTIC 1
#include "../src/transport_iokit.cpp"
#include "../../dext/amdgpu/amdgpu_metrics_state.h"
#include "../../dext/amdgpu/amdgpu_software_stats.h"
#include <charconv>
#include <csignal>
#include <stdexcept>
#include <time.h>

namespace mac_hsa { namespace {
struct IdleDiagnosticAccess {
    static hsa_status_t initialize(IOKitConnection &c) {
        std::lock_guard lock(c.sessionMutex);
        return c.ensureReady();
    }
    static io_connect_t port(const IOKitConnection &c) { return c.ownerPort; }
};
}}
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signalHandler(int) { interrupted = 1; }
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
uint64_t now() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t utc() { return clock_gettime_nsec_np(CLOCK_REALTIME) / 1000000; }
template<size_t N> std::array<uint64_t,N> scalar(io_connect_t port, uint32_t selector) {
    std::array<uint64_t,N> result{};uint32_t count=N;
    const auto status=IOConnectCallScalarMethod(port,selector,nullptr,0,result.data(),&count);
    if(status || count!=N) {
        std::fprintf(stderr,"selector %u transport=%#x outputs=%u expected=%zu\n",selector,unsigned(status),count,N);
        throw std::runtime_error("scalar RPC failed");
    }
    return result;
}
template<class T> T structure(io_connect_t port, uint32_t selector) {
    T result{};size_t size=sizeof(result);
    const auto status=IOConnectCallStructMethod(port,selector,nullptr,0,&result,&size);
    require(!status && size==sizeof(result),"struct RPC failed/size mismatch");
    require(result.version==1 && result.size==sizeof(result),"struct version mismatch");
    return result;
}
bool cleanStop(const std::array<uint64_t,2> &stop) { return stop[0]==0 && stop[1]==6; }
bool validMetrics(const amdgpu::SMUMetricsSnapshot &m, uint64_t sampled) {
    return m.version==1 && m.size==sizeof(m) && !m.status &&
        (m.flags&amdgpu::kSMUMetricsValid) && m.collectedAtNs && sampled>=m.collectedAtNs &&
        sampled-m.collectedAtNs<=amdgpu::kSMUMetricsStaleAfterNs;
}
void printValue(const amdgpu::SMUMetricsSnapshot &m, unsigned index) {
    if(m.validFields&(uint64_t(1)<<index)) std::printf("%llu",(unsigned long long)m.values[index]);
    else std::printf("null");
}
void sample(io_connect_t port, unsigned index) {
    const auto begin=now();
    // Same owning client: selector30 includes two read-only SMU feature queries.
    // There is no queue creation, dispatch, signal runtime or mailbox service.
    const auto live=scalar<12>(port,30);
    const auto collected=scalar<3>(port,46);
    const auto m=structure<amdgpu::SMUMetricsSnapshot>(port,47);
    const auto work=structure<amdgpu::software_stats::Snapshot>(port,61);
    const auto end=now();
    require(amdgpu::software_stats::valid(work),"invalid software snapshot");
    std::printf("{\"event\":\"sample\",\"index\":%u,\"utc_ms\":%llu,\"begin_uptime_ns\":%llu,\"end_uptime_ns\":%llu,",
        index,(unsigned long long)utc(),(unsigned long long)begin,(unsigned long long)end);
    std::printf("\"grbm_status\":%llu,\"grbm_gui_active\":%s,\"grbm_any_active\":%s,\"grbm_cp_busy\":%s,\"cp_stat\":%llu,\"cp_busy\":%s,\"live_raw\":[",
        (unsigned long long)live[0],live[0]&0x80000000?"true":"false",live[0]&0x08000000?"true":"false",
        live[0]&0x20000000?"true":"false",(unsigned long long)live[1],live[1]&0x80000000?"true":"false");
    for(size_t i=0;i<live.size();++i)std::printf("%s%llu",i?",":"",(unsigned long long)live[i]);
    std::printf("],\"collect_status\":%llu,\"metrics_status\":%u,\"metrics_flags\":%u,\"metrics_fresh\":%s,\"interface\":%u,\"generation\":%llu,\"sequence\":%llu,\"collected_uptime_ns\":%llu,\"firmware_counter\":%u,\"valid_fields\":%llu,\"values\":[",
        (unsigned long long)collected[0],m.status,m.flags,validMetrics(m,end)?"true":"false",m.driverInterface,
        (unsigned long long)m.generation,(unsigned long long)m.sequence,(unsigned long long)m.collectedAtNs,
        m.firmwareCounter,(unsigned long long)m.validFields);
    for(unsigned i=0;i<amdgpu::metrics::Count;++i){if(i)std::putchar(',');printValue(m,i);}
    std::printf("],\"gfx_activity_percent\":");printValue(m,amdgpu::metrics::GfxActivityPercent);
    std::printf(",\"umc_activity_percent\":");printValue(m,amdgpu::metrics::UmcActivityPercent);
    std::printf(",\"gfx_average_mhz\":");printValue(m,amdgpu::metrics::GfxClockMHz);
    std::printf(",\"socket_power_mw\":");printValue(m,amdgpu::metrics::SocketPowerMilliwatts);
    std::printf(",\"participants\":%llu,\"active_queues\":%llu,\"queued_packets\":%llu,\"published_packets\":%llu,\"consumed_packets\":%llu,\"engines\":[",
        (unsigned long long)work.participants,(unsigned long long)work.activeQueues,(unsigned long long)work.queuedPackets,
        (unsigned long long)work.publishedPackets,(unsigned long long)work.consumedPackets);
    for(unsigned i=0;i<amdgpu::software_stats::EngineCount;++i){const auto &e=work.engines[i];
        std::printf("%s{\"submitted\":%llu,\"completed\":%llu,\"pending\":%llu,\"failed\":%llu}",i?",":"",
            (unsigned long long)e.submitted,(unsigned long long)e.completed,(unsigned long long)e.pending,(unsigned long long)e.failed);}
    std::printf("]}\n");
    require(live[0]!=UINT32_MAX && live[1]!=UINT32_MAX && live[7]==1,"invalid MMIO or incomplete bringup");
    require(work.participants==1 && !work.activeQueues && !work.queuedPackets,"foreign owner/queue appeared; idle comparison invalid");
    for(const auto &e:work.engines)require(!e.pending,"outstanding work invalidates idle comparison");
    require(!collected[0] && validMetrics(m,end),"sensor capture failed or stale");
}
int checkOffline() {
    require(cleanStop({0,6}) && !cleanStop({0,5}) && !cleanStop({1,6}),"stop validation");
    amdgpu::SMUMetricsSnapshot m{};m.version=1;m.size=sizeof(m);m.flags=amdgpu::kSMUMetricsValid;m.collectedAtNs=1;
    require(validMetrics(m,2) && !validMetrics(m,0) && !validMetrics(m,amdgpu::kSMUMetricsStaleAfterNs+2),"freshness validation");
    m.status=1;require(!validMetrics(m,2),"operation failure validation");
    std::puts("PASS: offline stop/freshness/error checks; no IOKit calls");return 0;
}
}
int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==2 && !std::strcmp(argv[1],"--check"))return checkOffline();
    if(argc<2 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --check | --run [--seconds 8..12] [--registry HEX]\nOwns and initializes a stopped GPU, samples idle state, then resets/stops it. No workload is submitted.\n",argv[0]);return 2;
    }
    unsigned seconds=10;uint64_t registry=0;
    for(int i=2;i<argc;++i){
        if(++i>=argc)return 2;
        const std::string option=argv[i-1],value=argv[i];
        if(option=="--seconds"){
            const auto r=std::from_chars(value.data(),value.data()+value.size(),seconds);
            if(r.ec!=std::errc{} || r.ptr!=value.data()+value.size() || seconds<8 || seconds>12)return 2;
        } else if(option=="--registry"){
            const char *start=value.data();if(value.starts_with("0x"))start+=2;
            const auto r=std::from_chars(start,value.data()+value.size(),registry,16);
            if(r.ec!=std::errc{} || r.ptr!=value.data()+value.size() || !registry)return 2;
        } else return 2;
    }
    std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);
    std::shared_ptr<mac_hsa::IOKitConnection> owner;
    bool attempted=false,passed=false;
    try {
        std::vector<std::shared_ptr<mac_hsa::Connection>> devices;
        require(mac_hsa::discover(devices)==HSA_STATUS_SUCCESS,"discover failed");
        for(const auto &device:devices){mac_hsa::DeviceSnapshot d{};require(device->read(d)==HSA_STATUS_SUCCESS,"identity failed");
            if(registry && d.registryID!=registry)continue;
            require(!owner,"multiple devices; choose --registry");
            require(d.build>=193 && d.stage==0,"requires driver193+ stopped stage0 GPU");
            owner=std::dynamic_pointer_cast<mac_hsa::IOKitConnection>(device);
        }
        require(bool(owner),"selected device unavailable");devices.clear();
        {
            io_connect_t probe=IO_OBJECT_NULL;
            require(IOServiceOpen(owner->service,mach_task_self(),0,&probe)==KERN_SUCCESS,"preflight open failed");
            struct CloseProbe { io_connect_t port;~CloseProbe(){IOServiceClose(port);} } close{probe};
            const auto work=structure<amdgpu::software_stats::Snapshot>(probe,61);
            require(amdgpu::software_stats::valid(work) && !work.participants && !work.activeQueues &&
                !(work.flags&amdgpu::software_stats::RuntimeReady),"pre-existing owner/session; leave it untouched");
        }
        std::printf("{\"event\":\"initialize\",\"registry\":%llu,\"utc_ms\":%llu,\"uptime_ns\":%llu}\n",
            (unsigned long long)owner->registryID,(unsigned long long)utc(),(unsigned long long)now());
        attempted=true;
        require(mac_hsa::IdleDiagnosticAccess::initialize(*owner)==HSA_STATUS_SUCCESS,"initialization failed");
        mac_hsa::DeviceSnapshot ready{};require(owner->read(ready)==HSA_STATUS_SUCCESS && ready.stage==15,"not initialized");
        std::printf("{\"event\":\"initialized_idle\",\"stage\":15,\"utc_ms\":%llu,\"uptime_ns\":%llu}\n",(unsigned long long)utc(),(unsigned long long)now());
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<=seconds && !interrupted;++i){
            std::this_thread::sleep_until(start+std::chrono::seconds(i));
            if(interrupted)break;
            sample(mac_hsa::IdleDiagnosticAccess::port(*owner),i);
        }
        passed=!interrupted;
    } catch(const std::exception &e){std::fprintf(stderr,"FAIL: %s\n",e.what());}
    if(attempted && owner && mac_hsa::IdleDiagnosticAccess::port(*owner)) {
        bool stopped=false;
        try {
            const auto stop=scalar<2>(mac_hsa::IdleDiagnosticAccess::port(*owner),42);
            mac_hsa::DeviceSnapshot after{};
            stopped=cleanStop(stop) && owner->read(after)==HSA_STATUS_SUCCESS && after.stage==0;
            std::printf("{\"event\":\"stop\",\"status\":%llu,\"phase\":%llu,\"verified_stage0\":%s,\"utc_ms\":%llu}\n",
                (unsigned long long)stop[0],(unsigned long long)stop[1],stopped?"true":"false",(unsigned long long)utc());
        }catch(const std::exception &e){std::fprintf(stderr,"Stop failed: %s\n",e.what());}
        if(!stopped){
            std::fprintf(stderr,"RETIREMENT UNCONFIRMED: owner connection retained. No host/shared GPU buffers were created. Process remains alive; inspect/power-cycle before terminating.\n");
            // Do not release the owner and provoke another implicit shutdown.
            // Driver session backing remains retained until reset or detach.
            for(;;)std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    owner.reset();
    std::printf("{\"event\":\"result\",\"passed\":%s}\n",passed?"true":"false");return passed?0:1;
}
