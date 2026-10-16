// test_chrome_state.cpp — the no-document disabled contract + dock tab
// overflow policy: the options bar and the contextual task bar dim when no
// document is open and re-enable on open/close, and dock tab bars keep full
// names with scroll buttons instead of eliding to "Chan...".
// Runs headless (QT_QPA_PLATFORM=offscreen).
#include <cstdio>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/brush_popup_scale.h"
#include "ui/contextual_task_bar.h"
#include "ui/selection_mask.h"
#include "ui/dpi_pixmap.h"
#include "ui/main_window.h"
#include "ui/options_bar.h"

using namespace pittore::ui;

int main(int argc, char** argv) {
    // Hermetic: the Enhance Edges check must exercise the classical fallback,
    // not the developer's multi-GB model cache (see test_refine_dialog).
    const QString noModels =
        QDir::tempPath() + QStringLiteral("/pittore-test-no-models-chrome");
    QDir(noModels).removeRecursively();
    QDir().mkpath(noModels);
    qputenv("PITTORE_MODELS_DIR", noModels.toLocal8Bit());
    QApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-chrome-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    AppState state;
    QWidget window;
    auto* options = new OptionsBar(&state, &window);
    auto* taskBar = new ContextualTaskBar(&state, &window);
    window.show();
    app.processEvents();

    // No document: both strips dim.
    CHECK(!options->isEnabledTo(&window));
    CHECK(!taskBar->isEnabledTo(&window));

    // Opening a document re-enables both.
    DocumentItem* doc = state.addDocument(QStringLiteral("c"), QSize(64, 48), 72);
    CHECK(doc != nullptr);
    app.processEvents();
    CHECK(options->isEnabledTo(&window));
    CHECK(taskBar->isEnabledTo(&window));

    // Closing the last document dims them again.
    state.closeDocument(0);
    app.processEvents();
    CHECK(!options->isEnabledTo(&window));
    CHECK(!taskBar->isEnabledTo(&window));

    // Dock tab bars: full names + scroll buttons, never elided text.
    // (A document stays open so the startup page never goes modal.)
    DocumentItem* doc2 = state.addDocument(QStringLiteral("c2"), QSize(64, 48), 72);
    CHECK(doc2 != nullptr);
    MainWindow mainWin(&state);
    mainWin.show();
    app.processEvents();
    bool sawDockBar = false;
    for (QTabBar* bar : mainWin.findChildren<QTabBar*>()) {
        if (bar->objectName() == QStringLiteral("documentTabs")) continue;
        sawDockBar = true;
        CHECK(bar->elideMode() == Qt::ElideNone);
        CHECK(bar->usesScrollButtons());
    }
    CHECK(sawDockBar);

    // Brush preset popup: with every section expanded, no two controls may
    // overlap and nothing may hang outside the panel (the squash + cutoff
    // regressions). Geometry is checked in panel space, headless.
    state.setActiveTool(ToolId::Brush);
    app.processEvents();
    QToolButton* presetButton = nullptr;
    for (QToolButton* b : options->findChildren<QToolButton*>()) {
        if (b->toolTip().contains(QStringLiteral("Brush preset"))) {
            presetButton = b;
            break;
        }
    }
    CHECK(presetButton != nullptr);
    if (presetButton) {
        // NOTE: never showMenu() here — it nests an event loop that never
        // exits offscreen. The menu object exists from construction, so the
        // layout is inspected (and grabbed) unshown.
        QMenu* popup = presetButton->menu();
        CHECK(popup != nullptr);
        if (popup) {
            for (QToolButton* h : popup->findChildren<QToolButton*>()) {
                const QString t = h->text();
                if (h->isCheckable() &&
                    (t == QStringLiteral("Brush") || t == QStringLiteral("Tip") ||
                     t == QStringLiteral("Dynamics") ||
                     t == QStringLiteral("Setup")))
                    h->setChecked(true);
            }
            app.processEvents();
            QWidget* panel = nullptr;
            int bestLabels = -1;
            for (QWidget* w : popup->findChildren<QWidget*>()) {
                if (w->parentWidget() != popup) continue;
                const int n = w->findChildren<QLabel*>().size() +
                              w->findChildren<QSlider*>().size();
                if (n > bestLabels) {
                    bestLabels = n;
                    panel = w;
                }
            }
            CHECK(panel != nullptr);
            if (panel) {
                // Never open the real popup (showMenu nests a loop
                // offscreen): detach the panel into its own hidden
                // top-level window so style polish + layout run exactly
                // as they do on screen, then measure that.
                panel->setParent(nullptr);
                panel->show();
                app.processEvents();
                if (panel->layout()) panel->layout()->activate();
                panel->adjustSize();
                app.processEvents();
                if (panel->layout()) panel->layout()->activate();
                const QRect bounds(QPoint(0, 0), panel->size());
                QList<QRect> seen;
                auto rectInPanel = [&](QWidget* w) {
                    return QRect(w->mapTo(panel, QPoint(0, 0)), w->size());
                };
                auto checkWidget = [&](QWidget* w) {
                    if (!w->isVisibleTo(panel) || w->width() < 2 ||
                        w->height() < 2)
                        return;
                    const QRect r = rectInPanel(w);
                    CHECK(bounds.contains(r));
                    for (const QRect& s : seen) {
                        // Siblings share edges; only real area overlap counts.
                        const QRect inter = r.intersected(s);
                        CHECK(inter.width() < 2 && inter.height() < 2);
                    }
                    seen.append(r);
                };
                for (QLabel* w : panel->findChildren<QLabel*>()) checkWidget(w);
                for (QSlider* w : panel->findChildren<QSlider*>()) checkWidget(w);
                for (QSpinBox* w : panel->findChildren<QSpinBox*>()) checkWidget(w);
                for (QComboBox* w : panel->findChildren<QComboBox*>()) checkWidget(w);
                for (QCheckBox* w : panel->findChildren<QCheckBox*>()) checkWidget(w);
                for (QPushButton* w : panel->findChildren<QPushButton*>())
                    checkWidget(w);
                for (QToolButton* w : panel->findChildren<QToolButton*>())
                    checkWidget(w);
                CHECK(!seen.isEmpty());
                std::printf("  popup controls=%lld size=%dx%d\n",
                            static_cast<long long>(seen.size()),
                            bounds.width(), bounds.height());
                panel->grab().save(QDir::tempPath() + "/brush_popup.png");
                // Gallery cells: items snap to the fixed grid pitch
                // (uniform columns), names fully visible, none clipped.
                if (QListWidget* gal = panel->findChild<QListWidget*>()) {
                    CHECK(gal->viewMode() == QListView::IconMode);
                    CHECK(gal->gridSize() == QSize(96, 90));
                    QSet<int> cols;
                    int cells = 0;
                    for (int i = 0; i < gal->count(); ++i) {
                        const QRect r = gal->visualRect(
                            gal->model()->index(i, 0));
                        if (!r.isValid()) continue;
                        cols.insert(r.x());
                        ++cells;
                    }
                    CHECK(cells == gal->count());
                    // One x per grid column: 3 columns ⇒ ≤3 distinct lefts.
                    CHECK(cols.size() <= 3);
                    std::printf("  gallery cells=%d cols=%lld\n", cells,
                                static_cast<long long>(cols.size()));
                }
            }
        }
    }

    // Brush popup fit-to-screen scale (pure geometry, ui/brush_popup_scale.h):
    // big screens stay exactly 1.0, small screens shrink into [0.6, 1.0).
    {
        // Reference popup (~1090x750 with chrome) on a 1080p screen: fits.
        CHECK(brushPopupScaleFor(1090.0, 750.0, 1920.0, 1040.0) == 1.0);
        // Same popup on 1440p: fits with room.
        CHECK(brushPopupScaleFor(1090.0, 750.0, 2560.0, 1400.0) == 1.0);
        // 720p laptop (1280x720, avail ~1280x690): must shrink but stay up.
        const double s720 =
            brushPopupScaleFor(1090.0, 750.0, 1280.0, 690.0);
        CHECK(s720 < 1.0 && s720 >= kMinPopupScale);
        // Tiny screen: floors, never below.
        CHECK(brushPopupScaleFor(1090.0, 750.0, 400.0, 300.0) ==
              kMinPopupScale);
        // Degenerate inputs: no scaling, never NaN.
        CHECK(brushPopupScaleFor(0.0, 0.0, 1280.0, 720.0) == 1.0);
        CHECK(brushPopupScaleFor(1090.0, 750.0, 0.0, 0.0) == 1.0);
        std::printf("  popup scale 720p=%.3f floor=%.2f\n", s720,
                    kMinPopupScale);
    }

    // DPI pixmap bridge (ui/dpi_pixmap.h): logical size preserved, the
    // widget's ratio stamped so 150/200% displays get full-res pixels.
    {
        QWidget w;
        QImage img(56, 56, QImage::Format_ARGB32);
        img.fill(Qt::white);
        const QPixmap pm = pixmapForWidget(img, &w);
        CHECK(pm.size() == QSize(56, 56));
        CHECK(pm.devicePixelRatio() == w.devicePixelRatioF());
        const QPixmap pm2 = pixmapForDpr(img, 2.0);
        CHECK(pm2.size() == QSize(56, 56));
        CHECK(pm2.devicePixelRatio() == 2.0);
        std::printf("  dpi pixmap logical=56x56 ratio=%.1f\n",
                    w.devicePixelRatioF());
    }

    // Enhance Edges: on the Selection task bar (the screenshot strip) and
    // runnable headless — classical fallback when no AI model is installed.
    state.setTaskContext(TaskContext::Selection);
    app.processEvents();
    {
        bool sawEnhance = false;
        for (QPushButton* b : taskBar->findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Enhance Edges")) {
                sawEnhance = true;
                break;
            }
        }
        CHECK(sawEnhance);
    }
    {
        DocumentItem* doc =
            state.addDocument(QStringLiteral("enh"), QSize(64, 48), 72);
        CHECK(doc != nullptr);
        QImage split(64, 48, QImage::Format_ARGB32_Premultiplied);
        split.fill(0xFFFFFFFF);
        state.placeImageLayer(split, QStringLiteral("w"), QPointF(32, 24), 1.0);
        state.setSelection(QRectF(4, 4, 14, 40), false);
        doc->rebuildComposite();
        app.processEvents();
        // Click the Enhance Edges button on the window's own task bar:
        // standalone taskBar is not wired to runCommand, but the canvas
        // bar is (document_area connects commandTriggered).
        QPushButton* enhanceBtn = nullptr;
        for (QPushButton* b : mainWin.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Enhance Edges")) {
                enhanceBtn = b;
                break;
            }
        }
        CHECK(enhanceBtn != nullptr);
        if (enhanceBtn) enhanceBtn->click();
        app.processEvents();
        CHECK(state.activeDocument()->selectionIsMask);
        CHECK(!selectionMaskBbox(state.activeDocument()->selectionMask).isEmpty());
    }

    // Brush-size keys repeat while held (issue #8): a first [ press and
    // a held (auto-repeat) [ press both shrink every brush tool, ] grows.
    state.setActiveTool(ToolId::Brush);
    state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 100.0);
    app.processEvents();
    {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_BracketLeft,
                        Qt::NoModifier, QStringLiteral("["));
        QApplication::sendEvent(&mainWin, &press);
        app.processEvents();
        const double s1 = state.option(ToolId::Brush, QStringLiteral("brush_size")).toDouble();
        CHECK(qAbs(s1 - 96.0) < 1e-9);
        // Held key: Qt delivers KeyPress with isAutoRepeat() set; the old
        // filter dropped those before the brush handler ever saw them.
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_BracketLeft,
                         Qt::NoModifier, QStringLiteral("["), true);
        CHECK(repeat.isAutoRepeat());
        QApplication::sendEvent(&mainWin, &repeat);
        app.processEvents();
        const double s2 = state.option(ToolId::Brush, QStringLiteral("brush_size")).toDouble();
        CHECK(qAbs(s2 - 96.0 * 0.96) < 1e-9);
        QKeyEvent bigger(QEvent::KeyPress, Qt::Key_BracketRight,
                         Qt::NoModifier, QStringLiteral("]"), true);
        QApplication::sendEvent(&mainWin, &bigger);
        app.processEvents();
        const double s3 = state.option(ToolId::Brush, QStringLiteral("brush_size")).toDouble();
        CHECK(s3 > s2);
    }

    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return pittore_test::failures() == 0 ? 0 : 1;
}
