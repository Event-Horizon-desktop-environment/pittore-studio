// Adjustment layers: the parameter table (names, ranges and defaults) and the
// AppState entry points that add, re-parameterise, reset and preview them.
// Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/adjust.h"
#include "engine/compute/backend.h"
#include "engine/compute/brushes/adjust/adjust_dab.h"
#include "engine/compute/brushes/replace/replace.h"
#include "engine/core/log.h"
#include "ui/color_mode.h"

#include <algorithm>
#include <chrono>

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


// Aux-table footprint for an adjustment layer: 3x256 floats for Curves
// (R/G/B), 256 for Levels, 0 (no table) otherwise.
std::size_t adjustmentAuxSize(const LayerItem& l) {
    if (l.kind != LayerItem::Kind::Adjustment) return 0;
    if (l.adjustmentKind == static_cast<int>(
                                pittore::compute::AdjustmentKind::Curves))
        return 768;
    if (l.adjustmentKind == static_cast<int>(
                                pittore::compute::AdjustmentKind::Levels))
        return 256;
    return 0;
}

void rebuildAdjustmentLUT(LayerItem& l) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto toPairs = [](const QVector<QPointF>& q) {
        std::vector<std::pair<double, double>> pts;
        pts.reserve(static_cast<std::size_t>(q.size()));
        for (const QPointF& p : q) pts.emplace_back(p.x(), p.y());
        return pts;
    };
    if (l.adjustmentKind == static_cast<int>(
                                pittore::compute::AdjustmentKind::Levels)) {
        // Tabled Levels: one 256-entry map so the composite samples instead
        // of running pow() per channel per pixel (see adjust.h).
        l.adjustmentLUT.assign(256, 0.0f);
        pittore::buildLevelsLUT(l.adjustmentParams, l.adjustmentLUT.data());
        ++l.adjustStamp;
        const double lms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
        if (lms >= pittore::core::log::slowEventMs())
            PITTORE_LOG("[grade] rebuildLUT kind=%d ms=%.3f",
                         l.adjustmentKind, lms);
        return;
    }
    // Standard order: each channel's own curve first, then the RGB
    // composite (master). Pre-folded here so the engine keeps sampling one
    // table per channel (adjustmentLUT is 3x256: R, G, B back to back).
    float master[256];
    pittore::buildCurveLUT(toPairs(l.adjustmentCurve), master);
    l.adjustmentLUT.assign(768, 0.0f);
    for (int c = 0; c < 3; ++c) {
        const QVector<QPointF>& qp =
            c == 0 ? l.adjustmentCurveR
                   : (c == 1 ? l.adjustmentCurveG : l.adjustmentCurveB);
        float ch[256];
        pittore::buildCurveLUT(toPairs(qp), ch);
        float* dst = l.adjustmentLUT.data() + 256 * c;
        for (int i = 0; i < 256; ++i) {
            const float v = std::clamp(ch[i], 0.0f, 1.0f);
            const int bin =
                std::clamp(static_cast<int>(v * 255.0f + 0.5f), 0, 255);
            dst[i] = master[bin];
        }
    }
    ++l.adjustStamp;
    // 256-entry builds: expect microseconds. Slow here means per-tick LUT
    // churn worth attributing, so slow builds always report.
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (ms >= pittore::core::log::slowEventMs())
        PITTORE_LOG("[grade] rebuildLUT kind=%d ms=%.3f",
                     l.adjustmentKind, ms);
}

