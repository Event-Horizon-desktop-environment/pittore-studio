#include "engine/io/af_layers.h"
#include "engine/io/af.h"
#include "engine/core/parallel.h"
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


Mat Walker::nodeCtm(const Node* node, const Mat& parent) const {
    const std::vector<double>* xf = f64s(g, node, "Xfrm");
    if (xf && xf->size() >= 6)
        return matThen(parent, Mat{{(*xf)[0], (*xf)[1], (*xf)[2], (*xf)[3], (*xf)[4], (*xf)[5]}});
    return parent;
}

std::optional<Bitmap> Walker::sourceImage(const Node* bitm, std::uint32_t w, std::uint32_t h,
                                  std::string& why) {
    const Value* bckg = g.field(bitm, "Bckg");
    if (!bckg || bckg->k != Value::K::Embedded) {
        why = "source-backed bitmap has no Bckg entry";
        return std::nullopt;
    }
    const std::string& name = bckg->s;
    auto cached = sourceCache.find(name);
    if (cached != sourceCache.end()) {
        if (!cached->second) {
            why = "cached source decode failure";
            return std::nullopt;
        }
        if (cached->second->w != w || cached->second->h != h) {
            why = "source size mismatch with bitmap dims";
            return std::nullopt;
        }
        return cached->second;
    }
    std::optional<Bitmap> result;
    const ArchiveEntry* e = ar.head(name);
    if (!e) {
        why = "missing source entry " + name;
    } else {
        try {
            std::vector<std::uint8_t> data = ar.extract(*e);
            Graph sg = parseGraph(data);
            Node* root = sg.root();
            const std::vector<std::uint8_t>* file = nullptr;
            if (root)
                for (auto& f : root->fields)
                    if (f.second.k == Value::K::Blob) {
                        file = &f.second.bytes;
                        break;
                    }
            Bitmap img;
            std::string derr;
            if (!file) {
                // No embedded bytes: this is a "Link" entry, which keeps
                // only the external file's path. Resolve it against the
                // Wine/Linux mapping of the recorded path and the .af's own
                // directory; decode the file the same way as an embedded one.
                const std::string link = findLinkPath(sg);
                std::optional<std::string> resolved;
                if (!link.empty()) resolved = resolveLinkedPath(link, baseDir);
                if (!resolved) {
                    why = "source entry '" + name + "' has no Data blob";
                    if (!link.empty()) why += " (linked file not found: " + link + ")";
                } else if (!readFileBytes(*resolved, data)) {
                    why = "cannot read linked file " + *resolved;
                } else if (!decodeImageBytes(data, img, derr)) {
                    why = "linked file '" + *resolved + "': " + derr;
                } else if (img.w != w || img.h != h) {
                    why = "linked image is " + std::to_string(img.w) + "x" +
                          std::to_string(img.h) + ", bitmap says " + std::to_string(w) + "x" +
                          std::to_string(h);
                } else {
                    result = std::move(img);
                    doc.log.push_back("linked source '" + name + "' <- " + *resolved);
                }
            } else if (!decodeImageBytes(*file, img, derr)) {
                why = derr;
            } else if (img.w != w || img.h != h) {
                why = "source image is " + std::to_string(img.w) + "x" +
                      std::to_string(img.h) + ", bitmap says " + std::to_string(w) + "x" +
                      std::to_string(h);
            } else {
                result = std::move(img);
            }
        } catch (const AfError& err) {
            why = err.msg;
        }
    }
    sourceCache[name] = result;
    return result;
}

