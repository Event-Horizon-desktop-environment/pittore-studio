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
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_map>
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

// Minimal Liang hyphenation over the system dictionaries. The layout only
// needs break positions, so the pattern table is parsed once and queried per
// word with a small cache.
class Hyphenator {
  public:
    static Hyphenator& instance() {
        static Hyphenator h;
        return h;
    }

    std::vector<std::size_t> breakPoints(const std::string& word) const {
        if (word.size() < 3) return {};
        std::string lower;
        lower.reserve(word.size());
        for (char ch : word)
            lower.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(lower);
            if (it != cache_.end()) return it->second;
        }
        std::vector<std::size_t> out;
        if (!patterns_.empty()) {
            const std::string w = "." + lower + ".";
            std::vector<int> score(w.size() + 1, 0);
            for (std::size_t i = 0; i < w.size(); ++i) {
                for (int len = 1; len <= maxLen_ && i + len <= w.size(); ++len) {
                    auto it = patterns_.find(w.substr(i, len));
                    if (it == patterns_.end()) continue;
                    for (std::size_t j = 0; j < it->second.size(); ++j)
                        score[i + j] = std::max(score[i + j], it->second[j]);
                }
            }
            for (std::size_t k = leftMin_;
                 k + rightMin_ <= lower.size(); ++k) {
                if (k < score.size() && (score[k + 1] % 2) == 1)
                    out.push_back(k);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[lower] = out;
        return out;
    }

  private:
    Hyphenator() {
        for (const char* path : {"/usr/share/hyphen/hyph_en_US.dic",
                                 "/usr/share/hyphen/hyph_en_GB.dic"}) {
            std::ifstream in(path);
            if (in) {
                load(in);
                break;
            }
        }
    }

    void load(std::ifstream& in) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (line.rfind("LEFTHYPHENMIN", 0) == 0) {
                leftMin_ = static_cast<std::size_t>(std::atoi(line.c_str() + 13));
                continue;
            }
            if (line.rfind("RIGHTHYPHENMIN", 0) == 0) {
                rightMin_ = static_cast<std::size_t>(std::atoi(line.c_str() + 14));
                continue;
            }
            if (line.rfind("NEXTLEVEL", 0) == 0 ||
                line.rfind("UTF-8", 0) == 0 ||
                line.find(' ') == 0)
                continue;
            std::size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                    ++i;
                std::string letters;
                std::vector<int> values{0};
                while (i < line.size() &&
                       !std::isspace(static_cast<unsigned char>(line[i]))) {
                    const unsigned char ch = static_cast<unsigned char>(line[i++]);
                    if (std::isdigit(ch)) {
                        values.back() = ch - '0';
                    } else {
                        letters.push_back(static_cast<char>(ch));
                        values.push_back(0);
                    }
                }
                if (letters.empty()) continue;
                patterns_[letters] = std::move(values);
                maxLen_ = std::max(maxLen_, static_cast<int>(letters.size()));
            }
        }
    }

    std::unordered_map<std::string, std::vector<int>> patterns_;
    mutable std::unordered_map<std::string, std::vector<std::size_t>> cache_;
    mutable std::mutex mutex_;
    std::size_t leftMin_ = 2, rightMin_ = 3;
    int maxLen_ = 0;
};

