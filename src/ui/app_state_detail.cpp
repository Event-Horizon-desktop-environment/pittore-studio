// Helpers shared by the app_state translation units. See ui/app_state_detail.h.
//
// Everything here moved out of app_state.cpp unchanged; only the includes and
// the declarations in the header were added.

#include "ui/app_state_detail.h"

#include "engine/compute/adjust.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/paint.h"
#include "engine/core/parallel.h"
#include "engine/compute/dither.h"
#include "ui/live_filter.h"

#include <QImage>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace pittore::ui {
namespace {

// Styled-bake resolution cap override (see setStyledBakeCap): the fx dialog
// lowers it during drag storms for proxy-res live previews. UI-thread only.
double gStyledBakeCap = 2048.0;

}  // namespace

// Whitelist for painting: a visible pixel layer that is not transparency-locked.
bool isPaintable(const LayerItem& l) {
    return l.visible && l.kind == LayerItem::Kind::Pixel && !l.lockTransparency;
}

// --- stashed layer pixels --------------------------------------------------
// A layer imported hidden behind a flattened base keeps its native pixels in
// LayerItem::deferredRgba8 (straight RGBA8) instead of realising them as
// RGBAf, which is 4 B/px instead of 16. These are the only sanctioned ways
// between the two states: realise when the layer is shown or exported, pack
// it back when a read-only consumer is done with it.

bool hasDeferredPixels(const LayerItem& l) {
    return l.pixels == nullptr && !l.deferredRgba8.isEmpty() &&
           l.deferredWidth > 0 && l.deferredHeight > 0;
}

void materializeLayerPixels(LayerItem& l) {
    if (l.pixels || !hasDeferredPixels(l)) return;
    // The stash is the 8-bit source the decoder expanded as u8*257, so
    // u8/255.0f is exactly the float the RGBAf path produced — realising a
    // layer changes no sample.
    const auto w = static_cast<std::uint32_t>(l.deferredWidth);
    const auto h = static_cast<std::uint32_t>(l.deferredHeight);
    auto img = std::make_shared<pittore::Image>(w, h);
    const auto* src =
        reinterpret_cast<const uchar*>(l.deferredRgba8.constData());
    pittore::RGBAf* dst = img->data();
    const std::size_t n = static_cast<std::size_t>(w) * h;
    // Each i reads only its own packed quad and writes only its own float, so
    // the expansion runs across cores bit-identically (same divide per sample).
    pittore::core::parallel_for(static_cast<std::uint32_t>(n), 4096,
                                [&](std::uint32_t i0, std::uint32_t i1) {
                                    for (std::size_t i = i0; i < i1; ++i) {
                                        const uchar* s = src + (i << 2);
                                        dst[i] = pittore::RGBAf{
                                            s[0] / 255.0f, s[1] / 255.0f,
                                            s[2] / 255.0f, s[3] / 255.0f};
                                    }
                                });
    l.pixels = std::move(img);
    ++l.sourceStamp;  // new native pixels: refresh the device copy
    l.deferredRgba8 = QByteArray();  // hand the packed allocation back
    l.deferredWidth = l.deferredHeight = 0;
}

namespace {

// The inverse of materializeLayerPixels, for a layer that was stashed at
// import and is still hidden. Only stashDeferredPixels() calls this: a layer
// the user has since shown, filtered or styled keeps its realised pixels.
void packLayerPixels(LayerItem& l) {
    if (!l.pixels || l.pixels->width() == 0 || l.pixels->height() == 0) return;
    const std::size_t n = static_cast<std::size_t>(l.pixels->width()) *
                          l.pixels->height();
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max() / 4))
        return;  // QByteArray is int-sized; leave it realised rather than wrap
    QByteArray packed(static_cast<int>(n) * 4, Qt::Uninitialized);
    uchar* dst = reinterpret_cast<uchar*>(packed.data());
    const pittore::RGBAf* src = l.pixels->data();
    auto byte = [](float v) {
        return static_cast<uchar>(qBound(0, static_cast<int>(std::lround(v * 255.0f)), 255));
    };
    for (std::size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = byte(src[i].r);
        dst[i * 4 + 1] = byte(src[i].g);
        dst[i * 4 + 2] = byte(src[i].b);
        dst[i * 4 + 3] = byte(src[i].a);
    }
    l.deferredRgba8 = std::move(packed);
    l.deferredWidth = l.pixels->width();
    l.deferredHeight = l.pixels->height();
    l.pixels.reset();
    ++l.sourceStamp;
}

// Straight RGBA64 — exactly what the project codec stores verbatim — straight
// out of a stashed layer, without realising it as RGBAf first. Saving must not
// be the call that inflates a document back to 16 B/px: u8*257 is the same
// sample the RGBAf round trip would have produced, so the file is identical.
QImage stashedToRgba64(const LayerItem& l) {
    const int w = static_cast<int>(l.deferredWidth);
    const int h = static_cast<int>(l.deferredHeight);
    const std::size_t n = static_cast<std::size_t>(w) * h * 4;
    if (w <= 0 || h <= 0 || l.deferredRgba8.size() != static_cast<int>(n))
        return QImage();
    QImage q(w, h, QImage::Format_RGBA64);
    if (q.isNull()) return QImage();
    const auto* src =
        reinterpret_cast<const uchar*>(l.deferredRgba8.constData());
    auto* dst = reinterpret_cast<quint16*>(q.bits());
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = static_cast<quint16>(src[i]) * 257u;
    return q;
}

}  // namespace

QVector<int> materializeDeferredPixels(DocumentItem& doc) {
    QVector<int> expanded;
    for (int i = 0; i < doc.layers.size(); ++i) {
        if (!hasDeferredPixels(doc.layers[i])) continue;
        materializeLayerPixels(doc.layers[i]);
        expanded.append(i);
    }
    return expanded;
}

