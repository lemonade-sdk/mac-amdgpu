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
    RefreshMode mode;assert(mode.milliseconds()==500);mode.toggle();assert(mode.milliseconds()==100);mode.toggle();assert(mode.milliseconds()==500);
}
