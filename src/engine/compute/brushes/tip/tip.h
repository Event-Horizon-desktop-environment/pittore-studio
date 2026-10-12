#pragma once
// Brush tip shape model: an "auto" tip generated from parameters (circle or
// square, stretched by ratio and rotated by angle). Spacing is a percentage
// of the tip diameter between dabs along a stroke. Extra shape fields are
// original additions (defaults preserve the legacy round tip exactly):
// spikes star-lobes, fadeAniso H/V hardness split, falloff linear/gaussian,
// sharpness threshold + soften edge, autoSpacing quadratic stride.
#include <algorithm>
#include <cmath>

namespace pittore::compute {

// Which silhouette the auto tip generates.
enum class TipSilhouette : int {
    Round = 0,
    Square = 1,
};

struct AutoTip {
    TipSilhouette silhouette = TipSilhouette::Round;
    float ratio = 1.0f;      // minor/major axis, (0,1]; 1 == circle
    float angleDeg = 0.0f;   // rotation of the major axis, degrees
    float hardness = 0.5f;   // 0 soft .. 1 hard
    float spacingPct = 15.0f;  // % of diameter between dabs along a stroke
    int spikes = 0;          // star lobes: 0/1 off, 2..12 star (round only)
    float fadeAniso = 0.0f;  // H/V hardness split: -1..1, 0 isotropic
    int falloff = 0;         // edge curve: 0 linear, 1 gaussian
    float sharpness = 0.0f;  // threshold 0 off .. 1 full cut
    float soften = 0.0f;     // threshold edge width 0 hard .. 1 soft
    bool autoSpacing = false;  // quadratic stride for fine low-end control
    // Size-law stride coefficient (0 = legacy quadratic above). When > 0,
    // auto spacing strides coeff*(d<1 ? d : sqrt(d)) on the dab diameter,
    // ignoring the percentage: dense sub-pixel strides for large soft tips.
    float spacingAutoCoeff = 0.0f;

