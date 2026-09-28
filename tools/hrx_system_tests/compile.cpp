#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "lse/backends/hrx/arch_database.hpp"
#include "lse/backends/hrx/loomc/loom_emitter.hpp"
#include "lse/backends/hrx/loomc/loomc_compiler.hpp"
#include "lse/graph/graph.hpp"
#include "lse/graph/ops.hpp"
using namespace lse;
using namespace lse::graph;
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::filesystem::path out = argv[1];
  auto leaf = [](Shape shape, DType dtype) {
    auto n = std::make_shared<Node>();
    n->shape = shape;
    n->dtype = dtype;
    n->materialized = true;
    return Array(n);
  };
  backend::DeviceInfo device;
  backend::AmdDeviceInfo amd;
  device.arch = "gfx1201";
  backend::apply_arch_defaults(device, amd);
  device.extension_id = backend::AmdDeviceInfo::kExtensionId;
  device.extension = &amd;
  std::ofstream csv(out / "compile.csv");
  csv << "case,trial,kind,source_lines,source_bytes,code_bytes,compile_ms,grid_x,wg_x\n";
  // Fresh compiler per case: trial0 includes lazy compiler initialization.
  // Every repetition calls compile(source), without the JIT disk-cache layer.
  for (const int m : {1, 512}) {
    auto x = leaf({1, m, 5120}, DType::kF32);
    auto w = leaf({17408, 640}, DType::kU32);
    auto sc = leaf({17408, 80}, DType::kBF16);
    auto bi = leaf({17408, 80}, DType::kBF16);
    auto y = quant_linear(x, w, sc, bi, 4, 64);
    const NodePtr roots[] = {y.node()};
    const auto groups = Partitioner::partition(roots);
    if (groups.size() != 1) return 3;
    backend::LoomEmitter emitter;
    auto emitted = emitter.emit(groups.front(), device);
    if (!emitted.ok()) {
      std::fprintf(stderr, "%s\n", emitted.status().message().c_str());
      return 4;
    }
    const std::string name = "q4_m" + std::to_string(m);
    std::ofstream(out / (name + ".loom")) << emitted->source;
    const size_t lines =
        static_cast<size_t>(std::count(emitted->source.begin(), emitted->source.end(), '\n')) +
        (!emitted->source.empty() && emitted->source.back() != '\n');
    backend::LoomcCompiler compiler;
    for (unsigned trial = 0; trial <= 7; ++trial) {
      const auto start = std::chrono::steady_clock::now();
      auto code = compiler.compile(emitted->source, "gfx1201");
      const auto stop = std::chrono::steady_clock::now();
      if (!code.ok()) {
        std::fprintf(stderr, "%s\n", code.status().message().c_str());
        return 5;
      }
      const double ms = std::chrono::duration<double, std::milli>(stop - start).count();
      if (trial == 0) {
        std::ofstream file(out / (name + ".hsaco"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(code->code.data()),
                   static_cast<std::streamsize>(code->code.size()));
      }
      csv << name << ',' << trial << ',' << (trial ? "warm_uncached" : "first_call") << ',' << lines
          << ',' << emitted->source.size() << ',' << code->code.size() << ',' << ms << ','
          << emitted->dims.workgroup_count[0] << ',' << emitted->dims.workgroup_size[0] << '\n';
      std::printf("COMPILE %s trial=%u lines=%zu source_bytes=%zu code_bytes=%zu %.6fms\n",
                  name.c_str(), trial, lines, emitted->source.size(), code->code.size(), ms);
    }
  }
  return 0;
}
