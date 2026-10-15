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
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "ui/tools/log/tool_log.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


// ---------------------------------------------------------------------------
// Move tool: body drags, transform handles, image drops
// ---------------------------------------------------------------------------
// Memoized Move-gizmo geometry: for a group, "has pixel descendants" and the
// bounds are O(N) walks over the whole subtree, so with a large SVG selection
// on screen every hover move and every canvas repaint would otherwise pay that
// walk (on top of the composite blit). Recomputed only when the active layer
// or the document's composite revision changes.
void CanvasView::ensureGizmoCache() const {
    DocumentItem* d = doc();
    const LayerItem* l = d ? state_->activeLayer() : nullptr;
    if (!d || !l) {
        gizmoCacheDoc_ = nullptr;
        gizmoCacheLayer_ = -1;
        gizmoCacheValid_ = false;
        gizmoCacheBounds_ = QRectF();
        gizmoCacheHasPixels_ = false;
        return;
    }
    if (gizmoCacheValid_ && gizmoCacheDoc_ == d &&
        gizmoCacheLayer_ == d->activeLayer && gizmoCacheRev_ == d->revision)
        return;
    gizmoCacheDoc_ = d;
    gizmoCacheLayer_ = d->activeLayer;
    gizmoCacheRev_ = d->revision;
    gizmoCacheValid_ = true;
    if (l->kind == LayerItem::Kind::Pixel && l->pixels) {
        gizmoCacheBounds_ = layerBounds(*d, *l);
        gizmoCacheHasPixels_ = true;
    } else if (l->kind == LayerItem::Kind::Group) {
        const QVector<int> desc =
            state_->groupPixelDescendantIndices(d->activeLayer);
        gizmoCacheHasPixels_ = !desc.isEmpty();
        gizmoCacheBounds_ =
            desc.isEmpty() ? QRectF() : state_->groupBounds(d->activeLayer);
    } else {
        gizmoCacheBounds_ = QRectF();
        gizmoCacheHasPixels_ = false;
    }
}


bool CanvasView::moveTransformVisible() const {
    if (state_->activeTool() != ToolId::Move) return false;
    if (!state_->option(ToolId::Move, QStringLiteral("showtransform")).toBool())
        return false;
    DocumentItem* d = doc();
    const LayerItem* l = d ? state_->activeLayer() : nullptr;
    if (!d || !l) return false;
    ensureGizmoCache();
    return gizmoCacheHasPixels_;
}


QRectF CanvasView::activeLayerBounds() const {
    DocumentItem* d = doc();
    const LayerItem* l = d ? state_->activeLayer() : nullptr;
    if (!d || !l) return QRectF();
    ensureGizmoCache();
    return gizmoCacheBounds_;
}


QPointF CanvasView::handleDocPos(int handle, const QRectF& bounds) const {
    const double cx = bounds.center().x(), cy = bounds.center().y();
    switch (handle) {
        case 0: return bounds.topLeft();
        case 1: return QPointF(cx, bounds.top());
        case 2: return bounds.topRight();
        case 3: return QPointF(bounds.right(), cy);
        case 4: return bounds.bottomRight();
        case 5: return QPointF(cx, bounds.bottom());
        case 6: return bounds.bottomLeft();
        default: return QPointF(bounds.left(), cy);
    }
}


int CanvasView::handleAtView(const QPointF& viewPos) const {
    const QRectF bounds = activeLayerBounds();
    if (bounds.isNull()) return -1;
    for (int h = 0; h < 8; ++h) {
        if ((documentToView(handleDocPos(h, bounds)) - viewPos).manhattanLength() <
            7.0)
            return h;
    }
    return -1;
}


