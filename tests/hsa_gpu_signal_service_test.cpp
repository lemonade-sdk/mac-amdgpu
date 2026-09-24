#include "runtime_state.h"
#include "gpu_signal_service.h"
#include "signal_mailbox.h"
#include <hsa/amd_hsa_queue.h>
#include <cassert>
#include <cstdio>
#include <set>

namespace {
enum class Failure { None, NoReady, NoAck, Unmap };
std::atomic<unsigned> published{0},fallbacks{0},created{0},removed{0};
Failure failure=Failure::None;
uint64_t driverBuild=190;
uint64_t update(unsigned op,uint64_t operand,uint64_t compare,uint64_t *pointer) {
    auto value=std::atomic_ref<uint64_t>(*pointer);uint64_t old=0;
    switch(op) {
    case 1:case 7:return value.exchange(operand);
    case 2:return value.fetch_add(operand);case 3:return value.fetch_sub(operand);
    case 4:return value.fetch_and(operand);case 5:return value.fetch_or(operand);
    case 6:return value.fetch_xor(operand);
    case 8:old=compare;value.compare_exchange_strong(old,operand);return old;
    default:assert(false);return 0;
    }
}
}
namespace mac_hsa {
struct TestConnection final:Connection {
    std::mutex mutex;uint64_t next=0;
    std::map<uint64_t,void *> shared;
    std::map<uint64_t,std::vector<uint8_t>> device;
    struct Queue {SharedBuffer ring,metadata;std::thread peer;};
    std::map<uint64_t,std::unique_ptr<Queue>> queues;
    ~TestConnection() override {
        for(auto &[id,q]:queues) {(void)id;if(q->peer.joinable())q->peer.join();}
        for(auto &[id,p]:shared) {(void)id;std::free(p);}
    }
    bool supportsBuffers() const override{return true;}
    hsa_status_t read(DeviceSnapshot &s) override {s={1,driverBuild,15,256ull<<20,32ull<<30,12,0,1};return HSA_STATUS_SUCCESS;}
    hsa_status_t allocateBuffer(uint64_t bytes,DeviceBuffer &out) override {
        std::lock_guard lock(mutex);out={++next,0x8000000000ull+next*0x100000,(bytes+16383)&~uint64_t(16383)};
        device[out.handle].resize(out.size);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &b) override {std::lock_guard lock(mutex);assert(device.erase(b.handle));return HSA_STATUS_SUCCESS;}
    hsa_status_t writeBuffer(const DeviceBuffer &b,uint64_t offset,const void *source,size_t bytes) override {
        std::lock_guard lock(mutex);assert(offset+bytes<=device.at(b.handle).size());std::memcpy(device.at(b.handle).data()+offset,source,bytes);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateSharedBuffer(uint64_t bytes,SharedBuffer &out) override {
        std::lock_guard lock(mutex);bytes=(bytes+16383)&~uint64_t(16383);void *p=nullptr;assert(!posix_memalign(&p,16384,bytes));
        std::memset(p,0,bytes);out={{++next,reinterpret_cast<uintptr_t>(p),bytes},p,0};shared[out.device.handle]=p;return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &b) override {
        std::lock_guard lock(mutex);
        for(auto &[id,q]:queues) {(void)id;assert(q->ring.device.handle!=b.device.handle && q->metadata.device.handle!=b.device.handle);}
        assert(shared.contains(b.device.handle));std::free(shared.at(b.device.handle));shared.erase(b.device.handle);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t createQueue(const SharedBuffer &ring,const SharedBuffer &metadata,uint32_t size,uint64_t &handle) override {
        std::lock_guard lock(mutex);handle=0;if(queues.size()==7)return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        auto *q=static_cast<amd_queue_t *>(metadata.host);assert(size==64 && q->hsa_queue.base_address==ring.host && q->hsa_queue.size==size);
        assert(!q->read_dispatch_id && !q->write_dispatch_id && q->queue_properties==AMD_QUEUE_PROPERTIES_IS_PTR64);
        handle=++next;auto record=std::make_unique<Queue>();record->ring=ring;record->metadata=metadata;queues.emplace(handle,std::move(record));++created;return HSA_STATUS_SUCCESS;
    }
    hsa_status_t kickQueue(uint64_t handle,uint64_t index) override {
        std::lock_guard lock(mutex);assert(index==0);auto &queue=*queues.at(handle);
        const auto packet=*static_cast<hsa_kernel_dispatch_packet_t *>(queue.ring.host);
        assert(packet.workgroup_size_x==32 && packet.grid_size_x==32 && packet.kernel_object);
        const auto *args=static_cast<const uint64_t *>(packet.kernarg_address);
        auto *mailbox=reinterpret_cast<uint64_t *>(args[0]);auto *arena=reinterpret_cast<uint64_t *>(args[1]);
        auto *done=reinterpret_cast<SignalABI *>(packet.completion_signal.handle);assert(args[2]==256 && args[3]);
        auto *metadata=static_cast<amd_queue_t *>(queue.metadata.host);
        queue.peer=std::thread([mailbox,arena,done,metadata] {
            SignalMailboxClient peer(mailbox,MailboxWait::Active);
            if(failure!=Failure::NoReady)peer.store(MAC_MAILBOX_READY,1);
            uint64_t sequence=1;
            for(;;) {
                if(peer.load(MAC_MAILBOX_ABORT))break;
                if(peer.load(MAC_MAILBOX_REQUEST_SEQUENCE)!=sequence || failure==Failure::NoReady) {std::this_thread::yield();continue;}
                ++published;
                const auto slot=peer.load(MAC_MAILBOX_SLOT,std::memory_order_relaxed);assert(slot<256);
                assert(peer.load(MAC_MAILBOX_COUNT,std::memory_order_relaxed)==1);
                const auto op=peer.load(MAC_MAILBOX_REQUESTS,std::memory_order_relaxed);
                const auto operand=peer.load(MAC_MAILBOX_REQUESTS+1,std::memory_order_relaxed);
                const auto compare=peer.load(MAC_MAILBOX_REQUESTS+2,std::memory_order_relaxed);
                const auto old=update(unsigned(op),operand,compare,arena+slot*8+1);
                if(failure==Failure::NoAck) {
                    while(!peer.load(MAC_MAILBOX_ABORT))std::this_thread::yield();break;
                }
                peer.store(MAC_MAILBOX_RESULTS,old,std::memory_order_relaxed);
                peer.store(MAC_MAILBOX_COMPLETION_SEQUENCE,sequence++);
            }
            peer.store(MAC_MAILBOX_STATE,1);
            std::atomic_ref<int64_t>(done->value).store(0,std::memory_order_release);
            std::atomic_ref<uint64_t>(const_cast<uint64_t &>(metadata->read_dispatch_id)).store(1,std::memory_order_release);
        });
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroyQueue(uint64_t handle) override {
        std::unique_ptr<Queue> queue;
        {
            std::lock_guard lock(mutex);
            if(failure==Failure::Unmap)return HSA_STATUS_ERROR;
            queue=std::move(queues.at(handle));queues.erase(handle);++removed;
        }
        if(queue->peer.joinable())queue->peer.join();return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &request,uint64_t &completion) override {
        std::lock_guard lock(mutex);++fallbacks;
        const auto *args=device.at(request.kernargHandle).data();
        uint64_t address,result,operand,compare;uint32_t operation;
        std::memcpy(&address,args,8);std::memcpy(&result,args+8,8);std::memcpy(&operand,args+16,8);
        std::memcpy(&compare,args+24,8);std::memcpy(&operation,args+32,4);
        const auto old=update(operation,operand,compare,reinterpret_cast<uint64_t *>(address));
        std::memcpy(reinterpret_cast<void *>(result),&old,8);completion=0;return HSA_STATUS_SUCCESS;
    }
    size_t queueCount() {std::lock_guard lock(mutex);return queues.size();}
    size_t bufferCount() {std::lock_guard lock(mutex);return shared.size()+device.size();}
};
std::shared_ptr<TestConnection> connection;
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &out) {
    connection=std::make_shared<TestConnection>();out.push_back(connection);return HSA_STATUS_SUCCESS;
}
}
using namespace mac_hsa;
int main() {
    setenv("MAC_HSA_SIGNAL_BACKEND","mailbox",1);
    assert(hsa_init()==0);hsa_agent_t gpu{};
    assert(hsa_iterate_agents([](hsa_agent_t agent,void *out) {hsa_device_type_t type;assert(!hsa_agent_get_info(agent,HSA_AGENT_INFO_DEVICE,&type));if(type==HSA_DEVICE_TYPE_GPU)*static_cast<hsa_agent_t *>(out)=agent;return HSA_STATUS_SUCCESS;},&gpu)==0);
    hsa_signal_t signal{};assert(hsa_signal_create(0,1,&gpu,&signal)==0);
    assert(hsa_signal_exchange_scacq_screl(signal,5)==0);
    assert(hsa_signal_cas_scacq_screl(signal,5,8)==5);
    assert(hsa_signal_cas_scacq_screl(signal,7,9)==8);
    hsa_signal_add_scacq_screl(signal,2);assert(hsa_signal_load_scacquire(signal)==10);
    assert(created==1 && published==4 && !fallbacks);
    // Every public slot remains available, even after the signal service is hot.
    hsa_queue_t *queues[7]{};
    for(auto &queue:queues)assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,0,0,&queue)==0);
    assert(connection->queueCount()==7 && removed==1);
    hsa_signal_add_scacq_screl(signal,1);assert(hsa_signal_load_scacquire(signal)==11 && fallbacks==1);
    for(auto *queue:queues)assert(hsa_queue_destroy(queue)==0);
    hsa_signal_store_screlease(signal,0);assert(!hsa_signal_load_scacquire(signal));
    const auto before=created.load();
    std::vector<std::thread> producers;
    for(unsigned i=0;i<4;++i)producers.emplace_back([&] {for(unsigned j=0;j<32;++j)hsa_signal_add_scacq_screl(signal,1);});
    for(auto &thread:producers)thread.join();
    assert(hsa_signal_load_scacquire(signal)==128 && created==before && fallbacks==1);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(connection->queueCount() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!connection->queueCount());
    assert(hsa_signal_exchange_scacq_screl(signal,17)==128 && created==before+1);
    assert(hsa_signal_destroy(signal)==0 && !connection->queueCount());
    assert(hsa_shut_down()==0 && !connection->bufferCount());connection.reset();
    // Qualified default and explicit rollback select different real executors.
    driverBuild=193;
    for(bool forceOneShot:{false,true}) {
        if(forceOneShot)setenv("MAC_HSA_SIGNAL_BACKEND","one-shot",1);
        else unsetenv("MAC_HSA_SIGNAL_BACKEND");
        const auto initialMailbox=published.load(),initialFallback=fallbacks.load();
        assert(hsa_init()==0);
        assert(hsa_iterate_agents([](hsa_agent_t a,void *out){hsa_device_type_t t;hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&t);if(t==HSA_DEVICE_TYPE_GPU)*static_cast<hsa_agent_t *>(out)=a;return HSA_STATUS_SUCCESS;},&gpu)==0);
        assert(hsa_signal_create(0,1,&gpu,&signal)==0);
        hsa_signal_add_scacq_screl(signal,1);assert(hsa_signal_load_scacquire(signal)==1);
        assert(published==initialMailbox+(forceOneShot ? 0 : 1));
        assert(fallbacks==initialFallback+(forceOneShot ? 1 : 0));
        assert(hsa_signal_destroy(signal)==0 && !connection->queueCount());
        assert(hsa_shut_down()==0 && !connection->bufferCount());connection.reset();
    }
    driverBuild=190;setenv("MAC_HSA_SIGNAL_BACKEND","mailbox",1);
    // Execute-once failure: GPU changes the value then loses its acknowledgement.
    for(auto injected:{Failure::NoReady,Failure::NoAck,Failure::Unmap}) {
        failure=injected;published=0;fallbacks=0;assert(hsa_init()==0);
        assert(hsa_iterate_agents([](hsa_agent_t a,void *out){hsa_device_type_t t;hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&t);if(t==HSA_DEVICE_TYPE_GPU)*static_cast<hsa_agent_t *>(out)=a;return HSA_STATUS_SUCCESS;},&gpu)==0);
        assert(hsa_signal_create(0,1,&gpu,&signal)==0);
        hsa_signal_add_scacq_screl(signal,1);
        if(injected==Failure::Unmap)assert(detail::reclaimGPUSignalService(connection)==HSA_STATUS_ERROR);
        auto internal=detail::findSignal(signal);assert(internal);
        assert(hsa_signal_load_scacquire(signal)==(injected==Failure::NoReady ? 0 : 1));
        assert(!internal->alive && !fallbacks);
        assert(published==(injected==Failure::NoReady ? 0 : 1));
        hsa_signal_add_scacq_screl(signal,1);assert(!fallbacks && published==(injected==Failure::NoReady ? 0 : 1));
        assert(hsa_signal_destroy(signal)==0);assert(hsa_shut_down()==0);
        internal.reset(); // force final service/context teardown before retention check
        assert(connection->bufferCount()>0);
        // Worker already saw abort; retained fake allocations are reclaimed by
        // the test connection destructor only after the failed lifetime ends.
    }
    failure=Failure::None;connection.reset();unsetenv("MAC_HSA_SIGNAL_BACKEND");
    puts("Opt-in GPU signal service: reuse, all seven public slots, safe unpublished fallback, concurrent producers, idle retirement/restart, final teardown, no replay after timeout, and fault retention passed without GPU access.");
}
