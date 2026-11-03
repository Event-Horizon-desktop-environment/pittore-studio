// test_place_ui.cpp — placed-layer workflow through AppState: drop/place an
// image, move it, scale it (up, down, and back), paint on it.
//
// Runs headless under QCoreApplication (no widgets are constructed) with the
// CPU backend: XDG_CONFIG_HOME is pointed at a scratch dir by meson so the
// real Settings.toml is never touched, and engine logs are redirected to the
// system temp dir so the user's log files stay clean.
#include <QCoreApplication>
#include <QDir>
#include <QImage>

#include "engine/compute/factory.h"
#include "engine/compute/adjust.h"
#include "engine/core/log.h"
#include "engine/io/af_layers.h"
#include "test_util.h"
#include "ui/app_state.h"

using namespace pittore::ui;

namespace {

QImage solidImage(int w, int h, QRgb c) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

bool isRed(const QColor& c) { return c.red() > 250 && c.green() < 5 && c.blue() < 5; }
bool isBg(const QColor& c) {
    return abs(c.red() - 0xf2) <= 2 && abs(c.green() - 0xf2) <= 2 &&
           abs(c.blue() - 0xf2) <= 2;
}

}  // namespace

static void test_place_ui() {
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("t"), QSize(64, 48), 300);
    CHECK(d != nullptr);
    if (!d) return;

    // --- place a 16x16 opaque red image at the centre, 1:1 -----------------
    state.placeImageLayer(solidImage(16, 16, 0xFFFF0000), QStringLiteral("red"),
                          QPointF(32, 24), 1.0);
    LayerItem* l = state.activeLayer();
    CHECK(l != nullptr);
    CHECK(l->pixels != nullptr);
    CHECK_EQ(l->pixels->width(), 16u);
    CHECK_EQ(l->pixels->height(), 16u);
    CHECK(l->offset == QPointF(24, 16));
    CHECK_EQ(topPixelLayerAt(*d, QPointF(32, 24)), d->activeLayer);
    CHECK(isRed(d->composite.pixelColor(32, 24)));
    CHECK(isBg(d->composite.pixelColor(0, 0)));

    // --- layer preview: fits the content to the box, cached until it changes -
    const QImage t1 = layerThumbnail(*d, *l, 30);
    CHECK(!t1.isNull());
    CHECK_EQ(t1.width(), 30);
    CHECK_EQ(t1.height(), 30);
    CHECK(isRed(t1.pixelColor(0, 0)));         // content covers the whole box
    CHECK(isRed(t1.pixelColor(15, 16)));
    CHECK(isRed(t1.pixelColor(29, 29)));
    // The preview is cached on the layer until the model invalidates it.
    CHECK(layerThumbnail(*d, *l, 30).cacheKey() == t1.cacheKey());

    // --- move: the composite follows, the source is untouched --------------
    CHECK(state.setActiveLayerPlacement(QPointF(32, 16), 1.0, 1.0));
    CHECK(isBg(d->composite.pixelColor(24, 24)));
    const QColor moved = d->composite.pixelColor(40, 24);
    CHECK(isRed(moved));
    CHECK_EQ(l->pixels->width(), 16u);  // native pixels never resampled
    // The preview tracks the pixels, not their placement, so moving leaves it
    // (and its cache) untouched.
    CHECK(layerThumbnail(*d, *l, 30).cacheKey() == t1.cacheKey());
    // The background layer previews as opaque white across the canvas.
    const LayerItem& bg = d->layers.last();
    const QImage bgThumb = layerThumbnail(*d, bg, 30);
    CHECK(isBg(bgThumb.pixelColor(15, 11)));

    // --- upscale 2x in place: integer source texels stay exact -------------
    CHECK(state.setActiveLayerPlacement(QPointF(32, 16), 2.0, 2.0));
    CHECK(isRed(d->composite.pixelColor(40, 24)));
    // The pixel just outside the edge is half covered: red hue at half alpha
    // over the background, while the next one out is pure background.
    const QColor edge = d->composite.pixelColor(31, 24);
    CHECK(edge.red() > 200 && edge.green() > 100 && edge.green() < 150);
    CHECK(isBg(d->composite.pixelColor(30, 24)));

    // --- lossless round trip: 2x then back to 1x restores the pixels -------
    CHECK(state.setActiveLayerPlacement(QPointF(32, 16), 1.0, 1.0));
    const QColor back = d->composite.pixelColor(40, 24);
    CHECK_EQ(back.red(), moved.red());
    CHECK_EQ(back.green(), moved.green());
    CHECK_EQ(back.blue(), moved.blue());

    // --- downscale 0.5x exercises the box path; source intact --------------
    CHECK(state.setActiveLayerPlacement(QPointF(28, 20), 0.5, 0.5));
    CHECK(isRed(d->composite.pixelColor(32, 24)));
    CHECK(isBg(d->composite.pixelColor(0, 0)));
    CHECK_EQ(l->pixels->width(), 16u);

    // --- painting lands through the transform ------------------------------
    state.setActiveLayerIndex(d->activeLayer);  // still the placed layer
    CHECK(state.paintDab(QPointF(32, 24), 5.0, 1.0, 1.0, QColor(Qt::blue)));
    state.flushPaint();
    const QColor painted = d->composite.pixelColor(32, 24);
    CHECK(painted.blue() > 0);
    // A pixel edit invalidates the cached preview: the dab lands at the source
    // centre, so the rebuilt thumbnail reads blue there too.
    CHECK(layerThumbnail(*d, *l, 30).pixelColor(15, 15).blue() > 150);

    // --- locked background refuses placement changes -----------------------
    const int bgIndex = d->layers.size() - 1;
    state.setActiveLayerIndex(bgIndex);
    CHECK(!state.setActiveLayerPlacement(QPointF(5, 5), 1.0, 1.0));
    CHECK_EQ(topPixelLayerAt(*d, QPointF(0, 0)), bgIndex);
    CHECK_EQ(topPixelLayerAt(*d, QPointF(1000, 1000)), -1);
}