    void sanitize() {
        ratio = std::clamp(ratio, 0.01f, 1.0f);
        spacingPct = std::clamp(spacingPct, 1.0f, 200.0f);
        if (!(spacingAutoCoeff >= 0.0f) || spacingAutoCoeff > 8.0f)
            spacingAutoCoeff = 0.0f;
        hardness = std::clamp(hardness, 0.0f, 1.0f);
        spikes = std::clamp(spikes, 0, 12);
        fadeAniso = std::clamp(fadeAniso, -1.0f, 1.0f);
        if (!std::isfinite(fadeAniso)) fadeAniso = 0.0f;
        falloff = (falloff == 1) ? 1 : 0;
        sharpness = std::clamp(sharpness, 0.0f, 1.0f);
        soften = std::clamp(soften, 0.0f, 1.0f);
        if (!std::isfinite(angleDeg)) angleDeg = 0.0f;
        // Wrap to (-180,180] so presets compare canonically.
        while (angleDeg > 180.0f) angleDeg -= 360.0f;
        while (angleDeg <= -180.0f) angleDeg += 360.0f;
    }
};

// Size-law stride on a dab diameter: linear below one pixel, square-root
// above, scaled by coeff. Gives large soft tips dense sub-pixel strides
// where a fixed percentage would dot.
inline float auto_spacing_stride(float diameter, float coeff) {
    const float d = diameter < 0.0f ? 0.0f : diameter;
    const float base = d < 1.0f ? d : std::sqrt(d);
    return coeff * base;
}

// Spacing in doc units for a dab of `radius` under this tip. Auto spacing
// squares the percentage (15% -> 2.25%) for fine low-end control, unless a
// size-law coefficient is set (see above).
inline float tip_spacing(float radius, const AutoTip& tip) {
    const float diameter = 2.0f * std::max(0.0f, radius);
    if (tip.autoSpacing && tip.spacingAutoCoeff > 0.0f)
        return std::max(0.5f, auto_spacing_stride(diameter,
                                                 tip.spacingAutoCoeff));
    float pct = std::clamp(tip.spacingPct, 1.0f, 200.0f);
    if (tip.autoSpacing) pct = std::clamp(pct * pct / 100.0f, 0.5f, 400.0f);
    return std::max(0.5f, diameter * pct / 100.0f);
}

// Coverage 0..1 for a dab-geometry offset (dx, dy) at 1/radius scale.
// Shared by paint/erase/smudge auto tips so shapes never diverge.
// cosA/sinA precompute the -angle rotation, invRatio = 1/ratio.
inline float auto_tip_coverage(float dx, float dy, float inv_r,
                               const AutoTip& t, float cosA, float sinA,
                               float invRatio) {
    const float u = dx * cosA - dy * sinA;
    const float v = dx * sinA + dy * cosA;
    const bool square = t.silhouette == TipSilhouette::Square;
    float tn;
    if (square) {
        tn = std::max(std::fabs(u), std::fabs(v) * invRatio) * inv_r;
    } else {
        tn = std::sqrt(u * u + v * v * invRatio * invRatio) * inv_r;
    }
    if (tn >= 1.0f) return 0.0f;
    // Star lobes (round only): boundary radius swings with angle between
    // ratio (valley) and 1 (peak), so spikes interact with roundness.
    if (!square && t.spikes >= 2) {
        const float theta = std::atan2(v, u);
        const float lobe =
            0.5f + 0.5f * std::cos(float(t.spikes) * theta);
        const float rBound =
            std::max(1.0f - (1.0f - t.ratio) * lobe, 1e-4f);
        tn /= rBound;
        if (tn >= 1.0f) return 0.0f;
    }
    // Directional hardness: H along u, V along v, split by fadeAniso.
    const float hardH =
        std::clamp(t.hardness + t.fadeAniso * 0.5f, 0.0f, 1.0f);
    const float hardV =
        std::clamp(t.hardness - t.fadeAniso * 0.5f, 0.0f, 1.0f);
    float cov;
    if (t.falloff == 1) {
        // Gaussian, normalized to 1 at center and 0 at rim. Stiffness
        // follows the directional hardness so aniso still shapes it.
        const float denom = u * u + v * v;
        float w = 0.5f;
        if (denom > 1e-12f) w = (u * u) / denom;
        const float hard = hardH * w + hardV * (1.0f - w);
        const float k = 1.0f + hard * 4.0f;
        const float e = std::exp(-k * tn * tn);
        const float e1 = std::exp(-k);
        cov = (e - e1) / std::max(1.0f - e1, 1e-4f);
    } else if (t.fadeAniso == 0.0f) {
        // Legacy fast path: bit-exact with the original linear kernel.
        const float span = std::max(1.0f - t.hardness, 1e-4f);
        cov = 1.0f;
        if (tn > 1.0f - span) cov = (1.0f - tn) / span;
    } else {
        const float denom = u * u + v * v;
        float w = 0.5f;
        if (denom > 1e-12f) w = (u * u) / denom;
        const float spanH = std::max(1.0f - hardH, 1e-4f);
        const float spanV = std::max(1.0f - hardV, 1e-4f);
        const float span = spanH * w + spanV * (1.0f - w);
        cov = 1.0f;
        if (tn > 1.0f - span) cov = (1.0f - tn) / span;
    }
    // Sharpness threshold over the mask: cuts the soft edge into a
    // pixel-art step (soften 0) or a smoothed step (soften > 0).
    if (t.sharpness > 0.0f) {
        const float th = std::clamp(t.sharpness, 0.0f, 1.0f);
        const float wEdge = std::clamp(t.soften, 0.0f, 1.0f) * 0.5f;
        if (wEdge < 1e-4f) {
            cov = (cov >= th) ? 1.0f : 0.0f;
        } else {
            const float lo = th - wEdge * 0.5f, hi = th + wEdge * 0.5f;
            if (cov <= lo)
                cov = 0.0f;
            else if (cov >= hi)
                cov = 1.0f;
            else {
                const float tt = (cov - lo) / std::max(hi - lo, 1e-6f);
                cov = tt * tt * (3.0f - 2.0f * tt);
            }
        }
    }
    return std::clamp(cov, 0.0f, 1.0f);
}

}  // namespace pittore::compute
