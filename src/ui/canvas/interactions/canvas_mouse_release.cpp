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
#include "ui/persona/vector_gradient.h"
#include "ui/persona/vector_point_ops.h"

namespace pittore::ui {


void CanvasView::mouseReleaseEvent(QMouseEvent* event) {
    DocumentItem* d = doc();

    // Pairs with input/mouse-press in the active tool's own log.
    tool_logf(state_->activeTool(), "input/mouse-release", "button=0x%x mods=0x%x",
              static_cast<int>(event->button()), static_cast<int>(event->modifiers()));

    // Alt+Right-drag brush resize ends on release (no undo: it only
    // retargets the live options, like the [ ] keys). Ends on ANY release
    // while active so the gesture can never stick, and swallows the
    // right-release context menu that would otherwise pop up.
    if (brushResizeActive_) {
        brushResizeActive_ = false;
        brushResizeSuppressMenu_ = true;
        state_->setStatusHint(QString());
        viewport()->update();
        event->accept();
        return;
    }

    // Scrubby zoom ends on release; a press without a drag is the
    // classic step zoom in/out. Ends on any release while scrubbing so a
    // missed left-release can never leave the zoom glued to the pointer.
    if (zoomScrubbing_) {
        zoomScrubbing_ = false;
        dragging_ = false;
        if (!zoomScrubMoved_ && event->button() == Qt::LeftButton) {
            const ToolId tool = state_->activeTool();
            const bool out =
                event->modifiers().testFlag(Qt::AltModifier) ||
                state_->option(tool, QStringLiteral("zoommode")).toInt() == 1;
            const double before = zoom();
            out ? zoomOut() : zoomIn();
            if (!qFuzzyCompare(before, zoom()))
                setZoom(zoom(), event->position());
        }
        viewport()->update();
        event->accept();
        return;
    }

    // Finishing a caret/selection drag: nothing to commit (a text edit commits
    // its own undo step per change).
    if (textSelecting_) {
        textSelecting_ = false;
        event->accept();
        return;
    }

    // Type tool: fix the box the press started. A click (no meaningful drag)
    // makes artistic text at the point; a drag makes a frame text box. The
    // layer is created inside the same undo step the editing session owns, so
    // one Undo removes the layer and everything typed into it.
    if (textCreating_) {
        textCreating_ = false;
        textCreateFrame_ = false;
        if (d && event->button() == Qt::LeftButton) {
            const ToolId tool = state_->activeTool();
            const QRectF box = QRectF(textCreateStartDoc_, dragCurrentDoc_).normalized();
            const QString family =
                typeFamilyForIndex(state_->option(tool, QStringLiteral("family")).toInt());
            const QColor color =
                state_->option(tool, QStringLiteral("color")).value<QColor>();
            const double optionSize =
                state_->option(tool, QStringLiteral("size")).toDouble();
            // A plain drag "draws an A": the drag distance sets the font size
            // (artistic text). Shift-drag sizes a frame box instead.
            const bool frame = event->modifiers().testFlag(Qt::ShiftModifier) &&
                               box.width() > 6.0 && box.height() > 6.0;
            double size = optionSize;
            if (!frame && textCreateSize_ > 4.0)
                size = qBound(1.0, textCreateSize_, 1296.0);
            textCreateSize_ = 0.0;
            state_->beginUndoStep();
            const int index =
                frame ? state_->addTextLayer(box.topLeft(), box.width(), family, size, color)
                      : state_->addTextLayer(textCreateStartDoc_, 0.0, family, size, color);
            if (index < 0) {
                state_->discardUndoStep();
            } else {
                if (frame)
                    state_->activeDocument()->layers[index].textSpec.frameHeight =
                        box.height();
                startTextEdit(index, /*undoBegan=*/true);
            }
        }
        event->accept();
        refresh();
        return;
    }

    // Rotate View ends with the drag; rotation is a view property, no history.
    if (rotating_) {
        rotating_ = false;
        event->accept();
        return;
    }

    // Slice: release commits the drawn rectangle as one history step (a drag
    // smaller than 2×2 px — i.e. a click — is dropped).
    if (sliceDragging_) {
        sliceDragging_ = false;
        if (d && event->button() == Qt::LeftButton) {
            const QRectF viewRect = QRectF(sliceDragAnchorDoc_, sliceDragCurrentDoc_).normalized();
            const QRect slice = viewRect.toAlignedRect()
                                    .intersected(QRect(0, 0, d->size.width(), d->size.height()));
            if (slice.width() >= 2 && slice.height() >= 2) {
                state_->beginUndoStep();
                d->slices.append(slice);
                d->selectedSlice = d->slices.size() - 1;
                state_->commitUndoStep(tr("Slice"), QStringLiteral("slice"));
                state_->markAnnotationsChanged();
            }
        }
        viewport()->update();
        event->accept();
        return;
    }

    // Frame: release commits the placeholder box as a vector shape layer.
    if (frameDragging_) {
        frameDragging_ = false;
        dragging_ = false;
        if (d && event->button() == Qt::LeftButton) {
            const QRectF box =
                QRectF(frameAnchorDoc_, dragCurrentDoc_).normalized().intersected(
                    QRectF(QPointF(0, 0), QSizeF(d->size)));
            if (box.width() >= 3.0 && box.height() >= 3.0) {
                const ToolId shape =
                    state_->option(ToolId::Frame, QStringLiteral("frameshape"))
                                .toInt() == 1
                            ? ToolId::Ellipse
                            : ToolId::Rectangle;
                state_->addVectorShapeLayer(shape, box, tr("Frame"));
            }
        }
        viewport()->update();
        event->accept();
        return;
    }

    // Artboard: release resizes the canvas (dragged rect, or the W/H
    // options on a click) and paints exposed paper with the well colour.
    if (artboardDragging_) {
        artboardDragging_ = false;
        dragging_ = false;
        if (d && event->button() == Qt::LeftButton) {
            const QRectF box =
                QRectF(artboardAnchorDoc_, dragCurrentDoc_).normalized();
            bool ok = false;
            if (box.width() >= 3.0 && box.height() >= 3.0) {
                ok = state_->resizeCanvas(
                    box.topLeft(),
                    QSize(std::max(1, int(std::round(box.width()))),
                          std::max(1, int(std::round(box.height())))));
            } else {
                const int w = std::clamp(
                    state_->option(ToolId::Artboard, QStringLiteral("w"))
                        .toInt(),
                    1, 100000);
                const int h = std::clamp(
                    state_->option(ToolId::Artboard, QStringLiteral("h"))
                        .toInt(),
                    1, 100000);
                ok = state_->resizeCanvas(QPointF(0, 0), QSize(w, h));
            }
            if (ok) {
                const QColor bg =
                    state_
                        ->option(ToolId::Artboard, QStringLiteral("artboardbg"))
                        .value<QColor>();
                if (bg.isValid() && bg != d->canvasPaper) {
                    d->canvasPaper = bg;
                    d->rebuildComposite();
                }
                state_->commitUndoStep(tr("Artboard"),
                                       QStringLiteral("artboard"));
                state_->setStatusHint(
                    tr("Artboard: canvas %1 × %2.")
                        .arg(d->size.width())
                        .arg(d->size.height()));
            } else {
                state_->discardUndoStep();
            }
        } else {
            state_->discardUndoStep();
        }
        viewport()->update();
        event->accept();
        refresh();
        return;
    }

    // Vector drag tools: release commits the gesture (one undo step).
    if (inkActive_) {
        inkRelease(viewToDocument(event->position()));
        event->accept();
        return;
    }

    // Perspective Crop: release arms the quad (rect corners, or the moved
    // corner which is already live). Double-click commits the warp.
    if (state_->activeTool() == ToolId::PerspectiveCrop && dragging_) {
        dragging_ = false;
        if (d && event->button() == Qt::LeftButton && pcropCorner_ < 0) {
            const QRectF box =
                QRectF(pcropAnchorDoc_, dragCurrentDoc_).normalized();
            if (box.width() >= 3.0 && box.height() >= 3.0) {
                pcropQuad_[0] = box.topLeft();
                pcropQuad_[1] = box.topRight();
                pcropQuad_[2] = box.bottomRight();
                pcropQuad_[3] = box.bottomLeft();
                pcropArmed_ = true;
                state_->setStatusHint(
                    tr("Perspective Crop: drag corners, double-click to "
                       "apply."));
            }
        }
        pcropCorner_ = -1;
        viewport()->update();
        event->accept();
        refresh();
        return;
    }

    // Slice Select: a move drag commits in one history step (opened on press,
    // so Undo restores the pre-drag position); a click with no movement drops
    // the step and just leaves the selection.
    if (sliceSelectDragging_) {
        sliceSelectDragging_ = false;
        if (d && sliceSelectMoved_ && sliceSelectIndex_ >= 0 &&
            sliceSelectIndex_ < d->slices.size()) {
            state_->commitUndoStep(tr("Move Slice"), QStringLiteral("slice"));
            state_->markAnnotationsChanged();
        } else {
            state_->discardUndoStep();
        }
        sliceSelectMoved_ = false;
        event->accept();
        return;
    }

    // Patch: phase 1 commits the marquee as the selection (same Alt/Shift
    // semantics as the marquee tools, via marqueeLiveRect); phase 2 heals
    // the original area from the drop area in one history step (opened on
    // press, so Undo restores the pre-drag pixels).
    if (patchPhase_ != 0 && state_->activeTool() == ToolId::Patch) {
        const int phase = patchPhase_;
        patchPhase_ = 0;
        dragging_ = false;
        if (d && phase == 1 && event->button() == Qt::LeftButton) {
            marqueeMods_ = event->modifiers();
            const QRectF mrect = marqueeLiveRect().intersected(
                QRectF(QPointF(0, 0), QSizeF(d->size)));
            if (mrect.width() > 1 && mrect.height() > 1) {
                state_->setSelection(mrect, false);
                state_->pushHistory(toolName(ToolId::Patch),
                                    QString::fromUtf8(
                                        toolDef(ToolId::Patch).iconKey));
            } else {
                state_->clearSelection();
            }
        } else if (d && phase == 2 && event->button() == Qt::LeftButton) {
            const QPointF relDoc = viewToDocument(event->position());
            const QPointF delta = relDoc - patchPressDoc_;
            bool ok = false;
            if (patchMoved_ &&
                (std::fabs(delta.x()) > 0.5 || std::fabs(delta.y()) > 0.5)) {
                const int mode = std::clamp(
                    state_->option(ToolId::Patch, QStringLiteral("patchmode"))
                        .toInt(),
                    0, 1);
                const int structure = std::clamp(
                    state_->option(ToolId::Patch, QStringLiteral("structure"))
                        .toInt(),
                    1, 7);
                const int colorMix = std::clamp(
                    state_->option(ToolId::Patch, QStringLiteral("colorblend"))
                        .toInt(),
                    0, 10);
                const bool sampleAll =
                    state_
                        ->option(ToolId::Patch, QStringLiteral("sample_all"))
                        .toBool();
                ok = state_->patchTransfer(delta, mode, structure, colorMix,
                                           sampleAll);
                if (!ok)
                    state_->setStatusHint(
                        tr("Patch: drag onto a donor area with texture."));
            }
            if (ok)
                state_->commitUndoStep(
                    toolName(ToolId::Patch),
                    QString::fromUtf8(toolDef(ToolId::Patch).iconKey));
            else
                state_->discardUndoStep();
        } else if (phase == 2) {
            // Phase 1 never opened an undo step (selection commits its own
            // history entry); only the drag phase must drop its snapshot.
            state_->discardUndoStep();
        }
        patchMoved_ = false;
        event->accept();
        refresh();
        return;
    }

    // ContentAwareMove: phase 1 commits the marquee as the selection;
    // phase 2 moves the selected content to the drop point and heals the
    // vacated hole, in one history step (snapshot opened on press).
    if (camPhase_ != 0 && state_->activeTool() == ToolId::ContentAwareMove) {
        const int phase = camPhase_;
        camPhase_ = 0;
        dragging_ = false;
        if (d && phase == 1 && event->button() == Qt::LeftButton) {
            marqueeMods_ = event->modifiers();
            const QRectF mrect = marqueeLiveRect().intersected(
                QRectF(QPointF(0, 0), QSizeF(d->size)));
            if (mrect.width() > 1 && mrect.height() > 1) {
                state_->setSelection(mrect, false);
                state_->pushHistory(toolName(ToolId::ContentAwareMove),
                                    QString::fromUtf8(
                                        toolDef(ToolId::ContentAwareMove).iconKey));
            } else {
                state_->clearSelection();
            }
        } else if (d && phase == 2 && event->button() == Qt::LeftButton) {
            const QPointF relDoc = viewToDocument(event->position());
            const QPointF delta = relDoc - camPressDoc_;
            bool ok = false;
            if (camMoved_ &&
                (std::fabs(delta.x()) > 0.5 || std::fabs(delta.y()) > 0.5)) {
                const bool extend =
                    state_
                        ->option(ToolId::ContentAwareMove,
                                 QStringLiteral("camode"))
                        .toInt() == 1;
                const int structure = std::clamp(
                    state_->option(ToolId::ContentAwareMove,
                                   QStringLiteral("structure"))
                        .toInt(),
                    1, 7);
                const int colorMix = std::clamp(
                    state_->option(ToolId::ContentAwareMove,
                                   QStringLiteral("colorblend"))
                        .toInt(),
                    0, 10);
                const bool sampleAll =
                    state_
                        ->option(ToolId::ContentAwareMove,
                                 QStringLiteral("sample_all"))
                        .toBool();
                ok = state_->contentAwareMove(delta, extend, structure,
                                              colorMix, sampleAll);
                if (!ok)
                    state_->setStatusHint(
                        tr("Content-Aware Move: drag onto a textured area."));
            }
            if (ok)
                state_->commitUndoStep(
                    toolName(ToolId::ContentAwareMove),
                    QString::fromUtf8(
                        toolDef(ToolId::ContentAwareMove).iconKey));
            else
                state_->discardUndoStep();
        } else if (phase == 2) {
            // Phase 1 never opened an undo step; only the drag phase must
            // drop its snapshot.
            state_->discardUndoStep();
        }
        camMoved_ = false;
        event->accept();
        refresh();
        return;
    }

    // Ruler: a real drag commits the measurement, a bare click clears it.
    // Measure shares the gesture; only the history name and readout differ.
    if (rulerDragging_) {
        rulerDragging_ = false;
        const bool measuring = state_->activeTool() == ToolId::MeasureTool;
        if (d) {
            const double len = QLineF(d->rulerStart, d->rulerEnd).length();
            if (len < 1.0) {
                d->rulerHasMeasurement = false;
                state_->discardUndoStep();
            } else {
                state_->commitUndoStep(measuring ? tr("Measure") : tr("Ruler"),
                                       QStringLiteral("ruler"));
                if (measuring) updateMeasureHint();
            }
            state_->markAnnotationsChanged();
        } else {
            state_->discardUndoStep();
        }
        refresh();
        event->accept();
        return;
    }

    // Area: a real rectangle commits the measurement, a bare click clears it.
    if (areaDragging_) {
        areaDragging_ = false;
        if (d) {
            if (d->areaRect.width() < 1.0 || d->areaRect.height() < 1.0) {
                d->areaHasMeasurement = false;
                state_->discardUndoStep();
            } else {
                state_->commitUndoStep(tr("Area"), QStringLiteral("ruler"));
                updateAreaHint();
            }
            state_->markAnnotationsChanged();
        } else {
            state_->discardUndoStep();
        }
        refresh();
        event->accept();
        return;
    }

    // Red Eye: fix the boxed pupil (a click uses the Pupil Size box).
    // Reset dragging_ first so the generic section below never sees it.
    if (redeyeDragging_) {
        redeyeDragging_ = false;
        dragging_ = false;
        if (d && event->button() == Qt::LeftButton) {
            QRectF box = QRectF(dragStartDoc_, dragCurrentDoc_).normalized();
            if (box.width() < 3.0 && box.height() < 3.0) {
                const double half =
                    8.0 + state_->option(ToolId::RedEye,
                                         QStringLiteral("pupilsize"))
                                  .toDouble() *
                              0.48;
                box = QRectF(dragStartDoc_ - QPointF(half, half),
                             QSizeF(half * 2.0, half * 2.0));
            }
            const double darken =
                state_->option(ToolId::RedEye, QStringLiteral("darken"))
                    .toDouble() /
                100.0;
            state_->redEyeAt(box, darken);
        }
        refresh();
        event->accept();
        return;
    }

    // Generative Fill / Background: the marked area becomes the selection
    // the Generate button consumes (same Alt/Shift semantics as marquee).
    if (genMarking_) {
        genMarking_ = false;
        dragging_ = false;
        if (d && event->button() == Qt::LeftButton) {
            marqueeMods_ = event->modifiers();
            const QRectF mrect = marqueeLiveRect().intersected(
                QRectF(QPointF(0, 0), QSizeF(d->size)));
            if (mrect.width() > 1 && mrect.height() > 1) {
                state_->setSelection(mrect, false);
                state_->pushHistory(toolName(state_->activeTool()),
                                    QString::fromUtf8(
                                        toolDef(state_->activeTool()).iconKey));
                state_->setStatusHint(
                    tr("Set a prompt and press Generate."));
            } else {
                state_->clearSelection();
            }
        }
        refresh();
        event->accept();
        return;
    }

    // Content-Aware Tracing: the drag becomes the selection, then traces
    // it per the output option (Shape drops a vector layer, Selection
    // rewrites the mask, Path honestly refuses).
    if (traceMarking_) {
        traceMarking_ = false;
        dragging_ = false;
        if (d && event->button() == Qt::LeftButton) {
            marqueeMods_ = event->modifiers();
            const QRectF mrect = marqueeLiveRect().intersected(
                QRectF(QPointF(0, 0), QSizeF(d->size)));
            if (mrect.width() > 1 && mrect.height() > 1) {
                state_->setSelection(mrect, false);
                state_->pushHistory(toolName(state_->activeTool()),
                                    QString::fromUtf8(
                                        toolDef(state_->activeTool()).iconKey));
                const int output = std::clamp(
                    state_
                        ->option(ToolId::ContentAwareTracing,
                                 QStringLiteral("output"))
                        .toInt(),
                    0, 2);
                const int detail = std::clamp(
                    state_
                        ->option(ToolId::ContentAwareTracing,
                                 QStringLiteral("detail"))
                        .toInt(),
                    0, 100);
                state_->traceSelection(output, detail);
            } else {
                state_->clearSelection();
            }
        }
        refresh();
        event->accept();
        return;
    }

    // Node: release commits the drag as one re-raster step; a bare click
    // only selected the anchor (convert/close target, no history entry).
    if (nodeDragging_) {
        nodeDragging_ = false;
        const int layer = nodeLayer_;
        const int seg = (nodeHandleSeg_ >= 0) ? nodeHandleSeg_ : nodeSeg_;
        nodeLayer_ = -1;
        nodeSeg_ = -1;
        nodeHandleSeg_ = -1;
        if (d && nodeMoved_ && layer >= 0) {
            state_->applyVectorNode(layer, nodeWork_, tr("Node"));
            state_->setStatusHint(tr("Node: point moved."));
        } else if (d && layer >= 0 && seg >= 0) {
            nodeSelLayer_ = layer;
            nodeSelSeg_ = seg;
            state_->setStatusHint(tr("Node: point selected — convert or drag."));
        }
        nodeMoved_ = false;
        refresh();
        event->accept();
        return;
    }

    // Shape Builder: release combines or deletes the marqueed art. One
    // undo step; a click picks the topmost layer.
    if (builderActive_ && state_->activeTool() == ToolId::ShapeBuilderTool) {
        builderActive_ = false;
        const QRectF box =
            QRectF(builderStartDoc_, builderCurDoc_).normalized();
        const int action =
            state_->option(ToolId::ShapeBuilderTool,
                           QStringLiteral("builder_action")).toInt();
        state_->shapeBuilderAt(box, qBound(0, action, 2));
        refresh();
        event->accept();
        return;
    }

    // Knife: release cuts the art under the press point. One undo step per
    // cut gesture (0 cuts = no history).
    if (knifeActive_ && state_->activeTool() == ToolId::KnifeTool) {
        knifeActive_ = false;
        if (d) {
            QPointF a = knifeStartDoc_, b = knifeCurDoc_;
            if (state_->option(ToolId::KnifeTool, QStringLiteral("straight_line"))
                    .toBool()) {
                // Snap the cut to 15° increments around the press point.
                const double dx = b.x() - a.x(), dy = b.y() - a.y();
                const double len = std::hypot(dx, dy);
                if (len > 1e-9) {
                    const double step = 3.14159265358979323846 / 12.0;
                    const double ang =
                        std::round(std::atan2(dy, dx) / step) * step;
                    b = a + QPointF(std::cos(ang) * len, std::sin(ang) * len);
                }
            }
            const double viewLen =
                std::hypot(b.x() - a.x(), b.y() - a.y()) * qMax(0.01, d->zoom);
            if (viewLen >= 4.0) {
                // Prefer the art under the press point; otherwise take the
                // first art the cut segment actually crosses, so a cut that
                // starts off-shape still works as the hint promises.
                int hit = topPixelLayerAt(*d, a);
                auto hasArt = [&](int i) {
                    return i >= 0 && i < d->layers.size() &&
                           d->layers[i].art &&
                           !d->layers[i].art->isEmpty();
                };
                if (!hasArt(hit)) {
                    hit = -1;
                    for (int k = 1; k <= 32 && hit < 0; ++k) {
                        const QPointF p =
                            a + (b - a) * (double(k) / 32.0);
                        const int h = topPixelLayerAt(*d, p);
                        if (hasArt(h)) hit = h;
                    }
                }
                const LayerItem* l =
                    (hit >= 0 && hit < d->layers.size()) ? &d->layers[hit] : nullptr;
                QPointF na, nb;
                if (l && l->art && !l->art->isEmpty() && docToArtNode(hit, a, &na) &&
                    docToArtNode(hit, b, &nb)) {
                    pittore::vector::ArtNode work = *l->art;
                    const int smooth =
                        state_->option(ToolId::KnifeTool, QStringLiteral("smoothness"))
                            .toInt();
                    const double tol = 1.0 - qBound(0, smooth, 100) / 100.0 * 0.9;
                    const int closeMode =
                        state_->option(ToolId::KnifeTool, QStringLiteral("auto_close"))
                            .toInt();
                    const int cuts = knifeCutNode(
                        work, na, nb, qBound(0.1, tol, 1.0), qBound(0, closeMode, 3));
                    if (cuts > 0 &&
                        state_->applyVectorNode(hit, work, tr("Knife"))) {
                        state_->setStatusHint(
                            tr("Knife: %1 cut%2.").arg(cuts).arg(cuts == 1 ? QString() : tr("s")));
                    } else if (cuts == 0) {
                        state_->setStatusHint(tr("Knife: the cut missed the outline."));
                    }
                    state_->setActiveLayerIndex(hit);
                } else {
                    state_->setStatusHint(tr("Knife: drag across a vector shape."));
                }
            }
        }
        refresh();
        event->accept();
        return;
    }

    // Stroke Width: release commits the working paint (local profile or
    // Shift-uniform base width) as one paint step.
    if (swActive_ && state_->activeTool() == ToolId::StrokeWidthTool) {
        swActive_ = false;
        if (d && swLayer_ >= 0 && swLayer_ < d->layers.size() &&
            d->layers[swLayer_].art) {
            pittore::vector::ArtPaint paint;
            QString hint;
            if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
                paint = swStartPaint_;
                paint.strokeWidth = swLiveWidth_;
                hint = tr("Stroke width: %1 px.").arg(swLiveWidth_, 0, 'f', 1);
            } else if (swHasPreview_) {
                paint = swPreviewPaint_;
                hint = tr("Width profile updated.");
            } else {
                // Click without drag: nothing shaped.
                swLayer_ = -1;
                refresh();
                event->accept();
                return;
            }
            if (state_->applyVectorPaint(swLayer_, paint, -1.0,
                                         tr("Stroke Width")))
                state_->setStatusHint(hint);
        }
        swLayer_ = -1;
        swHasPreview_ = false;
        refresh();
        event->accept();
        return;
    }

