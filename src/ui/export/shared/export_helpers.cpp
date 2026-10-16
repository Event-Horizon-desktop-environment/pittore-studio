#include "ui/export_dialog.h"

#include <QCheckBox>
#include <QColorSpace>
#include <QComboBox>
#include <QBuffer>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRgba64>
#include <QSaveFile>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include <zlib.h>

#include "ui/app_state.h"
#include "ui/project_manager.h"
#include "ui/settings.h"
#include "ui/theme.h"
#include "ui/export/shared/export_helpers.h"

namespace pittore::ui {
namespace export_detail {

constexpr double kPi = 3.14159265358979323846;

// Formats that carry a lossy quality knob. Everything else (PNG, TIFF, XCF,
// PSD/PSB, BMP…) hides the quality row entirely, the same as an Export As dialog.
// The set matches the lossy writers Qt reports Quality support for, plus the
// built-in WebP encoder.
bool formatHasQuality(const QString& fmt) {
    return fmt == QLatin1String("jpg") || fmt == QLatin1String("jpeg") ||
           fmt == QLatin1String("jfif") || fmt == QLatin1String("webp") ||
           fmt == QLatin1String("avif") || fmt == QLatin1String("avci") ||
           fmt == QLatin1String("heif") || fmt == QLatin1String("heic") ||
           fmt == QLatin1String("jxl") || fmt == QLatin1String("jp2") ||
           fmt == QLatin1String("j2k");
}

// Whether the encoder for `fmt` can actually write a progressive/interlaced
// scan. Asked of the real QImageWriter rather than guessed, so formats whose
// handler can't do it (or isn't installed — e.g. the built-in WebP encoder)
// leave the checkbox disabled instead of silently ignoring it.
bool formatSupportsProgressive(const QString& fmt) {
    QImageWriter writer;
    writer.setFormat((fmt == QLatin1String("jpg")) ? QByteArrayLiteral("jpeg")
                                                   : fmt.toLatin1());
    return writer.supportsOption(QImageIOHandler::ProgressiveScanWrite);
}

// Which encoders actually record a resolution / an ICC profile. Verified
// against Qt's writers and the built-in codecs: the built-in PSD/XCF/WebP
// encoders carry neither, TIFF carries DPI only, and BMP carries DPI only.
bool formatWritesDpi(const QString& fmt) {
    return fmt == QLatin1String("png") || fmt == QLatin1String("jpg") ||
           fmt == QLatin1String("jpeg") || fmt == QLatin1String("jfif") ||
           fmt == QLatin1String("bmp") || fmt == QLatin1String("avif") ||
           fmt == QLatin1String("avci") || fmt == QLatin1String("heif") ||
           fmt == QLatin1String("heic") || fmt == QLatin1String("tif") ||
           fmt == QLatin1String("tiff");
}

bool formatWritesIcc(const QString& fmt) {
    return fmt == QLatin1String("png") || fmt == QLatin1String("jpg") ||
           fmt == QLatin1String("jpeg") || fmt == QLatin1String("jfif") ||
           fmt == QLatin1String("avif") || fmt == QLatin1String("avci") ||
           fmt == QLatin1String("heif") || fmt == QLatin1String("heic");
}

// Every suffix the app can export, in a stable order: our built-in codecs
// first, then anything QImageWriter offers at runtime. Aliases (jpeg/tif/heic)
// collapse onto the suffix saveImageFile already understands.
QStringList availableExportFormats() {
    QStringList out{QStringLiteral("png"), QStringLiteral("jpg"),
                    QStringLiteral("psd"), QStringLiteral("xcf"),
                    QStringLiteral("svg"), QStringLiteral("pdf"),
                    QStringLiteral("dxf")};
#ifdef PITTORE_WEBP
    out << QStringLiteral("webp");
#endif
#ifdef PITTORE_TIFF
    out << QStringLiteral("tiff");
#endif
    for (const QByteArray& f : QImageWriter::supportedImageFormats()) {
        QString name = QString::fromLatin1(f).toLower();
        if (name == QLatin1String("jpeg")) name = QStringLiteral("jpg");
        if (name == QLatin1String("tif")) name = QStringLiteral("tiff");
        if (name == QLatin1String("heic")) name = QStringLiteral("heif");
        if (!name.isEmpty() && !out.contains(name)) out << name;
    }
    return out;
}

QColorSpace colorSpaceFor(const QString& name) {
    if (name.startsWith(QLatin1String("sRGB"))) return QColorSpace::SRgb;
    if (name.startsWith(QLatin1String("Adobe"))) return QColorSpace::AdobeRgb;
    if (name.contains(QLatin1String("P3"))) return QColorSpace::DisplayP3;
    return QColorSpace();   // "Don't Convert" / unknown: leave pixels alone
}

QPixmap checkerboard(QSize size, qreal dpr) {
    const qreal useDpr = dpr > 0.0 ? dpr : 1.0;
    QPixmap pm(qMax(1, qRound(size.width() * useDpr)),
               qMax(1, qRound(size.height() * useDpr)));
    pm.setDevicePixelRatio(useDpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const int cell = 8;
    for (int y = 0; y < size.height(); y += cell) {
        for (int x = 0; x < size.width(); x += cell) {
            const bool alt = ((x / cell) + (y / cell)) % 2 == 0;
            p.fillRect(QRect(x, y, cell, cell),
                       alt ? QColor(0x2a, 0x2c, 0x2f) : QColor(0x24, 0x26, 0x28));
        }
    }
    return pm;
}

QString dimStyle(AppState* state) {
    return QStringLiteral("color: %1;").arg(colorsFor(state->theme()).textDim.name());
}

QString presetsFilePath() {
    return QFileInfo(settingsPath()).absolutePath() +
           QStringLiteral("/export_presets.json");
}

// Separable Lanczos-3 resample, used for the "Lanczos" option Qt's own
// QImage::scaled doesn't expose. Operates on premultiplied ARGB so edges stay
// clean; the support widens when downscaling (the usual Lanczos-moiré guard).
QImage lanczosResample(const QImage& src, QSize dst) {
    if (dst.isEmpty() || src.isNull()) return {};
    if (src.size() == dst) return src;
    const QImage in = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int sw = in.width(), sh = in.height();
    const int dw = dst.width(), dh = dst.height();
    QImage tmp(dw, sh, QImage::Format_ARGB32_Premultiplied);
    QImage out(dw, dh, QImage::Format_ARGB32_Premultiplied);

    auto kernel = [](double t) -> double {
        t = std::abs(t);
        if (t < 1e-9) return 1.0;
        if (t >= 3.0) return 0.0;
        const double pt = kPi * t;
        return 3.0 * std::sin(pt) * std::sin(pt / 3.0) / (pt * pt);
    };
    auto clamp8 = [](double v) {
        return int(std::lround(std::min(255.0, std::max(0.0, v))));
    };

    auto pass = [&](const QImage& image, QImage& result, int axis, double scale) {
        const double fs = std::max(1.0, scale);   // filter scale, widens on shrink
        const double support = 3.0 * fs;
        const int iw = image.width(), ih = image.height();
        const int ow = axis == 0 ? result.width() : iw;
        const int oh = axis == 0 ? ih : result.height();
        for (int oy = 0; oy < oh; ++oy) {
            for (int ox = 0; ox < ow; ++ox) {
                const double centre = axis == 0 ? (ox + 0.5) * scale - 0.5
                                                : (oy + 0.5) * scale - 0.5;
                const int span = axis == 0 ? iw : ih;
                const int lo = std::max(0, int(std::ceil(centre - support)));
                const int hi = std::min(span - 1, int(std::floor(centre + support)));
                double a = 0, r = 0, g = 0, b = 0, wsum = 0;
                for (int i = lo; i <= hi; ++i) {
                    const double w = kernel((i - centre) / fs);
                    if (w == 0.0) continue;
                    const QRgb px = axis == 0 ? image.pixel(i, oy) : image.pixel(ox, i);
                    a += w * qAlpha(px);
                    r += w * qRed(px);
                    g += w * qGreen(px);
                    b += w * qBlue(px);
                    wsum += w;
                }
                if (wsum == 0.0) wsum = 1.0;
                result.setPixel(ox, oy,
                                qRgba(clamp8(r / wsum), clamp8(g / wsum),
                                      clamp8(b / wsum), clamp8(a / wsum)));
            }
        }
    };
    pass(in, tmp, 0, double(sw) / dw);
    pass(tmp, out, 1, double(sh) / dh);
    return out;
}

// Map a conventional blend-mode name to the CSS mix-blend-mode an SVG
// renderer understands. Normal (and anything unmapped) returns empty.
QString blendCssFor(const QString& name) {
    static const QHash<QString, QString> kMap{
        {QStringLiteral("Multiply"), QStringLiteral("multiply")},
        {QStringLiteral("Screen"), QStringLiteral("screen")},
        {QStringLiteral("Overlay"), QStringLiteral("overlay")},
        {QStringLiteral("Darken"), QStringLiteral("darken")},
        {QStringLiteral("Lighten"), QStringLiteral("lighten")},
        {QStringLiteral("Color Dodge"), QStringLiteral("color-dodge")},
        {QStringLiteral("Color Burn"), QStringLiteral("color-burn")},
        {QStringLiteral("Hard Light"), QStringLiteral("hard-light")},
        {QStringLiteral("Soft Light"), QStringLiteral("soft-light")},
        {QStringLiteral("Difference"), QStringLiteral("difference")},
        {QStringLiteral("Exclusion"), QStringLiteral("exclusion")},
        {QStringLiteral("Hue"), QStringLiteral("hue")},
        {QStringLiteral("Saturation"), QStringLiteral("saturation")},
        {QStringLiteral("Color"), QStringLiteral("color")},
        {QStringLiteral("Luminosity"), QStringLiteral("luminosity")},
    };
    return kMap.value(name);
}

// Straight-alpha RGBAf (the engine's source of truth) to the premultiplied
// ARGB32 an encoder wants.
QImage engineImageToQImage(const pittore::Image& img) {
    const int w = static_cast<int>(img.width());
    const int h = static_cast<int>(img.height());
    if (w <= 0 || h <= 0) return {};
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    const pittore::RGBAf* src = img.data();
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const pittore::RGBAf& s = src[static_cast<std::size_t>(y) * w + x];
            const int a = qBound(0, int(std::lround(s.a * 255.0f)), 255);
            const int r = qBound(0, int(std::lround(s.r * s.a * 255.0f)), 255);
            const int g = qBound(0, int(std::lround(s.g * s.a * 255.0f)), 255);
            const int b = qBound(0, int(std::lround(s.b * s.a * 255.0f)), 255);
            row[x] = qRgba(r, g, b, a);
        }
    }
    return out;
}

std::string pngBase64Of(const QImage& img) {
    if (img.isNull()) return {};
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    buffer.close();
    return pittore::vector::base64Encode(
        reinterpret_cast<const std::uint8_t*>(png.constData()),
        static_cast<std::size_t>(png.size()));
}


std::string jpegBase64Of(const QImage& img, int quality) {
    if (img.isNull()) return {};
    QByteArray jpg;
    QBuffer buffer(&jpg);
    buffer.open(QIODevice::WriteOnly);
    QImage flat = img;
    if (flat.hasAlphaChannel()) {
        QImage rgb(flat.size(), QImage::Format_RGB32);
        rgb.fill(Qt::white);
        QPainter p(&rgb);
        p.drawImage(0, 0, flat);
        p.end();
        flat = rgb;
    }
    QImageWriter writer(&buffer, "jpeg");
    if (writer.supportsOption(QImageIOHandler::Quality))
        writer.setQuality(qBound(1, quality, 100));
    if (!writer.write(flat)) return {};
    buffer.close();
    return pittore::vector::base64Encode(
        reinterpret_cast<const std::uint8_t*>(jpg.constData()),
        static_cast<std::size_t>(jpg.size()));
}


QImage flattenOverMatte(const QImage& src, const QColor& matte) {
    if (src.isNull()) return {};
    QColor fill = matte.isValid() ? matte : QColor(255, 255, 255);
    QImage out(src.size(), QImage::Format_RGB32);
    out.fill(fill);
    QPainter p(&out);
    p.drawImage(0, 0, src);
    p.end();
    out.setDotsPerMeterX(src.dotsPerMeterX());
    out.setDotsPerMeterY(src.dotsPerMeterY());
    return out;
}


QImage convertPixelFormat(const QImage& src, const QString& pixelFormat,
                          const QColor& matte) {
    if (src.isNull()) return {};
    if (pixelFormat == QLatin1String("doc") || pixelFormat.isEmpty())
        return src;
    const bool hasTransparency =
        src.hasAlphaChannel() && !src.createAlphaMask().isNull();
    // Targets without alpha flatten over the matte (or white).
    if (pixelFormat == QLatin1String("rgb8")) {
        QImage flat = hasTransparency ? flattenOverMatte(src, matte) : src;
        return flat.convertToFormat(QImage::Format_RGB32);
    }
    if (pixelFormat == QLatin1String("rgba8"))
        return src.convertToFormat(QImage::Format_ARGB32);
    if (pixelFormat == QLatin1String("gray8")) {
        QImage flat = hasTransparency ? flattenOverMatte(src, matte) : src;
        return flat.convertToFormat(QImage::Format_Grayscale8);
    }
    if (pixelFormat == QLatin1String("rgb16")) {
        QImage flat = hasTransparency ? flattenOverMatte(src, matte) : src;
        return flat.convertToFormat(QImage::Format_RGBX64);
    }
    if (pixelFormat == QLatin1String("gray16")) {
        QImage flat = hasTransparency ? flattenOverMatte(src, matte) : src;
        return flat.convertToFormat(QImage::Format_Grayscale16);
    }
    return src;
}


// --- Palettized export (median-cut + Floyd-Steinberg) -----------------------
// A box of sampled RGB pixels; splitting the widest channel at the median
// converges on the classic median-cut palette in O(N log K).
namespace {
struct QuantSample {
    int r, g, b;
};
struct QuantBox {
    int lo = 0, hi = 0;   // half-open range into the sample array
    int rMin = 255, rMax = 0, gMin = 255, gMax = 0, bMin = 255, bMax = 0;
};
}  // namespace

QImage quantizeIndexed(const QImage& src, int maxColors, bool dither,
                       bool grayscale) {
    if (src.isNull()) return {};
    maxColors = qBound(2, maxColors, 256);
    const QImage in =
        src.convertToFormat(QImage::Format_ARGB32);
    const int w = in.width(), h = in.height();

    // Transparent pixels reserve index 0; everything else quantizes.
    bool anyTransparent = false;
    for (int y = 0; y < h && !anyTransparent; y += qMax(1, h / 64))
        for (int x = 0; x < w; x += qMax(1, w / 64)) {
            if (qAlpha(in.pixel(x, y)) < 128) {
                anyTransparent = true;
                break;
            }
        }

    QVector<QRgb> table;
    if (anyTransparent) table.append(qRgba(0, 0, 0, 0));

    if (grayscale) {
        // Fixed gray ramp (transparent entry already reserved when needed).
        const int count = anyTransparent ? maxColors - 1 : maxColors;
        for (int i = 0; i < count; ++i) {
            const int v = count > 1 ? (i * 255 + count / 2) / (count - 1) : 0;
            table.append(qRgb(v, v, v));
        }
    } else {
        // Median cut over a bounded sample of the opaque pixels.
        std::vector<QuantSample> samples;
        samples.reserve(200000);
        const int step =
            qMax(1, int(std::sqrt(double(w) * h / 200000.0)));
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                const QRgb px = in.pixel(x, y);
                if (qAlpha(px) < 128) continue;
                samples.push_back({qRed(px), qGreen(px), qBlue(px)});
                if (samples.size() >= 200000) break;
            }
            if (samples.size() >= 200000) break;
        }
        const int want = anyTransparent ? maxColors - 1 : maxColors;
        std::vector<QuantBox> boxes;
        if (!samples.empty()) {
            QuantBox root;
            root.hi = int(samples.size());
            boxes.push_back(root);
        }
        auto boxRange = [&](QuantBox& b) {
            b.rMin = b.gMin = b.bMin = 255;
            b.rMax = b.gMax = b.bMax = 0;
            for (int i = b.lo; i < b.hi; ++i) {
                const auto& s = samples[std::size_t(i)];
                b.rMin = std::min(b.rMin, s.r);
                b.rMax = std::max(b.rMax, s.r);
                b.gMin = std::min(b.gMin, s.g);
                b.gMax = std::max(b.gMax, s.g);
                b.bMin = std::min(b.bMin, s.b);
                b.bMax = std::max(b.bMax, s.b);
            }
        };
        for (auto& b : boxes) boxRange(b);
        while (int(boxes.size()) < want) {
            // Split the box with the widest weighted channel spread.
            int best = -1, bestSpread = 0;
            for (int i = 0; i < int(boxes.size()); ++i) {
                QuantBox& b = boxes[std::size_t(i)];
                if (b.hi - b.lo < 2) continue;
                const int spread =
                    std::max({b.rMax - b.rMin, b.gMax - b.gMin,
                              b.bMax - b.bMin});
                if (spread > bestSpread) {
                    bestSpread = spread;
                    best = i;
                }
            }
            if (best < 0) break;
            QuantBox src = boxes[std::size_t(best)];
            const int rSpread = src.rMax - src.rMin;
            const int gSpread = src.gMax - src.gMin;
            const int bSpread = src.bMax - src.bMin;
            auto* samplesPtr = samples.data();
            if (rSpread >= gSpread && rSpread >= bSpread) {
                std::nth_element(
                    samplesPtr + src.lo, samplesPtr + (src.lo + src.hi) / 2,
                    samplesPtr + src.hi,
                    [](const QuantSample& a, const QuantSample& b) {
                        return a.r < b.r;
                    });
            } else if (gSpread >= bSpread) {
                std::nth_element(
                    samplesPtr + src.lo, samplesPtr + (src.lo + src.hi) / 2,
                    samplesPtr + src.hi,
                    [](const QuantSample& a, const QuantSample& b) {
                        return a.g < b.g;
                    });
            } else {
                std::nth_element(
                    samplesPtr + src.lo, samplesPtr + (src.lo + src.hi) / 2,
                    samplesPtr + src.hi,
                    [](const QuantSample& a, const QuantSample& b) {
                        return a.b < b.b;
                    });
            }
            const int mid = (src.lo + src.hi) / 2;
            if (mid <= src.lo || mid >= src.hi) break;
            boxes[std::size_t(best)].hi = mid;
            boxRange(boxes[std::size_t(best)]);
            QuantBox nb;
            nb.lo = mid;
            nb.hi = src.hi;
            boxRange(nb);
            boxes.push_back(nb);
        }
        for (const QuantBox& b : boxes) {
            if (b.hi <= b.lo) continue;
            long r = 0, g = 0, bl = 0;
            for (int i = b.lo; i < b.hi; ++i) {
                r += samples[std::size_t(i)].r;
                g += samples[std::size_t(i)].g;
                bl += samples[std::size_t(i)].b;
            }
            const int n = b.hi - b.lo;
            table.append(
                qRgb(int((r + n / 2) / n), int((g + n / 2) / n),
                     int((bl + n / 2) / n)));
        }
        if (table.size() == (anyTransparent ? 1 : 0)) {
            // Fully transparent image: keep one opaque entry so the table is
            // never degenerate.
            table.append(qRgb(0, 0, 0));
        }
    }

    QImage out(w, h, QImage::Format_Indexed8);
    out.setColorTable(table);
    out.setDotsPerMeterX(in.dotsPerMeterX());
    out.setDotsPerMeterY(in.dotsPerMeterY());
    const int firstOpaque = anyTransparent ? 1 : 0;
    auto nearest = [&](int r, int g, int b) {
        int best = firstOpaque, bestDist = INT_MAX;
        for (int i = firstOpaque; i < table.size(); ++i) {
            const QRgb c = table[i];
            const int dr = r - qRed(c), dg = g - qGreen(c),
                      db = b - qBlue(c);
            const int dist = dr * dr * 2 + dg * dg * 3 + db * db;
            if (dist < bestDist) {
                bestDist = dist;
                best = i;
            }
        }
        return best;
    };
    if (!dither) {
        for (int y = 0; y < h; ++y) {
            uchar* row = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const QRgb px = in.pixel(x, y);
                if (anyTransparent && qAlpha(px) < 128) {
                    row[x] = 0;
                    continue;
                }
                row[x] = uchar(nearest(qRed(px), qGreen(px), qBlue(px)));
            }
        }
        return out;
    }
    // Floyd-Steinberg, serpentine, in RGB with alpha-tested transparency.
    std::vector<float> errR(std::size_t(w) * h, 0.0f),
        errG(std::size_t(w) * h, 0.0f), errB(std::size_t(w) * h, 0.0f);
    auto errAt = [&](std::vector<float>& e, int x, int y) -> float& {
        return e[std::size_t(y) * w + x];
    };
    for (int y = 0; y < h; ++y) {
        const bool rtl = (y & 1) != 0;
        for (int ix = 0; ix < w; ++ix) {
            const int x = rtl ? (w - 1 - ix) : ix;
            const QRgb px = in.pixel(x, y);
            uchar* row = out.scanLine(y);
            if (anyTransparent && qAlpha(px) < 128) {
                row[x] = 0;
                continue;
            }
            const float r = qBound(0.0f, qRed(px) + errAt(errR, x, y), 255.0f);
            const float g =
                qBound(0.0f, qGreen(px) + errAt(errG, x, y), 255.0f);
            const float b =
                qBound(0.0f, qBlue(px) + errAt(errB, x, y), 255.0f);
            const int idx = nearest(int(r + 0.5f), int(g + 0.5f),
                                    int(b + 0.5f));
            row[x] = uchar(idx);
            const QRgb c = table[idx];
            const float er = r - qRed(c), eg = g - qGreen(c),
                        eb = b - qBlue(c);
            const int xNext = rtl ? x - 1 : x + 1;
            const int xPrev = rtl ? x + 1 : x - 1;
            if (xNext >= 0 && xNext < w) {
                errAt(errR, xNext, y) += er * (7.0f / 16.0f);
                errAt(errG, xNext, y) += eg * (7.0f / 16.0f);
                errAt(errB, xNext, y) += eb * (7.0f / 16.0f);
            }
            if (y + 1 < h) {
                if (xPrev >= 0 && xPrev < w) {
                    errAt(errR, xPrev, y + 1) += er * (3.0f / 16.0f);
                    errAt(errG, xPrev, y + 1) += eg * (3.0f / 16.0f);
                    errAt(errB, xPrev, y + 1) += eb * (3.0f / 16.0f);
                }
                errAt(errR, x, y + 1) += er * (5.0f / 16.0f);
                errAt(errG, x, y + 1) += eg * (5.0f / 16.0f);
                errAt(errB, x, y + 1) += eb * (5.0f / 16.0f);
                if (xNext >= 0 && xNext < w) {
                    errAt(errR, xNext, y + 1) += er * (1.0f / 16.0f);
                    errAt(errG, xNext, y + 1) += eg * (1.0f / 16.0f);
                    errAt(errB, xNext, y + 1) += eb * (1.0f / 16.0f);
                }
            }
        }
    }
    return out;
}


