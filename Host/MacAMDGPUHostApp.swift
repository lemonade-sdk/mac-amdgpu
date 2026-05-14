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
import IOKit

// MARK: - User-client selectors (must match dext/MacAMDGPU.cpp)

private let kSelPing:            UInt32 = 0
private let kSelGetIdentity:     UInt32 = 1
private let kSelGetBARInfo:      UInt32 = 2
private let kSelAllocateDMA:     UInt32 = 6
private let kSelInitDevice:      UInt32 = 9
private let kSelLoadFirmware:    UInt32 = 10
private let kSelQueryInfo:       UInt32 = 21

// Firmware type tags — match MacAMDGPU.cpp enum.
private let kFwSOS:         UInt64 = 0
private let kFwKDB:         UInt64 = 1
private let kFwSPL:         UInt64 = 2
private let kFwSysDrv:      UInt64 = 3
private let kFwSocDrv:      UInt64 = 4
private let kFwIntfDrv:     UInt64 = 5
private let kFwDbgDrv:      UInt64 = 6
private let kFwRASDrv:      UInt64 = 7
private let kFwIPKeyMgrDrv: UInt64 = 8
private let kFwIP_SMU:      UInt64 = 0x100 + 18
private let kFwIP_SDMA0:    UInt64 = 0x100 + 9
private let kFwIP_SDMA1:    UInt64 = 0x100 + 10
private let kFwIP_RLC_G:    UInt64 = 0x100 + 8
private let kFwIP_CP_ME:    UInt64 = 0x100 + 1
private let kFwIP_CP_PFP:   UInt64 = 0x100 + 2
private let kFwIP_CP_MEC:   UInt64 = 0x100 + 4
private let kFwIP_RS64_MES: UInt64 = 0x100 + 76

// Bringup stages — match amdgpu_init.h.
private let kStageIPDiscovery:   UInt64 = 1
private let kStagePSPInit:       UInt64 = 2

