// Retouch: heal, patch, remove, content-aware move, red eye.
//
// Split out of app_state.cpp; AppState members, no extra declarations needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"

#include "engine/compute/brushes/heal/heal.h"
#include "engine/compute/brushes/stamp/pattern.h"
#include "engine/core/log.h"
#include "engine/filter/core/filter_detail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace pittore::ui {

bool AppState::healBrushDab(const QPointF& docPos, double radius,
                              double hardness, const QPointF& offsetDoc,
                              int sampleMode, int diffusion, bool usePattern,
                              int patternId) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) {
        setStatusHint(tr("Healing Brush: select a pixel layer first."));
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
    const int diff = std::clamp(diffusion, 1, 7);
    const int sample = std::clamp(sampleMode, 0, 2);
    const int pid = std::clamp(patternId, 0, 3);
    const float ox = float(offsetDoc.x() / lsx);
    const float oy = float(offsetDoc.y() / lsy);
    // Donor key: quantized offset + sample + pattern + size + stroke. The
    // translate is one full-frame pass — rebuilt only on key change,
    // amortized over the stroke's dabs (µs each).
    const int qox = int(std::round(ox * 4.0f));
    const int qoy = int(std::round(oy * 4.0f));
    const std::uint64_t seed = strokeSeed_;
    bool keyMatch = !healDonor_.empty() && healDonorW_ == lw &&
                    healDonorH_ == lh && healDonorSample_ == sample &&
                    healDonorPattern_ == (usePattern ? pid : -2) &&
                    healDonorOx_ == float(qox) / 4.0f &&
                    healDonorOy_ == float(qoy) / 4.0f &&
                    healDonorSeed_ == seed;
    if (!keyMatch) {
        healDonor_.assign(std::size_t(lw) * lh, pittore::RGBAf{0, 0, 0, 0});
        if (usePattern) {
            const auto tile = pittore::compute::make_pattern_tile(pid);
            for (std::uint32_t y = 0; y < lh; ++y)
                for (std::uint32_t x = 0; x < lw; ++x)
                    healDonor_[std::size_t(y) * lw + x] =
                        tile.px[std::size_t(y % 64) * 64 + (x % 64)];
        } else {
            // Frozen base: the layer itself, or the composite resampled to
            // layer space for the composite sample modes (same buffer the
            // Magic Wand measures) — caught now so the stroke never feeds
            // its own output back.
            const pittore::RGBAf* base = layer->pixels->data();
            std::vector<pittore::RGBAf> matchBuf;
            if (sample != 0 && !d->composite.isNull()) {
                QImage comp = d->composite;
                if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
                    comp.format() != QImage::Format_ARGB32)
                    comp = comp.convertToFormat(
                        QImage::Format_ARGB32_Premultiplied);
                matchBuf.resize(std::size_t(lw) * lh);
                for (std::uint32_t y = 0; y < lh; ++y) {
                    const double docY =
                        layer->offset.y() + (double(y) + 0.5) * lsy;
                    const int py = std::clamp(
                        static_cast<int>(std::floor(docY)), 0,
                        comp.height() - 1);
                    const QRgb* row = reinterpret_cast<const QRgb*>(
                        comp.constScanLine(py));
                    for (std::uint32_t x = 0; x < lw; ++x) {
                        const double docX =
                            layer->offset.x() + (double(x) + 0.5) * lsx;
                        const int px = std::clamp(
                            static_cast<int>(std::floor(docX)), 0,
                            comp.width() - 1);
                        const QRgb c = row[px];
                        const int a = qAlpha(c);
                        pittore::RGBAf& dstC =
                            matchBuf[std::size_t(y) * lw + x];
                        if (a <= 0) {
                            dstC = pittore::RGBAf{0, 0, 0, 0};
                        } else {
                            const float inv = 1.0f / float(a);
                            dstC = pittore::RGBAf{qRed(c) * inv,
                                                   qGreen(c) * inv,
                                                   qBlue(c) * inv,
                                                   float(a) / 255.0f};
                        }
                    }
                }
                base = matchBuf.data();
            }
            std::vector<pittore::RGBAf> baseCopy(
                base, base + std::size_t(lw) * lh);
            pittore::compute::translate_heal_donor_host(
                baseCopy.data(), healDonor_.data(), lw, lh, ox, oy);
        }
        healDonorW_ = lw;
        healDonorH_ = lh;
        healDonorOx_ = float(qox) / 4.0f;
        healDonorOy_ = float(qoy) / 4.0f;
        healDonorSample_ = sample;
        healDonorPattern_ = usePattern ? pid : -2;
        healDonorSeed_ = seed;
    }
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    int bbox[4] = {0, 0, 0, 0};
    // Proximity (single global donor, no per-pixel refinement): the live
    // brush budget is single-digit ms; ContentAware's refinement is
    // reserved for one-shot Patch application.
    const bool changed = pittore::compute::spot_heal_host(
        layer->pixels->data(), lw, lh, lcx, lcy, lradius, hard,
        healDonor_.data(), pittore::compute::HealType::Proximity, diff,
        bbox, selPtr);
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

