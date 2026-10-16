// test_undo_gestures.cpp — Ctrl+Z covers the live panel controls.
//
// The Layers/Properties panels write straight through to the layer during a
// drag and hold one undo step for the whole gesture (begin on press, commit
// on release); spinbox/combo edits wrap singly. This drives the same
// begin/write/commit sequences headless and checks undo restores them:
// one step per drag, one per click, redo re-applies.
//
// Runs headless under QCoreApplication (no widgets are constructed).
#include <QCoreApplication>
#include <QDir>

#include "engine/compute/adjust.h"
#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"

using namespace pittore::ui;

namespace {

// A document with one pixel layer, like the panel header edits.
DocumentItem* makeDoc(AppState& state) {
    DocumentItem* d = state.addDocument(QStringLiteral("gest"), QSize(32, 32), 72);
    if (!d) return nullptr;
    LayerItem px;
    px.name = QStringLiteral("px");
    px.kind = LayerItem::Kind::Pixel;
    auto img = std::make_shared<pittore::Image>(32, 32);
    img->fill(pittore::RGBAf{0.2f, 0.3f, 0.4f, 1.0f});
    px.pixels = std::move(img);
    px.sourceStamp = 1;
    state.beginUndoStep();
    state.addLayer(px);
    state.commitUndoStep(QStringLiteral("New Layer"), QStringLiteral("newlayer"));
    state.setActiveLayerIndex(0);
    return state.activeDocument();
}

}  // namespace

// A slider drag (press, N live writes, release) is exactly one undo step,
// and undo restores the pre-drag value.
static void test_opacity_drag_coalesces() {
    AppState state;
    DocumentItem* d = makeDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const int depthBefore = state.undoDepth();

    // Press: open the gesture, as LayersPanel::opacitySlider_ does.
    CHECK(!state.undoStepActive());
    state.beginUndoStep();
    CHECK(state.undoStepActive());
    for (int v : {90, 70, 45}) {
        d->layers[d->activeLayer].opacity = v;
        d->rebuildComposite();
    }
    state.commitUndoStep(QStringLiteral("Layer Opacity"), QStringLiteral("opacity"));
    CHECK(!state.undoStepActive());
    CHECK_EQ(state.undoDepth(), depthBefore + 1);
    CHECK_EQ(d->layers[d->activeLayer].opacity, 45);

    state.undo();
    CHECK_EQ(d->layers[d->activeLayer].opacity, 100);
    state.redo();
    CHECK_EQ(d->layers[d->activeLayer].opacity, 45);
}

// Spinbox/combo edits wrap singly: one change, one step.
static void test_fill_blend_single_steps() {
    AppState state;
    DocumentItem* d = makeDoc(state);
    CHECK(d != nullptr);
    if (!d) return;

    state.beginUndoStep();
    d->layers[d->activeLayer].fill = 60;
    state.commitUndoStep(QStringLiteral("Fill"), QStringLiteral("fill"));
    CHECK_EQ(d->layers[d->activeLayer].fill, 60);
    state.undo();
    CHECK_EQ(d->layers[d->activeLayer].fill, 100);

    state.beginUndoStep();
    d->layers[d->activeLayer].blendMode = QStringLiteral("Multiply");
    state.commitUndoStep(QStringLiteral("Blend Mode"), QStringLiteral("blend"));
    CHECK(d->layers[d->activeLayer].blendMode == QStringLiteral("Multiply"));
    state.undo();
    CHECK(d->layers[d->activeLayer].blendMode == QStringLiteral("Normal"));
}

// An adjustment drag coalesces too, and nested layer creation joins the
// outer gesture (the Color Grade dialog contract).
static void test_adjustment_drag_and_nested_create() {
    AppState state;
    DocumentItem* d = makeDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const int depthBefore = state.undoDepth();
    const int layersBefore = static_cast<int>(d->layers.size());

    state.beginUndoStep();  // dialog session opens
    CHECK(state.undoStepActive());
    const int expKind = static_cast<int>(pittore::compute::AdjustmentKind::Exposure);
    CHECK(state.addAdjustmentLayer(expKind));  // nested begin/commit joins
    const int idx = d->activeLayer;
    for (float v : {1.0f, 2.5f, 5.0f})
        CHECK(state.setAdjustmentParamAt(idx, 0, v));
    state.commitUndoStep(QStringLiteral("Color Grade"), QStringLiteral("adjustments"));
    CHECK_EQ(state.undoDepth(), depthBefore + 1);

    // One undo removes the created layer and its edits together.
    state.undo();
    CHECK_EQ(static_cast<int>(state.activeDocument()->layers.size()), layersBefore);
    state.redo();
    CHECK_EQ(static_cast<int>(state.activeDocument()->layers.size()), layersBefore + 1);
}

// Discarding a gesture drops the step but keeps live writes (for gestures
// that turn out to change nothing, the caller compares first); cancelling
// rewinds them, for modal sessions whose Cancel must leave no trace.
static void test_discard_vs_cancel() {
    AppState state;
    DocumentItem* d = makeDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const int depthBefore = state.undoDepth();

    state.beginUndoStep();
    d->layers[d->activeLayer].opacity = 10;
    d->rebuildComposite();
    state.discardUndoStep();
    CHECK(!state.undoStepActive());
    CHECK_EQ(state.undoDepth(), depthBefore);
    CHECK_EQ(d->layers[d->activeLayer].opacity, 10);  // write stands

    state.beginUndoStep();
    d->layers[d->activeLayer].opacity = 20;
    d->rebuildComposite();
    state.cancelUndoStep();
    CHECK(!state.undoStepActive());
    CHECK_EQ(state.undoDepth(), depthBefore);
    CHECK_EQ(d->layers[d->activeLayer].opacity, 10);  // rewound one gesture
    state.undo();  // the surviving history still steps
    CHECK_EQ(d->layers[d->activeLayer].opacity, 100);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir = QDir::tempPath() + QStringLiteral("/pittore-undo-gestures-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_opacity_drag_coalesces();
    test_fill_blend_single_steps();
    test_adjustment_drag_and_nested_create();
    test_discard_vs_cancel();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
