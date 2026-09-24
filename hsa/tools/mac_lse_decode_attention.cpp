#include "lse/backends/hrx/arch_database.hpp"
#include "lse/backends/hrx/loomc/loom_emitter.hpp"
#include "lse/backends/hrx/loomc/loomc_compiler.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include "lse/graph/program.hpp"
#include "lse/kv/block.hpp"
#include "lse/place/devices.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace lse;
using namespace lse::graph;
void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
void check(const Status &status) {
  if (!status.ok())
    throw std::runtime_error(status.to_string());
}
Array leaf(Shape shape) {
  auto n = std::make_shared<Node>();
  n->set_kind(OpKind::kBuffer);
  n->shape = shape;
  n->dtype = DType::kF32;
  n->materialized = true;
  return Array(n);
}
Array attention(Array q, Array k, Array v, Array meta, Array table,
                MaskKind mask = MaskKind::kCausal, int window = 0) {
  return sdpa_paged(q, k, v, 0.0625f, mask, window, meta, table, 16);
}
int compile_only(const std::string &path) {
  backend::DeviceInfo device;
  device.arch = "gfx1201";
  device.compute_units = 64;
  device.wavefront_size = 32;
  device.max_threads_per_workgroup = 1024;
  device.lds_bytes_per_workgroup = 65536;
  backend::AmdDeviceInfo amd;
  backend::apply_arch_defaults(device, amd);
  device.extension_id = backend::AmdDeviceInfo::kExtensionId;
  device.extension = &amd;
  backend::LoomEmitter emitter;
  backend::LoomcCompiler compiler;
  for (int batch : {1, 2})
    for (int capacity : {128, 512, 8192})
      for (int mode = 0; mode < 4; ++mode) {
        auto out = attention(leaf(Shape{batch, 24, 1, 256}),
                             leaf(Shape{capacity / 16 + 1, 4, 16, 256}),
                             leaf(Shape{capacity / 16 + 1, 4, 16, 256}),
                             leaf(Shape{kv::step_meta_elems(batch)}),
                             leaf(Shape{batch, capacity / 16}),
                             mode == 0   ? MaskKind::kCausal
                             : mode == 1 ? MaskKind::kNone
                                         : MaskKind::kSlidingWindow,
                             mode == 3 ? 0 : 7);
        const NodePtr roots[]{out.node()};
        auto groups = Partitioner::partition(roots, &device);
        require(groups.size() == 1, "one attention group expected");
        auto emitted = emitter.emit(groups[0], device);
        check(emitted.status());
        require(emitted->dims.workgroup_count[0] == unsigned(batch * 24) &&
                    emitted->dims.workgroup_size[0] == 256,
                "shared kernel plan not selected");
        const auto stem = path + "-b" + std::to_string(batch) + "-c" +
                          std::to_string(capacity) + "-m" +
                          std::to_string(mode);
        std::ofstream(stem + ".loom") << emitted->source;
        auto object = compiler.compile(emitted->source, "gfx1201");
        check(object.status());
        std::ofstream file(stem + ".hsaco", std::ios::binary);
        file.write(reinterpret_cast<const char *>(object->code.data()),
                   static_cast<std::streamsize>(object->code.size()));
        std::printf("COMPILE PASS batch=%d capacity=%d mode=%d bytes=%zu\n",
                    batch, capacity, mode, object->code.size());
      }
  // Decline unsupported shape/device/LDS contracts, retaining the existing
  // attention primitive rather than attempting an oversized shared allocation.
  for (int variant = 0; variant < 4; ++variant) {
    const int dim = variant == 0 ? 128 : 256, query = variant == 1 ? 2 : 1;
    auto out =
        attention(leaf(Shape{1, 24, query, dim}), leaf(Shape{9, 4, 16, dim}),
                  leaf(Shape{9, 4, 16, dim}),
                  leaf(Shape{kv::step_meta_elems(1)}), leaf(Shape{1, 8}));
    auto fallback = device;
    if (variant == 2)
      fallback.arch = "gfx1100";
    if (variant == 3) {
      fallback.extension = nullptr;
      fallback.extension_id = {};
      fallback.lds_bytes_per_workgroup = 0;
    }
    const NodePtr roots[]{out.node()};
    auto groups = Partitioner::partition(roots, &fallback);
    auto emitted = emitter.emit(groups[0], fallback);
    check(emitted.status());
    require(emitted->source.find("buffer.alloca") == std::string::npos,
            "unsupported shared kernel selected");
    std::printf("FALLBACK PASS variant=%d\n", variant);
  }
  return 0;
}
struct Input {
  Array a;
  backend::DeviceBuffer allocation;
  std::vector<std::byte> image;
};
int run_gpu(bool benchmark, int maskMode) {
  check(place::open_default_devices("hrx:0"));
  auto *devices = place::default_devices();
  require(devices && devices->size() == 1, "one GPU required");
  auto &be = devices->device(devices->primary());
  require(be.device_info().arch == "gfx1201", "gfx1201 required");
  auto *scheduler = default_scheduler();
  require(scheduler, "scheduler required");
  scheduler->set_dialect(Dialect::kLoom);
  auto upload = [&](const std::vector<float> &data, Shape shape) {
    Input in;
    in.image.assign(data.size() * 4 + 128, std::byte{0xa5});
    std::memcpy(in.image.data() + 64, data.data(), data.size() * 4);
    auto allocated =
        be.allocate(in.image.size(), backend::MemoryClass::kDevice);
    check(allocated.status());
    in.allocation = allocated.release();
    check(be.copy_h2d(in.image.data(), in.allocation, in.image.size(), 0));
    auto view = in.allocation;
    view.offset += 64;
    view.size_bytes = data.size() * 4;
    in.a = Array::from_buffer(view, shape, DType::kF32);
    return in;
  };
  constexpr int batch = 2, heads = 24, kvheads = 4, dim = 256, block = 16;
  struct Fixture {
    int capacity, length;
    bool live_zero = false;
  };
  std::vector<Fixture> fixtures;
  for (int length : benchmark
                        ? std::vector<int>{5, 64, 128}
                        : std::vector<int>{0, 1, 5, 17, 63, 64, 65, 127, 128})
    fixtures.push_back({128, length});
  if (!benchmark) {
    fixtures.push_back({128, 0, true});
    for (int length : {255, 256, 257, 511, 512})
      fixtures.push_back({512, length});
  }
  const auto modes = maskMode >= 0 ? std::vector<int>{maskMode}
                     : benchmark   ? std::vector<int>{0}
                                   : std::vector<int>{0, 1, 2, 3};
  for (const auto fixture : fixtures)
    for (int mode : modes) {
      const int capacity = fixture.capacity, length = fixture.length,
                stride = capacity / block, blocks = 1 + batch * stride;
      const int liveRows = fixture.live_zero ? 1
                           : length          ? (length == 1 ? 1 : 2)
                                             : 0;
      std::vector<float> q(batch * heads * dim),
          k(blocks * kvheads * block * dim), v(k.size()),
          meta(kv::step_meta_elems(batch), 0), table(batch * capacity / block);
      for (size_t i = 0; i < q.size(); ++i)
        q[i] = float(int(i % 19) - 9) / 32;
      for (size_t i = 0; i < k.size(); ++i) {
        k[i] = float(int(i % 23) - 11) / 64;
        v[i] = float(int(i % 29) - 14) / 16;
      }
      for (int b = 0; b < batch; ++b)
        for (int j = 0; j < capacity / block; ++j)
          table[b * stride + j] = float(1 + b * stride + (stride - 1 - j));
      meta[0] = float(std::max(0, length - 1));
      meta[1] = float(length);
      meta[2] = float(liveRows);
      for (int b = 0; b < batch; ++b) {
        meta[3 + 2 * b] = float(std::max(0, length - 1 - b));
        meta[4 + 2 * b] = float(b < liveRows ? std::max(0, length - b) : 0);
      }
      auto qi = upload(q, Shape{batch, heads, 1, dim}),
           ki = upload(k, Shape{blocks, kvheads, block, dim}),
           vi = upload(v, Shape{blocks, kvheads, block, dim});
      auto mi = upload(meta, Shape{kv::step_meta_elems(batch)}),
           ti = upload(table, Shape{batch, stride});
      auto mask = mode == 0   ? MaskKind::kCausal
                  : mode == 1 ? MaskKind::kNone
                              : MaskKind::kSlidingWindow;
      const int window = mode == 3 ? 0 : 7;
      auto out = attention(qi.a, ki.a, vi.a, mi.a, ti.a, mask, window);
      std::vector<float> poison(q.size(),
                                std::numeric_limits<float>::quiet_NaN());
      auto oi = upload(poison, out.shape());
      out.node()->buffer = oi.a.node()->buffer;
      struct Retirement {
        backend::IBackend &backend;
        ~Retirement() {
          const auto status = backend.synchronize();
          if (!status.ok()) {
            std::fprintf(stderr,
                         "unconfirmed GPU retirement; terminating before "
                         "allocation release: %s\n",
                         status.to_string().c_str());
            std::_Exit(1);
          }
        }
      } retirement{be};
      const NodePtr roots[]{out.node()};
      Program program;
      check(scheduler->eval(roots, false, &program));
      check(be.synchronize());
      std::uint64_t outputHash = 0;
      auto validate = [&] {
        require(out.node()->buffer.handle == oi.a.node()->buffer.handle &&
                    out.node()->buffer.ptr == oi.a.node()->buffer.ptr &&
                    out.node()->buffer.offset == oi.a.node()->buffer.offset,
                "guarded output storage replaced");
        std::vector<float> actual(q.size());
        check(be.copy_d2h(out.node()->buffer, actual.data(), actual.size() * 4,
                          0));
        outputHash = 14695981039346656037ull;
        const auto *bytes =
            reinterpret_cast<const unsigned char *>(actual.data());
        for (size_t i = 0; i < actual.size() * sizeof(float); ++i) {
          outputHash ^= bytes[i];
          outputHash *= 1099511628211ull;
        }
        double maxerr = 0;
        for (int b = 0; b < batch; ++b)
          for (int h = 0; h < heads; ++h) {
            const int len = int(meta[4 + 2 * b]), off = int(meta[3 + 2 * b]);
            std::vector<float> scores(size_t(len), 0);
            float maximum = -INFINITY;
            auto address = [&](int j, int d) {
              int blk = int(table[b * stride + j / 16]);
              return ((blk * kvheads + h / 6) * 16 + j % 16) * 256 + d;
            };
            auto allowed = [&](int j) {
              return mode == 1 || (j <= off && (mode == 0 || off - j < window));
            };
            for (int j = 0; j < len; ++j) {
              float sum = 0;
              for (int d = 0; d < 256; ++d)
                sum = std::fma(q[(b * heads + h) * 256 + d], k[address(j, d)],
                               sum);
              scores[size_t(j)] = sum * 0.0625f;
              if (allowed(j))
                maximum = std::max(maximum, scores[size_t(j)]);
            }
            for (int d = 0; d < 256; ++d) {
              float den = 0, sum = 0;
              for (int j = 0; j < len; ++j) {
                float weight =
                    allowed(j) ? std::exp(scores[size_t(j)] - maximum) : 0;
                den += weight;
                sum = std::fma(weight, v[address(j, d)], sum);
              }
              float expected = b < liveRows ? sum / (den == 0 ? 1 : den) : 0;
              float got = actual[size_t((b * heads + h) * 256 + d)];
              double error = std::abs(double(got) - expected);
              require(std::isfinite(got) &&
                          error <= 2e-5 + 2e-5 * std::abs(expected),
                      "attention result mismatch");
              maxerr = std::max(maxerr, error);
            }
          }
        for (auto *input : {&qi, &ki, &vi, &mi, &ti, &oi}) {
          std::vector<std::byte> read(input->image.size());
          check(be.copy_d2h(input->allocation, read.data(), read.size(), 0));
          require(std::equal(read.begin(), read.begin() + 64,
                             input->image.begin()) &&
                      std::equal(read.end() - 64, read.end(),
                                 input->image.end() - 64),
                  "guard corrupted");
          if (input != &oi)
            require(read == input->image, "input modified");
        }
        return maxerr;
      };
      const double maxerr = validate();
      const auto trace = scheduler->last_trace();
      require(trace.device_groups == 1 && trace.kernels_launched == 1 &&
                  !trace.host_groups && !trace.host_fallbacks,
              "one GPU kernel required");
      for (int repeats :
           benchmark ? std::vector<int>{1, 8, 64} : std::vector<int>{1}) {
        const auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < repeats; ++r) {
          program.reset_compute();
          check(scheduler->eval(roots, false, &program));
        }
        check(be.synchronize());
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        validate();
        std::printf("PASS capacity=%d length=%d mode=%d rows=%d live_zero=%d "
                    "outputs=%zu output_hash=%016llx max_error=%g repeats=%d "
                    "host_dispatch_retire_us=%.3f per_dispatch_us=%.3f "
                    "gpu_timestamps=no\n",
                    capacity, length, mode, liveRows, fixture.live_zero ? 1 : 0,
                    q.size(), static_cast<unsigned long long>(outputHash),
                    maxerr, repeats, double(ns) / 1e3,
                    double(ns) / repeats / 1e3);
      }
    }
  check(be.synchronize());
  return 0;
}
int main(int argc, char **argv) {
  try {
    if (argc == 3 && !std::strcmp(argv[1], "--compile-only"))
      return compile_only(argv[2]);
    if (argc >= 2 && !std::strcmp(argv[1], "--run")) {
      bool benchmark = false;
      int maskMode = -1;
      for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--benchmark") && !benchmark)
          benchmark = true;
        else if (!std::strcmp(argv[i], "--mask-mode") && maskMode < 0 &&
                 i + 1 < argc) {
          const std::string mode = argv[++i];
          if (mode == "causal")
            maskMode = 0;
          else if (mode == "none")
            maskMode = 1;
          else
            throw std::runtime_error("--mask-mode must be causal or none");
        } else
          throw std::runtime_error(
              "unknown, duplicate or incomplete run option");
      }
      setenv("LSE_REQUIRE_DEVICE_KERNELS", "1", 1);
      return run_gpu(benchmark, maskMode);
    }
    std::fprintf(stderr,
                 "Usage: %s --compile-only PATH | --run [--benchmark] "
                 "[--mask-mode causal|none]\n",
                 argv[0]);
    return 2;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