// Snapping during a Move-tool drag: given the proposed layer offset `raw`,
// align the layer's left/center/right and top/center/bottom with every active
// target (guides, grid lines, document bounds) that falls within tolerance.
// Tolerance is 8 view pixels, translated to doc pixels by the zoom and capped
// at 32 doc px so a snap never teleports the layer at very low zoom. Returns
// the raw offset unchanged when snapping is off or nothing is near.
CanvasView::MoveSnap CanvasView::snappedMoveOffset(const QPointF& raw,
                                                   const LayerItem& layer,
                                                   const QVector<int>& moving) const {
    if (!layer.pixels) {
        MoveSnap snap;
        snap.offset = raw;
        return snap;
    }
    return snappedMoveSize(raw, QSizeF(layer.pixels->width() * layer.scaleX,
                                       layer.pixels->height() * layer.scaleY),
                           moving);
}


CanvasView::MoveSnap CanvasView::snappedMoveSize(const QPointF& raw,
                                                 const QSizeF& size,
                                                 const QVector<int>& moving) const {
    DocumentItem* d = doc();
    MoveSnap snap;
    snap.offset = raw;
    if (!d || !state_->snapEnabled() || size.isEmpty()) return snap;
    const int targets = state_->snapTargets();
    if (!targets) return snap;
    const double tol = qMin(8.0 / zoom(), 32.0);
    if (tol <= 0.0) return snap;

    const double w = size.width();
    const double h = size.height();
    if (w <= 0.0 || h <= 0.0) return snap;

    std::vector<double> xFeat{raw.x(), raw.x() + w / 2.0, raw.x() + w};
    std::vector<double> yFeat{raw.y(), raw.y() + h / 2.0, raw.y() + h};
    std::vector<double> xTargets, yTargets;

    if (targets & AppState::SnapGuides) {
        for (double g : d->verticalGuides) xTargets.push_back(g);
        for (double g : d->horizontalGuides) yTargets.push_back(g);
    }
    if (targets & AppState::SnapGrid) {
        // Same spacing the overlay paints (one source of truth).
        const double step = gridStep();
        for (double g = 0.0; g <= d->size.width(); g += step)
            xTargets.push_back(g);
        for (double g = 0.0; g <= d->size.height(); g += step)
            yTargets.push_back(g);
    }
    if (targets & AppState::SnapDocumentBounds) {
        xTargets.push_back(0.0);
        xTargets.push_back(d->size.width() / 2.0);
        xTargets.push_back(static_cast<double>(d->size.width()));
        yTargets.push_back(0.0);
        yTargets.push_back(d->size.height() / 2.0);
        yTargets.push_back(static_cast<double>(d->size.height()));
    }
    if (targets & AppState::SnapSlices) {
        for (const QRect& s : d->slices) {
            if (s.isEmpty()) continue;
            xTargets.push_back(s.left());
            xTargets.push_back(s.left() + s.width() / 2.0);
            xTargets.push_back(s.right() + 1.0);
            yTargets.push_back(s.top());
            yTargets.push_back(s.top() + s.height() / 2.0);
            yTargets.push_back(s.bottom() + 1.0);
        }
    }
    if (targets & AppState::SnapLayers) {
        // Edges and centres of the other visible layers. The layers this
        // gesture drags are excluded: their own edges sit a per-event step
        // behind the proposed position, always inside tolerance, so keeping
        // them would snap every small step back to where the layer just was
        // (the drag crawls in lurches instead of tracking the cursor) and,
        // once a feature drifted within tolerance of another of its own
        // edges, yank it by half its own width.
        for (int i = 0; i < d->layers.size(); ++i) {
            if (moving.contains(i)) continue;
            const LayerItem& l = d->layers[i];
            if (l.kind == LayerItem::Kind::Group) continue;
            if (!l.visible || !l.pixels) continue;
            const QRectF b = layerBounds(*d, l);
            if (b.isEmpty()) continue;
            xTargets.push_back(b.left());
            xTargets.push_back(b.center().x());
            xTargets.push_back(b.right());
            yTargets.push_back(b.top());
            yTargets.push_back(b.center().y());
            yTargets.push_back(b.bottom());
        }
    }

    // For each layer feature, the nearest target wins; the whole axis snaps by
    // the same delta so the alignment is exact (rawV - (feat - target)). The
    // winning target is reported so the overlay can draw its guide line.
    auto nearest = [&tol](const std::vector<double>& features,
                          const std::vector<double>& tgts, double rawV,
                          double* outTarget, bool* outHit, int* outFeat) {
        double best = rawV;
        double bestDist = tol + 1.0;
        double target = 0.0;
        int feat = -1;
        for (int i = 0; i < static_cast<int>(features.size()); ++i) {
            const double f = features[i];
            for (double t : tgts) {
                const double dist = std::fabs(f - t);
                if (dist <= tol && dist < bestDist) {
                    bestDist = dist;
                    best = rawV - (f - t);
                    target = t;
                    feat = i;
                }
            }
        }
        *outTarget = target;
        *outHit = bestDist <= tol;
        *outFeat = feat;
        return best;
    };
    snap.offset =
        QPointF(nearest(xFeat, xTargets, raw.x(), &snap.xPos, &snap.x,
                        &snap.xFeat),
                nearest(yFeat, yTargets, raw.y(), &snap.yPos, &snap.y,
                        &snap.yFeat));
    // The centre guide is special-cased in the overlay: green vertical, red
    // horizontal, as the live centre readout.
    snap.xCenter = snap.x && std::fabs(snap.xPos - d->size.width() / 2.0) < 0.5;
    snap.yCenter = snap.y && std::fabs(snap.yPos - d->size.height() / 2.0) < 0.5;
    return snap;
}


