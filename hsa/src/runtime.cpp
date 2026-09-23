#include "transport.h"
#include "mac_hsa.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace {
struct Agent {
    hsa_agent_t handle;
    std::shared_ptr<mac_hsa::Connection> connection; // null for host CPU
};
std::mutex runtimeMutex;
uint32_t references = 0;
uint64_t lastHandle = 0;
std::vector<Agent> agents;

// Caller holds runtimeMutex. Opaque IDs are never reused across sessions.
Agent *findAgent(hsa_agent_t handle) {
    for (auto &agent : agents) if (agent.handle.handle == handle.handle) return &agent;
    return nullptr;
}

template<typename T> hsa_status_t writeValue(void *output, T value) {
    std::memcpy(output, &value, sizeof(value));
    return HSA_STATUS_SUCCESS;
}
} // namespace

extern "C" {
hsa_status_t hsa_init() {
    std::lock_guard lock(runtimeMutex);
    if (references == INT32_MAX) return HSA_STATUS_ERROR_REFCOUNT_OVERFLOW;
    if (references) { ++references; return HSA_STATUS_SUCCESS; }
    try {
        std::vector<std::shared_ptr<mac_hsa::Connection>> connections;
        const auto status = mac_hsa::discover(connections);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (connections.size() >= UINT64_MAX - lastHandle)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::vector<Agent> fresh;
        fresh.reserve(connections.size() + 1);
        fresh.push_back({{++lastHandle}, nullptr});
        for (auto &connection : connections)
            fresh.push_back({{++lastHandle}, std::move(connection)});
        agents.swap(fresh);
        references = 1;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) {
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
}

hsa_status_t hsa_shut_down() {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!--references) agents.clear();
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_iterate_agents(hsa_status_t (*callback)(hsa_agent_t, void *), void *data) {
    std::vector<Agent> snapshot;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!callback) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        try { snapshot = agents; }
        catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
    }
    // Callbacks can query agents or take another runtime reference.
    for (const auto &agent : snapshot) {
        const auto status = callback(agent.handle, data);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_agent_get_info(hsa_agent_t handle, hsa_agent_info_t attribute, void *value) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto agent = findAgent(handle);
    if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
    mac_hsa::DeviceSnapshot snapshot;
    if (agent->connection) {
        const auto status = agent->connection->read(snapshot);
        if (status != HSA_STATUS_SUCCESS) return status;
    }
    switch (attribute) {
    case HSA_AGENT_INFO_NAME:
        std::memset(value, 0, 64);
        if (agent->connection)
            std::snprintf(static_cast<char *>(value), 64, "gfx%u%u%x",
                          snapshot.gfxMajor, snapshot.gfxMinor, snapshot.gfxRevision);
        else std::snprintf(static_cast<char *>(value), 64, "Mac host CPU");
        return HSA_STATUS_SUCCESS;
    case HSA_AGENT_INFO_VENDOR_NAME:
        std::memset(value, 0, 64);
        std::snprintf(static_cast<char *>(value), 64, "%s", agent->connection ? "AMD" : "Apple");
        return HSA_STATUS_SUCCESS;
    case HSA_AGENT_INFO_DEVICE:
        return writeValue(value, agent->connection ? HSA_DEVICE_TYPE_GPU : HSA_DEVICE_TYPE_CPU);
    case HSA_AGENT_INFO_FEATURE:
        return writeValue(value, uint32_t(0)); // no kernel/agent dispatch implemented yet
    case HSA_AGENT_INFO_QUEUES_MAX:
        return writeValue(value, uint32_t(0));
    case HSA_AGENT_INFO_MACHINE_MODEL:
        return writeValue(value, HSA_MACHINE_MODEL_LARGE);
    case HSA_AGENT_INFO_PROFILE:
        return writeValue(value, HSA_PROFILE_BASE);
    case HSA_AGENT_INFO_VERSION_MAJOR:
        return writeValue(value, uint16_t(1));
    case HSA_AGENT_INFO_VERSION_MINOR:
        return writeValue(value, uint16_t(2));
    case HSA_AGENT_INFO_EXTENSIONS:
        std::memset(value, 0, 128);
        return HSA_STATUS_SUCCESS;
    case HSA_AGENT_INFO_CACHE_SIZE:
        std::memset(value, 0, 4 * sizeof(uint32_t)); // unknown
        return HSA_STATUS_SUCCESS;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}

hsa_status_t mac_hsa_agent_get_driver_info(hsa_agent_t handle,
                                          mac_hsa_device_info_t *info, size_t size) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!info || size != sizeof(*info)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto agent = findAgent(handle);
    if (!agent || !agent->connection) return HSA_STATUS_ERROR_INVALID_AGENT;
    mac_hsa::DeviceSnapshot snapshot;
    const auto status = agent->connection->read(snapshot);
    if (status != HSA_STATUS_SUCCESS) return status;
    *info = {snapshot.registryID, snapshot.build, snapshot.stage, snapshot.visibleVRAM,
             snapshot.totalVRAM, snapshot.gfxMajor, snapshot.gfxMinor, snapshot.gfxRevision, 0};
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_system_get_info(hsa_system_info_t attribute, void *value) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!value) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_SYSTEM_INFO_VERSION_MAJOR: return writeValue(value, uint16_t(1));
    case HSA_SYSTEM_INFO_VERSION_MINOR: return writeValue(value, uint16_t(2));
    case HSA_SYSTEM_INFO_TIMESTAMP:
        return writeValue(value, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    case HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY: return writeValue(value, uint64_t(1000000000));
    case HSA_SYSTEM_INFO_ENDIANNESS: return writeValue(value, HSA_ENDIANNESS_LITTLE);
    case HSA_SYSTEM_INFO_MACHINE_MODEL: return writeValue(value, HSA_MACHINE_MODEL_LARGE);
    case HSA_SYSTEM_INFO_EXTENSIONS:
        std::memset(value, 0, 128);
        return HSA_STATUS_SUCCESS;
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}

hsa_status_t hsa_system_major_extension_supported(uint16_t extension, uint16_t,
                                                  uint16_t *minor, bool *result) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!minor || !result ||
        (extension > HSA_EXTENSION_STD_LAST &&
         (extension < HSA_AMD_FIRST_EXTENSION || extension > HSA_AMD_LAST_EXTENSION))) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *minor = 0;
    *result = false;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_system_get_major_extension_table(uint16_t, uint16_t, size_t, void *) {
    std::lock_guard lock(runtimeMutex);
    return references ? HSA_STATUS_ERROR_INVALID_ARGUMENT : HSA_STATUS_ERROR_NOT_INITIALIZED;
}

