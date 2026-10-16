#pragma once
// Export-dialog helpers shared by the shell/settings/writers translation units.
// They live in export_detail because the bare names (notably dimStyle)
// already exist in pittore::ui.
#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <string>

#include "engine/core/image.h"
#include "ui/app_state.h"
#include "ui/export_dialog.h"

namespace pittore::ui {
class AppState;
namespace export_detail {

bool formatHasQuality(const QString& fmt);
bool formatSupportsProgressive(const QString& fmt);
bool formatWritesDpi(const QString& fmt);
bool formatWritesIcc(const QString& fmt);
QStringList availableExportFormats();
QColorSpace colorSpaceFor(const QString& name);
QPixmap checkerboard(QSize size, qreal dpr = 1.0);
QString dimStyle(AppState* state);
QString presetsFilePath();
QImage lanczosResample(const QImage& src, QSize dst);
QString blendCssFor(const QString& name);
QImage engineImageToQImage(const pittore::Image& img);
std::string pngBase64Of(const QImage& img);
std::string jpegBase64Of(const QImage& img, int quality);

// One conventional export preset: a named ExportSettings bundle for a file
// suffix. builtInExportPresets(format) lists them in dialog order.
struct ExportPreset {
    QString id;        // stable id, stored as ExportSettings::presetId
    QString format;    // encoder suffix ("png", "jpg", "gif", "tiff", "svg")
    QString label;     // dialog text, e.g. "PNG-8 (Dithered)"
    ExportSettings settings;
};
QVector<ExportPreset> builtInExportPresets(const QString& format);
const ExportPreset* findExportPreset(const QString& id);

// Palettized export: median-cut quantize to <= maxColors entries (+ a
// transparent entry when the source has transparent pixels), optionally with
// Floyd-Steinberg diffusion. grayscale forces a gray ramp instead. Returns an
// indexed image with its color table, or null on failure.
QImage quantizeIndexed(const QImage& src, int maxColors, bool dither,
                       bool grayscale);
// Flatten src over an opaque matte color (used for formats without alpha and
// the RGB/Gray pixel formats).
QImage flattenOverMatte(const QImage& src, const QColor& matte);
// Convert to an export pixel format ("rgb8", "rgba8", "gray8", "rgb16",
// "gray16"); "doc" passes through. matte fills transparent pixels when one is
// given (null = keep transparency for alpha formats).
QImage convertPixelFormat(const QImage& src, const QString& pixelFormat,
                          const QColor& matte);
// HDR stills: linear-sRGB QImage (0..1 floats in RGBA64 channels) re-encoded
// through the PQ (ST 2084) or HLG OETF into 16-bit Rec.2020 codes, full or
// narrow range. makeHdrPng() writes the whole PNG (pHYs + cICP tagged) since
// Qt's writer cannot tag transfer characteristics.
QImage encodeHdrCodes(const QImage& src, int transferFunction, bool fullRange,
                       bool bt2020);
QByteArray encodePngWithCIcp(const QImage& img, int transferFunction,
                             bool fullRange, bool bt2020);
QByteArray optimizePngLossless(const QByteArray& png);
// Insert a cICP chunk (primaries BT.2020/sRGB, matrix=identity RGB,
// transfer PQ/HLG, full/narrow) into PNG bytes before the first IDAT.
QByteArray insertCIcpChunk(const QByteArray& png, int transferFunction,
                           bool fullRange, bool bt2020);

// Scoped export helper for the "Don't export hidden layers" toggle:
// when enabled (off state), reveals every hidden layer and rebuilds the
// composite; the destructor restores visibility and rebuilds again. No-op
// when doc is null or nothing was hidden.
struct AllLayersVisibleGuard {
    explicit AllLayersVisibleGuard(DocumentItem* d) : doc(d) {
        if (!doc) return;
        for (int i = 0; i < doc->layers.size(); ++i) {
            if (!doc->layers[i].visible) {
                hidden.push_back(i);
                doc->layers[i].visible = true;
            }
        }
        if (!hidden.isEmpty()) doc->rebuildComposite();
    }
    ~AllLayersVisibleGuard() {
        if (!doc || hidden.isEmpty()) return;
        for (int i : hidden) {
            if (i >= 0 && i < doc->layers.size())
                doc->layers[i].visible = false;
        }
        doc->rebuildComposite();
    }
    DocumentItem* doc = nullptr;
    QVector<int> hidden;
};

}  // namespace export_detail
}  // namespace pittore::ui
