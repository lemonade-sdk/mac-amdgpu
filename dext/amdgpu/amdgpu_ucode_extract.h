//
//  amdgpu_ucode_extract.h — port of upstream
//  `amdgpu_ucode_init_single_fw` + `psp_get_fw_type` into a single
//  decoder that turns a (hostFwType, .bin bytes) pair into one or more
//  (psp_gfx_fw_type, offset_bytes, size_bytes) payload descriptors,
//  ready to feed to `psp_load_ip_fw`.
//
//  The dext's LoadFirmware path streams an entire .bin file from the
//  host into a DART-mapped DMA buffer. PSP, however, doesn't want the
//  raw file — it wants one or more pre-trimmed payload slices, each
//  tagged with the right psp_gfx_fw_type. For some IPs there is one
//  payload (SMU, SDMA_RS64); for others there are multiple
//  (uni_mes → CP_MES + CP_MES_DATA; rlc.bin → up to ~5 sub-bins
//  depending on version_minor; cp_rs64 → ucode + 2-4 stacks).
//
//  This file mirrors:
//    drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c:851-1122
//        amdgpu_ucode_init_single_fw   (per-IP offset+size derivation)
//    drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c:2634-2840
//        psp_get_fw_type               (AMDGPU_UCODE_ID_* → GFX_FW_TYPE_*)
//    drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c:552-585
//        amdgpu_gfx_rlc_init_microcode (version_minor dispatch)
//    drivers/gpu/drm/amd/amdgpu/amdgpu_mes.c:719-743
//    drivers/gpu/drm/amd/amdgpu/amdgpu_sdma.c:277-302
//    drivers/gpu/drm/amd/amdgpu/imu_v12_0.c:60-75
//
//  Don't simplify. If you find yourself thinking "this RLC step is
//  redundant", check upstream first — it almost certainly isn't.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

// Maximum payloads from a single .bin file:
//   - rlc.bin (v2.4) can emit up to: RLC_G + GPM + SRM + SRM_CNTL +
//     IRAM + DRAM + RLC_P + RLC_V + 5 tap-delay variants = 13
//   - rs64 cp.bin emits up to: ucode + 4 stacks = 5
//   - uni_mes.bin emits 2.
// 16 leaves headroom without inflating LoadFirmware stack frames.
constexpr uint32_t kMaxUcodePayloadsPerFile = 16;

// Resolved location of a single LOAD_IP_FW payload within a .bin file.
struct UcodePayload {
    uint32_t fw_type;       // psp_gfx_fw_type (e.g. GFX_FW_TYPE_RS64_PFP = 87)
    uint32_t offset_bytes;  // byte offset within the .bin file
    uint32_t size_bytes;    // payload size
};

// Decode a single firmware .bin into one-or-more LOAD_IP_FW payloads.
//
// `hostFwType` is the public-API host fw_type passed via LoadFirmware
// (the kFwIP_* / kFwIP_FILE_* constants).
//
// `bin` points at the start of the .bin file (the common_firmware_header).
// `size_bytes` is the total file size.
//
// On success, returns the number of payloads written into `out[]`
// (1..kMaxUcodePayloadsPerFile). Returns 0 if hostFwType is unknown,
// the file is too small, or the header data is internally inconsistent.
// The caller should treat 0 as "fall through to the legacy
// common-header-only path".
//
// IMPORTANT: out[i].offset_bytes is relative to `bin`, not relative to
// the ucode_array region. The caller adds it to whatever GPU bus address
// the .bin starts at to produce fw_phy_addr.
uint32_t amdgpu_ucode_extract(uint64_t hostFwType,
                              const uint8_t *bin,
                              uint64_t size_bytes,
                              UcodePayload out[kMaxUcodePayloadsPerFile]);

} // namespace amdgpu
