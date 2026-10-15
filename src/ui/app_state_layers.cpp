// Layer placement and selection: what the canvas and the Layers panel drive
// during a drag, plus the selection set the rest of the stack operations
// read. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/tools/log/tool_log.h"

#include <algorithm>
#include <cmath>

namespace pittore::ui {

bool AppState::setActiveLayerPlacement(const QPointF& offset, double scaleX,
                                       double scaleY) {
    DocumentItem* d = activeDocument();
    LayerItem* l = activeLayer();
    if (!d || !l || l->kind != LayerItem::Kind::Pixel || l->locked)
        return false;
    if (!qIsFinite(offset.x()) || !qIsFinite(offset.y()) ||
        !qIsFinite(scaleX) || !qIsFinite(scaleY))
        return false;
    scaleX = qBound(0.01, scaleX, 100.0);
    scaleY = qBound(0.01, scaleY, 100.0);
    if (l->offset == offset && l->scaleX == scaleX && l->scaleY == scaleY)
        return true;
    // Moved/scaled layers re-composite only the union of the old and new
    // bounds — the incremental path, not a full rebuild. The halo covers the
    // resampling footprint: a doc pixel up to one source texel outside the
    // bounds still samples the layer edge (up to max(scale) document pixels).
    const QRectF oldBounds = layerBounds(*d, *l);
    const double halo =
        std::ceil(std::max({l->scaleX, l->scaleY, scaleX, scaleY})) + 1.0;
    if (l->liveText) {
        // Text keeps its layout origin as the source of truth: a move shifts
        // the origin. (Corner-resize of text goes through setLiveTextSize so it
        // re-renders at the new size instead of resampling; a placement call
        // that reaches here is a move, so scale is not applied.)
        l->textSpec.origin += offset - l->offset;
        scaleX = scaleY = 1.0;
    }
    l->offset = offset;
    l->scaleX = scaleX;
    l->scaleY = scaleY;
    syncLinkedMaskPlacement(*l);
    d->dirty = true;
    // The layers-panel preview fits the layer's content bounds, so a move or
    // scale leaves it unchanged — no preview invalidation on this hot path.
    const QRect region = oldBounds.united(layerBounds(*d, *l))
                             .adjusted(-halo, -halo, halo, halo)
                             .toAlignedRect()
                             .intersected(QRect(QPoint(0, 0), d->size));
    d->renderRegion(region);
    // Region-scoped notice: the canvas repaints only the moved strip (old ∪
    // new bounds), not the whole frame — that full repaint per drag frame was
    // the "everything lags while dragging" cost in the paint log.
    noteRegionEdit(QRectF(region));
    emit regionModified(d, QRectF(region));
    return true;
}

void AppState::removeActiveLayer() {
    // Selection-aware twin (menu, panel trash and the Del key all use
    // removeSelectedLayers directly; this stays for API compatibility/tests).
    removeSelectedLayers();
}

// --- layer selection -------------------------------------------------------

void AppState::selectAllLayers() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return;
    d->selectedLayers.clear();
    for (int i = 0; i < d->layers.size(); ++i) d->selectedLayers.push_back(i);
    emit activeLayerChanged();
}

void AppState::clearLayerSelection() {
    DocumentItem* d = activeDocument();
    if (!d || d->selectedLayers.isEmpty()) return;
    d->selectedLayers.clear();
    emit activeLayerChanged();
}

void AppState::selectLayerRange(int toIndex, bool add) {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return;
    toIndex = qBound(0, toIndex, d->layers.size() - 1);
    const int anchor = qBound(0, d->activeLayer, d->layers.size() - 1);
    const int lo = qMin(anchor, toIndex);
    const int hi = qMax(anchor, toIndex);
    if (!add) {
        d->selectedLayers.clear();
        for (int i = lo; i <= hi; ++i) d->selectedLayers.push_back(i);
    } else {
        // Union the range with the existing set, kept in stack order.
        for (int i = lo; i <= hi; ++i)
            if (!d->selectedLayers.contains(i)) d->selectedLayers.push_back(i);
        std::sort(d->selectedLayers.begin(), d->selectedLayers.end());
    }
    d->activeLayer = toIndex;
    emit activeLayerChanged();
}

