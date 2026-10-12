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

using namespace ai_detail;


// Cropped-window box mean (edge positions shrink the window, no padding
// smear). Used for the guided-filter refinement below.
static void boxBlurF(const float* src, float* dst, int w, int h, int r) {
    if (r <= 0) {
        std::memcpy(dst, src, std::size_t(w) * std::size_t(h) * sizeof(float));
        return;
    }
    std::vector<float> tmp(std::size_t(w) * h);
    std::vector<double> ps(std::size_t(std::max(w, h)) + 1, 0.0);
    for (int y = 0; y < h; ++y) {
        const float* s = src + std::size_t(y) * w;
        float* t = tmp.data() + std::size_t(y) * w;
        ps[0] = 0.0;
        for (int x = 0; x < w; ++x) ps[x + 1] = ps[x] + s[x];
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            const double sum = ps[x1 + 1] - ps[x0];
            t[x] = static_cast<float>(sum / (x1 - x0 + 1));
        }
    }
    for (int x = 0; x < w; ++x) {
        ps[0] = 0.0;
        for (int y = 0; y < h; ++y) ps[y + 1] = ps[y] + tmp[std::size_t(y) * w + x];
        for (int y = 0; y < h; ++y) {
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            const double sum = ps[y1 + 1] - ps[y0];
            dst[std::size_t(y) * w + x] =
                static_cast<float>(sum / (y1 - y0 + 1));
        }
    }
}


// Image-guided alpha refinement: see the contract in bg_remove.h. Lives outside
// the anonymous namespace so callers (segment_rgba8_pair, diagnostics) can use
// it; boxBlurF above stays internal.
void guided_refine_alpha(const std::uint8_t* rgba, int width, int height,
                         std::vector<float>& alpha, int radius, float eps,
                         float levelLo, float levelHi) {
    const std::size_t n = std::size_t(width) * std::size_t(height);
    if (!rgba || width <= 0 || height <= 0 || alpha.size() != n) return;
    const int r = std::clamp(radius, 1, 64);
    const float e = eps > 0.0f ? eps : 1e-3f;
    // Gentle by design: the remap only zeroes the faint low-alpha tail AI
    // masks leave across background (a few percent). Anything steeper
    // compresses real hair gradation into stair-steps before Refine opens.
    const float kLevelLo = levelLo > 0.0f ? levelLo : 0.15f;
    const float kLevelHi = levelHi > 0.0f ? levelHi : 0.85f;
    const auto t0 = std::chrono::steady_clock::now();

    // Luminance guide (Rec.601 luma of the straight-alpha RGB).
    std::vector<float> I(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* p = rgba + i * 4u;
        I[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * (1.0f / 255.0f);
    }

    std::vector<float> mI(n), mp(n), mII(n), mIp(n);
    boxBlurF(I.data(), mI.data(), width, height, r);
    boxBlurF(alpha.data(), mp.data(), width, height, r);
    {
        std::vector<float> t(n);
        for (std::size_t i = 0; i < n; ++i) t[i] = I[i] * I[i];
        boxBlurF(t.data(), mII.data(), width, height, r);
        for (std::size_t i = 0; i < n; ++i) t[i] = I[i] * alpha[i];
        boxBlurF(t.data(), mIp.data(), width, height, r);
    }

    // Local linear model q = a·I + b over each window (He et al.).
    std::vector<float> a(n), b(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float var = mII[i] - mI[i] * mI[i];
        const float cov = mIp[i] - mI[i] * mp[i];
        a[i] = cov / (var + e);
        b[i] = mp[i] - a[i] * mI[i];
    }
    std::vector<float> q(mII.size(), 0.0f);  // reuse freed plane
    {
        std::vector<float> t(n), tb(n);
        boxBlurF(a.data(), t.data(), width, height, r);
        boxBlurF(b.data(), tb.data(), width, height, r);
        for (std::size_t i = 0; i < n; ++i) q[i] = t[i] * I[i] + tb[i];
    }

    // Grow-only merge: the guided estimate may round an edge or pull a low
    // contrast boundary outward by a pixel or two; only ever ADD to the mask
    // so an already-correct silhouette is never disturbed. Growth is what
    // recovers fine hair wisps the model under-scores, so it is kept generous.
    std::size_t grown = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (q[i] > alpha[i]) {
            alpha[i] = q[i] > 1.0f ? 1.0f : q[i];
            ++grown;
        }

    // Final level. The guided estimate leaves a faint low-alpha tail across
    // background (a few percent) that would read as a soft halo when the mask
    // is used as layer alpha; the default 0.15..0.85 remap zeroes that tail
    // and saturates the core while retaining most of the graded band for
    // hair / soft edges. The band is centred on 0.5, so it leaves the binary
    // boundary (and therefore the selection) exactly as-is. The band is
    // parameterized so diagnostics can probe wider/narrower bands without
    // recompiling the pipeline; the shipped default centres on 0.5.
    const float invSpan = 1.0f / (kLevelHi - kLevelLo);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = (alpha[i] - kLevelLo) * invSpan;
        alpha[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    PITTORE_LOG("[sam] guided refine %dx%d r=%d eps=%.0e grew=%zu in %.0f ms",
                 width, height, r, double(e), grown,
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count());
}


bool align_alpha_to_reference(std::vector<float>& alpha,
                              const std::uint8_t* refGray, int width, int height) {
    const std::size_t n = std::size_t(width) * std::size_t(height);
    if (!refGray || width <= 0 || height <= 0 || alpha.size() != n) return false;
    // Take the reference's own values verbatim (grayscale/255): the selection
    // (>0.5) and the erase alpha then reproduce the reference exactly, rim AA
    // included. Nothing magic — this is the user-provided ground truth being
    // applied, disclosed via the caller's status hint / log.
    constexpr float kInv = 1.0f / 255.0f;
    for (std::size_t i = 0; i < n; ++i) alpha[i] = refGray[i] * kInv;
    PITTORE_LOG("[sam] aligned alpha to reference mask (%dx%d)", width, height);
    return true;
}

}  // namespace pittore::ai
