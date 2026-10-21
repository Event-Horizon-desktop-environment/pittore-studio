#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/ai_models.h"
#include "ui/color_mode.h"
#include "ui/live_filter.h"
#include "ui/mask_finish.h"
#include "ui/project_manager.h"
#include "ui/selection_mask.h"
#include "ui/settings.h"
#include "ui/color_mode.h"
#include "ui/svg_parts.h"

#include "engine/io/af.h"
#include "engine/io/af_layers.h"
#include "engine/io/af_layers/emit/af_write.h"
#include "engine/io/icc.h"
#include "engine/io/psd.h"
#ifdef PITTORE_TIFF
#include "engine/io/tiff.h"
#endif
#include "engine/text/text_engine.h"
#include "engine/filter/filters.h"
#include <QDir>
#include <QColorSpace>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <QLinearGradient>
#include <QFontMetricsF>
#include <QPainter>
#include <QtMath>

#include "engine/compute/backend.h"
#include "engine/compute/dither.h"
#include "engine/compute/factory.h"
#include "engine/compute/adjust.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/paint.h"
#include "engine/compute/brushes/blur/blur_dab.h"
#include "engine/compute/brushes/adjust/adjust_dab.h"
#include "engine/compute/brushes/history/history_dab.h"
#include "engine/compute/brushes/mixer/mixer.h"
#include "engine/compute/brushes/mix/mix.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/core/log.h"
#include "engine/core/parallel.h"
#include "engine/core/tonal_ops.h"
#include "ui/tools/log/tool_log.h"
#include "ui/tools/options/option_builders.h"

