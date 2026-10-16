// Layer masks: copy-on-write before a mask stroke, add/fill/delete/invert and
// the enable / link / target switches. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/layer_mask.h"
#include "ui/color_mode.h"

#include <algorithm>
#include <cstddef>
#include <memory>

namespace pittore::ui {

bool AppState::copyOnWriteActiveLayer() {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l || l->kind != LayerItem::Kind::Pixel || !l->pixels) return false;
    // New Image pointer → the engine's placed-source cache re-uploads once;
    // past snapshots keep the original object untouched.
    l->pixels = std::make_shared<pittore::Image>(l->pixels->clone());
    // CMYK samples participate in the same undo snapshots and must detach
    // before an edit can refresh their ink values.
    if (l->cmykPixels)
        l->cmykPixels = std::make_shared<pittore::color::CmykImage>(
            *l->cmykPixels);
    return true;
}

bool AppState::copyOnWriteActiveMask() {
    LayerItem* l = activeLayer();
    if (!l || (l->kind != LayerItem::Kind::Pixel &&
               l->kind != LayerItem::Kind::Adjustment) ||
        !l->mask)
        return false;
    // Masks share Images across snapshots exactly like pixels, so clone
    // before an in-place mask stroke. The new pointer also refreshes the
    // mask's device-cache slot.
    l->mask = std::make_shared<pittore::Image>(l->mask->clone());
    return true;
}

bool AppState::copyOnWriteActiveHeight() {
    LayerItem* l = activeLayer();
    if (!l || l->kind != LayerItem::Kind::Pixel || !l->heightMap)
        return false;
    // Relief shares its plane with undo snapshots exactly like pixels.
    l->heightMap = std::make_shared<std::vector<float>>(*l->heightMap);
    return true;
}

bool AppState::addLayerMask(float coverage) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer ||
        (layer->kind != LayerItem::Kind::Pixel &&
         layer->kind != LayerItem::Kind::Adjustment) ||
        layer->locked) {
        setStatusHint(tr("Add Mask needs an unlocked pixel or adjustment layer."));
        return false;
    }
    if (layer->hasMask && layer->mask) {
        setLayerMaskSelected(d->activeLayer, true);
        return true;
    }
    beginUndoStep();
    layer = activeLayer();
    if (!layer || !ensureLayerMask(*d, *layer, std::clamp(coverage, 0.0f, 1.0f))) {
        discardUndoStep();
        setStatusHint(tr("Could not add a mask to this layer."));
        return false;
    }
    layer->maskSelected = true;
    commitUndoStep(coverage <= 0.5f ? tr("Add Hide-All Layer Mask")
                                    : tr("Add Reveal-All Layer Mask"),
                   QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    setStatusHint(tr("Painting on the layer mask (black conceals, white reveals)."));
    return true;
}

