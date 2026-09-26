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

            guard hasData else {
                // No source is producing samples: keep the frame, explain why.
                let label = Text(emptyCaption)
                    .font(.system(size: 11))
                    .foregroundStyle(Palette.dim)
                context.draw(context.resolve(label),
                             at: CGPoint(x: plot.midX, y: plot.midY), anchor: .center)
                return
            }

            var path = Path()
            var area = Path()
            var last: CGPoint?
            for (age, value) in samples {
                guard let v = value, v.isFinite, age <= windowSeconds else { continue }
                let x = plot.maxX - plot.width * CGFloat(age / windowSeconds)
                let y = plot.minY + plot.height * CGFloat(1.0 - min(max(v / 100.0, 0.0), 1.0))
                let point = CGPoint(x: x, y: y)
                if last == nil {
                    path.move(to: point)
                    area.move(to: CGPoint(x: x, y: plot.maxY))
                    area.addLine(to: point)
                } else {
                    path.addLine(to: point)
                    area.addLine(to: point)
                }
                last = point
            }
            if let end = last {
                area.addLine(to: CGPoint(x: end.x, y: plot.maxY))
                area.closeSubpath()
                context.fill(area, with: .color(fill))
            }
            context.stroke(path, with: .color(color),
                           style: StrokeStyle(lineWidth: 1.6, lineJoin: .round))
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
                Spacer()
                if let right {
                    Text(right)
                        .font(.system(size: 12, weight: .semibold, design: .monospaced))
                        .foregroundStyle(Palette.bright)
                }
            }
            content()
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 10).fill(Palette.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Palette.border, lineWidth: 1))
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
                .frame(width: 74, alignment: .leading)
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
        umcActivity.last { $0.value != nil }?.value
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
                Panel(title: "GPU Core Load",
                      right: snap.coreCurrent != nil ? "[ \(fmt(snap.coreCurrent, 0))% ]" : "[ n/a ]") {
                    TimeSeriesChart(samples: snap.coreLoad, windowSeconds: 60,
                                    color: Palette.accent, fill: Palette.accentFill,
                                    hasData: snap.coreLoad.contains { $0.value != nil },
                                    emptyCaption: "no dispatch-in-flight samples yet (driver idle or pre-193 build)")
                        .frame(height: 150)
                    Text("source: driver dispatch-in-flight, selector 61 (software_stats pendingNs delta); SMU AverageGfxActivity is incoherent on this host and is not plotted; \(snap.softwareStatsStatus)")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Palette.dim)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                Panel(title: "UMC Memory Activity",
                      right: snap.umcCurrent != nil ? "[ \(fmt(snap.umcCurrent, 0))% ]" : "[ n/a ]") {
                    TimeSeriesChart(samples: snap.umcActivity, windowSeconds: 60,
                                    color: Palette.umc, fill: Palette.umcFill,
                                    hasData: snap.umcActivity.contains { $0.value != nil },
                                    emptyCaption: "no UMC samples yet (waiting for a fresh SMU UmcActivityPercent; MMHUB PERFCTR arrives with driver build 198+)")
                        .frame(height: 150)
                    Text("source: " + (snap.umcSourceLabel ?? "none"))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Palette.dim)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            HStack(alignment: .top, spacing: 10) {
                Panel(title: "VRAM") {
                    if let used = snap.vramUsedGiB, let total = snap.vramTotalGiB {
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
                        ForEach(snap.clocks) { row in
                            HStack {
                                Text(row.name).frame(width: 68, alignment: .leading)
                                Text(fmtInt(row.current) + " MHz")
                                    .frame(width: 70, alignment: .trailing)
                                if let lo = row.minimum, let hi = row.maximum {
                                    Text(String(format: "(%d – %d)", Int(lo), Int(hi)))
                                        .font(.system(size: 10, design: .monospaced))
                                        .foregroundStyle(Palette.dim)
                                }
                                Spacer()
                            }
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(row.current != nil ? Palette.bright : Palette.dim)
                        }
                    }
                }
                Panel(title: "Sensors (SMU)") {
                    VStack(alignment: .leading, spacing: 4) {
                        // Power: board limit when the SMU exposes it, else a
                        // sensible cap for the 300 W-class R9700 mobile GPU.
                        Meter(label: "Power",
                              value: snap.powerWatts,
                              maxValue: snap.boardPowerWatts.map { $0 > 0 ? $0 : nil } ?? 300,
                              text: (snap.powerWatts.map { String(format: "%.0f W", $0) } ?? "n/a")
                                  + (snap.boardPowerWatts.map { " / \(Int($0)) W cap" } ?? ""),
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
            Panel(title: "Engines (dispatch-in-flight, per window)") {
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
                Text("source: driver software_stats selector 61, per-engine pendingNs delta; VCN/JPEG have no observer counter")
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
                Text("idle — no in-flight GPU work")
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
