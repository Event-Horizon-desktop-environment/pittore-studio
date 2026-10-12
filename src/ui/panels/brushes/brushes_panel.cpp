#include "ui/dpi_pixmap.h"
#include "ui/panels.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDataStream>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

#include "engine/compute/brushes/loaders/loaders.h"
#include "engine/compute/brushes/loaders/pat.h"
#include "engine/io/zip.h"
#include "ui/brushes/brush_library.h"
#include "ui/brushes/brush_preview.h"
#include "ui/brushes/bundle_import.h"
#include "ui/canvas/shared/canvas_helpers.h"
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>

#include "ui/ai_models.h"
#include "ui/icons.h"
#include "ui/tool_registry.h"

#include <chrono>

#include "engine/core/log.h"
#include "engine/compute/adjust.h"
#include "ui/curve_editor.h"
#include "ui/panels/shared/panel_helpers.h"

namespace pittore::ui {
namespace {

using namespace brushlibrary;

// Live tool options -> preview auto params (shared by both brush panels).
brushpreview::AutoParams autoParamsForTool(AppState* state, ToolId tool) {
    namespace bp = pittore::ui::brushpreview;
    bp::AutoParams a;
    auto opt = [&](const char* id, double fallback) {
        const QVariant v = state->option(tool, QString::fromUtf8(id));
        return v.isValid() ? v.toDouble() : fallback;
    };
    a.ratio = std::clamp(opt("brush_roundness", 100.0) / 100.0, 0.01, 1.0);
    a.angleDeg = std::clamp(opt("brush_angle", 0.0), -180.0, 180.0);
    a.square = opt("brush_tip", 0.0) == 1.0;
    a.hardness = std::clamp(opt("brush_hardness", 50.0) / 100.0, 0.0, 1.0);
    a.spacingPct = std::clamp(opt("brush_spacing", 15.0), 1.0, 200.0);
    const QVariant psv = state->option(tool, QStringLiteral("pressure_size"));
    a.pressureSize = !psv.isValid() || psv.toBool();
    const QVariant pov =
        state->option(tool, QStringLiteral("pressure_opacity"));
    a.pressureOpacity = !pov.isValid() || pov.toBool();
    a.opacity = std::clamp(opt("opacity", 100.0) / 100.0, 0.0, 1.0);
    a.flowPct = std::clamp(opt("flow", 100.0) / 100.0, 0.0, 1.0);
    a.sizeCurve =
        state->option(tool, QStringLiteral("brush_size_curve")).toString();
    a.opacityCurve =
        state->option(tool, QStringLiteral("brush_opacity_curve")).toString();
    a.flowCurve =
        state->option(tool, QStringLiteral("brush_flow_curve")).toString();
    a.spikes = std::clamp(
        state->option(tool, QStringLiteral("brush_spikes")).toInt(), 0, 12);
    a.fadeAniso = std::clamp(
        state->option(tool, QStringLiteral("brush_fade_aniso")).toDouble(),
        -100.0, 100.0);
    a.falloff = std::clamp(
        state->option(tool, QStringLiteral("brush_falloff")).toInt(), 0, 1);
    a.sharpness = std::clamp(
        state->option(tool, QStringLiteral("brush_sharpness")).toDouble(),
        0.0, 100.0);
    a.soften = std::clamp(
        state->option(tool, QStringLiteral("brush_soften")).toDouble(),
        0.0, 100.0);
    a.spacingAuto =
        state->option(tool, QStringLiteral("brush_spacing_auto")).toBool();
    a.spacingAutoCoeff = std::clamp(
        state->option(tool, QStringLiteral("brush_spacing_auto_coeff"))
            .toDouble(),
        0.0, 8.0);
    a.mixOp = pittore::compute::dabmix::mix_from_id(
        state->option(tool, QStringLiteral("brush_composite"))
            .toString()
            .toUtf8()
            .constData());
    return a;
}

// Owned grain tile for previews from a tool's live texture options.
brushpreview::BrushPreviewWidget::OwnedPattern patternForTool(AppState* state,
                                                             ToolId tool) {
    brushpreview::BrushPreviewWidget::OwnedPattern out;
    const QString id =
        state->option(tool, QStringLiteral("brush_texture")).toString();
    if (id.isEmpty()) return out;
    const AppState::PatternGray* pat = state->brushPattern(id);
    if (!pat) return out;
    auto dbl = [&](const char* key, double fallback) {
        const QVariant v = state->option(tool, QString::fromUtf8(key));
        return v.isValid() ? v.toDouble() : fallback;
    };
    out.gray = pat->gray;
    out.tex.gray = out.gray.data();
    out.tex.w = pat->w;
    out.tex.h = pat->h;
    out.tex.strength =
        float(std::clamp(dbl("brush_texture_strength", 80.0) / 100.0, 0.0, 1.0));
    const double sc = dbl("brush_texture_scale", 100.0) / 100.0;
    out.tex.scale = float(sc > 1e-6 ? sc : 1.0);
    out.tex.offsetX = 0.0f;
    out.tex.offsetY = 0.0f;
    out.tex.neutral =
        float(std::clamp(dbl("brush_texture_neutral", 50.0) / 100.0, 0.0, 1.0));
    out.tex.brightness =
        float(std::clamp(dbl("brush_texture_brightness", 0.0) / 100.0, -1.0, 1.0));
    out.tex.contrast =
        float(std::clamp(dbl("brush_texture_contrast", 100.0) / 100.0, 0.0, 4.0));
    out.tex.invert =
        state->option(tool, QStringLiteral("brush_texture_invert")).toBool();
    if (state->option(tool, QStringLiteral("brush_texture_auto_invert_eraser"))
            .toBool()) {
        const bool erasing =
            tool == ToolId::Eraser ||
            state->option(tool, QStringLiteral("brush_erase_blend")).toBool();
        if (erasing) out.tex.invert = !out.tex.invert;
    }
    out.tex.mode = std::clamp(
        state->option(tool, QStringLiteral("brush_texture_mode")).toInt(0), 0,
        6);
    out.tex.soft =
        state->option(tool, QStringLiteral("brush_texture_soft")).toBool();
    out.tex.cutoffPolicy = std::clamp(
        state->option(tool, QStringLiteral("brush_texture_cutoff_policy"))
            .toInt(0),
        0, 2);
    out.tex.cutLo = float(std::clamp(dbl("brush_texture_cutlo", 0.0), 0.0, 1.0));
    out.tex.cutHi = float(std::clamp(dbl("brush_texture_cuthi", 1.0), 0.0, 1.0));
    if (out.tex.cutHi < out.tex.cutLo)
        std::swap(out.tex.cutLo, out.tex.cutHi);
    // Paper-grain evaluator for bundle-imported textures: same tile, the
    // grain pipeline instead of the legacy combine.
    if (state->option(tool, QStringLiteral("brush_texture_grain")).toBool()) {
        out.grainGray = pat->gray;
        out.grain.gray = out.grainGray.data();
        out.grain.w = pat->w;
        out.grain.h = pat->h;
        out.grain.strength = out.tex.strength;
        out.grain.scale = out.tex.scale;
        out.grain.offsetX = 0.0f;
        out.grain.offsetY = 0.0f;
        out.grain.neutral = out.tex.neutral;
        out.grain.brightness = out.tex.brightness;
        out.grain.contrast = out.tex.contrast;
        out.grain.invert = out.tex.invert;
        out.grain.mode = static_cast<pittore::compute::papergrain::GrainMode>(
            std::clamp(out.tex.mode, 0, 2));
        out.grain.cutoffPolicy = out.tex.cutoffPolicy;
        out.grain.cutLo = out.tex.cutLo;
        out.grain.cutHi = out.tex.cutHi;
        out.grainOn = true;
    }
    return out;
}

// Full tool-based scratch render shared by both brush panels.
void renderToolPreview(brushpreview::BrushPreviewWidget* view, AppState* state,
                       ToolId tool) {
    const QColor fg = state->foreground();
    const auto a = autoParamsForTool(state, tool);
    const auto pat = patternForTool(state, tool);
    const bool smudge =
        state->option(tool, QStringLiteral("brush_engine")).toString() ==
        QStringLiteral("smudge");
    const QVariant rv =
        state->option(tool, QStringLiteral("brush_smudge_rate"));
    const double rate =
        std::clamp(rv.isValid() ? rv.toDouble() : 70.0, 0.0, 100.0) / 100.0;
    const QString hoseId =
        state->option(tool, QStringLiteral("brush_hose")).toString();
    const auto* hose =
        hoseId.isEmpty() ? nullptr : state->brushHose(hoseId);
    if (hose && !hose->cells.empty() && !smudge) {
        std::vector<pittore::compute::StampTip> cells;
        for (const auto& c : hose->cells) cells.push_back(c.tip);
        view->setHose(cells, QString::fromStdString(hose->selection), 12345,
                      state->option(tool, QStringLiteral("brush_stamp_mode"))
                          .toInt(),
                      a.angleDeg, fg, &pat, a);
        return;
    }
    const QString stampId =
        state->option(tool, QStringLiteral("brush_stamp")).toString();
    const auto* stamp =
        stampId.isEmpty() ? nullptr : state->brushStamp(stampId);
    const int mode =
        state->option(tool, QStringLiteral("brush_stamp_mode")).toInt();
    if (stamp) {
        if (smudge) {
            auto dbl = [&](const char* key, double fallback) {
                const QVariant v = state->option(tool, QString::fromUtf8(key));
                return v.isValid() ? v.toDouble() : fallback;
            };
            view->setSmudgeStamp(
                *stamp, rate, a.angleDeg, fg, &pat, a,
                qBound(0,
                       state->option(tool, QStringLiteral("smudge_mode"))
                           .toInt(),
                       1),
                qBound(0.0, dbl("smudge_color_rate", 0.0), 100.0) / 100.0,
                qBound(0.0, dbl("smudge_length", 100.0), 200.0) / 100.0);
        } else {
            auto dbl = [&](const char* key, double fallback) {
                const QVariant v = state->option(tool, QString::fromUtf8(key));
                return v.isValid() ? v.toDouble() : fallback;
            };
            view->setStamp(*stamp, mode, a.angleDeg, fg, &pat, a,
                           state->background(),
                           dbl("brush_tip_neutral", 50.0),
                           dbl("brush_tip_brightness", 0.0),
                           dbl("brush_tip_contrast", 100.0));
        }
        return;
    }
    if (smudge) {
        auto dbl = [&](const char* key, double fallback) {
            const QVariant v = state->option(tool, QString::fromUtf8(key));
            return v.isValid() ? v.toDouble() : fallback;
        };
        view->setSmudgeAuto(
            a, rate, fg, &pat,
            qBound(0,
                   state->option(tool, QStringLiteral("smudge_mode")).toInt(),
                   1),
            qBound(0.0, dbl("smudge_color_rate", 0.0), 100.0) / 100.0,
            qBound(0.0, dbl("smudge_length", 100.0), 200.0) / 100.0);
    } else
        view->setAuto(a, fg, &pat);
}

class BrushesPanel final : public QWidget {

