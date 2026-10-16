#pragma once
// Solid paint to rgba.
#include <string>

namespace pittore::svg {

bool solidRgba(const std::string& color, const std::string& opacity,
               float out[4]);

// Single-color paint server (`<solidcolor>`): `solid-color` plus
// `solid-opacity` resolve to one rgba via solidRgba. Valid=false when the
// color is not a plain color (empty, none, url()).
struct SolidColor {
    float rgba[4] = {0, 0, 0, 1};
    bool valid = false;
};

SolidColor parseSolidColorElem(const std::string& color,
                               const std::string& opacity);

}  // namespace pittore::svg