void stashDeferredPixels(DocumentItem& doc, const QVector<int>& expanded) {
    for (int i : expanded) {
        if (i < 0 || i >= doc.layers.size()) continue;
        LayerItem& l = doc.layers[i];
        if (!l.pixels || l.visible || l.styled || l.filtered) continue;
        packLayerPixels(l);
    }
}

// Map the document's rectangular/elliptical selection into a layer's native
// pixel space so a dab or flood fill can be confined to it. Returns false when
// the document has no selection (the caller then passes a null mask, meaning
// the whole buffer is editable).
bool layerSelectionMask(const DocumentItem& d, const LayerItem& l,
                        pittore::compute::SelectionMask& out,
                        const pittore::Image* targetImage,
                        QPointF targetOffset,
                        double targetScaleX, double targetScaleY) {
    if (d.selection.isEmpty()) return false;
    const pittore::Image* img = targetImage ? targetImage : l.pixels.get();
    const QPointF offset = targetImage ? targetOffset : l.offset;
    const double sx = targetImage
                          ? std::max(targetScaleX > 0.0 ? targetScaleX : 1e-6,
                                     1e-6)
                          : std::max(l.scaleX, 1e-6);
    const double sy = targetImage
                          ? std::max(targetScaleY > 0.0 ? targetScaleY : 1e-6,
                                     1e-6)
                          : std::max(l.scaleY, 1e-6);
    if (d.selectionIsMask && !d.selectionMask.isNull()) {
        // Arbitrary-shape selection: resample the document-resolution
        // grayscale channel into the target's native pixel space over the
        // overlap of the mask bbox and the target image. Nearest sampling is
        // fine — the mask is soft (0..255), so edges stay anti-aliased.
        const QRectF r = d.selection.normalized();
        const int dw = d.selectionMask.width();
        const int dh = d.selectionMask.height();
        const int limX = img ? int(img->width()) : d.size.width();
        const int limY = img ? int(img->height()) : d.size.height();
        const int x0 = std::max(int(std::floor((r.left() - offset.x()) / sx)), 0);
        const int y0 = std::max(int(std::floor((r.top() - offset.y()) / sy)), 0);
        const int x1 = std::min(int(std::ceil((r.right() - offset.x()) / sx)), limX - 1);
        const int y1 = std::min(int(std::ceil((r.bottom() - offset.y()) / sy)), limY - 1);
        if (x1 < x0 || y1 < y0) return false;
        const int mw = x1 - x0 + 1, mh = y1 - y0 + 1;
        out.x0 = static_cast<float>(x0);
        out.y0 = static_cast<float>(y0);
        out.x1 = static_cast<float>(x1) + 1.0f;
        out.y1 = static_cast<float>(y1) + 1.0f;
        out.ellipse = false;
        out.pixels.assign(std::size_t(mw) * mh, 0);
        out.mw = mw;
        out.mh = mh;
        const uchar* src = d.selectionMask.constBits();
        for (int ly = y0; ly <= y1; ++ly) {
            const double dyv = offset.y() + (ly + 0.5) * sy;
            const int my = std::clamp(int(std::lround(dyv)), 0, dh - 1);
            std::size_t rowOut = std::size_t(ly - y0) * mw;
            const uchar* row = src + std::size_t(my) * dw;
            for (int lx = x0; lx <= x1; ++lx) {
                const double dxv = offset.x() + (lx + 0.5) * sx;
                const int mx = std::clamp(int(std::lround(dxv)), 0, dw - 1);
                out.pixels[rowOut + (lx - x0)] = row[mx];
            }
        }
        return true;
    }
    const QRectF r = d.selection.normalized();
    const double x0 = (r.left() - offset.x()) / sx;
    const double y0 = (r.top() - offset.y()) / sy;
    const double x1 = (r.right() - offset.x()) / sx;
    const double y1 = (r.bottom() - offset.y()) / sy;
    out.x0 = static_cast<float>(std::min(x0, x1));
    out.y0 = static_cast<float>(std::min(y0, y1));
    out.x1 = static_cast<float>(std::max(x0, x1));
    out.y1 = static_cast<float>(std::max(y0, y1));
    out.ellipse = d.selectionIsEllipse;
    return true;
}

// Ensure a pixel layer owns its engine Image. The bottom-most pixel layer of a
// document doubles as the document's Background (opaque white); everything
// above starts transparent so painting accumulates over the stack.
void ensureLayerPixels(DocumentItem& d, LayerItem& l) {
    if (l.pixels) return;
    if (hasDeferredPixels(l)) {
        // A layer imported stashed still carries its decoded content in
        // deferredRgba8. Expand that instead of handing it an empty
        // document-sized image, which would silently drop the layer.
        materializeLayerPixels(l);
        if (l.pixels) return;
    }
    auto img = std::make_shared<pittore::Image>(
        static_cast<std::uint32_t>(d.size.width()),
        static_cast<std::uint32_t>(d.size.height()));
    if (&l == &d.layers.last()) {
        // Background: the document's canvas paper (historic gray for
        // plain documents, the dialog choice for new projects).
        const QColor& paper = d.canvasPaper;
        img->fill(pittore::RGBAf{static_cast<float>(paper.redF()),
                                  static_cast<float>(paper.greenF()),
                                  static_cast<float>(paper.blueF()), 1.0f});
    } else {
        img->fill(pittore::RGBAf{0, 0, 0, 0});
    }
    l.pixels = std::move(img);
    ++l.sourceStamp;                // fresh native pixels → stale device cache
    l.thumbnail = QImage();         // …and a stale panel preview
}

// Ensure a pixel layer owns a zero relief plane matching its pixels.
// Reallocates when the pixel dims changed out from under it.
void ensureLayerHeight(LayerItem& l) {
    if (!l.pixels) return;
    const std::size_t n = static_cast<std::size_t>(l.pixels->width()) *
                          static_cast<std::size_t>(l.pixels->height());
    if (l.heightMap && l.heightMap->size() == n) return;
    l.heightMap = std::make_shared<std::vector<float>>(n, 0.0f);
}

