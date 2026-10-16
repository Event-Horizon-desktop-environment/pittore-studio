// test_window_min_size.cpp — issue #2: the main window must fit a
// 1280x720 screen un-maximized. The floor is emergent (dock minimums,
// the options bar's wrapped row, the tool strip's columns), so this
// drives the real MainWindow headless and pins both the accepted 720p
// size and the layout minimum underneath it.
#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QWidget>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/main_window.h"
#include "ui/options_bar.h"
#include "ui/persona/persona_manager.h"
#include "ui/tools_panel.h"

using namespace pittore::ui;

int main(int argc, char** argv) {
    const QString scratch =
        QDir::tempPath() + QStringLiteral("/pittore-test-min-window");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);
    qputenv("XDG_CONFIG_HOME", scratch.toLocal8Bit());
    QApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-min-window-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    AppState state;
    MainWindow win(&state);
    // Shown but never pumped: resize() lays out synchronously, and the
    // deferred Start page is a modal exec() no headless test wants.
    win.show();
    win.resize(1280, 720);

    // The window accepts the 720p target size instead of clamping up.
    CHECK(win.width() == 1280);
    CHECK(win.height() == 720);

    // And the layout minimum itself sits under the target, so a squeezed
    // arrangement of the same chrome still fits a 720p screen.
    CHECK(win.minimumSize().width() <= 1280);
    CHECK(win.minimumSize().height() <= 720);

    // The Vector persona adds its own panels; none may floor the window
    // above the Pixel arrangement. Docks show synchronously, so the floor
    // and the resize land without pumping events.
    if (PersonaManager* personas = win.findChild<PersonaManager*>()) {
        const QSize pixelFloor = win.minimumSize();
        personas->setPersona(Persona::Vector);
        CHECK(win.minimumSize().width() <= 1280);
        CHECK(win.minimumSize().height() <= pixelFloor.height());
        win.resize(1280, 720);
        CHECK(win.width() == 1280);
        CHECK(win.height() == 720);
    } else {
        CHECK(false);
    }

    // The two fixes the floor is made of: the tool strip stacked columns
    // instead of growing past the window, and the options bar wrapped
    // its crowded row instead of pinning the width.
    if (ToolsPanel* panel = win.findChild<ToolsPanel*>(QStringLiteral("toolsPanel")))
        CHECK(panel->twoColumn());
    else
        CHECK(false);
    if (OptionsBar* bar = win.findChild<OptionsBar*>(QStringLiteral("optionsBar")))
        CHECK(bar->height() <= 100);
    else
        CHECK(false);

    if (pittore_test::failures() == 0)
        std::printf("test_window_min_size: all checks passed\n");
    else
        std::printf("test_window_min_size: %d failures\n",
                    pittore_test::failures());
    return pittore_test::failures() ? 1 : 0;
}
