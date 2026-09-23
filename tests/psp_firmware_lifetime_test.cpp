#include "amdgpu_ucode_psp.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

// Only the DriverKit allocation boundary is mocked. The runner extracts
// the production SOS parser/snapshot functions from psp_v14_0.cpp.
using kern_return_t = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnUnsupported,
       kIOReturnNoMemory };
constexpr unsigned kIOMemoryDirectionOutIn = 3;
constexpr unsigned kASPageSize = 16384;
struct IOAddressSegment { uint64_t address, length; };
struct IOBufferMemoryDescriptor {
    std::vector<uint8_t> bytes;
    static inline unsigned live = 0;
    static inline bool failAllocation = false;
    static kern_return_t Create(unsigned, uint64_t size, unsigned,
                                IOBufferMemoryDescriptor **out) {
        if (failAllocation) return kIOReturnNoMemory;
        *out = new IOBufferMemoryDescriptor;
        (*out)->bytes.resize(size);
        ++live;
        return kIOReturnSuccess;
    }
    kern_return_t GetAddressRange(IOAddressSegment *out) {
        *out = {reinterpret_cast<uint64_t>(bytes.data()), bytes.size()};
        return kIOReturnSuccess;
    }
    void release() { --live; delete this; }
};

namespace amdgpu {
struct PSPContext {
    struct PSPSubBin {
        const uint8_t *start_addr = nullptr;
        uint64_t size_bytes = 0;
        uint32_t fw_version = 0;
    };
    PSPSubBin sos, sys, kdb, toc, spl, rl, soc_drv, intf_drv, dbg_drv,
        ras_drv, ipkeymgr_drv, spdm_drv, sys_drv_aux, sos_aux;
    IOBufferMemoryDescriptor *sosPackageBuffer = nullptr;
    const uint8_t *sos_fw_blob = nullptr;
    uint64_t sos_fw_blob_size = 0;
    bool firmwareLoadComplete = false;
};
#define PSP_LOG(...) do {} while (false)
#include "psp_snapshot_under_test.inc"
}

static std::vector<uint8_t> read(const char *name)
{
    std::ifstream f(name, std::ios::binary);
    assert(f.good());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main()
{
    using namespace amdgpu;
    const auto sos = read("firmware/psp_14_0_3_sos.bin");
    std::vector<uint8_t> upload(2 * 1024 * 1024);
    std::memcpy(upload.data(), sos.data(), sos.size());
    PSPContext psp{};
    assert(psp_parse_sos_microcode(psp, upload.data(), sos.size()) == kIOReturnSuccess);
    assert(psp.rl.size_bytes == 1712);
    assert(psp.rl.start_addr - psp.sos_fw_blob == 0x69f00);
    const std::vector<uint8_t> expected(psp.rl.start_addr,
                                       psp.rl.start_addr + psp.rl.size_bytes);
    assert(IOBufferMemoryDescriptor::live == 1);

    // Replay actual Host load order; ME/MEC/MES overwrite the old RL view.
    for (const char *path : {
        "firmware/psp_14_0_3_ta.bin", "firmware/smu_14_0_3.bin",
        "firmware/sdma_7_0_1.bin", "firmware/gc_12_0_1_pfp.bin",
        "firmware/gc_12_0_1_me.bin", "firmware/gc_12_0_1_mec.bin",
        "firmware/gc_12_0_1_uni_mes.bin", "firmware/gc_12_0_1_imu.bin",
        "firmware/gc_12_0_1_rlc.bin"}) {
        const auto next = read(path);
        assert(next.size() <= upload.size());
        std::memcpy(upload.data(), next.data(), next.size());
        assert(std::memcmp(psp.rl.start_addr, expected.data(), expected.size()) == 0);
    }
    unsigned overwritten = 0;
    for (unsigned i = 0; i < expected.size(); ++i)
        overwritten += upload[0x69f00 + i] != expected[i];
    assert(overwritten == 1067);

    // Destroy the caller's storage completely; private descriptors remain valid.
    upload.clear();
    upload.shrink_to_fit();
    assert(std::memcmp(psp.rl.start_addr, expected.data(), expected.size()) == 0);
    const auto *retained = psp.sos_fw_blob;
    auto reject = [&](std::vector<uint8_t> bad) {
        assert(psp_parse_sos_microcode(psp, bad.data(), bad.size()) != kIOReturnSuccess);
        assert(psp.sos_fw_blob == retained);
        assert(IOBufferMemoryDescriptor::live == 1);
        assert(std::memcmp(psp.rl.start_addr, expected.data(), expected.size()) == 0);
    };
    reject(std::vector<uint8_t>(sos.begin(), sos.begin() + 32));
    auto malformed = sos;
    const uint32_t excessive = 64;
    std::memcpy(malformed.data() + 32, &excessive, 4);
    reject(malformed);
    malformed = sos;
    const uint32_t beyond = UINT32_MAX;
    std::memcpy(malformed.data() + 36 + 8, &beyond, 4);
    reject(malformed);
    IOBufferMemoryDescriptor::failAllocation = true;
    reject(sos);
    IOBufferMemoryDescriptor::failAllocation = false;
    assert(psp_parse_sos_microcode(psp, sos.data(), sos.size()) == kIOReturnSuccess);
    assert(IOBufferMemoryDescriptor::live == 1);
    psp.sosPackageBuffer->release();
    assert(IOBufferMemoryDescriptor::live == 0);
    std::puts("PSP snapshot lifetime passed: production parser preserves RL across real firmware uploads; old view loses 1067/1712 bytes");
}
