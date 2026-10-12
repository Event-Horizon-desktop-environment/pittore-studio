// Mask selections: selection channel, per-pixel coverage, outline.
// No AI, headless AppState test, CPU backend.
#include <QCoreApplication>
#include <QDir>
#include <QImage>

#include <cstdint>
#include <vector>

#include "engine/compute/paint.h"
#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/selection_mask.h"

using namespace pittore::ui;

namespace {

// Grayscale8 mask, 255 inside rects. Writes via scanLine so strides match.
QImage rectMask(int w, int h, const std::vector<QRect>& rects) {
    QImage m(w, h, QImage::Format_Grayscale8);
    m.fill(0);
    for (const QRect& r : rects)
        for (int y = r.top(); y <= r.bottom(); ++y) {
            uchar* row = m.scanLine(y);
            for (int x = r.left(); x <= r.right(); ++x) row[x] = 255;
        }
    return m;
}

void test_engine_coverage() {
    // Rect fast path unchanged.
    {
        pittore::compute::SelectionMask m;
        m.x0 = 1.0f; m.y0 = 1.0f; m.x1 = 3.0f; m.y1 = 3.0f; m.ellipse = false;
        CHECK_EQ(m.coverage(2, 2), 1.0f);
        CHECK_EQ(m.coverage(0, 2), 0.0f);
        CHECK_EQ(m.coverage(2, 5), 0.0f);
    }
    // Uniform coverage: 0, half, full.
    {
        pittore::compute::SelectionMask m;
        m.x0 = 0.0f; m.y0 = 0.0f; m.x1 = 4.0f; m.y1 = 4.0f;
        m.pixels = std::vector<std::uint8_t>(16, 255);
        m.mw = 4; m.mh = 4;
        CHECK_NEAR(m.coverage(2, 2), 1.0f, 1e-3f);
        m.pixels.assign(16, 128);
        CHECK_NEAR(m.coverage(2, 2), 128.0f / 255.0f, 1e-3f);
        m.pixels.assign(16, 0);
        CHECK_NEAR(m.coverage(2, 2), 0.0f, 1e-3f);
    }
    // Soft edge: left half set, right half empty.
    {
        // 4x1 over [0,4]x[0,1], bilinear edge between halves.
        pittore::compute::SelectionMask m;
        m.x0 = 0.0f; m.y0 = 0.0f; m.x1 = 4.0f; m.y1 = 1.0f;
        m.pixels = {255, 255, 0, 0};
        m.mw = 4; m.mh = 1;
        CHECK_NEAR(m.coverage(0, 0), 1.0f, 1e-3f);
        const float mid = m.coverage(2, 0);
        CHECK(mid > 0.0f && mid < 0.5f);
        CHECK_NEAR(m.coverage(3, 0), 0.0f, 1e-3f);
    }
}

void test_outline() {
    using pittore::ui::selectionOutlineFromMask;
    using pittore::ui::selectionMaskBbox;

    // L-shape: outline must exclude the notch, unlike a plain rect.
    const QImage l = rectMask(10, 10, {QRect(QPoint(0, 0), QPoint(4, 9)),
                                       QRect(QPoint(0, 0), QPoint(9, 4))});
    CHECK(selectionMaskBbox(l) == QRect(QPoint(0, 0), QPoint(9, 9)));
    QPainterPath outline = selectionOutlineFromMask(l, QRect(0, 0, 10, 10));
    CHECK(!outline.isEmpty());
    CHECK(outline.contains(QPointF(2.5, 2.5)));   // inside the L
    CHECK(!outline.contains(QPointF(7.5, 7.5)));  // inside the notch — the
                                                  // discriminating property
    // Points clearly inside each arm (not on the contour boundary).
    CHECK(outline.contains(QPointF(2.0, 7.0)));   // inside the vertical arm
    CHECK(outline.contains(QPointF(7.0, 2.0)));   // inside the horizontal arm
    const QRectF b = outline.boundingRect();
    CHECK(b.left() >= -0.5 && b.top() >= -0.5 && b.right() <= 10.5 &&
          b.bottom() <= 10.5);

    // Full rect: outline is the boundary, encloses center.
    const QImage full = rectMask(10, 10, {QRect(QPoint(0, 0), QPoint(9, 9))});
    QPainterPath rectOutline = selectionOutlineFromMask(full, QRect(0, 0, 10, 10));
    CHECK(rectOutline.contains(QPointF(5.0, 5.0)));
    CHECK(rectOutline.contains(QPointF(0.5, 0.5)));

    // Donut: hole excluded (two loops).
    const QImage donutBase = rectMask(12, 12, {QRect(QPoint(0, 0), QPoint(11, 11))});
    QImage donut = donutBase;
    uchar* dbits = donut.bits();
    for (int y = 4; y <= 7; ++y)
        for (int x = 4; x <= 7; ++x)
            dbits[std::size_t(y) * 12 + x] = 0;
    QPainterPath ringOutline = selectionOutlineFromMask(donut, QRect(0, 0, 12, 12));
    CHECK(ringOutline.contains(QPointF(2.0, 2.0)));    // in the donut
    CHECK(!ringOutline.contains(QPointF(6.0, 6.0)));   // in the hole
    CHECK(ringOutline.contains(QPointF(10.0, 10.0)));  // in the donut
    CHECK(ringOutline.elementCount() > 20);            // outer + inner loops

    // Empty mask: no bbox, empty outline.
    const QImage empty = rectMask(8, 8, {});
    CHECK(selectionMaskBbox(empty).isEmpty());
    CHECK(selectionOutlineFromMask(empty, QRect(0, 0, 8, 8)).isEmpty());
    // Soft mask (half-selected everywhere, above threshold) is fully outlined.
    QImage soft(8, 8, QImage::Format_Grayscale8);
    soft.fill(150);
    const QPainterPath softOutline = selectionOutlineFromMask(soft, QRect(0, 0, 8, 8));
    CHECK(!softOutline.isEmpty());
    CHECK(softOutline.contains(QPointF(4.0, 4.0)));

    // Largest component keeps the big blob, drops shadow + specks.
    QImage stray(24, 24, QImage::Format_Grayscale8);
    stray.fill(0);
    {
        uchar* r;
        for (int y = 4; y <= 15; ++y) {          // the object: 12x12 block
            r = stray.scanLine(y);
            for (int x = 4; x <= 15; ++x) r[x] = 255;
        }
        for (int y = 19; y <= 22; ++y) {         // detached shadow below
            r = stray.scanLine(y);
            for (int x = 7; x <= 10; ++x) r[x] = 255;
        }
        stray.scanLine(2)[21] = 255;             // lone speck
        stray.scanLine(17)[20] = 255;            // lone speck
    }
    CHECK(selectionMaskBbox(stray) == QRect(QPoint(4, 2), QPoint(21, 22)));
    const QImage kept = selectionMaskLargestComponent(stray);
    CHECK(kept.width() == 24 && kept.height() == 24);
    const QRect keptBbox = selectionMaskBbox(kept);
    CHECK(keptBbox == QRect(QPoint(4, 4), QPoint(15, 15)));  // only the 12x12
    // Everything inside the kept block is still selected; strays are gone.
    CHECK(kept.constScanLine(8)[8] == 255);
    CHECK(kept.constScanLine(20)[8] == 0);
    CHECK(kept.constScanLine(2)[21] == 0);

    // Layer alpha -> doc mirrors canvas: scale 2 maps 1px to 2x2.
    {
        std::vector<float> alpha(5 * 5, 0.0f);
        alpha[1 * 5 + 1] = 1.0f;  // layer pixel (1,1) → solid
        alpha[3 * 5 + 4] = 0.5f;  // layer pixel (4,3) → soft (128)
        const QImage mapped = selectionMaskFromLayerAlpha(
            alpha.data(), 5, 5, QPointF(0, 0), 2.0, 2.0, QSize(10, 10));
        CHECK(mapped.width() == 10 && mapped.height() == 10);
        // Bilinear (not nearest): doc pixels straddling the solid layer
        // pixel share its coverage; well outside stays empty, and the soft
        // value attenuates by covered area instead of surviving whole.
        CHECK(mapped.constScanLine(2)[2] == 143);
        CHECK(mapped.constScanLine(3)[3] == 143);
        CHECK(mapped.constScanLine(4)[4] == 16);    // 6% bleed, not a step
        CHECK(mapped.constScanLine(7)[9] == 96);
    }
}

void test_app_state_selection() {
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("sel"), QSize(40, 30), 300);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->selection.isEmpty() && !d->selectionIsMask);

    // Rect marquee: classic selection, no mask stored.
    state.setSelection(QRectF(2, 3, 10, 8), false);
    CHECK(!d->selectionIsMask && d->selectionMask.isNull());
    const quint64 stamp0 = d->selectionStamp;

    // Mask: bbox at 50% threshold, mask stored.
    const QImage m = rectMask(40, 30, {QRect(QPoint(5, 6), QPoint(19, 23))});
    state.setSelectionMask(m);
    CHECK(d->selectionIsMask);
    CHECK(d->selection == QRectF(5.0, 6.0, 15.0, 18.0));
    CHECK(d->selectionStamp != stamp0);

    // Marquee and mask replace each other.
    state.setSelection(QRectF(0, 0, 5, 5), false);
    CHECK(!d->selectionIsMask);
    state.setSelectionMask(m);
    CHECK(d->selectionIsMask);

    // Wrong size or empty mask deselects.
    state.setSelection(QRectF(1, 1, 2, 2), false);
    state.setSelectionMask(QImage(8, 8, QImage::Format_Grayscale8));
    CHECK(d->selection.isEmpty() && !d->selectionIsMask);
    QImage emptyMask(40, 30, QImage::Format_Grayscale8);
    emptyMask.fill(0);
    state.setSelection(QRectF(1, 1, 2, 2), false);
    state.setSelectionMask(emptyMask);
    CHECK(d->selection.isEmpty() && !d->selectionIsMask);

    // clearSelection clears the mask too.
    state.setSelectionMask(m);
    CHECK(d->selectionIsMask);
    state.clearSelection();
    CHECK(!d->selectionIsMask && d->selectionMask.isNull());
}

