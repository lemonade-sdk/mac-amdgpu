// amdgpu_mtopg — read-only GPU telemetry transport.
//
// Swift re-implementation of the IOKit observer client used by the terminal
// monitor (amdgpu_mtop/transport.cpp). Same MacAMDGPU service, same
// ExternalMethod selector protocol. Read-only: this process never opens the
// HSA runtime, never submits work and never mutates driver state; running it
// in parallel with the terminal TUI or a GPU workload is safe.
//
// Note: struct endpoints are read as raw byte buffers sized to the C struct
// sizes (192 / 96 / 456, see dext/amdgpu static_asserts) and decoded with
// explicit offsets, because Swift's layout of C++ mirror structs is not
// reliable (arrays in structs change padding).

// Bounds-checked little-endian readers for the raw struct buffers. The raw
// pointer load the previous code used (baseAddress!.advanced(by:).load(as:))
// traps with a Swift brk#1 when the buffer is empty or shorter than the read
// offset, which happens when a struct call lands while the driver service is
// being torn down at app quit. These read the bytes explicitly (no pointer
// arithmetic, no force-unwrap) and return 0 for any out-of-range read, so a
// partial/empty response decodes to a zeroed snapshot instead of crashing.
enum SafeLE {
    static func u32(_ b: [UInt8], _ offset: Int) -> UInt32 {
        guard offset >= 0, offset + 4 <= b.count else { return 0 }
        return UInt32(b[offset]) | UInt32(b[offset + 1]) << 8
             | UInt32(b[offset + 2]) << 16 | UInt32(b[offset + 3]) << 24
    }
    static func u64(_ b: [UInt8], _ offset: Int) -> UInt64 {
        guard offset >= 0, offset + 8 <= b.count else { return 0 }
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(b[offset + i]) << (8 * i) }
        return v
    }
}
//
// MIT License — see the repository LICENSE.

import CoreFoundation
import Foundation
import IOKit

// MARK: - Driver ABI (mirror of dext/amdgpu headers, versioned by the driver
// identity word read on every connection; see kMinimumBuild).

enum DriverABI {
    static let selIdentity: UInt32 = 43     // out: magic "AMDGPUAB", 1, build
    static let selQuery: UInt32 = 21        // in: tag; out: tag-specific words
    static let selMetrics: UInt32 = 47      // struct out: SMUMetricsSnapshot (192 B)
    static let selClocks: UInt32 = 62       // struct out: SMUClockSnapshot (96 B)
    static let selSoftware: UInt32 = 61     // struct out: software_stats::Snapshot (456 B)
    static let selSensors: UInt32 = 63      // out: 3 u64 (refresh bounded sensor cache)
    static let selSqSlot: UInt32 = 69       // out: 5 u64 [status, seq, sq_busy, ref, clock]
    static let selSqBusy: UInt32 = 70       // out: 5 u64 [status, busy_delta, total_delta, pct_x100, at_ns]
    static let selGrbmStatus: UInt32 = 71   // out: 3 u64 [status, raw GRBM_STATUS, at_ns]
    static let selSpecSnapshot: UInt32 = 73 // struct out: GFXSpecSnapshot (128 B)
    static let selMmhub: UInt32 = 68        // out: 4 u64 PERFSTATUS UMC busy

    static let minimumBuild: UInt32 = 172
    static let softwareMinimumBuild: UInt32 = 193
    static let mmhubMinimumBuild: UInt32 = 198
    static let sqSlotMinimumBuild: UInt32 = 200
    static let sqBusyMinimumBuild: UInt32 = 200   // driver-owned SQ busy counter (sel 70)
    static let grbmStatusMinimumBuild: UInt32 = 203
    static let specSnapshotMinimumBuild: UInt32 = 204
    static let magic: UInt64 = 0x414D444750554142 // "AMDGPUAB"

    // Struct payload sizes (C static_asserts in dext/amdgpu).
    static let metricsSize = 192
    static let clocksSize = 96
    static let softwareSize = 456
    static let specSize = 128

    // QueryInfo (21) tags.
    static let tagGfxVersion: UInt64 = 1
    static let tagVram: UInt64 = 2
    static let tagStage: UInt64 = 4
    static let tagAccounting: UInt64 = 5
}

