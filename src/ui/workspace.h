#pragma once
// Keep QObject first: qchar.h must complete QChar before libstdc++ SFINAE
// headers see it (trips GCC 16 otherwise).
#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

class QMainWindow;

namespace pittore::ui {

class AppState;
class ToolsPanel;

// A workspace is a named panel layout (+ optional toolbar/shortcuts).
// Built on QMainWindow::saveState plus what Qt misses: panel set, columns,
// hidden tools.
struct WorkspaceSnapshot {
    QString name;
    QByteArray dockState;
    QByteArray windowGeometry;
    QStringList visiblePanels;
    QStringList hiddenTools;
    bool twoColumnTools = false;
    // True when the column count was an explicit choice (grip toggle,
    // Preferences): restores keep it, otherwise the strip auto-fits and old
    // snapshots without this key auto-fit too.
    bool twoColumnManual = false;
    bool includeShortcuts = false;
    bool includeMenus = false;
    bool includeToolbar = true;
    bool builtIn = false;
};

class WorkspaceManager : public QObject {
    Q_OBJECT

  public:
    WorkspaceManager(QMainWindow* window, AppState* state, ToolsPanel* tools,
                     QObject* parent = nullptr);

    // Built-ins are visibility recipes, not blobs, so they survive panel changes.
    QStringList builtInNames() const;
    QStringList customNames() const;
    QString current() const { return current_; }

    void apply(const QString& name);
    void resetCurrent();
    void resetAll();

    WorkspaceSnapshot capture(const QString& name) const;
    void saveCurrentAs(const QString& name, bool includeShortcuts, bool includeMenus,
                       bool includeToolbar);
    void remove(const QString& name);

    void loadFromDisk();
    void saveToDisk() const;

  signals:
    void workspacesChanged();
    void currentChanged(const QString& name);
    // Panels a preset wants; the main window creates the docks.
    void presetRequested(const QString& name, const QStringList& panels);

  private:
    QString settingsPath() const;

    QMainWindow* window_;
    AppState* state_;
    ToolsPanel* tools_;
    QString current_ = QStringLiteral("Essentials");
    QList<WorkspaceSnapshot> custom_;
};

}  // namespace pittore::ui
