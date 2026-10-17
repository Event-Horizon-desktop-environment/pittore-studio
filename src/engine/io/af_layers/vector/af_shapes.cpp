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


// Rebuild a shape's outline in its local bounds box from its parameters.
// Kinds whose geometry cannot be rebuilt return nullopt rather than a guess.
std::optional<ShapeGeometry> shapeGeometry(const Graph& g, const Node* shpe,
                                           float x0, float y0, float x1, float y1) {
    using namespace pittore::vector;
    const float w = x1 - x0, h = y1 - y0;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const auto closed = [](std::vector<Anchor> a) {
        ShapeSubPaths s;
        s.push_back({std::move(a), true});
        return s;
    };
    const auto f = [&](const char* t, float d) {
        const auto v = f32Last(shpe, t);
        return v ? *v : d;
    };
    const std::uint32_t tag = g.typeTag(shpe);

    if (tag == tag4("ShNR") || tag == tag4("ShRR")) {
        std::array<float, 4> radii = {0.0f, 0.0f, 0.0f, 0.0f};
        const Value* scr = g.field(shpe, "ShCR");
        if (scr && scr->k == Value::K::VecF && scr->vf.size() >= 4)
            radii = {scr->vf[0], scr->vf[1], scr->vf[2], scr->vf[3]};
        else if (scr && scr->k == Value::K::VecF && !scr->vf.empty())
            radii = {scr->vf[0], scr->vf[0], scr->vf[0], scr->vf[0]};
        // Designer's rectangle tool keeps default radii in ShCR but only
        // shapes corners whose type says so. A CTyp-less ShNR renders sharp.
        std::array<std::uint16_t, 4> cornerTypes = {0, 0, 0, 0};
        const Value* ctyp = g.field(shpe, "CTyp");
        const bool hasCtyp = ctyp && ctyp->k == Value::K::Array;
        // Corner type encodings come in two generations, flagged by the
        // record's "Lgcy" marker. The current enum is 0 = rounded arc, 1 =
        // bevel, 2 = inverted curve, 3 = notch; a square corner is simply a
        // zero radius. Legacy documents (observed in the Disney birthday-card
        // template: a ShNR v0 rectangle carrying CTyp [0,4,4,4] whose card
        // background rounds three corners and keeps one sharp) instead store
        // 0 = square and 4 = rounded arc, and except for those two codes the
        // meaning of a legacy value is unknown, so it renders sharp rather
        // than guessed.
        const bool legacyEnums = boolOf(g, shpe, "Lgcy").value_or(false);
        if (hasCtyp) {
            for (int i = 0; i < 4; ++i) {
                const Value* it =
                    static_cast<std::size_t>(i) < ctyp->arr.size() ? &ctyp->arr[i] : nullptr;
                if (!it || it->k != Value::K::Enum) {
                    radii[i] = 0.0f;
                    continue;
                }
                if (legacyEnums) {
                    if (it->u == 4) cornerTypes[i] = 0;  // legacy rounded arc
                    else radii[i] = 0.0f;                // square or unknown
                } else if (it->u <= 3) {
                    cornerTypes[i] = static_cast<std::uint16_t>(it->u);
                } else {
                    radii[i] = 0.0f;
                }
            }
        } else if (tag == tag4("ShNR")) {
            radii = {0.0f, 0.0f, 0.0f, 0.0f};
        }
        if (boolOf(g, shpe, "Lock").value_or(false)) {
            radii = {radii[0], radii[0], radii[0], radii[0]};
            cornerTypes = {cornerTypes[0], cornerTypes[0], cornerTypes[0], cornerTypes[0]};
        }
        const float shorter = std::min(w, h);
        const float scale = boolOf(g, shpe, "AbSz").value_or(false)
                                ? 1.0f
                                : (hasCtyp ? shorter : shorter * 0.5f);
        for (float& r : radii) r = std::clamp(r * scale, 0.0f, shorter * 0.5f);
        const bool sharp = radii[0] < 0.25f && radii[1] < 0.25f &&
                           radii[2] < 0.25f && radii[3] < 0.25f;
        ShapeGeometry out;
        out.name = sharp ? "Rectangle" : "Rounded Rectangle";
        out.subpaths = closed(corneredRectAnchors(x0, y0, x1, y1, radii, cornerTypes));
        return out;
    }
    if (tag == tag4("ShpE")) {
        ShapeGeometry out;
        out.name = "Ellipse";
        out.subpaths = closed(ellipseAnchors(x0, y0, x1, y1));
        return out;
    }
    if (tag == tag4("ShSt")) {
        const float cr = std::clamp(f("CrvR", 0.0f), -1.0f, 1.0f);
        const float cl = std::clamp(f("CrvL", 0.0f), -1.0f, 1.0f);
        const int points = std::clamp<int>(u16Of(g, shpe, "Pnts").value_or(5), 3, 100);
        const float inner = std::clamp(f("IRad", 0.5f), 0.0f, 1.0f);
        std::vector<Anchor> anchors;
        anchors.reserve(static_cast<std::size_t>(points) * 2);
        for (int i = 0; i < points * 2; ++i) {
            const float r = (i % 2 == 0) ? 1.0f : inner;
            const float ang = -1.5707963267948966f +
                              3.141592653589793f * static_cast<float>(i) /
                                  static_cast<float>(points);
            anchors.push_back(unitAnchor(std::cos(ang) * r, std::sin(ang) * r, x0, y0, x1, y1));
        }
        const int n = static_cast<int>(anchors.size());
        for (int i = 0; i < n; ++i) {
            const float c = (i % 2 == 0) ? cr : cl;
            if (std::abs(c) < 0.005f) continue;
            Anchor& a = anchors[i];
            Anchor& b = anchors[(i + 1) % n];
            const float dx = b.px - a.px, dy = b.py - a.py;
            const float nx = dy, ny = -dx;  // left of travel in screen space
            const float len = std::max(std::hypot(nx, ny), 1e-6f);
            const float sag = c * std::hypot(dx, dy) * 0.22f;
            const float mx = (a.px + b.px) * 0.5f + nx / len * 2.0f * sag;
            const float my = (a.py + b.py) * 0.5f + ny / len * 2.0f * sag;
            a.hox = (mx - a.px) * (2.0f / 3.0f);
            a.hoy = (my - a.py) * (2.0f / 3.0f);
            b.hix = (mx - b.px) * (2.0f / 3.0f);
            b.hiy = (my - b.py) * (2.0f / 3.0f);
        }
        ShapeGeometry out;
        out.name = "Star";
        out.subpaths = closed(std::move(anchors));
        return out;
    }
    if (tag == tag4("ShSS")) {
        const int sides = std::clamp<int>(u16Of(g, shpe, "Side").value_or(4), 3, 100);
        const float cut = std::clamp(f32Last(shpe, "COut").value_or(0.5f), 0.01f, 0.99f);
        ShapeGeometry out;
        out.name = "Square Star";
        out.subpaths = closed(squareStarAnchors(static_cast<std::uint32_t>(sides), cut, x0, y0, x1, y1));
        return out;
    }
    if (tag == tag4("ShCl")) {
        const int bubbles = std::clamp<int>(u16Of(g, shpe, "Bubl").value_or(12), 3, 100);
        const float meet = std::clamp(f32Last(shpe, "IRad").value_or(0.8f), 0.1f, 0.999f);
        ShapeGeometry out;
        out.name = "Cloud";
        out.subpaths = closed(cloudAnchors(static_cast<std::uint32_t>(bubbles), meet, x0, y0, x1, y1));
        return out;
    }
    if (tag == tag4("ShHt")) {
        const float spread = std::clamp(f32Last(shpe, "Sprd").value_or(0.2f), 0.0f, 1.0f);
        ShapeGeometry out;
        out.name = "Heart";
        out.subpaths = closed(heartAnchors(x0, y0, x1, y1, spread));
        return out;
    }
    if (tag == tag4("ShpT")) {
        const float pos = std::clamp(f("Pos ", 0.5f), 0.0f, 1.0f);
        ShapeGeometry out;
        out.name = "Triangle";
        out.subpaths = closed({Anchor::corner(x0 + pos * w, y0), Anchor::corner(x1, y1),
                               Anchor::corner(x0, y1)});
        return out;
    }
    if (tag == tag4("ShpD")) {
        const float pos = std::clamp(f("Pos ", 0.5f), 0.0f, 1.0f);
        const float ym = y0 + pos * h;
        ShapeGeometry out;
        out.name = "Diamond";
        out.subpaths = closed({Anchor::corner(cx, y0), Anchor::corner(x1, ym),
                               Anchor::corner(cx, y1), Anchor::corner(x0, ym)});
        return out;
    }
    if (tag == tag4("ShTz")) {
        const float l = std::clamp(f("PosL", 0.25f), 0.0f, 1.0f);
        const float r = std::clamp(f("PosR", 0.75f), 0.0f, 1.0f);
        ShapeGeometry out;
        out.name = "Trapezoid";
        out.subpaths = closed({Anchor::corner(x0 + l * w, y0), Anchor::corner(x0 + r * w, y0),
                               Anchor::corner(x1, y1), Anchor::corner(x0, y1)});
        return out;
    }
    if (tag == tag4("ShPy")) {
        if (std::abs(f("Curv", 0.0f)) > 0.01f) return std::nullopt;
        const int sides = std::clamp<int>(u16Of(g, shpe, "Side").value_or(5), 3, 100);
        std::vector<Anchor> anchors;
        anchors.reserve(static_cast<std::size_t>(sides));
        for (int i = 0; i < sides; ++i) {
            const float ang = -1.5707963267948966f +
                              6.283185307179586f * static_cast<float>(i) /
                                  static_cast<float>(sides);
            anchors.push_back(unitAnchor(std::cos(ang), std::sin(ang), x0, y0, x1, y1));
        }
        ShapeGeometry out;
        out.name = "Polygon";
        out.subpaths = closed(std::move(anchors));
        return out;
    }
    if (tag == tag4("ShDS")) {
        const int points = std::clamp<int>(u16Of(g, shpe, "Pnts").value_or(5), 2, 100);
        const float inner = std::clamp(f("IRad", 0.5f), 0.0f, 1.0f);
        const float mid = std::clamp(f("PRad", 0.8f), 0.0f, 1.0f);
        const float radii[4] = {1.0f, inner, mid, inner};
        std::vector<Anchor> anchors;
        anchors.reserve(static_cast<std::size_t>(points) * 4);
        for (int i = 0; i < points * 4; ++i) {
            const float r = radii[i % 4];
            const float ang = -1.5707963267948966f +
                              6.283185307179586f * static_cast<float>(i) /
                                  static_cast<float>(points * 4);
            anchors.push_back(unitAnchor(std::cos(ang) * r, std::sin(ang) * r, x0, y0, x1, y1));
        }
        ShapeGeometry out;
        out.name = "Double Star";
        out.subpaths = closed(std::move(anchors));
        return out;
    }
    if (tag == tag4("ShPi")) {
        const float angS = f("AngS", 0.0f);
        const float angE = f("AngE", 0.0f);
        const float inner = std::clamp(f("IRad", 0.0f), 0.0f, 0.999f);
        const float rx = w * 0.5f, ry = h * 0.5f;
        ShapeGeometry out;
        if (std::abs(angS - angE) < 1e-4f) {
            out.name = inner > 0.001f ? "Donut" : "Ellipse";
            out.subpaths.push_back({ellipseAnchors(x0, y0, x1, y1), true});
            if (inner > 0.001f)
                out.subpaths.push_back({ellipseAnchors(cx - rx * inner, cy - ry * inner,
                                                       cx + rx * inner, cy + ry * inner),
                                        true});
            return out;
        }
        // The canvas points its y axis downward, so the recorded angles are
        // flipped to match before they are walked.
        const float t0 = -angS;
        const float t1 = -(angE + (angE <= angS ? 6.283185307179586f : 0.0f));
        std::vector<Anchor> anchors = arcAnchors(cx, cy, rx, ry, t0, t1);
        if (inner > 0.001f) {
            std::vector<Anchor> back =
                arcAnchors(cx, cy, rx * inner, ry * inner, t1, t0);
            anchors.insert(anchors.end(), back.begin(), back.end());
        } else {
            anchors.push_back(Anchor::corner(cx, cy));
        }
        out.name = "Pie";
        out.subpaths = closed(std::move(anchors));
        return out;
    }
    if (tag == tag4("ShSg")) {
        const float pos0 = std::clamp(f("Pos0", 0.25f), 0.0f, 1.0f);
        const float uy = std::clamp(1.0f - 2.0f * pos0, -1.0f, 1.0f);
        const float a = std::asin(uy);
        const float rx = w * 0.5f, ry = h * 0.5f;
        ShapeGeometry out;
        out.name = "Segment";
        out.subpaths = closed(arcAnchors(cx, cy, rx, ry, 3.141592653589793f - a,
                                         6.283185307179586f + a));
        return out;
    }
    if (tag == tag4("ShCr")) {
        std::vector<Anchor> unit = bowArcUnit(f("ArcL", -1.0f), true);
        std::vector<Anchor> up = bowArcUnit(f("ArcR", -0.3f), false);
        unit.insert(unit.end(), up.begin(), up.end());
        for (Anchor& a : unit) {
            a.px = x0 + a.px * w;
            a.py = y0 + a.py * h;
            a.hix *= w;
            a.hiy *= h;
            a.hox *= w;
            a.hoy *= h;
        }
        ShapeGeometry out;
        out.name = "Crescent";
        out.subpaths = closed(std::move(unit));
        return out;
    }
    if (tag == tag4("ShDA")) {
        const float sh = std::clamp(f("Thck", 0.35f), 0.0f, 1.0f) * h * 0.5f;
        const auto head = [&](const char* name) {
            const Value* v = g.field(shpe, name);
            if (v && v->k == Value::K::Enum) return v->u != 0;
            return true;
        };
        const bool lHead = head("LSty");
        const bool rHead = head("RSty");
        const float lw = lHead ? std::min(f("LPr1", 0.5f) * h, w * 0.45f) : 0.0f;
        const float rw = rHead ? std::min(f("RPr1", 0.5f) * h, w * 0.45f) : 0.0f;
        std::vector<Anchor> a;
        if (lHead) {
            a.push_back(Anchor::corner(x0, cy));
            a.push_back(Anchor::corner(x0 + lw, y0));
            a.push_back(Anchor::corner(x0 + lw, cy - sh));
        } else {
            a.push_back(Anchor::corner(x0, cy - sh));
        }
        if (rHead) {
            a.push_back(Anchor::corner(x1 - rw, cy - sh));
            a.push_back(Anchor::corner(x1 - rw, y0));
            a.push_back(Anchor::corner(x1, cy));
            a.push_back(Anchor::corner(x1 - rw, y1));
            a.push_back(Anchor::corner(x1 - rw, cy + sh));
        } else {
            a.push_back(Anchor::corner(x1, cy - sh));
            a.push_back(Anchor::corner(x1, cy + sh));
        }
        if (lHead) {
            a.push_back(Anchor::corner(x0 + lw, cy + sh));
            a.push_back(Anchor::corner(x0 + lw, y1));
        } else {
            a.push_back(Anchor::corner(x0, cy + sh));
        }
        ShapeGeometry out;
        out.name = "Arrow";
        out.subpaths = closed(std::move(a));
        return out;
    }
    if (tag == tag4("ShCg")) {
        if (std::abs(f("Curv", 0.0f)) > 0.01f) return std::nullopt;
        const int teeth = std::clamp<int>(u16Of(g, shpe, "Teth").value_or(12), 3, 200);
        const float root = std::clamp(f("IRad", 0.85f), 0.0f, 1.0f);
        const float hole = std::clamp(f("Hole", 0.2f), 0.0f, 0.999f);
        const float ts = std::clamp(f("TtSz", 0.37f), 0.0f, 1.0f);
        const float ns = std::clamp(f("NtSz", 0.42f), 0.0f, 1.0f);
        const float step = 6.283185307179586f / static_cast<float>(teeth);
        std::vector<Anchor> a;
        a.reserve(static_cast<std::size_t>(teeth) * 4);
        for (int k = 0; k < teeth; ++k) {
            const float c = -1.5707963267948966f + step * static_cast<float>(k);
            const float gp = c + step * 0.5f;
            const float rr[4] = {1.0f, 1.0f, root, root};
            const float aa[4] = {c - ts * step * 0.5f, c + ts * step * 0.5f,
                                 gp - ns * step * 0.5f, gp + ns * step * 0.5f};
            for (int i = 0; i < 4; ++i)
                a.push_back(unitAnchor(std::cos(aa[i]) * rr[i], std::sin(aa[i]) * rr[i],
                                       x0, y0, x1, y1));
        }
        ShapeGeometry out;
        out.name = "Cog";
        out.subpaths.push_back({std::move(a), true});
        if (hole > 0.001f) {
            const float hx = w * 0.5f * hole, hy = h * 0.5f * hole;
            out.subpaths.push_back(
                {ellipseAnchors(cx - hx, cy - hy, cx + hx, cy + hy), true});
        }
        return out;
    }
    if (tag == tag4("ShCR")) {
        const float tailH = std::clamp(f("TlHg", 0.3f), 0.0f, 0.95f);
        const float tailW = std::clamp(f("TlWd", 0.15f), 0.0f, 1.0f);
        const float root = std::clamp(f("TlRP", 0.4f), 0.0f, 1.0f);
        const float tip = std::clamp(f("TlEP", 0.2f), 0.0f, 1.0f);
        const float yr = y1 - tailH * h;
        std::array<float, 4> radii = {0.25f, 0.25f, 0.25f, 0.25f};
        const Value* scr = g.field(shpe, "ShCR");
        if (scr && scr->k == Value::K::VecF && scr->vf.size() >= 4)
            radii = {scr->vf[0], scr->vf[1], scr->vf[2], scr->vf[3]};
        const float balloonH = yr - y0;
        const float shorter = std::min(w, balloonH);
        const float scale = boolOf(g, shpe, "AbSz").value_or(false) ? 1.0f : shorter;
        for (float& r : radii) r = std::clamp(r * scale, 0.0f, shorter * 0.5f);
        std::vector<Anchor> a = roundedRectAnchors(x0, y0, x1, yr, radii);
        std::size_t afterBr = a.size();
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].py >= yr - 0.01f && a[i].px > cx) {
                afterBr = i + 1;
                break;
            }
        const float half = tailW * w * 0.5f;
        const float rc = x0 + root * w;
        a.insert(a.begin() + static_cast<std::ptrdiff_t>(afterBr),
                 {Anchor::corner(std::min(rc + half, x1), yr),
                  Anchor::corner(x0 + tip * w, y1),
                  Anchor::corner(std::max(rc - half, x0), yr)});
        ShapeGeometry out;
        out.name = "Callout";
        out.subpaths = closed(std::move(a));
        return out;
    }
    if (tag == tag4("ShCE")) {
        const float tailH = std::clamp(f("TlHg", 0.2f), 0.0f, 0.95f);
        const float tipX = x0 + std::clamp(f("TlEP", 0.15f), 0.0f, 1.0f) * w;
        const float halfAng = std::clamp(f("TlAn", 0.35f) * 0.5f, 0.02f, 1.5f);
        const float yr = y1 - tailH * h;
        const float rx = w * 0.5f, ry = (yr - y0) * 0.5f;
        const float cey = (y0 + yr) * 0.5f;
        const float tDir =
            std::atan2((y1 - cey) / ry, (tipX - cx) / rx);
        std::vector<Anchor> a =
            arcAnchors(cx, cey, rx, ry, tDir + halfAng,
                       tDir - halfAng + 6.283185307179586f);
        a.push_back(Anchor::corner(tipX, y1));
        ShapeGeometry out;
        out.name = "Callout";
        out.subpaths = closed(std::move(a));
        return out;
    }
    if (tag == tag4("ShTr")) {
        const float tail = std::clamp(f("Tail", 0.5f), 0.05f, 0.95f);
        const float ym = y0 + std::min(tail * 1.03f, 0.9f) * h;
        const float hw = w * 0.5f;
        const float dx = 0.410f * hw, dy = 0.159f * h;
        const float v = 0.161f * h;
        Anchor apex = Anchor::corner(cx, y0);
        apex.hix = dx;
        apex.hiy = dy;
        apex.hox = -dx;
        apex.hoy = dy;
        std::vector<Anchor> a = {apex};
        std::vector<Anchor> bottom =
            arcAnchors(cx, ym, hw, y1 - ym, 3.141592653589793f, 0.0f);
        if (!bottom.empty()) {
            bottom.front().hix = 0.0f;
            bottom.front().hiy = -v;
            bottom.back().hox = 0.0f;
            bottom.back().hoy = -v;
        }
        a.insert(a.end(), bottom.begin(), bottom.end());
        ShapeGeometry out;
        out.name = "Tear";
        out.subpaths = closed(std::move(a));
        return out;
    }
    return std::nullopt;
}


