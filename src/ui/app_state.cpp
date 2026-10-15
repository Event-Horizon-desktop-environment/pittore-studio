#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/ai_models.h"
#include "ui/color_mode.h"
#include "ui/live_filter.h"
#include "ui/mask_finish.h"
#include "ui/project_manager.h"
#include "ui/selection_mask.h"
#include "ui/settings.h"
#include "ui/color_mode.h"
#include "ui/svg_parts.h"

#include "engine/io/af.h"
#include "engine/io/af_layers.h"
#include "engine/io/af_layers/emit/af_write.h"
#include "engine/io/icc.h"
#include "engine/io/psd.h"
#ifdef PITTORE_TIFF
#include "engine/io/tiff.h"
#endif
#include "engine/text/text_engine.h"
#include "engine/filter/filters.h"
#include <QDir>
#include <QColorSpace>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <QLinearGradient>
#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>

#include "engine/compute/backend.h"
#include "engine/compute/dither.h"
#include "engine/compute/factory.h"
#include "engine/compute/adjust.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/paint.h"
#include "engine/compute/brushes/blur/blur_dab.h"
#include "engine/compute/brushes/adjust/adjust_dab.h"
#include "engine/compute/brushes/history/history_dab.h"
#include "engine/compute/brushes/mixer/mixer.h"
#include "engine/compute/brushes/mix/mix.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/core/log.h"
#include "engine/core/parallel.h"
#include "engine/core/tonal_ops.h"
#include "ui/tools/log/tool_log.h"

namespace pittore::ui {
namespace {

// Separator positions match the conventional blend-mode menu families.
const QSet<int>& separatorIndices() {
    static const QSet<int> s = {1, 4, 9, 14, 21, 25};
    return s;
}

// Fallback used by documents before AppState points them at a real backend, and
// whenever a GPU selection fails (unavailable runtime, refused device). The
// live backend (AppState::computeBackend) replaces this per document.
pittore::compute::ComputeBackend& cpuBackend() {
    static std::unique_ptr<pittore::compute::ComputeBackend> backend =
        pittore::compute::make_backend(pittore::compute::BackendType::CPU);
    return *backend;
}

// Create the backend a settings block requests: GPU when enabled and usable
// (the named device when it exists, else the first CUDA/HIP device; anything
// unavailable falls through to a second CPU attempt, never to nullptr).
std::unique_ptr<pittore::compute::ComputeBackend> makeBackendFor(
    const AppSettings& s) {
    if (s.gpuEnabled) {
        const auto devices = pittore::compute::enumerate_devices();
        // Named device first; then any GPU.
        for (const auto& d : devices) {
            if (d.type == pittore::compute::BackendType::CPU) continue;
            if (!s.gpuDevice.isEmpty() &&
                s.gpuDevice != QString::fromStdString(d.name))
                continue;
            if (auto b = pittore::compute::make_backend(d.type)) {
                ::pittore::core::log::log_info(
                    "[prefs] requested GPU '%s', selected %s",
                    s.gpuDevice.isEmpty() ? "(any)" : s.gpuDevice.toStdString().c_str(),
                    b->name().c_str());
                return b;
            }
        }
        // No named device matched → pick the first GPU we can build.
        for (const auto& d : devices) {
            if (d.type == pittore::compute::BackendType::CPU) continue;
            if (auto b = pittore::compute::make_backend(d.type)) {
                ::pittore::core::log::log_info(
                    "[prefs] no matching device, fell through to %s", b->name().c_str());
                return b;
            }
        }
        ::pittore::core::log::log_warning(
            "[prefs] GPU requested but no GPU backend could be built; using CPU");
    }
    // GPU disabled / unusable → CPU reference backend (always buildable).
    return pittore::compute::make_backend(pittore::compute::BackendType::CPU);
}

std::size_t layerBytes(const QSize& size) {
    return static_cast<std::size_t>(size.width()) * size.height() * sizeof(pittore::RGBAf);
}

// Rotate opaque-grey mask coverage through the same Qt transform a pixel
// rotation uses, so a linked mask keeps its orientation as well as its size.
std::shared_ptr<pittore::Image> rotatedMaskImage(const pittore::Image& mask,
                                                  const QTransform& t) {
    const int mw = static_cast<int>(mask.width());
    const int mh = static_cast<int>(mask.height());
    if (mw <= 0 || mh <= 0) return nullptr;
    QImage gray(mw, mh, QImage::Format_Grayscale8);
    if (gray.isNull()) return nullptr;
    for (int y = 0; y < mh; ++y) {
        uchar* row = gray.scanLine(y);
        for (int x = 0; x < mw; ++x) {
            const float c = std::clamp(
                mask.data()[static_cast<std::size_t>(y) * mw + x].r, 0.0f,
                1.0f);
            row[x] = static_cast<uchar>(
                qBound(0, static_cast<int>(std::lround(c * 255.0f)), 255));
        }
    }
    const QImage rotated = gray.transformed(t, Qt::SmoothTransformation);
    if (rotated.isNull()) return nullptr;
    const QImage conv = rotated.convertToFormat(QImage::Format_Grayscale8);
    if (conv.isNull()) return nullptr;
    auto out = std::make_shared<pittore::Image>(
        static_cast<std::uint32_t>(conv.width()),
        static_cast<std::uint32_t>(conv.height()));
    for (int y = 0; y < conv.height(); ++y) {
        const uchar* row = conv.constScanLine(y);
        pittore::RGBAf* dst =
            out->data() + static_cast<std::size_t>(y) * conv.width();
        for (int x = 0; x < conv.width(); ++x)
            dst[x] = pittore::compute::make_mask_pixel(row[x] / 255.0f);
    }
    return out;
}

// Decode an imported QImage (premultiplied ARGB32) into a straight-alpha
// engine Image at native resolution. The result is the lossless source a
// placed layer keeps; the compositor resamples from it on the fly. Images
// over 100 MP are refused (a 100 MP RGBAf layer alone is 1.6 GB).
std::shared_ptr<pittore::Image> imageFromQImage(const QImage& in) {
    if (in.isNull()) return nullptr;
    const QImage img = in.format() == QImage::Format_ARGB32_Premultiplied
                           ? in
                           : in.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return nullptr;
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0 ||
        static_cast<qint64>(w) * h > 100'000'000)
        return nullptr;
    auto out = std::make_shared<pittore::Image>(static_cast<std::uint32_t>(w),
                                                 static_cast<std::uint32_t>(h));
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        pittore::RGBAf* dst =
            out->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 0; x < w; ++x) {
            const int a = qAlpha(row[x]);
            if (a <= 0) {
                dst[x] = pittore::RGBAf{0, 0, 0, 0};
                continue;
            }
            const float inv = 1.0f / static_cast<float>(a);
            dst[x] = pittore::RGBAf{qRed(row[x]) * inv, qGreen(row[x]) * inv,
                                     qBlue(row[x]) * inv,
                                     static_cast<float>(a) / 255.0f};
        }
    }
    return out;
}

}  // namespace

// Full 27-mode + Pass Through mapping (blend.h owns the names it understands).
// "Add" is the imported display name for Linear Dodge (Add) — the one name
// that differs from the engine's; everything else round-trips as-is.
// Namespace scope (declared in app_state_detail.h) so the split-out
// composite TU can call it.
pittore::compute::BlendMode engineBlendMode(const QString& name) {
    if (name == QStringLiteral("Add")) return pittore::compute::BlendMode::LinearDodge;
    return pittore::compute::blend::from_display_name(name.toUtf8().constData());
}

// History source lookup shared by the History and Art History Brushes:
// the oldest undo snapshot's matching layer (document-open state).
// Shared pixel Images are copy-on-write safe to read. Null when no
// usable history state exists yet.
const LayerItem* AppState::historySourceFor(DocumentItem* d, int activeLayer,
                                            std::uint32_t lw,
                                            std::uint32_t lh) {
    if (!d || d->undoStack_.isEmpty()) return nullptr;
    const DocumentItem::DocumentSnapshot& snap = d->undoStack_.first();
    const int ai = std::clamp(activeLayer, 0, int(d->layers.size()) - 1);
    if (ai >= 0 && ai < snap.layers.size()) {
        const LayerItem& c = snap.layers[ai];
        if (c.kind == LayerItem::Kind::Pixel && c.pixels &&
            c.pixels->width() == lw && c.pixels->height() == lh)
            return &c;
    }
    for (const LayerItem& c : snap.layers) {
        if (c.kind == LayerItem::Kind::Pixel && c.pixels &&
            c.pixels->width() == lw && c.pixels->height() == lh)
            return &c;
    }
    return nullptr;
}


// Declared in app_state.h; defined here (outside the anonymous namespace
// above) so the Layers panel can link against them.
QRect layerMaskThumbRect(const LayerItem& layer) {
    const int x = 26 + layer.indent * 12 +
                  (layer.kind == LayerItem::Kind::Group ? 14 : 0) + 32 + 6;
    return QRect(x, 4, 32, 32);
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

pittore::text::TextSpec textSpecFor(const TextItem& t) {
    pittore::text::TextSpec spec;
    spec.text = t.text.toStdString();
    spec.family = t.family.toStdString();
    spec.bold = t.bold;
    spec.italic = t.italic;
    spec.size = static_cast<float>(std::max(1.0, t.size));
    spec.align = t.align == 1   ? pittore::text::Align::Center
                 : t.align == 2 ? pittore::text::Align::Right
                                : pittore::text::Align::Left;
    spec.lineHeight = static_cast<float>(std::max(0.1, t.lineHeight));
    spec.tracking = static_cast<float>(t.tracking);
    if (t.wrapWidth > 0.5) spec.wrapWidth = static_cast<float>(t.wrapWidth);
    spec.underline = qBound(0, t.underline, 2);
    spec.strike = qBound(0, t.strike, 2);
    auto rgba = [](const QColor& c) {
        return std::array<float, 4>{c.redF(), c.greenF(), c.blueF(), c.alphaF()};
    };
    spec.inkColor = rgba(t.color);
    if (t.underlineColor.isValid()) spec.underlineColor = rgba(t.underlineColor);
    if (t.strikeColor.isValid()) spec.strikeColor = rgba(t.strikeColor);
    if (t.backgroundColor.isValid()) spec.backgroundColor = rgba(t.backgroundColor);
    spec.baselineShift = static_cast<float>(t.baselineShift);
    spec.hScale = static_cast<float>(t.hScale > 0.0 ? t.hScale / 100.0 : 1.0);
    spec.vScale = static_cast<float>(t.vScale > 0.0 ? t.vScale / 100.0 : 1.0);
    spec.superSub = qBound(-1, t.superSub, 1);
    spec.allCaps = t.allCaps;
    spec.kerning = t.kerning;
    spec.otFeatures = t.otFeatures;
    return spec;
}

pittore::text::TextLayout textLayoutFor(const TextItem& t) {
    std::optional<pittore::text::TextLayout> laid =
        pittore::text::layoutText(textSpecFor(t));
    return laid ? *laid : pittore::text::TextLayout{};
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

QString typeFamilyForIndex(int index) {
    const std::vector<std::string>& families = typeFontFamilies();
    if (index < 0 || index >= static_cast<int>(families.size())) return QString();
    return QString::fromStdString(families[static_cast<std::size_t>(index)]);
}

int typeIndexForFamily(const QString& family) {
    const std::vector<std::string>& families = typeFontFamilies();
    const std::string want = family.toStdString();
    for (std::size_t i = 0; i < families.size(); ++i)
        if (families[i] == want) return static_cast<int>(i);
    return -1;
}

// Halo-grown staging bounds of a layer for culling: identity layers are exact
// (a memcpy blit cannot bleed), while a resampled layer can colour document
// pixels up to one source texel past its bounds (the sampler footprint). This
// trades a tiny halo of rework for correctness when the toggled/edited layer
// is scaled (the sampler reaches one texel past the rect).
LayerDrawSource layerDrawSource(const LayerItem& l) {
    ensureLayerFilter(l);
    ensureLayerStyle(l);
    LayerDrawSource s;
    if (l.styled) {
        // The styled raster may be a capped bake (see ensureLayerStyle): scale
        // it back up to the layer's full document footprint so effects keep
        // their exact document size while the CPU/VRAM cost stays bounded.
        s.img = l.styled;
        s.offset = l.styledOffset;
        const double r = l.styledResample;
        s.scaleX = r > 0.0 ? 1.0 / r : 1.0;
        s.scaleY = r > 0.0 ? 1.0 / r : 1.0;
    } else {
        // Unstyled: composite the filtered render when a live filter
        // applies, else the native pixels (same placement either way —
        // the cache is layer-sized by construction).
        if (l.filtered && l.filteredValid)
            s.img = l.filtered;
        else
            s.img = l.pixels;
        s.offset = l.offset;
        s.scaleX = l.scaleX;
        s.scaleY = l.scaleY;
    }
    return s;
}

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

    // THROWAWAY thumb logging (removed after diagnosis).
    ::pittore::core::log::log_info(
        "[thumb] group '%s' idx=%d pix=%d ops=%d",
        d.layers[groupIndex].name.toUtf8().constData(), groupIndex,
        pix.size(), ops.size());
    if (pix.isEmpty()) {
        ::pittore::core::log::log_info("[thumb] group '%s' NULL: no pixels",
                                        d.layers[groupIndex].name.toUtf8().constData());
        return QImage();
    }
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
    if (b.width() <= 0.0 || b.height() <= 0.0) {
        // THROWAWAY thumb logging (removed after diagnosis).
        ::pittore::core::log::log_info("[thumb] group '%s' NULL: bad bounds",
                                        d.layers[groupIndex].name.toUtf8().constData());
        return QImage();
    }
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
    // THROWAWAY thumb logging (removed after diagnosis).
    if (!painted) {
        ::pittore::core::log::log_info("[thumb] group '%s' NULL: unpainted",
                                        d.layers[groupIndex].name.toUtf8().constData());
        return QImage();
    }

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

int topPixelLayerAt(const DocumentItem& d, const QPointF& docPos) {
    for (int i = 0; i < d.layers.size(); ++i) {
        const LayerItem& l = d.layers[i];
        if (!l.visible || l.kind != LayerItem::Kind::Pixel || !l.pixels)
            continue;
        if (layerBounds(d, l).contains(docPos)) return i;
    }
    return -1;
}

QColor sampleCompositeColor(const DocumentItem& d, const QPointF& docPos,
                            int sampleSize) {
    if (d.composite.isNull() || d.size.isEmpty()) return QColor();
    static const int kSizes[] = {1, 3, 5, 11, 31, 51, 101};
    const int n = kSizes[std::clamp(sampleSize, 0, 6)];
    const int cx = static_cast<int>(std::floor(docPos.x()));
    const int cy = static_cast<int>(std::floor(docPos.y()));
    if (!d.composite.rect().contains(QPoint(cx, cy))) return QColor();

    QImage img = d.composite;
    if (img.format() != QImage::Format_ARGB32_Premultiplied &&
        img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);

    const int half = n / 2;
    double sr = 0, sg = 0, sb = 0, sa = 0;
    int count = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        if (y < 0 || y >= img.height()) continue;
        for (int x = cx - half; x <= cx + half; ++x) {
            if (x < 0 || x >= img.width()) continue;
            const QRgb p = img.pixel(x, y);
            const int a = qAlpha(p);
            // Unpremultiply per sample so the average is of true colours, not
            // of premultiplied values (which would darken soft edges).
            if (a > 0) {
                sr += double(qRed(p)) * 255.0 / a;
                sg += double(qGreen(p)) * 255.0 / a;
                sb += double(qBlue(p)) * 255.0 / a;
            }
            sa += a;
            ++count;
        }
    }
    if (count == 0) return QColor();
    const int outA = qBound(0, int(std::lround(sa / count)), 255);
    return QColor(qBound(0, int(std::lround(sr / count)), 255),
                  qBound(0, int(std::lround(sg / count)), 255),
                  qBound(0, int(std::lround(sb / count)), 255), outA);
}

// Serialize a live document into the on-disk project description (no file IO).
// The single source for createNewProject and saveProject; round-trips exactly
// through loadProjectFile + openProject.
bool psdRawBlocksFresh(const LayerItem& l) {
    if (l.psdRawBlocks.empty()) return false;
    if (l.kind != l.psdRawKind) return false;
    if (l.sourceStamp != l.psdRawSrc) return false;
    if (l.kind == LayerItem::Kind::Adjustment && l.adjustStamp != l.psdRawAdj)
        return false;
    if (l.hasMask != l.psdRawHadMask) return false;
    if (l.hasMask && l.mask && l.maskStamp != l.psdRawMask) return false;
    if (l.offset != l.psdRawOffset || l.scaleX != l.psdRawScaleX ||
        l.scaleY != l.psdRawScaleY)
        return false;
    return true;
}

const QStringList& blendModeNames() {
    static const QStringList names = {
        QStringLiteral("Normal"), QStringLiteral("Dissolve"),
        QStringLiteral("Darken"), QStringLiteral("Multiply"), QStringLiteral("Color Burn"),
        QStringLiteral("Linear Burn"), QStringLiteral("Darker Color"),
        QStringLiteral("Lighten"), QStringLiteral("Screen"), QStringLiteral("Color Dodge"),
        QStringLiteral("Linear Dodge (Add)"), QStringLiteral("Lighter Color"),
        QStringLiteral("Overlay"), QStringLiteral("Soft Light"), QStringLiteral("Hard Light"),
        QStringLiteral("Vivid Light"), QStringLiteral("Linear Light"), QStringLiteral("Pin Light"),
        QStringLiteral("Hard Mix"),
        QStringLiteral("Difference"), QStringLiteral("Exclusion"), QStringLiteral("Subtract"),
        QStringLiteral("Divide"),
        QStringLiteral("Hue"), QStringLiteral("Saturation"), QStringLiteral("Color"),
        QStringLiteral("Luminosity")};
    return names;
}

bool blendModeIsSeparatorBefore(int index) { return separatorIndices().contains(index); }

// ---------------------------------------------------------------------------
// DocumentItem
// ---------------------------------------------------------------------------
DocumentItem::DocumentItem(QString t, QSize s, int d)
    : title(std::move(t)), size(s), dpi(d) {
    LayerItem bg;
    bg.name = QStringLiteral("Background");
    bg.locked = true;
    bg.swatch = QColor(0xf2, 0xf2, 0xf2);  // matches default canvasPaper
    layers.append(bg);

    history.append({QStringLiteral("New"), QStringLiteral("newlayer"), true});
    historyPosition = 0;
    rebuildComposite();
}

// Persistent per-document staging: the engine accumulator, allocated once per
// document size, plus the device-side source caches below. A brush stroke
// never reallocates per dab; renderRegion reuses the accumulator for
// incremental recomposites.
//
// `sources` is the device-side cache of placed layers' NATIVE pixels. GPU
// compositing samples moved/scaled layers straight from these (fused
// composite_many_into) instead of re-uploading the photo and re-sampling on
// the host every frame of a move/resize drag. Entries are keyed by the
// layer's pixels pointer + sourceStamp + byte count so a brush dab or a
// reallocated Image is re-uploaded before it is sampled.
struct DocumentItem::PaintStage {
    struct PlacedSource {
        const void* key = nullptr;   // l.pixels->data() when cached
        std::uint64_t stamp = 0;     // LayerItem::sourceStamp at cache time
        std::uint64_t rev = 0;       // LayerItem::styledRev at cache time
        std::size_t bytes = 0;
        std::unique_ptr<pittore::compute::Buffer> dev;
    };
    std::unique_ptr<pittore::compute::Buffer> acc;
    // Live Tone Blend Group scratch pool: per-nesting-depth (group, backdrop)
    // buffer pairs. Recursion depth indexes it, so nested groups never share
    // temporaries. Grown lazily inside the rebuild; persists across rebuilds.
    std::vector<std::unique_ptr<pittore::compute::Buffer>> toneScratch;
    // Keyed by l.pixels->data() so the per-layer device-texture lookup is O(1)
    // instead of a linear scan over 4k cached layers. Each full-canvas
    // recomposite was doing ~16.8M pointer compares (~4 ms) hunting for its
    // own layers — the dominant cost of a background/large-layer toggle.
    std::unordered_map<const void*, PlacedSource> sources;
    // Parallel cache for opaque-grey mask coverage, keyed by l.mask->data()
    // plus LayerItem::maskStamp. Pruned alongside `sources`.
    std::unordered_map<const void*, PlacedSource> masks;
    // Finished (density/feather-baked) mask images, keyed by the raw mask
    // pointer. The device upload in `masks` above always copies from here
    // when present, so the backends sample finished coverage untouched.
    struct FinishedMask {
        std::shared_ptr<pittore::Image> img;
        std::uint64_t stamp = 0;
        float density = 1.0f;
        float feather = 0.0f;
    };
    std::unordered_map<const void*, FinishedMask> finishedMasks;
    // Parallel cache for Curves LUT uploads (256 floats), keyed by
    // l.adjustmentLUT.data() plus LayerItem::adjustStamp.
    std::unordered_map<const void*, PlacedSource> auxs;
};

void DocumentItem::ensurePainter() {
    if (stage_ && stage_->acc) return;
    const std::size_t bytes = layerBytes(size);
    pittore::compute::ComputeBackend& be =
        backend ? *backend : cpuBackend();
    if (!stage_) stage_ = std::make_unique<PaintStage>();
    stage_->acc = std::move(be.make_buffer(bytes));
}

// Cached device copy of a placed layer's native pixels (see PaintStage::sources).
// Returns a buffer the GPU backend samples in composite_placed; the host copy
// is uploaded once and re-uploaded only when the layer's pixels change.
const pittore::compute::Buffer& DocumentItem::placedSourceFor(const LayerItem& l) {
    ensurePainter();
    const LayerDrawSource s = layerDrawSource(l);
    // Keyed on the layer's own pixels (stable across style edits) even when the
    // uploaded bytes are the styled render, so a dragged slider refreshes one
    // device slot instead of accumulating a new one per frame.
    const void* key = l.pixels ? static_cast<const void*>(l.pixels->data()) : nullptr;
    const std::size_t nbytes =
        s.img ? static_cast<std::size_t>(s.img->width()) * s.img->height() *
                    sizeof(pittore::RGBAf)
              : 0;
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    auto it = stage_->sources.find(key);
    if (it != stage_->sources.end()) {
        PaintStage::PlacedSource& e = it->second;
        if (e.dev && e.stamp == l.sourceStamp && e.bytes == nbytes &&
            e.rev == l.styledRev)
            return *e.dev;
        // Same source, new revision: refresh the existing slot instead of
        // pushing a new one, so a paint stroke never accumulates whole-layer
        // device buffers (a fresh Image replaces the slot outright).
        if (!e.dev || e.bytes != nbytes) e.dev = be.make_buffer(nbytes);
        e.stamp = l.sourceStamp;
        e.rev = l.styledRev;
        e.bytes = nbytes;
        if (key && s.img) std::memcpy(e.dev->host(), s.img->data(), nbytes);
        // Buffers are created zeroed; push the current pixels (no-op on CPU).
        e.dev->upload();
        // Full-layer refresh: rare (edits go through refreshPlacedRegion),
        // so this stays as the churn signal when fallbacks fire per event.
        if (pittore::core::log::strokeTrace())
            ::pittore::core::log::log_info(
                "[render][placed] source refreshed key=%p bytes=%zu backend=%s", key,
                nbytes, be.name().c_str());
        return *e.dev;
    }
    auto dev = be.make_buffer(nbytes);
    if (key && s.img) std::memcpy(dev->host(), s.img->data(), nbytes);
    // Buffers are created zeroed; push the current pixels (no-op on CPU).
    dev->upload();
    // Cache fills are per-layer traffic on thousand-layer documents (one
    // line per layer per cold composite); only an explicit trace pays for
    // it. The refresh path just above is already gated the same way.
    if (pittore::core::log::strokeTrace())
        ::pittore::core::log::log_info(
            "[render][placed] source cached key=%p bytes=%zu backend=%s", key,
            nbytes, be.name().c_str());
    auto ins = stage_->sources.emplace(
        key, PaintStage::PlacedSource{key, l.sourceStamp, l.styledRev, nbytes,
                                      std::move(dev)});
    return *ins.first->second.dev;
}

// Cached device copy of a layer mask's opaque-grey coverage. The host copy
// is uploaded once and re-uploaded only when the mask image changes.
const pittore::Image* DocumentItem::finishedMaskFor(const LayerItem& l) {
    const pittore::Image* mask = effectiveMaskImage(size, l);
    if (!mask || (l.maskFeather <= 0.0f && l.maskDensity == 1.0f))
        return nullptr;
    ensurePainter();
    const void* fkey = static_cast<const void*>(mask->data());
    auto fit = stage_->finishedMasks.find(fkey);
    if (fit != stage_->finishedMasks.end() &&
        fit->second.stamp == l.maskStamp &&
        fit->second.density == l.maskDensity &&
        fit->second.feather == l.maskFeather)
        return fit->second.img.get();
    auto finished = finishMaskImage(*mask, l.maskDensity, l.maskFeather);
    const pittore::Image* out = finished.get();
    stage_->finishedMasks[fkey] = {std::move(finished), l.maskStamp,
                                   l.maskDensity, l.maskFeather};
    return out;
}

const pittore::compute::Buffer* DocumentItem::maskSourceFor(const LayerItem& l) {
    const pittore::Image* mask = effectiveMaskImage(size, l);
    if (!mask) return nullptr;
    ensurePainter();
    // Density/feather finish on the host (cached; see finishedMaskFor). The
    // device upload below copies the finished bytes, so every backend
    // samples identical coverage.
    const pittore::Image* upload = mask;
    if (const pittore::Image* fin = finishedMaskFor(l)) upload = fin;
    // Keyed by the raw mask (stamp-invalidated); the uploaded bytes are the
    // finished image when density/feather apply (identical dimensions).
    const void* key = static_cast<const void*>(mask->data());
    const std::size_t nbytes = static_cast<std::size_t>(mask->width()) *
                               mask->height() * sizeof(pittore::RGBAf);
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    auto it = stage_->masks.find(key);
    if (it != stage_->masks.end()) {
        PaintStage::PlacedSource& e = it->second;
        if (e.dev && e.stamp == l.maskStamp && e.bytes == nbytes)
            return e.dev.get();
        if (!e.dev || e.bytes != nbytes) e.dev = be.make_buffer(nbytes);
        e.stamp = l.maskStamp;
        e.rev = 0;
        e.bytes = nbytes;
        std::memcpy(e.dev->host(), upload->data(), nbytes);
        e.dev->upload();
        return e.dev.get();
    }
    auto dev = be.make_buffer(nbytes);
    std::memcpy(dev->host(), upload->data(), nbytes);
    dev->upload();
    auto ins = stage_->masks.emplace(
        key, PaintStage::PlacedSource{key, l.maskStamp, 0, nbytes,
                                      std::move(dev)});
    return ins.first->second.dev.get();
}

void DocumentItem::refreshPlacedRegion(LayerItem& l, const QRect& layerRect) {
    if (!l.pixels) {
        ++l.sourceStamp;
        return;
    }
    ensurePainter();
    // Direct iff the compositor samples the native pixels themselves (no
    // live filter/style render in between). Pointer comparison is exact:
    // filtered/styled renders are separate Images.
    const LayerDrawSource s = layerDrawSource(l);  // cheap on valid stamps
    if (!s.img || s.img->data() != l.pixels->data()) {
        ++l.sourceStamp;  // filtered/styled: full refresh on next gather
        return;
    }
    const std::uint32_t w = s.img->width();
    const std::uint32_t h = s.img->height();
    const int x0 = std::max(0, layerRect.left());
    const int y0 = std::max(0, layerRect.top());
    const int x1 = std::min<std::uint32_t>(w, static_cast<std::uint32_t>(
        std::max(0, layerRect.left() + layerRect.width())));
    const int y1 = std::min<std::uint32_t>(h, static_cast<std::uint32_t>(
        std::max(0, layerRect.top() + layerRect.height())));
    if (x0 >= x1 || y0 >= y1) return;  // dab missed: pixels provably untouched
    const void* key = static_cast<const void*>(l.pixels->data());
    const std::size_t nbytes =
        static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    auto it = stage_->sources.find(key);
    if (it == stage_->sources.end() || !it->second.dev ||
        it->second.bytes != nbytes || it->second.stamp != l.sourceStamp ||
        it->second.rev != l.styledRev) {
        ++l.sourceStamp;  // no usable slot: full refresh on next gather
        return;
    }
    // Slot bytes are current except this rect: patch the staged rows and push
    // only the region (no-op on CPU). The stamp stays valid — no churn.
    PaintStage::PlacedSource& e = it->second;
    const auto* src = s.img->data();
    auto* dst = static_cast<char*>(e.dev->host());
    const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(pittore::RGBAf);
    const std::size_t rowBytes =
        static_cast<std::size_t>(x1 - x0) * sizeof(pittore::RGBAf);
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * pitch +
                        static_cast<std::size_t>(x0) * sizeof(pittore::RGBAf),
                    src + static_cast<std::size_t>(y) * w + x0, rowBytes);
    e.dev->upload_region(w, static_cast<std::uint32_t>(x0),
                         static_cast<std::uint32_t>(y0),
                         static_cast<std::uint32_t>(x1),
                         static_cast<std::uint32_t>(y1));
}

