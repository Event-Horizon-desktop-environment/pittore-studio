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


// ---------------------------------------------------------------------------
// R21: keyboard grammar
// ---------------------------------------------------------------------------

void MainWindow::installShortcuts() {
    qApp->installEventFilter(this);
    keymapCache_ = loadKeymapEntries();

    auto addShortcut = [this](const QString& sequence, std::function<void()> handler) {
        auto* action = new QAction(this);
        action->setShortcut(QKeySequence(sequence));
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, [handler] { handler(); });
        addAction(action);
    };
    // Keymap-driven carriers: the shortcut comes from keymap.json (with the
    // given fallback) and refreshCarrierShortcuts() re-reads it whenever
    // Settings > Shortcuts is applied, so no restart is needed.
    auto addKeyedShortcut = [this](const QString& id, std::function<void()> handler) {
        auto* action = new QAction(this);
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, [handler] { handler(); });
        addAction(action);
        carrierShortcuts_.append({id, action});
    };

    addKeyedShortcut(QStringLiteral("toggle-panels"), [this] {
        state_->setChromeVisibility(state_->chromeVisibility() == ChromeVisibility::All
                                        ? ChromeVisibility::AllHidden
                                        : ChromeVisibility::All);
    });
    addShortcut(QStringLiteral("Shift+Tab"), [this] {
        state_->setChromeVisibility(state_->chromeVisibility() == ChromeVisibility::PanelsHidden
                                        ? ChromeVisibility::All
                                        : ChromeVisibility::PanelsHidden);
    });
    addKeyedShortcut(QStringLiteral("screen-mode"),
                     [this] { state_->cycleScreenMode(); });
    addShortcut(QStringLiteral("Shift+F"), [this] { state_->cycleScreenMode(true); });
    addKeyedShortcut(QStringLiteral("swap-colors"),
                     [this] { state_->swapColors(); });
    addKeyedShortcut(QStringLiteral("reset-colors"),
                     [this] { state_->resetColors(); });
    addKeyedShortcut(QStringLiteral("next-doc"),
                     [this] {
        const int count = state_->documents().size();
        if (count > 1)
            state_->setActiveDocumentIndex((state_->activeDocumentIndex() + 1) % count);
    });
    addKeyedShortcut(QStringLiteral("prev-doc"),
                     [this] {
        const int count = state_->documents().size();
        if (count > 1)
            state_->setActiveDocumentIndex((state_->activeDocumentIndex() + count - 1) % count);
    });
    addKeyedShortcut(QStringLiteral("spotlight"),
                     [this] { showSpotlight(); });
    addKeyedShortcut(QStringLiteral("zoom-in"),
                     [this] { if (canvas_) canvas_->zoomIn(); });
    addKeyedShortcut(QStringLiteral("zoom-out"),
                     [this] { if (canvas_) canvas_->zoomOut(); });
    addKeyedShortcut(QStringLiteral("fit-screen"),
                     [this] { if (canvas_) canvas_->zoomToFit(); });
    addKeyedShortcut(QStringLiteral("actual-pixels"),
                     [this] { if (canvas_) canvas_->zoomActualPixels(); });
    // conventional behaviour (Windows/Linux): these keymap ids had defaults but no
    // carrier action, so a keymap.json override could never reach them.
    addKeyedShortcut(QStringLiteral("invert"), [this] {
        if (canvas_ && canvas_->textEditing()) return;
        runCommand(QStringLiteral("invert"));
    });
    addKeyedShortcut(QStringLiteral("fill-fg"), [this] {
        if (canvas_ && canvas_->textEditing()) return;
        runCommand(QStringLiteral("fill-fg"));
    });
    addKeyedShortcut(QStringLiteral("fill-bg"), [this] {
        if (canvas_ && canvas_->textEditing()) return;
        runCommand(QStringLiteral("fill-bg"));
    });
    refreshCarrierShortcuts();
    // Session snapping already mirrors Settings.toml from the AppState
    // constructor; re-assert here so a stale transient can never survive
    // startup (both the master toggle and the Snap To targets persist).
    state_->setSnapEnabled(state_->settings().snapEnabled);
    state_->setSnapTargets(state_->settings().snapTargets);
}


