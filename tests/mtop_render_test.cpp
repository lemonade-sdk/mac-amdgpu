// Exercise the actual renderers with a synthetic driver response; no IOKit.
#define main mtop_program_main
#include "../amdgpu_mtop/main.cpp"
#undef main
#include <cassert>
namespace mtop {
std::vector<Device> discover(std::string &) { return {}; }
}

int main() {
    using namespace amdgpu;
    using namespace vram_accounting;
    mtop::Device d;
    d.registry = 1; d.build = 178; d.stage = 15;
    VRAMBumpAllocator low, high;
    low.init(24 * 1024 * 1024, 232 * 1024 * 1024);
    high.init(256 * 1024 * 1024, 767 * 1024 * 1024);
    d.accounting = snapshot(true, 0, 1024 * 1024 * 1024, 256 * 1024 * 1024, low, high);
    d.accountingSupported = true;
    assert(mtop::hasAccounting(d));
    std::ostringstream captured;
    auto *previous = std::cout.rdbuf(captured.rdbuf());
    mtop::Selection selected;
    selected.registry = 1;
    json({d}, selected, {});
    dashboard({d}, selected, {}, false, false);
    std::cout.rdbuf(previous);
    auto output = captured.str();
    assert(output.find("\"visible_pool_used_bytes\":0") != std::string::npos);
    assert(output.find("\"device_pool_free_bytes\":804257792") != std::string::npos);
    assert(output.find("\"excluded_bytes\":26214400") != std::string::npos);
    assert(output.find("\"vram_used_bytes\":null") != std::string::npos);
    assert(output.find("\"umc_activity_percent\":null") != std::string::npos);
    assert(output.find("CPU-visible used 0.00 GiB / 0.23 GiB") != std::string::npos);
    d.accounting = snapshot(false, 0, 0, 0, low, high);
    assert(!mtop::hasAccounting(d));
    captured.str(""); captured.clear();
    previous = std::cout.rdbuf(captured.rdbuf());
    json({d}, selected, {});
    std::cout.rdbuf(previous);
    assert(captured.str().find("\"visible_pool_used_bytes\":null") != std::string::npos);
    d.accountingSupported = false;
    assert(!mtop::hasAccounting(d));
    // Live software counters remain distinct from hardware utilization.
    software_stats::Counters counters;
    const auto now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    counters.reset(now-1000000000);
    assert(counters.begin(software_stats::SDMA0,now-500000000));
    counters.complete(software_stats::SDMA0,now-200000000,1048576,software_stats::HostToDevice);
    d.softwareSupported=true;d.software=counters.snapshot(now,true);
    assert(mtop::hasSoftware(d));
    d.clocksSupported=true;
    d.clocks.version=1;d.clocks.size=sizeof(d.clocks);
    d.clocks.flags=kSMUMetricsValid;d.clocks.collectedAtNs=now;
    d.clocks.currentValid=d.clocks.limitsValid=5;
    d.clocks.currentMHz[0]=1000;d.clocks.currentMHz[2]=1500;
    d.clocks.minimumMHz[0]=500;d.clocks.maximumACMHz[0]=2900;
    assert(mtop::freshClocks(d,now));
    assert(!mtop::freshClocks(d,now+kSMUMetricsStaleAfterNs+1));
    assert(clockValue(d,0,now+kSMUMetricsStaleAfterNs+1,2)==2900);
    d.clocks.status=1;
    assert(clockValue(d,0,now)==1000); // Range query failure does not invalidate current clocks.
    d.clocks.status=0;
    Histories histories;
    histories[d.registry].clocks(1200,1800);
    histories[d.registry].clocks(1000,1500);
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    json({d},selected,{});
    dashboard({d},selected,{},false,false,&histories,mtop::RefreshMode{true},80,30,now);
    std::cout.rdbuf(previous);output=captured.str();
    assert(output.find("\"host_to_device_bytes\":1048576")!=std::string::npos);
    assert(output.find("\"gfx_activity_percent\":null")!=std::string::npos);
    assert(output.find("raw GFX 1000  MEM 1500")!=std::string::npos);
    assert(output.find("DPM min / AC max  GFX 500 / 2900")!=std::string::npos);
    assert(output.find("\"gfx\":{\"raw_current_mhz\":1000,\"dpm_min_mhz\":500,\"ac_dpm_max_mhz\":2900}")!=std::string::npos);
    assert(output.find("\"retired_packets\":0")!=std::string::npos);
    assert(output.find("[h] fast/slow")!=std::string::npos && output.find("FAST 0.01 s")!=std::string::npos);
    assert(output.find("GPU UTIL  ")!=std::string::npos);
    assert(output.find("VRAM USED ")!=std::string::npos);
    assert(output.find("TEMP      ")!=std::string::npos);
    assert(output.find("SMU GFX   ")!=std::string::npos);
    assert(output.find("util: driver dispatch-in-flight telemetry")!=std::string::npos);
    assert(output.find("WORK  queues")!=std::string::npos);
    // A low absolute hardware percentage must not fill the chart by being
    // normalized to its own peak. Compatible firmware retains its raw reading.
    d.telemetrySupported=true;
    d.metrics.version=kSMUMetricsSnapshotVersion;d.metrics.size=sizeof(d.metrics);
    d.metrics.driverInterface=metrics::kCompatibleInterface;
    d.metrics.flags=kSMUMetricsValid|kSMUMetricsLinuxCompatible;
    d.metrics.collectedAtNs=now;
    d.metrics.validFields=uint64_t(1)<<metrics::GfxActivityPercent;
    d.metrics.values[metrics::GfxActivityPercent]=5;
    histories[d.registry].add(now,{}, {},5.0);
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    dashboard({d},selected,{},false,false,&histories,{},80,40,now);
    json({d},selected,{});
    std::cout.rdbuf(previous);output=captured.str();
    assert(output.find("SMU GFX   ")!=std::string::npos);
    assert(output.find("5.0 %")!=std::string::npos);
    assert(output.find("\"gfx_activity_percent\":5")!=std::string::npos);
    assert(output.find("idle can report 100%")!=std::string::npos);
    assert(output.find("\"gfx_activity_accuracy\":\"idle_100_percent_observed; workload_utilization_unverified\"")!=std::string::npos);
    assert(output.find("firmware-reported activity; not CU occupancy or productive workload utilization")!=std::string::npos);
    // The raw SMU row shows the 1-block level (5% of 0..100), never a full bar.
    const auto smu=output.find("SMU GFX   ");
    const auto smuEnd=output.find('\n',smu);
    const auto plot=output.substr(smu,smuEnd-smu);
    const std::string fullBar=std::string(1,char(0xE2))+std::string(1,char(0x96))+std::string(1,char(0x88));
    assert(plot.find(fullBar)==std::string::npos); // no full bar at 5%
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    dashboard({d},selected,{},false,false,&histories,{},80,40,now+kSMUMetricsStaleAfterNs+1);
    std::cout.rdbuf(previous);output=captured.str();
    assert(output.find("5.0 %")==std::string::npos); // stale headline cleared
    assert(output.find("SUBMISSIONS  ")==std::string::npos);
    // Keep the raw100% reading alongside the warning; do not replace it with
    // a software-derived zero even when driver counters show no pending work.
    d.metrics.values[metrics::GfxActivityPercent]=100;
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    dashboard({d},selected,{},false,false,&histories,{},80,40,now);
    json({d},selected,{});
    std::cout.rdbuf(previous);output=captured.str();
    assert(output.find("SMU GFX   ")!=std::string::npos);
    assert(output.find("100.0 %")!=std::string::npos);
    assert(output.find("\"gfx_activity_percent\":100")!=std::string::npos);
    assert(output.find("idle can report 100%")!=std::string::npos);
    // Common dashboard heights retain all primary data and both complete charts.
    for(unsigned height:{35u,42u}) {
        mtop::Frame layout;
        captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
        dashboard({d},selected,{},true,false,&histories,{},110,height,now,mtop::Graphics::Text,&layout);
        std::cout.rdbuf(previous);
        std::string visible;for(auto&row:layout.previous)visible+=row+"\n";
        for(const char*label:{"GPU  gfx","DPM min / AC max","POWER  ","VRAM allocation:","GPU UTIL  ","VRAM USED ","TEMP      ","SMU GFX   ","60 s history"})
            assert(visible.find(label)!=std::string::npos);
    }
    // A narrow terminal is clipped by columns and reserves its final row for controls.
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    mtop::Frame frame;
    dashboard({d},selected,{},true,false,&histories,{},40,12,now,mtop::Graphics::Text,&frame);
    std::cout.rdbuf(previous);output=captured.str();
    assert(frame.previous.size()==12);
    for(const auto &row:frame.previous) assert(mtop::clipColumns(row,39)==row);
    assert(frame.previous.back().find("[h] fast/slow")!=std::string::npos);
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    dashboard({d},selected,{},true,false,&histories,{},40,12,now,mtop::Graphics::Text,&frame);
    std::cout.rdbuf(previous);
    assert(captured.str().empty()); // unchanged screen writes no clears or text
}