// SMU metrics fields (amdgpu_metrics.h enum order).
enum SMUField: UInt {
    case gfxActivityPercent = 0
    case umcActivityPercent = 1
    case mediaActivityPercent = 2
    case gfxClockMHz = 3
    case memoryClockMHz = 4
    case socClockMHz = 5
    case fabricClockMHz = 6
    case socketPowerMilliwatts = 7
    case boardPowerMilliwatts = 8
    case edgeTemperatureMillicelsius = 9
    case hotspotTemperatureMillicelsius = 10
    case memoryTemperatureMillicelsius = 11
    case fanRPM = 12
    static let count = 16
}

let kSMUMetricsSnapshotVersion: UInt32 = 1
let kSMUMetricsStaleAfterNs: UInt64 = 2_500_000_000
let kMMHUBPerfStatusMaxQ8: UInt64 = 1_048_575

// MARK: - Decoded snapshots

struct SMUMetricsSnapshot {

    var version: UInt32 = 0
    var size: UInt32 = 0
    var status: UInt32 = 0
    var flags: UInt32 = 0
    var generation: UInt64 = 0
    var sequence: UInt64 = 0
    var collectedAtNs: UInt64 = 0
    var attemptedAtNs: UInt64 = 0
    var driverInterface: UInt32 = 0
    var firmwareCounter: UInt32 = 0
    var validFields: UInt64 = 0
    var values = [UInt64](repeating: 0, count: Int(SMUField.count))

    init() {}

    init(bytes: [UInt8]) {
        // C layout: 4 x u32, 4 x u64, 2 x u32, u64, u64[17].
        // Bounds-checked little-endian reads: a struct call that returns a
        // short/empty buffer (driver being torn down) must read back zero, not
        // trap on an out-of-range or nil-based raw load.
        func le(_ offset: Int) -> UInt32 { SafeLE.u32(bytes, offset) }
        func lu(_ offset: Int) -> UInt64 { SafeLE.u64(bytes, offset) }
        version = le(0); size = le(4); status = le(8); flags = le(12)
        generation = lu(16); sequence = lu(24); collectedAtNs = lu(32); attemptedAtNs = lu(40)
        driverInterface = le(48); firmwareCounter = le(52)
        validFields = lu(56)
        for i in 0..<Int(SMUField.count) { values[i] = lu(64 + 8 * i) }
    }
}

struct SMUClockSnapshot {

    var version: UInt32 = 0
    var size: UInt32 = 0
    var status: UInt32 = 0
    var flags: UInt32 = 0
    var generation: UInt64 = 0
    var collectedAtNs: UInt64 = 0
    var driverInterface: UInt32 = 0
    var firmwareVersion: UInt32 = 0
    var currentValid: UInt32 = 0
    var limitsValid: UInt32 = 0
    var currentMHz = [UInt32](repeating: 0, count: 4)
    var minimumMHz = [UInt32](repeating: 0, count: 4)
    var maximumACMHz = [UInt32](repeating: 0, count: 4)

    init() {}

    init(bytes: [UInt8]) {
        // C layout: 4 x u32, 2 x u64, 4 x u32, 3 x u32[4].
        func le(_ offset: Int) -> UInt32 { SafeLE.u32(bytes, offset) }
        func lu(_ offset: Int) -> UInt64 { SafeLE.u64(bytes, offset) }
        version = le(0); size = le(4); status = le(8); flags = le(12)
        generation = lu(16); collectedAtNs = lu(24)
        driverInterface = le(32); firmwareVersion = le(36)
        currentValid = le(40); limitsValid = le(44)
        for i in 0..<4 {
            currentMHz[i] = le(48 + 4 * i)
            minimumMHz[i] = le(64 + 4 * i)
            maximumACMHz[i] = le(80 + 4 * i)
        }
    }
}

struct SoftwareEngineSnapshot {
    var submitted: UInt64 = 0, completed: UInt64 = 0, failed: UInt64 = 0
    var pending: UInt64 = 0, retired: UInt64 = 0
    var pendingNs: UInt64 = 0
    var bytes = [UInt64](repeating: 0, count: 5)

    init(bytes: [UInt8]) {
        // C layout: 5 x u64 + u64[5].
        func lu(_ offset: Int) -> UInt64 { SafeLE.u64(bytes, offset) }
        submitted = lu(0); completed = lu(8); failed = lu(16); pending = lu(24); retired = lu(32)
        pendingNs = lu(40)
        for i in 0..<5 { self.bytes[i] = lu(48 + 8 * i) }
    }

    init() {}
}

struct SoftwareStatsSnapshot {
    static let engineCount = 4

