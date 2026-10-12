#pragma once
// Color Replacement brush dab.
// Split from engine/compute/paint.h.
#include <cstdint>

#include "engine/compute/brushes/selection_mask/selection_mask.h"
#include "engine/compute/brushes/dab/native_cmyk.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

enum class ReplaceMode : int {
    Hue = 0,
    Saturation = 1,
    Color = 2,
    Luminosity = 3,
};

bool replace_color_dab_host(RGBAf* dst, std::uint32_t w, std::uint32_t h,
                            float cx, float cy, float radius, float hardness,
                            const RGBAf* targets, int targetCount,
                            const RGBAf& replacement, float tolerance,
                            ReplaceMode mode, int limits, bool antialias,
                            float harmony, int* bbox,
                            const SelectionMask* selection = nullptr,
                            NativeCmykPaint* nativeCmyk = nullptr);

}  // namespace pittore::compute
