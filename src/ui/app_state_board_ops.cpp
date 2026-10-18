// Artboard operations: the undoable board mutations on AppState. Each targets
// the active document, does its model work inside one snapshot step, and emits
// boardsChanged + layersChanged on success. A board is a Kind::Group row with a
// nonzero boardId (D1, INV-011), so these reuse the group stack machinery for
// subtree walks. DocumentItem is not a QObject, so signal emission lives here
// (AppState), matching grouping and resizeCanvas. Read-only board queries are
// in app_state_boards.cpp. Split out (AGENTS.md: one responsibility per file).

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include <algorithm>
#include <cmath>

namespace pittore::ui {

namespace {

// Quantise a board rect to whole logical pixels and enforce a positive, finite
// area (INV-003, INV-004). Returns a null rect when the input is not a usable
// board (non-finite, or collapses below one pixel).
QRectF quantisedBoardRect(const QRectF& r) {
    if (!std::isfinite(r.x()) || !std::isfinite(r.y()) ||
        !std::isfinite(r.width()) || !std::isfinite(r.height()))
        return QRectF();
    const double x0 = std::floor(r.x());
    const double y0 = std::floor(r.y());
    const double x1 = std::ceil(r.right());
    const double y1 = std::ceil(r.bottom());
    if (x1 - x0 < 1.0 || y1 - y0 < 1.0) return QRectF();
    return QRectF(x0, y0, x1 - x0, y1 - y0);
}

}  // namespace

std::uint64_t AppState::createBoard(const QRectF& rect, const QString& name) {
    DocumentItem* d = activeDocument();
    if (!d) return 0;
    const QRectF rq = quantisedBoardRect(rect);
    if (rq.isNull()) return 0;

    LayerItem b;
    b.kind = LayerItem::Kind::Group;  // a board is a container group (INV-011)
    b.boardId = ++d->boardIdCounter;  // never reuse (INV-006); 0 reserved
    b.boardRect = rq;
    b.name = name.isEmpty() ? tr("Artboard %1").arg(d->boardIdCounter) : name;
    b.indent = 0;                     // root-only boards in v1 (INV-010)
    b.groupExpanded = true;
    b.visible = true;
    b.locked = false;
    b.opacity = 100;                  // board layer opacity reuses `opacity`
    b.swatch = QColor(0xd8, 0xd8, 0xd8);
    const std::uint64_t id = b.boardId;

    d->beginUndoAction();
    // Top of the stack (index 0), matching how new layers/groups land; the
    // board paints above existing content and is immediately visible.
    d->layers.prepend(b);
    for (int& idx : d->selectedLayers) ++idx;  // keep selection on same rows
    d->selectedLayers.clear();
    d->selectedLayers.append(0);
    d->activeLayer = 0;
    d->boardOrder.prepend(id);        // mirrors tree order, top-to-bottom
    d->activeBoard = id;
    d->commitUndoAction(tr("Artboard"), QStringLiteral("artboard"));

    d->rebuildComposite();
    emit boardsChanged(d);
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return id;
}

bool AppState::deleteBoard(std::uint64_t id, bool keepContent) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0 || d->layers[row].locked) return false;
    const int end = layerTokenEnd(d->layers, row);

    d->beginUndoAction();
    if (keepContent) {
        // Reparent the subtree to the document root, preserving global
        // geometry (INV-013, INV-019): the rows keep their offsets, only the
        // board row goes away, and children de-indent by one level.
        const int base = d->layers[row].indent;
        for (int i = row + 1; i <= end && i < d->layers.size(); ++i)
            d->layers[i].indent = std::max(0, d->layers[i].indent - 1 - base);
        d->layers.removeAt(row);
        normalizeLayerIndents(d->layers);
    } else {
        // Drop the board and its whole subtree.
        for (int i = end; i >= row; --i) d->layers.removeAt(i);
    }
    for (int& idx : d->selectedLayers) {
        if (idx == row) idx = -1;
        else if (idx > row) idx -= (end - row + 1);
    }
    d->selectedLayers.removeAll(-1);
    d->boardOrder.removeAll(id);
    if (d->activeBoard == id)
        d->activeBoard = d->boardOrder.isEmpty() ? 0 : d->boardOrder.first();
    if (d->activeLayer >= d->layers.size()) d->activeLayer = 0;
    d->commitUndoAction(tr("Delete Artboard"), QStringLiteral("delete"));

    d->rebuildComposite();
    emit boardsChanged(d);
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::renameBoard(std::uint64_t id, const QString& name) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0 || d->layers[row].locked) return false;
    if (d->layers[row].name == name) return true;
    d->beginUndoAction();
    d->layers[row].name = name;
    d->commitUndoAction(tr("Rename Artboard"), QStringLiteral("rename"));
    emit boardsChanged(d);
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setBoardRect(std::uint64_t id, const QRectF& rect) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0 || d->layers[row].locked) return false;
    const QRectF rq = quantisedBoardRect(rect);
    if (rq.isNull()) return false;
    if (d->layers[row].boardRect == rq) return true;
    d->beginUndoAction();
    d->layers[row].boardRect = rq;
    d->commitUndoAction(tr("Resize Artboard"), QStringLiteral("artboard"));
    d->rebuildComposite();
    emit boardsChanged(d);
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::moveBoardBy(std::uint64_t id, double dx, double dy) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0 || d->layers[row].locked) return false;
    if (dx == 0.0 && dy == 0.0) return true;
    d->beginUndoAction();
    // A board is a container: moving it moves its subtree (INV-008). Global
    // storage means this is a translation of every child's offset by the same
    // delta; board-local positions are unchanged (INV-002).
    d->layers[row].boardRect.translate(dx, dy);
    const int end = layerTokenEnd(d->layers, row);
    for (int i = row + 1; i <= end && i < d->layers.size(); ++i)
        d->layers[i].offset += QPointF(dx, dy);
    d->commitUndoAction(tr("Move Artboard"), QStringLiteral("move"));
    d->rebuildComposite();
    emit boardsChanged(d);
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setBoardFill(std::uint64_t id, bool hasFill, const QColor& fill,
                            double fillOpacity) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0 || d->layers[row].locked) return false;
    fillOpacity = std::clamp(fillOpacity, 0.0, 1.0);
    if (d->layers[row].boardHasFill == hasFill &&
        d->layers[row].boardFill == fill &&
        d->layers[row].boardFillOpacity == fillOpacity)
        return true;
    d->beginUndoAction();
    d->layers[row].boardHasFill = hasFill;
    d->layers[row].boardFill = fill;
    d->layers[row].boardFillOpacity = fillOpacity;
    d->commitUndoAction(tr("Artboard Appearance"), QStringLiteral("fill"));
    d->rebuildComposite();
    emit boardsChanged(d);
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setBoardVisible(std::uint64_t id, bool visible) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0) return false;
    if (d->layers[row].visible == visible) return true;
    d->beginUndoAction();
    d->layers[row].visible = visible;  // hidden board hides subtree (INV-023)
    d->commitUndoAction(visible ? tr("Show Artboard") : tr("Hide Artboard"),
                        QStringLiteral("visibility"));
    emit regionModified(d, d->layers[row].boardRect);
    emit boardsChanged(d);
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setBoardLocked(std::uint64_t id, bool locked) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    const int row = d->boardRowIndex(id);
    if (row < 0) return false;
    if (d->layers[row].locked == locked) return true;
    d->beginUndoAction();
    d->layers[row].locked = locked;  // locked rejects mutation (INV-023)
    d->commitUndoAction(locked ? tr("Lock Artboard") : tr("Unlock Artboard"),
                        QStringLiteral("lock"));
    emit boardsChanged(d);
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::reorderBoards(int from, int to) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    if (from < 0 || from >= d->boardOrder.size() || to < 0 ||
        to >= d->boardOrder.size() || from == to)
        return false;
    d->beginUndoAction();
    const std::uint64_t id = d->boardOrder.takeAt(from);
    d->boardOrder.insert(to, id);
    // Mirror the reorder in the tree so paint order and the index agree: move
    // the board row (and its subtree) to the matching stack position.
    const int srcRow = d->boardRowIndex(id);
    if (srcRow >= 0) {
        const int end = layerTokenEnd(d->layers, srcRow);
        QVector<LayerItem> block;
        for (int i = srcRow; i <= end && i < d->layers.size(); ++i)
            block.append(d->layers[i]);
        for (int i = end; i >= srcRow; --i) d->layers.removeAt(i);
        // Recompute the destination row now that indices shifted.
        const std::uint64_t anchorId =
            (to < d->boardOrder.size()) ? d->boardOrder[to] : 0;
        int dst = (to == d->boardOrder.size() - 1) ? 0 : d->boardRowIndex(anchorId);
        if (dst < 0) dst = 0;
        for (int i = 0; i < block.size(); ++i) d->layers.insert(dst + i, block[i]);
        normalizeLayerIndents(d->layers);
    }
    d->commitUndoAction(tr("Reorder Artboards"), QStringLiteral("reorder"));
    d->rebuildComposite();
    emit boardsChanged(d);
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::setActiveBoard(std::uint64_t id) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    if (id != 0 && d->boardRowIndex(id) < 0) return false;
    if (d->activeBoard == id) return true;
    d->activeBoard = id;  // view state: no undo step (INV-014 keeps it sane)
    emit boardsChanged(d);
    return true;
}

}  // namespace pittore::ui
