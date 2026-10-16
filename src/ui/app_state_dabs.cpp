// Brush dab entry points: erase, tone, blur, background erase, pattern,
// history, art history, mixer and spot heal.
//
// Split out of app_state.cpp; AppState members, no extra declarations needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"
#include "ui/tools/log/tool_log.h"

#include "engine/compute/brushes/blur/blur_dab.h"
#include "engine/compute/brushes/erase/erase.h"
#include "engine/compute/brushes/heal/heal.h"
#include "engine/compute/brushes/history/history_dab.h"
#include "engine/compute/brushes/mixer/mixer.h"
#include "engine/compute/brushes/stamp/pattern.h"
#include "engine/compute/brushes/tone/tone.h"
#include "engine/compute/layer_mask.h"
#include "engine/core/log.h"
#include "engine/core/tonal_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace pittore::ui {

bool AppState::eraseDab(const QPointF& docPos, double radius, double hardness,
                        double opacity, double ratio, double angleDeg,
                        bool squareTip) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->visible || layer->lockTransparency) return false;
    if (layer->kind != LayerItem::Kind::Pixel &&
        layer->kind != LayerItem::Kind::Adjustment)
        return false;

    // On a selected mask the eraser reveals (paints white coverage) rather
    // than touching pixels.
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
    // Canvas rule: the bottom-most pixel layer is the canvas bottom,
    // so erasing restores it instead of punching transparency holes
    // (brush marks lift, the canvas shows through). A transparent bottom
    // erases to real transparency via the normal path below.
    if (!toMask && layer->kind == LayerItem::Kind::Pixel &&
        layer == &d->layers.last() && !d->canvasTransparent) {
        return paintDab(docPos, radius, hardness, opacity, d->canvasPaper,
                        ratio, angleDeg, squareTip);
    }
    if (!toMask) ensureLayerPixels(*d, *layer);
    // Same document → target mapping as paintDab (see above).
    QPointF targetOffset = layer->offset;
    double targetScaleX = layer->scaleX, targetScaleY = layer->scaleY;
    const pittore::Image* targetImage = layer->pixels.get();
    if (toMask) {
        targetImage = constMask;
        maskTransformFor(*layer, targetOffset, targetScaleX, targetScaleY);
    }
    const double lsx = std::max(targetScaleX, 1e-6);
    const double lsy = std::max(targetScaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - targetOffset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - targetOffset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr = strokeSelectionMask(*d, *layer, targetImage,
                                             targetOffset, lsx, lsy);
    if (toMask) {
        pittore::compute::mask_dab_host(
            layer->mask->data(), targetImage->width(), targetImage->height(),
            lcx, lcy, lradius,
            static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
            static_cast<float>(std::clamp(opacity, 0.0, 1.0)), 1.0f, selPtr);
        const int mx0 = std::max(0, static_cast<int>(std::floor(lcx - lradius - 1.0f)));
        const int my0 = std::max(0, static_cast<int>(std::floor(lcy - lradius - 1.0f)));
        const int mx1 = std::min(static_cast<int>(targetImage->width()), static_cast<int>(std::ceil(lcx + lradius + 1.0f)));
        const int my1 = std::min(static_cast<int>(targetImage->height()), static_cast<int>(std::ceil(lcy + lradius + 1.0f)));
        d->refreshMaskRegion(*layer, QRect(mx0, my0, mx1 - mx0, my1 - my0));
    } else {
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
        const pittore::compute::PatternTex eraseTex =
            resolveTexture(activeTool_, lsx, lsy, targetOffset, dabPressure01_);
        const pittore::compute::DabDensity dabDenErase =
            resolveDensity(activeTool_);
        const pittore::compute::MaskTip maskErase =
            resolveMaskTip(activeTool_, dabPressure01_);
        if (roundTip) {
            pittore::compute::erase_dab_host(
                layer->pixels->data(), layer->pixels->width(),
                layer->pixels->height(), lcx, lcy, lradius,
                static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
                static_cast<float>(std::clamp(opacity, 0.0, 1.0)), selPtr,
                &eraseTex, &dabDenErase, &maskErase);
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
            tip.sanitize();
            pittore::compute::erase_tip_dab_host(
                layer->pixels->data(), layer->pixels->width(),
                layer->pixels->height(), lcx, lcy, lradius, tip,
                static_cast<float>(std::clamp(opacity, 0.0, 1.0)), selPtr,
                &eraseTex, &dabDenErase, &maskErase);
        }
        d->touchPixelsAfterDab(*layer, lcx, lcy, lradius,
                               static_cast<std::uint32_t>(layer->pixels->width()),
                               static_cast<std::uint32_t>(layer->pixels->height()),
                               true);
    }

    const double ex = (static_cast<double>(lradius) + 1.0) * lsx + 1.0;
    const double ey = (static_cast<double>(lradius) + 1.0) * lsy + 1.0;
    const QRect dabRect =
        QRect(int(std::floor(docPos.x() - ex)), int(std::floor(docPos.y() - ey)),
              int(std::ceil(2 * ex)) + 1, int(std::ceil(2 * ey)) + 1)
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    if (pittore::core::log::strokeTrace())
        PITTORE_LOG("[erase] dab pos=(%.1f,%.1f) r=%.1f hard=%.2f op=%.2f backend=%s dirty=(%d,%d,%d,%d)",
                     docPos.x(), docPos.y(), radius, hardness, opacity,
                     d->backend ? d->backend->name().c_str() : "cpu", dabRect.x(),
                     dabRect.y(), dabRect.width(), dabRect.height());
    return true;
}

