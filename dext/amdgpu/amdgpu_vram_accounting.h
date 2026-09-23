#pragma once
#include "amdgpu_vram.h"

namespace amdgpu {
namespace vram_accounting {
// QueryInfo (21), tag 5. CPU accounting only, sampled on the allocator queue.
// Never interpret ExcludedBytes or PoolUsed as board-wide firmware occupancy.
constexpr uint64_t kVersion = 1;
constexpr uint64_t kValid = 1;
enum Field : unsigned {
    Version, Flags, UsableBytes, VisibleBytes, ExcludedBytes,
    VisibleCapacity, VisibleUsed, VisibleFree, VisibleLargestSpan, VisibleCount,
    DeviceCapacity, DeviceUsed, DeviceFree, DeviceLargestSpan, DeviceCount,
    Count
};
struct Snapshot { uint64_t values[Count]{}; };
static_assert(sizeof(Snapshot) == 15 * sizeof(uint64_t), "scalar ABI");

inline bool valid(const Snapshot &s) {
    const auto *v = s.values;
    if (v[Version] != kVersion || (v[Flags] & ~kValid)) return false;
    if (!(v[Flags] & kValid)) {
        for (unsigned i = UsableBytes; i < Count; ++i) if (v[i]) return false;
        return true;
    }
    if (!v[UsableBytes] || !v[VisibleBytes] || v[VisibleBytes] > v[UsableBytes] ||
        v[VisibleCapacity] > v[VisibleBytes] ||
        v[DeviceCapacity] > v[UsableBytes] - v[VisibleBytes]) return false;
    for (unsigned pool = 0; pool < 2; ++pool) {
        const unsigned i = pool ? DeviceCapacity : VisibleCapacity;
        if (v[i + 1] > v[i] || v[i + 2] != v[i] - v[i + 1] ||
            v[i + 3] > v[i + 2] || v[i + 4] > v[i + 1] / 16384) return false;
    }
    return v[ExcludedBytes] == v[UsableBytes] - v[VisibleCapacity] - v[DeviceCapacity];
}

inline Snapshot snapshot(bool ready, uint64_t base, uint64_t usable,
                         uint64_t visible, const VRAMBumpAllocator &low,
                         const VRAMBumpAllocator &high) {
    Snapshot s{};
    s.values[Version] = kVersion;
    if (!ready || !usable || !visible || visible > usable ||
        base > UINT64_MAX - usable || !low.is_inited() ||
        low.base() < base || low.base() - base > visible ||
        low.size() > visible - (low.base() - base)) return s;
    if (high.size() && (!high.is_inited() || high.base() < base + visible ||
        high.base() - base > usable || high.size() > usable - (high.base() - base))) return s;
    s.values[Flags] = kValid;
    s.values[UsableBytes] = usable;
    s.values[VisibleBytes] = visible;
    s.values[ExcludedBytes] = usable - low.size() - high.size();
    const VRAMBumpAllocator *pools[] = {&low, &high};
    for (unsigned i = 0; i < 2; ++i) {
        const auto &pool = *pools[i];
        const unsigned at = i ? DeviceCapacity : VisibleCapacity;
        s.values[at] = pool.size();
        s.values[at + 1] = pool.bytes_used();
        s.values[at + 2] = pool.bytes_free();
        s.values[at + 3] = pool.largest_free_span();
        s.values[at + 4] = pool.alloc_count();
    }
    if (!valid(s)) {
        s = {};
        s.values[Version] = kVersion;
    }
    return s;
}
} // namespace vram_accounting
} // namespace amdgpu
