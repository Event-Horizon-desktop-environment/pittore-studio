#include "engine/compute/brushes/replace/replace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "engine/compute/blend.h"
#include "engine/core/pixel.h"
#include "engine/color/icc_convert.h"

namespace pittore::compute {

namespace {

// Largest per-channel RGB difference between two straight-alpha colours
// (alpha excluded: transparent texels carry no meaningful colour). The same
// metric flood_fill_host uses, so Tolerance behaves identically across the
// Bucket, the Wand and this brush.
inline float replace_distance(const RGBAf& a, const RGBAf& b) {
    return std::max(std::fabs(a.r - b.r),
                    std::max(std::fabs(a.g - b.g), std::fabs(a.b - b.b)));
}

}  // namespace

bool replace_color_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h,
                            float cx, float cy, float radius, float hardness,
                            const RGBAf* targets, int targetCount,
                            const RGBAf& replacement, float tolerance,
                            ReplaceMode mode, int limits, bool antialias,
                            float harmony, int* bbox,
                            const SelectionMask* selection,
                            NativeCmykPaint* nativeCmyk) {
    if (!dst || !targets || targetCount <= 0 || w == 0 || h == 0 ||
        radius <= 0.0f)
        return false;
    const float tol = std::clamp(tolerance, 0.0f, 1.0f);
    const float hard = std::clamp(hardness, 0.0f, 1.0f);
    const float harm = std::clamp(harmony, 0.0f, 1.0f);

    BlendMode blendMode;
    switch (mode) {
        case ReplaceMode::Hue:        blendMode = BlendMode::Hue; break;
        case ReplaceMode::Saturation: blendMode = BlendMode::Saturation; break;
        case ReplaceMode::Luminosity: blendMode = BlendMode::Luminosity; break;
        case ReplaceMode::Color:
        default:                      blendMode = BlendMode::Color; break;
    }
    const bool contiguous = limits == 1 || limits == 2;
    const bool findEdges = limits == 2;
    // Find Edges stops the flood where the luminance steps harder than this.
    // The exact trip point is unpublished; a quarter of the range
    // keeps textured shading passable while real object boundaries hold.
    constexpr float kEdgeStep = 0.25f;

    // Brush footprint in pixel space (same bbox/hardness kernel as
    // paint_dab_host so the brush feels identical).
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));
    if (x1 < x0 || y1 < y0) return false;
    const float inv_r = 1.0f / radius;
    const float soft_span = std::max(1.0f - hard, 1e-4f);

    const auto brush_cov = [&](int x, int y) {
        const float dx = static_cast<float>(x) + 0.5f - cx;
        const float dy = static_cast<float>(y) + 0.5f - cy;
        const float t = std::sqrt(dx * dx + dy * dy) * inv_r;
        if (t >= 1.0f) return 0.0f;
        if (t > 1.0f - soft_span) return (1.0f - t) / soft_span;
        return 1.0f;
    };

    // Match region over the footprint: 1 = recolor this texel. Contiguous
    // modes grow it with a 4-connected flood from the brush centre through
    // in-tolerance texels (Find Edges additionally refuses to step across a
    // strong luminance edge); discontiguous tests every texel in the disc.
    const int fw = x1 - x0 + 1, fh = y1 - y0 + 1;
    std::vector<std::uint8_t> region(std::size_t(fw) * fh, 0);
    const auto at = [&](int x, int y) -> std::size_t {
        return std::size_t(y - y0) * fw + (x - x0);
    };
    const auto matches = [&](int x, int y) {
        const RGBAf& c = dst[std::size_t(y) * w + x];
        if (c.a <= 0.0f) return false;
        for (int t = 0; t < targetCount; ++t)
            if (replace_distance(c, targets[t]) <= tol) return true;
        return false;
    };
    // Nearest-target distance (caller guarantees an opaque texel). Drives the
    // soft tolerance falloff at apply time so the match fades out instead of
    // banding at the cutoff.
    const auto match_dist = [&](int x, int y) {
        const RGBAf& c = dst[std::size_t(y) * w + x];
        float md = 2.0f;
        for (int t = 0; t < targetCount; ++t)
            md = std::min(md, replace_distance(c, targets[t]));
        return md;
    };
    // Smoothstep edge 0→1. `e0 == e1` would divide by zero; the caller guards
    // the zero-tolerance (exact-match) case separately.
    const auto smooth = [](float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    if (!contiguous) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (brush_cov(x, y) <= 0.0f) continue;
                if (matches(x, y)) region[at(x, y)] = 1;
            }
    } else {
        // Contiguous/Find Edges: a 4-connected flood from the brush centre
        // through in-tolerance texels, bounded by the brush footprint (only
        // the disc is ever painted, so the region never needs to leave it).
        // Find Edges additionally refuses to step across a strong luminance
        // edge, so object boundaries survive the recolor.
        const int seedX = std::clamp(static_cast<int>(std::floor(cx)), x0, x1);
        const int seedY = std::clamp(static_cast<int>(std::floor(cy)), y0, y1);
        // A centre that does not match selects nothing: seeding blindly would
        // recolor the seed against the tool's own rule.
        if (matches(seedX, seedY)) {
            std::vector<int> stack;
            stack.reserve(1024);
            region[at(seedX, seedY)] = 1;
            stack.push_back((seedY - y0) * fw + (seedX - x0));
            while (!stack.empty()) {
                const int idx = stack.back();
                stack.pop_back();
                const int x = x0 + idx % fw;
                const int y = y0 + idx / fw;
                const float luma =
                    blend::clum(dst[std::size_t(y) * w + x].r,
                                dst[std::size_t(y) * w + x].g,
                                dst[std::size_t(y) * w + x].b);
                const int nx[4] = {x - 1, x + 1, x, x};
                const int ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    const int qx = nx[k], qy = ny[k];
                    if (qx < x0 || qx > x1 || qy < y0 || qy > y1) continue;
                    if (region[at(qx, qy)]) continue;
                    if (!matches(qx, qy)) continue;
                    if (findEdges) {
                        const RGBAf& q =
                            dst[std::size_t(qy) * w + qx];
                        const float qluma =
                            blend::clum(q.r, q.g, q.b);
                        if (std::fabs(qluma - luma) > kEdgeStep) continue;
                    }
                    region[at(qx, qy)] = 1;
                    stack.push_back((qy - y0) * fw + (qx - x0));
                }
            }
        }
    }

    // Harmony support: per-pixel local means over the pre-dab pixels, via
    // integral images (sum r/g/b + contributing-texel count) so each mean is
    // O(1) no matter the window. Only MATCHED texels with meaningful alpha
    // contribute: averaging the colours being replaced keeps the deviation
    // small and on-hue, while foreign colours next door would skew the mean
    // and print as speckle. A pixel with no contributing neighbours falls
    // back to itself (deviation 0). Built lazily — skipped at harmony 0.
    const int hr =
        harm > 0.0f ? std::clamp(static_cast<int>(radius / 6.0f), 1, 12) : 0;
    const int ix0 = std::max(0, x0 - hr), iy0 = std::max(0, y0 - hr);
    const int ix1 = std::min(static_cast<int>(w) - 1, x1 + hr);
    const int iy1 = std::min(static_cast<int>(h) - 1, y1 + hr);
    const int iw = ix1 - ix0 + 1, ih = iy1 - iy0 + 1;
    std::vector<float> intR, intG, intB;
    std::vector<int> intN;
    if (hr > 0) {
        intR.assign(std::size_t(iw + 1) * (ih + 1), 0.0f);
        intG.assign(std::size_t(iw + 1) * (ih + 1), 0.0f);
        intB.assign(std::size_t(iw + 1) * (ih + 1), 0.0f);
        intN.assign(std::size_t(iw + 1) * (ih + 1), 0);
        for (int y = 0; y < ih; ++y) {
            float rs = 0.0f, gs = 0.0f, bs = 0.0f;
            int ns = 0;
            for (int x = 0; x < iw; ++x) {
                const int lx = ix0 + x, ly = iy0 + y;
                const bool inReg =
                    lx >= x0 && lx <= x1 && ly >= y0 && ly <= y1 &&
                    region[std::size_t(ly - y0) * fw + (lx - x0)];
                const RGBAf& c = dst[std::size_t(ly) * w + lx];
                if (inReg && c.a > 0.01f) {
                    rs += c.r;
                    gs += c.g;
                    bs += c.b;
                    ns += 1;
                }
                const std::size_t row = std::size_t(y + 1) * (iw + 1);
                const std::size_t prev = std::size_t(y) * (iw + 1);
                intR[row + x + 1] = intR[prev + x + 1] + rs;
                intG[row + x + 1] = intG[prev + x + 1] + gs;
                intB[row + x + 1] = intB[prev + x + 1] + bs;
                intN[row + x + 1] = intN[prev + x + 1] + ns;
            }
        }
    }
    const auto local_mean = [&](int x, int y, float& mr, float& mg,
                                float& mb) {
        const int qx0 = std::max(ix0, x - hr), qy0 = std::max(iy0, y - hr);
        const int qx1 = std::min(ix1, x + hr), qy1 = std::min(iy1, y + hr);
        const std::size_t A = std::size_t(qy0 - iy0) * (iw + 1) + (qx0 - ix0);
        const std::size_t B = std::size_t(qy0 - iy0) * (iw + 1) + (qx1 - ix0 + 1);
        const std::size_t C = std::size_t(qy1 - iy0 + 1) * (iw + 1) + (qx0 - ix0);
        const std::size_t D =
            std::size_t(qy1 - iy0 + 1) * (iw + 1) + (qx1 - ix0 + 1);
        const int n = intN[D] - intN[B] - intN[C] + intN[A];
        if (n <= 0) {
            const RGBAf& c = dst[std::size_t(y) * w + x];
            mr = c.r;
            mg = c.g;
            mb = c.b;
            return;
        }
        const float inv = 1.0f / static_cast<float>(n);
        mr = (intR[D] - intR[B] - intR[C] + intR[A]) * inv;
        mg = (intG[D] - intG[B] - intG[C] + intG[A]) * inv;
        mb = (intB[D] - intB[B] - intB[C] + intB[A]) * inv;
    };

    // Apply: brush coverage × region coverage (faded when Anti-alias is on)
    // × selection.
    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    bool any = false;
    // Faded edges: with Anti-alias the region mask is box-blurred so the
    // recolor fades over several pixels instead of stopping dead. Radius
    // scales with the brush (~1/8 of it, 2..8px) via an integral image, so
    // even a huge brush stays O(1) per pixel.
    const int fr = antialias ? std::clamp(static_cast<int>(radius / 8.0f), 2, 8) : 0;
    std::vector<int> intReg;
    if (antialias) {
        intReg.assign(std::size_t(fw + 1) * (fh + 1), 0);
        for (int y = 0; y < fh; ++y) {
            int rowSum = 0;
            for (int x = 0; x < fw; ++x) {
                rowSum += region[std::size_t(y) * fw + x] ? 1 : 0;
                const std::size_t row = std::size_t(y + 1) * (fw + 1);
                const std::size_t prev = std::size_t(y) * (fw + 1);
                intReg[row + x + 1] = intReg[prev + x + 1] + rowSum;
            }
        }
    }
    const int margin = antialias ? fr : 0;
    for (int y = std::max(0, y0 - margin);
         y <= std::min(static_cast<int>(h) - 1, y1 + margin); ++y) {
        for (int x = std::max(0, x0 - margin);
             x <= std::min(static_cast<int>(w) - 1, x1 + margin); ++x) {
            const float bcov = (x < x0 || x > x1 || y < y0 || y > y1)
                                   ? 0.0f
                                   : brush_cov(x, y);
            if (bcov <= 0.0f) continue;
            float rcov;
            if (!antialias) {
                if (!region[at(x, y)]) continue;
                rcov = 1.0f;
            } else {
                // Faded edge: mean region membership over the fade window,
                // normalised by the in-footprint cell count so a region
                // touching the footprint border keeps full coverage there.
                // Out-of-footprint samples read as 0 (the disc clips anyway).
                const int qx0 = std::max(x0, x - fr), qy0 = std::max(y0, y - fr);
                const int qx1 = std::min(x1, x + fr), qy1 = std::min(y1, y + fr);
                const int valid = (qx1 - qx0 + 1) * (qy1 - qy0 + 1);
                if (valid <= 0) continue;
                const std::size_t A = std::size_t(qy0 - y0) * (fw + 1) + (qx0 - x0);
                const std::size_t B = std::size_t(qy0 - y0) * (fw + 1) + (qx1 - x0 + 1);
                const std::size_t C = std::size_t(qy1 - y0 + 1) * (fw + 1) + (qx0 - x0);
                const std::size_t D =
                    std::size_t(qy1 - y0 + 1) * (fw + 1) + (qx1 - x0 + 1);
                const int sum = intReg[D] - intReg[B] - intReg[C] + intReg[A];
                rcov = static_cast<float>(sum) / static_cast<float>(valid);
                if (rcov <= 0.0f) continue;
            }
            float k = bcov * rcov;
            // Soft tolerance falloff: full strength well inside the cutoff,
            // fading to zero exactly at it — neighbouring shades blend into
            // each other instead of banding. Zero tolerance stays exact.
            if (tol <= 1e-6f) {
                if (match_dist(x, y) > 0.0f) continue;
            } else {
                const float mw =
                    1.0f - smooth(tol * 0.75f, tol, match_dist(x, y));
                if (mw <= 0.0f) continue;
                k *= mw;
            }
            if (selection) {
                k *= selection->coverage(x, y);
                if (k <= 0.0f) continue;
            }
            RGBAf& d = dst[std::size_t(y) * w + x];
            if (d.a <= 0.0f) continue;
            const RGBAf before = d;
            const std::size_t pixelIndex = std::size_t(y) * w + x;
            std::array<std::uint16_t, 4> beforeInk{};
            const bool hasNativeInk = nativeCmyk && nativeCmyk->pixels &&
                nativeCmyk->preview && nativeCmyk->separation;
            if (hasNativeInk) beforeInk = nativeCmyk->pixels[pixelIndex];
            // Plain transfer: the mode blend between this pixel and the
            // replacement colour.
            float nr, ng, nb;
            blend::stitch_color(blendMode, before.r, before.g, before.b,
                                replacement.r, replacement.g, replacement.b,
                                nr, ng, nb);
            if (harm > 0.0f) {
                // Re-anchor on the neighbourhood: the recolored local mean
                // plus this pixel's own deviation from it. Local texture
                // survives verbatim; only the base colour moves. The
                // deviation is scaled to fit the gamut (largest t with the
                // result in [0,1]) rather than hard-clipped, so an
                // overshooting texel keeps its hue instead of printing as a
                // wrong-colour speck. Mixed with the plain transfer so
                // Harmony reads as a strength.
                float mr, mg, mb;
                local_mean(x, y, mr, mg, mb);
                float ar, ag, ab;
                blend::stitch_color(blendMode, mr, mg, mb, replacement.r,
                                    replacement.g, replacement.b, ar, ag, ab);
                const float dr = before.r - mr;
                const float dg = before.g - mg;
                const float db = before.b - mb;
                float t = 1.0f;
                const auto fit = [&](float base, float d) {
                    if (d > 1e-6f)
                        t = std::min(t, (1.0f - base) / d);
                    else if (d < -1e-6f)
                        t = std::min(t, (0.0f - base) / d);
                };
                fit(ar, dr);
                fit(ag, dg);
                fit(ab, db);
                t = std::clamp(t, 0.0f, 1.0f);
                ar += dr * t;
                ag += dg * t;
                ab += db * t;
                nr += (ar - nr) * harm;
                ng += (ag - ng) * harm;
                nb += (ab - nb) * harm;
            }
            bool inkChanged = false;
            if (hasNativeInk) {
                float resultInk[4];
                nativeCmyk->separation->convert(
                    std::clamp(nr, 0.0f, 1.0f),
                    std::clamp(ng, 0.0f, 1.0f),
                    std::clamp(nb, 0.0f, 1.0f), resultInk);
                auto& ink = nativeCmyk->pixels[pixelIndex];
                float previewInk[4];
                for (int c = 0; c < 4; ++c) {
                    const float oldValue = beforeInk[c] / 65535.0f;
                    const float replacementValue = resultInk[c];
                    const float mixed = oldValue +
                        (replacementValue - oldValue) * k;
                    ink[c] = static_cast<std::uint16_t>(std::clamp(
                        std::lround(mixed * 65535.0f), 0L, 65535L));
                    previewInk[c] = ink[c] / 65535.0f;
                }
                inkChanged = beforeInk != ink;
                float rgb[3];
                nativeCmyk->preview->convert(previewInk[0], previewInk[1],
                                             previewInk[2], previewInk[3], rgb);
                d.r = rgb[0]; d.g = rgb[1]; d.b = rgb[2];
            } else {
                d.r = before.r + (nr - before.r) * k;
                d.g = before.g + (ng - before.g) * k;
                d.b = before.b + (nb - before.b) * k;
            }
            if (replace_distance(before, d) < 1e-6f && !inkChanged) continue;
            if (!any) {
                rx0 = rx1 = x;
                ry0 = ry1 = y;
                any = true;
            } else {
                rx0 = std::min(rx0, x);
                ry0 = std::min(ry0, y);
                rx1 = std::max(rx1, x);
                ry1 = std::max(ry1, y);
            }
        }
    }

    if (!any) return false;
    if (bbox) {
        bbox[0] = rx0;
        bbox[1] = ry0;
        bbox[2] = rx1 + 1;  // half-open
        bbox[3] = ry1 + 1;
    }
    return true;
}

}  // namespace pittore::compute
