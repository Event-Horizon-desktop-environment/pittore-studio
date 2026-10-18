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

// Right-rail top tab group: document structure. Everything else docks into
// the bottom group. Layers is added first so it leads the top tabs, always.
bool inTopGroup(const QString& id) {
    return id == QLatin1String("layers") || id == QLatin1String("channels") ||
           id == QLatin1String("paths") || id == QLatin1String("properties");
}

}  // namespace


// ---------------------------------------------------------------------------
// C + E: docks
// ---------------------------------------------------------------------------

void MainWindow::buildDocks() {
    tools_ = new ToolsPanel(state_, this);
    connect(tools_, &ToolsPanel::editToolbarRequested, this, [this] { editToolbarDialog(); });
    connect(tools_, &ToolsPanel::screenModeCycleRequested, this,
            [this] { state_->cycleScreenMode(); });

    toolsDock_ = new QDockWidget(tr("Tools"), this);
    toolsDock_->setObjectName(QStringLiteral("dock.tools"));
    toolsDock_->setWidget(tools_);
    toolsDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    toolsDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    toolsDock_->setTitleBarWidget(new QWidget(toolsDock_));  // PS has no tools title bar
    addDockWidget(Qt::LeftDockWidgetArea, toolsDock_);

    // The right rail is two vertically split tab groups: a top group
    // led by Layers, and everything else below. Order matters: the split runs
    // while both anchors are still singletons, because splitDockWidget rips
    // its second dock out of whatever tab group it sits in. Members tabify
    // onto the anchors after the split, which never disturbs it.
    topAnchor_ = bottomAnchor_ = nullptr;
    railGrouped_.clear();

    for (const PanelInfo& info : allPanels()) {
        if (!info.defaultVisible || info.floating) continue;
        QDockWidget* dock = dockFor(info.id, true);
        if (!dock) continue;
        const bool top = inTopGroup(info.id);
        if (!groupAnchor(top)) {
            addDockWidget(Qt::RightDockWidgetArea, dock);
            setGroupAnchor(top, dock);
        }
    }
    if (topAnchor_ && bottomAnchor_ && topAnchor_ != bottomAnchor_)
        splitDockWidget(topAnchor_, bottomAnchor_, Qt::Vertical);

    for (const PanelInfo& info : allPanels()) {
        if (!info.defaultVisible || info.floating) continue;
        QDockWidget* dock = dockFor(info.id, false);
        if (!dock) continue;
        QDockWidget* anchor = groupAnchor(inTopGroup(info.id));
        if (anchor && anchor != dock) tabifyDockWidget(anchor, dock);
        railGrouped_.insert(info.id);
    }
    // Layers leads the top group, always: leftmost tab, surfaced.
    // History leads the bottom group the same way.
    if (QDockWidget* layers = dockFor(QStringLiteral("layers"), false)) {
        leadGroupWith(true, layers);
        layers->show();
        layers->raise();
    } else if (topAnchor_) {
        topAnchor_->raise();
    }
    if (QDockWidget* history = dockFor(QStringLiteral("history"), false)) {
        history->show();
        history->raise();
    }

    resizeDocks(docks_.contains(QStringLiteral("layers"))
                    ? QList<QDockWidget*>{docks_.value(QStringLiteral("layers"))}
                    : QList<QDockWidget*>{},
                {360}, Qt::Horizontal);
    fixDockTabBars();
}


// Dock tab bars must never clip names to "Chan...": full text with scroll
// arrows instead of Qt's default elide-when-narrow. Runs after every layout
// change that can (re)create a tab group; the document tab bar keeps its own
// elide policy and is left alone.
void MainWindow::fixDockTabBars() {
    for (QTabBar* bar : findChildren<QTabBar*>()) {
        if (bar == documentTabs_) continue;
        bar->setElideMode(Qt::ElideNone);
        bar->setUsesScrollButtons(true);
    }
}


void MainWindow::raiseDefaultTabs() {
    // Preset switches: same defaults, but hidden or torn-out docks stay
    // exactly as the user left them (a preset must never resurrect a
    // closed panel or yank a floating window).
    if (QDockWidget* layers = dockFor(QStringLiteral("layers"), false)) {
        if (layers->isVisible() && !layers->isFloating()) {
            leadGroupWith(true, layers);
            layers->show();
            layers->raise();
        }
    }
    if (QDockWidget* history = dockFor(QStringLiteral("history"), false)) {
        if (history->isVisible() && !history->isFloating()) {
            history->show();
            history->raise();
        }
    }
}


QDockWidget* MainWindow::dockFor(const QString& id, bool create) {
    if (docks_.contains(id) && docks_.value(id)) return docks_.value(id);
    if (!create) return nullptr;

    const PanelInfo* info = panelInfo(id);
    if (!info) return nullptr;

    auto* dock = new QDockWidget(info->title, this);
    dock->setObjectName(QStringLiteral("dock.") + id);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                      QDockWidget::DockWidgetClosable);
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setWidget(info->factory(state_, dock));
    dock->setMinimumWidth(230);

    connect(dock, &QDockWidget::visibilityChanged, this, [this, id](bool visible) {
        if (QAction* action = panelActions_.value(id)) {
            QSignalBlocker blocker(action);
            action->setChecked(visible);
        }
    });

    docks_.insert(id, dock);
    return dock;
}