    // Point Transform: release commits the working copy (one step); a bare
    // click only selected the layer.
    if (ptDragging_ && state_->activeTool() == ToolId::PointTransformTool) {
        ptDragging_ = false;
        const int layer = ptLayer_;
        ptLayer_ = -1;
        ptSeg_ = -1;
        if (d && ptMoved_ && layer >= 0) {
            state_->applyVectorNode(layer, ptWork_, tr("Point Transform"));
            state_->setStatusHint(tr("Point transformed."));
        }
        ptMoved_ = false;
        refresh();
        event->accept();
        return;
    }

    // Gradient / Transparency: release maps the axis to node space and
    // commits one paint step. A bare click defines no axis (no history).
    if (vgradActive_) {
        vgradActive_ = false;
        const ToolId tool = state_->activeTool();
        if (d && (tool == ToolId::Gradient || tool == ToolId::TransparencyTool) &&
            vgradLayer_ >= 0 && vgradLayer_ < d->layers.size()) {
            const double viewLen =
                std::hypot(vgradCurDoc_.x() - vgradStartDoc_.x(),
                           vgradCurDoc_.y() - vgradStartDoc_.y()) *
                qMax(0.01, d->zoom);
            QPointF ns, ne;
            if (viewLen < 4.0) {
                state_->setStatusHint(tr("Drag to define the gradient axis."));
            } else if (!docToArtNode(vgradLayer_, vgradStartDoc_, &ns) ||
                       !docToArtNode(vgradLayer_, vgradCurDoc_, &ne)) {
                state_->setStatusHint(tr("The shape moved; draw the axis again."));
            } else {
                const auto paint = d->layers[vgradLayer_].art
                                       ? d->layers[vgradLayer_].art->paint
                                       : pittore::vector::ArtPaint();
                if (tool == ToolId::Gradient) {
                    const int preset =
                        state_->option(tool, QStringLiteral("preset")).toInt();
                    const int type =
                        state_->option(tool, QStringLiteral("gradtype")).toInt();
                    const bool reverse = state_
                                             ->option(tool, QStringLiteral("reverse"))
                                             .toBool();
                    const auto stops = gradientPresetStops(
                        preset, state_->foreground(), state_->background(),
                        reverse);
                    if (state_->applyVectorPaint(
                            vgradLayer_,
                            paintWithGradient(paint, ns, ne, type == 1, stops),
                            -1.0, tr("Gradient"))) {
                        if (type != 0 && type != 1)
                            state_->setStatusHint(
                                tr("Gradient applied (type approximated as linear)."));
                        else
                            state_->setStatusHint(tr("Gradient applied."));
                    }
                } else {
                    const int type =
                        state_
                            ->option(tool, QStringLiteral("transparency_type"))
                            .toInt();
                    const bool reverse = state_
                                             ->option(tool, QStringLiteral("reverse"))
                                             .toBool();
                    if (state_->applyVectorPaint(
                            vgradLayer_,
                            paintWithTransparency(paint, ns, ne, type, reverse,
                                                  state_->foreground()),
                            -1.0, tr("Transparency"))) {
                        if (type == 4)
                            state_->setStatusHint(
                                tr("Transparency applied (conical approximated "
                                   "as linear)."));
                        else
                            state_->setStatusHint(tr("Transparency applied."));
                    }
                }
            }
        }
        vgradLayer_ = -1;
        refresh();
        event->accept();
        return;
    }