float* heightPlaneFor(LayerItem& l, std::uint32_t w, std::uint32_t h) {
    if (!l.heightMap) return nullptr;
    if (l.heightMap->size() !=
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h))
        return nullptr;
    return l.heightMap->data();
}

// Immediate parent group row, or -1 for a top-level row. Clipping groups
// never cross this boundary: a clipped layer needs a base in the same scope.
int parentGroupIndex(const DocumentItem& d, int index) {
    const QVector<int> groups = d.enclosingGroups(index);
    return groups.isEmpty() ? -1 : groups.first();
}

// The mask image the compositor may sample, or null. A linked pixel mask
// must share its layer's native dimensions; a linked adjustment mask must be
// document-sized (adjustments live in document space). Anything else is
// treated as missing rather than sampled out of alignment.
const pittore::Image* effectiveMaskImage(const QSize& docSize,
                                          const LayerItem& l) {
    if ((l.kind != LayerItem::Kind::Pixel &&
         l.kind != LayerItem::Kind::Adjustment) ||
        !l.hasMask || !l.maskEnabled || !l.mask)
        return nullptr;
    if (l.kind == LayerItem::Kind::Pixel) {
        if (!l.pixels) return nullptr;
        if (l.maskLinked &&
            (l.mask->width() != l.pixels->width() ||
             l.mask->height() != l.pixels->height()))
            return nullptr;
    } else {
        if (l.maskLinked &&
            (static_cast<int>(l.mask->width()) != docSize.width() ||
             static_cast<int>(l.mask->height()) != docSize.height()))
            return nullptr;
    }
    return l.mask.get();
}

// Document placement of a live mask. Linked masks ride the layer transform;
// unlinked masks keep their own frozen placement.
void maskTransformFor(const LayerItem& l, QPointF& offset, double& scaleX,
                      double& scaleY) {
    if (l.maskLinked || !l.mask) {
        offset = l.offset;
        scaleX = l.scaleX;
        scaleY = l.scaleY;
    } else {
        offset = l.maskOffset;
        scaleX = l.maskScaleX;
        scaleY = l.maskScaleY;
    }
}

QRectF maskBoundsFor(const DocumentItem& d, const LayerItem& l) {
    const pittore::Image* mask = effectiveMaskImage(d.size, l);
    if (!mask) return QRectF();
    QPointF offset{0, 0};
    double scaleX = 1.0, scaleY = 1.0;
    maskTransformFor(l, offset, scaleX, scaleY);
    return QRectF(offset, QSizeF(mask->width() * scaleX,
                                mask->height() * scaleY));
}

// Apply the same native-pixel permutation to a linked mask that a rotate or
// flip applies to its layer. The placement update still goes through
// syncLinkedMaskPlacement at the call site.
void transformLinkedMaskPixels(
    LayerItem& l, const pittore::Image& oldPixels,
    const std::function<pittore::Image(const pittore::Image&)>& op) {
    if (!l.mask || !l.maskLinked) return;
    if (l.mask->width() != oldPixels.width() ||
        l.mask->height() != oldPixels.height())
        return;
    l.mask = std::make_shared<pittore::Image>(op(*l.mask));
    ++l.maskStamp;
}

// Keep a linked mask glued to its pixels. Matching dimensions simply adopt
// the layer placement; a dimension change (text re-render, rotate/flip
// helpers that forgot the mask) nearest-resamples coverage into the new
// native grid instead of sampling out of alignment.
void syncLinkedMaskPlacement(LayerItem& l) {
    if (!l.mask || !l.maskLinked || !l.pixels) return;
    if (l.mask->width() != l.pixels->width() ||
        l.mask->height() != l.pixels->height()) {
        const std::uint32_t nw = l.pixels->width();
        const std::uint32_t nh = l.pixels->height();
        const std::uint32_t ow = l.mask->width();
        const std::uint32_t oh = l.mask->height();
        auto next = std::make_shared<pittore::Image>(nw, nh);
        for (std::uint32_t y = 0; y < nh; ++y) {
            const std::uint32_t sy =
                std::min(oh - 1, static_cast<std::uint32_t>(
                                     (static_cast<std::uint64_t>(y) * oh) / nh));
            for (std::uint32_t x = 0; x < nw; ++x) {
                const std::uint32_t sx = std::min(
                    ow - 1, static_cast<std::uint32_t>(
                                (static_cast<std::uint64_t>(x) * ow) / nw));
                const float c = std::clamp(l.mask->at(sx, sy).r, 0.0f, 1.0f);
                next->at(x, y) = pittore::compute::make_mask_pixel(c);
            }
        }
        l.mask = std::move(next);
        ++l.maskStamp;
    }
    l.maskOffset = l.offset;
    l.maskScaleX = l.scaleX;
    l.maskScaleY = l.scaleY;
}

// Realise a layer's mask image. Existing masks are kept (and linked
// ones re-glued); only a missing mask is allocated. Pixel masks share the
// layer's native grid; adjustment masks live in document space.
bool ensureLayerMask(DocumentItem& d, LayerItem& l, float coverage) {
    if ((l.kind != LayerItem::Kind::Pixel &&
         l.kind != LayerItem::Kind::Adjustment) ||
        l.locked)
        return false;
    std::uint32_t mw, mh;
    if (l.kind == LayerItem::Kind::Pixel) {
        ensureLayerPixels(d, l);
        if (!l.pixels) return false;
        mw = l.pixels->width();
        mh = l.pixels->height();
    } else {
        mw = static_cast<std::uint32_t>(d.size.width());
        mh = static_cast<std::uint32_t>(d.size.height());
    }
    if (!l.mask) {
        auto mask = std::make_shared<pittore::Image>(mw, mh);
        mask->fill(pittore::compute::make_mask_pixel(coverage));
        l.mask = std::move(mask);
        l.maskLinked = true;
        ++l.maskStamp;
    } else if (l.kind == LayerItem::Kind::Pixel) {
        syncLinkedMaskPlacement(l);
    }
    if (l.maskLinked) {
        // Adjustment layers never move: their offset/scale stay at the
        // document identity, so linked masks are document-sized by
        // construction.
        l.maskOffset = l.offset;
        l.maskScaleX = l.scaleX;
        l.maskScaleY = l.scaleY;
    }
    l.hasMask = true;
    l.maskEnabled = true;
    return true;
}