void test_refine_stages() {
    // Hard 4px block at (4,4)-(7,7).
    QImage m(16, 16, QImage::Format_Grayscale8);
    m.fill(0);
    for (int y = 4; y <= 7; ++y)
        for (int x = 4; x <= 7; ++x) m.scanLine(y)[x] = 255;

    // Grow dilates; bbox inflates by ~radius.
    const QImage grown = selectionMaskGrow(m, 2);
    const QRect gb = selectionMaskBbox(grown);
    CHECK(gb.left() <= 2 && gb.top() <= 2 && gb.right() >= 9 && gb.bottom() >= 9);

    // Shrink erodes.
    const QImage shrunk = selectionMaskGrow(m, -1);
    const QRect sb = selectionMaskBbox(shrunk);
    CHECK(sb.width() <= 4 && sb.height() <= 4);

    // Smooth adds a soft penumbra at the edge.
    const QImage smoothed = selectionMaskSmooth(m, 2);
    bool soft = false;
    for (int y = 0; y < 16 && !soft; ++y) {
        const uchar* row = smoothed.constScanLine(y);
        for (int x = 0; x < 16 && !soft; ++x)
            if (row[x] > 0 && row[x] < 255) soft = true;
    }
    CHECK(soft);
    CHECK(!selectionMaskBbox(smoothed).isEmpty());

    // Feather widens the transition band.
    const QImage feathered = selectionMaskFeather(m, 2);
    const QRect fb = selectionMaskBbox(feathered);
    CHECK(!fb.isEmpty());
    bool edge = false;
    for (int y = 0; y < 16 && !edge; ++y) {
        const uchar* row = feathered.constScanLine(y);
        for (int x = 0; x < 16 && !edge; ++x)
            if (row[x] > 0 && row[x] < 255) edge = true;
    }
    CHECK(edge);

    // Ramp tweaks midtones; 1 is identity, input untouched.
    QImage softM(8, 8, QImage::Format_Grayscale8);
    softM.fill(160);
    const QImage hard = selectionMaskRamp(softM, 2.0);
    const QImage same = selectionMaskRamp(softM, 1.0);
    CHECK(hard.constScanLine(4)[4] > 0 && hard.constScanLine(4)[4] < 160);
    CHECK(same.constScanLine(4)[4] == 160);     // identity
    CHECK(softM.constScanLine(4)[4] == 160);    // input untouched

    // Brush stamp lifts toward 255, erases toward 0, soft rim.
    const QImage lifted = selectionMaskBrushStamp(m, QPointF(6.0, 10.0), 3.0,
                                                  0.5, 255);
    CHECK(lifted.constScanLine(10)[6] == 255);            // stamp centre
    bool rim = false;
    {
        const int y = 10;
        for (int x = 0; x < 16; ++x)
            if (lifted.constScanLine(y)[x] > 0 && lifted.constScanLine(y)[x] < 255)
                rim = true;
    }
    CHECK(rim);                                          // soft edge
    const QImage erased = selectionMaskBrushStamp(m, QPointF(10.0, 5.0), 2.0,
                                                  1.0, 0);
    CHECK(erased.constScanLine(5)[10] == 0);
}

