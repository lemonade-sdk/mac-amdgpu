#include "mac_hsa.h"
#include "../src/signal_mailbox.h"
#include <hsa/amd_hsa_signal.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
using mac_hsa::SignalOperation;
constexpr uint64_t guard=0x97531ace2468bdf0ull;
std::atomic<bool> queueError{false};
void require(bool v,const char *why) {if (!v) throw std::runtime_error(why);}
void check(hsa_status_t s,const char *why) {if (s) {std::fprintf(stderr,"%s: %#x\n",why,s);throw std::runtime_error(why);}}
uint64_t apply(const SignalOperation &op,uint64_t &value) {
    const auto old=value;
    switch(op.operation) {
    case 1: case 7:value=op.operand;break;
    case 2:value+=op.operand;break;
    case 3:value-=op.operand;break;
    case 4:value&=op.operand;break;
    case 5:value|=op.operand;break;
    case 6:value^=op.operand;break;
    case 8:if(value==op.compare)value=op.operand;break;
    case 9:break;
    default:throw std::runtime_error("invalid model operation");
    }
    return old;
}
std::vector<SignalOperation> plan(size_t count) {
    std::vector<SignalOperation> out;uint64_t value=0;
    for(size_t i=0;i<count;++i) {
        SignalOperation op{};
        switch(i%11) {
        case 0:op={7,UINT64_MAX-3,0};break;
        case 1:op={2,7,0};break;
        case 2:op={3,9,0};break;
        case 3:op={4,0x7fff,0};break;
        case 4:op={5,0x8000,0};break;
        case 5:op={6,0x5555,0};break;
        case 6:op={8,0x123456789abcdef0ull,value};break;
        case 7:op={8,77,value^1};break;
        case 8:op={9,0,0};break;
        case 9:op={1,0x8000000000000000ull,0};break;
        case 10:op={2,UINT64_MAX,0};break;
        }
        apply(op,value);out.push_back(op);
    }
    return out;
}
void baseline(hsa_signal_t signal,const std::vector<SignalOperation> &ops) {
    hsa_signal_store_screlease(signal,0);uint64_t expected=0;
    const auto start=Clock::now();
    for(const auto &op:ops) {
        require(Clock::now()-start<std::chrono::seconds(30),"one-shot baseline deadline");
        const auto old=apply(op,expected);uint64_t observed=old;
        switch(op.operation) {
        case 1:hsa_signal_store_screlease(signal,int64_t(op.operand));break;
        case 2:hsa_signal_add_scacq_screl(signal,int64_t(op.operand));break;
        case 3:hsa_signal_subtract_scacq_screl(signal,int64_t(op.operand));break;
        case 4:hsa_signal_and_scacq_screl(signal,int64_t(op.operand));break;
        case 5:hsa_signal_or_scacq_screl(signal,int64_t(op.operand));break;
        case 6:hsa_signal_xor_scacq_screl(signal,int64_t(op.operand));break;
        case 7:observed=uint64_t(hsa_signal_exchange_scacq_screl(signal,int64_t(op.operand)));break;
        case 8:observed=uint64_t(hsa_signal_cas_scacq_screl(signal,int64_t(op.compare),int64_t(op.operand)));break;
        case 9:observed=uint64_t(hsa_signal_load_scacquire(signal));break;
        }
        require(observed==old && uint64_t(hsa_signal_load_scacquire(signal))==expected,"one-shot operation semantics");
    }
    const auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
    std::printf("PASS current-HSA-one-shot operations=%zu seconds=%.6f ops/s=%.0f ns/op=%.1f (includes exact validation)\n",ops.size(),seconds,ops.size()/seconds,seconds*1e9/ops.size());
}
bool mutableWord(size_t i) {
    return i==MAC_MAILBOX_REQUEST_SEQUENCE || i==MAC_MAILBOX_COMPLETION_SEQUENCE ||
        i==MAC_MAILBOX_READY || i==MAC_MAILBOX_ABORT || i==MAC_MAILBOX_STATE || i==MAC_MAILBOX_COUNT ||
        (i>=MAC_MAILBOX_REQUESTS && i<MAC_MAILBOX_REQUESTS+MAC_MAILBOX_CAPACITY*MAC_MAILBOX_REQUEST_STRIDE) ||
        (i>=MAC_MAILBOX_RESULTS && i<MAC_MAILBOX_RESULTS+MAC_MAILBOX_CAPACITY);
}
void runMailbox(uint64_t *mailbox,uint64_t *args,hsa_queue_t *queue,hsa_signal_t target,hsa_signal_t done,
                uint64_t kernel,const std::vector<SignalOperation> &ops,size_t batch,mac_hsa::MailboxWait strategy) {
    std::fill(mailbox,mailbox+MAC_MAILBOX_WORDS,guard);
    for(size_t i=0;i<MAC_MAILBOX_WORDS;++i) if(mutableWord(i)) mailbox[i]=0;
    std::fill(args,args+MAC_MAILBOX_WORDS,guard);
    args[0]=reinterpret_cast<uintptr_t>(mailbox);args[1]=target.handle+offsetof(amd_signal_t,value);
    args[2]=(ops.size()+batch-1)/batch;args[3]=100000000;
    hsa_signal_store_screlease(target,0);hsa_signal_store_screlease(done,1);
    hsa_kernel_dispatch_packet_t packet{};
    packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
        (HSA_FENCE_SCOPE_SYSTEM<<HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
    packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
    packet.workgroup_size_x=packet.grid_size_x=32;
    packet.workgroup_size_y=packet.workgroup_size_z=packet.grid_size_y=packet.grid_size_z=1;
    packet.kernel_object=kernel;packet.kernarg_address=args;packet.completion_signal=done;
    const auto index=hsa_queue_add_write_index_relaxed(queue,1);require(index<queue->size,"queue capacity");
    auto *destination=static_cast<char *>(queue->base_address)+index*64;
    std::memcpy(destination+4,reinterpret_cast<char *>(&packet)+4,60);
    uint32_t header;std::memcpy(&header,&packet,4);
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(destination)).store(header,std::memory_order_release);
    hsa_signal_store_screlease(queue->doorbell_signal,int64_t(index));
    mac_hsa::SignalMailboxClient client(mailbox,strategy);
    const auto deadline=Clock::now()+std::chrono::seconds(30);
    bool valid=client.await(MAC_MAILBOX_READY,1,deadline)==mac_hsa::MailboxResult::Success;
    const auto start=Clock::now();uint64_t expected=0;size_t completed=0;
    std::vector<uint64_t> old(batch);
    while(valid && completed<ops.size()) {
        const size_t count=std::min(batch,ops.size()-completed);
        valid=client.execute({ops.data()+completed,count},{old.data(),count},deadline)==mac_hsa::MailboxResult::Success;
        if (!valid) break;
        for(size_t i=0;i<count;++i) if(old[i]!=apply(ops[completed+i],expected)) valid=false;
        completed+=count;
        if(queueError) valid=false;
    }
    const auto seconds=std::chrono::duration<double>(Clock::now()-start).count();
    if(!valid)client.cancel();
    const bool retired=hsa_signal_wait_scacquire(done,HSA_SIGNAL_CONDITION_EQ,0,3000000000ull,HSA_WAIT_STATE_BLOCKED)==0;
    bool guards=retired;
    if(retired) {
        for(size_t i=0;i<MAC_MAILBOX_WORDS;++i) if(!mutableWord(i) && mailbox[i]!=guard)guards=false;
        for(size_t i=4;i<MAC_MAILBOX_WORDS;++i) if(args[i]!=guard)guards=false;
        guards=guards && args[0]==reinterpret_cast<uintptr_t>(mailbox) &&
            args[1]==target.handle+offsetof(amd_signal_t,value) && args[2]==(ops.size()+batch-1)/batch && args[3]==100000000;
    }
    valid=valid && retired && guards && !queueError && client.load(MAC_MAILBOX_STATE)==1 &&
        client.load(MAC_MAILBOX_COMPLETION_SEQUENCE)==args[2] && uint64_t(hsa_signal_load_scacquire(target))==expected;
    std::printf("%s DMA-mailbox mode=%s batch=%zu operations=%zu/%zu seconds=%.6f ops/s=%.0f ns/op=%.1f polls=%llu sleeps=%llu completion=%s guards=%s\n",
        valid?"PASS":"FAIL",strategy==mac_hsa::MailboxWait::Active?"active":"hybrid",batch,completed,ops.size(),seconds,
        seconds ? completed/seconds : 0,completed ? seconds*1e9/completed : 0,
        (unsigned long long)client.polls,(unsigned long long)client.sleeps,retired?"retired":"pending",guards?"intact":"unverified/changed");
    require(valid,"DMA mailbox semantics/completion/guards");
}
}
int main(int argc,char **argv) {
    if(argc<3 || argc>4 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run build/tests/hsa-signal-mailbox.hsaco [operation-count=4096]\n",argv[0]);return 2;
    }
    size_t count=4096;
    if(argc==4) {char *end=nullptr;count=std::strtoull(argv[3],&end,10);if(!end || *end || !count || count>1000000)return 2;}
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
    if(bytes.empty())return 2;
    bool initialized=false,passed=false,safe=true;hsa_agent_t gpu{};
    hsa_code_object_reader_t reader{};hsa_executable_t executable{};hsa_queue_t *queue=nullptr;
    hsa_signal_t target{},done{};void *mailbox=nullptr,*arguments=nullptr;
    try {
        check(hsa_init(),"init");initialized=true;
        check(hsa_iterate_agents([](hsa_agent_t agent,void *out) {
            hsa_device_type_t type;const auto s=hsa_agent_get_info(agent,HSA_AGENT_INFO_DEVICE,&type);
            if(!s && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(out)->handle)*static_cast<hsa_agent_t *>(out)=agent;
            return s;
        },&gpu),"agents");require(gpu.handle,"GPU required");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver");require(info.driver_build>=190,"driver190 required");
        check(hsa_code_object_reader_create_from_memory(bytes.data(),bytes.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load");check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;uint32_t size=0;
        check(hsa_executable_get_symbol_by_name(executable,"signal_mailbox.kd",&gpu,&symbol),"symbol");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"kernel");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&size),"kernarg size");require(size==32,"mailbox ABI");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&size),"private size");require(!size,"mailbox scratch unsupported");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&size),"group size");require(!size,"mailbox LDS unsupported");
        check(mac_hsa_memory_allocate_shared(gpu,16384,&mailbox),"mailbox");check(mac_hsa_memory_allocate_shared(gpu,16384,&arguments),"arguments");
        uint32_t flags=0;check(mac_hsa_memory_get_sync_capabilities(gpu,mailbox,&flags),"synchronization policy");
        require((flags&MAC_HSA_SYNC_OWNERSHIP_TRANSFER) && !(flags&MAC_HSA_SYNC_NATIVE_CPU_GPU_RMW),"mapping policy mismatch");
        std::printf("sync capabilities=%#x; CPU map=default, GTT mapping; no native mixed RMW; CPU publishes requests, GPU owns target updates.\n",flags);
        std::puts("Active and hybrid waits always acquire/recheck completion sequence. IRQ wake unsupported by current mailbox transport; no fastest strategy claimed before measurement.");
        check(hsa_signal_create(0,1,&gpu,&target),"target signal");check(hsa_signal_create(1,1,&gpu,&done),"completion");
        check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t,hsa_queue_t *,void *){queueError=true;},nullptr,0,0,&queue),"queue");
        const auto operations=plan(count);baseline(target,operations);
        for(auto mode:{mac_hsa::MailboxWait::Active,mac_hsa::MailboxWait::Hybrid})for(size_t batch:{1,8,64})
            runMailbox(static_cast<uint64_t *>(mailbox),static_cast<uint64_t *>(arguments),queue,target,done,kernel,operations,batch,mode);
        passed=true;
    } catch(const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());}
    if(queue && hsa_queue_destroy(queue)) {safe=false;passed=false;}
    if(safe) {
        for(auto signal:{target,done})if(signal.handle && hsa_signal_destroy(signal))passed=false;
        for(auto p:{mailbox,arguments})if(p && hsa_memory_free(p))passed=false;
        if(executable.handle && hsa_executable_destroy(executable))passed=false;
    } else std::fputs("Queue unmap unconfirmed; retain mailbox/args/signal/code backing until recovery.\n",stderr);
    if(reader.handle && hsa_code_object_reader_destroy(reader))passed=false;
    if(initialized && hsa_shut_down())passed=false;
    return passed ? 0 : 1;
}
