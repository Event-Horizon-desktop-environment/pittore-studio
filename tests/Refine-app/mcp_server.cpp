#include "mcp_server.h"

#include <QBuffer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "lab_mainwindow.h"

namespace pittore::lab {

namespace {

// ---- JSON-RPC helpers ----------------------------------------------------
QJsonObject rpcResult(const QJsonValue& id, const QJsonValue& result) {
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}
QJsonObject rpcError(const QJsonValue& id, int code, const QString& msg) {
    return QJsonObject{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", QJsonObject{{"code", code}, {"message", msg}}}};
}

// A text content item for tools/call.
QJsonObject textContent(const QString& text) {
    return QJsonObject{{"type", "text"}, {"text", text}};
}
QJsonObject imageContent(const QByteArray& png) {
    return QJsonObject{{"type", "image"},
                       {"mimeType", "image/png"},
                       {"data", QString::fromLatin1(png.toBase64())}};
}

// The tool catalogue. Every slider / button / view control in the UI maps to
// a tool or to the free-form `set_params` (which takes ANY LabParams field).
QJsonArray toolDefinitions() {
    auto obj = [](const char* name, const char* desc,
                  const QJsonObject& props, const QJsonArray& required) {
        return QJsonObject{{"name", name},
                           {"description", desc},
                           {"inputSchema",
                            QJsonObject{{"type", "object"},
                                        {"properties", props},
                                        {"required", required}}}};
    };
    const QJsonObject strP{{"type", "string"}};
    auto intP = [](int mn, int mx) {
        return QJsonObject{{"type", "integer"},
                           {"minimum", mn},
                           {"maximum", mx}};
    };
    const QJsonObject anyObj{{"type", "object"}};

    return QJsonArray{
        obj("get_state", "Return every parameter plus live status "
                         "(image/matte sizes, stage list, timings).", {}, {}),
        obj("set_params", "Merge a JSON object of LabParams fields "
                          "(any slider/checkbox), then re-run the pipeline. "
                          "Returns the new state.",
            {{"params", anyObj}}, {{"params"}}),
        obj("reset_params", "Reset every parameter to defaults and re-run.", {},
            {}),
        obj("reset_image", "Drop the seed matte (keep the image) so a fresh "
                           "segment can start clean.", {}, {}),
        obj("undo", "Step back to the previous tuning state.", {}, {}),
        obj("redo", "Step forward to the next tuning state.", {}, {}),
        obj("load_image", "Load an image from a path (becomes the source).",
            {{"path", strP}}, {{"path"}}),
        obj("segment", "Run the AI model to build the seed matte "
                       "(Select Subject / Remove Background path).",
            {}, {}),
        obj("refine", "Run the full refine pipeline with the current "
                      "parameters. Returns stage list + timing + coverage.",
            {}, {}),
        obj("render", "Render the current view as a PNG image (the result "
                      "you would see). Optionally set the view mode and a "
                      "max dimension to downscale for inspection.",
            {{"view", intP(0, 6)}, {"max_dim", intP(64, 4096)}}, {}),
        obj("set_view", "Set the preview mode: 0 Original, 1 Matte, "
                        "2 Rubylith, 3 Cutout/transparency, 4 Cutout/flat "
                        "color, 5 Black&White, 6 Fringe map.",
            {{"mode", intP(0, 6)}}, {{"mode"}}),
        obj("set_solid_bg", "Set the flat background for the fringe view: "
                            "0 Magenta, 1 Green, 2 Gray, 3 Black.",
            {{"index", intP(0, 3)}}, {{"index"}}),
        obj("set_dark", "Toggle dark mode.", {{"on", QJsonObject{{"type", "boolean"}}}},
            {{"on"}}),
        obj("save_preset", "Write the current parameters to a JSON file.",
            {{"path", strP}}, {{"path"}}),
        obj("load_preset", "Load parameters from a JSON file.", {{"path", strP}},
            {{"path"}}),
    };
}

// ---- HTTP ---------------------------------------------------------------
void respond(QTcpSocket* sock, int status, const QByteArray& body,
             const char* contentType = "application/json") {
    QByteArray out;
    out += "HTTP/1.1 " + QByteArray::number(status) + " " +
           (status == 200 ? "OK" : (status == 202 ? "Accepted" : "Error")) +
           "\r\n";
    out += "Content-Type: " + QByteArray(contentType) + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    sock->write(out);
    sock->flush();
    sock->disconnectFromHost();
}

}  // namespace

LabMcpServer::LabMcpServer(LabWindow* window, QObject* parent)
    : QObject(parent), window_(window), server_(new QTcpServer(this)) {
    connect(server_, &QTcpServer::newConnection, this,
            [this] { onNewConnection(); });
}

bool LabMcpServer::listen(quint16 port, QString* err) {
    if (!server_->listen(QHostAddress::LocalHost, port)) {
        if (err) *err = server_->errorString();
        return false;
    }
    return true;
}

quint16 LabMcpServer::port() const { return server_->serverPort(); }

void LabMcpServer::onNewConnection() {
    while (QTcpSocket* sock = server_->nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            // Accumulate; parse once the full HTTP request (headers +
            // Content-Length body) has arrived.
            QByteArray& buf = buffers_[sock];
            buf += sock->readAll();
            const int hdrEnd = buf.indexOf("\r\n\r\n");
            if (hdrEnd < 0) return;  // incomplete headers; wait for more
            const QByteArray headers = buf.left(hdrEnd);
            int contentLength = 0;
            for (const QByteArray& line : headers.split('\n')) {
                const QByteArray l = line.trimmed().toLower();
                if (l.startsWith("content-length:"))
                    contentLength =
                        l.mid(QStringLiteral("content-length:").size()).trimmed().toInt();
            }
            if (buf.size() - (hdrEnd + 4) < contentLength) return;  // wait
            const QByteArray body = buf.mid(hdrEnd + 4, contentLength);
            buffers_.remove(sock);
            handleRequest(sock, body);
        });
        connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
            buffers_.remove(sock);
            sock->deleteLater();
        });
    }
}

