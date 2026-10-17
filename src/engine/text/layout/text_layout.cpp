#include "engine/text/text_engine.h"

#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>

#include <fontconfig/fontconfig.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/text/shared/text_internal.h"

namespace pittore::text {
namespace detail {

std::string displayText(const TextSpec& spec) {
    if (!spec.allCaps) return spec.text;
    std::string s = spec.text;
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

GlyphStyle glyphStyle(const TextSpec& spec) {
    GlyphStyle gs;
    gs.size = std::max(0.01f, spec.size);
    gs.hScale = std::clamp(spec.hScale, 0.05f, 20.0f);
    gs.vScale = std::clamp(spec.vScale, 0.05f, 20.0f);
    gs.shift = spec.baselineShift;
    if (spec.superSub > 0) {
        gs.shift += spec.size * 0.33f;
        gs.size *= 0.65f;
    } else if (spec.superSub < 0) {
        gs.shift -= spec.size * 0.20f;
        gs.size *= 0.65f;
    }
    return gs;
}

std::size_t charCount(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

float advanceForRange(const std::vector<ShapedGlyph>& shaped, std::size_t from,
                      std::size_t len, float tracking, std::size_t chars,
                      float hScale) {
    float w = tracking * static_cast<float>(chars);
    for (const ShapedGlyph& g : shaped)
        if (g.cluster >= from && g.cluster < from + len) w += g.xAdvance;
    return w * hScale;
}

Layout layout(const TextSpec& spec, Face& face) {
    Layout out;
    const GlyphStyle gs = glyphStyle(spec);
    setSize(face, gs.size);
    // Use the face's declared metrics (hhea ascender/descender/line gap) rather
    // than FreeType's pixel-rounded size metrics, so the line box is the exact
    // one the document was set with, not a whole-pixel approximation. The line
    // box stays at the nominal size even when super/subscript shrinks the
    // glyphs, matching the .af renderer (the run does not change the leading).
    const float upem =
        face.ft->units_per_EM > 0 ? static_cast<float>(face.ft->units_per_EM) : 1000.0f;
    const float ascent = static_cast<float>(face.ft->ascender) * spec.size / upem;
    const float lineStep = static_cast<float>(face.ft->height) * spec.size / upem;
    // A document that records its leading explicitly overrides the face's
    // natural line step; paragraph breaks ('\v') additionally add the
    // recorded spacing between paragraphs.
    const float lineAdvance =
        spec.emLeading > 0.0f
            ? spec.emLeading * spec.size
            : lineStep * std::max(0.1f, spec.lineHeight);
    const std::string text = displayText(spec);

    struct Line {
        std::string text;
        float width = 0.0f;
        std::size_t start = 0;  // byte offset of the line's first character
        std::size_t end = 0;    // byte offset one past its last character
        float gapAfter = 0.0f;  // paragraph spacing before the next line
    };
    std::vector<Line> lines;

    std::size_t pos = 0;
    while (true) {
        const std::size_t nl = text.find_first_of("\n\v", pos);
        const std::string rawLine = text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);

        // Wrap on word boundaries; a single over-long word is left to overflow
        // rather than being broken mid-word.
        const std::vector<ShapedGlyph> measured = shape(face, rawLine, gs.size, spec);
        std::string current;
        float width = 0.0f;
        std::size_t start = pos;
        std::size_t at = pos;
        std::size_t i = 0;
        while (i < rawLine.size()) {
            const std::size_t sp = rawLine.find(' ', i);
            const std::size_t end = (sp == std::string::npos) ? rawLine.size() : sp + 1;
            const std::string word = rawLine.substr(i, end - i);
            const std::size_t chars = charCount(word);
            float wordWidth =
                advanceForRange(measured, i, word.size(), spec.tracking, chars, gs.hScale);
            if (spec.wrapWidth.has_value() && !current.empty() &&
                width + wordWidth > *spec.wrapWidth) {
                lines.push_back(Line{current, width, start, at});
                current.clear();
                width = 0.0f;
                start = at;
                // Re-measure with no kerning context: the word now starts a
                // line, so there is no preceding glyph to kern against.
                const std::vector<ShapedGlyph> alone = shape(face, word, gs.size, spec);
                wordWidth =
                    advanceForRange(alone, 0, word.size(), spec.tracking, chars, gs.hScale);
            }
            current += word;
            width += wordWidth;
            at += word.size();
            i = end;
        }
        lines.push_back(Line{current, width, start, at});
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    // A line whose next byte is the paragraph separator ends a paragraph
    // and carries the recorded spacing before the next one.
    for (Line& l : lines)
        if (l.end < text.size() && text[l.end] == '\v' && spec.paragraphGap > 0.0f)
            l.gapAfter = spec.paragraphGap;

    out.layoutWidth = 0.0f;
    for (const Line& l : lines) out.layoutWidth = std::max(out.layoutWidth, l.width);

    float top = 0.0f;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const Line& line = lines[li];
        const float baseline = top + ascent;
        if (li == 0) {
            out.firstBaseline = ascent;
            out.lineAdvance = lineAdvance;
        }
        float startX = 0.0f;
        if (spec.align == Align::Center)
            startX = (out.layoutWidth - line.width) / 2.0f;
        else if (spec.align == Align::Right)
            startX = out.layoutWidth - line.width;

        LineSpan span;
        span.start = line.start;
        span.end = line.end;
        span.x = startX;
        span.width = line.width;
        span.top = top;
        span.height = lineAdvance;

        const std::vector<ShapedGlyph> shaped = shape(face, line.text, gs.size, spec);
        float x = startX;
        for (const ShapedGlyph& g : shaped) {
            PlacedGlyph p;
            p.glyph = g.id;
            p.x = x + g.xOffset * gs.hScale;
            p.baseline = baseline + g.yOffset * gs.vScale - gs.shift;
            out.glyphs.push_back(p);
            out.chars.push_back(TextLayout::CharPos{line.start + g.cluster, x});
            x += g.xAdvance * gs.hScale + spec.tracking * gs.hScale;
        }
        out.lines.push_back(span);
        top += lineAdvance + line.gapAfter;
    }
    return out;
}

std::size_t clampBoundary(const std::string& s, std::size_t byte) {
    if (byte > s.size()) byte = s.size();
    while (byte > 0 && (static_cast<unsigned char>(s[byte]) & 0xC0) == 0x80) --byte;
    return byte;
}

}  // namespace detail
}  // namespace pittore::text

namespace pittore::text {

std::optional<TextLayout> layoutText(const TextSpec& spec) {
    using namespace detail;
    std::shared_ptr<Face> face = cachedFace(spec.family, spec.bold, spec.italic);
    if (!face) return std::nullopt;
    const Layout laid = layout(spec, *face);
    TextLayout out;
    out.lines = laid.lines;
    out.chars = laid.chars;
    out.firstBaseline = laid.firstBaseline;
    out.lineAdvance = laid.lineAdvance;
    out.layoutWidth = laid.layoutWidth;
    return out;
}

Caret caretAt(const TextSpec& spec, const TextLayout& layout, std::size_t byte) {
    using namespace detail;
    const std::size_t at = clampBoundary(spec.text, byte);
    const LineSpan* span = nullptr;
    for (auto it = layout.lines.rbegin(); it != layout.lines.rend(); ++it) {
        if (it->start <= at) {
            span = &*it;
            break;
        }
    }
    if (!span && !layout.lines.empty()) span = &layout.lines.front();
    static const LineSpan kEmpty;
    const LineSpan& s = span ? *span : kEmpty;
    const std::size_t upto = std::max(s.start, std::min(at, s.end));
    float x = s.x + s.width;
    if (upto < s.end) {
        for (const TextLayout::CharPos& c : layout.chars) {
            if (c.byte == upto) {
                x = c.x;
                break;
            }
        }
    }
    Caret c;
    c.x = x;
    c.top = s.top;
    c.height = s.height > 0.0f ? s.height : spec.size;
    return c;
}

std::size_t hitTest(const TextSpec& spec, const TextLayout& layout, float x,
                    float y) {
    using namespace detail;
    (void)spec;
    if (layout.lines.empty()) return 0;
    // Pick the line whose box is vertically nearest, then the character
    // boundary on it that is horizontally nearest.
    const LineSpan* best = &layout.lines.front();
    float bestDist = -1.0f;
    for (const LineSpan& l : layout.lines) {
        const float d = y < l.top ? l.top - y
                                  : (y > l.top + l.height ? y - (l.top + l.height) : 0.0f);
        if (bestDist < 0.0f || d < bestDist) {
            bestDist = d;
            best = &l;
        }
    }
    std::size_t byte = best->end;
    float bestX = best->x + best->width;
    for (const TextLayout::CharPos& c : layout.chars) {
        if (c.byte < best->start || c.byte >= best->end) continue;
        if (std::fabs(x - c.x) < std::fabs(x - bestX)) {
            bestX = c.x;
            byte = c.byte;
        }
    }
    return byte;
}

}  // namespace pittore::text
