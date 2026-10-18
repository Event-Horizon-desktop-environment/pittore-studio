// Layer thumbnails, bounds and picking helpers.
//
// Split out of app_state.cpp: pure DocumentItem/LayerItem queries plus the
// panel preview painters, so tools and panels link them without the rest of
// the state.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/adjust.h"
#include "engine/core/log.h"

#include <QColor>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace pittore::ui {

// Declared in app_state.h; defined here (outside the anonymous namespace
// above) so the Layers panel can link against them.
QRect layerMaskThumbRect(const LayerItem& layer) {
    const int x = 26 + layer.indent * 12 +
                  (layer.kind == LayerItem::Kind::Group ? 14 : 0) + 32 + 6;
    return QRect(x, 4, 32, 32);
}

QImage layerMaskThumbnail(const LayerItem& l, int box) {    if (box <= 0 || !l.hasMask || !l.mask) return QImage();
    const std::uint32_t mw = l.mask->width();
    const std::uint32_t mh = l.mask->height();
    if (mw == 0 || mh == 0) return QImage();
    QImage thumb(box, box, QImage::Format_ARGB32_Premultiplied);
    for (int ty = 0; ty < box; ++ty) {
        auto* row = reinterpret_cast<QRgb*>(thumb.scanLine(ty));
        const std::uint32_t sy =
            std::min(mh - 1, static_cast<std::uint32_t>(
                                 (static_cast<std::uint64_t>(ty) * mh) /
                                 static_cast<std::uint64_t>(box)));
        for (int tx = 0; tx < box; ++tx) {
            const std::uint32_t sx = std::min(
                mw - 1, static_cast<std::uint32_t>(
                            (static_cast<std::uint64_t>(tx) * mw) /
                            static_cast<std::uint64_t>(box)));
            const int v = qBound(
                0,
                static_cast<int>(std::lround(
                    std::clamp(l.mask->at(sx, sy).r, 0.0f, 1.0f) * 255.0f)),
                255);
            row[tx] = qRgba(v, v, v, 255);
        }
    }
    if (!l.maskEnabled) {
        QPainter p(&thumb);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(Qt::red, 2));
        p.drawLine(2, box - 3, box - 3, 2);
    }
    return thumb;
}