void DocumentItem::touchPixelsAfterDab(LayerItem& l, float lcx, float lcy,
                                       float lradius, std::uint32_t lw,
                                       std::uint32_t lh,
                                       bool preserveInkColor) {
    const int x0 = std::max(0, static_cast<int>(std::floor(lcx - lradius - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(lcy - lradius - 1.0f)));
    const int x1 = std::min(static_cast<int>(lw),
                            static_cast<int>(std::ceil(lcx + lradius + 1.0f)));
    const int y1 = std::min(static_cast<int>(lh),
                            static_cast<int>(std::ceil(lcy + lradius + 1.0f)));
    refreshPlacedRegion(l, QRect(x0, y0, x1 - x0, y1 - y0));
    if (!preserveInkColor)
        refreshLayerCmyk(*this, l, QRect(x0, y0, x1 - x0, y1 - y0));
    l.thumbnail = QImage();  // stale panel preview
}

void DocumentItem::refreshMaskRegion(LayerItem& l, const QRect& maskRect) {
    if (!l.mask) {
        ++l.maskStamp;
        return;
    }
    ensurePainter();
    const pittore::Image* mask = effectiveMaskImage(size, l);
    if (!mask || mask->data() != l.mask->data()) {
        ++l.maskStamp;
        return;
    }
    // The staged slot holds finished bytes when density/feather apply:
    // patch the finished cache in place instead of re-baking it whole.
    const pittore::Image* upload = mask;
    if (const pittore::Image* fin = finishedMaskFor(l)) {
        if (!patchFinishedMaskRegion(*mask, l.maskDensity, l.maskFeather,
                                     maskRect.left(), maskRect.top(),
                                     maskRect.left() + maskRect.width(),
                                     maskRect.top() + maskRect.height(),
                                     *const_cast<pittore::Image*>(fin))) {
            ++l.maskStamp;  // size mismatch: full re-bake on next gather
            return;
        }
        upload = fin;
    }
    const std::uint32_t w = upload->width();
    const std::uint32_t h = upload->height();
    const int x0 = std::max(0, maskRect.left());
    const int y0 = std::max(0, maskRect.top());
    const int x1 = std::min<std::uint32_t>(w, static_cast<std::uint32_t>(
        std::max(0, maskRect.left() + maskRect.width())));
    const int y1 = std::min<std::uint32_t>(h, static_cast<std::uint32_t>(
        std::max(0, maskRect.top() + maskRect.height())));
    if (x0 >= x1 || y0 >= y1) return;
    const void* key = static_cast<const void*>(mask->data());
    const std::size_t nbytes =
        static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    auto it = stage_->masks.find(key);
    if (it == stage_->masks.end() || !it->second.dev ||
        it->second.bytes != nbytes || it->second.stamp != l.maskStamp) {
        ++l.maskStamp;
        return;
    }
    PaintStage::PlacedSource& e = it->second;
    const auto* src = upload->data();
    auto* dst = static_cast<char*>(e.dev->host());
    const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(pittore::RGBAf);
    const std::size_t rowBytes =
        static_cast<std::size_t>(x1 - x0) * sizeof(pittore::RGBAf);
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * pitch +
                        static_cast<std::size_t>(x0) * sizeof(pittore::RGBAf),
                    src + static_cast<std::size_t>(y) * w + x0, rowBytes);
    e.dev->upload_region(w, static_cast<std::uint32_t>(x0),
                         static_cast<std::uint32_t>(y0),
                         static_cast<std::uint32_t>(x1),
                         static_cast<std::uint32_t>(y1));
}

// Cached device copy of an adjustment layer's aux table: 3x256 floats for
// Curves (R/G/B), 256 for Levels (see PaintStage::auxs).
const pittore::compute::Buffer* DocumentItem::adjAuxSourceFor(
    const LayerItem& l) {
    const std::size_t want = adjustmentAuxSize(l);
    if (want == 0 || l.adjustmentLUT.size() != want) return nullptr;
    ensurePainter();
    const void* key = static_cast<const void*>(l.adjustmentLUT.data());
    const std::size_t nbytes = want * sizeof(float);
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    auto it = stage_->auxs.find(key);
    if (it != stage_->auxs.end()) {
        PaintStage::PlacedSource& e = it->second;
        if (e.dev && e.stamp == l.adjustStamp && e.bytes == nbytes)
            return e.dev.get();
        if (!e.dev || e.bytes != nbytes) e.dev = be.make_buffer(nbytes);
        e.stamp = l.adjustStamp;
        e.rev = 0;
        e.bytes = nbytes;
        std::memcpy(e.dev->host(), l.adjustmentLUT.data(), nbytes);
        e.dev->upload();
        return e.dev.get();
    }
    auto dev = be.make_buffer(nbytes);
    std::memcpy(dev->host(), l.adjustmentLUT.data(), nbytes);
    dev->upload();
    auto ins = stage_->auxs.emplace(
        key, PaintStage::PlacedSource{key, l.adjustStamp, 0, nbytes,
                                      std::move(dev)});
    return ins.first->second.dev.get();
}

void DocumentItem::prunePlacedSources() {
    if (!stage_) return;
    std::vector<const void*> ownedPixels;
    std::vector<const void*> ownedMasks;
    ownedPixels.reserve(layers.size());
    ownedMasks.reserve(layers.size());
    for (const LayerItem& l : layers) {
        if (l.kind == LayerItem::Kind::Pixel && l.pixels)
            ownedPixels.push_back(static_cast<const void*>(l.pixels->data()));
        if (effectiveMaskImage(size, l))
            ownedMasks.push_back(
                static_cast<const void*>(l.mask->data()));
    }
    std::sort(ownedPixels.begin(), ownedPixels.end());
    std::sort(ownedMasks.begin(), ownedMasks.end());
    for (auto it = stage_->sources.begin(); it != stage_->sources.end();) {
        if (std::binary_search(ownedPixels.begin(), ownedPixels.end(),
                               it->first))
            ++it;
        else
            it = stage_->sources.erase(it);
    }
    for (auto it = stage_->masks.begin(); it != stage_->masks.end();) {
        if (std::binary_search(ownedMasks.begin(), ownedMasks.end(), it->first))
            ++it;
        else
            it = stage_->masks.erase(it);
    }
    for (auto it = stage_->finishedMasks.begin();
         it != stage_->finishedMasks.end();) {
        if (std::binary_search(ownedMasks.begin(), ownedMasks.end(), it->first))
            ++it;
        else
            it = stage_->finishedMasks.erase(it);
    }
    std::vector<const void*> ownedAux;
    ownedAux.reserve(layers.size());
    for (const LayerItem& l : layers) {
        if (adjustmentAuxSize(l) != 0 &&
            l.adjustmentLUT.size() == adjustmentAuxSize(l))
            ownedAux.push_back(
                static_cast<const void*>(l.adjustmentLUT.data()));
    }
    std::sort(ownedAux.begin(), ownedAux.end());
    for (auto it = stage_->auxs.begin(); it != stage_->auxs.end();) {
        if (std::binary_search(ownedAux.begin(), ownedAux.end(), it->first))
            ++it;
        else
            it = stage_->auxs.erase(it);
    }
}

