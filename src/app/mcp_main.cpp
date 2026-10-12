// pittore-mcp: MCP stdio shim for the live Pittore Studio session. AI
// clients speak MCP (JSON-RPC 2.0, newline-delimited) to this process on
// stdin/stdout; it forwards tool calls over the app's local-socket bridge
// (see ui/mcp/mcp_bridge) to the running GUI and translates the answers
// back. No GUI runs here (QCoreApplication); without a live session every
// tool call fails honestly instead of inventing state.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTextStream>

#include "ui/mcp/mcp_bridge.h"

namespace {

constexpr char kProtocolVersion[] = "2024-11-05";

// One bridge round trip over a fresh socket connection. Blocking waits are
// fine: MCP stdio is low-frequency request/response, and each call is one
// GUI-thread dispatch on the other end.
QJsonObject bridgeCall(const QString& cmd, const QJsonObject& params,
                       QString* error) {
    QLocalSocket sock;
    sock.connectToServer(pittore::ui::mcpSocketPath());
    if (!sock.waitForConnected(3000)) {
        *error = QStringLiteral(
                     "no running Pittore Studio session (bridge socket '%1' "
                     "unreachable; open the app first)")
                     .arg(pittore::ui::mcpSocketPath());
        return {};
    }
    const QJsonObject req{
        {"id", 1}, {"cmd", cmd}, {"params", params}};
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.write("\n", 1);
    if (!sock.waitForBytesWritten(3000)) {
        *error = QStringLiteral("bridge write failed");
        return {};
    }
    QByteArray buf;
    for (;;) {
        if (buf.contains('\n')) break;
        if (!sock.waitForReadyRead(10000)) {
            *error = QStringLiteral("bridge read timed out");
            return {};
        }
        buf += sock.readAll();
    }
    QJsonParseError perr{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(buf.left(buf.indexOf('\n') + 1), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("bridge returned malformed JSON");
        return {};
    }
    const QJsonObject resp = doc.object();
    if (!resp.value("ok").toBool(false)) {
        *error = resp.value("error").toString(QStringLiteral("bridge error"));
        return {};
    }
    return resp.value("result").toObject();
}

QJsonObject rpcError(const QJsonValue& id, int code, const QString& message) {
    return QJsonObject{{"jsonrpc", QStringLiteral("2.0")},
                       {"id", id},
                       {"error", QJsonObject{{"code", code},
                                             {"message", message}}}};
}

QJsonObject rpcOk(const QJsonValue& id, const QJsonObject& result) {
    return QJsonObject{
        {"jsonrpc", QStringLiteral("2.0")}, {"id", id}, {"result", result}};
}

QJsonObject toolDef(const char* name, const char* desc,
                    const QJsonObject& properties,
                    const QStringList& required = {}) {
    QJsonArray req;
    for (const QString& r : required) req.append(r);
    return QJsonObject{
        {"name", QString::fromLatin1(name)},
        {"description", QString::fromLatin1(desc)},
        {"inputSchema",
         QJsonObject{{"type", QStringLiteral("object")},
                     {"properties", properties},
                     {"required", req}}}};
}

QJsonObject listTools() {
    QJsonArray tools;
    tools.append(toolDef(
        "ping", "Check the bridge and report the app version.",
        QJsonObject{}));
    tools.append(toolDef(
        "list_documents", "List open documents (index, title, size).",
        QJsonObject{}));
    tools.append(toolDef(
        "document_info",
        "Active document metadata and layer stack (index, name, kind, "
        "visibility, opacity). Operates on the active document.",
        QJsonObject{}));
    tools.append(toolDef(
        "composite_png",
        "Rendered composite as a PNG image (long side capped by maxDim).",
        QJsonObject{{"maxDim",
                     QJsonObject{{"type", QStringLiteral("integer")},
                                 {"default", 768},
                                 {"minimum", 16},
                                 {"maximum", 2048}}}},
        {}));
    tools.append(toolDef(
        "set_layer_blend",
        "Blend mode of one layer of the active document (own undo step). "
        "Names follow the Layers panel menu: Normal, Multiply, Screen, "
        "Overlay, Soft Light, Color, Luminosity and the rest.",
        QJsonObject{
            {"layer",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"description",
                          QStringLiteral("panel index, top-first")}}},
            {"mode", QJsonObject{{"type", QStringLiteral("string")},
                                 {"description",
                                  QStringLiteral("blend mode name")}}}},
        {"layer", "mode"}));
    tools.append(toolDef(
        "set_layer_opacity",
        "Opacity 0..100 of one layer of the active document (own undo "
        "step). Makes glazes and blended texture layers possible.",
        QJsonObject{
            {"layer",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"description",
                          QStringLiteral("panel index, top-first")}}},
            {"opacity",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"minimum", 0},
                         {"maximum", 100}}}},
        {"layer", "opacity"}));
    tools.append(toolDef(
        "list_tools",
        "Every editor tool (key, display name, shortcut). Names work "
        "anywhere a tool is accepted.",
        QJsonObject{}));
    tools.append(toolDef(
        "list_tool_options",
        "All options of one tool with ranges, defaults and live values.",
        QJsonObject{
            {"tool",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"description",
                          QStringLiteral("tool name or key, e.g. brush")}}}},
        {"tool"}));
    tools.append(toolDef(
        "set_tool_option",
        "Set one tool option (options bar follows, like a user drag). "
        "Colors take #rrggbb, combos take their text.",
        QJsonObject{
            {"tool", QJsonObject{{"type", QStringLiteral("string")}}},
            {"id", QJsonObject{{"type", QStringLiteral("string")}}},
            {"value", QJsonObject{{"description",
                                   QStringLiteral("number, boolean, text or color")}}}},
        {"tool", "id", "value"}));
    tools.append(toolDef(
        "list_brushes",
        "Brush presets (factory plus custom) with the active marker.",
        QJsonObject{}));
    tools.append(toolDef(
        "select_brush",
        "Load a brush preset onto its tool (full known state).",
        QJsonObject{
            {"name", QJsonObject{{"type", QStringLiteral("string")}}}},
        {"name"}));
    tools.append(toolDef(
        "paint_stroke",
        "Round-brush stroke on the active pixel layer (own undo step): "
        "gap-filled dabs along points [[x,y],..] in document pixels. "
        "Optional per-point pressures scale size and opacity. Normal "
        "blend, ignores the active selection.",
        QJsonObject{
            {"points",
             QJsonObject{
                 {"type", QStringLiteral("array")},
                 {"items",
                  QJsonObject{
                      {"type", QStringLiteral("array")},
                      {"items",
                       QJsonObject{{"type", QStringLiteral("number")}}},
                      {"minItems", 2},
                      {"maxItems", 2}}}}},
            {"radius",
             QJsonObject{{"type", QStringLiteral("number")},
                         {"default", 16},
                         {"minimum", 1},
                         {"maximum", 2000}}},
            {"color",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"default", "#000000"}}},
            {"hardness",
             QJsonObject{{"type", QStringLiteral("number")},
                         {"default", 50},
                         {"minimum", 0},
                         {"maximum", 100}}},
            {"opacity",
             QJsonObject{{"type", QStringLiteral("number")},
                         {"default", 100},
                         {"minimum", 0},
                         {"maximum", 100}}},
            {"spacing",
             QJsonObject{{"type", QStringLiteral("number")},
                         {"default", 25},
                         {"description",
                          QStringLiteral("percent of diameter")}}},
            {"pressures",
             QJsonObject{
                 {"type", QStringLiteral("array")},
                 {"items", QJsonObject{{"type", QStringLiteral("number")}}},
                 {"description",
                  QStringLiteral("0..1 per point, defaults to 1")}}}},
        {"points"}));
    tools.append(toolDef(
        "mix_colors",
        "Painter's mixer: two CSS colors meet at ratio t (or a ramp of "
        "steps swatches) in Oklab perceptual space by default — linear "
        "for physical light mixes, srgb for naive ones. Pure math, needs "
        "no document.",
        QJsonObject{
            {"a", QJsonObject{{"type", QStringLiteral("string")}}},
            {"b", QJsonObject{{"type", QStringLiteral("string")}}},
            {"t",
             QJsonObject{{"type", QStringLiteral("number")},
                         {"default", 0.5},
                         {"minimum", 0},
                         {"maximum", 1}}},
            {"steps",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"minimum", 2},
                         {"maximum", 16},
                         {"description",
                          QStringLiteral("ramp swatches instead of one mix")}}},
            {"space",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"default", "oklab"},
                         {"enum", QJsonArray{"oklab", "linear", "srgb"}}}}},
        {"a", "b"}));
    tools.append(toolDef(
        "list_filters",
        "Every engine filter with its parameters (id, name, category, "
        "min/max/defaults). Run one with apply_filter.",
        QJsonObject{}));
    tools.append(toolDef(
        "undo", "Undo the last history step. Nothing happens when the "
                "stack is empty.",
        QJsonObject{}));
    tools.append(toolDef(
        "redo", "Redo the last undone step.",
        QJsonObject{}));
    tools.append(toolDef(
        "set_active_layer",
        "Select a layer of the active document (view state, no undo).",
        QJsonObject{
            {"layer",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"description",
                          QStringLiteral("panel index, top-first")}}}},
        {"layer"}));
    tools.append(toolDef(
        "add_text",
        "Live text layer on the active document (own undo step) at "
        "document position x/y with point size, CSS color and optional "
        "font family.",
        QJsonObject{
            {"text", QJsonObject{{"type", QStringLiteral("string")}}},
            {"x", QJsonObject{{"type", QStringLiteral("number")},
                              {"default", 0}}},
            {"y", QJsonObject{{"type", QStringLiteral("number")},
                              {"default", 0}}},
            {"size",
             QJsonObject{{"type", QStringLiteral("number")},
                         {"default", 48},
                         {"minimum", 1},
                         {"maximum", 1000}}},
            {"color",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"default", "#ffffff"}}},
            {"family", QJsonObject{{"type", QStringLiteral("string")}}}},
        {"text"}));
    tools.append(toolDef(
        "save_project",
        "Save the active document's project file to path.",
        QJsonObject{
            {"path", QJsonObject{{"type", QStringLiteral("string")}}}},
        {"path"}));
    tools.append(toolDef(
        "open_image",
        "Open an image file as a new document and make it active.",
        QJsonObject{
            {"path", QJsonObject{{"type", QStringLiteral("string")}}}},
        {"path"}));
    tools.append(toolDef(
        "set_layer_visible",
        "Show or hide one layer of the active document (own undo step).",
        QJsonObject{
            {"layer",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"description",
                          QStringLiteral("panel index, top-first")}}},
            {"visible", QJsonObject{{"type", QStringLiteral("boolean")}}}},
        {"layer", "visible"}));
    tools.append(toolDef(
        "new_document",
        "Create a document and make it active. Sizes in pixels.",
        QJsonObject{
            {"title", QJsonObject{{"type", QStringLiteral("string")}}},
            {"width",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"minimum", 1},
                         {"maximum", 16384}}},
            {"height",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"minimum", 1},
                         {"maximum", 16384}}},
            {"dpi",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"default", 72}}}},
        {"width", "height"}));
    tools.append(toolDef(
        "add_shape",
        "Painted vector shape layer on the active document (own undo "
        "step): rectangle, ellipse or triangle at rect [x, y, w, h] in "
        "document pixels with a CSS fill color. Lands above the active "
        "layer.",
        QJsonObject{
            {"shape",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"enum", QJsonArray{"rectangle", "ellipse",
                                             "triangle"}}}},
            {"rect",
             QJsonObject{
                 {"type", QStringLiteral("array")},
                 {"items", QJsonObject{{"type", QStringLiteral("number")}}},
                 {"minItems", 4},
                 {"maxItems", 4},
                 {"description",
                  QStringLiteral("[x, y, w, h] in document pixels")}}},
            {"fill",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"description",
                          QStringLiteral("CSS color, e.g. #ff0000")}}}},
        {"shape", "rect", "fill"}));
    tools.append(toolDef(
        "apply_filter",
        "Run a named engine filter on the active layer (own undo step, "
        "like the Filter Gallery). Params override the filter defaults "
        "positionally; omit for defaults.",
        QJsonObject{
            {"filter",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"description",
                          QStringLiteral(
                              "filter id, e.g. clouds, gaussian_blur, "
                              "sponge, add_noise")}}},
            {"params",
             QJsonObject{
                 {"type", QStringLiteral("array")},
                 {"items", QJsonObject{{"type", QStringLiteral("number")}}},
                 {"description",
                  QStringLiteral("positional params, defaults when omitted")}}}},
        {"filter"}));
    tools.append(toolDef(
        "export_png",
        "Write the active document's rendered composite to a PNG file. "
        "maxDim optionally downscales the long side first.",
        QJsonObject{
            {"path",
             QJsonObject{{"type", QStringLiteral("string")},
                         {"description",
                          QStringLiteral("destination file path")}}},
            {"maxDim",
             QJsonObject{{"type", QStringLiteral("integer")},
                         {"minimum", 16},
                         {"maximum", 4096}}}},
        {"path"}));
    return QJsonObject{{"tools", tools}};
}