namespace pittore::ui {
namespace {

// Separator positions match the conventional blend-mode menu families.
const QSet<int>& separatorIndices() {
    static const QSet<int> s = {1, 4, 9, 14, 21, 25};
    return s;
}

// Fallback used by documents before AppState points them at a real backend, and
// whenever a GPU selection fails (unavailable runtime, refused device). The
// live backend (AppState::computeBackend) replaces this per document.

// True when a tool's "mode" option is the painting tools' shared blend list
// (option_builders.cpp). Several tools reuse the id for their own enum —
// Color Replacement's recolor modes, the Eraser's tip shapes — and those
// must never be read as blend modes.
bool isBlendModeOption(const OptionSpec& spec) {
    const std::vector<const char*> blend = detail::blendModeItems();
    if (spec.items.size() != blend.size()) return false;
    for (std::size_t i = 0; i < blend.size(); ++i)
        if (std::strcmp(spec.items[i], blend[i]) != 0) return false;
    return true;
}

// Create the backend a settings block requests: GPU when enabled and usable
// (the named device when it exists, else the first CUDA/HIP device; anything
// unavailable falls through to a second CPU attempt, never to nullptr).
std::unique_ptr<pittore::compute::ComputeBackend> makeBackendFor(
    const AppSettings& s) {
    if (s.gpuEnabled) {
        const auto devices = pittore::compute::enumerate_devices();
        // Named device first; then any GPU.
        for (const auto& d : devices) {
            if (d.type == pittore::compute::BackendType::CPU) continue;
            if (!s.gpuDevice.isEmpty() &&
                s.gpuDevice != QString::fromStdString(d.name))
                continue;
            if (auto b = pittore::compute::make_backend(d.type)) {
                ::pittore::core::log::log_info(
                    "[prefs] requested GPU '%s', selected %s",
                    s.gpuDevice.isEmpty() ? "(any)" : s.gpuDevice.toStdString().c_str(),
                    b->name().c_str());
                return b;
            }
        }
        // No named device matched → pick the first GPU we can build.
        for (const auto& d : devices) {
            if (d.type == pittore::compute::BackendType::CPU) continue;
            if (auto b = pittore::compute::make_backend(d.type)) {
                ::pittore::core::log::log_info(
                    "[prefs] no matching device, fell through to %s", b->name().c_str());
                return b;
            }
        }
        ::pittore::core::log::log_warning(
            "[prefs] GPU requested but no GPU backend could be built; using CPU");
    }
    // GPU disabled / unusable → CPU reference backend (always buildable).
    return pittore::compute::make_backend(pittore::compute::BackendType::CPU);
}




}  // namespace

// Full 27-mode + Pass Through mapping (blend.h owns the names it understands).
// "Add" is the imported display name for Linear Dodge (Add) — the one name
// that differs from the engine's; everything else round-trips as-is.
// Namespace scope (declared in app_state_detail.h) so the split-out
// composite TU can call it.
pittore::compute::BlendMode engineBlendMode(const QString& name) {
    if (name == QStringLiteral("Add")) return pittore::compute::BlendMode::LinearDodge;
    return pittore::compute::blend::from_display_name(name.toUtf8().constData());
}

// History source lookup shared by the History and Art History Brushes:
// the oldest undo snapshot's matching layer (document-open state).
// Shared pixel Images are copy-on-write safe to read. Null when no
// usable history state exists yet.
const LayerItem* AppState::historySourceFor(DocumentItem* d, int activeLayer,
                                            std::uint32_t lw,
                                            std::uint32_t lh) {
    if (!d || d->undoStack_.isEmpty()) return nullptr;
    const DocumentItem::DocumentSnapshot& snap = d->undoStack_.first();
    const int ai = std::clamp(activeLayer, 0, int(d->layers.size()) - 1);
    if (ai >= 0 && ai < snap.layers.size()) {
        const LayerItem& c = snap.layers[ai];
        if (c.kind == LayerItem::Kind::Pixel && c.pixels &&
            c.pixels->width() == lw && c.pixels->height() == lh)
            return &c;
    }
    for (const LayerItem& c : snap.layers) {
        if (c.kind == LayerItem::Kind::Pixel && c.pixels &&
            c.pixels->width() == lw && c.pixels->height() == lh)
            return &c;
    }
    return nullptr;
}















const QStringList& blendModeNames() {
    static const QStringList names = {
        QStringLiteral("Normal"), QStringLiteral("Dissolve"),
        QStringLiteral("Darken"), QStringLiteral("Multiply"), QStringLiteral("Color Burn"),
        QStringLiteral("Linear Burn"), QStringLiteral("Darker Color"),
        QStringLiteral("Lighten"), QStringLiteral("Screen"), QStringLiteral("Color Dodge"),
        QStringLiteral("Linear Dodge (Add)"), QStringLiteral("Lighter Color"),
        QStringLiteral("Overlay"), QStringLiteral("Soft Light"), QStringLiteral("Hard Light"),
        QStringLiteral("Vivid Light"), QStringLiteral("Linear Light"), QStringLiteral("Pin Light"),
        QStringLiteral("Hard Mix"),
        QStringLiteral("Difference"), QStringLiteral("Exclusion"), QStringLiteral("Subtract"),
        QStringLiteral("Divide"),
        QStringLiteral("Hue"), QStringLiteral("Saturation"), QStringLiteral("Color"),
        QStringLiteral("Luminosity")};
    return names;
}

bool blendModeIsSeparatorBefore(int index) { return separatorIndices().contains(index); }


QVector<int> DocumentItem::enclosingGroups(int index) const {
    QVector<int> out;
    if (index < 0 || index >= layers.size()) return out;
    int indent = layers[index].indent;
    for (int i = index - 1; i >= 0; --i) {
        const LayerItem& l = layers[i];
        if (l.indent >= indent) continue;   // same/deeper rows aren't ancestors
        if (l.kind != LayerItem::Kind::Group) break;   // a shallower non-group ends it
        out.append(i);                     // a shallower group IS an ancestor
        indent = l.indent;                 // keep climbing inside that group
    }
    return out;   // innermost group first, outermost last
}

bool DocumentItem::effectivelyVisible(int index) const {
    if (index < 0 || index >= layers.size() || !layers[index].visible) return false;
    for (int g : enclosingGroups(index))
        if (!layers[g].visible) return false;
    return true;
}

void DocumentItem::effectiveVisibility(QVector<char>& vis,
                                       QVector<int>& parent) const {
    const int n = layers.size();
    vis.resize(n);
    parent.resize(n);
    // Indent-stack sweep: leaders hold strictly decreasing indents, so the
    // stack top is always the nearest preceding row with smaller indent —
    // exactly what the scan-back in enclosingGroups() meets first. A
    // non-group top shields deeper followers from groups below it (the
    // scan-back `break`), which the walk below reproduces by stopping at it.
    struct Lead {
        int indent;
        int index;
        bool isGroup;
    };
    std::vector<Lead> st;
    st.reserve(16);
    for (int i = 0; i < n; ++i) {
        const LayerItem& l = layers[i];
        const int t = l.indent;
        while (!st.empty() && st.back().indent >= t) st.pop_back();
        int par = -1;
        bool chain = true;
        for (int s = static_cast<int>(st.size()) - 1; s >= 0; --s) {
            if (!st[s].isGroup) break;  // shield: scan-back stops here too
            if (par < 0) par = st[s].index;
            if (!layers[st[s].index].visible) chain = false;
        }
        vis[i] = (l.visible && chain) ? 1 : 0;
        parent[i] = par;
        st.push_back(Lead{t, i, l.kind == LayerItem::Kind::Group});
    }
}

bool DocumentItem::rowHiddenByCollapsedGroup(int index) const {
    if (index < 0 || index >= layers.size()) return false;
    for (int g : enclosingGroups(index))
        if (!layers[g].groupExpanded) return true;
    return false;
}

// Text / Shape / Adjustment stand-ins, drawn over the flattened pixels. Shared
// by the full rebuild and the incremental region path so both stay consistent.
void DocumentItem::drawVectorOverlays() {
    // Fast path: pixel-only documents have nothing to overlay. The loop below
    // would only construct a QPainter and skip every layer.
    bool any = false;
    for (int i = layers.size() - 1; i >= 0; --i) {
        const LayerItem& l = layers[i];
        if (l.kind != LayerItem::Kind::Pixel &&
            l.kind != LayerItem::Kind::Group && effectivelyVisible(i)) {
            any = true;
            break;
        }
    }
    if (!any) return;
    QPainter p(&composite);
    p.setRenderHint(QPainter::Antialiasing, true);

    for (int i = layers.size() - 1; i >= 0; --i) {
        const LayerItem& l = layers[i];
        if (!effectivelyVisible(i)) continue;
        if (l.kind == LayerItem::Kind::Pixel || l.kind == LayerItem::Kind::Group) continue;
        p.setOpacity(l.opacity / 100.0 * (l.fill / 100.0));

        switch (l.kind) {
            case LayerItem::Kind::Text: {
                QFont f = p.font();
                f.setPixelSize(qMax(24, size.height() / 12));
                p.setFont(f);
                p.setPen(l.swatch);
                p.drawText(composite.rect().adjusted(size.width() / 12, size.height() / 8, 0, 0),
                           Qt::AlignLeft | Qt::AlignTop, l.name);
                break;
            }
            case LayerItem::Kind::Shape: {
                p.setBrush(l.swatch);
                p.setPen(Qt::NoPen);
                const QRectF r(size.width() * 0.22, size.height() * 0.22,
                               size.width() * 0.4, size.height() * 0.4);
                p.drawRoundedRect(r, size.width() * 0.02, size.width() * 0.02);
                break;
            }
            case LayerItem::Kind::Adjustment: {
                // Legacy stand-ins (no live kind) keep their tint wash so an
                // old document still signals the row; live adjustments are
                // composited by the engine and draw nothing here.
                if (l.adjustmentKind == 0)
                    p.fillRect(composite.rect(),
                               QColor(l.swatch.red(), l.swatch.green(),
                                      l.swatch.blue(), 60));
                break;
            }
            default:
                break;
        }
    }
    p.setOpacity(1.0);
}

QString DocumentItem::statusText() const {
    const double mb = size.width() * double(size.height()) * 4.0 / (1024.0 * 1024.0);
    QString s = QStringLiteral("%1 × %2 px  ·  %3 ppi  ·  %4  ·  %5 MB RGBA")
                    .arg(size.width())
                    .arg(size.height())
                    .arg(dpi)
                    .arg(colorMode)
                    .arg(mb, 0, 'f', 1);
    if (!filePath.isEmpty()) {
        const QFileInfo fi(filePath);
        if (fi.exists())
            s += QStringLiteral("  ·  %1 MiB file").arg(
                fi.size() / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (isProxy)
        s += QStringLiteral("  ·  proxy 1/%1 of %2 × %3")
                 .arg(proxyFactor)
                 .arg(fullSize.width())
                 .arg(fullSize.height());
    return s;
}

// ---------------------------------------------------------------------------
// AppState
// ---------------------------------------------------------------------------
AppState::AppState(QObject* parent) : QObject(parent) {
    swatches_ = {QColor("#000000"), QColor("#3f3f3f"), QColor("#7f7f7f"), QColor("#bfbfbf"),
                 QColor("#ffffff"), QColor("#ff2600"), QColor("#ff9300"), QColor("#fffb00"),
                 QColor("#00f900"), QColor("#00fdff"), QColor("#0433ff"), QColor("#9437ff"),
                 QColor("#ff40ff"), QColor("#941100"), QColor("#945200"), QColor("#4f8f00"),
                 QColor("#005493"), QColor("#011993"), QColor("#531b93"), QColor("#8c2c85")};

    // Preferences: load persisted settings, adopt the stored theme as the live
    // theme_ (main.cpp applies the palette from state.theme() right after this),
    // and build the compute backend the settings call for.
    loadSettings(settings_);
    theme_ = settings_.theme;
    // Session snapping mirrors the persisted view settings so a restart
    // restores both the master toggle and the Snap To targets.
    snapEnabled_ = settings_.snapEnabled;
    snapTargets_ = settings_.snapTargets & SnapTargetsAll;
    DocumentItem::setMaxUndoSteps(settings_.undoLimit);
    backend_ = makeBackendFor(settings_);
}

pittore::compute::ComputeBackend& AppState::computeBackend() const {
    return backend_ ? *backend_ : cpuBackend();
}

QString AppState::computeDeviceLabel() const {
    const pittore::compute::Device& dev = computeBackend().device();
    return QString::fromLatin1(pittore::compute::to_string(dev.type)) +
           QStringLiteral(" · ") + QString::fromStdString(dev.name);
}

void AppState::applySettings(const AppSettings& next) {
    const bool computeChanged =
        next.gpuEnabled != settings_.gpuEnabled ||
        next.gpuDevice != settings_.gpuDevice ||
        next.cpuDevice != settings_.cpuDevice ||
        next.ramLimitMb != settings_.ramLimitMb;
    const bool snapChangedByPrefs =
        next.snapEnabled != snapEnabled_ ||
        (next.snapTargets & SnapTargetsAll) != snapTargets_;
    const bool proofChangedByPrefs =
        next.proofProfile != settings_.proofProfile ||
        next.proofIntent != settings_.proofIntent ||
        next.proofBpc != settings_.proofBpc;

    settings_ = next;
    // Shrinking the recent cap drops the tail immediately so the menu and
    // the start page agree without waiting for the next open.
    while (settings_.recentProjects.size() > qMax(0, settings_.recentMax))
        settings_.recentProjects.removeLast();
    saveSettings(settings_);

    DocumentItem::setMaxUndoSteps(settings_.undoLimit);

    if (snapChangedByPrefs) {
        snapEnabled_ = settings_.snapEnabled;
        snapTargets_ = settings_.snapTargets & SnapTargetsAll;
        emit snapChanged();
    }

    if (computeChanged) {
        backend_ = makeBackendFor(settings_);
        for (DocumentItem* d : documents_) d->setBackend(backend_.get());
        ::pittore::core::log::log_info(
            "[prefs] backend rebuilt: gpuEnabled=%d gpuDevice=%d cpuDevice=%d selected=%s",
            settings_.gpuEnabled ? 1 : 0, settings_.gpuDevice, settings_.cpuDevice,
            backend_ ? backend_->name().c_str() : "(null)");
    }
    if (settings_.theme != theme_) {
        theme_ = settings_.theme;
        emit themeChanged(theme_);
    }
    if (proofChangedByPrefs) emit proofChanged();
    emit settingsChanged();
}

const char* taskContextName(TaskContext c) {
    switch (c) {
        case TaskContext::None: return "None";
        case TaskContext::Selection: return "Selection";
        case TaskContext::Text: return "Text";
        case TaskContext::Crop: return "Crop";
        case TaskContext::Transform: return "Transform";
        case TaskContext::Path: return "Path";
        case TaskContext::ShapeLayer: return "ShapeLayer";
        case TaskContext::GenerativeResult: return "GenerativeResult";
    }
    return "?";
}

void AppState::setActiveTool(ToolId id, const char* reason) {
    if (activeTool_ == id) return;
    const ToolId previous = activeTool_;
    activeTool_ = id;

    // Per-tool setup trace. Everything below — and every toolChanged consumer
    // it wakes up — writes into id's own log file, so the tail of that file
    // names the exact stage a crash died in (see ui/tools/log/tool_log.h).
    ToolSetupTrace trace(id, reason);
    tool_logf(id, "state/set-active", "reason=%s previous=%s",
              reason ? reason : "switch", toolDef(previous).name);

    setStatusHint(QString::fromUtf8(toolDef(id).hint));
    tool_log(id, "state/status-hint", "hint set");

    tool_log(id, "state/emit-toolChanged", "before consumers");
    emit toolChanged(id);
    tool_log(id, "state/emit-toolChanged", "after consumers");

    // Tool switches redefine the task context (R20: a live crop or type edit is
    // a task, not just a tool).
    switch (id) {
        case ToolId::Crop:
        case ToolId::PerspectiveCrop:
            setTaskContext(TaskContext::Crop);
            break;
        case ToolId::HorizontalType:
        case ToolId::VerticalType:
        case ToolId::HorizontalTypeMask:
        case ToolId::VerticalTypeMask:
            setTaskContext(TaskContext::Text);
            break;
        case ToolId::Pen:
        case ToolId::FreeformPen:
        case ToolId::CurvaturePen:
        case ToolId::PathSelection:
        case ToolId::DirectSelection:
        case ToolId::NodeTool:
        case ToolId::PointTransformTool:
        case ToolId::CornerTool:
        case ToolId::ContourTool:
        case ToolId::StrokeWidthTool:
        case ToolId::KnifeTool:
        case ToolId::VectorBrushTool:
            setTaskContext(TaskContext::Path);
            break;
        default: {
            DocumentItem* d = activeDocument();
            setTaskContext(d && !d->selection.isEmpty() ? TaskContext::Selection
                                                        : TaskContext::None);
            break;
        }
    }
}

void AppState::cycleToolGroup(char key) {
    const ToolGroup* g = groupForKey(key);
    if (!g) return;
    const ToolGroup* current = groupForTool(activeTool_);
    setActiveTool(current == g ? nextInGroup(activeTool_) : g->leader, "cycle-group");
}

void AppState::pushTemporaryTool(ToolId id) {
    toolStack_.append(activeTool_);
    ++temporaryDepth_;
    setActiveTool(id, "temporary-push");
}

void AppState::popTemporaryTool() {
    if (toolStack_.isEmpty()) return;
    const ToolId previous = toolStack_.takeLast();
    if (temporaryDepth_ > 0) --temporaryDepth_;
    setActiveTool(previous, "temporary-pop");
}

QVariant AppState::option(ToolId tool, const QString& id) const {
    const QVariant stored =
        options_.value(QStringLiteral("%1/%2").arg(int(tool)).arg(id));
    if (stored.isValid()) return stored;
    // Unset options fall back to the registry's declared default, so painting
    // works before the user ever touches the options bar (brush_size, …).
    for (const OptionSpec& spec : optionsFor(tool))
        if (id == QString::fromUtf8(spec.id)) return spec.defaultValue;
    return QVariant();
}

void AppState::setOption(ToolId tool, const QString& id, const QVariant& value) {
    const QString key = QStringLiteral("%1/%2").arg(int(tool)).arg(id);
    if (options_.value(key) == value) return;
    setOptionSilently(tool, id, value);
    // The Type tools' options drive the text being typed: push them straight
    // through to the active live text layer so the canvas updates as the user
    // picks a family, size, alignment or colour.
    if (tool == ToolId::HorizontalType || tool == ToolId::VerticalType)
        applyTextOption(id, value);
}

void AppState::setOptionSilently(ToolId tool, const QString& id, const QVariant& value) {
    const QString key = QStringLiteral("%1/%2").arg(int(tool)).arg(id);
    if (options_.value(key) == value) return;
    options_.insert(key, value);
    emit optionChanged(tool, id, value);
}

pittore::compute::BlendMode AppState::paintBlendFor(ToolId tool) const {
    const QVariant v = option(tool, QStringLiteral("mode"));
    if (!v.isValid()) return pittore::compute::BlendMode::Normal;
    const int idx = v.toInt();
    if (idx <= 0) return pittore::compute::BlendMode::Normal;
    for (const OptionSpec& spec : optionsFor(tool)) {
        if (QString::fromUtf8(spec.id) != QStringLiteral("mode")) continue;
        if (!isBlendModeOption(spec)) break;
        if (idx >= int(spec.items.size())) break;
        return engineBlendMode(QString::fromUtf8(spec.items[idx]));
    }
    return pittore::compute::BlendMode::Normal;
}

bool AppState::setToolBlendMode(ToolId tool, const QString& name) {
    for (const OptionSpec& spec : optionsFor(tool)) {
        if (QString::fromUtf8(spec.id) != QStringLiteral("mode")) continue;
        if (!isBlendModeOption(spec)) return false;
        for (std::size_t i = 0; i < spec.items.size(); ++i) {
            if (name == QString::fromUtf8(spec.items[i])) {
                setOption(tool, QStringLiteral("mode"), int(i));
                return true;
            }
        }
        return false;
    }
    return false;
}

void AppState::setForeground(const QColor& c) {
    if (foreground_ == c) return;
    foreground_ = c;
    emit colorsChanged();
}

void AppState::setBackground(const QColor& c) {
    if (background_ == c) return;
    background_ = c;
    emit colorsChanged();
}

void AppState::swapColors() {
    std::swap(foreground_, background_);
    emit colorsChanged();
}

void AppState::resetColors() {
    foreground_ = Qt::black;
    background_ = Qt::white;
    emit colorsChanged();
}

void AppState::addSwatch(const QColor& c) {
    if (swatches_.contains(c)) return;
    swatches_.append(c);
    emit swatchesChanged();
}

void AppState::setQuickMask(bool on) {
    if (quickMask_ == on) return;
    quickMask_ = on;
    emit quickMaskChanged(on);
}

void AppState::setScreenMode(ScreenMode m) {
    if (screenMode_ == m) return;
    screenMode_ = m;
    emit screenModeChanged(m);
}

void AppState::cycleScreenMode(bool reverse) {
    const int n = 3;
    int i = static_cast<int>(screenMode_);
    i = ((i + (reverse ? -1 : 1)) % n + n) % n;
    setScreenMode(static_cast<ScreenMode>(i));
}

void AppState::setChromeVisibility(ChromeVisibility v) {
    if (chrome_ == v) return;
    chrome_ = v;
    emit chromeVisibilityChanged(v);
}

void AppState::cycleSurround(bool reverse) {
    surround_ = ((surround_ + (reverse ? -1 : 1)) % 3 + 3) % 3;
    emit surroundChanged(surround_);
}

void AppState::setTheme(UiTheme t) {
    if (theme_ == t) return;
    theme_ = t;
    settings_.theme = t;
    saveSettings(settings_);
    emit themeChanged(t);
}

void AppState::setSnapEnabled(bool on) {
    if (snapEnabled_ == on && settings_.snapEnabled == on) return;
    snapEnabled_ = on;
    // Persist View > Snap toggles so they survive restarts. Lightweight:
    // no backend rebuild, just the snap broadcast (applySettings callers
    // sync the same fields without re-entering here).
    if (settings_.snapEnabled != on) {
        settings_.snapEnabled = on;
        saveSettings(settings_);
    }
    emit snapChanged();
}

void AppState::setSnapTargets(int targets) {
    targets &= SnapTargetsAll;
    if (snapTargets_ == targets && settings_.snapTargets == targets) return;
    snapTargets_ = targets;
    if (settings_.snapTargets != targets) {
        settings_.snapTargets = targets;
        saveSettings(settings_);
    }
    emit snapChanged();
}

void AppState::setSnapTarget(SnapTarget target, bool on) {
    setSnapTargets(on ? (snapTargets_ | target) : (snapTargets_ & ~target));
}

void AppState::setShowRulers(bool on) {
    if (settings_.showRulers == on) return;
    settings_.showRulers = on;
    saveSettings(settings_);
}

void AppState::setShowGuides(bool on) {
    if (settings_.showGuides == on) return;
    settings_.showGuides = on;
    saveSettings(settings_);
}

void AppState::setShowGrid(bool on) {
    if (settings_.showGrid == on) return;
    settings_.showGrid = on;
    saveSettings(settings_);
}

void AppState::setShowSelectionEdges(bool on) {
    if (settings_.showSelectionEdges == on) return;
    settings_.showSelectionEdges = on;
    saveSettings(settings_);
}

void AppState::setShowSmartGuides(bool on) {
    if (settings_.showSmartGuides == on) return;
    settings_.showSmartGuides = on;
    saveSettings(settings_);
}

void AppState::setShowPixelGrid(bool on) {
    if (settings_.showPixelGrid == on) return;
    settings_.showPixelGrid = on;
    saveSettings(settings_);
}

void AppState::setShowExtras(bool on) {
    if (settings_.showExtras == on) return;
    settings_.showExtras = on;
    saveSettings(settings_);
}

void AppState::setProofEnabled(bool on) {
    if (proofEnabled_ == on) return;
    proofEnabled_ = on;
    emit proofChanged();
}

void AppState::setProofGamut(bool on) {
    if (proofGamut_ == on) return;
    proofGamut_ = on;
    emit proofChanged();
}

DocumentItem* AppState::activeDocument() const {
    if (activeDocument_ < 0 || activeDocument_ >= documents_.size()) return nullptr;
    return documents_.at(activeDocument_);
}

void AppState::setActiveDocumentIndex(int index) {
    if (index == activeDocument_ || index < -1 || index >= documents_.size()) return;
    activeDocument_ = index;
    emit activeDocumentChanged(activeDocument());
    emit layersChanged();
    emit historyChanged();
    emit selectionChanged();
}

DocumentItem* AppState::addDocument(const QString& title, QSize size, int dpi) {
    // RAM limit gate: refuse documents whose initial working set (background
    // layer pixels + engine staging pair + composite QImage) exceeds the user's
    // budget, if one was set.
    if (settings_.ramLimitMb > 0 && !size.isEmpty()) {
        const double mb =
            static_cast<double>(documentWorkingSetBytes(size)) / (1024.0 * 1024.0);
        if (mb > settings_.ramLimitMb) {
            setStatusHint(tr("Document exceeds RAM limit (%1 MB > %2 MB)")
                               .arg(QString::number(mb, 'f', 1))
                               .arg(settings_.ramLimitMb));
            return nullptr;
        }
    }
    auto* doc = new DocumentItem(title, size, dpi);
    doc->resetHistory();
    doc->setBackend(backend_.get());
    documents_.append(doc);
    emit documentsChanged();
    setActiveDocumentIndex(documents_.size() - 1);
    return doc;
}

void AppState::closeDocument(int index) {
    if (index < 0 || index >= documents_.size()) return;
    delete documents_.takeAt(index);
    if (documents_.isEmpty()) {
        activeDocument_ = -1;
        emit documentsChanged();
        emit activeDocumentChanged(nullptr);
        emit layersChanged();
        emit historyChanged();
        return;
    }
    const int next = qBound(0, activeDocument_ >= index ? activeDocument_ - 1 : activeDocument_,
                            documents_.size() - 1);
    activeDocument_ = -1;
    emit documentsChanged();
    setActiveDocumentIndex(next);
}

DocumentItem* AppState::createNewProject(const ProjectFileData& data, QString* error) {
    const QString name = data.name.trimmed();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\')) || name == QLatin1String(".") ||
        name == QLatin1String("..")) {
        if (error) *error = tr("Enter a project name without / or \\.");
        return nullptr;
    }
    DocumentItem* doc = addDocument(name, data.size, data.dpi);
    if (!doc) {
        // The RAM-limit gate in addDocument already surfaced a status hint.
        if (error)
            *error = tr("Could not create the document (check the RAM limit in Preferences).");
        return nullptr;
    }
    doc->colorMode = data.colorMode;
    if (doc->colorMode.startsWith(QStringLiteral("CMYK"),
                                  Qt::CaseInsensitive))
        doc->iccProfile = resolveCmykProfile(*doc);

    // The canvas bottom erasing restores: white/black paper, or real
    // transparency the eraser punches through.
    if (data.background == QStringLiteral("transparent")) {
        doc->canvasTransparent = true;
    } else if (data.background == QStringLiteral("black")) {
        doc->canvasPaper = QColor(0, 0, 0);
    } else {
        doc->canvasPaper = QColor(255, 255, 255);
    }

    // Fill the document's background layer per the requested canvas bottom.
    LayerItem& bg = doc->layers.last();
    if (!bg.pixels) ensureLayerPixels(*doc, bg);
    if (bg.pixels) {
        if (data.background == QStringLiteral("transparent"))
            bg.pixels->fill(pittore::RGBAf{0, 0, 0, 0});
        else if (data.background == QStringLiteral("black"))
            bg.pixels->fill(pittore::RGBAf{0, 0, 0, 1});
        else
            bg.pixels->fill(pittore::RGBAf{1, 1, 1, 1});
        bg.swatch = data.background == QStringLiteral("black")
                        ? QColor(0, 0, 0)
                        : data.background == QStringLiteral("transparent")
                              ? QColor(0, 0, 0, 0)
                              : QColor(255, 255, 255);
        ++bg.sourceStamp;
        bg.thumbnail = QImage();
    }
    doc->dirty = false;
    doc->rebuildComposite();

    // Persist the project file immediately so it shows up on the start page.
    const QString path = projectPathForName(name);
    doc->filePath = path;
    QString saveErr;
    if (!saveProject(path, &saveErr)) {
        // The document is live regardless; a persistence failure should not
        // undo the creation, just tell the caller what happened.
        if (error)
            *error = tr("Project created but could not be saved:\n%1").arg(saveErr);
    }
    return doc;
}


void AppState::setTaskContext(TaskContext c) {
    if (context_ == c) return;
    context_ = c;
    // Lands in the active tool's own log: the contextual task bar rebuilds on
    // this signal, so this line says why.
    tool_log(activeTool_, "state/task-context", taskContextName(c));
    emit taskContextChanged(c);
}

void AppState::addPixelLayerFromImage(const QImage& img, const QString& name,
                                      const QPointF& offset, double scaleX,
                                      double scaleY, const QString& undoName,
                                      const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d || img.isNull()) return;
    auto pixels = imageFromQImage(img);
    if (!pixels) {
        setStatusHint(tr("Could not add layer (unsupported or too large)."));
        return;
    }
    if (!(scaleX > 0.0) || !qIsFinite(scaleX)) scaleX = 1.0;
    if (!(scaleY > 0.0) || !qIsFinite(scaleY)) scaleY = 1.0;
    d->beginUndoAction();
    LayerItem layer;
    layer.name = name.isEmpty() ? tr("Layer") : name;
    layer.kind = LayerItem::Kind::Pixel;
    layer.pixels = std::move(pixels);
    layer.offset = offset;
    layer.scaleX = scaleX;
    layer.scaleY = scaleY;
    addLayer(std::move(layer));
    d->commitUndoAction(undoName, iconKey);
    emit historyChanged();
}

void AppState::addLayer(LayerItem layer) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    const int at = qBound(0, d->activeLayer, d->layers.size());
    d->layers.insert(at, std::move(layer));
    d->activeLayer = at;
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
}