    // Vector Brush: release commits the streamed ribbon as one filled layer.
    // A bare click still leaves its dot.
    if (vbrushActive_ &&
        state_->activeTool() == ToolId::VectorBrushTool) {
        vbrushActive_ = false;
        if (d && !vbrushStroke_.isEmpty()) {
            QColor color = state_->option(ToolId::VectorBrushTool,
                                          QStringLiteral("color"))
                               .value<QColor>();
            if (!color.isValid()) color = state_->foreground();
            double opacity =
                state_
                    ->option(ToolId::VectorBrushTool,
                             QStringLiteral("brush_opacity"))
                    .toDouble();
            if (!(opacity > 0.0)) opacity = 100.0;
            QString blend;
            const int modeIdx = state_
                                    ->option(ToolId::VectorBrushTool,
                                             QStringLiteral("mode"))
                                    .toInt();
            const QStringList& modes = blendModeNames();
            if (modeIdx >= 0 && modeIdx < modes.size() &&
                !blendModeIsSeparatorBefore(modeIdx) &&
                !modes.at(modeIdx).isEmpty())
                blend = modes.at(modeIdx);
            double nibRatio, nibAngle;
            bool nibSquare;
            vbrushNib(nibRatio, nibAngle, nibSquare);
            state_->addVectorBrushLayer(
                vbrushStroke_.buildRibbon(nibRatio, nibAngle, nibSquare),
                color, opacity / 100.0, blend,
                toolName(ToolId::VectorBrushTool));
        }
        vbrushStroke_.clear();
        refresh();
        event->accept();
        return;
    }

