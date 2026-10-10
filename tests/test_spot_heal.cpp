// Spot Healing Brush (all types): blemish removal, donor search,
// grain smoothing, transparency, undo.
// Headless AppState test, CPU backend.
#include <QCoreApplication>
#include <QDir>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"

using namespace pittore::ui;
using pittore::RGBAf;

namespace {

// Fresh doc with a WxH pixel layer; layer 0 is the test layer.
DocumentItem* makeDoc(AppState& state, const QString& title, int w = 16,
                      int h = 16) {
    DocumentItem* d = state.addDocument(title, QSize(64, 64), 300);
    CHECK(d != nullptr);
    if (!d) return nullptr;
    d->layers.clear();
    LayerItem l;
    l.name = QStringLiteral("T");
    l.kind = LayerItem::Kind::Pixel;
    l.pixels = std::make_shared<pittore::Image>(std::uint32_t(w),
                                                 std::uint32_t(h));
    l.pixels->fill(RGBAf{0, 0, 0, 0});
    l.offset = QPointF(0, 0);
    l.scaleX = l.scaleY = 1.0;
    d->layers.append(std::move(l));
    d->activeLayer = 0;
    d->selectedLayers = QVector<int>{0};
    d->rebuildComposite();
    return d;
}

void fillRect(DocumentItem& d, int x0, int y0, int x1, int y1, RGBAf c) {
    auto& img = *d.layers[0].pixels;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) img.at(x, y) = c;
}

// Per-texel noise in [0,1). Unsigned math: signed overflow is UB.
float hash01(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                      static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffffff) / float(0x1000000);
}

}  // namespace

// Proximity removes a dark spot on a flat field.
static void test_proximity_removes_spot() {
    AppState state;
    DocumentItem* d = makeDoc(state, QStringLiteral("proxspot"), 48, 48);
    if (!d) return;
    fillRect(*d, 0, 0, 47, 47, RGBAf{1, 0, 0, 1});
    fillRect(*d, 23, 23, 25, 25, RGBAf{0.2f, 0, 0, 1});  // dark speck
    CHECK(state.spotHealDab(QPointF(24, 24), 6.0, 1.0, /*Proximity*/ 2,
                            /*diffusion*/ 5, /*sampleAll*/ false));
    const RGBAf c = d->layers[0].pixels->at(24, 24);
    CHECK(c.r > 0.9f);
    CHECK_EQ(c.a, 1.0f);
}

// Surroundings win: red blotch on green heals green.
static void test_surroundings_win() {
    AppState state;
    DocumentItem* d = makeDoc(state, QStringLiteral("surround"), 48, 48);
    if (!d) return;
    fillRect(*d, 0, 0, 47, 47, RGBAf{0, 1, 0, 1});
    fillRect(*d, 23, 23, 24, 24, RGBAf{1, 0, 0, 1});  // red blotch
    CHECK(state.spotHealDab(QPointF(24, 24), 6.0, 1.0, 2, 5, false));
    const RGBAf c = d->layers[0].pixels->at(23, 23);
    CHECK(c.g > 0.8f);
    CHECK(c.r < 0.3f);
}

// Proximity continues stripes through a blotch.
static void test_proximity_continues_stripes() {
    AppState state;
    DocumentItem* d = makeDoc(state, QStringLiteral("stripes"), 48, 48);
    if (!d) return;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            d->layers[0].pixels->at(x, y) =
                ((y / 2) & 1) ? RGBAf{0.6f, 0, 0, 1} : RGBAf{1, 0, 0, 1};
    fillRect(*d, 21, 21, 26, 26, RGBAf{0.5f, 0.5f, 0.5f, 1});
    CHECK(state.spotHealDab(QPointF(24, 24), 5.0, 1.0, 2, 5, false));
    // (24,24) is bright stripe phase.
    const RGBAf c = d->layers[0].pixels->at(24, 24);
    CHECK(c.r > 0.75f);
    CHECK(c.g < 0.25f);
    CHECK(c.b < 0.25f);
}

