// test_incremental_parity.cpp — the Move tool's incremental renderer and a
// full rebuild must produce the same composite.
//
// renderRegion() and rebuildComposite() share gatherCompositedLayers() and
// the same compositing kernel; they differ only in the rectangle they clear,
// gather and blit. The Move tool always takes the incremental path, and the
// reported defect is that the canvas shows a dragged layer in one place while
// the model has it in another — until any full rebuild (moving a single
// colour-grade slider calls rebuildComposite) snaps it back to the truth.
//
// Every model-side invariant passes on that path (the mover lands on the
// snapped offset, the revision bumps, the region is non-empty), so the fault
// is only observable by rendering one model state both ways and comparing the
// pixels. That is what this test does.
#include <QCoreApplication>
#include <QImage>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <string>

#include "engine/compute/adjust.h"
#include "test_util.h"
#include "ui/app_state.h"

using namespace pittore::ui;

namespace {

QImage solid(int w, int h, QRgb c) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

// A gradient so a displaced copy differs from the reference wherever it
// lands, not only along one edge.
QImage patterned(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y,
                         qRgba((x * 255) / std::max(1, w - 1),
                               (y * 255) / std::max(1, h - 1),
                               ((x + y) * 255) / std::max(1, w + h - 2), 255));
    return img;
}

struct Diff {
    qint64 bytes = 0;
    int maxDelta = 0;
    int firstX = -1;
    int firstY = -1;
};

// Byte-wise compare of two same-size composites. Reports how many bytes
// differ, the largest channel excursion and where the first one is, so a
// failure says how far off and where — not merely "not equal".
Diff compare(const QImage& a, const QImage& b) {
    Diff r;
    if (a.size() != b.size() || a.format() != b.format()) {
        r.bytes = -1;
        return r;
    }
    const uchar* pa = a.constBits();
    const uchar* pb = b.constBits();
    const qint64 n = a.sizeInBytes();
    const int bpl = a.bytesPerLine();
    for (qint64 i = 0; i < n; ++i) {
        if (pa[i] == pb[i]) continue;
        if (++r.bytes == 1) {
            r.firstX = int((i % bpl) / 4);
            r.firstY = int(i / bpl);
        }
        const int d = int(pa[i]) - int(pb[i]);
        r.maxDelta = std::max(r.maxDelta, d > 0 ? d : -d);
    }
    return r;
}

int findLayer(DocumentItem* d, const char* name) {
    const QString want = QString::fromUtf8(name);
    for (int i = 0; i < d->layers.size(); ++i)
        if (d->layers[i].name == want) return i;
    return -1;
}

// Move layer `idx` to `to` through the Move tool's own entry point (the
// absolute-placement form canvas_mouse_move.cpp calls, so the whole
// moveLayersAt -> renderRegion path is exercised), snapshot that result,
// rebuild, and require the two to agree.
void expectParity(AppState& state, DocumentItem* d, int idx, const QPointF& to,
                  const char* label) {
    const QPointF from = d->layers[idx].offset;
    if (from == to) return;

    const QImage before = d->composite.copy();
    const QVector<int> idxs{idx};
    const QVector<QPointF> starts{from};
    state.moveLayersAt(idxs, to - from, starts);   // -> renderRegion()
    const QImage incr = d->composite.copy();

    // A parity result is only meaningful if the mover is actually on screen:
    // an off-frame move renders identically either way. Report it as a real
    // failure rather than silently passing a vacuous comparison.
    const Diff moved = compare(before, incr);
    ++pittore_test::checks();
    if (moved.bytes == 0) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "%s: move to (%.1f,%.1f) changed no pixel - the mover "
                      "is off-frame or covered, so parity would be vacuous",
                      label, to.x(), to.y());
        pittore_test::report_failure(__FILE__, __LINE__, buf);
    }

    d->rebuildComposite();
    const QImage full = d->composite.copy();

    const Diff diff = compare(incr, full);
    ++pittore_test::checks();
    if (diff.bytes != 0) {
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "%s: incremental renderRegion != full rebuild: %lld "
                      "differing bytes, maxDelta=%d, first at (%d,%d); mover "
                      "-> (%.1f,%.1f)",
                      label, static_cast<long long>(diff.bytes), diff.maxDelta,
                      diff.firstX, diff.firstY, to.x(), to.y());
        pittore_test::report_failure(__FILE__, __LINE__, buf);
    }
}

}  // namespace

