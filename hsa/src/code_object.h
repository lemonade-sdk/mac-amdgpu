#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mac_hsa {
struct KernelMetadata {
    std::string name, symbol;
    uint64_t descriptor = 0, entry = 0;
    uint32_t kernargSize = 0, kernargAlignment = 0, groupSize = 0, privateSize = 0;
    uint32_t rsrc1 = 0, rsrc2 = 0, rsrc3 = 0;
    uint16_t properties = 0, preload = 0;
    bool dynamicStack = false;
};
struct Relocation {
    uint64_t offset, symbol;
    int64_t addend;
    uint32_t type;
    bool absolute;
};
struct CodeObject {
    uint64_t virtualBase = 0;
    std::vector<uint8_t> image;
    std::vector<KernelMetadata> kernels;
    std::vector<Relocation> relocations;
};
// Bounded ELF64 AMDHSA gfx1201 loader. Linked code objects only; unsupported
// targets, imports, TLS and relocation forms are rejected before GPU upload.
bool parseCodeObject(std::span<const uint8_t> file, CodeObject &output);
bool relocateCodeObject(CodeObject &object, uint64_t gpuAddress);
}