// Brush colour → mask coverage using the engine's Rec.709 luma, so painting
// with black conceals, white reveals, and colours land on their grey value.
float maskBrushValue(const QColor& color) {
    if (!color.isValid()) return 1.0f;
    const float v = static_cast<float>(0.2126 * color.redF() +
                                       0.7152 * color.greenF() +
                                       0.0722 * color.blueF());
    return std::clamp(v, 0.0f, 1.0f);
}

// Straight-alpha engine Image → ARGB32 QImage. Used by the few operations
// (layer straighten/rotate) that are easier to express with Qt's resampling.
QImage qImageFromImage(const pittore::Image& img) {
    const int w = static_cast<int>(img.width());
    const int h = static_cast<int>(img.height());
    QImage out(w, h, QImage::Format_ARGB32);
    if (out.isNull()) return out;
    const pittore::RGBAf* src = img.data();
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const pittore::RGBAf& p =
                src[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
            const int a = qBound(0, static_cast<int>(std::lround(p.a * 255.0f)), 255);
            const int r = qBound(0, static_cast<int>(std::lround(p.r * 255.0f)), 255);
            const int g = qBound(0, static_cast<int>(std::lround(p.g * 255.0f)), 255);
            const int b = qBound(0, static_cast<int>(std::lround(p.b * 255.0f)), 255);
            row[x] = qRgba(r, g, b, a);
        }
    }
    return out;
}

// Rasterize a live text layer's spec through the shared text engine.
std::optional<pittore::text::TextRaster> rasterizeTextItem(const TextItem& t) {
    return pittore::text::rasterize(textSpecFor(t));
}

// Creation defaults for every live adjustment kind (params + curves). Single
// source of truth shared by addAdjustmentLayer and resetAdjustmentToDefaults
// so Reset always lands exactly where a fresh layer starts.
void setAdjustmentDefaults(LayerItem& l) {
    using pittore::compute::AdjustmentKind;
    const AdjustmentKind k = static_cast<AdjustmentKind>(l.adjustmentKind);
    for (float& v : l.adjustmentParams) v = 0.0f;
    if (k == AdjustmentKind::Levels) {
        l.adjustmentParams[0] = 0.0f;
        l.adjustmentParams[1] = 1.0f;
        l.adjustmentParams[2] = 1.0f;
        l.adjustmentParams[3] = 0.0f;
        l.adjustmentParams[4] = 1.0f;
    } else if (k == AdjustmentKind::Curves) {
        l.adjustmentCurve = {{{0.0, 0.0}, {1.0, 1.0}}};
        l.adjustmentCurveR.clear();
        l.adjustmentCurveG.clear();
        l.adjustmentCurveB.clear();
        rebuildAdjustmentLUT(l);
    } else if (k == AdjustmentKind::Threshold) {
        l.adjustmentParams[0] = 0.5f;
    } else if (k == AdjustmentKind::Posterize) {
        l.adjustmentParams[0] = 4.0f;
    } else if (k == AdjustmentKind::PhotoFilter) {
        // Warming 85-style default at the conventional 25% density, luminosity
        // on.
        l.adjustmentParams[0] = 236.0f / 255.0f;
        l.adjustmentParams[1] = 138.0f / 255.0f;
        l.adjustmentParams[2] = 0.0f;
        l.adjustmentParams[3] = 0.25f;
        l.adjustmentParams[4] = 1.0f;
    } else if (k == AdjustmentKind::WhiteBalance) {
        l.adjustmentParams[0] = 6500.0f;
        l.adjustmentParams[1] = 0.0f;
    } else if (k == AdjustmentKind::BlackWhite) {
        l.adjustmentParams[0] = 40.0f;
        l.adjustmentParams[1] = 60.0f;
        l.adjustmentParams[2] = 40.0f;
        l.adjustmentParams[3] = 60.0f;
        l.adjustmentParams[4] = 20.0f;
        l.adjustmentParams[5] = 80.0f;
    } else if (k == AdjustmentKind::ChannelMixer) {
        // Identity matrix, zero constants, color mode.
        l.adjustmentParams[0] = 1.0f;
        l.adjustmentParams[4] = 1.0f;
        l.adjustmentParams[8] = 1.0f;
    } else if (k == AdjustmentKind::ColorBalance) {
        // Neutral balances, luminosity preserved (the conventional defaults).
        l.adjustmentParams[9] = 1.0f;
    }
}

