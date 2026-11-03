#include "ui/persona/vector_shapes.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "engine/vector/vector_art.h"
#include "engine/vector/vector_shape.h"
#include "ui/app_state.h"
#include "ui/persona/vector_edit.h"
#include "ui/persona/vector_profile.h"
#include "ui/persona/vector_build.h"
#include "ui/persona/vector_qr.h"
#include "ui/persona/vector_raster.h"
#include "ui/project_manager.h"
#include "ui/tool_registry.h"

#include <QPainterPath>

namespace pittore::ui {
namespace {

constexpr float kPi = 3.141592653589793f;
using pittore::vector::Anchor;
using pittore::vector::SubPath;
using pittore::vector::VectorPath;
using pittore::vector::VectorShape;

SubPath closedPath(std::vector<Anchor> anchors) {
    SubPath s;
    s.anchors = std::move(anchors);
    s.closed = true;
    return s;
}

SubPath openPath(std::vector<Anchor> anchors) {
    SubPath s;
    s.anchors = std::move(anchors);
    s.closed = false;
    return s;
}

Anchor pt(float x, float y) { return Anchor::corner(x, y); }

// Regular n-gon, point up, elliptically fitted (the .af format stretches too).
std::vector<Anchor> polygonAnchors(float cx, float cy, float rx, float ry,
                                   int n) {
    std::vector<Anchor> out;
    for (int i = 0; i < n; ++i) {
        const float a = -kPi / 2.0f + i * 2.0f * kPi / static_cast<float>(n);
        out.push_back(pt(cx + rx * std::cos(a), cy + ry * std::sin(a)));
    }
    return out;
}

// Pointed star: 2n alternating outer/inner corners.
std::vector<Anchor> starAnchors(float cx, float cy, float rx, float ry,
                                int n, float inner) {
    std::vector<Anchor> out;
    for (int i = 0; i < 2 * n; ++i) {
        const float a = -kPi / 2.0f + i * kPi / static_cast<float>(n);
        const float k = (i % 2 == 0) ? 1.0f : inner;
        out.push_back(pt(cx + rx * k * std::cos(a), cy + ry * k * std::sin(a)));
    }
    return out;
}

// Polar helper in local coords.
QPointF polar(float cx, float cy, float r, float a) {
    return QPointF(cx + r * std::cos(a), cy + r * std::sin(a));
}

}  // namespace

ShapeStyle shapeStyleFor(const AppState* state, ToolId tool) {
    ShapeStyle st;
    if (!state) return st;
    QColor fill = state->option(tool, QStringLiteral("fill")).value<QColor>();
    if (!fill.isValid()) fill = state->foreground();
    // Open-path tools (Line, Spiral) draw stroke-only: default the stroke to
    // the foreground so a fresh drag draws something. Closed shapes default
    // to transparent stroke (fill carries them), mirroring the wells.
    QColor stroke = state->option(tool, QStringLiteral("stroke")).value<QColor>();
    if (!stroke.isValid()) {
        stroke = (tool == ToolId::Line || tool == ToolId::Spiral)
                     ? state->foreground()
                     : QColor(0, 0, 0, 0);
    }
    st.hasFill = fill.alpha() > 0;
    st.fill = fill;
    // Line-family options call it "weight"; everything else "strokewidth".
    QVariant widthOpt = state->option(tool, QStringLiteral("strokewidth"));
    if (!widthOpt.isValid())
        widthOpt = state->option(tool, QStringLiteral("weight"));
    st.strokeWidth = widthOpt.isValid() ? widthOpt.toDouble() : 1.0;
    st.hasStroke = stroke.alpha() > 0 && st.strokeWidth > 0.0;
    st.stroke = stroke;
    st.cap = std::clamp(state->option(tool, QStringLiteral("cap")).toInt(), 0, 2);
    st.join = std::clamp(state->option(tool, QStringLiteral("join")).toInt(), 0, 2);
    st.radius = state->option(tool, QStringLiteral("radius")).toDouble();
    st.rtl = state->option(tool, QStringLiteral("radius_tl")).toDouble();
    st.rtr = state->option(tool, QStringLiteral("radius_tr")).toDouble();
    st.rbr = state->option(tool, QStringLiteral("radius_br")).toDouble();
    st.rbl = state->option(tool, QStringLiteral("radius_bl")).toDouble();
    st.cornerType = state->option(tool, QStringLiteral("corner_type")).toInt();
    st.sides = state->option(tool, QStringLiteral("sides")).toInt();
    st.indent = state->option(tool, QStringLiteral("indent")).toDouble();
    st.hole = state->option(tool, QStringLiteral("hole")).toDouble();
    st.startDeg = state->option(tool, QStringLiteral("start_angle")).toDouble();
    st.endDeg = state->option(tool, QStringLiteral("end_angle")).toDouble();
    st.skew = state->option(tool, QStringLiteral("skew")).toDouble();
    st.startArrow =
        state->option(tool, QStringLiteral("start_arrow")).toBool();
    st.endArrow = state->option(tool, QStringLiteral("end_arrow")).toBool();
    const QVariant aw = state->option(tool, QStringLiteral("arrow_width"));
    if (aw.isValid()) st.arrowWPct = aw.toDouble();
    const QVariant al = state->option(tool, QStringLiteral("arrow_length"));
    if (al.isValid()) st.arrowLPct = al.toDouble();
    st.qrContent = state->option(tool, QStringLiteral("qr_content")).toString();
    const QVariant qs = state->option(tool, QStringLiteral("qr_size"));
    if (qs.isValid()) st.qrSize = qs.toInt();
    st.qrEcc = state->option(tool, QStringLiteral("qr_ecc")).toInt();
    st.customshape =
        state->option(tool, QStringLiteral("customshape")).toInt();
    return st;
}

std::shared_ptr<pittore::vector::ArtNode> makeShapeArt(ToolId tool,
                                                        const QRectF& local,
                                                        const ShapeStyle& style) {
    const float x0 = static_cast<float>(local.x());
    const float y0 = static_cast<float>(local.y());
    const float w = static_cast<float>(local.width());
    const float h = static_cast<float>(local.height());
    if (!(w > 0.0f) || !(h > 0.0f)) return nullptr;
    const float x1 = x0 + w, y1 = y0 + h;
    const float cx = x0 + w / 2.0f, cy = y0 + h / 2.0f;

    VectorPath path;
    path.name = toolName(tool).toStdString();
    bool evenOdd = false;
    bool forceStrokeOnly = false;

    const auto cornerR = [&](double specific) {
        return static_cast<float>(specific >= 0.0 ? specific : style.radius);
    };

    switch (tool) {
        case ToolId::Rectangle:
        case ToolId::RoundedRectangle: {
            const std::array<float, 4> radii = {
                cornerR(style.rtl), cornerR(style.rtr), cornerR(style.rbr),
                cornerR(style.rbl)};
            const auto t = static_cast<std::uint16_t>(std::clamp(style.cornerType, 0, 3));
            path.subpaths.push_back(
                {pittore::vector::corneredRectAnchors(x0, y0, x1, y1, radii,
                                                       {t, t, t, t}),
                 true});
            break;
        }
        case ToolId::Ellipse:
            path.subpaths.push_back(
                {pittore::vector::ellipseAnchors(x0, y0, x1, y1), true});
            break;
        case ToolId::Triangle:
            path.subpaths.push_back(
                closedPath({pt(cx, y0), pt(x1, y1), pt(x0, y1)}));
            break;
        case ToolId::Diamond: {
            const float ym = (y0 + y1) / 2.0f;
            path.subpaths.push_back(
                closedPath({pt(cx, y0), pt(x1, ym), pt(cx, y1), pt(x0, ym)}));
            break;
        }
        case ToolId::Trapezoid:
            path.subpaths.push_back(closedPath({pt(x0 + 0.25f * w, y0),
                                                pt(x1 - 0.25f * w, y0),
                                                pt(x1, y1), pt(x0, y1)}));
            break;
        case ToolId::Polygon:
            path.subpaths.push_back(closedPath(
                polygonAnchors(cx, cy, w / 2.0f, h / 2.0f,
                               std::clamp(style.sides, 3, 100))));
            break;
        case ToolId::Hexagon:
            path.subpaths.push_back(
                closedPath(polygonAnchors(cx, cy, w / 2.0f, h / 2.0f, 6)));
            break;
        case ToolId::Octagon:
            path.subpaths.push_back(
                closedPath(polygonAnchors(cx, cy, w / 2.0f, h / 2.0f, 8)));
            break;
        case ToolId::Star:
            path.subpaths.push_back(closedPath(starAnchors(
                cx, cy, w / 2.0f, h / 2.0f, std::clamp(style.sides, 3, 100),
                static_cast<float>(std::clamp(style.indent, 1.0, 100.0) / 100.0))));
            break;
        case ToolId::Sparkle:
            path.subpaths.push_back(closedPath(starAnchors(
                cx, cy, w / 2.0f, h / 2.0f, std::clamp(style.sides, 3, 100),
                static_cast<float>(std::clamp(style.indent, 1.0, 100.0) / 100.0))));
            break;
        case ToolId::DoubleStar: {
            // Paired points read as a doubled star.
            const int n = std::clamp(style.sides, 3, 100);
            const float inner =
                static_cast<float>(std::clamp(style.indent, 1.0, 100.0) / 100.0);
            std::vector<Anchor> out;
            for (int i = 0; i < n; ++i) {
                const float a = -kPi / 2.0f + i * 2.0f * kPi / static_cast<float>(n);
                const float step = 2.0f * kPi / static_cast<float>(n);
                out.push_back(pt(cx + w / 2.0f * std::cos(a), cy + h / 2.0f * std::sin(a)));
                out.push_back(pt(cx + w / 2.0f * inner * std::cos(a + step * 0.25f),
                                  cy + h / 2.0f * inner * std::sin(a + step * 0.25f)));
                out.push_back(pt(cx + w / 2.0f * 0.9f * std::cos(a + step * 0.5f),
                                  cy + h / 2.0f * 0.9f * std::sin(a + step * 0.5f)));
                out.push_back(pt(cx + w / 2.0f * inner * std::cos(a + step * 0.75f),
                                  cy + h / 2.0f * inner * std::sin(a + step * 0.75f)));
            }
            path.subpaths.push_back(closedPath(std::move(out)));
            break;
        }
        case ToolId::SquareStar:
            path.subpaths.push_back(closedPath(
                pittore::vector::squareStarAnchors(
                    static_cast<std::uint32_t>(std::clamp(style.sides, 3, 100)),
                    static_cast<float>(std::clamp(style.indent, 1.0, 100.0) / 100.0),
                    x0, y0, x1, y1)));
            break;
        case ToolId::Arrow: {
            const float sh = 0.4f * h, hl = 0.35f * w, ym = (y0 + y1) / 2.0f;
            path.subpaths.push_back(closedPath(
                {pt(x0, ym - sh / 2), pt(x1 - hl, ym - sh / 2),
                 pt(x1 - hl, ym - sh), pt(x1, ym), pt(x1 - hl, ym + sh),
                 pt(x1 - hl, ym + sh / 2), pt(x0, ym + sh / 2)}));
            break;
        }
        case ToolId::DoubleArrow: {
            const float sh = 0.4f * h, hl = 0.25f * w, ym = (y0 + y1) / 2.0f;
            path.subpaths.push_back(closedPath(
                {pt(x0, ym), pt(x0 + hl, ym - sh), pt(x0 + hl, ym - sh / 2),
                 pt(x1 - hl, ym - sh / 2), pt(x1 - hl, ym - sh), pt(x1, ym),
                 pt(x1 - hl, ym + sh), pt(x1 - hl, ym + sh / 2),
                 pt(x0 + hl, ym + sh / 2), pt(x0 + hl, ym + sh)}));
            break;
        }
        case ToolId::Donut: {
            path.subpaths.push_back(
                {pittore::vector::ellipseAnchors(x0, y0, x1, y1), true});
            const float k =
                static_cast<float>(std::clamp(style.hole, 0.0, 100.0) / 100.0);
            const float ix0 = cx - w / 2.0f * k, ix1 = cx + w / 2.0f * k;
            const float iy0 = cy - h / 2.0f * k, iy1 = cy + h / 2.0f * k;
            path.subpaths.push_back(
                {pittore::vector::ellipseAnchors(ix0, iy0, ix1, iy1), true});
            evenOdd = true;
            break;
        }
        case ToolId::Pie:
        case ToolId::Segment: {
            const float t0 = static_cast<float>(style.startDeg * kPi / 180.0);
            const float t1 = static_cast<float>(style.endDeg * kPi / 180.0);
            float sweep = t1 - t0;
            while (sweep < 0.0f) sweep += 2.0f * kPi;
            if (sweep < 1e-3f || sweep >= 2.0f * kPi - 1e-3f) {
                path.subpaths.push_back(
                    {pittore::vector::ellipseAnchors(x0, y0, x1, y1), true});
            } else if (tool == ToolId::Pie) {
                auto arc = pittore::vector::arcAnchors(cx, cy, w / 2.0f,
                                                        h / 2.0f, t0, t0 + sweep);
                arc.push_back(pt(cx, cy));
                path.subpaths.push_back(closedPath(std::move(arc)));
            } else {
                auto arc = pittore::vector::arcAnchors(cx, cy, w / 2.0f,
                                                        h / 2.0f, t0, t0 + sweep);
                path.subpaths.push_back(closedPath(std::move(arc)));
            }
            break;
        }
        case ToolId::Crescent: {
            path.subpaths.push_back(
                {pittore::vector::ellipseAnchors(x0, y0, x1, y1), true});
            const float iw = w * 0.78f, ih = h * 0.78f;
            const float ix0 = cx + w * 0.30f - iw / 2.0f;
            const float iy0 = cy - h * 0.22f - ih / 2.0f;
            path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                         ix0, iy0, ix0 + iw, iy0 + ih),
                                     true});
            evenOdd = true;
            break;
        }
        case ToolId::Cog: {
            // Rectangular teeth: outer flat, drop, inner flat, rise per tooth.
            const int n = std::clamp(style.sides, 3, 100);
            const float step = 2.0f * kPi / static_cast<float>(n);
            const float rIn = 0.72f;
            std::vector<Anchor> out;
            for (int i = 0; i < n; ++i) {
                const float a = i * step;
                for (const auto& [frac, rad] :
                     {std::pair<float, float>{0.0f, 1.0f},
                      std::pair<float, float>{0.4f, 1.0f},
                      std::pair<float, float>{0.5f, rIn},
                      std::pair<float, float>{0.9f, rIn}}) {
                    const float t = a + frac * step;
                    out.push_back(pt(cx + w / 2.0f * rad * std::cos(t),
                                     cy + h / 2.0f * rad * std::sin(t)));
                }
            }
            path.subpaths.push_back(closedPath(std::move(out)));
            const float hk =
                static_cast<float>(std::clamp(style.hole, 0.0, 100.0) / 100.0);
            if (hk > 0.01f) {
                const float ix0 = cx - w / 2.0f * hk, ix1 = cx + w / 2.0f * hk;
                const float iy0 = cy - h / 2.0f * hk, iy1 = cy + h / 2.0f * hk;
                path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                             ix0, iy0, ix1, iy1),
                                         true});
                evenOdd = true;
            }
            break;
        }
        case ToolId::Cloud:
            path.subpaths.push_back(closedPath(
                pittore::vector::cloudAnchors(10, 0.8f, x0, y0, x1, y1)));
            break;
        case ToolId::CalloutRect: {
            const float r = 0.12f * std::min(w, h);
            path.subpaths.push_back(closedPath(
                pittore::vector::roundedRectAnchors(x0, y0, x1, y1, {r, r, r, r})));
            path.subpaths.push_back(closedPath({pt(x0 + 0.30f * w, y1 - 1.0f),
                                                pt(x0 + 0.45f * w, y1 - 1.0f),
                                                pt(x0 + 0.38f * w, y1 + 0.30f * h)}));
            break;
        }
        case ToolId::CalloutEllipse: {
            path.subpaths.push_back(
                {pittore::vector::ellipseAnchors(x0, y0, x1, y1), true});
            path.subpaths.push_back(closedPath({pt(x0 + 0.55f * w, y1 - 0.10f * h),
                                                pt(x0 + 0.70f * w, y1 - 0.05f * h),
                                                pt(x0 + 0.80f * w, y1 + 0.28f * h)}));
            break;
        }
        case ToolId::Tear: {
            auto arc = pittore::vector::arcAnchors(cx, cy, w / 2.0f, h / 2.0f,
                                                    2.618f, 6.806f);
            arc.push_back(pt(cx, y0));
            path.subpaths.push_back(closedPath(std::move(arc)));
            break;
        }
        case ToolId::Heart:
            path.subpaths.push_back(closedPath(
                pittore::vector::heartAnchors(x0, y0, x1, y1, 0.2f)));
            break;
        case ToolId::CustomShape: {
            // Gallery picker (customshape option): silhouettes without a
            // dedicated tool. 0 Lightning, 1 Droplet, 2 Moon, 3 Plus,
            // 4 Target, 5 Frame.
            const int which = std::clamp(style.customshape, 0, 5);
            if (which == 0) {
                path.subpaths.push_back(closedPath(
                    {pt(x0 + 0.55f * w, y0), pt(x0 + 0.20f * w, y0 + 0.55f * h),
                     pt(x0 + 0.44f * w, y0 + 0.55f * h),
                     pt(x0 + 0.38f * w, y1),
                     pt(x0 + 0.80f * w, y0 + 0.42f * h),
                     pt(x0 + 0.56f * w, y0 + 0.42f * h)}));
            } else if (which == 1) {
                // Droplet: tip triangle over a disc (nonzero merges them).
                path.subpaths.push_back(closedPath(
                    {pt(cx, y0), pt(x0 + 0.18f * w, y0 + 0.52f * h),
                     pt(x1 - 0.18f * w, y0 + 0.52f * h)}));
                path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                              cx - 0.32f * w, y0 + 0.30f * h,
                                              cx + 0.32f * w, y1),
                                          true});
            } else if (which == 2) {
                // Moon: punched disc.
                path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                              x0, y0, x1, y1),
                                          true});
                path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                              x0 + 0.28f * w, y0 - 0.08f * h,
                                              x1 + 0.28f * w, y1 - 0.08f * h),
                                          true});
                evenOdd = true;
            } else if (which == 3) {
                const float ax = x0 + 0.38f * w, bx = x0 + 0.62f * w;
                const float ay = y0 + 0.38f * h, by = y0 + 0.62f * h;
                path.subpaths.push_back(closedPath(
                    {pt(ax, y0), pt(bx, y0), pt(bx, ay), pt(x1, ay),
                     pt(x1, by), pt(bx, by), pt(bx, y1), pt(ax, y1),
                     pt(ax, by), pt(x0, by), pt(x0, ay), pt(ax, ay)}));
            } else if (which == 4) {
                // Target: concentric rings.
                const float r1 = 0.5f * std::min(w, h);
                for (int k = 0; k < 3; ++k) {
                    const float rk = r1 * (1.0f - 0.30f * k);
                    const float rkIn = rk * 0.72f;
                    path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                                  cx - rk, cy - rk, cx + rk,
                                                  cy + rk),
                                              true});
                    path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                                  cx - rkIn, cy - rkIn,
                                                  cx + rkIn, cy + rkIn),
                                              true});
                }
                evenOdd = true;
            } else {
                // Frame: outer rect minus inner rect.
                path.subpaths.push_back(closedPath(
                    {pt(x0, y0), pt(x1, y0), pt(x1, y1), pt(x0, y1)}));
                const float ix0 = x0 + 0.22f * w, ix1 = x1 - 0.22f * w;
                const float iy0 = y0 + 0.22f * h, iy1 = y1 - 0.22f * h;
                path.subpaths.push_back(closedPath(
                    {pt(ix0, iy0), pt(ix1, iy0), pt(ix1, iy1), pt(ix0, iy1)}));
                evenOdd = true;
            }
            break;
        }
        case ToolId::Spiral: {
            // Open polyline spiral: stroke-only.
            std::vector<Anchor> out;
            constexpr int kSteps = 72;
            constexpr float kTurns = 2.5f;
            for (int i = 0; i <= kSteps; ++i) {
                const float t = static_cast<float>(i) / kSteps;
                const float a = t * kTurns * 2.0f * kPi;
                const float r = (1.0f - 0.94f * t) / 2.0f;
                out.push_back(pt(cx + w * r * std::cos(a), cy + h * r * std::sin(a)));
            }
            path.subpaths.push_back(openPath(std::move(out)));
            forceStrokeOnly = true;
            break;
        }
        case ToolId::Cross: {
            const float ax = x0 + w / 3.0f, bx = x0 + 2.0f * w / 3.0f;
            const float ay = y0 + h / 3.0f, by = y0 + 2.0f * h / 3.0f;
            path.subpaths.push_back(closedPath(
                {pt(ax, y0), pt(bx, y0), pt(bx, ay), pt(x1, ay), pt(x1, by),
                 pt(bx, by), pt(bx, y1), pt(ax, y1), pt(ax, by), pt(x0, by),
                 pt(x0, ay), pt(ax, ay)}));
            break;
        }
        case ToolId::RightTriangle:
            path.subpaths.push_back(
                closedPath({pt(x0, y1), pt(x1, y1), pt(x0, y0)}));
            break;
        case ToolId::Parallelogram: {
            const float s = std::clamp(static_cast<float>(style.skew) / 100.0f,
                                       -0.45f, 0.45f) *
                            w;
            path.subpaths.push_back(closedPath({pt(x0 + s, y0), pt(x1 + s, y0),
                                                pt(x1 - s, y1), pt(x0 - s, y1)}));
            break;
        }
        case ToolId::Chevron: {
            const float th = 0.32f * h, bx = 0.28f * w, ym = (y0 + y1) / 2.0f;
            path.subpaths.push_back(closedPath(
                {pt(x0, y0), pt(x1, ym), pt(x0, y1), pt(x0, y1 - th),
                 pt(x1 - bx, ym), pt(x0, y0 + th)}));
            break;
        }
        case ToolId::CircularArrow: {
            const float R = 0.5f * std::min(w, h);
            const float r = R * 0.70f;
            const float t0 = 1.9f, sweep = 5.2f, t1 = t0 + sweep;
            auto band = pittore::vector::arcAnchors(cx, cy, R, R, t0, t1);
            auto inner =
                pittore::vector::arcAnchors(cx, cy, r, r, t1, t0);
            band.insert(band.end(), inner.begin(), inner.end());
            path.subpaths.push_back(closedPath(std::move(band)));
            // Head at the sweep end, pointing tangentially.
            const QPointF e = polar(cx, cy, R, t1);
            const QPointF tv(-std::sin(t1), std::cos(t1));
            const QPointF nv(-tv.y(), tv.x());
            const float L = 0.22f * R * 2.0f, Wd = 0.30f * R * 2.0f;
            path.subpaths.push_back(closedPath(
                {pt(static_cast<float>(e.x() + tv.x() * L),
                    static_cast<float>(e.y() + tv.y() * L)),
                 pt(static_cast<float>(e.x() - tv.x() * 0.1f * L + nv.x() * Wd / 2.0f),
                    static_cast<float>(e.y() - tv.y() * 0.1f * L + nv.y() * Wd / 2.0f)),
                 pt(static_cast<float>(e.x() - tv.x() * 0.1f * L - nv.x() * Wd / 2.0f),
                    static_cast<float>(e.y() - tv.y() * 0.1f * L - nv.y() * Wd / 2.0f))}));
            break;
        }
        case ToolId::Shield:
            path.subpaths.push_back(closedPath({pt(x0, y0), pt(x1, y0),
                                                pt(x1, y0 + 0.55f * h),
                                                pt(cx, y1),
                                                pt(x0, y0 + 0.55f * h)}));
            break;
        case ToolId::Ticket: {
            const float r = 0.10f * std::min(w, h);
            path.subpaths.push_back(closedPath(
                pittore::vector::roundedRectAnchors(x0, y0, x1, y1, {r, r, r, r})));
            const float nr = 0.12f * h;
            path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                         x0 - nr, cy - nr, x0 + nr, cy + nr),
                                     true});
            path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                         x1 - nr, cy - nr, x1 + nr, cy + nr),
                                     true});
            evenOdd = true;
            break;
        }
        case ToolId::Sun: {
            const float R = 0.5f * std::min(w, h);
            path.subpaths.push_back({pittore::vector::ellipseAnchors(
                                         cx - R * 0.34f, cy - R * 0.34f,
                                         cx + R * 0.34f, cy + R * 0.34f),
                                     true});
            const int n = std::clamp(style.sides, 3, 100);
            const float hw = kPi / static_cast<float>(n) * 0.35f;
            for (int i = 0; i < n; ++i) {
                const float a = -kPi / 2.0f + i * 2.0f * kPi / static_cast<float>(n);
                const QPointF b1 = polar(cx, cy, R * 0.36f, a - hw);
                const QPointF tip = polar(cx, cy, R, a);
                const QPointF b2 = polar(cx, cy, R * 0.36f, a + hw);
                path.subpaths.push_back(closedPath(
                    {pt(static_cast<float>(b1.x()), static_cast<float>(b1.y())),
                     pt(static_cast<float>(tip.x()), static_cast<float>(tip.y())),
                     pt(static_cast<float>(b2.x()), static_cast<float>(b2.y()))}));
            }
            break;
        }
        case ToolId::Cat: {
            path.subpaths.push_back(
                {pittore::vector::ellipseAnchors(x0 + 0.12f * w, y0 + 0.22f * h,
                                                 x1 - 0.12f * w, y1),
                 true});
            path.subpaths.push_back(closedPath({pt(x0 + 0.16f * w, y0 + 0.34f * h),
                                                pt(x0 + 0.20f * w, y0),
                                                pt(x0 + 0.40f * w, y0 + 0.26f * h)}));
            path.subpaths.push_back(closedPath({pt(x1 - 0.16f * w, y0 + 0.34f * h),
                                                pt(x1 - 0.20f * w, y0),
                                                pt(x1 - 0.40f * w, y0 + 0.26f * h)}));
            break;
        }
        case ToolId::Line:
            path.subpaths.push_back(openPath({pt(x0, y0), pt(x1, y1)}));
            forceStrokeOnly = true;
            break;
        case ToolId::QRCode: {
            // Module grid in its own frame (content/size/ECC from the bar);
            // bypasses the VectorShape path — modules are already segments.
            auto qr = makeQrArt(style.qrContent,
                                std::clamp(style.qrSize, 21, 1024),
                                std::clamp(style.qrEcc, 0, 3));
            if (!qr) return nullptr;
            qr->paint.cap = std::clamp(style.cap, 0, 2);
            qr->paint.join = std::clamp(style.join, 0, 2);
            return qr;
        }
        default:
            return nullptr;
    }

    if (path.subpaths.empty()) return nullptr;

    VectorShape shape;
    shape.path = std::move(path);
    shape.evenOdd = evenOdd;
    shape.fill = {static_cast<std::uint8_t>(style.fill.red()),
                  static_cast<std::uint8_t>(style.fill.green()),
                  static_cast<std::uint8_t>(style.fill.blue()),
                  static_cast<std::uint8_t>(style.fill.alpha())};
    shape.hasStroke = style.hasStroke;
    shape.stroke = {static_cast<std::uint8_t>(style.stroke.red()),
                    static_cast<std::uint8_t>(style.stroke.green()),
                    static_cast<std::uint8_t>(style.stroke.blue()),
                    static_cast<std::uint8_t>(style.stroke.alpha())};
    shape.strokeWidth = static_cast<float>(std::max(0.01, style.strokeWidth));

    auto node = pittore::vector::artNodeFromShape(shape, nullptr, 0.0, 0.0);
    if (!node) return nullptr;
    // artNodeFromShape always bakes round caps/joins; shapes honor the bar.
    node->paint.cap = std::clamp(style.cap, 0, 2);
    node->paint.join = std::clamp(style.join, 0, 2);
    if (forceStrokeOnly) {
        node->paint.hasFill = false;
        node->paint.hasGradient = false;
    }
    // Line arrowheads: filled triangles in the stroke colour, sized from the
    // Arrow W/L options (% of the stroke width). The shaft stays open.
    if (tool == ToolId::Line && (style.startArrow || style.endArrow) &&
        node->paint.hasStroke) {
        const QPointF p0(x0, y0), p1(x1, y1);
        const QPointF span = p1 - p0;
        const double len = std::hypot(span.x(), span.y());
        if (len > 1e-9) {
            const double wgt = std::max(0.01, style.strokeWidth);
            const double L =
                wgt * std::clamp(style.arrowLPct, 10.0, 5000.0) / 100.0;
            const double W =
                wgt * std::clamp(style.arrowWPct, 10.0, 1000.0) / 100.0 / 2.0;
            auto head = [&](const QPointF& tip, const QPointF& dir) {
                const QPointF n(-dir.y(), dir.x());
                const QPointF base = tip - dir * L;
                std::vector<std::pair<float, float>> tri = {
                    {static_cast<float>(tip.x()), static_cast<float>(tip.y())},
                    {static_cast<float>(base.x() + n.x() * W),
                     static_cast<float>(base.y() + n.y() * W)},
                    {static_cast<float>(base.x() - n.x() * W),
                     static_cast<float>(base.y() - n.y() * W)}};
                pittore::vector::Segment m, l1, l2, c;
                m.kind = pittore::vector::Segment::Kind::MoveTo;
                m.x = tri[0].first;
                m.y = tri[0].second;
                l1.kind = l2.kind =
                    pittore::vector::Segment::Kind::LineTo;
                l1.x = tri[1].first;
                l1.y = tri[1].second;
                l2.x = tri[2].first;
                l2.y = tri[2].second;
                c.kind = pittore::vector::Segment::Kind::Close;
                node->segments.push_back(m);
                node->segments.push_back(l1);
                node->segments.push_back(l2);
                node->segments.push_back(c);
            };
            const QPointF dir(span.x() / len, span.y() / len);
            if (style.endArrow) head(p1, dir);
            if (style.startArrow) head(p0, QPointF(-dir.x(), -dir.y()));
            // Heads fill with the stroke colour (the shaft is stroke-only).
            node->paint.hasFill = true;
            std::copy(std::begin(node->paint.stroke),
                      std::end(node->paint.stroke),
                      std::begin(node->paint.fill));
        }
    }
    return node;
}