void AppState::removeLayerSilently(int index) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size()) return;
    d->layers.removeAt(index);
    d->activeLayer = qBound(0, d->activeLayer, d->layers.size() - 1);
    d->selectedLayers.clear();
    if (!d->layers.isEmpty()) d->selectedLayers.push_back(d->activeLayer);
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit documentModified(d);
}

void AppState::placeImageLayer(const QImage& img, const QString& name,
                               const QPointF& centerDoc, double scale) {
    DocumentItem* d = activeDocument();
    if (!d || img.isNull()) return;
    auto pixels = imageFromQImage(img);
    if (!pixels) {
        setStatusHint(tr("Could not place image (unsupported or too large)."));
        return;
    }
    if (!(scale > 0.0) || !qIsFinite(scale)) scale = 1.0;
    scale = qBound(0.01, scale, 100.0);
    LayerItem layer;
    layer.name = name.isEmpty() ? tr("Placed Image") : name;
    layer.kind = LayerItem::Kind::Pixel;
    layer.pixels = std::move(pixels);
    const double pw = img.width() * scale, ph = img.height() * scale;
    layer.offset = QPointF(centerDoc.x() - pw * 0.5, centerDoc.y() - ph * 0.5);
    layer.scaleX = layer.scaleY = scale;
    d->beginUndoAction();
    addLayer(std::move(layer));  // re-composites, selects, signals
    d->commitUndoAction(tr("Place"), QStringLiteral("move"));
    emit historyChanged();
}

