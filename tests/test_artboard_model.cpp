// test_artboard_model.cpp — board model invariants (P1 step 1).
//
// A board is a Kind::Group row carrying a nonzero boardId plus the payload (D1,
// INV-011). These checks pin the invariants this step establishes: creation and
// id allocation (INV-006), the boardOrder permutation (INV-007), boardAt /
// boardChildRows resolution (INV-011, D3), move-translates-children (INV-008),
// delete keep-vs-drop (INV-013/019), and that every mutation is one undo step
// that restores the exact prior model (INV-030).
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
    DocumentItem* d =
        state.addDocument(QStringLiteral("boards"), QSize(400, 400), 72);
    // addDocument seeds a default layer; the board invariants are about the
    // board rows alone, so start from an empty stack.
    if (d) {
        d->layers.clear();
        d->activeLayer = 0;
        d->selectedLayers.clear();
    }
    return d;
}

// A plain pixel layer at a known global offset, to prove reparent/move keep
// geometry (INV-012, INV-008).
LayerItem pixelAt(const QPointF& offset) {
    LayerItem px;
    px.name = QStringLiteral("px");
    px.kind = LayerItem::Kind::Pixel;
    px.offset = offset;
    auto img = std::make_shared<pittore::Image>(8, 8);
    img->fill(pittore::RGBAf{0.5f, 0.5f, 0.5f, 1.0f});
    px.pixels = std::move(img);
    px.sourceStamp = 1;
    return px;
}

}  // namespace

// Creation allocates a fresh id, puts a root board row at the top, records it
// in boardOrder, and makes it active+selected.
static void test_create_board() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;

    const std::uint64_t a = state.createBoard(QRectF(0, 0, 100, 50),
                                              QStringLiteral("A"));
    CHECK(a != 0);
    CHECK_EQ(d->layers.size(), 1);
    CHECK(d->layers[0].isBoard());
    CHECK_EQ(d->layers[0].boardId, a);
    CHECK_EQ(d->layers[0].indent, 0);              // root-only (INV-010)
    CHECK(d->layers[0].boardRect == QRectF(0, 0, 100, 50));
    CHECK_EQ(d->boardOrder.size(), 1);
    CHECK_EQ(d->boardOrder[0], a);
    CHECK_EQ(d->activeBoard, a);
    CHECK(d->boardOrderIsPermutation());           // INV-007

    const std::uint64_t b = state.createBoard(QRectF(120, 0, 100, 50),
                                              QStringLiteral("B"));
    CHECK(b != 0 && b != a);                        // fresh id (INV-006)
    CHECK_EQ(d->layers.size(), 2);
    CHECK_EQ(d->boardOrder.size(), 2);
    CHECK(d->boardOrderIsPermutation());
    CHECK_EQ(d->boardRowIndex(a), 1);               // B prepended above A
    CHECK_EQ(d->boardRowIndex(b), 0);
}

// A non-finite or zero-area rect is refused and allocates nothing.
static void test_create_board_rejects_bad_rect() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_EQ(state.createBoard(QRectF(0, 0, 0, 0), QStringLiteral("z")), 0u);
    CHECK_EQ(state.createBoard(QRectF(0, 0, -5, 10), QStringLiteral("z")), 0u);
    CHECK_EQ(state.createBoard(QRectF(qQNaN(), 0, 10, 10), QStringLiteral("z")),
             0u);
    CHECK_EQ(d->layers.size(), 0);
    CHECK_EQ(d->boardIdCounter, 0u);                // no id burned on refusal
}

// boardAt resolves a child to its board and root content to 0 (pasteboard, D3);
// boardChildRows lists exactly the subtree.
static void test_board_at_and_children() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const std::uint64_t a = state.createBoard(QRectF(0, 0, 100, 100),
                                              QStringLiteral("A"));
    const int boardRow = d->boardRowIndex(a);

    // Insert a pixel child under the board (indent 1), and one at root (0).
    d->beginUndoAction();
    d->layers.insert(boardRow + 1, pixelAt(QPointF(10, 10)));
    d->layers[boardRow + 1].indent = 1;
    d->layers.prepend(pixelAt(QPointF(300, 300)));  // root content, indent 0
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));

    CHECK_EQ(d->layers.size(), 3);
    const int childRow = d->boardRowIndex(a) + 1;
    CHECK_EQ(d->boardAt(childRow), a);              // child resolves to board
    CHECK_EQ(d->boardAt(0), 0u);                    // root pixel = pasteboard
    QVector<int> kids = d->boardChildRows(d->boardRowIndex(a));
    CHECK_EQ(kids.size(), 1);
    if (!kids.isEmpty()) CHECK_EQ(kids[0], childRow);
    CHECK_EQ(d->boardAt(d->boardRowIndex(a)), a);   // the board itself
}

// Moving a board translates its subtree by the same delta (INV-008); a root
// pixel is untouched.
static void test_move_board_moves_children() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const std::uint64_t a = state.createBoard(QRectF(0, 0, 100, 100),
                                              QStringLiteral("A"));
    const int boardRow = d->boardRowIndex(a);
    d->beginUndoAction();
    d->layers.insert(boardRow + 1, pixelAt(QPointF(10, 10)));
    d->layers[boardRow + 1].indent = 1;
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));
    const int childRow = d->boardRowIndex(a) + 1;

    CHECK(state.moveBoardBy(a, 25, 40));
    CHECK(d->layers[d->boardRowIndex(a)].boardRect ==
          QRectF(25, 40, 100, 100));
    CHECK(d->layers[childRow].offset == QPointF(35, 50));  // 10+25, 10+40
    CHECK_EQ(d->boardAt(childRow), a);             // still on the board

    state.undo();
    CHECK(d->layers[d->boardRowIndex(a)].boardRect == QRectF(0, 0, 100, 100));
    CHECK(d->layers[childRow].offset == QPointF(10, 10));
}

