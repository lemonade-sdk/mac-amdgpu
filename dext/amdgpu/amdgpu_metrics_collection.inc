// Included by amdgpu_metrics.cpp. All entry points require device-queue
// serialization across setup, mailbox requests, collection and teardown.

static void smu_metrics_error(SMUMetricsContext &ctx, kern_return_t status,
                              bool faulted)
{
    ctx.faulted |= faulted;
    ctx.snapshot.version = kSMUMetricsSnapshotVersion;
    ctx.snapshot.size = sizeof(SMUMetricsSnapshot);
    ctx.snapshot.driverInterface = ctx.driverInterface;
    ctx.snapshot.status = static_cast<uint32_t>(status);
    ctx.snapshot.flags = ctx.faulted ? kSMUMetricsFaulted : 0;
    ctx.snapshot.validFields = 0;
    memset(ctx.snapshot.values, 0, sizeof(ctx.snapshot.values));
}

void smu_metrics_invalidate(SMUMetricsContext &ctx, kern_return_t status)
{
    ctx.setupComplete = false;
    smu_metrics_error(ctx, status, false);
}

void smu_metrics_snapshot(const SMUMetricsContext &ctx, bool runtimeReady,
                          SMUMetricsSnapshot &out)
{
    out = ctx.snapshot;
    out.version = kSMUMetricsSnapshotVersion;
    out.size = sizeof(SMUMetricsSnapshot);
    out.driverInterface = ctx.driverInterface;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const bool stale = (out.flags & kSMUMetricsValid) &&
        (now < out.collectedAtNs || now - out.collectedAtNs > kSMUMetricsStaleAfterNs);
    if (!runtimeReady || !ctx.setupComplete || stale || !(out.flags & kSMUMetricsValid)) {
        out.flags &= ~kSMUMetricsValid;
        if (stale) out.flags |= kSMUMetricsStale;
        out.validFields = 0;
        memset(out.values, 0, sizeof(out.values));
        if (!out.status) out.status = static_cast<uint32_t>(kIOReturnNotReady);
    }
}

void smu_clock_snapshot(const SMUMetricsContext &ctx, bool runtimeReady,
                        SMUClockSnapshot &out)
{
    out = ctx.clocks;
    out.version = 1; out.size = sizeof(out);
    out.generation = ctx.snapshot.generation;
    out.driverInterface = ctx.driverInterface;
    out.firmwareVersion = ctx.firmwareVersion;
    SMUMetricsSnapshot metrics{};
    smu_metrics_snapshot(ctx, runtimeReady, metrics);
    out.flags = metrics.flags;
    if (!(metrics.flags & kSMUMetricsValid)) {
        out.currentValid = 0;
        memset(out.currentMHz, 0, sizeof(out.currentMHz));
    }
    if (!runtimeReady || !ctx.setupComplete) out.limitsValid = 0;
}

