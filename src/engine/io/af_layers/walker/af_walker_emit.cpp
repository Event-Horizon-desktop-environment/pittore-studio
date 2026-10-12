#include "engine/io/af_layers.h"
#include "engine/io/af.h"
#include "engine/render/layer_style.h"
#include "engine/text/text_engine.h"
#include "engine/vector/vector_shape.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>
#ifdef PITTORE_AF
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif
#ifdef PITTORE_WEBP
#include "engine/io/webp.h"
#endif
#ifdef PITTORE_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#endif
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/geom/af_geom.h"
#include "engine/io/af_layers/geom/af_homography.h"
#include "engine/io/af_layers/graph/af_graph.h"
#include "engine/io/af_layers/graph/af_graph_parser.h"
#include "engine/io/af_layers/image/af_image.h"
#include "engine/io/af_layers/filter/af_blur.h"
#include "engine/io/af_layers/filter/af_distort.h"
#include "engine/io/af_layers/filter/af_fx.h"
#include "engine/io/af_layers/filter/af_live.h"
#include "engine/io/af_layers/vector/af_shapes.h"
#include "engine/io/af_layers/walker/af_walker.h"

namespace pittore::io {
namespace af_detail {


void Walker::emitRaster(const Node* node, const Mat& ctm, int indent, bool clipped) {
    const std::uint32_t kind = g.typeTag(node);
    Node* bitm = g.child(node, "Bitm");
    if (!bitm || g.typeTag(bitm) != tag4("DyBm")) {
        noteSkip(node, "no bitmap record");
        return;
    }
    std::string why;
    auto img = decodeBitmap(bitm, why);
    if (!img) {
        noteSkip(node, "bitmap: " + why);
        return;
    }
    std::string display = strOf(g, node, "Desc");
    const std::string irfn = strOf(g, node, "IRFN");
    if (display.empty() || display == "Image")
        display = irfn.empty() ? (kind == tag4("ImgN") ? "Image" : "Pixel") : irfn;
    // Live filters rework the layer's own pixels in place before
    // placement; a Live Perspective also warps the placement map by
    // wherever its destination quad lands.
    Mat map = nodeCtm(node, ctm);
    for (const LiveFilter& f : liveFilters(g, node)) {
        switch (f.kind) {
            case LiveFilter::KindGeometry:
                img->px = distortApply(img->w, img->h, img->px, f.distort);
                break;
            case LiveFilter::KindBlur:
                img->px = liveBlurApply(img->w, img->h, img->px, f.blur);
                break;
            case LiveFilter::KindVignette:
                img->px = vignetteApply(img->w, img->h, img->px, f.vignette);
                break;
        }
    }
    if (const auto h = liveWarp(g, node)) {
        if (auto warped = perspectiveResample(*img, *h)) {
            map = matThen(map, translation(static_cast<double>(warped->ox),
                                           static_cast<double>(warped->oy)));
            *img = std::move(warped->img);
        }
    }
    for (Node* f : g.children(node, "AdCh")) {
        if (!nodeIsFlrn(f) || filterIsIdentity(g, f)) continue;
        if (warpOf(g, f) || distortOf(g, f)) continue;
        Node* filt = g.child(f, "Filt");
        const std::string lfName = filt ? tagName(g.typeTag(filt)) : "?";
        doc.log.push_back("skip live filter " + lfName + " on '" + display +
                          "': not supported");
        ++doc.skippedLayers;
    }
    auto placed = placeRaster(map, std::move(*img));
    if (!placed) {
        noteSkip(node, "placement failed");
        return;
    }
    finishImageLayer(node, ctm, indent, clipped, display, irfn, std::move(placed->img),
                     placed->rect);
}

