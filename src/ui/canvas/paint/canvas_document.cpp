#include "ui/canvas_view.h"

#include "ui/proof_preview.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QSet>
#include <QString>
#include <QDateTime>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui/ai_models.h"
#include "ui/contextual_task_bar.h"
#include "ui/icons.h"
#include "ui/persona/vector_raster.h"
#include "ui/persona/vector_profile.h"
#include "ui/persona/vector_view.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"
#include "ui/canvas/trace/canvas_trace.h"

#include "ui/canvas/paint/canvas_display_mode.h"
#include "ui/canvas/paint/canvas_crisp.h"
#include "ui/canvas/paint/canvas_layer_cache.h"
#include "ui/canvas/paint/canvas_tile_store.h"

namespace pittore::ui {
namespace {


// Blit a composite source exactly like the legacy path (full or damaged
// sub-rect), so segmented and legacy paints sample identically.
void blitComposite(QPainter& p, const QTransform& t, const QImage& src,
                   const QRectF& docRect, const QRect& viewDirty) {
    if (src.isNull()) return;
    if (viewDirty.isEmpty()) {
        p.drawImage(docRect, src);
        return;
    }
    // Sample only the damaged source stretch so a region edit paints the
    // sub-rect, not a full 2000×2000 texture transform. Snap the
    // inverse-mapped rect to whole document pixels: at fractional zoom
    // an unaligned sub-rect lets the smoothing grid wobble the painted
    // pixels between flushes (the "pixels moving" brush artifact).
    const QRectF docDirtyF =
        t.inverted().mapRect(QRectF(viewDirty)).intersected(docRect);
    const QRect docDirty =
        docDirtyF.isEmpty()
            ? QRect()
            : docDirtyF.toAlignedRect().intersected(src.rect());
    if (!docDirty.isEmpty())
        p.drawImage(QRectF(docDirty), src, docDirty);
}

// Fully-opaque art for a crisp live draw: any translucency (fill,
// gradient stop, stroke, node opacity), paint servers (patterns), markers,
// clips/masks, meshes or filters means the layer keeps its correctly
// blended composite raster instead. Mirrors the per-drawable checks the old
// global solo gate applied, now decided per layer. Layer-level blend,
// opacity, adjustment, clip, mask, style and filter state is already vetted
// by vectorViewOrder before a layer can reach here.
bool liveDrawOpaque(const LayerItem& l) {
    if (!l.art || l.art->isEmpty()) return false;
    if (l.art->opacity < 1.0) return false;
    const auto& pt = l.art->paint;
    if (pt.hasFill && pt.fill[3] != 255) return false;
    if (pt.hasGradient) {
        for (const auto& stop : pt.gradient.stops) {
            if (stop.rgba[3] != 255) return false;
        }
    }
    if (pt.hasStrokeGradient) {
        for (const auto& stop : pt.strokeGradient.stops) {
            if (stop.rgba[3] != 255) return false;
        }
    }
    if (!pt.patternId.empty()) return false;
    if (pt.hasStroke && pt.stroke[3] != 255) return false;
    if (!pt.markerStart.empty() || !pt.markerMid.empty() ||
        !pt.markerEnd.empty())
        return false;
    if (!pt.clipId.empty() || !pt.maskId.empty()) return false;
    if (pt.hasMesh || pt.hasFilter) return false;
    return true;
}

// Conservative doc-space box for culling a live vector draw: the layer's
// baked footprint grown well past any stroke/marker overhang. Layers with
// markers are never culled (marker glyphs can extend past the bake box, and
// a wrongly skipped layer reads as missing content). Anything else either
// draws (box visible) or would have been fully clipped by the painter.
bool liveDrawCulled(const LayerItem& l, const QRectF& visibleDoc) {
    if (visibleDoc.isEmpty()) return false;
    const auto& pt = l.art->paint;
    if (!pt.markerStart.empty() || !pt.markerMid.empty() ||
        !pt.markerEnd.empty())
        return false;
    const LayerDrawSource src = layerDrawSource(l);
    if (!src.img) return false;  // no bake box: cannot prove off-view
    const QRectF box(QPointF(src.offset.x(), src.offset.y()),
                     QSizeF(src.img->width() * src.scaleX,
                            src.img->height() * src.scaleY));
    if (box.isEmpty()) return false;
    const double m =
        16.0 + 0.05 * std::max(box.width(), box.height());
    return !box.adjusted(-m, -m, m, m).intersects(visibleDoc);
}

// Content box for one solo run: the union of its pixel layers' baked
// footprints. Runs holding anything that can paint outside layer footprints
// (overlay stand-ins for Text/Shape/Adjustment rows, whole-region live
// adjustments, tone spans with their low-pass halo, layer-style glows)
// report fullDoc and keep the legacy full-width blit. Anything else is
// transparent outside the union, so shrinking or skipping the blit paints
// bit-identically with far less upscale filtering.
struct RunBox {
    QRectF box;
    bool fullDoc = true;
};
RunBox runContentBox(const DocumentItem& d, int from, int to,
                     const QVector<char>& effVis) {
    RunBox out;
    out.fullDoc = false;
    bool any = false;
    for (int k = from; k <= to; ++k) {
        if (k < 0) continue;
        if (k >= d.layers.size() || k >= effVis.size()) break;
        const LayerItem& l = d.layers[k];
        if (!l.visible || !effVis[k]) continue;
        // Plain groups carry no pixels and draw no overlay: they never
        // extend a run's painted area. Tone headers are the exception —
        // their spans read a blurred backdrop with a halo — as are overlay
        // stand-ins (Text/Shape rows paint doc-relative shapes), live
        // adjustments (they grade the whole region) and styled layers
        // (glows reach past the footprint). None of those is bounded by a
        // layer box: keep the full overlap.
        if (l.kind == LayerItem::Kind::Group) {
            if (l.toneBlendGroup) {
                out.fullDoc = true;
                return out;
            }
            continue;
        }
        if (l.kind != LayerItem::Kind::Pixel || !l.style.empty()) {
            out.fullDoc = true;
            return out;
        }
        if (!l.pixels) {
            // Gather would realise this (doc-sized, opaque for the
            // Background): cannot bound it without painting.
            out.fullDoc = true;
            return out;
        }
        // stageBounds, not layerBounds: the box must also cover the
        // resample halo (a scaled layer colours up to a texel past its
        // footprint) and any unlinked mask overhang, or the blit would
        // shave a real fringe.
        const QRectF b = stageBounds(d, l);
        if (b.isEmpty()) continue;
        out.box = any ? out.box.united(b) : b;
        any = true;
    }
    if (!any) out.box = QRectF();
    return out;
}



}  // namespace

// Segmented paint: pixel runs composite solo (in order), simple vector runs
// draw from geometry at view resolution. Returns false when no layer
// qualifies, and the caller falls back to the single flattened blit — photo
// documents paint exactly one composite, as before.
bool CanvasView::paintSegmented(QPainter& p, const QTransform& t,
                                const QRectF& docRect, const QRect& viewDirty,
                                bool baseDrawn) {
    DocumentItem* d = doc();
    if (!d) return false;
    const QVector<int> drawable = vectorViewOrder(*d);
    if (drawable.isEmpty()) return false;
    QSet<int> drawableSet(drawable.begin(), drawable.end());

    // Document region visible through the damaged view rect: live draws
    // outside it would be fully clipped by the painter, so they are skipped
    // before building any QPainterPath (the expensive part at 32x zoom with
    // hundreds of parts). Empty on full repaints: nothing is culled then.
    QRectF visibleDoc;
    if (!viewDirty.isEmpty()) {
        visibleDoc =
            t.inverted().mapRect(QRectF(viewDirty)).intersected(docRect);
    }

    // One linear visibility sweep shared by the gate, the walk and the
    // draws below. Per-index effectivelyVisible() scans back O(n) rows per
    // call, turning every sweep here quadratic on multi-thousand-layer docs.
    QVector<char> effVis;
    QVector<int> effParent;
    d->effectiveVisibility(effVis, effParent);

    // Unified vector paint: one edit-time composite blit plus crisp live
    // draws only where provably safe. The composite already holds every
    // layer correctly ordered and blended (pixels, groups, adjustments,
    // tone spans, styles, filters); a live draw over it is exact exactly
    // when it paints over its own raster with nothing stacked above it
    // that it would wrongly cover. That holds when the layer is fully
    // opaque AND no pixel content above it overlaps the visible region:
    // an opaque draw covers its raster texel-for-texel (same geometry),
    // and with no above-pixels the composite beneath it is just the
    // below-stack it belongs over. Everything else stays raster-only:
    // translucent art keeps its correct composite blend (sampled at
    // document resolution, like placed photos). Partially covered lives
    // draw crisp clipped to their uncovered region; fully covered ones
    // are skipped (their raster already shows correctly underneath).
    // Zero rebuilds, zero visibility churn, zero uploads, zero cached
    // images: zoom/pan frames cost one blit plus the visible crisp draws,
    // independent of layer count. The old fast path is subsumed (its
    // documents draw every live: nothing above, all opaque).
    //
    // Pixel-run ranges between the drawables (panel order): only their
    // boxes matter here — no images, no rebuilds. A run stacked above a
    // live with an overlapping box clips (or vetoes) that live's draw.
    struct Run {
        int from = -1;  // panel indices, from <= to
        int to = -1;
        QRectF box;
        bool full = true;  // unbounded: overlaps everything below it
    };
    std::vector<Run> runs;
    {
        int w = d->layers.size() - 1;
        while (w >= 0) {
            if (drawableSet.contains(w) && effVis[w]) {
                --w;
                continue;
            }
            int runTop = w;
            while (w >= 0 && (!drawableSet.contains(w) || !effVis[w]))
                --w;
            bool anyVisible = false;
            for (int k = runTop; k > w; --k) {
                if (d->layers[k].visible && effVis[k]) {
                    anyVisible = true;
                    break;
                }
            }
            if (!anyVisible) continue;
            const RunBox rb = runContentBox(*d, w + 1, runTop, effVis);
            runs.push_back(Run{w + 1, runTop, rb.box, rb.fullDoc});
        }
    }
    // Visible region for the overlap tests (whole doc on full repaints).
    const QRectF vis = visibleDoc.isEmpty() ? docRect : visibleDoc;
    // Effective view density (transform scale times device pixels): the
    // view-cache bucket key, so repeat frames blit instead of rebuilding.
    const double zoomEff = std::hypot(t.m11(), t.m12()) *
                           (p.device() ? p.device()->devicePixelRatioF() : 1.0);
    // The tiled path already laid the base down before its ready tiles; a
    // second composite blit here would cover them and lose the sharpening.
    if (!baseDrawn) blitComposite(p, t, d->composite, docRect, viewDirty);
    // Coverage index over the runs: the per-layer overlap scan below turns
    // quadratic past a few thousand layers, so queries only visit runs
    // sharing a cell with the live box. Same candidate set, same clipping.
    std::vector<CoverRun> coverRuns;
    coverRuns.reserve(runs.size());
    for (const Run& r : runs) coverRuns.push_back(CoverRun{r.box, r.full});
    CoverIndex cover;
    cover.build(coverRuns, docRect);
    std::vector<int> candidates;
    CrispStats crispStats;
    const auto crispT0 = std::chrono::steady_clock::now();
    // Live-bake budget: the frame rasterizes crisp layers only while time
    // remains; the rest keep composite pixels this frame and sharpen via
    // background tiles. Without it a 35k-layer frame bakes for seconds and
    // the cache (which can never hold that working set) just thrashes.
    const auto crispDeadline =
        crispT0 + std::chrono::milliseconds(kMaxCrispBakeMs);
    // Bottom-to-top: panel index size-1 down to 0 (matches legacy order).
    for (int k = d->layers.size() - 1; k >= 0; --k) {
        if (!drawableSet.contains(k) || !effVis[k]) continue;
        const LayerItem& l = d->layers[k];
        if (liveDrawCulled(l, visibleDoc)) continue;
        if (!liveDrawOpaque(l)) continue;  // raster fallback, still correct
        // Above-pixel overlap: runs stacked above (smaller panel index)
        // whose boxes reach the live's visible box. Fully covered lives are
        // skipped outright (their raster already shows correctly in the
        // composite); partial overlaps draw crisp only where uncovered.
        // Clipping can only ever remove crispness, never correctness: the
        // complete composite beneath is always right.
        const QRectF liveBox = layerBounds(*d, l).intersected(docRect);
        if (liveBox.isEmpty()) continue;  // degenerate bake: draws nothing
        QPainterPath crisp;
        crisp.addRect(liveBox);
        bool anyAbove = false;
        cover.query(liveBox.intersected(vis), candidates);
        for (int ri : candidates) {
            const Run& r = runs[(size_t)ri];
            if (r.to >= k) continue;  // at or below the live, not above
            const QRectF box = r.full ? docRect : r.box;
            const QRectF over = box.intersected(liveBox).intersected(vis);
            if (over.isEmpty()) continue;
            anyAbove = true;
            QPainterPath cut;
            cut.addRect(over);
            crisp = crisp.subtracted(cut);
            if (crisp.isEmpty()) break;
        }
        if (anyAbove && crisp.isEmpty()) continue;
        p.save();
        if (anyAbove) p.setClipPath(crisp);
        // Cached crisp draw: blit after the first bake (the clip above
        // still applies to the blit).
        LayerViewCache::instance().paintCachedCrisp(p, *d, k, liveBox,
                                                    zoomEff, &crispStats,
                                                    crispDeadline);
        p.restore();
    }
    if (crispStats.drawn > 0 || crispStats.skipped > 0) {
        const double crispMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - crispT0)
                .count();
        ::pittore::core::log::log_info(
            "[render][segmented] crisp=%ld cached=%ld baked=%ld direct=%ld "
            "skipped=%ld cache=%.1fMB ms=%.2f",
            crispStats.drawn, crispStats.hits, crispStats.baked,
            crispStats.direct, crispStats.skipped,
            LayerViewCache::instance().usageBytes() / 1048576.0, crispMs);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void CanvasView::paintEvent(QPaintEvent* e) {
    const auto t0 = std::chrono::steady_clock::now();
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing, true);
    // Smooth in both directions: zoomed-out minification AND zoomed-in
    // magnification. Nearest on zoom-in turns every edge (including vector
    // art, even supersampled) into hard stair-steps; other editors
    // all smooth-sample the view.
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    p.fillRect(viewport()->rect(), canvasSurround(state_->surroundIndex(), state_->theme()));

    if (!doc()) {
        p.setPen(colorsFor(state_->theme()).textDim);
        p.drawText(viewport()->rect(), Qt::AlignCenter,
                   tr("No document open.\nFile ▸ New (Ctrl+N) or File ▸ Open (Ctrl+O)"));
        return;
    }

    paintDocument(p, e->rect());
    // The brush cursor stays up whether or not Extras are visible (a hidden
    // cursor would blind the brush); selection edges and the other helpers
    // follow the Extras toggle, so Ctrl+H gives a clean view of the work.
    paintBrushCursor(p);
    if (extras_) paintOverlay(p);

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    // Incremental-repaint diagnostics: how long a canvas paint takes, and over
    // what region. Lets us see the real (async) cost behind a layer eye toggle.
    // `proof` tags whether the soft-proof path ran, so a slow frame under a
    // proof profile lines up with the [render][proof] build lines.
    ::pittore::core::log::log_info(
        "[render][paint] dirty=%dx%d at (%d,%d) viewport=%dx%d zoom=%.2f "
        "proof=%d mode=%d ms=%.2f",
        e->rect().width(), e->rect().height(), e->rect().x(), e->rect().y(),
        viewport()->width(), viewport()->height(), zoom(),
        (state_->proofEnabled() || state_->proofGamut()) ? 1 : 0,
        doc() ? static_cast<int>(
                    DisplayModeGovernor::instance().modeFor(doc()))
              : 0,
        ms);
    // Feed the display-mode governor: sustained slow frames on heavy
    // documents degrade Full to Draft automatically (and recover).
    DisplayModeGovernor::instance().noteFrame(doc(), ms);

    // True click→paint latency: a region edit (eye toggle, move/scale frame,
    // stroke flush) stamps its doc rect + monotonic time; the first canvas
    // paint that covers it reports the age. This captures everything the
    // handler timers can't: event-loop queuing, paint scheduling, the region
    // update being coalesced or delayed behind other work.
    const qint64 editAgeUs = state_->takeRegionEditAgeUs();
    if (editAgeUs >= 0) {
        const QRectF vr = documentTransform().mapRect(state_->lastRegionEditRect());
        if (vr.intersects(QRectF(e->rect()))) {
            ::pittore::core::log::log_info(
                "[render][region-vis] click_to_paint=%.3f ms dirty=%dx%d at (%d,%d)",
                editAgeUs / 1000.0, e->rect().width(), e->rect().height(),
                e->rect().x(), e->rect().y());
        }
    }

    // Repaint provenance: the cause that queued this frame and where the frame
    // actually landed. `t` is the whole story behind "the canvas shows the
    // layer somewhere other than the model" — the model and the composite are
    // already covered by the move/render rows — while off/bnds are the
    // placement this frame would paint the active layer at. Emitted last so
    // the line also reports frames that painted nothing but the surround.
    {
        DocumentItem* td = doc();
        canvasTraceFrame(state_->activeTool(), canvasTakeUpdateReason(),
                         e->rect(), viewport()->rect(), documentTransform(), td,
                         td ? td->activeLayer : -1, ms);
    }
}


void CanvasView::paintDocument(QPainter& p) {
    paintDocument(p, QRect());
}


bool CanvasView::proofActive() const {
    if (!state_->proofEnabled() && !state_->proofGamut()) return false;
    syncProofConfig();
    return proofManager_ != nullptr;
}

void CanvasView::syncProofConfig() const {
    const AppSettings& s = state_->settings();
    const bool gamut = state_->proofGamut();
    // Compared field by field: the old formatted fingerprint cost four
    // string builds per paint just to discover nothing had moved. The flag
    // also covers a refused profile, so a missing/broken .icc is one attempt
    // (and one hint), not a file probe on every repaint.
    if (proofAttempted_ && s.proofProfile == proofProfile_ &&
        s.proofIntent == proofIntent_ && s.proofBpc == proofBpc_ &&
        gamut == proofGamut_)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    proofAttempted_ = true;
    proofProfile_ = s.proofProfile;
    proofIntent_ = s.proofIntent;
    proofBpc_ = s.proofBpc;
    proofGamut_ = gamut;
    // The setup moved: whatever the cache holds was proofed for the old one.
    proofCache_ = {};
    proofCacheRect_ = {};
    proofManager_ = std::make_unique<pittore::color::ProofManager>();
    const std::string err =
        proofManager_->setProofProfile(s.proofProfile.toStdString());
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (!err.empty()) {
        proofManager_.reset();
        state_->setStatusHint(tr("Soft proof unavailable: %1")
                                  .arg(QString::fromStdString(err)));
        ::pittore::core::log::log_warning(
            "[render][proof] setup failed profile='%s' ms=%.2f: %s",
            s.proofProfile.toUtf8().constData(), ms, err.c_str());
        return;
    }
    proofManager_->setIntent(
        static_cast<pittore::color::ProofIntent>(qBound(0, s.proofIntent, 3)),
        static_cast<pittore::color::ProofIntent>(qBound(0, s.proofIntent, 3)));
    proofManager_->setBlackPointCompensation(s.proofBpc);
    proofManager_->setGamutCheck(gamut);
    // Which setup is live, for the [render][proof] trail below.
    ::pittore::core::log::log_info(
        "[render][proof] setup profile='%s' intent=%d bpc=%d gamut=%d ms=%.2f",
        s.proofProfile.toUtf8().constData(), s.proofIntent,
        s.proofBpc ? 1 : 0, gamut ? 1 : 0, ms);
}

QImage CanvasView::proofedComposite(const QRect& docRect, QRect* srcRect) const {
    DocumentItem* d = doc();
    if (!d || d->composite.isNull() || !proofManager_) return {};
    const QRect full = d->composite.rect();
    const QRect want = docRect.isEmpty() ? full : docRect.intersected(full);
    if (want.isEmpty()) return {};
    if (srcRect) *srcRect = want;

    // Cache hit: same document, same composite, same document size, and the
    // cached proof covers the request. This is the zoom/pan/idle path — a
    // blit instead of a float conversion + CMS transform over every visible
    // pixel.
    const bool hit = !proofCache_.isNull() && proofCacheDoc_ == d &&
                     proofCacheRev_ == d->revision &&
                     proofCacheDocSize_ == d->composite.size() &&
                     proofCacheRect_.contains(want);
    if (hit) {
        if (srcRect)
            *srcRect = QRect(want.topLeft() - proofCacheRect_.topLeft(),
                             want.size());
        return proofCache_;
    }

    // Cache miss. A request that covers most of the document proofs the
    // whole document once, so every later zoom frame (and every smaller
    // request) is a hit; a small request proofs just itself, which keeps an
    // edit under proof as cheap as it was. The 40 MP guard keeps the extra
    // document-sized copy bounded — beyond it the old per-paint proof simply
    // continues, logged like everything else.
    const std::uint64_t wantArea = static_cast<std::uint64_t>(want.width()) *
                                   static_cast<std::uint64_t>(want.height());
    const std::uint64_t docArea = static_cast<std::uint64_t>(full.width()) *
                                  static_cast<std::uint64_t>(full.height());
    const std::uint64_t kCacheCap = 40ull * 1000ull * 1000ull;
    const bool cacheable = docArea <= kCacheCap;
    const bool buildWhole = want == full || (cacheable && wantArea * 2 >= docArea);
    const QRect build = buildWhole ? full : want;

    const auto t0 = std::chrono::steady_clock::now();
    const QImage proofed =
        proofPreviewImage(build == full ? d->composite : d->composite.copy(build),
                          *proofManager_);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (proofed.isNull()) return {};  // CMS refused: caller falls back
    bool kept = false;
    if (cacheable) {
        proofCache_ = proofed;
        proofCacheDoc_ = d;
        proofCacheRect_ = build;
        proofCacheDocSize_ = d->composite.size();
        proofCacheRev_ = d->revision;
        kept = true;
    }
    // The "why" behind a slow proofed frame: builds are the expensive ones
    // (cache hits cost a blit and are not worth a line each).
    ::pittore::core::log::log_info(
        "[render][proof] build rect=%dx%d+%d+%d doc=%dx%d zoom=%.2f "
        "revision=%llu kept=%d ms=%.2f",
        build.x(), build.y(), build.width(), build.height(), full.width(),
        full.height(), zoom(), static_cast<unsigned long long>(d->revision),
        kept ? 1 : 0, ms);
    if (srcRect && build != want)
        *srcRect = QRect(want.topLeft() - build.topLeft(), want.size());
    return proofed;
}


void CanvasView::paintDocument(QPainter& p, const QRect& viewDirty) {
    // DPR sync: moving the window across mixed-DPR screens changes the
    // paint device ratio with no zoom event. Tile buckets (zoom × DPR)
    // and the checker key (DPR) rebake on miss by construction; scroll
    // ranges still need re-evaluation here in the paint path.
    if (QPaintDevice* dev = p.device()) {
        const qreal now = qreal(dev->devicePixelRatioF());
        if (now > 0.0 && lastPaintDpr_ > 0.0 && now != lastPaintDpr_)
            updateScrollRange();
        if (now > 0.0) lastPaintDpr_ = now;
    }
    DocumentItem* d = doc();
    const QTransform t = documentTransform();
    const QRectF docRect(0, 0, d->size.width(), d->size.height());

    p.save();
    p.setTransform(t, false);

    // Drop shadow separating the canvas from the pasteboard.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 90));
    p.drawRect(docRect.translated(2.0 / d->zoom, 2.0 / d->zoom));

    // Transparency checkerboard, drawn in view space so the squares stay a
    // constant size on screen regardless of zoom. Bounded to the damaged
    // region so a 60×60 layer-eye toggle repaints ~60 cells, not the whole
    // checkerboard of a zoomed-out document.
    p.resetTransform();
    QPainterPath clip;
    clip.addPolygon(t.map(QPolygonF(docRect)));
    if (!viewDirty.isEmpty()) {
        QPainterPath dirty;
        dirty.addRect(QRectF(viewDirty));
        clip = clip.intersected(dirty);
    }
    p.setClipPath(clip);
    const QRectF bounds = clip.boundingRect();
    // Checkerboard as one pattern fill (the old per-cell loop cost ~13k
    // fillRect calls on a zoomed-out 2000×2000 document). Tiles are cached by
    // size + inks (Settings > Canvas > Transparency); the cache holds a
    // handful of entries, so dragging a color wheel never reallocates per
    // repaint. The default 8px cells align to the device pixel grid
    // (multiples of 8), which is exactly how the old loop placed them.
    const AppSettings& prefs = state_->settings();
    const int cell = qBound(4, prefs.transparencyCellPx, 64);
    // Bake the checker at device resolution and tag its DPR, or Qt
    // stretches a 1x pixmap on 150/200% displays and the transparency
    // grid goes soft. Keyed by DPR so a screen move rebakes crisply.
    const qreal dpr = p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0;
    const qreal useDpr = dpr > 0.0 ? dpr : 1.0;
    const QString tileKey = QStringLiteral("%1/%2/%3/%4")
                                .arg(cell)
                                .arg(prefs.transparencyLight.name(QColor::HexArgb),
                                     prefs.transparencyDark.name(QColor::HexArgb))
                                .arg(QString::number(useDpr, 'f', 2));
    static QMap<QString, QPixmap> checkerCache;
    auto it = checkerCache.find(tileKey);
    if (it == checkerCache.end()) {
        if (checkerCache.size() >= 8) checkerCache.clear();
        const int dev = qMax(1, qRound(2 * cell * useDpr));
        QPixmap pm(dev, dev);
        pm.setDevicePixelRatio(useDpr);
        pm.fill(Qt::transparent);
        QPainter qp(&pm);
        // Painter coords are logical (Qt maps to device via the DPR tag),
        // so paint in logical cell units, not device px.
        qp.fillRect(0, 0, cell, cell, prefs.transparencyLight);
        qp.fillRect(cell, cell, cell, cell, prefs.transparencyLight);
        qp.fillRect(cell, 0, cell, cell, prefs.transparencyDark);
        qp.fillRect(0, cell, cell, cell, prefs.transparencyDark);
        it = checkerCache.insert(tileKey, pm);
    }
    p.fillRect(bounds, QBrush(*it));

    p.setTransform(t, false);
    // >>> Engine seam: this is the one call the Vulkan presentation path
    // replaces with a swapchain blit of the composited tile grid (R56/R91).
    // Vector art with simple paint draws straight from geometry at view
    // resolution (paintSegmented); everything else keeps the flattened blit.
    // Soft proof bypasses the segmented path so every pixel is proofed.
    const bool proof = proofActive();
    QRect docDirty;
    if (!viewDirty.isEmpty()) {
        // Sample only the damaged source stretch so a region edit paints
        // the sub-rect, not a full 2000×2000 texture transform. Snap the
        // inverse-mapped rect to whole document pixels: at fractional zoom
        // an unaligned sub-rect lets the smoothing grid wobble the painted
        // pixels between flushes (the "pixels moving" brush artifact).
        const QRectF docDirtyF =
            t.inverted().mapRect(QRectF(viewDirty)).intersected(docRect);
        docDirty = docDirtyF.isEmpty()
                       ? QRect()
                       : docDirtyF.toAlignedRect().intersected(d->composite.rect());
    }
    // Display-mode content: exact replacements for the classic
    // composite+segmented path below. Any false return falls through to it,
    // so these paths can only ever skip work, never change pixels.
    bool contentDone = false;
    // Set when a content path already put the composite base on screen. Only
    // the tiled path can do that and still return false: it lays the base
    // down, draws every tile that had finished, and reports that some were
    // missing. The segmented walk below then owns only the live draws - if
    // it blitted the base again it would cover those ready tiles and the
    // frame would drop back to the resampled composite.
    bool baseDrawn = false;
    if (!proof) {
        const double zoomEff =
            std::hypot(t.m11(), t.m12()) *
            (p.device() ? p.device()->devicePixelRatioF() : 1.0);
        switch (DisplayModeGovernor::instance().modeFor(d)) {
            case CanvasDisplayMode::Draft:
                contentDone = paintDraftContent(p, *d, t, zoomEff, docRect,
                                                viewDirty, [this] {
                                                    viewport()->update();
                                                });
                break;
            case CanvasDisplayMode::Outline:
                contentDone = paintOutlineContent(p, *d, t, zoomEff, docRect,
                                                 viewDirty);
                break;
            case CanvasDisplayMode::Full:
                contentDone = paintTiledContent(
                    p, *d, t, zoomEff, docRect, viewDirty,
                    [this] { viewport()->update(); });
                baseDrawn = !contentDone && !d->composite.isNull();
                break;
        }
    }
    const bool segmentedEligible = !contentDone && !d->composite.isNull();
    // The live walk draws over whatever base is on screen; it never lays the
    // base down itself when the tiled path already did (see paintSegmented).
    const bool drewLive = segmentedEligible && !proof &&
                          paintSegmented(p, t, docRect, viewDirty, baseDrawn);
    if (segmentedEligible && !drewLive && !baseDrawn) {
        if (proof) {
            // Full repaint proofs (and draws) the whole document; a damaged
            // rect proofs just that stretch. Both sample through the cache,
            // so a zoom that only moves the viewport is a blit.
            if (viewDirty.isEmpty()) {
                QRect src;
                const QImage proofed = proofedComposite(QRect(), &src);
                p.drawImage(docRect, proofed.isNull() ? d->composite : proofed);
            } else if (!docDirty.isEmpty()) {
                QRect src = docDirty;
                const QImage proofed = proofedComposite(docDirty, &src);
                if (!proofed.isNull())
                    p.drawImage(QRectF(docDirty), proofed, src);
                else
                    p.drawImage(QRectF(docDirty), d->composite, docDirty);
            }
        } else if (viewDirty.isEmpty()) {
            p.drawImage(docRect, d->composite);
        } else if (!docDirty.isEmpty()) {
            p.drawImage(QRectF(docDirty), d->composite, docDirty);
        }
    }
    p.setClipping(false);

    // Quick Mask tints the unselected area red at 50%, as editors conventionally do.
    if (state_->quickMask()) {
        p.setBrush(QColor(255, 0, 0, 128));
        p.setPen(Qt::NoPen);
        if (d->selection.isEmpty()) {
            p.drawRect(docRect);
        } else if (d->selectionIsMask && !d->selectionMask.isNull()) {
            // Tint from the channel itself: red where coverage is 0, so the
            // tint hugs the object's true boundary.
            const QImage& m = d->selectionMask;
            QImage tint(m.size(), QImage::Format_ARGB32);
            tint.fill(Qt::transparent);
            for (int yy = 0; yy < m.height(); ++yy) {
                const uchar* srow = m.constScanLine(yy);
                QRgb* trow = reinterpret_cast<QRgb*>(tint.scanLine(yy));
                for (int xx = 0; xx < m.width(); ++xx)
                    trow[xx] = qRgba(255, 0, 0, (255 - srow[xx]) * 128 / 255);
            }
            p.drawImage(docRect, tint);
        } else {
            QPainterPath outside;
            outside.addRect(docRect);
            QPainterPath inside;
            if (d->selectionIsEllipse) inside.addEllipse(d->selection);
            else inside.addRect(d->selection);
            p.drawPath(outside.subtracted(inside));
        }
    }
    p.restore();

    // Canvas border.
    p.setPen(QPen(QColor(0, 0, 0, 160), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(t.map(QPolygonF(docRect)));
}

}  // namespace pittore::ui
