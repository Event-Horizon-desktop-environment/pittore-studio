// Stroke execution and the brush registries: where a pointer event turns into
// dabs, the per-stroke state that frames it (seed, curves, wash scratch) and
// the cached per-stroke selection mask. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"

#include "engine/color/icc_convert.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/paint.h"
#include "engine/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

namespace pittore::ui {

namespace {
struct StrokeCmykTransforms {
    QByteArray profile;
    std::unique_ptr<pittore::color::SrgbToCmyk> separation;
    std::unique_ptr<pittore::color::CmykToSrgb> preview;
};

StrokeCmykTransforms& strokeCmykTransforms(const QByteArray& profile) {
    static thread_local StrokeCmykTransforms cache;
    if (cache.profile != profile || !cache.separation || !cache.preview) {
        cache.profile = profile;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(profile.constData());
        const auto size = static_cast<std::size_t>(profile.size());
        cache.separation = std::make_unique<pittore::color::SrgbToCmyk>(bytes, size);
        cache.preview = std::make_unique<pittore::color::CmykToSrgb>(bytes, size);
    }
    return cache;
}
}  // namespace

bool AppState::prepareNativeCmykPaint(
    DocumentItem& doc, LayerItem& layer, const QColor& color,
    pittore::compute::NativeCmykPaint& paint, bool trackStroke) {
    if (!layer.pixels ||
        !doc.colorMode.startsWith(QStringLiteral("CMYK"), Qt::CaseInsensitive))
        return false;
    const QByteArray profile = resolveCmykProfile(doc);
    const std::uint64_t profileStamp = pittore::color::CmykImage::fingerprint(
        reinterpret_cast<const std::uint8_t*>(profile.constData()),
        static_cast<std::size_t>(profile.size()));
    if (!trackStroke || nativeCmykStrokeDoc_ != &doc ||
        nativeCmykStrokeLayer_ != &layer) {
        const bool current = layer.cmykPixels && layer.cmykPixels->valid() &&
            layer.cmykPixels->width == layer.pixels->width() &&
            layer.cmykPixels->height == layer.pixels->height() &&
            layer.cmykPixels->profileFingerprint == profileStamp &&
            layer.cmykPixels->appearanceFingerprint ==
                pittore::color::CmykImage::fingerprint(*layer.pixels);
        if (!current) refreshLayerCmyk(doc, layer);
        if (!layer.cmykPixels || !layer.cmykPixels->valid()) return false;
        if (layer.cmykPixels.use_count() > 1)
            layer.cmykPixels = std::make_shared<pittore::color::CmykImage>(
                *layer.cmykPixels);
        if (trackStroke) {
            nativeCmykStrokeDoc_ = &doc;
            nativeCmykStrokeLayer_ = &layer;
        }
    }
    auto& transforms = strokeCmykTransforms(profile);
    float ink[4];
    transforms.separation->convert(color.redF(), color.greenF(), color.blueF(),
                                   ink);
    for (int c = 0; c < 4; ++c)
        paint.source[c] = static_cast<std::uint16_t>(std::clamp(
            std::lround(ink[c] * 65535.0f), 0L, 65535L));
    paint.pixels = layer.cmykPixels->pixels.data();
    paint.preview = transforms.preview.get();
    paint.separation = transforms.separation.get();
    return true;
}

bool AppState::paintDab(const QPointF& docPos, double radius, double hardness,
                        double opacity, const QColor& color, double ratio,
                        double angleDeg, bool squareTip) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->visible || layer->lockTransparency) return false;
    if (layer->kind != LayerItem::Kind::Pixel &&
        layer->kind != LayerItem::Kind::Adjustment)
        return false;

    // A selected mask retargets the stroke from pixels to coverage. Brush
    // colour becomes grey through luma; the eraser twin below always reveals.
    const bool toMask = layer->maskSelected;
    const pittore::Image* constMask =
        toMask ? effectiveMaskImage(d->size, *layer) : nullptr;
    if (toMask && !constMask) {
        setStatusHint(tr("The selected mask is missing or disabled."));
        return false;
    }
    // Adjustment layers have no pixels: only their selected mask is paintable.
    if (!toMask && layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Select the layer's mask to paint here."));
        return false;
    }
    if (!toMask) ensureLayerPixels(*d, *layer);

    // The dab kernel runs in target space so brush work on a moved or scaled
    // photo lands at full source resolution; out-of-bounds dabs are silently
    // clipped at the target edge by the kernel itself. The radius uses the
    // minor axis so the dab covers the whole document-space circle.
    QPointF targetOffset = layer->offset;
    double targetScaleX = layer->scaleX, targetScaleY = layer->scaleY;
    const pittore::Image* targetImage = layer->pixels.get();
    if (toMask) {
        targetImage = constMask;
        maskTransformFor(*layer, targetOffset, targetScaleX, targetScaleY);
    }
    const double lsx = std::max(targetScaleX, 1e-6);
    const double lsy = std::max(targetScaleY, 1e-6);
    const std::uint32_t lw = targetImage->width();
    const std::uint32_t lh = targetImage->height();
    const float lcx = static_cast<float>((docPos.x() - targetOffset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - targetOffset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr = strokeSelectionMask(*d, *layer, targetImage,
                                             targetOffset, lsx, lsy);
    bool toScratch = false;  // wash scratch (set in the pixel branch below)
    if (toMask) {
        pittore::compute::mask_dab_host(
            layer->mask->data(), lw, lh, lcx, lcy, lradius,
            static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
            static_cast<float>(std::clamp(opacity, 0.0, 1.0)),
            maskBrushValue(color), selPtr);
        // Incremental device refresh (region upload, no stamp churn).
        const int mx0 = std::max(0, static_cast<int>(std::floor(lcx - lradius - 1.0f)));
        const int my0 = std::max(0, static_cast<int>(std::floor(lcy - lradius - 1.0f)));
        const int mx1 = std::min(static_cast<int>(lw), static_cast<int>(std::ceil(lcx + lradius + 1.0f)));
        const int my1 = std::min(static_cast<int>(lh), static_cast<int>(std::ceil(lcy + lradius + 1.0f)));
        d->refreshMaskRegion(*layer, QRect(mx0, my0, mx1 - mx0, my1 - my0));
    } else {
        // Wash mode paints into a target-space scratch buffer (identity
        // layers only; transformed layers fall back to direct buildup) and
        // composites at the stroke opacity; the bake folds it in on release.
        const bool identityTarget =
            std::fabs(targetOffset.x()) < 1e-6 &&
            std::fabs(targetOffset.y()) < 1e-6 &&
            std::fabs(lsx - 1.0) < 1e-6 && std::fabs(lsy - 1.0) < 1e-6;
        pittore::RGBAf* dabDst = layer->pixels->data();
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
        // Extra shape options (all neutral by default, so legacy presets
        // keep the fast round path bit-exactly).
        const int spikesOpt = qBound(
            0, option(activeTool_, QStringLiteral("brush_spikes")).toInt(), 12);
        const double anisoOpt = qBound(
            -100.0,
            option(activeTool_, QStringLiteral("brush_fade_aniso")).toDouble(),
            100.0);
        const int falloffOpt = qBound(
            0, option(activeTool_, QStringLiteral("brush_falloff")).toInt(), 1);
        const double sharpOpt = qBound(
            0.0,
            option(activeTool_, QStringLiteral("brush_sharpness")).toDouble(),
            100.0);
        const bool roundTip = !squareTip && ratio >= 0.999 &&
                              std::fabs(angleDeg) < 1e-6 && spikesOpt < 2 &&
                              anisoOpt == 0.0 && falloffOpt == 0 &&
                              sharpOpt <= 0.0;
        // One resolve per dab: texture, density seed and mask are identical
        // for both tip branches (density advances the stroke seed, so a
        // second resolve would also skip it).
        const pittore::compute::PatternTex paintTex =
            resolveTexture(activeTool_, lsx, lsy, targetOffset, dabPressure01_);
        const pittore::compute::papergrain::PaperGrain paintGrain =
            resolvePaperGrain(activeTool_, lsx, lsy, targetOffset,
                              dabPressure01_);
        const pittore::compute::papergrain::PaperGrain* grainPtr =
            paintGrain.valid() ? &paintGrain : nullptr;
        const std::string mixId =
            option(activeTool_, QStringLiteral("brush_composite"))
                .toString()
                .toStdString();
        const auto mixOp =
            pittore::compute::dabmix::mix_from_id(mixId.c_str());
        // Tool blend mode ("Mode" combo). Wash strokes accumulate coverage
        // into the scratch buffer at Normal and apply the mode once at
        // bake/preview time; direct buildup dabs blend per dab.
        const pittore::compute::BlendMode dabBlend =
            toScratch ? pittore::compute::BlendMode::Normal
                      : paintBlendFor(activeTool_);
        pittore::compute::NativeCmykPaint nativePaint;
        pittore::compute::NativeCmykPaint* nativePaintPtr = nullptr;
        const bool useNativeCmyk =
            !toMask && mixOp == pittore::compute::dabmix::DabMixOp::Normal &&
            d->colorMode.startsWith(QStringLiteral("CMYK"), Qt::CaseInsensitive) &&
            (activeTool_ == ToolId::Brush || activeTool_ == ToolId::Pencil);
        if (useNativeCmyk && prepareNativeCmykPaint(*d, *layer, color, nativePaint)) {
            if (toScratch && washScratchCmyk_.size() ==
                                 static_cast<std::size_t>(lw) * lh)
                nativePaint.pixels = washScratchCmyk_.data();
            nativePaintPtr = &nativePaint;
        }
        const pittore::compute::DabDensity dabDenPaint =
            resolveDensity(activeTool_);
        const pittore::compute::MaskTip maskPaint =
            resolveMaskTip(activeTool_, dabPressure01_);
        if (roundTip) {
            pittore::compute::paint_dab_host(
                dabDst, lw, lh, lcx, lcy, lradius,
                static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
                static_cast<float>(std::clamp(opacity, 0.0, 1.0)),
                pittore::RGBAf{color.redF(), color.greenF(), color.blueF(), 1.0f},
                selPtr, &paintTex, &dabDenPaint, &maskPaint, grainPtr, mixOp,
                nativePaintPtr, dabBlend);
        } else {
            pittore::compute::AutoTip tip;
            tip.silhouette = squareTip
                                 ? pittore::compute::TipSilhouette::Square
                                 : pittore::compute::TipSilhouette::Round;
            tip.ratio = static_cast<float>(ratio);
            tip.angleDeg = static_cast<float>(angleDeg);
            tip.hardness = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
            tip.spikes = spikesOpt;
            tip.fadeAniso = static_cast<float>(anisoOpt / 100.0);
            tip.falloff = falloffOpt;
            tip.sharpness = static_cast<float>(sharpOpt / 100.0);
            tip.soften = static_cast<float>(qBound(
                0.0,
                option(activeTool_, QStringLiteral("brush_soften")).toDouble(),
                100.0) /
                                            100.0);
            tip.spacingPct = static_cast<float>(qBound(
                1.0,
                option(activeTool_, QStringLiteral("brush_spacing")).toDouble(),
                200.0));
            tip.autoSpacing =
                option(activeTool_, QStringLiteral("brush_spacing_auto"))
                    .toBool();
            tip.spacingAutoCoeff = static_cast<float>(qBound(
                0.0,
                option(activeTool_,
                       QStringLiteral("brush_spacing_auto_coeff"))
                    .toDouble(),
                8.0));
            tip.sanitize();
            pittore::compute::paint_tip_dab_host(
                dabDst, lw, lh, lcx, lcy, lradius, tip,
                static_cast<float>(std::clamp(opacity, 0.0, 1.0)),
                pittore::RGBAf{color.redF(), color.greenF(), color.blueF(), 1.0f},
                selPtr, &paintTex, &dabDenPaint, &maskPaint, grainPtr, mixOp,
                nativePaintPtr, dabBlend);
        }
        if (!toScratch) {
            // Incremental device refresh (region upload, no stamp churn);
            // falls back to the stamp bump internally when not direct.
            d->touchPixelsAfterDab(*layer, lcx, lcy, lradius, lw, lh,
                                   nativePaintPtr != nullptr);
        }
    }

    // Remember the touched rect (the dab's footprint mapped back to document
    // space, plus the resampling halo: painting a source texel also affects
    // document pixels up to one source texel away); the composite is rebuilt
    // once per input event by flushPaint(), not per dab.
    const double ex = (static_cast<double>(lradius) + 1.0) * lsx + 1.0;
    const double ey = (static_cast<double>(lradius) + 1.0) * lsy + 1.0;
    const QRect dabRect =
        QRect(int(std::floor(docPos.x() - ex)), int(std::floor(docPos.y() - ey)),
              int(std::ceil(2 * ex)) + 1, int(std::ceil(2 * ey)) + 1)
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    if (toScratch)
        washDirty_ = washDirty_.isNull() ? dabRect : washDirty_.united(dabRect);
    if (pittore::core::log::strokeTrace())
        PITTORE_LOG("[paint] dab pos=(%.1f,%.1f) r=%.1f hard=%.2f op=%.2f backend=%s dirty=(%d,%d,%d,%d)",
                     docPos.x(), docPos.y(), radius, hardness, opacity,
                     d->backend ? d->backend->name().c_str() : "cpu", dabRect.x(),
                     dabRect.y(), dabRect.width(), dabRect.height());
    return true;
}

void AppState::setBrushStamp(const QString& id,
                               const pittore::compute::StampTip& tip) {
    if (id.isEmpty() || !tip.valid()) return;
    stamps_.insert(id, tip);
}

const pittore::compute::StampTip* AppState::brushStamp(
    const QString& id) const {
    auto it = stamps_.constFind(id);
    if (it == stamps_.constEnd() || !it->valid()) return nullptr;
    return &(*it);
}

void AppState::clearBrushStamp(const QString& id) { stamps_.remove(id); }

void AppState::setBrushPattern(const QString& id, PatternGray pattern) {
    if (id.isEmpty() || !pattern.valid()) return;
    patterns_.insert(id, std::move(pattern));
}

const AppState::PatternGray* AppState::brushPattern(const QString& id) const {
    auto it = patterns_.constFind(id);
    if (it == patterns_.constEnd() || !it->valid()) return nullptr;
    return &(*it);
}

void AppState::clearBrushPattern(const QString& id) { patterns_.remove(id); }

void AppState::setBrushHose(
    const QString& id,
    const pittore::compute::brushload::LoadedHose& hose) {
    if (id.isEmpty() || hose.cells.empty()) return;
    hoses_.insert(id, hose);
}

const pittore::compute::brushload::LoadedHose* AppState::brushHose(
    const QString& id) const {
    auto it = hoses_.constFind(id);
    if (it == hoses_.constEnd() || it->cells.empty()) return nullptr;
    return &(*it);
}

void AppState::clearBrushHoses() { hoses_.clear(); }

void AppState::beginStrokeState(ToolId tool, std::uint64_t seed) {
    if (seed == 0) seed = pinnedSeed_;
    if (seed == 0) {
        std::random_device rd;
        seed = (std::uint64_t(rd()) << 32) | rd();
    }
    strokeSeed_ = seed;
    strokeRng_.seed(static_cast<std::uint32_t>(seed ^ (seed >> 32)));
    strokeStateLive_ = true;
    nativeCmykStrokeDoc_ = nullptr;
    nativeCmykStrokeLayer_ = nullptr;
    dabCount_ = 0;
    strokeSelLayer_ = nullptr;  // selection cache re-arms on first dab
    smudgeCarry_.clear();         // travelling patch re-arms per stroke
    bgEraseSampled_ = false;    // Once sampling re-arms per stroke
    patternOriginArmed_ = true;  // non-aligned origin latches on first dab
    arthDabIndex_ = 0;           // art-history curl re-arms per stroke
    // Mixer load: fresh foreground load each stroke unless load_after is
    // off and a load survives from the previous stroke.
    if (!(tool == ToolId::MixerBrush && mixerLoadedValid_ &&
          !option(tool, QStringLiteral("load_after")).toBool())) {
        const QColor fg = foreground_;
        const double load =
            (tool == ToolId::MixerBrush)
                ? std::clamp(option(tool, QStringLiteral("load")).toDouble() /
                                 100.0,
                             0.0, 1.0)
                : 1.0;
        mixerLoaded_ = pittore::RGBAf{
            float(fg.redF()), float(fg.greenF()), float(fg.blueF()),
            float(load)};
        mixerLoadedValid_ = true;
    }
    // Wash mode arms for paint tools (erasers and tonal tools always work
    // directly). The scratch allocates lazily on the first dab so no-op
    // strokes cost nothing.
    washArmed_ = (tool == ToolId::Brush || tool == ToolId::Pencil) &&
                 option(tool, QStringLiteral("brush_painting_mode"))
                         .toString() != QStringLiteral("buildup");
    washDirty_ = QRect();
    if (DocumentItem* dd = activeDocument()) dd->washOverlayDirty = QRect();
    // Authored pressure response for this tool (empty = built-in).
    strokeSizeCurve_ = pittore::ui::brushcurve::parseEncoded(
        option(tool, QStringLiteral("brush_size_curve")).toString());
    strokeOpacityCurve_ = pittore::ui::brushcurve::parseEncoded(
        option(tool, QStringLiteral("brush_opacity_curve")).toString());
    strokeFlowCurve_ = pittore::ui::brushcurve::parseEncoded(
        option(tool, QStringLiteral("brush_flow_curve")).toString());
strokeRotationCurve_ = pittore::ui::brushcurve::parseEncoded(
        option(tool, QString("brush_rotation_curve")).toString());
    // Paper-grain origin: random per stroke when the tool asks for it (the
    // usual), else the stored offsets. Document space; dab methods map it
    // into their target space.
    const QVariant rv = option(tool, QStringLiteral("brush_texture_random"));
    if (!rv.isValid() || rv.toBool()) {
        std::uniform_real_distribution<double> uni(0.0, 512.0);
        strokePatternOrigin_ = QPointF(uni(strokeRng_), uni(strokeRng_));
    } else {
        strokePatternOrigin_ = QPointF(
            option(tool, QStringLiteral("brush_texture_offsetx")).toDouble(),
            option(tool, QStringLiteral("brush_texture_offsety")).toDouble());
    }
}

void AppState::endStrokeState() {
    if (nativeCmykStrokeDoc_ && nativeCmykStrokeLayer_ &&
        nativeCmykStrokeLayer_->pixels && nativeCmykStrokeLayer_->cmykPixels &&
        nativeCmykStrokeLayer_->cmykPixels->valid()) {
        nativeCmykStrokeLayer_->cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(
                *nativeCmykStrokeLayer_->pixels);
        nativeCmykStrokeLayer_->cmykSourceStamp =
            nativeCmykStrokeLayer_->sourceStamp;
    }
    nativeCmykStrokeDoc_ = nullptr;
    nativeCmykStrokeLayer_ = nullptr;
    strokeStateLive_ = false;
    washArmed_ = false;
    washDirty_ = QRect();
    if (DocumentItem* dd = activeDocument()) dd->washOverlayDirty = QRect();
    strokeSelLayer_ = nullptr;
    // Mixer clean_after: the brush comes up empty for the next stroke.
    if (activeTool_ == ToolId::MixerBrush &&
        option(ToolId::MixerBrush, QStringLiteral("clean_after")).toBool()) {
        mixerLoaded_ = pittore::RGBAf{0, 0, 0, 0};
        mixerLoadedValid_ = true;
    }
    // Wipe the wash scratch so the next stroke starts transparent even if
    // its first dab takes the direct path (transformed-layer fallback).
    if (washScratch_.width() > 1 || washScratch_.height() > 1) {
        washScratch_.fill(pittore::RGBAf{0, 0, 0, 0});
    }
    std::fill(washScratchCmyk_.begin(), washScratchCmyk_.end(),
              std::array<std::uint16_t, 4>{});
}

const pittore::compute::SelectionMask* AppState::strokeSelectionMask(
    DocumentItem& d, LayerItem& layer, const pittore::Image* targetImage,
    const QPointF& targetOffset, double sx, double sy) {
    if (d.selection.isEmpty()) return nullptr;
    if (strokeSelLayer_ == &layer && strokeSelStamp_ == d.selectionStamp &&
        strokeSelOffset_ == targetOffset && strokeSelSx_ == sx &&
        strokeSelSy_ == sy)
        return &strokeSelCache_;
    if (!layerSelectionMask(d, layer, strokeSelCache_, targetImage,
                            targetOffset, sx, sy)) {
        strokeSelLayer_ = nullptr;
        return nullptr;
    }
    strokeSelLayer_ = &layer;
    strokeSelStamp_ = d.selectionStamp;
    strokeSelOffset_ = targetOffset;
    strokeSelSx_ = sx;
    strokeSelSy_ = sy;
    return &strokeSelCache_;
}

}  // namespace pittore::ui
