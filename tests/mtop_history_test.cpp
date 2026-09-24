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
    assert(std::abs(*h.points.back().submissionsPerSecond-20)<0.001);
    assert(std::abs(*h.points.back().transferMiBPerSecond-10)<0.001);
    // Slow refresh changes elapsed time, not graph span or rate units.
    h.add(sec+sec*6/10,RateCounters{1,sec+sec*6/10,22,6291456},2.0);
    assert(std::abs(*h.points.back().submissionsPerSecond-20)<0.001);
    auto buckets=h.buckets(60,sec+sec*6/10,&ActivityPoint::allocatedGiB);
    assert(buckets.back()==2.0 && !buckets.front());
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
    assert(plot.size()==2 && plot[0]=="  # " && plot[1]==" ##.");
    assert(meter(20,10,4)=="[####]");
    assert(meter(0,0,4)=="[unavailable]");
    RefreshMode mode;assert(mode.milliseconds()==500);mode.toggle();assert(mode.milliseconds()==100);mode.toggle();assert(mode.milliseconds()==500);
}
