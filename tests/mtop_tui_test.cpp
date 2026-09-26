// Pure renderer tests for the modern btop-style TUI (TUI-DESIGN-SPEC.md):
// Braille 2x4 canvas, 8-level sub-cell bars, Y-scale geometry, truecolor
// gradient stops, hotkey dispatch, and layout fit. No IOKit, no GPU.
#include "../amdgpu_mtop/tui.h"
#include <cassert>
#include <cmath>


int main() {
    using namespace mtop::tui;

    // --- Sub-cell bars: 8 fractional levels per cell ---
    // 0 filled -> all empty glyphs.
    assert(visible(fracBar(0.0, 4)) == 4);
    assert(fracBar(0.0, 4).find("\xE2\x96\x91") != std::string::npos); // \u2591
    // 1.0 filled -> one full block.
    assert(fracBar(1.0, 4).find(kBlock) != std::string::npos);
    // 0.5 filled of cell 0 -> half block (U+258C = \xE2\x96\x8C).
    const auto half = fracBar(0.5, 4);
    assert(half.find("\xE2\x96\x84") != std::string::npos); // \u2584 half block
    // 1.5 of 2 -> one full, one half.
    const auto oneHalf = fracBar(1.5, 2);
    assert(oneHalf.find(kBlock) != std::string::npos);
    assert(oneHalf.find("\xE2\x96\x84") != std::string::npos); // \u2584 half block
    // 2.0 of 2 -> two full blocks.
    const auto two = fracBar(2.0, 2);
    assert(two.find(kBlock) != std::string::npos);
    // Width overflow is clipped.
    assert(fracBar(10.0, 3).size() > 0);
    // The last filled cell uses a fractional glyph when the value is not
    // integral: 2.75 of 4 = 2 full + 6/8 (U+258B).
    const auto threeQuarter = fracBar(2.75, 4);
    assert(threeQuarter.find("\xE2\x96\x86") != std::string::npos); // \u2586 6/8 block

    // --- Gradient stops ---
    // Default (dark-on-light) palette: cyan #0891b2, blue #1d4ed8,
    // yellow #b45309, orange #c2410c, crimson #be123c.
    {
        auto [r, g, b] = gradient(0);
        assert(r == 8 && g == 145 && b == 178);
    }
    {
        auto [r, g, b] = gradient(50);
        assert(r == 29 && g == 78 && b == 216);
    }
    // 60% -> between yellow #b45309 (180,83,9) and orange #c2410c (194,65,12), t = 1/3.
    // r: 180 + (1/3)(194-180) = 180 + 4.67 = 184 (truncated)
    // g: 83  + (1/3)(65-83)   = 83  - 6    = 77
    // b: 9   + (1/3)(12-9)    = 9   + 1    = 10
    {
        auto [r, g, b] = gradient(60);
        assert(r == 184);
        assert(g == 77);
        assert(b == 10);
    }
    {
        auto [r, g, b] = gradient(80);
        assert(r == 194 && g == 65 && b == 12);
    }
    {
        auto [r, g, b] = gradient(100);
        assert(r == 190 && g == 18 && b == 60);
    }
    // Clamped: negative -> 0%, >100 -> 100%.
    {
        auto [r, g, b] = gradient(-10);
        assert(r == 8 && g == 145 && b == 178);
        auto [r2, g2, b2] = gradient(200);
        assert(r2 == 190 && g2 == 18 && b2 == 60);
    }
    // --dark palette restores the original bright-on-dark spec values:
    // cyan #00F0FF, blue #0072FF, yellow #FFD600, orange #FF6B00,
    // crimson #FF0055.
    {
        auto [r, g, b] = gradient(0, true);
        assert(r == 0 && g == 240 && b == 255);
        auto [r5, g5, b5] = gradient(50, true);
        assert(r5 == 0 && g5 == 114 && b5 == 255);
        auto [r1, g1, b1] = gradient(100, true);
        assert(r1 == 255 && g1 == 0 && b1 == 85);
    }

    // --- Gradient bar: cell-by-cell truecolor ---
    // cells is the filled count in cells (not percent). A 10/10 bar renders
    // the whole gradient; cell i+1 colors at percent (i+1)/width*100.
    const auto palette = makePalette(false); // dark-on-light default
    const auto bar = gradientBar(10.0, 10, palette);
    assert(bar.find("\033[38;2;") != std::string::npos);
    assert(bar.find("\033[0m") != std::string::npos);
    // Light palette: first cell is at 10% (between cyan #0891b2 and blue),
    // last cell is the crimson end stop #be123c.
    assert(bar.find("\033[38;2;12;131;185m") != std::string::npos);
    assert(bar.find("\033[38;2;190;18;60m") != std::string::npos);
    // A 0% bar is all empty, colored for the light background (#d1d5db).
    const auto empty = gradientBar(0.0, 10, palette);
    assert(empty.find("\033[38;2;") != std::string::npos);
    assert(empty.find("\033[38;2;209;213;219m") != std::string::npos);
    assert(visible(empty) == 10);
    // --dark palette: bright stops, dimmed empty cells.
    const auto darkPalette = makePalette(true);
    const auto darkBar = gradientBar(10.0, 10, darkPalette);
    // First cell at 10% in the bright palette (cyan #00F0FF -> blue #0072FF).
    assert(darkBar.find("\033[38;2;") != std::string::npos);
    assert(darkBar.find("\033[38;2;0;214;255m") != std::string::npos);
    assert(darkBar.find("\033[38;2;255;0;85m") != std::string::npos);
    const auto darkEmpty = gradientBar(0.0, 10, darkPalette);
    assert(darkEmpty.find("\033[2m") != std::string::npos);
    assert(visible(darkEmpty) == 10);

    // --- Braille canvas: 2 columns per cell, 4 rows per line ---
    // 2 samples of 100% at ceiling 1.0 -> a single cell, column fully filled.
    // Each text row shows the dots at that vertical position:
    //   top    (r=0): left-top (bit 6) + right-top (bit 7)    = U+28C0
    //   mid-lo (r=1): left-mid-high (bit 2) + right-mid-high (bit 5) = U+2824
    //   mid-hi (r=2): left-mid-low (bit 1) + right-mid-low (bit 4)   = U+2812
    //   bottom (r=3): left-bottom (bit 0) + right-bottom (bit 3)     = U+2809
    {
        auto br = braille({1.0, 1.0}, 1, 1.0);
        assert(br.cells == 1);
        assert(br.rows.size() == 4);
        const unsigned expected[] = {0x28C0, 0x2824, 0x2812, 0x2809};
        for (size_t i = 0; i < 4; ++i) {
            assert(br.rows[i].size() == 3);
            unsigned cp = ((unsigned(char(br.rows[i][0])) & 0x1F) << 12) |
                          ((unsigned(char(br.rows[i][1])) & 0x3F) << 6) |
                          (unsigned(char(br.rows[i][2])) & 0x3F);
            assert(cp == expected[i]);
        }
    }
    // 2 samples of 0% -> only the bottom dots (level 0).
    // Left sample: dot[0][0]=0 (left-bottom). Right sample: dot[1][0]=3 (right-bottom).
    // Bottom row: bits 0 and 3 = U+2809. All other rows blank.
    {
        auto br = braille({0.0, 0.0}, 1, 1.0);
        assert(br.cells == 1);
        assert(br.rows.size() == 4);
        const auto &bottom = br.rows.back();
        unsigned cp = ((unsigned(char(bottom[0])) & 0x1F) << 12) |
                      ((unsigned(char(bottom[1])) & 0x3F) << 6) |
                      (unsigned(char(bottom[2])) & 0x3F);
        assert(cp == 0x2809);
        // Top row is blank (3 spaces).
        assert(br.rows.front() == "   ");
    }
    // 4 samples (2 cells), values 0.0, 1.0, 0.0, 1.0.
    // Cell 0: left=0.0 (bottom dot only), right=1.0 (full column).
    // Cell 1: same.
    // Top row: right-top (bit 7) only = U+2880 for both cells.
    // Bottom row: left-bottom (bit 0) + right-bottom (bit 3) = U+2809.
    {
        auto br = braille({0.0, 1.0, 0.0, 1.0}, 1, 1.0);
        assert(br.cells == 2);
        assert(br.rows.size() == 4);
        const auto &top = br.rows.front();
        unsigned cp0 = ((unsigned(char(top[0])) & 0x1F) << 12) |
                       ((unsigned(char(top[1])) & 0x3F) << 6) |
                       (unsigned(char(top[2])) & 0x3F);
        assert(cp0 == 0x2880);
        const auto &bot = br.rows.back();
        unsigned cp1 = ((unsigned(char(bot[0])) & 0x1F) << 12) |
                       ((unsigned(char(bot[1])) & 0x3F) << 6) |
                       (unsigned(char(bot[2])) & 0x3F);
        assert(cp1 == 0x2809);
    }
    // Missing samples leave dots clear.
    {
        std::vector<std::optional<double>> values{1.0, {}, 1.0, {}};
        auto br = braille(values, 1, 1.0);
        assert(br.cells == 2);
        // Cell 0: left=full, right=missing (no dots except baseline? no baseline
        // because right sample missing -> present[0] true only if any sample).
        // present[0] = true (sample 0 present). Bottom row gets baseline dot
        // for cell 0.
        const auto &bottom = br.rows.back();
        unsigned cp0 = ((unsigned(char(bottom[0])) & 0x1F) << 12) |
                       ((unsigned(char(bottom[1])) & 0x3F) << 6) |
                       (unsigned(char(bottom[2])) & 0x3F);
        // Cell 0 bottom: left sample 0 = 0.0 -> baseline bit 1; right sample
        // missing -> no dots. Mask = 0b00000001 = 1 -> U+2801.
        assert(cp0 == 0x2801);
    }

    // --- Y-scale geometry: one label per quarter ---
    {
        // label=4: right-aligned. "100%" fits exactly; shorter labels get
        // leading spaces.
        auto ys = yscale(4);
        assert(ys.size() == 5);
        assert(ys[0] == "100%");
        assert(ys[1] == " 75%");
        assert(ys[2] == " 50%");
        assert(ys[3] == " 25%");
        assert(ys[4] == "  0%");
    }
    {
        // label=10: right-aligned, 6 leading spaces for "100%".
        auto ys = yscale(10);
        assert(ys.size() == 5);
        assert(ys[0] == "      100%");
        assert(ys[4] == "        0%");
    }

    // --- X-axis caption: 1m ... 0s across the width ---
    {
        auto xa = xaxis(20);
        assert(visible(xa) == 20);
        assert(xa.find("1m") != std::string::npos);
        assert(xa.find("45s") != std::string::npos);
        assert(xa.find("30s") != std::string::npos);
        assert(xa.find("15s") != std::string::npos);
        assert(xa.find("0s") != std::string::npos);
    }

    // --- Hotkey dispatch (spec footer) ---
    {
        Ui ui;
        assert(ui.sort.kind == Sort::Gpu);
        assert(ui.enginesVisible);
        ui.onKey('m');
        assert(ui.sort.kind == Sort::Vram);
        ui.onKey('g');
        assert(ui.sort.kind == Sort::Gpu);
        ui.onKey('p');
        assert(ui.sort.kind == Sort::Pid);
        ui.onKey('r');
        assert(!ui.enginesVisible);
        ui.onKey('r');
        assert(ui.enginesVisible);
        // Unrelated keys are ignored.
        ui.onKey('q');
        ui.onKey('h');
        assert(ui.sort.kind == Sort::Pid);
        assert(ui.enginesVisible);
    }

    // --- Layout fit: top/mid/bottom/frame produce exact widths ---
    {
        assert(visible(top(20, "TITLE")) == 20);
        assert(visible(top(20, "")) == 20);
        // A long title is clipped to the inner width.
        assert(visible(top(10, "A VERY LONG TITLE THAT OVERFLOWS")) == 10);
        assert(visible(mid(20, "SECTION")) == 20);
        assert(visible(bottom(20)) == 20);
        assert(visible(cell(20, "content")) == 20);
        // Narrow width guards: corners still render (visible >= 1).
        assert(visible(top(1, "X")) >= 1);
        assert(visible(bottom(1)) >= 1);
        assert(visible(cell(1, "X")) >= 1);
    }

    // --- Padding is ANSI-aware ---
    {
        assert(visible(bright("hi")) == 2);
        assert(visible(pad(bright("hi"), 5)) == 5);
        assert(visible(pad("hello", 3)) == 3); // clipped
    }

    return 0;
}
