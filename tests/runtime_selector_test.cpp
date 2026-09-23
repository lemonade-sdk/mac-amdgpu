#include <cassert>
#include <cstdint>
#include <cstdio>
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnUnsupported = 2;
constexpr uint64_t kMacAMDGPUMethodRuntimeBuild = 43;
#define MACAMDGPU_BUILD_VERSION 159
struct Arguments {
    uint32_t scalarInputCount = 0;
    uint64_t *scalarOutput = nullptr;
    uint32_t scalarOutputCount = 0;
};
static int query(uint64_t selector, Arguments *arguments) {
    // No PCI object or hardware helpers exist in this harness: the response
    // must depend only on the binary's build and the RPC argument shape.
    switch (selector) {
#include "runtime_selector_under_test.inc"
    default: return kIOReturnUnsupported;
    }
}
int main() {
    uint64_t output[4] = {};
    Arguments a{0, output, 4};
    assert(query(43, &a) == 0);
    assert(a.scalarOutputCount == 3);
    assert(output[0] == 0x414D444750554142ull && output[1] == 1 && output[2] == 159);
    a.scalarInputCount = 1;
    assert(query(43, &a) == kIOReturnBadArgument);
    a.scalarInputCount = 0; a.scalarOutputCount = 2;
    assert(query(43, &a) == kIOReturnBadArgument);
    a.scalarOutputCount = 3; a.scalarOutput = nullptr;
    assert(query(43, &a) == kIOReturnBadArgument);
    puts("Runtime selector: responding build, protocol identity, exact result count and argument validation pass");
}
