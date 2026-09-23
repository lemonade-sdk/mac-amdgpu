#include "runtime_state.h"

using namespace mac_hsa::detail;
static_assert(sizeof(hsa_amd_ipc_memory_t) == sizeof(mac_hsa::BufferToken));
extern "C" {
HSA_API_EXPORT hsa_status_t hsa_amd_ipc_memory_create(void *pointer, size_t size, hsa_amd_ipc_memory_t *out) {
    std::shared_ptr<Allocation> allocation;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        *out = {};
        allocation = findAllocation(pointer);
        if (!allocation || allocation->base != pointer || size != allocation->size || !allocation->connection ||
            (allocation->type != HSA_EXT_POINTER_TYPE_HSA && allocation->type != HSA_EXT_POINTER_TYPE_IPC))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    mac_hsa::BufferToken token;
    const auto status = allocation->connection->exportBuffer(allocation->buffer, token);
    if (status == HSA_STATUS_SUCCESS) std::memcpy(out, &token, sizeof(token));
    return status;
}
HSA_API_EXPORT hsa_status_t hsa_amd_ipc_memory_attach(const hsa_amd_ipc_memory_t *handle,
    size_t size, uint32_t count, const hsa_agent_t *requested, void **out) {
    mac_hsa::BufferToken token;
    std::vector<Agent> candidates;
    try {
        {
            std::lock_guard lock(runtimeMutex);
            if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            *out = nullptr;
            if (!handle || !size || (count && !requested)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            std::memcpy(&token, handle, sizeof(token));
            if (token.size != size || !token.registryID || (!token.token[0] && !token.token[1])) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            if (!count) {
                // This transport cannot expose VRAM to the host or every GPU.
                return HSA_STATUS_ERROR_INVALID_ARGUMENT;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const auto agent = findAgent(requested[i]);
                if (!agent || !agent->connection) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
                if (!candidates.empty() && candidates[0].handle.handle != agent->handle.handle)
                    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
                if (candidates.empty()) candidates.push_back(*agent);
            }
            for (const auto &[address, allocation] : allocations) {
                (void)address;
                if (allocation->type == HSA_EXT_POINTER_TYPE_IPC && allocation->owner.handle == candidates[0].handle.handle &&
                    !std::memcmp(&allocation->ipcToken, &token, sizeof(token))) {
                    if (allocation->ipcReferences == UINT32_MAX) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
                    ++allocation->ipcReferences; *out = allocation->base; return HSA_STATUS_SUCCESS;
                }
            }
        }
        auto allocation = std::make_shared<Allocation>();
        allocation->connection = candidates[0].connection; allocation->owner = candidates[0].handle;
        const auto status = allocation->connection->importBuffer(token, allocation->buffer);
        if (status != HSA_STATUS_SUCCESS) return status;
        allocation->type = HSA_EXT_POINTER_TYPE_IPC;
        allocation->ipcToken = token;
        allocation->base = reinterpret_cast<void *>(allocation->buffer.address); allocation->size = allocation->buffer.size;
        if (!allocation->buffer.handle || !allocation->base || allocation->size != size || size > UINTPTR_MAX - uintptr_t(allocation->base))
            return HSA_STATUS_ERROR;
        {
            std::lock_guard lock(runtimeMutex);
            if (!references || !findAgent(allocation->owner)) return HSA_STATUS_ERROR_NOT_INITIALIZED;
            // Repeated imports of the same MC address cannot have distinct host
            // pointer handles yet. Reject alias collisions instead of overwriting.
            const auto address = uintptr_t(allocation->base);
            if (findAllocation(allocation->base)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            const auto next = allocations.lower_bound(address);
            if (next != allocations.end() && size > next->first - address) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
            allocations.emplace(address, allocation);
        }
        *out = allocation->base; return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
HSA_API_EXPORT hsa_status_t hsa_amd_ipc_memory_detach(void *pointer) {
    std::shared_ptr<Allocation> retired;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        const auto found = allocations.find(uintptr_t(pointer));
        if (found == allocations.end() || found->second->type != HSA_EXT_POINTER_TYPE_IPC)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (--found->second->ipcReferences) return HSA_STATUS_SUCCESS;
        retired = std::move(found->second); allocations.erase(found);
    }
    return retired.use_count() == 1 ? retired->release() : HSA_STATUS_SUCCESS;
}
}
