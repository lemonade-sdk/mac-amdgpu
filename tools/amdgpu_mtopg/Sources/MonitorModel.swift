// amdgpu_mtopg — sample model and honest rate computation.
//
// Mirrors the terminal monitor's accounting: a value is published only when
// the underlying driver source actually produced it. In particular:
//  - GPU CORE LOAD is the delta of the driver dispatch-in-flight union
//    counter (software_stats selector 61). The SMU AverageGfxActivity field
//    is incoherent on this host (reads ~100% at verified idle) and is never
//    used for the core-load chart.
//  - UMC MEMORY ACTIVITY matches the terminal monitor: the MMHUB PERFSTATUS
//    hardware PERFCTR delta (selector 68, driver build 198+) is preferred
//    when it has produced a sample; otherwise the SMU UmcActivityPercent
//    field is shown when fresh and plausible (0..100). That field lives in
//    the same unqualified SMU table as AverageGfxActivity and is known
//    incoherent on this host (it moves at verified idle and reads 0% under
//    real traffic, per the build 193 idle/load capture in
//    docs/GPU_MONITOR.md), so its caption says so. On a pre-198 build it is
//    the only UMC source that exists; a permanently blank chart would hide a
//    real firmware number, so it is plotted with the honest caption.
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
    var coreSourceLabel: String?   // provenance of the plotted "GPU load" value
    var umcActivity: [(age: Double, value: Double?)] = []
    var umcSourceLabel: String?   // label of the most recent plotted source
    // False when the plotted UMC number is a known-incoherent firmware field
    // (the SMU UmcActivityPercent average) rather than a true memory-busy
    // counter. The panel dims the readout + chart and adds a marker so a
    // user reading "UMC 0%" under load is not misled.
    var umcReliable: Bool = false
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
    // previousBusy carries publishedPackets because the driver can regress it
    // between samples (queue-observer retire, see add()); the baseline must
    // reject the sample rather than re-baseline, matching the reference TUI.
    private var previousBusy: (generation: UInt64, timeNs: UInt64, published: UInt64, pendingNs: UInt64, engines: [UInt64], gfxSubmitted: UInt64)?
    private var lastPublishedNs: (timeNs: UInt64, generation: UInt64, pendingNs: UInt64, engines: [UInt64])?
    private var previousUmc: (q8: UInt64, atNs: UInt64)?
    // Reference GFX dispatch rate (packets/sec) used to scale the "GPU load"
    // bar. It is a decaying peak: it rises quickly to match a fresh burst of
    // work (so a new decode calibrates within a second or two) but decays
    // slowly toward the current rate when work drops, so the bar tracks
    // CURRENT load and the number actually moves rather than freezing against
    // a frozen all-time high. The GPU load meter is a dispatch-rate proxy,
    // not a busy %: there is no working busy counter on gfx1201 (SMU
    // GfxActivity is pinned at 100 while the GPU is awake; GFX pendingNs is
    // structurally 0 because the driver publishes GFX work to the ring with no
    // software-outstanding interval). The only signal that reliably tracks real
    // GFX compute work is the per-second rate of GFX packets submitted (eng2 /
    // the GFX engine). The caption states the scale is adaptive, not a fixed
    // busy percentage, so it is not read as CU occupancy.
    private var peakGfxRatePerSec: Double = 0
    // Last computed GPU-load value, forward-filled across the driver's ~1 Hz
    // sample repeats (the TUI polls at 10 Hz) so the chart is continuous.
    private var lastCoreValue: Double?
    // Wall-clock time (nowNs) of the last fresh GPU-load sample; used to expire
    // the held value once the driver stops reporting new work.
    private var lastCoreNs: UInt64?
    // Driver sample time of the last work advance (informational / for expiry).
    private var lastWorkNs: UInt64 = 0
    // Consecutive below-peak samples before the reference eases down. During a
    // steady decode the per-sample rate fluctuates, so we require a sustained
    // run of low samples (not a single one) before decaying the peak, keeping a
    // saturated decode reading near 100 instead of deflating to ~50.
    private var gfxLowStreak: Int = 0
    private let gfxLowStreakThreshold: Int = 3
    // How long (wall clock) to hold the last GPU-load value after the driver
    // stops reporting new work before it decays to 0. ~1.5s is long enough to
    // bridge the driver's 1 Hz sample gap, short enough that a finished decode
    // drops to 0 promptly instead of staying stuck.
    private let coreHoldMaxNs: UInt64 = 1_500_000_000
    // When the current rate is below the reference, decay the reference toward
    // it by this fraction each sample (a ~1 Hz driver cadence, so 0.05 gives a
    // ~20-sample / ~20s time-constant). When the current rate exceeds it, the
    // reference jumps up to the current rate immediately (fast calibration).
    private let gfxRateDecay: Double = 0.05

    var lastUmhubSource: String?
    var lastUmhubReliable: Bool = false
    // Honest provenance for the plotted "GPU load" value. It is a GFX
    // dispatch-rate proxy (auto-scaled to the peak rate observed so far),
    // NOT a busy %: on gfx1201 there is no working hardware busy counter
    // (SMU GfxActivity is pinned at 100 while the GPU is awake; GFX pendingNs
    // is structurally 0). Stating this in the panel keeps the meter from
    // being read as CU occupancy or productive-workload utilization.
    var lastCoreSource: String?

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

        // The driver's selector 61 handler calls observe() on every AQL queue
        // before returning the snapshot. Queues whose read index can no longer
        // be sampled (owner gone, BAR remap, ...) retire their pending
        // publishes into retiredPackets and clear published, so a single
        // unsampleable queue makes publishedPackets REGRESS between samples.
        // The driver's own validation accepts the snapshot, and the reference
        // terminal TUI (amdgpu_mtop main.cpp busyPercent) gates only on the
        // pendingNs counters, so it still plots core load. The Swift model
        // used publishedPackets as its baseline key, which meant the union
        // baseline reset after every regression and a 2 s window never
        // completed: the GPU Core Load chart and all four engine rows stayed
        // blank. Key on generation + sampledAtNs + counters, same as the
        // reference; a true counter regression (generation change, retired
        // in-flight work) is caught by the existing monotonicity checks.
        // ---- GPU LOAD: GFX dispatch-rate proxy (selector 61) ----
        // On gfx1201 there is no working "GPU busy %" hardware counter:
        // the SMU GfxActivityPercent field is pinned at 100 whenever the GPU
        // is awake (incoherent as a load meter), and the GFX engine's
        // pendingNs is structurally 0 because the driver publishes GFX work
        // to the ring with no software-outstanding interval (verified: GFX
        // pendingNs stayed 0 while publishedPackets advanced ~100k/sample and
        // GFX submitted climbed ~6.9M/sample during a real decode). The only
        // signal that reliably tracks real GFX compute work is the per-second
        // rate at which GFX packets are submitted. We compute that rate and
        // normalize it against the peak rate seen so far, so a saturated
        // decode reads ~100 and idle reads 0. The caption states the scale is
        // adaptive-to-peak, not a fixed busy percentage, so it is not read as
        // CU occupancy.
        if d.softwareSupported, !d.software.saturated {
            let gfxSubmitted = d.software.engines[2].submitted  // Engine::GFX
            var ratio: Double?
            let sampledNow = d.software.sampledAtNs
            // The driver re-samples the software stats at its own ~1 Hz cadence,
            // but the TUI polls at 10 Hz. Several consecutive polls return the
            // SAME cached snapshot (identical sampledAtNs). Only an advancing
            // sampledAtNs carries new counter data. On a real advance compute
            // the fresh rate; otherwise hold/decay the last value (see below).
            var sawFreshData = false
            if let prev = previousBusy,
               prev.generation == d.software.generation,
               prev.timeNs < sampledNow,
               prev.gfxSubmitted <= gfxSubmitted,
               sampledNow - prev.timeNs <= 2_000_000_000 {
                let delta = gfxSubmitted - prev.gfxSubmitted
                let elapsed = sampledNow - prev.timeNs
                if elapsed > 0 {
                    let ratePerSec = Double(delta) / Double(elapsed) * 1e9   // packets / sec
                    if ratePerSec > 0, ratePerSec.isFinite {
                        sawFreshData = true
                        lastWorkNs = sampledNow
                        // The reference tracks the peak rate. It rises
                        // instantly on a new burst (fast calibration). It only
                        // decays when the current rate stays below it for a
                        // sustained run of samples, NOT on every low sample:
                        // during a steady decode the per-sample rate fluctuates
                        // (a decode step may land between 1 Hz samples), and
                        // decaying on each low sample deflated the reference
                        // mid-decode, making a saturated run read ~50 instead
                        // of ~100. We require `gfxLowStreak` consecutive
                        // below-peak samples before the reference eases down.
                        if ratePerSec > peakGfxRatePerSec {
                            peakGfxRatePerSec = ratePerSec
                            gfxLowStreak = 0
                        } else {
                            gfxLowStreak += 1
                            if gfxLowStreak >= gfxLowStreakThreshold {
                                peakGfxRatePerSec += (ratePerSec - peakGfxRatePerSec) * gfxRateDecay
                            }
                        }
                        if peakGfxRatePerSec > 0 {
                            ratio = min(max(ratePerSec / peakGfxRatePerSec * 100.0, 0.0), 100.0)
                        }
                    }
                }
            }
            previousBusy = (d.software.generation, sampledNow,
                            d.software.publishedPackets, 0, [0, 0, 0, 0], gfxSubmitted)
            if let value = ratio {
                lastCoreValue = value
                lastCoreNs = now
                lastCoreSource = "GFX dispatch-rate proxy (adaptive scale, decays toward current) - tracks real GFX compute work; NOT a busy % (no working busy counter on gfx1201)"
                core = value
            } else {
                // No fresh rate this poll (stale repeat, first sample, or the
                // driver re-sampled but the counter did not advance = idle).
                // Hold the last value across stale repeats so the line is
                // continuous at the driver's ~1 Hz rate, but EXPIRE it once the
                // driver stops reporting new work: after `coreHoldMaxNs` with no
                // fresh data the held value decays to 0, so the bar drops when
                // the decode finishes instead of staying stuck at the last %.
                if let held = lastCoreValue, let heldNs = lastCoreNs {
                    let stale = now - heldNs
                    if sawFreshData == false && stale <= coreHoldMaxNs {
                        core = held
                    } else if stale > coreHoldMaxNs {
                        // Expired: no new work for > the hold window -> 0.
                        core = 0
                        lastCoreValue = nil
                        lastCoreNs = nil
                    }
                    if core != nil, lastCoreSource == nil {
                        lastCoreSource = "GFX dispatch-rate proxy (adaptive scale, decays toward current) - tracks real GFX compute work; NOT a busy % (no working busy counter on gfx1201)"
                    }
                }
            }
        } else {
            previousBusy = nil
            lastCoreValue = nil
            lastCoreNs = nil
        }

        // ---- UMC MEMORY ACTIVITY: same source priority as the terminal TUI ----
        // The MMHUB PERFSTATUS hardware PERFCTR delta (selector 68, build 198+)
        // is preferred while it has produced a sample. Otherwise the SMU
        // UmcActivityPercent field (firmware offset 126) is shown when fresh
        // and plausible: it is the only UMC source that exists on a pre-198
        // build, and although it sits in the unqualified SMU table (it moves
        // at verified idle and reads 0% under real traffic on this host), it
        // is a real firmware number — plotted with the honest caption, exactly
        // as the terminal monitor does.
        var umcSource: String?
        var umcReliable = false
        let m = d.mmhub
        // usable() additionally rejects an all-ones raw PERFSTATUS register
        // (unmapped MMIO; see MmhubPerfStatus.usable), which the driver
        // reports with status 0 on the R9700.
        let mmhubValid = m.usable && d.stage == 15 &&
            m.collectedAtNs <= now && now - m.collectedAtNs <= kSMUMetricsStaleAfterNs
        if mmhubValid,
           let prev = previousUmc, prev.atNs < m.collectedAtNs, prev.q8 <= m.umcBusyQ8 {
            let elapsed = m.collectedAtNs - prev.atNs
            if elapsed >= 1_000_000_000 && elapsed <= 2_000_000_000 {
                let ratio = Double(m.umcBusyQ8 - prev.q8) /
                            (Double(kMMHUBPerfStatusMaxQ8) * Double(elapsed) / 1e9) * 100.0
                if ratio >= 0, ratio.isFinite {
                    umc = min(max(ratio, 0), 100)
                    umcReliable = true
                    umcSource = "MMHUB PERFSTATUS UMC busy (hardware PERFCTR delta, selector 68; 0-100%)"
                }
            }
        }
        if umc == nil, let smu = d.smuValue(.umcActivityPercent), smu <= 100 {
            // SMU fallback, matching the terminal TUI exactly: on a pre-198
            // build this is the only UMC source that exists; on 198+ it
            // bridges the first second while the MMHUB delta has no window
            // yet. It is a real firmware number, just from the unqualified
            // SMU table (moves at verified idle, reads 0 under traffic on
            // this host), so the caption says so.
            umc = smu
            umcReliable = false
            umcSource = "SMU UmcActivityPercent (firmware table offset 126; UMC busy 0-100%) — unqualified firmware field on this host (moves at idle, 0 under traffic)"
        }
        previousUmc = mmhubValid ? (m.umcBusyQ8, m.collectedAtNs) : nil
        if umc != nil, let s = umcSource { lastUmhubSource = s }
        if umc != nil { lastUmhubReliable = umcReliable }

        points.append(Point(timeNs: now, core: core, umc: umc))
        while !points.isEmpty,
              now > points[0].timeNs, now - points[0].timeNs >= Self.windowNs {
            points.removeFirst()
        }
        if points.count > Self.maxPoints { points.removeFirst(points.count - Self.maxPoints) }
    }

    // Per-engine dispatch-in-flight busy % for the GRBM strip. Must be
    // called before add() consumes the previous baseline.
    // Per-engine busy proxy for the GRBM strip, computed from the per-engine
    // submitted-packet rate (the pendingNs counters are flat for the GFX
    // engine on this driver, so the rate is the only per-engine signal that
    // tracks real work). Normalized against the peak per-engine rate seen so
    // far, matching the aggregate GPU-load meter.
    private var peakEngineRatePerSec: [Double] = [0, 0, 0, 0]
    private var previousEngineSubmitted: [UInt64]? = nil
    private var previousEngineTimeNs: UInt64? = nil
    private var previousEngineGeneration: UInt64? = nil

    func enginePercent(_ d: Device, index: Int) -> Double? {
        guard d.softwareSupported, !d.software.saturated, index < 4 else { return nil }
        let submitted = d.software.engines[index].submitted
        defer {
            previousEngineSubmitted = Array(d.software.engines.map { $0.submitted })
            previousEngineTimeNs = d.software.sampledAtNs
            previousEngineGeneration = d.software.generation
        }
        guard let prevGen = previousEngineGeneration,
              prevGen == d.software.generation,
              let prevTime = previousEngineTimeNs,
              prevTime < d.software.sampledAtNs,
              let prevSub = previousEngineSubmitted,
              prevSub[index] <= submitted,
              d.software.sampledAtNs - prevTime <= 2_000_000_000 else { return nil }
        let delta = submitted - prevSub[index]
        let elapsed = d.software.sampledAtNs - prevTime
        guard elapsed > 0 else { return nil }
        let ratePerSec = Double(delta) / Double(elapsed) * 1e9
        guard ratePerSec > 0, ratePerSec.isFinite else { return nil }
        if ratePerSec > peakEngineRatePerSec[index] { peakEngineRatePerSec[index] = ratePerSec }
        guard peakEngineRatePerSec[index] > 0 else { return nil }
        return min(max(ratePerSec / peakEngineRatePerSec[index] * 100.0, 0.0), 100.0)
    }

    func reset() {
        points.removeAll()
        previousBusy = nil
        lastPublishedNs = nil
        previousUmc = nil
        lastUmhubSource = nil
        lastUmhubReliable = false
        lastCoreSource = nil
        peakGfxRatePerSec = 0
        lastCoreValue = nil
        lastCoreNs = nil
        lastWorkNs = 0
        gfxLowStreak = 0
        peakEngineRatePerSec = [0, 0, 0, 0]
        previousEngineSubmitted = nil
        previousEngineTimeNs = nil
        previousEngineGeneration = nil
    }
}

