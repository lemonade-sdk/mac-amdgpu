#pragma once
#include "amdgpu_aql_abi.h"
#include "amdgpu_queue_topology.h"
#include "amdgpu_vram.h"
#include "amdgpu_regs.h"
namespace amdgpu {
struct GMCContext;
struct MESContext;
struct GFXConfig;
struct AQLLaunch { VRAMAllocation storage; bool retained; };
struct AQLLaunchResult {
    uint64_t completion, inactive, readIndex;
    uint32_t stage; // 0 preflight, 1 upload, 2 map, 3 publish/wait, 4 unmap, 5 complete
};
kern_return_t aql_launch(DeviceContext &, GMCContext &, MESContext &, const GFXConfig &,
    AQLLaunch &, uint64_t descriptorVA, uint64_t kernargVA,
    const AQLDispatchRequest &, AQLLaunchResult &);
struct PersistentAQLQueue {
    VRAMAllocation storage;
    void *owner, *metadataCPU;
    uint64_t handle, ringHandle, metadataHandle, metadataVA;
    uint64_t lastDoorbell;
    uint32_t packets, slot;
    bool mapped, retained, published;
};
kern_return_t aql_queue_open(DeviceContext &, GMCContext &, MESContext &, const GFXConfig &,
    PersistentAQLQueue &, uint64_t ringVA, uint64_t metadataVA, void *metadataCPU,
    uint32_t packets, uint32_t slot);
kern_return_t aql_queue_kick(DeviceContext &, PersistentAQLQueue &, uint64_t lastPacket);
kern_return_t aql_queue_close(DeviceContext &, GMCContext &, MESContext &, PersistentAQLQueue &);

}
