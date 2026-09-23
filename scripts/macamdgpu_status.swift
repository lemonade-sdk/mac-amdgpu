// Build: swiftc -module-cache-path build/ModuleCache -framework IOKit
//        scripts/macamdgpu_status.swift -o build/macamdgpu_status
// Query the loaded driver without allocating buffers, submitting work,
// initializing engines, or resetting the device. Supports older drivers.
import Foundation
import IOKit

func fail(_ message: String) -> Never {
    fputs(message + "\n", stderr)
    exit(1)
}

var iterator: io_iterator_t = 0
guard IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceNameMatching("MacAMDGPU"),
                                   &iterator) == KERN_SUCCESS else {
    fail("Cannot enumerate MacAMDGPU services")
}
defer { IOObjectRelease(iterator) }
var found = false
while case let service = IOIteratorNext(iterator), service != 0 {
    found = true
    defer { IOObjectRelease(service) }
    var registryID: UInt64 = 0
    IORegistryEntryGetRegistryEntryID(service, &registryID)
    print(String(format: "MacAMDGPU registry ID %#llx", registryID))
    var connection: io_connect_t = 0
    let openResult = IOServiceOpen(service, mach_task_self_, 0, &connection)
    guard openResult == KERN_SUCCESS else {
        fail(String(format: "IOServiceOpen failed: %#x", openResult))
    }
    defer { IOServiceClose(connection) }
    func query(_ selector: UInt32, _ input: [UInt64] = [], _ count: Int) -> [UInt64]? {
        var output = [UInt64](repeating: 0, count: count)
        var outputCount = UInt32(count)
        let kr = input.withUnsafeBufferPointer {
            IOConnectCallScalarMethod(connection, selector, $0.baseAddress,
                                      UInt32(input.count), &output, &outputCount)
        }
        guard kr == KERN_SUCCESS, outputCount == count else {
            print(String(format: "  selector %u: kr=%#x count=%u", selector, kr, outputCount))
            return nil
        }
        return output
    }
    guard let ping = query(0, [], 1), ping[0] == 0xA117AB1E else {
        fail("Driver ping failed")
    }
    if let id = query(1, [], 7) {
        print(String(format: "  PCI %02llx:%02llx.%llu %04llx:%04llx revision=%02llx",
                     id[0], id[1], id[2], id[3], id[4], id[6]))
    }
    for bar in [UInt64(0), 2, 5] {
        if let info = query(2, [bar], 3) {
            print(String(format: "  BAR%llu memoryIndex=%llu bytes=%llu type=%#llx",
                         bar, info[0], info[1], info[2]))
        }
        if let info = query(41, [bar], 6) {
            print(String(format: "  ReBAR cap=%#llx sizes=%#llx selected=%llu assigned=%llu",
                         info[0], info[3], info[4], info[5]))
        }
    }
    if let info = query(21, [2], 2) {
        print("  VRAM visible=\(info[0]) total=\(info[1]) bytes (zero before GMCInit)")
    }
    if let info = query(21, [4], 1) {
        print("  Bringup reached stage \(info[0]) (15 = SDMAInit; does not prove data correctness)")
    }
}
if !found { fail("No attached MacAMDGPU service") }
