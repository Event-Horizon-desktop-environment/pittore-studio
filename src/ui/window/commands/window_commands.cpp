#include "ui/main_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QPointer>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSet>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "engine/ai/bg_remove.h"
#include "engine/compute/factory.h"
#include "engine/compute/adjust.h"
#include "engine/core/log.h"
#include "engine/core/tonal_ops.h"
#include "ui/ai_models.h"
#include "ui/canvas_view.h"
#include "ui/contextual_task_bar.h"
#include "ui/export_dialog.h"
#include "ui/icons.h"
#include "ui/layer_style_dialog.h"
#include "ui/options_bar.h"
#include "ui/panels.h"
#include "ui/persona/vector_path_ops.h"
#include "ui/persona/vector_edit.h"
#include "ui/persona/vector_node.h"
#include "ui/keymap.h"
#include "ui/spotlight.h"
#include "ui/filter_dialog.h"
#include "engine/filter/filters.h"
#include "ui/preferences_dialog.h"
#include "ui/project_manager.h"
#include "ui/refine_dialog.h"
#include "ui/selection_mask.h"
#include "ui/theme.h"
#include "ui/tone_dialogs.h"
#include "ui/tools_panel.h"
#include "ui/workspace.h"
#include "ui/window/shared/window_helpers.h"