bool CanvasView::pickLayerAt(const QPointF& docPos) {
    DocumentItem* d = doc();
    if (!d || d->layers.isEmpty()) return false;
    int hit = topPixelLayerAt(*d, docPos);
    if (hit < 0) return false;
    // Clicking a member of a group reveals the whole (outermost) group, matching
    // beginMoveDrag: the unit drags/scales together, members are picked in the
    // Layers panel.
    const int g = state_->outerGroupContaining(hit);
    if (g >= 0) hit = g;
    // Shift adds to the multi-selection (multi-drag together) instead of
    // replacing it. The fallback only runs with Ctrl held, so this is
    // Ctrl+Shift+click to grow the set from the canvas.
    const bool shiftHeld =
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (shiftHeld) {
        if (!d->selectedLayers.contains(hit)) {
            if (d->selectedLayers.isEmpty())
                d->selectedLayers.push_back(
                    qBound(0, d->activeLayer, d->layers.size() - 1));
            d->selectedLayers.push_back(hit);
        }
    } else if (!d->selectedLayers.contains(hit)) {
        d->selectedLayers.clear();
        d->selectedLayers.push_back(hit);
    }
    if (d->activeLayer == hit) {
        // Already active: setActiveLayerIndex() would early-out without a
        // signal, but the Layers panel still needs to scroll to the row.
        emit state_->activeLayerChanged();
    } else {
        state_->setActiveLayerIndex(hit);
    }
    emit layerPickedFromCanvas();
    viewport()->update();
    return true;
}

bool CanvasView::pressHitsSelection(const QPointF& docPos) const {
    const DocumentItem* d = doc();
    if (!d || d->layers.isEmpty()) return false;
    // Only an explicit selection sticks (panel, Shift/Ctrl+A stack): with
    // nothing selected, auto-select picks normally.
    if (d->selectedLayers.isEmpty()) return false;
    for (int i : d->selectedLayers) {
        if (i < 0 || i >= d->layers.size()) continue;
        const LayerItem& l = d->layers[i];
        if (!l.visible) continue;
        const QRectF bounds = l.kind == LayerItem::Kind::Group
                                  ? state_->groupBounds(i)
                                  : layerBounds(*d, l);
        if (!bounds.isEmpty() && bounds.contains(docPos)) return true;
    }
    return false;
}