std::optional<std::vector<std::uint8_t>> Walker::loadPlane(const Node* bitm, const char* sta,
                                                   const char* idx, std::size_t gridWidth,
                                                   std::size_t pitch, std::size_t rows,
                                                   std::size_t height, std::size_t bps,
                                                   const Bitmap* source, std::size_t channel,
                                                   std::string& why) {
    std::vector<std::uint8_t> statuses;
    const Value* st = g.field(bitm, sta);
    if (st && st->k == Value::K::Array)
        for (const Value& v : st->arr)
            if (v.k == Value::K::U8) statuses.push_back(static_cast<std::uint8_t>(v.u));
    std::vector<Node*> blocks = g.children(bitm, idx);
    std::size_t nextBlock = 0;
    std::vector<std::uint8_t> plane(pitch * rows, 0);
    const auto places = tileOffsets(gridWidth, height);
    struct Stored {
        std::size_t x, y;
        std::array<int, 4> rect;
        std::string name;
    };
    std::vector<Stored> stored;
    const std::size_t np = std::min(statuses.size(), places.size());
    for (std::size_t i = 0; i < np; ++i) {
        const std::uint8_t status = statuses[i];
        const auto [x, y] = places[i];
        if (status <= 1) continue;
        if (status == 2) {
            fillTile(plane, pitch, x, y, {0xff});
            continue;
        }
        if (status == 3) {
            fillTile(plane, pitch, x, y, {0x00, 0x00, 0x80, 0x3f});
            continue;
        }
        if (status == 4) {
            if (nextBlock >= blocks.size()) {
                why = "more stored tiles than blocks";
                return std::nullopt;
            }
            Node* block = blocks[nextBlock++];
            std::array<int, 4> rect = {0, 0, 256, 256};
            const Value* r = g.field(block, "Rect");
            if (!r) r = g.field(block, "IRct");
            if (r && r->k == Value::K::VecI && r->vi.size() == 4)
                rect = {r->vi[0], r->vi[1], r->vi[2], r->vi[3]};
            const Value* d = g.field(block, "Data");
            if (!d || d->k != Value::K::Embedded) {
                why = "stored block has no data reference";
                return std::nullopt;
            }
            stored.push_back({x, y, rect, d->s});
            continue;
        }
        if (status == 5) {
            if (!source || channel >= 4) {
                why = "source-backed tile without a decodable source";
                return std::nullopt;
            }
            copySourceTile(plane, pitch, rows, bps, *source, channel, x, y);
            continue;
        }
        why = "unknown tile status " + std::to_string(status);
        return std::nullopt;
    }
    for (const Stored& s : stored) {
        const ArchiveEntry* e = ar.head(s.name);
        if (!e) {
            why = "missing tile entry " + s.name;
            return std::nullopt;
        }
        std::vector<std::uint8_t> plain;
        try {
            plain = ar.extract(*e);
        } catch (const AfError& err) {
            why = err.msg;
            return std::nullopt;
        }
        auto tile = tilePayload(std::move(plain));
        if (!tile) {
            why = "tile " + s.name + " has no 64 KiB payload";
            return std::nullopt;
        }
        const std::size_t x0 = static_cast<std::size_t>(std::clamp(s.rect[0], 0, 256));
        const std::size_t y0 = static_cast<std::size_t>(std::clamp(s.rect[1], 0, 256));
        const std::size_t x1 = static_cast<std::size_t>(std::clamp(s.rect[2], 0, 256));
        const std::size_t y1 = static_cast<std::size_t>(std::clamp(s.rect[3], 0, 256));
        for (std::size_t ty = y0; ty < y1; ++ty) {
            if (s.y + ty >= rows) break;
            const std::size_t dst = (s.y + ty) * pitch + s.x + x0;
            const std::size_t src = ty * 256 + x0;
            const std::size_t n = std::min(x1 - x0, pitch - (s.x + x0));
            std::memcpy(&plane[dst], &(*tile)[src], n);
        }
    }
    return plane;
}

