#include "gpu_signal_service.h"
#include "signal_mailbox.h"
#include "signal_state.h"
#include "code_object.h"
#include "signal_mailbox_service_code.h"
#include <hsa/amd_hsa_queue.h>
#include <array>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace mac_hsa {
struct GPUSignalService::State {
    using Clock=std::chrono::steady_clock;
    std::shared_ptr<Connection> connection;
    SharedBuffer arena,mailbox,arguments,ring,metadata;
    DeviceBuffer code;
    CodeObject object;
    std::unique_ptr<SignalMailboxClient> client;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::chrono::milliseconds idle;
    Clock::time_point lastRequest{};
    std::atomic<bool> faulted{false};
    bool stopWorker=false,initialized=false,initializationAttempted=false,released=false;
    uint64_t handle=0;
    uint64_t completedRequests=0;
    bool trace=false;
    static constexpr size_t completionOffset=1024;
    State(std::shared_ptr<Connection> c,const SharedBuffer &a,std::chrono::milliseconds duration)
        :connection(std::move(c)),arena(a),idle(duration) {
        const auto *enabled=std::getenv("MAC_HSA_SIGNAL_TRACE");trace=enabled && std::strcmp(enabled,"1")==0;
    }
    uint64_t completion() const {
        auto *signal=reinterpret_cast<SignalABI *>(static_cast<char *>(metadata.host)+completionOffset);
        return uint64_t(std::atomic_ref<int64_t>(signal->value).load(std::memory_order_acquire));
    }
    bool initialize() {
        if(initializationAttempted)return initialized;
        initializationAttempted=true;
        if(!parseCodeObject(kSignalMailboxServiceCode,object) || object.kernels.size()!=1)return false;
        const auto &k=object.kernels[0];
        if(k.kernargSize!=32 || k.properties!=0x408 || k.privateSize || k.groupSize || k.preload)return false;
        if(connection->allocateBuffer(object.image.size(),code)!=0 || !relocateCodeObject(object,code.address) ||
            connection->writeBuffer(code,0,object.image.data(),object.image.size())!=0)return false;
        for(auto *buffer:{&mailbox,&arguments,&ring,&metadata}) {
            if(connection->allocateSharedBuffer(16384,*buffer)!=0 || !buffer->host || buffer->device.size<16384 ||
                buffer->device.address!=reinterpret_cast<uintptr_t>(buffer->host))return false;
        }
        initialized=true;return true;
    }
    SignalServiceResult start() {
        if(handle)return SignalServiceResult::Success;
        if(!initialize())return SignalServiceResult::Unavailable;
        for(auto *buffer:{&mailbox,&arguments,&ring,&metadata})std::memset(buffer->host,0,16384);
        auto *q=static_cast<amd_queue_t *>(metadata.host);
        q->hsa_queue.type=HSA_QUEUE_TYPE_MULTI;q->hsa_queue.features=HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
        q->hsa_queue.base_address=ring.host;q->hsa_queue.size=64;
        q->queue_properties=AMD_QUEUE_PROPERTIES_IS_PTR64;
        q->read_dispatch_id_field_base_byte_offset=offsetof(amd_queue_t,read_dispatch_id);
        for(unsigned i=0;i<64;++i)static_cast<uint16_t *>(ring.host)[i*32]=HSA_PACKET_TYPE_INVALID;
        auto *done=reinterpret_cast<SignalABI *>(static_cast<char *>(metadata.host)+completionOffset);
        *done={};done->value=1;
        client=std::make_unique<SignalMailboxClient>(static_cast<uint64_t *>(mailbox.host),MailboxWait::Hybrid);
        const auto status=connection->createQueue(ring,metadata,64,handle);
        if(status==HSA_STATUS_ERROR_OUT_OF_RESOURCES && !handle) {
            if(trace)std::fprintf(stderr,"signal-mailbox: unavailable reason=queue-slots\n");
            return SignalServiceResult::Unavailable;
        }
        if(status || !handle) {faulted=true;return SignalServiceResult::Failed;}
        auto *args=static_cast<uint64_t *>(arguments.host);
        args[0]=mailbox.device.address;args[1]=arena.device.address;args[2]=256;args[3]=100000000;
        hsa_kernel_dispatch_packet_t packet{};
        packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
            (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
            (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
        packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
        packet.workgroup_size_x=packet.grid_size_x=32;
        packet.workgroup_size_y=packet.workgroup_size_z=packet.grid_size_y=packet.grid_size_z=1;
        packet.kernel_object=code.address+object.kernels[0].descriptor;packet.kernarg_address=arguments.host;
        packet.completion_signal.handle=metadata.device.address+completionOffset;
        std::memcpy(static_cast<char *>(ring.host)+4,reinterpret_cast<char *>(&packet)+4,60);
        uint32_t header;std::memcpy(&header,&packet,4);
        std::atomic_ref<uint32_t>(*static_cast<uint32_t *>(ring.host)).store(header,std::memory_order_release);
        std::atomic_ref<uint64_t>(const_cast<uint64_t &>(q->write_dispatch_id)).store(1,std::memory_order_release);
        if(connection->kickQueue(handle,0)!=0 ||
            client->await(MAC_MAILBOX_READY,1,Clock::now()+std::chrono::milliseconds(250))!=MailboxResult::Success) {
            faulted=true;retire("startup-failure");return SignalServiceResult::Failed;
        }
        completedRequests=0;
        if(trace)std::fprintf(stderr,"signal-mailbox: ready queue=%llu\n",(unsigned long long)handle);
        lastRequest=Clock::now();changed.notify_all();return SignalServiceResult::Success;
    }
    // Caller owns mutex; no published CPU request may race retirement.
    bool retire(const char *reason) {
        if(!handle)return !faulted;
        if(client)client->cancel();
        const auto deadline=Clock::now()+std::chrono::milliseconds(100);
        bool healthyCompletion=false;
        while(Clock::now()<deadline) {
            if(!completion()) {healthyCompletion=true;break;}
            uint64_t inactive=0;
            if(connection->serviceQueue(handle,inactive)!=0 || inactive)break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        // Even on a fault attempt verified unmap. If it cannot be confirmed,
        // every buffer and the borrowed signal arena must remain retained.
        if(connection->destroyQueue(handle)!=0) {
            if(trace)std::fprintf(stderr,"signal-mailbox: retirement-failed reason=%s queue=%llu\n",reason,(unsigned long long)handle);
            faulted=true;return false;
        }
        if(trace)std::fprintf(stderr,"signal-mailbox: retired reason=%s queue=%llu completed-requests=%llu completion=%s\n",
            reason,(unsigned long long)handle,(unsigned long long)completedRequests,healthyCompletion ? "confirmed" : "unconfirmed");
        handle=0;client.reset();
        if(!healthyCompletion)faulted=true;
        return healthyCompletion && !faulted;
    }
    void run() {
        std::unique_lock lock(mutex);
        while(!stopWorker) {
            if(!handle || faulted) {changed.wait(lock,[&]{return stopWorker || (handle && !faulted);});continue;}
            const auto deadline=lastRequest+idle;
            if(changed.wait_until(lock,deadline,[&]{return stopWorker || !handle || faulted || lastRequest+idle!=deadline;}))continue;
            retire("idle");
        }
    }
    bool shutdown() {
        {std::lock_guard lock(mutex);stopWorker=true;changed.notify_all();}
        if(worker.joinable())worker.join();
        std::lock_guard lock(mutex);
        if(released)return !faulted;
        const bool clean=retire("shutdown");
        // The connection can be faulted even with a zero returned queue handle
        // after a failed map. Do not free borrowed/owned DMA or executable bytes.
        if(faulted)return false;
        for(auto *buffer:{&metadata,&ring,&arguments,&mailbox})if(buffer->host) {
            if(connection->freeSharedBuffer(*buffer)!=0) {faulted=true;return false;}
            *buffer={};
        }
        if(code.handle && connection->freeBuffer(code)!=0) {faulted=true;return false;}
        code={};released=true;return clean;
    }
};
GPUSignalService::GPUSignalService(std::shared_ptr<Connection> connection,const SharedBuffer &arena,std::chrono::milliseconds idle)
    :state_(std::make_unique<State>(std::move(connection),arena,idle)) {
    state_->worker=std::thread([state=state_.get()]{state->run();});
}
GPUSignalService::~GPUSignalService() {shutdown();}
bool GPUSignalService::healthy() const {return !state_->faulted;}
bool GPUSignalService::shutdown() {return state_->shutdown();}
bool GPUSignalService::reclaim() {std::lock_guard lock(state_->mutex);return state_->retire("public-queue");}
SignalServiceResult GPUSignalService::execute(unsigned slot,unsigned operation,int64_t value,int64_t compare,int64_t &old) {
    std::lock_guard lock(state_->mutex);
    try {
    if(state_->faulted || state_->stopWorker)return SignalServiceResult::Failed;
    if(slot>=256 || operation<1 || operation>8)return SignalServiceResult::Failed;
    const auto ready=state_->start();if(ready!=SignalServiceResult::Success)return ready;
    state_->client->store(MAC_MAILBOX_SLOT,slot,std::memory_order_relaxed);
    const SignalOperation request{operation,uint64_t(value),uint64_t(compare)};uint64_t result=0;
    const auto status=state_->client->execute({&request,1},{&result,1},State::Clock::now()+std::chrono::milliseconds(100));
    if(status!=MailboxResult::Success) {state_->faulted=true;state_->retire("request-failure");return SignalServiceResult::Failed;}
    if(!state_->completedRequests && state_->trace)std::fprintf(stderr,"signal-mailbox: backend=mailbox first-request-completed queue=%llu\n",(unsigned long long)state_->handle);
    ++state_->completedRequests;
    old=int64_t(result);state_->lastRequest=State::Clock::now();state_->changed.notify_all();return SignalServiceResult::Success;
    } catch (const std::bad_alloc &) {
        state_->faulted=true;state_->retire("allocation-failure");return SignalServiceResult::Failed;
    }
}
}