bool AppState::toneDab(const QPointF& docPos, double radius, double hardness,
                       double amount, int op, int range, bool protectTones,
                       bool vibrance) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;

    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;

    // Capture the pre-stroke image once, when the stroke begins: a tonal dab
    // re-renders from these pixels at the stroke's accumulated coverage, so
    // overlapping dabs raise coverage instead of compounding the tone.
    if (!toneStrokeActive_) {
        toneStrokePre_.assign(layer->pixels->data(),
                              layer->pixels->data() + std::size_t(lw) * lh);
        toneStrokeCoverage_.assign(std::size_t(lw) * lh, 0.0f);
        toneStrokeW_ = lw;
        toneStrokeH_ = lh;
        toneStrokeActive_ = true;
    }

    // Same document → layer mapping as paintDab (see above).
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);

    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::tone_stroke_dab_host(
        toneStrokePre_.data(), layer->pixels->data(), toneStrokeCoverage_.data(),
        lw, lh, lcx, lcy, lradius,
        static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
        static_cast<float>(std::clamp(amount, 0.0, 1.0)),
        static_cast<pittore::compute::ToneOp>(std::clamp(op, 0, 3)), range,
        protectTones, vibrance, selPtr, bbox);
    if (!changed) return false;

    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    refreshLayerCmyk(*d, *layer,
                     QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                           bbox[3] - bbox[1]));
    layer->thumbnail = QImage();

    // The touched layer rect mapped back to document space, plus a one-pixel
    // resampling halo for the incremental composite.
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

void AppState::endToneStroke() {
    if (!toneStrokeActive_) return;
    toneStrokeActive_ = false;
    toneStrokePre_.clear();
    toneStrokePre_.shrink_to_fit();
    toneStrokeCoverage_.clear();
    toneStrokeCoverage_.shrink_to_fit();
    toneStrokeW_ = 0;
    toneStrokeH_ = 0;
}

