//
//  amdgpu_ucode_psp.h — vendored verbatim from upstream amdgpu_ucode.h
//
//  AMD firmware ".bin" files have a common header followed by version-
//  specific sub-firmware descriptors. This file mirrors the relevant
//  structs and enums 1:1 from upstream so the parser can be a direct
//  port of `psp_init_sos_microcode` in amdgpu_psp.c.
//
//  Sources (Linux 6.x):
//    drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h:30-148
//    drivers/gpu/drm/amd/amdgpu/amdgpu_psp.h:93-107  (bootloader cmds)
//
//  IMPORTANT: do not "simplify" these — see [[mac-amdgpu-port-full-
//  parsers-not-shortcuts]]. Every field exists for a reason.
//

#pragma once

#include <stdint.h>

namespace amdgpu {

// ---- Common firmware header --------------------------------------------

struct common_firmware_header {
    uint32_t size_bytes;             // total file size in bytes
    uint32_t header_size_bytes;      // header struct size
    uint16_t header_version_major;   // 1 or 2
    uint16_t header_version_minor;
    uint16_t ip_version_major;       // e.g. 14 for psp_v14
    uint16_t ip_version_minor;
    uint32_t ucode_version;
    uint32_t ucode_size_bytes;       // size of payload region in bytes
    uint32_t ucode_array_offset_bytes;  // payload offset from header start
    uint32_t crc32;
} __attribute__((packed));
static_assert(sizeof(common_firmware_header) == 32,
              "common_firmware_header must be 32 bytes");

// ---- PSP v1 legacy descriptors (3 fields: version, offset, size) -------
//
// Used by older PSP firmware versions (v1.0 through v1.3). The sub-bins
// are named directly in the struct (sos, kdb, toc, etc.).

struct psp_fw_legacy_bin_desc {
    uint32_t fw_version;
    uint32_t offset_bytes;
    uint32_t size_bytes;
} __attribute__((packed));
static_assert(sizeof(psp_fw_legacy_bin_desc) == 12,
              "psp_fw_legacy_bin_desc must be 12 bytes");

// version_major=1, version_minor=0
struct psp_firmware_header_v1_0 {
    common_firmware_header     header;
    psp_fw_legacy_bin_desc     sos;
} __attribute__((packed));

// version_major=1, version_minor=1
struct psp_firmware_header_v1_1 {
    psp_firmware_header_v1_0   v1_0;
    psp_fw_legacy_bin_desc     toc;
    psp_fw_legacy_bin_desc     kdb;
} __attribute__((packed));

// version_major=1, version_minor=2
struct psp_firmware_header_v1_2 {
    psp_firmware_header_v1_0   v1_0;
    psp_fw_legacy_bin_desc     res;
    psp_fw_legacy_bin_desc     kdb;
} __attribute__((packed));

// version_major=1, version_minor=3
struct psp_firmware_header_v1_3 {
    psp_firmware_header_v1_1   v1_1;
    psp_fw_legacy_bin_desc     spl;
    psp_fw_legacy_bin_desc     rl;
    psp_fw_legacy_bin_desc     sys_drv_aux;
    psp_fw_legacy_bin_desc     sos_aux;
} __attribute__((packed));

// ---- PSP v2 typed descriptors (fw_type-tagged, flexible array) ---------
//
// Used by psp_v13+ (so RDNA3 onward, including RDNA4). Sub-bins are
// identified by fw_type enum, not by struct field name.

struct psp_fw_bin_desc {
    uint32_t fw_type;        // psp_fw_type enum
    uint32_t fw_version;
    uint32_t offset_bytes;   // relative to ucode_array_offset_bytes
    uint32_t size_bytes;
} __attribute__((packed));
static_assert(sizeof(psp_fw_bin_desc) == 16,
              "psp_fw_bin_desc must be 16 bytes");

enum psp_fw_type {
    PSP_FW_TYPE_UNKOWN              = 0,
    PSP_FW_TYPE_PSP_SOS             = 1,
    PSP_FW_TYPE_PSP_SYS_DRV         = 2,
    PSP_FW_TYPE_PSP_KDB             = 3,
    PSP_FW_TYPE_PSP_TOC             = 4,
    PSP_FW_TYPE_PSP_SPL             = 5,
    PSP_FW_TYPE_PSP_RL              = 6,
    PSP_FW_TYPE_PSP_SOC_DRV         = 7,
    PSP_FW_TYPE_PSP_INTF_DRV        = 8,
    PSP_FW_TYPE_PSP_DBG_DRV         = 9,
    PSP_FW_TYPE_PSP_RAS_DRV         = 10,
    PSP_FW_TYPE_PSP_IPKEYMGR_DRV    = 11,
    PSP_FW_TYPE_PSP_SPDM_DRV        = 12,
    PSP_FW_TYPE_MAX_INDEX,
};

// version_major=2, version_minor=0
struct psp_firmware_header_v2_0 {
    common_firmware_header     header;
    uint32_t                   psp_fw_bin_count;
    psp_fw_bin_desc            psp_fw_bin[];  // flexible array
} __attribute__((packed));

// version_major=2, version_minor=1
struct psp_firmware_header_v2_1 {
    common_firmware_header     header;
    uint32_t                   psp_fw_bin_count;
    uint32_t                   psp_aux_fw_bin_index;
    psp_fw_bin_desc            psp_fw_bin[];
} __attribute__((packed));

// ---- Bootloader command codes ------------------------------------------
//
// Written to MP0_C2PMSG_35 to tell the PSP bootloader which sub-firmware
// to load (after the host has written the buffer's bus addr >> 20 to
// MP0_C2PMSG_36).

enum psp_bootloader_cmd_e {
    PSP_BL__LOAD_SYSDRV         = 0x10000,
    PSP_BL__LOAD_SOSDRV         = 0x20000,
    PSP_BL__LOAD_KEY_DATABASE   = 0x80000,
    PSP_BL__LOAD_SOCDRV         = 0xB0000,
    PSP_BL__LOAD_DBGDRV         = 0xC0000,
    PSP_BL__LOAD_HADDRV         = 0xC0000,  // alias on psp_v14
    PSP_BL__LOAD_INTFDRV        = 0xD0000,
    PSP_BL__LOAD_RASDRV         = 0xE0000,
    PSP_BL__LOAD_IPKEYMGRDRV    = 0xF0000,
    PSP_BL__DRAM_LONG_TRAIN     = 0x100000,
    PSP_BL__DRAM_SHORT_TRAIN    = 0x200000,
    PSP_BL__LOAD_TOS_SPL_TABLE  = 0x10000000,
    PSP_BL__LOAD_SPDMDRV        = 0x20000000,
};

} // namespace amdgpu
