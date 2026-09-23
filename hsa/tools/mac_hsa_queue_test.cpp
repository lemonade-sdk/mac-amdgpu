#include "mac_hsa.h"
#include <hsa/hsa_ext_amd.h>
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
std::atomic<unsigned> errors{0};
void check(hsa_status_t status,const char *step) {
    if (!status) return;
    std::fprintf(stderr,"%s: HSA status %#x\n",step,status);
    throw std::runtime_error(step);
}
void require(bool good,const char *step) {if (!good) throw std::runtime_error(step);}
constexpr uint16_t scopes=(HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
    (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
void submit(hsa_queue_t *queue,const void *packet) {
    const auto index=hsa_queue_add_write_index_relaxed(queue,1);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (index-hsa_queue_load_read_index_scacquire(queue)>=queue->size) {
        require(std::chrono::steady_clock::now()<deadline && !errors,"queue capacity timeout");
        std::this_thread::yield();
    }
    auto *target=static_cast<uint8_t *>(queue->base_address)+(index&(queue->size-1))*64;
    // Publish the header/setup together only after the remaining payload.
    std::memcpy(target+4,static_cast<const uint8_t *>(packet)+4,60);
    uint32_t header;std::memcpy(&header,packet,4);
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(target)).store(header,std::memory_order_release);
    hsa_signal_store_screlease(queue->doorbell_signal,static_cast<int64_t>(index));
    require(!errors,"queue doorbell error");
}
void wait(hsa_signal_t signal,const char *step) {
    require(hsa_signal_wait_scacquire(signal,HSA_SIGNAL_CONDITION_EQ,0,2000000000ull,HSA_WAIT_STATE_BLOCKED)==0,step);
    require(!errors,"queue error callback");
}
void drain(hsa_queue_t *queue) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (hsa_queue_load_read_index_scacquire(queue)!=hsa_queue_load_write_index_scacquire(queue)) {
        require(std::chrono::steady_clock::now()<deadline,"queue read-index timeout");
        std::this_thread::yield();
    }
}
}
int main(int argc,char **argv) {
    if (argc!=3 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run build/tests/hsa-code-object.hsaco\n",argv[0]);return 2;
    }
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
    if (bytes.empty()) return 1;
    bool initialized=false,passed=false,safe=true;
    hsa_agent_t gpu{};hsa_code_object_reader_t reader{};hsa_executable_t executable{};
    hsa_queue_t *queues[2]{};hsa_signal_t done{},dependency{};void *data=nullptr,*arguments=nullptr;
    try {
        check(hsa_init(),"initialize runtime");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t a,void *p) {
            hsa_device_type_t type;const auto status=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
            if (!status && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(p)->handle) *static_cast<hsa_agent_t *>(p)=a;
            return status;
        },&gpu),"enumerate");require(gpu.handle,"no GPU");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver build");
        require(info.driver_build>=185,"Install driver 185 before running persistent queues");
        check(hsa_code_object_reader_create_from_memory(bytes.data(),bytes.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load kernel");
        check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;
        check(hsa_executable_get_symbol_by_name(executable,"vector_add.kd",&gpu,&symbol),"symbol");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"kernel descriptor");
        check(mac_hsa_memory_allocate_shared(gpu,16384,&data),"data");
        check(mac_hsa_memory_allocate_shared(gpu,16384,&arguments),"kernargs");
        check(hsa_signal_create(1,0,nullptr,&done),"completion signal");
        check(hsa_signal_create(1,0,nullptr,&dependency),"dependency signal");
        for (auto &queue:queues) check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,
            [](hsa_status_t status,hsa_queue_t *,void *) {++errors;std::fprintf(stderr,"Queue error: %#x\n",status);},
            nullptr,0,0,&queue),"persistent queue");
        std::printf("Two hardware queues created; signal=%#llx data=%p kernargs=%p\n",(unsigned long long)done.handle,data,arguments);
        hsa_kernel_dispatch_packet_t packet{};
        packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH|scopes;packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
        packet.workgroup_size_x=32;packet.workgroup_size_y=packet.workgroup_size_z=1;
        packet.grid_size_x=128;packet.grid_size_y=packet.grid_size_z=1;
        packet.kernel_object=kernel;packet.kernarg_address=arguments;packet.completion_signal=done;
        auto *words=static_cast<uint32_t *>(data);uint32_t expected[4096];
        const auto prepare=[&](uint32_t seed) {
            for (unsigned i=0;i<4096;++i) words[i]=i*0x10203041u^seed;
            std::memcpy(expected,words,sizeof(expected));
            for (unsigned i=0;i<128;++i) expected[256+i]=words[i]+seed;
            const uint64_t address=reinterpret_cast<uintptr_t>(data);
            std::memcpy(arguments,&address,8);std::memcpy(static_cast<uint8_t *>(arguments)+8,&seed,4);
        };
        for (unsigned iteration=0;iteration<192;++iteration) {
            prepare(0x6ba18937u^iteration);
            if (iteration) hsa_signal_store_screlease(done,1);
            submit(queues[0],&packet);wait(done,"kernel completion timeout");drain(queues[0]);
            require(!std::memcmp(data,expected,sizeof(expected)),"kernel output or guards differ");
            if (iteration==0 || (iteration+1)%64==0) std::printf("PASS: %u dispatches; read/write=%llu; 16384 bytes verified\n",
                iteration+1,(unsigned long long)hsa_queue_load_read_index_scacquire(queues[0]));
        }
        // Queue 0 waits on a shared signal. Queue 1's CP completion releases it.
        hsa_barrier_and_packet_t barrier{};barrier.header=HSA_PACKET_TYPE_BARRIER_AND|scopes;
        barrier.dep_signal[0]=dependency;barrier.completion_signal=done;
        hsa_signal_store_screlease(done,1);submit(queues[0],&barrier);
        require(hsa_signal_wait_scacquire(done,HSA_SIGNAL_CONDITION_EQ,0,10000000,HSA_WAIT_STATE_BLOCKED)==1,"barrier ignored dependency");
        prepare(0x7ab129u);packet.completion_signal=dependency;submit(queues[1],&packet);
        wait(done,"cross-queue barrier timeout");wait(dependency,"producer completion timeout");drain(queues[0]);drain(queues[1]);
        require(!std::memcmp(data,expected,sizeof(expected)),"cross-queue shader result");
        std::puts("PASS: queue-to-queue shared completion dependency");
        // The HSA CPU store executes on reserved queue 0 while the persistent
        // queue waits, proving the dependency can be released without deadlock.
        hsa_signal_store_screlease(dependency,1);hsa_signal_store_screlease(done,1);
        submit(queues[0],&barrier);
        require(hsa_signal_wait_scacquire(done,HSA_SIGNAL_CONDITION_EQ,0,10000000,HSA_WAIT_STATE_BLOCKED)==1,"CPU dependency not pending");
        hsa_signal_store_screlease(dependency,0);wait(done,"CPU-to-GPU signal timeout");drain(queues[0]);
        std::puts("PASS: CPU HSA signal store released a waiting hardware queue");passed=true;
    } catch (const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());}
    // Never release payload/code/signal backing while an unmap is unconfirmed.
    for (auto *queue:queues) if (queue && hsa_queue_destroy(queue)) {safe=false;passed=false;}
    if (safe) {
        if (done.handle && hsa_signal_destroy(done)) passed=false;
        if (dependency.handle && hsa_signal_destroy(dependency)) passed=false;
        if (arguments && hsa_memory_free(arguments)) passed=false;
        if (data && hsa_memory_free(data)) passed=false;
        if (executable.handle && hsa_executable_destroy(executable)) passed=false;
    } else std::fputs("Queue unmap failed: backing retained until session reset\n",stderr);
    if (reader.handle && hsa_code_object_reader_destroy(reader)) passed=false;
    if (initialized && hsa_shut_down()) passed=false;
    std::puts(passed ? "PASS: persistent AQL wraparound, shared signals, dependencies and teardown" : "FAIL: persistent queue test");
    return passed ? 0 : 1;
}