bool AppState::addVectorShapeLayer(ToolId tool, const QRectF& docRect,
                                   const QString& undoName) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    // Pixels/Path shape output still needs its backend: refuse loudly
    // instead of making the wrong thing. Boolean path ops (below) fold the
    // new shape with the selected art in one undo step; New Layer commits
    // solo.
    if (option(tool, QStringLiteral("shapemode")).toInt() != 0) {
        setStatusHint(tr("Path/Pixels shape output — planned; Shape mode works."));
        return false;
    }
    const int pathop = option(tool, QStringLiteral("pathop")).toInt();

    const ShapeStyle st = shapeStyleFor(this, tool);
    if (!st.hasFill && !st.hasStroke) {
        setStatusHint(tr("The shape would be invisible: give it a fill or stroke."));
        return false;
    }

    QRectF rect = docRect.normalized();
    if (tool == ToolId::QRCode) {
        // Module geometry comes from the QR options; the drag only anchors
        // the top-left (a click places at option size).
        const double qs = std::clamp(st.qrSize, 21, 1024);
        if (rect.width() <= 0.0 || rect.height() <= 0.0)
            rect.setSize(QSizeF(qs, qs));
    }
    if (option(tool, QStringLiteral("aligned_edges")).toBool()) {
        rect = QRectF(qRound(rect.x()), qRound(rect.y()), qRound(rect.width()),
                      qRound(rect.height()));
    }
    if (rect.width() <= 0.0 || rect.height() <= 0.0) return false;

    // Local frame: anchors live in (0,0,w,h). The trim pre-pass maps them to
    // image-relative pixels (matrix = -trim origin), so the layer offset
    // anchors the trimmed image exactly like an import — export, bounds and
    // the Node tool all compose from the same frame.
    auto node = makeShapeArt(tool, QRectF(0, 0, rect.width(), rect.height()), st);
    if (!node) {
#ifdef HAVE_QRENCODE
        setStatusHint(tool == ToolId::QRCode
                          ? tr("QR Code: enter content to encode.")
                          : tr("Could not build the shape."));
#else
        setStatusHint(tool == ToolId::QRCode
                          ? tr("QR Code needs the qrencode library.")
                          : tr("Could not build the shape."));
#endif
        return false;
    }
    if (tool == ToolId::QRCode)
        rect = QRectF(rect.topLeft(), artNodePath(*node).boundingRect().size());
    if (pathop != 0) {
        // Fold with the selected art: the node is still in its local frame
        // (identity matrix), so a temp layer at the rect origin maps it.
        LayerItem frame;
        frame.offset = rect.topLeft();
        frame.scaleX = frame.scaleY = 1.0;
        const std::vector<pittore::vector::BoolRing> extra =
            artToRings(*node, frame);
        if (extra.empty()) {
            setStatusHint(tr("The shape has no area to combine."));
            return false;
        }
        QVector<int> targets;
        for (int i : selectedLayerIndices()) {
            if (i >= 0 && i < d->layers.size() && d->layers[i].art &&
                !d->layers[i].art->isEmpty())
                targets.push_back(i);
        }
        if (targets.isEmpty())
            return commitArtNodeLayer(std::move(node), rect, tool, undoName,
                                      false);
        const pittore::vector::BoolOp op =
            pathop == 2   ? pittore::vector::BoolOp::Difference
            : pathop == 3 ? pittore::vector::BoolOp::Intersection
            : pathop == 4 ? pittore::vector::BoolOp::Xor
                          : pittore::vector::BoolOp::Union;
        const QString name = pathop == 2   ? tr("Subtract")
                             : pathop == 3 ? tr("Intersect")
                             : pathop == 4 ? tr("Exclude")
                                           : tr("Combine");
        return booleanFoldLayers(targets, op, &extra, name);
    }
    return commitArtNodeLayer(std::move(node), rect, tool, undoName, false);
}

