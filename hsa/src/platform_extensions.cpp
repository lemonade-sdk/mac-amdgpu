#include "runtime_state.h"

namespace mac_hsa::detail {
namespace {
hsa_amd_system_event_callback_t eventCallback = nullptr;
void *eventData = nullptr;
bool validAddress(void *pointer, size_t size) {
    return pointer && size && size <= UINTPTR_MAX - uintptr_t(pointer);
}
}
void clearSystemEvents() { eventCallback = nullptr; eventData = nullptr; }
hsa_status_t deliverSystemEvent(const hsa_amd_event_t &event) {
    hsa_amd_system_event_callback_t callback;
    void *data;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        callback = eventCallback; data = eventData;
    }
    return callback ? callback(&event, data) : HSA_STATUS_ERROR;
}
}
using namespace mac_hsa::detail;
extern "C" {
HSA_API_EXPORT hsa_status_t hsa_amd_register_system_event_handler(hsa_amd_system_event_callback_t callback, void *data) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (eventCallback) return HSA_STATUS_ERROR;
    eventCallback = callback; eventData = data; return HSA_STATUS_SUCCESS;
}
// macOS has no Linux DMA-BUF object namespace. Never interpret a caller's
// unrelated POSIX descriptor as GPU memory, and never close one we did not export.
HSA_API_EXPORT hsa_status_t hsa_amd_interop_map_buffer(uint32_t count, hsa_agent_t *requested,
    int fd, uint32_t flags, size_t *size, void **pointer, size_t *metadataSize, const void **metadata) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (pointer) *pointer = nullptr;
    if (size) *size = 0;
    if (metadataSize) *metadataSize = 0;
    if (metadata) *metadata = nullptr;
    if (!size || !pointer || fd < 0 || flags || (count && !requested)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; ++i) if (!findAgent(requested[i])) return HSA_STATUS_ERROR_INVALID_AGENT;
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}
HSA_API_EXPORT hsa_status_t hsa_amd_interop_unmap_buffer(void *) {
    std::lock_guard lock(runtimeMutex);
    return references ? HSA_STATUS_ERROR_INVALID_ARGUMENT : HSA_STATUS_ERROR_NOT_INITIALIZED;
}
HSA_API_EXPORT hsa_status_t hsa_amd_portable_export_dmabuf(const void *pointer, size_t size, int *fd, uint64_t *offset) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!pointer || !size || !fd || !offset) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto allocation = findAllocation(pointer);
    if (!allocation || size > allocation->size - (uintptr_t(pointer) - uintptr_t(allocation->base)))
        return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    return HSA_STATUS_ERROR_INVALID_AGENT; // outputs intentionally unchanged on failure
}
HSA_API_EXPORT hsa_status_t hsa_amd_portable_close_dmabuf(int) {
    std::lock_guard lock(runtimeMutex);
    return references ? HSA_STATUS_ERROR_INVALID_ARGUMENT : HSA_STATUS_ERROR_NOT_INITIALIZED;
}
// HMM migration and GPU fault servicing are not available through this driver.
// These entry points are present for dynamic linking but must never report a
// successful migration, grant GPU access, or signal completion without one.
HSA_API_EXPORT hsa_status_t hsa_amd_svm_attributes_set(void *pointer, size_t size,
    hsa_amd_svm_attribute_pair_t *attributes, size_t count) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!validAddress(pointer, size) || (count && !attributes)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    return HSA_STATUS_ERROR;
}
HSA_API_EXPORT hsa_status_t hsa_amd_svm_attributes_get(void *pointer, size_t size,
    hsa_amd_svm_attribute_pair_t *attributes, size_t count) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!validAddress(pointer, size) || (count && !attributes)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    return HSA_STATUS_ERROR; // leave caller's attribute array unchanged
}
HSA_API_EXPORT hsa_status_t hsa_amd_svm_prefetch_async(void *pointer, size_t size, hsa_agent_t agent,
    uint32_t count, const hsa_signal_t *dependencies, hsa_signal_t completion) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!validAddress(pointer, size) || (count && !dependencies)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (!findAgent(agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
    if (completion.handle && !signals.contains(completion.handle)) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    for (uint32_t i = 0; i < count; ++i) if (!signals.contains(dependencies[i].handle)) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    return HSA_STATUS_ERROR;
}
}