    // Pen / Freehand / Curvature release. Freehand commits the streamed path
    // (auto-closing near its start). Pen/Curvature: a press-drag already
    // shaped a smooth point live; a click adds a corner (or closes/finishes).
    if (penPressed_ && penActive_) {
        penPressed_ = false;
        const ToolId tool = state_->activeTool();
        const QPointF relDoc = viewToDocument(event->position());
        if (d && isPenTool(tool)) {
            const double tol = 8.0 / qMax(0.01, d->zoom);
            if (tool == ToolId::FreeformPen) {
                if (penPath_.size() >= 3 && penPath_.closeHit(relDoc, tol))
                    penPath_.setClosed(true);
                finishPenPath();
            } else if (penShaping_) {
                penPath_.demoteLastToCorner(3.0 / qMax(0.01, d->zoom));
                penShaping_ = false;
                state_->setStatusHint(tr("%1: click to add points, click the "
                                         "first point to close, double-click "
                                         "or Enter to finish.")
                                          .arg(toolName(tool)));
            } else if (penPath_.closeHit(relDoc, tol)) {
                penPath_.setClosed(true);
                finishPenPath();
            } else {
                QPointF at = relDoc;
                if (state_->option(tool, QStringLiteral("magnetic")).toBool()) {
                    QPointF snapped;
                    if (snapPenToArt(relDoc, tol, &snapped)) at = snapped;
                }
                penPath_.addCorner(at);
                state_->setStatusHint(tr("%1: click to add points, click the "
                                         "first point to close, double-click "
                                         "or Enter to finish.")
                                          .arg(toolName(tool)));
            }
        }
        penShaping_ = false;
        refresh();
        event->accept();
        return;
    }