void test_blend_ai_hair() {
    // Band-limited AI blend: AI wins only near the base edge.
    QImage base(16, 16, QImage::Format_Grayscale8);
    base.fill(0);
    for (int y = 4; y <= 11; ++y)
        for (int x = 4; x <= 11; ++x) base.scanLine(y)[x] = 255;
    // All-white AI: far exterior must stay 0 (band-limited), interior stays.
    QImage white(16, 16, QImage::Format_Grayscale8);
    white.fill(255);
    const QImage wblend = selectionMaskBlendAiHair(base, white, 4);
    CHECK(wblend.constScanLine(0)[0] == 0);     // far outside untouched
    CHECK(wblend.constScanLine(8)[8] == 255);   // deep interior untouched
    // Inverted AI (0 inside, 255 outside) must auto-flip, never flop the matte.
    QImage inv(16, 16, QImage::Format_Grayscale8);
    inv.fill(255);
    for (int y = 4; y <= 11; ++y)
        for (int x = 4; x <= 11; ++x) inv.scanLine(y)[x] = 0;
    const QImage iblend = selectionMaskBlendAiHair(base, inv, 4);
    CHECK(iblend.constScanLine(8)[8] == 255);   // interior survives inversion
    CHECK(iblend.constScanLine(0)[0] == 0);     // exterior survives inversion
    // Soft strand from AI inside the band is taken; null/mismatch/empty pass.
    QImage strand = base;
    strand.scanLine(8)[12] = 128;  // AI-only wisp just outside the edge
    const QImage sblend = selectionMaskBlendAiHair(base, strand, 4);
    CHECK(sblend.constScanLine(8)[12] == 128);  // band pixel takes AI
    CHECK(sblend.constScanLine(8)[8] == 255);
    CHECK(selectionMaskBlendAiHair(QImage(), strand, 4).isNull());
    CHECK(selectionMaskBlendAiHair(base, QImage(8, 8, QImage::Format_Grayscale8),
                                   4).constScanLine(8)[8] == 255);
    QImage empty(16, 16, QImage::Format_Grayscale8);
    empty.fill(0);
    CHECK(selectionMaskBlendAiHair(empty, white, 4).constScanLine(8)[8] == 0);
}

