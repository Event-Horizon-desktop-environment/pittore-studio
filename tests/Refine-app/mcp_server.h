#pragma once
// Embedded MCP server for the Refine Lab. Speaks JSON-RPC 2.0 over HTTP
// (POST /mcp) — the "streamable HTTP" MCP transport — so a client (the agent)
// can drive the whole app: read/set every parameter, segment, refine, render
// the current view back as an image, undo/redo/reset. Requests are marshalled
// onto the GUI thread, so the pipeline runs exactly as it does from the widgets.
//
// It is deliberately dependency-free (QTcpServer + hand-rolled HTTP), because
// the lab must build with whatever Qt the tree already uses.

#include <QHash>
#include <QObject>

class QTcpServer;
class QTcpSocket;

namespace pittore::lab {

class LabWindow;

// No Q_OBJECT: every Qt signal is wired to a lambda, so no moc step is needed.
class LabMcpServer : public QObject {
  public:
    explicit LabMcpServer(LabWindow* window, QObject* parent = nullptr);
    bool listen(quint16 port, QString* err);
    quint16 port() const;

  private:
    void onNewConnection();
    void handleRequest(QTcpSocket* sock, const QByteArray& body);

    LabWindow* window_;
    QTcpServer* server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
};

}  // namespace pittore::lab