// Shared creation tail: frame `node` (local coords) with a trim margin,
// rasterize, bake and add it as a retained-geometry layer, one undo step.
// `rect` is the node's local frame origin in document space. `stayInTool`
// keeps the creation tool active (Pen keeps drawing); otherwise the
// keep_selected option decides between Move and staying.
bool AppState::commitArtNodeLayer(std::shared_ptr<pittore::vector::ArtNode> node,
                                   const QRectF& rect, ToolId tool,
                                   const QString& undoName, bool stayInTool,
                                   const QString& blendMode) {
    DocumentItem* d = activeDocument();
    if (!d || !node || node->isEmpty()) return false;
    // Creation-time stroke style (the bar's Solid/Dashed/Dotted preset).
    if (node->paint.hasStroke && !node->paint.hasDash) {
        const int ss = option(tool, QStringLiteral("strokestyle")).toInt();
        if (ss == 1) {
            node->paint.hasDash = true;
            node->paint.dash = {4.0f, 2.0f};
        } else if (ss == 2) {
            node->paint.hasDash = true;
            node->paint.dash = {0.0f, 2.0f};
            node->paint.cap = 1;  // dots need round caps
        }
    }
    const double margin = node->paint.hasStroke
                              ? maxProfileWidth(node->paint) / 2.0 + 1.0
                              : 1.0;
    const QRect trim =
        artNodePath(*node).boundingRect().adjusted(-margin, -margin, margin,
                                                   margin).toAlignedRect();
    node->matrix[4] = -trim.x();
    node->matrix[5] = -trim.y();
    QImage img;
    QPointF trimCheck;
    if (!rasterizeArtNode(*node, &img, &trimCheck)) {
        setStatusHint(tr("Could not render the shape."));
        return false;
    }
    auto pixels = straightRgba64ToImage(img);
    if (!pixels) {
        setStatusHint(tr("Could not render the shape."));
        return false;
    }

    d->beginUndoAction();
    LayerItem layer;
    layer.name = toolName(tool);
    layer.kind = LayerItem::Kind::Pixel;
    layer.blendMode = blendMode.isEmpty() ? QStringLiteral("Normal") : blendMode;
    layer.pixels = std::move(pixels);
    layer.offset = rect.topLeft() + QPointF(trim.x(), trim.y());
    layer.scaleX = 1.0;
    layer.scaleY = 1.0;
    layer.art = std::move(node);
    ++layer.sourceStamp;
    bakeArtDense(layer, d->zoom);
    addLayer(std::move(layer));  // inserts, selects, recomposites, signals
    d->commitUndoAction(undoName.isEmpty() ? toolName(tool) : undoName,
                        QString::fromUtf8(toolDef(tool).iconKey));
    emit historyChanged();
    if (!stayInTool && !option(tool, QStringLiteral("keep_selected")).toBool())
        setActiveTool(ToolId::Move);
    else
        setStatusHint(tr("%1 created.").arg(toolName(tool)));
    return true;
}

