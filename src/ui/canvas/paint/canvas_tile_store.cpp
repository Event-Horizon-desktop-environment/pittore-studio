// Tiled crisp overlay with background baking. Paint/blits run on the GUI
// thread; tile rasterization runs on the global thread pool from immutable
// geometry snapshots (shared vector nodes plus plain placement values), so
// a bake racing an edit can only miss its revision pin and be discarded.
#include "ui/canvas/paint/canvas_tile_store.h"

#include <QImage>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QPainter>
#include <QPaintDevice>
#include <QPainterPath>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/canvas/paint/canvas_crisp.h"
#include "ui/persona/vector_raster.h"
#include "engine/core/log.h"
#include "engine/vector/vector_art.h"

namespace pittore::ui {

// Full tile payload shared with the header's completion entry point.
struct BakedTile {
    const DocumentItem* doc = nullptr;
    int tx = 0;
    int ty = 0;
    quint64 revision = 0;
    double bucket = 0.0;
    // View density this tile was queued for, in device px per doc px. The
    // bucket picks the grid; this picks the raster, so the bake lands 1:1 on
    // the frame that asked for it instead of on a quarter-quantized stand-in
    // that the draw would then have to rescale (a resample of even a percent
    // costs about a quarter of the region's edge energy).
    double density = 0.0;
    QRectF docRect;
    QImage img;
    QImage comp;      // composite snapshot over docRect at doc resolution:
                      // Tier-3 layers blit from here in stack order, so the
                      // tile matches the composite exactly where it is soft.
    QRect compRect;   // composite pixels backing comp (doc coords)
    qint64 bakeMs = 0;  // worker raster time, for slow-bake logging
    // What the worker actually painted, for the tile-bake line. A tile whose
    // crisp draws are all buried under a composite blit above them is the
    // "tiles never sharpen" failure showing up as data instead of an
    // impression.
    int drawnPaths = 0;   // crisp draws that produced geometry
    int buriedPaths = 0;  // ... fully covered by a composite blit above them
    int emptyPaths = 0;   // geometry that produced nothing to draw
    int compBlits = 0;    // composite blits this tile
    int inkPct = 0;       // sampled share of the tile that is not empty
    // Geometry shared by every tile queued in one frame: each node's fill
    // and stroke paths are built once no matter how many tiles cover it.
    // Frame-local (dies with the jobs), so no invalidation bookkeeping.
    struct PathCache {
        std::mutex mutex;
        std::unordered_map<const pittore::vector::ArtNode*, CrispPaths> paths;
    };
    std::shared_ptr<PathCache> pathCache;
    // Converted image bitmaps, shared by a frame's tiles: float sources
    // convert once per frame, not once per tile they touch.
    struct ImgCache {
        std::mutex mutex;
        std::unordered_map<const pittore::Image*, QImage> imgs;
    };
    std::shared_ptr<ImgCache> imgCache;
    struct Layer {
        std::shared_ptr<const pittore::vector::ArtNode> art;
        double scaleX = 1.0;
        double scaleY = 1.0;
        double offX = 0.0;
        double offY = 0.0;
        QRectF box;
        // True: blit box from comp instead of painting art (anything the
        // crisp recipe cannot reproduce exactly).
        bool useComposite = false;
        // Source bitmap for the simple-blit tier (plain images): pinned
        // here so the baker never touches live layer state.
        std::shared_ptr<const pittore::Image> pixels;
        // True: blit pixels (resampled to view density) instead of the
        // composite snapshot, so vector art below stays sharp.
        bool usePixels = false;
        // Panel index in doc.layers, snapshotted with the plan: lets the
        // tile-trace lines name the layer the baker can no longer see.
        int index = -1;
    };
    std::vector<Layer> layers;
};

namespace {

// Document pixels per tile side. Fixed at this until its raster would reach
// kMaxTilePx; past that the tile shrinks in document space so its density
// keeps tracking the zoom (see tileDocPxFor) instead of hitting a resolution
// ceiling.
constexpr int kTileDocPx = 256;
// Cap on the tile raster edge: at most kMaxTilePx^2 x 4 bytes per tile, and
// only the tiles covering the view are alive, so memory stays bounded at any
// zoom while the density tracks the view (the bake clamps to this cap).
constexpr int kMaxTilePx = 2048;

// Document pixels per tile side for a zoom bucket: the fixed grid while the
// raster fits the pixel cap, shrinking with the bucket once it would not.
// Deep zoom therefore gets a smaller tile rather than a resolution ceiling -
// the tile's image edge is min(kTileDocPx * bucket, kMaxTilePx) at the view's
// own density, so a deep bake is affordable instead of refused.
double tileDocPxFor(double bucket) {
    const double b =
        std::isfinite(bucket) && bucket > 0.0 ? bucket : 1.0;
    return std::min((double)kTileDocPx, (double)kMaxTilePx / b);
}
// Store budget and bookkeeping caps.
constexpr qint64 kTileBudgetBytes = 192ll * 1024 * 1024;
constexpr std::size_t kMaxTiles = 384;
// In-flight + queued bakes per document: pan storms re-queue every frame,
// so excess demand is dropped (next frame re-queues what is still needed).
constexpr int kMaxPendingPerDoc = 64;
// Revision bookkeeping is per painted document; documents are few, this
// only bounds pathological churn.
constexpr std::size_t kMaxKnownDocs = 64;
// How far a baked tile's density may sit from the view and still be drawn:
// the draw maps raster to device within this band, and nearest on a stretch
// that small repeats at most one column per fifty - no visible edge cost.
// Outside it the raster would have to be rescaled, and the resample (not the
// raster) is what read as soft, so the tile counts as missing and re-bakes at
// the view's own density. Wheel steps move further than this, so at rest it
// never fires and during a pinch it costs one bake per couple of percent.
constexpr double kDensityTol = 0.02;

// True when a tile baked at `baked` still lands on a view at `view`.
bool densityClose(double baked, double view) {
    return baked > 0.0 && std::abs(baked - view) <= kDensityTol * view;
}

// Draw a baked tile so its raster lands on the device pixels it was made
// for. Destination edges are rounded in device space and neighbours share
// the same rounded edge, so tiles abut exactly with no seam and no gap; the
// raster then maps 1:1 and needs no resample at all. This is the difference
// between "crisp" and "soft": the tile's destination used to be fractional
// (doc rect x view), and a smoothing resample through a fractional
// destination costs about a quarter of the region's edge energy even when
// the scale error is under a percent - which is why every zoom that was not
// a clean quarter came back softer than the quarters the tests sat on.
// Nearest is used only inside that near-1:1 band; a tile drawn far off its
// density (a stale placeholder) keeps the smoothing transform.
void drawTileImage(QPainter& p, const QRectF& docRect, const QImage& img) {
    if (img.isNull() || docRect.isEmpty()) return;
    const QTransform w = p.worldTransform();
    // Rotated or skewed views are not a canvas mode: keep the transformed
    // blit rather than guess at an axis-aligned edge.
    if (w.m12() != 0.0 || w.m21() != 0.0 || w.m13() != 0.0 || w.m23() != 0.0 ||
        w.m33() != 1.0) {
        p.drawImage(docRect, img);
        return;
    }
    int x0 = qRound(w.dx() + docRect.left() * w.m11());
    int x1 = qRound(w.dx() + docRect.right() * w.m11());
    int y0 = qRound(w.dy() + docRect.top() * w.m22());
    int y1 = qRound(w.dy() + docRect.bottom() * w.m22());
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (x1 <= x0 || y1 <= y0) return;  // thinner than the device grid
    const QRect dest(x0, y0, x1 - x0, y1 - y0);
    const double rx = double(dest.width()) / img.width();
    const double ry = double(dest.height()) / img.height();
    // The band is exactly what the readiness rule can hand us: a tile baked
    // within kDensityTol of the view maps at view/density, which lands in
    // [1/(1+tol), 1/(1-tol)] - so a tile that counts as ready is always
    // drawn in this band, never rescaled on the way to the screen.
    const double lo = 1.0 / (1.0 + kDensityTol);
    const double hi = 1.0 / (1.0 - kDensityTol);
    const bool nearOne =
        rx >= lo && rx <= hi && ry >= lo && ry <= hi;
    const bool smooth =
        p.renderHints().testFlag(QPainter::SmoothPixmapTransform);
    if (nearOne) p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setWorldTransform(QTransform());
    p.drawImage(dest, img);
    p.setWorldTransform(w);
    if (nearOne) p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
}

// Tile-trace helpers (defined with the plan below): whether a layer box
// warrants a trace line under PITTORE_TILE_TRACE[_BOX].
bool tileTraceWanted(const QRectF& box);

// Source conversion (defined with the plan below).
QImage bakedSourceImage(BakedTile::ImgCache* cache,
                        const pittore::Image* src);

class TileJob : public QRunnable {
public:
    TileJob(BakedTile tile, QObject* notifier)
        : tile_(std::move(tile)), notifier_(notifier) {
        setAutoDelete(true);
    }
    void run() override {
        // Superseded while queued (the zoom step moved on): abort before
        // spending 100-1000 ms rasterizing a bucket nobody will draw.
        // Fast scroll would otherwise flood the pool with dead bakes and
        // starve the one the current frame actually wants.
        if (!CanvasTileStore::instance().stillWanted(tile_)) return;
        QElapsedTimer bakeT;
        bakeT.start();
        // Raster at the density the frame asked for, not at the bucket: the
        // grid comes from the bucket, but the pixels must land on the view
        // or the draw would have to rescale them. Clamped to the pixel cap
        // so deep zoom still buys a smaller tile rather than a bigger one.
        const double ze =
            tile_.density > 0.0
                ? std::min(tile_.density,
                           (double)kMaxTilePx / tile_.docRect.width())
                : tile_.bucket;
        const int iw = qMax(1, qRound(tile_.docRect.width() * ze));
        const int ih = qMax(1, qRound(tile_.docRect.height() * ze));
        QImage img(iw, ih, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        // Label the crisp draws a composite blit stacked above them fully
        // covers: those are the ones that cannot sharpen. Comp blits are few,
        // so one reverse walk over the plan is enough to mark the draws.
        std::vector<char> covered(tile_.layers.size(), 0);
        {
            std::vector<QRectF> above;
            above.reserve(8);
            for (int i = int(tile_.layers.size()) - 1; i >= 0; --i) {
                const BakedTile::Layer& l = tile_.layers[i];
                if (l.useComposite) {
                    const QRectF hit = l.box.intersected(tile_.docRect);
                    if (!hit.isEmpty()) above.push_back(hit);
                    continue;
                }
                if (above.empty()) continue;
                const QRectF hit = l.box.intersected(tile_.docRect);
                if (hit.isEmpty()) continue;
                for (const QRectF& b : above) {
                    if (b.contains(hit)) {
                        covered[i] = 1;
                        break;
                    }
                }
            }
        }
        int drawn = 0, empty = 0, comp = 0, coveredPaths = 0;
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setTransform(QTransform(ze, 0, 0, ze,
                                     -tile_.docRect.x() * ze,
                                     -tile_.docRect.y() * ze),
                           false);
            for (int i = 0; i < int(tile_.layers.size()); ++i) {
                const BakedTile::Layer& l = tile_.layers[i];
                if (!l.box.intersects(tile_.docRect)) continue;
                if (l.useComposite) {
                    if (tile_.comp.isNull()) continue;
                    const QRectF hit =
                        l.box.intersected(tile_.docRect);
                    // Composite snapshot is doc-resolution over compRect:
                    // source pixels are doc coords minus its origin.
                    const QRectF src =
                        hit.translated(-tile_.compRect.x(),
                                       -tile_.compRect.y());
                    p.drawImage(hit, tile_.comp, src);
                    ++comp;
                    // Which blit buried which crisp draws is the "my vector
                    // art stays soft" answer: the aggregate cannot say it.
                    if (tileTraceWanted(l.box))
                        ::pittore::core::log::log_info(
                            "[render][tile-bake] comp-blit i=%d "
                            "box=%.1fx%.1f@%.0f,%.0f",
                            l.index, l.box.width(), l.box.height(),
                            l.box.x(), l.box.y());
                    continue;
                }
                if (l.usePixels) {
                    // Own-bitmap blit at view density: same placement the
                    // composite used, resampled by the tile transform, so
                    // art below stays at full density underneath.
                    // Deliberately not a burial source: translucent pixels
                    // show the crisp below through, and opaque ones cover
                    // exactly what the plan already drew.
                    QImage src =
                        bakedSourceImage(tile_.imgCache.get(), l.pixels.get());
                    if (src.isNull()) continue;
                    p.save();
                    p.setTransform(QTransform()
                                       .scale(l.scaleX, l.scaleY)
                                       .translate(l.offX, l.offY) *
                                   p.transform(),
                                   false);
                    p.drawImage(QRectF(0, 0, src.width(), src.height()), src);
                    p.restore();
                    continue;
                }
                if (!l.art) continue;
                // Shared geometry: first tile to reach a node builds its
                // paths, the rest copy the implicitly-shared result.
                CrispPaths paths;
                bool havePaths = false;
                if (tile_.pathCache) {
                    std::lock_guard<std::mutex> lk(tile_.pathCache->mutex);
                    const auto it =
                        tile_.pathCache->paths.find(l.art.get());
                    if (it != tile_.pathCache->paths.end()) {
                        paths = it->second;
                        havePaths = true;
                    }
                }
                if (!havePaths) {
                    paths = buildCrispPaths(*l.art);
                    if (tile_.pathCache) {
                        std::lock_guard<std::mutex> lk(
                            tile_.pathCache->mutex);
                        tile_.pathCache->paths[l.art.get()] = paths;
                    }
                }
                if (drawCrispPaths(p, paths, *l.art, l.scaleX, l.scaleY,
                                   l.offX, l.offY)) {
                    ++drawn;
                    coveredPaths += covered[i];
                    // A crisp draw fully covered by a composite blit above
                    // it paints pixels the blit then replaces: the layer
                    // reads soft at every zoom despite going crisp. The
                    // aggregate buried count cannot name it; this can.
                    if (covered[i] && tileTraceWanted(l.box))
                        ::pittore::core::log::log_info(
                            "[render][tile-bake] buried i=%d", l.index);
                } else {
                    ++empty;
                    // Geometry that rasterized to nothing: wrong box, empty
                    // path, or a recipe gap. Silent without the trace.
                    if (tileTraceWanted(l.box))
                        ::pittore::core::log::log_info(
                            "[render][tile-bake] empty i=%d", l.index);
                }
            }
        }
        // Sampled share of the tile that actually has paint: a tile that
        // drew thousands of paths and comes back empty says the recipe put
        // nothing on the raster.
        {
            long hit = 0, seen = 0;
            for (int y = 0; y < ih; y += 3) {
                const QRgb* row =
                    reinterpret_cast<const QRgb*>(img.constScanLine(y));
                for (int x = 0; x < iw; x += 3) {
                    hit += ((row[x] >> 24) & 255) != 0;
                    ++seen;
                }
            }
            tile_.inkPct = seen ? int(100.0 * hit / seen + 0.5) : 0;
        }
        tile_.drawnPaths = drawn;
        tile_.buriedPaths = coveredPaths;
        tile_.emptyPaths = empty;
        tile_.compBlits = comp;
        tile_.img = std::move(img);
        tile_.bakeMs = bakeT.elapsed();
        BakedTile done = std::move(tile_);
        QMetaObject::invokeMethod(
            notifier_, [done = std::move(done)]() mutable {
                CanvasTileStore::instance().completeTile(std::move(done));
            },
            Qt::QueuedConnection);
    }

private:
    BakedTile tile_;
    QObject* notifier_ = nullptr;
};

// Receiver for baker completion callbacks; created on first use, so in the
// app it lives on the GUI thread and completions queue there.
QObject* tileNotifier() {
    static QObject* obj = new QObject();
    return obj;
}

// True when the crisp recipe reproduces this layer's paint exactly.
bool crispReproducible(const LayerItem& l) {
    if (!l.art) return false;
    const auto& pt = l.art->paint;
    return pt.patternId.empty() && pt.markerStart.empty() &&
           pt.markerMid.empty() && pt.markerEnd.empty() &&
           pt.clipId.empty() && !pt.hasMesh && !pt.hasFilter;
}

// Why a Pixel layer missed the crisp tier: nullptr means reproducible.
// Mirrors crispReproducible branch for branch, so the trace names the exact
// veto instead of leaving "comp" as the whole story.
const char* crispVetoReason(const LayerItem& l) {
    if (!l.art) return "no-art";
    const auto& pt = l.art->paint;
    if (!pt.patternId.empty()) return "pattern";
    if (!pt.markerStart.empty() || !pt.markerMid.empty() ||
        !pt.markerEnd.empty())
        return "marker";
    if (!pt.clipId.empty()) return "clip";
    if (pt.hasMesh) return "mesh";
    if (pt.hasFilter) return "filter";
    return nullptr;
}

const char* layerKindName(LayerItem::Kind kind) {

    switch (kind) {
    case LayerItem::Kind::Pixel: return "pixel";
    case LayerItem::Kind::Text: return "text";
    case LayerItem::Kind::Shape: return "shape";
    case LayerItem::Kind::Adjustment: return "adjust";
    case LayerItem::Kind::Group: return "group";
    case LayerItem::Kind::SmartObject: return "smart";
    case LayerItem::Kind::Frame: return "frame";
    }
    return "?";
}

// A bitmap layer the tile can blit from its own source instead of the
// composite snapshot: no art, plain placement of real pixels, fully opaque
// paint, Normal blend, no mask or style, not clipped. Anything fancier
// keeps the snapshot (which already blends it exactly), because a wrong
// blit is worse than a soft one. Bitmap-internal alpha is fine - it is the
// halo case this tier exists for.
bool simpleBlittable(const LayerItem& l) {
    if (l.kind != LayerItem::Kind::Pixel) return false;
    if (l.art) return false;
    if (!l.pixels || l.pixels->width() == 0 || l.pixels->height() == 0)
        return false;
    if (l.opacity != 100 || l.fill != 100) return false;
    if (l.blendMode != QStringLiteral("Normal")) return false;
    if (l.hasMask || l.styled || l.clipped) return false;
    return true;
}

// Float source to the premultiplied QImage the canvas draws: the same
// straight-to-premul converter the composite uses, so a simple blit
// matches the snapshot pixels it replaces up to the dither phase.
// Converted once per frame per source, shared by the frame's tiles.
QImage bakedSourceImage(BakedTile::ImgCache* cache,
                        const pittore::Image* src) {
    if (!src || src->width() == 0 || src->height() == 0) return QImage();
    if (cache) {
        std::lock_guard<std::mutex> lk(cache->mutex);
        const auto it = cache->imgs.find(src);
        if (it != cache->imgs.end()) return it->second;
    }
    QImage img((int)src->width(), (int)src->height(),
               QImage::Format_ARGB32_Premultiplied);
    blitRGBAfToPremul(src->data(), img.bits(), 0, 0, src->width(),
                      src->height());
    if (cache) {
        std::lock_guard<std::mutex> lk(cache->mutex);
        cache->imgs[src] = img;
    }
    return img;
}

// Tile-trace gate, file-local (not in log.h next to its siblings) so
// enabling the trace never rebuilds the world: only this TU reads it.
// Off unless PITTORE_TILE_TRACE=1.
bool tileTrace() {
    static const bool on = [] {
        const char* e = ::pittore::core::log::pittoreEnv(
            "PITTORE_TILE_TRACE", "INFINITY_TILE_TRACE");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return on;
}

// Region filter for the tile trace (PITTORE_TILE_TRACE_BOX=x,y,w,h in doc
// coords, parsed once): with it set, skips and per-draw outcomes log only
// for intersecting layers, so tracing a badge does not dump thousands of
// unrelated road layers. Empty/absent means no filtering.
QRectF tileTraceBox() {
    static const QRectF box = [] {
        const char* e = ::pittore::core::log::pittoreEnv(
            "PITTORE_TILE_TRACE_BOX", "INFINITY_TILE_TRACE_BOX");
        double x = 0, y = 0, w = 0, h = 0;
        if (e && std::sscanf(e, "%lf,%lf,%lf,%lf", &x, &y, &w, &h) == 4 &&
            w > 0 && h > 0)
            return QRectF(x, y, w, h);
        return QRectF();
    }();
    return box;
}

bool tileTraceWanted(const QRectF& box) {
    if (!tileTrace()) return false;
    const QRectF filter = tileTraceBox();
    return filter.isEmpty() || filter.intersects(box);
}

// Verdicts never change within a document revision (the plan is
// zoom-independent), but plans rebuild on every miss-frame while tiles
// settle: without dedupe the same layer logs its verdict dozens of times,
// and whole-document boxes intersect every region filter. Remember what was
// said, keyed by document + revision + panel index, so edits re-log and
// repeats stay silent.
bool tileVerdictLogged(const DocumentItem* doc, int index) {
    static std::mutex mutex;
    static std::unordered_set<std::uint64_t> seen;
    std::uint64_t key = (std::uint64_t)(const void*)doc;
    key ^= (std::uint64_t)doc->revision + 0x9e3779b97f4a7c15ULL + (key << 6) +
           (key >> 2);
    key ^= (std::uint64_t)(std::uint32_t)index + 0x9e3779b97f4a7c15ULL +
           (key << 6) + (key >> 2);
    std::lock_guard<std::mutex> lk(mutex);
    return !seen.insert(key).second;
}

// Footprint of the paint a composite-tier layer contributes. Its blit pastes
// the flattened composite at that layer's stack position, so the box must be
// the layer's own paint extent: bounding an art-less layer (an imported
// <image>, a raster row) by the whole document pasted the composite over the
// entire tile, and the top-most such layer buried every crisp layer below it
// - the tile then reprinted the soft composite and nothing sharpened, at any
// zoom. An adjustment is the one layer whose paint really is the whole stack.
QRectF compositeTierBox(const DocumentItem& doc, const LayerItem& l) {
    const QRectF docBox(0, 0, doc.size.width(), doc.size.height());
    if (l.kind == LayerItem::Kind::Adjustment) return docBox;
    const LayerDrawSource src = layerDrawSource(l);
    if (src.img) {
        // The source's own rect, scaled about the origin the way the painter
        // places it, so a flipped or resized source still bounds its paint.
        const QRectF local(0, 0, src.img->width(), src.img->height());
        QRectF mapped(QPointF(local.x() * src.scaleX, local.y() * src.scaleY),
                      QPointF(local.right() * src.scaleX,
                              local.bottom() * src.scaleY));
        mapped.translate(src.offset);
        mapped = mapped.normalized();  // negative scale mirrors the source
        if (mapped.isEmpty()) return mapped;
        // A resampled source colours past its own texel grid (the halo
        // stageBounds allows): never clip the blit inside its paint.
        const double halo =
            std::ceil(std::max({src.scaleX, src.scaleY, 1.0})) + 1.0;
        return mapped.adjusted(-halo, -halo, halo, halo).intersected(docBox);
    }
    if (!l.art) return docBox;  // pixels not materialized yet: whole doc
    const QPainterPath path = artNodePath(*l.art);
    if (path.isEmpty()) return {};
    const double* m = l.art->matrix;
    const QTransform nodeToDoc =
        QTransform(m[0], m[1], m[2], m[3], m[4], m[5]) *
        QTransform().scale(l.scaleX, l.scaleY) *
        QTransform().translate(l.offset.x(), l.offset.y());
    const auto& pt = l.art->paint;
    const double sc = std::max({std::abs(l.scaleX), std::abs(l.scaleY), 1e-9});
    const double pad = (pt.hasStroke ? pt.strokeWidth * sc / 2.0 : 0.0) + 2.0;
    const QRectF box = nodeToDoc.mapRect(path.boundingRect());
    if (box.isEmpty()) return box;
    return box.adjusted(-pad, -pad, pad, pad).intersected(docBox);
}

// The one tile plan, at every zoom: a bottom-to-top sweep over every layer
// (panel index high to low). Crisp-reproducible art paints from geometry;
// everything else visible that contributes paint blits from the composite
// snapshot at its own stack position. Interleaved in true paint order the
// tile matches the composite exactly where it is soft, and is razor sharp
// everywhere its recipe reproduces the paint - no veto lists, no burial
// hazard, no zoom threshold (density comes from the bucket, never from
// which layers are sharp).
void buildTilePlan(const DocumentItem& doc, const QRectF& vis,
                   std::vector<BakedTile::Layer>& planned,
                   size_t& plannedLayers, size_t& compLayers,
                   size_t& simpleLayers) {
    planned.clear();
    plannedLayers = 0;
    compLayers = 0;
    simpleLayers = 0;
    QVector<char> effVis;
    QVector<int> effParent;
    doc.effectiveVisibility(effVis, effParent);
    const bool tracing = tileTrace();
    const QRectF traceFilter = tracing ? tileTraceBox() : QRectF();
    for (int k = doc.layers.size() - 1; k >= 0; --k) {
        if (k >= effVis.size() || !effVis[k]) {
            // Invisible layers never reach a tile; log them only under a
            // region filter (an expected-crisp layer missing here means its
            // whole subtree is hidden, which the verdict lines alone hide).
            if (tracing && !traceFilter.isEmpty() &&
                !tileVerdictLogged(&doc, k)) {
                const LayerItem& hidden = doc.layers[k];
                if (layerBounds(doc, hidden).intersects(traceFilter))
                    ::pittore::core::log::log_info(
                        "[render][tile-plan] skip-hidden i=%d kind=%s '%s'",
                        k, layerKindName(hidden.kind),
                        hidden.name.toUtf8().constData());
            }
            continue;
        }
        const LayerItem& l = doc.layers[k];
        if (l.kind != LayerItem::Kind::Pixel &&
            l.kind != LayerItem::Kind::Adjustment) {
            if (tracing && !traceFilter.isEmpty() &&
                !tileVerdictLogged(&doc, k) &&
                layerBounds(doc, l).intersects(traceFilter))
                ::pittore::core::log::log_info(
                    "[render][tile-plan] skip-kind i=%d kind=%s '%s'", k,
                    layerKindName(l.kind), l.name.toUtf8().constData());
            continue;
        }
        if (l.kind == LayerItem::Kind::Pixel && simpleBlittable(l)) {
            // Own-bitmap blit: resampled to view density over crisp-baked
            // art below, so a translucent halo no longer buries sharp roads
            // under a doc-resolution snapshot. Footprint is the source's
            // own (see compositeTierBox), never the document.
            const QRectF box = compositeTierBox(doc, l);
            if (box.isEmpty() || !box.intersects(vis)) continue;
            BakedTile::Layer b;
            b.pixels = l.pixels;
            b.scaleX = l.scaleX;
            b.scaleY = l.scaleY;
            b.offX = l.offset.x();
            b.offY = l.offset.y();
            b.box = box;
            b.usePixels = true;
            b.index = k;
            planned.push_back(std::move(b));
            ++simpleLayers;
            if (tileTraceWanted(box) && !tileVerdictLogged(&doc, k))
                ::pittore::core::log::log_info(
                    "[render][tile-plan] simple i=%d kind=%s box=%.1fx%.1f"
                    "@%.0f,%.0f '%s'",
                    k, layerKindName(l.kind), box.width(), box.height(),
                    box.x(), box.y(), l.name.toUtf8().constData());
            continue;
        }
        if (l.kind == LayerItem::Kind::Pixel && crispReproducible(l)) {
            const QRectF box = crispLayerBox(doc, k);
            if (!box.intersects(vis)) continue;
            BakedTile::Layer b;
            b.art = l.art;
            b.scaleX = l.scaleX;
            b.scaleY = l.scaleY;
            b.offX = l.offset.x();
            b.offY = l.offset.y();
            b.box = box;
            b.index = k;
            planned.push_back(std::move(b));
            ++plannedLayers;
            if (tileTraceWanted(box) && !tileVerdictLogged(&doc, k))
                ::pittore::core::log::log_info(
                    "[render][tile-plan] crisp i=%d kind=%s box=%.1fx%.1f"
                    "@%.0f,%.0f '%s'",
                    k, layerKindName(l.kind), box.width(), box.height(),
                    box.x(), box.y(), l.name.toUtf8().constData());
            continue;
        }
        // Composite tier: photos, styled/masked/blended layers,
        // pattern/marker/mesh/filter/clip paint, adjustments. Bounded by the
        // layer's own paint, never by the document (see compositeTierBox).
        const QRectF box = compositeTierBox(doc, l);
        if (box.isEmpty() || !box.intersects(vis)) continue;
        BakedTile::Layer b;
        b.useComposite = true;
        b.box = box;
        b.index = k;
        planned.push_back(std::move(b));
        ++compLayers;
        if (tileTraceWanted(box) && !tileVerdictLogged(&doc, k)) {
            const char* reason = l.kind == LayerItem::Kind::Adjustment
                                     ? "adjustment"
                                     : crispVetoReason(l);
            ::pittore::core::log::log_info(
                "[render][tile-plan] comp i=%d kind=%s reason=%s "
                "box=%.1fx%.1f@%.0f,%.0f '%s'",
                k, layerKindName(l.kind), reason ? reason : "?",
                box.width(), box.height(), box.x(), box.y(),
                l.name.toUtf8().constData());
        }
    }
}

}  // namespace

CanvasTileStore& CanvasTileStore::instance() {
    static CanvasTileStore store;
    return store;
}

void CanvasTileStore::setEnabled(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = on;
    if (!on) {
        tiles_.clear();
        pending_.clear();
        usedBytes_ = 0;
    }
}

bool CanvasTileStore::bakeAllowedAt(double zoomEff) const {
    // Every positive finite zoom bakes: the grid shrinks the tile in document
    // space rather than the store refusing (see tileDocPxFor). Only
    // degenerate zoom has no raster to make.
    if (!(zoomEff > 0.0) || !std::isfinite(zoomEff)) return false;
    return true;
}

int CanvasTileStore::tileImageEdge(double zoomEff) {
    const double bucket = crispZoomBucket(zoomEff);
    const double side = tileDocPxFor(bucket);
    // Density is the view, clamped to the pixel cap exactly as the bake
    // clamps it, so this is the raster the store would actually publish.
    return qRound(side * std::min(zoomEff, (double)kMaxTilePx / side));
}

void CanvasTileStore::describeTilePlan(const DocumentItem& doc,
                                       const QRectF& vis, double bucket,
                                       long* crisp, long* comp,
                                       long* simple) {
    (void)bucket;  // density only: the plan itself never depends on zoom
    std::vector<BakedTile::Layer> planned;
    size_t c = 0, m = 0, s = 0;
    buildTilePlan(doc, vis, planned, c, m, s);
    if (crisp) *crisp = (long)c;
    if (comp) *comp = (long)m;
    if (simple) *simple = (long)s;
}

bool CanvasTileStore::paintReady(QPainter& p, const DocumentItem& doc,
                                 const QTransform& docToView, double zoomEff,
                                 const QRectF& docRect, const QRect& viewDirty,
                                 ReadySlot scheduleUpdate) {
    if (!enabled_) return false;
    const double bucket = crispZoomBucket(zoomEff);
    // Document px per tile side at this bucket: fixed until its raster would
    // reach kMaxTilePx, then shrinking with the bucket so a tile's raster
    // stays inside the pixel cap. Only the tiles covering the view are built,
    // and the viewport holds about kViewportPx / kMaxTilePx of them whatever
    // the zoom, so the grid is scale-invariant in cost as well as in sharpness.
    const double side = tileDocPxFor(bucket);
    const bool bucketBakeable = bakeAllowedAt(zoomEff);
    const QRectF vis = viewDirty.isEmpty()
                           ? docRect
                           : docToView.inverted()
                                 .mapRect(QRectF(viewDirty))
                                 .intersected(docRect);
    if (vis.isEmpty()) return true;
    const int tx0 = (int)std::floor(vis.left() / side);
    const int ty0 = (int)std::floor(vis.top() / side);
    const int tx1 = (int)std::floor((vis.right() - 1e-6) / side);
    const int ty1 = (int)std::floor((vis.bottom() - 1e-6) / side);

    // Guard the repaint callback by the paint device widget: a tile
    // finishing after its view closed updates nothing instead of touching
    // a dead viewport.
    ReadySlot guarded = scheduleUpdate;
    QPaintDevice* dev = p.device();
    if (dev && dev->devType() == QInternal::Widget) {
        const QPointer<QWidget> guard(static_cast<QWidget*>(dev));
        guarded = [guard, slot = std::move(scheduleUpdate)] {
            if (!guard.isNull() && slot) slot();
        };
    }

    // One shared plan for every tile this frame: a single O(n) sweep, not
    // one per tile. Tiles replace the composite for their own rect, so they
    // paint every qualifying vector layer bottom-to-top with no overpaint
    // hazard - interleaved in true stack order the tile matches the
    // composite exactly where it is soft and is razor sharp everywhere the
    // crisp recipe reproduces the paint. No zoom threshold: the same plan
    // runs at every bucket, high zoom just rasterizes it denser. Layers
    // whose paint the crisp recipe cannot reproduce exactly (patterns,
    // markers, meshes, filters, clips) stay on the composite path:
    // wrong-crisp is worse than soft.
    std::vector<BakedTile::Layer> planned;
    bool plannedBuilt = false;
    // Diagnostics for the tiles line: crisp vector layers vs composite
    // blits carried per missing tile set.
    size_t plannedLayers = 0, compLayers = 0, simpleLayers = 0;
    auto buildPlanned = [&] {
        if (plannedBuilt) return;
        plannedBuilt = true;
        buildTilePlan(doc, vis, planned, plannedLayers, compLayers,
                      simpleLayers);
    };

    bool allReady = true;
    long readyCount = 0, missingCount = 0, staleCount = 0;
    // One geometry cache for every tile queued by this frame: nodes shared
    // across tiles build their paths once.
    const auto pathCache = std::make_shared<BakedTile::PathCache>();
    // One converted-bitmap cache per frame: plain image sources convert
    // once, not once per tile they touch.
    const auto imgCache = std::make_shared<BakedTile::ImgCache>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++tick_;
        knownRevision_[&doc] = doc.revision;
        wantBucket_[&doc] = bucket;
        wantDensity_[&doc] = zoomEff;
        if (knownRevision_.size() > kMaxKnownDocs) {
            for (auto it = knownRevision_.begin();
                 it != knownRevision_.end();) {
                bool live = false;
                for (const auto& kv : tiles_) {
                    if (kv.first.doc == it->first) {
                        live = true;
                        break;
                    }
                }
                it = live ? std::next(it)
                          : (wantBucket_.erase(it->first),
                             wantDensity_.erase(it->first),
                             knownRevision_.erase(it));
            }
        }
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                const TileKey key{&doc, tx, ty};
                const QRectF tileDoc((qreal)tx * side, (qreal)ty * side,
                                     side, side);
                auto it = tiles_.find(key);
                if (it != tiles_.end() && !it->second.img.isNull() &&
                    it->second.revision == doc.revision &&
                    it->second.bucket == bucket &&
                    densityClose(it->second.density, zoomEff)) {
                    it->second.tick = tick_;
                    drawTileImage(p, tileDoc, it->second.img);
                    ++readyCount;
                    continue;
                }
                // Not current: stale bucket, or the same bucket baked at a
                // density the view has moved off. Draw it as the placeholder
                // instead of the soft composite - within a few buckets it
                // still resolves more detail - while the exact bucket at the
                // view's density bakes below. Its own grid may differ from
                // this one (the side tracks the bucket), so it is laid down
                // over the document rect it was baked for.
                if (it != tiles_.end() && !it->second.img.isNull() &&
                    it->second.revision == doc.revision &&
                    it->second.bucket > 0.0 &&
                    it->second.bucket >= bucket / 4.0 &&
                    it->second.bucket <= bucket * 4.0) {
                    const double staleSide = tileDocPxFor(it->second.bucket);
                    it->second.tick = tick_;
                    drawTileImage(
                        p, QRectF((qreal)tx * staleSide, (qreal)ty * staleSide,
                                  staleSide, staleSide),
                        it->second.img);
                    ++staleCount;
                }
                allReady = false;
                ++missingCount;
                const quint64 rev = doc.revision;
                const auto pit = pending_.find(key);
                const bool wanted =
                    bucketBakeable &&
                    (pit == pending_.end() || pit->second.revision != rev ||
                     pit->second.bucket != bucket ||
                     !densityClose(pit->second.density, zoomEff));
                if (wanted && (int)pending_.size() < kMaxPendingPerDoc) {
                    buildPlanned();
                    BakedTile job;
                    job.doc = &doc;
                    job.tx = tx;
                    job.ty = ty;
                    job.revision = rev;
                    job.bucket = bucket;
                    job.density = zoomEff;
                    job.docRect = tileDoc;
                    job.pathCache = pathCache;
                    job.imgCache = imgCache;
                    // Composite snapshot backing the composite-tier blits:
                    // one doc-resolution copy per tile, taken on the GUI
                    // thread so workers never race a rebuild.
                    if (!doc.composite.isNull()) {
                        job.compRect =
                            tileDoc.toAlignedRect().intersected(
                                doc.composite.rect());
                        if (!job.compRect.isEmpty())
                            job.comp = doc.composite.copy(job.compRect);
                    }
                    for (const BakedTile::Layer& b : planned) {
                        if (b.box.intersects(tileDoc))
                            job.layers.push_back(b);
                    }
                    // The frame that queued the bake owns its repaint: the
                    // callback fires when this bake publishes.
                    pending_.insert_or_assign(
                        key, Pending{rev, bucket, zoomEff, guarded});
                    QThreadPool::globalInstance()->start(
                        new TileJob(std::move(job), tileNotifier()));
                }
            }
        }
    }
    if (readyCount > 0 || missingCount > 0)
        ::pittore::core::log::log_info(
            "[render][tiles] ready=%ld stale=%ld missing=%ld bucket=%.2f "
            "store=%.1fMB%s",
            readyCount, staleCount, missingCount, bucket,
            usageBytes() / 1048576.0,
            missingCount > 0
                ? QStringLiteral(" crisp=%1 comp=%2 simple=%3")
                      .arg(plannedLayers)
                      .arg(compLayers)
                      .arg(simpleLayers)
                      .toUtf8()
                      .constData()
                : "");
    return allReady;
}

