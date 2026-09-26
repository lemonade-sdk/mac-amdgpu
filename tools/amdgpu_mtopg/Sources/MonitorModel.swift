// amdgpu_mtopg — sample model and honest rate computation.
//
// Mirrors the terminal monitor's accounting: a value is published only when
// the underlying driver source actually produced it. In particular:
//  - GPU CORE LOAD is the delta of the driver dispatch-in-flight union
//    counter (software_stats selector 61). The SMU AverageGfxActivity field
//    is incoherent on this host (reads ~100% at verified idle) and is never
//    used for the core-load chart.
//  - UMC MEMORY ACTIVITY prefers the SMU UmcActivityPercent field when it is
//    fresh and plausible; otherwise the MMHUB PERFSTATUS hardware PERFCTR
//    delta (selector 68, driver build 198+). When neither has a value the
//    chart stays blank with an explanation — no synthetic number.
//
// MIT License — see the repository LICENSE.

import Foundation

// One rendered frame's data, computed on the sampler thread and consumed by
// SwiftUI on the main thread.

struct EngineRow: Identifiable {
    let id: Int
    let name: String
    let value: Double?      // in-flight busy % over the sample window, if any
    let note: String        // provenance / why unavailable
}

struct ClockRow: Identifiable {
    let id: Int
    let name: String
    let current: Double?    // MHz
    let minimum: Double?
    let maximum: Double?
}

struct TelemetrySnapshot {
    var deviceLabel: String = ""
    var gfxVersion: String = ""
    var build: UInt32 = 0
    var stage: UInt32 = 0
    var error: String?
    var idle: Bool = false   // stage 0 with zero in-flight dispatches: healthy at-rest state
    var specInfo: String?   // "96 CUs / 8 SEs"
    var softwareStatsStatus: String = "unsupported"

    // Charts (rolling, oldest -> newest; nil entries are gaps).
    var coreLoad: [(age: Double, value: Double?)] = []
    var umcActivity: [(age: Double, value: Double?)] = []
    var umcSourceLabel: String?   // label of the most recent plotted source
    var coreCurrent: Double?      // latest published sample

    var selectedDeviceID: UInt64?

    // Readouts.
    var vramUsedGiB: Double?
    var vramTotalGiB: Double?
    var vramVisibleUsedGiB: Double?
    var vramVisibleTotalGiB: Double?
    var clocks: [ClockRow] = []
    var engines: [EngineRow] = []

    var powerWatts: Double?
    var boardPowerWatts: Double?
    var edgeCelsius: Double?
    var hotspotCelsius: Double?
    var fanRPM: Double?
}

// Rolling window of raw driver samples. All rate math lives here so the
// render path stays trivial and the values stay honest.

final class SampleHistory {
    static let windowNs: UInt64 = 60_000_000_000   // 60 s
    static let maxPoints = 1200                   // 10 Hz * 120 s safety

    struct Point {
        var timeNs: UInt64
        var core: Double?
        var umc: Double?
    }

    private(set) var points: [Point] = []
    private var previousBusy: (generation: UInt64, timeNs: UInt64, pendingNs: UInt64, engines: [UInt64])?
    private var previousUmc: (q8: UInt64, atNs: UInt64)?

    var lastUmhubSource: String?

    // The plotted window as (age-from-`nowNs`-seconds, value), oldest ->
    // newest. Ages come from the sample timestamps, not the array index, so
    // an irregular 10 Hz cadence still scrolls correctly.
    func series(_ key: KeyPath<Point, Double?>, nowNs: UInt64) -> [(age: Double, value: Double?)] {
        points.map { (age: Double(nowNs - $0.timeNs) / 1e9, value: $0[keyPath: key]) }
    }

    var coreCurrent: Double? { points.last { $0.core != nil }?.core }

