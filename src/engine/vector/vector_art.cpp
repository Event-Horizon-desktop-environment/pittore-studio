#include "engine/vector/vector_art.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/vector/vector_shape.h"

namespace pittore::vector {
namespace {

int gSvgDecimals = 4;   // set by artDocumentToSvg(doc, opt) per call

std::string num(double v) {
    const int decimals = std::clamp(gSvgDecimals, 0, 6);
    if (std::abs(v) < 0.5 * std::pow(10.0, -decimals)) v = 0.0;
    char fmt[16];
    std::snprintf(fmt, sizeof fmt, "%%.%df", decimals);
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s.empty() || s == "-0") s = "0";
    return s;
}

std::string hexColor(const std::uint8_t c[4]) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c[0], c[1], c[2]);
    return buf;
}

bool isIdentity(const double m[6]) {
    return m[0] == 1.0 && m[1] == 0.0 && m[2] == 0.0 && m[3] == 1.0 &&
           m[4] == 0.0 && m[5] == 0.0;
}

const char* capName(int cap) {
    switch (cap) {
        case 1: return "round";
        case 2: return "square";
        default: return nullptr;
    }
}

const char* joinName(int join) {
    switch (join) {
        case 1: return "round";
        case 2: return "bevel";
        default: return nullptr;
    }
}

std::string pathData(const std::vector<Segment>& segs) {
    std::string d;
    for (const Segment& s : segs) {
        switch (s.kind) {
            case Segment::Kind::MoveTo:
                d += "M" + num(s.x) + " " + num(s.y) + " ";
                break;
            case Segment::Kind::LineTo:
                d += "L" + num(s.x) + " " + num(s.y) + " ";
                break;
            case Segment::Kind::CubicTo:
                d += "C" + num(s.c1x) + " " + num(s.c1y) + " " + num(s.c2x) +
                     " " + num(s.c2y) + " " + num(s.x) + " " + num(s.y) + " ";
                break;
            case Segment::Kind::Close:
                d += "Z ";
                break;
        }
    }
    if (!d.empty() && d.back() == ' ') d.pop_back();
    return d;
}

void appendStop(std::string& out, const ArtStop& stop) {
    out += "<stop offset=\"" + num(stop.pos) + "\" stop-color=\"" +
           hexColor(stop.rgba) + "\"";
    if (stop.rgba[3] != 255)
        out += " stop-opacity=\"" + num(stop.rgba[3] / 255.0) + "\"";
    out += "/>\n";
}

// --- binary encode/decode -------------------------------------------------

struct ByteWriter {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(std::uint8_t(v >> (8 * i)));
    }
    void f32(float v) {
        std::uint32_t x;
        std::memcpy(&x, &v, 4);
        u32(x);
    }
    void f64(double v) {
        std::uint64_t x;
        std::memcpy(&x, &v, 8);
        for (int i = 0; i < 8; ++i) bytes.push_back(std::uint8_t(x >> (8 * i)));
    }
    void rgba(const std::uint8_t c[4]) {
        bytes.insert(bytes.end(), c, c + 4);
    }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};

struct ByteReader {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    std::size_t i = 0;
    bool ok = true;

    bool need(std::size_t k) {
        if (i + k > n || i + k < i) {
            ok = false;
            return false;
        }
        return true;
    }
    std::uint8_t u8() { return need(1) ? p[i++] : 0; }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= std::uint32_t(p[i + k]) << (8 * k);
        i += 4;
        return v;
    }
    float f32() {
        const std::uint32_t x = u32();
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }
    double f64() {
        if (!need(8)) return 0.0;
        std::uint64_t x = 0;
        for (int k = 0; k < 8; ++k) x |= std::uint64_t(p[i + k]) << (8 * k);
        i += 8;
        double d;
        std::memcpy(&d, &x, 8);
        return d;
    }
    void rgba(std::uint8_t c[4]) {
        if (!need(4)) return;
        for (int k = 0; k < 4; ++k) c[k] = p[i++];
    }
    std::string str() {
        const std::uint32_t len = u32();
        if (!ok || len > n) {
            ok = false;
            return {};
        }
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
};

}  // namespace

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    if (!data || size == 0) return out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) |
                                (std::uint32_t(data[i + 1]) << 8) |
                                std::uint32_t(data[i + 2]);
        out += kTable[(v >> 18) & 0x3f];
        out += kTable[(v >> 12) & 0x3f];
        out += kTable[(v >> 6) & 0x3f];
        out += kTable[v & 0x3f];
    }
    const std::size_t rem = size - i;
    if (rem == 1) {
        const std::uint32_t v = std::uint32_t(data[i]) << 16;
        out += kTable[(v >> 18) & 0x3f];
        out += kTable[(v >> 12) & 0x3f];
        out += "==";
    } else if (rem == 2) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) |
                                (std::uint32_t(data[i + 1]) << 8);
        out += kTable[(v >> 18) & 0x3f];
        out += kTable[(v >> 12) & 0x3f];
        out += kTable[(v >> 6) & 0x3f];
        out += '=';
    }
    return out;
}