    // A fill raster ("FRst", sections FRst, Rstr, Node): a solid fill colour
    // from its `BFFl` descriptor (same classes as vector paint, so HSLA and
    // friends resolve) masked by its bitmap, which is usually format 6 (one
    // mask channel). The mask-kind interleave stores coverage in R.
    void Walker::emitFill(const Node* node, const Mat& ctm, int indent, bool clipped) {
        const std::vector<Node*> fillDesc = g.children(node, "BFFl");
        Node* fillNode = fillDesc.empty() ? nullptr : g.child(fillDesc.front(), "FDeF");
        if (!fillNode) fillNode = g.child(node, "BFil");
        const auto color = fillColorBytes(g, fillNode);
        if (!color) {
            noteSkip(node, "fill has no solid colour");
            return;
        }
        Node* bitm = g.child(node, "Bitm");
        if (!bitm || g.typeTag(bitm) != tag4("DyBm")) {
            noteSkip(node, "fill has no bitmap record");
            return;
        }
        std::string why;
        auto mask = decodeBitmap(bitm, why);
        if (!mask) {
            noteSkip(node, "fill bitmap: " + why);
            return;
        }
        render::Rgba8Image img;
        img.w = mask->w;
        img.h = mask->h;
        img.px.resize(static_cast<std::size_t>(mask->w) * mask->h * 4);
        for (std::size_t i = 0, n = static_cast<std::size_t>(mask->w) * mask->h; i < n; ++i) {
            const std::uint32_t cov = mask->px[i * 4];
            img.px[i * 4 + 0] = static_cast<std::uint8_t>((*color)[0] * cov / 255);
            img.px[i * 4 + 1] = static_cast<std::uint8_t>((*color)[1] * cov / 255);
            img.px[i * 4 + 2] = static_cast<std::uint8_t>((*color)[2] * cov / 255);
            img.px[i * 4 + 3] = static_cast<std::uint8_t>((*color)[3] * cov / 255);
        }
        std::string display = strOf(g, node, "Desc");
        if (display.empty()) display = "Fill";
        auto placed = placeRaster(nodeCtm(node, ctm), std::move(img));
        if (!placed) {
            noteSkip(node, "fill placement failed");
            return;
        }
        const IntRect rect{placed->rect.x0, placed->rect.y0, placed->rect.x1, placed->rect.y1};
        finishImageLayer(node, ctm, indent, clipped, display, "", std::move(placed->img), rect);
    }