static void test_incremental_parity() {
    AppState state;
    DocumentItem* d =
        state.addDocument(QStringLiteral("parity"), QSize(1280, 720), 96);
    CHECK(d != nullptr);
    if (!d) return;

    // The reported document: a background filling the frame, then a layer one
    // or two pixels smaller than it (1276x719 in 1280x720), dragged to
    // offsets that push it off the canvas. addLayer() inserts at the active
    // row, so the mover added last sits above the background.
    state.addPixelLayerFromImage(solid(1280, 720, qRgba(24, 24, 32, 255)),
                                 QStringLiteral("bg"), QPointF(0, 0), 1.0, 1.0,
                                 QStringLiteral("bg"), QStringLiteral("layer"));
    state.addPixelLayerFromImage(patterned(1276, 719), QStringLiteral("mover"),
                                 QPointF(2, 1), 1.0, 1.0,
                                 QStringLiteral("mover"), QStringLiteral("layer"));
    const int mover = findLayer(d, "mover");
    CHECK(mover >= 0);
    if (mover < 0) return;

    // Offsets taken from the recorded drag session, where every one of them
    // left an edge outside the document.
    expectParity(state, d, mover, QPointF(-374.9, -14.6),
                 "plain/off-canvas-left");
    expectParity(state, d, mover, QPointF(528.5, 0.0), "plain/off-canvas-right");
    expectParity(state, d, mover, QPointF(249.6, -186.6),
                 "plain/off-canvas-top");
    expectParity(state, d, mover, QPointF(81.0, -14.6), "plain/near-frame");

    // A mask on the mover: the region path gathers with layerBounds() while
    // the staging footprint also accounts for an unlinked mask (stageBounds),
    // so this is the configuration where the two rects can disagree.
    if (state.addLayerMask(1.0f)) {
        const int m = findLayer(d, "mover");
        CHECK(m >= 0);
        if (m >= 0) {
            expectParity(state, d, m, QPointF(-120.0, 40.0), "mask/moved");
            expectParity(state, d, m, QPointF(300.0, -60.0),
                         "mask/moved-again");
        }
    }

    // A layer clipped to the mover: clipBase indexes into the gathered list,
    // and the region gather contains a subset of the stack the full rebuild
    // sees.
    state.addPixelLayerFromImage(solid(512, 384, qRgba(220, 40, 40, 160)),
                                 QStringLiteral("clipper"), QPointF(60, 40),
                                 1.0, 1.0, QStringLiteral("clipper"),
                                 QStringLiteral("layer"));
    const int clipper = findLayer(d, "clipper");
    CHECK(clipper >= 0);
    if (clipper >= 0) {
        d->layers[clipper].clipped = true;
        d->rebuildComposite();
        const int m = findLayer(d, "mover");
        CHECK(m >= 0);
        if (m >= 0) {
            expectParity(state, d, m, QPointF(-200.0, -30.0), "clip/moved");
            expectParity(state, d, m, QPointF(140.0, 90.0), "clip/moved-again");
        }
    }

    // An adjustment layer on top, then a parameter change: that change is
    // what rebuildComposite() does in the app (a colour-grade slider — the
    // dialog drives BrightnessContrast/Levels/Exposure kinds), and it is
    // reported as snapping the canvas back to where the model already had
    // the layer. Parity must hold before it, or the slider is masking a
    // region-render defect rather than fixing anything.
    using pittore::compute::AdjustmentKind;
    const int moverNow = findLayer(d, "mover");
    CHECK(moverNow >= 0);
    if (moverNow >= 0)
        expectParity(state, d, moverNow, QPointF(-466.1, -103.3),
                     "adjust/before-slider");
    CHECK(state.addAdjustmentLayer(
        static_cast<int>(AdjustmentKind::BrightnessContrast)));
    CHECK(state.activeLayer() != nullptr &&
          state.activeLayer()->kind == LayerItem::Kind::Adjustment);
    state.setAdjustmentParamAt(d->activeLayer, 0, 0.15f);  // the "slider"
    state.setAdjustmentParamAt(d->activeLayer, 1, 0.20f);
    {
        const int m = findLayer(d, "mover");
        CHECK(m >= 0);
        if (m >= 0) {
            expectParity(state, d, m, QPointF(256.0, -71.7),
                         "adjust/after-slider");
            expectParity(state, d, m, QPointF(-30.0, 12.0),
                         "adjust/after-slider-2");
            expectParity(state, d, m, QPointF(528.5, 0.0),
                         "adjust/off-canvas-right");
        }
    }

    // A resampled mover. Every case above is scale 1.0, but the reported drag
    // moved a layer whose document footprint is 1276.16x718.56 in a 1280x720
    // document — its source pixels are sampled into document space, not copied
    // texel for texel. layerBounds() sizes that footprint from the scaled
    // image while stageBounds() grows it by a scale-derived halo, so this is
    // where the gathered region and the staged footprint separate most, and a
    // one-texel disagreement here is a visible offset on screen.
    state.addPixelLayerFromImage(patterned(940, 528), QStringLiteral("scaled"),
                                 QPointF(37.4, -22.1), 1.3576, 1.3609,
                                 QStringLiteral("scaled"),
                                 QStringLiteral("layer"));
    const int scaled = findLayer(d, "scaled");
    CHECK(scaled >= 0);
    if (scaled >= 0) {
        // The logged offset of the reported drag, reproduced exactly.
        expectParity(state, d, scaled, QPointF(212.16, 0.72),
                     "scaled/logged-fractional");
        expectParity(state, d, scaled, QPointF(-410.35, 88.9),
                     "scaled/off-canvas-left");
        expectParity(state, d, scaled, QPointF(121.7, -96.4),
                     "scaled/off-canvas-top");
        expectParity(state, d, scaled, QPointF(-374.9, -14.6),
                     "scaled/off-canvas-far");
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_incremental_parity();
    std::printf("checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return pittore_test::failures() == 0 ? 0 : 1;
}
