#pragma once
// Brush preset library: the preset model shared by the Brushes panel and
// the options-bar brush popup. A preset is the complete tip parameter set;
// selecting one writes a known state onto a paint tool. Custom presets
// persist as JSON beside Settings.toml; imported tip/pattern files live
// under the brushes dir (patterns in its patterns/ child).
#include <QString>
#include <QStringList>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStandardPaths>

#include <vector>

#include "ui/app_state.h"
#include "ui/brushes/sensor_drives.h"
#include "ui/brushes/sensor_drives_json.h"
#include "ui/canvas/shared/canvas_helpers.h"
#include "ui/tools/options/tool_options.h"

namespace pittore::ui::brushlibrary {

struct BrushPreset {
    QString name;
    bool factory = false;
    double size = 64.0;
    double hardness = 50.0;
    double angle = 0.0;
    double roundness = 100.0;
    double spacing = 15.0;
    int tip = 0;  // 0 round, 1 square (auto tips)
    // Star lobes 0/1 off, 2..12 star (round only). H/V hardness split
    // -100..100 (0 isotropic). Edge curve 0 linear, 1 gaussian.
    // Sharpness threshold 0 off .. 100 full cut, soften edge 0 hard ..
    // 100 soft. Auto spacing squares the percentage for fine control.
    // A positive size-law coefficient strides the dab diameter directly
    // instead (linear sub-pixel, square-root above).
    int spikes = 0;
    double fadeAniso = 0.0;
    int falloff = 0;
    double sharpness = 0.0;
    double soften = 0.0;
    bool spacingAuto = false;
    double spacingAutoCoeff = 0.0;
    double opacity = 100.0;
    bool pressureSize = true;
    bool pressureOpacity = true;
    // Authored pressure response ("value|x,y;..." or empty = built-in).
    QString sizeCurve, opacityCurve, flowCurve;
    // Stamp tips (user-imported tip files): tipKind "stamp" selects the
    // library stamp `stampId`; stampMode 0 = alpha mask (foreground tint),
    // 1 = color image, 2 = lightness map, 3 = gradient map (FG->BG).
    // Empty stampId (or a missing file) falls back to auto.
    QString tipKind;  // "" (legacy/auto) or "stamp"
    QString stampId;
    int stampMode = 0;
    // Tip lightness levels for the map modes (pivot/brightness/contrast).
    double tipNeutral = 50.0;
    double tipBrightness = 0.0;
    double tipContrast = 100.0;
    // Paint thickness 0..100: relief deposited by lightness-map dabs
    // (0 = flat, legacy behavior).
    double tipThickness = 0.0;
    QString tool;  // "" = active paint tool (or Brush), else "Eraser"
    // Smudge engine + extras.
    QString engine;  // "" (paint) or "smudge"
    double smudgeRate = 70.0;
    double smudgeRadius = 100.0;
    // Stroke shape 0 dulling / 1 smear, FG reload 0..100, smear trail
    // length in % of dab radius (100 = one radius behind).
    int smudgeMode = 0;
    double smudgeColorRate = 0.0;
    double smudgeLength = 100.0;
    // Generic sensor drives (property <- sensor mappings, JSON array).
    std::vector<sensordrive::SensorDrive> drives;
    double scatterPct = 0.0;
    bool scatterX = true, scatterY = true;
    double densityPct = 100.0;
    bool airbrush = false;
    double airbrushRate = 20.0;
    double flowPct = 100.0;
    bool tiltRotation = false;
    // Paper grain.
    QString textureFile;
    double textureStrength = 80.0;
    double textureScale = 100.0;
    double textureNeutral = 50.0;
    double textureBrightness = 0.0;
    double textureContrast = 100.0;
    bool textureInvert = false;
    // Paper-grain evaluator: false keeps the legacy texture combine,
    // true evaluates the grain pipeline (subtractive brightness,
    // neutral remap, cutoff window) used by imported preset bundles.
    bool textureGrain = false;
    // Multi-cell hose: ordered cell stamp ids + pick mode.
    QString hoseId;
    QStringList hoseCells;
    QString hoseSelection;  // "incremental" (default) or "random"
    QStringList tags;       // bundle grouping labels (empty = untagged)
    QString paintingMode;   // "" (wash default) or "buildup"
    // Dab composite op id: "" (normal default), "geometric_mean",
    // "parallel" or "allanon" (pigment-mix glazing for imported presets).
    QString compositeOp;
    // Tip rotation source: 0 off, 1 tilt, 2 drawing angle, 3 pressure,
    // 4 barrel, 5 fuzzy. Plus the pressure/fuzzy response curve.
    int rotationMode = 0;
    QString rotationCurve;
    // Dab color source: 0 foreground, 1 random hue, 2 FG/BG pressure
    // mix, 3 stroke gradient (FG→BG over the gradient length).
    int sourceMode = 0;
    // Pressure followers (all off) + stroke-gradient length in px.
    bool texturePressure = false, maskPressure = false;
    bool smudgePressure = false;
    double gradientLen = 500.0;
    // Isotropic spacing strides on the full diameter, not the minor axis.
    bool spacingIsotropic = false;
    // Masked second tip (stamp file + combine mode/ratio/angle).
    QString maskStamp;
    int maskMode = 0;
    double maskRatio = 100.0;
    double maskAngle = 0.0;
    // Grain combine mode 0..6 + soft toggle + cutoff window (0..1 range).
    int textureMode = 0;
    bool textureSoft = false;
    bool textureAutoInvertEraser = false;
    int textureCutoffPolicy = 0;
    double textureCutLo = 0.0;
    double textureCutHi = 1.0;
    // Static tip mirroring for asymmetric stamps.
    bool flipX = false, flipY = false;
    // Stylus lean response: size grows with lean, opacity shrinks (0..100,
    // 0 = off). Wheel (tangential pressure) scales flow when on.
    double tiltSize = 0.0;
    double tiltOpacity = 0.0;
    // Master tilt scale (0..100, 100 = neutral): scales every lean-driven
    // amount (size, opacity, X/Y split) for tablet work.
    double tiltMaster = 100.0;
    bool tangentialFlow = false;
    // Stroke stabilizer strength (0..100, 0 = off).
    double smoothing = 0.0;
    // Stabilizer feel: 0 classic ease, 1 speed-weighted lag, 2 whole-
    // pixel snap.
    int smoothingMode = 0;
    // Tip filter tier: 1 bilinear (smooth stamps), 0 nearest (draft).
    int tipFilter = 1;
    // Erase blend (drawing eraser): Brush/Pencil dabs erase. Sticky tool
    // state like the tilt toggle (a preset carrying it applies it).
    bool eraserBlend = false;
    // Fade taper length in px (0 = off): size + opacity taper to nothing
    // over the travelled distance. Darken pulls the dab toward black by
    // pressure (0..100). Hue jitter bounds the random-hue deviation
    // (0..180 degrees; 0 keeps the legacy full-range random). PressureIn
    // holds the stroke's running-maximum pressure (monotonic inking).
    // Speed size thins fast spans (0..100, 0 = off).
    double fadeLen = 0.0;
    double darkenPct = 0.0;
    double hueJitter = 0.0;
    double satJitter = 0.0;
    double valJitter = 0.0;
    bool pressureIn = false;
    double speedSize = 0.0;
    // Tilt-axis split (X/Y size amounts), time fade (seconds), fuzzy-stroke
    // size/opacity, perspective depth + vanishing point (doc px, -1 = auto
    // canvas center).
    double tiltXSize = 0.0, tiltYSize = 0.0;
    double timeFade = 0.0;
    double fuzzySize = 0.0, fuzzyOpacity = 0.0;
    double perspective = 0.0, vpX = -1.0, vpY = -1.0;

