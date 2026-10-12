#include "ui/main_window.h"
#include "ui/color_mode.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSet>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "engine/ai/bg_remove.h"
#include "engine/compute/factory.h"
#include "engine/compute/adjust.h"
#include "engine/core/log.h"
#include "engine/core/tonal_ops.h"
#include "ui/ai_models.h"
#include "ui/af_export.h"
#include "engine/io/af_layers/emit/af_write.h"
#include "ui/canvas_view.h"
#include "ui/color_mode.h"
#include "ui/contextual_task_bar.h"
#include "ui/embedded_icc.h"
#include "ui/export_dialog.h"
#include "ui/export/shared/export_helpers.h"
#include "ui/icons.h"
#include "ui/layer_style_dialog.h"
#include "ui/options_bar.h"
#include "ui/panels.h"
#include "ui/keymap.h"
#include "ui/spotlight.h"
#include "ui/filter_dialog.h"
#include "engine/filter/filters.h"
#include "ui/preferences_dialog.h"
#include "ui/project_manager.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "ui/theme.h"
#include "ui/tone_dialogs.h"
#include "ui/tools_panel.h"
#include "ui/workspace.h"
#include "ui/window/shared/window_helpers.h"

namespace pittore::ui {


// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

void MainWindow::newDocumentDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Document"));
    auto* form = new QFormLayout(&dialog);

    auto* name = new QLineEdit(tr("Untitled-%1").arg(state_->documents().size() + 1), &dialog);
    const AppSettings& prefs = state_->settings();
    auto* width = new QSpinBox(&dialog);
    width->setRange(1, 300000);
    width->setValue(qBound(1, prefs.newDocWidth, 300000));
    width->setSuffix(tr(" px"));
    auto* height = new QSpinBox(&dialog);
    height->setRange(1, 300000);
    height->setValue(qBound(1, prefs.newDocHeight, 300000));
    height->setSuffix(tr(" px"));
    auto* dpi = new QSpinBox(&dialog);
    dpi->setRange(1, 10000);
    dpi->setValue(qBound(1, prefs.newDocDpi, 10000));
    dpi->setSuffix(tr(" ppi"));
    auto* mode = new QComboBox(&dialog);
    mode->addItems({QStringLiteral("RGB/8"), QStringLiteral("RGB/16"), QStringLiteral("RGB/32"),
                    QStringLiteral("Grayscale/8"), QStringLiteral("CMYK/8"),
                    QStringLiteral("CMYK/16")});
    const int modeIdx = mode->findData(prefs.newDocColorMode);
    mode->setCurrentIndex(modeIdx >= 0 ? modeIdx : 0);

    form->addRow(tr("Name"), name);
    form->addRow(tr("Width"), width);
    form->addRow(tr("Height"), height);
    form->addRow(tr("Resolution"), dpi);
    form->addRow(tr("Color Mode"), mode);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        DocumentItem* doc = state_->addDocument(name->text(),
                                                QSize(width->value(), height->value()),
                                                dpi->value());
        if (doc) {
            doc->colorMode = mode->currentText();
            if (doc->colorMode.startsWith(QStringLiteral("CMYK"),
                                          Qt::CaseInsensitive))
                doc->iccProfile = resolveCmykProfile(*doc);
        }
        canvas_->zoomToFit();
    }
}


// ---------------------------------------------------------------------------
// Project manager / own-format project actions
// ---------------------------------------------------------------------------

void MainWindow::showStartPage() {
    if (startPageActive_) return;   // modal dialog; never re-enter
    startPageActive_ = true;
    ProjectManagerDialog dialog(state_, this);
    dialog.refresh();
    const bool opened = dialog.exec() == QDialog::Accepted;
    startPageActive_ = false;
    if (opened) {
        canvas_->zoomToFit();
        syncDocumentTabs();
        updateStatus();
        canvas_->viewport()->repaint();
        if (!state_->resolvePendingProfileMismatch()) {
            syncDocumentTabs();
            updateStatus();
        } else {
            updateStatus();
        }
    }
}


