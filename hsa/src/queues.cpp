#include "runtime_state.h"
#include "mac_hsa.h"
#include <hsa/amd_hsa_queue.h>
#include <chrono>
#include <condition_variable>
#include <system_error>

namespace mac_hsa::detail {
struct RuntimeQueue {
    amd_queue_t hostABI{};
    amd_queue_t *abi=&hostABI;
    std::shared_ptr<Connection> connection;
    SharedBuffer ring, metadata;
    hsa_agent_t agent{};
    uint64_t hardwareHandle=0;
    std::mutex mutex;
    void (*errorCallback)(hsa_status_t,hsa_queue_t *,void *)=nullptr;
    void *errorData=nullptr;
    bool errorDelivered=false;
    struct ServiceState {
        std::atomic<bool> stop{false};
        std::mutex waitMutex;
        std::condition_variable changed;
    };
    std::shared_ptr<ServiceState> serviceState;
    std::mutex serviceThreadMutex;
    std::thread serviceThread;
    void stopService() {
        if (serviceState) {serviceState->stop=true;serviceState->changed.notify_all();}
        std::thread retired;
        {
            std::lock_guard lock(serviceThreadMutex);
            retired=std::move(serviceThread);
        }
        if (retired.joinable()) {
            if (retired.get_id()==std::this_thread::get_id()) retired.detach();
            else retired.join();
        }
    }
    void service() {
        hsa_status_t status=HSA_STATUS_SUCCESS;
        bool notify=false;
        {
            std::lock_guard lock(mutex);
            if (!active || !hardwareHandle || errorDelivered) return;
            uint64_t inactive=0;
            status=connection->serviceQueue(hardwareHandle,inactive);
            if (status!=HSA_STATUS_SUCCESS) {errorDelivered=true;notify=true;}
        }
        // Callbacks may query or destroy this queue; never hold its mutex here.
        if (notify) {
            if (serviceState) serviceState->stop=true;
            invalidateGPUSignals(connection);
            if (errorCallback) errorCallback(status,&abi->hsa_queue,errorData);
        }
    }
    void startService(const std::shared_ptr<RuntimeQueue> &self) {
        serviceState=std::make_shared<ServiceState>();
        const std::weak_ptr<RuntimeQueue> weak=self;
        std::lock_guard publication(serviceThreadMutex);
        serviceThread=std::thread([weak,state=serviceState] {
            while (!state->stop.load()) {
                {
                    auto queue=weak.lock();
                    if (!queue) break;
                    queue->service();
                }
                std::unique_lock lock(state->waitMutex);
                state->changed.wait_for(lock,std::chrono::milliseconds(1),[&] {return state->stop.load();});
            }
        });
    }
    hsa_status_t inactivate() {
        std::lock_guard lock(mutex);
        if (!active) return HSA_STATUS_SUCCESS;
        if (hardwareHandle) {
            const auto status=connection->destroyQueue(hardwareHandle);
            if (status!=HSA_STATUS_SUCCESS) return status;
            hardwareHandle=0;
        }
        active=false;
        if (serviceState) {serviceState->stop=true;serviceState->changed.notify_all();}
        return HSA_STATUS_SUCCESS;
    }
    void ringDoorbell(int64_t value) {
        bool notify=false;
        {
            std::lock_guard lock(mutex);
            if (!active || !hardwareHandle || errorDelivered) return;
            if (connection->kickQueue(hardwareHandle,uint64_t(value))!=HSA_STATUS_SUCCESS) {
                errorDelivered=true;notify=true;
            }
        }
        if (notify && errorCallback) errorCallback(HSA_STATUS_ERROR,&abi->hsa_queue,errorData);
    }
    std::shared_ptr<Signal> doorbell;
    bool active = true;
    ~RuntimeQueue() {
        stopService();
        if (connection) {
            if (inactivate()!=HSA_STATUS_SUCCESS) return; // driver retains backing until reset
            if (ring.host) connection->freeSharedBuffer(ring);
            if (metadata.host) connection->freeSharedBuffer(metadata);
        } else std::free(abi->hsa_queue.base_address);
    }
};
namespace {
RetiredQueueSet queues;
std::shared_ptr<RuntimeQueue> findQueue(const hsa_queue_t *pointer) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return {};
    const auto entry = queues.find(pointer);
    return entry == queues.end() ? nullptr : entry->second;
}
auto index(volatile uint64_t &value) { return std::atomic_ref<uint64_t>(const_cast<uint64_t &>(value)); }
static_assert(offsetof(amd_queue_t, write_dispatch_id) % std::atomic_ref<uint64_t>::required_alignment == 0);
static_assert(offsetof(amd_queue_t, read_dispatch_id) % std::atomic_ref<uint64_t>::required_alignment == 0);
}
RetiredQueueSet clearQueues() {
    RetiredQueueSet retired;
    retired.swap(queues);return retired;
}
void stopQueueServices(RetiredQueueSet &retired) {
    for (auto &[pointer,queue]:retired) {(void)pointer;queue->stopService();}
}

} // namespace mac_hsa::detail
using namespace mac_hsa::detail;

