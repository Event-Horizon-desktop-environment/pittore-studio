#pragma once
// Live-session MCP bridge endpoint (in-app side). McpBridge listens on a
// well-known local socket and answers newline-delimited JSON requests from
// the pittore-mcp shim (which speaks MCP stdio to AI clients). Everything
// runs on the GUI thread the bridge lives on: socket signals arrive through
// that thread's event loop, so AppState calls need no extra dispatch — but
// every command must stay fast, since a request blocks the UI while it runs.
// Mutations reuse the same undo-safe AppState entry points as the panels.
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;

namespace pittore::ui {

class AppState;

// Well-known local socket path, shared with the pittore-mcp shim.
QString mcpSocketPath();

class McpBridge : public QObject {
    Q_OBJECT
  public:
    explicit McpBridge(AppState* state, QObject* parent = nullptr);
    // Listen for shim connections. False when another session owns the
    // path (second GUI instance) — the app keeps running bridgeless.
    // An explicit path overrides the well-known one (tests).
    bool start(const QString& socketPath = QString());
    QString socketPath() const;
    // Headless command runner, wired by main.cpp to MainWindow. Kept as a
    // callback so the bridge never includes window headers; empty until set.
    void setCommandRunner(std::function<bool(const QString&)> runner) {
        commandRunner_ = std::move(runner);
    }

  private:
    void onNewConnection();
    void onClientGone(QObject* sock);
    void onReadyRead(QLocalSocket* sock);
    // One request object in, one response object out (never null).
    QJsonObject dispatch(const QJsonObject& req);

    AppState* state_ = nullptr;
    std::function<bool(const QString&)> commandRunner_;
    QLocalServer* server_ = nullptr;
    QString socketPath_;
    QHash<QLocalSocket*, QByteArray> pending_;
};

}  // namespace pittore::ui