namespace pittore::ui {


// ---------------------------------------------------------------------------
// Command routing (options bar + contextual task bar + menus)
// ---------------------------------------------------------------------------

QImage MainWindow::maskedSelectionCopy(QRectF* docRectOut) {
    // Delegate to the shared AppState lift: the active layer's appearance
    // inside the live selection, clipped by its mask when one exists.
    const DocumentItem* d = state_->activeDocument();
    const QImage noMask;
    const QImage& m = (d && d->selectionIsMask && !d->selectionMask.isNull())
                          ? d->selectionMask
                          : noMask;
    return state_->copyActiveLayerMasked(m, d ? d->selection : QRectF(),
                                         docRectOut);
}


bool MainWindow::runHeadlessCommand(const QString& id) {
    // Headless-safe: these never open dialogs, switch task context, or need
    // a cursor position. Everything else (refine dialogs, generative fill,
    // transforms, menus) is refused — a modal dialog with no one to click it
    // would hang the session.
    static const char* kAllowed[] = {
        "auto_refine",
        "select_subject",
        "select-subject",
        "remove_background",
        "remove-background",
        "enhance_edges",
        "enhance-edges",
        "deselect",
        "invert_selection",
        "invert-selection",
    };
    const QString norm = id.toLower().trimmed();
    bool allowed = false;
    for (const char* a : kAllowed) {
        if (norm == QString::fromLatin1(a)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        state_->setStatusHint(
            tr("'%1' is not available headlessly.").arg(id));
        updateStatus();
        return false;
    }
    runCommand(norm);
    return true;
}


void MainWindow::runCommand(const QString& id) {
    DocumentItem* doc = state_->activeDocument();

    if (id == QLatin1String("fit-screen")) {
        canvas_->zoomToFit();
    } else if (id == QLatin1String("fill-screen")) {
        canvas_->zoomToFill();
    } else if (id == QLatin1String("actual-pixels") || id == QLatin1String("100")) {
        canvas_->zoomActualPixels();
    } else if (id == QLatin1String("select-all")) {
        if (doc) state_->setSelection(QRectF(QPointF(0, 0), QSizeF(doc->size)), false);
    } else if (id == QLatin1String("deselect")) {
        state_->clearSelection();
    } else if (id == QLatin1String("invert-selection") ||
               id == QLatin1String("invert_selection")) {
        state_->invertSelection();
    } else if (id == QLatin1String("fill-fg") || id == QLatin1String("fill_fg")) {
        // Alt+Backspace: fill the live selection (or the whole layer) with
        // the foreground color.
        if (!state_->fillActiveSelectionWithColor(state_->foreground(),
                                                  tr("Fill with Foreground")))
            state_->setStatusHint(tr("Fill needs an unlocked pixel layer."));
    } else if (id == QLatin1String("fill-bg") || id == QLatin1String("fill_bg")) {
        // Ctrl+Backspace: same with the background color.
        if (!state_->fillActiveSelectionWithColor(state_->background(),
                                                  tr("Fill with Background")))
            state_->setStatusHint(tr("Fill needs an unlocked pixel layer."));
    } else if (id == QLatin1String("invert") ||
               id == QLatin1String("invert-image")) {
        // Ctrl+I: complement the active layer's RGB (selection-aware).
        if (!state_->invertActiveLayerPixels())
            state_->setStatusHint(tr("Invert needs an unlocked pixel layer."));
    } else if (id == QLatin1String("select-subject") || id == QLatin1String("remove-background") ||
               id == QLatin1String("select_subject") ||
               id == QLatin1String("remove_background")) {
        runAiBackgroundRemoval(id);
    } else if (id == QLatin1String("select-and-mask") || id == QLatin1String("select_and_mask")) {
        // Select and Mask IS the Refine Selection dialog: with a live
        // selection it opens directly, otherwise Select Subject runs first
        // and the dialog follows on success.
        refineSelection(true);
    } else if (id == QLatin1String("enhance-edges") ||
               id == QLatin1String("enhance_edges")) {
        runEnhanceEdges();
    } else if (id == QLatin1String("auto_refine")) {
        runAutoRefine();
    } else if (id == QLatin1String("refine") || id == QLatin1String("refine-selection") ||
               id == QLatin1String("refine_selection")) {
        // Options-bar Refine buttons sit on manual selection tools: open the
        // dialog when a selection exists, otherwise hint — never an implicit
        // subject select.
        refineSelection(false);
    } else if (id == QLatin1String("generative-fill") || id == QLatin1String("generate")) {
        state_->pushHistory(tr("Generative Fill"), QStringLiteral("sparkle"));
        state_->setTaskContext(TaskContext::GenerativeResult);
    } else if (id == QLatin1String("free-transform")) {
        state_->setTaskContext(TaskContext::Transform);
    } else if (id == QLatin1String("commit")) {
        state_->pushHistory(tr("Commit"), QStringLiteral("check"));
        state_->setTaskContext(doc && !doc->selection.isEmpty() ? TaskContext::Selection
                                                                : TaskContext::None);
    } else if (id == QLatin1String("cancel")) {
        state_->setTaskContext(TaskContext::None);
    } else if (id == QLatin1String("apply_crop") ||
               id == QLatin1String("apply-crop")) {
        // Contextual task bar Apply / Enter: crop the document to the live
        // frame in one undo step (no-op with a hint when the frame already
        // covers the whole image).
        canvas_->commitCrop();
    } else if (id == QLatin1String("cancel_crop") ||
               id == QLatin1String("cancel-crop")) {
        // Contextual task bar Cancel / Escape: the frame returns to the whole
        // document; nothing in the document ever changed, so no undo entry.
        canvas_->cancelCrop();
    } else if (id == QLatin1String("new-layer")) {
        LayerItem layer;
        layer.name = tr("Layer %1").arg(doc ? doc->layers.size() + 1 : 1);
        state_->beginUndoStep();
        state_->addLayer(layer);
        state_->commitUndoStep(tr("New Layer"), QStringLiteral("newlayer"));
    } else if (id == QLatin1String("new-group")) {
        LayerItem group;
        group.kind = LayerItem::Kind::Group;
        group.name = tr("Group %1").arg(doc ? doc->layers.size() + 1 : 1);
        state_->beginUndoStep();
        state_->addLayer(group);
        state_->commitUndoStep(tr("New Group"), QStringLiteral("group"));
    } else if (id == QLatin1String("delete-layer")) {
        // The Del key, the Layer menu and the Layers-panel trash all route
        // here. A live arbitrary-shape selection makes Delete erase just the
        // selected pixels of the active layer (standard editors behaviour);
        // otherwise it removes every selected layer (canvas clicks select
        // first, so click-a-layer-then-Del works too).
        if (!state_->eraseSelectionFromActiveLayer())
            state_->removeSelectedLayers();
    } else if (id == QLatin1String("clear")) {
        // The "clear" id means two different things depending on the live
        // tool: annotation tools clear their markers; everywhere else it is
        // Edit ▸ Clear, which erases the active selection from the active
        // pixel layer (mask-aware, soft edges).
        const ToolId cTool = state_->activeTool();
        if (cTool == ToolId::ColorSampler || cTool == ToolId::Note ||
            cTool == ToolId::Count || cTool == ToolId::Ruler) {
            // Annotation tools share the "clear" button id; what gets cleared
            // depends on the live tool. One undo step when something was there.
            if (doc) {
                state_->beginUndoStep();
                bool any = false;
                if (cTool == ToolId::ColorSampler) {
                    any = !doc->colorSamples.isEmpty();
                    doc->colorSamples.clear();
                } else if (cTool == ToolId::Note) {
                    any = !doc->notes.isEmpty();
                    doc->notes.clear();
                } else if (cTool == ToolId::Count) {
                    any = !doc->countMarkers.isEmpty();
                    doc->countMarkers.clear();
                } else if (cTool == ToolId::Ruler) {
                    any = doc->rulerHasMeasurement;
                    doc->rulerHasMeasurement = false;
                }
                if (any) {
                    state_->commitUndoStep(tr("Clear"), QStringLiteral("clear"));
                    state_->markAnnotationsChanged();
                } else {
                    state_->discardUndoStep();
                }
            }
        } else {
            state_->eraseSelectionFromActiveLayer();
        }
    } else if (id == QLatin1String("copy")) {
        // Edit ▸ Copy (Ctrl+C): stash the active layer's pixels — clipped to
        // the live selection when one exists — for an internal Paste.
        QRectF rect;
        clipboardImage_ = maskedSelectionCopy(&rect);
        clipboardRect_ = rect;
        clipboardLabel_ = !clipboardImage_.isNull()
                              ? tr("Clipboard") + QStringLiteral(" (%1×%2)")
                                    .arg(clipboardImage_.width())
                                    .arg(clipboardImage_.height())
                              : QString();
        state_->setStatusHint(
            clipboardImage_.isNull()
                ? tr("Copy: need a pixel layer (optionally with a selection).")
                : tr("Copied %1×%2 px to the clipboard.")
                      .arg(clipboardImage_.width())
                      .arg(clipboardImage_.height()));
    } else if (id == QLatin1String("paste") || id == QLatin1String("paste-in-place")) {
        // Edit ▸ Paste (Ctrl+V): place the internal clipboard as a new pixel
        // layer, centered on the document (Paste in Place keeps the original
        // spot). The clipboard is the masked copy, so pasting after an Object
        // Select drops exactly the selected pixels onto a fresh layer.
        if (clipboardImage_.isNull()) {
            state_->setStatusHint(tr("Nothing on the clipboard — Copy first."));
        } else if (doc) {
            QPointF center;
            if (id == QLatin1String("paste-in-place") && !clipboardRect_.isEmpty()) {
                center = clipboardRect_.center();
                center.setX(qBound(0.0, center.x(), double(doc->size.width())));
                center.setY(qBound(0.0, center.y(), double(doc->size.height())));
            } else {
                center = QPointF(doc->size.width() * 0.5, doc->size.height() * 0.5);
            }
            const QString label =
                clipboardLabel_.isEmpty() ? tr("Pasted Layer") : clipboardLabel_;
            state_->placeImageLayer(clipboardImage_, label, center, 1.0);
        }
    } else if (id == QLatin1String("layer-via-copy")) {
        // Layer ▸ New ▸ Layer via Copy (Ctrl+J): one step — a new pixel layer
        // holding exactly the selected pixels, on top, in place.
        QRectF rect;
        const QImage copy = maskedSelectionCopy(&rect);
        if (copy.isNull() || !doc) {
            state_->setStatusHint(tr("Layer via Copy needs a pixel layer."));
        } else {
            state_->placeImageLayer(
                copy, tr("Layer via Copy"), rect.isEmpty()
                                               ? QPointF(doc->size.width() * 0.5,
                                                        doc->size.height() * 0.5)
                                               : rect.center(),
                1.0);
        }
    } else if (id == QLatin1String("duplicate-layers") ||
               id == QLatin1String("duplicate-layer")) {
        // Layer ▸ Duplicate Layer… and the Layers-panel right-click menu: a
        // full in-place copy of every selected layer (group subtrees intact),
        // placed directly above its source. One undo step.
        if (!state_->duplicateSelectedLayers())
            state_->setStatusHint(tr("Duplicate Layer needs a layer to copy."));
    } else if (id == QLatin1String("lock_area")) {
        // Replace Color options-bar Lock Area button: the click-free twin of
        // Alt+click (some window managers swallow Alt+click for window
        // moves). Locks the palette under the brush cursor.
        if (canvas_) canvas_->lockReplacePalette();
    } else if (id == QLatin1String("select-all-layers")) {
        state_->selectAllLayers();
    } else if (id == QLatin1String("deselect-layers")) {
        state_->clearLayerSelection();
    } else if (id == QLatin1String("group-layers")) {
        state_->groupSelectedLayers();
    } else if (id == QLatin1String("tone-blend-group")) {
        const int g = state_->makeToneBlendGroup();
        if (g >= 0) toneBlendDialog(g);
    } else if (id == QLatin1String("ungroup-layers")) {
        state_->ungroupSelectedLayers();
    } else if (id == QLatin1String("bring-to-front")) {
        state_->bringSelectedLayersToFront();
    } else if (id == QLatin1String("send-to-back")) {
        state_->sendSelectedLayersToBack();
    } else if (id == QLatin1String("bring-forward")) {
        state_->moveSelectedLayersInStack(-1);
    } else if (id == QLatin1String("send-backward")) {
        state_->moveSelectedLayersInStack(1);
    } else if (id == QLatin1String("panels")) {
        // The Type options bar's "Character and Paragraph Panels" button pops
        // both text palettes as floating windows.
        showPanel(QStringLiteral("character"));
        showPanel(QStringLiteral("paragraph"));
    } else if (id == QLatin1String("reset") &&
               state_->activeTool() == ToolId::RotateView) {
        canvas_->resetRotation();
    } else if (id == QLatin1String("straighten") &&
               state_->activeTool() == ToolId::Ruler) {
        // Rotate the active pixel layer so the measured line lies flat.
        if (doc && doc->rulerHasMeasurement) {
            const double ang =
                std::atan2(doc->rulerEnd.y() - doc->rulerStart.y(),
                           doc->rulerEnd.x() - doc->rulerStart.x()) *
                180.0 / 3.14159265358979323846;
            state_->rotateActiveLayer(-ang);
        } else {
            state_->setStatusHint(tr("Ruler: drag a measurement first."));
        }
    } else if (id == QLatin1String("newgroup") &&
               state_->activeTool() == ToolId::Count) {
        const int g = state_->option(ToolId::Count, QStringLiteral("group")).toInt();
        state_->setOption(ToolId::Count, QStringLiteral("group"), g + 1);
    } else if (id == QLatin1String("delete") &&
               state_->activeTool() == ToolId::Count) {
        if (doc) {
            const int g = state_->option(ToolId::Count, QStringLiteral("group")).toInt();
            int idx = -1;
            for (int i = doc->countMarkers.size() - 1; i >= 0; --i)
                if (doc->countMarkers[i].group == g) {
                    idx = i;
                    break;
                }
            if (idx >= 0) {
                state_->beginUndoStep();
                doc->countMarkers.removeAt(idx);
                state_->commitUndoStep(tr("Count"), QStringLiteral("count"));
                state_->markAnnotationsChanged();
            } else {
                state_->setStatusHint(tr("Count: this group has no markers."));
            }
        }
    } else if (id == QLatin1String("delete") &&
               state_->activeTool() == ToolId::SliceSelect) {
        if (doc && doc->selectedSlice >= 0 && doc->selectedSlice < doc->slices.size()) {
            state_->beginUndoStep();
            const int removed = doc->selectedSlice;
            doc->slices.removeAt(removed);
            doc->selectedSlice = -1;
            state_->commitUndoStep(tr("Delete Slice"), QStringLiteral("slice"));
            state_->markAnnotationsChanged();
        } else {
            state_->setStatusHint(tr("Slice Select: no slice selected."));
        }
    } else if (id == QLatin1String("slices_from_guides")) {
        slicesFromGuides();
    } else if (id == QLatin1String("export-slices")) {
        exportSlices();
    } else if (id == QLatin1String("add_mask")) {
        state_->addLayerMask(1.0f);
    } else if (id == QLatin1String("adjustment_layer")) {
        state_->addAdjustmentLayer(static_cast<int>(
            pittore::compute::AdjustmentKind::BrightnessContrast));
    } else if (id == QLatin1String("mask-reveal")) {
        state_->fillLayerMask(1.0f);
    } else if (id == QLatin1String("mask-hide")) {
        state_->fillLayerMask(0.0f);
    } else if (id == QLatin1String("mask-reveal-selection")) {
        state_->maskRevealSelection();
    } else if (id == QLatin1String("mask-hide-selection")) {
        state_->maskHideSelection();
    } else if (id == QLatin1String("mask-from-transparency")) {
        state_->maskFromTransparency();
    } else if (id == QLatin1String("mask-delete")) {
        state_->deleteLayerMask();
    } else if (id == QLatin1String("mask-apply")) {
        state_->applyLayerMask();
    } else if (id == QLatin1String("mask-enable-toggle")) {
        if (const LayerItem* l = state_->activeLayer())
            state_->setLayerMaskEnabled(!l->maskEnabled);
    } else if (id == QLatin1String("mask-link-toggle")) {
        if (const LayerItem* l = state_->activeLayer())
            state_->setLayerMaskLinked(!l->maskLinked);
    } else if (id == QLatin1String("toggle-clip")) {
        state_->toggleLayerClipped();
    } else if (id == QLatin1String("undo")) {
        state_->undo();
    } else if (id == QLatin1String("redo")) {
        state_->redo();
    } else if (id == QLatin1String("path_to_selection")) {
        // Path task-bar button + Paths-panel loader: art outline → selection.
        vectorPathToSelection(state_);
    } else if (id == QLatin1String("fill_path") || id == QLatin1String("shape_fill")) {
        vectorFillActiveShape(state_, state_->foreground(), tr("Fill Path"));
    } else if (id == QLatin1String("stroke_path") ||
               id == QLatin1String("shape_stroke")) {
        vectorStrokeActiveShape(state_, state_->foreground(), tr("Stroke Path"));
    } else if (id == QLatin1String("path_to_shape") ||
               id == QLatin1String("combine_shapes")) {
        // Union the selected vector shapes into one layer.
        QVector<int> targets;
        for (int i : state_->selectedLayerIndices()) {
            const DocumentItem* dd = state_->activeDocument();
            if (dd && i >= 0 && i < dd->layers.size() && dd->layers[i].art &&
                !dd->layers[i].art->isEmpty())
                targets.push_back(i);
        }
        state_->booleanFoldLayers(targets, pittore::vector::BoolOp::Union,
                                  nullptr, tr("Combine"));
    } else if (id == QLatin1String("text_on_path")) {
        // Glyph outlines along the editable art path (baked vector).
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, tr("Text on Path"), tr("Text:"), QLineEdit::Normal,
            QString(), &ok);
        if (ok && !text.isEmpty()) {
            const QString family =
                state_->option(ToolId::HorizontalType, QStringLiteral("family"))
                    .toString();
            double size =
                state_->option(ToolId::HorizontalType, QStringLiteral("size"))
                    .toDouble();
            if (!(size > 0.0)) size = 36.0;
            state_->textOnPath(text, family, size);
        }
    } else if (id == QLatin1String("text_in_shape")) {
        // Glyph outlines flowed into the editable art frame (baked vector).
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, tr("Text in Shape"), tr("Text:"), QLineEdit::Normal,
            QString(), &ok);
        if (ok && !text.isEmpty()) {
            const QString family =
                state_->option(ToolId::HorizontalType, QStringLiteral("family"))
                    .toString();
            double size =
                state_->option(ToolId::HorizontalType, QStringLiteral("size"))
                    .toDouble();
            if (!(size > 0.0)) size = 36.0;
            state_->textInShape(text, family, size);
        }
    } else if (id == QLatin1String("node_close")) {
        // Close the editable art path: appends Close, one undo step.
        // Whole-path op — no point selection needed.
        const int index = vectorEditableLayer(state_);
        DocumentItem* dd = state_->activeDocument();
        const LayerItem* l =
            (dd && index >= 0 && index < dd->layers.size())
                ? &dd->layers[index]
                : nullptr;
        if (!l || !l->art || l->art->isEmpty()) {
            state_->setStatusHint(tr("Select a vector shape layer to close."));
        } else {
            pittore::vector::ArtNode work = *l->art;
            if (!closeNodePath(work)) {
                state_->setStatusHint(tr("The path is already closed."));
            } else if (state_->applyVectorNode(index, work, tr("Close Path"))) {
                state_->setStatusHint(tr("Path closed."));
            }
        }
    } else if (id == QLatin1String("node_split") || id == QLatin1String("node_join") ||
               id == QLatin1String("node_reverse")) {
        // Split/join/reverse act on the Node tool's click-selected anchor
        // (CanvasView owns the selection; indices revalidate here).
        int layer = -1, seg = -1;
        if (canvas_) {
            layer = canvas_->selectedNodeLayer();
            seg = canvas_->selectedNodeSeg();
        }
        DocumentItem* dd = state_->activeDocument();
        const LayerItem* l =
            (dd && layer >= 0 && layer < dd->layers.size())
                ? &dd->layers[layer]
                : nullptr;
        const bool usable =
            l && l->art && !l->art->isEmpty() && seg >= 0 &&
            seg < static_cast<int>(l->art->segments.size());
        if (!usable) {
            state_->setStatusHint(
                tr("Node: click an anchor point first."));
        } else {
            pittore::vector::ArtNode work = *l->art;
            bool ok = false;
            QString name, done, fail;
            if (id == QLatin1String("node_split")) {
                ok = splitNodePath(work, seg);
                name = tr("Split Path");
                done = tr("Path split.");
                fail = tr("Nothing to split there.");
            } else if (id == QLatin1String("node_join")) {
                ok = joinNodePath(work, seg);
                name = tr("Join Path");
                done = tr("Points joined.");
                fail = tr("Select an open end point to join.");
            } else {
                ok = reverseNodeSubpath(work, seg);
                name = tr("Reverse Path");
                done = tr("Path reversed.");
                fail = tr("Nothing to reverse there.");
            }
            if (ok && state_->applyVectorNode(layer, work, name))
                state_->setStatusHint(done);
            else if (!ok)
                state_->setStatusHint(fail);
        }
    } else if (id == QLatin1String("bake_appearance")) {
        // Corner/Contour bake needs curve conversion (Phase 3): honest hint.
        vectorPlannedHint(state_, tr("Bake Appearance"));
    } else if (id == QLatin1String("reset_profile")) {
        state_->resetStrokeProfile();
    } else if (id == QLatin1String("place_browse")) {
        vectorPlannedHint(state_, tr("File ▸ Place"));
    } else if (id == QLatin1String("lpe_open")) {
        showPanel(QStringLiteral("lpe"));
    } else if (id == QLatin1String("page_add")) {
        // New page frame from the bar's size preset, centered in the doc.
        DocumentItem* d = state_->activeDocument();
        if (!d) {
            state_->setStatusHint(tr("Open a document first."));
        } else {
            const int preset =
                state_->option(ToolId::PagesTool, QStringLiteral("page_size"))
                    .toInt();
            double w = 794, h = 1123;  // A4 @96dpi
            if (preset == 1) {
                w = 1123;
                h = 1587;
            } else if (preset == 2) {
                w = 816;
                h = 1056;
            } else if (preset == 3) {
                w = d->size.width();
                h = d->size.height();
            }
            auto node = std::make_shared<pittore::vector::ArtNode>();
            node->name = "Page";
            using Seg = pittore::vector::Segment;
            node->segments = {
                Seg{Seg::Kind::MoveTo, 0, 0}, Seg{Seg::Kind::LineTo, (float)w, 0},
                Seg{Seg::Kind::LineTo, (float)w, (float)h},
                Seg{Seg::Kind::LineTo, 0, (float)h}, Seg{Seg::Kind::Close}};
            node->paint.hasStroke = true;
            node->paint.stroke[0] = node->paint.stroke[1] =
                node->paint.stroke[2] = 90;
            node->paint.stroke[3] = 255;
            node->paint.strokeWidth = 1.0;
            const double x = (d->size.width() - w) / 2;
            const double y = (d->size.height() - h) / 2;
            state_->commitArtNodeLayer(std::move(node), QRectF(x, y, w, h),
                                       ToolId::PagesTool, tr("New Page"), true);
        }
    } else if (id == QLatin1String("style_unload")) {
        state_->setStatusHint(tr("Style Picker unloaded."));
    } else {
        // Every other command id is a researched affordance whose engine call
        // does not exist yet; surface it rather than silently swallowing it.
        state_->setStatusHint(tr("%1 is not implemented in this build").arg(id));
    }
    updateStatus();
}


