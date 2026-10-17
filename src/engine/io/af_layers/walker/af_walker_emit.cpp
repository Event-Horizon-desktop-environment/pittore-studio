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
#include <cstdio>
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
    // True when the shape produced a pixel layer (children are then emitted
    // as clipped rows by finishImageLayer). False when the shape itself had
    // nothing to paint; the caller may still keep its children.
bool Walker::emitShape(const Node* node, const Mat& ctm, int indent, bool clipped) {
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
            return false;
        }
        const float x0 = static_cast<float>(std::min((*b)[0], (*b)[2]));
        const float y0 = static_cast<float>(std::min((*b)[1], (*b)[3]));
        const float x1 = static_cast<float>(std::max((*b)[0], (*b)[2]));
        const float y1 = static_cast<float>(std::max((*b)[1], (*b)[3]));
        const double dw = (x1 - x0) * matScaleX(lctm);
        const double dh = (y1 - y0) * matScaleY(lctm);
        if (dw < 0.5 || dh < 0.5 || dw > 1e6 || dh > 1e6) {
            noteSkip(node, "shape is degenerate or too large");
            return false;
        }
        auto geo = shapeGeometry(g, shpe, x0, y0, x1, y1);
        if (!geo) {
            noteSkip(node, "shape geometry is not supported");
            return false;
        }
        if (displayName.empty()) displayName = geo->name;
        evenOdd = geo->subpaths.size() > 1;
        for (auto& sp : geo->subpaths)
            path.subpaths.push_back(SubPath{std::move(sp.first), sp.second});
        transformAnchors(path, lctm);
    } else {
        auto path = pathFromCurveRecords(g, node, lctm);
        if (!path) {
            noteSkip(node, "path has no curve data");
            return false;
        }
        // Even-odd, so a path layer's nested contours punch holes — unless
        // the curve asks for winding fill: `CvWi` 1 fills same-direction
        // double coverage (stroke crossings, retraced spikes) instead of
        // punching it out. Observed on files Affinity renders hole-free.
        bool curveEvenOdd = true;
        if (const auto wi = enumOf(g, node, "CvWi"))
            curveEvenOdd = *wi != 1;
        evenOdd = curveEvenOdd;
        if (displayName.empty()) displayName = "Curve";
        // pathFromCurveRecords folds "no data" and "no geometry" together;
        // both mean there is nothing to draw here.
        VectorPath draw = std::move(*path);
        std::optional<GradientFill> gradient;
        auto shape = vectorPaint(g, node, lctm, std::move(draw), evenOdd, gradient);
        if (!shape) {
            noteSkip(node, "vector layer is invisible");
            return false;
        }
        auto img = rasterizeShape(*shape, gradient ? &*gradient : nullptr);
        if (!img) {
            noteSkip(node, "vector shape is empty or too large");
            return false;
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
        return true;
    }

    std::optional<GradientFill> gradient;
    auto shape = vectorPaint(g, node, lctm, std::move(path), evenOdd, gradient);
    if (!shape) {
        noteSkip(node, "vector layer is invisible");
        return false;
    }
    auto img = rasterizeShape(*shape, gradient ? &*gradient : nullptr);
    if (!img) {
        noteSkip(node, "vector shape is empty or too large");
        return false;
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
    return true;
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
    // Line separators, the vertical tab the format writes for a soft
    // return, and CR/CRLF all become plain breaks. The paragraph
    // separator U+2029 becomes '\v', which the layout engine treats as a
    // paragraph break carrying the story's recorded paragraph spacing.
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
                   static_cast<unsigned char>(raw[i + 2]) == 0xA9) {
            text += '\v';  // U+2029 paragraph separator
            i += 3;
        } else if (c == 0xE2 && i + 2 < raw.size() &&
                   static_cast<unsigned char>(raw[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(raw[i + 2]) == 0xA8) {
            text += '\n';  // U+2028 line separator
            i += 3;
        } else {
            text += raw[i++];
        }
    }
    if (trimmed(text).empty()) {
        noteSkip(node, "text is empty");
        return;
    }

    // The first block's paragraph attributes give the alignment, plus the
    // explicit vertical metrics the record carries.
    int align = 0;
    double paraGapUnits = 0.0;  // spacing between paragraphs, text-space units
    double leadEm = 0.0;        // line advance as a fraction of the em size
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
        }
        const Value* pd = g.field(item, "Doub");
        if (pd && pd->k == Value::K::Array && pd->arr.size() > 10) {
            // Slot semantics read off the observed layouts these records
            // belong to: index 6 is the spacing between paragraphs (25 in
            // these documents, matching the gap rendered at the one break
            // in the story); index 8 is the leading as a fraction of the
            // em size (0.8 here, where the faces' natural line steps are
            // ~1.17 and would double-space the design). Remaining slots
            // held zeros or values that matched nothing rendered.
            const double gap = pd->arr[6].f;
            const double lead = pd->arr[8].f;
            if (gap > 0.0 && gap < 10000.0) paraGapUnits = gap;
            if (lead > 0.05 && lead < 8.0) leadEm = lead;
        }
        break;
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
    // The run size and frame box are units of the text node's own space;
    // every transform above it — including one that stretches the axes
    // differently (the Disney cards carry a 1.5-y artboard under a 0.837-y
    // text scale) — belongs to the placed raster, not to the glyph size.
    // Layout therefore happens in a uniform layout space at the larger axis
    // scale, and the recorded warp resamples the result. Baking a net axis
    // scale into the size itself would widen the advances as well.
    double docScale = std::max(std::hypot(lctm.m[1], lctm.m[4]), 1e-6);
    const double frmWl = std::abs((*frmb)[2] - (*frmb)[0]);
    const double frmHl = std::abs((*frmb)[3] - (*frmb)[1]);
    bool stretched = false;
    if (!rotated) {
        const double netX = std::hypot(lctm.m[0], lctm.m[3]);
        stretched = std::abs(netX - docScale) > 0.004 * std::max(netX, docScale);
        docScale = std::max(netX, docScale);
    }
    int frameLeft = 0, frameTop = 0, frameWidth = 0, frameHeight = 0;
    if (rotated) {
        frameWidth = static_cast<int>(std::llround(frmWl * docScale));
        frameHeight = static_cast<int>(std::llround(frmHl * docScale));
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
        // In layout units, so the wrap needs no further conversion.
        frameWidth = static_cast<int>(std::llround(stretched ? frmWl * docScale
                                                             : maxx - minx));
        frameHeight = static_cast<int>(std::llround(stretched ? frmHl * docScale
                                                              : maxy - miny));
    }
    const float effSize = static_cast<float>(size * docScale);
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
    // Frame text honours the story's recorded leading and paragraph
    // spacing; artistic text keeps solving its line step from the
    // recorded pen box below.
    if (frameText) {
        if (leadEm > 0.0) spec.emLeading = static_cast<float>(leadEm);
        if (paraGapUnits > 0.0)
            spec.paragraphGap = static_cast<float>(paraGapUnits * docScale);
    }
    // Frame text fills its box and wraps there; artistic text runs on one line.
    if (frameText && frameWidth > 8) spec.wrapWidth = static_cast<float>(frameWidth);
    // The engine bakes decorations/background with the ink colour, so hand
    // it the run's fill before rasterizing.
    for (int i = 0; i < 4; ++i)
        spec.inkColor[i] = static_cast<float>(color[i]) / 255.0f;

    auto raster = pittore::text::rasterize(spec);
    bool defaultFont = false;
    if (!raster) {
        spec.family = pittore::text::defaultFamily();
        defaultFont = true;
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
        const std::size_t nl = text.find_first_of("\n\v");
        const std::string firstLine =
            trimmed(text.substr(0, nl == std::string::npos ? text.size() : nl));
        std::string label = utf8Prefix(firstLine, 32);
        if (label.size() < firstLine.size()) label += "\xE2\x80\xA6";
        displayName = label.empty() ? "Text" : label;
    }

    // Import log: what the story asked for and what this machine laid
    // out, so a shortfall in the vertical metrics is auditable from the
    // log alone, without a debugger or a second application.
    {
        auto mag = [](double v) {
            char b[32];
            std::snprintf(b, sizeof b, "%.4g", v);
            return std::string(b);
        };
        std::string line = "text '" + displayName + "': size " + mag(size);
        if (std::abs(spec.size - size) > 0.005) line += " laid " + mag(spec.size);
        line += ", font '" + family + "'";
        if (defaultFont) line += " (unresolved, using '" + spec.family + "')";
        line += leadEm > 0.0 ? ", leading " + mag(leadEm) + " em"
                             : ", leading face natural";
        line += paraGapUnits > 0.0 ? ", para-gap " + mag(paraGapUnits)
                                   : ", no para-gap";
        if (spec.wrapWidth) line += ", wrap " + mag(*spec.wrapWidth);
        line += ", frame " + std::to_string(frameWidth) + "x" +
                std::to_string(frameHeight) + ", lines " +
                std::to_string(raster->lineCount);
        doc.log.push_back(line);
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
    // A non-rotated, unstretched layer can be handed back to the UI as a
    // live spec: the origin the editable render expects is the frame's left
    // for wrapped (frame) text, and the pen box for artistic text.
    TextPayload payload;
    const bool editable = !rotated && !stretched;
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
    } else if (stretched) {
        // The ink offsets computed above are canvas coords for a uniform
        // placement; here the anchor is the frame's own corner in layout
        // space (frameLeft/Top are 0 there), then the recorded warp places
        // it — like the rotation branch, without the shear components.
        int px = 0, py = 0;
        if (raster->layoutWidth > 1.0f) {
            const int lw = static_cast<int>(std::llround(raster->layoutWidth));
            if (spec.align == pittore::text::Align::Center && frameWidth > 8)
                px = (frameWidth - lw) / 2;
            else if (spec.align == pittore::text::Align::Right && frameWidth > 8)
                px = frameWidth - lw;
        } else {
            px = -raster->bounds.left;
        }
        if (!frameText && raster->firstBaseline > 0.0f) {
            const std::size_t n = lineCount > 0 ? lineCount : 1;
            const float lastBaseline =
                raster->firstBaseline + static_cast<float>(n - 1) * raster->lineAdvance;
            py = frameHeight - static_cast<int>(std::llround(lastBaseline));
        } else {
            py = -raster->bounds.top;
        }
        const auto c0 = matApply(lctm, std::min((*frmb)[0], (*frmb)[2]),
                                 std::min((*frmb)[1], (*frmb)[3]));
        const double inv = 1.0 / docScale;
        const Mat lin{{lctm.m[0] * inv, 0.0, 0.0, lctm.m[3] * inv, lctm.m[4] * inv, 0.0}};
        const auto o = matApply(lin, static_cast<double>(raster->bounds.left + px),
                                static_cast<double>(raster->bounds.top + py));
        const Mat map{{lin.m[0], lin.m[1], o.first + c0.first, lin.m[3], lin.m[4],
                       o.second + c0.second}};
        auto placed = affineResample(img, map);
        if (!placed) {
            noteSkip(node, "text placement failed");
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
        if (boardSplit) {
            if (auto it = boardIndex.find(node); it != boardIndex.end()) {
                // Designer artboard: the board itself is a page, not a panel
                // group. Its content rows join the board's page (indent
                // unchanged), and the board's own paint — its background
                // fill, when the file gives the shape one — stays below them.
                const int saved = currentBoard;
                currentBoard = it->second;
                const std::size_t skipBefore = doc.skippedLayers;
                const std::size_t logBefore = doc.log.size();
                if (!emitShape(node, ctm, indent, clipped) &&
                    doc.skippedLayers == skipBefore + 1 &&
                    doc.log.size() == logBefore + 1) {
                    // An unpainted board is a page bounds, not a skip.
                    doc.log.pop_back();
                    doc.skippedLayers = skipBefore;
                }
                const Mat childCtm = nodeCtm(node, ctm);
                for (Node* ch : g.children(node, "Chld")) emit(ch, childCtm, indent, false);
                currentBoard = saved;
                return;
            }
        }
        const std::size_t skipBefore = doc.skippedLayers;
        const std::size_t logBefore = doc.log.size();
        if (emitShape(node, ctm, indent, clipped)) return;
        // The shape itself had nothing to paint (unfilled artboard, guide,
        // transparent container). When it owns children those are the real
        // content — keep them under a group row instead of dropping the
        // subtree with the parent. Observed on Designer files where each
        // page is an unpainted "ShpN" artboard holding text and placed
        // images.
        std::vector<Node*> kids = g.children(node, "Chld");
        if (!kids.empty()) {
            if (doc.skippedLayers == skipBefore + 1 && doc.log.size() == logBefore + 1) {
                doc.log.pop_back();
                doc.skippedLayers = skipBefore;
            }
            std::string display = strOf(g, node, "Desc");
            AfLayer layer;
            layer.indent = indent;
            layer.clipped = clipped;
            layer.isGroup = true;
            layer.name = display.empty() ? "Group" : display;
            applyCommon(node, layer);
            layer.masks = placeMasks(node, ctm);
            const bool maskedGroup = !layer.masks.empty();
            doc.layers.push_back(std::move(layer));
            const Mat childCtm = nodeCtm(node, ctm);
            if (maskedGroup) ++maskedAncestors;
            for (Node* ch : kids) emit(ch, childCtm, indent + 1, false);
            if (maskedGroup) --maskedAncestors;
            doc.log.push_back("group '" + display + "': unpainted shape kept as container");
        }
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