bool AppState::adjustmentBrushDab(const QPointF& docPos, double radius,
                                  double hardness, double strength,
                                  int adjIndex) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Adjustment Brush: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    // Fixed documented strengths per adjustment (the bar carries
    // opacity/flow; amount curves are dialog work).
    using Kind = pittore::compute::AdjustmentKind;
    Kind kind = Kind::None;
    float p[16] = {};
    if (adjIndex == 0) {
        kind = Kind::BrightnessContrast;
        p[0] = 0.12f;
        p[1] = 0.10f;
    } else if (adjIndex == 3) {
        kind = Kind::Exposure;
        p[0] = 0.5f;
    } else if (adjIndex == 5) {
        kind = Kind::HueSaturation;
        p[0] = 0.0f;
        p[1] = 0.25f;
        p[2] = 0.0f;
    } else {
        setStatusHint(tr("Adjustment Brush: that adjustment is planned."));
        return false;
    }
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    const float hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const float st = static_cast<float>(std::clamp(strength, 0.0, 1.0));
    if (!(st > 0.0f)) return false;
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    bool changed = false;
    pittore::compute::ComputeBackend& be = computeBackend();
    // Device fastpath on bbox buffers (CUDA only: HIP has no
    // brightness/hue kernels yet, and Exposure has no device virtual at
    // all — both stay on the shared core below).
    const bool gpu =
        be.type() == pittore::compute::BackendType::CUDA &&
        (kind == Kind::BrightnessContrast || kind == Kind::HueSaturation);
    if (gpu) {
        // Device fastpath on bbox buffers (the virtuals upload/download
        // internally): adjust fully, then rim-mix on the host. Exposure
        // has no device virtual and stays on the shared core below.
        const int bx0 = std::max(0, int(std::floor(lcx - lradius)));
        const int by0 = std::max(0, int(std::floor(lcy - lradius)));
        const int bx1 =
            std::min(int(lw), int(std::ceil(lcx + lradius)));
        const int by1 =
            std::min(int(lh), int(std::ceil(lcy + lradius)));
        if (bx1 > bx0 && by1 > by0) {
            const int bw = bx1 - bx0, bh = by1 - by0;
            const std::size_t n = std::size_t(bw) * bh;
            auto src = be.make_buffer(n * sizeof(pittore::RGBAf));
            auto dst = be.make_buffer(n * sizeof(pittore::RGBAf));
            if (src && dst) {
                for (int y = 0; y < bh; ++y)
                    std::memcpy(
                        static_cast<pittore::RGBAf*>(src->host()) +
                            std::size_t(y) * bw,
                        layer->pixels->data() +
                            std::size_t(by0 + y) * lw + bx0,
                        std::size_t(bw) * sizeof(pittore::RGBAf));
                if (kind == Kind::BrightnessContrast)
                    be.brightness_contrast(*src, *dst, std::uint32_t(bw),
                                           std::uint32_t(bh), p[0], p[1]);
                else
                    be.hue_saturation(*src, *dst, std::uint32_t(bw),
                                      std::uint32_t(bh), p[0], p[1], p[2]);
                dst->download();
                const auto* adj =
                    static_cast<const pittore::RGBAf*>(dst->host());
                for (int y = by0; y < by1; ++y) {
                    for (int x = bx0; x < bx1; ++x) {
                        const float dx = float(x) + 0.5f - lcx;
                        const float dy = float(y) + 0.5f - lcy;
                        const float t =
                            std::sqrt(dx * dx + dy * dy) / lradius;
                        if (t >= 1.0f) continue;
                        float m = 1.0f;
                        if (t > hard) {
                            const float span = std::max(1.0f - hard, 1e-4f);
                            m = (1.0f - t) / span;
                        }
                        m *= st;
                        if (selPtr) {
                            m *= selPtr->coverage(x, y);
                            if (!(m > 0.0f)) continue;
                        }
                        pittore::RGBAf& o =
                            layer->pixels->data()[std::size_t(y) * lw + x];
                        const pittore::RGBAf& q =
                            adj[std::size_t(y - by0) * bw + (x - bx0)];
                        const pittore::RGBAf before = o;
                        o.r = q.r * m + o.r * (1.0f - m);
                        o.g = q.g * m + o.g * (1.0f - m);
                        o.b = q.b * m + o.b * (1.0f - m);
                        if (std::memcmp(&before, &o,
                                        sizeof(pittore::RGBAf)) != 0)
                            changed = true;
                    }
                }
                if (changed) {
                    bbox[0] = bx0;
                    bbox[1] = by0;
                    bbox[2] = bx1;
                    bbox[3] = by1;
                }
            }
        }
    }
    if (!changed) {
        changed = pittore::compute::adjustment_dab_host(
            layer->pixels->data(), lw, lh, lcx, lcy, lradius, hard, st,
            kind, p, bbox, selPtr);
    }
    if (!changed) return false;
    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    refreshLayerCmyk(*d, *layer,
                     QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                           bbox[3] - bbox[1]));
    layer->thumbnail = QImage();
    const double dx0 = layer->offset.x() + (bbox[0] - 1.0) * lsx;
    const double dy0 = layer->offset.y() + (bbox[1] - 1.0) * lsy;
    const double dx1 = layer->offset.x() + (bbox[2] + 1.0) * lsx;
    const double dy1 = layer->offset.y() + (bbox[3] + 1.0) * lsy;
    const QRect dabRect =
        QRectF(QPointF(dx0 - 1.0, dy0 - 1.0), QPointF(dx1 + 1.0, dy1 + 1.0))
            .toAlignedRect()
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    return true;
}

