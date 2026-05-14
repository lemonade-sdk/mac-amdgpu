//
//  macamdgpu_ping.swift — Phase 1A/B userspace smoke test.
//
//  Build:
//      swiftc -framework IOKit -framework CoreFoundation \
//          scripts/macamdgpu_ping.swift -o build/macamdgpu_ping
//
//  Run:
//      ./build/macamdgpu_ping
//          # Phase 1A surface: ping, identity, BARs, DMA, IRQ
//      ./build/macamdgpu_ping --init
//          # Also calls InitDevice up to PSPInit (no firmware yet).
//      ./build/macamdgpu_ping --init --load-sos PATH
//          # Loads a SOS firmware blob from PATH, runs InitDevice to
//          # PSPLoadSOS. Needs IP base for MP0 to be set first.
//      ./build/macamdgpu_ping --reset
//          # Issues FLR at the end (will quiesce + re-init device).
//      ./build/macamdgpu_ping --query-info
//          # Calls QueryInfo for every known type and prints the result.
//      ./build/macamdgpu_ping --bo-test
//          # Exercises BOAlloc + BOGetInfo + BOFree against the per-
//          # client DMABuffer.
//      ./build/macamdgpu_ping --sdma-test
//          # Asks the dext to issue an SDMA COPY_LINEAR over two
//          # halves of the DMABuffer (requires SDMA microcode loaded
//          # + InitDevice(SDMAInit) reached on real hardware).
//      ./build/macamdgpu_ping --submit-test
//          # Existing direct-CP test: SubmitTestPM4 emits NOP +
//          # RELEASE_MEM, polls EOP fence. Needs CP HQD programmed.
//

import Foundation
import IOKit

// MUST match enums in dext/MacAMDGPU.cpp.
private let kPing:              UInt32 = 0
private let kGetIdentity:       UInt32 = 1
private let kGetBARInfo:        UInt32 = 2
private let kSetupInterrupts:   UInt32 = 3
private let kWaitInterrupt:     UInt32 = 4
private let kSetIRQMask:        UInt32 = 5
private let kAllocateDMABuffer: UInt32 = 6
private let kFreeDMABuffer:     UInt32 = 7
private let kResetDevice:       UInt32 = 8
private let kInitDevice:        UInt32 = 9
private let kLoadFirmware:      UInt32 = 10
private let kSetIPBase:         UInt32 = 11
private let kGetIPBase:         UInt32 = 12
private let kLoadDiscoveryBin:  UInt32 = 13
private let kSubmitTestPM4:     UInt32 = 14
private let kSDMACopyTest:      UInt32 = 15
private let kBOAlloc:           UInt32 = 16
private let kBOFree:            UInt32 = 17
private let kBOGetInfo:         UInt32 = 18
private let kSubmitIB:          UInt32 = 19
private let kWaitFence:         UInt32 = 20
private let kQueryInfo:         UInt32 = 21

// QueryInfo type tags (match MacAMDGPU.cpp).
private let kInfoGFXVersion:     UInt64 = 1
private let kInfoVRAMSizes:      UInt64 = 2
private let kInfoIPVersions:     UInt64 = 3
private let kInfoBringupReached: UInt64 = 4

private let kMemBAR0:           UInt32 = 0
private let kMemBAR2:           UInt32 = 2
private let kMemDMABuffer:      UInt32 = 6
private let kMemIRQState:       UInt32 = 7

// Firmware type for SOS (matches kMacAMDGPUFwTypeSOS).
private let kFwSOS:             UInt64 = 0

// Bringup stages (match amdgpu_init.h).
private let kStageNone:           UInt64 = 0
private let kStageIPDiscovery:    UInt64 = 1
private let kStagePSPInit:        UInt64 = 2
private let kStagePSPLoadSOS:     UInt64 = 3
private let kStagePSPRingCreate:  UInt64 = 4
private let kStageTMRSetup:       UInt64 = 5
private let kStageSMUInit:        UInt64 = 6

