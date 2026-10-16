// Adjustment layers: the parameter table (names, ranges and defaults) and the
// AppState entry points that add, re-parameterise, reset and preview them.
// Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/adjust.h"

namespace pittore::ui {

QString adjustmentKindName(int kind) {
    using pittore::compute::AdjustmentKind;
    switch (static_cast<AdjustmentKind>(kind)) {
        case AdjustmentKind::BrightnessContrast:
            return QStringLiteral("Brightness/Contrast");
        case AdjustmentKind::Levels: return QStringLiteral("Levels");
        case AdjustmentKind::Curves: return QStringLiteral("Curves");
        case AdjustmentKind::Exposure: return QStringLiteral("Exposure");
        case AdjustmentKind::Vibrance: return QStringLiteral("Vibrance");
        case AdjustmentKind::HueSaturation:
            return QStringLiteral("Hue/Saturation");
        case AdjustmentKind::Invert: return QStringLiteral("Invert");
        case AdjustmentKind::Threshold: return QStringLiteral("Threshold");
        case AdjustmentKind::Posterize: return QStringLiteral("Posterize");
        case AdjustmentKind::PhotoFilter:
            return QStringLiteral("Photo Filter");
        case AdjustmentKind::WhiteBalance:
            return QStringLiteral("White Balance");
        case AdjustmentKind::BlackWhite:
            return QStringLiteral("Black & White");
        case AdjustmentKind::ChannelMixer:
            return QStringLiteral("Channel Mixer");
        case AdjustmentKind::ColorBalance:
            return QStringLiteral("Color Balance");
        case AdjustmentKind::None: break;
    }
    return QString();
}

QVector<AdjustmentParamDesc> adjustmentParamDescs(int kind) {
    using pittore::compute::AdjustmentKind;
    const AdjustmentKind k = static_cast<AdjustmentKind>(kind);
    if (k == AdjustmentKind::BrightnessContrast)
        return {{"Brightness", -100, 100, -1.0, 1.0},
                {"Contrast", -100, 100, -1.0, 1.0}};
    if (k == AdjustmentKind::Levels)
        return {{"Input Black", 0, 255, 0.0, 1.0},
                {"Input White", 0, 255, 0.0, 1.0},
                {"Gamma", 10, 999, 0.1, 9.99},
                {"Output Black", 0, 255, 0.0, 1.0},
                {"Output White", 0, 255, 0.0, 1.0}};
    if (k == AdjustmentKind::Exposure)
        return {{"Exposure", -500, 500, -5.0, 5.0}};
    if (k == AdjustmentKind::Vibrance)
        return {{"Vibrance", -100, 100, -1.0, 1.0}};
    if (k == AdjustmentKind::HueSaturation)
        return {{"Hue", -180, 180, -180.0, 180.0},
                {"Saturation", -100, 100, -1.0, 1.0},
                {"Lightness", -100, 100, -1.0, 1.0}};
    if (k == AdjustmentKind::Threshold)
        return {{"Threshold", 0, 255, 0.0, 1.0}};
    if (k == AdjustmentKind::Posterize)
        return {{"Levels", 2, 255, 2.0, 255.0}};
    if (k == AdjustmentKind::PhotoFilter)
        return {{"Filter Red", 0, 255, 0.0, 1.0},
                {"Filter Green", 0, 255, 0.0, 1.0},
                {"Filter Blue", 0, 255, 0.0, 1.0},
                {"Density", 1, 100, 0.01, 1.0},
                {"Preserve Luminosity", 0, 1, 0.0, 1.0}};
    if (k == AdjustmentKind::WhiteBalance)
        return {{"Temperature", 2000, 10000, 2000.0, 10000.0},
                {"Tint", -100, 100, -1.0, 1.0}};
    if (k == AdjustmentKind::BlackWhite)
        return {{"Reds", -200, 300, -200.0, 300.0},
                {"Yellows", -200, 300, -200.0, 300.0},
                {"Greens", -200, 300, -200.0, 300.0},
                {"Cyans", -200, 300, -200.0, 300.0},
                {"Blues", -200, 300, -200.0, 300.0},
                {"Magentas", -200, 300, -200.0, 300.0}};
    if (k == AdjustmentKind::ChannelMixer)
        return {{"Red Out Red", -200, 200, -2.0, 2.0},
                {"Red Out Green", -200, 200, -2.0, 2.0},
                {"Red Out Blue", -200, 200, -2.0, 2.0},
                {"Green Out Red", -200, 200, -2.0, 2.0},
                {"Green Out Green", -200, 200, -2.0, 2.0},
                {"Green Out Blue", -200, 200, -2.0, 2.0},
                {"Blue Out Red", -200, 200, -2.0, 2.0},
                {"Blue Out Green", -200, 200, -2.0, 2.0},
                {"Blue Out Blue", -200, 200, -2.0, 2.0},
                {"Constant Red", -200, 200, -2.0, 2.0},
                {"Constant Green", -200, 200, -2.0, 2.0},
                {"Constant Blue", -200, 200, -2.0, 2.0},
                {"Monochrome", 0, 1, 0.0, 1.0}};
    if (k == AdjustmentKind::ColorBalance)
        return {{"Shadows Cyan-Red", -100, 100, -100.0, 100.0},
                {"Shadows Magenta-Green", -100, 100, -100.0, 100.0},
                {"Shadows Yellow-Blue", -100, 100, -100.0, 100.0},
                {"Midtones Cyan-Red", -100, 100, -100.0, 100.0},
                {"Midtones Magenta-Green", -100, 100, -100.0, 100.0},
                {"Midtones Yellow-Blue", -100, 100, -100.0, 100.0},
                {"Highlights Cyan-Red", -100, 100, -100.0, 100.0},
                {"Highlights Magenta-Green", -100, 100, -100.0, 100.0},
                {"Highlights Yellow-Blue", -100, 100, -100.0, 100.0},
                {"Preserve Luminosity", 0, 1, 0.0, 1.0}};
    return {};
}

bool AppState::addAdjustmentLayer(int kind) {
    using pittore::compute::AdjustmentKind;
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const QString name = adjustmentKindName(kind);
    if (name.isEmpty()) {
        setStatusHint(tr("Unknown adjustment type."));
        return false;
    }
    LayerItem layer;
    layer.kind = LayerItem::Kind::Adjustment;
    layer.name = name;
    layer.adjustmentType = name;
    layer.adjustmentKind = kind;
    layer.swatch = QColor(0x9a, 0x72, 0xd0);
    setAdjustmentDefaults(layer);
    beginUndoStep();
    addLayer(std::move(layer));  // re-composites, selects, signals
    commitUndoStep(name, QStringLiteral("adjustments"));
    emit historyChanged();
    return true;
}

bool AppState::setAdjustmentParamAt(int layerIndex, int paramIndex,
                                     float value) {
    DocumentItem* d = activeDocument();
    if (!d || layerIndex < 0 || layerIndex >= d->layers.size()) return false;
    LayerItem* l = &d->layers[layerIndex];
    if (l->kind != LayerItem::Kind::Adjustment || paramIndex < 0 ||
        paramIndex > 15)
        return false;
    l->adjustmentParams[paramIndex] = value;
    if (l->adjustmentKind ==
        static_cast<int>(pittore::compute::AdjustmentKind::Levels))
        rebuildAdjustmentLUT(*l);  // table follows the params
    else
        ++l->adjustStamp;
    d->rebuildComposite();
    emit documentModified(d);
    return true;
}

bool AppState::setAdjustmentParam(int index, float value) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    return setAdjustmentParamAt(d->activeLayer, index, value);
}

