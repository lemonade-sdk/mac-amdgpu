#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "amdgpu_compute_packets.h"
#include "amdgpu_vram.h"
#include "amdgpu_ip.h"
#include "../upstream/linux/drivers/gpu/drm/amd/amdgpu/nvd.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/soc24_enum.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h"
#include "../upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h"
using kern_return_t = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBusy = 1, kIOReturnNotReady = 2,
    kIOReturnUnsupported = 3, kIOReturnNoMemory = 4, kIOReturnBadArgument = 5,
    kIOReturnIOError = 6, kIOReturnNoSpace = 7, kIOReturnTimeout = 8,
    kIOReturnNotAttached = 9;
static int mode;
static unsigned reads, writes, submits, failWrite;
static std::vector<uint32_t> stream;
struct PCI {
    uint32_t memory[16384 / 4]{};
    void MemoryWrite32(unsigned, uint64_t off, uint32_t val) {
        assert(off / 4 < 4096);
        if (++writes != failWrite) memory[off / 4] = val;
    }
    void MemoryRead32(unsigned, uint64_t off, uint32_t *val) {
        assert(off / 4 < 4096); ++reads; *val = memory[off / 4];
    }
    void MemoryRead64(unsigned, uint64_t off, uint64_t *val) {
        assert(off + 8 <= sizeof(memory)); std::memcpy(val, reinterpret_cast<char *>(memory) + off, 8);
    }
};
namespace amdgpu {
struct DeviceContext { PCI *pci; uint64_t bar0Size = 16384; unsigned bar0MemIndex = 0; IPBaseTable ip{}; };
struct GMCContext { uint64_t vram_start = 0x8000000000; VRAMBumpAllocator vram_alloc; };
struct CPContext { bool inited = true, ringReady = true; uint64_t wptr = 0; };
struct GFXConfig {
    unsigned num_active_cus = 64, max_shader_engines = 4, max_sh_per_se = 1;
    uint32_t active_cu_bitmap[4][2] = {{0xffff, 0}, {0xffff, 0}, {0xffff, 0}, {0xffff, 0}};
};
#include "compute_context.inc"
static void amdgpu_hdp_flush(DeviceContext &) {}
static uint32_t cp_ring_write(CPContext &cp, const uint32_t *p, uint32_t n) {
    if (mode == 1) return 0;
    stream.assign(p, p+n); cp.wptr += n; return n;
}
static std::map<uint32_t, uint32_t> decode(const std::vector<uint32_t> &p) {
    std::map<uint32_t, uint32_t> sh;
    unsigned dispatches = 0, syncs = 0, partials = 0;
    for (unsigned i = 0; i < p.size();) {
        assert(CP_PACKET_GET_TYPE(p[i]) == PACKET_TYPE3);
        const unsigned n = CP_PACKET_GET_COUNT(p[i]) + 2;
        assert(i + n <= p.size());
        switch (CP_PACKET3_GET_OPCODE(p[i])) {
        case PACKET3_SET_SH_REG:
            assert(n == 3 && (p[i] & 2) && !dispatches);
            assert(sh.emplace((p[i+1] + 0x2c00) * 4, p[i+2]).second); break;
        case PACKET3_DISPATCH_DIRECT:
            assert(n == 5 && (p[i] & 2) && !dispatches && syncs == 1);
            assert(p[i+1] == 1 && p[i+2] == 1 && p[i+3] == 1);
            assert(p[i+4] == (COMPUTE_DISPATCH_INITIATOR__COMPUTE_SHADER_EN_MASK |
                COMPUTE_DISPATCH_INITIATOR__FORCE_START_AT_000_MASK |
                COMPUTE_DISPATCH_INITIATOR__ORDER_MODE_MASK |
                COMPUTE_DISPATCH_INITIATOR__CS_W32_EN_MASK));
            ++dispatches; break;
        case PACKET3_EVENT_WRITE:
            assert(n == 2 && dispatches == 1 && syncs == 1);
            assert(p[i+1] == (CS_PARTIAL_FLUSH | (4 << 8))); ++partials; break;
        case PACKET3_ACQUIRE_MEM:
            assert(n == 8 && p[i+1] == 0 && p[i+2] == UINT32_MAX && p[i+3] == 0xffffff);
            assert(!p[i+4] && !p[i+5] && p[i+6] == 10);
            assert(p[i+7] == (PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_INV(1) |
                PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_WB(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_INV(1) |
                PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_WB(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GL1_INV(1) |
                PACKET3_ACQUIRE_MEM_GCR_CNTL_GLV_INV(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GLK_INV(1) |
                PACKET3_ACQUIRE_MEM_GCR_CNTL_GLI_INV(1)));
            if (syncs) assert(dispatches == 1 && partials == 1);
            ++syncs; break;
        default: assert(false);
        }
        i += n;
    }
    assert(dispatches == 1 && syncs == 2 && partials == 1);
    return sh;
}
static int cp_submit_eop_test(DeviceContext &dev, CPContext &, uint64_t timeout, uint32_t *fence) {
    assert(timeout == 100000); ++submits;
    if (mode == 2) return kIOReturnTimeout;
    auto sh = decode(stream);
    assert(sh.at(0xb900) == 4096 && sh.at(0xb904) == 0x80);
    if (mode != 3) { // Model successful shader stores; mode 3 is fence-only completion.
        for (unsigned i = 0; i < 32; ++i)
            dev.pci->memory[1024 + 64 + i] = dev.pci->memory[1024 + i] + sh.at(0xb908);
    }
    if (mode == 4) dev.pci->memory[1024 + 63] ^= 1; // underrun guard
    if (mode == 5) dev.pci->memory[1024 + 96] ^= 1; // overrun guard
    if (mode == 6) dev.pci->memory[1024] ^= 1; // clobbered input
    *fence = 17;
    return 0;
}
}
#include "amdgpu_vram_io.h"
namespace amdgpu {
#include "compute_test.inc"
}
int main() {
    using namespace amdgpu;
    const uint32_t masks[4] = {0xffff, 0x33ff, 0, 0xffff};
    uint32_t packets[kComputeSmokePacketCapacity];
    const auto n = compute_smoke_packets(packets, 0x8001800000, 0x8001801000, 0xfeedcafe, masks);
    assert(n && n <= kComputeSmokePacketCapacity);
    auto regs = decode({packets, packets+n});
    // Linux GC register offsets are relative to GC segment base 0x1260.
    auto reg = [](unsigned offset) { return (offset + 0x1260) * 4; };
    assert(regs.at(reg(regCOMPUTE_PGM_LO)) == 0x80018000);
    assert(regs.at(reg(regCOMPUTE_PGM_HI)) == 0);
    assert(regs.at(reg(regCOMPUTE_USER_DATA_0)) == 0x01801000);
    assert(regs.at(reg(regCOMPUTE_USER_DATA_1)) == 0x80);
    assert(regs.at(reg(regCOMPUTE_USER_DATA_2)) == 0xfeedcafe);
    assert(regs.at(reg(regCOMPUTE_PGM_RSRC1)) == (0xc0 << COMPUTE_PGM_RSRC1__FLOAT_MODE__SHIFT));
    assert(regs.at(reg(regCOMPUTE_PGM_RSRC2)) == (3 << COMPUTE_PGM_RSRC2__USER_SGPR__SHIFT));
    assert(regs.at(reg(regCOMPUTE_NUM_THREAD_X)) == 32);
    assert(regs.at(reg(regCOMPUTE_STATIC_THREAD_MGMT_SE1)) == masks[1]);
    assert(!compute_smoke_packets(packets, 1, 0, 0, masks));
    assert(!compute_smoke_packets(packets, 1ull << 48, 0, 0, masks));
    assert(!compute_smoke_packets(packets, 0, UINT64_MAX - 3, 0, masks));
    assert(compute_smoke_packets(packets, 0, 0, 0, masks)); // VA zero is valid.
    const uint32_t disabled[4]{};
    assert(!compute_smoke_packets(packets, 0, 0, 0, disabled));
    for (mode = 0; mode <= 6; ++mode) {
        PCI pci{}; DeviceContext dev{&pci}; GMCContext gmc; CPContext cp; GFXConfig gfx;
        dev.ip.version[0] = {12, 0, 1}; gmc.vram_alloc.init(gmc.vram_start, 16384);
        ComputeTest test{}; ComputeTestResult result{};
        const auto r = compute_test(dev, gmc, cp, gfx, test, 0xabcd7654, result);
        if (!mode) {
            assert(r == 0 && !test.active && result.stage == 5 && !result.mismatches);
            assert(result.fence == 17 && gmc.vram_alloc.bytes_used() == 0);
            assert(compute_test(dev, gmc, cp, gfx, test, 0x13572468, result) == 0);
        } else {
            assert(r != 0 && test.active && gmc.vram_alloc.bytes_used() == 16384);
            if (mode == 3) assert(result.mismatches == 32 && result.firstMismatch == 256);
            if (mode >= 4) assert(result.mismatches == 1);
            const auto oldWrites = writes, oldSubmits = submits;
            assert(compute_test(dev, gmc, cp, gfx, test, 0, result) == kIOReturnBusy);
            assert(writes == oldWrites && submits == oldSubmits);
        }
    }
    mode = 0;
    for (auto failure : {1u, 4097u, 4108u}) {
        PCI pci{}; DeviceContext dev{&pci}; GMCContext gmc; CPContext cp; GFXConfig gfx;
        std::memset(pci.memory, 0xff, sizeof(pci.memory));
        dev.ip.version[0] = {12, 0, 1}; gmc.vram_alloc.init(gmc.vram_start, 16384);
        ComputeTest test{}; ComputeTestResult result{};
        writes = 0; failWrite = failure;
        const auto before = submits;
        assert(compute_test(dev, gmc, cp, gfx, test, 5, result) == kIOReturnIOError);
        assert(test.active && result.stage == 2 && before == submits);
    }
    failWrite = 0;
    PCI pci{}; DeviceContext dev{&pci}; GMCContext gmc; CPContext cp; GFXConfig gfx;
    ComputeTest test{}; ComputeTestResult result{};
    gmc.vram_alloc.init(gmc.vram_start, 16384);
    assert(compute_test(dev, gmc, cp, gfx, test, 0, result) == kIOReturnUnsupported);
    assert(!test.active && !gmc.vram_alloc.bytes_used());
    dev.ip.version[0] = {12, 0, 1}; cp.ringReady = false;
    assert(compute_test(dev, gmc, cp, gfx, test, 0, result) == kIOReturnNotReady);
    assert(!test.active);
    puts("Compute packet oracle, shader bytes, data/guard verification and failure retention passed");
}