// Create Texture smooths grain but keeps mean colour.
static void test_create_texture_smooths() {
    AppState state;
    DocumentItem* d = makeDoc(state, QStringLiteral("texture"));
    if (!d) return;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            d->layers[0].pixels->at(x, y) =
                RGBAf{0.8f + 0.2f * hash01(x, y), 0, 0, 1};
    double mean0 = 0, var0 = 0;
    for (int y = 5; y <= 10; ++y)
        for (int x = 5; x <= 10; ++x)
            mean0 += d->layers[0].pixels->at(x, y).r;
    mean0 /= 36.0;
    for (int y = 5; y <= 10; ++y)
        for (int x = 5; x <= 10; ++x) {
            const double v = d->layers[0].pixels->at(x, y).r - mean0;
            var0 += v * v;
        }
    var0 /= 36.0;
    CHECK(state.spotHealDab(QPointF(8, 8), 8.0, 1.0, /*CreateTexture*/ 1, 5,
                            false));
    double mean1 = 0, var1 = 0;
    for (int y = 5; y <= 10; ++y)
        for (int x = 5; x <= 10; ++x)
            mean1 += d->layers[0].pixels->at(x, y).r;
    mean1 /= 36.0;
    for (int y = 5; y <= 10; ++y)
        for (int x = 5; x <= 10; ++x) {
            const double v = d->layers[0].pixels->at(x, y).r - mean1;
            var1 += v * v;
        }
    var1 /= 36.0;
    CHECK(var1 < var0);
    CHECK(std::fabs(mean1 - mean0) < 0.08);
}

// Content-Aware reaches what Proximity can't: wide search restores the dot.
static void test_content_aware_beats_proximity() {
    float errProx = 0, errCA = 0;
    for (int type : {2, 0}) {
        AppState state;
        DocumentItem* d = makeDoc(
            state, QStringLiteral("dots%1").arg(type), 96, 96);
        if (!d) return;
        fillRect(*d, 0, 0, 95, 95, RGBAf{1, 0, 0, 1});
        for (int gy = 16; gy < 96; gy += 32)
            for (int gx = 16; gx < 96; gx += 32)
                fillRect(*d, gx, gy, gx + 1, gy + 1, RGBAf{0.4f, 0, 0, 1});
        fillRect(*d, 44, 44, 51, 51, RGBAf{0.5f, 0.5f, 0.5f, 1});
        CHECK(state.spotHealDab(QPointF(48, 48), 7.0, 1.0, type, 5, false));
        // (48,48) is inside the blotted dot.
        const RGBAf c = d->layers[0].pixels->at(48, 48);
        const float err = std::fabs(c.r - 0.4f) + c.g + c.b;
        if (type == 2)
            errProx = err;
        else
            errCA = err;
    }
    CHECK(errCA < 0.3f);
    CHECK(errCA < errProx);
}

// Junction: per-pixel match continues stripes on both sides; one offset can't.
static void test_junction_both_bars() {
    float errProx = 0, errCA = 0;
    for (int type : {2, 0}) {
        AppState state;
        DocumentItem* d =
            makeDoc(state, QStringLiteral("jx%1").arg(type), 64, 64);
        if (!d) return;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const bool dark = x < 32 ? ((y / 2) & 1) : ((x / 2) & 1);
                d->layers[0].pixels->at(x, y) =
                    dark ? RGBAf{0.55f, 0, 0, 1} : RGBAf{1, 0, 0, 1};
            }
        fillRect(*d, 24, 24, 39, 39, RGBAf{0.5f, 0.5f, 0.5f, 1});
        CHECK(state.spotHealDab(QPointF(32, 32), 12.0, 1.0, type, 5, false));
        // Left: bright phase; right: dark phase.
        const RGBAf l = d->layers[0].pixels->at(26, 32);
        const RGBAf r = d->layers[0].pixels->at(38, 32);
        const float err =
            std::fabs(l.r - 1.0f) + l.g + l.b + std::fabs(r.r - 0.55f) + r.g + r.b;
        if (type == 2)
            errProx = err;
        else
            errCA = err;
    }
    CHECK(errCA < 0.5f);
    CHECK(errCA < errProx);
}