bool CanvasView::beginMoveDrag(const QPointF& docPos) {
    DocumentItem* d = doc();
    if (!d || d->layers.isEmpty()) return false;
    moveSnap_ = MoveSnap{};   // no smart guide until a move actually snaps
    const bool autoSelectOpt =
        state_->option(ToolId::Move, QStringLiteral("autoselect")).toBool();
    // Ctrl temporarily enables Auto-Select (conventional): a Ctrl+click
    // picks the layer under the cursor even when the option is off. Holding
    // Ctrl also pushes a temporary Move tool from any other tool, so this is
    // the click-an-image-to-find-its-layer gesture. Shift also forces a pick,
    // adding to the multi-selection so several layers drag together.
    const bool ctrlHeld =
        QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
    const bool shiftHeld =
        QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    const bool autoSelect = autoSelectOpt || ctrlHeld || shiftHeld;
    int index = qBound(0, d->activeLayer, d->layers.size() - 1);
    // A press inside the ACTIVE group's bounds keeps the group: grabbing its
    // body drags the whole unit instead of auto-selecting the part underneath.
    const bool pressInActiveGroup =
        d->layers[index].kind == LayerItem::Kind::Group &&
        state_->groupBounds(index).contains(docPos);
    // An explicit selection keeps its grip: a plain press inside the current
    // selection drags it instead of re-picking the topmost layer under the
    // cursor (which made an occluded selected layer unmovable — the Layers
    // panel choice always lost to the bigger front image). Ctrl/Shift keep
    // their pick/grow meaning.
    const bool pressInSelection =
        !shiftHeld && !ctrlHeld && pressHitsSelection(docPos);
    if (autoSelect && !pressInActiveGroup && !pressInSelection) {
        index = topPixelLayerAt(*d, docPos);
        if (index < 0) return false;   // grabbed empty canvas: nothing to move
        // Clicking a member of a group selects the whole (outermost) group so
        // the unit drags and scales together; individual members are picked in
        // the Layers panel.
        const int g = state_->outerGroupContaining(index);
        if (g >= 0) index = g;
        if (shiftHeld) {
            // Shift+click grows the set: the hit joins the multi-selection
            // and becomes active, so the whole set drags together.
            if (!d->selectedLayers.contains(index)) {
                if (d->selectedLayers.isEmpty())
                    d->selectedLayers.push_back(qBound(
                        0, d->activeLayer, d->layers.size() - 1));
                d->selectedLayers.push_back(index);
            }
        } else if (!d->selectedLayers.contains(index)) {
            // Clicking a layer that is already part of a multi-selection keeps
            // the whole set selected (a Shift/Ctrl+A'd stack stays whole and
            // moves together); clicking any other layer collapses to it.
            d->selectedLayers.clear();
            d->selectedLayers.push_back(index);
        }
        if (d->activeLayer == index) {
            // Re-clicking the active layer must still reveal it: the setter
            // early-outs without a signal, so emit to make the Layers panel
            // scroll to the row.
            emit state_->activeLayerChanged();
        } else {
            state_->setActiveLayerIndex(index);
        }
        emit layerPickedFromCanvas();
    }
    const LayerItem& l = d->layers[index];
    if (l.locked || !l.visible) return false;
    const bool isGroup = l.kind == LayerItem::Kind::Group;
    if (!isGroup && (l.kind != LayerItem::Kind::Pixel || !l.pixels)) return false;
    // Which layers move: the selection when the active layer is a plain pixel
    // layer, or the whole group's pixel descendants when it is a group — the
    // dropped-SVG unit drags as one piece.
    moveLayerSet_.clear();
    if (isGroup) {
        moveLayerSet_ = state_->groupPixelDescendantIndices(index);
        if (moveLayerSet_.isEmpty()) return false;
        const QRectF bounds = state_->groupBounds(index);
        moveGrabOffset_ = docPos - bounds.topLeft();
        moveAnchorStartOffset_ = bounds.topLeft();
    } else {
        moveLayerSet_ = state_->selectedLayerIndices();
        moveGrabOffset_ = docPos - l.offset;
        moveAnchorStartOffset_ = l.offset;
    }
    moveDragging_ = true;
    moveChanged_ = false;
    // Baseline every moved layer's offset at press so each mouse move applies
    // one absolute delta instead of accumulating. Locked/non-pixel layers get a
    // placeholder start too — the mover skips them, but the array must stay
    // index-aligned with moveLayerSet_.
    moveLayerStarts_.clear();
    for (int idx : moveLayerSet_) {
        if (idx < 0 || idx >= d->layers.size()) continue;
        moveLayerStarts_.append(d->layers[idx].offset);
    }
    // The press-time baseline a trace can measure every move/drag line against:
    // a random off-screen jump only makes sense relative to where the gesture
    // started (grab point, anchor offset, how many layers ride along).
    tool_logf(state_->activeTool(), "move/drag-begin",
              "press=(%.1f,%.1f) grab=(%.1f,%.1f) anchor0=(%.1f,%.1f) "
              "set=%d starts=%d group=%d",
              docPos.x(), docPos.y(), moveGrabOffset_.x(), moveGrabOffset_.y(),
              moveAnchorStartOffset_.x(), moveAnchorStartOffset_.y(),
              int(moveLayerSet_.size()), int(moveLayerStarts_.size()),
              int(isGroup));
    return true;
}


