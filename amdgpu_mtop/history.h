#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace mtop {
struct RateCounters {
    uint64_t generation=0, timeNs=0, submitted=0, completedBytes=0;
    std::array<uint64_t,5> directions{};
};
struct ActivityPoint {
    uint64_t timeNs=0;
    std::optional<double> submissionsPerSecond, transferMiBPerSecond, allocatedGiB;
    std::optional<double> gfxPercent;
    std::array<std::optional<double>,5> copyMiBPerSecond{};
};
// Fixed time window: switching between 100 ms and 500 ms does not change the
// graph's time scale. A disconnected/reset interval never becomes a rate spike.
struct History {
    static constexpr uint64_t windowNs=60'000'000'000ull;
    std::deque<ActivityPoint> points;
    std::optional<RateCounters> previous;
    std::deque<RateCounters> rateWindow;
    std::optional<double> peakGfxClockMHz, peakMemoryClockMHz;
    std::optional<double> minimumGfxClockMHz, minimumMemoryClockMHz;
    void clocks(std::optional<double> gfx, std::optional<double> memory) {
        if (gfx && (!minimumGfxClockMHz || *gfx<*minimumGfxClockMHz)) minimumGfxClockMHz=gfx;
        if (memory && (!minimumMemoryClockMHz || *memory<*minimumMemoryClockMHz)) minimumMemoryClockMHz=memory;
        if (gfx && (!peakGfxClockMHz || *gfx>*peakGfxClockMHz)) peakGfxClockMHz=gfx;
        if (memory && (!peakMemoryClockMHz || *memory>*peakMemoryClockMHz)) peakMemoryClockMHz=memory;
    }
    void add(uint64_t now, std::optional<RateCounters> counters,
             std::optional<double> allocatedGiB, std::optional<double> gfxPercent={}) {
        ActivityPoint point{now,{}, {},allocatedGiB,gfxPercent};
        if (counters) {
            const bool reset=previous && (counters->generation!=previous->generation ||
                counters->timeNs<previous->timeNs || counters->submitted<previous->submitted ||
                counters->completedBytes<previous->completedBytes ||
                counters->timeNs-previous->timeNs>2'000'000'000ull ||
                !std::equal(counters->directions.begin(),counters->directions.end(),
                            previous->directions.begin(),[](auto a,auto b){return a>=b;}));
            if (reset || !previous) {
                rateWindow.clear();
                if (reset) {
                    points.clear();
                    peakGfxClockMHz.reset();peakMemoryClockMHz.reset();
                    minimumGfxClockMHz.reset();minimumMemoryClockMHz.reset();
                }
            }
            // Duplicate snapshots are not zero-byte samples. In particular, a
            // redraw while the observer is busy must not manufacture a rate.
            if (!previous || counters->timeNs>previous->timeNs || reset) {
                rateWindow.push_back(*counters);
                while (rateWindow.size()>2 && counters->timeNs-rateWindow[1].timeNs>=1'000'000'000ull)
                    rateWindow.pop_front();
                if (rateWindow.size()>1) {
                    const auto &base=rateWindow.front();
                    const auto elapsed=counters->timeNs-base.timeNs;
                    // Fixed one-second average; startup has no false instant peak.
                    if (elapsed>=1'000'000'000ull) {
                        const double seconds=double(elapsed)/1e9;
                        point.submissionsPerSecond=double(counters->submitted-base.submitted)/seconds;
                        point.transferMiBPerSecond=double(counters->completedBytes-base.completedBytes)/seconds/1048576.0;
                        for(size_t i=0;i<5;++i)
                            point.copyMiBPerSecond[i]=double(counters->directions[i]-base.directions[i])/seconds/1048576.0;
                    }
                }
            } else if (!points.empty()) {
                point.submissionsPerSecond=points.back().submissionsPerSecond;
                point.transferMiBPerSecond=points.back().transferMiBPerSecond;
                point.copyMiBPerSecond=points.back().copyMiBPerSecond;
            }
            previous=counters;
        } else {previous.reset();rateWindow.clear();}
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
        std::vector<unsigned> counts(width);
        for (const auto &point:points) {
            if (point.timeNs>now || now-point.timeNs>=windowNs) continue;
            const auto value=point.*field;
            if (!value || !std::isfinite(*value) || *value<0) continue;
            const size_t ageBucket=size_t((now-point.timeNs)*width/windowNs);
            const size_t column=width-1-std::min(width-1,ageBucket);
            result[column]=result[column].value_or(0)+*value;
            ++counts[column];
        }
        for(size_t i=0;i<width;++i) if(counts[i]) result[i]=*result[i]/counts[i];
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
// UTF-8 Braille: two horizontal by four vertical samples per terminal cell.
inline std::vector<std::string> graph(const std::vector<std::optional<double>> &values,
                                     unsigned height, double ceiling) {
    const size_t columns=(values.size()+1)/2;
    std::vector<std::string> rows(height);
    if (!height) return rows;
    constexpr unsigned dots[2][4]={{0,1,2,6},{3,4,5,7}};
    for(unsigned row=0;row<height;++row) for(size_t col=0;col<columns;++col) {
        unsigned mask=0; bool present=false;
        for(unsigned side=0;side<2;++side) {
            const size_t x=2*col+side;
            if(x>=values.size() || !values[x] || !std::isfinite(*values[x]) || !(ceiling>0)) continue;
            present=true;
            const unsigned level=unsigned(std::lround(std::clamp(*values[x]/ceiling,0.0,1.0)*(height*4-1)));
            const unsigned top=height*4-1-level;
            if(top/4==row) mask|=1u<<dots[side][top%4];
        }
        if(mask) {
            const unsigned cp=0x2800+mask;
            rows[row]+=char(0xe0|(cp>>12));rows[row]+=char(0x80|((cp>>6)&63));rows[row]+=char(0x80|(cp&63));
        } else rows[row]+= !present && row==height-1 ? '.' : ' ';
    }
    return rows;
}
} // namespace mtop