bool AppState::patchTransfer(const QPointF& deltaDoc, int mode,
                               int featherPx, int colorMix, bool sampleAll) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Patch: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int dx = int(std::round(deltaDoc.x() / lsx));
    const int dy = int(std::round(deltaDoc.y() / lsy));
    if (dx == 0 && dy == 0) return false;
    if (d->selection.isEmpty()) {
        setStatusHint(tr("Patch: select an area first, then drag it onto "
                         "clean texture."));
        return false;
    }
    const int feather = std::clamp(featherPx, 1, 7);
    const double mix = std::clamp(colorMix, 0, 10) / 10.0;
    const bool contentAware = std::clamp(mode, 0, 1) == 1;
    const bool transparent =
        option(ToolId::Patch, QStringLiteral("transparent")).toBool();
    // Work bbox: selection grown by the feather, in layer pixels.
    const int selX0 = std::max(
        0, int(std::floor((d->selection.left() - layer->offset.x()) / lsx)) -
               feather);
    const int selY0 = std::max(
        0, int(std::floor((d->selection.top() - layer->offset.y()) / lsy)) -
               feather);
    const int selX1 = std::min(
        int(lw), int(std::ceil((d->selection.right() - layer->offset.x()) /
                               lsx)) +
                       feather);
    const int selY1 = std::min(
        int(lh), int(std::ceil((d->selection.bottom() - layer->offset.y()) /
                               lsy)) +
                       feather);
    if (selX1 <= selX0 || selY1 <= selY0) return false;
    const int bw = selX1 - selX0, bh = selY1 - selY0;
    // Coverage: rect / ellipse / mask channel, then feathered.
    std::vector<float> cover(std::size_t(bw) * bh, 0.0f);
    const bool ellipse = d->selectionIsEllipse && !d->selectionIsMask;
    const QImage& maskImg = d->selectionMask;
    const bool useMask = d->selectionIsMask && !maskImg.isNull();
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const double docX =
                layer->offset.x() + (selX0 + x + 0.5) * lsx;
            const double docY =
                layer->offset.y() + (selY0 + y + 0.5) * lsy;
            float c = 0.0f;
            if (useMask) {
                const int mx = std::clamp(int(std::floor(docX)), 0,
                                          maskImg.width() - 1);
                const int my = std::clamp(int(std::floor(docY)), 0,
                                          maskImg.height() - 1);
                c = qGray(maskImg.pixel(mx, my)) / 255.0f;
            } else if (d->selection.contains(QPointF(docX, docY))) {
                c = 1.0f;
                if (ellipse) {
                    const QPointF ctr = d->selection.center();
                    const double rx = d->selection.width() / 2.0;
                    const double ry = d->selection.height() / 2.0;
                    if (rx > 0.0 && ry > 0.0) {
                        const double ex = (docX - ctr.x()) / rx;
                        const double ey = (docY - ctr.y()) / ry;
                        c = (ex * ex + ey * ey <= 1.0) ? 1.0f : 0.0f;
                    }
                }
            }
            cover[std::size_t(y) * bw + x] = c;
        }
    }
    // Feather via a small box blur of the coverage (separable, bbox-only).
    {
        std::vector<float> tmp = cover;
        const int r = feather;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                double s = 0;
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sx = std::clamp(x + k, 0, bw - 1);
                    s += tmp[std::size_t(y) * bw + sx];
                    ++n;
                }
                cover[std::size_t(y) * bw + x] = float(s / n);
            }
        tmp = cover;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                double s = 0;
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sy = std::clamp(y + k, 0, bh - 1);
                    s += tmp[std::size_t(sy) * bw + x];
                    ++n;
                }
                cover[std::size_t(y) * bw + x] = float(s / n);
            }
    }
    // Donor base: active layer, or the composite resampled to layer space.
    std::vector<pittore::RGBAf> matchBuf;
    const pittore::RGBAf* donorBase = layer->pixels->data();
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
        donorBase = matchBuf.data();
    }
    // Low-frequency pair for the lighting match (fixed r=8 characteristics).
    std::vector<pittore::RGBAf> lowT(std::size_t(bw) * bh);
    std::vector<pittore::RGBAf> lowD(std::size_t(bw) * bh);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            lowT[std::size_t(y) * bw + x] =
                layer->pixels->data()[std::size_t(selY0 + y) * lw + selX0 +
                                      x];
            const int qx = std::clamp(selX0 + x + dx, 0, int(lw) - 1);
            const int qy = std::clamp(selY0 + y + dy, 0, int(lh) - 1);
            lowD[std::size_t(y) * bw + x] =
                donorBase[std::size_t(qy) * lw + qx];
        }
    {
        pittore::Image imgT{std::uint32_t(bw), std::uint32_t(bh)};
        pittore::Image imgD{std::uint32_t(bw), std::uint32_t(bh)};
        std::memcpy(imgT.data(), lowT.data(),
                    lowT.size() * sizeof(pittore::RGBAf));
        std::memcpy(imgD.data(), lowD.data(),
                    lowD.size() * sizeof(pittore::RGBAf));
        pittore::Image scratchT{std::uint32_t(bw), std::uint32_t(bh)};
        pittore::Image scratchD{std::uint32_t(bw), std::uint32_t(bh)};
        pittore::filter::detail::boxBlurInto(imgT, scratchT, 8);
        pittore::filter::detail::boxBlurInto(imgD, scratchD, 8);
        std::memcpy(lowT.data(), scratchT.data(),
                    lowT.size() * sizeof(pittore::RGBAf));
        std::memcpy(lowD.data(), scratchD.data(),
                    lowD.size() * sizeof(pittore::RGBAf));
    }
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    std::atomic<bool> touched{false};
    pittore::core::parallel_rows(
        std::uint32_t(bh), [&](std::uint32_t y0, std::uint32_t y1) {
            bool local = false;
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (int x = 0; x < bw; ++x) {
                    float m = cover[std::size_t(y) * bw + x];
                    if (!(m > 0.0f)) continue;
                    if (selPtr) {
                        m *= selPtr->coverage(selX0 + x, selY0 + int(y));
                        if (!(m > 0.0f)) continue;
                    }
                    const int qx = selX0 + x + dx, qy = selY0 + int(y) + dy;
                    if (qx < 0 || qy < 0 || qx >= int(lw) || qy >= int(lh))
                        continue;
                    pittore::RGBAf& o =
                        layer->pixels
                            ->data()[std::size_t(selY0 + y) * lw + selX0 + x];
                    const pittore::RGBAf before = o;
                    const pittore::RGBAf& dn =
                        donorBase[std::size_t(qy) * lw + qx];
                    const pittore::RGBAf& lt =
                        lowT[std::size_t(y) * bw + x];
                    const pittore::RGBAf& ld =
                        lowD[std::size_t(y) * bw + x];
                    pittore::RGBAf out;
                    if (transparent) {
                        // Donor detail on the target base.
                        out.r = o.r + (dn.r - ld.r) * float(m);
                        out.g = o.g + (dn.g - ld.g) * float(m);
                        out.b = o.b + (dn.b - ld.b) * float(m);
                        out.a = o.a;
                    } else if (contentAware) {
                        const float k = float(mix * m);
                        out.r = dn.r + (lt.r - ld.r) * k;
                        out.g = dn.g + (lt.g - ld.g) * k;
                        out.b = dn.b + (lt.b - ld.b) * k;
                        out.a = dn.a;
                        out.r = out.r * m + o.r * (1.0f - m);
                        out.g = out.g * m + o.g * (1.0f - m);
                        out.b = out.b * m + o.b * (1.0f - m);
                        out.a = out.a * m + o.a * (1.0f - m);
                    } else {
                        out.r = dn.r * m + o.r * (1.0f - m);
                        out.g = dn.g * m + o.g * (1.0f - m);
                        out.b = dn.b * m + o.b * (1.0f - m);
                        out.a = dn.a * m + o.a * (1.0f - m);
                    }
                    o = out;
                    if (std::memcmp(&before, &o, sizeof(pittore::RGBAf)) !=
                        0)
                        local = true;
                }
            }
            if (local) touched.store(true);
        });
    if (!touched.load()) return false;
    d->refreshPlacedRegion(
        *layer, QRect(selX0, selY0, bw, bh));
    refreshLayerCmyk(*d, *layer, QRect(selX0, selY0, bw, bh));
    layer->thumbnail = QImage();
    const double dx0 = layer->offset.x() + (selX0 - 1.0) * lsx;
    const double dy0 = layer->offset.y() + (selY0 - 1.0) * lsy;
    const double dx1 = layer->offset.x() + (selX1 + 1.0) * lsx;
    const double dy1 = layer->offset.y() + (selY1 + 1.0) * lsy;
    const QRect dabRect =
        QRectF(QPointF(dx0 - 1.0, dy0 - 1.0), QPointF(dx1 + 1.0, dy1 + 1.0))
            .toAlignedRect()
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    flushPaint();  // one-shot commit (no per-move painting): recomposite now
    return true;
}

