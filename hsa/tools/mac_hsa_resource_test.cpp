#include "mac_hsa.h"
#include <hsa/hsa_ext_amd.h>
#include <hsa/amd_hsa_queue.h>
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
        std::fprintf(stderr,"Usage: %s --run build/tests/hsa-resource-object.hsaco\n",argv[0]);return 2;
    }
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
    if (bytes.empty()) return 1;
    bool initialized=false,passed=false,safe=true;
    hsa_agent_t gpu{},cpu{};hsa_code_object_reader_t reader{};hsa_executable_t executable{};
    hsa_queue_t *queues[2]{};hsa_signal_t done[2]{};void *data=nullptr,*arguments=nullptr;
    try {
        check(hsa_init(),"initialize runtime");initialized=true;
        struct Agents {hsa_agent_t *gpu,*cpu;} agents{&gpu,&cpu};
        check(hsa_iterate_agents([](hsa_agent_t a,void *opaque) {
            auto &out=*static_cast<Agents *>(opaque);hsa_device_type_t type;
            const auto status=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
            if (!status && type==HSA_DEVICE_TYPE_GPU && !out.gpu->handle) *out.gpu=a;
            if (!status && type==HSA_DEVICE_TYPE_CPU && !out.cpu->handle) *out.cpu=a;
            return status;
        },&agents),"enumerate");require(gpu.handle && cpu.handle,"missing GPU or CPU");
        mac_hsa_device_info_t info{};check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver build");
        require(info.driver_build>=189,"Install driver 189 before scratch/LDS validation");
        check(hsa_code_object_reader_create_from_memory(bytes.data(),bytes.size(),&reader),"reader");
        check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&executable),"executable");
        check(hsa_executable_load_agent_code_object(executable,gpu,reader,nullptr,nullptr),"load kernel");
        check(hsa_executable_freeze(executable,nullptr),"freeze");
        hsa_executable_symbol_t symbol{};uint64_t kernel=0;uint32_t priv=0,group=0,kernarg=0;
        check(hsa_executable_get_symbol_by_name(executable,"scratch_lds.kd",&gpu,&symbol),"symbol");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT,&kernel),"descriptor");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE,&priv),"private size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE,&group),"group size");
        check(hsa_executable_symbol_get_info(symbol,HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE,&kernarg),"kernarg size");
        require(priv>=256 && priv<4096 && group==128 && kernarg==12,"resource shader metadata");
        struct PoolSelection {hsa_agent_t gpu;hsa_amd_memory_pool_t pool{};} selection{gpu};
        check(hsa_amd_agent_iterate_memory_pools(cpu,[](hsa_amd_memory_pool_t pool,void *opaque) {
            auto &out=*static_cast<PoolSelection *>(opaque);uint32_t flags=0;
            auto status=hsa_amd_memory_pool_get_info(pool,HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS,&flags);
            if (status) return status;
            if (flags!=(HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED|HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_KERNARG_INIT))
                return HSA_STATUS_SUCCESS;
            hsa_amd_memory_pool_access_t access;
            status=hsa_amd_agent_memory_pool_get_info(out.gpu,pool,HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS,&access);
            if (!status && access!=HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED) out.pool=pool;
            return status;
        },&selection),"host kernarg pool");require(selection.pool.handle,"missing public shared pool");
        check(hsa_amd_memory_pool_allocate(selection.pool,16384,0,&data),"data");
        check(hsa_amd_memory_pool_allocate(selection.pool,16384,0,&arguments),"arguments");
        for (auto *pointer:{data,arguments}) check(hsa_amd_agents_allow_access(1,&gpu,nullptr,pointer),"allow GPU access");
        // An impossible explicit request must fail before publishing a queue.
        hsa_queue_t *invalid=nullptr;
        require(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,262129,0,&invalid)!=0 && !invalid,"private limit rejection");
        require(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,nullptr,nullptr,0,65537,&invalid)!=0 && !invalid,"LDS limit rejection");
        for (unsigned q=0;q<2;++q) {
            check(hsa_signal_create(1,0,nullptr,&done[q]),"completion");
            check(hsa_queue_create(gpu,64,HSA_QUEUE_TYPE_MULTI,[](hsa_status_t status,hsa_queue_t *,void *) {
                std::fprintf(stderr,"queue callback: %#x\n",status);++errors;
            },nullptr,q ? 64 : 0,65536,&queues[q]),"resource queue");
            const auto &metadata=*reinterpret_cast<amd_queue_t *>(queues[q]);
            require(metadata.group_segment_aperture_base_hi==0x10000 && metadata.private_segment_aperture_base_hi==0x20000,"flat apertures");
            require(metadata.scratch_wave64_lane_byte_size==(q ? 64u : 0u),"initial scratch request");
        }
        std::vector<uint32_t> expected(4096);uint64_t retainedScratch[2]{};
        for (unsigned pass=0;pass<3;++pass) {
            const uint32_t packetPrivate=pass ? 4096 : priv;
            const uint32_t guard=0xd15ea500u+pass;
            std::fill(expected.begin(),expected.end(),guard);
            std::memcpy(data,expected.data(),16384);std::memset(arguments,0xa5,16384);
            for (unsigned q=0;q<2;++q) {
                const uint32_t seed=0x12340000u+pass*1024+q*32;
                const uint64_t address=reinterpret_cast<uintptr_t>(data)+(64+q*1024)*4;
                auto *args=static_cast<uint8_t *>(arguments)+q*16;
                std::memcpy(args,&address,8);std::memcpy(args+8,&seed,4);
                for (unsigned i=0;i<512;++i) {
                    const auto lane=i&31,neighbor=(lane+1)&31;
                    expected[64+q*1024+i]=2*seed+neighbor+((neighbor+7)&63)+lane+((lane+11)&63);
                }
                hsa_signal_store_screlease(done[q],1);
                hsa_kernel_dispatch_packet_t packet{};
                packet.header=HSA_PACKET_TYPE_KERNEL_DISPATCH|scopes|(1<<HSA_PACKET_HEADER_BARRIER);
                packet.setup=1<<HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
                packet.workgroup_size_x=32;packet.workgroup_size_y=packet.workgroup_size_z=1;
                packet.grid_size_x=512;packet.grid_size_y=packet.grid_size_z=1;
                packet.private_segment_size=packetPrivate;packet.group_segment_size=group;
                packet.kernel_object=kernel;packet.kernarg_address=args;packet.completion_signal=done[q];
                submit(queues[q],&packet);
            }
            for (unsigned q=0;q<2;++q) {
                wait(done[q],"scratch/LDS completion timeout");drain(queues[q]);
                const auto &metadata=*reinterpret_cast<amd_queue_t *>(queues[q]);
                require(metadata.scratch_wave64_lane_byte_size>=packetPrivate && metadata.compute_tmpring_size,"scratch service growth");
                require(metadata.scratch_backing_memory_location && metadata.scratch_resource_descriptor[2],"scratch GPU backing");
                if (pass==2) require(metadata.scratch_backing_memory_location==retainedScratch[q],"scratch reuse");
                retainedScratch[q]=metadata.scratch_backing_memory_location;
                std::printf("queue%u pass%u private=%u backing=%#llx tmpring=%#x\n",q,pass,
                    metadata.scratch_wave64_lane_byte_size,(unsigned long long)retainedScratch[q],metadata.compute_tmpring_size);
            }
            require(!std::memcmp(data,expected.data(),16384),"scratch/LDS result or guard mismatch");
            const auto *args=static_cast<const uint8_t *>(arguments);
            for (unsigned i=0;i<16384;++i)
                if (!((i<12)||(i>=16 && i<28))) require(args[i]==0xa5,"kernarg guard corruption");
            std::printf("PASS: both queues, pass%u, all16384 data bytes and kernarg guards\n",pass);
        }
        require(retainedScratch[0]!=retainedScratch[1],"queues share scratch backing");passed=true;
    } catch (const std::exception &e) {std::fprintf(stderr,"FAIL: %s\n",e.what());}
    for (auto *queue:queues) if (queue && hsa_queue_destroy(queue)) {safe=false;passed=false;}
    if (safe) {
        for (auto signal:done) if (signal.handle && hsa_signal_destroy(signal)) passed=false;
        for (auto *pointer:{data,arguments}) if (pointer && hsa_memory_free(pointer)) passed=false;
        if (executable.handle && hsa_executable_destroy(executable)) passed=false;
    } else std::fputs("Queue removal unconfirmed; backing retained for reset\n",stderr);
    if (reader.handle && hsa_code_object_reader_destroy(reader)) passed=false;
    if (initialized && hsa_shut_down()) passed=false;
    std::puts(passed ? "PASS: persistent scratch growth/reuse, LDS and independent queue cleanup" : "FAIL: resource test");
    return passed ? 0 : 1;
}
