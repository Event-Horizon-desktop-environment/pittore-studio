// Live-session MCP bridge endpoint: local-socket JSON command server for
// the pittore-mcp shim. Protocol is newline-delimited JSON, one request per
// line: {"id":1,"cmd":"ping","params":{}} -> {"id":1,"ok":true,"result":{}}
// or {"id":1,"ok":false,"error":"..."} (id echoed back verbatim; a missing
// id gets null). Malformed lines are ignored, never answered.
#include "ui/mcp/mcp_bridge.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "engine/compute/brushes/dab/dab.h"
#include "engine/compute/oklab.h"
#include "engine/filter/filters.h"
#include "engine/filter/registry/filter_registry.h"

#include "engine/core/log.h"
#include "ui/app_state.h"
#include "ui/brushes/brush_library.h"
#include "ui/tools/defs/tool_defs.h"
#include "ui/tools/options/tool_options.h"

namespace pittore::ui {

QString mcpSocketPath() {
    // Runtime dir is per-user 0700 on a normal system, so the socket is
    // only reachable by this user. Temp dir is the fallback where the
    // runtime dir is unavailable (some sandboxes).
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(
            QStandardPaths::TempLocation);
    return base + QStringLiteral("/pittore-studio-mcp.sock");
}

McpBridge::McpBridge(AppState* state, QObject* parent)
    : QObject(parent), state_(state) {}

bool McpBridge::start(const QString& socketPath) {
    if (server_) return true;
    socketPath_ = socketPath.isEmpty() ? mcpSocketPath() : socketPath;
    QLocalServer::removeServer(socketPath_);  // stale socket from a crash
    server_ = new QLocalServer(this);
    connect(server_, &QLocalServer::newConnection, this,
            &McpBridge::onNewConnection);
    if (!server_->listen(socketPath_)) {
        ::pittore::core::log::log_warning(
            "[mcp] bridge unavailable (socket '%s' busy): AI bridge disabled",
            socketPath_.toLocal8Bit().constData());
        server_->deleteLater();
        server_ = nullptr;
        return false;
    }
    ::pittore::core::log::log_info("[mcp] bridge listening on '%s'",
                                   socketPath_.toLocal8Bit().constData());
    return true;
}

QString McpBridge::socketPath() const { return socketPath_; }

void McpBridge::onNewConnection() {
    while (server_ && server_->hasPendingConnections()) {
        QLocalSocket* sock = server_->nextPendingConnection();
        connect(sock, &QLocalSocket::readyRead, this,
                [this, sock] { onReadyRead(sock); });
        connect(sock, &QLocalSocket::disconnected, this,
                [this, sock] { onClientGone(sock); });
    }
}

void McpBridge::onClientGone(QObject* sock) {
    auto* s = static_cast<QLocalSocket*>(sock);
    pending_.remove(s);
    s->deleteLater();
}

void McpBridge::onReadyRead(QLocalSocket* sock) {
    pending_[sock] += sock->readAll();
    QByteArray& buf = pending_[sock];
    for (;;) {
        const int nl = buf.indexOf('\n');
        if (nl < 0) return;
        const QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError perr{};
        const QJsonDocument doc =
            QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            continue;  // malformed: ignore, keep serving this client
        const QJsonObject resp = dispatch(doc.object());
        sock->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        sock->write("\n", 1);
        sock->flush();
    }
}

namespace {

QJsonObject errResp(const QJsonValue& id, const QString& msg) {
    return QJsonObject{{"id", id}, {"ok", false}, {"error", msg}};
}

QJsonObject okResp(const QJsonValue& id, const QJsonObject& result) {
    return QJsonObject{{"id", id}, {"ok", true}, {"result", result}};
}

// Painter's mixer: two sRGB colors meet in a perceptual (Oklab), physical
// (linear) or naive (sRGB) space. sRGB transfer functions are the textbook
// IEC 61966-2-1 piecewise curves.
float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c) {
    return c <= 0.0031308f
               ? 12.92f * c
               : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

QString mixQColors(const QColor& a, const QColor& b, double t,
                   const QString& space) {
    const float fa[3] = {a.redF(), a.greenF(), a.blueF()};
    const float fb[3] = {b.redF(), b.greenF(), b.blueF()};
    float ca[3], cb[3];
    if (space == QStringLiteral("srgb")) {
        for (int k = 0; k < 3; ++k) {
            ca[k] = fa[k];
            cb[k] = fb[k];
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            ca[k] = srgbToLinear(fa[k]);
            cb[k] = srgbToLinear(fb[k]);
        }
    }
    float la[3], lb[3];
    if (space == QStringLiteral("oklab")) {
        float L, u, v;
        pittore::compute::oklab::rgb_to_oklab(ca[0], ca[1], ca[2], L, u, v);
        la[0] = L;
        la[1] = u;
        la[2] = v;
        pittore::compute::oklab::rgb_to_oklab(cb[0], cb[1], cb[2], L, u, v);
        lb[0] = L;
        lb[1] = u;
        lb[2] = v;
    } else {
        for (int k = 0; k < 3; ++k) {
            la[k] = ca[k];
            lb[k] = cb[k];
        }
    }
    const float k = (float)std::clamp(t, 0.0, 1.0);
    float m[3] = {la[0] + (lb[0] - la[0]) * k, la[1] + (lb[1] - la[1]) * k,
                  la[2] + (lb[2] - la[2]) * k};
    float rgb[3];
    if (space == QStringLiteral("oklab")) {
        float r, g, b2;
        pittore::compute::oklab::oklab_to_rgb(m[0], m[1], m[2], r, g, b2);
        pittore::compute::oklab::gamut_map(r, g, b2);
        rgb[0] = r;
        rgb[1] = g;
        rgb[2] = b2;
    } else {
        for (int k2 = 0; k2 < 3; ++k2) rgb[k2] = m[k2];
    }
    float out[3];
    if (space == QStringLiteral("srgb")) {
        for (int k2 = 0; k2 < 3; ++k2) out[k2] = rgb[k2];
    } else {
        for (int k2 = 0; k2 < 3; ++k2)
            out[k2] = linearToSrgb(std::clamp(rgb[k2], 0.0f, 1.0f));
    }
    return QColor::fromRgbF(std::clamp(out[0], 0.0f, 1.0f),
                            std::clamp(out[1], 0.0f, 1.0f),
                            std::clamp(out[2], 0.0f, 1.0f))
        .name(QColor::HexRgb);
}

// Tool reference by menu name or icon key ("brush", "Eraser", "Clone
// Stamp", ...), case-insensitive.
std::optional<ToolId> resolveTool(const QString& s) {
    const QString want = s.trimmed().toLower();
    if (want.isEmpty()) return std::nullopt;
    for (const ToolDef& t : allTools()) {
        if (QString::fromUtf8(t.iconKey).toLower() == want ||
            QString::fromUtf8(t.name).toLower() == want)
            return t.id;
    }
    return std::nullopt;
}

const char* optionKindName(OptionKind kind) {
    switch (kind) {
        case OptionKind::Label: return "label";
        case OptionKind::Slider: return "slider";
        case OptionKind::Spin: return "spin";
        case OptionKind::Combo: return "combo";
        case OptionKind::Check: return "check";
        case OptionKind::ColorWell: return "color";
        case OptionKind::ToggleGroup: return "togglegroup";
        case OptionKind::Button: return "button";
        case OptionKind::BrushPreset: return "brushpreset";
        case OptionKind::Text: return "text";
        case OptionKind::Separator: return "separator";
    }
    return "label";
}

// Option values into JSON: ints stay ints, colors become #rrggbb(aa),
// everything else falls back to its display string.
QJsonValue variantToJson(const QVariant& v) {
    switch (v.userType()) {
        case QMetaType::Bool: return QJsonValue(v.toBool());
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong: return QJsonValue(v.toLongLong());
        case QMetaType::Double:
        case QMetaType::Float: return QJsonValue(v.toDouble());
        case QMetaType::QString: return QJsonValue(v.toString());
        case QMetaType::QColor: {
            const QColor c = v.value<QColor>();
            return QJsonValue(c.alpha() == 255
                                  ? c.name(QColor::HexRgb)
                                  : c.name(QColor::HexArgb));
        }
        default: break;
    }
    if (v.userType() == QMetaType::QStringList ||
        v.userType() == QMetaType::QVariantList) {
        QJsonArray arr;
        for (const QVariant& e : v.toList()) arr.append(variantToJson(e));
        return arr;
    }
    return QJsonValue(v.toString());
}

QString layerKindName(LayerItem::Kind kind) {
    switch (kind) {
        case LayerItem::Kind::Pixel: return QStringLiteral("pixel");
        case LayerItem::Kind::Text: return QStringLiteral("text");
        case LayerItem::Kind::Shape: return QStringLiteral("shape");
        case LayerItem::Kind::Adjustment: return QStringLiteral("adjustment");
        case LayerItem::Kind::Group: return QStringLiteral("group");
        case LayerItem::Kind::SmartObject: return QStringLiteral("smartobject");
        case LayerItem::Kind::Frame: return QStringLiteral("frame");
    }
    return QStringLiteral("pixel");
}

}  // namespace

QJsonObject McpBridge::dispatch(const QJsonObject& req) {
    const QJsonValue id = req.value("id");
    const QString cmd = req.value("cmd").toString();
    const QJsonObject params = req.value("params").toObject();
    if (!state_) return errResp(id, QStringLiteral("no app state"));
    AppState& state = *state_;

    if (cmd == QStringLiteral("ping")) {
        return okResp(id, QJsonObject{{"app", QStringLiteral("pittore-studio")},
                                      {"version", QStringLiteral("0.1.1")}});
    }
    if (cmd == QStringLiteral("list_documents")) {
        QJsonArray docs;
        const auto& all = state.documents();
        for (int i = 0; i < all.size(); ++i) {
            const DocumentItem* d = all[i];
            if (!d) continue;
            docs.append(QJsonObject{{"index", i},
                                    {"title", d->title},
                                    {"width", d->size.width()},
                                    {"height", d->size.height()}});
        }
        return okResp(id, QJsonObject{{"documents", docs}});
    }
    if (cmd == QStringLiteral("document_info")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        QJsonArray layers;
        for (int i = 0; i < d->layers.size(); ++i) {
            const LayerItem& l = d->layers[i];
            layers.append(QJsonObject{{"index", i},
                                      {"name", l.name},
                                      {"kind", layerKindName(l.kind)},
                                      {"visible", l.visible},
                                      {"opacity", l.opacity},
                                      {"indent", l.indent},
                                      {"clipped", l.clipped},
                                      {"hasMask", l.hasMask},
                                      {"offsetX", l.offset.x()},
                                      {"offsetY", l.offset.y()}});
        }
        return okResp(id, QJsonObject{
                                 {"title", d->title},
                                 {"width", d->size.width()},
                                 {"height", d->size.height()},
                                 {"dpi", d->dpi},
                                 {"activeLayer", d->activeLayer},
                                 {"layers", layers},
                             });
    }
    if (cmd == QStringLiteral("composite_png")) {
        DocumentItem* d = state.activeDocument();
        if (!d || d->composite.isNull())
            return errResp(id, QStringLiteral("no rendered composite"));
        const int maxDim =
            std::clamp(params.value("maxDim").toInt(768), 16, 2048);
        QImage img = d->composite.scaled(
            maxDim, maxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        if (!img.save(&buf, "PNG"))
            return errResp(id, QStringLiteral("png encode failed"));
        return okResp(id, QJsonObject{
                                 {"width", img.width()},
                                 {"height", img.height()},
                                 {"png_base64",
                                  QString::fromLatin1(bytes.toBase64())},
                             });
    }
    if (cmd == QStringLiteral("set_layer_visible")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        if (!params.contains("layer") || !params.contains("visible"))
            return errResp(
                id, QStringLiteral("need {\"layer\": int, \"visible\": bool}"));
        const int layer = params.value("layer").toInt(-1);
        const bool visible = params.value("visible").toBool();
        if (layer < 0 || layer >= d->layers.size())
            return errResp(id, QStringLiteral("layer index out of range"));
        // Undo-safe panel entry point (own undo step, region recomposite).
        const bool changed = state.setLayersVisible({layer}, visible);
        return okResp(id, QJsonObject{{"changed", changed}});
    }
    if (cmd == QStringLiteral("new_document")) {
        const int w = params.value("width").toInt(0);
        const int h = params.value("height").toInt(0);
        const int dpi = params.value("dpi").toInt(72);
        if (w < 1 || h < 1 || w > 16384 || h > 16384)
            return errResp(id, QStringLiteral("need 1..16384 width/height"));
        if (dpi < 1 || dpi > 1200)
            return errResp(id, QStringLiteral("need 1..1200 dpi"));
        const QString title = params.value("title").toString(
            QStringLiteral("Untitled"));
        DocumentItem* d =
            state.addDocument(title, QSize(w, h), dpi);
        if (!d) return errResp(id, QStringLiteral("could not create document"));
        return okResp(id, QJsonObject{
                                 {"index", state.documents().indexOf(d)},
                                 {"title", d->title},
                                 {"width", d->size.width()},
                                 {"height", d->size.height()},
                             });
    }
    if (cmd == QStringLiteral("add_shape")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        const QString shape = params.value("shape").toString().toLower();
        ToolId tool = ToolId::Rectangle;
        if (shape == QStringLiteral("rectangle"))
            tool = ToolId::Rectangle;
        else if (shape == QStringLiteral("ellipse"))
            tool = ToolId::Ellipse;
        else if (shape == QStringLiteral("triangle"))
            tool = ToolId::Triangle;
        else
            return errResp(id, QStringLiteral(
                                    "shape must be rectangle, ellipse or triangle"));
        const QJsonArray rect = params.value("rect").toArray();
        if (rect.size() != 4)
            return errResp(id, QStringLiteral("need rect [x, y, w, h]"));
        const QRectF box(rect[0].toDouble(), rect[1].toDouble(),
                         rect[2].toDouble(), rect[3].toDouble());
        if (!(box.width() > 0.0) || !(box.height() > 0.0))
            return errResp(id, QStringLiteral("rect needs positive size"));
        const QColor fill(params.value("fill").toString());
        if (!fill.isValid())
            return errResp(id, QStringLiteral("fill must be a color like #ff0000"));
        // Silent option set: the value is read directly, no panel churn.
        state.setOptionSilently(tool, QStringLiteral("fill"), fill);
        if (!state.addVectorShapeLayer(tool, box, QStringLiteral("MCP shape")))
            return errResp(id, QStringLiteral("shape refused (see status hint)"));
        return okResp(id, QJsonObject{{"index", d->activeLayer}});
    }
    if (cmd == QStringLiteral("set_layer_blend")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        if (!params.contains("layer") || !params.contains("mode"))
            return errResp(
                id, QStringLiteral("need {\"layer\": int, \"mode\": str}"));
        const int layer = params.value("layer").toInt(-1);
        QString mode = params.value("mode").toString();
        if (layer < 0 || layer >= d->layers.size())
            return errResp(id, QStringLiteral("layer index out of range"));
        // The imported display alias for Linear Dodge (Add).
        if (mode == QStringLiteral("Add"))
            mode = QStringLiteral("Linear Dodge (Add)");
        if (!blendModeNames().contains(mode))
            return errResp(id, QStringLiteral("unknown blend mode '%1'").arg(
                                    params.value("mode").toString()));
        if (d->layers[layer].blendMode == mode)
            return okResp(id, QJsonObject{{"changed", false}});
        // Own undo step (the panel's live combo skips this; a remote
        // mutation must stay undoable like every other model edit).
        state.beginUndoStep();
        d->layers[layer].blendMode = mode;
        d->rebuildComposite();
        state.commitUndoStep(QStringLiteral("Blend %1").arg(mode),
                             QStringLiteral("blend"));
        emit state.documentModified(d);
        return okResp(id, QJsonObject{{"changed", true}});
    }
    if (cmd == QStringLiteral("list_filters")) {
        QJsonArray filters;
        for (const auto& def : pittore::filter::allFilterDefs()) {
            QJsonArray params;
            for (const auto& p : def.params) {
                params.append(
                    QJsonObject{{"id", QString::fromUtf8(p.id)},
                                {"label", QString::fromUtf8(p.label)},
                                {"min", p.min},
                                {"max", p.max},
                                {"default", p.def},
                                {"suffix", QString::fromUtf8(p.suffix)}});
            }
            filters.append(QJsonObject{
                {"id", QString::fromUtf8(def.id)},
                {"name", QString::fromUtf8(def.name)},
                {"category", QString::fromUtf8(def.category)},
                {"params", params}});
        }
        return okResp(id, QJsonObject{{"filters", filters}});
    }
    if (cmd == QStringLiteral("list_tools")) {
        QJsonArray tools;
        for (const ToolDef& t : allTools()) {
            tools.append(QJsonObject{
                {"key", QString::fromUtf8(t.iconKey)},
                {"name", QString::fromUtf8(t.name)},
                {"shortcut", toolShortcutText(t.id).isEmpty()
                                 ? QJsonValue::Null
                                 : QJsonValue(toolShortcutText(t.id))}});
        }
        return okResp(id, QJsonObject{{"tools", tools}});
    }
    if (cmd == QStringLiteral("list_tool_options")) {
        const auto tool = resolveTool(params.value("tool").toString());
        if (!tool) return errResp(id, QStringLiteral("unknown tool"));
        QJsonArray opts;
        for (const OptionSpec& spec : optionsFor(*tool)) {
            if (QString::fromUtf8(spec.id).isEmpty()) continue;  // labels
            QJsonObject o{{"id", QString::fromUtf8(spec.id)},
                          {"label", QString::fromUtf8(spec.label)},
                          {"kind", QString::fromUtf8(optionKindName(spec.kind))},
                          {"min", spec.min},
                          {"max", spec.max},
                          {"default",
                           variantToJson(spec.defaultValue)},
                          {"value", variantToJson(state.option(*tool, QString::fromUtf8(spec.id)))}};
            if (!spec.items.empty()) {
                QJsonArray items;
                for (const char* item : spec.items)
                    items.append(QString::fromUtf8(item));
                o["items"] = items;
            }
            if (spec.suffix[0] != '\0')
                o["suffix"] = QString::fromUtf8(spec.suffix);
            opts.append(o);
        }
        return okResp(id, QJsonObject{{"options", opts}});
    }
    if (cmd == QStringLiteral("set_tool_option")) {
        const auto tool = resolveTool(params.value("tool").toString());
        if (!tool) return errResp(id, QStringLiteral("unknown tool"));
        const QString optId = params.value("id").toString();
        if (optId.isEmpty()) return errResp(id, QStringLiteral("need id"));
        const QVariant current = state.option(*tool, optId);
        if (!current.isValid())
            return errResp(id, QStringLiteral("unknown option '%1'").arg(optId));
        // Convert into the live value's own type (numbers accept numbers,
        // colors accept "#rrggbb", combos accept text): a wrong shape is
        // an honest error, never a silent mis-set.
        QVariant v;
        const QJsonValue given = params.value("value");
        if (current.userType() == QMetaType::QColor) {
            const QColor c(given.toString());
            if (!c.isValid())
                return errResp(id, QStringLiteral("need a color like #ff0000"));
            v = c;
        } else if (current.userType() == QMetaType::Bool) {
            if (!given.isBool())
                return errResp(id, QStringLiteral("need true/false"));
            v = given.toBool();
        } else if (given.isString()) {
            v = QString(given.toString());
            if (!v.convert(QMetaType(current.userType())))
                return errResp(id, QStringLiteral("bad value"));
        } else if (given.isDouble()) {
            v = given.toDouble();
            if (!v.convert(QMetaType(current.userType())))
                return errResp(id, QStringLiteral("bad value"));
        } else {
            return errResp(id, QStringLiteral("bad value"));
        }
        // Loud set: the options bar follows, like a user drag.
        state.setOption(*tool, optId, v);
        return okResp(id, QJsonObject{{"value",
                                       variantToJson(state.option(*tool, optId))}});
    }
    if (cmd == QStringLiteral("list_brushes")) {
        QJsonArray presets;
        const QString active = state.activeBrushPresetName();
        auto push = [&](const std::vector<brushlibrary::BrushPreset>& ps) {
            for (const brushlibrary::BrushPreset& p : ps) {
                presets.append(QJsonObject{{"name", p.name},
                                           {"active", p.name == active}});
            }
        };
        push(brushlibrary::factoryBrushPresets());
        push(brushlibrary::loadCustomPresets());
        return okResp(id, QJsonObject{{"brushes", presets}});
    }
    if (cmd == QStringLiteral("select_brush")) {
        const QString want = params.value("name").toString();
        if (want.isEmpty()) return errResp(id, QStringLiteral("need name"));
        // Copy out: factory/custom return by value, so no borrowed refs.
        std::optional<brushlibrary::BrushPreset> hit;
        for (const brushlibrary::BrushPreset& p :
             brushlibrary::factoryBrushPresets()) {
            if (p.name.compare(want, Qt::CaseInsensitive) == 0) {
                hit = p;
                break;
            }
        }
        if (!hit) {
            for (const brushlibrary::BrushPreset& p :
                 brushlibrary::loadCustomPresets()) {
                if (p.name.compare(want, Qt::CaseInsensitive) == 0) {
                    hit = p;
                    break;
                }
            }
        }
        if (!hit)
            return errResp(id, QStringLiteral("unknown brush '%1'").arg(want));
        const brushlibrary::ApplyResult r = brushlibrary::applyPreset(&state, *hit);
        if (r != brushlibrary::ApplyResult::Applied &&
            r != brushlibrary::ApplyResult::StampMissing)
            return errResp(id, QStringLiteral("preset refused"));
        return okResp(id, QJsonObject{{"applied", hit->name}});
    }
    if (cmd == QStringLiteral("paint_stroke")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        const QJsonArray pts = params.value("points").toArray();
        if (pts.size() < 1)
            return errResp(id, QStringLiteral("need points [[x,y],..]"));
        if (pts.size() > 2000)
            return errResp(id, QStringLiteral("too many points (max 2000)"));
        const double radius = params.value("radius").toDouble(16.0);
        if (!(radius > 0.0) || radius > 2000.0)
            return errResp(id, QStringLiteral("need 0 < radius <= 2000"));
        const QColor color(params.value("color").toString(QStringLiteral("#000000")));
        if (!color.isValid())
            return errResp(id, QStringLiteral("color must be like #ff0000"));
        const double hardness =
            std::clamp(params.value("hardness").toDouble(50.0), 0.0, 100.0);
        const double opacity =
            std::clamp(params.value("opacity").toDouble(100.0), 0.0, 100.0);
        const double spacingPct =
            std::clamp(params.value("spacing").toDouble(25.0), 1.0, 1000.0);
        std::vector<QPointF> path;
        path.reserve((size_t)pts.size());
        for (const QJsonValue& v : pts) {
            const QJsonArray p = v.toArray();
            if (p.size() != 2)
                return errResp(id, QStringLiteral("each point is [x, y]"));
            path.emplace_back(p[0].toDouble(), p[1].toDouble());
        }
        std::vector<double> pressure(path.size(), 1.0);
        const QJsonArray pr = params.value("pressures").toArray();
        if (!pr.isEmpty()) {
            if (pr.size() != (int)path.size())
                return errResp(id, QStringLiteral("pressures must match points"));
            for (size_t i = 0; i < path.size(); ++i)
                pressure[i] = std::clamp(pr[(int)i].toDouble(1.0), 0.0, 1.0);
        }
        // Gap-filled round-brush dabs along the polyline, one undo step.
        // Placement is captured up front (edit sees only the pixel buffer).
        LayerItem* layer = state.activeLayer();
        if (!layer) return errResp(id, QStringLiteral("no active layer"));
        const QPointF off = layer->offset;
        const double sx = layer->scaleX != 0.0 ? layer->scaleX : 1.0;
        const double sy = layer->scaleY != 0.0 ? layer->scaleY : 1.0;
        const pittore::RGBAf ink{color.redF(), color.greenF(),
                                 color.blueF(), 1.0f};
        const float hard = (float)(hardness / 100.0);
        const float op = (float)(opacity / 100.0);
        const double step =
            std::max(1.0, 2.0 * radius * spacingPct / 100.0);
        struct Dab {
            float x, y, r, o;
        };
        std::vector<Dab> dabs;
        dabs.reserve(1024);
        auto emitDab = [&](const QPointF& doc, double p) {
            if (!(p > 0.0)) return true;
            if (dabs.size() >= 10000) return false;
            dabs.push_back(
                Dab{(float)((doc.x() - off.x()) / sx),
                    (float)((doc.y() - off.y()) / sy),
                    (float)(radius * p / ((sx + sy) * 0.5)),
                    (float)(op * p)});
            return true;
        };
        for (size_t i = 0; i < path.size(); ++i) {
            const QPointF a = path[i];
            const double pa = pressure[i];
            if (i == 0) {
                if (!emitDab(a, pa))
                    return errResp(id, QStringLiteral("stroke too long"));
                continue;
            }
            const QPointF b = path[i - 1];
            const double dist = QLineF(b, a).length();
            const int steps = std::max(1, int(std::ceil(dist / step)));
            for (int s = 1; s <= steps; ++s) {
                const double t = double(s) / double(steps);
                const QPointF p = b + (a - b) * t;
                const double prs =
                    pressure[i - 1] + (pa - pressure[i - 1]) * t;
                if (!emitDab(p, prs))
                    return errResp(id, QStringLiteral("stroke too long"));
            }
        }
        if (dabs.empty()) return errResp(id, QStringLiteral("nothing to paint"));
        // Gallery one-shot: undo step, copy-on-write, recomposite.
        // Touch pressure is per-dab above; the selection is not honored
        // (documented): agent strokes paint unmasked.
        const bool ok = state.applyLayerEditOneShot(
            [&](pittore::Image& img) {
                const std::uint32_t w = img.width(), h = img.height();
                for (const Dab& db : dabs) {
                    pittore::compute::paint_dab_host(
                        img.data(), w, h, db.x, db.y, db.r, hard, db.o,
                        ink);
                }
            },
            QStringLiteral("MCP stroke"), QStringLiteral("brush"));
        if (!ok)
            return errResp(id, QStringLiteral("no editable active layer"));
        // applyLayerEditOneShot only signals history: tell the canvas.
        emit state.documentModified(d);
        return okResp(id, QJsonObject{{"dabs", (int)dabs.size()}});
    }
    if (cmd == QStringLiteral("undo")) {
        if (!state.canUndo())
            return okResp(id, QJsonObject{{"undone", false}});
        state.undo();
        return okResp(id, QJsonObject{{"undone", true}});
    }
    if (cmd == QStringLiteral("redo")) {
        if (!state.canRedo())
            return okResp(id, QJsonObject{{"redone", false}});
        state.redo();
        return okResp(id, QJsonObject{{"redone", true}});
    }
    if (cmd == QStringLiteral("set_active_layer")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        const int layer = params.value("layer").toInt(-1);
        if (layer < 0 || layer >= d->layers.size())
            return errResp(id, QStringLiteral("layer index out of range"));
        // Selection is view state, not a mutation: no undo step.
        state.setActiveLayerIndex(layer);
        return okResp(id, QJsonObject{{"activeLayer", d->activeLayer}});
    }
    if (cmd == QStringLiteral("add_text")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        const QString text = params.value("text").toString();
        if (text.isEmpty()) return errResp(id, QStringLiteral("need text"));
        const double x = params.value("x").toDouble(0.0);
        const double y = params.value("y").toDouble(0.0);
        const double size = params.value("size").toDouble(48.0);
        if (!(size > 0.0) || size > 1000.0)
            return errResp(id, QStringLiteral("need 0 < size <= 1000"));
        const QColor color(params.value("color").toString(QStringLiteral("#ffffff")));
        if (!color.isValid())
            return errResp(id, QStringLiteral("color must be like #ffffff"));
        const QString family = params.value("family").toString();
        state.beginUndoStep();
        const int index = state.addTextLayer(QPointF(x, y), 0.0, family, size,
                                             color);
        if (index < 0) {
            state.discardUndoStep();
            return errResp(id, QStringLiteral("text layer refused"));
        }
        d->layers[index].textSpec.text = text;
        if (!state.refreshTextLayer(index)) {
            state.discardUndoStep();
            return errResp(id, QStringLiteral("text render failed"));
        }
        state.commitUndoStep(QStringLiteral("MCP text"), QStringLiteral("text"));
        emit state.documentModified(d);
        return okResp(id, QJsonObject{{"index", index}});
    }
    if (cmd == QStringLiteral("save_project")) {
        const QString path = params.value("path").toString();
        if (path.isEmpty()) return errResp(id, QStringLiteral("need path"));
        QString error;
        if (!state.saveProject(path, &error))
            return errResp(id, QStringLiteral("save failed: %1").arg(error));
        return okResp(id, QJsonObject{{"path", path}});
    }
    if (cmd == QStringLiteral("open_image")) {
        const QString path = params.value("path").toString();
        if (path.isEmpty()) return errResp(id, QStringLiteral("need path"));
        QString error;
        if (!state.openImageFile(path, &error))
            return errResp(id, QStringLiteral("open failed: %1").arg(error));
        DocumentItem* d = state.activeDocument();
        return okResp(id, QJsonObject{
                                 {"title", d ? d->title : QString()},
                                 {"width", d ? d->size.width() : 0},
                                 {"height", d ? d->size.height() : 0},
                             });
    }
    if (cmd == QStringLiteral("mix_colors")) {
        // Pure color math: needs no document, works session-less.
        const QColor a(params.value("a").toString());
        const QColor b(params.value("b").toString());
        if (!a.isValid() || !b.isValid())
            return errResp(id, QStringLiteral("a/b must be colors like #ff0000"));
        QString space = params.value("space").toString(QStringLiteral("oklab"));
        if (space != QStringLiteral("oklab") &&
            space != QStringLiteral("linear") && space != QStringLiteral("srgb"))
            return errResp(id, QStringLiteral("space is oklab, linear or srgb"));
        const QJsonValue stepsV = params.value("steps");
        if (!stepsV.isUndefined() && !stepsV.isNull()) {
            const int steps = stepsV.toInt(0);
            if (steps < 2 || steps > 16)
                return errResp(id, QStringLiteral("steps must be 2..16"));
            QJsonArray ramp;
            for (int i = 0; i < steps; ++i) {
                ramp.append(mixQColors(a, b, double(i) / double(steps - 1),
                                       space));
            }
            return okResp(id, QJsonObject{{"colors", ramp}});
        }
        const double t = params.value("t").toDouble(0.5);
        return okResp(id, QJsonObject{{"color", mixQColors(a, b, t, space)}});
    }
    if (cmd == QStringLiteral("set_layer_opacity")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        if (!params.contains("layer") || !params.contains("opacity"))
            return errResp(
                id, QStringLiteral("need {\"layer\": int, \"opacity\": 0..100}"));
        const int layer = params.value("layer").toInt(-1);
        const int opacity = params.value("opacity").toInt(-1);
        if (layer < 0 || layer >= d->layers.size())
            return errResp(id, QStringLiteral("layer index out of range"));
        if (opacity < 0 || opacity > 100)
            return errResp(id, QStringLiteral("opacity must be 0..100"));
        // Selection is not a mutation; the opacity set below owns the step.
        state.setActiveLayerIndex(layer);
        const bool ok = state.setActiveLayerOpacity(opacity);
        if (!ok) return errResp(id, QStringLiteral("opacity refused"));
        return okResp(id, QJsonObject{{"opacity", d->layers[layer].opacity}});
    }
    if (cmd == QStringLiteral("apply_filter")) {
        DocumentItem* d = state.activeDocument();
        if (!d) return errResp(id, QStringLiteral("no active document"));
        const std::string fid =
            params.value("filter").toString().toStdString();
        const pittore::filter::FilterDef* def =
            pittore::filter::findFilter(fid);
        if (!def)
            return errResp(id, QStringLiteral("unknown filter '%1'").arg(
                                    QString::fromStdString(fid)));
        std::vector<double> p =
            pittore::filter::defaultParams(fid);
        const QJsonArray given = params.value("params").toArray();
        if (!given.isEmpty()) {
            p.clear();
            for (const QJsonValue& v : given) p.push_back(v.toDouble());
        }
        // Gallery's undo-safe one-shot path (own undo step, recomposite).
        const QString name = QString::fromUtf8(def->name);
        const bool ok = state.applyLayerEditOneShot(
            [fid, p](pittore::Image& img) {
                pittore::filter::applyFilter(img, fid, p);
            },
            name, QStringLiteral("filter"));
        if (!ok)
            return errResp(id, QStringLiteral("no editable active layer"));
        // applyLayerEditOneShot only signals history: tell the canvas.
        emit state.documentModified(d);
        return okResp(id, QJsonObject{{"applied", true}});
    }
    if (cmd == QStringLiteral("export_png")) {
        DocumentItem* d = state.activeDocument();
        if (!d || d->composite.isNull())
            return errResp(id, QStringLiteral("no rendered composite"));
        const QString path = params.value("path").toString();
        if (path.isEmpty()) return errResp(id, QStringLiteral("need path"));
        QImage img = d->composite;
        const int maxDim = params.value("maxDim").toInt(0);
        if (maxDim > 0)
            img = img.scaled(qBound(16, maxDim, 4096), qBound(16, maxDim, 4096),
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (!img.save(path, "PNG"))
            return errResp(id, QStringLiteral("could not write '%1'").arg(path));
        return okResp(id, QJsonObject{{"path", path},
                                      {"width", img.width()},
                                      {"height", img.height()}});
    }
    if (cmd == QStringLiteral("run_command")) {
        // Drive a headless-safe MainWindow command by id (auto_refine,
        // select_subject, ...). Blocks the UI thread while it runs — AI
        // commands take tens of seconds; the shim waits patiently.
        if (!commandRunner_)
            return errResp(id, QStringLiteral("bridge has no command runner"));
        const QString cid = params.value("id").toString();
        if (cid.isEmpty()) return errResp(id, QStringLiteral("need id"));
        const bool ran = commandRunner_(cid);
        if (!ran)
            return errResp(id, state.statusHint().isEmpty()
                                    ? QStringLiteral("command refused")
                                    : state.statusHint());
        return okResp(id, QJsonObject{{"ran", true},
                                      {"statusHint", state.statusHint()}});
    }
    if (cmd == QStringLiteral("run_info")) {
        // Last headless-capable AI run report (production get_state): what
        // ran, whether it worked, model, timing, matte coverage, stages.
        const AppState::AiRunReport& r = state.lastAiRun();
        QJsonArray stages;
        for (const QString& s : r.stages) stages.append(s);
        return okResp(id, QJsonObject{{"command", r.command},
                                      {"ok", r.ok},
                                      {"message", r.message},
                                      {"model", r.model},
                                      {"milliseconds", r.milliseconds},
                                      {"coverage", r.coverage},
                                      {"stages", stages},
                                      {"statusHint", state.statusHint()}});
    }
    if (cmd == QStringLiteral("export_layer_png")) {
        // Active pixel layer's straight RGBA pixels to a PNG file (alpha
        // preserved) — the diagnostic the composite cannot give: this is the
        // matte as data, not as rendered.
        DocumentItem* d = state.activeDocument();
        LayerItem* layer = state.activeLayer();
        if (!d || !layer || layer->kind != LayerItem::Kind::Pixel ||
            !layer->pixels)
            return errResp(id, QStringLiteral("no active pixel layer"));
        const QString path = params.value("path").toString();
        if (path.isEmpty()) return errResp(id, QStringLiteral("need path"));
        const std::uint32_t pw = layer->pixels->width();
        const std::uint32_t ph = layer->pixels->height();
        QImage img(int(pw), int(ph), QImage::Format_RGBA8888);
        for (std::uint32_t y = 0; y < ph; ++y) {
            uchar* row = img.scanLine(int(y));
            for (std::uint32_t x = 0; x < pw; ++x) {
                const pittore::RGBAf& p =
                    layer->pixels->at(int(x), int(y));
                const auto enc = [](float v) {
                    return static_cast<uchar>(
                        std::clamp(int(v * 255 + 0.5f), 0, 255));
                };
                row[x * 4 + 0] = enc(p.r);
                row[x * 4 + 1] = enc(p.g);
                row[x * 4 + 2] = enc(p.b);
                row[x * 4 + 3] = enc(p.a);
            }
        }
        const int maxDim = params.value("maxDim").toInt(0);
        if (maxDim > 0)
            img = img.scaled(qBound(16, maxDim, 4096),
                             qBound(16, maxDim, 4096), Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        if (!img.save(path, "PNG"))
            return errResp(id, QStringLiteral("could not write '%1'").arg(path));
        return okResp(id, QJsonObject{{"path", path},
                                      {"width", img.width()},
                                      {"height", img.height()}});
    }
    return errResp(id, QStringLiteral("unknown cmd '%1'").arg(cmd));
}

}  // namespace pittore::ui