void AppState::setActiveLayerIndex(int index) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size() || d->activeLayer == index) return;
    d->activeLayer = index;
    // Selection-only change: repaints the rows' highlight + the canvas gizmo
    // instead of rebuilding every row widget (thousands of SVG parts make a
    // full layersChanged rebuild far too slow for a plain click).
    emit activeLayerChanged();
}

LayerItem* AppState::activeLayer() const {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return nullptr;
    return &d->layers[qBound(0, d->activeLayer, d->layers.size() - 1)];
}

void AppState::setStatusHint(const QString& hint) {
    if (statusHint_ == hint) return;
    statusHint_ = hint;
    emit statusHintChanged(hint);
}





const char* AppState::generativeModelId() {
    // Catalogue id a bundled generative model would register under. None
    // exists yet (all catalogue entries are segmentation/matting), so the
    // store reports Absent and both call sites refuse honestly. When a
    // model lands, the audit gate below flips from SKIP-MODEL to tested.
    return "generative-fill";
}

bool AppState::generativeFill(bool background) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    const ToolId tool =
        background ? ToolId::GenerateBackground : ToolId::GenerativeFill;
    const QString prompt =
        option(tool, QStringLiteral("prompt")).toString().trimmed();
    if (prompt.isEmpty()) {
        setStatusHint(tr("Describe what to generate in the Prompt field."));
        return false;
    }
    if (!background && d->selection.isEmpty() && !d->selectionIsMask) {
        setStatusHint(tr("Generative Fill: mark an area first."));
        return false;
    }
    if (aiModelStore().state(QString::fromUtf8(generativeModelId())) !=
        AiModelState::Present) {
        setStatusHint(tr("Generative Fill needs a generative model "
                         "(none installed)."));
        return false;
    }
    return false;
}











