// amdgpu_mtopg — SwiftUI 2D views.
//
// Real vector drawing: everything chart-like is a SwiftUI Canvas (Core
// Graphics under the hood). No braille, no block characters.
//
// MIT License — see the repository LICENSE.

import AppKit
import SwiftUI

// Esc quits the app (window close does the same via the delegate).
extension View {
    func escToQuit() -> some View {
        self
            .background(NSViewRepresentableBridge())
    }
}

struct NSViewRepresentableBridge: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let v = EscCatcher()
        context.coordinator.monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            if event.keyCode == 53 {   // Esc
                NSApp.terminate(nil)
                return nil
            }
            return event
        }
        return v
    }
    func updateNSView(_ nsView: NSView, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator() }
    final class Coordinator { var monitor: Any? }
    static func dismantleNSView(_ nsView: NSView, coordinator: Coordinator) {
        if let m = coordinator.monitor { NSEvent.removeMonitor(m) }
    }
}

// Esc key catcher: SwiftUI's onExitCommand is unreliable in a background-
// launched app (the SwiftUI window isn't first responder), so a local NSEvent
// monitor plus a focusable NSView make Esc quit the app.
final class EscCatcher: NSView {
    override var acceptsFirstResponder: Bool { true }
    override func becomeFirstResponder() -> Bool { true }
}

// MARK: - Palette (dark, btop-style)

enum Palette {
    static let background = Color(red: 0.055, green: 0.07, blue: 0.09)
    static let panel = Color(red: 0.086, green: 0.106, blue: 0.133)
    static let border = Color(red: 0.23, green: 0.27, blue: 0.33)
    static let grid = Color.white.opacity(0.08)
    static let axis = Color.white.opacity(0.45)
    static let dim = Color.white.opacity(0.55)
    static let bright = Color.white.opacity(0.92)
    static let accent = Color(red: 0.35, green: 0.85, blue: 0.95)   // core load cyan
    static let accentFill = Color(red: 0.35, green: 0.85, blue: 0.95).opacity(0.16)
    static let umc = Color(red: 0.55, green: 0.85, blue: 0.45)      // UMC green
    static let umcFill = Color(red: 0.55, green: 0.85, blue: 0.45).opacity(0.14)
    static let sq = Color(red: 0.80, green: 0.55, blue: 0.95)        // SQ busy violet
    static let sqFill = Color(red: 0.80, green: 0.55, blue: 0.95).opacity(0.14)
    static let warm = Color(red: 0.95, green: 0.62, blue: 0.35)
}

// MARK: - Time-series chart (Canvas)

/// A rolling-window line/area chart with grid, Y-axis % labels and an X-axis
/// age label. `samples` is (age-in-seconds-from-now, value) oldest ->
/// newest; nil values render as gaps. The chart is time-aligned: each point
/// is placed by its age so the right edge is "now" and the window scrolls
/// left.
struct TimeSeriesChart: View {
    let samples: [(age: Double, value: Double?)]
    let windowSeconds: Double
    let color: Color
    let fill: Color
    let hasData: Bool
    let emptyCaption: String

    var body: some View {
        Canvas { context, size in
            let leftMargin: CGFloat = 38
            let bottomMargin: CGFloat = 18
            let topMargin: CGFloat = 8
            let plot = CGRect(x: leftMargin, y: topMargin,
                              width: size.width - leftMargin - 8,
                              height: size.height - topMargin - bottomMargin)
            guard plot.width > 20, plot.height > 20 else { return }

            // Grid: horizontal lines at 0/25/50/75/100%.
            for fraction in [0.0, 0.25, 0.5, 0.75, 1.0] {
                let y = plot.minY + plot.height * CGFloat(1.0 - fraction)
                var line = Path()
                line.move(to: CGPoint(x: plot.minX, y: y))
                line.addLine(to: CGPoint(x: plot.maxX, y: y))
                context.stroke(line, with: .color(fraction == 0 ? Palette.axis : Palette.grid),
                               lineWidth: fraction == 0 ? 1 : 0.5)
                let label = Text("\(Int(fraction * 100))%")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Palette.dim)
                context.draw(context.resolve(label),
                             at: CGPoint(x: plot.minX - 5, y: y), anchor: .trailing)
            }
            // X-axis labels: now, -15s, -30s, -45s, -60s (proportional).
            let xLabels: [(Double, String)] = [(0, "now"),
                                                (windowSeconds * 0.25, "\(Int(windowSeconds * 0.25))s ago"),
                                                (windowSeconds * 0.5, "\(Int(windowSeconds * 0.5))s ago"),
                                                (windowSeconds * 0.75, "\(Int(windowSeconds * 0.75))s ago"),
                                                (windowSeconds, "\(Int(windowSeconds))s ago")]
            for (age, text) in xLabels {
                let x = plot.maxX - plot.width * CGFloat(age / windowSeconds)
                var tick = Path()
                tick.move(to: CGPoint(x: x, y: plot.maxY))
                tick.addLine(to: CGPoint(x: x, y: plot.maxY + 3))
                context.stroke(tick, with: .color(Palette.axis), lineWidth: 0.5)
                let label = Text(text)
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Palette.dim)
                let anchor: UnitPoint = age == 0 ? .bottomLeading : (age == windowSeconds ? .bottomTrailing : .bottom)
                context.draw(context.resolve(label),
                             at: CGPoint(x: x, y: size.height - 4), anchor: anchor)
            }

