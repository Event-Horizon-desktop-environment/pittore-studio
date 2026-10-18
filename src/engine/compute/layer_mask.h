#pragma once
// Batched masked/clipped placed-layer composite shared by the CPU reference
// path and the GPU kernels' host side.
//
// A layer mask is opaque grey RGBAf coverage: r=g=b=coverage with a=1. The
// placed sampler averages premultiplied taps, so reading coverage from R
// gives the correctly weighted mean. Coverage must not be alpha-carried:
// with coverage in alpha the accumulator would average cov²/cov instead of
// cov.
//
// Clip chains use conventional clipping groups: a clipped layer multiplies
// its alpha by the post-mask/fold alpha of its own base layer (`clipBase`).
// Clipped layers never become coverage for another layer.

#include <cstddef>
#include <cstdint>

#include "engine/compute/paint.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

// One opaque-grey coverage texel. Values outside [0,1] are clamped.
inline RGBAf make_mask_pixel(float coverage) {
    const float c = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
    return RGBAf{c, c, c, 1.0f};
}

// One batched layer for the host reference walk. `src` and `mask` point at
// row-major RGBAf pixels owned by the caller for the duration of the call.
struct HostPlacedLayer {
    const RGBAf* src = nullptr;
    std::uint32_t sw = 0, sh = 0;
    double ox = 0.0, oy = 0.0;
    double sx = 1.0, sy = 1.0;
    std::uint32_t x0 = 0, y0 = 0;
    std::uint32_t x1 = 0, y1 = 0;
    float fold = 1.0f;
    BlendMode mode = BlendMode::Normal;
    const RGBAf* mask = nullptr;
    std::uint32_t msw = 0, msh = 0;
    double mox = 0.0, moy = 0.0;
    double msx = 1.0, msy = 1.0;
    bool clipped = false;
    int clipBase = -1;
    // Live adjustment layer: no sampled source; the walk adjusts the current
    // accumulator instead. `adjAux` borrows 3x256 curve-LUT floats (R/G/B back
    // to back) for Curves.
    bool isAdjustment = false;
    int adjKind = 0;
    float adjP[16] = {};
    const float* adjAux = nullptr;
    // Solid fill (D1): constant `fillColor` over the window instead of `src`.
    bool isFill = false;
    RGBAf fillColor;
};

// Sample mask coverage at a document pixel. A null/invalid mask reveals
// everything (coverage 1), matching a layer without a mask.
float mask_coverage_host(const RGBAf* mask, std::uint32_t msw,
                         std::uint32_t msh, double docX, double docY,
                         double mox, double moy, double msx, double msy);

// Paint one circular dab into an opaque-grey mask. `value` is the coverage
// the stroke moves toward (0 hides, 1 reveals); dab alpha interpolates the
// old coverage toward it. Geometry, hardness falloff and selection handling
// match paint_dab_host so mask strokes feel like pixel strokes.
void mask_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, float cx,
                   float cy, float radius, float hardness, float opacity,
                   float value, const SelectionMask* selection = nullptr);

// Bottom→top batched composite over [rx0,rx1) × [ry0,ry1). For every pixel,
// each covering layer is sampled, multiplied by its mask coverage, folded by
// opacity×fill, multiplied by the active base coverage when clipped, and
// blended. `bottom` must hold w*h pixels.
void composite_many_host(RGBAf* bottom, std::uint32_t w, std::uint32_t h,
                         std::uint32_t rx0, std::uint32_t ry0,
                         std::uint32_t rx1, std::uint32_t ry1,
                         const HostPlacedLayer* layers, std::size_t count);

}  // namespace pittore::compute
