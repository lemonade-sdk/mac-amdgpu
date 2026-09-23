#pragma once
#include <stdint.h>

namespace amdgpu {
namespace atomic_requester {
// Explicit experiment only. This does not qualify the upstream PCIe path,
// change MQD policy or advertise CPU/GPU system atomic support.
constexpr uint16_t kBit=0x40;
enum Field : unsigned { Version,Before,Requested,Observed,Original,Active,RestorePending,Status,Count };
struct Snapshot { uint64_t values[Count]{}; };
inline bool valid(const Snapshot &s) {
    if(s.values[Version]!=1 || s.values[Active]>1 || s.values[RestorePending]>1 ||
       s.values[Status]>UINT32_MAX || (s.values[RestorePending] && !s.values[Active])) return false;
    for(unsigned i=Before;i<=Original;++i)
        if(s.values[i]>UINT16_MAX && s.values[i]!=UINT64_MAX) return false;
    return true;
}
struct Experiment {
    void *owner=nullptr;
    uint64_t capability=0;
    uint16_t original=0;
    bool pending=false, acquiredExclusive=false;

    Snapshot snapshot() const {
        Snapshot s{};s.values[Version]=1;
        s.values[Before]=s.values[Requested]=s.values[Observed]=UINT64_MAX;
        s.values[Original]=owner ? original : UINT64_MAX;
        s.values[Active]=owner!=nullptr;s.values[RestorePending]=pending;
        return s;
    }
    template<class PCI> static bool locate(PCI &pci,uint64_t &cap,const char **reason=nullptr) {
        auto fail=[&](const char *why) {if(reason) *reason=why;return false;};
        uint32_t identity=UINT32_MAX;pci.ConfigurationRead32(0,&identity);
        if(identity!=0x75511002u) return fail("live-identity-not-1002-7551");
        if(pci.FindPCICapability(0x10,0,&cap)!=0 || cap<0x40 || cap>0xd4 || (cap&3))
            return fail("pcie-capability-unavailable-or-invalid");
        uint16_t header=UINT16_MAX,flags=UINT16_MAX;
        pci.ConfigurationRead16(cap,&header);pci.ConfigurationRead16(cap+2,&flags);
        const unsigned type=(flags>>4)&15;
        if((header&255)!=0x10 || flags==UINT16_MAX) return fail("pcie-capability-read-invalid");
        if((flags&15)!=2) return fail("pcie-capability-version-not-2");
        if(type!=0 && type!=1) return fail("pcie-device-not-endpoint");
        return true;
    }
    template<class PCI> bool begin(PCI &pci,void *client,Snapshot &out) {
        out=snapshot();uint64_t cap=0;
        if(owner || !client || !locate(pci,cap)) return false;
        uint16_t before=UINT16_MAX;pci.ConfigurationRead16(cap+0x28,&before);
        if(before==UINT16_MAX) return false;
        // Record recovery state before the first possibly successful write.
        owner=client;capability=cap;original=before;pending=true;
        out=snapshot();out.values[Before]=before;out.values[Requested]=before|kBit;
        pci.ConfigurationWrite16(cap+0x28,uint16_t(out.values[Requested]));
        uint16_t observed=UINT16_MAX;pci.ConfigurationRead16(cap+0x28,&observed);
        out.values[Observed]=observed;
        return observed==out.values[Requested];
    }
    template<class PCI> bool restore(PCI &pci,Snapshot &out) {
        out=snapshot();uint64_t cap=0;
        if(!owner || !locate(pci,cap) || cap!=capability) return false;
        uint16_t before=UINT16_MAX;pci.ConfigurationRead16(cap+0x28,&before);
        if(before==UINT16_MAX) return false;
        out.values[Before]=before;
        out.values[Requested]=(before&~kBit)|(original&kBit);
        pending=true;out.values[RestorePending]=1;
        // Only bit6 belongs to this experiment; retain any other control bits.
        pci.ConfigurationWrite16(cap+0x28,uint16_t(out.values[Requested]));
        uint16_t observed=UINT16_MAX;pci.ConfigurationRead16(cap+0x28,&observed);
        out.values[Observed]=observed;
        if(observed!=out.values[Requested]) return false;
        pending=false;out.values[RestorePending]=0;return true;
    }
    template<class PCI> bool verifyAfterReset(PCI &pci) {
        if(!owner) return true;
        uint64_t cap=0;uint16_t observed=UINT16_MAX;
        if(locate(pci,cap) && cap==capability) pci.ConfigurationRead16(cap+0x28,&observed);
        pending=observed==UINT16_MAX || ((observed^original)&kBit);
        return !pending;
    }
};
} // namespace atomic_requester
} // namespace amdgpu
