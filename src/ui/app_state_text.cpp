// Live text layers: creation, re-render, hit testing and the Character-panel
// options. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/text/text_engine.h"
#include "ui/tools/options/tool_options.h"

#include <QFont>
#include <QFontMetricsF>

#include <algorithm>
#include <utility>

namespace pittore::ui {

int AppState::addTextLayer(const QPointF& origin, double wrapWidth,
                           const QString& family, double size, const QColor& color) {
    DocumentItem* d = activeDocument();
    if (!d) return -1;
    LayerItem layer;
    layer.kind = LayerItem::Kind::Pixel;
    layer.isText = true;
    layer.liveText = true;
    layer.textSpec.origin = origin;
    layer.textSpec.wrapWidth = wrapWidth > 0.5 ? wrapWidth : 0.0;
    layer.textSpec.family =
        family.isEmpty() ? QString::fromStdString(pittore::text::defaultFamily()) : family;
    layer.textSpec.size = size > 0.0 ? size : 48.0;
    layer.textSpec.color = color.isValid() ? color : QColor(Qt::black);
    // The Character panel's attributes seed the new run; family/size/colour
    // come from the caller (the Type tool options).
    layer.textSpec.align = characterDefaults_.align;
    layer.textSpec.lineHeight = characterDefaults_.lineHeight;
    layer.textSpec.tracking = characterDefaults_.tracking;
    layer.textSpec.underline = characterDefaults_.underline;
    layer.textSpec.strike = characterDefaults_.strike;
    layer.textSpec.underlineColor = characterDefaults_.underlineColor;
    layer.textSpec.strikeColor = characterDefaults_.strikeColor;
    layer.textSpec.backgroundColor = characterDefaults_.backgroundColor;
    layer.textSpec.baselineShift = characterDefaults_.baselineShift;
    layer.textSpec.hScale = characterDefaults_.hScale;
    layer.textSpec.vScale = characterDefaults_.vScale;
    layer.textSpec.superSub = characterDefaults_.superSub;
    layer.textSpec.allCaps = characterDefaults_.allCaps;
    layer.textSpec.kerning = characterDefaults_.kerning;
    layer.textSpec.otFeatures = characterDefaults_.otFeatures;
    // The Type options bar also owns style (synthetic weight/slant) and
    // alignment; the run starts from what it shows, keeping a Character-panel
    // edit made before typing consistent with the options bar.
    const ToolId typeTool = activeTool() == ToolId::VerticalType ? ToolId::VerticalType
                                                                 : ToolId::HorizontalType;
    const int style = option(typeTool, QStringLiteral("style")).toInt();
    layer.textSpec.bold = style >= 3;
    layer.textSpec.italic = style == 1 || style == 6;
    layer.textSpec.align = qBound(0, option(typeTool, QStringLiteral("align")).toInt(), 2);
    layer.name = tr("Text");
    layer.swatch = QColor(0x9a, 0x72, 0xd0);
    const int at = qBound(0, d->activeLayer, d->layers.size());
    d->layers.insert(at, std::move(layer));
    d->activeLayer = at;
    d->selectedLayers.clear();
    d->selectedLayers.push_back(at);
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit documentModified(d);
    return at;
}

bool AppState::refreshTextLayer(int index) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size()) return false;
    LayerItem& l = d->layers[index];
    if (!l.liveText) return false;
    const QRectF before = l.pixels ? layerBounds(*d, l) : QRectF();
    if (!renderLiveText(l)) {
        setStatusHint(tr("No font is available for this text."));
        return false;
    }
    const QString label = textLabel(l.textSpec.text);
    if (!label.isEmpty()) l.name = label;
    d->dirty = true;
    // Recompute just the union of the old and new ink so typing stays cheap;
    // the resampling halo covers the one-texel bleed of a non-identity layer
    // (text is always identity, so a small pad is enough).
    QRectF after = l.pixels ? layerBounds(*d, l) : QRectF();
    const QRect region = before.united(after)
                             .adjusted(-2, -2, 2, 2)
                             .toAlignedRect()
                             .intersected(QRect(QPoint(0, 0), d->size));
    if (!region.isEmpty()) {
        d->renderRegion(region);
        emit regionModified(d, QRectF(region));
    }
    emit layersChanged();
    return true;
}

