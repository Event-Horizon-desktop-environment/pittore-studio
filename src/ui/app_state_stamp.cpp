// Wash baking and the stamp-brush dabs: folding an armed wash stroke back into
// the layer at tool opacity, resolving a tool's density/tip/texture options
// into the engine's dab descriptors, and painting or erasing with a stamped
// tip. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/compute/brushes/clone/clone.h"
#include "ui/color_mode.h"

#include "engine/compute/brushes/mix/mix.h"
#include "engine/color/icc_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>

namespace pittore::ui {

// Folds an armed wash stroke into the active layer at the tool opacity.
bool AppState::bakeWashStroke() {
    if (!washArmed_ || washDirty_.isNull()) {
        washArmed_ = false;
        washDirty_ = QRect();
        return false;
    }    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    const ToolId tool = activeTool_;
    const QRect baked = washDirty_;
    washArmed_ = false;
    washDirty_ = QRect();
    if (d) d->washOverlayDirty = QRect();
    if (!d || !layer || layer->kind != LayerItem::Kind::Pixel) return false;
    if (!layer->pixels || washScratch_.width() != layer->pixels->width() ||
        washScratch_.height() != layer->pixels->height())
        return false;
    const QVariant o = option(tool, QStringLiteral("opacity"));
    const double op = o.isValid() ? std::clamp(o.toDouble() / 100.0, 0.0, 1.0)
                                  : 1.0;
    if (op <= 0.0) return true;  // invisible stroke: nothing to fold in
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    const int x0 = std::max(0, baked.left());
    const int y0 = std::max(0, baked.top());
    const int x1 = std::min<int>(w, baked.left() + baked.width());
    const int y1 = std::min<int>(h, baked.top() + baked.height());
    pittore::RGBAf* dst = layer->pixels->data();
    const pittore::RGBAf* src = washScratch_.data();
    const std::string mixId =
        option(tool, QStringLiteral("brush_composite"))
            .toString()
            .toStdString();
    const auto mixOp =
        pittore::compute::dabmix::mix_from_id(mixId.c_str());
    const bool useMix = mixOp != pittore::compute::dabmix::DabMixOp::Normal;
    pittore::compute::NativeCmykPaint nativePaint;
    const bool nativeWash = !useMix &&
        nativeCmykStrokeDoc_ == d && nativeCmykStrokeLayer_ == layer &&
        washScratchCmyk_.size() == layer->pixels->pixel_count() &&
        prepareNativeCmykPaint(*d, *layer, foreground_, nativePaint);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = std::size_t(y) * w + x;
            const float sa =
                std::clamp(src[i].a * float(op), 0.0f, 1.0f);
            if (sa <= 0.0f) continue;
            RGBAf& dd = dst[i];
            if (useMix) {
                pittore::compute::dabmix::mix_pixel(
                    mixOp, src[i].r, src[i].g, src[i].b, sa, dd.r, dd.g,
                    dd.b, dd.a, dd.r, dd.g, dd.b, dd.a);
                continue;
            }
            const float inv = 1.0f - sa;
            const float out_a = sa + dd.a * inv;
            if (out_a > 0.0f) {
                if (nativeWash) {
                    float ink[4];
                    for (int c = 0; c < 4; ++c) {
                        auto& dstInk = nativePaint.pixels[i][c];
                        const float mixed =
                            (sa * (washScratchCmyk_[i][c] / 65535.0f) +
                             dd.a * (dstInk / 65535.0f) * inv) / out_a;
                        dstInk = static_cast<std::uint16_t>(std::clamp(
                            std::lround(mixed * 65535.0f), 0L, 65535L));
                        ink[c] = dstInk / 65535.0f;
                    }
                    float rgb[3];
                    nativePaint.preview->convert(ink[0], ink[1], ink[2],
                                                 ink[3], rgb);
                    dd.r = rgb[0]; dd.g = rgb[1]; dd.b = rgb[2];
                } else {
                    dd.r = (src[i].r * sa + dd.r * dd.a * inv) / out_a;
                    dd.g = (src[i].g * sa + dd.g * dd.a * inv) / out_a;
                    dd.b = (src[i].b * sa + dd.b * dd.a * inv) / out_a;
                }
                dd.a = out_a;
            }
        }
    }
    ++layer->sourceStamp;
    layer->thumbnail = QImage();
    // The fold changed layer pixels behind the device's back: re-upload
    // through the usual dab path (keeps the GPU cache coherent and drops
    // the stale panel thumbnail state) and re-render + repaint the baked
    // region, or the stroke stays invisible until the next full rebuild
    // (e.g. a zoom). Mirrors the flushPaint tail.
    const QRect bakedDoc =
        baked.intersected(QRect(QPoint(0, 0), d->size));
    if (!bakedDoc.isNull()) {
        const float cx =
            float(bakedDoc.center().x());
        const float cy =
            float(bakedDoc.center().y());
        const float rr = float(std::max(bakedDoc.width(),
                                        bakedDoc.height())) *
                         0.5f;
        d->touchPixelsAfterDab(*layer, cx, cy, rr, w, h, nativeWash);
        d->renderRegion(bakedDoc);
        noteRegionEdit(QRectF(bakedDoc));
        emit regionModified(d, QRectF(bakedDoc));
    }
    return true;
}