bool AppState::blurSharpenDab(const QPointF& docPos, double radius,
                              double hardness, double strength, bool sharpen,
                              bool protectDetail) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
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
    // GPU fastpath: bboxes at/above 96x96 blur on-device (bbox upload,
    // resident box_blur, bbox download — never a full-frame roundtrip).
    // Smaller dabs stay on the shared CPU core (transfer would dominate).
    pittore::compute::ComputeBackend& be = computeBackend();
    const bool gpu =
        be.type() != pittore::compute::BackendType::CPU &&
        lradius >= 48.0f;
    int bbox[4] = {0, 0, 0, 0};
    bool changed = false;
    if (gpu) {
        const int kr = pittore::compute::blurDabKernelRadius(lradius);
        const int halo = kr * 2 + 1;
        const int bx0 = std::max(0, int(std::floor(lcx - lradius - halo)));
        const int by0 = std::max(0, int(std::floor(lcy - lradius - halo)));
        const int bx1 =
            std::min(int(lw), int(std::ceil(lcx + lradius + halo)));
        const int by1 =
            std::min(int(lh), int(std::ceil(lcy + lradius + halo)));
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
                src->upload();
                be.box_blur(*src, *dst, std::uint32_t(bw), std::uint32_t(bh),
                            kr);
                dst->download();
                const auto* blurred =
                    static_cast<const pittore::RGBAf*>(dst->host());
                const int mx0 =
                    std::max(bx0, int(std::floor(lcx - lradius)));
                const int my0 =
                    std::max(by0, int(std::floor(lcy - lradius)));
                const int mx1 =
                    std::min(bx1, int(std::ceil(lcx + lradius)));
                const int my1 =
                    std::min(by1, int(std::ceil(lcy + lradius)));
                for (int y = my0; y < my1; ++y) {
                    for (int x = mx0; x < mx1; ++x) {
                        const float dx = float(x) + 0.5f - lcx;
                        const float dy = float(y) + 0.5f - lcy;
                        float m = pittore::compute::blurDabMask(dx, dy,
                                                                  lradius,
                                                                  hard) *
                                  st;
                        if (!(m > 0.0f)) continue;
                        if (selPtr) {
                            m *= selPtr->coverage(x, y);
                            if (!(m > 0.0f)) continue;
                        }
                        pittore::RGBAf& o =
                            layer->pixels->data()[std::size_t(y) * lw + x];
                        const pittore::RGBAf& b =
                            blurred[std::size_t(y - by0) * bw + (x - bx0)];
                        if (sharpen) {
                            const float dr = (o.r - b.r) * m;
                            const float dg = (o.g - b.g) * m;
                            const float db = (o.b - b.b) * m;
                            if (protectDetail) {
                                const float lum =
                                    0.2126f * std::fabs(dr) +
                                    0.7152f * std::fabs(dg) +
                                    0.0722f * std::fabs(db);
                                if (lum < 0.015f) continue;
                            }
                            o.r = std::clamp(o.r + dr, 0.0f, 1.0f);
                            o.g = std::clamp(o.g + dg, 0.0f, 1.0f);
                            o.b = std::clamp(o.b + db, 0.0f, 1.0f);
                        } else {
                            o.r += (b.r - o.r) * m;
                            o.g += (b.g - o.g) * m;
                            o.b += (b.b - o.b) * m;
                            o.a += (b.a - o.a) * m;
                        }
                        changed = true;
                    }
                }
                if (changed) {
                    bbox[0] = mx0;
                    bbox[1] = my0;
                    bbox[2] = mx1;
                    bbox[3] = my1;
                }
            }
        }
    }
    if (!gpu || !changed) {
        // Shared CPU core (also the small-dab path on GPU backends).
        changed = pittore::compute::blur_sharpen_dab_host(
            layer->pixels->data(), lw, lh, lcx, lcy, lradius, hard, st,
            sharpen, protectDetail, bbox, selPtr);
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

bool AppState::backgroundEraseDab(const QPointF& docPos, double radius,
                                  double hardness, double opacity,
                                  double tolerance01, int sampling, int limits,
                                  bool protectFg) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Background Eraser: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    const float hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (!(op > 0.0f)) return false;
    const float tol =
        static_cast<float>(std::clamp(tolerance01, 0.0, 1.0));
    const int sx = std::clamp(int(std::floor(lcx)), 0, int(lw) - 1);
    const int sy = std::clamp(int(std::floor(lcy)), 0, int(lh) - 1);
    // Sample colour: Continuous resamples the centre, Once latches the
    // first dab of the stroke, Background Swatch uses the background.
    pittore::RGBAf sample{0, 0, 0, 1};
    if (sampling == 2) {
        const QColor bg = background();
        sample = pittore::RGBAf{float(bg.redF()), float(bg.greenF()),
                                 float(bg.blueF()), 1.0f};
    } else if (sampling == 1 && bgEraseSampled_) {
        sample = bgEraseSample_;
    } else {
        sample = layer->pixels->data()[std::size_t(sy) * lw + sx];
        sample.a = 1.0f;
        if (sampling == 1) {
            bgEraseSample_ = sample;
            bgEraseSampled_ = true;
        }
    }
    const QColor fgQ = foreground();
    const pittore::RGBAf fg{float(fgQ.redF()), float(fgQ.greenF()),
                             float(fgQ.blueF()), 1.0f};
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    bool changed = false;
    if (limits == 0) {
        // Discontiguous: shared host core, or the device bg_erase fastpath
        // on GPU backends (kernel + bbox-only download, like paint_dab).
        pittore::compute::ComputeBackend& be = computeBackend();
        if (be.type() != pittore::compute::BackendType::CPU) {
            const std::size_t need =
                std::size_t(lw) * lh * sizeof(pittore::RGBAf);
            auto buf = be.make_buffer(need);
            if (buf) {
                std::memcpy(buf->host(), layer->pixels->data(), need);
                buf->upload();
                be.bg_erase(*buf, lw, lh, lcx, lcy, lradius, hard, op,
                            sample, tol, protectFg, fg, bbox);
                buf->download();
                // The kernel reports the geometric bbox; effect is decided
                // by comparing bbox bytes (a tolerance miss erases nothing).
                if (bbox[2] > bbox[0] && bbox[3] > bbox[1]) {
                    bool diff = false;
                    for (int y = bbox[1]; y < bbox[3] && !diff; ++y) {
                        if (std::memcmp(
                                layer->pixels->data() + std::size_t(y) * lw +
                                    bbox[0],
                                static_cast<const pittore::RGBAf*>(
                                    buf->host()) +
                                    std::size_t(y) * lw + bbox[0],
                                std::size_t(bbox[2] - bbox[0]) *
                                    sizeof(pittore::RGBAf)) != 0)
                            diff = true;
                    }
                    if (diff) {
                        for (int y = bbox[1]; y < bbox[3]; ++y)
                            std::memcpy(
                                layer->pixels->data() + std::size_t(y) * lw +
                                    bbox[0],
                                static_cast<const pittore::RGBAf*>(
                                    buf->host()) +
                                    std::size_t(y) * lw + bbox[0],
                                std::size_t(bbox[2] - bbox[0]) *
                                    sizeof(pittore::RGBAf));
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            // Selection gating lives inside the core via selPtr.
            changed = pittore::compute::background_erase_dab_host(
                layer->pixels->data(), lw, lh, lcx, lcy, lradius, hard, op,
                sample, tol, protectFg, fg, bbox, selPtr);
        }
    } else {
        // Contiguous / Find Edges: bbox flood from the centre on the host
        // (serial BFS, bbox-small by construction), then scale the flood's
        // erase fraction by the dab rim times opacity. Find Edges currently
        // shares the contiguous path.
        const int bx0 = std::max(0, int(std::floor(lcx - lradius)));
        const int by0 = std::max(0, int(std::floor(lcy - lradius)));
        const int bx1 =
            std::min(int(lw), int(std::ceil(lcx + lradius)));
        const int by1 =
            std::min(int(lh), int(std::ceil(lcy + lradius)));
        if (bx1 > bx0 && by1 > by0) {
            const int bw = bx1 - bx0, bh = by1 - by0;
            std::vector<pittore::RGBAf> tmp(std::size_t(bw) * bh);
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x)
                    tmp[std::size_t(y) * bw + x] =
                        layer->pixels->data()[std::size_t(by0 + y) * lw +
                                              bx0 + x];
            int fbbox[4] = {0, 0, 0, 0};
            // Seed at the dab centre; tolerance match uses the dab sample.
            // flood_fill_host seeds from its own match buffer, so plant the
            // sample at the seed texel first (restored below by the combine).
            const int seedX = std::clamp(sx - bx0, 0, bw - 1);
            const int seedY = std::clamp(sy - by0, 0, bh - 1);
            tmp[std::size_t(seedY) * bw + seedX] = sample;
            pittore::compute::flood_fill_host(
                tmp.data(), tmp.data(), std::uint32_t(bw), std::uint32_t(bh),
                seedX, seedY, tol, /*contiguous=*/true, /*antialias=*/true,
                /*opacity=*/1.0f, pittore::RGBAf{0, 0, 0, 0},
                /*erase=*/true, fbbox, nullptr);
            for (int y = by0; y < by1; ++y) {
                for (int x = bx0; x < bx1; ++x) {
                    const pittore::RGBAf& orig =
                        layer->pixels->data()[std::size_t(y) * lw + x];
                    if (!(orig.a > 0.0f)) continue;
                    const float ta =
                        tmp[std::size_t(y - by0) * bw + (x - bx0)].a;
                    float f = 1.0f - ta / orig.a;
                    if (!(f > 0.0f)) continue;
                    if (selPtr) {
                        f *= selPtr->coverage(x, y);
                        if (!(f > 0.0f)) continue;
                    }
                    if (protectFg) {
                        const float pd = std::max(
                            std::max(std::fabs(orig.r - fg.r),
                                     std::fabs(orig.g - fg.g)),
                            std::fabs(orig.b - fg.b));
                        if (pd <= tol) continue;
                    }
                    const float dx = float(x) + 0.5f - lcx;
                    const float dy = float(y) + 0.5f - lcy;
                    const float t =
                        std::sqrt(dx * dx + dy * dy) / lradius;
                    if (t >= 1.0f) continue;
                    float cov = 1.0f;
                    if (t > hard) {
                        const float span = std::max(1.0f - hard, 1e-4f);
                        cov = (1.0f - t) / span;
                    }
                    const float a = std::clamp(cov * f * op, 0.0f, 1.0f);
                    if (!(a > 0.0f)) continue;
                    layer->pixels->data()[std::size_t(y) * lw + x].a *=
                        (1.0f - a);
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

bool AppState::patternStampDab(const QPointF& docPos, double radius,
                                double hardness, double opacity, int patternId,
                                bool aligned, bool impressionist) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Pattern Stamp: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    const float hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (!(op > 0.0f)) return false;
    const int pid = std::clamp(patternId, 0, 3);
    // Tile cache: procedural bytes, rebuilt only on pattern change.
    if (patternTileId_ != pid || patternTilePx_.size() != 64u * 64u) {
        patternTilePx_ = pittore::compute::make_pattern_tile(pid).px;
        patternTileId_ = pid;
    }
    // Origin: aligned pins the tile to the document; otherwise the
    // stroke's first dab latches it (pattern travels with the stroke).
    // Impressionist jitters the offset per dab from the stroke RNG
    // (seeded in beginStrokeState, so reruns are identical).
    float ox, oy;
    if (aligned) {
        ox = float(-layer->offset.x() / lsx);
        oy = float(-layer->offset.y() / lsy);
    } else {
        if (patternOriginArmed_) {
            patternStrokeStart_ = docPos;
            patternOriginArmed_ = false;
        }
        ox = float((patternStrokeStart_.x() - layer->offset.x()) / lsx);
        oy = float((patternStrokeStart_.y() - layer->offset.y()) / lsy);
    }
    if (impressionist && strokeStateLive_) {
        std::uniform_real_distribution<float> jit(-3.0f, 3.0f);
        ox += jit(strokeRng_);
        oy += jit(strokeRng_);
    }
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    bool changed = false;
    pittore::compute::ComputeBackend& be = computeBackend();
    if (be.type() != pittore::compute::BackendType::CPU) {
        // Device fastpath: tile crosses as a 64KB buffer, only the dab
        // bbox downloads. Falls through to the host core on any failure.
        const std::size_t need =
            std::size_t(lw) * lh * sizeof(pittore::RGBAf);
        auto buf = be.make_buffer(need);
        auto tile = be.make_buffer(64u * 64u * sizeof(pittore::RGBAf));
        if (buf && tile) {
            std::memcpy(buf->host(), layer->pixels->data(), need);
            std::memcpy(tile->host(), patternTilePx_.data(),
                        64u * 64u * sizeof(pittore::RGBAf));
            buf->upload();
            tile->upload();
            be.pattern_stamp(*buf, lw, lh, lcx, lcy, lradius, hard, op,
                             *tile, ox, oy, bbox);
            buf->download();
            if (bbox[2] > bbox[0] && bbox[3] > bbox[1]) {
                bool diff = false;
                for (int y = bbox[1]; y < bbox[3] && !diff; ++y) {
                    if (std::memcmp(
                            layer->pixels->data() + std::size_t(y) * lw +
                                bbox[0],
                            static_cast<const pittore::RGBAf*>(
                                buf->host()) +
                                std::size_t(y) * lw + bbox[0],
                            std::size_t(bbox[2] - bbox[0]) *
                                sizeof(pittore::RGBAf)) != 0)
                        diff = true;
                }
                if (diff) {
                    for (int y = bbox[1]; y < bbox[3]; ++y)
                        std::memcpy(
                            layer->pixels->data() + std::size_t(y) * lw +
                                bbox[0],
                            static_cast<const pittore::RGBAf*>(buf->host()) +
                                std::size_t(y) * lw + bbox[0],
                            std::size_t(bbox[2] - bbox[0]) *
                                sizeof(pittore::RGBAf));
                    changed = true;
                }
            }
        }
    }
    if (!changed) {
        pittore::compute::PatternTile tile;
        tile.id = pid;
        tile.px = patternTilePx_;
        changed = pittore::compute::pattern_stamp_dab_host(
            layer->pixels->data(), lw, lh, lcx, lcy, lradius, hard, op,
            tile, ox, oy, bbox, selPtr);
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

bool AppState::historyBrushDab(const QPointF& docPos, double radius,
                                double hardness, double opacity) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("History Brush: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    // Source: the oldest undo snapshot's matching layer (document-open
    // state). Shared pixel Images are copy-on-write safe to read.
    const LayerItem* srcLayer = historySourceFor(d, d->activeLayer, lw, lh);
    if (!srcLayer) {
        setStatusHint(tr("History Brush: paint something first — there is "
                         "no history state to restore yet."));
        return false;
    }
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    const float hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (!(op > 0.0f)) return false;
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    bool changed = false;
    pittore::compute::ComputeBackend& be = computeBackend();
    if (be.type() != pittore::compute::BackendType::CPU) {
        // Device fastpath: snapshot layer crosses as a buffer, only the
        // dab bbox downloads. Falls through to the host core on failure.
        const std::size_t need =
            std::size_t(lw) * lh * sizeof(pittore::RGBAf);
        auto buf = be.make_buffer(need);
        auto src = be.make_buffer(need);
        if (buf && src) {
            std::memcpy(buf->host(), layer->pixels->data(), need);
            std::memcpy(src->host(), srcLayer->pixels->data(), need);
            buf->upload();
            src->upload();
            be.history_dab(*buf, lw, lh, lcx, lcy, lradius, hard, op,
                           *src, bbox);
            buf->download();
            if (bbox[2] > bbox[0] && bbox[3] > bbox[1]) {
                bool diff = false;
                for (int y = bbox[1]; y < bbox[3] && !diff; ++y) {
                    if (std::memcmp(
                            layer->pixels->data() + std::size_t(y) * lw +
                                bbox[0],
                            static_cast<const pittore::RGBAf*>(
                                buf->host()) +
                                std::size_t(y) * lw + bbox[0],
                            std::size_t(bbox[2] - bbox[0]) *
                                sizeof(pittore::RGBAf)) != 0)
                        diff = true;
                }
                if (diff) {
                    for (int y = bbox[1]; y < bbox[3]; ++y)
                        std::memcpy(
                            layer->pixels->data() + std::size_t(y) * lw +
                                bbox[0],
                            static_cast<const pittore::RGBAf*>(buf->host()) +
                                std::size_t(y) * lw + bbox[0],
                            std::size_t(bbox[2] - bbox[0]) *
                                sizeof(pittore::RGBAf));
                    changed = true;
                }
            }
        }
    }
    if (!changed) {
        changed = pittore::compute::history_brush_dab_host(
            layer->pixels->data(), srcLayer->pixels->data(), lw, lh, lcx,
            lcy, lradius, hard, op, bbox, selPtr);
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

bool AppState::artHistoryDab(const QPointF& docPos, double radius,
                               double hardness, double opacity, int style,
                               double areaPx, double tolerance01) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Art History Brush: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const LayerItem* srcLayer = historySourceFor(d, d->activeLayer, lw, lh);
    if (!srcLayer) {
        setStatusHint(tr("Art History Brush: paint something first — there "
                         "is no history state to restore yet."));
        return false;
    }
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    const float hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const float op = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    if (!(op > 0.0f)) return false;
    // Style table: curl step (deg/dab) + offset length (layer px). Dab
    // paints the source stamp; Tight/Loose + Short/Medium/Long/Curl set
    // how far it wanders. Order matches the options-bar Style combo
    // (Tight Short/Medium/Long, Loose Medium/Long, Dab, Tight/Loose Curl).
    static constexpr float kLen[8] = {6, 12, 20, 12, 20, 0, 10, 16};
    static constexpr float kStep[8] = {25, 25, 25, 60, 60, 0, 140, 140};
    const int si = std::clamp(style, 0, 7);
    const float ang =
        (kStep[si] * float(arthDabIndex_)) * 3.14159265f / 180.0f;
    const float ox = std::cos(ang) * kLen[si];
    const float oy = std::sin(ang) * kLen[si];
    ++arthDabIndex_;
    const float stampR =
        std::clamp(float(areaPx) / 2.0f, 2.0f, 200.0f);
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::arthistory_dab_host(
        layer->pixels->data(), srcLayer->pixels->data(), lw, lh, lcx, lcy,
        lradius, hard, op, ox, oy, stampR,
        static_cast<float>(std::clamp(tolerance01, 0.0, 1.0)), bbox, selPtr);
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

bool AppState::mixerBrushDab(const QPointF& docPos, double radius,
                              double hardness, double flow, double mix,
                              double wet, double load01, bool sampleAll) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Mixer Brush: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    if (!mixerLoadedValid_) {
        const QColor fg = foreground();
        mixerLoaded_ = pittore::RGBAf{float(fg.redF()), float(fg.greenF()),
                                       float(fg.blueF()),
                                       float(std::clamp(load01, 0.0, 1.0))};
        mixerLoadedValid_ = true;
    }
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    // Sample source: the active layer, or the composite resampled to layer
    // space (same buffer the Magic Wand measures).
    const pittore::RGBAf* sample = layer->pixels->data();
    std::vector<pittore::RGBAf> matchBuf;
    if (sampleAll && !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        matchBuf.resize(std::size_t(lw) * lh);
        for (std::uint32_t y = 0; y < lh; ++y) {
            const double docY = layer->offset.y() + (double(y) + 0.5) * lsy;
            const int py = std::clamp(static_cast<int>(std::floor(docY)), 0,
                                      comp.height() - 1);
            const QRgb* row =
                reinterpret_cast<const QRgb*>(comp.constScanLine(py));
            for (std::uint32_t x = 0; x < lw; ++x) {
                const double docX =
                    layer->offset.x() + (double(x) + 0.5) * lsx;
                const int px = std::clamp(static_cast<int>(std::floor(docX)),
                                          0, comp.width() - 1);
                const QRgb c = row[px];
                const int a = qAlpha(c);
                pittore::RGBAf& dstC = matchBuf[std::size_t(y) * lw + x];
                if (a <= 0) {
                    dstC = pittore::RGBAf{0, 0, 0, 0};
                } else {
                    const float inv = 1.0f / float(a);
                    dstC = pittore::RGBAf{qRed(c) * inv, qGreen(c) * inv,
                                           qBlue(c) * inv,
                                           float(a) / 255.0f};
                }
            }
        }
        sample = matchBuf.data();
    }
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::mixer_dab_host(
        layer->pixels->data(), sample, lw, lh, lcx, lcy, lradius,
        static_cast<float>(std::clamp(hardness, 0.0, 1.0)),
        static_cast<float>(std::clamp(flow, 0.0, 1.0)),
        static_cast<float>(std::clamp(mix, 0.0, 1.0)),
        static_cast<float>(std::clamp(wet, 0.0, 1.0)), mixerLoaded_, bbox,
        selPtr);
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

bool AppState::spotHealDab(const QPointF& docPos, double radius,
                           double hardness, int type, int diffusion,
                           bool sampleAll) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) {
        setStatusHint(tr("Spot Healing: select a pixel layer first."));
        return false;
    }

    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;

    // Per-dab perf trace: the Spot Healing tool log gets one line per dab
    // with the ms split (match buffer / algorithm / placement refresh), so a
    // slow big-brush dab names its expensive stage (see the function tail).
    const auto tDab0 = std::chrono::steady_clock::now();

    // Same document → layer mapping as paintDab (see above).
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);

    // Donor measurement buffer: the layer itself, or the composite resampled
    // to layer space for Sample All Layers (nearest neighbour — the same
    // buffer the Magic Wand measures, so the donor search sees what the eye
    // sees). Writes always land on the active layer's own pixels.
    std::vector<pittore::RGBAf> matchBuf;
    const pittore::RGBAf* match = layer->pixels->data();
    if (sampleAll && !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        matchBuf.resize(std::size_t(lw) * lh);
        for (std::uint32_t y = 0; y < lh; ++y) {
            const double docY = layer->offset.y() + (double(y) + 0.5) * lsy;
            const int py = std::clamp(static_cast<int>(std::floor(docY)), 0,
                                      comp.height() - 1);
            const QRgb* row =
                reinterpret_cast<const QRgb*>(comp.constScanLine(py));
            for (std::uint32_t x = 0; x < lw; ++x) {
                const double docX =
                    layer->offset.x() + (double(x) + 0.5) * lsx;
                const int px = std::clamp(static_cast<int>(std::floor(docX)), 0,
                                          comp.width() - 1);
                const QRgb c = row[px];
                const int a = qAlpha(c);
                pittore::RGBAf& dstC =
                    matchBuf[std::size_t(y) * lw + x];
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
        match = matchBuf.data();
    }
    const auto tMatch = std::chrono::steady_clock::now();

    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::spot_heal_host(
        layer->pixels->data(), lw, lh, lcx, lcy, lradius,
        static_cast<float>(std::clamp(hardness, 0.0, 1.0)), match,
        static_cast<pittore::compute::HealType>(std::clamp(type, 0, 2)),
        std::clamp(diffusion, 1, 7), bbox, selPtr);
    const auto tHeal = std::chrono::steady_clock::now();
    const auto dabMs = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    if (!changed) {
        tool_logf(ToolId::SpotHealing, "perf/dab",
                  "pos=(%.0f,%.0f) r=%.0f type=%d sample_all=%d changed=0 "
                  "match=%.2f heal=%.2f total=%.2f",
                  docPos.x(), docPos.y(), radius, type, sampleAll ? 1 : 0,
                  dabMs(tDab0, tMatch), dabMs(tMatch, tHeal),
                  dabMs(tDab0, tHeal));
        setStatusHint(tr("Spot Healing: nothing to heal here."));
        return false;
    }

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
    const auto tDabEnd = std::chrono::steady_clock::now();
    // One line per dab to the tool's own log: which stage ate the ms —
    // match = Sample-All-Layers composite resample (O(layer pixels) per
    // dab), heal = the algorithm, refresh = placement mark + dirty rect
    // (the viewport composite runs later, reported in the [render] logs).
    tool_logf(ToolId::SpotHealing, "perf/dab",
              "pos=(%.0f,%.0f) r=%.0f hard=%.2f type=%d diff=%d "
              "sample_all=%d bbox=%dx%d match=%.2f heal=%.2f refresh=%.2f "
              "total=%.2f",
              docPos.x(), docPos.y(), radius, hardness, type, diffusion,
              sampleAll ? 1 : 0, bbox[2] - bbox[0], bbox[3] - bbox[1],
              dabMs(tDab0, tMatch), dabMs(tMatch, tHeal),
              dabMs(tHeal, tDabEnd), dabMs(tDab0, tDabEnd));
    return true;
}

}  // namespace pittore::ui
