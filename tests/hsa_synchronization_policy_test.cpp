#include "synchronization_policy.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace mac_hsa;
    assert(synchronizationCapabilities(MemoryPath::HostOnly)==MAC_HSA_SYNC_CPU_LOCAL_ATOMICS);
    assert(!synchronizationCapabilities(MemoryPath::DriverKitShared));
    DeviceSnapshot snapshot{1,190,15,256ull<<20,32ull<<30,12,0,1};
    assert(synchronizationCapabilities(MemoryPath::DeviceVRAM,&snapshot)==MAC_HSA_SYNC_GPU_LOCAL_ATOMICS);
    const uint32_t expected=MAC_HSA_SYNC_CPU_LOCAL_ATOMICS|MAC_HSA_SYNC_GPU_LOCAL_ATOMICS|
        MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS|MAC_HSA_SYNC_OWNERSHIP_TRANSFER;
    for(auto caps:{AtomicEvidence::Unknown,AtomicEvidence::Unsupported,AtomicEvidence::Advertised})
        for(auto config:{AtomicEvidence::Unknown,AtomicEvidence::Unsupported,AtomicEvidence::Advertised}) {
            snapshot.originalAtomicCaps.captured=true;
            snapshot.originalAtomicCaps.capabilities=caps;snapshot.originalAtomicCaps.configuration=config;
            assert(!nativeMixedAtomicBackendQualified(MemoryPath::DriverKitShared,snapshot));
            assert(synchronizationCapabilities(MemoryPath::DriverKitShared,&snapshot)==expected);
        }
    // No currently qualified native backend, even when every prerequisite is
    // advertised. Changing a control register never updates cached originals.
    snapshot.originalAtomicCaps.captured=false;
    assert(synchronizationCapabilities(MemoryPath::DriverKitShared,&snapshot)==expected);
    snapshot.build=189;
    assert(!(synchronizationCapabilities(MemoryPath::DriverKitShared,&snapshot)&MAC_HSA_SYNC_OWNERSHIP_TRANSFER));
    snapshot.build=186;
    assert(!synchronizationCapabilities(MemoryPath::DriverKitShared,&snapshot));
    snapshot.build=190;snapshot.gfxRevision=2;
    assert(!synchronizationCapabilities(MemoryPath::DriverKitShared,&snapshot));
    for(const uint64_t build:{186,187,189,190,192,193,194}) {
        snapshot.build=build;snapshot.gfxRevision=1;
        assert(useSignalMailbox(MemoryPath::DriverKitShared,snapshot)==(build>=193));
        assert(useSignalMailbox(MemoryPath::DriverKitShared,snapshot,"")==(build>=193));
        assert(useSignalMailbox(MemoryPath::DriverKitShared,snapshot,"mailbox")== (build>=190));
        assert(!useSignalMailbox(MemoryPath::DriverKitShared,snapshot,"one-shot"));
        assert(!useSignalMailbox(MemoryPath::DriverKitShared,snapshot,"unknown"));
        assert(!useSignalMailbox(MemoryPath::HostOnly,snapshot,"mailbox"));
        assert(!useSignalMailbox(MemoryPath::DeviceVRAM,snapshot,"mailbox"));
        for(uint32_t revision:{0,2}) {
            snapshot.gfxRevision=revision;
            assert(!useSignalMailbox(MemoryPath::DriverKitShared,snapshot));
            assert(!useSignalMailbox(MemoryPath::DriverKitShared,snapshot,"mailbox"));
        }
    }
    puts("Synchronization policy: CPU/GPU ownership domains, mapping/build/ASIC gates, unknown/missing/advertised original PCIe prerequisites, and mediated fallback passed.");
}