bool CanvasView::beginScaleDrag(int handle, const QPointF& docPos) {
    Q_UNUSED(docPos);
    DocumentItem* d = doc();
    LayerItem* l = d ? state_->activeLayer() : nullptr;
    if (!d || !l || l->locked) return false;
    const bool isGroup = l->kind == LayerItem::Kind::Group;
    if (!isGroup && (l->kind != LayerItem::Kind::Pixel || !l->pixels))
        return false;
    const QRectF bounds =
        isGroup ? state_->groupBounds(d->activeLayer) : layerBounds(*d, *l);
    if (bounds.isEmpty()) return false;
    // Corners scale freeform by default; Shift constrains proportions live
    // (pressing or releasing it mid-drag switches modes). Edges scale one
    // axis. The opposite handle stays pinned for the whole gesture.
    scaleDragging_ = true;
    activeHandle_ = handle;
    scaleAnchorDoc_ = handleDocPos((handle + 4) % 8, bounds);
    scaleStartBounds_ = bounds;
    if (isGroup) {
        // Baseline every pixel descendant's offset/scale so the whole group
        // scales about the same anchor as one unit.
        moveLayerSet_ = state_->groupPixelDescendantIndices(d->activeLayer);
        scaleStartOffsets_.clear();
        scaleStartXs_.clear();
        scaleStartYs_.clear();
        for (int idx : moveLayerSet_) {
            scaleStartOffsets_.append(d->layers[idx].offset);
            scaleStartXs_.append(d->layers[idx].scaleX);
            scaleStartYs_.append(d->layers[idx].scaleY);
        }
        if (scaleStartOffsets_.isEmpty()) {
            scaleDragging_ = false;
            return false;
        }
        scaleStartOffset_ = QPointF();
        scaleStartX_ = scaleStartY_ = 1.0;
    } else {
        moveLayerSet_ = QVector<int>{d->activeLayer};
        scaleStartOffsets_ = QVector<QPointF>{l->offset};
        scaleStartXs_ = QVector<double>{l->scaleX};
        scaleStartYs_ = QVector<double>{l->scaleY};
        scaleStartOffset_ = l->offset;
        scaleStartX_ = l->scaleX;
        scaleStartY_ = l->scaleY;
        scaleStartTextSize_ = l->textSpec.size;
        scaleStartTextOrigin_ = l->textSpec.origin;
    }
    moveChanged_ = false;
    return true;
}


