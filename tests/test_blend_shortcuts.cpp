// test_blend_shortcuts.cpp — the Alt+Shift+<letter> Photoshop-style family
// through a real MainWindow (headless): Alt+Shift+G toggles the active
// layer's clipping mask, Alt+Shift+M/S pick the painting tool's "Mode" option
// or, with the Move tool, the active layer's blend mode. Wrong modifier sets
// never fire, and a Multiply/Screen stroke folds the blend into the layer for
// both the wash and the direct build-up paths.
// Runs offscreen; the meson XDG_CONFIG_HOME sandbox keeps the user keymap
// and settings out of the way.
#include <cstdio>
#include <cstddef>
#include <memory>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QKeyEvent>
#include <QSize>

#include "engine/core/log.h"
#include "engine/core/pixel.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/main_window.h"
#include "ui/tools/options/tool_options.h"

using namespace pittore::ui;
using pittore::RGBAf;

namespace {

// Index of `name` in `tool`'s declared Mode combo; -1 when it has none.
int modeIndex(ToolId tool, const char* name) {
    for (const OptionSpec& spec : optionsFor(tool)) {
        if (QString::fromUtf8(spec.id) != QStringLiteral("mode")) continue;
        for (std::size_t i = 0; i < spec.items.size(); ++i)
            if (QString::fromUtf8(spec.items[i]) == QLatin1String(name))
                return int(i);
    }
    return -1;
}

void sendKey(QWidget* target, int key, Qt::KeyboardModifiers mods,
             const QString& text) {
    QKeyEvent e(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(target, &e);
}

void pump(int rounds = 3) {
    for (int i = 0; i < rounds; ++i) qApp->processEvents();
}

// Two stacked 16x16 pixel layers on a 16x16 document (layer size == document
// size, so the live wash preview folds into the composite too).
DocumentItem* makeDoc(AppState& state, RGBAf fill) {
    DocumentItem* d =
        state.addDocument(QStringLiteral("keys"), QSize(16, 16), 300);
    if (!d) return nullptr;
    d->layers.clear();
    for (int i = 0; i < 2; ++i) {
        LayerItem l;
        l.name = QStringLiteral("Layer %1").arg(i);
        l.kind = LayerItem::Kind::Pixel;
        l.pixels = std::make_shared<pittore::Image>(16, 16);
        l.pixels->fill(fill);
        l.offset = QPointF(0, 0);
        l.scaleX = l.scaleY = 1.0;
        d->layers.append(std::move(l));
    }
    d->activeLayer = 1;
    d->selectedLayers = QVector<int>{1};
    d->rebuildComposite();
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    const QString scratch =
        QDir::tempPath() + QStringLiteral("/pittore-test-blend-keys");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);
    qputenv("XDG_CONFIG_HOME", scratch.toLocal8Bit());
    QApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-blend-keys-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    AppState state;
    DocumentItem* d = makeDoc(state, RGBAf{0.25f, 0.25f, 0.25f, 1.0f});
    CHECK(d != nullptr);
    if (!d) return 1;
    MainWindow win(&state);
    win.show();
    pump();

    const Qt::KeyboardModifiers altShift = Qt::AltModifier | Qt::ShiftModifier;

    // --- Alt+Shift+G: clipping mask toggle on the active layer, undoable ----
    {
        CHECK(!d->layers[1].clipped);
        sendKey(&win, Qt::Key_G, altShift, QStringLiteral("G"));
        pump();
        CHECK(d->layers[1].clipped);
        state.undo();
        pump();
        CHECK(!d->layers[1].clipped);
    }

    // --- Painting tool: the letters pick the tool's Mode option -------------
    {
        state.setActiveTool(ToolId::Brush);
        pump();
        const int mul = modeIndex(ToolId::Brush, "Multiply");
        const int scr = modeIndex(ToolId::Brush, "Screen");
        CHECK(mul > 0);
        CHECK(scr > 0);
        sendKey(&win, Qt::Key_M, altShift, QStringLiteral("M"));
        pump();
        CHECK(state.option(ToolId::Brush, QStringLiteral("mode")).toInt() == mul);
        CHECK(state.paintBlendFor(ToolId::Brush) ==
              pittore::compute::BlendMode::Multiply);
        sendKey(&win, Qt::Key_S, altShift, QStringLiteral("S"));
        pump();
        CHECK(state.option(ToolId::Brush, QStringLiteral("mode")).toInt() == scr);
        CHECK(state.paintBlendFor(ToolId::Brush) ==
              pittore::compute::BlendMode::Screen);

        // Strict modifiers: a missing or extra one never fires the binding.
        sendKey(&win, Qt::Key_M, Qt::AltModifier, QStringLiteral("m"));
        pump();
        CHECK(state.option(ToolId::Brush, QStringLiteral("mode")).toInt() == scr);
        sendKey(&win, Qt::Key_M,
                Qt::AltModifier | Qt::ShiftModifier | Qt::ControlModifier,
                QStringLiteral("M"));
        pump();
        CHECK(state.option(ToolId::Brush, QStringLiteral("mode")).toInt() == scr);
    }

    // --- Non-painting tool: consumed with a hint, nothing changes -----------
    {
        state.setActiveTool(ToolId::Hand);
        pump();
        const int before =
            state.option(ToolId::Brush, QStringLiteral("mode")).toInt();
        sendKey(&win, Qt::Key_M, altShift, QStringLiteral("M"));
        pump();
        CHECK(state.option(ToolId::Brush, QStringLiteral("mode")).toInt() ==
              before);
        CHECK(state.statusHint() == QStringLiteral(
              "Blend-mode shortcuts need a painting tool or the Move tool."));
    }

    // --- Move tool: the letters set the active layer's blend mode -----------
    {
        state.setActiveTool(ToolId::Move);
        pump();
        CHECK(d->layers[1].blendMode == QStringLiteral("Normal"));
        sendKey(&win, Qt::Key_S, altShift, QStringLiteral("S"));
        pump();
        CHECK(d->layers[1].blendMode == QStringLiteral("Screen"));
        CHECK(state.statusHint() == QStringLiteral("Layer blend mode: Screen"));
        state.undo();
        pump();
        CHECK(d->layers[1].blendMode == QStringLiteral("Normal"));
    }

    // --- Non-blend "mode" ids never leak into the dab path ------------------
    // Color Replacement reuses the id for its recolor modes
    // (Hue/Saturation/Color/Luminosity); its default "Color" must not turn
    // paint dabs into the HSL colour blend mode.
    {
        state.setActiveTool(ToolId::ColorReplacement);
        pump();
        CHECK(state.paintBlendFor(ToolId::ColorReplacement) ==
              pittore::compute::BlendMode::Normal);
        CHECK(!state.setToolBlendMode(ToolId::ColorReplacement,
                                      QStringLiteral("Multiply")));
        const int recolor =
            state.option(ToolId::ColorReplacement, QStringLiteral("mode")).toInt();
        sendKey(&win, Qt::Key_M, altShift, QStringLiteral("M"));
        pump();
        CHECK(state.option(ToolId::ColorReplacement, QStringLiteral("mode")).toInt() ==
              recolor);
        CHECK(state.statusHint() == QStringLiteral(
              "Blend-mode shortcuts need a painting tool or the Move tool."));
        // A plain dab over white leaves the source colour, unblended.
        d->layers[1].pixels->fill(RGBAf{1, 1, 1, 1});
        d->rebuildComposite();
        state.beginStrokeState(ToolId::ColorReplacement, 5);
        CHECK(state.paintDab(QPointF(8, 8), 4.0, 1.0, 1.0, QColor(0, 255, 0)));
        state.endStrokeState();
        const RGBAf dabbed = d->layers[1].pixels->at(8, 8);
        CHECK_NEAR(dabbed.r, 0.0f, 1e-3f);
        CHECK_NEAR(dabbed.g, 1.0f, 1e-3f);
        CHECK_NEAR(dabbed.b, 0.0f, 1e-3f);
    }

    // --- Wash stroke: the blend folds once at bake time ---------------------
    {
        state.setActiveTool(ToolId::Brush);
        CHECK(state.setToolBlendMode(ToolId::Brush, QStringLiteral("Multiply")));
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_painting_mode"),
                        QStringLiteral("wash"));
        d->layers[1].pixels->fill(RGBAf{0.25f, 0.25f, 0.25f, 1.0f});
        d->rebuildComposite();
        state.beginStrokeState(ToolId::Brush, 4242);
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        CHECK(state.paintDab(QPointF(8, 8), 4.0, 1.0, 1.0,
                             QColor(128, 128, 128)));
        // The live preview folds the scratch into the composite with the same
        // blend before release.
        state.flushPaint();
        const QColor preview = d->composite.pixelColor(8, 8);
        CHECK(preview.red() >= 30 && preview.red() <= 34);  // ~0.125 * 255
        CHECK(state.bakeWashStroke());
        state.endStrokeState();
        state.commitUndoStep(QStringLiteral("Brush"), QStringLiteral("brush"));
        const RGBAf painted = d->layers[1].pixels->at(8, 8);
        CHECK_NEAR(painted.r, 0.25f * (128.0f / 255.0f), 2e-3f);  // multiplied
        CHECK_NEAR(painted.g, painted.r, 1e-5f);
        CHECK_NEAR(painted.a, 1.0f, 1e-3f);
        state.undo();
        pump();
        CHECK_NEAR(d->layers[1].pixels->at(8, 8).r, 0.25f, 1e-3f);
    }

