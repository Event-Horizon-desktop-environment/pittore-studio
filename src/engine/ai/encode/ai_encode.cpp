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


SamEncodings encode_rgba8(const std::uint8_t* rgba, int width, int height,
                          const std::string& encoderPath, int inputSize) {
    SamEncodings enc;
    enc.width = width;
    enc.height = height;
#ifndef PITTORE_HAS_ONNX
    // No-ONNX stub: the parameters feed the ONNX path below, so they are
    // intentionally unused in this configuration.
    (void)rgba;
    (void)encoderPath;
    (void)inputSize;
    enc.error = "This build links no ONNX Runtime. Install onnxruntime (or "
                "onnxruntime-cuda) and rebuild, or pass -Donnxruntime-root.";
    return enc;
#else
    const auto tStart = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - tStart)
            .count();
    };
    if (!rgba || width <= 0 || height <= 0 || encoderPath.empty()) {
        enc.error = "Invalid segmentation input (null buffer, empty encoder).";
        return enc;
    }
    PITTORE_LOG("[sam] encode begin: source=%dx%d bytes=%zu input=%d encoder=%.80s",
                 width, height, std::size_t(width) * std::size_t(height) * 4u,
                 inputSize, encoderPath.c_str());
    const int S = std::clamp(inputSize > 0 ? inputSize : 1024, 64, 4096);
    enc.inputSize = S;
    try {
        std::lock_guard<std::mutex> lock(gMutex);
        std::string loadErr;
        OrtSession* session =
            getOrLoadSessionLocked(encoderPath, "SAM encoder", loadErr, elapsedMs());
        if (!session) {
            enc.error = loadErr;
            return enc;
        }
        PITTORE_LOG("[sam] preprocessing %dx%d -> %dx%d ImageNet-normalised RGB "
                     "(at %.0f ms)",
                     width, height, S, S, elapsedMs());
        std::vector<float> input(std::size_t(3u) * S * S, 0.0f);
        // SAM's standard preprocessing is (pixel - mean) / std with mean/std in
        // 0..255 units — i.e. ImageNet normalisation. Feeding raw 0..255 (the
        // old behaviour) left the encoder embeddings badly out of distribution:
        // point prompts returned small sub-part fragments and the whole
        // selection was unreliable. Normalising restores full-object masks.
        resizeRgba8ToNchwRgb(rgba, width, height, input.data(), S,
                             /*normalize=*/true);

        const std::array<int64_t, 4> shape{1, 3, S, S};
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        auto inName = session->session.GetInputNameAllocated(0, allocator);
        auto outName = session->session.GetOutputNameAllocated(0, allocator);
        const char* inNames[] = {inName.get()};
        const char* outNames[] = {outName.get()};

        PITTORE_LOG("[sam] encoder inference start on %s (at %.0f ms)",
                     session->provider.c_str(), elapsedMs());
        std::vector<Ort::Value> outputs = session->session.Run(
            Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
        if (outputs.empty()) {
            enc.error = "Encoder produced no output.";
            return enc;
        }
        const auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const float* raw = outputs[0].GetTensorMutableData<float>();
        std::size_t total = 1;
        std::string shapeStr = "[";
        for (std::size_t i = 0; i < outShape.size(); ++i) {
            if (i) shapeStr += ",";
            shapeStr += std::to_string(outShape[i]);
            total *= std::size_t(std::max<int64_t>(outShape[i], 1));
        }
        shapeStr += "]";
        PITTORE_LOG("[sam] encoder inference done in %.0f ms: shape=%s total=%zu",
                     elapsedMs(), shapeStr.c_str(), total);
        if (total < 2) {
            enc.error = "Encoder produced an empty output (shape " + shapeStr + ").";
            return enc;
        }
        enc.embeddings.assign(raw, raw + total);
        enc.ok = true;
        PITTORE_LOG("[sam] encode done: %dx%d source, %zu embeddings, total %.0f ms",
                     width, height, enc.embeddings.size(), elapsedMs());
        return enc;
    } catch (const std::bad_alloc&) {
        enc.error = "Out of memory encoding " + std::to_string(width) + "x" +
                    std::to_string(height) + ".";
        PITTORE_LOG("[sam] OUT OF MEMORY encoding %dx%d encoder=%.60s (%.0f ms)",
                     width, height, encoderPath.c_str(), elapsedMs());
    } catch (const std::exception& e) {
        enc.error = "Segmentation encode failed: " + std::string(e.what());
        PITTORE_LOG("[sam] encode exception: %s (at %.0f ms)", e.what(), elapsedMs());
    } catch (...) {
        enc.error = "Segmentation encode failed with an unknown error.";
        PITTORE_LOG("[sam] encode unknown exception (at %.0f ms)", elapsedMs());
    }
    return enc;
#endif
}

}  // namespace pittore::ai
