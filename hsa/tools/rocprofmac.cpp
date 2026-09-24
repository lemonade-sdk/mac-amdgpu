#include "mac_hsa.h"
#include <hsa/hsa_ext_amd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<bool> finished=false,queueError=false;
void check(hsa_status_t s,const char *what) {if(s) {std::fprintf(stderr,"%s: %#x\n",what,s);throw std::runtime_error(what);}}
void require(bool b,const char *what) {if(!b) throw std::runtime_error(what);}
uint64_t ns() {return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
struct Record {unsigned batch,index;uint64_t hostBegin,hostEnd;mac_hsa_dispatch_timestamps_t gpu{};};
constexpr unsigned count=32,wordsPerSlice=4096;
constexpr uint16_t scope=(HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE)|(HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
}
int main(int argc,char **argv) {
    if(argc!=4 || std::strcmp(argv[1],"--run")) {
        std::printf("rocprofmac: CP dispatch timestamp qualification, no work without --run\nUsage: %s --run vector_add.hsaco trace.json\n",argv[0]);return argc==1?0:2;
    }
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::thread watchdog([] {for(unsigned i=0;i<1200 && !finished;++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));if(!finished) {std::fputs("rocprofmac: hard deadline; retaining unresolved GPU resources\n",stderr);std::_Exit(3);}});
    bool initialized=false,passed=false;
    hsa_agent_t gpu{};hsa_executable_t executable{};hsa_code_object_reader_t reader{};
    hsa_queue_t *queue=nullptr;void *data=nullptr,*args=nullptr;
    std::vector<hsa_signal_t> signals;std::vector<Record> records;
    std::vector<uint32_t> expected(count*wordsPerSlice);
    try {
        std::ifstream input(argv[2],std::ios::binary);std::vector<char> code{std::istreambuf_iterator<char>(input),{}};
        require(!code.empty(),"missing code object");
        check(hsa_init(),"HSA init");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t a,void *out) {hsa_device_type_t type;auto s=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);if(!s && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(out)->handle)*static_cast<hsa_agent_t *>(out)=a;return s;},&gpu),"agents");require(gpu.handle,"no GPU");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"device");
        require(info.gfx_major==12 && info.gfx_minor==0 && info.gfx_revision==1 && info.driver_build>=195,"requires qualified gfx1201 driver195+");
        check(hsa_code_object_reader_create_from_memory(code.data(),code.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load");check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;uint32_t group=0,priv=0,kernarg=0;
        check(hsa_executable_get_symbol_by_name(executable,"vector_add.kd",&gpu,&symbol),"symbol");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"object");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&group),"group");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&priv),"private");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&kernarg),"kernargs");
        require(!group && !priv && kernarg==12,"unexpected fixture ABI");
        check(mac_hsa_memory_allocate_shared(gpu,expected.size()*4,&data),"data allocation");
        check(mac_hsa_memory_allocate_shared(gpu,count*64,&args),"argument allocation");
        signals.reserve(count);records.reserve(count*2);
        // An unprofiled control and two independently profiled queues. Every
        // packet owns a fresh signal; no timed-path signal reset/executor work.
        for(unsigned batch=0;batch<3;++batch) {
            check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_SINGLE,[](hsa_status_t,hsa_queue_t *,void *){queueError=true;},nullptr,0,0,&queue),"queue");
            if(batch) check(hsa_amd_profiling_set_profiler_enabled(queue,1),"enable unused queue profiling");
            for(unsigned i=0;i<count;++i) {hsa_signal_t s{};check(hsa_signal_create(1,1,&gpu,&s),"unique completion");signals.push_back(s);}
            auto *words=static_cast<uint32_t *>(data);
            for(unsigned i=0;i<count;++i) {
                const uint32_t seed=0x61b728u+batch*count+i;
                for(unsigned w=0;w<wordsPerSlice;++w) expected[i*wordsPerSlice+w]=words[i*wordsPerSlice+w]=(w*0x10203041u)^seed;
                for(unsigned w=0;w<128;++w) expected[i*wordsPerSlice+256+w]=words[i*wordsPerSlice+w]+seed;
                uint64_t address=reinterpret_cast<uintptr_t>(words+i*wordsPerSlice);
                auto *a=static_cast<uint8_t *>(args)+i*64;std::memset(a,0,64);std::memcpy(a,&address,8);std::memcpy(a+8,&seed,4);
            }
            const uint64_t batchBegin=ns();
            for(unsigned i=0;i<count;++i) {
                hsa_kernel_dispatch_packet_t p{};p.header=HSA_PACKET_TYPE_KERNEL_DISPATCH|scope;
                p.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;p.workgroup_size_x=32;p.workgroup_size_y=p.workgroup_size_z=1;
                p.grid_size_x=128;p.grid_size_y=p.grid_size_z=1;p.kernel_object=kernel;p.kernarg_address=static_cast<uint8_t *>(args)+i*64;p.completion_signal=signals[i];
                const auto begin=ns();const auto index=hsa_queue_add_write_index_relaxed(queue,1);
                auto *target=static_cast<uint8_t *>(queue->base_address)+(index&(queue->size-1))*64;
                std::memcpy(target+4,reinterpret_cast<const uint8_t *>(&p)+4,60);uint32_t header;std::memcpy(&header,&p,4);
                std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(target)).store(header,std::memory_order_release);
                hsa_signal_store_screlease(queue->doorbell_signal,index);
                require(!queueError,"queue error");if(batch)records.push_back({batch,i,begin,ns(),{}});
            }
            const auto deadline=Clock::now()+std::chrono::seconds(20);
            for(auto s:signals) {
                while(hsa_signal_load_scacquire(s)!=0) {require(!queueError && Clock::now()<deadline,"batch completion deadline");std::this_thread::sleep_for(std::chrono::microseconds(50));}
            }
            require(!queueError,"queue failed");
            require(!std::memcmp(data,expected.data(),expected.size()*4),"output/input/full-slice guard mismatch");
            for(unsigned i=0;i<count;++i) {
                mac_hsa_dispatch_timestamps_t stamp{};
                const auto status=mac_hsa_dispatch_timestamps(queue,signals[i],&stamp,sizeof(stamp));
                if(!batch) require(status==HSA_STATUS_ERROR_INVALID_QUEUE,"control unexpectedly profiled");
                else {check(status,"CP hardware dispatch timestamps absent/invalid");require(stamp.end_ticks>stamp.start_ticks,"zero dispatch duration");records[(batch-1)*count+i].gpu=stamp;}
            }
            require(hsa_amd_profiling_set_profiler_enabled(queue,!batch)!=HSA_STATUS_SUCCESS,"used queue property mutation unexpectedly accepted");
            check(hsa_queue_destroy(queue),"queue retirement");queue=nullptr;
            for(auto s:signals)check(hsa_signal_destroy(s),"signal release");signals.clear();
            std::printf("PASS batch=%u profile=%u dispatches=%u exact-output/full-guards bytes=%zu host_batch_us=%.3f retired\n",batch,bool(batch),count,expected.size()*4,double(ns()-batchBegin)/1000);
        }
        std::ofstream output(argv[3]);require(bool(output),"trace output");output.precision(17);
        output<<"{\"displayTimeUnit\":\"ns\",\"clock_domains_correlated\":false,\"timing_source\":\"CP completion signal start_ts/end_ts\",\"notes\":\"GPU and host lanes have independent zero origins. Dispatch intervals can overlap; gaps are queue envelope gaps, not bandwidth or occupancy. Profiling perturbation is unmeasured.\",\"traceEvents\":[";
        uint64_t hostOrigin=records.front().hostBegin,gpuOrigin=records.front().gpu.start_ticks;bool comma=false;
        for(const auto &r:records) {require(r.gpu.frequency_hz==records.front().gpu.frequency_hz,"GPU clock frequency changed");gpuOrigin=std::min(gpuOrigin,r.gpu.start_ticks);}
        for(const auto &r:records) {
            if(comma)output<<',';comma=true;
            const double factor=1e6/double(r.gpu.frequency_hz);
            output<<"{\"ph\":\"X\",\"name\":\"vector_add\",\"cat\":\"gpu_dispatch\",\"pid\":1,\"tid\":"<<r.batch<<",\"ts\":"<<double(r.gpu.start_ticks-gpuOrigin)*factor<<",\"dur\":"<<double(r.gpu.end_ticks-r.gpu.start_ticks)*factor<<",\"args\":{\"ordinal\":"<<r.index<<",\"start_ticks\":"<<r.gpu.start_ticks<<",\"end_ticks\":"<<r.gpu.end_ticks<<",\"frequency_hz\":"<<r.gpu.frequency_hz<<"}},";
            output<<"{\"ph\":\"X\",\"name\":\"submit vector_add\",\"cat\":\"host_submission_uncorrelated\",\"pid\":2,\"tid\":1,\"ts\":"<<double(r.hostBegin-hostOrigin)/1000<<",\"dur\":"<<double(r.hostEnd-r.hostBegin)/1000<<"}";
        }
        output<<"]}\n";output.close();require(bool(output),"trace write");passed=true;
        std::printf("rocprofmac: %zu GPU dispatch timestamps -> %s; host clock is NOT correlated\n",records.size(),argv[3]);
    } catch(const std::exception &e) {std::fprintf(stderr,"rocprofmac: %s\n",e.what());}
    // Never release signal/BO/code backing until queue removal is acknowledged.
    if(queue && hsa_queue_destroy(queue)!=0) {std::fputs("rocprofmac: queue retirement failed; retaining resources until process exit\n",stderr);std::_Exit(3);}
    for(auto s:signals) if(hsa_signal_destroy(s))passed=false;
    if(args && hsa_memory_free(args))passed=false;if(data && hsa_memory_free(data))passed=false;
    if(executable.handle && hsa_executable_destroy(executable))passed=false;
    if(reader.handle && hsa_code_object_reader_destroy(reader))passed=false;
    if(initialized && hsa_shut_down())passed=false;
    finished=true;watchdog.join();return passed?0:1;
}
