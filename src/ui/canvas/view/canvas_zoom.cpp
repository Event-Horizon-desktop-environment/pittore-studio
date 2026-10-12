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
#include <QScreen>
#include <QWindow>
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
#include "ui/contextual_task_bar.h"
#include "ui/icons.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


// ---------------------------------------------------------------------------
// View maths
// ---------------------------------------------------------------------------
double CanvasView::zoom() const { return doc() ? doc()->zoom : 1.0; }


QSizeF CanvasView::scaledDocumentSize() const {
    DocumentItem* d = doc();
    if (!d) return {};
    return QSizeF(d->size.width() * d->zoom, d->size.height() * d->zoom);
}


QTransform CanvasView::documentTransform() const {
    DocumentItem* d = doc();
    if (!d) return {};

    const QSizeF scaled = scaledDocumentSize();
    const QSize view = viewport()->size();

    // The document is centred while it fits, and scrolled once it does not.
    const double offsetX = scaled.width() <= view.width()
                               ? (view.width() - scaled.width()) / 2.0
                               : -horizontalScrollBar()->value();
    const double offsetY = scaled.height() <= view.height()
                               ? (view.height() - scaled.height()) / 2.0
                               : -verticalScrollBar()->value();

    QTransform t;
    t.translate(offsetX + scaled.width() / 2.0, offsetY + scaled.height() / 2.0);
    t.rotate(d->rotation);
    t.translate(-scaled.width() / 2.0, -scaled.height() / 2.0);
    t.scale(d->zoom, d->zoom);
    return t;
}


QPointF CanvasView::viewToDocument(QPointF viewPoint) const {
    return documentTransform().inverted().map(viewPoint);
}


QPointF CanvasView::documentToView(QPointF documentPoint) const {
    return documentTransform().map(documentPoint);
}


void CanvasView::updateScrollRange() {
    DocumentItem* d = doc();
    if (!d) {
        horizontalScrollBar()->setRange(0, 0);
        verticalScrollBar()->setRange(0, 0);
        return;
    }
    const QSizeF scaled = scaledDocumentSize();
    const QSize view = viewport()->size();
    // Infinite zoom can scale far past int range: saturate the scrollbar
    // instead of overflowing the conversion. Panning past INT_MAX view
    // pixels is unreachable by bar, but nothing overflows.
    const auto excessRange = [](double scaledSide, int viewSide) {
        const double excess = scaledSide - viewSide;
        if (!(excess > 0.0)) return 0;
        return int(std::min(excess, 2147483647.0));  // INT_MAX
    };
    horizontalScrollBar()->setRange(0, excessRange(scaled.width(), view.width()));
    verticalScrollBar()->setRange(0, excessRange(scaled.height(), view.height()));
    horizontalScrollBar()->setPageStep(view.width());
    verticalScrollBar()->setPageStep(view.height());
    horizontalScrollBar()->setSingleStep(32);
    verticalScrollBar()->setSingleStep(32);
}


