#include "mac_hsa.h"
#include <hsa/amd_hsa_signal.h>
#include <hsa/hsa_ext_amd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>

int main(int argc,char **argv) {
    if (argc!=2 || std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"Usage: %s --run (shared GPU signal API hardware test)\n",argv[0]);return 2;
    }
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (hsa_init()!=0) return 1;
    hsa_signal_t signal{};bool passed=false;
    do {
        const auto status=hsa_signal_create(5,0,nullptr,&signal);
        if (status) {std::fprintf(stderr,"GPU-visible signal creation failed: %#x\n",status);break;}
        auto *abi=reinterpret_cast<amd_signal_t *>(signal.handle);
        std::printf("Shared AMD signal=%p kind=%lld\n",abi,(long long)abi->kind);
        const auto verify=[&](int64_t expected,const char *step) {
            const auto actual=hsa_signal_load_scacquire(signal);
            const auto direct=std::atomic_ref<int64_t>(const_cast<int64_t &>(abi->value)).load(std::memory_order_acquire);
            if (actual==expected && direct==expected) return true;
            std::fprintf(stderr,"%s: HSA=%lld direct=%lld expected=%lld\n",step,(long long)actual,(long long)direct,(long long)expected);return false;
        };
        if (abi->kind!=AMD_SIGNAL_KIND_USER || !verify(5,"initial")) break;
        hsa_signal_add_scacq_screl(signal,7);if (!verify(12,"add")) break;
        hsa_signal_subtract_relaxed(signal,2);if (!verify(10,"subtract")) break;
        hsa_signal_or_screlease(signal,0x80);if (!verify(138,"or")) break;
        hsa_signal_and_scacquire(signal,0x7f);if (!verify(10,"and")) break;
        hsa_signal_xor_relaxed(signal,3);if (!verify(9,"xor")) break;
        if (hsa_signal_exchange_scacq_screl(signal,17)!=9 || !verify(17,"exchange")) break;
        if (hsa_signal_cas_scacq_screl(signal,99,42)!=17 || !verify(17,"failed CAS")) break;
        if (hsa_signal_cas_scacq_screl(signal,17,42)!=17 || !verify(42,"successful CAS")) break;
        hsa_signal_store_screlease(signal,-7);if (!verify(-7,"negative store")) break;
        hsa_signal_silent_store_relaxed(signal,INT64_MAX);if (!verify(INT64_MAX,"silent store")) break;
        hsa_signal_add_relaxed(signal,1);if (!verify(INT64_MIN,"signed wrap")) break;
        hsa_signal_store_relaxed(signal,0);
        if (hsa_signal_wait_scacquire(signal,HSA_SIGNAL_CONDITION_EQ,0,1000000000,HSA_WAIT_STATE_BLOCKED)!=0 || !verify(0,"wait")) break;
        passed=true;
    } while (false);
    if (signal.handle && hsa_signal_destroy(signal)!=0) passed=false;
    if (hsa_shut_down()!=0) passed=false;
    std::puts(passed ? "PASS: all GPU-backed HSA signal operations, return values, direct observation and cleanup" : "FAIL: GPU-backed HSA signal API");
    return passed ? 0 : 1;
}
