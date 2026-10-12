#pragma once
// Preset-bundle import: a .bundle file is a ZIP of preset images, tip files
// and an XML manifest; each preset image carries its engine parameters as a
// compressed XML text annotation. All parsing here is original work from the
// publicly documented container layout. Only bundles the user imports are
// ever read; nothing third-party ships with the app.
//
// Best-effort mapping into our preset model:
//   paint engine    -> Brush (Eraser when the eraser flag is set)
//   smudge engine   -> Brush with the smudge engine flag (rate + radius)
//   generated tip   -> auto tip (ratio/angle/hardness/spacing)
//   bitmap tip      -> stamp tip (embedded image preferred, else the
//                      bundle's tip file; the mask flag picks the mode)
//   pressure gates  -> our pressure-for-size / pressure-for-opacity options
//   authored curves -> size/opacity/flow/rotation response curves
//   rotation sensor -> rotation source (tilt, drawing angle, pressure,
//                      barrel, fuzzy) with its curve where applicable
//   color jitter    -> dab color source (random hue, FG/BG pressure mix)
//   stylus tilt     -> lean sensor drive with the authored curve when the
//                      sensor entry carries one, else the linear lean
//                      response (noted approximate)
//   wheel sensor    -> tangential sensor drive with the authored curve when
//                      present, else the tangential-flow toggle (noted
//                      approximate)
//   smoothing params -> stabilizer strength (noted approximate)
//   masked twin     -> second-tip mask when its tip resolves to a bitmap
//                      (multiply combine, approximated)
//   paper texture   -> grain tile with levels, combine mode and cutoff
//   scatter, density, airbrush, flow, painting mode, hose selection
//                   -> direct mapping
//   exotic engines, masked auto tips and unresolvable files
//                   -> skipped, recorded in `approximateNotes`
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QImage>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include "engine/io/zip.h"
#include "ui/brushes/sensor_drives.h"

