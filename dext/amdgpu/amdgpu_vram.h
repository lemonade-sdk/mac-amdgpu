//
//  amdgpu_vram.h — Minimal bump allocator over the visible VRAM
//  aperture (BAR2).
//
//  On Apple Silicon + TB5 the visible VRAM window is 256 MB without
//  ReBAR, ~1 GB with the partial ReBAR our setup supports. Of the
//  32 GB total VRAM the rest is not directly CPU-mappable; reaching
//  it requires SDMA bouncing.
//
//  Several Phase 1B subsystems (RLC clear-state buffer, KIQ MQD,
//  MES MQDs, GFX ring buffers in some configs) want VRAM-resident
//  allocations. A bump allocator off the *top* of the visible window
//  is the simplest workable thing — none of these objects are freed
//  in normal operation, so a bump-with-no-real-free is fine for the
//  bringup phase. We add a real allocator later when BOs ship.
//
//  Address space convention:
//      gpu_va  = GMC.vram_start + offset_in_window
//      cpu_va  = (uint8_t *)bar2_cpu_mapping + offset_in_window
//
//  We don't currently map BAR2 inside the dext (clients map it
//  themselves via CopyClientMemoryForType). The CPU pointer is
//  filled in at allocator setup time iff the caller hands us a
//  BAR2 mapping; otherwise it stays null and only gpu_va is valid.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

struct VRAMAllocation {
    uint64_t gpu_va;     // GPU-side bus address
    void    *cpu_ptr;    // CPU-side pointer (nullptr if BAR2 not mapped in-dext)
    uint64_t size;
    uint64_t alignment;
};

//
// Bump allocator over the *top* of a [base, base+size) range.
// Top-down so the rest of the visible window stays available for
// userspace BO mapping when we add that.
//
class VRAMBumpAllocator {
public:
    VRAMBumpAllocator() = default;

    // base + size define the GPU-VA range. cpu_base is optional;
    // if non-null, CPU pointers are returned in allocations.
    void init(uint64_t base, uint64_t size, void *cpu_base = nullptr) {
        m_base       = base;
        m_size       = size;
        m_cpu_base   = static_cast<uint8_t *>(cpu_base);
        m_top_offset = size;     // grow downward from the top
        m_bytes_used = 0;
        m_alloc_count = 0;
        m_inited     = true;
    }

    bool is_inited() const { return m_inited; }
    uint64_t base() const { return m_base; }
    uint64_t size() const { return m_size; }
    uint64_t bytes_used() const { return m_bytes_used; }
    uint64_t bytes_free() const { return m_size - m_bytes_used; }
    uint32_t alloc_count() const { return m_alloc_count; }

    // Allocate. alignment must be a power of two; rounded up to
    // amdgpu::kASPageSize (16 KB) if smaller to keep DART happy
    // when these allocations get re-exported as BAR mappings.
    bool alloc(uint64_t bytes, uint64_t alignment, VRAMAllocation *out) {
        if (!m_inited || bytes == 0 || out == nullptr) return false;
        // Coerce alignment up to 16 KB (AS page size).
        if (alignment < 16384) alignment = 16384;
        // Top-down: round size up to alignment, subtract from top.
        uint64_t rounded = (bytes + alignment - 1) & ~(alignment - 1);
        if (rounded > m_top_offset) return false;  // OOM
        uint64_t new_top = m_top_offset - rounded;
        // Align the resulting offset.
        new_top &= ~(alignment - 1);
        if (new_top >= m_top_offset) return false;
        uint64_t allocated = m_top_offset - new_top;
        m_top_offset = new_top;
        m_bytes_used += allocated;
        m_alloc_count++;

        out->gpu_va = m_base + new_top;
        out->size   = allocated;
        out->alignment = alignment;
        out->cpu_ptr = (m_cpu_base != nullptr)
                          ? (m_cpu_base + new_top)
                          : nullptr;
        return true;
    }

    // No-op for the bump allocator; here for API symmetry.
    void free(const VRAMAllocation &) { /* not implemented (bump) */ }

private:
    bool      m_inited      = false;
    uint64_t  m_base        = 0;
    uint64_t  m_size        = 0;
    uint64_t  m_top_offset  = 0;
    uint64_t  m_bytes_used  = 0;
    uint32_t  m_alloc_count = 0;
    uint8_t  *m_cpu_base    = nullptr;
};

} // namespace amdgpu
