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
}
