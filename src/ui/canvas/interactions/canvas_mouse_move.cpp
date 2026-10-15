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
#include "ui/contextual_task_bar.h"
#include "ui/icons.h"
#include "ui/persona/vector_node.h"
#include "ui/persona/vector_point_ops.h"
#include "ui/persona/vector_profile.h"
#include "ui/persona/vector_raster.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "ui/tools/log/tool_log.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/brushes/tip/tip.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"
#include "ui/canvas/trace/canvas_trace.h"

namespace pittore::ui {


QRectF CanvasView::marqueeLiveRect() const {
    const Qt::KeyboardModifiers mods = marqueeMods_;
    QPointF a = dragStartDoc_;
    QPointF b = dragCurrentDoc_;
    if (mods.testFlag(Qt::AltModifier)) {
        // Draw from the center: the press point stays the middle.
        const QPointF half = b - dragStartDoc_;
        a = dragStartDoc_ - half;
        b = dragStartDoc_ + half;
    }
    if (mods.testFlag(Qt::ShiftModifier)) {
        // Constrain proportions: square marquee, circular ellipse.
        const double w = std::abs(b.x() - a.x());
        const double h = std::abs(b.y() - a.y());
        const double m = std::max(w, h);
        b.setX(a.x() + (b.x() >= a.x() ? m : -m));
        b.setY(a.y() + (b.y() >= a.y() ? m : -m));
    }
    return QRectF(a, b).normalized();
}


void CanvasView::mouseMoveEvent(QMouseEvent* event) {
    // A move implies the pointer is over the canvas (an Enter always precedes
    // real hover, but synthetic events and drags into the window can skip it).
    cursorInViewport_ = true;
    // Keep the canvas cursor in sync no matter which path ran before: the
    // configured cursor shape (or the blank one when the ring is the only
    // cursor) can never sit wrong on top of the ring (self-heals any
    // transient setCursor call).
    if (cursorToolActive())
        viewport()->setCursor(canvasCursor());
    syncCursorOverride();
    cursorDoc_ = viewToDocument(event->position());
    emit cursorMoved(cursorDoc_);

    // Alt+Right-drag brush resize: horizontal sets size, vertical sets
    // hardness (up = harder). Fine-grain rates so small drags land exactly;
    // the canvas HUD reads the same live options back. The button guard
    // self-heals a missed release: any move without the right button held
    // ends the gesture instead of resizing, so the brush can never keep
    // growing after release.
    if (brushResizeActive_) {
        if (!(event->buttons() & Qt::RightButton)) {
            brushResizeActive_ = false;
            state_->setStatusHint(QString());
            viewport()->update();
            event->accept();
            return;
        }
        const ToolId tool = state_->activeTool();
        const double dx = event->position().x() - brushResizeStartView_.x();
        const double dy = event->position().y() - brushResizeStartView_.y();
        const double size = qBound(1.0, brushResizeStartSize_ + dx * 0.25, 5000.0);
        const double hard = qBound(0.0, brushResizeStartHardness_ - dy * 0.15, 100.0);
        state_->setOption(tool, QStringLiteral("brush_size"), size);
        state_->setOption(tool, QStringLiteral("brush_hardness"), hard);
        state_->setStatusHint(
            tr("Brush: %1 px, %2% hard").arg(qRound(size)).arg(qRound(hard)));
        viewport()->update();
        event->accept();
        return;
    }

    // Scrubby zoom: horizontal scrub around the press anchor. Same
    // self-heal as the brush resize: a move without the left button ends a
    // stranded scrub instead of zooming forever.
    if (zoomScrubbing_) {
        if (!(event->buttons() & Qt::LeftButton)) {
            zoomScrubbing_ = false;
            dragging_ = false;
            viewport()->update();
            event->accept();
            return;
        }
        const double dx = event->position().x() - zoomScrubLastView_.x();
        zoomScrubLastView_ = event->position();
        if (std::abs(event->position().x() - zoomScrubAnchorView_.x()) > 3.0)
            zoomScrubMoved_ = true;
        if (dx != 0.0) {
            // Right = in, left = out, anchored at the press point.
            setZoom(zoom() * std::pow(1.0025, dx), zoomScrubAnchorView_);
        }
        dragCurrentDoc_ = cursorDoc_;
        event->accept();
        return;
    }

    // The brush-cursor ring tracks the pointer: repaint only the old ring
    // strip ∪ the new one. This must run during a stroke too — the dab flush
    // repaints just the dab footprints (+ one-texel halo), and its first dab
    // starts a spacing step (~0.3·radius) ahead of the old cursor, so the
    // trailing arc of the previous ring falls outside the flush rect and would
    // pool into a ghost behind the brush. Repainting old ∪ new keeps the ring
    // exact on every move; the updates coalesce with the flush's repaint.
    {
        const QRectF next = brushCursorVisible() ? brushCursorViewRect() : QRectF();
        const QRectF dirty =
            brushCursorRect_.isEmpty() ? next : (brushCursorRect_ | next);
        brushCursorRect_ = next;
        if (!dirty.isEmpty())
            viewport()->update(dirty.adjusted(-1, -1, 1, 1).toAlignedRect());
    }

    // Object Selection hover preview: with the AI pair model + Object Finder,
    // decode at the pointer and show what the click would select as a blue
    // overlay (no click needed, it refines as you move).
    // Throttled by both movement and time: the decoder is ~50–150 ms, so only
    // the latest hover position per gesture matters.
    {
        const ToolId overTool = state_->activeTool();
        const bool want =
            !dragging_ && overTool == ToolId::ObjectSelection &&
            state_->option(overTool, QStringLiteral("object_finder")).toBool() &&
            cursorInViewport_;
        if (want) {
            const QPoint p = cursorDoc_.toPoint();
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (p != hoverPreviewPoint_ && now - hoverPreviewLastMs_ >= 90) {
                hoverPreviewPoint_ = p;
                hoverPreviewLastMs_ = now;
                QImage m;
                float iou = 0.0f;
                QString status;
                // Live preview: single decode (fast) so the overlay keeps up
                // with the pointer; the settle timer upgrades it to the
                // expanded union mask once the cursor rests.
                hoverPreviewMask_ = objectSelectMaskAt(p, m, iou, status, false)
                                       ? m
                                       : QImage();
                hoverPreviewActive_ = !hoverPreviewMask_.isNull();
                hoverPreviewExpanded_ = false;
                hoverSettleTimer_->start();
                viewport()->update();
            }
        } else if (hoverPreviewActive_) {
            hoverPreviewActive_ = false;
            hoverPreviewMask_ = QImage();
            hoverPreviewPoint_ = QPoint();
            hoverPreviewExpanded_ = false;
            hoverSettleTimer_->stop();
            viewport()->update();
        }
    }

    if (rotating_) {
        if (doc()) {
            const QPointF center(viewport()->width() / 2.0,
                                 viewport()->height() / 2.0);
            const double cur =
                std::atan2(event->position().y() - center.y(),
                           event->position().x() - center.x()) *
                180.0 / 3.14159265358979323846;
            double deg = rotateStartRotation_ + (cur - rotateStartPointerAngle_);
            if (event->modifiers().testFlag(Qt::ShiftModifier))
                deg = std::round(deg / 15.0) * 15.0;
            setRotation(deg);
        }
        return;
    }

    if (sliceDragging_) {
        sliceDragCurrentDoc_ = cursorDoc_;
        viewport()->update();
        return;
    }

    if (sliceSelectDragging_) {
        if (DocumentItem* d = doc(); d && sliceSelectIndex_ >= 0 &&
            sliceSelectIndex_ < d->slices.size()) {
            const QPointF delta = cursorDoc_ - sliceSelectGrabDoc_;
            const QPointF topLeft = sliceSelectStartOffset_ + delta;
            d->slices[sliceSelectIndex_].moveTo(qRound(topLeft.x()), qRound(topLeft.y()));
            if (delta.manhattanLength() > 0.5) sliceSelectMoved_ = true;
            viewport()->update();
        }
        return;
    }

    if (rulerDragging_) {
        if (DocumentItem* d = doc()) {
            QPointF end = cursorDoc_;
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                const double dx = end.x() - d->rulerStart.x();
                const double dy = end.y() - d->rulerStart.y();
                const double len = std::hypot(dx, dy);
                const double step = 3.14159265358979323846 / 4.0;
                const double ang = std::round(std::atan2(dy, dx) / step) * step;
                end = d->rulerStart + QPointF(std::cos(ang) * len, std::sin(ang) * len);
            }
            d->rulerEnd = end;
            if (state_->activeTool() == ToolId::MeasureTool) updateMeasureHint();
            viewport()->update();
        }
        return;
    }