extern "C" {
hsa_status_t mac_hsa_shared_atomic_diagnostics(const void *pointer,const hsa_queue_t *q,
    mac_hsa_shared_atomic_diagnostics_t *out,size_t outSize) {
    std::shared_ptr<Allocation> allocation;
    std::shared_ptr<RuntimeQueue> queue;
    uint64_t offset=0;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!pointer || !q || !out || outSize!=sizeof(*out) || (reinterpret_cast<uintptr_t>(pointer)&7))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        const auto found=queues.find(q);
        if (found==queues.end()) return HSA_STATUS_ERROR_INVALID_QUEUE;
        queue=found->second;allocation=findAllocation(pointer);
        if (!allocation || !allocation->shared.host || !allocation->connection ||
            allocation->connection!=queue->connection || allocation->base!=allocation->shared.host ||
            allocation->shared.device.address!=reinterpret_cast<uintptr_t>(allocation->base))
            return HSA_STATUS_ERROR_INVALID_ALLOCATION;
        offset=reinterpret_cast<uintptr_t>(pointer)-reinterpret_cast<uintptr_t>(allocation->base);
        if (offset>allocation->size || 8>allocation->size-offset) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    }
    std::lock_guard lock(queue->mutex);
    if (!queue->active || !queue->hardwareHandle || queue->errorDelivered) return HSA_STATUS_ERROR_INVALID_QUEUE;
    amdgpu::atomic_diag::Snapshot snapshot{};
    const auto status=queue->connection->sharedAtomicDiagnostics(allocation->shared,offset,queue->hardwareHandle,snapshot);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (!amdgpu::atomic_diag::snapshot_valid(snapshot,reinterpret_cast<uintptr_t>(pointer))) return HSA_STATUS_ERROR;
    static_assert(sizeof(snapshot)==sizeof(*out));
    std::memcpy(out,&snapshot,sizeof(*out));return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_queue_create(hsa_agent_t agent,uint32_t size,hsa_queue_type32_t type,
    void (*callback)(hsa_status_t,hsa_queue_t *,void *),void *data,
    uint32_t privateBytes,uint32_t groupBytes,hsa_queue_t **out) {
    std::lock_guard lifecycle(executableLifecycleMutex);
    std::shared_ptr<mac_hsa::Connection> connection;
    uint64_t id=0;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        *out=nullptr;
        const auto found=findAgent(agent);
        if (!found) return HSA_STATUS_ERROR_INVALID_AGENT;
        if (!size || (size&(size-1)) || (type!=HSA_QUEUE_TYPE_SINGLE && type!=HSA_QUEUE_TYPE_MULTI))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (!found->connection || size<64 || size>4096 ||
            (privateBytes>262128 && privateBytes!=UINT32_MAX) || (groupBytes>65536 && groupBytes!=UINT32_MAX))
            return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
        if (lastHandle==UINT64_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        connection=found->connection;id=++lastHandle;
    }
    mac_hsa::DeviceSnapshot info{};
    auto status=connection->read(info);
    if (status!=HSA_STATUS_SUCCESS) return status;
    if (!mac_hsa::supportsPersistentQueues(info)) return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
    if (info.build<mac_hsa::kQueueResourceDriverBuild &&
        ((privateBytes && privateBytes!=UINT32_MAX) || (groupBytes && groupBytes!=UINT32_MAX)))
        return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
    try {
        auto queue=std::make_shared<RuntimeQueue>();
        queue->connection=connection;queue->agent=agent;queue->errorCallback=callback;queue->errorData=data;
        status=connection->allocateSharedBuffer(size_t(size)*64,queue->ring);
        if (status!=HSA_STATUS_SUCCESS) return status;
        status=connection->allocateSharedBuffer(16384,queue->metadata);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!queue->ring.host || !queue->metadata.host || queue->ring.device.size<uint64_t(size)*64 ||
            queue->metadata.device.size<sizeof(amd_queue_t) ||
            queue->ring.device.address!=reinterpret_cast<uintptr_t>(queue->ring.host) ||
            queue->metadata.device.address!=reinterpret_cast<uintptr_t>(queue->metadata.host)) return HSA_STATUS_ERROR;
        std::memset(queue->metadata.host,0,queue->metadata.device.size);
        std::memset(queue->ring.host,0,uint64_t(size)*64);
        queue->abi=static_cast<amd_queue_t *>(queue->metadata.host);
        auto &q=*queue->abi;
        q.hsa_queue.type=type;q.hsa_queue.features=HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
        q.hsa_queue.base_address=queue->ring.host;q.hsa_queue.size=size;q.hsa_queue.id=id;
        q.queue_properties=AMD_QUEUE_PROPERTIES_IS_PTR64;
        q.scratch_wave64_lane_byte_size=privateBytes==UINT32_MAX ? 0 : privateBytes;
        q.read_dispatch_id_field_base_byte_offset=offsetof(amd_queue_t,read_dispatch_id);
        auto *packets=static_cast<uint16_t *>(queue->ring.host);
        for (uint32_t i=0;i<size;++i) packets[size_t(i)*32]=HSA_PACKET_TYPE_INVALID;
        queue->doorbell=std::make_shared<mac_hsa::Signal>();
        const std::weak_ptr<RuntimeQueue> weak=queue;
        queue->doorbell->storeHook=[weak](int64_t value) {if (auto q=weak.lock()) q->ringDoorbell(value);};
        q.hsa_queue.doorbell_signal.handle=reinterpret_cast<uintptr_t>(queue->doorbell->address());
        status=connection->createQueue(queue->ring,queue->metadata,size,queue->hardwareHandle);
        if (status!=HSA_STATUS_SUCCESS) return status;
        if (!queue->hardwareHandle) return HSA_STATUS_ERROR;
        auto *pointer=&q.hsa_queue;
        {
            std::lock_guard lock(runtimeMutex);
            signals.emplace(q.hsa_queue.doorbell_signal.handle,queue->doorbell);
            try {queues.emplace(pointer,queue);}
            catch (...) {signals.erase(q.hsa_queue.doorbell_signal.handle);throw;}
        }
        if (info.build>=mac_hsa::kQueueResourceDriverBuild) {
            try {queue->startService(queue);}
            catch (...) {
                std::lock_guard lock(runtimeMutex);
                queues.erase(pointer);signals.erase(q.hsa_queue.doorbell_signal.handle);throw;
            }
        }
        *out=pointer;return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) {return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
      catch (const std::system_error &) {return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
}
HSA_API_EXPORT hsa_status_t hsa_amd_profiling_set_profiler_enabled(hsa_queue_t *pointer, int enable) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!pointer || (enable != 0 && enable != 1)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto queue = queues.find(pointer);
    if (queue == queues.end()) return HSA_STATUS_ERROR_INVALID_QUEUE;
    // CP caches these properties when mapping a hardware queue. Enabling
    // profiling requires a synchronized suspend/resume, as in ROCr SetProfiling.
    // Until that refresh exists, do not promise timestamps from a host-only bit.
    if (enable && queue->second->connection) return HSA_STATUS_ERROR;
    auto properties = std::atomic_ref<uint32_t>(queue->second->abi->queue_properties);
    constexpr uint32_t mask = AMD_QUEUE_PROPERTIES_ENABLE_PROFILING;
    if (enable) properties.fetch_or(mask, std::memory_order_release);
    else properties.fetch_and(~mask, std::memory_order_release);
    return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_queue_cu_set_mask(const hsa_queue_t *pointer, uint32_t bits, const uint32_t *mask) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.contains(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (bits % 32 || (bits && !mask)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    // Software queues have no CU affinity; the initial persistent-queue ABI
    // also has no synchronized MQD update operation.
    return HSA_STATUS_ERROR_INVALID_QUEUE;
}
HSA_API_EXPORT hsa_status_t hsa_amd_queue_set_priority(hsa_queue_t *pointer, hsa_amd_queue_priority_t priority) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.contains(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (priority < HSA_AMD_QUEUE_PRIORITY_LOW || priority > HSA_AMD_QUEUE_PRIORITY_HIGH) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    return HSA_STATUS_ERROR_INVALID_QUEUE;
}
HSA_API_EXPORT hsa_status_t hsa_amd_queue_get_info(hsa_queue_t *pointer, hsa_queue_info_attribute_t attribute, void *value) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.contains(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (!value || (attribute != HSA_AMD_QUEUE_INFO_AGENT && attribute != HSA_AMD_QUEUE_INFO_DOORBELL_ID)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto &queue=queues.at(pointer);
    if (!queue->connection) return HSA_STATUS_ERROR_INVALID_QUEUE;
    if (attribute==HSA_AMD_QUEUE_INFO_AGENT) return writeValue(value,queue->agent);
    // The handle is a lifetime nonce, not the physical doorbell index.
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}
hsa_status_t hsa_soft_queue_create(hsa_region_t region, uint32_t size, hsa_queue_type32_t type,
    uint32_t features, hsa_signal_t doorbell, hsa_queue_t **out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    const auto pool = findPool(region.handle);
    if (!pool) return HSA_STATUS_ERROR_INVALID_REGION;
    if (pool->connection) return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    if (!size || (size & (size - 1)) || (type != HSA_QUEUE_TYPE_SINGLE && type != HSA_QUEUE_TYPE_MULTI) ||
        features & ~(HSA_QUEUE_FEATURE_KERNEL_DISPATCH | HSA_QUEUE_FEATURE_AGENT_DISPATCH) || !doorbell.handle)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto signal = signals.find(doorbell.handle);
    if (signal == signals.end()) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    if (lastHandle == UINT64_MAX || uint64_t(size) * 64 > pool->capacity)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    try {
        auto queue = std::make_shared<RuntimeQueue>();
        if (posix_memalign(&queue->abi->hsa_queue.base_address, 4096, (size_t(size) * 64 + 4095) & ~size_t(4095)))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::memset(queue->abi->hsa_queue.base_address, 0, size_t(size) * 64);
        auto packets = static_cast<uint16_t *>(queue->abi->hsa_queue.base_address);
        for (uint32_t i = 0; i < size; ++i) packets[size_t(i) * 32] = HSA_PACKET_TYPE_INVALID;
        queue->abi->hsa_queue.type = type;
        queue->abi->hsa_queue.features = features;
        queue->abi->hsa_queue.doorbell_signal = doorbell;
        queue->abi->hsa_queue.size = size;
        queue->abi->hsa_queue.id = ++lastHandle;
        queue->abi->queue_properties = AMD_QUEUE_PROPERTIES_IS_PTR64;
        queue->abi->read_dispatch_id_field_base_byte_offset = offsetof(amd_queue_t, read_dispatch_id);
        queue->doorbell = signal->second;
        auto pointer = &queue->abi->hsa_queue;
        queues.emplace(pointer, std::move(queue));
        *out = pointer;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_queue_destroy(hsa_queue_t *pointer) {
    const auto queue=findQueue(pointer);
    if (!queue) {
        std::lock_guard lock(runtimeMutex);
        return references ? HSA_STATUS_ERROR_INVALID_QUEUE : HSA_STATUS_ERROR_NOT_INITIALIZED;
    }
    const auto status=queue->inactivate();
    if (status!=HSA_STATUS_SUCCESS) return status;
    {
        std::lock_guard lock(runtimeMutex);
        if (!queues.erase(pointer)) return HSA_STATUS_ERROR_INVALID_QUEUE;
        if (queue->connection) {
            queue->doorbell->alive=false;
            signals.erase(queue->abi->hsa_queue.doorbell_signal.handle);
        }
    }
    queue->stopService();
    return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_queue_inactivate(hsa_queue_t *pointer) {
    const auto queue=findQueue(pointer);
    if (!queue) {
        std::lock_guard lock(runtimeMutex);
        return references ? HSA_STATUS_ERROR_INVALID_QUEUE : HSA_STATUS_ERROR_NOT_INITIALIZED;
    }
    return queue->inactivate();
}

#define QUEUE_LOAD(which, suffix, order) \
uint64_t hsa_queue_load_##which##_index_##suffix(const hsa_queue_t *pointer) { \
    const auto queue = findQueue(pointer); \
    return queue ? index(queue->abi->which##_dispatch_id).load(order) : 0; \
}
QUEUE_LOAD(read, relaxed, std::memory_order_relaxed)
QUEUE_LOAD(read, scacquire, std::memory_order_acquire)
QUEUE_LOAD(write, relaxed, std::memory_order_relaxed)
QUEUE_LOAD(write, scacquire, std::memory_order_acquire)
#undef QUEUE_LOAD
#define QUEUE_STORE(which, suffix, order) \
void hsa_queue_store_##which##_index_##suffix(const hsa_queue_t *pointer, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    if (queue) index(queue->abi->which##_dispatch_id).store(value, order); \
}
QUEUE_STORE(read, relaxed, std::memory_order_relaxed)
QUEUE_STORE(read, screlease, std::memory_order_release)
QUEUE_STORE(write, relaxed, std::memory_order_relaxed)
QUEUE_STORE(write, screlease, std::memory_order_release)
#undef QUEUE_STORE
#define QUEUE_RMW(suffix, order, failure) \
uint64_t hsa_queue_add_write_index_##suffix(const hsa_queue_t *pointer, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    return queue ? index(queue->abi->write_dispatch_id).fetch_add(value, order) : 0; \
} \
uint64_t hsa_queue_cas_write_index_##suffix(const hsa_queue_t *pointer, uint64_t expected, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    if (!queue) return 0; \
    index(queue->abi->write_dispatch_id).compare_exchange_strong(expected, value, order, failure); \
    return expected; \
}
QUEUE_RMW(relaxed, std::memory_order_relaxed, std::memory_order_relaxed)
QUEUE_RMW(scacquire, std::memory_order_acquire, std::memory_order_acquire)
QUEUE_RMW(screlease, std::memory_order_release, std::memory_order_relaxed)
QUEUE_RMW(scacq_screl, std::memory_order_acq_rel, std::memory_order_acquire)
#undef QUEUE_RMW
} // extern C