namespace pittore::ui::bundleimport {

struct BundlePreset {
    QString name;
    QString targetTool;  // "Brush" or "Eraser"
    double size = 64.0;
    double hardness = 50.0;
    double angleDeg = 0.0;
    double roundness = 100.0;
    double spacingPct = 15.0;
    bool useAutoSpacing = false;
    double autoSpacingCoeff = 1.0;
    double opacity = 100.0;
    bool pressureSize = true;
    bool pressureOpacity = true;
    // Authored response curves ("value|x,y;..." or empty = built-in
    // response). Only an explicit sensor curve counts; untouched defaults
    // stay on the legacy path.
    QString sizeCurve, opacityCurve, flowCurve;
    bool squareTip = false;
    bool isStamp = false;
    QByteArray stampPng;  // PNG bytes for the stamp (when isStamp)
    QString stampExt;     // "png" for PNG bytes, else native format ext
    int stampMode = 0;    // 0 alpha mask, 1 color image
    QStringList tags;     // bundle grouping labels (empty = untagged)
    // Smudge engine + extras.
    bool isSmudge = false;
    double smudgeRate = 70.0;    // 0..100
    double smudgeRadius = 100.0;  // 0..100 (% of dab radius)
    double scatterPct = 0.0;
    bool scatterX = true, scatterY = true;
    double densityPct = 100.0;
    bool airbrush = false;
    double airbrushRate = 20.0;
    double flowPct = 100.0;
    // Paper grain (pattern PNG bytes + levels).
    bool hasTexture = false;
    QByteArray texturePng;
    // Painting mode: "wash" (opacity caps the whole stroke) or "buildup"
    // (opacity works per dab). Missing or other values mean wash.
    QString paintingMode = QStringLiteral("wash");
    // Dab composite op: "normal" or one of the pigment-mix ids
    // ("geometric_mean", "parallel", "allanon"). Anything else paints as
    // normal with an approximate note.
    QString compositeOp = QStringLiteral("normal");
    // Darken dynamics amount 0..100 (pressure-driven pull toward black).
    double darkenPct = 0.0;
    // Sharpness threshold + soft fringe 0..100 (static transfer; sensor
    // response noted approximate when a curve drives them).
    double sharpness = 0.0;
    double softness = 0.0;
    int textureMode = 0;          // 0 multiply, 1 subtract, 2 darken
    int textureCutoffPolicy = 0;  // 0 off, else window the grain
    double textureCutLo = 0.0;    // 0..1
    double textureCutHi = 1.0;    // 0..1
    double textureStrength = 80.0;
    double textureScale = 100.0;
    double textureNeutral = 50.0;
    double textureBrightness = 0.0;
    double textureContrast = 100.0;
    bool textureInvert = false;
    // Tip rotation source: 0 off, 1 tilt, 2 drawing angle, 3 pressure,
    // 4 barrel, 5 fuzzy. Plus the pressure/fuzzy response curve.
    int rotationMode = 0;
    QString rotationCurve;
    // Dab color source: 0 foreground, 1 random hue, 2 FG/BG pressure mix.
    int sourceMode = 0;
    // Static tip mirroring.
    bool flipX = false, flipY = false;
    // Stylus lean response (0..100, 0 = off) and wheel-to-flow toggle.
    // These cover bare sensor mentions (no curve to transfer); sensors
    // with explicit curves become sensor drives below instead.
    double tiltSize = 0.0;
    double tiltOpacity = 0.0;
    bool tangentialFlow = false;
    // Sensor drives for tilt/wheel sensors that carry explicit curves
    // (exact shape transfer; amounts follow our lean/wheel conventions).
    std::vector<sensordrive::SensorDrive> drives;
    // Stroke stabilizer strength (0..100, 0 = off).
    double smoothing = 0.0;
    // Masked second tip (bitmap bytes + combine mode/ratio/angle).
    bool hasMask = false;
    QByteArray maskPng;
    QString maskExt;
    int maskMode = 0;
    double maskRatio = 100.0;
    double maskAngle = 0.0;
    QStringList approximateNotes;
};

struct BundleResult {
    QList<BundlePreset> presets;
    // Tips shipped with no preset referencing them: (baseName, pngBytes).
    QList<QPair<QString, QByteArray>> looseTips;
    QString bundleTitle;
    int skippedFiles = 0;
};

namespace detail {

inline double attrDouble(const QXmlStreamAttributes& a, const char* key,
                         double fallback) {
    const QString v = a.value(QLatin1String(key)).toString();
    if (v.isEmpty()) return fallback;
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : fallback;
}

inline bool hasPressureSensor(const QString& sensorXml) {
    // <params id="pressure"> with a <curve> child means pressure drives it.
    return sensorXml.contains(QLatin1String("id=\"pressure\"")) &&
           sensorXml.contains(QLatin1String("<curve>"));
}

// One sensor entry: its id plus the first <curve> body inside its block
// (empty when the entry carries no curve, e.g. <params id="x"/>).
struct SensorEntry {
    QString id;
    QString curve;
};

// All <params id="..."> entries in a sensor blob, in order. Bare entries
// (<params id="x"/>) yield an empty curve; only the first curve per entry
// is taken.
inline std::vector<SensorEntry> sensorEntries(const QString& sensorXml) {
    std::vector<SensorEntry> out;
    int at = 0;
    while (true) {
        const int tag =
            sensorXml.indexOf(QLatin1String("<params"), at, Qt::CaseInsensitive);
        if (tag < 0) break;
        const int end = sensorXml.indexOf(QLatin1Char('>'), tag);
        if (end < 0) break;
        const QString head = sensorXml.mid(tag, end - tag);
        const int idAt = head.indexOf(QLatin1String("id=\""), 0,
                                      Qt::CaseInsensitive);
        QString id;
        if (idAt >= 0) {
            const int vs = idAt + 4;
            const int ve = head.indexOf(QLatin1Char('"'), vs);
            if (ve > vs) id = head.mid(vs, ve - vs);
        }
        SensorEntry e;
        e.id = id;
        if (!head.trimmed().endsWith(QLatin1Char('/'))) {
            const int next =
                sensorXml.indexOf(QLatin1String("<params"), end,
                                  Qt::CaseInsensitive);
            const int scopeEnd = next >= 0 ? next : sensorXml.size();
            const int curveAt = sensorXml.indexOf(QLatin1String("<curve>"),
                                                  end);
            if (curveAt >= 0 && curveAt < scopeEnd) {
                const int curveTo = sensorXml.indexOf(
                    QLatin1String("</curve>"), curveAt);
                if (curveTo >= 0 && curveTo < scopeEnd)
                    e.curve = sensorXml
                                  .mid(curveAt + 7, curveTo - (curveAt + 7))
                                  .trimmed();
            }
        }
        out.push_back(e);
        at = end + 1;
    }
    return out;
}

// First non-empty curve whose entry id contains `needle` (case-insensitive).
// Empty when no matching entry carries a curve.
inline QString sensorCurveFor(const QString& sensorXml, const char* needle) {
    const QString want = QString::fromUtf8(needle).toLower();
    for (const SensorEntry& e : sensorEntries(sensorXml)) {
        if (!e.curve.isEmpty() &&
            e.id.toLower().contains(want))
            return e.curve;
    }
    return QString();
}

// First non-empty curve inside a <ChildSensor id="...pressure..."> block
// (case-insensitive). Sensor lists (<params id="sensorslist">) carry one
// block per driver (fuzzy, speed, pressure, ...); the plain first-<curve>
// scan would grab whichever driver comes first. Empty when no pressure
// block carries a curve.
inline QString pressureChildCurve(const QString& sensorXml) {
    int at = 0;
    while (true) {
        const int tag =
            sensorXml.indexOf(QLatin1String("<ChildSensor"), at,
                              Qt::CaseInsensitive);
        if (tag < 0) break;
        const int headEnd = sensorXml.indexOf(QLatin1Char('>'), tag);
        if (headEnd < 0) break;
        const QString head =
            sensorXml.mid(tag, headEnd - tag).toLower();
        const int next =
            sensorXml.indexOf(QLatin1String("<ChildSensor"), headEnd,
                              Qt::CaseInsensitive);
        const int scopeEnd = next >= 0 ? next : sensorXml.size();
        if (head.contains(QLatin1String("pressure"))) {
            const int curveAt = sensorXml.indexOf(QLatin1String("<curve>"),
                                                  headEnd);
            if (curveAt >= 0 && curveAt < scopeEnd) {
                const int curveTo = sensorXml.indexOf(
                    QLatin1String("</curve>"), curveAt);
                if (curveTo >= 0 && curveTo < scopeEnd) {
                    const QString pts =
                        sensorXml.mid(curveAt + 7, curveTo - (curveAt + 7))
                            .trimmed();
                    if (!pts.isEmpty()) return pts;
                }
            }
            return QString();
        }
        at = headEnd + 1;
    }
    return QString();
}

// All <ChildSensor id="..."> blocks in a sensor list, in order: id plus
// the first <curve> body inside the block (empty when the block carries
// no curve).
inline std::vector<SensorEntry> childSensorEntries(
    const QString& sensorXml) {
    std::vector<SensorEntry> out;
    int at = 0;
    while (true) {
        const int tag =
            sensorXml.indexOf(QLatin1String("<ChildSensor"), at,
                              Qt::CaseInsensitive);
        if (tag < 0) break;
        const int headEnd = sensorXml.indexOf(QLatin1Char('>'), tag);
        if (headEnd < 0) break;
        const QString head = sensorXml.mid(tag, headEnd - tag);
        const int idAt = head.indexOf(QLatin1String("id=\""), 0,
                                      Qt::CaseInsensitive);
        QString id;
        if (idAt >= 0) {
            const int vs = idAt + 4;
            const int ve = head.indexOf(QLatin1Char('"'), vs);
            if (ve > vs) id = head.mid(vs, ve - vs);
        }
        SensorEntry e;
        e.id = id;
        const int next =
            sensorXml.indexOf(QLatin1String("<ChildSensor"), headEnd,
                              Qt::CaseInsensitive);
        const int scopeEnd = next >= 0 ? next : sensorXml.size();
        const int curveAt =
            sensorXml.indexOf(QLatin1String("<curve>"), headEnd);
        if (curveAt >= 0 && curveAt < scopeEnd) {
            const int curveTo = sensorXml.indexOf(QLatin1String("</curve>"),
                                                  curveAt);
            if (curveTo >= 0 && curveTo < scopeEnd)
                e.curve = sensorXml.mid(curveAt + 7, curveTo - (curveAt + 7))
                              .trimmed();
        }
        out.push_back(e);
        at = headEnd + 1;
    }
    return out;
}

// Invert authored curve points ("x,y;..." -> "x,1-y;...") so an additive
// drive with amount -100 reproduces a multiplicative companion exactly
// (factor = 1 - (1-S) = S). Items that do not parse are dropped, matching
// the response parser.
inline QString invertCurvePts(const QString& pts) {
    QStringList items;
    for (const QString& item :
         pts.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int comma = item.indexOf(QLatin1Char(','));
        if (comma < 0) continue;
        bool okY = false;
        const double y = item.mid(comma + 1).toDouble(&okY);
        if (!okY) continue;
        items.push_back(item.left(comma + 1) +
                        QString::number(std::clamp(1.0 - y, 0.0, 1.0), 'g',
                                        12));
    }
    if (items.isEmpty()) return QString();
    return items.join(QLatin1Char(';')) + QLatin1Char(';');
}

inline double normDeg(double deg) {
    while (deg > 180.0) deg -= 360.0;
    while (deg <= -180.0) deg += 360.0;
    return deg;
}

// Brush definitions live one level down: the main tip is the
// <param name="brush_definition"> CDATA payload (an inner <Brush> document),
// NOT a top-level element. (The masked twin hides the same way inside
// MaskingBrush/Preset/brush_definition and is intentionally ignored here.)
struct BrushTag {
    QString type;  // auto_brush, png_brush, gbr_brush, gih_brush, abr_brush
    QString filename;
    QString md5;
    double angleRad = 0.0;
    double spacing = 0.1;  // fraction of diameter
    double scale = 1.0;
    bool useAutoSpacing = false;
    double autoSpacingCoeff = 1.0;
    bool colorAsMask = true;
    QString maskType;  // circle, rect
    double diameter = 64.0;
    double ratio = 1.0;
    double fade = 0.5;  // avg(hfade, vfade)
    double density = 1.0;
    bool found = false;
};

inline BrushTag firstBrushTag(const QString& brushDefXml) {
    BrushTag tag;
    if (brushDefXml.trimmed().isEmpty()) return tag;
    QXmlStreamReader r(brushDefXml);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("Brush")) {
            const auto a = r.attributes();
            tag.type = a.value(QLatin1String("type")).toString();
            tag.filename = a.value(QLatin1String("filename")).toString();
            tag.md5 = a.value(QLatin1String("md5sum")).toString();
            tag.angleRad = attrDouble(a, "angle", 0.0);
            tag.spacing = attrDouble(a, "spacing", 0.1);
            tag.scale = attrDouble(a, "scale", 1.0);
            const QString autoSpacing =
                a.value(QLatin1String("useAutoSpacing")).toString();
            tag.useAutoSpacing =
                autoSpacing.compare(QLatin1String("1"),
                                    Qt::CaseSensitive) == 0 ||
                autoSpacing.compare(QLatin1String("true"),
                                    Qt::CaseInsensitive) == 0;
            tag.autoSpacingCoeff = attrDouble(a, "autoSpacingCoeff", 1.0);
            tag.colorAsMask =
                a.value(QLatin1String("ColorAsMask")).toString() !=
                QLatin1String("0");
            tag.density = attrDouble(a, "density", 1.0);
            // MaskGenerator child (auto tips).
            while (!r.atEnd()) {
                r.readNext();
                if (r.isStartElement() &&
                    r.name() == QLatin1String("MaskGenerator")) {
                    const auto m = r.attributes();
                    tag.maskType = m.value(QLatin1String("type")).toString();
                    tag.diameter = attrDouble(m, "diameter", 64.0);
                    tag.ratio = attrDouble(m, "ratio", 1.0);
                    tag.fade = (attrDouble(m, "hfade", 0.5) +
                                attrDouble(m, "vfade", 0.5)) *
                               0.5;
                }
                if (r.isEndElement() && r.name() == QLatin1String("Brush"))
                    break;
            }
            tag.found = true;
            return tag;
        }
    }
    return tag;
}

