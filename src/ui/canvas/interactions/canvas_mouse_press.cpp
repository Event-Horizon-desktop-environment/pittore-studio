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
#include "ui/persona/vector_edit.h"
#include "ui/persona/vector_node.h"
#include "ui/persona/vector_point_ops.h"
#include "ui/persona/vector_path_ops.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "ui/tools/log/tool_log.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


void CanvasView::updateMeasureHint() {
    DocumentItem* d = doc();
    if (!d || !d->rulerHasMeasurement) return;
    const double len = QLineF(d->rulerStart, d->rulerEnd).length();
    const double ang =
        qRadiansToDegrees(std::atan2(d->rulerEnd.y() - d->rulerStart.y(),
                                     d->rulerEnd.x() - d->rulerStart.x()));
    const int units =
        state_->option(ToolId::MeasureTool, QStringLiteral("measure_units")).toInt();
    const double scale =
        state_->option(ToolId::MeasureTool, QStringLiteral("draw_scale")).toDouble();
    static const char* kNames[] = {"px", "pt", "mm", "cm", "in"};
    const char* name = (units >= 0 && units < 5) ? kNames[units] : kNames[0];
    state_->setStatusHint(
        tr("Measure: %1 %2  ∠ %3°")
            .arg(vectorMeasureDisplay(len, units, d->dpi, scale), 0, 'f', 1)
            .arg(QString::fromUtf8(name))
            .arg(ang, 0, 'f', 1));
}

void CanvasView::updateAreaHint() {
    DocumentItem* d = doc();
    if (!d || !d->areaHasMeasurement) return;
    const double w = d->areaRect.width();
    const double h = d->areaRect.height();
    const double areaPx = w * h;
    const int units =
        state_->option(ToolId::AreaTool, QStringLiteral("measure_units")).toInt();
    const double scale =
        state_->option(ToolId::AreaTool, QStringLiteral("draw_scale")).toDouble();
    static const char* kNames[] = {"px", "pt", "mm", "cm", "in"};
    const char* name = (units >= 0 && units < 5) ? kNames[units] : kNames[0];
    // Area scales quadratically through the drawing scale (an N-px square
    // is (N/scale)² units); px reports raw document pixels.
    const double areaUnits =
        (scale > 0.0 && units != 0) ? areaPx / (scale * scale) : areaPx;
    state_->setStatusHint(
        tr("Area: %1 %2² (%3 × %4 px)")
            .arg(areaUnits, 0, 'f', 1)
            .arg(QString::fromUtf8(name))
            .arg(w, 0, 'f', 1)
            .arg(h, 0, 'f', 1));
}