            guard hasData else { return }

            // Collect the real (in-window, finite) points in chronological
            // order (oldest first) with their x/y. We then bridge SMALL time
            // gaps by interpolation and smooth the result with a Catmull-Rom
            // spline, so the trace reads as one continuous curve instead of a
            // jagged line full of empty spots. LARGE gaps (real idle stretches,
            // or a source that stops) still break the line so we don't draw a
            // false segment across a period with no data.
            let pts: [(x: CGFloat, y: CGFloat, age: Double)] = samples.compactMap { (age, value) in
                guard let v = value, v.isFinite, age <= windowSeconds else { return nil }
                let x = plot.maxX - plot.width * CGFloat(age / windowSeconds)
                let y = plot.minY + plot.height * CGFloat(1.0 - min(max(v / 100.0, 0.0), 1.0))
                return (x, y, age)
            }
            .sorted { $0.age > $1.age }   // oldest (largest age) first -> left to right

            // A gap larger than this (in seconds) breaks the line; smaller gaps
            // are interpolated. ~3s is well above the driver's ~1 Hz cadence and
            // the ~2.5s sample-expiry, so a finished decode still breaks cleanly
            // but normal sampling jitter is bridged.
            let maxGap: Double = 3.0
            // Split into contiguous runs separated by > maxGap.
            var runs: [[(x: CGFloat, y: CGFloat, age: Double)]] = []
            var cur: [(x: CGFloat, y: CGFloat, age: Double)] = []
            for p in pts {
                if let lastAge = cur.last?.age, (lastAge - p.age) > maxGap {
                    if cur.count >= 2 { runs.append(cur) } else if cur.count == 1 { runs.append(cur) }
                    cur = []
                }
                cur.append(p)
            }
            if cur.count >= 1 { runs.append(cur) }

            var path = Path()
            var area = Path()
            for run in runs {
                // Single point: draw a dot so an isolated sample isn't lost.
                if run.count == 1 {
                    let p = run[0]
                    path.move(to: CGPoint(x: p.x, y: p.y))
                    path.addLine(to: CGPoint(x: p.x + 0.01, y: p.y))
                    continue
                }
                // Build a smoothed Catmull-Rom curve through the run's points.
                // Each interior point contributes a cubic segment to the next.
                let c = run.map { CGPoint(x: $0.x, y: $0.y) }
                var segStart = c[0]
                path.move(to: segStart)
                area.move(to: CGPoint(x: c[0].x, y: plot.maxY))
                area.addLine(to: segStart)
                for i in 0..<(c.count - 1) {
                    let p0 = (i > 0) ? c[i - 1] : c[i]
                    let p1 = c[i]
                    let p2 = c[i + 1]
                    let p3 = (i + 2 < c.count) ? c[i + 2] : c[i + 1]
                    // Catmull-Rom -> cubic Bezier control points (tension 0.5).
                    let c1 = CGPoint(x: p1.x + (p2.x - p0.x) / 6.0, y: p1.y + (p2.y - p0.y) / 6.0)
                    let c2 = CGPoint(x: p2.x - (p3.x - p1.x) / 6.0, y: p2.y - (p3.y - p1.y) / 6.0)
                    path.move(to: p1)
                    path.addCurve(to: p2, control1: c1, control2: c2)
                    area.addCurve(to: p2, control1: c1, control2: c2)
                    segStart = p2
                }
                area.addLine(to: CGPoint(x: segStart.x, y: plot.maxY))
                area.closeSubpath()
            }
            context.fill(area, with: .color(fill))
            context.stroke(path, with: .color(color),
                           style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round))
        }
        .overlay {
            if !hasData {
                GeometryReader { geometry in
                    Text(emptyCaption)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(Palette.dim)
                        .multilineTextAlignment(.center)
                        .lineLimit(3)
                        .frame(width: max(0, geometry.size.width - 66))
                        .position(x: geometry.size.width / 2 + 15,
                                  y: geometry.size.height / 2 - 5)
                }
                .allowsHitTesting(false)
            }
        }
    }
}

