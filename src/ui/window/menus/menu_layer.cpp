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
#include "ui/tone_dialogs.h"
#include "ui/tools_panel.h"
#include "ui/workspace.h"
#include "ui/window/shared/window_helpers.h"

namespace pittore::ui {


void MainWindow::buildLayerMenu() {
    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    QMenu* newMenu = layer->addMenu(tr("New"));
    makeAction(newMenu, tr("Layer…"), QStringLiteral("Ctrl+Shift+N"),
               [this] { runCommand(QStringLiteral("new-layer")); });
    makeAction(newMenu, tr("Group…"), QString(), [this] { runCommand(QStringLiteral("new-group")); });
    makeAction(newMenu, tr("Group from Layers…"), QStringLiteral("Ctrl+G"),
               [this] { runCommand(QStringLiteral("group-layers")); });
    makeAction(newMenu, tr("Live Tone Blend Group…"), {},
               [this] { runCommand(QStringLiteral("tone-blend-group")); });
    makeAction(newMenu, tr("Ungroup Layers"), QStringLiteral("Ctrl+Shift+G"),
               [this] { runCommand(QStringLiteral("ungroup-layers")); });
    makeAction(newMenu, tr("Artboard…"));
    makeAction(newMenu, tr("Layer via Copy"), QStringLiteral("Ctrl+J"),
               [this] { runCommand(QStringLiteral("layer-via-copy")); });
    makeAction(newMenu, tr("Layer via Cut"), QStringLiteral("Ctrl+Shift+J"));
    makeAction(layer, tr("Duplicate Layer…"), QString(),
               [this] { runCommand(QStringLiteral("duplicate-layers")); });
    makeAction(layer, tr("Delete Layer"), QString(),
               [this] { runCommand(QStringLiteral("delete-layer")); });
    layer->addSeparator();
    makeAction(layer, tr("Quick Export as PNG"));
    layer->addSeparator();

    QMenu* fill = layer->addMenu(tr("New Fill Layer"));
    for (const QString& name : {tr("Solid Color…"), tr("Gradient…"), tr("Pattern…")})
        makeAction(fill, name);
    QMenu* adjustmentLayer = layer->addMenu(tr("New Adjustment Layer"));
    for (const QString& name :
         {tr("Brightness/Contrast…"), tr("Levels…"), tr("Curves…"), tr("Exposure…"),
          tr("Vibrance…"), tr("Hue/Saturation…")})
        makeAction(adjustmentLayer, name);
    makeAction(adjustmentLayer, tr("Color Balance…"), QString(), [this] {
        state_->addAdjustmentLayer(static_cast<int>(
            pittore::compute::AdjustmentKind::ColorBalance));
    });
    makeAction(adjustmentLayer, tr("Black & White…"), QString(), [this] {
        state_->addAdjustmentLayer(static_cast<int>(
            pittore::compute::AdjustmentKind::BlackWhite));
    });
    makeAction(adjustmentLayer, tr("Photo Filter…"), QString(), [this] {
        state_->addAdjustmentLayer(static_cast<int>(
            pittore::compute::AdjustmentKind::PhotoFilter));
    });
    makeAction(adjustmentLayer, tr("Channel Mixer…"), QString(), [this] {
        state_->addAdjustmentLayer(static_cast<int>(
            pittore::compute::AdjustmentKind::ChannelMixer));
    });
    makeAction(adjustmentLayer, tr("Recolour…"), QString(), [this] {
        state_->addAdjustmentLayer(static_cast<int>(
            pittore::compute::AdjustmentKind::Recolour));
    });
    for (const QString& name :
         {tr("Color Lookup…"), tr("Invert"),
          tr("Posterize…"), tr("Threshold…"), tr("Gradient Map…"), tr("Selective Color…")})
        makeAction(adjustmentLayer, name);
    makeAction(layer, tr("Layer Content Options…"));
    layer->addSeparator();

    QMenu* mask = layer->addMenu(tr("Layer Mask"));
    makeAction(mask, tr("Reveal All"), {},
               [this] { runCommand(QStringLiteral("mask-reveal")); });
    makeAction(mask, tr("Hide All"), {},
               [this] { runCommand(QStringLiteral("mask-hide")); });
    makeAction(mask, tr("Reveal Selection"), {},
               [this] { runCommand(QStringLiteral("mask-reveal-selection")); });
    makeAction(mask, tr("Hide Selection"), {},
               [this] { runCommand(QStringLiteral("mask-hide-selection")); });
    makeAction(mask, tr("From Transparency"), {},
               [this] { runCommand(QStringLiteral("mask-from-transparency")); });
    makeAction(mask, tr("Apply"), {},
               [this] { runCommand(QStringLiteral("mask-apply")); });
    makeAction(mask, tr("Delete"), {},
               [this] { runCommand(QStringLiteral("mask-delete")); });
    makeAction(mask, tr("Enable"), {},
               [this] { runCommand(QStringLiteral("mask-enable-toggle")); });
    makeAction(mask, tr("Unlink"), {},
               [this] { runCommand(QStringLiteral("mask-link-toggle")); });
    QMenu* vectorMask = layer->addMenu(tr("Vector Mask"));
    for (const QString& name : {tr("Reveal All"), tr("Hide All"), tr("Current Path"), tr("Delete")})
        makeAction(vectorMask, name);
    makeAction(layer, tr("Create Clipping Mask"), QStringLiteral("Ctrl+Alt+G"),
               [this] { runCommand(QStringLiteral("toggle-clip")); });
    layer->addSeparator();

    QMenu* style = layer->addMenu(tr("Layer Style"));
    makeAction(style, tr("Blending Options…"), {}, [this] { layerStyleDialog(-1); });
    const QVector<QPair<QString, int>> styleEffects = {
        {tr("Bevel & Emboss…"), 0},    {tr("Stroke…"), 1},
        {tr("Inner Shadow…"), 2},      {tr("Inner Glow…"), 3},
        {tr("Satin…"), 4},             {tr("Color Overlay…"), 5},
        {tr("Gradient Overlay…"), 6},  {tr("Outer Glow…"), 7},
        {tr("Drop Shadow…"), 8},       {tr("Blur…"), 9},
    };
    for (const auto& effect : styleEffects)
        makeAction(style, effect.first, {},
                   [this, row = effect.second] { layerStyleDialog(row); });
    makeAction(style, tr("Pattern Overlay…"));
    QMenu* smart = layer->addMenu(tr("Smart Objects"));
    for (const QString& name : {tr("Convert to Smart Object"), tr("New Smart Object via Copy"),
                                tr("Edit Contents"), tr("Rasterize")})
        makeAction(smart, name);
    QMenu* rasterize = layer->addMenu(tr("Rasterize"));
    for (const QString& name : {tr("Type"), tr("Shape"), tr("Fill Content"), tr("Vector Mask"),
                                tr("Smart Object"), tr("Layer"), tr("All Layers")})
        makeAction(rasterize, name);
    layer->addSeparator();

    QMenu* arrange = layer->addMenu(tr("Arrange"));
    makeAction(arrange, tr("Bring to Front"), QStringLiteral("Ctrl+Shift+]"),
               [this] { runCommand(QStringLiteral("bring-to-front")); });
    makeAction(arrange, tr("Bring Forward"), QStringLiteral("Ctrl+]"),
               [this] { runCommand(QStringLiteral("bring-forward")); });
    makeAction(arrange, tr("Send Backward"), QStringLiteral("Ctrl+["),
               [this] { runCommand(QStringLiteral("send-backward")); });
    makeAction(arrange, tr("Send to Back"), QStringLiteral("Ctrl+Shift+["),
               [this] { runCommand(QStringLiteral("send-to-back")); });
    QMenu* align = layer->addMenu(tr("Align"));
    for (const QString& name :
         {tr("Top Edges"), tr("Vertical Centers"), tr("Bottom Edges"), tr("Left Edges"),
          tr("Horizontal Centers"), tr("Right Edges")})
        makeAction(align, name);
    layer->addSeparator();
    makeAction(layer, tr("Merge Down"), QStringLiteral("Ctrl+E"));
    makeAction(layer, tr("Merge Visible"), QStringLiteral("Ctrl+Shift+E"));
    makeAction(layer, tr("Flatten Image"));
}

}  // namespace pittore::ui
