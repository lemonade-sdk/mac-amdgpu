#include "mac_hsa.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(hsa_status_t status,const char *step) {if(status) {std::fprintf(stderr,"%s: %#x\n",step,status);throw std::runtime_error(step);}}
void require(bool value,const char *step) {if(!value)throw std::runtime_error(step);}
std::atomic<bool> queueError{false};
}
int main(int argc,char **argv) {
    const char *backend=std::getenv("MAC_HSA_SIGNAL_BACKEND");
    if(argc!=2 || std::strcmp(argv[1],"--run") || (backend && *backend && std::strcmp(backend,"mailbox"))) {
        std::fprintf(stderr,"Usage: %s --run (driver193+ default mailbox, or explicit MAC_HSA_SIGNAL_BACKEND=mailbox on190+)\n",argv[0]);return 2;
    }
    std::setvbuf(stdout,nullptr,_IONBF,0);
    bool initialized=false,passed=false,safe=true;hsa_agent_t gpu{};hsa_signal_t signal{};hsa_queue_t *queues[7]{};
    try {
        check(hsa_init(),"init");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t agent,void *out) {
            hsa_device_type_t type;auto status=hsa_agent_get_info(agent,HSA_AGENT_INFO_DEVICE,&type);
            if(!status && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(out)->handle)*static_cast<hsa_agent_t *>(out)=agent;
            return status;
        },&gpu),"agents");require(gpu.handle,"GPU required");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver");
        require(info.driver_build>=(backend && *backend ? 190u : 193u),"driver build does not qualify selected mailbox mode");
        check(hsa_signal_create(0,1,&gpu,&signal),"signal");
        for(unsigned i=0;i<64;++i)hsa_signal_add_scacq_screl(signal,1);
        require(hsa_signal_load_scacquire(signal)==64,"hot mailbox updates");
        std::puts("PASS 64 hot HSA signal additions");
        std::vector<std::thread> threads;
        try {
            for(unsigned i=0;i<2;++i)threads.emplace_back([signal] {for(unsigned j=0;j<64;++j)hsa_signal_add_scacq_screl(signal,1);});
        } catch(...) {for(auto &thread:threads)if(thread.joinable())thread.join();throw;}
        for(auto &thread:threads)thread.join();
        require(hsa_signal_load_scacquire(signal)==192,"concurrent CPU producers");
        std::puts("PASS two CPU producers, 128 additional GPU-mediated updates, total192");
        for(auto &queue:queues)check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,
            [](hsa_status_t,hsa_queue_t *,void *){queueError=true;},nullptr,0,0,&queue),"all seven public queues");
        hsa_signal_add_scacq_screl(signal,1);
        require(!queueError && hsa_signal_load_scacquire(signal)==193,"reserved one-shot fallback with seven public queues");
        std::puts("PASS seven public queues coexist; signal update succeeds using reserved fallback path");
        for(auto &queue:queues) {check(hsa_queue_destroy(queue),"public queue retirement");queue=nullptr;}
        require(hsa_signal_exchange_scacq_screl(signal,17)==193,"mailbox restart after queue pressure");
        std::puts("Idle150ms: service should retire its internal queue; next operation must remain correct");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(hsa_signal_exchange_scacq_screl(signal,23)==17 && hsa_signal_load_scacquire(signal)==23,"operation after idle lease");
        check(hsa_signal_destroy(signal),"last signal teardown");signal={};
        check(hsa_signal_create(5,1,&gpu,&signal),"new signal context");
        require(hsa_signal_cas_scacq_screl(signal,5,9)==5 && hsa_signal_load_scacquire(signal)==9,"new context CAS");
        check(hsa_signal_destroy(signal),"new context teardown");signal={};
        for(auto &queue:queues)check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,0,0,&queue),"seven slots after final signal teardown");
        for(auto &queue:queues) {check(hsa_queue_destroy(queue),"final slot verification retirement");queue=nullptr;}
        require(!queueError,"queue service fault");passed=true;
    } catch(const std::exception &error) {std::fprintf(stderr,"FAIL: %s\n",error.what());}
    for(auto *queue:queues)if(queue && hsa_queue_destroy(queue)) {safe=false;passed=false;}
    if(safe && signal.handle && hsa_signal_destroy(signal))passed=false;
    if(initialized && hsa_shut_down())passed=false;
    std::puts(passed ? "PASS GPU-mediated HSA signal service: hot updates, CPU contention, seven public queues, pressure/idle recovery, final teardown and new context" : "FAIL signal service lifecycle");
    return passed ? 0 : 1;
}