    func add(_ d: Device) {
        let now = d.nowNs
        var core: Double?
        var umc: Double?

        // ---- GPU CORE LOAD: dispatch-in-flight delta (selector 61) ----
        if d.softwareSupported, !d.software.saturated {
            var pendingNs: UInt64 = 0
            var engines: [UInt64] = []
            engines.reserveCapacity(4)
            var overflow = false
            for e in d.software.engines {
                if pendingNs > UInt64.max - e.pendingNs { overflow = true; break }
                pendingNs += e.pendingNs
                engines.append(e.pendingNs)
            }
            if overflow {
                previousBusy = nil
            } else {
                if let prev = previousBusy,
                   prev.generation == d.software.generation,
                   prev.timeNs < d.software.sampledAtNs,
                   prev.pendingNs <= pendingNs,
                   d.software.sampledAtNs - prev.timeNs <= 2_000_000_000,
                   engines.count == prev.engines.count {
                    let delta = pendingNs - prev.pendingNs
                    let elapsed = d.software.sampledAtNs - prev.timeNs
                    if elapsed > 0 {
                        let ratio = Double(delta) / Double(elapsed) * 100.0
                        if ratio >= 0, ratio.isFinite {
                            core = min(max(ratio, 0), 100)
                        }
                    }
                }
                previousBusy = (d.software.generation, d.software.sampledAtNs, pendingNs, engines)
            }
        } else {
            previousBusy = nil
        }

        // ---- UMC MEMORY ACTIVITY: SMU field, else MMHUB PERFCTR delta ----
        var umcSource: String?
        if let smu = d.smuValue(.umcActivityPercent), smu <= 100 {
            umc = smu
            umcSource = "SMU UmcActivityPercent (firmware table offset 126; UMC busy 0-100%)"
        }
        if umc == nil {
            // MMHUB PERFSTATUS (build 198+, stage 15 only, observer cached).
            let m = d.mmhub
            if m.valid, d.stage == 15, m.status == 0,
               m.collectedAtNs <= now, now - m.collectedAtNs <= kSMUMetricsStaleAfterNs,
               let prev = previousUmc, prev.atNs < m.collectedAtNs, prev.q8 <= m.umcBusyQ8 {
                let elapsed = m.collectedAtNs - prev.atNs
                if elapsed >= 1_000_000_000 && elapsed <= 2_000_000_000 {
                    let ratio = Double(m.umcBusyQ8 - prev.q8) /
                                (Double(kMMHUBPerfStatusMaxQ8) * Double(elapsed) / 1e9) * 100.0
                    if ratio >= 0, ratio.isFinite {
                        umc = min(max(ratio, 0), 100)
                        umcSource = "MMHUB PERFSTATUS UMC busy (hardware PERFCTR delta, selector 68; 0-100%)"
                    }
                }
            }
        }
        previousUmc = d.mmhub.valid && d.stage == 15 && d.mmhub.status == 0
            ? (d.mmhub.umcBusyQ8, d.mmhub.collectedAtNs) : nil
        if let s = umcSource { lastUmhubSource = s }

        points.append(Point(timeNs: now, core: core, umc: umc))
        while !points.isEmpty,
              now > points[0].timeNs, now - points[0].timeNs >= Self.windowNs {
            points.removeFirst()
        }
        if points.count > Self.maxPoints { points.removeFirst(points.count - Self.maxPoints) }
    }

    // Per-engine dispatch-in-flight busy % for the GRBM strip. Must be
    // called before add() consumes the previous baseline.
    func enginePercent(_ d: Device, index: Int) -> Double? {
        guard d.softwareSupported, !d.software.saturated, index < 4 else { return nil }
        guard let prev = previousBusy,
              prev.generation == d.software.generation,
              prev.timeNs < d.software.sampledAtNs,
              d.software.engines[index].pendingNs >= prev.engines[index] else { return nil }
        let delta = d.software.engines[index].pendingNs - prev.engines[index]
        let elapsed = d.software.sampledAtNs - prev.timeNs
        guard elapsed > 0, elapsed <= 2_000_000_000 else { return nil }
        let ratio = Double(delta) / Double(elapsed) * 100.0
        guard ratio >= 0, ratio.isFinite else { return nil }
        return min(max(ratio, 0), 100)
    }

    func reset() {
        points.removeAll()
        previousBusy = nil
        previousUmc = nil
        lastUmhubSource = nil
    }
}

// Turns a freshly read Device into the render snapshot.