pittore::compute::DabDensity AppState::resolveDensity(ToolId tool) {    const QVariant v = option(tool, QStringLiteral("brush_density"));
    const double pct = v.isValid() ? std::clamp(v.toDouble(), 0.0, 100.0) : 100.0;
    pittore::compute::DabDensity den;
    den.density = float(pct / 100.0);
    den.seed = static_cast<std::uint32_t>(
        (strokeSeed_ & 0xFFFFFFFFull) ^ (std::uint64_t(dabCount_) * 2654435761ull));
    ++dabCount_;
    return den;
}

double AppState::strokeRandom() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(strokeRng_);
}

pittore::compute::MaskTip AppState::resolveMaskTip(ToolId tool,
                                                     double pressure01) const {
    pittore::compute::MaskTip m;
    const QString id =
        option(tool, QStringLiteral("brush_mask_stamp")).toString();
    if (id.isEmpty()) return m;
    const pittore::compute::StampTip* tip = brushStamp(id);
    if (!tip || !tip->valid()) return m;
    auto dbl = [&](const char* key, double fallback) {
        const QVariant v = option(tool, QString::fromUtf8(key));
        return v.isValid() ? v.toDouble() : fallback;
    };
    m.bitmap.alpha = tip->alpha.data();
    m.bitmap.w = tip->w;
    m.bitmap.h = tip->h;
    m.sizeRatio = float(
        std::clamp(dbl("brush_mask_ratio", 100.0) / 100.0, 0.05, 4.0));
    // Pressure shrinks the mask with the dab when the tool opts in.
    if (option(tool, QStringLiteral("brush_mask_pressure")).toBool())
        m.sizeRatio *= float(0.25 + 0.75 * std::clamp(pressure01, 0.0, 1.0));
    m.angleDeg =
        float(std::clamp(dbl("brush_mask_angle", 0.0), -180.0, 180.0));
    m.mode = std::clamp(int(dbl("brush_mask_mode", 0.0)), 0, 3);
    return m;
}

