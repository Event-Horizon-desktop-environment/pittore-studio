// Reuse color parse.
#include "engine/vector/svg/solid_color.h"

#include "engine/vector/svg/color_parse.h"
#include "engine/vector/svg/stop_opacity.h"

namespace pittore::svg {

bool solidRgba(const std::string& color, const std::string& opacity,
               float out[4]) {
    if (!parseColor(color, out)) {
        return false;
    }
    out[3] *= stopAlpha(opacity, 1.0f);
    return true;
}

SolidColor parseSolidColorElem(const std::string& color,
                               const std::string& opacity) {
    SolidColor sc;
    sc.valid = solidRgba(color, opacity, sc.rgba);
    return sc;
}

}  // namespace pittore::svg
