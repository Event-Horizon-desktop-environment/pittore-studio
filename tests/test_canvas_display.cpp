// Display modes, crisp overlay plan/painter, view cache and tile store.
// Headless: paints into QImages, never onto a widget. QApplication, not
// QCoreApplication: importing an SVG fixture with <text> builds glyph paths
// through the font database, which needs a QGuiApplication behind it.
#include <QApplication>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <QTransform>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>

#include "test_util.h"
#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/canvas/paint/canvas_crisp.h"
#include "ui/canvas/paint/canvas_display_mode.h"
#include "ui/canvas/paint/canvas_layer_cache.h"
#include "ui/canvas/paint/canvas_tile_store.h"
#include "ui/canvas/shared/canvas_helpers.h"
#include "ui/svg_parts.h"
#include "engine/compute/adjust.h"
#include "engine/core/log.h"

using namespace pittore::ui;

namespace {

const char* kSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
    "<g id=\"G\">"
    "<rect x=\"10\" y=\"10\" width=\"40\" height=\"20\" fill=\"#ff0000\"/>"
    "<rect x=\"20\" y=\"50\" width=\"30\" height=\"30\" fill=\"#00ff00\"/>"
    "</g>"
    "</svg>";

DocumentItem* openVectorDoc(AppState& state) {
    SvgImportResult result;
    int dpi = 96;
    QString error;
    if (!svgPartsImport(QByteArray(kSvg), &result, &dpi, &error)) return nullptr;
    QString openError;
    if (!state.openSvgParts(QStringLiteral("vec.svg"), result, dpi, &openError))
        return nullptr;
    return state.activeDocument();
}

void test_zoom_bucket() {
    CHECK(crispZoomBucket(1.0) == 1.0);
    CHECK(crispZoomBucket(0.13) == 0.25);
    CHECK(crispZoomBucket(2.9) == 3.0);
    CHECK(crispZoomBucket(0.0) == 1.0);
    CHECK(crispZoomBucket(-4.0) == 1.0);
    // Nearby fractional zooms share a bucket instead of re-baking.
    CHECK(crispZoomBucket(1.10) == crispZoomBucket(1.12));
    CHECK(crispZoomBucket(1.10) != crispZoomBucket(1.40));
}

void test_zoom_range_and_caps() {
    // Zoom is no longer capped at the old 32x step ceiling: the ladder
    // ends there by design and direct sets continue geometrically to the
    // sanity rails (test_infinite_zoom pins the pass-through contract).
    CHECK(zoomSteps().first() <= 0.001);
    CHECK(zoomSteps().last() == 32.0);
    CHECK(kMaxZoom == 1e9);
    CHECK(kMinZoom == 1e-9);
    // Buckets track deep zoom instead of clamping at 64x.
    CHECK(crispZoomBucket(500.0) == 500.0);
    CHECK(crispZoomBucket(100000.0) == 100000.0);
    // Any zoom bakes. Past the point where a fixed 256-doc-px tile would
    // outgrow the pixel cap the tile now shrinks in document space instead
    // of the store refusing: refusing stranded Draft on the soft composite
    // above 8x, which is exactly where deep zoom went low res.
    CanvasTileStore& store = CanvasTileStore::instance();
    CHECK(store.bakeAllowedAt(1.0));
    CHECK(store.bakeAllowedAt(8.0));
    CHECK(store.bakeAllowedAt(8.5));
    CHECK(store.bakeAllowedAt(100.0));
    CHECK(store.bakeAllowedAt(1000.0));
    // Degenerate zooms still refuse.
    CHECK(!store.bakeAllowedAt(0.0));
    CHECK(!store.bakeAllowedAt(std::numeric_limits<double>::quiet_NaN()));
    // A tile never outgrows the pixel cap at any zoom - that is what keeps
    // deep bakes affordable, so the store never has to give up and leave the
    // classic walk (whose frame budget cannot cover a whole frame).
    for (const double z : {0.5, 1.0, 7.0, 8.5, 12.0, 100.0, 1000.0}) {
        const int edge = CanvasTileStore::tileImageEdge(z);
        CHECK(edge > 0);
        CHECK(edge <= 2048);
    }
}

// Crisp happens at every zoom. The tile plan must not depend on the zoom
// bucket: below the old 2x threshold it fell back to the overlap-vetoed
// plan, which dropped tens of thousands of layers to the soft composite.
void test_tile_plan_zoom_independent() {
    AppState state;
    DocumentItem* d = openVectorDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const QRectF vis(0, 0, d->size.width(), d->size.height());

    long crispLow = 0, compLow = 0, simpleLow = 0;
    long crispHigh = 0, compHigh = 0, simpleHigh = 0;
    CanvasTileStore::describeTilePlan(*d, vis, 0.5, &crispLow, &compLow,
                                      &simpleLow);
    CanvasTileStore::describeTilePlan(*d, vis, 4.0, &crispHigh, &compHigh,
                                      &simpleHigh);
    // Every visible crisp-reproducible layer is planned at any zoom.
    CHECK(crispLow > 0);
    CHECK(crispLow == crispHigh);
    CHECK(compLow == compHigh);
    CHECK(simpleLow == 0);
    CHECK(simpleLow == simpleHigh);
    // A full-document cover adds exactly one composite blit, also
    // zoom-independent (it never vetoes the layers below out of the plan:
    // stack order handles the occlusion).
    LayerItem cover;
    cover.kind = LayerItem::Kind::Pixel;
    cover.name = QStringLiteral("cover");
    d->layers.prepend(cover);
    long crispCover = 0, compCover = 0, simpleCover = 0;
    CanvasTileStore::describeTilePlan(*d, vis, 0.5, &crispCover, &compCover,
                                      &simpleCover);
    CHECK(crispCover == crispLow);
    CHECK(compCover == compLow + 1);
    CHECK(simpleCover == 0);
}

void test_governor() {
    DisplayModeGovernor& gov = DisplayModeGovernor::instance();
    CHECK(gov.modeFor(nullptr) == CanvasDisplayMode::Full);
    gov.noteFrame(nullptr, 500.0);
    CHECK(gov.modeFor(nullptr) == CanvasDisplayMode::Full);

    AppState state;
    DocumentItem* small = state.addDocument(QStringLiteral("small"),
                                            QSize(64, 64), 96);
    CHECK(small != nullptr);
    if (!small) return;
    for (int i = 0; i < 10; ++i) gov.noteFrame(small, 500.0);
    CHECK(gov.modeFor(small) == CanvasDisplayMode::Full);  // too few layers
    gov.setOverride(small, CanvasDisplayMode::Outline);
    CHECK(gov.modeFor(small) == CanvasDisplayMode::Outline);
    for (int i = 0; i < 10; ++i) gov.noteFrame(small, 500.0);
    CHECK(gov.modeFor(small) == CanvasDisplayMode::Outline);  // manual wins
    gov.setOverride(small, std::nullopt);
    CHECK(gov.modeFor(small) == CanvasDisplayMode::Full);

    DocumentItem* big = state.addDocument(QStringLiteral("big"),
                                          QSize(64, 64), 96);
    CHECK(big != nullptr);
    if (!big) return;
    for (int i = 0; i < 2500; ++i) big->layers.append(LayerItem{});
    CHECK(gov.modeFor(big) == CanvasDisplayMode::Full);
    gov.noteFrame(big, 60.0);
    gov.noteFrame(big, 60.0);
    CHECK(gov.modeFor(big) == CanvasDisplayMode::Full);  // need 3 in a row
    gov.noteFrame(big, 60.0);
    CHECK(gov.modeFor(big) == CanvasDisplayMode::Draft);
    for (int i = 0; i < 59; ++i) gov.noteFrame(big, 10.0);
    CHECK(gov.modeFor(big) == CanvasDisplayMode::Draft);  // hysteresis
    gov.noteFrame(big, 10.0);
    CHECK(gov.modeFor(big) == CanvasDisplayMode::Full);  // recovered
    gov.forget(small);
    gov.forget(big);
}

void test_plan_and_cache() {
    AppState state;
    DocumentItem* d = openVectorDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const QRectF docRect(0, 0, d->size.width(), d->size.height());

    const QVector<int> plan = planCrispOverlay(*d, docRect);
    CHECK(!plan.isEmpty());  // both rects vetted, opaque, uncovered
    if (plan.isEmpty()) return;

    for (int k : plan) {
        const QRectF box = crispLayerBox(*d, k);
        CHECK(!box.isEmpty());
    }
    // Off-view plan: nothing qualifies.
    CHECK(planCrispOverlay(*d, QRectF(1000, 1000, 10, 10)).isEmpty());

    // A full-document pixel run stacked above vetoes every crisp layer.
    LayerItem cover;
    cover.kind = LayerItem::Kind::Pixel;
    cover.name = QStringLiteral("cover");
    d->layers.prepend(cover);
    CHECK(planCrispOverlay(*d, docRect).isEmpty());
    d->layers.removeFirst();

    // Cache: miss, bake through paint, hit, then stamp/zoom invalidation.
    LayerViewCache& cache = LayerViewCache::instance();
    const int k = plan.front();
    const QRectF box = crispLayerBox(*d, k).intersected(docRect);
    QRectF hitBox;
    CHECK(cache.find(*d, k, LayerViewCache::kCrisp, 1.0, &hitBox) == nullptr);
    QImage frame(d->size, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    {
        QPainter p(&frame);
        p.setRenderHint(QPainter::Antialiasing, true);
        cache.paintCachedCrisp(p, *d, k, box, 1.0);
    }
    const QImage* hit = cache.find(*d, k, LayerViewCache::kCrisp, 1.0, &hitBox);
    CHECK(hit != nullptr);
    if (hit) {
        CHECK(!hit->isNull());
        CHECK(hitBox == box);
    }
    // Second paint blits (no re-bake): same pixels out.
    QImage frame2(d->size, QImage::Format_ARGB32_Premultiplied);
    frame2.fill(Qt::transparent);
    {
        QPainter p(&frame2);
        cache.paintCachedCrisp(p, *d, k, box, 1.0);
    }
    CHECK(frame == frame2);
    // Past deadline: miss skips the draw (composite shows instead).
    {
        QPainter p(&frame2);
        CrispStats stats;
        cache.paintCachedCrisp(p, *d, k, box, 1.0, &stats,
                               std::chrono::steady_clock::now() -
                                   std::chrono::seconds(1));
        CHECK(stats.skipped == 0);  // hit: deadline only gates misses
        d->layers[k].sourceStamp++;
        cache.paintCachedCrisp(p, *d, k, box, 1.0, &stats,
                               std::chrono::steady_clock::now() -
                                   std::chrono::seconds(1));
        CHECK(stats.skipped == 1);
        d->layers[k].sourceStamp--;
    }
    // Content stamp bump invalidates.
    d->layers[k].sourceStamp++;
    CHECK(cache.find(*d, k, LayerViewCache::kCrisp, 1.0, &hitBox) == nullptr);
    d->layers[k].sourceStamp--;
    // Other zoom bucket misses.
    CHECK(cache.find(*d, k, LayerViewCache::kCrisp, 2.0, &hitBox) == nullptr);
    // Pixel layers (no art) are never cached: they blit already.
    d->layers[k].art.reset();
    CHECK(cache.find(*d, k, LayerViewCache::kCrisp, 1.0, &hitBox) == nullptr);
}

void test_outline_and_content() {
    AppState state;
    DocumentItem* d = openVectorDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const QRectF docRect(0, 0, d->size.width(), d->size.height());
    const QTransform t;  // identity: doc pixels are view pixels here

    // Outline paints hairlines with no composite at all.
    QImage frame(d->size, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    {
        QPainter p(&frame);
        CHECK(paintOutlineContent(p, *d, t, 1.0, docRect, QRect()));
    }
    int painted = 0;
    for (int y = 0; y < frame.height(); ++y) {
        const QRgb* row =
            reinterpret_cast<const QRgb*>(frame.constScanLine(y));
        for (int x = 0; x < frame.width(); ++x)
            if (qAlpha(row[x]) > 0) ++painted;
    }
    CHECK(painted > 0);  // rect contours drawn

    // Draft blits the composite (new documents already carry one); tiles
    // disabled here so only the blit runs. The tiled path refuses with
    // tiles disabled and falls back to classic.
    QImage blank(d->size, QImage::Format_ARGB32_Premultiplied);
    blank.fill(Qt::transparent);
    {
        QPainter p(&blank);
        DocumentItem* bare = state.addDocument(QStringLiteral("bare"),
                                               QSize(32, 32), 96);
        CHECK(bare != nullptr);
        if (bare) {
            CHECK(paintDraftContent(p, *d, t, 1.0, docRect, QRect(), {}));
            CHECK(!paintTiledContent(p, *bare, t, 1.0, docRect, QRect(), {}));
            // Deep zoom does not outrun the bake caps any more: past the
            // point where a fixed-size tile would outgrow the pixel cap the
            // grid shrinks the tile in document space, so Draft rides its
            // own tiles instead of refusing and falling back to the classic
            // walk (whose frame budget cannot cover a whole deep-zoom frame
            // in one pass). A small dirty rect keeps this to one tile.
            CHECK(paintDraftContent(p, *d, t, 100.0, docRect, QRect(0, 0, 16, 16),
                                    {}));
        }
    }
}

// ---- crispness of a settled tile frame -----------------------------------
//
// "Crisp should always happen": above zoom 1 the document composite is a
// resample of doc-resolution pixels, so a frame showing only the composite
// is soft. A settled tile frame must carry materially more edge detail than
// the composite alone. The failure mode this gates is a whole-document
// composite blit sitting above the vector art in the tile plan: the tile then
// reprints the soft composite and the gain collapses to zero.

// Mean |Laplacian| over RGB, sampled every second pixel: edges drawn at view
// density score high, a bilinear upscale of doc-resolution pixels scores low.
double edgeEnergy(const QImage& in) {
    const QImage img = in.convertToFormat(QImage::Format_RGB32);
    if (img.width() < 3 || img.height() < 3) return 0.0;
    double sum = 0.0;
    long n = 0;
    for (int y = 1; y < img.height() - 1; y += 2) {
        const QRgb* r = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        const QRgb* a = reinterpret_cast<const QRgb*>(img.constScanLine(y - 1));
        const QRgb* b = reinterpret_cast<const QRgb*>(img.constScanLine(y + 1));
        for (int x = 1; x < img.width() - 1; x += 2) {
            for (int c = 0; c < 3; ++c) {
                const int v = ((r[x - 1] >> (c * 8)) & 255) +
                              ((r[x + 1] >> (c * 8)) & 255) +
                              ((a[x] >> (c * 8)) & 255) +
                              ((b[x] >> (c * 8)) & 255) -
                              4 * ((r[x] >> (c * 8)) & 255);
                sum += v < 0 ? -v : v;
            }
            ++n;
        }
    }
    return n ? sum / (double)n : 0.0;
}

QSize frameSize(const DocumentItem& d, double zoom) {
    return QSize(qMax(1, qRound(d.size.width() * zoom)),
                 qMax(1, qRound(d.size.height() * zoom)));
}

// Device-space rect of a probe frame: a viewport-sized crop when one is
// asked for (deep zoom, where scaling the whole document would allocate a
// surface far larger than any screen and demand tiles the store budget
// could never hold), else the whole document at view density.
QRect frameRect(const DocumentItem& d, double zoom, const QRect& dirty) {
    if (!dirty.isEmpty()) return dirty;
    return QRect(QPoint(0, 0), frameSize(d, zoom));
}

// World transform for such a frame: document to view density, then shifted
// so the crop's top-left lands at the image origin. Qt composes transforms
// left to right in application order, so the scale comes first.
QTransform frameTransform(const QRect& r, double zoom) {
    return QTransform(zoom, 0, 0, zoom, 0, 0) *
           QTransform::fromTranslate(-r.x(), -r.y());
}

// What the screen shows with the tile path off: the composite resampled to
// view density.
QImage compositeFrame(const DocumentItem& d, double zoom,
                      const QRect& dirty = QRect()) {
    const QRect r = frameRect(d, zoom, dirty);
    QImage img(r.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    if (d.composite.isNull()) return img;
    const QRectF docRect(0, 0, d.size.width(), d.size.height());
    QPainter p(&img);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setTransform(frameTransform(r, zoom), false);
    p.drawImage(docRect, d.composite);
    return img;
}

// Paint frames until every visible tile has published, then return the last
// one: composite blit plus the baked tiles, exactly as paintDocument draws it.
QImage tiledFrame(DocumentItem& d, double zoom, bool* settled,
                  const QRect& dirty = QRect()) {
    if (settled) *settled = false;
    const QRectF docRect(0, 0, d.size.width(), d.size.height());
    const QRect r = frameRect(d, zoom, dirty);
    const QTransform t = frameTransform(r, zoom);
    const QRect view(0, 0, r.width(), r.height());
    CanvasTileStore::instance().setEnabled(true);
    QImage frame;
    bool all = false;
    QElapsedTimer budget;
    budget.start();
    while (budget.elapsed() < 10000) {
        QImage img(r.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setTransform(t, false);
            all = paintTiledContent(p, d, t, zoom, docRect, view, {});
        }
        frame = img;
        if (all) break;
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    if (settled) *settled = all;
    return frame;
}

// Edge energy of the settled tile frame relative to the composite alone.
double crispGain(DocumentItem& d, double zoom, const QRect& dirty = QRect()) {
    bool settled = false;
    const QImage tiled = tiledFrame(d, zoom, &settled, dirty);
    CHECK(settled);
    if (!settled) return 0.0;
    const double comp = edgeEnergy(compositeFrame(d, zoom, dirty));
    const double tile = edgeEnergy(tiled);
    std::printf("[display] crisp zoom=%.2f frame=%dx%d tiled=%.3f "
                "composite=%.3f gain=%+.1f%%\n",
                zoom, tiled.width(), tiled.height(), tile, comp,
                comp > 1e-9 ? 100.0 * (tile - comp) / comp : 0.0);
    return comp > 1e-9 ? tile / comp : 0.0;
}

DocumentItem* openSvgDoc(AppState& state, const QByteArray& xml,
                         const QString& name) {
    SvgImportResult result;
    int dpi = 96;
    QString error;
    if (!svgPartsImport(xml, &result, &dpi, &error)) return nullptr;
    QString openError;
    if (!state.openSvgParts(name, result, dpi, &openError)) return nullptr;
    return state.activeDocument();
}

// Crisp happens at any zoom, whatever is stacked above the vector art: an
// art-less layer (an imported <image>) goes to the composite tier, and if
// its blit is bounded by the whole document it buries every crisp layer
// below it - the tile then reprints the soft composite.
void test_tile_frame_crisp() {
    QString stripes;
    for (int x = 0; x < 64; x += 2)
        stripes += QStringLiteral("M%1 0h1v64h-1z").arg(x);
    QByteArray png;
    {
        QImage px(1, 1, QImage::Format_ARGB32);
        px.setPixel(0, 0, qRgba(255, 0, 0, 255));
        QBuffer buf(&png);
        CHECK(px.save(&buf, "PNG"));
    }
    const QByteArray xml =
        QStringLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
            "width=\"64\" height=\"64\">"
            "<rect width=\"64\" height=\"64\" fill=\"#ffffff\"/>"
            "<path fill=\"#000000\" d=\"%1\"/>"
            "<image x=\"54\" y=\"54\" width=\"8\" height=\"8\" "
            "xlink:href=\"data:image/png;base64,%2\"/>"
            "</svg>")
            .arg(stripes, QString::fromLatin1(png.toBase64()))
            .toUtf8();

    AppState state;
    DocumentItem* d = openSvgDoc(state, xml, QStringLiteral("stripes.svg"));
    CHECK(d != nullptr);
    if (!d) return;
    // The image is the last child, so it sits on top of the stack.
    CHECK(!d->layers.isEmpty());
    if (!d->layers.isEmpty()) CHECK(!d->layers.first().art);
    const double gain = crispGain(*d, 4.0);
    CHECK(gain > 1.15);
    // Deep zoom too: past the old 8x bake cap the store refused tiles
    // entirely, so the frame fell back to the classic walk and its frame
    // budget left part of the view at composite resolution.
    const double deep = crispGain(*d, 12.0);
    CHECK(deep > 1.15);
    // A wheel step that is not a clean quarter must be as sharp as its own
    // bucket: 3.70 and 3.75 share bucket 3.75, so the plan, the grid and the
    // raster density are all identical and only the view differs, by 1.3%.
    // Resampling the tile through a fractional destination at that ratio
    // costs about a quarter of the region's edge energy, which is why the
    // zoom a user actually stops on (8.66, 3.70, 5.83 - never a quarter)
    // read as softer than the quarter-boundary zooms the tests used.
    const double onBucket = crispGain(*d, 3.75);
    const double offBucket = crispGain(*d, 3.70);
    CHECK(offBucket > onBucket * 0.92);
}

// A translucent image over vector art keeps the art sharp: the image goes
// to the simple-blit tier (its own bitmap resampled to view density over
// crisp-baked art) instead of the composite tier, whose snapshot blit
// would bury the crisp draws below it and reprint the soft composite.
// Without the tier the overlap reads barely sharper than the composite.
void test_tile_frame_simple_blit() {
    QString stripes;
    for (int x = 0; x < 64; x += 2)
        stripes += QStringLiteral("M%1 0h1v64h-1z").arg(x);
    QImage img(16, 16, QImage::Format_ARGB32);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            img.setPixel(x, y, qRgba(255, 0, 0, (x * 255) / 15));
    QByteArray png;
    {
        QBuffer buf(&png);
        CHECK(img.save(&buf, "PNG"));
    }
    const QByteArray xml =
        QStringLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
            "width=\"64\" height=\"64\">"
            "<rect width=\"64\" height=\"64\" fill=\"#ffffff\"/>"
            "<path fill=\"#000000\" d=\"%1\"/>"
            "<image x=\"24\" y=\"24\" width=\"16\" height=\"16\" "
            "xlink:href=\"data:image/png;base64,%2\"/>"
            "</svg>")
            .arg(stripes, QString::fromLatin1(png.toBase64()))
            .toUtf8();

    AppState state;
    DocumentItem* d = openSvgDoc(state, xml, QStringLiteral("softblit.svg"));
    CHECK(d != nullptr);
    if (!d) return;
    // Tier census: the image is a plain blit, not a composite snapshot.
    const QRectF vis(0, 0, d->size.width(), d->size.height());
    long crisp = 0, comp = 0, simple = 0;
    CanvasTileStore::describeTilePlan(*d, vis, 4.0, &crisp, &comp, &simple);
    CHECK(crisp > 0);
    CHECK(comp == 0);
    CHECK(simple == 1);
    // Sharpness over the overlap: crisp stripes under a translucent blit.
    const double z = 4.0;
    const QRect crop(qRound(24 * z), qRound(24 * z), qRound(16 * z),
                     qRound(16 * z));
    const double gain = crispGain(*d, z, crop);
    CHECK(gain > 2.0);
    // Exactness: the settled tile matches a direct paint of the same art
    // (sharp stripes plus the resampled bitmap) nearly exactly.
    bool settled = false;
    const QImage tiled = tiledFrame(*d, z, &settled, crop);
    CHECK(settled);
    if (!settled) return;
    QImage ref(crop.size(), QImage::Format_ARGB32_Premultiplied);
    ref.fill(Qt::transparent);
    {
        QPainter p(&ref);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setTransform(frameTransform(crop, z), false);
        p.fillRect(QRectF(0, 0, 64, 64), Qt::white);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        for (int x = 0; x < 64; x += 2) p.drawRect(QRectF(x, 0, 1, 64));
        p.drawImage(QRectF(24, 24, 16, 16), img);
    }
    int worst = 0;
    for (int y = 0; y < crop.height(); ++y) {
        const QRgb* a =
            reinterpret_cast<const QRgb*>(tiled.constScanLine(y));
        const QRgb* b = reinterpret_cast<const QRgb*>(ref.constScanLine(y));
        for (int x = 0; x < crop.width(); ++x)
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst,
                                 std::abs(int((a[x] >> (c * 8)) & 255) -
                                          int((b[x] >> (c * 8)) & 255)));
    }
    std::printf("[display] simple-blit maxdiff=%d\n", worst);
    CHECK(worst <= 4);
    // TEMP-DIAG: save both sides (remove with the check below).
    if (std::getenv("PITTORE_DUMP")) {
        tiled.save(QStringLiteral("/tmp/simple_tiled.png"));
        ref.save(QStringLiteral("/tmp/simple_ref.png"));
    }
}

// A moved layer must render identically through the tile path and through the
// composite the Move tool left behind. This is the one configuration the
// static checks above cannot see, because the tile path only ever draws for a
// document revision that has stopped moving:
//   * every drag step bumps DocumentItem::revision, so paintReady's
//     `tile.revision == doc.revision` can never match while the drag runs;
//     the store queues a bake, reports the tile missing, and the caller's
//     composite blit is what is on screen (which is why dragging looks right);
//   * on release the revision freezes, the queued bakes publish and draw over
//     that composite;
//   * the next revision bump - a colour-grade slider, which calls
//     rebuildComposite - invalidates them again and the composite shows
//     through once more.
// A tile that places the moved layer anywhere but where the composite does
// therefore reads exactly as "it is here while I drag, then jumps when I let
// go, and the grade puts it back".
// The tile path must place a moved layer where the composite does. This is
// checked at several document states, because which tier a layer lands in
// depends on the rest of the stack: a plain bitmap takes the simple-blit
// tier, an adjustment above it takes the composite tier.
void checkTilePlacement(DocumentItem* d, int idx, double z,
                        const char* label) {
    bool settled = false;
    const QImage tiled = tiledFrame(*d, z, &settled);
    CHECK(settled);
    if (!settled) return;
    const QImage comp = compositeFrame(*d, z);
    CHECK(tiled.size() == comp.size());
    if (tiled.size() != comp.size()) return;

    int worst = 0;
    qint64 structural = 0;
    // A displacement shows up deep inside the layer, so count separately the
    // disagreements that are NOT hugging one of its fractional edges. The
    // composite covers a layer's edge pixel fractionally (offset 212.16 means
    // pixel 212 is 84% layer); the tile's simple blit snaps that edge to the
    // device grid instead. That is a sub-pixel blending difference confined
    // to the edge columns, not a placement difference.
    qint64 awayFromEdge = 0;
    // Edge bands in FRAME pixels: the composite covers a layer's edge pixel
    // fractionally while the tile's blit snaps it, so disagreements hugging
    // the footprint's frame-space edges are sub-pixel blending, not
    // placement. Anything deeper inside is a real displacement.
    const LayerItem& mover = d->layers[idx];
    const double footW = mover.pixels ? mover.pixels->width() * mover.scaleX
                                      : 0.0;
    const double footH = mover.pixels ? mover.pixels->height() * mover.scaleY
                                      : 0.0;
    const double edgeL = mover.offset.x() * z;
    const double edgeT = mover.offset.y() * z;
    const double edgeR = (mover.offset.x() + footW) * z;
    const double edgeB = (mover.offset.y() + footH) * z;
    const bool fracL = edgeL - std::floor(edgeL) > 0.0;
    const bool fracT = edgeT - std::floor(edgeT) > 0.0;
    const bool fracR = edgeR - std::floor(edgeR) > 0.0;
    const bool fracB = edgeB - std::floor(edgeB) > 0.0;
    for (int y = 0; y < comp.height(); ++y) {
        const QRgb* a = reinterpret_cast<const QRgb*>(tiled.constScanLine(y));
        const QRgb* b = reinterpret_cast<const QRgb*>(comp.constScanLine(y));
        for (int x = 0; x < comp.width(); ++x) {
            bool pixelDiffers = false;
            for (int c = 0; c < 4; ++c) {
                const int delta = std::abs(int((a[x] >> (c * 8)) & 255) -
                                            int((b[x] >> (c * 8)) & 255));
                worst = std::max(worst, delta);
                if (delta > 32) pixelDiffers = true;
            }
            if (!pixelDiffers) continue;
            ++structural;
            const bool onLeftEdge = fracL && x <= int(edgeL) + 2;
            const bool onTopEdge = fracT && y <= int(edgeT) + 2;
            const bool onRightEdge =
                fracR && x >= int(edgeR) - 2 && x < comp.width();
            const bool onBottomEdge =
                fracB && y >= int(edgeB) - 2 && y < comp.height();
            if (!onLeftEdge && !onTopEdge && !onRightEdge && !onBottomEdge)
                ++awayFromEdge;
        }
    }
    std::printf("[display] %s z=%.4f tiled-vs-composite maxdiff=%d "
                "channels-over-32=%lld away-from-edge=%lld\n",
                label, z, worst, static_cast<long long>(structural),
                static_cast<long long>(awayFromEdge));
    if (const char* dump = std::getenv("PITTORE_DUMP")) {
        QString safeLabel = QString::fromLatin1(label);
        safeLabel.replace(QLatin1Char('/'), QLatin1Char('_'));
        QString base = QString::fromLocal8Bit(dump) + safeLabel +
                       QStringLiteral("_");
        const bool okT =
            tiled.save(base + QStringLiteral("tiled.png"));
        const bool okC =
            comp.save(base + QStringLiteral("comp.png"));
        if (!okT || !okC)
            std::printf("[display] dump %s -> tiled=%d comp=%d\n",
                        base.toLocal8Bit().constData(), int(okT), int(okC));
    }
    // Position must agree. Sub-pixel edge blending may not, and is reported
    // above rather than asserted away.
    CHECK(awayFromEdge == 0);
}

void test_tile_frame_after_move() {
    AppState state;
    DocumentItem* d =
        state.addDocument(QStringLiteral("move-tiles"), QSize(1280, 720), 96);
    CHECK(d != nullptr);
    if (!d) return;

    // The reported document: a background filling the frame and a layer one or
    // two pixels smaller than it, both plain bitmaps so the mover qualifies
    // for the tile store's simple-blit tier (no style, mask, clip or art).
    // The mover is resampled like the session's (a ~1595x898 source at 0.8
    // lands at the logged 1276.16x718.56 footprint): the tile blit then
    // resamples through QPainter while the composite resamples through the
    // engine, so this is where the two paths can separate across the whole
    // layer rather than only along its fractional edges.
    QImage bg(1280, 720, QImage::Format_ARGB32_Premultiplied);
    bg.fill(qRgba(24, 24, 32, 255));
    QImage mover(1595, 898, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < mover.height(); ++y)
        for (int x = 0; x < mover.width(); ++x)
            mover.setPixel(x, y, qRgba((x * 255) / (mover.width() - 1),
                                       (y * 255) / (mover.height() - 1),
                                       ((x + y) * 255) /
                                           (mover.width() + mover.height() - 2),
                                       255));
    state.addPixelLayerFromImage(bg, QStringLiteral("bg"), QPointF(0, 0), 1.0,
                                 1.0, QStringLiteral("bg"),
                                 QStringLiteral("layer"));
    state.addPixelLayerFromImage(mover, QStringLiteral("mover"),
                                 QPointF(2, 1), 0.8, 0.8,
                                 QStringLiteral("mover"),
                                 QStringLiteral("layer"));
    int idx = -1;
    for (int i = 0; i < d->layers.size(); ++i)
        if (d->layers[i].name == QStringLiteral("mover")) idx = i;
    CHECK(idx >= 0);
    if (idx < 0) return;

    // The offset the reported drag ended on: fractional, and far enough right
    // that the mover's left edge uncovers the background.
    const QPointF from = d->layers[idx].offset;
    const QPointF to(212.16, 0.72);
    const QVector<int> idxs{idx};
    const QVector<QPointF> starts{from};
    CHECK(state.moveLayersAt(idxs, to - from, starts));

    // The mover's panel index shifts whenever a layer is inserted above it,
    // so re-resolve by name at every stage rather than trusting a snapshot.
    const auto findMover = [d] {
        for (int i = 0; i < d->layers.size(); ++i)
            if (d->layers[i].name == QStringLiteral("mover")) return i;
        return -1;
    };

    // Before any grade: the state the reported drag leaves behind.
    checkTilePlacement(d, idx, 1.0, "move/plain-z1");
    checkTilePlacement(d, idx, 1.5625, "move/plain-z1.5625");

    // The reported sequence continues with a colour grade. An adjustment
    // layer covers the whole document and takes the tile store's COMPOSITE
    // tier while the mover stays on the SIMPLE tier, so one tile now mixes a
    // self-placed bitmap with a snapshot blit of the composite above it. The
    // two-layer case above never exercised that mix.
    using pittore::compute::AdjustmentKind;
    CHECK(state.addAdjustmentLayer(
        static_cast<int>(AdjustmentKind::BrightnessContrast)));
    state.setAdjustmentParamAt(d->activeLayer, 0, 0.15f);
    state.setAdjustmentParamAt(d->activeLayer, 1, 0.20f);
    idx = findMover();
    CHECK(idx >= 0);
    if (idx < 0) return;
    checkTilePlacement(d, idx, 1.0, "grade/plain-z1");
    checkTilePlacement(d, idx, 1.5625, "grade/plain-z1.5625");

    // Move again under the grade: the gesture the report describes, where the
    // drag reads right and the jump appears once the drag is over.
    const QPointF from2 = d->layers[idx].offset;
    const QPointF to2(-96.35, 41.8);
    const QVector<int> idxs2{idx};
    const QVector<QPointF> starts2{from2};
    CHECK(state.moveLayersAt(idxs2, to2 - from2, starts2));
    checkTilePlacement(d, idx, 1.0, "grade/moved-z1");
    checkTilePlacement(d, idx, 1.5625, "grade/moved-z1.5625");
}

// Same check on the shipped fixtures: the Bay Area map carries embedded
// <image> layers above 80k vector paths, which is where the soft-tile report
// came from. Skips cleanly when the fixtures are absent (never commit them).
void test_tile_frame_crisp_fixture() {
    const char* env = std::getenv("PITTORE_SVG_DIR");
    const QString dir =
        env ? QString::fromLocal8Bit(env) : QString();
    for (const char* name : {"Location_map_San_Francisco_Bay_Area.svg",
                             "Mapa_do_Brasil_por_código_DDD.svg"}) {
        if (dir.isEmpty()) {
            std::printf("[display] no PITTORE_SVG_DIR - %s skipped\n", name);
            continue;
        }
        // UTF-8, not Latin-1: the fixture name's high bytes are a filename
        // on disk, and Latin-1 would re-encode them into a different path.
        QFile f(dir + QLatin1Char('/') + QString::fromUtf8(name));
        if (!f.exists()) {
            std::printf("[display] fixture absent - %s skipped\n",
                        qPrintable(f.fileName()));
            continue;
        }
        if (!f.open(QIODevice::ReadOnly)) continue;
        AppState state;
        DocumentItem* d = openSvgDoc(state, f.readAll(),
                                     QString::fromUtf8(name));
        CHECK(d != nullptr);
        if (!d) continue;
        const double gain = crispGain(*d, 2.0);
        CHECK(gain > 1.35);
        // Deep zoom, the case that read as "858% is low res": one
        // viewport-sized crop through the middle of the map at 12x. Past the
        // old bake cap the store refused tiles entirely, so this frame was
        // nothing but the composite upscaled twelve times.
        const double z = 12.0;
        const QRect crop(qRound(d->size.width() * z / 2.0) - 400,
                         qRound(d->size.height() * z / 2.0) - 225, 800, 450);
        const double deep = crispGain(*d, z, crop);
        CHECK(deep > 1.35);
        // The zoom the report came from: 866% is not a quarter of anything,
        // and it shares bucket 8.75 with 8.75 itself. Same plan, same grid,
        // same raster density - so this frame must land as sharp as the
        // quarter-boundary frame instead of losing a quarter of its edge
        // energy to a resampled tile draw.
        const double offZ = 8.66;
        const QRect offCrop(qRound(d->size.width() * offZ / 2.0) - 400,
                            qRound(d->size.height() * offZ / 2.0) - 225, 800,
                            450);
        const double off = crispGain(*d, offZ, offCrop);
        const double onZ = 8.75;
        const QRect onCrop(qRound(d->size.width() * onZ / 2.0) - 400,
                           qRound(d->size.height() * onZ / 2.0) - 225, 800,
                           450);
        const double on = crispGain(*d, onZ, onCrop);
        CHECK(off > on * 0.92);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString logDir = QDir::tempPath() + QStringLiteral("/pittore-display-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    CanvasTileStore::instance().setEnabled(false);  // sync parts only here
    test_zoom_bucket();
    test_zoom_range_and_caps();
    test_tile_plan_zoom_independent();
    test_governor();
    test_plan_and_cache();
    test_outline_and_content();
    // Crisp-gain checks last: they leave the tile store enabled.
    test_tile_frame_crisp();
    test_tile_frame_simple_blit();
    test_tile_frame_after_move();
    test_tile_frame_crisp_fixture();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
