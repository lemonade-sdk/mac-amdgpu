#include "../amdgpu_mtop/history.h"
#include <cassert>
#include <cmath>
int main() {
    using namespace mtop;
    constexpr uint64_t sec=1'000'000'000;
    History h;
    h.add(sec,RateCounters{1,sec,10,0},1.0);
    assert(!h.points.back().submissionsPerSecond);
    h.add(sec+sec/10,RateCounters{1,sec+sec/10,12,1048576},1.5);
    assert(!h.points.back().submissionsPerSecond);
    assert(!h.points.back().transferMiBPerSecond);
    // Slow refresh changes elapsed time, not graph span or rate units.
    h.add(sec+sec*6/10,RateCounters{1,sec+sec*6/10,22,6291456},2.0);
    assert(!h.points.back().submissionsPerSecond);
    auto buckets=h.buckets(60,sec+sec*6/10,&ActivityPoint::allocatedGiB);
    assert(buckets.back()==1.5 && !buckets.front());
    h.add(sec*2,RateCounters{2,sec*2,0,0},0.0);
    assert(h.points.size()==1 && !h.points.back().transferMiBPerSecond);
    h.add(sec*3,{},{});
    h.add(sec*4,RateCounters{2,sec*4,500,100000000},1.0);
    assert(!h.points.back().submissionsPerSecond); // reconnect is not a burst
    h.add(sec*5,RateCounters{2,sec*5,501,101048576},1.0);
    assert(h.points.back().submissionsPerSecond==1.0 && h.points.back().transferMiBPerSecond==1.0);
    h.add(sec*6,RateCounters{2,sec*6,1,0},1.0);
    assert(!h.points.back().submissionsPerSecond); // unexpected counter rollback
    h.add(sec*66,RateCounters{2,sec*66,2,0},1.0);
    assert(!h.points.back().submissionsPerSecond); // stale interval
    assert(h.points.front().timeNs>=sec*6);
    assert(h.buckets(0,sec*66,&ActivityPoint::allocatedGiB).empty());
    auto plot=graph({0.0,0.5,1.0,{}},2,1.0);
    assert(plot.size()==2 && plot[0].find("#")==std::string::npos);
    History burst;
    for(unsigned i=0;i<=10;++i)burst.add(sec+i*sec/10,RateCounters{1,sec+i*sec/10,i,i==10?104857600u:0u},{});
    assert(burst.points.back().transferMiBPerSecond==100.0); // not1000MiB/s at100ms completion boundary
    burst.add(sec*2+sec/10,RateCounters{1,sec*2+sec/10,10,104857600},{});
    assert(burst.points.back().transferMiBPerSecond==100.0);
    burst.add(sec*2+sec/10,RateCounters{1,sec*2+sec/10,10,104857600},{});
    assert(burst.points.back().transferMiBPerSecond==100.0); // duplicate not zero
    burst.add(sec*3+sec/10,RateCounters{1,sec*3+sec/10,10,104857600},{});
    assert(burst.points.back().transferMiBPerSecond==0.0);
    burst.clocks(100,200);burst.clocks(90,250);
    assert(burst.minimumGfxClockMHz==90 && burst.peakMemoryClockMHz==250);
    RateCounters reset{2,sec*4,0,0};reset.directions[0]=100;
    burst.add(sec*4,reset,{});
    assert(!burst.peakMemoryClockMHz && !burst.points.back().transferMiBPerSecond);
    reset.timeNs+=sec;reset.directions[0]=10;burst.add(sec*5,reset,{});
    assert(!burst.points.back().transferMiBPerSecond); // per-direction rollback invalidates baseline

    assert(meter(20,10,4)=="[####]");
    assert(meter(0,0,4)=="[unavailable]");
    RefreshMode mode;assert(mode.milliseconds()==100);mode.toggle();assert(mode.milliseconds()==10);mode.toggle();assert(mode.milliseconds()==100);
    // True delta-based utilization: busy counter delta over the wall window.
    assert(!busyRatio(1000,0,0));                       // no elapsed window
    assert(!busyRatio(100,200,1'000'000'000ull));        // rollback is not -90%
    auto r=busyRatio(500'000'000ull,0,1'000'000'000ull); assert(r && *r==50.0);
    r=busyRatio(2'000'000'000ull,0,1'000'000'000ull);    // saturates at 100, never >100
    assert(r && *r==100.0);
    r=busyRatio(900'000'000ull,500'000'000ull,1'000'000'000ull); // window delta = 40%
    assert(r && *r==40.0);
    // 60-second sparkline renderer: known series -> expected bar string.
    std::vector<std::optional<double>> series{0.0,25.0,50.0,75.0,100.0,100.0,5.0,{}};
    // Expected: 0->blank, 25%->▂, 50%->▄, 75%->▆, 100%->█, 100%->█, 5%->▁, empty->space
    // Actual UTF-8 from the measured run: 20 e29681 e29683 e29685 e29687 e29687 e29680 20
    const std::string expected=std::string(" \xe2\x96\x81\xe2\x96\x83\xe2\x96\x85\xe2\x96\x87\xe2\x96\x87\xe2\x96\x80 ");
    assert(sparkline(series,100.0)==expected);
    assert(sparkline({1.0,2.0},0.0).empty()); // no ceiling, no bars
    assert(sparkline({-1.0,0.0},10.0)=="  ");  // non-positive is blank
    // min/max/avg summary over present values.
    std::optional<std::array<double,3>> s;
    { std::optional<double> a=10.0, b={}, c=30.0, dd=20.0;
      auto vals=std::vector<std::optional<double>>{a,b,c,dd}; s=stats(vals); }
    assert(s && s->at(0)==10.0 && s->at(1)==30.0 && s->at(2)==20.0);
    { auto none=std::vector<std::optional<double>>{std::nullopt,std::nullopt}; assert(!stats(none)); }
    // History retains the busy percentage next to the raw SMU field.
    History busy;
    busy.add(sec,{},1.0,90.0,55.0);
    assert(busy.points.back().gfxPercent==90.0 && !busy.points.back().busyPercent);
    assert(busy.points.back().temperatureC==55.0);
    auto busyBuckets=busy.buckets(60,sec,&ActivityPoint::busyPercent);
    assert(std::all_of(busyBuckets.begin(),busyBuckets.end(),[](const auto&v){return !v;}));

}