// Paper grain for the active texture options, mapped into the dab's target
// space. Returns an invalid (disabled) texture when no pattern applies.
pittore::compute::PatternTex AppState::resolveTexture(
    ToolId tool, double lsx, double lsy, const QPointF& targetOffset,
    double pressure01) const {
    pittore::compute::PatternTex tex;
    const QString id =
        option(tool, QStringLiteral("brush_texture")).toString();
    if (id.isEmpty()) return tex;
    const PatternGray* pat = brushPattern(id);
    if (!pat) return tex;
    auto dbl = [&](const char* key, double fallback) {
        const QVariant v = option(tool, QString::fromUtf8(key));
        return v.isValid() ? v.toDouble() : fallback;
    };
    tex.gray = pat->gray.data();
    tex.w = pat->w;
    tex.h = pat->h;
    tex.strength = float(std::clamp(dbl("brush_texture_strength", 80.0) / 100.0,
                                    0.0, 1.0));
    // Pressure thins the grain with the dab when the tool opts in.
    if (option(tool, QStringLiteral("brush_texture_pressure")).toBool())
        tex.strength *=
            float(0.25 + 0.75 * std::clamp(pressure01, 0.0, 1.0));
    const double sc = dbl("brush_texture_scale", 100.0) / 100.0;
    tex.scale = float(sc > 1e-6 ? sc : 1.0);
    const double sx = std::max(lsx, 1e-6), sy = std::max(lsy, 1e-6);
    tex.offsetX =
        float((strokePatternOrigin_.x() - targetOffset.x()) / sx);
    tex.offsetY =
        float((strokePatternOrigin_.y() - targetOffset.y()) / sy);
    tex.neutral = float(std::clamp(dbl("brush_texture_neutral", 50.0) / 100.0,
                                   0.0, 1.0));
    tex.brightness = float(std::clamp(dbl("brush_texture_brightness", 0.0) / 100.0,
                                      -1.0, 1.0));
    tex.contrast = float(std::clamp(dbl("brush_texture_contrast", 100.0) / 100.0,
                                    0.0, 4.0));
    const QVariant iv = option(tool, QStringLiteral("brush_texture_invert"));
    tex.invert = iv.isValid() && iv.toBool();
    // Auto-invert for eraser: flip the grain when erasing so the same
    // preset cuts instead of deposits (opt-in per preset).
    if (option(tool, QStringLiteral("brush_texture_auto_invert_eraser"))
            .toBool()) {
        const bool erasing =
            tool == ToolId::Eraser ||
            option(tool, QStringLiteral("brush_erase_blend")).toBool();
        if (erasing) tex.invert = !tex.invert;
    }
    tex.mode = std::clamp(
        option(tool, QStringLiteral("brush_texture_mode")).toInt(0), 0, 6);
    tex.soft =
        option(tool, QStringLiteral("brush_texture_soft")).toBool();
    tex.cutoffPolicy =
        std::clamp(option(tool, QStringLiteral("brush_texture_cutoff_policy"))
                       .toInt(0),
                   0, 2);
    tex.cutLo = float(
        std::clamp(dbl("brush_texture_cutlo", 0.0), 0.0, 1.0));
    tex.cutHi =
        float(std::clamp(dbl("brush_texture_cuthi", 1.0), 0.0, 1.0));
    if (tex.cutHi < tex.cutLo) std::swap(tex.cutLo, tex.cutHi);
    return tex;
}

// Paper-grain evaluator for bundle-imported textures: same pattern store
// as resolveTexture, but the grain pipeline (subtractive brightness,
// neutral remap, cutoff window) instead of the legacy combine. Invalid
// unless the preset opts into grain evaluation.
pittore::compute::papergrain::PaperGrain AppState::resolvePaperGrain(
    ToolId tool, double lsx, double lsy, const QPointF& targetOffset,
    double pressure01) const {
    pittore::compute::papergrain::PaperGrain grain;
    if (!option(tool, QStringLiteral("brush_texture_grain")).toBool())
        return grain;
    const QString id =
        option(tool, QStringLiteral("brush_texture")).toString();
    if (id.isEmpty()) return grain;
    const PatternGray* pat = brushPattern(id);
    if (!pat) return grain;
    auto dbl = [&](const char* key, double fallback) {
        const QVariant v = option(tool, QString::fromUtf8(key));
        return v.isValid() ? v.toDouble() : fallback;
    };
    grain.gray = pat->gray.data();
    grain.w = pat->w;
    grain.h = pat->h;
    grain.strength = float(std::clamp(dbl("brush_texture_strength", 80.0) / 100.0,
                                      0.0, 1.0));
    if (option(tool, QStringLiteral("brush_texture_pressure")).toBool())
        grain.strength *=
            float(0.25 + 0.75 * std::clamp(pressure01, 0.0, 1.0));
    const double sc = dbl("brush_texture_scale", 100.0) / 100.0;
    grain.scale = float(sc > 1e-6 ? sc : 1.0);
    const double sx = std::max(lsx, 1e-6), sy = std::max(lsy, 1e-6);
    grain.offsetX =
        float((strokePatternOrigin_.x() - targetOffset.x()) / sx);
    grain.offsetY =
        float((strokePatternOrigin_.y() - targetOffset.y()) / sy);
    grain.neutral = float(std::clamp(dbl("brush_texture_neutral", 50.0) / 100.0,
                                     0.0, 1.0));
    grain.brightness = float(std::clamp(dbl("brush_texture_brightness", 0.0) / 100.0,
                                        -1.0, 1.0));
    grain.contrast = float(std::clamp(dbl("brush_texture_contrast", 100.0) / 100.0,
                                      0.0, 4.0));
    const QVariant iv = option(tool, QStringLiteral("brush_texture_invert"));
    grain.invert = iv.isValid() && iv.toBool();
    if (option(tool, QStringLiteral("brush_texture_auto_invert_eraser"))
            .toBool()) {
        const bool erasing =
            tool == ToolId::Eraser ||
            option(tool, QStringLiteral("brush_erase_blend")).toBool();
        if (erasing) grain.invert = !grain.invert;
    }
    grain.mode = static_cast<pittore::compute::papergrain::GrainMode>(
        std::clamp(option(tool, QStringLiteral("brush_texture_mode")).toInt(0),
                   0, 2));
    grain.cutoffPolicy =
        std::clamp(option(tool, QStringLiteral("brush_texture_cutoff_policy"))
                       .toInt(0),
                   0, 2);
    grain.cutLo = float(
        std::clamp(dbl("brush_texture_cutlo", 0.0), 0.0, 1.0));
    grain.cutHi =
        float(std::clamp(dbl("brush_texture_cuthi", 1.0), 0.0, 1.0));
    if (grain.cutHi < grain.cutLo) std::swap(grain.cutLo, grain.cutHi);
    return grain;
}