void LabMcpServer::handleRequest(QTcpSocket* sock, const QByteArray& body) {
    // Health probe / GET -> simple info so `curl host:port` confirms liveness.
    if (body.isEmpty()) {
        respond(sock, 200,
                QJsonDocument(QJsonObject{
                    {"server", "refine-lab"},
                    {"transport", "mcp-over-http POST /mcp"}})
                    .toJson());
        return;
    }

    const QJsonObject rpc = QJsonDocument::fromJson(body).object();
    const QString method = rpc["method"].toString();
    const QJsonValue id = rpc["id"];  // absent for notifications
    const QJsonObject params = rpc["params"].toObject();

    // Notifications need no reply.
    if (method == QStringLiteral("notifications/initialized") ||
        method.startsWith(QStringLiteral("notifications/"))) {
        respond(sock, 202, {});
        return;
    }

    if (method == QStringLiteral("initialize")) {
        const QJsonObject init{
            {"protocolVersion", "2024-11-05"},
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
            {"serverInfo",
             QJsonObject{{"name", "refine-lab"}, {"version", "1.0"}}}};
        respond(sock, 200,
                QJsonDocument(rpcResult(id, init)).toJson());
        return;
    }
    if (method == QStringLiteral("tools/list")) {
        respond(sock, 200,
                QJsonDocument(rpcResult(id, QJsonObject{{"tools", toolDefinitions()}}))
                    .toJson());
        return;
    }
    if (method == QStringLiteral("tools/call")) {
        const QString name = params["name"].toString();
        const QJsonObject args = params["arguments"].toObject();
        QString err;
        QJsonArray content;
        bool toolErr = false;

        auto stateText = [&](const QJsonObject& s) {
            content.append(
                textContent(QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Compact))));
        };

        if (name == QStringLiteral("get_state")) {
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("set_params")) {
            window_->pushHistory();
            window_->applyParams(args["params"].toObject());
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("reset_params")) {
            window_->pushHistory();
            window_->resetParams();
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("reset_image")) {
            window_->resetImage();
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("undo")) {
            window_->undo();
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("redo")) {
            window_->redo();
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("load_image")) {
            if (!window_->loadImageFile(args["path"].toString(), &err)) {
                toolErr = true;
                content.append(textContent(err));
            } else {
                stateText(window_->paramsState());
            }
        } else if (name == QStringLiteral("segment")) {
            if (!window_->segment(&err)) {
                toolErr = true;
                content.append(textContent(err));
            } else {
                stateText(window_->paramsState());
            }
        } else if (name == QStringLiteral("refine")) {
            stateText(window_->refine());
        } else if (name == QStringLiteral("render")) {
            if (args.contains("view"))
                window_->setViewMode(args["view"].toInt());
            QImage img = window_->render();
            const int maxDim = args["max_dim"].toInt(1200);
            if (!img.isNull() && maxDim > 0) {
                const int m = std::max(img.width(), img.height());
                if (m > maxDim) {
                    const double s = maxDim / double(m);
                    img = img.scaled(std::max(1, int(img.width() * s)),
                                     std::max(1, int(img.height() * s)),
                                     Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
                }
            }
            if (img.isNull()) {
                toolErr = true;
                content.append(textContent(QStringLiteral("nothing to render")));
            } else {
                QByteArray png;
                QBuffer pb(&png);
                pb.open(QIODevice::WriteOnly);
                img.save(&pb, "PNG");
                content.append(imageContent(png));
            }
        } else if (name == QStringLiteral("set_view")) {
            window_->setViewMode(args["mode"].toInt());
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("set_solid_bg")) {
            window_->setSolidBg(args["index"].toInt());
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("set_dark")) {
            window_->setDarkMode(args["on"].toBool());
            stateText(window_->paramsState());
        } else if (name == QStringLiteral("save_preset")) {
            QFile f(args["path"].toString());
            if (!f.open(QIODevice::WriteOnly)) {
                toolErr = true;
                content.append(textContent(QStringLiteral("could not write ")));
            } else {
                f.write(QJsonDocument(window_->paramsState()["params"].toObject())
                            .toJson(QJsonDocument::Indented));
                content.append(textContent(QStringLiteral("saved")));
            }
        } else if (name == QStringLiteral("load_preset")) {
            QFile f(args["path"].toString());
            if (!f.open(QIODevice::ReadOnly)) {
                toolErr = true;
                content.append(textContent(QStringLiteral("could not read")));
            } else {
                window_->pushHistory();
                window_->applyParams(
                    QJsonDocument::fromJson(f.readAll()).object());
                stateText(window_->paramsState());
            }
        } else {
            toolErr = true;
            content.append(textContent(QStringLiteral("unknown tool: ") + name));
        }

        QJsonObject result{{"content", content}, {"isError", toolErr}};
        respond(sock, 200, QJsonDocument(rpcResult(id, result)).toJson());
        return;
    }

    respond(sock, 200,
            QJsonDocument(rpcError(id, -32601,
                                   QStringLiteral("method not found: ") + method))
                .toJson());
}

}  // namespace pittore::lab