// MARK: - Panel chrome

struct Panel<Content: View>: View {
    let title: String
    var right: String? = nil
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text(title)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Palette.bright)
                    .textCase(.uppercase)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                Spacer()
                if let right {
                    Text(right)
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .foregroundStyle(Palette.bright)
                        .fixedSize(horizontal: true, vertical: false)
                }
            }
            content()
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 10).fill(Palette.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Palette.border, lineWidth: 1))
    }
}

struct SourceCaption: View {
    let summary: String
    let detail: String
    var color: Color = Palette.dim

    var body: some View {
        Text(summary)
            .font(.system(size: 9, design: .monospaced))
            .foregroundStyle(color)
            .lineLimit(2)
            .frame(maxWidth: .infinity, alignment: .leading)
            .help(detail)
    }
}

// MARK: - Horizontal meter

struct Meter: View {
    let label: String
    let value: Double?
    let maxValue: Double?
    let text: String
    var color: Color = Palette.accent

    var body: some View {
        HStack(spacing: 10) {
            Text(label)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(Palette.dim)
                .lineLimit(1)
                .frame(width: 125, alignment: .leading)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 3).fill(Color.white.opacity(0.08))
                    if let value, let maxValue, maxValue > 0 {
                        RoundedRectangle(cornerRadius: 3)
                            .fill(color)
                            .frame(width: Swift.max(0, Swift.min(1, value / maxValue)) * geo.size.width)
                    }
                }
            }
            .frame(height: 10)
            Text(text)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(Palette.bright)
                .frame(minWidth: 120, alignment: .trailing)
        }
    }
}

extension TelemetrySnapshot {
    var umcCurrent: Double? {
        umcActivity.last?.value
    }
}

// MARK: - Main window

struct ContentView: View {
    @EnvironmentObject private var sampler: GPUSampler