    // Count: commit a moved marker, or drop the step when it never moved.
    if (countDragging_) {
        countDragging_ = false;
        if (d && countDragged_)
            state_->commitUndoStep(tr("Count"), QStringLiteral("count"));
        else
            state_->discardUndoStep();
        countDragIndex_ = -1;
        countDragged_ = false;
        state_->markAnnotationsChanged();
        refresh();
        event->accept();
        return;
    }

    // Note: a drag commits the move; a click without movement opens the note's
    // text for editing (its own history step).
    if (noteDragging_) {
        noteDragging_ = false;
        if (d && noteDragged_) {
            state_->commitUndoStep(tr("Note"), QStringLiteral("note"));
        } else {
            state_->discardUndoStep();
            if (d && noteDragIndex_ >= 0 && noteDragIndex_ < d->notes.size()) {
                const QString author = state_->option(
                    ToolId::Note, QStringLiteral("author")).toString();
                const QColor noteColor = state_->option(
                    ToolId::Note, QStringLiteral("notecolor")).value<QColor>();
                bool ok = false;
                const QString text = QInputDialog::getMultiLineText(
                    viewport(), tr("Note"), tr("Note text:"),
                    d->notes[noteDragIndex_].text, &ok);
                if (ok) {
                    state_->beginUndoStep();
                    d->notes[noteDragIndex_].text = text;
                    if (!author.isEmpty()) d->notes[noteDragIndex_].author = author;
                    if (noteColor.isValid()) d->notes[noteDragIndex_].color = noteColor;
                    state_->commitUndoStep(tr("Note"), QStringLiteral("note"));
                }
            }
        }
        noteDragIndex_ = -1;
        noteDragged_ = false;
        state_->markAnnotationsChanged();
        refresh();
        event->accept();
        return;
    }