void CanvasView::setZoom(double zoom, QPointF anchor) {
    DocumentItem* d = doc();
    if (!d) return;
    // Infinite zoom: the ladder only steps the keys/tool; direct sets pass
    // through geometrically to the sanity rails. Non-positive and
    // non-finite inputs are rejected, never clamped into a surprise value.
    if (!(zoom > 0.0) || !std::isfinite(zoom)) return;
    const double z = qBound(kMinZoom, zoom, kMaxZoom);
    if (qFuzzyCompare(z, d->zoom)) return;

    const QPointF anchorPoint =
        anchor.x() < 0 ? QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0) : anchor;
    const QPointF docAnchor = viewToDocument(anchorPoint);

    d->zoom = z;
    updateScrollRange();
    // Zoom-coupled vector display is rebaked on settle, not per tick: a
    // synchronous rebake here re-rasterizes every art layer at ceil(zoom)²
    // and re-uploads hundreds of MB on each wheel step. The timer coalesces
    // the burst; frames meanwhile paint from geometry / the cached composite.
    if (!zoomSettleTimer_) {
        zoomSettleTimer_ = new QTimer(this);
        zoomSettleTimer_->setSingleShot(true);
        zoomSettleTimer_->setInterval(150);
        connect(zoomSettleTimer_, &QTimer::timeout, this, [this] {
            if (state_) state_->rezoomVectorArt();
        });
    }
    zoomSettleTimer_->start();

    // Keep the anchored document point under the cursor. Rounded (not
    // truncated) so fractional anchors at 125/150% stay under the cursor;
    // computed in double and clamped to the bar range first, because
    // qRound asserts out-of-range in this build (Q_ASSERT active) and
    // extreme zooms (1e30 → rails) make delta astronomic.
    const QPointF after = documentToView(docAnchor);
    const QPointF delta = after - anchorPoint;
    QScrollBar* hb = horizontalScrollBar();
    QScrollBar* vb = verticalScrollBar();
    const double nxd =
        std::floor(double(hb->value()) + delta.x() + 0.5);
    const double nyd =
        std::floor(double(vb->value()) + delta.y() + 0.5);
    hb->setValue(int(qBound(double(hb->minimum()), nxd,
                            double(hb->maximum()))));
    vb->setValue(int(qBound(double(vb->minimum()), nyd,
                            double(vb->maximum()))));

    emit zoomChanged(z);
    refresh();
    brushCursorRect_ = QRectF();   // ring geometry lives in view space
}


void CanvasView::zoomIn() {
    const double current = zoom();
    for (double step : zoomSteps())
        if (step > current * 1.0001) return setZoom(step);
    // Past the ladder: geometric continuation to the rails.
    setZoom(current * 2.0);
}


void CanvasView::zoomOut() {
    const double current = zoom();
    for (int i = zoomSteps().size() - 1; i >= 0; --i)
        if (zoomSteps()[i] < current * 0.9999) return setZoom(zoomSteps()[i]);
    // Past the ladder: geometric continuation to the rails.
    setZoom(current * 0.5);
}


void CanvasView::zoomToFit() {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    const QSize view = viewport()->size();
    const double fit = qMin((view.width() - 32.0) / d->size.width(),
                            (view.height() - 32.0) / d->size.height());
    setZoom(fit);  // validated inside; tiny fits pass through unclamped
}


void CanvasView::zoomToFill() {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    const QSize view = viewport()->size();
    setZoom(qMax(double(view.width()) / d->size.width(),
                 double(view.height()) / d->size.height()));
}


void CanvasView::zoomToWidth() {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    const QSize view = viewport()->size();
    const double fit = (view.width() - 16.0) / d->size.width();
    setZoom(fit);  // validated inside; tiny fits pass through unclamped
}


void CanvasView::zoomToActiveLayer() {
    const QRectF bounds = activeLayerBounds();
    if (bounds.isEmpty()) {
        zoomToFit();
        return;
    }
    const QSize view = viewport()->size();
    const double fit = qMin((view.width() - 32.0) / bounds.width(),
                            (view.height() - 32.0) / bounds.height());
    setZoom(fit);  // validated inside; tiny fits pass through unclamped
}


void CanvasView::zoomPrintSize() {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    // Print Size (R14): one image pixel shown at one physical print pixel.
    // zoom is logical (doc px -> logical view px); Qt maps logical to
    // device via DPR. device px per doc px = zoom * DPR, and we want
    // physicalDpi / docDpi, so zoom = physical / (docDpi * DPR).
    // Uses the window's own screen (not primary) so mixed-DPR multi-monitor
    // picks the screen the canvas is actually on.
    const int dpi = d->dpi > 0 ? d->dpi : 300;
    qreal physical = 0.0;
    qreal dpr = viewport() ? viewport()->devicePixelRatioF() : 1.0;
    if (QWindow* win = window() ? window()->windowHandle() : nullptr) {
        if (QScreen* scr = win->screen()) physical = scr->physicalDotsPerInch();
    }
    if (!(physical > 0.0)) {
        if (QScreen* primary = QGuiApplication::primaryScreen())
            physical = primary->physicalDotsPerInch();
    }
    if (!(physical > 0.0)) physical = 96.0;
    if (!(dpr > 0.0)) dpr = 1.0;
    setZoom(physical / (dpi * dpr));
}


