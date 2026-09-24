// Explicit GPU qualification for typed metadata extents and paged KV ownership.
#include "lse/place/devices.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
#include "lse/kv/block.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void check(const lse::Status& status, const char* why) {
  if (!status.ok()) throw std::runtime_error(std::string(why) + ": " + status.to_string());
}
struct Input { lse::graph::Array array; std::vector<std::byte> original; };
}
int main(int argc, char** argv) {
  if (argc != 2 || std::strcmp(argv[1], "--run")) {
    std::fprintf(stderr, "Usage: %s --run (actual LSE/Loom/HRX paged KV and attention)\n", argv[0]);
    return 2;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  setenv("LSE_REQUIRE_DEVICE_KERNELS", "1", 1);
  try {
    using namespace lse; using namespace lse::graph;
    check(place::open_default_devices("hrx:0"), "open GPU");
    auto* devices = place::default_devices();
    require(devices && devices->size() == 1, "one GPU required");
    auto& backend = devices->device(devices->primary());
    require(backend.name() == "hrx" && backend.device_info().arch == "gfx1201", "gfx1201 HRX required");
    auto* scheduler = default_scheduler(); require(scheduler, "scheduler required");
    scheduler->set_dialect(Dialect::kLoom);
    auto upload = [&](const std::vector<float>& values, Shape shape) {
      Input input; input.original.assign(values.size() * 4 + 256, std::byte{0xa5});
      std::memcpy(input.original.data(), values.data(), values.size() * 4);
      auto allocation = backend.allocate(input.original.size(), backend::MemoryClass::kDevice);
      check(allocation.status(), "allocate guarded input"); auto buffer = allocation.release();
      check(backend.copy_h2d(input.original.data(), buffer, input.original.size(), 0), "upload input");
      input.array = Array::from_buffer(buffer, shape, DType::kF32); return input;
    };
    auto gpuTrace = [&] {
      const auto trace = scheduler->last_trace();
      require(trace.device_groups && trace.kernels_launched && !trace.host_groups && !trace.host_fallbacks,
              "GPU-only execution required");
    };
    constexpr int batch = 2, heads = 2, kvHeads = 1, seq = 2, dim = 8, blockSize = 4, blocks = 4;
    const std::vector<float> table{2, 0, 3, 1};
    auto address = [&](int row, int position, int channel) {
      return (int(table[row * 2 + position / blockSize]) * blockSize + position % blockSize) * dim + channel;
    };
    // Same tensor shapes and compiled kernels; only dispatch metadata changes.
    for (int liveRows : {0, 1, 2, 1}) {
      std::vector<float> metadata(kv::step_meta_elems(batch), 0);
      metadata[0] = liveRows == 2 ? 6 : liveRows == 1 ? 3 : 0;
      metadata[1] = liveRows == 2 ? 8 : liveRows == 1 ? 5 : 0;
      metadata[2] = float(liveRows);
      metadata[3] = 3; metadata[4] = liveRows >= 1 ? 5 : 0;
      metadata[5] = 6; metadata[6] = liveRows >= 2 ? 8 : 0;
      std::vector<float> keys(blocks * kvHeads * blockSize * dim), values(keys.size());
      std::vector<float> newKeys(batch * kvHeads * seq * dim), newValues(newKeys.size());
      std::vector<float> query(batch * heads * seq * dim);
      for (size_t i = 0; i < keys.size(); ++i) {
        keys[i] = float(int(i % 13) - 6) / 32;
        values[i] = float(int(i % 17) - 8) / 8;
      }
      for (size_t i = 0; i < newKeys.size(); ++i) {
        newKeys[i] = float(int(i % 7) - 3) / 16;
        newValues[i] = float(int(i % 11) + 1) / 4;
      }
      for (size_t i = 0; i < query.size(); ++i) query[i] = float(int(i % 9) - 4) / 8;
      auto expectedKeys = keys, expectedValues = values;
      for (int row = 0; row < liveRows; ++row) for (int t = 0; t < seq; ++t) for (int d = 0; d < dim; ++d) {
        const int dst = address(row, int(metadata[3 + 2 * row]) + t, d);
        expectedKeys[dst] = newKeys[(row * seq + t) * dim + d];
        expectedValues[dst] = newValues[(row * seq + t) * dim + d];
      }
      auto ki = upload(keys, Shape{blocks, kvHeads, blockSize, dim});
      auto vi = upload(values, Shape{blocks, kvHeads, blockSize, dim});
      auto ksi = upload(newKeys, Shape{batch, kvHeads, seq, dim});
      auto vsi = upload(newValues, Shape{batch, kvHeads, seq, dim});
      auto qi = upload(query, Shape{batch, heads, seq, dim});
      auto mi = upload(metadata, Shape{kv::step_meta_elems(batch)});
      auto ti = upload(table, Shape{batch, 2});
      auto writtenKeys = kv_page_write(ki.array, ksi.array, mi.array, ti.array, blockSize);
      auto writtenValues = kv_page_write(vi.array, vsi.array, mi.array, ti.array, blockSize);
      std::vector<float> actualKeys(keys.size()), actualValues(values.size());
      check(writtenKeys.to_host(actualKeys.data(), actualKeys.size() * 4), "write keys"); gpuTrace();
      check(writtenValues.to_host(actualValues.data(), actualValues.size() * 4), "write values"); gpuTrace();
      require(actualKeys == expectedKeys && actualValues == expectedValues, "paged write or untouched slots differ");
      constexpr float scale = 0.25f;
      auto output = sdpa_paged(qi.array, writtenKeys, writtenValues, scale, MaskKind::kCausal, 0,
                               mi.array, ti.array, blockSize);
      std::vector<float> actual(query.size()), expected(query.size(), 0);
      for (int row = 0; row < liveRows; ++row) for (int head = 0; head < heads; ++head) for (int t = 0; t < seq; ++t) {
        const int last = int(metadata[3 + 2 * row]) + t;
        std::vector<double> scores(last + 1); double largest = -1e100;
        for (int pos = 0; pos <= last; ++pos) {
          double score = 0;
          for (int d = 0; d < dim; ++d)
            score += double(query[((row * heads + head) * seq + t) * dim + d]) * expectedKeys[address(row, pos, d)];
          scores[pos] = score * scale; largest = std::max(largest, scores[pos]);
        }
        double denom = 0; for (auto& score : scores) { score = std::exp(score - largest); denom += score; }
        for (int d = 0; d < dim; ++d) {
          double sum = 0; for (int pos = 0; pos <= last; ++pos) sum += scores[pos] * expectedValues[address(row, pos, d)];
          expected[((row * heads + head) * seq + t) * dim + d] = float(sum / denom);
        }
      }
      check(output.to_host(actual.data(), actual.size() * 4), "paged attention"); gpuTrace();
      float maxError = 0;
      for (size_t i = 0; i < actual.size(); ++i) {
        const float error = std::abs(actual[i] - expected[i]); maxError = std::max(maxError, error);
        if (!std::isfinite(actual[i]) || error > 2e-5f || (i >= size_t(liveRows * heads * seq * dim) && actual[i] != 0)) {
          std::fprintf(stderr, "paged mismatch rows%d index%zu expected%g actual%g\n", liveRows, i, expected[i], actual[i]);
          throw std::runtime_error("paged attention mismatch");
        }
      }
      for (auto* input : {&ki, &vi, &ksi, &vsi, &qi, &mi, &ti}) {
        std::vector<std::byte> observed(input->original.size());
        check(backend.copy_d2h(input->array.node()->buffer, observed.data(), observed.size(), 0), "verify input guards");
        auto want = input->original;
        if (input == &ki) std::memcpy(want.data(), expectedKeys.data(), expectedKeys.size() * 4);
        if (input == &vi) std::memcpy(want.data(), expectedValues.data(), expectedValues.size() * 4);
        require(observed == want, "input payload or guard mismatch");
      }
      check(backend.synchronize(), "retire paged workload");
      std::printf("PASS paged rows%d/2: 2 exact cache images, %zu attention outputs max_error=%g; seven guards; GPU-only\n",
                  liveRows, actual.size(), maxError);
    }
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
