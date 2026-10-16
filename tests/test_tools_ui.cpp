// The annotation/navigation/paint tools added on top of the brush engine:
// Paint Bucket + Magic Eraser flood fills, Dodge/Burn/Sponge tonal dabs, the
// Color Sampler / Ruler / Count / Note annotation model, and Rotate View.
// Runs headless (QT_QPA_PLATFORM=offscreen).
#include <cstdio>
#include <cmath>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QWidget>

#include "engine/compute/paint.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/canvas_view.h"

using namespace pittore::ui;

namespace {

void sendMouse(QWidget* target, QEvent::Type type, const QPointF& pos,
               Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPointF global = target->mapToGlobal(pos);
    QMouseEvent e(type, pos, global, button, buttons, mods);
    QApplication::sendEvent(target, &e);
}

// Force the active layer to own real pixels (addDocument leaves the background
// image lazily allocated), flood it with `color`, then flush so the composite
// matches. A dab radius far larger than the document covers every texel.
void primeLayer(AppState& state, QSize size, const QColor& color) {
    state.paintDab(QPointF(size.width() / 2.0, size.height() / 2.0), 1000.0, 1.0, 1.0,
                   color);
    state.flushPaint();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    AppState state;

    // ---- Paint Bucket: fills, one history step, undo restores -------------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("bucket"), QSize(32, 32), 300);
        CHECK(d != nullptr);
        primeLayer(state, QSize(32, 32), QColor(242, 242, 242));
        state.setActiveTool(ToolId::PaintBucket);
        state.setForeground(QColor(255, 0, 0));
        state.setOption(ToolId::PaintBucket, QStringLiteral("tolerance"), 0);
        state.setOption(ToolId::PaintBucket, QStringLiteral("antialias"), false);
        state.setOption(ToolId::PaintBucket, QStringLiteral("contiguous"), true);
        state.setOption(ToolId::PaintBucket, QStringLiteral("opacity"), 100);

        const int hist = d ? d->history.size() : 0;
        CHECK(state.paintBucketAt(QPointF(16.5, 16.5)));
        LayerItem* l = state.activeLayer();
        CHECK(l && l->pixels);
        if (l && l->pixels) {
            CHECK_NEAR(l->pixels->data()[0].r, 1.0, 0.02);
            CHECK_NEAR(l->pixels->data()[0].g, 0.0, 0.02);
        }
        CHECK(d && d->history.size() == hist + 1);

        state.undo();
        LayerItem* lu = state.activeLayer();
        CHECK(lu && lu->pixels);
        if (lu && lu->pixels) {
            CHECK_NEAR(lu->pixels->data()[0].r, 0.949, 0.03);  // 0xf2 restored
            CHECK_NEAR(lu->pixels->data()[0].g, 0.949, 0.03);
        }
    }

    // ---- Magic Eraser: floods alpha down, undo restores -------------------
    {
        state.addDocument(QStringLiteral("erase"), QSize(16, 16), 300);
        primeLayer(state, QSize(16, 16), QColor(0, 120, 255));
        state.setActiveTool(ToolId::MagicEraser);
        state.setOption(ToolId::MagicEraser, QStringLiteral("tolerance"), 0);
        state.setOption(ToolId::MagicEraser, QStringLiteral("antialias"), false);
        state.setOption(ToolId::MagicEraser, QStringLiteral("contiguous"), true);
        state.setOption(ToolId::MagicEraser, QStringLiteral("opacity"), 100);

        CHECK(state.magicEraseAt(QPointF(8, 8)));
        CHECK_NEAR(state.activeLayer()->pixels->data()[0].a, 0.0, 0.01);
        state.undo();
        CHECK_NEAR(state.activeLayer()->pixels->data()[0].a, 1.0, 0.01);
    }