void MainWindow::showPanel(const QString& id, bool show) {
    // Keep the Window-menu checkmark in step whether the panel is opened from
    // the menu, the options bar or a workspace preset.
    if (QAction* action = panelActions_.value(id)) {
        QSignalBlocker blocker(action);
        action->setChecked(show);
    }
    const PanelInfo* info = panelInfo(id);
    if (info && info->floating) {
        if (!show) {
            if (QWidget* window = floatingPanel(id, false)) window->hide();
            return;
        }
        QWidget* window = floatingPanel(id, true);
        if (!window) return;
        window->show();
        window->raise();
        window->activateWindow();
        return;
    }
    if (!show) {
        if (QDockWidget* dock = dockFor(id, false)) dock->hide();
        return;
    }
    QDockWidget* dock = dockFor(id, true);
    if (!dock) return;
    // Left-docked panels (if any) bypass the right-rail tab groups. They must
    // form a full-height column immediately right of the tools rail (Affinity's
    // tools -> Pages -> canvas order); a bare addDockWidget stacks them
    // *beneath* the tools rail instead, which is not the layout anyone wants.
    if (info && info->defaultDock == QLatin1String("left")) {
        if (!dock->parentWidget() || dockWidgetArea(dock) == Qt::NoDockWidgetArea) {
            if (toolsDock_ && dock != toolsDock_ &&
                dockWidgetArea(toolsDock_) == Qt::LeftDockWidgetArea)
                splitDockWidget(toolsDock_, dock, Qt::Horizontal);
            else
                addDockWidget(Qt::LeftDockWidgetArea, dock);
        }
        dock->show();
        dock->raise();
        return;
    }
    // A panel the user tore out of the rail stays where they put it: just
    // surface it instead of dragging it back into a tab group.
    if (dock->isFloating()) {
        dock->show();
        dock->raise();
        return;
    }
    // Join the panel's rail group. The buildDocks split stands untouched:
    // splitDockWidget rips its second dock out of its tab group, so layout
    // calls it exactly once (two singletons) and never again. Tabifying a
    // joining dock onto its anchor is always safe and never moves the anchor.
    const bool top = inTopGroup(id);
    QDockWidget* anchor = groupAnchor(top);
    if (dockWidgetArea(dock) == Qt::NoDockWidgetArea) {
        if (anchor && anchor != dock) {
            tabifyDockWidget(anchor, dock);
            railGrouped_.insert(id);
        } else {
            // The group is empty (everything floated): stack for now and
            // become the anchor; the next shown member tabifies onto us.
            addDockWidget(Qt::RightDockWidgetArea, dock);
            setGroupAnchor(top, dock);
        }
    } else if (anchor && anchor != dock && !railGrouped_.contains(id)) {
        // Already docked (re-shown after hide — hiding keeps tab
        // membership): rejoin only a dock that was never grouped. Blindly
        // re-tabifying would shove its tab to the end of the bar.
        tabifyDockWidget(anchor, dock);
        railGrouped_.insert(id);
    }
    dock->show();
    dock->raise();
    // Layers leads the top group, always: re-assert it leftmost so a
    // close/reopen cycle can't strand it at the end of the tab bar.
    if (id == QLatin1String("layers")) leadGroupWith(top, dock);
    // A new tab group may have been born above: keep full tab names.
    fixDockTabBars();
}


QDockWidget* MainWindow::groupAnchor(bool top) {
    QDockWidget* anchor = top ? topAnchor_ : bottomAnchor_;
    if (anchor && !anchor->isFloating() &&
        dockWidgetArea(anchor) != Qt::NoDockWidgetArea)
        return anchor;
    // Anchor lost (never set, or the user undocked it): fall back to the
    // first still-docked, non-floating group member so the group survives.
    for (const PanelInfo& info : allPanels()) {
        if (info.floating || inTopGroup(info.id) != top) continue;
        QDockWidget* w = dockFor(info.id, false);
        if (w && !w->isFloating() &&
            dockWidgetArea(w) != Qt::NoDockWidgetArea) {
            setGroupAnchor(top, w);
            return w;
        }
    }
    return nullptr;
}

void MainWindow::setGroupAnchor(bool top, QDockWidget* anchor) {
    if (top)
        topAnchor_ = anchor;
    else
        bottomAnchor_ = anchor;
}

void MainWindow::leadGroupWith(bool top, QDockWidget* first) {
    // Re-tabify the group's other docked members onto `first` in registry
    // order: tabification appends, so the group ends up ordered with `first`
    // leftmost and everyone else in their stable slots, hidden or shown.
    for (const PanelInfo& info : allPanels()) {
        if (info.floating || inTopGroup(info.id) != top) continue;
        QDockWidget* w = dockFor(info.id, false);
        if (!w || w == first || w->isFloating() ||
            dockWidgetArea(w) == Qt::NoDockWidgetArea)
            continue;
        tabifyDockWidget(first, w);
    }
}


QWidget* MainWindow::floatingPanel(const QString& id, bool create) {
    if (QWidget* existing = floatingPanels_.value(id)) return existing;
    if (!create) return nullptr;
    const PanelInfo* info = panelInfo(id);
    if (!info) return nullptr;
    QWidget* content = info->factory(state_, nullptr);
    if (!content) return nullptr;
    auto* window = new FloatingPanelWindow(info->title, content, this);
    window->setObjectName(QStringLiteral("panel.") + id);
    window->onClose = [this, id] {
        if (QAction* action = panelActions_.value(id)) {
            QSignalBlocker blocker(action);
            action->setChecked(false);
        }
        floatingPanels_.remove(id);
    };
    floatingPanels_.insert(id, window);
    return window;
}


bool MainWindow::panelVisible(const QString& id) const {
    const PanelInfo* info = panelInfo(id);
    if (info && info->floating) {
        const QPointer<QWidget> window = floatingPanels_.value(id);
        return window && window->isVisible();
    }
    const QPointer<QDockWidget> dock = docks_.value(id);
    return dock && dock->isVisible();
}

}  // namespace pittore::ui
