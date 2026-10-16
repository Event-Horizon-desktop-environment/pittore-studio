// Bridge implementation over engine/vector/svg_dom + clone.
#include "ui/svg_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include <QColor>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainterPath>
#include <QRectF>
#include <QSaveFile>

#include "engine/vector/svg_exchange.h"
#include "ui/app_state.h"

#include "engine/vector/clone.h"
#include "engine/vector/marker.h"
#include "engine/vector/pattern.h"
#include "engine/vector/svg_dom.h"
#include "engine/vector/svg/important.h"
#include "engine/vector/svg/specificity.h"
#include "ui/svg_parts.h"

namespace pittore::ui {
namespace {

void inlineStyles(vector::SvgDocument& doc) {
    if (!doc.root || doc.stylesheet.empty()) return;
    std::function<void(vector::SvgElement*)> walk = [&](vector::SvgElement* el) {
        static const char* kProps[] = {"fill",          "stroke",       "stroke-width",
                                       "opacity",       "fill-opacity", "stroke-opacity",
                                       "marker-start",  "marker-mid",   "marker-end",
                                       "clip-path",     "mask",         "filter",
                                       "font-size",     "font-family",  "font-weight",
                                       "stop-color",    "stop-opacity", "display",
                                       "visibility",    nullptr};
        for (int i = 0; kProps[i]; i++) {
            // Stylesheet winner per CSS cascade order: !important beats
            // normal, higher specificity beats lower, later beats earlier on
            // ties. Written back as an attribute so the downstream parser
            // (attribute-first) sees the cascade. Layering stays sheet >
            // style="" > attribute; only rule-vs-rule ordering is fixed here.
            const std::string prop = kProps[i];
            bool have = false;
            bool bestImp = false;
            int bestSpec = -1;
            size_t bestOrd = 0;
            std::string bestVal;
            size_t ord = 0;
            for (auto& rule : doc.stylesheet) {
                ++ord;
                if (!rule.matches(*el)) continue;
                const auto dit = rule.decls.find(prop);
                if (dit == rule.decls.end()) continue;
                const ::pittore::svg::DeclVal dv =
                    ::pittore::svg::splitImportant(dit->second);
                const int spec = ::pittore::svg::specificity(rule.selector);
                if (!have || (dv.important && !bestImp) ||
                    (dv.important == bestImp &&
                     (spec > bestSpec || (spec == bestSpec && ord >= bestOrd)))) {
                    have = true;
                    bestImp = dv.important;
                    bestSpec = spec;
                    bestOrd = ord;
                    bestVal = dv.value;
                }
            }
            if (have) el->set(kProps[i], bestVal);
        }
        for (auto& c : el->children) walk(c.get());
    };
    walk(doc.root.get());
}

// A DOM rebuild costs time and peak memory proportional to the document, so
// only bounded files take that path. Tag count is the right yardstick: it is
// what a rebuild actually allocates, and it lets a sparse-but-large map in
// while rejecting a flat geometry dump that happens to be small in bytes.
qsizetype countTags(const QByteArray& xml) {
    const char* p = xml.constData();
    qsizetype n = xml.size();
    qsizetype tags = 0;
    while (n > 0) {
        const void* hit = ::memchr(p, '<', (size_t)n);
        if (!hit) break;
        ++tags;
        const char* q = static_cast<const char*>(hit) + 1;
        n -= q - p;
        p = q;
    }
    return tags;
}

}  // namespace

QByteArray svgNormalizeForImport(const QByteArray& xml) {
    // Fast path: most SVGs (including multi-MB stress files with millions of
    // flat rects) contain no <use>/<symbol>/<style> to expand/inline. Skipping
    // the full DOM build + re-serialize avoids 2-3x peak memory and seconds
    // of work on such files; the streaming parser below handles flat
    // geometry directly.
    const bool maybeUse = xml.contains("<use") || xml.contains("<symbol");
    const bool maybeStyle = xml.contains("<style");
    if (!maybeUse && !maybeStyle) return xml;
    // Safety valves: never build a runaway DOM, and never clone an unbounded
    // number of <use> targets (each clone can itself contain more uses).
    constexpr qsizetype kNormalizeCapBytes = 64 * 1024 * 1024;
    constexpr qsizetype kMaxNormalizeTags = 512 * 1024;
    constexpr qsizetype kMaxUseRefs = 64 * 1024;
    if (xml.size() > kNormalizeCapBytes) return xml;
    if (countTags(xml) > kMaxNormalizeTags) return xml;
    if (xml.count("<use") > kMaxUseRefs) return xml;
    std::string in(xml.constData(), (size_t)xml.size());
    auto parsed = vector::parseSvgDom(in);
    if (!parsed.ok || !parsed.doc.root) return xml;
    inlineStyles(parsed.doc);
    vector::expandAllUses(parsed.doc);
    std::string out = vector::serializeSvgDom(parsed.doc);
    if (out.empty()) return xml;
    return QByteArray(out.data(), (qsizetype)out.size());
}

QByteArray svgEnsureBuiltinDefs(const QByteArray& xml) {
    // No std::string copy: contains scans the shared bytes in place. The old
    // version copied 100MB+ just to run two finds and then discarded it.
    const bool needMarker =
        xml.contains("mk-") && !xml.contains("<marker");
    const bool needPattern =
        xml.contains("pat-") && !xml.contains("<pattern");
    if (!needMarker && !needPattern) return xml;
    std::string s(xml.constData(), (size_t)xml.size());
    std::string defs;
    if (needMarker) defs += vector::builtinMarkersSvg();
    if (needPattern) defs += vector::builtinPatternsSvg();
    size_t pos = s.find('>');
    if (pos == std::string::npos) return xml;
    s.insert(pos + 1, defs);
    return QByteArray(s.data(), (qsizetype)s.size());
}

namespace {

bool parseHexColor(const QString& v, std::uint8_t out[4], bool& none) {
    QString s = v.trimmed();
    if (s.compare("none", Qt::CaseInsensitive) == 0) {
        none = true;
        return true;
    }
    const QColor c(s);
    if (!c.isValid()) return false;
    out[0] = (std::uint8_t)qBound(0, c.red(), 255);
    out[1] = (std::uint8_t)qBound(0, c.green(), 255);
    out[2] = (std::uint8_t)qBound(0, c.blue(), 255);
    out[3] = (std::uint8_t)qBound(0, c.alpha(), 255);
    return true;
}

QString urlId(const QString& v) {
    QString s = v.trimmed();
    if (s.startsWith("url(")) {
        int h = s.indexOf('#');
        int e = s.indexOf(')', h);
        if (h < 0) return {};
        return s.mid(h + 1, (e < 0 ? s.size() : e) - h - 1).trimmed();
    }
    if (s.startsWith('#')) return s.mid(1).trimmed();
    return s;
}

QString hexOf(const std::uint8_t c[4]) {
    char b[16];
    snprintf(b, sizeof(b), "#%02x%02x%02x", c[0], c[1], c[2]);
    QString s(b);
    if (c[3] != 255) s += QString(" alpha=%1").arg(c[3] / 255.0, 0, 'f', 2);
    return s;
}

// Local engine-image → QImage (flattened on white for JPEG XObjects), so the
// multipage exporter needs no export-dialog link dependency.
QImage flatImage(const pittore::Image& img) {
    const int w = (int)img.width(), h = (int)img.height();
    QImage out(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const pittore::RGBAf& px = img.at((std::uint32_t)x, (std::uint32_t)y);
            const float a = std::min(1.0f, std::max(0.0f, px.a));
            out.setPixel(x, y, qRgb((int)((px.r * a + (1 - a)) * 255),
                                    (int)((px.g * a + (1 - a)) * 255),
                                    (int)((px.b * a + (1 - a)) * 255)));
        }
    return out;
}

}  // namespace

bool applyXmlEditToArt(vector::ArtNode& node, const QString& attr,
                       const QString& value, QString* error) {
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        return false;
    };
    const QString a = attr.trimmed().toLower();
    if (a == "d") {
        const QPainterPath p = svgParsePathData(value);
        if (p.isEmpty()) return fail(QStringLiteral("d: no drawable path"));
        node.segments = svgPathSegments(p);
        return true;
    }
    if (a == "fill" || a == "stroke") {
        std::uint8_t c[4] = {0, 0, 0, 255};
        bool none = false;
        // Pattern fills arrive as url(#id).
        if (value.trimmed().startsWith("url(") && a == "fill") {
            node.paint.patternId = urlId(value).toStdString();
            node.paint.hasFill = true;
            return true;
        }
        if (!parseHexColor(value, c, none)) return fail(attr + ": bad color");
        if (a == "fill") {
            node.paint.hasFill = !none;
            if (!none) std::copy(c, c + 4, node.paint.fill);
        } else {
            node.paint.hasStroke = !none;
            if (!none) std::copy(c, c + 4, node.paint.stroke);
        }
        return true;
    }
    bool ok = false;
    const double num = value.trimmed().toDouble(&ok);
    if (a == "stroke-width") {
        if (!ok || !(num > 0)) return fail(QStringLiteral("stroke-width: positive number"));
        node.paint.strokeWidth = num;
        node.paint.hasStroke = true;
        return true;
    }
    if (a == "opacity") {
        if (!ok) return fail(QStringLiteral("opacity: 0..1"));
        node.opacity = qBound(0.0, num, 1.0);
        return true;
    }
    if (a == "fill-rule") {
        node.evenOdd = value.trimmed().compare("evenodd", Qt::CaseInsensitive) == 0;
        return true;
    }
    if (a == "marker-start" || a == "marker-mid" || a == "marker-end") {
        const std::string id = urlId(value).toStdString();
        if (a == "marker-start") node.paint.markerStart = id;
        if (a == "marker-mid") node.paint.markerMid = id;
        if (a == "marker-end") node.paint.markerEnd = id;
        return true;
    }
    if (a == "clip-path") {
        node.paint.clipId = urlId(value).toStdString();
        return true;
    }
    if (a == "mask") {
        node.paint.maskId = urlId(value).toStdString();
        return true;
    }
    if (a == "filter") {
        node.paint.filter.id = urlId(value).toStdString();
        node.paint.hasFilter = !node.paint.filter.id.empty() && !node.paint.filter.prims.empty();
        return true;
    }
    if (a == "id" || a == "name") {
        node.name = value.trimmed().toStdString();
        return true;
    }
    return true;  // unknown attributes never fail the edit
}