void MainWindow::openProjectDialog() {
    QString startDir = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    if (startDir.isEmpty()) startDir = projectsRootDir();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open"), startDir, imageOpenFilter());
    if (!path.isEmpty()) openProjectFile(path);
}


void MainWindow::openProjectFile(const QString& path) {
    ::pittore::core::log::log_info("[import] openProjectFile path=%s",
                                    path.toUtf8().constData());
    QString error;
    // Draw first, then ask: the image paints before the profile question.
    // Defer any mismatch dialog past the open; drain it after sync + a
    // synchronous repaint so it opens over the visible image.
    state_->setDeferMismatchDialogs(true);
    const bool ok =
        isNativeProjectSuffix(QFileInfo(path).suffix())
            ? state_->openProject(path, &error)
            : state_->openImageFile(path, &error);
    state_->setDeferMismatchDialogs(false);
    if (!ok) {
        // Cancel from the profile-mismatch dialog leaves *error empty: a
        // quiet abort, not a failure worth a warning box.
        if (error.isEmpty()) {
            ::pittore::core::log::log_info("[import] open cancelled %s",
                                            QFileInfo(path).fileName().toUtf8().constData());
            return;
        }
        ::pittore::core::log::log_warning("[import] open failed %s: %s",
                                           QFileInfo(path).fileName().toUtf8().constData(),
                                           error.toUtf8().constData());
        QMessageBox::warning(this, tr("Open"), error);
        return;
    }
    ::pittore::core::log::log_info("[import] open finished %s",
                                    QFileInfo(path).fileName().toUtf8().constData());
    canvas_->zoomToFit();
    syncDocumentTabs();
    updateStatus();
    // Synchronous viewport paint (QWidget::repaint on the scroll area would
    // only repaint the chrome, not the image): the picture must be on screen
    // before the profile question lands.
    canvas_->viewport()->repaint();
    if (!state_->resolvePendingProfileMismatch(&error)) {
        syncDocumentTabs();
        updateStatus();
    } else {
        updateStatus();  // surface the keep/convert hint
    }
}


void MainWindow::placeAt(const QPointF& docPos) {
    DocumentItem* doc = state_->activeDocument();
    if (!doc) {
        state_->setStatusHint(tr("Place: open a document first."));
        return;
    }
    QString startDir = QStandardPaths::writableLocation(
        QStandardPaths::PicturesLocation);
    if (startDir.isEmpty()) startDir = projectsRootDir();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Place"), startDir, imageOpenFilter());
    if (path.isEmpty()) return;
    const QString name = QFileInfo(path).completeBaseName();
    if (QFileInfo(path).suffix().compare(QStringLiteral("svg"),
                                          Qt::CaseInsensitive) == 0) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(
                this, tr("Place"),
                tr("Could not read %1.").arg(QFileInfo(path).fileName()));
            return;
        }
        SvgImportResult svg;
        int dpi = 96;
        QString error;
        if (!svgPartsImport(file.readAll(), &svg, &dpi, &error) ||
            !state_->placeSvgParts(path, svg, docPos, &error)) {
            QMessageBox::warning(this, tr("Place"), error);
            return;
        }
    } else {
        const QImage img(path);
        if (img.isNull()) {
            QMessageBox::warning(
                this, tr("Place"),
                tr("Could not decode %1.").arg(QFileInfo(path).fileName()));
            return;
        }
        // Same mismatch policy as drops (ask when pasting):
        // Cancel aborts the place, anything else places as decoded. The
        // helper scans the container when the decode reports no color space,
        // so a profiled file still asks.
        if (!state_->resolveImportedProfile(embeddedProfileName(path, img)))
            return;
        // Mirror drops: fit oversized images down, never upscale.
        const double fit = qMin(
            1.0, qMin(double(doc->size.width()) / qMax(1, img.width()),
                      double(doc->size.height()) / qMax(1, img.height())));
        state_->placeImageLayer(img, name.isEmpty() ? tr("Placed Image") : name,
                                docPos, fit);
    }
    state_->setActiveTool(ToolId::Move);  // transform controls invite a resize
    syncDocumentTabs();
    updateStatus();
}