bool AppState::fillLayerMask(float coverage) {
    DocumentItem* d = activeDocument();
    if (!d || !activeLayer() ||
        (activeLayer()->kind != LayerItem::Kind::Pixel &&
         activeLayer()->kind != LayerItem::Kind::Adjustment) ||
        activeLayer()->locked) {
        setStatusHint(tr("Fill Mask needs an unlocked pixel or adjustment layer."));
        return false;
    }
    beginUndoStep();
    LayerItem* layer = activeLayer();
    if (!layer || !ensureLayerMask(*d, *layer, 1.0f) || !copyOnWriteActiveMask()) {
        discardUndoStep();
        setStatusHint(tr("Could not fill this layer mask."));
        return false;
    }
    layer = activeLayer();
    const float c = std::clamp(coverage, 0.0f, 1.0f);
    layer->mask->fill(pittore::compute::make_mask_pixel(c));
    ++layer->maskStamp;
    layer->maskSelected = true;
    commitUndoStep(c <= 0.5f ? tr("Hide-All Layer Mask") : tr("Reveal-All Layer Mask"),
                   QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

bool AppState::deleteLayerMask() {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->hasMask || !layer->mask) {
        setStatusHint(tr("The active layer has no mask to delete."));
        return false;
    }
    beginUndoStep();
    layer = activeLayer();
    layer->mask.reset();
    layer->hasMask = false;
    layer->maskSelected = false;
    layer->maskEnabled = true;
    layer->maskLinked = true;
    layer->maskOffset = QPointF(0, 0);
    layer->maskScaleX = layer->maskScaleY = 1.0;
    ++layer->maskStamp;
    commitUndoStep(tr("Delete Layer Mask"), QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

bool AppState::invertLayerMask() {
    DocumentItem* d = activeDocument();
    if (!d || !activeLayer() || !activeLayer()->mask) {
        setStatusHint(tr("The active layer has no mask to invert."));
        return false;
    }
    beginUndoStep();
    if (!copyOnWriteActiveMask()) {
        discardUndoStep();
        return false;
    }
    LayerItem* layer = activeLayer();
    pittore::RGBAf* px = layer->mask->data();
    for (std::size_t i = 0, n = layer->mask->pixel_count(); i < n; ++i) {
        const float c = 1.0f - std::clamp(px[i].r, 0.0f, 1.0f);
        px[i] = pittore::compute::make_mask_pixel(c);
    }
    ++layer->maskStamp;
    commitUndoStep(tr("Invert Layer Mask"), QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setLayerMaskEnabled(bool enabled) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->hasMask || !layer->mask ||
        layer->maskEnabled == enabled) {
        if (d && layer && (!layer->hasMask || !layer->mask))
            setStatusHint(tr("The active layer has no mask to enable."));
        return false;
    }
    beginUndoStep();
    activeLayer()->maskEnabled = enabled;
    commitUndoStep(enabled ? tr("Enable Layer Mask") : tr("Disable Layer Mask"),
                   QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setLayerMaskLinked(bool linked) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->hasMask || !layer->mask ||
        layer->maskLinked == linked) {
        if (d && layer && (!layer->hasMask || !layer->mask))
            setStatusHint(tr("The active layer has no mask to link."));
        return false;
    }
    beginUndoStep();
    layer = activeLayer();
    layer->maskLinked = linked;
    syncLinkedMaskPlacement(*layer);
    commitUndoStep(linked ? tr("Link Layer Mask") : tr("Unlink Layer Mask"),
                   QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setLayerMaskSelected(int index, bool selected) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size()) return false;
    LayerItem& layer = d->layers[index];
    if (!layer.hasMask || !layer.mask) return false;
    for (int i = 0; i < d->layers.size(); ++i)
        d->layers[i].maskSelected = selected && i == index;
    emit activeLayerChanged();
    setStatusHint(selected ? tr("Painting on the layer mask (black conceals, white reveals).")
                           : tr("Painting on layer pixels."));
    return true;
}


bool AppState::applyActiveLayerAlpha(const float* mask, int width, int height,
                                     const QString& historyName,
                                     const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels) return false;
    if (!mask || width <= 0 || height <= 0) return false;
    if (static_cast<std::uint32_t>(width) != layer->pixels->width() ||
        static_cast<std::uint32_t>(height) != layer->pixels->height())
        return false;

    // One undo step for the whole alpha edit; the snapshot shares the current
    // image, so copy-on-write BEFORE the in-place multiply (or undoing would
    // restore the already-multiplied pixels).
    d->beginUndoAction();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector (the snapshot shares it),
    // so the pointer taken before it is stale: re-resolve before touching the
    // pixels. Without this the multiply lands on the snapshot's shared image and
    // the live layer — and therefore the canvas — never changes.
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }

    // Straight-alpha edit: multiply alpha down by the soft mask. RGB is kept
    // untouched so the compositor's straight-alpha blend still sees true colour
    // (fringe cleanup against the removed background is future work).
    pittore::RGBAf* px = layer->pixels->data();
    const std::size_t n = layer->pixels->pixel_count();
    for (std::size_t i = 0; i < n; ++i)
        px[i].a *= std::clamp(mask[i], 0.0f, 1.0f);

    ++layer->sourceStamp;           // native pixels changed in place
    layer->thumbnail = QImage();    // …and a stale panel preview
    const QRect bounds =
        layerBounds(*d, *layer).toAlignedRect().intersected(QRect(QPoint(0, 0), d->size));
    if (bounds.isEmpty())
        d->rebuildComposite();
    else
        d->renderRegion(bounds);
    d->commitUndoAction(historyName, iconKey);
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::replaceActiveLayerRgba(const QImage& rgba,
                                      const QString& historyName,
                                      const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel ||
        !layer->pixels)
        return false;
    if (rgba.isNull() || rgba.format() != QImage::Format_RGBA8888) return false;
    if (static_cast<std::uint32_t>(rgba.width()) != layer->pixels->width() ||
        static_cast<std::uint32_t>(rgba.height()) != layer->pixels->height())
        return false;

    d->beginUndoAction();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }

    pittore::RGBAf* px = layer->pixels->data();
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* s = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            const std::size_t i = std::size_t(y) * rgba.width() + x;
            px[i].r = s[x * 4 + 0] / 255.0f;
            px[i].g = s[x * 4 + 1] / 255.0f;
            px[i].b = s[x * 4 + 2] / 255.0f;
            px[i].a = s[x * 4 + 3] / 255.0f;
        }
    }
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer);
    layer->thumbnail = QImage();
    const QRect bounds =
        layerBounds(*d, *layer).toAlignedRect().intersected(QRect(QPoint(0, 0), d->size));
    if (bounds.isEmpty())
        d->rebuildComposite();
    else
        d->renderRegion(bounds);
    d->commitUndoAction(historyName, iconKey);
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::eraseSelectionFromActiveLayer() {
    DocumentItem* d = activeDocument();
    if (!d || !d->selectionIsMask || d->selectionMask.isNull() ||
        d->selection.isEmpty())
        return false;
    const QRect selRect =
        d->selection.toAlignedRect().intersected(QRect(QPoint(0, 0), d->size));
    if (selRect.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel ||
        !layer->pixels)
        return false;

    d->beginUndoAction();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector (the snapshot shares it):
    // re-resolve so the erase lands on the live clone, not the snapshot's image.
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }

    // Erase by mapping every selected doc pixel back to its layer pixel
    // (inverse transform, exactly how selectionMaskFromLayerAlpha builds the
    // channel) and multiplying that pixel's alpha by the complement of the
    // mask coverage. Multiple doc pixels can land on one layer pixel when the
    // layer is scaled down; accumulated factors keep the result consistent.
    const QImage& m = d->selectionMask;
    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);
    pittore::RGBAf* px = layer->pixels->data();
    for (int dy = selRect.top(); dy <= selRect.bottom(); ++dy) {
        const uchar* mrow = m.constScanLine(dy);
        for (int dx = selRect.left(); dx <= selRect.right(); ++dx) {
            const int cov = mrow[dx];
            if (!cov) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            px[std::size_t(ly) * pw + lx].a *= (255 - cov) / 255.0;
        }
    }

    ++layer->sourceStamp;           // native pixels changed in place
    layer->thumbnail = QImage();    // …stale panel preview
    d->renderRegion(selRect);
    d->commitUndoAction(tr("Delete"), QStringLiteral("eraser"));
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::fillActiveSelectionWithColor(const QColor& color,
                                            const QString& undoName) {
    DocumentItem* d = activeDocument();
    if (!d || !color.isValid()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel)
        return false;
    // Realize an empty layer so Fill works on a fresh Layer (fill covers
    // the whole layer when nothing is selected).
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;

    // Coverage source: the live selection (mask or rect/ellipse rasterised),
    // or full coverage when nothing is selected.
    QRect clipRect;
    QImage covMask;
    bool fullDoc = false;
    if (!d->selection.isEmpty() || (d->selectionIsMask && !d->selectionMask.isNull())) {
        covMask = selectionAsMask(*d);
        if (covMask.isNull()) return false;
        clipRect = d->selection.toAlignedRect()
                       .intersected(QRect(QPoint(0, 0), d->size));
        if (clipRect.isEmpty()) return false;
    } else {
        clipRect = QRect(QPoint(0, 0), d->size);
        fullDoc = true;
    }
    if (clipRect.isEmpty()) return false;

    d->beginUndoAction();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }
    const float fr = color.red() / 255.0f;
    const float fg = color.green() / 255.0f;
    const float fb = color.blue() / 255.0f;
    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);
    pittore::RGBAf* px = layer->pixels->data();
    const bool hasMask = !covMask.isNull();
    const bool maskSized = hasMask && covMask.size() == d->size;
    pittore::compute::NativeCmykPaint nativeFill;
    const bool nativeCmyk = prepareNativeCmykPaint(
        *d, *layer, color, nativeFill, false);
    int nativeX0 = pw, nativeY0 = ph, nativeX1 = -1, nativeY1 = -1;
    for (int dy = clipRect.top(); dy <= clipRect.bottom(); ++dy) {
        const uchar* mrow = maskSized ? covMask.constScanLine(dy) : nullptr;
        for (int dx = clipRect.left(); dx <= clipRect.right(); ++dx) {
            const float cov = fullDoc ? 1.0f : (mrow[dx] / 255.0f);
            if (cov <= 0.0f) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            const std::size_t index = std::size_t(ly) * pw + lx;
            pittore::RGBAf& p = px[index];
            const float oldAlpha = p.a;
            // Straight-alpha "over": opaque FG over dst by coverage.
            const float na = cov + oldAlpha * (1.0f - cov);
            if (na > 1e-6f) {
                p.r = (fr * cov + p.r * oldAlpha * (1.0f - cov)) / na;
                p.g = (fg * cov + p.g * oldAlpha * (1.0f - cov)) / na;
                p.b = (fb * cov + p.b * oldAlpha * (1.0f - cov)) / na;
                p.a = na;
            } else {
                p.r = fr;
                p.g = fg;
                p.b = fb;
                p.a = 0.0f;
            }
            if (nativeCmyk && nativeFill.pixels) {
                auto& inks = nativeFill.pixels[index];
                for (int c = 0; c < 4; ++c) {
                    const float oldInk = inks[c] / 65535.0f;
                    const float srcInk = nativeFill.source[c] / 65535.0f;
                    const float outInk = na > 1e-6f
                        ? (srcInk * cov + oldInk * oldAlpha * (1.0f - cov)) / na
                        : srcInk;
                    inks[c] = static_cast<std::uint16_t>(std::clamp(
                        std::lround(outInk * 65535.0f), 0L, 65535L));
                }
                nativeX0 = std::min(nativeX0, lx);
                nativeY0 = std::min(nativeY0, ly);
                nativeX1 = std::max(nativeX1, lx);
                nativeY1 = std::max(nativeY1, ly);
            }
        }
    }
    ++layer->sourceStamp;
    if (nativeCmyk && nativeX1 >= nativeX0 && nativeY1 >= nativeY0) {
        layer->cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(*layer->pixels);
        layer->cmykSourceStamp = layer->sourceStamp;
    } else {
        refreshLayerCmyk(*d, *layer);
    }
    layer->thumbnail = QImage();
    d->renderRegion(clipRect);
    d->commitUndoAction(undoName.isEmpty() ? tr("Fill") : undoName,
                        QStringLiteral("fill"));
    emit historyChanged();
    emit documentModified(d);
    return true;
}

}  // namespace pittore::ui
