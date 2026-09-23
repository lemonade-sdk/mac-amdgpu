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
using Clock=std::chrono::steady_clock;
constexpr size_t bytes=16384, words=bytes/8;
constexpr size_t counter=0, lockWord=16, payload=32, inverse=33,
    cpuInside=48, gpuInside=64, gpuErrors=96, ready=112, go=128,
    cpuProgress=144, gpuProgress=160, abortWord=176, state=192,
    gpuAttempts=208, gpuOverlap=224,cpuOldSum=240,gpuOldSum=256,handoffResult=272,
    gpuStarted=288,gpuFinished=304;
constexpr uint64_t magic=0x6d3f0a519c27e8b4ull, guard=0xa739d2815e0bf46cull;
std::atomic<unsigned> queueErrors{0};
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);
void check(hsa_status_t status,const char *what) {
    if (status) {std::fprintf(stderr,"%s: HSA status %#x\n",what,status);throw std::runtime_error(what);}
}
void require(bool condition,const char *what) {if (!condition) throw std::runtime_error(what);}
uint64_t load(uint64_t *data,size_t index) {return std::atomic_ref<uint64_t>(data[index]).load(std::memory_order_acquire);}
void store(uint64_t *data,size_t index,uint64_t value) {std::atomic_ref<uint64_t>(data[index]).store(value,std::memory_order_release);}
bool mutableWord(size_t index) {
    for (auto field:{counter,lockWord,payload,inverse,cpuInside,gpuInside,gpuErrors,ready,go,
                    cpuProgress,gpuProgress,abortWord,state,gpuAttempts,gpuOverlap,cpuOldSum,gpuOldSum,
                    handoffResult,handoffResult+1,handoffResult+2,handoffResult+3,gpuStarted,gpuFinished})
        if (index==field) return true;
    return false;
}
void publish(hsa_queue_t *queue,const hsa_kernel_dispatch_packet_t &packet) {
    const auto index=hsa_queue_add_write_index_relaxed(queue,1);
    require(index-hsa_queue_load_read_index_scacquire(queue)<queue->size,"queue unexpectedly full");
    auto *slot=static_cast<uint8_t *>(queue->base_address)+(index&(queue->size-1))*64;
    std::memcpy(slot+4,reinterpret_cast<const uint8_t *>(&packet)+4,60);
    uint32_t header;std::memcpy(&header,&packet,4);
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(slot)).store(header,std::memory_order_release);
    hsa_signal_store_screlease(queue->doorbell_signal,static_cast<int64_t>(index));
    require(!queueErrors,"doorbell failed");
}
struct TrialResult {bool completed=false,correct=false,overlapped=false;};
enum class Participants {CPUOnly,GPUOnly,Mixed};
TrialResult trial(Participants participants,unsigned ordinal,unsigned mode,uint64_t cpuIterations,uint64_t gpuIterations,
                  bool stagger,unsigned timeoutSeconds,
                  uint64_t *data,void *arguments,hsa_queue_t *queue,hsa_signal_t done,
                  uint64_t kernel,uint32_t privateSize,uint32_t groupSize) {
    const bool useCPU=participants!=Participants::GPUOnly,useGPU=participants!=Participants::CPUOnly;
    const bool mixed=useCPU && useGPU;
    const bool lockMode=mode==1;
    const char *operation=lockMode ? "CAS-lock" : mode==2 ? "fetch-add-return" : "fetch-add";
    const char *label=mixed ? "mixed" : useCPU ? "CPU-only" : "GPU-only";
    stagger=stagger && mixed;
    const uint64_t expectedCount=(useCPU ? cpuIterations : 0)+(useGPU ? gpuIterations : 0);
    const uint64_t prefix=cpuIterations/4,tailStart=cpuIterations-prefix,middle=tailStart-prefix;
    std::fill(data,data+words,guard);
    for (size_t i=0;i<words;++i) if (mutableWord(i)) data[i]=0;
    data[inverse]=magic;
    std::memset(arguments,0,bytes);
    const uint64_t address=reinterpret_cast<uintptr_t>(data);
    const uint64_t maxAttempts=100000000;
    std::memcpy(arguments,&address,8);std::memcpy(static_cast<char *>(arguments)+8,&gpuIterations,8);
    std::memcpy(static_cast<char *>(arguments)+16,&maxAttempts,8);
    std::memcpy(static_cast<char *>(arguments)+24,&mode,4);
    std::memcpy(static_cast<char *>(arguments)+32,&cpuIterations,8);
    const uint64_t overlapFloor=stagger ? prefix : 0;
    std::memcpy(static_cast<char *>(arguments)+40,&overlapFloor,8);
    if (useGPU) hsa_signal_store_screlease(done,1);
    hsa_kernel_dispatch_packet_t packet{};
    packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet.workgroup_size_x=32;packet.workgroup_size_y=packet.workgroup_size_z=1;
    packet.grid_size_x=32;packet.grid_size_y=packet.grid_size_z=1;
    packet.kernel_object=kernel;packet.kernarg_address=arguments;
    packet.private_segment_size=privateSize;packet.group_segment_size=groupSize;packet.completion_signal=done;
    const auto start=Clock::now(),deadline=start+std::chrono::seconds(timeoutSeconds);
    std::atomic<bool> cancel{false},cpuDone{false};
    uint64_t cpuCompleted=0,cpuErrors=0,cpuTries=0,cpuOverlap=0,cpuSum=0;
    bool cpuStarted=false;
    uint64_t prefixCounter=0,preTailCounter=0;bool prefixSampled=false,preTailSampled=false;
    Clock::time_point cpuBegin{},cpuEnd{},gpuBeginObserved{},gpuEndObserved{};
    auto seconds=[&](Clock::time_point value) {return value==Clock::time_point{} ? -1.0 : std::chrono::duration<double>(value-start).count();};
    if (useGPU) publish(queue,packet);
    std::thread cpu([&] {
        while (useGPU && !load(data,ready)) {
            if (cancel || Clock::now()>=deadline) {cpuDone=true;return;}
            std::this_thread::yield();
        }
        cpuStarted=true;
        if (useGPU && !stagger) store(data,go,1);
        // The GPU-only control uses the identical shader/handshake but never
        // modifies its counter or lock from the CPU.
        if (!useCPU) {cpuDone=true;return;}
        // Require the GPU to begin before the CPU starts its bounded work.
        while (mixed && !stagger && !load(data,gpuProgress)) {
            if (cancel || load(data,state) || Clock::now()>=deadline) {cpuDone=true;return;}
            std::this_thread::yield();
        }
        std::atomic_ref<uint64_t> count(data[counter]),mutex(data[lockWord]);
        cpuBegin=Clock::now();
        for (uint64_t i=0;i<cpuIterations;++i) {
            if (stagger && i==prefix) {
                store(data,cpuProgress,cpuCompleted);prefixCounter=load(data,counter);prefixSampled=true;
                std::printf("phase=CPU25 CPU=%llu GPU=%llu counter=%llu expected=%llu host_elapsed=%.6f\n",
                    (unsigned long long)cpuCompleted,(unsigned long long)load(data,gpuProgress),
                    (unsigned long long)prefixCounter,(unsigned long long)prefix,seconds(Clock::now()));
                store(data,go,1);
            }
            if (stagger && i>=prefix) {
                const uint64_t target=i>=tailStart ? gpuIterations : ((i-prefix)*gpuIterations/middle)+1;
                while (load(data,gpuProgress)<target || (i>=tailStart && !load(data,gpuFinished))) {
                    if (cancel || Clock::now()>=deadline || load(data,state)==2) goto finished;
                    std::this_thread::yield();
                }
                if (i==tailStart) {
                    store(data,cpuProgress,cpuCompleted);preTailCounter=load(data,counter);preTailSampled=true;
                    std::printf("phase=before-CPU-tail CPU=%llu GPU=%llu counter=%llu expected=%llu host_elapsed=%.6f\n",
                        (unsigned long long)cpuCompleted,(unsigned long long)load(data,gpuProgress),
                        (unsigned long long)preTailCounter,(unsigned long long)(tailStart+gpuIterations),seconds(Clock::now()));
                }
            }
            if (!(i&255) && (cancel || Clock::now()>=deadline)) break;
            if (!lockMode) {
                const auto old=count.fetch_add(1,std::memory_order_seq_cst);
                if (mode==2) cpuSum+=old;
            }
            else {
                uint64_t expected=0;
                while (!mutex.compare_exchange_weak(expected,1,std::memory_order_acq_rel,std::memory_order_acquire)) {
                    expected=0;
                    if (++cpuTries>=maxAttempts || cancel || Clock::now()>=deadline) goto finished;
                }
                std::atomic_ref<uint64_t>(data[cpuInside]).store(1,std::memory_order_seq_cst);
                if (std::atomic_ref<uint64_t>(data[gpuInside]).load(std::memory_order_seq_cst)) ++cpuErrors;
                const auto previous=load(data,payload);
                if (load(data,inverse)!=(previous^magic)) ++cpuErrors;
                store(data,payload,previous+1);store(data,inverse,(previous+1)^magic);
                if (std::atomic_ref<uint64_t>(data[gpuInside]).load(std::memory_order_seq_cst)) ++cpuErrors;
                std::atomic_ref<uint64_t>(data[cpuInside]).store(0,std::memory_order_seq_cst);
                expected=1;
                if (!mutex.compare_exchange_strong(expected,0,std::memory_order_release,std::memory_order_relaxed)) {
                    ++cpuErrors;goto finished;
                }
            }
            cpuCompleted=i+1;
            if (!(i&255)) {
                const auto gpu=load(data,gpuProgress);
                if (gpu && gpu<gpuIterations) ++cpuOverlap;
                store(data,cpuProgress,cpuCompleted);
            }
        }
finished:
        cpuEnd=Clock::now();
        store(data,cpuOldSum,cpuSum);
        store(data,cpuProgress,cpuCompleted);cpuDone.store(true,std::memory_order_release);
    });
    auto nextReport=start+std::chrono::seconds(1);
    bool timedOut=false;
    while (!cpuDone.load(std::memory_order_acquire) || (useGPU && hsa_signal_load_scacquire(done)!=0)) {
        const auto now=Clock::now();
        if (useGPU && gpuBeginObserved==Clock::time_point{} && load(data,gpuStarted)) gpuBeginObserved=now;
        if (useGPU && gpuEndObserved==Clock::time_point{} && load(data,gpuFinished)) gpuEndObserved=now;
        if (now>=nextReport) {
            std::printf("participants=%s trial=%u mode=%s CPU=%llu GPU=%llu state=%llu elapsed=%.1fs\n",label,ordinal,operation,
                (unsigned long long)load(data,cpuProgress),(unsigned long long)load(data,gpuProgress),
                (unsigned long long)load(data,state),std::chrono::duration<double>(now-start).count());
            nextReport=now+std::chrono::seconds(1);
        }
        if (now>=deadline || queueErrors) {timedOut=true;cancel=true;store(data,abortWord,1);break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancel=true;cpu.join();
    // Let a responsive GPU observe cancellation before any queue teardown.
    if (timedOut && useGPU) {
        const auto grace=Clock::now()+std::chrono::seconds(3);
        while (hsa_signal_load_scacquire(done)!=0 && Clock::now()<grace && !queueErrors)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool retired=!useGPU || hsa_signal_load_scacquire(done)==0;
    const auto gpuCompleted=load(data,gpuProgress),gpuErrorCount=load(data,gpuErrors);
    const auto observed=load(data,lockMode ? payload : counter);
    bool guards=true;
    if (retired) for (size_t i=0;i<words;++i) if (!mutableWord(i) && data[i]!=guard) guards=false;
    const bool completed=!timedOut && retired && cpuStarted && cpuCompleted==(useCPU ? cpuIterations : 0) &&
        gpuCompleted==(useGPU ? gpuIterations : 0) && load(data,state)==(useGPU ? 1u : 0u) && !queueErrors;
    const bool overlap=!mixed || (cpuOverlap && load(data,gpuOverlap));
    const uint64_t sum=cpuSum+load(data,gpuOldSum),expectedSum=expectedCount*(expectedCount-1)/2;
    const bool phasesCorrect=!stagger || (prefixSampled && preTailSampled && prefixCounter==prefix &&
        preTailCounter==tailStart+gpuIterations && observed-preTailCounter==cpuIterations-tailStart);
    const bool correct=completed && phasesCorrect && (mode!=2 || sum==expectedSum) && observed==expectedCount && guards && !cpuErrors && !gpuErrorCount &&
        (!lockMode || (!load(data,lockWord) && !load(data,cpuInside) && !load(data,gpuInside) &&
                   load(data,inverse)==(observed^magic)));
    std::printf("%s participants=%s trial=%u %s expected=%llu observed=%llu CPU=%llu GPU=%llu overlap_batches=%llu/%llu overlap_required=%s "
        "lock_errors=%llu/%llu CAS_retries=%llu/%llu guards=%s completion=%s elapsed=%.3fs\n",
        !completed ? "INCOMPLETE" : !correct ? "MISMATCH" : !overlap ? "INCONCLUSIVE" : "PASS",label,ordinal,
        operation,(unsigned long long)expectedCount,(unsigned long long)observed,
        (unsigned long long)cpuCompleted,(unsigned long long)gpuCompleted,(unsigned long long)cpuOverlap,
        (unsigned long long)load(data,gpuOverlap),mixed ? "yes" : "no",(unsigned long long)cpuErrors,(unsigned long long)gpuErrorCount,
        (unsigned long long)cpuTries,(unsigned long long)load(data,gpuAttempts),guards ? "intact" : "changed",
        !useGPU ? "no-GPU-dispatch" : retired ? "retired" : "pending",std::chrono::duration<double>(Clock::now()-start).count());
    const auto sampled=Clock::now();
    if (useGPU && gpuBeginObserved==Clock::time_point{} && load(data,gpuStarted)) gpuBeginObserved=sampled;
    if (useGPU && gpuEndObserved==Clock::time_point{} && load(data,gpuFinished)) gpuEndObserved=sampled;
    std::printf("timing host-steady seconds since dispatch: CPU-start=%.6f CPU-end=%.6f GPU-start-observed=%.6f GPU-end-observed=%.6f (GPU markers sampled by host; observation lag, no GPU clock timestamp)\n",
        seconds(cpuBegin),seconds(cpuEnd),seconds(gpuBeginObserved),seconds(gpuEndObserved));
    if (stagger && prefixSampled && preTailSampled) std::printf("phase-accounting prefix=%llu/%llu middle-phase-counter-delta=%llu/%llu tail-counter-delta=%llu/%llu final=%llu/%llu (measured phases; exact concurrent RMW count is unavailable)\n",
        (unsigned long long)prefixCounter,(unsigned long long)prefix,
        (unsigned long long)(preTailCounter-prefixCounter),(unsigned long long)(middle+gpuIterations),
        (unsigned long long)(observed-preTailCounter),(unsigned long long)(cpuIterations-tailStart),
        (unsigned long long)observed,(unsigned long long)expectedCount);
    else if (stagger) std::puts("phase-accounting incomplete: no conclusion about where updates were lost");
    if (mode==2) std::printf("returned-old sum CPU=%llu GPU=%llu total=%llu expected=%llu\n",
        (unsigned long long)cpuSum,(unsigned long long)load(data,gpuOldSum),
        (unsigned long long)sum,(unsigned long long)expectedSum);
    return {completed,correct,overlap};
}

bool handoff(uint64_t rounds,unsigned timeoutSeconds,uint64_t *data,void *arguments,
             hsa_queue_t *queue,hsa_signal_t done,uint64_t kernel) {
    std::fill(data,data+words,guard);
    for (size_t i=0;i<words;++i) if (mutableWord(i)) data[i]=0;
    std::memset(arguments,0,bytes);
    const uint64_t address=reinterpret_cast<uintptr_t>(data),attempts=100000000;
    std::memcpy(arguments,&address,8);std::memcpy(static_cast<char *>(arguments)+8,&rounds,8);
    std::memcpy(static_cast<char *>(arguments)+16,&attempts,8);
    hsa_signal_store_screlease(done,1);
    hsa_kernel_dispatch_packet_t packet{};
    packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet.workgroup_size_x=packet.grid_size_x=32;
    packet.workgroup_size_y=packet.workgroup_size_z=packet.grid_size_y=packet.grid_size_z=1;
    packet.kernel_object=kernel;packet.kernarg_address=arguments;packet.completion_signal=done;
    const auto deadline=Clock::now()+std::chrono::seconds(timeoutSeconds);
    publish(queue,packet);
    bool failed=false;
    auto expect=[&](uint64_t actual,uint64_t expected,const char *operation,uint64_t round) {
        if (actual==expected) return;
        std::fprintf(stderr,"handoff round=%llu %s expected=%#llx observed=%#llx\n",
            (unsigned long long)round,operation,(unsigned long long)expected,(unsigned long long)actual);
        failed=true;
    };
    auto waitFor=[&](size_t field,uint64_t wanted) {
        while (load(data,field)!=wanted) {
            if (Clock::now()>=deadline || queueErrors || load(data,state)==2) {
                failed=true;return false;
            }
            std::this_thread::yield();
        }
        return true;
    };
    uint64_t completed=0;
    std::atomic_ref<uint64_t> word(data[counter]);
    if (waitFor(ready,1)) for (uint64_t round=0;round<rounds && !failed;++round) {
        const uint64_t base=0x1234567800000000ull+round*64;
        expect(word.exchange(base,std::memory_order_seq_cst),round ? base-24 : 0,"CPU exchange old",round);
        for (uint64_t phase=1;phase<=4 && !failed;++phase) {
            if (phase==2) expect(word.fetch_add(7,std::memory_order_seq_cst),base+5,"CPU add old",round);
            if (phase==3 || phase==4) {
                uint64_t expected=base+(phase==3 ? 20 : 41);
                const bool success=word.compare_exchange_strong(expected,phase==3 ? base+30 : 0xdeadbeefull,
                    std::memory_order_acq_rel,std::memory_order_acquire);
                expect(success,phase==3,"CPU CAS result",round);
                expect(expected,base+(phase==3 ? 20 : 40),"CPU CAS expected update",round);
            }
            if (failed) break;
            const uint64_t turn=round*4+phase;
            store(data,go,turn);
            if (!waitFor(gpuProgress,turn)) break;
            const uint64_t oldOffsets[]={0,12,30,40},newOffsets[]={5,20,40,40};
            expect(load(data,handoffResult+phase-1),base+oldOffsets[phase-1],"GPU returned old/expected",round);
            expect(word.load(std::memory_order_seq_cst),base+newOffsets[phase-1],"CPU observes GPU result",round);
            expect(load(data,gpuErrors),0,"GPU operation errors",round);
        }
        if (!failed) completed=round+1;
    }
    if (failed) store(data,abortWord,1);
    const auto grace=Clock::now()+std::chrono::seconds(3);
    while (hsa_signal_load_scacquire(done)!=0 && Clock::now()<(failed ? grace : deadline) && !queueErrors)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool retired=hsa_signal_load_scacquire(done)==0;
    if (!retired) store(data,abortWord,1);
    if (retired) for (size_t i=0;i<words;++i) if (!mutableWord(i) && data[i]!=guard) failed=true;
    const bool passed=!failed && retired && completed==rounds && load(data,state)==1 && !queueErrors;
    std::printf("%s: serialized CPU/GPU handoff rounds=%llu/%llu completion=%s; add/exchange old values, CAS success/failure expected updates. This does not test concurrent RMW.\n",
        passed ? "PASS" : "FAIL/INCOMPLETE",(unsigned long long)completed,(unsigned long long)rounds,
        retired ? "retired" : "pending");
    return passed;
}
}

int main(int argc,char **argv) {
    if (argc<3 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run shader.hsaco [--return-add] [--handoff] [--add-only] [--stagger] [--trials N] [--iterations N | --cpu-iterations N --gpu-iterations N] [--lock-iterations N] [--timeout-seconds N]\n",argv[0]);return 2;
    }
    uint64_t cpuIncrements=10000000,gpuIncrements=10000000,lockIterations=1000000;unsigned trials=3,timeout=60;
    bool returnAdd=false,handoffOnly=false,addOnly=false,stagger=false;
    try {
        for (int i=3;i<argc;) {
            if (!std::strcmp(argv[i],"--return-add")) {returnAdd=true;++i;continue;}
            if (!std::strcmp(argv[i],"--handoff")) {handoffOnly=true;++i;continue;}
            if (!std::strcmp(argv[i],"--add-only")) {addOnly=true;++i;continue;}
            if (!std::strcmp(argv[i],"--stagger")) {stagger=true;++i;continue;}
            if (i+1==argc) throw std::runtime_error("missing option value");
            size_t used=0;const auto value=std::stoull(argv[i+1],&used);
            if (used!=std::strlen(argv[i+1]) || !value || value>100000000) throw std::runtime_error("invalid option value");
            if (!std::strcmp(argv[i],"--iterations")) cpuIncrements=gpuIncrements=value;
            else if (!std::strcmp(argv[i],"--cpu-iterations")) cpuIncrements=value;
            else if (!std::strcmp(argv[i],"--gpu-iterations")) gpuIncrements=value;
            else if (!std::strcmp(argv[i],"--lock-iterations")) lockIterations=value;
            else if (!std::strcmp(argv[i],"--trials") && value<=20) trials=static_cast<unsigned>(value);
            else if (!std::strcmp(argv[i],"--timeout-seconds") && value<=300) timeout=static_cast<unsigned>(value);
            else throw std::runtime_error("invalid option");
            i+=2;
        }
        if (handoffOnly && (returnAdd || addOnly || stagger)) throw std::runtime_error("--handoff selects a separate experiment");
        if (stagger && (!addOnly || cpuIncrements<4)) throw std::runtime_error("--stagger requires --add-only and at least four CPU increments");
    } catch (const std::exception &e) {std::fprintf(stderr,"%s\n",e.what());return 2;}
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);std::vector<char> image{std::istreambuf_iterator<char>(file),{}};
    if (image.empty()) return 2;
    hsa_agent_t gpu{};hsa_code_object_reader_t reader{};hsa_executable_t executable{};
    hsa_queue_t *queue=nullptr;hsa_signal_t done{};void *data=nullptr,*arguments=nullptr;
    bool initialized=false,passed=true,safe=true;
    try {
        check(hsa_init(),"init");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t a,void *opaque) {
            hsa_device_type_t type;const auto status=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
            if (!status && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(opaque)->handle) *static_cast<hsa_agent_t *>(opaque)=a;
            return status;
        },&gpu),"enumeration");require(gpu.handle,"missing GPU");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver info");
        require(info.driver_build>=190,"install driver 190 first for live read-only mapping diagnostics");
        check(hsa_code_object_reader_create_from_memory(image.data(),image.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load");
        check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;uint32_t privateSize=0,groupSize=0,kernargSize=0;
        check(hsa_executable_get_symbol_by_name(executable,handoffOnly ? "atomic_handoff.kd" : "atomic_contention.kd",&gpu,&symbol),"kernel");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"descriptor");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&privateSize),"private size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&groupSize),"group size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&kernargSize),"kernarg size");
        require(kernargSize>=28 && kernargSize<=bytes,"unexpected kernarg layout");
        require(handoffOnly || kernargSize>=48,"contention fixture lacks CPU iteration and overlap-floor arguments");
        require(!privateSize && !groupSize,"atomic diagnostic must not require scratch or LDS");
        check(mac_hsa_memory_allocate_shared(gpu,bytes,&data),"DMA allocation");
        check(mac_hsa_memory_allocate_shared(gpu,bytes,&arguments),"kernarg allocation");
        require(!(reinterpret_cast<uintptr_t>(data)&16383),"DMA allocation not 16KiB aligned");
        check(hsa_signal_create(1,0,nullptr,&done),"completion");
        check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *) {
            std::fprintf(stderr,"queue error callback %#x\n",status);++queueErrors;
        },nullptr,UINT32_MAX,UINT32_MAX,&queue),"persistent queue");
        mac_hsa_shared_atomic_diagnostics_t policy{};
        check(mac_hsa_shared_atomic_diagnostics(data,queue,&policy,sizeof(policy)),"read atomic mapping/queue policy");
        require(policy.version==1 && (policy.valid_fields&3)==3,"missing PTE or MQD backing diagnostics");
        std::printf("Read-only policy: valid=%#llx GPUVA=%#llx DART=%#llx PTEoffset=%#llx actual=%#llx expected=%#llx match=%s\n",
            (unsigned long long)policy.valid_fields,(unsigned long long)policy.gpu_address,
            (unsigned long long)policy.dma_address,(unsigned long long)policy.pte_vram_offset,
            (unsigned long long)policy.pte_actual,(unsigned long long)policy.pte_expected,
            policy.pte_actual==policy.pte_expected ? "yes" : "no");
        std::printf("MQD backing=%#llx HQ_STATUS0=%#llx CP-PCIe-atomic-policy-bit29=%llu (backing snapshot, not live selected HQD)\n",
            (unsigned long long)policy.mqd_gpu_address,(unsigned long long)policy.mqd_backing_hq_status0,
            (unsigned long long)((policy.mqd_backing_hq_status0>>29)&1));
        if (policy.valid_fields&4)
            std::printf("Endpoint PCIe cap=%#llx DeviceCapabilities2=%#llx DeviceControl2=%#llx AtomicOp-requester-enable=%llu; root/bridge routing is separate\n",
                (unsigned long long)policy.pcie_capability_offset,(unsigned long long)policy.pcie_device_capabilities2,
                (unsigned long long)policy.pcie_device_control2,(unsigned long long)((policy.pcie_device_control2>>6)&1));
        else std::puts("Endpoint PCIe capability snapshot unavailable");
        if (policy.valid_fields&8)
            std::printf("GFXHUB PTBASE=%#llx CONTEXT0_CONTROL=%#llx\n",
                (unsigned long long)policy.gfxhub_page_table_base,(unsigned long long)policy.gfxhub_context0_control);
        std::printf("CPU mapping options requested=%#llx; actual CPU cache/MAIR=%s\n",
            (unsigned long long)policy.cpu_mapping_options,policy.cpu_cache_attributes==UINT64_MAX ? "unknown" : "reported");
        require(policy.pte_actual==policy.pte_expected,"actual GART PTE differs from allocation policy");
        std::printf("Native contention: driver=%llu gfx%u%u%u registry=%#llx CPU/GPU VA=%p bytes=%zu alignment=16384 trials=%u\n",
            (unsigned long long)info.driver_build,info.gfx_major,info.gfx_minor,info.gfx_revision,
            (unsigned long long)info.registry_id,data,bytes,trials);
        std::puts("Mapping: IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn), IODMACommand::PrepareForDMA, CPU IOConnectMapMemory64 options=0 (default cache policy), equal CPU/GPU VA. Actual CPU cacheability is not independently queried.");
        std::puts("GPU expected PTE policy is VALID|SYSTEM|SNOOPED|EXEC|R|W|IS_PTE with gfx12 MTYPE=2 (UC, bits54-55); actual mapping is recorded above.");
        std::puts("MQD bit29 acknowledges PCIe-atomic support to CP firmware; it is not the shader instruction's system-scope bit. DeviceControl2 requester enable is a separate endpoint policy. No policy registers are changed by this tool.");
        std::puts("Policy reference: docs/PCIE_ATOMIC_TEST_POLICY.md; AMD CPFW patch https://www.spinics.net/lists/amd-gfx/msg90787.html");
        std::puts("Experimental access exceeds the coarse allocation's promised semantics. Native CPU atomic_ref and GPU system-scope instructions touch the same words; HSA only controls queue completion.");
        std::puts("Record scripts/check-pcie-atomics.py output alongside this log; missing cached bits do not replace this measured result.");
        if (handoffOnly) passed=handoff(32,timeout,static_cast<uint64_t *>(data),arguments,queue,done,kernel);
        else {
        std::printf("Controls first: CPU-only and GPU-only %s use the same mapped words and operation counts. Mixed trials require progress overlap in both directions.\n",addOnly ? "add" : "add/CAS");
        std::printf("Requested increments CPU=%llu GPU=%llu add-only=%s stagger=%s; shared phase deadline=%us includes deliberate waiting\n",
            (unsigned long long)cpuIncrements,(unsigned long long)gpuIncrements,addOnly ? "yes" : "no",stagger ? "yes" : "no",timeout);
        for (auto participants:{Participants::CPUOnly,Participants::GPUOnly}) {
            for (unsigned mode:{returnAdd ? 2u : 0u,1u}) {
                if (addOnly && mode==1) continue;
                const auto result=trial(participants,0,mode,mode==1 ? lockIterations : cpuIncrements,
                    mode==1 ? lockIterations : gpuIncrements,false,timeout,
                    static_cast<uint64_t *>(data),arguments,queue,done,kernel,privateSize,groupSize);
                require(result.completed,"control incomplete; stopping before reusing GPU storage");
                require(result.correct,"single-agent control failed; mixed results would not isolate interoperability");
            }
        }
        for (unsigned n=1;n<=trials;++n) {
            for (unsigned mode:{returnAdd ? 2u : 0u,1u}) {
                if (addOnly && mode==1) continue;
                const auto result=trial(Participants::Mixed,n,mode,mode==1 ? lockIterations : cpuIncrements,
                    mode==1 ? lockIterations : gpuIncrements,stagger,timeout,
                    static_cast<uint64_t *>(data),arguments,queue,done,kernel,privateSize,groupSize);
                passed&=result.completed && result.correct && result.overlapped;
                require(result.completed,"phase incomplete; stopping before reusing GPU storage");
            }
        }
        }
    } catch (const std::exception &e) {std::fprintf(stderr,"FAIL/INCOMPLETE: %s\n",e.what());passed=false;}
    if (queue && hsa_queue_destroy(queue)) {safe=false;passed=false;}
    if (safe) {
        if (done.handle && hsa_signal_destroy(done)) passed=false;
        for (auto *pointer:{data,arguments}) if (pointer && hsa_memory_free(pointer)) passed=false;
        if (executable.handle && hsa_executable_destroy(executable)) passed=false;
    } else std::fputs("Queue removal unconfirmed; retaining DMA buffers, code and completion storage until reset.\n",stderr);
    if (reader.handle && hsa_code_object_reader_destroy(reader)) passed=false;
    if (initialized && hsa_shut_down()) passed=false;
    std::puts(passed ? (handoffOnly ? "PASS: serialized native atomic usage only; contended interoperability remains untested." : "PASS: selected CPU-only/GPU-only controls and mixed-agent trials completed with exact results and intact guards; mixed trials had overlap. This tests this mapping and path only.") :
        "FAIL/INCONCLUSIVE under current queue/mapping policy: inspect completion, overlap, counters and guards. This does not prove hardware impossibility; no general atomic capability enabled.");
    return passed ? 0 : 1;
}
