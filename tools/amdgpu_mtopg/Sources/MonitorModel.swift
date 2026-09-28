// amdgpu_mtopg — sample model and honest rate computation.
//
// Mirrors the terminal monitor's accounting: a value is published only when
// the underlying driver source actually produced it. In particular:
//  - GPU CORE LOAD uses rolling GRBM_STATUS.GUI_ACTIVE samples when the
//    driver provides selector 71. Older drivers fall back to the submitted-
//    packet-rate proxy from selector 61. Neither is CU occupancy.
//    SMU AverageGfxActivity reads ~100% at verified idle and is separate.
//  - UMC MEMORY ACTIVITY uses selector 68 only when it supplies a hardware
//    sample. It is unavailable on gfx1201; the uncalibrated SMU UCLK activity
//    field remains a labeled diagnostic rather than a memory-busy meter.
//
// MIT License — see the repository LICENSE.

import Foundation

// One rendered frame's data, computed on the sampler thread and consumed by
// SwiftUI on the main thread.

struct EngineRow: Identifiable {
    let id: Int
    let name: String
    let value: Double?      // submitted-packet rate scaled to observed peak
    let note: String        // provenance / why unavailable
}

struct ClockRow: Identifiable {
    let id: Int
    let name: String
    let current: Double?    // SMU GFX average, or raw CurrClock (MHz)
    let currentKind: String // "avg" for firmware average, "raw" for CurrClock
    let rawCurrent: Double? // unverified as an instantaneous operating clock
    let average: Double?    // firmware average, retained as a diagnostic
    let minimum: Double?
    let maximum: Double?
    let note: String?
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
    var coreHardware: Bool = false
    var umcActivity: [(age: Double, value: Double?)] = []
    var umcSourceLabel: String?   // label of the most recent plotted source
    // False when the plotted UMC number is a known-incoherent firmware field
    // (the SMU UmcActivityPercent average) rather than a true memory-busy
    // counter. The panel dims the readout + chart and adds a marker so a
    // user reading "UMC 0%" under load is not misled.
    var umcReliable: Bool = false
    var coreCurrent: Double?      // latest published sample
    // Real-hardware SQ busy % (aqlprofile SQ_BUSY_CYCLES via the LSE backend,
    // published into the shared GART slot, selector 69). nil = no source
    // registered / no fresh window yet; 0 = source says no work this window.
    var sqBusy: [(age: Double, value: Double?)] = []
    var sqBusyCurrent: Double?
    var sqBusySourceLabel: String?

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
    var unverifiedSmuSocketPowerWatts: Double?
    var unverifiedSmuBoardPowerWatts: Double?
    var powerDiagnostic: String?
    var smuGfxActivityPercent: Double?
    var unverifiedSmuGfxActivityPercent: Double?
    var smuGfxDiagnostic: String?
    var smuUclkActivityPercent: Double?
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
        var sqBusy: Double?
    }

    private(set) var points: [Point] = []
    // Previous GFX submission count and driver snapshot time. A generation
    // change or counter regression starts a new rate baseline.
    private var previousBusy: (generation: UInt64, timeNs: UInt64, gfxSubmitted: UInt64)?
    private var previousUmc: (q8: UInt64, atNs: UInt64)?
    private var grbmSamples: [(atNs: UInt64, active: Bool)] = []
    private let grbmWindowNs: UInt64 = 2_000_000_000
    private(set) var lastCoreIsHardware = false
    // Reference GFX dispatch rate (packets/sec) used to scale the "GPU load"
    // bar. It is a decaying peak: it rises quickly to match a fresh burst of
    // work (so a new decode calibrates within a second or two) but decays
    // slowly toward the current rate when work drops, so the bar tracks
    // CURRENT load and the number actually moves rather than freezing against
    // a frozen all-time high. The fallback meter is a dispatch-rate proxy,
    // not a busy %: there is no SQ busy-cycle counter on gfx1201 (SMU
    // GfxActivity is pinned at 100 while the GPU is awake; GFX pendingNs is
    // structurally 0 because the driver publishes GFX work to the ring with no
    // software-outstanding interval). The per-second rate of GFX packets
    // submitted (engine 2) remains a fallback on older driver builds.
    private var peakGfxRatePerSec: Double = 0
    // Last computed GPU-load value, forward-filled across the driver's ~1 Hz
    // sample repeats (the TUI polls at 10 Hz) so the chart is continuous.
    private var lastCoreValue: Double?
    // SQ busy % tracking: the slot publishes one window every ~500 ms (seq
    // advances); we convert each fresh window to busy % and hold it until the
    // next window. seq comparison handles driver clock gaps and restarts.
    private var lastSqSeq: UInt32 = 0
    private var lastSqValue: Double?
    private var lastSqNs: UInt64?
    var lastSqSource: String?  // which SQ source produced the held value (read by makeSnapshot)
    private let sqHoldMaxNs: UInt64 = 2_500_000_000  // ~5 windows of silence
    // Wall-clock time (nowNs) of the last fresh GPU-load sample; used to expire
    // the held value once the driver stops reporting new work.
    private var lastCoreNs: UInt64?
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
    // Provenance for the selected sampled-active or packet-rate source.
    var lastCoreSource: String?

    // The plotted window as (age-from-`nowNs`-seconds, value), oldest ->
    // newest. Ages come from the sample timestamps, not the array index, so
    // an irregular 10 Hz cadence still scrolls correctly.
    func series(_ key: KeyPath<Point, Double?>, nowNs: UInt64) -> [(age: Double, value: Double?)] {
        points.map { (age: Double(nowNs - $0.timeNs) / 1e9, value: $0[keyPath: key]) }
    }

    var coreCurrent: Double? { points.last?.core }
    var sqBusyCurrent: Double? { points.last?.sqBusy }

    func add(_ d: Device) {
        // A released owner session invalidates every activity source. Discard
        // the old chart immediately so stage 0 cannot display a held 0% load.
        if d.stage != 15 {
            reset()
            return
        }
        let now = d.nowNs
        var core: Double?
        var umc: Double?
        var sq: Double?
        lastCoreIsHardware = false

        // ---- SQ BUSY %: real hardware counter ----
        // Try the driver-owned SQ counter (selector 70), then the workload
        // slot (selector 69). Both are currently unavailable on this host;
        // keep the readout empty until either source produces a sample.
        if d.stage == 15, d.sqBusy.usable {
            lastSqValue = min(max(Double(d.sqBusy.pctX100) / 100.0, 0), 100)
            lastSqNs = now
            lastSqSource = "Driver-provided SQ busy-cycle delta (selector 70); hardware counter, not CU occupancy"
        } else if d.stage == 15, d.sqSlot.valid, d.sqSlot.seq != lastSqSeq {
            // Workload-owned aqlprofile slot (selector 69). busy % = sq_busy /
            // ref * 100, clamped; a fresh seq means a fresh window, so hold the
            // last value across the ~500 ms gap and expire it after ~2.5 s of
            // silence (the panel drops to a gap instead of a stale number).
            lastSqSeq = d.sqSlot.seq
            if d.sqSlot.refTicks > 0, d.sqSlot.sqBusy <= d.sqSlot.refTicks * 100 {
                let pct = Double(d.sqSlot.sqBusy) / Double(d.sqSlot.refTicks) * 100.0
                lastSqValue = min(max(pct, 0), 100)
            } else {
                lastSqValue = d.sqSlot.sqBusy > 0 ? 100.0 : 0.0
            }
            lastSqNs = now
            lastSqSource = "Real hardware SQ busy-cycle counter (aqlprofile SQ_BUSY_CYCLES, via the LSE workload process; all-CU sum over a ~0.5 s window) - NOT CU occupancy, NOT the dispatch-rate proxy"
        }
        if d.stage != 15 {
            lastSqValue = nil
            lastSqNs = nil
            lastSqSource = nil
        }
        if let held = lastSqValue, let heldNs = lastSqNs {
            if now - heldNs <= sqHoldMaxNs {
                sq = held
            } else {
                lastSqValue = nil
                lastSqNs = nil
                lastSqSource = nil
            }
        }

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
        // The fallback has no hardware busy-cycle denominator:
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
            previousBusy = (d.software.generation, sampledNow, gfxSubmitted)
            if let value = ratio {
                lastCoreValue = value
                lastCoreNs = now
                lastCoreSource = "GFX dispatch-rate proxy (adaptive scale); not a hardware busy percentage"
                core = value
            } else {
                // No fresh rate this poll (stale repeat, first sample, or the
                // driver re-sampled but the counter did not advance = idle).
                // Hold the last value across stale repeats so the line is
                // continuous at the driver's ~1 Hz rate, then SMOOTHLY DECAY
                // it toward 0 once the driver stops reporting new work (instead
                // of a hard drop to 0), so the chart eases down as a decode
                // finishes rather than spiking flat. The decay is exponential
                // in the staleness beyond the hold window (~0.4s time-constant),
                // so a just-finished decode reads near its last value and fully
                // settles to 0 within ~1.5s.
                if let held = lastCoreValue, let heldNs = lastCoreNs {
                    let stale = now - heldNs
                    if sawFreshData == false && stale <= coreHoldMaxNs {
                        core = held
                    } else {
                        // Beyond the hold window: ease the held value toward 0.
                        let overNs = UInt64(max(stale - coreHoldMaxNs, 0))
                        let decayTau: UInt64 = 400_000_000   // ~0.4 s time-constant
                        let factor = exp(-Double(overNs) / Double(decayTau))
                        let decayed = held * factor
                        core = decayed < 0.5 ? 0 : decayed
                        // Once effectively settled, release the held baseline so
                        // the next burst re-calibrates cleanly from 0.
                        if core == 0 { lastCoreValue = nil; lastCoreNs = nil }
                    }
                    if core != nil, lastCoreSource == nil {
                        lastCoreSource = "GFX dispatch-rate proxy (adaptive scale); not a hardware busy percentage"
                    }
                }
            }
        } else {
            previousBusy = nil
            lastCoreValue = nil
            lastCoreNs = nil
        }

        // GUI_ACTIVE is a hardware idle/active indication. Sample it at the
        // monitor's 10 Hz cadence and report the active-sample fraction over
        // the last two seconds. This approximates GFX active time; it does
        // not measure CU occupancy or productive shader cycles. A fresh
        // sample must advance the driver's timestamp. Older builds retain
        // the software dispatch-rate proxy computed above.
        if d.stage == 15, d.grbmStatus.valid,
           d.grbmStatus.sampledAtNs <= now,
           now - d.grbmStatus.sampledAtNs <= 500_000_000,
           grbmSamples.last.map({ $0.atNs < d.grbmStatus.sampledAtNs }) ?? true {
            grbmSamples.append((d.grbmStatus.sampledAtNs, d.grbmStatus.active))
        }
        grbmSamples.removeAll { sample in
            now < sample.atNs || now - sample.atNs > grbmWindowNs
        }
        if d.stage != 15 { grbmSamples.removeAll() }
        if grbmSamples.count >= 8 {
            let active = grbmSamples.reduce(0) { $0 + ($1.active ? 1 : 0) }
            core = Double(active) / Double(grbmSamples.count) * 100.0
            lastCoreIsHardware = true
            lastCoreSource = "GRBM_STATUS.GUI_ACTIVE (selector 71): active samples over a rolling 2 s window at up to 10 Hz; approximate GFX active time, not CU occupancy or shader busy cycles"
        } else if core != nil {
            lastCoreSource = "GFX submitted-packet rate (selector 61), scaled to the observed peak; workload activity proxy, not a hardware busy percentage"
        }

        // ---- UMC MEMORY ACTIVITY ----
        // The MMHUB PERFSTATUS hardware PERFCTR delta (selector 68, build 198+)
        // is preferred while it has produced a sample. The SMU average may
        // fill in on a qualified 0x2e table. Firmware interface 0x33 has
        // reported UMC activity at idle and zero under traffic on this host;
        // keep that decoded field diagnostic-only.
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
        if umc == nil, d.metrics.driverInterface == 0x2e,
           let smu = d.smuValue(.umcActivityPercent), smu <= 100 {
            umc = smu
            umcReliable = false
            umcSource = "SMU UmcActivityPercent (firmware table offset 126; UMC busy 0-100%) — unqualified firmware field on this host (moves at idle, 0 under traffic)"
        }
        previousUmc = mmhubValid ? (m.umcBusyQ8, m.collectedAtNs) : nil
        if umc != nil, let s = umcSource { lastUmhubSource = s }
        if umc != nil { lastUmhubReliable = umcReliable }

        points.append(Point(timeNs: now, core: core, umc: umc, sqBusy: sq))
        while !points.isEmpty,
              now > points[0].timeNs, now - points[0].timeNs >= Self.windowNs {
            points.removeFirst()
        }
        if points.count > Self.maxPoints { points.removeFirst(points.count - Self.maxPoints) }
    }

    // Per-engine packet-rate proxy for the strip, computed from the per-engine
    // submitted-packet rate (the pendingNs counters are flat for the GFX
    // engine on this driver, so the rate is the only per-engine signal that
    // tracks real work). Normalized against the peak per-engine rate seen so
    // far, matching the aggregate GPU-load meter.
    private var peakEngineRatePerSec: [Double] = [0, 0, 0, 0]
    private var previousEngineSubmitted: [UInt64]? = nil
    private var previousEngineTimeNs: UInt64? = nil
    private var previousEngineGeneration: UInt64? = nil

    func enginePercents(_ d: Device) -> [Double?] {
        guard d.stage == 15, d.softwareSupported, !d.software.saturated else {
            previousEngineSubmitted = nil
            previousEngineTimeNs = nil
            previousEngineGeneration = nil
            return Array(repeating: nil, count: 4)
        }
        let submitted = d.software.engines.map(\.submitted)
        defer {
            previousEngineSubmitted = submitted
            previousEngineTimeNs = d.software.sampledAtNs
            previousEngineGeneration = d.software.generation
        }
        guard let prevGen = previousEngineGeneration,
              prevGen == d.software.generation,
              let prevTime = previousEngineTimeNs,
              prevTime < d.software.sampledAtNs,
              let prevSub = previousEngineSubmitted,
              d.software.sampledAtNs - prevTime <= 2_000_000_000
        else { return Array(repeating: nil, count: 4) }
        let elapsed = d.software.sampledAtNs - prevTime
        return (0..<4).map { index in
            guard prevSub[index] <= submitted[index] else { return nil }
            let ratePerSec = Double(submitted[index] - prevSub[index]) / Double(elapsed) * 1e9
            guard ratePerSec > 0, ratePerSec.isFinite else { return nil }
            if ratePerSec > peakEngineRatePerSec[index] {
                peakEngineRatePerSec[index] = ratePerSec
            }
            guard peakEngineRatePerSec[index] > 0 else { return nil }
            return min(max(ratePerSec / peakEngineRatePerSec[index] * 100.0, 0.0), 100.0)
        }
    }

    func reset() {
        points.removeAll()
        previousBusy = nil
        previousUmc = nil
        grbmSamples.removeAll()
        lastCoreIsHardware = false
        lastUmhubSource = nil
        lastUmhubReliable = false
        lastCoreSource = nil
        lastSqSeq = 0
        lastSqValue = nil
        lastSqNs = nil
        peakGfxRatePerSec = 0
        lastCoreValue = nil
        lastCoreNs = nil
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
    snap.sqBusy = history.series(\.sqBusy, nowNs: nowNs)
    snap.sqBusyCurrent = history.sqBusyCurrent
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
    let sqBusyUnavailableLabel: String? = device.flatMap { d in
        // Panel is empty: no usable SQ-busy value. Explain why — on gfx1201
        // the driver-owned counter (sel 70) is unavailable (the SQ busy-cycle
        // counter is armed only by the closed aqlprofile CP-perfmon PM4, not a
        // driver GRBM read) and the workload slot (sel 69) is not registered.
        (d.build >= DriverABI.sqBusyMinimumBuild || d.build >= DriverABI.sqSlotMinimumBuild)
            ? "No live SQ-busy counter on gfx1201: the hardware SQ busy-cycle counter requires aqlprofile CP-perfmon PM4 (selector 70 reports unavailable), and the workload slot (selector 69) is not registered. GPU Load uses sampled GFX active time on driver build 203+, or a labeled packet-rate proxy on older builds."
            : nil
    }
    snap.sqBusySourceLabel = history.sqBusyCurrent != nil
        ? history.lastSqSource
        : sqBusyUnavailableLabel
    var umcLabel = history.lastUmhubSource ?? umcDefaultLabel
    if let d = device, d.stage == 15, d.metrics.driverInterface == 0x33,
       let decoded = d.smuValue(.umcActivityPercent) {
        umcLabel = "SMU UCLK activity (0x2e layout, 0x33 uncalibrated): \(fmt(decoded, 0))%; no hardware UMC busy counter."
    } else if mmhubDead {
        umcLabel = hwAvailable
            ? "MMHUB PERFSTATUS UMC busy (hardware PERFCTR delta, selector 68; 0-100%)"
            : "UNRELIABLE — SMU UmcActivityPercent firmware average (offset 126); NOT a memory-busy counter. MMHUB PERFSTATUS (selector 68, offset 0x04c18) does not exist in the RDNA4 register map - driver build 199+ reports it unavailable (pre-199 read back 0xFFFFFFFF idle + load), so there is no hardware UMC-busy counter on this ASIC."
    }
    snap.umcSourceLabel = umcLabel
    snap.coreCurrent = history.coreCurrent
    snap.coreSourceLabel = history.lastCoreSource
    snap.coreHardware = history.lastCoreIsHardware
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
        snap.specInfo = "\(d.specWords[9]) CUs / \(d.specWords[1]) SEs"
    }
    snap.softwareStatsStatus = d.softwareSupported ? "selector 61 active" : (d.softwareError ?? "build < 193")

    snap.vramUsedGiB = d.vramUsedGiB
    snap.vramTotalGiB = d.vramCapacityGiB
    snap.vramVisibleUsedGiB = d.vramVisibleUsedGiB
    snap.vramVisibleTotalGiB = d.vramVisibleCapacityGiB

    let clockFields: [SMUField] = [.gfxClockMHz, .socClockMHz,
                                   .memoryClockMHz, .fabricClockMHz]
    for (i, name) in ["GFX", "SOC", "MEMORY", "FABRIC"].enumerated() {
        let selected = d.stage == 15 ? d.smuValue(clockFields[i]) : nil
        let raw = d.stage == 15 ? d.clockValue(index: i, kind: 0) : nil
        let maximum = d.clockValue(index: i, kind: 2)
        // The firmware's selected UCLK average can read ~2517 MHz while its
        // own advertised AC maximum is 1258 MHz and raw CurrClock is 1258.
        // Keep incompatible averages out of the SOC/MEMORY/FABRIC headlines;
        // a GFX average remains a labeled SMU field even if it conflicts.
        let selectedUsable: Bool
        if let selected {
            selectedUsable = maximum.map { selected <= $0 * 1.05 } ?? true
        } else {
            selectedUsable = false
        }
        // Keep the fresh GFX average visible at idle and under load. On the
        // 0x33 firmware it is a decoded SMU field, not a calibrated current
        // core clock; the raw CurrClock remains visible on the next line.
        let useAverage = i == 0 && selected != nil &&
            (d.metrics.driverInterface == 0x2e || d.metrics.driverInterface == 0x33)
        let note: String?
        if i == 0 && selected != nil && d.metrics.driverInterface == 0x33 {
            note = selectedUsable
                ? "IF 0x33; uncalibrated"
                : "SMU avg exceeds DPM max; IF 0x33"
        } else if selected != nil && !selectedUsable {
            note = "firmware average conflicts with DPM MHz"
        } else {
            note = nil
        }
        snap.clocks.append(ClockRow(id: i, name: name,
                                    current: useAverage ? selected : raw,
                                    currentKind: useAverage ? "avg" : "raw",
                                    rawCurrent: raw,
                                    average: selected,
                                    minimum: d.clockValue(index: i, kind: 1),
                                    maximum: maximum,
                                    note: note))
    }

    // Per-engine strip: the driver exposes four dispatch engines (SDMA0,
    // SDMA1, GFX, AQL); VCN/JPEG have no counter in the observer surface.
    let engineNames = ["SDMA0 (DMA)", "SDMA1 (DMA)", "GFX (Graphics)", "AQL (Compute)"]
    for i in 0..<4 {
        let value = engineValues?[i]
        snap.engines.append(EngineRow(
            id: i, name: engineNames[i], value: value,
            note: value != nil ? "" :
                (i >= 2
                    ? "selector 61 saw no packets; HSA dispatches may be outside this counter"
                    : "selector 61 saw no packets in this window")))
    }
    snap.engines.append(EngineRow(id: 4, name: "VCN (Video)", value: nil,
                                  note: "driver exposes no VCN counter"))
    snap.engines.append(EngineRow(id: 5, name: "JPEG", value: nil,
                                  note: "driver exposes no JPEG counter"))

    let socketPower = (d.stage == 15 ? d.smuValue(.socketPowerMilliwatts) : nil).map { $0 / 1000.0 }
    let boardPower = (d.stage == 15 ? d.smuValue(.boardPowerMilliwatts) : nil).map { $0 / 1000.0 }
    if d.metrics.driverInterface == 0x2e {
        snap.powerWatts = socketPower
        snap.boardPowerWatts = boardPower
    } else if d.metrics.driverInterface == 0x33,
              socketPower != nil || boardPower != nil {
        snap.unverifiedSmuSocketPowerWatts = socketPower
        snap.unverifiedSmuBoardPowerWatts = boardPower
        snap.powerDiagnostic = "Unverified SMU fields: socket \(fmt(socketPower, 0)) W, board avg \(fmt(boardPower, 0)) W"
    }
    if d.stage == 15, let activity = d.smuValue(.gfxActivityPercent) {
        if d.metrics.driverInterface == 0x2e {
            snap.smuGfxActivityPercent = activity
        } else if d.metrics.driverInterface == 0x33 {
            snap.unverifiedSmuGfxActivityPercent = activity
            snap.smuGfxDiagnostic = "Unverified SMU activity field: \(fmt(activity, 0))%"
        }
    }
    if d.stage == 15 {
        snap.smuUclkActivityPercent = d.smuValue(.umcActivityPercent)
    }
    if d.stage == 15, let t = d.smuValue(.edgeTemperatureMillicelsius) { snap.edgeCelsius = t / 1000.0 }
    if d.stage == 15, let t = d.smuValue(.hotspotTemperatureMillicelsius) { snap.hotspotCelsius = t / 1000.0 }
    if d.stage == 15, let f = d.smuValue(.fanRPM) { snap.fanRPM = f }
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
