// Uses production CPU slab and TLSF helpers without opening a GPU.
// Exercise whole-slab reuse, fragmented ranges, causal frontiers and trim.
#include "iree/async/notification.h"
#include "iree/async/proactor.h"
#include "iree/async/proactor_platform.h"
#include "iree/hal/memory/cpu_slab_provider.h"
#include "iree/hal/memory/tlsf_pool.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
static void check(iree_status_t s) {
  if (!iree_status_is_ok(s)) {
    iree_status_fprint(stderr, s);
    std::abort();
  }
}
static std::atomic<int> failures{0};
static void expect(bool b, const char *label) {
  if (!b) {
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", label);
  }
}
static iree_async_proactor_t *proactor;
using Reservation = iree_hal_pool_reservation_t;
using Frontier = iree_async_frontier_t;
constexpr auto no_growth = IREE_HAL_POOL_RESERVE_FLAG_DISALLOW_GROWTH;
struct Pool {
  iree_async_notification_t *notification = nullptr;
  iree_hal_slab_provider_t *provider = nullptr;
  iree_hal_pool_t *value = nullptr;
  explicit Pool(
      iree_hal_pool_epoch_query_t query = iree_hal_pool_epoch_query_null()) {
    check(iree_async_notification_create(
        proactor, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &notification));
    check(
        iree_hal_cpu_slab_provider_create(iree_allocator_system(), &provider));
    iree_hal_tlsf_pool_options_t options = {};
    options.tlsf_options.range_length = 4096;
    options.tlsf_options.alignment = 64;
    options.tlsf_options.initial_block_capacity = 16;
    options.tlsf_options.frontier_capacity = 2;
    check(iree_hal_tlsf_pool_create(options, provider, notification, query,
                                    iree_allocator_system(), &value));
  }
  ~Pool() {
    iree_hal_pool_release(value);
    iree_hal_slab_provider_release(provider);
    iree_async_notification_release(notification);
  }
  Reservation acquire(size_t bytes,
                      unsigned flags = IREE_HAL_POOL_RESERVE_FLAG_NONE,
                      const Frontier *f = nullptr, unsigned align = 64) {
    Reservation r = {};
    iree_hal_pool_acquire_info_t info = {};
    iree_hal_pool_acquire_result_t result;
    check(iree_hal_pool_acquire_reservation(value, bytes, align, f, flags, &r,
                                            &info, &result));
    if (result != IREE_HAL_POOL_ACQUIRE_OK &&
        result != IREE_HAL_POOL_ACQUIRE_OK_FRESH) {
      expect(result == IREE_HAL_POOL_ACQUIRE_EXHAUSTED,
             "only expected exhaustion");
      return {};
    }
    expect(r.offset % align == 0, "reservation alignment");
    expect(!info.wait_frontier, "no hidden causal wait");
    return r;
  }
  void release(Reservation &r, const Frontier *f = nullptr) {
    if (r.block_handle) {
      iree_hal_pool_release_reservation(value, &r, f);
      r = {};
    }
  }
  void release(std::vector<Reservation> &rs, const Frontier *f = nullptr) {
    for (auto &r : rs)
      release(r, f);
  }
  iree_hal_pool_stats_t stats() {
    iree_hal_pool_stats_t s = {};
    iree_hal_pool_query_stats(value, &s);
    return s;
  }
};
struct F {
  alignas(16) unsigned char bytes[sizeof(Frontier) +
                                  sizeof(iree_async_frontier_entry_t)];
  F(unsigned axis, unsigned epoch) {
    iree_async_frontier_initialize(get(), 1);
    get()->entries[0] = {iree_async_axis_make_queue(1, 0, 0, axis), epoch};
  }
  Frontier *get() { return reinterpret_cast<Frontier *>(bytes); }
};
static std::vector<Reservation> fill(Pool &p, unsigned n, size_t size = 4096,
                                     unsigned flags = 0,
                                     const Frontier *f = nullptr) {
  std::vector<Reservation> rs;
  for (unsigned i = 0; i < n; ++i) {
    auto r = p.acquire(size, flags, f);
    if (!r.block_handle)
      break;
    rs.push_back(r);
  }
  return rs;
}
static void full_slabs(unsigned count, bool grow) {
  Pool p;
  auto rs = fill(p, count);
  expect(rs.size() == count, "initial full slabs");
  p.release(rs);
  unsigned initial = p.stats().slab_count;
  for (unsigned repeat = 0; repeat < 6; ++repeat) {
    rs = fill(p, count, 4096, grow ? 0 : no_growth);
    expect(rs.size() == count, "all released slabs reachable");
    expect(p.stats().slab_count == initial, "matched footprint does not grow");
    p.release(rs);
  }
  auto s = p.stats();
  expect(s.bytes_reserved == 0, "all reservations uncharged");
  std::printf("full slabs=%u growth=%d committed=%u free=%llu\n", count, grow,
              s.slab_count, (unsigned long long)s.bytes_free);
}
static void frontiers() {
  Pool p;
  auto rs = fill(p, 13);
  F death(0, 50), before(0, 49), same(0, 50), other(1, 80);
  p.release(rs, death.get());
  for (const auto *f :
       {static_cast<Frontier *>(nullptr), before.get(), other.get()}) {
    auto r = p.acquire(
        4096, no_growth | IREE_HAL_POOL_RESERVE_FLAG_ALLOW_WAIT_FRONTIER, f);
    expect(!r.block_handle, "unsafe frontier refused");
    p.release(r);
  }
  rs = fill(p, 13, 4096, no_growth, same.get());
  expect(rs.size() == 13, "dominated frontiers beyond hint ring reused");
  expect(p.stats().slab_count == 13, "frontier reuse did not grow");
  p.release(rs);
  std::puts("frontier ordering checked");
}
static bool completed(void *data, iree_async_axis_t, uint64_t epoch) {
  return static_cast<std::atomic<unsigned> *>(data)->load() >= epoch;
}
static void observed_frontiers() {
  std::atomic<unsigned> epoch{0};
  Pool p({completed, &epoch});
  auto rs = fill(p, 13);
  F death(0, 50);
  p.release(rs, death.get());
  auto r = p.acquire(4096, no_growth);
  expect(!r.block_handle, "unobserved frontier refused");
  p.release(r);
  epoch = 50;
  rs = fill(p, 13, 4096, no_growth);
  expect(rs.size() == 13, "observed frontier fallback safe reuse");
  p.release(rs);
}
static void fragments() {
  Pool p;
  std::vector<Reservation> small, live;
  for (unsigned i = 0; i < 12; ++i) {
    small.push_back(p.acquire(1024));
    live.push_back(p.acquire(3072));
  }
  expect(p.stats().slab_count == 12, "mixed slabs initialized");
  p.release(small);
  auto impossible = p.acquire(2048, no_growth);
  expect(!impossible.block_handle, "separate fragments not falsely coalesced");
  p.release(impossible);
  auto rs = fill(p, 24, 512, no_growth);
  expect(rs.size() == 24, "all mixed fragments reused");
  expect(p.stats().slab_count == 12, "fragment reuse no growth");
  for (auto a : rs)
    for (auto b : live)
      if (a.slab_index == b.slab_index)
        expect(a.offset + a.byte_length <= b.offset ||
                   b.offset + b.byte_length <= a.offset,
               "live fragment not overlapped");
  p.release(rs);
  p.release(live);
  rs = fill(p, 12, 4096, no_growth);
  expect(rs.size() == 12, "coalesced full slabs all reachable");
  p.release(rs);
  expect(p.stats().bytes_reserved == 0, "mixed sizes uncharged");
}
static void concurrent() {
  Pool p;
  auto initial = fill(p, 8);
  p.release(initial);
  std::mutex mutex;
  std::vector<Reservation> active;
  std::atomic<bool> stop{false};
  std::thread trimmer([&] {
    while (!stop.load()) {
      check(iree_hal_pool_trim(p.value));
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> workers;
  for (unsigned t = 0; t < 4; ++t)
    workers.emplace_back([&] {
      for (unsigned i = 0; i < 500; ++i) {
        auto r = p.acquire(1024, no_growth);
        if (!r.block_handle) {
          expect(false, "concurrent footprint reuse");
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          for (auto b : active)
            if (r.slab_index == b.slab_index)
              expect(r.offset + r.byte_length <= b.offset ||
                         b.offset + b.byte_length <= r.offset,
                     "concurrent reservations distinct");
          active.push_back(r);
        }
        std::this_thread::yield();
        {
          std::lock_guard<std::mutex> lock(mutex);
          std::erase_if(
              active, [&](auto b) { return b.block_handle == r.block_handle; });
          p.release(r);
        }
      }
    });
  for (auto &t : workers)
    t.join();
  stop = true;
  trimmer.join();
  auto s = p.stats();
  expect(s.slab_count == 8, "concurrent matched footprint did not grow");
  expect(s.bytes_reserved == 0, "concurrent reservations uncharged");
  std::puts("2000 concurrent allocations with trim checked");
}
int main() {
  check(
      iree_async_proactor_create_platform(iree_async_proactor_options_default(),
                                          iree_allocator_system(), &proactor));
  full_slabs(2, false);
  full_slabs(8, false);
  full_slabs(33, false);
  full_slabs(33, true);
  frontiers();
  observed_frontiers();
  fragments();
  concurrent();
  iree_async_proactor_release(proactor);
  std::printf("failures=%d\n", failures.load());
  return failures ? 1 : 0;
}
