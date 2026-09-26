// amdgpu_mtopg — SwiftUI GPU monitor (2D companion to the terminal amdgpu_mtop).
//
// Real 2D vector drawing (SwiftUI Canvas): live rolling charts for GPU core
// load and UMC memory activity, VRAM/clock/sensor readouts, per-engine
// dispatch-in-flight strip. Data comes from the same MacAMDGPU IOKit
// observer client the terminal monitor uses (selector 61 dispatch-in-flight
// is the trusted core-load source; SMU activity fields are incoherent on
// this host and are only shown when a source actually produced the value).
//
// Read-only: no GPU work is submitted and no driver state is mutated.
//
// MIT License — see the repository LICENSE.

import AppKit
import SwiftUI

// MARK: - Sampler (background thread -> main-thread snapshot)

final class GPUSampler: ObservableObject {
    static let shared = GPUSampler()

    @Published var snapshot = TelemetrySnapshot()

    private let transport = DriverTransport()
    private let history = SampleHistory()
    private let queue = DispatchQueue(label: "amdgpu_mtopg.sampler", qos: .userInitiated)
    private var selectedRegistry: UInt64?
    private var running = false

    static let umcDefaultLabel = "no UMC sample yet (fresh SMU UmcActivityPercent, or MMHUB PERFCTR on driver build 198+)"

    func start() {
        guard !running else { return }
        running = true
        queue.async { [weak self] in
            self?.loop()
        }
    }

    func stop() {
        // Stop the sampler loop first so it cannot call into the driver while
        // the app is tearing the IOKit service down (a read racing the
        // teardown traps in the struct decode). Then close the transport off
        // the main thread; the connection is already per-sample open/close so
        // no IOConnect leaks.
        running = false
        queue.async { [weak self] in
            self?.transport.close()
        }
    }

    private func loop() {
        // 10 Hz: charts want a per-second sample; faster polling burns IOKit
        // calls for identical counter values. The loop exits when stop() clears
        // `running`, so app shutdown does not leave a sampler thread reading a
        // half-torn-down driver connection.
        while running {
            let loopStart = Date()
            let (devices, error) = transport.refresh()
            var snap = makeSnapshot(device: nil, history: history,
                                    umcDefaultLabel: Self.umcDefaultLabel, engineValues: nil)
            if let r = selectedRegistry, !devices.contains(where: { $0.registry == r }) {
                selectedRegistry = nil   // selected card vanished: reselect
            }
            if selectedRegistry == nil {
                selectedRegistry = devices.first?.registry
            }
            let selectedDevice = devices.first { $0.registry == selectedRegistry }
            if let d = selectedDevice {
                // Engine percents need the pre-add baseline; compute them
                // first, then fold the sample into the history.
                let engines = (0..<4).map { history.enginePercent(d, index: $0) }
                snap = makeSnapshot(device: d, history: history,
                                    umcDefaultLabel: Self.umcDefaultLabel, engineValues: engines)
                history.add(d)
            }
            if let e = error { snap.error = e }
            if let r = selectedRegistry { snap.selectedDeviceID = r }
            DispatchQueue.main.async { [weak self] in
                self?.snapshot = snap
            }
            // 10 Hz cadence: sleep the remainder of the 100 ms period.
            let elapsed = Date().timeIntervalSince(loopStart)
            if elapsed < 0.1 { Thread.sleep(forTimeInterval: 0.1 - elapsed) }
        }
    }
}

// MARK: - App

@main
struct MtopGApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup("amdgpu_mtopg") {
            ContentView()
                .environmentObject(GPUSampler.shared)
                .frame(minWidth: 780, minHeight: 600)
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var didCreateWindow = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Fully AppKit-hosted single window: in this headless/background
        // launch context the SwiftUI WindowGroup scene does not install its
        // hosting window (the app auto-terminates with none open), so the
        // SwiftUI view tree is embedded in a classic NSWindow instead.
        didCreateWindow = true
        GPUSampler.shared.start()
        makeMainWindow()
    }

    private func makeMainWindow() {
        guard NSApp.windows.filter({ $0.contentView != nil }).isEmpty else { return }
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 860, height: 660),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "amdgpu_mtopg — AMD GPU monitor"
        window.minSize = NSSize(width: 780, height: 600)
        let host = NSHostingView(rootView: ContentView()
            .environmentObject(GPUSampler.shared))
        window.contentView = host
        window.center()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(host)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationWillTerminate(_ notification: Notification) {
        GPUSampler.shared.stop()
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        if !didCreateWindow {
            didCreateWindow = true
            makeMainWindow()
        } else if NSApp.windows.filter({ $0.contentView != nil }).isEmpty {
            makeMainWindow()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}
