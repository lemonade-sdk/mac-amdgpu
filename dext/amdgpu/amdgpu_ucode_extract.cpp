//
//  amdgpu_ucode_extract.cpp — port of upstream `amdgpu_ucode_init_single_fw`
//  and `psp_get_fw_type` for the per-IP firmware files the dext loads
//  during bringup. See amdgpu_ucode_extract.h for the overall design.
//
//  Every case here cites the corresponding upstream file:line so a
//  reviewer can audit the (offset_bytes, size_bytes, fw_type) tuple
//  against AMD's reference driver.
//
//  Constraints baked in:
//    - For psp_v14_0 / GMC v12 / gfx12 / RDNA4 specifically.
//    - We don't support legacy SDMA v1/v2 packaging (not shipped on
//      R9700); RDNA4 always uses sdma_firmware_header_v3_0.
//    - cp_v12_0 (RDNA4) always uses RS64 firmwares; legacy CP_ME etc.
//      paths are not exercised.
//

#include "amdgpu_ucode_extract.h"
#include "amdgpu_ucode_psp.h"
#include "amdgpu_psp.h"

#include <string.h>
#include <stdint.h>

namespace amdgpu {

namespace {

// Lightweight bounds check: confirm `[off, off+sz)` lies entirely
// inside the .bin file. Returns false on overflow or out-of-range.
inline bool within(uint64_t off, uint64_t sz, uint64_t file_size) {
    if (sz == 0) return false;            // empty payloads are skipped upstream
    if (off >= file_size) return false;
    if (off + sz > file_size) return false;
    return true;
}

// Push a single payload onto out[]. Returns the new count, or `count`
// if the bounds check fails (skip-on-zero matches upstream — every
// rlc_v2_x init function skips sub-bins whose size_bytes==0).
inline uint32_t push(UcodePayload out[], uint32_t count,
                     uint32_t fw_type,
                     uint64_t offset, uint64_t size,
                     uint64_t file_size) {
    if (count >= kMaxUcodePayloadsPerFile) return count;
    if (!within(offset, size, file_size)) return count;
    out[count].fw_type      = fw_type;
    out[count].offset_bytes = static_cast<uint32_t>(offset);
    out[count].size_bytes   = static_cast<uint32_t>(size);
    return count + 1;
}

// =====================================================================
// Per-IP extractors. Each takes the raw .bin file and emits one or more
// payloads into out[]. Returns the count.
// =====================================================================

// SMU is handled by the common-header path below (extract_common_header
// with caller-supplied PSPGfxFwType::SMU=18). Upstream amdgpu_ucode.c:1107-1111
// (the default switch branch) is identical to that path; SMU
// (AMDGPU_UCODE_ID_SMC → GFX_FW_TYPE_SMU, amdgpu_psp.c:2743-2744)
// doesn't need an IP-specific helper.

// SDMA on RDNA4 (sdma_v7_1) — packaging is sdma_firmware_header_v3_0,
// fw_type is SDMA_UCODE_TH0 (= 71). Upstream:
//   - amdgpu_sdma.c:291-299 (init: case 3 → AMDGPU_UCODE_ID_SDMA_RS64)
//   - amdgpu_ucode.c:901-905 (load: AMDGPU_UCODE_ID_SDMA_RS64 uses
//     `header.ucode_array_offset_bytes` + `sdmav3_hdr->ucode_size_bytes`)
//   - amdgpu_psp.c:2779-2782 (psp_get_fw_type:
//     AMDGPU_UCODE_ID_SDMA_RS64 → GFX_FW_TYPE_SDMA_UCODE_TH0 = 71)
uint32_t extract_sdma(const uint8_t *bin, uint64_t size,
                      UcodePayload out[]) {
    if (size < sizeof(sdma_firmware_header_v3_0)) return 0;
    auto *hdr = reinterpret_cast<const sdma_firmware_header_v3_0 *>(bin);
    // Only v3.0 is supported on RDNA4. Reject anything else; caller
    // will fall back to common-header path.
    if (hdr->header.header_version_major != 3) return 0;
    return push(out, 0, PSPGfxFwType::SDMA_UCODE_TH0,
                hdr->header.ucode_array_offset_bytes,
                hdr->ucode_size_bytes, size);
}

// RLC. Single .bin file emits up to ~13 sub-firmwares depending on
// header_version_minor. Upstream dispatch:
//   amdgpu_rlc.c:552-585 amdgpu_gfx_rlc_init_microcode
// Each minor-specific helper (v2_1 / v2_2 / v2_3 / v2_4 / v2_5) emits
// the sub-bins whose size_bytes is non-zero. We mirror that exactly.
//
// Order (matters: matches upstream's array order, which mirrors the
// firmware.ucode[] index order in amdgpu_ucode.h:478-553):
//   v2_0 (always)  → RLC_G
//   v2_1 (>= 1)    → CNTL, GPM_MEM, SRM_MEM
//   v2_2 (>= 2)    → IRAM, DRAM
//   v2_3 (== 3)    → RLC_P, RLC_V
//   v2_4 (== 4)    → GLOBAL/SE0/SE1/SE2/SE3 tap delays
//   v2_5 (== 5)    → IRAM_1, DRAM_1 (not shipped on RDNA4)
//
// Offset fields are file-relative when computed as
//   (struct-pointer + offset_bytes) - bin
// which simplifies to plain offset_bytes since the struct lives at the
// start of bin. Upstream's `(u8 *)rlc_hdr + offset_bytes` is the same.
uint32_t extract_rlc(const uint8_t *bin, uint64_t size,
                     UcodePayload out[]) {
    if (size < sizeof(rlc_firmware_header_v2_0)) return 0;
    auto *hv2_0 = reinterpret_cast<const rlc_firmware_header_v2_0 *>(bin);
    uint16_t maj = hv2_0->header.header_version_major;
    uint16_t mnr = hv2_0->header.header_version_minor;
    if (maj != 2) return 0;

    uint32_t n = 0;

    // ---- v2.0 — RLC_G is the main RLC ucode -------------------------
    // amdgpu_rlc.c:331-340 (sets info->ucode_id = AMDGPU_UCODE_ID_RLC_G)
    // amdgpu_ucode.c:1107-1111 (default case: header.ucode_array_offset
    //                            + header.ucode_size_bytes)
    // amdgpu_psp.c:2704-2706 (AMDGPU_UCODE_ID_RLC_G → GFX_FW_TYPE_RLC_G=8)
    n = push(out, n, PSPGfxFwType::RLC_G,
             hv2_0->header.ucode_array_offset_bytes,
             hv2_0->header.ucode_size_bytes, size);

    // ---- v2.1 — save/restore lists ----------------------------------
    if (mnr >= 1 && size >= sizeof(rlc_firmware_header_v2_1)) {
        auto *hv2_1 = reinterpret_cast<const rlc_firmware_header_v2_1 *>(bin);
        // amdgpu_rlc.c:366-389
        // amdgpu_ucode.c:920-931 (RLC_RESTORE_LIST_{CNTL,GPM_MEM,SRM_MEM})
        // amdgpu_psp.c:2707-2715
        n = push(out, n, PSPGfxFwType::RLC_RESTORE_LIST_SRM_CNTL,
                 hv2_1->save_restore_list_cntl_offset_bytes,
                 hv2_1->save_restore_list_cntl_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_RESTORE_LIST_GPM_MEM,
                 hv2_1->save_restore_list_gpm_offset_bytes,
                 hv2_1->save_restore_list_gpm_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_RESTORE_LIST_SRM_MEM,
                 hv2_1->save_restore_list_srm_offset_bytes,
                 hv2_1->save_restore_list_srm_size_bytes, size);
    }

    // ---- v2.2 — IRAM + DRAM -----------------------------------------
    if (mnr >= 2 && size >= sizeof(rlc_firmware_header_v2_2)) {
        auto *hv2_2 = reinterpret_cast<const rlc_firmware_header_v2_2 *>(bin);
        // amdgpu_rlc.c:404-420
        // amdgpu_ucode.c:932-939 (RLC_IRAM / RLC_DRAM)
        // amdgpu_psp.c:2716-2721
        n = push(out, n, PSPGfxFwType::RLC_IRAM,
                 hv2_2->rlc_iram_ucode_offset_bytes,
                 hv2_2->rlc_iram_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_DRAM_BOOT,
                 hv2_2->rlc_dram_ucode_offset_bytes,
                 hv2_2->rlc_dram_ucode_size_bytes, size);
    }

    // ---- v2.3 — RLC_P + RLC_V ---------------------------------------
    if (mnr == 3 && size >= sizeof(rlc_firmware_header_v2_3)) {
        auto *hv2_3 = reinterpret_cast<const rlc_firmware_header_v2_3 *>(bin);
        // amdgpu_rlc.c:439-455
        // amdgpu_ucode.c:948-955 (RLC_P / RLC_V)
        // amdgpu_psp.c:2698-2703
        n = push(out, n, PSPGfxFwType::RLC_P,
                 hv2_3->rlcp_ucode_offset_bytes,
                 hv2_3->rlcp_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::RLC_V,
                 hv2_3->rlcv_ucode_offset_bytes,
                 hv2_3->rlcv_ucode_size_bytes, size);
    }

    // ---- v2.4 — tap delays ------------------------------------------
    if (mnr == 4 && size >= sizeof(rlc_firmware_header_v2_4)) {
        auto *hv2_4 = reinterpret_cast<const rlc_firmware_header_v2_4 *>(bin);
        // amdgpu_rlc.c:475-515
        // amdgpu_ucode.c:956-975
        // amdgpu_psp.c:2728-2741
        n = push(out, n, PSPGfxFwType::GLOBAL_TAP_DELAYS,
                 hv2_4->global_tap_delays_ucode_offset_bytes,
                 hv2_4->global_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE0_TAP_DELAYS,
                 hv2_4->se0_tap_delays_ucode_offset_bytes,
                 hv2_4->se0_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE1_TAP_DELAYS,
                 hv2_4->se1_tap_delays_ucode_offset_bytes,
                 hv2_4->se1_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE2_TAP_DELAYS,
                 hv2_4->se2_tap_delays_ucode_offset_bytes,
                 hv2_4->se2_tap_delays_ucode_size_bytes, size);
        n = push(out, n, PSPGfxFwType::SE3_TAP_DELAYS,
                 hv2_4->se3_tap_delays_ucode_offset_bytes,
                 hv2_4->se3_tap_delays_ucode_size_bytes, size);
    }

    return n;
}

// IMU (one .bin → two LOAD_IP_FW frames: IMU_I + IMU_D).
// Upstream:
//   imu_v12_0.c:60-75   (sets IMU_I and IMU_D ucode_ids)
//   amdgpu_ucode.c:1021-1031 (IMU_I uses header.ucode_array_offset_bytes
//                              + imu_hdr->imu_iram_ucode_size_bytes;
//                              IMU_D uses header.ucode_array_offset_bytes
//                              + imu_iram_ucode_size_bytes (i.e. starts
//                              right after iram) + imu_dram_ucode_size_bytes)
//   amdgpu_psp.c:2786-2791 (IMU_I → GFX_FW_TYPE_IMU_I=68;
//                            IMU_D → GFX_FW_TYPE_IMU_D=69)
uint32_t extract_imu(const uint8_t *bin, uint64_t size,
                     UcodePayload out[]) {
    if (size < sizeof(imu_firmware_header_v1_0)) return 0;
    auto *hdr = reinterpret_cast<const imu_firmware_header_v1_0 *>(bin);
    if (hdr->header.header_version_major != 1) return 0;
    uint32_t n = 0;
    n = push(out, n, PSPGfxFwType::IMU_I,
             hdr->header.ucode_array_offset_bytes,
             hdr->imu_iram_ucode_size_bytes, size);
    // IMU_D starts immediately after IMU_I in the same file — there is
    // no independent dram offset field. Mirror amdgpu_ucode.c:1027-1030.
    n = push(out, n, PSPGfxFwType::IMU_D,
             static_cast<uint64_t>(hdr->header.ucode_array_offset_bytes) +
                 hdr->imu_iram_ucode_size_bytes,
             hdr->imu_dram_ucode_size_bytes, size);
    return n;
}

// RS64 CP firmware (PFP / ME / MEC). Single .bin → ucode + per-pipe
// stack copies. Upstream:
//   gfx_v12_0.c:605-642 (init_microcode: PFP+P0_STACK, ME+P0_STACK,
//                         MEC+P0_STACK+P1_STACK)
//   amdgpu_ucode.c:1032-1085 (init_single_fw for RS64 ucode + stacks)
//   amdgpu_psp.c:2792-2823 (psp_get_fw_type RS64_PFP/ME/MEC/_Px_STACK)
//
// On RDNA4 + GC12.0.x:
//   PFP file emits: RS64_PFP (87) + RS64_PFP_P0_STACK (90) + RS64_PFP_P1_STACK (91)
//   ME  file emits: RS64_ME  (88) + RS64_ME_P0_STACK  (92) + RS64_ME_P1_STACK  (93)
//   MEC file emits: RS64_MEC (89) + RS64_MEC_P0_STACK (94) + RS64_MEC_P1_STACK (95)
//                                 + RS64_MEC_P2_STACK (96) + RS64_MEC_P3_STACK (97)
//
// The "stack" payloads all point at the same (data_offset_bytes,
// data_size_bytes) — upstream re-uses the same bytes per pipe, only
// the fw_type changes (so PSP routes the same data into different
// per-pipe scratch regions). See amdgpu_ucode.c:1037-1045 etc. —
// every CP_RS64_*_STACK case has identical (data_offset, data_size).

enum class RS64Family { PFP, ME, MEC };

uint32_t extract_cp_rs64(const uint8_t *bin, uint64_t size,
                         RS64Family family,
                         UcodePayload out[]) {
    if (size < sizeof(gfx_firmware_header_v2_0)) return 0;
    auto *hdr = reinterpret_cast<const gfx_firmware_header_v2_0 *>(bin);
    if (hdr->header.header_version_major != 2) return 0;

    uint32_t n = 0;
    uint32_t ucode_fw_type = 0;
    uint32_t stack_fw_types[4] = {0,0,0,0};
    uint32_t num_stacks = 0;

    switch (family) {
    case RS64Family::PFP:
        ucode_fw_type   = PSPGfxFwType::RS64_PFP;
        stack_fw_types[0] = PSPGfxFwType::RS64_PFP_P0;
        stack_fw_types[1] = PSPGfxFwType::RS64_PFP_P1;
        num_stacks = 2;
        break;
    case RS64Family::ME:
        ucode_fw_type   = PSPGfxFwType::RS64_ME;
        stack_fw_types[0] = PSPGfxFwType::RS64_ME_P0;
        stack_fw_types[1] = PSPGfxFwType::RS64_ME_P1;
        num_stacks = 2;
        break;
    case RS64Family::MEC:
        ucode_fw_type   = PSPGfxFwType::RS64_MEC;
        stack_fw_types[0] = PSPGfxFwType::RS64_MEC_P0;
        stack_fw_types[1] = PSPGfxFwType::RS64_MEC_P1;
        stack_fw_types[2] = PSPGfxFwType::RS64_MEC_P2;
        stack_fw_types[3] = PSPGfxFwType::RS64_MEC_P3;
        num_stacks = 4;
        break;
    }

    // ucode portion — amdgpu_ucode.c:1032-1036, 1047-1051, 1062-1066:
    //   ucode_size = cpv2_hdr->ucode_size_bytes
    //   addr       = fw->data + header.ucode_array_offset_bytes
    n = push(out, n, ucode_fw_type,
             hdr->header.ucode_array_offset_bytes,
             hdr->ucode_size_bytes, size);

    // Stack portions — amdgpu_ucode.c:1037-1086:
    //   data_size = cpv2_hdr->data_size_bytes
    //   addr      = fw->data + cpv2_hdr->data_offset_bytes
    // identical (offset, size) repeated with different fw_types.
    for (uint32_t i = 0; i < num_stacks; i++) {
        n = push(out, n, stack_fw_types[i],
                 hdr->data_offset_bytes,
                 hdr->data_size_bytes, size);
    }
    return n;
}

// uni_mes / mes packaging. Single .bin → CP_MES (ucode) + CP_MES_DATA
// (data). Upstream:
//   amdgpu_mes.c:719-743 (init: ucode + ucode_data)
//   amdgpu_ucode.c:976-995 (init_single_fw: mes_ucode_size_bytes /
//                            mes_ucode_offset_bytes for ucode;
//                            mes_ucode_data_size_bytes /
//                            mes_ucode_data_offset_bytes for data)
//   amdgpu_psp.c:2665-2676 (CP_MES → 33, CP_MES_DATA / MES_STACK → 34)
//
// NOTE: PSP wants CP_MES (=33) for the sched pipe (amdgpu_mes_init_microcode
// case AMDGPU_MES_SCHED_PIPE → AMDGPU_UCODE_ID_CP_MES → GFX_FW_TYPE_CP_MES).
// CP_MES1 (=81) is for KIQ pipe; we don't expose KIQ on R9700 yet, so
// the uni_mes file we ship is sched-pipe only.
uint32_t extract_mes(const uint8_t *bin, uint64_t size,
                     UcodePayload out[]) {
    if (size < sizeof(mes_firmware_header_v1_0)) return 0;
    auto *hdr = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin);
    if (hdr->header.header_version_major != 1) return 0;
    uint32_t n = 0;
    n = push(out, n, PSPGfxFwType::CP_MES,
             hdr->mes_ucode_offset_bytes,
             hdr->mes_ucode_size_bytes, size);
    n = push(out, n, PSPGfxFwType::CP_MES_DATA,
             hdr->mes_ucode_data_offset_bytes,
             hdr->mes_ucode_data_size_bytes, size);
    return n;
}

// =====================================================================
// Single-payload fallback for backward-compatible 0x100+psp_fw_type
// callers (SMU, single-type IMU_I/IMU_D, etc.). Just uses the common
// header. Matches the default branch in amdgpu_ucode.c:1107-1111.
// =====================================================================
uint32_t extract_common_header(uint32_t psp_fw_type,
                               const uint8_t *bin, uint64_t size,
                               UcodePayload out[]) {
    if (size < sizeof(common_firmware_header)) return 0;
    auto *hdr = reinterpret_cast<const common_firmware_header *>(bin);
    return push(out, 0, psp_fw_type,
                hdr->ucode_array_offset_bytes,
                hdr->ucode_size_bytes, size);
}

} // anonymous namespace

