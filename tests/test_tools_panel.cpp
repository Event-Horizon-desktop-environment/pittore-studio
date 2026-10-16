// test_tools_panel.cpp — tool strip contents per persona, grid cell
// integrity (no overlapping dividers), plus Krita-style auto columns:
// double up when a single column no longer fits, relax back when it does,
// explicit choices stick. Runs headless (QT_QPA_PLATFORM=offscreen).
#include <cstdio>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QGridLayout>
#include <QImage>
#include <QLayoutItem>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/persona/persona.h"
#include "ui/persona/persona_manager.h"
#include "ui/tool_registry.h"
#include "ui/tools_panel.h"

using namespace pittore::ui;

namespace {

// Deleted-then-rebuilt generations linger until the deferred deletes are
// delivered: pump hard so counts only ever see the live strip.
void pump() {
    for (int i = 0; i < 3; ++i) qApp->processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qApp->processEvents();
}

int stripButtons(ToolsPanel* panel) {
    int n = 0;
    for (QToolButton* b : panel->findChildren<QToolButton*>())
        if (b->property("toolId").isValid() && b->isVisibleTo(panel)) ++n;
    return n;
}

// Every grid item owns disjoint cells: a section divider must never land on
// a button cell (the two-column white-lines defect).
bool gridCellsClean(ToolsPanel* panel) {
    QGridLayout* grid = panel->findChild<QGridLayout*>();
    if (!grid) return false;
    struct Cell {
        int r, c, rs, cs;
    };
    std::vector<Cell> cells;
    for (int i = 0; i < grid->count(); ++i) {
        int r = 0, c = 0, rs = 0, cs = 0;
        grid->getItemPosition(i, &r, &c, &rs, &cs);
        cells.push_back({r, c, rs, cs});
    }
    for (size_t i = 0; i < cells.size(); ++i)
        for (size_t j = i + 1; j < cells.size(); ++j) {
            const Cell& a = cells[i];
            const Cell& b = cells[j];
            if (a.r < b.r + b.rs && b.r < a.r + a.rs && a.c < b.c + b.cs &&
                b.c < a.c + a.cs)
                return false;
        }
    return !cells.empty();
}

}  // namespace

int main(int argc, char** argv) {
    const QString scratch =
        QDir::tempPath() + QStringLiteral("/pittore-test-tools-panel");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);
    qputenv("XDG_CONFIG_HOME", scratch.toLocal8Bit());
    QApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-tools-panel-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    AppState state;
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new ToolsPanel(&state);
    layout->addWidget(panel);
    window.resize(400, 2000);
    window.show();
    pump();

    // Full strip with no persona filtering: one button per group.
    CHECK(stripButtons(panel) == (int)allGroups().size());
    CHECK(gridCellsClean(panel));

    // Each persona shows exactly its designed leader set.
    PersonaManager personas(&state, panel);
    personas.setPersona(Persona::Draw);
    pump();
    CHECK(stripButtons(panel) == (int)drawVisibleLeaders().size());
    CHECK(gridCellsClean(panel));
    personas.setPersona(Persona::Color);
    pump();
    CHECK(stripButtons(panel) == (int)colorVisibleLeaders().size());
    CHECK(gridCellsClean(panel));
    personas.setPersona(Persona::Vector);
    pump();
    CHECK(stripButtons(panel) == (int)vectorVisibleLeaders().size());
    CHECK(gridCellsClean(panel));

    // Pixel first-visit on a fresh strip applies the designed Pixel set
    // (revisits restore cached customs instead, by design).
    {
        QWidget window2;
        auto* layout2 = new QVBoxLayout(&window2);
        auto* panel2 = new ToolsPanel(&state);
        layout2->addWidget(panel2);
        window2.resize(400, 2000);
        window2.show();
        pump();
        PersonaManager personas2(&state, panel2);
        personas2.setPersona(Persona::Vector);
        pump();
        personas2.setPersona(Persona::Pixel);
        pump();
        CHECK(stripButtons(panel2) ==
              (int)allGroups().size() - (int)pixelHiddenLeaders().size());
        CHECK(gridCellsClean(panel2));
    }

    // Full set for the auto-column checks below.
    panel->setHiddenTools({});
    pump();

    // Auto columns on the full set: short room doubles up, tall room
    // relaxes to one column, and an explicit choice sticks either way.
    // Row gaps stay at base spacing: the strip never spreads to chase
    // leftover room.
    window.resize(400, 600);
    pump();
    CHECK(panel->twoColumn());
    CHECK(gridCellsClean(panel));
    window.resize(400, 2200);
    pump();
    CHECK(!panel->twoColumn());
    CHECK(gridCellsClean(panel));
    {
        QGridLayout* grid = panel->findChild<QGridLayout*>();
        CHECK(grid && grid->spacing() == 4);
    }
    panel->setTwoColumn(false);
    window.resize(400, 600);
    pump();
    CHECK(!panel->twoColumn());
    panel->setTwoColumn(true);
    window.resize(400, 2200);
    pump();
    CHECK(panel->twoColumn());

    // TEMPORARY visual dump for manual inspection.
    {
        personas.setPersona(Persona::Vector);
        window.resize(400, 700);
        pump();
        QImage img(panel->size(), QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        panel->render(&img);
        img.save(QStringLiteral("/tmp/opencode/strip_vector2col.png"));
        std::printf("saved dump %dx%d twoColumn=%d\n", img.width(), img.height(),
                    (int)panel->twoColumn());
    }

    if (pittore_test::failures() == 0)
        std::printf("test_tools_panel: all checks passed\n");
    else
        std::printf("test_tools_panel: %d failures\n",
                    pittore_test::failures());
    return pittore_test::failures() ? 1 : 0;
}