bool AppState::setAdjustmentCurve(const QVector<QPointF>& points) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l || l->kind != LayerItem::Kind::Adjustment ||
        l->adjustmentKind != static_cast<int>(
                                pittore::compute::AdjustmentKind::Curves))
        return false;
    l->adjustmentCurve = points;
    rebuildAdjustmentLUT(*l);
    d->rebuildComposite();
    emit documentModified(d);
    return true;
}

// Replace one Curves channel's control points (ch 0 = R, 1 = G, 2 = B),
// rebuild the folded LUT and recomposite. Live write; the caller holds the
// undo step (single edit or drag gesture).
bool AppState::setAdjustmentCurveForChannel(int ch,
                                            const QVector<QPointF>& points) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l || l->kind != LayerItem::Kind::Adjustment ||
        l->adjustmentKind != static_cast<int>(
                                pittore::compute::AdjustmentKind::Curves) ||
        ch < 0 || ch > 2)
        return false;
    if (ch == 0)
        l->adjustmentCurveR = points;
    else if (ch == 1)
        l->adjustmentCurveG = points;
    else
        l->adjustmentCurveB = points;
    rebuildAdjustmentLUT(*l);
    d->rebuildComposite();
    emit documentModified(d);
    return true;
}

bool AppState::resetAdjustmentAt(int layerIndex) {
    using pittore::compute::AdjustmentKind;
    DocumentItem* d = activeDocument();
    if (!d || layerIndex < 0 || layerIndex >= d->layers.size()) return false;
    LayerItem* l = &d->layers[layerIndex];
    if (l->kind != LayerItem::Kind::Adjustment ||
        l->adjustmentKind ==
            static_cast<int>(AdjustmentKind::None))
        return false;
    setAdjustmentDefaults(*l);
    const AdjustmentKind k =
        static_cast<AdjustmentKind>(l->adjustmentKind);
    if (k == AdjustmentKind::Levels || k == AdjustmentKind::Curves)
        rebuildAdjustmentLUT(*l);  // table follows the params
    else
        ++l->adjustStamp;
    d->rebuildComposite();
    emit documentModified(d);
    return true;
}

bool AppState::resetAdjustmentToDefaults() {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    return resetAdjustmentAt(d->activeLayer);
}

void AppState::beginAdjustmentPreview() {
    using pittore::compute::AdjustmentKind;
    if (previewAdjustmentIndex_ != -1) return;
    DocumentItem* d = activeDocument();
    if (!d) return;
    const int i = d->activeLayer;
    if (i < 0 || i >= d->layers.size()) return;
    LayerItem& l = d->layers[i];
    if (l.kind != LayerItem::Kind::Adjustment || !l.visible) return;
    previewAdjustmentIndex_ = i;
    previewWasVisible_ = l.visible;
    l.visible = false;
    d->rebuildComposite();
    // documentModified only: layersChanged would rebuild the Properties panel
    // out from under the held compare button (losing the release event and
    // stranding the row hidden).
    emit documentModified(d);
}

void AppState::endAdjustmentPreview() {
    if (previewAdjustmentIndex_ == -1) return;
    DocumentItem* d = activeDocument();
    const int i = previewAdjustmentIndex_;
    previewAdjustmentIndex_ = -1;
    if (!d || i < 0 || i >= d->layers.size()) return;
    LayerItem& l = d->layers[i];
    if (l.kind != LayerItem::Kind::Adjustment) return;
    l.visible = previewWasVisible_;
    d->rebuildComposite();
    emit documentModified(d);
}

}  // namespace pittore::ui