bool AppState::removeMarkDab(const QPointF& docPos, double radius,
                              double hardness) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) return false;
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    if (removeW_ != lw || removeH_ != lh) {
        removeCoverage_.assign(std::size_t(lw) * lh, 0.0f);
        removeW_ = lw;
        removeH_ = lh;
    }
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const float lcx = static_cast<float>((docPos.x() - layer->offset.x()) / lsx);
    const float lcy = static_cast<float>((docPos.y() - layer->offset.y()) / lsy);
    const float lradius = static_cast<float>(radius / std::min(lsx, lsy));
    if (!(lradius > 0.0f)) return false;
    const float hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const int y0 = std::max(0, int(std::floor(lcy - lradius)));
    const int y1 = std::min(int(lh) - 1, int(std::ceil(lcy + lradius)));
    const int x0 = std::max(0, int(std::floor(lcx - lradius)));
    const int x1 = std::min(int(lw) - 1, int(std::ceil(lcx + lradius)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x) + 0.5f - lcx;
            const float dy = float(y) + 0.5f - lcy;
            const float t =
                std::sqrt(dx * dx + dy * dy) / std::max(lradius, 1e-6f);
            if (t >= 1.0f) continue;
            float m = 1.0f;
            if (t > hard) {
                const float span = std::max(1.0f - hard, 1e-4f);
                m = (1.0f - t) / span;
            }
            float& c = removeCoverage_[std::size_t(y) * lw + x];
            c = std::min(1.0f, c + m);
        }
    return true;
}