// =====================================================================
// Public entrypoint.
// =====================================================================

// Host fwType encoding:
//   0x000..0x0FF   pre-SOS bootloader components (handled elsewhere)
//   0x100..0x1FF   single-payload IP firmware (psp_fw_type = hostFwType-0x100)
//                  legacy / backward-compatible; used by SMU and any IP
//                  whose .bin is a single payload at
//                  (common_header.ucode_array_offset_bytes, ucode_size_bytes)
//   0x200..0x2FF   multi-payload .bin files (per-file extractors below)
//                  the dispatcher emits N LOAD_IP_FW frames per host call
//
// 0x200+ host fw_type IDs (file-typed):
constexpr uint64_t kHostFwFile_SDMA    = 0x200 + 0;  // sdma_<v>.bin (v3.0)
constexpr uint64_t kHostFwFile_RLC     = 0x200 + 1;  // gc_<v>_rlc.bin (v2.x)
constexpr uint64_t kHostFwFile_IMU     = 0x200 + 2;  // gc_<v>_imu.bin
constexpr uint64_t kHostFwFile_MES_UNI = 0x200 + 3;  // gc_<v>_uni_mes.bin
constexpr uint64_t kHostFwFile_CP_PFP  = 0x200 + 4;  // gc_<v>_pfp.bin (RS64)
constexpr uint64_t kHostFwFile_CP_ME   = 0x200 + 5;  // gc_<v>_me.bin  (RS64)
constexpr uint64_t kHostFwFile_CP_MEC  = 0x200 + 6;  // gc_<v>_mec.bin (RS64)