    // Rebuild a live shape ("ShpN") or free path ("PCrv") into pixels.
void Walker::emitShape(const Node* node, const Mat& ctm, int indent, bool clipped) {
    using namespace pittore::vector;
    const std::uint32_t kind = g.typeTag(node);
    const std::string display = strOf(g, node, "Desc");
    std::string displayName = display;
    const Mat lctm = nodeCtm(node, ctm);
    VectorPath path;
    bool evenOdd = false;

    if (kind == tag4("ShpN")) {
        Node* shpe = g.child(node, "Shpe");
        const std::vector<double>* b = f64s(g, node, "ShpB");
        if (!shpe || !b || b->size() < 4) {
            noteSkip(node, "shape has no geometry");
            return;
        }
        const float x0 = static_cast<float>(std::min((*b)[0], (*b)[2]));
        const float y0 = static_cast<float>(std::min((*b)[1], (*b)[3]));
        const float x1 = static_cast<float>(std::max((*b)[0], (*b)[2]));
        const float y1 = static_cast<float>(std::max((*b)[1], (*b)[3]));
        const double dw = (x1 - x0) * matScaleX(lctm);
        const double dh = (y1 - y0) * matScaleY(lctm);
        if (dw < 0.5 || dh < 0.5 || dw > 1e6 || dh > 1e6) {
            noteSkip(node, "shape is degenerate or too large");
            return;
        }
        auto geo = shapeGeometry(g, shpe, x0, y0, x1, y1);
        if (!geo) {
            noteSkip(node, "shape geometry is not supported");
            return;
        }
        if (displayName.empty()) displayName = geo->name;
        evenOdd = geo->subpaths.size() > 1;
        for (auto& sp : geo->subpaths)
            path.subpaths.push_back(SubPath{std::move(sp.first), sp.second});
        transformAnchors(path, lctm);
    } else {
        Node* crvs = g.child(node, "Crvs");
        Node* data = crvs ? g.child(crvs, "Data") : nullptr;
        if (!data || data->fields.empty()) {
            noteSkip(node, "path has no curve data");
            return;
        }
        const auto toDoc = [&](double x, double y) {
            const auto p = matApply(lctm, x, y);
            return std::make_pair(static_cast<float>(p.first),
                                  static_cast<float>(p.second));
        };
        // Records run: a closed flag, then per subpath an 18-byte stream
        // (two f64s and a marker pair). The flag resets for the next one.
        bool closed = true;
        for (const auto& fld : data->fields) {
            const Value& v = fld.second;
            if (v.k == Value::K::Bool) {
                closed = v.u != 0;
                continue;
            }
            if (v.k != Value::K::Array) continue;
            bool hasCurve = false;
            for (const Value& item : v.arr)
                if (item.k == Value::K::Struct) {
                    hasCurve = true;
                    break;
                }
            if (!hasCurve) continue;
            std::vector<PathRecord> records;
            bool malformed = false;
            for (const Value& item : v.arr) {
                if (item.k != Value::K::Struct) continue;
                if (item.bytes.size() != 18) {
                    malformed = true;
                    break;
                }
                double x = 0.0, y = 0.0;
                std::memcpy(&x, item.bytes.data(), 8);
                std::memcpy(&y, item.bytes.data() + 8, 8);
                const auto p = toDoc(x, y);
                records.push_back(
                    PathRecord{p.first, p.second, item.bytes[16], item.bytes[17]});
            }
            if (malformed) {
                noteSkip(node, "path record is malformed");
                return;
            }
            if (auto sub = subpathFromRecords(records, closed))
                path.subpaths.push_back(std::move(*sub));
            closed = true;
        }
        if (path.isEmpty()) {
            noteSkip(node, "path has no geometry");
            return;
        }
        // Even-odd, so a path layer's nested contours punch holes.
        evenOdd = true;
        if (displayName.empty()) displayName = "Curve";
    }

    std::optional<GradientFill> gradient;
    auto shape = vectorPaint(g, node, lctm, std::move(path), evenOdd, gradient);
    if (!shape) {
        noteSkip(node, "vector layer is invisible");
        return;
    }
    auto img = rasterizeShape(*shape, gradient ? &*gradient : nullptr);
    if (!img) {
        noteSkip(node, "vector shape is empty or too large");
        return;
    }
    const IntRect rect{img->rect.left, img->rect.top, img->rect.right, img->rect.bottom};
    // Keep the true geometry: shift document-space anchors into the layer's
    // source space, matching the raster's origin, so later moves/scales of
    // the layer carry the vector with the pixels.
    auto art = pittore::vector::artNodeFromShape(
        *shape, gradient ? &*gradient : nullptr,
        -static_cast<double>(rect.x0), -static_cast<double>(rect.y0));
    render::Rgba8Image rimg;
    rimg.w = static_cast<std::uint32_t>(img->rect.width());
    rimg.h = static_cast<std::uint32_t>(img->rect.height());
    rimg.px = std::move(img->rgba);
    finishImageLayer(node, ctm, indent, clipped, displayName, "", std::move(rimg), rect,
                     false, nullptr, std::move(art));
}

