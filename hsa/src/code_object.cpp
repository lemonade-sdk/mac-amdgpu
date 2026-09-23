#include "code_object.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

namespace mac_hsa {
namespace {
bool range(uint64_t offset, uint64_t size, uint64_t capacity) {
    return offset <= capacity && size <= capacity - offset;
}
template<class T> bool read(std::span<const uint8_t> bytes, uint64_t offset, T &value) {
    if (!range(offset, sizeof(T), bytes.size())) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T)); return true;
}
struct ELFHeader {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct ProgramHeader { uint32_t type, flags; uint64_t offset, address, physical, fileSize, memorySize, alignment; };
struct SectionHeader { uint32_t name, type; uint64_t flags, address, offset, size; uint32_t link, info; uint64_t alignment, entrySize; };
struct ELFSymbol { uint32_t name; uint8_t info, other; uint16_t section; uint64_t value, size; };
struct ELFRelocation { uint64_t offset, info; int64_t addend; };
static_assert(sizeof(ELFHeader) == 64 && sizeof(ProgramHeader) == 56 && sizeof(SectionHeader) == 64);
static_assert(sizeof(ELFSymbol) == 24 && sizeof(ELFRelocation) == 24);

struct Value {
    enum Kind { None, Integer, String, Array, Map, Boolean } kind = None;
    uint64_t integer = 0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> map;
    const Value *get(const char *key) const {
        if (kind != Map) return nullptr;
        const auto found = map.find(key); return found == map.end() ? nullptr : &found->second;
    }
};
class MessagePack {
    std::span<const uint8_t> bytes;
    size_t position = 0, nodes = 0;
    bool number(unsigned length, uint64_t &value) {
        if (!range(position, length, bytes.size())) return false;
        value = 0;
        while (length--) value = (value << 8) | bytes[position++];
        return true;
    }
    bool parse(Value &value, unsigned depth) {
        if (depth > 32 || ++nodes > 100000 || position == bytes.size()) return false;
        const auto tag = bytes[position++];
        uint64_t length = 0;
        if (tag <= 0x7f) { value.kind = Value::Integer; value.integer = tag; return true; }
        if (tag == 0xc0) return true;
        if (tag == 0xc2 || tag == 0xc3) { value.kind = Value::Boolean; value.integer = tag == 0xc3; return true; }
        if (tag >= 0xcc && tag <= 0xcf) {
            value.kind = Value::Integer; return number(1u << (tag - 0xcc), value.integer);
        }
        if (tag >= 0xd0 && tag <= 0xd3) {
            const unsigned width = 1u << (tag - 0xd0);
            value.kind = Value::Integer;
            return number(width, value.integer) && !(value.integer & (uint64_t(1) << (width * 8 - 1)));
        }
        if ((tag & 0xe0) == 0xa0) { value.kind = Value::String; length = tag & 31; }
        else if (tag >= 0xd9 && tag <= 0xdb) {
            value.kind = Value::String; if (!number(1u << (tag - 0xd9), length)) return false;
        } else if ((tag & 0xf0) == 0x90) { value.kind = Value::Array; length = tag & 15; }
        else if (tag == 0xdc || tag == 0xdd) {
            value.kind = Value::Array; if (!number(tag == 0xdc ? 2 : 4, length)) return false;
        } else if ((tag & 0xf0) == 0x80) { value.kind = Value::Map; length = tag & 15; }
        else if (tag == 0xde || tag == 0xdf) {
            value.kind = Value::Map; if (!number(tag == 0xde ? 2 : 4, length)) return false;
        } else return false;
        if (length > bytes.size() - position) return false;
        if (value.kind == Value::String) {
            value.string.assign(reinterpret_cast<const char *>(bytes.data() + position), size_t(length));
            position += length; return true;
        }
        if (length > 100000 - nodes) return false;
        for (uint64_t i = 0; i < length; ++i) {
            Value child;
            if (value.kind == Value::Array) {
                if (!parse(child, depth + 1)) return false;
                value.array.push_back(std::move(child));
            } else {
                Value key;
                if (!parse(key, depth + 1) || key.kind != Value::String || !parse(child, depth + 1)) return false;
                if (!value.map.emplace(std::move(key.string), std::move(child)).second) return false;
            }
        }
        return true;
    }
public:
    explicit MessagePack(std::span<const uint8_t> data) : bytes(data) {}
    bool decode(Value &value) { return parse(value, 0) && position == bytes.size(); }
};
bool integer(const Value &map, const char *key, uint32_t &out) {
    const auto value = map.get(key);
    if (!value || value->kind != Value::Integer || value->integer > UINT32_MAX) return false;
    out = uint32_t(value->integer); return true;
}
bool string(const Value &map, const char *key, std::string &out) {
    const auto value = map.get(key);
    if (!value || value->kind != Value::String || value->string.empty() ||
        value->string.find('\0') != std::string::npos) return false;
    out = value->string; return true;
}
bool metadata(std::span<const uint8_t> data, std::vector<KernelMetadata> &kernels, const char *expectedTarget) {
    Value root;
    if (!MessagePack(data).decode(root)) return false;
    const auto version = root.get("amdhsa.version"), list = root.get("amdhsa.kernels");
    if (!version || version->kind != Value::Array || version->array.size() != 2 ||
        version->array[0].kind != Value::Integer || version->array[0].integer != 1 ||
        version->array[1].kind != Value::Integer || version->array[1].integer > 2 ||
        !list || list->kind != Value::Array || list->array.empty()) return false;
    const auto target = root.get("amdhsa.target");
    if (target && (target->kind != Value::String || target->string != expectedTarget)) return false;
    for (const auto &item : list->array) {
        KernelMetadata kernel; uint32_t wave = 0;
        if (!string(item, ".name", kernel.name) || !string(item, ".symbol", kernel.symbol) ||
            !integer(item, ".kernarg_segment_size", kernel.kernargSize) ||
            !integer(item, ".kernarg_segment_align", kernel.kernargAlignment) ||
            !integer(item, ".group_segment_fixed_size", kernel.groupSize) ||
            !integer(item, ".private_segment_fixed_size", kernel.privateSize) ||
            !integer(item, ".wavefront_size", wave) || (wave != 32 && wave != 64) ||
            !kernel.kernargAlignment || (kernel.kernargAlignment & (kernel.kernargAlignment - 1))) return false;
        if (const auto dynamic = item.get(".uses_dynamic_stack")) {
            if (dynamic->kind != Value::Boolean) return false;
            kernel.dynamicStack = dynamic->integer;
        }
        for (const auto &previous : kernels)
            if (previous.name == kernel.name || previous.symbol == kernel.symbol) return false;
        kernels.push_back(std::move(kernel));
    }
    return true;
}
unsigned relocationWidth(uint32_t type) {
    // AMDGPU dynamic relocations supported by ROCr ApplyDynamicRelocation.
    switch (type) {
    case 1: case 2: case 6: return 4; // ABS32_LO, ABS32_HI, ABS32
    case 3: case 13: return 8; // ABS64, RELATIVE64
    default: return 0;
    }
}
bool symbolName(std::span<const uint8_t> file, const SectionHeader &strings, uint32_t offset, std::string &out) {
    if (offset >= strings.size) return false;
    const auto start = reinterpret_cast<const char *>(file.data() + strings.offset + offset);
    const auto end = static_cast<const char *>(std::memchr(start, 0, strings.size - offset));
    if (!end) return false;
    out.assign(start, end); return true;
}
}

