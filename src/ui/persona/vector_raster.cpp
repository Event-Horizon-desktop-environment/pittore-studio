#include "ui/persona/vector_raster.h"
#include "ui/persona/vector_profile.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTransform>

#include <algorithm>
#include <cmath>

#include "engine/vector/vector_art.h"

namespace pittore::ui {
namespace {

QTransform nodeTransform(const pittore::vector::ArtNode& node) {
    // matrix is (a b c d e f) in QTransform order: node → source pixels.
    return QTransform(node.matrix[0], node.matrix[1], node.matrix[2],
                      node.matrix[3], node.matrix[4], node.matrix[5]);
}

}  // namespace

QPainterPath artNodePath(const pittore::vector::ArtNode& node) {
    using Seg = pittore::vector::Segment;
    QPainterPath path;
    for (const Seg& s : node.segments) {
        switch (s.kind) {
            case Seg::Kind::MoveTo:
                path.moveTo(s.x, s.y);
                break;
            case Seg::Kind::LineTo:
                path.lineTo(s.x, s.y);
                break;
            case Seg::Kind::CubicTo:
                path.cubicTo(s.c1x, s.c1y, s.c2x, s.c2y, s.x, s.y);
                break;
            case Seg::Kind::Close:
                path.closeSubpath();
                break;
        }
    }
    if (node.evenOdd)
        path.setFillRule(Qt::OddEvenFill);
    else
        path.setFillRule(Qt::WindingFill);  // never rely on the Qt
                                            // default (OddEven): brush
                                            // ribbons self-overlap and
                                            // would slit under odd-even.
    return path;
}

namespace {

QColor rgbaColor(const std::uint8_t rgba[4]) {
    return QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
}

// One gradient as a brush in node space (the painter transform carries it to
// layer/view space unchanged, like the segments).
QBrush gradientBrushOf(const pittore::vector::ArtGradient& g) {
    QGradient* grad = nullptr;
    QLinearGradient linear;
    QRadialGradient radial;
    if (g.radial) {
        radial = QRadialGradient(QPointF(g.cx, g.cy), std::max(0.01, g.r));
        grad = &radial;
    } else {
        linear = QLinearGradient(QPointF(g.x1, g.y1), QPointF(g.x2, g.y2));
        grad = &linear;
    }
    for (const pittore::vector::ArtStop& s : g.stops)
        grad->setColorAt(std::clamp(s.pos, 0.0f, 1.0f), rgbaColor(s.rgba));
    // grad points at a local; build the brush before returning.
    if (g.radial) return QBrush(radial);
    return QBrush(linear);
}

}  // namespace

QBrush artFillBrush(const pittore::vector::ArtNode& node) {
    const pittore::vector::ArtPaint& paint = node.paint;
    if (!paint.hasFill) return Qt::NoBrush;
    if (paint.hasGradient && !paint.gradient.stops.empty())
        return gradientBrushOf(paint.gradient);
    return QBrush(rgbaColor(paint.fill));
}

QBrush artStrokeBrush(const pittore::vector::ArtNode& node) {
    const pittore::vector::ArtPaint& paint = node.paint;
    if (!paint.hasStroke) return Qt::NoBrush;
    if (paint.hasStrokeGradient && !paint.strokeGradient.stops.empty())
        return gradientBrushOf(paint.strokeGradient);
    return QBrush(rgbaColor(paint.stroke));
}

Qt::PenCapStyle artCapStyle(int cap) {
    // ArtPaint: 0 butt, 1 round, 2 square.
    switch (cap) {
        case 1:
            return Qt::RoundCap;
        case 2:
            return Qt::SquareCap;
        default:
            return Qt::FlatCap;
    }
}

Qt::PenJoinStyle artJoinStyle(int join) {
    // ArtPaint: 0 miter, 1 round, 2 bevel.
    switch (join) {
        case 1:
            return Qt::RoundJoin;
        case 2:
            return Qt::BevelJoin;
        default:
            return Qt::MiterJoin;
    }
}

void applyDashToPen(QPen& pen, const pittore::vector::ArtPaint& paint) {
    if (!paint.hasDash || paint.dash.empty()) return;
    QList<qreal> pattern;
    pattern.reserve(static_cast<int>(paint.dash.size()));
    for (float v : paint.dash) pattern.push_back(std::max(0.0, (double)v));
    // An all-zero pattern would draw nothing; fall back to solid instead of
    // a vanishing stroke.
    bool any = false;
    for (qreal v : pattern)
        if (v > 0.0) {
            any = true;
            break;
        }
    if (!any) return;
    pen.setDashPattern(pattern);
    pen.setDashOffset(paint.dashOffset);
}

bool rasterizeArtNode(const pittore::vector::ArtNode& node, QImage* img,
                      QPointF* sourceOrigin) {
    if (img) *img = QImage();
    if (sourceOrigin) *sourceOrigin = QPointF();
    if (node.isEmpty() || (!node.paint.hasFill && !node.paint.hasStroke)) return false;

    const QTransform tf = nodeTransform(node);
    const QPainterPath path = artNodePath(node);
    if (path.isEmpty()) return false;

    const double sx = std::hypot(tf.m11(), tf.m12());
    const double sy = std::hypot(tf.m21(), tf.m22());
    const double sc = std::max(sx, std::max(sy, 1e-9));
    // Mirror the importer's trim: stroked bounds grown by half the (scaled)
    // maximum stroke width (profile peak included) plus a 1px margin.
    const double margin = node.paint.hasStroke
                              ? maxProfileWidth(node.paint) * sc / 2.0 + 1.0
                              : 1.0;
    const QRectF grown = tf.mapRect(path.boundingRect())
                             .adjusted(-margin, -margin, margin, margin);
    const QRect ir = grown.toAlignedRect();
    if (ir.width() <= 0 || ir.height() <= 0 || ir.width() > 16384 ||
        ir.height() > 16384)
        return false;

    QImage out(ir.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-ir.x(), -ir.y());
    p.setOpacity(std::clamp(node.opacity, 0.0, 1.0));
    p.setTransform(tf, true);
    p.setBrush(artFillBrush(node));
    if (node.paint.hasStroke) {
        // Variable-width strokes expand to filled outlines (no QPen
        // equivalent); uniform strokes keep the fast pen path.
        const QPainterPath expanded = expandStrokePath(node);
        if (!expanded.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(artStrokeBrush(node));
            p.drawPath(expanded);
            p.setBrush(artFillBrush(node));
        } else {
            QPen pen(artStrokeBrush(node),
                     std::max(0.01, node.paint.strokeWidth));
            pen.setCosmetic(false);
            pen.setCapStyle(artCapStyle(node.paint.cap));
            pen.setJoinStyle(artJoinStyle(node.paint.join));
            applyDashToPen(pen, node.paint);
            p.setPen(pen);
        }
    } else {
        p.setPen(Qt::NoPen);
    }
    p.drawPath(path);
    p.end();

    if (img) *img = std::move(out);
    if (sourceOrigin) *sourceOrigin = QPointF(ir.x(), ir.y());
    return true;
}

}  // namespace pittore::ui