void transformAnchors(pittore::vector::VectorPath& path, const Mat& m) {
    for (auto& sub : path.subpaths) {
        for (auto& a : sub.anchors) {
            const auto p = matApply(m, a.px, a.py);
            const double hix = m.m[0] * a.hix + m.m[1] * a.hiy;
            const double hiy = m.m[3] * a.hix + m.m[4] * a.hiy;
            const double hox = m.m[0] * a.hox + m.m[1] * a.hoy;
            const double hoy = m.m[3] * a.hox + m.m[4] * a.hoy;
            a.px = static_cast<float>(p.first);
            a.py = static_cast<float>(p.second);
            a.hix = static_cast<float>(hix);
            a.hiy = static_cast<float>(hiy);
            a.hox = static_cast<float>(hox);
            a.hoy = static_cast<float>(hoy);
        }
    }
}


std::optional<pittore::vector::VectorPath> pathFromCurveRecords(const Graph& g, const Node* node,
                                                                const Mat& lctm) {
    using namespace pittore::vector;
    Node* crvs = g.child(node, "Crvs");
    Node* data = crvs ? g.child(crvs, "Data") : nullptr;
    if (!data || data->fields.empty()) return std::nullopt;
    const auto toDoc = [&](double x, double y) {
        const auto p = matApply(lctm, x, y);
        return std::make_pair(static_cast<float>(p.first), static_cast<float>(p.second));
    };
    // Records run: a closed flag, then per subpath an 18-byte stream
    // (two f64s and a marker pair). The flag resets for the next one.
    VectorPath path;
    bool closed = true;
    for (const auto& fld : data->fields) {
        const Value& v = fld.second;
        if (v.k == Value::K::Bool) {
            closed = v.u != 0;
            continue;
        }
        if (v.k != Value::K::Array) continue;
        bool hasCurve = false;
        for (const Value& item : v.arr)
            if (item.k == Value::K::Struct) {
                hasCurve = true;
                break;
            }
        if (!hasCurve) continue;
        std::vector<PathRecord> records;
        bool malformed = false;
        for (const Value& item : v.arr) {
            if (item.k != Value::K::Struct) continue;
            if (item.bytes.size() != 18) {
                malformed = true;
                break;
            }
            double x = 0.0, y = 0.0;
            std::memcpy(&x, item.bytes.data(), 8);
            std::memcpy(&y, item.bytes.data() + 8, 8);
            const auto p = toDoc(x, y);
            records.push_back(PathRecord{p.first, p.second, item.bytes[16], item.bytes[17]});
        }
        if (malformed) return std::nullopt;
        if (auto sub = subpathFromRecords(records, closed))
            path.subpaths.push_back(std::move(*sub));
        closed = true;
    }
    if (path.isEmpty()) return std::nullopt;
    return path;
}