bool AppState::replaceColorDab(const QPointF& docPos, double radius,
                               double hardness, int mode,
                               const std::vector<pittore::RGBAf>& targets,
                               double tolerance, int limits, bool antialias,
                               double harmony) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) {
        setStatusHint(tr("Replace Color: select a pixel layer first."));
        return false;
    }
    if (targets.empty()) return false;

    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;

    // Same document → layer mapping as paintDab (see above).
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);

    const QColor fg = foreground();
    pittore::compute::NativeCmykPaint nativePaint;
    pittore::compute::NativeCmykPaint* nativePaintPtr = nullptr;
    if (d->colorMode.startsWith(QStringLiteral("CMYK"),
                                Qt::CaseInsensitive) &&
        prepareNativeCmykPaint(*d, *layer, fg, nativePaint))
        nativePaintPtr = &nativePaint;
    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::replace_color_dab_host(
        layer->pixels->data(), lw, lh, lcx, lcy, lradius,
        static_cast<float>(std::clamp(hardness, 0.0, 1.0)), targets.data(),
        static_cast<int>(targets.size()),
        pittore::RGBAf{fg.redF(), fg.greenF(), fg.blueF(), 1.0f},
        static_cast<float>(std::clamp(tolerance, 0.0, 1.0)),
        static_cast<pittore::compute::ReplaceMode>(std::clamp(mode, 0, 3)),
        std::clamp(limits, 0, 2), antialias,
        static_cast<float>(std::clamp(harmony, 0.0, 1.0)), bbox, selPtr,
        nativePaintPtr);
    if (!changed) return false;

    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    layer->thumbnail = QImage();

    // The touched layer rect mapped back to document space, plus a one-pixel
    // resampling halo for the incremental composite (mirrors toneDab).
    const double dx0 = layer->offset.x() + (bbox[0] - 1.0) * lsx;
    const double dy0 = layer->offset.y() + (bbox[1] - 1.0) * lsy;
    const double dx1 = layer->offset.x() + (bbox[2] + 1.0) * lsx;
    const double dy1 = layer->offset.y() + (bbox[3] + 1.0) * lsy;
    const QRect dabRect =
        QRectF(QPointF(dx0 - 1.0, dy0 - 1.0), QPointF(dx1 + 1.0, dy1 + 1.0))
            .toAlignedRect()
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    return true;
}

