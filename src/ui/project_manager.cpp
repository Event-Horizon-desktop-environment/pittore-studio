#include "ui/dpi_pixmap.h"
#include "ui/project_manager.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QComboBox>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "engine/io/af.h"
#include "engine/io/project.h"
#include "engine/io/psd.h"
#include "engine/io/tiff.h"
#include "engine/io/webp.h"
#include "engine/io/xcf.h"
#include "engine/io/zip.h"
#include "engine/color/icc_convert.h"
#include "engine/core/log.h"

#include "ui/app_state.h"

namespace pittore::ui {

// True for a suffix the app can open: the native project (.psc, legacy .ifp),
// the built-in codecs (PSD/PSB layered + flattened, XCF, KRA) and every suffix
// Qt's image plugins can read at runtime. Shared by the open dialog filter and
// the drag-and-drop targets (window + start page).
bool suffixIsOpenable(const QString& suffix) {
    const QString s = suffix.toLower();
    if (isNativeProjectSuffix(s) || s == QLatin1String("psd") ||
        s == QLatin1String("psb") || s == QLatin1String("xcf") ||
        s == QLatin1String("kra") || s == QLatin1String("svg") ||
        s == QLatin1String("af") || s == QLatin1String("afphoto") ||
        s == QLatin1String("afdesign") || s == QLatin1String("afpub"))
        return true;
#ifdef PITTORE_WEBP
    if (s == QLatin1String("webp")) return true;
#endif
#ifdef PITTORE_TIFF
    if (s == QLatin1String("tif") || s == QLatin1String("tiff")) return true;
#endif
    return QImageReader::supportedImageFormats().contains(s.toLatin1());
}

// True when any of the dropped URLs is a local file the app can open.
bool dropHasOpenableFiles(const QMimeData* mime) {
    if (!mime || !mime->hasUrls()) return false;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        if (suffixIsOpenable(QFileInfo(url.toLocalFile()).suffix())) return true;
    }
    return false;
}

// File-dialog filter covering every importable format: the native project plus
// everything the Qt image plugins can read at runtime, with the document
// formats (PSD/PSB/XCF/KRA) always listed even where no system plugin exists —
// the built-in codecs read those.
QString imageOpenFilter() {
    QStringList suffixes{QStringLiteral("psc"), QStringLiteral("ifp"), QStringLiteral("psd"),
                         QStringLiteral("psb"), QStringLiteral("xcf"),
                         QStringLiteral("kra"), QStringLiteral("svg"),
                         QStringLiteral("af"), QStringLiteral("afphoto"),
                         QStringLiteral("afdesign"), QStringLiteral("afpub")};
#ifdef PITTORE_WEBP
    suffixes << QStringLiteral("webp");
#endif
#ifdef PITTORE_TIFF
    suffixes << QStringLiteral("tif") << QStringLiteral("tiff");
#endif
    for (const QByteArray& f : QImageReader::supportedImageFormats())
        suffixes << QString::fromLatin1(f).toLower();
    suffixes.sort();
    suffixes.removeDuplicates();
    QStringList patterns;
    for (const QString& s : suffixes)
        patterns << QStringLiteral("*.%1").arg(s);
    return QObject::tr("All Supported Images (%1)").arg(patterns.join(QLatin1Char(' '))) +
           QStringLiteral(";;") + QObject::tr("All Files (*)");
}

// File-dialog filter for exporting the flattened document: PSD/XCF via the
// built-in codecs plus every suffix QImageWriter supports at runtime.
QString imageExportFilter() {
    QStringList suffixes{QStringLiteral("psd"), QStringLiteral("psb"),
                         QStringLiteral("xcf"), QStringLiteral("svg")};
#ifdef PITTORE_WEBP
    suffixes << QStringLiteral("webp");
#endif
#ifdef PITTORE_TIFF
    suffixes << QStringLiteral("tif") << QStringLiteral("tiff");
#endif
    for (const QByteArray& f : QImageWriter::supportedImageFormats())
        suffixes << QString::fromLatin1(f).toLower();
    suffixes.sort();
    suffixes.removeDuplicates();
    QStringList patterns;
    for (const QString& s : suffixes)
        patterns << QStringLiteral("*.%1").arg(s);
    return QObject::tr("All Supported Formats (%1)").arg(patterns.join(QLatin1Char(' '))) +
           QStringLiteral(";;") + QObject::tr("All Files (*)");
}

namespace {

std::string toStd(const QString& s) { return s.toStdString(); }

QString fromStd(const std::string& s) { return QString::fromStdString(s); }

// Mask coverage bridge: the codec stores one u16 sample per mask pixel while
// the meta layer keeps the live model's opaque-grey RGBA64 image (coverage in
// R, A forced opaque). Both directions are exact for 8-bit-step coverage.
std::vector<std::uint16_t> maskCoverageFromQImage(const QImage& q) {
    if (q.isNull() || q.width() <= 0 || q.height() <= 0) return {};
    const QImage src = q.convertToFormat(QImage::Format_RGBA64);
    if (src.isNull()) return {};
    std::vector<std::uint16_t> out(static_cast<std::size_t>(src.width()) *
                                   src.height());
    for (int y = 0; y < src.height(); ++y) {
        const auto* row =
            reinterpret_cast<const quint16*>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
            out[static_cast<std::size_t>(y) * src.width() + x] =
                row[x * 4 + 0];
    }
    return out;
}

QImage maskCoverageToQImage(const std::vector<std::uint16_t>& samples, int w,
                            int h) {
    if (w <= 0 || h <= 0) return {};
    if (samples.size() != static_cast<std::size_t>(w) * h) return {};
    QImage img(w, h, QImage::Format_RGBA64);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<quint16*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const quint16 c =
                samples[static_cast<std::size_t>(y) * w + x];
            row[x * 4 + 0] = c;
            row[x * 4 + 1] = c;
            row[x * 4 + 2] = c;
            row[x * 4 + 3] = 65535;
        }
    }
    return img;
}

// Snapshot the flat composite into a small straight-alpha preview (≤512 px)
// for embedding in the project file. The composite is premultiplied ARGB32, so
// it is first drawn over opaque white (premultiplied-src-over-opaque-dest
// yields correct straight pixels); saving its raw bytes would print dark halos
// wherever alpha is partial.
QImage previewImage(const QImage& composite, int maxEdge = 512) {
    if (composite.isNull()) return {};
    QImage flat(composite.size(), QImage::Format_ARGB32);
    flat.fill(Qt::white);
    QPainter p(&flat);
    p.drawImage(0, 0, composite);
    p.end();
    const double scale =
        qMin(1.0, static_cast<double>(maxEdge) /
                      qMax(1.0, static_cast<double>(
                                    std::max(composite.width(), composite.height()))));
    if (scale >= 1.0) return flat;
    return flat.scaled(qMax(1, qRound(composite.width() * scale)),
                       qMax(1, qRound(composite.height() * scale)),
                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

}  // namespace

// The IFP codec stores straight 16-bit sample values big-endian (verified in
// tests), so straight RGBA64 scanline values are what lands in the file — and
// what comes back out. We use that instead of Qt's premultiplied semantics,
// which would double-modulate alpha.
QImage imageToStraightRgba64(const pittore::Image& img) {
    if (!img.width() || !img.height()) return {};
    QImage out(static_cast<int>(img.width()), static_cast<int>(img.height()),
               QImage::Format_RGBA64);
    const pittore::RGBAf* src = img.data();
    for (int y = 0; y < out.height(); ++y) {
        auto* row = reinterpret_cast<quint16*>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const pittore::RGBAf& p = src[y * out.width() + x];
            row[x * 4 + 0] = static_cast<quint16>(qBound(0.0, p.r * 65535.0, 65535.0));
            row[x * 4 + 1] = static_cast<quint16>(qBound(0.0, p.g * 65535.0, 65535.0));
            row[x * 4 + 2] = static_cast<quint16>(qBound(0.0, p.b * 65535.0, 65535.0));
            row[x * 4 + 3] = static_cast<quint16>(qBound(0.0, p.a * 65535.0, 65535.0));
        }
    }
    return out;
}