// Layer masks and clipping through AppState on the CPU backend: a mask hides
// and reveals its own layer, mask strokes undo, and a clipped layer only
// shows where its base shows.
static void test_layer_masks_ui() {
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("m"), QSize(32, 24), 300);
    CHECK(d != nullptr);
    if (!d) return;

    state.placeImageLayer(solidImage(16, 16, 0xFFFF0000), QStringLiteral("red"),
                          QPointF(16, 12), 1.0);
    CHECK(isRed(d->composite.pixelColor(16, 12)));
    CHECK(state.addLayerMask(1.0f));
    LayerItem* l = state.activeLayer();
    CHECK(l && l->hasMask && l->mask && l->maskSelected);
    CHECK(!layerMaskThumbnail(*l, 16).isNull());
    CHECK(isRed(d->composite.pixelColor(16, 12)));

    // Black paint on the selected mask conceals the layer; undo restores it.
    state.beginUndoStep();
    CHECK(state.copyOnWriteActiveMask());
    CHECK(state.paintDab(QPointF(16, 12), 4.0, 1.0, 1.0, QColor(Qt::black)));
    state.flushPaint();
    state.commitUndoStep(QStringLiteral("Mask"), QStringLiteral("mask"));
    CHECK(isBg(d->composite.pixelColor(16, 12)));
    state.undo();
    CHECK(isRed(d->composite.pixelColor(16, 12)));
    state.redo();
    CHECK(isBg(d->composite.pixelColor(16, 12)));

    // The eraser reveals mask coverage again.
    state.beginUndoStep();
    CHECK(state.copyOnWriteActiveMask());
    CHECK(state.eraseDab(QPointF(16, 12), 4.0, 1.0, 1.0));
    state.flushPaint();
    state.commitUndoStep(QStringLiteral("Mask"), QStringLiteral("mask"));
    CHECK(isRed(d->composite.pixelColor(16, 12)));

    // Invert/enable/delete all change the composite through one code path.
    CHECK(state.invertLayerMask());
    CHECK(isBg(d->composite.pixelColor(16, 12)));
    CHECK(state.setLayerMaskEnabled(false));
    CHECK(isRed(d->composite.pixelColor(16, 12)));
    CHECK(state.setLayerMaskEnabled(true));
    CHECK(isBg(d->composite.pixelColor(16, 12)));
    CHECK(state.setLayerMaskLinked(false));
    CHECK(!state.activeLayer()->maskLinked);
    CHECK(state.setLayerMaskLinked(true));
    CHECK(state.deleteLayerMask());
    CHECK(isRed(d->composite.pixelColor(16, 12)));

    // Clipping: a blue layer over the red base shows while clipped, vanishes
    // when moved off the base, and returns when released.
    state.placeImageLayer(solidImage(8, 8, 0xFF0000FF), QStringLiteral("blue"),
                          QPointF(16, 12), 1.0);
    CHECK(state.toggleLayerClipped());
    CHECK(d->composite.pixelColor(16, 12).blue() > 150);
    CHECK(state.setActiveLayerPlacement(QPointF(0, 0), 1.0, 1.0));
    CHECK(isBg(d->composite.pixelColor(2, 2)));
    CHECK(state.toggleLayerClipped());
    CHECK(d->composite.pixelColor(2, 2).blue() > 150);
}

