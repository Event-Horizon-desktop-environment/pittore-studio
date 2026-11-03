#include "ui/main_window.h"

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
#include "ui/canvas_view.h"
#include "ui/contextual_task_bar.h"
#include "ui/export_dialog.h"
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
#include "ui/theme.h"
#include "ui/tone_blend_dialog.h"
#include "ui/tone_dialogs.h"
#include "ui/color_grade_dialog.h"
#include "ui/tools_panel.h"
#include "ui/workspace.h"
#include "ui/window/shared/window_helpers.h"

namespace pittore::ui {


void MainWindow::filterDialog(const QString &filterId) {
    if (!state_->activeDocument()) {
        state_->setStatusHint(tr("Open a document first."));
        return;
    }
    FilterDialog dialog(state_, filterId, this);
    if (dialog.exec() == QDialog::Accepted) {
        lastFilterId_ = filterId;
        lastFilterParams_ = dialog.appliedParams();
    }
}


void MainWindow::filterGalleryDialog(const QString &category) {
    if (!state_->activeDocument()) {
        state_->setStatusHint(tr("Open a document first."));
        return;
    }
    FilterGalleryDialog dialog(state_, category, this);
    dialog.exec();
}


void MainWindow::applyFilterOneShot(const QString &filterId, const std::vector<double> &params) {
    if (!state_->activeDocument()) {
        state_->setStatusHint(tr("Open a document first."));
        return;
    }
    const filter::FilterDef *def = filter::findFilter(filterId.toStdString());
    const QString name = def ? QString::fromUtf8(def->name) : filterId;
    std::vector<double> p = params.empty() ? filter::defaultParams(filterId.toStdString()) : params;
    const bool ok = state_->applyLayerEditOneShot(
        [filterId, p](pittore::Image &img) {
            filter::applyFilter(img, filterId.toStdString(), p);
        },
        name, QStringLiteral("filter"));
    if (ok) {
        lastFilterId_ = filterId;
        lastFilterParams_ = p;
    } else {
        state_->setStatusHint(tr("Need an editable pixel layer."));
    }
}


void MainWindow::layerStyleDialog(int effectIndex) {    const QVector<int> targets = state_->fxTargetLayers();
    if (targets.isEmpty()) {
        // Styles render from the layers' own pixels, so group headers and
        // pixel-less stand-ins have nothing to style.
        state_->setStatusHint(tr("Layer styles need a pixel layer"));
        return;
    }
    LayerStyleDialog dialog(state_, this);
    if (effectIndex >= 0 &&
        effectIndex < static_cast<int>(StyleEffect::Count))
        dialog.selectEffect(static_cast<StyleEffect>(effectIndex));
    dialog.exec();
}


void MainWindow::toneBlendDialog(int groupIndex) {
    DocumentItem* doc = state_->activeDocument();
    if (!doc || groupIndex < 0 || groupIndex >= doc->layers.size()) {
        state_->setStatusHint(tr("Select a tone blend group first."));
        return;
    }
    const LayerItem& layer = doc->layers[groupIndex];
    if (layer.kind != LayerItem::Kind::Group || !layer.toneBlendGroup) {
        state_->setStatusHint(tr("Tone Blend needs a tone blend group."));
        return;
    }
    ToneBlendDialog dialog(state_, groupIndex, this);
    dialog.exec();
}


// Tonal / filter dialogs (R49, R54). Each opens a live-preview dialog on the
// active pixel layer; the session commits as one history entry.
void MainWindow::levelsDialog() {
    if (!state_->activeDocument()) return;
    LevelsDialog dialog(state_, this);
    dialog.exec();
}


void MainWindow::curvesDialog() {
    if (!state_->activeDocument()) return;
    CurvesDialog dialog(state_, this);
    dialog.exec();
}


void MainWindow::colorGradeDialog() {
    if (!state_->activeDocument()) {
        state_->setStatusHint(tr("Open a document first."));
        return;
    }
    // The dialog edits the active adjustment layer, creating a Photo Filter
    // one when needed, so this always opens a window with options.
    // Affinity surfaces its adjustment chooser beside the canvas while
    // Colour Grading is up: raise the Adjustments panel on the left rail so
    // the two read side by side.
    raisePanelOnLeft(QStringLiteral("adjustments"));
    ColorGradeDialog dialog(state_, this);
    dialog.exec();
    updateStatus();
    syncUndoRedo();
}


void MainWindow::addNoiseDialog() {
    if (!state_->activeDocument()) return;
    AddNoiseDialog dialog(state_, this);
    dialog.exec();
}


void MainWindow::medianDialog() {
    if (!state_->activeDocument()) return;
    MedianDialog dialog(state_, this);
    dialog.exec();
}


void MainWindow::unsharpMaskDialog() {
    if (!state_->activeDocument()) return;
    UnsharpMaskDialog dialog(state_, this);
    dialog.exec();
}


void MainWindow::sharpenActiveLayer(double amount) {
    state_->applyLayerEditOneShot(
        [amount](pittore::Image& img) { pittore::applyUnsharpMask(img, amount, 1, 0.0); },
        tr("Sharpen"), QStringLiteral("sharpen"));
}


// R17: add a single guide at a numeric position (guides are not undo-tracked,
// matching Clear Guides; the document is marked modified instead).
void MainWindow::newGuideDialog() {
    DocumentItem* doc = state_->activeDocument();
    if (!doc) {
        state_->setStatusHint(tr("New Guide: open a document first."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New Guide"));
    auto* orientation = new QComboBox(&dialog);
    orientation->addItems({tr("Horizontal"), tr("Vertical")});
    auto* position = new QDoubleSpinBox(&dialog);
    position->setRange(-100000.0, 100000.0);
    position->setDecimals(1);
    position->setValue(0.0);
    position->setSuffix(QStringLiteral(" px"));
    auto* form = new QFormLayout;
    form->addRow(tr("Orientation:"), orientation);
    form->addRow(tr("Position:"), position);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    if (orientation->currentIndex() == 0)
        doc->horizontalGuides.append(position->value());
    else
        doc->verticalGuides.append(position->value());
    emit state_->documentModified(doc);
}

}  // namespace pittore::ui