std::shared_ptr<pittore::Image> straightRgba64ToImage(const QImage& q) {
    if (q.isNull()) return nullptr;
    // Avoid a 3.3GB duplicate when the source is already RGBA64 (the TIFF
    // banded path decodes straight into RGBA64).
    const QImage* srcPtr = &q;
    QImage conv;
    if (q.format() != QImage::Format_RGBA64) {
        conv = q.convertToFormat(QImage::Format_RGBA64);
        if (conv.isNull()) return nullptr;
        srcPtr = &conv;
    }
    const QImage& src = *srcPtr;
    auto out = std::make_shared<pittore::Image>(
        static_cast<std::uint32_t>(src.width()), static_cast<std::uint32_t>(src.height()));
    constexpr double kInv = 1.0 / 65535.0;
    for (int y = 0; y < src.height(); ++y) {
        const auto* row = reinterpret_cast<const quint16*>(src.constScanLine(y));
        pittore::RGBAf* dst = out->data() + static_cast<std::size_t>(y) * src.width();
        for (int x = 0; x < src.width(); ++x) {
            dst[x] = {static_cast<float>(row[x * 4 + 0] * kInv),
                      static_cast<float>(row[x * 4 + 1] * kInv),
                      static_cast<float>(row[x * 4 + 2] * kInv),
                      static_cast<float>(row[x * 4 + 3] * kInv)};
        }
    }
    return out;
}

QString projectsRootDir() {
    if (const char* env = std::getenv("PITTORE_PROJECTS_DIR"); env && *env)
        return QString::fromLocal8Bit(env);
    if (const char* legacy = std::getenv("INFINITY_PROJECTS_DIR"); legacy && *legacy)
        return QString::fromLocal8Bit(legacy);
    const QString pics =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString base = pics.isEmpty() ? QDir::homePath() + QStringLiteral("/Pictures")
                                        : pics;
    return base + QStringLiteral("/Pittore Studio/Projects");
}

QString projectPathForName(const QString& name) {
    QDir root(projectsRootDir());
    return root.filePath(name + QStringLiteral(".psc"));
}

bool isNativeProjectSuffix(const QString& suffix) {
    const QString s = suffix.toLower();
    return s == QLatin1String("psc") || s == QLatin1String("ifp");
}

// Copy a straight RGBA QImage into the codec's 16-bit sample vector. The
// preview passed to the codec is already drawn over opaque white (see
// previewImage), so its scanlines hold straight values; layout conversion only.
std::vector<std::uint16_t> rgba16FromQImage(const QImage& q) {
    if (q.isNull()) return {};
    const QImage src = q.convertToFormat(QImage::Format_RGBA64);
    if (src.isNull()) return {};
    std::vector<std::uint16_t> out(static_cast<std::size_t>(src.width()) *
                                   src.height() * 4);
    for (int y = 0; y < src.height(); ++y) {
        const auto* row = reinterpret_cast<const quint16*>(src.constScanLine(y));
        std::copy(row, row + static_cast<std::ptrdiff_t>(src.width()) * 4,
                  out.data() + static_cast<std::size_t>(y) * src.width() * 4);
    }
    return out;
}

QImage qimageFromRgba16(const std::vector<std::uint16_t>& samples, int w, int h) {
    if (w <= 0 || h <= 0) return {};
    if (samples.size() != static_cast<std::size_t>(w) * h * 4) return {};
    QImage img(w, h, QImage::Format_RGBA64);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<quint16*>(img.scanLine(y));
        std::copy_n(samples.data() + static_cast<std::size_t>(y) * w * 4,
                    static_cast<std::ptrdiff_t>(w) * 4, row);
    }
    return img;
}

bool saveProjectFile(const QString& path, const ProjectFileData& data,
                     QString* error) {
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty()) {
        QDir libDir(dir);
        if (!libDir.exists() && !libDir.mkpath(QStringLiteral("."))) {
            if (error) *error = QObject::tr("Could not create the project library");
            return false;
        }
    }

    pittore::io::ProjectFileDoc doc;
    doc.width = static_cast<std::uint32_t>(qMax(1, data.size.width()));
    doc.height = static_cast<std::uint32_t>(qMax(1, data.size.height()));
    doc.dpi = static_cast<std::uint16_t>(qBound(1, data.dpi, 65535));
    doc.colorMode =
        data.colorMode.startsWith(QStringLiteral("Grayscale"), Qt::CaseInsensitive) ? 1
        : data.colorMode.startsWith(QStringLiteral("CMYK"), Qt::CaseInsensitive) ? 4
                                                                                : 3;
    doc.name = toStd(data.name);
    doc.colorModeName = toStd(data.colorMode);
    doc.background = toStd(data.background);
    doc.icc.assign(data.iccProfile.cbegin(), data.iccProfile.cend());
    for (const ProjectLayerMeta& m : data.layers) {
        pittore::io::ProjectLayerFile layer;
        layer.name = toStd(m.name);
        layer.kind = static_cast<std::uint8_t>(m.kind);
        layer.visible = m.visible;
        layer.locked = m.locked;
        layer.opacity = static_cast<std::uint16_t>(qBound(0, m.opacity, 100));
        layer.fill = static_cast<std::uint16_t>(qBound(0, m.fill, 100));
        layer.blend = toStd(m.blendMode);
        layer.offsetX = m.offset.x();
        layer.offsetY = m.offset.y();
        layer.scaleX = m.scaleX;
        layer.scaleY = m.scaleY;
        layer.lockTransparency = m.lockTransparency;
        layer.clipped = m.clipped;
        layer.indent = m.indent;
        if (m.hasMask && !m.mask.isNull() && m.mask.width() > 0 &&
            m.mask.height() > 0) {
            const std::vector<std::uint16_t> cov =
                maskCoverageFromQImage(m.mask);
            if (cov.size() == static_cast<std::size_t>(m.mask.width()) *
                                 m.mask.height()) {
                layer.hasMask = true;
                layer.maskEnabled = m.maskEnabled;
                layer.maskOffsetX = m.maskOffset.x();
                layer.maskOffsetY = m.maskOffset.y();
                layer.maskScaleX = m.maskScaleX;
                layer.maskScaleY = m.maskScaleY;
                layer.maskDensity = m.maskDensity;
                layer.maskFeather = m.maskFeather;
                layer.maskWidth =
                    static_cast<std::uint32_t>(m.mask.width());
                layer.maskHeight =
                    static_cast<std::uint32_t>(m.mask.height());
                layer.mask = cov;
            }
        }
        layer.isText = m.isText;
        layer.hasTextSpec = m.liveText;
        if (m.liveText) {
            layer.text = toStd(m.text);
            layer.textFamily = toStd(m.textFamily);
            layer.textBold = m.textBold;
            layer.textItalic = m.textItalic;
            layer.textAlign = static_cast<std::uint8_t>(qBound(0, m.textAlign, 2));
            layer.textSize = m.textSize;
            layer.textLineHeight = m.textLineHeight;
            layer.textTracking = m.textTracking;
            layer.textWrapWidth = m.textWrapWidth;
            layer.textFrameHeight = m.textFrameHeight;
            layer.textOriginX = m.textOrigin.x();
            layer.textOriginY = m.textOrigin.y();
            layer.textColor = static_cast<std::uint32_t>(m.textColor.rgba());
            // Character-panel extras: transparent colours mean "inherit/none".
            auto argb = [](const QColor& c) -> std::uint32_t {
                return c.isValid() ? static_cast<std::uint32_t>(c.rgba()) : 0u;
            };
            layer.hasTextExtras = true;
            layer.textUnderline = static_cast<std::uint8_t>(qBound(0, m.textUnderline, 2));
            layer.textStrike = static_cast<std::uint8_t>(qBound(0, m.textStrike, 2));
            layer.textUnderlineColor = argb(m.textUnderlineColor);
            layer.textStrikeColor = argb(m.textStrikeColor);
            layer.textBackgroundColor = argb(m.textBackgroundColor);
            layer.textBaselineShift = m.textBaselineShift;
            layer.textHScale = m.textHScale;
            layer.textVScale = m.textVScale;
            layer.textSuperSub = static_cast<std::int8_t>(qBound(-1, m.textSuperSub, 1));
            layer.textAllCaps = m.textAllCaps;
            layer.textKerning = m.textKerning;
            layer.textOtFeatures = m.textOtFeatures;
        }
        if (m.art && !m.art->segments.empty()) {
            layer.vectorData = pittore::vector::encodeArtNode(*m.art);
            layer.hasVector = !layer.vectorData.empty();
        }
        if (m.hasAdjustment && m.adjustmentKind > 0 &&
            m.adjustmentKind <= 14) {
            layer.hasAdjustment = true;
            layer.adjustmentKind =
                static_cast<std::uint8_t>(m.adjustmentKind);
            for (int k = 0; k < 16; ++k)
                layer.adjustmentParams[k] = m.adjustmentParams[k];
            layer.adjustmentCurve.reserve(
                static_cast<std::size_t>(m.adjustmentCurve.size()));
            for (const QPointF& p : m.adjustmentCurve)
                layer.adjustmentCurve.emplace_back(p.x(), p.y());
            for (const QPointF& p : m.adjustmentCurveR)
                layer.adjustmentCurveR.emplace_back(p.x(), p.y());
            for (const QPointF& p : m.adjustmentCurveG)
                layer.adjustmentCurveG.emplace_back(p.x(), p.y());
            for (const QPointF& p : m.adjustmentCurveB)
                layer.adjustmentCurveB.emplace_back(p.x(), p.y());
        }
        if (m.hasToneBlend) {
            layer.hasToneBlend = true;
            layer.toneBlend[0] = m.toneBlendStrength;
            layer.toneBlend[1] = m.toneBlendColor;
            layer.toneBlend[2] = m.toneBlendContrast;
            layer.toneBlend[3] = m.toneBlendLowPass;
            layer.toneBlend[4] = m.toneBlendContentType;
        }
        if (m.hasLiveFilter) {
            layer.hasLiveFilter = true;
            layer.liveFilterEnabled = m.liveFilterEnabled;
            layer.liveFilterId = m.liveFilterId.toStdString();
            layer.liveFilterParams.reserve(
                static_cast<std::size_t>(m.liveFilterParams.size()));
            for (double v : m.liveFilterParams)
                layer.liveFilterParams.push_back(v);
        }
        // Fresh-only foreign blocks arrive via the Meta (stale ones were
        // dropped upstream); the v4 codec caps sizes again defensively.
        layer.psdBlocks.reserve(m.psdRawBlocks.size());
        for (const auto& rb : m.psdRawBlocks) {
            pittore::io::ProjectLayerFile::PsdBlock o;
            std::memcpy(o.sig, rb.sig, 4);
            std::memcpy(o.key, rb.key, 4);
            o.data = rb.data;
            o.padding = rb.padding;
            layer.psdBlocks.push_back(std::move(o));
        }
        if (!m.pixels.isNull()) {
            layer.width = static_cast<std::uint32_t>(m.pixels.width());
            layer.height = static_cast<std::uint32_t>(m.pixels.height());
            layer.rgba = rgba16FromQImage(m.pixels);
            if (data.colorMode.startsWith(QStringLiteral("CMYK"),
                                          Qt::CaseInsensitive)) {
                const std::size_t n =
                    static_cast<std::size_t>(layer.width) * layer.height;
                if (m.cmyk.size() == n * 4) {
                    layer.cmyk = m.cmyk;
                } else {
                    std::vector<std::uint16_t> rgb(n * 3);
                    for (std::size_t i = 0; i < n; ++i) {
                        rgb[i * 3 + 0] = layer.rgba[i * 4 + 0];
                        rgb[i * 3 + 1] = layer.rgba[i * 4 + 1];
                        rgb[i * 3 + 2] = layer.rgba[i * 4 + 2];
                    }
                    layer.cmyk.resize(n * 4);
                    const pittore::color::SrgbToCmyk sep(
                        reinterpret_cast<const std::uint8_t*>(data.iccProfile.constData()),
                        static_cast<std::size_t>(data.iccProfile.size()));
                    sep.convert16(rgb.data(), layer.cmyk.data(), n);
                }
            }
        }
        doc.layers.push_back(std::move(layer));
    }
    if (!data.preview.isNull()) {
        const QImage flat = previewImage(data.preview);
        doc.previewWidth = static_cast<std::uint32_t>(flat.width());
        doc.previewHeight = static_cast<std::uint32_t>(flat.height());
        doc.preview = rgba16FromQImage(flat);
    }

    std::string codecError;
    auto bytes = pittore::io::projectEncode(doc, &codecError);
    if (!bytes) {
        if (error) *error = QObject::tr("Could not encode project: %1")
                                .arg(fromStd(codecError));
        return false;
    }

    // QSaveFile writes a sibling temp file and atomically renames it over the
    // destination on commit (R98): a crash or full disk mid-write can never
    // leave a torn project file replacing the last good one.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QObject::tr("Could not write %1").arg(path);
        return false;
    }
    const qint64 wanted = static_cast<qint64>(bytes->size());
    if (f.write(reinterpret_cast<const char*>(bytes->data()), wanted) != wanted) {
        if (error) *error = QObject::tr("Could not write %1").arg(path);
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (error) *error = QObject::tr("Could not write %1").arg(path);
        return false;
    }
    return true;
}