void CanvasView::mousePressEvent(QMouseEvent* event) {
    DocumentItem* d = doc();
    if (!d) return;

    // A stale brush-resize menu suppressor must never leak into a later
    // genuine right-click: any new press clears it.
    brushResizeSuppressMenu_ = false;

    const QPointF docPoint = viewToDocument(event->position());
    const ToolId tool = state_->activeTool();

    // Manual double-click detector: some setups never deliver a real
    // MouseButtonDblClick to the canvas, which left double-click-to-edit dead
    // (the second press just began another move/paint drag). A second quick
    // left press near the previous one on a text layer starts editing
    // instead; anywhere else it falls through to the normal press handling.
    // (If Qt does deliver the DblClick event afterwards, the live-session
    // branch turns it into a word selection — the standard pairing.)
    if (event->button() == Qt::LeftButton) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool secondPress =
            lastPressOnCanvas_ && lastPressMs_ > 0 &&
            lastPressTool_ == tool &&
            (nowMs - lastPressMs_) <= QApplication::doubleClickInterval() &&
            (event->position() - lastPressView_).manhattanLength() <=
                QApplication::startDragDistance();
        lastPressMs_ = nowMs;
        lastPressView_ = event->position();
        lastPressOnCanvas_ = true;
        lastPressTool_ = tool;
        if (secondPress && !textEditing_) {
            dragging_ = false;
            if (handleTextDoubleClick(docPoint)) {
                event->accept();
                return;
            }
            // Crop: the manual second press is the double-click that commits
            // the frame, matching the real MouseButtonDblClick path. It must
            // not also fall through into a rubber band (that would replace
            // the frame with a hairline right before the commit).
            if (tool == ToolId::Crop) {
                commitCrop();
                event->accept();
                return;
            }
            // Not text: fall through; the press below starts the drag/stroke
            // exactly like a normal single click.
        }
    }

    // A press outside the pen family abandons the in-progress path (same as
    // switching tools mid-draw); a press with a different pen tool restarts.
    // A stranded brush stroke is dropped the same way.
    if (penActive_ && !isPenTool(tool)) cancelPenPath();
    if (vbrushActive_ && tool != ToolId::VectorBrushTool) {
        vbrushActive_ = false;
        vbrushStroke_.clear();
    }
    // A polygonal session belongs to its tool: leaving it abandons the loop.
    if (tool != ToolId::PolygonalLasso && !polyPts_.empty()) {
        polyPts_.clear();
        polyHoverOn_ = false;
    }
    if (tool != ToolId::Lasso && tool != ToolId::MagneticLasso) {
        lassoLive_ = false;
        lassoStroke_.clear();
    }
    if (tool != ToolId::SelectionBrush) {
        selBrushLive_ = false;
        selBrushMask_ = QImage();
    }

    // First line of any gesture, in this tool's own log: a crash on click
    // (rather than on switch) still names the tool and where it was pressed.
    tool_logf(tool, "input/mouse-press", "button=0x%x mods=0x%x doc=(%.1f, %.1f)",
              static_cast<int>(event->button()), static_cast<int>(event->modifiers()),
              docPoint.x(), docPoint.y());

    // Middle-drag and the Hand tool both pan; so does Space, which the main
    // window turns into a temporary Hand tool before the event reaches here.
    // Only the left button pans with Hand — a right-press must fall through to
    // contextMenuEvent (the zoom/rotate menu), not start a pan.
    if (event->button() == Qt::MiddleButton ||
        (tool == ToolId::Hand && event->button() == Qt::LeftButton)) {
        panning_ = true;
        panStartView_ = event->position();
        panStartScroll_ = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }

    // Left-click only: a right-press reaches contextMenuEvent and must not zoom.
    // Alt+Right-drag resizes the brush (conventional behaviour): horizontal sets
    // size, vertical sets hardness. Starts here, tracks in mouseMove, ends on
    // right-release. Only for tools with a brush_size option.
    if (event->button() == Qt::RightButton &&
        event->modifiers().testFlag(Qt::AltModifier)) {
        if (state_->option(tool, QStringLiteral("brush_size")).isValid()) {
            brushResizeActive_ = true;
            brushResizeStartView_ = event->position();
            brushResizeStartSize_ =
                state_->option(tool, QStringLiteral("brush_size")).toDouble();
            if (!(brushResizeStartSize_ > 0.0)) brushResizeStartSize_ = 64.0;
            const QVariant h =
                state_->option(tool, QStringLiteral("brush_hardness"));
            brushResizeStartHardness_ = h.isValid() ? h.toDouble() : 50.0;
            event->accept();
            return;
        }
    }
    if (tool == ToolId::Zoom && event->button() == Qt::LeftButton) {
        // Scrubby zoom (honours the options-bar checkbox): press-drag scrubs
        // continuously; a press without a drag is the classic step zoom.
        if (state_->option(tool, QStringLiteral("scrubby")).toBool()) {
            zoomScrubbing_ = true;
            zoomScrubMoved_ = false;
            zoomScrubAnchorView_ = event->position();
            zoomScrubLastView_ = event->position();
            zoomScrubStart_ = zoom();
            dragging_ = true;
            dragStartDoc_ = docPoint;
            dragCurrentDoc_ = docPoint;
            event->accept();
            return;
        }
        const bool out = event->modifiers().testFlag(Qt::AltModifier) ||
                         state_->option(tool, QStringLiteral("zoommode")).toInt() == 1;
        const double before = zoom();
        out ? zoomOut() : zoomIn();
        if (!qFuzzyCompare(before, zoom())) setZoom(zoom(), event->position());
        return;
    }

    if (tool == ToolId::Eyedropper) {
        if (!d->composite.isNull()) {
            const QPoint pixel = docPoint.toPoint();
            if (d->composite.rect().contains(pixel)) {
                const QColor sampled = d->composite.pixelColor(pixel);
                const bool toBackground = event->modifiers().testFlag(Qt::AltModifier);
                toBackground ? state_->setBackground(sampled) : state_->setForeground(sampled);
                emit colorSampled(sampled, toBackground);
            }
        }
        return;
    }

    // Color Sampler: place/move a pin (max 4); Alt-click removes one.
    if (tool == ToolId::ColorSampler && event->button() == Qt::LeftButton) {
        const int hit = samplePinAtView(event->position());
        if (event->modifiers().testFlag(Qt::AltModifier)) {
            if (hit < 0) return;
            state_->beginUndoStep();
            d->colorSamples.removeAt(hit);
        } else if (hit >= 0) {
            if (QLineF(d->colorSamples[hit], docPoint).length() < 0.5) return;
            state_->beginUndoStep();
            d->colorSamples[hit] = docPoint;
        } else if (d->colorSamples.size() < 4) {
            state_->beginUndoStep();
            d->colorSamples.append(docPoint);
        } else {
            state_->setStatusHint(tr("Color Sampler: maximum of 4 samples."));
            return;
        }
        state_->commitUndoStep(tr("Color Sampler"), QStringLiteral("color-sampler"));
        state_->markAnnotationsChanged();
        return;
    }

    // Slice (R19): press-drag draws a new export rectangle; the live rectangle
    // is shown dashed until release, then committed as one history step.
    if (tool == ToolId::Slice && event->button() == Qt::LeftButton) {
        sliceDragging_ = true;
        sliceDragAnchorDoc_ = docPoint;
        sliceDragCurrentDoc_ = docPoint;
        viewport()->update();
        return;
    }

    // Frame: press-drag draws the placeholder box; release commits it as a
    // vector shape layer (Rectangle/Ellipse by the shape toggle, carrying
    // that shape tool's current fill/stroke style).
    if (tool == ToolId::Frame && event->button() == Qt::LeftButton) {
        frameDragging_ = true;
        frameAnchorDoc_ = docPoint;
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        viewport()->update();
        return;
    }

    // Artboard: press-drag resizes the canvas to the dragged rect; the
    // background well paints exposed paper. One undo step on release.
    if (tool == ToolId::Artboard && event->button() == Qt::LeftButton) {
        artboardDragging_ = true;
        artboardAnchorDoc_ = docPoint;
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        state_->beginUndoStep();
        viewport()->update();
        return;
    }

    // Perspective Crop: press-drag draws the crop quad (a rectangle until
    // a corner moves); press near a committed quad corner drags it;
    // double-click commits the warp + crop. No undo until commit.
    if (tool == ToolId::PerspectiveCrop && event->button() == Qt::LeftButton) {
        const double tol = 8.0 / qMax(0.01, d->zoom);
        pcropCorner_ = -1;
        if (pcropArmed_) {
            double best = tol;
            for (int i = 0; i < 4; ++i) {
                const double dd = QLineF(docPoint, pcropQuad_[i]).length();
                if (dd < best) {
                    best = dd;
                    pcropCorner_ = i;
                }
            }
        }
        if (pcropCorner_ < 0) {
            pcropArmed_ = false;
            pcropAnchorDoc_ = docPoint;
        }
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        viewport()->update();
        return;
    }

    // Slice Select: press selects a slice and starts a move drag (one history
    // step on release, only when it actually moves); a press on empty canvas
    // clears the selection.
    if (tool == ToolId::SliceSelect && event->button() == Qt::LeftButton) {
        const int hit = sliceAtView(event->position());
        if (hit >= 0) {
            // Snapshot before the drag mutates the slice list, so Undo returns
            // the slice to where it was (mirrors the Note drag).
            state_->beginUndoStep();
            sliceSelectDragging_ = true;
            sliceSelectMoved_ = false;
            sliceSelectIndex_ = hit;
            sliceSelectGrabDoc_ = docPoint;
            sliceSelectStartOffset_ = QPointF(d->slices[hit].topLeft());
            d->selectedSlice = hit;
        } else {
            d->selectedSlice = -1;
        }
        viewport()->update();
        return;
    }

    // Patch: press inside the committed selection starts a donor drag
    // (one history step on release, only when it actually moves); a press
    // on empty canvas starts a marquee that becomes the selection.
    // Mask selections hit-test by bounding box; the transfer itself reads
    // true coverage, so a sloppy grab stays harmless.
    if (tool == ToolId::Patch && event->button() == Qt::LeftButton) {
        patchAnchorDoc_ = docPoint;
        patchPressDoc_ = docPoint;
        patchMoved_ = false;
        if (!d->selection.isEmpty() && d->selection.contains(docPoint)) {
            patchPhase_ = 2;
            state_->beginUndoStep();
            // Copy-on-write now (the paint press block below is skipped by
            // the early return): without it the release-time transfer would
            // write through the snapshot's shared pixels and corrupt undo.
            state_->copyOnWriteActiveLayer();
        } else {
            patchPhase_ = 1;
        }
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        viewport()->update();
        return;
    }

    // ContentAwareMove: press inside the selection starts a content drag
    // (one history step on release); elsewhere starts a marquee selection.
    // transform_on_drop is not applied (noted at the transfer twin).
    if (tool == ToolId::ContentAwareMove && event->button() == Qt::LeftButton) {
        camPressDoc_ = docPoint;
        camMoved_ = false;
        if (!d->selection.isEmpty() && d->selection.contains(docPoint)) {
            camPhase_ = 2;
            state_->beginUndoStep();
            // Copy-on-write now (same trap as the Patch press above).
            state_->copyOnWriteActiveLayer();
        } else {
            camPhase_ = 1;
        }
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        viewport()->update();
        return;
    }

    // Rotate View: drag around the viewport centre; the pointer's bearing sets
    // the angle (Shift snaps to 15°). The rotation is a view property — no undo.
    if (tool == ToolId::RotateView && event->button() == Qt::LeftButton) {
        rotating_ = true;
        const QPointF center(viewport()->width() / 2.0, viewport()->height() / 2.0);
        rotateStartPointerAngle_ =
            std::atan2(event->position().y() - center.y(),
                       event->position().x() - center.x()) *
            180.0 / 3.14159265358979323846;
        rotateStartRotation_ = d->rotation;
        return;
    }

    // Ruler: press-drag measures from the anchor to the pointer. Measure shares
    // the gesture (same line, same undo); only its readout converts units.
    if ((tool == ToolId::Ruler || tool == ToolId::MeasureTool) &&
        event->button() == Qt::LeftButton) {
        if (tool == ToolId::MeasureTool &&
            state_->option(tool, QStringLiteral("assign_scale")).toBool()) {
            bool ok = false;
            const double current =
                state_->option(tool, QStringLiteral("draw_scale")).toDouble();
            const double scale = QInputDialog::getDouble(
                viewport(), tr("Drawing Scale"),
                tr("Document pixels per unit (e.g. 100 for 1:100):"),
                current > 0.0 ? current : 1.0, 0.0001, 1000000.0, 4, &ok);
            state_->setOptionSilently(tool, QStringLiteral("assign_scale"), false);
            if (!ok) return;
            state_->setOption(tool, QStringLiteral("draw_scale"), scale);
        }
        rulerDragging_ = true;
        state_->beginUndoStep();
        d->rulerStart = docPoint;
        d->rulerEnd = docPoint;
        d->rulerHasMeasurement = true;
        if (tool == ToolId::MeasureTool) updateMeasureHint();
        viewport()->update();
        return;
    }

    // Area: press-drag measures the rectangle from the anchor corner to
    // the pointer (same undo and annotation plumbing as the ruler line).
    // assign_scale shares the Measure tool's drawing-scale dialog.
    if (tool == ToolId::AreaTool && event->button() == Qt::LeftButton) {
        if (state_->option(tool, QStringLiteral("assign_scale")).toBool()) {
            bool ok = false;
            const double current =
                state_->option(tool, QStringLiteral("draw_scale")).toDouble();
            const double scale = QInputDialog::getDouble(
                viewport(), tr("Drawing Scale"),
                tr("Document pixels per unit (e.g. 100 for 1:100):"),
                current > 0.0 ? current : 1.0, 0.0001, 1000000.0, 4, &ok);
            state_->setOptionSilently(tool, QStringLiteral("assign_scale"), false);
            if (!ok) return;
            state_->setOption(tool, QStringLiteral("draw_scale"), scale);
        }
        areaDragging_ = true;
        areaAnchorDoc_ = docPoint;
        state_->beginUndoStep();
        d->areaRect = QRectF(docPoint, docPoint);
        d->areaHasMeasurement = true;
        updateAreaHint();
        viewport()->update();
        return;
    }

    // Red Eye: press-drag boxes the pupil, release fixes it. The box
    // reuses the marquee overlay (dragging_ drives it); the release path
    // below resets dragging_ before the generic section can see it.
    if (tool == ToolId::RedEye && event->button() == Qt::LeftButton) {
        redeyeDragging_ = true;
        redeyeAnchorDoc_ = docPoint;
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        viewport()->update();
        return;
    }

    // Place: the click point goes to the window (file dialog), which places
    // the image centred there. No drag, no undo of our own.
    if (tool == ToolId::PlaceTool && event->button() == Qt::LeftButton) {
        emit placeRequested(docPoint);
        event->accept();
        return;
    }

    // Import Photos: same file-dialog placement as Place (multi-select);
    // device import has no desktop API, so the dialog is the honest path.
    if (tool == ToolId::ImportPhotos && event->button() == Qt::LeftButton) {
        emit placeRequested(docPoint);
        event->accept();
        return;
    }

    // Generative Fill / Background: press-drag marks the fill area as a
    // selection; the Generate button consumes it (needs a model).
    if ((tool == ToolId::GenerativeFill ||
         tool == ToolId::GenerateBackground) &&
        event->button() == Qt::LeftButton) {
        genMarking_ = true;
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        viewport()->update();
        return;
    }

    // Content-Aware Tracing: press-drag marquees the area to vectorize.
    if (tool == ToolId::ContentAwareTracing &&
        event->button() == Qt::LeftButton) {
        traceMarking_ = true;
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        viewport()->update();
        return;
    }

    // Style Picker: first click on art samples its paint, next click on art
    // applies it (attribute toggles come from the options bar). Value-kept so
    // nothing can dangle between pick and apply.
    if (tool == ToolId::StylePickerTool && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (!l || !l->art || l->art->isEmpty()) {
            state_->setStatusHint(
                tr("Style Picker: click a vector shape to sample."));
            stylePicked_.reset();
            event->accept();
            return;
        }
        if (!stylePicked_) {
            stylePicked_ = l->art->paint;
            stylePickedOpacity_ = l->art->opacity;
            state_->setStatusHint(
                tr("Style Picker: paint sampled — click a target shape."));
        } else {
            auto paint = l->art->paint;
            const bool useStroke = state_->option(tool, QStringLiteral("pick_stroke")).toBool();
            const bool useFill = state_->option(tool, QStringLiteral("pick_fill")).toBool();
            const bool useOpacity =
                state_->option(tool, QStringLiteral("pick_opacity")).toBool();
            if (useStroke) {
                paint.hasStroke = stylePicked_->hasStroke;
                std::copy(std::begin(stylePicked_->stroke),
                          std::end(stylePicked_->stroke), std::begin(paint.stroke));
                paint.strokeWidth = stylePicked_->strokeWidth;
                paint.cap = stylePicked_->cap;
                paint.join = stylePicked_->join;
                paint.hasDash = stylePicked_->hasDash;
                paint.dash = stylePicked_->dash;
                paint.dashOffset = stylePicked_->dashOffset;
                paint.hasProfile = stylePicked_->hasProfile;
                paint.profile = stylePicked_->profile;
            }
            if (useFill) {
                paint.hasFill = stylePicked_->hasFill;
                std::copy(std::begin(stylePicked_->fill),
                          std::end(stylePicked_->fill), std::begin(paint.fill));
            }
            // Opacity transfers from the SAMPLED layer, not the target.
            const double sampledOpacity =
                useOpacity ? stylePickedOpacity_ : -1.0;
            if (state_->applyVectorPaint(hit, paint, sampledOpacity,
                                         tr("Style Picker")))
                state_->setStatusHint(tr("Style Picker: applied."));
            stylePicked_.reset();
        }
        viewport()->update();
        event->accept();
        return;
    }

    // Vector Crop: press-drag draws the keep rectangle; release masks outside
    // it away (non-destructive — the mask stays editable).
    if (tool == ToolId::VectorCropTool && event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        return;
    }

    // Node: press on a curve point of the art under the cursor grabs it. The
    // working copy previews in the overlay; release commits one re-raster.
    // Handles win over anchors on overlap; a bare click selects the anchor
    // for the convert/close actions.
    if (tool == ToolId::NodeTool && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (l && l->art && !l->art->isEmpty() && l->scaleX > 0.0 &&
            l->scaleY > 0.0) {
            const QPointF src((docPoint.x() - l->offset.x()) / l->scaleX,
                              (docPoint.y() - l->offset.y()) / l->scaleY);
            const QTransform inv =
                QTransform(l->art->matrix[0], l->art->matrix[1],
                           l->art->matrix[2], l->art->matrix[3],
                           l->art->matrix[4], l->art->matrix[5])
                    .inverted();
            const QPointF nodePos = inv.map(src);
            // Tolerance in node units for an ~8 px view grab: view → doc
            // (÷ zoom) → source (÷ layer scale) → node (× inverse scale).
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) *
                                   std::hypot(inv.m11(), inv.m12());
            const NodeHandle nh =
                nodeHandleAt(*l->art, nodePos, qMax(1e-9, tolNode));
            if (nh.anchorSeg >= 0) {
                nodeDragging_ = true;
                nodeMoved_ = false;
                nodeLayer_ = hit;
                nodeSeg_ = nh.anchorSeg;
                nodeHandleSeg_ = nh.anchorSeg;
                nodeHandleSide_ = (nh.side == NodeHandleSide::Out) ? 1 : 0;
                nodeHandleMirror_ =
                    !event->modifiers().testFlag(Qt::AltModifier) &&
                    nodeHandlesMirrored(*l->art, nh.anchorSeg,
                                        qMax(1e-9, tolNode * 0.05));
                nodeWork_ = *l->art;
                nodeHandleGrabDelta_ = nh.pos - nodePos;
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
            int seg = -1;
            if (nodeEndpointAt(*l->art, nodePos, qMax(1e-9, tolNode), &seg) >=
                0) {
                nodeDragging_ = true;
                nodeMoved_ = false;
                nodeLayer_ = hit;
                nodeSeg_ = seg;
                nodeHandleSeg_ = -1;
                nodeWork_ = *l->art;
                QPointF ep;
                {
                    const auto& s = nodeWork_.segments[static_cast<std::size_t>(seg)];
                    ep = QPointF(s.x, s.y);
                }
                nodeGrabDelta_ = ep - nodePos;
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
        }
        // Clicked no point: clear the convert/close selection.
        nodeSelLayer_ = -1;
        nodeSelSeg_ = -1;
        state_->setStatusHint(tr("Node: drag a curve point of a vector shape."));
        viewport()->update();
        event->accept();
        return;
    }

    // Direct Selection: grab one anchor (or handle) and drag it, exactly
    // like the Node tool's grab path; a bare click selects the anchor.
    // Release commits through the shared node-drag path below.
    if (tool == ToolId::DirectSelection && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (l && l->art && !l->art->isEmpty() && l->scaleX > 0.0 &&
            l->scaleY > 0.0) {
            const QPointF src((docPoint.x() - l->offset.x()) / l->scaleX,
                              (docPoint.y() - l->offset.y()) / l->scaleY);
            const QTransform inv =
                QTransform(l->art->matrix[0], l->art->matrix[1],
                           l->art->matrix[2], l->art->matrix[3],
                           l->art->matrix[4], l->art->matrix[5])
                    .inverted();
            const QPointF nodePos = inv.map(src);
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) *
                                   std::hypot(inv.m11(), inv.m12());
            const NodeHandle nh =
                nodeHandleAt(*l->art, nodePos, qMax(1e-9, tolNode));
            if (nh.anchorSeg >= 0) {
                nodeDragging_ = true;
                nodeMoved_ = false;
                nodeLayer_ = hit;
                nodeSeg_ = nh.anchorSeg;
                nodeHandleSeg_ = nh.anchorSeg;
                nodeHandleSide_ = (nh.side == NodeHandleSide::Out) ? 1 : 0;
                nodeHandleMirror_ =
                    !event->modifiers().testFlag(Qt::AltModifier) &&
                    nodeHandlesMirrored(*l->art, nh.anchorSeg,
                                        qMax(1e-9, tolNode * 0.05));
                nodeWork_ = *l->art;
                nodeHandleGrabDelta_ = nh.pos - nodePos;
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
            int seg = -1;
            if (nodeEndpointAt(*l->art, nodePos, qMax(1e-9, tolNode), &seg) >=
                0) {
                nodeDragging_ = true;
                nodeMoved_ = false;
                nodeLayer_ = hit;
                nodeSeg_ = seg;
                nodeHandleSeg_ = -1;
                nodeWork_ = *l->art;
                QPointF ep;
                {
                    const auto& s = nodeWork_.segments[static_cast<std::size_t>(seg)];
                    ep = QPointF(s.x, s.y);
                }
                nodeGrabDelta_ = ep - nodePos;
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
        }
        nodeSelLayer_ = -1;
        nodeSelSeg_ = -1;
        state_->setStatusHint(tr("Direct Selection: drag an anchor point."));
        viewport()->update();
        event->accept();
        return;
    }

    // Vector tools press: starts the creation/modify gesture.
    {
        const QPointF docPoint = viewToDocument(event->position());
        if (inkPress(docPoint)) {
            event->accept();
            return;
        }
    }
    // Vector Brush: press starts the stroke, the stream commits on release
    // as a filled ribbon (one undo step). Width rides the bar's Width and
    // Controller; colour/opacity/blend come from the bar too.
    if (tool == ToolId::VectorBrushTool && event->button() == Qt::LeftButton) {
        vbrushActive_ = true;
        vbrushStroke_.clear();
        vbrushStroke_.addDab(docPoint, vbrushWidthAt(docPoint));
        viewport()->update();
        event->accept();
        return;
    }
    // Gradient / Transparency on art: press-drag defines the fill axis.
    // Pixel layers honestly refuse (their gradient engine is separate).
    if ((tool == ToolId::Gradient || tool == ToolId::TransparencyTool) &&
        event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (!l || !l->art || l->art->isEmpty()) {
            state_->setStatusHint(
                tool == ToolId::Gradient
                    ? tr("Gradient: drag across a vector shape.")
                    : tr("Transparency: drag across a vector shape."));
            event->accept();
            return;
        }
        vgradActive_ = true;
        vgradTool_ = tool;
        vgradLayer_ = hit;
        vgradStartDoc_ = vgradCurDoc_ = docPoint;
        state_->setActiveLayerIndex(hit);
        viewport()->update();
        event->accept();
        return;
    }
    // Corner: click an anchor to round it with the bar Radius (Shift-click
    // sharpens it back). One undo step per click.
    if (tool == ToolId::CornerTool && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        QPointF nodePos;
        if (l && l->art && !l->art->isEmpty() && docToArtNode(hit, docPoint, &nodePos)) {
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) * 2.0;
            int seg = -1;
            if (nodeEndpointAt(*l->art, nodePos, qMax(1e-9, tolNode), &seg) >= 0) {
                pittore::vector::ArtNode work = *l->art;
                if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                    convertNodePoint(work, seg, false);
                    if (state_->applyVectorNode(hit, work, tr("Sharpen")))
                        state_->setStatusHint(tr("Corner: sharpened."));
                } else {
                    double r = state_->option(tool, QStringLiteral("radius")).toDouble();
                    if (!(r > 0.0)) r = 10.0;
                    if (roundNodeCorner(work, seg, r) &&
                        state_->applyVectorNode(hit, work, tr("Round Corner")))
                        state_->setStatusHint(tr("Corner: rounded."));
                    else
                        state_->setStatusHint(tr("Corner: set a Radius first."));
                }
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
        }
        state_->setStatusHint(tr("Corner: click a corner anchor of a vector shape."));
        event->accept();
        return;
    }

    // Add Anchor: click a span to insert a corner anchor there. One undo
    // step per click; the closing edge is not splittable.
    if (tool == ToolId::AddAnchorPoint && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        QPointF nodePos;
        if (l && l->art && !l->art->isEmpty() && docToArtNode(hit, docPoint, &nodePos)) {
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) * 2.0;
            pittore::vector::ArtNode work = *l->art;
            int seg = -1;
            if (insertAnchorPoint(work, nodePos, qMax(1e-9, tolNode), &seg) &&
                state_->applyVectorNode(hit, work, tr("Add Anchor Point"))) {
                state_->setStatusHint(tr("Anchor added."));
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
        }
        state_->setStatusHint(tr("Add Anchor: click a path segment."));
        event->accept();
        return;
    }

    // Delete Anchor: click a corner anchor to remove it (neighbours join
    // straight). One undo step per click.
    if (tool == ToolId::DeleteAnchorPoint && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        QPointF nodePos;
        if (l && l->art && !l->art->isEmpty() && docToArtNode(hit, docPoint, &nodePos)) {
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) * 2.0;
            int seg = -1;
            if (nodeEndpointAt(*l->art, nodePos, qMax(1e-9, tolNode), &seg) >= 0) {
                pittore::vector::ArtNode work = *l->art;
                if (deleteAnchorPoint(work, seg) &&
                    state_->applyVectorNode(hit, work, tr("Delete Anchor Point"))) {
                    state_->setStatusHint(tr("Anchor deleted."));
                    state_->setActiveLayerIndex(hit);
                    viewport()->update();
                    event->accept();
                    return;
                }
                state_->setStatusHint(tr("Delete Anchor: that point cannot go."));
                event->accept();
                return;
            }
        }
        state_->setStatusHint(tr("Delete Anchor: click a corner anchor."));
        event->accept();
        return;
    }

    // Convert Point: click an anchor to toggle it between corner and
    // smooth. One undo step per click.
    if (tool == ToolId::ConvertPoint && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        QPointF nodePos;
        if (l && l->art && !l->art->isEmpty() && docToArtNode(hit, docPoint, &nodePos)) {
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) * 2.0;
            int seg = -1;
            if (nodeEndpointAt(*l->art, nodePos, qMax(1e-9, tolNode), &seg) >= 0) {
                pittore::vector::ArtNode work = *l->art;
                const bool smooth = !isSmoothAnchor(work, seg);
                convertNodePoint(work, seg, smooth);
                if (state_->applyVectorNode(hit, work, tr("Convert Point"))) {
                    state_->setStatusHint(smooth ? tr("Point smoothed.")
                                                 : tr("Point sharpened."));
                    state_->setActiveLayerIndex(hit);
                    viewport()->update();
                    event->accept();
                    return;
                }
            }
        }
        state_->setStatusHint(tr("Convert Point: click an anchor."));
        event->accept();
        return;
    }

    // Contour: click art to offset its outlines by the bar Radius
    // (negative insets). One undo step per click.
    if (tool == ToolId::ContourTool && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (l && l->art && !l->art->isEmpty()) {
            const double r =
                state_->option(tool, QStringLiteral("contour_radius")).toDouble();
            if (!(std::abs(r) > 1e-9)) {
                state_->setStatusHint(tr("Contour: set a Radius first."));
            } else {
                pittore::vector::ArtNode work = *l->art;
                const int join =
                    state_->option(tool, QStringLiteral("contour_type")).toInt();
                if (contourNodePath(work, r, join) &&
                    state_->applyVectorNode(hit, work, tr("Contour")))
                    state_->setStatusHint(tr("Contour: offset applied."));
                else
                    state_->setStatusHint(tr("Contour needs a closed outline."));
            }
            state_->setActiveLayerIndex(hit);
            viewport()->update();
            event->accept();
            return;
        }
        state_->setStatusHint(tr("Contour: click a vector shape."));
        event->accept();
        return;
    }

    // Stroke Width: drag shapes the width profile at the cursor (Shift
    // scales the base width uniformly instead); release commits one step.
    if (tool == ToolId::StrokeWidthTool && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (l && l->art && !l->art->isEmpty() && l->art->paint.hasStroke) {
            swActive_ = true;
            swLayer_ = hit;
            swStartWidth_ = swLiveWidth_ = l->art->paint.strokeWidth;
            swPressDoc_ = docPoint;
            swStartPaint_ = l->art->paint;
            swPreviewPaint_ = l->art->paint;
            swHasPreview_ = false;
            // Flatten once for the t lookup (node space, like the segments).
            swFlat_ = pittore::vector::flattenSegments(
                l->art->segments, 0.25f);
            swTotals_.clear();
            for (const auto& sub : swFlat_.subpaths) {
                double total = 0.0;
                for (std::size_t k = 1; k < sub.size(); ++k)
                    total += std::hypot(sub[k].first - sub[k - 1].first,
                                        sub[k].second - sub[k - 1].second);
                swTotals_.push_back(total);
            }
            state_->setActiveLayerIndex(hit);
            viewport()->update();
            event->accept();
            return;
        }
        state_->setStatusHint(tr("Stroke Width: drag a stroked vector shape."));
        event->accept();
        return;
    }

    // Knife: press-drag draws the cut line across the art.
    if (tool == ToolId::KnifeTool && event->button() == Qt::LeftButton) {
        knifeActive_ = true;
        knifeStartDoc_ = knifeCurDoc_ = docPoint;
        event->accept();
        return;
    }

    // Point Transform: grab an anchor (free move), Shift for scale-about-
    // centroid, Alt for rotate-about-centroid. Release commits one step.
    if (tool == ToolId::PointTransformTool && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        QPointF nodePos;
        if (l && l->art && !l->art->isEmpty() && docToArtNode(hit, docPoint, &nodePos)) {
            const double tolNode = 8.0 / qMax(1e-9, d->zoom) /
                                   qMax(1e-9, qMax(l->scaleX, l->scaleY)) * 2.0;
            int seg = -1;
            if (nodeEndpointAt(*l->art, nodePos, qMax(1e-9, tolNode), &seg) >= 0) {
                ptDragging_ = true;
                ptMoved_ = false;
                ptLayer_ = hit;
                ptSeg_ = seg;
                ptMode_ = event->modifiers().testFlag(Qt::ShiftModifier)
                              ? 1
                              : (event->modifiers().testFlag(Qt::AltModifier) ? 2 : 0);
                ptOrig_ = ptWork_ = *l->art;
                ptCentre_ = nodeAnchorCentroid(ptOrig_);
                ptPressDoc_ = docPoint;
                const auto& s = ptOrig_.segments[static_cast<std::size_t>(seg)];
                ptGrabDelta_ = QPointF(s.x, s.y) - nodePos;
                state_->setActiveLayerIndex(hit);
                viewport()->update();
                event->accept();
                return;
            }
        }
        state_->setStatusHint(tr("Point Transform: drag an anchor point."));
        event->accept();
        return;
    }
    // Vector Flood Fill: click fills the bounded region as a vector layer
    // (Smart refill repaints the hit art instead). One undo step.
    if (tool == ToolId::VectorFloodFillTool && event->button() == Qt::LeftButton) {
        state_->floodFillVectorArt(docPoint);
        refresh();
        event->accept();
        return;
    }

    // Shape Builder: press-drag marquees art; release combines or deletes.
    if (tool == ToolId::ShapeBuilderTool && event->button() == Qt::LeftButton) {
        builderActive_ = true;
        builderStartDoc_ = builderCurDoc_ = docPoint;
        event->accept();
        return;
    }
    // Pen / Freehand / Curvature: press arms the gesture. Clicks add anchors
    // on release, a Pen press-drag shapes Bezier handles live, Freehand
    // streams points until release (which commits). A press with a different
    // pen tool starts a fresh path.
    if (isPenTool(tool) && event->button() == Qt::LeftButton) {
        if (!penActive_ || penTool_ != tool) {
            penPath_.clear();
            penActive_ = true;
            penTool_ = tool;
        }
        penPressed_ = true;
        penPressDoc_ = docPoint;
        penShaping_ = false;
        penHoverDoc_ = docPoint;
        if (tool == ToolId::FreeformPen) {
            penPath_.clear();
            penPath_.setClosed(false);
            QPointF at = docPoint;
            if (state_->option(tool, QStringLiteral("magnetic")).toBool()) {
                QPointF snapped;
                const double tol = 8.0 / qMax(0.01, d->zoom);
                if (snapPenToArt(docPoint, tol, &snapped)) at = snapped;
            }
            penPath_.addFreehand(at, 0.0);
        }
        event->accept();
        return;
    }

    // Note: Alt-click removes an existing note; dragging one moves it (one
    // history step for the whole gesture); a plain click on one edits its text;
    // a click on empty canvas drops a new note.
    if (tool == ToolId::Note && event->button() == Qt::LeftButton) {
        const int hit = noteAtView(event->position());
        if (hit >= 0 && event->modifiers().testFlag(Qt::AltModifier)) {
            state_->beginUndoStep();
            d->notes.removeAt(hit);
            state_->commitUndoStep(tr("Note"), QStringLiteral("note"));
            state_->markAnnotationsChanged();
            refresh();
            return;
        }
        if (hit >= 0) {
            state_->beginUndoStep();
            noteDragging_ = true;
            noteDragged_ = false;
            noteDragIndex_ = hit;
            noteDragGrab_ = docPoint - d->notes[hit].pos;
            noteDragOrig_ = d->notes[hit].pos;
            return;
        }
        const QString author =
            state_->option(tool, QStringLiteral("author")).toString();
        const QColor noteColor =
            state_->option(tool, QStringLiteral("notecolor")).value<QColor>();
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            viewport(), tr("Note"), tr("Note text:"), QString(), &ok);
        if (!ok) return;
        state_->beginUndoStep();
        DocNote note;
        note.pos = docPoint;
        note.text = text;
        note.author = author;
        if (noteColor.isValid()) note.color = noteColor;
        d->notes.append(note);
        state_->commitUndoStep(tr("Note"), QStringLiteral("note"));
        state_->markAnnotationsChanged();
        return;
    }

    // Count: click adds the next numbered marker, Alt-click removes the one
    // under the pointer, drag moves an existing marker.
    if (tool == ToolId::Count && event->button() == Qt::LeftButton) {
        const int hit = countMarkerAtView(event->position());
        if (hit >= 0 && event->modifiers().testFlag(Qt::AltModifier)) {
            state_->beginUndoStep();
            d->countMarkers.removeAt(hit);
            state_->commitUndoStep(tr("Count"), QStringLiteral("count"));
            state_->markAnnotationsChanged();
            refresh();
            return;
        }
        if (hit >= 0) {
            state_->beginUndoStep();
            countDragging_ = true;
            countDragged_ = false;
            countDragIndex_ = hit;
            countDragGrab_ = docPoint - d->countMarkers[hit].pos;
            return;
        }
        state_->beginUndoStep();
        CountMarker marker;
        marker.pos = docPoint;
        marker.group = state_->option(tool, QStringLiteral("group")).toInt();
        d->countMarkers.append(marker);
        state_->commitUndoStep(tr("Count"), QStringLiteral("count"));
        state_->markAnnotationsChanged();
        return;
    }

    // One-shot fills: a click is the whole action, with its own history step.
    if (tool == ToolId::PaintBucket && event->button() == Qt::LeftButton) {
        if (!state_->paintBucketAt(docPoint))
            state_->setStatusHint(tr("Paint Bucket: nothing to fill here."));
        return;
    }
    if (tool == ToolId::MagicEraser && event->button() == Qt::LeftButton) {
        if (!state_->magicEraseAt(docPoint))
            state_->setStatusHint(tr("Magic Eraser: nothing to erase here."));
        return;
    }
    // Magic Wand: one click selects the colour-similar region (tolerance /
    // contiguous / anti-alias / sample size come from the options bar). Shift
    // forces Add, Alt forces Subtract. The selection is committed as a mask.
    if (tool == ToolId::MagicWand && event->button() == Qt::LeftButton) {
        int mode = state_->option(tool, QStringLiteral("selmode")).toInt();
        if (event->modifiers().testFlag(Qt::ShiftModifier)) mode = 1;
        else if (event->modifiers().testFlag(Qt::AltModifier)) mode = 2;
        state_->magicWandSelectAt(docPoint, mode);
        return;
    }

    // Ctrl+click layer pick (any non-Move tool): click an image on the canvas
    // to reveal its layer in the Layers panel (select + scroll to its row),
    // without starting the tool's own gesture (no paint stroke, no marquee).
    // Holding Ctrl with the Move tool already reaches the same selection
    // through beginMoveDrag below (Ctrl = temporary Auto-Select), and the
    // window also pushes a temporary Move tool while Ctrl is held, so this is
    // only the fallback for the race where the press still carries the old
    // tool. Alt is excluded (Clone Stamp / Color Replacement chords).
    if (tool != ToolId::Move && event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::ControlModifier) &&
        !event->modifiers().testFlag(Qt::AltModifier)) {
        pickLayerAt(docPoint);
        event->accept();
        return;
    }

    // Path Selection: click art to select the whole path, drag to move it.
    // Rides the Move tool's drag machinery (auto-select + one history step
    // on release through the shared move path below); transform handles,
    // Alt-duplicate and group logic stay Move-only.
    if (tool == ToolId::PathSelection && event->button() == Qt::LeftButton) {
        const int hit = topPixelLayerAt(*d, docPoint);
        const LayerItem* l =
            (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
        if (!l || !l->art || l->art->isEmpty()) {
            state_->setStatusHint(tr("Path Selection: click a vector shape."));
            event->accept();
            return;
        }
        state_->setActiveLayerIndex(hit);
        if (beginMoveDrag(docPoint)) {
            dragging_ = true;
            dragStartDoc_ = docPoint;
            dragCurrentDoc_ = docPoint;
            state_->beginUndoStep();
        }
        event->accept();
        return;
    }

    if (tool == ToolId::Move && event->button() == Qt::LeftButton) {
        // Transform handles win over the layer body; a missed grab selects
        // nothing and starts no drag.
        if (moveTransformVisible()) {
            const int handle = handleAtView(event->position());
            if (handle >= 0 && beginScaleDrag(handle, docPoint)) {
                // A press that lands within reach of a transform handle starts a
                // SCALE gesture, which never emits move/drag. Without this line a
                // handle grab looks like "no drag happened" while the layer still
                // changes shape — an invisible path next to a body move.
                tool_logf(tool, "move/press", "path=scale handle=%d", handle);
                dragging_ = true;
                dragStartDoc_ = docPoint;
                dragCurrentDoc_ = docPoint;
                state_->beginUndoStep();   // snapshot pre-gesture for undo
                return;
            }
        }
        // Alt+drag duplicates the selection, then the drag moves the fresh
        // copies (conventional behaviour). The copy is its own history step; the
        // move below is another.
        moveDuplicated_ = false;
        if (event->modifiers().testFlag(Qt::AltModifier) &&
            !event->modifiers().testFlag(Qt::ControlModifier)) {
            // Auto-select fires inside beginMoveDrag below: pick first so the
            // copy is of the layer under the cursor, not the old active one.
            const bool autoOpt = state_->option(ToolId::Move,
                                                QStringLiteral("autoselect"))
                                     .toBool();
            const bool ctrlH =
                event->modifiers().testFlag(Qt::ControlModifier);
            const bool shiftH =
                event->modifiers().testFlag(Qt::ShiftModifier);
            if (autoOpt || ctrlH || shiftH) {
                const int idx0 =
                    qBound(0, d->activeLayer, d->layers.size() - 1);
                const bool inGroup =
                    d->layers[idx0].kind == LayerItem::Kind::Group &&
                    state_->groupBounds(idx0).contains(docPoint);
                if (!inGroup) {
                    int hit = topPixelLayerAt(*d, docPoint);
                    if (hit < 0) return;   // empty canvas: nothing to copy
                    const int gg = state_->outerGroupContaining(hit);
                    if (gg >= 0) hit = gg;
                    if (!d->selectedLayers.contains(hit)) {
                        d->selectedLayers.clear();
                        d->selectedLayers.push_back(hit);
                    }
                    if (d->activeLayer != hit)
                        state_->setActiveLayerIndex(hit);
                    else
                        emit state_->activeLayerChanged();
                }
            }
            if (state_->duplicateSelectedLayers()) {
                moveDuplicated_ = true;
                tool_logf(tool, "move/press", "path=duplicate");
                // Re-resolve: duplication re-selected the copies.
                d = doc();
                if (!d) return;
            } else {
                state_->setStatusHint(
                    tr("Duplicate needs a layer to copy — Alt+drag cancelled."));
                return;
            }
        }
        if (beginMoveDrag(docPoint)) {
            dragging_ = true;
            dragStartDoc_ = docPoint;
            dragCurrentDoc_ = docPoint;
            state_->beginUndoStep();   // snapshot pre-gesture for undo
        }
        return;
    }

    // Type tools: a click inside the run being edited moves the caret and
    // starts a drag-selection; a click on another text layer resumes that one;
    // anywhere else starts a new layer. A click makes artistic text, a drag
    // sizes a frame box (the box is only fixed on release).
    // Type Mask: click starts a live text session like the Type tools;
    // committing (Enter/Escape/click-away) rasterizes the typed run into
    // a selection mask instead of keeping a text layer. Vertical shares
    // the horizontal layout until the text engine models vertical flow.
    if ((tool == ToolId::HorizontalTypeMask ||
         tool == ToolId::VerticalTypeMask) &&
        event->button() == Qt::LeftButton) {
        commitTextEdit();
        textMaskMode_ = (tool == ToolId::VerticalTypeMask) ? 2 : 1;
        textCreating_ = true;
        textCreateStartDoc_ = docPoint;
        textCreateSize_ = 0.0;
        textCreateFrame_ = false;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        viewport()->setFocus(Qt::MouseFocusReason);
        viewport()->update();
        return;
    }

    if (isTypeTool(tool) && event->button() == Qt::LeftButton) {
        const int hit = state_->textLayerAt(docPoint);
        if (textEditing_ && (hit == textEditIndex_ || textPointInRun(docPoint))) {
            textSelecting_ = true;
            // A double-click's second press arrives as a click with the caret
            // already placed; extend keeps the anchor.
            setTextCaret(textOffsetAtDoc(docPoint),
                         event->modifiers().testFlag(Qt::ShiftModifier));
            viewport()->setFocus(Qt::MouseFocusReason);
            return;
        }
        commitTextEdit();
        if (hit >= 0) {
            startTextEdit(hit);
            return;
        }
        textCreating_ = true;
        textCreateStartDoc_ = docPoint;
        textCreateSize_ = 0.0;
        textCreateFrame_ = false;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        viewport()->setFocus(Qt::MouseFocusReason);
        viewport()->update();
        return;
    }

    // Freehand lasso (Lasso + Magnetic): press-drag streams a loop.
    // Magnetic snaps each point to the strongest nearby edge.
    if ((tool == ToolId::Lasso || tool == ToolId::MagneticLasso) &&
        event->button() == Qt::LeftButton) {
        lassoLive_ = true;
        lassoStroke_.clear();
        QPointF at = (tool == ToolId::MagneticLasso) ? magneticSnap(docPoint)
                                                     : docPoint;
        lassoStroke_.push_back(at);
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        liveViewRect_ = QRectF();
        viewport()->update();
        event->accept();
        return;
    }

    // Polygonal lasso: click-click anchors, rubber-banded until close.
    // Clicking near the first anchor with 3+ points commits.
    if (tool == ToolId::PolygonalLasso && event->button() == Qt::LeftButton) {
        const double tol = 8.0 / qMax(0.01, d->zoom);
        if (polyPts_.size() >= 3 &&
            QLineF(docPoint, polyPts_.front()).length() <= tol) {
            lassoCommitPolygon(polyPts_, event->modifiers());
            polyPts_.clear();
            polyHoverOn_ = false;
            viewport()->update();
            event->accept();
            refresh();
            return;
        }
        polyPts_.push_back(docPoint);
        polyHover_ = docPoint;
        polyHoverOn_ = true;
        state_->setStatusHint(
            tr("Polygonal Lasso: click to add points, click the start to "
               "close, double-click or Enter to finish, Esc to cancel."));
        viewport()->update();
        event->accept();
        return;
    }

    // Selection brush: press-drag paints an incoming coverage mask.
    if (tool == ToolId::SelectionBrush && event->button() == Qt::LeftButton) {
        if (!d->size.isEmpty()) {
            selBrushLive_ = true;
            selBrushMask_ =
                QImage(d->size, QImage::Format_Grayscale8);
            selBrushMask_.fill(0);
            selBrushLast_ = docPoint;
            const double radius =
                qMax(0.5, state_->option(tool, QStringLiteral("brush_size"))
                              .toDouble() *
                          0.5);
            const double hardness =
                state_->option(tool, QStringLiteral("hardness")).toDouble() /
                100.0;
            const double opacity =
                state_->option(tool, QStringLiteral("opacity")).toDouble() /
                100.0;
            selBrushPaintTo(docPoint, radius,
                            hardness >= 0.0 ? hardness : 1.0,
                            opacity > 0.0 ? opacity : 1.0);
            dragging_ = true;
            dragStartDoc_ = docPoint;
            dragCurrentDoc_ = docPoint;
            marqueeMods_ = event->modifiers();
            liveViewRect_ = QRectF();
            viewport()->update();
            event->accept();
            return;
        }
    }

    // Crop: the frame is a pending canvas edit, not a selection. The press
    // picks the gesture from the hit test — resize from a handle, reposition
    // from inside, rubber-band a new frame from outside. Commit is a separate
    // step (Enter / double-click / Apply), so nothing here touches history.
    if (tool == ToolId::Crop && event->button() == Qt::LeftButton) {
        cropPress(docPoint, event->position());
        viewport()->update();
        event->accept();
        return;
    }

    if (isSelectionTool(tool)) {
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        marqueeMods_ = event->modifiers();
        liveViewRect_ = QRectF();
        return;
    }

    // Shape tools: press-drag draws the box; release builds the live shape.
    if (isShapeTool(tool) && event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        shapeAnchorDoc_ = docPoint;
        viewport()->update();
        return;
    }
    // Liquify: a warp stroke is one history entry, committed on release.
    if (tool == ToolId::Liquify && event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        beginLiquifyStroke(docPoint);
        if (!liquifyActive_) dragging_ = false;   // nothing warppable
        return;
    }

    // Color Replacement Alt+click: lock the area's whole colour range (up to
    // 48 targets from a multi-sampled grid over the brush disc) as the match
    // set. Later strokes then replace exactly those colours with the
    // foreground, wherever they are painted. Never starts a stroke: no undo
    // step, no dab. Re-pick to replace the lock; switching tools clears it.
    // (On desktops whose window manager swallows Alt+click, the options-bar
    // Lock Area button does the same at the hover point.)
    if (tool == ToolId::ColorReplacement && event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::AltModifier)) {
        lockReplacePaletteAt(docPoint);
        event->accept();
        return;
    }

    // Clone Stamp Alt+click: pin the source point the stamp copies from.
    // Never starts a stroke. Aligned strokes keep their offset across
    // presses; non-aligned ones re-derive it from this anchor every press.
    // The Healing Brush shares the same Alt-source anchor.
    if ((tool == ToolId::CloneStamp || tool == ToolId::HealingBrush) &&
        event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::AltModifier)) {
        cloneAltPoint_ = docPoint;
        cloneHasAlt_ = true;
        cloneHasOffset_ = false;  // next press re-derives the offset
        state_->setStatusHint(tool == ToolId::CloneStamp
                                  ? tr("Clone Stamp: source set — paint to copy.")
                                  : tr("Healing Brush: source set — paint to heal."));
        event->accept();
        return;
    }

    // Painting tools: a stroke is one history entry, committed on release.
    if (isPaintTool(tool) && event->button() == Qt::LeftButton) {
        dragging_ = true;
        strokeActive_ = true;
        strokePainted_ = false;
        replaceStrokeHasTargets_ = false;  // Once-sampling re-arms per stroke
        if (tool == ToolId::CloneStamp || tool == ToolId::HealingBrush) {
            // Without a source there is nothing to stamp: refuse the stroke
            // before any undo step begins.
            if (!cloneHasAlt_) {
                dragging_ = false;
                strokeActive_ = false;
                state_->setStatusHint(
                    tool == ToolId::CloneStamp
                        ? tr("Clone Stamp: Alt-click to set the source first.")
                        : tr("Healing Brush: Alt-click to set the source first."));
                return;
            }
            // Non-aligned re-anchors every press (repeat stamping); aligned
            // keeps the first press's offset until the next Alt-click. The
            // offset points from dab to source (engine samples dab + offset).
            const bool aligned = state_->option(tool, QStringLiteral("aligned"))
                                     .toBool();
            if (!aligned || !cloneHasOffset_) {
                cloneOffset_ = cloneAltPoint_ - docPoint;
                cloneHasOffset_ = true;
            }
        }
        strokeLastDoc_ = docPoint;
        strokeLastPressure_ = std::clamp(pixelPressure_, 0.0, 1.0);
        strokeDabCount_ = 0;
        strokeSmoothDoc_ = docPoint;
        strokeLastRawDoc_ = docPoint;
        strokeSpeed_ = 0.0;
        strokeDist_ = 0.0;
        strokeMaxP_ = std::clamp(pixelPressure_, 0.0, 1.0);
        strokePressMs_ = event->timestamp();
        strokeElapsedMs_ = 0;
        strokeLastMoveMs_ = event->timestamp();
        dragStartDoc_ = docPoint;
        dragCurrentDoc_ = docPoint;
        // Per-stroke brush state (RNG seed, smudge paint, grain origin)
        // starts before the first dab reads it.
        state_->beginStrokeState(tool);
        // Fuzzy-stroke clock: one hash from the finished seed, stable for
        // the whole stroke without consuming the stroke RNG.
        fuzzyStrokeH01_ = hash01(state_->strokeSeed());
        // Undo step + copy-on-write BEFORE the first dab: the pending snapshot
        // shares the layer's current image, so a dab must mutate a private
        // clone or undoing the stroke would show its own result.
        state_->beginUndoStep();
        state_->copyOnWriteActiveLayer();
        state_->copyOnWriteActiveMask();
        state_->copyOnWriteActiveHeight();
        refreshDabOpts();
        paintSymmetricAt(docPoint);
        state_->flushPaint();
        startAirbrush(tool);
        return;
    }

    // Painting tools: a stroke is one history entry.
    dragging_ = true;
    dragStartDoc_ = docPoint;
    dragCurrentDoc_ = docPoint;
}

}  // namespace pittore::ui