void DocumentItem::gatherCompositedLayers(const QRect& region,
                                          std::vector<CompositedLayer>& out) {
    out.clear();
    if (region.isEmpty() || size.isEmpty()) return;
    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(size.width(), region.left() + region.width());
    const int y1 = qMin(size.height(), region.top() + region.height());
    if (x0 >= x1 || y0 >= y1) return;
    out.reserve(static_cast<std::size_t>(layers.size()));

    // Linear visibility + parent-group sweep shared by every row below.
    // Per-index effectivelyVisible()/parentGroupIndex() scan back O(n) rows
    // per call, turning this loop quadratic on multi-thousand-layer docs.
    QVector<char> effVis;
    QVector<int> effParent;
    effectiveVisibility(effVis, effParent);

    // Standard clipping groups: a clipped layer uses the nearest
    // included non-clipped pixel layer in the same parent scope as its base.
    // A base that exists but contributes nothing here (hidden or outside the
    // region) leaves the clipped layer invisible; only a layer with no base
    // anywhere acts as its own base.
    int baseOutPos = -1;
    int baseParent = -2;
    bool baseExists = false;
    for (int i = layers.size() - 1; i >= 0; --i) {
        LayerItem& l = layers[i];
        const int parent = effParent[i];
        const bool isAdjustment =
            l.kind == LayerItem::Kind::Adjustment && l.adjustmentKind != 0;
        if (l.kind != LayerItem::Kind::Pixel && !isAdjustment) {
            if (l.kind != LayerItem::Kind::Group &&
                effVis[i] && parent == baseParent) {
                baseOutPos = -1;
                baseExists = false;
            }
            continue;
        }
        if (!effVis[i]) {
            // A hidden pixel base still blocks the chain (its clipped
            // followers stay invisible); a hidden adjustment is simply absent.
            if (!isAdjustment && parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        if (isAdjustment) {
            // Live adjustments transform the composite-so-far across the
            // whole region. They never establish or break clip coverage, but
            // a clipped adjustment still resolves to the pixel base below it.
            bool clipped = false;
            int clipBase = -1;
            if (l.clipped) {
                if (baseOutPos >= 0 && baseParent == parent) {
                    clipped = true;
                    clipBase = baseOutPos;
                } else if (baseExists && baseParent == parent) {
                    clipped = true;
                    clipBase = -1;
                }
            }
            const pittore::Image* mask = effectiveMaskImage(size, l);
            QPointF maskOffset{0, 0};
            double maskScaleX = 1.0, maskScaleY = 1.0;
            if (mask) maskTransformFor(l, maskOffset, maskScaleX, maskScaleY);
            CompositedLayer e;
            e.layerIndex = i;
            e.mask = mask;
            e.maskOffset = maskOffset;
            e.maskScaleX = maskScaleX;
            e.maskScaleY = maskScaleY;
            e.clipped = clipped;
            e.clipBase = clipBase;
            e.isAdjustment = true;
            e.adjKind = l.adjustmentKind;
            for (int k = 0; k < 16; ++k)
                e.adjP[k] = l.adjustmentParams[k];
            e.adjAux = l.adjustmentLUT.size() == adjustmentAuxSize(l) &&
                             adjustmentAuxSize(l) != 0
                         ? l.adjustmentLUT.data()
                         : nullptr;
            e.fold = (l.opacity / 100.0f) * (l.fill / 100.0f);
            e.blendMode = l.blendMode;
            e.window = QRect(x0, y0, x1 - x0, y1 - y0);
            out.push_back(std::move(e));
            continue;
        }
        ensureLayerPixels(*this, l);
        if (!l.pixels || !effVis[i]) {
            if (parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        std::uint32_t rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        if (!gpuCompositeWindow(*this, l, static_cast<std::uint32_t>(x0),
                                static_cast<std::uint32_t>(y0),
                                static_cast<std::uint32_t>(x1),
                                static_cast<std::uint32_t>(y1), &rx0, &ry0,
                                &rx1, &ry1)) {
            if (parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        const LayerDrawSource src = layerDrawSource(l);
        if (!src.img) {
            if (parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        bool clipped = false;
        int clipBase = -1;
        if (l.clipped) {
            if (baseOutPos >= 0 && baseParent == parent) {
                clipped = true;
                clipBase = baseOutPos;
            } else if (baseExists && baseParent == parent) {
                clipped = true;
                clipBase = -1;
            } else {
                baseOutPos = static_cast<int>(out.size());
                baseParent = parent;
                baseExists = true;
            }
        } else {
            baseOutPos = static_cast<int>(out.size());
            baseParent = parent;
            baseExists = true;
        }
        const pittore::Image* mask = effectiveMaskImage(size, l);
        QPointF maskOffset{0, 0};
        double maskScaleX = 1.0, maskScaleY = 1.0;
        if (mask) maskTransformFor(l, maskOffset, maskScaleX, maskScaleY);
        CompositedLayer e;
        e.layerIndex = i;
        e.source = src;
        e.mask = mask;
        e.maskOffset = maskOffset;
        e.maskScaleX = maskScaleX;
        e.maskScaleY = maskScaleY;
        e.clipped = clipped;
        e.clipBase = clipBase;
        e.fold = (l.opacity / 100.0f) * (l.fill / 100.0f);
        e.blendMode = l.blendMode;
        e.window = QRect(static_cast<int>(rx0), static_cast<int>(ry0),
                         static_cast<int>(rx1 - rx0),
                         static_cast<int>(ry1 - ry0));
        out.push_back(std::move(e));
    }
}

void DocumentItem::rebuildComposite() {
    if (size.isEmpty()) return;
    ++revision;   // any composite changes the canvas's derived-geometry caches
    // --- Live Tone Blend Group scaffolding (shared by both backends) ------
    // A tone-blend header + its indent subtree form a span. Spans nest;
    // callers recurse so inner groups blend against the outer group's
    // work-in-progress (the correct nesting semantic). Headers that are
    // effectively invisible blend nothing (their children are hidden too).
    struct ToneSpan {
        int header = -1;
        int end = -1;
    };
    auto toneTokenEnd = [&](int start) {
        if (start < 0 || start >= layers.size()) return start;
        const int base = layers[start].indent;
        int end = start;
        while (end + 1 < layers.size() && layers[end + 1].indent > base) ++end;
        return end;
    };
    auto toneSpansIn = [&](int top, int bottom) {
        std::vector<ToneSpan> spans;
        for (int i = top; i <= bottom && i < layers.size();) {
            const LayerItem& l = layers[i];
            if (l.kind == LayerItem::Kind::Group && l.toneBlendGroup &&
                effectivelyVisible(i)) {
                const int end = toneTokenEnd(i);
                spans.push_back(ToneSpan{i, end});
                i = end + 1;
                continue;
            }
            ++i;
        }
        return spans;
    };
    auto hasVisibleToneBlendGroup = [&]() {
        bool any = false;
        for (const LayerItem& l : layers) {
            if (l.kind == LayerItem::Kind::Group && l.toneBlendGroup) {
                any = true;
                break;
            }
        }
        if (!any) return false;
        QVector<char> tv;
        QVector<int> tp;
        effectiveVisibility(tv, tp);
        for (int i = 0; i < layers.size(); ++i)
            if (layers[i].kind == LayerItem::Kind::Group &&
                layers[i].toneBlendGroup && tv[i])
                return true;
        return false;
    };
    const bool toneActive = hasVisibleToneBlendGroup();
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint32_t w = static_cast<std::uint32_t>(size.width());
    const std::uint32_t h = static_cast<std::uint32_t>(size.height());

    // Reuse the canvas across rebuilds (33MB at 4K): only realloc when the
    // size/format actually changed. Both paths overwrite every pixel.
    if (composite.size() != size ||
        composite.format() != QImage::Format_ARGB32_Premultiplied)
        composite = QImage(size, QImage::Format_ARGB32_Premultiplied);

    // Pixel layers are composited by the engine, bottom→top, straight alpha.
    // The accumulator starts fully transparent; each layer's opacity × fill is
    // folded into its alpha before the blend. Text / Shape / Adjustment layers
    // remain QPainter stand-ins drawn above the flattened pixels.
    ensurePainter();
    const bool gpu =
        backend && backend->type() != pittore::compute::BackendType::CPU;

    // GPU path: the accumulator lives on the device end to end. Clear it there,
    // sample/fold/blend EVERY pixel layer with the fused kernel — the 1:1
    // background included, since the sampler reads an integer-aligned texel
    // exactly — then hand the finished frame to the canvas with one device
    // convert + DMA. No host resample, no work buffer, no per-layer transfer.
    // Device failures (OOM included) fall through to the CPU reference
    // path below instead of terminating: a huge import on a small GPU
    // still opens.
    if (gpu && rebuildCompositeGPU(*backend, *stage_->acc, stage_->toneScratch,
                                   w, h, toneActive))
        return;

    // CPU reference path: host-authoritative accumulator. This uses the same
    // shared bottom→top row walk as the GPU batched kernel (masks, folds and
    // clip groups included), sampled straight from the live layer Images.
    composite.fill(Qt::transparent);
    auto* acc = static_cast<pittore::RGBAf*>(stage_->acc->host());
    std::fill_n(acc, static_cast<std::size_t>(w) * h, pittore::RGBAf{});
    const auto tfill = std::chrono::steady_clock::now();

    std::vector<CompositedLayer> gathered;
    gatherCompositedLayers(QRect(QPoint(0, 0), size), gathered);
    // Gathered-index slice for a panel range (gather preserves panel
    // order, so the slice is contiguous).
    auto gatheredRangeForPanels = [&](int plo, int phi) {
        std::size_t a = gathered.size(), b = 0;
        for (std::size_t j = 0; j < gathered.size(); ++j) {
            const int li = gathered[j].layerIndex;
            if (li >= plo && li <= phi) {
                a = std::min(a, j);
                b = std::max(b, j + 1);
            }
        }
        if (b <= a) return std::pair<std::size_t, std::size_t>{0, 0};
        return std::pair<std::size_t, std::size_t>{a, b};
    };
    // Slice converter with clipBase remapped to slice-relative positions
    // (out-of-slice bases degrade to unclipped, mirroring
    // composite_many_host's own validation).
    auto sliceViews = [&](std::size_t glo, std::size_t ghi) {
        std::vector<pittore::compute::HostPlacedLayer> views;
        if (ghi > glo) views.reserve(ghi - glo);
        for (std::size_t j = glo; j < ghi && j < gathered.size(); ++j) {
            const CompositedLayer& e = gathered[j];
            pittore::compute::HostPlacedLayer v;
            if (e.isAdjustment) {
                v.isAdjustment = true;
                v.adjKind = e.adjKind;
                for (int k = 0; k < 16; ++k) v.adjP[k] = e.adjP[k];
                v.adjAux = e.adjAux;
            } else {
                v.src = e.source.img->data();
                v.sw = e.source.img->width();
                v.sh = e.source.img->height();
                v.ox = e.source.offset.x();
                v.oy = e.source.offset.y();
                v.sx = e.source.scaleX;
                v.sy = e.source.scaleY;
            }
            // Finished coverage when density/feather apply, so the CPU
            // reference matches the device path byte-for-byte.
            const pittore::Image* fin =
                finishedMaskFor(layers[e.layerIndex]);
            const pittore::Image* mv = fin ? fin : e.mask;
            v.mask = mv ? mv->data() : nullptr;
            v.msw = mv ? mv->width() : 0;
            v.msh = mv ? mv->height() : 0;
            v.mox = e.maskOffset.x();
            v.moy = e.maskOffset.y();
            v.msx = e.maskScaleX;
            v.msy = e.maskScaleY;
            v.x0 = static_cast<std::uint32_t>(e.window.left());
            v.y0 = static_cast<std::uint32_t>(e.window.top());
            v.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
            v.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
            v.fold = e.fold;
            v.mode = engineBlendMode(e.blendMode);
            v.clipped = e.clipped;
            v.clipBase = e.clipBase;
            if (v.clipped && v.clipBase >= 0) {
                if (static_cast<std::size_t>(v.clipBase) >= glo &&
                    static_cast<std::size_t>(v.clipBase) < ghi)
                    v.clipBase -= static_cast<int>(glo);
                else {
                    v.clipped = false;
                    v.clipBase = -1;
                }
            }
            views.push_back(v);
        }
        return views;
    };
    if (!toneActive) {
        auto views = sliceViews(0, gathered.size());
        if (!views.empty())
            pittore::compute::composite_many_host(acc, w, h, 0, 0, w, h,
                                                   views.data(), views.size());
    } else {
        // Segmented composite mirroring the GPU path: tone-blend spans
        // composite their children into local temps (nesting-safe: every
        // recursion level owns its vectors), blend against a snapshot of
        // the accumulator below, and re-enter as one synthetic layer.
        const std::size_t n = static_cast<std::size_t>(w) * h;
        // Depth-indexed scratch pair per recursion level (mirrors the GPU
        // toneScratch): grown on first use, reused by sibling groups, never
        // freed in steady state. bdrop is fully overwritten by the acc copy;
        // grp is zeroed before each children composite (the walk blends over
        // it, so stale bytes would leak through).
        std::vector<std::vector<pittore::RGBAf>> toneHost;
        auto toneBuf = [&](std::size_t idx) -> pittore::RGBAf* {
            if (toneHost.size() <= idx) toneHost.resize(idx + 1);
            auto& v = toneHost[idx];
            if (v.size() != n) v.assign(n, pittore::RGBAf{});
            return v.data();
        };
        std::function<void(pittore::RGBAf*, int, int, int)> compCPU =
            [&](pittore::RGBAf* dst, int top, int bottom, int depth) {
                int g = -1, gend = -1;
                for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                    const auto cr = gatheredRangeForPanels(s.header + 1, s.end);
                    if (cr.second <= cr.first) continue;
                    if (s.header > g) {
                        g = s.header;
                        gend = s.end;
                    }
                }
                if (g < 0) {
                    const auto r = gatheredRangeForPanels(top, bottom);
                    if (r.second <= r.first) return;
                    auto slice = sliceViews(r.first, r.second);
                    if (!slice.empty())
                        pittore::compute::composite_many_host(
                            dst, w, h, 0, 0, w, h, slice.data(), slice.size());
                    return;
                }
                compCPU(dst, gend + 1, bottom, depth);  // everything below
                pittore::RGBAf* bdrop = toneBuf(std::size_t(2 * depth));
                std::memcpy(bdrop, acc, n * sizeof(pittore::RGBAf));  // backdrop
                pittore::RGBAf* grp = toneBuf(std::size_t(2 * depth + 1));
                std::fill_n(grp, n, pittore::RGBAf{});  // children
                compCPU(grp, g + 1, gend, depth + 1);
                const LayerItem& header = layers[g];
                pittore::compute::applyToneBlend(
                    grp, bdrop, grp, w, h,
                    header.toneBlend);
                pittore::compute::HostPlacedLayer synth;
                synth.src = grp;
                synth.sw = w;
                synth.sh = h;
                synth.ox = 0.0;
                synth.oy = 0.0;
                synth.sx = 1.0;
                synth.sy = 1.0;
                synth.x0 = 0;
                synth.y0 = 0;
                synth.x1 = w;
                synth.y1 = h;
                synth.fold = (header.opacity / 100.0f) *
                             (header.fill / 100.0f);
                synth.mode = engineBlendMode(header.blendMode);
                pittore::compute::composite_many_host(dst, w, h, 0, 0, w, h,
                                                       &synth, 1);
                compCPU(dst, top, g - 1, depth);  // everything above
            };
        compCPU(acc, 0, layers.size() - 1, 0);
    }
    const auto tcomp = std::chrono::steady_clock::now();

    blitRGBAfToPremul(acc, composite.bits(), 0, 0, w,
                        static_cast<std::uint32_t>(size.height()));
    const auto tblit = std::chrono::steady_clock::now();

    // (overlay QPainter pass shared with renderRegion)
    drawVectorOverlays();
    const auto tdone = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(tdone - t0)
                          .count();
    PITTORE_LOG("[render][rebuild] w=%u h=%u layers=%d backend=%s ms=%.2f", w, h,
                 static_cast<int>(layers.size()),
                 backend ? backend->name().c_str() : "cpu", ms);
    if (pittore::core::log::strokeTrace() ||
        ms >= pittore::core::log::slowEventMs())
        PITTORE_LOG(
            "[render][rebuild-breakdown] fill=%.3f composite=%.3f "
            "blit=%.3f overlay=%.3f stack=%s",
            std::chrono::duration<double, std::milli>(tfill - t0).count(),
            std::chrono::duration<double, std::milli>(tcomp - tfill).count(),
            std::chrono::duration<double, std::milli>(tblit - tcomp).count(),
            std::chrono::duration<double, std::milli>(tdone - tblit).count(),
            describeStack(gathered).toLocal8Bit().constData());
}

// Re-composite only the changed rect through the same shared bottom→top
// stack walk the full rebuild uses (masks, folds and clip groups included),
// then blit just those rows into the composite QImage. Overlay
// (Text/Shape/etc.) layers are redrawn over the region too, so the result is
// pixel-identical to a full rebuild wherever anything repainted.
void DocumentItem::renderRegion(const QRect& region) {
    if (region.isEmpty() || size.isEmpty()) return;
    if (composite.isNull()) {            // nothing rendered yet → full rebuild
        rebuildComposite();
        return;
    }
    // Live Tone Blend Groups read the backdrop through a blur with finite
    // footprint: expand the dirty rect by a halo and run a region-bounded
    // tone blend. Falls back to full rebuild only for huge regions.
    bool toneVisible = false;
    for (int i = 0; i < layers.size(); ++i) {
        if (layers[i].kind == LayerItem::Kind::Group &&
            layers[i].toneBlendGroup && effectivelyVisible(i)) {
            toneVisible = true;
            break;
        }
    }
    if (toneVisible) {
        renderRegionTone(region);
        return;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(size.width());
    const std::uint32_t h = static_cast<std::uint32_t>(size.height());
    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(static_cast<int>(w), region.left() + region.width());
    const int y1 = qMin(static_cast<int>(h), region.top() + region.height());
    if (x0 >= x1 || y0 >= y1) return;
    ++revision;   // a region edit changes the pixels the canvas draws
    const auto t0 = std::chrono::steady_clock::now();

    ensurePainter();
    const bool gpu =
        backend && backend->type() != pittore::compute::BackendType::CPU;

    // GPU path — the per-frame move/resize drag. The accumulator stays on the
    // device: clear it there, then sample + fold + blend every layer that
    // touches the dirty rect (the 1:1 background included) with the fused
    // kernel, and hand just the dirty rows to the canvas with a device convert
    // + DMA. The host never resamples a photo and never stages the work buffer.
    // Same guard as the full rebuild: device failure falls through to
    // the CPU path below instead of terminating.
    if (gpu && renderRegionGPU(*backend, *stage_->acc, x0, y0, x1, y1, w, h,
                                region))
        return;

    // CPU reference path: host-authoritative accumulator. Like the full
    // rebuild, this walks the same shared bottom→top stack instead of
    // staging one layer at a time.
    auto* acc = static_cast<pittore::RGBAf*>(stage_->acc->host());

    // Clear only the region of the accumulator.
    for (int y = y0; y < y1; ++y) {
        pittore::RGBAf* row = acc + static_cast<std::size_t>(y) * w + x0;
        std::fill(row, row + static_cast<std::size_t>(x1 - x0), pittore::RGBAf{});
    }
    const auto tclear = std::chrono::steady_clock::now();

    std::vector<CompositedLayer> gathered;
    gatherCompositedLayers(QRect(x0, y0, x1 - x0, y1 - y0), gathered);
    std::vector<pittore::compute::HostPlacedLayer> views;
    views.reserve(gathered.size());
    for (const CompositedLayer& e : gathered) {
        pittore::compute::HostPlacedLayer v;
        if (e.isAdjustment) {
            v.isAdjustment = true;
            v.adjKind = e.adjKind;
            for (int k = 0; k < 16; ++k) v.adjP[k] = e.adjP[k];
            v.adjAux = e.adjAux;
        } else {
            v.src = e.source.img->data();
            v.sw = e.source.img->width();
            v.sh = e.source.img->height();
            v.ox = e.source.offset.x();
            v.oy = e.source.offset.y();
            v.sx = e.source.scaleX;
            v.sy = e.source.scaleY;
        }
        // Finished coverage when density/feather apply, so the CPU
        // reference matches the device path byte-for-byte.
        const pittore::Image* fin = finishedMaskFor(layers[e.layerIndex]);
        const pittore::Image* mv = fin ? fin : e.mask;
        v.mask = mv ? mv->data() : nullptr;
        v.msw = mv ? mv->width() : 0;
        v.msh = mv ? mv->height() : 0;
        v.mox = e.maskOffset.x();
        v.moy = e.maskOffset.y();
        v.msx = e.maskScaleX;
        v.msy = e.maskScaleY;
        v.x0 = static_cast<std::uint32_t>(e.window.left());
        v.y0 = static_cast<std::uint32_t>(e.window.top());
        v.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
        v.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
        v.fold = e.fold;
        v.mode = engineBlendMode(e.blendMode);
        v.clipped = e.clipped;
        v.clipBase = e.clipBase;
        views.push_back(v);
    }
    pittore::compute::composite_many_host(
        acc, w, static_cast<std::uint32_t>(size.height()),
        static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
        static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(y1),
        views.data(), views.size());
    const auto tcomp = std::chrono::steady_clock::now();

    // Blit only the updated rows.
    for (int y = y0; y < y1; ++y) {
        uchar* dst = composite.bits() +
                     static_cast<qsizetype>(y) * composite.bytesPerLine() +
                     static_cast<qsizetype>(x0) * 4;
        blitRGBAfToPremul(acc + static_cast<std::size_t>(y) * w + x0, dst,
                          static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y),
                          static_cast<std::uint32_t>(x1 - x0), 1);
    }
    const auto tblit = std::chrono::steady_clock::now();

    drawVectorOverlays();
    const auto tdone = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(tdone - t0)
                          .count();
    if (pittore::core::log::strokeTrace() ||
        ms >= pittore::core::log::slowEventMs())
        PITTORE_LOG(
            "[render][region] incremental backend=%s region=(%d,%d,%d,%d) "
            "layers=%d clear=%.2f composite=%.2f blit=%.2f overlay=%.2f ms=%.2f",
            backend ? backend->name().c_str() : "cpu", region.x(), region.y(),
            region.width(), region.height(),
            static_cast<int>(layers.size()),
            std::chrono::duration<double, std::milli>(tclear - t0).count(),
            std::chrono::duration<double, std::milli>(tcomp - tclear).count(),
            std::chrono::duration<double, std::milli>(tblit - tcomp).count(),
            std::chrono::duration<double, std::milli>(tdone - tblit).count(),
            ms);
}

// Region-bounded tone composite for incremental repaints (erase/paint dabs
// inside a tone-blend group). Expands the dirty rect by a halo covering the
// tone pyramid blur footprint, composites below-slice + children over just
// that expanded region, runs tone_blend_region, and blits only the inner
// dirty rect. Falls back to full rebuild when the expanded region would
// cover most of the frame anyway.
void DocumentItem::renderRegionTone(const QRect& region) {
    if (region.isEmpty() || size.isEmpty()) return;
    if (composite.isNull()) {
        rebuildComposite();
        return;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(size.width());
    const std::uint32_t h = static_cast<std::uint32_t>(size.height());
    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(static_cast<int>(w), region.left() + region.width());
    const int y1 = qMin(static_cast<int>(h), region.top() + region.height());
    if (x0 >= x1 || y0 >= y1) return;
    ++revision;
    const auto t0 = std::chrono::steady_clock::now();

    ensurePainter();
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    const bool gpu =
        backend && backend->type() != pittore::compute::BackendType::CPU;

    // Halo must cover the tone pyramid blur footprint at full resolution.
    // Must match the halo in tone_blend_region (32px).
    constexpr int kToneHalo = 32;
    const int hx0 = qMax(0, x0 - kToneHalo);
    const int hy0 = qMax(0, y0 - kToneHalo);
    const int hx1 = qMin(static_cast<int>(w), x1 + kToneHalo);
    const int hy1 = qMin(static_cast<int>(h), y1 + kToneHalo);

    // If expanded region covers most of frame, full rebuild is simpler.
    const std::uint64_t expArea =
        static_cast<std::uint64_t>(hx1 - hx0) * (hy1 - hy0);
    const std::uint64_t frameArea = static_cast<std::uint64_t>(w) * h;
    if (expArea * 4 >= frameArea * 3) {
        rebuildComposite();
        return;
    }

    // Tone scaffolding (mirrors rebuildComposite).
    struct ToneSpan {
        int header = -1;
        int end = -1;
    };
    auto toneTokenEnd = [&](int start) {
        if (start < 0 || start >= layers.size()) return start;
        const int base = layers[start].indent;
        int end = start;
        while (end + 1 < layers.size() && layers[end + 1].indent > base) ++end;
        return end;
    };
    auto toneSpansIn = [&](int top, int bottom) {
        std::vector<ToneSpan> spans;
        for (int i = top; i <= bottom && i < layers.size();) {
            const LayerItem& l = layers[i];
            if (l.kind == LayerItem::Kind::Group && l.toneBlendGroup &&
                effectivelyVisible(i)) {
                const int end = toneTokenEnd(i);
                spans.push_back(ToneSpan{i, end});
                i = end + 1;
                continue;
            }
            ++i;
        }
        return spans;
    };

    if (gpu) {
        const auto tprep0 = std::chrono::steady_clock::now();
        // Clear the accumulator like the non-tone region path does. Only the
        // dirty region is blitted back to canvas, so pixels outside keep
        // their existing canvas content. Starting transparent is required
        // because composite_many_into blends over existing device bytes.
        be.clear(*stage_->acc);
        std::vector<CompositedLayer> gathered;
        gatherCompositedLayers(QRect(hx0, hy0, hx1 - hx0, hy1 - hy0), gathered);
        std::vector<pittore::compute::PlacedLayer> placed;
        placed.reserve(gathered.size());
        for (const CompositedLayer& e : gathered) {
            const LayerItem& l = layers[e.layerIndex];
            pittore::compute::PlacedLayer p;
            if (e.isAdjustment) {
                p.isAdjustment = true;
                p.adjKind = e.adjKind;
                for (int k = 0; k < 16; ++k) p.adjP[k] = e.adjP[k];
                p.adjAux = adjAuxSourceFor(l);
            } else {
                p.src = &placedSourceFor(l);
                p.sw = e.source.img->width();
                p.sh = e.source.img->height();
                p.ox = e.source.offset.x();
                p.oy = e.source.offset.y();
                p.sx = e.source.scaleX;
                p.sy = e.source.scaleY;
            }
            const pittore::compute::Buffer* maskBuf = maskSourceFor(l);
            p.mask = maskBuf;
            p.msw = e.mask ? static_cast<std::uint32_t>(e.mask->width()) : 0u;
            p.msh = e.mask ? static_cast<std::uint32_t>(e.mask->height()) : 0u;
            p.mox = e.maskOffset.x();
            p.moy = e.maskOffset.y();
            p.msx = e.maskScaleX;
            p.msy = e.maskScaleY;
            p.x0 = static_cast<std::uint32_t>(e.window.left());
            p.y0 = static_cast<std::uint32_t>(e.window.top());
            p.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
            p.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
            p.fold = e.fold;
            p.mode = engineBlendMode(e.blendMode);
            p.clipped = e.clipped;
            p.clipBase = e.clipBase;
            placed.push_back(p);
        }
        auto remapPlaced = [&](std::size_t glo, std::size_t ghi) {
            std::vector<pittore::compute::PlacedLayer> slice;
            if (ghi > glo) slice.reserve(ghi - glo);
            for (std::size_t j = glo; j < ghi && j < placed.size(); ++j) {
                auto p = placed[j];
                if (p.clipped && p.clipBase >= 0) {
                    if (static_cast<std::size_t>(p.clipBase) >= glo &&
                        static_cast<std::size_t>(p.clipBase) < ghi)
                        p.clipBase -= static_cast<int>(glo);
                    else {
                        p.clipped = false;
                        p.clipBase = -1;
                    }
                }
                slice.push_back(p);
            }
            return slice;
        };
        auto placedRangeForPanels = [&](int plo, int phi) {
            std::size_t a = placed.size(), b = 0;
            for (std::size_t j = 0; j < gathered.size() && j < placed.size();
                 ++j) {
                const int li = gathered[j].layerIndex;
                if (li >= plo && li <= phi) {
                    a = std::min(a, j);
                    b = std::max(b, j + 1);
                }
            }
            if (b <= a) return std::pair<std::size_t, std::size_t>{0, 0};
            return std::pair<std::size_t, std::size_t>{a, b};
        };
        // Region-bounded recursive composite. Unlike rebuildComposite's
        // compGPU (full frame), every composite_many_into and tone_blend
        // call here is bounded to [ux0,ux1) x [uy0,uy1). Scratch buffers
        // are full-frame (reused from stage_) but only the region is
        // written; the final synth composite targets just the inner dirty
        // rect of stage_->acc, preserving everything outside it.
        const std::uint32_t ux0 = static_cast<std::uint32_t>(hx0);
        const std::uint32_t uy0 = static_cast<std::uint32_t>(hy0);
        const std::uint32_t ux1 = static_cast<std::uint32_t>(hx1);
        const std::uint32_t uy1 = static_cast<std::uint32_t>(hy1);
        std::function<void(pittore::compute::Buffer&, int, int, int)> compGPURegion =
            [&](pittore::compute::Buffer& acc, int top, int bottom,
                int depth) {
                int g = -1, gend = -1;
                for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                    const auto cr =
                        placedRangeForPanels(s.header + 1, s.end);
                    if (cr.second <= cr.first) continue;
                    if (s.header > g) {
                        g = s.header;
                        gend = s.end;
                    }
                }
                if (g < 0) {
                    const auto r = placedRangeForPanels(top, bottom);
                    if (r.second <= r.first) return;
                    auto slice = remapPlaced(r.first, r.second);
                    if (!slice.empty())
                        be.composite_many_into(acc, w, ux0, uy0, ux1, uy1,
                                               slice.data(), slice.size());
                    return;
                }
                compGPURegion(acc, gend + 1, bottom, depth);
                const std::size_t need = std::size_t(2 * (depth + 1));
                while (stage_->toneScratch.size() < need)
                    stage_->toneScratch.push_back(
                        be.make_buffer(static_cast<std::size_t>(w) * h *
                                       sizeof(pittore::RGBAf)));
                pittore::compute::Buffer& grp =
                    *stage_->toneScratch[std::size_t(2 * depth)];
                pittore::compute::Buffer& bdrop =
                    *stage_->toneScratch[std::size_t(2 * depth + 1)];
                // Backdrop = below-slice composited over the expanded
                // region into scratch. Clear the region first since the
                // walk blends over existing device bytes.
                be.clear(bdrop);
                {
                    const auto r = placedRangeForPanels(gend + 1, bottom);
                    if (r.second > r.first) {
                        auto slice = remapPlaced(r.first, r.second);
                        if (!slice.empty())
                            be.composite_many_into(bdrop, w, ux0, uy0, ux1, uy1,
                                                   slice.data(), slice.size());
                    }
                }
                be.clear(grp);
                compGPURegion(grp, g + 1, gend, depth + 1);
                const LayerItem& header = layers[g];
                // Region tone transfer over the expanded rect (halo absorbs
                // pyramid edge effects); inner dirty rect is exact.
                be.tone_blend_region(grp, bdrop, w, h, ux0, uy0, ux1, uy1,
                                     header.toneBlend);
                pittore::compute::PlacedLayer synth;
                synth.src = &grp;
                synth.sw = w;
                synth.sh = h;
                synth.ox = 0.0;
                synth.oy = 0.0;
                synth.sx = 1.0;
                synth.sy = 1.0;
                synth.x0 = ux0;
                synth.y0 = uy0;
                synth.x1 = ux1;
                synth.y1 = uy1;
                synth.fold = (header.opacity / 100.0f) *
                             (header.fill / 100.0f);
                synth.mode = engineBlendMode(header.blendMode);
                // Composite the toned region back into the accumulator.
                // The region path preserves acc outside [x0,x1) x [y0,y1).
                be.composite_placed_into(acc, grp, w, h, 0.0, 0.0, 1.0, 1.0,
                                         w, ux0, uy0, ux1, uy1,
                                         synth.fold, synth.mode);
                compGPURegion(acc, top, g - 1, depth);
            };
        compGPURegion(*stage_->acc, 0, layers.size() - 1, 0);
        prunePlacedSources();
        // Blit just the inner dirty region (not halo) to canvas.
        uchar* dst = composite.bits() +
                     static_cast<qsizetype>(y0) * composite.bytesPerLine() +
                     static_cast<qsizetype>(x0) * 4;
        const auto tprep1 = std::chrono::steady_clock::now();
        be.blit_premul(*stage_->acc, dst, w, static_cast<std::uint32_t>(x0),
                       static_cast<std::uint32_t>(y0),
                       static_cast<std::uint32_t>(x1),
                       static_cast<std::uint32_t>(y1),
                       static_cast<std::size_t>(composite.bytesPerLine()));
        const auto tblit1 = std::chrono::steady_clock::now();
        drawVectorOverlays();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG(
                "[render][region-tone] backend=%s dirty=(%d,%d,%d,%d) "
                "layers=%d prep=%.2f blit=%.2f ms=%.2f",
                backend ? backend->name().c_str() : "cpu", x0, y0,
                x1 - x0, y1 - y0, static_cast<int>(layers.size()),
                std::chrono::duration<double, std::milli>(tprep1 - tprep0).count(),
                std::chrono::duration<double, std::milli>(tblit1 - tprep1).count(),
                ms);
        return;
    }

    // CPU reference path: region-bounded tone composite on the host
    // accumulator, mirroring compGPURegion above (same recursion, same
    // expanded-region contract) instead of a full rebuild. Scratch buffers
    // are full-frame but only the expanded region is written; only the inner
    // dirty rect is blitted back, so the canvas outside keeps its pixels.
    {
        const auto tprep0 = std::chrono::steady_clock::now();
        auto* acc = static_cast<pittore::RGBAf*>(stage_->acc->host());
        // Clear only the expanded region: the walk blends over existing
        // bytes, so the region must start transparent.
        for (int y = hy0; y < hy1; ++y)
            std::fill_n(acc + static_cast<std::size_t>(y) * w + hx0,
                        static_cast<std::size_t>(hx1 - hx0),
                        pittore::RGBAf{});
        std::vector<CompositedLayer> gathered;
        gatherCompositedLayers(QRect(hx0, hy0, hx1 - hx0, hy1 - hy0),
                               gathered);
        // Gathered-index slice for a panel range (gather preserves panel
        // order, so the slice is contiguous) — same as rebuildComposite.
        auto gatheredRangeForPanels = [&](int plo, int phi) {
            std::size_t a = gathered.size(), b = 0;
            for (std::size_t j = 0; j < gathered.size(); ++j) {
                const int li = gathered[j].layerIndex;
                if (li >= plo && li <= phi) {
                    a = std::min(a, j);
                    b = std::max(b, j + 1);
                }
            }
            if (b <= a) return std::pair<std::size_t, std::size_t>{0, 0};
            return std::pair<std::size_t, std::size_t>{a, b};
        };
        // Slice converter with clipBase remapped to slice-relative positions
        // — same as rebuildComposite (finished coverage when density/feather
        // apply, so the CPU reference matches the device path byte-for-byte).
        auto sliceViews = [&](std::size_t glo, std::size_t ghi) {
            std::vector<pittore::compute::HostPlacedLayer> views;
            if (ghi > glo) views.reserve(ghi - glo);
            for (std::size_t j = glo; j < ghi && j < gathered.size(); ++j) {
                const CompositedLayer& e = gathered[j];
                pittore::compute::HostPlacedLayer v;
                if (e.isAdjustment) {
                    v.isAdjustment = true;
                    v.adjKind = e.adjKind;
                    for (int k = 0; k < 16; ++k) v.adjP[k] = e.adjP[k];
                    v.adjAux = e.adjAux;
                } else {
                    v.src = e.source.img->data();
                    v.sw = e.source.img->width();
                    v.sh = e.source.img->height();
                    v.ox = e.source.offset.x();
                    v.oy = e.source.offset.y();
                    v.sx = e.source.scaleX;
                    v.sy = e.source.scaleY;
                }
                const pittore::Image* fin =
                    finishedMaskFor(layers[e.layerIndex]);
                const pittore::Image* mv = fin ? fin : e.mask;
                v.mask = mv ? mv->data() : nullptr;
                v.msw = mv ? mv->width() : 0;
                v.msh = mv ? mv->height() : 0;
                v.mox = e.maskOffset.x();
                v.moy = e.maskOffset.y();
                v.msx = e.maskScaleX;
                v.msy = e.maskScaleY;
                v.x0 = static_cast<std::uint32_t>(e.window.left());
                v.y0 = static_cast<std::uint32_t>(e.window.top());
                v.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
                v.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
                v.fold = e.fold;
                v.mode = engineBlendMode(e.blendMode);
                v.clipped = e.clipped;
                v.clipBase = e.clipBase;
                if (v.clipped && v.clipBase >= 0) {
                    if (static_cast<std::size_t>(v.clipBase) >= glo &&
                        static_cast<std::size_t>(v.clipBase) < ghi)
                        v.clipBase -= static_cast<int>(glo);
                    else {
                        v.clipped = false;
                        v.clipBase = -1;
                    }
                }
                views.push_back(v);
            }
            return views;
        };
        const std::uint32_t ux0 = static_cast<std::uint32_t>(hx0);
        const std::uint32_t uy0 = static_cast<std::uint32_t>(hy0);
        const std::uint32_t ux1 = static_cast<std::uint32_t>(hx1);
        const std::uint32_t uy1 = static_cast<std::uint32_t>(hy1);
        const std::size_t n = static_cast<std::size_t>(w) * h;
        // Depth-indexed scratch pair per recursion level (mirrors the full
        // rebuild's toneHost): grown on first use, reused by sibling groups.
        // Only the expanded region is written; grp is zeroed over the region
        // before each children composite (the walk blends over it, so stale
        // bytes would leak through).
        std::vector<std::vector<pittore::RGBAf>> toneHost;
        auto toneBuf = [&](std::size_t idx) -> pittore::RGBAf* {
            if (toneHost.size() <= idx) toneHost.resize(idx + 1);
            auto& v = toneHost[idx];
            if (v.size() != n) v.assign(n, pittore::RGBAf{});
            return v.data();
        };
        auto clearRegion = [&](pittore::RGBAf* buf) {
            for (int y = hy0; y < hy1; ++y)
                std::fill_n(buf + static_cast<std::size_t>(y) * w + hx0,
                            static_cast<std::size_t>(hx1 - hx0),
                            pittore::RGBAf{});
        };
        std::function<void(pittore::RGBAf*, int, int, int)> compCPURegion =
            [&](pittore::RGBAf* dst, int top, int bottom, int depth) {
                int g = -1, gend = -1;
                for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                    const auto cr =
                        gatheredRangeForPanels(s.header + 1, s.end);
                    if (cr.second <= cr.first) continue;
                    if (s.header > g) {
                        g = s.header;
                        gend = s.end;
                    }
                }
                if (g < 0) {
                    const auto r = gatheredRangeForPanels(top, bottom);
                    if (r.second <= r.first) return;
                    auto slice = sliceViews(r.first, r.second);
                    if (!slice.empty())
                        pittore::compute::composite_many_host(
                            dst, w, h, ux0, uy0, ux1, uy1, slice.data(),
                            slice.size());
                    return;
                }
                compCPURegion(dst, gend + 1, bottom, depth);  // below
                pittore::RGBAf* bdrop = toneBuf(std::size_t(2 * depth));
                clearRegion(bdrop);
                {
                    const auto r = gatheredRangeForPanels(gend + 1, bottom);
                    if (r.second > r.first) {
                        auto slice = sliceViews(r.first, r.second);
                        if (!slice.empty())
                            pittore::compute::composite_many_host(
                                bdrop, w, h, ux0, uy0, ux1, uy1, slice.data(),
                                slice.size());
                    }
                }
                pittore::RGBAf* grp = toneBuf(std::size_t(2 * depth + 1));
                clearRegion(grp);
                compCPURegion(grp, g + 1, gend, depth + 1);  // children
                const LayerItem& header = layers[g];
                // Region tone transfer over the expanded rect (the cached
                // level + pivot make the inner dirty rect exact).
                pittore::compute::applyToneBlendRegion(
                    grp, bdrop, grp, w, h, ux0, uy0, ux1, uy1,
                    header.toneBlend);
                pittore::compute::HostPlacedLayer synth;
                synth.src = grp;
                synth.sw = w;
                synth.sh = h;
                synth.ox = 0.0;
                synth.oy = 0.0;
                synth.sx = 1.0;
                synth.sy = 1.0;
                synth.x0 = ux0;
                synth.y0 = uy0;
                synth.x1 = ux1;
                synth.y1 = uy1;
                synth.fold = (header.opacity / 100.0f) *
                             (header.fill / 100.0f);
                synth.mode = engineBlendMode(header.blendMode);
                // Composite the toned region back; acc outside the expanded
                // region is preserved.
                pittore::compute::composite_many_host(dst, w, h, ux0, uy0,
                                                       ux1, uy1, &synth, 1);
                compCPURegion(dst, top, g - 1, depth);  // above
            };
        compCPURegion(acc, 0, layers.size() - 1, 0);
        // Blit just the inner dirty region (not halo) to canvas.
        const auto tprep1 = std::chrono::steady_clock::now();
        for (int y = y0; y < y1; ++y) {
            uchar* d = composite.bits() +
                       static_cast<qsizetype>(y) * composite.bytesPerLine() +
                       static_cast<qsizetype>(x0) * 4;
            blitRGBAfToPremul(acc + static_cast<std::size_t>(y) * w + x0, d,
                              static_cast<std::uint32_t>(x0),
                              static_cast<std::uint32_t>(y),
                              static_cast<std::uint32_t>(x1 - x0), 1);
        }
        const auto tblit1 = std::chrono::steady_clock::now();
        drawVectorOverlays();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG(
                "[render][region-tone] backend=cpu dirty=(%d,%d,%d,%d) "
                "layers=%d prep=%.2f blit=%.2f ms=%.2f",
                x0, y0, x1 - x0, y1 - y0, static_cast<int>(layers.size()),
                std::chrono::duration<double, std::milli>(tprep1 - tprep0).count(),
                std::chrono::duration<double, std::milli>(tblit1 - tprep1).count(),
                ms);
        return;
    }
}

QVector<int> DocumentItem::enclosingGroups(int index) const {
    QVector<int> out;
    if (index < 0 || index >= layers.size()) return out;
    int indent = layers[index].indent;
    for (int i = index - 1; i >= 0; --i) {
        const LayerItem& l = layers[i];
        if (l.indent >= indent) continue;   // same/deeper rows aren't ancestors
        if (l.kind != LayerItem::Kind::Group) break;   // a shallower non-group ends it
        out.append(i);                     // a shallower group IS an ancestor
        indent = l.indent;                 // keep climbing inside that group
    }
    return out;   // innermost group first, outermost last
}

bool DocumentItem::effectivelyVisible(int index) const {
    if (index < 0 || index >= layers.size() || !layers[index].visible) return false;
    for (int g : enclosingGroups(index))
        if (!layers[g].visible) return false;
    return true;
}

void DocumentItem::effectiveVisibility(QVector<char>& vis,
                                       QVector<int>& parent) const {
    const int n = layers.size();
    vis.resize(n);
    parent.resize(n);
    // Indent-stack sweep: leaders hold strictly decreasing indents, so the
    // stack top is always the nearest preceding row with smaller indent —
    // exactly what the scan-back in enclosingGroups() meets first. A
    // non-group top shields deeper followers from groups below it (the
    // scan-back `break`), which the walk below reproduces by stopping at it.
    struct Lead {
        int indent;
        int index;
        bool isGroup;
    };
    std::vector<Lead> st;
    st.reserve(16);
    for (int i = 0; i < n; ++i) {
        const LayerItem& l = layers[i];
        const int t = l.indent;
        while (!st.empty() && st.back().indent >= t) st.pop_back();
        int par = -1;
        bool chain = true;
        for (int s = static_cast<int>(st.size()) - 1; s >= 0; --s) {
            if (!st[s].isGroup) break;  // shield: scan-back stops here too
            if (par < 0) par = st[s].index;
            if (!layers[st[s].index].visible) chain = false;
        }
        vis[i] = (l.visible && chain) ? 1 : 0;
        parent[i] = par;
        st.push_back(Lead{t, i, l.kind == LayerItem::Kind::Group});
    }
}

bool DocumentItem::rowHiddenByCollapsedGroup(int index) const {
    if (index < 0 || index >= layers.size()) return false;
    for (int g : enclosingGroups(index))
        if (!layers[g].groupExpanded) return true;
    return false;
}

// Text / Shape / Adjustment stand-ins, drawn over the flattened pixels. Shared
// by the full rebuild and the incremental region path so both stay consistent.
void DocumentItem::drawVectorOverlays() {
    // Fast path: pixel-only documents have nothing to overlay. The loop below
    // would only construct a QPainter and skip every layer.
    bool any = false;
    for (int i = layers.size() - 1; i >= 0; --i) {
        const LayerItem& l = layers[i];
        if (l.kind != LayerItem::Kind::Pixel &&
            l.kind != LayerItem::Kind::Group && effectivelyVisible(i)) {
            any = true;
            break;
        }
    }
    if (!any) return;
    QPainter p(&composite);
    p.setRenderHint(QPainter::Antialiasing, true);

    for (int i = layers.size() - 1; i >= 0; --i) {
        const LayerItem& l = layers[i];
        if (!effectivelyVisible(i)) continue;
        if (l.kind == LayerItem::Kind::Pixel || l.kind == LayerItem::Kind::Group) continue;
        p.setOpacity(l.opacity / 100.0 * (l.fill / 100.0));

        switch (l.kind) {
            case LayerItem::Kind::Text: {
                QFont f = p.font();
                f.setPixelSize(qMax(24, size.height() / 12));
                p.setFont(f);
                p.setPen(l.swatch);
                p.drawText(composite.rect().adjusted(size.width() / 12, size.height() / 8, 0, 0),
                           Qt::AlignLeft | Qt::AlignTop, l.name);
                break;
            }
            case LayerItem::Kind::Shape: {
                p.setBrush(l.swatch);
                p.setPen(Qt::NoPen);
                const QRectF r(size.width() * 0.22, size.height() * 0.22,
                               size.width() * 0.4, size.height() * 0.4);
                p.drawRoundedRect(r, size.width() * 0.02, size.width() * 0.02);
                break;
            }
            case LayerItem::Kind::Adjustment: {
                // Legacy stand-ins (no live kind) keep their tint wash so an
                // old document still signals the row; live adjustments are
                // composited by the engine and draw nothing here.
                if (l.adjustmentKind == 0)
                    p.fillRect(composite.rect(),
                               QColor(l.swatch.red(), l.swatch.green(),
                                      l.swatch.blue(), 60));
                break;
            }
            default:
                break;
        }
    }
    p.setOpacity(1.0);
}

QString DocumentItem::statusText() const {
    const double mb = size.width() * double(size.height()) * 4.0 / (1024.0 * 1024.0);
    QString s = QStringLiteral("%1 × %2 px  ·  %3 ppi  ·  %4  ·  %5 MB")
                    .arg(size.width())
                    .arg(size.height())
                    .arg(dpi)
                    .arg(colorMode)
                    .arg(mb, 0, 'f', 1);
    if (isProxy)
        s += QStringLiteral("  ·  proxy 1/%1 of %2 × %3")
                 .arg(proxyFactor)
                 .arg(fullSize.width())
                 .arg(fullSize.height());
    return s;
}

// ---------------------------------------------------------------------------
// AppState
// ---------------------------------------------------------------------------
AppState::AppState(QObject* parent) : QObject(parent) {
    swatches_ = {QColor("#000000"), QColor("#3f3f3f"), QColor("#7f7f7f"), QColor("#bfbfbf"),
                 QColor("#ffffff"), QColor("#ff2600"), QColor("#ff9300"), QColor("#fffb00"),
                 QColor("#00f900"), QColor("#00fdff"), QColor("#0433ff"), QColor("#9437ff"),
                 QColor("#ff40ff"), QColor("#941100"), QColor("#945200"), QColor("#4f8f00"),
                 QColor("#005493"), QColor("#011993"), QColor("#531b93"), QColor("#8c2c85")};

    // Preferences: load persisted settings, adopt the stored theme as the live
    // theme_ (main.cpp applies the palette from state.theme() right after this),
    // and build the compute backend the settings call for.
    loadSettings(settings_);
    theme_ = settings_.theme;
    // Session snapping mirrors the persisted view settings so a restart
    // restores both the master toggle and the Snap To targets.
    snapEnabled_ = settings_.snapEnabled;
    snapTargets_ = settings_.snapTargets & SnapTargetsAll;
    DocumentItem::setMaxUndoSteps(settings_.undoLimit);
    backend_ = makeBackendFor(settings_);
}

AppState::~AppState() { qDeleteAll(documents_); }

void DocumentItem::setBackend(pittore::compute::ComputeBackend* be) {
    backend = be;
    stage_.reset();               // staging buffers belong to the old backend
    rebuildComposite();
}

pittore::compute::ComputeBackend& AppState::computeBackend() const {
    return backend_ ? *backend_ : cpuBackend();
}

QString AppState::computeDeviceLabel() const {
    const pittore::compute::Device& dev = computeBackend().device();
    return QString::fromLatin1(pittore::compute::to_string(dev.type)) +
           QStringLiteral(" · ") + QString::fromStdString(dev.name);
}

void AppState::applySettings(const AppSettings& next) {
    const bool computeChanged =
        next.gpuEnabled != settings_.gpuEnabled ||
        next.gpuDevice != settings_.gpuDevice ||
        next.cpuDevice != settings_.cpuDevice ||
        next.ramLimitMb != settings_.ramLimitMb;
    const bool snapChangedByPrefs =
        next.snapEnabled != snapEnabled_ ||
        (next.snapTargets & SnapTargetsAll) != snapTargets_;
    const bool proofChangedByPrefs =
        next.proofProfile != settings_.proofProfile ||
        next.proofIntent != settings_.proofIntent ||
        next.proofBpc != settings_.proofBpc;

    settings_ = next;
    // Shrinking the recent cap drops the tail immediately so the menu and
    // the start page agree without waiting for the next open.
    while (settings_.recentProjects.size() > qMax(0, settings_.recentMax))
        settings_.recentProjects.removeLast();
    saveSettings(settings_);

    DocumentItem::setMaxUndoSteps(settings_.undoLimit);

    if (snapChangedByPrefs) {
        snapEnabled_ = settings_.snapEnabled;
        snapTargets_ = settings_.snapTargets & SnapTargetsAll;
        emit snapChanged();
    }

    if (computeChanged) {
        backend_ = makeBackendFor(settings_);
        for (DocumentItem* d : documents_) d->setBackend(backend_.get());
        ::pittore::core::log::log_info(
            "[prefs] backend rebuilt: gpuEnabled=%d gpuDevice=%d cpuDevice=%d selected=%s",
            settings_.gpuEnabled ? 1 : 0, settings_.gpuDevice, settings_.cpuDevice,
            backend_ ? backend_->name().c_str() : "(null)");
    }
    if (settings_.theme != theme_) {
        theme_ = settings_.theme;
        emit themeChanged(theme_);
    }
    if (proofChangedByPrefs) emit proofChanged();
    emit settingsChanged();
}

const char* taskContextName(TaskContext c) {
    switch (c) {
        case TaskContext::None: return "None";
        case TaskContext::Selection: return "Selection";
        case TaskContext::Text: return "Text";
        case TaskContext::Crop: return "Crop";
        case TaskContext::Transform: return "Transform";
        case TaskContext::Path: return "Path";
        case TaskContext::ShapeLayer: return "ShapeLayer";
        case TaskContext::GenerativeResult: return "GenerativeResult";
    }
    return "?";
}

void AppState::setActiveTool(ToolId id, const char* reason) {
    if (activeTool_ == id) return;
    const ToolId previous = activeTool_;
    activeTool_ = id;

    // Per-tool setup trace. Everything below — and every toolChanged consumer
    // it wakes up — writes into id's own log file, so the tail of that file
    // names the exact stage a crash died in (see ui/tools/log/tool_log.h).
    ToolSetupTrace trace(id, reason);
    tool_logf(id, "state/set-active", "reason=%s previous=%s",
              reason ? reason : "switch", toolDef(previous).name);

    setStatusHint(QString::fromUtf8(toolDef(id).hint));
    tool_log(id, "state/status-hint", "hint set");

    tool_log(id, "state/emit-toolChanged", "before consumers");
    emit toolChanged(id);
    tool_log(id, "state/emit-toolChanged", "after consumers");

    // Tool switches redefine the task context (R20: a live crop or type edit is
    // a task, not just a tool).
    switch (id) {
        case ToolId::Crop:
        case ToolId::PerspectiveCrop:
            setTaskContext(TaskContext::Crop);
            break;
        case ToolId::HorizontalType:
        case ToolId::VerticalType:
        case ToolId::HorizontalTypeMask:
        case ToolId::VerticalTypeMask:
            setTaskContext(TaskContext::Text);
            break;
        case ToolId::Pen:
        case ToolId::FreeformPen:
        case ToolId::CurvaturePen:
        case ToolId::PathSelection:
        case ToolId::DirectSelection:
        case ToolId::NodeTool:
        case ToolId::PointTransformTool:
        case ToolId::CornerTool:
        case ToolId::ContourTool:
        case ToolId::StrokeWidthTool:
        case ToolId::KnifeTool:
        case ToolId::VectorBrushTool:
            setTaskContext(TaskContext::Path);
            break;
        default: {
            DocumentItem* d = activeDocument();
            setTaskContext(d && !d->selection.isEmpty() ? TaskContext::Selection
                                                        : TaskContext::None);
            break;
        }
    }
}

void AppState::cycleToolGroup(char key) {
    const ToolGroup* g = groupForKey(key);
    if (!g) return;
    const ToolGroup* current = groupForTool(activeTool_);
    setActiveTool(current == g ? nextInGroup(activeTool_) : g->leader, "cycle-group");
}

void AppState::pushTemporaryTool(ToolId id) {
    toolStack_.append(activeTool_);
    ++temporaryDepth_;
    setActiveTool(id, "temporary-push");
}

void AppState::popTemporaryTool() {
    if (toolStack_.isEmpty()) return;
    const ToolId previous = toolStack_.takeLast();
    if (temporaryDepth_ > 0) --temporaryDepth_;
    setActiveTool(previous, "temporary-pop");
}

QVariant AppState::option(ToolId tool, const QString& id) const {
    const QVariant stored =
        options_.value(QStringLiteral("%1/%2").arg(int(tool)).arg(id));
    if (stored.isValid()) return stored;
    // Unset options fall back to the registry's declared default, so painting
    // works before the user ever touches the options bar (brush_size, …).
    for (const OptionSpec& spec : optionsFor(tool))
        if (id == QString::fromUtf8(spec.id)) return spec.defaultValue;
    return QVariant();
}

void AppState::setOption(ToolId tool, const QString& id, const QVariant& value) {
    const QString key = QStringLiteral("%1/%2").arg(int(tool)).arg(id);
    if (options_.value(key) == value) return;
    setOptionSilently(tool, id, value);
    // The Type tools' options drive the text being typed: push them straight
    // through to the active live text layer so the canvas updates as the user
    // picks a family, size, alignment or colour.
    if (tool == ToolId::HorizontalType || tool == ToolId::VerticalType)
        applyTextOption(id, value);
}

void AppState::setOptionSilently(ToolId tool, const QString& id, const QVariant& value) {
    const QString key = QStringLiteral("%1/%2").arg(int(tool)).arg(id);
    if (options_.value(key) == value) return;
    options_.insert(key, value);
    emit optionChanged(tool, id, value);
}

void AppState::setForeground(const QColor& c) {
    if (foreground_ == c) return;
    foreground_ = c;
    emit colorsChanged();
}

void AppState::setBackground(const QColor& c) {
    if (background_ == c) return;
    background_ = c;
    emit colorsChanged();
}

void AppState::swapColors() {
    std::swap(foreground_, background_);
    emit colorsChanged();
}

void AppState::resetColors() {
    foreground_ = Qt::black;
    background_ = Qt::white;
    emit colorsChanged();
}

void AppState::addSwatch(const QColor& c) {
    if (swatches_.contains(c)) return;
    swatches_.append(c);
    emit swatchesChanged();
}

void AppState::setQuickMask(bool on) {
    if (quickMask_ == on) return;
    quickMask_ = on;
    emit quickMaskChanged(on);
}

void AppState::setScreenMode(ScreenMode m) {
    if (screenMode_ == m) return;
    screenMode_ = m;
    emit screenModeChanged(m);
}

void AppState::cycleScreenMode(bool reverse) {
    const int n = 3;
    int i = static_cast<int>(screenMode_);
    i = ((i + (reverse ? -1 : 1)) % n + n) % n;
    setScreenMode(static_cast<ScreenMode>(i));
}

void AppState::setChromeVisibility(ChromeVisibility v) {
    if (chrome_ == v) return;
    chrome_ = v;
    emit chromeVisibilityChanged(v);
}

void AppState::cycleSurround(bool reverse) {
    surround_ = ((surround_ + (reverse ? -1 : 1)) % 3 + 3) % 3;
    emit surroundChanged(surround_);
}

void AppState::setTheme(UiTheme t) {
    if (theme_ == t) return;
    theme_ = t;
    settings_.theme = t;
    saveSettings(settings_);
    emit themeChanged(t);
}

void AppState::setSnapEnabled(bool on) {
    if (snapEnabled_ == on && settings_.snapEnabled == on) return;
    snapEnabled_ = on;
    // Persist View > Snap toggles so they survive restarts. Lightweight:
    // no backend rebuild, just the snap broadcast (applySettings callers
    // sync the same fields without re-entering here).
    if (settings_.snapEnabled != on) {
        settings_.snapEnabled = on;
        saveSettings(settings_);
    }
    emit snapChanged();
}

void AppState::setSnapTargets(int targets) {
    targets &= SnapTargetsAll;
    if (snapTargets_ == targets && settings_.snapTargets == targets) return;
    snapTargets_ = targets;
    if (settings_.snapTargets != targets) {
        settings_.snapTargets = targets;
        saveSettings(settings_);
    }
    emit snapChanged();
}

void AppState::setSnapTarget(SnapTarget target, bool on) {
    setSnapTargets(on ? (snapTargets_ | target) : (snapTargets_ & ~target));
}

void AppState::setShowRulers(bool on) {
    if (settings_.showRulers == on) return;
    settings_.showRulers = on;
    saveSettings(settings_);
}

void AppState::setShowGuides(bool on) {
    if (settings_.showGuides == on) return;
    settings_.showGuides = on;
    saveSettings(settings_);
}

void AppState::setShowGrid(bool on) {
    if (settings_.showGrid == on) return;
    settings_.showGrid = on;
    saveSettings(settings_);
}

void AppState::setShowSelectionEdges(bool on) {
    if (settings_.showSelectionEdges == on) return;
    settings_.showSelectionEdges = on;
    saveSettings(settings_);
}

void AppState::setShowSmartGuides(bool on) {
    if (settings_.showSmartGuides == on) return;
    settings_.showSmartGuides = on;
    saveSettings(settings_);
}

void AppState::setShowPixelGrid(bool on) {
    if (settings_.showPixelGrid == on) return;
    settings_.showPixelGrid = on;
    saveSettings(settings_);
}

void AppState::setShowExtras(bool on) {
    if (settings_.showExtras == on) return;
    settings_.showExtras = on;
    saveSettings(settings_);
}

void AppState::setProofEnabled(bool on) {
    if (proofEnabled_ == on) return;
    proofEnabled_ = on;
    emit proofChanged();
}

void AppState::setProofGamut(bool on) {
    if (proofGamut_ == on) return;
    proofGamut_ = on;
    emit proofChanged();
}

DocumentItem* AppState::activeDocument() const {
    if (activeDocument_ < 0 || activeDocument_ >= documents_.size()) return nullptr;
    return documents_.at(activeDocument_);
}

void AppState::setActiveDocumentIndex(int index) {
    if (index == activeDocument_ || index < -1 || index >= documents_.size()) return;
    activeDocument_ = index;
    emit activeDocumentChanged(activeDocument());
    emit layersChanged();
    emit historyChanged();
    emit selectionChanged();
}

DocumentItem* AppState::addDocument(const QString& title, QSize size, int dpi) {
    // RAM limit gate: refuse documents whose initial working set (background
    // layer pixels + engine staging pair + composite QImage) exceeds the user's
    // budget, if one was set.
    if (settings_.ramLimitMb > 0 && !size.isEmpty()) {
        const double mb =
            static_cast<double>(documentWorkingSetBytes(size)) / (1024.0 * 1024.0);
        if (mb > settings_.ramLimitMb) {
            setStatusHint(tr("Document exceeds RAM limit (%1 MB > %2 MB)")
                               .arg(QString::number(mb, 'f', 1))
                               .arg(settings_.ramLimitMb));
            return nullptr;
        }
    }
    auto* doc = new DocumentItem(title, size, dpi);
    doc->resetHistory();
    doc->setBackend(backend_.get());
    documents_.append(doc);
    emit documentsChanged();
    setActiveDocumentIndex(documents_.size() - 1);
    return doc;
}

void AppState::closeDocument(int index) {
    if (index < 0 || index >= documents_.size()) return;
    delete documents_.takeAt(index);
    if (documents_.isEmpty()) {
        activeDocument_ = -1;
        emit documentsChanged();
        emit activeDocumentChanged(nullptr);
        emit layersChanged();
        emit historyChanged();
        return;
    }
    const int next = qBound(0, activeDocument_ >= index ? activeDocument_ - 1 : activeDocument_,
                            documents_.size() - 1);
    activeDocument_ = -1;
    emit documentsChanged();
    setActiveDocumentIndex(next);
}

DocumentItem* AppState::createNewProject(const ProjectFileData& data, QString* error) {
    const QString name = data.name.trimmed();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\')) || name == QLatin1String(".") ||
        name == QLatin1String("..")) {
        if (error) *error = tr("Enter a project name without / or \\.");
        return nullptr;
    }
    DocumentItem* doc = addDocument(name, data.size, data.dpi);
    if (!doc) {
        // The RAM-limit gate in addDocument already surfaced a status hint.
        if (error)
            *error = tr("Could not create the document (check the RAM limit in Preferences).");
        return nullptr;
    }
    doc->colorMode = data.colorMode;
    if (doc->colorMode.startsWith(QStringLiteral("CMYK"),
                                  Qt::CaseInsensitive))
        doc->iccProfile = resolveCmykProfile(*doc);

    // The canvas bottom erasing restores: white/black paper, or real
    // transparency the eraser punches through.
    if (data.background == QStringLiteral("transparent")) {
        doc->canvasTransparent = true;
    } else if (data.background == QStringLiteral("black")) {
        doc->canvasPaper = QColor(0, 0, 0);
    } else {
        doc->canvasPaper = QColor(255, 255, 255);
    }

    // Fill the document's background layer per the requested canvas bottom.
    LayerItem& bg = doc->layers.last();
    if (!bg.pixels) ensureLayerPixels(*doc, bg);
    if (bg.pixels) {
        if (data.background == QStringLiteral("transparent"))
            bg.pixels->fill(pittore::RGBAf{0, 0, 0, 0});
        else if (data.background == QStringLiteral("black"))
            bg.pixels->fill(pittore::RGBAf{0, 0, 0, 1});
        else
            bg.pixels->fill(pittore::RGBAf{1, 1, 1, 1});
        bg.swatch = data.background == QStringLiteral("black")
                        ? QColor(0, 0, 0)
                        : data.background == QStringLiteral("transparent")
                              ? QColor(0, 0, 0, 0)
                              : QColor(255, 255, 255);
        ++bg.sourceStamp;
        bg.thumbnail = QImage();
    }
    doc->dirty = false;
    doc->rebuildComposite();

    // Persist the project file immediately so it shows up on the start page.
    const QString path = projectPathForName(name);
    doc->filePath = path;
    QString saveErr;
    if (!saveProject(path, &saveErr)) {
        // The document is live regardless; a persistence failure should not
        // undo the creation, just tell the caller what happened.
        if (error)
            *error = tr("Project created but could not be saved:\n%1").arg(saveErr);
    }
    return doc;
}

void AppState::deriveCanvasPaper(DocumentItem& doc) {
    doc.canvasPaper = QColor(0xf2, 0xf2, 0xf2);
    doc.canvasTransparent = false;
    if (doc.layers.isEmpty()) return;
    const LayerItem& bg = doc.layers.last();
    if (!bg.pixels || bg.pixels->width() == 0 || bg.pixels->height() == 0)
        return;
    const pittore::RGBAf c = bg.pixels->data()[0];
    if (c.a < 0.5f) {
        doc.canvasTransparent = true;
    } else if (c.r < 0.5f && c.g < 0.5f && c.b < 0.5f) {
        doc.canvasPaper = QColor(0, 0, 0);
    } else if (c.r > 0.85f && c.g > 0.85f && c.b > 0.85f &&
               std::fabs(c.r - c.g) < 0.03f &&
               std::fabs(c.r - c.b) < 0.03f &&
               std::fabs(c.g - c.b) < 0.03f) {
        // Achromatic light gray (including the historic canvas gray):
        // keep the exact value so erasing restores the file's own
        // background. Chromatic corners fall through to white (the
        // conventional background-color behavior for photos).
        const int v = std::clamp(int(std::lround(c.r * 255.0f)), 0, 255);
        const int w = std::clamp(int(std::lround(c.g * 255.0f)), 0, 255);
        const int u = std::clamp(int(std::lround(c.b * 255.0f)), 0, 255);
        doc.canvasPaper = QColor(v, w, u);
    } else {
        doc.canvasPaper = QColor(255, 255, 255);
    }
}

void AppState::setTaskContext(TaskContext c) {
    if (context_ == c) return;
    context_ = c;
    // Lands in the active tool's own log: the contextual task bar rebuilds on
    // this signal, so this line says why.
    tool_log(activeTool_, "state/task-context", taskContextName(c));
    emit taskContextChanged(c);
}

void AppState::addPixelLayerFromImage(const QImage& img, const QString& name,
                                      const QPointF& offset, double scaleX,
                                      double scaleY, const QString& undoName,
                                      const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d || img.isNull()) return;
    auto pixels = imageFromQImage(img);
    if (!pixels) {
        setStatusHint(tr("Could not add layer (unsupported or too large)."));
        return;
    }
    if (!(scaleX > 0.0) || !qIsFinite(scaleX)) scaleX = 1.0;
    if (!(scaleY > 0.0) || !qIsFinite(scaleY)) scaleY = 1.0;
    d->beginUndoAction();
    LayerItem layer;
    layer.name = name.isEmpty() ? tr("Layer") : name;
    layer.kind = LayerItem::Kind::Pixel;
    layer.pixels = std::move(pixels);
    layer.offset = offset;
    layer.scaleX = scaleX;
    layer.scaleY = scaleY;
    addLayer(std::move(layer));
    d->commitUndoAction(undoName, iconKey);
    emit historyChanged();
}

void AppState::addLayer(LayerItem layer) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    const int at = qBound(0, d->activeLayer, d->layers.size());
    d->layers.insert(at, std::move(layer));
    d->activeLayer = at;
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
}

void AppState::removeLayerSilently(int index) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size()) return;
    d->layers.removeAt(index);
    d->activeLayer = qBound(0, d->activeLayer, d->layers.size() - 1);
    d->selectedLayers.clear();
    if (!d->layers.isEmpty()) d->selectedLayers.push_back(d->activeLayer);
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit documentModified(d);
}

void AppState::placeImageLayer(const QImage& img, const QString& name,
                               const QPointF& centerDoc, double scale) {
    DocumentItem* d = activeDocument();
    if (!d || img.isNull()) return;
    auto pixels = imageFromQImage(img);
    if (!pixels) {
        setStatusHint(tr("Could not place image (unsupported or too large)."));
        return;
    }
    if (!(scale > 0.0) || !qIsFinite(scale)) scale = 1.0;
    scale = qBound(0.01, scale, 100.0);
    LayerItem layer;
    layer.name = name.isEmpty() ? tr("Placed Image") : name;
    layer.kind = LayerItem::Kind::Pixel;
    layer.pixels = std::move(pixels);
    const double pw = img.width() * scale, ph = img.height() * scale;
    layer.offset = QPointF(centerDoc.x() - pw * 0.5, centerDoc.y() - ph * 0.5);
    layer.scaleX = layer.scaleY = scale;
    d->beginUndoAction();
    addLayer(std::move(layer));  // re-composites, selects, signals
    d->commitUndoAction(tr("Place"), QStringLiteral("move"));
    emit historyChanged();
}

void AppState::setActiveLayerIndex(int index) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size() || d->activeLayer == index) return;
    d->activeLayer = index;
    // Selection-only change: repaints the rows' highlight + the canvas gizmo
    // instead of rebuilding every row widget (thousands of SVG parts make a
    // full layersChanged rebuild far too slow for a plain click).
    emit activeLayerChanged();
}

