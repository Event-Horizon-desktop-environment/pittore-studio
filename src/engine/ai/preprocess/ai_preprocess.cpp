#include "engine/ai/bg_remove.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine/core/log.h"

#ifdef PITTORE_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#include "engine/ai/shared/ai_session.h"
#include "engine/ai/shared/ai_preprocess.h"

namespace pittore::ai {
namespace ai_detail {

#ifdef PITTORE_HAS_ONNX
// ImageNet mean/std used by rembg's BaseSession for every family in the
// catalogue (u2net / isnet / birefnet all consume the same normalisation).
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

void resizeRgba8ToNchwRgb(const std::uint8_t* src, int w, int h, float* dst, int S,
                          bool normalize) {
    for (int y = 0; y < S; ++y) {
        const double fy = (h > 1) ? (y + 0.5) * h / S - 0.5 : 0.0;
        const double fyf = std::floor(fy);
        const int y0 = std::clamp(int(fyf), 0, h - 1);
        const int y1 = std::clamp(std::min(int(fyf) + 1, h - 1), 0, h - 1);
        const float wy = static_cast<float>(fy - fyf);
        const float wy0 = 1.0f - wy, wy1 = wy;
        for (int x = 0; x < S; ++x) {
            const double fx = (w > 1) ? (x + 0.5) * w / S - 0.5 : 0.0;
            const double fxf = std::floor(fx);
            const int x0 = std::clamp(int(fxf), 0, w - 1);
            const int x1 = std::clamp(std::min(int(fxf) + 1, w - 1), 0, w - 1);
            const float wx = static_cast<float>(fx - fxf);
            const float wx0 = 1.0f - wx, wx1 = wx;
            for (int c = 0; c < 3; ++c) {
                const auto px = [&](int yy, int xx) {
                    return src[(std::size_t(yy) * w + xx) * 4 + c] * (1.0f / 255.0f);
                };
                const float v = px(y0, x0) * wx0 * wy0 + px(y0, x1) * wx1 * wy0 +
                                px(y1, x0) * wx0 * wy1 + px(y1, x1) * wx1 * wy1;
                dst[std::size_t(c) * S * S + std::size_t(y) * S + x] =
                    normalize ? (v - kMean[c]) / kStd[c] : v * 255.0f;
            }
        }
    }
}

std::vector<float> resizeMaskToSize(const float* src, int sw, int sh, int w, int h) {
    std::vector<float> out(std::size_t(w) * std::size_t(h));
    for (int y = 0; y < h; ++y) {
        const double fy = (sh > 1) ? (y + 0.5) * sh / h - 0.5 : 0.0;
        const double fyf = std::floor(fy);
        const int y0 = std::clamp(int(fyf), 0, sh - 1);
        const int y1 = std::clamp(std::min(int(fyf) + 1, sh - 1), 0, sh - 1);
        const float wy = static_cast<float>(fy - fyf);
        const float wy0 = 1.0f - wy, wy1 = wy;
        for (int x = 0; x < w; ++x) {
            const double fx = (sw > 1) ? (x + 0.5) * sw / w - 0.5 : 0.0;
            const double fxf = std::floor(fx);
            const int x0 = std::clamp(int(fxf), 0, sw - 1);
            const int x1 = std::clamp(std::min(int(fxf) + 1, sw - 1), 0, sw - 1);
            const float wx = static_cast<float>(fx - fxf);
            const float wx0 = 1.0f - wx, wx1 = wx;
            out[std::size_t(y) * w + x] =
                src[std::size_t(y0) * sw + x0] * wx0 * wy0 +
                src[std::size_t(y0) * sw + x1] * wx1 * wy0 +
                src[std::size_t(y1) * sw + x0] * wx0 * wy1 +
                src[std::size_t(y1) * sw + x1] * wx1 * wy1;
        }
    }
    return out;
}

// Joint bilateral upsampling of a low-res model mask into the full-res
// guide's lattice (Kopf et al. 2007). Plain bilinear (resizeMaskToSize)
// smooths each pixel but keeps the mask's 0.5-crossing anchored to the coarse
// model grid, so a diagonal hair edge upsamples to a staircase whose step
// size is (full-res / model-res) px. JBU re-solves only the edge band,
// weighting each coarse tap by spatial closeness AND colour agreement in the
// FULL-res guide, so the contour lands on real image edges instead of the
// lattice. Interiors are flat (all four bilinear taps equal) and are already
// exact under bilinear, so this stays linear in the contour's perimeter
// rather than the image area.
std::vector<float> resizeMaskGuided(const float* src, int sw, int sh,
                                     const std::uint8_t* guideRgba, int w,
                                     int h) {
    if (!src || !guideRgba || sw <= 0 || sh <= 0 || w <= 0 || h <= 0)
        return {};
    std::vector<float> out(std::size_t(w) * std::size_t(h));

    constexpr int kRadius = 2;                 // coarse taps: 5x5
    constexpr double kSigmaS = 1.25;            // spatial sigma, coarse px
    constexpr double kSigmaR = 24.0;            // colour sigma, 8-bit RGB
    constexpr double kInv2S = 1.0 / (2.0 * kSigmaS * kSigmaS);
    constexpr double kInv2R = 1.0 / (2.0 * kSigmaR * kSigmaR);
    // exp() lookup on the combined energy (spatial + colour), so the inner
    // loop costs arithmetic only and never a transcendental. Energy beyond
    // the table saturates to ~0, i.e. very dissimilar colours contribute
    // nothing.
    constexpr int kLutN = 1024;
    constexpr double kLutMax = 12.0;
    constexpr double kLutScale = (kLutN - 1) / kLutMax;
    std::array<float, kLutN> lut{};
    for (int i = 0; i < kLutN; ++i)
        lut[i] = static_cast<float>(std::exp(-double(i) / kLutScale));

    for (int y = 0; y < h; ++y) {
        const double fy = (sh > 1) ? (y + 0.5) * sh / h - 0.5 : 0.0;
        const int y0 = std::clamp(int(std::floor(fy)), 0, sh - 1);
        const int y1 = std::clamp(y0 + 1, 0, sh - 1);
        const float wy = static_cast<float>(fy - std::floor(fy));
        const std::uint8_t* grow =
            guideRgba + std::size_t(y) * std::size_t(w) * 4u;
        for (int x = 0; x < w; ++x) {
            const double fx = (sw > 1) ? (x + 0.5) * sw / w - 0.5 : 0.0;
            const int x0 = std::clamp(int(std::floor(fx)), 0, sw - 1);
            const int x1 = std::clamp(x0 + 1, 0, sw - 1);
            const float wx = static_cast<float>(fx - std::floor(fx));
            const float v00 = src[std::size_t(y0) * sw + x0];
            const float v01 = src[std::size_t(y0) * sw + x1];
            const float v10 = src[std::size_t(y1) * sw + x0];
            const float v11 = src[std::size_t(y1) * sw + x1];
            const float bil = v00 * (1 - wx) * (1 - wy) + v01 * wx * (1 - wy) +
                              v10 * (1 - wx) * wy + v11 * wx * wy;
            const float lo = std::min(std::min(v00, v01), std::min(v10, v11));
            const float hi = std::max(std::max(v00, v01), std::max(v10, v11));
            if (hi - lo < 1e-4f) {  // interior: bilinear is already exact
                out[std::size_t(y) * w + x] = bil;
                continue;
            }
            const double gr = grow[std::size_t(x) * 4u + 0];
            const double gg = grow[std::size_t(x) * 4u + 1];
            const double gb = grow[std::size_t(x) * 4u + 2];
            double acc = 0.0, wsum = 0.0;
            for (int qy = y0 - kRadius; qy <= y0 + kRadius; ++qy) {
                const int sy = std::clamp(qy, 0, sh - 1);
                const double dy = fy - sy;
                const int uy =
                    std::clamp(int((sy + 0.5) * h / sh), 0, h - 1);
                const std::uint8_t* qrow =
                    guideRgba + std::size_t(uy) * std::size_t(w) * 4u;
                for (int qx = x0 - kRadius; qx <= x0 + kRadius; ++qx) {
                    const int sx = std::clamp(qx, 0, sw - 1);
                    const double dx = fx - sx;
                    const int ux =
                        std::clamp(int((sx + 0.5) * w / sw), 0, w - 1);
                    const double dr = gr - qrow[std::size_t(ux) * 4u + 0];
                    const double dg = gg - qrow[std::size_t(ux) * 4u + 1];
                    const double db = gb - qrow[std::size_t(ux) * 4u + 2];
                    const double e = kInv2S * (dx * dx + dy * dy) +
                                     kInv2R * (dr * dr + dg * dg + db * db);
                    int idx = int(e * kLutScale);
                    if (idx >= kLutN) idx = kLutN - 1;
                    const double wt = lut[idx < 0 ? 0 : idx];
                    acc += wt * src[std::size_t(sy) * sw + sx];
                    wsum += wt;
                }
            }
            out[std::size_t(y) * w + x] =
                wsum > 1e-9 ? static_cast<float>(acc / wsum) : bil;
        }
    }
    return out;
}

#endif

}  // namespace ai_detail
}  // namespace pittore::ai