QString artNodeToXmlLine(const vector::ArtNode& node) {
    const auto& p = node.paint;
    QString s = QString("<path id=\"%1\"").arg(QString::fromStdString(node.name));
    if (p.hasFill && p.patternId.empty())
        s += QString(" fill=\"%1\"").arg(hexOf(p.fill));
    else if (!p.patternId.empty())
        s += QString(" fill=\"url(#%1)\"").arg(QString::fromStdString(p.patternId));
    else
        s += " fill=\"none\"";
    if (p.hasStroke)
        s += QString(" stroke=\"%1\" stroke-width=\"%2\"")
                 .arg(hexOf(p.stroke))
                 .arg(p.strokeWidth);
    if (!p.markerStart.empty())
        s += QString(" marker-start=\"url(#%1)\"")
                 .arg(QString::fromStdString(p.markerStart));
    if (!p.markerMid.empty())
        s += QString(" marker-mid=\"url(#%1)\"").arg(QString::fromStdString(p.markerMid));
    if (!p.markerEnd.empty())
        s += QString(" marker-end=\"url(#%1)\"").arg(QString::fromStdString(p.markerEnd));
    if (!p.clipId.empty())
        s += QString(" clip-path=\"url(#%1)\"").arg(QString::fromStdString(p.clipId));
    if (!p.maskId.empty())
        s += QString(" mask=\"url(#%1)\"").arg(QString::fromStdString(p.maskId));
    if (p.hasFilter)
        s += QString(" filter=\"url(#%1)\"").arg(QString::fromStdString(p.filter.id));
    if (p.hasMesh) s += " data-mesh=\"(retained)\"";
    s += QString(" opacity=\"%1\"").arg(node.opacity, 0, 'f', 3);
    s += QString(" segs=\"%1\"/>").arg(node.segments.size());
    return s;
}