bool AppState::addVectorPathLayer(
    const std::vector<pittore::vector::Segment>& docSegments, ToolId tool,
    const QString& undoName) {
    using Kind = pittore::vector::Segment::Kind;
    DocumentItem* d = activeDocument();
    if (!d) return false;
    // Creation-time path ops fold with the selection (Union/Subtract/
    // Intersect/Exclude); New Layer commits solo. Saved for after the node
    // is built below.
    const int pathop = option(tool, QStringLiteral("pathop")).toInt();
    // Frame the path: bounds over endpoints and handle tips, segments
    // shifted into a local frame like every other creation flow. Explicit
    // min/max: QRectF point-union ignores null rects, which would make the
    // node origin accidental instead of the true minimum corner.
    int anchors = 0;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    bool closed = false;
    auto eat = [&](double x, double y) {
        if (anchors == 0) {
            x0 = x1 = x;
            y0 = y1 = y;
        } else {
            x0 = qMin(x0, x);
            y0 = qMin(y0, y);
            x1 = qMax(x1, x);
            y1 = qMax(y1, y);
        }
        ++anchors;
    };
    for (const auto& s : docSegments) {
        if (s.kind == Kind::MoveTo || s.kind == Kind::LineTo ||
            s.kind == Kind::CubicTo) {
            eat(s.x, s.y);
        }
        if (s.kind == Kind::CubicTo) {
            eat(s.c1x, s.c1y);
            eat(s.c2x, s.c2y);
        }
        if (s.kind == Kind::Close) closed = true;
    }
    if (anchors < 2) {
        setStatusHint(tr("A path needs at least two points."));
        return false;
    }
    const QRectF box(QPointF(x0, y0), QPointF(x1, y1));

    const ShapeStyle st = shapeStyleFor(this, tool);
    auto node = std::make_shared<pittore::vector::ArtNode>();
    node->name = toolName(tool).toStdString();
    node->segments.reserve(docSegments.size());
    for (auto s : docSegments) {
        if (s.kind == Kind::MoveTo || s.kind == Kind::LineTo ||
            s.kind == Kind::CubicTo) {
            s.x = static_cast<float>(s.x - box.x());
            s.y = static_cast<float>(s.y - box.y());
        }
        if (s.kind == Kind::CubicTo) {
            s.c1x = static_cast<float>(s.c1x - box.x());
            s.c1y = static_cast<float>(s.c1y - box.y());
            s.c2x = static_cast<float>(s.c2x - box.x());
            s.c2y = static_cast<float>(s.c2y - box.y());
        }
        node->segments.push_back(s);
    }
    node->paint.hasFill = st.hasFill;
    node->paint.fill[0] = static_cast<std::uint8_t>(st.fill.red());
    node->paint.fill[1] = static_cast<std::uint8_t>(st.fill.green());
    node->paint.fill[2] = static_cast<std::uint8_t>(st.fill.blue());
    node->paint.fill[3] = static_cast<std::uint8_t>(st.fill.alpha());
    node->paint.hasStroke = st.hasStroke;
    node->paint.stroke[0] = static_cast<std::uint8_t>(st.stroke.red());
    node->paint.stroke[1] = static_cast<std::uint8_t>(st.stroke.green());
    node->paint.stroke[2] = static_cast<std::uint8_t>(st.stroke.blue());
    node->paint.stroke[3] = static_cast<std::uint8_t>(st.stroke.alpha());
    node->paint.strokeWidth = std::max(0.01, st.strokeWidth);
    node->paint.cap = std::clamp(st.cap, 0, 2);
    node->paint.join = std::clamp(st.join, 0, 2);
    if (!closed) {
        // Open paths draw stroke-only (like Line); the stroke falls back to
        // the foreground so a fresh path is never invisible.
        node->paint.hasFill = false;
        node->paint.hasGradient = false;
        if (!node->paint.hasStroke) {
            const QColor fg = foreground();
            node->paint.hasStroke = true;
            node->paint.stroke[0] = static_cast<std::uint8_t>(fg.red());
            node->paint.stroke[1] = static_cast<std::uint8_t>(fg.green());
            node->paint.stroke[2] = static_cast<std::uint8_t>(fg.blue());
            node->paint.stroke[3] = static_cast<std::uint8_t>(fg.alpha());
            node->paint.strokeWidth = std::max(node->paint.strokeWidth, 1.0);
        }
    }
    if (!node->paint.hasFill && !node->paint.hasStroke) {
        setStatusHint(tr("The path would be invisible: give it a fill or stroke."));
        return false;
    }
    // Freehand carries its width in its own option.
    if (tool == ToolId::FreeformPen && node->paint.hasStroke) {
        const double w = option(tool, QStringLiteral("width")).toDouble();
        if (w > 0.0) node->paint.strokeWidth = w;
    }
    if (pathop != 0) {
        LayerItem frame;
        frame.offset = box.topLeft();
        frame.scaleX = frame.scaleY = 1.0;
        const std::vector<pittore::vector::BoolRing> extra =
            artToRings(*node, frame);
        if (extra.empty()) {
            setStatusHint(tr("The path has no area to combine."));
            return false;
        }
        QVector<int> targets;
        for (int i : selectedLayerIndices()) {
            if (i >= 0 && i < d->layers.size() && d->layers[i].art &&
                !d->layers[i].art->isEmpty())
                targets.push_back(i);
        }
        if (targets.isEmpty())
            return commitArtNodeLayer(std::move(node), box, tool, undoName,
                                      true);
        const pittore::vector::BoolOp op =
            pathop == 2   ? pittore::vector::BoolOp::Difference
            : pathop == 3 ? pittore::vector::BoolOp::Intersection
            : pathop == 4 ? pittore::vector::BoolOp::Xor
                          : pittore::vector::BoolOp::Union;
        const QString name = pathop == 2   ? tr("Subtract")
                             : pathop == 3 ? tr("Intersect")
                             : pathop == 4 ? tr("Exclude")
                                           : tr("Combine");
        return booleanFoldLayers(targets, op, &extra, name);
    }
    return commitArtNodeLayer(std::move(node), box, tool, undoName, true);
}