// Firmware types (match MacAMDGPU.cpp).
private let kFwKDB:               UInt64 = 1
private let kFwSPL:               UInt64 = 2
private let kFwSysDrv:            UInt64 = 3
private let kFwSocDrv:            UInt64 = 4
private let kFwIntfDrv:           UInt64 = 5
private let kFwDbgDrv:            UInt64 = 6
private let kFwRASDrv:            UInt64 = 7
private let kFwIPKeyMgrDrv:       UInt64 = 8
// Post-SOS IP firmware (0x100 + psp_gfx_fw_type)
private let kFwIP_PMFW:           UInt64 = 0x100 + 18
private let kFwIP_RLC_G:          UInt64 = 0x100 + 8
private let kFwIP_CP_ME:          UInt64 = 0x100 + 1
private let kFwIP_CP_PFP:         UInt64 = 0x100 + 2
private let kFwIP_CP_MEC:         UInt64 = 0x100 + 4
private let kFwIP_SDMA0:          UInt64 = 0x100 + 9
private let kFwIP_IMU_I:          UInt64 = 0x100 + 68
private let kFwIP_IMU_D:          UInt64 = 0x100 + 69
private let kFwIP_RS64_MES:       UInt64 = 0x100 + 76

// IP block ids (match amdgpu_ip.h IPBlock enum).
private let kBlockGC:    UInt64 = 0
private let kBlockHDP:   UInt64 = 1
private let kBlockSDMA0: UInt64 = 2
private let kBlockSDMA1: UInt64 = 3
private let kBlockMP0:   UInt64 = 4
private let kBlockMP1:   UInt64 = 5
private let kBlockNBIO:  UInt64 = 6
private let kBlockOSSSYS: UInt64 = 7
private let kBlockGMC:   UInt64 = 8

func die(_ msg: String, _ code: Int32 = 1) -> Never {
    let bytes = Array("error: \(msg)\n".utf8)
    FileHandle.standardError.write(Data(bytes))
    exit(code)
}

func warn(_ msg: String) {
    let bytes = Array("warn: \(msg)\n".utf8)
    FileHandle.standardError.write(Data(bytes))
}

func openService() -> io_connect_t {
    guard let matching = IOServiceMatching("MacAMDGPU") else {
        die("IOServiceMatching returned nil")
    }
    var iter: io_iterator_t = 0
    let kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter)
    if kr != KERN_SUCCESS { die("IOServiceGetMatchingServices: \(kr)") }
    defer { IOObjectRelease(iter) }
    let svc = IOIteratorNext(iter)
    if svc == 0 {
        die("MacAMDGPU service not found — "
            + "`systemextensionsctl list` should show the dext activated.")
    }
    defer { IOObjectRelease(svc) }
    var conn: io_connect_t = 0
    let openKr = IOServiceOpen(svc, mach_task_self_, 0, &conn)
    if openKr != KERN_SUCCESS {
        die("IOServiceOpen: \(String(format: "%#x", openKr)) "
            + "(0xe00002c1 = entitlement missing)")
    }
    return conn
}

func callScalar(_ conn: io_connect_t, _ selector: UInt32,
                input: [UInt64] = [], outputCount: UInt32 = 0)
    -> (kern_return_t, [UInt64])
{
    var outCount = outputCount
    var out = [UInt64](repeating: 0, count: Int(max(outputCount, 1)))
    var inArr = input
    let kr = inArr.withUnsafeMutableBufferPointer { p -> kern_return_t in
        IOConnectCallScalarMethod(conn, selector,
                                  input.isEmpty ? nil : p.baseAddress,
                                  UInt32(input.count),
                                  &out, &outCount)
    }
    return (kr, Array(out.prefix(Int(outCount))))
}