bool AppState::setLayersVisible(const QVector<int>& indices, bool visible) {
    DocumentItem* d = activeDocument();
    if (!d || indices.isEmpty()) return false;
    // Dedupe + clamp to the live stack.
    QVector<int> ids;
    ids.reserve(indices.size());
    for (int idx : indices) {
        if (idx < 0 || idx >= d->layers.size()) continue;
        if (!ids.contains(idx)) ids.push_back(idx);
    }
    if (ids.isEmpty()) return false;
    bool any = false;
    for (int idx : ids) {
        if (d->layers[idx].visible != visible) {
            any = true;
            break;
        }
    }
    if (!any) return false;
    beginUndoStep();
    for (int idx : ids) {
        d->layers[idx].visible = visible;
        // Showing a layer packed behind the flattened base expands its pixels
        // (hiding never does — a hidden layer stays packed). The undo snapshot
        // taken above still holds the packed form, so undo repacks it.
        if (visible) materializeLayerPixels(d->layers[idx]);
    }
    // Union of footprints: groups contribute their whole pixel subtree bounds
    // (an empty group contributes nothing — never a full-canvas recomposite).
    QRectF unionSb;
    for (int idx : ids) {
        const LayerItem& l = d->layers[idx];
        const QRectF b = (l.kind == LayerItem::Kind::Group)
                             ? groupBounds(idx)
                             : stageBounds(*d, l);
        unionSb |= b;
    }
    unionSb = unionSb.intersected(QRectF(QPointF(0, 0), QSizeF(d->size)));
    if (unionSb.isEmpty()) {
        d->rebuildComposite();
    } else {
        d->recompositeRegion(unionSb.toAlignedRect());
    }
    const QString undoName =
        ids.size() == 1
            ? tr("%1 \"%2\"").arg(visible ? tr("Show") : tr("Hide"),
                                  d->layers[ids.first()].name)
            : tr("%1 %2 layers").arg(visible ? tr("Show") : tr("Hide"))
                                .arg(ids.size());
    commitUndoStep(undoName, QStringLiteral("eye"));
    if (unionSb.isEmpty()) {
        emit documentModified(d);
    } else {
        noteRegionEdit(unionSb);
        emit regionModified(d, unionSb);
    }
    return true;
}

QVector<int> AppState::selectedLayerIndices() const {
    QVector<int> out;
    const DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return out;
    out = d->selectedLayers;
    if (out.isEmpty()) out.push_back(d->activeLayer);
    for (int i = out.size() - 1; i >= 0; --i)
        if (out.at(i) < 0 || out.at(i) >= d->layers.size()) out.removeAt(i);
    return out;
}


void AppState::removeSelectedLayers() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return;
    QVector<int> kill = selectedLayerIndices();
    if (kill.isEmpty()) return;
    std::sort(kill.begin(), kill.end());
    d->beginUndoAction();
    // Keep the selection's active layer sensible as indices shift: a removed
    // layer below it decrements it; removing the active layer keeps the one
    // above (which slides into its slot). Deleting every layer is allowed —
    // the document may be empty; activeLayer is clamped to 0 and ignored by
    // every consumer that checks layers.isEmpty() first.
    int newActive = d->activeLayer;
    for (int idx : kill) {
        if (idx < newActive) --newActive;
    }
    for (auto it = kill.crbegin(); it != kill.crend(); ++it) d->layers.removeAt(*it);
    d->selectedLayers.clear();
    d->activeLayer =
        d->layers.isEmpty() ? 0 : qBound(0, newActive, d->layers.size() - 1);
    d->commitUndoAction(tr("Delete Layer"), QStringLiteral("trash"));
    d->rebuildComposite();
    emit layersChanged();
    emit historyChanged();
    emit documentModified(d);
}

bool AppState::moveSelectedLayers(const QPointF& delta, const QVector<QPointF>& starts) {
    return moveLayersAt(selectedLayerIndices(), delta, starts);
}