bool loadProjectFile(const QString& path, ProjectFileData* out, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QObject::tr("Not a project file (%1)").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    std::string codecError;
    auto doc = pittore::io::projectDecode(
        std::vector<std::uint8_t>(bytes.constBegin(), bytes.constEnd()), &codecError);
    if (!doc) {
        if (error) *error = QObject::tr("Could not read project: %1")
                                .arg(fromStd(codecError));
        return false;
    }

    ProjectFileData data;
    data.name = fromStd(doc->name);
    data.size = QSize(static_cast<int>(doc->width), static_cast<int>(doc->height));
    data.dpi = doc->dpi;
    data.colorMode = fromStd(doc->colorModeName);
    data.iccProfile =
        QByteArray(reinterpret_cast<const char*>(doc->icc.data()),
                   static_cast<qsizetype>(doc->icc.size()));
    data.background = fromStd(doc->background);
    for (const pittore::io::ProjectLayerFile& l : doc->layers) {
        ProjectLayerMeta m;
        m.name = fromStd(l.name);
        m.kind = l.kind;
        m.visible = l.visible;
        m.locked = l.locked;
        m.opacity = l.opacity;
        m.fill = l.fill;
        m.blendMode = fromStd(l.blend);
        m.offset = QPointF(l.offsetX, l.offsetY);
        m.scaleX = l.scaleX;
        m.scaleY = l.scaleY;
        m.lockTransparency = l.lockTransparency;
        m.clipped = l.clipped;
        m.indent = l.indent;
        if (l.hasMask && l.maskWidth > 0 && l.maskHeight > 0 &&
            l.mask.size() == static_cast<std::size_t>(l.maskWidth) *
                                 l.maskHeight) {
            QImage mask = maskCoverageToQImage(
                l.mask, static_cast<int>(l.maskWidth),
                static_cast<int>(l.maskHeight));
            if (!mask.isNull()) {
                m.hasMask = true;
                m.maskEnabled = l.maskEnabled;
                m.maskOffset = QPointF(l.maskOffsetX, l.maskOffsetY);
                m.maskScaleX = l.maskScaleX;
                m.maskScaleY = l.maskScaleY;
                m.maskDensity = static_cast<float>(l.maskDensity);
                m.maskFeather = static_cast<float>(l.maskFeather);
                m.mask = std::move(mask);
            }
        }
        m.isText = l.isText;
        m.liveText = l.hasTextSpec;
        if (l.hasTextSpec) {
            m.text = fromStd(l.text);
            m.textFamily = fromStd(l.textFamily);
            m.textBold = l.textBold;
            m.textItalic = l.textItalic;
            m.textAlign = l.textAlign;
            m.textSize = l.textSize;
            m.textLineHeight = l.textLineHeight;
            m.textTracking = l.textTracking;
            m.textWrapWidth = l.textWrapWidth;
            m.textFrameHeight = l.textFrameHeight;
            m.textOrigin = QPointF(l.textOriginX, l.textOriginY);
            m.textColor = QColor::fromRgba(static_cast<QRgb>(l.textColor));
            if (l.hasTextExtras) {
                m.textUnderline = l.textUnderline;
                m.textStrike = l.textStrike;
                if (l.textUnderlineColor != 0)
                    m.textUnderlineColor =
                        QColor::fromRgba(static_cast<QRgb>(l.textUnderlineColor));
                if (l.textStrikeColor != 0)
                    m.textStrikeColor =
                        QColor::fromRgba(static_cast<QRgb>(l.textStrikeColor));
                if (l.textBackgroundColor != 0)
                    m.textBackgroundColor =
                        QColor::fromRgba(static_cast<QRgb>(l.textBackgroundColor));
                m.textBaselineShift = l.textBaselineShift;
                m.textHScale = l.textHScale;
                m.textVScale = l.textVScale;
                m.textSuperSub = l.textSuperSub;
                m.textAllCaps = l.textAllCaps;
                m.textKerning = l.textKerning;
                m.textOtFeatures = l.textOtFeatures;
            }
        }
        if (l.hasVector && !l.vectorData.empty()) {
            if (auto art = pittore::vector::decodeArtNode(l.vectorData))
                m.art = std::make_shared<pittore::vector::ArtNode>(std::move(*art));
        }
        if (l.hasAdjustment) {
            m.hasAdjustment = true;
            m.adjustmentKind = l.adjustmentKind;
            for (int k = 0; k < 16; ++k)
                m.adjustmentParams[k] = static_cast<float>(l.adjustmentParams[k]);
            m.adjustmentCurve.reserve(
                static_cast<int>(l.adjustmentCurve.size()));
            for (const auto& p : l.adjustmentCurve)
                m.adjustmentCurve.append(QPointF(p.first, p.second));
            for (const auto& p : l.adjustmentCurveR)
                m.adjustmentCurveR.append(QPointF(p.first, p.second));
            for (const auto& p : l.adjustmentCurveG)
                m.adjustmentCurveG.append(QPointF(p.first, p.second));
            for (const auto& p : l.adjustmentCurveB)
                m.adjustmentCurveB.append(QPointF(p.first, p.second));
        }
        if (l.hasToneBlend) {
            m.hasToneBlend = true;
            m.toneBlendStrength = static_cast<float>(l.toneBlend[0]);
            m.toneBlendColor = static_cast<float>(l.toneBlend[1]);
            m.toneBlendContrast = static_cast<float>(l.toneBlend[2]);
            m.toneBlendLowPass = static_cast<float>(l.toneBlend[3]);
            m.toneBlendContentType =
                qBound(0, static_cast<int>(l.toneBlend[4]), 2);
        }
        if (l.hasLiveFilter) {
            m.hasLiveFilter = true;
            m.liveFilterEnabled = l.liveFilterEnabled;
            m.liveFilterId = QString::fromStdString(l.liveFilterId);
            m.liveFilterParams.reserve(
                static_cast<int>(l.liveFilterParams.size()));
            for (double v : l.liveFilterParams)
                m.liveFilterParams.append(v);
        }
        m.psdRawBlocks.reserve(static_cast<int>(l.psdBlocks.size()));
        for (const auto& rb : l.psdBlocks) {
            ProjectLayerMeta::PsdRawBlock o;
            std::memcpy(o.sig, rb.sig, 4);
            std::memcpy(o.key, rb.key, 4);
            o.data = rb.data;
            o.padding = rb.padding;
            m.psdRawBlocks.push_back(std::move(o));
        }
        m.cmyk = l.cmyk;
        if (l.width != 0 && l.height != 0)
            m.pixels = qimageFromRgba16(l.rgba, static_cast<int>(l.width),
                                        static_cast<int>(l.height));
        data.layers.append(m);
    }
    data.preview = qimageFromRgba16(doc->preview, static_cast<int>(doc->previewWidth),
                                    static_cast<int>(doc->previewHeight));

    *out = std::move(data);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// All-format import/export: PSD/XCF use the built-in codecs, KRA the built-in
// ZIP reader, and everything else Qt's image plugins (whatever QImageReader /
// QImageWriter support at runtime). UI calls these through AppState::openImageFile
// and the File menu; the tests exercise the same free functions directly.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

bool writeBytesToFile(const QString& path, const QByteArray& bytes,
                      QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QObject::tr("Could not write %1").arg(path);
        return false;
    }
    const qint64 wanted = bytes.size();
    if (f.write(bytes.constData(), wanted) != wanted || !f.flush()) {
        if (error) *error = QObject::tr("Could not write %1").arg(path);
        return false;
    }
    return true;
}