    if (panning_) {
        panning_ = false;
        applyToolCursor();
        return;
    }
    if (!dragging_ || !d) return;
    dragging_ = false;

    const ToolId tool = state_->activeTool();

    // Brush strokes commit on release: one history entry for the whole stroke.
    if (strokeActive_) {
        strokeActive_ = false;
        if (airbrushTimer_) airbrushTimer_->stop();
        // Remove: heal the marked area first so the commit below covers
        // the fill, not just the invisible marks. The generative checkbox
        // falls back to the healing fill (no model bundled yet).
        if (tool == ToolId::Remove && strokePainted_) {
            const bool removeAfter = state_->option(
                                         tool, QStringLiteral("remove_after_stroke"))
                                         .toBool();
            bool filled = false;
            if (state_->option(tool, QStringLiteral("generative")).toBool()) {
                filled = state_->generativeFill(false);
                if (!filled)
                    state_->setStatusHint(
                        tr("Remove: healing fill (no generative model)."));
            }
            if (!state_->option(tool, QStringLiteral("generative")).toBool() ||
                !filled) {
                const bool sampleAll =
                    state_->option(tool, QStringLiteral("sample_all")).toBool();
                state_->removeHealMarked(removeAfter, sampleAll, 4);
            }
        }
        // Wash strokes fold their scratch buffer into the layer first, so
        // the committed snapshot holds the finished stroke.
        state_->bakeWashStroke();
        // Finalize retained CMYK ink fingerprints before the post-stroke
        // snapshot is captured; redo must restore valid native samples too.
        state_->endStrokeState();
        if (strokePainted_)
            state_->commitUndoStep(toolName(tool),
                                   QString::fromUtf8(toolDef(tool).iconKey));
        else
            state_->discardUndoStep();   // click with no dab: no history
        // Dodge/Burn/Sponge accumulate stroke-wide coverage; the pre-stroke
        // snapshot and coverage buffer are only needed while the stroke runs.
        // Clone Stamp keeps the same per-stroke accumulation alive.
        state_->endToneStroke();
        state_->endCloneStroke();
        state_->endHealStroke();
        if (tool == ToolId::Remove)
            state_->endRemoveStroke(
                !state_->option(tool, QStringLiteral("remove_after_stroke"))
                     .toBool());
        strokeLastDoc_ = QPointF();
        event->accept();
        return;
    }