void AppState::setCursorInfo(const QPointF& pos, const QColor& color) {
    cursorPos_ = pos;
    cursorColor_ = color;
    emit cursorInfoChanged(pos, color);
}

void AppState::markAnnotationsChanged() {
    if (DocumentItem* d = activeDocument()) {
        // Push derived readouts into the annotation tools' options fields so
        // the options bar shows the live Count and Ruler numbers.
        const int group = option(ToolId::Count, QStringLiteral("group")).toInt();
        int count = 0;
        for (const CountMarker& m : d->countMarkers)
            if (m.group == group) ++count;
        setOptionSilently(ToolId::Count, QStringLiteral("count"), count);

        if (d->rulerHasMeasurement) {
            const QPointF a = d->rulerStart;
            const QPointF b = d->rulerEnd;
            setOptionSilently(ToolId::Ruler, QStringLiteral("x"), a.x());
            setOptionSilently(ToolId::Ruler, QStringLiteral("y"), a.y());
            setOptionSilently(ToolId::Ruler, QStringLiteral("w"), b.x() - a.x());
            setOptionSilently(ToolId::Ruler, QStringLiteral("h"), b.y() - a.y());
            const double ang =
                std::atan2(b.y() - a.y(), b.x() - a.x()) * 180.0 /
                3.14159265358979323846;
            setOptionSilently(ToolId::Ruler, QStringLiteral("a"), ang);
            setOptionSilently(ToolId::Ruler, QStringLiteral("l1"), 0.0);
            setOptionSilently(ToolId::Ruler, QStringLiteral("l2"), std::hypot(b.x() - a.x(), b.y() - a.y()));
        }
        emit documentModified(d);
    }
    emit annotationsChanged();
}










