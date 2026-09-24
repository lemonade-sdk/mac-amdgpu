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

    // Metadata must accommodate every later free, not silently lose a range
    // when inference caches thousands of independent code BOs.
    VRAMBumpAllocator fragmented;
    constexpr uint32_t count = VRAMBumpAllocator::kMaxAllocations;
    fragmented.init(base, (uint64_t(count) + 2) * page);
    VRAMAllocation allocations[count]{};
    for (auto &allocation : allocations) assert(fragmented.alloc(page, page, &allocation));
    const auto fullUsed = fragmented.bytes_used();
    VRAMAllocation rejected{123, nullptr, 456, 789};
    assert(!fragmented.alloc(page, page, &rejected));
    assert(rejected.gpu_va == 123 && rejected.size == 456 && fragmented.bytes_used() == fullUsed);
    // 4096 nonadjacent frees exercise the old 256-node leak directly.
    uint32_t freed = 0;
    for (uint32_t i = 0; i < count; i += 2) { fragmented.free(allocations[i]); ++freed; }
    assert(fragmented.alloc_count() == count - freed);
    assert(fragmented.bytes_used() == uint64_t(count - freed) * page);
    assert(fragmented.largest_free_span() == 3 * page);
    VRAMAllocation reused{};
    assert(fragmented.alloc(page, page, &reused) && reused.gpu_va == allocations[0].gpu_va);
    fragmented.free(reused);
    for (uint32_t i = 1; i < count; i += 2) fragmented.free(allocations[i]);
    assert(fragmented.alloc_count() == 0 && fragmented.bytes_used() == 0);
    assert(fragmented.bytes_free() == fragmented.size() && fragmented.largest_free_span() == fragmented.size());
    assert(fragmented.alloc(fragmented.size(), page, &reused));
    fragmented.free(reused);
    assert(fragmented.largest_free_span() == fragmented.size());
    // Alignment splits create free gaps too; all must coalesce on release.
    fragmented.init(base + page, (uint64_t(count) * 2 + 4) * page);
    for (uint32_t i = 0; i < 1024; ++i) assert(fragmented.alloc(page, 2 * page, &allocations[i]));
    for (uint32_t i = 0; i < 1024; i += 2) fragmented.free(allocations[i]);
    for (uint32_t i = 1; i < 1024; i += 2) fragmented.free(allocations[i]);
    assert(!fragmented.bytes_used() && !fragmented.alloc_count());
    assert(fragmented.largest_free_span() == fragmented.size());
    puts("VRAM accounting tests passed");
}