std::string artDocumentToSvg(const ArtDocument& doc) {
    return artDocumentToSvg(doc, ArtSvgOptions{});
}

std::string artDocumentToSvg(const ArtDocument& doc, const ArtSvgOptions& opt) {
    if (doc.width <= 0.0 || doc.height <= 0.0) return {};
    gSvgDecimals = std::clamp(opt.decimals, 0, 6);
    const std::string nl = opt.lineBreaks ? "\n" : "";

    double vx = doc.viewX, vy = doc.viewY, vw = doc.viewW, vh = doc.viewH;
    if (vw <= 0.0 || vh <= 0.0) {
        vx = 0.0;
        vy = 0.0;
        vw = doc.width;
        vh = doc.height;
    }

    // Gradients live in <defs>; each gradient-bearing element references one.
    // Fill and stroke servers get separate ids: one shape may carry both.
    std::string defs;
    std::vector<int> gradIndex(doc.elements.size(), -1);
    std::vector<int> strokeGradIndex(doc.elements.size(), -1);
    int gradientCount = 0;
    auto emitGradient = [&](const ArtGradient& g) -> int {
        const int index = gradientCount++;
        const std::string id = "grad" + std::to_string(index);
        if (g.radial) {
            defs += "<radialGradient id=\"" + id +
                    "\" gradientUnits=\"userSpaceOnUse\" cx=\"" + num(g.cx) +
                    "\" cy=\"" + num(g.cy) + "\" r=\"" + num(g.r) + "\">\n";
        } else {
            defs += "<linearGradient id=\"" + id +
                    "\" gradientUnits=\"userSpaceOnUse\" x1=\"" + num(g.x1) +
                    "\" y1=\"" + num(g.y1) + "\" x2=\"" + num(g.x2) +
                    "\" y2=\"" + num(g.y2) + "\">\n";
        }
        for (const ArtStop& stop : g.stops) appendStop(defs, stop);
        defs += g.radial ? "</radialGradient>\n" : "</linearGradient>\n";
        return index;
    };
    for (std::size_t i = 0; i < doc.elements.size(); ++i) {
        const ArtElement& el = doc.elements[i];
        if (el.raster) continue;
        const ArtPaint& paint = el.node.paint;
        if (paint.hasFill && paint.hasGradient &&
            !paint.gradient.stops.empty())
            gradIndex[i] = emitGradient(paint.gradient);
        if (paint.hasStroke && paint.hasStrokeGradient &&
            !paint.strokeGradient.stops.empty())
            strokeGradIndex[i] = emitGradient(paint.strokeGradient);
    }
    // Mesh + filter defs ride the same <defs> block (ids referenced above).
    for (std::size_t i = 0; i < doc.elements.size(); ++i) {
        const ArtElement& el = doc.elements[i];
        if (el.raster) continue;
        const ArtPaint& paint = el.node.paint;
        if (paint.hasMesh && !paint.mesh.patches.empty()) {
            std::array<std::uint8_t, 4> fb{paint.fill[0], paint.fill[1], paint.fill[2],
                                            paint.fill[3]};
            defs += meshToSvg(paint.mesh, "mesh" + std::to_string(i), fb) + "\n";
        }
        if (paint.hasFilter && !paint.filter.prims.empty())
            defs += filterGraphToSvg(paint.filter) + "\n";
    }

    std::string svg;
    svg += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + nl;
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
    svg += " width=\"" + num(doc.width) + "\" height=\"" + num(doc.height) +
           "\"";
    if (opt.setViewbox) {
        svg += " viewBox=\"" + num(vx) + " " + num(vy) + " " + num(vw) + " " +
               num(vh) + "\"";
    }
    svg += ">" + nl;
    if (!defs.empty()) svg += "<defs>" + nl + defs + "</defs>" + nl;

    for (std::size_t i = 0; i < doc.elements.size(); ++i) {
        const ArtElement& el = doc.elements[i];
        const std::string styleAttr =
            el.blendCss.empty() ? std::string()
                                : " style=\"mix-blend-mode:" + el.blendCss + "\"";
        if (el.raster) {
            const ArtRaster& r = el.image;
            if (r.base64Png.empty()) continue;
            const std::string mime =
                r.mime.empty() ? "image/png" : r.mime;
            const std::string href =
                "data:" + mime + ";base64," + r.base64Png;
            svg += "<image x=\"" + num(r.x) + "\" y=\"" + num(r.y) +
                   "\" width=\"" + num(r.w) + "\" height=\"" + num(r.h) +
                   "\" preserveAspectRatio=\"none\"";
            if (r.opacity < 1.0)
                svg += " opacity=\"" + num(r.opacity) + "\"";
            svg += styleAttr;
            svg += " href=\"" + href + "\" xlink:href=\"" + href + "\"/>\n";
            continue;
        }

        const ArtNode& n = el.node;
        if (n.segments.empty()) continue;
        const ArtPaint& paint = n.paint;
        // Variable-width strokes have no SVG stroke equivalent: the fill
        // below draws from the centerline as usual, and the expanded
        // outline is emitted afterwards as a second filled element.
        const std::vector<Segment> profSegs =
            (paint.hasStroke && paint.hasProfile && !paint.profile.empty())
                ? profiledStrokeSegments(n)
                : std::vector<Segment>();
        const bool bakedProfile = !profSegs.empty();
        svg += "<path d=\"" + pathData(n.segments) + "\"";

        const bool gradient = paint.hasFill && paint.hasGradient &&
                              gradIndex[i] >= 0;
        if (!paint.patternId.empty()) {
            svg += " fill=\"url(#" + paint.patternId + ")\"";
            if (paint.hasPatternXform) {
                svg += " patternTransform=\"matrix(" + num(paint.patternXform[0]) + "," +
                       num(paint.patternXform[1]) + "," + num(paint.patternXform[2]) + "," +
                       num(paint.patternXform[3]) + "," + num(paint.patternXform[4]) + "," +
                       num(paint.patternXform[5]) + ")\"";
            }
        } else if (paint.hasMesh) {
            // Mesh proposal element + flat fallback so strict viewers agree.
            char fb[8];
            snprintf(fb, sizeof(fb), "#%02x%02x%02x", paint.fill[0], paint.fill[1],
                     paint.fill[2]);
            svg += " fill=\"" + std::string(fb) + "\"";
        } else if (gradient) {
            svg += " fill=\"url(#grad" + std::to_string(gradIndex[i]) + ")\"";
        } else if (paint.hasFill) {
            svg += " fill=\"" + hexColor(paint.fill) + "\"";
            if (paint.fill[3] != 255)
                svg += " fill-opacity=\"" + num(paint.fill[3] / 255.0) + "\"";
        } else {
            svg += " fill=\"none\"";
        }
        if (n.evenOdd && !bakedProfile) svg += " fill-rule=\"evenodd\"";

        if (paint.hasStroke && paint.strokeWidth > 0.0 && !bakedProfile) {
            if (strokeGradIndex[i] >= 0) {
                svg += " stroke=\"url(#grad" + std::to_string(strokeGradIndex[i]) +
                       ")\"";
            } else {
                svg += " stroke=\"" + hexColor(paint.stroke) + "\"";
                if (paint.stroke[3] != 255)
                    svg += " stroke-opacity=\"" + num(paint.stroke[3] / 255.0) +
                           "\"";
            }
            svg += " stroke-width=\"" + num(paint.strokeWidth) + "\"";
            if (const char* c = capName(paint.cap))
                svg += std::string(" stroke-linecap=\"") + c + "\"";
            if (const char* j = joinName(paint.join))
                svg += std::string(" stroke-linejoin=\"") + j + "\"";
            if (paint.hasDash && !paint.dash.empty()) {
                // Model units are stroke-width multiples (QPen parity);
                // SVG dasharray is user-space, so scale by the width.
                svg += " stroke-dasharray=\"";
                for (std::size_t k = 0; k < paint.dash.size(); ++k) {
                    if (k) svg += " ";
                    svg += num(paint.dash[k] * paint.strokeWidth);
                }
                svg += "\"";
                if (paint.dashOffset != 0.0f)
                    svg += " stroke-dashoffset=\"" +
                           num(paint.dashOffset * paint.strokeWidth) + "\"";
            }
        }

        if (!isIdentity(n.matrix)) {
            svg += " transform=\"matrix(" + num(n.matrix[0]) + "," +
                   num(n.matrix[1]) + "," + num(n.matrix[2]) + "," +
                   num(n.matrix[3]) + "," + num(n.matrix[4]) + "," +
                   num(n.matrix[5]) + ")\"";
        }
        if (!paint.markerStart.empty())
            svg += " marker-start=\"url(#" + paint.markerStart + ")\"";
        if (!paint.markerMid.empty())
            svg += " marker-mid=\"url(#" + paint.markerMid + ")\"";
        if (!paint.markerEnd.empty())
            svg += " marker-end=\"url(#" + paint.markerEnd + ")\"";
        if (!paint.clipId.empty())
            svg += " clip-path=\"url(#" + paint.clipId + ")\"";
        if (!paint.maskId.empty())
            svg += " mask=\"url(#" + paint.maskId + ")\"";
        if (paint.hasFilter)
            svg += " filter=\"url(#" + paint.filter.id + ")\"";
        if (paint.hasMesh && !paint.mesh.patches.empty())
            svg += " data-mesh=\"mesh" + std::to_string(i) + "\"";
        if (n.opacity < 1.0)
            svg += " opacity=\"" + num(n.opacity) + "\"";
        svg += styleAttr;
        svg += "/>\n";
        if (bakedProfile) {
            // The stroke outline as its own filled element (same placement).
            svg += "<path d=\"" + pathData(profSegs) + "\"";
            svg += " fill=\"" + hexColor(paint.stroke) + "\"";
            if (paint.stroke[3] != 255)
                svg += " fill-opacity=\"" + num(paint.stroke[3] / 255.0) + "\"";
            if (!isIdentity(n.matrix)) {
                svg += " transform=\"matrix(" + num(n.matrix[0]) + "," +
                       num(n.matrix[1]) + "," + num(n.matrix[2]) + "," +
                       num(n.matrix[3]) + "," + num(n.matrix[4]) + "," +
                       num(n.matrix[5]) + ")\"";
            }
            if (n.opacity < 1.0)
                svg += " opacity=\"" + num(n.opacity) + "\"";
            svg += styleAttr;
            svg += "/>\n";
        }
    }

    svg += "</svg>\n";
    gSvgDecimals = 4;
    if (!opt.lineBreaks)
        svg.erase(std::remove(svg.begin(), svg.end(), '\n'), svg.end());
    return svg;
}