// KRA archives are plain ZIPs; the flattened composite lives in
// mergedimage.png (preview.png as a fallback). Decoded through the built-in ZIP
// reader + Qt's PNG path so no plugin dependency is required.
QImage decodeKra(const QByteArray& bytes, QString* error) {
    auto entries = pittore::io::zipRead(
        std::vector<std::uint8_t>(bytes.constBegin(), bytes.constEnd()));
    if (!entries) {
        if (error) *error = QObject::tr("Not a valid KRA archive.");
        return {};
    }
    const pittore::io::ZipEntry* e = pittore::io::zipFind(*entries, "mergedimage.png");
    if (!e) e = pittore::io::zipFind(*entries, "preview.png");
    if (!e) {
        if (error) *error = QObject::tr("KRA archive has no flattened image inside.");
        return {};
    }
    const QImage img = QImage::fromData(
        QByteArray(reinterpret_cast<const char*>(e->data.data()),
                   static_cast<int>(e->data.size())),
        "PNG");
    if (img.isNull()) {
        if (error) *error = QObject::tr("Could not decode the KRA image data.");
        return {};
    }
    return img;
}

QString lowerSuffix(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

// Fill an RGBA64 image from RGBA8 rows. Shared by the TIFF banded (full-res)
// and subsampled (proxy) sinks: 8-bit -> 16-bit straight (x257), matching
// tiffDecodeRgba16.
void fillRgba64Row(QImage& img, std::uint32_t y, const std::uint8_t* row8,
                   std::uint32_t w) {
    auto* dst = reinterpret_cast<std::uint16_t*>(img.scanLine(y));
    for (std::uint32_t x = 0; x < w; ++x) {
        dst[x * 4 + 0] = static_cast<std::uint16_t>(row8[x * 4 + 0] * 257u);
        dst[x * 4 + 1] = static_cast<std::uint16_t>(row8[x * 4 + 1] * 257u);
        dst[x * 4 + 2] = static_cast<std::uint16_t>(row8[x * 4 + 2] * 257u);
        dst[x * 4 + 3] = static_cast<std::uint16_t>(row8[x * 4 + 3] * 257u);
    }
}

}  // namespace

int proxyFactorForPixels(std::uint64_t pixels, std::uint64_t maxPixels) {
    if (maxPixels == 0 || pixels <= maxPixels) return 1;
    const double f = std::sqrt(static_cast<double>(pixels) /
                               static_cast<double>(maxPixels));
    return qMax(1, static_cast<int>(std::ceil(f)));
}

bool probeImageSize(const QString& path, QSize& sizeOut, int& dpiOut) {
    sizeOut = QSize();
    dpiOut = 0;
    if (path.isEmpty()) return false;
    const QString suffix = lowerSuffix(path);
#ifdef PITTORE_TIFF
    if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff")) {
        std::uint32_t w = 0, h = 0;
        int dpi = 0;
        if (!pittore::io::tiffProbeFile(path.toLocal8Bit().constData(), w, h,
                                         dpi))
            return false;
        sizeOut = QSize(static_cast<int>(w), static_cast<int>(h));
        dpiOut = dpi;
        return true;
    }
#else
    (void)suffix;
#endif
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize s = reader.size();
    if (!s.isValid() || s.isEmpty()) return false;
    sizeOut = s;
    return true;
}