struct ResourceEntry {
    QString type;      // brushes, patterns, ...
    QString filename;  // as referenced
    QString md5;
    QByteArray pngBytes;  // embedded CDATA (may be empty)
};

inline QList<ResourceEntry> resourceEntries(const QString& xml) {
    QList<ResourceEntry> out;
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("resource")) {
            ResourceEntry e;
            const auto a = r.attributes();
            e.type = a.value(QLatin1String("type")).toString();
            e.filename = a.value(QLatin1String("filename")).toString();
            e.md5 = a.value(QLatin1String("md5sum")).toString();
            QString cdata;
            while (!r.atEnd()) {
                r.readNext();
                if (r.isCharacters()) cdata += r.text().toString();
                if (r.isEndElement() &&
                    r.name() == QLatin1String("resource"))
                    break;
            }
            cdata.remove(QChar::Space);
            cdata.remove(QChar::LineFeed);
            cdata.remove(QChar::CarriageReturn);
            cdata.remove(QChar::Tabulation);
            if (!cdata.isEmpty())
                e.pngBytes = QByteArray::fromBase64(cdata.toLatin1());
            out.push_back(e);
        }
    }
    return out;
}

inline QMap<QString, QString> flatParams(const QString& xml) {
    QMap<QString, QString> out;
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("param")) {
            const QString name =
                r.attributes().value(QLatin1String("name")).toString();
            QString text;
            while (!r.atEnd()) {
                r.readNext();
                if (r.isCharacters()) text += r.text().toString();
                if (r.isEndElement() && r.name() == QLatin1String("param"))
                    break;
            }
            if (!name.isEmpty()) out.insert(name, text);
        }
    }
    return out;
}

inline double paramDouble(const QMap<QString, QString>& p, const char* key,
                          double fallback) {
    auto it = p.find(QLatin1String(key));
    if (it == p.end()) return fallback;
    bool ok = false;
    const double d = it->toDouble(&ok);
    return ok ? d : fallback;
}

}  // namespace detail

