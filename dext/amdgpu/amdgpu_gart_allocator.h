#pragma once

#include <stdint.h>

namespace amdgpu {

struct GARTReservation {
    uint64_t offset;
    uint64_t size;
    uint64_t id;
};

// Session-owned aperture reservations, serialized by the driver dispatch queue.
// A reservation survives failed PTE publication/teardown. Release it only after
// both hub invalidations and DMA completion, or discard the session after reset.
struct GARTApertureAllocator {
    static constexpr unsigned capacity = 128;
    GARTReservation records[capacity]{};
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t lastID = 0;
    bool initialized = false;

    bool init(uint64_t apertureBase, uint64_t apertureSize) {
        if (!apertureSize || (apertureBase & 4095) || (apertureSize & 4095) ||
            apertureBase > UINT64_MAX - apertureSize) return false;
        if (initialized) return base == apertureBase && size == apertureSize;
        base = apertureBase;
        size = apertureSize;
        initialized = true;
        return true;
    }

    bool find(uint64_t bytes, uint64_t alignment, uint64_t &offset,
              unsigned &slot) const {
        if (!initialized || !bytes || (bytes & 4095) || alignment < 4096 ||
            (alignment & (alignment - 1)) || bytes > size || lastID == UINT64_MAX)
            return false;
        slot = capacity;
        for (unsigned i = 0; i < capacity; ++i)
            if (!records[i].id) { slot = i; break; }
        if (slot == capacity) return false;
        uint64_t candidate = 0;
        // Each collision advances past at least one reservation. Unsorted
        // records permit bounded metadata without allocation in the driver.
        for (unsigned pass = 0; pass <= capacity; ++pass) {
            const uint64_t address = base + candidate;
            if (address > UINT64_MAX - (alignment - 1)) return false;
            candidate = ((address + alignment - 1) & ~(alignment - 1)) - base;
            if (candidate > size || bytes > size - candidate) return false;
            uint64_t next = candidate;
            for (const auto &record : records) {
                if (record.id && candidate < record.offset + record.size &&
                    record.offset < candidate + bytes && record.offset + record.size > next)
                    next = record.offset + record.size;
            }
            if (next == candidate) { offset = candidate; return true; }
            candidate = next;
        }
        return false;
    }

    bool reserve(uint64_t bytes, uint64_t alignment, GARTReservation &out) {
        uint64_t offset;
        unsigned slot;
        if (!find(bytes, alignment, offset, slot)) return false;
        out = {offset, bytes, ++lastID};
        records[slot] = out;
        return true;
    }

    bool owns(uint64_t id, uint64_t offset, uint64_t bytes) const {
        if (!id || !initialized) return false;
        for (const auto &record : records)
            if (record.id == id && record.offset == offset && record.size == bytes)
                return true;
        return false;
    }

    bool release(uint64_t id, uint64_t offset, uint64_t bytes) {
        if (!id || !initialized) return false;
        for (auto &record : records) {
            if (record.id == id && record.offset == offset && record.size == bytes) {
                record = {};
                return true;
            }
        }
        return false;
    }

    uint64_t bytes_used() const {
        uint64_t used = 0;
        for (const auto &record : records) if (record.id) used += record.size;
        return used;
    }
};

} // namespace amdgpu
