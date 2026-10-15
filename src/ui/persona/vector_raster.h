#pragma once
// Re-rasterize retained vector geometry (ui/persona).
//
// The SVG importer bakes each part's pixels once; the Stroke panel needs the
// same render on demand after a paint edit, without touching svg_parts.cpp.
// Input is the layer's ArtNode (segments in node space, matrix maps node →
// layer source pixels); output is a trimmed source-space image plus the trim
// origin, so the caller can compensate the layer's document offset.
#include <QImage>
#include <QPen>
#include <QPointF>

#include <memory>

namespace pittore::vector {
struct ArtNode;
struct ArtPaint;
}

namespace pittore::ui {

// Build the node's outline as a Qt path (node space). Shared by the
// re-rasterizer and the Node-tool overlay.
QPainterPath artNodePath(const pittore::vector::ArtNode& node);

// Solid/gradient fill brush shared by the rasterizer and the view-space
// painter (gradient coordinates ride the painter transform unchanged).
QBrush artFillBrush(const pittore::vector::ArtNode& node);

// Stroke brush: the retained stroke gradient when the shape carries one
// (SVG stroke="url(#id)"), else the flat stroke colour. Shared by the
// rasterizer and the view-space painter for the same reason.
QBrush artStrokeBrush(const pittore::vector::ArtNode& node);

// ArtPaint cap/join codes shared by the rasterizer and the view-space
// painter, so both stroke identically (cap 0 butt, 1 round, 2 square; join
// 0 miter, 1 round, 2 bevel).
Qt::PenCapStyle artCapStyle(int cap);
Qt::PenJoinStyle artJoinStyle(int join);

// Apply the node's dash pattern to a configured stroke pen (rasterizer and
// view-space painter share it). No-op for solid strokes.
void applyDashToPen(QPen& pen, const pittore::vector::ArtPaint& paint);

// Render `node` into a trimmed ARGB32_Premultiplied image. Returns false (with
// null outputs) when the node is empty or its footprint is degenerate/absurd.
bool rasterizeArtNode(const pittore::vector::ArtNode& node, QImage* img,
                      QPointF* sourceOrigin);

}  // namespace pittore::ui