// Affinity masks stay live on import: the decoder's reveal buffer becomes an
// editable layer mask instead of baked alpha.
static void test_af_masks_ui() {
    AppState state;
    pittore::io::AfLayersDoc pd;
    pd.width = 8;
    pd.height = 4;
    pd.depth = 8;
    pd.complete = true;
    pittore::io::AfLayer l;
    l.left = 0;
    l.top = 0;
    l.width = 8;
    l.height = 4;
    l.name = "Pixel";
    l.rgba.assign(static_cast<std::size_t>(8) * 4 * 4, 0);
    for (std::size_t i = 0; i < l.rgba.size(); i += 4) {
        l.rgba[i + 0] = 65535;
        l.rgba[i + 3] = 65535;
    }
    pittore::io::AfMask m;
    m.left = 0;
    m.top = 0;
    m.width = 4;
    m.height = 4;
    m.px.assign(static_cast<std::size_t>(4) * 4, 255);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 2; ++x)
            m.px[static_cast<std::size_t>(y) * 4 + x] = 0;
    l.masks.push_back(std::move(m));
    pd.layers.push_back(std::move(l));

    QString error;
    CHECK(state.openAfLayers(QStringLiteral("af"), pd, QSize(8, 4), QImage(),
                             &error));
    DocumentItem* d = state.activeDocument();
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_EQ(d->layers.size(), 1);
    CHECK(d->layers[0].hasMask && d->layers[0].mask);
    CHECK_EQ(d->composite.pixelColor(0, 0).alpha(), 0);
    CHECK(isRed(d->composite.pixelColor(3, 0)));
    CHECK(isRed(d->composite.pixelColor(6, 0)));
}

// Live adjustment layers through AppState on the CPU backend: an invert
// adjustment visibly transforms the composite, honours masks and clipping,
// and its creation is undoable.
static void test_adjustment_layers_ui() {
    using pittore::compute::AdjustmentKind;
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("a"), QSize(32, 24), 300);
    CHECK(d != nullptr);
    if (!d) return;

    state.placeImageLayer(solidImage(16, 16, 0xFFFF0000), QStringLiteral("red"),
                          QPointF(16, 12), 1.0);
    CHECK(isRed(d->composite.pixelColor(16, 12)));

    // Invert above the red: red becomes cyan.
    CHECK(state.addAdjustmentLayer(static_cast<int>(AdjustmentKind::Invert)));
    const QColor inv = d->composite.pixelColor(16, 12);
    CHECK(inv.red() < 30 && inv.green() > 200 && inv.blue() > 200);
    // Creation is one undo step.
    state.undo();
    CHECK(isRed(d->composite.pixelColor(16, 12)));
    state.redo();
    CHECK(d->composite.pixelColor(16, 12).blue() > 200);

    // Opacity folds the effect: 0% opacity restores red.
    LayerItem* adj = state.activeLayer();
    CHECK(adj && adj->kind == LayerItem::Kind::Adjustment);
    adj->opacity = 0;
    d->rebuildComposite();
    CHECK(isRed(d->composite.pixelColor(16, 12)));
    adj->opacity = 100;
    d->rebuildComposite();

    // A mask on the adjustment confines it: hide-all restores red.
    CHECK(state.addLayerMask(0.0f));
    CHECK(isRed(d->composite.pixelColor(16, 12)));
    CHECK(state.fillLayerMask(1.0f));
    CHECK(d->composite.pixelColor(16, 12).blue() > 200);

    // Clipping an adjustment to the red base: moving the adjustment's...
    // (adjustments span the frame, so clip to base, then hide the base and
    // watch the clipped adjustment vanish with it).
    CHECK(state.toggleLayerClipped());
    CHECK(d->composite.pixelColor(16, 12).blue() > 200);
    d->layers[1].visible = false;  // hide the red base
    d->rebuildComposite();
    CHECK(isBg(d->composite.pixelColor(16, 12)));
    d->layers[1].visible = true;
    d->rebuildComposite();
    CHECK(d->composite.pixelColor(16, 12).blue() > 200);
}