std::vector<Segment> profiledStrokeSegments(const ArtNode& node) {
    std::vector<Segment> out;
    const ArtPaint& paint = node.paint;
    if (!paint.hasStroke || !paint.hasProfile || paint.profile.empty() ||
        !(paint.strokeWidth > 0.0))
        return out;
    const Path flat = flattenSegments(node.segments, 0.25f);
    if (flat.subpaths.empty()) return out;
    WidthProfile prof;
    for (const ArtWidthPoint& p : paint.profile) {
        prof.pos.push_back(p.t);
        prof.scale.push_back(p.w);
    }
    // Per-point absolute widths at arclength fractions (per subpath).
    std::vector<std::vector<float>> widths;
    widths.reserve(flat.subpaths.size());
    for (std::size_t i = 0; i < flat.subpaths.size(); ++i) {
        const auto& sub = flat.subpaths[i];
        std::vector<float> cum(sub.size(), 0.0f);
        for (std::size_t k = 1; k < sub.size(); ++k)
            cum[k] = cum[k - 1] + std::hypot(sub[k].first - sub[k - 1].first,
                                             sub[k].second - sub[k - 1].second);
        float total = cum.back();
        if (flat.isClosed(i) && sub.size() >= 2)
            total += std::hypot(sub.front().first - sub.back().first,
                                sub.front().second - sub.back().second);
        std::vector<float> ws;
        ws.reserve(sub.size());
        for (std::size_t k = 0; k < sub.size(); ++k) {
            const float t = (total > 1e-9f) ? cum[k] / total : 0.0f;
            ws.push_back(std::max(
                0.0f, static_cast<float>(paint.strokeWidth) * widthProfileAt(prof, t)));
        }
        widths.push_back(std::move(ws));
    }
    StrokeStyle style(paint.strokeWidth);
    style.cap = paint.cap == 1 ? LineCap::Round
                : paint.cap == 2 ? LineCap::Square
                                 : LineCap::Butt;
    style.join = paint.join == 1 ? LineJoin::Round
                 : paint.join == 2 ? LineJoin::Bevel
                                   : LineJoin::Miter;
    std::vector<float> dashAbs;
    for (float v : paint.dash) dashAbs.push_back(v * paint.strokeWidth);
    const Path expanded = strokeVariable(
        flat, widths, style,
        paint.hasDash ? dashAbs : std::vector<float>(),
        paint.hasDash ? paint.dashOffset * paint.strokeWidth : 0.0f);
    for (const auto& piece : expanded.subpaths) {
        if (piece.empty()) continue;
        Segment m;
        m.kind = Segment::Kind::MoveTo;
        m.x = piece.front().first;
        m.y = piece.front().second;
        out.push_back(m);
        for (std::size_t k = 1; k < piece.size(); ++k) {
            Segment l;
            l.kind = Segment::Kind::LineTo;
            l.x = piece[k].first;
            l.y = piece[k].second;
            out.push_back(l);
        }
        Segment c;
        c.kind = Segment::Kind::Close;
        out.push_back(c);
    }
    return out;
}