std::optional<Bitmap> Walker::decodeBitmap(const Node* bitm, std::string& why) {
    const auto frmt = enumOf(g, bitm, "Frmt");
    if (!frmt) {
        why = "bitmap has no format";
        return std::nullopt;
    }
    const auto fmt = formatOf(*frmt);
    if (!fmt) {
        why = "unknown pixel format " + std::to_string(*frmt);
        return std::nullopt;
    }
    const std::int32_t wi = i32Of(g, bitm, "BmpW", 0);
    const std::int32_t hi = i32Of(g, bitm, "BmpH", 0);
    if (wi <= 0 || hi <= 0 || wi > (1 << 20) || hi > (1 << 20)) {
        why = "implausible bitmap dims " + std::to_string(wi) + "x" + std::to_string(hi);
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(wi) * hi > kMaxPixels) {
        why = "bitmap over the pixel cap";
        return std::nullopt;
    }
    const std::size_t width = wi, height = hi;
    const std::size_t rowBytes = width * fmt->bps;
    const std::size_t pitch = ((rowBytes + 255) / 256) * 256;
    const std::size_t rows = ((height + 255) / 256) * 256;

    const std::vector<std::uint8_t> statuses = allStatuses(g, bitm);
    if (statuses.empty() && g.field(bitm, "Bckg") != nullptr)
        return sourceImage(bitm, static_cast<std::uint32_t>(width),
                           static_cast<std::uint32_t>(height), why);
    std::optional<Bitmap> source;
    if (std::find(statuses.begin(), statuses.end(), std::uint8_t(5)) != statuses.end()) {
        source = sourceImage(bitm, static_cast<std::uint32_t>(width),
                             static_cast<std::uint32_t>(height), why);
        if (!source) return std::nullopt;
    }

    static const char* kSta[5] = {"Sta1", "Sta2", "Sta3", "Sta4", "Sta5"};
    static const char* kIdx[5] = {"Idx1", "Idx2", "Idx3", "Idx4", "Idx5"};
    static const char* kTwi[5] = {"TWi1", "TWi2", "TWi3", "TWi4", "TWi5"};
    std::vector<std::vector<std::uint8_t>> planes;
    for (int ch = 0; ch < fmt->channels; ++ch) {
        const std::int32_t tw = i32Of(g, bitm, kTwi[ch], 0);
        const std::size_t gridWidth = tw > 0 ? std::size_t(tw) : (rowBytes + 255) / 256;
        auto plane = loadPlane(bitm, kSta[ch], kIdx[ch], gridWidth, pitch, rows, height,
                               std::size_t(fmt->bps), source ? &*source : nullptr,
                               std::size_t(ch), why);
        if (!plane) return std::nullopt;
        planes.push_back(std::move(*plane));
    }
    return interleave(planes, pitch, width, height, std::size_t(fmt->bps), fmt->kind);
}

void Walker::noteSkip(const Node* node, const std::string& why) {
    std::string display = strOf(g, node, "Desc");
    if (display.empty()) display = tagName(g.typeTag(node));
    doc.log.push_back("skip " + display + " (" + tagName(g.typeTag(node)) + "): " + why);
    ++doc.skippedLayers;
}

