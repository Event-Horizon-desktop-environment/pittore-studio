#include "ui/persona/persona_manager.h"

#include <QSettings>

#include "ui/app_state.h"
#include "ui/panels.h"
#include "ui/tool_registry.h"
#include "ui/tools_panel.h"

namespace pittore::ui {
namespace {

const char* kOrg = "PittoreStudio";
const char* kApp = "painter";
const char* kKey = "persona/current";

}  // namespace

PersonaManager::PersonaManager(AppState* state, ToolsPanel* tools, QObject* parent)
    : QObject(parent), state_(state), tools_(tools) {
    load();
}

void PersonaManager::setPersona(Persona p) {
    if (p == persona_) return;
    // Remember the outgoing persona's toolbar customisation before overwriting
    // it, so Edit Toolbar changes survive a Vector round-trip.
    if (tools_) hiddenCache_.insert(static_cast<int>(persona_), tools_->hiddenTools());
    previous_ = persona_;
    persona_ = p;
    apply(p);
    save();
    emit personaChanged(p);
}

void PersonaManager::applyCurrent() {
    apply(persona_);
    emit personaChanged(persona_);
}

void PersonaManager::apply(Persona p) {
    if (tools_) {
        const QStringList hidden = hiddenCache_.value(static_cast<int>(p), hiddenToolsFor(p));
        tools_->setHiddenTools(hidden);
        // A tool hidden by the incoming tab must not stay active; Draw
        // lands on the Brush, everything else falls back to Move.
        if (state_) {
            if (const ToolGroup* group = groupForTool(state_->activeTool())) {
                if (hidden.contains(QString::number(static_cast<int>(group->leader))))
                    state_->setActiveTool(p == Persona::Draw ? ToolId::Brush
                                                             : ToolId::Move);
            }
        }
    }
    if (showPanel_) {
        auto isFloating = [](const QString& id) {
            const PanelInfo* info = panelInfo(id);
            return info && info->floating;
        };
        auto capturePixel = [&] {
            if (panelsCaptured_.value(static_cast<int>(p), false) ||
                !panelVisible_)
                return;
            QStringList captured;
            for (const PanelInfo& info : allPanels())
                if (!info.floating && panelVisible_(info.id))
                    captured << info.id;
            capturedPanels_.insert(static_cast<int>(p), captured);
            panelsCaptured_.insert(static_cast<int>(p), true);
        };
        if (p == Persona::Pixel) {
            const QStringList restore =
                !capturedPanels_.value(static_cast<int>(previous_)).isEmpty()
                    ? capturedPanels_.value(static_cast<int>(previous_))
                    : pixelFallbackPanels();
            for (const QString& id : restore) showPanel_(id, true);
            return;
        }
        // Leaving Pixel: snapshot once per target tab, hide the panels
        // that tab must not see, then show its own (never floating —
        // separate windows stay uninvited).
        capturePixel();
        for (const QString& id : pixelOnlyPanels()) showPanel_(id, false);
        for (const QString& id : artboardOnlyPanels()) showPanel_(id, false);
        if (p == Persona::Vector) {
            for (const QString& id : vectorPanels())
                if (!isFloating(id)) showPanel_(id, true);
        } else if (p == Persona::Color) {
            for (const QString& id : vectorOnlyPanels()) showPanel_(id, false);
            for (const QString& id : colorPanels())
                if (!isFloating(id)) showPanel_(id, true);
        } else if (p == Persona::Artboard) {
            for (const QString& id : vectorOnlyPanels()) showPanel_(id, false);
            for (const QString& id : artboardPanels())
                if (!isFloating(id)) showPanel_(id, true);
        } else {
            for (const QString& id : vectorOnlyPanels()) showPanel_(id, false);
            for (const QString& id : drawPanels())
                if (!isFloating(id)) showPanel_(id, true);
        }
    }
}

void PersonaManager::load() {
    QSettings settings(QString::fromUtf8(kOrg), QString::fromUtf8(kApp));
    QString id = settings.value(QString::fromUtf8(kKey)).toString();
    if (id.isEmpty()) {
        // Migrate from the InfinityPhoto identity on first run.
        const QSettings legacy(QStringLiteral("InfinityPhoto"),
                               QStringLiteral("InfinityPhoto"));
        id = legacy.value(QStringLiteral("persona/current")).toString();
        if (id.isEmpty())
            id = legacy.value(QString::fromUtf8(kKey)).toString();
    } else {
        Persona p = Persona::Pixel;
        if (personaFromId(id, &p)) persona_ = p;
        return;
    }
    Persona p = Persona::Pixel;
    if (personaFromId(id.isEmpty() ? personaId(Persona::Pixel) : id, &p))
        persona_ = p;
}

void PersonaManager::save() const {
    QSettings settings(QString::fromUtf8(kOrg), QString::fromUtf8(kApp));
    settings.setValue(QString::fromUtf8(kKey), personaId(persona_));
}

}  // namespace pittore::ui
