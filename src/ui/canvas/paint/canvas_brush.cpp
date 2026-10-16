#include "ui/canvas_view.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QScrollBar>
#include <QDateTime>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui/ai_models.h"
#include "ui/brushes/brush_preview.h"
#include "ui/brushes/sensor_drives_json.h"
#include "ui/contextual_task_bar.h"
#include "ui/icons.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"
#include "ui/canvas/shared/symmetry.h"

namespace pittore::ui {


double CanvasView::strokeRadius() const {
    const ToolId tool = state_->activeTool();
    const QVariant v = state_->option(tool, QStringLiteral("brush_size"));
    const double size = v.isValid() ? v.toDouble() : 64.0;   // registry default
    return qMax(0.0, size * 0.5);
}


void CanvasView::brushTipShape(double& ratio, double& angleDeg,
                               bool& square) const {
    const ToolId tool = state_->activeTool();
    const QVariant av = state_->option(tool, QStringLiteral("brush_angle"));
    const QVariant rv = state_->option(tool, QStringLiteral("brush_roundness"));
    const QVariant tv = state_->option(tool, QStringLiteral("brush_tip"));
    angleDeg = av.isValid() ? qBound(-180.0, av.toDouble(), 180.0) : 0.0;
    ratio = rv.isValid() ? qBound(0.01, rv.toDouble() / 100.0, 1.0) : 1.0;
    square = tv.isValid() && tv.toInt() == 1;
}


int CanvasView::brushRotationMode(ToolId tool) const {
    const QVariant v = state_->option(tool, QStringLiteral("brush_rotation"));
    if (v.isValid()) return qBound(0, v.toInt(), 5);
    // Legacy toggle (predates the mode combo).
    const QVariant t = state_->option(tool, QStringLiteral("tilt_rotation"));
    if (t.isValid() && t.toBool()) return 1;
    return 0;
}


double CanvasView::pressureFlowMult(ToolId, double p,
                                     const sensordrive::SensorState& st) const {
    double m;
    const auto& curve = state_->strokeFlowCurve();
    if (curve.has())
        m = std::clamp(pittore::ui::brushcurve::eval(curve, p), 0.0, 1.0);
    else
        m = 1.0;
    // Sensor drives compose on top (empty = exactly 1.0).
    m *= std::max(sensordrive::driveFactor(dabOpts_.sensorDrives, "flow", st,
                                           sensordrive::brushCurveShape),
                  0.0);
    return m;
}


sensordrive::SensorState CanvasView::dabSensorState(double pr) {
    const auto& drives = dabOpts_.sensorDrives;
    double fuzzy = 0.5;
    if (!drives.empty() &&
        (sensordrive::needsFuzzyDab(drives, "scatter") ||
         sensordrive::needsFuzzyDab(drives, "size") ||
         sensordrive::needsFuzzyDab(drives, "opacity") ||
         sensordrive::needsFuzzyDab(drives, "flow")))
        fuzzy = state_->strokeRandom();
    return sensordrive::makeSensorState(
        pr, strokeMaxP_, tiltX_, tiltY_,
        std::clamp(strokeSpeed_ / 2000.0, 0.0, 1.0), strokeDist_,
        strokeElapsedMs_ / 1000.0, fuzzy, fuzzyStrokeH01_, barrelRotation_,
        tangentialPressure_, tabletDown_);
}


double CanvasView::tiltLean(double tiltX, double tiltY) {
    return std::clamp(std::hypot(tiltX, tiltY) / 60.0, 0.0, 1.0);
}


double CanvasView::tiltSizeFactor(double lean01, double amtPct) {
    const double amt = std::clamp(amtPct, 0.0, 100.0) / 100.0;
    return 1.0 + amt * std::clamp(lean01, 0.0, 1.0);
}


double CanvasView::tiltOpacityFactor(double lean01, double amtPct) {
    const double amt = std::clamp(amtPct, 0.0, 100.0) / 100.0;
    return 1.0 - amt * std::clamp(lean01, 0.0, 1.0);
}


double CanvasView::tangentialFlowFactor(double tangential01) {
    return 0.15 + 0.85 * std::clamp(tangential01, 0.0, 1.0);
}


// Dab option cache: every tool option paintDabAt needs, resolved once per
// input event (press / move / airbrush tick). Only pressure, position and
// the stylus latches vary per dab; curves live in the stroke state.
void CanvasView::refreshDabOpts() {
    const ToolId tool = state_->activeTool();
    DabToolOpts o;
    o.tool = tool;
    auto opt = [&](const char* id) {
        return state_->option(tool, QString::fromUtf8(id));
    };
    auto dbl = [&](const char* id, double fallback) {
        const QVariant v = opt(id);
        return v.isValid() ? v.toDouble() : fallback;
    };
    o.baseRadius = strokeRadius();
    o.hardness = dbl("brush_hardness", 50.0);
    if (tool == ToolId::Pencil) o.hardness = 100.0;
    brushTipShape(o.tipRatio, o.tipAngle, o.squareTip);
    o.opacity01 = qBound(0.0, dbl("opacity", 100.0) / 100.0, 1.0);
    o.flow01 = qBound(0.0, dbl("flow", 100.0) / 100.0, 1.0);
    o.wash = (tool == ToolId::Brush || tool == ToolId::Pencil) &&
             opt("brush_painting_mode").toString() !=
                 QStringLiteral("buildup");
    o.rotationMode = brushRotationMode(tool);
    o.sourceMode = qBound(0, opt("brush_source").toInt(), 3);
    o.flip = (opt("brush_flip_x").toBool() ? 1 : 0) |
             (opt("brush_flip_y").toBool() ? 2 : 0);
    o.tiltSizeAmt = qMax(0.0, dbl("brush_tilt_size", 0.0));
    o.tiltOpacityAmt = qMax(0.0, dbl("brush_tilt_opacity", 0.0));
    // Master tilt scale (100 = neutral): scales every lean-driven amount
    // so tablet users get one fader for the whole tilt response.
    {
        const double master =
            qBound(0.0, dbl("brush_tilt_master", 100.0), 100.0) / 100.0;
        o.tiltSizeAmt *= master;
        o.tiltOpacityAmt *= master;
    }
    o.tangentialOn = opt("brush_tangential_flow").toBool();
    {
        const QVariant ps = opt("pressure_size");
        o.pressureSizeOn = !ps.isValid() || ps.toBool();
        const QVariant po = opt("pressure_opacity");
        o.pressureOpacityOn = !po.isValid() || po.toBool();
    }
    o.scatterPct = qBound(0.0, dbl("brush_scatter", 0.0), 500.0);
    {
        const QVariant xv = opt("brush_scatter_x");
        o.scatterAx = (!xv.isValid() || xv.toBool()) ? 1.0 : 0.0;
        const QVariant yv = opt("brush_scatter_y");
        o.scatterAy = (!yv.isValid() || yv.toBool()) ? 1.0 : 0.0;
    }
    o.densityGate = dbl("brush_density", 100.0);
    o.hoseId = opt("brush_hose").toString();
    o.stampId = opt("brush_stamp").toString();
    o.stampMode = qBound(0, opt("brush_stamp_mode").toInt(), 3);
    o.smudge = opt("brush_engine").toString() == QStringLiteral("smudge");
    o.smudgeRate = qBound(0.0, dbl("brush_smudge_rate", 70.0), 100.0) / 100.0;
    o.smudgeRadiusFrac =
        qBound(5.0, dbl("brush_smudge_radius", 100.0), 100.0) / 100.0;
    o.smudgeMode = qBound(0, opt("smudge_mode").toInt(), 1);
    o.smudgeColorRate =
        qBound(0.0, dbl("smudge_color_rate", 0.0), 100.0) / 100.0;
    o.smudgeLength =
        qBound(0.0, dbl("smudge_length", 100.0), 200.0) / 100.0;
    o.sensorDrives = sensordrive::decodeDrives(
        opt("sensor_drives").toString());
    o.fadeLen = qMax(0.0, dbl("brush_fade", 0.0));
    o.darkenAmt = qBound(0.0, dbl("brush_darken", 0.0), 100.0);
    o.hueJitterAmt = qBound(0.0, dbl("brush_hue_jitter", 0.0), 360.0);
    o.satJitterAmt = qBound(0.0, dbl("brush_sat_jitter", 0.0), 100.0);
    o.valJitterAmt = qBound(0.0, dbl("brush_val_jitter", 0.0), 100.0);
    o.pressureIn = opt("brush_pressure_in").toBool();
    o.speedSizeAmt = qBound(0.0, dbl("brush_speed_size", 0.0), 100.0);
    o.tiltXSizeAmt = qMax(0.0, dbl("brush_tiltx_size", 0.0));
    o.tiltYSizeAmt = qMax(0.0, dbl("brush_tilty_size", 0.0));
    {
        const double master =
            qBound(0.0, dbl("brush_tilt_master", 100.0), 100.0) / 100.0;
        o.tiltXSizeAmt *= master;
        o.tiltYSizeAmt *= master;
    }
    o.timeFadeSec = qMax(0.0, dbl("brush_timefade", 0.0));
    o.fuzzySizeAmt = qBound(0.0, dbl("brush_fuzzy_size", 0.0), 100.0);
    o.fuzzyOpacityAmt = qBound(0.0, dbl("brush_fuzzy_opacity", 0.0), 100.0);
    o.perspectiveAmt = qBound(0.0, dbl("brush_perspective", 0.0), 100.0);
    o.vpX = dbl("brush_vp_x", -1.0);
    o.vpY = dbl("brush_vp_y", -1.0);
    o.texturePressure = opt("brush_texture_pressure").toBool();
    o.maskPressure = opt("brush_mask_pressure").toBool();
    o.smudgePressure = opt("brush_smudge_pressure").toBool();
    o.gradientLen = qMax(0.0, dbl("brush_gradient_len", 500.0));
    {
        const QVariant tf = opt("brush_tip_filter");
        o.tipFilter = !tf.isValid() ? 1 : qBound(0, tf.toInt(), 1);
    }
    o.eraseBlend = (tool == ToolId::Brush || tool == ToolId::Pencil) &&
                   opt("brush_erase_blend").toBool();
    dabOpts_ = o;
}


double CanvasView::fadeFactor() const {
    if (dabOpts_.fadeLen <= 0.0) return 1.0;
    return std::clamp(1.0 - strokeDist_ / dabOpts_.fadeLen, 0.0, 1.0);
}


double CanvasView::speedSizeFactor(double speed01, double amtPct) {
    const double amt = std::clamp(amtPct, 0.0, 100.0) / 100.0;
    if (amt <= 0.0) return 1.0;
    return 1.0 - amt * std::clamp(speed01, 0.0, 1.0) * 0.75;
}


double CanvasView::hash01(std::uint64_t z) {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ULL;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (z >> 11) * (1.0 / 9007199254740992.0);
}


double CanvasView::fuzzyFactor(double h01, double amtPct) {
    const double amt = std::clamp(amtPct, 0.0, 100.0) / 100.0;
    if (amt <= 0.0) return 1.0;
    return 1.0 + (std::clamp(h01, 0.0, 1.0) * 2.0 - 1.0) * amt;
}


double CanvasView::timeFadeFactorMs(quint64 elapsedMs, double lenSec) {
    if (!(lenSec > 0.0)) return 1.0;
    const double span = lenSec * 1000.0;
    if (!(span > 0.0)) return 1.0;
    return std::clamp(1.0 - double(elapsedMs) / span, 0.0, 1.0);
}


double CanvasView::timeFadeFactor() const {
    return timeFadeFactorMs(strokeElapsedMs_, dabOpts_.timeFadeSec);
}


// Perspective depth factor at a document point: 1 far from the vanishing
// point, down to (1 - amount) at the point itself. Off is exactly 1.0;
// an unset point (-1,-1) means the canvas center.
double CanvasView::perspectiveFactor(const QPointF& docPos) const {
    if (dabOpts_.perspectiveAmt <= 0.0) return 1.0;
    DocumentItem* dd = state_->activeDocument();
    if (!dd || dd->size.isEmpty()) return 1.0;
    const double vpx =
        dabOpts_.vpX >= 0.0 ? dabOpts_.vpX : dd->size.width() / 2.0;
    const double vpy =
        dabOpts_.vpY >= 0.0 ? dabOpts_.vpY : dd->size.height() / 2.0;
    const double diag = std::max(
        1.0, std::hypot(double(dd->size.width()), double(dd->size.height())));
    const double dn =
        std::clamp(std::hypot(docPos.x() - vpx, docPos.y() - vpy) / diag,
                   0.0, 1.0);
    return 1.0 -
           std::clamp(dabOpts_.perspectiveAmt, 0.0, 100.0) / 100.0 *
               (1.0 - dn);
}


// Size/opacity response shared by paintDabAt and gap-fill spacing, so the
// stride always matches the dab it lays. Same math as the retired per-dab
// helpers: authored curve when live, else built-in response. Sensor drives
// compose on top (empty = exactly 1.0).
double CanvasView::dabSizeFactor(double pr,
                                 const sensordrive::SensorState& st) const {
    double m;
    if (!dabOpts_.pressureSizeOn) {
        m = 1.0;
    } else {
        const auto& curve = state_->strokeSizeCurve();
        if (curve.has()) {
            m = std::clamp(pittore::ui::brushcurve::eval(curve, pr), 0.0,
                           1.0);
        } else {
            const double pc = std::clamp(pr, 0.0, 1.0);
            m = 0.15 + 0.85 * pc * std::sqrt(pc);
        }
    }
    if (dabOpts_.tiltSizeAmt > 0.0)
        m *= tiltSizeFactor(tiltLean(tiltX_, tiltY_), dabOpts_.tiltSizeAmt);
    // Tilt-axis split: independent X/Y lean amounts multiply on top of
    // the combined (elevation-like) response above.
    if (dabOpts_.tiltXSizeAmt > 0.0 || dabOpts_.tiltYSizeAmt > 0.0) {
        const double lx =
            std::clamp(std::fabs(tiltX_) / 60.0, 0.0, 1.0);
        const double ly =
            std::clamp(std::fabs(tiltY_) / 60.0, 0.0, 1.0);
        m *= 1.0 + std::clamp(dabOpts_.tiltXSizeAmt, 0.0, 100.0) / 100.0 *
                       lx;
        m *= 1.0 + std::clamp(dabOpts_.tiltYSizeAmt, 0.0, 100.0) / 100.0 *
                       ly;
    }
    m *= fadeFactor();
    m *= timeFadeFactor();
    if (dabOpts_.fuzzySizeAmt > 0.0)
        m *= fuzzyFactor(fuzzyStrokeH01_, dabOpts_.fuzzySizeAmt);
    if (dabOpts_.speedSizeAmt > 0.0)
        m *= speedSizeFactor(
            std::clamp(strokeSpeed_ / 2000.0, 0.0, 1.0),
            dabOpts_.speedSizeAmt);
    m *= std::max(sensordrive::driveFactor(dabOpts_.sensorDrives, "size", st,
                                           sensordrive::brushCurveShape),
                  0.0);
    return m;
}


double CanvasView::dabOpacityFactor(double pr,
                                    const sensordrive::SensorState& st) const {
    double m;
    if (!dabOpts_.pressureOpacityOn) {
        m = 1.0;
    } else {
        const auto& curve = state_->strokeOpacityCurve();
        if (curve.has()) {
            m = std::clamp(pittore::ui::brushcurve::eval(curve, pr), 0.0,
                           1.0);
        } else {
            m = std::clamp(pr, 0.0, 1.0);
        }
    }
    if (dabOpts_.tiltOpacityAmt > 0.0)
        m *= tiltOpacityFactor(tiltLean(tiltX_, tiltY_),
                               dabOpts_.tiltOpacityAmt);
    m *= fadeFactor();
    m *= timeFadeFactor();
    if (dabOpts_.fuzzyOpacityAmt > 0.0)
        m *= fuzzyFactor(fuzzyStrokeH01_, dabOpts_.fuzzyOpacityAmt);
    m *= std::max(sensordrive::driveFactor(dabOpts_.sensorDrives, "opacity",
                                           st, sensordrive::brushCurveShape),
                  0.0);
    return m;
}


void CanvasView::startAirbrush(ToolId tool) {    if (!airbrushTimer_) return;
    if (!state_->option(tool, QStringLiteral("airbrush")).toBool()) return;
    const QVariant rv = state_->option(tool, QStringLiteral("airbrush_rate"));
    const int rate = rv.isValid() ? std::clamp(rv.toInt(), 1, 100) : 20;
    airbrushTimer_->start(1000 / rate);
}


void CanvasView::airbrushTick() {
    if (!strokeActive_ || !airbrushTimer_) return;
    const ToolId tool = state_->activeTool();
    if (!isPaintTool(tool)) return;
    if (!state_->option(tool, QStringLiteral("airbrush")).toBool()) return;
    if (!cursorInViewport_) return;
    refreshDabOpts();
    paintSymmetricAt(cursorDoc_);
    state_->flushPaint();
}


bool CanvasView::cursorToolActive() const {
    if (!cursorInViewport_ || !doc()) return false;
    const ToolId tool = state_->activeTool();
    if (tool == ToolId::VectorBrushTool)
        return state_->option(tool, QStringLiteral("brush_width"))
                   .toDouble() > 0.0;
    return strokeRadius() > 0.0 &&
           state_->option(tool, QStringLiteral("brush_size")).isValid();
}


bool CanvasView::brushCursorVisible() const {
    // Outline disabled, or hidden mid-stroke by preference: no ring to draw
    // (the OS cursor shape still applies through syncCursorOverride).
    if (state_->outlineShape() == 0) return false;
    if (!state_->showOutlineWhilePainting() &&
        (strokeActive_ || vbrushActive_))
        return false;
    return cursorToolActive();
}


// Single source of truth for the ring both paintBrushCursor draws and
// brushCursorViewRect invalidates: one function, so the painted ring can
// never fall outside its dirty rect and pool into ghosts. rDoc is the
// half-width in document px; with effective sizing it is the base width,
// ignoring live pressure/velocity modulation.
void CanvasView::cursorRingGeometry(double& rDoc, double& ratio,
                                    double& angleDeg, bool& square) const {
    const ToolId tool = state_->activeTool();
    if (tool == ToolId::VectorBrushTool) {
        vbrushNib(ratio, angleDeg, square);
        double base =
            state_->option(tool, QStringLiteral("brush_width")).toDouble();
        if (!(base > 0.0)) base = 8.0;
        if (state_->outlineEffectiveSize())
            rDoc = base * 0.5;
        else
            rDoc = qMax(0.0, vbrushWidthAt(cursorDoc_) * 0.5);
        return;
    }
    brushTipShape(ratio, angleDeg, square);
    rDoc = strokeRadius();
}


QRectF CanvasView::brushCursorViewRect() const {
    const QTransform t = documentTransform();
    // Clone-source cross (issue #8): view-sized arms, unioned with the ring
    // rect so the cross can never paint outside its invalidated area. The
    // cross is tool-gated, not outline-gated: it reads even when the ring
    // is hidden mid-stroke by preference.
    QRectF cross;
    if (cloneSourceVisible()) {
        const QPointF sView = t.map(cloneSourceDoc());
        static constexpr double kHalf = 10.0;
        cross = QRectF(sView.x() - kHalf, sView.y() - kHalf, 2.0 * kHalf,
                       2.0 * kHalf);
    }
    if (!brushCursorVisible()) return cross;
    double r, ratio, angleDeg;
    bool square;
    cursorRingGeometry(r, ratio, angleDeg, square);
    // Circle outline ignores the nib shaping (diameter only).
    if (state_->outlineShape() == 1) {
        ratio = 1.0;
        square = false;
    }
    const QPointF cView = t.map(cursorDoc_);
    const QPointF eView = t.map(cursorDoc_ + QPointF(r, 0.0));
    const double rv = std::hypot(eView.x() - cView.x(), eView.y() - cView.y());
    const double rvMinor = rv * ratio;
    if (rv <= 0.5 && rvMinor <= 0.5) return cross;   // hairline brush: no ring to draw
    // Rotated-ellipse bbox in view space (+3px slack for the pen width).
    const double rad = angleDeg * 3.14159265358979323846 / 180.0;
    const double hx = std::hypot(rv * std::cos(rad), rvMinor * std::sin(rad));
    const double hy = std::hypot(rv * std::sin(rad), rvMinor * std::cos(rad));
    const QRectF ring(cView.x() - hx - 3.0, cView.y() - hy - 3.0,
                      2.0 * hx + 6.0, 2.0 * hy + 6.0);
    return cross.isEmpty() ? ring : (ring | cross);
}


bool CanvasView::cloneSourceVisible() const {
    const ToolId tool = state_->activeTool();
    if (tool != ToolId::CloneStamp && tool != ToolId::HealingBrush)
        return false;
    return cloneHasAlt_;
}


QPointF CanvasView::cloneSourceDoc() const {
    // The engine samples each dab at dab + offset, so mid-stroke the live
    // source tracks the cursor. Between strokes an aligned tool keeps its
    // offset (the hover preview is where the next dab samples); a
    // non-aligned one re-anchors to the Alt point every press, so the
    // honest hover preview is the anchor itself.
    const ToolId tool = state_->activeTool();
    const bool aligned =
        state_->option(tool, QStringLiteral("aligned")).toBool();
    if (cloneHasOffset_ && (aligned || strokeActive_))
        return cursorDoc_ + cloneOffset_;
    return cloneAltPoint_;
}


QCursor CanvasView::canvasCursor() const {
    // Outline-only blanks the OS cursor (the ring is the pointer);
    // otherwise a stock Qt cursor pairs with the ring.
    switch (state_->cursorShape()) {
        case 1: return QCursor(Qt::ArrowCursor);
        case 2: return QCursor(Qt::CrossCursor);
        default: break;
    }
    return brushHiddenCursor();
}


// The brush ring is the app's own cursor: while a brush tool
// is live over the canvas, push an application-wide cursor so no widget,
// theme, or stray setCursor call anywhere in the app can show a crosshair on
// top of the ring. The override sits above every widget cursor and is popped
// the instant the pointer leaves the canvas or the tool switches away.
void CanvasView::syncCursorOverride() {
    const bool want = cursorToolActive() &&
                      (brushCursorVisible() || state_->cursorShape() != 0);
    if (want == cursorOverrideActive_) {
        // Shape may have changed while active (menu toggle mid-hover).
        if (want)
            QApplication::changeOverrideCursor(canvasCursor());
        return;
    }
    cursorOverrideActive_ = want;
    if (want)
        QApplication::setOverrideCursor(canvasCursor());
    else
        QApplication::restoreOverrideCursor();
}


// Color Replacement palette lock (Alt+click gesture + options-bar button).
// Sampling honours the brush's own Sample Size and Tolerance options.
void CanvasView::lockReplacePalette() {
    if (state_->activeTool() != ToolId::ColorReplacement || !cursorInViewport_)
        return;
    lockReplacePaletteAt(cursorDoc_);
}


void CanvasView::lockReplacePaletteAt(const QPointF& docPos) {
    const ToolId tool = ToolId::ColorReplacement;
    const int sampleSize = std::clamp(
        state_->option(tool, QStringLiteral("sample_size")).toInt(), 0, 4);
    const double tolerance =
        std::clamp(state_->option(tool, QStringLiteral("tolerance")).toDouble() / 100.0,
                   0.0, 1.0);
    replacePaletteTargets_ =
        state_->replacePaletteAt(docPos, strokeRadius(), sampleSize,
                                 tolerance, 48);
    replacePaletteLocked_ = !replacePaletteTargets_.empty();
    if (replacePaletteLocked_) {
        state_->setStatusHint(
            tr("Replace Color: locked %1 colours — paint to replace them.")
                .arg(replacePaletteTargets_.size()));
    } else {
        state_->setStatusHint(
            tr("Replace Color: nothing visible under the brush to lock."));
    }
}


void CanvasView::paintDabAt(const QPointF& docPos) {    const ToolId tool = state_->activeTool();
    // Dab options resolve once per input event (press / move / tick); only
    // pressure, position and the stylus latches vary per dab. The mismatch
    // guard covers any path that paints without a refresh.
    if (dabOpts_.tool != tool) refreshDabOpts();
    const DabToolOpts& dopt = dabOpts_;
    // Tablet pressure: authored curves when the preset carries them, else
    // the built-in response (concave size, linear opacity). A mouse (or
    // untouched run) sits at exactly 1.0, so non-tablet behaviour is
    // bit-identical. PressureIn holds the stroke's running maximum instead
    // of the live value (monotonic inking response; a mouse maxes at 1.0
    // anyway, so the gate is unnecessary).
    const double pLive = std::clamp(pixelPressure_, 0.0, 1.0);
    const double p =
        dopt.pressureIn ? std::max(strokeMaxP_, pLive) : pLive;
    state_->setDabPressure01(p);
    // One sensor snapshot per dab, shared by every drive client below
    // (size, opacity, flow, scatter) so a dab's drives stay coherent.
    const sensordrive::SensorState sens = dabSensorState(p);
    const double sizeMult = dabSizeFactor(p, sens);
    const double opMult = dabOpacityFactor(p, sens);
    double radius = dopt.baseRadius * sizeMult;
    // Perspective depth: dabs shrink toward the vanishing point (doc
    // center when the preset leaves it unset). Off is exactly 1.0.
    radius *= perspectiveFactor(docPos);
    if (radius <= 0.0) return;

    double hardness = dopt.hardness;  // soft by default; pencil pins hard
    double tipRatio = dopt.tipRatio, tipAngle = dopt.tipAngle;
    bool squareTip = dopt.squareTip;

    // Per-dab rate: opacity caps, flow scales each dab; the two multiply.
    // Either may carry the preset's pressure curve on top. Wash strokes
    // split the two: dabs land at the flow rate and the opacity caps the
    // whole stroke at composite/bake time.
    const double flowBase = dopt.flow01 * pressureFlowMult(tool, p, sens) *
                            ((dopt.tangentialOn && tabletDown_)
                                 ? tangentialFlowFactor(tangentialPressure_)
                                 : 1.0);
    const double op = dopt.opacity01 * opMult * flowBase;
    const double dabAlpha = dopt.wash ? flowBase * opMult : op;

    // Dab color source: plain foreground, random hue per dab (foreground
    // saturation/value kept, bounded by the hue-jitter amount when set),
    // or a pressure mix toward the background. Saturation/value jitter
    // vary each dab around the foreground S/V by +/- amount (percent of
    // full scale). Darken then pulls the result toward black by pressure
    // (all three compose).
    QColor dabColor = state_->foreground();
    {
        if (dopt.sourceMode == 1) {
            QColor hsv = dabColor.toHsv();
            const int baseHue = hsv.hue() < 0 ? 0 : hsv.hue();
            int hue = baseHue;
            if (dopt.hueJitterAmt > 0.0) {
                const double jitter =
                    (state_->strokeRandom() * 2.0 - 1.0) * dopt.hueJitterAmt;
                hue = (baseHue + int(std::round(jitter))) % 360;
                if (hue < 0) hue += 360;
            } else {
                hue = int(state_->strokeRandom() * 359.0);
            }
            int sat = hsv.saturation();
            int val = hsv.value();
            if (dopt.satJitterAmt > 0.0) {
                const double span =
                    std::clamp(dopt.satJitterAmt, 0.0, 100.0) / 100.0 * 255.0;
                sat = qBound(0, sat + int(std::round(
                    (state_->strokeRandom() * 2.0 - 1.0) * span)), 255);
            }
            if (dopt.valJitterAmt > 0.0) {
                const double span =
                    std::clamp(dopt.valJitterAmt, 0.0, 100.0) / 100.0 * 255.0;
                val = qBound(0, val + int(std::round(
                    (state_->strokeRandom() * 2.0 - 1.0) * span)), 255);
            }
            hsv.setHsv(hue, sat, val);
            dabColor = hsv.toRgb();
        } else if (dopt.sourceMode == 2) {
            const QColor bg = state_->background();
            dabColor = QColor::fromRgbF(
                dabColor.redF() + (bg.redF() - dabColor.redF()) * p,
                dabColor.greenF() + (bg.greenF() - dabColor.greenF()) * p,
                dabColor.blueF() + (bg.blueF() - dabColor.blueF()) * p);
        } else if (dopt.sourceMode == 3) {
            // Stroke gradient: FG at the press point bleeding to BG over
            // the gradient length of travelled stroke.
            const QColor bg = state_->background();
            const double t = dopt.gradientLen > 0.0
                                 ? std::clamp(strokeDist_ / dopt.gradientLen,
                                              0.0, 1.0)
                                 : 0.0;
            dabColor = QColor::fromRgbF(
                dabColor.redF() + (bg.redF() - dabColor.redF()) * t,
                dabColor.greenF() + (bg.greenF() - dabColor.greenF()) * t,
                dabColor.blueF() + (bg.blueF() - dabColor.blueF()) * t);
        }
        if (dopt.darkenAmt > 0.0) {
            const double k = std::clamp(dopt.darkenAmt / 100.0, 0.0, 1.0) *
                             std::clamp(p, 0.0, 1.0);
            dabColor = QColor::fromRgbF(dabColor.redF() * (1.0 - k),
                                        dabColor.greenF() * (1.0 - k),
                                        dabColor.blueF() * (1.0 - k));
        }
    }

    // Tip mirroring for asymmetric stamps (static toggles).
    const int flip = dopt.flip;

    // Tip rotation sources (mutually exclusive). Mice sit neutral in all
    // of them, so non-tablet strokes are bit-identical with any mode on:
    // tilt reads the lean azimuth (0 upright), drawing angle aligns the
    // major axis with the stroke, pressure spins around the half-way
    // point, barrel follows the stylus rotation, fuzzy rolls per dab.
    {
        const int mode = dopt.rotationMode;
        double offset = 0.0;
        switch (mode) {
            case 1:
                offset = brushpreview::tiltRotationOffset(tiltX_, tiltY_);
                break;
            case 2: {
                // Canvas-clockwise stroke direction; tip angles run
                // counter-clockwise, hence the negation. A still dab
                // (press, airbrush tick) keeps its angle.
                const QPointF dd = docPos - strokeLastDoc_;
                if (std::hypot(dd.x(), dd.y()) > 1e-6)
                    offset = -std::atan2(dd.y(), dd.x()) * 180.0 /
                             3.141592653589793;
                break;
            }
            case 3: {
                const auto& curve = state_->strokeRotationCurve();
                const double c = curve.has()
                                     ? std::clamp(pittore::ui::brushcurve::eval(
                                                      curve, p),
                                                  0.0, 1.0)
                                     : p;
                offset = (c - 0.5) * 180.0;
                break;
            }
            case 4:
                offset = barrelRotation_;
                break;
            case 5: {
                const auto& curve = state_->strokeRotationCurve();
                const double r = state_->strokeRandom();
                const double c = curve.has()
                                     ? std::clamp(pittore::ui::brushcurve::eval(
                                                      curve, r),
                                                  0.0, 1.0)
                                     : r;
                offset = (c - 0.5) * 360.0;
                break;
            }
            default:
                break;
        }
        tipAngle += offset;
    }
    // Stroke symmetry mirrors the tip orientation with the position: each
    // single-axis mirror flips the rotation sense (set per copy by
    // paintSymmetricAt; exactly one flip negates).
    if (symFlipX_ != symFlipY_) tipAngle = -tipAngle;

    const bool isTipTool = tool == ToolId::Brush || tool == ToolId::Pencil ||
                           tool == ToolId::Eraser;
    const bool smudge = dopt.smudge;
    // The Smudge tool: full stroke machinery with the dirty-brush cores.
    // Tip resolution mirrors the tip tools (hose cycles, stamp, auto fall
    // back); strength/pressure/flow fold into the rate exactly like the
    // brush-engine smudge path so both feel identical.
    const bool smudgeTool = (tool == ToolId::Smudge);

    // Scatter + density: per-dab jitter for tip tools. Density gates the
    // dab (skipped dabs never mark the stroke painted); scatter offsets it
    // within the dab radius. Both draw from the stroke RNG, so a reseeded
    // stroke replays bit-exactly.
    QPointF dabPos = docPos;
    double strokeDirDeg = 0.0;  // canvas-clockwise drawing direction
    {
        const QPointF dd = docPos - strokeLastDoc_;
        if (std::hypot(dd.x(), dd.y()) > 1e-6)
            strokeDirDeg = std::atan2(dd.y(), dd.x()) * 180.0 / 3.141592653589793;
    }
    if (isTipTool || smudgeTool) {
        // Density lives in the dab kernels now (per-pixel noise, seeded per
        // dab); an exactly-empty brush skips the stroke up front so the
        // release still discards its undo step.
        if (dopt.densityGate <= 0.0) return;
        const double scatter = dopt.scatterPct;
        if (scatter > 0.0 && radius > 0.0) {
            // Sensor drives scale the scatter throw (empty = exactly 1.0,
            // so legacy presets are bit-identical).
            const double scatterFactor = std::max(
                sensordrive::driveFactor(dopt.sensorDrives, "scatter", sens,
                                         sensordrive::brushCurveShape),
                0.0);
            const double ax = dopt.scatterAx;
            const double ay = dopt.scatterAy;
            // Stroke-relative axes: X along the drawing direction, Y across
            // it (a still dab falls back to canvas axes).
            const double dirRad =
                strokeDirDeg * 3.141592653589793 / 180.0;
            const double ux = std::cos(dirRad), uy = std::sin(dirRad);
            const double a = state_->strokeRandom() * 2.0 * 3.141592653589793;
            const double rr = std::sqrt(state_->strokeRandom()) * scatter /
                              100.0 * radius * std::max(scatterFactor, 0.0);
            const double lx = std::cos(a) * rr * ax;
            const double ly = std::sin(a) * rr * ay;
            dabPos += QPointF(ux * lx - uy * ly, uy * lx + ux * ly);
        }
    }

    // Tip resolution: hose cell > stamp id > auto. Hoses cycle one cell per
    // dab; unknown/missing ids fall through to the auto tip so a stroke
    // never silently vanishes.
    const QString& hoseId = dopt.hoseId;
    const pittore::compute::brushload::LoadedHose* hose =
        hoseId.isEmpty() ? nullptr : state_->brushHose(hoseId);
    const QString& stampId = dopt.stampId;
    const pittore::compute::StampTip* stamp =
        stampId.isEmpty() ? nullptr : state_->brushStamp(stampId);
    const pittore::compute::StampTip* tip = nullptr;
    if (hose && (isTipTool || smudgeTool)) {
        const double dirRad =
            strokeDirDeg * 3.141592653589793 / 180.0;
        const double speed01 = std::clamp(strokeSpeed_ / 2000.0, 0.0, 1.0);
        tip = &hose->cells[pittore::compute::brushload::hose_cell_index(
                               *hose, strokeDabCount_, state_->strokeSeed(),
                               dirRad, p, speed01)]
                   .tip;
        ++strokeDabCount_;
    } else if (stamp && (isTipTool || smudgeTool)) {
        tip = stamp;
    }
    const bool eraserSmudge = tool == ToolId::Eraser && smudge;
    // Erase blend (or the Eraser tool): same branches, erase cores.
    const bool erasing = tool == ToolId::Eraser || dopt.eraseBlend;
    // Smudge-tool blend modes (schema order): the smear result is blended
    // over the untouched texel, alpha following the plain smear.
    auto smudgeBlendFor = [&]() {
        switch (state_->option(tool, QStringLiteral("mode")).toInt()) {
            case 1: return pittore::compute::BlendMode::Darken;
            case 2: return pittore::compute::BlendMode::Lighten;
            case 3: return pittore::compute::BlendMode::Hue;
            case 4: return pittore::compute::BlendMode::Saturation;
            case 5: return pittore::compute::BlendMode::Color;
            case 6: return pittore::compute::BlendMode::Luminosity;
            default: break;
        }
        return pittore::compute::BlendMode::Normal;
    };
    // Smear trail: behind the stroke direction, in doc px, scaled by the
    // pressure-shaped dab radius. Still dabs trail along -x (harmless:
    // smear needs motion to streak).
    const double smearRad =
        strokeDirDeg * 3.141592653589793 / 180.0;
    const double trailDist = dopt.smudgeLength * radius;
    const double trailX = -std::cos(smearRad) * trailDist;
    const double trailY = -std::sin(smearRad) * trailDist;
    if (tip && (isTipTool || smudgeTool) && !eraserSmudge) {
        const int smode = dopt.stampMode;
        bool ok = false;
        if (erasing) {
            ok = state_->stampTipEraseDab(dabPos, radius, op, tipAngle, *tip,
                                          flip, dopt.tipFilter);
        } else if (smudgeTool) {
            // Strength defaults to the schema value (50%) when the options
            // bar has never written it, so headless strokes behave.
            const QVariant sv =
                state_->option(tool, QStringLiteral("strength"));
            const double strength =
                qBound(0.0, sv.isValid() ? sv.toDouble() / 100.0 : 0.5, 1.0);
            const double rateEff =
                strength * (0.25 + 0.75 * std::clamp(p, 0.0, 1.0));
            ok = state_->smudgeStampDab(
                dabPos, radius, tipAngle, *tip, rateEff * op,
                dopt.smudgeRadiusFrac, flip, dopt.tipFilter,
                state_->option(tool, QStringLiteral("sample_all")).toBool(),
                smudgeBlendFor(),
                state_->option(tool, QStringLiteral("finger_paint")).toBool(),
                dopt.smudgeMode, dopt.smudgeColorRate, trailX, trailY);
        } else if (smudge) {
            const double rateEff =
                dopt.smudgeRate *
                (dopt.smudgePressure
                     ? 0.25 + 0.75 * std::clamp(p, 0.0, 1.0)
                     : 1.0);
            ok = state_->smudgeStampDab(
                dabPos, radius, tipAngle, *tip,
                rateEff * op, dopt.smudgeRadiusFrac, flip, dopt.tipFilter,
                false, pittore::compute::BlendMode::Normal, true,
                dopt.smudgeMode, dopt.smudgeColorRate, trailX, trailY);
        } else {
            ok = state_->stampTipDab(dabPos, radius, smode, dabAlpha,
                                     dabColor, tipAngle, *tip, flip,
                                     dopt.tipFilter);
        }
        if (ok) strokePainted_ = true;
        return;
    }
    if (erasing) {
        if (state_->eraseDab(dabPos, radius, hardness / 100.0, op, tipRatio,
                             tipAngle, squareTip))
            strokePainted_ = true;
        return;
    }
    if (smudge && isTipTool) {
        const double rateEff =
            dopt.smudgeRate *
            (dopt.smudgePressure ? 0.25 + 0.75 * std::clamp(p, 0.0, 1.0)
                                 : 1.0);
        if (state_->smudgeTipDab(dabPos, radius, hardness / 100.0, tipRatio,
                                 tipAngle, squareTip, rateEff * op,
                                 dopt.smudgeRadiusFrac, false,
                                 pittore::compute::BlendMode::Normal, true,
                                 dopt.smudgeMode, dopt.smudgeColorRate,
                                 trailX, trailY))
            strokePainted_ = true;
        return;
    }
    if (smudgeTool) {
        const QVariant sv =
            state_->option(tool, QStringLiteral("strength"));
        const double strength =
            qBound(0.0, sv.isValid() ? sv.toDouble() / 100.0 : 0.5, 1.0);
        const double rateEff =
            strength * (0.25 + 0.75 * std::clamp(p, 0.0, 1.0));
        if (state_->smudgeTipDab(
                dabPos, radius, hardness / 100.0, tipRatio, tipAngle,
                squareTip, rateEff * op, dopt.smudgeRadiusFrac,
                state_->option(tool, QStringLiteral("sample_all")).toBool(),
                smudgeBlendFor(),
                state_->option(tool, QStringLiteral("finger_paint")).toBool(),
                dopt.smudgeMode, dopt.smudgeColorRate, trailX, trailY))
            strokePainted_ = true;
        return;
    }

    // Dodge/Burn/Sponge: the same dab, a tonal operator instead of paint.
    if (tool == ToolId::Dodge || tool == ToolId::Burn || tool == ToolId::Sponge) {        int opKind = 0;
        int range = 1;
        bool protect = false;
        bool vibrance = false;
        double amount = 0.5;
        if (tool == ToolId::Sponge) {
            opKind = state_->option(tool, QStringLiteral("spongemode")).toInt() == 0
                         ? int(pittore::compute::ToneOp::Desaturate)
                         : int(pittore::compute::ToneOp::Saturate);
            amount = state_->option(tool, QStringLiteral("flow")).toDouble() / 100.0;
            vibrance = state_->option(tool, QStringLiteral("vibrance")).toBool();
        } else {
            opKind = tool == ToolId::Dodge
                         ? int(pittore::compute::ToneOp::Dodge)
                         : int(pittore::compute::ToneOp::Burn);
            range = state_->option(tool, QStringLiteral("range")).toInt();
            amount = state_->option(tool, QStringLiteral("exposure")).toDouble() / 100.0;
            protect = state_->option(tool, QStringLiteral("protect_tones")).toBool();
        }
        amount = qBound(0.0, amount, 1.0) * opMult;
        // Tonal tools have no opacity; the range/exposure carries the strength.
        if (state_->toneDab(docPos, radius, hardness / 100.0, amount, opKind,
                            range, protect, vibrance))
            strokePainted_ = true;
        return;
    }

    // Blur/Sharpen: each dab box-blurs its bbox and mixes by the dab mask
    // times Strength (Sharpen unsharp-mixes with the Protect Detail gate).
    // CPU runs the shared blur_dab.h core; bboxes at/above 96x96 on a GPU
    // backend take the device box_blur fastpath (see AppState).
    if (tool == ToolId::Blur || tool == ToolId::Sharpen) {
        double strength =
            state_->option(tool, QStringLiteral("strength")).toDouble() / 100.0;
        if (!(strength > 0.0)) strength = 0.5;
        strength = qBound(0.0, strength, 1.0) * opMult;
        const bool sharpen = (tool == ToolId::Sharpen);
        const bool protect =
            sharpen &&
            state_->option(tool, QStringLiteral("protect_detail")).toBool();
        if (state_->blurSharpenDab(docPos, radius, hardness / 100.0,
                                   strength, sharpen, protect))
            strokePainted_ = true;
        return;
    }

    // Background Eraser: erase where the layer matches the sampled colour
    // (tolerance + limits from the options bar). Same stroke contract as
    // paint (one undo step, committed on release).
    if (tool == ToolId::BackgroundEraser) {
        const double tolerance =
            std::clamp(state_->option(tool, QStringLiteral("tolerance"))
                           .toDouble() /
                           100.0,
                       0.0, 1.0);
        const int sampling = std::clamp(
            state_->option(tool, QStringLiteral("sampling")).toInt(), 0, 2);
        const int limits = std::clamp(
            state_->option(tool, QStringLiteral("limits")).toInt(), 0, 2);
        const bool protect = state_->option(tool, QStringLiteral("protect_fg"))
                                 .toBool();
        if (state_->backgroundEraseDab(docPos, radius, hardness / 100.0, op,
                                       tolerance, sampling, limits, protect))
            strokePainted_ = true;
        return;
    }

    // History Brush: source-over the history source under the dab mask.
    // Only Normal blend is supported (same contract as Paint Bucket).
    if (tool == ToolId::HistoryBrush) {
        if (state_->option(tool, QStringLiteral("mode")).toInt() != 0) {
            state_->setStatusHint(tr("History Brush: only Normal blend is "
                                     "supported."));
            return;
        }
        if (state_->historyBrushDab(docPos, radius, hardness / 100.0, op))
            strokePainted_ = true;
        return;
    }

    // Art History Brush: stylised history stamp (style curl + area +
    // tolerance from the options bar). Only Normal blend is supported.
    if (tool == ToolId::ArtHistoryBrush) {
        if (state_->option(tool, QStringLiteral("mode")).toInt() != 0) {
            state_->setStatusHint(tr("Art History Brush: only Normal blend "
                                     "is supported."));
            return;
        }
        const int style = std::clamp(
            state_->option(tool, QStringLiteral("style")).toInt(), 0, 7);
        const double area = std::clamp(
            state_->option(tool, QStringLiteral("area")).toDouble(), 0.0,
            500.0);
        const double tolerance =
            std::clamp(state_->option(tool, QStringLiteral("tolerance"))
                           .toDouble() /
                           100.0,
                       0.0, 1.0);
        if (state_->artHistoryDab(docPos, radius, hardness / 100.0, op,
                                  style, area, tolerance))
            strokePainted_ = true;
        return;
    }

    // Mixer Brush: wet-mix the loaded paint with the canvas sample.
    // Wet/Load/Mix/Flow from the options bar; load_after/clean_after and
    // the foreground load live in the stroke state.
    if (tool == ToolId::MixerBrush) {
        const double flow =
            std::clamp(state_->option(tool, QStringLiteral("flow")).toDouble() /
                           100.0,
                       0.0, 1.0) *
            opMult;
        const double mix =
            std::clamp(state_->option(tool, QStringLiteral("mix")).toDouble() /
                           100.0,
                       0.0, 1.0);
        const double wet =
            std::clamp(state_->option(tool, QStringLiteral("wet")).toDouble() /
                           100.0,
                       0.0, 1.0);
        const double load =
            std::clamp(state_->option(tool, QStringLiteral("load")).toDouble() /
                           100.0,
                       0.0, 1.0);
        const bool sampleAll =
            state_->option(tool, QStringLiteral("sample_all")).toBool();
        if (state_->mixerBrushDab(docPos, radius, hardness / 100.0, flow,
                                  mix, wet, load, sampleAll))
            strokePainted_ = true;
        return;
    }

    // Adjustment Brush: apply the bar's adjustment under the dab mask.
    // Only the implemented trio applies; the rest say so honestly.
    if (tool == ToolId::AdjustmentBrush) {
        const int adj = state_->option(tool, QStringLiteral("adjustment"))
                            .toInt();
        if (state_->adjustmentBrushDab(docPos, radius, hardness / 100.0, op,
                                       adj))
            strokePainted_ = true;
        return;
    }

    // Pattern Stamp: source-over the procedural tile under the dab mask.
    // Only Normal blend is supported (same contract as Paint Bucket).
    if (tool == ToolId::PatternStamp) {
        if (state_->option(tool, QStringLiteral("mode")).toInt() != 0) {
            state_->setStatusHint(tr("Pattern Stamp: only Normal blend is "
                                     "supported."));
            return;
        }
        const int pattern = std::clamp(
            state_->option(tool, QStringLiteral("pattern")).toInt(), 0, 3);
        const bool aligned =
            state_->option(tool, QStringLiteral("aligned")).toBool();
        const bool impressionist =
            state_->option(tool, QStringLiteral("impressionist")).toBool();
        if (state_->patternStampDab(docPos, radius, hardness / 100.0, op,
                                    pattern, aligned, impressionist))
            strokePainted_ = true;
        return;
    }

    // Color Replacement: recolor the sampled colour(s) with the foreground,
    // keeping each pixel's shading (the conventional colour-replacement tool, plus
    // multi-target sampling and neighbourhood harmony).
    if (tool == ToolId::ColorReplacement) {
        const int mode =
            std::clamp(state_->option(tool, QStringLiteral("mode")).toInt(), 0, 3);
        const int sampling = std::clamp(
            state_->option(tool, QStringLiteral("sampling")).toInt(), 0, 2);
        const int limits = std::clamp(
            state_->option(tool, QStringLiteral("limits")).toInt(), 0, 2);
        const double tolerance =
            std::clamp(state_->option(tool, QStringLiteral("tolerance")).toDouble() / 100.0,
                       0.0, 1.0);
        const int sampleSize = std::clamp(
            state_->option(tool, QStringLiteral("sample_size")).toInt(), 0, 4);
        const double harmony =
            std::clamp(state_->option(tool, QStringLiteral("harmony")).toDouble() / 100.0,
                       0.0, 1.0);
        const bool antialias =
            state_->option(tool, QStringLiteral("antialias")).toBool();
        std::vector<pittore::RGBAf> targets;
        if (!replacePaletteTargets_.empty() && replacePaletteLocked_) {
            // Alt+click palette lock: the explicitly picked area colours win
            // over every sampling mode until re-picked or cleared.
            targets = replacePaletteTargets_;
        } else if (sampling == 2) {
            // Background Swatch: only the background colour is ever replaced.
            const QColor bg = state_->background();
            targets.push_back(
                pittore::RGBAf{bg.redF(), bg.greenF(), bg.blueF(), 1.0f});
        } else if (sampling == 1) {
            // Once: the stroke replaces the colour set under the press point
            // for its whole length. Built on the first dab attempt (the flag
            // is cleared when the stroke begins), reused by every later dab.
            if (!replaceStrokeHasTargets_) {
                replaceStrokeTargets_ = state_->replaceTargetsAt(
                    docPos, radius, sampleSize, tolerance);
                if (replaceStrokeTargets_.empty()) return;
                replaceStrokeHasTargets_ = true;
            }
            targets = replaceStrokeTargets_;
        } else {
            // Continuous: every dab re-samples under its own footprint, so
            // the tool follows the colour as it is dragged across the image.
            targets = state_->replaceTargetsAt(docPos, radius, sampleSize,
                                               tolerance);
            if (targets.empty()) return;
        }
        if (state_->replaceColorDab(docPos, radius, hardness / 100.0, mode,
                                    targets, tolerance, limits, antialias,
                                    harmony))
            strokePainted_ = true;
        return;
    }

    // Spot Healing Brush: remove the blemish under the brush by
    // texture-replacement. Type/Diffusion/Sample All Layers come from the
    // options bar; one dab per press, gap-filled across drags, one undo
    // step per stroke like every other brush.
    if (tool == ToolId::SpotHealing) {
        const int type = std::clamp(
            state_->option(tool, QStringLiteral("type")).toInt(), 0, 2);
        const int diffusion = std::clamp(
            state_->option(tool, QStringLiteral("diffusion")).toInt(), 1, 7);
        const bool sampleAll =
            state_->option(tool, QStringLiteral("sample_all")).toBool();
        if (state_->spotHealDab(docPos, radius, hardness / 100.0, type,
                                diffusion, sampleAll))
            strokePainted_ = true;
        return;
    }

    // Clone Stamp: stamp the Alt-pinned source at the stroke offset. Opacity
    // caps the stroke, Flow sets the per-dab rate, Sample picks the buffer.
    // The offset was fixed at press (re-derived per press unless Aligned).
    if (tool == ToolId::CloneStamp) {
        if (!cloneHasAlt_ || !cloneHasOffset_) return;
        // Opacity caps the stroke; pressure drives the per-dab Flow only.
        // Scaling both would square the response (p^2) and make light
        // touches vanish.
        const double opacity =
            std::clamp(state_->option(tool, QStringLiteral("opacity")).toDouble() / 100.0,
                       0.0, 1.0);
        const double flow =
            std::clamp(state_->option(tool, QStringLiteral("flow")).toDouble() / 100.0,
                       0.0, 1.0) *
            opMult;
        const int sample = std::clamp(
            state_->option(tool, QStringLiteral("sample")).toInt(), 0, 2);
        if (state_->cloneStampDab(docPos, radius, hardness / 100.0, opacity,
                                  flow, cloneOffset_, sample))
            strokePainted_ = true;
        return;
    }

    // Healing Brush: heal the dab with the Alt-pinned donor (Sampled) or
    // the pattern (Pattern source reads the Pattern Stamp's pattern).
    // Only Normal blend is supported. The offset was fixed at press
    // (re-derived per press unless Aligned), shared with Clone Stamp.
    if (tool == ToolId::HealingBrush) {
        if (!cloneHasAlt_ || !cloneHasOffset_) return;
        if (state_->option(tool, QStringLiteral("mode")).toInt() != 0) {
            state_->setStatusHint(tr("Healing Brush: only Normal blend is "
                                     "supported."));
            return;
        }
        const bool usePattern =
            state_->option(tool, QStringLiteral("source")).toInt() == 1;
        const int sample = std::clamp(
            state_->option(tool, QStringLiteral("sample")).toInt(), 0, 2);
        const int diffusion = std::clamp(
            state_->option(tool, QStringLiteral("diffusion")).toInt(), 1, 7);
        const int patternId = std::clamp(
            state_->option(ToolId::PatternStamp, QStringLiteral("pattern"))
                .toInt(),
            0, 3);
        if (state_->healBrushDab(docPos, radius, hardness / 100.0,
                                 cloneOffset_, sample, diffusion, usePattern,
                                 patternId))
            strokePainted_ = true;
        return;
    }

    // Remove: mark the stroke for the release-time healing fill. Only
    // checked strokes (remove_after_stroke) arm the undo step; unchecked
    // strokes just accumulate marks for a later checked one.
    if (tool == ToolId::Remove) {
        const bool removeAfter =
            state_->option(tool, QStringLiteral("remove_after_stroke"))
                .toBool();
        if (state_->removeMarkDab(docPos, radius, hardness / 100.0) &&
            removeAfter)
            strokePainted_ = true;
        return;
    }

    if (state_->paintDab(dabPos, radius, hardness / 100.0, dabAlpha,
                         dabColor, tipRatio, tipAngle, squareTip))
        strokePainted_ = true;
}


void CanvasView::paintSymmetricAt(const QPointF& docPos) {
    DocumentItem* d = state_->activeDocument();
    const double cx =
        (d && d->size.width() > 0) ? d->size.width() / 2.0 : 0.0;
    const double cy =
        (d && d->size.height() > 0) ? d->size.height() / 2.0 : 0.0;
    const int n = symmetry::copyCount(symmetryX_, symmetryY_);
    if (n <= 1 || !d) {
        paintDabAt(docPos);
        return;
    }
    // Hose cells advance per dab: share one cell across the mirrors so the
    // symmetric stamps match, then step once for the next event.
    const ToolId tool = state_->activeTool();
    const bool tipPath = tool == ToolId::Brush || tool == ToolId::Pencil ||
                         tool == ToolId::Eraser || tool == ToolId::Smudge;
    const QString hoseId =
        state_->option(tool, QStringLiteral("brush_hose")).toString();
    const bool hoseActive =
        tipPath && !hoseId.isEmpty() && state_->brushHose(hoseId);
    const QPointF savedLast = strokeLastDoc_;
    const QPointF savedClone = cloneOffset_;
    const std::size_t savedHose = strokeDabCount_;
    for (int copy = 0; copy < n; ++copy) {
        const bool fx = symmetry::copyFlipX(copy);
        const bool fy = symmetry::copyFlipY(copy);
        symFlipX_ = fx;
        symFlipY_ = fy;
        strokeLastDoc_ =
            QPointF(symmetry::mirrorX(savedLast.x(), cx, fx),
                    symmetry::mirrorX(savedLast.y(), cy, fy));
        cloneOffset_ =
            QPointF(symmetry::mirrorComp(savedClone.x(), fx),
                    symmetry::mirrorComp(savedClone.y(), fy));
        if (hoseActive) strokeDabCount_ = savedHose;
        paintDabAt(QPointF(symmetry::mirrorX(docPos.x(), cx, fx),
                           symmetry::mirrorX(docPos.y(), cy, fy)));
    }
    strokeLastDoc_ = savedLast;
    cloneOffset_ = savedClone;
    symFlipX_ = symFlipY_ = false;
    if (hoseActive) strokeDabCount_ = savedHose + 1;
}

}  // namespace pittore::ui
