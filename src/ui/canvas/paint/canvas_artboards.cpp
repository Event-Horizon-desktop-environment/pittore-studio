#include "ui/canvas_view.h"

#include <QPainter>
#include <QPainterPath>

#include <cstdint>

namespace pittore::ui {

// Artboard chrome on the canvas: a hairline outline around every board plus its
// name in small text above the top-left corner. Affinity labels every artboard
// this way, so the two boards in a document stay tellable apart without opening
// the Artboards panel.
//
// Painted in VIEW space, never document space: the label must stay legible and
// the outline must stay a 1 px hairline at any zoom, exactly as the ruler and
// annotation overlays do. Runs at the end of the document border pass, after the
// composite, so none of it can become document pixels.
void CanvasView::paintArtboardOverlay(QPainter& p, const QTransform& t) {
    DocumentItem* d = doc();
    if (!d || d->boardOrder.isEmpty()) return;
    const ThemeColors c = colorsFor(state_->theme());

    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);

    p.setBrush(Qt::NoBrush);
    for (const std::uint64_t bid : d->boardOrder) {
        const int row = d->boardRowIndex(bid);
        if (row < 0) continue;
        const QRectF br = d->layers[row].boardRect;
        if (br.isEmpty()) continue;

        const QRectF vr = t.mapRect(br);
        const bool active = (bid == d->activeBoard);

        // Inactive boards take a neutral grey so they read on both a light and
        // a dark pasteboard; the active board takes the accent.
        p.setPen(QPen(active ? c.accent : QColor(128, 128, 128, 160), 1));
        p.drawRect(vr);

        // Name sits in the gap above the board, flush with its left edge.
        p.setPen(active ? c.text : c.textDim);
        const QRectF label(vr.left(), vr.top() - 18.0, vr.width(), 16.0);
        p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                   d->layers[row].name);
    }
}

}  // namespace pittore::ui
