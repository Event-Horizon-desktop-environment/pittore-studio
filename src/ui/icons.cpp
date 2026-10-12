#include "ui/icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QFile>
#include <QLinearGradient>
#include <QResource>
#include <QSvgRenderer>
#include <QtMath>

namespace pittore::ui {
namespace {

// All glyphs use this grid, scaled at paint time.
constexpr qreal kGrid = 24.0;

QPolygonF poly(std::initializer_list<QPointF> pts) { return QPolygonF(pts); }

QPolygonF regularPolygon(QPointF c, qreal r, int sides, qreal rot = -M_PI / 2) {
    QPolygonF p;
    for (int i = 0; i < sides; ++i) {
        const qreal a = rot + i * 2 * M_PI / sides;
        p << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return p;
}

QPolygonF starPolygon(QPointF c, qreal outer, qreal inner, int points) {
    QPolygonF p;
    for (int i = 0; i < points * 2; ++i) {
        const qreal r = (i % 2 == 0) ? outer : inner;
        const qreal a = -M_PI / 2 + i * M_PI / points;
        p << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return p;
}

// Slanted pen/brush body shared by a few glyphs.
void strokeBody(QPainter& p, QPointF tip, QPointF tail, qreal halfWidth) {
    QPointF d = tail - tip;
    const qreal len = std::hypot(d.x(), d.y());
    if (len < 0.001) return;
    const QPointF n(-d.y() / len * halfWidth, d.x() / len * halfWidth);
    p.drawPolygon(poly({tip, tail + n, tail - n}));
}

void dashedRect(QPainter& p, const QRectF& r) {
    QPen pen = p.pen();
    pen.setStyle(Qt::CustomDashLine);
    pen.setDashPattern({2.2, 1.8});
    p.setPen(pen);
    p.drawRect(r);
    pen.setStyle(Qt::SolidLine);
    p.setPen(pen);
}

void dashedEllipse(QPainter& p, const QRectF& r) {
    QPen pen = p.pen();
    pen.setStyle(Qt::CustomDashLine);
    pen.setDashPattern({2.2, 1.8});
    p.setPen(pen);
    p.drawEllipse(r);
    pen.setStyle(Qt::SolidLine);
    p.setPen(pen);
}

// 4-point AI sparkle, generative-fill style.
QPainterPath sparklePath(QPointF ctr, qreal r) {
    QPainterPath sp(QPointF(ctr.x(), ctr.y() - r));
    sp.quadTo(ctr.x() + r * 0.18, ctr.y() - r * 0.18, ctr.x() + r, ctr.y());
    sp.quadTo(ctr.x() + r * 0.18, ctr.y() + r * 0.18, ctr.x(), ctr.y() + r);
    sp.quadTo(ctr.x() - r * 0.18, ctr.y() + r * 0.18, ctr.x() - r, ctr.y());
    sp.quadTo(ctr.x() - r * 0.18, ctr.y() - r * 0.18, ctr.x(), ctr.y() - r);
    return sp;
}

void squareNode(QPainter& p, QPointF ctr, qreal s = 2.4) {
    p.drawRect(QRectF(ctr.x() - s / 2, ctr.y() - s / 2, s, s));
}

void smallPlus(QPainter& p, QPointF ctr, qreal r = 2.2) {
    p.drawLine(QPointF(ctr.x() - r, ctr.y()), QPointF(ctr.x() + r, ctr.y()));
    p.drawLine(QPointF(ctr.x(), ctr.y() - r), QPointF(ctr.x(), ctr.y() + r));
}

// Classic rubber-stamp side view, shared by clone / pattern.
void stampBase(QPainter& p, const QBrush& fill) {
    p.setBrush(fill);
    p.drawEllipse(QPointF(12, 5.2), 3.1, 2.4);
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(12, 7.4), QPointF(12, 11.5));
    p.drawLine(QPointF(9.5, 11.5), QPointF(14.5, 11.5));
    p.drawPolygon(poly({{8.5, 11.5}, {15.5, 11.5}, {17, 16.5}, {7, 16.5}}));
    p.drawRect(QRectF(6, 16.5, 12, 3.2));
}

// Tilted eraser with bevel split, shared by eraser family.
void eraserBody(QPainter& p, QPointF ctr, qreal w, qreal h, qreal angle) {
    p.save();
    p.translate(ctr);
    p.rotate(angle);
    p.drawRoundedRect(QRectF(-w / 2, -h / 2, w, h), 1.6, 1.6);
    p.drawLine(QPointF(-w / 2 + 5.5, -h / 2), QPointF(-w / 2 + 5.5, h / 2));
    p.restore();
}

void drawGlyphProcedural(QPainter& p, const QString& k, const QColor& c) {
    QPen pen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QBrush fill(c);

    // --- Selection (researched, redrawn from scratch) ---
    if (k == "move") {
        // Four-way move arrows, move tool.
        p.setBrush(fill);
        p.drawLine(QPointF(12, 5.5), QPointF(12, 18.5));
        p.drawLine(QPointF(5.5, 12), QPointF(18.5, 12));
        p.drawPolygon(poly({{12, 2.5}, {9.4, 6.2}, {14.6, 6.2}}));
        p.drawPolygon(poly({{12, 21.5}, {9.4, 17.8}, {14.6, 17.8}}));
        p.drawPolygon(poly({{2.5, 12}, {6.2, 9.4}, {6.2, 14.6}}));
        p.drawPolygon(poly({{21.5, 12}, {17.8, 9.4}, {17.8, 14.6}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "artboard") {
        // Artboard: document rect + plus badge, artboard tool.
        p.drawRect(QRectF(4.5, 8, 15, 11.5));
        p.drawLine(QPointF(4.5, 11.5), QPointF(19.5, 11.5));
        p.setBrush(fill);
        p.drawEllipse(QPointF(7.2, 5.2), 2.6, 2.6);
        p.setBrush(Qt::NoBrush);
        QPen white(Qt::white, 1.1, Qt::SolidLine, Qt::RoundCap);
        // Plus cut into badge using background-independent cross in accent color:
        // draw badge ring then plus in tool color on top is enough at 20px.
        p.setPen(QPen(c, 1.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(7.2, 4.1), QPointF(7.2, 6.3));
        p.drawLine(QPointF(6.1, 5.2), QPointF(8.3, 5.2));
        p.setPen(pen);
    } else if (k == "marquee-rect") {
        dashedRect(p, QRectF(4, 6, 16, 12));
        p.setBrush(fill);
        squareNode(p, QPointF(4, 6));
        squareNode(p, QPointF(20, 6));
        squareNode(p, QPointF(4, 18));
        squareNode(p, QPointF(20, 18));
        p.setBrush(Qt::NoBrush);
    } else if (k == "marquee-ell") {
        dashedEllipse(p, QRectF(3.5, 5.5, 17, 13));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 5.5), 1.1, 1.1);
        p.drawEllipse(QPointF(12, 18.5), 1.1, 1.1);
        p.drawEllipse(QPointF(3.5, 12), 1.1, 1.1);
        p.drawEllipse(QPointF(20.5, 12), 1.1, 1.1);
        p.setBrush(Qt::NoBrush);
    } else if (k == "marquee-row") {
        // Single-pixel row: thin dashed strip with edge ticks.
        dashedRect(p, QRectF(3, 11, 18, 2.2));
        p.drawLine(QPointF(3, 8.5), QPointF(3, 10));
        p.drawLine(QPointF(21, 8.5), QPointF(21, 10));
        p.drawLine(QPointF(3, 14.2), QPointF(3, 15.5));
        p.drawLine(QPointF(21, 14.2), QPointF(21, 15.5));
    } else if (k == "marquee-col") {
        dashedRect(p, QRectF(11, 3, 2.2, 18));
        p.drawLine(QPointF(8.5, 3), QPointF(10, 3));
        p.drawLine(QPointF(14.2, 3), QPointF(15.5, 3));
        p.drawLine(QPointF(8.5, 21), QPointF(10, 21));
        p.drawLine(QPointF(14.2, 21), QPointF(15.5, 21));
    } else if (k == "lasso") {
        // Freehand lasso loop with trailing rope and knot.
        QPainterPath loop(QPointF(7.5, 18.5));
        loop.cubicTo(2.5, 14, 4.5, 6.5, 10.5, 4.8);
        loop.cubicTo(16.5, 3.2, 21, 8, 18.2, 13.2);
        loop.cubicTo(16, 17, 10.5, 17.5, 8.2, 14.5);
        p.drawPath(loop);
        p.drawLine(QPointF(8.2, 14.5), QPointF(7.5, 18.5));
        p.setBrush(fill);
        p.drawEllipse(QPointF(7.3, 19.8), 1.7, 1.7);
        p.setBrush(Qt::NoBrush);
    } else if (k == "lasso-poly") {
        // Straight segments with square anchors, open at cursor.
        p.drawPolyline(poly({{4.5, 17}, {6.5, 6.5}, {14.5, 4.5}, {20, 10.5}, {14, 14.5}}));
        p.drawLine(QPointF(14, 14.5), QPointF(12.5, 19.5));
        p.setBrush(fill);
        squareNode(p, QPointF(4.5, 17));
        squareNode(p, QPointF(6.5, 6.5));
        squareNode(p, QPointF(14.5, 4.5));
        squareNode(p, QPointF(20, 10.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12.5, 19.5), 1.2, 1.2);
    } else if (k == "lasso-mag") {
        // Magnetic lasso: edge-hugging curve + horseshoe magnet.
        QPainterPath edge(QPointF(4.5, 18));
        edge.cubicTo(3, 10, 8, 3.5, 14, 5);
        edge.cubicTo(19, 6.2, 20, 12, 15.5, 15);
        p.drawPath(edge);
        p.setBrush(fill);
        p.drawEllipse(QPointF(4.5, 18), 1.2, 1.2);
        p.drawEllipse(QPointF(9, 9.5), 1.0, 1.0);
        p.drawEllipse(QPointF(14, 6.2), 1.0, 1.0);
        p.setBrush(Qt::NoBrush);
        // U magnet at the cursor end.
        QPainterPath mag(QPointF(15.5, 15.5));
        mag.lineTo(15.5, 19);
        mag.arcTo(QRectF(15.5, 17.5, 5, 3.5), 180, 180);
        mag.lineTo(QPointF(20.5, 15.5));
        p.drawPath(mag);
        p.setBrush(fill);
        p.drawRect(QRectF(14.4, 14.2, 2.2, 1.8));
        p.drawRect(QRectF(19.4, 14.2, 2.2, 1.8));
        p.setBrush(Qt::NoBrush);
    } else if (k == "sel-brush") {
        // Selection Brush: paintbrush over marching-ants circle.
        p.setBrush(fill);
        strokeBody(p, QPointF(3.5, 20.5), QPointF(11.5, 11), 2.5);
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(9.5, 12.5);
        p.rotate(45);
        p.drawRect(QRectF(-2, -1.2, 4, 2.4));
        p.restore();
        dashedEllipse(p, QRectF(12.5, 11.5, 8.5, 8.5));
    } else if (k == "quick-sel") {
        // Quick Selection: brush tip growing a dotted mask + plus.
        p.setBrush(fill);
        strokeBody(p, QPointF(4, 20), QPointF(11.5, 11), 2.6);
        p.setBrush(Qt::NoBrush);
        dashedEllipse(p, QRectF(11, 10.5, 10, 9.5));
        p.setBrush(fill);
        p.drawEllipse(QPointF(16, 15.2), 1.9, 1.9);
        p.setBrush(Qt::NoBrush);
        smallPlus(p, QPointF(19.5, 5.5), 2.0);
    } else if (k == "object-sel") {
        // Object Selection: finder rectangle + auto lasso + cursor arrow.
        dashedRect(p, QRectF(3.5, 5, 17, 12));
        QPainterPath finder(QPointF(7, 13.5));
        finder.cubicTo(7, 9, 11, 7.5, 14, 9);
        finder.cubicTo(17, 10.5, 16.5, 14.5, 12.5, 15);
        finder.cubicTo(9.5, 15.3, 7.5, 14.8, 7, 13.5);
        p.drawPath(finder);
        p.setBrush(fill);
        p.drawPolygon(poly({{15, 15.5}, {15, 21.5}, {16.8, 19.8}, {18.4, 21.6}, {19.4, 20.8}, {17.8, 19}, {19.6, 18.9}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "wand") {
        // Magic Wand: diagonal wand + three twinkles.
        p.setBrush(fill);
        strokeBody(p, QPointF(3.5, 20.5), QPointF(12.5, 10.5), 2.0);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(11, 12), QPointF(14, 9));
        smallPlus(p, QPointF(17.5, 6), 2.4);
        smallPlus(p, QPointF(20.5, 11.5), 1.7);
        p.setBrush(fill);
        p.drawPath(sparklePath(QPointF(15, 11.5), 1.6));
        p.setBrush(Qt::NoBrush);

    // ================= Crop / measure ===================================
    } else if (k == "crop") {
        // Overlapping crop brackets, heavier weight than the rest.
        QPen bold(c, 2.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
        p.setPen(bold);
        p.drawPolyline(poly({{17.5, 6}, {6, 6}, {6, 17.5}}));
        p.drawPolyline(poly({{6.5, 18}, {18, 18}, {18, 6.5}}));
        p.setPen(pen);
    } else if (k == "crop-persp") {
        // Perspective crop: keystone quad + thirds grid.
        p.drawPolygon(poly({{6, 6.5}, {18, 9}, {18, 18}, {6, 16}}));
        QPen thin(c, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(thin);
        p.drawLine(QPointF(6, 11.2), QPointF(18, 12.2));
        p.drawLine(QPointF(6, 13.8), QPointF(18, 15.2));
        p.drawLine(QPointF(10, 7.2), QPointF(10, 16.6));
        p.drawLine(QPointF(14, 8.1), QPointF(14, 17.3));
        p.setPen(pen);
    } else if (k == "slice") {
        // Slice grid with cutter diagonal.
        p.drawRect(QRectF(3.5, 5.5, 17, 13));
        QPen dash(c, 1.2, Qt::DashLine, Qt::RoundCap);
        p.setPen(dash);
        p.drawLine(QPointF(12, 5.5), QPointF(12, 18.5));
        p.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));
        p.setPen(pen);
        p.drawLine(QPointF(16, 3.5), QPointF(21, 8.5));
    } else if (k == "slice-sel") {
        p.drawRect(QRectF(3.5, 5.5, 17, 13));
        p.setBrush(fill);
        p.setOpacity(0.25);
        p.drawRect(QRectF(12, 5.5, 8.5, 6.5));
        p.setOpacity(1.0);
        p.setBrush(Qt::NoBrush);
        QPen dash(c, 1.2, Qt::DashLine, Qt::RoundCap);
        p.setPen(dash);
        p.drawLine(QPointF(12, 5.5), QPointF(12, 18.5));
        p.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));
        p.setPen(pen);
        p.setBrush(fill);
        p.drawPolygon(poly({{15, 15}, {15, 21}, {16.8, 19.2}, {18.4, 21}, {19.4, 20.2}, {17.8, 18.4}, {19.8, 18.2}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "frame") {
        // Placeholder frame: rect + X + inset border.
        p.drawRect(QRectF(4, 5, 16, 14));
        p.drawLine(QPointF(4, 5), QPointF(20, 19));
        p.drawLine(QPointF(20, 5), QPointF(4, 19));
    } else if (k == "eyedropper") {
        // Classic pipette: needle tip, barrel, rubber bulb.
        p.drawLine(QPointF(4, 20), QPointF(12.5, 11.5));
        p.setBrush(fill);
        p.drawPolygon(poly({{4, 20}, {5.2, 16.8}, {7.2, 18.8}}));
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(15.5, 8.5);
        p.rotate(45);
        p.drawRect(QRectF(-1.8, -4.5, 3.6, 6.5));
        p.drawRoundedRect(QRectF(-2.6, -8, 5.2, 4.2), 2.0, 2.0);
        p.restore();
    } else if (k == "sampler") {
        // Color Sampler: survey target with centre readout dot.
        p.drawEllipse(QPointF(12, 12), 7.6, 7.6);
        p.drawLine(QPointF(12, 2), QPointF(12, 7));
        p.drawLine(QPointF(12, 17), QPointF(12, 22));
        p.drawLine(QPointF(2, 12), QPointF(7, 12));
        p.drawLine(QPointF(17, 12), QPointF(22, 12));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 1.8, 1.8);
        p.setBrush(Qt::NoBrush);
    } else if (k == "ruler") {
        p.save();
        p.translate(12, 12);
        p.rotate(-28);
        p.drawRoundedRect(QRectF(-9.5, -3.4, 19, 6.8), 1.2, 1.2);
        for (int i = -6; i <= 6; i += 2) {
            const qreal h = (i % 6 == 0) ? 2.8 : 1.6;
            p.drawLine(QPointF(i, -3.4), QPointF(i, -3.4 + h));
        }
        p.restore();
    } else if (k == "note") {
        // Sticky-note speech bubble with text lines.
        p.drawPolygon(poly({{4, 3.5}, {20, 3.5}, {20, 14.5}, {12.5, 14.5}, {7.5, 19.5}, {8.2, 14.5}, {4, 14.5}}));
        p.drawLine(QPointF(7.5, 7.5), QPointF(16.5, 7.5));
        p.drawLine(QPointF(7.5, 11), QPointF(14, 11));
    } else if (k == "count") {
        // Count: map pin with number 1.
        QPainterPath pin(QPointF(12, 21));
        pin.cubicTo(5.5, 14.5, 5, 12, 5.5, 9.5);
        pin.addEllipse(QRectF(5.5, 3, 13, 11));
        p.drawPath(pin);
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(5.5, 3.5, 13, 10), Qt::AlignCenter, QStringLiteral("1"));
        p.setBrush(fill);
        p.drawEllipse(QPointF(18.5, 18.5), 1.4, 1.4);
        p.setBrush(Qt::NoBrush);
    } else if (k == "style-picker") {
        // Style Picker: pipette lifting a two-tone style droplet.
        p.drawLine(QPointF(4, 20), QPointF(11.5, 12.5));
        p.setBrush(fill);
        p.drawPolygon(poly({{4, 20}, {5.2, 16.8}, {7.2, 18.8}}));
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(14.5, 9.5);
        p.rotate(45);
        p.drawRect(QRectF(-1.8, -4, 3.6, 6));
        p.restore();
        p.drawEllipse(QPointF(18, 16.5), 3.4, 3.4);
        QPainterPath half;
        half.moveTo(18, 13.1);
        half.arcTo(QRectF(14.6, 13.1, 6.8, 6.8), 90, -180);
        half.closeSubpath();
        p.fillPath(half, fill);
    } else if (k == "measure") {
        // Drawing-scale measure: dimension line with end ticks + arrows.
        p.drawLine(QPointF(5, 19), QPointF(19, 5));
        p.setBrush(fill);
        p.drawPolygon(poly({{5, 19}, {8.2, 18.6}, {6.8, 16.2}}));
        p.drawPolygon(poly({{19, 5}, {15.8, 5.4}, {17.2, 7.8}}));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(6.5, 15.5), QPointF(9.5, 18.5));
        p.drawLine(QPointF(14.5, 7.5), QPointF(17.5, 10.5));
    } else if (k == "area") {
        // Area: hatched plan shape with dimension frame.
        QPainterPath shape(QPointF(5, 18));
        shape.lineTo(5, 8);
        shape.lineTo(12, 5);
        shape.lineTo(19, 9);
        shape.lineTo(17, 18);
        shape.closeSubpath();
        p.drawPath(shape);
        QPen hatch(c, 1.0, Qt::SolidLine, Qt::RoundCap);
        p.setPen(hatch);
        p.drawLine(QPointF(8, 16.5), QPointF(13, 8.5));
        p.drawLine(QPointF(11, 17), QPointF(15.5, 9.5));
        p.setPen(pen);
        p.setBrush(fill);
        squareNode(p, QPointF(5, 8));
        squareNode(p, QPointF(12, 5));
        squareNode(p, QPointF(19, 9));
        p.setBrush(Qt::NoBrush);

    // ================= Retouch & paint ==================================
    } else if (k == "spot-heal") {
        // Spot Healing: dotted blemish eclipsed by a bandage.
        dashedEllipse(p, QRectF(4.5, 4.5, 10, 10));
        p.save();
        p.translate(14, 14);
        p.rotate(-35);
        p.drawRoundedRect(QRectF(-8.5, -3.2, 17, 6.4), 3.2, 3.2);
        p.drawLine(QPointF(-2.5, -3.2), QPointF(-2.5, 3.2));
        p.drawLine(QPointF(2.5, -3.2), QPointF(2.5, 3.2));
        p.restore();
        p.setBrush(fill);
        p.drawEllipse(QPointF(14, 14), 1.1, 1.1);
        p.setBrush(Qt::NoBrush);
    } else if (k == "remove") {
        // Remove: brush sweeping a dotted flaw away + sparkle.
        p.setBrush(fill);
        strokeBody(p, QPointF(3.5, 20.5), QPointF(11, 12), 2.4);
        p.setBrush(Qt::NoBrush);
        dashedEllipse(p, QRectF(12, 11, 8.5, 8.5));
        p.drawLine(QPointF(14.5, 13.5), QPointF(18, 17));
        p.drawLine(QPointF(18, 13.5), QPointF(14.5, 17));
    } else if (k == "heal") {
        // Healing Brush: bandage with source crosshair.
        p.save();
        p.translate(11, 13);
        p.rotate(-30);
        p.drawRoundedRect(QRectF(-8.5, -3.4, 17, 6.8), 3.4, 3.4);
        QPen dots(c, 1.1, Qt::DotLine, Qt::RoundCap);
        p.setPen(dots);
        p.drawLine(QPointF(-5.5, 0), QPointF(5.5, 0));
        p.setPen(pen);
        p.restore();
        smallPlus(p, QPointF(18.5, 5.5), 2.2);
    } else if (k == "patch") {
        // Patch: stitched graft with drag arrow.
        QPainterPath graft(QPointF(5, 14));
        graft.cubicTo(3.5, 7, 10, 3, 15, 5.5);
        graft.cubicTo(20, 8, 19, 16, 13, 18.5);
        graft.cubicTo(9, 20, 6, 18, 5, 14);
        p.drawPath(graft);
        QPen dots(c, 1.1, Qt::DotLine, Qt::RoundCap);
        p.setPen(dots);
        QPainterPath inner(QPointF(7, 13));
        inner.cubicTo(6, 8.5, 11, 5.5, 14.5, 7.5);
        inner.cubicTo(17.5, 9.5, 16.5, 15, 12.5, 16.5);
        p.drawPath(inner);
        p.setPen(pen);
        p.setBrush(fill);
        p.drawPolygon(poly({{18, 3.5}, {18, 8.5}, {19.4, 6.8}, {21, 8.2}, {21.6, 7}, {20, 5.6}, {20.8, 3.6}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "ca-move") {
        // Content-Aware Move: selected blob shifted with arrow.
        QPainterPath blob(QPointF(4, 13));
        blob.cubicTo(3, 7, 9, 4, 12.5, 6.5);
        blob.cubicTo(15.5, 9, 13, 16, 8, 16.5);
        blob.cubicTo(5.5, 16.7, 4.2, 15, 4, 13);
        p.drawPath(blob);
        p.setBrush(fill);
        p.drawPolygon(poly({{22, 12}, {16, 8.8}, {16, 15.2}}));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(13, 12), QPointF(17, 12));
    } else if (k == "red-eye") {
        // Red Eye: eye outline, red pupil, corrective cross.
        QPainterPath eye(QPointF(2.5, 12));
        eye.cubicTo(7, 5.5, 17, 5.5, 21.5, 12);
        eye.cubicTo(17, 18.5, 7, 18.5, 2.5, 12);
        p.drawPath(eye);
        p.drawEllipse(QPointF(12, 12), 4.4, 4.4);
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 2.0, 2.0);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(17.5, 5), QPointF(21, 8.5));
    } else if (k == "clone") {
        stampBase(p, fill);
    } else if (k == "pattern") {
        stampBase(p, Qt::NoBrush);
        p.setBrush(fill);
        p.drawRect(QRectF(8.2, 6.2, 3.2, 3.2));
        p.drawRect(QRectF(12.6, 9.4, 3.2, 3.2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(11.4, 6.2, 3.2, 3.2));
        p.drawRect(QRectF(8.2, 9.4, 3.2, 3.2));
    } else if (k == "eraser") {
        eraserBody(p, QPointF(12, 12.5), 18, 8.5, -32);
        p.setBrush(fill);
        p.drawEllipse(QPointF(7.5, 15.5), 1.0, 1.0);
        p.drawEllipse(QPointF(10, 17.5), 1.0, 1.0);
        p.setBrush(Qt::NoBrush);
    } else if (k == "eraser-bg") {
        // Background Eraser: eraser + guarded-edge dotted ring.
        eraserBody(p, QPointF(10.5, 14), 14, 7, -32);
        QPen dots(c, 1.2, Qt::DotLine, Qt::RoundCap);
        p.setPen(dots);
        p.drawEllipse(QPointF(17, 7.5), 4.2, 4.2);
        p.setPen(pen);
        p.setBrush(fill);
        p.drawEllipse(QPointF(17, 7.5), 1.2, 1.2);
        p.setBrush(Qt::NoBrush);
    } else if (k == "eraser-magic") {
        eraserBody(p, QPointF(10.5, 14.5), 14, 7, -32);
        smallPlus(p, QPointF(18, 5.5), 2.2);
        p.setBrush(fill);
        p.drawPath(sparklePath(QPointF(20.5, 9.5), 1.5));
        p.setBrush(Qt::NoBrush);
    } else if (k == "blur") {
        // Blur: soft water droplet with highlight.
        QPainterPath drop(QPointF(12, 3));
        drop.cubicTo(16.5, 10, 19.5, 13.5, 19.5, 16.2);
        drop.arcTo(QRectF(4.5, 8.7, 15, 12.3), 0, -180);
        drop.cubicTo(4.5, 13.5, 7.5, 10, 12, 3);
        p.drawPath(drop);
        QPen thin(c, 1.1, Qt::SolidLine, Qt::RoundCap);
        p.setPen(thin);
        p.drawArc(QRectF(7.5, 13, 4, 5), 90 * 16, 140 * 16);
        p.setPen(pen);
    } else if (k == "sharpen") {
        // Sharpen: faceted pyramid/cone.
        p.drawPolygon(poly({{12, 3}, {18.5, 19.5}, {5.5, 19.5}}));
        p.drawLine(QPointF(12, 3), QPointF(12, 19.5));
        p.drawLine(QPointF(8.8, 15.5), QPointF(15.2, 15.5));
    } else if (k == "smudge") {
        // Smudge: fingertip dragging a paint trail.
        QPainterPath trail(QPointF(4.5, 20));
        trail.cubicTo(5, 13, 9.5, 10, 11.5, 4.5);
        trail.cubicTo(14, 8, 15, 10, 17.5, 12.5);
        trail.cubicTo(15.5, 13.5, 13, 14, 11, 16.5);
        p.drawPath(trail);
        p.setBrush(fill);
        p.drawEllipse(QPointF(18.5, 18.5), 2.2, 2.2);
        p.setBrush(Qt::NoBrush);
    } else if (k == "liquify") {
        // Liquify: brush well with an S-warp swirl + arrowhead.
        p.drawEllipse(QPointF(12, 12), 8.6, 8.6);
        QPainterPath swirl(QPointF(17.5, 9));
        swirl.cubicTo(18.5, 13.5, 15, 17.5, 11, 16.5);
        swirl.cubicTo(7.5, 15.5, 7, 10.5, 10.5, 8.8);
        swirl.cubicTo(12.5, 7.8, 15, 9, 14.8, 11.2);
        p.drawPath(swirl);
        p.setBrush(fill);
        p.drawPolygon(poly({{17.5, 9}, {14.8, 8.2}, {15.6, 11.2}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "dodge") {
        // Dodge: lollipop paddle lightening.
        p.drawEllipse(QPointF(9.5, 8.5), 5.4, 5.4);
        QPainterPath half;
        half.moveTo(9.5, 3.1);
        half.arcTo(QRectF(4.1, 3.1, 10.8, 10.8), 90, 180);
        half.closeSubpath();
        p.fillPath(half, fill);
        p.drawLine(QPointF(13.2, 12.2), QPointF(20, 20));
        p.drawLine(QPointF(17.5, 17.5), QPointF(20, 20));
    } else if (k == "burn") {
        // Burn: cupped hand darkening, burn tool.
        QPainterPath hand(QPointF(6.5, 21));
        hand.lineTo(6.5, 12.5);
        hand.cubicTo(6.5, 9.5, 9.5, 9, 10.5, 11.5);
        hand.lineTo(11.5, 6);
        hand.cubicTo(11.8, 3.8, 14.8, 4.2, 14.3, 6.8);
        hand.lineTo(13.8, 11.5);
        hand.cubicTo(16.5, 11, 18.5, 13.5, 17.5, 17);
        hand.lineTo(16, 21);
        hand.closeSubpath();
        p.drawPath(hand);
        p.drawLine(QPointF(10, 15.5), QPointF(14.5, 15.5));
    } else if (k == "sponge") {
        // Sponge: porous block with sheen.
        p.drawRoundedRect(QRectF(4, 8.5, 16, 10.5), 4.5, 4.5);
        p.setBrush(fill);
        p.drawEllipse(QPointF(9, 12.5), 1.3, 1.3);
        p.drawEllipse(QPointF(13.5, 11.8), 1.3, 1.3);
        p.drawEllipse(QPointF(11.5, 15.8), 1.3, 1.3);
        p.drawEllipse(QPointF(16.2, 15.2), 1.3, 1.3);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(6.5, 10.5), QPointF(9.5, 10.5));
    } else if (k == "brush") {
        // Brush: tapered handle, ferrule, pointed bristles.
        p.setBrush(fill);
        strokeBody(p, QPointF(16.5, 4.5), QPointF(8.5, 13.5), 2.0);
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(12.2, 9.8);
        p.rotate(45);
        p.drawRect(QRectF(-1.9, -1.4, 3.8, 2.8));
        p.restore();
        p.setBrush(fill);
        QPainterPath tip(QPointF(3.5, 20.5));
        tip.quadTo(5.5, 15.5, 8.5, 13.5);
        tip.quadTo(6.5, 17.5, 3.5, 20.5);
        p.drawPath(tip);
        p.setBrush(Qt::NoBrush);
    } else if (k == "pencil") {
        // Pencil: hex body, sharpened wood, graphite point, eraser cap.
        p.drawPolygon(poly({{3.5, 20.5}, {5, 16}, {14.5, 6.5}, {17.5, 9.5}, {8, 19}}));
        p.drawPolygon(poly({{5, 16}, {6.8, 17.8}, {8, 19}, {6.2, 19.2}}));
        p.setBrush(fill);
        p.drawPolygon(poly({{3.5, 20.5}, {5.2, 18.8}, {6.2, 19.2}}));
        p.save();
        p.translate(16.2, 6.2);
        p.rotate(45);
        p.drawRect(QRectF(-2.6, -2.8, 5.2, 2.6));
        p.restore();
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5, 16), QPointF(8, 19));
    } else if (k == "replace") {
        // Color Replacement: brush + split color well + orbit.
        p.setBrush(fill);
        strokeBody(p, QPointF(3.5, 20.5), QPointF(10, 12.5), 2.3);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(17, 8), 4.6, 4.6);
        QPainterPath half;
        half.moveTo(17, 3.4);
        half.arcTo(QRectF(12.4, 3.4, 9.2, 9.2), 90, -180);
        half.closeSubpath();
        p.fillPath(half, fill);
        p.drawLine(QPointF(13.5, 14.5), QPointF(15.5, 17.5));
        p.drawLine(QPointF(20.5, 14.5), QPointF(18.5, 17.5));
    } else if (k == "mixer") {
        // Mixer Brush: brush loading paint from a droplet palette.
        p.setBrush(fill);
        strokeBody(p, QPointF(3, 21), QPointF(10, 13), 2.2);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(16.5, 9), 5.2, 5.2);
        QPainterPath drop(QPointF(16.5, 5.5));
        drop.cubicTo(18.5, 8.5, 19.5, 10, 19.5, 11.5);
        drop.cubicTo(19.5, 13.5, 18, 14.5, 16.5, 14.5);
        drop.cubicTo(15, 14.5, 13.5, 13.5, 13.5, 11.5);
        drop.cubicTo(13.5, 10, 14.5, 8.5, 16.5, 5.5);
        p.drawPath(drop);
    } else if (k == "history-br") {
        // History Brush: brush + rewind clock arrow.
        p.setBrush(fill);
        strokeBody(p, QPointF(3.5, 20.5), QPointF(10, 13), 2.3);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(12.5, 3.5, 9, 9), 30 * 16, 290 * 16);
        p.setBrush(fill);
        p.drawPolygon(poly({{17.5, 2.5}, {17.3, 6}, {20, 4.2}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "art-history") {
        // Art History: brush trailing an artistic flourish curl.
        p.setBrush(fill);
        strokeBody(p, QPointF(3.5, 20.5), QPointF(10, 13), 2.3);
        p.setBrush(Qt::NoBrush);
        QPainterPath curl(QPointF(12, 12));
        curl.cubicTo(16, 4, 22, 7, 19, 10.5);
        curl.cubicTo(17, 13, 13.5, 11.5, 15, 8);
        curl.cubicTo(15.8, 6.2, 18.5, 6.5, 19.2, 8.2);
        p.drawPath(curl);
    } else if (k == "gradient") {
        // Gradient: swatch fading to transparent + drag arrow.
        QLinearGradient g(4, 0, 20, 0);
        g.setColorAt(0, c);
        QColor faded = c;
        faded.setAlpha(25);
        g.setColorAt(1, faded);
        p.setBrush(g);
        p.drawRect(QRectF(4, 7, 16, 10));
        p.setBrush(Qt::NoBrush);
        p.setBrush(fill);
        p.drawPolygon(poly({{5, 19.5}, {9, 19.5}, {7, 21.5}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "bucket") {
        // Paint Bucket: tilted pail pouring a drop onto a fill line.
        p.save();
        p.translate(10, 11);
        p.rotate(-28);
        p.drawPolygon(poly({{-5.5, -5}, {5.5, -5}, {4, 6}, {-4, 6}}));
        p.drawLine(QPointF(-5.5, -5), QPointF(5.5, -5));
        p.restore();
        p.setBrush(fill);
        QPainterPath drop(QPointF(19, 12));
        drop.cubicTo(21.5, 15.5, 22, 18, 19, 18.5);
        drop.cubicTo(16, 18, 16.5, 15.5, 19, 12);
        p.drawPath(drop);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(4, 20.5), QPointF(15, 20.5));
    } else if (k == "transparency") {
        // Transparency: solid fading into checkerboard.
        p.drawRoundedRect(QRectF(4, 6, 16, 12), 1.5, 1.5);
        p.drawLine(QPointF(12, 6), QPointF(12, 18));
        QColor faint = c;
        faint.setAlpha(110);
        p.fillRect(QRectF(12.8, 6.8, 3.1, 3.1), faint);
        p.fillRect(QRectF(16, 10, 3.2, 3.2), faint);
        p.fillRect(QRectF(12.8, 13.2, 3.1, 3.1), faint);
        p.setBrush(fill);
        p.drawPolygon(poly({{4, 20.5}, {8, 20.5}, {6, 22}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "adj-brush") {
        // Adjustment Brush: brush + tune sliders.
        p.setBrush(fill);
        strokeBody(p, QPointF(3, 21), QPointF(9.5, 13.5), 2.2);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(13, 5), QPointF(21, 5));
        p.drawLine(QPointF(13, 10), QPointF(21, 10));
        p.drawLine(QPointF(13, 15), QPointF(21, 15));
        p.setBrush(fill);
        p.drawEllipse(QPointF(16, 5), 1.9, 1.9);
        p.drawEllipse(QPointF(18.5, 10), 1.9, 1.9);
        p.drawEllipse(QPointF(15, 15), 1.9, 1.9);
        p.setBrush(Qt::NoBrush);

    // ================= Draw & type ======================================
    } else if (k == "pen") {
        // Pen nib front view with breather hole + slit.
        p.drawPolygon(poly({{12, 2.5}, {17.5, 9}, {15.5, 17.5}, {8.5, 17.5}, {6.5, 9}}));
        p.drawLine(QPointF(12, 17.5), QPointF(12, 21.5));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 11), 1.7, 1.7);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 12.7), QPointF(12, 17.5));
    } else if (k == "pen-free") {
        // Freeform Pen: nib + loose freehand wave.
        p.drawPolygon(poly({{15.5, 2.5}, {20, 7}, {9, 19}, {4, 20}, {5, 15}}));
        QPainterPath wig(QPointF(3, 11.5));
        wig.cubicTo(6, 6.5, 8.5, 13.5, 12, 8.5);
        p.drawPath(wig);
    } else if (k == "pen-curve") {
        // Curvature Pen: nib + pinned curve.
        p.drawPolygon(poly({{15.5, 2.5}, {20, 7}, {9, 19}, {4, 20}, {5, 15}}));
        QPainterPath arc(QPointF(3, 20.5));
        arc.cubicTo(9, 21.5, 17, 20, 21, 14);
        p.drawPath(arc);
        p.setBrush(fill);
        squareNode(p, QPointF(9, 20.5));
        squareNode(p, QPointF(15, 19.2));
        p.setBrush(Qt::NoBrush);
    } else if (k == "trace") {
        // Content-Aware Tracing: dashed photo edge + committed trace.
        QPen dash(c, 1.2, Qt::DashLine, Qt::RoundCap);
        p.setPen(dash);
        p.drawEllipse(QRectF(3.5, 5.5, 14, 11));
        p.setPen(pen);
        QPainterPath edge(QPointF(3.5, 12));
        edge.cubicTo(7, 5.5, 13, 5.5, 17.5, 11);
        p.drawPath(edge);
        p.setBrush(fill);
        p.drawPolygon(poly({{17.5, 11}, {14.5, 19}, {16.5, 19.5}, {18, 21.5}, {19, 20.5}, {17.6, 18.7}, {19.8, 18.2}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "anchor-add" || k == "anchor-del" || k == "anchor-conv") {
        QPainterPath seg(QPointF(3, 18));
        seg.cubicTo(7, 6.5, 16, 6, 21, 15.5);
        p.drawPath(seg);
        p.setBrush(fill);
        p.drawRect(QRectF(10.3, 7, 3.4, 3.4));
        p.setBrush(Qt::NoBrush);
        squareNode(p, QPointF(3, 18), 2.0);
        squareNode(p, QPointF(21, 15.5), 2.0);
        if (k == "anchor-add") {
            smallPlus(p, QPointF(18, 20), 2.2);
        } else if (k == "anchor-del") {
            p.drawLine(QPointF(15.8, 20), QPointF(20.2, 20));
        } else {
            // Convert: direction handles forming a peak.
            p.drawLine(QPointF(18, 20), QPointF(15, 17.5));
            p.drawLine(QPointF(18, 20), QPointF(21, 17.5));
            p.setBrush(fill);
            p.drawEllipse(QPointF(18, 20), 1.3, 1.3);
            p.setBrush(Qt::NoBrush);
        }
    } else if (k == "stroke-width") {
        // Stroke Width: tapering pressure profile + nodes.
        QPainterPath profile(QPointF(3.5, 12));
        profile.cubicTo(8, 8.5, 16, 8.5, 20.5, 12);
        profile.cubicTo(16, 15.5, 8, 15.5, 3.5, 12);
        p.drawPath(profile);
        p.drawLine(QPointF(12, 8.8), QPointF(12, 15.2));
        p.setBrush(fill);
        squareNode(p, QPointF(3.5, 12));
        squareNode(p, QPointF(20.5, 12));
        p.drawEllipse(QPointF(12, 12), 1.4, 1.4);
        p.setBrush(Qt::NoBrush);
    } else if (k == "knife") {
        // Knife: craft blade slicing a dashed cut.
        p.drawLine(QPointF(4, 20), QPointF(12, 12));
        QPen dash(c, 1.2, Qt::DashLine, Qt::RoundCap);
        p.setPen(dash);
        p.drawLine(QPointF(12, 12), QPointF(20, 4));
        p.setPen(pen);
        p.setBrush(fill);
        p.drawPolygon(poly({{4, 20}, {6.5, 13.5}, {9.5, 15.5}}));
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(8.5, 15.5);
        p.rotate(45);
        p.drawRect(QRectF(-1.6, -5, 3.2, 7));
        p.restore();
    } else if (k == "path-sel" || k == "direct-sel") {
        // Path arrows: solid black vs hollow white.
        if (k == "path-sel") p.setBrush(fill);
        p.drawPolygon(poly({{7, 2.5}, {7, 18}, {10.8, 14.2}, {13.6, 20.5}, {16, 19.2}, {13.4, 13.2}, {18.4, 12.6}}));
        p.setBrush(Qt::NoBrush);
        if (k == "direct-sel") {
            p.setBrush(fill);
            squareNode(p, QPointF(7, 2.5), 1.8);
            p.setBrush(Qt::NoBrush);
        }
    } else if (k == "node") {
        // Node Tool: bezier with square nodes + handles.
        QPainterPath curve(QPointF(4, 18));
        curve.cubicTo(8, 6, 16, 18, 20, 8);
        p.drawPath(curve);
        QPen thin(c, 1.1, Qt::SolidLine, Qt::RoundCap);
        p.setPen(thin);
        p.drawLine(QPointF(8.5, 11.5), QPointF(5.5, 9));
        p.drawLine(QPointF(15.5, 13), QPointF(18.5, 15.5));
        p.setPen(pen);
        p.setBrush(fill);
        p.drawRect(QRectF(7.3, 10.3, 2.6, 2.6));
        p.drawRect(QRectF(14.2, 11.7, 2.6, 2.6));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(5.5, 9), 1.1, 1.1);
        p.drawEllipse(QPointF(18.5, 15.5), 1.1, 1.1);
    } else if (k == "point-xform") {
        // Point Transform: boxed point with pivot + rotate handle.
        p.drawRect(QRectF(6, 7, 11, 11));
        p.setBrush(fill);
        squareNode(p, QPointF(6, 7));
        squareNode(p, QPointF(17, 7));
        squareNode(p, QPointF(6, 18));
        squareNode(p, QPointF(17, 18));
        p.drawEllipse(QPointF(11.5, 12.5), 1.4, 1.4);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(13.5, 2.5, 7, 7), 200 * 16, 220 * 16);
        p.setBrush(fill);
        p.drawPolygon(poly({{20.5, 6.5}, {17.5, 6.8}, {19.2, 9}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "corner") {
        // Corner Tool: sharp L pulled into a rounded corner.
        p.drawPolyline(poly({{5, 19}, {5, 7}, {19, 7}}));
        QPainterPath round(QPointF(5, 14));
        round.quadTo(5, 7, 12, 7);
        p.drawPath(round);
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 7), 1.8, 1.8);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(12, 7), QPointF(16, 11));
        p.drawEllipse(QPointF(16, 11), 1.2, 1.2);
    } else if (k == "contour") {
        // Contour: offset outline with distance arrow.
        p.drawRoundedRect(QRectF(4.5, 4.5, 12, 12), 2, 2);
        p.drawRoundedRect(QRectF(8, 8, 12, 12), 2, 2);
        p.setBrush(fill);
        p.drawPolygon(poly({{20, 12}, {17.5, 10.5}, {17.5, 13.5}}));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(16.5, 4.5), QPointF(16.5, 2.5));
        p.drawLine(QPointF(20, 8), QPointF(22, 8));
    } else if (k == "shape-rect") {
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(4, 7, 16, 10), 1.2, 1.2);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(4, 7, 16, 10), 1.2, 1.2);
    } else if (k == "shape-ell") {
        p.drawEllipse(QRectF(3.5, 6, 17, 12));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 1.3, 1.3);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-tri") {
        p.drawPolygon(poly({{12, 4.5}, {20.5, 19}, {3.5, 19}}));
        p.setBrush(fill);
        p.drawPolygon(poly({{12, 10.5}, {15.5, 16.5}, {8.5, 16.5}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-poly") {
        p.drawPolygon(regularPolygon(QPointF(12, 12), 8.6, 6));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 1.4, 1.4);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-star") {
        p.drawPolygon(starPolygon(QPointF(12, 12), 9.2, 4.0, 5));
    } else if (k == "shape-line") {
        p.setBrush(fill);
        p.drawPolygon(poly({{4, 20}, {5.5, 18}, {18, 5.5}, {20, 4}}));
        p.drawPolygon(poly({{20, 4}, {16.5, 4.2}, {17.2, 7.5}}));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(4.5, 19.5), QPointF(17, 7));
    } else if (k == "shape-custom") {
        // Crown/fleur custom shape: more "library" than a heart.
        p.drawPolygon(starPolygon(QPointF(12, 12), 9.0, 5.2, 6));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 2.2, 2.2);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-rounded") {
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(4, 6, 16, 12), 3.5, 3.5);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-diamond") {
        p.setBrush(fill);
        p.drawPolygon(poly({{12, 3.5}, {20.5, 12}, {12, 20.5}, {3.5, 12}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-trapezoid") {
        p.setBrush(fill);
        p.drawPolygon(poly({{8, 5}, {16, 5}, {21, 19}, {3, 19}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-doublestar") {
        p.setBrush(fill);
        p.drawPolygon(starPolygon(QPointF(12, 12), 9.5, 4.2, 8));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-squarestar") {
        p.setBrush(fill);
        p.drawPolygon(starPolygon(QPointF(12, 12), 9.5, 7.4, 4));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-arrow") {
        p.drawLine(QPointF(3, 12), QPointF(15, 12));
        p.setBrush(fill);
        p.drawPolygon(poly({{15, 6.5}, {22, 12}, {15, 17.5}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-donut") {
        p.setBrush(fill);
        QPainterPath ring;
        ring.addEllipse(QRectF(3.5, 3.5, 17, 17));
        ring.addEllipse(QRectF(8, 8, 8, 8));
        p.drawPath(ring);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-pie") {
        p.setBrush(fill);
        QPainterPath pie(QPointF(12, 12));
        pie.moveTo(12, 12);
        pie.lineTo(20.5, 12);
        pie.arcTo(QRectF(3.5, 3.5, 17, 17), 0, 90);
        pie.closeSubpath();
        p.drawPath(pie);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-segment") {
        p.drawEllipse(QRectF(3.5, 5.5, 17, 13));
        p.drawLine(QPointF(5, 15), QPointF(19, 9));
    } else if (k == "shape-crescent") {
        QPainterPath moon(QPointF(14, 4));
        moon.arcTo(QRectF(4, 3, 16, 18), 90, 180);
        moon.arcTo(QRectF(8, 3, 16, 18), 270, 180);
        moon.closeSubpath();
        p.setBrush(fill);
        p.drawPath(moon);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-cog") {
        p.drawEllipse(QRectF(6.5, 6.5, 11, 11));
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4;
            p.drawLine(QPointF(12 + 5.5 * qCos(a), 12 + 5.5 * qSin(a)),
                       QPointF(12 + 8.5 * qCos(a), 12 + 8.5 * qSin(a)));
        }
    } else if (k == "shape-cloud") {
        p.drawEllipse(QPointF(8.5, 13), 4.5, 4.5);
        p.drawEllipse(QPointF(13.5, 10.5), 5, 5);
        p.drawEllipse(QPointF(17.5, 14), 3.6, 3.6);
        p.drawLine(QPointF(4.5, 16.5), QPointF(20.5, 16.5));
    } else if (k == "shape-callout-rect") {
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(3, 3, 18, 11), 2, 2);
        p.drawPolygon(poly({{8, 14}, {8, 21}, {13, 14}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-callout-ell") {
        p.drawEllipse(QRectF(3, 3, 18, 11));
        p.setBrush(fill);
        p.drawPolygon(poly({{14, 13}, {19, 20}, {11, 14.5}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-tear") {
        QPainterPath tear(QPointF(12, 2.5));
        tear.cubicTo(16, 10, 19.5, 13.5, 19.5, 17);
        tear.arcTo(QRectF(4.5, 9.5, 15, 15), 0, -180);
        tear.cubicTo(8, 13.5, 8.5, 10, 12, 2.5);
        p.setBrush(fill);
        p.drawPath(tear);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-heart") {
        QPainterPath heart(QPointF(12, 20));
        heart.cubicTo(2, 13, 4, 4, 12, 8);
        heart.cubicTo(20, 4, 22, 13, 12, 20);
        p.drawPath(heart);
    } else if (k == "shape-spiral") {
        QPainterPath spiral(QPointF(12, 12));
        spiral.cubicTo(16, 12, 18, 14, 17, 17);
        spiral.cubicTo(16, 20.5, 11, 21.5, 7.5, 19);
        spiral.cubicTo(3.5, 16, 5, 10, 10, 8.5);
        spiral.cubicTo(15, 7, 20.5, 11, 19.5, 16.5);
        p.drawPath(spiral);
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 1.6, 1.6);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-qr") {
        p.setBrush(fill);
        p.drawRect(QRectF(3.5, 3.5, 7, 7));
        p.drawRect(QRectF(13.5, 3.5, 7, 7));
        p.drawRect(QRectF(3.5, 13.5, 7, 7));
        p.drawRect(QRectF(14.5, 14.5, 3, 3));
        p.drawRect(QRectF(18, 18, 2.5, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(14.5, 18, 2.5, 2.5));
    } else if (k == "shape-cat") {
        p.drawPolygon(poly({{5, 11}, {4, 4}, {10, 7}}));
        p.drawPolygon(poly({{19, 11}, {20, 4}, {14, 7}}));
        p.drawEllipse(QPointF(12, 13.5), 7, 6.5);
        p.setBrush(fill);
        p.drawEllipse(QPointF(9.5, 12.5), 1.2, 1.2);
        p.drawEllipse(QPointF(14.5, 12.5), 1.2, 1.2);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-hexagon") {
        p.setBrush(fill);
        p.drawPolygon(regularPolygon(QPointF(12, 12), 9, 6));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-octagon") {
        p.setBrush(fill);
        p.drawPolygon(regularPolygon(QPointF(12, 12), 9, 8));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-cross") {
        p.setBrush(fill);
        p.drawPolygon(poly({{9, 4}, {15, 4}, {15, 9}, {20, 9}, {20, 15}, {15, 15},
                            {15, 20}, {9, 20}, {9, 15}, {4, 15}, {4, 9}, {9, 9}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-rtriangle") {
        p.setBrush(fill);
        p.drawPolygon(poly({{6, 4}, {6, 20}, {20, 20}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-parallelogram") {
        p.setBrush(fill);
        p.drawPolygon(poly({{9, 5}, {21, 5}, {15, 19}, {3, 19}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-chevron") {
        p.setBrush(fill);
        p.drawPolygon(poly({{4, 5}, {13, 12}, {4, 19}, {4, 15}, {9.5, 12}, {4, 9}}));
        p.drawPolygon(poly({{11, 5}, {20, 12}, {11, 19}, {11, 15}, {16.5, 12}, {11, 9}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-doublearrow") {
        p.drawLine(QPointF(7, 12), QPointF(17, 12));
        p.setBrush(fill);
        p.drawPolygon(poly({{7, 7.5}, {2, 12}, {7, 16.5}}));
        p.drawPolygon(poly({{17, 7.5}, {22, 12}, {17, 16.5}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-circulararrow") {
        QPainterPath arc(QPointF(19, 9));
        arc.arcTo(QRectF(4, 4, 16, 16), 45, 270);
        p.drawPath(arc);
        p.setBrush(fill);
        p.drawPolygon(poly({{19, 9}, {14.5, 7.5}, {16.5, 12.5}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-sparkle") {
        p.setBrush(fill);
        p.drawPolygon(starPolygon(QPointF(12, 12), 9.5, 2.2, 4));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-shield") {
        p.setBrush(fill);
        p.drawPolygon(poly({{6, 3.5}, {18, 3.5}, {18, 11}, {12, 20.5}, {6, 11}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-ticket") {
        p.drawRect(QRectF(3, 7, 18, 10));
        p.setBrush(fill);
        p.drawEllipse(QPointF(3, 12), 2, 2.5);
        p.drawEllipse(QPointF(21, 12), 2, 2.5);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(16, 7), QPointF(16, 17));
    } else if (k == "shape-sun") {
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 4.5, 4.5);
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4;
            p.drawLine(QPointF(12 + 6 * qCos(a), 12 + 6 * qSin(a)),
                       QPointF(12 + 9 * qCos(a), 12 + 9 * qSin(a)));
        }
    } else if (k == "type" || k == "type-mask") {
        QFont f = p.font();
        f.setPixelSize(18);
        f.setBold(true);
        f.setFamily(QStringLiteral("Serif"));
        p.setFont(f);
        p.drawText(QRectF(2, 1, 20, 18), Qt::AlignCenter, QStringLiteral("T"));
        p.drawLine(QPointF(6, 20.5), QPointF(18, 20.5));
        if (k == "type-mask") dashedRect(p, QRectF(3, 3, 18, 18));
    } else if (k == "type-vert" || k == "type-mask-v") {
        QFont f = p.font();
        f.setPixelSize(13);
        f.setBold(true);
        p.setFont(f);
        p.save();
        p.translate(12, 12);
        p.rotate(90);
        p.drawText(QRectF(-10, -8, 20, 16), Qt::AlignCenter, QStringLiteral("T"));
        p.restore();
        p.drawLine(QPointF(15.5, 4), QPointF(15.5, 20));
        p.setBrush(fill);
        p.drawPolygon(poly({{15.5, 21.5}, {13.7, 18.5}, {17.3, 18.5}}));
        p.setBrush(Qt::NoBrush);
        if (k == "type-mask-v") dashedRect(p, QRectF(3, 3, 18, 18));
    } else if (k == "vector-brush") {
        // Vector Brush: tapered stroke with end nodes + bristles.
        QPainterPath stroke(QPointF(3.5, 19));
        stroke.cubicTo(8, 17, 14, 15, 20.5, 6.5);
        QPen fat(c, 2.6, Qt::SolidLine, Qt::RoundCap);
        p.setPen(fat);
        p.drawPath(stroke);
        p.setPen(pen);
        p.setBrush(fill);
        squareNode(p, QPointF(3.5, 19));
        squareNode(p, QPointF(20.5, 6.5));
        p.setBrush(Qt::NoBrush);
    } else if (k == "flood-vector") {
        // Vector Flood Fill: pail filling a bounded blob.
        QPainterPath blob(QPointF(4, 14));
        blob.cubicTo(4, 7, 10, 5, 14, 7);
        blob.cubicTo(18, 9, 18, 16, 13, 18);
        blob.cubicTo(8, 20, 4, 18, 4, 14);
        p.drawPath(blob);
        p.save();
        p.translate(16.5, 6.5);
        p.rotate(-25);
        p.drawPolygon(poly({{-3.5, -2.5}, {3.5, -2.5}, {2.5, 3.5}, {-2.5, 3.5}}));
        p.restore();
        p.setBrush(fill);
        QPainterPath drop(QPointF(14.5, 11));
        drop.cubicTo(16, 13, 16.2, 14.5, 14.5, 14.8);
        drop.cubicTo(12.8, 14.5, 13, 13, 14.5, 11);
        p.drawPath(drop);
        p.setBrush(Qt::NoBrush);
    } else if (k == "shape-builder") {
        // Shape Builder: two overlapping shapes merged with plus.
        p.drawEllipse(QPointF(9.5, 12), 5.5, 5.5);
        p.drawRect(QRectF(10.5, 7.5, 9, 9));
        p.setBrush(fill);
        p.drawEllipse(QPointF(17.5, 18.5), 2.8, 2.8);
        p.setBrush(Qt::NoBrush);
        QPen bg(Qt::white, 1.4, Qt::SolidLine, Qt::RoundCap);
        // Knockout ring so the badge reads on any theme:
        p.setPen(QPen(c, 3.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(17.5, 17), QPointF(17.5, 20));
        p.drawLine(QPointF(16, 18.5), QPointF(19, 18.5));
        p.setPen(pen);
        smallPlus(p, QPointF(17.5, 18.5), 1.8);
    } else if (k == "crop-vector") {
        // Vector Crop: crop Ls biting a noded rectangle.
        QPen bold(c, 1.9, Qt::SolidLine, Qt::SquareCap);
        p.setPen(bold);
        p.drawPolyline(poly({{10, 3.5}, {3.5, 3.5}, {3.5, 10}}));
        p.drawPolyline(poly({{14, 20.5}, {20.5, 20.5}, {20.5, 14}}));
        p.setPen(pen);
        p.drawRect(QRectF(7, 7.5, 10, 9));
        p.setBrush(fill);
        squareNode(p, QPointF(7, 7.5));
        squareNode(p, QPointF(17, 7.5));
        squareNode(p, QPointF(7, 16.5));
        squareNode(p, QPointF(17, 16.5));
        p.setBrush(Qt::NoBrush);
    } else if (k == "place") {
        // Place: landscape photo landing into a frame with arrow.
        p.drawRect(QRectF(3.5, 6, 13, 10));
        p.drawPolyline(poly({{3.5, 13.5}, {8, 9.5}, {11, 12}, {13, 10}, {16.5, 13}}));
        p.setBrush(fill);
        p.drawEllipse(QPointF(7, 9), 1.3, 1.3);
        p.setBrush(Qt::NoBrush);
        p.setBrush(fill);
        p.drawPolygon(poly({{14, 15}, {14, 21}, {15.8, 19.2}, {17.4, 21}, {18.4, 20.2}, {16.8, 18.4}, {18.8, 18.2}}));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(18.5, 13.5), QPointF(21.5, 13.5));
        p.drawLine(QPointF(21.5, 13.5), QPointF(21.5, 16.5));

    // ================= Navigation =======================================
    } else if (k == "hand") {
        QPainterPath hand(QPointF(6.5, 19.5));
        hand.lineTo(5.5, 12);
        hand.cubicTo(5.5, 9.8, 8.2, 9.6, 8.2, 12);
        hand.lineTo(8.2, 6);
        hand.cubicTo(8.2, 3.8, 11, 3.8, 11, 6);
        hand.lineTo(11, 10.5);
        hand.lineTo(11.6, 6.3);
        hand.cubicTo(11.9, 4.2, 14.6, 4.5, 14.3, 6.9);
        hand.lineTo(13.9, 10.8);
        hand.lineTo(15.2, 8.2);
        hand.cubicTo(16.2, 6.2, 18.8, 7.4, 17.9, 9.6);
        hand.lineTo(16.3, 15.8);
        hand.cubicTo(15.5, 19.5, 13.8, 20.8, 10.8, 20.8);
        hand.closeSubpath();
        p.drawPath(hand);
        p.drawLine(QPointF(8.2, 12), QPointF(8.2, 16.5));
        p.drawLine(QPointF(11, 10.5), QPointF(11, 16.5));
    } else if (k == "rotate-view") {
        p.drawArc(QRectF(4.5, 4.5, 15, 15), 35 * 16, 275 * 16);
        p.setBrush(fill);
        p.drawPolygon(poly({{19.5, 9.5}, {15.2, 9.8}, {17.8, 13.4}}));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 1.6, 1.6);
        p.drawLine(QPointF(12, 10.4), QPointF(12, 7.5));
    } else if (k == "zoom") {
        p.drawEllipse(QPointF(10, 10), 6.6, 6.6);
        p.setBrush(fill);
        p.drawEllipse(QPointF(10, 10), 1.2, 1.2);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(14.8, 14.8), QPointF(20.5, 20.5));
        QPen bold(c, 1.8, Qt::SolidLine, Qt::RoundCap);
        p.setPen(bold);
        p.drawLine(QPointF(10, 7), QPointF(10, 13));
        p.drawLine(QPointF(7, 10), QPointF(13, 10));
        p.setPen(pen);

    // ================= Generative =======================================
    } else if (k == "import") {
        p.drawRoundedRect(QRectF(3.5, 5.5, 17, 13.5), 1.5, 1.5);
        p.drawPolyline(poly({{3.5, 15}, {9, 9.5}, {12.5, 13}, {15, 10.5}, {20.5, 15.5}}));
        p.setBrush(fill);
        p.drawEllipse(QPointF(8, 9.5), 1.5, 1.5);
        p.setBrush(Qt::NoBrush);
        p.setBrush(fill);
        p.drawPolygon(poly({{17, 3}, {17, 6.5}, {18, 5.2}, {19.5, 6.5}, {20, 5.5}, {18.5, 4.2}, {19.2, 2.7}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "gen-fill") {
        dashedRect(p, QRectF(3.5, 7.5, 12, 9));
        p.setBrush(fill);
        p.drawPath(sparklePath(QPointF(17.5, 8), 4.6));
        p.drawPath(sparklePath(QPointF(12, 18.5), 2.6));
        p.setBrush(Qt::NoBrush);
    } else if (k == "gen-bg") {
        p.drawRect(QRectF(3.5, 9.5, 13, 9.5));
        p.drawPolyline(poly({{3.5, 16}, {8, 11.5}, {11, 14.5}, {13.5, 12}, {16.5, 15}}));
        p.setBrush(fill);
        p.drawPath(sparklePath(QPointF(18, 7), 4.8));
        p.drawPath(sparklePath(QPointF(13.5, 6), 2.2));
        p.setBrush(Qt::NoBrush);
    } else if (k == "sparkle") {
        p.setBrush(fill);
        p.drawPath(sparklePath(QPointF(9.5, 11.5), 7.0));
        p.drawPath(sparklePath(QPointF(18, 17.5), 3.8));
        p.setBrush(Qt::NoBrush);
        p.setBrush(fill);
        p.drawEllipse(QPointF(19, 5.5), 1.2, 1.2);
        p.setBrush(Qt::NoBrush);

    // --- Chrome / panels ---------------------------------------------------
    } else if (k == "layers") {
        p.drawPolygon(poly({{12, 3}, {21, 8}, {12, 13}, {3, 8}}));
        p.drawPolyline(poly({{4.5, 12}, {12, 16.2}, {19.5, 12}}));
        p.drawPolyline(poly({{4.5, 16}, {12, 20.2}, {19.5, 16}}));
    } else if (k == "channels") {
        p.drawEllipse(QPointF(9.5, 9.5), 5.4, 5.4);
        p.drawEllipse(QPointF(14.5, 9.5), 5.4, 5.4);
        p.drawEllipse(QPointF(12, 14.5), 5.4, 5.4);
    } else if (k == "paths") {
        QPainterPath arc(QPointF(3.5, 18));
        arc.cubicTo(8, 5, 16, 5, 20.5, 18);
        p.drawPath(arc);
        p.setBrush(fill);
        p.drawRect(QRectF(2, 16.5, 3, 3));
        p.drawRect(QRectF(19, 16.5, 3, 3));
        p.setBrush(Qt::NoBrush);
    } else if (k == "adjustments") {
        p.drawEllipse(QPointF(12, 12), 8.6, 8.6);
        QPainterPath half;
        half.moveTo(12, 3.4);
        half.arcTo(QRectF(3.4, 3.4, 17.2, 17.2), 90, -180);
        half.closeSubpath();
        p.fillPath(half, fill);
    } else if (k == "properties") {
        p.drawLine(QPointF(4, 7), QPointF(20, 7));
        p.drawLine(QPointF(4, 12), QPointF(20, 12));
        p.drawLine(QPointF(4, 17), QPointF(20, 17));
        p.setBrush(QBrush(c));
        p.drawEllipse(QPointF(9, 7), 2.1, 2.1);
        p.drawEllipse(QPointF(15, 12), 2.1, 2.1);
        p.drawEllipse(QPointF(7.5, 17), 2.1, 2.1);
        p.setBrush(Qt::NoBrush);
    } else if (k == "color") {
        p.drawEllipse(QPointF(12, 12), 8.4, 8.4);
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 3.6, 3.6);
        p.setBrush(Qt::NoBrush);
    } else if (k == "swatches") {
        for (int gy = 0; gy < 3; ++gy)
            for (int gx = 0; gx < 3; ++gx) {
                const QRectF r(4 + gx * 5.6, 4 + gy * 5.6, 4.6, 4.6);
                if ((gx + gy) % 2 == 0) p.fillRect(r, fill);
                else p.drawRect(r);
            }
    } else if (k == "history") {
        p.drawArc(QRectF(4, 4, 16, 16), 50 * 16, 280 * 16);
        p.drawLine(QPointF(12, 8), QPointF(12, 12.5));
        p.drawLine(QPointF(12, 12.5), QPointF(15.5, 14.5));
        p.setBrush(fill);
        p.drawPolygon(poly({{19.5, 8.5}, {15, 8.6}, {18, 12}}));
        p.setBrush(Qt::NoBrush);
    } else if (k == "info") {
        p.drawEllipse(QPointF(12, 12), 8.6, 8.6);
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 7.6), 1.15, 1.15);
        p.drawRoundedRect(QRectF(11, 10.6, 2, 7), 1, 1);
        p.setBrush(Qt::NoBrush);
    } else if (k == "navigator") {
        p.drawRect(QRectF(3.5, 4.5, 17, 15));
        QPen pen2(c, 1.6);
        p.setPen(pen2);
        p.drawRect(QRectF(7, 8, 9, 8));
        p.setPen(pen);
    } else if (k == "histogram") {
        // Five bars on a baseline.
        p.drawLine(QPointF(3.5, 20.5), QPointF(20.5, 20.5));
        p.setBrush(fill);
        const double hs[5] = {5.0, 9.0, 13.0, 8.0, 11.0};
        for (int i = 0; i < 5; ++i)
            p.drawRect(QRectF(4.5 + i * 3.4, 20.5 - hs[i], 2.2, hs[i]));
        p.setBrush(Qt::NoBrush);
    } else if (k == "brushes") {
        p.setBrush(fill);
        for (int i = 0; i < 4; ++i) p.drawEllipse(QPointF(6.0 + i * 4.0, 12), 1.0 + i * 0.9, 1.0 + i * 0.9);
        p.setBrush(Qt::NoBrush);
    } else if (k == "character") {
        QFont f = p.font();
        f.setPixelSize(17);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, QStringLiteral("A"));
        p.drawLine(QPointF(4, 20.5), QPointF(20, 20.5));
    } else if (k == "paragraph") {
        p.drawLine(QPointF(4, 6), QPointF(20, 6));
        p.drawLine(QPointF(4, 10), QPointF(20, 10));
        p.drawLine(QPointF(4, 14), QPointF(20, 14));
        p.drawLine(QPointF(4, 18), QPointF(14, 18));
    } else if (k == "actions") {
        p.setBrush(fill);
        p.drawPolygon(poly({{8, 4}, {20, 12}, {8, 20}}));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(4, 4), QPointF(4, 20));
    } else if (k == "libraries") {
        p.drawRect(QRectF(4, 4, 4, 16));
        p.drawRect(QRectF(9.5, 4, 4, 16));
        p.save();
        p.translate(18, 12);
        p.rotate(12);
        p.drawRect(QRectF(-2.2, -8, 4.4, 16));
        p.restore();
    } else if (k == "quickmask") {
        p.drawRect(QRectF(3.5, 6.5, 17, 11));
        p.setBrush(QBrush(c, Qt::Dense5Pattern));
        p.drawRect(QRectF(12, 6.5, 8.5, 11));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(9, 12), 3.2, 3.2);
    } else if (k == "screenmode") {
        p.drawRect(QRectF(3.5, 5.5, 17, 13));
        p.drawLine(QPointF(3.5, 9), QPointF(20.5, 9));
        p.setBrush(fill);
        p.drawRect(QRectF(6, 11.5, 3, 5));
        p.setBrush(Qt::NoBrush);
    } else if (k == "columns") {
        p.drawPolyline(poly({{14, 6}, {9, 12}, {14, 18}}));
        p.drawPolyline(poly({{18, 6}, {13, 12}, {18, 18}}));
    } else if (k == "edittoolbar") {
        p.drawPolygon(poly({{3, 21}, {4.5, 16.5}, {15, 6}, {19, 10}, {8.5, 20.5}}));
        p.drawLine(QPointF(4.5, 16.5), QPointF(8.5, 20.5));
    } else if (k == "grip") {
        p.setBrush(fill);
        for (int gy = 0; gy < 2; ++gy)
            for (int gx = 0; gx < 3; ++gx)
                p.drawEllipse(QPointF(8.0 + gy * 4.0, 7.0 + gx * 5.0), 0.9, 0.9);
        p.setBrush(Qt::NoBrush);
    } else if (k == "eye") {
        QPainterPath e(QPointF(2.5, 12));
        e.cubicTo(7, 5, 17, 5, 21.5, 12);
        e.cubicTo(17, 19, 7, 19, 2.5, 12);
        p.drawPath(e);
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 2.6, 2.6);
        p.setBrush(Qt::NoBrush);
    } else if (k == "mask") {
        p.drawRect(QRectF(3.5, 6.5, 17, 11));
        p.setBrush(fill);
        p.drawEllipse(QPointF(12, 12), 3.6, 3.6);
        p.setBrush(Qt::NoBrush);
    } else if (k == "fx") {
        QFont f = p.font();
        f.setPixelSize(14);
        f.setItalic(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, QStringLiteral("fx"));
    } else if (k == "smart") {
        p.drawRoundedRect(QRectF(4.5, 5.5, 15, 13), 2, 2);
        p.drawLine(QPointF(7.5, 10), QPointF(16.5, 10));
        p.drawLine(QPointF(7.5, 14.5), QPointF(16.5, 14.5));
        p.setBrush(fill);
        p.drawEllipse(QPointF(13.5, 10), 1.8, 1.8);
        p.drawEllipse(QPointF(10, 14.5), 1.8, 1.8);
        p.setBrush(Qt::NoBrush);
    } else if (k == "group") {
        p.drawPolygon(poly({{3, 19}, {3, 6}, {9, 6}, {11, 8.5}, {21, 8.5}, {21, 19}}));
    } else if (k == "newlayer") {
        p.drawRect(QRectF(4, 5, 16, 14));
        p.drawLine(QPointF(12, 8.5), QPointF(12, 15.5));
        p.drawLine(QPointF(8.5, 12), QPointF(15.5, 12));
    } else if (k == "trash") {
        p.drawPolyline(poly({{6, 7}, {7, 20}, {17, 20}, {18, 7}}));
        p.drawLine(QPointF(4, 7), QPointF(20, 7));
        p.drawPolyline(poly({{9.5, 7}, {9.5, 4.5}, {14.5, 4.5}, {14.5, 7}}));
    } else if (k == "link") {
        p.drawRoundedRect(QRectF(3, 9.5, 10, 5), 2.5, 2.5);
        p.drawRoundedRect(QRectF(11, 9.5, 10, 5), 2.5, 2.5);
    } else if (k == "lock") {
        p.drawRoundedRect(QRectF(5.5, 11, 13, 9), 1.6, 1.6);
        p.drawArc(QRectF(8, 4.5, 8, 9), 0, 180 * 16);
    } else if (k == "plus") {
        p.drawLine(QPointF(12, 6), QPointF(12, 18));
        p.drawLine(QPointF(6, 12), QPointF(18, 12));
    } else if (k == "minus") {
        p.drawLine(QPointF(6, 12), QPointF(18, 12));
    } else if (k == "close") {
        p.drawLine(QPointF(7, 7), QPointF(17, 17));
        p.drawLine(QPointF(17, 7), QPointF(7, 17));
    } else if (k == "check") {
        p.drawPolyline(poly({{5, 12.5}, {10, 18}, {19, 6.5}}));
    } else if (k == "chevron-down") {
        p.drawPolyline(poly({{6, 9.5}, {12, 15.5}, {18, 9.5}}));
    } else if (k == "chevron-right") {
        p.drawPolyline(poly({{9.5, 6}, {15.5, 12}, {9.5, 18}}));
    } else if (k == "menu") {
        p.drawLine(QPointF(5, 8), QPointF(19, 8));
        p.drawLine(QPointF(5, 12), QPointF(19, 12));
        p.drawLine(QPointF(5, 16), QPointF(19, 16));
    } else if (k == "lq-forward") {
        p.drawLine(QPointF(4, 12), QPointF(17, 12));
        p.setBrush(fill);
        p.drawPolygon(poly({{17, 8.5}, {17, 15.5}, {21.5, 12}}));
    } else if (k == "lq-reconstruct") {
        QPainterPath arc;
        arc.arcMoveTo(QRectF(5, 5, 14, 14), 40);
        arc.arcTo(QRectF(5, 5, 14, 14), 40, 250);
        p.drawPath(arc);
        p.setBrush(fill);
        p.drawPolygon(poly({{4.5, 14.5}, {9.5, 16.5}, {7.5, 10.5}}));
    } else if (k == "lq-smooth") {
        QPainterPath wave(QPointF(3, 9));
        wave.cubicTo(7, 4, 9, 14, 13, 9);
        wave.cubicTo(16, 5.5, 18, 12, 21, 9);
        p.drawPath(wave);
        p.drawLine(QPointF(3, 17), QPointF(21, 17));
    } else if (k == "lq-twirl-cw") {
        QPainterPath arc;
        arc.arcMoveTo(QRectF(4.5, 4.5, 15, 15), 90);
        arc.arcTo(QRectF(4.5, 4.5, 15, 15), 90, -270);
        p.drawPath(arc);
        p.setBrush(fill);
        p.drawPolygon(poly({{16.5, 15.5}, {20.5, 12.5}, {14.5, 11.5}}));
    } else if (k == "lq-twirl-ccw") {
        QPainterPath arc;
        arc.arcMoveTo(QRectF(4.5, 4.5, 15, 15), 90);
        arc.arcTo(QRectF(4.5, 4.5, 15, 15), 90, 270);
        p.drawPath(arc);
        p.setBrush(fill);
        p.drawPolygon(poly({{7.5, 15.5}, {3.5, 12.5}, {9.5, 11.5}}));
    } else if (k == "lq-pucker") {
        p.drawLine(QPointF(12, 4), QPointF(12, 9));
        p.drawLine(QPointF(12, 20), QPointF(12, 15));
        p.drawLine(QPointF(4, 12), QPointF(9, 12));
        p.drawLine(QPointF(20, 12), QPointF(15, 12));
        p.setBrush(fill);
        p.drawPolygon(poly({{12, 11}, {10, 8}, {14, 8}}));
        p.drawPolygon(poly({{12, 13}, {10, 16}, {14, 16}}));
        p.drawPolygon(poly({{11, 12}, {8, 10}, {8, 14}}));
        p.drawPolygon(poly({{13, 12}, {16, 10}, {16, 14}}));
    } else if (k == "lq-bloat") {
        p.drawLine(QPointF(12, 11), QPointF(12, 5));
        p.drawLine(QPointF(12, 13), QPointF(12, 19));
        p.drawLine(QPointF(11, 12), QPointF(5, 12));
        p.drawLine(QPointF(13, 12), QPointF(19, 12));
        p.setBrush(fill);
        p.drawPolygon(poly({{12, 3}, {10, 6.5}, {14, 6.5}}));
        p.drawPolygon(poly({{12, 21}, {10, 17.5}, {14, 17.5}}));
        p.drawPolygon(poly({{3, 12}, {6.5, 10}, {6.5, 14}}));
        p.drawPolygon(poly({{21, 12}, {17.5, 10}, {17.5, 14}}));
    } else if (k == "lq-push-left") {
        p.drawLine(QPointF(13, 20), QPointF(13, 5));
        p.setBrush(fill);
        p.drawPolygon(poly({{13, 2.5}, {10.5, 6.5}, {15.5, 6.5}}));
        p.drawLine(QPointF(13, 10), QPointF(6, 10));
        p.drawPolygon(poly({{4, 10}, {7, 8}, {7, 12}}));
    } else if (k == "lq-push-right") {
        p.drawLine(QPointF(11, 20), QPointF(11, 5));
        p.setBrush(fill);
        p.drawPolygon(poly({{11, 2.5}, {8.5, 6.5}, {13.5, 6.5}}));
        p.drawLine(QPointF(11, 10), QPointF(18, 10));
        p.drawPolygon(poly({{20, 10}, {17, 8}, {17, 12}}));
    } else if (k == "lq-mirror") {
        p.drawPolygon(poly({{12, 3.5}, {20, 17}, {4, 17}}));
        QPen pen2 = p.pen();
        pen2.setStyle(Qt::CustomDashLine);
        pen2.setDashPattern({2.0, 1.6});
        p.setPen(pen2);
        p.drawLine(QPointF(4, 20.5), QPointF(20, 20.5));
        p.setPen(pen);
    } else if (k == "lq-turbulence") {
        p.drawPolyline(poly({{13, 3}, {7, 11}, {11, 12}, {8, 21}}));
        p.setBrush(fill);
        p.drawPolygon(poly({{8, 21}, {6, 16}, {10.5, 17}}));
    } else if (k == "lq-clone") {
        p.drawRoundedRect(QRectF(3.5, 8.5, 10, 10), 1.5, 1.5);
        p.drawRoundedRect(QRectF(10.5, 5.5, 10, 10), 1.5, 1.5);
        p.setBrush(fill);
        p.drawPolygon(poly({{15.5, 20.5}, {13.5, 17}, {17.5, 17}}));
        p.drawLine(QPointF(15.5, 13), QPointF(15.5, 18));
    } else if (k == "lq-freeze") {
        p.drawLine(QPointF(12, 4), QPointF(12, 20));
        p.drawLine(QPointF(5, 8), QPointF(19, 16));
        p.drawLine(QPointF(19, 8), QPointF(5, 16));
    } else if (k == "lq-thaw") {
        QPainterPath drop(QPointF(12, 3.5));
        drop.cubicTo(16.5, 11, 18, 14, 18, 16.5);
        drop.cubicTo(18, 20, 15.5, 21.5, 12, 21.5);
        drop.cubicTo(8.5, 21.5, 6, 20, 6, 16.5);
        drop.cubicTo(6, 14, 7.5, 11, 12, 3.5);
        p.drawPath(drop);
    } else {
        // Placeholder so unknown keys never make an empty button.
        p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
        p.drawLine(QPointF(9, 12), QPointF(15, 12));
    }
}

// Lucide (ISC, vendored in src/ui/icons/lucide/, mapped by our key name).
// Tries :/icons/lucide/<key>.svg first so tool + chrome icons stay stylistically
// consistent and professional; falls back to the procedural glyphs below for
// niche photo tools with no Lucide equivalent and to preserve distinct flyout
// members (e.g. eraser-bg vs eraser, marquee-col vs marquee-row).
bool tryRenderLucide(QPainter& p, const QString& k, const QColor& c) {
    QFile f(QStringLiteral(":/icons/lucide/") + k + QStringLiteral(".svg"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray svg = f.readAll();
    if (svg.isEmpty() || !svg.contains("currentColor"))
        return false;
    svg.replace("currentColor", c.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return false;
    renderer.render(&p, QRectF(0, 0, 24, 24));
    return true;
}

void drawGlyph(QPainter& p, const QString& k, const QColor& c) {
    if (tryRenderLucide(p, k, c))
        return;
    drawGlyphProcedural(p, k, c);
}

}  // namespace

QPixmap glyphPixmap(const QString& key, int size, const QColor& color, qreal dpr) {
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const qreal s = size / kGrid;
    p.scale(s, s);
    drawGlyph(p, key, color);
    p.end();
    return pm;
}

QIcon toolIcon(ToolId id, const QColor& normal, const QColor& active) {
    return chromeIcon(QString::fromUtf8(toolDef(id).iconKey), normal, active);
}

QIcon chromeIcon(const QString& key, const QColor& normal, const QColor& active) {
    QIcon icon;
    // Ship 1x and 2x DPR variants so Qt picks (or downscales) correctly at
    // fractional scales like 125/150%: a single 2.0 bake blurs one notch
    // down and wastes VRAM at scale 1.
    for (int size : {16, 20, 24, 32}) {
        for (qreal dpr : {1.0, 2.0}) {
            icon.addPixmap(glyphPixmap(key, size, normal, dpr), QIcon::Normal,
                           QIcon::Off);
            icon.addPixmap(glyphPixmap(key, size, active, dpr), QIcon::Normal,
                           QIcon::On);
            icon.addPixmap(glyphPixmap(key, size, active, dpr), QIcon::Active,
                           QIcon::Off);
            QColor disabled = normal;
            disabled.setAlpha(90);
            icon.addPixmap(glyphPixmap(key, size, disabled, dpr),
                           QIcon::Disabled, QIcon::Off);
        }
    }
    return icon;
}

}  // namespace pittore::ui

// QRC objects inside static archives are dropped by the linker unless
// referenced, so force the pull with an explicit init (rcc -name icons).
struct PittoreLucideResourceInit {
    PittoreLucideResourceInit();
};
PittoreLucideResourceInit pittoreLucideResourceInit;

PittoreLucideResourceInit::PittoreLucideResourceInit() { Q_INIT_RESOURCE(icons); }
