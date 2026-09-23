#pragma once

#include "transport.h"
#include "signal_state.h"
#include <hsa/hsa_ext_amd.h>
#include <cstdlib>
#include <cstring>
#include <map>

namespace mac_hsa::detail {
struct Agent {
    hsa_agent_t handle;
    std::shared_ptr<Connection> connection; // null for host CPU
};
struct Pool {
    uint64_t handle;
    hsa_agent_t owner;
    size_t capacity;
    std::shared_ptr<Connection> connection;
};
struct Allocation {
    hsa_amd_pointer_type_t type = HSA_EXT_POINTER_TYPE_HSA;
    hsa_access_permission_t access = HSA_ACCESS_PERMISSION_RW;
    BufferToken ipcToken{};
    uint32_t ipcReferences = 1;
    std::shared_ptr<void> backing; // mappings retain their reservation and storage
    void *base = nullptr;
    size_t size = 0;
    hsa_agent_t owner{};
    void *userData = nullptr;
    std::shared_ptr<Connection> connection;
    DeviceBuffer buffer;
    hsa_status_t release() {
        if (!connection || !buffer.handle) return HSA_STATUS_SUCCESS;
        const auto status = connection->freeBuffer(buffer);
        if (status == HSA_STATUS_SUCCESS) { buffer = {}; base = nullptr; }
        return status;
    }
    ~Allocation() {
        if (connection) { if (buffer.handle) release(); }
        else if (!backing && type == HSA_EXT_POINTER_TYPE_HSA) std::free(base);
    }
};
struct CopyJob {
    std::atomic<bool> done{false};
    std::jthread worker;
};

extern std::mutex runtimeMutex;
extern uint32_t references;
extern uint64_t lastHandle;
extern std::vector<Agent> agents;
extern std::unordered_map<uint64_t, std::shared_ptr<Signal>> signals;
extern std::vector<Pool> pools;
extern std::map<uintptr_t, std::shared_ptr<Allocation>> allocations;
extern std::vector<std::unique_ptr<CopyJob>> copyJobs;
struct Executable;
struct ExecutableSymbol;
struct CodeReader;
extern std::unordered_map<uint64_t, std::shared_ptr<Executable>> executables;
extern std::unordered_map<uint64_t, std::shared_ptr<ExecutableSymbol>> executableSymbols;
extern std::unordered_map<uint64_t, std::shared_ptr<CodeReader>> codeReaders;

// Caller holds runtimeMutex. IDs are never reused across runtime sessions.
Agent *findAgent(hsa_agent_t handle);
Pool *findPool(uint64_t handle);
std::shared_ptr<Allocation> findAllocation(const void *pointer);
std::shared_ptr<Signal> findSignal(hsa_signal_t handle);
void clearQueues(); // caller holds runtimeMutex; CPU software queues only
size_t hostPageSize();
void clearVirtualMemory(); // caller holds runtimeMutex; allocation pins retain mappings
void clearHostLocks();
bool describeHostLock(const void *pointer, hsa_amd_pointer_info_t &info); // caller holds runtimeMutex
void clearCaches();
void reapCopyJobs();
void clearSystemEvents();
hsa_status_t deliverSystemEvent(const hsa_amd_event_t &event);
hsa_status_t createIPCSignal(hsa_signal_value_t initial, uint32_t count, const hsa_agent_t *consumers, hsa_signal_t *out);

template<typename T> hsa_status_t writeValue(void *output, T value) {
    std::memcpy(output, &value, sizeof(value));
    return HSA_STATUS_SUCCESS;
}
} // namespace mac_hsa::detail
