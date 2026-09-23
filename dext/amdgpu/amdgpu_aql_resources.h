#pragma once
#include "../../hsa/third_party/hsa/include/hsa/amd_hsa_queue.h"
#include "amdgpu_arch_capabilities.h"
#include <stdint.h>

namespace amdgpu {
// ROCr GpuAgent::QueueCreate and AqlQueue::Fill*Gfx12. A 64-lane allocation
// is deliberately conservative: CP halves lane-byte capacity for wave32.
constexpr uint32_t kAQLMaxPrivateBytes = 262128;
constexpr uint32_t kAQLGroupBytes = 65536;
constexpr uint32_t kAQLScratchMinimumWavesPerEngine = 32;
constexpr uint32_t kAQLGroupApertureHi = 0x10000;
constexpr uint32_t kAQLPrivateApertureHi = 0x20000;

inline bool aql_scratch_geometry(IPVersion ip,uint32_t laneBytes, uint32_t cuCount,
    uint32_t engines, uint32_t wavesPerCU, uint32_t &alignedLaneBytes, uint32_t &wavesPerEngine,
    uint64_t &bytes) {
    const auto *capability=scratch_architecture(ip);
    if (!capability || !aql_scratch_layout_supported(ip) ||
        !laneBytes || laneBytes > kAQLMaxPrivateBytes || !cuCount ||
        !engines || engines > 4 || cuCount > 512 || !wavesPerCU || wavesPerCU>64) return false;
    alignedLaneBytes=(laneBytes+15)&~15u;
    if (alignedLaneBytes>kAQLMaxPrivateBytes ||
        uint64_t(alignedLaneBytes)*64/capability->waveSizeUnitBytes >= (1ull<<capability->waveSizeBits)) return false;
    wavesPerEngine=((cuCount+engines-1)/engines)*wavesPerCU;
    // Allocate a multiple of the largest wave32 workgroup slot count.
    wavesPerEngine=(wavesPerEngine+31)&~31u;
    if (wavesPerEngine>=(1u<<capability->wavesBits)) return false;
    bytes=uint64_t(alignedLaneBytes)*64*wavesPerEngine*engines;
    return true;
}
inline bool aql_scratch_metadata(IPVersion ip,amd_queue_t &metadata, uint64_t base,
    uint64_t bytes, uint32_t laneBytes, uint32_t engines, uint32_t wavesPerEngine) {
    const auto *capability=scratch_architecture(ip);
    if (!capability || !aql_scratch_layout_supported(ip) ||
        capability->srdLayout!=ScratchSRDLayout::Gfx12 || !capability->wavesPerEngine) return false;
    const uint64_t waveBytes=uint64_t(laneBytes)*64;
    const uint64_t waveSize=waveBytes/capability->waveSizeUnitBytes;
    if (!base || (base&16383) || base>=(1ull<<capability->srdAddressBits) ||
        !bytes || bytes>=(1ull<<capability->srdRecordBits) ||
        bytes>(1ull<<capability->srdAddressBits)-base || !laneBytes || (laneBytes&15) ||
        waveBytes%capability->waveSizeUnitBytes || waveSize>=(1ull<<capability->waveSizeBits) ||
        laneBytes>kAQLMaxPrivateBytes || !engines || engines>4 ||
        wavesPerEngine<kAQLScratchMinimumWavesPerEngine || wavesPerEngine>=(1u<<capability->wavesBits) ||
        uint64_t(laneBytes)*64*engines*wavesPerEngine>bytes) return false;
    metadata.scratch_resource_descriptor[0]=uint32_t(base);
    metadata.scratch_resource_descriptor[1]=uint32_t(base>>32)|(1u<<30);
    metadata.scratch_resource_descriptor[2]=uint32_t(bytes);
    // SQ_SEL_X/Y/Z/W, BUF_FORMAT_32_UINT, ADD_TID_ENABLE, OOB_SELECT=2.
    metadata.scratch_resource_descriptor[3]=4u|(5u<<3)|(6u<<6)|(7u<<9)|
        (0x14u<<12)|(1u<<23)|(2u<<28);
    metadata.scratch_backing_memory_location=base;
    metadata.scratch_wave64_lane_byte_size=laneBytes;
    metadata.compute_tmpring_size=wavesPerEngine|(uint32_t(waveSize)<<capability->wavesBits);
    return true;
}
}
