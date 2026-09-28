#include <hsa/hsa_ext_amd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "hrx_runtime.h"
#include "mac_hsa.h"
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t guard = 4096;
constexpr uint64_t timeout_ns = 30000000000ULL;
constexpr uint64_t magic = 0xabcdef0198765432ULL;
constexpr uint64_t sentinel = 0x81726354aabbccddULL;
hrx_device_t device = nullptr;
hrx_stream_t stream = nullptr;
hrx_executable_t executable = nullptr;
std::ofstream bandwidth_csv, latency_csv;
std::atomic<bool> queue_error{false};
std::filesystem::path out;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void check(hrx_status_t s, const char* where) {
  if (hrx_status_is_ok(s)) return;
  char* text = nullptr;
  size_t n = 0;
  hrx_status_ignore(hrx_status_to_string(s, &text, &n));
  std::string message = std::string(where) + ": " + (text ? std::string(text, n) : "HRX error");
  if (text) hrx_status_free_message(text);
  hrx_status_ignore(s);
  throw std::runtime_error(message);
}
void hcheck(hsa_status_t s, const char* where) {
  if (!s) return;
  throw std::runtime_error(std::string(where) + ": HSA status " + std::to_string(s));
}
double ns(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::nano>(b - a).count();
}
void retire() {
  check(hrx_stream_flush(stream), "flush");
  hrx_timeline_point_t position{};
  check(hrx_stream_get_timeline_position(stream, &position), "timeline");
  if (position.value)
    check(hrx_semaphore_wait(position.semaphore, position.value, timeout_ns), "retirement");
}
void wait_current() {
  hrx_timeline_point_t position{};
  check(hrx_stream_get_timeline_position(stream, &position), "timeline");
  if (position.value)
    check(hrx_semaphore_wait(position.semaphore, position.value, timeout_ns), "retirement");
}
struct Buffer {
  hrx_buffer_t handle = nullptr;
  unsigned char* host = nullptr;
};
std::vector<Buffer> buffers;
Buffer allocate(size_t bytes, bool host) {
  Buffer b;
  const auto type = host ? HRX_MEMORY_TYPE_HOST_VISIBLE | HRX_MEMORY_TYPE_HOST_COHERENT |
                               HRX_MEMORY_TYPE_DEVICE_VISIBLE
                         : HRX_MEMORY_TYPE_DEVICE_LOCAL;
  const auto usage =
      HRX_BUFFER_USAGE_DEFAULT |
      (host ? HRX_BUFFER_USAGE_MAPPING_PERSISTENT | HRX_BUFFER_USAGE_MAPPING_SCOPED : 0);
  check(hrx_buffer_allocate(stream, bytes, type, usage, &b.handle), "allocate");
  buffers.push_back(b);
  size_t capacity = 0;
  check(hrx_buffer_get_size(b.handle, &capacity), "allocation size");
  require(capacity >= bytes, "allocation smaller than request");
  if (host) {
    void* mapping = nullptr;
    check(hrx_buffer_map(b.handle, HRX_MAP_READ | HRX_MAP_WRITE, 0, bytes, &mapping),
          "map shared host buffer");
    require(mapping != nullptr, "null host mapping");
    b.host = static_cast<unsigned char*>(mapping);
    buffers.back().host = b.host;
  }
  return b;
}
void free_buffers() {
  retire();
  for (auto& b : buffers) {
    if (b.host) check(hrx_buffer_unmap(b.handle), "unmap");
    hrx_buffer_release(b.handle);
  }
  buffers.clear();
}
void copy(const Buffer& src, size_t so, const Buffer& dst, size_t to, size_t bytes) {
  check(hrx_stream_copy_buffer(stream, src.handle, so, dst.handle, to, bytes), "record copy");
}
unsigned char pattern(size_t at, unsigned generation) {
  uint64_t x = uint64_t(at) + uint64_t(generation) * 0x9e3779b97f4a7c15ULL;
  x ^= x >> 17;
  x *= 0xed5ad4bbULL;
  x ^= x >> 23;
  return static_cast<unsigned char>(x ^ (x >> 8));
}
void seed(unsigned char* host, size_t bytes, unsigned generation, bool payload) {
  std::memset(host, 0xa5, bytes + 2 * guard);
  if (payload)
    for (size_t i = 0; i < bytes; ++i) host[guard + i] = pattern(i, generation);
  else
    std::memset(host + guard, 0x3c, bytes);
}
void verify(const unsigned char* host, size_t bytes, unsigned generation) {
  for (size_t i = 0; i < bytes + 2 * guard; ++i) {
    const auto expected = i < guard || i >= guard + bytes ? 0xa5 : pattern(i - guard, generation);
    require(host[i] == expected, "payload/guard mismatch");
  }
}
void bandwidth(size_t bytes) {
  const size_t total = bytes + 2 * guard;
  const auto hs = allocate(total, true), hd = allocate(total, true);
  const auto ds = allocate(total, false), dd = allocate(total, false);
  retire();
  for (unsigned direction = 0; direction < 2; ++direction) {
    const char* label = direction ? "D2H" : "H2D";
    for (unsigned trial = 0; trial <= 3; ++trial) {
      const unsigned generation = 19 + direction * 101 + trial * 13;
      seed(hs.host, bytes, generation, true);
      seed(hd.host, bytes, generation, false);
      copy(hs, 0, ds, 0, total);
      copy(hd, 0, dd, 0, total);
      retire();
      const auto& src = direction ? ds : hs;
      const auto& dst = direction ? hd : dd;
      // Warmup uses the same eight-copy batch as a measured trial.
      const auto start = Clock::now();
      for (unsigned n = 0; n < 8; ++n) copy(src, guard, dst, guard, bytes);
      retire();
      const auto stop = Clock::now();
      if (!direction) {
        copy(dd, 0, hd, 0, total);
        retire();
      }
      verify(hd.host, bytes, generation);
      verify(hs.host, bytes, generation);
      copy(ds, 0, hd, 0, total);
      retire();
      verify(hd.host, bytes, generation);
      if (trial) {
        const double seconds = ns(start, stop) / 1e9;
        bandwidth_csv << label << ',' << bytes << ",8," << trial << ',' << seconds << ','
                      << bytes * 8.0 / seconds / 1e9 << ",verified\n";
        bandwidth_csv.flush();
        std::printf("PASS %s bytes=%zu trial=%u %.6fGB/s\n", label, bytes, trial,
                    bytes * 8.0 / seconds / 1e9);
      }
    }
  }
  free_buffers();
}
void tiny_transfers() {
  const size_t bytes = 8, total = bytes + 2 * guard;
  const auto hs = allocate(total, true), hd = allocate(total, true);
  const auto ds = allocate(total, false), dd = allocate(total, false);
  seed(hs.host, bytes, 71, true);
  seed(hd.host, bytes, 71, false);
  copy(hs, 0, ds, 0, total);
  copy(hd, 0, dd, 0, total);
  retire();
  for (unsigned direction = 0; direction < 2; ++direction) {
    const auto& src = direction ? ds : hs;
    const auto& dst = direction ? hd : dd;
    std::vector<double> times;
    for (unsigned i = 0; i < 32 + 256; ++i) {
      const auto start = Clock::now();
      copy(src, guard, dst, guard, bytes);
      retire();
      const auto stop = Clock::now();
      if (i >= 32) times.push_back(ns(start, stop));
    }
    if (!direction) {
      copy(dd, 0, hd, 0, total);
      retire();
    }
    verify(hd.host, bytes, 71);
    verify(hs.host, bytes, 71);
    for (unsigned i = 0; i < times.size(); ++i)
      latency_csv << (direction ? "d2h_8byte_completion" : "h2d_8byte_completion")
                  << ",host_steady,1," << i << ',' << times[i] << '\n';
    std::printf("PASS %s 8byte latency n=%zu payload/guards verified\n", direction ? "D2H" : "H2D",
                times.size());
  }
  free_buffers();
}
void kernel_latency(const std::vector<char>& code) {
  check(
      hrx_executable_load_data(device, code.data(), code.size(), "amdgpu", "gfx1201", &executable),
      "load latency executable");
  uint32_t ordinal = 0;
  hrx_executable_export_info_t info{};
  check(hrx_executable_lookup_export_by_name(executable, "hrx_touch", &ordinal), "touch export");
  check(hrx_executable_export_info(executable, ordinal, &info), "touch ABI");
  require(info.binding_count == 1 && info.constant_byte_length == 4 && info.workgroup_size[0] == 32,
          "touch ABI mismatch");
  const auto data = allocate(2 * guard + 4, true);
  retire();
  const hrx_buffer_ref_t binding{data.handle, guard, 4};
  const hrx_dispatch_config_t config{{1, 1, 1}, {32, 1, 1}, 32};
  for (unsigned i = 0; i < 32 + 200; ++i) {
    std::memset(data.host, 0xa5, 2 * guard + 4);
    const uint32_t sequence = 0x31415000U + i;
    const auto start = Clock::now();
    check(hrx_stream_dispatch(stream, executable, ordinal, &config, &sequence, 4, &binding, 1,
                              HRX_DISPATCH_FLAG_NONE),
          "touch dispatch");
    const auto recorded = Clock::now();
    check(hrx_stream_flush(stream), "touch flush");
    const auto issued = Clock::now();
    wait_current();
    const auto completed = Clock::now();
    uint32_t observed = 0;
    std::memcpy(&observed, data.host + guard, 4);
    require(observed == sequence, "touch output mismatch");
    for (size_t p = 0; p < 2 * guard + 4; ++p)
      if (p < guard || p >= guard + 4) require(data.host[p] == 0xa5, "touch guard mismatch");
    if (i >= 32) {
      const unsigned n = i - 32;
      latency_csv << "hrx_kernel_record,host_steady,1," << n << ',' << ns(start, recorded) << '\n';
      latency_csv << "hrx_kernel_issue,host_steady,1," << n << ',' << ns(start, issued) << '\n';
      latency_csv << "hrx_kernel_completion,host_steady,1," << n << ',' << ns(start, completed)
                  << '\n';
      latency_csv << "hrx_kernel_post_issue_wait,host_steady,1," << n << ','
                  << ns(issued, completed) << '\n';
    }
  }
  std::puts("PASS HRX touch latency n=200 output/guards verified");
  free_buffers();
  hrx_executable_release(executable);
  executable = nullptr;
}
uint64_t load(uint64_t* data, size_t i) {
  return std::atomic_ref<uint64_t>(data[i]).load(std::memory_order_acquire);
}
void store(uint64_t* data, size_t i, uint64_t v) {
  std::atomic_ref<uint64_t>(data[i]).store(v, std::memory_order_release);
}
void publish(hsa_queue_t* q, const hsa_kernel_dispatch_packet_t& packet) {
  const auto index = hsa_queue_add_write_index_relaxed(q, 1);
  require(index - hsa_queue_load_read_index_scacquire(q) < q->size, "queue full");
  auto* slot = static_cast<uint8_t*>(q->base_address) + (index & (q->size - 1)) * 64;
  std::memcpy(slot + 4, reinterpret_cast<const uint8_t*>(&packet) + 4, 60);
  uint32_t header;
  std::memcpy(&header, &packet, 4);
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(slot))
      .store(header, std::memory_order_release);
  hsa_signal_store_screlease(q->doorbell_signal, static_cast<int64_t>(index));
  require(!queue_error, "queue error");
}
bool mutable_word(size_t i, size_t rounds) {
  return i == 0 || i == 16 || i == 32 || i == 48 || i == 64 || i == 80 || i == 96 ||
         (i >= 128 && i < 128 + rounds);
}
void hsa_latencies(const std::vector<char>& code) {
  hsa_agent_t gpu{};
  hsa_code_object_reader_t reader{};
  hsa_executable_t exe{};
  hsa_queue_t* queue = nullptr;
  void* data_memory = nullptr;
  void* args_memory = nullptr;
  std::vector<hsa_signal_t> signals;
  bool safe = true;
  hcheck(hsa_init(), "HSA init");
  try {
    hcheck(hsa_iterate_agents(
               [](hsa_agent_t a, void* p) {
                 hsa_device_type_t type;
                 const auto s = hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &type);
                 if (!s && type == HSA_DEVICE_TYPE_GPU && !static_cast<hsa_agent_t*>(p)->handle)
                   *static_cast<hsa_agent_t*>(p) = a;
                 return s;
               },
               &gpu),
           "HSA agents");
    require(gpu.handle != 0, "GPU missing");
    uint64_t frequency = 0;
    hcheck(hsa_agent_get_info(gpu, static_cast<hsa_agent_info_t>(0xA016), &frequency),
           "GPU timestamp frequency");
    require(frequency != 0, "GPU frequency zero");
    mac_hsa_device_info_t identity{};
    hcheck(mac_hsa_agent_get_driver_info(gpu, &identity, sizeof(identity)), "driver identity");
    char name[64]{};
    hcheck(hsa_agent_get_info(gpu, HSA_AGENT_INFO_NAME, name), "GPU name");
    std::ofstream identity_json(out / "gpu.json");
    identity_json << "{\"gpu\":\"" << name << "\",\"driver_build\":" << identity.driver_build
                  << ",\"gfx_major\":" << identity.gfx_major
                  << ",\"gfx_minor\":" << identity.gfx_minor
                  << ",\"gfx_revision\":" << identity.gfx_revision
                  << ",\"total_vram_bytes\":" << identity.total_vram_bytes
                  << ",\"timestamp_frequency_hz\":" << frequency << "}\n";
    hcheck(hsa_code_object_reader_create_from_memory(code.data(), code.size(), &reader), "reader");
    hcheck(hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,
                                     nullptr, &exe),
           "executable");
    hcheck(hsa_executable_load_agent_code_object(exe, gpu, reader, nullptr, nullptr), "load code");
    hcheck(hsa_executable_freeze(exe, nullptr), "freeze code");
    hcheck(mac_hsa_memory_allocate_shared(gpu, 16384, &data_memory), "mailbox memory");
    hcheck(mac_hsa_memory_allocate_shared(gpu, 16384, &args_memory), "argument memory");
    uint32_t sync = 0;
    hcheck(mac_hsa_memory_get_sync_capabilities(gpu, data_memory, &sync), "mapping capabilities");
    require((sync & MAC_HSA_SYNC_OWNERSHIP_TRANSFER) != 0, "ownership transfer unsupported");
    hcheck(
        hsa_queue_create(
            gpu, 64, HSA_QUEUE_TYPE_MULTI,
            [](hsa_status_t, hsa_queue_t*, void*) { queue_error = true; }, nullptr, 0, 0, &queue),
        "queue");
    hcheck(hsa_amd_profiling_set_profiler_enabled(queue, 1), "CP profiling");
    auto* data = static_cast<uint64_t*>(data_memory);
    auto* args = static_cast<uint64_t*>(args_memory);
    auto packet_for = [&](const char* export_name, hsa_signal_t done, uint32_t expected_size) {
      hsa_executable_symbol_t symbol{};
      uint64_t kernel = 0;
      uint32_t size = 0;
      const std::string symbol_name = std::string(export_name) + ".kd";
      hcheck(hsa_executable_get_symbol_by_name(exe, symbol_name.c_str(), &gpu, &symbol),
             "kernel symbol");
      hcheck(
          hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kernel),
          "kernel object");
      hcheck(hsa_executable_symbol_get_info(
                 symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE, &size),
             "kernarg size");
      require(size == expected_size, "HSA kernarg ABI mismatch");
      hcheck(hsa_executable_symbol_get_info(
                 symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE, &size),
             "scratch size");
      require(size == 0, "unexpected scratch");
      hcheck(hsa_executable_symbol_get_info(
                 symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE, &size),
             "LDS size");
      require(size == 0, "unexpected LDS");
      hsa_kernel_dispatch_packet_t p{};
      p.header = HSA_PACKET_TYPE_KERNEL_DISPATCH |
                 (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCACQUIRE_FENCE_SCOPE) |
                 (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_SCRELEASE_FENCE_SCOPE);
      p.setup = 1 << HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS;
      p.workgroup_size_x = p.grid_size_x = 32;
      p.workgroup_size_y = p.workgroup_size_z = p.grid_size_y = p.grid_size_z = 1;
      p.kernel_object = kernel;
      p.kernarg_address = args;
      p.completion_signal = done;
      return p;
    };
    auto fresh_done = [&]() {
      hsa_signal_t done{};
      hcheck(hsa_signal_create(1, 1, &gpu, &done), "completion signal");
      signals.push_back(done);
      return done;
    };
    auto wait_done = [&](hsa_signal_t done) {
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (hsa_signal_load_scacquire(done) != 0) {
        if (queue_error || Clock::now() >= deadline) {
          safe = false;
          throw std::runtime_error("HSA completion timeout");
        }
      }
    };
    // Fresh signal per profiled dispatch; publish and readout are paired exactly.
    for (unsigned i = 0; i < 32 + 200; ++i) {
      std::fill(data, data + 2048, sentinel);
      std::fill(args, args + 2048, sentinel);
      const uint32_t value = 0x51416000U + i;
      args[0] = reinterpret_cast<uintptr_t>(data);
      std::memcpy(reinterpret_cast<char*>(args) + 8, &value, 4);
      const auto done = fresh_done();
      const auto packet = packet_for("hrx_touch", done, 12);
      const auto start = Clock::now();
      publish(queue, packet);
      const auto issued = Clock::now();
      wait_done(done);
      const auto completed = Clock::now();
      require(static_cast<uint32_t>(data[0]) == value && (data[0] >> 32) == (sentinel >> 32),
              "HSA touch mismatch");
      for (unsigned j = 1; j < 2048; ++j) require(data[j] == sentinel, "HSA touch guard changed");
      for (unsigned j = 2; j < 2048; ++j) require(args[j] == sentinel, "HSA arg guard changed");
      mac_hsa_dispatch_timestamps_t stamps{};
      hcheck(mac_hsa_dispatch_timestamps(queue, done, &stamps, sizeof(stamps)), "CP stamps");
      require(stamps.end_ticks >= stamps.start_ticks && stamps.frequency_hz == frequency &&
                  stamps.clock_domain == MAC_HSA_TIMESTAMP_DOMAIN_GPU,
              "CP clock mismatch");
      if (i >= 32) {
        const auto n = i - 32;
        latency_csv << "hsa_kernel_issue,host_steady,1," << n << ',' << ns(start, issued) << '\n';
        latency_csv << "hsa_kernel_completion,host_steady,1," << n << ',' << ns(start, completed)
                    << '\n';
        latency_csv << "hsa_kernel_cp_duration,gpu_agent,1," << n << ','
                    << (stamps.end_ticks - stamps.start_ticks) * 1e9 / frequency << '\n';
      }
      hcheck(hsa_signal_destroy(done), "signal destruction");
      signals.pop_back();
    }
    std::puts("PASS bare HSA touch n=200 paired raw CP timestamps/output/guards");
    constexpr size_t warmup = 128, count = 1024, rounds = warmup + count;
    for (unsigned gpu_first = 0; gpu_first < 2; ++gpu_first) {
      for (unsigned trial = 1; trial <= 3; ++trial) {
        std::fill(data, data + 2048, sentinel);
        std::fill(args, args + 2048, sentinel);
        for (size_t i = 0; i < 2048; ++i)
          if (mutable_word(i, rounds)) data[i] = 0;
        args[0] = reinterpret_cast<uintptr_t>(data);
        args[1] = rounds;
        std::memcpy(reinterpret_cast<char*>(args) + 16, &gpu_first, 4);
        const auto done = fresh_done();
        const auto packet = packet_for("latency_mailbox", done, 20);
        publish(queue, packet);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        auto wait_word = [&](size_t word, uint64_t wanted) {
          while (load(data, word) != wanted) {
            if (Clock::now() >= deadline || queue_error || load(data, 48) >= 2) {
              store(data, 32, 1);
              wait_done(done);
              throw std::runtime_error("mailbox timeout/state error");
            }
            require(load(data, word) <= wanted, "mailbox skipped sequence");
          }
        };
        wait_word(16, 1);
        store(data, 80, 1);
        std::vector<double> host_times;
        for (uint64_t round = 1; round <= rounds; ++round) {
          if (gpu_first) {
            wait_word(0, round * 2 - 1);
            require(load(data, 64) == round, "GPU request payload mismatch");
            store(data, 64, round ^ magic);
            store(data, 0, round * 2);
          } else {
            store(data, 64, round);
            const auto start = Clock::now();
            store(data, 0, round * 2 - 1);
            wait_word(0, round * 2);
            const auto stop = Clock::now();
            require(load(data, 64) == (round ^ magic), "GPU response payload mismatch");
            if (round > warmup) host_times.push_back(ns(start, stop));
          }
        }
        wait_done(done);
        require(load(data, 48) == 1 && load(data, 96) == rounds, "mailbox incomplete");
        for (size_t i = 0; i < 2048; ++i)
          if (!mutable_word(i, rounds)) require(data[i] == sentinel, "mailbox guard changed");
        for (size_t i = 3; i < 2048; ++i)
          require(args[i] == sentinel, "mailbox argument guard changed");
        for (size_t i = 0; i < count; ++i) {
          const double value = gpu_first ? data[128 + warmup + i] * 1e9 / frequency : host_times[i];
          require(value > 0, "zero mailbox duration");
          latency_csv << (gpu_first ? "gpu_cpu_gpu_mailbox_rtt" : "cpu_gpu_cpu_mailbox_rtt") << ','
                      << (gpu_first ? "gpu_agent" : "host_steady") << ',' << trial << ',' << i
                      << ',' << value << '\n';
        }
        hcheck(hsa_signal_destroy(done), "mailbox completion destroy");
        signals.pop_back();
        std::printf("PASS %s mailbox trial=%u n=1024+128warm sequence/payload/guards\n",
                    gpu_first ? "GPU→CPU→GPU" : "CPU→GPU→CPU", trial);
      }
    }
    hcheck(hsa_queue_destroy(queue), "queue destroy");
    queue = nullptr;
    hcheck(hsa_memory_free(args_memory), "argument free");
    args_memory = nullptr;
    hcheck(hsa_memory_free(data_memory), "mailbox free");
    data_memory = nullptr;
    hcheck(hsa_executable_destroy(exe), "executable destroy");
    exe.handle = 0;
    hcheck(hsa_code_object_reader_destroy(reader), "reader destroy");
    reader.handle = 0;
    hcheck(hsa_shut_down(), "HSA shutdown");
  } catch (...) {
    if (queue && hsa_queue_destroy(queue)) safe = false;
    if (safe) {
      for (auto signal : signals) hsa_signal_destroy(signal);
      if (args_memory) hsa_memory_free(args_memory);
      if (data_memory) hsa_memory_free(data_memory);
      if (exe.handle) hsa_executable_destroy(exe);
    }
    if (reader.handle) hsa_code_object_reader_destroy(reader);
    if (safe) hsa_shut_down();
    throw;
  }
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 3 || std::strcmp(argv[1], "--run")) return 2;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  out = argv[2];
  std::ifstream file(out / "latency.hsaco", std::ios::binary);
  const std::vector<char> code{std::istreambuf_iterator<char>(file), {}};
  if (code.empty()) return 2;
  bandwidth_csv.open(out / "bandwidth.csv");
  latency_csv.open(out / "latency.csv");
  bandwidth_csv.precision(17);
  latency_csv.precision(17);
  bandwidth_csv << "direction,payload_bytes,copies,trial,seconds,GBps,validation\n";
  latency_csv << "test,clock,trial,sample,ns\n";
  bool initialized = false;
  try {
    check(hrx_gpu_initialize(0), "HRX init");
    initialized = true;
    check(hrx_gpu_device_get(0, &device), "HRX device");
    check(hrx_stream_create(device, 0, &stream), "HRX stream");
    for (size_t bytes : {size_t(4) << 20, size_t(16) << 20, size_t(64) << 20}) bandwidth(bytes);
    tiny_transfers();
    kernel_latency(code);
    retire();
    hrx_stream_release(stream);
    stream = nullptr;
    check(hrx_gpu_shutdown(), "HRX shutdown");
    initialized = false;
    hsa_latencies(code);
    latency_csv.flush();
    std::puts("PASS four-category suite; all queues/buffers/executables retired and released");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    bool safe = stream == nullptr;
    if (stream) try {
        retire();
        safe = true;
      } catch (...) {
      }
    if (safe) {
      try {
        if (stream) {
          free_buffers();
          hrx_stream_release(stream);
        }
        if (executable) hrx_executable_release(executable);
        if (initialized) check(hrx_gpu_shutdown(), "failure shutdown");
      } catch (...) {
      }
    }
    return 1;
  }
}