// Spot-heal grid fill over a bbox, shared by Remove (marked area) and
// ContentAwareMove (vacated hole): Proximity dabs on a step grid (the
// grid covers the bbox, not the layer), gated by `cover` when given
// (null = whole bbox). Reports per-dab regions for the incremental
// composite. Returns whether any dab moved pixels.
bool AppState::healFillGrid(DocumentItem& d, LayerItem& layer,
                         const std::vector<pittore::RGBAf>& donor, int bx0,
                         int by0, int bx1, int by1, const float* cover,
                         int diff) {
    const std::uint32_t lw = layer.pixels->width();
    const std::uint32_t lh = layer.pixels->height();
    const int step = std::max(8, (std::min(bx1 - bx0, by1 - by0) + 3) / 4);
    bool moved = false;
    // One refresh for the whole grid (not per dab): all callers are
    // one-shot fills, so there is no live stroke to preview.
    int ux0 = int(lw), uy0 = int(lh), ux1 = -1, uy1 = -1;
    for (int cy = by0; cy <= by1; cy += step)
        for (int cx = bx0; cx <= bx1; cx += step) {
            const float r = float(step);
            int bbox[4] = {0, 0, 0, 0};
            // Only dabs whose disc touches work do work; the heal core
            // reports its own bbox for the dirty rect.
            if (cover) {
                bool near = false;
                for (int y = std::max(by0, cy - step);
                     y <= std::min(by1, cy + step) && !near; ++y)
                    for (int x = std::max(bx0, cx - step);
                         x <= std::min(bx1, cx + step); ++x)
                        if (cover[std::size_t(y) * lw + x] > 0.01f) {
                            near = true;
                            break;
                        }
                if (!near) continue;
            }
            if (pittore::compute::spot_heal_host(
                    layer.pixels->data(), lw, lh, float(cx) + 0.5f,
                    float(cy) + 0.5f, r, 0.5f, donor.data(),
                    pittore::compute::HealType::Proximity, diff, bbox,
                    nullptr)) {
                moved = true;
                ux0 = std::min(ux0, bbox[0]);
                uy0 = std::min(uy0, bbox[1]);
                ux1 = std::max(ux1, bbox[2]);
                uy1 = std::max(uy1, bbox[3]);
            }
        }
    if (moved) {
        d.refreshPlacedRegion(layer,
                              QRect(ux0, uy0, ux1 - ux0, uy1 - uy0));
    }
    return moved;
}

