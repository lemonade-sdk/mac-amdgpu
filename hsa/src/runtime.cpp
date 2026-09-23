#include "runtime_state.h"
#include "mac_hsa.h"
#include "signal_state.h"
#include <hsa/hsa_ext_amd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <sys/sysctl.h>

namespace mac_hsa::detail {
std::mutex runtimeMutex;
uint32_t references = 0;
uint64_t lastHandle = 0;
std::vector<Agent> agents;
std::vector<Pool> pools;
std::map<uintptr_t, std::shared_ptr<Allocation>> allocations;
std::vector<std::unique_ptr<CopyJob>> copyJobs;
std::unordered_map<uint64_t, std::shared_ptr<mac_hsa::Signal>> signals;
std::shared_ptr<mac_hsa::Signal> findSignal(hsa_signal_t handle) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return {};
    const auto entry = signals.find(handle.handle);
    return entry == signals.end() ? nullptr : entry->second;
}


// Caller holds runtimeMutex. Opaque IDs are never reused across sessions.
Agent *findAgent(hsa_agent_t handle) {
    for (auto &agent : agents) if (agent.handle.handle == handle.handle) return &agent;
    return nullptr;
}

uint32_t waitSignals(bool all, uint32_t count, hsa_signal_t *handles,
    hsa_signal_condition_t *conditions, hsa_signal_value_t *values,
    uint64_t timeout, hsa_wait_state_t hint, hsa_signal_value_t *observed) {
    if (count && (!handles || !conditions || !values)) return UINT32_MAX;
    try {
        std::vector<std::shared_ptr<mac_hsa::Signal>> waiting;
        std::vector<bool> satisfied(count, false);
        waiting.reserve(count);
        bool valid = false;
        for (uint32_t i = 0; i < count; ++i) {
            waiting.push_back(findSignal(handles[i]));
            valid |= bool(waiting.back());
            if (all && observed) observed[i] = 0;
            if (waiting.back() && (conditions[i] < HSA_SIGNAL_CONDITION_EQ ||
                                  conditions[i] > HSA_SIGNAL_CONDITION_GTE)) return UINT32_MAX;
        }
        if (!valid) return all ? 0 : UINT32_MAX;
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            bool complete = true;
            for (uint32_t i = 0; i < count; ++i) {
                const auto &signal = waiting[i];
                if (!signal || (all && satisfied[i])) continue;
                if (!signal->alive.load()) return UINT32_MAX;
                const auto value = signal->value().load(std::memory_order_relaxed);
                if (mac_hsa::signalCondition(value, conditions[i], values[i])) {
                    if (observed) observed[all ? i : 0] = value;
                    if (!all) return i;
                    satisfied[i] = true;
                } else complete = false;
            }
            if (all && complete) return 0;
            const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            if (elapsed >= timeout) return UINT32_MAX;
            if (hint == HSA_WAIT_STATE_ACTIVE) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(
                1000000, timeout - elapsed)));
        }
    } catch (const std::bad_alloc &) { return UINT32_MAX; }
}

} // namespace mac_hsa::detail

using namespace mac_hsa::detail;

