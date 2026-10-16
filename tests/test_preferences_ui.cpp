// test_preferences_ui.cpp
// Smoke test for the Settings dialog, in particular the Machine Learning
// page: it must render a searchable category list (General / Interface /
// Canvas / Cursors / Documents / Export / Performance / Machine Learning /
// Shortcuts / Advanced), list every registered model in the "model to use"
// drop-down (pre-selected to the persisted choice) and add one download/remove
// row per model. Runs headless (QT_QPA_PLATFORM=offscreen).

#include <cstdio>

#include <QApplication>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>

#include "ui/ai_models.h"
#include "ui/app_state.h"
#include "ui/preferences_dialog.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    pittore::ui::AppState state;
    pittore::ui::PreferencesDialog dlg(&state);

    const int models = pittore::ui::allAiModels().size();
    if (models == 0) {
        std::printf("FAIL: no models in the catalogue\n");
        return 1;
    }

    auto* nav = dlg.findChild<QListWidget*>();
    if (!nav) {
        std::printf("FAIL: settings has no category list\n");
        return 1;
    }
    if (nav->count() != 10) {
        std::printf("FAIL: expected 10 categories, got %d\n",
                    nav->count());
        return 1;
    }
    {
        const QStringList want{QStringLiteral("General"), QStringLiteral("Interface"),
                               QStringLiteral("Canvas"), QStringLiteral("Cursors"),
                               QStringLiteral("Documents"), QStringLiteral("Export"),
                               QStringLiteral("Performance"),
                               QStringLiteral("Machine Learning"),
                               QStringLiteral("Shortcuts"), QStringLiteral("Advanced")};
        for (int i = 0; i < want.size(); ++i) {
            if (nav->item(i)->text() != want.at(i)) {
                std::printf("FAIL: category %d is '%s', want '%s'\n", i,
                            nav->item(i)->text().toUtf8().constData(),
                            want.at(i).toUtf8().constData());
                return 1;
            }
        }
    }
    if (!dlg.findChild<QLineEdit*>()) {
        std::printf("FAIL: settings has no search field\n");
        return 1;
    }
    auto* pages = dlg.findChild<QStackedWidget*>();
    if (!pages) {
        std::printf("FAIL: settings has no stacked pages\n");
        return 1;
    }
    if (pages->count() != 10) {
        std::printf("FAIL: expected 10 settings pages, got %d\n",
                    pages->count());
        return 1;
    }

    QWidget* aiTab = pages->widget(7);
    if (!aiTab) {
        std::printf("FAIL: no Machine Learning page\n");
        return 1;
    }
    auto* combo = aiTab->findChild<QComboBox*>();
    if (!combo) {
        std::printf("FAIL: AI tab has no model drop-down\n");
        return 1;
    }
    if (combo->count() != models) {
        std::printf("FAIL: drop-down lists %d models, want %d\n", combo->count(),
                    models);
        return 1;
    }
    if (combo->currentData().toString() != state.settings().bgModel) {
        std::printf("FAIL: drop-down selection '%s' != setting '%s'\n",
                    combo->currentData().toString().toUtf8().constData(),
                    state.settings().bgModel.toUtf8().constData());
        return 1;
    }
    const auto rows = aiTab->findChildren<QPushButton*>();
    if (rows.size() < models) {
        std::printf("FAIL: expected >= %d action buttons on the AI tab, got %zu\n",
                    models, std::size_t(rows.size()));
        return 1;
    }

    std::printf("OK: settings Machine Learning page — %d models listed, %zu action buttons, "
                "selection '%s'\n",
                models, std::size_t(rows.size()),
                combo->currentData().toString().toUtf8().constData());

    // Deep-linking: an initial tab id selects that category; unknown falls
    // back to General.
    {
        pittore::ui::PreferencesDialog linked(&state, nullptr,
                                               QStringLiteral("Shortcuts"));
        auto* linkedNav = linked.findChild<QListWidget*>();
        if (!linkedNav || linkedNav->currentRow() != 8) {
            std::printf("FAIL: deep-link to Shortcuts selected row %d, want 8\n",
                        linkedNav ? linkedNav->currentRow() : -1);
            return 1;
        }
        pittore::ui::PreferencesDialog bogus(&state, nullptr,
                                             QStringLiteral("Nope"));
        auto* bogusNav = bogus.findChild<QListWidget*>();
        if (!bogusNav || bogusNav->currentRow() != 0) {
            std::printf("FAIL: unknown tab should fall back to General\n");
            return 1;
        }
    }

    // Interface hooks: workspace presets appear on the Interface page only
    // when supplied (the plain dialog stays settings-only). The page also
    // owns a font-preview combo now, so match the workspace tooltip.
    {
        auto hasWorkspaceCombo = [](QStackedWidget* pages) {
            for (QComboBox* box :
                 pages->widget(1)->findChildren<QComboBox*>()) {
                if (box->toolTip().contains(QStringLiteral("Panel layout")))
                    return true;
            }
            return false;
        };
        auto* barePages = dlg.findChild<QStackedWidget*>();
        if (hasWorkspaceCombo(barePages)) {
            std::printf("FAIL: Interface page should have no workspace combo without hooks\n");
            return 1;
        }
        // Font preview size combo lives on the Interface page unconditionally.
        bool previewFound = false;
        for (QComboBox* box : barePages->widget(1)->findChildren<QComboBox*>()) {
            if (box->toolTip().contains(QStringLiteral("typeface")) &&
                box->count() == 4)
                previewFound = true;
        }
        if (!previewFound) {
            std::printf("FAIL: Interface page missing font preview combo\n");
            return 1;
        }
        pittore::ui::InterfaceHooks hooks;
        hooks.hasToolbar = true;
        hooks.workspaceNames = QStringList{QStringLiteral("Essentials")};
        hooks.workspaceIds = QStringList{QStringLiteral("Essentials")};
        pittore::ui::PreferencesDialog hooked(&state, nullptr, QStringLiteral("Interface"),
                                               hooks);
        auto* hookedPages = hooked.findChild<QStackedWidget*>();
        if (!hasWorkspaceCombo(hookedPages)) {
            std::printf("FAIL: Interface page missing workspace combo with hooks\n");
            return 1;
        }
    }
    std::printf("OK: deep-linking + Interface hooks\n");

    // Shortcuts filter narrows the table; duplicate shortcuts flag conflicts.
    {
        auto* allPages = dlg.findChild<QStackedWidget*>();
        QWidget* shortcutsTab = allPages->widget(8);
        auto* table = shortcutsTab->findChild<QTableWidget*>(
            QStringLiteral("shortcuts.table"));
        auto* filter = shortcutsTab->findChild<QLineEdit*>(
            QStringLiteral("shortcuts.filter"));
        if (!table || !filter) {
            std::printf("FAIL: Shortcuts tab missing table/filter\n");
            return 1;
        }
        filter->setText(QStringLiteral("undo"));
        int visible = 0;
        for (int r = 0; r < table->rowCount(); ++r)
            if (!table->isRowHidden(r)) ++visible;
        if (visible != 1) {
            std::printf("FAIL: filter 'undo' shows %d rows, want 1\n", visible);
            return 1;
        }
        filter->clear();
        auto* edit0 = qobject_cast<QKeySequenceEdit*>(table->cellWidget(0, 1));
        auto* edit1 = qobject_cast<QKeySequenceEdit*>(table->cellWidget(1, 1));
        if (!edit0 || !edit1) {
            std::printf("FAIL: shortcut edits missing\n");
            return 1;
        }
        edit0->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Q")));
        edit1->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Q")));
        if (!table->item(0, 0)->toolTip().contains(QStringLiteral("onflict")) ||
            !table->item(1, 0)->toolTip().contains(QStringLiteral("onflict"))) {
            std::printf("FAIL: duplicate shortcuts not flagged\n");
            return 1;
        }
        edit1->setKeySequence(QKeySequence());
        if (table->item(0, 0)->toolTip().contains(QStringLiteral("onflict"))) {
            std::printf("FAIL: conflict flag stuck after unbind\n");
            return 1;
        }
        // Fixed canvas keys lose to the event filter: remapping onto one
        // must warn even with no keymap duplicate.
        edit0->setKeySequence(QKeySequence(QStringLiteral("Space")));
        if (!table->item(0, 0)->toolTip().contains(QStringLiteral("eserved"))) {
            std::printf("FAIL: reserved canvas key not flagged\n");
            return 1;
        }
        edit0->setKeySequence(QKeySequence());
    }
    std::printf("OK: shortcut filter + conflicts\n");

    // Canvas page carries the persisted Show toggles.
    {
        auto* allPages = dlg.findChild<QStackedWidget*>();
        bool found = false;
        bool nudge = false;
        bool subdiv = false;
        for (QLabel* label : allPages->widget(2)->findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("Pixel grid")) found = true;
            if (label->text() == QStringLiteral("Nudge distance")) nudge = true;
            if (label->text() == QStringLiteral("Grid subdivisions"))
                subdiv = true;
        }
        if (!found) {
            std::printf("FAIL: Canvas page has no Show section\n");
            return 1;
        }
        if (!nudge) {
            std::printf("FAIL: Canvas page has no Nudge section\n");
            return 1;
        }
        if (!subdiv) {
            std::printf("FAIL: Canvas page has no subdivisions row\n");
            return 1;
        }
        bool undo = false;
        for (QLabel* label : allPages->widget(6)->findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("Undo limit")) {
                undo = true;
                break;
            }
        }
        if (!undo) {
            std::printf("FAIL: Performance page has no History section\n");
            return 1;
        }
        // Issue #7: a build without GPU backends still shows the GPU device
        // combo; it must explain itself rather than sit empty.
        auto* gpuCombo =
            allPages->widget(6)->findChild<QComboBox*>(QStringLiteral("perf.gpuDevice"));
        if (!gpuCombo) {
            std::printf("FAIL: Performance page has no GPU device combo\n");
            return 1;
        }
        if (gpuCombo->count() == 0) {
            std::printf("FAIL: GPU device combo is empty with no explanation\n");
            return 1;
        }
    }
    std::printf("OK: canvas Show section\n");

    // Advanced extras supplied by the host appear as extra path rows.
    {
        pittore::ui::AdvancedHooks advanced;
        advanced.extraNames = QStringList{QStringLiteral("Recovery snapshot")};
        advanced.extraPaths = QStringList{QStringLiteral("/tmp/recovery-check.ifp")};
        pittore::ui::PreferencesDialog extra(&state, nullptr, QStringLiteral("Advanced"),
                                              {}, {}, advanced);
        auto* extraPages = extra.findChild<QStackedWidget*>();
        bool found = false;
        for (QLabel* label : extraPages->widget(9)->findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("recovery-check.ifp"))) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::printf("FAIL: Advanced page missing host-supplied path row\n");
            return 1;
        }
    }
    std::printf("OK: advanced host paths\n");
    return 0;
}