// Re-render a live text layer into its own pixels. Returns false when no font
// is available (the layer is left untouched); empty text clears the pixels
// while the layer stays alive so the caret still has somewhere to sit.
bool renderLiveText(LayerItem& l) {
    const TextItem& t = l.textSpec;
    l.thumbnail = QImage();
    auto raster = rasterizeTextItem(t);
    if (!raster) return false;
    l.textLayoutWidth = raster->layoutWidth;
    l.textFirstBaseline = raster->firstBaseline;
    l.textLineAdvance = raster->lineAdvance;
    if (raster->isEmpty()) {
        l.pixels.reset();
        l.offset = QPointF(0, 0);
        l.scaleX = l.scaleY = 1.0;
        ++l.sourceStamp;
        return true;
    }
    // The engine lays every line inside 0..layoutWidth; a frame wider than the
    // run centres or right-aligns that pen box inside it.
    double alignShift = 0.0;
    if (t.wrapWidth > 0.5) {
        if (t.align == 1) alignShift = (t.wrapWidth - raster->layoutWidth) * 0.5;
        else if (t.align == 2) alignShift = t.wrapWidth - raster->layoutWidth;
    }
    const int w = raster->bounds.width();
    const int h = raster->bounds.height();
    if (w <= 0 || h <= 0) return false;
    auto img = std::make_shared<pittore::Image>(static_cast<std::uint32_t>(w),
                                                 static_cast<std::uint32_t>(h));
    const float r = t.color.redF();
    const float g = t.color.greenF();
    const float b = t.color.blueF();
    const float ca = t.color.alphaF();
    const std::size_t px = static_cast<std::size_t>(w) * h;
    if (raster->colorRgba.size() == px * 4) {
        // The engine already composited the fill, highlight box and
        // decorations (with their own colours); use its pixels.
        for (std::size_t i = 0; i < px; ++i) {
            const std::uint8_t* p = &raster->colorRgba[i * 4];
            img->data()[i] = pittore::RGBAf{p[0] / 255.0f, p[1] / 255.0f,
                                             p[2] / 255.0f, p[3] / 255.0f};
        }
    } else {
        for (std::size_t i = 0; i < raster->coverage.size(); ++i) {
            const float a = static_cast<float>(raster->coverage[i]) / 255.0f * ca;
            img->data()[i] = pittore::RGBAf{r, g, b, a};
        }
    }
    l.pixels = std::move(img);
    l.offset = QPointF(t.origin.x() + alignShift + raster->bounds.left,
                       t.origin.y() + raster->bounds.top);
    l.scaleX = l.scaleY = 1.0;
    syncLinkedMaskPlacement(l);
    ++l.sourceStamp;
    return true;
}

// The Layers-panel label for a text layer: its first line, elided.
QString textLabel(const QString& text) {
    const QString first = text.section('\n', 0, 0).trimmed();
    if (first.isEmpty()) return QString();
    return first.size() > 32 ? first.left(32) + QChar(0x2026) : first;
}

// Smallest integer pixel window covering a layer's compositing footprint,
// clipped to the (already canvas-clamped) dirty rectangle. The GPU placed
// kernels iterate exactly this window instead of the whole frame, so a canvas
// full of small parts composites only the pixels each layer actually colours.
// Returns false when the layer does not touch the dirty rect at all.
bool gpuCompositeWindow(const DocumentItem& d, const LayerItem& l,
                        std::uint32_t cx0,
                        std::uint32_t cy0, std::uint32_t cx1,
                        std::uint32_t cy1, std::uint32_t* rx0,
                        std::uint32_t* ry0, std::uint32_t* rx1,
                        std::uint32_t* ry1) {
    const QRectF sb = stageBounds(d, l);
    const double fx0 = std::max(sb.left(), static_cast<double>(cx0));
    const double fy0 = std::max(sb.top(), static_cast<double>(cy0));
    const double fx1 = std::min(sb.left() + sb.width(), static_cast<double>(cx1));
    const double fy1 = std::min(sb.top() + sb.height(), static_cast<double>(cy1));
    if (fx0 >= fx1 || fy0 >= fy1) return false;
    *rx0 = static_cast<std::uint32_t>(std::floor(fx0));
    *ry0 = static_cast<std::uint32_t>(std::floor(fy0));
    *rx1 = static_cast<std::uint32_t>(std::ceil(fx1));
    *ry1 = static_cast<std::uint32_t>(std::ceil(fy1));
    return true;
}

// Convert a straight-alpha RGBAf buffer to the premultiplied ARGB32 QImage the
// canvas draws. Ordered Bayer dither (absolute coords) so gradients don't
// band; exact integers are untouched (see compute/dither.h).
void blitRGBAfToPremul(const pittore::RGBAf* src, uchar* dst,
                       std::uint32_t x0, std::uint32_t y0, std::uint32_t rw,
                       std::uint32_t rh) {
    // Rows write disjoint dst rows with absolute dither coords:
    // bit-identical threaded.
    pittore::core::parallel_rows(rh, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const pittore::RGBAf* row = src + static_cast<std::size_t>(y) * rw;
            uchar* out = dst + static_cast<std::size_t>(y) * rw * 4;
            for (std::uint32_t x = 0; x < rw; ++x) {
                const pittore::RGBAf& s = row[x];
                const unsigned int ax = x0 + x, ay = y0 + y;
                out[0] = pittore::compute::dither::quantize(s.b * s.a, ax, ay);
                out[1] = pittore::compute::dither::quantize(s.g * s.a, ax, ay);
                out[2] = pittore::compute::dither::quantize(s.r * s.a, ax, ay);
                out[3] = pittore::compute::dither::quantize(s.a, ax, ay);
                out += 4;
            }
        }
    });
}

// Uniformly rescale a style's pixel-space parameters by `r` (the styled
// raster's bake factor). Angles, opacities, blend colours, spread ratios and
// enums are invariant — only every length shrinks with the raster, so the
// effects keep the same DOCUMENT-space footprint at a reduced bake.
pittore::render::LayerStyle scaledStyle(const pittore::render::LayerStyle& s,
                                         double r) {
    pittore::render::LayerStyle out = s;
    if (out.hasBlur) out.blur.radius *= r;
    auto shadow = [r](pittore::render::ShadowStyle& sh) {
        sh.distance *= r;
        sh.size *= r;
    };
    if (out.hasDropShadow) shadow(out.dropShadow);
    if (out.hasInnerShadow) shadow(out.innerShadow);
    if (out.hasOuterGlow) out.outerGlow.size *= r;
    if (out.hasInnerGlow) out.innerGlow.size *= r;
    if (out.hasSatin) {
        out.satin.distance *= r;
        out.satin.size *= r;
    }
    if (out.hasStroke) out.stroke.size *= r;
    if (out.hasBevel) {
        out.bevel.size *= r;
        out.bevel.soften *= r;
    }
    return out;   // gradient.scale is a normalized zoom ratio — invariant
}

void setStyledBakeCap(double maxEdge) {
    gStyledBakeCap = maxEdge > 0.0 ? maxEdge : 2048.0;
}