// Turns a freshly read Device into the render snapshot.

func makeSnapshot(device: Device?, history: SampleHistory,
                  umcDefaultLabel: String, engineValues: [Double?]?) -> TelemetrySnapshot {
    var snap = TelemetrySnapshot()
    let nowNs = device?.nowNs ?? clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    snap.coreLoad = history.series(\.core, nowNs: nowNs)
    snap.umcActivity = history.series(\.umc, nowNs: nowNs)
    // Reliability: a plotted UMC value is trustworthy only when it came from
    // the MMHUB hardware PERFCTR delta. On this ASIC (gfx1201 / RDNA4) the
    // MMHUB "PERFSTATUS" register the driver reads (selector 68, offset
    // 0x04c18) does not exist in the RDNA4 register map — it sits inside a
    // contiguous run of defined VM/steering registers — so the read always
    // returns the unmapped-register default 0xFFFFFFFF. The fallback, the
    // SMU UmcActivityPercent firmware average, is uncalibrated: it moves at
    // verified idle and reads 0 under real traffic (the opposite of a memory
    // busy counter). State both facts rather than implying the MMHUB source
    // is merely not ready yet.
    // The driver reports the MMHUB UMC-busy source unavailable (status != 0,
    // driver build 199+) on this ASIC, or - on the pre-199 binary - the
    // fabricated 0x04c18 register read back 0xFFFFFFFF. Either way: there is
    // no hardware UMC-busy counter on gfx1201, so surface the SMU fallback and
    // say so plainly rather than implying the MMHUB source is merely not ready.
    let mmhubDead = device.map { d in
        d.stage == 15 && (d.mmhub.status != 0 || (d.mmhub.valid && d.mmhub.raw == 0xFFFFFFFF))
    } ?? false
    let hwAvailable = history.lastUmhubReliable
    snap.umcReliable = hwAvailable && !mmhubDead
    var umcLabel = history.lastUmhubSource ?? umcDefaultLabel
    if mmhubDead {
        umcLabel = hwAvailable
            ? "MMHUB PERFSTATUS UMC busy (hardware PERFCTR delta, selector 68; 0-100%)"
            : "UNRELIABLE — SMU UmcActivityPercent firmware average (offset 126); NOT a memory-busy counter. MMHUB PERFSTATUS (selector 68, offset 0x04c18) does not exist in the RDNA4 register map - driver build 199+ reports it unavailable (pre-199 read back 0xFFFFFFFF idle + load), so there is no hardware UMC-busy counter on this ASIC."
    }
    snap.umcSourceLabel = umcLabel
    snap.coreCurrent = history.coreCurrent
    snap.coreSourceLabel = history.lastCoreSource
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
