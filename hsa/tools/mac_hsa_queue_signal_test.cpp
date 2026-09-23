#include "mac_hsa.h"
#include <hsa/amd_hsa_signal.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(hsa_status_t status,const char *name) {
    if (status) {std::fprintf(stderr,"%s: %#x\n",name,status);throw std::runtime_error(name);}
}
void require(bool value,const char *name) {if (!value) throw std::runtime_error(name);}
std::atomic<bool> queueError{false};
void publish(hsa_queue_t *q,const void *packet) {
    const auto index=hsa_queue_add_write_index_relaxed(q,1);
    require(index<q->size,"test queue capacity");
    auto *target=static_cast<uint8_t *>(q->base_address)+index*64;
    std::memcpy(target+4,static_cast<const uint8_t *>(packet)+4,60);
    uint32_t header;std::memcpy(&header,packet,4);
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(target)).store(header,std::memory_order_release);
    hsa_signal_store_screlease(q->doorbell_signal,int64_t(index));
    require(!queueError,"doorbell failed");
}
}
int main(int argc,char **argv) {
    if (argc!=3 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run build/tests/hsa-signal-object.hsaco\n",argv[0]);return 2;
    }
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
    if (bytes.empty()) return 1;
    bool initialized=false,passed=false,safe=true;
    hsa_agent_t gpu{};hsa_code_object_reader_t reader{};hsa_executable_t executable{};
    hsa_queue_t *queues[2]{};hsa_signal_t value{},start{},cpCompletion{},done[2]{};void *args=nullptr;
    try {
        check(hsa_init(),"runtime");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t a,void *p) {
            hsa_device_type_t type;auto status=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
            if (!status && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(p)->handle) *static_cast<hsa_agent_t *>(p)=a;
            return status;
        },&gpu),"agents");require(gpu.handle,"no GPU");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver");
        require(info.driver_build>=185,"Install driver 185 first");
        check(hsa_code_object_reader_create_from_memory(bytes.data(),bytes.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load");
        check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel;
        check(hsa_executable_get_symbol_by_name(executable,"signal_accumulate.kd",&gpu,&symbol),"symbol");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"kernel");
        check(mac_hsa_memory_allocate_shared(gpu,16384,&args),"kernargs");
        check(hsa_signal_create(0,0,nullptr,&value),"value signal");
        check(hsa_signal_create(1,0,nullptr,&start),"start signal");
        check(hsa_signal_create(2,0,nullptr,&cpCompletion),"shared CP completion");
        for (unsigned i=0;i<2;++i) {
            check(hsa_signal_create(1,0,nullptr,&done[i]),"completion");
            check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t,hsa_queue_t *,void *) {queueError=true;},
                nullptr,0,0,&queues[i]),"queue");
        }
        const uint64_t address=value.handle+offsetof(amd_signal_t,value);
        constexpr uint32_t iterations=2048,cpuAdds=64;
        std::memcpy(args,&address,8);std::memcpy(static_cast<uint8_t *>(args)+8,&iterations,4);
        constexpr uint16_t scope=(HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
            (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
        hsa_barrier_and_packet_t barrier{};barrier.header=HSA_PACKET_TYPE_BARRIER_AND|scope;barrier.dep_signal[0]=start;
        hsa_kernel_dispatch_packet_t packet{};
        packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH|scope|(1<<HSA_PACKET_HEADER_BARRIER);
        packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
        packet.workgroup_size_x=packet.grid_size_x=32;packet.workgroup_size_y=packet.workgroup_size_z=packet.grid_size_y=packet.grid_size_z=1;
        packet.kernel_object=kernel;packet.kernarg_address=args;
        for (unsigned i=0;i<2;++i) {publish(queues[i],&barrier);packet.completion_signal=done[i];publish(queues[i],&packet);}
        require(hsa_signal_load_scacquire(value)==0,"GPU ignored the start barrier");
        std::printf("Two queues waiting; shared signal=%#llx; releasing both\n",(unsigned long long)value.handle);
        hsa_signal_store_screlease(start,0);
        unsigned overlapped=0;
        for (unsigned i=0;i<cpuAdds;++i) {
            if (hsa_signal_load_scacquire(done[0]) || hsa_signal_load_scacquire(done[1])) ++overlapped;
            hsa_signal_add_scacq_screl(value,1);
        }
        for (unsigned i=0;i<2;++i) {
            require(hsa_signal_wait_scacquire(done[i],HSA_SIGNAL_CONDITION_EQ,0,2000000000ull,HSA_WAIT_STATE_BLOCKED)==0,"GPU completion timeout");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (hsa_queue_load_read_index_scacquire(queues[i])!=2) {
                require(std::chrono::steady_clock::now()<deadline,"read index timeout");std::this_thread::yield();
            }
        }
        const auto observed=hsa_signal_load_scacquire(value);
        constexpr int64_t expected=2*32*iterations+cpuAdds;
        std::printf("Signal observed=%lld expected=%lld; CPU calls issued before GPU completion=%u/%u\n",
            (long long)observed,(long long)expected,overlapped,cpuAdds);
        require(observed==expected && !queueError,"lost signal updates");
        require(overlapped>0,"no overlapping CPU/GPU signal operations observed");
        require(hsa_signal_load_scacquire(start)==0,"adjacent signal corrupted");
        // Exercise CP's completion decrement in the same value as shader-backed
        // CPU HSA updates, rather than assuming all GPU atomic clients agree.
        hsa_signal_store_screlease(value,0);hsa_signal_store_screlease(start,1);
        packet.completion_signal=cpCompletion;
        for (auto *q:queues) {publish(q,&barrier);publish(q,&packet);}
        hsa_signal_store_screlease(start,0);
        unsigned cpOverlap=0;
        for (unsigned i=0;i<cpuAdds;++i) {
            if (hsa_signal_load_scacquire(cpCompletion)>int64_t(i)) ++cpOverlap;
            hsa_signal_add_scacq_screl(cpCompletion,1);
        }
        require(hsa_signal_wait_scacquire(cpCompletion,HSA_SIGNAL_CONDITION_EQ,cpuAdds,2000000000ull,HSA_WAIT_STATE_BLOCKED)==cpuAdds,
            "CP completion and GPU-backed CPU signal updates lost operations");
        for (auto *q:queues) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (hsa_queue_load_read_index_scacquire(q)!=4) {
                require(std::chrono::steady_clock::now()<deadline,"CP completion read index timeout");std::this_thread::yield();
            }
        }
        require(hsa_signal_load_scacquire(value)==2*32*iterations,"second-phase shader increments");
        require(cpOverlap>0 && !queueError,"no concurrent CP completion updates observed");
        std::printf("PASS: two CP decrements plus %u CPU HSA additions, result=%u; overlapping calls=%u\n",cpuAdds,cpuAdds,cpOverlap);
        passed=true;
    } catch (const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());}
    for (auto *q:queues) if (q && hsa_queue_destroy(q)) {safe=false;passed=false;}
    if (safe) {
        for (const auto s:{value,start,cpCompletion,done[0],done[1]}) if (s.handle && hsa_signal_destroy(s)) passed=false;
        if (args && hsa_memory_free(args)) passed=false;
        if (executable.handle && hsa_executable_destroy(executable)) passed=false;
    } else std::fputs("Queue removal unconfirmed; backing retained for session reset\n",stderr);
    if (reader.handle && hsa_code_object_reader_destroy(reader)) passed=false;
    if (initialized && hsa_shut_down()) passed=false;
    std::puts(passed ? "PASS: shared signal updates from two GPU queues and CPU HSA API" : "FAIL: concurrent queue signal test");
    return passed ? 0 : 1;
}
