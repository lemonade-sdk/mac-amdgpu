//
//  MacAMDGPUHostApp.swift — Minimal SwiftUI host app for the dext.
//
//  Apple requires every DriverKit system extension to ship inside a
//  parent application, and that application must live in /Applications
//  for systemextensionsctl to stage the dext.
//
//  This app:
//   • Shows a one-window UI with "Install Driver" / "Remove Driver"
//     buttons that call OSSystemExtensionRequest.
//   • Streams activation-request results into the window.
//   • Stays small on purpose — once Phase 1B firmware loading is
//     ready, that UX can grow, but for now this is just the
//     entitlement / activation harness.
//

import SwiftUI
import SystemExtensions

@main
struct MacAMDGPUHostApp: App {
    @StateObject private var controller = DriverController()

    var body: some Scene {
        WindowGroup("MacAMDGPU") {
            ContentView()
                .environmentObject(controller)
                .frame(minWidth: 480, minHeight: 320)
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var controller: DriverController

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("MacAMDGPU")
                .font(.title)
            Text("Native AMD GPU driver for Radeon AI PRO R9700 over Thunderbolt 5.")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            Divider()

            HStack {
                Button("Install Driver") {
                    controller.requestActivate()
                }
                Button("Remove Driver") {
                    controller.requestDeactivate()
                }
                Spacer()
                Text(controller.status)
                    .font(.callout.monospaced())
                    .foregroundStyle(controller.statusColor)
            }

            ScrollView {
                Text(controller.log)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .background(Color(NSColor.textBackgroundColor))
            .border(Color.gray.opacity(0.4))
        }
        .padding()
    }
}

final class DriverController: NSObject, ObservableObject,
                                OSSystemExtensionRequestDelegate {
    // Must match the dext's CFBundleIdentifier from dext/Info.plist
    // after xcodegen has resolved $(PRODUCT_BUNDLE_IDENTIFIER).
    private let dextBundleIdentifier =
        "com.example.MacAMDGPUHost.MacAMDGPU"

    @Published var log: String = ""
    @Published var status: String = "idle"
    @Published var statusColor: Color = .secondary

    func requestActivate() {
        append("submitting activation request for \(dextBundleIdentifier)")
        let req = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier,
            queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
        status = "activating…"
        statusColor = .orange
    }

    func requestDeactivate() {
        append("submitting deactivation request")
        let req = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: dextBundleIdentifier,
            queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
        status = "deactivating…"
        statusColor = .orange
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction
    {
        append("replacing existing extension version \(existing.bundleShortVersion) "
               + "with \(ext.bundleShortVersion)")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        append("user approval required — open System Settings → Privacy & Security "
               + "and click Allow next to MacAMDGPU")
        status = "awaiting user approval"
        statusColor = .yellow
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result)
    {
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
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: any Error)
    {
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