bool CanvasTileStore::stillWanted(const BakedTile& tile) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const TileKey key{tile.doc, tile.tx, tile.ty};
    const auto pit = pending_.find(key);
    if (pit == pending_.end()) return false;
    return pit->second.revision == tile.revision &&
           pit->second.bucket == tile.bucket &&
           densityClose(tile.density, pit->second.density);
}

void CanvasTileStore::completeTile(BakedTile tile) {
    ReadySlot ready;
    const int bakedW = tile.img.width();
    const int bakedH = tile.img.height();
    const size_t bakedLayers = tile.layers.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const TileKey key{tile.doc, tile.tx, tile.ty};
        const auto pit = pending_.find(key);
        if (pit != pending_.end()) {
            // Only the bake this frame-slot is still waiting for may
            // claim its repaint. A superseded bake (its zoom step moved
            // on) must NOT erase the newer slot's entry: doing so stole
            // the completion callback of the job that replaced it, so
            // the tile published but no repaint was scheduled - the
            // exact "settled and still soft" failure after a fast zoom
            // burst. Mismatched completions leave the entry alone.
            if (pit->second.revision == tile.revision &&
                pit->second.bucket == tile.bucket &&
                densityClose(tile.density, pit->second.density)) {
                ready = pit->second.onReady;
                pending_.erase(pit);
            }
        }
        // Stale bake (an edit or zoom landed while it rendered): discard.
        // Compared against the last revision seen on paint, never by
        // dereferencing the key - the document may be closed by now.
        const auto kr = knownRevision_.find(tile.doc);
        if (kr == knownRevision_.end() || kr->second != tile.revision)
            return;
        auto it = tiles_.find(key);
        if (it != tiles_.end()) {
            if (it->second.revision == tile.revision &&
                it->second.bucket == tile.bucket &&
                densityClose(it->second.density, tile.density))
                return;  // duplicate job already published
            // Never let a late bake for an old zoom step clobber a tile
            // the current step already published: that put the store back
            // on the wrong bucket and forced a re-queue loop while the
            // user scrolled back and forth. The same guard covers density:
            // a bake the view has moved off must not replace a raster that
            // already sits closer to the view.
            const auto wb = wantBucket_.find(tile.doc);
            const double wantB =
                wb != wantBucket_.end() ? wb->second : tile.bucket;
            const auto wd = wantDensity_.find(tile.doc);
            const double wantD =
                wd != wantDensity_.end() ? wd->second : tile.density;
            if (it->second.revision == tile.revision &&
                it->second.bucket == wantB && tile.bucket != wantB)
                return;  // keep the tile the current zoom wants
            if (it->second.revision == tile.revision &&
                it->second.bucket == tile.bucket &&
                std::abs(it->second.density - wantD) <=
                    std::abs(tile.density - wantD))
                return;  // published raster is already as close as this
            usedBytes_ -= (qint64)it->second.img.sizeInBytes();
        }
        TileEntry e;
        e.revision = tile.revision;
        e.bucket = tile.bucket;
        e.density = tile.density;
        e.img = std::move(tile.img);
        e.tick = ++tick_;
        usedBytes_ += (qint64)e.img.sizeInBytes();
        tiles_.insert_or_assign(key, std::move(e));
        evictLocked();
    }
    // Slow bakes are the jank signal (one heavy tile stalls its whole
    // region): worth a line with the layer count behind it. The content
    // counters ride along, and the line also fires on the two shapes of
    // "the tile came back wrong": geometry that drew nothing, or crisp
    // draws with every one of them covered by a composite blit above.
    if (tile.bakeMs > 25 || tile.emptyPaths > 0 ||
        (tile.drawnPaths > 0 &&
         (tile.buriedPaths >= tile.drawnPaths || tile.inkPct == 0)))
        ::pittore::core::log::log_info(
            "[render][tile-bake] slow=%lldms layers=%zu px=%dx%d drawn=%d "
            "buried=%d empty=%d comp=%d ink=%d%%",
            tile.bakeMs, bakedLayers, bakedW, bakedH, tile.drawnPaths,
            tile.buriedPaths, tile.emptyPaths, tile.compBlits, tile.inkPct);
    if (ready) ready();
}

