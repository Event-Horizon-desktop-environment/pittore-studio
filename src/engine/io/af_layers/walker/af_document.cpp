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
#include <cstdlib>
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
#include "engine/io/af_layers/pages/af_pages.h"
#include "engine/io/af_layers/vector/af_shapes.h"
#include "engine/io/af_layers/walker/af_walker.h"

namespace pittore::io {
namespace af_detail {

std::optional<AfLayersDoc> buildDocument(const Archive& ar, const Graph& g,
                                         const std::string& baseDir) {
    AfLayersDoc doc;
    doc.log.push_back("container v" + std::to_string(ar.version) + ": " +
                      std::to_string(ar.entries.size()) + " entries, " +
                      std::to_string(ar.names.size()) + " names");
    Node* root = g.root();
    Node* docNode = g.child(root, "DocR");
    if (!docNode) {
        doc.log.push_back("no document root (DocR)");
        return doc;
    }
    std::vector<Node*> spreads = g.children(docNode, "Chld");
    if (spreads.empty()) {
        doc.log.push_back("document has no spreads");
        return doc;
    }

    // One source rect per spread: the union of its pages' trim ("PagR" rctp)
    // when present, else the spread bounds ("SprB"), else the document size.
    // "PagR" wins because "DfSz" can disagree with the true page geometry
    // (900x1500 vs 1200x1800 on a real two-page file) while the border
    // artwork only fits the page trim.
    struct SpreadRect {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        std::string source;
    };
    std::vector<SpreadRect> rects;
    rects.reserve(spreads.size());
    const std::vector<double>* dfsz = f64s(g, docNode, "DfSz");
    for (size_t i = 0; i < spreads.size(); ++i) {
        Node* sp = spreads[i];
        SpreadRect r;
        bool found = false;
        if (Node* spmd = g.child(sp, "SpMd")) {
            bool any = false;
            double ux0 = 0.0, uy0 = 0.0, ux1 = 0.0, uy1 = 0.0;
            for (Node* page : g.children(spmd, "PagR")) {
                const std::vector<double>* rc = f64s(g, page, "rctp");
                if (!rc || rc->size() < 4) continue;
                const double px0 = (*rc)[0], py0 = (*rc)[1];
                const double px1 = (*rc)[2], py1 = (*rc)[3];
                if (!(px1 > px0 && py1 > py0)) continue;
                if (!any) {
                    ux0 = px0;
                    uy0 = py0;
                    ux1 = px1;
                    uy1 = py1;
                    any = true;
                } else {
                    ux0 = std::min(ux0, px0);
                    uy0 = std::min(uy0, py0);
                    ux1 = std::max(ux1, px1);
                    uy1 = std::max(uy1, py1);
                }
            }
            if (any) {
                r = {ux0, uy0, ux1, uy1, "PagR"};
                found = true;
            }
        }
        if (!found) {
            if (const std::vector<double>* sb = f64s(g, sp, "SprB"); sb && sb->size() >= 4 &&
                (*sb)[2] > (*sb)[0] && (*sb)[3] > (*sb)[1]) {
                r = {(*sb)[0], (*sb)[1], (*sb)[2], (*sb)[3], "SprB"};
                found = true;
            }
        }
        if (!found) {
            if (dfsz && dfsz->size() >= 2 && (*dfsz)[0] > 0.0 && (*dfsz)[1] > 0.0) {
                r = {0.0, 0.0, (*dfsz)[0], (*dfsz)[1], "DfSz"};
                found = true;
            }
        }
        if (!found) {
            doc.log.push_back("spread " + std::to_string(i) +
                              " has no page bounds (PagR), spread bounds (SprB) or "
                              "document size (DfSz)");
            return doc;
        }
        rects.push_back(r);
    }

    // Stacked canvas via the shared page-layout helper (unit tested in
    // test_af_pages): widest page wide, every page tall plus a gap between
    // neighbours. Single-spread files keep their exact page size.
    constexpr int kPageGap = 128;
    std::vector<PageRect> pageRects;
    pageRects.reserve(rects.size());
    for (const SpreadRect& r : rects) pageRects.push_back({r.x0, r.y0, r.x1, r.y1});
    const StackedLayout layout = stackPages(pageRects, kPageGap);
    if (!layout.ok) {
        doc.log.push_back("implausible page geometry");
        return doc;
    }
    if (std::uint64_t(layout.width) * layout.height > kMaxPixels) {
        doc.log.push_back("canvas over the pixel cap");
        return doc;
    }
    doc.width = layout.width;
    doc.height = layout.height;
    doc.depth = 8;
    if (spreads.size() > 1) {
        doc.log.push_back("canvas " + std::to_string(doc.width) + "x" +
                          std::to_string(doc.height) + " (" +
                          std::to_string(spreads.size()) + " spreads stacked, gap " +
                          std::to_string(layout.gap) + ")");
    } else {
        doc.log.push_back("canvas " + std::to_string(doc.width) + "x" +
                          std::to_string(doc.height) + " (" + rects[0].source + ")");
    }
    for (size_t i = 0; i < spreads.size(); ++i)
        doc.log.push_back("page " + std::to_string(i) + " " + std::to_string(layout.pageW[i]) +
                          "x" + std::to_string(layout.pageH[i]) + " (" + rects[i].source + ")");

    Walker w{.ar = ar, .g = g, .doc = doc, .baseDir = baseDir};
    const bool multi = spreads.size() > 1;
    for (size_t i = 0; i < spreads.size(); ++i) {
        Node* spread = spreads[i];
        const std::uint32_t xOff = layout.xOff[i];
        const std::uint32_t yOff = layout.yOff[i];
        const Mat ctm = translation(-rects[i].x0 + double(xOff), -rects[i].y0 + double(yOff));

        // Designer artboards: top-level "ShpN" nodes whose "ShNR" child
        // carries the "AbSz" marker (plain shapes carry a bare "ShNR" and no
        // "AbSz"). Each is its own board: the page rect comes from the node's
        // "ShpB" bounds under its placement transform, so a board canvas is
        // read from file records only. The nodes directly below each board
        // are the board's real content, so they emit at the document's top
        // level and are tagged with the board's page index for the UI.
        const std::vector<Node*> content = g.children(spread, "Chld");
        std::vector<Node*> boards;
        for (Node* ch : content) {
            if (g.typeTag(ch) != tag4("ShpN")) continue;
              // The artboard marker lives on the "Shpe" child: an artboard's
            // shape record carries "AbSz" (artboard size); a plain shape's
            // bare record does not. Files saved under the paired-record
            // scheme instead hang "phrp" -> "aprp" off the board node, where
            // the aprp record's "node" field points back at its ShpN
            // (observed on the newest record of a two-artboard file).
            Node* shpe = g.child(ch, "Shpe");
            bool isBoard = shpe && g.field(shpe, "AbSz") != nullptr;
            if (!isBoard) {
                if (Node* ap = g.child(ch, "phrp")) {
                    const Value* nv = g.field(ap, "node");
                    isBoard = nv && nv->k == Value::K::Class && nv->cls && nv->cls == ch;
                }
            }
            if (!isBoard) continue;
            const std::vector<double>* b = f64s(g, ch, "ShpB");
            if (!b || b->size() < 4) {
                doc.log.push_back("artboard on spread " + std::to_string(i) +
                                  " has no ShpB bounds; opening as content");
                continue;
            }
            boards.push_back(ch);
        }
        if (!boards.empty()) w.boardSplit = true;
        if (boards.size() != content.size()) {
            // Loose nodes beside the boards stay untagged: the UI opens them
            // as residue beside the board canvases instead of dropping them.
            const bool loose = std::any_of(content.begin(), content.end(),
                                           [&boards](Node* n) {
                                               return std::find(boards.begin(),
                                                                boards.end(),
                                                                n) == boards.end();
                                           });
            if (loose)
                doc.log.push_back("spread " + std::to_string(i) +
                                  " keeps content outside every artboard");
        }
        for (Node* bd : boards) {
            const Mat actm = w.nodeCtm(bd, ctm);
            const std::vector<double>* b = f64s(g, bd, "ShpB");
            const std::pair<double, double> corner[4] = {
                matApply(actm, (*b)[0], (*b)[1]), matApply(actm, (*b)[0], (*b)[3]),
                matApply(actm, (*b)[2], (*b)[1]), matApply(actm, (*b)[2], (*b)[3])};
            double bx0 = corner[0].first, by0 = corner[0].second;
            double bx1 = bx0, by1 = by0;
            for (const auto& p : corner) {
                bx0 = std::min(bx0, p.first);
                by0 = std::min(by0, p.second);
                bx1 = std::max(bx1, p.first);
                by1 = std::max(by1, p.second);
            }
            std::string name;
            if (const Value* nv = g.field(bd, "Desc"); nv && nv->k == Value::K::Str)
                name = nv->s;
            AfPage page;
            page.x0 = bx0;
            page.y0 = by0;
            page.x1 = bx1;
            page.y1 = by1;
            page.source = "ShpB";
            page.name = name;
            page.artboard = true;
            doc.pages.push_back(std::move(page));
            w.boardIndex[bd] = static_cast<int>(doc.pages.size()) - 1;
            doc.log.push_back("artboard '" + name + "' " +
                              std::to_string(std::llround(bx1 - bx0)) + "x" +
                              std::to_string(std::llround(by1 - by0)) + " @ " +
                              std::to_string(std::llround(bx0)) + "," +
                              std::to_string(std::llround(by0)) +
                              " (ShpB under its placement)");
        }
        if (boards.empty()) {
            AfPage page;
            page.x0 = double(xOff);
            page.y0 = double(yOff);
            page.x1 = double(xOff + layout.pageW[i]);
            page.y1 = double(yOff + layout.pageH[i]);
            page.source = rects[i].source;
            doc.pages.push_back(std::move(page));
        }
        const int baseIndent = (multi && boards.empty()) ? 1 : 0;
        if (multi && boards.empty()) {
            AfLayer page;
            page.indent = 0;
            page.isGroup = true;
            page.groupExpanded = true;
            page.name = "Page " + std::to_string(i + 1);
            page.blend = "Normal";
            doc.layers.push_back(std::move(page));
        }

        if (Node* ras = g.child(spread, "RasS")) {
            if (Node* b = g.child(ras, "Bitm"); b && bitmapHasContent(g, b)) {
                std::string why;
                if (auto img = w.decodeBitmap(b, why)) {
                    if (auto placed = placeRaster(w.nodeCtm(ras, ctm), std::move(*img))) {
                        AfLayer layer;
                        layer.indent = baseIndent;
                        layer.left = placed->rect.x0;
                        layer.top = placed->rect.y0;
                        layer.width = static_cast<std::uint32_t>(placed->rect.width());
                        layer.height = static_cast<std::uint32_t>(placed->rect.height());
                        layer.name = "Background";
                        layer.rgba.resize(placed->img.px.size());
                        for (std::size_t k = 0; k < placed->img.px.size(); ++k)
                            layer.rgba[k] = static_cast<std::uint16_t>(placed->img.px[k]) * 257u;
                        doc.log.push_back("background raster spread " + std::to_string(i) +
                                          " " + std::to_string(layer.width) + "x" +
                                          std::to_string(layer.height));
                        doc.layers.push_back(std::move(layer));
                        ++doc.frameCount;
                    }
                }
            }
        }

        for (Node* ch : content) w.emit(ch, ctm, baseIndent, false);
    }

    doc.complete = (doc.skippedLayers == 0);
    return doc;
}

}  // namespace af_detail
}  // namespace pittore::io

namespace pittore::io {

std::optional<AfLayersDoc> afDecodeLayers(const std::vector<std::uint8_t>& data,
                                          std::string* error, const std::string& baseDir) {
    using namespace af_detail;
    if (!data.empty() && !(data[0] == 0x00 && data.size() >= 4 && data[1] == 0xff &&
                           data[2] == 0x4b && data[3] == 0x41)) {
        if (error) *error = "not an Affinity archive (bad magic)";
        return std::nullopt;
    }
    try {
        const Archive ar = Archive::parse(data);
        const ArchiveEntry* docEntry = ar.head("doc.dat");
        if (!docEntry) {
            if (error) *error = "container has no doc.dat";
            return std::nullopt;
        }
        const std::vector<std::uint8_t> docBytes = ar.extract(*docEntry);
        const Graph g = parseGraph(docBytes);
        return buildDocument(ar, g, baseDir);
    } catch (const AfError& e) {
        if (error) *error = e.msg;
        return std::nullopt;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

}  // namespace pittore::io
