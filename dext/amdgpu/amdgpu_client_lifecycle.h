#pragma once
#include <stdint.h>

namespace amdgpu {
// Protected by the driver's serial lifecycle queue. PCI belongs to the driver;
// clients hold participant references and, during bootstrap, one initialization
// lease. Legacy raw access remains exclusive until reset or client retirement.
struct ClientSessions {
    void *initializationClient = nullptr;
    void *exclusiveClient = nullptr;
    uint32_t participants = 0;

    bool attach(void *client, bool &attached, bool ready) {
        const auto exclusive = __atomic_load_n(&exclusiveClient, __ATOMIC_ACQUIRE);
        if (exclusive && exclusive != client) return false;
        if (!ready && initializationClient && initializationClient != client) return false;
        if (ready) initializationClient = nullptr;
        if (attached) return true;
        if (participants == UINT32_MAX) return false;
        if (!ready) initializationClient = client;
        ++participants;
        attached = true;
        return true;
    }
    bool claimExclusive(void *client, bool attached) {
        const auto exclusive = __atomic_load_n(&exclusiveClient, __ATOMIC_ACQUIRE);
        if (!attached || participants != 1 || (exclusive && exclusive != client)) return false;
        __atomic_store_n(&exclusiveClient, client, __ATOMIC_RELEASE);
        return true;
    }
    bool canReset(void *client, bool attached) const {
        const auto exclusive = __atomic_load_n(&exclusiveClient, __ATOMIC_ACQUIRE);
        return participants == (attached ? 1u : 0u) && (!exclusive || exclusive == client);
    }
    void detach(void *client, bool &attached) {
        if (attached) { if (participants) --participants; attached = false; }
        if (initializationClient == client) initializationClient = nullptr;
        if (__atomic_load_n(&exclusiveClient, __ATOMIC_ACQUIRE) == client)
            __atomic_store_n(&exclusiveClient, (void *)nullptr, __ATOMIC_RELEASE);
    }
};

// RPC inputs must not overflow or keep the serial lifecycle queue spinning.
inline bool client_allocation_shape(uint64_t size, uint64_t requestedAlignment,
                                    uint64_t minimumAlignment,
                                    uint64_t &alignment, uint64_t &rounded)
{
    if (!size || !minimumAlignment || (minimumAlignment & (minimumAlignment - 1))) return false;
    alignment = requestedAlignment < minimumAlignment ? minimumAlignment : requestedAlignment;
    if (alignment > (uint64_t(1) << 63)) return false;
    if (alignment & (alignment - 1)) {
        uint64_t next = minimumAlignment;
        while (next < alignment) next <<= 1;
        alignment = next;
    }
    if (size > UINT64_MAX - (alignment - 1)) return false;
    rounded = (size + alignment - 1) & ~(alignment - 1);
    return rounded != 0;
}

inline bool client_subrange(uint64_t offset, uint64_t size, uint64_t capacity)
{
    return offset <= capacity && size <= capacity - offset;
}

// A wait may occupy the shared queue for at most one second. Async waiting
// and per-job BO references are required before supporting general workloads.
inline uint64_t client_wait_ns(uint64_t requested, bool legacyMicroseconds = false)
{
    constexpr uint64_t maximum = 1000000000ull;
    if (!requested) return maximum;
    if (legacyMicroseconds) return requested >= maximum / 1000 ? maximum : requested * 1000;
    return requested > maximum ? maximum : requested;
}

// One raw submission at a time. Completion is latched before the shared WB
// slot is reused, so later submissions cannot erase an older CS's result.
struct ClientSubmission {
    bool pending = false;
    bool usesCP = false;
    uint32_t issued = 0;
    uint32_t completed = 0;
    uint64_t expected = 0;
    uint64_t lastCPFence = 0;
    uint64_t completedCPFence = 0;
    volatile uint32_t *sdmaFence = nullptr;
    volatile uint64_t *cpFence = nullptr;
    bool (*sdmaReader)(void *, uint32_t *) = nullptr;
    void *sdmaReaderContext = nullptr;
    bool (*cpReader)(void *, uint64_t *) = nullptr;
    void *cpReaderContext = nullptr;

    bool poll() {
        if (pending && usesCP && cpReader) {
            uint64_t observed = 0;
            if (!cpReader(cpReaderContext, &observed) || observed == UINT64_MAX)
                return false;
            if (cpFence) __atomic_store_n(cpFence, observed, __ATOMIC_RELEASE);
        }
        if (pending && !usesCP && sdmaReader) {
            uint32_t observed = 0;
            // A failed/removed-device read cannot complete or recycle a job.
            if (!sdmaReader(sdmaReaderContext, &observed) || observed == UINT32_MAX)
                return false;
            if (sdmaFence) __atomic_store_n(sdmaFence, observed, __ATOMIC_RELEASE);
        }
        if (pending && expected &&
            (usesCP ? cpFence && __atomic_load_n(cpFence, __ATOMIC_ACQUIRE) == expected
                    : sdmaFence && __atomic_load_n(sdmaFence, __ATOMIC_ACQUIRE) == expected)) {
            if (usesCP) completedCPFence = expected;
            else completed = uint32_t(expected);
            pending = false;
        }
        return !pending;
    }
    uint32_t beginSDMA(volatile uint32_t *slot,
                       bool (*reader)(void *, uint32_t *) = nullptr,
                       void *context = nullptr) {
        // Reserve all-ones for PCI read failure, never a successful sequence.
        if (!poll() || issued >= UINT32_MAX - 1 || !slot) return 0;
        sdmaReader = reader;
        sdmaReaderContext = context;
        expected = ++issued;
        sdmaFence = slot;
        usesCP = false;
        pending = true; // stays set on a partial ring-write or failed kick
        __atomic_store_n(slot, 0, __ATOMIC_RELEASE);
        return uint32_t(expected);
    }
    bool beginCP(volatile uint64_t *slot,
                 bool (*reader)(void *, uint64_t *) = nullptr,
                 void *context = nullptr) {
        if (!poll() || !slot) return false;
        cpFence = slot;
        cpReader = reader;
        cpReaderContext = context;
        expected = 0; // no completion accepted until the fence is emitted
        usesCP = true;
        pending = true;
        return true;
    }
};
}