bool AppState::stampDab(const QPointF& docPos, double radius, int mode,
                        double opacity, const QColor& color, double angleDeg,
                        const QString& stampId) {
    const pittore::compute::StampTip* tip = brushStamp(stampId);
    if (!tip) return false;
    return stampTipDab(docPos, radius, mode, opacity, color, angleDeg, *tip);
}

bool AppState::stampTipDab(const QPointF& docPos, double radius, int mode,
                           double opacity, const QColor& color,
                           double angleDeg,
                           const pittore::compute::StampTip& tip,
                           int flip, int filter) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->visible || layer->lockTransparency) return false;
    if (layer->kind != LayerItem::Kind::Pixel &&
        layer->kind != LayerItem::Kind::Adjustment)
        return false;
    const bool toMask = layer->maskSelected;
    if (toMask || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Stamp brushes paint pixels only (no mask target)."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    // Same document → target mapping as paintDab.
    QPointF targetOffset = layer->offset;
    double targetScaleX = layer->scaleX, targetScaleY = layer->scaleY;
    const pittore::Image* targetImage = layer->pixels.get();
    const double lsx = std::max(targetScaleX, 1e-6);
    const double lsy = std::max(targetScaleY, 1e-6);
    const std::uint32_t lw = targetImage->width();
    const std::uint32_t lh = targetImage->height();
    const float lcx = static_cast<float>((docPos.x() - targetOffset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - targetOffset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Wash mode paints into the target-space scratch buffer (identity
    // layers only; transformed layers fall back to direct buildup), shared
    // with the auto-tip path so the live preview and release bake treat
    // stamp strokes identically.
    const bool identityTarget =
        std::fabs(targetOffset.x()) < 1e-6 &&
        std::fabs(targetOffset.y()) < 1e-6 &&
        std::fabs(lsx - 1.0) < 1e-6 && std::fabs(lsy - 1.0) < 1e-6;
    pittore::RGBAf* dabDst = layer->pixels->data();
    bool toScratch = false;
    if (washArmed_ && identityTarget) {
        if (washScratch_.width() != lw || washScratch_.height() != lh) {
            washScratch_ = pittore::Image(lw, lh);
            washScratch_.fill(pittore::RGBAf{0, 0, 0, 0});
        }
        if (washScratchCmyk_.size() != static_cast<std::size_t>(lw) * lh)
            washScratchCmyk_.assign(static_cast<std::size_t>(lw) * lh, {});
        dabDst = washScratch_.data();
        toScratch = true;
    }
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr = strokeSelectionMask(*d, *layer, targetImage,
                                             targetOffset, lsx, lsy);
    const auto smode = mode == 1   ? pittore::compute::StampMode::ColorImage
                       : mode == 2 ? pittore::compute::StampMode::LightnessMap
                       : mode == 3 ? pittore::compute::StampMode::GradientMap
                                   : pittore::compute::StampMode::AlphaMask;
    auto optDbl = [&](const char* key, double fallback) {
        const QVariant v = option(activeTool_, QString::fromUtf8(key));
        return v.isValid() ? v.toDouble() : fallback;
    };
    pittore::compute::StampLevels levels;
    levels.neutral = float(
        std::clamp(optDbl("brush_tip_neutral", 50.0) / 100.0, 0.0, 1.0));
    levels.brightness = float(
        std::clamp(optDbl("brush_tip_brightness", 0.0) / 100.0, -1.0, 1.0));
    levels.contrast = float(
        std::clamp(optDbl("brush_tip_contrast", 100.0) / 100.0, 0.0, 4.0));
    const std::string mixId =
        option(activeTool_, QStringLiteral("brush_composite"))
            .toString()
            .toStdString();
    const auto mixOp =
        pittore::compute::dabmix::mix_from_id(mixId.c_str());
    const QColor bgQ = background();
    const pittore::RGBAf bgC{float(bgQ.redF()), float(bgQ.greenF()),
                              float(bgQ.blueF()), 1.0f};
    const pittore::compute::PatternTex tex =
        resolveTexture(activeTool_, lsx, lsy, targetOffset, dabPressure01_);
    const pittore::compute::papergrain::PaperGrain paintGrain =
        resolvePaperGrain(activeTool_, lsx, lsy, targetOffset,
                          dabPressure01_);
    const pittore::compute::papergrain::PaperGrain* grainPtr =
        paintGrain.valid() ? &paintGrain : nullptr;
    pittore::compute::StampSharp sharp;
    sharp.strength = float(std::clamp(optDbl("brush_sharpness", 0.0) / 100.0,
                                      0.0, 1.0));
    sharp.softness = float(std::clamp(optDbl("brush_soften", 0.0) / 100.0,
                                      0.0, 1.0));
    const pittore::compute::DabDensity dabDenStamp =
        resolveDensity(activeTool_);
    const pittore::compute::MaskTip maskStamp =
        resolveMaskTip(activeTool_, dabPressure01_);
    pittore::compute::NativeCmykPaint nativePaint;
    pittore::compute::NativeCmykPaint* nativePaintPtr = nullptr;
    if (mixOp == pittore::compute::dabmix::DabMixOp::Normal &&
        (activeTool_ == ToolId::Brush || activeTool_ == ToolId::Pencil) &&
        prepareNativeCmykPaint(*d, *layer, color, nativePaint)) {
        nativePaint.sourceFromRgb = true;
        if (toScratch && washScratchCmyk_.size() ==
                             static_cast<std::size_t>(lw) * lh)
            nativePaint.pixels = washScratchCmyk_.data();
        nativePaintPtr = &nativePaint;
    }
    // Relief deposition (lightness mode with thickness): ensure a plane
    // matching the target and scale the tip lightness by the thickness.
    // Wash strokes skip relief: there is no scratch height plane, and the
    // bake only folds color.
    float* heightPtr = nullptr;
    float heightAmt = 0.0f;
    if (mode == 2 && !toScratch) {
        const QVariant tv =
            option(activeTool_, QStringLiteral("brush_tip_thickness"));
        heightAmt = float(std::clamp(tv.isValid() ? tv.toDouble() / 100.0 : 0.0,
                                     0.0, 1.0));
        if (heightAmt > 0.0f) {
            ensureLayerHeight(*layer);
            heightPtr = heightPlaneFor(*layer, lw, lh);
        }
    }
    pittore::compute::stamp_dab_host(
        dabDst, lw, lh, lcx, lcy, lradius, tip, smode,
        static_cast<float>(std::clamp(opacity, 0.0, 1.0)),
        pittore::RGBAf{color.redF(), color.greenF(), color.blueF(), 1.0f},
        static_cast<float>(angleDeg), selPtr, &tex, &dabDenStamp, &maskStamp,
        flip, filter, &levels, &bgC, heightPtr, heightAmt, mixOp, grainPtr,
        &sharp, nativePaintPtr);
    if (!toScratch) {
        d->touchPixelsAfterDab(*layer, lcx, lcy, lradius, lw, lh,
                               nativePaintPtr != nullptr);
    }
    const double ex = (static_cast<double>(lradius) + 1.0) * lsx + 1.0;
    const double ey = (static_cast<double>(lradius) + 1.0) * lsy + 1.0;
    const QRect dabRect =
        QRect(int(std::floor(docPos.x() - ex)), int(std::floor(docPos.y() - ey)),
              int(std::ceil(2 * ex)) + 1, int(std::ceil(2 * ey)) + 1)
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    if (toScratch)
        washDirty_ = washDirty_.isNull() ? dabRect : washDirty_.united(dabRect);
    return true;
}

bool AppState::stampEraseDab(const QPointF& docPos, double radius,
                             double opacity, double angleDeg,
                             const QString& stampId, int flip, int filter) {
    const pittore::compute::StampTip* tip = brushStamp(stampId);
    if (!tip) return false;
    return stampTipEraseDab(docPos, radius, opacity, angleDeg, *tip, flip,
                            filter);
}

bool AppState::stampTipEraseDab(const QPointF& docPos, double radius,
                                double opacity, double angleDeg,
                                const pittore::compute::StampTip& tip,
                                int flip, int filter) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->visible || layer->lockTransparency) return false;
    if (layer->kind != LayerItem::Kind::Pixel) return false;
    if (layer->maskSelected) {
        setStatusHint(tr("Stamp brushes paint pixels only (no mask target)."));
        return false;
    }
    // Background parity (see eraseDab): paint the canvas paper through
    // the same tip as an alpha mask so the eraser shape matches exactly;
    // a transparent bottom falls through to alpha erase below.
    if (layer == &d->layers.last() && !d->canvasTransparent) {
        return stampTipDab(docPos, radius, 0, opacity, d->canvasPaper,
                           angleDeg, tip, flip, filter);
    }
    ensureLayerPixels(*d, *layer);
    QPointF targetOffset = layer->offset;
    double targetScaleX = layer->scaleX, targetScaleY = layer->scaleY;
    const pittore::Image* targetImage = layer->pixels.get();
    const double lsx = std::max(targetScaleX, 1e-6);
    const double lsy = std::max(targetScaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - targetOffset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - targetOffset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr = strokeSelectionMask(*d, *layer, targetImage,
                                             targetOffset, lsx, lsy);
    const pittore::compute::PatternTex stampEraseTex =
        resolveTexture(activeTool_, lsx, lsy, targetOffset, dabPressure01_);
    const pittore::compute::papergrain::PaperGrain eraseGrain =
        resolvePaperGrain(activeTool_, lsx, lsy, targetOffset,
                          dabPressure01_);
    const pittore::compute::papergrain::PaperGrain* eraseGrainPtr =
        eraseGrain.valid() ? &eraseGrain : nullptr;
    const pittore::compute::DabDensity dabDenStampErase =
        resolveDensity(activeTool_);
    const pittore::compute::MaskTip maskStampErase =
        resolveMaskTip(activeTool_, dabPressure01_);
    // Erased paint takes its relief with it (no plane created just to
    // carve nothing; stale-sized planes are skipped, never written).
    float* eraseHeight =
        heightPlaneFor(*layer, targetImage->width(), targetImage->height());
    pittore::compute::stamp_erase_dab_host(
        layer->pixels->data(), targetImage->width(), targetImage->height(),
        lcx, lcy, lradius, tip,
        static_cast<float>(std::clamp(opacity, 0.0, 1.0)),
        static_cast<float>(angleDeg), selPtr, &stampEraseTex, &dabDenStampErase,
        &maskStampErase, flip, filter, eraseHeight, eraseGrainPtr);
    d->touchPixelsAfterDab(*layer, lcx, lcy, lradius, targetImage->width(),
                           targetImage->height());
    const double ex = (static_cast<double>(lradius) + 1.0) * lsx + 1.0;
    const double ey = (static_cast<double>(lradius) + 1.0) * lsy + 1.0;
    const QRect dabRect =
        QRect(int(std::floor(docPos.x() - ex)), int(std::floor(docPos.y() - ey)),
              int(std::ceil(2 * ex)) + 1, int(std::ceil(2 * ey)) + 1)
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    return true;
}


bool AppState::cloneStampDab(const QPointF& docPos, double radius,
                             double hardness, double opacity, double flow,
                             const QPointF& offsetDoc, int sampleMode) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) {
        setStatusHint(tr("Clone Stamp: select a pixel layer first."));
        return false;
    }

    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);

    // Capture the stroke snapshots once, when the stroke begins: the layer's
    // pre-stroke pixels (re-render base) and the source (the layer itself,
    // or the composite for the composite sample modes — caught now so the
    // stroke paints from a stable picture, never its own fresh output).
    if (!cloneStrokeActive_ || cloneStrokeW_ != lw || cloneStrokeH_ != lh) {
        cloneStrokePre_.assign(layer->pixels->data(),
                               layer->pixels->data() + std::size_t(lw) * lh);
        cloneStrokeCoverage_.assign(std::size_t(lw) * lh, 0.0f);
        cloneStrokeW_ = lw;
        cloneStrokeH_ = lh;
        if (std::clamp(sampleMode, 0, 2) == 0 || d->composite.isNull()) {
            cloneStrokeSrc_ = cloneStrokePre_;
        } else {
            // "Current & Below" and "All Layers" both read the flattened
            // composite (per-layer compositing below is not separated yet),
            // resampled to layer space like the Magic Wand measures.
            QImage comp = d->composite;
            if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
                comp.format() != QImage::Format_ARGB32)
                comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            cloneStrokeSrc_.resize(std::size_t(lw) * lh);
            for (std::uint32_t y = 0; y < lh; ++y) {
                const double docY =
                    layer->offset.y() + (double(y) + 0.5) * lsy;
                const int py = std::clamp(static_cast<int>(std::floor(docY)),
                                          0, comp.height() - 1);
                const QRgb* row =
                    reinterpret_cast<const QRgb*>(comp.constScanLine(py));
                for (std::uint32_t x = 0; x < lw; ++x) {
                    const double docX =
                        layer->offset.x() + (double(x) + 0.5) * lsx;
                    const int px = std::clamp(static_cast<int>(std::floor(docX)),
                                              0, comp.width() - 1);
                    const QRgb c = row[px];
                    const int a = qAlpha(c);
                    pittore::RGBAf& dstC =
                        cloneStrokeSrc_[std::size_t(y) * lw + x];
                    if (a <= 0) {
                        dstC = pittore::RGBAf{0, 0, 0, 0};
                    } else {
                        const float inv = 1.0f / static_cast<float>(a);
                        dstC = pittore::RGBAf{qRed(c) * inv, qGreen(c) * inv,
                                               qBlue(c) * inv,
                                               static_cast<float>(a) / 255.0f};
                    }
                }
            }
        }
        cloneStrokeActive_ = true;
    }

    // Same document → layer mapping as paintDab (see above); the
    // document-space stroke offset rides along per-axis (scaled layers keep
    // source and destination registered).
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    const float lox = static_cast<float>(offsetDoc.x() / lsx);
    const float loy = static_cast<float>(offsetDoc.y() / lsy);
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);

    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::clone_stamp_dab_host(
        cloneStrokePre_.data(), layer->pixels->data(),
        cloneStrokeCoverage_.data(), lw, lh, lcx, lcy, lradius,
        static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
        cloneStrokeSrc_.data(), lw, lh, lox, loy,
        static_cast<float>(std::clamp(opacity, 0.0, 1.0)),
        static_cast<float>(std::clamp(flow, 0.0, 1.0)), bbox, selPtr);
    if (!changed) return false;

    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    refreshLayerCmyk(*d, *layer,
                     QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
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

void AppState::endCloneStroke() {
    if (!cloneStrokeActive_) return;
    cloneStrokeActive_ = false;
    cloneStrokePre_.clear();
    cloneStrokePre_.shrink_to_fit();
    cloneStrokeSrc_.clear();
    cloneStrokeSrc_.shrink_to_fit();
    cloneStrokeCoverage_.clear();
    cloneStrokeCoverage_.shrink_to_fit();
    cloneStrokeW_ = 0;
    cloneStrokeH_ = 0;
}

}  // namespace pittore::ui
