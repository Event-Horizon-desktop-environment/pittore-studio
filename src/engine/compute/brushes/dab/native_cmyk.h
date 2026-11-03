#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "engine/color/icc_convert.h"
#include "engine/compute/blend.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

// Optional native CMYK destination for normal source-over paint dabs. RGB
// pixels remain the display projection; these device inks are editable data.
struct NativeCmykPaint {
    std::array<std::uint16_t, 4>* pixels = nullptr;
    std::array<std::uint16_t, 4> source{};
    const pittore::color::CmykToSrgb* preview = nullptr;
    const pittore::color::SrgbToCmyk* separation = nullptr;
    bool sourceFromRgb = false;
};

// One blend-mode paint texel into a native-CMYK destination. The dab blends
// in RGB through the shared W3C shell (blend.h), and the result is separated
// back to device inks so the editable inks follow the blended projection.
// Unlike source-over (one pre-separation per stroke), a blend function
// varies with the backdrop texel, so the separation runs per texel. Returns
// true when the destination was written; false when the caller should take
// its plain-RGB path instead (no inks to maintain here).
inline bool native_cmyk_blend_texel(NativeCmykPaint* paint, std::size_t idx,
                                    RGBAf& d, BlendMode mode, float sr,
                                    float sg, float sb, float sa, int x, int y) {
    if (!paint || !paint->pixels || !paint->separation || !paint->preview)
        return false;
    float or_, og, ob, oa;
    blend::pixel(mode, sr, sg, sb, sa, d.r, d.g, d.b, d.a, x, y, or_, og, ob,
                 oa);
    if (oa <= 0.0f) {
        paint->pixels[idx] = {};
        d = RGBAf{0, 0, 0, 0};
        return true;
    }
    float ink[4];
    paint->separation->convert(or_, og, ob, ink);
    auto& dstInk = paint->pixels[idx];
    for (int c = 0; c < 4; ++c)
        dstInk[c] = static_cast<std::uint16_t>(std::clamp(
            std::lround(ink[c] * 65535.0f), 0L, 65535L));
    float rgb[3];
    paint->preview->convert(dstInk[0] / 65535.0f, dstInk[1] / 65535.0f,
                            dstInk[2] / 65535.0f, dstInk[3] / 65535.0f, rgb);
    d.r = rgb[0];
    d.g = rgb[1];
    d.b = rgb[2];
    d.a = oa;
    return true;
}

}  // namespace pittore::compute