bool AppState::setLiveTextSize(int index, double size, const QPointF& origin) {
    DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size()) return false;
    LayerItem& l = d->layers[index];
    if (!l.liveText || l.locked) return false;
    size = qBound(1.0, size, 1296.0);
    if (l.textSpec.size == size && l.textSpec.origin == origin) return true;
    l.textSpec.size = size;
    l.textSpec.origin = origin;
    // Re-render from the outlines at the new size: a transform-box drag never
    // resamples the old raster, so growing the text stays sharp.
    return refreshTextLayer(index);
}

namespace {
// Estimated ink box for live text with no baked pixels: font advances
// around the spec origin (top-left of the first line). Only a fallback for
// the hit test — baked bounds win whenever pixels exist.
QRectF liveTextEstimateBounds(const TextItem& t) {
    QFont font(t.family.isEmpty() ? QFont().family() : t.family);
    font.setPixelSize(qMax(1, qRound(t.size)));
    if (t.bold) font.setWeight(QFont::Bold);
    if (t.italic) font.setItalic(true);
    const QFontMetricsF fm(font);
    double w = 0.0;
    int lines = 0;
    for (const QString& line : t.text.split(u'\n')) {
        w = std::max(w, fm.horizontalAdvance(line));
        ++lines;
    }
    return QRectF(t.origin.x(), t.origin.y(), std::max(w, 1.0),
                  fm.height() * std::max(1, lines));
}
}  // namespace

int AppState::textLayerAt(const QPointF& docPos) const {
    const DocumentItem* d = activeDocument();
    if (!d) return -1;
    int estimate = -1;
    for (int i = 0; i < d->layers.size(); ++i) {
        const LayerItem& l = d->layers[i];
        if (!l.visible || !l.liveText) continue;
        if (l.pixels) {
            if (layerBounds(*d, l).contains(docPos)) return i;
            continue;
        }
        // Live text with content but no baked ink (whitespace-only runs,
        // fully transparent fills, or a render blocked on a missing font):
        // fall back to the font-metric box so the run stays double-clickable
        // instead of invisible to the hit test. Baked hits always win; the
        // first (topmost) estimate wins otherwise.
        if (estimate < 0 && !l.textSpec.text.isEmpty() &&
            liveTextEstimateBounds(l.textSpec).contains(docPos))
            estimate = i;
    }
    return estimate;
}

