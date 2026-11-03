// Artboard queries: resolving between board rows, board ids and layer rows.
// A board is a Kind::Group row carrying a nonzero boardId plus the payload (D1,
// INV-011), so structure, visibility, opacity and indent all ride the existing
// group machinery; this file only adds board-specific resolution. The undoable
// mutations live in app_state_board_ops.cpp. Split out of app_state.cpp
// (AGENTS.md: one responsibility per file).

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include <algorithm>

namespace pittore::ui {

namespace {

// Row index of the board row carrying `id`, or -1. Scans the stack (boards are
// few — tens, not thousands). Authoritative over boardOrder: the tree is the
// model, boardOrder is derived (INV-007).
int findBoardRow(const DocumentItem& d, std::uint64_t id) {
    if (id == 0) return -1;
    for (int i = 0; i < d.layers.size(); ++i)
        if (d.layers[i].isBoard() && d.layers[i].boardId == id) return i;
    return -1;
}

// Board rows in stack order (top-first), for the permutation check.
QVector<std::uint64_t> boardRowsInOrder(const DocumentItem& d) {
    QVector<std::uint64_t> rows;
    for (const LayerItem& l : d.layers)
        if (l.isBoard()) rows.append(l.boardId);
    return rows;
}

}  // namespace

int DocumentItem::boardRowIndex(std::uint64_t id) const {
    return findBoardRow(*this, id);
}

std::uint64_t DocumentItem::boardAt(int layerRowIndex) const {
    if (layerRowIndex < 0 || layerRowIndex >= layers.size()) return 0;
    if (layers[layerRowIndex].isBoard()) return layers[layerRowIndex].boardId;
    // Innermost enclosing group that is a board; enclosingGroups is innermost
    // first, so the first board wins (INV-011 nesting is deferred — v1 boards
    // are root-only, so this returns at most one).
    for (int g : enclosingGroups(layerRowIndex))
        if (layers[g].isBoard()) return layers[g].boardId;
    return 0;  // pasteboard content (D3)
}

QVector<int> DocumentItem::boardChildRows(int boardRow) const {
    QVector<int> out;
    if (boardRow < 0 || boardRow >= layers.size() ||
        !layers[boardRow].isBoard())
        return out;
    const int end = layerTokenEnd(layers, boardRow);
    for (int i = boardRow + 1; i <= end && i < layers.size(); ++i) out.append(i);
    return out;
}

bool DocumentItem::boardOrderIsPermutation() const {
    QVector<std::uint64_t> a = boardRowsInOrder(*this), b = boardOrder;
    if (a.size() != b.size()) return false;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

void DocumentItem::boardOwnership(QVector<std::uint64_t>& board) const {
    const int n = layers.size();
    board.resize(n);
    // Indent-stack sweep, the same shape as effectiveVisibility (app_state.cpp):
    // leaders hold strictly decreasing indents, so the stack top is the nearest
    // preceding row with smaller indent. A non-group top shields deeper
    // followers from boards below it — mirroring enclosingGroups' scan-back
    // `break`. Because a board IS a group, tracking the innermost enclosing
    // board is just tracking the nearest enclosing group that isBoard().
    struct Lead {
        int indent;
        int index;
        bool isGroup;
    };
    std::vector<Lead> st;
    st.reserve(16);
    for (int i = 0; i < n; ++i) {
        const LayerItem& l = layers[i];
        const int t = l.indent;
        while (!st.empty() && st.back().indent >= t) st.pop_back();
        std::uint64_t own = 0;
        for (int s = static_cast<int>(st.size()) - 1; s >= 0; --s) {
            if (!st[s].isGroup) break;  // shield: scan-back stops here too
            if (own == 0 && layers[st[s].index].isBoard())
                own = layers[st[s].index].boardId;
        }
        board[i] = l.isBoard() ? l.boardId : own;
        st.push_back(Lead{t, i, l.kind == LayerItem::Kind::Group});
    }
}

}  // namespace pittore::ui
