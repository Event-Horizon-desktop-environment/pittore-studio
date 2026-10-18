// test_af_artboard.cpp — D9: a multi-artboard Affinity file opens as ONE
// document containing N boards on one canvas, replacing the old
// one-document-per-board split.
//
// The test drives the real import path (AppState::openImageFile, the same route
// File > Open runs) and pins the new expectation: one document, two board rows
// at their recorded rects and names, every recovered layer parented to its
// board, and no second/third document. The fixture is an external sample (two
// birthday cards); a checkout without it self-skips, so no sample bytes land in
// the repo (PITTORE_AF_ARTBOARD_FIXTURE points at a real .af file).
//
// Board rects and names below are pinned to the decoder contract: canvas from
// the spread union, board rects from each board's ShpB record under its
// placement, names from the file's Desc labels. Board 1's origin is fractional
// in the file (~1200.114), so it is checked with a tolerance.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QCoreApplication>
#include <QString>

#include "ui/app_state.h"

using pittore::ui::AppState;
using pittore::ui::DocumentItem;
using pittore::ui::LayerItem;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void testOneDocumentManyBoards() {
    const char* env = std::getenv("PITTORE_AF_ARTBOARD_FIXTURE");
    if (!env || !*env) {
        std::printf("test_af_artboard: fixture absent "
                    "(PITTORE_AF_ARTBOARD_FIXTURE) — skipped\n");
        check(true, "artboard fixture absent — skipped");
        return;
    }

    AppState state;
    QString err;
    const bool ok = state.openImageFile(QString::fromLocal8Bit(env), &err);
    check(ok, ok ? "multi-artboard file opens"
                 : err.toUtf8().constData());
    if (!ok) return;

    // D9 headline: ONE document, not one per board (the old split made two
    // tabs). This is the assertion the rewrite exists to establish.
    check(state.documents().size() == 1,
          "one document (was one-per-board before D9)");
    DocumentItem* doc = state.activeDocument();
    check(doc != nullptr, "the opened document is active");
    if (!doc) return;

    // Collect the board rows (root-level Group rows with a nonzero boardId).
    int boardRows = 0;
    std::uint64_t firstId = 0, secondId = 0;
    for (const LayerItem& li : doc->layers) {
        if (!li.isBoard()) continue;
        if (boardRows == 0) firstId = li.boardId;
        else if (boardRows == 1) secondId = li.boardId;
        ++boardRows;
    }
    check(boardRows == 2, "two board rows at the document root");
    if (boardRows != 2) return;

    check(firstId != 0 && secondId != 0 && firstId != secondId,
          "board rows carry distinct nonzero ids");

    // Names + rects, matched by name (the panel lists boards in reverse file
    // order — Lee above Bryan — which mirrors Affinity's Layers panel, so the
    // assertions must not depend on row position). Board rects come straight
    // from the recorded ShpB bounds; board 1's origin is fractional in-file.
    bool sawBryan = false, sawLee = false;
    for (const LayerItem& li : doc->layers) {
        if (!li.isBoard()) continue;
        const QRectF& r = li.boardRect;
        if (li.name == QStringLiteral("Bryan's Card")) {
            sawBryan = true;
            check(std::fabs(r.x() - 0.0) < 0.001 &&
                      std::fabs(r.y() - 0.0) < 0.001 &&
                      std::fabs(r.width() - 1050.0) < 0.001 &&
                      std::fabs(r.height() - 900.0) < 0.001,
                  "Bryan's Card rect 0,0 1050x900");
        } else if (li.name == QStringLiteral("Lee's Card")) {
            sawLee = true;
            check(std::fabs(r.x() - 1200.114) < 0.5 &&
                      std::fabs(r.y() - 0.0) < 0.001 &&
                      std::fabs(r.width() - 1050.0) < 1.0 &&
                      std::fabs(r.height() - 900.0) < 0.001,
                  "Lee's Card rect ~1200,0 1050x900");
        }
    }
    check(sawBryan, "a board row named \"Bryan's Card\"");
    check(sawLee, "a board row named \"Lee's Card\"");

    // Every recovered content layer is parented to a board (boardAt() != 0);
    // none is left on the pasteboard at the document root. The fixture's ten
    // layers are all board-tagged, so the root must hold only the two boards.
    int contentLayers = 0;
    int rootContent = 0;
    for (int i = 0; i < doc->layers.size(); ++i) {
        if (doc->layers[i].isBoard()) continue;
        ++contentLayers;
        if (doc->boardAt(i) == 0) ++rootContent;
    }
    check(contentLayers == 10, "ten recovered content layers");
    check(rootContent == 0, "no content layer left on the pasteboard root");

    // The board index is coherent: a two-element permutation and an active
    // board that resolves to a real row (INV-006/007).
    check(doc->boardOrder.size() == 2, "boardOrder lists both boards");
    check(doc->boardOrderIsPermutation(), "boardOrder is a permutation of the rows");
    check(doc->activeBoard == firstId || doc->activeBoard == secondId,
          "activeBoard is one of the loaded boards");
    check(doc->boardRowIndex(doc->activeBoard) >= 0,
          "activeBoard resolves to a layer row");
    check(doc->boardIdCounter >= secondId,
          "id counter resumes above every loaded id");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef PITTORE_AF
    QCoreApplication app(argc, argv);
    testOneDocumentManyBoards();
#else
    (void)argc;
    (void)argv;
    std::printf("test_af_artboard: skipped (decode needs libzstd)\n");
#endif
    if (failures == 0) {
        std::printf("test_af_artboard: all checks passed\n");
        return 0;
    }
    std::printf("test_af_artboard: %d failures\n", failures);
    return 1;
}
