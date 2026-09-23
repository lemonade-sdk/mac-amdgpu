// Software peer exercises the hardware test's completion/error/timeout oracle.
// It establishes no CPU/GPU coherency result and never opens a GPU connection.
#define main atomic_tool_unused_main
#include "../hsa/tools/mac_hsa_atomic_contention_test.cpp"
#undef main
#include <cassert>

namespace {
std::atomic<bool> mockDoorbell{false};
std::atomic<int64_t> mockCompletion{1};
std::atomic<uint64_t> mockWrite{0};
constexpr hsa_signal_t mockDone{0x111},mockBell{0x222};
enum class Injection {None,GPUPayload,GPUErrors,Guard,KernargGuard,SkippedSequence,NoReady};
}
extern "C" uint64_t hsa_queue_add_write_index_relaxed(const hsa_queue_t *,uint64_t value) {
    return mockWrite.fetch_add(value);
}
extern "C" uint64_t hsa_queue_load_read_index_scacquire(const hsa_queue_t *) {return 0;}
extern "C" void hsa_signal_store_screlease(hsa_signal_t signal,hsa_signal_value_t value) {
    if (signal.handle==mockDone.handle) mockCompletion.store(value,std::memory_order_release);
    else {assert(signal.handle==mockBell.handle);mockDoorbell.store(true,std::memory_order_release);}
}
extern "C" hsa_signal_value_t hsa_signal_load_scacquire(hsa_signal_t signal) {
    assert(signal.handle==mockDone.handle);return mockCompletion.load(std::memory_order_acquire);
}

int main() {
    alignas(64) hsa_kernel_dispatch_packet_t ring[64]{};
    hsa_queue_t queue{};queue.size=64;queue.base_address=ring;queue.doorbell_signal=mockBell;
    for (auto injection:{Injection::None,Injection::GPUPayload,Injection::GPUErrors,Injection::Guard,Injection::KernargGuard,Injection::SkippedSequence,Injection::NoReady}) {
        std::vector<uint64_t> data(words),arguments(words);
        mockDoorbell=false;mockCompletion=1;mockWrite=0;queueErrors=0;
        constexpr uint64_t rounds=256;
        std::thread peer([&] {
            while (!mockDoorbell.load(std::memory_order_acquire)) std::this_thread::yield();
            assert(arguments[0]==reinterpret_cast<uintptr_t>(data.data()) && arguments[1]==rounds);
            assert(ring[0].kernarg_address==arguments.data() && ring[0].completion_signal.handle==mockDone.handle);
            if (injection==Injection::NoReady) {
                while (!load(data.data(),32)) std::this_thread::yield();
                store(data.data(),48,2);mockCompletion.store(0,std::memory_order_release);return;
            }
            store(data.data(),16,1);
            for (uint64_t round=1;round<=rounds;++round) {
                while (load(data.data(),0)!=round*2-1) {
                    if (load(data.data(),32)) {store(data.data(),48,2);mockCompletion.store(0,std::memory_order_release);return;}
                    std::this_thread::yield();
                }
                for (uint64_t word=0;word<64;++word)
                    assert(std::atomic_ref<uint64_t>(data[128+word]).load(std::memory_order_relaxed)==
                        (0x13579bdf2468ace0ull^(round*0x9e3779b97f4a7c15ull)^(word*0x0101010101010101ull)));
                if (injection==Injection::SkippedSequence) {
                    store(data.data(),0,round*2+2);store(data.data(),48,2);
                    mockCompletion.store(0,std::memory_order_release);return;
                }
                if (injection==Injection::GPUErrors) {
                    data[80]=1;data[96]=round;data[97]=7;data[98]=8;data[99]=9;
                    store(data.data(),48,3);mockCompletion.store(0,std::memory_order_release);return;
                }
                for (uint64_t word=0;word<64;++word)
                    std::atomic_ref<uint64_t>(data[128+word]).store(
                        0xfedcba9876543210ull^(round*0x9e3779b97f4a7c15ull)^(word*0x0101010101010101ull),std::memory_order_relaxed);
                if (injection==Injection::GPUPayload && round==1) data[135]^=1;
                store(data.data(),64,round);store(data.data(),0,round*2);
            }
            if (injection==Injection::Guard) data[1000]^=1;
            if (injection==Injection::KernargGuard) arguments[1000]^=1;
            store(data.data(),48,1);mockCompletion.store(0,std::memory_order_release);
        });
        const bool result=pingPong(rounds,1,data.data(),arguments.data(),&queue,mockDone,0x800000);
        peer.join();
        assert(result==(injection==Injection::None));
        assert(mockWrite==1 && mockCompletion==0);
    }
    std::puts("Ownership protocol host oracle: success, bidirectional errors, data/kernarg guards, skipped sequence, and bounded abort passed without GPU access.");
}
