#pragma once
// Paper grain for brush dabs: a tiled grayscale pattern modulates dab
// coverage for watercolor-style paper tooth. Original implementation
// from observed parameter behavior; where finer distinctions exist
// (hard/soft strength, emboss height modes), this file documents the
// approximation next to the code.
// Pipeline per texel, in order: tile sample -> invert -> subtractive
// brightness (larger darkens) -> mid-gray-pivot contrast -> neutral-point
// piecewise remap -> cutoff window -> strength combine. Strength is exact
// at both ends (0 = no-op, 1 = full combine) and interpolates between.
#include <cstdint>

#if defined(__CUDACC__)
#define PITTORE_GRAIN_DEVICE __device__ __forceinline__
#elif defined(__HIPCC__)
#define PITTORE_GRAIN_DEVICE __device__ inline
#else
#define PITTORE_GRAIN_DEVICE inline
#endif

namespace pittore::compute {
namespace papergrain {

// Grain combine mode: 0 multiply, 1 subtract, 2 darken (minimum).
enum class GrainMode {
    Multiply = 0,
    Subtract = 1,
    Darken = 2,
};

struct PaperGrain {
    const float* gray = nullptr;  // w*h luma in [0,1], row-first, tiled
    std::uint32_t w = 0, h = 0;
    float strength = 0.0f;  // 0 = off, 1 = full grain
    float scale = 1.0f;     // pattern px per target px
    float offsetX = 0.0f, offsetY = 0.0f;
    float neutral = 0.5f;   // neutral-point remap pivot
    float brightness = 0.0f;  // subtractive shift (larger darkens)
    float contrast = 1.0f;    // spread around mid gray
    bool invert = false;
    GrainMode mode = GrainMode::Multiply;
    int cutoffPolicy = 0;  // 0 off, 1 outside-window kills, 2 keeps
    float cutLo = 0.0f, cutHi = 1.0f;

    bool valid() const {
        return gray != nullptr && w > 0 && h > 0 && w <= 1024 && h <= 1024 &&
               strength > 0.0f && scale > 0.0f;
    }
};

PITTORE_GRAIN_DEVICE float grain_clamp(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Fractional part in [0,1) without lib calls (device-safe).
PITTORE_GRAIN_DEVICE float grain_fract(float v) {
    int qi = (int)v;
    if ((float)qi > v) --qi;
    return v - (float)qi;
}

// Pattern texel at target-space (x, y) through levels + cutoff window.
// Returns the grain value in [0,1]; 1 means "no thinning".
PITTORE_GRAIN_DEVICE float grain_sample(const PaperGrain* t, float x,
                                        float y) {
    const float s = t->scale;
    const float u = (x + t->offsetX) / s;
    const float v = (y + t->offsetY) / s;
    const float W = (float)t->w, H = (float)t->h;
    float fu = grain_fract(u / W) * W;
    float fv = grain_fract(v / H) * H;
    std::uint32_t xi = (std::uint32_t)fu;
    std::uint32_t yi = (std::uint32_t)fv;
    if (xi >= t->w) xi = t->w - 1;
    if (yi >= t->h) yi = t->h - 1;
    float g = t->gray[(std::size_t)yi * t->w + xi];
    if (t->invert) g = 1.0f - g;
    g = grain_clamp(g - t->brightness);
    g = grain_clamp((g - 0.5f) * t->contrast + 0.5f);
    float n = t->neutral;
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    float g2;
    if (n >= 1.0f) {
        g2 = g * 0.5f;
    } else if (n <= 0.0f) {
        g2 = 0.5f + g * 0.5f;
    } else if (g <= n) {
        g2 = g / (2.0f * n);
    } else {
        g2 = 0.5f + (g - n) / (2.0f - 2.0f * n);
    }
    g = grain_clamp(g2);
    if (t->cutoffPolicy == 1) {
        if (g < t->cutLo || g > t->cutHi) return 0.0f;
    } else if (t->cutoffPolicy == 2) {
        if (g < t->cutLo || g > t->cutHi) return 1.0f;
    }
    return g;
}

// Coverage multiplier for dab coverage `cov` under grain `g` at strength
// `st`. Exact at the ends (st 0 = cov untouched, st 1 = full combine).
PITTORE_GRAIN_DEVICE float grain_combine(float cov, float g, float st,
                                         GrainMode mode) {
    if (st <= 0.0f) return cov;
    if (st > 1.0f) st = 1.0f;
    switch (mode) {
        case GrainMode::Subtract: {
            const float r = cov - st * g;
            return r > 0.0f ? r : 0.0f;
        }
        case GrainMode::Darken: {
            const float m = 1.0f - st + st * g;
            return cov < m ? cov : m;
        }
        case GrainMode::Multiply:
        default: {
            const float m = 1.0f - st + st * g;
            return cov * m;
        }
    }
}

// Full per-pixel grain multiplier. Null/invalid grain passes through.
PITTORE_GRAIN_DEVICE float grain_mult(const PaperGrain* t, float cov, float x,
                                      float y) {
    if (t == nullptr || !t->valid() || cov <= 0.0f) return cov;
    const float g = grain_sample(t, x, y);
    return grain_combine(cov, g, t->strength, t->mode);
}

}  // namespace papergrain
}  // namespace pittore::compute
