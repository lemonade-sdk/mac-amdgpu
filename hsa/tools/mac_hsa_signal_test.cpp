#include "mac_hsa.h"
#include <hsa/hsa_ext_amd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

int main(int argc,char **argv) {
    if (argc!=3 || (std::strcmp(argv[1],"--observe") && std::strcmp(argv[1],"--publish") && std::strcmp(argv[1],"--concurrent"))) {
        std::fprintf(stderr,"Usage: %s --observe|--publish|--concurrent build/tests/hsa-signal-object.hsaco\n",argv[0]); return 2;
    }
    const bool publish=!std::strcmp(argv[1],"--publish"), concurrent=!std::strcmp(argv[1],"--concurrent");
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[2],std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
    const auto check=[](hsa_status_t status,const char *step) {
        if (!status) return true;
        std::fprintf(stderr,"%s: HSA status %#x\n",step,status);return false;
    };
    if (bytes.empty() || !check(hsa_init(),"runtime init")) return 1;
    hsa_agent_t gpu{}; hsa_code_object_reader_t reader{};hsa_executable_t exe{};void *memory=nullptr;
    bool passed=false;
    do {
        if (!check(hsa_iterate_agents([](hsa_agent_t a,void *context) {
            hsa_device_type_t type;auto status=hsa_agent_get_info(a,HSA_AGENT_INFO_DEVICE,&type);
            if (!status && type==HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t *>(context)->handle)
                *static_cast<hsa_agent_t *>(context)=a;
            return status;
        },&gpu),"GPU enumeration") || !gpu.handle) break;
        mac_hsa_device_info_t info{};
        if (!check(mac_hsa_agent_get_driver_info(gpu,&info,sizeof(info)),"driver build") || info.driver_build<184) break;
        if (!check(hsa_code_object_reader_create_from_memory(bytes.data(),bytes.size(),&reader),"reader") ||
            !check(hsa_executable_create_alt(HSA_PROFILE_BASE,HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,nullptr,&exe),"executable") ||
            !check(hsa_executable_load_agent_code_object(exe,gpu,reader,nullptr,nullptr),"load kernel") ||
            !check(hsa_executable_freeze(exe,nullptr),"freeze")) break;
        hsa_executable_symbol_t symbol{};
        if (!check(hsa_executable_get_symbol_by_name(exe,publish ? "signal_publish.kd" : "signal_add.kd",&gpu,&symbol),"symbol") ||
            !check(mac_hsa_memory_allocate_shared(gpu,16384,&memory),"shared signal storage")) break;
        std::memset(memory,0,16384);
        auto *words=static_cast<int64_t *>(memory);
        std::atomic_ref<int64_t> value(words[0]);
        const uint32_t iterations=publish ? 1024 : 128;
        uint8_t args[12];const uint64_t address=reinterpret_cast<uintptr_t>(memory);
        std::memcpy(args,&address,8);std::memcpy(args+8,&iterations,4);
        std::atomic<bool> observing{false},done{false},bad{false};
        uint64_t cpuAdds=0,observations=0,transitions=0;
        std::thread observer([&] {
            int64_t previous=0;observing.store(true,std::memory_order_release);
            while (!done.load(std::memory_order_acquire)) {
                int64_t current;
                if (concurrent) { current=value.fetch_add(1,std::memory_order_acq_rel)+1; ++cpuAdds; }
                else current=value.load(std::memory_order_acquire);
                ++observations;if (current!=previous) ++transitions;
                if (current<previous || (publish && (current<0 || current>iterations))) bad=true;
                if (publish && current>0 && current<=iterations && words[64+current]!=int64_t(0x6143210000000000ull+current)) bad=true;
                previous=current;
            }
        });
        while (!observing.load(std::memory_order_acquire)) std::this_thread::yield();
        const uint32_t groups[3]={1,1,1},threads[3]={32,1,1};const void *refs[]={memory};uint64_t completion=UINT64_MAX;
        const auto result=mac_hsa_executable_dispatch_aql(symbol,args,12,groups,threads,refs,1,&completion);
        done.store(true,std::memory_order_release);observer.join();
        const auto observed=value.load(std::memory_order_acquire);
        const auto expected=cpuAdds+iterations*(publish ? 1ull : 32ull);
        std::printf("Shared signal %p: observed=%lld expected=%llu CPU additions=%llu samples=%llu transitions=%llu ordering_error=%d\n",
            memory,(long long)observed,(unsigned long long)expected,(unsigned long long)cpuAdds,
            (unsigned long long)observations,(unsigned long long)transitions,bad.load());
        if (!check(result,"AQL signal shader") || completion || observed!=int64_t(expected) || bad || !observations || !transitions) break;
        bool guards=true;
        for (unsigned i=1;i<2048;++i) {
            if (publish ? (i>=65 && i<=64+iterations) : (i>=64 && i<96)) continue;
            if (words[i]) guards=false;
        }
        if (!guards) {std::fputs("Signal allocation guards changed\n",stderr);break;}
        if (publish) for (uint32_t i=1;i<=iterations;++i)
            if (words[64+i]!=int64_t(0x6143210000000000ull+i)) guards=false;
        passed=guards;
    } while (false);
    if (memory && !check(hsa_memory_free(memory),"free signal storage")) passed=false;
    if (exe.handle && !check(hsa_executable_destroy(exe),"destroy executable")) passed=false;
    if (reader.handle && !check(hsa_code_object_reader_destroy(reader),"destroy reader")) passed=false;
    if (!check(hsa_shut_down(),"shutdown")) passed=false;
    std::puts(passed ? "PASS: signal shader, live host observation, result and guards verified" : "FAIL: shared signal behavior remains unverified");
    return passed ? 0 : 1;
}