bool AppState::removeHealMarked(bool consume, bool sampleAll, int diffusion) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (removeW_ != lw || removeH_ != lh || removeCoverage_.empty())
        return false;
    int bx0 = int(lw), by0 = int(lh), bx1 = -1, by1 = -1;
    for (std::uint32_t y = 0; y < lh; ++y)
        for (std::uint32_t x = 0; x < lw; ++x)
            if (removeCoverage_[std::size_t(y) * lw + x] > 0.01f) {
                bx0 = std::min(bx0, int(x));
                by0 = std::min(by0, int(y));
                bx1 = std::max(bx1, int(x));
                by1 = std::max(by1, int(y));
            }
    if (bx1 < bx0) return false;
    // Donor: the layer itself, or the composite for sample-all (frozen now
    // so the fill never feeds its own output back). Grow-only scratch so
    // steady state pays the copy but never allocation + first-touch.
    healFillScratch_.assign(layer->pixels->data(),
                            layer->pixels->data() + std::size_t(lw) * lh);
    std::vector<pittore::RGBAf>& donor = healFillScratch_;
    if (sampleAll && !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const double lsx = std::max(layer->scaleX, 1e-6);
        const double lsy = std::max(layer->scaleY, 1e-6);
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
                pittore::RGBAf& dstC = donor[std::size_t(y) * lw + x];
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
    }
    // Heal the marked bbox on the shared spot-heal grid.
    const int diff = std::clamp(diffusion, 1, 7);
    const bool moved =
        healFillGrid(*d, *layer, donor, bx0, by0, bx1, by1,
                     removeCoverage_.data(), diff);
    if (consume) {
        removeCoverage_.assign(removeCoverage_.size(), 0.0f);
    }
    if (!moved) return false;
    refreshLayerCmyk(*d, *layer, QRect(bx0, by0, bx1 - bx0 + 1,
                                      by1 - by0 + 1));
    layer->thumbnail = QImage();
    d->rebuildComposite();
    return true;
}

void AppState::endRemoveStroke(bool keep) {
    if (!keep) {
        removeCoverage_.clear();
        removeCoverage_.shrink_to_fit();
        removeW_ = 0;
        removeH_ = 0;
    }
}

