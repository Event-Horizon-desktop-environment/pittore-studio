// Mask finishing + the density/feather/apply AppState ops (defined here, not
// in app_state.cpp, per the new-source-files rule; only declared in
// app_state.h).
#include "ui/mask_finish.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/compute/layer_mask.h"
#include "engine/core/parallel.h"
#include "engine/filter/core/filter_detail.h"
#include "ui/app_state.h"

namespace pittore::ui {

std::shared_ptr<pittore::Image> finishMaskImage(const pittore::Image& src,
                                                float density,
                                                float featherPx) {
    auto out = std::make_shared<pittore::Image>(src.width(), src.height());
    if (src.width() == 0 || src.height() == 0) return out;
    // Blur straight out of the caller's image instead of memcpy'ing the whole
    // frame first (5.9ms at 4K), and fold the density copy+scale into one
    // pass where there is no blur. Both branches stay byte-identical to the
    // old "memcpy, blur, then scale" sequence.
    if (featherPx > 0.0f) {
        pittore::filter::detail::gaussBlurFrom(src, *out,
                                                static_cast<double>(featherPx));
        if (density != 1.0f) {
            const float k = std::clamp(density, 0.0f, 1.0f);
            pittore::RGBAf* d = out->data();
            const std::uint32_t w = out->width(), h = out->height();
            // Rows disjoint: bit-identical.
            pittore::core::parallel_rows(h,
                                          [&](std::uint32_t lo, std::uint32_t hi) {
                for (std::uint32_t y = lo; y < hi; ++y) {
                    const std::size_t base = static_cast<std::size_t>(y) * w;
                    for (std::uint32_t x = 0; x < w; ++x) {
                        const std::size_t i = base + x;
                        d[i].r = std::clamp(d[i].r * k, 0.0f, 1.0f);
                        d[i].g = std::clamp(d[i].g * k, 0.0f, 1.0f);
                        d[i].b = std::clamp(d[i].b * k, 0.0f, 1.0f);
                    }
                }
            });
        }
    } else if (density != 1.0f) {
        // No blur to do: copy and scale in the same pass (alpha untouched,
        // exactly what memcpy-then-scale leaves behind).
        const float k = std::clamp(density, 0.0f, 1.0f);
        const pittore::RGBAf* s = src.data();
        pittore::RGBAf* d = out->data();
        const std::uint32_t w = out->width(), h = out->height();
        pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
            for (std::uint32_t y = lo; y < hi; ++y) {
                const std::size_t base = static_cast<std::size_t>(y) * w;
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t i = base + x;
                    d[i].r = std::clamp(s[i].r * k, 0.0f, 1.0f);
                    d[i].g = std::clamp(s[i].g * k, 0.0f, 1.0f);
                    d[i].b = std::clamp(s[i].b * k, 0.0f, 1.0f);
                    d[i].a = s[i].a;
                }
            }
        });
    } else {
        std::memcpy(out->data(), src.data(),
                    sizeof(pittore::RGBAf) * src.pixel_count());
    }
    return out;
}

bool patchFinishedMaskRegion(const pittore::Image& raw, float density,
                             float featherPx, int x0, int y0, int x1, int y1,
                             pittore::Image& finished) {
    const std::uint32_t w = raw.width(), h = raw.height();
    if (w == 0 || h == 0 || finished.width() != w || finished.height() != h)
        return false;
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(x1, static_cast<int>(w));
    y1 = std::min(y1, static_cast<int>(h));
    if (x0 >= x1 || y0 >= y1) return true;
    const float k = std::clamp(density, 0.0f, 1.0f);
    if (featherPx <= 0.05f) {
        // Blur is identity here (gaussBlur early-out): pointwise scale+copy.
        const pittore::RGBAf* s = raw.data();
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const pittore::RGBAf& p =
                    s[static_cast<std::size_t>(y) * w + x];
                pittore::RGBAf& o =
                    finished.data()[static_cast<std::size_t>(y) * w + x];
                o.r = std::clamp(p.r * k, 0.0f, 1.0f);
                o.g = std::clamp(p.g * k, 0.0f, 1.0f);
                o.b = std::clamp(p.b * k, 0.0f, 1.0f);
                o.a = p.a;
            }
        }
        return true;
    }
    // 3 box passes of radius r spread exactly 3r: a halo of 3r+2 makes the
    // dirty rect bit-identical to the full-frame finish (no tap crosses the
    // patch edge into the dirty area).
    const int r = std::max(1, static_cast<int>(std::round(featherPx)));
    const int halo = 3 * r + 2;
    const int ex0 = std::max(0, x0 - halo);
    const int ey0 = std::max(0, y0 - halo);
    const int ex1 = std::min(static_cast<int>(w), x1 + halo);
    const int ey1 = std::min(static_cast<int>(h), y1 + halo);
    const std::uint32_t pw = static_cast<std::uint32_t>(ex1 - ex0);
    const std::uint32_t ph = static_cast<std::uint32_t>(ey1 - ey0);
    pittore::Image patch(pw, ph);
    const pittore::RGBAf* s = raw.data();
    for (std::uint32_t y = 0; y < ph; ++y)
        std::memcpy(patch.data() + static_cast<std::size_t>(y) * pw,
                    s + static_cast<std::size_t>(ey0 + y) * w + ex0,
                    static_cast<std::size_t>(pw) * sizeof(pittore::RGBAf));
    pittore::filter::detail::gaussBlur(patch, static_cast<double>(featherPx));
    // Rows disjoint: bit-identical (serial below 64 rows by design).
    pittore::core::parallel_rows(static_cast<std::uint32_t>(y1 - y0),
                                 [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t yy = lo; yy < hi; ++yy) {
            const int y = y0 + static_cast<int>(yy);
            for (int x = x0; x < x1; ++x) {
                const pittore::RGBAf& p = patch.data()[static_cast<std::size_t>(y - ey0) * pw + (x - ex0)];
                pittore::RGBAf& o =
                    finished.data()[static_cast<std::size_t>(y) * w + x];
                o.r = std::clamp(p.r * k, 0.0f, 1.0f);
                o.g = std::clamp(p.g * k, 0.0f, 1.0f);
                o.b = std::clamp(p.b * k, 0.0f, 1.0f);
                o.a = p.a;
            }
        }
    });
    return true;
}

