#pragma once

#include <array>
#include <cstdint>

namespace pittore::color { class CmykToSrgb; }
namespace pittore::color { class SrgbToCmyk; }

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

}  // namespace pittore::compute
