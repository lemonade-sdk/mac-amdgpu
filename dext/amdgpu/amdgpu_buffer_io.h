#pragma once
#include <stdint.h>
#include "amdgpu_client_lifecycle.h"

namespace amdgpu {
constexpr uint64_t kBufferIOChunkBytes = 4096;
constexpr uint64_t kBufferCopyMaxBytes = 4 * 1024 * 1024;
constexpr uint32_t kBufferVisibleVRAM = 1;
constexpr uint32_t kBufferDeviceVRAM = 3;

inline bool buffer_vram_domain(uint32_t domain) {
    return domain == kBufferVisibleVRAM || domain == kBufferDeviceVRAM;
}

inline bool buffer_gpu_range(uint64_t base, uint64_t capacity,
    uint64_t offset, uint64_t bytes, uint64_t &address) {
    if (!bytes || !client_subrange(offset, bytes, capacity) ||
        base >= (1ull << 48) || capacity > (1ull << 48) - base) return false;
    address = base + offset;
    return true;
}

inline bool buffer_copy_ranges(uint64_t source, uint64_t sourceSize, uint64_t sourceOffset,
    uint64_t destination, uint64_t destinationSize, uint64_t destinationOffset,
    uint64_t bytes, uint64_t &sourceAddress, uint64_t &destinationAddress) {
    if (bytes > kBufferCopyMaxBytes ||
        !buffer_gpu_range(source, sourceSize, sourceOffset, bytes, sourceAddress) ||
        !buffer_gpu_range(destination, destinationSize, destinationOffset, bytes, destinationAddress))
        return false;
    // SDMA COPY_LINEAR is not memmove. Reject all overlapping copies.
    return sourceAddress >= destinationAddress + bytes ||
           destinationAddress >= sourceAddress + bytes;
}

// Split CPU-visible bootstrap/staging allocations from GPU-only model storage.
// The final MiB remains reserved for discovery/firmware data even when the
// reported usable size already excludes the board's firmware reservation.
inline bool buffer_device_pool(uint64_t base, uint64_t visible, uint64_t usable,
    uint64_t &poolBase, uint64_t &poolBytes) {
    constexpr uint64_t page = 16384, tail = 1024 * 1024;
    poolBase = poolBytes = 0;
    if (!visible || visible > usable || base >= (1ull << 48) ||
        usable > (1ull << 48) - base || (base & (page - 1))) return false;
    if (usable <= tail || visible > UINT64_MAX - (page - 1)) return true;
    const auto start = (visible + page - 1) & ~(page - 1);
    const auto end = (usable - tail) & ~(page - 1);
    if (start >= end) return true; // A full BAR needs no separate invisible pool.
    poolBase = base + start;
    poolBytes = end - start;
    return true;
}
}