void test_lasso_polygon() {
    // Triangle: inside fully covered, outside empty, bbox tight.
    {
        const std::vector<QPointF> tri{QPointF(2, 2), QPointF(12, 2),
                                       QPointF(2, 12)};
        const QImage m =
            selectionMaskFromPolygon(tri, QSize(16, 16), false, 0);
        CHECK(!m.isNull());
        CHECK(m.constScanLine(4)[4] == 255);  // deep inside
        CHECK(m.constScanLine(13)[13] == 0);  // outside
        const QRect bb = selectionMaskBbox(m);
        CHECK(!bb.isEmpty());
        CHECK(bb.left() >= 2 && bb.top() >= 2);
    }
    // Anti-aliased edge produces a soft rim; hard edge does not. A diagonal
    // edge exercises the sub-row coverage (axis-aligned boxes stay hard).
    {
        const std::vector<QPointF> tri{QPointF(4, 4), QPointF(12, 4),
                                       QPointF(4, 12)};
        const QImage hard = selectionMaskFromPolygon(tri, QSize(16, 16),
                                                     false, 0);
        const QImage soft = selectionMaskFromPolygon(tri, QSize(16, 16),
                                                     true, 0);
        bool hardRim = false, softRim = false;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int hv = hard.constScanLine(y)[x];
                const int sv = soft.constScanLine(y)[x];
                if (hv > 0 && hv < 255) hardRim = true;
                if (sv > 0 && sv < 255) softRim = true;
            }
        }
        CHECK(!hardRim);
        CHECK(softRim);
        CHECK(soft.constScanLine(5)[5] == 255);  // interior still solid
    }
    // Feather softens the finished edge.
    {
        const std::vector<QPointF> sq{QPointF(4, 4), QPointF(12, 4),
                                      QPointF(12, 12), QPointF(4, 12)};
        const QImage f =
            selectionMaskFromPolygon(sq, QSize(16, 16), false, 2);
        bool edge = false;
        for (int y = 0; y < 16 && !edge; ++y)
            for (int x = 0; x < 16 && !edge; ++x) {
                const int v = f.constScanLine(y)[x];
                if (v > 0 && v < 255) edge = true;
            }
        CHECK(edge);
    }
    // Degenerate loops select nothing.
    {
        CHECK(selectionMaskFromPolygon({}, QSize(16, 16), true, 0).isNull());
        const std::vector<QPointF> two{QPointF(1, 1), QPointF(5, 5)};
        CHECK(selectionMaskFromPolygon(two, QSize(16, 16), true, 0).isNull());
        CHECK(selectionMaskFromPolygon(
                  {QPointF(1, 1), QPointF(2, 2), QPointF(3, 3)}, QSize(0, 0),
                  true, 0)
                  .isNull());
    }
}