void CanvasTileStore::evictLocked() {
    long dropped = 0;
    while (!tiles_.empty() &&
           (usedBytes_ > kTileBudgetBytes || tiles_.size() > kMaxTiles)) {
        quint64 oldest = std::numeric_limits<quint64>::max();
        auto victim = tiles_.end();
        for (auto v = tiles_.begin(); v != tiles_.end(); ++v) {
            if (v->second.tick < oldest) {
                oldest = v->second.tick;
                victim = v;
            }
        }
        if (victim == tiles_.end()) break;
        usedBytes_ -= (qint64)victim->second.img.sizeInBytes();
        tiles_.erase(victim);
        ++dropped;
    }
    if (dropped > 32)
        ::pittore::core::log::log_info(
            "[render][tiles] evicted=%ld usage=%.1fMB", dropped,
            usedBytes_ / 1048576.0);
}

qint64 CanvasTileStore::usageBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usedBytes_;
}

// Blit the composite exactly like the classic path (full or damaged
// sub-rect): snapped to whole document pixels so fractional zooms never
// wobble painted pixels between flushes.
void blitTiledComposite(QPainter& p, const DocumentItem& doc,
                        const QTransform& docToView, const QRectF& docRect,
                        const QRect& viewDirty) {
    if (doc.composite.isNull()) return;
    if (viewDirty.isEmpty()) {
        p.drawImage(docRect, doc.composite);
        return;
    }
    const QRectF docDirtyF =
        docToView.inverted().mapRect(QRectF(viewDirty)).intersected(docRect);
    const QRect docDirty =
        docDirtyF.isEmpty()
            ? QRect()
            : docDirtyF.toAlignedRect().intersected(doc.composite.rect());
    if (!docDirty.isEmpty())
        p.drawImage(QRectF(docDirty), doc.composite, docDirty);
}

