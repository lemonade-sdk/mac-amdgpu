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
    if (!ctx.vramBacked || ctx.driverInterface != metrics::kDriverInterface ||
        kIP_SMU.major != 14 || kIP_SMU.minor != 0 || kIP_SMU.rev != 3)
        return fail(kIOReturnUnsupported, true);
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
        kIP_SMU.minor, kIP_SMU.rev, ctx.driverInterface, sample);
    ctx.collecting = false;
    if (!decoded || !sample.valid) return fail(kIOReturnIOError, true);
    ctx.snapshot.version = kSMUMetricsSnapshotVersion;
    ctx.snapshot.size = sizeof(SMUMetricsSnapshot);
    ctx.snapshot.status = static_cast<uint32_t>(kIOReturnSuccess);
    ctx.snapshot.flags = kSMUMetricsValid;
    ++ctx.snapshot.sequence;
    ctx.snapshot.collectedAtNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ctx.snapshot.driverInterface = ctx.driverInterface;
    ctx.snapshot.firmwareCounter = sample.firmwareCounter;
    ctx.snapshot.validFields = sample.valid;
    memcpy(ctx.snapshot.values, sample.value, sizeof(sample.value));
    return kIOReturnSuccess;
}