bool AppState::contentAwareMove(const QPointF& deltaDoc, bool extend,
                                 int featherPx, int colorMix,
                                 bool sampleAll) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Content-Aware Move: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int dx = int(std::round(deltaDoc.x() / lsx));
    const int dy = int(std::round(deltaDoc.y() / lsy));
    if (dx == 0 && dy == 0) return false;
    if (d->selection.isEmpty()) {
        setStatusHint(tr("Content-Aware Move: select an area first, then "
                         "drag it somewhere new."));
        return false;
    }
    const int feather = std::clamp(featherPx, 1, 7);
    const double mix = std::clamp(colorMix, 0, 10) / 10.0;
    // Selection bbox grown by the feather, in layer pixels.
    const int selX0 = std::max(
        0, int(std::floor((d->selection.left() - layer->offset.x()) / lsx)) -
               feather);
    const int selY0 = std::max(
        0, int(std::floor((d->selection.top() - layer->offset.y()) / lsy)) -
               feather);
    const int selX1 = std::min(
        int(lw), int(std::ceil((d->selection.right() - layer->offset.x()) /
                               lsx)) +
                        feather);
    const int selY1 = std::min(
        int(lh), int(std::ceil((d->selection.bottom() - layer->offset.y()) /
                               lsy)) +
                        feather);
    if (selX1 <= selX0 || selY1 <= selY0) return false;
    const int bw = selX1 - selX0, bh = selY1 - selY0;
    // Coverage over the selection bbox (rect / ellipse / mask), feathered.
    std::vector<float> cover(std::size_t(bw) * bh, 0.0f);
    const bool ellipse = d->selectionIsEllipse && !d->selectionIsMask;
    const QImage& maskImg = d->selectionMask;
    const bool useMask = d->selectionIsMask && !maskImg.isNull();
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const double docX =
                layer->offset.x() + (selX0 + x + 0.5) * lsx;
            const double docY =
                layer->offset.y() + (selY0 + y + 0.5) * lsy;
            float c = 0.0f;
            if (useMask) {
                const int mx = std::clamp(int(std::floor(docX)), 0,
                                          maskImg.width() - 1);
                const int my = std::clamp(int(std::floor(docY)), 0,
                                          maskImg.height() - 1);
                c = qGray(maskImg.pixel(mx, my)) / 255.0f;
            } else if (d->selection.contains(QPointF(docX, docY))) {
                c = 1.0f;
                if (ellipse) {
                    const QPointF ctr = d->selection.center();
                    const double rx = d->selection.width() / 2.0;
                    const double ry = d->selection.height() / 2.0;
                    if (rx > 0.0 && ry > 0.0) {
                        const double ex = (docX - ctr.x()) / rx;
                        const double ey = (docY - ctr.y()) / ry;
                        c = (ex * ex + ey * ey <= 1.0) ? 1.0f : 0.0f;
                    }
                }
            }
            cover[std::size_t(y) * bw + x] = c;
        }
    }
    {
        std::vector<float> tmp = cover;
        const int r = feather;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                double s = 0;
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sx = std::clamp(x + k, 0, bw - 1);
                    s += tmp[std::size_t(y) * bw + sx];
                    ++n;
                }
                cover[std::size_t(y) * bw + x] = float(s / n);
            }
        tmp = cover;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                double s = 0;
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sy = std::clamp(y + k, 0, bh - 1);
                    s += tmp[std::size_t(sy) * bw + x];
                    ++n;
                }
                cover[std::size_t(y) * bw + x] = float(s / n);
            }
    }
    // Snapshot the selection content before anything mutates (paste reads
    // pre-move texels even after the hole heals beneath them).
    std::vector<pittore::RGBAf> snap(std::size_t(bw) * bh);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x)
            snap[std::size_t(y) * bw + x] =
                layer->pixels->data()[std::size_t(selY0 + y) * lw + selX0 +
                                      x];
    // Donor base for the hole heal: the live layer, or the composite
    // resampled to layer space when sampling all layers. Grow-only
    // scratch (see Remove path).
    healFillScratch_.assign(layer->pixels->data(),
                            layer->pixels->data() + std::size_t(lw) * lh);
    std::vector<pittore::RGBAf>& donor = healFillScratch_;
    if (sampleAll && !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
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
                pittore::RGBAf& dstC = donor[std::size_t(y) * lw + x];
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
    }
    // Move mode heals the vacated hole first (donor scoring skips hole
    // texels, so the fill reads the surroundings, not the selection).
    // Extend mode skips this: the source stays put.
    const int diff = feather;
    const bool healed =
        !extend && healFillGrid(*d, *layer, donor, selX0, selY0, selX1, selY1,
                               nullptr, diff);
    // Paste at the drop point with the lighting match. Drop bbox covers
    // the shifted selection; lowT reads the (possibly healed) target.
    const int dx0 = std::max(0, selX0 + dx), dy0 = std::max(0, selY0 + dy);
    const int dx1 = std::min(int(lw), selX1 + dx);
    const int dy1 = std::min(int(lh), selY1 + dy);
    std::atomic<bool> touched{false};
    if (dx1 > dx0 && dy1 > dy0) {
        const int dw = dx1 - dx0, dh = dy1 - dy0;
        std::vector<pittore::RGBAf> lowT(std::size_t(dw) * dh);
        std::vector<pittore::RGBAf> lowD(std::size_t(dw) * dh);
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                lowT[std::size_t(y) * dw + x] =
                    layer->pixels->data()[std::size_t(dy0 + y) * lw + dx0 +
                                          x];
                const int qx = std::clamp(dx0 + x - dx - selX0, 0, bw - 1);
                const int qy = std::clamp(dy0 + y - dy - selY0, 0, bh - 1);
                lowD[std::size_t(y) * dw + x] =
                    snap[std::size_t(qy) * bw + qx];
            }
        {
            pittore::Image imgT{std::uint32_t(dw), std::uint32_t(dh)};
            pittore::Image imgD{std::uint32_t(dw), std::uint32_t(dh)};
            std::memcpy(imgT.data(), lowT.data(),
                        lowT.size() * sizeof(pittore::RGBAf));
            std::memcpy(imgD.data(), lowD.data(),
                        lowD.size() * sizeof(pittore::RGBAf));
            pittore::Image scratchT{std::uint32_t(dw), std::uint32_t(dh)};
            pittore::Image scratchD{std::uint32_t(dw), std::uint32_t(dh)};
            pittore::filter::detail::boxBlurInto(imgT, scratchT, 8);
            pittore::filter::detail::boxBlurInto(imgD, scratchD, 8);
            std::memcpy(lowT.data(), scratchT.data(),
                        lowT.size() * sizeof(pittore::RGBAf));
            std::memcpy(lowD.data(), scratchD.data(),
                        lowD.size() * sizeof(pittore::RGBAf));
        }
        const auto* selPtr =
            strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
        (void)selPtr;  // No selPtr gate here: writes land at the DROP, far
        // from the selection the cached mask covers. `cover` (shifted)
        // already carries rect/ellipse/mask shape + feather.
        pittore::core::parallel_rows(
            std::uint32_t(dh), [&](std::uint32_t y0, std::uint32_t y1) {
                bool local = false;
                for (std::uint32_t y = y0; y < y1; ++y) {
                    for (int x = 0; x < dw; ++x) {
                        const int sx = int(dx0 + x - dx - selX0);
                        const int sy = int(dy0 + y - dy - selY0);
                        float m = 0.0f;
                        if (sx >= 0 && sy >= 0 && sx < bw && sy < bh)
                            m = cover[std::size_t(sy) * bw + sx];
                        if (!(m > 0.0f)) continue;
                        pittore::RGBAf& o =
                            layer->pixels->data()[std::size_t(dy0 + y) * lw +
                                                  dx0 + x];
                        const pittore::RGBAf before = o;
                        const int qx = std::clamp(sx, 0, bw - 1);
                        const int qy = std::clamp(sy, 0, bh - 1);
                        const pittore::RGBAf& dn =
                            snap[std::size_t(qy) * bw + qx];
                        const pittore::RGBAf& lt =
                            lowT[std::size_t(y) * dw + x];
                        const pittore::RGBAf& ld =
                            lowD[std::size_t(y) * dw + x];
                        const float k = float(mix * m);
                        pittore::RGBAf out;
                        out.r = dn.r + (lt.r - ld.r) * k;
                        out.g = dn.g + (lt.g - ld.g) * k;
                        out.b = dn.b + (lt.b - ld.b) * k;
                        out.a = dn.a;
                        out.r = out.r * m + o.r * (1.0f - m);
                        out.g = out.g * m + o.g * (1.0f - m);
                        out.b = out.b * m + o.b * (1.0f - m);
                        out.a = out.a * m + o.a * (1.0f - m);
                        o = out;
                        if (std::memcmp(&before, &o,
                                        sizeof(pittore::RGBAf)) != 0)
                            local = true;
                    }
                }
                if (local) touched.store(true);
            });
    }
    const int ux0 = extend ? dx0 : std::min(selX0, dx0);
    const int uy0 = extend ? dy0 : std::min(selY0, dy0);
    const int ux1 = extend ? dx1 : std::max(selX1, dx1);
    const int uy1 = extend ? dy1 : std::max(selY1, dy1);
    if (!touched.load() && !healed) return false;
    d->refreshPlacedRegion(*layer,
                           QRect(ux0, uy0, ux1 - ux0, uy1 - uy0));
    refreshLayerCmyk(*d, *layer, QRect(ux0, uy0, ux1 - ux0, uy1 - uy0));
    layer->thumbnail = QImage();
    const double rx0 = layer->offset.x() + (ux0 - 1.0) * lsx;
    const double ry0 = layer->offset.y() + (uy0 - 1.0) * lsy;
    const double rx1 = layer->offset.x() + (ux1 + 1.0) * lsx;
    const double ry1 = layer->offset.y() + (uy1 + 1.0) * lsy;
    const QRect dabRect =
        QRectF(QPointF(rx0 - 1.0, ry0 - 1.0), QPointF(rx1 + 1.0, ry1 + 1.0))
            .toAlignedRect()
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dabRect : d->paintDirty.united(dabRect);
    flushPaint();  // one-shot commit (no per-move painting): recomposite now
    return true;
}

