#include "runtime_state.h"
#include <hsa/amd_hsa_queue.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <set>

#include <thread>

namespace mac_hsa {
struct ServiceConnection final:Connection {
    std::mutex mutex;
    std::map<uint64_t,void *> shared;
    std::map<uint64_t,std::vector<uint8_t>> device;
    std::set<uint64_t> active;
    uint64_t next=0;
    std::atomic<unsigned> calls{0},destroyed{0},freed{0};
    std::atomic<bool> fail{false};
    bool supportsBuffers() const override {return true;}
    hsa_status_t read(DeviceSnapshot &snapshot) override {
        snapshot={1,190,15,256ull<<20,32ull<<30,12,0,1};return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateSharedBuffer(uint64_t bytes,SharedBuffer &out) override {
        std::lock_guard lock(mutex);void *pointer=nullptr;
        if (posix_memalign(&pointer,16384,bytes)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        out={{++next,reinterpret_cast<uintptr_t>(pointer),bytes},pointer,0};shared[out.device.handle]=pointer;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override {
        uint64_t timestamp;hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&timestamp);
        std::lock_guard lock(mutex);assert(shared.contains(buffer.device.handle));
        std::free(shared.at(buffer.device.handle));shared.erase(buffer.device.handle);++freed;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateBuffer(uint64_t bytes,DeviceBuffer &out) override {
        std::lock_guard lock(mutex);const auto handle=++next;
        out={handle,0x8000000000ull+handle*0x100000,((bytes+16383)&~uint64_t(16383))};
        device[handle].resize(out.size);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        std::lock_guard lock(mutex);assert(device.erase(buffer.handle)==1);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t writeBuffer(const DeviceBuffer &buffer,uint64_t offset,const void *data,size_t bytes) override {
        std::lock_guard lock(mutex);auto &target=device.at(buffer.handle);
        assert(offset<=target.size() && bytes<=target.size()-offset);
        std::memcpy(target.data()+offset,data,bytes);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t createQueue(const SharedBuffer &,const SharedBuffer &metadata,uint32_t,uint64_t &handle) override {
        std::lock_guard lock(mutex);handle=++next;active.insert(handle);
        const auto &queue=*static_cast<amd_queue_t *>(metadata.host);
        assert(queue.scratch_wave64_lane_byte_size==0 || queue.scratch_wave64_lane_byte_size==256);
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroyQueue(uint64_t handle) override {
        uint64_t timestamp;hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&timestamp);
        std::lock_guard lock(mutex);assert(active.erase(handle)==1);++destroyed;return HSA_STATUS_SUCCESS;
    }
    hsa_status_t serviceQueue(uint64_t handle,uint64_t &inactive) override {
        uint64_t timestamp;hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&timestamp);
        std::lock_guard lock(mutex);assert(active.contains(handle));++calls;
        inactive=fail ? 4 : 0;return fail ? HSA_STATUS_ERROR_INVALID_ALLOCATION : HSA_STATUS_SUCCESS;
    }
};
static std::shared_ptr<ServiceConnection> backend;
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &out) {
    backend=std::make_shared<ServiceConnection>();out.push_back(backend);return HSA_STATUS_SUCCESS;
}
}
namespace {
using namespace std::chrono_literals;
template<class F> void until(F predicate) {
    const auto end=std::chrono::steady_clock::now()+2s;
    while (!predicate()) {assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(1ms);}
}
hsa_agent_t begin() {
    assert(hsa_init()==0);hsa_agent_t result{};
    assert(hsa_iterate_agents([](hsa_agent_t a,void *opaque) {
        hsa_device_type_t type;assert(hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type)==0);
        if (type==HSA_DEVICE_TYPE_GPU) *static_cast<hsa_agent_t *>(opaque)=a;
        return HSA_STATUS_SUCCESS;
    },&result)==0 && result.handle);return result;
}
struct CallbackState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,release=false;
    std::atomic<unsigned> count{0};
    std::atomic<bool> returned{false};
};
}
int main() {
    {
        const auto gpu=begin();hsa_queue_t *queue=nullptr;
        assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,256,65536,&queue)==0);
        until([] {return mac_hsa::backend->calls.load()>0;});
        assert(hsa_queue_inactivate(queue)==0);
        const auto stopped=mac_hsa::backend->calls.load();
        std::this_thread::sleep_for(5ms);
        assert(mac_hsa::backend->calls.load()==stopped);
        assert(hsa_queue_destroy(queue)==0 && mac_hsa::backend->destroyed==1);
        assert(hsa_shut_down()==0 && mac_hsa::backend->freed==2);
    }
    {
        const auto gpu=begin();hsa_queue_t *queue=nullptr;CallbackState state;
        assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *opaque) {
            assert(status==HSA_STATUS_ERROR_INVALID_ALLOCATION);
            auto &state=*static_cast<CallbackState *>(opaque);++state.count;
            uint64_t stamp;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&stamp)==0);
            std::unique_lock lock(state.mutex);state.entered=true;state.changed.notify_all();
            state.changed.wait(lock,[&] {return state.release;});
        },&state,0,0,&queue)==0);
        mac_hsa::backend->fail=true;
        {std::unique_lock lock(state.mutex);assert(state.changed.wait_for(lock,2s,[&] {return state.entered;}));}
        std::atomic<bool> destroyStarted=false,destroyReturned=false;
        std::thread destroy([&] {destroyStarted=true;assert(hsa_queue_destroy(queue)==0);destroyReturned=true;});
        until([&] {return destroyStarted.load();});
        std::this_thread::sleep_for(10ms);
        assert(!destroyReturned); // callback user data cannot be released yet
        {std::lock_guard lock(state.mutex);state.release=true;state.changed.notify_all();}
        destroy.join();
        assert(destroyReturned && state.count==1 && mac_hsa::backend->freed==2);
        assert(hsa_shut_down()==0);
    }
    {
        const auto gpu=begin();hsa_queue_t *queue=nullptr;CallbackState state;
        assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t,hsa_queue_t *self,void *opaque) {
            auto &state=*static_cast<CallbackState *>(opaque);++state.count;
            assert(hsa_queue_destroy(self)==0);state.returned=true;
        },&state,0,0,&queue)==0);
        mac_hsa::backend->fail=true;until([&] {return state.returned.load();});
        until([] {return mac_hsa::backend->freed.load()==2;});
        assert(state.count==1 && mac_hsa::backend->destroyed==1);
        assert(hsa_shut_down()==0);
    }
    {
        const auto gpu=begin();hsa_queue_t *queue=nullptr;CallbackState state;
        assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t,hsa_queue_t *,void *opaque) {
            auto &state=*static_cast<CallbackState *>(opaque);++state.count;
            assert(hsa_shut_down()==0);state.returned=true;
        },&state,0,0,&queue)==0);
        mac_hsa::backend->fail=true;until([&] {return state.returned.load();});
        until([] {return mac_hsa::backend->freed.load()==2;});
        assert(state.count==1 && mac_hsa::backend->destroyed==1);
        uint64_t stamp;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&stamp)==HSA_STATUS_ERROR_NOT_INITIALIZED);
    }
    {
        const auto gpu=begin();hsa_queue_t *queue=nullptr;hsa_signal_t completion{},dependent{},rejected{};
        assert(hsa_signal_create(7,1,&gpu,&completion)==0);
        assert(hsa_signal_create(9,1,&gpu,&dependent)==0);
        assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,0,0,&queue)==0);
        std::atomic<bool> waiting=false,woke=false;
        hsa_signal_value_t observed=0;
        std::thread waiter([&] {
            waiting=true;
            observed=hsa_signal_wait_scacquire(completion,HSA_SIGNAL_CONDITION_EQ,0,UINT64_MAX,HSA_WAIT_STATE_BLOCKED);
            woke=true;
        });
        until([&] {return waiting.load();});mac_hsa::backend->fail=true;
        until([&] {return woke.load();});waiter.join();
        assert(observed==7); // fault wakes the waiter without inventing completion
        assert(hsa_signal_load_scacquire(dependent)==9);
        assert(hsa_signal_create(1,1,&gpu,&rejected)==HSA_STATUS_ERROR_OUT_OF_RESOURCES && !rejected.handle);
        assert(hsa_queue_destroy(queue)==0);
        assert(hsa_signal_destroy(completion)==0 && hsa_signal_destroy(dependent)==0);
        assert(hsa_shut_down()==0 && mac_hsa::backend->freed==4);
        assert(mac_hsa::backend->device.empty());
    }
    puts("HSA queue services: background polling, error callback join, reentrant destroy/shutdown and storage lifetime passed");
}