QJsonObject handleCall(const QString& name, const QJsonObject& args,
                       QString* error) {
    if (name == QStringLiteral("ping")) {
        const QJsonObject r = bridgeCall("ping", {}, error);
        if (r.isEmpty() && !error->isEmpty()) return {};
        return QJsonObject{{"content", QJsonArray{QJsonObject{
                                             {"type", QStringLiteral("text")},
                                             {"text", QString::fromUtf8(
                                                          QJsonDocument(r).toJson(
                                                              QJsonDocument::Compact))}}}}};
    }
    if (name == QStringLiteral("composite_png")) {
        QJsonObject params;
        if (args.contains("maxDim"))
            params["maxDim"] = args.value("maxDim").toInt(768);
        const QJsonObject r = bridgeCall("composite_png", params, error);
        if (r.isEmpty() && !error->isEmpty()) return {};
        QJsonArray content;
        content.append(QJsonObject{
            {"type", QStringLiteral("text")},
            {"text", QStringLiteral("composite %1x%2")
                         .arg(r.value("width").toInt())
                         .arg(r.value("height").toInt())}});
        content.append(
            QJsonObject{{"type", QStringLiteral("image")},
                        {"data", r.value("png_base64").toString()},
                        {"mimeType", QStringLiteral("image/png")}});
        return QJsonObject{{"content", content}};
    }
    // Straightforward JSON tools: bridge params are the MCP arguments.
    QString cmd;
    if (name == QStringLiteral("list_documents"))
        cmd = QStringLiteral("list_documents");
    else if (name == QStringLiteral("document_info"))
        cmd = QStringLiteral("document_info");
    else if (name == QStringLiteral("set_layer_visible"))
        cmd = QStringLiteral("set_layer_visible");
    else if (name == QStringLiteral("list_filters"))
        cmd = QStringLiteral("list_filters");
    else if (name == QStringLiteral("undo"))
        cmd = QStringLiteral("undo");
    else if (name == QStringLiteral("redo"))
        cmd = QStringLiteral("redo");
    else if (name == QStringLiteral("set_active_layer"))
        cmd = QStringLiteral("set_active_layer");
    else if (name == QStringLiteral("add_text"))
        cmd = QStringLiteral("add_text");
    else if (name == QStringLiteral("save_project"))
        cmd = QStringLiteral("save_project");
    else if (name == QStringLiteral("open_image"))
        cmd = QStringLiteral("open_image");
    else if (name == QStringLiteral("mix_colors"))
        cmd = QStringLiteral("mix_colors");
    else if (name == QStringLiteral("list_tools"))
        cmd = QStringLiteral("list_tools");
    else if (name == QStringLiteral("list_tool_options"))
        cmd = QStringLiteral("list_tool_options");
    else if (name == QStringLiteral("set_tool_option"))
        cmd = QStringLiteral("set_tool_option");
    else if (name == QStringLiteral("list_brushes"))
        cmd = QStringLiteral("list_brushes");
    else if (name == QStringLiteral("select_brush"))
        cmd = QStringLiteral("select_brush");
    else if (name == QStringLiteral("paint_stroke"))
        cmd = QStringLiteral("paint_stroke");
    else if (name == QStringLiteral("set_layer_blend"))
        cmd = QStringLiteral("set_layer_blend");
    else if (name == QStringLiteral("set_layer_opacity"))
        cmd = QStringLiteral("set_layer_opacity");
    else if (name == QStringLiteral("new_document"))
        cmd = QStringLiteral("new_document");
    else if (name == QStringLiteral("add_shape"))
        cmd = QStringLiteral("add_shape");
    else if (name == QStringLiteral("export_png"))
        cmd = QStringLiteral("export_png");
    else if (name == QStringLiteral("apply_filter"))
        cmd = QStringLiteral("apply_filter");
    else {
        *error = QStringLiteral("unknown tool '%1'").arg(name);
        return {};
    }
    const QJsonObject r = bridgeCall(cmd, args, error);
    if (r.isEmpty() && !error->isEmpty()) return {};
    return QJsonObject{{"content", QJsonArray{QJsonObject{
                                         {"type", QStringLiteral("text")},
                                         {"text", QString::fromUtf8(
                                                      QJsonDocument(r).toJson(
                                                          QJsonDocument::Compact))}}}}};
}