void CanvasView::zoomActualPixels() {
    // 1 image px = 1 device px. device px per doc px = zoom * DPR,
    // so zoom = 1 / DPR (not 1.0, which shows 200% on a 200% display).
    qreal dpr = viewport() ? viewport()->devicePixelRatioF() : 1.0;
    if (!(dpr > 0.0)) dpr = 1.0;
    setZoom(1.0 / dpr);
}


void CanvasView::setRotation(double degrees) {
    DocumentItem* d = doc();
    if (!d) return;
    // Keep the stored angle in (-180, 180] so the options-bar field stays in
    // range however many turns the pointer makes.
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees < -180.0) degrees += 360.0;
    d->rotation = degrees;
    // "Rotate All Windows" mirrors the view rotation onto every open tab, so
    // switching documents keeps the same on-screen bearing.
    if (state_->option(ToolId::RotateView, QStringLiteral("rotate_all")).toBool()) {
        for (DocumentItem* other : state_->documents())
            if (other && other != d) other->rotation = degrees;
    }
    refresh();
    emit rotationChanged(degrees);
    brushCursorRect_ = QRectF();   // ring geometry lives in view space
    // Mirror into the Rotate View options field. setOptionSilently is a no-op
    // when the value is unchanged, so dragging does not echo back here.
    state_->setOptionSilently(ToolId::RotateView, QStringLiteral("angle"), degrees);
}


void CanvasView::resetRotation() { setRotation(0.0); }


void CanvasView::setRulersVisible(bool on) {
    rulers_ = on;
    setViewportMargins(on ? kRulerSize : 0, on ? kRulerSize : 0, 0, 0);
    horizontalRuler_->setVisible(on);
    verticalRuler_->setVisible(on);
    rulerCorner_->setVisible(on);
    layoutRulers();
    refresh();
}


void CanvasView::setGuidesVisible(bool on) { guides_ = on; refresh(); }

void CanvasView::setGridVisible(bool on) { grid_ = on; refresh(); }


void CanvasView::setSymmetryX(bool on) { symmetryX_ = on; refresh(); }


void CanvasView::setSymmetryY(bool on) { symmetryY_ = on; refresh(); }


void CanvasView::setSelectionEdgesVisible(bool on) {
    selectionEdges_ = on;
    refresh();
}


void CanvasView::setSmartGuidesVisible(bool on) {
    smartGuides_ = on;
    refresh();
}


double CanvasView::gridStep() const {
    return std::max(1.0, state_->settings().gridSpacing);
}


void CanvasView::setPixelGridVisible(bool on) {
    pixelGrid_ = on;
    refresh();
}

void CanvasView::setExtrasVisible(bool on) { extras_ = on; refresh(); }


void CanvasView::refresh() {
    viewport()->update();
    if (horizontalRuler_) horizontalRuler_->update();
    if (verticalRuler_) verticalRuler_->update();
    update();
}


void CanvasView::scrollContentsBy(int, int) { refresh(); }


void CanvasView::applyPendingFit() {
    if (!pendingFit_) return;
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    const QSize view = viewport()->size();
    if (view.width() < 64 || view.height() < 64) return;  // not laid out yet
    pendingFit_ = false;
    if (d->size.width() > view.width() - 32 || d->size.height() > view.height() - 32)
        zoomToFit();
}


void CanvasView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    applyPendingFit();
    updateScrollRange();
    layoutRulers();
    if (taskBar_) taskBar_->reflow();
}

}  // namespace pittore::ui