    init() {}
    var version: UInt32 = 0
    var size: UInt32 = 0
    var flags: UInt32 = 0
    var reserved: UInt32 = 0
    var generation: UInt64 = 0
    var sampledAtNs: UInt64 = 0
    var sessionStartNs: UInt64 = 0
    var participants: UInt64 = 0
    var activeQueues: UInt64 = 0
    var queuedPackets: UInt64 = 0
    var publishedPackets: UInt64 = 0
    var consumedPackets: UInt64 = 0
    var retiredPackets: UInt64 = 0
    var cpuUploadBytes: UInt64 = 0
    var cpuReadbackBytes: UInt64 = 0
    var engines = [SoftwareEngineSnapshot](repeating: SoftwareEngineSnapshot(), count: SoftwareStatsSnapshot.engineCount)
    var available: Bool { flags & 1 != 0 }
    var saturated: Bool { flags & 4 != 0 }

    init(bytes: [UInt8]) {
        // C layout: 4 x u32, 11 x u64, then 4 x EngineSnapshot (88 B each).
        func le(_ offset: Int) -> UInt32 { SafeLE.u32(bytes, offset) }
        func lu(_ offset: Int) -> UInt64 { SafeLE.u64(bytes, offset) }
        version = le(0); size = le(4); flags = le(8); reserved = le(12)
        generation = lu(16); sampledAtNs = lu(24); sessionStartNs = lu(32)
        participants = lu(40); activeQueues = lu(48); queuedPackets = lu(56)
        publishedPackets = lu(64); consumedPackets = lu(72); retiredPackets = lu(80)
        cpuUploadBytes = lu(88); cpuReadbackBytes = lu(96)
        for i in 0..<Self.engineCount {
            let base = 104 + 88 * i
            engines[i] = SoftwareEngineSnapshot(bytes: Array(bytes[base...]))
        }
    }
}

// The shared SQ busy-cycle slot as published by the workload process
// (HRX LSE backend sq_profiler: aqlprofile SQ performance counter sums over a
// fixed window) and read by the dext through the BAR4/GART window.
// status != 0 means no slot registered / source unavailable (show n/a, not 0).
struct SqSlotSample {
    var status: UInt32 = 0     // 0 = fresh sample below
    var seq: UInt32 = 0        // monotonic window counter (wraps)
    var sqBusy: UInt32 = 0     // SQ busy cycles accumulated in the window
    var refTicks: UInt32 = 0   // reference cycles (tick range) in the window
    var clockHz: UInt32 = 0    // 0 = wall-clock-bounded window (ns in refTicks)
    var valid = false
}

struct MmhubPerfStatus {
    var status: UInt32 = 0
    var raw: UInt32 = 0
    var umcBusyQ8: UInt64 = 0
    var collectedAtNs: UInt64 = 0
    var valid = false

    /// The MMHUB UMC-busy PERFSTATUS register (offset 0x04c18) does not exist
    /// on this ASIC (gfx1201 / RDNA4): no such register is defined in any AMD
    /// header. Driver build 199+ reports the source unavailable (status != 0,
    /// zeroed data) instead of reading the unmapped slot; pre-199 read back
    /// 0xFFFFFFFF (all-ones) at both idle and under an active LSE decode. The
    /// usable check therefore rejects any non-zero status (unavailable) or an
    /// all-ones raw (the pre-199 unmapped-register read), falling back to the
    /// SMU firmware average.
    var usable: Bool {
        valid && status == 0 && raw != 0xFFFFFFFF
    }
}

// Selector 70's ABI is reserved for a driver-owned SQ busy-cycle delta.
// The current driver reports kIOReturnUnsupported because gfx1201 SQ counters
// require a workload-side CP perfmon program; show n/a until one is qualified.
struct SqBusySample {
    var status: UInt32 = 0     // 0 = fresh sample; kIOReturnUnsupported = no source
    var busyDelta: UInt64 = 0  // busy shader cycles in the window
    var totalDelta: UInt64 = 0 // total shader cycles in the window
    var pctX100: UInt64 = 0    // busy % scaled by 100 (55% => 5500)
    var sampledAtNs: UInt64 = 0
    var valid = false
    var usable: Bool { valid && status == 0 && totalDelta > 0 }

    /// Busy percentage in 0..100 (for display), or nil when no usable sample.
    var percent: Double? {
        usable ? Double(pctX100) / 100.0 : nil
    }
}

