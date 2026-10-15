// Crop tool: the interactive crop frame. A standard editor's crop is a
// two-phase gesture — a live frame the user shapes (draw, resize from the
// eight handles, reposition from inside), then a commit that resizes the
// canvas in one undo step with the content staying put. The frame is view
// state, never the document selection: nothing in the document changes
// until the crop is applied (Enter, double-click, or the task bar's Apply).
//
// The canvas model cannot grow a document, so the frame is clamped to the
// document bounds at every step: a crop never exposes paper outside the
// original pixels.

#include "ui/canvas_view.h"

#include <QKeyEvent>
#include <QLine>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "ui/tools/log/tool_log.h"

namespace pittore::ui {

namespace {

// Smallest frame the user can leave behind (document px): below this the
// frame would be ungrabbable at any sane zoom.
constexpr double kMinCrop = 4.0;
// Handle grab radius in view px — the same 7 px the move-tool gizmo uses.
constexpr double kHandleTol = 7.0;

}  // namespace

QPointF CanvasView::cropHandlePos(int handle, const QRectF& rect) {
    const double cx = rect.center().x(), cy = rect.center().y();
    switch (handle) {
        case 0: return rect.topLeft();
        case 1: return QPointF(cx, rect.top());
        case 2: return rect.topRight();
        case 3: return QPointF(rect.right(), cy);
        case 4: return rect.bottomRight();
        case 5: return QPointF(cx, rect.bottom());
        case 6: return rect.bottomLeft();
        default: return QPointF(rect.left(), cy);
    }
}

QRectF CanvasView::cropOverlayRect() const {
    const DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return QRectF();
    const QRectF bounds(QPointF(0, 0), QSizeF(d->size));
    QRectF frame = cropRect_.normalized().intersected(bounds);
    // A degenerate frame (zero area, or dragged out to nothing) falls back
    // to the whole document rather than showing an unclippable hairline.
    if (frame.width() < kMinCrop || frame.height() < kMinCrop) frame = bounds;
    return frame;
}

double CanvasView::cropRatio() const {
    const ToolId tool = ToolId::Crop;
    const double rw = state_->option(tool, QStringLiteral("ratio_w")).toDouble();
    const double rh = state_->option(tool, QStringLiteral("ratio_h")).toDouble();
    // Explicit W/H always wins: it is the custom proportion the options bar
    // edits, and the only input "W x H x Resolution" has.
    if (rw > 0.0 && rh > 0.0) return rw / rh;
    switch (state_->option(tool, QStringLiteral("ratio")).toInt()) {
        case 2: {  // Original Ratio: the document's own aspect.
            const DocumentItem* d = doc();
            return (d && d->size.height() > 0)
                       ? double(d->size.width()) / double(d->size.height())
                       : 0.0;
        }
        case 3: return 1.0;            // 1:1 Square
        case 4: return 4.0 / 5.0;      // 4:5 (8:10)
        case 5: return 5.0 / 7.0;      // 5:7
        case 6: return 2.0 / 3.0;      // 2:3 (4:6)
        case 7: return 16.0 / 9.0;     // 16:9
        default: return 0.0;           // Ratio / W x H x Resolution, free
    }
}

void CanvasView::cropArmForDocument() {
    const DocumentItem* d = doc();
    cropRect_ = d ? QRectF(QPointF(0, 0), QSizeF(d->size)) : QRectF();
    cropDragMode_ = -1;
}

int CanvasView::cropHandleAtView(const QPointF& viewPos) const {
    const QRectF frame = cropOverlayRect();
    if (frame.isEmpty()) return -1;
    for (int h = 0; h < 8; ++h) {
        if (QLineF(documentToView(cropHandlePos(h, frame)), viewPos).length() <=
            kHandleTol)
            return h;
    }
    return -1;
}

// Grow/shrink a frame drawn from `anchor` to `cursor` so its aspect is
// exactly `ratio` (w/h). The axis the pointer actually travelled along
// drives the size — the same max-extent rule the Shift-square marquee uses —
// and the frame keeps the quadrant the pointer is in.
static QRectF cropFrameToRatio(const QPointF& anchor, const QPointF& cursor,
                               double ratio) {
    const double w = std::abs(cursor.x() - anchor.x());
    const double h = std::abs(cursor.y() - anchor.y());
    const double m = std::max(w, h * ratio);
    const double sx = cursor.x() < anchor.x() ? -1.0 : 1.0;
    const double sy = cursor.y() < anchor.y() ? -1.0 : 1.0;
    return QRectF(anchor, QPointF(anchor.x() + sx * m,
                                  anchor.y() + sy * m / ratio))
        .normalized();
}

void CanvasView::cropPress(const QPointF& docPoint, const QPointF& viewPos) {
    if (!doc()) return;
    cropPressDoc_ = docPoint;
    cropPressRect_ = cropOverlayRect();
    dragging_ = true;
    dragStartDoc_ = docPoint;
    dragCurrentDoc_ = docPoint;
    const int handle = cropHandleAtView(viewPos);
    if (handle >= 0) {
        cropDragMode_ = handle;
    } else if (cropOverlayRect().contains(docPoint)) {
        cropDragMode_ = kCropMove;   // inside: reposition the frame
    } else {
        cropDragMode_ = kCropDraw;   // outside: rubber-band a new frame
        cropPressRect_ = QRectF(docPoint, docPoint);
    }
    tool_logf(state_->activeTool(), "crop/press", "mode=%d", cropDragMode_);
}

void CanvasView::cropDragTo(const QPointF& docPoint) {
    const DocumentItem* d = doc();
    if (!d || cropDragMode_ < 0 || d->size.isEmpty()) return;
    const QRectF bounds(QPointF(0, 0), QSizeF(d->size));
    const double ratio = cropRatio();
    QRectF frame;
    switch (cropDragMode_) {
        case kCropDraw: {
            frame = ratio > 0.0
                        ? cropFrameToRatio(cropPressDoc_, docPoint, ratio)
                        : QRectF(cropPressDoc_, docPoint).normalized();
            break;
        }
        case kCropMove: {
            // Slide, never clip: the frame keeps its size and stops at the
            // document edges instead of shrinking against them. The shift is
            // the overlap of the desired delta with the room still free on
            // each side (positive = room right/below, negative = room
            // left/above), so a frame the size of the document simply has no
            // room to slide and stays put rather than collapsing.
            frame = cropPressRect_;
            const QPointF delta = docPoint - cropPressDoc_;
            const double dx =
                qBound(-frame.left(), delta.x(), bounds.right() - frame.right());
            const double dy =
                qBound(-frame.top(), delta.y(), bounds.bottom() - frame.bottom());
            frame.translate(dx, dy);
            break;
        }
        default: {
            const int h = cropDragMode_;
            if (ratio > 0.0) {
                // Ratio resize: the opposite handle is the fixed anchor. An
                // edge handle keeps the frame's centre line on the other axis
                // so the frame grows from its own middle, not off to a side.
                const QPointF anchor = cropHandlePos((h + 4) % 8, cropPressRect_);
                QPointF p = docPoint;
                if (h == 1 || h == 5) p.setX(cropPressRect_.center().x());
                if (h == 3 || h == 7) p.setY(cropPressRect_.center().y());
                frame = cropFrameToRatio(anchor, p, ratio);
            } else {
                frame = cropPressRect_;
                switch (h) {
                    case 0: frame.setTopLeft(docPoint); break;
                    case 1: frame.setTop(docPoint.y()); break;
                    case 2: frame.setTopRight(docPoint); break;
                    case 3: frame.setRight(docPoint.x()); break;
                    case 4: frame.setBottomRight(docPoint); break;
                    case 5: frame.setBottom(docPoint.y()); break;
                    case 6: frame.setBottomLeft(docPoint); break;
                    default: frame.setLeft(docPoint.x()); break;
                }
                frame = frame.normalized();
            }
            break;
        }
    }
    frame = frame.intersected(bounds);
    // A gesture that collapsed the frame below the minimum (dragged the far
    // corner past its opposite, say) leaves the press rect standing instead
    // of collapsing the frame to a hairline.
    if (frame.width() < kMinCrop || frame.height() < kMinCrop) {
        frame = cropPressRect_.intersected(bounds);
        if (frame.width() < kMinCrop || frame.height() < kMinCrop) frame = bounds;
    }
    cropRect_ = frame;
    state_->setStatusHint(
        tr("Crop: %1 × %2 px — Enter crops, Esc resets.")
            .arg(qRound(frame.width()))
            .arg(qRound(frame.height())));
}

void CanvasView::cropHoverUpdate(const QPointF& viewPos) {
    const int handle = cropHandleAtView(viewPos);
    Qt::CursorShape shape = Qt::CrossCursor;
    if (handle >= 0) {
        switch (handle) {
            case 0:
            case 4: shape = Qt::SizeFDiagCursor; break;
            case 2:
            case 6: shape = Qt::SizeBDiagCursor; break;
            case 1:
            case 5: shape = Qt::SizeVerCursor; break;
            default: shape = Qt::SizeHorCursor; break;
        }
    } else if (cropOverlayRect().contains(viewToDocument(viewPos))) {
        shape = Qt::SizeAllCursor;   // inside: drag to reposition the frame
    }
    viewport()->setCursor(shape);
}

void CanvasView::commitCrop() {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    const QRect box = cropOverlayRect().toAlignedRect().intersected(
        QRect(QPoint(0, 0), d->size));
    if (box.isEmpty()) return;
    if (box == QRect(QPoint(0, 0), d->size)) {
        state_->setStatusHint(tr("Crop: the frame already covers the whole image."));
        return;
    }
    // The canvas resize shifts every layer's offset by the new origin, so the
    // visible content lands exactly where it was, minus the cropped edges.
    state_->beginUndoStep();
    if (state_->resizeCanvas(box.topLeft(), box.size())) {
        state_->commitUndoStep(tr("Crop"), QStringLiteral("crop"));
        state_->setStatusHint(
            tr("Cropped to %1 × %2 px.").arg(box.width()).arg(box.height()));
    } else {
        state_->discardUndoStep();
    }
    cropArmForDocument();   // the fresh frame sits on the new document
    tool_logf(ToolId::Crop, "crop/commit", "box=%d,%d %dx%d", box.x(), box.y(),
              box.width(), box.height());
    viewport()->update();
}

void CanvasView::cancelCrop() {
    cropArmForDocument();
    state_->setStatusHint(tr("Crop reset to the whole image."));
    viewport()->update();
}

}  // namespace pittore::ui
