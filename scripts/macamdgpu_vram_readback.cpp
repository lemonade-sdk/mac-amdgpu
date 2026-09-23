// Build: xcrun clang++ -std=c++20 -Wall -Wextra -Werror -framework IOKit
//        -framework CoreFoundation scripts/macamdgpu_vram_readback.cpp
//        -o build/macamdgpu_vram_readback
// Usage: macamdgpu_vram_readback SRC_VRAM_OFFSET DST_VRAM_OFFSET BYTES
// Read retained SDMA smoke-test allocations while the initialized host remains
// connected. Offsets are GPU addresses minus vram_start, not GPU addresses.
// Does not reset the device, submit commands, or write the mapped framebuffer.
#include <IOKit/IOKitLib.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

static bool parse(const char *text, uint64_t &value)
{
    if (!text || !*text || *text == '-') return false;
    errno = 0;
    char *end = nullptr;
    value = strtoull(text, &end, 0);
    return errno == 0 && end != text && *end == '\0';
}

int main(int argc, char **argv)
{
    uint64_t src = 0, dst = 0, bytes = 0;
    if (argc != 4 || !parse(argv[1], src) || !parse(argv[2], dst) ||
        !parse(argv[3], bytes) || (src | dst | bytes) & 3 ||
        bytes == 0 || bytes > 16384) {
        std::fprintf(stderr, "Usage: %s SRC_VRAM_OFFSET DST_VRAM_OFFSET BYTES\n"
                             "Offsets/count must be dword aligned; 4..16384 bytes.\n",
                     argv[0]);
        return 2;
    }
    const io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceNameMatching("MacAMDGPU"));
    if (!service) {
        std::fprintf(stderr, "No attached MacAMDGPU service\n");
        return 1;
    }
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr, "IOServiceOpen: %#x\n", kr);
        return 1;
    }
    uint64_t query = 4, stage = 0;
    uint32_t count = 1;
    kr = IOConnectCallScalarMethod(connection, 21, &query, 1, &stage, &count);
    if (kr != KERN_SUCCESS || count != 1 || stage != 15) {
        std::fprintf(stderr, "Initialize GPU and run SDMA Copy first (kr=%#x stage=%" PRIu64 ")\n",
                     kr, stage);
        IOServiceClose(connection);
        return 1;
    }
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    kr = IOConnectMapMemory64(connection, 0, mach_task_self(), &address, &size,
                              kIOMapAnywhere | kIOMapReadOnly);
    if (kr != KERN_SUCCESS) {
        std::fprintf(stderr, "Read-only BAR0 mapping: %#x\n", kr);
        IOServiceClose(connection);
        return 1;
    }
    int result = 1;
    if (src > size || dst > size || bytes > size - src || bytes > size - dst) {
        std::fprintf(stderr, "Range exceeds BAR0 aperture (%" PRIu64 " bytes)\n", size);
    } else {
        auto *source = reinterpret_cast<volatile const uint32_t *>(address + src);
        auto *destination = reinterpret_cast<volatile const uint32_t *>(address + dst);
        uint32_t sourceBad = 0, destinationBad = 0, poison = 0;
        for (uint64_t i = 0; i < bytes / 4; ++i) {
            const uint32_t expected = 0xcafe0000u + static_cast<uint32_t>(i);
            const uint32_t s = source[i], d = destination[i];
            sourceBad += s != expected;
            if (d != expected) {
                if (destinationBad < 12)
                    std::printf("offset=%#" PRIx64 " expected=%#010x source=%#010x destination=%#010x\n",
                                i * 4, expected, s, d);
                ++destinationBad;
            }
            poison += d == 0xdeadbeef;
        }
        std::printf("BAR0 readback: source_mismatches=%u destination_mismatches=%u"
                    " destination_poison=%u dwords=%" PRIu64 "\n",
                    sourceBad, destinationBad, poison, bytes / 4);
        result = (sourceBad || destinationBad) ? 1 : 0;
    }
    IOConnectUnmapMemory64(connection, 0, mach_task_self(), address);
    IOServiceClose(connection);
    return result;
}
