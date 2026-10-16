#include "ui/export_dialog.h"

#include <QCheckBox>
#include <QColor>
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
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTransform>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ui/app_state.h"
#include "ui/project_manager.h"
#include "ui/settings.h"
#include "ui/theme.h"
#include "ui/export/shared/export_helpers.h"

namespace pittore::ui {

using namespace export_detail;


void ExportDialog::seedWidgets() {
    loading_ = true;
    const int fi = formatCombo_->findData(settings_.format);
    if (fi >= 0) formatCombo_->setCurrentIndex(fi);
    showSettings(settings_);
    exportLocationCombo_->setCurrentIndex(qBound(0, settings_.exportLocation, 2));
    for (QPushButton* chip : scaleChips_)
        chip->setChecked(qFuzzyCompare(chip->property("percent").toDouble(), 100.0));
    rebuildPresetList();
    loading_ = false;
}


void ExportDialog::showSettings(const ExportSettings& s) {
    loading_ = true;
    widthSpin_->setValue(settings_.pixelSize.width() > 0 ? settings_.pixelSize.width()
                                                         : doc_->size.width());
    heightSpin_->setValue(settings_.pixelSize.height() > 0 ? settings_.pixelSize.height()
                                                           : doc_->size.height());
    dpiSpin_->setValue(s.dpi);
    const int ci = colorSpaceCombo_->findText(s.colorProfile);
    colorSpaceCombo_->setCurrentIndex(ci >= 0 ? ci : 0);
    embedIccCheck_->setChecked(s.embedIccProfile);
    qualitySlider_->setValue(s.quality);
    resampleCombo_->setCurrentIndex(qBound(0, s.resampleMode, 3));
    lockAspectCheck_->setChecked(s.lockAspect);
    ditherCheck_->setChecked(s.dither);
    interlaceCheck_->setChecked(s.interlace);
    hideHiddenCheck_->setChecked(s.hideHiddenLayers);
    const int pfi = pixelFormatCombo_->findData(s.pixelFormat);
    pixelFormatCombo_->setCurrentIndex(pfi >= 0 ? pfi : 0);
    matteColor_ = s.matte.isValid() ? s.matte : QColor(255, 255, 255);
    matteTransparent_ = !s.matteEnabled;
    syncMatteButton();
    embedMetadataCheck_->setChecked(s.embedMetadata);
    includeBleedCheck_->setChecked(false);
    transferCombo_->setCurrentIndex(qBound(0, s.transferFunction, 2));
    primariesCombo_->setCurrentIndex(qBound(0, s.primaries, 1));
    fullRangeCheck_->setChecked(s.fullRange);
    palettizedCheck_->setChecked(s.palettized);
    const int pci = paletteColorsCombo_->findData(s.paletteColors);
    paletteColorsCombo_->setCurrentIndex(
        pci >= 0 ? pci : paletteColorsCombo_->findData(256));
    paletteDitherCheck_->setChecked(s.paletteDither);
    limitSizeCheck_->setChecked(s.limitFileSize);
    maxSizeSpin_->setValue(qBound(0.1, s.maxFileSizeMB, 100.0));
    maxSizeSpin_->setEnabled(s.limitFileSize);
    tiffCompressionCombo_->setCurrentIndex(qBound(0, s.tiffCompression, 2));
    svgRasterizeCombo_->setCurrentIndex(qBound(0, s.svgRasterize, 1));
    svgUseDocResCheck_->setChecked(s.svgUseDocRes);
    svgRasterDpiSpin_->setValue(qBound(72, s.svgRasterDpi, 1200));
    svgDownsampleCheck_->setChecked(s.svgDownsample);
    const int adi = svgAboveDpiCombo_->findData(s.svgAboveDpi);
    svgAboveDpiCombo_->setCurrentIndex(adi >= 0 ? adi : 3);
    svgAllowJpegCheck_->setChecked(s.svgAllowJpeg);
    const int sqi = svgQualityCombo_->findData(s.svgJpegQuality);
    svgQualityCombo_->setCurrentIndex(sqi >= 0 ? sqi : 4);
    const int sdi = svgDecimalCombo_->findData(s.svgDecimalPlaces);
    svgDecimalCombo_->setCurrentIndex(sdi >= 0 ? sdi : 3);
    svgViewboxCheck_->setChecked(s.svgSetViewbox);
    svgBreaksCheck_->setChecked(s.svgLineBreaks);
    loading_ = false;
    refreshFormatDependentFields();
    refreshPreview();
    refreshEstimatedSize();
}


void ExportDialog::syncMatteButton() {
    if (!matteButton_) return;
    if (matteTransparent_) {
        matteButton_->setText(tr("Transparent"));
    } else {
        matteButton_->setText(matteColor_.name().toUpper());
        const QString fg =
            (matteColor_.red() * 299 + matteColor_.green() * 587 +
             matteColor_.blue() * 114) /
                    1000 >
                    128
                ? QStringLiteral("black")
                : QStringLiteral("white");
        matteButton_->setStyleSheet(
            QStringLiteral("background-color: %1; color: %2;")
                .arg(matteColor_.name(), fg));
    }
    if (matteTransparent_) matteButton_->setStyleSheet(QString());
}


void ExportDialog::markCustom() {
    if (loading_) return;
    if (presetCombo_ && presetCombo_->currentIndex() != 0) {
        const QSignalBlocker block(presetCombo_);
        presetCombo_->setCurrentIndex(0);
    }
}


void ExportDialog::rebuildPresetList() {
    if (!presetCombo_ || !formatCombo_) return;
    const QSignalBlocker block(presetCombo_);
    presetCombo_->clear();
    presetCombo_->addItem(tr("Custom"), QStringLiteral("custom"));
    const QString fmt = formatCombo_->currentData().toString();
    for (const ExportPreset& p : builtInExportPresets(fmt))
        presetCombo_->addItem(p.label, QStringLiteral("builtin:") + p.id);
    for (int i = 0; i < presets_.size(); ++i) {
        // User presets are never filtered by format: selecting one switches
        // the format combo along with every other widget.
        presetCombo_->addItem(presets_[i].name,
                              QStringLiteral("user:") + QString::number(i));
    }
}


void ExportDialog::applyPresetId(const QString& id) {
    const ExportPreset* p = findExportPreset(id);
    if (!p) return;
    ExportSettings s = p->settings;
    // Keep the dialog's current geometry: presets carry output defaults, not
    // the live canvas size.
    s.pixelSize = QSize(widthSpin_->value(), heightSpin_->value());
    s.dpi = dpiSpin_->value();
    s.lockAspect = lockAspectCheck_->isChecked();
    s.exportTarget = currentTarget();
    s.exportLocation = exportLocationCombo_->currentIndex();
    showSettings(s);
}


ExportSettings ExportDialog::collect() const {
    ExportSettings s = settings_;
    s.format = formatCombo_->currentData().toString();
    // Remember the selected built-in preset so a re-open restores it.
    const QString data = presetCombo_ ? presetCombo_->currentData().toString()
                                      : QString();
    s.presetId = data.startsWith(QLatin1String("builtin:"))
                     ? data.mid(QStringLiteral("builtin:").size())
                     : QString();
    s.quality = qualitySlider_->value();
    s.dither = ditherCheck_->isChecked() || palettizedCheck_->isChecked();
    s.palettized = palettizedCheck_->isChecked();
    s.paletteColors = paletteColorsCombo_->currentData().toInt();
    if (s.paletteColors <= 0) s.paletteColors = 256;
    s.paletteDither = paletteDitherCheck_->isChecked();
    s.limitFileSize = limitSizeCheck_->isChecked();
    s.maxFileSizeMB = maxSizeSpin_->value();
    s.interlace = interlaceCheck_->isChecked();
    s.progressive = s.interlace;
    s.pixelSize = QSize(widthSpin_->value(), heightSpin_->value());
    s.dpi = dpiSpin_->value();
    s.lockAspect = lockAspectCheck_->isChecked();
    s.resampleMode = qBound(0, resampleCombo_->currentIndex(), 3);
    s.colorProfile = colorSpaceCombo_->currentText();
    s.embedIccProfile = embedIccCheck_->isChecked();
    s.hideHiddenLayers = hideHiddenCheck_->isChecked();
    s.pixelFormat = pixelFormatCombo_->currentData().toString();
    s.matteEnabled = !matteTransparent_;
    s.matte = matteColor_;
    s.embedMetadata = embedMetadataCheck_->isChecked();
    s.includeBleed = false;
    s.transferFunction = transferCombo_->currentIndex();
    s.primaries = primariesCombo_->currentIndex();
    s.fullRange = fullRangeCheck_->isChecked();
    s.tiffCompression = tiffCompressionCombo_->currentIndex();
    s.svgRasterize = svgRasterizeCombo_->currentIndex();
    s.svgUseDocRes = svgUseDocResCheck_->isChecked();
    s.svgRasterDpi = svgRasterDpiSpin_->value();
    s.svgDownsample = svgDownsampleCheck_->isChecked();
    s.svgAboveDpi = svgAboveDpiCombo_->currentData().toInt();
    if (s.svgAboveDpi <= 0) s.svgAboveDpi = 375;
    s.svgAllowJpeg = svgAllowJpegCheck_->isChecked();
    s.svgJpegQuality = svgQualityCombo_->currentData().toInt();
    if (s.svgJpegQuality <= 0) s.svgJpegQuality = 85;
    s.svgDecimalPlaces = svgDecimalCombo_->currentData().toInt();
    s.svgSetViewbox = svgViewboxCheck_->isChecked();
    s.svgLineBreaks = svgBreaksCheck_->isChecked();
    s.keepMetadata = keepMetadataCheck_->isChecked();
    s.stripGpsLocation = stripGpsCheck_->isChecked();
    s.exportLocation = exportLocationCombo_->currentIndex();
    s.exportTarget = currentTarget();
    for (QPushButton* chip : scaleChips_) {
        if (!chip->isChecked()) continue;
        s.scalePercent = chip->property("percent").toDouble();
        s.scaleSuffix = chip->property("suffix").toString();
    }
    return s;
}


int ExportDialog::currentTarget() const {
    const int row = targetList_ ? targetList_->currentRow() : 0;
    return targetValues_.value(row, -1);
}


QSize ExportDialog::targetNaturalSize() const {
    const int target = currentTarget();
    if (target >= 0 && target < doc_->layers.size()) {
        const QRect bounds = layerBounds(*doc_, doc_->layers[target]).toAlignedRect();
        if (!bounds.isEmpty()) return bounds.size();
    } else if (target == -2 && !doc_->selection.isEmpty()) {
        const QRect bounds = doc_->selection.toAlignedRect();
        if (!bounds.isEmpty()) return bounds.size();
    }
    return doc_->size;   // whole canvas (and any empty target) falls back here
}


void ExportDialog::applyTargetSize() {
    const QSize natural = targetNaturalSize();
    aspect_ = natural.height() > 0 ? double(natural.width()) / natural.height() : 1.0;
    loading_ = true;
    widthSpin_->setValue(qMax(1, natural.width()));
    heightSpin_->setValue(qMax(1, natural.height()));
    loading_ = false;
    for (QPushButton* chip : scaleChips_)
        chip->setChecked(qFuzzyCompare(chip->property("percent").toDouble(), 100.0));
    refreshEstimatedSize();
}


void ExportDialog::refreshPreview() {
    if (!preview_) return;
    const QSize box(320, 260);
    // Bake at the label's own ratio: a 1x board shown on a 150/200%
    // display upscales soft. Logical painting below is untouched.
    const qreal dpr = preview_->devicePixelRatioF();
    const qreal useDpr = dpr > 0.0 ? dpr : 1.0;
    QPixmap board = checkerboard(box, useDpr);

    const int target = currentTarget();
    QImage content;
    if (target >= 0 && target < doc_->layers.size()) {
        content = layerThumbnail(*doc_, doc_->layers[target], 640, true);
    } else if (target == -2 && !doc_->selection.isEmpty()) {
        const QRect r = doc_->selection.toAlignedRect()
                            .intersected(QRect(QPoint(0, 0), doc_->size));
        content = r.isEmpty() ? doc_->composite : doc_->composite.copy(r);
    } else {
        content = doc_->composite;
    }

    if (!content.isNull()) {
        QPainter p(&board);
        const QPixmap scaled = QPixmap::fromImage(
            content.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        p.drawPixmap((box.width() - scaled.width()) / 2,
                     (box.height() - scaled.height()) / 2, scaled);
    }
    preview_->setPixmap(board);
}


void ExportDialog::refreshFormatDependentFields() {
    const QString fmt = formatCombo_->currentData().toString();
    const bool isVector = fmt == QLatin1String("svg");
    const bool isPng = fmt == QLatin1String("png");
    const bool isJpg =
        fmt == QLatin1String("jpg") || fmt == QLatin1String("jpeg");
    const bool isGif = fmt == QLatin1String("gif");
    const bool isTiff =
        fmt == QLatin1String("tiff") || fmt == QLatin1String("tif");
    const bool isRaster = !isVector;
    if (qualityRow_) fileForm_->setRowVisible(qualityRow_, formatHasQuality(fmt));
    if (resampleCombo_) {
        // SVG output is resolution-independent, so there is no resampling step
        // for the vector content; the width/height only set the viewport.
        resampleCombo_->setEnabled(!isVector);
        resampleCombo_->setToolTip(
            isVector ? tr("SVG stays sharp at any size; width and height set the "
                          "viewport instead.")
                     : QString());
    }
    if (colorSpaceCombo_) {
        // The vector writer emits the authored colours and embeds no profile,
        // so a colour-space conversion would be a silent no-op.
        colorSpaceCombo_->setEnabled(!isVector);
        colorSpaceCombo_->setToolTip(
            isVector ? tr("SVG keeps the colours as authored and embeds no ICC "
                          "profile.")
                     : QString());
    }
    if (interlaceCheck_) {
        const bool prog = formatSupportsProgressive(fmt);
        interlaceCheck_->setEnabled(prog);
        interlaceCheck_->setToolTip(
            prog ? tr("Write a progressive / interlaced scan.")
                 : tr("This format has no progressive / interlaced mode."));
        if (!prog) interlaceCheck_->setChecked(false);
    }
    if (dpiSpin_) {
        const bool writes = formatWritesDpi(fmt);
        dpiSpin_->setEnabled(writes);
        dpiSpin_->setToolTip(writes
                                 ? tr("Resolution stored in the exported file.")
                                 : tr("This format's encoder does not record a "
                                      "resolution."));
    }
    if (embedIccCheck_) {
        const bool canConvert = colorSpaceCombo_->currentIndex() != 3;
        const bool writes = formatWritesIcc(fmt);
        embedIccCheck_->setEnabled(canConvert && writes);
        if (!canConvert) embedIccCheck_->setChecked(false);
        embedIccCheck_->setToolTip(
            !canConvert
                ? tr("\"Don't Convert\" leaves the pixels untouched.")
                : writes ? tr("Attach the converted profile to the file.")
                         : tr("This format's encoder does not store an ICC "
                              "profile; the pixels are still converted."));
    }
    // -- Advanced groups ----------------------------------------------------
    // Shared raster rows.
    if (advGenericRow_) advGenericRow_->setVisible(isRaster);
    if (pixelFormatRow_) pixelFormatRow_->setVisible(isPng || isTiff || isGif);
    if (matteRow_) matteRow_->setVisible(isRaster);
    // PNG rows: HDR controls for truecolor, palette for indexed.
    const bool hdr = isPng && transferCombo_->currentIndex() != 0;
    if (hdrRow_) hdrRow_->setVisible(isPng);
    if (transferCombo_) transferCombo_->setEnabled(isPng);
    if (primariesCombo_) primariesCombo_->setEnabled(isPng);
    if (fullRangeCheck_) fullRangeCheck_->setEnabled(isPng && hdr);
    if (paletteRow_) paletteRow_->setVisible(isPng || isGif);
    if (palettizedCheck_) palettizedCheck_->setEnabled(isPng || isGif);
    if (paletteColorsCombo_) paletteColorsCombo_->setEnabled(isPng || isGif);
    if (paletteDitherCheck_) paletteDitherCheck_->setEnabled(isPng || isGif);
    if (isGif && palettizedCheck_ && !palettizedCheck_->isChecked()) {
        // GIF is always indexed: force the toggle with the format.
        const QSignalBlocker block(palettizedCheck_);
        palettizedCheck_->setChecked(true);
    }
    // JPEG rows.
    if (jpegRow_) jpegRow_->setVisible(isJpg);
    // TIFF rows.
    if (tiffRow_) tiffRow_->setVisible(isTiff);
    // SVG rows.
    if (svgRow_) svgRow_->setVisible(isVector);
    if (svgRasterDpiSpin_)
        svgRasterDpiSpin_->setEnabled(isVector && svgUseDocResCheck_ &&
                                      !svgUseDocResCheck_->isChecked());
    if (svgAboveDpiCombo_)
        svgAboveDpiCombo_->setEnabled(isVector && svgDownsampleCheck_ &&
                                      svgDownsampleCheck_->isChecked());
    if (svgQualityCombo_)
        svgQualityCombo_->setEnabled(isVector && svgAllowJpegCheck_ &&
                                     svgAllowJpegCheck_->isChecked());
    refreshEstimatedSize();
}


void ExportDialog::refreshEstimatedSize() {
    if (!widthSpin_ || !heightSpin_ || !formatCombo_ || !estimateLabel_) return;
    const qint64 pixels =
        qint64(widthSpin_->value()) * qint64(heightSpin_->value());
    const QString fmt = formatCombo_->currentData().toString();
    double bytesPerPixel = 4.0;   // PNG-ish baseline
    if (fmt == QLatin1String("svg")) {
        bytesPerPixel = 2.8;
    } else if (transferCombo_ && transferCombo_->currentIndex() != 0 &&
               fmt == QLatin1String("png")) {
        bytesPerPixel = 8.0;   // 16-bit HDR codes
    } else if (palettizedCheck_ && palettizedCheck_->isChecked() &&
               (fmt == QLatin1String("png") ||
                fmt == QLatin1String("gif"))) {
        bytesPerPixel = 1.1;
    } else if (fmt == QLatin1String("gif")) {
        bytesPerPixel = 1.1;
    } else if (formatHasQuality(fmt)) {
        bytesPerPixel = 0.05 + (qualitySlider_->value() / 100.0) * 1.2;
    } else if (fmt == QLatin1String("tiff") || fmt == QLatin1String("tif")) {
        const int comp =
            tiffCompressionCombo_ ? tiffCompressionCombo_->currentIndex() : 1;
        const bool wide = pixelFormatCombo_ &&
                          (pixelFormatCombo_->currentData().toString() ==
                               QLatin1String("rgb16") ||
                           pixelFormatCombo_->currentData().toString() ==
                               QLatin1String("gray16"));
        bytesPerPixel = (wide ? 8.0 : 4.0) *
                        (comp == 2 ? 0.55 : comp == 1 ? 0.65 : 1.0);
        if (pixelFormatCombo_ &&
            pixelFormatCombo_->currentData().toString().startsWith(
                QLatin1String("gray")))
            bytesPerPixel /= 3.0;
    } else if (fmt == QLatin1String("psd") || fmt == QLatin1String("psb") ||
               fmt == QLatin1String("xcf")) {
        bytesPerPixel = 4.2;
    }
    const qint64 estBytes = qint64(pixels * bytesPerPixel);
    const double mb = estBytes / (1024.0 * 1024.0);
    if (limitSizeCheck_ && limitSizeCheck_->isChecked() &&
        (fmt == QLatin1String("png") || fmt == QLatin1String("gif"))) {
        estimateLabel_->setText(
            tr("Estimated size: ≤ %1 MB (auto palette)  •  %2, %3×%4")
                .arg(maxSizeSpin_ ? maxSizeSpin_->value() : 1.5, 0, 'f', 1)
                .arg(fmt.toUpper())
                .arg(widthSpin_->value())
                .arg(heightSpin_->value()));
        return;
    }
    estimateLabel_->setText(
        tr("Estimated size: ~%1 MB  •  %2, %3×%4")
            .arg(mb, 0, 'f', mb < 1.0 ? 2 : 1)
            .arg(fmt.toUpper())
            .arg(widthSpin_->value())
            .arg(heightSpin_->value()));
}


void ExportDialog::syncHeightFromWidth() {
    if (!lockAspectCheck_->isChecked() || aspect_ <= 0) return;
    loading_ = true;
    heightSpin_->setValue(qMax(1, int(widthSpin_->value() / aspect_ + 0.5)));
    loading_ = false;
}


void ExportDialog::syncWidthFromHeight() {
    if (!lockAspectCheck_->isChecked() || aspect_ <= 0) return;
    loading_ = true;
    widthSpin_->setValue(qMax(1, int(heightSpin_->value() * aspect_ + 0.5)));
    loading_ = false;
}


void ExportDialog::applyScaleChip(double percent, const QString& suffix) {
    settings_.scalePercent = percent;
    settings_.scaleSuffix = suffix;
    const QSize natural = targetNaturalSize();
    loading_ = true;
    widthSpin_->setValue(qMax(1, int(natural.width() * percent / 100.0)));
    heightSpin_->setValue(qMax(1, int(natural.height() * percent / 100.0)));
    loading_ = false;
    refreshEstimatedSize();
}


void ExportDialog::applyPreset(int index) {
    if (loading_ || index <= 0 || !presetCombo_) return;   // 0 = Custom
    const QString data = presetCombo_->itemData(index).toString();
    if (data.startsWith(QLatin1String("builtin:"))) {
        applyPresetId(data.mid(QStringLiteral("builtin:").size()));
        return;
    }
    if (data.startsWith(QLatin1String("user:"))) {
        const int ui = data.mid(QStringLiteral("user:").size()).toInt();
        if (ui < 0 || ui >= presets_.size()) return;
        // User presets are full bundles (legacy JSON keys still load).
        ExportSettings s = presets_[ui].s;
        s.pixelSize = QSize(widthSpin_->value(), heightSpin_->value());
        s.exportTarget = currentTarget();
        s.exportLocation = exportLocationCombo_->currentIndex();
        const int fi = formatCombo_->findData(s.format);
        if (fi >= 0) {
            const QSignalBlocker block(formatCombo_);
            formatCombo_->setCurrentIndex(fi);
            rebuildPresetList();
            // Re-select this user row after the refill.
            for (int i = 0; i < presetCombo_->count(); ++i) {
                if (presetCombo_->itemData(i).toString() == data) {
                    const QSignalBlocker b2(presetCombo_);
                    presetCombo_->setCurrentIndex(i);
                    break;
                }
            }
        }
        showSettings(s);
        return;
    }
}


void ExportDialog::savePreset() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Save Export Preset"), tr("Preset name:"),
                              QLineEdit::Normal, tr("My Preset"), &ok)
            .trimmed();
    if (!ok || name.isEmpty()) return;