func mustScalar(_ conn: io_connect_t, _ selector: UInt32,
                input: [UInt64] = [], outputCount: UInt32 = 0,
                name: String) -> [UInt64]
{
    let (kr, out) = callScalar(conn, selector,
                               input: input, outputCount: outputCount)
    if kr != KERN_SUCCESS {
        die("\(name) failed kr=\(String(format: "%#x", kr))")
    }
    return out
}

func barTypeName(_ t: UInt64) -> String {
    switch t {
    case 0x00: return "mem32"
    case 0x01: return "io"
    case 0x04: return "mem64"
    case 0x08: return "mem32-prefetch"
    case 0x0C: return "mem64-prefetch"
    default:   return String(format: "unknown(%#llx)", t)
    }
}

func mapMemory(_ conn: io_connect_t, _ type: UInt32, _ tag: String)
    -> (UnsafeMutableRawPointer, mach_vm_size_t)
{
    var addr: mach_vm_address_t = 0
    var size: mach_vm_size_t = 0
    let kr = IOConnectMapMemory64(conn, type,
                                  mach_task_self_, &addr, &size,
                                  IOOptionBits(kIOMapAnywhere | kIOMapDefaultCache))
    if kr != KERN_SUCCESS {
        die("IOConnectMapMemory64(\(tag)): \(String(format: "%#x", kr))")
    }
    return (UnsafeMutableRawPointer(bitPattern: UInt(addr))!, size)
}

// --- Phase 1A surface ---------------------------------------------------

func ping(_ conn: io_connect_t) {
    let out = mustScalar(conn, kPing, outputCount: 1, name: "Ping")
    print(String(format: "ping:    0x%llX", out[0]))
}

func getIdentity(_ conn: io_connect_t) {
    let out = mustScalar(conn, kGetIdentity, outputCount: 6,
                         name: "GetIdentity")
    print(String(format: "identity: %04llx:%04llx class=%06llx bus=%02llx "
                       + "dev=%02llx fn=%llu",
                 out[3], out[4], out[5], out[0], out[1], out[2]))
    if out[3] != 0x1002 || out[4] != 0x7551 {
        warn("expected VID:DID 1002:7551, got "
             + String(format: "%04llx:%04llx", out[3], out[4]))
    }
}

func enumerateBARs(_ conn: io_connect_t) {
    for bar: UInt64 in 0...5 {
        let (kr, out) = callScalar(conn, kGetBARInfo,
                                   input: [bar], outputCount: 3)
        if kr == KERN_SUCCESS {
            print(String(format: "BAR%llu:    memIdx=%llu size=%llu type=%@",
                         bar, out[0], out[1], barTypeName(out[2])))
        }
    }
}

func mapBAR0(_ conn: io_connect_t) {
    let (p, size) = mapMemory(conn, kMemBAR0, "BAR0")
    print(String(format: "BAR0 map: vaddr=%#llx size=%llu",
                 UInt64(UInt(bitPattern: p)), UInt64(size)))
    let u = p.assumingMemoryBound(to: UInt32.self)
    print(String(format: "BAR0[0x0]=%#010x BAR0[0x4]=%#010x",
                 u.pointee, u.advanced(by: 1).pointee))
    _ = IOConnectUnmapMemory64(conn, kMemBAR0, mach_task_self_,
                               mach_vm_address_t(UInt(bitPattern: p)))
}

func testDMA(_ conn: io_connect_t, size: UInt64 = 64 * 1024) -> Bool {
    let out = mustScalar(conn, kAllocateDMABuffer,
                         input: [size, 4096], outputCount: 2,
                         name: "AllocateDMABuffer")
    print(String(format: "dma alloc: segs=%llu first_bus=%#llx size=%llu",
                 out[0], out[1], size))
    let (p, sz) = mapMemory(conn, kMemDMABuffer, "DMABuffer")
    let u = p.assumingMemoryBound(to: UInt64.self)
    let pat: UInt64 = 0xDEADBEEFCAFEBABE
    u.pointee = pat
    u.advanced(by: 1).pointee = ~pat
    let ok = u.pointee == pat && u.advanced(by: 1).pointee == ~pat
    print(ok ? "dma cpu:   pattern verified" : "dma cpu:   MISMATCH")
    _ = IOConnectUnmapMemory64(conn, kMemDMABuffer, mach_task_self_,
                               mach_vm_address_t(UInt(bitPattern: p)))
    _ = sz
    let (kr, _) = callScalar(conn, kFreeDMABuffer)
    return ok && kr == KERN_SUCCESS
}

func testIRQ(_ conn: io_connect_t) {
    _ = mustScalar(conn, kSetupInterrupts, name: "SetupInterrupts")
    print("irq:       setup ok")
    let (p, _) = mapMemory(conn, kMemIRQState, "IRQState")
    let pending = p.assumingMemoryBound(to: UInt64.self)
    let enabled = pending.advanced(by: 4)
    print(String(format: "irq enabled[0]=%016llx", enabled.pointee))
    _ = IOConnectUnmapMemory64(conn, kMemIRQState, mach_task_self_,
                               mach_vm_address_t(UInt(bitPattern: p)))
}

// --- Phase 1B surface ---------------------------------------------------

func getIPBase(_ conn: io_connect_t, _ block: UInt64) -> (UInt64, Bool) {
    let (kr, out) = callScalar(conn, kGetIPBase, input: [block],
                               outputCount: 2)
    if kr != KERN_SUCCESS { return (0, false) }
    return (out[0], out[1] != 0)
}

func ipBlockName(_ block: UInt64) -> String {
    switch block {
    case kBlockGC:    return "GC"
    case kBlockHDP:   return "HDP"
    case kBlockSDMA0: return "SDMA0"
    case kBlockSDMA1: return "SDMA1"
    case kBlockMP0:   return "MP0"
    case kBlockMP1:   return "MP1"
    case kBlockNBIO:  return "NBIO"
    case kBlockOSSSYS: return "OSSSYS"
    case kBlockGMC:   return "GMC"
    default: return "?"
    }
}

func showIPBases(_ conn: io_connect_t) {
    print("ip bases:")
    for b: UInt64 in 0..<9 {
        let (base, resolved) = getIPBase(conn, b)
        print(String(format: "  %@:%@ %@ %#llx",
                     ipBlockName(b),
                     String(repeating: " ", count: max(0, 7 - ipBlockName(b).count)),
                     resolved ? "resolved" : "MISSING ",
                     base))
    }
}

func initDevice(_ conn: io_connect_t, stage: UInt64) {
    let (kr, out) = callScalar(conn, kInitDevice,
                               input: [stage], outputCount: 1)
    let reached = out.first ?? 0
    if kr == KERN_SUCCESS {
        print("init:      reached stage \(reached) (target \(stage))")
    } else {
        warn("InitDevice(target=\(stage)) kr=\(String(format: "%#x", kr)) "
             + "reached=\(reached)")
    }
}

func loadFirmware(_ conn: io_connect_t, type: UInt64, path: String) {
    let url = URL(fileURLWithPath: path)
    guard let data = try? Data(contentsOf: url) else {
        warn("could not read firmware at \(path)")
        return
    }
    let size = UInt64(data.count)
    print("fw:        loading \(path) (\(size) bytes, type=\(type))")

    // Allocate DMA buffer sized for the firmware, copy into it.
    _ = mustScalar(conn, kAllocateDMABuffer,
                   input: [size, 4096], outputCount: 2,
                   name: "AllocateDMABuffer(fw)")
    let (p, _) = mapMemory(conn, kMemDMABuffer, "DMABuffer")
    data.withUnsafeBytes { raw in
        _ = memcpy(p, raw.baseAddress, Int(size))
    }
    let (kr, _) = callScalar(conn, kLoadFirmware, input: [type, size])
    _ = IOConnectUnmapMemory64(conn, kMemDMABuffer, mach_task_self_,
                               mach_vm_address_t(UInt(bitPattern: p)))
    _ = callScalar(conn, kFreeDMABuffer)
    if kr == KERN_SUCCESS {
        print("fw:        load ok")
    } else {
        warn("LoadFirmware kr=\(String(format: "%#x", kr))")
    }
}

func loadDiscovery(_ conn: io_connect_t, path: String) {
    let url = URL(fileURLWithPath: path)
    guard let data = try? Data(contentsOf: url) else {
        warn("could not read discovery binary at \(path)")
        return
    }
    let size = UInt64(data.count)
    print("discovery:  loading \(path) (\(size) bytes)")

    _ = mustScalar(conn, kAllocateDMABuffer,
                   input: [size, 16384], outputCount: 2,
                   name: "AllocateDMABuffer(discovery)")
    let (p, _) = mapMemory(conn, kMemDMABuffer, "DMABuffer")
    data.withUnsafeBytes { raw in
        _ = memcpy(p, raw.baseAddress, Int(size))
    }
    let (kr, out) = callScalar(conn, kLoadDiscoveryBin,
                               input: [size], outputCount: 2)
    _ = IOConnectUnmapMemory64(conn, kMemDMABuffer, mach_task_self_,
                               mach_vm_address_t(UInt(bitPattern: p)))
    _ = callScalar(conn, kFreeDMABuffer)
    if kr == KERN_SUCCESS, out.first == 1 {
        print("discovery:  parsed ok (\(out[1]) ips total)")
    } else {
        warn("LoadDiscoveryBin kr=\(String(format: "%#x", kr)) "
             + "ok=\(out.first ?? 0)")
    }
}

func resetDevice(_ conn: io_connect_t) {
    warn("ResetDevice — issuing FLR")
    _ = callScalar(conn, kResetDevice)
    print("reset:     issued")
}

func queryInfo(_ conn: io_connect_t) {
    print("query:     listing QueryInfo facts")
    do {
        let (kr, o) = callScalar(conn, kQueryInfo,
                                 input: [kInfoGFXVersion], outputCount: 3)
        if kr == KERN_SUCCESS && o.count >= 3 {
            print("query:       gfx version \(o[0]).\(o[1]).\(o[2])")
        } else {
            warn("query: gfx version kr=\(String(format: "%#x", kr))")
        }
    }
    do {
        let (kr, o) = callScalar(conn, kQueryInfo,
                                 input: [kInfoVRAMSizes], outputCount: 2)
        if kr == KERN_SUCCESS && o.count >= 2 {
            print("query:       vram visible=\(o[0]) total=\(o[1]) bytes")
        }
    }
    do {
        let (kr, o) = callScalar(conn, kQueryInfo,
                                 input: [kInfoIPVersions], outputCount: 4)
        if kr == KERN_SUCCESS && o.count >= 4 {
            func unpack(_ v: UInt64) -> String {
                let mj = (v >> 16) & 0xFF
                let mi = (v >> 8) & 0xFF
                let rv = v & 0xFF
                return "\(mj).\(mi).\(rv)"
            }
            print("query:       gmc=\(unpack(o[0])) sdma=\(unpack(o[1])) "
                  + "psp=\(unpack(o[2])) smu=\(unpack(o[3]))")
        }
    }
    do {
        let (kr, o) = callScalar(conn, kQueryInfo,
                                 input: [kInfoBringupReached], outputCount: 1)
        if kr == KERN_SUCCESS && o.count >= 1 {
            print("query:       bringup reached stage \(o[0])")
        }
    }
}

func boTest(_ conn: io_connect_t) {
    print("bo test:   alloc 16KB BO")
    let (kr, o) = callScalar(conn, kBOAlloc, input: [16384], outputCount: 3)
    if kr != KERN_SUCCESS || o.count < 3 {
        warn("bo test: BOAlloc kr=\(String(format: "%#x", kr))")
        return
    }
    let handle = o[0]
    let bus    = o[1]
    let off    = o[2]
    print("bo test:     handle=\(String(format: "%#llx", handle)) "
          + "bus=\(String(format: "%#llx", bus)) "
          + "off=\(String(format: "%#llx", off))")

    let (kr2, oi) = callScalar(conn, kBOGetInfo, input: [handle], outputCount: 3)
    if kr2 == KERN_SUCCESS && oi.count >= 3 {
        print("bo test:     info bus=\(String(format: "%#llx", oi[0])) "
              + "off=\(String(format: "%#llx", oi[1])) size=\(oi[2])")
    }

    let (kr3, _) = callScalar(conn, kBOFree, input: [handle], outputCount: 0)
    print("bo test:     free kr=\(String(format: "%#x", kr3))")
}

func sdmaCopyTest(_ conn: io_connect_t) {
    // Userspace must have already AllocateDMABuffer'd a region big enough
    // for src+dst; here we use offsets 0 and 64 KB into a 1 MB region.
    print("sdma test: COPY_LINEAR 4 KB src→dst within DMA buf")
    let src:  UInt64 = 0
    let dst:  UInt64 = 64 * 1024
    let cnt:  UInt64 = 4 * 1024
    let to:   UInt64 = 1_000_000
    let (kr, o) = callScalar(conn, kSDMACopyTest,
                             input: [0, src, dst, cnt, to], outputCount: 1)
    let st = o.first ?? UInt64.max
    if kr == KERN_SUCCESS && Int64(bitPattern: st) == 0 {
        print("sdma test:   ok")
    } else {
        warn("sdma test: kr=\(String(format: "%#x", kr)) "
             + "inner=\(String(format: "%#x", st))")
    }
}

func submitTestPM4(_ conn: io_connect_t, timeoutUs: UInt64 = 1_000_000) {
    print("pm4 test:  emitting NOP + RELEASE_MEM (timeout \(timeoutUs) µs)")
    let (kr, out) = callScalar(conn, kSubmitTestPM4,
                               input: [timeoutUs], outputCount: 1)
    let fence = out.first ?? 0
    if kr == KERN_SUCCESS && fence != 0 {
        print("pm4 test:  EOP fence \(fence) observed")
    } else {
        warn("pm4 test: kr=\(String(format: "%#x", kr)) fence=\(fence) "
             + "(needs IP bases resolved + CP HQD programmed; check os_log)")
    }
}

// --- Main ---------------------------------------------------------------

let args = CommandLine.arguments
let wantInit  = args.contains("--init")
let wantReset = args.contains("--reset")
var sosPath: String? = nil
if let i = args.firstIndex(of: "--load-sos"), i + 1 < args.count {
    sosPath = args[i + 1]
}
var discoveryPath: String? = nil
if let i = args.firstIndex(of: "--load-discovery"), i + 1 < args.count {
    discoveryPath = args[i + 1]
}
let wantSubmitTest = args.contains("--submit-test")
let wantQueryInfo  = args.contains("--query-info")
let wantBOTest     = args.contains("--bo-test")
let wantSDMATest   = args.contains("--sdma-test")

print("opening MacAMDGPU service…")
let conn = openService()
defer { IOServiceClose(conn) }
print("service:   connected (handle=\(conn))")

ping(conn)
getIdentity(conn)
enumerateBARs(conn)
mapBAR0(conn)      // triggers lazy PCI Open + populates BringupContext
_ = testDMA(conn)
testIRQ(conn)

if let p = discoveryPath {
    loadDiscovery(conn, path: p)
    showIPBases(conn)
}

if wantInit || sosPath != nil {
    if discoveryPath == nil { showIPBases(conn) }
    initDevice(conn, stage: kStageIPDiscovery)
    initDevice(conn, stage: kStagePSPInit)

    if let p = sosPath {
        loadFirmware(conn, type: kFwSOS, path: p)
        // load_sos is also exposed via InitDevice(stage=PSPLoadSOS), but
        // the firmware bytes have to land in fw_pri before SOS can come
        // alive, so LoadFirmware is the entry point — it calls
        // psp_load_sos internally.
    } else {
        // Just try the stage without firmware to confirm it fails
        // cleanly with a useful error.
        initDevice(conn, stage: kStagePSPLoadSOS)
    }
}

if wantQueryInfo {
    queryInfo(conn)
}

if wantBOTest {
    boTest(conn)
}

if wantSDMATest {
    sdmaCopyTest(conn)
}

if wantSubmitTest {
    submitTestPM4(conn)
}

if wantReset {
    resetDevice(conn)
}
print("ok.")
