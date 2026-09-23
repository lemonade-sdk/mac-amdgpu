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

#ifdef __cplusplus
}
#endif