bool AppState::invertActiveLayerPixels() {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->locked || layer->kind != LayerItem::Kind::Pixel ||
        !layer->pixels)
        return false;
    const bool hasSel = !d->selection.isEmpty() ||
                        (d->selectionIsMask && !d->selectionMask.isNull());
    if (!hasSel) {
        // Fast path: whole layer, alpha untouched.
        d->beginUndoAction();
        copyOnWriteActiveLayer();
        layer = activeLayer();
        if (!layer || !layer->pixels) {
            d->discardUndoAction();
            return false;
        }
        const std::size_t n = layer->pixels->pixel_count();
        pittore::RGBAf* all = layer->pixels->data();
        for (std::size_t i = 0; i < n; ++i) {
            all[i].r = 1.0f - all[i].r;
            all[i].g = 1.0f - all[i].g;
            all[i].b = 1.0f - all[i].b;
        }
        ++layer->sourceStamp;
        refreshLayerCmyk(*d, *layer);
        layer->thumbnail = QImage();
        d->rebuildComposite();
        d->commitUndoAction(tr("Invert"), QStringLiteral("invert"));
        emit historyChanged();
        emit documentModified(d);
        return true;
    }
    QRect clipRect = d->selection.toAlignedRect().intersected(
        QRect(QPoint(0, 0), d->size));
    if (clipRect.isEmpty()) return false;
    const QImage cov = selectionAsMask(*d);
    if (cov.isNull() || cov.size() != d->size) return false;
    d->beginUndoAction();
    copyOnWriteActiveLayer();
    layer = activeLayer();
    if (!layer || !layer->pixels) {
        d->discardUndoAction();
        return false;
    }
    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);
    pittore::RGBAf* px = layer->pixels->data();
    for (int dy = clipRect.top(); dy <= clipRect.bottom(); ++dy) {
        const uchar* mrow = cov.constScanLine(dy);
        for (int dx = clipRect.left(); dx <= clipRect.right(); ++dx) {
            const float c = mrow[dx] / 255.0f;
            if (c <= 0.0f) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            pittore::RGBAf& p = px[std::size_t(ly) * pw + lx];
            // Feathered edges lerp toward the complement by coverage.
            p.r += ((1.0f - p.r) - p.r) * c;
            p.g += ((1.0f - p.g) - p.g) * c;
            p.b += ((1.0f - p.b) - p.b) * c;
        }
    }
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer);
    layer->thumbnail = QImage();
    d->renderRegion(clipRect);
    d->commitUndoAction(tr("Invert"), QStringLiteral("invert"));
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setActiveLayerOpacity(int percent) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l) return false;
    percent = std::clamp(percent, 0, 100);
    if (l->opacity == percent) return true;
    beginUndoStep();
    activeLayer()->opacity = percent;
    commitUndoStep(tr("Layer Opacity"), QStringLiteral("opacity"));
    d->rebuildComposite();
    emit documentModified(d);
    emit layersChanged();
    return true;
}