bool AppState::moveLayersAt(const QVector<int>& indices, const QPointF& delta,
                            const QVector<QPointF>& starts) {
    DocumentItem* d = activeDocument();
    if (!d || delta.isNull()) return false;
    if (!qIsFinite(delta.x()) || !qIsFinite(delta.y())) return false;
    if (indices.isEmpty()) return false;
    const bool useStarts = starts.size() == indices.size();
    QRect region;
    bool moved = false;
    // Visual-split detector. The composite is drawn from
    // layerDrawSource().offset, not from l->offset directly, so a path that
    // leaves the draw placement behind (a stale styled/mask bake) moves the
    // model and the pixels by different amounts — a jump that no move/drag
    // line, which only reads the model, can ever show. A style bake sits a
    // constant `grow` away from the model origin, so the absolute positions
    // are allowed to differ; the *deltas* are not.
    bool split = false;
    QPointF splitModel, splitDraw;
    QPointF off;   // last layer's model placement, for the trace below
    for (int i = 0; i < indices.size(); ++i) {
        const int idx = indices.at(i);
        if (idx < 0 || idx >= d->layers.size()) continue;
        LayerItem* l = &d->layers[idx];
        if (l->kind != LayerItem::Kind::Pixel || l->locked || !l->pixels) continue;
        const QRectF oldBounds = layerBounds(*d, *l);
        const QPointF drawFrom = layerDrawSource(*l).offset;
        // Absolute placement: start + delta (gesture-baseline compatible).
        const QPointF from = l->offset;
        const QPointF to = (useStarts ? starts.at(i) : l->offset) + delta;
        l->offset = to;
        syncLinkedMaskPlacement(*l);
        // The layout origin tracks the placement step, not the gesture
        // delta: `delta` is the whole press-to-cursor distance and this runs
        // once per move event, so adding it would walk the origin away from
        // the pixels quadratically (the next re-render then drops the run
        // off the canvas).
        if (l->liveText) l->textSpec.origin += to - from;
        const QPointF drawTo = layerDrawSource(*l).offset;
        if (!split &&
            (std::abs((to - from).x() - (drawTo - drawFrom).x()) > 0.01 ||
             std::abs((to - from).y() - (drawTo - drawFrom).y()) > 0.01)) {
            split = true;
            splitModel = to - from;
            splitDraw = drawTo - drawFrom;
        }
        off = to;
        const QRectF newBounds = layerBounds(*d, *l);
        const double halo = std::ceil(std::max({l->scaleX, l->scaleY})) + 1.0;
        region |= oldBounds.adjusted(-halo, -halo, halo, halo).toAlignedRect();
        region |= newBounds.adjusted(-halo, -halo, halo, halo).toAlignedRect();
        moved = true;
    }
    if (!moved) return false;
    d->dirty = true;
    region = region.intersected(QRect(QPoint(0, 0), d->size));
    if (region.isEmpty()) {
        // The model moved but nothing intersects the document: neither the
        // composite nor regionModified runs, so the screen keeps the previous
        // frame while the layer sits somewhere new. Silent here would be a
        // purely visual jump, so name it.
        tool_logf(activeTool(), "move/no-render",
                  "off=(%.1f,%.1f) delta=(%.1f,%.1f) region-empty doc=%dx%d",
                  off.x(), off.y(), delta.x(), delta.y(), d->size.width(),
                  d->size.height());
        return true;
    }
    // renderRegion() bumps DocumentItem::revision only when it actually
    // recomposited — it returns past the bump when the region clips away or
    // the tone path bails. An unchanged revision means the model moved and
    // the composite did not.
    const std::uint64_t revBefore = d->revision;
    d->renderRegion(region);
    if (d->revision == revBefore)
        tool_logf(activeTool(), "render/SKIP",
                  "off=(%.1f,%.1f) delta=(%.1f,%.1f) region=(%d,%d %dx%d) "
                  "rev=%llu",
                  off.x(), off.y(), delta.x(), delta.y(), region.x(),
                  region.y(), region.width(), region.height(),
                  static_cast<unsigned long long>(d->revision));
    if (split)
        tool_logf(activeTool(), "render/SPLIT",
                  "off=(%.1f,%.1f) model=(%+.2f,%+.2f) draw=(%+.2f,%+.2f) "
                  "region=(%d,%d %dx%d)",
                  off.x(), off.y(), splitModel.x(), splitModel.y(),
                  splitDraw.x(), splitDraw.y(), region.x(), region.y(),
                  region.width(), region.height());
    noteRegionEdit(QRectF(region));
    emit regionModified(d, QRectF(region));
    return true;
}

bool AppState::setLayerPlacements(const QVector<int>& indices,
                                  const QVector<QPointF>& offsets,
                                  const QVector<double>& scaleXs,
                                  const QVector<double>& scaleYs) {
    DocumentItem* d = activeDocument();
    if (!d || indices.isEmpty()) return false;
    if (offsets.size() != indices.size() || scaleXs.size() != indices.size() ||
        scaleYs.size() != indices.size())
        return false;
    QRect region;
    bool moved = false;
    for (int i = 0; i < indices.size(); ++i) {
        const int idx = indices.at(i);
        if (idx < 0 || idx >= d->layers.size()) continue;
        LayerItem* l = &d->layers[idx];
        if (l->kind != LayerItem::Kind::Pixel || l->locked || !l->pixels) continue;
        const QPointF offset = offsets.at(i);
        double sx = qBound(0.01, scaleXs.at(i), 100.0);
        double sy = qBound(0.01, scaleYs.at(i), 100.0);
        if (!qIsFinite(offset.x()) || !qIsFinite(offset.y())) continue;
        if (l->offset == offset && l->scaleX == sx && l->scaleY == sy) continue;
        const QRectF oldBounds = layerBounds(*d, *l);
        if (l->liveText) {
            // Text keeps its layout origin as the source of truth; a placement
            // that reaches here is a move, so the scale stays 1.0.
            l->textSpec.origin += offset - l->offset;
            sx = sy = 1.0;
        }
        l->offset = offset;
        l->scaleX = sx;
        l->scaleY = sy;
        syncLinkedMaskPlacement(*l);
        const QRectF newBounds = layerBounds(*d, *l);
        const double halo = std::ceil(std::max({l->scaleX, l->scaleY})) + 1.0;
        region |= oldBounds.adjusted(-halo, -halo, halo, halo).toAlignedRect();
        region |= newBounds.adjusted(-halo, -halo, halo, halo).toAlignedRect();
        moved = true;
    }
    if (!moved) return false;
    d->dirty = true;
    region = region.intersected(QRect(QPoint(0, 0), d->size));
    if (!region.isEmpty()) {
        d->renderRegion(region);
        noteRegionEdit(QRectF(region));
        emit regionModified(d, QRectF(region));
    }
    return true;
}

