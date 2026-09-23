#pragma once
#include <hsa/hsa.h>

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic ABI, independent of HSA extension numbering. This reports a live
// driver snapshot, not dispatch capability or HSA conformance.
typedef struct mac_hsa_device_info_s {
    uint64_t registry_id;
    uint64_t driver_build;
    uint64_t bringup_stage;
    uint64_t visible_vram_bytes;
    uint64_t total_vram_bytes;
    uint32_t gfx_major;
    uint32_t gfx_minor;
    uint32_t gfx_revision;
    uint32_t reserved;
} mac_hsa_device_info_t;

__attribute__((visibility("default")))
hsa_status_t mac_hsa_agent_get_driver_info(hsa_agent_t agent,
                                          mac_hsa_device_info_t *info,
                                          size_t info_size);

// Synchronous native launch of a frozen HSA-loaded gfx1201 kernel. This is
// not an HSA AQL queue. Currently accepts wave32, a kernarg-pointer-only user
// SGPR layout, and no scratch, LDS, preload or dynamic stack. The caller lists
// every device/shared allocation referenced by kernargs; ordinary unmapped CPU
// pointers are invalid. The driver must be build 183 or newer.
// Buffers/code remain retained through completion. A failed GPU launch requires
// session recovery. Group counts are workgroups, not individual workitems.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_executable_dispatch(hsa_executable_symbol_t symbol,
    const void *kernarg, size_t kernarg_size, const uint32_t groups[3],
    const uint32_t threads[3], const void *const *buffers, size_t buffer_count,
    uint64_t *completion_fence);

// Explicit coarse shared allocation for the native launch path. CPU and GPU
// addresses are identical. CPU access is allowed only between completed GPU
// operations; this does not provide system atomics or HSA fine-grained memory.
// Release with hsa_memory_free; normal HSA pool capabilities are unchanged.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_memory_allocate_shared(hsa_agent_t agent, size_t size, void **out);

#ifdef __cplusplus
}
#endif
