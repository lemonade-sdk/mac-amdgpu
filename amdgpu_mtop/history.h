#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace mtop {
struct RateCounters {
    uint64_t generation=0, timeNs=0, submitted=0, completedBytes=0;
};
struct ActivityPoint {
    uint64_t timeNs=0;
    std::optional<double> submissionsPerSecond, transferMiBPerSecond, allocatedGiB;
    std::optional<double> gfxPercent;
};
// Fixed time window: switching between 100 ms and 500 ms does not change the
// graph's time scale. A disconnected/reset interval never becomes a rate spike.
struct History {
    static constexpr uint64_t windowNs=60'000'000'000ull;
    std::deque<ActivityPoint> points;
    std::optional<RateCounters> previous;
    std::optional<double> peakGfxClockMHz, peakMemoryClockMHz;
    void clocks(std::optional<double> gfx, std::optional<double> memory) {
        if (gfx && (!peakGfxClockMHz || *gfx>*peakGfxClockMHz)) peakGfxClockMHz=gfx;
        if (memory && (!peakMemoryClockMHz || *memory>*peakMemoryClockMHz)) peakMemoryClockMHz=memory;
    }
    void add(uint64_t now, std::optional<RateCounters> counters,
             std::optional<double> allocatedGiB, std::optional<double> gfxPercent={}) {
        ActivityPoint point{now,{}, {},allocatedGiB,gfxPercent};
        if (counters) {
            if (previous && counters->generation!=previous->generation) points.clear();
            if (previous && counters->generation==previous->generation &&
                counters->timeNs>previous->timeNs &&
                counters->timeNs-previous->timeNs<=2'000'000'000ull &&
                counters->submitted>=previous->submitted && counters->completedBytes>=previous->completedBytes) {
                const double seconds=double(counters->timeNs-previous->timeNs)/1e9;
                point.submissionsPerSecond=double(counters->submitted-previous->submitted)/seconds;
                point.transferMiBPerSecond=double(counters->completedBytes-previous->completedBytes)/seconds/1048576.0;
            }
            previous=counters;
        } else previous.reset();
        if (!points.empty() && now<points.back().timeNs) points.clear();
        if (!points.empty() && now==points.back().timeNs) points.back()=point;
        else points.push_back(point);
        while (!points.empty() && now>points.front().timeNs && now-points.front().timeNs>windowNs) points.pop_front();
        while (points.size()>1200) points.pop_front();
    }
    std::vector<std::optional<double>> buckets(size_t width, uint64_t now,
        std::optional<double> ActivityPoint::*field) const {
        std::vector<std::optional<double>> result(width);
        if (!width) return result;
        for (const auto &point:points) {
            if (point.timeNs>now || now-point.timeNs>=windowNs) continue;
            const auto value=point.*field;
            if (!value || !std::isfinite(*value) || *value<0) continue;
            const size_t ageBucket=size_t((now-point.timeNs)*width/windowNs);
            const size_t column=width-1-std::min(width-1,ageBucket);
            if (!result[column] || *value>*result[column]) result[column]=value;
        }
        return result;
    }
};
struct RefreshMode {
    bool fast=false;
    void toggle() {fast=!fast;}
    unsigned milliseconds() const {return fast ? 100 : 500;}
    const char *label() const {return fast ? "FAST 0.1 s" : "SLOW 0.5 s";}
};
inline std::string meter(double used, double capacity, size_t width) {
    if (!(capacity>0) || !std::isfinite(used) || !std::isfinite(capacity)) return "[unavailable]";
    const size_t filled=size_t(std::clamp(used/capacity,0.0,1.0)*double(width));
    return "["+std::string(filled,'#')+std::string(width-filled,'-')+"]";
}
inline std::vector<std::string> graph(const std::vector<std::optional<double>> &values,
                                     unsigned height, double ceiling) {
    std::vector<std::string> rows(height,std::string(values.size(),' '));
    if (!height || !(ceiling>0)) return rows;
    for (size_t x=0;x<values.size();++x) {
        if (!values[x]) { rows.back()[x]='.';continue; }
        const unsigned filled=unsigned(std::ceil(std::clamp(*values[x]/ceiling,0.0,1.0)*height));
        for (unsigned y=0;y<height;++y)
            if (y<filled) rows[height-1-y][x]='#';
    }
    return rows;
}
} // namespace mtop
