//
//  amdgpu_discovery.cpp — IP discovery binary parser.
//
//  Port of upstream/linux/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c
//  reader portion (amdgpu_discovery_init through amdgpu_discovery_*
//  table parsers). We collapse what Linux distributes across
//  multiple functions for clarity here; the contract is:
//
//      validate header signature → walk table_list[0] (IPDS) → for
//      each die, walk ips → for the IPs we care about, copy the
//      first base_address into dev.ip.set(IPBlock::..., base).
//
//  Cross-validated against the gfx1151 dump in
//  docs/reference/gfx1151_discovery.bin (Strix Halo APU); the format
//  is shared with gfx1201 (R9700) per the upstream code paths
//  exercising both.
//

#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include "amdgpu_discovery.h"

#define DISC_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "mac.amdgpu.disc: " fmt, ##__VA_ARGS__)

namespace amdgpu {

namespace {

inline void fail(DiscoveryParseResult *r, const char *msg) {
    if (r != nullptr) {
        r->ok = false;
        snprintf(r->err, sizeof(r->err), "%s", msg);
    }
    DISC_LOG("parse error: %{public}s", msg);
}

uint16_t compute_checksum(const uint8_t *p, uint32_t size) {
    uint16_t sum = 0;
    for (uint32_t i = 0; i < size; i++) sum += p[i];
    return sum;
}

// Map an HW_ID from the binary to our IPBlock enum. Returns IPBlock::Count
// if we don't track this HW block.
IPBlock hwid_to_block(uint16_t hwid) {
    switch (hwid) {
    case HWID::MP0:    return IPBlock::MP0;
    case HWID::MP1:    return IPBlock::MP1;
    case HWID::GC:     return IPBlock::GC;
    case HWID::MMHUB:  return IPBlock::GMC;
    case HWID::HDP:    return IPBlock::HDP;
    case HWID::OSSSYS: return IPBlock::OSSSYS;
    case HWID::SDMA0:  return IPBlock::SDMA0;
    case HWID::SDMA1:  return IPBlock::SDMA1;
    case HWID::NBIF:   return IPBlock::NBIO;
    default:           return IPBlock::Count;
    }
}

const char *block_name(IPBlock b) {
    switch (b) {
    case IPBlock::MP0:    return "MP0";
    case IPBlock::MP1:    return "MP1";
    case IPBlock::GC:     return "GC";
    case IPBlock::GMC:    return "GMC";
    case IPBlock::HDP:    return "HDP";
    case IPBlock::OSSSYS: return "OSSSYS";
    case IPBlock::SDMA0:  return "SDMA0";
    case IPBlock::SDMA1:  return "SDMA1";
    case IPBlock::NBIO:   return "NBIO";
    default:              return "?";
    }
}

} // anon namespace

kern_return_t
discovery_parse(const uint8_t *binary, uint64_t binarySize,
                DeviceContext &dev,
                DiscoveryParseResult *outResult)
{
    if (outResult) {
        memset(outResult, 0, sizeof(*outResult));
        outResult->ok = false;
    }
    if (binary == nullptr || binarySize < sizeof(DiscoveryBinaryHeader)) {
        fail(outResult, "binary too small for header");
        return kIOReturnBadArgument;
    }

    auto *hdr = reinterpret_cast<const DiscoveryBinaryHeader *>(binary);
    if (hdr->signature != kDiscoverySignature) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "bad binary signature %#010x (expected %#010x)",
                 hdr->signature, kDiscoverySignature);
        fail(outResult, msg);
        return kIOReturnInvalid;
    }
    if (hdr->binary_size > binarySize) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "header says binary_size=%u but only %llu bytes available",
                 hdr->binary_size, binarySize);
        fail(outResult, msg);
        return kIOReturnInvalid;
    }

    // Validate top-level checksum (sum of bytes after the checksum field).
    // checksum lives at offset 0x08 and covers bytes [0x0C..binary_size).
    const uint32_t kHeaderChecksumStart = 12;
    if (hdr->binary_size > kHeaderChecksumStart) {
        uint16_t sum = compute_checksum(binary + kHeaderChecksumStart,
                                        hdr->binary_size - kHeaderChecksumStart);
        if (sum != hdr->binary_checksum) {
            DISC_LOG("warning: binary checksum mismatch (computed %#06x, "
                     "header says %#06x) — continuing anyway",
                     sum, hdr->binary_checksum);
            // Linux also warns but doesn't bail.
        }
    }

    // Table[0] is the IP discovery section.
    const DiscoveryTableInfo &ipds_info = hdr->table_list[0];
    if (ipds_info.offset == 0 || ipds_info.offset + sizeof(DiscoveryIPDSHeader) > binarySize) {
        fail(outResult, "IPDS table offset out of range");
        return kIOReturnInvalid;
    }
    auto *ipds = reinterpret_cast<const DiscoveryIPDSHeader *>(
        binary + ipds_info.offset);
    if (ipds->signature != kDiscoveryIPDSSignature) {
        char msg[128];
        snprintf(msg, sizeof(msg), "bad IPDS signature %#010x", ipds->signature);
        fail(outResult, msg);
        return kIOReturnInvalid;
    }

    // v4 binaries include an extra byte indicating 64-bit base addresses.
    bool base_addr_64 = false;
    if (ipds->version >= 4) {
        // After die_info[16], v4 has a packed { uint8 base_addr_64_bit:1 } byte.
        const uint8_t *flag = reinterpret_cast<const uint8_t *>(ipds)
            + offsetof(DiscoveryIPDSHeader, die_info)
            + sizeof(DiscoveryDieInfo) * 16;
        if (flag >= binary && flag < binary + binarySize) {
            base_addr_64 = (*flag & 0x01) != 0;
        }
    }

    DISC_LOG("IPDS v%u, num_dies=%u, base_addr_64=%d",
             ipds->version, ipds->num_dies, base_addr_64 ? 1 : 0);

    uint32_t ips_recognised = 0;
    uint32_t ips_total = 0;

    for (uint16_t d = 0; d < ipds->num_dies && d < 16; d++) {
        uint32_t die_off = ipds->die_info[d].die_offset;
        if (die_off == 0 || die_off + sizeof(DiscoveryDieHeader) > binarySize) {
            DISC_LOG("die[%u]: die_offset %u out of range — skipping",
                     d, die_off);
            continue;
        }
        auto *die = reinterpret_cast<const DiscoveryDieHeader *>(binary + die_off);
        uint32_t pos = die_off + sizeof(DiscoveryDieHeader);

        DISC_LOG("die[%u] id=%u, num_ips=%u, ips start at %#x",
                 d, die->die_id, die->num_ips, pos);

        for (uint16_t i = 0; i < die->num_ips; i++) {
            if (pos + sizeof(DiscoveryIPv4) > binarySize) {
                DISC_LOG("die[%u] ip[%u]: walked off the end at %#x",
                         d, i, pos);
                break;
            }
            auto *ip = reinterpret_cast<const DiscoveryIPv4 *>(binary + pos);
            const uint32_t base_size = base_addr_64 ? 8 : 4;
            const uint32_t this_ip_size =
                sizeof(DiscoveryIPv4) + ip->num_base_address * base_size;
            if (pos + this_ip_size > binarySize) {
                DISC_LOG("die[%u] ip[%u]: spans past binary", d, i);
                break;
            }

            // First base address only — that's what SOC15 uses (BASE_IDX=0).
            uint32_t base32 = 0;
            if (ip->num_base_address > 0) {
                if (base_addr_64) {
                    auto *p = reinterpret_cast<const uint64_t *>(
                        reinterpret_cast<const uint8_t *>(ip) + sizeof(DiscoveryIPv4));
                    base32 = static_cast<uint32_t>(*p);  // low 32 bits
                } else {
                    auto *p = reinterpret_cast<const uint32_t *>(
                        reinterpret_cast<const uint8_t *>(ip) + sizeof(DiscoveryIPv4));
                    base32 = *p;
                }
            }

            IPBlock blk = hwid_to_block(ip->hw_id);
            if (blk != IPBlock::Count && ip->instance_number == 0) {
                // Only first instance — multi-instance for things like SDMA
                // already has its own HWID per instance (SDMA0 vs SDMA1).
                dev.ip.set(blk, base32);
                ips_recognised++;
                DISC_LOG("  ip[%u] hw_id=%u %{public}s v%u.%u.%u "
                         "base=%#010x",
                         i, ip->hw_id, block_name(blk),
                         ip->major, ip->minor, ip->revision, base32);

                // First time we see GC, capture its version too for
                // user-visible reporting.
                if (blk == IPBlock::GC && outResult) {
                    outResult->ip_version_major = ip->major;
                    outResult->ip_version_minor = ip->minor;
                    outResult->ip_version_rev   = ip->revision;
                }
            }
            ips_total++;
            pos += this_ip_size;
        }
    }

    if (outResult) {
        outResult->ok            = true;
        outResult->num_dies      = ipds->num_dies;
        outResult->num_ips_total = ips_total;
    }
    DISC_LOG("discovery parse complete: %u ips total, %u matched our IPBlock map",
             ips_total, ips_recognised);
    return kIOReturnSuccess;
}

} // namespace amdgpu
