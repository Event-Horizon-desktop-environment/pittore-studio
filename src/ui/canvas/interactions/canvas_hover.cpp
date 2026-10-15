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
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


void CanvasView::leaveEvent(QEvent* event) {
    cursorInViewport_ = false;
    hoverSettleTimer_->stop();
    if (hoverPreviewActive_) {
        hoverPreviewActive_ = false;
        hoverPreviewMask_ = QImage();
        hoverPreviewPoint_ = QPoint();
        hoverPreviewExpanded_ = false;
        viewport()->update();
    }
    QAbstractScrollArea::leaveEvent(event);
}


QPainterPath CanvasView::selectionOutlinePath() {
    DocumentItem* d = doc();
    if (!d || !d->selectionIsMask || d->selectionMask.isNull() ||
        d->selection.isEmpty())
        return QPainterPath();
    if (selectionOutlineStamp_ == d->selectionStamp) return selectionOutline_;
    QPainterPath outline = pittore::ui::selectionOutlineFromMask(
        d->selectionMask, d->selection.toAlignedRect().adjusted(-1, -1, 1, 1));
    // The raw tracer is pixel-exact but reads as jagged noise at small zooms:
    // smooth it for display (spike strip + collinear collapse + Catmull-Rom).
    selectionOutline_ = pittore::ui::selectionOutlineSmoothed(outline);
    selectionOutlineStamp_ = d->selectionStamp;
    return selectionOutline_;
}


void CanvasView::mouseDoubleClickEvent(QMouseEvent* event) {
    DocumentItem* d = doc();
    if (!d || event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    // A Type session is already live: double-clicking a different text layer
    // moves the session there (caret at the click); otherwise it selects the
    // word under the cursor.
    if (textEditing_) {
        const QPointF docPoint = viewToDocument(event->position());
        const int hit = state_->textLayerAt(docPoint);
        if (hit >= 0 && hit != textEditIndex_) {
            commitTextEdit();
            if (d->layers[hit].locked) {
                state_->setStatusHint(tr("Text layer is locked."));
                event->accept();
                return;
            }
            state_->setActiveLayerIndex(hit);
            startTextEdit(hit);
            setTextCaret(textOffsetAtDoc(docPoint), false);
        } else {
            selectWordAt(textOffsetAtDoc(docPoint));
        }
        event->accept();
        return;
    }
    const ToolId tool = state_->activeTool();
    // Polygonal lasso: double-click closes the loop.
    if (tool == ToolId::PolygonalLasso && polyPts_.size() >= 3) {
        // The double-click's two presses added a duplicated anchor; drop a
        // trailing near-duplicate so the corner stays clean.
        if (polyPts_.size() >= 2 &&
            QLineF(polyPts_.back(),
                   polyPts_[polyPts_.size() - 2])
                    .length() < 2.0)
            polyPts_.pop_back();
        lassoCommitPolygon(polyPts_, event->modifiers());
        polyPts_.clear();
        polyHoverOn_ = false;
        event->accept();
        viewport()->update();
        refresh();
        return;
    }
    // Double-click finishes an open Pen/Curvature path (Freehand commits on
    // release, so it never has a working path here).
    if (penActive_ && isPenTool(tool) && tool != ToolId::FreeformPen) {
        finishPenPath();
        event->accept();
        refresh();
        return;
    }
    // Double-click with Stroke Width resets the profile back to uniform.
    if (tool == ToolId::StrokeWidthTool) {
        state_->resetStrokeProfile();
        event->accept();
        refresh();
        return;
    }
    // Double-click commits an armed crop frame (Enter and the task bar's
    // Apply do the same).
    if (tool == ToolId::Crop) {
        commitCrop();
        event->accept();
        refresh();
        return;
    }
    // Double-click with an armed Perspective Crop quad commits the warp.
    if (tool == ToolId::PerspectiveCrop && pcropArmed_) {
        const int outW = state_->option(tool, QStringLiteral("w")).toInt();
        const int outH = state_->option(tool, QStringLiteral("h")).toInt();
        if (state_->perspectiveCrop(pcropQuad_, outW, outH)) {
            pcropArmed_ = false;
            pcropCorner_ = -1;
        }
        event->accept();
        refresh();
        return;
    }
    // Double-clicking a live text layer drops the caret into it from any
    // tool: with Move that is how you go from "selected" to "editing" —
    // and the double-click also promotes Move to the Type tool (standard
    // editor behaviour), so the options bar shows the run being edited.
    // Every other tool edits in place without switching away. Locked layers
    // refuse with a hint instead of opening a session that could mutate
    // them.
    if (handleTextDoubleClick(viewToDocument(event->position()))) {
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}


bool CanvasView::handleTextDoubleClick(const QPointF& docPos) {
    DocumentItem* d = doc();
    if (!d || textEditing_) return false;
    const int hit = state_->textLayerAt(docPos);
    if (hit < 0 || hit >= d->layers.size()) return false;
    if (d->layers[hit].locked) {
        state_->setStatusHint(tr("Text layer is locked."));
        return true;
    }
    // standard editors-style: double-clicking text with the Move tool
    // promotes it to the Type tool — the options bar, Character panel and
    // I-beam cursor all come alive with the run being edited. The switch
    // must precede startTextEdit because toolChanged commits any prior
    // text session. Every other tool still edits in place without switching
    // away; the Type group is visible in every persona, so the toolbar is
    // never stranded.
    if (state_->activeTool() == ToolId::Move)
        state_->setActiveTool(ToolId::HorizontalType, "text-dblclick");
    state_->setActiveLayerIndex(hit);
    startTextEdit(hit);
    refresh();
    return true;
}

}  // namespace pittore::ui