bool parseCodeObject(std::span<const uint8_t> file, CodeObject &output) {
    CodeObject object;
    ELFHeader header{};
    if (file.size() > (256ull << 20) || !read(file, 0, header) ||
        std::memcmp(header.ident, "\177ELF\2\1\1\100", 8) ||
        header.ident[8] < 1 || header.ident[8] > 4 || header.type != 3 || header.machine != 224 ||
        header.version != 1 || header.ehsize != 64 || header.phentsize != 56 || header.shentsize != 64 ||
        !header.phnum || !header.shnum ||
        !range(header.phoff, uint64_t(header.phnum) * 56, file.size()) ||
        !range(header.shoff, uint64_t(header.shnum) * 64, file.size())) return false;
    // ROCr's IsaRegistry associates gfx1201 with gfx12-generic version 1.
    // HRX's built-in helper kernels use that generic target, not gfx1201.
    const auto machine=header.flags&0xff;
    const auto genericVersion=header.flags>>24;
    const bool generic=machine==0x59 && header.ident[8]>=4 && genericVersion==1;
    if (!generic && (machine!=0x4e || genericVersion)) return false;
    const char *target=generic ? "amdgcn-amd-amdhsa--gfx12-generic" : "amdgcn-amd-amdhsa--gfx1201";
    std::vector<ProgramHeader> segments;
    uint64_t begin = UINT64_MAX, end = 0;
    bool haveMetadata = false;
    for (unsigned i = 0; i < header.phnum; ++i) {
        ProgramHeader ph{};
        if (!read(file, header.phoff + i * 56, ph)) return false;
        if (ph.type == 7) return false; // TLS unsupported
        if (!range(ph.offset, ph.fileSize, file.size())) return false;
        if (ph.type == 1 && ph.memorySize) {
            if (ph.fileSize > ph.memorySize || ph.address > UINT64_MAX - ph.memorySize ||
                (ph.alignment > 1 && ((ph.alignment & (ph.alignment - 1)) ||
                 ph.address % ph.alignment != ph.offset % ph.alignment))) return false;
            for (const auto &prior : segments)
                if (ph.address < prior.address + prior.memorySize && prior.address < ph.address + ph.memorySize) return false;
            begin = std::min(begin, ph.address); end = std::max(end, ph.address + ph.memorySize);
            segments.push_back(ph);
        }
        if (ph.type == 4) {
            uint64_t cursor = ph.offset;
            while (cursor < ph.offset + ph.fileSize) {
                uint32_t nameSize, dataSize, type;
                if (!range(cursor - ph.offset, 12, ph.fileSize) || !read(file, cursor, nameSize) ||
                    !read(file, cursor + 4, dataSize) || !read(file, cursor + 8, type)) return false;
                cursor += 12;
                const uint64_t paddedName = (uint64_t(nameSize) + 3) & ~uint64_t(3);
                const uint64_t paddedData = (uint64_t(dataSize) + 3) & ~uint64_t(3);
                if (!range(cursor - ph.offset, paddedName + paddedData, ph.fileSize)) return false;
                if (type == 32 && nameSize == 7 && !std::memcmp(file.data() + cursor, "AMDGPU", 7)) {
                    if (haveMetadata || !metadata(file.subspan(cursor + paddedName, dataSize), object.kernels,target)) return false;
                    haveMetadata = true;
                }
                cursor += paddedName + paddedData;
            }
        }
    }
    if (!haveMetadata || begin == UINT64_MAX || end - begin > (256ull << 20) || begin % 16384) return false;
    object.virtualBase = begin;
    object.image.resize(end - begin, 0);
    for (const auto &segment : segments) {
        std::memcpy(object.image.data() + segment.address - begin, file.data() + segment.offset, segment.fileSize);
        object.segments.push_back({segment.address - begin, segment.memorySize,
                                   segment.offset, segment.fileSize, segment.flags});
    }
    const auto loadedRange = [&](uint64_t address, uint64_t size, bool executable = false) {
        for (const auto &segment : segments)
            if ((!executable || (segment.flags & 1)) && address >= segment.address &&
                range(address - segment.address, size, segment.memorySize)) return true;
        return false;
    };
    std::vector<SectionHeader> sections(header.shnum);
    for (unsigned i = 0; i < header.shnum; ++i) {
        if (!read(file, header.shoff + i * 64, sections[i])) return false;
        if (sections[i].type != 8 && !range(sections[i].offset, sections[i].size, file.size())) return false;
    }
    std::map<std::string, ELFSymbol> symbols;
    for (const auto &section : sections) {
        if (section.type == 9 && section.size) return false; // REL has no explicit addend
        if (section.type != 11) continue; // exported dynamic symbols
        if (section.entrySize != 24 || section.size % 24 || section.link >= sections.size() || sections[section.link].type != 3) return false;
        for (uint64_t offset = 0; offset < section.size; offset += 24) {
            ELFSymbol symbol{}; std::string name;
            if (!read(file, section.offset + offset, symbol) || !symbolName(file, sections[section.link], symbol.name, name)) return false;
            if (!name.empty() && !symbols.emplace(std::move(name), symbol).second) return false;
        }
    }
    for (auto &kernel : object.kernels) {
        const auto found = symbols.find(kernel.symbol);
        if (found == symbols.end()) return false;
        const auto &symbol = found->second;
        if (!symbol.section || symbol.section >= sections.size() || symbol.size != 64 || (symbol.info & 15) != 1 ||
            symbol.value % 64 || !loadedRange(symbol.value, 64)) return false;
        kernel.descriptor = symbol.value - begin;
        int64_t displacement;
        uint32_t group, priv, args;
        auto image = std::span<const uint8_t>(object.image);
        if (!read(image, kernel.descriptor, group) || !read(image, kernel.descriptor + 4, priv) ||
            !read(image, kernel.descriptor + 8, args) || !read(image, kernel.descriptor + 16, displacement) ||
            !read(image, kernel.descriptor + 44, kernel.rsrc3) || !read(image, kernel.descriptor + 48, kernel.rsrc1) ||
            !read(image, kernel.descriptor + 52, kernel.rsrc2) || !read(image, kernel.descriptor + 56, kernel.properties) ||
            !read(image, kernel.descriptor + 58, kernel.preload)) return false;
        if (group != kernel.groupSize || priv != kernel.privateSize || args != kernel.kernargSize) return false;
        const __int128 entry = __int128(symbol.value) + displacement;
        if (entry < 0 || entry > UINT64_MAX || uint64_t(entry) % 256 || !loadedRange(uint64_t(entry), 4, true)) return false;
        kernel.entry = uint64_t(entry) - begin;
    }
    for (const auto &section : sections) {
        if (section.type != 4 || !section.size) continue; // SHT_RELA
        if (section.entrySize != 24 || section.size % 24 || section.link >= sections.size()) return false;
        const auto &symbolTable = sections[section.link];
        if ((symbolTable.type != 11 && symbolTable.type != 2) || symbolTable.entrySize != 24) return false;
        for (uint64_t offset = 0; offset < section.size; offset += 24) {
            ELFRelocation relocation{}; ELFSymbol symbol{};
            if (!read(file, section.offset + offset, relocation)) return false;
            const uint32_t type = uint32_t(relocation.info);
            if (!type) continue;
            const auto width=relocationWidth(type);
            if (!width || !loadedRange(relocation.offset,width)) return false;
            const auto symbolIndex = relocation.info >> 32;
            if (type == 13) { if (symbolIndex) return false; }
            else {
                if (!range(symbolIndex * 24, 24, symbolTable.size) || !read(file, symbolTable.offset + symbolIndex * 24, symbol)) return false;
                if (!symbol.section || (symbol.section != 0xfff1 && !loadedRange(symbol.value, std::max<uint64_t>(symbol.size, 1)))) return false;
            }
            object.relocations.push_back({relocation.offset - begin, symbol.value, relocation.addend, type, symbol.section == 0xfff1});
        }
    }
    output = std::move(object); return true;
}
bool relocateCodeObject(CodeObject &object, uint64_t gpuAddress) {
    if (gpuAddress < object.virtualBase || object.image.size() > UINT64_MAX - gpuAddress) return false;
    const auto bias = gpuAddress - object.virtualBase;
    // Validate every fixup before changing the image, so a rejected relocation
    // cannot leave an executable half-rebased.
    const auto evaluate = [&](const Relocation &relocation,uint64_t &encoded) {
        const auto width=relocationWidth(relocation.type);
        if (!width || !range(relocation.offset,width,object.image.size())) return false;
        // Metadata was checked against these descriptor bytes before relocation.
        for (const auto &kernel : object.kernels)
            if (relocation.offset<kernel.descriptor+64 && kernel.descriptor<relocation.offset+width)
                return false;
        const __int128 value=(relocation.type==13 ? __int128(bias) :
            __int128(relocation.symbol)+(relocation.absolute ? 0 : bias))+relocation.addend;
        if (value<0 || value>UINT64_MAX || (relocation.type==6 && value>UINT32_MAX)) return false;
        encoded=uint64_t(value);
        if (relocation.type==2) encoded>>=32;
        return true;
    };
    for (const auto &relocation : object.relocations) {
        uint64_t encoded;
        if (!evaluate(relocation,encoded)) return false;
    }
    for (const auto &relocation : object.relocations) {
        uint64_t encoded;
        evaluate(relocation,encoded);
        if (relocationWidth(relocation.type)==4) {
            const auto word=uint32_t(encoded);
            std::memcpy(object.image.data()+relocation.offset,&word,4);
        } else std::memcpy(object.image.data()+relocation.offset,&encoded,8);
    }
    return true;
}
}