// Big brush (r > 16: the coarse-lattice path, which no other test reaches):
// the hole is wider than a patch can score, so most of the interior has no
// local evidence at all. It must inherit the rim's verified continuations
// (propagation floods them inward) — and at a junction the left and right
// sides need different donors: the left bands repeat every 4 px in y
// while the right bands run diagonally with period 6 in x+y, so the
// axis-aligned shifts either search grid tries cannot continue both at
// once (a vertical multiple-of-4 shift keeps the left but moves the right
// by 40 = 13*3+1, flipping most of its bands). Proximity must settle for
// a one-sided donor while per-pixel refinement serves each side.
static void test_big_brush_continues_junction() {
    float errProx = 0, errCA = 0;
    for (int type : {2, 0}) {
        AppState state;
        DocumentItem* d =
            makeDoc(state, QStringLiteral("bigjx%1").arg(type), 128, 128);
        if (!d) return;
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const bool dark = x < 64 ? ((y / 2) & 1) : (((x + y) / 3) & 1);
                d->layers[0].pixels->at(x, y) =
                    dark ? RGBAf{0.55f, 0, 0, 1} : RGBAf{1, 0, 0, 1};
            }
        // Blot exactly the hole disc (pixel-centre test): every texel the
        // scorer can still *see* keeps its stripes, so the rim it scores
        // against is the clean continuation and nothing else. The frame has
        // to be well over 4r wide, or no donor disc can both clear the hole
        // and fit in the image.
        for (int y = 44; y <= 84; ++y)
            for (int x = 44; x <= 84; ++x) {
                const float dx = x + 0.5f - 64.0f;
                const float dy = y + 0.5f - 64.0f;
                if (dx * dx + dy * dy < 20.0f * 20.0f)
                    d->layers[0].pixels->at(x, y) =
                        RGBAf{0.5f, 0.5f, 0.5f, 1};
            }
        CHECK(state.spotHealDab(QPointF(64, 64), 20.0, 1.0, type, 5, false));
        // Outer ring (scorable from the rim): left vertical phase, right
        // diagonal phase. Centre (deeper than any patch reaches):
        // flood-dependent. The right-centre probe sits on a light diagonal
        // band (((66+66)/3)&1 == 0).
        const RGBAf l = d->layers[0].pixels->at(50, 64);
        const RGBAf r = d->layers[0].pixels->at(78, 64);
        const RGBAf cl = d->layers[0].pixels->at(62, 64);
        const RGBAf cr = d->layers[0].pixels->at(66, 66);
        const float err = std::fabs(l.r - 1.0f) + l.g + l.b +
                          std::fabs(r.r - 0.55f) + r.g + r.b +
                          std::fabs(cl.r - 1.0f) + cl.g + cl.b +
                          std::fabs(cr.r - 1.0f) + cr.g + cr.b;
        if (type == 2)
            errProx = err;
        else
            errCA = err;
    }
    CHECK(errCA < 1.0f);
    CHECK(errCA < errProx);
}

