#include <cassert>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
using kern_return_t = int;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnUnsupported=2;
enum class IPBlock { GC, GMC };
// Actual discovery resolves GC but never populates the abstract GMC slot.
struct IP { bool isResolved(IPBlock block) const { return block==IPBlock::GC; } };
struct DeviceContext { IP ip; struct { struct { unsigned gfx_ring0=0; } index; } doorbell; };
struct HubContext { IPBlock ip=IPBlock::GC; };
struct GMCContext { HubContext gfxhub; };
struct CPContext { bool inited=false, firmwarePrepared=false, enginesStarted=false, ringReady=false; unsigned doorbell_index=0; };
enum class BringupStage { RLCInit, CPInit, MESInit, GFXInit };
struct RLCContext { bool microcode_loaded=true, bootload_complete=false; };
using MESContext=int;
struct BringupContext { DeviceContext device; GMCContext gmc; CPContext cp; RLCContext rlc; int psp=0, mes=0, gart=0, gfx=0; };
static std::vector<std::string> events;
static std::string failAt;
static int step(const char *name) { events.emplace_back(name); return failAt==name ? 99 : 0; }
#define CP_LOG(...) do {} while(0)
#define INIT_LOG(...) do {} while(0)
static void cp_log_control(const DeviceContext &, const char *) {}
static int cp_alloc_storage(DeviceContext &, GMCContext &, CPContext &cp) { auto r=step("storage"); cp.inited=!r; return r; }
static int cp_enable(const DeviceContext &, bool enable) { return step(enable?"gfx-on":"gfx-off"); }
static int cp_compute_enable(const DeviceContext &, bool enable) { return step(enable?"mec-on":"mec-off"); }
static int cp_configure_rs64(const DeviceContext &, CPContext &) { return step("rs64"); }
static int cp_set_doorbell_range(const DeviceContext &, CPContext &, int, int) { return step("doorbells"); }
static int cp_map_gfx_queue(DeviceContext &, GMCContext &, CPContext &, MESContext &) { return step("map-gfx"); }
#include "cp_startup_under_test.inc"
static int gmc_gfxhub_gart_enable(DeviceContext &, GMCContext &) { return step("gfxhub"); }
static int gmc_hdp_flush(DeviceContext &) { return step("hdp"); }
static int gmc_flush_gpu_tlb(DeviceContext &, GMCContext &, HubContext, int, int) { return step("tlb"); }
static int gart_init(DeviceContext &, GMCContext &, int &) { return step("gart"); }
static int gfx_constants_init(DeviceContext &, int &) { return step("constants"); }
static int rlc_wait_for_autoload_complete(DeviceContext &, RLCContext &r) { auto err=step("autoload"); r.bootload_complete=!err; return err; }
static int rlc_init_full(DeviceContext &, GMCContext &, RLCContext &r) { assert(r.bootload_complete); return step("rlc-resume"); }
static int mes_init_full(DeviceContext &, int &, GMCContext &, int &) { return step("mes"); }
static int stage(BringupContext &ctx, BringupStage which) {
    switch (which) {
#include "cp_stages_under_test.inc"
    }
    return kIOReturnUnsupported;
}
static int run(BringupContext &ctx) {
    for (auto s : {BringupStage::RLCInit, BringupStage::CPInit, BringupStage::MESInit, BringupStage::GFXInit})
        if (auto r=stage(ctx,s)) return r;
    return 0;
}
int main() {
    BringupContext ctx;
    assert(cp_init_full(ctx.device,ctx.gmc,ctx.cp,ctx.mes)==kIOReturnNotReady);
    assert(events.empty());
    assert(stage(ctx,BringupStage::RLCInit)==0);
    assert(stage(ctx,BringupStage::CPInit)==0);
    assert(ctx.cp.firmwarePrepared && !ctx.cp.ringReady);
    assert(stage(ctx,BringupStage::MESInit)==0 && !ctx.cp.ringReady);
    assert(stage(ctx,BringupStage::GFXInit)==0 && ctx.cp.ringReady);
    const std::vector<std::string> expected={"autoload","storage","gfx-off","mec-off","rs64","doorbells",
        "gfxhub","hdp","tlb","gart","constants","rlc-resume","mec-on","gfx-on","mes","map-gfx"};
    assert(events==expected);
    const auto before=events.size();
    assert(cp_init_full(ctx.device,ctx.gmc,ctx.cp,ctx.mes)==0 && events.size()==before);
    // A failed prerequisite cannot publish a usable queue or run later stages.
    for (const auto &name : expected) {
        if (name=="gart") continue;
        failAt=name; events.clear(); ctx={};
        assert(run(ctx)==99 && !ctx.cp.ringReady);
        assert(events.back()==name);
        assert(std::equal(events.begin(),events.end(),expected.begin()));
    }
    // GTT support is optional; failure leaves the established VRAM path usable.
    failAt="gart"; events.clear(); ctx={};
    assert(run(ctx)==0 && ctx.cp.ringReady && events==expected);
    puts("CP startup: RLC autoload -> RS64/GFXHUB/constants -> RLC resume -> MEC/GFX enable -> MES -> GFX MQD mapping; failure gates pass");
}
