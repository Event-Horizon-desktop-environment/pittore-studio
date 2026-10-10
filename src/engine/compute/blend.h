#pragma once
// PSD-compatible blend math, shared by CPU + CUDA/HIP kernels.
// One source of truth: parity tests hold CPU and GPU equal (float tolerance).
//
// Follows W3C compositing: co·αo = Cs·αs·(1−αb) + B·αs·αb + Cb·αb·(1−αs)
// Straight f32 in/out; premultiplied math hides inside. Separable modes run
// B() per channel, clamped; Hue/Sat/Color/Lum + Darker/Lighter Color work the
// whole triple.
//
// Dissolve is the only position-dependent mode: a fixed hash of doc position
// vs source alpha, so every backend + repaint region agrees.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/core/pixel.h"

// nvcc has __forceinline__ built in; hipcc only after HIP headers, so HIP
// uses __device__ inline here. Host gets plain inline.
#if defined(__CUDACC__)
#define PITTORE_BLEND_DEVICE __device__ __forceinline__
#elif defined(__HIPCC__)
#define PITTORE_BLEND_DEVICE __device__ inline
#else
#define PITTORE_BLEND_DEVICE inline
#endif

namespace pittore::compute {

// The standard 27 modes + Pass Through (group-only, blends like Normal).
// Normal/Multiply keep 0/1 (kernels serialise via static_cast<int>);
// rest follow UI order.
enum class BlendMode {
    Normal = 0,
    Multiply = 1,
    Dissolve,
    Darken,
    ColorBurn,
    LinearBurn,
    DarkerColor,
    Lighten,
    Screen,
    ColorDodge,
    LinearDodge,  // UI / import name: "Linear Dodge (Add)"
    LighterColor,
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    Difference,
    Exclusion,
    Subtract,
    Divide,
    Hue,
    Saturation,
    Color,
    Luminosity,
    PassThrough,
};

namespace blend {

// ---------------------------------------------------------------------------
// Small scalar helpers. Written as ternaries / plain C math so the exact same
// bodies compile unchanged on the CPU (gcc/clang) and device (nvcc/hipcc).
// ---------------------------------------------------------------------------

PITTORE_BLEND_DEVICE float bmin(float a, float b) { return a < b ? a : b; }
PITTORE_BLEND_DEVICE float bmax(float a, float b) { return a > b ? a : b; }
PITTORE_BLEND_DEVICE float babs(float v) { return v < 0.0f ? -v : v; }
PITTORE_BLEND_DEVICE float bclamp(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
PITTORE_BLEND_DEVICE float bsqrt(float v) {
#if defined(__CUDACC__) || defined(__HIPCC__)
    return sqrtf(v);
#else
    return std::sqrt(v);
#endif
}

// ---------------------------------------------------------------------------
// Per-channel separable helpers (W3C compositing §5.1) — b = backdrop, s =
// source, per channel.
// ---------------------------------------------------------------------------

PITTORE_BLEND_DEVICE float screen_c(float b, float s) { return b + s - b * s; }

PITTORE_BLEND_DEVICE float hard_light_c(float b, float s) {
    return s <= 0.5f ? b * (2.0f * s) : screen_c(b, 2.0f * s - 1.0f);
}

PITTORE_BLEND_DEVICE float color_dodge_c(float b, float s) {
    if (b <= 0.0f) return 0.0f;
    if (s >= 1.0f) return 1.0f;
    return bmin(b / (1.0f - s), 1.0f);
}

PITTORE_BLEND_DEVICE float color_burn_c(float b, float s) {
    if (b >= 1.0f) return 1.0f;
    if (s <= 0.0f) return 0.0f;
    return 1.0f - bmin((1.0f - b) / s, 1.0f);
}

PITTORE_BLEND_DEVICE float soft_light_c(float b, float s) {
    if (s <= 0.5f)
        return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    const float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : bsqrt(b);
    return b + (2.0f * s - 1.0f) * (d - b);
}

// One separable channel of B(Cb, Cs); the spec clamps the result to [0,1].
PITTORE_BLEND_DEVICE float separable(BlendMode mode, float b, float s) {
    float v;
    switch (mode) {
        case BlendMode::Normal:
        case BlendMode::PassThrough:
        case BlendMode::Dissolve: v = s; break;
        case BlendMode::Darken: v = bmin(b, s); break;
        case BlendMode::Multiply: v = b * s; break;
        case BlendMode::ColorBurn: v = color_burn_c(b, s); break;
        case BlendMode::LinearBurn: v = b + s - 1.0f; break;
        case BlendMode::Lighten: v = bmax(b, s); break;
        case BlendMode::Screen: v = screen_c(b, s); break;
        case BlendMode::ColorDodge: v = color_dodge_c(b, s); break;
        case BlendMode::LinearDodge: v = b + s; break;
        case BlendMode::Overlay: v = hard_light_c(s, b); break;
        case BlendMode::SoftLight: v = soft_light_c(b, s); break;
        case BlendMode::HardLight: v = hard_light_c(b, s); break;
        case BlendMode::VividLight:
            v = s <= 0.5f ? color_burn_c(b, 2.0f * s)
                          : color_dodge_c(b, 2.0f * s - 1.0f);
            break;
        case BlendMode::LinearLight: v = b + 2.0f * s - 1.0f; break;
        case BlendMode::PinLight:
            v = s <= 0.5f ? bmin(b, 2.0f * s) : bmax(b, 2.0f * s - 1.0f);
            break;
        case BlendMode::HardMix: v = (b + s >= 1.0f) ? 1.0f : 0.0f; break;
        case BlendMode::Difference: v = babs(b - s); break;
        case BlendMode::Exclusion: v = b + s - 2.0f * b * s; break;
        case BlendMode::Subtract: v = b - s; break;
        case BlendMode::Divide: v = s <= 0.0f ? 1.0f : b / s; break;
        // Full-colour modes are handled by stitch_color() below.
        case BlendMode::Hue:
        case BlendMode::Saturation:
        case BlendMode::Color:
        case BlendMode::Luminosity:
        case BlendMode::DarkerColor:
        case BlendMode::LighterColor:
        default: v = s; break;
    }
    return bclamp(v);
}

// ---------------------------------------------------------------------------
// Non-separable (HSL family) helpers (W3C compositing §5.2). Note the three
// coefficients are the spec's luma weights (0.30/0.59/0.11), deliberately
// distinct from the Rec.709 luma the rest of the engine uses.
// ---------------------------------------------------------------------------

PITTORE_BLEND_DEVICE float clum(float r, float g, float b) {
    return 0.30f * r + 0.59f * g + 0.11f * b;
}

PITTORE_BLEND_DEVICE float csat(float r, float g, float b) {
    return bmax(r, bmax(g, b)) - bmin(r, bmin(g, b));
}

// Clip out-of-gamut channel values back onto the luminance axis (spec §5.2.1).
PITTORE_BLEND_DEVICE void clip_color(float r, float g, float b,
                                      float& or_, float& og, float& ob) {
    const float l = clum(r, g, b);
    const float n = bmin(r, bmin(g, b));
    const float x = bmax(r, bmax(g, b));
    float cr = r, cg = g, cb = b;
    if (n < 0.0f) {
        const float k = l / (l - n);
        cr = l + (cr - l) * k;
        cg = l + (cg - l) * k;
        cb = l + (cb - l) * k;
    }
    if (x > 1.0f) {
        const float k = (1.0f - l) / (x - l);
        cr = l + (cr - l) * k;
        cg = l + (cg - l) * k;
        cb = l + (cb - l) * k;
    }
    or_ = cr;
    og = cg;
    ob = cb;
}

PITTORE_BLEND_DEVICE void set_lum(float r, float g, float b, float l,
                                   float& or_, float& og, float& ob) {
    const float d = l - clum(r, g, b);
    clip_color(r + d, g + d, b + d, or_, og, ob);
}

// Scale the saturated channel range of a colour to s, keeping the middle
// channel's relative position. Tie-broken min/max picks are value-identical
// (equal channels produce the same output whichever slot they land in).
PITTORE_BLEND_DEVICE void set_sat(float r, float g, float b, float s,
                                   float& or_, float& og, float& ob) {
    int imin = 0, imax = 0;
    if (g < r) imin = 1;
    if (b < (imin == 1 ? g : r)) imin = 2;
    if (g > r) imax = 1;
    if (b > (imax == 1 ? g : r)) imax = 2;
    const float v[3] = {r, g, b};
    const int imid = 3 - imin - imax;
    or_ = og = ob = 0.0f;
    if (v[imax] > v[imin]) {
        float& midv = (imid == 0) ? or_ : (imid == 1 ? og : ob);
        midv = (v[imid] - v[imin]) * s / (v[imax] - v[imin]);
        float& maxv = (imax == 0) ? or_ : (imax == 1 ? og : ob);
        maxv = s;
    }
}

// Full-colour blend B(Cb, Cs). The separable modes reuse the channel
// function; the HSL family and Darker/Lighter Color pick or construct a
// whole colour instead.
PITTORE_BLEND_DEVICE void stitch_color(BlendMode mode, float br, float bg,
                                        float bb, float sr, float sg, float sb,
                                        float& or_, float& og, float& ob) {
    switch (mode) {
        case BlendMode::Hue: {
            float h1, h2, h3;
            set_sat(sr, sg, sb, csat(br, bg, bb), h1, h2, h3);
            set_lum(h1, h2, h3, clum(br, bg, bb), or_, og, ob);
            break;
        }
        case BlendMode::Saturation: {
            float h1, h2, h3;
            set_sat(br, bg, bb, csat(sr, sg, sb), h1, h2, h3);
            set_lum(h1, h2, h3, clum(br, bg, bb), or_, og, ob);
            break;
        }
        case BlendMode::Color: {
            set_lum(sr, sg, sb, clum(br, bg, bb), or_, og, ob);
            break;
        }
        case BlendMode::Luminosity: {
            set_lum(br, bg, bb, clum(sr, sg, sb), or_, og, ob);
            break;
        }
        case BlendMode::DarkerColor: {
            if (clum(sr, sg, sb) < clum(br, bg, bb)) {
                or_ = sr;
                og = sg;
                ob = sb;
            } else {
                or_ = br;
                og = bg;
                ob = bb;
            }
            break;
        }
        case BlendMode::LighterColor: {
            if (clum(sr, sg, sb) > clum(br, bg, bb)) {
                or_ = sr;
                og = sg;
                ob = sb;
            } else {
                or_ = br;
                og = bg;
                ob = bb;
            }
            break;
        }
        default: {
            or_ = separable(mode, br, sr);
            og = separable(mode, bg, sg);
            ob = separable(mode, bb, sb);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Dissolve
// ---------------------------------------------------------------------------

// Fixed per-pixel hash in [0,1). Dissolve shows the source opaque wherever the
// source alpha exceeds the hash, so any given document dissolves identically
// on every backend and repaint region.
PITTORE_BLEND_DEVICE float dissolve_hash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B9u ^
                      static_cast<std::uint32_t>(y) * 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.0f;  // 2^24
}

// ---------------------------------------------------------------------------
// The per-pixel composite — the one function every path funnels through.
// ---------------------------------------------------------------------------

// Blend `src` over `dst` with `mode`, writing the result into the out params.
// `x`/`y` are document coordinates (only Dissolve reads them; every other mode
// is coordinate-independent, which is what keeps region and placed repaints
// pixel-identical to full recomposites). All inputs are read before any output
// is written, so the outputs may alias the inputs.
PITTORE_BLEND_DEVICE void pixel(BlendMode mode, float sr, float sg, float sb,
                                 float sa, float dr, float dg, float db,
                                 float da, int x, int y, float& or_,
                                 float& og, float& ob, float& oa) {
    if (mode == BlendMode::Dissolve) {
        // Revealed texels draw the source opaque; everything else keeps the
        // backdrop whole (equivalent to source-over with alpha 1.0).
        if (sa > dissolve_hash(x, y)) {
            or_ = sr;
            og = sg;
            ob = sb;
            oa = 1.0f;
        } else {
            or_ = dr;
            og = dg;
            ob = db;
            oa = da;
        }
        return;
    }
    if (sa <= 0.0f) {
        // Fully transparent source: the backdrop is returned untouched.
        or_ = dr;
        og = dg;
        ob = db;
        oa = da;
        return;
    }
    float blr, blg, blb;
    stitch_color(mode, dr, dg, db, sr, sg, sb, blr, blg, blb);
    const float ao = sa + da * (1.0f - sa);
    if (ao <= 0.0f) {
        or_ = 0.0f;
        og = 0.0f;
        ob = 0.0f;
        oa = 0.0f;
        return;
    }
    const float inv = 1.0f / ao;
    or_ = (sr * sa * (1.0f - da) + blr * sa * da + dr * da * (1.0f - sa)) * inv;
    og = (sg * sa * (1.0f - da) + blg * sa * da + dg * da * (1.0f - sa)) * inv;
    ob = (sb * sa * (1.0f - da) + blb * sa * da + db * da * (1.0f - sa)) * inv;
    oa = ao;
}

// ---------------------------------------------------------------------------
// Host-only full-buffer driver (defined in blend.cpp — the GPU full-buffer
// kernel mirrors it directly).
// ---------------------------------------------------------------------------

// Composite `src` over `dst` for `n` pixels of a `w`-wide row-major image in
// place. Callers pick `mode` once; Dissolve needs the per-pixel document
// coordinates, which we derive exactly as the GPU kernel does (x = i % w).
void composite_buffer(RGBAf* dst, const RGBAf* src, std::size_t n,
                      BlendMode mode, std::size_t w);

// ---------------------------------------------------------------------------
// Display-name mapping (host code only — the kernels dispatch on the enum).
// ---------------------------------------------------------------------------

struct ModeEntry {
    BlendMode mode;
    const char* name;
};

inline constexpr ModeEntry kModeNames[] = {
    {BlendMode::Normal, "Normal"},
    {BlendMode::Dissolve, "Dissolve"},
    {BlendMode::Darken, "Darken"},
    {BlendMode::Multiply, "Multiply"},
    {BlendMode::ColorBurn, "Color Burn"},
    {BlendMode::LinearBurn, "Linear Burn"},
    {BlendMode::DarkerColor, "Darker Color"},
    {BlendMode::Lighten, "Lighten"},
    {BlendMode::Screen, "Screen"},
    {BlendMode::ColorDodge, "Color Dodge"},
    {BlendMode::LinearDodge, "Linear Dodge (Add)"},
    {BlendMode::LighterColor, "Lighter Color"},
    {BlendMode::Overlay, "Overlay"},
    {BlendMode::SoftLight, "Soft Light"},
    {BlendMode::HardLight, "Hard Light"},
    {BlendMode::VividLight, "Vivid Light"},
    {BlendMode::LinearLight, "Linear Light"},
    {BlendMode::PinLight, "Pin Light"},
    {BlendMode::HardMix, "Hard Mix"},
    {BlendMode::Difference, "Difference"},
    {BlendMode::Exclusion, "Exclusion"},
    {BlendMode::Subtract, "Subtract"},
    {BlendMode::Divide, "Divide"},
    {BlendMode::Hue, "Hue"},
    {BlendMode::Saturation, "Saturation"},
    {BlendMode::Color, "Color"},
    {BlendMode::Luminosity, "Luminosity"},
    {BlendMode::PassThrough, "Pass Through"},
};

inline const char* display_name(BlendMode m) {
    for (const ModeEntry& e : kModeNames)
        if (e.mode == m) return e.name;
    return "Normal";
}

// Case-sensitive lookup of the UI/import display names. Unknown names map to
// Normal, the same fallback the PSD/AF importers use.
inline BlendMode from_display_name(const char* name) {
    if (!name) return BlendMode::Normal;
    for (const ModeEntry& e : kModeNames)
        if (std::strcmp(e.name, name) == 0) return e.mode;
    return BlendMode::Normal;
}

}  // namespace blend
}  // namespace pittore::compute