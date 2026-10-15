#include "ui/canvas_view.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
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
#include <QScreen>
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
#include "ui/canvas/trace/canvas_trace.h"
#include "ui/persona/vector_node.h"
#include "engine/vector/boolean.h"

namespace pittore::ui {


CanvasView::CanvasView(AppState* state, QWidget* parent)
    : QAbstractScrollArea(parent), state_(state) {
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    // Right-click on the navigation tools is handled by CanvasView::contextMenuEvent
    // (the zoom/rotate menu, R14/R101); the viewport must not eat the event first.
    viewport()->setContextMenuPolicy(Qt::NoContextMenu);
    setViewportMargins(kRulerSize, kRulerSize, 0, 0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    horizontalRuler_ = new CanvasRuler(this, Qt::Horizontal, this);
    verticalRuler_ = new CanvasRuler(this, Qt::Vertical, this);
    rulerCorner_ = new QWidget(this);

    taskBar_ = new ContextualTaskBar(state_, viewport());
    taskBar_->resetPosition();

    // Image files dropped on the viewport are placed as new layers (a document
    // is opened for them when none exists). The viewport gets the events, so
    // it — not this widget — is the drop target we filter.
    viewport()->setAcceptDrops(true);
    viewport()->installEventFilter(this);

    // The Type tool types directly on the canvas, so the viewport has to be
    // able to take keyboard focus (the rest of the app drives the canvas with
    // window-wide shortcuts, which still work when the viewport holds focus).
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusPolicy(Qt::StrongFocus);

    textCaretTimer_ = new QTimer(this);
    textCaretTimer_->setInterval(530);
    connect(textCaretTimer_, &QTimer::timeout, this, [this] {
        if (!textEditing_) return;
        textCaretOn_ = !textCaretOn_;
        viewport()->update();
    });

    antsTimer_ = new QTimer(this);
    antsTimer_->setInterval(90);    connect(antsTimer_, &QTimer::timeout, this, [this] {
        antsPhase_ = (antsPhase_ + 1) % 8;
        // Hidden ants (Extras off) march silently: no repaint while there
        // is nothing to draw, so a live selection costs nothing unseen.
        if (!extras_) return;
        // Repaint only the ants' own strip, not the whole 2000×2000 canvas:
        // a full-viewport repaint here costs 4–5 ms eleven times a second.
        QRectF vr;
        if (DocumentItem* d = doc()) {
            vr = documentTransform().mapRect(d->selection).adjusted(-5, -5, 5, 5);
            if (dragging_ && isSelectionTool(state_->activeTool()))
                vr |= documentTransform()
                          .mapRect(QRectF(dragStartDoc_, dragCurrentDoc_).normalized())
                          .adjusted(-5, -5, 5, 5);
        }
        viewport()->update(vr.isEmpty() ? viewport()->rect() : vr.toAlignedRect());
    });

    // Airbrush metronome (created once; started/stopped per stroke).
    airbrushTimer_ = new QTimer(this);
    connect(airbrushTimer_, &QTimer::timeout, this, &CanvasView::airbrushTick);

    // Object Select: when the pointer stops moving, upgrade the cheap live
    // preview to the full multi-point expansion so the blue overlay shows the
    // whole object, not the sub-part a single SAM point prompt returns.
    hoverSettleTimer_ = new QTimer(this);
    hoverSettleTimer_->setSingleShot(true);
    hoverSettleTimer_->setInterval(150);
    connect(hoverSettleTimer_, &QTimer::timeout, this, &CanvasView::onHoverSettle);

    connect(state_, &AppState::activeDocumentChanged, this, [this] {
        // Each tab carries its own zoom and scroll, so two screenshots taken
        // on different tabs are two different views of the same file — named
        // here so a position that "changed by itself" can be told apart from a
        // view that changed tabs under it.
        DocumentItem* td = doc();
        canvasTraceDoc(state_->activeTool(), state_->activeDocumentIndex(),
                       td ? td->title : QString(), td ? td->zoom : 0.0);
        canvasNoteUpdate("doc");
        pendingFit_ = true;
        applyPendingFit();
        updateScrollRange();
        // A brush tool's cursor decision flips with a document opening/closing
        // (the ring only exists over an open document).
        applyToolCursor();
        // The crop frame belongs to the document it was drawn on: switching
        // documents arms a fresh frame over the new one (empty when there is
        // none, which the overlay paints as nothing).
        if (state_->activeTool() == ToolId::Crop) cropArmForDocument();
        refresh();
    });
    connect(state_, &AppState::documentModified, this, &CanvasView::refresh);
    // Soft-proof toggles/setup: full repaint (the blit path itself decides
    // raw vs proofed).
    connect(state_, &AppState::proofChanged, this, &CanvasView::refresh);
    // Cursor preferences only touch the overlay: re-sync the OS cursor and
    // repaint the old ∪ new ring area (no composite work).
    connect(state_, &AppState::settingsChanged, this, [this] {
        if (!brushCursorRect_.isEmpty())
            viewport()->update(brushCursorRect_.adjusted(-2, -2, 2, 2).toAlignedRect());
        brushCursorRect_ = brushCursorViewRect();
        if (!brushCursorRect_.isEmpty())
            viewport()->update(brushCursorRect_.adjusted(-2, -2, 2, 2).toAlignedRect());
        syncCursorOverride();
        if (cursorToolActive()) viewport()->setCursor(canvasCursor());
    });
    // Cheap edits (layer visibility toggle) refresh only the changed document
    // rect as a viewport update — the composite is already updated, so there
    // is no full-viewport repaint behind the eye click.
    connect(state_, &AppState::regionModified, this,
            [this](DocumentItem* d, const QRectF& docRect) {
        if (doc() != d || docRect.isEmpty()) return;
        const QRectF v =
            documentTransform().mapRect(docRect)
                .adjusted(-4, -4, 4, 4)
                .intersected(QRectF(viewport()->rect()));
        // Named even when nothing gets scheduled: a region that maps entirely
        // outside the viewport is a repaint that never came, and the screen
        // keeps the previous frame for exactly that region.
        canvasTraceUpdate(state_->activeTool(),
                          v.isEmpty() ? "region-outside" : "region", docRect,
                          v.toAlignedRect());
        if (v.isEmpty()) return;
        canvasNoteUpdate("region");
        viewport()->update(v.toAlignedRect());
    });
    connect(state_, &AppState::layersChanged, this, [this] {
        // A Character-panel or options-bar edit re-sets the run, so the caret
        // and selection overlays must be re-mapped onto the new layout.
        if (textEditing_) refreshTextLayout();
        refresh();
    });
    // Selection-only layer change: redraw the transform gizmo on its new
    // layer without a composite rebuild or panel storm.
    connect(state_, &AppState::activeLayerChanged, this,
            [this] { viewport()->update(); });
    connect(state_, &AppState::selectionChanged, this, [this] {
        DocumentItem* d = doc();
        if (d && !d->selection.isEmpty()) antsTimer_->start();
        else antsTimer_->stop();
        refresh();
    });
    connect(state_, &AppState::toolChanged, this, [this](ToolId id) {
        // Leaving Liquify mid-stroke commits what has been warped so far (one
        // history entry), so a stray tool shortcut never strands the snapshot.
        // Each hop is a separate line in the incoming tool's own log: these
        // three commit paths are the ones that touch document state during a
        // tool switch.
        tool_log(id, "canvas/commit-liquify-stroke", "leaving previous tool");
        commitLiquifyStrokeIfActive();
        // Leaving the Type tool ends any text session the same way clicking
        // away does (empty layers are dropped).
        tool_log(id, "canvas/commit-text-edit", "ending any text session");
        commitTextEdit();
        // The Replace Color palette lock belongs to that tool: switching away
        // drops it so a stale 48-colour set can never ambush the next tool.
        replacePaletteLocked_ = false;
        replacePaletteTargets_.clear();
        tool_log(id, "canvas/apply-tool-cursor", "cursor + brush ring");
        applyToolCursor();
        // Entering Crop arms its frame over the whole document (standard
        // crop-tool behaviour: the frame appears the moment the tool is
        // picked, ready to drag in or resize from a handle).
        if (id == ToolId::Crop) {
            tool_log(id, "canvas/crop-arm", "arming crop frame");
            cropArmForDocument();
        }
        // Entering Rotate View seeds its angle field with the live rotation.
        if (state_->activeTool() == ToolId::RotateView) {
            tool_log(id, "canvas/rotate-view-seed", "seeding angle");
            DocumentItem* d = doc();
            state_->setOptionSilently(ToolId::RotateView, QStringLiteral("angle"),
                                      d ? d->rotation : 0.0);
            tool_log(id, "canvas/rotate-view-seed", "done");
        }
        tool_log(id, "canvas/tool-changed", "done");
    });
    // The Rotate View angle field drives the canvas directly; brush-size and
    // hardness edits resize the brush-cursor ring in place (no pointer move
    // needed).
    connect(state_, &AppState::optionChanged, this,
            [this](ToolId tool, const QString& id, const QVariant& value) {
        if (tool == state_->activeTool() &&
            (id == QLatin1String("brush_size") || id == QLatin1String("brush_hardness"))) {
            if (!brushCursorRect_.isEmpty())
                viewport()->update(brushCursorRect_.adjusted(-2, -2, 2, 2).toAlignedRect());
            brushCursorRect_ = brushCursorViewRect();
            if (!brushCursorRect_.isEmpty())
                viewport()->update(brushCursorRect_.adjusted(-2, -2, 2, 2).toAlignedRect());
        }
        // Node convert combo (Sharp/Smooth/Smart): applies to the
        // click-selected anchor the moment it changes. Silent when nothing
        // is selected or a drag is in flight.
        if (tool == ToolId::NodeTool && id == QLatin1String("convert") &&
            !nodeDragging_ && nodeSelLayer_ >= 0 && nodeSelSeg_ >= 0) {
            DocumentItem* dd = doc();
            const LayerItem* l =
                (dd && nodeSelLayer_ < dd->layers.size())
                    ? &dd->layers[nodeSelLayer_]
                    : nullptr;
            if (l && l->art && !l->art->isEmpty()) {
                pittore::vector::ArtNode work = *l->art;
                convertNodePoint(work, nodeSelSeg_, value.toInt() != 0);
                if (state_->applyVectorNode(nodeSelLayer_, work, tr("Convert"))) {
                    state_->setStatusHint(value.toInt() == 0
                                              ? tr("Node: corner point.")
                                              : tr("Node: smooth point."));
                    refresh();
                }
            }
        }
        // Path Selection path-op combo (Combine/Subtract/Intersect/Exclude/
        // Merge): applies to the selected art the moment it changes.
        if (tool == ToolId::PathSelection && id == QLatin1String("pathop")) {
            QVector<int> targets;
            if (DocumentItem* dd = doc()) {
                for (int i : state_->selectedLayerIndices()) {
                    if (i >= 0 && i < dd->layers.size() && dd->layers[i].art &&
                        !dd->layers[i].art->isEmpty())
                        targets.push_back(i);
                }
            }
            if (targets.size() >= 2) {
                const int mode = value.toInt();
                const pittore::vector::BoolOp op =
                    mode == 1   ? pittore::vector::BoolOp::Difference
                    : mode == 2 ? pittore::vector::BoolOp::Intersection
                    : mode == 3 ? pittore::vector::BoolOp::Xor
                                : pittore::vector::BoolOp::Union;
                state_->booleanFoldLayers(targets, op, nullptr,
                                          mode == 1   ? tr("Subtract")
                                          : mode == 2 ? tr("Intersect")
                                          : mode == 3 ? tr("Exclude")
                                                      : tr("Combine"));
                refresh();
            } else {
                state_->setStatusHint(
                    tr("Select two vector shapes to combine."));
            }
        }
        if (tool != ToolId::RotateView || id != QLatin1String("angle")) return;
        DocumentItem* d = doc();
        if (d && std::fabs(d->rotation - value.toDouble()) > 1e-4)
            setRotation(value.toDouble());
    });
    connect(state_, &AppState::surroundChanged, this, [this] { refresh(); });
    connect(state_, &AppState::themeChanged, this, [this] { refresh(); });
    connect(state_, &AppState::quickMaskChanged, this, [this] { refresh(); });

    // Mixed-DPR multi-monitor: a screen appearing/vanishing (dock/undock)
    // can change which DPR the canvas paints at — repaint so tile buckets
    // (keyed by zoom × DPR) and the DPR-keyed checker rebake promptly.
    // Window moves between existing screens are caught by the paint-path
    // DPR sync in paintDocument (lastPaintDpr_).
    connect(qApp, &QGuiApplication::screenAdded, this,
            [this](QScreen*) { refresh(); });
    connect(qApp, &QGuiApplication::screenRemoved, this,
            [this](QScreen*) { refresh(); });

    applyToolCursor();
}

}  // namespace pittore::ui
