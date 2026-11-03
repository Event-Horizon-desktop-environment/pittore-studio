// test_artboard_bench.cpp — board benchmark (P1 step 7, §16.5).
//
// Builds a 200-board document and performs 100 board moves, printing the three
// numbers §16.5 asks for, which turn §7.2's estimate and R-03 into data:
//
//   - snapshot bytes per undo step (R-02: is the board list's memory growth
//     bounded? §7.2 target is < 1 MB per step for the 200-board case);
//   - recomposite rect area per move (R-03: the region a correct, region-scoped
//     invalidation would need to recomposite — the moved board's old ∪ new rect);
//   - full-rebuild count (R-03: today's moveBoardBy does one full-document
//     rebuild per move — the invalidation storm R-03 flags — so the area above
//     is the saving region-scoped invalidation would realise).
//
// This is model-level and geometric: no composite is run, so it is fast and
// byte-for-byte deterministic on every backend (CPU, CUDA, HIP). The layer rows
// are built directly on the model (mirroring createBoard's payload) so setup
// does not perform 200 full composites either.
//
// Runs headless under QCoreApplication (no widgets are constructed).
#include <QCoreApplication>
#include <QDir>

#include <cstdio>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"

using namespace pittore::ui;

namespace {

constexpr int kBoards = 200;
constexpr int kMoves = 100;

DocumentItem* freshDoc(AppState& state) {
    DocumentItem* d =
        state.addDocument(QStringLiteral("bench"), QSize(2200, 1200), 72);
    if (d) {
        d->layers.clear();
        d->activeLayer = 0;
        d->selectedLayers.clear();
    }
    return d;
}

// A tiny opaque pixel layer, one per board, so a move translates a subtree
// (INV-008) and the snapshot carries realistic per-board content.
LayerItem childPx(const QPointF& offset) {
    LayerItem px;
    px.kind = LayerItem::Kind::Pixel;
    auto img = std::make_shared<pittore::Image>(8, 8);
    img->fill(pittore::RGBAf{0.5f, 0.5f, 0.5f, 1.0f});
    px.pixels = std::move(img);
    px.sourceStamp = 1;
    px.offset = offset;
    px.indent = 1;
    return px;
}

// Build the 200-board grid directly on the model. Each board is a root Group
// row with the board payload (INV-011) and one child beneath it; boards are
// prepended like createBoard so the newest sits at the top of the stack.
void buildBoards(DocumentItem* d) {
    constexpr int kCols = 20;
    for (int i = 0; i < kBoards; ++i) {
        const QPointF origin((i % kCols) * 110.0, (i / kCols) * 110.0);
        LayerItem b;
        b.kind = LayerItem::Kind::Group;
        b.boardId = ++d->boardIdCounter;  // never reuse (INV-006)
        b.boardRect = QRectF(origin, QSizeF(100, 100));
        b.name = QStringLiteral("Board %1").arg(b.boardId);
        b.indent = 0;  // root-only (INV-010)
        b.groupExpanded = true;
        b.visible = true;
        b.opacity = 100;
        d->layers.prepend(b);
        d->layers.insert(1, childPx(origin));
        d->boardOrder.prepend(b.boardId);
    }
    d->activeBoard = d->boardOrder.isEmpty() ? 0 : d->boardOrder.first();
}

// A snapshot is a structural copy of the model with pixel Images shared (one
// refcount bump each), so its owned heap is the layer struct array plus the
// board id list — exactly the terms R-02 bounds. Measuring the model gives the
// per-step cost without touching the private undo engine; history is bounded by
// the undo cap (100) and small, so it is not the growth term.
std::size_t snapshotBytes(const DocumentItem* d) {
    return d->layers.size() * sizeof(LayerItem) +
           d->boardOrder.size() * sizeof(quint64);
}

}  // namespace

// 200 boards, 100 moves: report snapshot bytes/step, the region-scoped
// recomposite area/step, and the full-rebuild count, then check the §7.2
// per-step snapshot budget.
static void bench_boards() {
    AppState state;
    DocumentItem* d = freshDoc(state);
    CHECK(d != nullptr);
    if (!d) return;

    buildBoards(d);
    CHECK_EQ(d->layers.size(), kBoards * 2);
    CHECK_EQ(d->boardOrder.size(), kBoards);
    CHECK(d->boardOrderIsPermutation());  // INV-007

    d->resetHistory();  // measure the 100 moves alone

    const std::size_t baseBytes = snapshotBytes(d);
    const double fullArea =
        static_cast<double>(d->size.width()) * d->size.height();

    // 100 moves: translate a board and its child by a fixed delta, each one undo
    // step (snapshot captured), mirroring moveBoardBy's model work (INV-008).
    double areaSum = 0.0;
    std::size_t maxBytes = baseBytes;
    for (int i = 0; i < kMoves; ++i) {
        const std::uint64_t id = d->boardOrder[i % kBoards];
        const int row = d->boardRowIndex(id);
        CHECK(row >= 0);
        if (row < 0 || row + 1 >= d->layers.size()) break;
        const QRectF before = d->layers[row].boardRect;
        const QPointF delta(3.0, 2.0);
        d->beginUndoAction();
        d->layers[row].boardRect.translate(delta);
        d->layers[row + 1].offset += delta;  // the board's single child
        d->commitUndoAction(QStringLiteral("Move"), QStringLiteral("move"));
        const QRectF affected = before.united(d->layers[row].boardRect);
        areaSum += affected.width() * affected.height();
        maxBytes = std::max(maxBytes, snapshotBytes(d));
    }

    const double regionArea = areaSum / kMoves;
    // Today moveBoardBy performs one full rebuild per move (the R-03 storm), so
    // the full-rebuild count equals the move count and its per-step area is the
    // whole document; region-scoped invalidation would touch only `regionArea`.
    const int fullRebuilds = kMoves;

    std::printf(
        "  boards=%d moves=%d\n"
        "  snapshot/step: base=%.1f KB max=%.1f KB  (budget 1024 KB)\n"
        "  recomposite/step: region=%.0f px  full-rebuild=%.0f px  "
        "(saving %.0fx)\n"
        "  full-rebuild count=%d\n",
        kBoards, kMoves, baseBytes / 1024.0, maxBytes / 1024.0, regionArea,
        fullArea, fullArea / regionArea, fullRebuilds);

    // §7.2: a per-step snapshot stays under 1 MB for the 200-board case.
    CHECK(maxBytes < 1024 * 1024);
    // Region-scoped area is strictly smaller than a full rebuild (R-03 has a
    // concrete target, not an assumption).
    CHECK(regionArea < fullArea);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-artboard-bench-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    bench_boards();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
