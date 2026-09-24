#pragma once
#include "transport.h"
#include "mac_hsa.h"
#include <cstring>

namespace mac_hsa {
enum class MemoryPath { HostOnly, DeviceVRAM, DriverKitShared };

inline bool nativeMixedAtomicBackendQualified(MemoryPath path,const DeviceSnapshot &device) {
    if (path!=MemoryPath::DriverKitShared || !hasOriginalAtomicPrerequisites(device.originalAtomicCaps))
        return false;
    // Advertisement is necessary, not sufficient: no current implementation is
    // qualified for this mapping's width, scope, and ARM64/GPU atomic domain.
    return false;
}

// Qualification belongs to a device + mapping path, never an endpoint PCIe bit.
// No current profile establishes concurrent native ARM64/GFX12 RMW on one word.
// Ordered ownership transfer and GPU-mediated HSA signals are distinct contracts.
inline uint32_t synchronizationCapabilities(MemoryPath path, const DeviceSnapshot *device=nullptr) {
    if (path==MemoryPath::HostOnly) return MAC_HSA_SYNC_CPU_LOCAL_ATOMICS;
    if (!device || !supportsPersistentQueues(*device)) return 0;
    uint32_t flags=MAC_HSA_SYNC_GPU_LOCAL_ATOMICS;
    if (path==MemoryPath::DriverKitShared) {
        flags|=MAC_HSA_SYNC_CPU_LOCAL_ATOMICS|MAC_HSA_SYNC_GPU_MEDIATED_SIGNALS;
        if (device->build>=kQueueResourceDriverBuild) flags|=MAC_HSA_SYNC_OWNERSHIP_TRANSFER;
        if (nativeMixedAtomicBackendQualified(path,*device)) flags|=MAC_HSA_SYNC_NATIVE_CPU_GPU_RMW;
    }
    // IRQ wakeup is not wired to the shared request/completion protocol yet.
    return flags;
}

// The persistent signal service is qualified on the gfx1201 DriverKit DMA
// mapping with driver193+. Earlier ownership-capable builds remain explicit
// experiments. This GPU-mediated choice never enables native mixed RMW.
inline bool useSignalMailbox(MemoryPath path,const DeviceSnapshot &device,const char *override=nullptr) {
    if (!(synchronizationCapabilities(path,&device)&MAC_HSA_SYNC_OWNERSHIP_TRANSFER)) return false;
    if (override && *override) return std::strcmp(override,"mailbox")==0;
    return device.build>=193;
}
}
