#include "engine/compute/brushes/stamp/stamp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/parallel.h"
#include "engine/core/pixel.h"
#include "engine/color/icc_convert.h"

namespace pittore::compute {
namespace {

// Tip-plane sample with zero outside: bilinear (smooth stamps) or
// nearest (draft tier: crisp pixel-art tips, cheaper per texel).
float sample_plane(const float* p, std::uint32_t w, std::uint32_t h, float u,
                   float v, bool bilinear = true) {
    if (u < 0.0f || v < 0.0f || u > float(w) - 1.0f || v > float(h) - 1.0f)
        return 0.0f;
    if (!bilinear) {
        const std::uint32_t xi = std::min(static_cast<std::uint32_t>(u + 0.5f),
                                          w - 1);
        const std::uint32_t yi = std::min(static_cast<std::uint32_t>(v + 0.5f),
                                          h - 1);
        return p[std::size_t(yi) * w + xi];
    }
    const std::uint32_t x0 = static_cast<std::uint32_t>(u);
    const std::uint32_t y0 = static_cast<std::uint32_t>(v);
    const std::uint32_t x1 = x0 + 1 < w ? x0 + 1 : x0;
    const std::uint32_t y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float fx = u - float(x0), fy = v - float(y0);
    const float a = p[std::size_t(y0) * w + x0];
    const float b = p[std::size_t(y0) * w + x1];
    const float c = p[std::size_t(y1) * w + x0];
    const float d = p[std::size_t(y1) * w + x1];
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

struct StampMap {
    float cosA, sinA, scale;  // dab px -> tip px
    std::uint32_t tw, th;
};

// Tip's longest side spans the dab diameter (2*radius).
StampMap make_map(const StampTip& tip, float radius, float angleDeg) {
    const float rad = angleDeg * 3.14159265358979323846f / 180.0f;
    StampMap m{std::cos(rad), std::sin(rad), 1.0f, tip.w, tip.h};
    const float longest = float(tip.w > tip.h ? tip.w : tip.h);
    m.scale = longest / (2.0f * radius);
    return m;
}

}  // namespace

void stamp_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, float cx,
                    float cy, float radius, const StampTip& tip,
                    StampMode mode, float opacity, const RGBAf& color,
                    float angleDeg, const SelectionMask* selection,
                    const PatternTex* tex, const DabDensity* den,
                    const MaskTip* mask, int flip,
                    int filter, const StampLevels* levels,
                    const RGBAf* bg, float* height, float heightAmt,
                    dabmix::DabMixOp mix,
                    const papergrain::PaperGrain* kgrain,
                    const StampSharp* sharp,
                    NativeCmykPaint* nativeCmyk) {
    if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
    if (!tip.valid()) return;
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    const StampMap m = make_map(tip, radius, angleDeg);
    const bool useLight = mode == StampMode::LightnessMap;
    const bool useGrad = mode == StampMode::GradientMap;
    const bool useMap = (useLight || useGrad);
    const float neutral =
        levels ? std::clamp(levels->neutral, 0.0f, 1.0f) : 0.5f;
    const float bright =
        levels ? std::clamp(levels->brightness, -1.0f, 1.0f) : 0.0f;
    const float contrast =
        levels ? std::clamp(levels->contrast, 0.0f, 4.0f) : 1.0f;

    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));
    const float cxTip = (float(tip.w) - 1.0f) * 0.5f;
    const float cyTip = (float(tip.h) - 1.0f) * 0.5f;

    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        std::size_t idx = std::size_t(y) * w + std::size_t(x0);
        for (int x = x0; x <= x1; ++x, ++idx) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            // Dab frame -> tip frame: rotate counter-clockwise by the
            // angle, scale to tip px.
            const float uc = (dx * m.cosA - dy * m.sinA) * m.scale;
            const float vc = (dx * m.sinA + dy * m.cosA) * m.scale;
            const float u = (flip & 1 ? -uc : uc) + cxTip;
            const float v = (flip & 2 ? -vc : vc) + cyTip;
            float cov = sample_plane(tip.alpha.data(), tip.w, tip.h, u, v,
                                       filter != 0);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            cov = combine_mask(cov, mask, dx, dy, radius);
            if (cov <= 0.0f) continue;
            if (selection) cov *= selection->coverage(x, y);
            // Sharpness threshold runs before grain (imported-behavior
            // order); grain replaces the legacy texture when present.
            cov = sharpen_coverage(cov, sharp);
            if (kgrain)
                cov = papergrain::grain_mult(kgrain, cov, float(x), float(y));
            else if (tex)
                cov *= texture_mult(tex, float(x), float(y));
            if (!density_keep(den, x, y)) continue;
            const float a = cov * op;
            if (a <= 0.0f) continue;
            float cr = color.r, cg = color.g, cb = color.b;
            float lightT = 0.0f;
            bool hasLight = false;
            if (mode == StampMode::ColorImage && tip.color) {
                cr = sample_plane(tip.red.data(), tip.w, tip.h, u, v,
                                    filter != 0);
                cg = sample_plane(tip.green.data(), tip.w, tip.h, u, v,
                                    filter != 0);
                cb = sample_plane(tip.blue.data(), tip.w, tip.h, u, v,
                                    filter != 0);
            } else if (useMap) {
                // Tip lightness: color tips use Rec.709 luma of the RGB
                // planes; grayscale tips reuse coverage (dark paints).
                float light;
                if (tip.color) {
                    const float lr = sample_plane(tip.red.data(), tip.w,
                                                  tip.h, u, v, filter != 0);
                    const float lg = sample_plane(tip.green.data(), tip.w,
                                                  tip.h, u, v, filter != 0);
                    const float lb = sample_plane(tip.blue.data(), tip.w,
                                                  tip.h, u, v, filter != 0);
                    light = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
                } else {
                    light = 1.0f - cov;
                }
                const float t = std::clamp(
                    (light - neutral) * contrast + neutral + bright,
                    0.0f, 1.0f);
                if (useLight) {
                    lightT = t;
                    hasLight = true;
                }
                if (useGrad) {
                    const float br = bg ? bg->r : color.r;
                    const float bge = bg ? bg->g : color.g;
                    const float bb = bg ? bg->b : color.b;
                    cr = std::clamp(br + (color.r - br) * t, 0.0f, 1.0f);
                    cg = std::clamp(bge + (color.g - bge) * t, 0.0f, 1.0f);
                    cb = std::clamp(bb + (color.b - bb) * t, 0.0f, 1.0f);
                } else {
                    const float lift = t - neutral;
                    cr = std::clamp(color.r + lift, 0.0f, 1.0f);
                    cg = std::clamp(color.g + lift, 0.0f, 1.0f);
                    cb = std::clamp(color.b + lift, 0.0f, 1.0f);
                }
            }
            RGBAf& d = dst[idx];
            if (mix == dabmix::DabMixOp::Normal) {
                const float inv_a = 1.0f - a;
                const float out_a = a + d.a * inv_a;
                if (out_a > 0.0f) {
                    if (nativeCmyk && nativeCmyk->pixels && nativeCmyk->preview) {
                        auto& ink = nativeCmyk->pixels[idx];
                        std::array<std::uint16_t, 4> source = nativeCmyk->source;
                        if (nativeCmyk->sourceFromRgb && nativeCmyk->separation) {
                            float separated[4];
                            nativeCmyk->separation->convert(cr, cg, cb, separated);
                            for (int c = 0; c < 4; ++c)
                                source[c] = static_cast<std::uint16_t>(std::clamp(
                                    std::lround(separated[c] * 65535.0f), 0L, 65535L));
                        }
                        float values[4];
                        for (int c = 0; c < 4; ++c) {
                            const float mixed =
                                (a * (source[c] / 65535.0f) +
                                 d.a * (ink[c] / 65535.0f) * inv_a) / out_a;
                            ink[c] = static_cast<std::uint16_t>(std::clamp(
                                std::lround(mixed * 65535.0f), 0L, 65535L));
                            values[c] = ink[c] / 65535.0f;
                        }
                        float rgb[3];
                        nativeCmyk->preview->convert(values[0], values[1],
                                                     values[2], values[3], rgb);
                        d.r = rgb[0]; d.g = rgb[1]; d.b = rgb[2];
                    } else {
                        d.r = (a * cr + d.r * d.a * inv_a) / out_a;
                        d.g = (a * cg + d.g * d.a * inv_a) / out_a;
                        d.b = (a * cb + d.b * d.a * inv_a) / out_a;
                    }
                    d.a = out_a;
                }
            } else {
                dabmix::mix_pixel(mix, cr, cg, cb, a, d.r, d.g, d.b, d.a,
                                  d.r, d.g, d.b, d.a);
            }
            // Relief deposition (lightness mode only): relief builds toward
            // the tip lightness scaled by thickness, weighted by coverage —
            // paint piles up, never carves.
            if (height && hasLight && heightAmt > 0.0f) {
                float& h = height[idx];
                const float dep = lightT * heightAmt;
                h += (std::max(h, dep) - h) * cov;
            }
        }
    }
}