uint32_t amdgpu_ucode_extract(uint64_t hostFwType,
                              const uint8_t *bin, uint64_t size_bytes,
                              UcodePayload out[kMaxUcodePayloadsPerFile]) {
    if (bin == nullptr || size_bytes == 0) return 0;
    memset(out, 0, sizeof(UcodePayload) * kMaxUcodePayloadsPerFile);

    // ---- Multi-payload per-file extractors --------------------------
    switch (hostFwType) {
    case kHostFwFile_SDMA:    return extract_sdma(bin, size_bytes, out);
    case kHostFwFile_RLC:     return extract_rlc(bin, size_bytes, out);
    case kHostFwFile_IMU:     return extract_imu(bin, size_bytes, out);
    case kHostFwFile_MES_UNI: return extract_mes(bin, size_bytes, out);
    case kHostFwFile_CP_PFP:
        return extract_cp_rs64(bin, size_bytes, RS64Family::PFP, out);
    case kHostFwFile_CP_ME:
        return extract_cp_rs64(bin, size_bytes, RS64Family::ME, out);
    case kHostFwFile_CP_MEC:
        return extract_cp_rs64(bin, size_bytes, RS64Family::MEC, out);
    default:
        break;
    }

    // ---- Legacy 0x100+psp_fw_type single-payload path ---------------
    //
    // Special-cased so callers that already work (SMU, plus anyone who
    // wants a coarse one-shot) don't have to migrate. Specific psp
    // fw_types that are part of a multi-payload .bin (e.g. RS64_PFP,
    // CP_MES, RLC_G) are also acceptable here when the host explicitly
    // routes to a single sub-bin — but typically you want the 0x200+
    // file-typed path instead.
    if (hostFwType >= 0x100 && hostFwType < 0x200) {
        uint32_t psp_fw_type = static_cast<uint32_t>(hostFwType - 0x100);

        // SDMA via 0x100+71 should ideally use the v3.0 extractor too —
        // emit through extract_sdma so the offset/size matches a v3.0
        // .bin's `header.ucode_array_offset_bytes` + `ucode_size_bytes`,
        // not the common-header default. This keeps the existing host
        // path (kFwIP_SDMA_TH0 = 0x100+71) working with correct fields.
        if (psp_fw_type == PSPGfxFwType::SDMA_UCODE_TH0) {
            return extract_sdma(bin, size_bytes, out);
        }

        // CP_MES / CP_MES_DATA via legacy 0x100+33/34: the host has loaded
        // a full uni_mes.bin file and is asking for only one half. Emit
        // through extract_mes and filter, same as IMU below. This is
        // required because the mes_firmware_header_v1_0 fields
        // (mes_ucode_offset_bytes etc.) differ from the common header's
        // ucode_array_offset_bytes — using the latter would feed PSP the
        // wrong slice of the file.
        if (psp_fw_type == PSPGfxFwType::CP_MES ||
            psp_fw_type == PSPGfxFwType::CP_MES_DATA) {
            UcodePayload all[kMaxUcodePayloadsPerFile] = {};
            uint32_t total = extract_mes(bin, size_bytes, all);
            uint32_t n = 0;
            for (uint32_t i = 0; i < total; i++) {
                if (all[i].fw_type == psp_fw_type) {
                    out[n++] = all[i];
                }
            }
            return n;
        }

        // CP RS64 family via legacy 0x100+8x: similar reasoning — the
        // RS64 ucode lives at (header.ucode_array_offset_bytes,
        // cpv2_hdr->ucode_size_bytes); stack payloads at
        // (data_offset_bytes, data_size_bytes). Common-header trim
        // would be wrong for the stack types.
        if (psp_fw_type >= PSPGfxFwType::RS64_PFP &&
            psp_fw_type <= PSPGfxFwType::RS64_MEC_P3) {
            // Determine family from the requested sub-type.
            RS64Family fam;
            if (psp_fw_type == PSPGfxFwType::RS64_PFP ||
                psp_fw_type == PSPGfxFwType::RS64_PFP_P0 ||
                psp_fw_type == PSPGfxFwType::RS64_PFP_P1) {
                fam = RS64Family::PFP;
            } else if (psp_fw_type == PSPGfxFwType::RS64_ME ||
                       psp_fw_type == PSPGfxFwType::RS64_ME_P0 ||
                       psp_fw_type == PSPGfxFwType::RS64_ME_P1) {
                fam = RS64Family::ME;
            } else {
                fam = RS64Family::MEC;
            }
            UcodePayload all[kMaxUcodePayloadsPerFile] = {};
            uint32_t total = extract_cp_rs64(bin, size_bytes, fam, all);
            uint32_t n = 0;
            for (uint32_t i = 0; i < total; i++) {
                if (all[i].fw_type == psp_fw_type) {
                    out[n++] = all[i];
                }
            }
            return n;
        }

        // Same for IMU_I / IMU_D shipped individually — fall through to
        // the multi-payload IMU extractor and let the caller pick which
        // payload to submit. But callers that hand-pick a sub-bin
        // typically pass a single fw_type, so just emit the matching
        // one. We compute both then filter.
        if (psp_fw_type == PSPGfxFwType::IMU_I ||
            psp_fw_type == PSPGfxFwType::IMU_D) {
            UcodePayload all[kMaxUcodePayloadsPerFile] = {};
            uint32_t total = extract_imu(bin, size_bytes, all);
            uint32_t n = 0;
            for (uint32_t i = 0; i < total; i++) {
                if (all[i].fw_type == psp_fw_type) {
                    out[n++] = all[i];
                }
            }
            return n;
        }

        // Default: common-header trim with the caller-supplied fw_type.
        return extract_common_header(psp_fw_type, bin, size_bytes, out);
    }

    return 0;
}

} // namespace amdgpu
