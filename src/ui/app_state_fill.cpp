// Flood fill, paint bucket and magic eraser.
//
// Split out of app_state.cpp; AppState members, no extra declarations needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"

#include "engine/compute/brushes/flood/flood.h"

#include <algorithm>
namespace pittore::ui {

bool AppState::floodEdit(const QPointF& docPos, bool erase) {
    const ToolId tool = activeTool();
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;

    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int sx = static_cast<int>(
        std::floor((docPos.x() - layer->offset.x()) / lsx));
    const int sy = static_cast<int>(
        std::floor((docPos.y() - layer->offset.y()) / lsy));

    // Snapshot + private pixel copy before mutating, exactly like a paint
    // stroke, so undo restores the pre-fill pixels.
    beginUndoStep();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector: drop the pre-COW
    // pointer and re-resolve so the fill lands on the live copy, not the
    // snapshot's shared image.
    layer = activeLayer();
    if (!layer) {
        discardUndoStep();
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) {
        discardUndoStep();
        return false;
    }

    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0 || sx < 0 || sy < 0 ||
        sx >= static_cast<int>(lw) || sy >= static_cast<int>(lh)) {
        discardUndoStep();
        return false;
    }

    // The region is measured in the layer's own pixel space. With Sample All
    // Layers it is measured through the document composite: each layer texel is
    // looked up at its document position (nearest neighbour — the composite is
    // already a resampled render, so a further filter would only blur it).
    std::vector<pittore::RGBAf> matchBuf;
    const pittore::RGBAf* match = layer->pixels->data();
    if (option(tool, QStringLiteral("sample_all")).toBool() &&
        !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        matchBuf.resize(static_cast<std::size_t>(lw) * lh);
        for (std::uint32_t y = 0; y < lh; ++y) {
            const double docY = layer->offset.y() + (static_cast<double>(y) + 0.5) * lsy;
            const int py = std::clamp(static_cast<int>(std::floor(docY)), 0,
                                      comp.height() - 1);
            const QRgb* row = reinterpret_cast<const QRgb*>(comp.constScanLine(py));
            for (std::uint32_t x = 0; x < lw; ++x) {
                const double docX =
                    layer->offset.x() + (static_cast<double>(x) + 0.5) * lsx;
                const int px = std::clamp(static_cast<int>(std::floor(docX)), 0,
                                          comp.width() - 1);
                const QRgb c = row[px];
                const int a = qAlpha(c);
                pittore::RGBAf& dst =
                    matchBuf[static_cast<std::size_t>(y) * lw + x];
                if (a <= 0) {
                    dst = pittore::RGBAf{0, 0, 0, 0};
                } else {
                    const float inv = 1.0f / static_cast<float>(a);
                    dst = pittore::RGBAf{qRed(c) * inv, qGreen(c) * inv,
                                          qBlue(c) * inv,
                                          static_cast<float>(a) / 255.0f};
                }
            }
        }
        match = matchBuf.data();
    }

    const float tol = static_cast<float>(std::clamp(
        option(tool, QStringLiteral("tolerance")).toDouble() / 255.0, 0.0, 1.0));
    const bool contiguous = option(tool, QStringLiteral("contiguous")).toBool();
    const bool antialias = option(tool, QStringLiteral("antialias")).toBool();
    const float opacity = static_cast<float>(std::clamp(
        option(tool, QStringLiteral("opacity")).toDouble() / 100.0, 0.0, 1.0));
    const QColor fg = foreground();

    // The engine fills source-over only. Say so instead of silently ignoring a
    // non-Normal Mode or the Pattern source the options bar offers.
    if (tool == ToolId::PaintBucket) {
        if (option(tool, QStringLiteral("mode")).toInt() != 0)
            setStatusHint(tr("Paint Bucket: only Normal blend is supported."));
        if (option(tool, QStringLiteral("fillsource")).toInt() == 1)
            setStatusHint(tr("Paint Bucket: pattern fill is not supported; "
                             "using the foreground colour."));
    }

    int bbox[4] = {0, 0, 0, 0};
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    pittore::compute::NativeCmykPaint nativeFill;
    pittore::compute::NativeCmykPaint* nativeFillPtr = nullptr;
    if (!erase && d->colorMode.startsWith(QStringLiteral("CMYK"),
                                           Qt::CaseInsensitive) &&
        prepareNativeCmykPaint(*d, *layer, fg, nativeFill, false))
        nativeFillPtr = &nativeFill;
    const bool changed = pittore::compute::flood_fill_host(
        match, layer->pixels->data(), lw, lh, sx, sy, tol, contiguous, antialias,
        opacity,
        pittore::RGBAf{fg.redF(), fg.greenF(), fg.blueF(), 1.0f}, erase, bbox,
        selPtr, nativeFillPtr);
    if (!changed) {
        discardUndoStep();
        return false;
    }

    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    if (nativeFillPtr) {
        layer->cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(*layer->pixels);
        layer->cmykSourceStamp = layer->sourceStamp;
    }
    layer->thumbnail = QImage();

    const double dx0 = layer->offset.x() + bbox[0] * lsx;
    const double dy0 = layer->offset.y() + bbox[1] * lsy;
    const double dx1 = layer->offset.x() + bbox[2] * lsx;
    const double dy1 = layer->offset.y() + bbox[3] * lsy;
    const QRect dirty =
        QRect(int(std::floor(std::min(dx0, dx1))) - 1,
              int(std::floor(std::min(dy0, dy1))) - 1,
              int(std::ceil(std::fabs(dx1 - dx0))) + 3,
              int(std::ceil(std::fabs(dy1 - dy0))) + 3)
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dirty : d->paintDirty.united(dirty);
    flushPaint();
    commitUndoStep(erase ? tr("Magic Eraser") : tr("Paint Bucket"),
                   erase ? QStringLiteral("magic-eraser")
                         : QStringLiteral("paint-bucket"));
    return true;
}

bool AppState::paintBucketAt(const QPointF& docPos) {
    return floodEdit(docPos, /*erase=*/false);
}

bool AppState::magicEraseAt(const QPointF& docPos) {
    return floodEdit(docPos, /*erase=*/true);
}

}  // namespace pittore::ui