    SavedPreset preset{name, collect()};
    for (int i = 0; i < presets_.size(); ++i) {
        if (presets_[i].name != name) continue;
        presets_[i] = preset;   // overwrite an existing name in place
        presetCombo_->setItemText(i + 3, name);
        persistPresets();
        return;
    }
    presets_.append(preset);
    presetCombo_->addItem(name);
    persistPresets();
}


void ExportDialog::loadPresets() {
    presets_.clear();
    QFile file(presetsFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject o = value.toObject();
        SavedPreset p;
        p.name = o.value(QStringLiteral("name")).toString();
        if (p.name.isEmpty()) continue;
        ExportSettings& s = p.s;
        s.format = o.value(QStringLiteral("format")).toString(s.format);
        s.presetId = o.value(QStringLiteral("presetId")).toString();
        s.quality = o.value(QStringLiteral("quality")).toInt(s.quality);
        s.dither = o.value(QStringLiteral("dither")).toBool(s.dither);
        s.palettized = o.value(QStringLiteral("palettized")).toBool(s.dither);
        s.paletteColors =
            o.value(QStringLiteral("paletteColors")).toInt(s.paletteColors);
        s.paletteDither =
            o.value(QStringLiteral("paletteDither")).toBool(s.paletteDither);
        s.limitFileSize =
            o.value(QStringLiteral("limitFileSize")).toBool(s.limitFileSize);
        s.maxFileSizeMB =
            o.value(QStringLiteral("maxFileSizeMB")).toDouble(s.maxFileSizeMB);
        s.interlace = o.value(QStringLiteral("interlace")).toBool(s.interlace);
        s.progressive = s.interlace;
        s.dpi = o.value(QStringLiteral("dpi")).toInt(s.dpi);
        s.resampleMode = o.value(QStringLiteral("resample")).toInt(s.resampleMode);
        s.colorProfile =
            o.value(QStringLiteral("colorProfile")).toString(s.colorProfile);
        s.embedIccProfile =
            o.value(QStringLiteral("embedIcc")).toBool(s.embedIccProfile);
        s.hideHiddenLayers =
            o.value(QStringLiteral("hideHidden")).toBool(s.hideHiddenLayers);
        s.pixelFormat =
            o.value(QStringLiteral("pixelFormat")).toString(s.pixelFormat);
        s.matteEnabled = o.value(QStringLiteral("matteEnabled")).toBool();
        if (o.contains(QStringLiteral("matte")))
            s.matte = QColor(o.value(QStringLiteral("matte")).toString());
        s.embedMetadata =
            o.value(QStringLiteral("embedMetadata")).toBool(s.embedMetadata);
        s.transferFunction =
            o.value(QStringLiteral("transfer")).toInt(s.transferFunction);
        s.primaries = o.value(QStringLiteral("primaries")).toInt(s.primaries);
        s.fullRange = o.value(QStringLiteral("fullRange")).toBool(s.fullRange);
        s.tiffCompression =
            o.value(QStringLiteral("tiffCompression")).toInt(s.tiffCompression);
        s.svgRasterize =
            o.value(QStringLiteral("svgRasterize")).toInt(s.svgRasterize);
        s.svgUseDocRes =
            o.value(QStringLiteral("svgUseDocRes")).toBool(s.svgUseDocRes);
        s.svgRasterDpi =
            o.value(QStringLiteral("svgRasterDpi")).toInt(s.svgRasterDpi);
        s.svgDownsample =
            o.value(QStringLiteral("svgDownsample")).toBool(s.svgDownsample);
        s.svgAboveDpi =
            o.value(QStringLiteral("svgAboveDpi")).toInt(s.svgAboveDpi);
        s.svgAllowJpeg =
            o.value(QStringLiteral("svgAllowJpeg")).toBool(s.svgAllowJpeg);
        s.svgJpegQuality =
            o.value(QStringLiteral("svgJpegQuality")).toInt(s.svgJpegQuality);
        s.svgDecimalPlaces =
            o.value(QStringLiteral("svgDecimals")).toInt(s.svgDecimalPlaces);
        s.svgSetViewbox =
            o.value(QStringLiteral("svgViewbox")).toBool(s.svgSetViewbox);
        s.svgLineBreaks =
            o.value(QStringLiteral("svgBreaks")).toBool(s.svgLineBreaks);
        presets_.append(p);
    }
}