void Walker::applyCommon(const Node* node, AfLayer& layer) {
    if (const auto v = boolOf(g, node, "Visi")) layer.visible = *v;
    if (const auto o = f32Of(g, node, "Opac"))
        layer.opacity = static_cast<std::uint16_t>(std::clamp(*o, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (layer.isGroup) {
        const auto p = boolOf(g, node, "PasT");
        if (p && !*p) layer.blend = "Normal";
    }
    if (const auto b = enumVerOf(g, node, "Blnd"))
        layer.blend = blendName(b->first, b->second);
}

    // Place the "MRst" masks hanging off `node`'s "AdCh" list on the canvas.
    // A mask lives in the layer's own space, so it is placed through the
    // layer's transform as well as its own, exactly like a clipped child.
    // Several masks multiply, as the format stacks them; only visible ones join
    // unless none is visible, in which case the first still does.
std::vector<AfMask> Walker::cropMasks(const Node* node, const Mat& base,
                                    const IntRect* pad) {
    using namespace pittore::vector;
    std::vector<Node*> crops;
    for (Node* ch : g.children(node, "AdCh")) {
        const std::uint32_t k = g.typeTag(ch);
        if (k == tag4("ShpN") || k == tag4("PCrv")) crops.push_back(ch);
    }
    if (crops.empty()) return {};
    std::vector<Node*> used;
    for (Node* c : crops)
        if (boolOf(g, c, "Visi").value_or(true)) used.push_back(c);
    if (used.empty()) used.push_back(crops[0]);
    std::vector<AfMask> out;
    for (Node* c : used) {
        const std::uint32_t k = g.typeTag(c);
        const Mat lctm = nodeCtm(c, base);
        VectorPath path;
        bool evenOdd = true;
        std::string why;
        if (k == tag4("ShpN")) {
            Node* shpe = g.child(c, "Shpe");
            const std::vector<double>* b = f64s(g, c, "ShpB");
            if (!shpe || !b || b->size() < 4) {
                why = "crop shape has no geometry";
            } else {
                const float x0 = static_cast<float>(std::min((*b)[0], (*b)[2]));
                const float y0 = static_cast<float>(std::min((*b)[1], (*b)[3]));
                const float x1 = static_cast<float>(std::max((*b)[0], (*b)[2]));
                const float y1 = static_cast<float>(std::max((*b)[1], (*b)[3]));
                if (auto geo = shapeGeometry(g, shpe, x0, y0, x1, y1)) {
                    evenOdd = geo->subpaths.size() > 1;
                    for (auto& sp : geo->subpaths)
                        path.subpaths.push_back(SubPath{std::move(sp.first), sp.second});
                    transformAnchors(path, lctm);
                } else {
                    why = "crop shape geometry is not supported";
                }
            }
        } else if (auto p = pathFromCurveRecords(g, c, lctm)) {
            path = std::move(*p);
        } else {
            why = "crop path has no curve data";
        }
        if (path.isEmpty()) {
            std::string display = strOf(g, node, "Desc");
            if (display.empty()) display = tagName(g.typeTag(node));
            doc.log.push_back("skip crop of '" + display + "': " +
                              (why.empty() ? "no geometry" : why));
            ++doc.skippedLayers;
            continue;
        }
        VectorShape shape;
        shape.path = std::move(path);
        shape.fill = {255, 255, 255, 255};
        shape.evenOdd = evenOdd;
        auto img = rasterizeShape(shape, nullptr);
        if (!img) {
            std::string display = strOf(g, node, "Desc");
            if (display.empty()) display = tagName(g.typeTag(node));
            doc.log.push_back("skip crop of '" + display + "': raster is empty or too large");
            ++doc.skippedLayers;
            continue;
        }
        AfMask mb;
        mb.left = img->rect.left;
        mb.top = img->rect.top;
        mb.width = static_cast<std::uint32_t>(img->rect.width());
        mb.height = static_cast<std::uint32_t>(img->rect.height());
        mb.px.resize(static_cast<std::size_t>(mb.width) * mb.height);
        for (std::size_t i = 0, n = mb.px.size(); i < n; ++i)
            mb.px[i] = img->rgba[i * 4 + 3];
        // Masks reveal outside their own rect, so widen to the photo's
        // footprint: covered inside the frame, hidden past it.
        if (pad && !pad->empty() && pad->width() > 0 && pad->height() > 0 &&
            (pad->x0 != mb.left || pad->y0 != mb.top ||
             pad->width() != static_cast<int>(mb.width) ||
             pad->height() != static_cast<int>(mb.height))) {
            const std::uint32_t pw = static_cast<std::uint32_t>(pad->width());
            const std::uint32_t ph = static_cast<std::uint32_t>(pad->height());
            if (std::uint64_t(pw) * ph <= (1ull << 28)) {
                std::vector<std::uint8_t> full(static_cast<std::size_t>(pw) * ph, 0);
                const int ox0 = std::max(mb.left, pad->x0);
                const int oy0 = std::max(mb.top, pad->y0);
                const int ox1 =
                    std::min(mb.left + static_cast<int>(mb.width), pad->x1);
                const int oy1 =
                    std::min(mb.top + static_cast<int>(mb.height), pad->y1);
                for (int y = oy0; y < oy1; ++y)
                    std::memcpy(&full[static_cast<std::size_t>(y - pad->y0) * pw +
                                       static_cast<std::size_t>(ox0 - pad->x0)],
                                &mb.px[static_cast<std::size_t>(y - mb.top) * mb.width +
                                        static_cast<std::size_t>(ox0 - mb.left)],
                                static_cast<std::size_t>(ox1 - ox0));
                mb.left = pad->x0;
                mb.top = pad->y0;
                mb.width = pw;
                mb.height = ph;
                mb.px = std::move(full);
            }
        }
        out.push_back(std::move(mb));
    }
    if (!out.empty()) {
        std::string display = strOf(g, node, "Desc");
        if (display.empty()) display = tagName(g.typeTag(node));
        doc.log.push_back("crop '" + display + "': " + std::to_string(out.size()) +
                          (out.size() == 1 ? " mask" : " masks"));
    }
    return out;
}

std::vector<AfMask> Walker::placeMasks(const Node* node, const Mat& ctm,
                                     const IntRect* pad) {
    std::vector<Node*> masks;
    for (Node* ch : g.children(node, "AdCh"))
        if (g.typeTag(ch) == tag4("MRst")) masks.push_back(ch);
    std::vector<AfMask> out;
    const Mat base = nodeCtm(node, ctm);
    // Vector-crop shapes ("ShpN") and free paths ("PCrv") nested in a
    // layer's `AdCh` list: Affinity's vector crop. Rebuild the outline
    // exactly like the shape emitter (same geometry helpers), rasterize
    // its coverage, and keep it as a live mask — the photo shows cropped
    // but stays editable. Invisible crops are skipped unless every crop
    // is invisible (same policy as raster masks). Runs before the raster
    // early-out below so crops apply with no MRst present.
    for (const AfMask& cm : cropMasks(node, base, pad)) out.push_back(cm);
    if (masks.empty()) return out;
    std::vector<Node*> used;
    if (masks.size() == 1) {
        used.push_back(masks[0]);
    } else {
        for (Node* m : masks)
            if (boolOf(g, m, "Visi").value_or(true)) used.push_back(m);
        if (used.empty()) used.push_back(masks[0]);
    }
    for (Node* m : used) {
        Node* bitm = g.child(m, "Bitm");
        std::string why = "mask has no bitmap";
        if (bitm) {
            auto img = decodeBitmap(bitm, why);
            if (img) {
                auto p = placeRaster(nodeCtm(m, base), std::move(*img));
                if (p) {
                    AfMask mb;
                    mb.left = p->rect.x0;
                    mb.top = p->rect.y0;
                    mb.width = static_cast<std::uint32_t>(p->rect.width());
                    mb.height =
                        static_cast<std::uint32_t>(p->rect.height());
                    const std::size_t n =
                        static_cast<std::size_t>(mb.width) * mb.height;
                    mb.px.resize(n);
                    for (std::size_t i = 0; i < n; ++i) mb.px[i] = p->img.px[i * 4];
                    out.push_back(std::move(mb));
                    continue;
                }
                why = "mask placement failed";
            }
        }
        std::string display = strOf(g, node, "Desc");
        if (display.empty()) display = tagName(g.typeTag(node));
        doc.log.push_back("skip mask of '" + display + "': " + why);
        ++doc.skippedLayers;
    }
    if (!out.empty()) {
        std::string display = strOf(g, node, "Desc");
        if (display.empty()) display = tagName(g.typeTag(node));
        doc.log.push_back("mask '" + display + "': " + std::to_string(out.size()) +
                          (out.size() == 1 ? " mask" : " masks"));
    }
    return out;
}

    // Bake a layer's effects into its pixels, keep its masks live, and push it.
    // `img`/`rect` hold the layer's straight-alpha RGBA8 and its document-space
    // footprint; effects grow both together.
void Walker::finishImageLayer(const Node* node, const Mat& ctm, int indent, bool clipped,
                      const std::string& display, const std::string& irfn,
                      render::Rgba8Image img, IntRect rect, bool isText,
                      const TextPayload* text,
                      std::shared_ptr<const pittore::vector::ArtNode> art) {
    std::vector<std::string> fxSkips;
    const render::LayerStyle fx = fxParse(g, node, nodeCtm(node, ctm), fxSkips);
    // Retained vector geometry only describes what the decoder kept: once
    // an effect or mask alters the pixels, the paths alone would export a
    // different picture, so the layer stays raster.
    bool pixelsAltered = !fx.empty() || !fxSkips.empty();
    for (const std::string& s : fxSkips) {
        doc.log.push_back("skip effects of '" + display + "': " + s);
        ++doc.skippedLayers;
    }
    if (!fx.empty()) {
        int grow = 0;
        if (render::applyLayerStyle(img, fx, &grow)) {
            rect.x0 -= grow;
            rect.y0 -= grow;
            rect.x1 += grow;
            rect.y1 += grow;
            doc.log.push_back("effects '" + display + "' applied");
        } else {
            doc.log.push_back("skip effects of '" + display + "': layer is too large");
            ++doc.skippedLayers;
        }
    }
    AfLayer layer;
    layer.left = rect.x0;
    layer.top = rect.y0;
    layer.width = static_cast<std::uint32_t>(rect.width());
    layer.height = static_cast<std::uint32_t>(rect.height());
    layer.indent = indent;
    layer.clipped = clipped;
    layer.name = display;
    layer.sourcePath = irfn;
    layer.isText = isText;
    if (text) {
        layer.hasText = true;
        layer.textSpec = text->spec;
        layer.textOriginX = text->originX;
        layer.textOriginY = text->originY;
        layer.textFrameHeight = text->frameHeight;
        for (int i = 0; i < 4; ++i)
            layer.textColor[i] = static_cast<float>(text->color[i]) / 255.0f;
    }
    layer.rgba.resize(img.px.size());
    // Widening pass over the whole layer: each i writes only its own slot.
    pittore::core::parallel_for(static_cast<std::uint32_t>(img.px.size()), 4096,
                                [&](std::uint32_t i0, std::uint32_t i1) {
                                    for (std::size_t i = i0; i < i1; ++i)
                                        layer.rgba[i] =
                                            static_cast<std::uint16_t>(img.px[i]) * 257u;
                                });
    applyCommon(node, layer);
    doc.log.push_back("layer '" + layer.name + "' " + std::to_string(layer.width) + "x" +
                      std::to_string(layer.height) + " @ " + std::to_string(layer.left) + "," +
                      std::to_string(layer.top));
    // Group masks stay live on their group rows; this layer keeps its own
    // masks live instead of baking any of them into alpha. Either kind of
    // mask means vector geometry alone no longer reproduces the layer.
    layer.masks = placeMasks(node, ctm, &rect);
    if (maskedAncestors > 0 || !layer.masks.empty()) pixelsAltered = true;
    if (art && !pixelsAltered) layer.art = std::move(art);
    doc.layers.push_back(std::move(layer));
    ++doc.frameCount;
    const Mat childCtm = nodeCtm(node, ctm);
    for (Node* ch : g.children(node, "Chld")) emit(ch, childCtm, indent, true);
}
}  // namespace af_detail
}  // namespace pittore::io