std::shared_ptr<ArtNode> artNodeFromShape(const VectorShape& shape,                                          const GradientFill* gradient,
                                          double tx, double ty) {
    if (shape.path.isEmpty()) return {};

    auto node = std::make_shared<ArtNode>();
    node->name = shape.path.name;

    // Walk each subpath exactly as flattenPath() does, but keep the cubics
    // instead of flattening them, so the exported path stays smooth.
    for (const SubPath& sub : shape.path.subpaths) {
        if (sub.anchors.empty()) continue;
        const Anchor& first = sub.anchors.front();
        Segment mv;
        mv.kind = Segment::Kind::MoveTo;
        mv.x = first.px;
        mv.y = first.py;
        node->segments.push_back(mv);
        const std::size_t n = sub.anchors.size();
        for (std::size_t i = 1; i <= n; ++i) {
            const Anchor& from = sub.anchors[i - 1];
            const Anchor& to = sub.anchors[i % n];
            if (i == n && !sub.closed) break;
            Segment c;
            c.kind = Segment::Kind::CubicTo;
            c.c1x = from.px + from.hox;
            c.c1y = from.py + from.hoy;
            c.c2x = to.px + to.hix;
            c.c2y = to.py + to.hiy;
            c.x = to.px;
            c.y = to.py;
            node->segments.push_back(c);
        }
        if (sub.closed) {
            Segment cl;
            cl.kind = Segment::Kind::Close;
            node->segments.push_back(cl);
        }
    }

    node->matrix[0] = 1.0;
    node->matrix[1] = 0.0;
    node->matrix[2] = 0.0;
    node->matrix[3] = 1.0;
    node->matrix[4] = tx;
    node->matrix[5] = ty;
    node->evenOdd = shape.evenOdd;

    const bool hasGradient = gradient && !gradient->stops.empty();
    node->paint.hasFill = shape.fill[3] > 0 || hasGradient;
    std::copy(shape.fill.begin(), shape.fill.end(), node->paint.fill);
    node->paint.hasStroke = shape.hasStroke && shape.stroke[3] > 0;
    std::copy(shape.stroke.begin(), shape.stroke.end(), node->paint.stroke);
    node->paint.strokeWidth = shape.strokeWidth;
    node->paint.cap = 1;   // rasterizeShape() strokes with round caps...
    node->paint.join = 1;  // ...and round joins.
    if (hasGradient) {
        node->paint.hasGradient = true;
        ArtGradient& g = node->paint.gradient;
        g.radial = gradient->radial;
        g.stops.reserve(gradient->stops.size());
        for (const GradientStop& s : gradient->stops)
            g.stops.push_back(
                ArtStop{s.pos, {s.color[0], s.color[1], s.color[2], s.color[3]}});
        if (gradient->radial) {
            // rasterizeShape(): t = |p - start| / |end - start|, so the centre
            // is `start` and the radius is the start→end distance.
            g.cx = gradient->startX;
            g.cy = gradient->startY;
            const double dx = gradient->endX - gradient->startX;
            const double dy = gradient->endY - gradient->startY;
            g.r = std::sqrt(dx * dx + dy * dy);
        } else {
            g.x1 = gradient->startX;
            g.y1 = gradient->startY;
            g.x2 = gradient->endX;
            g.y2 = gradient->endY;
        }
    }
    return node;
}