namespace {

// TIFF decode with an optional pixel cap. nullopt = not a TIFF per libtiff
// (caller falls through to the Qt plugin); engaged = handled, with a null
// image + *error on failure. maxPixels = 0 decodes dense (legacy).
// factorOut/fullSizeOut report the applied subsample (1 = full-res).
std::optional<QImage> decodeTiffImage(const QString& path,
                                      const QFileInfo& fi,
                                      std::uint64_t maxPixels,
                                      QString* error, int* dpiOut,
                                      int* factorOut, QSize* fullSizeOut) {
#ifdef PITTORE_TIFF
    const QByteArray fsPath = path.toLocal8Bit();
    std::uint32_t tw = 0, th = 0;
    int tiffDpi = 0;
    if (!pittore::io::tiffProbeFile(fsPath.constData(), tw, th, tiffDpi)) {
        ::pittore::core::log::log_info(
            "[import] TIFF probe missed %s, trying plugin path",
            fi.fileName().toUtf8().constData());
        return std::nullopt;
    }
    const std::uint64_t npix = static_cast<std::uint64_t>(tw) * th;
    if (npix > (1ull << 30)) {
        ::pittore::core::log::log_warning(
            "[import] TIFF dimensions too large %s (%ux%u)",
            fi.fileName().toUtf8().constData(), tw, th);
        if (error)
            *error = QObject::tr("Could not read %1: image dimensions too large.")
                         .arg(fi.fileName());
        return QImage{};
    }
    int factor = 1;
    if (maxPixels > 0 && npix > maxPixels)
        factor = proxyFactorForPixels(npix, maxPixels);
    if (factorOut) *factorOut = factor;
    if (fullSizeOut) *fullSizeOut = QSize(static_cast<int>(tw),
                                          static_cast<int>(th));
    const std::uint32_t ow = (tw + static_cast<std::uint32_t>(factor) - 1) /
                             static_cast<std::uint32_t>(factor);
    const std::uint32_t oh = (th + static_cast<std::uint32_t>(factor) - 1) /
                             static_cast<std::uint32_t>(factor);
    QImage img(static_cast<int>(ow), static_cast<int>(oh),
               QImage::Format_RGBA64);
    if (img.isNull()) {
        ::pittore::core::log::log_warning(
            "[import] TIFF QImage alloc failed %s (%ux%u)",
            fi.fileName().toUtf8().constData(), ow, oh);
        if (error)
            *error = QObject::tr("Could not read %1: not enough memory.")
                         .arg(fi.fileName());
        return img;
    }
    std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
        [&img, ow](std::uint32_t y, const std::uint8_t* row8) -> bool {
        if (y >= static_cast<std::uint32_t>(img.height())) return false;
        fillRgba64Row(img, y, row8, ow);
        return true;
    };
    bool ok = false;
    try {
        if (factor == 1) {
            ok = pittore::io::tiffDecodeFileBanded(fsPath.constData(), sink);
        } else {
            std::uint32_t dw = 0, dh = 0;
            int ddpi = 0;
            ok = pittore::io::tiffDecodeFileSubsampled(
                fsPath.constData(), factor, dw, dh, ddpi, sink);
            if (ok && (dw != ow || dh != oh)) ok = false;
            if (ok) tiffDpi = ddpi;
        }
    } catch (const std::bad_alloc&) {
        ok = false;
    } catch (...) {
        ok = false;
    }
    if (ok && !img.isNull()) {
        if (dpiOut) *dpiOut = tiffDpi;
        if (factor == 1) {
            ::pittore::core::log::log_info("[import] TIFF decoded %s (%ux%u @%idpi) banded",
                                            fi.fileName().toUtf8().constData(),
                                            tw, th, tiffDpi);
        } else {
            ::pittore::core::log::log_info(
                "[import] TIFF proxy 1/%d %s (%ux%u of %ux%u @%idpi) subsampled",
                factor, fi.fileName().toUtf8().constData(), ow, oh, tw, th,
                tiffDpi);
        }
        return img;
    }
    // Dense banded declined (exotic variant) or failed mid-decode: buffered
    // fallback only when small enough to hold raster+output safely.
    if (factor == 1) {
        constexpr std::uint64_t kBufferedCap = 64ull * 1024 * 1024;
        if (npix <= kBufferedCap) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray bytes = f.readAll();
                std::uint32_t w = 0, h = 0;
                std::vector<std::uint16_t> rgba;
                int dpi2 = 0;
                bool bok = false;
                try {
                    bok = pittore::io::tiffDecodeRgba16(
                        std::vector<std::uint8_t>(bytes.constBegin(),
                                                  bytes.constEnd()),
                        w, h, rgba, dpi2);
                } catch (const std::bad_alloc&) {
                    bok = false;
                } catch (...) {
                    bok = false;
                }
                if (bok && w > 0 && h > 0) {
                    QImage got;
                    try {
                        got = qimageFromRgba16(rgba, static_cast<int>(w),
                                               static_cast<int>(h));
                    } catch (const std::bad_alloc&) {
                        got = QImage();
                    } catch (...) {
                        got = QImage();
                    }
                    if (!got.isNull()) {
                        if (dpiOut) *dpiOut = dpi2;
                        ::pittore::core::log::log_info(
                            "[import] TIFF decoded %s (%ux%u @%idpi) buffered",
                            fi.fileName().toUtf8().constData(), w, h, dpi2);
                        return got;
                    }
                }
            }
            // Small file but undecodable: fall through to the plugin.
            return std::nullopt;
        }
    }
    ::pittore::core::log::log_warning(
        "[import] TIFF decode failed %s (%ux%u factor=%d), trying plugin path",
        fi.fileName().toUtf8().constData(), tw, th, factor);
    // Subsampled failures are final (the plugin would attempt the full 416MP
    // dense read); exotic dense declines still get the plugin attempt below.
    if (factor > 1) {
        if (error)
            *error = QObject::tr("Could not read %1 as a TIFF image.")
                         .arg(fi.fileName());
        return QImage{};
    }
    return std::nullopt;
#else
    (void)path;
    (void)fi;
    (void)maxPixels;
    (void)error;
    (void)dpiOut;
    (void)factorOut;
    (void)fullSizeOut;
    return std::nullopt;
#endif
}

}  // namespace

QImage qimageFromFile(const QString& path, QString* error, int* dpiOut) {
    return qimageFromFileCapped(path, 0, error, dpiOut, nullptr, nullptr);
}

QImage qimageFromFileCapped(const QString& path, std::uint64_t maxPixels,
                            QString* error, int* dpiOut, int* factorOut,
                            QSize* fullSizeOut) {
    if (dpiOut) *dpiOut = 0;
    if (path.isEmpty()) {
        if (error) *error = QObject::tr("No file given.");
        return {};
    }
    const QString suffix = lowerSuffix(path);
    const QFileInfo fi(path);
    ::pittore::core::log::log_info("[import] decode start %s (%lld bytes) suffix=%s",
                                    fi.fileName().toUtf8().constData(), fi.size(),
                                    suffix.toUtf8().constData());
    // Built-in codecs handle the document formats regardless of system plugins.
    if (suffix == QLatin1String("psd") || suffix == QLatin1String("psb") ||
        suffix == QLatin1String("xcf")) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QObject::tr("Could not open %1").arg(path);
            return {};
        }
        const QByteArray bytes = f.readAll();
        const std::vector<std::uint8_t> data(bytes.constBegin(), bytes.constEnd());
        if (suffix == QLatin1String("psd") || suffix == QLatin1String("psb")) {
            std::string codecError;
            // Embedded ICC rides into the decode so CMYK sources convert
            // profiled instead of naive (falls back automatically).
            const std::vector<std::uint8_t> icc =
                pittore::io::psdEmbeddedIcc(data);
            auto img = pittore::io::psdDecode(data, &codecError, &icc);
            if (!img) {
                // PSB / exotic PSDs may defeat the built-in decoder; try the
                // system plugin before giving up.
                QImageReader fallback(path);
                fallback.setAutoTransform(true);
                const QImage got = fallback.read();
                if (!got.isNull()) {
                    ::pittore::core::log::log_info(
                        "[import] flattened PSD via plugin fallback %s (%ux%u) codec='%s'",
                        fi.fileName().toUtf8().constData(), got.width(), got.height(),
                        codecError.c_str());
                    return got;
                }
                ::pittore::core::log::log_warning(
                    "[import] flattened PSD decode failed %s: codec '%s' plugin '%s'",
                    fi.fileName().toUtf8().constData(), codecError.c_str(),
                    fallback.errorString().toUtf8().constData());
                if (error)
                    *error = QObject::tr("Could not read %1 as a PSD image.")
                                 .arg(fi.fileName());
                return {};
            }
            const QImage got = qimageFromRgba16(
                img->rgba, static_cast<int>(img->width), static_cast<int>(img->height));
            if (got.isNull()) {
                ::pittore::core::log::log_warning(
                    "[import] flattened PSD rgba conversion failed %s",
                    fi.fileName().toUtf8().constData());
                if (error) *error = QObject::tr("Could not decode %1.").arg(fi.fileName());
                return {};
            }
            ::pittore::core::log::log_info(
                "[import] flattened PSD decoded %s (%ux%u depth=%d)",
                fi.fileName().toUtf8().constData(), got.width(), got.height(), img->depth);
            return got;
        }
        // XCF: built-in decoder first, plugin fallback.
        std::string codecError;
        auto img = pittore::io::xcfDecode(data, &codecError);
        if (!img) {
            QImageReader fallback(path);
            fallback.setAutoTransform(true);
            const QImage got = fallback.read();
            if (!got.isNull()) {
                ::pittore::core::log::log_info(
                    "[import] flattened XCF via plugin fallback %s (%ux%u) codec='%s'",
                    fi.fileName().toUtf8().constData(), got.width(), got.height(),
                    codecError.c_str());
                return got;
            }
            ::pittore::core::log::log_warning(
                "[import] flattened XCF decode failed %s: codec '%s' plugin '%s'",
                fi.fileName().toUtf8().constData(), codecError.c_str(),
                fallback.errorString().toUtf8().constData());
            if (error)
                *error = QObject::tr("Could not read %1 as an XCF image.")
                             .arg(fi.fileName());
            return {};
        }
        const QImage got = qimageFromRgba16(
            img->rgba, static_cast<int>(img->width), static_cast<int>(img->height));
        if (got.isNull()) {
            ::pittore::core::log::log_warning(
                "[import] flattened XCF rgba conversion failed %s",
                fi.fileName().toUtf8().constData());
            if (error) *error = QObject::tr("Could not decode %1.").arg(fi.fileName());
            return {};
        }
        ::pittore::core::log::log_info(
            "[import] flattened XCF decoded %s (%ux%u depth=%d)",
            fi.fileName().toUtf8().constData(), got.width(), got.height(), img->depth);
        return got;
    }
    if (suffix == QLatin1String("kra")) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QObject::tr("Could not open %1").arg(path);
            return {};
        }
        const QImage kra = decodeKra(f.readAll(), error);
        if (kra.isNull())
            ::pittore::core::log::log_warning(
                "[import] KRA decode failed %s: %s",
                fi.fileName().toUtf8().constData(),
                error && !error->isEmpty() ? error->toUtf8().constData() : "unknown");
        else
            ::pittore::core::log::log_info("[import] KRA decoded %s (%ux%u)",
                                            fi.fileName().toUtf8().constData(),
                                            kra.width(), kra.height());
        return kra;
    }

    // The .af document formats: built-in codec opens the document
    // preview embedded in the archive — no Qt/system plugin understands the
    // container, so there is no fallback past this path.
    if (suffix == QLatin1String("af") || suffix == QLatin1String("afphoto") ||
        suffix == QLatin1String("afdesign") || suffix == QLatin1String("afpub")) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QObject::tr("Could not open %1").arg(path);
            return {};
        }
        const QByteArray bytes = f.readAll();
        const std::vector<std::uint8_t> data(bytes.constBegin(), bytes.constEnd());
        std::string codecError;
        auto img = pittore::io::afDecode(data, &codecError);
        if (!img) {
            ::pittore::core::log::log_warning(
                "[import] Affinity decode failed %s: %s",
                fi.fileName().toUtf8().constData(), codecError.c_str());
            if (error)
                *error = QObject::tr("Could not read %1 as an Affinity document (%2).")
                             .arg(fi.fileName(), QString::fromStdString(codecError));
            return {};
        }
        const QImage got = qimageFromRgba16(
            img->rgba, static_cast<int>(img->width), static_cast<int>(img->height));
        if (got.isNull()) {
            ::pittore::core::log::log_warning(
                "[import] Affinity rgba conversion failed %s",
                fi.fileName().toUtf8().constData());
            if (error) *error = QObject::tr("Could not decode %1.").arg(fi.fileName());
            return {};
        }
        if (dpiOut) *dpiOut = img->dpi;
        // Surface the manifest (title, client version, linked resources) when
        // the codec was able to read it.
        auto meta = pittore::io::afDecodeDocument(data, nullptr);
        if (meta) {
            QStringList resources;
            for (const auto& pl : meta->placements) {
                if (resources.size() >= 4) {
                    resources << QStringLiteral("…");
                    break;
                }
                resources << QString::fromStdString(pl.path);
            }
            ::pittore::core::log::log_info(
                "[import] Affinity decoded %s (%ux%u preview) title='%s' "
                "client='%s' app='%s' frames=%llu placements=%zu [%s]",
                fi.fileName().toUtf8().constData(), got.width(), got.height(),
                meta->title.c_str(), meta->clientVersion.c_str(),
                meta->appVersion.c_str(),
                static_cast<unsigned long long>(meta->frameCount),
                meta->placements.size(), resources.join(QLatin1String(", ")).toUtf8().constData());
        }
        return got;
    }

