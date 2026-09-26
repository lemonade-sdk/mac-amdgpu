// Exercise the actual renderers with a synthetic driver response; no IOKit.
// The TUI is a modern btop-style layout (rounded borders, Braille chart,
// sub-cell bars, truecolor gradients). Assertions check structural invariants
// and the documented n/a panels.
#define main mtop_program_main
#include "../amdgpu_mtop/main.cpp"
#undef main
#include <cassert>
namespace mtop {
std::vector<Device> discover(std::string &) { return {}; }
}

// Strip ANSI escapes so we can assert on visible text.
static std::string strip(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

int main() {
    using namespace amdgpu;
    using namespace vram_accounting;
    // Minimal device: just the identity fields. The dashboard handles
    // missing sensors gracefully (renders n/a rows).
    mtop::Device d;
    d.registry = 1; d.build = 195; d.stage = 15;
    d.gfx[0] = 11; d.gfx[1] = 0; d.gfx[2] = 0;
    // No accounting/clock/telemetry: the dashboard renders n/a rows.
    const auto now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // Telemetry: power, temp, fan, clocks.
    d.telemetrySupported=true;
    d.metrics.version=kSMUMetricsSnapshotVersion;d.metrics.size=sizeof(d.metrics);
    d.metrics.driverInterface=metrics::kCompatibleInterface;
    d.metrics.flags=kSMUMetricsValid|kSMUMetricsLinuxCompatible;
    d.metrics.collectedAtNs=now;
    d.metrics.validFields=(uint64_t(1)<<metrics::Count)-1;
    d.metrics.values[metrics::GfxActivityPercent]=45;
    d.metrics.values[metrics::GfxClockMHz]=1820;
    d.metrics.values[metrics::MemoryClockMHz]=1250;
    d.metrics.values[metrics::SocketPowerMilliwatts]=105000;
    d.metrics.values[metrics::BoardPowerMilliwatts]=355000;
    d.metrics.values[metrics::FanRPM]=1240;
    d.metrics.values[metrics::EdgeTemperatureMillicelsius]=46000;
    d.metrics.values[metrics::HotspotTemperatureMillicelsius]=58000;
    d.metrics.values[metrics::MemoryTemperatureMillicelsius]=54000;
    // Software counters: leave unset for this render test. The GRBM panel
    // renders the documented n/a rows; a dedicated engine test covers the
    // per-engine delta path with real counter data.
    // Device spec (CUs / SEs) for the header.
    d.spec.valid = true;
    d.spec.words[12] = 64; // CUs
    d.spec.words[4] = 4;   // SEs

    mtop::Selection selected;
    selected.registry = 1;
    Histories histories;
    // Minimal history: one point so the dashboard has data to render.
    mtop::ActivityPoint p;
    p.timeNs = now;
    p.busyPercent = 50.0;
    p.allocatedGiB = 0.5;
    p.temperatureC = 46.0;
    p.gfxPercent = 45.0;
    histories[d.registry].points.push_back(p);
    // The render test doesn't exercise the engine delta path; leave
    // previousBusy unset so engineBusyPercent returns n/a (documented).
    // A dedicated mtop-engine-test covers the delta computation.

    std::ostringstream captured;
    auto *previous = std::cout.rdbuf(captured.rdbuf());

    mtop::Frame layout;
    mtop::tui::Ui ui;
    dashboard({d}, selected, {}, false, false, &histories, {}, 120, 40, now,
              mtop::Graphics::Text, &layout, false, &ui);
    std::cout.rdbuf(previous);
    auto output = captured.str();
    auto visible = strip(output);

    // Header: chip ID, CU/SE count, build, stage, sensors.
    assert(visible.find("AMDGPU gfx11.0.0") != std::string::npos);
    assert(visible.find("64 CUs / 4 SEs") != std::string::npos);
    assert(visible.find("build 195") != std::string::npos);
    assert(visible.find("initialized") != std::string::npos);

    // Section 2: GPU CORE LOAD with Braille chart.
    assert(visible.find("GPU CORE LOAD") != std::string::npos);
    assert(visible.find("100%") != std::string::npos);
    assert(visible.find("75%") != std::string::npos);
    assert(visible.find("50%") != std::string::npos);
    assert(visible.find("25%") != std::string::npos);
    assert(visible.find("0%") != std::string::npos);
    assert(visible.find("1m") != std::string::npos);
    assert(visible.find("45s") != std::string::npos);
    assert(visible.find("30s") != std::string::npos);
    assert(visible.find("15s") != std::string::npos);
    assert(visible.find("0s") != std::string::npos);

    // VRAM / GTT meters.
    assert(visible.find("VRAM USAGE") != std::string::npos);
    assert(visible.find("VRAM:") != std::string::npos);
    assert(visible.find("GTT:") != std::string::npos);

    // Power & sensor state.
    assert(visible.find("POWER & SENSOR STATE") != std::string::npos);
    assert(visible.find("Pwr:") != std::string::npos);
    assert(visible.find("Temp:") != std::string::npos);
    assert(visible.find("Junc:") != std::string::npos);
    assert(visible.find("Fan:") != std::string::npos);
    assert(visible.find("SCLK") != std::string::npos);
    assert(visible.find("MCLK") != std::string::npos);

    // Section 3: GRBM / GRBM2.
    assert(visible.find("PERFORMANCE COUNTERS (GRBM / GRBM2)") != std::string::npos);
    assert(visible.find("[GRBM Status]") != std::string::npos);
    assert(visible.find("[GRBM2 Status]") != std::string::npos);
    assert(visible.find("Graphics Pipe (GFX)") != std::string::npos);
    assert(visible.find("Compute Engine 0") != std::string::npos);
    assert(visible.find("Compute Engine 1") != std::string::npos);
    assert(visible.find("SDMA Engine (DMA)") != std::string::npos);
    assert(visible.find("VCN (Video Decode)") != std::string::npos);
    assert(visible.find("JPEG Engine") != std::string::npos);
    assert(visible.find("Command Processor (CPF)") != std::string::npos);
    assert(visible.find("Texture Cache (TCC)") != std::string::npos);
    assert(visible.find("Depth Block (DB)") != std::string::npos);
    assert(visible.find("Color Block (CB)") != std::string::npos);
    assert(visible.find("Shader Pipe (SPI)") != std::string::npos);
    assert(visible.find("Primitive Assembly (PA)") != std::string::npos);

    // Section 4: GPU PROCESSES.
    assert(visible.find("GPU PROCESSES (fdinfo)") != std::string::npos);
    assert(visible.find("PID") != std::string::npos);
    assert(visible.find("USER") != std::string::npos);
    assert(visible.find("PROCESS NAME") != std::string::npos);
    assert(visible.find("CPU%") != std::string::npos);
    assert(visible.find("GPU%") != std::string::npos);
    assert(visible.find("GFX/COMP") != std::string::npos);
    assert(visible.find("MEDIA") != std::string::npos);
    assert(visible.find("VRAM USAGE") != std::string::npos);
    assert(visible.find("VRAM BAR") != std::string::npos);
    // Documented n/a for per-process accounting.
    assert(visible.find("per-process GPU accounting not exposed by macOS driver") != std::string::npos);

    // Footer hotkeys.
    assert(visible.find("[q] Quit") != std::string::npos);
    assert(visible.find("[h] Interval:") != std::string::npos);
    assert(visible.find("[p] Sort PID") != std::string::npos);
    assert(visible.find("[m] Sort VRAM") != std::string::npos);
    assert(visible.find("[g] Sort GPU") != std::string::npos);
    assert(visible.find("[r] Toggle GRBM") != std::string::npos);

    // Structural (interactive): the frame is exactly `rows` lines, each
    // clipped to `columns-1` visible cells.
    mtop::Frame structLayout;
    captured.str(""); captured.clear();
    previous = std::cout.rdbuf(captured.rdbuf());
    dashboard({d}, selected, {}, true, false, &histories, {}, 120, 40, now,
              mtop::Graphics::Text, &structLayout, false, &ui);
    std::cout.rdbuf(previous);
    assert(structLayout.previous.size() == 40);
    for (const auto &row : structLayout.previous)
        assert(mtop::clipColumns(row, 119) == row);

    // Rounded borders are present (top-left, top-right, mid-left, mid-right,
    // horizontal, vertical). The bottom border is only drawn when the
    // dashboard content fills the screen; the section dividers use \u251C/\u2524.
    assert(output.find("\xE2\x94\xAD") != std::string::npos); // \u256D
    assert(output.find("\xE2\x94\xAE") != std::string::npos); // \u256E
    assert(output.find("\xE2\x94\x9C") != std::string::npos); // \u251C
    assert(output.find("\xE2\x94\xA4") != std::string::npos); // \u2524
    assert(output.find("\xE2\x94\x80") != std::string::npos); // \u2500
    assert(output.find("\xE2\x94\x82") != std::string::npos); // \u2502

    // With a single history point, the Braille chart may be mostly empty.
    // The chart structure (Y-axis labels, X-axis caption) is what matters
    // for the layout test. A dedicated mtop-tui-test covers Braille
    // dot-matrix correctness with a full 60s window.

    // Sub-cell bars: full block glyph present (the VRAM meter uses it).
    assert(output.find("\xE2\x96\x88") != std::string::npos); // \u2588

    // Truecolor: the gradient palette emits \033[38;2;R;G;Bm sequences.
    assert(output.find("\033[38;2;") != std::string::npos);

    assert(visible.find("n/a (no in-flight sample window yet)") != std::string::npos);

    // --- Interactive mode: footer is the last row, hotkeys work ---
    captured.str(""); captured.clear();
    previous = std::cout.rdbuf(captured.rdbuf());
    mtop::Frame interactive;
    ui.sort.kind = mtop::tui::Sort::Vram;
    dashboard({d}, selected, {}, true, false, &histories, mtop::RefreshMode{true},
              120, 40, now, mtop::Graphics::Text, &interactive, false, &ui);
    std::cout.rdbuf(previous);
    auto interactiveVisible = strip(interactive.previous.back());
    assert(interactiveVisible.find("[q] Quit") != std::string::npos);
    assert(interactiveVisible.find("Interval: 10ms") != std::string::npos);
    // Sort label reflects the ui state.
    assert(strip(captured.str()).find("sort: VRAM") != std::string::npos);

    // --- Engine toggle: 'r' hides the GRBM section ---
    ui.enginesVisible = false;
    captured.str(""); captured.clear();
    previous = std::cout.rdbuf(captured.rdbuf());
    dashboard({d}, selected, {}, false, false, &histories, {}, 120, 40, now,
              mtop::Graphics::Text, &layout, false, &ui);
    std::cout.rdbuf(previous);
    auto hiddenVisible = strip(captured.str());
    assert(hiddenVisible.find("(hidden; press r to toggle)") != std::string::npos);
    assert(hiddenVisible.find("Graphics Pipe (GFX)") == std::string::npos);

    // --- Unavailable sensors: stale telemetry clears the headline ---
    d.metrics.collectedAtNs = now - kSMUMetricsStaleAfterNs - 1;
    captured.str(""); captured.clear();
    previous = std::cout.rdbuf(captured.rdbuf());
    dashboard({d}, selected, {}, false, false, &histories, {}, 120, 40, now,
              mtop::Graphics::Text, &layout, false, &ui);
    std::cout.rdbuf(previous);
    auto staleVisible = strip(captured.str());
    // Power should be n/a when stale.
    assert(staleVisible.find("Pwr: n/a") != std::string::npos);

    return 0;
}
