// Layered PSD export (Phase 4A): DocumentItem -> PsdLayersDoc.
//
// New work lives here, not bolted onto app_state.cpp: AppState::exportLayeredPsd
// is only *declared* in app_state.h and defined at the bottom of this file.
//
// Export rules (mirroring the importer in reverse):
// - Panel order (index 0 = top) becomes file order (bottom -> top); each
//   group's record precedes its children, as the decoder expects.
// - Group opacity/visibility are UNFOLDED on export: panel children carry
//   folded values (import multiplies ancestors in), so the exporter divides
//   the ancestors back out. Reimport refolds to the same panel values, up to
//   the importer's integer quantisation. A child hidden only because an
//   ancestor is hidden exports visible=true (reimport hides it again).
// - Pixel layers export their RAW native pixels (`pixels`, never the styled
//   raster): layer styles don't survive a PSD hop. Placement (offset/scale)
//   is baked by resampling into document space; fractional origins round.
// - Masks export as layer-local coverage grids (maskRelative, origin 0,0)
//   bilinearly resampled from the mask image. A disabled mask is skipped
//   (reveal baked in). There is no mask-density field on LayerItem, so there
//   is nothing else to bake.
// - Adjustments export at full-document rect with params/curve straight
//   across. Kind-0 stand-ins are skipped (the encoder drops them anyway).
// - Dropped without a PSD representation (documented): per-layer `fill`
//   (opacity carries the closest analogue), lockTransparency/locked, text
//   specs and vector art (text exports its rendered pixels), swatches,
//   thumbnails, slices/guides.
#include "ui/psd_export.h"

#include <QImage>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "ui/app_state.h"
#include "ui/color_mode.h"
#include "ui/live_filter.h"
#include "ui/mask_finish.h"
#include "ui/project_manager.h"

