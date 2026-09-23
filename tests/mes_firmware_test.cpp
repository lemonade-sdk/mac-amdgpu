#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "amdgpu_ucode_psp.h"
using kern_return_t=int;
constexpr int kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnNotReady=2, kIOReturnIOError=3;
constexpr uint64_t kMacAMDGPUFwTypeFile_MES_UNI=515;
namespace amdgpu {
namespace PSPGfxFwType {
constexpr uint32_t CP_MES=33, CP_MES_DATA=34, CP_MES_KIQ=81, MES_KIQ_STACK=82;
}
enum class MESPipe { Sched, KIQ };
struct MESContext {
    bool sched_ucode_loaded=false, kiq_ucode_loaded=false;
    uint64_t entry[2]{};
};
static int mes_set_uc_start_addr(MESContext &mes, MESPipe pipe, uint64_t entry) {
    mes.entry[static_cast<unsigned>(pipe)]=entry;
    if (pipe==MESPipe::Sched) mes.sched_ucode_loaded=true;
    else mes.kiq_ucode_loaded=true;
    return 0;
}
}
struct Payload { uint32_t fw_type; };
struct Ivars { struct { amdgpu::MESContext mes; } bringup; };
struct Driver { Ivars *ivars; };
static int loadGate(Driver *driver, const uint8_t *bin, uint64_t fwSize,
    const std::vector<Payload> &payloads, size_t failedPayload=SIZE_MAX) {
    const auto fwType=kMacAMDGPUFwTypeFile_MES_UNI;
#include "mes_firmware_gate_under_test.inc"
}
int main() {
    std::ifstream file("firmware/gc_12_0_1_uni_mes.bin",std::ios::binary);
    assert(file);
    std::vector<uint8_t> bin{std::istreambuf_iterator<char>(file),{}};
    assert(bin.size()>=sizeof(amdgpu::mes_firmware_header_v1_0));
    amdgpu::mes_firmware_header_v1_0 hdr{};
    memcpy(&hdr,bin.data(),sizeof(hdr));
    const uint64_t entry=uint64_t(hdr.mes_uc_start_addr_lo)|(uint64_t(hdr.mes_uc_start_addr_hi)<<32);
    assert(entry && !(entry&3));
    Ivars state{}; Driver driver{&state};
    const std::vector<Payload> full{{33},{34},{81},{82}};
    for (size_t failure=0;failure<4;++failure) {
        // A previously successful load must not survive a failed replacement.
        state.bringup.mes={true,true,{entry,entry}};
        assert(loadGate(&driver,bin.data(),bin.size(),full,failure)==kIOReturnIOError);
        assert(!state.bringup.mes.sched_ucode_loaded && !state.bringup.mes.kiq_ucode_loaded);
    }
    for (size_t missing=0;missing<4;++missing) {
        auto partial=full; partial.erase(partial.begin()+missing);
        assert(loadGate(&driver,bin.data(),bin.size(),partial)==kIOReturnNotReady);
        assert(!state.bringup.mes.sched_ucode_loaded && !state.bringup.mes.kiq_ucode_loaded);
    }
    assert(loadGate(&driver,bin.data(),bin.size(),full)==0);
    assert(state.bringup.mes.sched_ucode_loaded && state.bringup.mes.kiq_ucode_loaded);
    assert(state.bringup.mes.entry[0]==entry && state.bringup.mes.entry[1]==entry);
    assert(loadGate(&driver,bin.data(),sizeof(hdr)-1,full)==kIOReturnBadArgument);
    assert(!state.bringup.mes.sched_ucode_loaded && !state.bringup.mes.kiq_ucode_loaded);
    for (unsigned invalid: {0u,1u,3u}) {
        hdr.mes_uc_start_addr_lo=invalid; hdr.mes_uc_start_addr_hi=0;
        memcpy(bin.data(),&hdr,sizeof(hdr));
        assert(loadGate(&driver,bin.data(),bin.size(),full)==kIOReturnBadArgument);
        assert(!state.bringup.mes.sched_ucode_loaded && !state.bringup.mes.kiq_ucode_loaded);
    }
    puts("MES firmware gate: real entry address, four PSP acknowledgements, failed/missing payloads and malformed entry rejection pass");
}