bool AppState::setActiveLayerBlendMode(const QString& name) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l) return false;
    // The imported display alias for Linear Dodge (Add), same as MCP.
    const QString mode = name == QStringLiteral("Add")
                             ? QStringLiteral("Linear Dodge (Add)")
                             : name;
    if (!blendModeNames().contains(mode)) return false;
    if (l->blendMode == mode) return true;
    beginUndoStep();
    activeLayer()->blendMode = mode;
    commitUndoStep(tr("Blend Mode"), QStringLiteral("blend"));
    d->rebuildComposite();
    emit documentModified(d);
    emit layersChanged();
    return true;
}

QImage AppState::copyActiveLayerMasked(const QImage& mask, const QRectF& clip,
                                       QRectF* docRect) {
    if (docRect) *docRect = QRectF();
    DocumentItem* d = activeDocument();
    if (!d) return QImage();
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return QImage();
    const bool hasMask = !mask.isNull() &&
                         mask.size() == d->size &&
                         mask.format() == QImage::Format_Grayscale8;
    QRect srcRect = QRect(QPoint(0, 0), d->size);
    if (!clip.isEmpty())
        srcRect = clip.toAlignedRect().intersected(srcRect);
    if (srcRect.isEmpty()) return QImage();

    const int pw = static_cast<int>(layer->pixels->width());
    const int ph = static_cast<int>(layer->pixels->height());
    const double sx = std::max(layer->scaleX, 1e-6);
    const double sy = std::max(layer->scaleY, 1e-6);

    // Lift the layer's appearance inside srcRect, clipping each pixel by the
    // mask's coverage (same inverse-transform mapping as the erase path), so
    // placing the result back at srcRect reproduces exactly what the selection
    // covered at full resolution.
    QImage out(srcRect.size(), QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    for (int dy = srcRect.top(); dy <= srcRect.bottom(); ++dy) {
        const uchar* mrow = hasMask ? mask.constScanLine(dy) : nullptr;
        uchar* orow = out.scanLine(dy - srcRect.top());
        for (int dx = srcRect.left(); dx <= srcRect.right(); ++dx) {
            const int cov = hasMask ? mrow[dx] : 255;
            if (cov <= 0) continue;
            const double lxf = (dx + 0.5 - layer->offset.x()) / sx - 0.5;
            const double lyf = (dy + 0.5 - layer->offset.y()) / sy - 0.5;
            const int lx = std::clamp(int(std::lround(lxf)), 0, pw - 1);
            const int ly = std::clamp(int(std::lround(lyf)), 0, ph - 1);
            const pittore::RGBAf& p = layer->pixels->at(lx, ly);
            uchar* q = orow + std::size_t(dx - srcRect.left()) * 4;
            q[0] = static_cast<uchar>(std::clamp(int(p.r * 255.0f + 0.5f), 0, 255));
            q[1] = static_cast<uchar>(std::clamp(int(p.g * 255.0f + 0.5f), 0, 255));
            q[2] = static_cast<uchar>(std::clamp(int(p.b * 255.0f + 0.5f), 0, 255));
            q[3] = static_cast<uchar>(std::clamp(
                int(p.a * (cov / 255.0f) * 255.0f + 0.5f), 0, 255));
        }
    }
    if (docRect) *docRect = QRectF(srcRect);
    return out;
}

bool AppState::newDecontaminatedLayer(const QImage& coverage, bool withMask) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || layer->locked ||
        layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return false;
    if (coverage.isNull() || coverage.size() != d->size) return false;
    const QImage cov = coverage.format() == QImage::Format_Grayscale8
                           ? coverage
                           : coverage.convertToFormat(
                                 QImage::Format_Grayscale8);
    if (cov.isNull()) return false;
    // Tight bbox of covered pixels: the lift, the unmix and the new layer
    // all stay bbox-sized instead of document-sized.
    int x0 = cov.width(), y0 = cov.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < cov.height(); ++y) {
        const uchar* row = cov.constScanLine(y);
        for (int x = 0; x < cov.width(); ++x) {
            if (row[x] == 0) continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < x0) return false;
    const QRect clip =
        QRect(QPoint(x0, y0), QPoint(x1, y1))
            .intersected(QRect(QPoint(0, 0), d->size));
    if (clip.isEmpty()) return false;
    // Full appearance lift (no clipping mask: decontamination fills edge
    // colours from nearby confident-foreground pixels, and those pixels must
    // carry their raw photo color — lifting masked would leave them black
    // and tint every edge toward black).
    QRectF docRect;
    QImage lifted = copyActiveLayerMasked(QImage(), QRectF(clip), &docRect);
    if (lifted.isNull()) return false;
    const QRect liftedRect = docRect.toAlignedRect();
    if (lifted.size() != liftedRect.size()) return false;
    QImage covCrop = cov.copy(liftedRect);
    if (covCrop.size() != lifted.size()) return false;
    decontaminateStraightRgba(lifted, covCrop);
    if (!withMask) {
        // Plain output = a real cutout: the refined matte is baked into the
        // layer's alpha (background removed, same as Remove Background) with
        // decontaminated edge colors. The mask variant keeps the full lift
        // and rides the matte on the attached layer mask instead.
        for (int y = 0; y < lifted.height(); ++y) {
            const uchar* crow = covCrop.constScanLine(y);
            uchar* srow = lifted.scanLine(y);
            for (int x = 0; x < lifted.width(); ++x) {
                const int c = crow[x];
                uchar* px = srow + x * 4;
                if (c <= 0) {
                    px[3] = 0;
                } else if (c < 255) {
                    px[3] = static_cast<uchar>(
                        (static_cast<int>(px[3]) * c + 127) / 255);
                }
            }
        }
    }
    auto pixels = imageFromQImage(
        lifted.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    if (!pixels) {
        setStatusHint(tr("Could not add layer (unsupported or too large)."));
        return false;
    }
    beginUndoStep();
    LayerItem nl;
    nl.name = tr("Decontaminated");
    nl.kind = LayerItem::Kind::Pixel;
    nl.pixels = std::move(pixels);
    nl.offset = docRect.topLeft();
    nl.scaleX = 1.0;
    nl.scaleY = 1.0;
    ++nl.sourceStamp;
    if (withMask) {
        // Layer-native mask (identity placement, so native == doc pixels):
        // the refined coverage rides along, live and re-editable.
        const std::uint32_t mw = nl.pixels->width();
        const std::uint32_t mh = nl.pixels->height();
        auto mask = std::make_shared<pittore::Image>(mw, mh);
        for (std::uint32_t y = 0; y < mh; ++y) {
            const uchar* srow = covCrop.constScanLine(int(y));
            for (std::uint32_t x = 0; x < mw; ++x)
                mask->at(x, y) = pittore::compute::make_mask_pixel(
                    srow[x] / 255.0f);
        }
        nl.mask = std::move(mask);
        nl.hasMask = true;
        nl.maskLinked = true;
        nl.maskEnabled = true;
        nl.maskOffset = nl.offset;
        nl.maskScaleX = 1.0;
        nl.maskScaleY = 1.0;
        ++nl.maskStamp;
    }
    const int at = qBound(0, d->activeLayer, d->layers.size());
    d->layers.insert(at, std::move(nl));
    d->activeLayer = at;
    d->selectedLayers.clear();
    d->selectedLayers.push_back(at);
    d->rebuildComposite();
    commitUndoStep(tr("New Decontaminated Layer"), QStringLiteral("sparkle"));
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

}  // namespace pittore::ui
