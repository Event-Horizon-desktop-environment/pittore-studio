// Per-layer view cache: budgeted LRU of display-density vector rasters.
// GUI thread only.
#include "ui/canvas/paint/canvas_layer_cache.h"

#include <QPainter>
#include <QTransform>

#include <bit>
#include <limits>

#include "ui/app_state.h"
#include "ui/canvas/paint/canvas_crisp.h"
#include "engine/core/log.h"
#include "engine/vector/vector_art.h"

namespace pittore::ui {

namespace {

// One entry never exceeds this (a zoomed-in full-document layer bakes
// through the direct path instead).
constexpr qint64 kMaxEntryPixels = 16ll * 1024 * 1024;
// Dead-document entries age out through this; live documents with more
// cached layers than this thrash, which the budget already prevents.
constexpr std::size_t kMaxEntries = 4096;

quint64 mixHash(quint64 h, quint64 v) {
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

}  // namespace

LayerViewCache& LayerViewCache::instance() {
    static LayerViewCache cache;
    return cache;
}

quint64 LayerViewCache::contentKey(const DocumentItem& doc,
                                   int index) const {
    const LayerItem& l = doc.layers[index];
    quint64 h = 1469598103934665603ull;
    h = mixHash(h, l.sourceStamp);
    h = mixHash(h, l.styledRev);
    h = mixHash(h, std::bit_cast<quint64>(l.offset.x()));
    h = mixHash(h, std::bit_cast<quint64>(l.offset.y()));
    h = mixHash(h, std::bit_cast<quint64>(l.scaleX));
    h = mixHash(h, std::bit_cast<quint64>(l.scaleY));
    h = mixHash(h, (quint64)l.opacity);
    return h;
}

double LayerViewCache::cacheWeight(const DocumentItem& doc,
                                   int index) const {
    if (index < 0 || index >= doc.layers.size()) return 0.0;
    const LayerItem& l = doc.layers[index];
    if (!l.art || l.art->isEmpty()) return 0.0;  // pixel layers blit already
    const auto& pt = l.art->paint;
    if (!pt.hasFill && !pt.hasStroke) return 0.0;  // draws nothing
    double w = 1.0;
    if (pt.hasGradient || pt.hasStrokeGradient || pt.hasMesh ||
        !pt.patternId.empty())
        w += 2.0;
    if (!l.flatArt.empty() || l.sharedNode) w += 2.0;
    if (pt.hasStroke) w += 1.0;
    if (pt.hasDash) w += 1.0;
    return w;
}

const QImage* LayerViewCache::find(const DocumentItem& doc, int layerIndex,
                                    int kind, double zoomBucket,
                                    QRectF* docBox) {
    if (layerIndex < 0 || layerIndex >= doc.layers.size()) return nullptr;
    if (cacheWeight(doc, layerIndex) <= 0.0) return nullptr;
    const Key key{&doc, layerIndex, kind, contentKey(doc, layerIndex),
                  zoomBucket};
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.img.isNull()) return nullptr;
    it->second.tick = ++tick_;
    if (docBox) *docBox = it->second.box;
    return &it->second.img;
}

void LayerViewCache::store(const DocumentItem& doc, int layerIndex, int kind,
                           double zoomBucket, QImage img,
                           const QRectF& docBox) {
    if (layerIndex < 0 || layerIndex >= doc.layers.size()) return;
    if (img.isNull() || docBox.isEmpty()) return;
    if ((qint64)img.width() * img.height() > kMaxEntryPixels) return;
    if (cacheWeight(doc, layerIndex) <= 0.0) return;
    const Key key{&doc, layerIndex, kind, contentKey(doc, layerIndex),
                  zoomBucket};
    auto it = entries_.find(key);
    if (it != entries_.end())
        usedBytes_ -= (qint64)it->second.img.sizeInBytes();
    Entry e;
    e.img = std::move(img);
    e.box = docBox;
    e.tick = ++tick_;
    usedBytes_ += (qint64)e.img.sizeInBytes();
    entries_.insert_or_assign(key, std::move(e));
    evict();
}

void LayerViewCache::setBudgetBytes(qint64 bytes) {
    budgetBytes_ = bytes > 0 ? bytes : 1;
    evict();
}

void LayerViewCache::paintCachedCrisp(QPainter& p, const DocumentItem& doc,
                                      int index, const QRectF& docBox,
                                      double zoomEff, CrispStats* stats,
                                      std::chrono::steady_clock::time_point deadline) {
    const double bucket = crispZoomBucket(zoomEff);
    QRectF cachedBox;
    if (const QImage* hit = find(doc, index, kCrisp, bucket, &cachedBox)) {
        p.drawImage(cachedBox, *hit);
        if (stats) {
            ++stats->drawn;
            ++stats->hits;
        }
        return;
    }
    if (docBox.isEmpty()) return;
    // Bake deadline: the frame only rasterizes what fits. Past it the layer
    // keeps its (always correct) composite pixels this frame; the tile baker
    // sharpens it behind. Skipping is never wrong - the composite beneath is
    // complete - it just defers crispness off the critical path.
    if (std::chrono::steady_clock::now() >= deadline) {
        if (stats) ++stats->skipped;
        return;
    }
    const int iw = qMax(1, qRound(docBox.width() * bucket));
    const int ih = qMax(1, qRound(docBox.height() * bucket));
    if ((qint64)iw * ih > kMaxEntryPixels || cacheWeight(doc, index) <= 0.0) {
        drawCrispLayer(p, doc, index);  // too big to keep: direct draw
        if (stats) {
            ++stats->drawn;
            ++stats->direct;
        }
        return;
    }
    QImage img(iw, ih, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter ip(&img);
        ip.setRenderHint(QPainter::Antialiasing, true);
        ip.setRenderHint(QPainter::SmoothPixmapTransform, true);
        ip.setTransform(QTransform(bucket, 0, 0, bucket,
                                   -docBox.x() * bucket,
                                   -docBox.y() * bucket),
                        false);
        drawCrispLayer(ip, doc, index);
    }
    p.drawImage(docBox, img);
    store(doc, index, kCrisp, bucket, std::move(img), docBox);
    if (stats) {
        ++stats->drawn;
        ++stats->baked;
    }
}

void LayerViewCache::evict() {
    // Oldest ticks first; a full sort on every store would be O(n log n)
    // per frame, so sweep linearly - stores are bounded by visible layers.
    qint64 droppedBytes = 0;
    long dropped = 0;
    while ((!entries_.empty() &&
            (usedBytes_ > budgetBytes_ || entries_.size() > kMaxEntries))) {
        quint64 oldest = std::numeric_limits<quint64>::max();
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.tick < oldest) {
                oldest = it->second.tick;
                victim = it;
            }
        }
        if (victim == entries_.end()) break;
        droppedBytes += (qint64)victim->second.img.sizeInBytes();
        usedBytes_ -= (qint64)victim->second.img.sizeInBytes();
        entries_.erase(victim);
        ++dropped;
    }
    // Bulk evictions are the jank signal (thrashing buckets): worth a line.
    if (dropped > 64)
        ::pittore::core::log::log_info(
            "[render][viewcache] evicted=%ld MB=%.1f usage=%.1fMB", dropped,
            droppedBytes / 1048576.0, usedBytes_ / 1048576.0);
}

}  // namespace pittore::ui
