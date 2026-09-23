#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "amdgpu_ip.h"
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
static void rejects(std::vector<uint8_t> b) {
    DeviceContext dev; dev.ip.setBase(IPBlock::GC,0,0x7777);
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
    std::ifstream f("docs/reference/gfx1151_discovery.bin",std::ios::binary); assert(f);
    std::vector<uint8_t> fixture((std::istreambuf_iterator<char>(f)),{});
    DeviceContext dev; DiscoveryParseResult r;
    assert(discovery_parse(fixture.data(),fixture.size(),dev,&r)==0 && r.ok);
    assert(r.ip_version_major==11 && r.ip_version_minor==5 && r.ip_version_rev==1);
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
