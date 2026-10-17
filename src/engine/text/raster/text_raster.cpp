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

bool renderGlyph(Face& f, unsigned gid, GlyphBitmap& out) {
    if (FT_Load_Glyph(f.ft, gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return false;
    if (FT_Render_Glyph(f.ft->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;
    const FT_Bitmap& bm = f.ft->glyph->bitmap;
    out.width = static_cast<int>(bm.width);
    out.height = static_cast<int>(bm.rows);
    out.left = f.ft->glyph->bitmap_left;
    out.top = f.ft->glyph->bitmap_top;
    out.data.assign(static_cast<std::size_t>(out.width) * out.height, 0);
    for (int y = 0; y < out.height; ++y) {
        const unsigned char* src = bm.buffer + static_cast<std::ptrdiff_t>(y) * bm.pitch;
        std::memcpy(&out.data[static_cast<std::size_t>(y) * out.width], src,
                    static_cast<std::size_t>(out.width));
    }
    return true;
}

}  // namespace detail
}  // namespace pittore::text

namespace pittore::text {

std::optional<TextRaster> rasterize(const TextSpec& spec) {
    using namespace detail;
    std::shared_ptr<Face> face = cachedFace(spec.family, spec.bold, spec.italic);
    if (!face) return std::nullopt;

    auto emptyRaster = [&] {
        TextRaster r;
        if (face->capRatio) r.capHeight = *face->capRatio * spec.size;
        return r;
    };
    if (spec.text.empty() || spec.size <= 0.0f) return emptyRaster();

    const GlyphStyle gs = glyphStyle(spec);
    const Layout laid = layout(spec, *face);
    TextRaster raster;
    if (face->capRatio) raster.capHeight = *face->capRatio * spec.size;
    raster.firstBaseline = laid.firstBaseline;
    raster.lineAdvance = laid.lineAdvance;
    raster.lineCount = static_cast<int>(laid.lines.size());
    raster.layoutWidth = laid.layoutWidth;

    struct Done {
        vector::IRect rect;
        GlyphBitmap bitmap;
    };
    std::vector<Done> done;
    vector::IRect bounds{};
    bool haveBounds = false;

    setSize(*face, gs.size);
    const bool scaled = std::abs(gs.hScale - 1.0f) > 1e-4f ||
                        std::abs(gs.vScale - 1.0f) > 1e-4f;
    if (scaled) {
        FT_Matrix m;
        m.xx = static_cast<FT_Fixed>(gs.hScale * 65536.0f);
        m.xy = 0;
        m.yx = 0;
        m.yy = static_cast<FT_Fixed>(gs.vScale * 65536.0f);
        FT_Set_Transform(face->ft, &m, nullptr);
    }
    for (const PlacedGlyph& g : laid.glyphs) {
        GlyphBitmap bm;
        if (!renderGlyph(*face, g.glyph, bm)) continue;
        if (bm.width == 0 || bm.height == 0) continue;
        const int left = static_cast<int>(std::floor(g.x + bm.left));
        const int top = static_cast<int>(std::floor(g.baseline - bm.top));
        const vector::IRect rect = vector::IRect::fromXYWH(left, top, bm.width, bm.height);
        if (!haveBounds) {
            bounds = rect;
            haveBounds = true;
        } else {
            bounds = bounds.unionWith(rect);
        }
        done.push_back(Done{rect, std::move(bm)});
    }
    if (scaled) FT_Set_Transform(face->ft, nullptr, nullptr);
    if (!haveBounds || bounds.isEmpty()) return emptyRaster();

    // Decorations and the highlight box can reach past the glyph ink, so size
    // the raster to hold them too — but only when they are painted, so a plain
    // run keeps exactly its glyph bounds (the imported-layer probes depend on
    // that).
    const bool wantBackground = spec.backgroundColor[3] > 0.0f;
    const bool wantUnderline = spec.underline > 0;
    const bool wantStrike = spec.strike > 0;
    const bool wantColor = wantBackground || wantUnderline || wantStrike;

    const float upemF =
        face->ft->units_per_EM > 0 ? static_cast<float>(face->ft->units_per_EM) : 1000.0f;
    const float metricScale = gs.size / upemF;
    const float undoThick =
        std::max(1.0f, std::fabs(static_cast<float>(face->ft->underline_thickness)) * metricScale);
    const float undoDy =
        -static_cast<float>(face->ft->underline_position) * metricScale;
    float strikeThick = -1.0f, strikeDy = 0.0f;
    if (const TT_OS2* os2 =
            static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face->ft, ft_sfnt_os2))) {
        if (os2->yStrikeoutSize != 0) {
            strikeThick = std::max(1.0f, std::fabs(static_cast<float>(os2->yStrikeoutSize)) * metricScale);
            strikeDy = -static_cast<float>(os2->yStrikeoutPosition) * metricScale;
        }
    }
    if (strikeThick < 0.0f) {
        strikeThick = std::max(1.0f, 0.05f * gs.size);
        strikeDy = -0.28f * gs.size;
    }
    auto lineBaseline = [&](std::size_t li) {
        return laid.firstBaseline + static_cast<float>(li) * laid.lineAdvance - gs.shift;
    };

    if (wantColor) {
        vector::IRect extra{};
        bool haveExtra = false;
        auto addRect = [&](int x0, int y0, int x1, int y1) {
            const vector::IRect r = vector::IRect::fromXYWH(x0, y0, x1 - x0, y1 - y0);
            extra = haveExtra ? extra.unionWith(r) : r;
            haveExtra = true;
        };
        for (std::size_t li = 0; li < laid.lines.size(); ++li) {
            const LineSpan& s = laid.lines[li];
            const int x0 = static_cast<int>(std::floor(s.x));
            const int x1 = static_cast<int>(std::ceil(s.x + s.width));
            if (wantBackground)
                addRect(x0, static_cast<int>(std::floor(s.top)), x1,
                        static_cast<int>(std::ceil(s.top + s.height)));
            const float base = lineBaseline(li);
            if (wantUnderline) {
                const float y = base + undoDy;
                addRect(x0, static_cast<int>(std::floor(y - undoThick * 0.5f)), x1,
                        static_cast<int>(std::ceil(y + undoThick * 0.5f +
                                                   (spec.underline == 2 ? undoThick * 2.0f : 0.0f))));
            }
            if (wantStrike) {
                const float y = base + strikeDy;
                addRect(x0, static_cast<int>(std::floor(y - strikeThick * 0.5f)), x1,
                        static_cast<int>(std::ceil(y + strikeThick * 0.5f +
                                                   (spec.strike == 2 ? strikeThick * 2.0f : 0.0f))));
            }
        }
        if (haveExtra) bounds = bounds.unionWith(extra);
    }

    raster.bounds = bounds;
    const int bw = bounds.width(), bh = bounds.height();
    const int bx0 = bounds.left, by0 = bounds.top;
    const std::size_t n = static_cast<std::size_t>(bw) * bh;
    raster.coverage.assign(n, 0);
    if (wantColor) raster.colorRgba.assign(n * 4, 0);

    // Source-over compositing into the straight-alpha colour buffer.
    auto composite = [&](int x, int y, float sr, float sg, float sb, float sa) {
        if (sa <= 0.0f) return;
        const int rx = x - bx0, ry = y - by0;
        if (rx < 0 || rx >= bw || ry < 0 || ry >= bh) return;
        const std::size_t i =
            (static_cast<std::size_t>(ry) * bw + rx) * 4;
        const float da = raster.colorRgba[i + 3] / 255.0f;
        const float oa = sa + da * (1.0f - sa);
        if (oa <= 0.0f) return;
        auto ch = [&](int c, float sc) {
            const float dc = raster.colorRgba[i + c] / 255.0f;
            const float oc = (sc * sa + dc * da * (1.0f - sa)) / oa;
            raster.colorRgba[i + c] = static_cast<std::uint8_t>(
                std::lround(std::clamp(oc, 0.0f, 1.0f) * 255.0f));
        };
        ch(0, sr);
        ch(1, sg);
        ch(2, sb);
        raster.colorRgba[i + 3] = static_cast<std::uint8_t>(
            std::lround(std::clamp(oa, 0.0f, 1.0f) * 255.0f));
    };

    // Highlight boxes behind the glyphs.
    if (wantBackground) {
        for (std::size_t li = 0; li < laid.lines.size(); ++li) {
            const LineSpan& s = laid.lines[li];
            const int x0 = static_cast<int>(std::floor(s.x));
            const int x1 = static_cast<int>(std::ceil(s.x + s.width));
            const int y0 = static_cast<int>(std::floor(s.top));
            const int y1 = static_cast<int>(std::ceil(s.top + s.height));
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    composite(x, y, spec.backgroundColor[0], spec.backgroundColor[1],
                              spec.backgroundColor[2], spec.backgroundColor[3]);
        }
    }

    for (const Done& d : done) {
        for (int gy = 0; gy < d.rect.height(); ++gy) {
            for (int gx = 0; gx < d.rect.width(); ++gx) {
                const std::uint8_t v =
                    d.bitmap.data[static_cast<std::size_t>(gy) * d.rect.width() + gx];
                if (v == 0) continue;
                const int x = d.rect.left + gx;
                const int y = d.rect.top + gy;
                const int rx = x - bx0, ry = y - by0;
                if (rx < 0 || rx >= bw || ry < 0 || ry >= bh) continue;
                std::uint8_t& slot = raster.coverage[static_cast<std::size_t>(ry) * bw + rx];
                slot = std::max(slot, v);
                if (wantColor)
                    composite(x, y, spec.inkColor[0], spec.inkColor[1],
                              spec.inkColor[2], spec.inkColor[3] * v / 255.0f);
            }
        }
    }

    auto paintBar = [&](int x0, int x1, float yCenter, float thickness, const float* c) {
        const int yTop = static_cast<int>(std::floor(yCenter - thickness * 0.5f));
        const int yBot = static_cast<int>(std::ceil(yCenter + thickness * 0.5f));
        for (int y = yTop; y < yBot; ++y)
            for (int x = x0; x < x1; ++x) composite(x, y, c[0], c[1], c[2], c[3]);
    };
    const float* uColor =
        spec.underlineColor[3] > 0.0f ? spec.underlineColor.data() : spec.inkColor.data();
    const float* sColor =
        spec.strikeColor[3] > 0.0f ? spec.strikeColor.data() : spec.inkColor.data();
    for (std::size_t li = 0; li < laid.lines.size(); ++li) {
        const LineSpan& s = laid.lines[li];
        const int x0 = static_cast<int>(std::floor(s.x));
        const int x1 = static_cast<int>(std::ceil(s.x + s.width));
        const float base = lineBaseline(li);
        if (wantUnderline) {
            paintBar(x0, x1, base + undoDy, undoThick, uColor);
            if (spec.underline == 2)
                paintBar(x0, x1, base + undoDy + undoThick * 2.0f, undoThick, uColor);
        }
        if (wantStrike) {
            paintBar(x0, x1, base + strikeDy, strikeThick, sColor);
            if (spec.strike == 2)
                paintBar(x0, x1, base + strikeDy + strikeThick * 2.0f, strikeThick, sColor);
        }
    }
    return raster;
}

}  // namespace pittore::text