    // --- Build-up stroke: the blend applies per dab -------------------------
    {
        state.setActiveTool(ToolId::Brush);
        CHECK(state.setToolBlendMode(ToolId::Brush, QStringLiteral("Screen")));
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_painting_mode"),
                        QStringLiteral("buildup"));
        d->layers[1].pixels->fill(RGBAf{0.25f, 0.25f, 0.25f, 1.0f});
        d->rebuildComposite();
        state.beginStrokeState(ToolId::Brush, 77);
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        CHECK(state.paintDab(QPointF(8, 8), 4.0, 1.0, 1.0,
                             QColor(128, 128, 128)));
        state.bakeWashStroke();  // canvas release calls it unconditionally
        state.endStrokeState();
        state.commitUndoStep(QStringLiteral("Brush"), QStringLiteral("brush"));
        // screen(0.25, 0.50196) = 0.25 + 0.50196 - 0.25 * 0.50196
        const RGBAf painted = d->layers[1].pixels->at(8, 8);
        CHECK_NEAR(painted.r, 0.62647f, 3e-3f);
        CHECK_NEAR(painted.a, 1.0f, 1e-3f);
        state.undo();
        pump();
        CHECK_NEAR(d->layers[1].pixels->at(8, 8).r, 0.25f, 1e-3f);
    }

    if (pittore_test::failures() == 0)
        std::printf("test_blend_shortcuts: all checks passed\n");
    else
        std::printf("test_blend_shortcuts: %d failures\n",
                    pittore_test::failures());
    return pittore_test::failures() ? 1 : 0;
}
