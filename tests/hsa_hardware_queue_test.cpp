#include "runtime_state.h"
#include "mac_hsa.h"
#include <hsa/amd_hsa_queue.h>
#include <cassert>
#include <cstdio>
#include <set>

static unsigned creates,kicks,destroys,atomics,callbacks;
static bool failKick=false,failAtomic=false;
static uint64_t driverBuild=187;
static uint32_t gfxRevision=1;
static unsigned diagnosticMode=0,diagnosticCalls=0;
static unsigned requesterMode=0,requesterCalls=0;
namespace mac_hsa {
struct TestConnection:Connection {
    uint64_t next=0;bool fault=false;
    std::map<uint64_t,DeviceBuffer> buffers;
    std::map<uint64_t,void *> shared;
    std::map<uint64_t,std::vector<uint8_t>> device;
    std::set<uint64_t> queues;
    ~TestConnection() override {for (auto &[id,pointer]:shared) { (void)id;std::free(pointer); }}
    bool supportsBuffers() const override {return true;}
    hsa_status_t read(DeviceSnapshot &s) override {s={1,driverBuild,15,256ull<<20,32ull<<30,12,0,gfxRevision};return HSA_STATUS_SUCCESS;}
    hsa_status_t allocateSharedBuffer(uint64_t bytes,SharedBuffer &out) override {
        bytes=(bytes+16383)&~uint64_t(16383);
        void *p=nullptr;if(posix_memalign(&p,16384,bytes)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::memset(p,0,bytes);out={{++next,reinterpret_cast<uintptr_t>(p),bytes},p,0};
        buffers[out.device.handle]=out.device;shared[out.device.handle]=p;return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeSharedBuffer(const SharedBuffer &buffer) override {
        uint64_t now;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now)==0 || !detail::references);
        if(fault) return HSA_STATUS_ERROR;
        assert(shared.contains(buffer.device.handle));std::free(shared.at(buffer.device.handle));
        shared.erase(buffer.device.handle);buffers.erase(buffer.device.handle);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t allocateBuffer(uint64_t bytes,DeviceBuffer &out) override {
        out={++next,0x8000000000ull+next*0x100000,(bytes+16383)&~uint64_t(16383)};
        buffers[out.handle]=out;device[out.handle].resize(out.size);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &buffer) override {
        uint64_t now;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now)==0 || !detail::references);
        if(fault) return HSA_STATUS_ERROR;
        assert(device.erase(buffer.handle)==1);buffers.erase(buffer.handle);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t writeBuffer(const DeviceBuffer &buffer,uint64_t offset,const void *data,size_t bytes) override {
        assert(offset+bytes<=buffer.size);std::memcpy(device.at(buffer.handle).data()+offset,data,bytes);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t atomicRequesterExperiment(bool enable,amdgpu::atomic_requester::Snapshot &out) override {
        using namespace amdgpu::atomic_requester;
        ++requesterCalls;uint64_t now;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now)==0);
        assert(queues.empty());out={};out.values[Version]=1;
        out.values[Before]=enable ? 0 : 64;out.values[Requested]=out.values[Observed]=enable ? 64 : 0;
        out.values[Active]=out.values[RestorePending]=enable;
        if(requesterMode==1) {out.values[Status]=2;return HSA_STATUS_ERROR;}
        if(requesterMode==2) out.values[Version]=2;
        if(requesterMode==3) out.values[Active]=2;
        if(requesterMode==4) out.values[Status]=1ull<<32;
        if(requesterMode==5) out.values[Observed]=1ull<<16;
        if(requesterMode==6) {out.values[Active]=0;out.values[RestorePending]=1;}
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t sharedAtomicDiagnostics(const SharedBuffer &buffer,uint64_t offset,uint64_t queue,
        amdgpu::atomic_diag::Snapshot &out) override {
        using namespace amdgpu::atomic_diag;
        ++diagnosticCalls;
        uint64_t now;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now)==0); // no global lock
        assert(queues.contains(queue) && shared.at(buffer.device.handle)==buffer.host);
        assert(mapping_addresses(buffer.device.address,0x81000000,buffer.device.size,0,0x700000,4096,offset,out));
        out.values[ValidFields]=kPTEValid|kMQDValid;
        out.values[PTEActual]=out.values[PTEExpected];
        out.values[MQDGPUAddress]=0x8001800000ull;out.values[MQDBackingHQStatus0]=1u<<29;
        if(diagnosticMode==1) return HSA_STATUS_ERROR;
        if(diagnosticMode==2) ++out.values[Version];
        if(diagnosticMode==3) ++out.values[GPUAddress];
        if(diagnosticMode==4) out.values[ValidFields]|=16;
        if(diagnosticMode==5) out.values[DMAAddress]=1ull<<44;
        return HSA_STATUS_SUCCESS;
    }
    hsa_status_t createQueue(const SharedBuffer &ring,const SharedBuffer &metadata,uint32_t size,uint64_t &handle) override {
        auto *q=static_cast<amd_queue_t *>(metadata.host);
        assert(q->hsa_queue.base_address==ring.host && q->hsa_queue.size==size && !q->write_dispatch_id && !q->read_dispatch_id);
        assert(q->queue_properties==AMD_QUEUE_PROPERTIES_IS_PTR64 && q->read_dispatch_id_field_base_byte_offset==offsetof(amd_queue_t,read_dispatch_id));
        for(unsigned i=0;i<size;++i) assert(static_cast<uint16_t *>(ring.host)[i*32]==HSA_PACKET_TYPE_INVALID);
        handle=++creates;queues.insert(handle);return HSA_STATUS_SUCCESS;
    }
    hsa_status_t kickQueue(uint64_t handle,uint64_t index) override {
        assert(queues.contains(handle));assert(index==7 || index==9);++kicks;
        if (failKick) {fault=true;return HSA_STATUS_ERROR;}return HSA_STATUS_SUCCESS;
    }
    hsa_status_t destroyQueue(uint64_t handle) override {
        uint64_t now;hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now); // no global lock held
        if(fault) return HSA_STATUS_ERROR;
        assert(queues.erase(handle)==1);++destroys;return HSA_STATUS_SUCCESS;
    }
    hsa_status_t dispatchAQL(const amdgpu::AQLDispatchRequest &r,uint64_t &completion) override {
        assert(amdgpu::aql_dispatch_shape(r) && r.kernargBytes==36 && r.threads[0]==32);
        if(failAtomic) {fault=true;return HSA_STATUS_ERROR;}
        ++atomics;
        const auto *args=device.at(r.kernargHandle).data();
        uint64_t address,result;int64_t value,compare;uint32_t op;
        std::memcpy(&address,args,8);std::memcpy(&result,args+8,8);std::memcpy(&value,args+16,8);
        std::memcpy(&compare,args+24,8);std::memcpy(&op,args+32,4);
        auto atomic=std::atomic_ref<int64_t>(*reinterpret_cast<int64_t *>(address));
        int64_t old=0;
        switch(op) {
        case 1:case 7:old=atomic.exchange(value);break;
        case 2:old=atomic.fetch_add(value);break;case 3:old=atomic.fetch_sub(value);break;
        case 4:old=atomic.fetch_and(value);break;case 5:old=atomic.fetch_or(value);break;
        case 6:old=atomic.fetch_xor(value);break;
        case 8:old=compare;atomic.compare_exchange_strong(old,value);break;
        default:assert(false);
        }
        std::memcpy(reinterpret_cast<void *>(result),&old,8);completion=0;return HSA_STATUS_SUCCESS;
    }
};
static std::shared_ptr<TestConnection> connection;
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &out) {
    connection=std::make_shared<TestConnection>();out.push_back(connection);return HSA_STATUS_SUCCESS;
}
}
int main() {
    assert(hsa_init()==0);hsa_agent_t gpu{};
    assert(hsa_iterate_agents([](hsa_agent_t a,void *p) {hsa_device_type_t type;hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
        if(type==HSA_DEVICE_TYPE_GPU)*static_cast<hsa_agent_t *>(p)=a;return HSA_STATUS_SUCCESS;},&gpu)==0);
    mac_hsa_atomic_requester_experiment_t requester{};
    std::memset(&requester,0xa5,sizeof(requester));const auto requesterUnchanged=requester;
    assert(mac_hsa_atomic_requester_experiment(gpu,2,&requester,sizeof(requester))==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(mac_hsa_atomic_requester_experiment(gpu,1,&requester,sizeof(requester)-1)==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(mac_hsa_atomic_requester_experiment({UINT64_MAX},1,&requester,sizeof(requester))==HSA_STATUS_ERROR_INVALID_AGENT);
    assert(!requesterCalls && std::memcmp(&requester,&requesterUnchanged,sizeof(requester))==0);
    assert(mac_hsa_atomic_requester_experiment(gpu,1,&requester,sizeof(requester))==0 && requester.active && requester.restore_pending && requester.observed_control2==64);
    assert(mac_hsa_atomic_requester_experiment(gpu,0,&requester,sizeof(requester))==0 && !requester.active && !requester.restore_pending);
    requesterMode=1;assert(mac_hsa_atomic_requester_experiment(gpu,1,&requester,sizeof(requester))==HSA_STATUS_ERROR);
    assert(requester.version==1 && requester.driver_status==2 && requester.active && requester.restore_pending);
    requester=requesterUnchanged;
    for(requesterMode=2;requesterMode<=6;++requesterMode) {
        assert(mac_hsa_atomic_requester_experiment(gpu,1,&requester,sizeof(requester))==HSA_STATUS_ERROR);
        assert(std::memcmp(&requester,&requesterUnchanged,sizeof(requester))==0);
    }
    requesterMode=0;
    hsa_queue_t *queue=nullptr;
    driverBuild=186;
    uint32_t capability=UINT32_MAX;
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_FEATURE,&capability)==0 && !capability);
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_QUEUES_MAX,&capability)==0 && !capability);
    assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,0,0,&queue)==HSA_STATUS_ERROR_INVALID_QUEUE_CREATION);
    assert(!queue && !creates && mac_hsa::connection->buffers.empty());
    driverBuild=187;
    gfxRevision=0;
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_FEATURE,&capability)==0 && !capability);
    assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,0,0,&queue)==HSA_STATUS_ERROR_INVALID_QUEUE_CREATION);
    assert(!queue && !creates && mac_hsa::connection->buffers.empty());
    gfxRevision=1;
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_FEATURE,&capability)==0 && capability==HSA_AGENT_FEATURE_KERNEL_DISPATCH);
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_QUEUES_MAX,&capability)==0 && capability==7);
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_QUEUE_MIN_SIZE,&capability)==0 && capability==64);
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_QUEUE_MAX_SIZE,&capability)==0 && capability==4096);
    hsa_queue_type_t queueType;
    assert(hsa_agent_get_info(gpu,HSA_AGENT_INFO_QUEUE_TYPE,&queueType)==0 && queueType==HSA_QUEUE_TYPE_MULTI);
    assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *) {
        assert(status==HSA_STATUS_ERROR);uint64_t now;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now)==0);++callbacks;
    },nullptr,UINT32_MAX,UINT32_MAX,&queue)==0 && queue);
    void *diagnosticMemory=nullptr;
    assert(mac_hsa_memory_allocate_shared(gpu,16384,&diagnosticMemory)==0);
    uint32_t syncFlags=0;
    assert(mac_hsa_memory_get_sync_capabilities(gpu,diagnosticMemory,&syncFlags)==0);
    assert(syncFlags==(MAC_HSA_SYNC_CPU_LOCAL_ATOMICS|MAC_HSA_SYNC_GPU_LOCAL_ATOMICS|
        MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS));
    driverBuild=190;
    assert(mac_hsa_memory_get_sync_capabilities(gpu,diagnosticMemory,&syncFlags)==0 &&
        (syncFlags&MAC_HSA_SYNC_OWNERSHIP_TRANSFER));
    driverBuild=187;
    // A successful requester experiment and MQD firmware atomics bit cannot
    // elevate this allocation to native mixed CPU/GPU RMW or IRQ notification.
    assert(!(syncFlags&(MAC_HSA_SYNC_NATIVE_CPU_GPU_RMW|MAC_HSA_SYNC_MAILBOX_IRQ_WAKE)));
    uint32_t unchangedFlags=0xfeed;
    assert(mac_hsa_memory_get_sync_capabilities(gpu,diagnosticMemory,nullptr)==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(mac_hsa_memory_get_sync_capabilities(gpu,static_cast<char *>(diagnosticMemory)+16384,&unchangedFlags)==HSA_STATUS_ERROR_INVALID_ALLOCATION);
    assert(unchangedFlags==0xfeed);
    gfxRevision=2;
    assert(mac_hsa_memory_get_sync_capabilities(gpu,diagnosticMemory,&syncFlags)==0 && !syncFlags);
    gfxRevision=1;
    mac_hsa_shared_atomic_diagnostics_t diagnostic{};
    assert(mac_hsa_shared_atomic_diagnostics(static_cast<char *>(diagnosticMemory)+16376,queue,&diagnostic,sizeof(diagnostic))==0);
    assert(diagnostic.gpu_address==reinterpret_cast<uintptr_t>(diagnosticMemory)+16376 &&
        diagnostic.dma_address==0x81003ff8 && diagnostic.pte_actual==diagnostic.pte_expected &&
        diagnostic.mqd_backing_hq_status0==(1u<<29) && diagnostic.cpu_cache_attributes==UINT64_MAX);
    std::memset(&diagnostic,0xa5,sizeof(diagnostic));const auto unchanged=diagnostic;
    const auto before=diagnosticCalls;
    assert(mac_hsa_shared_atomic_diagnostics(diagnosticMemory,queue,&diagnostic,sizeof(diagnostic)-1)==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(mac_hsa_shared_atomic_diagnostics(static_cast<char *>(diagnosticMemory)+1,queue,&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(mac_hsa_shared_atomic_diagnostics(static_cast<char *>(diagnosticMemory)+16384,queue,&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR_INVALID_ALLOCATION);
    uint64_t stack=0;
    assert(mac_hsa_shared_atomic_diagnostics(&stack,queue,&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR_INVALID_ALLOCATION);
    assert(mac_hsa_shared_atomic_diagnostics(diagnosticMemory,reinterpret_cast<hsa_queue_t *>(&stack),&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR_INVALID_QUEUE);
    auto allocation=mac_hsa::detail::findAllocation(diagnosticMemory);
    auto connection=allocation->connection;
    allocation->connection=std::make_shared<mac_hsa::TestConnection>();
    assert(mac_hsa_shared_atomic_diagnostics(diagnosticMemory,queue,&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR_INVALID_ALLOCATION);
    allocation->connection=connection;allocation.reset();connection.reset();
    assert(diagnosticCalls==before && std::memcmp(&diagnostic,&unchanged,sizeof(diagnostic))==0);
    for(diagnosticMode=1;diagnosticMode<=5;++diagnosticMode) {
        assert(mac_hsa_shared_atomic_diagnostics(diagnosticMemory,queue,&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR);
        assert(std::memcmp(&diagnostic,&unchanged,sizeof(diagnostic))==0);
    }
    diagnosticMode=0;
    const auto propertiesBefore=reinterpret_cast<amd_queue_t *>(queue)->queue_properties;
    assert(hsa_amd_profiling_set_profiler_enabled(queue,1)==HSA_STATUS_ERROR);
    assert(reinterpret_cast<amd_queue_t *>(queue)->queue_properties==propertiesBefore);
    assert(hsa_amd_profiling_set_profiler_enabled(queue,0)==HSA_STATUS_SUCCESS);
    assert(reinterpret_cast<amd_queue_t *>(queue)->queue_properties==propertiesBefore);
    hsa_agent_t queueAgent{};
    assert(hsa_amd_queue_get_info(queue,HSA_AMD_QUEUE_INFO_AGENT,&queueAgent)==0 && queueAgent.handle==gpu.handle);
    uint64_t doorbellID=0xabcdef;
    assert(hsa_amd_queue_get_info(queue,HSA_AMD_QUEUE_INFO_DOORBELL_ID,&doorbellID)==HSA_STATUS_ERROR_INVALID_ARGUMENT && doorbellID==0xabcdef);
    assert(creates==1 && hsa_queue_add_write_index_relaxed(queue,8)==0);
    hsa_signal_store_screlease(queue->doorbell_signal,7);assert(kicks==1);
    assert(hsa_queue_load_write_index_scacquire(queue)==8);
    auto *abi=reinterpret_cast<amd_queue_t *>(queue);abi->read_dispatch_id=8;
    assert(hsa_queue_load_read_index_scacquire(queue)==8);
    assert(hsa_queue_inactivate(queue)==0 && destroys==1);
    assert(mac_hsa_shared_atomic_diagnostics(diagnosticMemory,queue,&diagnostic,sizeof(diagnostic))==HSA_STATUS_ERROR_INVALID_QUEUE);
    assert(hsa_memory_free(diagnosticMemory)==0);
    hsa_signal_store_relaxed(queue->doorbell_signal,7);assert(kicks==1);
    assert(hsa_queue_destroy(queue)==0 && destroys==1 && mac_hsa::connection->buffers.empty());
    hsa_signal_t first{},second{};
    assert(hsa_signal_create(10,0,nullptr,&first)==0 && hsa_signal_create(20,1,&gpu,&second)==0);
    assert(mac_hsa::connection->buffers.size()==4); // one shared arena/executor for both signals
    hsa_signal_add_relaxed(first,3);assert(hsa_signal_load_scacquire(first)==13 && atomics==1);
    assert(hsa_signal_exchange_scacq_screl(first,9)==13);
    assert(hsa_signal_cas_relaxed(first,9,7)==9 && hsa_signal_load_relaxed(first)==7);
    assert(hsa_signal_cas_relaxed(first,9,5)==7 && hsa_signal_load_relaxed(first)==7);
    hsa_signal_subtract_relaxed(first,2);hsa_signal_or_relaxed(first,8);hsa_signal_and_relaxed(first,7);hsa_signal_xor_relaxed(first,1);
    assert(hsa_signal_load_relaxed(first)==4 && hsa_signal_load_relaxed(second)==20);
    hsa_signal_silent_store_screlease(first,INT64_MAX);hsa_signal_add_relaxed(first,1);assert(hsa_signal_load_relaxed(first)==INT64_MIN);
    std::array<hsa_signal_t,254> pooled{};
    for (auto &signal:pooled) assert(hsa_signal_create(91,0,nullptr,&signal)==0);
    hsa_signal_t exhausted{};
    assert(hsa_signal_create(0,0,nullptr,&exhausted)==HSA_STATUS_ERROR_OUT_OF_RESOURCES && !exhausted.handle);
    assert(mac_hsa::connection->buffers.size()==4 && hsa_signal_load_relaxed(second)==20);
    for (const auto signal:pooled) assert(hsa_signal_destroy(signal)==0);
    assert(hsa_signal_create(37,0,nullptr,&exhausted)==0 && hsa_signal_load_relaxed(exhausted)==37);
    assert(hsa_signal_destroy(exhausted)==0);
    assert(hsa_signal_destroy(first)==0 && mac_hsa::connection->buffers.size()==4);
    assert(hsa_signal_destroy(second)==0 && mac_hsa::connection->buffers.empty());
    assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_SINGLE,nullptr,nullptr,0,0,&queue)==0);
    // Shutdown unmaps before returning backing, outside the global HSA lock.
    assert(hsa_shut_down()==0 && mac_hsa::connection->queues.empty() && mac_hsa::connection->buffers.empty());
    assert(hsa_init()==0);
    assert(hsa_signal_create(1,0,nullptr,&first)==0);
    assert(hsa_signal_create(2,0,nullptr,&second)==0);
    failAtomic=true;hsa_signal_subtract_relaxed(first,1);
    assert(hsa_signal_load_relaxed(first)==1); // failed operation never invents completion
    assert(hsa_signal_wait_relaxed(first,HSA_SIGNAL_CONDITION_EQ,0,UINT64_MAX,HSA_WAIT_STATE_BLOCKED)==1);
    assert(hsa_signal_wait_relaxed(second,HSA_SIGNAL_CONDITION_EQ,0,UINT64_MAX,HSA_WAIT_STATE_BLOCKED)==2);
    hsa_signal_t rejected{};
    assert(hsa_signal_create(0,0,nullptr,&rejected)==HSA_STATUS_ERROR_OUT_OF_RESOURCES && !rejected.handle);
    assert(hsa_signal_destroy(first)==0 && hsa_signal_destroy(second)==0 && !mac_hsa::connection->buffers.empty());
    assert(hsa_shut_down()==0);mac_hsa::connection.reset();
    assert(hsa_init()==0);
    assert(hsa_iterate_agents([](hsa_agent_t a,void *p) {hsa_device_type_t type;hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
        if(type==HSA_DEVICE_TYPE_GPU)*static_cast<hsa_agent_t *>(p)=a;return HSA_STATUS_SUCCESS;},&gpu)==0);
    assert(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *) {
        assert(status==HSA_STATUS_ERROR);uint64_t now;assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP,&now)==0);++callbacks;
    },nullptr,0,0,&queue)==0);
    assert(hsa_queue_add_write_index_relaxed(queue,10)==0);
    failKick=true;hsa_signal_store_relaxed(queue->doorbell_signal,9);
    assert(callbacks==1);const auto oldKicks=kicks;
    hsa_signal_store_relaxed(queue->doorbell_signal,9);assert(callbacks==1 && kicks==oldKicks);
    assert(hsa_queue_destroy(queue)==HSA_STATUS_ERROR && !mac_hsa::connection->buffers.empty());
    assert(hsa_shut_down()==0);mac_hsa::connection.reset();
    puts("Persistent HSA queues, doorbells, shared GPU signal routing, return values, cleanup and fault retention passed");
}
