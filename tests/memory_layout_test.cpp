#include "amdgpu_gmc_address.h"
#include "amdgpu_vram.h"
#include "amdgpu_pci_rebar.h"
#include <array>
#include <cassert>
#include <cstdio>

int main()
{
    using namespace amdgpu;
    VRAMBumpAllocator allocator;
    allocator.init(0x8000000000ull, 1<<20);
    VRAMAllocation first{}, second{}, bad{};
    assert(!allocator.alloc(UINT64_MAX,16384,&bad));
    assert(!allocator.alloc(4096,UINT64_MAX,&bad));
    assert(!allocator.alloc(4096,32769,&bad));
    assert(allocator.bytes_used()==0);
    assert(allocator.alloc(4096,16384,&first));
    assert(allocator.alloc(4096,65536,&second));
    assert((second.gpu_va & 65535)==0 && second.size==65536);
    allocator.free({UINT64_MAX-100,nullptr,200,16384});
    assert(allocator.bytes_used()==81920);
    allocator.free(first); allocator.free(first); // double free leaves accounting intact
    assert(allocator.bytes_used()==65536);
    allocator.free(second); assert(allocator.bytes_used()==0 && allocator.bytes_free()==1<<20);
    assert(allocator.alloc(1<<20,16384,&first)); // both ranges coalesced
    allocator.init(UINT64_MAX-10,100); assert(!allocator.is_inited());
    constexpr uint64_t GiB = 1ULL << 30;
    uint64_t start = ~0ULL;
    // R9700 framebuffer at 512 GiB: LOW places GART at zero, a valid VA.
    assert(gfx12_gart_location_low(512 * GiB, 544 * GiB - 1, GiB / 4, start));
    assert(start == 0);
    assert(gfx12_gart_location_low(0, 32 * GiB - 1, GiB / 4, start));
    assert(start == 32 * GiB);
    assert(gfx12_gart_location_low(0, 33 * GiB - 1, GiB / 4, start));
    assert(start == 36 * GiB);
    assert(!gfx12_gart_location_low(0, (1ULL << 47) - 1, GiB / 4, start));
    assert(!gfx12_gart_location_low(0, 32 * GiB, 0, start));
    assert(!gfx12_gart_location_low(0, 32 * GiB, 4095, start));
    assert(!gfx12_gart_location_low(32 * GiB, 0, GiB / 4, start));
    assert(!gfx12_gart_location_low(0, ~0ULL, GiB / 4, start));
    assert(gfx12_vram_walker_address(512 * GiB + 0x700000,
                                     512 * GiB, 0) == 0x700000);
    assert(gfx12_vram_walker_address(512 * GiB + 0x780000,
                                     512 * GiB, 4 * GiB) == 4 * GiB + 0x780000);

    std::array<uint32_t, 1024> cfg{};
    unsigned reads = 0;
    auto read = [&](uint64_t off, uint32_t &value) {
        assert(off < 4096 && !(off & 3));
        ++reads;
        value = cfg[off / 4];
        return true;
    };
    // Two entries, ordered BAR2 then BAR0. Count is not encoded minus one.
    cfg[0x100 / 4] = 0x10015;
    cfg[0x104 / 4] = (1u << 1) << 4; // BAR2: 2 MiB
    cfg[0x108 / 4] = 2 | (2u << 5) | (1u << 8);
    cfg[0x10c / 4] = ((1u << 8) | (1u << 15)) << 4; // 256 MiB or 32 GiB
    cfg[0x110 / 4] = 8u << 8;
    ReBARInfo result{};
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Found);
    assert(result.selectedBytes == GiB / 4);
    assert(result.supportedSizes == ((1u << 8) | (1u << 15)));
    assert(read_rebar(0x100, 2, read, result) == ReBARResult::Found);
    assert(result.selectedBytes == 2 * 1024 * 1024);
    assert(read_rebar(0x100, 5, read, result) == ReBARResult::NotFound);
    cfg[0x110 / 4] = 15u << 8;
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Found);
    assert(result.selectedBytes == 32 * GiB);
    cfg[0x110 / 4] = 31u << 8;
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Malformed);
    cfg[0x110 / 4] = (15u << 8) | 2; // duplicate BAR
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Malformed);
    cfg[0x108 / 4] &= ~(7u << 5);
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Malformed);
    cfg[0x108 / 4] |= 7u << 5;
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Malformed);
    cfg[0x100 / 4] = 0xffffffff;
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Malformed);
    cfg[0x100 / 4] = 0x20015;
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::UnsupportedVersion);
    cfg[0x100 / 4] = 0x10015 | (0x10cu << 20); // next capability overlaps entries
    cfg[0x108 / 4] = 2 | (2u << 5) | (1u << 8);
    assert(read_rebar(0x100, 0, read, result) == ReBARResult::Malformed);
    cfg[0xff4 / 4] = 0x10015;
    cfg[0xffc / 4] = 2u << 5; // two entries would run beyond extended config
    assert(read_rebar(0xff4, 0, read, result) == ReBARResult::Malformed);
    reads = 0;
    assert(read_rebar(0xff8, 0, read, result) == ReBARResult::Malformed);
    assert(read_rebar(0x101, 0, read, result) == ReBARResult::Malformed);
    assert(read_rebar(0x100, 6, read, result) == ReBARResult::Malformed);
    assert(reads == 0);
    auto failedRead = [](uint64_t, uint32_t &) { return false; };
    assert(read_rebar(0x100, 0, failedRead, result) == ReBARResult::ReadError);
    puts("memory layout and ReBAR tests passed");
}
