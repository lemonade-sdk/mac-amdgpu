#pragma once
#include <array>
#include <cstdint>

namespace mac_hsa {
enum class AtomicEvidence : uint8_t { Unknown, Unsupported, Advertised };
enum AtomicRequirement : uint32_t {
    EndpointPresent=1, RootPresent=2, BridgeRouting=4, RootCompletion32=8,
    RootCompletion64=16, RequesterEnabled=32, EgressUnblocked=64, CompleteAncestry=128
};
struct AtomicPCIFunction {
    uint64_t registryID=0;
    uint32_t vendorID=0, deviceID=0, capabilities2=0;
    uint16_t expressCapabilities=0, control2=0;
    bool expressKnown=false, capabilities2Known=false, control2Known=false;
};
// Read-only registry evidence captured once, before this connection can initialize
// the GPU or run a configuration experiment. Controls and hardware capabilities
// are separate; an experimental requester write cannot mutate this snapshot.
struct OriginalAtomicCaps {
    bool captured=false, cachedOnly=true;
    AtomicEvidence capabilities=AtomicEvidence::Unknown, configuration=AtomicEvidence::Unknown;
    uint32_t missingCapabilities=0, unknownCapabilities=0, missingConfiguration=0, unknownConfiguration=0;
    uint32_t count=0;
    std::array<AtomicPCIFunction,32> functions{}; // endpoint towards root
};
inline void assessOriginalAtomicCaps(OriginalAtomicCaps &audit,bool ancestryComplete) {
    audit.missingCapabilities=audit.unknownCapabilities=0;
    audit.missingConfiguration=audit.unknownConfiguration=0;
    bool endpoint=false,root=false;
    auto requirement=[](bool known,bool supported,uint32_t bit,uint32_t &missing,uint32_t &unknown) {
        if (!known) unknown|=bit;else if (!supported) missing|=bit;
    };
    if (!ancestryComplete) audit.unknownCapabilities|=CompleteAncestry;
    for (uint32_t i=0;i<audit.count && i<audit.functions.size();++i) {
        const auto &n=audit.functions[i];
        if (!n.expressKnown) {audit.unknownCapabilities|=CompleteAncestry;continue;}
        const unsigned version=n.expressCapabilities&15,type=(n.expressCapabilities>>4)&15;
        const bool registersKnown=version>=2 && n.capabilities2Known;
        if (i==0 && (type==0 || type==1)) {
            endpoint=true;
            requirement(version>=2 && n.control2Known,(n.control2&0x40)!=0,RequesterEnabled,
                        audit.missingConfiguration,audit.unknownConfiguration);
        } else if (type==5 || type==6) {
            requirement(registersKnown,(n.capabilities2&0x40)!=0,BridgeRouting,
                        audit.missingCapabilities,audit.unknownCapabilities);
            if (type==5) requirement(version>=2 && n.control2Known,(n.control2&0x80)==0,EgressUnblocked,
                                    audit.missingConfiguration,audit.unknownConfiguration);
        } else if (type==4) {
            root=true;
            requirement(registersKnown,(n.capabilities2&0x80)!=0,RootCompletion32,
                        audit.missingCapabilities,audit.unknownCapabilities);
            requirement(registersKnown,(n.capabilities2&0x100)!=0,RootCompletion64,
                        audit.missingCapabilities,audit.unknownCapabilities);
            break;
        } else audit.unknownCapabilities|=CompleteAncestry;
    }
    if (!endpoint) audit.unknownCapabilities|=EndpointPresent;
    if (!root) audit.unknownCapabilities|=RootPresent;
    audit.capabilities=audit.missingCapabilities ? AtomicEvidence::Unsupported :
        audit.unknownCapabilities ? AtomicEvidence::Unknown : AtomicEvidence::Advertised;
    audit.configuration=audit.missingConfiguration ? AtomicEvidence::Unsupported :
        audit.unknownConfiguration || !endpoint ? AtomicEvidence::Unknown : AtomicEvidence::Advertised;
}
inline bool hasOriginalAtomicPrerequisites(const OriginalAtomicCaps &audit) {
    return audit.captured && audit.capabilities==AtomicEvidence::Advertised &&
        audit.configuration==AtomicEvidence::Advertised;
}
} // namespace mac_hsa
