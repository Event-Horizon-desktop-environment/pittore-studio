#pragma once
// Tiled crisp overlay: the vector overlay baked per document tile on worker
// threads, blitted over the composite on the GUI thread.
//
// Tiles are 256x256 document pixels, rasterized at the density of the view
// that asked for them. The quantized zoom bucket fixes the tile grid; the
// raster tracks the view itself, because a tile that is drawn scaled (even
// by a fraction of a percent) loses about a quarter of its edge energy to
// the resample. A tile paints the interleaved bottom-to-top plan of every
// visible layer (crisp recipe for reproducible art, composite snapshot for
// the rest), so any tile bakes correctly on its own from immutable geometry
// snapshots - the baker never touches live layer state, and the plan never
// depends on zoom: zoom picks the raster density, not what is sharp. Tiles
// are pinned by document revision plus zoom bucket plus view density; any
// edit, zoom step or density drift past a small tolerance misses and
// re-bakes. When every visible tile is ready the frame skips the
// per-layer walk entirely; on any miss the classic path paints this frame
// and the missing tiles queue for the background baker.
//
// Lifetime: entries are keyed by document pointer but the pointer is never
// dereferenced off the paint call that supplied it. Completion compares the
// baked revision against the last revision seen for that document, and the
// repaint callback is guarded by the paint device widget, so tiles baked
// for a closed document (or view) are discarded, never touched.
#include <QRect>
#include <QRectF>
#include <QTransform>

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QImage>

class QPainter;

namespace pittore::ui {

struct DocumentItem;
struct BakedTile;

class CanvasTileStore {
public:
    static CanvasTileStore& instance();
    void setEnabled(bool on);
    qint64 usageBytes() const;

    // Drop every queued bake and wait for the in-flight ones. CanvasView
    // calls this from its destructor: bakes run on a pool that outlives
    // QApplication, so a bake still rasterizing during application teardown
    // races the GUI/fontconfig shutdown (exit-time heap corruption). After
    // this returns no worker touches Qt or the store. Idempotent; safe to
    // call from the GUI thread while bakes are mid-flight (they only take
    // mutex_ in stillWanted, which is not held across the wait).
    void shutdown();

    using ReadySlot = std::function<void()>;

    // Blit the baked overlay for the visible region. Returns true when
    // every visible tile was ready (nothing more to paint for the
    // overlay); false queues the missing tiles and the caller paints the
    // classic path instead. The painter transform must map document space.
    bool paintReady(QPainter& p, const DocumentItem& doc,
                    const QTransform& docToView, double zoomEff,
                    const QRectF& docRect, const QRect& viewDirty,
                    ReadySlot scheduleUpdate);

    // Baker completion (queued to the GUI thread): publish or discard.
    void completeTile(BakedTile tile);

    // Worker-side gate: true while the pending entry for this tile still
    // asks for exactly this revision+bucket+view density. A bake whose zoom
    // step (or density) was superseded mid-flight aborts here instead of
    // burning thread-pool time on a raster nobody will draw.
    bool stillWanted(const BakedTile& tile) const;

    // Whether the store will ever bake a tile at this effective zoom. True
    // for every positive finite zoom: past the point where a fixed-size tile
    // would outgrow the pixel cap the tile shrinks in document space, so the
    // raster stays affordable instead of the store refusing and stranding the
    // caller on a soft composite. NaN/non-positive zoom is refused.
    bool bakeAllowedAt(double zoomEff) const;

    // Edge length in device pixels of a tile baked for this zoom. Tiles
    // shrink in document space as the zoom grows, so this sits inside the
    // pixel cap at any zoom - the seam for the "deep zoom still bakes"
    // guarantee.
    static int tileImageEdge(double zoomEff);

    // Layer counts a tile bake over `vis` would paint from geometry
    // (crisp), from their own bitmap (simple), vs the composite snapshot
    // (comp). The plan must not depend on zoom: crisp happens at every
    // bucket, high or low - zoom changes the raster density, never which
    // layers get sharp.
    static void describeTilePlan(const DocumentItem& doc,
                                 const QRectF& vis, double bucket,
                                 long* crisp, long* comp, long* simple);

private:
    CanvasTileStore() = default;
    ~CanvasTileStore();
    struct TileKey {
        const DocumentItem* doc = nullptr;
        int tx = 0;
        int ty = 0;
        bool operator==(const TileKey& o) const {
            return doc == o.doc && tx == o.tx && ty == o.ty;
        }
    };
    struct TileKeyHash {
        std::size_t operator()(const TileKey& k) const noexcept {
            std::size_t h = std::hash<const void*>{}(k.doc);
            h ^= std::hash<int>{}(k.tx) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(k.ty) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct TileEntry {
        quint64 revision = 0;
        double bucket = 0.0;
        double density = 0.0;  // view density this raster was baked at
        QImage img;
        quint64 tick = 0;
    };
    struct Pending {
        quint64 revision = 0;
        double bucket = 0.0;
        double density = 0.0;  // view density the waiting frame wants
        ReadySlot onReady;
    };
    void evictLocked();

    mutable std::mutex mutex_;
    std::unordered_map<TileKey, TileEntry, TileKeyHash> tiles_;
    std::unordered_map<TileKey, Pending, TileKeyHash> pending_;
    // Last revision observed per document on the paint thread: lets
    // completion discard stale bakes without dereferencing the key.
    std::unordered_map<const DocumentItem*, quint64> knownRevision_;
    // Zoom bucket the last paint wanted per document: completion uses it
    // to keep a late old-bucket bake from clobbering a newer tile.
    std::unordered_map<const DocumentItem*, double> wantBucket_;
    // View density the last paint wanted per document: the same guard for
    // bakes inside one bucket, whose rasters differ by the density they
    // were baked at (the bucket fixes the grid, never the pixels).
    std::unordered_map<const DocumentItem*, double> wantDensity_;
    qint64 usedBytes_ = 0;
    quint64 tick_ = 0;
    bool enabled_ = true;
};

// Draft-mode content: composite blit plus the baked tile overlay (tiles
// are blits, so Draft stays fast and still settles crisp instead of
// stranding sharp tiles behind a soft composite). True when painted (the
// composite exists); false falls through to the classic path.
bool paintDraftContent(QPainter& p, const DocumentItem& doc,
                       const QTransform& docToView, double zoomEff,
                       const QRectF& docRect, const QRect& viewDirty,
                       CanvasTileStore::ReadySlot scheduleUpdate);

// Full-mode content: composite blit plus the baked tile overlay. True when
// complete (the caller skips the classic path); false queues missing tiles
// and the caller paints the classic path this frame instead.
bool paintTiledContent(QPainter& p, const DocumentItem& doc,
                       const QTransform& docToView, double zoomEff,
                       const QRectF& docRect, const QRect& viewDirty,
                       CanvasTileStore::ReadySlot scheduleUpdate);

}  // namespace pittore::ui