namespace {

// Apply one Character-panel option to a TextItem; returns true when the value
// actually changed. Shared by the live layer and the stored insertion defaults.
bool applyTextItemOption(TextItem& t, const QString& id, const QVariant& value) {
    if (id == QStringLiteral("family")) {
        const QString family = typeFamilyForIndex(value.toInt());
        if (family.isEmpty() || family == t.family) return false;
        t.family = family;
    } else if (id == QStringLiteral("style")) {
        // 0 regular, 1 italic, 2 medium, 3 semibold, 4 bold, 5 black,
        // 6 bold italic (the options bar's list).
        const int style = value.toInt();
        const bool bold = style >= 3;
        const bool italic = style == 1 || style == 6;
        if (bold == t.bold && italic == t.italic) return false;
        t.bold = bold;
        t.italic = italic;
    } else if (id == QStringLiteral("size")) {
        const double size = qBound(1.0, value.toDouble(), 1296.0);
        if (qFuzzyCompare(size, t.size)) return false;
        t.size = size;
    } else if (id == QStringLiteral("align")) {
        const int align = qBound(0, value.toInt(), 2);
        if (align == t.align) return false;
        t.align = align;
    } else if (id == QStringLiteral("color")) {
        const QColor color = value.value<QColor>();
        if (!color.isValid() || color == t.color) return false;
        t.color = color;
    } else if (id == QStringLiteral("tracking")) {
        const double v = qBound(-100.0, value.toDouble(), 200.0);
        if (qFuzzyCompare(v + 1.0, t.tracking + 1.0)) return false;
        t.tracking = v;
    } else if (id == QStringLiteral("leading")) {
        const double v = qBound(0.1, value.toDouble(), 10.0);
        if (qFuzzyCompare(v, t.lineHeight)) return false;
        t.lineHeight = v;
        // The panel's leading is a natural-step multiple; a recorded em
        // leading (imported text) would silently override it, so the edit
        // takes over by clearing it.
        t.emLeading = 0.0;
    } else if (id == QStringLiteral("underline") || id == QStringLiteral("strike")) {
        const int v = qBound(0, value.toInt(), 2);
        int& slot = id == QStringLiteral("underline") ? t.underline : t.strike;
        if (v == slot) return false;
        slot = v;
    } else if (id == QStringLiteral("underlineColor") ||
               id == QStringLiteral("strikeColor") ||
               id == QStringLiteral("backgroundColor")) {
        // An invalid colour means "inherit the fill" / "no highlight".
        const QColor c = value.value<QColor>();
        QColor& slot = id == QStringLiteral("underlineColor")   ? t.underlineColor
                       : id == QStringLiteral("strikeColor") ? t.strikeColor
                                                             : t.backgroundColor;
        if (c == slot) return false;
        slot = c;
    } else if (id == QStringLiteral("baselineShift")) {
        const double v = qBound(-4000.0, value.toDouble(), 4000.0);
        if (qFuzzyCompare(v + 1.0, t.baselineShift + 1.0)) return false;
        t.baselineShift = v;
    } else if (id == QStringLiteral("hScale") || id == QStringLiteral("vScale")) {
        const double v = qBound(1.0, value.toDouble(), 1000.0);
        double& slot = id == QStringLiteral("hScale") ? t.hScale : t.vScale;
        if (qFuzzyCompare(v, slot)) return false;
        slot = v;
    } else if (id == QStringLiteral("superSub")) {
        const int v = qBound(-1, value.toInt(), 1);
        if (v == t.superSub) return false;
        t.superSub = v;
    } else if (id == QStringLiteral("allCaps") || id == QStringLiteral("kerning")) {
        const bool v = value.toBool();
        bool& slot = id == QStringLiteral("allCaps") ? t.allCaps : t.kerning;
        if (v == slot) return false;
        slot = v;
    } else if (id.startsWith(QStringLiteral("ot:"))) {
        // "ot:<tag>" toggles one OpenType feature bit.
        const QString tag = id.mid(3).toLower();
        unsigned bit = 0;
        if (tag == QStringLiteral("liga")) bit = pittore::text::OTF_Liga;
        else if (tag == QStringLiteral("calt")) bit = pittore::text::OTF_Calt;
        else if (tag == QStringLiteral("smcp")) bit = pittore::text::OTF_Smcp;
        else if (tag == QStringLiteral("c2sc")) bit = pittore::text::OTF_C2sc;
        else if (tag == QStringLiteral("sups")) bit = pittore::text::OTF_Sups;
        else if (tag == QStringLiteral("subs")) bit = pittore::text::OTF_Subs;
        else if (tag == QStringLiteral("frac")) bit = pittore::text::OTF_Frac;
        else if (tag == QStringLiteral("ordn")) bit = pittore::text::OTF_Ordn;
        else if (tag == QStringLiteral("swsh")) bit = pittore::text::OTF_Swsh;
        else if (tag == QStringLiteral("ss01")) bit = pittore::text::OTF_Ss01;
        else if (tag == QStringLiteral("ss02")) bit = pittore::text::OTF_Ss02;
        else if (tag == QStringLiteral("ss03")) bit = pittore::text::OTF_Ss03;
        if (bit == 0) return false;
        const unsigned next =
            value.toBool() ? (t.otFeatures | bit) : (t.otFeatures & ~bit);
        if (next == t.otFeatures) return false;
        t.otFeatures = next;
    } else {
        return false;  // not a typographic option
    }
    return true;
}

}  // namespace

bool AppState::applyTextOption(const QString& id, const QVariant& value) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int index = d->activeLayer;
    if (index < 0 || index >= d->layers.size()) return false;
    if (!d->layers[index].liveText) return false;
    if (!applyTextItemOption(d->layers[index].textSpec, id, value)) return false;
    refreshTextLayer(index);
    return true;
}

bool AppState::applyCharacterOption(const QString& id, const QVariant& value) {
    DocumentItem* d = activeDocument();
    const int index = d ? d->activeLayer : -1;
    const bool live =
        d && index >= 0 && index < d->layers.size() && d->layers[index].liveText;
    if (live) {
        beginUndoStep();
        if (applyTextOption(id, value)) {
            applyTextItemOption(characterDefaults_, id, value);
            commitUndoStep(tr("Text"), QStringLiteral("type"));
            return true;
        }
        discardUndoStep();
        return false;
    }
    // No live layer: the values seed the next text. The options-bar fields go
    // through setOption so both surfaces stay in step; the rest live in the
    // stored defaults the panel reads back.
    if (id == QStringLiteral("family") || id == QStringLiteral("style") ||
        id == QStringLiteral("size") || id == QStringLiteral("align") ||
        id == QStringLiteral("color")) {
        const ToolId tool = activeTool() == ToolId::VerticalType ? ToolId::VerticalType
                                                                 : ToolId::HorizontalType;
        setOption(tool, id, value);
    } else {
        applyTextItemOption(characterDefaults_, id, value);
    }
    return true;
}

