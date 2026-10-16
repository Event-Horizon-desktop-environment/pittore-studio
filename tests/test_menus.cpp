// test_menus.cpp — every menubar menu fits its content however it opens.
// Guards issue #5 (hover-switch cropping bottom rows): a shown QMenu caches
// action geometry, so each top-level menu is opened by direct popup and by
// menubar hover-switch and must contain its last action either way.
// Runs headless (QT_QPA_PLATFORM=offscreen).
#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QMenuBar>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/main_window.h"

using namespace pittore::ui;

namespace {

// True when every action rect sits inside the menu box.
bool menuFits(QMenu* menu) {
    for (QAction* action : menu->actions()) {
        if (action->isSeparator()) continue;
        if (!menu->actionGeometry(action).isEmpty() &&
            !menu->rect().contains(menu->actionGeometry(action)))
            return false;
    }
    return true;
}

void pump(int rounds = 3) {
    for (int i = 0; i < rounds; ++i) qApp->processEvents();
}

}  // namespace

int main(int argc, char** argv) {
    const QString scratch =
        QDir::tempPath() + QStringLiteral("/pittore-test-menus");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);
    qputenv("XDG_CONFIG_HOME", scratch.toLocal8Bit());
    QApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-menus-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    AppState state;
    // A document stays open so the startup page never goes modal.
    DocumentItem* doc = state.addDocument(QStringLiteral("m"), QSize(64, 48), 72);
    CHECK(doc != nullptr);
    MainWindow mainWin(&state);
    mainWin.show();
    pump();

    QMenuBar* bar = mainWin.menuBar();
    QList<QAction*> tops;
    for (QAction* action : bar->actions())
        if (action->menu()) tops.append(action);
    CHECK(!tops.isEmpty());

    // Direct popup each menu: everything must fit.
    for (QAction* top : tops) {
        QMenu* menu = top->menu();
        menu->popup(bar->mapToGlobal(QPoint(60, bar->height())));
        pump();
        CHECK(menuFits(menu));
        menu->hide();
        pump();
    }

    // Hover-switch across the whole bar: open each menu from its
    // predecessor exactly like the menubar hover machinery does, then
    // check the fit. This is the issue #5 path.
    for (QAction* top : tops) {
        QMenu* menu = top->menu();
        bar->setActiveAction(top);
        pump();
        CHECK(menu->isVisible());
        CHECK(menuFits(menu));
    }
    bar->setActiveAction(nullptr);
    pump();

    if (pittore_test::failures() == 0)
        std::printf("test_menus: all checks passed\n");
    else
        std::printf("test_menus: %d failures\n", pittore_test::failures());
    return pittore_test::failures() ? 1 : 0;
}