void MainWindow::refreshCarrierShortcuts() {
    keymapCache_ = loadKeymapEntries();
    for (const auto& entry : carrierShortcuts_) {
        const QString seq = keymapCache_.value(entry.first);
        entry.second->setShortcut(
            seq.isEmpty() ? QKeySequence()
                          : QKeySequence(seq, QKeySequence::PortableText));
    }
}


bool MainWindow::handleBrushSizeKey(QKeyEvent* key) {
    // Shift is the hardness flag, not part of the binding. An id missing
    // from the map was explicitly unbound, so it matches nothing (fresh
    // installs carry the defaults, so [ / ] work out of the box).
    const QString smallerSeq =
        keymapCache_.value(QStringLiteral("brush-smaller"));
    const QString largerSeq =
        keymapCache_.value(QStringLiteral("brush-larger"));
    const bool smaller =
        !smallerSeq.isEmpty() && keyEventMatchesShortcut(key, smallerSeq);
    const bool larger =
        !largerSeq.isEmpty() && keyEventMatchesShortcut(key, largerSeq);
    if (!smaller && !larger) return false;
    const ToolId t = state_->activeTool();
    const bool hardness = (key->modifiers() & Qt::ShiftModifier) != 0;
    if (hardness) {
        // Shift+key: brush hardness 0..100% in fine steps.
        const double cur =
            state_->option(t, QStringLiteral("brush_hardness")).toDouble();
        const double base =
            state_->option(t, QStringLiteral("brush_hardness")).isValid()
                ? cur
                : 50.0;
        const double next = smaller ? qMax(0.0, base - 5.0)
                                    : qMin(100.0, base + 5.0);
        state_->setOption(t, QStringLiteral("brush_hardness"), next);
    } else {
        // Fine-grain size steps (~4%) so tapping lands exactly.
        const double cur =
            state_->option(t, QStringLiteral("brush_size")).toDouble();
        const double base = cur > 0.0 ? cur : 64.0;
        const double next = smaller ? qMax(1.0, base * 0.96)
                                    : qMin(5000.0, base / 0.96);
        state_->setOption(t, QStringLiteral("brush_size"), next);
    }
    return true;
}


bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Text entry always wins: tool letters must not fire while a field has focus.
    if (QWidget* focus = qApp->focusWidget()) {
        if (qobject_cast<QLineEdit*>(focus) || focus->inherits("QAbstractSpinBox") ||
            focus->inherits("QTextEdit"))
            return QMainWindow::eventFilter(watched, event);
    }
    // A live Type-tool session on the canvas is text entry too: letters, Space,
    // Delete/Backspace, Escape and the spring-loaded Ctrl/Space tools must all
    // yield to the caret.
    if (canvas_ && canvas_->textEditing())
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Held brush-size keys repeat for every brush tool (issue #8);
        // every other key ignores repeats (no Space/Ctrl re-push, no
        // repeated layer delete, no tool-letter retrigger).
        if (key->isAutoRepeat()) {
            if (handleBrushSizeKey(key)) return true;
            return QMainWindow::eventFilter(watched, event);
        }

        // Escape cancels an in-progress Liquify warp back to the snapshot.
        if (key->key() == Qt::Key_Escape && canvas_ && canvas_->liquifyStrokeActive()) {
            canvas_->cancelLiquify();
            return true;
        }

        // Space = Hand, Ctrl = Move: the two universal spring-loaded overrides.
        if (key->key() == Qt::Key_Space && !spaceHeld_) {
            spaceHeld_ = true;
            state_->pushTemporaryTool(ToolId::Hand);
            return true;
        }
        if (key->key() == Qt::Key_Control && !ctrlHeld_) {
            ctrlHeld_ = true;
            state_->pushTemporaryTool(ToolId::Move);
            return true;
        }

        // Delete/Backspace remove the selected layer(s) from anywhere (canvas,
        // Layers panel) — the text-entry guard at the top already let
        // Delete/Backspace through inside fields, so this only fires when
        // nothing is being typed. The canvas Move-tool click selects a layer
        // first, so click-a-layer-then-Del removes it too.
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) &&
            !(key->modifiers() &
              (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            runCommand(QStringLiteral("delete-layer"));
            return true;
        }
        // Brush size keys follow Settings > Shortcuts (brush-smaller/larger);
        // first press and auto-repeat share one handler (issue #8).
        if (handleBrushSizeKey(key)) return true;
        if (key->key() >= Qt::Key_0 && key->key() <= Qt::Key_9 &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier))) {
            const int d = key->key() - Qt::Key_0;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            int pct = (d == 0) ? 100 : d * 10;
            // Two digits tapped quickly combine (4 then 5 = 45%, 0 then 0 = 100%).
            if (numFirst_ >= 0 && now - numFirstMs_ < 1000) {
                pct = numFirst_ * 10 + d;
                if (pct <= 0 || pct > 100) pct = 100;
                numFirst_ = -1;
            } else {
                numFirst_ = d;
                numFirstMs_ = now;
            }
            const ToolId t = state_->activeTool();
            if (state_->option(t, QStringLiteral("opacity")).isValid()) {
                state_->setOption(t, QStringLiteral("opacity"), pct);
                return true;
            }
            // Tools without their own opacity (e.g. Move) drive the active
            // layer's opacity instead, like the usual number keys.
            if (state_->setActiveLayerOpacity(pct)) return true;
        }
        // Arrow nudge: Move tool (or Ctrl-held temp Move) shifts every
        // selected pixel layer by the Settings > Canvas step (Shift multiplies
        // it to the second step).
        if ((key->key() == Qt::Key_Left || key->key() == Qt::Key_Right ||
             key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            const bool big = (key->modifiers() & Qt::ShiftModifier) != 0;
            const double step = big ? state_->settings().nudgeShiftStepPx
                                    : state_->settings().nudgeStepPx;
            QPointF delta(0, 0);
            if (key->key() == Qt::Key_Left) delta.setX(-step);
            else if (key->key() == Qt::Key_Right) delta.setX(step);
            else if (key->key() == Qt::Key_Up) delta.setY(-step);
            else delta.setY(step);
            // A nudge is one undoable step per keypress.
            state_->beginUndoStep();
            if (state_->moveSelectedLayers(delta)) {
                state_->commitUndoStep(tr("Nudge"), QStringLiteral("move"));
                return true;
            }
            state_->discardUndoStep();
        }

        const QString text = key->text().toUpper();
        if (text.size() == 1 && text.at(0).isLetter() &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            const char letter = text.at(0).toLatin1();
            // E on a Brush/Pencil toggles erase blend (same tip, the same
            // behaviour); from the Eraser it returns to the Brush; anywhere
            // else E selects the Eraser tool as before.
            if (letter == 'E' && !(key->modifiers() & Qt::ShiftModifier)) {
                const ToolId active = state_->activeTool();
                if (active == ToolId::Brush || active == ToolId::Pencil) {
                    const QVariant cur = state_->option(
                        active, QStringLiteral("brush_erase_blend"));
                    state_->setOption(active,
                                      QStringLiteral("brush_erase_blend"),
                                      !cur.toBool());
                    return true;
                }
                if (active == ToolId::Eraser) {
                    state_->setActiveTool(ToolId::Brush);
                    return true;
                }
            }
            if (const ToolGroup* group = groupForKey(letter)) {
                if (key->modifiers() & Qt::ShiftModifier) {
                    state_->cycleToolGroup(letter);
                } else if (!springActive_) {
                    springActive_ = true;
                    springTool_ = state_->activeTool();
                    springPressTime_ = QDateTime::currentMSecsSinceEpoch();
                    state_->setActiveTool(group->members.empty() ? group->leader
                                                                 : group->members.front());
                }
                return true;
            }
        }
    }

    if (event->type() == QEvent::KeyRelease) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->isAutoRepeat()) return QMainWindow::eventFilter(watched, event);

        if (key->key() == Qt::Key_Space && spaceHeld_) {
            spaceHeld_ = false;
            state_->popTemporaryTool();
            return true;
        }
        if (key->key() == Qt::Key_Control && ctrlHeld_) {
            ctrlHeld_ = false;
            state_->popTemporaryTool();
            return true;
        }
        if (springActive_) {
            const qint64 held = QDateTime::currentMSecsSinceEpoch() - springPressTime_;
            springActive_ = false;
            // Held: spring back to the previous tool. Tapped: the switch sticks.
            if (held >= kSpringDwellMs) state_->setActiveTool(springTool_);
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}


void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        // Never leave a spring-loaded tool latched when focus goes elsewhere.
        if (spaceHeld_ || ctrlHeld_) {
            spaceHeld_ = ctrlHeld_ = false;
            state_->popTemporaryTool();
        }
        springActive_ = false;
    }
    QMainWindow::changeEvent(event);
}

}  // namespace pittore::ui