    // Liquify stroke commits on release: one history entry for the whole warp.
    if (liquifyActive_) {
        finishLiquifyStroke(true);
        event->accept();
        return;
    }

    // Move/scale drags commit on release: one history entry per gesture, and
    // only when the pointer actually moved the layer.
    if (moveDragging_ || scaleDragging_) {
        moveDragging_ = false;
        scaleDragging_ = false;
        moveDuplicated_ = false;
        activeHandle_ = -1;
        moveSnap_ = MoveSnap{};   // smart guides vanish with the drag
        applyToolCursor();
        if (moveChanged_)
            state_->commitUndoStep(toolName(tool),
                                   QString::fromUtf8(toolDef(tool).iconKey));
        else
            state_->discardUndoStep();   // press without movement: no history
        const bool moved = moveChanged_;
        moveChanged_ = false;
        // Vector-direct refresh: moved art re-renders from geometry at the
        // final footprint (post-commit refinement of the resampled drag
        // preview — same visual state, no extra history step).
        if (moved) state_->refreshVectorArt();
        event->accept();
        refresh();
        return;
    }

    // Object Selection with the AI pair model + Object Finder: prompt the SAM
    // decoder at the pointer and select the object instead of the generic drag
    // rectangle (conventional Object Select behaviour). The hover preview was
    // living on the canvas; the click commits it.
    if (tool == ToolId::ObjectSelection &&
        state_->option(tool, QStringLiteral("object_finder")).toBool() &&
        runAiObjectSelect(dragCurrentDoc_.toPoint())) {
        hoverPreviewActive_ = false;
        hoverPreviewMask_ = QImage();
        event->accept();
        refresh();
        return;
    }