void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (pittore::ui::dropHasOpenableFiles(event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
}


void MainWindow::dropEvent(QDropEvent* event) {
    if (!pittore::ui::dropHasOpenableFiles(event->mimeData())) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    // Open every supported local file, each as its own document tab. Drops on
    // the canvas viewport never reach here — CanvasView handles those first.
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString localPath = url.toLocalFile();
        if (pittore::ui::suffixIsOpenable(QFileInfo(localPath).suffix()))
            openProjectFile(localPath);
    }
}


void MainWindow::saveActiveProject() {
    DocumentItem* doc = state_->activeDocument();
    if (!doc) return;
    if (doc->filePath.isEmpty()) {
        saveActiveProjectAs();
        return;
    }
    QString error;
    if (!state_->saveProject(doc->filePath, &error))
        QMessageBox::warning(this, tr("Save Project"), error);
    else
        clearRecovery();   // the work is safely on disk (R98)
}


void MainWindow::saveActiveProjectAs() {
    DocumentItem* doc = state_->activeDocument();
    if (!doc) return;
    // An imported layered file offers itself back first, so
    // edits can be saved straight back to the opened project; otherwise
    // the library.
    QString initial = doc->filePath.isEmpty() ? projectPathForName(doc->title) : doc->filePath;
    if (doc->filePath.isEmpty() && !doc->importSourcePath.isEmpty()) {
        const QString low = doc->importSourcePath.toLower();
        if (low.endsWith(QStringLiteral(".af")) ||
            low.endsWith(QStringLiteral(".afphoto")) ||
            low.endsWith(QStringLiteral(".afdesign")) || low.endsWith(QStringLiteral(".afpub")) ||
            low.endsWith(QStringLiteral(".psd")))
            initial = doc->importSourcePath;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Project As"), initial,
        tr("Pittore Studio Project (*.psc);;Pittore Studio Legacy (*.ifp);;Photoshop (*.psd);;Affinity Photo (*.afphoto);;Affinity (*.af)"));
    if (path.isEmpty()) return;
    QString target = path;
    const QString low = target.toLower();
    const bool isAf = low.endsWith(QStringLiteral(".af")) ||
                      low.endsWith(QStringLiteral(".afphoto")) ||
                      low.endsWith(QStringLiteral(".afdesign")) ||
                      low.endsWith(QStringLiteral(".afpub"));
    const bool isPsd = low.endsWith(QStringLiteral(".psd"));
    const bool isLegacy = low.endsWith(QStringLiteral(".ifp"));
    if (!isAf && !isPsd && !isLegacy && !target.endsWith(QStringLiteral(".psc"), Qt::CaseInsensitive))
        target += QStringLiteral(".psc");
    QString error;
    if (isAf) {
        // Saving to Affinity form bakes or drops whatever has no Affinity
        // record: confirm with the list before the original is replaced.
        auto enc = state_->exportAfLayers(*doc, &error);
        if (!enc) {
            QMessageBox::warning(this, tr("Save Project"),
                                 error.isEmpty() ? tr("Could not encode Affinity file.")
                                                 : error);
            return;
        }
        if (!enc->skipped.empty()) {
            QStringList notes;
            for (const auto& s : enc->skipped) notes.append(QString::fromStdString(s));
            QMessageBox msg(QMessageBox::Warning, tr("Save Project"),
                            tr("Saving as Affinity will bake or drop %1 thing(s). "
                               "Save anyway?")
                                .arg(notes.size()),
                            QMessageBox::Save | QMessageBox::Cancel, this);
            msg.setDetailedText(notes.join(QStringLiteral("\n")));
            if (msg.exec() != QMessageBox::Save) return;
        }
    }
    if (!state_->saveProject(target, &error))
        QMessageBox::warning(this, tr("Save Project"), error);
    else
        clearRecovery();   // the work is safely on disk (R98)
}


