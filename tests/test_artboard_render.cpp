// test_artboard_render.cpp — board clip in the composite (P1 step 4).
//
// A board's subtree paints only inside boardRect (D1). These checks drive the
// real gather→composite path: an opaque child that overflows its board must be
// clipped to the board's bounds, while a pasteboard layer (not on any board,
// D3) must paint un-clipped across the whole document. The clip is geometric
// and lives entirely on the CPU (app_state_board_render.cpp), so it is shared
// by the CPU and GPU composite paths — no parity surface.
//
// Runs headless under QCoreApplication (no widgets are constructed).
#include <QCoreApplication>
#include <QDir>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"

using namespace pittore::ui;

namespace {

DocumentItem* freshDoc(AppState& state) {
    DocumentItem* d = state.addDocument(QStringLiteral("clip"), QSize(200, 200), 72);
    // addDocument seeds a default layer; the clip invariants are about the
    // board rows alone, so start from an empty stack.
    if (d) {
        d->layers.clear();
        d->activeLayer = 0;
        d->selectedLayers.clear();
    }
    return d;
}

// An opaque solid-colour pixel layer covering (0,0,w,h) at scale 1.
LayerItem solidPixel(int w, int h, const QColor& c) {
    LayerItem px;
    px.kind = LayerItem::Kind::Pixel;
    auto img = std::make_shared<pittore::Image>(w, h);
    img->fill(pittore::RGBAf{c.redF(), c.greenF(), c.blueF(), 1.0f});
    px.pixels = std::move(img);
    px.sourceStamp = 1;
    px.offset = QPointF(0, 0);
    px.opacity = 100;
    px.fill = 100;
    return px;
}

bool nearColor(const QColor& c, int r, int g, int b) {
    return std::abs(c.red() - r) < 40 && std::abs(c.green() - g) < 40 &&
           std::abs(c.blue() - b) < 40;
}

}  // namespace

// A red child overflows its board (fills the whole 200x200 doc but sits on a
// 50x50 board): inside the board it shows; outside it is clipped away and the
// green pasteboard layer below it shows instead. The green root layer proves
// pasteboard content is NOT clipped (it paints at (100,100), outside any board).
static void test_board_clips_overflow_pasteboard_does_not() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;

    const std::uint64_t a =
        state.createBoard(QRectF(0, 0, 50, 50), QStringLiteral("A"));
    const int boardRow = d->boardRowIndex(a);

    d->beginUndoAction();
    // Red child ON the board (indent 1), overflowing it to the full doc.
    LayerItem red = solidPixel(200, 200, QColor(255, 0, 0));
    red.indent = 1;
    d->layers.insert(boardRow + 1, red);
    // Green pasteboard layer at the root (indent 0), below the board in z, so
    // it is visible wherever the red child is clipped away.
    d->layers.append(solidPixel(200, 200, QColor(0, 255, 0)));
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));

    d->rebuildComposite();

    // Inside the board: the red board child wins (opaque, on top of green).
    CHECK(nearColor(d->composite.pixelColor(25, 25), 255, 0, 0));
    // Outside the board: red is clipped away; the green pasteboard shows, and
    // green itself is un-clipped (it paints past the board to the doc edge).
    CHECK(nearColor(d->composite.pixelColor(100, 100), 0, 255, 0));
    CHECK(nearColor(d->composite.pixelColor(199, 199), 0, 255, 0));
}

// A board at a non-origin position clips to exactly its rect, not to (0,0).
static void test_board_clip_at_offset() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;

    const std::uint64_t a =
        state.createBoard(QRectF(50, 50, 40, 40), QStringLiteral("B"));
    const int boardRow = d->boardRowIndex(a);

    d->beginUndoAction();
    LayerItem red = solidPixel(200, 200, QColor(255, 0, 0));
    red.indent = 1;
    d->layers.insert(boardRow + 1, red);
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));

    d->rebuildComposite();

    // Corners of the board rect (50..89) are painted.
    CHECK(nearColor(d->composite.pixelColor(50, 50), 255, 0, 0));
    CHECK(nearColor(d->composite.pixelColor(89, 89), 255, 0, 0));
    // Just outside the board rect on every side: clipped away (transparent
    // composite here — no other layer, so the pixel is unpainted).
    CHECK(d->composite.pixelColor(49, 70).alpha() < 32);
    CHECK(d->composite.pixelColor(90, 70).alpha() < 32);
    CHECK(d->composite.pixelColor(70, 49).alpha() < 32);
    CHECK(d->composite.pixelColor(70, 90).alpha() < 32);
}

// A board fill (D1) paints behind the board's children and stops at the board
// edge. A blue-filled 50x50 board with a small red child proves the three
// z/clip rules: the child wins where it covers (fill is BEHIND), the fill shows
// where the child does not, and nothing paints outside the board rect. A second
// pass at 50% fill opacity proves the fold is applied.
static void test_board_fill_behind_children() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;

    const std::uint64_t a =
        state.createBoard(QRectF(0, 0, 50, 50), QStringLiteral("A"));
    CHECK(state.setBoardFill(a, true, QColor(0, 0, 255), 1.0));
    const int boardRow = d->boardRowIndex(a);

    d->beginUndoAction();
    // Small red child on the board (indent 1) covering only (0,0)-(20,20), so
    // the rest of the board shows the fill.
    LayerItem red = solidPixel(20, 20, QColor(255, 0, 0));
    red.indent = 1;
    d->layers.insert(boardRow + 1, red);
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));

    d->rebuildComposite();

    // Child over the fill: the fill is behind, so the red child wins.
    CHECK(nearColor(d->composite.pixelColor(10, 10), 255, 0, 0));
    // No child there: the blue fill shows, clipped to the board rect.
    CHECK(nearColor(d->composite.pixelColor(35, 35), 0, 0, 255));
    // Outside the board: the fill does not paint.
    CHECK(d->composite.pixelColor(60, 60).alpha() < 32);

    // Half-opacity fill: where only the fill lands it is semi-transparent.
    CHECK(state.setBoardFill(a, true, QColor(0, 0, 255), 0.5));
    d->rebuildComposite();
    const int fa = d->composite.pixelColor(35, 35).alpha();
    CHECK(fa > 96 && fa < 160);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-artboard-render-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_board_clips_overflow_pasteboard_does_not();
    test_board_clip_at_offset();
    test_board_fill_behind_children();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
