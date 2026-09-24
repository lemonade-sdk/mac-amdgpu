#include "../amdgpu_mtop/sampler.h"
#include <cassert>
#include <future>
namespace {
std::mutex mutex;
std::condition_variable change;
bool entered=false,released=false;
}
namespace mtop {
std::vector<Device> discover(std::string&) {
    std::unique_lock lock(mutex);entered=true;change.notify_one();
    change.wait(lock,[]{return released;});
    Device d;d.registry=42;return {d};
}
}
int main() {
    mtop::Sampler sampler(500);
    {std::unique_lock lock(mutex);change.wait(lock,[]{return entered;});}
    // Reading the latest frame and handling a refresh toggle must not wait for
    // a blocked driver call. A future bounds the regression instead of sleeps.
    auto input=std::async(std::launch::async,[&]{sampler.period(100);return sampler.latest();});
    assert(input.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    assert(input.get().sequence==0);
    {std::lock_guard lock(mutex);released=true;}change.notify_one();
}
