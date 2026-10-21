#pragma once
// Live adjustment-layer math, shared verbatim by the CPU reference path and
// the CUDA/HIP device kernels — the Phase-1 blend.h pattern. One source of
// truth: the parity tests hold CPU and GPU bit-for-bit equal (at float
// tolerance) for every kind.
//
// All adjustments work on straight (non-premultiplied) linear-light f32 RGB;
// alpha is never touched here (the compositor folds opacity/mask/clip into
// the blend separately). Values are clamped to [0,1] on output.
//
// Parameter semantics per kind (see AdjustmentParams::p):
//   BrightnessContrast  p[0] brightness [-1,1], p[1] contrast [-1,1]
//   Levels              p[0] inBlack, p[1] inWhite, p[2] gamma,
//                       p[3] outBlack, p[4] outWhite (master channel, 0..1
//                       except gamma in (0,inf); matches applyLevels)
//   Curves              256-entry LUT in `lut` (null = identity); bin rule
//                       matches applyCurveLUT exactly
//   Exposure            p[0] stops (may be negative)
//   Vibrance            p[0] [-1,1]; positive boosts muted colours while
//                       protecting vivid ones, negative desaturates uniformly
//   HueSaturation       p[0] hue shift degrees, p[1] saturation [-1,1],
//                       p[2] lightness [-1,1] (matches cpu_hue_saturation)
//   Invert              no params
//   Threshold           p[0] level 0..1 (luma threshold)
//   Posterize           p[0] level count >= 2
//   PhotoFilter         p[0..2] filter color RGB 0..1, p[3] density 0..1,
//                       p[4] preserve luminosity flag (gel multiply + HSL
//                       lightness restore)
//   WhiteBalance        p[0] temperature Kelvin, p[1] tint -1..1
//                       (- green, + magenta; Helland illuminant, diagonal gains)
//   BlackWhite          p[0..5] R/Y/G/C/B/M weights -200..300, 50 neutral
//                       (triangular hue mix, saturation-weighted)
//   ChannelMixer        p[0..2] red-out row, p[3..5] green-out row,
//                       p[6..8] blue-out row (mix -2..2), p[9..11] constants,
//                       p[12] monochrome flag (grey from red row)
//   ColorBalance        p[0..2] shadows, p[3..5] midtones, p[6..8] highlights
//                       (C-R/M-G/Y-B, -100..100), p[9] preserve flag
//                       (triangular zone weights on lightness)
//   Recolour           p[0] hue 0..360 degrees, p[1] saturation 0..1,
//                       p[2] lightness offset -1..1 (single hue at single
//                       saturation over the source's own HSL lightness)

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/pixel.h"

// nvcc predefines __forceinline__; hipcc exposes it only after including hip
// runtime headers, so HIP kernels use __device__ inline (same semantics for
// the purposes of these pure helpers). Host compilers get plain inline.
#if defined(__CUDACC__)
#define PITTORE_ADJUST_DEVICE __device__ __forceinline__
#elif defined(__HIPCC__)
#define PITTORE_ADJUST_DEVICE __device__ inline
#else
#define PITTORE_ADJUST_DEVICE inline
#endif

