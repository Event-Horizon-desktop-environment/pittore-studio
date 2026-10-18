#include "engine/compute/layer_mask.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/compute/adjust.h"
#include "engine/compute/paint.h"
#include "engine/core/parallel.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

float mask_coverage_host(const RGBAf* mask, std::uint32_t msw,
                         std::uint32_t msh, double docX, double docY,
                         double mox, double moy, double msx, double msy) {
    if (!mask || msw == 0 || msh == 0 || msx <= 0.0 || msy <= 0.0) return 1.0f;
    const RGBAf s =
        sample_placed_host(mask, msw, msh, docX, docY, mox, moy, msx, msy);
    return std::clamp(s.r, 0.0f, 1.0f);
}

void mask_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, float cx,
                   float cy, float radius, float hardness, float opacity,
                   float value, const SelectionMask* selection) {
    if (!dst || radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;

    const float hard = std::clamp(hardness, 0.0f, 1.0f);
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    const float target = std::clamp(value, 0.0f, 1.0f);

    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));
    const float inv_r = 1.0f / radius;
    const float soft_span = std::max(1.0f - hard, 1e-4f);

    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        std::size_t idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x0);
        for (int x = x0; x <= x1; ++x, ++idx) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float t = std::sqrt(dx * dx + dy * dy) * inv_r;
            if (t >= 1.0f) continue;

            float cov = 1.0f;
            if (t > 1.0f - soft_span) cov = (1.0f - t) / soft_span;
            if (selection) cov *= selection->coverage(x, y);
            const float a = cov * op;
            if (a <= 0.0f) continue;

            RGBAf& d = dst[idx];
            const float next = d.r + (target - d.r) * a;
            d.r = d.g = d.b = std::clamp(next, 0.0f, 1.0f);
            d.a = 1.0f;
        }
    }
}

namespace {

// Direct source texel reads used by the GPU batched kernels. The sampler
// would return exactly these texels, so the host reference takes the same
// shortcut instead of round-tripping integer placements through taps.
RGBAf sample_source_or_direct(const HostPlacedLayer& l, const RGBAf* src,
                              std::uint32_t w, std::size_t idx, int x, int y) {
    if (l.isFill) return l.fillColor;  // constant source, no geometry
    if (l.sx == 1.0 && l.sy == 1.0 && l.ox == 0.0 && l.oy == 0.0 &&
        l.sw == w && l.sh >= l.y1) {
        return src[idx];
    }
    if (l.sx == 1.0 && l.sy == 1.0 &&
        l.ox == static_cast<double>(static_cast<int>(l.ox)) &&
        l.oy == static_cast<double>(static_cast<int>(l.oy))) {
        const int sx = x - static_cast<int>(l.ox);
        const int sy = y - static_cast<int>(l.oy);
        if (sx >= 0 && sy >= 0 && static_cast<std::uint32_t>(sx) < l.sw &&
            static_cast<std::uint32_t>(sy) < l.sh) {
            return src[static_cast<std::size_t>(sy) * l.sw +
                       static_cast<std::size_t>(sx)];
        }
        return RGBAf{0, 0, 0, 0};
    }
    return sample_placed_host(src, l.sw, l.sh, static_cast<double>(x),
                              static_cast<double>(y), l.ox, l.oy, l.sx, l.sy);
}

// Post-mask/fold alpha of one layer at a document pixel: the coverage a
// clipped layer above it uses. Sampling the base on demand (instead of
// carrying running coverage) keeps each clip group exact even where the base
// does not cover the pixel.
float base_coverage_at(const HostPlacedLayer& b, std::uint32_t w,
                       std::size_t idx, int x, int y) {
    if (!b.isFill && (!b.src || b.sw == 0 || b.sh == 0)) return 0.0f;
    RGBAf s = sample_source_or_direct(b, b.src, w, idx, x, y);
    if (b.mask) {
        s.a *= mask_coverage_host(b.mask, b.msw, b.msh,
                                  static_cast<double>(x),
                                  static_cast<double>(y), b.mox, b.moy,
                                  b.msx, b.msy);
    }
    s.a *= b.fold;
    return s.a;
}

struct RowCell {
    int x0 = 0, x1 = 0;
    std::size_t layer = 0;
};

}  // namespace

