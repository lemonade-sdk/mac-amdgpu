#include "../amdgpu_mtop/model.h"
#include <cassert>

int main() {
    mtop::Device a, b, c;
    a.registry = 101; b.registry = 202; c.registry = 303;
    std::vector<mtop::Device> devices{a, b, c};
    mtop::Selection selection;
    selection.initialize({});
    assert(!selection.registry);
    selection.initialize(devices);
    assert(selection.find(devices)->registry == 101);
    selection.step(devices, true);
    assert(selection.find(devices)->registry == 202);
    devices = {c, b, a};
    assert(selection.find(devices)->registry == 202);
    devices = {a, c};
    selection.initialize(devices);
    assert(selection.registry == 202 && !selection.find(devices));
    selection.step(devices, true);
    assert(selection.registry == 101);
    selection.step(devices, false);
    assert(selection.registry == 303);
    selection.step({}, true);
    assert(selection.registry == 303);
    selection.step({c}, true);
    assert(selection.registry == 303);
    selection.registry = 999;
    selection.initialize(devices);
    assert(!selection.find(devices));

    a.telemetrySupported = true;
    a.metrics.version = amdgpu::kSMUMetricsSnapshotVersion;
    a.metrics.size = sizeof(a.metrics);
    a.metrics.flags = amdgpu::kSMUMetricsValid;
    a.metrics.validFields = 1;
    a.metrics.collectedAtNs = 100;
    a.metrics.driverInterface = amdgpu::metrics::kDriverInterface;
    assert(mtop::validSnapshot(a.metrics) && mtop::fresh(a, 101));
    assert(!mtop::fresh(a, 99));
    assert(!mtop::fresh(a, 101 + amdgpu::kSMUMetricsStaleAfterNs));
    a.metrics.validFields = uint64_t(1) << 63;
    assert(!mtop::validSnapshot(a.metrics));
    a.metrics.validFields = 1;
    a.metrics.status = 1;
    assert(!mtop::validSnapshot(a.metrics));
    a.metrics.status = 0;
    a.metrics.flags |= amdgpu::kSMUMetricsFaulted;
    assert(!mtop::validSnapshot(a.metrics));
    a.metrics.flags = amdgpu::kSMUMetricsFaulted;
    a.metrics.validFields = 0;
    assert(mtop::validSnapshot(a.metrics) && !mtop::fresh(a, 101));
    a.metrics.driverInterface = 0x33;
    assert(mtop::interfaceMismatch(a) && !mtop::fresh(a, 101));
    a.metrics.flags = amdgpu::kSMUMetricsValid;
    a.metrics.validFields = 1;
    assert(mtop::interfaceMismatch(a) && !mtop::fresh(a, 101));
    a.metrics.driverInterface = 0;
    assert(!mtop::interfaceMismatch(a) && !mtop::fresh(a, 101));
}
