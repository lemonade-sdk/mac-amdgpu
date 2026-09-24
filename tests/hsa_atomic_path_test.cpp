#include "../hsa/src/atomic_path.h"
#include <cassert>
using namespace mac_hsa;
OriginalAtomicCaps path() {
    OriginalAtomicCaps a;a.captured=true;a.count=4;
    for(auto &n:a.functions) {n.expressKnown=n.capabilities2Known=n.control2Known=true;}
    a.functions[0].expressCapabilities=0x12;a.functions[0].control2=0x40;
    a.functions[1].expressCapabilities=0x62;a.functions[1].capabilities2=0x40;
    a.functions[2].expressCapabilities=0x52;a.functions[2].capabilities2=0x40;
    a.functions[3].expressCapabilities=0x42;a.functions[3].capabilities2=0x180;
    return a;
}
int main() {
    auto a=path();assessOriginalAtomicCaps(a,true);assert(hasOriginalAtomicPrerequisites(a));
    for(unsigned i=1;i<4;++i) {
        a=path();a.functions[i].capabilities2=0;assessOriginalAtomicCaps(a,true);
        assert(a.capabilities==AtomicEvidence::Unsupported && !hasOriginalAtomicPrerequisites(a));
    }
    a=path();a.functions[0].capabilities2=0x380;a.functions[3].capabilities2=0x80;
    assessOriginalAtomicCaps(a,true);assert(a.missingCapabilities&RootCompletion64); // endpoint is not root
    a=path();a.functions[1].capabilities2Known=false;assessOriginalAtomicCaps(a,true);
    assert(a.capabilities==AtomicEvidence::Unknown && !hasOriginalAtomicPrerequisites(a));
    a=path();a.functions[0].control2=0;assessOriginalAtomicCaps(a,true);
    const auto original=a;assert(a.capabilities==AtomicEvidence::Advertised && a.configuration==AtomicEvidence::Unsupported);
    a.functions[0].control2|=0x40;assessOriginalAtomicCaps(a,true);
    assert(hasOriginalAtomicPrerequisites(a) && !hasOriginalAtomicPrerequisites(original));
    a=path();a.functions[2].control2=0x80;assessOriginalAtomicCaps(a,true);
    assert(a.missingConfiguration&EgressUnblocked);
    a=path();a.functions[1].control2=0x80;assessOriginalAtomicCaps(a,true);
    assert(hasOriginalAtomicPrerequisites(a)); // downstream egress is opposite to GPU→root direction
    a=path();a.functions[2].control2Known=false;assessOriginalAtomicCaps(a,true);
    assert(a.configuration==AtomicEvidence::Unknown);
    a=path();a.count=3;assessOriginalAtomicCaps(a,false);
    assert(a.unknownCapabilities&RootPresent);
    a=path();a.functions[1].expressKnown=false;assessOriginalAtomicCaps(a,true);
    assert(a.unknownCapabilities&CompleteAncestry);
    // The actual Apple/Intel route remains unsupported after forcing requester on.
    a=path();a.functions[1].capabilities2=a.functions[2].capabilities2=0x10800;
    a.functions[3].capabilities2=0xc1f;assessOriginalAtomicCaps(a,true);
    assert(a.missingCapabilities==(BridgeRouting|RootCompletion32|RootCompletion64));
    assert(!hasOriginalAtomicPrerequisites(a));
}