LayerItem* AppState::activeLayer() const {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return nullptr;
    return &d->layers[qBound(0, d->activeLayer, d->layers.size() - 1)];
}

void AppState::setStatusHint(const QString& hint) {
    if (statusHint_ == hint) return;
    statusHint_ = hint;
    emit statusHintChanged(hint);
}

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

const char* AppState::generativeModelId() {
    // Catalogue id a bundled generative model would register under. None
    // exists yet (all catalogue entries are segmentation/matting), so the
    // store reports Absent and both call sites refuse honestly. When a
    // model lands, the audit gate below flips from SKIP-MODEL to tested.
    return "generative-fill";
}

bool AppState::generativeFill(bool background) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    const ToolId tool =
        background ? ToolId::GenerateBackground : ToolId::GenerativeFill;
    const QString prompt =
        option(tool, QStringLiteral("prompt")).toString().trimmed();
    if (prompt.isEmpty()) {
        setStatusHint(tr("Describe what to generate in the Prompt field."));
        return false;
    }
    if (!background && d->selection.isEmpty() && !d->selectionIsMask) {
        setStatusHint(tr("Generative Fill: mark an area first."));
        return false;
    }
    if (aiModelStore().state(QString::fromUtf8(generativeModelId())) !=
        AiModelState::Present) {
        setStatusHint(tr("Generative Fill needs a generative model "
                         "(none installed)."));
        return false;
    }
    return false;
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