    // Area: the measured rectangle follows the pointer from its anchor.
    if (areaDragging_) {
        if (DocumentItem* d = doc()) {
            d->areaRect = QRectF(areaAnchorDoc_, cursorDoc_).normalized();
            updateAreaHint();
            viewport()->update();
        }
        return;
    }

    // Red Eye: the pupil box follows the pointer from its anchor.
    if (redeyeDragging_) {
        dragStartDoc_ = redeyeAnchorDoc_;
        dragCurrentDoc_ = cursorDoc_;
        dragging_ = true;
        viewport()->update();
        return;
    }

    // Node: drag the grabbed endpoint (or handle) through node space; the
    // overlay previews the working copy (no re-raster until release commits
    // one step). Alt held mid-drag breaks handle mirroring for that move.
    if (nodeDragging_) {
        if (DocumentItem* d = doc()) {
            const LayerItem* l =
                (nodeLayer_ >= 0 && nodeLayer_ < d->layers.size())
                    ? &d->layers[nodeLayer_]
                    : nullptr;
            if (l && l->art && l->scaleX > 0.0 && l->scaleY > 0.0) {
                const QPointF src(
                    (cursorDoc_.x() - l->offset.x()) / l->scaleX,
                    (cursorDoc_.y() - l->offset.y()) / l->scaleY);
                const QTransform inv =
                    QTransform(l->art->matrix[0], l->art->matrix[1],
                               l->art->matrix[2], l->art->matrix[3],
                               l->art->matrix[4], l->art->matrix[5])
                        .inverted();
                if (nodeHandleSeg_ >= 0) {
                    const bool alt = QApplication::keyboardModifiers()
                                         .testFlag(Qt::AltModifier);
                    moveNodeHandle(nodeWork_, nodeHandleSeg_,
                                   nodeHandleSide_ == 1
                                       ? NodeHandleSide::Out
                                       : NodeHandleSide::In,
                                   inv.map(src) + nodeHandleGrabDelta_,
                                   nodeHandleMirror_ && !alt);
                } else {
                    moveNodePoint(nodeWork_, nodeSeg_,
                                  inv.map(src) + nodeGrabDelta_);
                }
                nodeMoved_ = true;
                viewport()->update();
            }
        }
        return;
    }