struct GrbmStatusSample {
    var status: UInt32 = 0
    var raw: UInt32 = 0
    var sampledAtNs: UInt64 = 0
    var valid = false
    var active: Bool { raw & 0x8000_0000 != 0 }
}

// MARK: - Device

final class Device {
    var registry: UInt64 = 0
    var build: UInt32 = 0
    var stage: UInt32 = 0
    var gfxVersion: (UInt32, UInt32, UInt32) = (0, 0, 0)
    var visibleVRAM: UInt64 = 0
    var totalVRAM: UInt64 = 0
    var error: String?
    var specWords = [UInt32](repeating: 0, count: 32)
    var specValid = false

    // Live sources.
    var metrics = SMUMetricsSnapshot()
    var metricsSupported = false
    var metricsError: String?
    var clocks = SMUClockSnapshot()
    var clocksSupported = false
    var clocksError: String?
    var software = SoftwareStatsSnapshot()
    var softwareSupported = false
    var softwareError: String?
    var accounting = [UInt64](repeating: 0, count: 15)
    var accountingSupported = false
    var accountingError: String?
    var mmhub = MmhubPerfStatus()
    // Shared SQ busy-cycle slot (selector 69, driver 200+): the workload
    // process (HRX LSE backend) publishes real aqlprofile SQ counter sums
    // into a GART slot; status != 0 means no slot is registered yet.
    var sqSlot = SqSlotSample()
    // Driver-owned SQ busy-cycle counter (selector 70), currently unsupported.
    var sqBusy = SqBusySample()
    var grbmStatus = GrbmStatusSample()

    var label: String {
        "0x" + String(registry, radix: 16)
    }
}

// MARK: - IOKit transport

final class DriverTransport {
    // Matching dictionary, kept alive for the process lifetime. Each
    // refresh creates and fully consumes its own iterator (released
    // before this dictionary ever goes away), so no iterator ever
    // outlives its match.
    private let match: CFDictionary?
    private var lastSensorAttemptNs: [UInt64: UInt64] = [:]
    private var specCache: [UInt64: [UInt32]] = [:]

    init() {
        match = IOServiceNameMatching("MacAMDGPU")
    }

    func close() {
        // No long-lived iterator to release; the per-refresh iterator is
        // drained and released inside refresh().
    }

    private func krString(_ operation: String, _ kr: kern_return_t) -> String {
        String(format: "%s: 0x%08x", operation, kr)
    }

