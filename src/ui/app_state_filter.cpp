// Filter entry points, one-shot edits, image transforms, histogram.
//
// Split out of app_state.cpp; AppState members, no extra declarations needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"

#include "engine/compute/backend.h"
#include "engine/core/tonal_ops.h"
#include "engine/filter/filters.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
namespace pittore::ui {

namespace {

bool filterGpuRouted(const std::string& id) {
    return id == "gaussian_blur" || id == "box_blur" || id == "blur" ||
           id == "blur_more" || id == "median" || id == "despeckle" ||
           id == "unsharp_mask" || id == "sharpen" || id == "sharpen_more" ||
           id == "motion_blur" || id == "lens_blur";
}

double filterStrengthParam(const std::string& id,
                           const std::vector<double>& par) {
    if (id == "despeckle" || id == "sharpen" || id == "sharpen_more") {
        if (par.empty()) return 1.0;
        const double v = par[0] / 100.0;
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }
    return 1.0;
}

bool runFilterGpu(pittore::compute::ComputeBackend& be, pittore::Image& img,
                  const std::string& id, const std::vector<double>& par) {
    const std::uint32_t w = img.width();
    const std::uint32_t h = img.height();
    const std::size_t bytes = static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    auto pv = [&](std::size_t i, double d) {
        return i < par.size() ? par[i] : d;
    };
    auto src = be.make_buffer(bytes);
    auto dst = be.make_buffer(bytes);
    std::memcpy(src->host(), img.data(), bytes);
    src->upload();
    const auto rounded = [&](double v) { return std::max(1, static_cast<int>(std::round(v))); };
    if (id == "gaussian_blur") {
        const double sigma = pv(0, 4.0);
        if (sigma <= 0.05) return true;
        const int r = rounded(sigma);
        auto tmp = be.make_buffer(bytes);
        // Resident chain: same 3x-box math as the CPU gaussBlur path, but one
        // upload + one download instead of three roundtrips + three allocs.
        // (box_blur_resident defaults to box_blur on backends without a
        // device core, so this stays correct everywhere.)
        be.box_blur_resident(*src, *tmp, w, h, r);
        be.box_blur_resident(*tmp, *dst, w, h, r);
        be.box_blur_resident(*dst, *tmp, w, h, r);
        tmp->download();
        std::memcpy(img.data(), tmp->host(), bytes);
        return true;
    }
    if (id == "box_blur") {
        be.box_blur(*src, *dst, w, h, rounded(pv(0, 4.0)));
    } else if (id == "blur") {
        be.box_blur(*src, *dst, w, h, 1);
    } else if (id == "blur_more") {
        be.box_blur(*src, *dst, w, h, 3);
    } else if (id == "median") {
        be.median_radius(*src, *dst, w, h, rounded(pv(0, 2.0)));
    } else if (id == "despeckle") {
        be.median_radius(*src, *dst, w, h, 1);
    } else if (id == "unsharp_mask") {
        const double amt = std::clamp(pv(0, 100.0) / 100.0, 0.0, 5.0);
        const double thr = std::clamp(pv(2, 0.0) / 255.0, 0.0, 1.0);
        be.unsharp_box(*src, *dst, w, h, static_cast<float>(amt),
                       rounded(pv(1, 2.0)), static_cast<float>(thr));
    } else if (id == "sharpen") {
        be.unsharp_box(*src, *dst, w, h, 0.6f, 1, 0.0f);
    } else if (id == "sharpen_more") {
        be.unsharp_box(*src, *dst, w, h, 1.2f, 1, 0.0f);
    } else if (id == "motion_blur") {
        be.motion_blur(*src, *dst, w, h, static_cast<float>(pv(0, 0.0)),
                       rounded(pv(1, 20.0)));
    } else if (id == "lens_blur") {
        const int shape = std::clamp(static_cast<int>(std::round(pv(1, 0.0))), 0, 6);
        be.lens_blur(*src, *dst, w, h,
                     rounded(std::clamp(pv(0, 8.0), 0.0, 50.0)),
                     shape == 0 ? 0 : 10 + shape + 2,
                     static_cast<float>(std::clamp(pv(2, 0.0), 0.0, 100.0) / 100.0),
                     static_cast<float>(std::clamp(pv(3, 200.0), 0.0, 255.0) / 255.0),
                     static_cast<float>(std::clamp(pv(4, 0.0), 0.0, 50.0) / 50.0));
    } else {
        return false;
    }
    dst->download();
    std::memcpy(img.data(), dst->host(), bytes);
    return true;
}

}  // namespace

bool AppState::applyFilterToActiveLayer(const std::string& id,
                                        const std::vector<double>& params) {
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels || layer->pixels->width() == 0 ||
        layer->pixels->height() == 0)
        return false;
    pittore::compute::ComputeBackend& be = computeBackend();
    const bool gpu = be.type() != pittore::compute::BackendType::CPU;
    if (gpu && filterGpuRouted(id)) {
        try {
            const double m = filterStrengthParam(id, params);
            if (m <= 0.0) return true;
            if (m < 1.0) {
                pittore::Image orig = layer->pixels->clone();
                if (!runFilterGpu(be, *layer->pixels, id, params)) {
                    pittore::filter::applyFilter(*layer->pixels, id, params);
                    return true;
                }
                const float k = static_cast<float>(m);
                pittore::RGBAf* px = layer->pixels->data();
                const pittore::RGBAf* q = orig.data();
                const std::size_t n = layer->pixels->pixel_count();
                for (std::size_t i = 0; i < n; ++i) {
                    px[i].r = q[i].r + (px[i].r - q[i].r) * k;
                    px[i].g = q[i].g + (px[i].g - q[i].g) * k;
                    px[i].b = q[i].b + (px[i].b - q[i].b) * k;
                    px[i].a = q[i].a + (px[i].a - q[i].a) * k;
                }
                return true;
            }
            if (runFilterGpu(be, *layer->pixels, id, params)) return true;
        } catch (const std::exception&) {
        }
    }
    pittore::filter::applyFilter(*layer->pixels, id, params);
    return true;
}