func makeSnapshot(device: Device?, history: SampleHistory,
                  umcDefaultLabel: String, engineValues: [Double?]?) -> TelemetrySnapshot {
    var snap = TelemetrySnapshot()
    let nowNs = device?.nowNs ?? clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    snap.coreLoad = history.series(\.core, nowNs: nowNs)
    snap.umcActivity = history.series(\.umc, nowNs: nowNs)
    snap.umcSourceLabel = history.lastUmhubSource ?? umcDefaultLabel
    snap.coreCurrent = history.coreCurrent
    if let e = device?.error {
        // Device is bound but this read failed: show the specific failure so
        // it is diagnosable. Distinct from "no device found" below.
        snap.error = "driver present, read failed: \(e)"
        if let d = device { snap.deviceLabel = d.label }
        return snap
    }
    guard let d = device else {
        snap.error = "No GPU bound to MacAMDGPU (device not found)"
        return snap
    }
    snap.deviceLabel = d.label
    snap.gfxVersion = "gfx\(d.gfxVersion.0).\(d.gfxVersion.1).\(d.gfxVersion.2)"
    snap.build = d.build
    snap.stage = d.stage
    // Idle / standby: driver bound and readable, stage 0, nothing in flight.
    // This is the normal at-rest state — the device is healthy, not missing.
    let inFlight = d.softwareSupported
        ? d.software.queuedPackets + d.software.activeQueues
        : (d.stage == 15 ? 1 : 0)
    if d.stage == 0, inFlight == 0, d.error == nil {
        snap.idle = true
    }
    if d.specValid {
        snap.specInfo = "\(d.specWords[12]) CUs / \(d.specWords[4]) SEs"
    }
    snap.softwareStatsStatus = d.softwareSupported ? "selector 61 active" : (d.softwareError ?? "build < 193")

    snap.vramUsedGiB = d.vramUsedGiB
    snap.vramTotalGiB = d.vramCapacityGiB
    snap.vramVisibleUsedGiB = d.vramVisibleUsedGiB
    snap.vramVisibleTotalGiB = d.vramVisibleCapacityGiB

    for (i, name) in ["GFX", "SOC", "MEMORY", "FABRIC"].enumerated() {
        snap.clocks.append(ClockRow(id: i, name: name,
                                    current: d.clockValue(index: i, kind: 0),
                                    minimum: d.clockValue(index: i, kind: 1),
                                    maximum: d.clockValue(index: i, kind: 2)))
    }

    // Per-engine strip: the driver exposes four dispatch engines (SDMA0,
    // SDMA1, GFX, AQL); VCN/JPEG have no counter in the observer surface.
    let engineNames = ["SDMA0 (DMA)", "SDMA1 (DMA)", "GFX (Graphics)", "AQL (Compute)"]
    for i in 0..<4 {
        let value = engineValues?[i]
        snap.engines.append(EngineRow(
            id: i, name: engineNames[i], value: value,
            note: value != nil ? "" :
                (i == 3 ? "no AQL in-flight sample window" :
                 i == 1 ? "SDMA1 not observed" : "no in-flight sample window yet")))
    }
    snap.engines.append(EngineRow(id: 4, name: "VCN (Video)", value: nil,
                                  note: "driver exposes no VCN counter"))
    snap.engines.append(EngineRow(id: 5, name: "JPEG", value: nil,
                                  note: "driver exposes no JPEG counter"))

    if let p = d.smuValue(.socketPowerMilliwatts) { snap.powerWatts = p / 1000.0 }
    if let p = d.smuValue(.boardPowerMilliwatts) { snap.boardPowerWatts = p / 1000.0 }
    if let t = d.smuValue(.edgeTemperatureMillicelsius) { snap.edgeCelsius = t / 1000.0 }
    if let t = d.smuValue(.hotspotTemperatureMillicelsius) { snap.hotspotCelsius = t / 1000.0 }
    if let f = d.smuValue(.fanRPM) { snap.fanRPM = f }
    return snap
}

// Formatting helpers shared by the SwiftUI views.

func fmt(_ value: Double?, _ places: Int = 1) -> String {
    guard let v = value, v.isFinite else { return "n/a" }
    return String(format: "%.\(places)f", v)
}

func fmtInt(_ value: Double?) -> String {
    guard let v = value, v.isFinite else { return "n/a" }
    return String(format: "%.0f", v)
}