void stamp_erase_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h,
                          float cx, float cy, float radius,
                          const StampTip& tip, float opacity, float angleDeg,
                          const SelectionMask* selection,
                          const PatternTex* tex, const DabDensity* den,
                    const MaskTip* mask, int flip,
                    int filter, float* height,
                    const papergrain::PaperGrain* kgrain) {
    if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
    if (!tip.valid()) return;
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    const StampMap m = make_map(tip, radius, angleDeg);

    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));
    const float cxTip = (float(tip.w) - 1.0f) * 0.5f;
    const float cyTip = (float(tip.h) - 1.0f) * 0.5f;

    for (int y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        std::size_t idx = std::size_t(y) * w + std::size_t(x0);
        for (int x = x0; x <= x1; ++x, ++idx) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float uc = (dx * m.cosA - dy * m.sinA) * m.scale;
            const float vc = (dx * m.sinA + dy * m.cosA) * m.scale;
            const float u = (flip & 1 ? -uc : uc) + cxTip;
            const float v = (flip & 2 ? -vc : vc) + cyTip;
            float cov = sample_plane(tip.alpha.data(), tip.w, tip.h, u, v,
                                       filter != 0);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            cov = combine_mask(cov, mask, dx, dy, radius);
            if (cov <= 0.0f) continue;
            if (selection) cov *= selection->coverage(x, y);
            if (kgrain)
                cov = papergrain::grain_mult(kgrain, cov, float(x), float(y));
            else if (tex)
                cov *= texture_mult(tex, float(x), float(y));
            if (!density_keep(den, x, y)) continue;
            const float a = cov * op;
            if (a <= 0.0f) continue;
            dst[idx].a *= (1.0f - a);
            if (height) height[idx] *= (1.0f - a);  // erase carves relief
        }
    }
}


