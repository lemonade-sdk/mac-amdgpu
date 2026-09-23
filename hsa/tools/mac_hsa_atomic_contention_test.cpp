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
    gpuAttempts=208, gpuOverlap=224;
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
                    cpuProgress,gpuProgress,abortWord,state,gpuAttempts,gpuOverlap})
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
TrialResult trial(Participants participants,unsigned ordinal,unsigned mode,uint64_t iterations,unsigned timeoutSeconds,
                  uint64_t *data,void *arguments,hsa_queue_t *queue,hsa_signal_t done,
                  uint64_t kernel,uint32_t privateSize,uint32_t groupSize) {
    const bool useCPU=participants!=Participants::GPUOnly,useGPU=participants!=Participants::CPUOnly;
    const bool mixed=useCPU && useGPU;
    const char *label=mixed ? "mixed" : useCPU ? "CPU-only" : "GPU-only";
    const uint64_t expectedCount=iterations*(unsigned(useCPU)+unsigned(useGPU));
    std::fill(data,data+words,guard);
    for (size_t i=0;i<words;++i) if (mutableWord(i)) data[i]=0;
    data[inverse]=magic;
    std::memset(arguments,0,bytes);
    const uint64_t address=reinterpret_cast<uintptr_t>(data);
    const uint64_t maxAttempts=100000000;
    std::memcpy(arguments,&address,8);std::memcpy(static_cast<char *>(arguments)+8,&iterations,8);
    std::memcpy(static_cast<char *>(arguments)+16,&maxAttempts,8);
    std::memcpy(static_cast<char *>(arguments)+24,&mode,4);
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
    uint64_t cpuCompleted=0,cpuErrors=0,cpuTries=0,cpuOverlap=0;
    bool cpuStarted=false;
    if (useGPU) publish(queue,packet);
    std::thread cpu([&] {
        while (useGPU && !load(data,ready)) {
            if (cancel || Clock::now()>=deadline) {cpuDone=true;return;}
            std::this_thread::yield();
        }
        cpuStarted=true;
        if (useGPU) store(data,go,1);
        // The GPU-only control uses the identical shader/handshake but never
        // modifies its counter or lock from the CPU.
        if (!useCPU) {cpuDone=true;return;}
        // Require the GPU to begin before the CPU starts its bounded work.
        while (mixed && !load(data,gpuProgress)) {
            if (cancel || load(data,state) || Clock::now()>=deadline) {cpuDone=true;return;}
            std::this_thread::yield();
        }
        std::atomic_ref<uint64_t> count(data[counter]),mutex(data[lockWord]);
        for (uint64_t i=0;i<iterations;++i) {
            if (!(i&255) && (cancel || Clock::now()>=deadline)) break;
            if (!mode) count.fetch_add(1,std::memory_order_seq_cst);
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
                if (gpu && gpu<iterations) ++cpuOverlap;
                store(data,cpuProgress,cpuCompleted);
            }
        }
finished:
        store(data,cpuProgress,cpuCompleted);cpuDone.store(true,std::memory_order_release);
    });
    auto nextReport=start+std::chrono::seconds(1);
    bool timedOut=false;
    while (!cpuDone.load(std::memory_order_acquire) || (useGPU && hsa_signal_load_scacquire(done)!=0)) {
        const auto now=Clock::now();
        if (now>=nextReport) {
            std::printf("participants=%s trial=%u mode=%s CPU=%llu GPU=%llu state=%llu elapsed=%.1fs\n",label,ordinal,mode ? "CAS-lock" : "fetch-add",
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
    const auto observed=load(data,mode ? payload : counter);
    bool guards=true;
    if (retired) for (size_t i=0;i<words;++i) if (!mutableWord(i) && data[i]!=guard) guards=false;
    const bool completed=!timedOut && retired && cpuStarted && cpuCompleted==(useCPU ? iterations : 0) &&
        gpuCompleted==(useGPU ? iterations : 0) && load(data,state)==(useGPU ? 1u : 0u) && !queueErrors;
    const bool overlap=!mixed || (cpuOverlap && load(data,gpuOverlap));
    const bool correct=completed && observed==expectedCount && guards && !cpuErrors && !gpuErrorCount &&
        (!mode || (!load(data,lockWord) && !load(data,cpuInside) && !load(data,gpuInside) &&
                   load(data,inverse)==(observed^magic)));
    std::printf("%s participants=%s trial=%u %s expected=%llu observed=%llu CPU=%llu GPU=%llu overlap_batches=%llu/%llu overlap_required=%s "
        "lock_errors=%llu/%llu CAS_retries=%llu/%llu guards=%s completion=%s elapsed=%.3fs\n",
        !completed ? "INCOMPLETE" : !correct ? "MISMATCH" : !overlap ? "INCONCLUSIVE" : "PASS",label,ordinal,
        mode ? "CAS-lock" : "fetch-add",(unsigned long long)expectedCount,(unsigned long long)observed,
        (unsigned long long)cpuCompleted,(unsigned long long)gpuCompleted,(unsigned long long)cpuOverlap,
        (unsigned long long)load(data,gpuOverlap),mixed ? "yes" : "no",(unsigned long long)cpuErrors,(unsigned long long)gpuErrorCount,
        (unsigned long long)cpuTries,(unsigned long long)load(data,gpuAttempts),guards ? "intact" : "changed",
        !useGPU ? "no-GPU-dispatch" : retired ? "retired" : "pending",std::chrono::duration<double>(Clock::now()-start).count());
    return {completed,correct,overlap};
}
}

int main(int argc,char **argv) {
    if (argc<3 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run shader.hsaco [--trials N] [--iterations N] [--lock-iterations N] [--timeout-seconds N]\n",argv[0]);return 2;
    }
    uint64_t increments=10000000,lockIterations=1000000;unsigned trials=3,timeout=60;
    try {
        for (int i=3;i<argc;i+=2) {
            if (i+1==argc) throw std::runtime_error("missing option value");
            size_t used=0;const auto value=std::stoull(argv[i+1],&used);
            if (used!=std::strlen(argv[i+1]) || !value || value>100000000) throw std::runtime_error("invalid option value");
            if (!std::strcmp(argv[i],"--iterations")) increments=value;
            else if (!std::strcmp(argv[i],"--lock-iterations")) lockIterations=value;
            else if (!std::strcmp(argv[i],"--trials") && value<=20) trials=static_cast<unsigned>(value);
            else if (!std::strcmp(argv[i],"--timeout-seconds") && value<=300) timeout=static_cast<unsigned>(value);
            else throw std::runtime_error("invalid option");
        }
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
        require(info.driver_build>=189,"install driver 189 first");
        check(hsa_code_object_reader_create_from_memory(image.data(),image.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load");
        check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;uint32_t privateSize=0,groupSize=0,kernargSize=0;
        check(hsa_executable_get_symbol_by_name(executable,"atomic_contention.kd",&gpu,&symbol),"kernel");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"descriptor");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&privateSize),"private size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&groupSize),"group size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&kernargSize),"kernarg size");
        require(kernargSize>=28 && kernargSize<=bytes,"unexpected kernarg layout");
        check(mac_hsa_memory_allocate_shared(gpu,bytes,&data),"DMA allocation");
        check(mac_hsa_memory_allocate_shared(gpu,bytes,&arguments),"kernarg allocation");
        require(!(reinterpret_cast<uintptr_t>(data)&16383),"DMA allocation not 16KiB aligned");
        check(hsa_signal_create(1,0,nullptr,&done),"completion");
        check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *) {
            std::fprintf(stderr,"queue error callback %#x\n",status);++queueErrors;
        },nullptr,UINT32_MAX,UINT32_MAX,&queue),"persistent queue");
        std::printf("Native contention: driver=%llu gfx%u%u%u registry=%#llx CPU/GPU VA=%p bytes=%zu alignment=16384 trials=%u\n",
            (unsigned long long)info.driver_build,info.gfx_major,info.gfx_minor,info.gfx_revision,
            (unsigned long long)info.registry_id,data,bytes,trials);
        std::puts("Mapping: IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn), IODMACommand::PrepareForDMA, CPU IOConnectMapMemory64 options=0 (default cache policy), equal CPU/GPU VA. Actual CPU cacheability is not independently queried.");
        std::puts("GPU mapping: source SYSMEM_RW policy is VALID|SYSTEM|SNOOPED|EXEC|R|W|IS_PTE with gfx12 MTYPE=2 (UC, bits54-55); no live PTE readback in this test.");
        std::puts("Queue policy (candidate source, not live MQD readback): CP_HQD_HQ_STATUS0=0x00004000; bit14=1, bit29=0. Bit29 acknowledges PCIe-atomic support to CP firmware; clear selects firmware fallback. It is not the shader instruction's system-scope bit.");
        std::puts("PCIe DeviceControl2 AtomicOp Requester Enable is separate and is not queried or changed here. A failure under this queue/mapping policy cannot establish that the hardware or ARM64 platform can never support mixed atomics.");
        std::puts("Policy reference: docs/PCIE_ATOMIC_TEST_POLICY.md; AMD CPFW patch https://www.spinics.net/lists/amd-gfx/msg90787.html");
        std::puts("Experimental access exceeds the coarse allocation's promised semantics. Native CPU atomic_ref and GPU system-scope instructions touch the same words; HSA only controls queue completion.");
        std::puts("Record scripts/check-pcie-atomics.py output alongside this log; missing cached bits do not replace this measured result.");
        std::puts("Controls first: CPU-only and GPU-only add/CAS use the same mapped words and operation counts. Mixed trials require progress overlap in both directions.");
        for (auto participants:{Participants::CPUOnly,Participants::GPUOnly}) {
            for (unsigned mode=0;mode<2;++mode) {
                const auto result=trial(participants,0,mode,mode ? lockIterations : increments,timeout,
                    static_cast<uint64_t *>(data),arguments,queue,done,kernel,privateSize,groupSize);
                require(result.completed,"control incomplete; stopping before reusing GPU storage");
                require(result.correct,"single-agent control failed; mixed results would not isolate interoperability");
            }
        }
        for (unsigned n=1;n<=trials;++n) {
            for (unsigned mode=0;mode<2;++mode) {
                const auto result=trial(Participants::Mixed,n,mode,mode ? lockIterations : increments,timeout,
                    static_cast<uint64_t *>(data),arguments,queue,done,kernel,privateSize,groupSize);
                passed&=result.completed && result.correct && result.overlapped;
                require(result.completed,"phase incomplete; stopping before reusing GPU storage");
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
    std::puts(passed ? "PASS: CPU-only/GPU-only controls and every native mixed-agent count/lock trial completed with exact results and intact guards; mixed trials had overlap. This tests this mapping and path only." :
        "FAIL/INCONCLUSIVE under current queue/mapping policy: inspect completion, overlap, counters and guards. This does not prove hardware impossibility; no general atomic capability enabled.");
    return passed ? 0 : 1;
}
