#include "signal_mailbox.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace mac_hsa;
using Clock=std::chrono::steady_clock;
constexpr uint64_t guard=0xa55ac33c98765432ull;
uint64_t peerOperation(SignalOperation op,uint64_t &value) {
    const auto old=value;
    switch(op.operation) {
    case 1:case 7:value=op.operand;break;
    case 2:value+=op.operand;break;
    case 3:value-=op.operand;break;
    case 4:value&=op.operand;break;
    case 5:value|=op.operand;break;
    case 6:value^=op.operand;break;
    case 8:if(value==op.compare)value=op.operand;break;
    case 9:break;
    default:assert(false);
    }
    return old;
}
int main() {
    const std::array<SignalOperation,10> operations={{{7,UINT64_MAX,0},{2,2,0},{3,3,0},
        {4,255,0},{5,256,0},{6,0x55,0},{8,17,427},{8,99,18},{9,0,0},{1,0,0}}};
    const std::array<uint64_t,10> expected={0,UINT64_MAX,1,UINT64_MAX-1,254,510,427,17,17,17};
    for(auto mode:{MailboxWait::Active,MailboxWait::Hybrid})for(size_t batch:{1,3,64}) {
        alignas(128) std::array<uint64_t,MAC_MAILBOX_WORDS> data{};
        data.back()=guard;uint64_t canonical=0;SignalMailboxClient client(data.data(),mode),peer(data.data(),mode);
        const size_t batches=(operations.size()+batch-1)/batch;
        std::thread worker([&] {
            peer.store(MAC_MAILBOX_READY,1);
            for(size_t seq=1;seq<=batches;++seq) {
                while(peer.load(MAC_MAILBOX_REQUEST_SEQUENCE)!=seq)std::this_thread::yield();
                const auto count=peer.load(MAC_MAILBOX_COUNT,std::memory_order_relaxed);
                for(size_t i=0;i<count;++i) {
                    const auto base=MAC_MAILBOX_REQUESTS+i*MAC_MAILBOX_REQUEST_STRIDE;
                    SignalOperation op{peer.load(base,std::memory_order_relaxed),peer.load(base+1,std::memory_order_relaxed),peer.load(base+2,std::memory_order_relaxed)};
                    peer.store(MAC_MAILBOX_RESULTS+i,peerOperation(op,canonical),std::memory_order_relaxed);
                }
                peer.store(MAC_MAILBOX_COMPLETION_SEQUENCE,seq);
            }
            peer.store(MAC_MAILBOX_STATE,1);
        });
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        assert(client.await(MAC_MAILBOX_READY,1,deadline)==MailboxResult::Success);
        size_t count=0;
        while(count<operations.size()) {
            const auto n=std::min(batch,operations.size()-count);std::array<uint64_t,MAC_MAILBOX_CAPACITY> old{};
            assert(client.execute({operations.data()+count,n},{old.data(),n},deadline)==MailboxResult::Success);
            for(size_t i=0;i<n;++i)assert(old[i]==expected[count+i]);
            count+=n;
        }
        worker.join();assert(canonical==0 && data.back()==guard);
    }
    for(unsigned injection=0;injection<4;++injection) {
        std::array<uint64_t,MAC_MAILBOX_WORDS> data{};SignalMailboxClient client(data.data(),MailboxWait::Hybrid);
        uint64_t old=guard;
        if(injection==0)client.store(MAC_MAILBOX_STATE,3);
        if(injection==1)client.store(MAC_MAILBOX_COMPLETION_SEQUENCE,7);
        if(injection==2)client.cancel();
        const auto result=client.execute({operations.data(),1},{&old,1},Clock::now()+std::chrono::milliseconds(2));
        assert(result==(injection==0 || injection==2 ? MailboxResult::Failed : injection==1 ? MailboxResult::SequenceError : MailboxResult::Timeout));
        assert(old==guard && client.load(MAC_MAILBOX_ABORT)==1);
        assert(client.execute({operations.data(),1},{&old,1},Clock::now())==MailboxResult::Failed);
        assert(old==guard);
    }
    std::array<uint64_t,MAC_MAILBOX_WORDS> data{};SignalMailboxClient client(data.data(),MailboxWait::Active);uint64_t old=guard;
    SignalOperation invalid{10,0,0};
    assert(client.execute({&invalid,1},{&old,1},Clock::now())==MailboxResult::InvalidArgument);
    assert(client.execute({},{},Clock::now())==MailboxResult::InvalidArgument);
    assert(client.execute({operations.data(),1},{},Clock::now())==MailboxResult::InvalidArgument);
    assert(client.load(MAC_MAILBOX_REQUEST_SEQUENCE)==0 && old==guard);
    std::puts("Mailbox host protocol: all operation return values, wraparound, both CAS outcomes, batching, active/hybrid, failure/timeout/cancellation, and no failed output publication passed; no GPU access.");
}