void MainWindow::refineSelection(bool autoSubject) {
    DocumentItem* doc = state_->activeDocument();
    if (!doc || doc->size.isEmpty()) return;
    auto hasSelection = [&] {
        return !selectionMaskBbox(selectionAsMask(*doc)).isEmpty();
    };
    if (!hasSelection()) {
        if (!autoSubject) {
            state_->setStatusHint(
                tr("Make a selection first — Select Subject, then Refine."));
            updateStatus();
            return;
        }
        // Select and Mask with no selection: run Select Subject, then carry
        // its mask straight into the Refine dialog.
        runAiBackgroundRemoval(QStringLiteral("select-subject"));
        doc = state_->activeDocument();
        if (!doc || !hasSelection()) {
            updateStatus();
            return;
        }
    }
    RefineDialog dialog(state_, doc, this);
    if (dialog.exec() != QDialog::Accepted) {
        updateStatus();
        return;
    }
    applyRefineResult(dialog.result());
    updateStatus();
}


void MainWindow::applyRefineResult(const RefineResult& result) {
    DocumentItem* doc = state_->activeDocument();
    if (!result.accepted || !doc) return;
    if (result.mask.isNull() ||
        result.mask.size() != doc->size ||
        selectionMaskBbox(result.mask).isEmpty()) {
        state_->setStatusHint(tr("Refine produced an empty mask."));
        updateStatus();
        return;
    }
    switch (result.output) {
        case RefineResult::Output::Selection:
            state_->replaceSelectionMask(result.mask, tr("Refine Selection"),
                                         QStringLiteral("mask"));
            break;
        case RefineResult::Output::LayerMask:
            // Mask output reads the live selection, so publish the refined
            // channel first (no undo), then bake it into the mask (one step).
            state_->setSelectionMask(result.mask);
            if (!state_->maskRevealSelection()) {
                state_->setStatusHint(tr("Mask needs an unlocked pixel layer."));
                return;
            }
            break;
        case RefineResult::Output::NewDecontaminatedLayer:
            if (!state_->newDecontaminatedLayer(result.mask, false)) {
                state_->setStatusHint(
                    tr("Decontaminate needs an unlocked pixel layer."));
                return;
            }
            break;
        case RefineResult::Output::NewDecontaminatedLayerWithMask:
            if (!state_->newDecontaminatedLayer(result.mask, true)) {
                state_->setStatusHint(
                    tr("Decontaminate needs an unlocked pixel layer."));
                return;
            }
            break;
    }
    state_->setStatusHint(tr("Refined the selection."));
    updateStatus();
}

}  // namespace pittore::ui
