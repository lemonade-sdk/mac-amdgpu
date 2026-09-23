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
    case HWID::MMHUB:  return IPBlock::MMHUB;
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
    case IPBlock::MMHUB:  return "MMHUB";
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
    // This port consumes the v1 fixed six-table directory. v2 moves it.
    if (hdr->version_major != 1 || hdr->binary_size < sizeof(*hdr) ||
        hdr->binary_size > binarySize) {
        fail(outResult, "unsupported binary header or invalid declared size");
        return kIOReturnInvalid;
    }
    const uint32_t declaredSize = hdr->binary_size;
    constexpr uint32_t checksumStart =
        offsetof(DiscoveryBinaryHeader, binary_checksum) + sizeof(hdr->binary_checksum);
    if (compute_checksum(binary + checksumStart, declaredSize - checksumStart) !=
        hdr->binary_checksum) {
        fail(outResult, "binary checksum mismatch");
        return kIOReturnInvalid;
    }

    const DiscoveryTableInfo &ipds_info = hdr->table_list[0];
    if (ipds_info.offset < sizeof(*hdr) ||
        uint32_t(ipds_info.offset) + sizeof(DiscoveryIPDSHeader) > declaredSize) {
        fail(outResult, "IPDS table offset out of range");
        return kIOReturnInvalid;
    }
    auto *ipds = reinterpret_cast<const DiscoveryIPDSHeader *>(binary + ipds_info.offset);
    const uint32_t tableEnd = uint32_t(ipds_info.offset) + ipds->size;
    if (ipds->signature != kDiscoveryIPDSSignature ||
        ipds->version < 1 || ipds->version > 4 ||
        ipds->size < sizeof(*ipds) || tableEnd > declaredSize ||
        ipds->num_dies == 0 || ipds->num_dies > 16) {
        fail(outResult, "invalid IPDS header, version or table bounds");
        return kIOReturnInvalid;
    }
    // Linux uses the size inside IPDS, rather than table_info.size.
    if (compute_checksum(binary + ipds_info.offset, ipds->size) != ipds_info.checksum) {
        fail(outResult, "IPDS checksum mismatch");
        return kIOReturnInvalid;
    }
    const bool base_addr_64 = ipds->version == 4 && (ipds->flags & 1);
    IPBaseTable parsed;
    DISC_LOG("IPDS v%u, num_dies=%u, base_addr_64=%d",
             ipds->version, ipds->num_dies, base_addr_64 ? 1 : 0);

    uint32_t ips_recognised = 0;
    uint32_t ips_total = 0;

    for (uint16_t d = 0; d < ipds->num_dies; d++) {
        uint32_t die_off = ipds->die_info[d].die_offset;
        if (die_off < ipds_info.offset + sizeof(*ipds) ||
            die_off + sizeof(DiscoveryDieHeader) > tableEnd) {
            fail(outResult, "die offset outside IPDS table");
            return kIOReturnInvalid;
        }
        auto *die = reinterpret_cast<const DiscoveryDieHeader *>(binary + die_off);
        if (die->die_id != ipds->die_info[d].die_id) {
            fail(outResult, "die identifier mismatch");
            return kIOReturnInvalid;
        }
        uint32_t pos = die_off + sizeof(DiscoveryDieHeader);

        DISC_LOG("die[%u] id=%u, num_ips=%u, ips start at %#x",
                 d, die->die_id, die->num_ips, pos);

        for (uint16_t i = 0; i < die->num_ips; i++) {
            if (pos + sizeof(DiscoveryIPv4) > tableEnd) {
                fail(outResult, "truncated IP record");
                return kIOReturnInvalid;
            }
            auto *ip = reinterpret_cast<const DiscoveryIPv4 *>(binary + pos);
            const uint32_t base_size = base_addr_64 ? 8 : 4;
            const uint32_t this_ip_size =
                sizeof(DiscoveryIPv4) + ip->num_base_address * base_size;
            // Linux struct_size(ip, base_address, num_base_address) permits
            // zero bases: the record still carries an IP version. GC-housed
            // engines may have no independent register segment here.
            if (pos + this_ip_size > tableEnd) {
                DISC_LOG("die[%u] ip[%u] hw_id=%u bases=%u: record [%#x..%#x) exceeds table end %#x",
                         d, i, ip->hw_id, ip->num_base_address, pos, pos + this_ip_size, tableEnd);
                fail(outResult, "IP base-address array exceeds table bounds");
                return kIOReturnInvalid;
            }

            // Read ALL base addresses (up to kMaxBaseSegments). Upstream
            // registers declare their BASE_IDX alongside the offset; each
            // BASE_IDX picks a different slot in this array.
            uint32_t bases[IPBaseTable::kMaxBaseSegments] = {0};
            uint8_t numBases = ip->num_base_address;
            if (numBases > IPBaseTable::kMaxBaseSegments) {
                numBases = IPBaseTable::kMaxBaseSegments;
            }
            for (uint8_t b = 0; b < numBases; b++) {
                const uint8_t *p = binary + pos + sizeof(DiscoveryIPv4) + b * base_size;
                // Packed records need not align uint64_t on ARM64. Linux also
                // discards ASIC-specific high bits from 64-bit base addresses.
                if (base_addr_64) {
                    uint64_t value;
                    memcpy(&value, p, sizeof(value));
                    bases[b] = uint32_t(value) & 0x3FFFFFFFu;
                } else {
                    memcpy(&bases[b], p, sizeof(bases[b]));
                }
            }

            IPBlock blk = hwid_to_block(ip->hw_id);
            if (blk != IPBlock::Count && ip->instance_number == 0) {
                // Only first instance — multi-instance for things like SDMA
                // already has its own HWID per instance (SDMA0 vs SDMA1).
                for (uint8_t b = 0; b < numBases; b++) {
                    parsed.setBase(blk, b, bases[b]);
                }
                // Capture the discovered IP version per block so runtime
                // code can switch register-offset tables / function
                // pointers based on the actual chip.
                // [[feedback_mac_amdgpu_per_ip_version_offsets]]
                parsed.setVersion(blk, IPVersion{
                    ip->major, ip->minor, ip->revision});
                ips_recognised++;
                DISC_LOG("  ip[%u] hw_id=%u %{public}s v%u.%u.%u "
                         "bases=[%#010x %#010x %#010x %#010x %#010x "
                         "%#010x %#010x %#010x] (n=%u)",
                         i, ip->hw_id, block_name(blk),
                         ip->major, ip->minor, ip->revision,
                         bases[0], bases[1], bases[2], bases[3],
                         bases[4], bases[5], bases[6], bases[7],
                         numBases);

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

    dev.ip = parsed;
    if (outResult) {
        outResult->ok            = true;
        outResult->num_dies      = ipds->num_dies;
        outResult->num_ips_total = ips_total;
    }

    // Note: the on-die discovery binary DOES populate NBIO SEG5
    // (0x04040000) — kMaxBaseSegments=5 was previously truncating it.
    // Now that the array can hold it, isResolved(NBIO, 5) returns true.
    // HOWEVER: SEG5 base 0x04040000 (dwords) = 16 MB byte offset, which
    // is BEYOND Apple's 512 KB BAR5 mapping. NBIO SEG4 (0x0241B000) and
    // SEG5 (0x04040000) registers can only be reached via SMN indirect
    // (PCIE_INDEX2/PCIE_DATA2), not direct BAR5 MMIO. Callers that try
    // to RREG32/WREG32 at those addresses will panic the dext.

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

    // Step 0: match amdgpu_discovery_read_binary_from_mem: wait for
    // IFWI via the absolute C2PMSG_33 alias before reading MEMSIZE.
    // C2PMSG_35 is only the later PSP bootloader command-ready gate.
    {
        const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint64_t waited_ms = 0;
        uint32_t msg = 0;
        bool psp_ready = false;
        while (waited_ms < BootstrapRegs::kIFWITimeoutMs) {
            msg = RREG32_abs(dev, BootstrapRegs::MP0_C2PMSG_33);
            if (msg == UINT32_MAX) {
                fail(outResult, "IFWI register unavailable; GPU detached");
                return kIOReturnNotAttached;
            }
            if ((msg & BootstrapRegs::kIFWIReadyMask) ==
                BootstrapRegs::kIFWIReadyValue) {
                psp_ready = true;
                break;
            }
            IOSleep(1);
            waited_ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000000;
        }
        if (!psp_ready) {
            DISC_LOG("on-die: IFWI poll inconclusive after %llu ms "
                     "(MP0_C2PMSG_33=%#x); proceeding — MEMSIZE will be the "
                     "authoritative ready check", waited_ms, msg);
        } else {
            DISC_LOG("on-die: IFWI ready after %llu ms "
                     "(MP0_C2PMSG_33=%#x)", waited_ms, msg);
        }
    }

    // Step 1: VRAM size in MB from the legacy absolute mmRCC_CONFIG_MEMSIZE.
    // Upstream amdgpu does this on every ASIC. After IFWI completes,
    // the NBIO sets up the legacy alias so this read works regardless
    // of NBIO version. Value == 0 is NOT a failure — it means the TMR
    // is in sysmem (we check DRIVER_SCRATCH_2 next). Only U32_MAX is
    // a hard fault.
    uint32_t vram_mb = RREG32_abs(dev, BootstrapRegs::RCC_CONFIG_MEMSIZE);
    if (vram_mb == 0xFFFFFFFFu) {
        char m[96];
        snprintf(m, sizeof(m),
                 "RCC_CONFIG_MEMSIZE returned U32_MAX — GPU not responding");
        if (outResult) snprintf(outResult->err, sizeof(outResult->err), "%s", m);
        DISC_LOG("%{public}s", m);
        return kIOReturnNotReady;
    }
    bool tmr_in_sysmem = (vram_mb == 0);
    uint64_t vram_bytes = static_cast<uint64_t>(vram_mb) << 20;
    DISC_LOG("on-die: VRAM size = %u MB (%llu bytes) from RCC_CONFIG_MEMSIZE "
             "(tmr_in_sysmem=%d)",
             vram_mb, vram_bytes, (int)tmr_in_sysmem);

    // Step 2: sysmem-TMR override path. Always check DRIVER_SCRATCH_2
    // regardless of vram_mb — when tmr_in_sysmem this is the only
    // way to find the binary.
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

    // If we got here with tmr_in_sysmem but no DRIVER_SCRATCH backing,
    // there's nothing more we can do — PSP hasn't published the TMR
    // location anywhere and we have no VRAM to scan. Caller should
    // fall back to LoadDiscoveryBin from userspace (firmware file).
    if (tmr_in_sysmem) {
        DISC_LOG("on-die: vram_size=0 and DRIVER_SCRATCH_2=%#x — TMR "
                 "neither in VRAM nor sysmem-published. Need "
                 "LoadDiscoveryBin from userspace.", scratch2);
        if (outResult) snprintf(outResult->err, sizeof(outResult->err),
                                "no on-die TMR available (vram=0, "
                                "scratch2=%#x)", scratch2);
        return kIOReturnUnsupported;
    }

    // Step 3: VRAM-resident TMR. Upstream places it at vram_end - 1 MB
    // (top of FULL VRAM, not top-of-visible). For 32 GB cards with
    // small-BAR (256 MB visible) this is well outside BAR0's aperture.
    //
    // Mirror upstream amdgpu_device_vram_access(): if the offset is
    // inside the visible aperture, use BAR0 memcpy_fromio; otherwise
    // use the mmMM_INDEX/DATA register pair to walk VRAM dword-by-dword.
    uint64_t tmr_offset = vram_bytes - kDiscoveryTMROffset;
    bool use_aperture = (tmr_offset + kDiscoveryTMRSize) <= dev.bar0Size;
    DISC_LOG("on-die: TMR at VRAM offset %#llx (size %u); "
             "visible aperture %llu MB → using %{public}s path",
             tmr_offset, kDiscoveryTMRSize,
             dev.bar0Size >> 20,
             use_aperture ? "BAR0-aperture" : "MM_INDEX/DATA");

    // Step 4: read the binary into a local buffer.
    uint8_t  binary[kDiscoveryTMRSize];
    uint32_t *out_dw = reinterpret_cast<uint32_t *>(binary);
    const uint32_t dword_count = kDiscoveryTMRSize / 4;
    if (use_aperture) {
        for (uint32_t i = 0; i < dword_count; i++) {
            out_dw[i] = RBAR2_32(dev, tmr_offset + i * 4ULL);
        }
    } else {
        for (uint32_t i = 0; i < dword_count; i++) {
            out_dw[i] = RVRAM32_via_mm(dev, tmr_offset + i * 4ULL);
        }
    }

    // Sanity-check the binary signature before handing to the parser.
    auto *hdr = reinterpret_cast<const DiscoveryBinaryHeader *>(binary);
    if (hdr->signature != kDiscoverySignature) {
        DISC_LOG("on-die: bad signature %#010x at VRAM+%#llx via %{public}s "
                 "(visible-VRAM %llu MB)",
                 hdr->signature, tmr_offset,
                 use_aperture ? "BAR0-aperture" : "MM_INDEX/DATA",
                 dev.bar0Size >> 20);
        DISC_LOG("on-die: first 8 dwords: "
                 "%#010x %#010x %#010x %#010x %#010x %#010x %#010x %#010x",
                 out_dw[0], out_dw[1], out_dw[2], out_dw[3],
                 out_dw[4], out_dw[5], out_dw[6], out_dw[7]);
        if (outResult) snprintf(outResult->err, sizeof(outResult->err),
                                "bad sig %#010x at VRAM+%#llx (first dw=%#010x)",
                                hdr->signature, tmr_offset, out_dw[0]);
        return kIOReturnInvalid;
    }

    DISC_LOG("on-die: read %u bytes from VRAM+%#llx, signature ok",
             kDiscoveryTMRSize, tmr_offset);
    return discovery_parse(binary, kDiscoveryTMRSize, dev, outResult);
}

} // namespace amdgpu
