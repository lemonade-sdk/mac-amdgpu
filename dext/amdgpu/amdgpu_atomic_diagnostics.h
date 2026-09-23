#pragma once
#include <stdint.h>
#include "amdgpu_ip.h"

namespace amdgpu {
// QueryInfo7 ABI: snapshots only owned shared memory/queue resources. No
// arbitrary MMIO addresses, cache changes or PCI configuration writes.
namespace atomic_diag {
enum Field : unsigned {
    Version, ValidFields, GPUAddress, DMAAddress, PTEVRAMOffset, PTEActual,
    PTEExpected, MQDGPUAddress, MQDBackingHQStatus0, PCIeCapabilityOffset,
    PCIeDeviceCapabilities2, PCIeDeviceControl2, GFXHUBPageTableBase,
    GFXHUBContext0Control, CPUMapOptions, CPUCacheAttributes, Count
};
constexpr uint64_t kVersion = 1;
constexpr uint64_t kPTEValid = 1, kMQDValid = 2, kPCIeValid = 4, kGFXHUBValid = 8;
constexpr uint64_t kKnownFields = 15;
constexpr uint64_t kUnknown = UINT64_MAX;
struct Snapshot { uint64_t values[Count]{}; };
static_assert(sizeof(Snapshot) == 128);

// Linux gmc_v12_0.c uses dma_set_mask_and_coherent(DMA_BIT_MASK(44));
// the wider GPU PTE physical-address field is not the DMA address capability.
constexpr uint64_t kDMALimit = 1ull << 44;
inline bool mapping_addresses(uint64_t gpuBase, uint64_t dmaBase, uint64_t bytes,
    uint64_t gartOffset, uint64_t tableVRAMOffset, uint64_t tableBytes,
    uint64_t offset, Snapshot &out) {
    if (!bytes || (bytes & 4095) || (gpuBase & 4095) || (dmaBase & 4095) ||
        (gartOffset & 4095) || (tableVRAMOffset & 7) || (tableBytes & 7) ||
        (offset & 7) || offset > bytes || 8 > bytes-offset ||
        dmaBase >= kDMALimit || bytes > kDMALimit-dmaBase ||
        gpuBase >= (1ull<<47) || bytes > (1ull<<47)-gpuBase ||
        gartOffset/4096 > UINT64_MAX-(offset/4096)) return false;
    const uint64_t page = gartOffset/4096 + offset/4096;
    if (page >= tableBytes/8 || page > (UINT64_MAX-tableVRAMOffset)/8) return false;
    Snapshot snapshot{};
    snapshot.values[Version] = kVersion;
    snapshot.values[GPUAddress] = gpuBase+offset;
    snapshot.values[DMAAddress] = dmaBase+offset;
    snapshot.values[PTEVRAMOffset] = tableVRAMOffset+page*8;
    snapshot.values[PTEExpected] = ((dmaBase+offset)&~uint64_t(4095))|PTEFlags::SYSMEM_RW;
    snapshot.values[PCIeCapabilityOffset] = kUnknown;
    snapshot.values[PCIeDeviceCapabilities2] = kUnknown;
    snapshot.values[PCIeDeviceControl2] = kUnknown;
    snapshot.values[GFXHUBPageTableBase] = kUnknown;
    snapshot.values[GFXHUBContext0Control] = kUnknown;
    snapshot.values[CPUMapOptions] = 0; // requested default, not actual MAIR
    snapshot.values[CPUCacheAttributes] = kUnknown; // no public DriverKit query
    out = snapshot;
    return true;
}
inline bool snapshot_valid(const Snapshot &s, uint64_t expectedGPU) {
    const auto *v=s.values;
    if (v[Version]!=kVersion || (v[ValidFields]&~kKnownFields) ||
        (v[ValidFields]&(kPTEValid|kMQDValid))!=(kPTEValid|kMQDValid) ||
        v[GPUAddress]!=expectedGPU || (expectedGPU&7) || expectedGPU>=(1ull<<47) ||
        v[DMAAddress]>=kDMALimit || (v[DMAAddress]&4095)!=(expectedGPU&4095) ||
        (v[PTEVRAMOffset]&7) || v[PTEExpected]!=((v[DMAAddress]&~uint64_t(4095))|PTEFlags::SYSMEM_RW) ||
        !v[MQDGPUAddress] || (v[MQDGPUAddress]&16383) || v[MQDGPUAddress]>=(1ull<<48) ||
        v[MQDBackingHQStatus0]>UINT32_MAX || v[CPUMapOptions]!=0 || v[CPUCacheAttributes]!=kUnknown)
        return false;
    if (v[ValidFields]&kPCIeValid) {
        if (v[PCIeCapabilityOffset]<0x40 || v[PCIeCapabilityOffset]>0xd4 ||
            (v[PCIeCapabilityOffset]&3) || v[PCIeDeviceCapabilities2]>UINT32_MAX ||
            v[PCIeDeviceControl2]>UINT16_MAX) return false;
    } else if (v[PCIeCapabilityOffset]!=kUnknown || v[PCIeDeviceCapabilities2]!=kUnknown ||
               v[PCIeDeviceControl2]!=kUnknown) return false;
    if (v[ValidFields]&kGFXHUBValid) {
        if (v[GFXHUBContext0Control]>UINT32_MAX) return false;
    } else if (v[GFXHUBPageTableBase]!=kUnknown || v[GFXHUBContext0Control]!=kUnknown) return false;
    return true;
}
} // namespace atomic_diag
} // namespace amdgpu
