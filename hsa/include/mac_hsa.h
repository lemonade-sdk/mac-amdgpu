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

// Raw CP dispatch timestamps in this GPU's clock domain, NOT HSA system time.
// Enable queue profiling before its first submission. Use a fresh completion
// signal per dispatch, SYSTEM release scope, and retain it unchanged until readout.
// The caller associates the signal with its submitting queue; this API does not
// prove that association. No submission/wait is performed; pending/missing stamps
// fail without changing output. Cross-device/SDMA/host clock correlation is absent.
enum { MAC_HSA_TIMESTAMP_DOMAIN_GPU = 1 };
typedef struct mac_hsa_dispatch_timestamps_s {
    uint64_t version, start_ticks, end_ticks, frequency_hz;
    uint32_t valid_bits, clock_domain;
} mac_hsa_dispatch_timestamps_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_dispatch_timestamps(const hsa_queue_t *queue,hsa_signal_t completion,
    mac_hsa_dispatch_timestamps_t *out,size_t out_size);

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

// Bounded hardware AQL dispatch (driver 184+), with the same kernel limits as
// the native PM4 path above. Success proves a GPU-only VRAM completion signal
// changed from 1 to 0 and firmware acknowledged queue removal. This does not
// expose a persistent hsa_queue_t or CPU/GPU atomic signals. Output is the
// observed signal value (zero on success), not a monotonic fence sequence.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_executable_dispatch_aql(hsa_executable_symbol_t symbol,
    const void *kernarg, size_t kernarg_size, const uint32_t groups[3],
    const uint32_t threads[3], const void *const *buffers, size_t buffer_count,
    uint64_t *completion);

// Explicit coarse shared allocation for the native launch path. CPU and GPU
// addresses are identical. CPU access is allowed only between completed GPU
// operations; this does not provide system atomics or HSA fine-grained memory.
// Release with hsa_memory_free; normal HSA pool capabilities are unchanged.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_memory_allocate_shared(hsa_agent_t agent, size_t size, void **out);

enum {
    MAC_HSA_SYNC_CPU_LOCAL_ATOMICS = 1u << 0,
    MAC_HSA_SYNC_GPU_LOCAL_ATOMICS = 1u << 1,
    MAC_HSA_SYNC_OWNERSHIP_TRANSFER = 1u << 2,
    MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS = 1u << 3,
    MAC_HSA_SYNC_NATIVE_CPU_GPU_RMW = 1u << 4,
    MAC_HSA_SYNC_MAILBOX_IRQ_WAKE = 1u << 5
};
// Query a tracked allocation and its actual GPU mapping path. LOCAL flags allow
// atomics within one agent domain, not simultaneous CPU/GPU RMW on one word.
// OWNERSHIP_TRANSFER permits the tested release/acquire ownership protocol; it
// does not make the pool fine-grained. GPU_MEDIATED_SIGNALS describes HSA signal
// API routing, not automatic interception of arbitrary pointer atomics. Neither
// native mixed RMW nor mailbox IRQ wake is qualified by the current profiles.
// No device initialization, config writes, or submission. Output unchanged on
// failure; pass its GPU agent for GPU allocations, CPU agent for host-only ones.
__attribute__((visibility("default")))
hsa_status_t mac_hsa_memory_get_sync_capabilities(hsa_agent_t agent,
    const void *pointer, uint32_t *flags);

// Driver190+: read-only snapshot for the aligned 64-bit word at shared_pointer
// and an owned live hardware queue on the same GPU. This does not submit work,
// initialize the device or change mappings/PCIe/MQD policy. Caller keeps the
// queue idle while sampling; MQD backing is NOT a live selected HQD register.
// valid_fields bits: 1=PTE, 2=MQD backing, 4=PCIe endpoint, 8=GFXHUB context.
// CPU map options are the requested policy; actual CPU cache/MAIR attributes
// have no public query and remain UINT64_MAX. Output is unchanged on failure.
typedef struct mac_hsa_shared_atomic_diagnostics_s {
    uint64_t version, valid_fields, gpu_address, dma_address, pte_vram_offset;
    uint64_t pte_actual, pte_expected, mqd_gpu_address, mqd_backing_hq_status0;
    uint64_t pcie_capability_offset, pcie_device_capabilities2, pcie_device_control2;
    uint64_t gfxhub_page_table_base, gfxhub_context0_control;
    uint64_t cpu_mapping_options, cpu_cache_attributes;
} mac_hsa_shared_atomic_diagnostics_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_shared_atomic_diagnostics(const void *shared_pointer,
    const hsa_queue_t *queue, mac_hsa_shared_atomic_diagnostics_t *out, size_t out_size);

// Driver191 explicit experiment on an unqualified PCIe path. enable=1 sets
// only endpoint DeviceControl2 bit6; enable=0 restores its original value.
// No queue/submission may remain at either transition. Requires exclusive
// ownership; does not change HSA capabilities, PTE/cache/MQD policy or memory.
// A valid driver reply is copied even when its operation failed: inspect
// driver_status/restore_pending. Transport/argument failures leave out unchanged.
// Hot unplug or driver-process failure cannot guarantee software restoration.
typedef struct mac_hsa_atomic_requester_experiment_s {
    uint64_t version, before_control2, requested_control2, observed_control2;
    uint64_t original_control2, active, restore_pending, driver_status;
} mac_hsa_atomic_requester_experiment_t;
__attribute__((visibility("default")))
hsa_status_t mac_hsa_atomic_requester_experiment(hsa_agent_t agent,uint32_t enable,
    mac_hsa_atomic_requester_experiment_t *out,size_t out_size);

#ifdef __cplusplus
}
#endif