namespace {

// Shared sampler behind replaceTargetsAt / replacePaletteAt: average each
// document point over the Sample Size window (half-extents mirroring the
// eyedropper sizes the Magic Wand uses), then cluster greedily — a sample
// joins an existing target within half the tolerance — up to `cap` targets.
// Transparent samples are skipped. The first point has priority (callers pass
// the centre first).
std::vector<pittore::RGBAf> replaceClusterSamples(
    const LayerItem& layer, const std::vector<QPointF>& pts, int sampleSize,
    double tolerance, int cap) {
    std::vector<pittore::RGBAf> out;
    if (!layer.pixels || cap <= 0) return out;
    const double lsx = std::max(layer.scaleX, 1e-6);
    const double lsy = std::max(layer.scaleY, 1e-6);
    const int lw = static_cast<int>(layer.pixels->width());
    const int lh = static_cast<int>(layer.pixels->height());
    if (lw <= 0 || lh <= 0) return out;
    static const int kHalf[] = {0, 1, 2, 5, 15};
    const int half = kHalf[std::clamp(sampleSize, 0, 4)];

    const auto avgAt = [&](double docX, double docY)
        -> std::optional<pittore::RGBAf> {
        const int cx =
            static_cast<int>(std::floor((docX - layer.offset.x()) / lsx));
        const int cy =
            static_cast<int>(std::floor((docY - layer.offset.y()) / lsy));
        double ar = 0, ag = 0, ab = 0;
        int n = 0;
        for (int y = std::max(0, cy - half); y <= std::min(lh - 1, cy + half);
             ++y)
            for (int x = std::max(0, cx - half);
                 x <= std::min(lw - 1, cx + half); ++x) {
                const pittore::RGBAf& c = layer.pixels->at(
                    static_cast<std::uint32_t>(x),
                    static_cast<std::uint32_t>(y));
                if (c.a <= 0.01f) continue;
                ar += c.r;
                ag += c.g;
                ab += c.b;
                ++n;
            }
        if (n <= 0) return std::nullopt;
        return pittore::RGBAf{static_cast<float>(ar / n),
                               static_cast<float>(ag / n),
                               static_cast<float>(ab / n), 1.0f};
    };

    const float join =
        std::max(static_cast<float>(std::clamp(tolerance, 0.0, 1.0)) * 0.5f,
                 0.02f);
    for (const QPointF& p : pts) {
        auto s = avgAt(p.x(), p.y());
        if (!s) continue;
        bool known = false;
        for (const pittore::RGBAf& t : out) {
            const float dd =
                std::max(std::fabs(s->r - t.r),
                         std::max(std::fabs(s->g - t.g),
                                  std::fabs(s->b - t.b)));
            if (dd <= join) {
                known = true;
                break;
            }
        }
        if (!known) out.push_back(*s);
        if (static_cast<int>(out.size()) >= cap) break;
    }
    return out;
}

}  // namespace

pittore::RGBAf AppState::sampleActiveLayerAt(const QPointF& docPos,
                                              bool* ok) const {
    if (ok) *ok = false;
    const DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return pittore::RGBAf{0, 0, 0, 0};
    const LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return pittore::RGBAf{0, 0, 0, 0};
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int lw = static_cast<int>(layer->pixels->width());
    const int lh = static_cast<int>(layer->pixels->height());
    const int lx =
        static_cast<int>(std::floor((docPos.x() - layer->offset.x()) / lsx));
    const int ly =
        static_cast<int>(std::floor((docPos.y() - layer->offset.y()) / lsy));
    if (lx < 0 || ly < 0 || lx >= lw || ly >= lh)
        return pittore::RGBAf{0, 0, 0, 0};
    if (ok) *ok = true;
    return layer->pixels->at(static_cast<std::uint32_t>(lx),
                             static_cast<std::uint32_t>(ly));
}

std::vector<pittore::RGBAf> AppState::replaceTargetsAt(
    const QPointF& docPos, double radius, int sampleSize,
    double tolerance) const {
    const DocumentItem* d = activeDocument();
    const LayerItem* layer = activeLayer();
    if (!d || d->size.isEmpty() || !layer ||
        layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return {};
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));

    // Centre first (it has priority), then a ring at ~0.55×radius so a
    // gradient under the brush contributes its whole span.
    std::vector<QPointF> pts{{docPos.x(), docPos.y()}};
    if (lradius > 2.0f) {
        for (int k = 0; k < 8; ++k) {
            const double a = k * 3.14159265358979323846 / 4.0;
            pts.emplace_back(docPos.x() + std::cos(a) * lradius * 0.55 * lsx,
                             docPos.y() + std::sin(a) * lradius * 0.55 * lsy);
        }
    }
    return replaceClusterSamples(*layer, pts, sampleSize, tolerance, 4);
}

