#include "ui/persona/persona_wire.h"

#include <QBoxLayout>

#include "ui/main_window.h"
#include "ui/persona/persona_bar.h"
#include "ui/persona/persona_manager.h"
#include "ui/tools_panel.h"

namespace pittore::ui {

void wirePersona(QMainWindow* window, AppState* state) {
    if (!window || !state) return;
    auto* main = qobject_cast<MainWindow*>(window);
    if (!main) return;
    // ToolsPanel is a QFrame named "toolsPanel" owned by the MainWindow, so it
    // is found without touching any core header.
    ToolsPanel* tools = window->findChild<ToolsPanel*>(QStringLiteral("toolsPanel"));

    auto* manager = new PersonaManager(state, tools, window);
    manager->setShowPanel(
        [main](const QString& id, bool show) { main->showPanel(id, show); });
    manager->setPanelVisible(
        [main](const QString& id) { return main->panelVisible(id); });

    QWidget* central = window->centralWidget();
    auto* bar = new PersonaBar(state, central);
    if (auto* box = qobject_cast<QBoxLayout*>(central ? central->layout() : nullptr))
        box->insertWidget(0, bar);

    QObject::connect(bar, &PersonaBar::personaSelected, manager,
                     &PersonaManager::setPersona);
    QObject::connect(manager, &PersonaManager::personaChanged, bar,
                     &PersonaBar::setPersona);
    // Colour Grading: the Color persona is the grading tab, so its adjustment
    // chooser opens on the left rail beside the canvas instead of staying in
    // the right stack. Runs after PersonaManager::apply(), which shows the
    // panel set, so this only re-homes the Adjustments panel.
    QObject::connect(manager, &PersonaManager::personaChanged, main,
                     [main](Persona p) {
                         if (p == Persona::Color)
                             main->raisePanelOnLeft(QStringLiteral("adjustments"));
                     });
    manager->applyCurrent();
}

}  // namespace pittore::ui