    QJsonObject toJson() const {
        QJsonObject o{{"name", name},
                      {"size", size},
                      {"hardness", hardness},
                      {"angle", angle},
                      {"roundness", roundness},
                      {"spacing", spacing},
                      {"tip", tip},
                      {"opacity", opacity},
                      {"pressure_size", pressureSize},
                      {"pressure_opacity", pressureOpacity},
                      {"size_curve", sizeCurve},
                      {"opacity_curve", opacityCurve},
                      {"flow_curve", flowCurve}};
        if (!tipKind.isEmpty()) {
            o.insert("tipKind", tipKind);
            o.insert("stampId", stampId);
            o.insert("stampMode", stampMode);
            if (tipNeutral != 50.0) o.insert("tip_neutral", tipNeutral);
            if (tipBrightness != 0.0)
                o.insert("tip_brightness", tipBrightness);
            if (tipContrast != 100.0) o.insert("tip_contrast", tipContrast);
            if (tipThickness > 0.0) o.insert("tip_thickness", tipThickness);
        }
        if (!tool.isEmpty()) o.insert("tool", tool);
        if (engine == QStringLiteral("smudge")) {
            o.insert("engine", engine);
            o.insert("smudge_rate", smudgeRate);
            o.insert("smudge_radius", smudgeRadius);
        }
        if (smudgeMode != 0) o.insert("smudge_mode", smudgeMode);
        if (smudgeColorRate > 0.0)
            o.insert("smudge_color_rate", smudgeColorRate);
        if (smudgeLength != 100.0) o.insert("smudge_length", smudgeLength);
        if (!drives.empty()) {
            QJsonArray arr;
            for (const auto& d : drives)
                arr.push_back(sensordrive::driveToJson(d));
            o.insert("drives", arr);
        }
        if (scatterPct > 0.0) {
            o.insert("scatter", scatterPct);
            o.insert("scatter_x", scatterX);
            o.insert("scatter_y", scatterY);
        }
        if (densityPct < 100.0) o.insert("density", densityPct);
        if (airbrush) {
            o.insert("airbrush", airbrush);
            o.insert("airbrush_rate", airbrushRate);
        }
        if (flowPct < 100.0) o.insert("flow", flowPct);
        if (tiltRotation) o.insert("tilt_rotation", tiltRotation);
        if (spikes >= 2) o.insert("spikes", spikes);
        if (fadeAniso != 0.0) o.insert("fade_aniso", fadeAniso);
        if (falloff != 0) o.insert("falloff", falloff);
        if (sharpness > 0.0) o.insert("sharpness", sharpness);
        if (soften > 0.0) o.insert("soften", soften);
        if (spacingAuto) o.insert("spacing_auto", spacingAuto);
        if (spacingAutoCoeff > 0.0) o.insert("spacing_auto_coeff", spacingAutoCoeff);
        if (!textureFile.isEmpty()) {
            o.insert("texture", textureFile);
            o.insert("texture_strength", textureStrength);
            o.insert("texture_scale", textureScale);
            o.insert("texture_neutral", textureNeutral);
            o.insert("texture_brightness", textureBrightness);
            o.insert("texture_contrast", textureContrast);
            o.insert("texture_invert", textureInvert);
            if (textureGrain) o.insert("texture_grain", true);
        }
        if (!hoseId.isEmpty()) {
            o.insert("hose", hoseId);
            QJsonArray cells;
            for (const QString& c : hoseCells) cells.push_back(c);
            o.insert("hose_cells", cells);
            o.insert("hose_selection", hoseSelection);
        }
        if (!tags.isEmpty()) {
            QJsonArray arr;
            for (const QString& t : tags) arr.push_back(t);
            o.insert("tags", arr);
        }
        if (paintingMode == QStringLiteral("buildup"))
            o.insert("painting_mode", paintingMode);
        if (!compositeOp.isEmpty() &&
            compositeOp != QStringLiteral("normal"))
            o.insert("composite", compositeOp);
        if (rotationMode != 0) {
            o.insert("rotation_mode", rotationMode);
            o.insert("rotation_curve", rotationCurve);
        }
        if (sourceMode != 0) o.insert("source", sourceMode);
        if (spacingIsotropic) o.insert("spacing_isotropic", spacingIsotropic);
        if (!maskStamp.isEmpty()) {
            o.insert("mask_stamp", maskStamp);
            o.insert("mask_mode", maskMode);
            o.insert("mask_ratio", maskRatio);
            o.insert("mask_angle", maskAngle);
        }
        if (textureMode != 0) o.insert("texture_mode", textureMode);
        if (textureSoft) o.insert("texture_soft", textureSoft);
        if (textureAutoInvertEraser)
            o.insert("texture_auto_invert_eraser", textureAutoInvertEraser);
        if (textureCutoffPolicy != 0) {
            o.insert("texture_cutoff_policy", textureCutoffPolicy);
            o.insert("texture_cutlo", textureCutLo);
            o.insert("texture_cuthi", textureCutHi);
        }
        if (flipX) o.insert("flip_x", flipX);
        if (flipY) o.insert("flip_y", flipY);
        if (tiltSize > 0.0) o.insert("tilt_size", tiltSize);
        if (tiltMaster != 100.0) o.insert("tilt_master", tiltMaster);
        if (tiltOpacity > 0.0) o.insert("tilt_opacity", tiltOpacity);
        if (tangentialFlow) o.insert("tangential_flow", tangentialFlow);
        if (texturePressure) o.insert("texture_pressure", texturePressure);
        if (maskPressure) o.insert("mask_pressure", maskPressure);
        if (smudgePressure) o.insert("smudge_pressure", smudgePressure);
        if (gradientLen != 500.0) o.insert("gradient_len", gradientLen);
        if (smoothing > 0.0) o.insert("smoothing", smoothing);
        if (smoothingMode != 0) o.insert("smoothing_mode", smoothingMode);
        if (tipFilter != 1) o.insert("tip_filter", tipFilter);
        if (eraserBlend) o.insert("eraser_blend", eraserBlend);
        if (fadeLen > 0.0) o.insert("fade", fadeLen);
        if (darkenPct > 0.0) o.insert("darken", darkenPct);
        if (hueJitter > 0.0) o.insert("hue_jitter", hueJitter);
        if (satJitter > 0.0) o.insert("sat_jitter", satJitter);
        if (valJitter > 0.0) o.insert("val_jitter", valJitter);
        if (pressureIn) o.insert("pressure_in", pressureIn);
        if (speedSize > 0.0) o.insert("speed_size", speedSize);
        if (tiltXSize > 0.0) o.insert("tilt_x_size", tiltXSize);
        if (tiltYSize > 0.0) o.insert("tilt_y_size", tiltYSize);
        if (timeFade > 0.0) o.insert("timefade", timeFade);
        if (fuzzySize > 0.0) o.insert("fuzzy_size", fuzzySize);
        if (fuzzyOpacity > 0.0) o.insert("fuzzy_opacity", fuzzyOpacity);
        if (perspective > 0.0) {
            o.insert("perspective", perspective);
            o.insert("vp_x", vpX);
            o.insert("vp_y", vpY);
        }
        return o;
    }
    static BrushPreset fromJson(const QJsonObject& o, bool* ok = nullptr) {
        BrushPreset p;
        p.name = o.value(QStringLiteral("name")).toString();
        p.size = o.value(QStringLiteral("size")).toDouble(64.0);
        p.hardness = o.value(QStringLiteral("hardness")).toDouble(50.0);
        p.angle = o.value(QStringLiteral("angle")).toDouble(0.0);
        p.roundness = o.value(QStringLiteral("roundness")).toDouble(100.0);
        p.spacing = o.value(QStringLiteral("spacing")).toDouble(15.0);
        p.tip = o.value(QStringLiteral("tip")).toInt(0);
        p.opacity = o.value(QStringLiteral("opacity")).toDouble(100.0);
        p.pressureSize =
            o.value(QStringLiteral("pressure_size")).toBool(true);
        p.pressureOpacity =
            o.value(QStringLiteral("pressure_opacity")).toBool(true);
        p.sizeCurve = o.value(QStringLiteral("size_curve")).toString();
        p.opacityCurve = o.value(QStringLiteral("opacity_curve")).toString();
        p.flowCurve = o.value(QStringLiteral("flow_curve")).toString();
        p.tipKind = o.value(QStringLiteral("tipKind")).toString();
        p.stampId = o.value(QStringLiteral("stampId")).toString();
        p.stampMode = o.value(QStringLiteral("stampMode")).toInt(0);
        p.tipNeutral = o.value(QStringLiteral("tip_neutral")).toDouble(50.0);
        p.tipBrightness =
            o.value(QStringLiteral("tip_brightness")).toDouble(0.0);
        p.tipContrast =
            o.value(QStringLiteral("tip_contrast")).toDouble(100.0);
        p.tipThickness =
            o.value(QStringLiteral("tip_thickness")).toDouble(0.0);
        p.tool = o.value(QStringLiteral("tool")).toString();
        p.engine = o.value(QStringLiteral("engine")).toString();
        p.smudgeRate = o.value(QStringLiteral("smudge_rate")).toDouble(70.0);
        p.smudgeRadius = o.value(QStringLiteral("smudge_radius")).toDouble(100.0);
        p.smudgeMode = o.value(QStringLiteral("smudge_mode")).toInt(0);
        p.smudgeColorRate =
            o.value(QStringLiteral("smudge_color_rate")).toDouble(0.0);
        p.smudgeLength =
            o.value(QStringLiteral("smudge_length")).toDouble(100.0);
        p.drives.clear();
        for (const QJsonValue& v :
             o.value(QStringLiteral("drives")).toArray()) {
            if (!v.isObject()) continue;
            bool ok = false;
            sensordrive::SensorDrive d =
                sensordrive::driveFromJson(v.toObject(), &ok);
            if (ok) p.drives.push_back(d);
        }
        p.scatterPct = o.value(QStringLiteral("scatter")).toDouble(0.0);
        p.scatterX = o.value(QStringLiteral("scatter_x")).toBool(true);
        p.scatterY = o.value(QStringLiteral("scatter_y")).toBool(true);
        p.densityPct = o.value(QStringLiteral("density")).toDouble(100.0);
        p.airbrush = o.value(QStringLiteral("airbrush")).toBool(false);
        p.airbrushRate = o.value(QStringLiteral("airbrush_rate")).toDouble(20.0);
        p.flowPct = o.value(QStringLiteral("flow")).toDouble(100.0);
        p.tiltRotation = o.value(QStringLiteral("tilt_rotation")).toBool(false);
        p.spikes = o.value(QStringLiteral("spikes")).toInt(0);
        p.fadeAniso = o.value(QStringLiteral("fade_aniso")).toDouble(0.0);
        p.falloff = o.value(QStringLiteral("falloff")).toInt(0);
        p.sharpness = o.value(QStringLiteral("sharpness")).toDouble(0.0);
        p.soften = o.value(QStringLiteral("soften")).toDouble(0.0);
        p.spacingAuto = o.value(QStringLiteral("spacing_auto")).toBool(false);
        p.spacingAutoCoeff =
            o.value(QStringLiteral("spacing_auto_coeff")).toDouble(0.0);
        p.textureFile = o.value(QStringLiteral("texture")).toString();
        p.textureStrength = o.value(QStringLiteral("texture_strength")).toDouble(80.0);
        p.textureScale = o.value(QStringLiteral("texture_scale")).toDouble(100.0);
        p.textureNeutral = o.value(QStringLiteral("texture_neutral")).toDouble(50.0);
        p.textureBrightness =
            o.value(QStringLiteral("texture_brightness")).toDouble(0.0);
        p.textureContrast =
            o.value(QStringLiteral("texture_contrast")).toDouble(100.0);
        p.textureInvert = o.value(QStringLiteral("texture_invert")).toBool(false);
        p.textureGrain = o.value(QStringLiteral("texture_grain")).toBool(false);
        p.hoseId = o.value(QStringLiteral("hose")).toString();
        p.hoseCells.clear();
        for (const QJsonValue& cv : o.value(QStringLiteral("hose_cells")).toArray())
            p.hoseCells.push_back(cv.toString());
        p.hoseSelection =
            o.value(QStringLiteral("hose_selection")).toString(QStringLiteral("incremental"));
        p.tags.clear();
        for (const QJsonValue& tv : o.value(QStringLiteral("tags")).toArray())
            p.tags.push_back(tv.toString());
        p.paintingMode = o.value(QStringLiteral("painting_mode")).toString();
        p.compositeOp = o.value(QStringLiteral("composite")).toString();
        p.rotationMode = o.value(QStringLiteral("rotation_mode")).toInt(0);
        p.rotationCurve = o.value(QStringLiteral("rotation_curve")).toString();
        p.sourceMode = o.value(QStringLiteral("source")).toInt(0);
        p.spacingIsotropic =
            o.value(QStringLiteral("spacing_isotropic")).toBool(false);
        p.maskStamp = o.value(QStringLiteral("mask_stamp")).toString();
        p.maskMode = o.value(QStringLiteral("mask_mode")).toInt(0);
        p.maskRatio = o.value(QStringLiteral("mask_ratio")).toDouble(100.0);
        p.maskAngle = o.value(QStringLiteral("mask_angle")).toDouble(0.0);
        p.textureMode = o.value(QStringLiteral("texture_mode")).toInt(0);
        p.textureSoft = o.value(QStringLiteral("texture_soft")).toBool(false);
        p.textureAutoInvertEraser =
            o.value(QStringLiteral("texture_auto_invert_eraser")).toBool(false);
        p.textureCutoffPolicy =
            o.value(QStringLiteral("texture_cutoff_policy")).toInt(0);
        p.textureCutLo = o.value(QStringLiteral("texture_cutlo")).toDouble(0.0);
        p.textureCutHi = o.value(QStringLiteral("texture_cuthi")).toDouble(1.0);
        p.flipX = o.value(QStringLiteral("flip_x")).toBool(false);
        p.flipY = o.value(QStringLiteral("flip_y")).toBool(false);
        p.tiltSize = o.value(QStringLiteral("tilt_size")).toDouble(0.0);
        p.tiltMaster = o.value(QStringLiteral("tilt_master")).toDouble(100.0);
        p.tiltOpacity = o.value(QStringLiteral("tilt_opacity")).toDouble(0.0);
        p.tangentialFlow =
            o.value(QStringLiteral("tangential_flow")).toBool(false);
        p.texturePressure =
            o.value(QStringLiteral("texture_pressure")).toBool(false);
        p.maskPressure =
            o.value(QStringLiteral("mask_pressure")).toBool(false);
        p.smudgePressure =
            o.value(QStringLiteral("smudge_pressure")).toBool(false);
        p.gradientLen = o.value(QStringLiteral("gradient_len")).toDouble(500.0);
        p.smoothing = o.value(QStringLiteral("smoothing")).toDouble(0.0);
        p.smoothingMode = o.value(QStringLiteral("smoothing_mode")).toInt(0);
        p.tipFilter = o.value(QStringLiteral("tip_filter")).toInt(1);
        p.eraserBlend = o.value(QStringLiteral("eraser_blend")).toBool(false);
        p.fadeLen = o.value(QStringLiteral("fade")).toDouble(0.0);
        p.darkenPct = o.value(QStringLiteral("darken")).toDouble(0.0);
        p.hueJitter = o.value(QStringLiteral("hue_jitter")).toDouble(0.0);
        p.satJitter = o.value(QStringLiteral("sat_jitter")).toDouble(0.0);
        p.valJitter = o.value(QStringLiteral("val_jitter")).toDouble(0.0);
        p.pressureIn = o.value(QStringLiteral("pressure_in")).toBool(false);
        p.speedSize = o.value(QStringLiteral("speed_size")).toDouble(0.0);
        p.tiltXSize = o.value(QStringLiteral("tilt_x_size")).toDouble(0.0);
        p.tiltYSize = o.value(QStringLiteral("tilt_y_size")).toDouble(0.0);
        p.timeFade = o.value(QStringLiteral("timefade")).toDouble(0.0);
        p.fuzzySize = o.value(QStringLiteral("fuzzy_size")).toDouble(0.0);
        p.fuzzyOpacity =
            o.value(QStringLiteral("fuzzy_opacity")).toDouble(0.0);
        p.perspective =
            o.value(QStringLiteral("perspective")).toDouble(0.0);
        p.vpX = o.value(QStringLiteral("vp_x")).toDouble(-1.0);
        p.vpY = o.value(QStringLiteral("vp_y")).toDouble(-1.0);
        if (ok) *ok = !p.name.isEmpty();
        return p;
    }
    bool isStamp() const { return tipKind == QStringLiteral("stamp"); }
};

inline QString brushLibraryPath() {
    QDir base(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
    return base.filePath(QStringLiteral("PittoreStudio/brushes.json"));
}

inline QString brushTipsDir() {
    QDir base(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
    return base.filePath(QStringLiteral("PittoreStudio/brushes"));
}

inline QString brushPatternsDir() {
    return brushTipsDir() + QStringLiteral("/patterns");
}

inline std::vector<BrushPreset> factoryBrushPresets() {
    // Our own factory set (no third-party names or assets).
    std::vector<BrushPreset> out;
    auto add = [&](const char* name, double size, double hard, double angle,
                   double round, double spacing, int tip, double op) {
        BrushPreset p;
        p.name = QString::fromUtf8(name);
        p.factory = true;
        p.size = size;
        p.hardness = hard;
        p.angle = angle;
        p.roundness = round;
        p.spacing = spacing;
        p.tip = tip;
        p.opacity = op;
        out.push_back(p);
    };
    add("Soft Round", 64, 50, 0, 100, 15, 0, 100);
    add("Hard Round", 64, 100, 0, 100, 15, 0, 100);
    add("Soft Round Pressure", 64, 50, 0, 100, 15, 0, 100);
    add("Flat Bristle", 48, 80, 15, 40, 12, 0, 100);
    add("Chisel Square", 48, 90, 45, 60, 10, 1, 100);
    add("Wash Soft", 120, 0, 0, 100, 20, 0, 40);
    add("Stipple Scatter", 80, 0, 0, 100, 35, 0, 100);
    add("Liner Flat", 32, 100, 0, 25, 8, 0, 100);
    return out;
}

// Custom presets from disk (factory set is separate).
inline std::vector<BrushPreset> loadCustomPresets() {
    std::vector<BrushPreset> out;
    QFile f(brushLibraryPath());
    if (!f.open(QIODevice::ReadOnly)) return out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return out;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject()) continue;
        bool ok = false;
        BrushPreset p = BrushPreset::fromJson(v.toObject(), &ok);
        if (!ok) continue;
        p.factory = false;
        out.push_back(p);
    }
    return out;
}

inline void saveCustomPresets(const std::vector<BrushPreset>& presets) {
    QJsonArray arr;
    for (const auto& p : presets) {
        if (p.factory) continue;
        arr.push_back(p.toJson());
    }
    QDir().mkpath(QFileInfo(brushLibraryPath()).dir().path());
    QFile f(brushLibraryPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// The paint tool a preset applies to: the active tool when it strokes
// with the dab kernel, otherwise the Brush (presets always land
// somewhere predictable).
inline ToolId presetTarget(AppState* state) {
    const ToolId active = state->activeTool();
    return isPaintTool(active) ? active : ToolId::Brush;
}

// A preset pinned to the Eraser applies there regardless of the active
// tool (eraser-flagged presets); everything else uses presetTarget().
inline ToolId presetToolFor(AppState* state, const BrushPreset& p) {
    if (p.tool == QStringLiteral("Eraser")) return ToolId::Eraser;
    return presetTarget(state);
}

enum class ApplyResult {
    Applied,
    StampMissing,  // applied anyway; strokes fall back to the auto tip
};

// Writes the preset's full option state onto its tool. Selecting a brush
// always gives a known state (including clearing stamp/hose/texture and
// the tilt toggle when the preset carries none) — except for locked
// options: lock_size keeps the live diameter, lock_opacity the live
// opacity, lock_texture the live grain. Locks are sticky tool prefs,
// not preset content, so snapshots never carry them.
inline ApplyResult applyPreset(AppState* state, const BrushPreset& p) {
    const ToolId tool = presetToolFor(state, p);
    const bool lockSize =
        state->option(tool, QStringLiteral("lock_size")).toBool();
    const bool lockOpacity =
        state->option(tool, QStringLiteral("lock_opacity")).toBool();
    const bool lockTexture =
        state->option(tool, QStringLiteral("lock_texture")).toBool();
    state->setActiveBrushPresetName(p.name);
    if (!lockSize)
        state->setOption(tool, QStringLiteral("brush_size"), p.size);
    state->setOption(tool, QStringLiteral("brush_hardness"), p.hardness);
    state->setOption(tool, QStringLiteral("brush_angle"), p.angle);
    state->setOption(tool, QStringLiteral("brush_roundness"), p.roundness);
    state->setOption(tool, QStringLiteral("brush_spacing"), p.spacing);
    state->setOption(tool, QStringLiteral("brush_tip"), p.tip);
    state->setOption(tool, QStringLiteral("brush_spikes"),
                     qBound(0, p.spikes, 12));
    state->setOption(tool, QStringLiteral("brush_fade_aniso"),
                     qBound(-100.0, p.fadeAniso, 100.0));
    state->setOption(tool, QStringLiteral("brush_falloff"),
                     qBound(0, p.falloff, 1));
    state->setOption(tool, QStringLiteral("brush_sharpness"),
                     qBound(0.0, p.sharpness, 100.0));
    state->setOption(tool, QStringLiteral("brush_soften"),
                     qBound(0.0, p.soften, 100.0));
    state->setOption(tool, QStringLiteral("brush_spacing_auto"),
                     p.spacingAuto);
    state->setOption(tool, QStringLiteral("brush_spacing_auto_coeff"),
                     qBound(0.0, p.spacingAutoCoeff, 8.0));
    if (!lockOpacity)
        state->setOption(tool, QStringLiteral("opacity"), p.opacity);
    state->setOption(tool, QStringLiteral("pressure_size"), p.pressureSize);
    state->setOption(tool, QStringLiteral("pressure_opacity"),
                     p.pressureOpacity);
    state->setOption(tool, QStringLiteral("brush_size_curve"), p.sizeCurve);
    state->setOption(tool, QStringLiteral("brush_opacity_curve"),
                     p.opacityCurve);
    state->setOption(tool, QStringLiteral("brush_flow_curve"), p.flowCurve);
    state->setOption(tool, QStringLiteral("brush_stamp"),
                     p.isStamp() ? p.stampId : QString());
    state->setOption(tool, QStringLiteral("brush_stamp_mode"),
                     qBound(0, p.stampMode, 3));
    state->setOption(tool, QStringLiteral("brush_tip_neutral"),
                     qBound(0.0, p.tipNeutral, 100.0));
    state->setOption(tool, QStringLiteral("brush_tip_brightness"),
                     qBound(-100.0, p.tipBrightness, 100.0));
    state->setOption(tool, QStringLiteral("brush_tip_contrast"),
                     qBound(0.0, p.tipContrast, 400.0));
    state->setOption(tool, QStringLiteral("brush_tip_thickness"),
                     qBound(0.0, p.tipThickness, 100.0));
    state->setOption(tool, QStringLiteral("brush_engine"), p.engine);
    state->setOption(tool, QStringLiteral("brush_smudge_rate"), p.smudgeRate);
    state->setOption(tool, QStringLiteral("brush_smudge_radius"),
                     p.smudgeRadius);
    state->setOption(tool, QStringLiteral("smudge_mode"),
                     qBound(0, p.smudgeMode, 1));
    state->setOption(tool, QStringLiteral("smudge_color_rate"),
                     qBound(0.0, p.smudgeColorRate, 100.0));
    state->setOption(tool, QStringLiteral("smudge_length"),
                     qBound(0.0, p.smudgeLength, 200.0));
    state->setOption(tool, QStringLiteral("sensor_drives"),
                     sensordrive::encodeDrives(p.drives));
    state->setOption(tool, QStringLiteral("brush_scatter"), p.scatterPct);
    state->setOption(tool, QStringLiteral("brush_scatter_x"), p.scatterX);
    state->setOption(tool, QStringLiteral("brush_scatter_y"), p.scatterY);
    state->setOption(tool, QStringLiteral("brush_density"), p.densityPct);
    state->setOption(tool, QStringLiteral("airbrush"), p.airbrush);
    state->setOption(tool, QStringLiteral("airbrush_rate"), p.airbrushRate);
    state->setOption(tool, QStringLiteral("flow"), p.flowPct);
    state->setOption(tool, QStringLiteral("tilt_rotation"), p.tiltRotation);
    if (!lockTexture) {
        state->setOption(tool, QStringLiteral("brush_texture"),
                         p.textureFile);
    state->setOption(tool, QStringLiteral("brush_texture_strength"),
                     p.textureStrength);
    state->setOption(tool, QStringLiteral("brush_texture_scale"),
                     p.textureScale);
    state->setOption(tool, QStringLiteral("brush_texture_neutral"),
                     p.textureNeutral);
    state->setOption(tool, QStringLiteral("brush_texture_brightness"),
                     p.textureBrightness);
    state->setOption(tool, QStringLiteral("brush_texture_contrast"),
                     p.textureContrast);
    state->setOption(tool, QStringLiteral("brush_texture_invert"),
                     p.textureInvert);
    state->setOption(tool, QStringLiteral("brush_texture_grain"),
                     p.textureGrain);
    }  // !lockTexture
    state->setOption(tool, QStringLiteral("brush_hose"), p.hoseId);
    state->setOption(tool, QStringLiteral("brush_painting_mode"),
                     p.paintingMode.isEmpty() ? QStringLiteral("wash")
                                              : p.paintingMode);
    state->setOption(tool, QStringLiteral("brush_composite"),
                     p.compositeOp.isEmpty() ? QStringLiteral("normal")
                                             : p.compositeOp);
    // Rotation source + response curve. The legacy tilt toggle stays in
    // sync so older snapshots keep working.
    state->setOption(tool, QStringLiteral("brush_rotation"),
                     qBound(0, p.rotationMode, 5));
    state->setOption(tool, QStringLiteral("brush_rotation_curve"),
                     p.rotationCurve);
    state->setOption(tool, QStringLiteral("tilt_rotation"),
                     p.tiltRotation || p.rotationMode == 1);
    state->setOption(tool, QStringLiteral("brush_source"),
                     qBound(0, p.sourceMode, 3));
    state->setOption(tool, QStringLiteral("brush_spacing_isotropic"),
                     p.spacingIsotropic);
    state->setOption(tool, QStringLiteral("brush_mask_stamp"), p.maskStamp);
    state->setOption(tool, QStringLiteral("brush_mask_mode"),
                     qBound(0, p.maskMode, 3));
    state->setOption(tool, QStringLiteral("brush_mask_ratio"), p.maskRatio);
    state->setOption(tool, QStringLiteral("brush_mask_angle"), p.maskAngle);
    state->setOption(tool, QStringLiteral("brush_texture_mode"),
                     qBound(0, p.textureMode, 6));
    state->setOption(tool, QStringLiteral("brush_texture_soft"),
                     p.textureSoft);
    state->setOption(tool, QStringLiteral("brush_texture_auto_invert_eraser"),
                     p.textureAutoInvertEraser);
    state->setOption(tool, QStringLiteral("brush_texture_cutoff_policy"),
                     qBound(0, p.textureCutoffPolicy, 2));
    state->setOption(tool, QStringLiteral("brush_texture_cutlo"),
                     p.textureCutLo);
    state->setOption(tool, QStringLiteral("brush_texture_cuthi"),
                     p.textureCutHi);
    state->setOption(tool, QStringLiteral("brush_flip_x"), p.flipX);
    state->setOption(tool, QStringLiteral("brush_flip_y"), p.flipY);
    state->setOption(tool, QStringLiteral("brush_tilt_size"),
                     qBound(0.0, p.tiltSize, 100.0));
    state->setOption(tool, QStringLiteral("brush_tilt_opacity"),
                     qBound(0.0, p.tiltOpacity, 100.0));
    state->setOption(tool, QStringLiteral("brush_tilt_master"),
                     qBound(0.0, p.tiltMaster, 100.0));
    state->setOption(tool, QStringLiteral("brush_tangential_flow"),
                     p.tangentialFlow);
    state->setOption(tool, QStringLiteral("brush_texture_pressure"),
                     p.texturePressure);
    state->setOption(tool, QStringLiteral("brush_mask_pressure"),
                     p.maskPressure);
    state->setOption(tool, QStringLiteral("brush_smudge_pressure"),
                     p.smudgePressure);
    state->setOption(tool, QStringLiteral("brush_gradient_len"),
                     qMax(0.0, p.gradientLen));
    state->setOption(tool, QStringLiteral("smoothing"),
                     qBound(0.0, p.smoothing, 100.0));
    state->setOption(tool, QStringLiteral("smoothing_mode"),
                     qBound(0, p.smoothingMode, 2));
    state->setOption(tool, QStringLiteral("brush_tip_filter"),
                     qBound(0, p.tipFilter, 1));
    state->setOption(tool, QStringLiteral("brush_erase_blend"),
                     p.eraserBlend);
    state->setOption(tool, QStringLiteral("brush_fade"),
                     qMax(0.0, p.fadeLen));
    state->setOption(tool, QStringLiteral("brush_darken"),
                     qBound(0.0, p.darkenPct, 100.0));
    state->setOption(tool, QStringLiteral("brush_hue_jitter"),
                     qBound(0.0, p.hueJitter, 180.0));
    state->setOption(tool, QStringLiteral("brush_sat_jitter"),
                     qBound(0.0, p.satJitter, 100.0));
    state->setOption(tool, QStringLiteral("brush_val_jitter"),
                     qBound(0.0, p.valJitter, 100.0));
    state->setOption(tool, QStringLiteral("brush_pressure_in"),
                     p.pressureIn);
    state->setOption(tool, QStringLiteral("brush_speed_size"),
                     qBound(0.0, p.speedSize, 100.0));
    state->setOption(tool, QStringLiteral("brush_tiltx_size"),
                     qMax(0.0, p.tiltXSize));
    state->setOption(tool, QStringLiteral("brush_tilty_size"),
                     qMax(0.0, p.tiltYSize));
    state->setOption(tool, QStringLiteral("brush_timefade"),
                     qMax(0.0, p.timeFade));
    state->setOption(tool, QStringLiteral("brush_fuzzy_size"),
                     qBound(0.0, p.fuzzySize, 100.0));
    state->setOption(tool, QStringLiteral("brush_fuzzy_opacity"),
                     qBound(0.0, p.fuzzyOpacity, 100.0));
    state->setOption(tool, QStringLiteral("brush_perspective"),
                     qBound(0.0, p.perspective, 100.0));
    state->setOption(tool, QStringLiteral("brush_vp_x"), p.vpX);
    state->setOption(tool, QStringLiteral("brush_vp_y"), p.vpY);
    if (p.isStamp() && !state->brushStamp(p.stampId))
        return ApplyResult::StampMissing;
    return ApplyResult::Applied;
}

// Defaults for the popup-owned rows: option id → neutral value. Mirrors
// the refreshFromTool fallbacks below, so reset and first paint agree.
// Schema-owned ids (size preset, opacity, flow, …) come from optionsFor.
inline const std::vector<std::pair<const char*, QVariant>>&
brushPopupDefaults() {
    static const std::vector<std::pair<const char*, QVariant>> k = {
        {"brush_size", 64.0},
        {"opacity", 100.0},
        {"brush_hardness", 50.0},
        {"brush_angle", 0.0},
        {"brush_roundness", 100.0},
        {"brush_spacing", 15.0},
        {"brush_scatter", 0.0},
        {"brush_density", 100.0},
        {"brush_tilt_master", 100.0},
        {"brush_tilt_size", 0.0},
        {"brush_tilt_opacity", 0.0},
        {"brush_tiltx_size", 0.0},
        {"brush_tilty_size", 0.0},
        {"brush_fade", 0.0},
        {"brush_darken", 0.0},
        {"brush_hue_jitter", 0.0},
        {"brush_sat_jitter", 0.0},
        {"brush_val_jitter", 0.0},
        {"brush_speed_size", 0.0},
        {"brush_timefade", 0.0},
        {"brush_fuzzy_size", 0.0},
        {"brush_fuzzy_opacity", 0.0},
        {"brush_perspective", 0.0},
        {"brush_gradient_len", 500.0},
        {"brush_tip", 0},
        {"brush_stamp_mode", 0},
        {"brush_tip_neutral", 50.0},
        {"brush_tip_brightness", 0.0},
        {"brush_tip_contrast", 100.0},
        {"brush_tip_thickness", 0.0},
        {"brush_spikes", 0},
        {"brush_fade_aniso", 0.0},
        {"brush_falloff", 0},
        {"brush_sharpness", 0.0},
        {"brush_soften", 0.0},
        {"brush_spacing_auto", false},
        {"brush_spacing_auto_coeff", 0.0},
        {"brush_rotation", 0},
        {"brush_source", 0},
        {"brush_texture_mode", 0},
        {"brush_painting_mode", QStringLiteral("wash")},
        {"brush_composite", QStringLiteral("normal")},
        {"smoothing_mode", 0},
        {"brush_tip_filter", 1},
        {"brush_flip_x", false},
        {"brush_flip_y", false},
        {"brush_spacing_isotropic", false},
        {"brush_tangential_flow", false},
        {"brush_pressure_in", false},
        {"brush_texture_pressure", false},
        {"brush_texture_soft", false},
        {"brush_texture_auto_invert_eraser", false},
        {"brush_mask_pressure", false},
        {"brush_smudge_pressure", false},
        {"smudge_mode", 0},
        {"smudge_color_rate", 0.0},
        {"smudge_length", 100.0},
        {"sensor_drives", QString()},
        {"lock_size", false},
        {"lock_opacity", false},
        {"lock_texture", false},
        {"brush_erase_blend", false},
    };
    return k;
}

// Reset every value option of a tool to its schema default, then the
// popup-owned rows above (the schema only carries the bar-level rows).
// Structural rows (labels, separators, buttons, the preset well) carry
// no value and are skipped, as are kinds without a usable default.
inline void resetBrushToolToDefaults(AppState* state, ToolId tool) {
    for (const OptionSpec& spec : optionsFor(tool)) {
        switch (spec.kind) {
            case OptionKind::Combo:
            case OptionKind::Spin:
            case OptionKind::Slider:
            case OptionKind::Check:
            case OptionKind::ToggleGroup:
            case OptionKind::ColorWell:
            case OptionKind::Text:
                break;
            default:
                continue;
        }
        if (!spec.defaultValue.isValid()) continue;
        state->setOption(tool, QString::fromUtf8(spec.id),
                         spec.defaultValue);
    }
    for (const auto& kv : brushPopupDefaults())
        state->setOption(tool, QString::fromUtf8(kv.first), kv.second);
}

// Snapshot the tool's live options into a preset (the inverse of
// applyPreset above): same ids, same fallbacks, so capture → apply is a
// fixed point. Powers "Save as preset".
inline BrushPreset captureBrushState(AppState* state, ToolId tool) {
    BrushPreset p;
    p.factory = false;
    auto dbl = [&](const char* id, double fallback) {
        const QVariant v = state->option(tool, QString::fromUtf8(id));
        return v.isValid() ? v.toDouble() : fallback;
    };
    auto flag = [&](const char* id, bool fallback = false) {
        const QVariant v = state->option(tool, QString::fromUtf8(id));
        return v.isValid() ? v.toBool() : fallback;
    };
    auto str = [&](const char* id, const QString& fallback = QString()) {
        const QVariant v = state->option(tool, QString::fromUtf8(id));
        return v.isValid() ? v.toString() : fallback;
    };
    p.size = dbl("brush_size", 64.0);
    p.hardness = dbl("brush_hardness", 50.0);
    p.angle = dbl("brush_angle", 0.0);
    p.roundness = dbl("brush_roundness", 100.0);
    p.spacing = dbl("brush_spacing", 15.0);
    p.tip = int(dbl("brush_tip", 0.0));
    p.spikes = int(dbl("brush_spikes", 0.0));
    p.fadeAniso = dbl("brush_fade_aniso", 0.0);
    p.falloff = int(dbl("brush_falloff", 0.0));
    p.sharpness = dbl("brush_sharpness", 0.0);
    p.soften = dbl("brush_soften", 0.0);
    p.spacingAuto = flag("brush_spacing_auto");
    p.spacingAutoCoeff = dbl("brush_spacing_auto_coeff", 0.0);
    p.opacity = dbl("opacity", 100.0);
    p.pressureSize = flag("pressure_size", true);
    p.pressureOpacity = flag("pressure_opacity", true);
    p.sizeCurve = str("brush_size_curve");
    p.opacityCurve = str("brush_opacity_curve");
    p.flowCurve = str("brush_flow_curve");
    p.stampId = str("brush_stamp");
    p.tipKind = p.stampId.isEmpty() ? QString() : QStringLiteral("stamp");
    p.stampMode = int(dbl("brush_stamp_mode", 0.0));
    p.tipNeutral = dbl("brush_tip_neutral", 50.0);
    p.tipBrightness = dbl("brush_tip_brightness", 0.0);
    p.tipContrast = dbl("brush_tip_contrast", 100.0);
    p.tipThickness = dbl("brush_tip_thickness", 0.0);
    p.engine = str("brush_engine");
    p.smudgeRate = dbl("brush_smudge_rate", 70.0);
    p.smudgeRadius = dbl("brush_smudge_radius", 100.0);
    p.smudgeMode = int(dbl("smudge_mode", 0.0));
    p.smudgeColorRate = dbl("smudge_color_rate", 0.0);
    p.smudgeLength = dbl("smudge_length", 100.0);
    p.drives = sensordrive::decodeDrives(str("sensor_drives"));
    p.scatterPct = dbl("brush_scatter", 0.0);
    p.scatterX = flag("brush_scatter_x", true);
    p.scatterY = flag("brush_scatter_y", true);
    p.densityPct = dbl("brush_density", 100.0);
    p.airbrush = flag("airbrush");
    p.airbrushRate = dbl("airbrush_rate", 20.0);
    p.flowPct = dbl("flow", 100.0);
    p.tiltRotation = flag("tilt_rotation");
    p.textureFile = str("brush_texture");
    p.textureStrength = dbl("brush_texture_strength", 80.0);
    p.textureScale = dbl("brush_texture_scale", 100.0);
    p.textureNeutral = dbl("brush_texture_neutral", 50.0);
    p.textureBrightness = dbl("brush_texture_brightness", 0.0);
    p.textureContrast = dbl("brush_texture_contrast", 100.0);
    p.textureInvert = flag("brush_texture_invert");
    p.textureGrain = flag("brush_texture_grain");
    p.hoseId = str("brush_hose");
    p.paintingMode = str("brush_painting_mode", QStringLiteral("wash"));
    p.compositeOp = str("brush_composite", QStringLiteral("normal"));
    p.rotationMode = int(dbl("brush_rotation", 0.0));
    p.rotationCurve = str("brush_rotation_curve");
    p.sourceMode = int(dbl("brush_source", 0.0));
    p.spacingIsotropic = flag("brush_spacing_isotropic");
    p.maskStamp = str("brush_mask_stamp");
    p.maskMode = int(dbl("brush_mask_mode", 0.0));
    p.maskRatio = dbl("brush_mask_ratio", 100.0);
    p.maskAngle = dbl("brush_mask_angle", 0.0);
    p.textureMode = int(dbl("brush_texture_mode", 0.0));
    p.textureSoft = flag("brush_texture_soft");
    p.textureAutoInvertEraser = flag("brush_texture_auto_invert_eraser");
    p.textureCutoffPolicy = int(dbl("brush_texture_cutoff_policy", 0.0));
    p.textureCutLo = dbl("brush_texture_cutlo", 0.0);
    p.textureCutHi = dbl("brush_texture_cuthi", 1.0);
    p.flipX = flag("brush_flip_x");
    p.flipY = flag("brush_flip_y");
    p.tiltSize = dbl("brush_tilt_size", 0.0);
    p.tiltOpacity = dbl("brush_tilt_opacity", 0.0);
    p.tiltMaster = dbl("brush_tilt_master", 100.0);
    p.tangentialFlow = flag("brush_tangential_flow");
    p.texturePressure = flag("brush_texture_pressure");
    p.maskPressure = flag("brush_mask_pressure");
    p.smudgePressure = flag("brush_smudge_pressure");
    p.gradientLen = dbl("brush_gradient_len", 500.0);
    p.smoothing = dbl("smoothing", 0.0);
    p.smoothingMode = int(dbl("smoothing_mode", 0.0));
    p.tipFilter = int(dbl("brush_tip_filter", 1.0));
    p.eraserBlend = flag("brush_erase_blend");
    p.fadeLen = dbl("brush_fade", 0.0);
    p.darkenPct = dbl("brush_darken", 0.0);
    p.hueJitter = dbl("brush_hue_jitter", 0.0);
    p.satJitter = dbl("brush_sat_jitter", 0.0);
    p.valJitter = dbl("brush_val_jitter", 0.0);
    p.pressureIn = flag("brush_pressure_in");
    p.speedSize = dbl("brush_speed_size", 0.0);
    p.tiltXSize = dbl("brush_tiltx_size", 0.0);
    p.tiltYSize = dbl("brush_tilty_size", 0.0);
    p.timeFade = dbl("brush_timefade", 0.0);
    p.fuzzySize = dbl("brush_fuzzy_size", 0.0);
    p.fuzzyOpacity = dbl("brush_fuzzy_opacity", 0.0);
    p.perspective = dbl("brush_perspective", 0.0);
    p.vpX = dbl("brush_vp_x", -1.0);
    p.vpY = dbl("brush_vp_y", -1.0);
    return p;
}

}  // namespace pittore::ui::brushlibrary