void MainWindow::exportActiveDocument() {
    // Export As… / Save a Copy…: the Export dialog collects the format, size,
    // resample, DPI, colour-space and quality choices, then writes the result
    // through saveImageFile(). Never touches the native project file; the
    // document stays linked to its .psc.
    DocumentItem* doc = state_->activeDocument();
    if (!doc || doc->size.isEmpty()) return;

    ExportDialog dialog(state_, doc, this);
    if (dialog.exec() != QDialog::Accepted) return;
    ExportSettings exportSettings = dialog.settings();
    // "Use document format" on a CMYK-tagged document delivers TIFF through
    // the document's separation (ink planes + embedded profile); an explicit
    // RGB/gray pixel format overrides, and non-TIFF writers ignore the flag.
    if (exportSettings.pixelFormat == QLatin1String("doc") &&
        doc->colorMode.startsWith(QStringLiteral("CMYK"))) {
        exportSettings.cmyk = true;
        exportSettings.icc = resolveCmykProfile(*doc);
    }

    // "Don't export hidden layers" off reveals everything for the render;
    // the guard restores visibility (and the composite) afterwards, even on
    // the early returns below.
    export_detail::AllLayersVisibleGuard visGuard(!exportSettings.hideHiddenLayers ? doc
                                                                    : nullptr);

    // Source pixels + the crop rect for the chosen target. A single layer is
    // rendered alone and cropped to its own bounds; a selection crops the
    // composite; otherwise the whole canvas goes out.
    const QRect canvas(QPoint(0, 0), doc->size);
    QImage source = doc->composite;
    QRect crop = canvas;
    if (exportSettings.exportTarget == -2) {
        if (!doc->selection.isEmpty()) {
            const QRect sel = doc->selection.toAlignedRect().intersected(canvas);
            if (!sel.isEmpty()) crop = sel;
        }
    } else if (exportSettings.exportTarget >= 0 &&
               exportSettings.exportTarget < doc->layers.size()) {
        source = renderLayerForExport(doc, exportSettings.exportTarget);
        const QRect bounds = layerBounds(*doc, doc->layers[exportSettings.exportTarget])
                                 .toAlignedRect()
                                 .intersected(canvas);
        if (!bounds.isEmpty()) crop = bounds;
    }
    if (source.isNull()) return;

    // Resolve the destination from the Export Location choice.
    const QString stem = exportBaseName(*doc, exportSettings);
    const QString fileName = stem + QLatin1Char('.') + exportSettings.format;
    const QString documentDir =
        doc->filePath.isEmpty() ? QString() : QFileInfo(doc->filePath).absolutePath();
    QString path;
    if (exportSettings.exportLocation == 1 && !documentDir.isEmpty()) {
        path = documentDir + QLatin1Char('/') + fileName;
    } else if (exportSettings.exportLocation == 2) {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Export To Folder"), documentDir);
        if (dir.isEmpty()) return;
        path = dir + QLatin1Char('/') + fileName;
    } else {
        const QString initial =
            documentDir.isEmpty() ? fileName : documentDir + QLatin1Char('/') + fileName;
        path = QFileDialog::getSaveFileName(this, tr("Export As"), initial,
                                            imageExportFilter());
        if (path.isEmpty()) return;
        if (!path.endsWith(QLatin1Char('.') + exportSettings.format, Qt::CaseInsensitive))
            path += QLatin1Char('.') + exportSettings.format;
    }

    QString error;
    QString report;
    // SVG goes through the vector writer, which needs the layer stack (real
    // paths and per-layer raster), not the flattened composite.
    const bool ok = exportSettings.format == QLatin1String("svg")
                        ? writeExportVector(*doc, exportSettings, path, &error)
                        : writeExportImage(source, crop, exportSettings, path,
                                           &error, &report);
    if (!ok) {
        QMessageBox::warning(this, tr("Export"), error);
        return;
    }
    state_->setStatusHint(
        report.isEmpty()
            ? tr("Exported %1").arg(QFileInfo(path).fileName())
            : tr("Exported %1 (%2)").arg(QFileInfo(path).fileName(), report));
}


