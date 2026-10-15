// Crisp vector overlay: conservative plan plus a cache-safe painter.
// GUI thread for the plan; drawCrispArt is worker-safe (immutable inputs
// only, no per-layer mutable state).
#include "ui/canvas/paint/canvas_crisp.h"

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QSet>
#include <QTransform>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "ui/app_state.h"
#include "ui/canvas/paint/canvas_layer_cache.h"
#include "ui/persona/vector_profile.h"
#include "ui/persona/vector_raster.h"
#include "ui/persona/vector_view.h"
#include "engine/vector/vector_art.h"

namespace pittore::ui {

namespace {

// Fully-opaque vector content: any translucency (node opacity, fill or
// stroke alpha, gradient stops), paint servers, markers, clips/masks,
// meshes or filters means the layer keeps its correctly blended composite
// raster. Same rule as the live walk; duplicated here so this module needs
// no access to it.
bool crispOpaque(const LayerItem& l) {
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

// Uniform-stroke outline for cached fills: QPainterPathStroker generates
// what the QPen path rasterizes (same width/caps/joins/dash), but once per
// bake instead of once per frame. Empty when there is no stroke or the
// path is degenerate (the caller keeps the pen fallback, which preserves
// dot rendering).
QPainterPath strokeOutline(const pittore::vector::ArtNode& node,
                           const QPainterPath& path) {
    const pittore::vector::ArtPaint& paint = node.paint;
    if (!paint.hasStroke || path.isEmpty()) return {};
    QPainterPathStroker stroker;
    stroker.setWidth(std::max(0.01, paint.strokeWidth));
    stroker.setCapStyle(artCapStyle(paint.cap));
    stroker.setJoinStyle(artJoinStyle(paint.join));
    if (paint.hasDash && !paint.dash.empty()) {
        QList<qreal> pattern;
        pattern.reserve((int)paint.dash.size());
        bool any = false;
        for (float v : paint.dash) {
            pattern.push_back(std::max(0.0, (double)v));
            if (v > 0.0) any = true;
        }
        // All-zero pattern draws nothing either way; solid keeps parity
        // with the pen fallback.
        if (any) {
            stroker.setDashPattern(pattern);
            stroker.setDashOffset(paint.dashOffset);
        }
    }
    return stroker.createStroke(path);
}

}  // namespace

double crispZoomBucket(double zoom) {
    if (!(zoom > 0.0)) return 1.0;
    const double bucketed = std::round(zoom * 4.0) / 4.0;
    // No practical ceiling: a tile's raster is bounded by its pixel cap (the
    // grid shrinks the tile in document space as the bucket grows), so the
    // bucket itself tracks deep zoom instead of pinning at 64x and silently
    // re-baking the wrong density.
    return std::clamp(bucketed, 1.0 / 64.0, 1.0e6);
}

void CoverIndex::build(const std::vector<CoverRun>& runs,
                       const QRectF& docRect) {
    runs_ = runs;
    docRect_ = docRect;
    fullRuns_.clear();
    cols_ = qMax(1, qCeil(docRect.width() / cell_));
    rows_ = qMax(1, qCeil(docRect.height() / cell_));
    cells_.assign((size_t)cols_ * (size_t)rows_, {});
    seen_.assign(runs_.size(), 0);
    stamp_ = 0;
    for (size_t i = 0; i < runs_.size(); ++i) {
        if (runs_[i].full || runs_[i].box.isEmpty()) {
            if (runs_[i].full) fullRuns_.push_back((int)i);
            continue;
        }
        const QRectF b = runs_[i].box.intersected(docRect);
        if (b.isEmpty()) continue;
        const int x0 = qBound(0, (int)(b.left() / cell_), cols_ - 1);
        const int y0 = qBound(0, (int)(b.top() / cell_), rows_ - 1);
        const int x1 = qBound(0, (int)(b.right() / cell_), cols_ - 1);
        const int y1 = qBound(0, (int)(b.bottom() / cell_), rows_ - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                cells_[(size_t)y * (size_t)cols_ + (size_t)x].push_back((int)i);
    }
}

void CoverIndex::query(const QRectF& box, std::vector<int>& out) const {
    out.clear();
    if (runs_.empty() || box.isEmpty()) return;
    for (int i : fullRuns_) out.push_back(i);
    const QRectF b = box.intersected(docRect_);
    if (b.isEmpty()) return;
    if (++stamp_ < 0) {  // wrap guard: practically unreachable, stay correct
        std::fill(seen_.begin(), seen_.end(), 0);
        stamp_ = 1;
    }
    const int x0 = qBound(0, (int)(b.left() / cell_), cols_ - 1);
    const int y0 = qBound(0, (int)(b.top() / cell_), rows_ - 1);
    const int x1 = qBound(0, (int)(b.right() / cell_), cols_ - 1);
    const int y1 = qBound(0, (int)(b.bottom() / cell_), rows_ - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int i : cells_[(size_t)y * (size_t)cols_ + (size_t)x]) {
                if (seen_[(size_t)i] == stamp_) continue;
                seen_[(size_t)i] = stamp_;
                out.push_back(i);
            }
        }
    }
}

QRectF crispLayerBox(const DocumentItem& doc, int index) {
    const QRectF docRect(0, 0, doc.size.width(), doc.size.height());
    if (index < 0 || index >= doc.layers.size()) return docRect;
    const LayerItem& l = doc.layers[index];
    if (!l.art) return docRect;
    // Marker glyphs can extend past the bake box: never cull those.
    const auto& pt = l.art->paint;
    if (!pt.markerStart.empty() || !pt.markerMid.empty() ||
        !pt.markerEnd.empty())
        return docRect;
    const LayerDrawSource src = layerDrawSource(l);
    if (!src.img) return docRect;  // no bake box: cannot prove off-view
    const double sc = std::max({std::abs(l.scaleX), std::abs(l.scaleY), 1e-9});
    const double m =
        pt.hasStroke ? pt.strokeWidth * sc / 2.0 + 2.0 : 2.0;
    QRectF box = layerBounds(doc, l);
    if (box.isEmpty()) return box;
    return box.adjusted(-m, -m, m, m).intersected(docRect);
}

QVector<int> planCrispOverlay(const DocumentItem& doc,
                              const QRectF& visibleDoc) {
    QVector<int> plan;
    const QVector<int> drawable = vectorViewOrder(doc);
    if (drawable.isEmpty()) return plan;
    const QRectF docRect(0, 0, doc.size.width(), doc.size.height());
    const QRectF vis = visibleDoc.isEmpty() ? docRect : visibleDoc;
    const QSet<int> drawableSet(drawable.begin(), drawable.end());

    QVector<char> effVis;
    QVector<int> effParent;
    doc.effectiveVisibility(effVis, effParent);

    // Pixel runs between the drawables (panel order): only their boxes
    // matter. A drawable overlapped anywhere by a run stacked above it is
    // vetoed outright - no partial clipping, so every planned layer paints
    // correctly inside any sub-rect on its own.
    struct Run {
        int from = -1;
        int to = -1;
        QRectF box;
        bool full = false;
    };
    std::vector<Run> runs;
    {
        int w = (int)doc.layers.size() - 1;
        while (w >= 0) {
            if (w < effVis.size() && drawableSet.contains(w) && effVis[w]) {
                --w;
                continue;
            }
            const int runTop = w;
            while (w >= 0 && (w >= effVis.size() ||
                              !drawableSet.contains(w) || !effVis[w]))
                --w;
            bool anyVisible = false;
            for (int k = runTop; k > w; --k) {
                if (k < effVis.size() && doc.layers[k].visible && effVis[k]) {
                    anyVisible = true;
                    break;
                }
            }
            if (!anyVisible) continue;
            // Union of member stage boxes; anything unbounded (groups with
            // tone spans, adjustments, styles, missing pixels) covers all.
            Run r;
            r.from = w + 1;
            r.to = runTop;
            bool any = false;
            for (int k = r.from; k <= r.to; ++k) {
                if (k < 0 || k >= doc.layers.size()) continue;
                const LayerItem& l = doc.layers[k];
                if (!l.visible || k >= effVis.size() || !effVis[k]) continue;
                if (l.kind == LayerItem::Kind::Group) {
                    if (l.toneBlendGroup) {
                        r.full = true;
                        break;
                    }
                    continue;
                }
                if (l.kind != LayerItem::Kind::Pixel || !l.style.empty() ||
                    !l.pixels) {
                    r.full = true;
                    break;
                }
                const QRectF b = stageBounds(doc, l);
                if (b.isEmpty()) continue;
                r.box = any ? r.box.united(b) : b;
                any = true;
            }
            if (!r.full && !any) r.box = QRectF();
            runs.push_back(r);
        }
    }

    // Bottom-to-top (paint order): panel index size-1 down to 0. The
    // coverage index keeps the per-layer overlap test sublinear: a flat
    // scan over every run per layer turns quadratic past a few thousand
    // layers (tens of seconds on an 80k-layer map).
    std::vector<CoverRun> coverRuns;
    coverRuns.reserve(runs.size());
    for (const Run& r : runs) coverRuns.push_back(CoverRun{r.box, r.full});
    CoverIndex cover;
    cover.build(coverRuns, docRect);
    std::vector<int> candidates;
    for (int i = drawable.size() - 1; i >= 0; --i) {
        const int k = drawable[i];
        if (k < 0 || k >= doc.layers.size()) continue;
        const LayerItem& l = doc.layers[k];
        if (!crispOpaque(l)) continue;
        const QRectF box = crispLayerBox(doc, k).intersected(vis);
        if (box.isEmpty()) continue;
        bool covered = false;
        cover.query(box, candidates);
        for (int ri : candidates) {
            if (runs[(size_t)ri].to >= k) continue;  // at/below, not above
            const QRectF over =
                (runs[(size_t)ri].full ? docRect : runs[(size_t)ri].box)
                    .intersected(box);
            if (!over.isEmpty()) {
                covered = true;
                break;
            }
        }
        if (!covered) plan.push_back(k);
    }
    return plan;
}

CrispPaths buildCrispPaths(const pittore::vector::ArtNode& node) {
    CrispPaths out;
    if (node.isEmpty()) return out;
    out.fill = artNodePath(node);
    if (out.fill.isEmpty()) return out;
    if (!node.evenOdd) out.fill.setFillRule(Qt::WindingFill);
    out.empty = false;
    if (node.paint.hasStroke) {
        const bool profiled =
            node.paint.hasProfile && !node.paint.profile.empty();
        out.stroke = profiled ? expandStrokePath(node)
                              : strokeOutline(node, out.fill);
        out.penStroke = out.stroke.isEmpty();
    }
    return out;
}

bool drawCrispPaths(QPainter& p, const CrispPaths& paths,
                    const pittore::vector::ArtNode& node, double scaleX,
                    double scaleY, double offX, double offY) {
    if (paths.empty) return false;
    const double* m = node.matrix;
    const QTransform nodeToDoc =
        QTransform(m[0], m[1], m[2], m[3], m[4], m[5]) *
        QTransform().scale(scaleX, scaleY) *
        QTransform().translate(offX, offY);
    const pittore::vector::ArtPaint& paint = node.paint;
    if (paint.hasFill) {
        p.save();
        p.setTransform(nodeToDoc * p.transform(), false);
        p.setOpacity(std::clamp(node.opacity, 0.0, 1.0));
        p.setPen(Qt::NoPen);
        p.setBrush(artFillBrush(node));
        p.drawPath(paths.fill);
        p.restore();
    }
    if (paint.hasStroke) {
        p.save();
        p.setTransform(nodeToDoc * p.transform(), false);
        p.setOpacity(std::clamp(node.opacity, 0.0, 1.0));
        if (!paths.penStroke) {
            p.setPen(Qt::NoPen);
            p.setBrush(artStrokeBrush(node));
            p.drawPath(paths.stroke);
        } else {
            QPen pen(artStrokeBrush(node),
                     std::max(0.01, paint.strokeWidth));
            pen.setCosmetic(false);
            pen.setCapStyle(artCapStyle(paint.cap));
            pen.setJoinStyle(artJoinStyle(paint.join));
            applyDashToPen(pen, paint);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(paths.fill);
        }
        p.restore();
    }
    return true;
}

bool drawCrispArt(QPainter& p, const pittore::vector::ArtNode& node,
                  double scaleX, double scaleY, double offX, double offY) {
    return drawCrispPaths(p, buildCrispPaths(node), node, scaleX, scaleY,
                          offX, offY);
}

bool drawCrispLayer(QPainter& p, const DocumentItem& doc, int index) {
    if (index < 0 || index >= doc.layers.size()) return false;
    const LayerItem& l = doc.layers[index];
    if (!l.art) return false;
    return drawCrispArt(p, *l.art, l.scaleX, l.scaleY, l.offset.x(),
                        l.offset.y());
}

void drawOutlineLayer(QPainter& p, const DocumentItem& doc, int index) {
    if (index < 0 || index >= doc.layers.size()) return;
    const LayerItem& l = doc.layers[index];
    if (!l.art || l.art->isEmpty()) return;
    const double* m = l.art->matrix;
    const QTransform nodeToDoc =
        QTransform(m[0], m[1], m[2], m[3], m[4], m[5]) *
        QTransform().scale(l.scaleX, l.scaleY) *
        QTransform().translate(l.offset.x(), l.offset.y());
    QPainterPath path = artNodePath(*l.art);
    if (path.isEmpty()) return;
    p.save();
    p.setTransform(nodeToDoc * p.transform(), false);
    // Cosmetic: one raster pixel at the caller's bake density, so contours
    // stay hairlines at any zoom instead of scaling with the geometry.
    p.setPen(QPen(QColor(40, 40, 40, 220), 0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.restore();
}

bool paintOutlineContent(QPainter& p, const DocumentItem& doc,
                         const QTransform& docToView, double zoomEff,
                         const QRectF& docRect, const QRect& viewDirty) {
    const QRectF vis = viewDirty.isEmpty()
                           ? docRect
                           : docToView.inverted()
                                 .mapRect(QRectF(viewDirty))
                                 .intersected(docRect);
    if (vis.isEmpty()) return true;
    const double bucket = crispZoomBucket(zoomEff);
    QVector<char> effVis;
    QVector<int> effParent;
    doc.effectiveVisibility(effVis, effParent);
    LayerViewCache& cache = LayerViewCache::instance();
    p.save();
    p.setTransform(docToView, false);
    for (int k = (int)doc.layers.size() - 1; k >= 0; --k) {
        if (k >= effVis.size() || !effVis[k]) continue;
        const QRectF box = crispLayerBox(doc, k).intersected(vis);
        if (box.isEmpty()) continue;
        QRectF cachedBox;
        if (const QImage* hit =
                cache.find(doc, k, LayerViewCache::kOutline, bucket,
                           &cachedBox)) {
            p.drawImage(cachedBox, *hit);
            continue;
        }
        // Bake at the bucket density, framed to the layer's visible box.
        const int iw = qMax(1, qRound(box.width() * bucket));
        const int ih = qMax(1, qRound(box.height() * bucket));
        if ((qint64)iw * ih > 16ll * 1024 * 1024) continue;  // absurd: skip
        QImage img(iw, ih, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter ip(&img);
            ip.setRenderHint(QPainter::Antialiasing, true);
            ip.setTransform(QTransform(bucket, 0, 0, bucket,
                                       -box.x() * bucket, -box.y() * bucket),
                            false);
            drawOutlineLayer(ip, doc, k);
        }
        p.drawImage(box, img);
        cache.store(doc, k, LayerViewCache::kOutline, bucket, std::move(img),
                    box);
    }
    p.restore();
    return true;
}

}  // namespace pittore::ui