extern "C" {
hsa_status_t hsa_init() {
    std::lock_guard lock(runtimeMutex);
    if (references == INT32_MAX) return HSA_STATUS_ERROR_REFCOUNT_OVERFLOW;
    if (references) { ++references; return HSA_STATUS_SUCCESS; }
    try {
        std::vector<std::shared_ptr<mac_hsa::Connection>> connections;
        const auto status = mac_hsa::discover(connections);
        if (status != HSA_STATUS_SUCCESS) return status;
        if (lastHandle >= UINT64_MAX - 2 || connections.size() > (UINT64_MAX - lastHandle - 2) / 2)
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        std::vector<Agent> fresh;
        fresh.reserve(connections.size() + 1);
        fresh.push_back({{++lastHandle}, nullptr});
        for (auto &connection : connections)
            fresh.push_back({{++lastHandle}, std::move(connection)});
        uint64_t capacity = 0;
        size_t capacitySize = sizeof(capacity);
        if (sysctlbyname("hw.memsize", &capacity, &capacitySize, nullptr, 0) || !capacity)
            return HSA_STATUS_ERROR;
        std::vector<Pool> freshPools{{++lastHandle, fresh.front().handle, size_t(capacity), nullptr}};
        for (const auto &agent : fresh)
            if (agent.connection && agent.connection->supportsBuffers())
                freshPools.push_back({++lastHandle, agent.handle, 0, agent.connection});
        pools.swap(freshPools);
        agents.swap(fresh);
        references = 1;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) {
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
}

hsa_status_t hsa_shut_down() {
    std::vector<Agent> retiredAgents;
    std::vector<std::unique_ptr<CopyJob>> retiredJobs;
    std::map<uintptr_t, std::shared_ptr<Allocation>> retiredAllocations;
    std::unordered_map<uint64_t, std::shared_ptr<Executable>> retiredExecutables;
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!--references) {
            for (auto &[handle, signal] : signals) {
                (void)handle;
                signal->alive.store(false);
                signal->changed.notify_all();
            }
            for (auto &job : copyJobs) job->worker.request_stop();
            retiredJobs.swap(copyJobs);
            retiredExecutables.swap(executables);
            executableSymbols.clear();
            codeReaders.clear();
            clearQueues();
            signals.clear();
            pools.clear();
            retiredAllocations.swap(allocations);
            retiredAgents.swap(agents);
        }
    }
    // Joining workers or closing a future owning connection must never run
    // under the global lock. Jobs retain every runtime-owned buffer they use.
    retiredJobs.clear();
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

hsa_status_t hsa_signal_create(hsa_signal_value_t initial, uint32_t count,
                               const hsa_agent_t *consumers, hsa_signal_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out || (count && !consumers)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    bool needsGPU = false;
    if (!count) for (const auto &agent : agents) needsGPU |= bool(agent.connection);
    for (uint32_t i = 0; i < count; ++i) {
        const auto *agent = findAgent(consumers[i]);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        for (uint32_t j = 0; j < i; ++j)
            if (consumers[i].handle == consumers[j].handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        needsGPU |= bool(agent->connection);
    }
    if (needsGPU) return HSA_STATUS_ERROR_OUT_OF_RESOURCES; // shared GPU signal backing pending
    try {
        auto signal = std::make_shared<mac_hsa::Signal>();
        signal->abi.value = initial;
        const uint64_t handle = reinterpret_cast<uintptr_t>(&signal->abi);
        signals.emplace(handle, std::move(signal));
        out->handle = handle;
        return HSA_STATUS_SUCCESS;
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}

HSA_API_EXPORT hsa_status_t hsa_amd_signal_create(hsa_signal_value_t initial, uint32_t count,
    const hsa_agent_t *consumers, uint64_t attributes, hsa_signal_t *out) {
    {
        std::lock_guard lock(runtimeMutex);
        if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
        if (!out || (attributes & ~(uint64_t(HSA_AMD_SIGNAL_AMD_GPU_ONLY) | HSA_AMD_SIGNAL_IPC)))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        *out = {};
        if ((attributes & HSA_AMD_SIGNAL_IPC) || (!count && (attributes & HSA_AMD_SIGNAL_AMD_GPU_ONLY)))
            return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    // GPU_ONLY is ignored when an explicit consumer list is supplied, as
    // specified by the AMD ABI. IPC needs separate shared lifetime/backing.
    return hsa_signal_create(initial, count, consumers, out);
}

HSA_API_EXPORT uint32_t hsa_amd_signal_wait_all(uint32_t count, hsa_signal_t *handles,
    hsa_signal_condition_t *conditions, hsa_signal_value_t *values,
    uint64_t timeout, hsa_wait_state_t hint, hsa_signal_value_t *observed) {
    return waitSignals(true, count, handles, conditions, values, timeout, hint, observed);
}
HSA_API_EXPORT uint32_t hsa_amd_signal_wait_any(uint32_t count, hsa_signal_t *handles,
    hsa_signal_condition_t *conditions, hsa_signal_value_t *values,
    uint64_t timeout, hsa_wait_state_t hint, hsa_signal_value_t *observed) {
    return waitSignals(false, count, handles, conditions, values, timeout, hint, observed);
}

hsa_status_t hsa_signal_destroy(hsa_signal_t handle) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!handle.handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    const auto entry = signals.find(handle.handle);
    if (entry == signals.end()) return HSA_STATUS_ERROR_INVALID_SIGNAL;
    entry->second->alive.store(false);
    entry->second->changed.notify_all();
    signals.erase(entry);
    return HSA_STATUS_SUCCESS;
}

hsa_signal_value_t hsa_signal_load_relaxed(hsa_signal_t handle) {
    const auto signal = findSignal(handle);
    return signal ? signal->value().load(std::memory_order_relaxed) : 0;
}
hsa_signal_value_t hsa_signal_wait_relaxed(hsa_signal_t handle, hsa_signal_condition_t condition,
    hsa_signal_value_t compare, uint64_t timeout, hsa_wait_state_t hint) {
    return mac_hsa::waitSignal(findSignal(handle), condition, compare, timeout, hint,
                               std::memory_order_relaxed);
}
hsa_signal_value_t hsa_signal_load_scacquire(hsa_signal_t handle) {
    const auto signal = findSignal(handle);
    return signal ? signal->value().load(std::memory_order_acquire) : 0;
}
hsa_signal_value_t hsa_signal_wait_scacquire(hsa_signal_t handle, hsa_signal_condition_t condition,
    hsa_signal_value_t compare, uint64_t timeout, hsa_wait_state_t hint) {
    return mac_hsa::waitSignal(findSignal(handle), condition, compare, timeout, hint,
                               std::memory_order_acquire);
}
void hsa_signal_store_relaxed(hsa_signal_t handle, hsa_signal_value_t value) {
    const auto signal = findSignal(handle);
    if (!signal) return;
    signal->value().store(value, std::memory_order_relaxed);
    signal->changed.notify_all();
}
void hsa_signal_silent_store_relaxed(hsa_signal_t handle, hsa_signal_value_t value) {
    const auto signal = findSignal(handle);
    if (!signal) return;
    signal->value().store(value, std::memory_order_relaxed);
}
void hsa_signal_store_screlease(hsa_signal_t handle, hsa_signal_value_t value) {
    const auto signal = findSignal(handle);
    if (!signal) return;
    signal->value().store(value, std::memory_order_release);
    signal->changed.notify_all();
}
void hsa_signal_silent_store_screlease(hsa_signal_t handle, hsa_signal_value_t value) {
    const auto signal = findSignal(handle);
    if (!signal) return;
    signal->value().store(value, std::memory_order_release);
}
// Keep all atomic variants on the same 64-bit storage, with the ordering
// specified by each HSA entry point. Fetch arithmetic uses atomic wraparound.
#define SIGNAL_RMW(name, op, suffix, order) \
void hsa_signal_##name##_##suffix(hsa_signal_t handle, hsa_signal_value_t value) { \
    const auto signal = findSignal(handle); \
    if (!signal) return; \
    signal->value().op(value, order); \
    signal->changed.notify_all(); \
}
#define SIGNAL_VALUE_RMW(suffix, order, failure) \
hsa_signal_value_t hsa_signal_exchange_##suffix(hsa_signal_t handle, hsa_signal_value_t value) { \
    const auto signal = findSignal(handle); \
    if (!signal) return 0; \
    const auto old = signal->value().exchange(value, order); \
    signal->changed.notify_all(); return old; \
} \
hsa_signal_value_t hsa_signal_cas_##suffix(hsa_signal_t handle, hsa_signal_value_t expected, \
                                         hsa_signal_value_t desired) { \
    const auto signal = findSignal(handle); \
    if (!signal) return 0; \
    signal->value().compare_exchange_strong(expected, desired, order, failure); \
    signal->changed.notify_all(); return expected; \
}
#define SIGNAL_ATOMICS(suffix, order, failure) \
SIGNAL_RMW(add, fetch_add, suffix, order) \
SIGNAL_RMW(subtract, fetch_sub, suffix, order) \
SIGNAL_RMW(and, fetch_and, suffix, order) \
SIGNAL_RMW(or, fetch_or, suffix, order) \
SIGNAL_RMW(xor, fetch_xor, suffix, order) \
SIGNAL_VALUE_RMW(suffix, order, failure)
SIGNAL_ATOMICS(relaxed, std::memory_order_relaxed, std::memory_order_relaxed)
SIGNAL_ATOMICS(scacquire, std::memory_order_acquire, std::memory_order_acquire)
SIGNAL_ATOMICS(screlease, std::memory_order_release, std::memory_order_relaxed)
SIGNAL_ATOMICS(scacq_screl, std::memory_order_acq_rel, std::memory_order_acquire)
#undef SIGNAL_ATOMICS
#undef SIGNAL_VALUE_RMW
#undef SIGNAL_RMW

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
