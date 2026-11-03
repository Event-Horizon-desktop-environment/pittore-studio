#pragma once

#include "ui/app_state.h"

#include <unordered_map>

// Free helpers shared by the app_state translation units. This file is
// internal to src/ui: nothing outside the state split includes it.
//
// The helpers used to live at the top of app_state.cpp. They are declared
// here so that file and the files split out of it can all call them.

namespace pittore::ui {

bool isPaintable(const LayerItem& l);

bool layerSelectionMask(const DocumentItem& d, const LayerItem& l,
                        pittore::compute::SelectionMask& out,
                        const pittore::Image* targetImage = nullptr,
                        QPointF targetOffset = QPointF(),
                        double targetScaleX = -1.0, double targetScaleY = -1.0);

void ensureLayerPixels(DocumentItem& d, LayerItem& l);
// Display-name to engine blend mode (lives in app_state.cpp).
pittore::compute::BlendMode engineBlendMode(const QString& name);
// Flat relief plane matching the layer's pixel dims (no-op without pixels).
void ensureLayerHeight(LayerItem& l);
// Relief plane for a target of w*h, or null when absent or stale-sized.
float* heightPlaneFor(LayerItem& l, std::uint32_t w, std::uint32_t h);
int parentGroupIndex(const DocumentItem& d, int index);

const pittore::Image* effectiveMaskImage(const QSize& docSize,
                                          const LayerItem& l);

void maskTransformFor(const LayerItem& l, QPointF& offset, double& scaleX,
                      double& scaleY);

QRectF maskBoundsFor(const DocumentItem& d, const LayerItem& l);

void transformLinkedMaskPixels(
    LayerItem& l, const pittore::Image& oldPixels,
    const std::function<pittore::Image(const pittore::Image&)>& op);

void syncLinkedMaskPlacement(LayerItem& l);

bool ensureLayerMask(DocumentItem& d, LayerItem& l, float coverage);

float maskBrushValue(const QColor& color);

QImage qImageFromImage(const pittore::Image& img);

std::optional<pittore::text::TextRaster> rasterizeTextItem(const TextItem& t);

void setAdjustmentDefaults(LayerItem& l);

bool renderLiveText(LayerItem& l);

QString textLabel(const QString& text);

bool gpuCompositeWindow(const DocumentItem& d, const LayerItem& l,
                        std::uint32_t cx0,
                        std::uint32_t cy0, std::uint32_t cx1,
                        std::uint32_t cy1, std::uint32_t* rx0,
                        std::uint32_t* ry0, std::uint32_t* rx1,
                        std::uint32_t* ry1);

void blitRGBAfToPremul(const pittore::RGBAf* src, uchar* dst,
                       std::uint32_t x0, std::uint32_t y0, std::uint32_t rw,
                       std::uint32_t rh);

pittore::render::LayerStyle scaledStyle(const pittore::render::LayerStyle& s,
                                         double r);

void ensureLayerStyle(const LayerItem& l);

ProjectFileData projectDataFromDocument(const DocumentItem& doc);

// Estimated per-document working set (pixel layers + engine staging + the
// composite QImage). Used to enforce the preferences RAM limit. Exact bytes
// are not critical — this is a budget guard, and such limits are coarse.
std::size_t documentWorkingSetBytes(const QSize& size);

// Last panel index of the subtree rooted at `start`. A group header owns every
// following row with deeper indent; a leaf's token is just itself. Used for all
// stack surgery so a moved group travels with its children.
int layerTokenEnd(const QVector<LayerItem>& layers, int start);

// The rows to move as a unit: the selection, expanded so each selected group
// carries its whole subtree. Returns the sorted unit; `base` receives the
// shallowest indent among them.
QVector<int> layerMoveUnit(const DocumentItem* d, int* base = nullptr);

// Repair indent jumps so token walks (layerTokenEnd) and ancestor scans
// (DocumentItem::enclosingGroups / effectiveVisibility) agree: every row must
// sit at most one level deeper than its predecessor, and a row deeper than
// its nearest shallower predecessor must hang under a Group (pixel rows
// cannot parent children). Grouping or re-parenting rows gathered from
// different nesting levels can otherwise strand a child behind a pixel row,
// where the token claims it for the group but the scan-back (and the
// composite) treats it as outside — the group eye then misses it.
void normalizeLayerIndents(QVector<LayerItem>& layers);

// --- artboard compositing (app_state_board_render.cpp) ----------------------
// A board's subtree paints only inside boardRect (D1). The gather walk narrows
// each row's paint bounds by its owning board's rect; these two helpers do the
// geometry so the hot loop stays a couple of lines per row.
//
// Board paint rects for `region`: every board's rect intersected with the
// region, keyed by board id, resolved once per gather so each child looks up
// in O(1). Boards that miss the region are omitted (their subtrees are outside
// it and gather skips them anyway).
std::unordered_map<std::uint64_t, QRectF> boardClipRects(const DocumentItem& d,
                                                         const QRect& region);
// The whole-pixel rect a board's subtree may paint into: its entry in `rects`,
// or an empty rect when the board misses the boardId lookup (outside the
// region). boardRect edges are whole pixels (quantised at creation, INV-004),
// so this is an exact integer rect, not a rounded approximation.
QRect boardPaintRect(const std::unordered_map<std::uint64_t, QRectF>& rects,
                     std::uint64_t boardId);

// Shared file-local helpers from the app_state.cpp split, one definition
// each for the whole UI lib:
// - cpuBackend: the fallback CPU backend (defined in app_state_composite.cpp)
// - imageFromQImage: premultiplied QImage to straight-alpha engine Image
//   (defined in app_state_detail.cpp)
// - adjustmentAuxSize: Curves/Levels aux-table footprint (defined in
//   app_state_adjust.cpp)
pittore::compute::ComputeBackend& cpuBackend();
std::shared_ptr<pittore::Image> imageFromQImage(const QImage& in);
std::size_t adjustmentAuxSize(const LayerItem& l);
// Working-set bytes of a WxH RGBAf plane (defined in app_state_composite.cpp).
std::size_t layerBytes(const QSize& size);

}  // namespace pittore::ui