void AppState::endHealStroke() {
    healDonor_.clear();
    healDonor_.shrink_to_fit();
    healDonorW_ = 0;
    healDonorH_ = 0;
    healDonorSample_ = -1;
    healDonorPattern_ = -1;
}

bool AppState::redEyeAt(const QRectF& boxDoc, double darken01) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel) {
        setStatusHint(tr("Red Eye: select a pixel layer first."));
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;
    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int x0 = std::max(
        0, int(std::floor((boxDoc.left() - layer->offset.x()) / lsx)));
    const int y0 = std::max(
        0, int(std::floor((boxDoc.top() - layer->offset.y()) / lsy)));
    const int x1 = std::min(
        int(lw),
        int(std::ceil((boxDoc.right() - layer->offset.x()) / lsx)));
    const int y1 = std::min(
        int(lh),
        int(std::ceil((boxDoc.bottom() - layer->offset.y()) / lsy)));
    if (x1 <= x0 || y1 <= y0) return false;
    beginUndoStep();
    copyOnWriteActiveLayer();
    // Re-resolve after COW (see paintDab).
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        discardUndoStep();
        return false;
    }
    int bbox[4] = {0, 0, 0, 0};
    const bool changed = pittore::compute::redeye_fix_host(
        layer->pixels->data(), lw, lh, x0, y0, x1, y1,
        static_cast<float>(std::clamp(darken01, 0.0, 1.0)), bbox);
    if (!changed) {
        discardUndoStep();
        setStatusHint(tr("Red Eye: no red pupil found here."));
        return false;
    }
    commitUndoStep(tr("Red Eye"), QStringLiteral("red-eye"));
    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    refreshLayerCmyk(*d, *layer,
                     QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                           bbox[3] - bbox[1]));
    layer->thumbnail = QImage();
    d->rebuildComposite();
    return true;
}

}  // namespace pittore::ui