bool pattern_stamp_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h,
                            float cx, float cy, float radius, float hardness,
                            float opacity, const PatternTile& tile, float ox,
                            float oy, int* bboxOut,
                            const SelectionMask* selection) {
    if (!dst || !tile.valid() || radius <= 0.0f || opacity <= 0.0f || w == 0 ||
        h == 0)
        return false;
    const float hard = std::clamp(hardness, 0.0f, 1.0f);
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(static_cast<int>(h) - 1,
                            static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(static_cast<int>(w) - 1,
                            static_cast<int>(std::ceil(cx + radius)));
    if (y1 < y0 || x1 < x0) return false;
    const float inv_r = 1.0f / radius;
    constexpr int TS = PatternTile::kSize;
    std::atomic<bool> touched{false};
    core::parallel_rows(std::uint32_t(y1 - y0 + 1),
                        [&](std::uint32_t lo, std::uint32_t hi) {
                            bool local = false;
                            for (std::uint32_t r = lo; r < hi; ++r) {
                                const int y = y0 + int(r);
                                const float dy =
                                    static_cast<float>(y) + 0.5f - cy;
                                std::size_t idx =
                                    std::size_t(y) * w + std::size_t(x0);
                                for (int x = x0; x <= x1; ++x, ++idx) {
                                    const float dx =
                                        static_cast<float>(x) + 0.5f - cx;
                                    const float t = std::sqrt(dx * dx +
                                                              dy * dy) *
                                                    inv_r;
                                    if (t >= 1.0f) continue;
                                    float cov = 1.0f;
                                    if (t > hard) {
                                        const float span = std::max(
                                            1.0f - hard, 1e-4f);
                                        cov = (1.0f - t) / span;
                                    }
                                    if (selection)
                                        cov *= selection->coverage(x, y);
                                    const float a = cov * op;
                                    if (!(a > 0.0f)) continue;
                                    int tx = int(std::floor(float(x) - ox)) % TS;
                                    int ty = int(std::floor(float(y) - oy)) % TS;
                                    if (tx < 0) tx += TS;
                                    if (ty < 0) ty += TS;
                                    const RGBAf& s = tile.px[std::size_t(ty) *
                                                             TS + tx];
                                    RGBAf& d = dst[idx];
                                    const float inv_a = 1.0f - a;
                                    const float out_a = a + d.a * inv_a;
                                    if (out_a > 0.0f) {
                                        d.r = (a * s.r + d.r * d.a * inv_a) /
                                              out_a;
                                        d.g = (a * s.g + d.g * d.a * inv_a) /
                                              out_a;
                                        d.b = (a * s.b + d.b * d.a * inv_a) /
                                              out_a;
                                        d.a = out_a;
                                    }
                                    local = true;
                                }
                            }
                            if (local) touched.store(true);
                        });
    if (bboxOut) {
        bboxOut[0] = x0;
        bboxOut[1] = y0;
        bboxOut[2] = x1 + 1;
        bboxOut[3] = y1 + 1;
    }
    return touched.load();
}

}  // namespace pittore::compute