    // Open one observer connection, read every supported endpoint, close it.
    // The C++ monitor does the same per-sample open/close; at 10 Hz this is
    // cheap and keeps no IOConnect alive longer than one refresh.
    func read(_ service: io_service_t) -> Device {
        let device = Device()
        _ = IORegistryEntryGetRegistryEntryID(service, &device.registry)

        var connection: io_connect_t = IO_OBJECT_NULL
        let openResult = IOServiceOpen(service, mach_task_self_, 0, &connection)
        guard openResult == KERN_SUCCESS else {
            device.error = krString("IOServiceOpen", openResult)
            return device
        }
        defer { IOServiceClose(connection) }

        func scalar(_ selector: UInt32, _ inWords: [UInt64], into outWords: inout [UInt64]) -> kern_return_t {
            var count = UInt32(outWords.count)
            let result = outWords.withUnsafeMutableBufferPointer { outPtr in
                inWords.withUnsafeBufferPointer { inPtr in
                    IOConnectCallScalarMethod(connection, selector, inPtr.baseAddress, UInt32(inWords.count),
                                              outPtr.baseAddress, &count)
                }
            }
            return result == KERN_SUCCESS && count != outWords.count ? kIOReturnBadArgument : result
        }

        func structCall(_ selector: UInt32, into payload: inout [UInt8]) -> kern_return_t {
            var bytes = payload.count
            let result = IOConnectCallStructMethod(connection, selector, nil, 0,
                                                   &payload, &bytes)
            return result == KERN_SUCCESS && bytes != payload.count ? kIOReturnBadArgument : result
        }

        // Identity / build gate (selector 43).
        var identity = [UInt64](repeating: 0, count: 3)
        guard scalar(DriverABI.selIdentity, [], into: &identity) == KERN_SUCCESS, identity.count == 3,
              identity[0] == DriverABI.magic, identity[1] == 1, identity[2] >= DriverABI.minimumBuild else {
            device.error = "Unsupported driver observer ABI (requires build 172+)"
            return device
        }
        device.build = UInt32(identity[2])

        // QueryInfo tag 1: gfx version.
        var gfx = [UInt64](repeating: 0, count: 3)
        var tag = DriverABI.tagGfxVersion
        if scalar(DriverABI.selQuery, [tag], into: &gfx) == KERN_SUCCESS, gfx.count == 3,
           gfx[0] <= 0xffff, gfx[1] <= 0xff, gfx[2] <= 0xffff {
            device.gfxVersion = (UInt32(gfx[0]), UInt32(gfx[1]), UInt32(gfx[2]))
        }
        // Tag 2: VRAM visible/total.
        var vram = [UInt64](repeating: 0, count: 2)
        tag = DriverABI.tagVram
        if scalar(DriverABI.selQuery, [tag], into: &vram) == KERN_SUCCESS, vram.count == 2, vram[0] <= vram[1] {
            device.visibleVRAM = vram[0]
            device.totalVRAM = vram[1]
        }
        // Tag 4: lifecycle stage.
        var stage = [UInt64](repeating: 0, count: 1)
        tag = DriverABI.tagStage
        if scalar(DriverABI.selQuery, [tag], into: &stage) == KERN_SUCCESS, stage.count == 1 {
            device.stage = UInt32(stage[0])
        }

        // Selector 47/62 only return cached CPU snapshots. Ask the driver to
        // refresh that cache when an initialized owner already has the GPU
        // running. This observer endpoint neither opens PCI nor claims a
        // session; both the monitor and driver limit collection to 1 Hz.
        if device.build >= DriverABI.softwareMinimumBuild, device.stage == 15 {
            let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
            let previous = lastSensorAttemptNs[device.registry] ?? 0
            if previous == 0 || now < previous || now - previous >= 1_000_000_000 {
                lastSensorAttemptNs[device.registry] = now
                var result = [UInt64](repeating: 0, count: 3)
                _ = scalar(DriverABI.selSensors, [], into: &result)
            }
        }

        // The original QueryInfo tag 8 returns 32 scalars, above DriverKit's
        // 16-scalar RPC limit. Build 203 exposes the same 128-byte data as a
        // struct; older builds leave geometry unavailable.
        if device.stage != 15 { specCache.removeValue(forKey: device.registry) }
        if device.build >= DriverABI.specSnapshotMinimumBuild, device.stage == 15 {
            if let spec = specCache[device.registry] {
                device.specWords = spec
                device.specValid = true
            } else {
                var raw = [UInt8](repeating: 0, count: DriverABI.specSize)
                if structCall(DriverABI.selSpecSnapshot, into: &raw) == KERN_SUCCESS {
                    let spec = (0..<32).map { SafeLE.u32(raw, $0 * 4) }
                    if spec[0] != 0 {
                        specCache[device.registry] = spec
                        device.specWords = spec
                        device.specValid = true
                    }
                }
            }
        }

        // Software activity counters (selector 61, build 193+).
        if device.build >= DriverABI.softwareMinimumBuild {
            var raw = [UInt8](repeating: 0, count: DriverABI.softwareSize)
            let kr = structCall(DriverABI.selSoftware, into: &raw)
            if kr == KERN_SUCCESS {
                let s = SoftwareStatsSnapshot(bytes: raw)
                if Self.validSoftware(s) {
                    device.software = s
                    device.softwareSupported = true
                } else {
                    device.softwareError = "Invalid software counter ABI"
                }
            } else {
                device.softwareError = krString("Software counters", kr)
            }
        }

        // Shared SQ busy-cycle slot (selector 69, build 200+): five scalars
        // [status, seq, sq_busy, ref, clock]. Observer-only, no allocation.
        // A zeroed slot (seq 0, no data) is a legitimate "source says no
        // work yet" reading once the workload has registered, so status==0
        // with words present is valid even at seq == 0.
        if device.build >= DriverABI.sqSlotMinimumBuild {
            var words = [UInt64](repeating: 0, count: 5)
            let kr = scalar(DriverABI.selSqSlot, [], into: &words)
            // The driver sign-extends a negative kern_return_t to the full 64
            // bits (e.g. 0xffffffffe000xxxx), which does NOT fit a UInt32 and
            // would trap on a raw UInt32(words[n]). Mask every word to its low
            // 32 bits BEFORE narrowing (same discipline as selector 70 / MMHUB).
            let statusLow32 = UInt32(words[0] & 0xFFFFFFFF)
            if kr == KERN_SUCCESS, statusLow32 == 0 {
                device.sqSlot = SqSlotSample(
                    status: 0,
                    seq: UInt32(words[1] & 0xFFFFFFFF),
                    sqBusy: UInt32(words[2] & 0xFFFFFFFF),
                    refTicks: UInt32(words[3] & 0xFFFFFFFF),
                    clockHz: UInt32(words[4] & 0xFFFFFFFF))
                device.sqSlot.valid = true
            } else if kr == KERN_SUCCESS {
                // status != 0 (no slot registered yet): leave invalid, the
                // panel shows the honest n/a.
                device.sqSlot.status = statusLow32
            } else {
                device.sqSlot.status = UInt32(truncatingIfNeeded: kr)
            }
        }

        // Driver-owned SQ busy-cycle counter (selector 70, build 200+): five
        // scalars [status, busy_delta, total_delta, pct_x100, at_ns].
        // Observer-only, no allocation. status != 0 (kIOReturnUnsupported)
        // means the counter is not available (not ready) — leave invalid so
        // the panel shows the honest n/a. Check the low-32 status BEFORE any
        // narrowing (the SIGTRAP lesson: never trap on the unsupported code).
        if device.build >= DriverABI.sqBusyMinimumBuild {
            var words = [UInt64](repeating: 0, count: 5)
            let kr = scalar(DriverABI.selSqBusy, [], into: &words)
            if kr == KERN_SUCCESS, words[0] == 0 {
                device.sqBusy = SqBusySample(
                    status: 0,
                    busyDelta: words[1],
                    totalDelta: words[2],
                    pctX100: words[3],
                    sampledAtNs: words[4])
                device.sqBusy.valid = true
            } else if kr == KERN_SUCCESS {
                device.sqBusy.status = UInt32(truncatingIfNeeded: words[0])
            } else {
                device.sqBusy.status = UInt32(truncatingIfNeeded: kr)
            }
        }

        // The driver makes one passive GRBM_STATUS read per observer poll.
        // A rolling fraction of GUI_ACTIVE samples is computed in the model.
        if device.build >= DriverABI.grbmStatusMinimumBuild {
            var words = [UInt64](repeating: 0, count: 3)
            let kr = scalar(DriverABI.selGrbmStatus, [], into: &words)
            if kr == KERN_SUCCESS, words[0] == 0,
               words[1] <= UInt32.max, words[2] > 0 {
                device.grbmStatus = GrbmStatusSample(status: 0,
                    raw: UInt32(words[1]), sampledAtNs: words[2], valid: true)
            } else if kr == KERN_SUCCESS {
                device.grbmStatus.status = UInt32(truncatingIfNeeded: words[0])
            } else {
                device.grbmStatus.status = UInt32(truncatingIfNeeded: kr)
            }
        }

        // SMU clock snapshot (selector 62, build 193+).
        if device.build >= 193 {
            var raw = [UInt8](repeating: 0, count: DriverABI.clocksSize)
            let kr = structCall(DriverABI.selClocks, into: &raw)
            if kr == KERN_SUCCESS {
                let c = SMUClockSnapshot(bytes: raw)
                if c.version == 1, c.size == UInt32(DriverABI.clocksSize),
                   c.currentValid & ~15 == 0, c.limitsValid & ~15 == 0 {
                    device.clocks = c
                    device.clocksSupported = true
                } else {
                    device.clocksError = "Invalid clock snapshot ABI"
                }
            } else {
                device.clocksError = krString("Clock snapshot", kr)
            }
        }

        // SMU metrics table (selector 47, build 176+).
        if device.build >= 176 {
            var raw = [UInt8](repeating: 0, count: DriverABI.metricsSize)
            let kr = structCall(DriverABI.selMetrics, into: &raw)
            switch kr {
            case KERN_SUCCESS:
                let m = SMUMetricsSnapshot(bytes: raw)
                if Self.validMetrics(m) {
                    device.metrics = m
                    device.metricsSupported = true
                } else {
                    device.metricsError = "Invalid metrics snapshot ABI"
                }
            case kIOReturnUnsupported:
                device.metricsError = "Driver does not provide the metrics endpoint"
            default:
                device.metricsError = krString("Metrics snapshot", kr)
            }
        }

        // VRAM accounting (query tag 5, build 178+).
        if device.build >= 178 {
            var values = [UInt64](repeating: 0, count: 15)
            tag = DriverABI.tagAccounting
            if scalar(DriverABI.selQuery, [tag], into: &values) == KERN_SUCCESS, values.count == 15 {
                device.accounting = values
                device.accountingSupported = Self.validAccounting(values)
                if !device.accountingSupported { device.accountingError = "Invalid VRAM accounting ABI" }
            } else {
                device.accountingError = "VRAM accounting query failed"
            }
        }

        // MMHUB PERFSTATUS UMC busy (selector 68, build 198+; observer only).
        // Driver build 199+ reports the source unavailable on ASICs with no
        // MMHUB UMC-busy counter (gfx1201 / RDNA4) by returning KERN_SUCCESS
        // with the low 32 bits of scalarOutput[0] set to kIOReturnUnsupported
        // (0xe00002c7) as a sentinel. We must check that sentinel BEFORE
        // narrowing to UInt32: the driver sign-extends the negative
        // kern_return_t to the full 64 bits (0xffffffffe00002c7), which does
        // not fit a UInt32 and would trap. Treat any non-zero low-32-bit status
        // (or an all-ones raw, the pre-199 unmapped read) as "no live counter"
        // and leave device.mmhub invalid so the UI falls back to the SMU
        // firmware average.
        if device.build >= DriverABI.mmhubMinimumBuild {
            var result = [UInt64](repeating: 0, count: 4)
            if scalar(DriverABI.selMmhub, [], into: &result) == KERN_SUCCESS, result.count == 4 {
                let statusLow32 = UInt32(result[0] & 0xFFFFFFFF)
                let raw = UInt32(result[1] & 0xFFFFFFFF)
                device.mmhub.status = statusLow32
                device.mmhub.raw = raw
                let unavailable = (statusLow32 & 0xFFFF) == 0x2c7 && (statusLow32 & 0xE000_0000) != 0
                if !unavailable, statusLow32 == 0, raw != 0xFFFFFFFF {
                    device.mmhub = MmhubPerfStatus(status: statusLow32, raw: raw,
                                                   umcBusyQ8: result[2] & kMMHUBPerfStatusMaxQ8,
                                                   collectedAtNs: result[3])
                    device.mmhub.valid = true
                }
                // else: no live UMC-busy counter on this ASIC — leave invalid.
            }
        }

        return device
    }