  public:
    BrushesPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, 4, 0, 0);
        auto* tagRow = new QWidget(this);
        auto* tagLayout = new QHBoxLayout(tagRow);
        tagLayout->setContentsMargins(0, 0, 0, 0);
        tagLayout->addWidget(new QLabel(tr("Tag"), tagRow));
        tagFilter_ = new QComboBox(tagRow);
        tagFilter_->setObjectName(QStringLiteral("brushTagFilter"));
        tagLayout->addWidget(tagFilter_, 1);
        column->addWidget(tagRow);
        QObject::connect(
            tagFilter_,
            static_cast<void (QComboBox::*)(int)>(
                &QComboBox::currentIndexChanged),
            this, [this](int) { applyTagFilter(); });
        tree_ = new QTreeWidget(this);
        tree_->setHeaderHidden(true);
        tree_->setIndentation(12);
        tree_->setIconSize(QSize(40, 40));
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        column->addWidget(tree_, 1);
        preview_ = new brushpreview::BrushPreviewWidget(this);
        column->addWidget(preview_);
        column->addWidget(makePanelFooter(
            state_,
            {{QStringLiteral("newlayer"), tr("Create new brush")},
             {QStringLiteral("trash"), tr("Delete brush")},
             {QStringLiteral("import"), tr("Import brush…")}},
            this, [this](const QString& id) {
                if (id == QStringLiteral("newlayer"))
                    createFromCurrent();
                else if (id == QStringLiteral("trash"))
                    deleteSelected();
                else if (id == QStringLiteral("import"))
                    importTips();
            }));
        presets_ = factoryBrushPresets();
        loadCustom();
        registerStamps();
        registerPatterns();
        registerHoses();
        rebuild();
        connect(tree_, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem* item, int) {
                    applyItem(item);
                    updatePreview();
                });
        connect(tree_, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) { contextMenu(pos); });
        connect(state_, &AppState::optionChanged, this,
                [this](ToolId, const QString&, const QVariant&) {
                    updatePreview();
                });
        updatePreview();
    }

  private:
    void applyItem(QTreeWidgetItem* item) {
        if (!item || item->parent() == nullptr) return;  // group header
        const int idx = item->data(0, Qt::UserRole).toInt();
        if (idx < 0 || idx >= (int)presets_.size()) return;
        const BrushPreset& p = presets_.at(idx);
        const ApplyResult r = applyPreset(state_, p);
        if (r == ApplyResult::StampMissing) {
            state_->setStatusHint(
                tr("Brush preset: %1 (stamp file missing — auto tip used).")
                    .arg(p.name));
            return;
        }
        state_->setStatusHint(tr("Brush preset: %1.").arg(p.name));
    }

    void createFromCurrent() {
        const ToolId tool = presetTarget(state_);
        auto opt = [&](const char* id, double fallback) {
            const QVariant v = state_->option(tool, QString::fromUtf8(id));
            return v.isValid() ? v.toDouble() : fallback;
        };
        BrushPreset p;
        int n = 1;
        for (const auto& q : presets_)
            if (!q.factory) ++n;
        p.name = tr("Custom %1").arg(n);
        p.size = opt("brush_size", 64.0);
        p.hardness = opt("brush_hardness", 50.0);
        p.angle = opt("brush_angle", 0.0);
        p.roundness = opt("brush_roundness", 100.0);
        p.spacing = opt("brush_spacing", 15.0);
        p.tip = (int)opt("brush_tip", 0.0);
        p.spikes = qBound(
            0, state_->option(tool, QStringLiteral("brush_spikes")).toInt(),
            12);
        p.fadeAniso = qBound(-100.0, opt("brush_fade_aniso", 0.0), 100.0);
        p.falloff = qBound(
            0, state_->option(tool, QStringLiteral("brush_falloff")).toInt(),
            1);
        p.sharpness = opt("brush_sharpness", 0.0);
        p.soften = opt("brush_soften", 0.0);
        p.spacingAuto =
            state_->option(tool, QStringLiteral("brush_spacing_auto"))
                .toBool();
        p.spacingAutoCoeff = std::clamp(
            state_->option(tool, QStringLiteral("brush_spacing_auto_coeff"))
                .toDouble(),
            0.0, 8.0);
        p.opacity = opt("opacity", 100.0);
        p.tipKind = state_->option(tool, QStringLiteral("brush_stamp"))
                        .toString()
                        .isEmpty()
                        ? QString()
                        : QStringLiteral("stamp");
        p.stampId = state_->option(tool, QStringLiteral("brush_stamp"))
                        .toString();
        p.stampMode = qBound(
            0,
            state_->option(tool, QStringLiteral("brush_stamp_mode")).toInt(),
            3);
        p.tipNeutral = opt("brush_tip_neutral", 50.0);
        p.tipBrightness = opt("brush_tip_brightness", 0.0);
        p.tipContrast = opt("brush_tip_contrast", 100.0);
        p.tipThickness = opt("brush_tip_thickness", 0.0);
        const QVariant psv =
            state_->option(tool, QStringLiteral("pressure_size"));
        p.pressureSize = !psv.isValid() || psv.toBool();
        const QVariant pov =
            state_->option(tool, QStringLiteral("pressure_opacity"));
        p.pressureOpacity = !pov.isValid() || pov.toBool();
        p.sizeCurve =
            state_->option(tool, QStringLiteral("brush_size_curve")).toString();
        p.opacityCurve =
            state_->option(tool, QStringLiteral("brush_opacity_curve"))
                .toString();
        p.flowCurve =
            state_->option(tool, QStringLiteral("brush_flow_curve")).toString();
        p.engine = state_->option(tool, QStringLiteral("brush_engine"))
                       .toString();
        p.smudgeRate = opt("brush_smudge_rate", 70.0);
        p.smudgeRadius = opt("brush_smudge_radius", 100.0);
        p.smudgeMode = qBound(
            0, state_->option(tool, QStringLiteral("smudge_mode")).toInt(),
            1);
        p.smudgeColorRate = opt("smudge_color_rate", 0.0);
        p.smudgeLength = opt("smudge_length", 100.0);
        p.drives = sensordrive::decodeDrives(
            state_->option(tool, QStringLiteral("sensor_drives")).toString());
        p.scatterPct = opt("brush_scatter", 0.0);
        {
            const QVariant xv =
                state_->option(tool, QStringLiteral("brush_scatter_x"));
            p.scatterX = !xv.isValid() || xv.toBool();
            const QVariant yv =
                state_->option(tool, QStringLiteral("brush_scatter_y"));
            p.scatterY = !yv.isValid() || yv.toBool();
        }
        p.densityPct = opt("brush_density", 100.0);
        p.airbrush =
            state_->option(tool, QStringLiteral("airbrush")).toBool();
        p.airbrushRate = opt("airbrush_rate", 20.0);
        p.flowPct = opt("flow", 100.0);
        p.tiltRotation =
            state_->option(tool, QStringLiteral("tilt_rotation")).toBool();
        p.textureFile =
            state_->option(tool, QStringLiteral("brush_texture")).toString();
        p.textureStrength = opt("brush_texture_strength", 80.0);
        p.textureScale = opt("brush_texture_scale", 100.0);
        p.textureNeutral = opt("brush_texture_neutral", 50.0);
        p.textureBrightness = opt("brush_texture_brightness", 0.0);
        p.textureContrast = opt("brush_texture_contrast", 100.0);
        p.textureInvert =
            state_->option(tool, QStringLiteral("brush_texture_invert"))
                .toBool();
        p.textureGrain =
            state_->option(tool, QStringLiteral("brush_texture_grain"))
                .toBool();
        p.textureGrain =
            state_->option(tool, QStringLiteral("brush_texture_grain"))
                .toBool();
        p.hoseId = state_->option(tool, QStringLiteral("brush_hose"))
                       .toString();
        p.paintingMode =
            state_->option(tool, QStringLiteral("brush_painting_mode"))
                .toString();
        p.rotationMode = qBound(
            0, state_->option(tool, QStringLiteral("brush_rotation")).toInt(0),
            5);
        if (p.rotationMode == 0 &&
            state_->option(tool, QStringLiteral("tilt_rotation")).toBool())
            p.rotationMode = 1;  // legacy toggle
        p.rotationCurve =
            state_->option(tool, QStringLiteral("brush_rotation_curve"))
                .toString();
        p.sourceMode = qBound(
            0, state_->option(tool, QStringLiteral("brush_source")).toInt(0),
            3);
        p.texturePressure =
            state_->option(tool, QStringLiteral("brush_texture_pressure"))
                .toBool();
        p.maskPressure =
            state_->option(tool, QStringLiteral("brush_mask_pressure"))
                .toBool();
        p.smudgePressure =
            state_->option(tool, QStringLiteral("brush_smudge_pressure"))
                .toBool();
        p.gradientLen = opt("brush_gradient_len", 500.0);
        p.spacingIsotropic =
            state_->option(tool, QStringLiteral("brush_spacing_isotropic"))
                .toBool();
        p.maskStamp =
            state_->option(tool, QStringLiteral("brush_mask_stamp")).toString();
        p.maskMode = qBound(
            0,
            state_->option(tool, QStringLiteral("brush_mask_mode")).toInt(0),
            3);
        p.maskRatio = opt("brush_mask_ratio", 100.0);
        p.maskAngle = opt("brush_mask_angle", 0.0);
        p.textureMode = qBound(
            0,
            state_->option(tool, QStringLiteral("brush_texture_mode"))
                .toInt(0),
            6);
        p.textureSoft =
            state_->option(tool, QStringLiteral("brush_texture_soft"))
                .toBool();
        p.textureAutoInvertEraser =
            state_->option(tool,
                           QStringLiteral("brush_texture_auto_invert_eraser"))
                .toBool();
        p.textureCutoffPolicy = qBound(
            0,
            state_->option(tool, QStringLiteral("brush_texture_cutoff_policy"))
                .toInt(0),
            2);
        p.textureCutLo = opt("brush_texture_cutlo", 0.0);
        p.textureCutHi = opt("brush_texture_cuthi", 1.0);
        p.flipX = state_->option(tool, QStringLiteral("brush_flip_x")).toBool();
        p.flipY = state_->option(tool, QStringLiteral("brush_flip_y")).toBool();
        p.tiltSize = opt("brush_tilt_size", 0.0);
        p.tiltOpacity = opt("brush_tilt_opacity", 0.0);
        p.tangentialFlow =
            state_->option(tool, QStringLiteral("brush_tangential_flow"))
                .toBool();
        p.smoothing = opt("smoothing", 0.0);
        p.smoothingMode = qBound(
            0, state_->option(tool, QStringLiteral("smoothing_mode"))
                   .toInt(0),
            2);
        {
            const QVariant tf =
                state_->option(tool, QStringLiteral("brush_tip_filter"));
            p.tipFilter = !tf.isValid() ? 1 : qBound(0, tf.toInt(), 1);
        }
        p.eraserBlend =
            state_->option(tool, QStringLiteral("brush_erase_blend"))
                .toBool();
        p.fadeLen = opt("brush_fade", 0.0);
        p.darkenPct = opt("brush_darken", 0.0);
        p.hueJitter = opt("brush_hue_jitter", 0.0);
        p.satJitter = opt("brush_sat_jitter", 0.0);
        p.valJitter = opt("brush_val_jitter", 0.0);
        p.pressureIn =
            state_->option(tool, QStringLiteral("brush_pressure_in"))
                .toBool();
        p.speedSize = opt("brush_speed_size", 0.0);
        p.tiltXSize = opt("brush_tiltx_size", 0.0);
        p.tiltYSize = opt("brush_tilty_size", 0.0);
        p.timeFade = opt("brush_timefade", 0.0);
        p.fuzzySize = opt("brush_fuzzy_size", 0.0);
        p.fuzzyOpacity = opt("brush_fuzzy_opacity", 0.0);
        p.perspective = opt("brush_perspective", 0.0);
        p.vpX = opt("brush_vp_x", -1.0);
        p.vpY = opt("brush_vp_y", -1.0);
        // Hose cell lists live on the hose presets themselves; a plain
        // snapshot keeps the reference only when it names a known hose.
        if (!p.hoseId.isEmpty()) {
            for (const auto& q : presets_) {
                if (!q.hoseId.isEmpty() && q.hoseId == p.hoseId) {
                    p.hoseCells = q.hoseCells;
                    p.hoseSelection = q.hoseSelection;
                    break;
                }
            }
            if (p.hoseCells.isEmpty()) p.hoseId.clear();
        }
        presets_.push_back(p);
        saveCustom();
        rebuild();
    }

    void deleteSelected() {
        auto* item = tree_->currentItem();
        if (!item || item->parent() == nullptr) return;
        const int idx = item->data(0, Qt::UserRole).toInt();
        if (idx < 0 || idx >= (int)presets_.size() || presets_.at(idx).factory)
            return;  // factory presets are protected
        const QString deadId = presets_.at(idx).isStamp()
                                   ? presets_.at(idx).stampId
                                   : QString();
        const QString deadTexture = presets_.at(idx).textureFile;
        const QStringList deadCells = presets_.at(idx).hoseCells;
        const QString deadMask = presets_.at(idx).maskStamp;
        presets_.erase(presets_.begin() + idx);
        auto stillUsed = [&](const QString& id) {
            for (const auto& q : presets_) {
                if (q.isStamp() && q.stampId == id) return true;
                if (q.hoseCells.contains(id)) return true;
                if (q.maskStamp == id) return true;
            }
            return false;
        };
        if (!deadId.isEmpty() && !stillUsed(deadId)) {
            state_->clearBrushStamp(deadId);
            QFile::remove(QDir(brushTipsDir()).filePath(deadId));
        }
        if (!deadMask.isEmpty() && deadMask != deadId &&
            !stillUsed(deadMask)) {
            state_->clearBrushStamp(deadMask);
            QFile::remove(QDir(brushTipsDir()).filePath(deadMask));
        }
        for (const QString& cellId : deadCells) {
            if (!stillUsed(cellId)) {
                state_->clearBrushStamp(cellId);
                QFile::remove(QDir(brushTipsDir()).filePath(cellId));
            }
        }
        if (!deadTexture.isEmpty()) {
            bool used = false;
            for (const auto& q : presets_) {
                if (q.textureFile == deadTexture) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                state_->clearBrushPattern(deadTexture);
                QFile::remove(QDir(brushPatternsDir()).filePath(deadTexture));
            }
        }
        registerHoses();
        saveCustom();
        rebuild();
    }

    void contextMenu(const QPoint& pos) {
        auto* item = tree_->itemAt(pos);
        if (!item || item->parent() == nullptr) return;
        const int idx = item->data(0, Qt::UserRole).toInt();
        if (idx < 0 || idx >= (int)presets_.size()) return;
        QMenu menu(tree_);
        QAction* dup = menu.addAction(tr("Duplicate"));
        QAction* ren = menu.addAction(tr("Rename…"));
        QAction* del = menu.addAction(tr("Delete"));
        if (presets_.at(idx).factory) del->setEnabled(false);
        QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
        if (chosen == dup) {
            BrushPreset p = presets_.at(idx);
            p.factory = false;
            p.name += tr(" copy");
            presets_.push_back(p);
            saveCustom();
            rebuild();
        } else if (chosen == ren) {
            if (presets_.at(idx).factory) return;
            bool ok = false;
            const QString name = QInputDialog::getText(
                tree_, tr("Rename brush"), tr("Name:"), QLineEdit::Normal,
                presets_.at(idx).name, &ok);
            if (ok && !name.trimmed().isEmpty()) {
                presets_.at(idx).name = name.trimmed();
                saveCustom();
                rebuild();
            }
        } else if (chosen == del) {
            deleteSelected();
        }
    }

    void rebuild() {
        namespace bp = pittore::ui::brushpreview;
        tree_->clear();
        auto* factory = new QTreeWidgetItem(tree_, {tr("Factory")});
        factory->setData(0, Qt::UserRole, QStringLiteral("group-factory"));
        auto* custom = new QTreeWidgetItem(tree_, {tr("Custom")});
        custom->setData(0, Qt::UserRole, QStringLiteral("group-custom"));
        for (int i = 0; i < (int)presets_.size(); ++i) {
            const BrushPreset& p = presets_.at(i);
            auto* leaf = new QTreeWidgetItem(
                p.factory ? factory : custom, {p.name});
            leaf->setData(0, Qt::UserRole, i);
            // Dab thumbnail, rendered with the real kernel: auto tips from
            // the preset fields, stamps from the registered tip.
            QImage thumb;
            const pittore::compute::StampTip* stamp =
                p.isStamp() ? state_->brushStamp(p.stampId) : nullptr;
            if (stamp) {
                thumb = bp::tipThumbStamp(
                    *stamp, p.stampMode, 56, true, state_->background(),
                    p.tipNeutral, p.tipBrightness, p.tipContrast);
            } else {
                thumb = bp::tipThumbAuto(presetAuto(p));
            }
            leaf->setIcon(0, QIcon(pixmapForWidget(thumb, leaf->treeWidget())));
        }
        factory->setExpanded(true);
        custom->setExpanded(true);
        refreshTagFilter();
        applyTagFilter();
        updatePreview();
    }

    // Tag filter options: All, Factory, then every custom tag in use.
    // The reference groups presets by tags rather than renaming them, so
    // artist prefixes ("c) ...") stay verbatim while the list stays browsable.
    void refreshTagFilter() {
        if (!tagFilter_) return;
        const QString keep = tagFilter_->currentText();
        tagFilter_->blockSignals(true);
        tagFilter_->clear();
        tagFilter_->addItem(tr("All tags"));
        tagFilter_->addItem(tr("Factory"));
        QStringList tags;
        for (const auto& p : presets_) {
            if (p.factory) continue;
            for (const QString& t : p.tags) {
                if (!t.isEmpty() && !tags.contains(t)) tags.push_back(t);
            }
        }
        std::sort(tags.begin(), tags.end());
        for (const QString& t : tags) tagFilter_->addItem(t);
        const int back = tagFilter_->findText(keep);
        tagFilter_->setCurrentIndex(back >= 0 ? back : 0);
        tagFilter_->blockSignals(false);
    }

    void applyTagFilter() {
        if (!tagFilter_) return;
        const QString sel = tagFilter_->currentText();
        const bool all = sel == tr("All tags");
        for (int g = 0; g < tree_->topLevelItemCount(); ++g) {
            auto* group = tree_->topLevelItem(g);
            const bool isFactory =
                group->data(0, Qt::UserRole).toString() ==
                QStringLiteral("group-factory");
            bool anyVisible = false;
            for (int r = 0; r < group->childCount(); ++r) {
                auto* leaf = group->child(r);
                const int idx = leaf->data(0, Qt::UserRole).toInt();
                bool show = all;
                if (!show) {
                    if (isFactory) {
                        show = sel == tr("Factory");
                    } else if (idx >= 0 && idx < (int)presets_.size()) {
                        show = presets_.at(idx).tags.contains(sel);
                    }
                }
                leaf->setHidden(!show);
                anyVisible = anyVisible || show;
            }
            group->setHidden(!anyVisible);
        }
    }

    // Preset fields -> preview auto params.
    static pittore::ui::brushpreview::AutoParams presetAuto(
        const BrushPreset& p) {
        namespace bp = pittore::ui::brushpreview;
        bp::AutoParams a;
        a.ratio = std::clamp(p.roundness / 100.0, 0.01, 1.0);
        a.angleDeg = p.angle;
        a.square = p.tip == 1;
        a.hardness = std::clamp(p.hardness / 100.0, 0.0, 1.0);
        a.spacingPct = std::clamp(p.spacing, 1.0, 200.0);
        a.pressureSize = p.pressureSize;
        a.pressureOpacity = p.pressureOpacity;
        a.opacity = std::clamp(p.opacity / 100.0, 0.0, 1.0);
        a.flowPct = std::clamp(p.flowPct / 100.0, 0.0, 1.0);
        a.sizeCurve = p.sizeCurve;
        a.opacityCurve = p.opacityCurve;
        a.flowCurve = p.flowCurve;
        a.spikes = std::clamp(p.spikes, 0, 12);
        a.fadeAniso = std::clamp(p.fadeAniso, -100.0, 100.0);
        a.falloff = std::clamp(p.falloff, 0, 1);
        a.sharpness = std::clamp(p.sharpness, 0.0, 100.0);
        a.soften = std::clamp(p.soften, 0.0, 100.0);
        a.spacingAuto = p.spacingAuto;
        a.spacingAutoCoeff = std::clamp(p.spacingAutoCoeff, 0.0, 8.0);
        a.mixOp = pittore::compute::dabmix::mix_from_id(
            p.compositeOp.toUtf8().constData());
        return a;
    }

    // Builds an owned grain tile for previews from a preset's texture file.
    brushpreview::BrushPreviewWidget::OwnedPattern patternFor(
        const BrushPreset& p) const {
        brushpreview::BrushPreviewWidget::OwnedPattern out;
        if (p.textureFile.isEmpty()) return out;
        const AppState::PatternGray* pat = state_->brushPattern(p.textureFile);
        if (!pat) return out;
        out.gray = pat->gray;
        out.tex.gray = out.gray.data();
        out.tex.w = pat->w;
        out.tex.h = pat->h;
        out.tex.strength =
            float(std::clamp(p.textureStrength / 100.0, 0.0, 1.0));
        const double sc = p.textureScale / 100.0;
        out.tex.scale = float(sc > 1e-6 ? sc : 1.0);
        out.tex.offsetX = 0.0f;
        out.tex.offsetY = 0.0f;
        out.tex.neutral = float(std::clamp(p.textureNeutral / 100.0, 0.0, 1.0));
        out.tex.brightness =
            float(std::clamp(p.textureBrightness / 100.0, -1.0, 1.0));
        out.tex.contrast =
            float(std::clamp(p.textureContrast / 100.0, 0.0, 4.0));
        out.tex.invert = p.textureInvert;
        out.tex.mode = std::clamp(p.textureMode, 0, 6);
        out.tex.soft = p.textureSoft;
        out.tex.cutoffPolicy = std::clamp(p.textureCutoffPolicy, 0, 2);
        out.tex.cutLo = float(std::clamp(p.textureCutLo, 0.0, 1.0));
        out.tex.cutHi = float(std::clamp(p.textureCutHi, 0.0, 1.0));
        if (out.tex.cutHi < out.tex.cutLo)
            std::swap(out.tex.cutLo, out.tex.cutHi);
        if (p.textureGrain) {
            out.grainGray = pat->gray;
            out.grain.gray = out.grainGray.data();
            out.grain.w = pat->w;
            out.grain.h = pat->h;
            out.grain.strength = out.tex.strength;
            out.grain.scale = out.tex.scale;
            out.grain.offsetX = 0.0f;
            out.grain.offsetY = 0.0f;
            out.grain.neutral = out.tex.neutral;
            out.grain.brightness = out.tex.brightness;
            out.grain.contrast = out.tex.contrast;
            out.grain.invert = out.tex.invert;
            out.grain.mode =
                static_cast<pittore::compute::papergrain::GrainMode>(
                    std::clamp(p.textureMode, 0, 2));
            out.grain.cutoffPolicy = out.tex.cutoffPolicy;
            out.grain.cutLo = out.tex.cutLo;
            out.grain.cutHi = out.tex.cutHi;
            out.grainOn = true;
        }
        return out;
    }

    // The scratch strip follows the selection; with nothing selected it
    // previews the current tool's live options.
    void updatePreview() {
        namespace bp = pittore::ui::brushpreview;
        const QColor fg = state_->foreground();
        auto* item = tree_->currentItem();
        if (item && item->parent() != nullptr) {
            const int idx = item->data(0, Qt::UserRole).toInt();
            if (idx >= 0 && idx < (int)presets_.size()) {
                const BrushPreset& p = presets_.at(idx);
                const auto pat = patternFor(p);
                const bool smudge = p.engine == QStringLiteral("smudge");
                const pittore::compute::brushload::LoadedHose* hose =
                    p.hoseId.isEmpty() ? nullptr
                                       : state_->brushHose(p.hoseId);
                if (hose && !hose->cells.empty() && !smudge) {
                    std::vector<pittore::compute::StampTip> cells;
                    for (const auto& c : hose->cells) cells.push_back(c.tip);
                    preview_->setHose(cells,
                                      QString::fromStdString(hose->selection),
                                      12345, p.stampMode, p.angle, fg, &pat,
                                      presetAuto(p));
                    return;
                }
                const pittore::compute::StampTip* stamp =
                    p.isStamp() ? state_->brushStamp(p.stampId) : nullptr;
                if (stamp) {
                    if (smudge)
                        preview_->setSmudgeStamp(
                            *stamp, p.smudgeRate / 100.0, p.angle, fg, &pat,
                            presetAuto(p), std::clamp(p.smudgeMode, 0, 1),
                            std::clamp(p.smudgeColorRate, 0.0, 100.0) / 100.0,
                            std::clamp(p.smudgeLength, 0.0, 200.0) / 100.0);
                    else
                        preview_->setStamp(*stamp, p.stampMode, p.angle, fg,
                                           &pat, presetAuto(p),
                                           state_->background(), p.tipNeutral,
                                           p.tipBrightness, p.tipContrast);
                    return;
                }
                if (smudge)
                    preview_->setSmudgeAuto(
                        presetAuto(p), p.smudgeRate / 100.0, fg, &pat,
                        std::clamp(p.smudgeMode, 0, 1),
                        std::clamp(p.smudgeColorRate, 0.0, 100.0) / 100.0,
                        std::clamp(p.smudgeLength, 0.0, 200.0) / 100.0);
                else
                    preview_->setAuto(presetAuto(p), fg, &pat);
                return;
            }
        }
        renderToolPreview(preview_, state_, presetTarget(state_));
    }

    // --- stamp-tip import (user files only; nothing third-party ships) -----
    // Every imported tip is converted to a PNG under the config brushes dir
    // and registered in the AppState stamp library; presets reference it by
    // file name.
    void registerStamps() {
        const QDir dir(brushTipsDir());
        for (const auto& p : presets_) {
            if (!p.isStamp() || state_->brushStamp(p.stampId)) continue;
            const QImage img(dir.filePath(p.stampId));
            if (img.isNull()) continue;  // missing file: auto-tip fallback
            pittore::compute::StampTip tip;
            if (stampFromImage(img, &tip)) state_->setBrushStamp(p.stampId, tip);
        }
    }

    // QImage -> paper grain (luma tile, capped like stamps).
    static bool patternFromImage(const QImage& src, AppState::PatternGray* pat) {
        QImage img = src;
        if (img.width() > 512 || img.height() > 512)
            img = img.scaled(512, 512, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        img = img.convertToFormat(QImage::Format_ARGB32);
        const int w = img.width(), h = img.height();
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
        pat->w = (std::uint32_t)w;
        pat->h = (std::uint32_t)h;
        pat->gray.assign(std::size_t(w) * h, 1.0f);
        for (int y = 0; y < h; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const float luma =
                    (0.299f * qRed(row[x]) + 0.587f * qGreen(row[x]) +
                     0.114f * qBlue(row[x])) /
                    255.0f;
                // Alpha thins the grain (transparent paper = no tooth).
                pat->gray[std::size_t(y) * w + x] =
                    luma * (qAlpha(row[x]) / 255.0f);
            }
        }
        return pat->valid();
    }

    // Store one pattern PNG + register it. Empty on failure.
    QString storePattern(const QString& baseName, const QImage& image) {
        AppState::PatternGray pat;
        if (!patternFromImage(image, &pat)) return QString();
        QDir().mkpath(brushPatternsDir());
        QString safe = baseName;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")),
                     QStringLiteral("_"));
        if (safe.isEmpty()) safe = QStringLiteral("grain");
        QString fileName;
        for (int n = 0; n < 1000; ++n) {
            fileName = n == 0 ? safe + QStringLiteral(".png")
                              : safe + QStringLiteral("-%1.png").arg(n);
            if (!QFile::exists(QDir(brushPatternsDir()).filePath(fileName))) break;
        }
        if (!image.scaled(512, 512, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation)
                 .save(QDir(brushPatternsDir()).filePath(fileName), "PNG"))
            return QString();
        state_->setBrushPattern(fileName, std::move(pat));
        return fileName;
    }

    void registerPatterns() {
        const QDir dir(brushPatternsDir());
        for (const auto& p : presets_) {
            if (p.textureFile.isEmpty() ||
                state_->brushPattern(p.textureFile))
                continue;
            const QImage img(dir.filePath(p.textureFile));
            if (img.isNull()) continue;  // missing file: grain off
            AppState::PatternGray pat;
            if (patternFromImage(img, &pat))
                state_->setBrushPattern(p.textureFile, std::move(pat));
        }
    }

    // Rebuild the hose library from hose presets (cells load from their
    // stamp PNGs). Missing cells drop the hose; strokes fall back to auto.
    void registerHoses() {
        using namespace pittore::compute::brushload;
        state_->clearBrushHoses();
        const QDir dir(brushTipsDir());
        for (const auto& p : presets_) {
            if (p.hoseId.isEmpty() || p.hoseCells.isEmpty()) continue;
            LoadedHose hose;
            hose.name = p.name.toStdString();
            hose.step = 20;
            hose.selection = p.hoseSelection.isEmpty()
                                 ? "incremental"
                                 : p.hoseSelection.toStdString();
            hose.ok = true;
            for (const QString& cellId : p.hoseCells) {
                const pittore::compute::StampTip* tip =
                    state_->brushStamp(cellId);
                if (!tip) {
                    // Cell PNG present but never registered (e.g. older
                    // library): load it now.
                    const QImage img(dir.filePath(cellId));
                    pittore::compute::StampTip loaded;
                    if (!img.isNull() && stampFromImage(img, &loaded))
                        state_->setBrushStamp(cellId, loaded);
                    tip = state_->brushStamp(cellId);
                }
                if (!tip) {
                    hose.ok = false;
                    break;
                }
                LoadedTip cell;
                cell.tip = *tip;
                cell.name = cellId.toStdString();
                cell.ok = true;
                hose.cells.push_back(std::move(cell));
            }
            if (hose.ok && !hose.cells.empty())
                state_->setBrushHose(p.hoseId, hose);
        }
    }

    // Build one hose preset from decoded cell images (first cell doubles as
    // the list thumbnail). Returns false when no cell survived.
    bool addHosePreset(const QString& baseName, const QStringList& cellNames,
                       const QList<QImage>& cellImages,
                       const QString& selection, double spacingPct) {
        QStringList cellIds;
        for (int i = 0; i < cellImages.size(); ++i) {
            pittore::compute::StampTip tip;
            if (!stampFromImage(cellImages[i], &tip)) continue;
            tip.spacingPct = std::clamp(float(spacingPct), 1.0f, 200.0f);
            const QString id = storeStampTip(
                cellNames[i].isEmpty() ? baseName + QStringLiteral("-cell%1").arg(i)
                                       : cellNames[i],
                tip);
            if (!id.isEmpty()) cellIds.push_back(id);
        }
        if (cellIds.isEmpty()) return false;
        BrushPreset p;
        p.name = baseName;
        p.size = 64.0;
        p.spacing = spacingPct;
        p.hoseSelection =
            selection.isEmpty() ? QStringLiteral("incremental") : selection;
        p.hoseCells = cellIds;
        QString safe = baseName;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")),
                     QStringLiteral("_"));
        if (safe.isEmpty()) safe = QStringLiteral("hose");
        QString hoseId = QStringLiteral("hose-") + safe;
        for (int n = 1; n < 1000; ++n) {
            bool taken = false;
            for (const auto& q : presets_) {
                if (q.hoseId == hoseId) {
                    taken = true;
                    break;
                }
            }
            if (!taken) break;
            hoseId = QStringLiteral("hose-") + safe +
                     QStringLiteral("-%1").arg(n);
        }
        p.hoseId = hoseId;
        // First cell's look doubles for the list icon; the stroke cycles.
        p.tipKind = QStringLiteral("stamp");
        p.stampId = cellIds.front();
        presets_.push_back(p);
        registerHoses();
        return true;
    }

    // QImage -> StampTip. Alpha channel (when present) is coverage; opaque
    // images use 1-luma (dark paints, the paint-mask convention). Color
    // is kept only when the tip is genuinely colorful (spread > 8) and has
    // alpha; anything else becomes an alpha mask tinted by the foreground.
    static bool stampFromImage(const QImage& src,
                               pittore::compute::StampTip* tip) {
        QImage img = src;
        // Stamps keep full detail up to the kernel limit: a small tip
        // scaled up to a large dab is what reads as blurry blobs at
        // high zoom. (Grain tiles stay capped lower; they repeat.)
        if (img.width() > 1024 || img.height() > 1024)
            img = img.scaled(1024, 1024, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        img = img.convertToFormat(QImage::Format_ARGB32);
        const int w = img.width(), h = img.height();
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
        bool anyAlpha = false, colorful = false;
        for (int y = 0; y < h; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int a = qAlpha(row[x]);
                if (a < 250) anyAlpha = true;
                if (a > 8 &&
                    (std::abs(qRed(row[x]) - qGreen(row[x])) > 8 ||
                     std::abs(qRed(row[x]) - qBlue(row[x])) > 8 ||
                     std::abs(qGreen(row[x]) - qBlue(row[x])) > 8))
                    colorful = true;
            }
        }
        tip->w = (std::uint32_t)w;
        tip->h = (std::uint32_t)h;
        tip->color = anyAlpha && colorful;
        tip->alpha.assign(std::size_t(w) * h, 0.0f);
        if (tip->color) {
            tip->red.assign(std::size_t(w) * h, 0.0f);
            tip->green.assign(std::size_t(w) * h, 0.0f);
            tip->blue.assign(std::size_t(w) * h, 0.0f);
        }
        for (int y = 0; y < h; ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(w) + x;
                const int r = qRed(row[x]), g = qGreen(row[x]),
                          b = qBlue(row[x]), a = qAlpha(row[x]);
                if (anyAlpha) {
                    tip->alpha[i] = float(a) / 255.0f;
                } else {
                    const float luma =
                        (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
                    tip->alpha[i] = 1.0f - luma;
                }
                if (tip->color) {
                    tip->red[i] = float(r) / 255.0f;
                    tip->green[i] = float(g) / 255.0f;
                    tip->blue[i] = float(b) / 255.0f;
                }
            }
        }
        tip->sanitize();
        return tip->valid();
    }

    // Store one tip bitmap as PNG under the config brushes dir and register
    // it in the AppState stamp library. Empty on any failure.
    QString storeStampTip(const QString& baseName,
                          const pittore::compute::StampTip& tip) {
        QDir().mkpath(brushTipsDir());
        QString safe = baseName;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")),
                     QStringLiteral("_"));
        if (safe.isEmpty()) safe = QStringLiteral("tip");
        QString fileName;
        for (int n = 0; n < 1000; ++n) {
            fileName = n == 0 ? safe + QStringLiteral(".png")
                              : safe + QStringLiteral("-%1.png").arg(n);
            if (!QFile::exists(QDir(brushTipsDir()).filePath(fileName))) break;
        }
        QImage out((int)tip.w, (int)tip.h, QImage::Format_ARGB32);
        for (std::uint32_t y = 0; y < tip.h; ++y) {
            for (std::uint32_t x = 0; x < tip.w; ++x) {
                const std::size_t i = std::size_t(y) * tip.w + x;
                if (tip.color) {
                    out.setPixel(x, y,
                                 qRgba(int(tip.red[i] * 255.0f),
                                       int(tip.green[i] * 255.0f),
                                       int(tip.blue[i] * 255.0f),
                                       int(tip.alpha[i] * 255.0f)));
                } else {
                    // Alpha-carrying gray: coverage survives stampFromImage
                    // via the alpha channel instead of the opaque 1-luma
                    // fallback (which would invert native light-paints tips
                    // on every save/load cycle).
                    out.setPixel(x, y,
                                 qRgba(0, 0, 0,
                                       int(tip.alpha[i] * 255.0f)));
                }
            }
        }
        if (!out.save(QDir(brushTipsDir()).filePath(fileName), "PNG"))
            return QString();
        state_->setBrushStamp(fileName, tip);
        return fileName;
    }

    // Store one tip bitmap as PNG, register it, and append a custom preset.
    // Returns false (with a status hint) on any failure.
    bool addImportedTip(const QString& baseName, const QImage& image,
                        float spacingPct, int mode) {
        pittore::compute::StampTip tip;
        if (!stampFromImage(image, &tip)) {
            state_->setStatusHint(tr("Import failed: unreadable tip image."));
            return false;
        }
        tip.spacingPct = std::clamp(spacingPct, 1.0f, 200.0f);
        const QString fileName = storeStampTip(baseName, tip);
        if (fileName.isEmpty()) {
            state_->setStatusHint(tr("Import failed: cannot write tip file."));
            return false;
        }
        BrushPreset p;
        p.name = baseName;
        p.size = 64.0;
        p.hardness = 50.0;
        p.spacing = tip.spacingPct;
        p.tipKind = QStringLiteral("stamp");
        p.stampId = fileName;
        p.stampMode = tip.color ? 1 : mode;
        presets_.push_back(p);
        return true;
    }

    static QImage tipToImage(const pittore::compute::StampTip& tip) {
        // Always ARGB32 with coverage in the alpha channel: grayscale tips
        // keep their exact coverage through stampFromImage (which reads
        // alpha when present). A Grayscale8 write would force the opaque
        // 1-luma fallback on reload and invert native (light-paints) tips.
        QImage img((int)tip.w, (int)tip.h, QImage::Format_ARGB32);
        for (std::uint32_t y = 0; y < tip.h; ++y) {
            for (std::uint32_t x = 0; x < tip.w; ++x) {
                const std::size_t i = std::size_t(y) * tip.w + x;
                if (tip.color) {
                    img.setPixel(x, y,
                                 qRgba(int(tip.red[i] * 255.0f),
                                       int(tip.green[i] * 255.0f),
                                       int(tip.blue[i] * 255.0f),
                                       int(tip.alpha[i] * 255.0f)));
                } else {
                    const int a = int(tip.alpha[i] * 255.0f);
                    img.setPixel(x, y, qRgba(0, 0, 0, a));
                }
            }
        }
        return img;
    }

    void importTips() {
        using namespace pittore::compute::brushload;
        const QString title =
            tr("Import brush (PNG, GBR, GIH, ABR, preset bundle) — only "
               "files you are licensed to use");
        const QStringList files = QFileDialog::getOpenFileNames(
            this, title, QDir::homePath(),
            tr("Brushes (*.png *.gbr *.gih *.abr *.bundle);;All files (*)"));
        if (files.isEmpty()) return;
        int added = 0;
        int approximate = 0;
        int computedApproximated = 0;
        for (const QString& path : files) {
            const QString ext = QFileInfo(path).suffix().toLower();
            const QString base = QFileInfo(path).baseName();
            if (ext == QStringLiteral("bundle")) {
                auto r = importBundleFile(path);
                added += r.first;
                approximate += r.second;
            } else if (ext == QStringLiteral("png")) {
                QImage img(path);
                if (!img.isNull() && addImportedTip(base, img, 15.0f, 0))
                    ++added;
            } else if (ext == QStringLiteral("gbr") ||
                       ext == QStringLiteral("gih") ||
                       ext == QStringLiteral("abr")) {
                QFile f(path);
                if (!f.open(QIODevice::ReadOnly)) continue;
                const QByteArray bytes = f.readAll();
                if (bytes.size() > 64 * 1024 * 1024) continue;  // sanity cap
                const auto* raw =
                    reinterpret_cast<const std::uint8_t*>(bytes.constData());
                const std::size_t n = std::size_t(bytes.size());
                if (ext == QStringLiteral("gbr")) {
                    LoadedTip t = load_gbr(raw, n);
                    if (t.ok &&
                        addImportedTip(QString::fromStdString(t.name),
                                       tipToImage(t.tip), t.tip.spacingPct,
                                       0))
                        ++added;
                } else if (ext == QStringLiteral("gih")) {
                    LoadedHose hose = load_gih(raw, n);
                    if (hose.ok && !hose.cells.empty()) {
                        // One hose preset: the stroke cycles the cells.
                        QList<QImage> imgs;
                        QStringList names;
                        int ci = 0;
                        for (const auto& cell : hose.cells) {
                            imgs.push_back(tipToImage(cell.tip));
                            names.push_back(
                                base + QStringLiteral("-cell%1").arg(ci++));
                            if (ci >= 64) break;  // hose flood cap
                        }
                        if (addHosePreset(base, names, imgs, QString::fromStdString(hose.selection),
                                          hose.step))
                            ++added;
                    }
                } else {
                    LoadedAbr abr = load_abr(raw, n);
                    if (abr.ok) {
                        // Computed (procedural) tips have no public layout
                        // to sample, so they cannot become bitmap presets;
                        // they are counted for the summary below instead of
                        // vanishing silently.
                        computedApproximated += abr.skippedComputed;
                        int ci = 0;
                        for (const auto& t : abr.tips) {
                            // Authored brush names win (like the reference
                            // library shows them); otherwise file + counter.
                            const QString tipName =
                                t.nameAuthored
                                    ? QString::fromStdString(t.name)
                                    : base + QStringLiteral("-%1").arg(ci);
                            if (addImportedTip(tipName, tipToImage(t.tip),
                                               t.tip.spacingPct, 0))
                                ++added;
                            ++ci;
                            if (ci >= 64) break;  // ABR flood cap
                        }
                    }
                }
            } else {
                state_->setStatusHint(
                    tr("Unsupported brush file (want PNG, GBR, GIH, ABR, "
                       "bundle)."));
                continue;
            }
        }
        if (added > 0) {
            saveCustom();
            rebuild();
            QString msg = tr("Imported %1 brush preset(s).").arg(added);
            if (approximate > 0)
                msg += QLatin1Char(' ') +
                       tr("%1 approximate (texture, smudge, or dynamics "
                          "not transferred).")
                           .arg(approximate);
            if (computedApproximated > 0)
                msg += QLatin1Char(' ') +
                       tr("%1 computed tip(s) skipped (procedural tips "
                          "have no public layout to sample).")
                           .arg(computedApproximated);
            state_->setStatusHint(msg);
        } else {
            state_->setStatusHint(
                tr("Nothing imported — files unreadable or unsupported."));
        }
    }

    // --- Preset bundles (.bundle ZIPs) ----------------------------------------
    // One preset per embedded preset image (best-effort param mapping, see
    // brushes/bundle_import.h) plus loose tips for unreferenced tip files.
    // Returns (added, approximate).
    QPair<int, int> importBundleFile(const QString& path) {
        namespace bi = pittore::ui::bundleimport;
        using namespace pittore::compute::brushload;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {0, 0};
        const QByteArray bytes = f.readAll();
        if (bytes.size() > 512 * 1024 * 1024) return {0, 0};  // sanity cap
        std::vector<std::uint8_t> raw(bytes.begin(), bytes.end());
        auto entries = pittore::io::zipRead(raw);
        if (!entries) return {0, 0};
        const bi::BundleResult result = bi::importBundleEntries(*entries);
        int added = 0, approximate = 0;
        for (const bi::BundlePreset& bp : result.presets) {
            QStringList notes = bp.approximateNotes;
            if (addBundlePreset(bp, &notes)) {
                ++added;
                if (!notes.isEmpty()) ++approximate;
            }
        }
        for (const auto& loose : result.looseTips) {
            if (addLooseBundleTip(loose.first, loose.second)) ++added;
        }
        return {added, approximate};
    }

    // Decode one bundle tip (PNG bytes, or RAW: native bytes) to a QImage.
    // Empty on failure. `notes` collects approximation reasons.
    QImage bundleTipImage(const QByteArray& stampPng, const QString& ext,
                          QStringList* notes) {
        using namespace pittore::compute::brushload;
        if (!stampPng.startsWith("RAW:")) {
            QImage img;
            img.loadFromData(stampPng, "PNG");
            return img;
        }
        const QByteArray rawBytes = stampPng.mid(4);
        const auto* raw =
            reinterpret_cast<const std::uint8_t*>(rawBytes.constData());
        const std::size_t n = std::size_t(rawBytes.size());
        if (ext == QStringLiteral("gbr")) {
            LoadedTip t = load_gbr(raw, n);
            if (t.ok) return tipToImage(t.tip);
        } else if (ext == QStringLiteral("gih")) {
            LoadedHose hose = load_gih(raw, n);
            // Hose cycling per-dab is a later slice: first cell for now.
            if (hose.ok && !hose.cells.empty()) {
                if (notes && hose.cells.size() > 1)
                    *notes << tr("hose: first of %1 cells")
                                  .arg(hose.cells.size());
                return tipToImage(hose.cells.front().tip);
            }
        } else if (ext == QStringLiteral("abr")) {
            LoadedAbr abr = load_abr(raw, n);
            if (abr.ok && !abr.tips.empty()) {
                if (notes && abr.tips.size() > 1)
                    *notes << tr("ABR: first of %1 tips")
                                  .arg(abr.tips.size());
                if (notes && abr.skippedComputed > 0)
                    *notes << tr("ABR: %1 computed tip(s) skipped "
                                  "(no public layout to sample)")
                                  .arg(abr.skippedComputed);
                return tipToImage(abr.tips.front().tip);
            }
        }
        return QImage();
    }

    // Decode bundle paper bytes (PNG, else GIMP PAT) to a QImage.
    // Empty on failure.
    QImage bundlePatternImage(const QByteArray& bytes) {
        QImage img;
        img.loadFromData(bytes, "PNG");
        if (!img.isNull()) return img;
        pittore::compute::brushload::PatImage pat;
        const auto* raw =
            reinterpret_cast<const std::uint8_t*>(bytes.constData());
        if (!pittore::compute::brushload::decode_pat(
                raw, std::size_t(bytes.size()), &pat))
            return QImage();
        QImage out((int)pat.w, (int)pat.h, QImage::Format_Grayscale8);
        for (std::uint32_t y = 0; y < pat.h; ++y) {
            uchar* row = out.scanLine(y);
            for (std::uint32_t x = 0; x < pat.w; ++x) {
                row[x] = static_cast<uchar>(
                    pat.gray[std::size_t(y) * pat.w + x] * 255.0f);
            }
        }
        return out;
    }

    bool addLooseBundleTip(const QString& baseName, const QByteArray& bytes) {
        const QString ext = baseName.section(QChar('.'), -1).toLower();
        const QString base = baseName.section(QChar('.'), 0, -2);
        if (ext == QStringLiteral("gih")) {
            using namespace pittore::compute::brushload;
            const auto* raw =
                reinterpret_cast<const std::uint8_t*>(bytes.constData());
            LoadedHose hose =
                load_gih(raw, std::size_t(bytes.size()));
            if (!hose.ok || hose.cells.empty()) return false;
            QList<QImage> imgs;
            QStringList names;
            int ci = 0;
            for (const auto& cell : hose.cells) {
                imgs.push_back(tipToImage(cell.tip));
                names.push_back(base + QStringLiteral("-cell%1").arg(ci++));
                if (ci >= 64) break;
            }
            return addHosePreset(base.isEmpty() ? baseName : base, names, imgs,
                                 QString::fromStdString(hose.selection),
                                 hose.step);
        }
        QImage img;
        if (ext == QStringLiteral("png")) {
            img.loadFromData(bytes, "PNG");
        } else {
            img = bundleTipImage(QByteArray("RAW:") + bytes, ext, nullptr);
        }
        if (img.isNull()) return false;
        return addImportedTip(base.isEmpty() ? baseName : base, img, 15.0f, 0);
    }

    // Mapped bundle fields -> preset (tip/hose/texture wiring is separate).
    void fillBundleFields(BrushPreset& p,
                          const pittore::ui::bundleimport::BundlePreset& bp) {
        p.size = std::clamp(bp.size, 1.0, 5000.0);
        p.hardness = std::clamp(bp.hardness, 0.0, 100.0);
        p.angle = std::clamp(bp.angleDeg, -180.0, 180.0);
        p.roundness = std::clamp(bp.roundness, 1.0, 100.0);
        p.spacing = std::clamp(bp.spacingPct, 1.0, 200.0);
        p.spacingAuto = bp.useAutoSpacing;
        p.spacingAutoCoeff = std::clamp(bp.autoSpacingCoeff, 0.0, 8.0);
        p.opacity = std::clamp(bp.opacity, 1.0, 100.0);
        p.pressureSize = bp.pressureSize;
        p.pressureOpacity = bp.pressureOpacity;
        p.sizeCurve = bp.sizeCurve;
        p.opacityCurve = bp.opacityCurve;
        p.flowCurve = bp.flowCurve;
        p.tip = bp.squareTip ? 1 : 0;
        if (bp.targetTool == QStringLiteral("Eraser"))
            p.tool = QStringLiteral("Eraser");
        if (bp.isSmudge) {
            p.engine = QStringLiteral("smudge");
            p.smudgeRate = std::clamp(bp.smudgeRate, 0.0, 100.0);
            p.smudgeRadius = std::clamp(bp.smudgeRadius, 5.0, 100.0);
        }
        p.scatterPct = std::clamp(bp.scatterPct, 0.0, 500.0);
        p.scatterX = bp.scatterX;        p.scatterY = bp.scatterY;
        p.densityPct = std::clamp(bp.densityPct, 0.0, 100.0);
        p.airbrush = bp.airbrush;
        p.airbrushRate = std::clamp(bp.airbrushRate, 1.0, 100.0);
        p.flowPct = std::clamp(bp.flowPct, 1.0, 100.0);
        p.tags = bp.tags;
        p.paintingMode = bp.paintingMode;
        p.compositeOp = bp.compositeOp;
        p.darkenPct = std::clamp(bp.darkenPct, 0.0, 100.0);
        p.sharpness = std::clamp(bp.sharpness, 0.0, 100.0);
        p.soften = std::clamp(bp.softness, 0.0, 100.0);
        p.rotationMode = std::clamp(bp.rotationMode, 0, 5);
        p.rotationCurve = bp.rotationCurve;
        p.sourceMode = std::clamp(bp.sourceMode, 0, 2);
        p.flipX = bp.flipX;
        p.flipY = bp.flipY;
        p.tiltSize = std::clamp(bp.tiltSize, 0.0, 100.0);
        p.tiltOpacity = std::clamp(bp.tiltOpacity, 0.0, 100.0);
        p.tangentialFlow = bp.tangentialFlow;
        p.smoothing = std::clamp(bp.smoothing, 0.0, 100.0);
        // Sensor drives transfer verbatim (amounts/lengths re-clamped to
        // our ranges; empty props never survive the parser anyway).
        p.drives.clear();
        for (const auto& bd : bp.drives) {
            if (bd.prop.empty()) continue;
            sensordrive::SensorDrive d = bd;
            d.amount = std::clamp(d.amount, -100.0, 100.0);
            d.lengthPx = std::clamp(d.lengthPx, 1.0, 20000.0);
            d.timeSec = std::clamp(d.timeSec, 0.1, 3600.0);
            p.drives.push_back(d);
        }
        // File spacing strides on the full diameter, not the minor axis.
        p.spacingIsotropic = true;
        p.textureMode = std::clamp(bp.textureMode, 0, 2);
        p.textureCutoffPolicy = std::clamp(bp.textureCutoffPolicy, 0, 2);
        p.textureCutLo = std::clamp(bp.textureCutLo, 0.0, 1.0);
        p.textureCutHi = std::clamp(bp.textureCutHi, 0.0, 1.0);
        if (bp.hasTexture && !bp.texturePng.isEmpty()) {
            const QImage img = bundlePatternImage(bp.texturePng);
            if (!img.isNull()) {
                const QString fileName = storePattern(bp.name, img);
                if (!fileName.isEmpty()) {
                    p.textureFile = fileName;
                    p.textureStrength = std::clamp(bp.textureStrength, 0.0, 100.0);
                    p.textureScale = std::clamp(bp.textureScale, 1.0, 800.0);
                    p.textureNeutral = std::clamp(bp.textureNeutral, 0.0, 100.0);
                    p.textureBrightness =
                        std::clamp(bp.textureBrightness, -100.0, 100.0);
                    p.textureContrast =
                        std::clamp(bp.textureContrast, 0.0, 400.0);
                    p.textureInvert = bp.textureInvert;
                    p.textureGrain = true;
                }
            }
        }
        if (bp.hasMask && !bp.maskPng.isEmpty()) {
            QImage img = bundleTipImage(bp.maskPng, bp.maskExt, nullptr);
            if (!img.isNull()) {
                pittore::compute::StampTip tip;
                if (stampFromImage(img, &tip)) {
                    const QString fileName =
                        storeStampTip(bp.name + QStringLiteral("-mask"), tip);
                    if (!fileName.isEmpty()) {
                        p.maskStamp = fileName;
                        p.maskMode = std::clamp(bp.maskMode, 0, 3);
                        p.maskRatio = std::clamp(bp.maskRatio, 5.0, 400.0);
                        p.maskAngle = std::clamp(bp.maskAngle, -180.0, 180.0);
                    }
                }
            }
        }
    }

    bool addBundlePreset(const pittore::ui::bundleimport::BundlePreset& bp,
                         QStringList* notes) {
        // Multi-cell hoses become one cycling preset (not first-cell-only).
        if (bp.isStamp && bp.stampExt == QStringLiteral("gih")) {
            using namespace pittore::compute::brushload;
            const QByteArray rawBytes = bp.stampPng.mid(4);
            const auto* raw =
                reinterpret_cast<const std::uint8_t*>(rawBytes.constData());
            LoadedHose hose =
                load_gih(raw, std::size_t(rawBytes.size()));
            if (hose.ok && !hose.cells.empty()) {
                QList<QImage> imgs;
                QStringList names;
                int ci = 0;
                for (const auto& cell : hose.cells) {
                    imgs.push_back(tipToImage(cell.tip));
                    names.push_back(bp.name +
                                    QStringLiteral("-cell%1").arg(ci++));
                    if (ci >= 64) break;
                }
                if (addHosePreset(bp.name, names, imgs, QString::fromStdString(hose.selection),
                                  hose.step)) {
                    fillBundleFields(presets_.back(), bp);
                    return true;
                }
            }
            if (notes)
                *notes << tr("hose unreadable: auto tip used");
            BrushPreset fallback;
            fallback.name = bp.name;
            fillBundleFields(fallback, bp);
            presets_.push_back(fallback);
            return true;
        }
        BrushPreset p;
        p.name = bp.name;
        fillBundleFields(p, bp);
        if (!bp.isStamp) {
            presets_.push_back(p);
            return true;
        }
        QImage img = bundleTipImage(bp.stampPng, bp.stampExt, notes);
        if (img.isNull()) {
            // Tip unresolvable: keep the mapped auto params, flagged.
            if (notes)
                *notes << tr("tip missing: auto tip used");
            presets_.push_back(p);
            return true;
        }
        pittore::compute::StampTip tip;
        if (!stampFromImage(img, &tip)) {
            presets_.push_back(p);
            return true;
        }
        tip.spacingPct = float(p.spacing);
        tip.spacingAuto = bp.useAutoSpacing;
        tip.spacingAutoCoeff = float(std::clamp(bp.autoSpacingCoeff, 0.0, 8.0));
        tip.sanitize();
        const QString fileName = storeStampTip(bp.name, tip);
        if (fileName.isEmpty()) return false;
        p.tipKind = QStringLiteral("stamp");
        p.stampId = fileName;
        p.stampMode = tip.color ? 1 : bp.stampMode;
        presets_.push_back(p);
        return true;
    }

    void loadCustom() {
        for (auto& p : loadCustomPresets()) presets_.push_back(std::move(p));
    }

    void saveCustom() { saveCustomPresets(presets_); }

    AppState* state_;
    QComboBox* tagFilter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    brushpreview::BrushPreviewWidget* preview_ = nullptr;
    std::vector<BrushPreset> presets_;
};