bool paintDraftContent(QPainter& p, const DocumentItem& doc,
                       const QTransform& docToView, double zoomEff,
                       const QRectF& docRect, const QRect& viewDirty,
                       CanvasTileStore::ReadySlot scheduleUpdate) {
    if (doc.composite.isNull()) return false;
    // Degenerate zoom has no raster to make; anything else bakes (the grid
    // shrinks the tile rather than refusing), so Draft rides the tiles. A
    // false here would fall through to the classic walk, whose frame budget
    // cannot cover a whole deep-zoom frame in one pass.
    if (!CanvasTileStore::instance().bakeAllowedAt(zoomEff)) return false;
    blitTiledComposite(p, doc, docToView, docRect, viewDirty);
    // Always done: the composite is on screen, and missing tiles repaint
    // through onReady when their bakes land. Returning tile readiness
    // instead would fall through to the live segmented walk and defeat
    // Draft's frame budget.
    CanvasTileStore::instance().paintReady(p, doc, docToView, zoomEff,
                                            docRect, viewDirty,
                                            std::move(scheduleUpdate));
    return true;
}

bool paintTiledContent(QPainter& p, const DocumentItem& doc,
                       const QTransform& docToView, double zoomEff,
                       const QRectF& docRect, const QRect& viewDirty,
                       CanvasTileStore::ReadySlot scheduleUpdate) {
    if (doc.composite.isNull()) return false;
    blitTiledComposite(p, doc, docToView, docRect, viewDirty);
    return CanvasTileStore::instance().paintReady(
        p, doc, docToView, zoomEff, docRect, viewDirty,
        std::move(scheduleUpdate));
}

}  // namespace pittore::ui