QJsonObject handleMessage(const QJsonObject& msg) {
    const QJsonValue id = msg.value("id");
    const QString method = msg.value("method").toString();
    const QJsonObject params = msg.value("params").toObject();
    // Notifications carry no id and get no reply.
    const bool isNotification = id.isNull() || id.isUndefined();
    if (method == QStringLiteral("initialize")) {
        if (isNotification) return {};
        return rpcOk(id, QJsonObject{
                              {"protocolVersion",
                               QString::fromLatin1(kProtocolVersion)},
                              {"capabilities",
                               QJsonObject{{"tools", QJsonObject{}}}},
                              {"serverInfo",
                               QJsonObject{
                                   {"name", QStringLiteral("pittore-studio-mcp")},
                                   {"version", QStringLiteral("0.1.1")},
                               }},
                          });
    }
    if (method == QStringLiteral("ping")) {
        if (isNotification) return {};
        return rpcOk(id, QJsonObject{});
    }
    if (method == QStringLiteral("tools/list")) {
        if (isNotification) return {};
        return rpcOk(id, listTools());
    }
    if (method == QStringLiteral("tools/call")) {
        if (isNotification) return {};
        QString error;
        const QJsonObject r = handleCall(params.value("name").toString(),
                                         params.value("arguments").toObject(),
                                         &error);
        if (!error.isEmpty())
            return rpcError(id, -32001, error);
        return rpcOk(id, r);
    }
    if (isNotification) return {};  // unknown notification: ignore
    return rpcError(id, -32601,
                    QStringLiteral("unknown method '%1'").arg(method));
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream in(stdin);
    QTextStream out(stdout);
    QString line;
    while (in.readLineInto(&line)) {
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            out << QJsonDocument(rpcError(QJsonValue::Null, -32700,
                                          QStringLiteral("parse error")))
                       .toJson(QJsonDocument::Compact)
                << "\n";
            out.flush();
            continue;
        }
        const QJsonObject resp = handleMessage(doc.object());
        if (resp.isEmpty()) continue;  // notification: no reply
        out << QJsonDocument(resp).toJson(QJsonDocument::Compact) << "\n";
        out.flush();
    }
    return 0;
}
