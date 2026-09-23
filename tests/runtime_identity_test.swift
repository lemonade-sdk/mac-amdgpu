// Test the production identity decision and connection ownership used by the
// host. The shell script supplies the exact production bodies at these markers.
// PRODUCTION_IDENTITY

private typealias io_connect_t = UInt32
private let KERN_SUCCESS: Int32 = 0
private let kIOReturnNotFound: Int32 = -1
private var events = [String]()
private var opened: (Int32, io_connect_t) = (0, 42)
private var queried = DriverRuntimeIdentity(transport: 0,
    output: [0x414D444750554142, 1, 159], count: 3)
@discardableResult
private func IOServiceClose(_ connection: io_connect_t) -> Int32 {
    events.append("close:\(connection)")
    return 0
}
private enum ProbeHarness {
    static func openDriverConnection(bundleID: String) -> (Int32, io_connect_t) {
        assert(bundleID == "test.driver")
        events.append("open")
        return opened
    }
    static func queryRuntime(connection: io_connect_t) -> DriverRuntimeIdentity {
        events.append("query:\(connection)")
        return queried
    }
    // PRODUCTION_PROBE
}
private func check(_ condition: @autoclosure () -> Bool) {
    precondition(condition())
}
private let magic: UInt64 = 0x414D444750554142
private let good = DriverRuntimeIdentity(transport: 0, output: [magic, 1, 159], count: 3)
check(good.permitsHardware(expectedBuild: 159))
check(!good.permitsHardware(expectedBuild: 158))
check(!good.permitsHardware(expectedBuild: nil))
for bad in [
    DriverRuntimeIdentity(transport: -1, output: [magic, 1, 159], count: 3),
    DriverRuntimeIdentity(transport: 0, output: [magic, 1, 159], count: 2),
    DriverRuntimeIdentity(transport: 0, output: [magic, 1], count: 3),
    DriverRuntimeIdentity(transport: 0, output: [magic + 1, 1, 159], count: 3),
    DriverRuntimeIdentity(transport: 0, output: [magic, 2, 159], count: 3),
    DriverRuntimeIdentity(transport: 0, output: [magic, 1, 0], count: 3)
] {
    check(!bad.permitsConnection(expectedBuild: 159, allowUnverified: false))
    check(bad.permitsConnection(expectedBuild: 159, allowUnverified: true))
    check(bad.build == nil)
}
private let older = DriverRuntimeIdentity(transport: 0, output: [magic, 1, 158], count: 3)
check(!older.permitsConnection(expectedBuild: 159, allowUnverified: false))
check(older.permitsConnection(expectedBuild: 159, allowUnverified: true))

// Every newly opened probe closes even if it reports a mismatching or unknown
// runtime. Existing initialized sessions only receive the read-only query.
for identity in [good, older, DriverRuntimeIdentity(transport: -2, output: [], count: 0)] {
    queried = identity
    opened = (0, 42)
    events = []
    let temporary = ProbeHarness.probeRuntime(bundleID: "test.driver", existingConnection: 0)
    check(temporary.connectionError == 0)
    check(temporary.identity.build == identity.build)
    check(events == ["open", "query:42", "close:42"])
    events = []
    let active = ProbeHarness.probeRuntime(bundleID: "test.driver", existingConnection: 99)
    check(active.identity.build == identity.build)
    check(events == ["query:99"])
}
opened = (kIOReturnNotFound, 0)
events = []
private let absent = ProbeHarness.probeRuntime(bundleID: "test.driver", existingConnection: 0)
check(absent.connectionError == kIOReturnNotFound && absent.identity.build == nil)
check(events == ["open"])
print("Runtime guard: strict build/ABI validation, legacy shutdown exception, temporary-client closure and active-session preservation pass")
