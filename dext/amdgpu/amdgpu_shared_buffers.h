#pragma once
#include <stdint.h>

namespace amdgpu {
// Serialized by the driver lifecycle queue. One reference per owning BO slot,
// including slots retained in quarantine. Exporting a token adds no reference.
struct SharedBuffers {
    struct Entry {
        uint64_t token[2] = {}, gpu = 0, size = 0, alignment = 0;
        uint32_t references = 0;
    };
    static constexpr uint32_t capacity = 256;
    Entry entries[capacity]{};
    uint32_t find(uint64_t first, uint64_t second) const {
        if (!first && !second) return 0;
        for (uint32_t i = 0; i < capacity; ++i)
            if (entries[i].references && entries[i].token[0] == first && entries[i].token[1] == second) return i + 1;
        return 0;
    }
    uint32_t publish(uint64_t first, uint64_t second, uint64_t gpu, uint64_t size, uint64_t alignment) {
        if ((!first && !second) || !gpu || !size || find(first, second)) return 0;
        for (uint32_t i = 0; i < capacity; ++i) if (!entries[i].references) {
            entries[i] = {{first, second}, gpu, size, alignment, 1}; return i + 1;
        }
        return 0;
    }
    bool retain(uint32_t id) {
        if (!id || id > capacity || !entries[id - 1].references || entries[id - 1].references == UINT32_MAX) return false;
        ++entries[id - 1].references; return true;
    }
    bool release(uint32_t id) {
        if (!id || id > capacity || !entries[id - 1].references) return false;
        return --entries[id - 1].references == 0;
    }
};
}
