#include "code_object.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

template<class T> static T get(const std::vector<uint8_t> &bytes, size_t offset) {
    assert(offset + sizeof(T) <= bytes.size()); T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
template<class T> static void set(std::vector<uint8_t> &bytes, size_t offset, T value) {
    assert(offset + sizeof(T) <= bytes.size()); std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream stream(argv[1], std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(stream), {}};
    assert(file.size() > 64);
    mac_hsa::CodeObject object;
    assert(mac_hsa::parseCodeObject(file, object));
    assert(object.kernels.size() == 1 && object.virtualBase == 0);
    const auto &kernel = object.kernels.front();
    assert(kernel.name == "vector_add" && kernel.symbol == "vector_add.kd");
    assert(kernel.kernargSize == 12 && kernel.kernargAlignment == 8 && !kernel.groupSize && !kernel.privateSize);
    assert(kernel.entry % 256 == 0 && kernel.descriptor % 64 == 0 && kernel.entry < object.image.size());
    assert(get<uint32_t>(object.image, kernel.entry) == 0xf400a000); // compiler fixture first instruction
    const auto before = object.image;
    assert(mac_hsa::relocateCodeObject(object, 0x8001800000ull) && object.image == before);
    auto descriptorRelocation = object;
    descriptorRelocation.relocations = {{kernel.descriptor + 16, 0, 0, 13, false}};
    assert(!mac_hsa::relocateCodeObject(descriptorRelocation, 0x8001800000ull));
    // Every truncation must be rejected without reading past the supplied span.
    for (size_t size = 0; size < file.size(); ++size) {
        mac_hsa::CodeObject truncated;
        assert(!mac_hsa::parseCodeObject(std::span(file).first(size), truncated));
    }
    for (size_t byte : {size_t(4), size_t(5), size_t(7), size_t(8), size_t(16), size_t(18), size_t(48)}) {
        auto bad = file; bad[byte] = 0xff; mac_hsa::CodeObject invalid;
        assert(!mac_hsa::parseCodeObject(bad, invalid));
    }
    const auto phoff = get<uint64_t>(file, 32);
    const auto phnum = get<uint16_t>(file, 56);
    for (unsigned i = 0; i < phnum; ++i) {
        const auto offset = size_t(phoff + i * 56);
        if (get<uint32_t>(file, offset) != 1) continue;
        auto bad = file;
        set(bad, offset + 16, UINT64_MAX - 4);
        mac_hsa::CodeObject invalid;
        assert(!mac_hsa::parseCodeObject(bad, invalid));
        bad = file; set(bad, offset + 40, uint64_t(1));
        assert(!mac_hsa::parseCodeObject(bad, invalid));
    }
    uint32_t random = 0x1234;
    for (unsigned i = 0; i < 1000; ++i) {
        auto mutation = file;
        random = random * 1664525u + 1013904223u;
        mutation[random % mutation.size()] ^= uint8_t(1u << ((random >> 16) & 7));
        mac_hsa::CodeObject result;
        mac_hsa::parseCodeObject(mutation, result); // valid mutations are allowed
    }
    mac_hsa::CodeObject reloc;
    reloc.image.resize(24);
    reloc.relocations = {{0, 0, 0x100, 13, false}, {8, 0x200, -16, 3, false}, {16, 0x5678, 8, 3, true}};
    assert(mac_hsa::relocateCodeObject(reloc, 0x8000000000));
    assert(get<uint64_t>(reloc.image, 0) == 0x8000000100 && get<uint64_t>(reloc.image, 8) == 0x80000001f0);
    assert(get<uint64_t>(reloc.image, 16) == 0x5680);
    reloc.relocations = {{17, 0, 0, 13, false}};
    assert(!mac_hsa::relocateCodeObject(reloc, 1));
    reloc.relocations = {{0, 0, -2, 13, false}};
    assert(!mac_hsa::relocateCodeObject(reloc, 1));
    puts("HSA: compiler-produced linked ELF metadata, descriptors, load ranges, relocations, truncations and mutation checks pass");
}