// --- HDR stills (Rec.2020 PQ/HLG PNG) --------------------------------------
namespace {

// Exact sRGB EOTF^-1: gamma-encoded 0..1 float to linear light.
double srgbToLinear(double v) {
    if (v <= 0.04045) return v / 12.92;
    return std::pow((v + 0.055) / 1.055, 2.4);
}

// ST 2084 (PQ) OETF, Y normalized so 1.0 = 10000 nits.
double pqOetf(double y) {
    static constexpr double m1 = 0.1593017578125;
    static constexpr double m2 = 78.84375;
    static constexpr double c1 = 0.8359375;
    static constexpr double c2 = 18.8515625;
    static constexpr double c3 = 18.6875;
    y = std::clamp(y, 0.0, 1.0);
    const double ym = std::pow(y, m1);
    return std::pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

// HLG OETF (BT.2100), E in scene-linear 0..1.
double hlgOetf(double e) {
    static constexpr double a = 0.17883277;
    static constexpr double b = 0.28466892;
    static constexpr double c = 0.55991073;
    e = std::max(0.0, e);
    if (e <= 1.0 / 12.0) return std::sqrt(3.0 * e);
    return a * std::log(12.0 * e - b) + c;
}

// Linear sRGB to linear BT.2020 (D65), row-major.
void srgbLinearToBt2020(float& r, float& g, float& b) {
    const float lr = r, lg = g, lb = b;
    r = 0.627404f * lr + 0.329283f * lg + 0.043313f * lb;
    g = 0.069097f * lr + 0.919540f * lg + 0.011362f * lb;
    b = 0.016391f * lr + 0.088013f * lg + 0.895595f * lb;
}

std::uint32_t crc32Table[256];
bool crc32Ready = false;

void ensureCrc32() {
    if (crc32Ready) return;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32Table[i] = c;
    }
    crc32Ready = true;
}

std::uint32_t crc32Of(const char* data, std::size_t n) {
    ensureCrc32();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i)
        c = crc32Table[(c ^ std::uint8_t(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void appendBe32(QByteArray& out, std::uint32_t v) {
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

std::uint32_t readBe32(const QByteArray& in, int pos) {
    return (std::uint32_t(std::uint8_t(in[pos])) << 24) |
           (std::uint32_t(std::uint8_t(in[pos + 1])) << 16) |
           (std::uint32_t(std::uint8_t(in[pos + 2])) << 8) |
           std::uint32_t(std::uint8_t(in[pos + 3]));
}

}  // namespace

QImage encodeHdrCodes(const QImage& src, int transferFunction, bool fullRange,
                       bool bt2020) {
    if (src.isNull()) return {};
    // SDR white maps to 100 nits (Y=0.01) under PQ and to HLG 75% (scene
    // 0.265, the HLG reference-white level), so the file displays at the
    // same brightness as the canvas on a mastering display.
    const double scale = transferFunction == 2 ? 0.265 : 0.01;
    const QImage in = src.convertToFormat(QImage::Format_ARGB32);
    QImage out(in.size(), QImage::Format_RGBA64);
    out.setDotsPerMeterX(in.dotsPerMeterX());
    out.setDotsPerMeterY(in.dotsPerMeterY());
    for (int y = 0; y < in.height(); ++y) {
        const QRgb* srow =
            reinterpret_cast<const QRgb*>(in.constScanLine(y));
        QRgba64* drow = reinterpret_cast<QRgba64*>(out.scanLine(y));
        for (int x = 0; x < in.width(); ++x) {
            const QRgb px = srow[x];
            float r = float(srgbToLinear(qRed(px) / 255.0));
            float g = float(srgbToLinear(qGreen(px) / 255.0));
            float b = float(srgbToLinear(qBlue(px) / 255.0));
            const float a = qAlpha(px) / 255.0f;
            if (bt2020) srgbLinearToBt2020(r, g, b);
            r = std::max(0.0f, r);
            g = std::max(0.0f, g);
            b = std::max(0.0f, b);
            double vr, vg, vb;
            if (transferFunction == 2) {
                vr = hlgOetf(r * scale);
                vg = hlgOetf(g * scale);
                vb = hlgOetf(b * scale);
            } else {
                vr = pqOetf(r * scale);
                vg = pqOetf(g * scale);
                vb = pqOetf(b * scale);
            }
            auto code = [&](double v) -> quint16 {
                v = std::clamp(v, 0.0, 1.0);
                if (fullRange) return quint16(std::lround(v * 65535.0));
                return quint16(
                    std::lround(4096.0 + v * 56064.0));   // narrow: 64..940 << 6
            };
            drow[x] = qRgba64(code(vr), code(vg), code(vb),
                              quint16(std::lround(
                                  std::clamp(a, 0.0f, 1.0f) * 65535.0f)));
        }
    }
    return out;
}


QByteArray insertCIcpChunk(const QByteArray& png, int transferFunction,
                           bool fullRange, bool bt2020) {
    static const char kSig[8] = {(char)0x89, 'P', 'N', 'G',
                                 '\r', '\n', (char)0x1A, '\n'};
    if (png.size() < 8 || std::memcmp(png.constData(), kSig, 8) != 0)
        return png;
    int pos = 8;
    int idatPos = -1;
    while (pos + 8 <= png.size()) {
        const std::uint32_t len = readBe32(png, pos);
        const QByteArray type = png.mid(pos + 4, 4);
        if (type == QByteArrayLiteral("IDAT")) {
            idatPos = pos;
            break;
        }
        if (len > std::uint32_t(png.size())) break;   // corrupt: give up
        pos += 8 + int(len) + 4;
    }
    if (idatPos < 0) return png;
    QByteArray chunk;
    appendBe32(chunk, 4);
    chunk.append("cICP", 4);
    chunk.append(char(bt2020 ? 9 : 1));                      // primaries
    chunk.append(char(transferFunction == 2 ? 18 : 16));     // HLG / PQ
    chunk.append(char(0));                                   // identity matrix
    chunk.append(char(fullRange ? 1 : 0));
    const std::uint32_t crc =
        crc32Of(chunk.constData() + 4, std::size_t(chunk.size() - 4));
    appendBe32(chunk, crc);
    QByteArray out = png.left(idatPos);
    out += chunk;
    out += png.mid(idatPos);
    return out;
}


QByteArray encodePngWithCIcp(const QImage& img, int transferFunction,
                             bool fullRange, bool bt2020) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    if (!writer.write(img)) return {};
    buffer.close();
    return insertCIcpChunk(png, transferFunction, fullRange, bt2020);
}


// --- Max-lossless PNG recompress --------------------------------------------
// Qt's PNG writer uses middle-of-the-road deflate settings and no per-row
// filter search, leaving 10-40% on the table. This pass parses its output,
// un-filters the scanlines, then re-encodes with an adaptive minimum-sum
// filter choice per row plus zlib level 9 — same pixels, smaller file.
// Along the way it folds lossless-only wins Qt never applies: RGB/RGBA
// images that are actually gray become gray/gray+alpha, and tiny palettes
// repack to 4/2/1-bit depth. Every other chunk (PLTE, tRNS, pHYs, iCCP,
// cICP, text) rides through byte-identical. Anything unrecognized (or any
// failure) returns the input untouched.
namespace {

int pngPaeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b),
              pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool inflateZlib(const std::uint8_t* in, std::size_t inSize,
                 std::vector<std::uint8_t>& out, std::size_t expectSize) {
    out.clear();
    out.resize(expectSize);
    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(in);
    strm.avail_in = static_cast<uInt>(inSize);
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(expectSize);
    if (inflateInit(&strm) != Z_OK) {
        out.clear();
        return false;
    }
    const int rc = inflate(&strm, Z_FINISH);
    const bool ok =
        (rc == Z_STREAM_END) && strm.total_out == expectSize;
    inflateEnd(&strm);
    if (!ok) out.clear();
    return ok;
}

bool deflateZlib(const std::uint8_t* in, std::size_t inSize,
                 std::vector<std::uint8_t>& out) {
    out.clear();
    uLong bound = compressBound(static_cast<uLong>(inSize));
    out.resize(bound);
    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(in);
    strm.avail_in = static_cast<uInt>(inSize);
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(bound);
    if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK) {
        out.clear();
        return false;
    }
    const int rc = deflate(&strm, Z_FINISH);
    const bool ok = (rc == Z_STREAM_END);
    deflateEnd(&strm);
    if (!ok) {
        out.clear();
        return false;
    }
    out.resize(strm.total_out);
    return true;
}

}  // namespace

QByteArray optimizePngLossless(const QByteArray& png) {
    static const char kSig[8] = {(char)0x89, 'P', 'N', 'G',
                                 '\r', '\n', (char)0x1A, '\n'};
    if (png.size() < 8 || std::memcmp(png.constData(), kSig, 8) != 0)
        return png;
    struct RawChunk {
        int pos = 0;   // offset of the length field
        int len = 0;   // data length
        QByteArray type;
    };
    std::vector<RawChunk> chunks;
    std::vector<std::uint8_t> idat;
    int pos = 8;
    int firstIdatPos = -1;
    bool hasActl = false;
    bool hasTrns = false;
    while (pos + 8 <= png.size()) {
        const std::uint32_t len = readBe32(png, pos);
        const QByteArray type = png.mid(pos + 4, 4);
        if (len > std::uint32_t(png.size()) ||
            pos + 8 + int(len) + 4 > png.size())
            return png;
        if (type == QByteArrayLiteral("acTL") ||
            type == QByteArrayLiteral("fdAT"))
            hasActl = true;   // animated: hands off
        if (type == QByteArrayLiteral("tRNS")) hasTrns = true;
        if (type == QByteArrayLiteral("IDAT")) {
            if (firstIdatPos < 0) firstIdatPos = pos;
            idat.insert(idat.end(),
                        reinterpret_cast<const std::uint8_t*>(png.constData()) +
                            pos + 8,
                        reinterpret_cast<const std::uint8_t*>(png.constData()) +
                            pos + 8 + len);
        } else {
            chunks.push_back({pos, int(len), type});
        }
        pos += 8 + int(len) + 4;
        if (type == QByteArrayLiteral("IEND")) break;
    }
    if (hasActl || idat.empty() || chunks.empty() || firstIdatPos < 0)
        return png;
    if (chunks.front().type != QByteArrayLiteral("IHDR") ||
        chunks.front().len != 13)
        return png;
    const std::uint8_t* ihdr =
        reinterpret_cast<const std::uint8_t*>(png.constData()) +
        chunks.front().pos + 8;
    const std::uint32_t w = (std::uint32_t(ihdr[0]) << 24) |
                            (std::uint32_t(ihdr[1]) << 16) |
                            (std::uint32_t(ihdr[2]) << 8) | ihdr[3];
    const std::uint32_t h = (std::uint32_t(ihdr[4]) << 24) |
                            (std::uint32_t(ihdr[5]) << 16) |
                            (std::uint32_t(ihdr[6]) << 8) | ihdr[7];
    const int bitDepth = ihdr[8];
    const int colorType = ihdr[9];
    const int interlace = ihdr[12];
    if (interlace != 0 || w == 0 || h == 0) return png;
    if (std::uint64_t(w) * h > (1ull << 26))
        return png;   // 64MP transient cap (3x raw in flight)
    int channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return png;
    }
    if (bitDepth != 8 && bitDepth != 16) {
        // Sub-byte indexed input: recompress as-is (repacking gains nothing).
        if (!(colorType == 3 &&
              (bitDepth == 1 || bitDepth == 2 || bitDepth == 4)))
            return png;
    }
    const bool lowBit = colorType == 3 && bitDepth < 8;
    const int inBpp = lowBit ? 1 : std::max(1, (bitDepth / 8) * channels);
    const std::uint64_t rowBytes =
        lowBit ? (std::uint64_t(w) * bitDepth + 7) / 8
               : std::uint64_t(w) * inBpp;
    const std::uint64_t expect = std::uint64_t(h) * (rowBytes + 1);
    std::vector<std::uint8_t> raw;
    if (!inflateZlib(idat.data(), idat.size(), raw, std::size_t(expect)))
        return png;
    // Unfilter to raw sample bytes.
    std::vector<std::uint8_t> pixels(std::uint64_t(h) * rowBytes);
    std::vector<std::uint8_t> prev(rowBytes, 0), cur(rowBytes, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* src = raw.data() + std::uint64_t(y) * (rowBytes + 1);
        const int f = src[0];
        if (f < 0 || f > 4) return png;
        for (std::uint64_t x = 0; x < rowBytes; ++x) {
            const int a = x >= std::uint64_t(inBpp) ? cur[x - inBpp] : 0;
            const int b = prev[x];
            const int c = x >= std::uint64_t(inBpp) ? prev[x - inBpp] : 0;
            int v = src[1 + x];
            switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += pngPaeth(a, b, c); break;
                default: break;
            }
            cur[x] = std::uint8_t(v & 0xFF);
        }
        std::copy(cur.begin(), cur.end(),
                  pixels.begin() + std::uint64_t(y) * rowBytes);
        prev = cur;
    }
    // Lossless transforms: gray-fold RGB(A), sub-byte repack of tiny palettes.
    int outDepth = bitDepth;
    int outType = colorType;
    std::vector<std::uint8_t> repacked;
    const std::uint8_t* samples = pixels.data();
    std::uint64_t outRowBytes = rowBytes;
    int outBpp = inBpp;
    if (!lowBit && (colorType == 2 || colorType == 6) && bitDepth <= 16 &&
        !hasTrns) {
        // Gray detect: every pixel has r == g == b. Skipped when a tRNS
        // chunk is present (its color key would not survive the fold).
        const int ch = channels;
        const int bps = bitDepth / 8;
        bool gray = true;
        bool opaque = true;
        for (std::uint32_t y = 0; y < h && gray; ++y) {
            const std::uint8_t* row =
                pixels.data() + std::uint64_t(y) * rowBytes;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint8_t* px = row + std::uint64_t(x) * ch * bps;
                if (bps == 1) {
                    if (px[0] != px[1] || px[1] != px[2]) {
                        gray = false;
                        break;
                    }
                    if (ch == 4 && px[3] != 255) opaque = false;
                } else {
                    if (std::memcmp(px, px + 2, 2) != 0 ||
                        std::memcmp(px + 2, px + 4, 2) != 0) {
                        gray = false;
                        break;
                    }
                    if (ch == 4 &&
                        std::memcmp(px + 6, "\xFF\xFF", 2) != 0)
                        opaque = false;
                }
            }
        }
        if (gray) {
            // Rec.709 luma into gray (+alpha) samples.
            repacked.resize(std::uint64_t(h) * w *
                            (opaque ? 1 : 2) * bps);
            for (std::uint32_t y = 0; y < h; ++y) {
                const std::uint8_t* srow =
                    pixels.data() + std::uint64_t(y) * rowBytes;
                std::uint8_t* drow =
                    repacked.data() +
                    std::uint64_t(y) * w * (opaque ? 1 : 2) * bps;
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint8_t* px =
                        srow + std::uint64_t(x) * ch * bps;
                    if (bps == 1) {
                        drow[x * (opaque ? 1 : 2)] = static_cast<std::uint8_t>(
                            (13933u * px[0] + 46871u * px[1] +
                             4732u * px[2] + 32768u) >>
                            16);
                        if (!opaque) drow[x * 2 + 1] = px[3];
                    } else {
                        const std::uint32_t r =
                            (std::uint32_t(px[0]) << 8) | px[1];
                        const std::uint32_t g =
                            (std::uint32_t(px[2]) << 8) | px[3];
                        const std::uint32_t b =
                            (std::uint32_t(px[4]) << 8) | px[5];
                        const std::uint32_t v =
                            (13933u * r + 46871u * g + 4732u * b + 32768u) >>
                            16;
                        drow[x * (opaque ? 2 : 4)] =
                            std::uint8_t((v >> 8) & 0xFF);
                        drow[x * (opaque ? 2 : 4) + 1] =
                            std::uint8_t(v & 0xFF);
                        if (!opaque) {
                            drow[x * 4 + 2] = px[6];
                            drow[x * 4 + 3] = px[7];
                        }
                    }
                }
            }
            samples = repacked.data();
            outType = opaque ? 0 : 4;
            outRowBytes = std::uint64_t(w) * (opaque ? 1 : 2) * bps;
            outBpp = (opaque ? 1 : 2) * bps;
        }
    }
    if (!lowBit && colorType == 3 && bitDepth == 8) {
        // Tiny palettes repack losslessly: <=16 colors to 4-bit, <=4 to
        // 2-bit, <=2 to 1-bit.
        std::uint8_t mx = 0;
        for (std::uint8_t v : pixels) mx = std::max(mx, v);
        int depth = 8;
        if (mx <= 1) depth = 1;
        else if (mx <= 3) depth = 2;
        else if (mx <= 15) depth = 4;
        if (depth < 8) {
            outRowBytes = (std::uint64_t(w) * depth + 7) / 8;
            repacked.assign(std::uint64_t(h) * outRowBytes, 0);
            const int perByte = 8 / depth;
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint8_t v =
                        pixels[std::uint64_t(y) * rowBytes + x] &
                        ((1 << depth) - 1);
                    repacked[std::uint64_t(y) * outRowBytes + x / perByte] |=
                        std::uint8_t(v << (8 - (x % perByte + 1) * depth));
                }
            }
            samples = repacked.data();
            outDepth = depth;
            outBpp = 1;
        }
    }
    // Adaptive minimum-sum filtering, then zlib-9.
    std::vector<std::uint8_t> filtered(std::uint64_t(h) * (outRowBytes + 1));
    std::vector<std::uint8_t> trial(outRowBytes);
    std::vector<std::uint8_t> pprev(outRowBytes, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* row = samples + std::uint64_t(y) * outRowBytes;
        const std::uint8_t* prow =
            y > 0 ? samples + std::uint64_t(y - 1) * outRowBytes : pprev.data();
        // Filter 0 (None) baseline.
        std::uint64_t bestSum = 0;
        for (std::uint64_t x = 0; x < outRowBytes; ++x)
            bestSum += std::uint8_t(std::abs(int(std::int8_t(row[x]))));
        int best = 0;
        std::vector<std::uint8_t> bestRow(row, row + outRowBytes);
        for (int f = 1; f <= 4; ++f) {
            std::uint64_t sum = 0;
            for (std::uint64_t x = 0; x < outRowBytes; ++x) {
                const int a =
                    x >= std::uint64_t(outBpp) ? row[x - outBpp] : 0;
                const int b = prow[x];
                const int c =
                    x >= std::uint64_t(outBpp) ? prow[x - outBpp] : 0;
                int v = row[x];
                switch (f) {
                    case 1: v -= a; break;
                    case 2: v -= b; break;
                    case 3: v -= (a + b) / 2; break;
                    case 4: v -= pngPaeth(a, b, c); break;
                    default: break;
                }
                trial[x] = std::uint8_t(v & 0xFF);
                sum += std::uint8_t(std::abs(int(std::int8_t(trial[x]))));
                if (sum >= bestSum) break;   // early out, ties keep filter 0
            }
            if (sum < bestSum) {
                bestSum = sum;
                best = f;
                bestRow.assign(trial.begin(), trial.end());
            }
        }
        filtered[std::uint64_t(y) * (outRowBytes + 1)] =
            std::uint8_t(best);
        std::copy(bestRow.begin(), bestRow.end(),
                  filtered.begin() + std::uint64_t(y) * (outRowBytes + 1) + 1);
    }
    std::vector<std::uint8_t> zbytes;
    if (!deflateZlib(filtered.data(), filtered.size(), zbytes)) return png;
    // Reassemble: every original chunk verbatim, new IDATs at the first
    // old-IDAT position, IHDR rewritten when depth/type changed.
    QByteArray out;
    out += QByteArray(kSig, 8);
    bool idatEmitted = false;
    auto emitIdat = [&] {
        static const std::size_t kChunk = 32768;
        for (std::size_t o = 0; o < zbytes.size(); o += kChunk) {
            const std::size_t n = std::min(kChunk, zbytes.size() - o);
            appendBe32(out, std::uint32_t(n));
            out.append("IDAT", 4);
            out.append(reinterpret_cast<const char*>(zbytes.data() + o),
                       int(n));
            const std::uint32_t crc = crc32Of(out.constData() + out.size() -
                                                  int(n) - 4,
                                              std::size_t(n) + 4);
            appendBe32(out, crc);
        }
    };
    for (const RawChunk& c : chunks) {
        // New IDATs take the original IDAT block's position, keeping every
        // other chunk (PLTE, tRNS, pHYs, iCCP, cICP, text) in place.
        if (!idatEmitted && c.pos > firstIdatPos) {
            idatEmitted = true;
            emitIdat();
        }
        if (c.type == QByteArrayLiteral("IHDR") &&
            (outDepth != bitDepth || outType != colorType)) {
            QByteArray ihdr = png.mid(c.pos + 8, 13);
            ihdr[8] = char(outDepth);
            ihdr[9] = char(outType);
            appendBe32(out, 13);
            out.append("IHDR", 4);
            out.append(ihdr);
            const std::uint32_t crc =
                crc32Of(out.constData() + out.size() - 13 - 4, 17);
            appendBe32(out, crc);
            continue;
        }
        out += png.mid(c.pos, 8 + c.len + 4);
    }
    if (!idatEmitted) emitIdat();
    return out;
}

}  // namespace export_detail
}  // namespace pittore::ui