namespace {

// Straight-alpha box average of the source footprint one preview pixel covers.
// The tap grid is capped at kThumbTaps² in each axis, so building a preview
// costs O(box²) no matter how large the layer's native pixels are — cheap
// enough to refresh on every frame of a move/resize drag. All coordinates are
// in the preview's (thumbnail) space; `sx`/`sy` are already scaled by the
// document→thumbnail factor, so the downscale branch actually box-averages an
// identity layer instead of point-sampling it.
// A layer's native pixels in either state: realised as RGBAf in `pixels`, or
// packed away as RGBA8 in LayerItem::deferredRgba8 (a layer imported hidden
// behind a flattened base). Both views answer with straight RGBAf so one
// sampler box-averages a packed layer exactly like a realised one — that is
// what keeps a panel full of hidden layers from realising gigabytes just to
// paint 40-pixel previews.
struct RealizedView {
    const pittore::RGBAf* px;
    bool valid() const { return px != nullptr; }
    pittore::RGBAf operator[](std::size_t i) const { return px[i]; }
};
struct StashedView {
    const uchar* px;
    bool valid() const { return px != nullptr; }
    pittore::RGBAf operator[](std::size_t i) const {
        const uchar* s = px + (i << 2);
        return pittore::RGBAf{s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f,
                              s[3] / 255.0f};
    }
};

template <class View>
pittore::RGBAf thumbSample(const View& src, int sw, int sh,
                            double px, double py, double ox, double oy,
                            double sx, double sy) {
    constexpr int kThumbTaps = 6;
    if (!src.valid() || sw <= 0 || sh <= 0 || sx <= 0.0 || sy <= 0.0)
        return pittore::RGBAf{0, 0, 0, 0};
    const double fx = (px - ox) / sx;
    const double fy = (py - oy) / sy;
    const double spanX = std::max(1.0, 1.0 / sx);
    const double spanY = std::max(1.0, 1.0 / sy);
    const int nx = qBound(1, static_cast<int>(std::ceil(spanX)), kThumbTaps);
    const int ny = qBound(1, static_cast<int>(std::ceil(spanY)), kThumbTaps);
    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    for (int j = 0; j < ny; ++j) {
        const double sy2 = fy - spanY * 0.5 + spanY * (j + 0.5) / ny;
        const int iy = static_cast<int>(std::floor(sy2));
        if (iy < 0 || iy >= sh) continue;
        for (int i = 0; i < nx; ++i) {
            const double sx2 = fx - spanX * 0.5 + spanX * (i + 0.5) / nx;
            const int ix = static_cast<int>(std::floor(sx2));
            if (ix < 0 || ix >= sw) continue;
            const pittore::RGBAf s = src[static_cast<std::size_t>(iy) * sw + ix];
            r += s.r * s.a;
            g += s.g * s.a;
            b += s.b * s.a;
            a += s.a;
        }
    }
    // `a` is both the premultiply weight sum and the alpha sum, so dividing the
    // colour by it returns straight alpha; dividing alpha by the full tap count
    // gives partial coverage at the layer's edges (free antialiasing).
    if (a <= 0.0) return pittore::RGBAf{0, 0, 0, 0};
    const double taps = static_cast<double>(nx) * ny;
    return pittore::RGBAf{static_cast<float>(r / a), static_cast<float>(g / a),
                           static_cast<float>(b / a),
                           static_cast<float>(a / taps)};
}

}  // namespace

QRectF stageBounds(const DocumentItem& d, const LayerItem& l) {
    QRectF b = layerBounds(d, l);
    // An unlinked mask can extend past the pixels, so the staging footprint
    // is the union of both. Linked masks share the layer transform already.
    b |= maskBoundsFor(d, l);
    if (layerIsIdentity(d, l) && maskBoundsFor(d, l).isEmpty()) return b;
    const LayerDrawSource s = layerDrawSource(l);
    QPointF maskOffset{0, 0};
    double maskScaleX = 1.0, maskScaleY = 1.0;
    maskTransformFor(l, maskOffset, maskScaleX, maskScaleY);
    const double halo =
        std::ceil(std::max({s.scaleX, s.scaleY, maskScaleX, maskScaleY})) + 1.0;
    return b.adjusted(-halo, -halo, halo, halo);
}

QRectF layerBounds(const DocumentItem& d, const LayerItem& l) {
    const LayerDrawSource s = layerDrawSource(l);
    if (!s.img)
        return QRectF(QPointF(0, 0), QSizeF(d.size));
    return QRectF(s.offset,
                  QSizeF(s.img->width() * s.scaleX,
                         s.img->height() * s.scaleY));
}

bool layerIsIdentity(const DocumentItem& d, const LayerItem& l) {
    const LayerDrawSource s = layerDrawSource(l);
    return s.img &&
           s.img->width() == static_cast<std::uint32_t>(d.size.width()) &&
           s.img->height() == static_cast<std::uint32_t>(d.size.height()) &&
           s.offset == QPointF(0, 0) && s.scaleX == 1.0 && s.scaleY == 1.0;
}

