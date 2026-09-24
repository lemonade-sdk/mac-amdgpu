#include "hrx_runtime.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t guard = 4096;
constexpr size_t max_payload = size_t(16) << 20;
constexpr uint64_t timeout_ns = 30000000000ull;
hrx_device_t device = nullptr;
hrx_stream_t stream = nullptr;
bool initialized = false;
struct Buffer { hrx_buffer_t handle = nullptr; unsigned char* host = nullptr; };
std::vector<Buffer> buffers;
void check(hrx_status_t s, const char* where) {
  if (hrx_status_is_ok(s)) return;
  char* text = nullptr; size_t n = 0;
  hrx_status_ignore(hrx_status_to_string(s, &text, &n));
  std::string message = std::string(where) + ": " + (text ? std::string(text, n) : "HRX error");
  if (text) hrx_status_free_message(text);
  hrx_status_ignore(s);
  throw std::runtime_error(message);
}
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void stamp(const char* event) {
  const auto now = std::time(nullptr);
  std::tm utc{}; gmtime_r(&now, &utc);
  char date[40]; std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc);
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  const auto utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::printf("PHASE %s utc=%s utc_ms=%lld host_steady_ns=%lld sample_uptime_raw_ns=%llu\n", event, date, static_cast<long long>(utc_ms), static_cast<long long>(ns), static_cast<unsigned long long>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)));
}
void retire() {
  check(hrx_stream_flush(stream), "flush");
  hrx_timeline_point_t position{};
  check(hrx_stream_get_timeline_position(stream, &position), "timeline");
  if (position.value) check(hrx_semaphore_wait(position.semaphore, position.value, timeout_ns), "bounded30s retirement");
}
void copy(const Buffer& src, size_t so, const Buffer& dst, size_t to, size_t bytes) {
  check(hrx_stream_copy_buffer(stream, src.handle, so, dst.handle, to, bytes), "record copy");
}
Buffer allocate(size_t bytes, bool host) {
  Buffer b;
  const auto type = host ? HRX_MEMORY_TYPE_HOST_VISIBLE | HRX_MEMORY_TYPE_HOST_COHERENT | HRX_MEMORY_TYPE_DEVICE_VISIBLE
                         : HRX_MEMORY_TYPE_DEVICE_LOCAL;
  const auto usage = HRX_BUFFER_USAGE_DEFAULT | (host ? HRX_BUFFER_USAGE_MAPPING_PERSISTENT | HRX_BUFFER_USAGE_MAPPING_SCOPED : 0);
  check(hrx_buffer_allocate(stream, bytes, type, usage, &b.handle), "allocate");
  buffers.push_back(b); // Retain handle even if mapping or a later operation fails.
  size_t actual_size = 0;
  check(hrx_buffer_get_size(b.handle, &actual_size), "query allocation capacity");
  require(actual_size >= bytes, "allocated buffer is smaller than requested capacity");
  if (host) {
    void* mapping = nullptr;
    check(hrx_buffer_map(b.handle, HRX_MAP_READ | HRX_MAP_WRITE, 0, bytes, &mapping), "map shared host buffer");
    require(mapping != nullptr, "null host mapping");
    b.host = static_cast<unsigned char*>(mapping);
    buffers.back().host = b.host;
  }
  return b;
}
void release_buffers() {
  for (auto& b : buffers) {
    if (b.host) check(hrx_buffer_unmap(b.handle), "unmap after retirement");
    hrx_buffer_release(b.handle); b = {};
  }
  buffers.clear();
}
unsigned char pattern(size_t at, unsigned generation) {
  uint64_t x = uint64_t(at) + uint64_t(generation) * 0x9e3779b97f4a7c15ull;
  x ^= x >> 17; x *= 0xed5ad4bbull; x ^= x >> 23;
  return static_cast<unsigned char>(x ^ (x >> 8));
}
void seed(unsigned char* host, size_t bytes, unsigned generation, bool payload) {
  std::memset(host, 0xa5, bytes + 2 * guard);
  if (payload) for (size_t i = 0; i < bytes; ++i) host[guard + i] = pattern(i, generation);
  else std::memset(host + guard, 0x3c, bytes);
}
void verify(const unsigned char* host, size_t bytes, unsigned generation, const char* which, bool report = true) {
  for (size_t i = 0; i < bytes + 2 * guard; ++i) {
    const auto expected = i < guard || i >= guard + bytes ? 0xa5 : pattern(i - guard, generation);
    if (host[i] != expected) {
      if (report) std::fprintf(stderr, "MISMATCH %s offset=%zu expected=%02x actual=%02x\n", which, i, expected, unsigned(host[i]));
      throw std::runtime_error("payload or guard mismatch");
    }
  }
}
void sweep(size_t bytes, unsigned iterations, unsigned trials) {
  require(bytes > 0 && bytes <= max_payload && iterations > 0 && trials > 0,
          "invalid bounded sweep parameters");
  const size_t total = bytes + 2 * guard;
  Buffer host_src = allocate(total, true), host_dst = allocate(total, true);
  Buffer dev_src = allocate(total, false), dev_dst = allocate(total, false);
  retire();
  for (unsigned direction = 0; direction < 3; ++direction) {
    const char* label = direction == 0 ? "H2D" : direction == 1 ? "D2H" : "D2D";
    std::vector<double> seconds;
    for (unsigned trial = 0; trial <= trials; ++trial) {
      const unsigned generation = 19u + direction * 101u + trial * 13u;
      seed(host_src.host, bytes, generation, true);
      seed(host_dst.host, bytes, generation, false);
      copy(host_src, 0, dev_src, 0, total);
      copy(host_dst, 0, dev_dst, 0, total);
      retire();
      const Buffer& src = direction == 0 ? host_src : dev_src;
      const Buffer& dst = direction == 1 ? host_dst : dev_dst;
      const unsigned copies = trial == 0 ? 1 : iterations;
      const auto started = Clock::now();
      for (unsigned i = 0; i < copies; ++i) copy(src, guard, dst, guard, bytes);
      retire();
      const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
      // Validation and poison/upload preparation are outside timed intervals.
      if (direction != 1) { copy(dev_dst, 0, host_dst, 0, total); retire(); }
      verify(host_dst.host, bytes, generation, "destination including guards");
      verify(host_src.host, bytes, generation, "host source unchanged");
      copy(dev_src, 0, host_dst, 0, total); retire();
      verify(host_dst.host, bytes, generation, "device source unchanged");
      if (trial) {
        seconds.push_back(elapsed);
        std::printf("PASS path=mapped-buffer-copy direction=%s payload_bytes=%zu copies=%u trial=%u elapsed_ms=%.6f payload_GBps=%.6f payload_Gbps=%.6f full_payload=verified source_destination_guards=verified source_unchanged=yes clock=host-steady\n",
            label, bytes, copies, trial, elapsed * 1e3, double(bytes) * copies / elapsed / 1e9, double(bytes) * copies / elapsed / 1e9 * 8);
      }
    }
    std::sort(seconds.begin(), seconds.end());
    const double median = seconds[seconds.size() / 2];
    std::printf("SUMMARY path=mapped-buffer-copy direction=%s payload_bytes=%zu copies=%u trials=%u median_elapsed_ms=%.6f median_payload_GBps=%.6f",
        label, bytes, iterations, trials, median * 1e3, double(bytes) * iterations / median / 1e9);
    if (direction == 2) std::printf(" read_plus_write_GBps=%.6f", double(bytes) * iterations * 2 / median / 1e9);
    std::puts("");
  }
  retire(); release_buffers();
}
void convenience(size_t bytes) {
  require(bytes > 0 && bytes <= max_payload, "invalid convenience transfer size");
  const size_t total = bytes + 2 * guard;
  Buffer device_buffer = allocate(total, false);
  std::vector<unsigned char> input(total), output(total);
  seed(input.data(), bytes, 777, true);
  seed(output.data(), bytes, 777, false);
  check(hrx_synchronous_h2d(device, output.data(), device_buffer.handle, 0, total), "convenience guard/poison initialization");
  for (unsigned direction = 0; direction < 2; ++direction) {
    seed(output.data(), bytes, 777, false);
    const auto started = Clock::now();
    if (!direction) check(hrx_stream_copy_h2d(stream, input.data() + guard, device_buffer.handle, guard, bytes), "convenience stream H2D");
    else check(hrx_stream_copy_d2h(stream, device_buffer.handle, guard, output.data() + guard, bytes), "convenience stream D2H");
    retire();
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (!direction) check(hrx_synchronous_d2h(device, device_buffer.handle, 0, output.data(), total), "convenience full readback");
    verify(output.data(), bytes, 777, "convenience transfer");
    verify(input.data(), bytes, 777, "convenience host input unchanged");
    std::printf("PASS path=stream-copy-%s-convenience payload_bytes=%zu copies=1 elapsed_ms=%.6f payload_GBps=%.6f full_payload=verified source_destination_guards=verified clock=host-steady\n",
        direction ? "d2h" : "h2d", bytes, seconds * 1e3, double(bytes) / seconds / 1e9);
  }
  retire(); release_buffers();
}
}
int check_host() {
  for (size_t bytes : {size_t(1), size_t(4096), size_t(65536)}) {
    std::vector<unsigned char> image(bytes + 2 * guard);
    seed(image.data(), bytes, 42, true);
    verify(image.data(), bytes, 42, "host reference");
    for (size_t index : {size_t(0), guard - 1, guard, guard + bytes - 1,
                         guard + bytes, image.size() - 1}) {
      image[index] ^= 1;
      bool rejected = false;
      try { verify(image.data(), bytes, 42, "expected corruption", false); }
      catch (const std::runtime_error&) { rejected = true; }
      image[index] ^= 1;
      require(rejected, "reference failed to reject corrupted payload/guard");
    }
    seed(image.data(), bytes, 42, false);
    bool rejected = false;
    try { verify(image.data(), bytes, 42, "unwritten destination", false); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "reference accepted destination poison");
  }
  std::puts("PASS host pattern/poison and payload/prefix/suffix corruption checks; no GPU access.");
  return 0;
}
int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 2 && !std::strcmp(argv[1], "--check-host")) {
    try { return check_host(); }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
  }
  if (argc != 2 || std::strcmp(argv[1], "--run")) {
    std::fprintf(stderr, "Usage: %s --check-host|--run\n4 sizes 4 KiB..16 MiB, 8 copies/batch, 3 trials, full payload+guards. No GPU access without --run.\n", argv[0]); return 2;
  }
  try {
    check(hrx_gpu_initialize(0), "initialize"); initialized = true;
    int count = 0; check(hrx_gpu_device_count(&count), "device count"); require(count == 1, "need one GPU");
    check(hrx_gpu_device_get(0, &device), "device");
    check(hrx_stream_create(device, 0, &stream), "stream");
    std::puts("HRX bandwidth: reusable mapped host buffers <-> DEVICE_LOCAL buffers; CPU/GPU ownership transfers, no cross-agent RMW. Rates are decimal payload GB/s; D2D also reports 2x read+write traffic. Host steady timing includes 8-copy recording+flush+retirement; excludes allocations/initialization/verification. Repeated buffers: cache residency unverified, not asserted raw DRAM or SDMA bandwidth. Convenience APIs reported separately.");
    stamp("initialized-idle-start"); std::this_thread::sleep_for(std::chrono::seconds(3)); stamp("load-start");
    for (size_t bytes : {size_t(4096), size_t(65536), size_t(1) << 20, size_t(16) << 20}) sweep(bytes, 8, 3);
    convenience(size_t(1) << 20);
    stamp("load-end-idle-start"); std::this_thread::sleep_for(std::chrono::seconds(3));
    retire(); hrx_stream_release(stream); stream = nullptr;
    check(hrx_gpu_shutdown(), "shutdown"); initialized = false;
    stamp("clean-shutdown");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    bool safe = stream == nullptr;
    if (stream) try { retire(); safe = true; } catch (const std::exception& retry) { std::fprintf(stderr, "Retirement unconfirmed: %s; retaining HRX objects for process/driver teardown\n", retry.what()); }
    if (safe) {
      try { release_buffers(); if (stream) hrx_stream_release(stream); if (initialized) check(hrx_gpu_shutdown(), "failure shutdown"); }
      catch (const std::exception& cleanup) { std::fprintf(stderr, "Cleanup: %s\n", cleanup.what()); }
    }
    return 1;
  }
}
