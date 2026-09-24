#pragma once
#include "signal_mailbox_layout.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>

namespace mac_hsa {
struct SignalOperation { uint64_t operation,operand,compare; };
static_assert(sizeof(SignalOperation)==MAC_MAILBOX_REQUEST_STRIDE*sizeof(uint64_t));
enum class MailboxWait { Active, Hybrid };
enum class MailboxResult { Success, InvalidArgument, Failed, Timeout, SequenceError };
class SignalMailboxClient {
    uint64_t *words_;
    uint64_t sequence_=0;
    MailboxWait wait_;
    bool failed_=false;
public:
    uint64_t polls=0,sleeps=0;
    explicit SignalMailboxClient(uint64_t *words,MailboxWait wait):words_(words),wait_(wait) {}
    uint64_t load(size_t word,std::memory_order order=std::memory_order_acquire) const {
        return std::atomic_ref<uint64_t>(words_[word]).load(order);
    }
    void store(size_t word,uint64_t value,std::memory_order order=std::memory_order_release) {
        std::atomic_ref<uint64_t>(words_[word]).store(value,order);
    }
    void cancel() {failed_=true;store(MAC_MAILBOX_ABORT,1);}
    MailboxResult await(size_t word,uint64_t expected,std::chrono::steady_clock::time_point deadline) {
        const auto start=std::chrono::steady_clock::now();
        for (;;) {
            const auto observed=load(word);
            if (observed==expected) return MailboxResult::Success;
            if (failed_ || load(MAC_MAILBOX_STATE)>=2) {cancel();return MailboxResult::Failed;}
            if (observed>expected) {cancel();return MailboxResult::SequenceError;}
            const auto now=std::chrono::steady_clock::now();
            if (now>=deadline) {cancel();return MailboxResult::Timeout;}
            ++polls;
            if (wait_==MailboxWait::Hybrid && now-start>=std::chrono::microseconds(50)) {
                ++sleeps;std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }
    MailboxResult execute(std::span<const SignalOperation> operations,std::span<uint64_t> oldValues,
                         std::chrono::steady_clock::time_point deadline) {
        if (failed_) return MailboxResult::Failed;
        if (operations.empty() || operations.size()>MAC_MAILBOX_CAPACITY || oldValues.size()!=operations.size() ||
            sequence_==UINT64_MAX) return MailboxResult::InvalidArgument;
        for (const auto &op:operations) if (op.operation<1 || op.operation>9) return MailboxResult::InvalidArgument;
        // This object has one CPU owner. The caller serializes producers, just
        // as the existing per-device signal executor serializes HSA API calls.
        if (load(MAC_MAILBOX_COMPLETION_SEQUENCE)!=sequence_) {cancel();return MailboxResult::SequenceError;}
        for (size_t i=0;i<operations.size();++i) {
            const auto base=MAC_MAILBOX_REQUESTS+i*MAC_MAILBOX_REQUEST_STRIDE;
            store(base,operations[i].operation,std::memory_order_relaxed);
            store(base+1,operations[i].operand,std::memory_order_relaxed);
            store(base+2,operations[i].compare,std::memory_order_relaxed);
        }
        store(MAC_MAILBOX_COUNT,operations.size(),std::memory_order_relaxed);
        store(MAC_MAILBOX_REQUEST_SEQUENCE,++sequence_);
        const auto result=await(MAC_MAILBOX_COMPLETION_SEQUENCE,sequence_,deadline);
        if (result!=MailboxResult::Success) return result;
        for (size_t i=0;i<operations.size();++i) oldValues[i]=load(MAC_MAILBOX_RESULTS+i,std::memory_order_relaxed);
        return MailboxResult::Success;
    }
};
}