QImage layerThumbnail(const DocumentItem& d, const LayerItem& l, int box, bool contain) {
    // A layer whose pixels are stashed behind the flattened base previews
    // straight from the packed bytes: realising it first would cost 16 B/px
    // for every hidden row the panel draws. `pixels` wins when both exist.
    const bool stashed = hasDeferredPixels(l);
    if (box <= 0 || (!l.pixels && !stashed) || d.size.isEmpty()) return QImage();
    if (!l.thumbnail.isNull() && l.thumbnail.width() == box) return l.thumbnail;

    // Fit the layer's own content extent to the square box. Regular layers
    // COVER it (uniform scale, overflowing edges cropped) so the preview fills
    // the square; text layers CONTAIN (letterbox) so the whole run stays legible
    // instead of zooming into a cropped fragment. An identity layer's extent is
    // the whole canvas. The preview shows the layer's own pixels, not its style
    // (the fx badge marks those).
    const std::uint32_t nw = l.pixels ? l.pixels->width() : l.deferredWidth;
    const std::uint32_t nh = l.pixels ? l.pixels->height() : l.deferredHeight;
    const QRectF b(l.offset, QSizeF(double(nw) * l.scaleX, double(nh) * l.scaleY));
    if (b.width() <= 0.0 || b.height() <= 0.0) return QImage();
    const double k = contain ? std::min(box / b.width(), box / b.height())
                             : std::max(box / b.width(), box / b.height());
    const double ox = (box - b.width() * k) * 0.5 - b.x() * k;
    const double oy = (box - b.height() * k) * 0.5 - b.y() * k;

    QImage thumb(box, box, QImage::Format_ARGB32_Premultiplied);
    thumb.fill(Qt::transparent);
    const int sw = static_cast<int>(nw);
    const int sh = static_cast<int>(nh);
    const RealizedView realized{l.pixels ? l.pixels->data() : nullptr};
    const StashedView packed{
        stashed ? reinterpret_cast<const uchar*>(l.deferredRgba8.constData())
                : nullptr};
    // The layer's placement mapped into thumbnail space: a source pixel lives
    // at offset*k + o and spans scale*k thumbnail pixels.
    const double sox = l.offset.x() * k + ox;
    const double soy = l.offset.y() * k + oy;
    const double ssx = l.scaleX * k;
    const double ssy = l.scaleY * k;
    for (int ty = 0; ty < box; ++ty) {
        auto* row = reinterpret_cast<QRgb*>(thumb.scanLine(ty));
        for (int tx = 0; tx < box; ++tx) {
            const pittore::RGBAf c =
                l.pixels
                    ? thumbSample(realized, sw, sh, tx + 0.5, ty + 0.5, sox, soy,
                                  ssx, ssy)
                    : thumbSample(packed, sw, sh, tx + 0.5, ty + 0.5, sox, soy,
                                  ssx, ssy);
            const int a = qBound(0, static_cast<int>(std::lround(c.a * 255.0f)), 255);
            const int r = qBound(0, static_cast<int>(std::lround(c.r * c.a * 255.0f)), 255);
            const int g = qBound(0, static_cast<int>(std::lround(c.g * c.a * 255.0f)), 255);
            const int b2 = qBound(0, static_cast<int>(std::lround(c.b * c.a * 255.0f)), 255);
            row[tx] = qRgba(r, g, b2, a);
        }
    }
    l.thumbnail = thumb;   // mutable lazy cache; model clears it on content change
    return thumb;
}