QVector<QRectF> pageFrames(const DocumentItem& doc) {    QVector<QRectF> frames;
    for (const LayerItem& l : doc.layers) {
        if (!l.name.startsWith("Page", Qt::CaseInsensitive)) continue;
        if (!l.art || l.art->isEmpty()) continue;
        vector::Path flat = vector::flattenSegments(l.art->segments, 0.5f);
        double x0 = 1e100, y0 = 1e100, x1 = -1e100, y1 = -1e100;
        for (const auto& sp : flat.subpaths)
            for (const auto& pt : sp) {
                double wx = l.art->matrix[0] * pt.first + l.art->matrix[2] * pt.second +
                            l.art->matrix[4];
                double wy = l.art->matrix[1] * pt.first + l.art->matrix[3] * pt.second +
                            l.art->matrix[5];
                wx = l.offset.x() + wx * l.scaleX;
                wy = l.offset.y() + wy * l.scaleY;
                x0 = std::min(x0, wx);
                y0 = std::min(y0, wy);
                x1 = std::max(x1, wx);
                y1 = std::max(y1, wy);
            }
        if (x1 > x0 && y1 > y0) frames.push_back(QRectF(x0, y0, x1 - x0, y1 - y0));
    }
    if (frames.isEmpty()) frames.push_back(QRectF(QPointF(0, 0), doc.size));
    return frames;
}