// Invert / combine / Wand / alpha-cut on top of the mask channel.
void test_selection_ops() {
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("ops"), QSize(40, 30), 300);
    CHECK(d != nullptr);
    if (!d) return;

    // Invert rect: complement, one undo step.
    state.setSelection(QRectF(5, 5, 10, 10), false);
    const int depth0 = state.undoDepth();
    state.invertSelection();
    CHECK(d->selectionIsMask);
    CHECK(!d->selectionMask.isNull());
    CHECK(d->selectionMask.constScanLine(10)[10] == 0);   // inside was selected
    CHECK(d->selectionMask.constScanLine(1)[1] == 255);   // outside now selected
    CHECK_EQ(state.undoDepth(), depth0 + 1);
    state.undo();
    CHECK(!d->selectionIsMask);
    CHECK(d->selection == QRectF(5.0, 5.0, 10.0, 10.0));

    // --- invert an arbitrary-shape mask ------------------------------------
    state.setSelectionMask(rectMask(40, 30, {QRect(QPoint(5, 6), QPoint(19, 23))}));
    state.invertSelection();
    CHECK(d->selectionMask.constScanLine(10)[10] == 0);
    CHECK(d->selectionMask.constScanLine(1)[1] == 255);

    // --- combine modes: add then subtract ----------------------------------
    const QImage a = rectMask(40, 30, {QRect(QPoint(0, 0), QPoint(9, 9))});
    const QImage b = rectMask(40, 30, {QRect(QPoint(5, 5), QPoint(14, 14))});
    state.setSelectionMask(a);
    state.combineSelection(b, 1, QStringLiteral("Add"), QStringLiteral("x"));
    CHECK(d->selectionMask.constScanLine(2)[2] == 255);
    CHECK(d->selectionMask.constScanLine(12)[12] == 255);
    CHECK(d->selectionMask.constScanLine(20)[20] == 0);
    state.combineSelection(b, 2, QStringLiteral("Sub"), QStringLiteral("x"));
    CHECK(d->selectionMask.constScanLine(2)[2] == 255);   // a-only survives
    CHECK(d->selectionMask.constScanLine(7)[7] == 0);     // overlap removed
    CHECK(d->selectionMask.constScanLine(12)[12] == 0);   // b-only not added

    // --- Magic Wand floods the clicked colour, not the whole frame ---------
    DocumentItem* wand = state.addDocument(QStringLiteral("wand"), QSize(32, 20), 300);
    CHECK(wand != nullptr);
    state.setActiveTool(ToolId::MagicWand);
    CHECK(state.paintDab(QPointF(8, 10), 6.0, 1.0, 1.0, QColor(0, 0, 255)));
    state.flushPaint();
    CHECK(state.magicWandSelectAt(QPointF(8, 10)));
    CHECK(wand->selectionIsMask);
    const QRect wb = pittore::ui::selectionMaskBbox(wand->selectionMask);
    CHECK(!wb.isEmpty());
    CHECK(wb.right() < 20);   // stopped at the colour edge
    CHECK(wand->selectionMask.constScanLine(10)[8] > 127);
    CHECK(wand->selectionMask.constScanLine(10)[28] < 128);

    // --- applyActiveLayerAlpha edits the LIVE layer; undo restores ---------
    DocumentItem* al = state.addDocument(QStringLiteral("alpha"), QSize(8, 8), 300);
    CHECK(al != nullptr);
    std::vector<float> mask(8 * 8, 1.0f);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 4; ++x) mask[std::size_t(y) * 8 + x] = 0.0f;
    LayerItem* lyr = state.activeLayer();
    CHECK(lyr && lyr->pixels);
    const float aBefore = lyr->pixels->at(1, 4).a;
    CHECK(state.applyActiveLayerAlpha(mask.data(), 8, 8, QStringLiteral("Cut"),
                                      QStringLiteral("x")));
    lyr = state.activeLayer();
    CHECK(lyr && lyr->pixels);
    CHECK(lyr->pixels->at(1, 4).a < aBefore);   // live pixels actually changed
    CHECK(lyr->pixels->at(6, 4).a > 0.99f);     // kept side untouched
    state.undo();
    CHECK(state.activeLayer()->pixels->at(1, 4).a > aBefore - 1e-3f);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir = QDir::tempPath() + QStringLiteral("/pittore-selection-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_engine_coverage();
    test_outline();
    test_app_state_selection();
    test_refine_stages();
    test_blend_ai_hair();
    test_lasso_polygon();
    test_selection_ops();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}