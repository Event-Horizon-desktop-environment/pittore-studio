// Scratch probe: open the Disney .af through the real MainWindow, then
// simulate selecting the second document tab and report what the app
// binds to. Diagnostic only; registered temporarily in tests/meson.build.
#include <cstdio>

#include <QApplication>
#include <QTabBar>

#include "test_util.h"
#include "ui/app_state.h"
#include "ui/main_window.h"

using namespace pittore::ui;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 2) return 2;
    AppState state;
    MainWindow win(&state);
    win.show();

    const QString path = QString::fromLocal8Bit(argv[1]);
    // MainWindow's open entry is private; the state exposes the same route
    // (openImageFile) that the File > Open command drives.
    QString err2;
    const bool opened = state.openImageFile(path, &err2);
    std::printf("open=%d %s\ndocs=%d active=%d\n", opened,
                qPrintable(err2), state.documents().size(),
                state.activeDocumentIndex());
    for (int i = 0; i < state.documents().size(); ++i)
        std::printf("  doc %d \"%s\" layers=%d\n", i,
                    qPrintable(state.documents()[i]->title),
                    int(state.documents()[i]->layers.size()));

    QTabBar* tabs = win.findChild<QTabBar*>(QStringLiteral("documentTabs"));
    if (!tabs) {
        std::printf("no document tab bar found\n");
        return 3;
    }
    std::printf("tabs count=%d current=%d\n", tabs->count(), tabs->currentIndex());
    for (int i = 0; i < tabs->count(); ++i)
        std::printf("  tab %d \"%s\"\n", i, qPrintable(tabs->tabText(i)));

    tabs->setCurrentIndex(1);
    app.processEvents();
    std::printf("after click index1: activeDocIndex=%d title=\"%s\" zoom=%.3f\n",
                state.activeDocumentIndex(),
                qPrintable(state.activeDocument()->title),
                state.activeDocument()->zoom);
    return 0;
}