// Huge brush (r=48): the user's large-brush case — hole far wider than any
// patch can score, so the interior must run on the rank-flood, and the
// frame must hold a donor disc that both clears the hole by 2r and fits
// (>= ~5r across, or no candidate is valid at all).
static void test_huge_brush_continues_junction() {
    float errProx = 0, errCA = 0;
    for (int type : {2, 0}) {
        AppState state;
        DocumentItem* d =
            makeDoc(state, QStringLiteral("hugejx%1").arg(type), 256, 256);
        if (!d) return;
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const bool dark = x < 128 ? ((y / 2) & 1) : ((x / 2) & 1);
                d->layers[0].pixels->at(x, y) =
                    dark ? RGBAf{0.55f, 0, 0, 1} : RGBAf{1, 0, 0, 1};
            }
        for (int y = 80; y <= 176; ++y)
            for (int x = 80; x <= 176; ++x) {
                const float dx = x + 0.5f - 128.0f;
                const float dy = y + 0.5f - 128.0f;
                if (dx * dx + dy * dy < 48.0f * 48.0f)
                    d->layers[0].pixels->at(x, y) =
                        RGBAf{0.5f, 0.5f, 0.5f, 1};
            }
        CHECK(state.spotHealDab(QPointF(128, 128), 48.0, 1.0, type, 5, false));
        // Scorable rim band (outside a patch's reach of the centre): left
        // keeps the vertical phase, right the horizontal one.
        const RGBAf l = d->layers[0].pixels->at(88, 130);
        const RGBAf r = d->layers[0].pixels->at(168, 130);
        // Deeper than any patch reaches — flood-dependent only.
        const RGBAf cl = d->layers[0].pixels->at(118, 128);
        const RGBAf cr = d->layers[0].pixels->at(138, 138);
        const float err = std::fabs(l.r - 0.55f) + l.g + l.b +
                          std::fabs(r.r - 1.0f) + r.g + r.b +
                          std::fabs(cl.r - 1.0f) + cl.g + cl.b +
                          std::fabs(cr.r - 0.55f) + cr.g + cr.b;
        if (type == 2)
            errProx = err;
        else
            errCA = err;
    }
    CHECK(errCA < 1.0f);
    CHECK(errCA < errProx);
}

// One stroke = one undo step.
static void test_heal_undo_single_step() {    AppState state;
    DocumentItem* d = makeDoc(state, QStringLiteral("healundo"), 48, 48);
    if (!d) return;
    fillRect(*d, 0, 0, 47, 47, RGBAf{0, 0, 1, 1});
    fillRect(*d, 23, 23, 25, 25, RGBAf{0.9f, 0.9f, 0.2f, 1});
    const int depth = state.undoDepth();
    state.beginUndoStep();
    state.copyOnWriteActiveLayer();
    CHECK(state.spotHealDab(QPointF(24, 24), 6.0, 1.0, 2, 5, false));
    state.commitUndoStep(QStringLiteral("Spot Healing"),
                         QStringLiteral("spot-heal"));
    CHECK_EQ(state.undoDepth(), depth + 1);
    state.undo();
    CHECK_EQ(d->layers[0].pixels->at(24, 24).r, 0.9f);
    state.redo();
    CHECK(d->layers[0].pixels->at(24, 24).b > 0.5f);
}

// Sample All Layers heals from the composite onto transparency.
static void test_sample_all_deposits() {
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("sampleall"),
                                        QSize(64, 64), 300);
    CHECK(d != nullptr);
    if (!d) return;
    d->layers.clear();
    LayerItem top;
    top.name = QStringLiteral("Top");
    top.kind = LayerItem::Kind::Pixel;
    top.pixels = std::make_shared<pittore::Image>(64, 64);
    top.pixels->fill(RGBAf{0, 0, 0, 0});
    LayerItem bottom;
    bottom.name = QStringLiteral("Bottom");
    bottom.kind = LayerItem::Kind::Pixel;
    bottom.pixels = std::make_shared<pittore::Image>(64, 64);
    bottom.pixels->fill(RGBAf{1, 0, 0, 1});
    d->layers.append(std::move(top));
    d->layers.append(std::move(bottom));
    d->activeLayer = 0;
    d->selectedLayers = QVector<int>{0};
    d->rebuildComposite();
    CHECK(state.spotHealDab(QPointF(32, 32), 8.0, 1.0, 2, 5,
                            /*sampleAll*/ true));
    const RGBAf c = d->layers[0].pixels->at(32, 32);
    CHECK(c.a > 0.5f);
    CHECK(c.r > 0.5f);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-spot-heal-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_proximity_removes_spot();
    test_surroundings_win();
    test_proximity_continues_stripes();
    test_create_texture_smooths();
    test_content_aware_beats_proximity();
    test_junction_both_bars();
    test_big_brush_continues_junction();
    test_huge_brush_continues_junction();
    test_heal_undo_single_step();
    test_sample_all_deposits();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