    // Discover every MacAMDGPU service and read each into a Device.
    //
    // The iterator is recreated on every refresh and fully drained (which
    // also consumes it) before being released, matching the C++ monitor.
    // An iterator must be destroyed before its matching dictionary is
    // released; the dictionary is held alive for the whole refresh and
    // released only when the iterator is.
    @discardableResult
    func refresh() -> (devices: [Device], error: String?) {
        guard let match else { return ([], "Unable to allocate IOKit matching dictionary") }
        var iterator: io_iterator_t = IO_OBJECT_NULL
        let kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator)
        guard kr == KERN_SUCCESS else { return ([], krString("Device enumeration", kr)) }
        var devices: [Device] = []
        while true {
            let service = IOIteratorNext(iterator)
            guard service != IO_OBJECT_NULL else { break }
            let device = read(service)
            IOObjectRelease(service)
            devices.append(device)
        }
        IOObjectRelease(iterator)
        devices.sort { $0.registry < $1.registry }
        if ProcessInfo.processInfo.environment["MTOPG_DEBUG"] == "1" {
            var line = "[mtopg-debug] refresh: \(devices.count) device(s)"
            for d in devices {
                line += " | \(d.label) build=\(d.build) stage=\(d.stage) error=\(d.error.map { "\($0)" } ?? "none")"
            }
            FileHandle.standardError.write((line + "\n").data(using: .utf8)!)
        }
        return (devices, nil)
    }

    // MARK: Snapshot validators (same rules as the C++ monitor)

    static func validMetrics(_ s: SMUMetricsSnapshot) -> Bool {
        let flagsMask: UInt32 = 0b1111
        let fieldsMask: UInt64 = (1 << Int(SMUField.count)) - 1
        guard s.version == kSMUMetricsSnapshotVersion, s.size == UInt32(DriverABI.metricsSize),
              s.flags & ~flagsMask == 0, s.validFields & ~fieldsMask == 0 else { return false }
        if s.flags & 1 == 0 { return true }
        return s.status == 0 && s.validFields != 0 && s.flags & (1 << 1 | 1 << 2) == 0
    }

    static func validSoftware(_ s: SoftwareStatsSnapshot) -> Bool {
        guard s.version == 1, s.size == UInt32(DriverABI.softwareSize), s.reserved == 0,
              s.flags & ~UInt32(15) == 0, s.available, s.generation > 0,
              s.sampledAtNs >= s.sessionStartNs,
              s.consumedPackets <= s.publishedPackets,
              s.retiredPackets <= s.publishedPackets - s.consumedPackets,
              s.queuedPackets <= s.publishedPackets - s.consumedPackets - s.retiredPackets
        else { return false }
        for e in s.engines {
            guard e.completed <= e.submitted, e.failed <= e.submitted,
                  e.retired <= e.submitted - e.completed,
                  e.pending <= e.submitted - e.completed - e.retired,
                  e.pendingNs <= s.sampledAtNs - s.sessionStartNs
            else { return false }
        }
        return true
    }

    static func validAccounting(_ v: [UInt64]) -> Bool {
        guard v[0] == 1, v[1] & ~1 == 0 else { return false }
        if v[1] & 1 == 0 { return !v[2...].contains(where: { $0 != 0 }) }
        guard v[2] > 0, v[3] > 0, v[3] <= v[2], v[5] <= v[3], v[10] <= v[2] - v[3] else { return false }
        for (cap, used, free, span, count) in [(v[5], v[6], v[7], v[8], v[9]), (v[10], v[11], v[12], v[13], v[14])] {
            if used > cap || free != cap - used || span > free || count > used / 16384 { return false }
        }
        return v[4] == v[2] - v[5] - v[10]
    }
}

