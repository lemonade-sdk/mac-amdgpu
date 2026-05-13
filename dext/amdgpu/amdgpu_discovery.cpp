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

//
// discover_ips_on_die — bootstraps the IP base table the way amdgpu
// does it on Linux. See amdgpu_discovery.h for the algorithm summary.
//
kern_return_t
discover_ips_on_die(DeviceContext &dev, DiscoveryParseResult *outResult)
{
    if (outResult) memset(outResult, 0, sizeof(*outResult));

    // Step 1: VRAM size in MB from the legacy absolute mmRCC_CONFIG_MEMSIZE.
    // amdgpu does this on every ASIC including RDNA4; the absolute
    // alias resolves to the right physical register regardless of
    // NBIO version.
    uint32_t vram_mb = RREG32_abs(dev, BootstrapRegs::RCC_CONFIG_MEMSIZE);
    if (vram_mb == 0 || vram_mb == 0xFFFFFFFFu) {
        char m[96];
        snprintf(m, sizeof(m),
                 "RCC_CONFIG_MEMSIZE returned %#x — GPU not responding",
                 vram_mb);
        if (outResult) snprintf(outResult->err, sizeof(outResult->err), "%s", m);
        DISC_LOG("%{public}s", m);
        return kIOReturnNotReady;
    }
    uint64_t vram_bytes = static_cast<uint64_t>(vram_mb) << 20;
    DISC_LOG("on-die: VRAM size = %u MB (%llu bytes) from RCC_CONFIG_MEMSIZE",
             vram_mb, vram_bytes);

    // Step 2: sysmem-TMR override path.
    uint32_t scratch2 = RREG32_abs(dev, BootstrapRegs::DRIVER_SCRATCH_2);
    if (scratch2 != 0 && scratch2 != 0xFFFFFFFFu) {
        uint32_t scratch0 = RREG32_abs(dev, BootstrapRegs::DRIVER_SCRATCH_0);
        uint32_t scratch1 = RREG32_abs(dev, BootstrapRegs::DRIVER_SCRATCH_1);
        uint64_t sysmem_tmr = (static_cast<uint64_t>(scratch1) << 32)
                            | scratch0;
        DISC_LOG("on-die: PSP placed TMR in sysmem at %#llx (size=%u) — "
                 "this path requires mapping host-physical sysmem from "
                 "the dext, which AS PCIDriverKit doesn't expose. Use "
                 "LoadDiscoveryBin from userspace as the escape hatch.",
                 sysmem_tmr, scratch2);
        if (outResult) snprintf(outResult->err, sizeof(outResult->err),
                                "PSP placed TMR in sysmem at %#llx — "
                                "use LoadDiscoveryBin", sysmem_tmr);
        return kIOReturnUnsupported;
    }

    // Step 3: VRAM-resident TMR. Lives at vram_end - 1 MB.
    uint64_t tmr_offset = vram_bytes - kDiscoveryTMROffset;

    // The dext can only read VRAM through BAR2's visible window.
    // If the TMR sits outside that window, on-die discovery is
    // not possible without PSP cooperation (which would land in
    // the DRIVER_SCRATCH path above).
    if (dev.bar2VisibleVRAMSize == 0) {
        DISC_LOG("on-die: BAR2 visible size unknown — cannot read TMR");
        if (outResult) snprintf(outResult->err, sizeof(outResult->err),
                                "BAR2 visible VRAM size = 0");
        return kIOReturnNotReady;
    }

    // BAR2 maps the *top* of VRAM or the *bottom* depending on PSP
    // firmware policy. The simplest assumption: BAR2 base corresponds
    // to vram_start (offset 0 in VRAM) and the visible region is
    // [0, bar2VisibleVRAMSize). On most discrete AMD GPUs the
    // TMR is placed at the very top of VRAM — outside the visible
    // window for small-BAR setups.
    //
    // Per scottjg's research (docs/AS_DART_LIMITS.md), our AS setup
    // is small-BAR (256 MB / 1 GB visible). On those configs PSP
    // either (a) places the TMR in visible VRAM by skipping the top
    // 32 GB and using offset (visible_size - 1 MB), or (b) writes
    // the location into DRIVER_SCRATCH (step 2 above).
    //
    // Try the top-of-VISIBLE-VRAM interpretation first since the
    // top-of-FULL-VRAM offset is out of BAR2 range:
    uint64_t bar2_off = tmr_offset;
    if (bar2_off >= dev.bar2VisibleVRAMSize) {
        uint64_t visible_top_offset = dev.bar2VisibleVRAMSize
                                    - kDiscoveryTMROffset;
        DISC_LOG("on-die: top-of-VRAM offset %#llx outside BAR2 visible "
                 "(%llu MB); retrying at top-of-visible %#llx",
                 tmr_offset, dev.bar2VisibleVRAMSize >> 20,
                 visible_top_offset);
        bar2_off = visible_top_offset;
    }

    // Step 4: read the binary out of BAR2 + bar2_off via 32-bit MMIO.
    // 10 KB = 2560 dwords; we allocate 16 KB to round to the AS
    // page size + the upstream DISCOVERY_TMR_SIZE.
    uint8_t  binary[kDiscoveryTMRSize];
    uint32_t *out_dw = reinterpret_cast<uint32_t *>(binary);
    const uint32_t dword_count = kDiscoveryTMRSize / 4;
    for (uint32_t i = 0; i < dword_count; i++) {
        out_dw[i] = RBAR2_32(dev, bar2_off + i * 4ULL);
    }

    // Sanity-check the binary signature before handing to the parser.
    auto *hdr = reinterpret_cast<const DiscoveryBinaryHeader *>(binary);
    if (hdr->signature != kDiscoverySignature) {
        DISC_LOG("on-die: bad signature %#010x at BAR2+%#llx — "
                 "TMR not where expected (visible-VRAM %llu MB; "
                 "consider LoadDiscoveryBin)",
                 hdr->signature, bar2_off, dev.bar2VisibleVRAMSize >> 20);
        if (outResult) snprintf(outResult->err, sizeof(outResult->err),
                                "bad signature %#010x at BAR2+%#llx",
                                hdr->signature, bar2_off);
        return kIOReturnInvalid;
    }

    DISC_LOG("on-die: read %u bytes from BAR2+%#llx, signature ok",
             kDiscoveryTMRSize, bar2_off);
    return discovery_parse(binary, kDiscoveryTMRSize, dev, outResult);
}

} // namespace amdgpu
