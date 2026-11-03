#include "engine/text/text_engine.h"

#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>

#include <fontconfig/fontconfig.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/text/shared/text_internal.h"

namespace pittore::text {
namespace detail {

std::vector<ShapedGlyph> shape(Face& f, const std::string& text, float size,
                               const TextSpec& spec) {
    std::vector<ShapedGlyph> out;
    if (text.empty()) return out;
    setSize(f, size);

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_language(
        buf, hb_language_from_string(spec.language.empty() ? "en"
                                                           : spec.language.c_str(),
                                     -1));
    hb_buffer_add_utf8(buf, text.data(), static_cast<int>(text.size()), 0,
                       static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buf);

    struct Feature {
        const char* tag;
        bool on;
    };
    const unsigned ot = spec.otFeatures;
    const Feature wanted[] = {
        {"kern", spec.kerning},
        {"rlig", true},
        {"liga", (ot & OTF_Liga) != 0},
        {"calt", (ot & OTF_Calt) != 0},
        {"clig", (ot & OTF_Calt) != 0},
        {"smcp", (ot & OTF_Smcp) != 0},
        {"c2sc", (ot & OTF_C2sc) != 0},
        {"sups", (ot & OTF_Sups) != 0},
        {"subs", (ot & OTF_Subs) != 0},
        {"frac", (ot & OTF_Frac) != 0},
        {"ordn", (ot & OTF_Ordn) != 0},
        {"swsh", (ot & OTF_Swsh) != 0},
        {"ss01", (ot & OTF_Ss01) != 0},
        {"ss02", (ot & OTF_Ss02) != 0},
        {"ss03", (ot & OTF_Ss03) != 0},
    };
    std::vector<hb_feature_t> feats;
    feats.reserve(32);
    auto add = [&](const char* tag, bool on) {
        char tmp[16];
        std::snprintf(tmp, sizeof tmp, "%s=%d", tag, on ? 1 : 0);
        hb_feature_t f2{};
        if (hb_feature_from_string(tmp, -1, &f2)) feats.push_back(f2);
    };
    for (const Feature& w : wanted) add(w.tag, w.on);
    // Substitutions that must stay off unless explicitly requested above, so
    // ligatures do not silently rewrite a run the user has not opted into.
    add("dlig", false);
    add("hlig", false);
    hb_shape(f.hb, buf, feats.data(), static_cast<unsigned>(feats.size()));

    unsigned count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
    out.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        ShapedGlyph g;
        g.id = info[i].codepoint;
        g.cluster = info[i].cluster;
        g.xAdvance = static_cast<float>(pos[i].x_advance) / 64.0f;
        g.xOffset = static_cast<float>(pos[i].x_offset) / 64.0f;
        g.yOffset = static_cast<float>(pos[i].y_offset) / 64.0f;
        out.push_back(g);
    }
    hb_buffer_destroy(buf);
    return out;
}

}  // namespace detail
}  // namespace pittore::text