namespace pittore::ui {
namespace {

std::uint16_t floatToU16(float v) {
    const long t = std::lround(static_cast<double>(v) * 65535.0);
    return static_cast<std::uint16_t>(std::clamp(t, 0L, 65535L));
}

// Document-space footprint of raw native pixels, canvas-clipped. Empty when
// the layer has no pixels or sits fully off-canvas.
QRect pixelFootprint(const LayerItem& l, const pittore::Image* px, const QSize& canvas) {
    if (!px || px->width() == 0 || px->height() == 0)
        return {};
    const double fw = static_cast<double>(px->width()) * l.scaleX;
    const double fh = static_cast<double>(px->height()) * l.scaleY;
    if (!(fw > 0.0) || !(fh > 0.0)) return {};
    const QRectF fp(l.offset, QSizeF(fw, fh));
    return fp.intersected(QRectF(QPointF(0, 0), QSizeF(canvas))).toAlignedRect();
}

// Straight RGBA16 samples for `rect` (document space) from raw native pixels.
// Scale 1:1 crops floats directly; anything else goes through a QImage
// smooth-resample of the whole footprint first.
std::vector<std::uint16_t> rasterizePixels(const LayerItem& l, const pittore::Image* px, const QRect& rect) {
    std::vector<std::uint16_t> out;
    if (rect.isEmpty() || !px) return out;
    const std::uint32_t nw = px->width(), nh = px->height();
    const int ox = qRound(l.offset.x()), oy = qRound(l.offset.y());
    if (l.scaleX == 1.0 && l.scaleY == 1.0) {
        out.resize(static_cast<std::size_t>(rect.width()) * rect.height() * 4);
        const pittore::RGBAf* src = px->data();
        std::size_t k = 0;
        for (int dy = 0; dy < rect.height(); ++dy) {
            const int sy = rect.top() + dy - oy;
            for (int dx = 0; dx < rect.width(); ++dx) {
                const int sx = rect.left() + dx - ox;
                float r = 0, g = 0, b = 0, a = 0;
                if (sx >= 0 && sy >= 0 &&
                    static_cast<std::uint32_t>(sx) < nw &&
                    static_cast<std::uint32_t>(sy) < nh) {
                    const pittore::RGBAf& p =
                        src[static_cast<std::size_t>(sy) * nw + sx];
                    r = p.r;
                    g = p.g;
                    b = p.b;
                    a = p.a;
                }
                out[k++] = floatToU16(r);
                out[k++] = floatToU16(g);
                out[k++] = floatToU16(b);
                out[k++] = floatToU16(a);
            }
        }
        return out;
    }
    QImage native = imageToStraightRgba64(*px);
    if (native.isNull()) return {};
    const int fw =
        std::max(1, qRound(static_cast<double>(nw) * l.scaleX));
    const int fh =
        std::max(1, qRound(static_cast<double>(nh) * l.scaleY));
    const QImage scaled =
        native.scaled(fw, fh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.isNull()) return {};
    QImage tile(rect.width(), rect.height(), QImage::Format_RGBA64);
    tile.fill(0);
    // Copy the canvas-visible part of the resampled footprint into the tile.
    // (ox, oy) is the footprint origin in document space; anything outside
    // the resampled image stays transparent.
    for (int dy = 0; dy < rect.height(); ++dy) {
        const int sy = rect.top() + dy - oy;
        if (sy < 0 || sy >= scaled.height()) continue;
        auto* dst =
            reinterpret_cast<quint16*>(tile.scanLine(dy));
        const auto* srcRow = reinterpret_cast<const quint16*>(
            scaled.constScanLine(sy));
        for (int dx = 0; dx < rect.width(); ++dx) {
            const int sx = rect.left() + dx - ox;
            if (sx < 0 || sx >= scaled.width()) continue;
            dst[dx * 4 + 0] = srcRow[sx * 4 + 0];
            dst[dx * 4 + 1] = srcRow[sx * 4 + 1];
            dst[dx * 4 + 2] = srcRow[sx * 4 + 2];
            dst[dx * 4 + 3] = srcRow[sx * 4 + 3];
        }
    }
    return rgba16FromQImage(tile);
}

// Coverage grid over `rect` (document space) from the layer's mask image via
// its document mapping, with density/feather baked (PSD has no record for
// either). Empty when there is no enabled mask.
std::vector<std::uint16_t> rasterizeMask(const LayerItem& l, const QRect& rect) {
    std::vector<std::uint16_t> out;
    if (rect.isEmpty() || !l.hasMask || !l.mask || !l.maskEnabled) return out;
    if (l.mask->width() == 0 || l.mask->height() == 0) return out;
    if (!(l.maskScaleX > 0.0) || !(l.maskScaleY > 0.0)) return out;
    const pittore::Image* m = l.mask.get();
    std::shared_ptr<pittore::Image> finished;
    if (l.maskFeather > 0.0f || l.maskDensity != 1.0f) {
        finished = finishMaskImage(*m, l.maskDensity, l.maskFeather);
        m = finished.get();
    }
    out.resize(static_cast<std::size_t>(rect.width()) * rect.height());
    std::size_t k = 0;
    for (int dy = 0; dy < rect.height(); ++dy) {
        for (int dx = 0; dx < rect.width(); ++dx) {
            const double docX = rect.left() + dx;
            const double docY = rect.top() + dy;
            const double mx = (docX - l.maskOffset.x()) / l.maskScaleX;
            const double my = (docY - l.maskOffset.y()) / l.maskScaleY;
            out[k++] = floatToU16(sampleCoverageBilinear(*m, mx, my));
        }
    }
    return out;
}

// Verbatim foreign blocks (TySh, SoLd, lfx2, vmsk, ...) survive export
// only while the layer is untouched since import: kind, pixel/param/mask
// stamps and placement must all match. Anything else means the descriptors
// would describe pixels that no longer exist, so they are dropped and the
// layer exports as plain pixels. Shared with IFP save
// (projectDataFromDocument keeps only fresh blocks).
void carryRawBlocks(pittore::io::PsdLayerFile& f, const LayerItem& l) {
    if (!psdRawBlocksFresh(l)) return;
    f.rawBlocks.reserve(l.psdRawBlocks.size());
    for (const auto& rb : l.psdRawBlocks) {
        pittore::io::PsdLayerFile::RawBlock o;
        std::memcpy(o.sig, rb.sig, 4);
        std::memcpy(o.key, rb.key, 4);
        o.data = rb.data;
        o.padding = rb.padding;
        f.rawBlocks.push_back(std::move(o));
    }
}

void fillCommon(pittore::io::PsdLayerFile& f, const LayerItem& l, int indent,
                int unfoldedOpacity, bool unfoldedVisible) {    f.name = l.name.toStdString();
    f.blend = l.blendMode.toStdString();
    if (f.blend.empty()) f.blend = "Normal";
    f.opacity = static_cast<std::uint16_t>(
        qBound(0, qRound(unfoldedOpacity * 255.0 / 100.0), 255));
    f.visible = unfoldedVisible;
    f.clipped = l.clipped;
    f.indent = indent;
}

pittore::io::PsdLayerFile exportPixel(const LayerItem& l, int indent,
                                       int unfoldedOpacity,
                                       bool unfoldedVisible,
                                       const QSize& canvas,
                                       std::uint64_t profileFingerprint) {
    pittore::io::PsdLayerFile f;
    fillCommon(f, l, indent, unfoldedOpacity, unfoldedVisible);
    // Live filters bake: PSD has no live-filter record, so the
    // filtered render goes out as plain pixels (native pixels stay live
    // in memory and in .psc).
    ensureLayerFilter(l);
    const pittore::Image* base = liveFilterBase(l);
    const QRect rect = pixelFootprint(l, base, canvas);
    if (rect.isEmpty()) return f;  // zero-rect record, like the codec's own
    f.left = static_cast<std::uint32_t>(rect.left());
    f.top = static_cast<std::uint32_t>(rect.top());
    f.width = static_cast<std::uint32_t>(rect.width());
    f.height = static_cast<std::uint32_t>(rect.height());
    f.rgba = rasterizePixels(l, base, rect);
    if (f.rgba.empty()) {
        f.width = f.height = 0;
        return f;
    }
    if (base == l.pixels.get() && l.cmykPixels &&
        l.cmykPixels->valid() &&
        l.cmykPixels->width == l.pixels->width() &&
        l.cmykPixels->height == l.pixels->height() &&
        l.cmykPixels->appearanceFingerprint ==
            pittore::color::CmykImage::fingerprint(*l.pixels) &&
        l.cmykPixels->profileFingerprint == profileFingerprint &&
        l.scaleX == 1.0 &&
        l.scaleY == 1.0) {
        const int ox = qRound(l.offset.x());
        const int oy = qRound(l.offset.y());
        const std::uint32_t nw = l.pixels->width();
        const std::uint32_t nh = l.pixels->height();
        f.cmyk.resize(static_cast<std::size_t>(f.width) * f.height * 4);
        for (std::uint32_t y = 0; y < f.height; ++y) {
            const int sy = rect.top() + static_cast<int>(y) - oy;
            for (std::uint32_t x = 0; x < f.width; ++x) {
                const int sx = rect.left() + static_cast<int>(x) - ox;
                if (sx < 0 || sy < 0 || static_cast<std::uint32_t>(sx) >= nw ||
                    static_cast<std::uint32_t>(sy) >= nh)
                    continue;
                const auto& sample = l.cmykPixels->pixels[
                    static_cast<std::size_t>(sy) * nw + sx];
                const std::size_t dst =
                    (static_cast<std::size_t>(y) * f.width + x) * 4;
                std::copy(sample.begin(), sample.end(), f.cmyk.begin() + dst);
            }
        }
    }
    auto mask = rasterizeMask(l, rect);
    if (!mask.empty()) {
        f.hasMask = true;
        f.maskEnabled = true;
        f.maskRelative = true;  // layer-local grid, origin 0,0
        f.maskLeft = 0;
        f.maskTop = 0;
        f.maskWidth = f.width;
        f.maskHeight = f.height;
        f.mask = std::move(mask);
    }
    carryRawBlocks(f, l);
    // Re-encoded pixels are always current, so the origin method needs no
    // freshness guard: keep it (no churn, no size surprise on resave).
    f.preferZip = (l.psdChannelMethod == 2);
    return f;
}

pittore::io::PsdLayerFile exportAdjustment(const LayerItem& l, int indent,
                                            int unfoldedOpacity,
                                            bool unfoldedVisible,
                                            const QSize& canvas) {
    pittore::io::PsdLayerFile f;
    fillCommon(f, l, indent, unfoldedOpacity, unfoldedVisible);
    f.isAdjustment = true;
    f.adjustmentKind = l.adjustmentKind;
    for (int k = 0; k < 16; ++k) f.adjustmentParams[k] = l.adjustmentParams[k];
    f.adjustmentCurve.reserve(
        static_cast<std::size_t>(l.adjustmentCurve.size()));
    for (const QPointF& p : l.adjustmentCurve)
        f.adjustmentCurve.emplace_back(p.x(), p.y());
    for (const QPointF& p : l.adjustmentCurveR)
        f.adjustmentCurveR.emplace_back(p.x(), p.y());
    for (const QPointF& p : l.adjustmentCurveG)
        f.adjustmentCurveG.emplace_back(p.x(), p.y());
    for (const QPointF& p : l.adjustmentCurveB)
        f.adjustmentCurveB.emplace_back(p.x(), p.y());
    // Null bounds, like the format's own fill/adjustment records: a 0-channel
    // record with a non-empty rect is rejected by third-party readers
    // (verified with ImageMagick). The mask descriptor carries the extent;
    // the flatten applies adjustments full-canvas regardless of the rect.
    f.left = 0;
    f.top = 0;
    f.width = 0;
    f.height = 0;
    const QRect full(QPoint(0, 0), canvas);
    auto mask = rasterizeMask(l, full);
    if (!mask.empty()) {
        f.hasMask = true;
        f.maskEnabled = true;
        f.maskRelative = true;
        f.maskLeft = 0;
        f.maskTop = 0;
        // Mask extent is canvas-sized even though the record rect is null.
        f.maskWidth = static_cast<std::uint32_t>(std::max(0, canvas.width()));
        f.maskHeight =
            static_cast<std::uint32_t>(std::max(0, canvas.height()));
        f.mask = std::move(mask);
    }
    carryRawBlocks(f, l);
    // Re-encoded pixels are always current, so the origin method needs no
    // freshness guard: keep it (no churn, no size surprise on resave).
    f.preferZip = (l.psdChannelMethod == 2);
    return f;
}

}  // namespace

pittore::io::PsdLayersDoc buildLayeredPsdDoc(DocumentItem& doc) {
    pittore::io::PsdLayersDoc out;
    out.width = static_cast<std::uint32_t>(std::max(0, doc.size.width()));
    out.height = static_cast<std::uint32_t>(std::max(0, doc.size.height()));
    out.depth = doc.colorMode.contains(QStringLiteral("16")) ? 16 : 8;
    const QByteArray resolvedProfile = resolveCmykProfile(doc);
    const std::uint64_t profileFingerprint =
        pittore::color::CmykImage::fingerprint(
            reinterpret_cast<const std::uint8_t*>(resolvedProfile.constData()),
            static_cast<std::size_t>(resolvedProfile.size()));
    // A CMYK-tagged document writes mode 4: layer planes and the composite
    // separate at write time, and the destination profile (the document's
    // own bytes, settings, or a system probe — see ui/color_mode.h) is
    // embedded so the round trip separates identically.
    if (doc.colorMode.startsWith(QStringLiteral("CMYK"))) {
        out.colorMode = 4;
        const QByteArray icc = resolveCmykProfile(doc);
        out.icc.assign(icc.cbegin(), icc.cend());
    }

    // Flattened-tree parse of the panel (top -> bottom): a row opens a group
    // for deeper rows until an equal-or-shallower indent closes it. Indent
    // jumps deeper than one level are clamped onto the open group.
    struct Node {
        int panelIdx = -1;
        int indent = 0;
        QVector<int> children;  // node indices, panel order (top -> bottom)
    };
    QVector<Node> nodes;
    QVector<int> roots;
    QVector<int> open;  // node stack
    for (int i = 0; i < doc.layers.size(); ++i) {
        const LayerItem& l = doc.layers[i];
        while (!open.isEmpty() && nodes[open.back()].indent >= l.indent)
            open.pop_back();
        Node n;
        n.panelIdx = i;
        n.indent = open.isEmpty()
                       ? 0
                       : std::min(l.indent, nodes[open.back()].indent + 1);
        const int idx = nodes.size();
        nodes.append(n);
        if (!open.isEmpty())
            nodes[open.back()].children.append(idx);
        else
            roots.append(idx);
        if (l.kind == LayerItem::Kind::Group) open.append(idx);
    }

    // File order (bottom -> top): siblings reversed, each group record before
    // its children. Unfolding divides the ancestors back out of every leaf.
    std::function<void(const QVector<int>&, double, bool)> emitSiblings =
        [&](const QVector<int>& ids, double ancestorProduct,
            bool ancestorHidden) {
            for (int k = ids.size() - 1; k >= 0; --k) {
                const Node& n = nodes[ids[k]];
                const LayerItem& l = doc.layers[n.panelIdx];
                if (l.kind == LayerItem::Kind::Group) {
                    pittore::io::PsdLayerFile f;
                    fillCommon(f, l, n.indent, l.opacity, l.visible);
                    f.isGroup = true;
                    f.groupExpanded = l.groupExpanded;
                    out.layers.push_back(std::move(f));
                    emitSiblings(n.children,
                                 ancestorProduct * (l.opacity / 100.0),
                                 ancestorHidden || !l.visible);
                    continue;
                }
                const int unfoldedOpacity =
                    ancestorProduct > 0.0
                        ? qBound(1, qRound(l.opacity / ancestorProduct), 100)
                        : l.opacity;
                const bool unfoldedVisible = l.visible || ancestorHidden;
                if (l.kind == LayerItem::Kind::Adjustment) {
                    if (l.adjustmentKind == 0) continue;  // stand-in: no PSD form
                    out.layers.push_back(exportAdjustment(
                        l, n.indent, unfoldedOpacity, unfoldedVisible,
                        doc.size));
                } else {
                    out.layers.push_back(exportPixel(l, n.indent,
                                                     unfoldedOpacity,
                                                     unfoldedVisible, doc.size,
                                                     profileFingerprint));
                }
            }
        };
    emitSiblings(roots, 1.0, false);
    return out;
}

std::optional<std::vector<std::uint8_t>> AppState::exportLayeredPsd(
    DocumentItem& doc, QString* error) const {
    if (doc.size.isEmpty()) {
        if (error) *error = tr("Nothing to export (empty canvas).");
        return std::nullopt;
    }
    // A layer packed behind the flattened base expands for the read and packs
    // back afterwards: the encoder needs real pixels, but exporting must not
    // be what leaves a document inflated at 16 B/px for good.
    const QVector<int> expanded = materializeDeferredPixels(doc);
    pittore::io::PsdLayersDoc layers = buildLayeredPsdDoc(doc);
    stashDeferredPixels(doc, expanded);
    if (layers.layers.empty()) {
        if (error) *error = tr("Nothing to export (no layers).");
        return std::nullopt;
    }
    std::string codecError;
    const auto comp = settings_.psdCompression == 1
                          ? pittore::io::PsdLayerCompression::Zip
                          : pittore::io::PsdLayerCompression::Rle;
    auto bytes = pittore::io::psdEncodeLayers(layers, &codecError, comp);
    if (!bytes && error)
        *error = codecError.empty()
                     ? tr("Could not encode PSD.")
                     : QString::fromStdString(codecError);
    return bytes;
}

}  // namespace pittore::ui