std::vector<std::uint8_t> encodeArtNode(const ArtNode& node) {
    ByteWriter w;
    w.str(node.name);
    std::uint8_t flags = 0;
    if (node.evenOdd) flags |= 1u << 0;
    if (node.paint.hasFill) flags |= 1u << 1;
    if (node.paint.hasGradient) flags |= 1u << 2;
    if (node.paint.hasStroke) flags |= 1u << 3;
    if (node.paint.gradient.radial) flags |= 1u << 4;
    w.u8(flags);
    w.f64(node.opacity);
    for (double m : node.matrix) w.f64(m);
    w.rgba(node.paint.fill);
    w.rgba(node.paint.stroke);
    w.f64(node.paint.strokeWidth);
    w.u8(static_cast<std::uint8_t>(node.paint.cap));
    w.u8(static_cast<std::uint8_t>(node.paint.join));

    if (node.paint.hasGradient) {
        const ArtGradient& g = node.paint.gradient;
        w.f64(g.x1);
        w.f64(g.y1);
        w.f64(g.x2);
        w.f64(g.y2);
        w.f64(g.cx);
        w.f64(g.cy);
        w.f64(g.r);
        w.u32(static_cast<std::uint32_t>(g.stops.size()));
        for (const ArtStop& s : g.stops) {
            w.f32(s.pos);
            w.rgba(s.rgba);
        }
    }

    w.u32(static_cast<std::uint32_t>(node.segments.size()));
    for (const Segment& s : node.segments) {
        w.u8(static_cast<std::uint8_t>(s.kind));
        switch (s.kind) {
            case Segment::Kind::MoveTo:
            case Segment::Kind::LineTo:
                w.f32(s.x);
                w.f32(s.y);
                break;
            case Segment::Kind::CubicTo:
                w.f32(s.c1x);
                w.f32(s.c1y);
                w.f32(s.c2x);
                w.f32(s.c2y);
                w.f32(s.x);
                w.f32(s.y);
                break;
            case Segment::Kind::Close:
                break;
        }
    }
    // Tail block: pre-dash files simply end above (forward compatible — old
    // decoders stop at `consumed`; new decoders default hasDash=false when
    // the tail is absent).
    w.u8(node.paint.hasDash ? 1u : 0u);
    if (node.paint.hasDash) {
        w.u32(static_cast<std::uint32_t>(node.paint.dash.size()));
        for (float v : node.paint.dash) w.f32(v);
        w.f32(node.paint.dashOffset);
    }
    // Profile tail: same forward-compatible convention as the dash tail.
    w.u8(node.paint.hasProfile ? 1u : 0u);
    if (node.paint.hasProfile) {
        w.u32(static_cast<std::uint32_t>(node.paint.profile.size()));
        for (const ArtWidthPoint& p : node.paint.profile) {
            w.f32(p.t);
            w.f32(p.w);
        }
    }
    // Object-refs tail: pattern/marker/clip/mask ids, mesh, filter graph.
    w.str(node.paint.patternId);
    w.str(node.paint.markerStart);
    w.str(node.paint.markerMid);
    w.str(node.paint.markerEnd);
    w.str(node.paint.clipId);
    w.str(node.paint.maskId);
    w.u8(node.paint.hasMesh ? 1u : 0u);
    if (node.paint.hasMesh) {
        const MeshGradient& m = node.paint.mesh;
        w.u32((std::uint32_t)std::max(0, m.rows));
        w.u32((std::uint32_t)std::max(0, m.cols));
        w.u8(m.isConical ? 1u : 0u);
        w.f64(m.conicalCx);
        w.f64(m.conicalCy);
        w.u32((std::uint32_t)m.patches.size());
        for (const MeshPatch& p : m.patches) {
            for (auto pt : p.p) {
                w.f64(pt[0]);
                w.f64(pt[1]);
            }
            for (auto c : p.c) w.rgba(c.data());
        }
    }
    w.u8(node.paint.hasFilter ? 1u : 0u);
    if (node.paint.hasFilter) {
        const FilterGraph& g = node.paint.filter;
        w.str(g.id);
        w.u32((std::uint32_t)g.prims.size());
        for (const FePrimitive& p : g.prims) {
            w.str(p.type);
            w.str(p.result);
            w.str(p.in);
            w.str(p.in2);
            w.u32((std::uint32_t)p.attrs.size());
            for (const auto& [k, v] : p.attrs) {
                w.str(k);
                w.str(v);
            }
        }
    }
    // Pattern-transform tail (see decode): end-appended for compat.
    w.u8(node.paint.hasPatternXform ? 1u : 0u);
    if (node.paint.hasPatternXform)
        for (double m : node.paint.patternXform) w.f64(m);
    // Stroke-gradient tail (see decode): same forward-compatible convention,
    // appended after the pattern-transform tail so older decoders, which stop
    // there, never see the extra bytes as part of a block they expect.
    w.u8(node.paint.hasStrokeGradient ? 1u : 0u);
    if (node.paint.hasStrokeGradient) {
        const ArtGradient& g = node.paint.strokeGradient;
        w.u8(g.radial ? 1u : 0u);
        w.f64(g.x1);
        w.f64(g.y1);
        w.f64(g.x2);
        w.f64(g.y2);
        w.f64(g.cx);
        w.f64(g.cy);
        w.f64(g.r);
        w.u32(static_cast<std::uint32_t>(g.stops.size()));
        for (const ArtStop& s : g.stops) {
            w.f32(s.pos);
            w.rgba(s.rgba);
        }
    }
    return std::move(w.bytes);
}