// Parse one .kpp file's bytes. `bundleFiles` maps lower-cased bundle paths
// (e.g. "brushes/foo.gbr") to raw bytes for external tip resolution.
inline BundlePreset parseKpp(const QString& kppName, const QByteArray& kppBytes,
                             const QMap<QString, QByteArray>& bundleFiles,
                             bool* ok = nullptr) {
    BundlePreset p;
    p.name = kppName;
    if (ok) *ok = false;
    QImage icon;
    if (!icon.loadFromData(kppBytes, "PNG")) return p;
    const QString xml = icon.text(QStringLiteral("preset"));
    if (xml.isEmpty()) return p;

    // Preset root: engine + display name. Engine id also appears as the
    // "paintop" param; the attribute is preferred.
    QString engineId;
    {
        QXmlStreamReader r(xml);
        while (!r.atEnd()) {
            r.readNext();
            if (r.isStartElement() &&
                r.name() == QLatin1String("Preset")) {
                engineId =
                    r.attributes().value(QLatin1String("paintopid")).toString();
                const QString name =
                    r.attributes().value(QLatin1String("name")).toString();
                if (!name.isEmpty()) p.name = name;
                break;
            }
        }
    }

    const auto params = detail::flatParams(xml);
    const bool maskEnabled =
        params.value(QLatin1String("MaskingBrush/Enabled")).trimmed() ==
        QLatin1String("true");
    if (engineId.isEmpty())
        engineId = params.value(QLatin1String("paintop")).trimmed();
    if (!engineId.isEmpty()) {
        if (engineId == QLatin1String("colorsmudge")) {
            p.isSmudge = true;
            p.smudgeRate =
                std::clamp(detail::paramDouble(params, "SmudgeRateValue", 0.7),
                           0.0, 1.0) *
                100.0;
            p.smudgeRadius =
                std::clamp(detail::paramDouble(params, "SmudgeRadiusValue", 1.0),
                           0.05, 1.0) *
                100.0;
        } else if (engineId != QLatin1String("paintbrush"))
            p.approximateNotes.push_back(QStringLiteral("unsupported engine ") +
                                         engineId);
    }
    if (ok) *ok = !p.name.isEmpty();
    const auto resources = detail::resourceEntries(xml);
    const bool eraser =
        params.value(QLatin1String("EraserMode")).trimmed() ==
        QLatin1String("true");
    p.targetTool = eraser ? QStringLiteral("Eraser") : QStringLiteral("Brush");
    p.opacity =
        std::clamp(detail::paramDouble(params, "OpacityValue", 1.0), 0.0, 1.0) *
        100.0;
    // Pressure gates: an enabled curve with a pressure sensor keeps ours on;
    // explicitly disabled curves turn ours off.
    if (params.value(QLatin1String("SizeUseCurve")).trimmed() ==
            QLatin1String("false") ||
        (params.contains(QLatin1String("SizeUseCurve")) &&
         !detail::hasPressureSensor(params.value(QLatin1String("SizeSensor")))))
        p.pressureSize = false;
    if (params.value(QLatin1String("OpacityUseCurve")).trimmed() ==
            QLatin1String("false") ||
        (params.contains(QLatin1String("OpacityUseCurve")) &&
         !detail::hasPressureSensor(
             params.value(QLatin1String("OpacitySensor")))))
        p.pressureOpacity = false;

    // Authored pressure curves: an enabled curve with an explicit sensor
    // curve transfers shape ("value|points"); anything else keeps the
    // built-in response. Explicit sensor curves win over shared defaults.
    // Sensor lists carry one <ChildSensor> block per driver: prefer the
    // pressure block so a leading fuzzy/speed entry cannot pose as the
    // pressure response (PW-03 size grabbed fuzzy, flow grabbed speed).
    auto takeCurve = [&](const char* useKey, const char* sensorKey,
                         const char* valueKey) {
        if (params.value(QLatin1String(useKey)).trimmed() ==
            QLatin1String("false"))
            return QString();
        const QString sensor = params.value(QLatin1String(sensorKey));
        QString pts = detail::pressureChildCurve(sensor);
        if (pts.isEmpty()) {
            const int at = sensor.indexOf(QLatin1String("<curve>"));
            if (at < 0) return QString();
            const int to = sensor.indexOf(QLatin1String("</curve>"), at);
            if (to < 0) return QString();
            pts = sensor.mid(at + 7, to - (at + 7)).trimmed();
            if (pts.isEmpty()) return QString();
        }
        return QString::number(
                   detail::paramDouble(params, valueKey, 1.0)) +
               QLatin1Char('|') + pts;
    };
    p.sizeCurve = takeCurve("SizeUseCurve", "SizeSensor", "SizeValue");
    p.opacityCurve =
        takeCurve("OpacityUseCurve", "OpacitySensor", "OpacityValue");
    p.flowCurve = takeCurve("FlowUseCurve", "FlowSensor", "FlowValue");

    // Multiplicative sensor companions (curveMode 0): non-pressure drivers
    // in a sensor list (fuzzy jitter, speed, ...) scale the main response
    // in Krita. An additive drive with amount -100 and the inverted curve
    // reproduces that product exactly (factor = 1 - (1-S) = S), so each
    // companion becomes one drive on the same property. Other combine
    // modes, and sensors with no engine counterpart, are noted + skipped.
    // (Caveats, all noted at the mapping: fuzzy reads as per-dab uniform
    // jitter, curves evaluate piecewise-linear, and speed normalizes to
    // our 0..1 scale rather than Krita's.)
    auto takeCompanions = [&](const char* useKey, const char* sensorKey,
                              const char* modeKey, const char* sameKey,
                              const char* commonKey, const char* prop) {
        if (params.value(QLatin1String(useKey)).trimmed() ==
            QLatin1String("false"))
            return;
        const QString sensor = params.value(QLatin1String(sensorKey));
        const int mode = int(detail::paramDouble(params, modeKey, 0.0));
        if (mode != 0) {
            if (sensor.contains(QLatin1String("<ChildSensor")))
                p.approximateNotes.push_back(
                    QStringLiteral("curve combine mode %1 not transferred "
                                   "for %2")
                        .arg(mode)
                        .arg(QString::fromUtf8(prop)));
            return;
        }
        QString commonPts;
        if (params.value(QLatin1String(sameKey)).trimmed() ==
            QLatin1String("true")) {
            const QString common =
                params.value(QLatin1String(commonKey)).trimmed();
            const int at = common.indexOf(QLatin1String("<curve>"));
            if (at >= 0) {
                const int to = common.indexOf(QLatin1String("</curve>"), at);
                if (to > at)
                    commonPts =
                        common.mid(at + 7, to - (at + 7)).trimmed();
            } else {
                commonPts = common;
            }
        }
        for (const detail::SensorEntry& e :
             detail::childSensorEntries(sensor)) {
            const QString low = e.id.toLower();
            if (low.contains(QLatin1String("pressure"))) continue;  // main
            const QString pts =
                commonPts.isEmpty() ? e.curve : commonPts;
            if (pts.isEmpty()) continue;
            sensordrive::Sensor target = sensordrive::Sensor::Pressure;
            bool known = true;
            if (low.contains(QLatin1String("fuzzy")))
                target = sensordrive::Sensor::FuzzyDab;
            else if (low.contains(QLatin1String("speed")))
                target = sensordrive::Sensor::Speed;
            else if (low.contains(QLatin1String("tiltx")))
                target = sensordrive::Sensor::TiltX;
            else if (low.contains(QLatin1String("tilty")))
                target = sensordrive::Sensor::TiltY;
            else if (low.contains(QLatin1String("tilt")))
                target = sensordrive::Sensor::Lean;
            else if (low.contains(QLatin1String("tangent")))
                target = sensordrive::Sensor::Tangential;
            else if (low.contains(QLatin1String("rotation")) ||
                     low.contains(QLatin1String("barrel")))
                target = sensordrive::Sensor::Barrel;
            else if (low.contains(QLatin1String("distance")))
                target = sensordrive::Sensor::Distance;
            else if (low.contains(QLatin1String("time")))
                target = sensordrive::Sensor::Time;
            else
                known = false;
            if (!known) {
                p.approximateNotes.push_back(
                    QStringLiteral("sensor '%1' not transferred for %2")
                        .arg(e.id.isEmpty() ? QStringLiteral("?") : e.id)
                        .arg(QString::fromUtf8(prop)));
                continue;
            }
            const QString inv = detail::invertCurvePts(pts);
            if (inv.isEmpty()) continue;
            sensordrive::SensorDrive d;
            d.prop = prop;
            d.sensor = target;
            d.amount = -100.0;
            d.curve = (QStringLiteral("1|") + inv).toStdString();
            p.drives.push_back(d);
        }
    };
    takeCompanions("SizeUseCurve", "SizeSensor", "SizecurveMode",
                   "SizeUseSameCurve", "SizecommonCurve", "size");
    takeCompanions("OpacityUseCurve", "OpacitySensor", "OpacitycurveMode",
                   "OpacityUseSameCurve", "OpacitycommonCurve", "opacity");
    takeCompanions("FlowUseCurve", "FlowSensor", "FlowcurveMode",
                   "FlowUseSameCurve", "FlowcommonCurve", "flow");

    // Tip rotation source. The sensor XML names the driver: a drawing-angle
    // entry aligns the tip with the stroke, pressure spins around the
    // half-way point, tilt leans it, barrel follows stylus rotation, and
    // fuzzy rolls per dab. Lists name their winner by priority below; an
    // unknown sensor (or no curve use) keeps the fixed tip angle.
    {
        const bool use =
            params.value(QLatin1String("RotationUseCurve")).trimmed() !=
            QLatin1String("false");
        const QString sensor =
            params.value(QLatin1String("RotationSensor"));
        const QString low = sensor.toLower();
        const bool hasDrawing = low.contains(QLatin1String("drawingangle"));
        const bool hasPressure = low.contains(QLatin1String("\"pressure\"")) ||
                                 low.contains(QLatin1String("id=\"pressure\""));
        const bool hasTilt = low.contains(QLatin1String("tilt"));
        const bool hasBarrel =
            low.contains(QLatin1String("id=\"rotation\"")) ||
            low.contains(QLatin1String("barrel"));
        const bool hasFuzzy = low.contains(QLatin1String("fuzz"));
        int mode = 0;
        if (use && hasDrawing)
            mode = 2;
        else if (use && hasPressure)
            mode = 3;
        else if (use && hasTilt)
            mode = 1;
        else if (use && hasBarrel)
            mode = 4;
        else if (use && hasFuzzy)
            mode = 5;
        p.rotationMode = mode;
        if (mode == 3 || mode == 5) {
            const int at = sensor.indexOf(QLatin1String("<curve>"));
            const int to = sensor.indexOf(QLatin1String("</curve>"), at);
            if (at >= 0 && to > at) {
                const QString pts =
                    sensor.mid(at + 7, to - (at + 7)).trimmed();
                if (!pts.isEmpty())
                    p.rotationCurve =
                        QString::number(detail::paramDouble(
                            params, "RotationValue", 1.0)) +
                        QLatin1Char('|') + pts;
            }
        }
    }

    // Dab color source: hue jitter becomes a per-dab random hue, an
    // FG/BG mix entry becomes the pressure mix. Anything else stays on
    // the plain foreground.
    {
        bool hueJitter = false, fgBgMix = false;
        for (auto it = params.begin(); it != params.end(); ++it) {
            const QString k = it.key().toLower();
            const QString v = it.value().trimmed().toLower();
            if (k.contains(QLatin1String("hue")) &&
                (v == QLatin1String("true") || v == QLatin1String("1")))
                hueJitter = true;
            if ((k.contains(QLatin1String("mix")) &&
                 (k.contains(QLatin1String("fg")) ||
                  k.contains(QLatin1String("bg")) ||
                  k.contains(QLatin1String("background")))) &&
                (v == QLatin1String("true") || v == QLatin1String("1")))
                fgBgMix = true;
        }
        if (fgBgMix)
            p.sourceMode = 2;
        else if (hueJitter)
            p.sourceMode = 1;
    }

    // Static tip mirroring (dab-level, not canvas symmetry). Sensor-driven
    // mirroring keeps the static side with a note.
    {
        p.flipX = params.value(QLatin1String("HorizontalMirrorEnabled"))
                      .trimmed()
                      .compare(QLatin1String("true"),
                               Qt::CaseInsensitive) == 0;
        p.flipY = params.value(QLatin1String("VerticalMirrorEnabled"))
                      .trimmed()
                      .compare(QLatin1String("true"),
                               Qt::CaseInsensitive) == 0;
        const QString ms = params.value(QLatin1String("MirrorSensor"));
        if ((p.flipX || p.flipY) &&
            params.value(QLatin1String("MirrorUseCurve")).trimmed() ==
                QLatin1String("true") &&
            ms.contains(QLatin1String("<curve>")))
            p.approximateNotes.push_back(
                QStringLiteral("mirror dynamics not transferred"));
    }

    // Per-brush blending: the pigment-mix ids transfer to our dab mix
    // ops; anything else paints as normal with a note.
    {
        const QString cop =
            params.value(QLatin1String("CompositeOp")).trimmed().toLower();
        if (cop == QLatin1String("geometric_mean") ||
            cop == QLatin1String("parallel") ||
            cop == QLatin1String("allanon")) {
            p.compositeOp = cop;
        } else if (!cop.isEmpty() && cop != QLatin1String("normal") &&
                   cop != QLatin1String("over") &&
                   cop != QLatin1String("svg:src-over")) {
            p.approximateNotes.push_back(
                QStringLiteral("blend mode not transferred: ") + cop);
        }
    }

    // Darken dynamics: the authored strength scales a pressure-driven pull
    // toward black (approximated by our brush_darken option).
    {
        const double dv =
            detail::paramDouble(params, "DarkenValue", 0.0);
        if (dv > 0.0 &&
            params.value(QLatin1String("DarkenUseCurve")).trimmed() !=
                QLatin1String("false")) {
            p.darkenPct = std::clamp(dv * 100.0, 0.0, 100.0);
            p.approximateNotes.push_back(
                QStringLiteral("darken dynamics approximated"));
        }
    }

    // Sharpness/softness: authored strengths transfer statically; an
    // enabled sensor curve behind them is approximated.
    {
        const double sh =
            detail::paramDouble(params, "SharpnessValue", 0.0);
        const double so =
            detail::paramDouble(params, "SoftnessValue", 0.0);
        if (sh > 0.0 || so > 0.0) {
            p.sharpness = std::clamp(sh * 100.0, 0.0, 100.0);
            p.softness = std::clamp(so * 100.0, 0.0, 100.0);
            const QString shS =
                params.value(QLatin1String("SharpnessSensor"));
            const QString soS =
                params.value(QLatin1String("SoftnessSensor"));
            if ((params.value(QLatin1String("SharpnessUseCurve")).trimmed() !=
                     QLatin1String("false") &&
                 shS.contains(QLatin1String("<curve>"))) ||
                (params.value(QLatin1String("SoftnessUseCurve")).trimmed() !=
                     QLatin1String("false") &&
                 soS.contains(QLatin1String("<curve>"))))
                p.approximateNotes.push_back(
                    QStringLiteral("sharpness dynamics approximated"));
        }
    }

    // Stylus lean + wheel: a sensor entry carrying an explicit curve
    // becomes a sensor drive (exact shape transfer; the amount follows our
    // lean/wheel conventions: size grows, opacity shrinks, flow scales).
    // A bare sensor mention keeps the legacy linear fallback + note.
    {
        const QString sizeSensor =
            params.value(QLatin1String("SizeSensor"));
        const QString opacitySensor =
            params.value(QLatin1String("OpacitySensor"));
        const QString flowSensor =
            params.value(QLatin1String("FlowSensor"));
        auto driveCurve = [&](const QString& sensorXml, const char* needle,
                              const char* valueKey) {
            const QString pts =
                detail::sensorCurveFor(sensorXml, needle);
            if (pts.isEmpty()) return QString();
            return QString::number(
                       detail::paramDouble(params, valueKey, 1.0)) +
                   QLatin1Char('|') + pts;
        };
        const QString sizeTilt =
            driveCurve(sizeSensor, "tilt", "SizeValue");
        if (!sizeTilt.isEmpty()) {
            sensordrive::SensorDrive d;
            d.prop = "size";
            d.sensor = sensordrive::Sensor::Lean;
            d.amount = 100.0;
            d.curve = sizeTilt.toStdString();
            p.drives.push_back(d);
            // The tilt-derived curve must not also pose as a pressure
            // response when no pressure sensor drives this param.
            if (!detail::hasPressureSensor(sizeSensor)) p.sizeCurve.clear();
        } else if (sizeSensor.toLower().contains(QLatin1String("tilt"))) {
            p.tiltSize = 100.0;
            p.approximateNotes.push_back(
                QStringLiteral("tilt size curve approximated as linear"));
        }
        const QString opacityTilt =
            driveCurve(opacitySensor, "tilt", "OpacityValue");
        if (!opacityTilt.isEmpty()) {
            sensordrive::SensorDrive d;
            d.prop = "opacity";
            d.sensor = sensordrive::Sensor::Lean;
            d.amount = -100.0;
            d.curve = opacityTilt.toStdString();
            p.drives.push_back(d);
            if (!detail::hasPressureSensor(opacitySensor))
                p.opacityCurve.clear();
        } else if (opacitySensor.toLower().contains(QLatin1String("tilt"))) {
            p.tiltOpacity = 100.0;
            p.approximateNotes.push_back(
                QStringLiteral("tilt opacity curve approximated as linear"));
        }
        const QString flowTangent =
            driveCurve(flowSensor, "tangent", "FlowValue");
        if (!flowTangent.isEmpty()) {
            sensordrive::SensorDrive d;
            d.prop = "flow";
            d.sensor = sensordrive::Sensor::Tangential;
            d.amount = 100.0;
            d.curve = flowTangent.toStdString();
            p.drives.push_back(d);
            if (!detail::hasPressureSensor(flowSensor)) p.flowCurve.clear();
        } else if (flowSensor.toLower().contains(QLatin1String("tangent"))) {
            p.tangentialFlow = true;
            p.approximateNotes.push_back(
                QStringLiteral("wheel flow curve approximated as linear"));
        }
    }

    // Stroke stabilizer: any smoothing-flavoured param with a positive
    // strength maps onto our 0..100 strength (type enums land mid-scale).
    {
        double best = 0.0;
        for (auto it = params.begin(); it != params.end(); ++it) {
            const QString k = it.key().toLower();
            if (!k.contains(QLatin1String("smooth")) &&
                !k.contains(QLatin1String("stabiliz")))
                continue;
            const QString v = it.value().trimmed().toLower();
            bool okN = false;
            const double d = it.value().toDouble(&okN);
            double pct = 0.0;
            if (okN && d > 0.0) {
                pct = k.contains(QLatin1String("type"))
                          ? 50.0
                          : (d <= 1.0 ? d * 100.0 : std::min(d, 100.0));
            } else if (v == QLatin1String("true") || v == QLatin1String("1")) {
                pct = 50.0;
            }
            if (pct > best) best = pct;
        }
        if (best > 0.0) {
            p.smoothing = std::clamp(best, 1.0, 100.0);
            p.approximateNotes.push_back(
                QStringLiteral("smoothing approximated"));
        }
    }

    // Scatter: only when a curve actually drives it (an enabled curve with
    // no sensor curve behind it evaluates to nothing).
    if (params.value(QLatin1String("ScatterUseCurve")).trimmed() ==
            QLatin1String("true") &&
        detail::hasPressureSensor(params.value(QLatin1String("ScatterSensor")))) {
        p.scatterPct =
            std::clamp(detail::paramDouble(params, "ScatterValue", 0.0) * 100.0,
                       0.0, 100.0);
        p.scatterX = params.value(QLatin1String("Scattering/AxisX")).trimmed() !=
                     QLatin1String("false");
        p.scatterY = params.value(QLatin1String("Scattering/AxisY")).trimmed() !=
                     QLatin1String("false");
    }
    p.flowPct = std::clamp(detail::paramDouble(params, "FlowValue", 1.0) * 100.0,
                           1.0, 100.0);
    // Painting mode: 1 means per-dab buildup, anything else (or missing)
    // means wash (opacity caps the whole stroke).
    if (params.value(QLatin1String("PaintOpAction")).trimmed() ==
        QLatin1String("1"))
        p.paintingMode = QStringLiteral("buildup");
    p.airbrush = params.value(QLatin1String("PaintOpSettings/isAirbrushing"))
                     .trimmed() == QLatin1String("true");
    p.airbrushRate = std::clamp(
        detail::paramDouble(params, "PaintOpSettings/rate", 20.0), 1.0, 100.0);

    // Paper grain.
    if (params.value(QLatin1String("Texture/Pattern/Enabled")).trimmed() ==
        QLatin1String("true")) {
        const QString texFile =
            params.value(QLatin1String("Texture/Pattern/PatternFileName"))
                .trimmed();
        const QString texMd5 =
            params
                .value(QLatin1String("Texture/Pattern/PatternMD5Sum"),
                       params.value(QLatin1String("Texture/Pattern/PatternMD5")))
                .trimmed()
                .toLower();
        QByteArray texBytes;
        for (const auto& res : resources) {
            if (res.type != QLatin1String("patterns") || res.pngBytes.isEmpty())
                continue;
            const bool nameHit =
                !texFile.isEmpty() && (res.filename == texFile ||
                                       res.filename.endsWith(texFile) ||
                                       texFile.endsWith(res.filename));
            if (nameHit || (!texMd5.isEmpty() && res.md5.toLower() == texMd5)) {
                texBytes = res.pngBytes;
                break;
            }
        }
        if (texBytes.isEmpty() && !texFile.isEmpty()) {
            const QString want = texFile.toLower();
            auto it = bundleFiles.find(QStringLiteral("patterns/") + want);
            if (it == bundleFiles.end()) {
                for (auto i = bundleFiles.begin(); i != bundleFiles.end();
                     ++i) {
                    if (i.key().endsWith(QLatin1String("/") + want) ||
                        i.key() == want) {
                        it = i;
                        break;
                    }
                }
            }
            if (it != bundleFiles.end()) texBytes = *it;
        }
        if (!texBytes.isEmpty()) {
            p.hasTexture = true;
            p.texturePng = texBytes;
            p.textureStrength =
                std::clamp(detail::paramDouble(params, "Texture/Strength/Value",
                                              0.8) *
                               100.0,
                           0.0, 100.0);
            p.textureScale =
                std::clamp(detail::paramDouble(params, "Texture/Pattern/Scale",
                                              1.0) *
                               100.0,
                           1.0, 800.0);
            p.textureNeutral =
                std::clamp(detail::paramDouble(params,
                                              "Texture/Pattern/NeutralPoint",
                                              0.5) *
                               100.0,
                           0.0, 100.0);
            p.textureBrightness =
                std::clamp(detail::paramDouble(params,
                                              "Texture/Pattern/Brightness",
                                              0.0) *
                               100.0,
                           -100.0, 100.0);
            p.textureContrast =
                std::clamp(detail::paramDouble(params,
                                              "Texture/Pattern/Contrast",
                                              1.0) *
                               100.0,
                           0.0, 400.0);
            p.textureInvert =
                params.value(QLatin1String("Texture/Pattern/Invert")).trimmed() ==
                QLatin1String("true");
            p.textureMode = std::clamp(
                int(detail::paramDouble(params, "Texture/Pattern/TexturingMode",
                                        0.0)),
                0, 2);
            if (detail::paramDouble(params, "Texture/Pattern/TexturingMode",
                                    0.0) > 2.5)
                p.approximateNotes.push_back(
                    QStringLiteral("grain combine mode clamped"));
            p.textureCutoffPolicy = std::clamp(
                int(detail::paramDouble(params, "Texture/Pattern/CutoffPolicy",
                                        0.0)),
                0, 2);
            p.textureCutLo = std::clamp(
                detail::paramDouble(params, "Texture/Pattern/CutoffLeft",
                                    0.0) /
                    255.0,
                0.0, 1.0);
            p.textureCutHi = std::clamp(
                detail::paramDouble(params, "Texture/Pattern/CutoffRight",
                                    255.0) /
                    255.0,
                0.0, 1.0);
            if (p.textureCutHi < p.textureCutLo)
                std::swap(p.textureCutLo, p.textureCutHi);
        } else if (!texFile.isEmpty()) {
            p.approximateNotes.push_back(QStringLiteral("pattern file missing: ") +
                                         texFile);
        } else {
            p.approximateNotes.push_back(
                QStringLiteral("paper texture not transferred"));
        }
    }

    // Masked second tip: when the twin brush is on and its tip resolves to
    // a bitmap, it becomes our per-dab mask (authored combine mode when the
    // kernel has it, else multiply with a note). Anything else keeps the
    // "not transferred" note.
    if (maskEnabled) {
        const detail::BrushTag maskTag = detail::firstBrushTag(
            params.value(QLatin1String("MaskingBrush/Preset/brush_definition")));
        if (!maskTag.found) {
            p.approximateNotes.push_back(
                QStringLiteral("masked twin brush not transferred"));
        } else if (maskTag.type == QLatin1String("auto_brush")) {
            p.approximateNotes.push_back(
                QStringLiteral("masked auto tip not transferred"));
        } else {
            QByteArray maskBytes;
            const QString wantFile = maskTag.filename;
            const QString wantMd5 = maskTag.md5.toLower();
            for (const auto& res : resources) {
                if (res.type != QLatin1String("brushes")) continue;
                const bool nameHit =
                    !wantFile.isEmpty() &&
                    (res.filename == wantFile ||
                     res.filename.endsWith(wantFile) ||
                     wantFile.endsWith(res.filename));
                const bool md5Hit =
                    !wantMd5.isEmpty() && res.md5.toLower() == wantMd5 &&
                    !res.pngBytes.isEmpty();
                if ((nameHit || md5Hit) && !res.pngBytes.isEmpty()) {
                    maskBytes = res.pngBytes;
                    break;
                }
            }
            if (maskBytes.isEmpty() && !wantFile.isEmpty()) {
                const QString want = wantFile.toLower();
                auto it = bundleFiles.find(QStringLiteral("brushes/") + want);
                if (it == bundleFiles.end()) {
                    for (auto i = bundleFiles.begin(); i != bundleFiles.end();
                         ++i) {
                        if (i.key().endsWith(QLatin1String("/") + want) ||
                            i.key() == want) {
                            it = i;
                            break;
                        }
                    }
                }
                if (it != bundleFiles.end()) maskBytes = *it;
            }
            static const unsigned char kPngMagic[8] = {
                137, 80, 78, 71, 13, 10, 26, 10};
            if (!maskBytes.isEmpty() && maskBytes.size() > 8 &&
                std::memcmp(maskBytes.constData(), kPngMagic, 8) == 0) {
                p.hasMask = true;
                p.maskPng = maskBytes;
                p.maskExt = QStringLiteral("png");
            } else if (!maskBytes.isEmpty()) {
                p.hasMask = true;
                p.maskPng = QByteArray("RAW:") + maskBytes;
                p.maskExt = wantFile.section(QChar('.'), -1).toLower();
            } else {
                p.approximateNotes.push_back(
                    QStringLiteral("mask tip file missing: ") + wantFile);
            }
            if (p.hasMask) {
                // Mask combine follows the authored MaskingCompositeOp when
                // our kernel has the same mode (multiply/subtract/darken/
                // lighten); anything else falls back to multiply with a note.
                const QString cop =
                    params.value(QLatin1String("MaskingBrush/MaskingCompositeOp"))
                        .trimmed()
                        .toLower();
                if (cop == QLatin1String("subtract")) {
                    p.maskMode = 1;
                } else if (cop == QLatin1String("darken") ||
                           cop == QLatin1String("darkest")) {
                    p.maskMode = 2;
                } else if (cop == QLatin1String("lighten") ||
                           cop == QLatin1String("lightest")) {
                    p.maskMode = 3;
                } else {
                    p.maskMode = 0;
                    if (!cop.isEmpty() &&
                        cop != QLatin1String("multiply"))
                        p.approximateNotes.push_back(
                            QStringLiteral("mask combine approximated as multiply: ") +
                            cop);
                }
                // Mask size tracks the main dab: the Brush scale gives the
                // base ratio, times MasterSizeCoeff when the preset follows
                // the master size (documented relative sizing).
                double ratio = maskTag.scale * 100.0;
                if (params.value(QLatin1String("MaskingBrush/UseMasterSize"))
                        .trimmed()
                        .compare(QLatin1String("true"),
                                 Qt::CaseInsensitive) == 0) {
                    bool ok = false;
                    const double coeff =
                        params.value(QLatin1String("MaskingBrush/MasterSizeCoeff"))
                            .toDouble(&ok);
                    if (ok && coeff > 0.0 && coeff <= 10.0) ratio *= coeff;
                }
                p.maskRatio = std::clamp(ratio, 5.0, 400.0);
                p.maskAngle = detail::normDeg(maskTag.angleRad * 180.0 /
                                              3.141592653589793);
            }
        }
    }

    const detail::BrushTag brush =
        detail::firstBrushTag(params.value(QLatin1String("brush_definition")));
    if (!brush.found) return p;  // params-only preset (auto tip defaults)
    p.densityPct = std::clamp(brush.density * 100.0, 0.0, 100.0);
    if (ok) *ok = true;

    p.spacingPct =
        std::clamp(brush.spacing * 100.0, 1.0, 200.0);
    p.useAutoSpacing = brush.useAutoSpacing;
    p.autoSpacingCoeff =
        std::clamp(brush.autoSpacingCoeff, 0.05, 8.0);
    p.angleDeg = detail::normDeg(brush.angleRad * 180.0 / 3.141592653589793);

    const bool predefined = brush.type != QLatin1String("auto_brush");
    if (!predefined) {
        p.size = std::clamp(brush.diameter, 1.0, 500.0);
        p.roundness = std::clamp(brush.ratio * 100.0, 1.0, 100.0);
        p.hardness = std::clamp((1.0 - brush.fade) * 100.0, 0.0, 100.0);
        p.squareTip = brush.maskType == QLatin1String("rect");
        return p;
    }

    // Predefined tip: embedded CDATA wins, else the bundle's brushes/ file.
    QByteArray tipBytes;
    const QString wantFile = brush.filename;
    const QString wantMd5 = brush.md5.toLower();
    for (const auto& res : resources) {
        if (res.type != QLatin1String("brushes")) continue;
        const bool nameHit =
            !wantFile.isEmpty() && (res.filename == wantFile ||
                                    res.filename.endsWith(wantFile) ||
                                    wantFile.endsWith(res.filename));
        const bool md5Hit = !wantMd5.isEmpty() &&
                            res.md5.toLower() == wantMd5 && !res.pngBytes.isEmpty();
        if ((nameHit || md5Hit) && !res.pngBytes.isEmpty()) {
            tipBytes = res.pngBytes;
            break;
        }
    }
    if (tipBytes.isEmpty() && !wantFile.isEmpty()) {
        const QString want = wantFile.toLower();
        auto it = bundleFiles.find(QStringLiteral("brushes/") + want);
        if (it == bundleFiles.end()) {
            // Bare filename match (bundles sometimes nest paths).
            for (auto i = bundleFiles.begin(); i != bundleFiles.end(); ++i) {
                if (i.key().endsWith(QLatin1String("/") + want) ||
                    i.key() == want) {
                    it = i;
                    break;
                }
            }
        }
        if (it != bundleFiles.end()) tipBytes = *it;
    }
    if (tipBytes.isEmpty()) {
        p.approximateNotes.push_back(QStringLiteral("tip file missing: ") +
                                     wantFile);
        return p;
    }
    // Normalize through PNG when possible; GBR/GIH stay native for the
    // engine loaders (panel converts). Detect PNG magic.
    static const unsigned char kPngMagic[8] = {137, 80, 78, 71,
                                               13,  10, 26, 10};
    if (tipBytes.size() > 8 &&
        std::memcmp(tipBytes.constData(), kPngMagic, 8) == 0) {
        p.isStamp = true;
        p.stampPng = tipBytes;
        p.stampExt = QStringLiteral("png");
    } else {
        // Non-PNG tip (GBR/GIH/ABR): stash raw bytes with a marker the
        // panel decodes via the engine loaders.
        p.isStamp = true;
        p.stampPng = QByteArray("RAW:") + tipBytes;
        p.stampExt = wantFile.section(QChar('.'), -1).toLower();
    }
    p.stampMode = brush.colorAsMask ? 0 : 1;
    p.size = 64.0;  // the relative scale attr needs a live size; user-adjustable
    return p;
}

