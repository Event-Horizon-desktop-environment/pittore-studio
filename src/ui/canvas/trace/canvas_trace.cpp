#include "ui/canvas/trace/canvas_trace.h"

#include "ui/app_state.h"
#include "ui/tools/log/tool_log.h"

#include <cstdio>

namespace pittore::ui {

namespace {

qint64 g_frame = 0;

// First repaint cause queued since the last frame. Literals only — stored by
// pointer, so no copy and no lifetime question.
const char* g_pending = nullptr;

// LayerItem::Kind is a 7-value enum with no string table of its own; the trace
// keeps its own three-letter spellings so a frame line stays greppable.
const char* kindToken(LayerItem::Kind k) {
    switch (k) {
        case LayerItem::Kind::Pixel: return "pix";
        case LayerItem::Kind::Text: return "txt";
        case LayerItem::Kind::Shape: return "shp";
        case LayerItem::Kind::Adjustment: return "adj";
        case LayerItem::Kind::Group: return "grp";
        case LayerItem::Kind::SmartObject: return "smf";
        case LayerItem::Kind::Frame: return "frm";
    }
    return "?";
}

}  // namespace

qint64 canvasFrameCount() { return g_frame; }

void canvasNoteUpdate(const char* reason) {
    if (g_pending) return;
    g_pending = reason ? reason : "unspecified";
}

const char* canvasTakeUpdateReason() {
    const char* r = g_pending ? g_pending : "none";
    g_pending = nullptr;
    return r;
}

void canvasTraceFrame(ToolId tool, const char* reason, const QRect& dirty,
                      const QRect& viewport, const QTransform& t,
                      const DocumentItem* doc, int layerIndex, double ms) {
    ++g_frame;
    const LayerItem* l =
        (doc && layerIndex >= 0 && layerIndex < doc->layers.size())
            ? &doc->layers[layerIndex]
            : nullptr;
    const QRectF bnds = l ? layerBounds(*doc, *l) : QRectF();
    tool_logf(tool, "canvas/frame",
              "pnt=%lld req=%s dirty=(%d,%d %dx%d) vp=%dx%d "
              "t=(%.4f,%.4f,%.4f,%.4f,%.2f,%.2f) rev=%llu "
              "act=%d name=\"%s\" kind=%s "
              "off=(%.2f,%.2f) bnds=(%.2f,%.2f %.2fx%.2f) ms=%.2f",
              static_cast<long long>(g_frame), reason ? reason : "none",
              dirty.x(), dirty.y(), dirty.width(), dirty.height(),
              viewport.width(), viewport.height(), t.m11(), t.m12(), t.m21(),
              t.m22(), t.dx(), t.dy(),
              static_cast<unsigned long long>(doc ? doc->revision : 0),
              layerIndex, l ? l->name.toUtf8().constData() : "(none)",
              l ? kindToken(l->kind) : "-", l ? l->offset.x() : 0.0,
              l ? l->offset.y() : 0.0, bnds.x(), bnds.y(), bnds.width(),
              bnds.height(), ms);
}

void canvasTraceResize(ToolId tool, const CanvasResizeState& before,
                       const CanvasResizeState& after) {
    tool_logf(tool, "canvas/resize",
              "view=(%d,%d)->(%d,%d) vp=(%d,%d)->(%d,%d) "
              "margins=(%d,%d,%d,%d)->(%d,%d,%d,%d) "
              "hbar=(v%d w%d)->(v%d w%d) vbar=(v%d w%d)->(v%d w%d)",
              before.view.width(), before.view.height(), after.view.width(),
              after.view.height(), before.viewport.width(),
              before.viewport.height(), after.viewport.width(),
              after.viewport.height(), before.margins.left(),
              before.margins.top(), before.margins.right(),
              before.margins.bottom(), after.margins.left(),
              after.margins.top(), after.margins.right(),
              after.margins.bottom(), before.hBarValue, before.hBarWidth,
              after.hBarValue, after.hBarWidth, before.vBarValue,
              before.vBarWidth, after.vBarValue, after.vBarWidth);
}

void canvasTraceUpdate(ToolId tool, const char* reason, const QRectF& docRect,
                       const QRect& viewRect) {
    tool_logf(tool, "canvas/update",
              "req=%s doc=(%.1f,%.1f %.1fx%.1f) view=(%d,%d %dx%d)",
              reason ? reason : "none", docRect.x(), docRect.y(),
              docRect.width(), docRect.height(), viewRect.x(), viewRect.y(),
              viewRect.width(), viewRect.height());
}

void canvasTraceZoom(ToolId tool, double before, double after,
                     const QPointF& originBefore, const QPointF& originAfter) {
    tool_logf(tool, "canvas/zoom", "%.4f->%.4f origin=(%.2f,%.2f)->(%.2f,%.2f)",
              before, after, originBefore.x(), originBefore.y(),
              originAfter.x(), originAfter.y());
}

void canvasTraceScroll(ToolId tool, int dx, int dy, const QPointF& origin) {
    tool_logf(tool, "canvas/scroll", "dx=%d dy=%d origin=(%.2f,%.2f)", dx, dy,
              origin.x(), origin.y());
}

void canvasTraceDoc(ToolId tool, int index, const QString& title,
                    double zoom) {
    tool_logf(tool, "canvas/doc", "idx=%d zoom=%.4f title=\"%s\"", index, zoom,
              title.toUtf8().constData());
}

}  // namespace pittore::ui
