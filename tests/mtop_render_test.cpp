// Exercise the actual renderers with a synthetic driver response; no IOKit.
#define main mtop_program_main
#include "../amdgpu_mtop/main.cpp"
#undef main
#include <cassert>
namespace mtop {
std::vector<Device> discover(std::string &) { return {}; }
}

int main() {
    using namespace amdgpu;
    using namespace vram_accounting;
    mtop::Device d;
    d.registry = 1; d.build = 178;
    VRAMBumpAllocator low, high;
    low.init(24 * 1024 * 1024, 232 * 1024 * 1024);
    high.init(256 * 1024 * 1024, 767 * 1024 * 1024);
    d.accounting = snapshot(true, 0, 1024 * 1024 * 1024, 256 * 1024 * 1024, low, high);
    d.accountingSupported = true;
    assert(mtop::hasAccounting(d));
    std::ostringstream captured;
    auto *previous = std::cout.rdbuf(captured.rdbuf());
    mtop::Selection selected;
    selected.registry = 1;
    json({d}, selected, {});
    dashboard({d}, selected, {}, false, false);
    std::cout.rdbuf(previous);
    auto output = captured.str();
    assert(output.find("\"visible_pool_used_bytes\":0") != std::string::npos);
    assert(output.find("\"device_pool_free_bytes\":804257792") != std::string::npos);
    assert(output.find("\"excluded_bytes\":26214400") != std::string::npos);
    assert(output.find("\"vram_used_bytes\":null") != std::string::npos);
    assert(output.find("\"umc_activity_percent\":null") != std::string::npos);
    assert(output.find("0.23 GiB / 0.00 GiB / 0.23 GiB / 0.23 GiB") != std::string::npos);
    d.accounting = snapshot(false, 0, 0, 0, low, high);
    assert(!mtop::hasAccounting(d));
    captured.str(""); captured.clear();
    previous = std::cout.rdbuf(captured.rdbuf());
    json({d}, selected, {});
    std::cout.rdbuf(previous);
    assert(captured.str().find("\"visible_pool_used_bytes\":null") != std::string::npos);
    d.accountingSupported = false;
    assert(!mtop::hasAccounting(d));
}