    // Shape Builder: track the marquee; the overlay previews it.
    if (builderActive_ && state_->activeTool() == ToolId::ShapeBuilderTool) {
        builderCurDoc_ = cursorDoc_;
        viewport()->update();
        return;
    }
    // Knife: track the cut line; the overlay previews it.
    if (knifeActive_ && state_->activeTool() == ToolId::KnifeTool) {
        knifeCurDoc_ = cursorDoc_;
        viewport()->update();
        return;
    }
    // Stroke Width: drag shapes the profile at the cursor (Shift scales the
    // base width uniformly). Recomputed from the press-time paint every
    // move, so there is no drift across moves.
    if (swActive_ && state_->activeTool() == ToolId::StrokeWidthTool) {
        if (DocumentItem* d = doc()) {
            const double dxView =
                (cursorDoc_.x() - swPressDoc_.x()) * qMax(0.01, d->zoom);
            const double factor = 1.0 + dxView / 200.0;
            if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
                swPreviewPaint_ = swStartPaint_;
                swPreviewPaint_.strokeWidth =
                    qMax(0.1, swStartWidth_ * factor);
                swLiveWidth_ = swPreviewPaint_.strokeWidth;
            } else {
                QPointF nodePos;
                if (docToArtNode(swLayer_, cursorDoc_, &nodePos) &&
                    !swFlat_.subpaths.empty()) {
                    // Nearest flattened point (node space) and its t.
                    double bestD = 1e300, bestT = 0.0;
                    for (std::size_t si = 0;
                         si < swFlat_.subpaths.size() && si < swTotals_.size();
                         ++si) {
                        const auto& sub = swFlat_.subpaths[si];
                        const double total = swTotals_[si];
                        double run = 0.0;
                        for (std::size_t k = 0; k < sub.size(); ++k) {
                            if (k > 0)
                                run += std::hypot(sub[k].first - sub[k - 1].first,
                                                  sub[k].second - sub[k - 1].second);
                            const double dd = std::hypot(
                                sub[k].first - nodePos.x(),
                                sub[k].second - nodePos.y());
                            if (dd < bestD) {
                                bestD = dd;
                                bestT = (total > 1e-9) ? run / total : 0.0;
                            }
                        }
                    }
                    swPreviewPaint_ = swStartPaint_;
                    const double base =
                        std::max(0.01, swStartPaint_.strokeWidth) *
                        pittore::vector::widthProfileAt(
                            widthProfileFrom(swStartPaint_),
                            static_cast<float>(bestT));
                    const double target = qBound(0.0, base * factor, 4096.0);
                    setProfilePoint(swPreviewPaint_, static_cast<float>(bestT),
                                    static_cast<float>(
                                        target / std::max(0.01, swStartPaint_.strokeWidth)));
                    swLiveWidth_ = target;
                }
            }
            swHasPreview_ = true;
            viewport()->update();
        }
        return;
    }
    // Point Transform: move the anchor, or scale/rotate everything about
    // the centroid from the press-time original (no drift across moves).
    if (ptDragging_ && state_->activeTool() == ToolId::PointTransformTool) {
        if (DocumentItem* d = doc()) {
            const double dxView =
                (cursorDoc_.x() - ptPressDoc_.x()) * qMax(0.01, d->zoom);
            if (ptMode_ == 0) {
                QPointF nodePos;
                if (docToArtNode(ptLayer_, cursorDoc_, &nodePos)) {
                    // Re-anchor the delta to the press point: the working
                    // copy always edits from the original geometry.
                    ptWork_ = ptOrig_;
                    moveNodePoint(ptWork_, ptSeg_, nodePos + ptGrabDelta_);
                    ptMoved_ = true;
                    viewport()->update();
                }
            } else if (ptMode_ == 1) {
                ptWork_ = ptOrig_;
                transformNodePoints(ptWork_, ptCentre_, qMax(0.01, 1.0 + dxView / 200.0), 0.0);
                ptMoved_ = true;
                viewport()->update();
            } else {
                ptWork_ = ptOrig_;
                transformNodePoints(ptWork_, ptCentre_, 1.0, dxView / 2.0);
                ptMoved_ = true;
                viewport()->update();
            }
        }
        return;
    }
    // Gradient / Transparency: track the live axis; the overlay previews it.
    if (vgradActive_ &&
        (state_->activeTool() == ToolId::Gradient ||
         state_->activeTool() == ToolId::TransparencyTool)) {
        vgradCurDoc_ = cursorDoc_;
        viewport()->update();
        return;
    }
    // Vector drag tools: stream the gesture; release commits.
    if (inkActive_ && (event->buttons() & Qt::LeftButton)) {
        inkMove(cursorDoc_);
        return;
    }
    // Vector Brush: stream dabs while the button is held; the overlay
    // previews the working ribbon, release commits it.
    if (vbrushActive_ && state_->activeTool() == ToolId::VectorBrushTool &&
        (event->buttons() & Qt::LeftButton)) {
        vbrushStroke_.addDab(cursorDoc_, vbrushWidthAt(cursorDoc_));
        viewport()->update();
        return;
    }
    // Pen: rubber-band cursor tracking, live handle shaping on press-drag,
    // Freehand streaming. The overlay previews; nothing commits until
    // release (Freehand) or finish (Pen/Curvature). Hover alone updates the
    // rubber band but lets the rest of hover handling run.
    if (penActive_ && isPenTool(state_->activeTool())) {
        penHoverDoc_ = cursorDoc_;
        if (!penPressed_) {
            viewport()->update();
        } else {
            const ToolId tool = state_->activeTool();
            if (tool == ToolId::FreeformPen) {
                double fit = state_->option(tool, QStringLiteral("curvefit")).toDouble();
                if (!(fit > 0.0)) fit = 2.0;
                QPointF at = cursorDoc_;
                if (state_->option(tool, QStringLiteral("magnetic")).toBool()) {
                    if (DocumentItem* d = doc()) {
                        QPointF snapped;
                        const double tol = 8.0 / qMax(0.01, d->zoom);
                        if (snapPenToArt(cursorDoc_, tol, &snapped)) at = snapped;
                    }
                }
                penPath_.addFreehand(at, qMax(1.0, fit * 0.5));
            } else if (tool == ToolId::Pen) {
                if (DocumentItem* d = doc()) {
                    const QPointF vec = cursorDoc_ - penPressDoc_;
                    const double viewLen =
                        std::hypot(vec.x(), vec.y()) * qMax(0.01, d->zoom);
                    if (viewLen > 3.0) {
                        if (!penShaping_) {
                            penPath_.addSmooth(penPressDoc_, vec);
                            penShaping_ = true;
                        } else {
                            penPath_.setLastHandles(penPressDoc_, vec);
                        }
                    }
                }
            }
            viewport()->update();
            return;
        }
    }

    if (countDragging_) {
        if (DocumentItem* d = doc(); d && countDragIndex_ >= 0 &&
            countDragIndex_ < d->countMarkers.size()) {
            d->countMarkers[countDragIndex_].pos = cursorDoc_ - countDragGrab_;
            countDragged_ = true;
            viewport()->update();
        }
        return;
    }

    if (noteDragging_) {
        if (DocumentItem* d = doc(); d && noteDragIndex_ >= 0 &&
            noteDragIndex_ < d->notes.size()) {
            d->notes[noteDragIndex_].pos = cursorDoc_ - noteDragGrab_;
            if ((d->notes[noteDragIndex_].pos - noteDragOrig_).manhattanLength() > 0.5)
                noteDragged_ = true;
            viewport()->update();
        }
        return;
    }

    if (state_->activeTool() == ToolId::Note) {
        if (DocumentItem* d = doc()) {
            const int hit = noteAtView(event->position());
            if (hit != noteHover_) {
                noteHover_ = hit;
                viewport()->setToolTip(hit >= 0 ? d->notes[hit].text : QString());
                viewport()->update();
            }
        }
    }

    if (textSelecting_) {
        setTextCaret(textOffsetAtDoc(cursorDoc_), /*extend=*/true);
        return;
    }

    if (textCreating_) {
        dragCurrentDoc_ = cursorDoc_;
        const double dist = std::hypot(cursorDoc_.x() - textCreateStartDoc_.x(),
                                       cursorDoc_.y() - textCreateStartDoc_.y());
        textCreateSize_ = dist;
        textCreateFrame_ = event->modifiers().testFlag(Qt::ShiftModifier);
        viewport()->update();
        return;
    }

    if (panning_) {
        const QPointF delta = event->position() - panStartView_;
        horizontalScrollBar()->setValue(panStartScroll_.x() - qRound(delta.x()));
        verticalScrollBar()->setValue(panStartScroll_.y() - qRound(delta.y()));
        return;  // scrollContentsBy refreshes the viewport
    }
    // Polygonal rubber band tracks the cursor between clicks (no buttons).
    if (!dragging_ && state_->activeTool() == ToolId::PolygonalLasso &&
        !polyPts_.empty()) {
        polyHover_ = cursorDoc_;
        polyHoverOn_ = true;
        viewport()->update();
        return;
    }
    if (dragging_) {
        // Freehand lasso: stream points (magnetic snaps to edges).
        {
            const ToolId lt = state_->activeTool();
            if (lassoLive_ &&
                (lt == ToolId::Lasso || lt == ToolId::MagneticLasso)) {
                QPointF at = cursorDoc_;
                if (lt == ToolId::MagneticLasso) at = magneticSnap(at);
                if (lassoStroke_.empty() ||
                    QLineF(lassoStroke_.back(), at).length() >= 1.5) {
                    lassoStroke_.push_back(at);
                }
                dragCurrentDoc_ = cursorDoc_;
                marqueeMods_ = event->modifiers();
                viewport()->update();
                return;
            }
            // Selection brush: paint the incoming mask along the drag.
            if (selBrushLive_ && lt == ToolId::SelectionBrush) {
                const double radius =
                    qMax(0.5, state_->option(lt, QStringLiteral("brush_size"))
                                  .toDouble() *
                              0.5);
                const double hardness =
                    state_->option(lt, QStringLiteral("hardness")).toDouble() /
                    100.0;
                const double opacity =
                    state_->option(lt, QStringLiteral("opacity")).toDouble() /
                    100.0;
                selBrushPaintTo(cursorDoc_, radius,
                                hardness >= 0.0 ? hardness : 1.0,
                                opacity > 0.0 ? opacity : 1.0);
                dragCurrentDoc_ = cursorDoc_;
                marqueeMods_ = event->modifiers();
                viewport()->update();
                return;
            }
        }
        // Patch donor drag: the rubber band is the committed selection
        // translated by the press→cursor delta (modifiers ignored so the
        // outline tracks the pointer 1:1, like a conventional patch preview).
        if (state_->activeTool() == ToolId::Patch && patchPhase_ == 2) {
            if (DocumentItem* dd = doc()) {
                const QPointF delta = cursorDoc_ - patchPressDoc_;
                dragStartDoc_ = dd->selection.topLeft() + delta;
                dragCurrentDoc_ = dd->selection.bottomRight() + delta;
                marqueeMods_ = Qt::NoModifier;
                if (delta.manhattanLength() > 0.5) patchMoved_ = true;
                viewport()->update();
            }
            return;
        }
        // ContentAwareMove content drag: same translated-outline rubber.
        if (state_->activeTool() == ToolId::ContentAwareMove && camPhase_ == 2) {
            if (DocumentItem* dd = doc()) {
                const QPointF delta = cursorDoc_ - camPressDoc_;
                dragStartDoc_ = dd->selection.topLeft() + delta;
                dragCurrentDoc_ = dd->selection.bottomRight() + delta;
                marqueeMods_ = Qt::NoModifier;
                if (delta.manhattanLength() > 0.5) camMoved_ = true;
                viewport()->update();
            }
            return;
        }
        // Crop: the frame tracks the gesture (rubber band, handle resize or
        // reposition) and stays live after release for fine tuning.
        if (state_->activeTool() == ToolId::Crop) {
            cropDragTo(cursorDoc_);
            viewport()->update();
            return;
        }
        // Perspective Crop corner drag: the quad follows the pointer.
        if (state_->activeTool() == ToolId::PerspectiveCrop &&
            pcropCorner_ >= 0) {
            pcropQuad_[pcropCorner_] = cursorDoc_;
            dragCurrentDoc_ = cursorDoc_;
            viewport()->update();
            return;
        }
        dragCurrentDoc_ = cursorDoc_;
        // The marquee modifiers ride the event stream: press seeds them, each
        // move refreshes them, release and the overlay read them back.
        marqueeMods_ = event->modifiers();
        // Shape tools: Shift constrains the drag to equal proportions
        // (square box — circle, square, proportional star), anchored at the
        // press corner like standard editors. Free aspect otherwise. Alt
        // draws from the centre: the cursor defines the half-extent, so the
        // press point stays the middle of the box. The two combine.
        if (isShapeTool(state_->activeTool())) {
            // Shift squares the corner about the press point; Alt mirrors
            // both ends about it so the press stays the middle. Absolute
            // recompute every event (never incremental) so toggling keys
            // mid-drag cannot walk the box.
            QPointF corner = cursorDoc_;
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                const QPointF delta = corner - shapeAnchorDoc_;
                const double m = qMax(std::abs(delta.x()), std::abs(delta.y()));
                corner = shapeAnchorDoc_ +
                         QPointF(delta.x() < 0.0 ? -m : m, delta.y() < 0.0 ? -m : m);
            }
            if (event->modifiers().testFlag(Qt::AltModifier)) {
                const QPointF half = corner - shapeAnchorDoc_;
                dragStartDoc_ = shapeAnchorDoc_ - half;
                dragCurrentDoc_ = shapeAnchorDoc_ + half;
            } else {
                dragStartDoc_ = shapeAnchorDoc_;
                dragCurrentDoc_ = corner;
            }
        }
        if (scaleDragging_) {
            updateScaleDrag(
                cursorDoc_,
                event->modifiers().testFlag(Qt::ShiftModifier));
        } else if (moveDragging_) {
            if (const LayerItem* l = state_->activeLayer()) {
                const QPointF raw = cursorDoc_ - moveGrabOffset_;
                // The footprint the snap measures against, also logged: origin
                // plus size is the only way a trace can place every edge, so an
                // off-canvas edge can be attributed to the snap or the cursor
                // instead of guessed at. Identical to what the snap calls use.
                const QSizeF footprint =
                    (l->kind == LayerItem::Kind::Group)
                        ? state_->groupBounds(doc()->activeLayer).size()
                        : (l->pixels ? QSizeF(l->pixels->width() * l->scaleX,
                                              l->pixels->height() * l->scaleY)
                                     : QSizeF());
                // Snap the anchor layer's edges/centers to whatever targets are
                // active (guides, grid, document bounds). Only interactive moves
                // snap; programmatic placement stays exact.
                moveSnap_ = (l->kind == LayerItem::Kind::Group)
                                ? snappedMoveSize(raw, footprint, moveLayerSet_)
                                : snappedMoveOffset(raw, *l, moveLayerSet_);
                const QPointF snapped = moveSnap_.offset;
                // One delta applied to every moved pixel layer: the anchor's
                // press offset is the baseline the snap is measured against.
                const QPointF delta = snapped - moveAnchorStartOffset_;
                const QPointF before = l->offset;
                if (state_->moveLayersAt(moveLayerSet_, delta, moveLayerStarts_))
                    moveChanged_ = true;
                // The mover places the anchor at exactly the snapped position
                // (start + delta), so any divergence is the mover itself — most
                // likely a stale/short starts[] baseline silently degrading the
                // gesture to per-event accumulation, which walks the layer off
                // the canvas while the cursor stays put. Flagged loudly: a jump
                // has to be attributable from the log alone.
                const QPointF actual =
                    (l->kind == LayerItem::Kind::Group)
                        ? state_->groupBounds(doc()->activeLayer).topLeft()
                        : l->offset;
                if (std::abs(actual.x() - snapped.x()) > 0.5 ||
                    std::abs(actual.y() - snapped.y()) > 0.5)
                    tool_logf(state_->activeTool(), "move/JUMP",
                              "actual=(%.1f,%.1f) snap=(%.1f,%.1f) "
                              "raw=(%.1f,%.1f) size=(%.1f,%.1f) set=%d starts=%d",
                              actual.x(), actual.y(), snapped.x(), snapped.y(),
                              raw.x(), raw.y(), footprint.width(),
                              footprint.height(), int(moveLayerSet_.size()),
                              int(moveLayerStarts_.size()));
                // One line per drag step so a random off-screen jump can be
                // traced to whichever value spiked:
                //   view  the event's position in viewport pixels
                //   glob  the same event's physical desktop position, and vg
                //         the viewport's own desktop origin; view must equal
                //         glob - vg, so if glob sweeps calmly while view
                //         lurches the local coordinate (widget origin / event
                //         source) is wrong, not the pointer
                //   cur   view mapped into the document; view steady while cur
                //         jumps means documentTransform moved under the drag
                //         (scroll/resize), not the pointer
                //   raw   cur minus the press-time grab offset
                //   snap  raw after snapping (the anchor's target position)
                //   delta snap minus the anchor's press offset
                //   hit   which axes snapped; feat names the layer feature that
                //         won: L/C/R left-centre-right, T/M/B top-middle-bottom
                //         (so tgt can be checked against the right edge)
                //   size  the snapped footprint; origin + size places every edge
                //   off   the anchor layer's real offset before -> after the
                //         mover ran (must track snap; a divergence here is the
                //         mover/starts, not the cursor)
                //   rev   composite revision once the mover's renderRegion ran;
                //         match it against canvas/frame's rev to find the frame
                //         this step is waiting for
                //   pnt   painted-frame count at the moment of the step: no
                //         frame advancing past it before the next row is a
                //         repaint that never came, and the screen is stale
                //   ets   Qt's own QInputEvent::timestamp() for the event, so
                //         two steps sharing a wall-clock millisecond can be
                //         told apart: equal ets means one Qt event was
                //         dispatched twice, differing ets means two events
                //   qc    where the OS reports the cursor right now, sampled
                //         independently of the event. qc tracking view rules
                //         out our dispatch; qc steady while view teleports
                //         proves the event itself carries a wrong position
                const QPoint vg = viewport()->mapToGlobal(QPoint(0, 0));
                const QPointF qc = viewport()->mapFromGlobal(QCursor::pos());
                const auto featChar = [](int i, const char* codes) {
                    return (i >= 0 && i <= 2) ? codes[i] : '-';
                };
                tool_logf(state_->activeTool(), "move/drag",
                          "view=(%.1f,%.1f) glob=(%.1f,%.1f) vg=(%d,%d) "
                          "qc=(%.1f,%.1f) "
                          "cur=(%.1f,%.1f) raw=(%.1f,%.1f) "
                          "snap=(%.1f,%.1f) delta=(%.1f,%.1f) hit=%c%c "
                          "feat=%c%c tgt=(%.1f,%.1f) size=(%.1f,%.1f) "
                          "off=(%.1f,%.1f)->(%.1f,%.1f) "
                          "zoom=%.3f rev=%llu pnt=%lld ets=%llu",
                          event->position().x(), event->position().y(),
                          event->globalPosition().x(),
                          event->globalPosition().y(), vg.x(), vg.y(),
                          qc.x(), qc.y(),
                          cursorDoc_.x(), cursorDoc_.y(), raw.x(), raw.y(),
                          snapped.x(), snapped.y(), delta.x(), delta.y(),
                          moveSnap_.x ? 'X' : '-', moveSnap_.y ? 'Y' : '-',
                          featChar(moveSnap_.xFeat, "LCR"),
                          featChar(moveSnap_.yFeat, "TMB"),
                          moveSnap_.xPos, moveSnap_.yPos, footprint.width(),
                          footprint.height(), before.x(),
                          before.y(), l->offset.x(), l->offset.y(), zoom(),
                          static_cast<unsigned long long>(
                              doc() ? doc()->revision : 0),
                          static_cast<long long>(canvasFrameCount()),
                          static_cast<unsigned long long>(event->timestamp()));
                // The smart guides span the whole document, so the drag repaints
                // the viewport rather than just the moved layer's strip.
                if (state_->snapEnabled() && state_->snapTargets()) {
                    canvasNoteUpdate("snap-guides");
                    viewport()->update();
                }
            }
        } else if (strokeActive_) {
            // Continuous stroke: fill the gap from the last dab to the cursor.
            // Spacing is proportional to the *pressure-scaled* diameter:
            // using the full-size radius here prints dotted
            // gaps at light pressure. Pressure ramps across the gap, so each
            // sub-dab gets its interpolated pressure. A single click lands one
            // dab (radius 0 path).
            const ToolId strokeTool = state_->activeTool();
            // Stabilizer: ease the cursor toward the pointer. Off (0) keeps
            // the raw cursor bit-exactly (legacy path); higher values lag
            // and smooth the line. Modes: classic exponential ease,
            // weighted (lag deepens with pointer speed), pixel snap
            // (smoothed cursor lands on whole document pixels for crisp
            // pixel-art lines).
            {
                const QVariant smv = state_->option(
                    strokeTool, QStringLiteral("smoothing"));
                const double sm = smv.isValid()
                                      ? qBound(0.0, smv.toDouble(), 100.0)
                                      : 0.0;
                const int smMode = qBound(
                    0,
                    state_
                        ->option(strokeTool,
                                 QStringLiteral("smoothing_mode"))
                        .toInt(),
                    2);
                const QPointF rawDoc = cursorDoc_;
                const std::uint64_t now = event->timestamp();
                if (strokeLastMoveMs_ > 0 && now > strokeLastMoveMs_) {
                    const double dt =
                        (now - strokeLastMoveMs_) / 1000.0;
                    const double step = std::hypot(
                        rawDoc.x() - strokeLastRawDoc_.x(),
                        rawDoc.y() - strokeLastRawDoc_.y());
                    strokeSpeed_ += (step / dt - strokeSpeed_) * 0.35;
                }
                strokeLastMoveMs_ = now;
                strokeLastRawDoc_ = rawDoc;
                if (sm > 0.0) {
                    double k = 1.0 - 0.95 * sm / 100.0;
                    if (smMode == 1) {
                        const double speed01 =
                            std::clamp(strokeSpeed_ / 2000.0, 0.0, 1.0);
                        k *= 1.0 - 0.6 * speed01;
                    }
                    strokeSmoothDoc_ += (rawDoc - strokeSmoothDoc_) * k;
                    if (smMode == 2)
                        strokeSmoothDoc_ = QPointF(
                            std::round(strokeSmoothDoc_.x()),
                            std::round(strokeSmoothDoc_.y()));
                    cursorDoc_ = strokeSmoothDoc_;
                }
            }
            const double baseRadius = qMax(0.0, strokeRadius());
            refreshDabOpts();
            auto scaledRadius = [&](double pr) {
                return baseRadius * dabSizeFactor(pr, dabSensorState(pr));
            };
            const double p0 = std::clamp(strokeLastPressure_, 0.0, 1.0);
            const double p1 = std::clamp(pixelPressure_, 0.0, 1.0);
            // Stroke clock for the time fade (event timestamps share one
            // clock, so synthetic events with set timestamps stay exact).
            {
                const quint64 now = event->timestamp();
                strokeElapsedMs_ =
                    now >= strokePressMs_ ? now - strokePressMs_ : 0;
            }
            // Isotropic spacing strides on the full diameter; the default
            // follows the pressure-scaled (minor-axis) radius instead.
            // Either way the stride follows tilt, fade and speed thinning
            // through the shared dab-size factor, so spacing never dots a
            // thinned stroke.
            const QVariant isoOpt = state_->option(
                strokeTool, QStringLiteral("brush_spacing_isotropic"));
            const double thinNow =
                tiltSizeFactor(tiltLean(tiltX_, tiltY_),
                               dabOpts_.tiltSizeAmt) *
                fadeFactor() *
                speedSizeFactor(
                    std::clamp(strokeSpeed_ / 2000.0, 0.0, 1.0),
                    dabOpts_.speedSizeAmt);
            double rMin =
                (isoOpt.isValid() && isoOpt.toBool())
                    ? baseRadius * thinNow
                    : qMax(0.0, std::min(scaledRadius(p0),
                                         scaledRadius(p1)));
            // Perspective thinning strides with the thinnest end of the
            // segment, so dabs never gap near the vanishing point.
            rMin *= std::min(perspectiveFactor(strokeLastDoc_),
                             perspectiveFactor(cursorDoc_));
            // Spacing is always the live brush_spacing option (% of tip
            // diameter; default 15 reproduces the legacy radius*0.3 step).
            // Auto spacing squares the percentage for fine low-end control,
            // unless a size-law coefficient is set: then the stride follows
            // the dab diameter directly (linear sub-pixel, square-root
            // above), ignoring the percentage.
            // Selecting a preset seeds the option from the file, so imports
            // keep their spacing while the slider stays live for every tip
            // kind, stamps and hoses included.
            const QVariant spacingOpt =
                state_->option(strokeTool, QStringLiteral("brush_spacing"));
            double spacingPct = spacingOpt.isValid()
                                    ? qBound(1.0, spacingOpt.toDouble(), 200.0)
                                    : 15.0;
            const bool spacingAuto =
                state_
                    ->option(strokeTool,
                             QStringLiteral("brush_spacing_auto"))
                    .toBool();
            const double spacingAutoCoeff = qBound(
                0.0,
                state_
                    ->option(strokeTool,
                             QStringLiteral("brush_spacing_auto_coeff"))
                    .toDouble(),
                8.0);
            double spacing = 0.0;
            if (spacingAuto && spacingAutoCoeff > 0.0) {
                spacing = qMax(
                    0.5, double(pittore::compute::auto_spacing_stride(
                             float(2.0 * rMin), float(spacingAutoCoeff))));
            } else {
                if (spacingAuto)
                    spacingPct = std::clamp(spacingPct * spacingPct / 100.0,
                                            0.5, 400.0);
                spacing = qMax(0.5, 2.0 * rMin * spacingPct / 100.0);
            }
            const QPointF delta = cursorDoc_ - strokeLastDoc_;
            const double dist = std::hypot(delta.x(), delta.y());
            if (dist > 0.0) {
                const int steps = std::max(1, int(dist / spacing));
                // Per pointer event: trace-gated (a full log write per event
                // at 120Hz input is pure overhead).
                if (pittore::core::log::strokeTrace())
                    PITTORE_LOG("[stroke] dist=%.1f spacing=%.2f steps=%d radius=%.1f last=(%.1f,%.1f) cur=(%.1f,%.1f)",
                                 dist, spacing, steps, rMin, strokeLastDoc_.x(),
                                 strokeLastDoc_.y(), cursorDoc_.x(), cursorDoc_.y());
                for (int k = 1; k <= steps; ++k) {
                    const double t = double(k) / double(steps);
                    pixelPressure_ = p0 + (p1 - p0) * t;
                    paintSymmetricAt(strokeLastDoc_ + delta * t);
                    // Travelled distance accumulates per sub-dab so fade
                    // tapers within a segment, not just across events.
                    strokeDist_ += dist / steps;
                }
                pixelPressure_ = p1;
                strokeLastDoc_ = cursorDoc_;
                strokeLastPressure_ = p1;
                strokeMaxP_ = std::max(strokeMaxP_, p1);
                state_->flushPaint();  // emits documentModified → refresh
            }
        } else if (liquifyActive_) {
            // One mesh dab per pointer sample; re-renders only the dab's
            // footprint from the frozen snapshot through the current mesh.
            liquifyDabAt(cursorDoc_);
        } else {
            // Marquee rubber-band drag: repaint only the band strip (old ∪
            // new), not the whole canvas. Crop (task context) darkens the full
            // surround, so it keeps a whole-viewport repaint.
            if (state_->taskContext() == TaskContext::Crop) {
                viewport()->update();
            } else {
                const ToolId bandTool = state_->activeTool();
                const QRectF nb =
                    isSelectionTool(bandTool)
                        ? marqueeLiveRect()
                        : QRectF(dragStartDoc_, dragCurrentDoc_).normalized();
                QRectF vr = documentTransform().mapRect(nb).adjusted(-6, -6, 6, 6);
                if (!liveViewRect_.isEmpty()) vr |= liveViewRect_;
                liveViewRect_ = vr;
                viewport()->update(vr.toAlignedRect());
            }
        }
        return;
    }
    // Crop hover: resize cursors over the frame's handles, the move cursor
    // inside it (drag to reposition), crosshair outside (draw a new frame).
    if (state_->activeTool() == ToolId::Crop) {
        cropHoverUpdate(event->position());
        return;
    }
    if (state_->activeTool() == ToolId::Move && moveTransformVisible()) {
        // Hover feedback over the transform handles.
        const int hov = handleAtView(event->position());
        Qt::CursorShape shape = Qt::SizeAllCursor;
        if (hov == 0 || hov == 4) shape = Qt::SizeFDiagCursor;
        else if (hov == 2 || hov == 6) shape = Qt::SizeBDiagCursor;
        else if (hov == 1 || hov == 5) shape = Qt::SizeVerCursor;
        else if (hov == 3 || hov == 7) shape = Qt::SizeHorCursor;
        viewport()->setCursor(shape);
    }
}

}  // namespace pittore::ui
