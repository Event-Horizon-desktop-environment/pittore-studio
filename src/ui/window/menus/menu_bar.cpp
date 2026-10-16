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

namespace {

// A shown QMenu caches action geometry (see refitBrushMenu in
// options_bar.cpp): a menu reshown after its content changed can keep the
// old, shorter box and clip its bottom rows. Recompute from current
// actions on every open, while the menu is still hidden and adjustSize
// takes effect. A no-op when the size is already current.
void refitMenuTree(QMenu* menu) {
    if (!menu) return;
    QObject::connect(menu, &QMenu::aboutToShow, menu,
                     [menu] { menu->adjustSize(); });
    for (QAction* action : menu->actions())
        if (action->menu()) refitMenuTree(action->menu());
}

}  // namespace

void MainWindow::buildMenuBar() {
    buildFileMenu();
    buildEditMenu();
    buildImageMenu();
    buildLayerMenu();
    buildTypeMenu();
    buildSelectMenu();
    buildFilterMenu();
    buildViewMenu();
    buildObjectMenu();
    buildPathMenu();
    buildPluginsMenu();
    buildWindowMenu();
    buildHelpMenu();
    for (QAction* top : menuBar()->actions())
        if (top->menu()) refitMenuTree(top->menu());
}

}  // namespace pittore::ui