#ifdef PITTORE_WEBP
    // WebP: the built-in codec fills the gap the Qt plugin set commonly has
    // (this system's kimg plugins list no webp at all).
    if (suffix == QLatin1String("webp")) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QObject::tr("Could not open %1").arg(path);
            return {};
        }
        const QByteArray bytes = f.readAll();
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint16_t> rgba;
        if (pittore::io::webpDecodeRgba16(
                std::vector<std::uint8_t>(bytes.constBegin(), bytes.constEnd()), w, h,
                rgba) &&
            w > 0 && h > 0) {
            const QImage got = qimageFromRgba16(rgba, static_cast<int>(w),
                                                static_cast<int>(h));
            if (!got.isNull()) {
                ::pittore::core::log::log_info("[import] WebP decoded %s (%ux%u)",
                                                fi.fileName().toUtf8().constData(),
                                                w, h);
                return got;
            }
        }
        // Fall through to the plugin attempt below for exotic/sidecar files.
    }
#endif
#ifdef PITTORE_TIFF
    // TIFF: probe dimensions first so a 416MP file never goes through the
    // buffered whole-file path. Capped imports subsample to a proxy.
    if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff")) {
        if (auto tiff = decodeTiffImage(path, fi, maxPixels, error, dpiOut,
                                        factorOut, fullSizeOut))
            return *tiff;
        if (error && !error->isEmpty()) return {};
        // nullopt with no error: fall through to the plugin attempt below.
    }
#endif

    // Raster + vector formats: whatever the Qt image plugins expose.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (maxPixels > 0) {
        // Scale huge reads down to a proxy instead of failing downstream.
        const QSize full = reader.size();
        if (full.isValid() && !full.isEmpty()) {
            const std::uint64_t npix =
                static_cast<std::uint64_t>(full.width()) * full.height();
            if (npix > maxPixels) {
                const int factor = proxyFactorForPixels(npix, maxPixels);
                const QSize scaled(qMax(1, full.width() / factor),
                                   qMax(1, full.height() / factor));
                reader.setScaledSize(scaled);
                if (factorOut) *factorOut = factor;
                if (fullSizeOut) *fullSizeOut = full;
                const QImage got = reader.read();
                if (!got.isNull()) {
                    if (dpiOut) {
                        const int dpm = got.dotsPerMeterX();
                        *dpiOut = dpm > 0 ? qRound(dpm * 0.0254) : 0;
                    }
                    ::pittore::core::log::log_info(
                        "[import] plugin proxy 1/%d %s (%dx%d of %dx%d)",
                        factor, fi.fileName().toUtf8().constData(),
                        got.width(), got.height(), full.width(),
                        full.height());
                    return got;
                }
                // Scaled read failed: fall through to the dense attempt below
                // so the error path stays identical to legacy behaviour.
            }
        }
    }
    const QImage got = reader.read();
    if (got.isNull()) {
        ::pittore::core::log::log_warning("[import] plugin decode failed %s: %s",
                                           fi.fileName().toUtf8().constData(),
                                           reader.errorString().toUtf8().constData());
        if (error)
            *error = QObject::tr("Could not read %1 as an image (%2).")
                         .arg(fi.fileName(), reader.errorString());
        return {};
    }
    if (dpiOut) {
        const int dpm = got.dotsPerMeterX();   // PNG/AVIF/HEIF carry it; JPEG untrusted
        *dpiOut = dpm > 0 ? qRound(dpm * 0.0254) : 0;
    }
    ::pittore::core::log::log_info("[import] plugin decoded %s (%ux%u)",
                                    fi.fileName().toUtf8().constData(),
                                    got.width(), got.height());
    return got;
}