void ExportDialog::persistPresets() {
    QJsonArray array;
    for (const SavedPreset& p : presets_) {
        const ExportSettings& s = p.s;
        QJsonObject o;
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("format"), s.format);
        o.insert(QStringLiteral("presetId"), s.presetId);
        o.insert(QStringLiteral("quality"), s.quality);
        o.insert(QStringLiteral("dither"), s.dither);
        o.insert(QStringLiteral("palettized"), s.palettized);
        o.insert(QStringLiteral("paletteColors"), s.paletteColors);
        o.insert(QStringLiteral("paletteDither"), s.paletteDither);
        o.insert(QStringLiteral("limitFileSize"), s.limitFileSize);
        o.insert(QStringLiteral("maxFileSizeMB"), s.maxFileSizeMB);
        o.insert(QStringLiteral("interlace"), s.interlace);
        o.insert(QStringLiteral("dpi"), s.dpi);
        o.insert(QStringLiteral("resample"), s.resampleMode);
        o.insert(QStringLiteral("colorProfile"), s.colorProfile);
        o.insert(QStringLiteral("embedIcc"), s.embedIccProfile);
        o.insert(QStringLiteral("hideHidden"), s.hideHiddenLayers);
        o.insert(QStringLiteral("pixelFormat"), s.pixelFormat);
        o.insert(QStringLiteral("matteEnabled"), s.matteEnabled);
        o.insert(QStringLiteral("matte"), s.matte.name());
        o.insert(QStringLiteral("embedMetadata"), s.embedMetadata);
        o.insert(QStringLiteral("transfer"), s.transferFunction);
        o.insert(QStringLiteral("primaries"), s.primaries);
        o.insert(QStringLiteral("fullRange"), s.fullRange);
        o.insert(QStringLiteral("tiffCompression"), s.tiffCompression);
        o.insert(QStringLiteral("svgRasterize"), s.svgRasterize);
        o.insert(QStringLiteral("svgUseDocRes"), s.svgUseDocRes);
        o.insert(QStringLiteral("svgRasterDpi"), s.svgRasterDpi);
        o.insert(QStringLiteral("svgDownsample"), s.svgDownsample);
        o.insert(QStringLiteral("svgAboveDpi"), s.svgAboveDpi);
        o.insert(QStringLiteral("svgAllowJpeg"), s.svgAllowJpeg);
        o.insert(QStringLiteral("svgJpegQuality"), s.svgJpegQuality);
        o.insert(QStringLiteral("svgDecimals"), s.svgDecimalPlaces);
        o.insert(QStringLiteral("svgViewbox"), s.svgSetViewbox);
        o.insert(QStringLiteral("svgBreaks"), s.svgLineBreaks);
        array.append(o);
    }
    QSaveFile file(presetsFilePath());
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.commit();
}

}  // namespace pittore::ui
