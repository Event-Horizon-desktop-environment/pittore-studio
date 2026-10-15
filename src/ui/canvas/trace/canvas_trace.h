#pragma once
// Canvas repaint trace: one line per painted frame, repaint request, doc->view
// transform change and active-document switch, written to the tools rollup so
// they interleave with the move/drag rows in one file.
//
// Why this exists: "the canvas shows a layer somewhere other than the model"
// cannot be localised from the move rows alone. Those prove the model, the
// composite and the revision; they do not say what the frame that followed
// drew, over what rectangle, or through which transform. Each frame records
// all three plus the active layer's placement, and each repaint request
// records who asked — so a stale or mis-aimed repaint, a repaint that never
// came, and an unexpected zoom/scroll are each nameable from a single run.
//
// Join key: every frame carries `rev=` (DocumentItem::revision, also on the
// render/SKIP rows) and the move rows carry `pnt=` (canvasFrameCount()), so a
// move can be matched to the frame that did — or did not — follow it.

#include "ui/tools/ids/tool_ids.h"

#include <QtGlobal>

#include <QMargins>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>

namespace pittore::ui {

struct DocumentItem;  // ui/app_state.h — read in the .cpp, kept out of the header

// Monotonic count of painted canvas frames. The move rows carry it too, so a
// drag step with no frame behind it reads as a stall rather than a mystery.
qint64 canvasFrameCount();

// Queue a repaint cause for the next frame. `reason` must be a string literal:
// it is stored by pointer, not copied. The first cause of a batch wins, since
// that is what scheduled the coalesced repaint.
void canvasNoteUpdate(const char* reason);

// Consume the cause queued since the previous frame. Returns "none" when
// nothing observable asked for that frame.
const char* canvasTakeUpdateReason();

// One painted frame: what was repainted, through which transform, against
// which composite revision, and where the active layer sits in the model and
// in the placement paint would use for it.
//
// `layerIndex` is logged with the layer's name and kind as well, because an
// index is not a stable identity: adding an adjustment layer re-indexes the
// stack and moves the selection, so a bare `act=8` would silently start
// describing a different layer halfway through a run.
void canvasTraceFrame(ToolId tool, const char* reason, const QRect& dirty,
                      const QRect& viewport, const QTransform& t,
                      const DocumentItem* doc, int layerIndex, double ms);

// Everything that can move the document on screen without a zoom or a scroll:
// the widget itself being resized, the viewport margins changing, or a
// scrollbar appearing. `documentTransform()` reads all three, so a frame that
// jumps can be attributed from this line alone.
struct CanvasResizeState {
    QSize view;        // CanvasView size
    QSize viewport;    // viewport() size — what the transform centres against
    QMargins margins;  // viewport margins (the rulers)
    int hBarValue = 0;
    int hBarWidth = 0;
    int vBarValue = 0;
    int vBarWidth = 0;
};

void canvasTraceResize(ToolId tool, const CanvasResizeState& before,
                       const CanvasResizeState& after);

// One repaint request: who asked and the rect handed to Qt. `viewRect` is
// empty when the requested document region mapped outside the viewport and no
// update was scheduled at all.
void canvasTraceUpdate(ToolId tool, const char* reason, const QRectF& docRect,
                       const QRect& viewRect);

// A zoom change, with document (0,0) in view space on both sides of it.
void canvasTraceZoom(ToolId tool, double before, double after,
                     const QPointF& originBefore, const QPointF& originAfter);

// A scroll step. `dx`/`dy` are Qt's raw scroll delta; `origin` is document
// (0,0) in view space once the step has landed.
void canvasTraceScroll(ToolId tool, int dx, int dy, const QPointF& origin);

// An active-document switch: each tab carries its own zoom and scroll, so two
// screenshots of different tabs are not comparable.
void canvasTraceDoc(ToolId tool, int index, const QString& title, double zoom);

}  // namespace pittore::ui
