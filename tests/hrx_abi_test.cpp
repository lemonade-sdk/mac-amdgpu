// Compare the runtime's AMD ABI directly with the pinned HRX consumer layouts.
#include <cstddef>
#include <cstdio>
#include <hsa/amd_hsa_queue.h>
#include <hsa/amd_hsa_signal.h>
#include "iree/hal/drivers/amdgpu/abi/queue.h"
#include "iree/hal/drivers/amdgpu/abi/signal.h"

#define SAME_OFFSET(A, B, field) static_assert(offsetof(A, field) == offsetof(B, field))
static_assert(sizeof(hsa_queue_t) == sizeof(iree_hsa_queue_t));
static_assert(sizeof(amd_queue_t) == sizeof(iree_amd_queue_t));
static_assert(sizeof(amd_signal_t) == sizeof(iree_amd_signal_t));
SAME_OFFSET(hsa_queue_t, iree_hsa_queue_t, base_address);
SAME_OFFSET(hsa_queue_t, iree_hsa_queue_t, doorbell_signal);
SAME_OFFSET(hsa_queue_t, iree_hsa_queue_t, size);
SAME_OFFSET(amd_queue_t, iree_amd_queue_t, write_dispatch_id);
SAME_OFFSET(amd_queue_t, iree_amd_queue_t, read_dispatch_id);
SAME_OFFSET(amd_queue_t, iree_amd_queue_t, queue_properties);
SAME_OFFSET(amd_signal_t, iree_amd_signal_t, kind);
SAME_OFFSET(amd_signal_t, iree_amd_signal_t, value);
SAME_OFFSET(amd_signal_t, iree_amd_signal_t, hardware_doorbell_ptr);
static_assert(int(AMD_SIGNAL_KIND_USER) == int(IREE_AMD_SIGNAL_KIND_USER));
static_assert(int(AMD_SIGNAL_KIND_DOORBELL) == int(IREE_AMD_SIGNAL_KIND_DOORBELL));
int main() { std::puts("PASS: pinned HRX queue/signal sizes and direct-access offsets match runtime AMD ABI."); }