float sampleCoverageBilinear(const pittore::Image& m, double x, double y) {
    const std::uint32_t w = m.width(), h = m.height();
    if (w == 0 || h == 0) return 1.0f;
    x = std::clamp(x, 0.0, static_cast<double>(w) - 1.0);
    y = std::clamp(y, 0.0, static_cast<double>(h) - 1.0);
    const std::uint32_t x0 = static_cast<std::uint32_t>(x);
    const std::uint32_t y0 = static_cast<std::uint32_t>(y);
    const std::uint32_t x1 = std::min(x0 + 1, w - 1);
    const std::uint32_t y1 = std::min(y0 + 1, h - 1);
    const float fx = static_cast<float>(x - x0);
    const float fy = static_cast<float>(y - y0);
    const pittore::RGBAf* d = m.data();
    const float c00 = d[static_cast<std::size_t>(y0) * w + x0].r;
    const float c10 = d[static_cast<std::size_t>(y0) * w + x1].r;
    const float c01 = d[static_cast<std::size_t>(y1) * w + x0].r;
    const float c11 = d[static_cast<std::size_t>(y1) * w + x1].r;
    return (c00 * (1 - fx) + c10 * fx) * (1 - fy) +
           (c01 * (1 - fx) + c11 * fx) * fy;
}

bool AppState::setLayerMaskDensity(float density) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l || !l->hasMask || !l->mask) {
        if (d && l && (!l->hasMask || !l->mask))
            setStatusHint(tr("The active layer has no mask."));
        return false;
    }
    density = std::clamp(density, 0.0f, 1.0f);
    if (l->maskDensity == density) return false;
    // Slider contract (like opacity/adjustment sliders): live write; the
    // caller holds the undo step (drag gesture or single edit).
    l->maskDensity = density;
    ++l->maskStamp;  // invalidates the finished-mask device cache
    d->rebuildComposite();
    emit documentModified(d);
    return true;
}

bool AppState::setLayerMaskFeather(float px) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l || !l->hasMask || !l->mask) {
        if (d && l && (!l->hasMask || !l->mask))
            setStatusHint(tr("The active layer has no mask."));
        return false;
    }
    px = std::max(0.0f, px);
    if (l->maskFeather == px) return false;
    l->maskFeather = px;
    ++l->maskStamp;
    d->rebuildComposite();
    emit documentModified(d);
    return true;
}

bool AppState::applyLayerMask() {
    DocumentItem* d = activeDocument();
    if (!d || !activeLayer() || activeLayer()->kind != LayerItem::Kind::Pixel ||
        !activeLayer()->hasMask || !activeLayer()->mask ||
        !activeLayer()->pixels) {
        setStatusHint(tr("The active pixel layer has no mask to apply."));
        return false;
    }
    beginUndoStep();
    if (!copyOnWriteActiveLayer()) {
        discardUndoStep();
        return false;
    }
    LayerItem* layer = activeLayer();
    // Finished coverage (feather + density baked), sampled through the live
    // placement: linked masks ride the layer transform, unlinked keep their
    // frozen one (mirrors maskTransformFor in app_state.cpp).
    std::shared_ptr<pittore::Image> finished;
    const pittore::Image* m = layer->mask.get();
    if (layer->maskFeather > 0.0f || layer->maskDensity != 1.0f) {
        finished =
            finishMaskImage(*m, layer->maskDensity, layer->maskFeather);
        m = finished.get();
    }
    QPointF moff;
    double msx, msy;
    if (layer->maskLinked) {
        moff = layer->offset;
        msx = layer->scaleX;
        msy = layer->scaleY;
    } else {
        moff = layer->maskOffset;
        msx = layer->maskScaleX;
        msy = layer->maskScaleY;
    }
    const std::uint32_t nw = layer->pixels->width();
    const std::uint32_t nh = layer->pixels->height();
    pittore::RGBAf* px = layer->pixels->data();
    for (std::uint32_t y = 0; y < nh; ++y) {
        for (std::uint32_t x = 0; x < nw; ++x) {
            const double docX = layer->offset.x() + x * layer->scaleX;
            const double docY = layer->offset.y() + y * layer->scaleY;
            float cov = 1.0f;
            if (msx > 0.0 && msy > 0.0)
                cov = sampleCoverageBilinear(
                    *m, (docX - moff.x()) / msx, (docY - moff.y()) / msy);
            pittore::RGBAf& p = px[static_cast<std::size_t>(y) * nw + x];
            p.a = std::clamp(p.a * cov, 0.0f, 1.0f);
        }
    }
    layer->hasMask = false;
    layer->mask.reset();
    layer->maskDensity = 1.0f;
    layer->maskFeather = 0.0f;
    ++layer->sourceStamp;
    ++layer->maskStamp;
    layer->thumbnail = QImage();
    commitUndoStep(tr("Apply Layer Mask"), QStringLiteral("mask"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

}  // namespace pittore::ui
