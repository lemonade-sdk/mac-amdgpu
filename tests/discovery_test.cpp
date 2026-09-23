#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "amdgpu_ip.h"
namespace linux_reference {
#include "discovery_gc_linux.inc"
}
static_assert(sizeof(linux_reference::gc_info_v1_0)==88 && sizeof(linux_reference::gc_info_v1_1)==100);
static_assert(sizeof(linux_reference::gc_info_v1_2)==132 && sizeof(linux_reference::gc_info_v1_3)==164);
static_assert(sizeof(linux_reference::gc_info_v2_0)==80 && sizeof(linux_reference::gc_info_v2_1)==108);
static_assert(offsetof(linux_reference::gc_info_v1_0,gc_num_sa_per_se)==12+16*4);
static_assert(offsetof(linux_reference::gc_info_v2_0,gc_num_sh_per_se)==12+2*4);
static_assert(offsetof(linux_reference::gc_info_v1_0,gc_wave_size)==12+11*4);
static_assert(offsetof(linux_reference::gc_info_v2_0,gc_wave_size)==12+11*4);
static_assert(offsetof(linux_reference::gc_info_v1_0,gc_max_scratch_slots_per_cu)==12+13*4);
static_assert(offsetof(linux_reference::gc_info_v2_0,gc_lds_size)==12+14*4);
using namespace amdgpu;
using kern_return_t = int;
constexpr int kIOReturnSuccess=0, kIOReturnInvalid=1, kIOReturnBadArgument=2;
struct DeviceContext { IPBaseTable ip; };
template<class... T> void discardLog(T...) {}
#define DISC_LOG(...) discardLog(__VA_ARGS__)
#include "discovery_types_under_test.inc"
#include "discovery_parser_under_test.inc"
static DiscoveryBinaryHeader &header(std::vector<uint8_t> &b) {
    return *reinterpret_cast<DiscoveryBinaryHeader *>(b.data());
}
static DiscoveryIPDSHeader &ipds(std::vector<uint8_t> &b) {
    return *reinterpret_cast<DiscoveryIPDSHeader *>(b.data()+header(b).table_list[0].offset);
}
static void sumBinary(std::vector<uint8_t> &b) {
    auto &h=header(b); h.binary_checksum=compute_checksum(b.data()+10,h.binary_size-10);
}
static void sumBoth(std::vector<uint8_t> &b) {
    auto &h=header(b); h.table_list[0].checksum=compute_checksum(b.data()+h.table_list[0].offset,ipds(b).size);
    sumBinary(b);
}
static std::vector<uint8_t> makeBinary(bool wide=false) {
    // Deliberately misalign the die and base-address payload.
    const size_t offset=sizeof(DiscoveryBinaryHeader);
    const size_t die=offset+sizeof(DiscoveryIPDSHeader)+1;
    const size_t end=die+sizeof(DiscoveryDieHeader)+sizeof(DiscoveryIPv4)+(wide?16:8);
    std::vector<uint8_t> b(end+64,0);
    auto &h=header(b); h.signature=kDiscoverySignature; h.version_major=1; h.binary_size=end; h.table_list[0].offset=offset;
    auto &t=ipds(b); t.signature=kDiscoveryIPDSSignature; t.version=4; t.size=end-offset; t.num_dies=1; t.die_info[0].die_offset=die; t.flags=wide;
    auto *d=reinterpret_cast<DiscoveryDieHeader *>(b.data()+die); d->num_ips=1;
    auto *p=reinterpret_cast<DiscoveryIPv4 *>(b.data()+die+sizeof(*d)); p->hw_id=HWID::GC; p->num_base_address=2; p->major=12; p->revision=1;
    uint8_t *addr=reinterpret_cast<uint8_t *>(p)+sizeof(*p);
    if(wide) { uint64_t v[2]={0x12345678C0001260ull,0xABCDEF00C000A000ull}; memcpy(addr,v,sizeof(v)); }
    else { uint32_t v[2]={0x1260,0xA000}; memcpy(addr,v,sizeof(v)); }
    sumBoth(b); return b;
}
static void put32(std::vector<uint8_t> &b,size_t offset,uint32_t value) {
    for(unsigned i=0;i<4;++i) b.at(offset+i)=uint8_t(value>>(8*i));
}
static void sumGC(std::vector<uint8_t> &b) {
    auto &entry=header(b).table_list[1];
    entry.checksum=compute_checksum(b.data()+entry.offset,entry.size);sumBinary(b);
}
static std::vector<uint8_t> withGC(uint16_t major,uint16_t minor) {
    const unsigned sizes[2][4]={{88,100,132,164},{80,108,0,0}};
    auto b=makeBinary();const auto offset=header(b).binary_size+1;
    const auto bytes=sizes[major-1][minor];b.resize(offset+bytes+64,0);
    auto &h=header(b);h.binary_size=offset+bytes;h.table_list[1].offset=offset;h.table_list[1].size=bytes;
    put32(b,offset,0x4347);put32(b,offset+4,uint32_t(major)|(uint32_t(minor)<<16));put32(b,offset+8,bytes);
    const auto word=[&](unsigned index,uint32_t value){put32(b,offset+12+index*4,value);};
    // Synthetic tables in each supported schema reproduce the hardware188
    // R9700 geometry (4 SE x 2 SA x 8 CU), not a captured GC binary.
    word(0,4);word(1,major==1?2:8);word(2,2);word(3,4);
    word(11,32);word(12,16);word(13,32);word(14,64);
    if(major==1) word(16,2);
    sumGC(b);return b;
}
static void rejects(std::vector<uint8_t> b) {
    DeviceContext dev; dev.ip.setBase(IPBlock::GC,0,0x7777);
    dev.ip.gfx.valid=true;dev.ip.gfx.max_cu_per_sh=31;
    auto saved=dev.ip;
    DiscoveryParseResult r;
    assert(discovery_parse(b.data(),b.size(),dev,&r)!=0 && !r.ok && r.err[0]);
    assert(memcmp(&dev.ip,&saved,sizeof(saved))==0);
}
int main() {
    static_assert(sizeof(DiscoveryBinaryHeader)==60 && sizeof(DiscoveryIPDSHeader)==80);
    for(bool wide:{false,true}) {
        auto b=makeBinary(wide); DeviceContext d; DiscoveryParseResult r;
        assert(discovery_parse(b.data(),b.size(),d,&r)==0 && r.ok);
        assert(d.ip.getBase(IPBlock::GC,0)==0x1260 && d.ip.getBase(IPBlock::GC,1)==0xA000);
        assert(r.ip_version_major==12 && r.ip_version_rev==1 && r.num_ips_total==1);
        assert(!d.ip.isResolved(IPBlock::GMC));
    }
    {
        auto binary=makeBinary();
        auto &h=header(binary);auto &t=ipds(binary);
        auto *die=reinterpret_cast<DiscoveryDieHeader *>(binary.data()+t.die_info[0].die_offset);
        ++die->num_ips;
        auto *smuio=reinterpret_cast<DiscoveryIPv4 *>(binary.data()+h.binary_size);
        *smuio={HWID::SMUIO,0,1,14,0,2,0};
        const uint32_t base=0x5a0;
        memcpy(reinterpret_cast<uint8_t *>(smuio)+sizeof(*smuio),&base,sizeof(base));
        h.binary_size+=sizeof(*smuio)+sizeof(base);t.size+=sizeof(*smuio)+sizeof(base);sumBoth(binary);
        DeviceContext parsed;DiscoveryParseResult result;
        assert(discovery_parse(binary.data(),binary.size(),parsed,&result)==0 && result.ok);
        assert(parsed.ip.get(IPBlock::SMUIO)==base && parsed.ip.isVersion(IPBlock::SMUIO,14,0,2));
    }
    for(unsigned major=1;major<=2;++major) {
        for(unsigned minor=0;minor<(major==1?4:2);++minor) {
            auto binary=withGC(major,minor);DeviceContext parsed;DiscoveryParseResult result;
            assert(discovery_parse(binary.data(),binary.size(),parsed,&result)==0 && result.ok);
            const auto &gc=parsed.ip.gfx;
            assert(gc.valid && gc.max_shader_engines==4 && gc.max_sh_per_se==2 && gc.max_cu_per_sh==8);
            assert(gc.max_backends_per_se==4 && gc.wave_front_size==32 && gc.max_waves_per_simd==16);
            assert(gc.max_scratch_slots_per_cu==32 && gc.lds_size_kib==64);
            for(unsigned index:{0,3,11,12,13,14}) {
                auto bad=binary;put32(bad,header(bad).table_list[1].offset+12+index*4,0);sumGC(bad);rejects(bad);
            }
            auto bad=binary;header(bad).table_list[1].checksum^=1;sumBinary(bad);rejects(bad);
            bad=binary;put32(bad,header(bad).table_list[1].offset,0);sumGC(bad);rejects(bad);
            bad=binary;put32(bad,header(bad).table_list[1].offset+8,12);sumGC(bad);rejects(bad);
            bad=binary;put32(bad,header(bad).table_list[1].offset+8,UINT32_MAX);sumGC(bad);rejects(bad);
            bad=binary;put32(bad,header(bad).table_list[1].offset+4,0x01000001);sumGC(bad);rejects(bad);
            for(unsigned remaining=0;remaining<header(binary).table_list[1].size;++remaining) {
                bad=binary;header(bad).binary_size=header(bad).table_list[1].offset+remaining;sumBinary(bad);rejects(bad);
            }
        }
    }
    auto overflow=withGC(1,0);put32(overflow,header(overflow).table_list[1].offset+16,UINT32_MAX);sumGC(overflow);rejects(overflow);
    std::ifstream f("docs/reference/gfx1151_discovery.bin",std::ios::binary); assert(f);
    std::vector<uint8_t> fixture((std::istreambuf_iterator<char>(f)),{});
    DeviceContext dev; DiscoveryParseResult r;
    assert(discovery_parse(fixture.data(),fixture.size(),dev,&r)==0 && r.ok);
    assert(r.ip_version_major==11 && r.ip_version_minor==5 && r.ip_version_rev==1);
    assert(dev.ip.gfx.valid && dev.ip.gfx.max_shader_engines==2 && dev.ip.gfx.max_sh_per_se==2 && dev.ip.gfx.max_cu_per_sh==10);
    assert(dev.ip.gfx.wave_front_size==32 && dev.ip.gfx.max_waves_per_simd==16 && dev.ip.gfx.max_scratch_slots_per_cu==32 && dev.ip.gfx.lds_size_kib==64);
    // Linux permits version-only IP records with no register-base array.
    // Put one between GC and MMHUB to verify advancement to the next IP.
    for (bool wide : {false,true}) {
        for (uint16_t version : {uint16_t(3),uint16_t(4)}) {
            if (wide && version==3) continue;
            auto z=makeBinary(wide);
            auto &h=header(z); auto &t=ipds(z); t.version=version;
            auto *die=reinterpret_cast<DiscoveryDieHeader *>(z.data()+t.die_info[0].die_offset);
            die->num_ips=3;
            const auto pos=h.binary_size;
            auto *empty=reinterpret_cast<DiscoveryIPv4 *>(z.data()+pos);
            *empty={HWID::SDMA0,0,0,7,0,1,0};
            auto *next=reinterpret_cast<DiscoveryIPv4 *>(z.data()+pos+sizeof(*empty));
            *next={HWID::MMHUB,0,1,4,1,0,0};
            uint64_t base=0x1a000;
            memcpy(reinterpret_cast<uint8_t *>(next)+sizeof(*next),&base,wide?8:4);
            const uint16_t added=sizeof(*empty)+sizeof(*next)+(wide?8:4);
            h.binary_size+=added; t.size+=added; sumBoth(z);
            DeviceContext parsed; DiscoveryParseResult result;
            assert(discovery_parse(z.data(),z.size(),parsed,&result)==0 && result.ok);
            assert(result.num_ips_total==3 && parsed.ip.getBase(IPBlock::MMHUB,0)==0x1a000);
            assert(!parsed.ip.isResolved(IPBlock::SDMA0));
            assert(parsed.ip.getVersion(IPBlock::SDMA0).major==7);
        }
    }
    auto b=makeBinary(); b[10]^=1; rejects(b); // binary_size participates in checksum
    b=makeBinary(); header(b).binary_checksum^=1; rejects(b);
    b=makeBinary(); header(b).table_list[0].checksum^=1; sumBinary(b); rejects(b);
    b=makeBinary(); header(b).binary_size=10; rejects(b);
    b=makeBinary(); header(b).version_major=2; sumBinary(b); rejects(b);
    b=makeBinary(); ipds(b).num_dies=17; sumBoth(b); rejects(b);
    b=makeBinary(); ipds(b).die_info[0].die_offset=header(b).binary_size; sumBoth(b); rejects(b);
    b=makeBinary(); auto *die=reinterpret_cast<DiscoveryDieHeader *>(b.data()+ipds(b).die_info[0].die_offset); die->num_ips=2; sumBoth(b); rejects(b); // valid first IP, truncated second: no partial publication
    b=makeBinary(); ipds(b).size+=1; sumBoth(b); rejects(b);
    b=makeBinary(); header(b).table_list[0].offset=header(b).binary_size; sumBinary(b); rejects(b);
    // Exercise declared-size boundary with every truncation of a real binary.
    for(size_t n=0;n<header(fixture).binary_size;++n) {
        assert(discovery_parse(fixture.data(),n,dev,&r)!=0);
    }
    puts("Discovery: captured Linux-format binary, checksums, unaligned 64-bit bases, bounds and atomic publication pass");
}
