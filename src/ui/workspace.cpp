#include "ui/workspace.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QStandardPaths>

#include "ui/app_state.h"
#include "ui/panels.h"
#include "ui/tools_panel.h"

namespace pittore::ui {
namespace {

// Built-in panel recipes, close to conventional presets.
struct Preset {
    const char* name;
    QStringList panels;
};

const QVector<Preset>& presets() {
    static const QVector<Preset> p = {
        {"Essentials",
         {QStringLiteral("color"), QStringLiteral("swatches"), QStringLiteral("properties"),
          QStringLiteral("adjustments"), QStringLiteral("layers"), QStringLiteral("channels"),
          QStringLiteral("paths")}},
        {"Photography",
         {QStringLiteral("histogram"), QStringLiteral("navigator"), QStringLiteral("info"),
          QStringLiteral("adjustments"), QStringLiteral("properties"), QStringLiteral("layers"),
          QStringLiteral("history")}},
        {"Painting",
         {QStringLiteral("brushes"), QStringLiteral("color"), QStringLiteral("swatches"),
          QStringLiteral("layers"), QStringLiteral("history")}},
        {"Graphic and Web",
         {QStringLiteral("color"), QStringLiteral("swatches"), QStringLiteral("libraries"),
          QStringLiteral("properties"), QStringLiteral("layers"), QStringLiteral("paths"),
          QStringLiteral("character"), QStringLiteral("paragraph")}},
        {"Typography",
         {QStringLiteral("character"), QStringLiteral("paragraph"), QStringLiteral("swatches"),
          QStringLiteral("layers"), QStringLiteral("properties")}},
        {"Motion",
         {QStringLiteral("layers"), QStringLiteral("properties"), QStringLiteral("history"),
          QStringLiteral("actions")}},
        {"Color Grading",
         {QStringLiteral("histogram"), QStringLiteral("info"), QStringLiteral("color"),
          QStringLiteral("swatches"), QStringLiteral("adjustments"),
          QStringLiteral("properties"), QStringLiteral("layers"),
          QStringLiteral("channels")}},
    };
    return p;
}

const Preset* findPreset(const QString& name) {
    for (const Preset& p : presets())
        if (QLatin1String(p.name) == name) return &p;
    return nullptr;
}

}  // namespace

WorkspaceManager::WorkspaceManager(QMainWindow* window, AppState* state, ToolsPanel* tools,
                                   QObject* parent)
    : QObject(parent), window_(window), state_(state), tools_(tools) {}

QStringList WorkspaceManager::builtInNames() const {
    QStringList names;
    for (const Preset& p : presets()) names << QString::fromUtf8(p.name);
    return names;
}

QStringList WorkspaceManager::customNames() const {
    QStringList names;
    for (const WorkspaceSnapshot& s : custom_) names << s.name;
    return names;
}

QString WorkspaceManager::settingsPath() const {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/workspaces.json");
}

WorkspaceSnapshot WorkspaceManager::capture(const QString& name) const {
    WorkspaceSnapshot snapshot;
    snapshot.name = name;
    snapshot.dockState = window_->saveState();
    snapshot.windowGeometry = window_->saveGeometry();
    snapshot.twoColumnTools = tools_ && tools_->twoColumn();
    snapshot.twoColumnManual = tools_ && tools_->twoColumnManual();
    if (tools_) snapshot.hiddenTools = tools_->hiddenTools();
    return snapshot;
}

void WorkspaceManager::apply(const QString& name) {
    if (const Preset* preset = findPreset(name)) {
        emit presetRequested(name, preset->panels);
        current_ = name;
        emit currentChanged(current_);
        return;
    }
    for (const WorkspaceSnapshot& snapshot : custom_) {
        if (snapshot.name != name) continue;
        // Panels first — restoreState only places docks that already exist.
        emit presetRequested(name, snapshot.visiblePanels);
        if (!snapshot.dockState.isEmpty()) window_->restoreState(snapshot.dockState);
        if (tools_) {
            tools_->setTwoColumn(snapshot.twoColumnTools, snapshot.twoColumnManual);
            tools_->setHiddenTools(snapshot.hiddenTools);
        }
        current_ = name;
        emit currentChanged(current_);
        return;
    }
}

void WorkspaceManager::resetCurrent() { apply(current_); }

void WorkspaceManager::resetAll() {
    custom_.clear();
    saveToDisk();
    apply(QStringLiteral("Essentials"));
    emit workspacesChanged();
}

void WorkspaceManager::saveCurrentAs(const QString& name, bool includeShortcuts,
                                     bool includeMenus, bool includeToolbar) {
    WorkspaceSnapshot snapshot = capture(name);
    snapshot.includeShortcuts = includeShortcuts;
    snapshot.includeMenus = includeMenus;
    snapshot.includeToolbar = includeToolbar;
    for (const PanelInfo& info : allPanels())
        if (QWidget* dock = window_->findChild<QWidget*>(QStringLiteral("dock_") + info.id))
            if (dock->isVisible()) snapshot.visiblePanels << info.id;

    for (int i = 0; i < custom_.size(); ++i) {
        if (custom_[i].name == name) {
            custom_[i] = snapshot;
            saveToDisk();
            current_ = name;
            emit workspacesChanged();
            emit currentChanged(current_);
            return;
        }
    }
    custom_.append(snapshot);
    saveToDisk();
    current_ = name;
    emit workspacesChanged();
    emit currentChanged(current_);
}

void WorkspaceManager::remove(const QString& name) {
    for (int i = 0; i < custom_.size(); ++i) {
        if (custom_[i].name == name) {
            custom_.removeAt(i);
            saveToDisk();
            emit workspacesChanged();
            if (current_ == name) apply(QStringLiteral("Essentials"));
            return;
        }
    }
}

void WorkspaceManager::saveToDisk() const {
    QJsonArray array;
    for (const WorkspaceSnapshot& snapshot : custom_) {
        QJsonObject object;
        object[QStringLiteral("name")] = snapshot.name;
        object[QStringLiteral("dockState")] =
            QString::fromLatin1(snapshot.dockState.toBase64());
        object[QStringLiteral("geometry")] =
            QString::fromLatin1(snapshot.windowGeometry.toBase64());
        object[QStringLiteral("panels")] = QJsonArray::fromStringList(snapshot.visiblePanels);
        object[QStringLiteral("hiddenTools")] = QJsonArray::fromStringList(snapshot.hiddenTools);
        object[QStringLiteral("twoColumnTools")] = snapshot.twoColumnTools;
        object[QStringLiteral("twoColumnManual")] = snapshot.twoColumnManual;
        object[QStringLiteral("includeShortcuts")] = snapshot.includeShortcuts;
        object[QStringLiteral("includeMenus")] = snapshot.includeMenus;
        object[QStringLiteral("includeToolbar")] = snapshot.includeToolbar;
        array.append(object);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("current")] = current_;
    root[QStringLiteral("workspaces")] = array;

    QFile file(settingsPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void WorkspaceManager::loadFromDisk() {
    QFile file(settingsPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    custom_.clear();
    for (const QJsonValue& value : root[QStringLiteral("workspaces")].toArray()) {
        const QJsonObject object = value.toObject();
        WorkspaceSnapshot snapshot;
        snapshot.name = object[QStringLiteral("name")].toString();
        snapshot.dockState = QByteArray::fromBase64(
            object[QStringLiteral("dockState")].toString().toLatin1());
        snapshot.windowGeometry = QByteArray::fromBase64(
            object[QStringLiteral("geometry")].toString().toLatin1());
        for (const QJsonValue& panel : object[QStringLiteral("panels")].toArray())
            snapshot.visiblePanels << panel.toString();
        for (const QJsonValue& tool : object[QStringLiteral("hiddenTools")].toArray())
            snapshot.hiddenTools << tool.toString();
        snapshot.twoColumnTools = object[QStringLiteral("twoColumnTools")].toBool();
        snapshot.twoColumnManual = object[QStringLiteral("twoColumnManual")].toBool();
        snapshot.includeShortcuts = object[QStringLiteral("includeShortcuts")].toBool();
        snapshot.includeMenus = object[QStringLiteral("includeMenus")].toBool();
        snapshot.includeToolbar = object[QStringLiteral("includeToolbar")].toBool(true);
        custom_.append(snapshot);
    }
    const QString stored = root[QStringLiteral("current")].toString();
    if (!stored.isEmpty()) current_ = stored;
    emit workspacesChanged();
}

}  // namespace pittore::ui