bool saveImageFile(const QImage& flat, const QString& path, QString* error,
                   int jpegQuality, bool progressive) {
    if (flat.isNull()) {
        if (error) *error = QObject::tr("Nothing to export (empty image).");
        return false;
    }
    const QString suffix = lowerSuffix(path);
    ::pittore::core::log::log_info("[export] save start path=%s suffix=%s %ux%u",
                                    path.toUtf8().constData(),
                                    suffix.toUtf8().constData(),
                                    flat.width(), flat.height());
    if (suffix.isEmpty()) {
        if (error) *error = QObject::tr("No file extension — cannot pick a format.");
        return false;
    }
    if (suffix == QLatin1String("psd") || suffix == QLatin1String("psb")) {
        const std::vector<std::uint16_t> rgba = rgba16FromQImage(flat);
        auto bytes = pittore::io::psdEncodeRgba(
            static_cast<std::uint32_t>(flat.width()),
            static_cast<std::uint32_t>(flat.height()), 16, rgba.data());
        if (!bytes) {
            ::pittore::core::log::log_warning("[export] PSD encode failed %s",
                                               QFileInfo(path).fileName().toUtf8().constData());
            if (error) *error = QObject::tr("Could not encode PSD.");
            return false;
        }
        const bool wrote = writeBytesToFile(
            path, QByteArray(reinterpret_cast<const char*>(bytes->data()),
                             static_cast<int>(bytes->size())),
            error);
        ::pittore::core::log::log_info(
            "[export] PSD written %s (%d bytes) %s",
            QFileInfo(path).fileName().toUtf8().constData(),
            static_cast<int>(bytes->size()), wrote ? "OK" : "failed");
        return wrote;
    }
    if (suffix == QLatin1String("xcf")) {
        const std::vector<std::uint16_t> rgba = rgba16FromQImage(flat);
        auto bytes = pittore::io::xcfEncodeRgba(
            static_cast<std::uint32_t>(flat.width()),
            static_cast<std::uint32_t>(flat.height()), rgba.data());
        if (!bytes) {
            ::pittore::core::log::log_warning("[export] XCF encode failed %s",
                                               QFileInfo(path).fileName().toUtf8().constData());
            if (error) *error = QObject::tr("Could not encode XCF.");
            return false;
        }
        const bool wrote = writeBytesToFile(
            path, QByteArray(reinterpret_cast<const char*>(bytes->data()),
                             static_cast<int>(bytes->size())),
            error);
        ::pittore::core::log::log_info(
            "[export] XCF written %s (%d bytes) %s",
            QFileInfo(path).fileName().toUtf8().constData(),
            static_cast<int>(bytes->size()), wrote ? "OK" : "failed");
        return wrote;
    }

#ifdef PITTORE_WEBP
    if (suffix == QLatin1String("webp")) {
        const std::vector<std::uint16_t> rgba = rgba16FromQImage(flat);
        // The dialog's quality slider drives lossy VP8; without one keep the
        // historic lossless VP8L at 90 so a plain call behaves as before.
        const bool lossless = jpegQuality < 0 || jpegQuality >= 100;
        const float quality = jpegQuality >= 0 ? float(jpegQuality) : 90.0f;
        std::vector<std::uint8_t> bytes;
        if (!pittore::io::webpEncodeRgba16(
                static_cast<std::uint32_t>(flat.width()),
                static_cast<std::uint32_t>(flat.height()), lossless, quality,
                rgba.data(), bytes)) {
            ::pittore::core::log::log_warning("[export] WebP encode failed %s",
                                               QFileInfo(path).fileName().toUtf8().constData());
            if (error) *error = QObject::tr("Could not encode WebP.");
            return false;
        }
        const bool wrote = writeBytesToFile(
            path, QByteArray(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size())),
            error);
        ::pittore::core::log::log_info(
            "[export] WebP written %s (%d bytes) %s",
            QFileInfo(path).fileName().toUtf8().constData(),
            static_cast<int>(bytes.size()), wrote ? "OK" : "failed");
        return wrote;
    }
#endif
#ifdef PITTORE_TIFF
    if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff")) {
        const std::vector<std::uint16_t> rgba = rgba16FromQImage(flat);
        const int dpm = flat.dotsPerMeterX();
        const int dpi = dpm > 0 ? qRound(dpm * 0.0254) : 0;
        std::vector<std::uint8_t> bytes;
        if (!pittore::io::tiffEncodeRgba16(
                static_cast<std::uint32_t>(flat.width()),
                static_cast<std::uint32_t>(flat.height()), dpi, rgba.data(), bytes)) {
            ::pittore::core::log::log_warning("[export] TIFF encode failed %s",
                                               QFileInfo(path).fileName().toUtf8().constData());
            if (error) *error = QObject::tr("Could not encode TIFF.");
            return false;
        }
        const bool wrote = writeBytesToFile(
            path, QByteArray(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size())),
            error);
        ::pittore::core::log::log_info(
            "[export] TIFF written %s (%d bytes) %s",
            QFileInfo(path).fileName().toUtf8().constData(),
            static_cast<int>(bytes.size()), wrote ? "OK" : "failed");
        return wrote;
    }
#endif

    const QByteArray format = [&suffix]() -> QByteArray {
        if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
            return "jpeg";
        if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff"))
            return "tiff";
        return suffix.toLatin1();
    }();
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    if (!supported.contains(format)) {
        ::pittore::core::log::log_warning("[export] unsupported format '.%s'",
                                           suffix.toUtf8().constData());
        if (error)
            *error = QObject::tr("Unsupported export format \".%1\".").arg(suffix);
        return false;
    }
    QImageWriter writer(path, format);
    // Lossy writers honour an explicit quality; without one, keep the historic
    // JPEG default of 92 and leave the other formats to their own default so a
    // plain 3-argument call behaves exactly as it always did.
    if (writer.supportsOption(QImageIOHandler::Quality)) {
        const bool lossy = format == "jpeg" || format == "webp" ||
                           format == "avif" || format == "heif";
        if (jpegQuality >= 0)
            writer.setQuality(jpegQuality);
        else if (lossy)
            writer.setQuality(92);
    }
    if (progressive &&
        writer.supportsOption(QImageIOHandler::ProgressiveScanWrite))
        writer.setProgressiveScanWrite(true);
    if (!writer.write(flat)) {
        ::pittore::core::log::log_warning(
            "[export] Qt writer failed %s (format %s): %s",
            QFileInfo(path).fileName().toUtf8().constData(), format.constData(),
            writer.errorString().toUtf8().constData());
        if (error)
            *error = QObject::tr("Could not write %1 (%2).")
                         .arg(QFileInfo(path).fileName(), writer.errorString());
        return false;
    }
    ::pittore::core::log::log_info("[export] %s written %s (%ux%u)",
                                    format.constData(),
                                    QFileInfo(path).fileName().toUtf8().constData(),
                                    flat.width(), flat.height());
    return true;
}

