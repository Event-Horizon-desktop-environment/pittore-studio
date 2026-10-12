// Layer masks: copy-on-write before a mask stroke, add/fill/delete/invert and
// the enable / link / target switches. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/layer_mask.h"

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

}  // namespace pittore::ui
