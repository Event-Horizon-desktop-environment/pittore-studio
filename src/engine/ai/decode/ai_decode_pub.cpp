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
#include "engine/ai/decode/ai_decode.h"

namespace pittore::ai {

using namespace ai_detail;


SamDecodeResult decode_point(const SamEncodings& enc, int px, int py,
                             const std::string& decoderPath) {
    return decodePrompt(enc, {float(px), float(py)}, {1.0f}, nullptr, decoderPath);
}


SamDecodeResult decode_mask(const SamEncodings& enc, int px, int py,
                            const float* promptMask,
                            const std::string& decoderPath) {
    return decodePrompt(enc, {float(px), float(py)}, {1.0f}, promptMask,
                        decoderPath);
}


SamDecodeResult decode_points(const SamEncodings& enc,
                              const std::vector<std::pair<int, int>>& points,
                              const std::string& decoderPath) {
    std::vector<float> coords, labels;
    coords.reserve(points.size() * 2);
    labels.reserve(points.size());
    for (const auto& p : points) {
        coords.push_back(float(p.first));
        coords.push_back(float(p.second));
        labels.push_back(1.0f);
    }
    return decodePrompt(enc, coords, labels, nullptr, decoderPath);
}


// Multi-point + previous-mask prompt: same as decode_points but also feeds the
// decoder a soft mask (has_mask=1) so the re-segmentation is anchored both to
// the prompt points and to the already-decoded region. The canonical one-shot
// amber-refine: with a mask prompt plus all grid points SAM confirms the full
// subject extent instead of re-inventing it around a single click.
SamDecodeResult decode_mask_points(const SamEncodings& enc,
                                   const std::vector<std::pair<int, int>>& points,
                                   const float* promptMask,
                                   const std::string& decoderPath) {
    std::vector<float> coords, labels;
    coords.reserve(points.size() * 2);
    labels.reserve(points.size());
    for (const auto& p : points) {
        coords.push_back(float(p.first));
        coords.push_back(float(p.second));
        labels.push_back(1.0f);
    }
    return decodePrompt(enc, coords, labels, promptMask, decoderPath);
}


SamDecodeResult refine_mask(const SamEncodings& enc, int px, int py,
                            const float* seedAlpha,
                            const std::string& decoderPath) {
    SamDecodeResult res;
#ifndef PITTORE_HAS_ONNX
    // No-ONNX stub: the parameters feed the ONNX path below, so they are
    // intentionally unused in this configuration.
    (void)enc;
    (void)px;
    (void)py;
    (void)seedAlpha;
    (void)decoderPath;
    res.error = "This build links no ONNX Runtime.";
    return res;
#else
    if (!seedAlpha || enc.width <= 0 || enc.height <= 0 ||
        decoderPath.empty()) {
        res.error = "Invalid refinement input (null seed, empty decoder).";
        return res;
    }
    const std::size_t total = std::size_t(enc.width) * std::size_t(enc.height);
    std::vector<float> acc(seedAlpha, seedAlpha + total);
    std::vector<float> prompt = acc;
    res.px = px;
    res.py = py;
    for (int it = 0; it < 3; ++it) {
        SamDecodeResult r = decode_mask(enc, px, py, prompt.data(), decoderPath);
        if (!r.ok) break;
        std::size_t grew = 0;
        for (std::size_t i = 0; i < acc.size(); ++i)
            if (r.alpha[i] > acc[i]) {
                acc[i] = r.alpha[i];
                ++grew;
            }
        prompt = std::move(r.alpha);
        if (grew < acc.size() / 200) break;  // <0.5% of pixels
    }
    res.ok = true;
    res.alpha = std::move(acc);
    PITTORE_LOG("[sam] refine done: seed=(%d,%d) alpha=%zu", px, py, res.alpha.size());
    return res;
#endif
}

}  // namespace pittore::ai
