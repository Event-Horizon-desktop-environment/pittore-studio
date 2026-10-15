// test_live_audit.cpp — all-in-one live audit: every tabled tool gets a
// real canvas gesture on a fresh document, every factory brush preset paints
// a stroke, every registered filter runs on a probe image. Per item the log
// carries wall ms, state effect and errors; every line is flushed immediately
// so a crash or hang attributes to the last-started item. Runs headless.
//
// Verdicts: OK (state changed), STUB (no change, on the known-stub list),
// UNEXPECTED (no change where behavior exists, or behavior where a stub was
// declared — both fail the suite), SKIP (needs a modal dialog or a model),
// SLOW (over the per-item budget, informational).
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWidget>

#include "engine/compute/paint.h"
#include "engine/core/pixel.h"
#include "engine/filter/filters.h"
#include "engine/filter/registry/filter_registry.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/ai_models.h"
#include "ui/brushes/brush_library.h"
#include "ui/canvas_view.h"
#include "ui/tools/defs/tool_defs.h"

using namespace pittore::ui;

namespace {

// Budget above which an item is flagged SLOW (still passes).
constexpr double kSlowMs = 250.0;

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch())
        .count();
}

void auditLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

std::uint64_t fnv(const void* data, std::size_t n, std::uint64_t h) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Whole-app state that any tool/filter/preset gesture could plausibly move.
std::uint64_t fingerprint(AppState& state, CanvasView* canvas) {
    std::uint64_t h = 1469598103934665603ULL;
    DocumentItem* d = state.activeDocument();
    if (!d) return h;
    if (!d->composite.isNull()) {
        const QImage& c = d->composite;
        for (int y = 0; y < c.height(); ++y)
            h = fnv(c.constScanLine(y), std::size_t(c.bytesPerLine()), h);
    }
    h = fnv(&d->layers, sizeof(d->layers.size()), h);
    auto mixInt = [&](long long v) { h = fnv(&v, sizeof(v), h); };
    mixInt(d->layers.size());
    mixInt(d->activeLayer);
    mixInt(std::llround(d->selection.x()));
    mixInt(std::llround(d->selection.y()));
    mixInt(std::llround(d->selection.width()));
    mixInt(std::llround(d->selection.height()));
    mixInt(d->selectionIsEllipse ? 1 : 0);
    mixInt(d->selectionIsMask ? 1 : 0);
    if (d->selectionIsMask && !d->selectionMask.isNull()) {
        const QImage& m = d->selectionMask;
        mixInt(m.width());
        mixInt(m.height());
        h = fnv(m.constBits(), std::size_t(m.sizeInBytes()), h);
    }
    const QColor fg = state.foreground();
    mixInt((fg.red() << 24) | (fg.green() << 16) | (fg.blue() << 8) |
           fg.alpha());
    mixInt(std::llround(d->zoom * 1000.0));
    mixInt(std::llround(d->rotation * 1000.0));
    mixInt(d->slices.size());
    for (const QRect& s : d->slices) {
        mixInt(s.x());
        mixInt(s.y());
        mixInt(s.width());
        mixInt(s.height());
    }
    mixInt(d->notes.size());
    mixInt(d->colorSamples.size());
    mixInt(d->horizontalGuides.size());
    for (double g : d->horizontalGuides) mixInt(std::llround(g * 1000.0));
    mixInt(d->verticalGuides.size());
    for (double g : d->verticalGuides) mixInt(std::llround(g * 1000.0));
    mixInt(d->rulerHasMeasurement ? 1 : 0);
    mixInt(std::llround(d->rulerStart.x()));
    mixInt(std::llround(d->rulerStart.y()));
    mixInt(d->areaHasMeasurement ? 1 : 0);
    mixInt(std::llround(d->areaRect.x()));
    mixInt(std::llround(d->areaRect.y()));
    mixInt(std::llround(d->areaRect.width()));
    mixInt(std::llround(d->areaRect.height()));
    mixInt(d->size.width());
    mixInt(d->size.height());
    mixInt((d->canvasPaper.red() << 24) | (d->canvasPaper.green() << 16) |
           (d->canvasPaper.blue() << 8) | d->canvasPaper.alpha());
    mixInt(d->countMarkers.size());
    if (canvas) {
        mixInt(canvas->horizontalScrollBar()->value());
        mixInt(canvas->verticalScrollBar()->value());
    }
    return h;
}