// GPU variant of test_place_ui: a placed layer on a GPU backend composites
// through DocumentItem::placedSourceFor + ComputeBackend::composite_placed —
// the fused device sampler/cache the move/resize path uses. Runs the same
// scenario shape as the CPU test (upscaled, so bilinear sampling actually
// matters) and checks the device cache invalidates on a brush dab. Skips
// cleanly when no GPU backend is available.
static void test_place_ui_gpu() {
    bool haveGpu = false;
    for (const auto& dev : pittore::compute::enumerate_devices())
        if (dev.type != pittore::compute::BackendType::CPU) haveGpu = true;
    if (!haveGpu) {
        std::printf("  [place-ui] no GPU device — GPU placed-path checks skipped\n");
        return;
    }

    AppState state;
    AppSettings settings = state.settings();
    settings.gpuEnabled = true;   // first available GPU device
    state.applySettings(settings);
    if (state.computeBackend().type() == pittore::compute::BackendType::CPU) {
        std::printf("  [place-ui] GPU backend unavailable — placed-path checks skipped\n");
        return;
    }

    DocumentItem* d = state.addDocument(QStringLiteral("t"), QSize(64, 48), 300);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->backend != nullptr);
    CHECK(d->backend->type() == state.computeBackend().type());

    // --- place a 16x16 red image at 2x (bilinear upscale path) -------------
    state.placeImageLayer(solidImage(16, 16, 0xFFFF0000), QStringLiteral("red"),
                          QPointF(32, 24), 2.0);
    LayerItem* l = state.activeLayer();
    CHECK(l != nullptr);
    CHECK(l->offset == QPointF(16, 8));   // 32x32 doc-space, centre (32,24)
    CHECK(isRed(d->composite.pixelColor(24, 16)));   // source texel (4,4)

    // --- move: vacated strip redraws the background, new spot shows red ----
    CHECK(state.setActiveLayerPlacement(QPointF(16, 40), 2.0, 2.0));
    CHECK(!isRed(d->composite.pixelColor(24, 16)));  // y=16 is above new bounds
    CHECK(isRed(d->composite.pixelColor(24, 44)));   // source texel (4,2)
    CHECK(state.setActiveLayerPlacement(QPointF(16, 8), 2.0, 2.0));
    CHECK(isRed(d->composite.pixelColor(24, 16)));

    // --- scale down to 0.5x (box-average downscale kernel path) ------------
    CHECK(state.setActiveLayerPlacement(QPointF(8, 4), 0.5, 0.5));
    const QColor small = d->composite.pixelColor(12, 6);   // source texel (8,4)
    CHECK(small.red() > 200 && small.green() < 55 && small.blue() < 55);
    CHECK(isBg(d->composite.pixelColor(16, 6)));           // just past the edge

    // --- paint the placed layer: device source cache must refresh ----------
    state.setActiveTool(ToolId::Brush);
    CHECK(state.paintDab(QPointF(12, 6), 3.0, 1.0, 1.0, QColor(0, 0, 255)));
    state.flushPaint();
    CHECK(d->composite.pixelColor(12, 6).blue() > 0);
}

// A Recolour created over a pixel layer attaches to it: the grade applies to
// that layer's footprint only, leaving the rest of the stack untouched.
static void test_recolour_attach_ui() {
    using pittore::compute::AdjustmentKind;
    AppState state;
    DocumentItem* d = state.addDocument(QStringLiteral("r"), QSize(32, 24), 300);
    CHECK(d != nullptr);
    if (!d) return;

    // Blue base over the whole frame; red patch on top.
    state.placeImageLayer(solidImage(32, 24, 0xFF0000FF),
                          QStringLiteral("blue"), QPointF(16, 12), 1.0);
    state.placeImageLayer(solidImage(8, 8, 0xFFFF0000),
                          QStringLiteral("red"), QPointF(16, 12), 1.0);

    CHECK(state.addAdjustmentLayer(
        static_cast<int>(AdjustmentKind::Recolour)));
    LayerItem* adj = state.activeLayer();
    CHECK(adj && adj->kind == LayerItem::Kind::Adjustment);
    CHECK(adj && adj->clipped);
    CHECK(adj && adj->indent == 1);  // child row under the pixel layer
    if (!adj) return;

    CHECK(state.setAdjustmentParam(0, 120.0f));  // hue: green
    CHECK(state.setAdjustmentParam(1, 1.0f));    // saturation 100%
    CHECK(state.setAdjustmentParam(2, 0.0f));    // lightness 0

    // Inside the red base the grade lands; outside it the blue is untouched.
    const QColor graded = d->composite.pixelColor(16, 12);
    CHECK(graded.green() > 200 && graded.red() < 30 && graded.blue() < 30);
    const QColor outside = d->composite.pixelColor(2, 2);
    CHECK(outside.blue() > 200 && outside.red() < 10);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-place-ui-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_place_ui();
    test_layer_masks_ui();
    test_af_masks_ui();
    test_adjustment_layers_ui();
    test_recolour_attach_ui();
    test_place_ui_gpu();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