// MARK: - Field accessors with the C++ monitor's validity rules

extension Device {
    var nowNs: UInt64 {
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    }

    /// Fresh + valid SMU value for a field (stale/faulted -> nil).
    func smuValue(_ field: SMUField) -> Double? {
        guard metricsSupported, metricsError == nil, DriverTransport.validMetrics(metrics),
              (metrics.driverInterface == 0x2e ||
               (metrics.driverInterface == 0x33 && metrics.flags & 8 != 0)),
              metrics.flags & 1 != 0, nowNs >= metrics.collectedAtNs,
              nowNs - metrics.collectedAtNs <= kSMUMetricsStaleAfterNs,
              metrics.validFields & (1 << field.rawValue) != 0 else { return nil }
        return Double(metrics.values[Int(field.rawValue)])
    }

    /// Clock in MHz. index: 0 gfx, 1 soc, 2 memory, 3 fabric.
    /// kind: 0 current (freshness-gated), 1 DPM min, 2 AC DPM max.
    func clockValue(index: Int, kind: Int) -> Double? {
        guard clocksSupported, clocksError == nil, clocks.version == 1,
              clocks.size == UInt32(DriverABI.clocksSize),
              clocks.currentValid & ~15 == 0, clocks.limitsValid & ~15 == 0,
              index >= 0, index < 4 else { return nil }
        switch kind {
        case 0:
            guard clocks.flags & 1 != 0, clocks.flags & (1 << 1 | 1 << 2) == 0,
                  clocks.currentValid & (1 << index) != 0,
                  nowNs >= clocks.collectedAtNs, nowNs - clocks.collectedAtNs <= kSMUMetricsStaleAfterNs
            else { return nil }
            return Double(clocks.currentMHz[index])
        case 1:
            guard clocks.limitsValid & (1 << index) != 0 else { return nil }
            return Double(clocks.minimumMHz[index])
        default:
            guard clocks.limitsValid & (1 << index) != 0 else { return nil }
            return Double(clocks.maximumACMHz[index])
        }
    }

    // VRAM accounting (driver CPU allocator pools, in GiB).
    var vramUsedGiB: Double? {
        guard accountingSupported, accountingError == nil,
              DriverTransport.validAccounting(accounting) else { return nil }
        return (Double(accounting[6]) + Double(accounting[11])) / 1_073_741_824.0
    }

    var vramCapacityGiB: Double? {
        guard accountingSupported, accountingError == nil,
              DriverTransport.validAccounting(accounting) else { return nil }
        return (Double(accounting[5]) + Double(accounting[10])) / 1_073_741_824.0
    }

    var vramVisibleUsedGiB: Double? {
        guard accountingSupported, accountingError == nil,
              DriverTransport.validAccounting(accounting) else { return nil }
        return Double(accounting[6]) / 1_073_741_824.0
    }

    var vramVisibleCapacityGiB: Double? {
        guard accountingSupported, accountingError == nil,
              DriverTransport.validAccounting(accounting) else { return nil }
        return Double(accounting[5]) / 1_073_741_824.0
    }
}
