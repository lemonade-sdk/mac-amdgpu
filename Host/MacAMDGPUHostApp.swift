//
//  MacAMDGPUHostApp.swift — Self-installing host for the dext.
//
//  Flow on launch:
//
//   1. If the app isn't in /Applications: show a one-button "Install"
//      panel. Click → copy ourselves into /Applications, relaunch
//      from there, exit. (macOS won't stage a DriverKit extension
//      unless its parent app lives in /Applications.)
//
//   2. Once running from /Applications: auto-submit an
//      OSSystemExtensionRequest.activationRequest. macOS may prompt
//      the user to approve in System Settings → Privacy & Security;
//      we surface that requirement live in the window.
//
//   3. Once activated, the dext is bound to the R9700 and macAMDGPU
//      is ready for the userspace test client (scripts/macamdgpu_ping).
//

import SwiftUI
import SystemExtensions
import AppKit

@main
struct MacAMDGPUHostApp: App {
    @StateObject private var controller = DriverController()

    var body: some Scene {
        WindowGroup("MacAMDGPU") {
            ContentView()
                .environmentObject(controller)
                .frame(minWidth: 540, minHeight: 380)
                .task { await controller.runStartupFlow() }
        }
    }
}

// MARK: - View

struct ContentView: View {
    @EnvironmentObject var controller: DriverController

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("MacAMDGPU")
                    .font(.title.weight(.semibold))
                Spacer()
                Text(controller.status)
                    .font(.callout.monospaced())
                    .foregroundStyle(controller.statusColor)
                    .padding(.horizontal, 10).padding(.vertical, 4)
                    .background(Color.gray.opacity(0.15))
                    .clipShape(Capsule())
            }
            Text("Third-party AMD GPU driver for Radeon AI PRO R9700 over Thunderbolt 5.")
                .foregroundStyle(.secondary)

            Divider()

            HStack(spacing: 8) {
                if controller.needsMoveToApplications {
                    Button {
                        controller.installToApplications()
                    } label: {
                        Label("Install to /Applications and relaunch",
                              systemImage: "arrow.down.app")
                            .padding(.horizontal, 4)
                    }
                    .keyboardShortcut(.defaultAction)
                } else {
                    Button("Install Driver") { controller.requestActivate() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(controller.isWorking)
                    Button("Remove Driver") { controller.requestDeactivate() }
                        .disabled(controller.isWorking)
                }
                Spacer()
                Button("Open Driver Status") {
                    NSWorkspace.shared.open(URL(fileURLWithPath:
                        "x-apple.systempreferences:com.apple.preference.security?Privacy_SystemServices"))
                }
                .help("Opens System Settings → Privacy & Security where blocked extensions are approved.")
            }

            ScrollView {
                Text(controller.log)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(6)
            }
            .background(Color(NSColor.textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.gray.opacity(0.4)))
        }
        .padding(14)
    }
}

// MARK: - Controller

