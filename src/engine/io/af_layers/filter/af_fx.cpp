#include "engine/io/af_layers.h"
#include "engine/io/af.h"
#include "engine/render/layer_style.h"
#include "engine/text/text_engine.h"
#include "engine/vector/vector_shape.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>
#ifdef PITTORE_AF
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif
#ifdef PITTORE_WEBP
#include "engine/io/webp.h"
#endif
#ifdef PITTORE_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#endif
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/geom/af_geom.h"
#include "engine/io/af_layers/geom/af_homography.h"
#include "engine/io/af_layers/graph/af_graph.h"
#include "engine/io/af_layers/graph/af_graph_parser.h"
#include "engine/io/af_layers/image/af_image.h"
#include "engine/io/af_layers/filter/af_blur.h"
#include "engine/io/af_layers/filter/af_distort.h"
#include "engine/io/af_layers/filter/af_fx.h"
#include "engine/io/af_layers/filter/af_live.h"
#include "engine/io/af_layers/vector/af_shapes.h"
#include "engine/io/af_layers/walker/af_walker.h"

namespace pittore::io {
namespace af_detail {

// Layer effects ("FiEf") are baked into a layer's pixels. The .af file's effects
// all derive from the layer's alpha: shadows and glows are that alpha blurred,
// offset and coloured; strokes are a band along its edge; bevels shade it by
// the slope of a blurred copy. Effects work on a straight-alpha float plane over
// the content grown by the effects' reach, then convert back to 8-bit.

// The separable blend modes effects can name; the rest fall back to Normal.
render::StyleBlend fxBlendOf(std::uint16_t id, std::uint16_t version) {
    switch (id) {
        case 1: return render::StyleBlend::Darken;
        case 2: return version == 0 ? render::StyleBlend::Multiply : render::StyleBlend::Normal;
        case 3: return render::StyleBlend::ColorBurn;
        case 4: return render::StyleBlend::Lighten;
        case 5: return render::StyleBlend::Screen;
        case 6: return version == 0 ? render::StyleBlend::ColorDodge : render::StyleBlend::Normal;
        case 7: return render::StyleBlend::Add;
        case 8: return render::StyleBlend::Overlay;
        case 9: return render::StyleBlend::SoftLight;
        case 10: return render::StyleBlend::HardLight;
        case 14: return render::StyleBlend::Difference;
        case 15: return version == 0 ? render::StyleBlend::Exclusion : render::StyleBlend::Normal;
        case 16: return render::StyleBlend::Subtract;
        default: return render::StyleBlend::Normal;
    }
}


std::optional<render::StyleColor> fxColor(const Graph& g, const Node* colr) {
    if (!colr) return std::nullopt;
    const Value* v = g.field(colr, "_col");
    if (!v || v->k != Value::K::Struct) return std::nullopt;
    const std::vector<std::uint8_t>& raw = v->bytes;
    auto f = [&](std::size_t i) -> float {
        float x = 0.0f;
        if ((i + 1) * 4 <= raw.size()) std::memcpy(&x, raw.data() + i * 4, 4);
        return x;
    };
    auto to = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    const std::uint32_t tag = g.typeTag(colr);
    if (tag == tag4("RGBA") && raw.size() >= 16)
        return render::StyleColor{to(f(0)), to(f(1)), to(f(2)), to(f(3))};
    if (tag == tag4("GRAY") && raw.size() >= 8) {
        const float v0 = to(f(0));
        return render::StyleColor{v0, v0, v0, to(f(1))};
    }
    if (tag == tag4("CMYK") && raw.size() >= 20) {
        const float k = f(3);
        return render::StyleColor{to((1.0f - f(0)) * (1.0f - k)), to((1.0f - f(1)) * (1.0f - k)),
                       to((1.0f - f(2)) * (1.0f - k)), to(f(4))};
    }
    if (tag == tag4("HSLA") && raw.size() >= 16) {
        const float h = f(0), s = f(1), l = f(2);
        auto hue = [](float p, float q, float t) {
            if (t < 0.0f) t += 1.0f;
            if (t > 1.0f) t -= 1.0f;
            if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
            if (t < 1.0f / 2.0f) return q;
            if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
            return p;
        };
        if (s <= 0.0f) return render::StyleColor{to(l), to(l), to(l), to(f(3))};
        const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        return render::StyleColor{to(hue(p, q, h + 1.0f / 3.0f)), to(hue(p, q, h)),
                       to(hue(p, q, h - 1.0f / 3.0f)), to(f(3))};
    }
    if (raw.size() >= 4)
        return render::StyleColor{raw[0] / 255.0f, raw[1] / 255.0f, raw[2] / 255.0f, raw[3] / 255.0f};
    return std::nullopt;
}


bool fxGradientStops(const Graph& g, const Node* fill, render::StyleColor& from, render::StyleColor& to, bool& radial) {
    if (!fill) return false;
    Node* grad = g.child(fill, "Grad");
    if (!grad) return false;
    const std::vector<Node*> cols = g.children(grad, "Cols");
    if (cols.empty()) return false;
    const auto a = fxColor(g, cols.front());
    const auto b = fxColor(g, cols.back());
    if (!a || !b) return false;
    from = *a;
    to = *b;
    const auto type = enumOf(g, fill, "Type");
    radial = type && *type >= 2;
    return true;
}


// Map a layer's `FiEf` list onto a LayerStyle. Effects we cannot restyle are
// reported through `skipped` (they change what the layer looks like).
render::LayerStyle fxParse(const Graph& g, const Node* node, const Mat& ctm,
                std::vector<std::string>& skipped) {
    render::LayerStyle st;
    const double sx = std::hypot(ctm.m[0], ctm.m[3]);
    const double sy = std::hypot(ctm.m[1], ctm.m[4]);
    const float scale = static_cast<float>((sx + sy) * 0.5);
    auto f64 = [&](const Node* n, const char* name, double dflt) -> double {
        const Value* v = g.field(n, name);
        if (v && v->k == Value::K::F64) return v->f;
        if (v && v->k == Value::K::F32) return v->f;
        return dflt;
    };
    for (Node* fx : g.children(node, "FiEf")) {
        if (!boolOf(g, fx, "Enab").value_or(false)) continue;
        const std::uint32_t tag = g.typeTag(fx);
        const float opacity = static_cast<float>(f64(fx, "Opac", 1.0));
        render::StyleBlend blend = render::StyleBlend::Normal;
        bool haveBlend = false;
        if (const auto b = enumVerOf(g, fx, "BlnM")) {
            blend = fxBlendOf(b->first, b->second);
            haveBlend = true;
        }
        const auto color = fxColor(g, g.child(fx, "Colr"));
        const float s = boolOf(g, fx, "SclO").value_or(false) ? scale : 1.0f;
        const float radius = static_cast<float>(f64(fx, "Radi", 0.0)) * s;
        const float blurRadius = radius * kFxBlurRadi;
        const float intensity =
            std::clamp(1.0f - static_cast<float>(f64(fx, "Comp", 1.0)), 0.0f, 1.0f);
        if (tag == tag4("Shad") || tag == tag4("InnS")) {
            render::ShadowStyle sh;
            sh.color = color.value_or(render::StyleColor{0.0f, 0.0f, 0.0f, 1.0f});
            sh.blend = haveBlend ? blend : render::StyleBlend::Multiply;
            sh.opacity = opacity;
            const double angl = f64(fx, "Angl", 3.14159265358979323846 / 4.0);
            sh.angle = 180.0f - static_cast<float>(angl * 180.0 / 3.14159265358979323846);
            sh.distance = static_cast<float>(f64(fx, "Offs", 0.0)) * s;
            sh.spread = intensity;
            sh.size = blurRadius;
            sh.knockout = boolOf(g, fx, "Knck").value_or(true);
            if (tag == tag4("Shad")) {
                st.hasDropShadow = true;
                st.dropShadow = sh;
            } else {
                st.hasInnerShadow = true;
                st.innerShadow = sh;
            }
        } else if (tag == tag4("OutG") || tag == tag4("InnG")) {
            render::GlowStyle gl;
            gl.color = color.value_or(render::StyleColor{1.0f, 1.0f, 0.75f, 1.0f});
            gl.blend = haveBlend ? blend : render::StyleBlend::Screen;
            gl.opacity = opacity;
            gl.spread = intensity;
            gl.size = blurRadius;
            if (tag == tag4("OutG")) {
                st.hasOuterGlow = true;
                st.outerGlow = gl;
            } else {
                st.hasInnerGlow = true;
                st.innerGlow = gl;
            }
        } else if (tag == tag4("ColO")) {
            st.hasColorOverlay = true;
            st.colorOverlay.color = color.value_or(render::StyleColor{1.0f, 0.0f, 0.0f, 1.0f});
            st.colorOverlay.blend = haveBlend ? blend : render::StyleBlend::Normal;
            st.colorOverlay.opacity = opacity;
        } else if (tag == tag4("Strk")) {
            if (!color) {
                skipped.push_back("Strk (gradient stroke)");
                continue;
            }
            st.hasStroke = true;
            st.stroke.color = *color;
            st.stroke.blend = haveBlend ? blend : render::StyleBlend::Normal;
            st.stroke.opacity = opacity;
            st.stroke.size = radius;
            const auto alig = enumOf(g, fx, "Alig");
            st.stroke.position = alig && *alig == 1 ? 1 : (alig && *alig == 2 ? 2 : 0);
        } else if (tag == tag4("BevE")) {
            st.hasBevel = true;
            render::BevelStyle& b = st.bevel;
            const auto beve = enumOf(g, fx, "Beve");
            b.style = beve && *beve == 1 ? 0 : (beve && *beve == 2 ? 2 : (beve && *beve == 3 ? 3 : 1));
            b.angle = static_cast<float>(f64(fx, "Azim", 2.356194490192345) * 180.0 /
                                         3.14159265358979323846);
            b.altitude = static_cast<float>(f64(fx, "Elev", 0.7853981633974483) * 180.0 /
                                            3.14159265358979323846);
            b.size = blurRadius;
            b.soften = static_cast<float>(f64(fx, "Sftn", 0.0)) * s;
            const float depth = static_cast<float>(f64(fx, "Dept", 5.0)) * s;
            const float sign = boolOf(g, fx, "Invt").value_or(false) ? -1.0f : 1.0f;
            b.depth = sign * std::clamp(depth / std::max(radius, 1e-3f), 0.0f, 1.0f);
            b.highlight = fxColor(g, g.child(fx, "HiCl")).value_or(render::StyleColor{1.0f, 1.0f, 1.0f, 1.0f});
            b.highlightBlend = haveBlend ? blend : render::StyleBlend::Screen;
            b.highlightOpacity = opacity;
            b.shadow = fxColor(g, g.child(fx, "ShCl")).value_or(render::StyleColor{0.0f, 0.0f, 0.0f, 1.0f});
            if (const auto sb = enumVerOf(g, fx, "ShBM"))
                b.shadowBlend = fxBlendOf(sb->first, sb->second);
            b.shadowOpacity = static_cast<float>(f64(fx, "ShOp", 0.75));
        } else if (tag == tag4("GrdO")) {
            render::StyleColor from, to;
            bool radial = false;
            Node* fill = g.child(fx, "GrFl");
            if (fill) fill = g.child(fill, "FDeF");
            if (!fxGradientStops(g, fill, from, to, radial)) {
                skipped.push_back("GrdO");
                continue;
            }
            st.hasGradient = true;
            st.gradient.from = from;
            st.gradient.to = to;
            st.gradient.blend = haveBlend ? blend : render::StyleBlend::Normal;
            st.gradient.opacity = opacity;
            st.gradient.angle = 0.0f;  // the ramp runs left to right, at the panel default
            st.gradient.radial = radial;
        } else if (tag == tag4("Gaus")) {
            st.hasBlur = true;
            st.blur.radius = radius * 0.60f;
            st.blur.preserveAlpha = boolOf(g, fx, "PrAl").value_or(false);
        } else {
            skipped.push_back(tagName(tag));
        }
    }
    return st;
}


// The last occurrence of a float field. Shape classes repeat a tag across
// their base-class sections (a base default, then the derived value); the last
// one is the one that renders.
std::optional<float> f32Last(const Node* n, const char* name) {
    if (!n) return std::nullopt;
    const std::uint32_t t = tag4(name);
    for (auto it = n->fields.rbegin(); it != n->fields.rend(); ++it) {
        if (it->first != t) continue;
        if (it->second.k == Value::K::F32 || it->second.k == Value::K::F64)
            return static_cast<float>(it->second.f);
        return std::nullopt;
    }
    return std::nullopt;
}


std::optional<std::uint16_t> u16Of(const Graph& g, const Node* n, const char* name) {
    const Value* v = g.field(n, name);
    if (!v) return std::nullopt;
    if (v->k == Value::K::U16 || v->k == Value::K::U32)
        return static_cast<std::uint16_t>(v->u & 0xffffu);
    if (v->k == Value::K::I32) {
        const std::int32_t i = static_cast<std::int32_t>(v->u);
        if (i >= 0 && i <= 0xffff) return static_cast<std::uint16_t>(i);
    }
    return std::nullopt;
}


std::array<float, 3> hslToRgb(float h, float s, float l) {
    const float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
    const float hp = std::fmod(std::fmod(h, 1.0f) + 1.0f, 1.0f) * 6.0f;
    const float x = c * (1.0f - std::abs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    switch (static_cast<std::uint32_t>(hp)) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    const float m = l - c / 2.0f;
    return {r + m, g + m, b + m};
}


// A colour class (`RGBA`/`HSLA`/`GRAY`/`CMYK`) as RGBA bytes.
std::optional<std::array<std::uint8_t, 4>> vectorColorBytes(const Graph& g,
                                                            const Node* colr) {
    if (!colr) return std::nullopt;
    const Value* v = g.field(colr, "_col");
    if (!v || v->k != Value::K::Struct) {
        // A spot/global colour keeps the resolved values in `srgb` (and the
        // document-space source in `base`) instead of carrying `_col` itself.
        for (const char* key : {"srgb", "base"}) {
            if (const Node* inner = g.child(colr, key)) {
                if (auto c = vectorColorBytes(g, inner)) return c;
            }
        }
        return std::nullopt;
    }
    const std::vector<std::uint8_t>& raw = v->bytes;
    auto f = [&](std::size_t i) -> float {
        float x = 0.0f;
        if ((i + 1) * 4 <= raw.size()) std::memcpy(&x, raw.data() + i * 4, 4);
        return x;
    };
    auto to = [](float x) {
        return static_cast<std::uint8_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    const std::uint32_t tag = g.typeTag(colr);
    if (tag == tag4("RGBA") && raw.size() == 16)
        return std::array<std::uint8_t, 4>{to(f(0)), to(f(1)), to(f(2)), to(f(3))};
    if (tag == tag4("HSLA") && raw.size() == 16) {
        const auto rgb = hslToRgb(f(0), f(1), f(2));
        return std::array<std::uint8_t, 4>{to(rgb[0]), to(rgb[1]), to(rgb[2]), to(f(3))};
    }
    if (tag == tag4("GRAY") && raw.size() == 8)
        return std::array<std::uint8_t, 4>{to(f(0)), to(f(0)), to(f(0)), to(f(1))};
    if (tag == tag4("CMYK") && raw.size() == 20) {
        const float k = f(3);
        return std::array<std::uint8_t, 4>{to((1.0f - f(0)) * (1.0f - k)),
                                           to((1.0f - f(1)) * (1.0f - k)),
                                           to((1.0f - f(2)) * (1.0f - k)), to(f(4))};
    }
    if (raw.size() == 4)
        return std::array<std::uint8_t, 4>{raw[0], raw[1], raw[2], raw[3]};
    // Spot-colour endpoints are stored as RGBA floats without a colour class
    // tag; their wrapper is the `srgb`/`base` node selected above.
    if (raw.size() == 16)
        return std::array<std::uint8_t, 4>{to(f(0)), to(f(1)), to(f(2)), to(f(3))};
    return std::nullopt;
}


// The colour of a solid fill class ("FilS"): its `Colr` child. "None" fills
// and gradients give nothing.
std::optional<std::array<std::uint8_t, 4>> fillColorBytes(const Graph& g,
                                                          const Node* fill) {
    return fill ? vectorColorBytes(g, g.child(fill, "Colr")) : std::nullopt;
}


std::vector<pittore::vector::GradientStop> vectorGradientStops(const Graph& g,
                                                                const Node* fill) {
    Node* grad = fill ? g.child(fill, "Grad") : nullptr;
    if (!grad) return {};
    std::vector<float> positions;
    const Value* p = g.field(grad, "Posn");
    if (p && p->k == Value::K::Array)
        for (const Value& v : p->arr)
            if (v.k == Value::K::VecD && !v.vd.empty())
                positions.push_back(static_cast<float>(v.vd[0]));
    std::vector<std::array<std::uint8_t, 4>> colors;
    for (Node* c : g.children(grad, "Cols"))
        if (auto col = vectorColorBytes(g, c)) colors.push_back(*col);
    if (positions.size() != colors.size() || positions.size() < 2) return {};
    std::vector<pittore::vector::GradientStop> out;
    for (std::size_t i = 0; i < positions.size(); ++i)
        out.push_back({positions[i], colors[i]});
    return out;
}


// Read a gradient off a fill class ("FilG"). `host` is the fill descriptor
// when one wraps it (newer files hang the gradient transform there; older
// ones put it on the fill itself).
std::optional<pittore::vector::GradientFill> vectorGradientFill(const Graph& g,
                                                                 const Node* fill,
                                                                 const Node* host) {
    if (!fill || g.typeTag(fill) != tag4("FilG")) return std::nullopt;
    const auto type = enumOf(g, fill, "Type");
    const bool radial = type && *type >= 2;
    auto stops = vectorGradientStops(g, fill);
    if (stops.empty()) return std::nullopt;
    const Value* m = host ? g.field(host, "FDeX") : nullptr;
    if (!m || m->k != Value::K::VecD) m = g.field(fill, "FDeX");
    if (!m || m->k != Value::K::VecD || m->vd.size() < 6) return std::nullopt;
    pittore::vector::GradientFill gf;
    gf.stops = std::move(stops);
    gf.radial = radial;
    gf.startX = m->vd[2];
    gf.startY = m->vd[5];
    gf.endX = m->vd[0] + m->vd[2];
    gf.endY = m->vd[3] + m->vd[5];
    return gf;
}

}  // namespace af_detail
}  // namespace pittore::io
