#include "transport.h"
#include "signal_state.h"
#include <hsa/hsa_ext_amd.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>

static bool gpuPresent = true;
namespace mac_hsa {
struct MockConnection final : Connection {
    hsa_status_t read(DeviceSnapshot &s) override { s.gfxMajor=12; return HSA_STATUS_SUCCESS; }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    if (gpuPresent) connections.push_back(std::make_shared<MockConnection>());
    return HSA_STATUS_SUCCESS;
}
}
static std::vector<hsa_agent_t> agents;
static void enumerate() {
    agents.clear();
    assert(hsa_iterate_agents([](hsa_agent_t a,void *) { agents.push_back(a); return HSA_STATUS_SUCCESS; },
                             nullptr)==HSA_STATUS_SUCCESS);
}
int main(int argc,char **argv) {
    assert(argc==1 || argc==2);
    if (argc==2) assert(mac_hsa::blockedSignalPollNs()==std::strtoull(argv[1],nullptr,10));
    assert(mac_hsa::blockedSignalPollNs(nullptr)==1000000);
    for (const char *bad : {"", "-1", "0", "9", "1001", "64us", " 64", "99999999999999999999"})
        assert(mac_hsa::blockedSignalPollNs(bad)==1000000);
    assert(mac_hsa::blockedSignalPollNs("10")==10000);
    assert(mac_hsa::blockedSignalPollNs("64")==64000);
    assert(mac_hsa::blockedSignalPollNs("1000")==1000000);
    hsa_signal_t signal{};
    assert(hsa_signal_create(0,0,nullptr,&signal)==HSA_STATUS_ERROR_NOT_INITIALIZED);
    assert(hsa_init()==HSA_STATUS_SUCCESS); enumerate();
    assert(agents.size()==2);
    assert(hsa_signal_create(0,1,nullptr,&signal)==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    const hsa_agent_t duplicates[]={agents[0],agents[0]};
    assert(hsa_signal_create(0,2,duplicates,&signal)==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    const hsa_agent_t invalid{UINT64_MAX};
    assert(hsa_signal_create(0,1,&invalid,&signal)==HSA_STATUS_ERROR_INVALID_AGENT);
    assert(hsa_signal_create(0,0,nullptr,&signal)==HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    assert(hsa_signal_create(0,1,&agents[1],&signal)==HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    assert(hsa_signal_create(13,1,&agents[0],&signal)==HSA_STATUS_SUCCESS);
    assert((signal.handle & 63)==0);
    const auto abi=reinterpret_cast<const mac_hsa::SignalABI *>(signal.handle);
    assert(abi->kind==1 && abi->value==13 && !abi->eventMailbox && !abi->queue);
    assert(hsa_signal_load_relaxed(signal)==13);
    assert(hsa_signal_load_scacquire(signal)==13);
#define CHECK_VARIANT(suffix) \
    hsa_signal_store_relaxed(signal,10); \
    hsa_signal_add_##suffix(signal,7); assert(hsa_signal_load_relaxed(signal)==17); \
    hsa_signal_subtract_##suffix(signal,3); assert(hsa_signal_load_relaxed(signal)==14); \
    hsa_signal_and_##suffix(signal,7); assert(hsa_signal_load_relaxed(signal)==6); \
    hsa_signal_or_##suffix(signal,8); assert(hsa_signal_load_relaxed(signal)==14); \
    hsa_signal_xor_##suffix(signal,2); assert(hsa_signal_load_relaxed(signal)==12); \
    assert(hsa_signal_exchange_##suffix(signal,-7)==12); \
    assert(hsa_signal_cas_##suffix(signal,1,3)==-7 && hsa_signal_load_relaxed(signal)==-7); \
    assert(hsa_signal_cas_##suffix(signal,-7,11)==-7 && hsa_signal_load_relaxed(signal)==11);
    CHECK_VARIANT(relaxed)
    CHECK_VARIANT(scacquire)
    CHECK_VARIANT(screlease)
    CHECK_VARIANT(scacq_screl)
#undef CHECK_VARIANT
    hsa_signal_store_relaxed(signal,INT64_MAX);
    hsa_signal_add_relaxed(signal,1);
    assert(hsa_signal_load_relaxed(signal)==INT64_MIN);
    hsa_signal_subtract_relaxed(signal,1);
    assert(hsa_signal_load_relaxed(signal)==INT64_MAX);
    hsa_signal_store_screlease(signal,0);
    std::vector<std::thread> threads;
    for(unsigned i=0;i<4;++i) threads.emplace_back([signal] {
        for(unsigned j=0;j<10000;++j) hsa_signal_add_relaxed(signal,1);
    });
    for(auto &thread:threads) thread.join();
    assert(hsa_signal_load_scacquire(signal)==40000);
    // Acquire wait publishes ordinary CPU data written before release store.
    int payload=0;
    hsa_signal_store_relaxed(signal,1);
    std::thread producer([&] { payload=0x12345678; hsa_signal_store_screlease(signal,0); });
    assert(hsa_signal_wait_scacquire(signal,HSA_SIGNAL_CONDITION_EQ,0,UINT64_MAX,HSA_WAIT_STATE_BLOCKED)==0);
    assert(payload==0x12345678); producer.join();
    hsa_signal_store_relaxed(signal,1);
    std::thread silent([&] { hsa_signal_silent_store_screlease(signal,-3); });
    assert(hsa_signal_wait_scacquire(signal,HSA_SIGNAL_CONDITION_LT,0,UINT64_MAX,HSA_WAIT_STATE_BLOCKED)==-3);
    silent.join();
    assert(hsa_signal_wait_relaxed(signal,HSA_SIGNAL_CONDITION_EQ,7,0,HSA_WAIT_STATE_ACTIVE)==-3);
    hsa_signal_silent_store_relaxed(signal,8);
    assert(hsa_signal_wait_relaxed(signal,HSA_SIGNAL_CONDITION_GTE,8,UINT64_MAX,HSA_WAIT_STATE_ACTIVE)==8);
    assert(hsa_signal_wait_relaxed(signal,HSA_SIGNAL_CONDITION_NE,0,UINT64_MAX,HSA_WAIT_STATE_BLOCKED)==8);
    const auto before=std::chrono::steady_clock::now();
    assert(hsa_signal_wait_relaxed(signal,HSA_SIGNAL_CONDITION_EQ,0,1000000,HSA_WAIT_STATE_BLOCKED)==8);
    assert(std::chrono::steady_clock::now()-before < std::chrono::seconds(2));
    hsa_signal_t other{};
    assert(hsa_amd_signal_create(0,1,&agents[0],4,&other)==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_amd_signal_create(0,1,&agents[0],HSA_AMD_SIGNAL_IPC,&other)==HSA_STATUS_SUCCESS);
    assert(hsa_signal_destroy(other)==HSA_STATUS_SUCCESS);
    assert(hsa_amd_signal_create(0,0,nullptr,HSA_AMD_SIGNAL_AMD_GPU_ONLY,&other)==HSA_STATUS_ERROR_OUT_OF_RESOURCES);
    assert(hsa_amd_signal_create(2,1,&agents[0],HSA_AMD_SIGNAL_AMD_GPU_ONLY,&other)==HSA_STATUS_SUCCESS);
    hsa_signal_t group[]={ {}, signal, other, {UINT64_MAX} };
    hsa_signal_condition_t conds[]={HSA_SIGNAL_CONDITION_EQ,HSA_SIGNAL_CONDITION_EQ,
                                  HSA_SIGNAL_CONDITION_LT,HSA_SIGNAL_CONDITION_EQ};
    hsa_signal_value_t values[]={1,8,1,1}, observed[4]={-1,-1,-1,-1};
    assert(hsa_amd_signal_wait_any(4,group,conds,values,0,HSA_WAIT_STATE_ACTIVE,observed)==1 && observed[0]==8);
    assert(hsa_amd_signal_wait_all(4,group,conds,values,0,HSA_WAIT_STATE_BLOCKED,observed)==UINT32_MAX);
    assert(observed[0]==0 && observed[1]==8 && observed[3]==0);
    std::thread complete([&] { hsa_signal_store_screlease(other,0); });
    assert(hsa_amd_signal_wait_all(4,group,conds,values,UINT64_MAX,HSA_WAIT_STATE_BLOCKED,observed)==0);
    complete.join();
    assert(observed[0]==0 && observed[1]==8 && observed[2]==0 && observed[3]==0);
    // Direct DMA-like stores cannot signal the condition variable. Both the
    // single and multi-signal blocked waits must observe them by polling.
    hsa_signal_store_relaxed(other,1);
    std::thread unnotified([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto *value=&reinterpret_cast<mac_hsa::SignalABI *>(other.handle)->value;
        std::atomic_ref<int64_t>(*value).store(0,std::memory_order_release);
    });
    assert(hsa_amd_signal_wait_any(1,&other,&conds[2],&values[2],1000000000,
        HSA_WAIT_STATE_BLOCKED,observed)==0 && observed[0]==0);
    unnotified.join();
    assert(hsa_signal_destroy(other)==HSA_STATUS_SUCCESS);
    assert(hsa_amd_signal_wait_any(0,nullptr,nullptr,nullptr,UINT64_MAX,HSA_WAIT_STATE_BLOCKED,nullptr)==UINT32_MAX);
    assert(hsa_amd_signal_wait_all(0,nullptr,nullptr,nullptr,UINT64_MAX,HSA_WAIT_STATE_BLOCKED,nullptr)==0);
    assert(hsa_signal_destroy({})==HSA_STATUS_ERROR_INVALID_ARGUMENT);
    assert(hsa_signal_destroy(signal)==HSA_STATUS_SUCCESS);
    assert(hsa_signal_destroy(signal)==HSA_STATUS_ERROR_INVALID_SIGNAL);
    assert(hsa_shut_down()==HSA_STATUS_SUCCESS);
    gpuPresent=false;
    assert(hsa_init()==HSA_STATUS_SUCCESS);
    assert(hsa_signal_create(19,0,nullptr,&signal)==HSA_STATUS_SUCCESS);
    assert(hsa_init()==HSA_STATUS_SUCCESS);
    assert(hsa_shut_down()==HSA_STATUS_SUCCESS);
    assert(hsa_signal_load_relaxed(signal)==19);
    assert(hsa_shut_down()==HSA_STATUS_SUCCESS); // outstanding backing released
    assert(hsa_signal_destroy(signal)==HSA_STATUS_ERROR_NOT_INITIALIZED);
    assert(hsa_init()==HSA_STATUS_SUCCESS);
    assert(hsa_signal_destroy(signal)==HSA_STATUS_ERROR_INVALID_SIGNAL);
    assert(hsa_shut_down()==HSA_STATUS_SUCCESS);
    puts("HSA CPU signals: atomic variants, concurrent updates, publication, waits, ABI and lifetime passed");
}
