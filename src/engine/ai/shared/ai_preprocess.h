#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/ai/bg_remove.h"

#ifdef PITTORE_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace pittore::ai {
namespace ai_detail {

#ifdef PITTORE_HAS_ONNX
void resizeRgba8ToNchwRgb(const std::uint8_t* src, int w, int h, float* dst, int S,
                          bool normalize = true);
std::vector<float> resizeMaskToSize(const float* src, int sw, int sh, int w, int h);
// Edge-aware lift of a coarse model mask into the full-res guide's lattice
// (guided / joint-bilateral upsampling). See ai_preprocess.cpp.
std::vector<float> resizeMaskGuided(const float* src, int sw, int sh,
                                     const std::uint8_t* guideRgba, int w,
                                     int h);
#endif

}  // namespace ai_detail
}  // namespace pittore::ai
