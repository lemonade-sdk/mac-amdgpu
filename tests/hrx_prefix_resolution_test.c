// Host-only resolver test. Queue allocation, GPU state and scheduler threads
// are absent; only semaphore type/scope/frontier services are stubbed. The
// production wait resolver and packet emitter are compiled unchanged into this
// executable.
#include "iree/hal/drivers/amdgpu/host_queue_policy.h"
#include "iree/hal/drivers/amdgpu/host_queue_waits.h"
#include "iree/hal/drivers/amdgpu/semaphore.h"
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
typedef struct {
  iree_async_semaphore_t base;
  iree_hal_amdgpu_last_signal_t cache;
  bool local;
} Fake;
bool iree_hal_amdgpu_semaphore_isa(iree_hal_semaphore_t *s) {
  return ((Fake *)s)->local;
}
iree_hal_amdgpu_last_signal_t *
iree_hal_amdgpu_semaphore_last_signal(iree_hal_semaphore_t *s) {
  return &((Fake *)s)->cache;
}
iree_hsa_fence_scope_t iree_hal_amdgpu_host_queue_wait_acquire_scope(
    const iree_hal_amdgpu_host_queue_t *q, iree_hal_semaphore_t *s) {
  return IREE_HSA_FENCE_SCOPE_SYSTEM;
}
iree_hsa_fence_scope_t iree_hal_amdgpu_host_queue_axis_acquire_scope(
    const iree_hal_amdgpu_host_queue_t *q, iree_async_axis_t a) {
  return IREE_HSA_FENCE_SCOPE_SYSTEM;
}
uint8_t iree_async_semaphore_query_frontier(iree_async_semaphore_t *s,
                                            iree_async_frontier_t *out,
                                            uint8_t n) {
  abort();
}
uint8_t iree_async_frontier_find_undominated(const iree_async_frontier_t *r,
                                             const iree_async_frontier_t *t,
                                             uint8_t n,
                                             iree_async_frontier_entry_t *out) {
  abort();
}
bool iree_async_frontier_merge(iree_async_frontier_t *t, uint8_t n,
                               const iree_async_frontier_t *s) {
  abort();
}
static void stamp(Fake *s, iree_async_axis_t axis, uint64_t epoch,
                  uint64_t value) {
  memset(s, 0, sizeof(*s));
  s->local = true;
  iree_hal_amdgpu_last_signal_store(
      &s->cache,
      IREE_HAL_AMDGPU_LAST_SIGNAL_FLAG_VALID |
          IREE_HAL_AMDGPU_LAST_SIGNAL_FLAG_PRODUCER_FRONTIER_EXACT,
      axis, epoch, value);
}
static iree_hal_amdgpu_wait_resolution_t
resolve(iree_hal_amdgpu_host_queue_t *q, Fake *a, Fake *b) {
  iree_hal_semaphore_t *sems[] = {(iree_hal_semaphore_t *)a,
                                  (iree_hal_semaphore_t *)b};
  uint64_t values[] = {9, 9};
  iree_hal_semaphore_list_t list = {
      .count = b ? 2 : 1, .semaphores = sems, .payload_values = values};
  iree_hal_amdgpu_wait_resolution_t r;
  memset(&r, 0xa5, sizeof(r));
  iree_hal_amdgpu_host_queue_resolve_waits(q, list, &r);
  return r;
}
int main(void) {
  iree_hal_amdgpu_host_queue_t q;
  memset(&q, 0, sizeof(q));
  q.axis = iree_async_axis_make_queue(1, 2, 3, 0);
  const iree_async_axis_t peer = iree_async_axis_make_queue(1, 2, 3, 1);
  q.wait_barrier_strategy = IREE_HAL_AMDGPU_WAIT_BARRIER_STRATEGY_DEFER;
  q.device_only_prefix_epoch = 7;
  iree_atomic_store(&q.notification_ring.epoch.last_published, 7,
                    iree_memory_order_release);
  q.epoch_table = calloc(1, iree_hal_amdgpu_epoch_signal_table_size(2));
  iree_hal_amdgpu_epoch_signal_table_initialize(q.epoch_table, 1, 2, 3, 2);
  iree_hal_amdgpu_epoch_signal_table_register(q.epoch_table, 0,
                                              (hsa_signal_t){.handle = 100});
  iree_hal_amdgpu_epoch_signal_table_register(q.epoch_table, 1,
                                              (hsa_signal_t){.handle = 200});
  Fake a, b;
  stamp(&a, q.axis, 7, 9);
  stamp(&b, q.axis, 7, 9);
  iree_hal_amdgpu_wait_resolution_t r = resolve(&q, &a, &b);
#if !defined(IREE_HAL_AMDGPU_MACOS_COARSE_HOST_ADAPTER)
  // The ordinary upstream DEFER policy must remain unchanged outside the Mac
  // adapter, even with exactly the same otherwise-eligible queue epoch.
  CHECK(r.needs_deferral && r.barrier_count == 0);
  free(q.epoch_table);
  puts("non-Mac production resolver preserves software deferral PASS");
  return 0;
#endif
  CHECK(!r.needs_deferral && r.barrier_count == 1 &&
        r.barriers[0].queue_prefix);
  CHECK(r.barrier_acquire_scope == IREE_HSA_FENCE_SCOPE_SYSTEM);
  CHECK(r.barriers[0].epoch_signal.handle == 0 &&
        r.barriers[0].target_epoch == 7);
  iree_hal_amdgpu_aql_packet_t packet;
  memset(&packet, 0xa5, sizeof(packet));
  uint16_t setup = 99;
  uint16_t header = iree_hal_amdgpu_host_queue_write_wait_barrier_packet_body(
      &q, &r.barriers[0], 123, (hsa_signal_t){.handle = 77},
      IREE_HSA_FENCE_SCOPE_SYSTEM, IREE_HSA_FENCE_SCOPE_AGENT, &packet, &setup);
  CHECK((header & 255) == IREE_HSA_PACKET_TYPE_BARRIER_AND &&
        (header & (1u << IREE_HSA_PACKET_HEADER_BARRIER)));
  CHECK(setup == 0 && packet.barrier_and.completion_signal.handle == 77);
  for (int i = 0; i < 5; ++i)
    CHECK(packet.barrier_and.dep_signal[i].handle == 0);
  // A single unresolved peer edge cancels the entire optimized resolution.
  stamp(&b, peer, 7, 9);
  r = resolve(&q, &a, &b);
  CHECK(r.needs_deferral && r.barrier_count == 0);
  // The actual resolver rejects failed, foreign, unsubmitted and future waits.
  iree_atomic_store(&a.base.failure_status, IREE_STATUS_ABORTED,
                    iree_memory_order_release);
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral && r.barrier_count == 0);
  stamp(&a, q.axis, 7, 9);
  a.local = false;
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral);
  stamp(&a, q.axis, 7, 8);
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral);
  stamp(&a, q.axis, 8, 9);
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral);
  // Host-action publication clears marker; no prefix barrier may bypass it.
  stamp(&a, q.axis, 7, 9);
  q.device_only_prefix_epoch = 0;
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral);
  q.device_only_prefix_epoch = 7;
  iree_atomic_store(&q.notification_ring.epoch.last_published, 6,
                    iree_memory_order_release);
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral);
  // Transitive host action A@6 -> device-only B@7 -> wait B cannot bypass A.
  iree_atomic_store(&q.notification_ring.epoch.last_published, 7,
                    iree_memory_order_release);
  q.last_prefix_host_action_epoch = 6;
  iree_atomic_store(&q.notification_ring.epoch.last_drained, 5,
                    iree_memory_order_release);
  r = resolve(&q, &a, NULL);
  CHECK(r.needs_deferral && r.barrier_count == 0);
  // Release-published last_drained is written only after A's host action
  // finishes.
  iree_atomic_store(&q.notification_ring.epoch.last_drained, 6,
                    iree_memory_order_release);
  r = resolve(&q, &a, NULL);
  CHECK(!r.needs_deferral && r.barrier_count == 1 &&
        r.barriers[0].queue_prefix);
  // The admitted prefix adds one real packet. Exercise the production ring
  // reservation helper at capacity and wrap rather than mirroring its
  // arithmetic. No HSA queue exists: read/write dispatch IDs are host-only test
  // storage.
  iree_hal_amdgpu_aql_packet_t slots[8];
  memset(slots, 0xa5, sizeof(slots));
  iree_atomic_int64_t write_id = 0, read_id = 0;
  q.aql_ring.base = slots;
  q.aql_ring.mask = 7;
  q.aql_ring.write_dispatch_id = &write_id;
  q.aql_ring.read_dispatch_id = (const volatile int64_t *)&read_id;
  iree_atomic_store(&write_id, 7, iree_memory_order_release);
  iree_atomic_store(&read_id, 0, iree_memory_order_release);
  uint64_t first_packet = 99;
  const uint32_t packet_count = r.barrier_count + 1;
  CHECK(packet_count == 2);
  CHECK(!iree_hal_amdgpu_aql_ring_try_reserve(&q.aql_ring, packet_count,
                                              &first_packet));
  CHECK(first_packet == 0 &&
        iree_atomic_load(&write_id, iree_memory_order_acquire) == 7);
  // Releasing exactly one older slot admits barrier@7 and payload@8(slot0).
  iree_atomic_store(&read_id, 1, iree_memory_order_release);
  CHECK(iree_hal_amdgpu_aql_ring_try_reserve(&q.aql_ring, packet_count,
                                             &first_packet));
  CHECK(first_packet == 7 &&
        iree_atomic_load(&write_id, iree_memory_order_acquire) == 9);
  CHECK(iree_hal_amdgpu_aql_ring_packet(&q.aql_ring, first_packet) ==
        &slots[7]);
  CHECK(iree_hal_amdgpu_aql_ring_packet(&q.aql_ring, first_packet + 1) ==
        &slots[0]);
  setup = 99;
  header = iree_hal_amdgpu_host_queue_write_wait_barrier_packet_body(
      &q, &r.barriers[0], first_packet, (hsa_signal_t){.handle = 0},
      IREE_HSA_FENCE_SCOPE_SYSTEM, IREE_HSA_FENCE_SCOPE_AGENT, &slots[7],
      &setup);
  iree_hal_amdgpu_aql_ring_commit(&slots[7], header, setup);
  CHECK(slots[7].barrier_and.header == header && setup == 0);
  for (unsigned i = 0; i < 5; ++i)
    CHECK(slots[7].barrier_and.dep_signal[i].handle == 0);
  CHECK(!iree_hal_amdgpu_aql_ring_try_reserve(&q.aql_ring, 1, &first_packet));
  CHECK(iree_atomic_load(&write_id, iree_memory_order_acquire) == 9);
  for (unsigned i = 0; i < 7; ++i)
    for (unsigned j = 0; j < 64; ++j)
      CHECK(slots[i].raw[j] == 0xa5);
  // Already-completed waits stay inline and preserve their acquire scope.
  iree_atomic_store(&a.base.timeline_value, 9, iree_memory_order_release);
  r = resolve(&q, &a, NULL);
  CHECK(!r.needs_deferral && r.barrier_count == 0 &&
        r.inline_acquire_scope == IREE_HSA_FENCE_SCOPE_SYSTEM);
  free(q.epoch_table);
  puts("production wait resolution: same queue, duplicate, cross queue, "
       "failure, host action, publication, capacity/wrap, completion PASS");
}