TextItem AppState::activeTextSpec() const {
    const DocumentItem* d = activeDocument();
    if (d) {
        const int index = d->activeLayer;
        if (index >= 0 && index < d->layers.size() && d->layers[index].liveText)
            return d->layers[index].textSpec;
    }
    TextItem t = characterDefaults_;
    const ToolId tool = activeTool() == ToolId::VerticalType ? ToolId::VerticalType
                                                             : ToolId::HorizontalType;
    const QString fam =
        typeFamilyForIndex(option(tool, QStringLiteral("family")).toInt());
    if (!fam.isEmpty()) t.family = fam;
    const int style = option(tool, QStringLiteral("style")).toInt();
    t.bold = style >= 3;
    t.italic = style == 1 || style == 6;
    const double size = option(tool, QStringLiteral("size")).toDouble();
    if (size > 0.0) t.size = size;
    t.align = qBound(0, option(tool, QStringLiteral("align")).toInt(), 2);
    const QColor c = option(tool, QStringLiteral("color")).value<QColor>();
    if (c.isValid()) t.color = c;
    return t;
}


pittore::text::TextSpec textSpecFor(const TextItem& t) {
    pittore::text::TextSpec spec;
    spec.text = t.text.toStdString();
    spec.family = t.family.toStdString();
    spec.bold = t.bold;
    spec.italic = t.italic;
    spec.size = static_cast<float>(std::max(1.0, t.size));
    spec.align = t.align == 1   ? pittore::text::Align::Center
                 : t.align == 2 ? pittore::text::Align::Right
                                : pittore::text::Align::Left;
    spec.lineHeight = static_cast<float>(std::max(0.1, t.lineHeight));
    spec.emLeading = static_cast<float>(std::max(0.0, t.emLeading));
    spec.paragraphGap = static_cast<float>(std::max(0.0, t.paragraphGap));
    spec.tracking = static_cast<float>(t.tracking);
    if (t.wrapWidth > 0.5) spec.wrapWidth = static_cast<float>(t.wrapWidth);
    spec.underline = qBound(0, t.underline, 2);
    spec.strike = qBound(0, t.strike, 2);
    auto rgba = [](const QColor& c) {
        return std::array<float, 4>{c.redF(), c.greenF(), c.blueF(), c.alphaF()};
    };
    spec.inkColor = rgba(t.color);
    if (t.underlineColor.isValid()) spec.underlineColor = rgba(t.underlineColor);
    if (t.strikeColor.isValid()) spec.strikeColor = rgba(t.strikeColor);
    if (t.backgroundColor.isValid()) spec.backgroundColor = rgba(t.backgroundColor);
    spec.baselineShift = static_cast<float>(t.baselineShift);
    spec.hScale = static_cast<float>(t.hScale > 0.0 ? t.hScale / 100.0 : 1.0);
    spec.vScale = static_cast<float>(t.vScale > 0.0 ? t.vScale / 100.0 : 1.0);
    spec.superSub = qBound(-1, t.superSub, 1);
    spec.allCaps = t.allCaps;
    spec.kerning = t.kerning;
    spec.otFeatures = t.otFeatures;
    return spec;
}

pittore::text::TextLayout textLayoutFor(const TextItem& t) {
    std::optional<pittore::text::TextLayout> laid =
        pittore::text::layoutText(textSpecFor(t));
    return laid ? *laid : pittore::text::TextLayout{};
}

QString typeFamilyForIndex(int index) {
    const std::vector<std::string>& families = typeFontFamilies();
    if (index < 0 || index >= static_cast<int>(families.size())) return QString();
    return QString::fromStdString(families[static_cast<std::size_t>(index)]);
}

int typeIndexForFamily(const QString& family) {
    const std::vector<std::string>& families = typeFontFamilies();
    const std::string want = family.toStdString();
    for (std::size_t i = 0; i < families.size(); ++i)
        if (families[i] == want) return static_cast<int>(i);
    return -1;
}

}  // namespace pittore::ui