// ---------------------------------------------------------------------------
// Brush Preview: a large live scratch pad for the active tool's brush, with
// the preset name, a dab thumbnail and the key settings. Follows live tool
// options (presets, sliders, imports) via option/tool change notices.
// ---------------------------------------------------------------------------
class BrushPreviewPanel final : public QWidget {
  public:
    BrushPreviewPanel(AppState* state, QWidget* parent)
        : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(12, 10, 12, 10);
        column->setSpacing(8);
        auto* head = new QWidget(this);
        auto* headRow = new QHBoxLayout(head);
        headRow->setContentsMargins(0, 0, 0, 0);
        headRow->setSpacing(8);
        thumb_ = new QLabel(head);
        thumb_->setObjectName(QStringLiteral("brushPreviewThumb"));
        thumb_->setFixedSize(48, 48);
        thumb_->setAlignment(Qt::AlignCenter);
        headRow->addWidget(thumb_);
        name_ = new QLabel(head);
        name_->setObjectName(QStringLiteral("brushPreviewName"));
        name_->setWordWrap(true);
        QFont nameFont = name_->font();
        nameFont.setBold(true);
        name_->setFont(nameFont);
        headRow->addWidget(name_, 1);
        column->addWidget(head);
        preview_ = new brushpreview::BrushPreviewWidget(this);
        preview_->setExpandable(true);
        column->addWidget(preview_, 1);
        readout_ = new QLabel(this);
        readout_->setObjectName(QStringLiteral("brushPreviewReadout"));
        readout_->setWordWrap(true);
        column->addWidget(readout_);
        connect(state_, &AppState::optionChanged, this,
                [this](ToolId, const QString&, const QVariant&) { refresh(); });
        connect(state_, &AppState::toolChanged, this,
                [this](ToolId) { refresh(); });
        connect(state_, &AppState::colorsChanged, this,
                [this] { refresh(); });
        refresh();
    }

  private:
    void refresh() {
        const ToolId tool = state_->activeTool();
        const QString name = state_->activeBrushPresetName();
        name_->setText(name.isEmpty() ? tr("Custom brush") : name);
        renderToolPreview(preview_, state_, tool);
        // Dab thumbnail: hose first cell, stamp, else the auto tip.
        const QString hoseId =
            state_->option(tool, QStringLiteral("brush_hose")).toString();
        const auto* hose =
            hoseId.isEmpty() ? nullptr : state_->brushHose(hoseId);
        const QString stampId =
            state_->option(tool, QStringLiteral("brush_stamp")).toString();
        const auto* stamp =
            stampId.isEmpty() ? nullptr : state_->brushStamp(stampId);
        QImage thumb;
        if (hose && !hose->cells.empty())
            thumb = brushpreview::tipThumbStamp(hose->cells.front().tip, 0);
        else if (stamp) {
            auto lvl = [&](const char* id, double fallback) {
                const QVariant v =
                    state_->option(tool, QString::fromUtf8(id));
                return v.isValid() ? v.toDouble() : fallback;
            };
            thumb = brushpreview::tipThumbStamp(
                *stamp,
                qBound(0,
                       state_->option(tool, QStringLiteral("brush_stamp_mode"))
                           .toInt(),
                       3),
                56, true, state_->background(),
                lvl("brush_tip_neutral", 50.0),
                lvl("brush_tip_brightness", 0.0),
                lvl("brush_tip_contrast", 100.0));
        } else
            thumb = brushpreview::tipThumbAuto(autoParamsForTool(state_, tool));
        thumb_->setPixmap(pixmapForWidget(
            thumb.scaled(48, 48, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation),
            thumb_));
        // Key settings readout.
        auto opt = [&](const char* id, double fallback) {
            const QVariant v = state_->option(tool, QString::fromUtf8(id));
            return v.isValid() ? v.toDouble() : fallback;
        };
        QStringList bits;
        bits.push_back(tr("%1 px").arg(opt("brush_size", 64.0), 0, 'f', 0));
        bits.push_back(tr("%1% opacity").arg(opt("opacity", 100.0), 0, 'f', 0));
        bits.push_back(
            tr("spacing %1%").arg(opt("brush_spacing", 15.0), 0, 'f', 0));
        if (hose && !hose->cells.empty())
            bits.push_back(tr("hose %1").arg(hose->cells.size()));
        else if (stamp)
            bits.push_back(tr("stamp"));
        if (state_->option(tool, QStringLiteral("brush_engine")).toString() ==
            QStringLiteral("smudge"))
            bits.push_back(tr("smudge"));
        if (!state_->option(tool, QStringLiteral("brush_texture"))
                 .toString()
                 .isEmpty())
            bits.push_back(tr("textured"));
        if (opt("brush_scatter", 0.0) > 0.0)
            bits.push_back(tr("scatter %1%").arg(opt("brush_scatter", 0.0), 0,
                                                 'f', 0));
        if (state_->option(tool, QStringLiteral("airbrush")).toBool())
            bits.push_back(tr("airbrush"));
        readout_->setText(bits.join(QStringLiteral(" · ")));
    }

    AppState* state_;
    QLabel* thumb_ = nullptr;
    QLabel* name_ = nullptr;
    brushpreview::BrushPreviewWidget* preview_ = nullptr;
    QLabel* readout_ = nullptr;
};

}  // namespace

QWidget* createBrushesPanel(AppState* state, QWidget* parent) {
    return new BrushesPanel(state, parent);
}

QWidget* createBrushPreviewPanel(AppState* state, QWidget* parent) {
    return new BrushPreviewPanel(state, parent);
}

}  // namespace pittore::ui
