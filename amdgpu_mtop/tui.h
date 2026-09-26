#pragma once
// Modern btop-style TUI rendering for amdgpu_mtop.
// Pure, unit-testable string builders. Layout follows TUI-DESIGN-SPEC.md:
// rounded borders, Braille 2x4 canvas, 8-level sub-cell bars, truecolor
// gradient palette. No IOKit here; the dashboard in main.cpp feeds values.
//
// All non-ASCII glyphs are written as UTF-8 escape sequences so the header
// stays ASCII-clean under -Werror.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "history.h"
#include "render.h" // clipColumns (ANSI-aware)

namespace mtop::tui {
// --- UTF-8 glyph table ---
// Box-drawing (spec "Box-Drawing & UI Framing").
inline constexpr const char *kTopL    = "\xE2\x94\xAD"; // \u256D
inline constexpr const char *kTopR    = "\xE2\x94\xAE"; // \u256E
inline constexpr const char *kBotL    = "\xE2\x94\xB0"; // \u2570
inline constexpr const char *kBotR    = "\xE2\x94\xAF"; // \u256F
inline constexpr const char *kHoriz   = "\xE2\x94\x80"; // \u2500
inline constexpr const char *kVert    = "\xE2\x94\x82"; // \u2502
inline constexpr const char *kMidL    = "\xE2\x94\x9C"; // \u251C
inline constexpr const char *kMidR    = "\xE2\x94\xA4"; // \u2524
// Blocks.
inline constexpr const char *kBlock   = "\xE2\x96\x88"; // \u2588
inline constexpr const char *kEmpty   = "\xE2\x96\x91"; // \u2591
// 8 fractional eighth-blocks U+258F..U+2588 as UTF-8, 24 bytes.
inline const char *kFrac() {
    // U+2581..U+2588: 1/8 .. 7/8 (U+2588 is the full block, used by kBlock).
    static const char table[] = "\xE2\x96\x81\xE2\x96\x82\xE2\x96\x83\xE2\x96\x84"
                                "\xE2\x96\x85\xE2\x96\x86\xE2\x96\x87\xE2\x96\x88";
    return table;
}
inline void appendFrac(std::string &out, int eighth) {
    out.append(kFrac() + (eighth - 1) * 3, 3);
}

// Spec section "TrueColor Gradient Palettes":
// 0-50% cyan #00F0FF -> blue #0072FF, 50-80% yellow #FFD600 -> orange #FF6B00,
// 80-100% into crimson #FF0055. One stop per percent; 100% is red.
inline std::array<int, 3> gradient(int percent) {
    static const std::array<std::array<int, 3>, 5> a = { { {0, 240, 255}, {0, 114, 255},
                                                     {255, 214, 0}, {255, 107, 0}, {255, 0, 85} } };
    static const int stops[] = {0, 50, 50, 80, 80, 100};
    const int p = std::clamp(percent, 0, 100);
    for (int i = 1; i < 5; ++i)
        if (p <= stops[i]) {
            const double t = (p - stops[i - 1]) / double(std::max(1, stops[i] - stops[i - 1]));
            return {int(a[i - 1][0] + t * (a[i][0] - a[i - 1][0])),
                    int(a[i - 1][1] + t * (a[i][1] - a[i - 1][1])),
                    int(a[i - 1][2] + t * (a[i][2] - a[i - 1][2]))};
        }
    return a.back();
}
inline std::string rgb(int r, int g, int b) {
    return "\033[38;2;" + std::to_string(std::clamp(r, 0, 255)) + ';' +
        std::to_string(std::clamp(g, 0, 255)) + ';' + std::to_string(std::clamp(b, 0, 255)) + 'm';
}
inline std::string dim(std::string_view text) { return "\033[2m" + std::string(text) + "\033[22m"; }
inline std::string bright(std::string_view text) { return "\033[1m" + std::string(text) + "\033[22m"; }
// Spec: thin dimmed borders, 24-bit #505064.
inline const char *kBorder = "\033[38;2;80;80;100m";

// --- sub-cell bar (spec "High-Resolution Sub-Cell Block Characters") ---
// cells is the filled count in cells (fractional), clipped to [0, width].
inline std::string fracBar(double cells, size_t width, std::string_view empty = kEmpty) {
    std::string out;
    if (!width) return out;
    if (!(cells > 0)) {
        std::string out;
        for (size_t i = 0; i < width; ++i) out += empty;
        return out;
    }
    cells = std::min<double>(cells, width);
    size_t full = size_t(cells);
    int eighth = int((cells - double(full)) * 8.0 + 0.5);
    if (eighth == 8) { ++full; eighth = 0; }
    for (size_t i = 0; i < full; ++i) out += kBlock;
    if (full < width) {
        if (eighth) appendFrac(out, eighth);
        out.append(width - full - size_t(eighth > 0), empty[0]);
    }
    return out;
}
// Color a full-width bar cell-by-cell along the truecolor gradient.
inline std::string gradientBar(double cells, size_t width) {
    std::string out;
    if (!width) return out;
    if (!(cells > 0)) {
        std::string out;
        for (size_t i = 0; i < width; ++i) out += kEmpty;
        return out;
    }
    cells = std::min<double>(cells, width);
    size_t full = size_t(cells);
    int eighth = int((cells - double(full)) * 8.0 + 0.5);
    if (eighth == 8) { ++full; eighth = 0; }
    for (size_t i = 0; i < width && i < full; ++i) {
        const auto [r, g, b] = gradient(int(double(i + 1) / double(width) * 100));
        out += rgb(r, g, b) + kBlock + "\033[0m";
    }
    if (full < width) {
        if (eighth) {
            const auto [r, g, b] = gradient(int(double(full + 1) / double(width) * 100));
            out += rgb(r, g, b);
            appendFrac(out, eighth);
            out += "\033[0m";
        }
        for (size_t i = 0; i < width - full - size_t(eighth > 0); ++i) out += kEmpty;
    }
    return out;
}

// --- Braille canvas (spec "Unicode Braille Matrix Canvas") ---
// values: one sample per column; 2 columns per cell, 4 rows per text line.
// Produces `height*4` text rows, each exactly `cells` UTF-8 cells wide.
// y=0 is the bottom row. Missing samples leave dots clear; the bottom text
// row keeps a single baseline dot for the plotted span.
struct Braille {
    std::vector<std::string> rows; // top to bottom, ANSI-free
    size_t cells = 0;
};
inline Braille braille(const std::vector<std::optional<double>> &values, size_t height, double ceiling) {
    const size_t cells = (values.size() + 1) / 2;
    const size_t lines = height * 4;
    Braille out{std::vector<std::string>(lines, std::string(cells * 3, ' ')), cells};
    if (!lines || !cells || !(ceiling > 0)) return out;
    // Braille dot bits: 0=left-bottom, 1=left-mid-low, 2=left-mid-high,
    // 3=right-bottom, 4=right-mid-low, 5=right-mid-high, 6=left-top, 7=right-top.
    constexpr unsigned dot[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};
    // masks[cellRow * cells + cx]: one 8-bit mask per cell row per column.
    std::vector<unsigned> masks(height * cells, 0);
    std::vector<bool> present(cells, false);
    for (size_t x = 0; x < values.size(); ++x) {
        if (!values[x] || !std::isfinite(*values[x])) continue;
        present[x / 2] = true;
        const double v = std::clamp(*values[x], 0.0, ceiling);
        const unsigned level = unsigned(std::lround(v / ceiling * double(lines - 1)));
        // Fill all dots from the bottom (level 0) up to `level` (inclusive).
        for (unsigned fill = 0; fill <= level; ++fill) {
            const size_t cellRow = fill / 4;
            if (cellRow >= height) break;
            const unsigned inCell = fill % 4;
            const size_t cx = x / 2;
            masks[cellRow * cells + cx] |= 1u << dot[x % 2][inCell];
        }
    }
    // Render: text row `r` (0=top) corresponds to cell row `height-1-r/4`
    // and dot position `3-r%4` within that cell.
    for (size_t r = 0; r < lines; ++r) {
        const size_t cellRow = height - 1 - r / 4;
        const unsigned dotPos = 3 - r % 4;
        std::string &row = out.rows[r];
        for (size_t cx = 0; cx < cells; ++cx) {
            const unsigned mask = masks[cellRow * cells + cx];
            // Extract the dot at position `dotPos` for both sides.
            unsigned bit = 0;
            for (unsigned side = 0; side < 2; ++side)
                if (dot[side][dotPos] >= 0 && (mask & (1u << dot[side][dotPos])))
                    bit |= 1u << dot[side][dotPos];
            if (!bit && r == lines - 1 && present[cx]) bit = 1u; // baseline
            if (bit) {
                const unsigned cp = 0x2800 + bit;
                row[cx * 3] = char(0xe0 | (cp >> 12));
                row[cx * 3 + 1] = char(0x80 | ((cp >> 6) & 63));
                row[cx * 3 + 2] = char(0x80 | (cp & 63));
            }
        }
    }
    return out;
}

// --- visible-width helpers (ANSI-aware) ---
inline size_t visible(std::string_view text) {
    size_t at = 0, n = 0;
    while (at < text.size()) {
        if (text[at] == '\033' && at + 1 < text.size() && text[at + 1] == '[') {
            at += 2;
            while (at < text.size() && !(text[at] >= '@' && text[at] <= '~')) ++at;
            if (at < text.size()) ++at;
            continue;
        }
        const unsigned char c = text[at];
        at += c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        ++n;
    }
    return n;
}
// Pad or clip a content cell to a fixed visible width (ignoring ANSI).
inline std::string pad(std::string_view text, size_t width) {
    const size_t n = visible(text);
    if (n >= width) return clipColumns(text, width);
    return std::string(text) + std::string(width - n, ' ');
}
inline std::string cell(size_t width, std::string_view content);

// --- rounded border helpers (spec "Box-Drawing & UI Framing") ---
// Append `n` horizontal glyphs to out.
inline void appendHoriz(std::string &out, size_t n) {
    for (size_t i = 0; i < n; ++i) out += kHoriz;
}
// Top border with an embedded title: "\u256D\u2500 title \u2500\u2500\u256E", exactly `width` cells.
inline std::string top(size_t width, std::string_view title) {
    if (width < 3) return std::string(kTopL) + (width > 1 ? kTopR : "");
    const size_t inner = width - 2;
    std::string out;
    out += kHoriz;
    out += ' ';
    size_t at = 0;
    while (at < title.size() && visible(out) + 1 < inner) {
        const unsigned char c = unsigned(title[at]);
        const size_t bytes = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        if (at + bytes > title.size()) break;
        out.append(title.substr(at, bytes));
        at += bytes;
    }
    appendHoriz(out, inner - visible(out));
    return std::string(kTopL) + out + kTopR;
}
inline std::string bottom(size_t width) {
    if (width < 2) return std::string(kBotL) + kBotR;
    std::string fill;
    for (size_t i = 0; i < width - 2; ++i) fill += kHoriz;
    return std::string(kBotL) + fill + kBotR;
}
// Middle border with an embedded title: "\u251C\u2500 title \u2500\u2500\u2524", exactly `width` cells.
inline std::string mid(size_t width, std::string_view title) {
    if (width < 3) return std::string(kMidL) + (width > 1 ? kMidR : "");
    const size_t inner = width - 2;
    std::string out;
    out += kHoriz;
    out += ' ';
    size_t at = 0;
    while (at < title.size() && visible(out) + 1 < inner) {
        const unsigned char c = unsigned(title[at]);
        const size_t bytes = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        if (at + bytes > title.size()) break;
        out.append(title.substr(at, bytes));
        at += bytes;
    }
    appendHoriz(out, inner - visible(out));
    return std::string(kMidL) + out + kMidR;
}
// One row of the enclosure: "\u2502  content  \u2502", padded/clipped to `width` cells.
inline std::string cell(size_t width, std::string_view content) {
    if (width < 2) return std::string(kVert) + kVert;
    const size_t inner = width - 2;
    return std::string(kVert) + pad(content, inner) + kVert;
}

// Y-axis scale geometry for the core-load chart: one label row per quarter,
// top to bottom: 100% / 75% / 50% / 25% / 0%, right-aligned in `label` cells.
inline std::vector<std::string> yscale(unsigned label) {
    const char *labels[] = {"100%", "75%", "50%", "25%", "0%"};
    std::vector<std::string> out;
    for (const auto *l : labels) {
        const size_t len = std::char_traits<char>::length(l);
        std::string cell(len, ' ');
        for (size_t i = 0; i < len; ++i) cell[i] = l[i];
        if (label <= len) out.push_back(cell);
        else out.push_back(std::string(label - len, ' ') + cell);
    }
    return out;
}
// X-axis caption for a 60 s window across `width` cells:
// "1m" on the left, "0s" on the right, ticks every 15 s.
inline std::string xaxis(size_t width) {
    if (width < 4) return std::string(width, ' ');
    std::string out(width, ' ');
    const char *marks[] = {"1m", "45s", "30s", "15s", "0s"};
    for (size_t i = 0; i < 5; ++i) {
        const size_t len = std::char_traits<char>::length(marks[i]);
        size_t at = i * (width - len) / 4;
        at = std::min(width - len, at);
        for (size_t j = 0; j < len; ++j) out[at + j] = marks[i][j];
    }
    return out;
}

// --- UI state (hotkeys from the spec footer) ---
struct Sort {
    enum Kind { Gpu, Vram, Pid } kind = Gpu;
    const char *label() const { return kind == Gpu ? "GPU" : kind == Vram ? "VRAM" : "PID"; }
    void cycle() { kind = Kind((kind + 1) % 3); }
};
struct Ui {
    Sort sort;
    bool enginesVisible = true;
    void onKey(char key) {
        switch (key) {
            case 'm': sort.kind = Sort::Vram; break;
            case 'g': sort.kind = Sort::Gpu; break;
            case 'p': sort.kind = Sort::Pid; break;
            case 'r': enginesVisible = !enginesVisible; break;
            default: break;
        }
    }
};
} // namespace mtop::tui
