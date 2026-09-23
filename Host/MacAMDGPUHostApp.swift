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
//   2. Once running from /Applications: check the installed build,
//      then activate only if it is not already enabled. macOS may prompt
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

// The identity is produced by the responding binary, independent of sysextd's
// installed-version metadata. An old or unknown ABI cannot authorize GPU work.
private struct DriverRuntimeIdentity: Sendable {
    let build: UInt64?
    init(transport: Int32, output: [UInt64], count: UInt32) {
        build = transport == 0 && count == 3 && output.count == 3 &&
            output[0] == 0x414D444750554142 && output[1] == 1 && output[2] > 0
                ? output[2] : nil
    }
    func permitsHardware(expectedBuild: UInt64?) -> Bool {
        guard let build, let expectedBuild else { return false }
        return build == expectedBuild
    }
    func permitsConnection(expectedBuild: UInt64?, allowUnverified: Bool) -> Bool {
        allowUnverified || permitsHardware(expectedBuild: expectedBuild)
    }
}

private struct DriverRuntimeProbe: Sendable {
    let connectionError: Int32
    let identity: DriverRuntimeIdentity
}

// MARK: - User-client selectors (must match dext/MacAMDGPU.cpp)

private let kSelPing:            UInt32 = 0
private let kSelGetIdentity:     UInt32 = 1
private let kSelGetBARInfo:      UInt32 = 2
private let kSelRuntimeBuild:    UInt32 = 43
private let kSelHostMemoryTest:  UInt32 = 44
private let kSelComputeTest:     UInt32 = 45
private let kSelShutdownGPU:     UInt32 = 42
private let kSelGetReBARInfo:    UInt32 = 41
private let kSelAllocateDMA:     UInt32 = 6
private let kSelResetDevice:     UInt32 = 8
private let kSelInitDevice:      UInt32 = 9
private let kSelLoadFirmware:    UInt32 = 10
private let kSelQueryInfo:       UInt32 = 21
private let kSelGetDiagnostics:  UInt32 = 23
private let kSelDumpTMR:         UInt32 = 24
private let kSelDumpPSP:         UInt32 = 25
private let kSelDumpCmdBuf:      UInt32 = 26
// v0.1.24 — runtime engine health + DPM toggle.
private let kSelLiveStatus:          UInt32 = 30
private let kSelDisableSmuFeatures:  UInt32 = 33
// v0.1.25 — VRAM->VRAM SDMA copy smoke test.
private let kSelSDMACopyVRAM:        UInt32 = 34
// v0.1.26 — KIQ PM4 NOP+fence smoke test.
private let kSelCPKIQSmoke:          UInt32 = 35
// v0.1.28 — command-stream submission ABI.
private let kSelSubmitIB:        UInt32 = 19
private let kSelWaitFence:       UInt32 = 20
private let kSelCSCreate:        UInt32 = 37
private let kSelCSWriteDwords:   UInt32 = 38
private let kSelCSDestroy:       UInt32 = 39

// CS IP types — match kMacAMDGPUCSIPType* in dext/MacAMDGPU.cpp.
private let kCSIPTypeSDMA:    UInt64 = 0
private let kCSIPTypeGFX:     UInt64 = 1
private let kCSIPTypeCompute: UInt64 = 2

// v0.1.29 — Per-state GFXCLK soft-clamp.
private let kSelSetPowerState:       UInt32 = 40

// v0.1.27 — BO management ABI. Selectors 16–18 existed pre-v0.1.27
// (legacy: bump-allocate a sub-range of the client DMA buffer). They
// now also accept the new (size, domain, alignment, flags) shape for
// real VRAM / GTT BOs. Selector 36 is the new BOMap.
private let kSelBOAlloc:   UInt32 = 16
private let kSelBOFree:    UInt32 = 17
private let kSelBOGetInfo: UInt32 = 18
private let kSelBOMap:     UInt32 = 36

// BO domains — must match dext/MacAMDGPU.cpp:kBODomain*.
private let kBODomainGTTLegacy: UInt64 = 0
private let kBODomainVRAM:      UInt64 = 1
private let kBODomainGTT:       UInt64 = 2

// Memory type IDs for IOConnectMapMemory64. The high range
// (kMacAMDGPUMemoryTypeBOBase = 0x10000 onward) is per-BO mappings as
// returned by BOMap.
private let kMemTypeDMABuffer: UInt32 = 6
private let kMemTypeIRQState:  UInt32 = 7
private let kMemTypeBOBase:    UInt32 = 0x10000

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
// psp_<chip>_ta.bin — Trusted Application package. Currently we
// extract just the ASD sub-binary; psp_asd_initialize runs between
// AUTOLOAD_RLC and psp_rl_load (amdgpu_psp.c:3153). Required for the
// PSP GC autoload state machine to fire on R9700 (v0.1.18 symptom:
// BOOTLOAD_STATUS stuck at 0 without this).
private let kFwTA:          UInt64 = 9
private let kFwIP_SMU:      UInt64 = 0x100 + 18
// SDMA on RDNA4 / gfx12 = sdma_v7_1 = RS64 SDMA, packaged as a single
// firmware. Upstream `amdgpu_sdma_init_microcode` for v3.0 headers
// loads it ONCE as GFX_FW_TYPE_SDMA_UCODE_TH0 (= 71). The legacy
// SDMA0=9 / SDMA1=10 fw_types are for older sdma_v4-style headers
// where the two instances each had a separate ucode blob.
private let kFwIP_SDMA_TH0: UInt64 = 0x100 + 71
private let kFwIP_RLC_G:    UInt64 = 0x100 + 8
// GFX12 / RDNA4 uses RS64 CP firmwares (separate from legacy ME/PFP/MEC).
// Per upstream `amdgpu_psp_get_fw_type` mapping for AMDGPU_UCODE_ID_CP_RS64_*:
//   RS64_PFP=87, RS64_ME=88, RS64_MEC=89,
//   RS64_PFP_P0_STACK=90, _P1=91, ME_P0=92, ME_P1=93,
//   MEC_P0=94, MEC_P1=95, MEC_P2=96, MEC_P3=97.
private let kFwIP_CP_RS64_PFP:     UInt64 = 0x100 + 87
private let kFwIP_CP_RS64_ME:      UInt64 = 0x100 + 88
private let kFwIP_CP_RS64_MEC:     UInt64 = 0x100 + 89
private let kFwIP_CP_RS64_PFP_P0:  UInt64 = 0x100 + 90
private let kFwIP_CP_RS64_PFP_P1:  UInt64 = 0x100 + 91
private let kFwIP_CP_RS64_ME_P0:   UInt64 = 0x100 + 92
private let kFwIP_CP_RS64_ME_P1:   UInt64 = 0x100 + 93
private let kFwIP_CP_RS64_MEC_P0:  UInt64 = 0x100 + 94
private let kFwIP_CP_RS64_MEC_P1:  UInt64 = 0x100 + 95
private let kFwIP_CP_RS64_MEC_P2:  UInt64 = 0x100 + 96
private let kFwIP_CP_RS64_MEC_P3:  UInt64 = 0x100 + 97
// uni_mes ships both code and data; upstream emits two LOAD_IP_FW
// frames per uni_mes file — CP_MES (=33) for the ucode portion, then
// MES_STACK (=34) for the data portion. 76 (RS64_MES) was the standalone
// MES ucode fw_type — wrong for the uni_mes packaging on RDNA4.
private let kFwIP_CP_MES:      UInt64 = 0x100 + 33
private let kFwIP_CP_MES_DATA: UInt64 = 0x100 + 34

// 0x200+ — per-file multi-payload firmware load. The dext expands one
// of these into N LOAD_IP_FW frames via amdgpu_ucode_extract, so the
// host doesn't need to know each .bin's internal layout. Use these
// when loading IPs whose .bin emits more than one psp_gfx_fw_type
// (rlc.bin, uni_mes.bin, imu.bin, pfp/me/mec .bin). Keep in sync with
// the kMacAMDGPUFwTypeFile_* enum in dext/MacAMDGPU.cpp.
//
// Cited refs (per file):
//   SDMA   sdma_v7_1 (gfx12) — amdgpu_sdma.c:291-299, header_v3_0
//   RLC    gfx_v12_0_init_microcode — gfx_v12_0.c:617-633
//   IMU    imu_v12_0_init_microcode — imu_v12_0.c:60-75
//   MES    amdgpu_mes_init_microcode — amdgpu_mes.c:719-743 (uni_mes)
//   CP     gfx_v12_0_init_microcode — gfx_v12_0.c:600-642 (RS64 pfp/me/mec)
private let kFwFile_SDMA:    UInt64 = 0x200 + 0
private let kFwFile_RLC:     UInt64 = 0x200 + 1
private let kFwFile_IMU:     UInt64 = 0x200 + 2
private let kFwFile_MES_UNI: UInt64 = 0x200 + 3
private let kFwFile_CP_PFP:  UInt64 = 0x200 + 4
private let kFwFile_CP_ME:   UInt64 = 0x200 + 5
private let kFwFile_CP_MEC:  UInt64 = 0x200 + 6
// gc_<v>_toc.bin — table-of-contents. Required for autoload-supported
// chips (psp_v14_0_3 / R9700): PSP parses it after ring_create to
// compute TMR layout. Without it, SDMA/CP/MES LOAD_IP_FW frames are
// rejected with TEE_BAD_PARAMETERS = 0xFFFF0006.
private let kFwFile_TOC:     UInt64 = 0x200 + 7

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