    // ---- Dodge / Burn / Sponge tonal dabs --------------------------------
    {
        state.addDocument(QStringLiteral("tone"), QSize(16, 16), 300);
        primeLayer(state, QSize(16, 16), QColor(128, 128, 128));
        LayerItem* l = state.activeLayer();
        const std::size_t mid = 8 * 16 + 8;
        const float before = l->pixels->data()[mid].r;

        // Mirror the canvas stroke gesture: snapshot + private copy, then dab.
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        CHECK(state.toneDab(QPointF(8, 8), 4.0, 1.0, 1.0,
                            int(pittore::compute::ToneOp::Dodge), 1, false, false));
        state.commitUndoStep(QStringLiteral("Dodge"), QStringLiteral("dodge"));
        state.endToneStroke();
        CHECK(state.activeLayer()->pixels->data()[mid].r > before);

        state.undo();
        const float restored = state.activeLayer()->pixels->data()[mid].r;
        CHECK_NEAR(restored, before, 0.01);

        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        CHECK(state.toneDab(QPointF(8, 8), 4.0, 1.0, 1.0,
                            int(pittore::compute::ToneOp::Burn), 1, false, false));
        state.commitUndoStep(QStringLiteral("Burn"), QStringLiteral("burn"));
        state.endToneStroke();
        CHECK(state.activeLayer()->pixels->data()[mid].r < restored);

        // A stroke is many overlapping dabs; they must raise coverage (max), not
        // compound the tone. Scribbling across the same pixel leaves it at the
        // single-dab result instead of racing to white.
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        const float strokeStart = state.activeLayer()->pixels->data()[mid].r;
        CHECK(state.toneDab(QPointF(8, 8), 4.0, 1.0, 0.5,
                            int(pittore::compute::ToneOp::Dodge), 1, false, false));
        const float oneDab = state.activeLayer()->pixels->data()[mid].r;
        CHECK(oneDab > strokeStart);
        // Second overlapping dab one pixel over: coverage at the centre is
        // already 1, so the centre must not brighten any further.
        CHECK(state.toneDab(QPointF(9, 8), 4.0, 1.0, 0.5,
                            int(pittore::compute::ToneOp::Dodge), 1, false, false));
        CHECK_NEAR(state.activeLayer()->pixels->data()[mid].r, oneDab, 1e-5f);
        state.commitUndoStep(QStringLiteral("Dodge"), QStringLiteral("dodge"));
        state.endToneStroke();
    }