kern_return_t smu_collect_metrics(const DeviceContext &dev,
    SMUMetricsContext &ctx, bool runtimeReady)
{
    if (ctx.collecting) return kIOReturnBusy;
    if (ctx.faulted) return static_cast<kern_return_t>(ctx.snapshot.status);
    auto fail = [&ctx](kern_return_t status, bool faulted) {
        smu_metrics_error(ctx, status, faulted);
        return status;
    };
    if (!runtimeReady || !ctx.setupComplete || !ctx.reserved ||
        !ctx.addressProgrammed || !dev.smuOnline || !dev.gmcReady)
        return fail(kIOReturnNotReady, false);
    if (!dev.pci) return fail(kIOReturnNotAttached, true);
    if (!ctx.vramBacked || (!metrics::verified_interface(ctx.driverInterface) &&
         ctx.driverInterface != metrics::kCompatibleInterface) ||
        kIP_SMU.major != 14 || kIP_SMU.minor != 0 || kIP_SMU.rev != 3)
        // A layout/address-domain mismatch is a host compatibility limitation,
        // not a failed firmware transaction. Reject before any mailbox access.
        return fail(kIOReturnUnsupported, false);
    if (ctx.tableBytes < metrics::kTableBytes ||
        !vram_io_range(dev, ctx.tableVRAMOffset, ctx.tableBytes))
        return fail(kIOReturnBadArgument, true);
    // A previous timed-out command may still DMA. A monitor must never replace
    // its destination or turn each refresh into another two-second timeout.
    if (dev.smuMessagePending) return fail(kIOReturnTimeout, true);
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if ((ctx.snapshot.flags & kSMUMetricsValid) && start >= ctx.snapshot.attemptedAtNs &&
        start - ctx.snapshot.attemptedAtNs < kSMUMetricsMinIntervalNs)
        return kIOReturnSuccess;
    ctx.snapshot.attemptedAtNs = start;
    ctx.collecting = true;
    if (!ctx.firmwareVersionRead) {
        uint32_t version = 0;
        const auto result = smu_get_version(dev, &version); // Linux GetSmuVersion0x02.
        if (result != kIOReturnSuccess) {
            ctx.collecting = false;
            return fail(result, true);
        }
        ctx.firmwareVersion = version;
        ctx.firmwareVersionRead = true;
    }
    if (!metrics::verified_interface(ctx.driverInterface) &&
        !metrics::linux_compatible(ctx.driverInterface, ctx.firmwareVersion)) {
        ctx.collecting = false;
        return fail(kIOReturnUnsupported, false);
    }
    // The driver default dispatch queue serializes this entire transaction,
    // including CPU copy, with every other user of the shared driver table.
    auto result = smu_transfer_table_smu_to_dram(dev, metrics::kFirmwareTable);
    if (result != kIOReturnSuccess) {
        ctx.collecting = false;
        return fail(result, true);
    }

    // Linux calls amdgpu_hdp_invalidate here, but hdp_v7_0 implements no
    // invalidate_hdp callback. Read through the same DriverKit PCI BAR0
    // accessor used by verified SDMA readback, never a CPU cached mapping.
    // The successful firmware response precedes these synchronous PCI reads.
    uint8_t raw[metrics::kTableBytes];
    const uint64_t readStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (size_t i = 0; i < sizeof(raw); i += sizeof(uint32_t)) {
        uint32_t word = UINT32_MAX;
        dev.pci->MemoryRead32(dev.bar0MemIndex, ctx.tableVRAMOffset + i, &word);
        memcpy(raw + i, &word, sizeof(word));
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - readStart > 100000000ull) {
            ctx.collecting = false;
            return fail(kIOReturnTimeout, true);
        }
    }
    metrics::Sample sample;
    const bool decoded = metrics::decode(raw, sizeof(raw), kIP_SMU.major,
        kIP_SMU.minor, kIP_SMU.rev, ctx.driverInterface, sample, ctx.firmwareVersion);
    if (!decoded || !sample.valid) {
        ctx.collecting = false;
        return fail(kIOReturnIOError, true);
    }
    // CurrClock[] uses uint32 MHz in Linux; these are distinct from the
    // pre/post-deep-sleep averages exported by the original metrics payload.
    ctx.clocks.currentValid = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const size_t o = i * 4;
        const uint32_t value = uint32_t(raw[o]) | (uint32_t(raw[o+1]) << 8) |
            (uint32_t(raw[o+2]) << 16) | (uint32_t(raw[o+3]) << 24);
        ctx.clocks.currentMHz[i] = value <= 10000 ? value : 0;
        if (value <= 10000) ctx.clocks.currentValid |= 1u << i;
    }
    // Read-only DPM range queries, once per initialized context. Do not infer
    // current AC/DC state or active throttling limits from these AC maxima.
    if (!ctx.clockLimitsAttempted) {
        ctx.clockLimitsAttempted = true;
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t minimum = 0, maximum = 0;
            auto clockResult = smu_send_msg_with_param(dev, 0x1d, i << 16, &minimum);
            if (clockResult == kIOReturnSuccess)
                clockResult = smu_send_msg_with_param(dev, 0x1e, i << 16, &maximum);
            ctx.clocks.status = static_cast<uint32_t>(clockResult);
            if (clockResult == kIOReturnTimeout || dev.smuMessagePending) {
                ctx.collecting = false;
                return fail(kIOReturnTimeout, true);
            }
            if (clockResult != kIOReturnSuccess) break;
            if (minimum && minimum <= maximum && maximum <= 10000) {
                ctx.clocks.minimumMHz[i] = minimum;
                ctx.clocks.maximumACMHz[i] = maximum;
                ctx.clocks.limitsValid |= 1u << i;
            }
        }
    }
    ctx.clocks.collectedAtNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ctx.snapshot.version = kSMUMetricsSnapshotVersion;
    ctx.snapshot.size = sizeof(SMUMetricsSnapshot);
    ctx.snapshot.status = static_cast<uint32_t>(kIOReturnSuccess);
    ctx.snapshot.flags = kSMUMetricsValid |
        (metrics::linux_compatible(ctx.driverInterface, ctx.firmwareVersion)
            ? kSMUMetricsLinuxCompatible : 0);
    ++ctx.snapshot.sequence;
    ctx.snapshot.collectedAtNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ctx.snapshot.driverInterface = ctx.driverInterface;
    ctx.snapshot.firmwareCounter = sample.firmwareCounter;
    ctx.snapshot.validFields = sample.valid;
    memcpy(ctx.snapshot.values, sample.value, sizeof(sample.value));
    ctx.collecting = false;
    return kIOReturnSuccess;
}
