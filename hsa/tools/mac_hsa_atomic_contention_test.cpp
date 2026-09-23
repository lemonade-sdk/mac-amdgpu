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
bool pingPong(uint64_t rounds,unsigned timeoutSeconds,uint64_t *data,void *arguments,
              hsa_queue_t *queue,hsa_signal_t done,uint64_t kernel) {
    constexpr size_t turn=0,readyWord=16,abort=32,gpuState=48,progress=64,errors=80,
        firstRound=96,firstWord=97,firstExpected=98,firstObserved=99,payloadBegin=128,payloadWords=64;
    constexpr uint64_t cpuTag=0x13579bdf2468ace0ull,gpuTag=0xfedcba9876543210ull,
        mix=0x9e3779b97f4a7c15ull,wordMix=0x0101010101010101ull;
    auto mutableCell=[&](size_t index) {
        return index==turn || index==readyWord || index==abort || index==gpuState || index==progress ||
            index==errors || (index>=firstRound && index<=firstObserved) ||
            (index>=payloadBegin && index<payloadBegin+payloadWords);
    };
    std::fill(data,data+words,guard);
    for (size_t i=0;i<words;++i) if (mutableCell(i)) data[i]=0;
    // Kernarg padding is also guarded and checked after retirement.
    std::fill(static_cast<uint64_t *>(arguments),static_cast<uint64_t *>(arguments)+words,guard);
    const uint64_t address=reinterpret_cast<uintptr_t>(data),maxAttempts=100000000;
    std::memcpy(arguments,&address,8);std::memcpy(static_cast<char *>(arguments)+8,&rounds,8);
    std::memcpy(static_cast<char *>(arguments)+16,&maxAttempts,8);
    hsa_signal_store_screlease(done,1);
    hsa_kernel_dispatch_packet_t packet{};
    packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet.workgroup_size_x=packet.grid_size_x=32;
    packet.workgroup_size_y=packet.workgroup_size_z=packet.grid_size_y=packet.grid_size_z=1;
    packet.kernel_object=kernel;packet.kernarg_address=arguments;packet.completion_signal=done;
    std::puts("Ownership ping-pong: one persistent dispatch; CPU/GPU alternate release/acquire of a 64-bit sequence on its own 128-byte region. Abort and 512-byte payload occupy separate regions.");
    std::puts("Payload accesses are relaxed atomic loads/stores under ownership; no fetch-add/CAS or contended RMW. This probes ownership visibility on the existing coarse mapping; it does not establish fine-grained allocation semantics.");
    const auto start=Clock::now(),deadline=start+std::chrono::seconds(timeoutSeconds);
    auto nextReport=start+std::chrono::seconds(1),exchangeStart=start,exchangeEnd=start;
    uint64_t completed=0,cpuErrors=0,polls=0;
    bool interrupted=false,timedOut=false;
    auto healthy=[&] {
        const auto now=Clock::now();
        if (now>=nextReport) {
            std::printf("ping-pong CPU-rounds=%llu GPU-rounds=%llu turn=%llu state=%llu elapsed=%.3fs\n",
                (unsigned long long)completed,(unsigned long long)load(data,progress),
                (unsigned long long)load(data,turn),(unsigned long long)load(data,gpuState),
                std::chrono::duration<double>(now-start).count());
            nextReport=now+std::chrono::seconds(1);
        }
        timedOut=now>=deadline;
        if (timedOut || queueErrors || load(data,gpuState)>=2) {interrupted=true;return false;}
        return true;
    };
    auto await=[&](size_t index,uint64_t expected) {
        for (;;) {
            const auto observed=load(data,index);
            if (observed==expected) return true;
            if (index==turn && observed>expected) {
                std::fprintf(stderr,"ping-pong sequence advanced unexpectedly: expected=%llu observed=%llu\n",
                    (unsigned long long)expected,(unsigned long long)observed);
                ++cpuErrors;interrupted=true;return false;
            }
            if (!(++polls&255) && !healthy()) return false;
            if (!(polls&4095)) std::this_thread::yield();
        }
    };
    publish(queue,packet);
    if (await(readyWord,1)) {
        exchangeStart=Clock::now();
        for (uint64_t round=0;round<rounds;++round) {
            if (!(round&255) && !healthy()) break;
            // Initial turn=0 belongs to CPU; thereafter the preceding acquire
            // of the even turn grants ownership before these relaxed stores.
            for (size_t word=0;word<payloadWords;++word)
                std::atomic_ref<uint64_t>(data[payloadBegin+word]).store(
                    cpuTag^((round+1)*mix)^(word*wordMix),std::memory_order_relaxed);
            store(data,turn,round*2+1);
            if (!await(turn,round*2+2)) break;
            for (size_t word=0;word<payloadWords;++word) {
                const auto expected=gpuTag^((round+1)*mix)^(word*wordMix);
                const auto observed=std::atomic_ref<uint64_t>(data[payloadBegin+word]).load(std::memory_order_relaxed);
                if (observed!=expected) {
                    std::fprintf(stderr,"CPU payload mismatch round=%llu word=%zu expected=%#llx observed=%#llx\n",
                        (unsigned long long)(round+1),word,(unsigned long long)expected,(unsigned long long)observed);
                    ++cpuErrors;interrupted=true;break;
                }
            }
            if (interrupted) break;
            completed=round+1;
        }
        exchangeEnd=Clock::now();
    }
    if (completed!=rounds || interrupted) store(data,abort,1);
    while (hsa_signal_load_scacquire(done)!=0 && !interrupted) {
        if (!healthy()) {store(data,abort,1);break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto grace=Clock::now()+std::chrono::seconds(3);
    while (hsa_signal_load_scacquire(done)!=0 && Clock::now()<grace && !queueErrors)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool retired=hsa_signal_load_scacquire(done)==0;
    bool guards=true,argsIntact=true;
    if (retired) {
        for (size_t i=0;i<words;++i) if (!mutableCell(i) && data[i]!=guard) guards=false;
        const auto *args=static_cast<const uint64_t *>(arguments);
        argsIntact=args[0]==address && args[1]==rounds && args[2]==maxAttempts;
        for (size_t i=3;i<words;++i) if (args[i]!=guard) argsIntact=false;
    }
    const auto gpuErrors=load(data,errors),gpuCompleted=load(data,progress),lastTurn=load(data,turn);
    const bool passed=retired && !interrupted && !queueErrors && completed==rounds && gpuCompleted==rounds &&
        lastTurn==rounds*2 && load(data,gpuState)==1 && !cpuErrors && !gpuErrors && guards && argsIntact;
    if (gpuErrors) std::fprintf(stderr,"GPU payload mismatch round=%llu word=%llu expected=%#llx observed=%#llx\n",
        (unsigned long long)load(data,firstRound),(unsigned long long)load(data,firstWord),
        (unsigned long long)load(data,firstExpected),(unsigned long long)load(data,firstObserved));
    const double exchangeSeconds=std::chrono::duration<double>(exchangeEnd-exchangeStart).count();
    std::printf("%s ownership-ping-pong requested-rounds=%llu CPU=%llu GPU=%llu turn=%llu/%llu payload-errors=%llu/%llu data-guards=%s kernarg-guards=%s completion=%s timeout=%s\n",
        passed ? "PASS" : "FAIL/INCOMPLETE",(unsigned long long)rounds,(unsigned long long)completed,
        (unsigned long long)gpuCompleted,(unsigned long long)lastTurn,(unsigned long long)(rounds*2),
        (unsigned long long)cpuErrors,(unsigned long long)gpuErrors,retired ? (guards ? "intact" : "changed") : "unverified",
        retired ? (argsIntact ? "intact" : "changed") : "unverified",retired ? "retired" : "pending",timedOut ? "yes" : "no");
    std::printf("Host steady-clock: exchange-start=%.6fs exchange-end=%.6fs elapsed=%.6fs rounds/s=%.1f ownership-transfers/s=%.1f (two transfers per full round; includes validation/polling; no GPU clock correlation)\n",
        std::chrono::duration<double>(exchangeStart-start).count(),std::chrono::duration<double>(exchangeEnd-start).count(),exchangeSeconds,
        exchangeSeconds>0 ? completed/exchangeSeconds : 0,exchangeSeconds>0 ? completed*2/exchangeSeconds : 0);
    return passed;
}

}

int main(int argc,char **argv) {
    if (argc<3 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run shader.hsaco [--ping-pong --rounds N] [--requester-ab] [--return-add] [--handoff] [--add-only] [--stagger] [--trials N] [--iterations N | --cpu-iterations N --gpu-iterations N] [--lock-iterations N] [--timeout-seconds N]\n",argv[0]);return 2;
    }
    uint64_t cpuIncrements=10000000,gpuIncrements=10000000,lockIterations=1000000;unsigned trials=3,timeout=60;
    bool returnAdd=false,handoffOnly=false,addOnly=false,stagger=false,requesterAB=false,pingPongOnly=false;
    uint64_t pingPongRounds=1000000;bool roundsOption=false,timeoutOption=false;
    bool countOption=false,trialOption=false,lockOption=false;
    try {
        for (int i=3;i<argc;) {
            if (!std::strcmp(argv[i],"--ping-pong")) {pingPongOnly=true;++i;continue;}
            if (!std::strcmp(argv[i],"--requester-ab")) {requesterAB=true;++i;continue;}
            if (!std::strcmp(argv[i],"--return-add")) {returnAdd=true;++i;continue;}
            if (!std::strcmp(argv[i],"--handoff")) {handoffOnly=true;++i;continue;}
            if (!std::strcmp(argv[i],"--add-only")) {addOnly=true;++i;continue;}
            if (!std::strcmp(argv[i],"--stagger")) {stagger=true;++i;continue;}
            if (i+1==argc) throw std::runtime_error("missing option value");
            size_t used=0;const auto value=std::stoull(argv[i+1],&used);
            if (used!=std::strlen(argv[i+1]) || !value || value>100000000) throw std::runtime_error("invalid option value");
            if (!std::strcmp(argv[i],"--iterations") || !std::strcmp(argv[i],"--cpu-iterations") || !std::strcmp(argv[i],"--gpu-iterations")) countOption=true;
            if (!std::strcmp(argv[i],"--trials")) trialOption=true;
            if (!std::strcmp(argv[i],"--timeout-seconds")) timeoutOption=true;
            if (!std::strcmp(argv[i],"--iterations")) cpuIncrements=gpuIncrements=value;
            else if (!std::strcmp(argv[i],"--cpu-iterations")) cpuIncrements=value;
            else if (!std::strcmp(argv[i],"--gpu-iterations")) gpuIncrements=value;
            else if (!std::strcmp(argv[i],"--lock-iterations")) {lockIterations=value;lockOption=true;}
            else if (!std::strcmp(argv[i],"--rounds")) {pingPongRounds=value;roundsOption=true;}
            else if (!std::strcmp(argv[i],"--trials") && value<=20) trials=static_cast<unsigned>(value);
            else if (!std::strcmp(argv[i],"--timeout-seconds") && value<=1200) timeout=static_cast<unsigned>(value);
            else throw std::runtime_error("invalid option");
            i+=2;
        }
        if (timeout>300 && !pingPongOnly) throw std::runtime_error("timeouts above 300 seconds require --ping-pong");
        if (roundsOption && !pingPongOnly) throw std::runtime_error("--rounds requires --ping-pong");
        if (pingPongOnly) {
            if (requesterAB || handoffOnly || returnAdd || addOnly || stagger || countOption || trialOption || lockOption)
                throw std::runtime_error("--ping-pong is a separate ownership experiment; use --rounds and --timeout-seconds");
            if (!timeoutOption) timeout=300;
        }
        if (requesterAB) {
            if (handoffOnly || returnAdd) throw std::runtime_error("--requester-ab requires the unchanged non-returning add shader path");
            if (countOption && (cpuIncrements!=10000000 || gpuIncrements!=1000000))
                throw std::runtime_error("--requester-ab fixes CPU=10000000 and GPU=1000000");
            if (trialOption && trials!=1) throw std::runtime_error("--requester-ab requires exactly one stagger A trial per policy");
            cpuIncrements=10000000;gpuIncrements=1000000;trials=1;addOnly=stagger=true;
        }
        if (handoffOnly && (returnAdd || addOnly || stagger)) throw std::runtime_error("--handoff selects a separate experiment");
        if (stagger && (!addOnly || cpuIncrements<4)) throw std::runtime_error("--stagger requires --add-only and at least four CPU increments");
    } catch (const std::exception &e) {std::fprintf(stderr,"%s\n",e.what());return 2;}
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);std::vector<char> image{std::istreambuf_iterator<char>(file),{}};
    if (image.empty()) return 2;
    hsa_agent_t gpu{};hsa_code_object_reader_t reader{};hsa_executable_t executable{};
    hsa_queue_t *queue=nullptr;hsa_signal_t done{};void *data=nullptr,*arguments=nullptr;
    bool initialized=false,passed=true,safe=true,requesterRestoreNeeded=false,experimentCompleted=false;
    bool abExact[2]={false,false},abOverlap[2]={false,false};
    uint64_t requesterBaselineControl2=UINT64_MAX;
    try {
        check(hsa_init(),"init");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t a,void *opaque) {
            hsa_device_type_t type;const auto status=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
            if (!status && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(opaque)->handle) *static_cast<hsa_agent_t *>(opaque)=a;
            return status;
        },&gpu),"enumeration");require(gpu.handle,"missing GPU");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver info");
        require(info.driver_build>=(requesterAB ? 191u : 190u),requesterAB ? "install driver 191 first for requester A/B" : "install driver 190 first for live read-only mapping diagnostics");
        check(hsa_code_object_reader_create_from_memory(image.data(),image.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load");
        check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;uint32_t privateSize=0,groupSize=0,kernargSize=0;
        check(hsa_executable_get_symbol_by_name(executable,pingPongOnly ? "ownership_ping_pong.kd" : handoffOnly ? "atomic_handoff.kd" : "atomic_contention.kd",&gpu,&symbol),"kernel");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"descriptor");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&privateSize),"private size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&groupSize),"group size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&kernargSize),"kernarg size");
        require(kernargSize>=(pingPongOnly ? 24u : 28u) && kernargSize<=bytes,"unexpected kernarg layout");
        require(!pingPongOnly || kernargSize==24,"unexpected ownership fixture kernarg layout");
        require(pingPongOnly || handoffOnly || kernargSize>=48,"contention fixture lacks CPU iteration and overlap-floor arguments");
        require(!privateSize && !groupSize,"atomic diagnostic must not require scratch or LDS");
        check(mac_hsa_memory_allocate_shared(gpu,bytes,&data),"DMA allocation");
        check(mac_hsa_memory_allocate_shared(gpu,bytes,&arguments),"kernarg allocation");
        require(!(reinterpret_cast<uintptr_t>(data)&16383),"DMA allocation not 16KiB aligned");
        check(hsa_signal_create(1,0,nullptr,&done),"completion");
        auto createQueue=[&] { check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *) {
            std::fprintf(stderr,"queue error callback %#x\n",status);++queueErrors;
        },nullptr,UINT32_MAX,UINT32_MAX,&queue),"persistent queue"); };
        createQueue();
        auto readPolicy=[&] {
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
        return policy;
        };
        const auto baselinePolicy=readPolicy();
        if (requesterAB) {
            require((baselinePolicy.valid_fields&4) && !(baselinePolicy.pcie_device_control2&(1u<<6)),"requester A/B requires confirmed baseline RequesterEnable OFF");
            requesterBaselineControl2=baselinePolicy.pcie_device_control2;
        }
        std::printf("Native contention: driver=%llu gfx%u%u%u registry=%#llx CPU/GPU VA=%p bytes=%zu alignment=16384 trials=%u\n",
            (unsigned long long)info.driver_build,info.gfx_major,info.gfx_minor,info.gfx_revision,
            (unsigned long long)info.registry_id,data,bytes,trials);
        std::puts("Mapping: IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn), IODMACommand::PrepareForDMA, CPU IOConnectMapMemory64 options=0 (default cache policy), equal CPU/GPU VA. Actual CPU cacheability is not independently queried.");
        std::puts("GPU expected PTE policy is VALID|SYSTEM|SNOOPED|EXEC|R|W|IS_PTE with gfx12 MTYPE=2 (UC, bits54-55); actual mapping is recorded above.");
        std::puts("MQD bit29 acknowledges PCIe-atomic support to CP firmware; it is not the shader instruction's system-scope bit. DeviceControl2 requester enable is a separate endpoint policy.");
        std::puts(requesterAB ? "EXPERIMENT: only endpoint DeviceControl2 RequesterEnable bit6 changes between retired queues. Cached/root path is not qualified; no general capability is enabled." : "No policy registers are changed by this tool.");
        std::puts("Policy reference: docs/PCIE_ATOMIC_TEST_POLICY.md; AMD CPFW patch https://www.spinics.net/lists/amd-gfx/msg90787.html");
        std::puts("Experimental access exceeds the coarse allocation's promised semantics. Native CPU atomic_ref and GPU system-scope instructions touch the same words; HSA only controls queue completion.");
        std::puts("Record scripts/check-pcie-atomics.py output alongside this log; missing cached bits do not replace this measured result.");
        if (pingPongOnly) passed=pingPong(pingPongRounds,timeout,static_cast<uint64_t *>(data),arguments,queue,done,kernel);
        else if (handoffOnly) passed=handoff(32,timeout,static_cast<uint64_t *>(data),arguments,queue,done,kernel);
        else {
        auto runTrials=[&](unsigned abIndex) {
        if (requesterAB) std::printf("REQUESTER-A/B phase=%s begin; identical allocation, shader, arguments, CPU ordering and trial algorithm\n",abIndex ? "ON" : "OFF");
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
                if (requesterAB) {abExact[abIndex]=result.correct;abOverlap[abIndex]=result.overlapped;}
                require(result.completed,"phase incomplete; stopping before reusing GPU storage");
            }
        }
        if (requesterAB) {
            for (size_t i=0;i<words;++i) if (!mutableWord(i))
                require(static_cast<uint64_t *>(data)[i]==guard,"guard damage prevents requester A/B continuation");
            std::printf("REQUESTER-A/B phase=%s completed exact-count-and-guards=%s overlap=%s\n",
                abIndex ? "ON" : "OFF",abExact[abIndex] ? "yes" : "no",abOverlap[abIndex] ? "yes" : "no");
        }
        };
        runTrials(0);
        if (requesterAB) {
            check(hsa_queue_destroy(queue),"retire OFF queue before requester change");queue=nullptr;
            mac_hsa_atomic_requester_experiment_t changed{};
            requesterRestoreNeeded=true;
            const auto changeStatus=mac_hsa_atomic_requester_experiment(gpu,1,&changed,sizeof(changed));
            std::printf("REQUESTER-A/B begin status=%#x before=%#llx requested=%#llx observed=%#llx original=%#llx active=%llu restore-pending=%llu driver-status=%#llx\n",changeStatus,
                (unsigned long long)changed.before_control2,(unsigned long long)changed.requested_control2,
                (unsigned long long)changed.observed_control2,(unsigned long long)changed.original_control2,
                (unsigned long long)changed.active,(unsigned long long)changed.restore_pending,(unsigned long long)changed.driver_status);
            check(changeStatus,"begin requester experiment");
            require(changed.version==1 && changed.active==1 && changed.restore_pending==1 && !changed.driver_status && changed.before_control2==baselinePolicy.pcie_device_control2 &&
                changed.original_control2==changed.before_control2 && changed.requested_control2==(changed.before_control2|(1u<<6)) &&
                changed.observed_control2==changed.requested_control2,"requester begin changed an unexpected policy bit");
            createQueue();
            const auto onPolicy=readPolicy();
            std::printf("REQUESTER-A/B queue recreated: MQD address OFF=%#llx ON=%#llx (backing address may change; queue policy must match)\n",
                (unsigned long long)baselinePolicy.mqd_gpu_address,(unsigned long long)onPolicy.mqd_gpu_address);
            require(onPolicy.valid_fields==baselinePolicy.valid_fields && (onPolicy.valid_fields&4) && onPolicy.pcie_device_control2==(baselinePolicy.pcie_device_control2|(1u<<6)) &&
                onPolicy.pcie_capability_offset==baselinePolicy.pcie_capability_offset &&
                onPolicy.pcie_device_capabilities2==baselinePolicy.pcie_device_capabilities2 &&
                onPolicy.gpu_address==baselinePolicy.gpu_address && onPolicy.dma_address==baselinePolicy.dma_address &&
                onPolicy.pte_vram_offset==baselinePolicy.pte_vram_offset && onPolicy.pte_actual==baselinePolicy.pte_actual &&
                onPolicy.pte_expected==baselinePolicy.pte_expected &&
                onPolicy.mqd_backing_hq_status0==baselinePolicy.mqd_backing_hq_status0 &&
                onPolicy.gfxhub_page_table_base==baselinePolicy.gfxhub_page_table_base &&
                onPolicy.gfxhub_context0_control==baselinePolicy.gfxhub_context0_control &&
                onPolicy.cpu_mapping_options==baselinePolicy.cpu_mapping_options &&
                onPolicy.cpu_cache_attributes==baselinePolicy.cpu_cache_attributes,
                "ON policy differs beyond requester bit or mapped storage changed");
            runTrials(1);experimentCompleted=true;
        }
        }
    } catch (const std::exception &e) {std::fprintf(stderr,"FAIL/INCOMPLETE: %s\n",e.what());passed=false;}
    if (queue && hsa_queue_destroy(queue)) {safe=false;passed=false;experimentCompleted=false;}
    if (requesterRestoreNeeded && safe) {
        mac_hsa_atomic_requester_experiment_t restored{};
        const auto status=mac_hsa_atomic_requester_experiment(gpu,0,&restored,sizeof(restored));
        const bool restoredOK=!status && restored.version==1 && !restored.active && !restored.restore_pending && !restored.driver_status &&
            restored.original_control2==requesterBaselineControl2 &&
            restored.requested_control2==requesterBaselineControl2 &&
            restored.observed_control2==requesterBaselineControl2;
        std::printf("REQUESTER-A/B restore status=%#x observed=%#llx original=%#llx baseline=%#llx requested=%#llx active=%llu restore-pending=%llu driver-status=%#llx result=%s\n",
            status,(unsigned long long)restored.observed_control2,(unsigned long long)restored.original_control2,
            (unsigned long long)requesterBaselineControl2,(unsigned long long)restored.requested_control2,
            (unsigned long long)restored.active,(unsigned long long)restored.restore_pending,
            (unsigned long long)restored.driver_status,restoredOK ? "restored" : "unconfirmed");
        if (!restoredOK) {passed=false;experimentCompleted=false;}
    } else if (requesterRestoreNeeded) {
        experimentCompleted=false;
        std::fputs("Requester restoration deferred to driver: queue removal is unconfirmed.\n",stderr);
    }
    if (safe) {
        if (done.handle && hsa_signal_destroy(done)) {passed=false;experimentCompleted=false;}
        for (auto *pointer:{data,arguments}) if (pointer && hsa_memory_free(pointer)) {passed=false;experimentCompleted=false;}
        if (executable.handle && hsa_executable_destroy(executable)) {passed=false;experimentCompleted=false;}
    } else std::fputs("Queue removal unconfirmed; retaining DMA buffers, code and completion storage until reset.\n",stderr);
    if (reader.handle && hsa_code_object_reader_destroy(reader)) {passed=false;experimentCompleted=false;}
    if (initialized && hsa_shut_down()) {passed=false;experimentCompleted=false;}
    if (requesterAB) std::printf("REQUESTER-A/B experiment=%s OFF-exact=%s OFF-overlap=%s ON-exact=%s ON-overlap=%s; completed experiment does not imply atomic interoperability passed\n",
        experimentCompleted && safe ? "completed" : "incomplete",abExact[0] ? "yes" : "no",abOverlap[0] ? "yes" : "no",
        abExact[1] ? "yes" : "no",abOverlap[1] ? "yes" : "no");
    std::puts(passed ? (pingPongOnly ? "PASS: selected ownership rounds validated both directions with release/acquire and intact guards on this experimental coarse mapping; no contended RMW or fine-grained capability claim." : handoffOnly ? "PASS: serialized native atomic usage only; contended interoperability remains untested." : "PASS: selected CPU-only/GPU-only controls and mixed-agent trials completed with exact results and intact guards; mixed trials had overlap. This tests this mapping and path only.") :
        "FAIL/INCONCLUSIVE under current queue/mapping policy: inspect completion, overlap, counters and guards. This does not prove hardware impossibility; no general atomic capability enabled.");
    return passed ? 0 : 1;
}