hsa_status_t hsa_queue_create(hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type,
                             void (*)(hsa_status_t, hsa_queue_t *, void *), void *,
                             uint32_t, uint32_t, hsa_queue_t **queue) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!queue) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *queue = nullptr;
    if (!findAgent(agent)) return HSA_STATUS_ERROR_INVALID_AGENT;
    if (!size || (size & (size - 1)) || (type != HSA_QUEUE_TYPE_SINGLE && type != HSA_QUEUE_TYPE_MULTI))
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
}

hsa_status_t hsa_status_string(hsa_status_t status, const char **out) {
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (status) {
#define STATUS(code) case code: *out = #code; return HSA_STATUS_SUCCESS
    STATUS(HSA_STATUS_SUCCESS);
    STATUS(HSA_STATUS_INFO_BREAK);
    STATUS(HSA_STATUS_ERROR);
    STATUS(HSA_STATUS_ERROR_INVALID_ARGUMENT);
    STATUS(HSA_STATUS_ERROR_INVALID_QUEUE_CREATION);
    STATUS(HSA_STATUS_ERROR_INVALID_ALLOCATION);
    STATUS(HSA_STATUS_ERROR_INVALID_AGENT);
    STATUS(HSA_STATUS_ERROR_INVALID_REGION);
    STATUS(HSA_STATUS_ERROR_INVALID_SIGNAL);
    STATUS(HSA_STATUS_ERROR_INVALID_QUEUE);
    STATUS(HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    STATUS(HSA_STATUS_ERROR_INVALID_PACKET_FORMAT);
    STATUS(HSA_STATUS_ERROR_RESOURCE_FREE);
    STATUS(HSA_STATUS_ERROR_NOT_INITIALIZED);
    STATUS(HSA_STATUS_ERROR_REFCOUNT_OVERFLOW);
    STATUS(HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
    STATUS(HSA_STATUS_ERROR_INVALID_INDEX);
    STATUS(HSA_STATUS_ERROR_INVALID_ISA);
    STATUS(HSA_STATUS_ERROR_INVALID_ISA_NAME);
    STATUS(HSA_STATUS_ERROR_INVALID_CODE_OBJECT);
    STATUS(HSA_STATUS_ERROR_INVALID_EXECUTABLE);
    STATUS(HSA_STATUS_ERROR_FROZEN_EXECUTABLE);
    STATUS(HSA_STATUS_ERROR_INVALID_SYMBOL_NAME);
    STATUS(HSA_STATUS_ERROR_VARIABLE_ALREADY_DEFINED);
    STATUS(HSA_STATUS_ERROR_VARIABLE_UNDEFINED);
    STATUS(HSA_STATUS_ERROR_EXCEPTION);
    STATUS(HSA_STATUS_ERROR_INVALID_CODE_SYMBOL);
    STATUS(HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL);
    STATUS(HSA_STATUS_ERROR_INVALID_FILE);
    STATUS(HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER);
    STATUS(HSA_STATUS_ERROR_INVALID_CACHE);
    STATUS(HSA_STATUS_ERROR_INVALID_WAVEFRONT);
    STATUS(HSA_STATUS_ERROR_INVALID_SIGNAL_GROUP);
    STATUS(HSA_STATUS_ERROR_INVALID_RUNTIME_STATE);
    STATUS(HSA_STATUS_ERROR_FATAL);
#undef STATUS
    default: return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
}
} // extern C