std::optional<ArtNode> decodeArtNode(const std::vector<std::uint8_t>& bytes,
                                     std::size_t* consumed) {
    ByteReader r{bytes.data(), bytes.size(), 0, true};
    ArtNode node;
    node.name = r.str();
    const std::uint8_t flags = r.u8();
    node.evenOdd = (flags & (1u << 0)) != 0;
    node.paint.hasFill = (flags & (1u << 1)) != 0;
    node.paint.hasGradient = (flags & (1u << 2)) != 0;
    node.paint.hasStroke = (flags & (1u << 3)) != 0;
    node.paint.gradient.radial = (flags & (1u << 4)) != 0;
    node.opacity = r.f64();
    for (double& m : node.matrix) m = r.f64();
    r.rgba(node.paint.fill);
    r.rgba(node.paint.stroke);
    node.paint.strokeWidth = r.f64();
    node.paint.cap = r.u8();
    node.paint.join = r.u8();

    if (node.paint.hasGradient) {
        ArtGradient& g = node.paint.gradient;
        g.x1 = r.f64();
        g.y1 = r.f64();
        g.x2 = r.f64();
        g.y2 = r.f64();
        g.cx = r.f64();
        g.cy = r.f64();
        g.r = r.f64();
        const std::uint32_t stops = r.u32();
        if (!r.ok || stops > bytes.size()) return std::nullopt;
        g.stops.resize(stops);
        for (ArtStop& s : g.stops) {
            s.pos = r.f32();
            r.rgba(s.rgba);
        }
    }

    const std::uint32_t count = r.u32();
    if (!r.ok || count > bytes.size()) return std::nullopt;
    node.segments.resize(count);
    for (Segment& s : node.segments) {
        const std::uint8_t kind = r.u8();
        if (kind > 3) return std::nullopt;
        s.kind = static_cast<Segment::Kind>(kind);
        switch (s.kind) {
            case Segment::Kind::MoveTo:
            case Segment::Kind::LineTo:
                s.x = r.f32();
                s.y = r.f32();
                break;
            case Segment::Kind::CubicTo:
                s.c1x = r.f32();
                s.c1y = r.f32();
                s.c2x = r.f32();
                s.c2y = r.f32();
                s.x = r.f32();
                s.y = r.f32();
                break;
            case Segment::Kind::Close:
                break;
        }
    }
    if (!r.ok) return std::nullopt;
    // Dash tail (see encode): absent in pre-dash files → solid stroke.
    node.paint.hasDash = false;
    node.paint.dash.clear();
    node.paint.dashOffset = 0.0f;
    if (r.i < bytes.size()) {
        node.paint.hasDash = r.u8() != 0;
        if (r.ok && node.paint.hasDash) {
            const std::uint32_t dn = r.u32();
            if (!r.ok || dn > bytes.size()) return std::nullopt;
            node.paint.dash.resize(dn);
            for (float& v : node.paint.dash) v = r.f32();
            node.paint.dashOffset = r.f32();
        }
        if (!r.ok) return std::nullopt;
    }
    // Profile tail: absent in older files → uniform stroke.
    node.paint.hasProfile = false;
    node.paint.profile.clear();
    if (r.i < bytes.size()) {
        node.paint.hasProfile = r.u8() != 0;
        if (r.ok && node.paint.hasProfile) {
            const std::uint32_t pn = r.u32();
            if (!r.ok || pn > bytes.size()) return std::nullopt;
            node.paint.profile.resize(pn);
            for (ArtWidthPoint& p : node.paint.profile) {
                p.t = r.f32();
                p.w = r.f32();
            }
        }
        if (!r.ok) return std::nullopt;
    }
    // Object-refs tail: absent in older files → no refs.
    node.paint.patternId.clear();
    node.paint.markerStart.clear();
    node.paint.markerMid.clear();
    node.paint.markerEnd.clear();
    node.paint.clipId.clear();
    node.paint.maskId.clear();
    node.paint.hasMesh = false;
    node.paint.hasFilter = false;
    if (r.i < bytes.size()) {
        node.paint.patternId = r.str();
        node.paint.markerStart = r.str();
        node.paint.markerMid = r.str();
        node.paint.markerEnd = r.str();
        node.paint.clipId = r.str();
        node.paint.maskId = r.str();
        if (!r.ok) return std::nullopt;
        node.paint.hasMesh = r.u8() != 0;
        if (r.ok && node.paint.hasMesh) {
            MeshGradient m;
            m.rows = (int)r.u32();
            m.cols = (int)r.u32();
            m.isConical = r.u8() != 0;
            m.conicalCx = r.f64();
            m.conicalCy = r.f64();
            const std::uint32_t np = r.u32();
            if (!r.ok || np > bytes.size()) return std::nullopt;
            m.patches.resize(np);
            for (MeshPatch& p : m.patches) {
                for (auto& pt : p.p) {
                    pt[0] = r.f64();
                    pt[1] = r.f64();
                }
                for (auto& c : p.c) r.rgba(c.data());
            }
            if (r.ok) {
                node.paint.mesh = std::move(m);
            } else {
                node.paint.hasMesh = false;
            }
        }
        if (!r.ok) return std::nullopt;
        node.paint.hasFilter = r.u8() != 0;
        if (r.ok && node.paint.hasFilter) {
            FilterGraph g;
            g.id = r.str();
            const std::uint32_t nf = r.u32();
            if (!r.ok || nf > bytes.size()) return std::nullopt;
            g.prims.resize(nf);
            for (FePrimitive& p : g.prims) {
                p.type = r.str();
                p.result = r.str();
                p.in = r.str();
                p.in2 = r.str();
                const std::uint32_t na = r.u32();
                if (!r.ok || na > bytes.size()) return std::nullopt;
                for (std::uint32_t k = 0; k < na; k++) {
                    std::string key = r.str(), val = r.str();
                    if (r.ok) p.attrs[key] = val;
                }
            }
            if (r.ok) node.paint.filter = std::move(g);
            if (!r.ok) return std::nullopt;
        }
    }
    // Pattern-transform tail (end-appended so older refs tails decode clean):
    // absent → identity.
    node.paint.hasPatternXform = false;
    node.paint.patternXform[0] = node.paint.patternXform[3] = 1.0;
    node.paint.patternXform[1] = node.paint.patternXform[2] =
        node.paint.patternXform[4] = node.paint.patternXform[5] = 0.0;
    if (r.i < bytes.size() && r.ok) {
        node.paint.hasPatternXform = r.u8() != 0;
        if (r.ok && node.paint.hasPatternXform)
            for (double& m : node.paint.patternXform) m = r.f64();
        if (!r.ok) return std::nullopt;
    }
    // Stroke-gradient tail (end-appended after the pattern-transform tail):
    // absent in older files → the stroke stays the flat `stroke` colour.
    node.paint.hasStrokeGradient = false;
    node.paint.strokeGradient = ArtGradient{};
    if (r.i < bytes.size() && r.ok) {
        node.paint.hasStrokeGradient = r.u8() != 0;
        if (r.ok && node.paint.hasStrokeGradient) {
            ArtGradient& g = node.paint.strokeGradient;
            g.radial = r.u8() != 0;
            g.x1 = r.f64();
            g.y1 = r.f64();
            g.x2 = r.f64();
            g.y2 = r.f64();
            g.cx = r.f64();
            g.cy = r.f64();
            g.r = r.f64();
            const std::uint32_t stops = r.u32();
            if (!r.ok || stops > bytes.size()) return std::nullopt;
            g.stops.resize(stops);
            for (ArtStop& s : g.stops) {
                s.pos = r.f32();
                r.rgba(s.rgba);
            }
        }
        if (!r.ok) return std::nullopt;
    }
    if (consumed) *consumed = r.i;
    return node;
}

}  // namespace pittore::vector