void composite_many_host(RGBAf* bottom, std::uint32_t w, std::uint32_t h,
                         std::uint32_t rx0, std::uint32_t ry0,
                         std::uint32_t rx1, std::uint32_t ry1,
                         const HostPlacedLayer* layers, std::size_t count) {
    if (!bottom || w == 0 || h == 0 || !layers || count == 0) return;
    rx0 = std::min(rx0, w);
    rx1 = std::min(rx1, w);
    ry0 = std::min(ry0, h);
    ry1 = std::min(ry1, h);
    if (rx0 >= rx1 || ry0 >= ry1) return;

    // Reused across calls (cleared, never freed in steady state): the cell
    // tables are rebuilt per call but the storage persists. thread_local
    // because composite_many_host is re-entered across worker threads only
    // through parallel_rows below, which never calls back in here.
    thread_local std::vector<HostPlacedLayer> tl_kept;
    thread_local std::vector<int> tl_per;
    thread_local std::vector<int> tl_rowStart;
    thread_local std::vector<int> tl_curs;
    thread_local std::vector<RowCell> tl_cells;
    std::vector<HostPlacedLayer>& kept = tl_kept;
    std::vector<int>& per = tl_per;
    std::vector<int>& rowStart = tl_rowStart;
    std::vector<int>& curs = tl_curs;
    std::vector<RowCell>& cells = tl_cells;
    kept.clear();
    kept.reserve(count);
    per.assign(static_cast<std::size_t>(h) + 1, 0);
    for (std::size_t i = 0; i < count; ++i) {
        HostPlacedLayer l = layers[i];
        if (!l.isAdjustment && !l.isFill &&
            (!l.src || l.sw == 0 || l.sh == 0)) continue;
        l.x0 = std::min(l.x0, w);
        l.x1 = std::min(l.x1, w);
        l.y0 = std::min(l.y0, h);
        l.y1 = std::min(l.y1, h);
        if (l.x0 >= l.x1 || l.y0 >= l.y1) continue;
        if (l.mask && (l.msw == 0 || l.msh == 0)) l.mask = nullptr;
        kept.push_back(l);
        const std::size_t index = kept.size() - 1;
        for (std::uint32_t yy = l.y0; yy < l.y1; ++yy) {
            ++per[yy];
            (void)index;
        }
    }
    if (kept.empty()) return;
    // A clipped layer's base must be a valid non-clipped layer; anything else
    // degrades to unclipped rather than sampling out of bounds. clipBase == -1
    // is the valid "base contributes nothing here" case and is preserved.
    for (auto& l : kept) {
        if (!l.clipped) {
            l.clipBase = -1;
            continue;
        }
        const bool validBase =
            l.clipBase >= 0 &&
            static_cast<std::size_t>(l.clipBase) < kept.size() &&
            !kept[static_cast<std::size_t>(l.clipBase)].clipped &&
            !kept[static_cast<std::size_t>(l.clipBase)].isAdjustment;
        if (l.clipBase < -1 || (l.clipBase >= 0 && !validBase))
            l.clipped = false;
        if (!l.clipped) l.clipBase = -1;
    }

    rowStart.resize(static_cast<std::size_t>(h) + 1);
    rowStart[0] = 0;
    for (std::uint32_t yy = 1; yy <= h; ++yy)
        rowStart[yy] = rowStart[yy - 1] + per[yy - 1];
    curs = rowStart;
    cells.clear();
    cells.resize(static_cast<std::size_t>(rowStart[h]));
    for (std::size_t j = 0; j < kept.size(); ++j) {
        const HostPlacedLayer& l = kept[j];
        const RowCell c{static_cast<int>(l.x0), static_cast<int>(l.x1), j};
        for (std::uint32_t yy = l.y0; yy < l.y1; ++yy)
            cells[static_cast<std::size_t>(curs[yy]++)] = c;
    }

    // Row-parallel: every row writes only its own accumulator pixels while
    // reading the frozen cell/layer tables, so worker threads never share
    // mutable state (bit-identical to the serial walk).
    pittore::core::parallel_rows(
        ry1 - ry0, [&](std::uint32_t lo, std::uint32_t hi) {
            for (std::uint32_t y = ry0 + lo; y < ry0 + hi; ++y) {
                const int rb = rowStart[y];
                const int n = rowStart[y + 1] - rb;
                for (std::uint32_t x = rx0; x < rx1; ++x) {
                    const std::size_t idx =
                        static_cast<std::size_t>(y) * w + x;
                    RGBAf& acc = bottom[idx];
                    for (int k = 0; k < n; ++k) {
                        const RowCell& c =
                            cells[static_cast<std::size_t>(rb + k)];
                        if (static_cast<int>(x) < c.x0 ||
                            static_cast<int>(x) >= c.x1)
                            continue;
                        const HostPlacedLayer& l = kept[c.layer];
                        RGBAf s;
                        if (l.isAdjustment) {
                            // The adjustment reads the composite-so-far: run
                            // the kind over the accumulator colour, keeping
                            // its alpha for the usual mask/fold/clip
                            // pipeline below.
                            float ar, ag, ab;
                            adjust::apply(
                                static_cast<AdjustmentKind>(l.adjKind), l.adjP,
                                l.adjAux, acc.r, acc.g, acc.b, ar, ag, ab);
                            s = RGBAf{ar, ag, ab, acc.a};
                        } else {
                            s = sample_source_or_direct(
                                l, l.src, w, idx, static_cast<int>(x),
                                static_cast<int>(y));
                        }
                        if (l.mask) {
                            s.a *= mask_coverage_host(
                                l.mask, l.msw, l.msh, static_cast<double>(x),
                                static_cast<double>(y), l.mox, l.moy, l.msx,
                                l.msy);
                        }
                        s.a *= l.fold;
                        if (l.clipped && l.clipBase >= 0) {
                            const HostPlacedLayer& base =
                                kept[static_cast<std::size_t>(l.clipBase)];
                            s.a *= base_coverage_at(
                                base, w, idx, static_cast<int>(x),
                                static_cast<int>(y));
                        } else if (l.clipped) {
                            s.a = 0.0f;
                        }
                        blend::pixel(l.mode, s.r, s.g, s.b, s.a, acc.r, acc.g,
                                     acc.b, acc.a, static_cast<int>(x),
                                     static_cast<int>(y), acc.r, acc.g, acc.b,
                                     acc.a);
                    }
                }
            }
        });
}

}  // namespace pittore::compute
