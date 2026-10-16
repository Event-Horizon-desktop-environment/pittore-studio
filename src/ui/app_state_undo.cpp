// The undo/redo engine: snapshots, gesture steps and the display history.
// Split out of app_state.cpp; everything here is a DocumentItem or AppState
// member, so no extra declarations are needed.

#include "ui/app_state.h"

#include <algorithm>
#include <memory>

namespace pittore::ui {

// ---------------------------------------------------------------------------
// Undo/redo engine
// ---------------------------------------------------------------------------
// Displays history as presentation only; the undo engine here is a real one.
// Each action captures a PRE-action snapshot; undo restores it and redo moves
// it back. Pixel images are shared with the snapshots, so in-place edits
// copy-on-write first (see copyOnWriteActiveLayer) — otherwise restoring a
// snapshot would show the edit that came after it.

void DocumentItem::beginUndoAction() {
    pendingUndo_ = std::make_unique<DocumentSnapshot>(captureSnapshot());
}

namespace {
// Process-wide undo depth cap (see DocumentItem::maxUndoSteps). Function-local
// so no separate definition is needed; tests in other binaries keep the
// default unless they opt in.
int& undoCap() {
    static int n = DocumentItem::kDefaultMaxUndoSteps;
    return n;
}
}  // namespace

int DocumentItem::maxUndoSteps() { return undoCap(); }

void DocumentItem::setMaxUndoSteps(int n) { undoCap() = std::clamp(n, 1, 1000); }

void DocumentItem::commitUndoAction(const QString& name, const QString& iconKey) {
    if (!pendingUndo_) return;
    undoStack_.append(*pendingUndo_);
    const int over = undoStack_.size() - maxUndoSteps();
    if (over > 0) undoStack_.remove(0, over);
    redoStack_.clear();
    pendingUndo_.reset();
    // Display-history entry (mirrors pushHistory): any new action truncates
    // the redo tail, keeping history linear.
    while (history.size() > historyPosition + 1) history.removeLast();
    history.append({name, iconKey, false});
    historyPosition = history.size() - 1;
    dirty = true;
}

void DocumentItem::discardUndoAction() { pendingUndo_.reset(); }

void DocumentItem::resetHistory() {
    undoStack_.clear();
    redoStack_.clear();
    pendingUndo_.reset();
    history.clear();
    historyPosition = 0;
}

DocumentItem::DocumentSnapshot DocumentItem::captureSnapshot() const {
    DocumentSnapshot s;
    s.layers = layers;
    s.activeLayer = activeLayer;
    s.selectedLayers = selectedLayers;
    s.selection = selection;
    s.selectionIsEllipse = selectionIsEllipse;
    s.selectionMask = selectionMask;
    s.selectionIsMask = selectionIsMask;
    s.selectionStamp = selectionStamp;
    s.history = history;
    s.historyPosition = historyPosition;
    s.colorSamples = colorSamples;
    s.notes = notes;
    s.countMarkers = countMarkers;
    s.rulerStart = rulerStart;
    s.rulerEnd = rulerEnd;
    s.rulerHasMeasurement = rulerHasMeasurement;
    s.areaRect = areaRect;
    s.areaHasMeasurement = areaHasMeasurement;
    s.docSize = size;
    s.canvasPaper = canvasPaper;
    s.colorMode = colorMode;
    s.iccProfile = iccProfile;
    s.slices = slices;
    return s;
}

void DocumentItem::restoreSnapshot(const DocumentSnapshot& s) {
    layers = s.layers;
    activeLayer = s.activeLayer;
    selectedLayers = s.selectedLayers;
    selection = s.selection;
    selectionIsEllipse = s.selectionIsEllipse;
    selectionMask = s.selectionMask;
    selectionIsMask = s.selectionIsMask;
    selectionStamp = s.selectionStamp;
    history = s.history;
    historyPosition = s.historyPosition;
    colorSamples = s.colorSamples;
    notes = s.notes;
    countMarkers = s.countMarkers;
    rulerStart = s.rulerStart;
    rulerEnd = s.rulerEnd;
    rulerHasMeasurement = s.rulerHasMeasurement;
    areaRect = s.areaRect;
    areaHasMeasurement = s.areaHasMeasurement;
    size = s.docSize.isValid() ? s.docSize : size;
    canvasPaper = s.canvasPaper.isValid() ? s.canvasPaper : canvasPaper;
    if (!s.colorMode.isEmpty()) colorMode = s.colorMode;
    iccProfile = s.iccProfile;
    slices = s.slices;
    pendingUndo_.reset();
    paintDirty = QRect();   // stale incremental state; restore rebuilds fully
}

void AppState::beginUndoStep() {
    if (DocumentItem* d = activeDocument()) {
        // Nesting (a Character-panel edit inside a live text session) must not
        // overwrite the outer snapshot: the whole session stays one step.
        if (undoGestureDepth_ == 0) d->beginUndoAction();
        ++undoGestureDepth_;
    }
}

void AppState::commitUndoStep(const QString& name, const QString& iconKey) {
    if (undoGestureDepth_ == 0) return;
    if (--undoGestureDepth_ != 0) return;   // an inner step stays in the gesture
    if (DocumentItem* d = activeDocument()) {
        d->commitUndoAction(name, iconKey);
        emit historyChanged();
    }
}

void AppState::discardUndoStep() {
    if (undoGestureDepth_ == 0) return;
    if (--undoGestureDepth_ != 0) return;
    if (DocumentItem* d = activeDocument()) d->discardUndoAction();
}

bool AppState::undoStepActive() const { return undoGestureDepth_ > 0; }

void AppState::cancelUndoStep() {
    if (undoGestureDepth_ == 0) return;
    if (--undoGestureDepth_ != 0) return;  // an inner step stays in the gesture
    if (DocumentItem* d = activeDocument()) {
        if (d->pendingUndo_) d->restoreSnapshot(*d->pendingUndo_);
        d->dirty = true;
        d->rebuildComposite();
        emit activeLayerChanged();
        emit layersChanged();
        emit selectionChanged();
        emit historyChanged();
        emit documentModified(d);
    }
}

bool AppState::canUndo() const {
    const DocumentItem* d = activeDocument();
    return d && d->canUndo();
}

bool AppState::canRedo() const {
    const DocumentItem* d = activeDocument();
    return d && d->canRedo();
}

int AppState::undoDepth() const {
    const DocumentItem* d = activeDocument();
    return d ? d->undoDepth() : 0;
}

int AppState::redoDepth() const {
    const DocumentItem* d = activeDocument();
    return d ? d->redoDepth() : 0;
}

QString AppState::undoStepName() const {
    const DocumentItem* d = activeDocument();
    return (d && d->canUndo() && !d->history.isEmpty()) ? d->history.last().name
                                                        : QString();
}

QString AppState::redoStepName() const {
    const DocumentItem* d = activeDocument();
    if (d && !d->redoStack_.isEmpty()) {
        const QString name = d->redoStack_.last().history.isEmpty()
                                 ? QString()
                                 : d->redoStack_.last().history.last().name;
        if (!name.isEmpty()) return name;
    }
    // Fall back to the step's pre-action name so "Redo" still labels something
    // when the snapshot predates any history entry (edge: first action of a doc).
    return undoStepName();
}

void AppState::undo() {
    DocumentItem* d = activeDocument();
    if (!d || d->undoStack_.isEmpty()) {
        setStatusHint(tr("Nothing to undo."));
        return;
    }
    undoGestureDepth_ = 0;   // a programmatic undo ends any pending gesture
    // The current (post-action) state becomes the redo tail.
    d->redoStack_.push_back(d->captureSnapshot());
    d->restoreSnapshot(d->undoStack_.takeLast());
    d->dirty = true;
    d->rebuildComposite();
    emit activeLayerChanged();
    emit layersChanged();
    emit selectionChanged();
    emit historyChanged();
    emit documentModified(d);
}

void AppState::redo() {
    DocumentItem* d = activeDocument();
    if (!d || d->redoStack_.isEmpty()) {
        setStatusHint(tr("Nothing to redo."));
        return;
    }
    undoGestureDepth_ = 0;
    d->undoStack_.push_back(d->captureSnapshot());
    d->restoreSnapshot(d->redoStack_.takeLast());
    d->dirty = true;
    d->rebuildComposite();
    emit activeLayerChanged();
    emit layersChanged();
    emit selectionChanged();
    emit historyChanged();
    emit documentModified(d);
}

void AppState::pushHistory(const QString& name, const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    // Any new action truncates the redo tail, keeping history linear —
    // both the display list and the engine's redo stack.
    while (d->history.size() > d->historyPosition + 1) d->history.removeLast();
    d->history.append({name, iconKey, false});
    d->historyPosition = d->history.size() - 1;
    d->redoStack_.clear();
    d->dirty = true;
    emit historyChanged();
    emit documentModified(d);
}

}  // namespace pittore::ui