// Render a layer's style over its placed pixels and cache the result on the
// layer. Effects use document-space sizes, so the layer is resampled into its
// document footprint first — the same thing the decoder does to a file's baked
// effects, and what editors do for live ones.
//
// Two performance properties keep an FX'd layer cheap to work with:
//  * The raster is capped at kMaxStyledEdge on the long edge and the
//    compositor resamples it back to document scale, so a 48MP photo pays for
//    a ~2048-px effect render instead of a 48MP float-plane pipeline.
//  * The baked content is anchored to the layer, not the document: a pure
//    translation only moves `styledOffset` and never re-renders, so a
//    Move-tool drag of an FX'd layer is as cheap as an unstyled one.
// The cache is keyed on sourceStamp (bumped for both pixel and style edits)
// plus the placement scale (which changes the document footprint).
void ensureLayerStyle(const LayerItem& l) {
    // Dropping the styled raster bumps styledRev so the compositor's device
    // cache re-uploads the plain pixels.
    auto drop = [&l] {
        if (l.styled) {
            l.styled.reset();
            ++l.styledRev;
        }
        l.styledValid = false;
    };
    if (l.style.empty()) {
        // Persona-owned dense vector bake (bakeArtDense): preserved while
        // valid (stamp + placement match the pixels), so zoomed-in art keeps
        // its supersampled display bake across composites. Zoom freshness is
        // maintained proactively — canvas zoom, undo/redo and every art
        // mutation re-bake synchronously before any composite — and style or
        // live-filter edits invalidate through their own paths, dropping back
        // here to the pixels fallback.
        if (l.art && !l.art->isEmpty() && l.styledValid && l.styled &&
            l.styledStamp == l.sourceStamp &&
            l.styledBaseOffset == l.offset &&
            l.styledBaseScaleX == l.scaleX && l.styledBaseScaleY == l.scaleY)
            return;
        drop();
        return;
    }
    if (!l.pixels) return;
    // Live-filter composition: styles render over the filtered base when one
    // applies (ensureLayerFilter runs first in layerDrawSource, so the cache
    // is current and styledValid was dropped on any filter change).
    const pittore::Image* filterBase = liveFilterBase(l);
    if (!filterBase) {
        drop();
        return;
    }
    if (l.styledValid && l.styled && l.styledStamp == l.sourceStamp &&
        l.styledBaseScaleX == l.scaleX && l.styledBaseScaleY == l.scaleY) {
        // Valid raster. A pure translation doesn't change its content (it is
        // anchored to the layer) — only where it is placed. Shift the baked
        // origin by the same delta (never recompute it from the outset: the
        // bake's true origin already accounts for resample rounding).
        if (l.styledBaseOffset != l.offset) {
            l.styledOffset += l.offset - l.styledBaseOffset;
            l.styledBaseOffset = l.offset;
        }
        return;
    }

    const std::uint32_t sw = filterBase->width();
    const std::uint32_t sh = filterBase->height();
    const double ox = l.offset.x(), oy = l.offset.y();
    if (sw == 0 || sh == 0 || l.scaleX <= 0.0 || l.scaleY <= 0.0) return;
    const int dw = std::max(1, static_cast<int>(std::ceil(sw * l.scaleX)));
    const int dh = std::max(1, static_cast<int>(std::ceil(sh * l.scaleY)));

    // Cap the effect raster: layers up to the cap on the long edge render
    // effects at 1:1 (an exact texel copy); bigger layers bake at the cap and
    // let the compositor resample back. This bounds a style's CPU cost and its
    // device footprint no matter how large the layer is.
    const double r = std::min(1.0, gStyledBakeCap / std::max(dw, dh));
    const int dwr = std::max(1, static_cast<int>(std::ceil(dw * r)));
    const int dhr = std::max(1, static_cast<int>(std::ceil(dh * r)));

    // Place into document space at integer pixel centres; at r == 1 (and
    // scale 1) this is an exact texel copy, so the styled raster lines up with
    // what the compositor would have drawn unstyled.
    pittore::render::Rgba8Image img;
    img.w = static_cast<std::uint32_t>(dwr);
    img.h = static_cast<std::uint32_t>(dhr);
    img.px.resize(static_cast<std::size_t>(dwr) * dhr * 4);
    const pittore::RGBAf* src = filterBase->data();
    const double rinv = 1.0 / r;
    for (int y = 0; y < dhr; ++y) {
        for (int x = 0; x < dwr; ++x) {
            const pittore::RGBAf c = pittore::compute::sample_placed_host(
                src, sw, sh, ox + x * rinv, oy + y * rinv, ox, oy, l.scaleX,
                l.scaleY);
            const std::size_t at = (static_cast<std::size_t>(y) * dwr + x) * 4;
            img.px[at + 0] = static_cast<std::uint8_t>(
                std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
            img.px[at + 1] = static_cast<std::uint8_t>(
                std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
            img.px[at + 2] = static_cast<std::uint8_t>(
                std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            img.px[at + 3] = static_cast<std::uint8_t>(
                std::clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    int grow = 0;
    if (!pittore::render::applyLayerStyle(img, scaledStyle(l.style, r),
                                           &grow)) {
        drop();
        return;
    }

    auto out = std::make_shared<pittore::Image>(img.w, img.h);
    const std::size_t n = static_cast<std::size_t>(img.w) * img.h;
    pittore::RGBAf* dst = out->data();
    constexpr float inv = 1.0f / 255.0f;
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = pittore::RGBAf{img.px[i * 4 + 0] * inv, img.px[i * 4 + 1] * inv,
                                 img.px[i * 4 + 2] * inv, img.px[i * 4 + 3] * inv};
    }
    l.styled = std::move(out);
    // The bake grew by `grow` bake-pixels around the sampled footprint, so
    // the image origin in document space is (ox, oy) minus grow resampled
    // back (grow/r). Using the unscaled outset here instead would disagree
    // with the image by resample rounding whenever r < 1 — a visible jump
    // between proxy and full bakes of the same style.
    l.styledOffset = QPointF(ox - grow * rinv, oy - grow * rinv);
    l.styledStamp = l.sourceStamp;
    l.styledBaseOffset = l.offset;
    l.styledBaseScaleX = l.scaleX;
    l.styledBaseScaleY = l.scaleY;
    l.styledResample = r;
    l.styledValid = true;
    ++l.styledRev;
}

ProjectFileData projectDataFromDocument(const DocumentItem& doc) {
    ProjectFileData data;
    data.name = doc.title;
    data.size = doc.size;
    data.dpi = doc.dpi;
    data.colorMode = doc.colorMode;
    data.iccProfile = doc.iccProfile;
    data.boardIdCounter = doc.boardIdCounter;
    // Background label is derived from the bottom-most realised pixel layer so
    // the start page can restate it after a reload.
    data.background = QStringLiteral("white");
    for (int i = doc.layers.size() - 1; i >= 0; --i) {
        const LayerItem& l = doc.layers[i];
        pittore::RGBAf c{0, 0, 0, 0};
        if (l.pixels) {
            c = l.pixels->data()[0];
        } else if (hasDeferredPixels(l)) {
            // Packed behind the flattened base: read the stash rather than
            // realise the layer just to label the background.
            const auto* s =
                reinterpret_cast<const uchar*>(l.deferredRgba8.constData());
            c = pittore::RGBAf{s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f,
                               s[3] / 255.0f};
        } else {
            continue;
        }
        if (c.a < 0.5f)
            data.background = QStringLiteral("transparent");
        else if (c.r < 0.5f && c.g < 0.5f && c.b < 0.5f)
            data.background = QStringLiteral("black");
        else
            data.background = QStringLiteral("white");
        break;
    }
    for (const LayerItem& li : doc.layers) {
        ProjectLayerMeta meta;
        meta.name = li.name;
        meta.kind = static_cast<int>(li.kind);
        meta.visible = li.visible;
        meta.locked = li.locked;
        meta.opacity = li.opacity;
        meta.fill = li.fill;
        meta.blendMode = li.blendMode;
        meta.offset = li.offset;
        meta.scaleX = li.scaleX;
        meta.scaleY = li.scaleY;
        meta.lockTransparency = li.lockTransparency;
        meta.clipped = li.clipped;
        meta.indent = li.indent;
        meta.isBoard = li.isBoard();
        if (li.isBoard()) {
            meta.boardId = li.boardId;
            meta.boardRect = li.boardRect;
            meta.boardHasFill = li.boardHasFill;
            meta.boardFill = li.boardFill;
            meta.boardFillOpacity = li.boardFillOpacity;
        }
        if (li.hasMask && li.mask && li.mask->width() > 0 &&
            li.mask->height() > 0) {
            meta.hasMask = true;
            meta.maskEnabled = li.maskEnabled;
            meta.maskOffset = li.maskOffset;
            meta.maskScaleX = li.maskScaleX;
            meta.maskScaleY = li.maskScaleY;
            meta.maskDensity = li.maskDensity;
            meta.maskFeather = li.maskFeather;
            meta.mask = imageToStraightRgba64(*li.mask);
        }
        meta.isText = li.isText;
        meta.liveText = li.liveText;
        if (li.liveText) {
            meta.text = li.textSpec.text;
            meta.textFamily = li.textSpec.family;
            meta.textBold = li.textSpec.bold;
            meta.textItalic = li.textSpec.italic;
            meta.textAlign = li.textSpec.align;
            meta.textSize = li.textSpec.size;
            meta.textLineHeight = li.textSpec.lineHeight;
            meta.textEmLeading = li.textSpec.emLeading;
            meta.textParagraphGap = li.textSpec.paragraphGap;
            meta.textTracking = li.textSpec.tracking;
            meta.textWrapWidth = li.textSpec.wrapWidth;
            meta.textFrameHeight = li.textSpec.frameHeight;
            meta.textOrigin = li.textSpec.origin;
            meta.textColor = li.textSpec.color;
            meta.textUnderline = li.textSpec.underline;
            meta.textStrike = li.textSpec.strike;
            meta.textUnderlineColor = li.textSpec.underlineColor;
            meta.textStrikeColor = li.textSpec.strikeColor;
            meta.textBackgroundColor = li.textSpec.backgroundColor;
            meta.textBaselineShift = li.textSpec.baselineShift;
            meta.textHScale = li.textSpec.hScale;
            meta.textVScale = li.textSpec.vScale;
            meta.textSuperSub = li.textSpec.superSub;
            meta.textAllCaps = li.textSpec.allCaps;
            meta.textKerning = li.textSpec.kerning;
            meta.textOtFeatures = li.textSpec.otFeatures;
        }
        meta.art = li.art;
        if (li.kind == LayerItem::Kind::Adjustment && li.adjustmentKind != 0) {
            meta.hasAdjustment = true;
            meta.adjustmentKind = li.adjustmentKind;
            for (int k = 0; k < 16; ++k)
                meta.adjustmentParams[k] = li.adjustmentParams[k];
            meta.adjustmentCurve = li.adjustmentCurve;
            meta.adjustmentCurveR = li.adjustmentCurveR;
            meta.adjustmentCurveG = li.adjustmentCurveG;
            meta.adjustmentCurveB = li.adjustmentCurveB;
        }
        if (li.kind == LayerItem::Kind::Group && li.toneBlendGroup) {
            meta.hasToneBlend = true;
            meta.toneBlendStrength = li.toneBlend.strength;
            meta.toneBlendColor = li.toneBlend.color;
            meta.toneBlendContrast = li.toneBlend.contrast;
            meta.toneBlendLowPass = li.toneBlend.lowPass;
            meta.toneBlendContentType = li.toneBlend.contentType;
        }
        if (li.hasLiveFilter) {
            meta.hasLiveFilter = true;
            meta.liveFilterEnabled = li.liveFilterEnabled;
            meta.liveFilterId = li.liveFilterId;
            meta.liveFilterParams.clear();
            for (double v : li.liveFilterParams)
                meta.liveFilterParams.append(v);
        }
        // Foreign PSD blocks persist only while fresh (see psdRawBlocksFresh):
        // a painted layer's stale descriptors are dropped here, exactly as
        // PSD export drops them, so IFP→PSD never resurrects a lie.
        if (psdRawBlocksFresh(li)) {
            meta.psdRawBlocks.reserve(
                static_cast<int>(li.psdRawBlocks.size()));
            for (const auto& rb : li.psdRawBlocks) {
                ProjectLayerMeta::PsdRawBlock o;
                std::memcpy(o.sig, rb.sig, 4);
                std::memcpy(o.key, rb.key, 4);
                o.data = rb.data;
                o.padding = rb.padding;
                meta.psdRawBlocks.push_back(std::move(o));
            }
        }
        if (li.pixels) {
            meta.pixels = imageToStraightRgba64(*li.pixels);
            if (li.cmykPixels &&
                li.cmykPixels->valid() &&
                li.cmykPixels->width == li.pixels->width() &&
                li.cmykPixels->height == li.pixels->height() &&
                li.cmykPixels->appearanceFingerprint ==
                    pittore::color::CmykImage::fingerprint(*li.pixels) &&
                li.cmykPixels->profileFingerprint ==
                    pittore::color::CmykImage::fingerprint(
                        reinterpret_cast<const std::uint8_t*>(data.iccProfile.constData()),
                        static_cast<std::size_t>(data.iccProfile.size()))) {
                meta.cmyk.reserve(li.cmykPixels->pixels.size() * 4);
                for (const auto& px : li.cmykPixels->pixels)
                    meta.cmyk.insert(meta.cmyk.end(), px.begin(), px.end());
            }
        } else if (hasDeferredPixels(li))
            meta.pixels = stashedToRgba64(li);  // packed, never realised
        data.layers.append(meta);
    }
    data.preview = doc.composite;
    return data;
}

std::size_t documentWorkingSetBytes(const QSize& size) {
    const std::size_t pixels =
        static_cast<std::size_t>(size.width()) * size.height();
    // Background pixels + acc staging + ARGB32 composite (plus headroom for
    // the per-layer device caches, which stay resident on GPU backends).
    return pixels * sizeof(pittore::RGBAf) * 3 + pixels * 4;
}

int layerTokenEnd(const QVector<LayerItem>& layers, int start) {
    if (start < 0 || start >= layers.size()) return start;
    const int base = layers[start].indent;
    int end = start;
    while (end + 1 < layers.size() && layers[end + 1].indent > base) ++end;
    return end;
}

QVector<int> layerMoveUnit(const DocumentItem* d, int* base) {
    QVector<int> sel = d->selectedLayers;
    if (sel.isEmpty()) sel.push_back(d->activeLayer);
    QSet<int> unit;
    for (int idx : sel) {
        if (idx < 0 || idx >= d->layers.size()) continue;
        unit.insert(idx);
        if (d->layers[idx].kind == LayerItem::Kind::Group) {
            const int end = layerTokenEnd(d->layers, idx);
            for (int j = idx; j <= end; ++j) unit.insert(j);
        }
    }
    QVector<int> sorted = unit.values();
    std::sort(sorted.begin(), sorted.end());
    if (base) {
        *base = sorted.isEmpty() ? 0 : d->layers[sorted.first()].indent;
        for (int idx : sorted) *base = std::min(*base, d->layers[idx].indent);
    }
    return sorted;
}

void normalizeLayerIndents(QVector<LayerItem>& layers) {
    for (int i = 0; i < layers.size(); ++i) {
        int d = layers[i].indent;
        if (d < 0) d = 0;
        if (i == 0) {
            // Nothing precedes the first row, so it cannot hang under a group.
            layers[i].indent = 0;
            continue;
        }
        // No step deeper than one past the predecessor.
        const int prev = layers[i - 1].indent;
        if (d > prev + 1) d = prev + 1;
        // A row deeper than its nearest shallower predecessor must hang
        // under a group: pixels cannot parent children. Step back up to a
        // sibling of the pixel that would otherwise shield the row from its
        // group (the scan-back in enclosingGroups stops at such rows).
        while (d > 0) {
            int parent = -1;
            for (int j = i - 1; j >= 0; --j) {
                if (layers[j].indent < d) {
                    parent = j;
                    break;
                }
            }
            if (parent < 0) {
                d = 0;
                break;
            }
            if (layers[parent].kind == LayerItem::Kind::Group) break;
            d = layers[parent].indent;
        }
        layers[i].indent = d;
    }
}


// Decode an imported QImage (premultiplied ARGB32) into a straight-alpha
// engine Image at native resolution. The result is the lossless source a
// placed layer keeps; the compositor resamples from it on the fly. Images
// over 100 MP are refused (a 100 MP RGBAf layer alone is 1.6 GB).
std::shared_ptr<pittore::Image> imageFromQImage(const QImage& in) {
    if (in.isNull()) return nullptr;
    const QImage img = in.format() == QImage::Format_ARGB32_Premultiplied
                           ? in
                           : in.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return nullptr;
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0 ||
        static_cast<qint64>(w) * h > 100'000'000)
        return nullptr;
    auto out = std::make_shared<pittore::Image>(static_cast<std::uint32_t>(w),
                                                 static_cast<std::uint32_t>(h));
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        pittore::RGBAf* dst =
            out->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 0; x < w; ++x) {
            const int a = qAlpha(row[x]);
            if (a <= 0) {
                dst[x] = pittore::RGBAf{0, 0, 0, 0};
                continue;
            }
            const float inv = 1.0f / static_cast<float>(a);
            dst[x] = pittore::RGBAf{qRed(row[x]) * inv, qGreen(row[x]) * inv,
                                     qBlue(row[x]) * inv,
                                     static_cast<float>(a) / 255.0f};
        }
    }
    return out;
}

}  // namespace pittore::ui
