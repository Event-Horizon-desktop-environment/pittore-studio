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


// Shared decoder prompt: run the small SAM decoder on the layer embeddings with
// foreground points (LAYER pixel space) and return the source-resolution soft
// alpha. When `promptMask` is non-null (width*height source-resolution floats
// in [0,1]) it is fed back to the decoder as the prompt mask (has_mask=1): SAM
// re-segments given its own mask, so iterating this grows a partial result
// toward the whole object. Multi-point prompts (two+ positives in one decode)
// are the canonical SAM interaction for merging attached objects in a single
// pass — better than unioning separately-decoded masks.
SamDecodeResult decodePrompt(const SamEncodings& enc,
                             const std::vector<float>& coordsPx,
                             const std::vector<float>& labels,
                             const float* promptMask,
                             const std::string& decoderPath) {
    SamDecodeResult res;
#ifndef PITTORE_HAS_ONNX
    // No-ONNX stub: the parameters feed the ONNX path below, so they are
    // intentionally unused in this configuration.
    (void)enc;
    (void)coordsPx;
    (void)labels;
    (void)promptMask;
    (void)decoderPath;
    res.error = "This build links no ONNX Runtime.";
    return res;
#else
    const auto tStart = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - tStart)
            .count();
    };
    if (enc.embeddings.empty() || enc.width <= 0 || enc.height <= 0 ||
        decoderPath.empty() || coordsPx.empty() ||
        coordsPx.size() != labels.size() * 2) {
        res.error = "No object embeddings for this layer — run Object Select once "
                    "first (or re-run after the layer changes).";
        return res;
    }
    constexpr std::size_t kEmb = std::size_t(256) * 64 * 64;  // [1,256,64,64]
    if (enc.embeddings.size() != kEmb) {
        res.error = "Embeddings look corrupt (got " +
                    std::to_string(enc.embeddings.size()) + " floats, expected " +
                    std::to_string(kEmb) + ").";
        return res;
    }
    const std::size_t nPoints = labels.size();
    PITTORE_LOG("[sam] decode begin: %zu prompts first=(%.0f,%.0f) mask=%d decoder=%.60s",
                 nPoints, coordsPx[0], coordsPx[1], promptMask ? 1 : 0,
                 decoderPath.c_str());
    const int S = (enc.inputSize > 0) ? enc.inputSize : 1024;
    try {
        std::lock_guard<std::mutex> lock(gMutex);
        std::string loadErr;
        OrtSession* session =
            getOrLoadSessionLocked(decoderPath, "SAM decoder", loadErr, elapsedMs());
        if (!session) {
            res.error = loadErr;
            return res;
        }

        // Prompts: foreground query points mapped into the S×S canvas the
        // encoder saw (full-frame bilinear resize => plain scale). Each extra
        // positive point grows the object; a negative point (label 0) carves
        // it out. First pass has no previous mask: zeros + has_mask=0; a
        // refinement pass feeds the previous decode's alpha back as the prompt.
        std::vector<float> coords;
        coords.reserve(coordsPx.size());
        for (std::size_t i = 0; i < nPoints; ++i) {
            coords.push_back((coordsPx[i * 2] + 0.5f) * S / enc.width);
            coords.push_back((coordsPx[i * 2 + 1] + 0.5f) * S / enc.height);
        }
        std::vector<float> maskInput(std::size_t(256) * 256, 0.0f);
        std::vector<float> hasMask{0.0f};
        if (promptMask) {
            maskInput = resizeMaskToSize(promptMask, enc.width, enc.height, 256, 256);
            hasMask[0] = 1.0f;
        }

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> embShape{1, 256, 64, 64};
        const std::array<int64_t, 3> coordShape{1, int64_t(nPoints), 2};
        const std::array<int64_t, 2> labelShape{1, int64_t(nPoints)};
        const std::array<int64_t, 4> maskShape{1, 1, 256, 256};
        const std::array<int64_t, 1> hasShape{1};
        Ort::Value embT = Ort::Value::CreateTensor<float>(
            memInfo, const_cast<float*>(enc.embeddings.data()),
            enc.embeddings.size(), embShape.data(), embShape.size());
        Ort::Value coordT = Ort::Value::CreateTensor<float>(
            memInfo, coords.data(), coords.size(), coordShape.data(), coordShape.size());
        Ort::Value labelT = Ort::Value::CreateTensor<float>(
            memInfo, const_cast<float*>(labels.data()), labels.size(),
            labelShape.data(), labelShape.size());
        Ort::Value maskT = Ort::Value::CreateTensor<float>(
            memInfo, maskInput.data(), maskInput.size(), maskShape.data(),
            maskShape.size());
        Ort::Value hasT = Ort::Value::CreateTensor<float>(
            memInfo, hasMask.data(), hasMask.size(), hasShape.data(), hasShape.size());

        Ort::AllocatorWithDefaultOptions allocator;
        auto n0 = session->session.GetInputNameAllocated(0, allocator);
        auto n1 = session->session.GetInputNameAllocated(1, allocator);
        auto n2 = session->session.GetInputNameAllocated(2, allocator);
        auto n3 = session->session.GetInputNameAllocated(3, allocator);
        auto n4 = session->session.GetInputNameAllocated(4, allocator);
        auto o0 = session->session.GetOutputNameAllocated(0, allocator);
        auto o1 = session->session.GetOutputNameAllocated(1, allocator);
        const char* inNames[] = {n0.get(), n1.get(), n2.get(), n3.get(), n4.get()};
        const char* outNames[] = {o0.get(), o1.get()};
        Ort::Value inVals[] = {std::move(embT), std::move(coordT), std::move(labelT),
                               std::move(maskT), std::move(hasT)};

        PITTORE_LOG("[sam] decoder inference start on %s (at %.0f ms)",
                     session->provider.c_str(), elapsedMs());
        std::vector<Ort::Value> outputs = session->session.Run(
            Ort::RunOptions{nullptr}, inNames, inVals, 5, outNames, 2);
        if (outputs.size() < 2) {
            res.error = "Decoder produced fewer than two outputs (masks, iou).";
            return res;
        }
        const auto mShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const float* masks = outputs[0].GetTensorMutableData<float>();
        const float* ious = outputs[1].GetTensorMutableData<float>();
        std::size_t total = 1;
        std::string shapeStr = "[";
        for (std::size_t i = 0; i < mShape.size(); ++i) {
            if (i) shapeStr += ",";
            shapeStr += std::to_string(mShape[i]);
            total *= std::size_t(std::max<int64_t>(mShape[i], 1));
        }
        shapeStr += "]";
        const int candidates =
            (mShape.size() >= 2) ? static_cast<int>(mShape[1]) : 1;
        const std::size_t plane =
            (candidates > 0) ? total / std::size_t(candidates) : 0;
        if (plane != std::size_t(256) * 256) {
            res.error = "Decoder returned an unexpected mask layout (shape " +
                        shapeStr + ").";
            return res;
        }
        // Pick the candidate with the best predicted quality score.
        int best = 0;
        for (int i = 1; i < candidates; ++i)
            if (ious[i] > ious[best]) best = i;
        res.iou = ious[best];
        res.px = static_cast<int>(coordsPx[0]);  // first prompt point, layer px
        res.py = static_cast<int>(coordsPx[1]);
        // SAM emits logits at 256². Resample the LOGITS bilinearly up to source
        // resolution and only then sigmoid — this is what Meta's reference
        // implementation does (`F.interpolate(logits)` then `> 0`). Sigmoiding
        // first and resampling the probabilities (the old order) pre-smooths the
        // decision boundary, so every silhouette edge came out several source
        // pixels wide and low-contrast; upsample-then-sigmoid keeps the 0.5
        // contour where the decoder actually put it.
        res.alpha = resizeMaskToSize(masks + std::size_t(best) * plane, 256, 256,
                                    enc.width, enc.height);
        for (float& a : res.alpha) a = 1.0f / (1.0f + std::exp(-a));
        res.ok = true;
        PITTORE_LOG("[sam] decode done: shape=%s candidates=%d best=%d iou=%.3f "
                     "alpha=%zu (%.0f ms)",
                     shapeStr.c_str(), candidates, best, res.iou, res.alpha.size(),
                     elapsedMs());
        return res;
    } catch (const std::bad_alloc&) {
        res.error = "Out of memory decoding the object mask.";
        PITTORE_LOG("[sam] OUT OF MEMORY decoding (%.0f ms)", elapsedMs());
    } catch (const std::exception& e) {
        res.error = "Object mask decode failed: " + std::string(e.what());
        PITTORE_LOG("[sam] decode exception: %s (at %.0f ms)", e.what(), elapsedMs());
    } catch (...) {
        res.error = "Object mask decode failed with an unknown error.";
        PITTORE_LOG("[sam] decode unknown exception (at %.0f ms)", elapsedMs());
    }
    return res;
#endif
}

}  // namespace ai_detail
}  // namespace pittore::ai