bool exportPagesPdf(AppState* state, int docIndex, const QString& path,
                    QString* error) {
    if (!state) {
        if (error) *error = QStringLiteral("No document.");
        return false;
    }
    const QVector<DocumentItem*>& docs = state->documents();
    const DocumentItem* doc =
        (docIndex >= 0 && docIndex < docs.size()) ? docs[docIndex] : nullptr;
    if (!doc || doc->layers.isEmpty()) {
        if (error) *error = QStringLiteral("Nothing to export (empty document).");
        return false;
    }
    const QVector<QRectF> frames = pageFrames(*doc);
    std::vector<vector::PdfPage> pages;
    char buf[128];
    for (const QRectF& frame : frames) {
        vector::PdfPage pg;
        pg.wPt = frame.width() * 72.0 / 96.0;
        pg.hPt = frame.height() * 72.0 / 96.0;
        std::string content;
        for (const LayerItem& l : doc->layers) {
            if (!l.visible || !l.art || l.art->isEmpty()) continue;
            // Skip the frame layers themselves (named Page*).
            if (l.name.startsWith("Page", Qt::CaseInsensitive)) continue;
            vector::Path flat = vector::flattenSegments(l.art->segments, 0.5f);
            for (const auto& sp : flat.subpaths) {
                if (sp.size() < 2) continue;
                for (size_t i = 0; i < sp.size(); i++) {
                    double wx = l.art->matrix[0] * sp[i].first +
                                l.art->matrix[2] * sp[i].second + l.art->matrix[4];
                    double wy = l.art->matrix[1] * sp[i].first +
                                l.art->matrix[3] * sp[i].second + l.art->matrix[5];
                    wx = l.offset.x() + wx * l.scaleX - frame.x();
                    wy = l.offset.y() + wy * l.scaleY - frame.y();
                    // PDF origin is bottom-left: flip Y.
                    wy = frame.height() - wy;
                    snprintf(buf, sizeof(buf), "%.2f %.2f %c ", wx, wy,
                             i == 0 ? 'm' : 'l');
                    content += buf;
                }
                content += l.art->paint.hasFill ? "f " : "S ";
            }
        }
        // Placed rasters intersecting the frame ride as JPEG XObjects.
        int imIndex = 0;
        for (const LayerItem& l : doc->layers) {
            if (!l.visible) continue;
            const LayerDrawSource src = layerDrawSource(l);
            if (!src.img || src.img->width() == 0 || src.img->height() == 0) continue;
            QImage raster = flatImage(*src.img);
            if (raster.isNull()) continue;
            double pw = src.img->width() * src.scaleX, ph = src.img->height() * src.scaleY;
            QRectF placed(src.offset.x() - frame.x(), src.offset.y() - frame.y(), pw, ph);
            if (!placed.intersects(frame.translated(-frame.topLeft()))) continue;
            QByteArray jpeg;
            QBuffer qbuf(&jpeg);
            qbuf.open(QIODevice::WriteOnly);
            QImageWriter w(&qbuf, "JPEG");
            w.setQuality(90);
            if (!w.write(raster)) continue;
            vector::PdfImage im;
            im.x = placed.x();
            im.y = frame.height() - placed.y() - placed.height();
            im.w = placed.width();
            im.h = placed.height();
            im.iw = raster.width();
            im.ih = raster.height();
            im.jpeg.assign(jpeg.constData(), jpeg.constData() + jpeg.size());
            pg.images.push_back(std::move(im));
            (void)imIndex;
            imIndex++;
        }
        if (content.empty() && pg.images.empty()) content = "0 0 m ";
        pg.content = content;
        pages.push_back(std::move(pg));
    }
    // Internal links chain pages in order (1→2→…→1 table of contents).
    for (size_t i = 0; i < pages.size(); i++) {
        vector::PdfLink link;
        link.x0 = 0;
        link.y0 = 0;
        link.x1 = 40;
        link.y1 = 12;
        link.dest = std::to_string((i + 1) % pages.size());
        pages[i].links.push_back(link);
    }
    const std::vector<std::uint8_t> bytes =
        vector::writePdf(pages, "Pittore Studio");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), (qint64)bytes.size());
    if (!file.commit()) {
        if (error) *error = QStringLiteral("Could not save %1").arg(path);
        return false;
    }
    return true;
}

}  // namespace pittore::ui
