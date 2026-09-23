#pragma once
#include <hsa/hsa.h>
#include <hsa/amd_hsa_signal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mac_hsa {
// AMD's 64-byte signal layout (amd_hsa_signal.h). This backing currently
// supports CPU consumers only. GPU access needs a coherent shared mapping;
// ordinary host addresses must never be presented as GPU signal addresses.
struct alignas(64) SignalABI {
    int64_t kind = 1;
    int64_t value = 0;
    uint64_t eventMailbox = 0;
    uint32_t eventID = 0, reserved1 = 0;
    uint64_t startTimestamp = 0, endTimestamp = 0, queue = 0;
    uint32_t reserved3[2]{};
};
static_assert(sizeof(SignalABI) == sizeof(amd_signal_t));
static_assert(alignof(SignalABI) == alignof(amd_signal_t));
static_assert(offsetof(SignalABI, value) == offsetof(amd_signal_t, value));
static_assert(offsetof(SignalABI, queue) == offsetof(amd_signal_t, queue_ptr));
static_assert(std::atomic_ref<int64_t>::is_always_lock_free);
struct Signal {
    SignalABI abi;
    std::atomic<bool> alive{true};
    std::mutex waitMutex;
    std::condition_variable changed;
    std::atomic_ref<int64_t> value() { return std::atomic_ref<int64_t>(abi.value); }
};
inline bool signalCondition(int64_t value, hsa_signal_condition_t condition, int64_t compare) {
    switch (condition) {
    case HSA_SIGNAL_CONDITION_EQ: return value == compare;
    case HSA_SIGNAL_CONDITION_NE: return value != compare;
    case HSA_SIGNAL_CONDITION_LT: return value < compare;
    case HSA_SIGNAL_CONDITION_GTE: return value >= compare;
    default: return false;
    }
}
inline int64_t waitSignal(const std::shared_ptr<Signal> &signal,
                         hsa_signal_condition_t condition, int64_t compare,
                         uint64_t timeout, hsa_wait_state_t hint, std::memory_order order) {
    if (!signal) return 0; // invalid-handle operations have undefined HSA behavior
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const auto value = signal->value().load(order);
        if (signalCondition(value, condition, compare) || !signal->alive.load() || !timeout)
            return value;
        const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        if (elapsed >= timeout) return value;
        if (hint == HSA_WAIT_STATE_ACTIVE) std::this_thread::yield();
        else {
            // Poll as well as notify: direct host atomic writes and silent stores
            // do not notify this condition variable. Never hold the runtime lock.
            const auto interval = std::min<uint64_t>(1000000, timeout - elapsed);
            std::unique_lock lock(signal->waitMutex);
            signal->changed.wait_for(lock, std::chrono::nanoseconds(interval));
        }
    }
}
}
