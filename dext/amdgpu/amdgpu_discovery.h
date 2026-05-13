//
//  amdgpu_discovery.h — Port of upstream IP discovery binary parser.
//
//  Source structs: upstream/linux/drivers/gpu/drm/amd/include/discovery.h
//  Parser logic:   upstream/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
//
//  The discovery binary lives in a Trust-Memory-Region (TMR) at the
//  top of VRAM, and on RDNA3+ also accessible via DRIVER_SCRATCH_0/1/2
//  registers. It's a packed structure with:
//
//      binary_header (16 bytes + 6 × 8 bytes of table_info entries)
//        - signature 0x28211407, version_major/minor, checksum, size
//        - 6 table_info[] entries pointing to:
//            [0] IP Discovery (signature "IPDS")
//            [1] GC info
//            [2] Harvest info
//            [3] VCN info
//            [4] MALL info
//            [5] NPS info (v4+)
//
//      ip_discovery_header at table[0].offset
//        - signature 0x53445049 ("IPDS"), version, size
//        - die_info[num_dies] each pointing to a die_header
//        - in v4 only, a base_addr_64_bit flag
//
//      For each die:
//        die_header { die_id, num_ips }
//        followed by num_ips × ip_v4 entries:
//            ip_v4 { hw_id, instance, num_base_addr, major, minor,
//                    revision, sub_revision:4, variant:4,
//                    base_address[num_base_addr] }
//
//  The HW IDs we care about (from soc15_hw_ip.h):
//      MP0  = 255   (PSP)
//      MP1  =   1   (SMU/PMFW)
//      GC   =  11   (graphics + compute)
//      GMC/MMHUB = 34
//      HDP  =  41
//      OSSSYS = 40 (interrupt handler)
//      SDMA0 =  42
//      SDMA1 =  43
//      NBIF = 108   (PCIe interface)
//

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"   // DeviceContext + IPBlock + IPBaseTable

namespace amdgpu {

#pragma pack(push, 1)

struct DiscoveryTableInfo {
    uint16_t offset;    // byte offset within binary
    uint16_t checksum;
    uint16_t size;
    uint16_t padding;
};

struct DiscoveryBinaryHeader {
    uint32_t signature;       // 0x28211407
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t binary_checksum;
    uint16_t binary_size;
    DiscoveryTableInfo table_list[6];
};

struct DiscoveryDieInfo {
    uint16_t die_id;
    uint16_t die_offset;
};

struct DiscoveryIPDSHeader {
    uint32_t signature;        // 0x53445049 'IPDS'
    uint16_t version;
    uint16_t size;
    uint32_t id;
    uint16_t num_dies;
    DiscoveryDieInfo die_info[16];
    // For version == 4 there's an extra base_addr_64_bit flag byte;
    // we handle that in the parser.
};

struct DiscoveryDieHeader {
    uint16_t die_id;
    uint16_t num_ips;
};

// ip_v4 — used by RDNA3+ including gfx1201.
struct DiscoveryIPv4 {
    uint16_t hw_id;
    uint8_t  instance_number;
    uint8_t  num_base_address;
    uint8_t  major;
    uint8_t  minor;
    uint8_t  revision;
    uint8_t  variant_subrev;     // LE: sub_revision:4 | variant:4
    // followed by base_address[num_base_address] of u32 (or u64 if header.base_addr_64_bit)
};

#pragma pack(pop)

constexpr uint32_t kDiscoverySignature       = 0x28211407u;
constexpr uint32_t kDiscoveryIPDSSignature   = 0x53445049u;  // 'IPDS'

// HW IDs of interest — values from soc15_hw_ip.h.
namespace HWID {
    constexpr uint16_t MP1     = 1;
    constexpr uint16_t GC      = 11;
    constexpr uint16_t MMHUB   = 34;   // GMC's hub
    constexpr uint16_t OSSSYS  = 40;
    constexpr uint16_t HDP     = 41;
    constexpr uint16_t SDMA0   = 42;
    constexpr uint16_t SDMA1   = 43;
    constexpr uint16_t NBIF    = 108;
    constexpr uint16_t MP0     = 255;
}

// Result of a discovery parse — what we hand back from the parser.
struct DiscoveryParseResult {
    bool     ok;
    char     err[128];          // populated on failure
    uint8_t  ip_version_major;
    uint8_t  ip_version_minor;
    uint8_t  ip_version_rev;
    uint16_t num_dies;
    uint32_t num_ips_total;
    // The actual IPBaseTable lives in DeviceContext; the parser
    // writes into it directly.
};

//
// discovery_parse — parse a captured discovery binary and populate
// dev.ip with base addresses for every HW block we recognise. Caller
// owns the binary memory; we don't keep references after return.
//
// Returns kIOReturnSuccess on success, with `outResult` filled in.
// On error returns the appropriate kIOReturn* code and writes a
// human-readable explanation into outResult->err.
//
kern_return_t discovery_parse(const uint8_t *binary, uint64_t binarySize,
                              DeviceContext &dev,
                              DiscoveryParseResult *outResult);

} // namespace amdgpu
