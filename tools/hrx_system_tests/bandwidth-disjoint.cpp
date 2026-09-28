// Reuse the standalone suite's checked allocation/copy/retirement helpers.
#define main original_suite_main
#include "native.cpp"
#undef main
namespace {
constexpr size_t region_count = 8;
constexpr size_t region_bytes = size_t(12) << 20;
constexpr size_t region_stride = region_bytes + 2 * guard;
Buffer allocate_direct(size_t bytes, bool host) {
  Buffer b;
  hrx_buffer_params_t params{};
  params.type = host ? HRX_MEMORY_TYPE_HOST_VISIBLE | HRX_MEMORY_TYPE_HOST_COHERENT |
                           HRX_MEMORY_TYPE_DEVICE_VISIBLE
                     : HRX_MEMORY_TYPE_DEVICE_LOCAL;
  params.access = HRX_MEMORY_ACCESS_ALL;
  params.usage = HRX_BUFFER_USAGE_DEFAULT |
                 (host ? HRX_BUFFER_USAGE_MAPPING_PERSISTENT | HRX_BUFFER_USAGE_MAPPING_SCOPED : 0);
  check(hrx_allocator_allocate_buffer(hrx_device_allocator(device), params, bytes, &b.handle),
        "direct allocate");
  buffers.push_back(b);
  size_t capacity = 0;
  check(hrx_buffer_get_size(b.handle, &capacity), "direct allocation size");
  require(capacity >= bytes, "allocation smaller than request");
  if (host) {
    void* mapping = nullptr;
    check(hrx_buffer_map(b.handle, HRX_MAP_READ | HRX_MAP_WRITE, 0, bytes, &mapping),
          "direct host map");
    require(mapping != nullptr, "null host mapping");
    b.host = static_cast<unsigned char*>(mapping);
    buffers.back().host = b.host;
    hsa_amd_pointer_info_t info{};
    info.size = sizeof(info);
    hcheck(hsa_amd_pointer_info(mapping, &info, nullptr, nullptr, nullptr),
           "host allocation metadata");
    require(info.hostBaseAddress == info.agentBaseAddress && info.hostBaseAddress == mapping,
            "shared CPU/GPU base pointer mismatch");
    require((info.global_flags & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED) != 0,
            "shared host mapping policy mismatch");
  }
  std::printf("ALLOCATED endpoint=%s requested=%zu actual=%zu cpu_gpu_same=%s\n",
              host ? "host" : "VRAM", bytes, capacity, host ? "yes" : "notCPU-mapped");
  return b;
}
void seed_regions(const std::vector<Buffer>& endpoint, unsigned generation, bool payload) {
  for (unsigned region = 0; region < region_count; ++region)
    seed(endpoint[region].host, region_bytes, generation + region * 101, payload);
}
void verify_regions(const std::vector<Buffer>& endpoint, unsigned generation) {
  for (unsigned region = 0; region < region_count; ++region)
    verify(endpoint[region].host, region_bytes, generation + region * 101);
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 3 || std::strcmp(argv[1], "--run")) return 2;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  out = argv[2];
  bandwidth_csv.open(out / "bandwidth-disjoint.csv");
  bandwidth_csv.precision(17);
  bandwidth_csv
      << "direction,region_bytes,regions,payload_working_set_bytes,trial,seconds,GBps,validation\n";
  bool initialized = false;
  try {
    check(hrx_gpu_initialize(0), "HRX init");
    initialized = true;
    check(hrx_gpu_device_get(0, &device), "HRX device");
    check(hrx_stream_create(device, 0, &stream), "HRX stream");
    auto endpoint = [](bool host) {
      std::vector<Buffer> regions;
      for (unsigned i = 0; i < region_count; ++i) {
        std::printf("ALLOC endpoint=%s region=%u bytes=%zu\n", host ? "host" : "VRAM", i,
                    region_stride);
        regions.push_back(allocate_direct(region_stride, host));
      }
      return regions;
    };
    // The runtime caps a single allocation below512 MiB. Eight independent
    // 12 MiB payload buffers retain a96 MiB working set per endpoint. One host
    // endpoint owns input or output in turn and is reused for checked readback.
    const auto hs = endpoint(true);
    const auto ds = endpoint(false), dd = endpoint(false);
    retire();
    for (unsigned direction = 0; direction < 2; ++direction) {
      const char* label = direction ? "D2H" : "H2D";
      for (unsigned trial = 0; trial <= 3; ++trial) {
        const unsigned generation = 991 + direction * 1009 + trial * 13;
        seed_regions(hs, generation, true);
        for (unsigned region = 0; region < region_count; ++region) {
          copy(hs[region], 0, ds[region], 0, region_stride);
        }
        retire();
        seed_regions(hs, generation, false);
        for (unsigned region = 0; region < region_count; ++region)
          copy(hs[region], 0, dd[region], 0, region_stride);
        retire();
        if (!direction) seed_regions(hs, generation, true);
        const auto& src = direction ? ds : hs;
        const auto& dst = direction ? hs : dd;
        const auto start = Clock::now();
        for (unsigned region = 0; region < region_count; ++region) {
          copy(src[region], guard, dst[region], guard, region_bytes);
        }
        retire();
        const auto stop = Clock::now();
        // H2D: check host source before reusing it for full readback. D2H:
        // This first check verifies the host destination and all region guards.
        verify_regions(hs, generation);
        if (!direction) {
          for (unsigned region = 0; region < region_count; ++region)
            copy(dd[region], 0, hs[region], 0, region_stride);
          retire();
          verify_regions(hs, generation);
        }
        for (unsigned region = 0; region < region_count; ++region)
          copy(ds[region], 0, hs[region], 0, region_stride);
        retire();
        verify_regions(hs, generation);
        if (trial) {
          const double seconds = ns(start, stop) / 1e9;
          const double gbps = region_bytes * region_count / seconds / 1e9;
          bandwidth_csv << label << ',' << region_bytes << ',' << region_count << ','
                        << region_bytes * region_count << ',' << trial << ',' << seconds << ','
                        << gbps << ",verified\n";
          bandwidth_csv.flush();
          std::printf(
              "PASS %s 8distinct12MiB regions trial=%u %.6fGB/s fullsource/destination/guards\n",
              label, trial, gbps);
        }
      }
    }
    free_buffers();
    hrx_stream_release(stream);
    stream = nullptr;
    check(hrx_gpu_shutdown(), "shutdown");
    initialized = false;
    std::puts("PASS disjoint-region transfer check; buffers/queue retired and released");
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
        if (initialized) check(hrx_gpu_shutdown(), "failure shutdown");
      } catch (...) {
      }
    }
    return 1;
  }
}