bool AppState::resizeCanvas(const QPointF& newTopLeftDoc, const QSize& newSize) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    if (newSize.width() < 1 || newSize.height() < 1) return false;
    for (LayerItem& l : d->layers) {
        l.offset -= newTopLeftDoc;
        if (l.mask && !l.maskLinked) l.maskOffset -= newTopLeftDoc;
        l.thumbnail = QImage();
    }
    // The selection lives in document space, so it rides the same origin
    // shift as the layers: after a crop the selection still outlines the same
    // pixels, not a stale absolute rect. Its mask (document-sized) is rebased
    // by the same shift, and anything the resize pushed off-canvas is dropped
    // — it can no longer be reached by any tool.
    if (!d->selection.isEmpty()) {
        const QSize oldSize = d->size;
        const QRectF moved =
            d->selection.translated(-newTopLeftDoc).intersected(
                QRectF(QPointF(0, 0), QSizeF(newSize)));
        if (d->selectionIsMask && !d->selectionMask.isNull() &&
            d->selectionMask.size() == oldSize) {
            QImage rebased(newSize, QImage::Format_Grayscale8);
            rebased.fill(0);
            const int sx = qRound(-newTopLeftDoc.x());
            const int sy = qRound(-newTopLeftDoc.y());
            for (int y = 0; y < newSize.height(); ++y) {
                const int srcY = y + sy;
                if (srcY < 0 || srcY >= oldSize.height()) continue;
                uchar* row = rebased.scanLine(y);
                const uchar* src = d->selectionMask.constScanLine(srcY);
                for (int x = 0; x < newSize.width(); ++x) {
                    const int srcX = x + sx;
                    row[x] = (srcX >= 0 && srcX < oldSize.width()) ? src[srcX] : 0;
                }
            }
            d->selectionMask = std::move(rebased);
        }
        d->selection = moved;
        if (moved.isEmpty()) {
            d->selectionIsEllipse = false;
            d->selectionIsMask = false;
            d->selectionMask = QImage();
        }
        ++d->selectionStamp;
    }
    d->size = newSize;
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

// 3x3 homography inverse (adjugate / determinant). False when singular.
static bool invertHomography3(const double h[3][3], double inv[3][3]) {
    const double det =
        h[0][0] * (h[1][1] * h[2][2] - h[1][2] * h[2][1]) -
        h[0][1] * (h[1][0] * h[2][2] - h[1][2] * h[2][0]) +
        h[0][2] * (h[1][0] * h[2][1] - h[1][1] * h[2][0]);
    if (std::fabs(det) <= 1e-12) return false;
    inv[0][0] = (h[1][1] * h[2][2] - h[1][2] * h[2][1]) / det;
    inv[0][1] = (h[0][2] * h[2][1] - h[0][1] * h[2][2]) / det;
    inv[0][2] = (h[0][1] * h[1][2] - h[0][2] * h[1][1]) / det;
    inv[1][0] = (h[1][2] * h[2][0] - h[1][0] * h[2][2]) / det;
    inv[1][1] = (h[0][0] * h[2][2] - h[0][2] * h[2][0]) / det;
    inv[1][2] = (h[0][2] * h[1][0] - h[0][0] * h[1][2]) / det;
    inv[2][0] = (h[1][0] * h[2][1] - h[1][1] * h[2][0]) / det;
    inv[2][1] = (h[0][1] * h[2][0] - h[0][0] * h[2][1]) / det;
    inv[2][2] = (h[0][0] * h[1][1] - h[0][1] * h[1][0]) / det;
    return true;
}

// Apply a homography to (u, v); identity fallback on w == 0.
static void applyHomography3(const double h[3][3], double u, double v,
                             double& sx, double& sy) {
    const double w = h[2][0] * u + h[2][1] * v + h[2][2];
    if (std::fabs(w) <= 1e-12) {
        sx = u;
        sy = v;
        return;
    }
    sx = (h[0][0] * u + h[0][1] * v + h[0][2]) / w;
    sy = (h[1][0] * u + h[1][1] * v + h[1][2]) / w;
}