    // ---- sampleCompositeColor reads the composited colour ----------------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("sample"), QSize(20, 20), 300);
        primeLayer(state, QSize(20, 20), QColor(64, 32, 16));
        const QColor c = sampleCompositeColor(*d, QPointF(10, 10), 0);
        CHECK(c.isValid());
        CHECK(c.red() > 40 && c.red() < 90);
        CHECK(c.blue() < 50);
        CHECK(!sampleCompositeColor(*d, QPointF(-5, -5), 0).isValid());
    }

    // ---- Annotations are undoable and drive the options readouts ---------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("annot"), QSize(64, 48), 300);
        state.setActiveTool(ToolId::Count);
        state.setOption(ToolId::Count, QStringLiteral("group"), 1);

        state.beginUndoStep();
        d->colorSamples.append(QPointF(10, 10));
        d->notes.append(DocNote{QPointF(20, 20), QStringLiteral("hi"),
                                QStringLiteral("me"), QColor(255, 210, 40)});
        CountMarker m;
        m.pos = QPointF(30, 30);
        m.group = 1;
        d->countMarkers.append(m);
        d->rulerStart = QPointF(0, 0);
        d->rulerEnd = QPointF(30, 40);
        d->rulerHasMeasurement = true;
        state.markAnnotationsChanged();
        state.commitUndoStep(QStringLiteral("Annot"), QStringLiteral("note"));

        CHECK_EQ(state.option(ToolId::Count, QStringLiteral("count")).toInt(), 1);
        CHECK_NEAR(state.option(ToolId::Ruler, QStringLiteral("l2")).toDouble(), 50.0,
                   1e-4);
        CHECK_NEAR(state.option(ToolId::Ruler, QStringLiteral("a")).toDouble(),
                   std::atan2(40.0, 30.0) * 180.0 / 3.14159265358979323846, 1e-4);

        state.undo();
        CHECK(d->countMarkers.isEmpty());
        CHECK(d->notes.isEmpty());
        CHECK(d->colorSamples.isEmpty());
        CHECK(!d->rulerHasMeasurement);
    }

    // ---- Rotate Layer bakes a rotation and is undoable -------------------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("rot"), QSize(40, 20), 300);
        (void)d;
        primeLayer(state, QSize(40, 20), QColor(10, 200, 10));
        LayerItem* l = state.activeLayer();
        const std::uint32_t w0 = l->pixels->width();
        const std::uint32_t h0 = l->pixels->height();
        CHECK(state.rotateActiveLayer(90.0));
        l = state.activeLayer();
        CHECK_EQ(l->pixels->width(), h0);
        CHECK_EQ(l->pixels->height(), w0);
        state.undo();
        l = state.activeLayer();
        CHECK_EQ(l->pixels->width(), w0);
        CHECK_EQ(l->pixels->height(), h0);
    }

    // ---- Canvas input: sampler pins, count markers, ruler, rotate view ---
    {
        DocumentItem* d = state.addDocument(QStringLiteral("canvas"), QSize(200, 150), 300);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        app.processEvents();
        QWidget* vp = canvas->viewport();

        // Color Sampler: the fifth pin is refused, the first four stick.
        state.setActiveTool(ToolId::ColorSampler);
        app.processEvents();
        for (int i = 0; i < 5; ++i) {
            const QPointF vv =
                canvas->documentToView(QPointF(20 + i * 20, 20 + i * 10));
            sendMouse(vp, QEvent::MouseButtonPress, vv, Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, vv, Qt::LeftButton, Qt::NoButton);
        }
        app.processEvents();
        CHECK_EQ(d->colorSamples.size(), 4);

        // Count: three clicks add three numbered markers; the readout agrees.
        state.setActiveTool(ToolId::Count);
        app.processEvents();
        for (int i = 0; i < 3; ++i) {
            const QPointF vv = canvas->documentToView(QPointF(30 + i * 30, 40));
            sendMouse(vp, QEvent::MouseButtonPress, vv, Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, vv, Qt::LeftButton, Qt::NoButton);
        }
        app.processEvents();
        CHECK_EQ(d->countMarkers.size(), 3);
        CHECK_EQ(state.option(ToolId::Count, QStringLiteral("count")).toInt(), 3);

        // Ruler: press-drag-release records the measurement.
        state.setActiveTool(ToolId::Ruler);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(10, 10));
        const QPointF b = canvas->documentToView(QPointF(60, 10));
        sendMouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
        app.processEvents();
        CHECK(d->rulerHasMeasurement);
        CHECK_NEAR(d->rulerStart.x(), 10.0, 1.5);
        CHECK_NEAR(d->rulerEnd.x(), 60.0, 1.5);

        // Rotate View: a quarter turn of the pointer about the viewport centre
        // rotates the document by that bearing.
        state.setActiveTool(ToolId::RotateView);
        app.processEvents();
        canvas->resetRotation();
        const QPointF center(vp->width() / 2.0, vp->height() / 2.0);
        sendMouse(vp, QEvent::MouseButtonPress, center + QPointF(120, 0),
                  Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseMove, center + QPointF(0, 120), Qt::NoButton,
                  Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, center + QPointF(0, 120),
                  Qt::LeftButton, Qt::NoButton);
        app.processEvents();
        CHECK_NEAR(d->rotation, 90.0, 3.0);
        canvas->resetRotation();
        CHECK_NEAR(d->rotation, 0.0, 1e-6);
    }

    // ---- Replace Color Alt+click: locks the area palette, no stroke -----
    {
        DocumentItem* d = state.addDocument(QStringLiteral("replacepick"), QSize(64, 64), 300);
        CHECK(d != nullptr);
        primeLayer(state, QSize(64, 64), QColor(200, 30, 30));
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->setZoom(1.0);   // doc px == view px, so the click lands exact
        app.processEvents();
        QWidget* vp = canvas->viewport();

        state.setActiveTool(ToolId::ColorReplacement);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::ColorReplacement, QStringLiteral("sample_size"), 0);
        app.processEvents();
        const int depth = state.undoDepth();
        const QPointF vv = canvas->documentToView(QPointF(32, 32));
        sendMouse(vp, QEvent::MouseButtonPress, vv, Qt::LeftButton,
                  Qt::LeftButton, Qt::AltModifier);
        sendMouse(vp, QEvent::MouseButtonRelease, vv, Qt::LeftButton,
                  Qt::NoButton, Qt::AltModifier);
        app.processEvents();
        // The uniform red disc locks exactly one colour...
        CHECK(state.statusHint().contains(QStringLiteral("locked 1")));
        // ...the foreground is untouched (it comes from the picker)...
        CHECK_EQ(state.foreground().red(), 0);
        // ...and no stroke began: no undo step, pixels untouched.
        CHECK_EQ(state.undoDepth(), depth);
        CHECK_NEAR(state.activeLayer()->pixels->data()[0].r, 200.0 / 255.0, 0.02);
    }

    // ---- Replace Color Lock Area button: same lock without Alt -----------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("replacelock"), QSize(64, 64), 300);
        CHECK(d != nullptr);
        primeLayer(state, QSize(64, 64), QColor(20, 200, 20));
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->setZoom(1.0);
        app.processEvents();
        QWidget* vp = canvas->viewport();

        state.setActiveTool(ToolId::ColorReplacement);
        app.processEvents();
        // Hover establishes the cursor point; the button locks under it.
        sendMouse(vp, QEvent::MouseMove, canvas->documentToView(QPointF(32, 32)),
                  Qt::NoButton, Qt::NoButton);
        app.processEvents();
        const int depth = state.undoDepth();
        canvas->lockReplacePalette();
        app.processEvents();
        CHECK(state.statusHint().contains(QStringLiteral("locked 1")));
        CHECK_EQ(state.undoDepth(), depth);
        CHECK_NEAR(state.activeLayer()->pixels->data()[0].g, 200.0 / 255.0, 0.02);
        // Leave pops the hover cursor override so later blocks start clean.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(vp, &leave);
        app.processEvents();
    }

    // ---- Spot Healing click: one dab heals, one undo step ----------------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("spotclick"), QSize(64, 64), 300);
        CHECK(d != nullptr);
        primeLayer(state, QSize(64, 64), QColor(200, 30, 30));
        // Poke a dark speck straight into the pixels.
        {
            LayerItem* l = state.activeLayer();
            CHECK(l && l->pixels);
            if (l && l->pixels) l->pixels->at(32, 32) = {200.0f / 255, 0, 0, 1};
        }
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->setZoom(1.0);
        app.processEvents();
        QWidget* vp = canvas->viewport();

        state.setActiveTool(ToolId::SpotHealing);
        state.setOption(ToolId::SpotHealing, QStringLiteral("type"), 2);
        state.setOption(ToolId::SpotHealing, QStringLiteral("diffusion"), 5);
        state.setOption(ToolId::SpotHealing, QStringLiteral("brush_size"), 24);
        app.processEvents();
        const int depth = state.undoDepth();
        const QPointF vv = canvas->documentToView(QPointF(32, 32));
        sendMouse(vp, QEvent::MouseButtonPress, vv, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vp, QEvent::MouseButtonRelease, vv, Qt::LeftButton, Qt::NoButton);
        app.processEvents();
        // Speck healed toward the field colour in exactly one undo step.
        CHECK_EQ(state.undoDepth(), depth + 1);
        CHECK(state.activeLayer()->pixels->at(32, 32).r > 0.6f);
        // Leave pops the hover cursor override so later blocks start clean.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(vp, &leave);
        app.processEvents();
    }

    // ---- Clone Stamp: Alt+click pins the source, strokes copy it --------
    {
        DocumentItem* d = state.addDocument(QStringLiteral("clonepick"), QSize(64, 64), 300);
        CHECK(d != nullptr);
        primeLayer(state, QSize(64, 64), QColor(200, 30, 30));
        // A blue source square to steal from.
        {
            LayerItem* l = state.activeLayer();
            CHECK(l && l->pixels);
            if (l && l->pixels)
                for (int y = 10; y <= 13; ++y)
                    for (int x = 10; x <= 13; ++x)
                        l->pixels->at(x, y) = {0, 0, 1, 1};
        }
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->setZoom(1.0);
        app.processEvents();
        QWidget* vp = canvas->viewport();

        state.setActiveTool(ToolId::CloneStamp);
        state.setOption(ToolId::CloneStamp, QStringLiteral("brush_size"), 24);
        app.processEvents();

        // Without a source the stroke is refused before any undo step.
        {
            const int depth = state.undoDepth();
            const QPointF vv = canvas->documentToView(QPointF(40, 40));
            sendMouse(vp, QEvent::MouseButtonPress, vv, Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, vv, Qt::LeftButton, Qt::NoButton);
            app.processEvents();
            CHECK_EQ(state.undoDepth(), depth);
            CHECK(state.statusHint().contains(QStringLiteral("Alt-click")));
        }

        // Alt+click the blue square, then paint: blue lands at the cursor.
        {
            const QPointF src = canvas->documentToView(QPointF(11, 11));
            sendMouse(vp, QEvent::MouseButtonPress, src, Qt::LeftButton,
                      Qt::LeftButton, Qt::AltModifier);
            sendMouse(vp, QEvent::MouseButtonRelease, src, Qt::LeftButton,
                      Qt::NoButton, Qt::AltModifier);
            app.processEvents();
            // Issue #8: the Alt-pinned source shows a tracking cross: it
            // is visible, sits on the anchor before the first stroke, and
            // the cursor dirty rect covers it (paint ⊆ invalidated area).
            CHECK(canvas->cloneSourceVisible());
            const QPointF srcDoc = canvas->cloneSourceDoc();
            CHECK((srcDoc - QPointF(11, 11)).manhattanLength() < 0.05);
            CHECK(canvas->brushCursorViewRect().contains(
                canvas->documentToView(QPointF(11, 11))));
            const int depth = state.undoDepth();
            const QPointF vv = canvas->documentToView(QPointF(40, 40));
            sendMouse(vp, QEvent::MouseButtonPress, vv, Qt::LeftButton, Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, vv, Qt::LeftButton, Qt::NoButton);
            app.processEvents();
            CHECK_EQ(state.undoDepth(), depth + 1);
            CHECK(state.activeLayer()->pixels->at(40, 40).b > 0.5f);
        }
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(vp, &leave);
        app.processEvents();
    }

    // ---- Brush cursor: brush tools draw a size ring under the pointer ----
    {
        state.addDocument(QStringLiteral("brushcursor"), QSize(200, 150), 300);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->setZoom(1.0);   // doc px == view px, so the ring is 1:1 with size
        app.processEvents();
        QWidget* vp = canvas->viewport();

        // Prime the layer solid mid-grey so the ring's white/black pen stands
        // out from the document pixels.
        primeLayer(state, QSize(200, 150), QColor(0x80, 0x80, 0x80));
        app.processEvents();

        state.setActiveTool(ToolId::Brush);
        // The registry default brush size is 64 for every dab tool — checked
        // before any explicit setOption, so the ring is size 64 out of the box.
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_size")).toDouble(),
                   64.0, 0.01);
        CHECK_NEAR(state.option(ToolId::Eraser, QStringLiteral("brush_size")).toDouble(),
                   64.0, 0.01);
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40.0);
        app.processEvents();

        const QPointF cDoc(100, 75);
        // Number of pixels inside `half` of the cursor whose colour is far
        // from the mid-grey document: ring pen pixels vs. plain canvas.
        auto ringPixels = [&](int half) {
            const QImage img = window.grab().toImage();
            // The viewport sits at the ruler margin inside the scroll area.
            const QPointF c = canvas->documentToView(cDoc) + QPointF(18, 18);
            int painted = 0;
            for (int y = int(c.y()) - half; y <= int(c.y()) + half; ++y)
                for (int x = int(c.x()) - half; x <= int(c.x()) + half; ++x) {
                    const QColor px = img.pixelColor(x, y);
                    const bool inRange = x >= 0 && y >= 0 && x < img.width() && y < img.height();
                    if (inRange && qAbs(px.red() - 0x80) > 60 &&
                        qAbs(px.green() - 0x80) > 60 && qAbs(px.blue() - 0x80) > 60)
                        ++painted;
                }
            return painted;
        };

        // Hover the doc centre with the Brush tool at size 40: a ring of
        // radius 20 outlines the cursor.
        sendMouse(vp, QEvent::MouseMove, canvas->documentToView(cDoc),
                  Qt::NoButton, Qt::NoButton);
        app.processEvents();
        const int ring = ringPixels(22);
        CHECK(ring > 60);   // a 40 px ring paints well over a hundred outline px

        // The ring's centre is the brush's landing point and must be clear:
        // the ring is the ONLY thing drawn at the cursor — no crosshair.
        CHECK(ringPixels(4) == 0);

        // The ring really is the brush size: every painted pixel in the box hugs
        // the radius-20 outline (brush 40 / 2 at zoom 1) — nothing inside it
        // (no crosshair centre) and nothing outside it (no arms).
        {
            const QImage img = window.grab().toImage();
            const QPointF c = canvas->documentToView(cDoc) + QPointF(18, 18);
            double minD = 1e9, maxD = -1e9;
            int painted = 0;
            for (int y = int(c.y()) - 22; y <= int(c.y()) + 22; ++y)
                for (int x = int(c.x()) - 22; x <= int(c.x()) + 22; ++x)
                    if (x >= 0 && y >= 0 && x < img.width() && y < img.height()) {
                        const QColor px = img.pixelColor(x, y);
                        if (qAbs(px.red() - 0x80) > 60 && qAbs(px.green() - 0x80) > 60 &&
                            qAbs(px.blue() - 0x80) > 60) {
                            ++painted;
                            const double d = std::hypot(x - c.x(), y - c.y());
                            minD = std::min(minD, d);
                            maxD = std::max(maxD, d);
                        }
                    }
            CHECK(painted > 0);
            CHECK(minD >= 14.0 && maxD <= 25.0);
        }

        // While a brush tool hovers the canvas the OS cursor is hidden
        // app-wide (the app owns the cursor, not the widget); leaving the
        // canvas or switching tools pops the override.
        CHECK(app.overrideCursor() != nullptr &&
              app.overrideCursor()->shape() == Qt::BlankCursor);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(vp, &leave);
        app.processEvents();
        CHECK(app.overrideCursor() == nullptr);
        sendMouse(vp, QEvent::MouseMove, canvas->documentToView(cDoc),
                  Qt::NoButton, Qt::NoButton);
        app.processEvents();
        CHECK(app.overrideCursor() != nullptr &&
              app.overrideCursor()->shape() == Qt::BlankCursor);

        // A non-brush tool shows no ring on the same hover, and the app-wide
        // blank cursor is popped so the tool's own cursor returns.
        state.setActiveTool(ToolId::Move);
        app.processEvents();
        sendMouse(vp, QEvent::MouseMove, canvas->documentToView(cDoc),
                  Qt::NoButton, Qt::NoButton);
        app.processEvents();
        CHECK(ringPixels(22) == 0);
        CHECK(app.overrideCursor() == nullptr);

        // And back to a brush tool: the override returns on the next hover.
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        sendMouse(vp, QEvent::MouseMove, canvas->documentToView(cDoc),
                  Qt::NoButton, Qt::NoButton);
        app.processEvents();
        CHECK(app.overrideCursor() != nullptr &&
              app.overrideCursor()->shape() == Qt::BlankCursor);
    }

    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return pittore_test::failures() == 0 ? 0 : 1;
}