// Delete with keepContent reparents children to the root (geometry preserved,
// INV-019); without it the subtree goes. Both are one undo step (INV-030).
static void test_delete_board_keep_vs_drop() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const std::uint64_t a = state.createBoard(QRectF(0, 0, 100, 100),
                                              QStringLiteral("A"));
    int boardRow = d->boardRowIndex(a);
    d->beginUndoAction();
    d->layers.insert(boardRow + 1, pixelAt(QPointF(10, 10)));
    d->layers[boardRow + 1].indent = 1;
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));

    // keepContent: board gone, pixel survives at root with same geometry.
    const int depth = state.undoDepth();
    CHECK(state.deleteBoard(a, /*keepContent=*/true));
    CHECK_EQ(state.undoDepth(), depth + 1);         // one step
    CHECK_EQ(d->layers.size(), 1);                 // pixel remains
    CHECK(!d->layers[0].isBoard());
    CHECK(d->layers[0].offset == QPointF(10, 10));  // geometry preserved
    CHECK_EQ(d->boardOrder.size(), 0);
    CHECK_EQ(d->activeBoard, 0u);

    state.undo();                                   // board + child restored
    CHECK_EQ(d->layers.size(), 2);
    CHECK(d->layers[d->boardRowIndex(a)].isBoard());

    // drop: board and subtree both go.
    boardRow = d->boardRowIndex(a);
    CHECK(state.deleteBoard(a, /*keepContent=*/false));
    CHECK_EQ(d->layers.size(), 0);
    CHECK_EQ(d->boardOrder.size(), 0);
    state.undo();
    CHECK_EQ(d->layers.size(), 2);
    CHECK(d->boardOrderIsPermutation());
}

// A locked board rejects mutation at the model layer (INV-023), not just in UI.
static void test_locked_board_rejects_mutation() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const std::uint64_t a = state.createBoard(QRectF(0, 0, 50, 50),
                                              QStringLiteral("A"));
    CHECK(state.setBoardLocked(a, true));
    CHECK(!state.moveBoardBy(a, 10, 10));
    CHECK(!state.setBoardRect(a, QRectF(0, 0, 80, 80)));
    CHECK(!state.renameBoard(a, QStringLiteral("nope")));
    CHECK(!state.deleteBoard(a, true));
    // Visibility and lock themselves still toggle (they are the affordances
    // that unlock the board).
    CHECK(state.setBoardVisible(a, false));
    CHECK(state.setBoardLocked(a, false));
    CHECK(state.moveBoardBy(a, 10, 10));            // now allowed
}

// Fill and opacity round-trip and undo as one step each.
static void test_board_fill_and_opacity() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const std::uint64_t a = state.createBoard(QRectF(0, 0, 50, 50),
                                              QStringLiteral("A"));
    const int row = d->boardRowIndex(a);
    CHECK(state.setBoardFill(a, true, QColor(0xff, 0x00, 0x00), 0.5));
    CHECK(d->layers[row].boardHasFill);
    CHECK_EQ(d->layers[row].boardFillOpacity, 0.5);
    state.undo();
    CHECK(!d->layers[row].boardHasFill);
    CHECK_EQ(d->layers[row].boardFillOpacity, 1.0);
    // Opacity reuses LayerItem::opacity (INV-022): there is no second field.
    d->beginUndoAction();
    d->layers[row].opacity = 40;
    d->commitUndoAction(QStringLiteral("opacity"), QStringLiteral("opacity"));
    CHECK_EQ(d->layers[row].opacity, 40);
    CHECK(!d->layers[row].isBoard() == false);      // still a board
}

// The O(n) boardOwnership sweep must agree with the O(depth) boardAt() query
// for every row — the paint path swaps one for the other, so they cannot drift.
static void test_board_ownership_sweep_matches_point_query() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;
    const std::uint64_t a = state.createBoard(QRectF(0, 0, 100, 100),
                                              QStringLiteral("A"));
    const int boardRow = d->boardRowIndex(a);
    // Child under the board (indent 1) and a root pixel (indent 0).
    d->beginUndoAction();
    d->layers.insert(boardRow + 1, pixelAt(QPointF(10, 10)));
    d->layers[boardRow + 1].indent = 1;
    d->layers.prepend(pixelAt(QPointF(300, 300)));
    d->commitUndoAction(QStringLiteral("Add"), QStringLiteral("add"));

    QVector<std::uint64_t> board;
    d->boardOwnership(board);
    CHECK_EQ(board.size(), d->layers.size());
    for (int i = 0; i < d->layers.size(); ++i)
        if (i < board.size()) CHECK_EQ(board[i], d->boardAt(i));
    // Explicit expectations: root content is pasteboard (0), child is on the
    // board, the board row claims itself.
    if (board.size() == 3) {
        CHECK_EQ(board[0], 0u);        // root pixel = pasteboard (D3)
        CHECK_EQ(board[1], a);         // board row claims itself
        CHECK_EQ(board[2], a);         // child resolves to board
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-artboard-model-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_create_board();
    test_create_board_rejects_bad_rect();
    test_board_at_and_children();
    test_move_board_moves_children();
    test_delete_board_keep_vs_drop();
    test_locked_board_rejects_mutation();
    test_board_fill_and_opacity();
    test_board_ownership_sweep_matches_point_query();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