// Resolve a shape/path node's paint: solid fill or gradient, then stroke.
// Returns nullopt for a layer with nothing to paint. A gradient's axis is
// transformed into document space alongside the path.
std::optional<pittore::vector::VectorShape> vectorPaint(
    const Graph& g, const Node* node, const Mat& ctm,
    pittore::vector::VectorPath path, bool evenOdd,
    std::optional<pittore::vector::GradientFill>& gradient) {
    using namespace pittore::vector;
    const std::vector<Node*> fillDesc = g.children(node, "BFFl");
    Node* fillNode = fillDesc.empty() ? nullptr : g.child(fillDesc.front(), "FDeF");
    if (!fillNode) fillNode = g.child(node, "BFil");
    std::optional<std::array<std::uint8_t, 4>> fill = fillColorBytes(g, fillNode);
    if (!fill && fillNode)
        gradient = vectorGradientFill(g, fillNode,
                                      fillDesc.empty() ? nullptr : fillDesc.front());
    if (gradient) {
        const auto s = matApply(ctm, gradient->startX, gradient->startY);
        const auto e = matApply(ctm, gradient->endX, gradient->endY);
        gradient->startX = s.first;
        gradient->startY = s.second;
        gradient->endX = e.first;
        gradient->endY = e.second;
    }

    const std::vector<Node*> strokeDesc = g.children(node, "LIFl");
    Node* strokeNode = strokeDesc.empty() ? nullptr : g.child(strokeDesc.front(), "FDeF");
    if (!strokeNode) strokeNode = g.child(node, "PFil");
    const std::optional<std::array<std::uint8_t, 4>> stroke = fillColorBytes(g, strokeNode);

    const std::vector<Node*> lines = g.children(node, "LILn");
    Node* line = lines.empty() ? g.child(node, "LSty") : lines.front();
    float strokeWidth = 0.0f;
    if (line) {
        // "LDeL" carries the line's dash pattern (byte 10 is its style) and
        // its weight; a zero style means no line at all.
        Node* ldel = g.child(line, "LDeL");
        if (ldel) {
            const Value* data = g.field(ldel, "Data");
            const bool hasLine =
                !(data && data->k == Value::K::Struct && data->bytes.size() > 10 &&
                  data->bytes[10] == 0);
            const Value* wv = g.field(ldel, "Wght");
            if (hasLine && wv && wv->k == Value::K::F64)
                strokeWidth = static_cast<float>(
                    wv->f * (matScaleX(ctm) + matScaleY(ctm)) / 2.0);
        }
    }
    const bool hasStroke = stroke.has_value() && strokeWidth > 0.05f;
    if (!fill && !gradient && !hasStroke) return std::nullopt;

    VectorShape shape;
    shape.path = std::move(path);
    shape.fill = fill.value_or(std::array<std::uint8_t, 4>{0, 0, 0, 0});
    shape.evenOdd = evenOdd;
    if (hasStroke) {
        shape.hasStroke = true;
        shape.stroke = *stroke;
        shape.strokeWidth = strokeWidth;
    }
    return shape;
}


// The first `n` Unicode code points of a UTF-8 string, never splitting one.
std::string utf8Prefix(const std::string& s, std::size_t n) {
    std::size_t i = 0, count = 0;
    while (i < s.size() && count < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + len > s.size()) len = s.size() - i;
        i += len;
        ++count;
    }
    return s.substr(0, i);
}


std::string trimmed(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}


// A line count matching the way a story's line endings are counted: a trailing
// newline does not open another line, and paragraph breaks ('\v') are breaks
// like any other.
std::size_t textLineCount(const std::string& s) {
    if (s.empty()) return 0;
    std::size_t count = 0;
    for (char c : s)
        if (c == '\n' || c == '\v') ++count;
    if (s.back() != '\n' && s.back() != '\v') ++count;
    return count;
}

}  // namespace af_detail
}  // namespace pittore::io
