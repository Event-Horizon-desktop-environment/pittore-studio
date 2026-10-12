#pragma once
// Paint-bucket / magic-eraser flood fill.
// Split from engine/compute/paint.h.
#include <cstdint>

#include "engine/compute/brushes/selection_mask/selection_mask.h"
#include "engine/compute/brushes/dab/native_cmyk.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

bool flood_fill_host(const RGBAf* match, RGBAf* dst, std::uint32_t w,
                     std::uint32_t h, int seedX, int seedY, float tolerance,
                     bool contiguous, bool antialias, float opacity,
                     const RGBAf& color, bool erase, int* bbox,
                     const SelectionMask* selection = nullptr,
                     NativeCmykPaint* nativeCmyk = nullptr);

}  // namespace pittore::compute