/// Small caption-styled label that prefixes each grouped row of
/// control buttons (Identity / Diagnostics / Engines / Power).
struct GroupLabel: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View {
        Text(text)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            .frame(width: 80, alignment: .leading)
    }
}

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
                        .foregroundStyle(.secondary)
                }
                Spacer()
                VStack(alignment: .leading, spacing: 2) {
                    Text("Running").font(.caption2).foregroundStyle(.secondary)
                    Text(controller.runningVersion)
                        .font(.system(.callout, design: .monospaced))
                        .foregroundStyle(controller.runningVersionMatch ? Color.green : Color.orange)
                }
                Button("Verify Running") { controller.verifyRunningDriver() }
                    .disabled(controller.isWorking)
                    .controlSize(.small)
                Button("Refresh") {
                    controller.refreshVersions()
                    controller.verifyRunningDriver()
                }
                    .disabled(controller.isWorking)
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
            // Grouped control panel. Each row is one category; the
            // big "Initialize GPU" sits above all the test buttons.
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Button("Initialize GPU") { controller.initializeGPU() }
                        .help("Run every bringup stage in order, printing each as it completes.")
                    Button("Load Firmware Only") {
                        controller.initializeGPU(stopAfterFirmware: true)
                    }
                    .help("Stop after PSP firmware loading, before SMU/CP/MES setup. Initialize GPU can continue from this checkpoint.")
                    Button("Stop GPU") { controller.shutdownGPU() }
                        .help("Reset the GPU with DMA disabled, close PCI, and release the session. Does not power off the enclosure.")
                    Button("Restart GPU") { controller.shutdownGPU(reinitialize: true) }
                        .help("Stop the GPU safely, then load firmware and initialize a fresh session without unplugging it.")
                    Spacer()
                }

                // Row 1 — Identity / discovery.
                HStack(spacing: 6) {
                    GroupLabel("Identity")
                    Button("Ping") { controller.testPing() }
                    Button("Identity") { controller.testGetIdentity() }
                    Button("BARs") { controller.testGetBARInfo() }
                    Button("Query") { controller.testQueryInfo() }
                    Spacer()
                }

                // Row 2 — Diagnostics + memory dumps.
                HStack(spacing: 6) {
                    GroupLabel("Diagnostics")
                    Button("Diagnostics") { controller.testGetDiagnostics() }
                        .help("PCI config + PM cap + IFWI + BAR0/2/5 MMIO probes.")
                    Button("Live Status") { controller.testLiveStatus() }
                        .help("Snapshot GRBM/CP/RLC/SDMA0/1/SMU running features — proves the dext+GPU are still responsive.")
                    Button("Dump TMR") { controller.testDumpTMR() }
                        .help("Read 16 dwords from VRAM at (vram_size - 64 KB) via MM_INDEX/DATA.")
                    Button("Dump PSP") { controller.testDumpPSP() }
                        .help("Read SOC15-resolved MP0 C2PMSG_33/35/36/64/81 registers.")
                    Button("Dump Cmd") { controller.testDumpCmdBuf() }
                        .help("Read VRAM cmd_buf + fence + ring after a submit attempt.")
                    Spacer()
                }

                // Row 3 — Engine smoke tests.
                HStack(spacing: 6) {
                    GroupLabel("Engines")
                    Button("BO Smoke") { controller.testBOSmoke() }
                        .help("v0.1.27 BO ABI smoke test: alloc VRAM + GTT BOs, round-trip GetInfo, map GTT BO, write pattern, free.")
                    Button("SDMA Copy") { controller.testSDMACopyVRAM() }
                        .help("VRAM→VRAM 4 KB SDMA COPY_LINEAR smoke test. Proves the SDMA engine processes a packet end-to-end + writes its fence.")
                    Button("CP GFX Fence") { controller.testCPKIQSmoke() }
                        .help("Verify a CP register-write packet first, then submit and verify a GFX memory fence.")
                    Button("CS Smoke") { controller.testCSSmoke() }
                        .help("v0.1.28 — Drive the new CS submission ABI: CSCreate(SDMA) → CSWriteDwords(4×NOP) → SubmitIB → WaitFence(1s) → CSDestroy.")
                    Button("GFX CS Smoke") { controller.testCSSmoke(gfx: true) }
                        .help("Submit PM4 NOP packets through a GFX command-stream handle and verify its memory fence.")
                    Spacer()
                }

                HStack(spacing: 6) {
                    GroupLabel("Memory")
                    Button("Host Memory Copy") { controller.testHostMemoryTransfer() }
                        .help("Verify 16 KB in each direction between host memory and VRAM through GART, then unbind the DMA mapping.")
                    Button("Compute Smoke") { controller.testCompute() }
                        .help("Run a 32-thread shader that reads, adds and writes values; verify every result and surrounding guard words.")
                    Spacer()
                }

                // Row 4 — Power state / fan control.
                HStack(spacing: 6) {
                    GroupLabel("Power")
                    Button("Auto") { controller.testSetPowerState(0) }
                        .help("v0.1.29 — clear GFXCLK soft-clamp (PMFW picks).")
                    Button("Low") { controller.testSetPowerState(1) }
                        .help("v0.1.29 — cap GFXCLK at 200 MHz.")
                    Button("Nominal") { controller.testSetPowerState(2) }
                        .help("v0.1.29 — same as Auto: no GFXCLK clamp.")
                    Button("High") { controller.testSetPowerState(3) }
                        .help("v0.1.29 — floor GFXCLK at 1500 MHz.")
                    Button("Peak") { controller.testSetPowerState(4) }
                        .help("v0.1.29 — pin GFXCLK to 2400 MHz (compute).")
                    Divider().frame(height: 16)
                    Button("Quiet Fan") { controller.testDisableSmuFeatures() }
                        .help("DisableAllSmuFeatures (PPSMC 0x7) — parks DPM so PMFW stops defaulting fan to MAX. Re-run Initialize GPU to undo.")
                    Spacer()
                }
            }
            .font(.caption)
            .disabled(controller.isWorking)
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
    @Published var runningVersion: String = "not verified"
    @Published var runningVersionMatch: Bool = false
    private var lastConnectionError: Int32 = kIOReturnNotFound
    private var runtimeVerificationInProgress = false

    // Cached PCI identity, populated by the first testGetIdentity().
    // Used to pick the kicker firmware variant where required (R9700
    // rev 0xC8 needs psp_14_0_3_sos_kicker.bin etc).
    var pciDeviceId: UInt16 = 0
    var pciRevision: UInt8 = 0

    private var didAutoActivate = false
    private var activationCheck: OSSystemExtensionRequest?
    private var lifecycleRequest: OSSystemExtensionRequest?
    private var lifecycleIsDeactivation = false
    private var cancelledIdenticalReplacement = false

    // MARK: Startup flow

    func runStartupFlow() async {
        let bundlePath = Bundle.main.bundlePath
        append("launched from \(bundlePath)")

        if !bundlePath.hasPrefix("/Applications/") {
            refreshVersions()
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
            append("running from /Applications — checking whether activation is needed")
            requestActivate()
        }
    }

    // MARK: Step 1 — install to /Applications

    func installToApplications() {
        guard !isWorking else {
            append("install deferred: another operation is running")
            return
        }
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
        config.createsNewApplicationInstance = true
        NSWorkspace.shared.openApplication(at: appURL,
                                            configuration: config) { app, error in
            Task { @MainActor in
                if let error {
                    self.append("relaunch failed: \(error.localizedDescription)")
                    self.status = "relaunch failed"
                    self.statusColor = .red
                    self.isWorking = false
                    return
                }
                guard let app, !app.isTerminated,
                      app.processIdentifier != ProcessInfo.processInfo.processIdentifier,
                      app.bundleURL?.standardizedFileURL == appURL.standardizedFileURL
                else {
                    self.append("relaunch did not confirm a separate /Applications instance; keeping this window open")
                    self.status = "relaunch failed"
                    self.statusColor = .red
                    self.isWorking = false
                    return
                }
                self.append("relaunch confirmed: PID \(app.processIdentifier)")
                // Give the confirmed new instance a beat to claim the foreground.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                    NSApp.terminate(nil)
                }
            }
        }
    }

    // MARK: Step 2 — activate / deactivate the dext

    func requestActivate() {
        guard !isWorking, activationCheck == nil, lifecycleRequest == nil else {
            append("activation deferred: another operation or approval is pending")
            return
        }
        isWorking = true
        bundledVersion = readBundledVersion()
        status = "checking installed driver…"
        statusColor = .orange
        let req = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: dextBundleIdentifier, queue: .main)
        activationCheck = req
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    private func submitActivation() {
        status = "preparing driver upgrade…"
        statusColor = .orange
        Task { [weak self] in
            guard let self else { return }
            // Compatible legacy drivers may be stopped even though they cannot
            // answer the new runtime identity query. Never initialize them.
            if self.openUserClient(allowUnverified: true) {
                guard await self.stopGPUConnection(), self.closeUserClientForLifecycle() else {
                    self.append("activation cancelled: the old GPU session could not be stopped safely")
                    self.isWorking = false
                    return
                }
            } else if self.lastConnectionError != kIOReturnNotFound {
                self.append("activation cancelled: the existing driver could not be contacted for safe shutdown")
                self.isWorking = false
                return
            }
            self.status = "activating…"
            self.statusColor = .orange
            self.append("OSSystemExtensionRequest.activationRequest(\(self.dextBundleIdentifier))")
            let req = OSSystemExtensionRequest.activationRequest(
                forExtensionWithIdentifier: self.dextBundleIdentifier, queue: .main)
            self.lifecycleRequest = req
            self.lifecycleIsDeactivation = false
            self.cancelledIdenticalReplacement = false
            req.delegate = self
            OSSystemExtensionManager.shared.submitRequest(req)
        }
    }

    func requestDeactivate() {
        guard !isWorking, activationCheck == nil, lifecycleRequest == nil else {
            append("deactivation deferred: another operation or approval is pending")
            return
        }
        isWorking = true
        status = "preparing driver deactivation…"
        statusColor = .orange
        Task { [weak self] in
            guard let self else { return }
            if self.openUserClient(allowUnverified: true) {
                guard await self.stopGPUConnection(), self.closeUserClientForLifecycle() else {
                    self.append("deactivation cancelled: the GPU session could not be stopped safely")
                    self.isWorking = false
                    return
                }
            } else if self.lastConnectionError != kIOReturnNotFound {
                self.append("deactivation cancelled: the driver could not be contacted for safe shutdown")
                self.isWorking = false
                return
            }
            self.status = "deactivating…"
            self.append("OSSystemExtensionRequest.deactivationRequest")
            let req = OSSystemExtensionRequest.deactivationRequest(
                forExtensionWithIdentifier: self.dextBundleIdentifier, queue: .main)
            self.lifecycleRequest = req
            self.lifecycleIsDeactivation = true
            self.cancelledIdenticalReplacement = false
            req.delegate = self
            OSSystemExtensionManager.shared.submitRequest(req)
        }
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction
    {
        guard request === lifecycleRequest else { return .cancel }
        if existing.bundleShortVersion == ext.bundleShortVersion &&
           existing.bundleVersion == ext.bundleVersion {
            cancelledIdenticalReplacement = true
            append("identical driver build \(ext.bundleShortVersion) (\(ext.bundleVersion)); replacement cancelled — increment the build number for changed drivers")
            return .cancel
        }
        append("replacing existing \(existing.bundleShortVersion) (\(existing.bundleVersion)) → "
               + "\(ext.bundleShortVersion) (\(ext.bundleVersion))")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        guard request === lifecycleRequest else { return }
        append("user approval required — open System Settings → General "
               + "→ Login Items & Extensions → Driver Extensions, toggle "
               + "MacAMDGPU on")
        status = "approval required"
        statusColor = .yellow
        // The activation remains pending while System Settings handles approval.
        // Keep lifecycle operations and hardware tests blocked until its callback.
        // Pop System Settings to the right pane (Tahoe path).
        if let url = URL(string:
            "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result)
    {
        guard request === lifecycleRequest else { return }
        lifecycleRequest = nil
        isWorking = false
        switch result {
        case .completed:
            append("request completed")
            status = lifecycleIsDeactivation ? "removed" : "registered — verifying attachment"
            statusColor = lifecycleIsDeactivation ? .green : .orange
            if lifecycleIsDeactivation {
                runningVersion = "not connected"
                runningVersionMatch = false
            } else {
                startRuntimeVerification()
            }
        case .willCompleteAfterReboot:
            append("request will complete after reboot")
            status = "reboot required"
            statusColor = .yellow
        @unknown default:
            append("request finished with unknown result \(result.rawValue)")
            status = "unknown"
            statusColor = .secondary
        }
        refreshVersions()
    }

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties])
    {
        defer {
            if request === activationCheck {
                activationCheck = nil
                let exactEnabled = properties.contains {
                    $0.isEnabled && !$0.isUninstalling &&
                    "\($0.bundleShortVersion) (\($0.bundleVersion))" == bundledVersion
                }
                if exactEnabled {
                    append("driver \(bundledVersion) is already enabled; activation skipped")
                    status = "registered — verifying attachment"
                    statusColor = .orange
                    startRuntimeVerification()
                } else {
                    submitActivation()
                }
            }
        }
        if properties.isEmpty {
            installedVersion = "not installed"
            versionMatch = false
            return
        }
        // macOS keeps zombie 'waiting to uninstall on reboot' entries
        // alongside the live one across version bumps. Prefer the
        // currently enabled registration (not proof of a running process), else
        // fall back to the highest version we have on disk.
        let active = properties.first {
                        $0.isEnabled && !$0.isUninstalling
                     }
                  ?? properties.sorted { lhs, rhs in
                        lhs.bundleVersion.compare(rhs.bundleVersion,
                            options: .numeric) == .orderedDescending
                     }.first
                  ?? properties[0]
        let short = active.bundleShortVersion
        let build = active.bundleVersion
        installedVersion = "\(short) (\(build))"
        // Compare against the bundled one we already computed.
        versionMatch = bundledVersion == "\(short) (\(build))"
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
        if request === activationCheck {
            activationCheck = nil
            isWorking = false
            append("installed-driver check failed; activation was not submitted")
        } else if request === lifecycleRequest {
            lifecycleRequest = nil
            isWorking = false
            let nsError = error as NSError
            if cancelledIdenticalReplacement &&
               nsError.domain == OSSystemExtensionErrorDomain &&
               nsError.code == OSSystemExtensionError.Code.requestCanceled.rawValue {
                cancelledIdenticalReplacement = false
                append("identical-build replacement cancelled; existing driver was left unchanged")
                status = "replacement skipped"
                statusColor = .secondary
                refreshVersions()
                return
            }
        } else {
            // A Refresh failure must not unlock a concurrent activation or bringup.
            append("version refresh failed: \(error.localizedDescription)")
            return
        }
        append("request failed: \(error.localizedDescription)")
        if let osErr = error as? OSSystemExtensionError {
            append("  OSSystemExtensionError code=\(osErr.errorCode)")
        }
        status = "error"
        statusColor = .red
    }

    // MARK: User-client connection (talks to the dext directly)

    private var ucConn: io_connect_t = 0

    private func closeUserClientForLifecycle() -> Bool {
        guard ucConn != 0 else { return true }
        let kr = IOServiceClose(ucConn)
        guard kr == KERN_SUCCESS else {
            append(String(format: "user-client close failed: %#x; lifecycle request not submitted", kr))
            status = "user-client close failed"
            statusColor = .red
            return false
        }
        ucConn = 0
        append("user client closed before driver lifecycle request")
        return true
    }

    /// Reset-based teardown is separate from extension activation. A successful
    /// stop gives Initialize a fresh context without asking macOS to unload the
    /// driver binary; binary replacement still uses Install Driver.
    func shutdownGPU(reinitialize: Bool = false) {
        guard !isWorking, activationCheck == nil, lifecycleRequest == nil else {
            append("GPU stop deferred: another operation or approval is pending")
            return
        }
        guard openUserClient(allowUnverified: true) else { return }
        isWorking = true
        Task { [weak self] in
            guard let self else { return }
            guard await self.stopGPUConnection(), self.closeUserClientForLifecycle() else {
                self.isWorking = false
                return
            }
            self.pciDeviceId = 0
            self.pciRevision = 0
            self.status = "GPU stopped — ready to initialize"
            self.statusColor = .green
            self.isWorking = false
            if reinitialize { self.initializeGPU() }
        }
    }

    // Shared by explicit Stop/Restart and activation preparation. FLR is a
    // blocking RPC, so it runs away from the UI actor with controls disabled.
    private func stopGPUConnection() async -> Bool {
        guard ucConn != 0 else { return false }
        let connection = ucConn
        status = "stopping GPU…"
        statusColor = .orange
        append("Stop GPU: block submissions → disable DMA → drain PCIe → function reset → release session")
        let result = await Task.detached(priority: .userInitiated) {
            var output = [UInt64](repeating: 0, count: 2)
            var count: UInt32 = 2
            let kr = IOConnectCallScalarMethod(connection, kSelShutdownGPU,
                                               nil, 0, &output, &count)
            return (kr, output, count)
        }.value
        let (kr, output, count) = result
        let operation = kr == KERN_SUCCESS && count == 2
            ? UInt32(truncatingIfNeeded: output[0]) : UInt32(bitPattern: kr)
        guard kr == KERN_SUCCESS, count == 2, operation == 0, output[1] == 6 else {
            append(String(format: "Stop GPU failed: transport=%#x operation=%#x phase=%llu", kr, operation, output[1]))
            append("Session backing was not released by Stop GPU. Close other clients or mapped/interrupt sessions before retrying; an unavailable reset may still require a hardware power cycle.")
            status = "GPU stop failed — see log"
            statusColor = .red
            return false
        }
        append("Stop GPU complete: function reset succeeded, PCI closed, session resources released. The enclosure remains powered.")
        return true
    }

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
    func openUserClient(allowUnverified: Bool = false) -> Bool {
        guard activationCheck == nil, lifecycleRequest == nil, !runtimeVerificationInProgress else {
            append("hardware operation deferred: driver lifecycle request or approval is pending")
            return false
        }
        if ucConn == 0 {
            let opened = Self.openDriverConnection(bundleID: dextBundleIdentifier)
            lastConnectionError = opened.0
            guard opened.0 == KERN_SUCCESS else {
                append(String(format: "openUserClient: no usable driver connection (kr=%#x)", opened.0))
                return false
            }
            ucConn = opened.1
        }
        let identity = Self.queryRuntime(connection: ucConn)
        updateRuntimeDisplay(identity)
        guard identity.permitsConnection(expectedBuild: expectedDriverBuild,
                                          allowUnverified: allowUnverified) else {
            append("GPU operation blocked: responding driver build does not match the bundled build; installed metadata does not prove attachment")
            _ = closeUserClientForLifecycle()
            return false
        }
        lastConnectionError = KERN_SUCCESS
        return true
    }

    nonisolated private static func openDriverConnection(bundleID: String) -> (Int32, io_connect_t) {
        guard let raw = IOServiceMatching("IOUserService") else { return (kIOReturnNotFound, 0) }
        var iter: io_iterator_t = 0
        let matched = IOServiceGetMatchingServices(kIOMainPortDefault, raw as CFDictionary, &iter)
        guard matched == KERN_SUCCESS else { return (matched, 0) }
        defer { IOObjectRelease(iter) }
        var svc = IOIteratorNext(iter)
        while svc != 0 {
            var props: Unmanaged<CFMutableDictionary>?
            let kr = IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0)
            let dict = props?.takeRetainedValue() as? [String: Any]
            if kr == KERN_SUCCESS,
               dict?["CFBundleIdentifier"] as? String == bundleID || dict?["IOUserClass"] as? String == "MacAMDGPU" {
                var conn: io_connect_t = 0
                let opened = IOServiceOpen(svc, mach_task_self_, 0, &conn)
                IOObjectRelease(svc)
                if opened != KERN_SUCCESS, conn != 0 { IOServiceClose(conn) }
                return (opened, opened == KERN_SUCCESS ? conn : 0)
            }
            IOObjectRelease(svc)
            svc = IOIteratorNext(iter)
        }
        return (kIOReturnNotFound, 0)
    }

    nonisolated private static func queryRuntime(connection: io_connect_t) -> DriverRuntimeIdentity {
        var output = [UInt64](repeating: 0, count: 3)
        var count: UInt32 = 3
        let kr = IOConnectCallScalarMethod(connection, kSelRuntimeBuild, nil, 0, &output, &count)
        return DriverRuntimeIdentity(transport: kr, output: output, count: count)
    }

    private func updateRuntimeDisplay(_ identity: DriverRuntimeIdentity) {
        runningVersionMatch = identity.permitsHardware(expectedBuild: expectedDriverBuild)
        if let build = identity.build {
            runningVersion = runningVersionMatch ? "build \(build) verified" : "build \(build) — differs"
        } else {
            runningVersion = "unverified / older driver"
        }
    }

    func verifyRunningDriver() {
        guard !isWorking, activationCheck == nil, lifecycleRequest == nil else { return }
        startRuntimeVerification()
    }

    nonisolated private static func probeRuntime(bundleID: String,
                                                   existingConnection: io_connect_t) -> DriverRuntimeProbe {
        if existingConnection != 0 {
            // Verification of an active session must not close its PCI owner.
            return DriverRuntimeProbe(connectionError: KERN_SUCCESS,
                                      identity: queryRuntime(connection: existingConnection))
        }
        let opened = openDriverConnection(bundleID: bundleID)
        guard opened.0 == KERN_SUCCESS else {
            return DriverRuntimeProbe(connectionError: opened.0,
                identity: DriverRuntimeIdentity(transport: opened.0, output: [], count: 0))
        }
        defer { IOServiceClose(opened.1) }
        return DriverRuntimeProbe(connectionError: KERN_SUCCESS,
                                  identity: queryRuntime(connection: opened.1))
    }

    private func startRuntimeVerification() {
        guard !runtimeVerificationInProgress, activationCheck == nil, lifecycleRequest == nil else { return }
        runtimeVerificationInProgress = true
        isWorking = true
        runningVersionMatch = false
        runningVersion = "checking…"
        status = "verifying running driver…"
        statusColor = .orange
        let bundleID = dextBundleIdentifier
        let existingConnection = ucConn
        Task { [weak self] in
            guard let self else { return }
            defer {
                self.runtimeVerificationInProgress = false
                self.isWorking = false
            }
            var lastConnectionError: Int32 = kIOReturnNotFound
            // A finite retry window accommodates kernel rematching. Temporary
            // probes always close; an existing initialized session stays open.
            // These calls never open PCI, submit commands or reset hardware.
            for attempt in 0..<12 {
                let probe = await Task.detached(priority: .utility) {
                    Self.probeRuntime(bundleID: bundleID, existingConnection: existingConnection)
                }.value
                lastConnectionError = probe.connectionError
                self.updateRuntimeDisplay(probe.identity)
                if self.runningVersionMatch {
                    self.append("running driver verified directly: \(self.runningVersion)")
                    self.status = "driver ready — runtime verified"
                    self.statusColor = .green
                    return
                }
                if attempt < 11 { try? await Task.sleep(nanoseconds: 500_000_000) }
            }
            if lastConnectionError == kIOReturnNotFound {
                self.runningVersion = "no attached driver"
                self.status = "GPU unavailable — no driver service"
                self.append("No MacAMDGPU service is attached. Check that the GPU is connected; registration alone does not mean a driver has matched it.")
            } else {
                self.status = "driver handoff pending — see log"
                self.append("Responding driver is unverified or has a different build. macOS may still be retiring the old process; this app does not force-terminate it. GPU tests remain blocked.")
            }
            self.statusColor = .orange
        }
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

    // Mirrors upstream amdgpu_ucode.c kicker_device_list[]: cards that
    // need the `_kicker` firmware variant instead of the plain one.
    // (Tested 0xC0 as kicker in v0.0.81 — kicker SOS bootloader timed
    // out, confirming rev=0xC0 is non-kicker. Upstream list is correct.)
    private static let kickerDeviceList: [(device: UInt16, revision: UInt8)] = [
        (0x744B, 0x00),
        (0x7551, 0xC8),
    ]

    static func amdgpuIsKickerFw(deviceId: UInt16, revision: UInt8) -> Bool {
        return kickerDeviceList.contains { $0.device == deviceId && $0.revision == revision }
    }

    func testGetIdentity() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelGetIdentity, outCount: 7)
        if kr == KERN_SUCCESS && out.count >= 7 {
            // dext returns [bus, dev, fn, vid, did, class+progif, revision]
            let bus      = UInt8(out[0] & 0xFF)
            let dev      = UInt8(out[1] & 0xFF)
            let fn       = UInt8(out[2] & 0xFF)
            let vid      = UInt16(out[3] & 0xFFFF)
            let did      = UInt16(out[4] & 0xFFFF)
            let classProg = UInt32(out[5] & 0xFFFFFF)
            let rev      = UInt8(out[6] & 0xFF)
            self.pciDeviceId = did
            self.pciRevision = rev
            let kicker = DriverController.amdgpuIsKickerFw(deviceId: did, revision: rev)
            append(String(format:
                "identity: %02x:%02x.%x VID=%04x DID=%04x class+progif=%06x rev=%02x%@",
                bus, dev, fn, vid, did, classProg, rev,
                kicker ? " [KICKER fw variant]" : ""))
        } else {
            append(String(format: "identity: failed kr=%#x", kr))
        }
    }

    func testGetBARInfo() {
        guard openUserClient() else { return }
        for i in 0..<6 {
            let (kr, out) = callScalar(kSelGetBARInfo,
                                       input: [UInt64(i)], outCount: 3)
            guard kr == KERN_SUCCESS, out.count >= 3, out[1] != 0 else { continue }
            append(String(format: "BAR%d: memoryIndex=%llu size=%llu KiB type=%#llx",
                          i, out[0], out[1] >> 10, out[2]))
            let (rebarKr, r) = callScalar(kSelGetReBARInfo,
                                          input: [UInt64(i)], outCount: 6)
            if rebarKr == KERN_SUCCESS && r.count == 6 {
                let supported = (0..<28).filter { r[3] & (UInt64(1) << $0) != 0 }
                    .map { "\(UInt64(1) << $0) MiB" }.joined(separator: ", ")
                append("  ReBAR: supported [\(supported)], selected \(r[4] >> 20) MiB, macOS assigned \(r[5] >> 20) MiB")
                if r[4] != r[5] {
                    append("  ReBAR size differs from the macOS mapping; only the assigned mapping is usable.")
                }
            } else {
                append(String(format: "  ReBAR query unavailable: kr=%#x (absent capability, access error, or older driver)", rebarKr))
            }
        }
        append("ReBAR is queried only. This driver has no public PCIDriverKit API to request larger PCI bridge windows.")
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

    func testGetDiagnostics() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelGetDiagnostics, outCount: 16)
        if kr != KERN_SUCCESS {
            append(String(format: "diagnostics: kr=%#x", kr))
            return
        }
        guard out.count >= 16 else {
            append("diagnostics: short reply (\(out.count) words)")
            return
        }
        let cmd       = UInt16(out[0] & 0xFFFF)
        let status    = UInt16((out[0] >> 16) & 0xFFFF)
        let bar0Lo    = UInt32(out[1] & 0xFFFFFFFF)
        let bar0Hi    = UInt32(out[2] & 0xFFFFFFFF)
        let bar2Lo    = UInt32(out[3] & 0xFFFFFFFF)
        let bar2Hi    = UInt32(out[4] & 0xFFFFFFFF)
        let bar5Cfg   = UInt32(out[5] & 0xFFFFFFFF)
        let pmcsr     = UInt16(out[6] & 0xFFFF)
        let pmCapOff  = UInt8((out[6] >> 16) & 0xFF)
        let pmState   = pmcsr & 0x3
        let memEnable = (cmd & 0x2) != 0
        let busMaster = (cmd & 0x4) != 0
        let bar0Full  = (UInt64(bar0Hi) << 32) | UInt64(bar0Lo & ~UInt32(0xF))
        let bar2Full  = (UInt64(bar2Hi) << 32) | UInt64(bar2Lo & ~UInt32(0xF))

        append(String(format:
            "diag/config: cmd=%#06x (MEM=%@ BM=%@) status=%#06x",
            cmd, memEnable ? "1" : "0",
            busMaster ? "1" : "0", status))
        let memIdxBar0 = UInt8(out[15] & 0xFF)
        let memIdxBar2 = UInt8((out[15] >> 8) & 0xFF)
        let memIdxBar5 = UInt8((out[15] >> 16) & 0xFF)
        append(String(format:
            "diag/bar0: cfg=%#010x:%#010x base=%#018llx size=%llu memIdx=%u",
            bar0Hi, bar0Lo, bar0Full, out[7], memIdxBar0))
        append(String(format:
            "diag/bar2: cfg=%#010x:%#010x base=%#018llx size=%llu memIdx=%u",
            bar2Hi, bar2Lo, bar2Full, out[8], memIdxBar2))
        append(String(format:
            "diag/bar5: cfg=%#010x memIdx=%u",
            bar5Cfg, memIdxBar5))
        if pmCapOff != 0 {
            append(String(format:
                "diag/pm: cap@%#x PMCSR=%#06x state=D%u",
                pmCapOff, pmcsr, pmState))
        } else {
            append("diag/pm: no PM capability found")
        }
        append(String(format:
            "diag/mmio bar5 (REGISTERS): [0x000]=%#x [0x004]=%#x [0xDE3*4]=%#x",
            UInt32(out[9]  & 0xFFFFFFFF),
            UInt32(out[10] & 0xFFFFFFFF),
            UInt32(out[11] & 0xFFFFFFFF)))
        append(String(format:
            "diag/mmio bar0 (FRAMEBUFFER): [0x000]=%#x [0x004]=%#x",
            UInt32(out[12] & 0xFFFFFFFF),
            UInt32(out[13] & 0xFFFFFFFF)))
        let ifwi = UInt32(out[14] & 0xFFFFFFFF)
        let ifwiReady = (ifwi & 0x80000000) == 0x80000000
        append(String(format:
            "diag/ifwi: MP0_C2PMSG_33=%#010x %@",
            ifwi, ifwiReady ? "(ready)" : "(NOT READY — waiting for IFWI)"))
    }

    // Stage labels mirror BringupStage in dext/amdgpu/amdgpu_init.h.
    // Order updated by Agent D (audit #9 #1): IHInit + GMCInit moved
    // ahead of PSP per upstream amdgpu_device_ip_init phase1 ordering.
    private static let stageNames: [UInt64: String] = [
        1: "IPDiscovery", 2: "IHInit", 3: "GMCInit",
        4: "PSPInit", 5: "PSPLoadSOS", 6: "PSPRingCreate",
        7: "TMRSetup", 8: "PSPFwLoad", 9: "SMUInit", 10: "IMUInit",
        11: "RLCInit", 12: "CPInit (prepare)", 13: "MESInit",
        14: "GFXInit (queue resume)", 15: "SDMAInit"
    ]
    // Stage progression for initializeGPU(). LoadFirmware(SMU/IMU/RLC/CP/MES/SDMA)
    // is interleaved between TMRSetup and PSPFwLoad — see
    // initializeGPU() for the exact order.
    private static let stageOrder: [UInt64] = [
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    ]

    func testDumpTMR() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelDumpTMR, outCount: 16)
        if kr != KERN_SUCCESS {
            append(String(format: "dumpTMR: kr=%#x", kr))
            return
        }
        guard out.count >= 16 else {
            append("dumpTMR: short reply (\(out.count) words)")
            return
        }
        append("tmr/vram dump @ (vram_size - 1 MB), 16 dwords:")
        for row in 0..<4 {
            let i = row * 4
            append(String(format:
                "  [+%02x] %#010x %#010x %#010x %#010x",
                i * 4,
                UInt32(out[i]   & 0xFFFFFFFF),
                UInt32(out[i+1] & 0xFFFFFFFF),
                UInt32(out[i+2] & 0xFFFFFFFF),
                UInt32(out[i+3] & 0xFFFFFFFF)))
        }
        // Tell the user whether this looks like discovery (signature
        // 0x28211407 in dw0) or zeroes (PSP didn't populate).
        let dw0 = UInt32(out[0] & 0xFFFFFFFF)
        if dw0 == 0x28211407 {
            append("→ valid IP-discovery signature found in VRAM")
        } else if dw0 == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0 {
            append("→ VRAM at TMR offset is ZEROED (PSP didn't stage discovery)")
        } else {
            append(String(format:
                "→ dw0=%#010x, expected signature 0x28211407 — unexpected data",
                dw0))
        }
    }

    func testDumpPSP() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelDumpPSP, outCount: 16)
        if kr != KERN_SUCCESS {
            append(String(format: "dumpPSP: kr=%#x", kr))
            return
        }
        guard out.count >= 16 else {
            append("dumpPSP: short reply (\(out.count) words)")
            return
        }
        let mp0Base = UInt32(out[0] & 0xFFFFFFFF)
        if mp0Base == 0xFFFFFFFF {
            append("psp: MP0 base UNRESOLVED — run Initialize GPU first")
            return
        }
        let c33 = UInt32(out[1] & 0xFFFFFFFF)
        let c35 = UInt32(out[2] & 0xFFFFFFFF)
        let c36 = UInt32(out[3] & 0xFFFFFFFF)
        let c64 = UInt32(out[4] & 0xFFFFFFFF)
        let c81 = UInt32(out[5] & 0xFFFFFFFF)
        let ringCreated = out[6] != 0
        append(String(format: "psp/mp0_base: %#010x", mp0Base))
        append(String(format:
            "psp/C2PMSG_33 (IFWI ready, bit 31): %#010x %@",
            c33, (c33 & 0x80000000) != 0 ? "✓ READY" : "✗ NOT READY"))
        append(String(format:
            "psp/C2PMSG_35 (bootloader, bit 31): %#010x %@",
            c35, (c35 & 0x80000000) != 0 ? "✓ READY" : "✗ NOT READY"))
        append(String(format:
            "psp/C2PMSG_36 (fw buf addr [>>20]):  %#010x",
            c36))
        append(String(format:
            "psp/C2PMSG_64 (ring base low):       %#010x",
            c64))
        append(String(format:
            "psp/C2PMSG_81 (sOS sign-of-life, nonzero): %#010x %@",
            c81, c81 != 0 ? "✓ ALIVE" : "✗ NOT ALIVE"))
        append("psp/ring created: \(ringCreated)")
        let wptr = UInt32(out[11] & 0xFFFFFFFF)
        append(String(format: "psp/C2PMSG_67 (ring wptr, dwords): %#010x", wptr))
        let mmhubBase = UInt32(out[7] & 0xFFFFFFFF)
        if mmhubBase == 0xFFFFFFFF {
            append("psp/mmhub: UNRESOLVED — MMHUB IP base not discovered")
        } else {
            let fbBase = UInt32(out[8] & 0xFFFFFFFF)
            let fbTop  = UInt32(out[9] & 0xFFFFFFFF)
            let vramStart = out[10]
            append(String(format:
                "psp/mmhub_base: %#010x  FB_LOCATION_BASE=%#010x  FB_LOCATION_TOP=%#010x",
                mmhubBase, fbBase, fbTop))
            append(String(format:
                "psp/vram_start (MC addr): %#018llx  → expected C2PMSG_36 = %#x",
                vramStart, UInt32(vramStart >> 20) & 0xFFFFFFFF))
            let fbOff   = UInt32(out[12] & 0xFFFFFFFF)
            let ptLo    = UInt32(out[13] & 0xFFFFFFFF)
            let ptHi    = UInt32(out[14] & 0xFFFFFFFF)
            let ctxCntl = UInt32(out[15] & 0xFFFFFFFF)
            let ptFull = (UInt64(ptHi) << 32) | UInt64(ptLo)
            append(String(format:
                "psp/gart: FB_OFFSET=%#010x  PT_BASE=%#010x:%#010x (%#018llx)  CTX0_CNTL=%#010x  ENABLE=%@",
                fbOff, ptHi, ptLo, ptFull, ctxCntl,
                (ctxCntl & 1) != 0 ? "yes" : "NO"))
        }
    }

    func testDumpCmdBuf() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelDumpCmdBuf, outCount: 16)
        if kr != KERN_SUCCESS {
            append(String(format: "dumpCmdBuf: kr=%#x", kr))
            return
        }
        guard out.count >= 16 else {
            append("dumpCmdBuf: short reply (\(out.count) words)")
            return
        }
        append("cmd_buf header [+0..+15]:")
        append(String(format: "  buf_size=%#x  buf_version=%#x  cmd_id=%#x  resp_lo=%#x",
            UInt32(out[0] & 0xFFFFFFFF), UInt32(out[1] & 0xFFFFFFFF),
            UInt32(out[2] & 0xFFFFFFFF), UInt32(out[3] & 0xFFFFFFFF)))
        append(String(format: "cmd_buf payload [+64..+79]: %#010x %#010x %#010x %#010x",
            UInt32(out[4] & 0xFFFFFFFF), UInt32(out[5] & 0xFFFFFFFF),
            UInt32(out[6] & 0xFFFFFFFF), UInt32(out[7] & 0xFFFFFFFF)))
        append(String(format: "cmd_buf resp status [+864..+879]: %#010x %#010x %#010x %#010x",
            UInt32(out[8] & 0xFFFFFFFF), UInt32(out[9] & 0xFFFFFFFF),
            UInt32(out[10] & 0xFFFFFFFF), UInt32(out[11] & 0xFFFFFFFF)))
        let fence = UInt32(out[12] & 0xFFFFFFFF)
        append(String(format:
            "fence_buf[0]: %#010x %@",
            fence,
            fence == 0 ? "(PSP never wrote — submit failed silently)" :
                         "(PSP wrote this fence value)"))
        append(String(format: "ring_mem [+0..+11]: %#010x %#010x %#010x",
            UInt32(out[13] & 0xFFFFFFFF), UInt32(out[14] & 0xFFFFFFFF),
            UInt32(out[15] & 0xFFFFFFFF)))
    }

    // v0.1.24 — runtime engine health snapshot. Proves the dext +
    // GPU are still responsive after bringup (or after a long idle).
    // Decodes GRBM_STATUS busy bits + SDMA STATUS_REG idle bits + the
    // SMU running-features bitmap so the user can see at a glance
    // which engines are alive.
    func testLiveStatus() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelLiveStatus, outCount: 12)
        if kr != KERN_SUCCESS {
            append(String(format: "LiveStatus: kr=%#x (dext may have died)", kr))
            return
        }
        guard out.count >= 8 else {
            append("LiveStatus: short reply (\(out.count) words)")
            return
        }
        let grbm     = UInt32(out[0] & 0xFFFFFFFF)
        let cp_stat  = UInt32(out[1] & 0xFFFFFFFF)
        let bootload = UInt32(out[2] & 0xFFFFFFFF)
        let sdma0    = UInt32(out[3] & 0xFFFFFFFF)
        let sdma1    = UInt32(out[4] & 0xFFFFFFFF)
        let feat_lo  = UInt32(out[5] & 0xFFFFFFFF)
        let feat_hi  = UInt32(out[6] & 0xFFFFFFFF)
        let sdmaReached = (out[7] & 1) != 0
        let sdma0_rptr   = out.count > 8  ? UInt32(out[8] & 0xFFFFFFFF) : 0
        let sdma0_wptr   = out.count > 9  ? UInt32(out[9] & 0xFFFFFFFF) : 0
        let sdma0_cntl   = out.count > 10 ? UInt32(out[10] & 0xFFFFFFFF) : 0
        let sdma0_mcu    = out.count > 11 ? UInt32(out[11] & 0xFFFFFFFF) : 0

        append("── Live Status ──")
        append(String(format:
            "GRBM_STATUS=%#010x CP_STAT=%#010x RLC_BOOTLOAD=%#010x",
            grbm, cp_stat, bootload))
        let bootloadOk = (bootload & 0x80000000) != 0
        append("  GC: bringup_complete=\(sdmaReached ? "yes" : "no")  " +
               "BOOTLOAD bit31=\(bootloadOk ? "set" : "clear")")
        // SDMA STATUS_REG decode — bit positions per gc_12_0_0_sh_mask.h:142+.
        func sdmaDecode(_ s: UInt32) -> String {
            let idle      = (s >> 0)  & 1
            let rb_empty  = (s >> 2)  & 1
            let rb_full   = (s >> 3)  & 1
            let ib_idle   = (s >> 6)  & 1
            let srbm_idle = (s >> 14) & 1
            return "idle=\(idle) rb_empty=\(rb_empty) rb_full=\(rb_full) " +
                   "ib_idle=\(ib_idle) srbm_idle=\(srbm_idle)"
        }
        append(String(format: "SDMA0 STATUS_REG=%#010x (%@)",
                      sdma0, sdmaDecode(sdma0)))
        append(String(format: "SDMA0 RPTR=%#x WPTR=%#x  RB_CNTL=%#010x  MCU_CNTL=%#010x",
                      sdma0_rptr, sdma0_wptr, sdma0_cntl, sdma0_mcu))
        let mcuHalt   = (sdma0_mcu >> 0) & 1
        let rbEnable  = (sdma0_cntl >> 0) & 1
        append("  → MCU_HALT=\(mcuHalt) RB_ENABLE=\(rbEnable) " +
               "(if WPTR stays 0 after SDMA Copy, doorbell isn't reaching engine)")
        append(String(format: "SDMA1 STATUS_REG=%#010x (%@)",
                      sdma1, sdmaDecode(sdma1)))
        append(String(format:
            "SMU running features: hi=%#010x lo=%#010x", feat_hi, feat_lo))
        let anyAlive = (grbm | cp_stat | sdma0 | sdma1 | feat_lo | feat_hi) != 0
        append("→ dext + GPU \(anyAlive ? "ALIVE ✓" : "may be unresponsive ✗")")
    }

    // v0.1.24 — DisableAllSmuFeatures (PPSMC 0x7). Parks DPM so PMFW
    // stops defaulting fan to MAX. The fan should drop within a few
    // seconds. Reverse by re-running Initialize GPU (which re-sends
    // EnableAllSmuFeatures via smu_smc_hw_setup).
    func testDisableSmuFeatures() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelDisableSmuFeatures, outCount: 1)
        if kr != KERN_SUCCESS {
            append(String(format: "DisableSmuFeatures: kr=%#x", kr))
            return
        }
        let resp = out.first.map { UInt32($0 & 0xFFFFFFFF) } ?? 0
        if resp == 0 {
            append("DisableSmuFeatures: ok — DPM parked, fan should drop. " +
                   "Re-run Initialize GPU to re-enable DPM.")
        } else {
            append(String(format:
                "DisableSmuFeatures: PMFW resp=%#x (non-zero — message may not be supported on this PMFW build)",
                resp))
        }
    }

    // v0.1.29 — pick a coarse GFXCLK soft-clamp via PMFW.
    //   state: 0=auto, 1=low, 2=nominal, 3=high, 4=peak
    // The dext maps these to SetSoftMin/MaxByFreq(GFXCLK,…). Logs the
    // dext-side kIOReturn so we can tell "PMFW happy" from
    // "UnknownCmd" right in the UI.
    func testSetPowerState(_ state: UInt64) {
        guard openUserClient() else { return }
        let names = ["Auto", "Low", "Nominal", "High", "Peak"]
        let label = state < UInt64(names.count) ? names[Int(state)] :
                                                  "state=\(state)"
        let (kr, out) = callScalar(kSelSetPowerState,
                                   input: [state],
                                   outCount: 1)
        if kr != KERN_SUCCESS {
            append(String(format: "SetPowerState(\(label)): kr=%#x", kr))
            return
        }
        let resp = out.first.map { UInt32($0 & 0xFFFFFFFF) } ?? 0
        if resp == 0 {
            append("SetPowerState(\(label)): ok")
        } else {
            append(String(format:
                "SetPowerState(\(label)): dext kr=%#x (PMFW likely rejected one of the soft-clamp msgs — may not be exposed on this PMFW build)",
                resp))
        }
    }

    // v0.1.27 — exercise the per-BO ABI end-to-end:
    //   1. BOAlloc VRAM(64 KB, align 4 KB)  → records handle, gpu_va
    //   2. BOGetInfo → verifies the metadata round-trips
    //   3. BOAlloc GTT(4 KB, align 4 KB)    → records second handle
    //   4. BOMap (GTT) → IOConnectMapMemory64 → write 0xAB pattern
    //   5. BOFree on both → table slots reclaimed
    //
    // Notes:
    //   - Each call goes through the v0.1.27 4-input shape so the dext
    //     takes the new free-list path (not the legacy bump-into-DMA-
    //     buffer carve-out).
    //   - The smoke test deliberately doesn't drive Initialize GPU —
    //     VRAM/GTT alloc need gmc.vram_alloc + gart up. The dext returns
    //     kIOReturnNotReady (0xE00002D8) for either domain if you click
    //     this without running Initialize GPU first; the log line below
    //     surfaces that explicitly.
    func testBOSmoke() {
        guard openUserClient() else { return }
        append("bo smoke: starting (need Initialize GPU run first for VRAM/GTT)")

        // Step 1 — BOAlloc VRAM(64 KB, align 4 KB).
        let vramSize: UInt64 = 64 * 1024
        let vramAlign: UInt64 = 4 * 1024
        let (k1, o1) = callScalar(kSelBOAlloc,
                                  input: [vramSize, kBODomainVRAM,
                                          vramAlign, /* flags */ 0],
                                  outCount: 3)
        guard k1 == KERN_SUCCESS, o1.count >= 3 else {
            append(String(format: "bo smoke: VRAM BOAlloc failed kr=%#x", k1))
            return
        }
        let vramHandle = o1[0]
        let vramGpuVa  = o1[1]
        append(String(format:
            "bo smoke: VRAM bo handle=%#llx gpu_va=%#llx",
            vramHandle, vramGpuVa))

        // Step 2 — BOGetInfo on the VRAM BO.
        let (k2, o2) = callScalar(kSelBOGetInfo,
                                  input: [vramHandle], outCount: 5)
        if k2 == KERN_SUCCESS && o2.count >= 5 {
            let gpu = o2[0]
            let size = o2[2]
            let align = o2[3]
            let dom = o2[4] & 0xFF
            let mapped = (o2[4] >> 8) & 1
            let ok = (gpu == vramGpuVa) && (size >= vramSize)
            append(String(format:
                "bo smoke: VRAM info gpu=%#llx size=%llu align=%llu " +
                "domain=%llu mapped=%llu %@",
                gpu, size, align, dom, mapped,
                ok ? "[round-trip OK]" : "[mismatch]"))
        } else {
            append(String(format: "bo smoke: VRAM BOGetInfo kr=%#x", k2))
        }

        // Step 3 — BOAlloc GTT(4 KB, align 4 KB).
        let gttSize: UInt64 = 4 * 1024
        let gttAlign: UInt64 = 4 * 1024
        let (k3, o3) = callScalar(kSelBOAlloc,
                                  input: [gttSize, kBODomainGTT,
                                          gttAlign, /* flags */ 0],
                                  outCount: 3)
        var gttHandle: UInt64 = 0
        var gttCanMap = false
        if k3 == KERN_SUCCESS && o3.count >= 3 {
            gttHandle = o3[0]
            let gpuVa = o3[1]
            append(String(format:
                "bo smoke: GTT bo handle=%#llx gpu_va=%#llx",
                gttHandle, gpuVa))
            gttCanMap = true
        } else {
            // GTT requires GART up; on AS+TB5 the GART path is
            // structurally fragile so this can return Unsupported /
            // NotReady before bringup is fully driven.
            append(String(format:
                "bo smoke: GTT BOAlloc kr=%#x (need GART; skipping map step)",
                k3))
        }

        // Step 4 — BOMap the GTT BO; then map it into this process via
        // IOConnectMapMemory64 and write a pattern.
        if gttCanMap {
            let (km, om) = callScalar(kSelBOMap,
                                      input: [gttHandle], outCount: 2)
            if km == KERN_SUCCESS && om.count >= 2 {
                let memType = UInt32(om[0] & 0xFFFFFFFF)
                let size    = om[1]
                var atAddr: mach_vm_address_t = 0
                var atSize: mach_vm_size_t    = 0
                let kr4 = IOConnectMapMemory64(ucConn, memType,
                                               mach_task_self_,
                                               &atAddr, &atSize, 0)
                if kr4 == KERN_SUCCESS && atAddr != 0 {
                    let ptr = UnsafeMutableRawPointer(bitPattern: UInt(atAddr))
                    if let ptr = ptr {
                        memset(ptr, 0xAB, Int(min(size, UInt64(atSize))))
                        // Read back one byte to confirm the write landed.
                        let first = ptr.assumingMemoryBound(to: UInt8.self)[0]
                        append(String(format:
                            "bo smoke: GTT map ok cpu=%#llx size=%llu " +
                            "first=%#x [%@]",
                            UInt64(atAddr), UInt64(atSize), first,
                            first == 0xAB ? "pattern OK" : "MISMATCH"))
                    }
                    _ = IOConnectUnmapMemory64(ucConn, memType,
                                               mach_task_self_, atAddr)
                } else {
                    append(String(format:
                        "bo smoke: IOConnectMapMemory64 kr=%#x type=%#x",
                        kr4, memType))
                }
            } else {
                append(String(format: "bo smoke: BOMap kr=%#x", km))
            }
        }

        // Step 5 — BOFree both BOs.
        let (kf1, _) = callScalar(kSelBOFree, input: [vramHandle], outCount: 0)
        append(String(format: "bo smoke: VRAM BOFree kr=%#x", kf1))
        if gttCanMap {
            let (kf2, _) = callScalar(kSelBOFree,
                                      input: [gttHandle], outCount: 0)
            append(String(format: "bo smoke: GTT  BOFree kr=%#x", kf2))
        }
        append("bo smoke: done")
    }

    // v0.1.25 — VRAM->VRAM SDMA copy smoke test. Allocates src+dst
    // from the dext-side VRAM bump allocator, stages a known pattern
    // via BAR0, asks SDMA0 to COPY_LINEAR, and reads dst back via
    // MM_INDEX/MM_DATA. End-to-end proof that the engine processes a
    // packet and writes its fence (sysmem-free; AS+TB5 safe).
    func testSDMACopyVRAM() {
        guard openUserClient() else { return }
        // bytes=4096, instance=0 (defaults)
        let (kr, out) = callScalar(kSelSDMACopyVRAM,
                                   input: [4096, 0],
                                   outCount: 12)
        if kr != KERN_SUCCESS {
            append(String(format:
                "SDMA Copy: kr=%#x (dext rejected the call)", kr))
            return
        }
        guard out.count >= 7 else {
            append("SDMA Copy: short reply (\(out.count) words)")
            return
        }
        let status     = Int32(bitPattern: UInt32(out[0] & 0xFFFFFFFF))
        let elapsedUs  = out[1]
        let mismatched = UInt32(out[2] & 0xFFFFFFFF)
        let firstBad   = UInt32(out[3] & 0xFFFFFFFF)
        let bytes      = out[4]
        let extended = out.count >= 12 && out[9] == 0x53444d41
        let srcVA = extended ? out[10] : out[5]
        let dstVA = extended ? out[11] : out[6]

        // Decode the SDMA status. kIOReturnSuccess == 0; the canonical
        // mac error codes (kIOReturnTimeout etc.) are full mach codes,
        // so just classify into the three the plan calls out.
        let statusLabel: String
        switch status {
        case 0:
            statusLabel = mismatched == 0 ? "OK"
                                          : "engine ran but readback differs"
        case Int32(bitPattern: 0xE00002D6):  // kIOReturnTimeout
            statusLabel = "TIMEOUT (engine never wrote the fence)"
        case Int32(bitPattern: 0xE00002BC):  // kIOReturnBadArgument
            statusLabel = "BAD_ARG"
        case Int32(bitPattern: 0xE00002D9):  // kIOReturnNotReady
            statusLabel = "NOT_READY (SDMA not brought up?)"
        case Int32(bitPattern: 0xE00002BE):  // kIOReturnNoSpace
            statusLabel = "NO_SPACE (ring or VRAM)"
        default:
            statusLabel = String(format: "kr=%#x", UInt32(bitPattern: status))
        }

        append("── SDMA Copy ──")
        append(String(format:
            "SDMA Copy: %llu B status=%d (%@) mismatched=%u elapsed=%llu µs",
            bytes, status, statusLabel, mismatched, elapsedUs))
        append(String(format:
            "  src=%#llx dst=%#llx%@", srcVA, dstVA,
            extended ? "" : " (low 32 bits; older driver)"))
        if extended {
            append("  source upload: \(out[7]) mismatches before SDMA submit")
            if out[7] != 0 {
                append(String(format: "  source upload failed at byte %#llx; SDMA copy was not submitted", out[8]))
            }
        }
        if mismatched > 0 {
            append(String(format:
                "  first mismatched dword @ byte_offset=0x%x (%u/%llu mismatches)",
                firstBad, mismatched, bytes / 4))
        } else if status == 0 {
            append("  → SDMA engine alive, packet executed, fence written ✓")
        }
    }

    // v0.1.26 — first PM4 packet on the KIQ ring. Builds NOP +
    // RELEASE_MEM(0xDEADBEEF) targeting a VRAM-resident fence slot
    // (pre-filled with 0xCAFEBABE), kicks the GFX RB0 doorbell,
    // polls the fence slot for the expected value.
    //
    // Output scalars:
    //   [0] kIOReturn
    //   [1] elapsed_us
    //   [2] expected (0xDEADBEEF)
    //   [3] observed_fence
    //   [4] fence_gpu_va lo32
    //   [5] fence_gpu_va hi32
    func testCPKIQSmoke() {
        guard openUserClient() else { return }
        let (kr, out) = callScalar(kSelCPKIQSmoke, outCount: 6)
        if kr != KERN_SUCCESS || out.count < 6 {
            append(String(format:
                "CP GFX Fence: FAIL kr=%#x (no valid diagnostic reply)", kr))
            return
        }
        let status   = UInt32(out[0] & 0xFFFFFFFF)
        let elapsed  = out[1]
        let expected = UInt32(out[2] & 0xFFFFFFFF)
        let observed = UInt32(out[3] & 0xFFFFFFFF)
        let lo       = out[4] & 0xFFFFFFFF
        let hi       = out[5] & 0xFFFFFFFF
        let gpuVa    = (hi << 32) | lo
        let okLabel = (status == 0 && expected == 0xDEADBEEF && observed == expected)
            ? "OK" : "FAIL"
        append(String(format:
            "CP GFX Fence: status=%#x (%@) elapsed=%llu us",
            status, okLabel, elapsed))
        if gpuVa == 0 && status != 0 {
            append(String(format: "  CP register-test/preflight failed (observed=%#x); memory fence was not submitted", observed))
            return
        }
        append("  CP register-write test passed")
        append(String(format:
            "  fence expected=%#x observed=%#x @ gpu_va=%#llx",
            expected, observed, gpuVa))
        if kr != KERN_SUCCESS && status == 0 {
            append(String(format: "  (kr=%#x)", kr))
        }
    }

    func testCompute() {
        guard openUserClient() else { return }
        let seed = UInt32(truncatingIfNeeded: DispatchTime.now().uptimeNanoseconds)
        append("Compute Smoke: starting one wave32 workgroup (input + seed → output)")
        let (kr, out) = callScalar(kSelComputeTest, input: [UInt64(seed)], outCount: 6)
        guard kr == KERN_SUCCESS, out.count == 6 else {
            append(String(format: "Compute Smoke: RPC failed kr=%#x", kr))
            return
        }
        let stages = ["preflight", "allocate", "upload", "cache/fence", "registers/fence",
                      "shader/fence", "verify", "complete"]
        let stage = out[1] < UInt64(stages.count) ? stages[Int(out[1])] : "unknown"
        append(String(format: "Compute Smoke: status=%#llx stage=%@ mismatches=%llu fence=%llu",
                      out[0], stage, out[2], out[4]))
        append(String(format: "  data GPU VA=%#llx seed=%#x", out[5], seed))
        if out[2] != 0 {
            append(String(format: "  first mismatch at data byte offset=%#llx", out[3]))
        }
        if out[1] < 6 {
            append("  results not checked — dispatch completion has not been verified")
        }
        if out[0] == 0 && out[1] == 7 {
            append("Compute Smoke: all 32 results, input and guard words verified; storage released")
        } else if out[1] != 0 {
            append("Compute Smoke: session retained for recovery — Stop GPU before retry")
        }
    }

    func testHostMemoryTransfer() {
        guard openUserClient() else { return }
        let seed = UInt32(truncatingIfNeeded: DispatchTime.now().uptimeNanoseconds)
        append("Host Memory Copy: starting 16 KB host → VRAM → host verification")
        let (kr, out) = callScalar(kSelHostMemoryTest, input: [UInt64(seed)], outCount: 6)
        guard kr == KERN_SUCCESS, out.count == 6 else {
            append(String(format: "Host Memory Copy: RPC failed kr=%#x", kr))
            return
        }
        let stages = ["preflight", "allocate/map", "upload", "GPU read",
                      "verify VRAM", "GPU write", "verify host", "unbind", "complete"]
        let stage = out[1] < UInt64(stages.count) ? stages[Int(out[1])] : "unknown"
        append(String(format: "Host Memory Copy: status=%#llx stage=%@ mismatches=%llu",
                      out[0], stage, out[2]))
        append(String(format: "  host GPU VA=%#llx VRAM GPU VA=%#llx seed=%#x", out[4], out[5], seed))
        if out[2] != 0 {
            append(String(format: "  first mismatch at byte offset=%#llx", out[3]))
        }
        if out[0] == 0 && out[1] == 8 {
            append("Host Memory Copy: GPU read and write verified; mappings released")
        } else if out[1] != 0 {
            append("Host Memory Copy: session retained for recovery — Stop GPU before retry")
        }
    }

    // Exercise the command-stream handle API on SDMA0 or the kernel GFX queue.
    // GFX NOP uses a PACKET3 header plus one payload dword; SDMA NOP is zero.
    func testCSSmoke(gfx: Bool = false) {
        guard openUserClient() else { return }

        let engine = gfx ? "GFX0" : "SDMA0"
        let ipType = gfx ? kCSIPTypeGFX : kCSIPTypeSDMA
        // Step 1: create CS on the selected engine.
        let (krC, outC) = callScalar(kSelCSCreate,
                                     input: [ipType, 0],
                                     outCount: 1)
        guard krC == KERN_SUCCESS, let handle = outC.first, handle != 0 else {
            append(String(format: "\(engine) CS smoke: CSCreate failed kr=%#x", krC))
            return
        }
        append(String(format: "\(engine) CS smoke: created handle=%#llx", handle))

        // CSWriteDwords expects [handle, dw0..dw7, count].
        let nop: UInt64 = gfx ? 0xc0001000 : 0
        let writeIn: [UInt64] = [handle, nop, 0, nop, 0, 0, 0, 0, 0, 4]
        let (krW, _) = callScalar(kSelCSWriteDwords,
                                  input: writeIn,
                                  outCount: 1)
        if krW != KERN_SUCCESS {
            append(String(format: "\(engine) CS smoke: CSWriteDwords failed kr=%#x", krW))
            _ = callScalar(kSelCSDestroy, input: [handle], outCount: 1)
            return
        }
        append("\(engine) CS smoke: appended 4 NOP dwords")

        // Step 3: submit.
        let (krS, outS) = callScalar(kSelSubmitIB,
                                     input: [handle],
                                     outCount: 1)
        guard krS == KERN_SUCCESS, let fence = outS.first else {
            append(String(format: "\(engine) CS smoke: SubmitIB failed kr=%#x", krS))
            _ = callScalar(kSelCSDestroy, input: [handle], outCount: 1)
            return
        }
        append(String(format: "\(engine) CS smoke: submitted, fence_handle=%#llx", fence))

        // Step 4: wait 1 s (timeout in ns).
        let (krWf, outWf) = callScalar(kSelWaitFence,
                                       input: [fence, 1_000_000_000],
                                       outCount: 1)
        let status = outWf.first ?? 0xFF
        switch krWf {
        case KERN_SUCCESS where status == 0:
            append("\(engine) CS smoke: WaitFence ok — fence signaled")
        case KERN_SUCCESS:
            append(String(format: "\(engine) CS smoke: WaitFence ok but status=%llu", status))
        default:
            append(String(format:
                "\(engine) CS smoke: WaitFence kr=%#x status=%llu", krWf, status))
        }

        // Step 5: destroy.
        let (krD, _) = callScalar(kSelCSDestroy,
                                  input: [handle],
                                  outCount: 1)
        if krD != KERN_SUCCESS {
            append(String(format: "\(engine) CS smoke: CSDestroy kr=%#x", krD))
        } else {
            append("\(engine) CS smoke: destroyed handle")
        }
    }

    // Returns true if the stage succeeded so initializeGPU can bail
    // out at the first failure instead of grinding through 8 stages
    // of timeouts.
    @discardableResult
    func testInitDeviceUpTo(_ stage: UInt64) -> Bool {
        guard openUserClient() else { return false }
        let (kr, out) = callScalar(kSelInitDevice,
                                   input: [stage],
                                   outCount: 1)
        let name = DriverController.stageNames[stage] ?? "stage\(stage)"
        if kr == KERN_SUCCESS, let reached = out.first {
            let reachedName = DriverController.stageNames[reached] ?? "stage\(reached)"
            append("\(name): ok (reached \(reachedName))")
            return true
        } else {
            append(String(format: "\(name): FAILED kr=%#x", kr))
            return false
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

    // initializeGPU: one-button bring-up that logs EVERY step it
    // attempts with a "→ start" line and a result line, stopping at
    // the first failure so later stages cannot hide an incomplete setup.
    func initializeGPU(stopAfterFirmware: Bool = false) {
        guard !isWorking else {
            append("initializeGPU: already running")
            return
        }
        isWorking = true
        status = "initializing GPU…"
        statusColor = .orange
        Task.detached(priority: .userInitiated) { [weak self] in
            let succeeded = await self?.initializeGPUBlocking(stopAfterFirmware: stopAfterFirmware) ?? false
            await MainActor.run {
                self?.isWorking = false
                self?.status = succeeded ? (stopAfterFirmware ? "firmware checkpoint complete" : "initialization stages complete") : "initialization failed — see log"
                self?.statusColor = succeeded ? .green : .red
            }
        }
    }

    private func initializeGPUBlocking(stopAfterFirmware: Bool) async -> Bool {
        append("──── initializeGPU starting ────")

        // Helper to log "step → start" then "step → result".
        func step(_ name: String, _ body: () -> Bool) -> Bool {
            append("\(name) → start")
            let ok = body()
            append(ok ? "\(name) → ok" : "\(name) → FAILED")
            return ok
        }

        // 1. Open user client.
        let uc = step("open user client") { self.openUserClient() }
        if !uc {
            append("initializeGPU: aborted — no user client; stop here")
            return false
        }
        let (stageKr, stageOut) = callScalar(kSelQueryInfo,
                                             input: [kInfoBringupReached],
                                             outCount: 1)
        guard stageKr == KERN_SUCCESS, let stage = stageOut.first else {
            append(String(format: "initializeGPU: cannot verify current stage (kr=%#x); reset was not attempted", stageKr))
            return false
        }
        if stage == 15 {
            let (identityKr, _) = callScalar(kSelGetIdentity, outCount: 7)
            guard identityKr == KERN_SUCCESS else {
                append("initializeGPU: previous stages completed, but PCI access is no longer active; reconnect the GPU for a fresh session")
                return false
            }
            append("initializeGPU: all bringup stages are already complete; keeping the current GPU session")
            return true
        }
        if stage == 8 {
            let (identityKr, _) = callScalar(kSelGetIdentity, outCount: 7)
            guard identityKr == KERN_SUCCESS else {
                append("initializeGPU: firmware checkpoint lost PCI access; reconnect for a fresh session")
                return false
            }
            if stopAfterFirmware {
                append("firmware checkpoint already complete; no reset or upload performed")
                return true
            }
            append("continuing from verified firmware checkpoint without resetting the GPU")
            return initializeRemainingIPs()
        }
        if stage != 0 {
            append("initializeGPU: stopped at stage \(stage); use Restart GPU to reset and rebuild this session")
            append("Restart GPU requires a supported function reset and no other connected clients or mapped/interrupt sessions")
            return false
        }
        // 1b. Read identity (PCI device id + revision) so we can pick
        // the kicker firmware variant where applicable.
        await MainActor.run { self.testGetIdentity() }
        // 2. DMA buffer.
        let dma = step("alloc DMA buffer") { self.ensureDMABuffer() }
        if !dma {
            append("initializeGPU: aborted — no DMA buffer; stop here")
            return false
        }
        // 3. Function-Level Reset — kicks off IFWI on cold-hotplugged
        // cards. qemu-vfio-apple does this before MMIO; we were
        // skipping it which is why C2PMSG_33 stays 0.
        let reset = step("PCIe Function-Level Reset (kicks IFWI)") {
            let (kr, _) = self.callScalar(kSelResetDevice, outCount: 0)
            return kr == KERN_SUCCESS
        }
        guard reset else {
            append("initializeGPU: reset failed; firmware loading was not started")
            return false
        }
        // Give IFWI ~150 ms to start the watchdog after reset before
        // we begin polling its status. Linux waits the same way.
        try? await Task.sleep(nanoseconds: 150_000_000)

        // 4. IP discovery (polls IFWI internally, up to 2 sec).
        let disc = step("IP discovery (IFWI wait + RCC_CONFIG_MEMSIZE)") {
            self.discoverIPs()
        }
        if !disc {
            append("initializeGPU: discovery failed — without IP versions we")
            append("  can't construct firmware filenames. Click Diagnostics")
            append("  to inspect IFWI / BAR state.")
            return false
        }

        guard let psp  = self.ipPSP?.path,
              let smu  = self.ipSMU?.path,
              let sdma = self.ipSDMA?.path,
              let gfx  = self.ipGFX?.path
        else {
            append("initializeGPU: discovery didn't fill IP versions")
            return false
        }

        // Stages 1–3: IPDiscovery → IHInit → GMCInit (no firmware needed).
        // New order per Agent D's reorder: GMC must be fully up BEFORE
        // PSP runs, so SOS comes up with the right TLB/L2 state.
        for s: UInt64 in [1, 2, 3] {
            if !testInitDeviceUpTo(s) {
                append("initializeGPU: stopping early — stage \(s) failed; downstream stages depend on it")
                return false
            }
        }

        // Stage 4: PSPInit (allocates fw_pri).
        if !testInitDeviceUpTo(4) {
            append("initializeGPU: stopping early — PSPInit failed")
            return false
        }

        // PSP SOS firmware load. Mirror upstream amdgpu_psp.c
        // psp_init_sos_microcode line 3700:
        //     if (amdgpu_is_kicker_fw(adev))
        //         "amdgpu/%s_sos_kicker.bin"
        //     else
        //         "amdgpu/%s_sos.bin"
        let isKicker = DriverController.amdgpuIsKickerFw(
            deviceId: pciDeviceId, revision: pciRevision)
        let sosName = isKicker ? "psp_\(psp)_sos_kicker.bin"
                               : "psp_\(psp)_sos.bin"
        append("PSP SOS firmware selection: device=0x\(String(format: "%04x", pciDeviceId)) rev=0x\(String(format: "%02x", pciRevision)) → \(sosName)")
        if loadFirmware(kFwSOS, sosName) {
            append("\(sosName) → loaded")
        } else {
            append("\(sosName) → FAILED to load")
            return false
        }

        // Stages 5–7: PSPLoadSOS → PSPRingCreate → TMRSetup. These don't
        // depend on per-IP microcode.
        for s: UInt64 in [5, 6, 7] {
            if !testInitDeviceUpTo(s) {
                append("initializeGPU: stopping early — stage \(s) failed; downstream stages depend on it")
                return false
            }
        }

        // TA package — psp_<chip>_ta.bin. The dext parses it and stages
        // the ASD sub-binary into a fwBuf VRAM slot. psp_asd_initialize
        // (post-AUTOLOAD_RLC) references that slot via GFX_CMD_ID_LOAD_ASD.
        // Mirrors upstream psp_v14_0_init_microcode (psp_v14_0.c:75/83)
        // which calls psp_init_ta_microcode for IP_VERSION(14,0,2/3/5).
        // Kicker chips use the _kicker suffix per amdgpu_is_kicker_fw.
        let taName = isKicker ? "psp_\(psp)_ta_kicker.bin"
                              : "psp_\(psp)_ta.bin"
        if loadFirmware(kFwTA, taName) {
            append("\(taName) → loaded")
        } else {
            append("\(taName) → FAILED to load (PSP autoload may stall)")
            return false
        }

        // (No TOC load on psp_v14_0_3 — upstream's psp_v14_0_init_microcode
        // only calls psp_init_toc_microcode for IP_VERSION(14,0,5); for
        // (14,0,2) and (14,0,3) it calls psp_init_sos + psp_init_ta only.
        // psp->toc.start_addr stays NULL, so psp_tmr_init's
        // `if (psp->toc.start_addr) psp_load_toc(...)` is skipped.)

        // Per-IP firmware load order MUST match upstream's
        // psp_load_non_psp_fw (amdgpu_psp.c:3051): SMU first (special-cased
        // when autoload_supported), then iterate firmware.ucode[] in
        // AMDGPU_UCODE_ID enum order (amdgpu_ucode.h:479-552). For
        // psp_v14_0_3 + gfx_v12 the loaded IDs in order are:
        //   SDMA_UCODE_TH0 → CP_RS64_PFP/ME/MEC(+stacks) → CP_MES/_DATA
        //   → IMU_I/_D → RLC_* sub-bins → RLC_G (LAST → AUTOLOAD_RLC).
        //
        // Loading out of order causes PSP to reject with status 0x5 on
        // the first frame, then 0xFFFF0006 on every subsequent frame
        // because PSP gets stuck in an error state.
        //
        // SMU/IMU/RLC have `_kicker.bin` variants that upstream selects
        // when `amdgpu_is_kicker_fw(adev)` is true (smu_v14_0.c:83,
        // imu_v12_0.c:51, gfx_v12_0.c:617). Mirror the full upstream
        // selection — not just SOS.
        let smuName = isKicker ? "smu_\(smu)_kicker.bin"
                               : "smu_\(smu).bin"
        let imuName = isKicker ? "gc_\(gfx)_imu_kicker.bin"
                               : "gc_\(gfx)_imu.bin"
        let rlcName = isKicker ? "gc_\(gfx)_rlc_kicker.bin"
                               : "gc_\(gfx)_rlc.bin"
        if loadFirmware(kFwIP_SMU, smuName) {
            append("\(smuName) → loaded")
        } else {
            append("initializeGPU: SMU PMFW load failed")
            return false
        }
        guard loadFirmware(kFwFile_SDMA,    "sdma_\(sdma).bin"),
              loadFirmware(kFwFile_CP_PFP,  "gc_\(gfx)_pfp.bin"),
              loadFirmware(kFwFile_CP_ME,   "gc_\(gfx)_me.bin"),
              loadFirmware(kFwFile_CP_MEC,  "gc_\(gfx)_mec.bin"),
              loadFirmware(kFwFile_MES_UNI, "gc_\(gfx)_uni_mes.bin"),
              loadFirmware(kFwFile_IMU,     imuName),
              loadFirmware(kFwFile_RLC,     rlcName) else {
            append("initializeGPU: firmware load failed; later loads and stages were not attempted")
            return false
        }

        // Stage 8: PSPFwLoad — synchronization point. Validates that
        // all firmware was loaded by the LoadFirmware calls above.
        if !testInitDeviceUpTo(8) {
            append("initializeGPU: stopping early — PSPFwLoad validation failed")
            return false
        }

        if stopAfterFirmware {
            append("──── firmware checkpoint complete; SMU/CP/MES setup not started ────")
            return true
        }
        return initializeRemainingIPs()
    }

    private func initializeRemainingIPs() -> Bool {
        // Stages 9–15: SMUInit → IMUInit → RLCInit → CPInit → MESInit →
        // GFXInit → SDMAInit. Each gates on its firmware being loaded
        // above (the microcode_loaded flags are set by LoadFirmware).
        for s: UInt64 in [9, 10, 11, 12, 13, 14, 15] {
            if !testInitDeviceUpTo(s) {
                append("initializeGPU: stopping early — stage \(s) failed; downstream stages depend on it")
                return false
            }
        }

        append("──── initializeGPU done ────")
        return true
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

        // Stages 1–3: IPDiscovery → IHInit → GMCInit (no firmware needed).
        for s: UInt64 in [1, 2, 3] { testInitDeviceUpTo(s) }

        // Stage 4: PSPInit (allocates fw_pri).
        testInitDeviceUpTo(4)

        // Stages 5–7: PSPLoadSOS → PSPRingCreate → TMRSetup.
        let isKickerFB = DriverController.amdgpuIsKickerFw(
            deviceId: pciDeviceId, revision: pciRevision)
        let sosNameFB = isKickerFB ? "psp_\(psp)_sos_kicker.bin"
                                   : "psp_\(psp)_sos.bin"
        if !loadFirmware(kFwSOS, sosNameFB) {
            append("runFullBringup: stop — PSP SOS load failed")
            return
        }
        for s: UInt64 in [5, 6, 7] { testInitDeviceUpTo(s) }

        // (See note in initializeGPUBlocking — no TOC load on psp_v14_0_3.)

        // Per-IP firmware load order: see note in initializeGPUBlocking —
        // must match upstream psp_load_non_psp_fw enum iteration:
        //   SMU → SDMA → CP_RS64 → MES → IMU → RLC (RLC_G last).
        // Kicker variants used when amdgpu_is_kicker_fw(adev) is true.
        let smuNameFB = isKickerFB ? "smu_\(smu)_kicker.bin"
                                   : "smu_\(smu).bin"
        let imuNameFB = isKickerFB ? "gc_\(gfx)_imu_kicker.bin"
                                   : "gc_\(gfx)_imu.bin"
        let rlcNameFB = isKickerFB ? "gc_\(gfx)_rlc_kicker.bin"
                                   : "gc_\(gfx)_rlc.bin"
        if !loadFirmware(kFwIP_SMU, smuNameFB) {
            append("runFullBringup: SMU PMFW load failed")
        }
        _ = loadFirmware(kFwFile_SDMA,    "sdma_\(sdma).bin")
        _ = loadFirmware(kFwFile_CP_PFP,  "gc_\(gfx)_pfp.bin")
        _ = loadFirmware(kFwFile_CP_ME,   "gc_\(gfx)_me.bin")
        _ = loadFirmware(kFwFile_CP_MEC,  "gc_\(gfx)_mec.bin")
        _ = loadFirmware(kFwFile_MES_UNI, "gc_\(gfx)_uni_mes.bin")
        _ = loadFirmware(kFwFile_IMU,     imuNameFB)
        _ = loadFirmware(kFwFile_RLC,     rlcNameFB)

        // Stage 8: PSPFwLoad — synchronization point. Validates that
        // all firmware was loaded by the host-side LoadFirmware calls
        // above. The microcode_loaded flags (sdma, imu, etc.) are set
        // by the LoadFirmware selector as each payload submits.
        if !testInitDeviceUpTo(8) {
            append("runFullBringup: stop — PSPFwLoad validation failed")
            return
        }

        // Stages 9–15: SMUInit → IMUInit → RLCInit → CPInit → MESInit →
        // GFXInit → SDMAInit. Each gates on its firmware being loaded
        // above (the microcode_loaded flags are set by LoadFirmware).
        for s: UInt64 in [9, 10, 11, 12, 13, 14, 15] { testInitDeviceUpTo(s) }

        append("runFullBringup: done — see log for stage outcomes")
    }

    // MARK: Version reporting

    private var expectedDriverBuild: UInt64? {
        let plistURL = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions")
            .appendingPathComponent(dextBundleIdentifier + ".dext/Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any],
              let text = plist["CFBundleVersion"] as? String else { return nil }
        return UInt64(text)
    }

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
