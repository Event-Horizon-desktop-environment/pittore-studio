// Artboard compositing geometry: the rects that clip a board's subtree to its
// bounds. A board is a Kind::Group row with a nonzero boardId (D1, INV-011), so
// it enters the gather walk as an ordinary container; the only extra rule is
// that its descendants paint only inside boardRect. That geometric clip is done
// on the CPU by narrowing each row's paint bounds (app_state_composite.cpp
// calls boardClipRects/boardPaintRect), so no engine kernel changes and no
// CPU/GPU parity surface. The board *fill* (a solid rect behind the subtree)
// is separate and needs an engine primitive — see the note in
// boardClipRects. Split out (AGENTS.md: one responsibility per file).

#include "ui/app_state_detail.h"

#include <cmath>

namespace pittore::ui {

std::unordered_map<std::uint64_t, QRectF> boardClipRects(const DocumentItem& d,
                                                         const QRect& region) {
    std::unordered_map<std::uint64_t, QRectF> out;
    const QRectF reg(region);
    for (const LayerItem& l : d.layers) {
        if (!l.isBoard()) continue;
        const QRectF r = l.boardRect.intersected(reg);
        if (!r.isEmpty()) out.emplace(l.boardId, r);
    }
    return out;
}

QRect boardPaintRect(const std::unordered_map<std::uint64_t, QRectF>& rects,
                     std::uint64_t boardId) {
    if (boardId == 0) return QRect();  // pasteboard content (D3): no clip
    const auto it = rects.find(boardId);
    if (it == rects.end()) return QRect();  // board outside region: paints nothing
    const QRectF& r = it->second;
    // boardRect edges are whole pixels (quantised at creation, INV-004), so a
    // direct integer rect is exact — no outward rounding that would let a
    // board on a fractional boundary leak a pixel it does not cover.
    return QRect(static_cast<int>(r.left()), static_cast<int>(r.top()),
                 static_cast<int>(r.width()), static_cast<int>(r.height()));
}

}  // namespace pittore::ui
