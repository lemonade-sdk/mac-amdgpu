#include <cassert>
#include <cstdio>
#include <vector>
#include "amdgpu_gart_allocator.h"
using namespace amdgpu;

int main() {
    GARTApertureAllocator allocator;
    GARTReservation out{};
    assert(!allocator.reserve(4096, 4096, out));
    assert(!allocator.init(1, 4096));
    assert(!allocator.init(0, 0));
    assert(!allocator.init(UINT64_MAX - 4095, 4096));
    assert(allocator.init(4096, 256 * 4096));
    assert(!allocator.reserve(1, 4096, out));
    assert(!allocator.reserve(4096, 4097, out));
    assert(!allocator.reserve(UINT64_MAX, 4096, out));
    assert(allocator.reserve(4096, 16384, out));
    assert(out.offset == 12288 && ((allocator.base + out.offset) & 16383) == 0);
    assert(allocator.init(4096, 256 * 4096));
    assert(!allocator.init(0, 256 * 4096));
    assert(allocator.owns(out.id, out.offset, out.size));
    assert(!allocator.release(out.id, out.offset, out.size + 4096));
    assert(allocator.release(out.id, out.offset, out.size));
    assert(!allocator.release(out.id, out.offset, out.size));

    // Independent page occupancy model checks first-fit placement, alignment,
    // overlap and reusable holes under mixed allocation/free order.
    bool occupied[256]{};
    std::vector<GARTReservation> live;
    uint32_t random = 0x4d574d41;
    auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    for (unsigned step = 0; step < 10000; ++step) {
        if (!live.empty() && next() % 3 == 0) {
            const unsigned index = next() % live.size();
            auto old = live[index];
            assert(allocator.release(old.id, old.offset, old.size));
            for (unsigned p = old.offset / 4096; p < (old.offset + old.size) / 4096; ++p)
                occupied[p] = false;
            live.erase(live.begin() + index);
            assert(!allocator.owns(old.id, old.offset, old.size));
        } else {
            const unsigned pages = 1 + next() % 12;
            const unsigned alignmentPages = 1u << (next() % 5);
            int expected = -1;
            if (live.size() < GARTApertureAllocator::capacity) {
                for (unsigned p = 0; p + pages <= 256; ++p) {
                    if ((p + 1) % alignmentPages) continue;
                    bool free = true;
                    for (unsigned q = p; q < p + pages; ++q) free &= !occupied[q];
                    if (free) { expected = p; break; }
                }
            }
            const bool success = allocator.reserve(pages * 4096, alignmentPages * 4096, out);
            assert(success == (expected >= 0));
            if (success) {
                assert(out.offset == uint64_t(expected) * 4096);
                assert(allocator.owns(out.id, out.offset, out.size));
                for (unsigned p = expected; p < unsigned(expected) + pages; ++p) occupied[p] = true;
                live.push_back(out);
            }
        }
        uint64_t used = 0;
        for (bool page : occupied) if (page) used += 4096;
        assert(used == allocator.bytes_used());
    }

    allocator = {};
    assert(allocator.init(0, 1024 * 4096));
    for (unsigned i = 0; i < GARTApertureAllocator::capacity; ++i)
        assert(allocator.reserve(4096, 4096, out));
    assert(!allocator.reserve(4096, 4096, out)); // metadata exhaustion
    auto stale = out;
    assert(allocator.release(out.id, out.offset, out.size));
    assert(allocator.reserve(4096, 4096, out));
    assert(out.offset == stale.offset && out.id != stale.id);
    assert(!allocator.release(stale.id, stale.offset, stale.size));
    allocator = {};
    assert(allocator.init(UINT64_MAX - 8191, 4096));
    assert(!allocator.reserve(4096, 16384, out)); // alignment would wrap
    assert(allocator.reserve(4096, 4096, out));
    assert(allocator.release(out.id, out.offset, out.size));
    allocator.lastID = UINT64_MAX;
    assert(!allocator.reserve(4096, 4096, out)); // never recycle ownership IDs
    puts("GART allocator: 10000 modeled operations, first-fit holes, absolute alignment, stale ownership, overflow and exhaustion pass");
}