// --- layer grouping and reordering ------------------------------------------

QVector<int> AppState::fxTargetLayers() const {
    QVector<int> out;
    const DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return out;
    QSet<int> unit;
    for (int idx : selectedLayerIndices()) {
        if (idx < 0 || idx >= d->layers.size()) continue;
        unit.insert(idx);
        // A group folder contributes its whole subtree (nested groups
        // included — the indent walk keeps descending).
        if (d->layers[idx].kind == LayerItem::Kind::Group) {
            const int end = layerTokenEnd(d->layers, idx);
            for (int j = idx + 1; j <= end; ++j) unit.insert(j);
        }
    }
    QVector<int> sorted = unit.values();
    std::sort(sorted.begin(), sorted.end());
    for (int idx : sorted) {
        const LayerItem& l = d->layers[idx];
        if (l.kind == LayerItem::Kind::Pixel && l.pixels) out.push_back(idx);
    }
    return out;
}


int AppState::groupSelectedLayers() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.size() < 2) return -1;
    int base = 0;
    QVector<int> unit = layerMoveUnit(d, &base);
    // Locked rows (the Background) never join a group — Select All Layers
    // includes them, but they must stay outside, pinned at the bottom.
    for (int i = unit.size() - 1; i >= 0; --i)
        if (d->layers[unit[i]].locked) unit.removeAt(i);
    if (unit.size() < 2) return -1;
    // Re-derive the base indent from the filtered unit (locked rows can't pull
    // the new header deeper than its members).
    base = d->layers[unit.first()].indent;
    for (int idx : unit) base = qMin(base, d->layers[idx].indent);

    // Gather the unit rows in panel order.
    QVector<LayerItem> moved;
    moved.reserve(unit.size());
    for (int idx : unit) moved.append(d->layers[idx]);

    // Rebuild: rows not in the unit stay put; the new group header plus the
    // re-indented unit rows land at the first unit row's slot.
    QVector<LayerItem> stack;
    stack.reserve(d->layers.size() + 1);
    int groupIndex = -1;
    bool placed = false;
    for (int i = 0; i < d->layers.size(); ++i) {
        if (unit.contains(i)) {
            if (!placed) {
                LayerItem grp;
                grp.kind = LayerItem::Kind::Group;
                grp.name = tr("Group %1").arg(d->layers.size());
                grp.indent = base;
                grp.groupExpanded = true;
                grp.visible = true;
                grp.opacity = 100;
                groupIndex = stack.size();
                stack.append(grp);
                for (const LayerItem& m : moved) {
                    LayerItem child = m;
                    child.indent = base + 1 + (m.indent - base);
                    stack.append(child);
                }
                placed = true;
            }
            continue;   // unit rows were emitted with the group header
        }
        stack.append(d->layers[i]);
    }

    // Selections gathered across nesting levels re-indent with gaps (a child
    // moved without its header jumps more than one level, or lands behind a
    // pixel row). Repair those before committing so the group token and the
    // ancestor scans agree on what the new header owns.
    normalizeLayerIndents(stack);
    d->beginUndoAction();
    d->layers = stack;
    d->selectedLayers.clear();
    d->selectedLayers.push_back(groupIndex);
    d->activeLayer = groupIndex;
    d->commitUndoAction(tr("Group Layers"), QStringLiteral("group"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    emit groupCreated(groupIndex);
    return groupIndex;
}

int AppState::makeToneBlendGroup() {
    DocumentItem* d = activeDocument();
    if (!d) return -1;
    int g = groupSelectedLayers();
    if (g < 0 || g >= d->layers.size()) {
        // Single-layer path: groupSelectedLayers needs two or more rows,
        // but the common case is one image re-graded against the composite
        // beneath it — wrap the lone selected (else active) layer, carrying
        // its whole subtree so nested children stay inside.
        int target = -1;
        if (d->selectedLayers.size() == 1)
            target = d->selectedLayers.first();
        else
            target = d->activeLayer;
        if (target < 0 || target >= d->layers.size()) {
            setStatusHint(tr("Tone Blend Group needs a layer selected."));
            return -1;
        }
        if (d->layers[target].locked) {
            setStatusHint(tr("The Background cannot join a Tone Blend Group."));
            return -1;
        }
        if (d->layers[target].kind == LayerItem::Kind::Group &&
            d->layers[target].toneBlendGroup)
            return target;  // already one; the caller opens its settings
        const int base = d->layers[target].indent;
        const int end = layerTokenEnd(d->layers, target);
        QVector<LayerItem> stack;
        stack.reserve(d->layers.size() + 1);
        for (int i = 0; i < target; ++i) stack.append(d->layers[i]);
        LayerItem grp;
        grp.kind = LayerItem::Kind::Group;
        grp.name = tr("Group %1").arg(d->layers.size());
        grp.indent = base;
        grp.groupExpanded = true;
        grp.visible = true;
        grp.opacity = 100;
        const int single = stack.size();
        stack.append(grp);
        for (int i = target; i <= end; ++i) {
            LayerItem child = d->layers[i];
            child.indent = base + 1 + (child.indent - base);
            stack.append(child);
        }
        for (int i = end + 1; i < d->layers.size(); ++i)
            stack.append(d->layers[i]);
        normalizeLayerIndents(stack);
        d->beginUndoAction();
        d->layers = stack;
        d->selectedLayers.clear();
        d->selectedLayers.push_back(single);
        d->activeLayer = single;
        d->commitUndoAction(tr("Group Layers"), QStringLiteral("group"));
        d->rebuildComposite();
        emit layersChanged();
        emit activeLayerChanged();
        emit historyChanged();
        emit documentModified(d);
        emit groupCreated(single);
        g = single;
    }
    LayerItem& header = d->layers[g];
    if (header.kind != LayerItem::Kind::Group) return -1;
    // Auto content type from the first child: vector
    // shapes first, then live/recovered text, else a plain image group.
    int contentType = 0;
    for (int i = g + 1; i < d->layers.size(); ++i) {
        if (d->layers[i].indent <= header.indent) break;
        const LayerItem& child = d->layers[i];
        if (child.kind == LayerItem::Kind::Shape)
            contentType = 1;
        else if (child.kind == LayerItem::Kind::Text || child.isText)
            contentType = 2;
        break;  // first child decides
    }
    beginUndoStep();
    // NOTE: re-acquire the header AFTER the snapshot. The reference above
    // aliases the layers buffer; QVector shares that buffer with the
    // snapshot, so writing through the stale reference would mutate the
    // snapshot in place (no detach) and undo could never revert the flag.
    LayerItem& fresh = d->layers[g];
    fresh.toneBlendGroup = true;
    fresh.toneBlend = pittore::compute::ToneBlendParams();
    fresh.toneBlend.contentType = contentType;
    fresh.name = tr("Tone Blend Group");
    d->rebuildComposite();
    commitUndoStep(tr("Tone Blend Group"), QStringLiteral("tone-blend"));
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return g;
}

bool AppState::ungroupSelectedLayers() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return false;

    // Explode every selected group; with no group selected, explode the active
    // layer's nearest ancestor group.
    struct Span { int start, end; };
    QVector<Span> spans;
    QSet<int> killHeaders;
    const QVector<int> sel = selectedLayerIndices();
    for (int idx : sel) {
        if (idx >= 0 && idx < d->layers.size() &&
            d->layers[idx].kind == LayerItem::Kind::Group) {
            const int end = layerTokenEnd(d->layers, idx);
            spans.append({idx, end});
            killHeaders.insert(idx);
        }
    }
    if (spans.isEmpty() && !sel.isEmpty()) {
        const int active = qBound(0, d->activeLayer, d->layers.size() - 1);
        for (int i = active - 1; i >= 0; --i) {
            if (d->layers[i].kind == LayerItem::Kind::Group &&
                d->layers[i].indent < d->layers[active].indent) {
                spans.append({i, layerTokenEnd(d->layers, i)});
                killHeaders.insert(i);
                break;
            }
        }
    }
    if (spans.isEmpty()) return false;

    QVector<LayerItem> stack;
    stack.reserve(d->layers.size());
    QVector<int> newSel;   // new-stack indices of the promoted rows
    for (int i = 0; i < d->layers.size(); ++i) {
        if (killHeaders.contains(i)) continue;   // drop the group header
        bool inside = false;
        for (const Span& s : spans)
            if (i > s.start && i <= s.end) { inside = true; break; }
        LayerItem l = d->layers[i];
        if (inside) {
            l.indent = qMax(0, l.indent - 1);
            newSel.append(stack.size());
        }
        stack.append(l);
    }

    // Promoting children can leave a row deeper than its new predecessor
    // allows; repair so the remaining groups keep owning their rows.
    normalizeLayerIndents(stack);
    // newSel holds positions, which normalization never reorders.
    d->beginUndoAction();
    d->layers = stack;
    d->selectedLayers = newSel;
    d->activeLayer = newSel.isEmpty() ? 0 : newSel.first();
    d->commitUndoAction(tr("Ungroup Layers"), QStringLiteral("group"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

void AppState::toggleLayerGroupExpanded(int groupIndex, bool subtree,
                                        bool allGroups) {
    DocumentItem* d = activeDocument();
    if (!d || groupIndex < 0 || groupIndex >= d->layers.size() ||
        d->layers[groupIndex].kind != LayerItem::Kind::Group)
        return;
    if (allGroups) {
        // Ctrl/Cmd+chevron: flip every group row in the document.
        for (LayerItem& l : d->layers)
            if (l.kind == LayerItem::Kind::Group)
                l.groupExpanded = !l.groupExpanded;
    } else if (subtree) {
        // Alt/Option+chevron: flip this group and every group nested under it.
        const int base = d->layers[groupIndex].indent;
        for (int i = groupIndex; i < d->layers.size(); ++i) {
            LayerItem& l = d->layers[i];
            if (i > groupIndex && l.indent <= base) break;
            if (l.kind == LayerItem::Kind::Group) l.groupExpanded = !l.groupExpanded;
        }
    } else {
        d->layers[groupIndex].groupExpanded =
            !d->layers[groupIndex].groupExpanded;
    }
    // UI-state toggle (not undoable): the panel
    // rebuilds its row set from groupExpanded via rowHiddenByCollapsedGroup.
    emit layersChanged();
}

void AppState::setAllGroupsExpanded(bool expanded) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    bool changed = false;
    for (LayerItem& l : d->layers)
        if (l.kind == LayerItem::Kind::Group && l.groupExpanded != expanded) {
            l.groupExpanded = expanded;
            changed = true;
        }
    if (changed) emit layersChanged();
}

bool AppState::moveSelectedLayersInStack(int step) {
    if (step == 0) return false;
    DocumentItem* d = activeDocument();
    if (!d || d->layers.size() < 2) return false;

    // The block to move: the active layer, expanded to its subtree when it is a
    // group. Multi-row selections move one active block at a time.
    const int active = qBound(0, d->activeLayer, d->layers.size() - 1);
    const int top = active;
    const int end = d->layers[active].kind == LayerItem::Kind::Group
                        ? layerTokenEnd(d->layers, active)
                        : active;
    const int n = d->layers.size();
    if (step < 0) {
        if (top == 0) return false;
        // Locked rows and the Background never move.
        if (d->layers[top].locked || d->layers[top - 1].locked) return false;
        // A child row never escapes above its container's header.
        if (d->layers[top - 1].indent < d->layers[top].indent) return false;
        // Swap the block with the single row directly above (the row above a
        // valid non-overlapping token is always a leaf: any deeper subtree row
        // would overlap the block and be rejected above).
        QVector<LayerItem> stack = d->layers;
        const LayerItem above = stack.takeAt(top - 1);
        stack.insert(end, above);   // after the shift the block occupies [top-1, end-1]
        d->beginUndoAction();
        d->layers = stack;
        d->selectedLayers = QVector<int>{top - 1};
        d->activeLayer = top - 1;
        d->commitUndoAction(tr("Bring Forward"), QStringLiteral("group"));
        d->rebuildComposite();
        emit layersChanged();
        emit activeLayerChanged();
        emit historyChanged();
        emit documentModified(d);
        return true;
    }
    if (end >= n - 1) return false;
    if (d->layers[top].locked || d->layers[end + 1].locked) return false;
    if (d->layers[end + 1].indent < d->layers[top].indent) return false;
    QVector<LayerItem> stack = d->layers;
    const LayerItem below = stack.takeAt(end + 1);
    stack.insert(top, below);   // the block shifts down one slot
    d->beginUndoAction();
    d->layers = stack;
    d->selectedLayers = QVector<int>{top + 1};
    d->activeLayer = top + 1;
    d->commitUndoAction(tr("Send Backward"), QStringLiteral("group"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::bringSelectedLayersToFront() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return false;
    const int active = qBound(0, d->activeLayer, d->layers.size() - 1);
    const int end = d->layers[active].kind == LayerItem::Kind::Group
                        ? layerTokenEnd(d->layers, active)
                        : active;
    if (d->layers[active].locked) return false;
    // Destination: the very top of the movable stack (below any locked run
    // pinned at the top). A locked Background normally sits at the bottom, but
    // a locked row at index 0 must not be dragged out from under.
    const int n = d->layers.size();
    int pos = 0;
    while (pos < n && d->layers[pos].locked) ++pos;
    if (pos >= active) return false;   // already at/behind the destination
    QVector<LayerItem> stack;
    stack.reserve(n);
    for (int i = 0; i < pos; ++i) stack.append(d->layers[i]);
    for (int i = active; i <= end; ++i) stack.append(d->layers[i]);
    for (int i = pos; i < n; ++i) {
        if (i >= active && i <= end) continue;
        stack.append(d->layers[i]);
    }
    d->beginUndoAction();
    d->layers = stack;
    d->selectedLayers = QVector<int>{pos};
    d->activeLayer = pos;
    d->commitUndoAction(tr("Bring to Front"), QStringLiteral("group"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::sendSelectedLayersToBack() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return false;
    const int active = qBound(0, d->activeLayer, d->layers.size() - 1);
    const int end = d->layers[active].kind == LayerItem::Kind::Group
                        ? layerTokenEnd(d->layers, active)
                        : active;
    if (d->layers[active].locked) return false;
    // Destination: the bottom of the movable stack — just above the trailing
    // locked run (the Background stays pinned at the bottom).
    const int n = d->layers.size();
    int pos = n;
    while (pos > 0 && d->layers[pos - 1].locked) --pos;
    if (pos <= end) return false;   // already at/behind the destination
    QVector<LayerItem> stack;
    stack.reserve(n);
    for (int i = 0; i < pos; ++i) {
        if (i >= active && i <= end) continue;
        stack.append(d->layers[i]);
    }
    for (int i = active; i <= end; ++i) stack.append(d->layers[i]);
    for (int i = pos; i < n; ++i) stack.append(d->layers[i]);
    const int newTop = pos - (end - active + 1);
    d->beginUndoAction();
    d->layers = stack;
    d->selectedLayers = QVector<int>{newTop};
    d->activeLayer = newTop;
    d->commitUndoAction(tr("Send to Back"), QStringLiteral("group"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::reparentSelectedLayers(int targetIndex, int targetIndent) {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return false;
    int base = 0;
    QVector<int> unit = layerMoveUnit(d, &base);
    // Locked rows (the Background) are never dragged, even when they are part
    // of the selection — Select All + drag moves everything else.
    for (int i = unit.size() - 1; i >= 0; --i)
        if (d->layers[unit[i]].locked) unit.removeAt(i);
    if (unit.isEmpty()) return false;
    base = d->layers[unit.first()].indent;
    for (int idx : unit) base = qMin(base, d->layers[idx].indent);
    // A locked row (the Background) is never dragged.
    if (d->layers[unit.first()].locked) return false;

    // Convert the pre-move target row index into an insertion slot in the
    // stack-minus-unit. targetIndex == size means "append at the end". The
    // caret is clamped to the bottom of the MOVABLE stack so a drop below the
    // trailing locked run (the Background) lands just above it instead of
    // shoving the locked row upward.
    const int n = d->layers.size();
    int movableEnd = n;
    while (movableEnd > 0 && d->layers[movableEnd - 1].locked) --movableEnd;
    int slot = 0;
    const int t = qBound(0, qMin(targetIndex, movableEnd), movableEnd);
    for (int i = 0; i < t; ++i)
        if (!unit.contains(i)) ++slot;
    // If the caret sits inside the dragged unit's span (other than its own
    // top-left boundary — a no-op), refuse: dropping a group onto itself must
    // not loop. `unit.contains(t) && t != unit.first()` — the panel hands us
    // the unit's header index for a caret above it and a child index for "into
    // the group".
    const int maxSlot = n - unit.size();
    if (slot > maxSlot || (t < n && unit.contains(t) && t != unit.first()))
        return false;
    slot = qBound(0, slot, maxSlot);
    targetIndent = qMax(0, targetIndent);

    QVector<LayerItem> stack;
    stack.reserve(n);
    QVector<int> movedNew;   // new-stack indices of the moved rows
    int idx = 0;
    for (int i = 0; i < d->layers.size(); ++i) {
        if (unit.contains(i)) continue;
        if (idx == slot) {
            for (int u : unit) {
                LayerItem l = d->layers[u];
                l.indent = targetIndent + (l.indent - base);
                movedNew.append(stack.size());
                stack.append(l);
            }
        }
        stack.append(d->layers[i]);
        ++idx;
    }
    if (slot == maxSlot) {
        for (int u : unit) {
            LayerItem l = d->layers[u];
            l.indent = targetIndent + (l.indent - base);
            movedNew.append(stack.size());
            stack.append(l);
        }
    }

    // Drops across nesting levels re-indent with gaps just like grouping;
    // repair so moved rows land inside the group they were dropped into.
    // movedNew holds positions, which normalization never reorders.
    normalizeLayerIndents(stack);
    d->beginUndoAction();
    d->layers = stack;
    d->selectedLayers = movedNew;
    d->activeLayer = movedNew.isEmpty() ? 0 : movedNew.first();
    d->commitUndoAction(tr("Move Layer"), QStringLiteral("group"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

bool AppState::duplicateSelectedLayers() {
    DocumentItem* d = activeDocument();
    if (!d || d->layers.isEmpty()) return false;
    int base = 0;
    QVector<int> unit = layerMoveUnit(d, &base);
    if (unit.isEmpty()) return false;

    // Split the unit into contiguous runs so each run can be copied directly
    // above itself (a group header plus its subtree stays one run, keeping
    // children names intact).
    struct Run { int start; int len; };
    QVector<Run> runs;
    for (int idx : unit) {
        if (!runs.isEmpty() &&
            idx == runs.last().start + runs.last().len) {
            ++runs.last().len;
        } else {
            runs.append({idx, 1});
        }
    }
    if (runs.isEmpty()) return false;

    const QSet<int> unitSet(unit.begin(), unit.end());
    // Snapshot the sources first: names and parent checks run in pre-move
    // index space, before any insertion shifts the rows below.
    const QVector<LayerItem> srcLayers = d->layers;
    struct RunCopies {
        int start = 0;
        QVector<LayerItem> copies;
    };
    QVector<RunCopies> prepared;
    for (const Run& run : runs) {
        RunCopies rc;
        rc.start = run.start;
        for (int k = 0; k < run.len; ++k) {
            LayerItem copy = srcLayers[run.start + k];
            // Deep-copy pixels: the duplicate must not share its Image with
            // the source, or painting one would corrupt the other's undo
            // snapshots (which share Images by design + copy-on-write).
            if (copy.pixels)
                copy.pixels =
                    std::make_shared<pittore::Image>(copy.pixels->clone());
            // Masks travel with their layer: clone the coverage too, or the
            // duplicate would paint its source's undo snapshots.
            if (copy.mask)
                copy.mask =
                    std::make_shared<pittore::Image>(copy.mask->clone());
            // Drop cached renders so the new row re-renders from its own
            // content instead of sharing the source's raster/thumbnail.
            copy.styled.reset();
            copy.styledValid = false;
            copy.styledStamp = 0;
            copy.styledRev = 0;
            copy.thumbnail = QImage();
            copy.groupThumbnailCache = QImage();
            copy.groupThumbnailStamp = 0;
            // Only the top row of each copied run gets " copy": children of a
            // duplicated group keep their names.
            bool isChildCopy = k > 0;
            if (!isChildCopy) {
                const int indent = srcLayers[run.start].indent;
                for (int i = run.start - 1; i >= 0; --i) {
                    if (srcLayers[i].indent < indent) {
                        if (unitSet.contains(i)) isChildCopy = true;
                        break;
                    }
                }
            }
            if (!isChildCopy) copy.name = tr("%1 copy").arg(copy.name);
            rc.copies.append(std::move(copy));
        }
        prepared.append(std::move(rc));
    }

    d->beginUndoAction();
    QVector<int> newSel;
    int offset = 0;  // rows already inserted above by earlier (top) runs
    for (const RunCopies& rc : prepared) {
        const int at = rc.start + offset;
        for (int k = 0; k < rc.copies.size(); ++k) {
            d->layers.insert(at + k, rc.copies[k]);
            newSel.append(at + k);
        }
        offset += rc.copies.size();
    }
    d->selectedLayers = newSel;
    d->activeLayer = newSel.isEmpty() ? d->activeLayer : newSel.first();
    const QString undoName =
        newSel.size() > 1 ? tr("Duplicate Layers") : tr("Duplicate Layer");
    d->commitUndoAction(undoName, QStringLiteral("newlayer"));
    d->rebuildComposite();
    emit layersChanged();
    emit activeLayerChanged();
    emit historyChanged();
    emit documentModified(d);
    return true;
}

QVector<int> AppState::groupPixelDescendantIndices(int groupIndex) const {
    const DocumentItem* d = activeDocument();
    if (!d || groupIndex < 0 || groupIndex >= d->layers.size() ||
        d->layers[groupIndex].kind != LayerItem::Kind::Group)
        return {};
    const int base = d->layers[groupIndex].indent;
    QVector<int> out;
    for (int i = groupIndex + 1; i < d->layers.size(); ++i) {
        const LayerItem& l = d->layers[i];
        if (l.indent <= base) break;   // end of the group's subtree
        if (l.kind == LayerItem::Kind::Pixel && l.pixels) out.push_back(i);
    }
    return out;
}

QRectF AppState::groupBounds(int groupIndex) const {
    const DocumentItem* d = activeDocument();
    if (!d) return QRectF();
    QRectF bounds;
    for (int idx : groupPixelDescendantIndices(groupIndex))
        bounds |= layerBounds(*d, d->layers[idx]);
    return bounds;
}

int AppState::outerGroupContaining(int index) const {
    const DocumentItem* d = activeDocument();
    if (!d || index < 0 || index >= d->layers.size()) return -1;
    int out = -1;
    int indent = d->layers[index].indent;
    // Walk up the stack: a Group row shallower than the current row is the
    // parent; keep climbing to reach the outermost ancestor. Rows at the same
    // or deeper indent belong to the subtree region and can't be ancestors.
    for (int i = index - 1; i >= 0; --i) {
        const LayerItem& l = d->layers[i];
        if (l.indent >= indent) continue;
        if (l.kind == LayerItem::Kind::Group) {
            out = i;
            indent = l.indent;   // continue only inside this group
        } else {
            break;   // a shallower non-group row ends the walk
        }
    }
    return out;
}

bool AppState::toggleLayerClipped() {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer ||
        (layer->kind != LayerItem::Kind::Pixel &&
         layer->kind != LayerItem::Kind::Adjustment) ||
        layer->locked) {
        setStatusHint(tr("Clipping needs an unlocked pixel or adjustment layer."));
        return false;
    }
    beginUndoStep();
    activeLayer()->clipped = !activeLayer()->clipped;
    const bool clipped = activeLayer()->clipped;
    commitUndoStep(clipped ? tr("Create Clipping Mask") : tr("Release Clipping Mask"),
                   QStringLiteral("clip"));
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

}  // namespace pittore::ui