// Full bundle: unzips are done by the caller (engine io::zipRead).
inline BundleResult importBundleEntries(
    const std::vector<pittore::io::ZipEntry>& entries) {
    BundleResult result;
    QMap<QString, QByteArray> files;  // lower-cased path -> bytes
    QMap<QString, QStringList> tagsByPath;  // lower-cased path -> tags
    for (const auto& e : entries) {
        const QString path =
            QString::fromUtf8(e.name.c_str()).toLower().replace(
                QChar('\\'), QChar('/'));
        files.insert(path, QByteArray(reinterpret_cast<const char*>(e.data.data()),
                                      int(e.data.size())));
        if (path.endsWith(QLatin1String("meta.xml"))) {
            QXmlStreamReader r(QString::fromUtf8(
                reinterpret_cast<const char*>(e.data.data()), int(e.data.size())));
            while (!r.atEnd()) {
                r.readNext();
                if (r.isStartElement() && r.name() == QLatin1String("title")) {
                    result.bundleTitle = r.readElementText();
                    break;
                }
            }
        }
        // Bundle manifest: per-file grouping tags (namespace-tolerant:
        // both element and attribute names may carry a prefix).
        if (path.endsWith(QLatin1String("manifest.xml"))) {
            QXmlStreamReader r(QString::fromUtf8(
                reinterpret_cast<const char*>(e.data.data()), int(e.data.size())));
            auto attrLocal = [](const QXmlStreamAttributes& attrs,
                                const char* local) {
                for (const auto& a : attrs) {
                    if (a.name().endsWith(QLatin1String(local)))
                        return a.value().toString();
                }
                return QString();
            };
            QString current;
            QStringList currentTags;
            auto flushEntry = [&] {
                if (!current.isEmpty() && !currentTags.isEmpty())
                    tagsByPath.insert(current.toLower(), currentTags);
                current.clear();
                currentTags.clear();
            };
            while (!r.atEnd()) {
                r.readNext();
                if (r.isStartElement() && r.name().endsWith(QLatin1String("file-entry"))) {
                    flushEntry();
                    current = attrLocal(r.attributes(), "full-path");
                    if (current.startsWith(QChar('/')))
                        current = current.mid(1);
                } else if (r.isStartElement() &&
                           r.name().endsWith(QLatin1String("tag"))) {
                    const QString tag = r.readElementText().trimmed();
                    if (!tag.isEmpty()) currentTags.push_back(tag);
                } else if (r.isEndElement() &&
                           r.name().endsWith(QLatin1String("file-entry"))) {
                    flushEntry();
                }
            }
            flushEntry();
        }
    }
    QMap<QString, bool> tipReferenced;  // bundle tip path -> referenced
    int count = 0;
    for (const auto& e : entries) {
        const QString path =
            QString::fromUtf8(e.name.c_str()).replace(QChar('\\'), QChar('/'));
        if (!path.startsWith(QLatin1String("paintoppresets/"),
                             Qt::CaseInsensitive))
            continue;
        if (!path.endsWith(QLatin1String(".kpp"), Qt::CaseInsensitive)) {
            result.skippedFiles++;
            continue;
        }
        if (count >= 128) {
            result.skippedFiles++;
            continue;
        }
        const QByteArray kpp(reinterpret_cast<const char*>(e.data.data()),
                             int(e.data.size()));
        bool ok = false;
        BundlePreset p = parseKpp(path.mid(path.lastIndexOf('/') + 1), kpp,
                                  files, &ok);
        if (!ok) {
            result.skippedFiles++;
            continue;
        }
        const QString lowerPath =
            QString::fromUtf8(e.name.c_str()).toLower().replace(QChar('\\'),
                                                                QChar('/'));
        auto tagIt = tagsByPath.find(lowerPath);
        if (tagIt == tagsByPath.end()) {
            // Bare-filename fallback (nested bundle paths).
            const QString base = lowerPath.mid(lowerPath.lastIndexOf('/') + 1);
            for (auto i = tagsByPath.begin(); i != tagsByPath.end(); ++i) {
                if (i.key().endsWith(QLatin1String("/") + base)) {
                    tagIt = i;
                    break;
                }
            }
        }
        if (tagIt != tagsByPath.end()) p.tags = *tagIt;
        // Mark referenced tips so unreferenced ones become loose tips.
        // (Re-derive from the preset XML: filename attribute.)
        {
            QImage icon;
            if (icon.loadFromData(kpp, "PNG")) {
                const QString x = icon.text(QStringLiteral("preset"));
                const QString fn = x.section(QLatin1String("filename=\""), 1,
                                             1)
                                       .section(QChar('"'), 0, 0);
                if (!fn.isEmpty())
                    tipReferenced.insert(
                        (QStringLiteral("brushes/") + fn).toLower(), true);
            }
        }
        result.presets.push_back(p);
        count++;
    }
    // Loose tips: brushes/* entries no preset referenced.
    for (const auto& e : entries) {
        const QString path =
            QString::fromUtf8(e.name.c_str()).replace(QChar('\\'), QChar('/'));
        if (!path.startsWith(QLatin1String("brushes/"), Qt::CaseInsensitive))
            continue;
        const QString lower = path.toLower();
        const QString ext = lower.section(QChar('.'), -1);
        if (ext != QLatin1String("png") && ext != QLatin1String("gbr") &&
            ext != QLatin1String("gih") && ext != QLatin1String("abr"))
            continue;
        if (tipReferenced.contains(lower)) continue;
        if (result.looseTips.size() >= 64) {
            result.skippedFiles++;
            continue;
        }
        const QByteArray bytes(reinterpret_cast<const char*>(e.data.data()),
                               int(e.data.size()));
        const QString base = path.mid(path.lastIndexOf('/') + 1);
        if (ext == QLatin1String("png")) {
            result.looseTips.push_back({base, bytes});
        } else {
            result.looseTips.push_back({base, QByteArray("RAW:") + bytes});
        }
    }
    return result;
}

}  // namespace pittore::ui::bundleimport