bool AppState::addVectorBrushLayer(
    const std::vector<pittore::vector::Segment>& ribbon, const QColor& color,
    double opacity, const QString& blendMode, const QString& undoName) {
    using Kind = pittore::vector::Segment::Kind;
    DocumentItem* d = activeDocument();
    if (!d || !color.isValid() || color.alpha() <= 0) return false;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    bool any = false;
    for (const auto& s : ribbon) {
        if (s.kind == Kind::MoveTo || s.kind == Kind::LineTo ||
            s.kind == Kind::CubicTo) {
            if (!any) {
                x0 = x1 = s.x;
                y0 = y1 = s.y;
                any = true;
            } else {
                x0 = qMin(x0, (double)s.x);
                y0 = qMin(y0, (double)s.y);
                x1 = qMax(x1, (double)s.x);
                y1 = qMax(y1, (double)s.y);
            }
        }
        if (s.kind == Kind::CubicTo) {
            x0 = qMin(x0, (double)s.c1x);
            y0 = qMin(y0, (double)s.c1y);
            x1 = qMax(x1, (double)s.c1x);
            y1 = qMax(y1, (double)s.c1y);
            x0 = qMin(x0, (double)s.c2x);
            y0 = qMin(y0, (double)s.c2y);
            x1 = qMax(x1, (double)s.c2x);
            y1 = qMax(y1, (double)s.c2y);
        }
    }
    if (!any) {
        setStatusHint(tr("The brush stroke is empty."));
        return false;
    }
    auto node = std::make_shared<pittore::vector::ArtNode>();
    node->name = toolName(ToolId::VectorBrushTool).toStdString();
    node->segments.reserve(ribbon.size());
    for (auto s : ribbon) {
        if (s.kind == Kind::MoveTo || s.kind == Kind::LineTo ||
            s.kind == Kind::CubicTo) {
            s.x = static_cast<float>(s.x - x0);
            s.y = static_cast<float>(s.y - y0);
        }
        if (s.kind == Kind::CubicTo) {
            s.c1x = static_cast<float>(s.c1x - x0);
            s.c1y = static_cast<float>(s.c1y - y0);
            s.c2x = static_cast<float>(s.c2x - x0);
            s.c2y = static_cast<float>(s.c2y - y0);
        }
        node->segments.push_back(s);
    }
    node->paint.hasFill = true;
    node->paint.fill[0] = static_cast<std::uint8_t>(color.red());
    node->paint.fill[1] = static_cast<std::uint8_t>(color.green());
    node->paint.fill[2] = static_cast<std::uint8_t>(color.blue());
    node->paint.fill[3] = static_cast<std::uint8_t>(color.alpha());
    node->opacity = qBound(0.01, opacity, 1.0);
    // Brush ribbons self-overlap on loops and reversals: winding fill
    // keeps every pass solid instead of slitting (even-odd would knock
    // the overlap out).
    node->evenOdd = false;
    return commitArtNodeLayer(std::move(node), QRectF(QPointF(x0, y0), QPointF(x1, y1)),
                              ToolId::VectorBrushTool, undoName, true, blendMode);
}

}  // namespace pittore::ui