namespace pittore::compute {

enum class AdjustmentKind {
    None = 0,
    BrightnessContrast = 1,
    Levels = 2,
    Curves = 3,
    Exposure = 4,
    Vibrance = 5,
    HueSaturation = 6,
    Invert = 7,
    Threshold = 8,
    Posterize = 9,
    PhotoFilter = 10,
    WhiteBalance = 11,
    BlackWhite = 12,
    ChannelMixer = 13,
    ColorBalance = 14,
    Recolour = 15,
};

struct AdjustmentParams {
    float p[16] = {};
};

namespace adjust {

PITTORE_ADJUST_DEVICE float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

PITTORE_ADJUST_DEVICE float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

PITTORE_ADJUST_DEVICE float absf(float v) {
    return v < 0.0f ? -v : v;
}

PITTORE_ADJUST_DEVICE float adj_pow(float base, float exp) {
#if defined(__CUDACC__) || defined(__HIPCC__)
    return ::powf(base, exp);
#else
    return std::pow(base, exp);
#endif
}

PITTORE_ADJUST_DEVICE float adj_pow2(float exp) {
#if defined(__CUDACC__) || defined(__HIPCC__)
    return ::exp2f(exp);
#else
    return std::exp2(exp);
#endif
}

PITTORE_ADJUST_DEVICE float adj_log(float v) {
#if defined(__CUDACC__) || defined(__HIPCC__)
    return ::logf(v);
#else
    return std::log(v);
#endif
}

// Fractional part in [0,1). All in-engine callers pass v in [0,3)
// (hue fractions), where two subtracts are correctly-rounded — hence
// bit-identical to fmod(v, 1.0f) there — at ~2 cycles vs a libm call.
// Out-of-domain inputs fall back to fmod (never taken in-engine).
PITTORE_ADJUST_DEVICE float frac_unit(float v) {
    if (v >= 2.0f) {
        if (v < 3.0f) return v - 2.0f;
    } else if (v >= 1.0f) {
        return v - 1.0f;
    } else if (v >= 0.0f) {
        return v;
    }
#if defined(__CUDACC__) || defined(__HIPCC__)
    return ::fmodf(v, 1.0f);
#else
    return std::fmod(v, 1.0f);
#endif
}

// Rec.709 luma, matching the engine's grayscale weights.
PITTORE_ADJUST_DEVICE float luma709(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// --- HSL helpers (same formulas as cpu_hue_saturation) ---------------------

PITTORE_ADJUST_DEVICE float hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

PITTORE_ADJUST_DEVICE void rgb_to_hsl(float r, float g, float b, float& h,
                                       float& s, float& l) {
    const float cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    l = 0.5f * (cmax + cmin);
    if (cmax == cmin) {
        h = s = 0.0f;
        return;
    }
    const float d = cmax - cmin;
    const float denom = 1.0f - absf(2.0f * l - 1.0f);
    // Zero with d > 0 only for out-of-range inputs: no saturation there
    // (d/0 = inf would come back out of hsl_to_rgb as NaN).
    s = denom < 1e-6f ? 0.0f : d / denom;
    if (cmax == r)
        h = d == 0.0f ? 0.0f : (g - b) / d + 6.0f;
    else if (cmax == g)
        h = (b - r) / d + 2.0f;
    else
        h = (r - g) / d + 4.0f;
    // Keep h in [0,6) like fmod((g-b)/d + 6, 6): inputs here are bounded
    // (|g-b|/d <= 1... plus 6), so subtract 6 when over.
    if (h >= 6.0f) h -= 6.0f;
    h /= 6.0f;
}

PITTORE_ADJUST_DEVICE void hsl_to_rgb(float h, float s, float l, float& r,
                                       float& g, float& b) {
    if (s == 0.0f) {
        r = g = b = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
}

// Lightness only: (max+min)/2 with the exact same min/max picks as
// rgb_to_hsl, so the result is bit-identical while skipping the hue
// branches and the saturation division. For callers that only need ls
// (color_balance zone weights).
PITTORE_ADJUST_DEVICE float rgb_to_lightness(float r, float g, float b) {
    const float cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return 0.5f * (cmax + cmin);
}

// Hue + lightness without saturation: bit-identical h/l to rgb_to_hsl
// (verbatim branch structure), skipping the unused saturation division.
// For black_white, which re-derives saturation from max/min itself.
PITTORE_ADJUST_DEVICE void rgb_to_hue_light(float r, float g, float b,
                                             float& h, float& l) {
    const float cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    l = 0.5f * (cmax + cmin);
    if (cmax == cmin) {
        h = 0.0f;
        return;
    }
    const float d = cmax - cmin;
    if (cmax == r)
        h = d == 0.0f ? 0.0f : (g - b) / d + 6.0f;
    else if (cmax == g)
        h = (b - r) / d + 2.0f;
    else
        h = (r - g) / d + 4.0f;
    if (h >= 6.0f) h -= 6.0f;
    h /= 6.0f;
}

// --- per-kind transfers ----------------------------------------------------

PITTORE_ADJUST_DEVICE float brightness_contrast_c(float v, float brightness,
                                                   float contrast) {
    return clamp01((v + brightness) * (1.0f + contrast));
}

PITTORE_ADJUST_DEVICE float levels_c(float v, float inBlack, float inWhite,
                                      float gamma, float outBlack,
                                      float outWhite) {
    const float span = inWhite - inBlack > 1e-6f ? inWhite - inBlack : 1e-6f;
    float t = clamp01((v - inBlack) / span);
    t = adj_pow(t, gamma > 0.0f ? 1.0f / gamma : 1.0f);
    return clamp01(outBlack + t * (outWhite - outBlack));
}

PITTORE_ADJUST_DEVICE float curve_lut_c(float v, const float* lut) {
    if (!lut) return clamp01(v);
    const float x = clamp01(v) * 255.0f + 0.5f;
    const int bin = static_cast<int>(x);
    return lut[bin < 0 ? 0 : (bin > 255 ? 255 : bin)];
}

PITTORE_ADJUST_DEVICE float exposure_c(float v, float stops) {
    return clamp01(v * adj_pow2(stops));
}

PITTORE_ADJUST_DEVICE void vibrance(float r, float g, float b, float amount,
                                     float& or_, float& og, float& ob) {
    const float l = luma709(r, g, b);
    const float cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const float span = cmax - cmin;
    const float amt = amount >= 0.0f ? 1.0f + amount * (1.0f - span)
                                     : 1.0f + amount;
    or_ = clamp01(l + (r - l) * amt);
    og = clamp01(l + (g - l) * amt);
    ob = clamp01(l + (b - l) * amt);
}

PITTORE_ADJUST_DEVICE void hue_saturation(float r, float g, float b,
                                           float hueShiftDeg, float satAdj,
                                           float lightAdj, float& or_,
                                           float& og, float& ob) {
    float h, s, l;
    rgb_to_hsl(r, g, b, h, s, l);
    h = frac_unit(h + hueShiftDeg / 360.0f + 1.0f);
    s = clamp01(s * (1.0f + satAdj));
    l = clamp01(l + lightAdj);
    hsl_to_rgb(h, s, l, or_, og, ob);
}

PITTORE_ADJUST_DEVICE float threshold_c(float v, float level) {
    return v >= level ? 1.0f : 0.0f;
}

PITTORE_ADJUST_DEVICE float posterize_c(float v, float levels) {
    const float n = levels < 2.0f ? 2.0f : levels;
    return clamp01(static_cast<float>(static_cast<int>(clamp01(v) * (n - 1.0f) +
                                                       0.5f)) /
                   (n - 1.0f));
}

// Triangular hue-sector mix, saturation-weighted; neutrals pass through.
PITTORE_ADJUST_DEVICE void black_white(float r, float g, float b,
                                        const float* w, float& gr, float& gg,
                                        float& gb) {
    float h, l;
    rgb_to_hue_light(r, g, b, h, l);
    const float cmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float cmin = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const float sat = cmax > 1e-6f ? (cmax - cmin) / cmax : 0.0f;
    const float hue = h * 6.0f;
    int i0 = static_cast<int>(hue);
    if (i0 < 0) i0 = 0;
    if (i0 > 5) i0 = 5;
    const int i1 = (i0 + 1) % 6;
    const float f = hue - static_cast<float>(i0);
    const float weight = w[i0] * (1.0f - f) + w[i1] * f;
    const float v = clamp01(l + (weight - 50.0f) / 100.0f * sat);
    gr = gg = gb = v;
}

PITTORE_ADJUST_DEVICE void channel_mixer(float r, float g, float b,
                                         const float* p, float& or_,
                                         float& og, float& ob) {
    const float rr = r * p[0] + g * p[1] + b * p[2] + p[9];
    const float gg = r * p[3] + g * p[4] + b * p[5] + p[10];
    const float bb = r * p[6] + g * p[7] + b * p[8] + p[11];
    if (p[12] > 0.5f) {
        const float v = clamp01(rr);
        or_ = og = ob = v;
        return;
    }
    or_ = clamp01(rr);
    og = clamp01(gg);
    ob = clamp01(bb);
}

// Per-range channel lifts under lightness zone masks; optional restore.
PITTORE_ADJUST_DEVICE void color_balance(float r, float g, float b,
                                         const float* p, float& or_,
                                         float& og, float& ob) {
    // Only lightness feeds the zone weights: skip hue branches + division.
    const float ls = rgb_to_lightness(r, g, b);
    const float ws = clamp01((0.5f - ls) * 2.0f);
    const float wh = clamp01((ls - 0.5f) * 2.0f);
    const float wm = clamp01(1.0f - absf(2.0f * ls - 1.0f));
    const float dr = (p[0] * ws + p[3] * wm + p[6] * wh) / 100.0f;
    const float dg = (p[1] * ws + p[4] * wm + p[7] * wh) / 100.0f;
    const float db = (p[2] * ws + p[5] * wm + p[8] * wh) / 100.0f;
    float tr = r + dr;
    float tg = g - dg;
    float tb = b - db;
    if (p[9] > 0.5f) {
        // Clamp before hue analysis; over-range lifts divide by zero below.
        float ht, st, lt;
        rgb_to_hsl(clamp01(tr), clamp01(tg), clamp01(tb), ht, st, lt);
        hsl_to_rgb(ht, st, ls, tr, tg, tb);
    }
    or_ = clamp01(tr);
    og = clamp01(tg);
    ob = clamp01(tb);
}

// Helland blackbody illuminant, green-normalized gains, green/magenta tint.
//
// The gains depend only on (tempK, tint) — constant across the millions of
// pixels in a rebuild — so the host path memoizes them in a thread-local
// slot: first pixel per worker computes (pow/log), the rest hit two float
// compares. Bit-identical (the cache stores computed values verbatim), and
// the device path is untouched, so CPU/GPU parity cannot drift.
#if !defined(__CUDACC__) && !defined(__HIPCC__)
struct WhiteBalanceCache {
    bool valid = false;
    float tempK = 0.0f, tint = 0.0f;
    float gr = 1.0f, gm = 1.0f, gb = 1.0f;
};
inline WhiteBalanceCache& white_balance_cache() {
    thread_local WhiteBalanceCache c;
    return c;
}
#endif
PITTORE_ADJUST_DEVICE void white_balance(float r, float g, float b,
                                          float tempK, float tint,
                                          float& or_, float& og, float& ob) {
#if !defined(__CUDACC__) && !defined(__HIPCC__)
    WhiteBalanceCache& cc = white_balance_cache();
    float gr, gm, gb;
    if (cc.valid && cc.tempK == tempK && cc.tint == tint) {
        gr = cc.gr;
        gm = cc.gm;
        gb = cc.gb;
    } else {
        const float t = clampf(tempK / 100.0f, 10.0f, 400.0f);
        float fr, fg, fb;
        fr = t <= 66.0f ? 1.0f
                        : clamp01(329.698727446f * adj_pow(t - 60.0f, -0.1332047592f) /
                                  255.0f);
        if (t <= 66.0f)
            fg = clamp01((99.4708025861f * adj_log(t) - 161.1195681661f) / 255.0f);
        else
            fg = clamp01(288.1221695283f * adj_pow(t - 60.0f, -0.0755148492f) /
                         255.0f);
        if (t >= 66.0f)
            fb = 1.0f;
        else if (t <= 19.0f)
            fb = 0.0f;
        else
            fb = clamp01((138.5177312231f * adj_log(t - 10.0f) - 305.0447927307f) /
                         255.0f);
        const float g0 = fg > 1e-3f ? fg : 1e-3f;
        gr = g0 / (fr > 1e-3f ? fr : 1e-3f);
        gb = g0 / (fb > 1e-3f ? fb : 1e-3f);
        gm = 1.0f - 0.5f * clampf(tint, -1.0f, 1.0f);
        cc.valid = true;
        cc.tempK = tempK;
        cc.tint = tint;
        cc.gr = gr;
        cc.gm = gm;
        cc.gb = gb;
    }
    or_ = clamp01(r * gr);
    og = clamp01(g * gm);
    ob = clamp01(b * gb);
    return;
}
#else
    const float t = clampf(tempK / 100.0f, 10.0f, 400.0f);
    float fr, fg, fb;
    fr = t <= 66.0f ? 1.0f
                    : clamp01(329.698727446f * adj_pow(t - 60.0f, -0.1332047592f) /
                              255.0f);
    if (t <= 66.0f)
        fg = clamp01((99.4708025861f * adj_log(t) - 161.1195681661f) / 255.0f);
    else
        fg = clamp01(288.1221695283f * adj_pow(t - 60.0f, -0.0755148492f) /
                     255.0f);
    if (t >= 66.0f)
        fb = 1.0f;
    else if (t <= 19.0f)
        fb = 0.0f;
    else
        fb = clamp01((138.5177312231f * adj_log(t - 10.0f) - 305.0447927307f) /
                     255.0f);
    const float g0 = fg > 1e-3f ? fg : 1e-3f;
    const float gr = g0 / (fr > 1e-3f ? fr : 1e-3f);
    const float gb = g0 / (fb > 1e-3f ? fb : 1e-3f);
    const float gm = 1.0f - 0.5f * clampf(tint, -1.0f, 1.0f);
    or_ = clamp01(r * gr);
    og = clamp01(g * gm);
    ob = clamp01(b * gb);
}
#endif

// Single hue at a single saturation, with the source's HSL lightness carrying
// the tone. This is what makes Recolour read as a duotone rather than a flat
// wash: black stays black, white stays white, and everything between picks up
// the target hue at the target saturation.
//
// Fit, not lifted: Affinity's own control semantics are Hue (0..360 degrees),
// Saturation (0..100%) and Lightness (0%, offset), and its default lands on
// hue 0 / saturation 100% / lightness 0. The tonal carrier here is plain HSL
// lightness, chosen because the existing hue_to_rgb path already defines the
// engine's HSL convention and keeps Recolour bit-consistent with
// hue_saturation.
PITTORE_ADJUST_DEVICE void recolour(float r, float g, float b, float hueDeg,
                                    float sat, float lightAdj, float& or_,
                                    float& og, float& ob) {
    const float l = clamp01(rgb_to_lightness(r, g, b) + lightAdj);
    float h = hueDeg / 360.0f;
    if (!(h >= 0.0f)) h = 0.0f;  // NaN guard: degenerate hue reads as 0
    if (h > 1.0f) h = 1.0f;
    hsl_to_rgb(h, clamp01(sat), l, or_, og, ob);
}

// Gel multiply (filter/luma), optional lightness restore, density mix.
PITTORE_ADJUST_DEVICE void photo_filter(float r, float g, float b,
                                         float fr, float fg, float fb,
                                         float density, float preserve,
                                         float& or_, float& og, float& ob) {
    const float lum_f = luma709(fr, fg, fb);
    const float inv = lum_f > 1e-3f ? 1.0f / lum_f : 1.0f;
    float tr = r * fr * inv;
    float tg = g * fg * inv;
    float tb = b * fb * inv;
    if (preserve > 0.5f) {
        // Clamp before hue analysis; over-range gels divide by zero below.
        float h, s, l;
        rgb_to_hsl(clamp01(tr), clamp01(tg), clamp01(tb), h, s, l);
        float hs, ss, ls;
        rgb_to_hsl(r, g, b, hs, ss, ls);
        hsl_to_rgb(h, s, ls, tr, tg, tb);
    }
    const float d = clamp01(density);
    or_ = clamp01(r + (tr - r) * d);
    og = clamp01(g + (tg - g) * d);
    ob = clamp01(b + (tb - b) * d);
}

// Apply one adjustment to straight RGB. `lut` is the 3x256-entry curve
// tables (Curves only, R/G/B back to back, master pre-folded; null =
// identity). Outputs may alias inputs.
PITTORE_ADJUST_DEVICE void apply(AdjustmentKind kind,
                                  const float* p, const float* lut, float r,
                                  float g, float b, float& or_, float& og,
                                  float& ob) {
    switch (kind) {
        case AdjustmentKind::BrightnessContrast:
            or_ = brightness_contrast_c(r, p[0], p[1]);
            og = brightness_contrast_c(g, p[0], p[1]);
            ob = brightness_contrast_c(b, p[0], p[1]);
            return;
        case AdjustmentKind::Levels:
            // Tabled when the caller prebuilt one (UI composite paths),
            // direct otherwise (tests, one-shots). The table matches direct
            // evaluation to ~1e-4 (1/255 sampling of a smooth map).
            if (lut) {
                or_ = curve_lut_c(r, lut);
                og = curve_lut_c(g, lut);
                ob = curve_lut_c(b, lut);
            } else {
                or_ = levels_c(r, p[0], p[1], p[2], p[3], p[4]);
                og = levels_c(g, p[0], p[1], p[2], p[3], p[4]);
                ob = levels_c(b, p[0], p[1], p[2], p[3], p[4]);
            }
            return;
        case AdjustmentKind::Curves:
            or_ = curve_lut_c(r, lut);
            og = curve_lut_c(g, lut ? lut + 256 : nullptr);
            ob = curve_lut_c(b, lut ? lut + 512 : nullptr);
            return;
        case AdjustmentKind::Exposure: {
            // Single pow2 per pixel instead of one per channel: identical
            // bits (same input → same libm result), a third of the cost.
            const float m = adj_pow2(p[0]);
            or_ = clamp01(r * m);
            og = clamp01(g * m);
            ob = clamp01(b * m);
            return;
        }
        case AdjustmentKind::Vibrance:
            vibrance(r, g, b, p[0], or_, og, ob);
            return;
        case AdjustmentKind::HueSaturation:
            hue_saturation(r, g, b, p[0], p[1], p[2], or_, og, ob);
            return;
        case AdjustmentKind::Invert:
            or_ = clamp01(1.0f - r);
            og = clamp01(1.0f - g);
            ob = clamp01(1.0f - b);
            return;
        case AdjustmentKind::Threshold: {
            const float l = luma709(r, g, b);
            const float v = threshold_c(l, p[0]);
            or_ = og = ob = v;
            return;
        }
        case AdjustmentKind::Posterize:
            or_ = posterize_c(r, p[0]);
            og = posterize_c(g, p[0]);
            ob = posterize_c(b, p[0]);
            return;
        case AdjustmentKind::PhotoFilter:
            photo_filter(r, g, b, p[0], p[1], p[2], p[3], p[4], or_, og, ob);
            return;
        case AdjustmentKind::WhiteBalance:
            white_balance(r, g, b, p[0], p[1], or_, og, ob);
            return;
        case AdjustmentKind::BlackWhite:
            black_white(r, g, b, p, or_, og, ob);
            return;
        case AdjustmentKind::ChannelMixer:
            channel_mixer(r, g, b, p, or_, og, ob);
            return;
        case AdjustmentKind::ColorBalance:
            color_balance(r, g, b, p, or_, og, ob);
            return;
        case AdjustmentKind::Recolour:
            recolour(r, g, b, p[0], p[1], p[2], or_, og, ob);
            return;
        case AdjustmentKind::None:
            or_ = r;
            og = g;
            ob = b;
            return;
    }
    or_ = r;
    og = g;
    ob = b;
}

}  // namespace adjust

}  // namespace pittore::compute