bool AppState::perspectiveCrop(const QPointF quad[4], int outW, int outH) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    // Reject degenerate quads (zero area or bowtie): signed area must clear
    // a pixel.
    double area2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF& p = quad[i];
        const QPointF& q = quad[(i + 1) % 4];
        area2 += p.x() * q.y() - q.x() * p.y();
    }
    if (std::fabs(area2) < 2.0) {
        setStatusHint(tr("Perspective Crop: the quad is degenerate."));
        return false;
    }
    // Output size: explicit W/H win, else the quad bbox.
    double qx0 = quad[0].x(), qy0 = quad[0].y(), qx1 = quad[0].x(),
           qy1 = quad[0].y();
    for (int i = 1; i < 4; ++i) {
        qx0 = std::min(qx0, quad[i].x());
        qy0 = std::min(qy0, quad[i].y());
        qx1 = std::max(qx1, quad[i].x());
        qy1 = std::max(qy1, quad[i].y());
    }
    const int ow = outW > 0 ? outW : std::max(1, int(std::round(qx1 - qx0)));
    const int oh = outH > 0 ? outH : std::max(1, int(std::round(qy1 - qy0)));
    if (ow < 1 || oh < 1 || ow > 16000 || oh > 16000) {
        setStatusHint(tr("Perspective Crop: bad output size."));
        return false;
    }
    // Homography quad -> output rect, via DLT + partial-pivot Gaussian
    // elimination (double precision, deterministic).
    double H[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    {
        const double dst[4][2] = {{0, 0},
                                  {double(ow), 0},
                                  {double(ow), double(oh)},
                                  {0, double(oh)}};
        double m[8][9] = {};
        for (int i = 0; i < 4; ++i) {
            const double x = quad[i].x(), y = quad[i].y();
            const double u = dst[i][0], v = dst[i][1];
            m[2 * i][0] = x;
            m[2 * i][1] = y;
            m[2 * i][2] = 1;
            m[2 * i][6] = -u * x;
            m[2 * i][7] = -u * y;
            m[2 * i][8] = u;
            m[2 * i + 1][3] = x;
            m[2 * i + 1][4] = y;
            m[2 * i + 1][5] = 1;
            m[2 * i + 1][6] = -v * x;
            m[2 * i + 1][7] = -v * y;
            m[2 * i + 1][8] = v;
        }
        for (int c = 0; c < 8; ++c) {
            int piv = c;
            for (int r = c + 1; r < 8; ++r)
                if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
            if (std::fabs(m[piv][c]) < 1e-12) {
                setStatusHint(tr("Perspective Crop: the quad is degenerate."));
                return false;
            }
            if (piv != c)
                for (int k = c; k < 9; ++k) std::swap(m[c][k], m[piv][k]);
            const double div = m[c][c];
            for (int k = c; k < 9; ++k) m[c][k] /= div;
            for (int r = 0; r < 8; ++r) {
                if (r == c) continue;
                const double f = m[r][c];
                if (f == 0.0) continue;
                for (int k = c; k < 9; ++k) m[r][k] -= f * m[c][k];
            }
        }
        H[0][0] = m[0][8];
        H[0][1] = m[1][8];
        H[0][2] = m[2][8];
        H[1][0] = m[3][8];
        H[1][1] = m[4][8];
        H[1][2] = m[5][8];
        H[2][0] = m[6][8];
        H[2][1] = m[7][8];
        H[2][2] = 1.0;
    }
    auto sampleBilinear = [](const pittore::Image* img, double x, double y) {
        const std::uint32_t w = img->width(), h = img->height();
        const int x0 = std::clamp(int(std::floor(x)), 0, int(w) - 1);
        const int y0 = std::clamp(int(std::floor(y)), 0, int(h) - 1);
        const int x1 = std::min(x0 + 1, int(w) - 1);
        const int y1 = std::min(y0 + 1, int(h) - 1);
        const float fx = float(std::clamp(x - x0, 0.0, 1.0));
        const float fy = float(std::clamp(y - y0, 0.0, 1.0));
        const pittore::RGBAf& a = img->data()[std::size_t(y0) * w + x0];
        const pittore::RGBAf& b = img->data()[std::size_t(y0) * w + x1];
        const pittore::RGBAf& c = img->data()[std::size_t(y1) * w + x0];
        const pittore::RGBAf& e = img->data()[std::size_t(y1) * w + x1];
        pittore::RGBAf o;
        o.r = (a.r * (1 - fx) + b.r * fx) * (1 - fy) +
              (c.r * (1 - fx) + e.r * fx) * fy;
        o.g = (a.g * (1 - fx) + b.g * fx) * (1 - fy) +
              (c.g * (1 - fx) + e.g * fx) * fy;
        o.b = (a.b * (1 - fx) + b.b * fx) * (1 - fy) +
              (c.b * (1 - fx) + e.b * fx) * fy;
        o.a = (a.a * (1 - fx) + b.a * fx) * (1 - fy) +
              (c.a * (1 - fx) + e.a * fx) * fy;
        return o;
    };
    double invH[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (!invertHomography3(H, invH)) {
        setStatusHint(tr("Perspective Crop: the quad is degenerate."));
        return false;
    }
    beginUndoStep();
    bool vectorMoved = false;
    for (LayerItem& l : d->layers) {
        if (l.kind != LayerItem::Kind::Pixel || !l.pixels) {
            // Non-pixel art cannot warp through the homography: keep it
            // visible at the quad origin (hinted below).
            if ((l.art && !l.art->isEmpty()) || l.liveText) {
                l.offset = QPointF(0, 0);
                vectorMoved = true;
            }
            l.thumbnail = QImage();
            continue;
        }
        // Fresh output images: the undo snapshot keeps sharing the old
        // pixels untouched, so no copy-on-write dance is needed here.
        const double lsx = std::max(l.scaleX, 1e-6);
        const double lsy = std::max(l.scaleY, 1e-6);
        auto warped = std::make_shared<pittore::Image>(std::uint32_t(ow),
                                                        std::uint32_t(oh));
        // Inverse-map each output texel: output -> doc (inverse H) ->
        // layer px. Rows are disjoint: parallel-safe.
        pittore::core::parallel_rows(
            std::uint32_t(oh), [&](std::uint32_t y0, std::uint32_t y1) {
                for (std::uint32_t y = y0; y < y1; ++y) {
                    for (int x = 0; x < ow; ++x) {
                        const double u = x + 0.5, v = y + 0.5;
                        // Inverse-map the output texel (shared
                        // precomputed inverse).
                        double sx = 0.0, sy = 0.0;
                        applyHomography3(invH, u, v, sx, sy);
                        const double lx = (sx - l.offset.x()) / lsx;
                        const double ly = (sy - l.offset.y()) / lsy;
                        warped->data()[std::size_t(y) * ow + x] =
                            sampleBilinear(l.pixels.get(), lx - 0.5,
                                           ly - 0.5);
                    }
                }
            });
        l.pixels = std::move(warped);
        l.offset = QPointF(0, 0);
        l.scaleX = 1.0;
        l.scaleY = 1.0;
        if (l.mask && l.maskLinked) {
            // Linked masks ride the layer frame: same inverse map.
            auto wmask = std::make_shared<pittore::Image>(std::uint32_t(ow),
                                                           std::uint32_t(oh));
            pittore::core::parallel_rows(
                std::uint32_t(oh), [&](std::uint32_t y0, std::uint32_t y1) {
                    for (std::uint32_t y = y0; y < y1; ++y)
                        for (int x = 0; x < ow; ++x) {
                            const double u = x + 0.5, v = y + 0.5;
                            double sx = 0.0, sy = 0.0;
                            applyHomography3(invH, u, v, sx, sy);
                            const double lx =
                                (sx - l.offset.x()) / lsx - 0.5;
                            const double ly =
                                (sy - l.offset.y()) / lsy - 0.5;
                            wmask->data()[std::size_t(y) * ow + x] =
                                sampleBilinear(l.mask.get(), lx, ly);
                        }
                });
            l.mask = std::move(wmask);
        }
        // Unlinked masks keep their frozen placement (documented).
        l.thumbnail = QImage();
    }
    const int res =
        option(ToolId::PerspectiveCrop, QStringLiteral("res")).toInt();
    if (res > 0) d->dpi = res;
    resizeCanvas(QPointF(0, 0), QSize(ow, oh));
    commitUndoStep(tr("Perspective Crop"), QStringLiteral("crop"));
    if (vectorMoved)
        setStatusHint(tr("Perspective Crop: vector layers repositioned, "
                         "not warped."));
    else
        setStatusHint(tr("Perspective Crop applied."));
    return true;
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

bool AppState::traceSelection(int output, int detail) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    if (d->selection.isEmpty() && !d->selectionIsMask) {
        setStatusHint(tr("Tracing: select an area first."));
        return false;
    }
    if (std::clamp(output, 0, 2) == 0) {
        setStatusHint(tr("Tracing: work-path storage is planned."));
        return false;
    }
    // Source polygon in document pixels: rect corners, a 64-gon ellipse,
    // or the largest marching-squares loop of the mask channel.
    std::vector<QPointF> poly;
    if (d->selectionIsMask && !d->selectionMask.isNull()) {
        const QPainterPath outline = pittore::ui::selectionOutlineFromMask(
            d->selectionMask,
            d->selection.toAlignedRect().adjusted(-1, -1, 1, 1));
        double bestArea = -1.0;
        for (const QPolygonF& sub : outline.toSubpathPolygons()) {
            if (sub.size() < 3) continue;
            const QRectF bb = sub.boundingRect();
            const double area = bb.width() * bb.height();
            if (area > bestArea) {
                bestArea = area;
                poly.assign(sub.begin(), sub.end());
            }
        }
    } else if (d->selectionIsEllipse) {
        const QPointF ctr = d->selection.center();
        const double rx = d->selection.width() / 2.0;
        const double ry = d->selection.height() / 2.0;
        if (rx > 0.0 && ry > 0.0) {
            for (int i = 0; i < 64; ++i) {
                const double a = 2.0 * 3.14159265358979323846 * i / 64.0;
                poly.emplace_back(ctr.x() + rx * std::cos(a),
                                  ctr.y() + ry * std::sin(a));
            }
        }
    } else {
        const QRectF& r = d->selection;
        poly = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    }
    if (poly.size() < 3) {
        setStatusHint(tr("Tracing: nothing traceable selected."));
        return false;
    }
    // Douglas-Peucker simplification, explicit stack (no recursion depth).
    // Epsilon scales with (100 - detail): 100 keeps every vertex.
    const double eps = (100.0 - std::clamp(detail, 0, 100)) * 0.05;
    if (eps > 1e-9 && poly.size() > 4) {
        std::vector<char> keep(poly.size(), 0);
        keep.front() = 1;
        keep.back() = 1;
        std::vector<std::pair<int, int>> stack{{0, int(poly.size()) - 1}};
        while (!stack.empty()) {
            const auto [s, e] = stack.back();
            stack.pop_back();
            const QPointF a = poly[std::size_t(s)], b = poly[std::size_t(e)];
            const double dx = b.x() - a.x(), dy = b.y() - a.y();
            const double len = std::hypot(dx, dy);
            double dmax = -1.0;
            int imax = -1;
            for (int i = s + 1; i < e; ++i) {
                const double dist =
                    len > 1e-12
                        ? std::fabs((poly[std::size_t(i)].x() - a.x()) * dy -
                                    (poly[std::size_t(i)].y() - a.y()) * dx) /
                              len
                        : std::hypot(poly[std::size_t(i)].x() - a.x(),
                                     poly[std::size_t(i)].y() - a.y());
                if (dist > dmax) {
                    dmax = dist;
                    imax = i;
                }
            }
            if (imax >= 0 && dmax > eps) {
                keep[std::size_t(imax)] = 1;
                stack.emplace_back(s, imax);
                stack.emplace_back(imax, e);
            }
        }
        std::vector<QPointF> simp;
        for (std::size_t i = 0; i < poly.size(); ++i)
            if (keep[i]) simp.push_back(poly[i]);
        if (simp.size() >= 3) poly = std::move(simp);
    }
    const bool closePath =
        option(ToolId::ContentAwareTracing, QStringLiteral("close_path"))
            .toBool();
    if (output == 2) {
        // Polygon vector layer (fill only; stroke comes from the bar).
        auto node = std::make_shared<pittore::vector::ArtNode>();
        node->name = "Trace";
        bool first = true;
        for (const QPointF& p : poly) {
            pittore::vector::Segment s;
            s.kind = first ? pittore::vector::Segment::Kind::MoveTo
                           : pittore::vector::Segment::Kind::LineTo;
            s.x = float(p.x());
            s.y = float(p.y());
            node->segments.push_back(s);
            first = false;
        }
        if (closePath) {
            pittore::vector::Segment c;
            c.kind = pittore::vector::Segment::Kind::Close;
            node->segments.push_back(c);
        }
        const QColor fg = foreground();
        node->paint.hasFill = true;
        node->paint.fill[0] = std::uint8_t(fg.red());
        node->paint.fill[1] = std::uint8_t(fg.green());
        node->paint.fill[2] = std::uint8_t(fg.blue());
        node->paint.fill[3] = 255;
        double bx0 = poly.front().x(), by0 = poly.front().y();
        double bx1 = bx0, by1 = by0;
        for (const QPointF& p : poly) {
            bx0 = std::min(bx0, p.x());
            by0 = std::min(by0, p.y());
            bx1 = std::max(bx1, p.x());
            by1 = std::max(by1, p.y());
        }
        return commitArtNodeLayer(
            std::move(node), QRectF(QPointF(bx0, by0), QPointF(bx1, by1)),
            ToolId::ContentAwareTracing, tr("Trace"), true,
            QStringLiteral("Normal"));
    }
    // Output 1: even-odd fill of the (implicitly closed) polygon back
    // into a document mask.
    QImage mask(QSize(d->size.width(), d->size.height()),
                QImage::Format_Grayscale8);
    mask.fill(0);
    double bx0 = poly.front().x(), by0 = poly.front().y();
    double bx1 = bx0, by1 = by0;
    for (const QPointF& p : poly) {
        bx0 = std::min(bx0, p.x());
        by0 = std::min(by0, p.y());
        bx1 = std::max(bx1, p.x());
        by1 = std::max(by1, p.y());
    }
    const int ix0 = std::max(0, int(std::floor(bx0)));
    const int iy0 = std::max(0, int(std::floor(by0)));
    const int ix1 = std::min(mask.width(), int(std::ceil(bx1)));
    const int iy1 = std::min(mask.height(), int(std::ceil(by1)));
    auto inside = [&](double x, double y) {
        bool in = false;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size();
             j = i++) {
            const QPointF& a = poly[i];
            const QPointF& b = poly[j];
            if ((a.y() > y) != (b.y() > y) &&
                x < (b.x() - a.x()) * (y - a.y()) / (b.y() - a.y()) + a.x())
                in = !in;
        }
        return in;
    };
    for (int y = iy0; y < iy1; ++y) {
        uchar* row = mask.scanLine(y);
        for (int x = ix0; x < ix1; ++x)
            if (inside(x + 0.5, y + 0.5)) row[x] = 255;
    }
    replaceSelectionMask(std::move(mask), tr("Trace"),
                         QStringLiteral("trace"));
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

bool AppState::floodEdit(const QPointF& docPos, bool erase) {
    const ToolId tool = activeTool();
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;

    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int sx = static_cast<int>(
        std::floor((docPos.x() - layer->offset.x()) / lsx));
    const int sy = static_cast<int>(
        std::floor((docPos.y() - layer->offset.y()) / lsy));

    // Snapshot + private pixel copy before mutating, exactly like a paint
    // stroke, so undo restores the pre-fill pixels.
    beginUndoStep();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector: drop the pre-COW
    // pointer and re-resolve so the fill lands on the live copy, not the
    // snapshot's shared image.
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

    const std::uint32_t lw = layer->pixels->width();
    const std::uint32_t lh = layer->pixels->height();
    if (lw == 0 || lh == 0 || sx < 0 || sy < 0 ||
        sx >= static_cast<int>(lw) || sy >= static_cast<int>(lh)) {
        discardUndoStep();
        return false;
    }

    // The region is measured in the layer's own pixel space. With Sample All
    // Layers it is measured through the document composite: each layer texel is
    // looked up at its document position (nearest neighbour — the composite is
    // already a resampled render, so a further filter would only blur it).
    std::vector<pittore::RGBAf> matchBuf;
    const pittore::RGBAf* match = layer->pixels->data();
    if (option(tool, QStringLiteral("sample_all")).toBool() &&
        !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        matchBuf.resize(static_cast<std::size_t>(lw) * lh);
        for (std::uint32_t y = 0; y < lh; ++y) {
            const double docY = layer->offset.y() + (static_cast<double>(y) + 0.5) * lsy;
            const int py = std::clamp(static_cast<int>(std::floor(docY)), 0,
                                      comp.height() - 1);
            const QRgb* row = reinterpret_cast<const QRgb*>(comp.constScanLine(py));
            for (std::uint32_t x = 0; x < lw; ++x) {
                const double docX =
                    layer->offset.x() + (static_cast<double>(x) + 0.5) * lsx;
                const int px = std::clamp(static_cast<int>(std::floor(docX)), 0,
                                          comp.width() - 1);
                const QRgb c = row[px];
                const int a = qAlpha(c);
                pittore::RGBAf& dst =
                    matchBuf[static_cast<std::size_t>(y) * lw + x];
                if (a <= 0) {
                    dst = pittore::RGBAf{0, 0, 0, 0};
                } else {
                    const float inv = 1.0f / static_cast<float>(a);
                    dst = pittore::RGBAf{qRed(c) * inv, qGreen(c) * inv,
                                          qBlue(c) * inv,
                                          static_cast<float>(a) / 255.0f};
                }
            }
        }
        match = matchBuf.data();
    }

    const float tol = static_cast<float>(std::clamp(
        option(tool, QStringLiteral("tolerance")).toDouble() / 255.0, 0.0, 1.0));
    const bool contiguous = option(tool, QStringLiteral("contiguous")).toBool();
    const bool antialias = option(tool, QStringLiteral("antialias")).toBool();
    const float opacity = static_cast<float>(std::clamp(
        option(tool, QStringLiteral("opacity")).toDouble() / 100.0, 0.0, 1.0));
    const QColor fg = foreground();

    // The engine fills source-over only. Say so instead of silently ignoring a
    // non-Normal Mode or the Pattern source the options bar offers.
    if (tool == ToolId::PaintBucket) {
        if (option(tool, QStringLiteral("mode")).toInt() != 0)
            setStatusHint(tr("Paint Bucket: only Normal blend is supported."));
        if (option(tool, QStringLiteral("fillsource")).toInt() == 1)
            setStatusHint(tr("Paint Bucket: pattern fill is not supported; "
                             "using the foreground colour."));
    }

    int bbox[4] = {0, 0, 0, 0};
    // Per-stroke cached selection (one resample per stroke, not per dab).
    const auto* selPtr =
        strokeSelectionMask(*d, *layer, nullptr, QPointF(), -1.0, -1.0);
    pittore::compute::NativeCmykPaint nativeFill;
    pittore::compute::NativeCmykPaint* nativeFillPtr = nullptr;
    if (!erase && d->colorMode.startsWith(QStringLiteral("CMYK"),
                                           Qt::CaseInsensitive) &&
        prepareNativeCmykPaint(*d, *layer, fg, nativeFill, false))
        nativeFillPtr = &nativeFill;
    const bool changed = pittore::compute::flood_fill_host(
        match, layer->pixels->data(), lw, lh, sx, sy, tol, contiguous, antialias,
        opacity,
        pittore::RGBAf{fg.redF(), fg.greenF(), fg.blueF(), 1.0f}, erase, bbox,
        selPtr, nativeFillPtr);
    if (!changed) {
        discardUndoStep();
        return false;
    }

    d->refreshPlacedRegion(*layer, QRect(bbox[0], bbox[1], bbox[2] - bbox[0],
                                         bbox[3] - bbox[1]));
    if (nativeFillPtr) {
        layer->cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(*layer->pixels);
        layer->cmykSourceStamp = layer->sourceStamp;
    }
    layer->thumbnail = QImage();

    const double dx0 = layer->offset.x() + bbox[0] * lsx;
    const double dy0 = layer->offset.y() + bbox[1] * lsy;
    const double dx1 = layer->offset.x() + bbox[2] * lsx;
    const double dy1 = layer->offset.y() + bbox[3] * lsy;
    const QRect dirty =
        QRect(int(std::floor(std::min(dx0, dx1))) - 1,
              int(std::floor(std::min(dy0, dy1))) - 1,
              int(std::ceil(std::fabs(dx1 - dx0))) + 3,
              int(std::ceil(std::fabs(dy1 - dy0))) + 3)
            .intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty = d->paintDirty.isNull() ? dirty : d->paintDirty.united(dirty);
    flushPaint();
    commitUndoStep(erase ? tr("Magic Eraser") : tr("Paint Bucket"),
                   erase ? QStringLiteral("magic-eraser")
                         : QStringLiteral("paint-bucket"));
    return true;
}

bool AppState::paintBucketAt(const QPointF& docPos) {
    return floodEdit(docPos, /*erase=*/false);
}

bool AppState::magicEraseAt(const QPointF& docPos) {
    return floodEdit(docPos, /*erase=*/true);
}

bool AppState::magicWandSelectAt(const QPointF& docPos, int modeOverride) {
    const ToolId tool = activeTool();
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels) {
        setStatusHint(tr("Magic Wand: select a pixel layer first."));
        return false;
    }

    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int lw = static_cast<int>(layer->pixels->width());
    const int lh = static_cast<int>(layer->pixels->height());
    const int sx = static_cast<int>(
        std::floor((docPos.x() - layer->offset.x()) / lsx));
    const int sy = static_cast<int>(
        std::floor((docPos.y() - layer->offset.y()) / lsy));
    if (lw <= 0 || lh <= 0 || sx < 0 || sy < 0 || sx >= lw || sy >= lh) {
        setStatusHint(tr("Magic Wand: click a pixel on the active layer."));
        return false;
    }

    // Colour the region is measured in: the active layer's own straight-alpha
    // pixels, or (Sample All Layers) the document composite read at each layer
    // texel's document position — the same buffer Paint Bucket floods.
    std::vector<pittore::RGBAf> matchBuf;
    const pittore::RGBAf* match = layer->pixels->data();
    if (option(tool, QStringLiteral("sample_all")).toBool() &&
        !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        matchBuf.resize(std::size_t(lw) * std::size_t(lh));
        for (int y = 0; y < lh; ++y) {
            const double docY = layer->offset.y() + (double(y) + 0.5) * lsy;
            const int py = std::clamp(static_cast<int>(std::floor(docY)), 0,
                                      comp.height() - 1);
            const QRgb* row =
                reinterpret_cast<const QRgb*>(comp.constScanLine(py));
            for (int x = 0; x < lw; ++x) {
                const double docX =
                    layer->offset.x() + (double(x) + 0.5) * lsx;
                const int px = std::clamp(static_cast<int>(std::floor(docX)), 0,
                                          comp.width() - 1);
                const QRgb c = row[px];
                const int a = qAlpha(c);
                pittore::RGBAf& dst = matchBuf[std::size_t(y) * lw + x];
                if (a <= 0) {
                    dst = pittore::RGBAf{0, 0, 0, 0};
                } else {
                    const float inv = 1.0f / static_cast<float>(a);
                    dst = pittore::RGBAf{qRed(c) * inv, qGreen(c) * inv,
                                          qBlue(c) * inv,
                                          static_cast<float>(a) / 255.0f};
                }
            }
        }
        match = matchBuf.data();
    }

    const float tol = static_cast<float>(std::clamp(
        option(tool, QStringLiteral("tolerance")).toDouble() / 255.0, 0.0, 1.0));
    const bool contiguous = option(tool, QStringLiteral("contiguous")).toBool();
    const bool antialias = option(tool, QStringLiteral("antialias")).toBool();

    // Reference colour: the average over the Sample Size window (Point Sample
    // reads the single pixel). Averaging keeps a noisy seed inside its region.
    static const int kSampleSizes[] = {1, 3, 5, 11, 31, 51, 101};
    const int si =
        std::clamp(option(tool, QStringLiteral("sample_size")).toInt(), 0, 6);
    const int half = kSampleSizes[si] / 2;
    double ar = 0, ag = 0, ab = 0, aa = 0;
    int cnt = 0;
    for (int y = std::max(0, sy - half); y <= std::min(lh - 1, sy + half); ++y)
        for (int x = std::max(0, sx - half); x <= std::min(lw - 1, sx + half); ++x) {
            const pittore::RGBAf& c = match[std::size_t(y) * lw + x];
            ar += c.r;
            ag += c.g;
            ab += c.b;
            aa += c.a;
            ++cnt;
        }
    if (cnt <= 0) return false;
    const pittore::RGBAf seed{static_cast<float>(ar / cnt),
                               static_cast<float>(ag / cnt),
                               static_cast<float>(ab / cnt),
                               static_cast<float>(aa / cnt)};
    // Same per-channel max metric as the Paint Bucket's flood fill.
    const auto within = [&](const pittore::RGBAf& c) {
        return std::max(std::max(std::fabs(c.r - seed.r), std::fabs(c.g - seed.g)),
                        std::max(std::fabs(c.b - seed.b),
                                 std::fabs(c.a - seed.a))) <= tol;
    };

    const std::size_t n = std::size_t(lw) * std::size_t(lh);
    std::vector<std::uint8_t> region(n, 0);
    const std::size_t seedIdx = std::size_t(sy) * lw + sx;
    if (contiguous) {
        // Iterative 4-connected flood; a vector is the stack so the depth stays
        // heap-bounded (mirrors flood_fill_host).
        std::vector<int> stack;
        stack.reserve(1024);
        region[seedIdx] = 1;
        stack.push_back(static_cast<int>(seedIdx));
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int x = idx % lw, y = idx / lw;
            const auto tryPush = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= lw || ny >= lh) return;
                const std::size_t ni = std::size_t(ny) * lw + nx;
                if (region[ni]) return;
                if (within(match[ni])) {
                    region[ni] = 1;
                    stack.push_back(static_cast<int>(ni));
                }
            };
            tryPush(x - 1, y);
            tryPush(x + 1, y);
            tryPush(x, y - 1);
            tryPush(x, y + 1);
        }
    } else {
        for (std::size_t i = 0; i < n; ++i)
            if (within(match[i])) region[i] = 1;
    }

    // Layer-space coverage: 1 inside the region, 0 outside. With Anti-alias a
    // 3x3 box feathers the boundary by one pixel — exactly the Paint Bucket's
    // edge treatment.
    std::vector<float> cov(n, 0.0f);
    if (!antialias) {
        for (std::size_t i = 0; i < n; ++i) cov[i] = region[i] ? 1.0f : 0.0f;
    } else {
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                float acc = 0.0f;
                int valid = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int cx = x + dx, cy = y + dy;
                        if (cx < 0 || cy < 0 || cx >= lw || cy >= lh) continue;
                        ++valid;
                        acc += region[std::size_t(cy) * lw + cx] ? 1.0f : 0.0f;
                    }
                cov[std::size_t(y) * lw + x] =
                    valid > 0 ? acc / static_cast<float>(valid) : 0.0f;
            }
    }

    QImage mask = pittore::ui::selectionMaskFromLayerAlpha(
        cov.data(), lw, lh, layer->offset, layer->scaleX, layer->scaleY,
        d->size);
    if (pittore::ui::selectionMaskBbox(mask).isEmpty()) {
        setStatusHint(tr("Magic Wand: nothing within tolerance here."));
        return false;
    }
    const int mode = modeOverride >= 0
                         ? modeOverride
                         : option(tool, QStringLiteral("selmode")).toInt();
    combineSelection(std::move(mask), mode, tr("Magic Wand"),
                     QStringLiteral("wand"));
    setStatusHint(tr("Magic Wand: selected by colour."));
    return true;
}

