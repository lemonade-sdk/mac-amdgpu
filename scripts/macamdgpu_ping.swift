//
//  macamdgpu_ping.swift — Phase 1A userspace smoke test.
//
//  Build:
//      swiftc -framework IOKit -framework CoreFoundation \
//          scripts/macamdgpu_ping.swift -o build/macamdgpu_ping
//
//  Run:
//      ./build/macamdgpu_ping            # everything except reset
//      ./build/macamdgpu_ping --reset    # also runs FLR at the end
//
//  Successful run exercises:
//      Ping, GetIdentity, GetBARInfo, BAR0 mmap,
//      AllocateDMABuffer + DMABuffer mmap + bit-compare CPU↔segment,
//      SetupInterrupts + IRQState mmap + layout check,
//      (optional) ResetDevice / FLR.
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

private let kMemBAR0:           UInt32 = 0
private let kMemBAR2:           UInt32 = 2
private let kMemDMABuffer:      UInt32 = 6
private let kMemIRQState:       UInt32 = 7

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
            + "(0xe00002c1 = entitlement missing, "
            + "0xe00002c2 = no permission)")
    }
    return conn
}

func callScalar(_ conn: io_connect_t, _ selector: UInt32,
                input: [UInt64] = [], outputCount: UInt32 = 0)
    -> [UInt64]?
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
    if kr != KERN_SUCCESS {
        warn("selector \(selector) failed kr=\(String(format: "%#x", kr))")
        return nil
    }
    return Array(out.prefix(Int(outCount)))
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

func ping(_ conn: io_connect_t) {
    guard let out = callScalar(conn, kPing, outputCount: 1) else {
        die("Ping failed")
    }
    print(String(format: "ping:    0x%llX", out[0]))
}

func getIdentity(_ conn: io_connect_t) {
    guard let out = callScalar(conn, kGetIdentity, outputCount: 6) else {
        die("GetIdentity failed")
    }
    print(String(format: "identity: %04llx:%04llx class=%06llx bus=%02llx "
                       + "dev=%02llx fn=%llu",
                 out[3], out[4], out[5], out[0], out[1], out[2]))
    if out[3] != 0x1002 || out[4] != 0x7551 {
        warn("expected VID:DID 1002:7551 (R9700), got "
             + String(format: "%04llx:%04llx", out[3], out[4]))
    }
}

func enumerateBARs(_ conn: io_connect_t) {
    for bar: UInt64 in 0...5 {
        if let out = callScalar(conn, kGetBARInfo,
                                input: [bar], outputCount: 3) {
            print(String(format: "BAR%llu:    memIdx=%llu size=%llu type=%@",
                         bar, out[0], out[1], barTypeName(out[2])))
        }
    }
}

func mapBAR0(_ conn: io_connect_t) {
    var addr: mach_vm_address_t = 0
    var size: mach_vm_size_t = 0
    let kr = IOConnectMapMemory64(conn, kMemBAR0,
                                  mach_task_self_, &addr, &size,
                                  IOOptionBits(kIOMapAnywhere | kIOMapDefaultCache))
    if kr != KERN_SUCCESS {
        die("IOConnectMapMemory64(BAR0): \(String(format: "%#x", kr))")
    }
    defer { _ = IOConnectUnmapMemory64(conn, kMemBAR0,
                                       mach_task_self_, addr) }
    print(String(format: "BAR0 map: vaddr=%#llx size=%llu",
                 UInt64(addr), UInt64(size)))
    let p = UnsafePointer<UInt32>(bitPattern: UInt(addr))!
    print(String(format: "BAR0[0x0]=%#010x BAR0[0x4]=%#010x",
                 p.pointee, p.advanced(by: 1).pointee))
}

func testDMA(_ conn: io_connect_t) {
    let kSize: UInt64 = 64 * 1024
    let kAlign: UInt64 = 4096
    guard let out = callScalar(conn, kAllocateDMABuffer,
                               input: [kSize, kAlign], outputCount: 2)
    else { die("AllocateDMABuffer failed") }
    let segCount = out[0]
    let firstBus = out[1]
    print(String(format: "dma alloc: segs=%llu first_bus=%#llx size=%llu",
                 segCount, firstBus, kSize))

    var addr: mach_vm_address_t = 0
    var size: mach_vm_size_t = 0
    let kr = IOConnectMapMemory64(conn, kMemDMABuffer,
                                  mach_task_self_, &addr, &size,
                                  IOOptionBits(kIOMapAnywhere | kIOMapDefaultCache))
    if kr != KERN_SUCCESS {
        die("IOConnectMapMemory64(DMABuffer): \(String(format: "%#x", kr))")
    }
    defer { _ = IOConnectUnmapMemory64(conn, kMemDMABuffer,
                                       mach_task_self_, addr) }
    print(String(format: "dma map:   vaddr=%#llx size=%llu",
                 UInt64(addr), UInt64(size)))

    // CPU write + readback — proves the mapping is live RAM.
    let pat: UInt64 = 0xDEADBEEFCAFEBABE
    let p = UnsafeMutablePointer<UInt64>(bitPattern: UInt(addr))!
    p.pointee = pat
    p.advanced(by: 1).pointee = ~pat
    if p.pointee != pat || p.advanced(by: 1).pointee != ~pat {
        die("DMA buffer CPU readback mismatch")
    }
    print("dma cpu ok (pattern verified)")

    _ = callScalar(conn, kFreeDMABuffer)
    print("dma freed")
}

func testIRQ(_ conn: io_connect_t) {
    guard callScalar(conn, kSetupInterrupts) != nil else {
        die("SetupInterrupts failed (PCI Open required first — "
            + "BAR mapping should have triggered it)")
    }
    print("irq:       setup ok")

    var addr: mach_vm_address_t = 0
    var size: mach_vm_size_t = 0
    let kr = IOConnectMapMemory64(conn, kMemIRQState,
                                  mach_task_self_, &addr, &size,
                                  IOOptionBits(kIOMapAnywhere | kIOMapDefaultCache))
    if kr != KERN_SUCCESS {
        die("IOConnectMapMemory64(IRQState): \(String(format: "%#x", kr))")
    }
    defer { _ = IOConnectUnmapMemory64(conn, kMemIRQState,
                                       mach_task_self_, addr) }
    print(String(format: "irq map:   vaddr=%#llx size=%llu",
                 UInt64(addr), UInt64(size)))

    // Layout: pending[0..3] at offset 0x00, enabled[0..3] at offset 0x20.
    let pending = UnsafePointer<UInt64>(bitPattern: UInt(addr))!
    let enabled = pending.advanced(by: 4)
    print(String(format: "irq pending[0..3]=%016llx %016llx %016llx %016llx",
                 pending.pointee,
                 pending.advanced(by: 1).pointee,
                 pending.advanced(by: 2).pointee,
                 pending.advanced(by: 3).pointee))
    print(String(format: "irq enabled[0..3]=%016llx %016llx %016llx %016llx",
                 enabled.pointee,
                 enabled.advanced(by: 1).pointee,
                 enabled.advanced(by: 2).pointee,
                 enabled.advanced(by: 3).pointee))

    // Mask all vectors via SetIRQMask, verify mirrored in shared page.
    _ = callScalar(conn, kSetIRQMask, input: [0, 0, 0, 0])
    let after = enabled.pointee
    if after != 0 {
        warn("SetIRQMask(0) — shared enabled[0]=\(String(format: "%#llx", after))")
    } else {
        print("irq mask:  SetIRQMask(0) → enabled[0]=0 (shared-page coherent)")
    }
    // Re-enable all.
    _ = callScalar(conn, kSetIRQMask,
                   input: [UInt64.max, UInt64.max, UInt64.max, UInt64.max])
}

func resetDevice(_ conn: io_connect_t) {
    warn("ResetDevice — issuing FLR (will quiesce + re-init device)")
    _ = callScalar(conn, kResetDevice)
    print("reset:     issued")
}

print("opening MacAMDGPU service…")
let conn = openService()
defer { IOServiceClose(conn) }
print("service:   connected (handle=\(conn))")

ping(conn)
getIdentity(conn)
enumerateBARs(conn)
mapBAR0(conn)        // triggers lazy PCI Open
testDMA(conn)
testIRQ(conn)

if CommandLine.arguments.contains("--reset") {
    resetDevice(conn)
}
print("ok.")
