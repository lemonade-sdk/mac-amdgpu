#include "runtime_state.h"
#include <hsa/amd_hsa_queue.h>

namespace mac_hsa::detail {
namespace {
struct SoftQueue {
    amd_queue_t abi{};
    std::shared_ptr<Signal> doorbell;
    bool active = true;
    ~SoftQueue() { std::free(abi.hsa_queue.base_address); }
};
std::unordered_map<const hsa_queue_t *, std::shared_ptr<SoftQueue>> queues;
std::shared_ptr<SoftQueue> findQueue(const hsa_queue_t *pointer) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return {};
    const auto entry = queues.find(pointer);
    return entry == queues.end() ? nullptr : entry->second;
}
auto index(volatile uint64_t &value) { return std::atomic_ref<uint64_t>(const_cast<uint64_t &>(value)); }
static_assert(offsetof(amd_queue_t, write_dispatch_id) % std::atomic_ref<uint64_t>::required_alignment == 0);
static_assert(offsetof(amd_queue_t, read_dispatch_id) % std::atomic_ref<uint64_t>::required_alignment == 0);
}
void clearQueues() { queues.clear(); }
} // namespace mac_hsa::detail
using namespace mac_hsa::detail;

extern "C" {
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
        auto queue = std::make_shared<SoftQueue>();
        if (posix_memalign(&queue->abi.hsa_queue.base_address, 4096, (size_t(size) * 64 + 4095) & ~size_t(4095)))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::memset(queue->abi.hsa_queue.base_address, 0, size_t(size) * 64);
        auto packets = static_cast<uint16_t *>(queue->abi.hsa_queue.base_address);
        for (uint32_t i = 0; i < size; ++i) packets[size_t(i) * 32] = HSA_PACKET_TYPE_INVALID;
        queue->abi.hsa_queue.type = type;
        queue->abi.hsa_queue.features = features;
        queue->abi.hsa_queue.doorbell_signal = doorbell;
        queue->abi.hsa_queue.size = size;
        queue->abi.hsa_queue.id = ++lastHandle;
        queue->abi.queue_properties = AMD_QUEUE_PROPERTIES_IS_PTR64;
        queue->abi.read_dispatch_id_field_base_byte_offset = offsetof(amd_queue_t, read_dispatch_id);
        queue->doorbell = signal->second;
        auto pointer = &queue->abi.hsa_queue;
        queues.emplace(pointer, std::move(queue));
        *out = pointer;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
hsa_status_t hsa_queue_destroy(hsa_queue_t *queue) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queues.erase(queue)) return HSA_STATUS_ERROR_INVALID_QUEUE;
    // The application supplied this software queue's doorbell; retain its
    // public signal until the application destroys it explicitly.
    return HSA_STATUS_SUCCESS;
}
hsa_status_t hsa_queue_inactivate(hsa_queue_t *queue) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    const auto found = queues.find(queue);
    if (found == queues.end()) return HSA_STATUS_ERROR_INVALID_QUEUE;
    found->second->active = false;
    return HSA_STATUS_SUCCESS;
}

#define QUEUE_LOAD(which, suffix, order) \
uint64_t hsa_queue_load_##which##_index_##suffix(const hsa_queue_t *pointer) { \
    const auto queue = findQueue(pointer); \
    return queue ? index(queue->abi.which##_dispatch_id).load(order) : 0; \
}
QUEUE_LOAD(read, relaxed, std::memory_order_relaxed)
QUEUE_LOAD(read, scacquire, std::memory_order_acquire)
QUEUE_LOAD(write, relaxed, std::memory_order_relaxed)
QUEUE_LOAD(write, scacquire, std::memory_order_acquire)
#undef QUEUE_LOAD
#define QUEUE_STORE(which, suffix, order) \
void hsa_queue_store_##which##_index_##suffix(const hsa_queue_t *pointer, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    if (queue) index(queue->abi.which##_dispatch_id).store(value, order); \
}
QUEUE_STORE(read, relaxed, std::memory_order_relaxed)
QUEUE_STORE(read, screlease, std::memory_order_release)
QUEUE_STORE(write, relaxed, std::memory_order_relaxed)
QUEUE_STORE(write, screlease, std::memory_order_release)
#undef QUEUE_STORE
#define QUEUE_RMW(suffix, order, failure) \
uint64_t hsa_queue_add_write_index_##suffix(const hsa_queue_t *pointer, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    return queue ? index(queue->abi.write_dispatch_id).fetch_add(value, order) : 0; \
} \
uint64_t hsa_queue_cas_write_index_##suffix(const hsa_queue_t *pointer, uint64_t expected, uint64_t value) { \
    const auto queue = findQueue(pointer); \
    if (!queue) return 0; \
    index(queue->abi.write_dispatch_id).compare_exchange_strong(expected, value, order, failure); \
    return expected; \
}
QUEUE_RMW(relaxed, std::memory_order_relaxed, std::memory_order_relaxed)
QUEUE_RMW(scacquire, std::memory_order_acquire, std::memory_order_acquire)
QUEUE_RMW(screlease, std::memory_order_release, std::memory_order_relaxed)
QUEUE_RMW(scacq_screl, std::memory_order_acq_rel, std::memory_order_acquire)
#undef QUEUE_RMW
} // extern C