bool AppState::rotateActiveLayer(double degrees) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;
    // A rotation that is a no-op (or the 360° wrap) should not touch pixels.
    double deg = std::fmod(degrees, 360.0);
    if (std::fabs(deg) < 0.01 || std::fabs(deg) > 359.99) return false;

    beginUndoStep();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector, so the pointer taken
    // above may be stale: re-resolve before touching the pixels.
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

    const QImage src = qImageFromImage(*layer->pixels)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (src.isNull()) {
        discardUndoStep();
        return false;
    }
    QTransform t;
    t.rotate(deg);
    const QImage rotated = src.transformed(t, Qt::SmoothTransformation);
    auto img = imageFromQImage(rotated);
    if (!img) {
        discardUndoStep();
        return false;
    }
    if (layer->mask && layer->maskLinked) {
        auto rotatedMask = rotatedMaskImage(*layer->mask, t);
        if (!rotatedMask) {
            discardUndoStep();
            return false;
        }
        layer->mask = std::move(rotatedMask);
        ++layer->maskStamp;
    }

    // Keep the layer's document-space centre fixed as its native bounds grow.
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const QPointF center(
        layer->offset.x() + layer->pixels->width() * lsx * 0.5,
        layer->offset.y() + layer->pixels->height() * lsy * 0.5);
    layer->pixels = img;
    layer->offset = QPointF(center.x() - img->width() * lsx * 0.5,
                            center.y() - img->height() * lsy * 0.5);
    syncLinkedMaskPlacement(*layer);
    ++layer->sourceStamp;
    layer->thumbnail = QImage();
    d->rebuildComposite();
    commitUndoStep(tr("Rotate Layer"), QStringLiteral("rotate"));
    return true;
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

void AppState::setCursorInfo(const QPointF& pos, const QColor& color) {
    cursorPos_ = pos;
    cursorColor_ = color;
    emit cursorInfoChanged(pos, color);
}

void AppState::markAnnotationsChanged() {
    if (DocumentItem* d = activeDocument()) {
        // Push derived readouts into the annotation tools' options fields so
        // the options bar shows the live Count and Ruler numbers.
        const int group = option(ToolId::Count, QStringLiteral("group")).toInt();
        int count = 0;
        for (const CountMarker& m : d->countMarkers)
            if (m.group == group) ++count;
        setOptionSilently(ToolId::Count, QStringLiteral("count"), count);

        if (d->rulerHasMeasurement) {
            const QPointF a = d->rulerStart;
            const QPointF b = d->rulerEnd;
            setOptionSilently(ToolId::Ruler, QStringLiteral("x"), a.x());
            setOptionSilently(ToolId::Ruler, QStringLiteral("y"), a.y());
            setOptionSilently(ToolId::Ruler, QStringLiteral("w"), b.x() - a.x());
            setOptionSilently(ToolId::Ruler, QStringLiteral("h"), b.y() - a.y());
            const double ang =
                std::atan2(b.y() - a.y(), b.x() - a.x()) * 180.0 /
                3.14159265358979323846;
            setOptionSilently(ToolId::Ruler, QStringLiteral("a"), ang);
            setOptionSilently(ToolId::Ruler, QStringLiteral("l1"), 0.0);
            setOptionSilently(ToolId::Ruler, QStringLiteral("l2"), std::hypot(b.x() - a.x(), b.y() - a.y()));
        }
        emit documentModified(d);
    }
    emit annotationsChanged();
}

void AppState::flushPaint() {
    DocumentItem* d = activeDocument();
    if (!d || d->paintDirty.isNull()) return;
    const QRect dirty = d->paintDirty;
    d->paintDirty = QRect();
    const auto t0 = std::chrono::steady_clock::now();
    d->renderRegion(dirty);
    // Wash strokes live in the scratch buffer: fold the dirty region over
    // the fresh composite at the stroke opacity so the stroke previews
    // live (the bake makes it permanent on release). Both sides stay in
    // premultiplied space: the composite is ARGB32_Premultiplied and the
    // straight scratch is premultiplied on the fly.
    if (washArmed_ && !washDirty_.isNull() && !d->composite.isNull() &&
        int(washScratch_.width()) == d->composite.width() &&
        int(washScratch_.height()) == d->composite.height()) {
        const QVariant o =
            option(activeTool_, QStringLiteral("opacity"));
        const double op =
            o.isValid() ? std::clamp(o.toDouble() / 100.0, 0.0, 1.0) : 1.0;
        if (op > 0.0) {
            const QRect region =
                dirty.intersected(washDirty_).intersected(
                    QRect(QPoint(0, 0), d->size));
            const std::string mixId =
                option(activeTool_, QStringLiteral("brush_composite"))
                    .toString()
                    .toStdString();
            const auto mixOp =
                pittore::compute::dabmix::mix_from_id(mixId.c_str());
            const bool useMix =
                mixOp != pittore::compute::dabmix::DabMixOp::Normal;
            const pittore::RGBAf* src = washScratch_.data();
            const std::uint32_t w = washScratch_.width();
            for (int y = region.top(); y <= region.bottom(); ++y) {                QRgb* row = reinterpret_cast<QRgb*>(
                    d->composite.scanLine(y));
                for (int x = region.left(); x <= region.right(); ++x) {
                    const pittore::RGBAf& s =
                        src[std::size_t(y) * w + x];
                    const float sa =
                        std::clamp(s.a * float(op), 0.0f, 1.0f);
                    if (sa <= 0.0f) continue;
                    if (useMix) {
                        // Pigment-mix preview: run the dab color through
                        // the mean shell, then re-premultiply into the
                        // composite row (which stores premultiplied, so
                        // un-premultiply first).
                        const QRgb dst = row[x];
                        const float da = qAlpha(dst) / 255.0f;
                        float dr = 0.0f, dg = 0.0f, db = 0.0f;
                        if (da > 1e-6f) {
                            dr = qRed(dst) / 255.0f / da;
                            dg = qGreen(dst) / 255.0f / da;
                            db = qBlue(dst) / 255.0f / da;
                        }
                        float or_, og, ob, oa;
                        pittore::compute::dabmix::mix_pixel(
                            mixOp, s.r, s.g, s.b, sa, dr, dg, db, da, or_,
                            og, ob, oa);
                        row[x] = qRgba(
                            std::clamp(int(or_ * oa * 255.0f), 0, 255),
                            std::clamp(int(og * oa * 255.0f), 0, 255),
                            std::clamp(int(ob * oa * 255.0f), 0, 255),
                            std::clamp(int(oa * 255.0f), 0, 255));
                        continue;
                    }
                    const QRgb dst = row[x];
                    const float inv = 1.0f - sa;
                    const float out_a =
                        sa + (qAlpha(dst) / 255.0f) * inv;
                    if (out_a <= 0.0f) continue;
                    const float r =
                        (s.r * sa + (qRed(dst) / 255.0f) * inv) / out_a;
                    const float g =
                        (s.g * sa + (qGreen(dst) / 255.0f) * inv) / out_a;
                    const float b =
                        (s.b * sa + (qBlue(dst) / 255.0f) * inv) / out_a;
                    row[x] = qRgba(std::clamp(int(r * out_a * 255.0f), 0, 255),
                                   std::clamp(int(g * out_a * 255.0f), 0, 255),
                                   std::clamp(int(b * out_a * 255.0f), 0, 255),
                                   std::clamp(int(out_a * 255.0f), 0, 255));
                }
            }
            // Track the overlay footprint so the tile cache falls back to
            // the composite tier under live wash (simple-tier tiles blit
            // own layer pixels, which carry no wash ink until bake).
            if (!region.isEmpty())
                d->washOverlayDirty = d->washOverlayDirty.united(region);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    // Per input event: trace-gated like the dab logs (mutex + file write per
    // event dominated fast strokes).
    if (pittore::core::log::strokeTrace())
        PITTORE_LOG("[flush] dirty=(%d,%d,%d,%d) backend=%s ms=%.2f", dirty.x(), dirty.y(),
                     dirty.width(), dirty.height(),
                     d->backend ? d->backend->name().c_str() : "cpu", ms);
    // Region notice: the canvas repaints only the stroke's touching rect —
    // a full-frame refresh per brush move was the lag in the paint logs.
    noteRegionEdit(QRectF(dirty));
    emit regionModified(d, QRectF(dirty));
}

// ---------------------------------------------------------------------------
// Liquify (displacement warp)
// ---------------------------------------------------------------------------
bool AppState::beginLiquifyStroke() {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;

    // One history entry for the whole stroke. The snapshot must be taken
    // BEFORE the layer is rebound: it shares the pre-stroke image, so undo
    // restores exactly what the warp started from. Use the gesture-scoped API
    // so the matching commitUndoStep()/discardUndoStep() from the stroke's end
    // actually promote/drop it (raw beginUndoAction would leave the gesture
    // depth at zero and the commit would be dropped).
    beginUndoStep();
    // beginUndoStep() COW-shares the layer vector with the snapshot, so the
    // pointer taken above goes stale the moment that vector detaches: re-resolve
    // it. This access detaches once; after it the pointer is stable for the
    // freeze/rebind below, and the snapshot keeps the untouched original.
    layer = activeLayer();
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels || layer->pixels->width() == 0 ||
        layer->pixels->height() == 0) {
        discardUndoStep();
        return false;
    }

    // Freeze the pre-stroke pixels as the warp source and hand the live layer a
    // private copy to receive the resamples. dst never aliases src, and the
    // frozen image stays alive for the whole stroke (the undo snapshot shares
    // it, so undoing restores the original).
    liquifySrc_ = layer->pixels;
    liquifyCmykSrc_ = layer->cmykPixels;
    layer->pixels = std::make_shared<pittore::Image>(liquifySrc_->clone());
    ++layer->sourceStamp;
    layer->cmykSourceStamp = layer->cmykPixels
                                 ? layer->sourceStamp
                                 : 0;
    layer->thumbnail = QImage();

    // Stage the frozen source once: a displaced sample can land anywhere in it,
    // so it stays device-resident for the whole stroke (the same contract
    // composite_placed has for a placed layer).
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    const std::size_t bytes =
        static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    if (!liquifySrcBuf_ || !liquifyDstBuf_ || liquifyBufW_ != w ||
        liquifyBufH_ != h) {
        pittore::compute::ComputeBackend& be = computeBackend();
        liquifySrcBuf_ = be.make_buffer(bytes);
        liquifyDstBuf_ = be.make_buffer(bytes);
        liquifyBufW_ = w;
        liquifyBufH_ = h;
    }
    std::memcpy(liquifySrcBuf_->host(), liquifySrc_->data(), bytes);
    liquifySrcBuf_->upload();
    PITTORE_LOG("[liquify] begin %ux%u backend=%s", w, h,
                 d->backend ? d->backend->name().c_str() : "cpu");
    return true;
}

bool AppState::liquifyDab(const pittore::compute::WarpMesh& mesh, int x0,
                          int y0, int x1, int y1) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels || !liquifySrc_ || !liquifySrcBuf_ ||
        !liquifyDstBuf_)
        return false;

    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    if (liquifySrc_->width() != w || liquifySrc_->height() != h) return false;

    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(x1, static_cast<int>(w));
    y1 = std::min(y1, static_cast<int>(h));
    if (x0 >= x1 || y0 >= y1) return false;

    const pittore::compute::WarpSubgrid grid =
        pittore::compute::warp_subgrid(mesh, x0, y0, x1, y1);
    if (grid.empty()) return false;

    computeBackend().warp(*liquifyDstBuf_, *liquifySrcBuf_, w, h, grid,
                          static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1),
                          static_cast<std::uint32_t>(y1));

    // The rebuilt region is in the destination buffer's host staging; copy it
    // into the layer's pixels, which stay the source of truth for undo, saving
    // and compositing.
    const pittore::RGBAf* src =
        static_cast<const pittore::RGBAf*>(liquifyDstBuf_->host());
    pittore::RGBAf* dst = layer->pixels->data();
    const std::size_t rowBytes =
        static_cast<std::size_t>(x1 - x0) * sizeof(pittore::RGBAf);
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * w + x0,
                    src + static_cast<std::size_t>(y) * w + x0, rowBytes);
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer, QRect(x0, y0, x1 - x0, y1 - y0));
    layer->thumbnail = QImage();
    // Dirty rect: the resampled layer rect mapped to document space, plus the
    // resampling halo (as in paintDab).
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const double dx0 = layer->offset.x() + x0 * lsx;
    const double dy0 = layer->offset.y() + y0 * lsy;
    const double dx1 = layer->offset.x() + x1 * lsx;
    const double dy1 = layer->offset.y() + y1 * lsy;
    const QRect dabRect(
        static_cast<int>(std::floor(dx0)) - 1,
        static_cast<int>(std::floor(dy0)) - 1,
        static_cast<int>(std::ceil(dx1 - dx0)) + 3,
        static_cast<int>(std::ceil(dy1 - dy0)) + 3);
    const QRect clipped = dabRect.intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty =
        d->paintDirty.isNull() ? clipped : d->paintDirty.united(clipped);
    return true;
}

void AppState::endLiquifyStroke() {
    // Keep the staging buffers (grown on demand, reused across strokes); only
    // the frozen snapshot is stroke-scoped.
    liquifySrc_.reset();
    liquifyCmykSrc_.reset();
}

void AppState::cancelLiquifyStroke() {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (d && layer && liquifySrc_ && layer->pixels &&
        layer->pixels->width() == liquifySrc_->width() &&
        layer->pixels->height() == liquifySrc_->height()) {
        layer->pixels = liquifySrc_;   // drop the partially warped copy
        ++layer->sourceStamp;
        layer->cmykPixels = liquifyCmykSrc_;
        layer->cmykSourceStamp = layer->cmykPixels
                                     ? layer->sourceStamp
                                     : 0;
        layer->thumbnail = QImage();
    }
    endLiquifyStroke();
    if (d) {
        discardUndoStep();
        d->paintDirty = QRect();
        d->rebuildComposite();
        emit regionModified(d, QRectF(QPoint(0, 0), d->size));
        emit documentModified(d);
    }
}

bool AppState::beginLiquifySession() {
    if (liquifySession_) return liquifySrc_ != nullptr;
    if (!beginLiquifyStroke()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels) {
        cancelLiquifyStroke();
        return false;
    }
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    liquifyMesh_ = pittore::compute::make_warp_mesh(w, h);
    liquifyMask_.assign(static_cast<std::size_t>(w) * h, 0.0f);
    liquifyMaskW_ = w;
    liquifyMaskH_ = h;
    liquifySession_ = true;
    liquifySessionMoved_ = false;
    liquifyMeshStrength_ = 100.0f;
    liquifyHasClone_ = false;
    return true;
}

void AppState::endLiquifySession() {
    endLiquifyStroke();
    liquifySession_ = false;
    liquifySessionMoved_ = false;
    liquifyMask_.clear();
    liquifyMaskW_ = 0;
    liquifyMaskH_ = 0;
    liquifyHasClone_ = false;
    liquifyMeshStrength_ = 100.0f;
}

void AppState::cancelLiquifySession() {
    cancelLiquifyStroke();
    liquifySession_ = false;
    liquifySessionMoved_ = false;
    liquifyMask_.clear();
    liquifyMaskW_ = 0;
    liquifyMaskH_ = 0;
    liquifyHasClone_ = false;
    liquifyMeshStrength_ = 100.0f;
    liquifyMesh_ = pittore::compute::WarpMesh();
}

float AppState::liquifyMaskAt(float x, float y) const {
    if (liquifyMask_.empty() || liquifyMaskW_ < 2 || liquifyMaskH_ < 2)
        return 0.0f;
    const float fx = std::clamp(x, 0.0f, static_cast<float>(liquifyMaskW_ - 1));
    const float fy = std::clamp(y, 0.0f, static_cast<float>(liquifyMaskH_ - 1));
    const auto c0 = static_cast<std::uint32_t>(fx);
    const auto r0 = static_cast<std::uint32_t>(fy);
    const auto c1 = std::min(c0 + 1, liquifyMaskW_ - 1);
    const auto r1 = std::min(r0 + 1, liquifyMaskH_ - 1);
    const float tx = fx - c0;
    const float ty = fy - r0;
    const float a = liquifyMask_[static_cast<std::size_t>(r0) * liquifyMaskW_ + c0];
    const float b = liquifyMask_[static_cast<std::size_t>(r0) * liquifyMaskW_ + c1];
    const float c = liquifyMask_[static_cast<std::size_t>(r1) * liquifyMaskW_ + c0];
    const float d = liquifyMask_[static_cast<std::size_t>(r1) * liquifyMaskW_ + c1];
    return a + (b - a) * tx + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

void AppState::paintLiquifyMask(float cx, float cy, float radius,
                                float hardness, bool freeze, float opacity) {
    if (liquifyMask_.empty() || radius <= 0.0f || opacity <= 0.0f) return;
    const float core = std::clamp(hardness, 0.0f, 1.0f);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int x1 = std::min(static_cast<int>(liquifyMaskW_),
                            static_cast<int>(std::ceil(cx + radius)) + 1);
    const int y1 = std::min(static_cast<int>(liquifyMaskH_),
                            static_cast<int>(std::ceil(cy + radius)) + 1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float d = std::hypot(static_cast<float>(x) - cx,
                                       static_cast<float>(y) - cy);
            if (d >= radius) continue;
            const float t = d / radius;
            float f = 1.0f;
            if (t > core && core < 1.0f) {
                const float u = 1.0f - (t - core) / (1.0f - core);
                f = u * u * (3.0f - 2.0f * u);
            }
            float& m = liquifyMask_[static_cast<std::size_t>(y) * liquifyMaskW_ + x];
            const float w = f * opacity;
            m = freeze ? std::max(m, w) : std::min(m, 1.0f - w);
        }
    }
}

bool AppState::liquifyMaskPresent() const {
    for (float v : liquifyMask_) {
        if (v > 0.001f) return true;
    }
    return false;
}

void AppState::clearLiquifyMask() {
    std::fill(liquifyMask_.begin(), liquifyMask_.end(), 0.0f);
}

void AppState::fillLiquifyMask(float v) {
    std::fill(liquifyMask_.begin(), liquifyMask_.end(),
              std::clamp(v, 0.0f, 1.0f));
}

void AppState::invertLiquifyMask() {
    for (float& v : liquifyMask_) v = 1.0f - v;
}

bool AppState::liquifyMaskFromSelection() {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->pixels || liquifyMask_.empty()) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    bool any = false;
    for (std::uint32_t y = 0; y < liquifyMaskH_; ++y) {
        for (std::uint32_t x = 0; x < liquifyMaskW_; ++x) {
            const double dx = layer->offset.x() + (x + 0.5) * lsx;
            const double dy = layer->offset.y() + (y + 0.5) * lsy;
            float v = 0.0f;
            if (d->selectionIsMask && !d->selectionMask.isNull()) {
                const int sx = std::clamp(static_cast<int>(dx), 0,
                                          d->selectionMask.width() - 1);
                const int sy = std::clamp(static_cast<int>(dy), 0,
                                          d->selectionMask.height() - 1);
                v = qGray(d->selectionMask.pixel(sx, sy)) / 255.0f;
            } else if (!d->selection.isEmpty() && d->selection.contains(dx, dy)) {
                v = 1.0f;
            }
            if (v > 0.0f) any = true;
            liquifyMask_[static_cast<std::size_t>(y) * liquifyMaskW_ + x] =
                std::max(liquifyMask_[static_cast<std::size_t>(y) * liquifyMaskW_ + x], v);
        }
    }
    return any;
}