void MainWindow::exportLayeredPsd() {    // File ▸ Export ▸ Export Layered PSD…: the inverse of the layered PSD
    // import — group structure, blends, masks and live adjustments ride as
    // native PSD records (see AppState::exportLayeredPsd). Unlike Export As…
    // (flattened), the document keeps editing normally afterwards.
    DocumentItem* doc = state_->activeDocument();
    if (!doc || doc->size.isEmpty()) return;
    const QString base = doc->filePath.isEmpty()
                             ? doc->title
                             : QFileInfo(doc->filePath).completeBaseName();
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Layered PSD"), base + QStringLiteral(".psd"),
        tr("Photoshop (*.psd)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".psd"), Qt::CaseInsensitive))
        path += QStringLiteral(".psd");
    QString error;
    auto bytes = state_->exportLayeredPsd(*doc, &error);
    if (!bytes) {
        QMessageBox::warning(this, tr("Export"),
                             error.isEmpty() ? tr("Could not encode PSD.")
                                             : error);
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Could not write %1.").arg(path));
        return;
    }
    f.write(reinterpret_cast<const char*>(bytes->data()),
            static_cast<qint64>(bytes->size()));
    state_->setStatusHint(tr("Exported %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::exportLayeredAf() {
    // File ▸ Export ▸ Export Layered Affinity…: the inverse of the layered
    // .af import — group structure, blends, masks and placed rasters ride
    // as a native .af graph (see AppState::exportAfLayers). Adjustments,
    // live text, vector art and effects bake into the pixels; the dialog
    // lists what was baked so nothing goes silently.
    DocumentItem* doc = state_->activeDocument();
    if (!doc || doc->size.isEmpty()) return;
    const QString base = doc->filePath.isEmpty()
                             ? doc->title
                             : QFileInfo(doc->filePath).completeBaseName();
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Layered Affinity"), base + QStringLiteral(".afphoto"),
        tr("Affinity Photo (*.afphoto);;Affinity (*.af)"));
    if (path.isEmpty()) return;
    const QString low = path.toLower();
    if (!low.endsWith(QStringLiteral(".afphoto")) && !low.endsWith(QStringLiteral(".af")) &&
        !low.endsWith(QStringLiteral(".afdesign")) && !low.endsWith(QStringLiteral(".afpub")))
        path += QStringLiteral(".afphoto");
    QString error;
    auto enc = state_->exportAfLayers(*doc, &error);
    if (!enc) {
        QMessageBox::warning(this, tr("Export"),
                             error.isEmpty() ? tr("Could not encode Affinity file.")
                                             : error);
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Could not write %1.").arg(path));
        return;
    }
    f.write(reinterpret_cast<const char*>(enc->bytes.data()),
            static_cast<qint64>(enc->bytes.size()));
    if (!enc->skipped.empty()) {
        QStringList notes;
        for (const auto& s : enc->skipped) notes.append(QString::fromStdString(s));
        QMessageBox msg(QMessageBox::Information, tr("Export"),
                        tr("Exported %1 with %2 note(s):")
                            .arg(QFileInfo(path).fileName())
                            .arg(notes.size()),
                        QMessageBox::Ok, this);
        msg.setDetailedText(notes.join(QStringLiteral("\n")));
        msg.exec();
    } else {
        state_->setStatusHint(tr("Exported %1").arg(QFileInfo(path).fileName()));
    }
}

void MainWindow::quickExportPng() {
    DocumentItem* doc = state_->activeDocument();
    if (!doc) return;
    const QString base = doc->filePath.isEmpty()
                             ? doc->title
                             : QFileInfo(doc->filePath).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Quick Export as PNG"), base + QStringLiteral(".png"),
        tr("PNG Image (*.png)"));
    if (path.isEmpty()) return;
    QString target = path;
    if (!target.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        target += QStringLiteral(".png");
    QString error;
    if (!saveImageFile(doc->composite, target, &error))
        QMessageBox::warning(this, tr("Export"), error);
}

}  // namespace pittore::ui