QVector<ProjectEntry> scanProjects() {
    QVector<ProjectEntry> entries;
    const QDir root(projectsRootDir());
    if (!root.exists()) return entries;
    const QFileInfoList files =
        root.entryInfoList(QStringList{QStringLiteral("*.psc"), QStringLiteral("*.ifp")}, QDir::Files, QDir::Time);
    for (const QFileInfo& fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        std::string codecError;
        auto info = pittore::io::projectScan(
            std::vector<std::uint8_t>(bytes.constBegin(), bytes.constEnd()),
            &codecError);
        if (!info) continue;
        ProjectEntry e;
        e.name = fromStd(info->name).isEmpty() ? fi.completeBaseName()
                                               : fromStd(info->name);
        e.path = fi.absoluteFilePath();
        e.modified = fi.lastModified();
        if (info->previewWidth > 0 && info->previewHeight > 0) {
            const QImage thumb = qimageFromRgba16(
                info->preview, static_cast<int>(info->previewWidth),
                static_cast<int>(info->previewHeight));
            if (!thumb.isNull())
                e.thumb = thumb.scaled(240, 150, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
        }
        entries.append(e);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// ProjectManagerDialog
// ---------------------------------------------------------------------------

ProjectManagerDialog::ProjectManagerDialog(AppState* state, QWidget* parent)
    : QDialog(parent), state_(state) {
    setWindowTitle(tr("Project Manager — Pittore Studio"));
    resize(880, 560);
    setAcceptDrops(true);   // a file dragged here opens it, dialog included

    auto* rootLayout = new QVBoxLayout(this);
    stack_ = new QStackedLayout;
    stack_->addWidget(buildStartPage());      // page 0
    stack_->addWidget(buildNewProjectPage()); // page 1
    rootLayout->addLayout(stack_);

    refresh();
}

void ProjectManagerDialog::refresh() {
    recents_->clear();
    const QVector<ProjectEntry> projects = scanProjects();
    if (projects.isEmpty()) {
        auto* item = new QListWidgetItem(tr("No projects yet — create your first one below."),
                                         recents_);
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
    for (const ProjectEntry& e : projects) {
        auto* item =
            new QListWidgetItem(QIcon(pixmapForWidget(e.thumb, recents_)), e.name,
            recents_);
        item->setToolTip(e.path);
        item->setData(Qt::UserRole, e.path);
        item->setData(Qt::UserRole + 1, e.modified.toString(Qt::ISODate));
    }
    if (!recents_->currentItem() && recents_->count() > 0) recents_->setCurrentRow(0);
}

QWidget* ProjectManagerDialog::buildStartPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Recent Projects"), page));
    recents_ = new QListWidget(page);
    recents_->setViewMode(QListView::IconMode);
    recents_->setIconSize(QSize(240, 150));
    recents_->setGridSize(QSize(268, 230));
    recents_->setWordWrap(true);
    recents_->setResizeMode(QListView::Adjust);
    recents_->setMovement(QListView::Static);
    recents_->setUniformItemSizes(false);
    recents_->setSpacing(12);
    connect(recents_, &QListWidget::itemDoubleClicked, this, &ProjectManagerDialog::openItem);
    connect(recents_, &QListWidget::itemActivated, this, &ProjectManagerDialog::openItem);
    layout->addWidget(recents_, 1);

    auto* row = new QHBoxLayout;
    auto* newBtn = new QPushButton(tr("New Project…"), page);
    newBtn->setDefault(true);
    connect(newBtn, &QPushButton::clicked, this, &ProjectManagerDialog::newClicked);
    auto* openBtn = new QPushButton(tr("Open…"), page);
    connect(openBtn, &QPushButton::clicked, this, &ProjectManagerDialog::openClicked);
    auto* cancel = new QPushButton(tr("Cancel"), page);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    row->addWidget(newBtn);
    row->addWidget(openBtn);
    row->addStretch(1);
    row->addWidget(cancel);
    layout->addLayout(row);
    return page;
}

QWidget* ProjectManagerDialog::buildNewProjectPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    name_ = new QLineEdit(tr("Untitled Project"), page);
    name_->setClearButtonEnabled(true);
    connect(name_, &QLineEdit::textEdited, this, &ProjectManagerDialog::nameEdited);
    pathLabel_ = new QLabel(page);
    nameEdited();

    preset_ = new QComboBox(page);
    preset_->addItems({tr("1920 × 1080 (Full HD)"), tr("2560 × 1440 (QHD 1440p)"),
                       tr("1280 × 720 (HD)"), tr("3840 × 2160 (4K UHD)"),
                       tr("800 × 600"), tr("1080 × 1080 (Square)"), tr("Custom")});
    connect(preset_, &QComboBox::currentIndexChanged, this, &ProjectManagerDialog::presetChanged);

    width_ = new QSpinBox(page);
    width_->setRange(1, 32767);
    width_->setValue(qBound(1, state_->settings().newDocWidth, 32767));
    width_->setSuffix(tr(" px"));
    height_ = new QSpinBox(page);
    height_->setRange(1, 32767);
    height_->setValue(qBound(1, state_->settings().newDocHeight, 32767));
    height_->setSuffix(tr(" px"));

    dpi_ = new QSpinBox(page);
    dpi_->setRange(1, 10000);
    dpi_->setValue(qBound(1, state_->settings().newDocDpi, 10000));
    dpi_->setSuffix(tr(" ppi"));

    mode_ = new QComboBox(page);
    mode_->addItems({QStringLiteral("RGB/8"), QStringLiteral("RGB/16"), QStringLiteral("RGB/32"),
                     QStringLiteral("Grayscale/8"), QStringLiteral("CMYK/8"),
                     QStringLiteral("CMYK/16")});
    {
        const int mi = mode_->findData(state_->settings().newDocColorMode);
        mode_->setCurrentIndex(mi >= 0 ? mi : 0);
    }

    background_ = new QComboBox(page);
    background_->addItems({tr("White"), tr("Transparent"), tr("Black")});
    {
        // Combo labels are translated; map the stored id onto a row.
        const QString bg = state_->settings().newDocBackground;
        const int bi = bg == QStringLiteral("transparent") ? 1
                     : bg == QStringLiteral("black")       ? 2
                                                           : 0;
        background_->setCurrentIndex(bi);
    }

    auto row = [page](const char* text, QWidget* w) {
        auto* r = new QHBoxLayout;
        r->addWidget(new QLabel(QObject::tr(text), page));
        r->addWidget(w, 1);
        return r;
    };
    layout->addLayout(row("Name", name_));
    layout->addLayout(row("Path", pathLabel_));
    layout->addLayout(row("Preset", preset_));
    auto* dims = new QHBoxLayout;
    dims->addWidget(new QLabel(tr("Size"), page));
    dims->addWidget(width_);
    dims->addWidget(new QLabel(tr("×"), page));
    dims->addWidget(height_);
    dims->addStretch(1);
    layout->addLayout(dims);
    layout->addLayout(row("Resolution", dpi_));
    layout->addLayout(row("Color Mode", mode_));
    layout->addLayout(row("Background", background_));
    layout->addStretch(1);

    auto* buttons = new QHBoxLayout;
    auto* back = new QPushButton(tr("Back"), page);
    connect(back, &QPushButton::clicked, this, &ProjectManagerDialog::switchToStart);
    auto* create = new QPushButton(tr("Create Project"), page);
    create->setDefault(true);
    connect(create, &QPushButton::clicked, this, &ProjectManagerDialog::createClicked);
    buttons->addWidget(back);
    buttons->addStretch(1);
    buttons->addWidget(create);
    layout->addLayout(buttons);
    return page;
}

void ProjectManagerDialog::switchToStart() { stack_->setCurrentIndex(0); }
void ProjectManagerDialog::switchToNew() {
    name_->setFocus();
    stack_->setCurrentIndex(1);
}
void ProjectManagerDialog::newClicked() { switchToNew(); }

void ProjectManagerDialog::openClicked() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Project"), projectsRootDir(), imageOpenFilter());
    if (path.isEmpty()) return;
    openProjectPath(path);
}

void ProjectManagerDialog::openItem(QListWidgetItem* item) {
    const QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty()) openProjectPath(path);
}

void ProjectManagerDialog::openProjectPath(const QString& path) {
    QString error;
    // Deferred like File > Open (see MainWindow::openProjectFile): any
    // mismatch question waits until the start page has closed and the image
    // is visible; MainWindow::showStartPage drains it.
    state_->setDeferMismatchDialogs(true);
    const bool ok =
        isNativeProjectSuffix(QFileInfo(path).suffix())
            ? state_->openProject(path, &error)
            : state_->openImageFile(path, &error);
    state_->setDeferMismatchDialogs(false);
    if (!ok) {
        // Cancel from the profile-mismatch dialog leaves *error empty: stay
        // on the start page quietly instead of warning.
        if (error.isEmpty()) return;
        QMessageBox::warning(this, tr("Open Project"),
                             tr("Could not open project:\n%1").arg(error));
        return;
    }
    accept();
}

void ProjectManagerDialog::dragEnterEvent(QDragEnterEvent* event) {
    if (dropHasOpenableFiles(event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void ProjectManagerDialog::dropEvent(QDropEvent* event) {
    if (!dropHasOpenableFiles(event->mimeData())) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString localPath = url.toLocalFile();
        if (suffixIsOpenable(QFileInfo(localPath).suffix())) {
            // Opening any supported file dismisses the dialog, so only the
            // first one is consumed.
            openProjectPath(localPath);
            return;
        }
    }
}

void ProjectManagerDialog::nameEdited() {
    QString name = name_->text().trimmed();
    if (name.isEmpty()) name = tr("Untitled Project");
    pathLabel_->setText(projectPathForName(name));
    pathLabel_->setToolTip(pathLabel_->text());
}

void ProjectManagerDialog::presetChanged(int index) {
    const QSize sizes[] = {{1920, 1080}, {2560, 1440}, {1280, 720}, {3840, 2160},
                           {800, 600}, {1080, 1080}};
    if (index >= 0 && index < static_cast<int>(std::size(sizes))) {
        width_->setValue(sizes[index].width());
        height_->setValue(sizes[index].height());
    }
}

void ProjectManagerDialog::createClicked() {
    const QString name = name_->text().trimmed();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(this, tr("New Project"),
                             tr("Please enter a project name without / or \\."));
        return;
    }
    const QString path = projectPathForName(name);
    if (QFileInfo::exists(path)) {
        ProjectFileData existing;
        if (loadProjectFile(path, &existing)) {
            QMessageBox::warning(
                this, tr("New Project"),
                tr("A project named “%1” already exists. Open it from the start page instead.")
                    .arg(name));
            return;
        }
    }

    ProjectFileData data;
    data.name = name;
    data.size = QSize(width_->value(), height_->value());
    data.dpi = dpi_->value();
    data.colorMode = mode_->currentText();
    data.background =
        background_->currentIndex() == 0 ? QStringLiteral("white")
        : background_->currentIndex() == 2 ? QStringLiteral("black")
                                           : QStringLiteral("transparent");
    QString error;
    if (!state_->createNewProject(data, &error)) {
        QMessageBox::warning(this, tr("New Project"), error);
        return;
    }
    accept();
}

}  // namespace pittore::ui