// A cheap content hash for a + part of a group's thumbnail identity.
static inline void mixStamp(std::uint64_t& h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

// Quantise a doc-space coordinate for the hash: two values agreeing to within a
// hundredth of a document pixel are the same placement for a preview, so this
// keeps the stamp stable across edit jitter without letting a real move slip by.
static inline std::uint64_t stampCoord(double v) {
    return static_cast<std::uint64_t>(qRound64(v * 100.0));
}

QImage groupThumbnail(const DocumentItem& d, int groupIndex, int box) {
    if (box <= 0 || d.size.isEmpty() || groupIndex < 0 ||
        groupIndex >= d.layers.size() ||
        d.layers[groupIndex].kind != LayerItem::Kind::Group)
        return QImage();

    // Content stamp: every pixel descendant's content + placement + opacity
    // and fill, plus every live adjustment's kind + parameters, keyed in
    // panel index space (no re-sort needed), plus the visibility of each
    // enclosing group. O(descendants) scan per call — the box is re-sampled
    // only when a real descendant change lands.
    std::uint64_t stamp = 14695981039346656037ull;   // FNV-1a offset basis
    // Seed the hash with the group's OWN identity. Without this the stamp is a
    // pure content hash, so two sibling groups whose members happen to agree on
    // content+placement hash identically and one returns the other's cached
    // `groupThumbnailCache` — exactly the two-artboards-show-the-same-image
    // bug. `groupIndex` is the row's position in this document's layer vector,
    // unique per row and stable between paints, so it separates every group. A
    // board's geometry and fill are mixed in too: they drive the render but live
    // on the board row, not on any descendant, so resizing or recolouring a
    // board with unchanged children must still invalidate its preview.
    const LayerItem& self = d.layers[groupIndex];
    mixStamp(stamp, static_cast<std::uint64_t>(groupIndex));
    if (self.isBoard()) {
        mixStamp(stamp, stampCoord(self.boardRect.x()));
        mixStamp(stamp, stampCoord(self.boardRect.y()));
        mixStamp(stamp, stampCoord(self.boardRect.width()));
        mixStamp(stamp, stampCoord(self.boardRect.height()));
        mixStamp(stamp, self.boardHasFill ? 1u : 0u);
        if (self.boardHasFill) {
            mixStamp(stamp, static_cast<std::uint64_t>(self.boardFill.alpha()));
            mixStamp(stamp, stampCoord(self.boardFillOpacity));
        }
    }
    QVector<int> pix;   // pixel descendants with pixels, top-first (bounds)
    QVector<int> ops;   // pixel + live-adjustment rows, top-first (paint)
    {
        const int base = d.layers[groupIndex].indent;
        for (int i = groupIndex + 1; i < d.layers.size(); ++i) {
            const LayerItem& l = d.layers[i];
            if (l.indent <= base) break;      // end of the group's subtree
            const bool isPix = l.kind == LayerItem::Kind::Pixel &&
                                 (l.pixels || hasDeferredPixels(l));
            const bool isAdj =
                l.kind == LayerItem::Kind::Adjustment &&
                l.adjustmentKind != 0;
            if (!isPix && !isAdj) continue;
            ops.append(i);
            mixStamp(stamp, static_cast<std::uint64_t>(i));
            mixStamp(stamp, l.visible ? 1u : 0u);
            mixStamp(stamp, static_cast<std::uint64_t>(l.opacity));
            mixStamp(stamp, static_cast<std::uint64_t>(l.fill));
            if (isPix) {
                pix.append(i);
                if (l.pixels) {
                    mixStamp(stamp, l.sourceStamp);
                } else {
                    // Stashed members count exactly like realised ones: the
                    // stash identity (size) changes if and only if the bytes
                    // do, so the preview still resamples only on real change.
                    mixStamp(stamp, static_cast<std::uint64_t>(l.deferredWidth));
                    mixStamp(stamp, static_cast<std::uint64_t>(l.deferredHeight));
                    mixStamp(stamp, static_cast<std::uint64_t>(l.deferredRgba8.size()));
                }
                mixStamp(stamp,
                         static_cast<std::uint64_t>(qRound64(l.offset.x() * 100.0)));
                mixStamp(stamp,
                         static_cast<std::uint64_t>(qRound64(l.offset.y() * 100.0)));
                mixStamp(stamp,
                         static_cast<std::uint64_t>(qRound64(l.scaleX * 100.0)));
                mixStamp(stamp,
                         static_cast<std::uint64_t>(qRound64(l.scaleY * 100.0)));
            } else {
                mixStamp(stamp, static_cast<std::uint64_t>(l.adjustmentKind));
                mixStamp(stamp, l.adjustStamp);
                for (int k = 0; k < 16; ++k) {
                    std::uint32_t u = 0;
                    std::memcpy(&u, &l.adjustmentParams[k],
                                sizeof(std::uint32_t));
                    mixStamp(stamp, u);
                }
            }
        }
    }
    for (int g : d.enclosingGroups(groupIndex))
        mixStamp(stamp, d.layers[g].visible ? 1u : 0u);
    // The group's own eye hides the whole subtree from the preview too.
    mixStamp(stamp, d.layers[groupIndex].visible ? 1u : 0u);

    if (pix.isEmpty()) return QImage();
    // Combined previews stop scaling: past a few thousand descendants the
    // box resamples oversampling noise at O(box² × ops) CPU (tens of
    // millions of samples for an 80k-leaf import), so huge groups keep the
    // folder glyph exactly like empty ones do.
    constexpr int kMaxGroupThumbOps = 2048;
    if (ops.size() > kMaxGroupThumbOps) return QImage();
    const LayerItem& grp = d.layers[groupIndex];
    if (grp.groupThumbnailStamp == stamp && !grp.groupThumbnailCache.isNull() &&
        grp.groupThumbnailCache.width() == box)
        return grp.groupThumbnailCache;

    // Fit the union of the descendants' doc-space bounds INSIDE the square
    // (contain, letterboxed): the preview shows the group's whole combined
    // content — every member — the way the Layers panel reads. Cover would
    // crop wide groups down to a zoomed slice.
    QRectF b;
    for (int idx : pix) {
        const LayerItem& c = d.layers[idx];
        const double cw = double(c.pixels ? c.pixels->width() : c.deferredWidth);
        const double ch =
            double(c.pixels ? c.pixels->height() : c.deferredHeight);
        b |= QRectF(c.offset, QSizeF(cw * c.scaleX, ch * c.scaleY));
    }
    if (b.width() <= 0.0 || b.height() <= 0.0) return QImage();
    const double k = std::min(box / b.width(), box / b.height());
    const double ox = (box - b.width() * k) * 0.5 - b.x() * k;
    const double oy = (box - b.height() * k) * 0.5 - b.y() * k;

    // Composite bottom→top with premultiplied source-over. `ops` is top-first,
    // so iterate reversed: the bottom-most row paints first. Pixel layers are
    // sampled through their own placement (thumbSample), folded by
    // opacity×fill, then blended over the accumulator; live adjustments
    // transform the accumulated colour in place (Normal blend at their own
    // opacity×fill — the same blend-mode simplification the pixel path
    // already makes here, and masks likewise ignored).
    bool painted = false;
    const std::size_t npx = static_cast<std::size_t>(box) * box;
    std::vector<std::array<float, 4>> acc(npx, {0.0f, 0.0f, 0.0f, 0.0f});
    for (int n = ops.size() - 1; n >= 0; --n) {
        const int idx = ops[n];
        const LayerItem& c = d.layers[idx];
        if (!d.effectivelyVisible(idx)) continue;
        const float alpha = (c.opacity / 100.0f) * (c.fill / 100.0f);
        if (alpha <= 0.0f) continue;
        if (c.kind == LayerItem::Kind::Adjustment) {
            const float* lut =
                c.adjustmentLUT.size() == adjustmentAuxSize(c) &&
                        adjustmentAuxSize(c) != 0
                    ? c.adjustmentLUT.data()
                    : nullptr;
            const auto kind = static_cast<
                pittore::compute::AdjustmentKind>(c.adjustmentKind);
            for (int ty = 0; ty < box; ++ty) {
                for (int tx = 0; tx < box; ++tx) {
                    std::array<float, 4>& dst =
                        acc[static_cast<std::size_t>(ty) * box + tx];
                    if (dst[3] <= 0.0f) continue;
                    painted = true;
                    const float inv = 1.0f / dst[3];
                    float ar, ag, ab;
                    pittore::compute::adjust::apply(
                        kind, c.adjustmentParams, lut, dst[0] * inv,
                        dst[1] * inv, dst[2] * inv, ar, ag, ab);
                    const float keep = 1.0f - alpha;
                    dst[0] = (ar * alpha + dst[0] * inv * keep) * dst[3];
                    dst[1] = (ag * alpha + dst[1] * inv * keep) * dst[3];
                    dst[2] = (ab * alpha + dst[2] * inv * keep) * dst[3];
                }
            }
            continue;
        }
        const int sw = static_cast<int>(c.pixels ? c.pixels->width() : c.deferredWidth);
        const int sh =
            static_cast<int>(c.pixels ? c.pixels->height() : c.deferredHeight);
        if (sw < 1 || sh < 1) continue;
        const RealizedView realized{c.pixels ? c.pixels->data() : nullptr};
        const StashedView packed{
            c.pixels ? nullptr
                     : reinterpret_cast<const uchar*>(c.deferredRgba8.constData())};
        const double sox = c.offset.x() * k + ox;
        const double soy = c.offset.y() * k + oy;
        const double ssx = c.scaleX * k;
        const double ssy = c.scaleY * k;
        // Tighten the pixel loop to the member's own footprint: a map glyph
        // covering one preview pixel must not pay a full-box scan. Same
        // samples in the same order, so the bytes are unchanged.
        const double fx0 = sox, fx1 = sox + sw * ssx;
        const double fy0 = soy, fy1 = soy + sh * ssy;
        int tx0 = static_cast<int>(std::floor(std::min(fx0, fx1) - 1.0));
        int tx1 = static_cast<int>(std::ceil(std::max(fx0, fx1) + 1.0));
        int ty0 = static_cast<int>(std::floor(std::min(fy0, fy1) - 1.0));
        int ty1 = static_cast<int>(std::ceil(std::max(fy0, fy1) + 1.0));
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 > box) tx1 = box;
        if (ty1 > box) ty1 = box;
        if (tx0 >= tx1 || ty0 >= ty1) continue;
        for (int ty = ty0; ty < ty1; ++ty) {
            for (int tx = tx0; tx < tx1; ++tx) {
                const pittore::RGBAf s =
                    c.pixels ? thumbSample(realized, sw, sh, tx + 0.5, ty + 0.5,
                                           sox, soy, ssx, ssy)
                             : thumbSample(packed, sw, sh, tx + 0.5, ty + 0.5,
                                           sox, soy, ssx, ssy);
                const float sa = s.a * alpha;
                if (sa <= 0.0f) continue;
                painted = true;
                std::array<float, 4>& dst =
                    acc[static_cast<std::size_t>(ty) * box + tx];
                const float inv = 1.0f - sa;
                dst[0] = s.r * sa + dst[0] * inv;
                dst[1] = s.g * sa + dst[1] * inv;
                dst[2] = s.b * sa + dst[2] * inv;
                dst[3] = sa + dst[3] * inv;
            }
        }
    }
    // Everything was invisible: report an empty group even though pixel rows
    // exist (the stamp still guards the cache).
    if (!painted) return QImage();

    QImage thumb(box, box, QImage::Format_ARGB32_Premultiplied);
    for (int ty = 0; ty < box; ++ty) {
        auto* row = reinterpret_cast<QRgb*>(thumb.scanLine(ty));
        for (int tx = 0; tx < box; ++tx) {
            const std::array<float, 4>& v =
                acc[static_cast<std::size_t>(ty) * box + tx];
            // premultiplied float colour → premultiplied bytes directly
            row[tx] = qRgba(
                qBound(0, static_cast<int>(std::lround(v[0] * 255.0f)), 255),
                qBound(0, static_cast<int>(std::lround(v[1] * 255.0f)), 255),
                qBound(0, static_cast<int>(std::lround(v[2] * 255.0f)), 255),
                qBound(0, static_cast<int>(std::lround(v[3] * 255.0f)), 255));
        }
    }
    d.layers[groupIndex].groupThumbnailCache = thumb;   // mutable lazy cache
    d.layers[groupIndex].groupThumbnailStamp = stamp;
    return thumb;
}

}  // namespace pittore::ui