bool AppState::applyLayerEditOneShot(const std::function<void(pittore::Image&)>& edit,
                                     const QString& name, const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;
    beginUndoStep();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer) {
        discardUndoStep();
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) {
        discardUndoStep();
        return false;
    }
    edit(*layer->pixels);
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer);
    layer->thumbnail = QImage();
    d->rebuildComposite();
    commitUndoStep(name, iconKey);
    return true;
}

bool AppState::transformDocumentImage(const QString& op) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;

    const bool cw90 = op == QLatin1String("cw90");
    const bool ccw90 = op == QLatin1String("ccw90");
    const bool r180 = op == QLatin1String("180");
    const bool flipH = op == QLatin1String("flipH");
    const bool flipV = op == QLatin1String("flipV");
    if (!cw90 && !ccw90 && !r180 && !flipH && !flipV) return false;

    beginUndoStep();
    bool any = false;
    for (LayerItem& layer : d->layers) {
        // Only layers that own pixels transform (pixel, live-text, recovered
        // glyph layers). Groups / adjustment / empty layers stay put.
        if (!layer.pixels || layer.pixels->width() == 0 || layer.pixels->height() == 0)
            continue;
        if (layer.kind == LayerItem::Kind::Group) continue;
        const std::shared_ptr<pittore::Image> src = layer.pixels;
        const double lsx = std::max(layer.scaleX, 1e-6);
        const double lsy = std::max(layer.scaleY, 1e-6);
        const double cx = layer.offset.x() + src->width() * lsx * 0.5;
        const double cy = layer.offset.y() + src->height() * lsy * 0.5;

        std::shared_ptr<pittore::Image> dst;
        std::shared_ptr<pittore::color::CmykImage> nativeCmyk;
        if (layer.cmykPixels && layer.cmykPixels->valid() &&
            layer.cmykPixels->width == src->width() &&
            layer.cmykPixels->height == src->height() &&
            layer.cmykPixels->appearanceFingerprint ==
                pittore::color::CmykImage::fingerprint(*src)) {
            nativeCmyk = std::make_shared<pittore::color::CmykImage>(
                cw90 ? layer.cmykPixels->rotate90(true)
                : ccw90 ? layer.cmykPixels->rotate90(false)
                : r180 ? layer.cmykPixels->rotate180()
                : layer.cmykPixels->flip(flipH));
        }
        double sx = lsx, sy = lsy;
        const double origOx = layer.offset.x();
        const double origOy = layer.offset.y();
        if (cw90 || ccw90) {
            // Quarter turn: swap axes; keep the layer's document-space centre
            // fixed as the native bounds grow.
            dst = std::make_shared<pittore::Image>(cw90 ? rotate90Cw(*src) : rotate90Ccw(*src));
            transformLinkedMaskPixels(
                layer, *src,
                [cw90](const pittore::Image& m) {
                    return cw90 ? rotate90Cw(m) : rotate90Ccw(m);
                });
            sx = lsy;
            sy = lsx;
            layer.pixels = dst;
            layer.scaleX = sx;
            layer.scaleY = sy;
            layer.offset = QPointF(cx - dst->width() * sx * 0.5, cy - dst->height() * sy * 0.5);
        } else if (r180) {
            dst = std::make_shared<pittore::Image>(rotate180(*src));
            transformLinkedMaskPixels(
                layer, *src,
                [](const pittore::Image& m) { return rotate180(m); });
            layer.pixels = dst;
            layer.offset = QPointF(cx - dst->width() * sx * 0.5, cy - dst->height() * sy * 0.5);
        } else if (flipH) {
            // Mirror about the document's vertical centre axis: the layer's doc
            // bounds [a, b] map to [docW - b, docW - a] (no centre recompute).
            dst = std::make_shared<pittore::Image>(flipHorizontal(*src));
            transformLinkedMaskPixels(
                layer, *src,
                [](const pittore::Image& m) { return flipHorizontal(m); });
            layer.pixels = dst;
            const double b = origOx + src->width() * lsx;
            layer.offset.setX(d->size.width() - b);
        } else {
            dst = std::make_shared<pittore::Image>(flipVertical(*src));
            transformLinkedMaskPixels(
                layer, *src,
                [](const pittore::Image& m) { return flipVertical(m); });
            layer.pixels = dst;
            const double b = origOy + src->height() * lsy;
            layer.offset.setY(d->size.height() - b);
        }

        syncLinkedMaskPlacement(layer);
        ++layer.sourceStamp;
        if (nativeCmyk && layer.pixels) {
            nativeCmyk->appearanceFingerprint =
                pittore::color::CmykImage::fingerprint(*layer.pixels);
            layer.cmykPixels = std::move(nativeCmyk);
            layer.cmykSourceStamp = layer.sourceStamp;
        } else if (layer.cmykPixels) {
            layer.cmykPixels.reset();
            layer.cmykSourceStamp = 0;
        }
        layer.thumbnail = QImage();
        any = true;
    }
    if (!any) {
        discardUndoStep();
        return false;
    }
    d->rebuildComposite();
    commitUndoStep(op == QLatin1String("180") ? tr("Rotate 180°") : tr("Image Rotation"),
                   QStringLiteral("rotate"));
    return true;
}

bool AppState::activeHistogram(pittore::Histogram256& out) const {
    out = pittore::Histogram256{};
    DocumentItem* d = activeDocument();
    if (!d || d->composite.isNull()) return false;

    const QImage& img = d->composite;
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0) return false;

    // Cap the sample count (~250k) so a 100 MP composite does not stall the UI
    // thread; the shape of the distribution is unchanged by uniform sampling.
    int step = 1;
    while (static_cast<qint64>(w / step) * (h / step) > 250000) ++step;

    const QImage src = img.format() == QImage::Format_ARGB32
                           ? img
                           : img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < h; y += step) {
        const QRgb* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < w; x += step) {
            const int r = qRed(row[x]), g = qGreen(row[x]), b = qBlue(row[x]);
            const int yv = qBound(0, qRound(0.2126 * r + 0.7152 * g + 0.0722 * b), 255);
            ++out.rgb[0][r];
            ++out.rgb[1][g];
            ++out.rgb[2][b];
            ++out.luma[yv];
        }
    }
    return true;
}

}  // namespace pittore::ui