    // Rebuild a text layer ("TxtA" artistic, "TxtF" frame) by re-setting its
    // stored story in the requested font: the string and paragraph alignment
    // come from the story blocks, the size and colour from the first run, and
    // the frame box records where (and, for rotated text, how) it sits.
void Walker::emitText(const Node* node, const Mat& ctm, int indent, bool clipped) {
    const bool frameText = g.typeTag(node) == tag4("TxtF");
    Node* story = g.child(node, "StSt");
    if (!story) {
        noteSkip(node, "text has no story");
        return;
    }
    const std::vector<Node*> blocks = g.children(story, "Blok");
    std::string raw;
    bool firstBlock = true;
    for (Node* b : blocks) {
        Node* glyph = g.child(b, "Glyp");
        if (!glyph) continue;
        std::string s = strOf(g, glyph, "Utf8");
        while (!s.empty() && s.back() == '\0') s.pop_back();
        if (!firstBlock) raw += '\n';
        raw += s;
        firstBlock = false;
    }
    // Paragraph and line separators, and the vertical tab the format writes
    // for a soft return, all become newlines; CR/CRLF normalize with them.
    std::string text;
    text.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
            text += '\n';
            ++i;
        } else if (c == 0x0B) {
            text += '\n';
            ++i;
        } else if (c == 0xE2 && i + 2 < raw.size() &&
                   static_cast<unsigned char>(raw[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(raw[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(raw[i + 2]) == 0xA9)) {
            text += '\n';
            i += 3;
        } else {
            text += raw[i++];
        }
    }
    if (trimmed(text).empty()) {
        noteSkip(node, "text is empty");
        return;
    }

    // The first block's paragraph attributes give the alignment.
    int align = 0;
    for (Node* b : blocks) {
        Node* patt = g.child(b, "PAtt");
        if (!patt) continue;
        const std::vector<Node*> runs = g.children(patt, "Runs");
        if (runs.empty()) continue;
        Node* item = g.child(runs[0], "Item");
        if (!item) continue;
        const Value* ints = g.field(item, "Ints");
        if (ints && ints->k == Value::K::Array && !ints->arr.empty() &&
            ints->arr[0].k == Value::K::I32) {
            align = static_cast<int>(ints->arr[0].u);
            break;
        }
    }

    // The first run's character attributes speak for the whole layer.
    Node* runItem = nullptr;
    for (Node* b : blocks) {
        Node* gatt = g.child(b, "GAtt");
        if (!gatt) continue;
        const std::vector<Node*> runs = g.children(gatt, "Runs");
        if (runs.empty()) continue;
        runItem = g.child(runs[0], "Item");
        if (runItem) break;
    }
    if (!runItem) {
        noteSkip(node, "text has no run attributes");
        return;
    }
    const Value* doub = g.field(runItem, "Doub");
    if (!doub || doub->k != Value::K::Array || doub->arr.empty() ||
        doub->arr[0].k != Value::K::F64) {
        noteSkip(node, "text run has no size");
        return;
    }
    const double size = doub->arr[0].f;

    // Two descriptors name the font: `DFnt` what the document was designed
    // with, `RFnt` what the writing machine resolved it to. A document font
    // that is installed (or whose family, stripped of a trailing
    // parenthetical, is) wins; only a font this machine lacks falls back to
    // the resolved one.
    auto installedName = [&](const Node* f) -> std::optional<std::string> {
        const std::string fam = strOf(g, f, "Famy");
        if (fam.empty()) return std::nullopt;
        if (pittore::text::hasFamily(fam)) return fam;
        const std::size_t p = fam.rfind(" (");
        if (p == std::string::npos) return std::nullopt;
        const std::string head = trimmed(fam.substr(0, p));
        if (!head.empty() && pittore::text::hasFamily(head)) return head;
        return std::nullopt;
    };
    Node* dfnt = g.child(runItem, "DFnt");
    Node* rfnt = g.child(runItem, "RFnt");
    const Node* font = nullptr;
    std::string family;
    bool familyInstalled = false;
    for (Node* f : {dfnt, rfnt}) {
        if (!f) continue;
        if (auto fam = installedName(f)) {
            font = f;
            family = *fam;
            familyInstalled = true;
            break;
        }
    }
    if (!familyInstalled) {
        for (Node* f : {rfnt, dfnt}) {
            if (!f) continue;
            const std::string fam = strOf(g, f, "Famy");
            if (!fam.empty()) {
                font = f;
                family = fam;
                break;
            }
        }
    }
    const std::string post = font ? strOf(g, font, "Post") : std::string();
    const int weight = font ? i32Of(g, font, "Wegt", 400) : 400;
    const bool italic = font ? boolOf(g, font, "Ital").value_or(false) : false;
    std::array<std::uint8_t, 4> color{0, 0, 0, 255};
    for (Node* obj : g.children(runItem, "Objs")) {
        Node* fdef = g.child(obj, "FDeF");
        if (!fdef) continue;
        if (auto c = fillColorBytes(g, fdef)) {
            color = *c;
            break;
        }
    }

    Node* frame = g.child(node, "TxtH");
    if (!frame) {
        noteSkip(node, "text has no frame");
        return;
    }
    const std::vector<double>* frmb = f64s(g, frame, "FrmB");
    if (!frmb || frmb->size() != 4) {
        noteSkip(node, "text frame box is malformed");
        return;
    }
    const Mat lctm = nodeCtm(node, ctm);
    const bool rotated = !matAxisAligned(lctm);
    const double docScale = std::max(std::hypot(lctm.m[1], lctm.m[4]), 1e-6);
    int frameLeft = 0, frameTop = 0, frameWidth = 0, frameHeight = 0;
    if (rotated) {
        frameWidth =
            static_cast<int>(std::llround(std::abs((*frmb)[2] - (*frmb)[0]) * docScale));
        frameHeight =
            static_cast<int>(std::llround(std::abs((*frmb)[3] - (*frmb)[1]) * docScale));
    } else {
        const auto p0 = matApply(lctm, (*frmb)[0], (*frmb)[1]);
        const auto p1 = matApply(lctm, (*frmb)[2], (*frmb)[1]);
        const auto p2 = matApply(lctm, (*frmb)[0], (*frmb)[3]);
        const auto p3 = matApply(lctm, (*frmb)[2], (*frmb)[3]);
        const double minx = std::min({p0.first, p1.first, p2.first, p3.first});
        const double miny = std::min({p0.second, p1.second, p2.second, p3.second});
        const double maxx = std::max({p0.first, p1.first, p2.first, p3.first});
        const double maxy = std::max({p0.second, p1.second, p2.second, p3.second});
        frameLeft = static_cast<int>(std::llround(minx));
        frameTop = static_cast<int>(std::llround(miny));
        frameWidth = static_cast<int>(std::llround(maxx - minx));
        frameHeight = static_cast<int>(std::llround(maxy - miny));
    }
    const float effSize = static_cast<float>(size * std::hypot(lctm.m[1], lctm.m[4]));
    if (!(effSize >= 0.5f && effSize <= 10000.0f)) {
        noteSkip(node, "text size is out of range");
        return;
    }

    pittore::text::TextSpec spec;
    spec.text = text;
    spec.family = family;
    spec.bold = weight >= 600 || post.find("Bold") != std::string::npos ||
                post.find("Black") != std::string::npos ||
                post.find("Heavy") != std::string::npos;
    spec.italic = italic || post.find("Italic") != std::string::npos ||
                  post.find("Oblique") != std::string::npos;
    spec.size = effSize;
    spec.align = align == 1   ? pittore::text::Align::Center
                 : align == 2 ? pittore::text::Align::Right
                              : pittore::text::Align::Left;
    spec.lineHeight = 1.0f;
    spec.tracking = 0.0f;
    // Frame text fills its box and wraps there; artistic text runs on one line.
    if (frameText && frameWidth > 8) spec.wrapWidth = static_cast<float>(frameWidth);
    // The engine bakes decorations/background with the ink colour, so hand
    // it the run's fill before rasterizing.
    for (int i = 0; i < 4; ++i)
        spec.inkColor[i] = static_cast<float>(color[i]) / 255.0f;

    auto raster = pittore::text::rasterize(spec);
    if (!raster) {
        spec.family = pittore::text::defaultFamily();
        raster = pittore::text::rasterize(spec);
        if (!raster) {
            noteSkip(node, "no installed font for text");
            return;
        }
    }
    if (raster->isEmpty()) {
        noteSkip(node, "text rasterized empty");
        return;
    }

    // An artistic frame records how wide the writing machine set the text —
    // as a pen box, side bearings and all. With the real family installed
    // the natural layout is already right; when substituting, scale the
    // size so our advance width still fills the recorded box. Frame text
    // keeps its size and reflows instead.
    if (!frameText && !familyInstalled && frameWidth > 8 && raster->layoutWidth > 1.0f) {
        const float ratio = static_cast<float>(frameWidth) / raster->layoutWidth;
        if (std::abs(ratio - 1.0f) > 0.002f) {
            spec.size *= ratio;
            raster = pittore::text::rasterize(spec);
            if (!raster || raster->isEmpty()) {
                noteSkip(node, "text re-rasterized empty");
                return;
            }
        }
    }
    // Multi-line artistic text records the first line's cap down to the
    // last line's baseline, so what the box has beyond that cap is the
    // leading the writer used. Solve for it rather than trusting the face's.
    const std::size_t lineCount = textLineCount(text);
    if (!frameText && lineCount > 1 && raster->lineAdvance > 0.0f) {
        const float cap = raster->capHeight.value_or(
            raster->firstBaseline - static_cast<float>(raster->bounds.top));
        const float wanted =
            (static_cast<float>(frameHeight) - cap) / static_cast<float>(lineCount - 1);
        const float scale = wanted / raster->lineAdvance;
        if (scale >= 0.25f && scale <= 4.0f && std::abs(scale - 1.0f) > 0.01f) {
            spec.lineHeight *= scale;
            raster = pittore::text::rasterize(spec);
            if (!raster || raster->isEmpty()) {
                noteSkip(node, "text re-rasterized empty");
                return;
            }
        }
    }

    // Anchor the pen box to the frame: the layout puts the widest line's
    // pen at 0, so the frame left is the origin and the ink lands one side
    // bearing inside it. (When no advances were measured, anchor the ink.)
    int ox = 0, oy = 0;
    if (raster->layoutWidth > 1.0f) {
        const int lw = static_cast<int>(std::llround(raster->layoutWidth));
        if (spec.align == pittore::text::Align::Center && frameWidth > 8)
            ox = frameLeft + (frameWidth - lw) / 2;
        else if (spec.align == pittore::text::Align::Right && frameWidth > 8)
            ox = frameLeft + frameWidth - lw;
        else
            ox = frameLeft;
    } else {
        ox = frameLeft - raster->bounds.left;
    }
    // Vertically, artistic text anchors by baseline (the frame's bottom
    // edge is the last line's baseline); frame text keeps ink-top anchoring.
    if (!frameText && raster->firstBaseline > 0.0f) {
        const std::size_t n = lineCount > 0 ? lineCount : 1;
        const float lastBaseline =
            raster->firstBaseline + static_cast<float>(n - 1) * raster->lineAdvance;
        oy = frameTop + frameHeight - static_cast<int>(std::llround(lastBaseline));
    } else {
        oy = frameTop - raster->bounds.top;
    }

    std::string display = strOf(g, node, "Desc");
    std::string displayName = display;
    if (displayName.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string firstLine =
            trimmed(text.substr(0, nl == std::string::npos ? text.size() : nl));
        std::string label = utf8Prefix(firstLine, 32);
        if (label.size() < firstLine.size()) label += "\xE2\x80\xA6";
        displayName = label.empty() ? "Text" : label;
    }

    const int rw = raster->bounds.width();
    const int rh = raster->bounds.height();
    render::Rgba8Image img;
    img.w = static_cast<std::uint32_t>(rw);
    img.h = static_cast<std::uint32_t>(rh);
    img.px.assign(static_cast<std::size_t>(rw) * rh * 4, 0);
    if (raster->colorRgba.size() == raster->coverage.size() * 4) {
        // The engine already composited the fill, the highlight box and any
        // decorations; use its straight-alpha pixels directly.
        std::memcpy(img.px.data(), raster->colorRgba.data(), raster->colorRgba.size());
    } else {
        const unsigned alphaScale = color[3];
        for (std::size_t i = 0; i < raster->coverage.size(); ++i) {
            std::uint8_t* p = &img.px[i * 4];
            p[0] = color[0];
            p[1] = color[1];
            p[2] = color[2];
            p[3] = static_cast<std::uint8_t>(
                static_cast<unsigned>(raster->coverage[i]) * alphaScale / 255u);
        }
    }
    IntRect rect;
    const int bx = raster->bounds.left + ox;
    const int by = raster->bounds.top + oy;
    // A non-rotated layer can be handed back to the UI as a live spec: the
    // origin the editable render expects is the frame's left for wrapped
    // (frame) text, and the pen box for artistic text.
    TextPayload payload;
    const bool editable = !rotated;
    if (editable) {
        payload.spec = spec;
        payload.originX =
            static_cast<float>(spec.wrapWidth.value_or(0.0f) > 0.0f ? frameLeft : ox);
        payload.originY = static_cast<float>(oy);
        payload.frameHeight = static_cast<float>(frameHeight);
        payload.color = color;
    }
    if (rotated) {
        // Map layout space back through the rotation: a layout pixel p sits
        // at ctm · (frame_local_origin + p / doc_scale).
        const auto c0 = matApply(lctm, std::min((*frmb)[0], (*frmb)[2]),
                                 std::min((*frmb)[1], (*frmb)[3]));
        const double inv = 1.0 / docScale;
        const Mat lin{{lctm.m[0] * inv, lctm.m[1] * inv, 0.0, lctm.m[3] * inv,
                       lctm.m[4] * inv, 0.0}};
        const auto o = matApply(lin, static_cast<double>(bx), static_cast<double>(by));
        const Mat map{{lin.m[0], lin.m[1], o.first + c0.first, lin.m[3], lin.m[4],
                       o.second + c0.second}};
        auto placed = affineResample(img, map);
        if (!placed) {
            noteSkip(node, "text rotation failed");
            return;
        }
        img = std::move(placed->img);
        rect = placed->rect;
    } else {
        rect = IntRect{bx, by, bx + raster->bounds.width(), by + raster->bounds.height()};
    }
    finishImageLayer(node, ctm, indent, clipped, displayName, "", std::move(img), rect, true,
                     editable ? &payload : nullptr);
}

void Walker::emit(const Node* node, const Mat& ctm, int indent, bool clipped) {
    if (!node) return;
    const std::uint32_t kind = g.typeTag(node);
    if (kind == tag4("Grup") || kind == tag4("Scop")) {
        std::string display = strOf(g, node, "Desc");
        AfLayer layer;
        layer.indent = indent;
        layer.clipped = clipped;
        layer.isGroup = true;
        layer.name = display.empty() ? (kind == tag4("Scop") ? "Layer" : "Group") : display;
        applyCommon(node, layer);
        // A group's masks stay live on the group row; openAfLayers folds
        // them into each descendant's editable mask.
        layer.masks = placeMasks(node, ctm);
        const bool maskedGroup = !layer.masks.empty();
        doc.layers.push_back(std::move(layer));
        const Mat childCtm = nodeCtm(node, ctm);
        if (maskedGroup) ++maskedAncestors;
        for (Node* ch : g.children(node, "Chld")) emit(ch, childCtm, indent + 1, false);
        if (maskedGroup) --maskedAncestors;
        return;
    }
    if (kind == tag4("Rstr") || kind == tag4("ImgN")) {
        emitRaster(node, ctm, indent, clipped);
        return;
    }
    if (kind == tag4("FRst")) {
        emitFill(node, ctm, indent, clipped);
        return;
    }
    if (kind == tag4("ShpN") || kind == tag4("PCrv")) {
        emitShape(node, ctm, indent, clipped);
        return;
    }
    if (kind == tag4("TxtA") || kind == tag4("TxtF")) {
        emitText(node, ctm, indent, clipped);
        return;
    }
    noteSkip(node, "layer kind is out of scope");
}
}  // namespace af_detail
}  // namespace pittore::io