    var body: some View {
        let snap = sampler.snapshot
        let selected = snap.deviceLabel.isEmpty ? "" : snap.deviceLabel
        VStack(spacing: 10) {
            header
            HStack(alignment: .top, spacing: 10) {
                Panel(title: "GPU Load",
                      right: snap.coreCurrent != nil ? "[ \(fmt(snap.coreCurrent, 0))% ]" : "[ n/a ]") {
                    TimeSeriesChart(samples: snap.coreLoad, windowSeconds: 60,
                                    color: Palette.accent, fill: Palette.accentFill,
                                    hasData: snap.coreLoad.contains { $0.value != nil },
                                    emptyCaption: "No GFX activity sample yet")
                        .frame(height: 150)
                    SourceCaption(summary: snap.coreHardware
                                    ? "GFX active samples (sel 71), rolling 2 s; not CU occupancy"
                                    : "GFX packet rate (sel 61), scaled to observed peak; not busy %",
                                  detail: snap.coreSourceLabel ?? "No fresh GFX activity source")
                }
                Panel(title: "UMC Activity",
                      right: snap.umcCurrent != nil
                         ? "[ \(fmt(snap.umcCurrent, 0))%\(snap.umcReliable ? "" : " ⚠") ]"
                         : "[ n/a ]") {
                    TimeSeriesChart(samples: snap.umcActivity, windowSeconds: 60,
                                    color: Palette.umc, fill: Palette.umcFill,
                                    hasData: snap.umcActivity.contains { $0.value != nil },
                                    emptyCaption: "No fresh UMC activity sample")
                        .frame(height: 150)
                        .opacity(snap.umcReliable || !snap.umcActivity.contains { $0.value != nil }
                                 ? 1.0 : 0.45)
                    SourceCaption(summary: snap.umcCurrent == nil
                                    ? "UMC busy unavailable on this GPU"
                                    : (snap.umcReliable
                                        ? "MMHUB hardware UMC counter (sel 68)"
                                        : "SMU firmware average; uncalibrated on this GPU"),
                                  detail: snap.umcSourceLabel ?? "No fresh SMU UMC activity sample; MMHUB UMC counter is unavailable on gfx1201",
                                  color: snap.umcReliable ? Palette.dim : .orange)
                }
                Panel(title: "SQ Busy",
                      right: snap.sqBusyCurrent != nil
                         ? "[ \(fmt(snap.sqBusyCurrent, 0))% ]"
                         : "[ n/a ]") {
                    TimeSeriesChart(samples: snap.sqBusy, windowSeconds: 60,
                                    color: Palette.sq, fill: Palette.sqFill,
                                    hasData: snap.sqBusy.contains { $0.value != nil },
                                    emptyCaption: "No live SQ busy-cycle window")
                        .frame(height: 150)
                    SourceCaption(summary: snap.sqBusyCurrent != nil
                                    ? (snap.sqBusySourceLabel?.contains("driver-owned") == true
                                        ? "SQ hardware busy cycles (driver sel 70)"
                                        : "SQ hardware busy cycles (workload sel 69)")
                                    : "No SQ counter: sel 70 unavailable; sel 69 slot absent",
                                  detail: snap.sqBusySourceLabel ?? "No shared SQ slot registered; LSE publishes aqlprofile SQ_BUSY_CYCLES when its SQ profiler is active")
                }
            }
            HStack(alignment: .top, spacing: 10) {
                Panel(title: "VRAM") {
                    if let used = snap.vramUsedGiB, let total = snap.vramTotalGiB,
                       used.isFinite, total.isFinite, total > 0 {
                        Meter(label: "VRAM", value: used, maxValue: total,
                              text: String(format: "%.2f / %.2f GB (%.0f%%)", used, total, used / total * 100))
                        if let vu = snap.vramVisibleUsedGiB, let vt = snap.vramVisibleTotalGiB, vt > 0 {
                            Meter(label: "GTT", value: vu, maxValue: vt,
                                  text: String(format: "%.2f / %.2f GB (%.0f%%)", vu, vt, vu / vt * 100),
                                  color: Palette.warm)
                        }
                    } else {
                        Text("driver VRAM accounting not available (stage \(snap.stage))")
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(Palette.dim)
                    }
                    Text("source: driver CPU allocator pools (query tag 5)")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Palette.dim)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                Panel(title: "Clocks (SMU, MHz)") {
                    VStack(alignment: .leading, spacing: 4) {
                        if snap.stage != 15 {
                            Text(snap.idle
                                 ? "GPU idle; no active driver session. Clocks appear during GPU work."
                                 : "current clocks unavailable until GPU initialization")
                                .font(.system(size: 9, design: .monospaced))
                                .foregroundStyle(Palette.dim)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        ForEach(snap.clocks) { row in
                            VStack(alignment: .leading, spacing: 1) {
                                HStack {
                                    Text(row.name).frame(width: 68, alignment: .leading)
                                    Text(row.current.map { fmtInt($0) + " MHz " + (row.currentKind == "avg" ? "SMU avg" : "raw") } ?? "n/a")
                                        .frame(width: 150, alignment: .trailing)
                                    if let lo = row.minimum, let hi = row.maximum {
                                        Text(String(format: "DPM %d–%d", Int(lo), Int(hi)))
                                            .font(.system(size: 10, design: .monospaced))
                                            .foregroundStyle(Palette.dim)
                                    }
                                    Spacer()
                                }
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundStyle(row.current != nil ? Palette.bright : Palette.dim)
                                if row.currentKind == "avg", let raw = row.rawCurrent {
                                    Text("raw CurrClock: \(fmtInt(raw)) MHz")
                                        .font(.system(size: 9, design: .monospaced))
                                        .foregroundStyle(Palette.dim)
                                        .padding(.leading, 68)
                                } else if let average = row.average {
                                    Text("SMU avg field: \(fmtInt(average)) MHz")
                                        .font(.system(size: 9, design: .monospaced))
                                        .foregroundStyle(Palette.dim)
                                        .padding(.leading, 68)
                                }
                                if let note = row.note {
                                    Text(note)
                                        .font(.system(size: 9, design: .monospaced))
                                        .foregroundStyle(Palette.warm)
                                        .padding(.leading, 68)
                                }
                            }
                        }
                        if snap.stage == 15 {
                            Text("DPM = advertised AC levels; averages may include deep sleep")
                                .font(.system(size: 9, design: .monospaced))
                                .foregroundStyle(Palette.dim)
                                .lineLimit(2)
                        }
                    }
                }
                Panel(title: "Sensors (SMU)") {
                    VStack(alignment: .leading, spacing: 4) {
                        if snap.stage != 15 {
                            Text(snap.idle
                                 ? "GPU idle; sensors need an active driver session."
                                 : "sensor cache unavailable until GPU initialization")
                                .font(.system(size: 9, design: .monospaced))
                                .foregroundStyle(Palette.dim)
                        }
                        Meter(label: snap.unverifiedSmuGfxActivityPercent != nil
                                    ? "GFX avg (SMU raw)" : "GFX avg",
                              value: snap.smuGfxActivityPercent ?? snap.unverifiedSmuGfxActivityPercent,
                              maxValue: 100,
                              text: (snap.smuGfxActivityPercent ?? snap.unverifiedSmuGfxActivityPercent)
                                  .map { String(format: "%.0f%%", $0) } ?? "n/a",
                              color: Palette.accent)
                        Meter(label: snap.unverifiedSmuSocketPowerWatts != nil || snap.unverifiedSmuBoardPowerWatts != nil
                                    ? "Power (SMU raw)" : "Power",
                              value: snap.powerWatts ?? snap.unverifiedSmuSocketPowerWatts ?? snap.unverifiedSmuBoardPowerWatts,
                              maxValue: 300,
                              text: (snap.powerWatts ?? snap.unverifiedSmuSocketPowerWatts ?? snap.unverifiedSmuBoardPowerWatts)
                                  .map { String(format: "%.0f W", $0) } ?? "n/a",
                              color: Palette.warm)
                        Meter(label: "Edge",
                              value: snap.edgeCelsius,
                              maxValue: 100,
                              text: snap.edgeCelsius.map { String(format: "%.0f °C", $0) } ?? "n/a",
                              color: Palette.warm)
                        // Junc (hotspot) is a separate sensor from Edge.
                        Meter(label: "Junc",
                              value: snap.hotspotCelsius,
                              maxValue: 120,
                              text: snap.hotspotCelsius.map { String(format: "%.0f °C", $0) } ?? "n/a",
                              color: Palette.warm)
                        Meter(label: "Fan",
                              value: snap.fanRPM,
                              maxValue: 5000,
                              text: snap.fanRPM.map { String(format: "%.0f RPM", $0) } ?? "n/a",
                              color: Palette.accent)
                    }
                }
            }
            Panel(title: "Engines (packet rate, per window)") {
                VStack(spacing: 6) {
                    ForEach(snap.engines) { row in
                        HStack(spacing: 10) {
                            Text(row.name)
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundStyle(Palette.dim)
                                .frame(width: 130, alignment: .leading)
                            GeometryReader { geo in
                                ZStack(alignment: .leading) {
                                    RoundedRectangle(cornerRadius: 3).fill(.white.opacity(0.08))
                                    if let v = row.value {
                                        RoundedRectangle(cornerRadius: 3)
                                            .fill(v > 80 ? Color.red : v > 50 ? Palette.warm : Palette.accent)
                                            .frame(width: min(1, v / 100) * geo.size.width)
                                    }
                                }
                            }
                            .frame(height: 9)
                            if let v = row.value {
                                Text("\(fmt(v, 0))%")
                                    .font(.system(size: 11, design: .monospaced))
                                    .foregroundStyle(Palette.bright)
                                    .frame(width: 44, alignment: .trailing)
                            } else {
                                Text(row.note)
                                    .font(.system(size: 10, design: .monospaced))
                                    .foregroundStyle(Palette.dim)
                            }
                            Spacer()
                        }
                    }
                }
                Text("source: driver software_stats selector 61, per-engine submitted-packet rate scaled to observed peak; VCN/JPEG have no observer counter")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Palette.dim)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Spacer(minLength: 0)
            HStack {
                Text("amdgpu_mtopg — read-only observer; Esc or window close quits")
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(Palette.dim)
                Spacer()
                if !selected.isEmpty {
                    Text("device \(selected)")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(Palette.dim)
                }
            }
        }
        .padding(12)
        .background(Palette.background.ignoresSafeArea())
        .escToQuit()
        .environment(\.colorScheme, .dark)
    }

    private var header: some View {
        HStack(spacing: 14) {
            let snap = sampler.snapshot
            Text("AMD GPU monitor")
                .font(.system(size: 15, weight: .bold, design: .monospaced))
                .foregroundStyle(Palette.bright)
            if !snap.gfxVersion.isEmpty {
                Text(snap.gfxVersion)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Palette.accent)
            }
            if let spec = snap.specInfo {
                Text(spec)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(Palette.dim)
            }
            Text("build \(snap.build)")
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(Palette.dim)
            if snap.idle {
                Text("idle — no active GPU session")
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                    .foregroundStyle(Palette.umc)
            } else {
                Text(snap.stage == 15 ? "initialized" : "stage \(snap.stage)")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(snap.stage == 15 ? Palette.umc : Palette.warm)
            }
            Spacer()
            if let error = snap.error {
                Text(error)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.red)
            } else if snap.deviceLabel.isEmpty, !snap.idle {
                Text("no device")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.red)
            }
        }
    }
}
