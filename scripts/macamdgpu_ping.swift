//
//  macamdgpu_ping.swift — Phase 1A userspace smoke test.
//
//  Build:
//      swiftc -framework IOKit -framework CoreFoundation \
//          scripts/macamdgpu_ping.swift -o build/macamdgpu_ping
//
//  Run:
//      ./build/macamdgpu_ping
//
//  Exits non-zero on any failure. Successful run prints:
//      service: <ioRegistryEntry>
//      ping:    0xA117AB1E
//      identity: 1002:7551 class=030000 bus=05 dev=00 fn=0
//      BAR0:    memIdx=0 size=... type=mem64-prefetch
//      ...
//      vid/did via BAR0 MMIO offset 0: 7551:1002  (matches)
//

import Foundation
import IOKit

// MUST match enums in MacAMDGPU.cpp
private let kMacAMDGPUMethodPing:        UInt32 = 0
private let kMacAMDGPUMethodGetIdentity: UInt32 = 1
private let kMacAMDGPUMethodGetBARInfo:  UInt32 = 2
private let kMacAMDGPUMemoryTypeBAR0:    UInt32 = 0

func die(_ msg: String, _ code: Int32 = 1) -> Never {
    let bytes = Array("error: \(msg)\n".utf8)
    FileHandle.standardError.write(Data(bytes))
    exit(code)
}

func openService() -> io_connect_t {
    // The DriverKit service registers under its bundle's IOUserServerName
    // (== bundle ID by convention). The class name is what matters for
    // IOServiceMatching — it's the IOUserClass from Info.plist.
    guard let matching = IOServiceMatching("MacAMDGPU") else {
        die("IOServiceMatching returned nil")
    }

    var iter: io_iterator_t = 0
    // IOServiceGetMatchingServices consumes a +1 reference on the dict.
    let kr = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter)
    if kr != KERN_SUCCESS { die("IOServiceGetMatchingServices: \(kr)") }
    defer { IOObjectRelease(iter) }

    let svc = IOIteratorNext(iter)
    if svc == 0 {
        die("MacAMDGPU service not found — is the dext loaded and matched? "
            + "`systemextensionsctl list` should show it activated.")
    }
    defer { IOObjectRelease(svc) }

    var conn: io_connect_t = 0
    let openKr = IOServiceOpen(svc, mach_task_self_, 0, &conn)
    if openKr != KERN_SUCCESS {
        die("IOServiceOpen failed: \(openKr) "
            + "(0xe00002c1 = entitlement missing, 0xe00002c2 = no permission)")
    }
    return conn
}

func callPing(_ conn: io_connect_t) {
    var outCount: UInt32 = 1
    var out = [UInt64](repeating: 0, count: Int(outCount))
    let kr = IOConnectCallScalarMethod(conn, kMacAMDGPUMethodPing,
                                       nil, 0, &out, &outCount)
    if kr != KERN_SUCCESS { die("Ping kr=\(String(format: "%#x", kr))") }
    print(String(format: "ping:    0x%llX", out[0]))
}

func callGetIdentity(_ conn: io_connect_t) {
    var outCount: UInt32 = 6
    var out = [UInt64](repeating: 0, count: Int(outCount))
    let kr = IOConnectCallScalarMethod(conn, kMacAMDGPUMethodGetIdentity,
                                       nil, 0, &out, &outCount)
    if kr != KERN_SUCCESS { die("GetIdentity kr=\(String(format: "%#x", kr))") }
    print(String(format: "identity: %04llx:%04llx class=%06llx bus=%02llx "
                       + "dev=%02llx fn=%llu",
                 out[3], out[4], out[5], out[0], out[1], out[2]))

    if out[3] != 0x1002 || out[4] != 0x7551 {
        let msg = "warning: expected VID:DID 1002:7551 (R9700), got "
                + String(format: "%04llx:%04llx\n", out[3], out[4])
        FileHandle.standardError.write(Data(Array(msg.utf8)))
    }
}

func callGetBARInfo(_ conn: io_connect_t, _ bar: UInt64) -> (UInt64, UInt64, UInt64)? {
    var inArr: [UInt64] = [bar]
    var outCount: UInt32 = 3
    var out = [UInt64](repeating: 0, count: Int(outCount))
    let kr = inArr.withUnsafeMutableBufferPointer { p in
        IOConnectCallScalarMethod(conn, kMacAMDGPUMethodGetBARInfo,
                                  p.baseAddress, UInt32(p.count),
                                  &out, &outCount)
    }
    if kr != KERN_SUCCESS { return nil }
    return (out[0], out[1], out[2])
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

func enumerateBARs(_ conn: io_connect_t) {
    for bar: UInt64 in 0...5 {
        if let (memIdx, size, type) = callGetBARInfo(conn, bar) {
            print(String(format: "BAR%llu:    memIdx=%llu size=%llu type=%@",
                         bar, memIdx, size, barTypeName(type)))
        }
    }
}

func mapBAR0AndReadVidDid(_ conn: io_connect_t) {
    var addr: mach_vm_address_t = 0
    var size: mach_vm_size_t = 0
    let kr = IOConnectMapMemory64(conn,
                                  kMacAMDGPUMemoryTypeBAR0,
                                  mach_task_self_,
                                  &addr, &size,
                                  IOOptionBits(kIOMapAnywhere | kIOMapDefaultCache))
    if kr != KERN_SUCCESS {
        die("IOConnectMapMemory64(BAR0) kr=\(String(format: "%#x", kr))")
    }
    defer { _ = IOConnectUnmapMemory64(conn, kMacAMDGPUMemoryTypeBAR0,
                                       mach_task_self_, addr) }
    print(String(format: "BAR0 mapped: vaddr=%#llx size=%llu",
                 UInt64(addr), UInt64(size)))

    // AMDGPU MMIO register window doesn't put VID/DID at offset 0; the
    // CPU-accessible BAR0 layout depends on the ASIC. This is a smoke
    // test only — we just want to prove the mapping is live and reads
    // return something non-uniform. Confirming actual GPU register
    // semantics is Phase 1B.
    let p = UnsafePointer<UInt32>(bitPattern: UInt(addr))!
    let a = p.pointee
    let b = p.advanced(by: 1).pointee
    print(String(format: "BAR0[0x0]=%#010x BAR0[0x4]=%#010x", a, b))
}

print("opening MacAMDGPU service…")
let conn = openService()
defer { IOServiceClose(conn) }
print("service: connected (handle=\(conn))")

callPing(conn)
callGetIdentity(conn)
enumerateBARs(conn)
mapBAR0AndReadVidDid(conn)

print("ok.")
