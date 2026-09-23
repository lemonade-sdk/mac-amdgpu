#include "transport.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

int main(int argc, char **argv) {
    if (argc != 2 || std::strcmp(argv[1], "--run")) {
        std::fprintf(stderr, "Usage: %s --run (GFX12.0.1, build 182+, exclusive diagnostic session)\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto check = [](hsa_status_t status, const char *step) {
        if (status == HSA_STATUS_SUCCESS) return true;
        std::fprintf(stderr, "%s: HSA status %#x\n", step, status); return false;
    };
    std::vector<std::shared_ptr<mac_hsa::Connection>> devices;
    if (!check(mac_hsa::discover(devices), "discover") || devices.empty()) return 1;
    auto &connection = *devices.front();
    mac_hsa::SharedBuffer shared;
    std::puts("Allocating signal backing: automatically initialize the GPU or join its ready session.");
    if (!check(connection.allocateSharedBuffer(16384, shared), "allocate shared signal backing")) return 1;
    constexpr uint64_t offset = 64;
    std::memset(shared.host, 0xa7, shared.device.size);
    auto &word = *reinterpret_cast<int64_t *>(static_cast<uint8_t *>(shared.host) + offset);
    std::atomic_ref<int64_t> value(word);
    bool passed = false;
    do {
        value.store(1, std::memory_order_release);
        std::puts("Testing GPU signal decrement in shared host memory...");
        if (!check(connection.testSharedAtomicAdd(shared, offset, -1, 1), "SDMA signal decrement")) break;
        auto observed = value.load(std::memory_order_acquire);
        if (observed != 0) { std::fprintf(stderr, "Decrement: expected 0, got %lld\n", (long long)observed); break; }
        value.store(0xffffffffll, std::memory_order_release);
        if (!check(connection.testSharedAtomicAdd(shared, offset, 1, 1), "64-bit carry")) break;
        observed = value.load(std::memory_order_acquire);
        if (observed != 0x100000000ll) { std::fprintf(stderr, "Carry: got %#llx\n", (unsigned long long)observed); break; }
        std::puts("Testing concurrent ARM64 CPU and SDMA atomic additions...");
        value.store(0, std::memory_order_release);
        std::atomic<bool> started{false}, stop{false};
        uint64_t cpuAdds = 0;
        std::thread cpu([&] {
            value.fetch_add(1, std::memory_order_acq_rel); ++cpuAdds;
            started.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire)) {
                value.fetch_add(1, std::memory_order_acq_rel); ++cpuAdds;
            }
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        uint64_t gpuAdds = 0;
        for (unsigned batch = 0; batch < 8; ++batch) {
            if (!check(connection.testSharedAtomicAdd(shared, offset, 1, 64), "concurrent SDMA additions")) break;
            gpuAdds += 64;
        }
        stop.store(true, std::memory_order_release); cpu.join();
        observed = value.load(std::memory_order_acquire);
        std::printf("CPU additions=%llu GPU additions=%llu observed=%lld\n",
            (unsigned long long)cpuAdds, (unsigned long long)gpuAdds, (long long)observed);
        if (gpuAdds != 512 || uint64_t(observed) != cpuAdds + gpuAdds) break;
        const auto *bytes = static_cast<const uint8_t *>(shared.host);
        for (size_t i = 0; i < shared.device.size; ++i) {
            if (i >= offset && i < offset + 8) continue;
            if (bytes[i] != 0xa7) { std::fprintf(stderr, "Guard changed at %zu\n", i); goto cleanup; }
        }
        passed = true;
    } while (false);
cleanup:
    if (!check(connection.freeSharedBuffer(shared), "release shared signal backing")) passed = false;
    if (passed) std::puts("PASS: GPU decrement, 64-bit carry, concurrent CPU/GPU additions and guards verified.");
    std::puts("This diagnostic does not establish shader atomics, payload publication ordering or hardware AQL queues.");
    std::puts("Closing the test session; the driver resets the GPU if this was its last participating client.");
    return passed ? 0 : 1;
}