Layout layout(const TextSpec& spec, Face& face) {
    if (spec.autoFit && spec.frameHeight > 0.0f && spec.wrapWidth.has_value()) {
        const float usable =
            std::max(1.0f, spec.frameHeight - spec.insetTop - spec.insetBottom);
        float lo = 4.0f, hi = std::max(4.0f, spec.size);
        for (int i = 0; i < 14; ++i) {
            const float mid = (lo + hi) * 0.5f;
            TextSpec trial = spec;
            trial.autoFit = false;
            trial.size = mid;
            const Layout probe = layout(trial, face);
            float bottom = 0.0f;
            for (const auto& span : probe.lines)
                bottom = std::max(bottom, span.top + span.height);
            if (bottom <= usable)
                lo = mid;
            else
                hi = mid;
        }
        TextSpec fit = spec;
        fit.autoFit = false;
        fit.size = lo;
        return layout(fit, face);
    }
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
    const int columnCount = std::max(1, spec.columns);
    const float contentWidth =
        spec.wrapWidth.has_value()
            ? *spec.wrapWidth - spec.insetLeft - spec.insetRight
            : 0.0f;
    const float gutter = std::max(0.0f, spec.columnGutter);
    const float columnWidth =
        columnCount > 1
            ? std::max(1.0f, (contentWidth - gutter * (columnCount - 1)) /
                                 columnCount)
            : contentWidth;

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
        const bool firstLineOfPara = pos == 0 || text[pos - 1] == '\v';
        const float lineIndent =
            spec.indentStart + (firstLineOfPara ? spec.firstLineIndent : 0.0f);
        const float available =
            spec.wrapWidth.has_value()
                ? (spec.noBreak
                       ? std::numeric_limits<float>::max()
                       : columnWidth - lineIndent - spec.indentEnd)
                : std::numeric_limits<float>::max();
        auto measure = [&](const std::string& w) {
            const std::vector<ShapedGlyph> sh = shape(face, w, gs.size, spec);
            return advanceForRange(sh, 0, w.size(), spec.tracking,
                                   charCount(w), gs.hScale);
        };
        // Affinity's paragraph composer balances centred lines instead of
        // filling greedily. Mirror that for the common centred frame text:
        // dynamic programming minimises the raggedness of every line except
        // the last. (Justified/left text keeps the greedy path below.)
        if (spec.align == Align::Center && spec.wrapWidth.has_value() &&
            !spec.noBreak && !spec.hyphenate) {
            struct Tok {
                std::string text;
                std::size_t start = 0;
                float width = 0.0f;
            };
            std::vector<Tok> toks;
            std::size_t ti = 0;
            while (ti < rawLine.size()) {
                const std::size_t sp = rawLine.find_first_of(" \t", ti);
                const std::size_t te =
                    (sp == std::string::npos) ? rawLine.size() : sp + 1;
                Tok tok;
                tok.text = rawLine.substr(ti, te - ti);
                tok.start = pos + ti;
                tok.width = measure(tok.text);
                toks.push_back(std::move(tok));
                ti = te;
            }
            const float avail =
                std::max(1.0f, available);
            const std::size_t nt = toks.size();
            std::vector<double> best(nt + 1, 1e300);
            std::vector<int> prev(nt + 1, -1);
            best[0] = 0.0;
            std::vector<float> prefix(nt + 1, 0.0f);
            for (std::size_t i = 0; i < nt; ++i)
                prefix[i + 1] = prefix[i] + toks[i].width;
            for (std::size_t j = 1; j <= nt; ++j) {
                for (std::size_t i = 0; i < j; ++i) {
                    const float w = prefix[j] - prefix[i];
                    if (w > avail && j > i + 1) continue;
                    const double slack =
                        j == nt ? 0.0 : (avail - std::min(w, avail));
                    const double cost = best[i] + slack * slack;
                    if (cost < best[j]) {
                        best[j] = cost;
                        prev[j] = static_cast<int>(i);
                    }
                }
            }
            if (prev[nt] >= 0) {
                std::vector<std::size_t> bounds;
                int at = static_cast<int>(nt);
                while (at > 0) {
                    bounds.push_back(static_cast<std::size_t>(at));
                    at = prev[static_cast<std::size_t>(at)];
                }
                bounds.push_back(0);
                std::reverse(bounds.begin(), bounds.end());
                for (std::size_t k = 0; k + 1 < bounds.size(); ++k) {
                    const std::size_t first = bounds[k];
                    const std::size_t last = bounds[k + 1];
                    float wsum = 0.0f;
                    std::size_t startByte = toks[first].start;
                    std::size_t endByte = startByte;
                    for (std::size_t q = first; q < last; ++q) {
                        wsum += toks[q].width;
                        endByte = toks[q].start + toks[q].text.size();
                    }
                    std::string line;
                    for (std::size_t q = first; q < last; ++q) line += toks[q].text;
                    lines.push_back(Line{line, wsum, startByte, endByte});
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
            continue;
        }
        std::string current;
        float width = 0.0f;
        std::size_t start = pos;
        std::size_t at = pos;
        std::size_t i = 0;
        while (i < rawLine.size()) {
            const std::size_t sp = rawLine.find_first_of(" \t", i);
            const std::size_t end = (sp == std::string::npos) ? rawLine.size() : sp + 1;
            std::string word = rawLine.substr(i, end - i);
            std::size_t coreLen = word.size();
            while (coreLen > 0 &&
                   (word[coreLen - 1] == ' ' || word[coreLen - 1] == '\t'))
                --coreLen;
            const std::string trailing = word.substr(coreLen);
            if (coreLen == 0) {
                current += word;
                width += measure(word);
                at += word.size();
                i = end;
                continue;
            }
            const std::string core = word.substr(0, coreLen);
            std::size_t cut = 0;
            while (true) {
                const std::string remaining = core.substr(cut);
                const std::string piece = remaining + trailing;
                const float pieceWidth = measure(piece);
                if (available > 1.0f && !current.empty() &&
                    width + pieceWidth > available) {
                    bool split = false;
                    if (spec.hyphenate && remaining.size() > 2) {
                        const std::vector<std::size_t> points =
                            Hyphenator::instance().breakPoints(remaining);
                        for (auto it = points.rbegin(); it != points.rend(); ++it) {
                            const std::string prefix =
                                remaining.substr(0, *it) + "-";
                            const float prefixWidth = measure(prefix);
                            if (width + prefixWidth <= available) {
                                lines.push_back(Line{current + prefix,
                                                     width + prefixWidth, start,
                                                     at + *it});
                                current.clear();
                                width = 0.0f;
                                start = at + *it;
                                at += *it;
                                cut += *it;
                                split = true;
                                break;
                            }
                        }
                    }
                    if (split) continue;
                    lines.push_back(Line{current, width, start, at});
                    current.clear();
                    width = 0.0f;
                    start = at;
                    continue;
                }
                if (available > 1.0f && current.empty() &&
                    pieceWidth > available && spec.hyphenate &&
                    remaining.size() > 2) {
                    const std::vector<std::size_t> points =
                        Hyphenator::instance().breakPoints(remaining);
                    bool split = false;
                    for (auto it = points.rbegin(); it != points.rend(); ++it) {
                        const std::string prefix = remaining.substr(0, *it) + "-";
                        const float prefixWidth = measure(prefix);
                        if (prefixWidth <= available) {
                            lines.push_back(Line{prefix, prefixWidth, start,
                                                 at + *it});
                            start = at + *it;
                            at += *it;
                            cut += *it;
                            split = true;
                            break;
                        }
                    }
                    if (split) continue;
                }
                current += piece;
                width += pieceWidth;
                at += piece.size();
                break;
            }
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

    const float frameTop = std::max(0.0f, spec.spaceBefore) + spec.insetTop;
    const float usableHeight =
        spec.frameHeight > 0.0f
            ? std::max(1.0f, spec.frameHeight - spec.insetTop - spec.insetBottom)
            : std::numeric_limits<float>::max();
    float top = frameTop;
    int column = 0;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const Line& line = lines[li];
        const bool paraStart =
            line.start == 0 || (line.start < text.size() &&
                                text[line.start - 1] == '\v');
        if (paraStart && spec.keepLinesTogether && columnCount > 1) {
            float paraHeight = 0.0f;
            for (std::size_t j = li; j < lines.size(); ++j) {
                paraHeight += lineAdvance;
                if (lines[j].gapAfter > 0.0f) {
                    paraHeight += lines[j].gapAfter +
                                  std::max(0.0f, spec.spaceAfter);
                    break;
                }
            }
            if (top > frameTop + 0.5f &&
                top + paraHeight > frameTop + usableHeight &&
                column + 1 < columnCount) {
                ++column;
                top = frameTop;
            }
        }
        if (columnCount > 1 && column + 1 < columnCount &&
            top + lineAdvance > frameTop + usableHeight) {
            ++column;
            top = frameTop;
        }
        const bool firstLineOfPara =
            line.start == 0 || text[line.start - 1] == '\v';
        const float leftIndent =
            spec.indentStart + (firstLineOfPara ? spec.firstLineIndent : 0.0f);
        const float columnX =
            spec.insetLeft + column * (columnWidth + gutter);
        const float baseline = top + ascent;
        if (li == 0) {
            out.firstBaseline = ascent;
            out.lineAdvance = lineAdvance;
        }
        const float avail =
            spec.wrapWidth.has_value()
                ? columnWidth - leftIndent - spec.indentEnd
                : out.layoutWidth;
        float startX = columnX + leftIndent;
        if (spec.align == Align::Center)
            startX += (avail - line.width) / 2.0f;
        else if (spec.align == Align::Right)
            startX += avail - line.width;
        const bool justify =
            spec.align == Align::Justify &&
            !(line.end >= text.size() ||
              (line.end < text.size() && text[line.end] == '\v'));
        int spaces = 0;
        if (justify)
            for (char ch : line.text)
                if (ch == ' ') ++spaces;
        const float extraPerSpace =
            justify && spaces > 0 ? (avail - line.width) / spaces : 0.0f;
        float justifyExtra = 0.0f;

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
            if (line.start + g.cluster < text.size() &&
                text[line.start + g.cluster] == '\t') {
                const float tab = std::max(1.0f, spec.tabInterval);
                const float relative = x - startX;
                x = startX + (std::floor(relative / tab) + 1.0f) * tab;
                continue;
            }
            p.glyph = g.id;
            p.x = x + justifyExtra + g.xOffset * gs.hScale;
            p.baseline = baseline + g.yOffset * gs.vScale - gs.shift;
            out.glyphs.push_back(p);
            out.chars.push_back(
                TextLayout::CharPos{line.start + g.cluster, x + justifyExtra});
            x += g.xAdvance * gs.hScale + spec.tracking * gs.hScale;
            if (justify && line.start + g.cluster < text.size() &&
                text[line.start + g.cluster] == ' ')
                justifyExtra += extraPerSpace;
        }
        out.lines.push_back(span);
        top += lineAdvance + line.gapAfter +
               (line.gapAfter > 0.0f ? std::max(0.0f, spec.spaceAfter) : 0.0f);
    }
    // List markers are decorations outside the source string: shape them
    // separately so caret offsets and the stored text stay untouched.
    if (spec.listType > 0) {
        int number = spec.listStart;
        for (const LineSpan& span : out.lines) {
            const bool paraStart =
                span.start == 0 ||
                (span.start < text.size() && text[span.start - 1] == '\v');
            if (!paraStart) continue;
            std::string marker = spec.listType == 1
                                     ? std::string("\xE2\x80\xA2")
                                     : std::to_string(number) + ".";
            ++number;
            const std::vector<ShapedGlyph> shaped =
                shape(face, marker + " ", gs.size, spec);
            const float markerWidth =
                advanceForRange(shaped, 0, marker.size() + 1, spec.tracking,
                                charCount(marker) + 1, gs.hScale);
            float mx = span.x - markerWidth;
            if (mx < 0.0f) mx = 0.0f;
            const float baseline = span.top + ascent;
            float x = mx;
            for (const ShapedGlyph& g : shaped) {
                PlacedGlyph p;
                p.glyph = g.id;
                p.x = x + g.xOffset * gs.hScale;
                p.baseline = baseline + g.yOffset * gs.vScale - gs.shift;
                out.glyphs.push_back(p);
                x += g.xAdvance * gs.hScale + spec.tracking * gs.hScale;
            }
        }
    }
    // Frame vertical alignment: shift the laid block inside the usable frame
    // height (the last line's advance is not part of the ink block).
    if (spec.frameHeight > 0.0f && spec.verticalAlign != 0 && !out.lines.empty()) {
        float contentBottom = 0.0f;
        for (const LineSpan& span : out.lines)
            contentBottom = std::max(contentBottom, span.top + span.height);
        const float usable = spec.frameHeight - spec.insetTop - spec.insetBottom;
        const float freeSpace = usable - (contentBottom - spec.insetTop);
        const float shift =
            freeSpace > 0.0f
                ? freeSpace * (spec.verticalAlign == 1 ? 0.5f : 1.0f)
                : 0.0f;
        if (shift > 0.0f) {
            for (LineSpan& span : out.lines) span.top += shift;
            for (PlacedGlyph& glyph : out.glyphs) glyph.baseline += shift;
        }
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
    TextSpec use = spec;
    // Auto-fit: shrink the em size until the laid lines fit the frame height.
    if (spec.autoFit && spec.frameHeight > 0.0f && spec.wrapWidth.has_value()) {
        const float usable =
            std::max(1.0f, spec.frameHeight - spec.insetTop - spec.insetBottom);
        float lo = 4.0f, hi = std::max(4.0f, spec.size * 4.0f);
        for (int i = 0; i < 14; ++i) {
            const float mid = (lo + hi) * 0.5f;
            TextSpec trial = spec;
            trial.autoFit = false;
            trial.size = mid;
            const Layout probe = layout(trial, *face);
            float bottom = 0.0f;
            for (const auto& span : probe.lines)
                bottom = std::max(bottom, span.top + span.height);
            if (bottom <= usable)
                lo = mid;
            else
                hi = mid;
        }
        use.size = lo;
    }
    use.autoFit = false;
    const Layout laid = layout(use, *face);
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
