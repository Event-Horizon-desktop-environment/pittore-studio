#pragma once
// Pigment-mix dab compositing: mean-family blend modes for brush dabs
// (watercolor-style glazing where overlaps average instead of covering).
// Original implementation from the textbook definitions of the means; the
// alpha shell follows the same source-over structure as the engine's W3C
// shell in blend.h, so results compose sanely with the rest of the app.
// Only Normal is wired into old call sites (bit-identical legacy path);
// mean ops are opt-in per dab via the trailing DabMixOp argument.
#include <cstring>

#include "engine/compute/blend.h"
#include "engine/core/pixel.h"

namespace pittore::compute {
namespace dabmix {

enum class DabMixOp {
    Normal = 0,
    GeometricMean,
    Parallel,
    Allanon,
};

// Per-channel mean F(s, d): s = dab color, d = backdrop, both in [0,1].
// Geometric: square root of the product (white guard keeps 1*1 exact).
// Parallel: harmonic mean 2*s*d/(s+d), zero when both are ~zero (avoids
// the 0/0 division). Allanon: plain arithmetic mean.
PITTORE_BLEND_DEVICE float mean_channel(DabMixOp op, float s, float d) {
    switch (op) {
        case DabMixOp::GeometricMean: {
            if (s >= 1.0f && d >= 1.0f) return 1.0f;
            const float p = s * d;
            return blend::bclamp(blend::bsqrt(p > 0.0f ? p : 0.0f));
        }
        case DabMixOp::Parallel: {
            const float denom = s + d;
            if (denom <= 1e-6f) return 0.0f;
            return blend::bclamp(2.0f * s * d / denom);
        }
        case DabMixOp::Allanon:
            return blend::bclamp(0.5f * (s + d));
        case DabMixOp::Normal:
        default:
            return s;
    }
}

// One dab texel of color (sr,sg,sb) at coverage alpha sa over backdrop
// (dr,dg,db,da). Same Porter-Duff/W3C shell as blend::pixel: transparent
// source keeps the backdrop; the blended color is weighed by both alphas.
// All inputs are read before any output is written, so outputs may alias
// the backdrop planes.
PITTORE_BLEND_DEVICE void mix_pixel(DabMixOp op, float sr, float sg, float sb,
                                    float sa, float dr, float dg, float db,
                                    float da, float& or_, float& og, float& ob,
                                    float& oa) {
    if (sa <= 0.0f) {
        or_ = dr;
        og = dg;
        ob = db;
        oa = da;
        return;
    }
    const float blr = mean_channel(op, sr, dr);
    const float blg = mean_channel(op, sg, dg);
    const float blb = mean_channel(op, sb, db);
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

// Preset/bundle string ids. Unknown ids fall back to Normal, the same
// fallback the PSD/AF importers use for blend names.
inline const char* mix_id(DabMixOp op) {
    switch (op) {
        case DabMixOp::GeometricMean:
            return "geometric_mean";
        case DabMixOp::Parallel:
            return "parallel";
        case DabMixOp::Allanon:
            return "allanon";
        case DabMixOp::Normal:
        default:
            return "normal";
    }
}

inline DabMixOp mix_from_id(const char* id) {
    if (!id) return DabMixOp::Normal;
    if (std::strcmp(id, "geometric_mean") == 0) return DabMixOp::GeometricMean;
    if (std::strcmp(id, "parallel") == 0) return DabMixOp::Parallel;
    if (std::strcmp(id, "allanon") == 0) return DabMixOp::Allanon;
    return DabMixOp::Normal;
}

}  // namespace dabmix
}  // namespace pittore::compute