@MainActor
final class DriverController: NSObject, ObservableObject,
                                OSSystemExtensionRequestDelegate {

    // Must match dext/Info.plist CFBundleIdentifier
    private let dextBundleIdentifier =
        "com.geramyloveless.MacAMDGPUHost.MacAMDGPU"

    @Published var log: String = ""
    @Published var status: String = "starting…"
    @Published var statusColor: Color = .secondary
    @Published var isWorking: Bool = false
    @Published var needsMoveToApplications: Bool = false

    private var didAutoActivate = false

    // MARK: Startup flow

    func runStartupFlow() async {
        let bundlePath = Bundle.main.bundlePath
        append("launched from \(bundlePath)")

        if !bundlePath.hasPrefix("/Applications/") {
            // Step 1: not in /Applications. Need user to opt in.
            needsMoveToApplications = true
            status = "needs install"
            statusColor = .orange
            append("not in /Applications — click the install button "
                   + "to copy the app there and relaunch")
            return
        }

        // Step 2: in /Applications. Auto-activate the dext.
        if !didAutoActivate {
            didAutoActivate = true
            append("running from /Applications — submitting activation request")
            requestActivate()
        }
    }

    // MARK: Step 1 — install to /Applications

    func installToApplications() {
        isWorking = true
        status = "installing…"
        statusColor = .orange

        let src = Bundle.main.bundleURL
        let appsURL = URL(fileURLWithPath: "/Applications", isDirectory: true)
        let dst = appsURL.appendingPathComponent(src.lastPathComponent)

        Task.detached(priority: .userInitiated) { [src, dst] in
            await MainActor.run { self.append("copying \(src.lastPathComponent) → \(dst.path)") }

            // If a previous copy exists, remove it first so the copy
            // doesn't fail with EEXIST.
            do {
                if FileManager.default.fileExists(atPath: dst.path) {
                    await MainActor.run { self.append("removing existing \(dst.path)") }
                    try FileManager.default.removeItem(at: dst)
                }
                try FileManager.default.copyItem(at: src, to: dst)
            } catch {
                await MainActor.run {
                    self.append("copy failed: \(error.localizedDescription)")
                    self.append("note: /Applications may require admin authorisation. "
                                + "Try: sudo cp -R \"\(src.path)\" /Applications/")
                    self.status = "install failed"
                    self.statusColor = .red
                    self.isWorking = false
                }
                return
            }

            await MainActor.run {
                self.append("copied; relaunching from /Applications")
                self.relaunchFromApplications(at: dst)
            }
        }
    }

    private func relaunchFromApplications(at appURL: URL) {
        // Use `open` so the new instance starts as a normal Launch
        // Services launch (with /Applications as origin), not as a
        // child of this process.
        let config = NSWorkspace.OpenConfiguration()
        config.activates = true
        NSWorkspace.shared.openApplication(at: appURL,
                                            configuration: config) { _, error in
            if let error {
                Task { @MainActor in
                    self.append("relaunch failed: \(error.localizedDescription)")
                    self.status = "relaunch failed"
                    self.statusColor = .red
                    self.isWorking = false
                }
                return
            }
            // Give the new instance a beat to claim the foreground
            // before this one exits.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                NSApp.terminate(nil)
            }
        }
    }

    // MARK: Step 2 — activate / deactivate the dext

    func requestActivate() {
        isWorking = true
        status = "activating…"
        statusColor = .orange
        append("OSSystemExtensionRequest.activationRequest("
               + "\(dextBundleIdentifier))")
        let req = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier,
            queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    func requestDeactivate() {
        isWorking = true
        status = "deactivating…"
        statusColor = .orange
        append("OSSystemExtensionRequest.deactivationRequest")
        let req = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier,
            queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction
    {
        append("replacing existing \(existing.bundleShortVersion) → "
               + "\(ext.bundleShortVersion)")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        append("user approval required — open System Settings → "
               + "Privacy & Security and click Allow next to MacAMDGPU")
        status = "approval required"
        statusColor = .yellow
        // Pop System Settings to the right pane.
        NSWorkspace.shared.open(URL(fileURLWithPath:
            "x-apple.systempreferences:com.apple.preference.security?Privacy_SystemServices"))
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result)
    {
        isWorking = false
        switch result {
        case .completed:
            append("request completed")
            status = "installed"
            statusColor = .green
        case .willCompleteAfterReboot:
            append("request will complete after reboot")
            status = "reboot required"
            statusColor = .yellow
        @unknown default:
            append("request finished with unknown result \(result.rawValue)")
            status = "unknown"
            statusColor = .secondary
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: any Error)
    {
        isWorking = false
        append("request failed: \(error.localizedDescription)")
        if let osErr = error as? OSSystemExtensionError {
            append("  OSSystemExtensionError code=\(osErr.errorCode)")
        }
        status = "error"
        statusColor = .red
    }

    // MARK: helpers

    private func append(_ line: String) {
        let ts = ISO8601DateFormatter().string(from: Date())
        let msg = "[\(ts)] \(line)\n"
        log += msg
        NSLog("%@", msg)
    }
}