bool AppState::liquifyMirrorDab(float cx, float cy, float radius,
                                float angle, bool invert, float strength,
                                float falloffExp) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->pixels || !liquifySrc_) return false;
    if (radius <= 0.0f || strength <= 0.0f) return false;
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    float ux = std::cos(angle);
    float uy = std::sin(angle);
    if (invert) {
        const float tx = ux;
        ux = -uy;
        uy = tx;
    }
    const auto sample = [&](float x, float y) {
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float tx = x - x0;
        const float ty = y - y0;
        float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
        const int xs[2] = {x0, x0 + 1};
        const int ys[2] = {y0, y0 + 1};
        const float ws[2] = {1.0f - tx, tx};
        const float hs[2] = {1.0f - ty, ty};
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const float wt = ws[i] * hs[j];
                if (wt <= 0.0f) continue;
                if (xs[i] < 0 || ys[j] < 0 || xs[i] >= static_cast<int>(w) ||
                    ys[j] >= static_cast<int>(h))
                    continue;
                const pittore::RGBAf& p =
                    layer->pixels->at(static_cast<std::uint32_t>(xs[i]),
                                      static_cast<std::uint32_t>(ys[j]));
                ar += p.r * p.a * wt;
                ag += p.g * p.a * wt;
                ab += p.b * p.a * wt;
                aa += p.a * wt;
            }
        }
        if (aa <= 1e-6f) return pittore::RGBAf{0, 0, 0, 0};
        return pittore::RGBAf{ar / aa, ag / aa, ab / aa, aa};
    };
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int x1 = std::min(static_cast<int>(w),
                            static_cast<int>(std::ceil(cx + radius)) + 1);
    const int y1 = std::min(static_cast<int>(h),
                            static_cast<int>(std::ceil(cy + radius)) + 1);
    bool changed = false;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dist = std::hypot(dx, dy);
            if (dist >= radius) continue;
            const float t = 1.0f - dist / radius;
            const float base = t * t * (3.0f - 2.0f * t);
            const float wt = std::pow(std::max(base, 0.0f), falloffExp) *
                             strength * (1.0f - liquifyMaskAt(static_cast<float>(x),
                                                             static_cast<float>(y)));
            if (wt <= 0.0f) continue;
            const float proj = dx * ux + dy * uy;
            const float qx = cx + proj * ux - (dx - proj * ux);
            const float qy = cy + proj * uy - (dy - proj * uy);
            const pittore::RGBAf s = sample(qx, qy);
            pittore::RGBAf& dst = layer->pixels->at(static_cast<std::uint32_t>(x),
                                                     static_cast<std::uint32_t>(y));
            const float da = dst.a;
            const float outA = da * (1.0f - wt) + s.a * wt;
            if (outA > 1e-6f) {
                dst.r = (dst.r * da * (1.0f - wt) + s.r * s.a * wt) / outA;
                dst.g = (dst.g * da * (1.0f - wt) + s.g * s.a * wt) / outA;
                dst.b = (dst.b * da * (1.0f - wt) + s.b * s.a * wt) / outA;
            } else {
                dst.r = dst.g = dst.b = 0.0f;
            }
            dst.a = outA;
            changed = true;
        }
    }
    if (!changed) return false;
    d->refreshPlacedRegion(*layer, QRect(x0, y0, x1 - x0, y1 - y0));
    refreshLayerCmyk(*d, *layer, QRect(x0, y0, x1 - x0, y1 - y0));
    layer->thumbnail = QImage();
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const QRect dabRect(
        static_cast<int>(std::floor(layer->offset.x() + x0 * lsx)) - 1,
        static_cast<int>(std::floor(layer->offset.y() + y0 * lsy)) - 1,
        (x1 - x0) + 3, (y1 - y0) + 3);
    const QRect clipped = dabRect.intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty =
        d->paintDirty.isNull() ? clipped : d->paintDirty.united(clipped);
    if (liquifySession_) liquifySessionMoved_ = true;
    return true;
}

bool AppState::liquifyCloneSource(float x, float y) {
    liquifyClone_ = QPointF(x, y);
    liquifyHasClone_ = true;
    return true;
}

void AppState::restoreLiquifyMask(std::vector<float> m) {
    if (m.size() != liquifyMask_.size()) return;
    liquifyMask_ = std::move(m);
}

bool AppState::liquifyMaskCombine(const std::vector<float>& src, int op) {
    if (liquifyMask_.empty() || src.size() != liquifyMask_.size()) return false;
    if (op == 4) {
        invertLiquifyMask();
        return true;
    }
    for (std::size_t i = 0; i < liquifyMask_.size(); ++i) {
        const float s = std::clamp(src[i], 0.0f, 1.0f);
        float& m = liquifyMask_[i];
        if (op == 0) m = s;
        else if (op == 1) m = std::max(m, s);
        else if (op == 2) m = m * (1.0f - s);
        else if (op == 3) m = m * s;
    }
    return true;
}

bool AppState::renderLiquifyFull() {
    if (!liquifySession_ || liquifyMesh_.cols < 2) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels) return false;
    const int w = static_cast<int>(layer->pixels->width());
    const int h = static_cast<int>(layer->pixels->height());
    if (!liquifyDab(liquifyMesh_, 0, 0, w, h)) return false;
    flushPaint();
    return true;
}

void AppState::scaleLiquifyMesh(float f) {
    if (!liquifySession_) return;
    for (float& v : liquifyMesh_.offsets) v *= f;
}

void AppState::resetLiquifyMesh() {
    if (!liquifySession_) return;
    if (!liquifyMesh_.identity()) {
        std::fill(liquifyMesh_.offsets.begin(), liquifyMesh_.offsets.end(),
                  0.0f);
        liquifyMeshStrength_ = 100.0f;
        if (renderLiquifyFull()) liquifySessionMoved_ = true;
    } else {
        liquifyMeshStrength_ = 100.0f;
    }
}

void AppState::setLiquifyMesh(pittore::compute::WarpMesh mesh) {
    liquifyMesh_ = std::move(mesh);
    liquifyMeshStrength_ = 100.0f;
}

bool AppState::saveLiquifyMesh(const QString& path) const {
    if (!liquifySession_ || liquifyMesh_.cols < 2) return false;
    return pittore::compute::warp_mesh_write(path.toStdString(),
                                              liquifyMesh_);
}

bool AppState::loadLiquifyMesh(const QString& path) {
    if (!liquifySession_) return false;
    pittore::compute::WarpMesh tmp;
    if (!pittore::compute::warp_mesh_read(path.toStdString(), tmp))
        return false;
    if (tmp.cols != liquifyMesh_.cols || tmp.rows != liquifyMesh_.rows)
        return false;
    liquifyMesh_ = std::move(tmp);
    liquifyMeshStrength_ = 100.0f;
    if (renderLiquifyFull()) liquifySessionMoved_ = true;
    return true;
}

void AppState::requestLiquifySaveMesh() {
    emit liquifySaveMeshRequested();
}

void AppState::requestLiquifyLoadMesh() {
    emit liquifyLoadMeshRequested();
}



void AppState::noteRegionEdit(const QRectF& docRect) {
    regionEditRect_ = docRect;
    regionEditTimeUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
}

qint64 AppState::takeRegionEditAgeUs() {
    if (regionEditTimeUs_ < 0) return -1;
    const qint64 ageUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count() -
                         regionEditTimeUs_;
    regionEditTimeUs_ = -1;  // consumed by the first paint after the edit
    return (ageUs < 0 || ageUs >= 1000000) ? -1 : ageUs;
}

bool AppState::applyActiveLayerAlpha(const float* mask, int width, int height,
                                     const QString& historyName,
                                     const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels) return false;
    if (!mask || width <= 0 || height <= 0) return false;
    if (static_cast<std::uint32_t>(width) != layer->pixels->width() ||
        static_cast<std::uint32_t>(height) != layer->pixels->height())
        return false;

    // One undo step for the whole alpha edit; the snapshot shares the current
    // image, so copy-on-write BEFORE the in-place multiply (or undoing would
    // restore the already-multiplied pixels).
    d->beginUndoAction();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector (the snapshot shares it),
    // so the pointer taken before it is stale: re-resolve before touching the
    // pixels. Without this the multiply lands on the snapshot's shared image and
    // the live layer — and therefore the canvas — never changes.
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }

    // Straight-alpha edit: multiply alpha down by the soft mask. RGB is kept
    // untouched so the compositor's straight-alpha blend still sees true colour
    // (fringe cleanup against the removed background is future work).
    pittore::RGBAf* px = layer->pixels->data();
    const std::size_t n = layer->pixels->pixel_count();
    for (std::size_t i = 0; i < n; ++i)
        px[i].a *= std::clamp(mask[i], 0.0f, 1.0f);

    ++layer->sourceStamp;           // native pixels changed in place
    layer->thumbnail = QImage();    // …and a stale panel preview
    const QRect bounds =
        layerBounds(*d, *layer).toAlignedRect().intersected(QRect(QPoint(0, 0), d->size));
    if (bounds.isEmpty())
        d->rebuildComposite();
    else
        d->renderRegion(bounds);
    d->commitUndoAction(historyName, iconKey);
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::replaceActiveLayerRgba(const QImage& rgba,
                                      const QString& historyName,
                                      const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel ||
        !layer->pixels)
        return false;
    if (rgba.isNull() || rgba.format() != QImage::Format_RGBA8888) return false;
    if (static_cast<std::uint32_t>(rgba.width()) != layer->pixels->width() ||
        static_cast<std::uint32_t>(rgba.height()) != layer->pixels->height())
        return false;

    d->beginUndoAction();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }

    pittore::RGBAf* px = layer->pixels->data();
    for (int y = 0; y < rgba.height(); ++y) {
        const uchar* s = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            const std::size_t i = std::size_t(y) * rgba.width() + x;
            px[i].r = s[x * 4 + 0] / 255.0f;
            px[i].g = s[x * 4 + 1] / 255.0f;
            px[i].b = s[x * 4 + 2] / 255.0f;
            px[i].a = s[x * 4 + 3] / 255.0f;
        }
    }
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer);
    layer->thumbnail = QImage();
    const QRect bounds =
        layerBounds(*d, *layer).toAlignedRect().intersected(QRect(QPoint(0, 0), d->size));
    if (bounds.isEmpty())
        d->rebuildComposite();
    else
        d->renderRegion(bounds);
    d->commitUndoAction(historyName, iconKey);
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::eraseSelectionFromActiveLayer() {
    DocumentItem* d = activeDocument();
    if (!d || !d->selectionIsMask || d->selectionMask.isNull() ||
        d->selection.isEmpty())
        return false;
    const QRect selRect =
        d->selection.toAlignedRect().intersected(QRect(QPoint(0, 0), d->size));
    if (selRect.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel ||
        !layer->pixels)
        return false;

    d->beginUndoAction();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector (the snapshot shares it):
    // re-resolve so the erase lands on the live clone, not the snapshot's image.
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }

    // Erase by mapping every selected doc pixel back to its layer pixel
    // (inverse transform, exactly how selectionMaskFromLayerAlpha builds the
    // channel) and multiplying that pixel's alpha by the complement of the
    // mask coverage. Multiple doc pixels can land on one layer pixel when the
    // layer is scaled down; accumulated factors keep the result consistent.
    const QImage& m = d->selectionMask;
    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);
    pittore::RGBAf* px = layer->pixels->data();
    for (int dy = selRect.top(); dy <= selRect.bottom(); ++dy) {
        const uchar* mrow = m.constScanLine(dy);
        for (int dx = selRect.left(); dx <= selRect.right(); ++dx) {
            const int cov = mrow[dx];
            if (!cov) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            px[std::size_t(ly) * pw + lx].a *= (255 - cov) / 255.0;
        }
    }

    ++layer->sourceStamp;           // native pixels changed in place
    layer->thumbnail = QImage();    // …stale panel preview
    d->renderRegion(selRect);
    d->commitUndoAction(tr("Delete"), QStringLiteral("eraser"));
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::fillActiveSelectionWithColor(const QColor& color,
                                            const QString& undoName) {
    DocumentItem* d = activeDocument();
    if (!d || !color.isValid()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel)
        return false;
    // Realize an empty layer so Fill works on a fresh Layer (fill covers
    // the whole layer when nothing is selected).
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) return false;

    // Coverage source: the live selection (mask or rect/ellipse rasterised),
    // or full coverage when nothing is selected.
    QRect clipRect;
    QImage covMask;
    bool fullDoc = false;
    if (!d->selection.isEmpty() || (d->selectionIsMask && !d->selectionMask.isNull())) {
        covMask = selectionAsMask(*d);
        if (covMask.isNull()) return false;
        clipRect = d->selection.toAlignedRect()
                       .intersected(QRect(QPoint(0, 0), d->size));
        if (clipRect.isEmpty()) return false;
    } else {
        clipRect = QRect(QPoint(0, 0), d->size);
        fullDoc = true;
    }
    if (clipRect.isEmpty()) return false;

    d->beginUndoAction();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }
    const float fr = color.red() / 255.0f;
    const float fg = color.green() / 255.0f;
    const float fb = color.blue() / 255.0f;
    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);
    pittore::RGBAf* px = layer->pixels->data();
    const bool hasMask = !covMask.isNull();
    const bool maskSized = hasMask && covMask.size() == d->size;
    pittore::compute::NativeCmykPaint nativeFill;
    const bool nativeCmyk = prepareNativeCmykPaint(
        *d, *layer, color, nativeFill, false);
    int nativeX0 = pw, nativeY0 = ph, nativeX1 = -1, nativeY1 = -1;
    for (int dy = clipRect.top(); dy <= clipRect.bottom(); ++dy) {
        const uchar* mrow = maskSized ? covMask.constScanLine(dy) : nullptr;
        for (int dx = clipRect.left(); dx <= clipRect.right(); ++dx) {
            const float cov = fullDoc ? 1.0f : (mrow[dx] / 255.0f);
            if (cov <= 0.0f) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            const std::size_t index = std::size_t(ly) * pw + lx;
            pittore::RGBAf& p = px[index];
            const float oldAlpha = p.a;
            // Straight-alpha "over": opaque FG over dst by coverage.
            const float na = cov + oldAlpha * (1.0f - cov);
            if (na > 1e-6f) {
                p.r = (fr * cov + p.r * oldAlpha * (1.0f - cov)) / na;
                p.g = (fg * cov + p.g * oldAlpha * (1.0f - cov)) / na;
                p.b = (fb * cov + p.b * oldAlpha * (1.0f - cov)) / na;
                p.a = na;
            } else {
                p.r = fr;
                p.g = fg;
                p.b = fb;
                p.a = 0.0f;
            }
            if (nativeCmyk && nativeFill.pixels) {
                auto& inks = nativeFill.pixels[index];
                for (int c = 0; c < 4; ++c) {
                    const float oldInk = inks[c] / 65535.0f;
                    const float srcInk = nativeFill.source[c] / 65535.0f;
                    const float outInk = na > 1e-6f
                        ? (srcInk * cov + oldInk * oldAlpha * (1.0f - cov)) / na
                        : srcInk;
                    inks[c] = static_cast<std::uint16_t>(std::clamp(
                        std::lround(outInk * 65535.0f), 0L, 65535L));
                }
                nativeX0 = std::min(nativeX0, lx);
                nativeY0 = std::min(nativeY0, ly);
                nativeX1 = std::max(nativeX1, lx);
                nativeY1 = std::max(nativeY1, ly);
            }
        }
    }
    ++layer->sourceStamp;
    if (nativeCmyk && nativeX1 >= nativeX0 && nativeY1 >= nativeY0) {
        layer->cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(*layer->pixels);
        layer->cmykSourceStamp = layer->sourceStamp;
    } else {
        refreshLayerCmyk(*d, *layer);
    }
    layer->thumbnail = QImage();
    d->renderRegion(clipRect);
    d->commitUndoAction(undoName.isEmpty() ? tr("Fill") : undoName,
                        QStringLiteral("fill"));
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::invertActiveLayerPixels() {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel ||
        !layer->pixels)
        return false;
    const bool hasSel = !d->selection.isEmpty() ||
                        (d->selectionIsMask && !d->selectionMask.isNull());
    if (!hasSel) {
        // Fast path: whole layer, alpha untouched.
        d->beginUndoAction();
        copyOnWriteActiveLayer();
        layer = activeLayer();
        if (!layer || !layer->pixels) {
            d->discardUndoAction();
            return false;
        }
        const std::size_t n = layer->pixels->pixel_count();
        pittore::RGBAf* all = layer->pixels->data();
        for (std::size_t i = 0; i < n; ++i) {
            all[i].r = 1.0f - all[i].r;
            all[i].g = 1.0f - all[i].g;
            all[i].b = 1.0f - all[i].b;
        }
        ++layer->sourceStamp;
        refreshLayerCmyk(*d, *layer);
        layer->thumbnail = QImage();
        d->rebuildComposite();
        d->commitUndoAction(tr("Invert"), QStringLiteral("invert"));
        emit historyChanged();
        emit documentModified(d);
        return true;
    }
    QRect clipRect = d->selection.toAlignedRect().intersected(
        QRect(QPoint(0, 0), d->size));
    if (clipRect.isEmpty()) return false;
    const QImage cov = selectionAsMask(*d);
    if (cov.isNull() || cov.size() != d->size) return false;
    d->beginUndoAction();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }
    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);
    pittore::RGBAf* px = layer->pixels->data();
    for (int dy = clipRect.top(); dy <= clipRect.bottom(); ++dy) {
        const uchar* mrow = cov.constScanLine(dy);
        for (int dx = clipRect.left(); dx <= clipRect.right(); ++dx) {
            const float c = mrow[dx] / 255.0f;
            if (c <= 0.0f) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            pittore::RGBAf& p = px[std::size_t(ly) * pw + lx];
            // Feathered edges lerp toward the complement by coverage.
            p.r += ((1.0f - p.r) - p.r) * c;
            p.g += ((1.0f - p.g) - p.g) * c;
            p.b += ((1.0f - p.b) - p.b) * c;
        }
    }
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer);
    layer->thumbnail = QImage();
    d->renderRegion(clipRect);
    d->commitUndoAction(tr("Invert"), QStringLiteral("invert"));
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setActiveLayerOpacity(int percent) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l) return false;
    percent = std::clamp(percent, 0, 100);
    if (l->opacity == percent) return true;
    beginUndoStep();
    activeLayer()->opacity = percent;
    commitUndoStep(tr("Layer Opacity"), QStringLiteral("opacity"));
    d->rebuildComposite();
    emit documentModified(d);
    emit layersChanged();
    return true;
}

QImage AppState::copyActiveLayerMasked(const QImage& mask, const QRectF& clip,
                                       QRectF* docRect) {
    if (docRect) *docRect = QRectF();
    DocumentItem* d = activeDocument();
    if (!d) return QImage();
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return QImage();
    const bool hasMask = !mask.isNull() &&
                         mask.size() == d->size &&
                         mask.format() == QImage::Format_Grayscale8;
    QRect srcRect = QRect(QPoint(0, 0), d->size);
    if (!clip.isEmpty())
        srcRect = clip.toAlignedRect().intersected(srcRect);
    if (srcRect.isEmpty()) return QImage();

    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);

    // Lift the layer's appearance inside srcRect, clipping each pixel by the
    // mask's coverage (same inverse-transform mapping as the erase path), so
    // placing the result back at srcRect reproduces exactly what the selection
    // covered at full resolution.
    QImage out(srcRect.size(), QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    for (int dy = srcRect.top(); dy <= srcRect.bottom(); ++dy) {
        const uchar* mrow = hasMask ? mask.constScanLine(dy) : nullptr;
        uchar* orow = out.scanLine(dy - srcRect.top());
        for (int dx = srcRect.left(); dx <= srcRect.right(); ++dx) {
            const int cov = hasMask ? mrow[dx] : 255;
            if (cov <= 0) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            const pittore::RGBAf& p = layer->pixels->at(lx, ly);
            uchar* q = orow + std::size_t(dx - srcRect.left()) * 4;
            q[0] = static_cast<uchar>(std::clamp(int(p.r * 255.0f + 0.5f), 0, 255));
            q[1] = static_cast<uchar>(std::clamp(int(p.g * 255.0f + 0.5f), 0, 255));
            q[2] = static_cast<uchar>(std::clamp(int(p.b * 255.0f + 0.5f), 0, 255));
            q[3] = static_cast<uchar>(std::clamp(
                int(p.a * (cov / 255.0f) * 255.0f + 0.5f), 0, 255));
        }
    }
    if (docRect) *docRect = QRectF(srcRect);
    return out;
}

bool AppState::newDecontaminatedLayer(const QImage& coverage, bool withMask) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || layer->locked ||
        layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return false;
    if (coverage.isNull() || coverage.size() != d->size) return false;
    const QImage cov = coverage.format() == QImage::Format_Grayscale8
                           ? coverage
                           : coverage.convertToFormat(
                                 QImage::Format_Grayscale8);
    if (cov.isNull()) return false;
    // Tight bbox of covered pixels: the lift, the unmix and the new layer
    // all stay bbox-sized instead of document-sized.
    int x0 = cov.width(), y0 = cov.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < cov.height(); ++y) {
        const uchar* row = cov.constScanLine(y);
        for (int x = 0; x < cov.width(); ++x) {
            if (row[x] == 0) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < x0) return false;
    const QRect clip =
        QRect(QPoint(x0, y0), QPoint(x1, y1))
            .intersected(QRect(QPoint(0, 0), d->size));
    if (clip.isEmpty()) return false;
    // Full appearance lift (no clipping mask: decontamination fills edge
    // colours from nearby confident-foreground pixels, and those pixels must
    // carry their raw photo color — lifting masked would leave them black
    // and tint every edge toward black).
    QRectF docRect;
    QImage lifted = copyActiveLayerMasked(QImage(), QRectF(clip), &docRect);
    if (lifted.isNull()) return false;
    const QRect liftedRect = docRect.toAlignedRect();
    if (lifted.size() != liftedRect.size()) return false;
    QImage covCrop = cov.copy(liftedRect);
    if (covCrop.size() != lifted.size()) return false;
    decontaminateStraightRgba(lifted, covCrop);
    if (!withMask) {
        // Plain output = a real cutout: the refined matte is baked into the
        // layer's alpha (background removed, same as Remove Background) with
        // decontaminated edge colors. The mask variant keeps the full lift
        // and rides the matte on the attached layer mask instead.
        for (int y = 0; y < lifted.height(); ++y) {
            const uchar* crow = covCrop.constScanLine(y);
            uchar* srow = lifted.scanLine(y);
            for (int x = 0; x < lifted.width(); ++x) {
                const int c = crow[x];
                uchar* px = srow + x * 4;
                if (c <= 0) {
                    px[3] = 0;
                } else if (c < 255) {
                    px[3] = static_cast<uchar>(
                        (static_cast<int>(px[3]) * c + 127) / 255);
                }
            }
        }
    }
    auto pixels = imageFromQImage(
        lifted.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    if (!pixels) {
        setStatusHint(tr("Could not add layer (unsupported or too large)."));
        return false;
    }
    beginUndoStep();
    LayerItem nl;
    nl.name = tr("Decontaminated");
    nl.kind = LayerItem::Kind::Pixel;
    nl.pixels = std::move(pixels);
    nl.offset = docRect.topLeft();
    nl.scaleX = 1.0;
    nl.scaleY = 1.0;
    ++nl.sourceStamp;
    if (withMask) {
        // Layer-native mask (identity placement, so native == doc pixels):
        // the refined coverage rides along, live and re-editable.
        const std::uint32_t mw = nl.pixels->width();
        const std::uint32_t mh = nl.pixels->height();
        auto mask = std::make_shared<pittore::Image>(mw, mh);
        for (std::uint32_t y = 0; y < mh; ++y) {
            const uchar* srow = covCrop.constScanLine(int(y));
            for (std::uint32_t x = 0; x < mw; ++x)
                mask->at(x, y) = pittore::compute::make_mask_pixel(
                    srow[x] / 255.0f);
        }
        nl.mask = std::move(mask);
        nl.hasMask = true;
        nl.maskLinked = true;
        nl.maskEnabled = true;
        nl.maskOffset = nl.offset;
        nl.maskScaleX = 1.0;
        nl.maskScaleY = 1.0;
        ++nl.maskStamp;
    }
    const int at = qBound(0, d->activeLayer, d->layers.size());
    d->layers.insert(at, std::move(nl));
    d->activeLayer = at;
    d->selectedLayers.clear();
    d->selectedLayers.push_back(at);
    d->rebuildComposite();
    commitUndoStep(tr("New Decontaminated Layer"), QStringLiteral("sparkle"));
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

}  // namespace pittore::ui
