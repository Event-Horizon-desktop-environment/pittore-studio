#include "engine/compute/brushes/dab/dab.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "engine/core/pixel.h"
#include "engine/color/icc_convert.h"

namespace pittore::compute {

void paint_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, float cx,
                    float cy, float radius, float hardness, float opacity,
                    const RGBAf& color, const SelectionMask* selection,
                    const PatternTex* tex, const DabDensity* den,
                    const MaskTip* mask,
                    const papergrain::PaperGrain* kgrain,
                    dabmix::DabMixOp mix, NativeCmykPaint* nativeCmyk,
                    BlendMode blend) {
    if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;

    const float hard = std::clamp(hardness, 0.0f, 1.0f);
    const float op = std::clamp(opacity, 0.0f, 1.0f);

    // Falloff: hardness=1 keeps full coverage inside, dropping at the rim.
    // hardness=0 ramps linearly from centre to rim (soft brush). We scale
    // (1 - t) with t = dist/radius, clamped, so the reach is exactly radius.
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));

    const float inv_r = 1.0f / radius;
    const float soft_span = std::max(1.0f - hard, 1e-4f);  // fraction of radius over which edge fades

    const float cr = color.r, cg = color.g, cb = color.b;

    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        std::size_t idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x0);
        for (int x = x0; x <= x1; ++x, ++idx) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            float t = std::sqrt(dx * dx + dy * dy) * inv_r;
            if (t >= 1.0f) continue;

            // Coverage: 1 in the core, linear falloff across the soft edge.
            float cov = 1.0f;
            if (t > 1.0f - soft_span)
                cov = (1.0f - t) / soft_span;
            cov = combine_mask(cov, mask, dx, dy, radius);
            if (cov <= 0.0f) continue;
            if (selection) cov *= selection->coverage(x, y);
            if (kgrain)
                cov = papergrain::grain_mult(kgrain, cov, float(x), float(y));
            else if (tex)
                cov *= texture_mult(tex, float(x), float(y));
            if (cov <= 0.0f || !density_keep(den, x, y)) continue;
            const float a = cov * op;
            if (a <= 0.0f) continue;

            RGBAf& d = dst[idx];
            if (mix == dabmix::DabMixOp::Normal &&
                blend == BlendMode::Normal) {
                const float inv_a = 1.0f - a;
                const float out_a = a + d.a * inv_a;
                if (out_a > 0.0f) {
                    if (nativeCmyk && nativeCmyk->pixels && nativeCmyk->preview) {
                        auto& ink = nativeCmyk->pixels[idx];
                        float previewInk[4];
                        for (int c = 0; c < 4; ++c) {
                            const float oldInk = ink[c] / 65535.0f;
                            const float srcInk = nativeCmyk->source[c] / 65535.0f;
                            const float mixed = (a * srcInk + d.a * oldInk * inv_a) / out_a;
                            ink[c] = static_cast<std::uint16_t>(std::clamp(
                                std::lround(mixed * 65535.0f), 0L, 65535L));
                            previewInk[c] = ink[c] / 65535.0f;
                        }
                        float rgb[3];
                        nativeCmyk->preview->convert(previewInk[0], previewInk[1],
                                                     previewInk[2], previewInk[3], rgb);
                        d.r = rgb[0]; d.g = rgb[1]; d.b = rgb[2];
                    } else {
                        d.r = (a * cr + d.r * d.a * inv_a) / out_a;
                        d.g = (a * cg + d.g * d.a * inv_a) / out_a;
                        d.b = (a * cb + d.b * d.a * inv_a) / out_a;
                    }
                    d.a = out_a;
                }
            } else if (mix == dabmix::DabMixOp::Normal) {
                // Per-dab blend mode: the W3C shell replaces the source-over
                // mix; native CMYK destinations re-separate the blended
                // projection back to device inks.
                if (!native_cmyk_blend_texel(nativeCmyk, idx, d, blend, cr, cg,
                                             cb, a, x, y))
                    blend::pixel(blend, cr, cg, cb, a, d.r, d.g, d.b, d.a, x,
                                 y, d.r, d.g, d.b, d.a);
            } else {
                dabmix::mix_pixel(mix, cr, cg, cb, a, d.r, d.g, d.b, d.a,
                                  d.r, d.g, d.b, d.a);
            }
        }
    }
}