// QueryInfo type tags
private let kInfoGFXVersion:     UInt64 = 1
private let kInfoVRAMSizes:      UInt64 = 2
private let kInfoIPVersions:     UInt64 = 3
private let kInfoBringupReached: UInt64 = 4

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

            HStack(spacing: 16) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Bundled").font(.caption2).foregroundStyle(.secondary)
                    Text(controller.bundledVersion)
                        .font(.system(.callout, design: .monospaced))
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("Installed").font(.caption2).foregroundStyle(.secondary)
                    Text(controller.installedVersion)
                        .font(.system(.callout, design: .monospaced))
                        .foregroundStyle(controller.versionMatch ? Color.green : Color.orange)
                }
                Spacer()
                Button("Refresh") { controller.refreshVersions() }
                    .controlSize(.small)
            }
            .padding(.vertical, 4)

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

            // Test buttons — drive dext selectors from inside the host
            // process. Useful for bring-up before we have a userspace
            // ICD; the dext's allow-any-userclient-access entitlement
            // makes external clients work too once Apple grants the
            // matching capability to a separate tool.
            HStack(spacing: 6) {
                Button("Ping") { controller.testPing() }
                Button("Identity") { controller.testGetIdentity() }
                Button("BARs") { controller.testGetBARInfo() }
                Button("Query") { controller.testQueryInfo() }
                Spacer()
                Button("Run Full Bringup") {
                    controller.runFullBringup()
                }
                .help("Loads PSP firmware then runs every InitDevice stage.")
            }
            .font(.caption)

            // Bring-up stage ladder. Click each in order; each runs
            // every stage up to and including the requested target.
            VStack(alignment: .leading, spacing: 4) {
                Text("Bringup stages").font(.caption2).foregroundStyle(.secondary)
                HStack(spacing: 4) {
                    Button("1. IPDiscovery")   { controller.testInitDeviceUpTo(1) }
                    Button("2. PSPInit")       { controller.testInitDeviceUpTo(2) }
                    Button("3. LoadSOS+...")   { controller.runFullBringup() }
                    Button("4. PSPRingCreate") { controller.testInitDeviceUpTo(4) }
                    Button("5. TMRSetup")      { controller.testInitDeviceUpTo(5) }
                }
                .font(.caption)
                HStack(spacing: 4) {
                    Button("6. SMUInit")  { controller.testInitDeviceUpTo(6) }
                    Button("7. GMCInit")  { controller.testInitDeviceUpTo(7) }
                    Button("9. RLCInit")  { controller.testInitDeviceUpTo(9) }
                    Button("10. CPInit")  { controller.testInitDeviceUpTo(10) }
                    Button("12. IHInit")  { controller.testInitDeviceUpTo(12) }
                    Button("14. SDMA")    { controller.testInitDeviceUpTo(14) }
                }
                .font(.caption)
            }
            .padding(.vertical, 4)

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
    @Published var bundledVersion: String = "—"
    @Published var installedVersion: String = "—"
    @Published var versionMatch: Bool = false

    private var didAutoActivate = false

    // MARK: Startup flow

    func runStartupFlow() async {
        let bundlePath = Bundle.main.bundlePath
        append("launched from \(bundlePath)")
        refreshVersions()

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
        append("user approval required — open System Settings → General "
               + "→ Login Items & Extensions → Driver Extensions, toggle "
               + "MacAMDGPU on")
        status = "approval required"
        statusColor = .yellow
        // Don't keep the buttons greyed out — the OS hands the
        // approval flow to System Settings; the user may take a while
        // and we want to let them retry / deactivate manually.
        isWorking = false
        // Pop System Settings to the right pane (Tahoe path).
        if let url = URL(string:
            "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
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
                 foundProperties properties: [OSSystemExtensionProperties])
    {
        if properties.isEmpty {
            installedVersion = "not installed"
            versionMatch = false
            return
        }
        // Pick the active one if there are multiple staged copies
        // (e.g. an old + a pending replacement).
        let active = properties.first { $0.isEnabled }
                  ?? properties.first { $0.isAwaitingUserApproval == false }
                  ?? properties[0]
        let short = active.bundleShortVersion
        let build = active.bundleVersion
        installedVersion = "\(short) (\(build))"
        // Compare against the bundled one we already computed.
        let bundledShort = (bundledVersion as NSString)
            .components(separatedBy: " ").first ?? ""
        versionMatch = bundledShort == short
        let stateBits: [String] = [
            active.isEnabled ? "enabled" : "",
            active.isAwaitingUserApproval ? "awaiting-approval" : "",
            active.isUninstalling ? "uninstalling" : "",
        ].filter { !$0.isEmpty }
        append("installed dext: \(short) (\(build)) "
               + (stateBits.isEmpty ? "" : "[\(stateBits.joined(separator: ","))]"))
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

    // MARK: User-client connection (talks to the dext directly)

    private var ucConn: io_connect_t = 0

    /// Open the dext's IOUserUserClient. Idempotent.
    ///
    /// Matching dexts requires iterating IOUserService instances and
    /// checking each candidate's CFBundleIdentifier in the registry,
    /// because `IOUserClass` is *not* a queryable registry property —
    /// it lives only on `IOMatchedPersonality`. Filtering on
    /// `IOUserClass` in the matching dict silently matches the FIRST
    /// IOUserService in the iterator (often an Apple system dext),
    /// then IOServiceOpen fails with kIOReturnNotPermitted (0xe00002e2)
    /// because it tried to open the wrong dext. The kernel log
    /// `DK: <SomeAppleDext>:UC failed userclient-access check, needed
    /// bundle ID com.apple.DriverKit-...` is the giveaway.
    @discardableResult
    func openUserClient() -> Bool {
        if ucConn != 0 { return true }
        guard let raw = IOServiceMatching("IOUserService") else {
            append("openUserClient: IOServiceMatching nil")
            return false
        }
        var iter: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,
                                           raw as CFDictionary,
                                           &iter) == KERN_SUCCESS else {
            append("openUserClient: IOServiceGetMatchingServices failed")
            return false
        }
        defer { IOObjectRelease(iter) }

        var svc: io_service_t = IOIteratorNext(iter)
        var candidates = 0
        while svc != 0 {
            candidates += 1
            // Pull CFBundleIdentifier from the matched personality and
            // compare to our dext bundle ID.
            var props: Unmanaged<CFMutableDictionary>?
            let kr = IORegistryEntryCreateCFProperties(
                svc, &props, kCFAllocatorDefault, 0)
            if kr == KERN_SUCCESS, let dict = props?.takeRetainedValue()
                as? [String: Any]
            {
                let bid = dict["CFBundleIdentifier"] as? String
                let userClass = dict["IOUserClass"] as? String
                if bid == dextBundleIdentifier ||
                   userClass == "MacAMDGPU"
                {
                    var conn: io_connect_t = 0
                    let ok = IOServiceOpen(svc, mach_task_self_, 0, &conn)
                    IOObjectRelease(svc)
                    guard ok == KERN_SUCCESS else {
                        append(String(format:
                            "openUserClient: matched bid=%@ but "
                          + "IOServiceOpen kr=%#x",
                            bid ?? "?", ok))
                        return false
                    }
                    ucConn = conn
                    append("openUserClient: connected to "
                           + "\(bid ?? "?") (handle=\(conn))")
                    return true
                }
            }
            IOObjectRelease(svc)
            svc = IOIteratorNext(iter)
        }
        append("openUserClient: scanned \(candidates) IOUserService "
               + "candidates, none matched bundle id \(dextBundleIdentifier)")
        return false
    }

    private func callScalar(_ selector: UInt32,
                            input: [UInt64] = [],
                            outCount: Int = 1) -> (Int32, [UInt64]) {
        guard ucConn != 0 else { return (KERN_INVALID_ARGUMENT, []) }
        let inCount = UInt32(input.count)
        var outArr = [UInt64](repeating: 0, count: max(1, outCount))
        var outN = UInt32(outArr.count)
        let kr: Int32 = input.withUnsafeBufferPointer { ibuf in
            outArr.withUnsafeMutableBufferPointer { obuf in
                IOConnectCallScalarMethod(ucConn, selector,
                                          ibuf.baseAddress,
                                          inCount,
                                          obuf.baseAddress,
                                          &outN)
            }
        }
        return (kr, Array(outArr.prefix(Int(outN))))
    }

    // MARK: Test buttons

    func testPing() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelPing, input: [0xCAFEBABE], outCount: 1)
        if kr == KERN_SUCCESS, let echoed = out.first {
            append(String(format: "ping: ok, echo=%#llx", echoed))
        } else {
            append(String(format: "ping: failed kr=%#x", kr))
        }
    }

    func testGetIdentity() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelGetIdentity, outCount: 6)
        if kr == KERN_SUCCESS && out.count >= 6 {
            // dext returns [bus, dev, fn, vid, did, classRev]
            let bus      = UInt8(out[0] & 0xFF)
            let dev      = UInt8(out[1] & 0xFF)
            let fn       = UInt8(out[2] & 0xFF)
            let vid      = UInt16(out[3] & 0xFFFF)
            let did      = UInt16(out[4] & 0xFFFF)
            let classRev = UInt32(out[5] & 0xFFFFFF)
            append(String(format:
                "identity: %02x:%02x.%x VID=%04x DID=%04x class+rev=%06x",
                bus, dev, fn, vid, did, classRev))
        } else {
            append(String(format: "identity: failed kr=%#x", kr))
        }
    }

    func testGetBARInfo() {
        guard openUserClient() else { return }
        for i in 0..<6 {
            let (kr, out) = callScalar(kSelGetBARInfo,
                                       input: [UInt64(i)],
                                       outCount: 3)
            if kr == KERN_SUCCESS && out.count >= 3 {
                let type = out[0]
                let size = out[1]
                let pref = out[2]
                if size != 0 {
                    append(String(format:
                        "bar[%d]: type=%d size=%#llx pref=%d",
                        i, Int(type), size, Int(pref)))
                }
            }
        }
    }

    func testQueryInfo() {
        guard openUserClient() else { return }
        // GFX version
        let (k1, o1) = callScalar(kSelQueryInfo,
                                  input: [kInfoGFXVersion],
                                  outCount: 3)
        if k1 == KERN_SUCCESS && o1.count >= 3 {
            append("gfx version: \(o1[0]).\(o1[1]).\(o1[2])")
        }
        // VRAM
        let (k2, o2) = callScalar(kSelQueryInfo,
                                  input: [kInfoVRAMSizes],
                                  outCount: 2)
        if k2 == KERN_SUCCESS && o2.count >= 2 {
            append("vram: visible=\(o2[0]) total=\(o2[1]) bytes")
        }
        // BringupReached
        let (k3, o3) = callScalar(kSelQueryInfo,
                                  input: [kInfoBringupReached],
                                  outCount: 1)
        if k3 == KERN_SUCCESS, let r = o3.first {
            append("bringup reached stage: \(r)")
        }
    }

    func testInitDeviceUpTo(_ stage: UInt64) {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelInitDevice,
                                   input: [stage],
                                   outCount: 1)
        if kr == KERN_SUCCESS, let reached = out.first {
            append("InitDevice(stage=\(stage)): ok, reached=\(reached)")
        } else {
            append(String(format:
                "InitDevice(stage=\(stage)): kr=%#x", kr))
        }
    }

    // MARK: Firmware

    /// Folder containing AMD microcode blobs. Always the
    /// `firmware/` directory inside the app bundle's Resources
    /// (Xcode's "Copy Files" build phase copies firmware/ in
    /// from the repo at build time — see project.yml).
    private var firmwareDir: URL? {
        Bundle.main.resourceURL?
            .appendingPathComponent("firmware", isDirectory: true)
    }

    /// Allocate a 32MB DMA buffer if we don't have one.
    private func ensureDMABuffer() -> Bool {
        guard openUserClient() else { return false }
        let size: UInt64 = 32 * 1024 * 1024
        let align: UInt64 = 16 * 1024
        let (kr, out) = callScalar(kSelAllocateDMA,
                                   input: [size, align],
                                   outCount: 2)
        if kr != KERN_SUCCESS {
            append(String(format: "AllocateDMABuffer: kr=%#x", kr))
            return false
        }
        if let segs = out.first, segs >= 1 {
            append("DMA buffer: \(segs) segment(s)")
        }
        return true
    }

    /// Load a single firmware file by type tag.
    private func loadFirmware(_ type: UInt64, _ filename: String) -> Bool {
        guard let dir = firmwareDir else {
            append("loadFirmware: no firmware dir set; click "
                   + "'Pick Firmware Folder…' first")
            return false
        }
        let url = dir.appendingPathComponent(filename)
        guard let data = try? Data(contentsOf: url) else {
            append("loadFirmware: can't read \(url.path)")
            return false
        }
        // Map the DMA buffer (memory type 6 = kMacAMDGPUMemoryTypeDMABuffer)
        // into our address space. kIOMapAnywhere = 1 → let the kernel
        // pick the address; without it, options=0 means "map AT
        // *addr" which is bad-arg when addr=0.
        var addr: mach_vm_address_t = 0
        var sz: mach_vm_size_t = 0
        let mapKr = IOConnectMapMemory64(ucConn, 6, mach_task_self_,
                                         &addr, &sz, 1 /* kIOMapAnywhere */)
        if mapKr != KERN_SUCCESS {
            append(String(format: "DMA map: kr=%#x", mapKr))
            return false
        }
        defer {
            IOConnectUnmapMemory64(ucConn, 6, mach_task_self_, addr)
        }
        if UInt64(data.count) > sz {
            append("loadFirmware: \(filename) is \(data.count) B "
                   + "but DMA buffer is only \(sz) B")
            return false
        }
        data.withUnsafeBytes { src in
            guard let base = src.baseAddress else { return }
            let dst = UnsafeMutableRawPointer(bitPattern: UInt(addr))!
            dst.copyMemory(from: base, byteCount: data.count)
        }
        let (kr, _) = callScalar(kSelLoadFirmware,
                                 input: [type, UInt64(data.count)],
                                 outCount: 1)
        if kr != KERN_SUCCESS {
            append(String(format:
                "LoadFirmware(%@) %@: kr=%#x",
                String(type, radix: 16), filename, kr))
            return false
        }
        append("LoadFirmware \(filename) (type=\(type)): ok")
        return true
    }

    // MARK: Firmware filename construction (mirrors upstream amdgpu)

    /// IP version triple in the same packed encoding as QueryInfo's
    /// kInfoIPVersions output: `(major << 16) | (minor << 8) | rev`.
    private struct IPVer {
        let major: Int
        let minor: Int
        let rev:   Int
        init(_ packed: UInt64) {
            major = Int((packed >> 16) & 0xFF)
            minor = Int((packed >> 8)  & 0xFF)
            rev   = Int(packed         & 0xFF)
        }
        var path: String { "\(major)_\(minor)_\(rev)" }
    }

    /// Discovered IP versions, filled by `runFullBringup`. We mirror
    /// upstream amdgpu_ucode's naming convention: each IP block has
    /// a per-version filename — `psp_<maj>_<min>_<rev>_sos.bin`,
    /// `smu_<maj>_<min>_<rev>.bin`, `gc_<maj>_<min>_<rev>_<part>.bin`,
    /// `sdma_<maj>_<min>_<rev>.bin`.
    private var ipPSP:  IPVer?
    private var ipSMU:  IPVer?
    private var ipSDMA: IPVer?
    private var ipGFX:  IPVer?

    /// Run IPDiscovery then read IP versions out of the dext via the
    /// QueryInfo selector. Sets the ipXxx fields.
    private func discoverIPs() -> Bool {
        // IPDiscovery first — populates the dext's IP base table.
        let (kr, _) = callScalar(kSelInitDevice, input: [1], outCount: 1)
        if kr != KERN_SUCCESS {
            append(String(format: "discoverIPs: IPDiscovery kr=%#x", kr))
            return false
        }
        // QueryInfo(IPVersions) → packed [GMC, SDMA, PSP, SMU].
        let (k1, ips) = callScalar(kSelQueryInfo,
                                   input: [kInfoIPVersions],
                                   outCount: 4)
        guard k1 == KERN_SUCCESS, ips.count >= 4 else {
            append("discoverIPs: QueryInfo(IPVersions) failed")
            return false
        }
        ipSDMA = IPVer(ips[1])
        ipPSP  = IPVer(ips[2])
        ipSMU  = IPVer(ips[3])
        // QueryInfo(GFXVersion) → three scalars [major, minor, rev].
        let (k2, gfx) = callScalar(kSelQueryInfo,
                                   input: [kInfoGFXVersion],
                                   outCount: 3)
        guard k2 == KERN_SUCCESS, gfx.count >= 3 else {
            append("discoverIPs: QueryInfo(GFXVersion) failed")
            return false
        }
        ipGFX = IPVer((gfx[0] << 16) | (gfx[1] << 8) | gfx[2])
        append("IP versions:"
               + " GFX=\(ipGFX!.path)"
               + " PSP=\(ipPSP!.path)"
               + " SMU=\(ipSMU!.path)"
               + " SDMA=\(ipSDMA!.path)")
        return true
    }

    /// One-shot: open user client, detect IP versions, load every
    /// firmware that amdgpu needs by name, drive every bring-up
    /// stage in order. Stops at the first failure for stages that
    /// gate later ones; logs and continues for ones that don't.
    ///
    /// Runs on a background task so the UI stays responsive — each
    /// InitDevice / LoadFirmware can block for up to ~10 s waiting
    /// for PSP / SMU mailbox responses.
    func runFullBringup() {
        guard !isWorking else {
            append("runFullBringup: already running")
            return
        }
        isWorking = true
        status = "bringup running…"
        statusColor = .orange
        Task.detached(priority: .userInitiated) { [weak self] in
            await self?.runFullBringupBlocking()
            await MainActor.run {
                self?.isWorking = false
                self?.status = "bringup done"
                self?.statusColor = .green
            }
        }
    }

    private func runFullBringupBlocking() async {
        guard openUserClient() else { return }
        guard ensureDMABuffer() else { return }
        guard discoverIPs() else { return }

        // Filenames derived from the discovered IP versions, mirroring
        // upstream amdgpu's per-IP fw_name construction:
        //   psp_v14_0_init_microcode  → psp_{v}_sos.bin
        //   smu_v14_0_init_microcode  → smu_{v}.bin
        //   sdma_v7_0_init_microcode  → sdma_{v}.bin
        //   gfx_v12_0_init_microcode  → gc_{v}_<part>.bin
        let psp  = ipPSP!.path
        let smu  = ipSMU!.path
        let sdma = ipSDMA!.path
        let gfx  = ipGFX!.path

        // Stage 2: PSPInit (no firmware needed — just alloc fw_pri).
        testInitDeviceUpTo(2)

        // Stages 3–5: PSPLoadSOS → PSPRingCreate → TMRSetup.
        if !loadFirmware(kFwSOS, "psp_\(psp)_sos.bin") {
            append("runFullBringup: stop — PSP SOS load failed")
            return
        }
        for s: UInt64 in [3, 4, 5] { testInitDeviceUpTo(s) }

        // Stage 6: SMUInit (needs PMFW).
        if loadFirmware(kFwIP_SMU, "smu_\(smu).bin") {
            testInitDeviceUpTo(6)
        }

        // Stages 7/12/9: GMCInit, IHInit, RLCInit — no firmware.
        for s: UInt64 in [7, 12, 9] { testInitDeviceUpTo(s) }

        // SDMA microcode (per-instance, same IP version on RDNA4).
        if loadFirmware(kFwIP_SDMA0, "sdma_\(sdma).bin") {
            _ = loadFirmware(kFwIP_SDMA1, "sdma_\(sdma).bin")
            testInitDeviceUpTo(14)  // SDMAInit
        }

        // GFX microcode: RLC + uni-MES — then MES + CP init.
        _ = loadFirmware(kFwIP_RLC_G,    "gc_\(gfx)_rlc.bin")
        _ = loadFirmware(kFwIP_RS64_MES, "gc_\(gfx)_uni_mes.bin")
        testInitDeviceUpTo(11)  // MESInit
        testInitDeviceUpTo(10)  // CPInit

        append("runFullBringup: done — see log for stage outcomes")
    }

    // MARK: Version reporting

    /// Read the version from the dext embedded inside this app bundle.
    private func readBundledVersion() -> String {
        let dextURL = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(dextBundleIdentifier + ".dext")
        let plistURL = dextURL.appendingPathComponent("Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(
                  from: data, options: [], format: nil) as? [String: Any]
        else { return "?" }
        let short = plist["CFBundleShortVersionString"] as? String ?? "?"
        let build = plist["CFBundleVersion"] as? String ?? "?"
        return "\(short) (\(build))"
    }

    /// Kick a propertiesRequest at sysextd to get the installed dext's
    /// version. Result comes back via the delegate
    /// `request(_:foundProperties:)`.
    func refreshVersions() {
        bundledVersion = readBundledVersion()
        installedVersion = "checking…"
        versionMatch = false
        let req = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: dextBundleIdentifier,
            queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    // MARK: helpers

    private func append(_ line: String) {
        let ts = ISO8601DateFormatter().string(from: Date())
        let msg = "[\(ts)] \(line)\n"
        log += msg
        NSLog("%@", msg)
    }
}
