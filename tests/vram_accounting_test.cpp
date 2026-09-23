#include "amdgpu_vram_accounting.h"
#include "amdgpu_buffer_io.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace amdgpu;
    using namespace vram_accounting;
    constexpr uint64_t MiB = 1024 * 1024, GiB = 1024 * MiB, page = 16384;
    constexpr uint64_t base = 512 * GiB, usable = 32 * GiB, visible = 256 * MiB;
    VRAMBumpAllocator low, high;
    auto read = [&](bool ready = true) { return snapshot(ready, base, usable, visible, low, high); };
    assert(valid(read()) && read().values[Flags] == 0);
    low.init(base + 24 * MiB, visible - 24 * MiB);
    uint64_t highBase, highBytes;
    assert(buffer_device_pool(base, visible, usable, highBase, highBytes));
    high.init(highBase, highBytes);
    auto s = read();
    assert(valid(s) && s.values[Flags] == kValid);
    assert(s.values[ExcludedBytes] == 25 * MiB);
    assert(s.values[VisibleUsed] == 0 && s.values[DeviceUsed] == 0);
    assert(s.values[VisibleLargestSpan] == visible - 24 * MiB);
    VRAMAllocation a{}, b{}, c{}, model{};
    assert(low.alloc(1, page, &a));
    assert(low.alloc(1, page, &b));
    assert(low.alloc(low.size() - 2 * page, page, &c));
    assert(high.alloc(22 * GiB, 65536, &model));
    s = read();
    assert(s.values[VisibleUsed] == low.size() && s.values[VisibleFree] == 0);
    assert(s.values[VisibleCount] == 3 && s.values[VisibleLargestSpan] == 0);
    assert(s.values[DeviceUsed] == 22 * GiB && s.values[DeviceCount] == 1);
    low.free(a); low.free(c);
    s = read();
    assert(s.values[VisibleUsed] == page); // Requested byte counts would underreport.
    assert(s.values[VisibleFree] == low.size() - page);
    assert(s.values[VisibleLargestSpan] == low.size() - 2 * page);
    low.free(a); // Duplicate free cannot reduce used bytes again.
    assert(read().values[VisibleUsed] == page);
    low.free(b); high.free(model);
    s = read();
    assert(s.values[VisibleLargestSpan] == low.size() && s.values[VisibleCount] == 0);
    assert(s.values[DeviceUsed] == 0 && s.values[DeviceLargestSpan] == high.size());
    s = read(false); // Stop/detach hides retained allocations without touching them.
    assert(valid(s) && s.values[Flags] == 0 && s.values[UsableBytes] == 0);
    assert(low.is_inited() && high.is_inited());
    high.init(base + visible - page, MiB); // Overlapping pools must not be double counted.
    assert(read().values[Flags] == 0);
    high.init(base + usable, MiB);
    assert(read().values[Flags] == 0);
    assert(snapshot(true, UINT64_MAX - 1, usable, visible, low, high).values[Flags] == 0);
    high.init(0, 0); // Full BAR legitimately has no GPU-only pool.
    low.init(base + 24 * MiB, usable - 24 * MiB);
    s = snapshot(true, base, usable, usable, low, high);
    assert(valid(s) && s.values[Flags] == kValid && s.values[DeviceCapacity] == 0);
    assert(s.values[ExcludedBytes] == 24 * MiB); // Report actual layout, not an assumed tail.
    auto bad = s;
    bad.values[DeviceFree] = 1; assert(!valid(bad));
    bad = s; bad.values[Flags] |= 2; assert(!valid(bad));
    bad = s; bad.values[ExcludedBytes]++; assert(!valid(bad));
    bad = s; bad.values[VisibleCount] = 1; assert(!valid(bad));

    // A failed free when metadata is exhausted must remain charged. No
    // optimistic subtraction or inference from BO handles is allowed.
    VRAMBumpAllocator fragmented;
    fragmented.init(base, 514 * page);
    VRAMAllocation allocations[513]{};
    for (auto &allocation : allocations) assert(fragmented.alloc(page, page, &allocation));
    for (unsigned i = 0; i < 510; i += 2) fragmented.free(allocations[i]);
    const auto retained = fragmented.bytes_used();
    fragmented.free(allocations[510]);
    assert(fragmented.bytes_used() == retained && fragmented.largest_free_span() == page);
    fragmented.free(allocations[509]); // Adjacent coalescing needs no spare node.
    assert(fragmented.bytes_used() == retained - page);
    puts("VRAM accounting tests passed");
}