std::vector<pittore::RGBAf> AppState::replacePaletteAt(
    const QPointF& docPos, double radius, int sampleSize, double tolerance,
    int maxTargets) const {
    const DocumentItem* d = activeDocument();
    const LayerItem* layer = activeLayer();
    if (!d || d->size.isEmpty() || !layer ||
        layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return {};
    if (maxTargets <= 0) return {};
    const double r = radius > 0.0 ? radius : 8.0;
    // 9×9 grid over the brush disc: the whole area's colour range, not just
    // the centre's (the disc keeps ~60 of the 81 cells, clustered down to at
    // most maxTargets). Cells outside the disc are skipped.
    std::vector<QPointF> pts{docPos};
    for (int gy = -4; gy <= 4; ++gy)
        for (int gx = -4; gx <= 4; ++gx) {
            if (gx == 0 && gy == 0) continue;  // centre already first
            const double dx = gx * r / 4.0, dy = gy * r / 4.0;
            if (std::hypot(dx, dy) > r) continue;
            pts.emplace_back(docPos.x() + dx, docPos.y() + dy);
        }
    return replaceClusterSamples(*layer, pts, sampleSize, tolerance,
                                 maxTargets);
}

LayerItem* AppState::beginTonalEdit() {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return nullptr;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer) || tonalEditActive_) return nullptr;
    beginUndoStep();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector: re-resolve the pointer.
    layer = activeLayer();
    if (!layer) {
        discardUndoStep();
        return nullptr;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) {
        discardUndoStep();
        return nullptr;
    }
    tonalEditPre_ = std::make_shared<pittore::Image>(layer->pixels->clone());
    tonalEditCmykPre_ = layer->cmykPixels;
    tonalEditActive_ = true;
    return layer;
}

void AppState::applyTonalEditPreview() {
    if (!tonalEditActive_) return;
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->pixels) return;
    ++layer->sourceStamp;
    layer->thumbnail = QImage();
    d->rebuildComposite();
    emit documentModified(d);
}

void AppState::resetTonalEditPixels() {
    if (!tonalEditActive_) return;
    LayerItem* layer = activeLayer();
    if (layer && layer->pixels && tonalEditPre_ &&
        layer->pixels->width() == tonalEditPre_->width() &&
        layer->pixels->height() == tonalEditPre_->height())
        *layer->pixels = *tonalEditPre_;
}

void AppState::commitTonalEdit(const QString& name, const QString& iconKey) {
    if (!tonalEditActive_) return;
    if (DocumentItem* d = activeDocument()) {
        if (LayerItem* layer = activeLayer(); layer && layer->pixels)
            refreshLayerCmyk(*d, *layer);
    }
    tonalEditActive_ = false;
    tonalEditPre_.reset();
    tonalEditCmykPre_.reset();
    commitUndoStep(name, iconKey);
}

void AppState::cancelTonalEdit() {
    if (!tonalEditActive_) return;
    tonalEditActive_ = false;
    if (LayerItem* layer = activeLayer()) {
        if (layer->pixels && tonalEditPre_ &&
            layer->pixels->width() == tonalEditPre_->width() &&
            layer->pixels->height() == tonalEditPre_->height()) {
            *layer->pixels = *tonalEditPre_;
            ++layer->sourceStamp;
            layer->cmykPixels = tonalEditCmykPre_;
            layer->cmykSourceStamp = layer->cmykPixels
                                         ? layer->sourceStamp
                                         : 0;
            layer->thumbnail = QImage();
            DocumentItem* d = activeDocument();
            if (d) d->rebuildComposite();
        }
    }
    tonalEditPre_.reset();
    tonalEditCmykPre_.reset();
    discardUndoStep();
}

}  // namespace pittore::ui