    // Quick Selection: the AI segmentation under the stroke is combined into
    // the selection (New/Add/Subtract from the options bar; Shift = Add,
    // Alt = Subtract). One undoable step for the whole gesture.
    if (tool == ToolId::QuickSelection) {
        int mode = state_->option(tool, QStringLiteral("selmode")).toInt();
        if (event->modifiers().testFlag(Qt::ShiftModifier)) mode = 1;
        else if (event->modifiers().testFlag(Qt::AltModifier)) mode = 2;
        runAiQuickSelect(dragStartDoc_, dragCurrentDoc_, mode);
        event->accept();
        refresh();
        return;
    }

    const QRectF rect = QRectF(dragStartDoc_, dragCurrentDoc_)
                            .normalized()
                            .intersected(QRectF(QPointF(0, 0), QSizeF(d->size)));

    // Freehand lasso: the streamed loop becomes a polygon selection.
    if ((tool == ToolId::Lasso || tool == ToolId::MagneticLasso) &&
        lassoLive_) {
        lassoLive_ = false;
        marqueeMods_ = event->modifiers();
        std::vector<QPointF> loop = lassoStroke_;
        lassoStroke_.clear();
        if (loop.size() >= 3) {
            lassoCommitPolygon(loop, marqueeMods_);
        } else if (lassoSelMode(marqueeMods_) == 0) {
            state_->clearSelection();
        }
        event->accept();
        refresh();
        return;
    }

    // Selection brush: the painted incoming mask combines through selmode.
    if (tool == ToolId::SelectionBrush && selBrushLive_) {
        selBrushLive_ = false;
        marqueeMods_ = event->modifiers();
        QImage incoming = selBrushMask_;
        selBrushMask_ = QImage();
        const int mode = lassoSelMode(marqueeMods_);
        if (!incoming.isNull() &&
            !selectionMaskBbox(incoming).isEmpty()) {
            state_->combineSelection(std::move(incoming), mode,
                                     toolName(tool),
                                     QString::fromUtf8(
                                         toolDef(tool).iconKey));
        } else if (mode == 0) {
            state_->clearSelection();
        }
        event->accept();
        refresh();
        return;
    }

    // Polygonal lasso never commits on release: anchors accumulate on press,
    // double-click or Enter closes the loop.
    if (tool == ToolId::PolygonalLasso) {
        event->accept();
        viewport()->update();
        return;
    }

    if (isSelectionTool(tool)) {
        // Alt = from center, Shift = square: resolved live so the commit
        // matches the rubber band exactly. The release event's own modifiers
        // win over the last move (keys may change on the way up).
        marqueeMods_ = event->modifiers();
        const QRectF mrect = marqueeLiveRect().intersected(
            QRectF(QPointF(0, 0), QSizeF(d->size)));
        if (mrect.width() > 1 && mrect.height() > 1) {
            state_->setSelection(mrect, tool == ToolId::EllipseMarquee);
            state_->pushHistory(toolName(tool), QString::fromUtf8(toolDef(tool).iconKey));
        } else {
            state_->clearSelection();
        }
    } else if (tool == ToolId::Crop) {
        if (rect.width() > 1 && rect.height() > 1) state_->setSelection(rect, false);
    } else if (tool == ToolId::VectorCropTool) {
        // Non-destructive object crop: the keep-rectangle becomes a reveal
        // mask on the active layer (outside hides, mask stays editable).
        if (rect.width() > 1 && rect.height() > 1) {
            state_->setSelection(rect, false);
            if (state_->maskRevealSelection()) {
                state_->clearSelection();
                state_->setStatusHint(
                    tr("Vector Crop: cropped — edit the layer mask to adjust."));
            }
        } else {
            state_->clearSelection();
        }
    } else if (isShapeTool(tool)) {
        // Live shape creation: drag box, or click with W/H set for a fixed
        // size centred on the press. One undoable layer, still vector inside.
        QRectF box = rect;
        const double fw =
            state_->option(tool, QStringLiteral("w")).toDouble();
        const double fh =
            state_->option(tool, QStringLiteral("h")).toDouble();
        const bool lineLike = tool == ToolId::Line;
        const bool bigEnough =
            lineLike ? (box.width() >= 3.0 || box.height() >= 3.0)
                     : (box.width() >= 3.0 && box.height() >= 3.0);
        if (!bigEnough && fw > 0.0 && fh > 0.0) {
            const QPointF c = dragStartDoc_;
            box = QRectF(c.x() - fw / 2.0, c.y() - fh / 2.0, fw, fh)
                      .intersected(QRectF(QPointF(0, 0), QSizeF(d->size)));
        }
        const bool ready =
            lineLike ? (box.width() >= 3.0 || box.height() >= 3.0)
                     : (box.width() >= 3.0 && box.height() >= 3.0);
        // Landing behaviour: the new shape stays selected under the Move tool,
        // transform handles live, ready to scale any which way. Keep Selected
        // off collapses back to no layer selection instead.
        if (ready && state_->addVectorShapeLayer(tool, box, toolName(tool))) {
            if (!state_->option(tool, QStringLiteral("keep_selected")).toBool())
                state_->clearLayerSelection();
            state_->setActiveTool(ToolId::Move);
        }
    } else if (rect.width() > 0.5 || rect.height() > 0.5) {
        state_->pushHistory(toolName(tool), QString::fromUtf8(toolDef(tool).iconKey));
    }

    event->accept();
    refresh();
}

}  // namespace pittore::ui