void CanvasView::updateScaleDrag(const QPointF& docPos, bool proportional) {
    DocumentItem* d = doc();
    if (!d || !scaleDragging_ || activeHandle_ < 0) return;
    LayerItem* l = state_->activeLayer();
    if (!l) return;
    if (l->kind == LayerItem::Kind::Group) {
        // Scale every pixel descendant about the same fixed anchor. The factor
        // is driven by the group's bounding box so the group scales as a unit.
        const double startW = scaleStartBounds_.width();
        const double startH = scaleStartBounds_.height();
        double fx = 1.0, fy = 1.0;
        const QPointF startHandle = handleDocPos(activeHandle_, scaleStartBounds_);
        // Proportional comes from the move event's Shift state (live, so it
        // can change mid-drag); otherwise corners scale each axis freely.
        if (proportional) {
            const QPointF diag = startHandle - scaleAnchorDoc_;
            const double denom = QPointF::dotProduct(diag, diag);
            double f = denom > 1e-9
                           ? QPointF::dotProduct(docPos - scaleAnchorDoc_, diag) / denom
                           : 1.0;
            f = qMax(f, 8.0 / qMax(1.0, qMax(startW, startH)));
            fx = fy = f;
        } else if (activeHandle_ == 1 || activeHandle_ == 5) {  // top / bottom
            const double span0 = startHandle.y() - scaleAnchorDoc_.y();
            fy = qAbs(span0) > 1e-9
                     ? (docPos.y() - scaleAnchorDoc_.y()) / span0
                     : 1.0;
            fy = qMax(fy, 8.0 / qMax(1.0, startH));
            fx = 1.0;
        } else if (activeHandle_ == 3 || activeHandle_ == 7) {  // left / right
            const double span0 = startHandle.x() - scaleAnchorDoc_.x();
            fx = qAbs(span0) > 1e-9
                     ? (docPos.x() - scaleAnchorDoc_.x()) / span0
                     : 1.0;
            fx = qMax(fx, 8.0 / qMax(1.0, startW));
            fy = 1.0;
        } else {  // corners freeform: each axis follows the cursor
            const double spanX = startHandle.x() - scaleAnchorDoc_.x();
            fx = qAbs(spanX) > 1e-9
                     ? (docPos.x() - scaleAnchorDoc_.x()) / spanX
                     : 1.0;
            fx = qMax(fx, 8.0 / qMax(1.0, startW));
            const double spanY = startHandle.y() - scaleAnchorDoc_.y();
            fy = qAbs(spanY) > 1e-9
                     ? (docPos.y() - scaleAnchorDoc_.y()) / spanY
                     : 1.0;
            fy = qMax(fy, 8.0 / qMax(1.0, startH));
        }
        QVector<QPointF> offsets;
        QVector<double> sxs, sys;
        offsets.reserve(moveLayerSet_.size());
        sxs.reserve(moveLayerSet_.size());
        sys.reserve(moveLayerSet_.size());
        for (int i = 0; i < moveLayerSet_.size(); ++i) {
            const int idx = moveLayerSet_.at(i);
            if (idx < 0 || idx >= d->layers.size()) {
                offsets.append(QPointF());
                sxs.append(1.0);
                sys.append(1.0);
                continue;
            }
            // The anchor's source position in this child is fixed; the dragged
            // handle follows the cursor. Offsets/scales are press-time baselines,
            // so each mouse move recomputes from the gesture start.
            const double sxo = scaleStartXs_.at(i);
            const double syo = scaleStartYs_.at(i);
            const QPointF& so = scaleStartOffsets_.at(i);
            const double aSrcX = (scaleAnchorDoc_.x() - so.x()) / sxo;
            const double aSrcY = (scaleAnchorDoc_.y() - so.y()) / syo;
            const double sx = sxo * fx;
            const double sy = syo * fy;
            offsets.append(QPointF(scaleAnchorDoc_.x() - aSrcX * sx,
                                   scaleAnchorDoc_.y() - aSrcY * sy));
            sxs.append(sx);
            sys.append(sy);
        }
        if (state_->setLayerPlacements(moveLayerSet_, offsets, sxs, sys))
            moveChanged_ = true;
        return;
    }
    if (!l || l->kind != LayerItem::Kind::Pixel || !l->pixels) return;
    // A live text layer scales through its point size: the glyphs are re-set at
    // the new size (never resampled) and the opposite handle stays pinned. The
    // scaling is about the layout origin, which is where the run's metrics grow
    // from, so the fixed point stays put.
    if (l->liveText) {
        const QPointF startHandle = handleDocPos(activeHandle_, scaleStartBounds_);
        const QPointF diag = startHandle - scaleAnchorDoc_;
        const double denom = QPointF::dotProduct(diag, diag);
        double f = denom > 1e-9 ? QPointF::dotProduct(docPos - scaleAnchorDoc_, diag) / denom
                                : 1.0;
        f = qMax(f, 1.0 / 128.0);
        const double startSize = qMax(1.0, scaleStartTextSize_);
        const double newSize = qBound(1.0, startSize * f, 1296.0);
        const double applied = newSize / startSize;
        const QPointF origin =
            scaleAnchorDoc_ - applied * (scaleAnchorDoc_ - scaleStartTextOrigin_);
        if (state_->setLiveTextSize(d->activeLayer, newSize, origin)) moveChanged_ = true;
        return;
    }
    const double srcW = l->pixels->width(), srcH = l->pixels->height();

    // The anchor's source position is fixed; the dragged handle follows the
    // cursor. Proportional corners project the cursor onto the starting
    // diagonal so the gesture feels stable in every direction.
    const double aSrcX =
        (scaleAnchorDoc_.x() - scaleStartOffset_.x()) / scaleStartX_;
    const double aSrcY =
        (scaleAnchorDoc_.y() - scaleStartOffset_.y()) / scaleStartY_;

    double sx = scaleStartX_, sy = scaleStartY_;
    const QPointF startHandle = handleDocPos(activeHandle_, scaleStartBounds_);
    // Proportional comes from the move event's Shift state (live, so it can
    // change mid-drag); otherwise corners scale each axis freely, edges one.
    if (proportional) {
        const QPointF diag = startHandle - scaleAnchorDoc_;
        const double denom = QPointF::dotProduct(diag, diag);
        double f = denom > 1e-9
                       ? QPointF::dotProduct(docPos - scaleAnchorDoc_, diag) / denom
                       : 1.0;
        f = qMax(f, 8.0 / qMax(1.0, qMax(srcW * scaleStartX_, srcH * scaleStartY_)));
        sx = scaleStartX_ * f;
        sy = scaleStartY_ * f;
    } else if (activeHandle_ == 1 || activeHandle_ == 5) {  // top / bottom
        const double span0 = startHandle.y() - scaleAnchorDoc_.y();
        if (qAbs(span0) > 1e-9)
            sy = scaleStartY_ * (docPos.y() - scaleAnchorDoc_.y()) / span0;
        sy = qMax(sy, 8.0 / qMax(1.0, srcH));
    } else if (activeHandle_ == 3 || activeHandle_ == 7) {  // left / right
        const double span0 = startHandle.x() - scaleAnchorDoc_.x();
        if (qAbs(span0) > 1e-9)
            sx = scaleStartX_ * (docPos.x() - scaleAnchorDoc_.x()) / span0;
        sx = qMax(sx, 8.0 / qMax(1.0, srcW));
    } else {  // corners freeform: each axis follows the cursor
        const double spanX = startHandle.x() - scaleAnchorDoc_.x();
        if (qAbs(spanX) > 1e-9)
            sx = scaleStartX_ * (docPos.x() - scaleAnchorDoc_.x()) / spanX;
        sx = qMax(sx, 8.0 / qMax(1.0, srcW));
        const double spanY = startHandle.y() - scaleAnchorDoc_.y();
        if (qAbs(spanY) > 1e-9)
            sy = scaleStartY_ * (docPos.y() - scaleAnchorDoc_.y()) / spanY;
        sy = qMax(sy, 8.0 / qMax(1.0, srcH));
    }
    const QPointF offset(scaleAnchorDoc_.x() - aSrcX * sx,
                         scaleAnchorDoc_.y() - aSrcY * sy);
    if (state_->setActiveLayerPlacement(offset, sx, sy)) moveChanged_ = true;
}

}  // namespace pittore::ui
