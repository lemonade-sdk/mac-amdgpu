#include "transport.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2 || std::strcmp(argv[1], "--run")) {
        std::fprintf(stderr, "Usage: %s --run (requires driver build 182 or newer)\n", argv[0]); return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<std::shared_ptr<mac_hsa::Connection>> devices;
    auto check = [](hsa_status_t status, const char *step) {
        if (status == HSA_STATUS_SUCCESS) return true;
        std::fprintf(stderr, "%s: HSA status %#x\n", step, status); return false;
    };
    if (!check(mac_hsa::discover(devices), "discover") || devices.empty()) return 1;
    auto &connection = *devices.front();
    mac_hsa::DeviceSnapshot info;
    if (!check(connection.read(info), "driver version") || info.build < 182) {
        std::fprintf(stderr, "Install build 182 before running this test.\n"); return 1;
    }
    mac_hsa::SharedBuffer shared;
    mac_hsa::DeviceBuffer device;
    bool passed = false;
    constexpr size_t capacity = 128 * 1024, payload = 64003, offset = 3, destination = 65539;
    std::vector<uint8_t> expected(capacity, 0xa9), observed(capacity), gpuExpected(capacity, 0x5c);
    std::puts("Allocating shared memory: automatically initialize the GPU or join its ready session.");
    if (!check(connection.allocateSharedBuffer(capacity, shared), "allocate equal-address host/GPU memory")) return 1;
    std::printf("Shared allocation: CPU=%p GPU=%#llx size=%llu\n", shared.host,
        (unsigned long long)shared.device.address, (unsigned long long)shared.device.size);
    do {
        if (reinterpret_cast<uintptr_t>(shared.host) != shared.device.address) break;
        if (!check(connection.allocateBuffer(capacity, device), "allocate device VRAM")) break;
        if (!check(connection.writeBuffer(device, 0, gpuExpected.data(), capacity), "initialize GPU guard bytes")) break;
        for (size_t i = 0; i < payload; ++i) expected[offset + i] = uint8_t(i * 79 + (i >> 8) + 13);
        std::memcpy(shared.host, expected.data(), capacity);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!check(connection.copyBuffers(shared.device, offset, device, offset, payload), "shared CPU writes -> GPU reads")) break;
        if (!check(connection.readBuffer(device, 0, observed.data(), capacity), "read GPU results")) break;
        std::copy_n(expected.begin() + offset, payload, gpuExpected.begin() + offset);
        if (observed != gpuExpected) { std::fputs("GPU source readback or guards differ\n", stderr); break; }
        for (size_t i = 0; i < payload; ++i) gpuExpected[offset + i] = uint8_t(i * 113 + (i >> 7) + 71);
        if (!check(connection.writeBuffer(device, 0, gpuExpected.data(), capacity), "write independent return pattern")) break;
        if (!check(connection.copyBuffers(device, offset, shared.device, destination, payload), "GPU writes -> direct CPU reads")) break;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::copy_n(gpuExpected.begin() + offset, payload, expected.begin() + destination);
        const auto *bytes = static_cast<volatile uint8_t *>(shared.host);
        for (size_t i = 0; i < capacity; ++i) {
            if (bytes[i] != expected[i]) {
                std::fprintf(stderr, "Shared byte %zu: got %#x expected %#x\n", i, bytes[i], expected[i]);
                goto cleanup;
            }
        }
        passed = true;
    } while (false);
cleanup:
    if (device.handle && !check(connection.freeBuffer(device), "free device VRAM")) passed = false;
    if (!check(connection.freeSharedBuffer(shared), "unmap and free shared memory")) passed = false;
    if (passed) std::puts("PASS: equal CPU/GPU addresses, 64003 unaligned bytes each way, all 131072 shared bytes and VRAM guards verified; mappings released");
    std::puts("This tests shared-memory copies, not concurrent system atomics or hardware AQL queues.");
    std::puts("Closing the test session; the driver resets the GPU if this was its last participating client.");
    return passed ? 0 : 1;
}
