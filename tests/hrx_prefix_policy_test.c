#include "iree/hal/drivers/amdgpu/same_queue_prefix.h"
#include "iree/hal/drivers/amdgpu/util/aql_emitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "failed line %d: %s\n", __LINE__, #x);                   \
      abort();                                                                 \
    }                                                                          \
  } while (0)
int main(void) {
  CHECK(iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 7, 7, 7, 0, 0));
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 11, 7, 7, 7, 0, 0));
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 0, 0, 7, 0, 0));
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 8, 7, 7, 0, 0));
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 7, 7, 6, 0, 0));
  // Host-action epoch clears qualification, including older semaphore aliases.
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 7, 0, 8, 0, 0));
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 8, 0, 8, 0, 0));
  // A later device-only submission cannot requalify the older host-action
  // epoch.
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 8, 9, 9, 0, 0));
  CHECK(iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 9, 9, 9, 0, 0));
  // A pending earlier host action taints later GPU-only completion until drain.
  CHECK(!iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 9, 9, 9, 8, 7));
  CHECK(iree_hal_amdgpu_same_queue_prefix_eligible(10, 10, 9, 9, 9, 8, 8));
  // Qualify all fence combinations, optional completion, and stale packet body.
  for (int acquire = 0; acquire <= 2; ++acquire) {
    for (int release = 0; release <= 2; ++release) {
      iree_hsa_barrier_and_packet_t packet;
      memset(&packet, 0xa5, sizeof(packet));
      const iree_hsa_signal_t done = {.handle = 123};
      uint16_t header = iree_hal_amdgpu_aql_emit_nop(
          &packet, iree_hal_amdgpu_aql_packet_control_barrier(acquire, release),
          done);
      CHECK((header & 255) == IREE_HSA_PACKET_TYPE_BARRIER_AND);
      CHECK(header & (1u << IREE_HSA_PACKET_HEADER_BARRIER));
      CHECK(((header >> IREE_HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) & 3) ==
            acquire);
      CHECK(((header >> IREE_HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE) & 3) ==
            release);
      for (int i = 0; i < 5; ++i)
        CHECK(packet.dep_signal[i].handle == 0);
      CHECK(packet.completion_signal.handle == 123);
      CHECK(packet.reserved0 == 0 && packet.reserved1 == 0 &&
            packet.reserved2 == 0);
    }
  }
  puts("same-queue prefix eligibility and standard AQL fence encoding PASS");
}
