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
    assert(output.find("0.23 GiB / 0.00 GiB / 0.23 GiB / 0.23 GiB") != std::string::npos);
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
    assert(output.find("avg -- | SMU raw 1000 | raw peak 1200")!=std::string::npos);
    assert(output.find("DPM min 500 | AC DPM max 2900 MHz")!=std::string::npos);
    assert(output.find("\"gfx\":{\"raw_current_mhz\":1000,\"dpm_min_mhz\":500,\"ac_dpm_max_mhz\":2900}")!=std::string::npos);
    assert(output.find("\"retired_packets\":0")!=std::string::npos);
    assert(output.find("[h] fast/slow")!=std::string::npos && output.find("FAST 0.1 s")!=std::string::npos);
    // A narrow terminal is clipped by columns and reserves its final row for controls.
    captured.str("");captured.clear();previous=std::cout.rdbuf(captured.rdbuf());
    dashboard({d},selected,{},true,false,&histories,{},40,12,now);
    std::cout.rdbuf(previous);output=captured.str();
    std::string plain;
    for(size_t i=0;i<output.size();++i) {
        if(output[i]=='\033' && i+1<output.size() && output[i+1]=='[') {
            i+=2;while(i<output.size() && !(output[i]>='@' && output[i]<='~')) ++i;
        } else plain+=output[i];
    }
    std::istringstream rows(plain);std::string row;unsigned count=0;
    while(std::getline(rows,row)) {assert(row.size()<=40);++count;}
    assert(count<=12 && plain.find("[h] fast/slow")!=std::string::npos);
}