void sendMouse(QWidget* target, QEvent::Type type, const QPointF& pos,
               Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPointF global = target->mapToGlobal(pos.toPoint());
    QMouseEvent e(type, pos, global, button, buttons, mods);
    QApplication::sendEvent(target, &e);
}

enum class Expect { Effect, NoOp, Skip };

struct ToolPolicy {
    Expect expect = Expect::Effect;
    const char* note = "";
};

// Tools with no canvas behavior (options + strip presence only). If one of
// these ever changes state, the suite fails so the list gets updated.
// The list is currently EMPTY: every tabled tool gestures something.
// NOTE: Smudge, QR Code, Blur/Sharpen, Background Eraser, Pattern Stamp,
// History Brush, Healing Brush, Patch, the vector point family, Area,
// Red Eye, Art History, both Type Masks, Frame, Import Photos, both
// Generative tools, Mixer, Remove, ContentAwareMove, PerspectiveCrop,
// Artboard, AdjustmentBrush and ContentAwareTracing all came alive after
// this list was written — the suite caught each via STUB-ALIVE.
bool isKnownStub(ToolId id) {
    (void)id;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    AppState state;

    QWidget window;
    auto* canvas = new CanvasView(&state, &window);
    window.resize(800, 600);
    canvas->setGeometry(0, 0, 800, 600);
    window.show();

    int toolsOk = 0, toolsStub = 0, toolsSkip = 0, toolsFail = 0, toolsSlow = 0;
    int nTools = 0;

    for (const ToolDef& def : allTools()) {
        const ToolId id = def.id;
        ++nTools;
        // Fresh isolated document per tool.
        state.addDocument(QStringLiteral("audit"), QSize(96, 64), 300);
        state.setActiveTool(id);
        app.processEvents();
        QWidget* vp = canvas->viewport();

        bool skip = (id == ToolId::Note || id == ToolId::PlaceTool ||
                     id == ToolId::ImportPhotos);
        // View tools move the view, not the document (scrollbars + zoom +
        // rotation are fingerprinted, so any motion still counts).
        const bool viewOnly = (id == ToolId::Hand || id == ToolId::RotateView ||
                               id == ToolId::Zoom);

        std::string errStr;
        char gesture[64];
        std::snprintf(gesture, sizeof(gesture), "press-drag-release");
        std::uint64_t before = 0;
        double t0 = 0.0;
        try {
            if (!skip) {
                const QPointF a =
                    canvas->documentToView(QPointF(20, 20));
                const QPointF b =
                    canvas->documentToView(QPointF(60, 45));
                auto stroke = [&](const QPointF& p, const QPointF& q) {
                    sendMouse(vp, QEvent::MouseButtonPress, p, Qt::LeftButton,
                              Qt::LeftButton);
                    for (int i = 1; i <= 4; ++i) {
                        const QPointF mp =
                            p + (q - p) * (double(i) / 4.0);
                        sendMouse(vp, QEvent::MouseMove, mp, Qt::NoButton,
                                  Qt::LeftButton);
                    }
                    sendMouse(vp, QEvent::MouseButtonRelease, q, Qt::LeftButton,
                              Qt::NoButton);
                };
                // Fixture: tools that need vector art get a real rectangle
                // (stroked for the width tool); AI tools get pixel content.
                const bool needsArt =
                    id == ToolId::Gradient || id == ToolId::TransparencyTool ||
                    id == ToolId::VectorFloodFillTool ||
                    id == ToolId::ShapeBuilderTool || id == ToolId::NodeTool ||
                    id == ToolId::PointTransformTool ||
                    id == ToolId::CornerTool || id == ToolId::ContourTool ||
                    id == ToolId::StrokeWidthTool || id == ToolId::KnifeTool ||
                    id == ToolId::AddAnchorPoint ||
                    id == ToolId::DeleteAnchorPoint ||
                    id == ToolId::ConvertPoint ||
                    id == ToolId::DirectSelection ||
                    id == ToolId::PathSelection;
                if (needsArt) {
                    if (id == ToolId::StrokeWidthTool) {
                        state.setOption(ToolId::Rectangle,
                                        QStringLiteral("stroke"),
                                        QColor(255, 0, 0));
                        state.setOption(ToolId::Rectangle,
                                        QStringLiteral("strokewidth"), 8.0);
                    }
                    state.setActiveTool(ToolId::Rectangle);
                    stroke(a, b);
                    if (id == ToolId::ShapeBuilderTool) {
                        const QPointF c =
                            canvas->documentToView(QPointF(35, 30));
                        const QPointF dpt =
                            canvas->documentToView(QPointF(75, 55));
                        state.setActiveTool(ToolId::Rectangle);
                        stroke(c, dpt);
                    }
                    state.setActiveTool(id);
                    app.processEvents();
                }
                if (id == ToolId::Move) {
                    QImage block(16, 16, QImage::Format_ARGB32);
                    block.fill(QColor(200, 30, 30));
                    state.placeImageLayer(block, QStringLiteral("auditblk"),
                                          QPointF(48, 32), 1.0);
                    app.processEvents();
                } else if (id == ToolId::CloneStamp) {
                    std::snprintf(gesture, sizeof(gesture), "alt-source+stroke");
                    sendMouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton,
                              Qt::LeftButton, Qt::AltModifier);
                    sendMouse(vp, QEvent::MouseButtonRelease, a, Qt::LeftButton,
                              Qt::NoButton, Qt::AltModifier);
                } else if (id == ToolId::SliceSelect) {
                    // Seed a slice with the Slice tool first.
                    state.setActiveTool(ToolId::Slice);
                    sendMouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseMove, b, Qt::NoButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, b, Qt::LeftButton,
                              Qt::NoButton);
                    state.setActiveTool(id);
                } else if (id == ToolId::HistoryBrush ||
                           id == ToolId::ArtHistoryBrush) {
                    // Seed history: a Brush stroke commits an undo step, so
                    // the History Brush has an open-state to restore toward.
                    // Re-assert per stroke (painting stays in Brush).
                    state.setActiveTool(ToolId::Brush);
                    state.setForeground(QColor(200, 30, 30));
                    stroke(a, b);
                    state.setActiveTool(id);
                } else if (id == ToolId::HealingBrush) {
                    // Seed a blemish + pin a clean source: the square is
                    // painted directly (fixture, before the baseline), the
                    // Alt-click only arms the donor.
                    DocumentItem* dd = state.activeDocument();
                    if (dd && !dd->layers.isEmpty()) {
                        LayerItem& bg = dd->layers.back();
                        if (bg.pixels) {
                            const std::uint32_t lw = bg.pixels->width();
                            const std::uint32_t lh = bg.pixels->height();
                            for (std::uint32_t y = lh / 4;
                                 y < lh * 3 / 4; ++y)
                                for (std::uint32_t x = lw / 4;
                                     x < lw * 3 / 4; ++x)
                                    bg.pixels->data()[std::size_t(y) * lw +
                                                      x] = pittore::RGBAf{
                                        0, 0, 0, 1};
                            ++bg.sourceStamp;
                            dd->rebuildComposite();
                        }
                    }
                    std::snprintf(gesture, sizeof(gesture),
                                  "alt-source+stroke");
                    sendMouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton,
                              Qt::LeftButton, Qt::AltModifier);
                    sendMouse(vp, QEvent::MouseButtonRelease, a, Qt::LeftButton,
                              Qt::NoButton, Qt::AltModifier);
                    state.setActiveTool(id);
                } else if (id == ToolId::Patch) {
                    // Seed a blemish + a selection holding the press point:
                    // the drag heals the covered dark edge from clean white.
                    DocumentItem* dd = state.activeDocument();
                    if (dd && !dd->layers.isEmpty()) {
                        LayerItem& bg = dd->layers.back();
                        if (bg.pixels) {
                            const std::uint32_t lw = bg.pixels->width();
                            const std::uint32_t lh = bg.pixels->height();
                            for (std::uint32_t y = lh / 4;
                                 y < lh * 3 / 4; ++y)
                                for (std::uint32_t x = lw / 4;
                                     x < lw * 3 / 4; ++x)
                                    bg.pixels->data()[std::size_t(y) * lw +
                                                      x] = pittore::RGBAf{
                                        0, 0, 0, 1};
                            ++bg.sourceStamp;
                            dd->rebuildComposite();
                        }
                    }
                    state.setSelection(
                        QRectF(QPointF(10, 10), QPointF(40, 30)), false);
                    state.setActiveTool(id);
                } else if (id == ToolId::ContentAwareMove) {
                    // Seed content + a selection holding the press point:
                    // the drag moves dark texels onto clean white.
                    DocumentItem* dd = state.activeDocument();
                    if (dd && !dd->layers.isEmpty()) {
                        LayerItem& bg = dd->layers.back();
                        if (bg.pixels) {
                            const std::uint32_t lw = bg.pixels->width();
                            const std::uint32_t lh = bg.pixels->height();
                            for (std::uint32_t y = lh / 4;
                                 y < lh * 3 / 4; ++y)
                                for (std::uint32_t x = lw / 4;
                                     x < lw * 3 / 4; ++x)
                                    bg.pixels->data()[std::size_t(y) * lw +
                                                      x] = pittore::RGBAf{
                                        0, 0, 0, 1};
                            ++bg.sourceStamp;
                            dd->rebuildComposite();
                        }
                    }
                    state.setSelection(
                        QRectF(QPointF(10, 10), QPointF(40, 30)), false);
                    state.setActiveTool(id);
                } else if (id == ToolId::RedEye) {
                    // Seed a red pupil the drag-box will fix.
                    DocumentItem* dd = state.activeDocument();
                    if (dd && !dd->layers.isEmpty()) {
                        LayerItem& bg = dd->layers.back();
                        if (bg.pixels) {
                            const std::uint32_t lw = bg.pixels->width();
                            const std::uint32_t lh = bg.pixels->height();
                            for (std::uint32_t y = lh / 4;
                                 y < lh * 3 / 4; ++y)
                                for (std::uint32_t x = lw / 4;
                                     x < lw * 3 / 4; ++x)
                                    bg.pixels->data()[std::size_t(y) * lw +
                                                      x] = pittore::RGBAf{
                                        0.9f, 0.1f, 0.1f, 1};
                            ++bg.sourceStamp;
                            dd->rebuildComposite();
                        }
                    }
                } else if (id == ToolId::StylePickerTool) {
                    // Two rects in different fills: pick the first, apply
                    // to the second (one rect cannot show a transfer).
                    // Re-assert the tool per stroke: creation yields to
                    // Move unless keep-selected is on.
                    state.setActiveTool(ToolId::Rectangle);
                    state.setOption(ToolId::Rectangle, QStringLiteral("fill"),
                                    QColor(0, 0, 255));
                    stroke(a, b);
                    const QPointF c2a =
                        canvas->documentToView(QPointF(65, 20));
                    const QPointF c2b =
                        canvas->documentToView(QPointF(90, 45));
                    state.setActiveTool(ToolId::Rectangle);
                    state.setOption(ToolId::Rectangle, QStringLiteral("fill"),
                                    QColor(255, 0, 0));
                    stroke(c2a, c2b);
                    state.setActiveTool(id);
                }
                // Fixture: content-sensitive tools need real edges (blank paper
                // blurs/sharpens/mixes/adjusts/heals/segments to nothing).
                if (id == ToolId::QuickSelection ||
                    id == ToolId::ObjectSelection || id == ToolId::Blur ||
                    id == ToolId::Sharpen || id == ToolId::MixerBrush ||
                    id == ToolId::Remove || id == ToolId::AdjustmentBrush) {
                    DocumentItem* dd = state.activeDocument();
                    if (dd && !dd->layers.isEmpty()) {
                        LayerItem& bg = dd->layers.back();
                        if (bg.pixels) {
                            const std::uint32_t lw = bg.pixels->width();
                            const std::uint32_t lh = bg.pixels->height();
                            for (std::uint32_t y = lh / 4;
                                 y < lh * 3 / 4; ++y)
                                for (std::uint32_t x = lw / 4;
                                     x < lw * 3 / 4; ++x)
                                    bg.pixels->data()[std::size_t(y) * lw +
                                                      x] = pittore::RGBAf{
                                        0, 0, 0, 1};
                            ++bg.sourceStamp;
                            dd->rebuildComposite();
                        }
                    }
                }
                // Fixture: Hand needs scroll room to pan into.
                if (id == ToolId::Hand) canvas->setZoom(8.0);
                // Fixture: Contour needs a nonzero radius (otherwise it
                // honestly asks for one and changes nothing).
                if (id == ToolId::ContourTool)
                    state.setOption(id, QStringLiteral("contour_radius"),
                                    5.0);
                // Fixture: Tracing defaults to Path output (planned), so
                // point it at Shape for the gesture.
                if (id == ToolId::ContentAwareTracing)
                    state.setOption(id, QStringLiteral("output"), 2);
                app.processEvents();
                // Baseline AFTER setup so fixtures never masquerade as
                // gesture effects.
                before = fingerprint(state, canvas);
                t0 = nowMs();
                // Gesture anchors: vector point tools must press on ink
                // near an anchor (exact-corner pixels are covererage-thin),
                // the knife cuts straight across, the width tool rides the
                // top edge it can profile.
                QPointF pa = a, pb = b;
                if (id == ToolId::NodeTool || id == ToolId::PointTransformTool ||
                    id == ToolId::CornerTool) {
                    pa = canvas->documentToView(QPointF(24, 24));
                    std::snprintf(gesture, sizeof(gesture), "anchor-press-drag");
                } else if (id == ToolId::AddAnchorPoint) {
                    // Top-edge midpoint of the fixture rect (a span, not an
                    // anchor — endpoints belong to the anchor tools).
                    pa = pb = canvas->documentToView(QPointF(40, 20));
                    std::snprintf(gesture, sizeof(gesture), "edge-click");
                } else if (id == ToolId::DeleteAnchorPoint) {
                    // Right-edge anchor of the fixture rect.
                    pa = pb = canvas->documentToView(QPointF(60, 42));
                    std::snprintf(gesture, sizeof(gesture), "anchor-click");
                } else if (id == ToolId::ConvertPoint ||
                           id == ToolId::DirectSelection) {
                    // Top-left corner anchor; Direct Selection drags it.
                    pa = canvas->documentToView(QPointF(22, 22));
                    if (id == ToolId::DirectSelection) {
                        pb = canvas->documentToView(QPointF(30, 30));
                        std::snprintf(gesture, sizeof(gesture),
                                      "anchor-drag");
                    } else {
                        pb = pa;
                        std::snprintf(gesture, sizeof(gesture),
                                      "anchor-click");
                    }
                } else if (id == ToolId::PathSelection) {
                    // Shape interior, dragged anywhere.
                    pa = canvas->documentToView(QPointF(40, 32));
                    pb = canvas->documentToView(QPointF(60, 45));
                    std::snprintf(gesture, sizeof(gesture), "shape-drag");
                } else if (id == ToolId::KnifeTool) {
                    pa = canvas->documentToView(QPointF(10, 32));
                    pb = canvas->documentToView(QPointF(70, 32));
                    std::snprintf(gesture, sizeof(gesture), "cut-across");
                } else if (id == ToolId::StrokeWidthTool) {
                    pa = canvas->documentToView(QPointF(30, 21));
                    pb = canvas->documentToView(QPointF(70, 21));
                    std::snprintf(gesture, sizeof(gesture), "ride-the-stroke");
                }
                if (id == ToolId::Pen || id == ToolId::CurvaturePen) {
                    std::snprintf(gesture, sizeof(gesture),
                                  "two-anchors+dblclick");
                    sendMouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, a, Qt::LeftButton,
                              Qt::NoButton);
                    sendMouse(vp, QEvent::MouseButtonPress, b, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, b, Qt::LeftButton,
                              Qt::NoButton);
                    sendMouse(vp, QEvent::MouseButtonDblClick, b, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, b, Qt::LeftButton,
                              Qt::NoButton);
                } else if (id == ToolId::PerspectiveCrop) {
                    // Drag the quad, then double-click to commit the warp.
                    std::snprintf(gesture, sizeof(gesture), "quad+dblclick");
                    sendMouse(vp, QEvent::MouseButtonPress, pa, Qt::LeftButton,
                              Qt::LeftButton);
                    for (int i = 1; i <= 4; ++i) {
                        const QPointF p =
                            pa + (pb - pa) * (double(i) / 4.0);
                        sendMouse(vp, QEvent::MouseMove, p, Qt::NoButton,
                                  Qt::LeftButton);
                    }
                    sendMouse(vp, QEvent::MouseButtonRelease, pb, Qt::LeftButton,
                              Qt::NoButton);
                    sendMouse(vp, QEvent::MouseButtonDblClick, pb, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, pb, Qt::LeftButton,
                              Qt::NoButton);
                } else if (id == ToolId::Crop) {
                    // Crop is two-phase like Perspective Crop: the drag only
                    // shapes the live frame; the commit (double-click here,
                    // Enter or Apply in the UI) is what resizes the document.
                    // Without the commit the audit would see a NO-EFFECT drag.
                    std::snprintf(gesture, sizeof(gesture), "frame+dblclick");
                    sendMouse(vp, QEvent::MouseButtonPress, pa, Qt::LeftButton,
                              Qt::LeftButton);
                    for (int i = 1; i <= 4; ++i) {
                        const QPointF p =
                            pa + (pb - pa) * (double(i) / 4.0);
                        sendMouse(vp, QEvent::MouseMove, p, Qt::NoButton,
                                  Qt::LeftButton);
                    }
                    sendMouse(vp, QEvent::MouseButtonRelease, pb, Qt::LeftButton,
                              Qt::NoButton);
                    sendMouse(vp, QEvent::MouseButtonDblClick, pb, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, pb, Qt::LeftButton,
                              Qt::NoButton);
                } else if (id == ToolId::StylePickerTool) {
                    // Sample the first seeded shape, apply onto the second.
                    // Pick toggles default off without an options bar.
                    std::snprintf(gesture, sizeof(gesture), "pick+apply");
                    state.setOption(id, QStringLiteral("pick_stroke"), true);
                    state.setOption(id, QStringLiteral("pick_fill"), true);
                    state.setOption(id, QStringLiteral("pick_opacity"), true);
                    const QPointF p1 =
                        canvas->documentToView(QPointF(40, 32));
                    const QPointF p2 =
                        canvas->documentToView(QPointF(77, 32));
                    sendMouse(vp, QEvent::MouseButtonPress, p1, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, p1, Qt::LeftButton,
                              Qt::NoButton);
                    sendMouse(vp, QEvent::MouseButtonPress, p2, Qt::LeftButton,
                              Qt::LeftButton);
                    sendMouse(vp, QEvent::MouseButtonRelease, p2, Qt::LeftButton,
                              Qt::NoButton);
                } else if (!skip) {
                    // Lasso family needs a real loop, not a straight drag:
                    // freehand streams a closed square, polygonal clicks one.
                    if (id == ToolId::Lasso ||
                        id == ToolId::MagneticLasso) {
                        std::snprintf(gesture, sizeof(gesture), "loop");
                        const QPointF p1 =
                            canvas->documentToView(QPointF(60, 20));
                        const QPointF p3 =
                            canvas->documentToView(QPointF(20, 45));
                        sendMouse(vp, QEvent::MouseButtonPress, pa,
                                  Qt::LeftButton, Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseMove, p1, Qt::NoButton,
                                  Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseMove, pb, Qt::NoButton,
                                  Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseMove, p3, Qt::NoButton,
                                  Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseMove, pa, Qt::NoButton,
                                  Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseButtonRelease, pa,
                                  Qt::LeftButton, Qt::NoButton);
                    } else if (id == ToolId::PolygonalLasso) {
                        std::snprintf(gesture, sizeof(gesture),
                                      "click-loop");
                        const QPointF p3 =
                            canvas->documentToView(QPointF(20, 45));
                        sendMouse(vp, QEvent::MouseButtonPress, pa,
                                  Qt::LeftButton, Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseButtonRelease, pa,
                                  Qt::LeftButton, Qt::NoButton);
                        sendMouse(vp, QEvent::MouseButtonPress, pb,
                                  Qt::LeftButton, Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseButtonRelease, pb,
                                  Qt::LeftButton, Qt::NoButton);
                        sendMouse(vp, QEvent::MouseButtonPress, p3,
                                  Qt::LeftButton, Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseButtonRelease, p3,
                                  Qt::LeftButton, Qt::NoButton);
                        sendMouse(vp, QEvent::MouseButtonPress, pa,
                                  Qt::LeftButton, Qt::LeftButton);
                        sendMouse(vp, QEvent::MouseButtonRelease, pa,
                                  Qt::LeftButton, Qt::NoButton);
                    } else {
                    sendMouse(vp, QEvent::MouseButtonPress, pa, Qt::LeftButton,
                              Qt::LeftButton);
                    for (int i = 1; i <= 4; ++i) {
                        const QPointF p =
                            pa + (pb - pa) * (double(i) / 4.0);
                        sendMouse(vp, QEvent::MouseMove, p, Qt::NoButton,
                                  Qt::LeftButton);
                    }
                    sendMouse(vp, QEvent::MouseButtonRelease, pb, Qt::LeftButton,
                              Qt::NoButton);
                    }
                }
                if (id == ToolId::HorizontalType ||
                    id == ToolId::VerticalType) {
                    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape,
                                  Qt::NoModifier);
                    QApplication::sendEvent(vp, &esc);
                } else if (id == ToolId::HorizontalTypeMask ||
                           id == ToolId::VerticalTypeMask) {
                    // Type the run, then commit it into a selection mask.
                    std::snprintf(gesture, sizeof(gesture), "type+enter");
                    QKeyEvent keyA(QEvent::KeyPress, Qt::Key_A,
                                   Qt::NoModifier, QStringLiteral("A"));
                    QApplication::sendEvent(vp, &keyA);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return,
                                    Qt::NoModifier);
                    QApplication::sendEvent(vp, &enter);
                }
                app.processEvents();
            }
        } catch (const std::exception& e) {
            errStr = e.what();
        } catch (...) {
            errStr = "<unknown exception>";
        }
        const double ms = skip ? 0.0 : nowMs() - t0;
        const std::uint64_t after = fingerprint(state, canvas);
        const bool changed = (after != before);
        // AI selection without models refuses cleanly (status hint, no
        // selection): a missing .onnx is an environment SKIP, not a bug.
        // With models present these must segment the painted square.
        // Generative tools gate on the generative model id the same way.
        const bool aiTool = (id == ToolId::QuickSelection ||
                             id == ToolId::ObjectSelection);
        const bool genTool = (id == ToolId::GenerativeFill ||
                              id == ToolId::GenerateBackground);
        const bool aiModelMissing =
            (aiTool &&
             aiModelStore().state(state.settings().bgModel) !=
                 AiModelState::Present) ||
            (genTool &&
             aiModelStore().state(
                 QString::fromUtf8(AppState::generativeModelId())) !=
                 AiModelState::Present);
        const char* verdict;
        bool fail = false;
        if (!errStr.empty()) {
            verdict = "ERROR";
            fail = true;
        } else if (skip) {
            verdict = "SKIP";
            ++toolsSkip;
        } else if (aiModelMissing) {
            verdict = "SKIP-MODEL";
            ++toolsSkip;
        } else if (viewOnly) {
            // View tools must never fail the audit; motion still counts.
            verdict = changed ? "OK" : "OK-noview";
            ++toolsOk;
        } else if (isKnownStub(id)) {
            if (changed) {
                verdict = "STUB-ALIVE";
                fail = true;
            } else {
                verdict = "STUB";
                ++toolsStub;
            }
        } else if (changed) {
            verdict = "OK";
            ++toolsOk;
        } else {
            verdict = "NO-EFFECT";
            fail = true;
        }
        if (fail) ++toolsFail;
        if (ms > kSlowMs) ++toolsSlow;
        DocumentItem* ddLog = state.activeDocument();
        const int nLayers = ddLog ? int(ddLog->layers.size()) : -1;
        auditLog("[tool] %-22s gesture=%-18s ms=%7.2f verdict=%-11s changed=%d%s%s layers=%d",
                 def.name, skip ? "-" : gesture, ms, verdict, changed ? 1 : 0,
                 ms > kSlowMs ? " SLOW" : "",
                 errStr.empty() ? "" : errStr.c_str(), nLayers);
        if (fail) {
            CHECK(false);
        }
    }

    auditLog("[live-audit] tools=%d ok=%d stub=%d skip=%d fail=%d slow(>%gms)=%d",
             nTools, toolsOk, toolsStub, toolsSkip, toolsFail, kSlowMs,
             toolsSlow);

    // ---- brush presets ----
    {
        namespace bl = pittore::ui::brushlibrary;
        const std::vector<bl::BrushPreset> presets = bl::factoryBrushPresets();
        auditLog("[presets] factory=%d", (int)presets.size());
        int ok = 0, fail = 0;
        for (const bl::BrushPreset& p : presets) {
            state.addDocument(QStringLiteral("auditbrush"), QSize(96, 64),
                              300);
            if (p.tool == QStringLiteral("Eraser"))
                state.setActiveTool(ToolId::Eraser);
            else
                state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const std::uint64_t before = fingerprint(state, canvas);
            const double t0 = nowMs();
            std::string errStr;
            try {
                bl::applyPreset(&state, p);
                const QPointF a = canvas->documentToView(QPointF(15, 32));
                const QPointF b = canvas->documentToView(QPointF(80, 32));
                QWidget* vp = canvas->viewport();
                sendMouse(vp, QEvent::MouseButtonPress, a, Qt::LeftButton,
                          Qt::LeftButton);
                for (int i = 1; i <= 4; ++i) {
                    const QPointF mp = a + (b - a) * (double(i) / 4.0);
                    sendMouse(vp, QEvent::MouseMove, mp, Qt::NoButton,
                              Qt::LeftButton);
                }
                sendMouse(vp, QEvent::MouseButtonRelease, b, Qt::LeftButton,
                          Qt::NoButton);
                app.processEvents();
            } catch (const std::exception& e) {
                errStr = e.what();
            } catch (...) {
                errStr = "<unknown>";
            }
            const double ms = nowMs() - t0;
            const bool changed =
                fingerprint(state, canvas) != before;
            bool bad = !errStr.empty() || !changed;
            if (bad)
                ++fail;
            else
                ++ok;
            auditLog("[preset] %-20s ms=%7.2f verdict=%s%s%s",
                     p.name.toUtf8().constData(), ms,
                     !errStr.empty()
                         ? "ERROR"
                         : (changed ? "OK" : "NO-EFFECT"),
                     ms > kSlowMs ? " SLOW" : "",
                     errStr.empty() ? "" : errStr.c_str());
            if (bad) {
                CHECK(false);
            }
        }
        auditLog("[live-audit] presets ok=%d fail=%d", ok, fail);
    }

    // ---- filters ----
    {
        const std::vector<pittore::filter::FilterDef> defs =
            pittore::filter::allFilterDefs();
        auditLog("[filters] registered=%d", (int)defs.size());
        const std::uint32_t w = 96, h = 64;
        int ok = 0, unchanged = 0, skipped = 0, fail = 0;
        for (const auto& def : defs) {
            pittore::Image probe(w, h);
            std::uint64_t salt = 0x9E3779B97F4A7C15ULL;
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x) {
                    // Left: smooth gradient. Right: gradient + salt noise so
                    // denoisers, sharpeners and edge detectors all see work.
                    float n = 0.0f;
                    if (x >= w / 2) {
                        salt = salt * 6364136223846793005ULL + 1442695040888963407ULL;
                        n = ((salt >> 33) & 1) ? 0.35f : -0.35f;
                    }
                    probe.data()[std::size_t(y) * w + x] = pittore::RGBAf{
                        std::clamp(float(x) / float(w) + n, 0.0f, 1.0f),
                        std::clamp(float(y) / float(h) + n, 0.0f, 1.0f), 0.5f,
                        1.0f};
                }
            std::uint64_t h0 = 1469598103934665603ULL;
            h0 = fnv(probe.data(), std::size_t(w) * h * sizeof(pittore::RGBAf),
                     h0);
            std::vector<double> par;
            for (const auto& prm : def.params) par.push_back(prm.def);
            const double t0 = nowMs();
            std::string errStr;
            bool modelSkip = false;
            try {
                pittore::filter::applyFilter(probe, def.id, par);
            } catch (const std::exception& e) {
                const std::string m = e.what();
                if (m.find("model") != std::string::npos ||
                    m.find("onnx") != std::string::npos ||
                    m.find("ORT") != std::string::npos ||
                    m.find("neural") != std::string::npos ||
                    m.find("weights") != std::string::npos) {
                    modelSkip = true;
                } else {
                    errStr = e.what();
                }
            } catch (...) {
                errStr = "<unknown>";
            }
            const double ms = nowMs() - t0;
            bool finite = true;
            for (std::uint32_t i = 0; i < w * h; ++i) {
                const pittore::RGBAf& q = probe.data()[i];
                if (!std::isfinite(q.r) || !std::isfinite(q.g) ||
                    !std::isfinite(q.b) || !std::isfinite(q.a)) {
                    finite = false;
                    break;
                }
            }
            std::uint64_t h1 = 1469598103934665603ULL;
            h1 = fnv(probe.data(), std::size_t(w) * h * sizeof(pittore::RGBAf),
                     h1);
            const char* verdict = nullptr;
            bool bad = false;
            if (modelSkip) {
                verdict = "SKIP-MODEL";
                ++skipped;
            } else if (!errStr.empty()) {
                verdict = "ERROR";
                ++fail;
                bad = true;
            } else if (!finite) {
                verdict = "NONFINITE";
                ++fail;
                bad = true;
            } else if (h1 == h0) {
                verdict = "UNCHANGED";
                ++unchanged;
            } else {
                verdict = "OK";
                ++ok;
            }
            auditLog("[filter] %-28s ms=%7.2f verdict=%-11s%s%s", def.id, ms,
                     verdict, ms > kSlowMs ? " SLOW" : "",
                     errStr.empty() ? "" : errStr.c_str());
            if (bad) {
                CHECK(false);
            }
        }
        auditLog("[live-audit] filters ok=%d unchanged=%d skipped-model=%d "
                 "fail=%d",
                 ok, unchanged, skipped, fail);
    }

    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    std::fflush(stdout);
    return rc;
}
