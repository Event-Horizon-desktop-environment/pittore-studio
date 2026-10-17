#pragma once
// Text layout and rasterization for text layers.
//
// Fonts are discovered through fontconfig, shaped with HarfBuzz and rasterized
// with FreeType, so imported text is re-set the same way the rest of the system
// sets it. The engine lays one family, style and size out left to right with
// kerning, word wrap and alignment, producing an 8-bit coverage mask.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/vector/path.h"

namespace pittore::text {

// Horizontal alignment of wrapped lines.
enum class Align { Left, Center, Right };

// OpenType features the engine can switch on, one bit per tag. They map
// directly onto the Typography section of the Character panel.
enum OTFeature : unsigned {
    OTF_Liga = 1u << 0,   // standard ligatures
    OTF_Calt = 1u << 1,   // contextual alternates
    OTF_Smcp = 1u << 2,   // small caps (lowercase)
    OTF_C2sc = 1u << 3,   // small caps (capitals)
    OTF_Sups = 1u << 4,   // superior figures/letters
    OTF_Subs = 1u << 5,   // inferior figures/letters
    OTF_Frac = 1u << 6,   // fractions
    OTF_Ordn = 1u << 7,   // ordinals
    OTF_Swsh = 1u << 8,   // swash
    OTF_Ss01 = 1u << 9,   // stylistic set 1
    OTF_Ss02 = 1u << 10,  // stylistic set 2
    OTF_Ss03 = 1u << 11,  // stylistic set 3
};

// Everything needed to lay a text layer out.
struct TextSpec {
    std::string text;
    std::string family;
    bool bold = false;
    bool italic = false;
    float size = 48.0f;  // em size, in pixels
    Align align = Align::Left;
    float lineHeight = 1.0f;  // multiple of the face's natural line step
    // Explicit vertical metrics from document formats that record them.
    // When `emLeading` is positive it replaces the natural step entirely
    // (some display faces are set with leading below their own line box,
    // which the hhea step cannot express); `paragraphGap` is added after
    // a paragraph break ('\v' in `text`), not after an ordinary '\n'.
    float emLeading = 0.0f;     // line advance as a fraction of the em size
    float paragraphGap = 0.0f;  // distance between paragraphs, in px
    float tracking = 0.0f;    // extra spacing between characters, in pixels
    std::optional<float> wrapWidth;  // px; nullopt never wraps

    // --- Character-panel attributes ---------------------------------------
    // Decorations: 0 = none, 1 = single, 2 = double. A colour whose alpha is
    // zero inherits the ink colour.
    int underline = 0;
    int strike = 0;
    // Straight RGBA in 0..1. `inkColor` is the fill the glyphs (and inherited
    // decorations) are painted with; `backgroundColor` is a highlight box drawn
    // behind the glyphs.
    std::array<float, 4> inkColor{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> underlineColor{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> strikeColor{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> backgroundColor{0.0f, 0.0f, 0.0f, 0.0f};
    // Post-shaping transforms. Baseline shift is in pixels, positive = up;
    // H/V scale stretch the glyphs and the advances non-uniformly.
    float baselineShift = 0.0f;
    float hScale = 1.0f;
    float vScale = 1.0f;
    // Synthetic super/subscript: -1 subscript, +1 superscript (shrinks the
    // glyphs and shifts the baseline, unlike the real `sups`/`subs` features).
    int superSub = 0;
    // Display transform: uppercase the run without changing the stored string
    // (ASCII only, so caret offsets stay aligned).
    bool allCaps = false;
    // Shaping: pair kerning, and the OpenType features switched on.
    bool kerning = true;
    unsigned otFeatures = 0;
};

// A rasterized text run: an 8-bit coverage mask and where it sits relative to
// the layout origin (y down). Glyphs may sit above the baseline, so `bounds` can
// start negative; `coverage` holds bounds.width()*bounds.height() bytes.
struct TextRaster {
    vector::IRect bounds;
    std::vector<std::uint8_t> coverage;
    // Optional straight-alpha RGBA for the raster (bounds.w*bounds.h*4), set
    // when the run carries a background box or a decoration whose colour is not
    // the ink colour. Callers that find it empty paint `coverage` with their
    // own colour; when present it already includes the ink colour and the
    // decorations composited in order.
    std::vector<std::uint8_t> colorRgba;
    // Baseline of the first line, in the same space as `bounds`.
    float firstBaseline = 0.0f;
    // Baseline-to-baseline distance actually used.
    float lineAdvance = 0.0f;
    // Number of laid lines, including wraps.
    int lineCount = 0;
    // The widest line's advance (pen) width; the pen box spans 0..layoutWidth.
    float layoutWidth = 0.0f;
    // The face's capital height at the requested size, when it declares one.
    std::optional<float> capHeight;

    bool isEmpty() const {
        if (bounds.isEmpty()) return true;
        for (std::uint8_t c : coverage)
            if (c != 0) return false;
        return true;
    }
};

// --- geometry (caret / selection) -----------------------------------------

// One laid-out visual line and the byte range of TextSpec::text it covers.
// Wrapping means a line is not always a source line, so the range is what lets
// a caret or a selection be mapped back onto the string.
struct LineSpan {
    std::size_t start = 0;  // byte offset of the line's first character
    std::size_t end = 0;    // byte offset one past its last character
    float x = 0.0f;         // pen x of the line's first glyph, layout space
    float width = 0.0f;     // advance width of the line
    float top = 0.0f;       // line box top, layout space (y down)
    float height = 0.0f;    // baseline-to-baseline step
};

// A caret in layout space, relative to the raster origin (y down).
struct Caret {
    float x = 0.0f;
    float top = 0.0f;
    float height = 0.0f;
};

// A layout pass with the per-character pens selection needs. Positions share
// the raster's origin, so the same offset maps a caret onto the canvas.
struct TextLayout {
    struct CharPos {
        std::size_t byte = 0;  // offset of the character in TextSpec::text
        float x = 0.0f;        // its pen position
    };
    std::vector<LineSpan> lines;
    std::vector<CharPos> chars;
    float firstBaseline = 0.0f;
    float lineAdvance = 0.0f;
    float layoutWidth = 0.0f;
};

// Every family name fontconfig knows, deduplicated and sorted (original case).
std::vector<std::string> familyNames();

// Lay the spec out without rasterizing: line boxes and per-character pens.
std::optional<TextLayout> layoutText(const TextSpec& spec);

// Caret for a byte offset, clamped to a character boundary.
Caret caretAt(const TextSpec& spec, const TextLayout& layout, std::size_t byte);

// Nearest character boundary to a layout-space point.
std::size_t hitTest(const TextSpec& spec, const TextLayout& layout, float x,
                    float y);

// True when the named family is installed. `name` is matched case-insensitively.
bool hasFamily(const std::string& name);

// The generic sans-serif family on this system.
std::string defaultFamily();

// Lay the spec out and rasterize it. Returns nullopt when no font can be
// loaded. The result is empty (no ink) for empty or whitespace-only text.
std::optional<TextRaster> rasterize(const TextSpec& spec);

}  // namespace pittore::text