// Auto-tip dab: the unit circle maps to an ellipse whose major axis is the
// rotated +x axis (horizontal at angle 0, turning counter-clockwise with
// positive angles). ratio==1 reproduces the round kernel exactly. Square
// measures distance in the rotated Chebyshev norm. The bbox covers the
// rotated major axis in both dimensions (conservative).
void paint_tip_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, float cx,
                        float cy, float radius, const AutoTip& tip,
                        float opacity, const RGBAf& color,
                        const SelectionMask* selection,
                        const PatternTex* tex, const DabDensity* den,
                    const MaskTip* mask,
                    const papergrain::PaperGrain* kgrain,
                    dabmix::DabMixOp mix, NativeCmykPaint* nativeCmyk,
                    BlendMode blend) {
    if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
    AutoTip t = tip;
    t.sanitize();

    const float op = std::clamp(opacity, 0.0f, 1.0f);
    // Inverse map: rotate by -angle, divide x-measure by ratio. Precompute
    // cos/sin of -angle so each pixel is two mults + one norm.
    const float rad = t.angleDeg * 3.14159265358979323846f / 180.0f;
    const float cosA = std::cos(rad), sinA = std::sin(rad);
    const float invRatio = 1.0f / t.ratio;
    // Rotated bbox half-extent of the major axis (conservative: radius both).
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));
    const float inv_r = 1.0f / radius;
    const float cr = color.r, cg = color.g, cb = color.b;

    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        std::size_t idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x0);
        for (int x = x0; x <= x1; ++x, ++idx) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            // Shared shape (spikes/aniso/falloff/sharpness live in tip.h).
            float cov = auto_tip_coverage(dx, dy, inv_r, t, cosA, sinA,
                                          invRatio);
            if (cov <= 0.0f) continue;
            cov = combine_mask(cov, mask, dx, dy, radius);
            if (cov <= 0.0f) continue;
            if (selection) cov *= selection->coverage(x, y);
            if (kgrain)
                cov = papergrain::grain_mult(kgrain, cov, float(x), float(y));
            else if (tex)
                cov *= texture_mult(tex, float(x), float(y));
            if (cov <= 0.0f || !density_keep(den, x, y)) continue;
            const float a = cov * op;
            if (a <= 0.0f) continue;
            RGBAf& d = dst[idx];
            if (mix == dabmix::DabMixOp::Normal &&
                blend == BlendMode::Normal) {
                const float inv_a = 1.0f - a;
                const float out_a = a + d.a * inv_a;
                if (out_a > 0.0f) {
                    if (nativeCmyk && nativeCmyk->pixels && nativeCmyk->preview) {
                        auto& ink = nativeCmyk->pixels[idx];
                        float previewInk[4];
                        for (int c = 0; c < 4; ++c) {
                            const float oldInk = ink[c] / 65535.0f;
                            const float srcInk = nativeCmyk->source[c] / 65535.0f;
                            const float mixed = (a * srcInk + d.a * oldInk * inv_a) / out_a;
                            ink[c] = static_cast<std::uint16_t>(std::clamp(
                                std::lround(mixed * 65535.0f), 0L, 65535L));
                            previewInk[c] = ink[c] / 65535.0f;
                        }
                        float rgb[3];
                        nativeCmyk->preview->convert(previewInk[0], previewInk[1],
                                                     previewInk[2], previewInk[3], rgb);
                        d.r = rgb[0]; d.g = rgb[1]; d.b = rgb[2];
                    } else {
                        d.r = (a * cr + d.r * d.a * inv_a) / out_a;
                        d.g = (a * cg + d.g * d.a * inv_a) / out_a;
                        d.b = (a * cb + d.b * d.a * inv_a) / out_a;
                    }
                    d.a = out_a;
                }
            } else if (mix == dabmix::DabMixOp::Normal) {
                // Per-dab blend mode: the W3C shell replaces the source-over
                // mix; native CMYK destinations re-separate the blended
                // projection back to device inks.
                if (!native_cmyk_blend_texel(nativeCmyk, idx, d, blend, cr, cg,
                                             cb, a, x, y))
                    blend::pixel(blend, cr, cg, cb, a, d.r, d.g, d.b, d.a, x,
                                 y, d.r, d.g, d.b, d.a);
            } else {
                dabmix::mix_pixel(mix, cr, cg, cb, a, d.r, d.g, d.b, d.a,
                                  d.r, d.g, d.b, d.a);
            }
        }
    }
}

}  // namespace pittore::compute
