#pragma once
#include "model.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace mtop {
struct Observation {std::vector<Device> devices;std::string error;uint64_t sequence=0;};
// Only the worker calls IOKit. Terminal input/redraw never waits for sensors.
class Sampler {
    std::mutex mutex_;
    std::condition_variable changed_;
    Observation latest_;
    bool stopped_=false;
    unsigned period_=500;
    std::thread worker_;
public:
    explicit Sampler(unsigned period):period_(period),worker_([this]{
        for(;;) {
            const auto start=std::chrono::steady_clock::now();
            Observation next;next.devices=discover(next.error);
            std::unique_lock lock(mutex_);
            next.sequence=latest_.sequence+1;latest_=std::move(next);
            if(stopped_)break;
            changed_.wait_until(lock,start+std::chrono::milliseconds(period_));
            if(stopped_)break;
        }
    }) {}
    ~Sampler() {{std::lock_guard lock(mutex_);stopped_=true;}changed_.notify_one();worker_.join();}
    Observation latest(){std::lock_guard lock(mutex_);return latest_;}
    void period(unsigned value){{std::lock_guard lock(mutex_);period_=value;}changed_.notify_one();}
};
} // namespace mtop
