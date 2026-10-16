#include "ui/panels.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDataStream>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>

#include "ui/ai_models.h"
#include "ui/icons.h"
#include "ui/tool_registry.h"

#include <chrono>

#include "engine/core/log.h"
#include "engine/compute/adjust.h"
#include "ui/curve_editor.h"
#include "ui/panels/shared/panel_helpers.h"

namespace pittore::ui {
namespace {

// Fixed row geometry: rows are positioned manually (see rebuild), so the
// height lives in one place. The end slack keeps an empty drop zone below
// the last row for end-of-list drops.
constexpr int kRowHeight = 40;
// Pool overscan each side of the viewport: scrolling within the overscan
// rebinds nothing, so steady scrolling costs one repaint per new row.
constexpr int kWindowOver = 10;
constexpr int kEndSlack = 200;


class LayerRow final : public QWidget {
  public:
    LayerRow(AppState* state, int index, LayerDropState* drop, QWidget* parent,
             QVector<LayerRow*>* siblings)
        : QWidget(parent),
          state_(state),
          index_(index),
          drop_(drop),
          siblings_(siblings) {
        setFixedHeight(kRowHeight);
        setAutoFillBackground(false);
        // Clicking a row must take keyboard focus: otherwise Del keeps going to
        // whatever was focused before (commonly an opacity/size spinbox), and
        // the app's Delete-layer handler never sees it.
        setFocusPolicy(Qt::ClickFocus);
        setAcceptDrops(true);
        // The inline name editor is created on demand in startRename(), not
        // here: one QLineEdit per row doubles widget count, and widget
        // teardown scans the posted-event queue per row (minutes on huge
        // documents), so rows stay editor-free until renamed.
    }

    // Rebind a pooled row to another layer index (the pool covers the
    // visible window, not the whole stack, so a structural change or a
    // scroll only rebinds indices instead of creating widgets). Same
    // index is a no-op; the panel's own repaint covers content edits.
    // An in-flight rename cannot survive a rebind, so it is cancelled.
    void setIndex(int index) {
        if (index == index_) return;
        cancelRename();
        index_ = index;
        // Observable binding for tests and tools: pooled rows move, so
        // position in the child list means nothing.
        setProperty("layerIndex", index_);
        dragStarted_ = false;
        pressWasMulti_ = false;
        update();
    }

    int layerIndex() const { return index_; }

    // Opens the inline editor on this row, pre-filled with the layer name and
    // fully selected so typing replaces it. Used by the context menu, by
    // double-clicking the name, and by the panel when a group is created.
    // The editor is built on first use (see the constructor): most rows are
    // never renamed, so most rows never pay for it.
    void startRename() {
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= d->layers.size() || renaming_) return;
        if (!renameEdit_) {
            renameEdit_ = new QLineEdit(this);
            renameEdit_->hide();
            // Commits on Return and on focus-out; Esc cancels via the event
            // filter below. The renaming_ flag makes the second
            // editingFinished (focus is pulled away by hide()) a no-op.
            connect(renameEdit_, &QLineEdit::editingFinished, this,
                    [this] { finishRename(); });
            renameEdit_->installEventFilter(this);
        }
        renaming_ = true;
        renameEdit_->setText(d->layers[index_].name);
        renameEdit_->setGeometry(nameRect().adjusted(0, 5, -2, -5));
        renameEdit_->show();
        renameEdit_->setFocus();
        renameEdit_->selectAll();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= d->layers.size()) return;
        const LayerItem& layer = d->layers[index_];
        const ThemeColors c = colorsFor(state_->theme());
        const bool active = d->activeLayer == index_;
        const bool selected = d->selectedLayers.contains(index_);

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), active     ? c.accent.darker(135)
                          : selected  ? c.accent.darker(185)
                                      : c.chrome);
        p.setPen(QPen(c.border, 1));
        p.drawLine(0, height() - 1, width(), height() - 1);

        const int indent = layer.indent * 12;

        // Visibility eye.
        if (layer.visible) {
            p.drawPixmap(QRect(5, 12, 16, 16), glyphPixmap(QStringLiteral("eye"), 16, c.text, p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0));
        } else {
            p.setPen(QPen(c.textDim, 1));
            p.drawRect(QRect(8, 15, 10, 10));
        }

        // Thumbnail. Group rows sit behind a disclosure chevron, so their
        // content (and combined preview) starts 14px further right.
        const bool isGroup = layer.kind == LayerItem::Kind::Group;
        const QRect thumb(26 + indent + (isGroup ? 14 : 0), 4, 32, 32);
        p.fillRect(thumb, QColor(0x22, 0x22, 0x22));
        if (isGroup) {
            // Disclosure chevron: ▾ while expanded, ► while collapsed. A plain
            // click toggles this group; Ctrl/Cmd toggles every group, Alt/Option
            // toggles this group's whole nested subtree.
            const QRect chevron(26 + indent, 13, 12, 12);
            const QPoint cc = chevron.center();
            QPolygon tri;
            if (layer.groupExpanded)
                tri << QPoint(cc.x() - 4, cc.y() - 4)
                    << QPoint(cc.x() + 4, cc.y() - 4)
                    << QPoint(cc.x(), cc.y() + 4);
            else
                tri << QPoint(cc.x() - 3, cc.y() - 5)
                    << QPoint(cc.x() - 3, cc.y() + 5)
                    << QPoint(cc.x() + 5, cc.y());
            p.setPen(Qt::NoPen);
            p.setBrush(c.textDim);
            p.drawPolygon(tri);
        }
        if (isGroup) {
            // A group row previews its combined descendant content, like
            // The default group thumbnails; empty groups keep the folder
            // glyph. Sampled through groupThumbnail's content stamp so only a
            // real descendant change recomposites the box.
            const QImage preview = groupThumbnail(*d, index_, 30);
            if (preview.isNull()) {
                p.drawPixmap(thumb.adjusted(7, 7, -7, -7),
                             glyphPixmap(QStringLiteral("group"), 18, c.text, p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0));
            } else {
                const QRect inner = thumb.adjusted(1, 1, -1, -1);
                constexpr int kCell = 5;
                for (int y = 0; y < inner.height(); y += kCell)
                    for (int x = 0; x < inner.width(); x += kCell)
                        p.fillRect(QRect(inner.left() + x, inner.top() + y, kCell, kCell)
                                       .intersected(inner),
                                   ((x / kCell + y / kCell) & 1)
                                       ? QColor(0x33, 0x33, 0x33)
                                       : QColor(0x44, 0x44, 0x44));
                p.drawImage(inner, preview);
            }
        } else if (layer.kind == LayerItem::Kind::Adjustment) {
            p.drawPixmap(thumb.adjusted(6, 6, -6, -6),
                         glyphPixmap(QStringLiteral("adjustments"), 20, c.text, p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0));
        } else {
            // Text layers letterbox the whole run so every glyph stays legible;
            // other layers COVER the square with their own content. Both sit on
            // the transparency checkerboard (falling back to the flat swatch
            // until pixels exist).
            const QRect inner = thumb.adjusted(1, 1, -1, -1);
            const QImage preview = layerThumbnail(*d, layer, inner.width(), layer.isText);
            if (preview.isNull()) {
                p.fillRect(inner, layer.swatch);
            } else {
                constexpr int kCell = 5;
                for (int y = 0; y < inner.height(); y += kCell)
                    for (int x = 0; x < inner.width(); x += kCell)
                        p.fillRect(QRect(inner.left() + x, inner.top() + y, kCell, kCell)
                                       .intersected(inner),
                                   ((x / kCell + y / kCell) & 1)
                                       ? QColor(0x33, 0x33, 0x33)
                                       : QColor(0x44, 0x44, 0x44));
                p.drawImage(inner, preview);
            }
        }
        p.setPen(QPen(active ? c.accentText : c.border, 1));
        p.drawRect(thumb);

        // Layer mask thumbnail: actual coverage, not a placeholder swatch.
        if (layer.hasMask) {
            const QRect mask = layerMaskThumbRect(layer);
            const QImage preview = layerMaskThumbnail(
                layer, mask.adjusted(1, 1, -1, -1).width());
            if (preview.isNull()) {
                p.fillRect(mask, Qt::white);
            } else {
                p.drawImage(mask.adjusted(1, 1, -1, -1), preview);
            }
            p.setPen(QPen(layer.maskSelected ? c.accentText : c.border,
                          layer.maskSelected ? 2 : 1));
            p.drawRect(mask);
        }

        // Name plus clipping indicator.
        p.setPen(active ? c.accentText : c.text);
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        const QString prefix = layer.clipped ? QStringLiteral("⌐ ") : QString();
        p.drawText(nameRect(), Qt::AlignVCenter | Qt::AlignLeft, prefix + layer.name);

        // Effects + lock badges on the right. A collapsed group with children
        // additionally shows conventional mini stacked sheets, so the row
        // still signals that content is tucked away.
        int badge = width() - 20;
        if (isGroup && !layer.groupExpanded) {
            const int base = layer.indent;
            bool hasChildren = false;
            for (int i = index_ + 1; i < d->layers.size(); ++i) {
                if (d->layers[i].indent <= base) break;
                hasChildren = true;
            }
            if (hasChildren) {
                badge -= 18;
                for (int k = 2; k >= 0; --k)
                    p.fillRect(QRect(badge + (2 - k) * 3, 16 + k * 2, 8, 7),
                               k == 2 ? c.textDim.darker(170)
                                      : c.textDim.darker(120 + k * 25));
            }
        }
        if (layer.locked) {
            p.drawPixmap(QRect(badge, 13, 14, 14),
                         glyphPixmap(QStringLiteral("lock"), 14, c.textDim, p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0));
            badge -= 18;
        }
        if (!layer.style.empty()) {
            p.drawPixmap(QRect(badge, 13, 14, 14),
                         glyphPixmap(QStringLiteral("fx"), 14, c.textDim, p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0));
            badge -= 18;
        }
        if (layer.hasLiveFilter) {
            p.drawPixmap(QRect(badge, 13, 14, 14),
                         glyphPixmap(QStringLiteral("smart"), 14,
                                      layer.liveFilterEnabled ? c.textDim
                                                              : c.textDim.darker(180),
                                      p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0));
            badge -= 18;
        }

        // Drop-caret overlay: a line above/below the row, or a whole-row
        // highlight when the drop targets the group itself.
        if (drop_ && drop_->row == index_ && drop_->mode != LayerDropState::None) {
            if (drop_->mode == LayerDropState::IntoGroup) {
                p.setPen(QPen(c.accentText, 2));
                p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 3, 3);
            } else {
                p.fillRect(QRect(0, drop_->mode == LayerDropState::Above ? 0 : height() - 2,
                                 width(), 2),
                           c.accentText);
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= static_cast<int>(d->layers.size())) return;
        // Only the left button selects. A right-click must not collapse the
        // multi-selection: it is delivered as BOTH a press and a
        // contextMenuEvent, and the menu must operate on the set as it was.
        if (event->button() != Qt::LeftButton) return;
        pressWasMulti_ = false;
        // Ctrl+left-click on the thumbnail loads the layer's transparency as
        // the selection marquee (the Ctrl+click thumbnail): the layer's
        // alpha channel becomes the document selection mask, traced at 50%
        // opacity by the canvas's marching ants. This runs before the plain
        // selection logic so it never toggles the multi-selection too.
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const bool isGroup =
                d->layers[index_].kind == LayerItem::Kind::Group;
            const QRect thumb(26 + d->layers[index_].indent * 12 + (isGroup ? 14 : 0),
                              4, 32, 32);
            if (thumb.contains(event->position().toPoint())) {
                state_->selectFromLayerAlpha(index_);
                return;
            }
        }
        // Clicking the mask thumbnail targets the mask for painting (click
        // again to target pixels). Shift+click instead toggles the mask's
        // enabled state (the Shift+click mask thumbnail). The clicked
        // row becomes the selection so the menu and brush always operate on
        // what the user clicked.
        if (d->layers[index_].hasMask &&
            !event->modifiers().testFlag(Qt::AltModifier) &&
            layerMaskThumbRect(d->layers[index_])
                .contains(event->position().toPoint())) {
            if (!d->selectedLayers.contains(index_)) {
                d->selectedLayers.clear();
                d->selectedLayers.push_back(index_);
                d->activeLayer = index_;
                emit state_->activeLayerChanged();
            }
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                state_->setLayerMaskEnabled(!d->layers[index_].maskEnabled);
                return;
            }
            state_->setLayerMaskSelected(index_,
                                         !d->layers[index_].maskSelected);
            return;
        }
        // The disclosure chevron on a group row collapses/expands it. Plain =
        // this group; Ctrl/Cmd = every group; Alt/Option = every group nested
        // under this one. This is a UI-state toggle, never part of the
        // selection (matches standard editors).
        if (d->layers[index_].kind == LayerItem::Kind::Group) {
            const QRect chevron(26 + d->layers[index_].indent * 12, 13, 12, 12);
            if (chevron.contains(event->position().toPoint())) {
                state_->toggleLayerGroupExpanded(
                    index_, event->modifiers().testFlag(Qt::AltModifier),
                    event->modifiers().testFlag(Qt::ControlModifier));
                return;
            }
        }
        // Alt+click a row toggles its clipping mask (the Alt+click on
        // the line between two layers). The row becomes the selection first
        // so toggleLayerClipped acts on what was clicked.
        if (event->modifiers().testFlag(Qt::AltModifier) &&
            !event->modifiers().testFlag(Qt::ControlModifier) &&
            !event->modifiers().testFlag(Qt::ShiftModifier) &&
            event->position().x() >= 24) {
            if (d->activeLayer != index_ || !d->selectedLayers.contains(index_)) {
                d->selectedLayers.clear();
                d->selectedLayers.push_back(index_);
                d->activeLayer = index_;
                emit state_->activeLayerChanged();
            }
            state_->toggleLayerClipped();
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        dragPressPos_ = event->position();
        dragStarted_ = false;
        // Delivery latency: event->timestamp() is the compositor time (ms,
        // CLOCK_MONOTONIC on Wayland); steady_clock is CLOCK_MONOTONIC too.
        // When epochs align, this is the time the click waited before the row
        // handler ran — the last unmeasured gap before the eye-change code.
        const qint64 inputTs = event->timestamp();
        if (event->position().x() < 24) {
            // Multi-selection eye: clicking the eye of a selected row flips
            // every selected layer together (one undo step), so a Shift/Ctrl
            // set turns on/off at once. A click on an unselected row keeps
            // the single-layer path below.
            {
                const QVector<int> sel = state_->selectedLayerIndices();
                if (sel.size() > 1 && sel.contains(index_)) {
                    const bool makeVisible = !d->layers[index_].visible;
                    if (state_->setLayersVisible(sel, makeVisible)) {
                        // No rebuild and no scroll: repaint the visible rows
                        // in place (the panel repaints all rows on
                        // documentModified, but the eye path emits the narrow
                        // regionModified, so refresh the stack directly).
                        if (QWidget* p = parentWidget()) p->update();
                        update();
                        ::pittore::core::log::log_info(
                            "[layer] visibility bulk count=%d visible=%d",
                            sel.size(), makeVisible ? 1 : 0);
                    }
                    return;
                }
            }
            // One undo step per toggle: snapshot BEFORE the flip so reverting
            // restores the previous visibility of this layer.
            state_->beginUndoStep();
            d->layers[index_].visible = !d->layers[index_].visible;
            // A layer imported hidden behind the flattened base kept its native
            // pixels packed; showing it is what pays to expand them (the undo
            // snapshot above still holds the packed form, so undo repacks it).
            if (d->layers[index_].visible)
                materializeLayerPixels(d->layers[index_]);
            const QString name = d->layers[index_].name;
            const bool visible = d->layers[index_].visible;
            // Only the toggled layer's footprint can change, so recomposite
            // that rect from the visible stack (renderRegion) instead of a
            // full-canvas rebuild. A canvas-sized layer (the background)
            // still resolves to the whole frame — same cost, right answer.
            const QRectF sb =
                stageBounds(*d, d->layers[index_])
                    .intersected(QRectF(QPointF(0, 0), QSizeF(d->size)));
            if (sb.isEmpty()) {
                d->rebuildComposite();  // nothing on canvas changed; cheap-ish
            } else {
                d->recompositeRegion(sb.toAlignedRect());
            }
            const auto t1 = std::chrono::steady_clock::now();  // after composite
            if (sb.isEmpty()) {
                emit state_->documentModified(d);
            } else {
                // Narrow notice: only this rect changed. The canvas maps it to
                // a viewport update and nothing else repaints — the 1.2 ms
                // documentModified fanout (status/row sweep/preview) is skipped
                // because visibility changes none of them.
                state_->noteRegionEdit(sb);
                emit state_->regionModified(d, sb);
            }
            const auto t2 = std::chrono::steady_clock::now();  // after emit
            // The row paints its eye straight from d->layers[index_].visible,
            // so a repaint (not a 4101-widget rebuild) is all it needs.
            update();
            const auto t3 = std::chrono::steady_clock::now();  // after update
            // Promote the pending snapshot to the undo stack (one step per
            // toggle). Cost: a snapshot of the layer vector — µs even for the
            // 4101-part SVG bench — plus a historyChanged fan-out.
            state_->commitUndoStep(
                tr("%1 \"%2\"").arg(visible ? tr("Show") : tr("Hide"), name),
                QStringLiteral("eye"));
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            // Phase split shows whether a toggle's time is compositing (the
            // GPU pass) or the emit fanout (panels/canvas/status).
            ::pittore::core::log::log_info(
                "[layer] visibility toggle index=%d name=\"%s\" visible=%d ms=%.2f "
                "rect=%dx%d comp=%.2f emit=%.2f upd=%.2f",
                index_, name.toUtf8().constData(), visible ? 1 : 0, ms,
                sb.width() > 0 ? static_cast<int>(sb.width()) : d->size.width(),
                sb.height() > 0 ? static_cast<int>(sb.height()) : d->size.height(),
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count(),
                std::chrono::duration<double, std::milli>(t3 - t2).count());
            // Delivery latency for this click (compositor → handler). Log raw
            // values: if `input_ts` ≈ `now_ms`, epochs are aligned and the
            // delta is the true queueing delay; if wildly different, the
            // compositor clock base differs and only relative drifts matter.
            if (inputTs > 0) {
                const qint64 handlerNow =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                ::pittore::core::log::log_info(
                    "[layer] input_delivery input_ts_ms=%lld "
                    "handler_now_ms=%lld delta_ms=%lld",
                    inputTs, handlerNow, handlerNow - inputTs);
            }
            return;
        }
        // Selection: plain click isolates the row (deferred to release when
        // the row already belongs to a multi-selection, so a press-drag
        // carries the whole set); Ctrl+click toggles it in
        // the multi-selection; Shift+click selects the contiguous range from
        // the anchor (active layer) to the clicked row — Ctrl+Shift unions
        // the range with the existing set. The set moves/deletes/eyes together.
        const bool shiftSel = event->modifiers().testFlag(Qt::ShiftModifier);
        const bool ctrlSel = event->modifiers().testFlag(Qt::ControlModifier);
        if (shiftSel) {
            state_->selectLayerRange(index_, ctrlSel);
        } else if (ctrlSel) {
            const int pos = d->selectedLayers.indexOf(index_);
            if (pos >= 0) {
                d->selectedLayers.removeAt(pos);
                // Never leave the active layer outside the selection when the
                // set is non-empty; with everything deselected it stays as the
                // fallback anchor so Delete/Move still know what to act on.
                if (d->activeLayer == index_ && !d->selectedLayers.isEmpty())
                    d->activeLayer = d->selectedLayers.first();
            } else {
                if (d->selectedLayers.isEmpty()) d->selectedLayers.push_back(d->activeLayer);
                d->selectedLayers.push_back(index_);
                d->activeLayer = index_;
            }
        } else if (d->selectedLayers.contains(index_) &&
                   d->selectedLayers.size() > 1) {
            // Press on a member of a multi-selection: keep the set so a drag
            // moves every selected row, and isolate this row on release when
            // no drag happened (plain click). Only the active layer follows
            // the press now, for the canvas gizmo.
            pressWasMulti_ = true;
            d->activeLayer = index_;
        } else {
            if (d->selectedLayers.size() == 1 && d->selectedLayers.contains(index_) &&
                d->activeLayer == index_)
                return;  // already the only selected + active layer
            d->selectedLayers.clear();
            d->selectedLayers.push_back(index_);
            d->activeLayer = index_;
        }
        const QString name = d->layers[index_].name;
        // Selection-only broadcast: rows repaint their highlight from live
        // state, the spinbox header syncs, and the canvas redraws its gizmo —
        // none of which needs (or should pay for) a full panel rebuild.
        emit state_->activeLayerChanged();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        ::pittore::core::log::log_info(
            "[layer] select index=%d name=\"%s\" ms=%.2f emit=%.2f", index_,
            name.toUtf8().constData(), ms,
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        // Double-clicking the layer name opens the inline rename; a double-click
        // anywhere else on the row just re-selects it.
        if (nameRect().contains(event->position().toPoint())) {
            startRename();
            return;
        }
        state_->setActiveLayerIndex(index_);
    }

    // --- drag to reorder / move into another group ---------------------------
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton) || dragStarted_) return;
        if ((event->position() - dragPressPos_).manhattanLength() < 8) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= d->layers.size()) return;
        // Drag only from the body of a row, not the visibility eye.
        if (state_->selectedLayerIndices().isEmpty()) return;
        dragStarted_ = true;
        pressWasMulti_ = false;   // the whole set rides the drag; release
                                  // must not isolate the pressed row after it
        QMimeData* mime = new QMimeData;
        QByteArray payload;
        QDataStream ds(&payload, QIODevice::WriteOnly);
        ds << state_->selectedLayerIndices();
        mime->setData(QLatin1String(kLayerDragMime), payload);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(grab());
        drag->exec(Qt::MoveAction, Qt::MoveAction);
        dragStarted_ = false;
        if (LayerDropState* ds = drop_) {
            ds->row = -1;
            ds->mode = LayerDropState::None;
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        // End of a deferred multi-selection press (no drag happened): a plain
        // click isolates the released row. A release after a drag, or with a
        // selection modifier held, leaves the set alone.
        const bool isolate = pressWasMulti_;
        pressWasMulti_ = false;
        if (event->button() != Qt::LeftButton || !isolate || dragStarted_)
            return;
        if (event->modifiers().testFlag(Qt::ShiftModifier) ||
            event->modifiers().testFlag(Qt::ControlModifier) ||
            event->modifiers().testFlag(Qt::AltModifier))
            return;
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= static_cast<int>(d->layers.size())) return;
        d->selectedLayers.clear();
        d->selectedLayers.push_back(index_);
        d->activeLayer = index_;
        emit state_->activeLayerChanged();
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasFormat(QLatin1String(kLayerDragMime)))
            event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (!event->mimeData()->hasFormat(QLatin1String(kLayerDragMime))) return;
        updateDropZone(event->position());
        event->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent*) override {
        clearDropZone();
    }

    void dropEvent(QDropEvent* event) override {
        if (!event->mimeData()->hasFormat(QLatin1String(kLayerDragMime))) return;
        DocumentItem* d = state_->activeDocument();
        if (d) {
            const QPointF pos = event->position();
            updateDropZone(pos);
            const int targetIndex = [&] {
                switch (dropZoneMode(pos)) {
                    case LayerDropState::Above:    return index_;
                    case LayerDropState::IntoGroup: return index_ + 1;
                    default:                       return index_ + 1;
                }
            }();
            const int targetIndent = [&] {
                const LayerItem& l = d->layers[index_];
                return dropZoneMode(pos) == LayerDropState::IntoGroup
                           ? l.indent + 1
                           : l.indent;
            }();
            state_->reparentSelectedLayers(targetIndex, targetIndent);
        }
        event->acceptProposedAction();
        clearDropZone();
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= d->layers.size()) return;
        // Right-click on an unselected row selects it before acting (so the
        // menu always operates on what the user clicked).
        if (!d->selectedLayers.contains(index_)) {
            d->selectedLayers.clear();
            d->selectedLayers.push_back(index_);
            d->activeLayer = index_;
            emit state_->activeLayerChanged();
        }
        QMenu menu(this);
        QAction* selAll = menu.addAction(tr("Select All Layers"));
        QAction* desel = menu.addAction(tr("Deselect Layers"));
        QAction* rename = menu.addAction(tr("Rename Layer…"));
        const int dupCount = state_->selectedLayerIndices().size();
        QAction* duplicate = menu.addAction(dupCount > 1 ? tr("Duplicate Layers")
                                                          : tr("Duplicate Layer…"));
        const LayerItem& clicked = d->layers[index_];
        const bool isPixel = clicked.kind == LayerItem::Kind::Pixel;
        menu.addSeparator();
        QAction* addMask = menu.addAction(
            clicked.hasMask ? tr("Select Layer Mask") : tr("Add Layer Mask"));
        addMask->setEnabled(isPixel);
        QAction* deleteMask = menu.addAction(tr("Delete Layer Mask"));
        deleteMask->setEnabled(isPixel && clicked.hasMask);
        QAction* invertMask = menu.addAction(tr("Invert Layer Mask"));
        invertMask->setEnabled(isPixel && clicked.hasMask);
        QAction* applyMask = menu.addAction(tr("Apply Layer Mask"));
        applyMask->setEnabled(isPixel && clicked.hasMask && clicked.pixels);
        QAction* enableMask = menu.addAction(clicked.maskEnabled
                                                 ? tr("Disable Layer Mask")
                                                 : tr("Enable Layer Mask"));
        enableMask->setEnabled(isPixel && clicked.hasMask);
        QAction* linkMask = menu.addAction(clicked.maskLinked
                                               ? tr("Unlink Layer Mask")
                                               : tr("Link Layer Mask"));
        linkMask->setEnabled(isPixel && clicked.hasMask);
        QAction* loadSel = menu.addAction(tr("Load Mask as Selection"));
        loadSel->setEnabled(isPixel && clicked.hasMask && clicked.mask);
        QAction* clip = menu.addAction(
            clicked.clipped ? tr("Release Clipping Mask")
                            : tr("Create Clipping Mask"),
            QKeySequence(QLatin1String("Ctrl+Alt+G")));
        clip->setEnabled(isPixel);
        QAction* toggleFilter = menu.addAction(
            clicked.hasLiveFilter && clicked.liveFilterEnabled
                ? tr("Disable Live Filter")
                : tr("Enable Live Filter"));
        toggleFilter->setEnabled(isPixel && clicked.hasLiveFilter);
        QAction* removeFilter = menu.addAction(tr("Remove Live Filter"));
        removeFilter->setEnabled(isPixel && clicked.hasLiveFilter);
        menu.addSeparator();
        // The hints mirror the menu bar's accelerators (Ctrl+G / Ctrl+Shift+G
        // / the Arrange set); QMenu renders them as right-aligned cues.
        QAction* group =
            menu.addAction(tr("Group Layers"), QKeySequence(QLatin1String("Ctrl+G")));
        QAction* ungroup = menu.addAction(tr("Ungroup Layers"),
                                          QKeySequence(QLatin1String("Ctrl+Shift+G")));
        QAction* toneMake = menu.addAction(tr("Make Tone Blend Group"));
        QAction* toneSettings = menu.addAction(tr("Tone Blend Settings…"));
        toneSettings->setEnabled(clicked.kind == LayerItem::Kind::Group &&
                                 clicked.toneBlendGroup);
        QAction* expandAll = menu.addAction(tr("Expand All Groups"));
        QAction* collapseAll = menu.addAction(tr("Collapse All Groups"));
        menu.addSeparator();
        QAction* front = menu.addAction(tr("Bring to Front"),
                                        QKeySequence(QLatin1String("Ctrl+Shift+]")));
        QAction* forward =
            menu.addAction(tr("Bring Forward"), QKeySequence(QLatin1String("Ctrl+]")));
        QAction* backward =
            menu.addAction(tr("Send Backward"), QKeySequence(QLatin1String("Ctrl+[")));
        QAction* back = menu.addAction(tr("Send to Back"),
                                       QKeySequence(QLatin1String("Ctrl+Shift+[")));
        menu.addSeparator();
        QAction* del = menu.addAction(tr("Delete Layer"), QKeySequence(QKeySequence::Delete));
        QAction* chosen = menu.exec(event->globalPos());
        if (!chosen) return;
        if (chosen == selAll)
            state_->selectAllLayers();
        else if (chosen == desel)
            state_->clearLayerSelection();
        else if (chosen == rename)
            startRename();
        else if (chosen == duplicate)
            state_->duplicateSelectedLayers();
        else if (chosen == addMask)
            state_->addLayerMask(1.0f);
        else if (chosen == deleteMask)
            state_->deleteLayerMask();
        else if (chosen == invertMask)
            state_->invertLayerMask();
        else if (chosen == applyMask)
            state_->applyLayerMask();
        else if (chosen == enableMask)
            state_->setLayerMaskEnabled(!d->layers[index_].maskEnabled);
        else if (chosen == linkMask)
            state_->setLayerMaskLinked(!d->layers[index_].maskLinked);
        else if (chosen == loadSel)
            state_->loadMaskAsSelection();
        else if (chosen == toggleFilter)
            state_->setLiveFilterEnabled(!d->layers[index_].liveFilterEnabled);
        else if (chosen == removeFilter)
            state_->removeLiveFilter();
        else if (chosen == clip)
            state_->toggleLayerClipped();
        else if (chosen == group)
            state_->groupSelectedLayers();
        else if (chosen == ungroup)
            state_->ungroupSelectedLayers();
        else if (chosen == toneMake) {
            const int g = state_->makeToneBlendGroup();
            if (g >= 0) emit state_->toneBlendRequested(g);
        } else if (chosen == toneSettings)
            emit state_->toneBlendRequested(index_);
        else if (chosen == expandAll)
            state_->setAllGroupsExpanded(true);
        else if (chosen == collapseAll)
            state_->setAllGroupsExpanded(false);
        else if (chosen == front)
            state_->bringSelectedLayersToFront();
        else if (chosen == forward)
            state_->moveSelectedLayersInStack(-1);
        else if (chosen == backward)
            state_->moveSelectedLayersInStack(1);
        else if (chosen == back)
            state_->sendSelectedLayersToBack();
        else if (chosen == del)
            state_->removeSelectedLayers();
    }

  private:
    // The rect the row paints the layer name into (after the preview throttles
    // and the mask thumb, leaving the badge strip on the right). Shared by
    // paintEvent and the inline rename editor so both align exactly.
    QRect nameRect() const {
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ >= d->layers.size()) return {};
        const LayerItem& layer = d->layers[index_];
        const int indent = layer.indent * 12;
        // Group rows keep room for the disclosure chevron before the preview.
        int x = 26 + indent + 32 + 6;
        if (layer.kind == LayerItem::Kind::Group) x += 14;
        if (layer.hasMask) x = layerMaskThumbRect(layer).right() + 6;
        return QRect(x, 0, width() - x - 46, height());
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        // Esc inside the inline editor cancels the rename instead of accepting
        // QLineEdit's default (commit-on-Escape semantics vary by platform).
        if (obj == renameEdit_ && event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                cancelRename();
                return true;
            }
        }
        return QWidget::eventFilter(obj, event);
    }

    // Commits the edited name as one undoable step. Safe to run twice: the
    // renaming_ flag turns the focus-out fired by hide() into a no-op.
    void finishRename() {
        if (!renaming_ || !renameEdit_) return;
        renaming_ = false;
        renameEdit_->hide();
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ < 0 || index_ >= d->layers.size()) return;
        const QString name = d->layers[index_].name;
        const QString next = renameEdit_->text().trimmed();
        if (next.isEmpty() || next == name) return;
        state_->beginUndoStep();
        d->layers[index_].name = next;
        // commitUndoStep pushes the snapshot and emits historyChanged. The
        // layersChanged rebuild repaints every name consumer; documentModified
        // marks the document dirty.
        state_->commitUndoStep(tr("Rename Layer"), QStringLiteral("rename"));
        emit state_->documentModified(d);
        emit state_->layersChanged();
    }

    void cancelRename() {
        if (!renaming_) return;
        renaming_ = false;
        if (renameEdit_) renameEdit_->hide();
    }

    LayerDropState::Mode dropZoneMode(const QPointF& pos) const {
        DocumentItem* d = state_->activeDocument();
        if (!d || index_ >= d->layers.size()) return LayerDropState::None;
        const LayerItem& l = d->layers[index_];
        if (l.kind == LayerItem::Kind::Group) {
            // Group rows: the top quarter places above, the rest drops INTO
            // the group (above its current children).
            return pos.y() < height() * 0.25 ? LayerDropState::Above
                                             : LayerDropState::IntoGroup;
        }
        return pos.y() < height() * 0.5 ? LayerDropState::Above
                                        : LayerDropState::Below;
    }

    void updateDropZone(const QPointF& pos) {
        if (!drop_) return;
        const LayerDropState::Mode mode = dropZoneMode(pos);
        // Repaint the previously highlighted row and this one.
        if (drop_->row >= 0 && drop_->row != index_) {
            // find the row widget for the old index and repaint it
            if (QWidget* w = siblingRow(drop_->row)) w->update();
        }
        drop_->row = index_;
        drop_->mode = mode;
        update();
    }

    void clearDropZone() {
        if (drop_ && drop_->row >= 0) {
            if (QWidget* w = siblingRow(drop_->row)) w->update();
            drop_->row = -1;
            drop_->mode = LayerDropState::None;
        }
    }

    // The sibling LayerRow widget for a document layer index, or null when
    // the index is off-screen (only the visible window is pooled).
    QWidget* siblingRow(int index) const {
        if (!siblings_) return nullptr;
        for (LayerRow* row : *siblings_) {
            if (row && row->layerIndex() == index) return row;
        }
        return nullptr;
    }

    AppState* state_;
    int index_;
    LayerDropState* drop_ = nullptr;
    // Panel's row pool (stable address): sibling lookup without the layout.
    QVector<LayerRow*>* siblings_ = nullptr;
    QPointF dragPressPos_;
    bool dragStarted_ = false;
    // True between a plain press on a member of a multi-selection and its
    // release (no drag yet): release isolates the row, a drag carries the set.
    bool pressWasMulti_ = false;
    QLineEdit* renameEdit_ = nullptr;   // inline name editor (hidden by default)
    bool renaming_ = false;             // true while the editor is open
};

class LayersPanel final : public QWidget {
  public:
    LayersPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, 4, 0, 0);
        column->setSpacing(8);

        // Filter row.
        auto* filter = new QHBoxLayout;
        filter->setContentsMargins(10, 0, 10, 0);
        filter->setSpacing(8);
        auto* kind = new QComboBox(this);
        kind->addItems({tr("Kind"), tr("Name"), tr("Effect"), tr("Mode"), tr("Attribute"),
                        tr("Color"), tr("Smart Object"), tr("Selected"), tr("Artboard")});
        filter->addWidget(kind);
        for (const char* key : {"eye", "adjustments", "type", "shape-rect", "libraries"}) {
            auto* b = footerButton(state_, QString::fromUtf8(key), QString(), this);
            b->setCheckable(true);
            filter->addWidget(b);
        }
        filter->addStretch(1);
        column->addLayout(filter);

        // Blend mode + opacity.
        auto* blend = new QHBoxLayout;
        blend->setContentsMargins(10, 0, 10, 0);
        blend->setSpacing(8);
        blendMode_ = new QComboBox(this);
        for (int i = 0; i < blendModeNames().size(); ++i) {
            if (blendModeIsSeparatorBefore(i)) blendMode_->insertSeparator(blendMode_->count());
            blendMode_->addItem(blendModeNames().at(i));
        }
        blend->addWidget(blendMode_, 1);
        blend->addWidget(sectionLabel(tr("Opacity:"), state_, this));
        // Stylus-sized slider beside the precise spinbox: dragging the
        // slider writes through the spinbox so both stay in sync.
        opacitySlider_ = new QSlider(Qt::Horizontal, this);
        opacitySlider_->setRange(0, 100);
        opacitySlider_->setValue(100);
        opacitySlider_->setMinimumHeight(30);
        opacitySlider_->setStyleSheet(
            QStringLiteral(
                "QSlider::groove:horizontal { height: 10px; }"
                "QSlider::handle:horizontal { width: 26px; height: 26px; "
                "margin: -8px 0; }"));
        blend->addWidget(opacitySlider_, 1);
        opacity_ = new QSpinBox(this);
        opacity_->setRange(0, 100);
        opacity_->setValue(100);
        opacity_->setSuffix(QStringLiteral("%"));
        opacity_->setFixedWidth(62);
        blend->addWidget(opacity_);
        column->addLayout(blend);

        // Lock row + fill.
        auto* locks = new QHBoxLayout;
        locks->setContentsMargins(10, 0, 10, 0);
        locks->setSpacing(8);
        locks->addWidget(sectionLabel(tr("Lock:"), state_, this));
        lockButtons_.clear();
        for (auto&& entry : {std::pair<const char*, const char*>{"swatches", "Lock transparent pixels"},
                             {"brush", "Lock image pixels"},
                             {"move", "Lock position"},
                             {"artboard", "Prevent auto-nesting"},
                             {"lock", "Lock all"}}) {
            auto* b = footerButton(state_, QString::fromUtf8(entry.first),
                                   tr(entry.second), this);
            b->setCheckable(true);
            locks->addWidget(b);
            lockButtons_.append(b);
        }
        // Only full-layer lock exists in the model (LayerItem::locked): the
        // per-aspect locks stay visible but disabled with the reason, instead
        // of checkable buttons whose clicks do nothing.
        for (int i = 0; i < 4 && i < lockButtons_.size(); ++i) {
            lockButtons_[i]->setEnabled(false);
            lockButtons_[i]->setToolTip(
                lockButtons_[i]->toolTip() + tr(" — not available yet."));
        }
        if (lockButtons_.size() == 5) {
            connect(lockButtons_[4], &QToolButton::toggled, this,
                    [this](bool on) { setSelectedLocked(on); });
        }
        locks->addStretch(1);
        locks->addWidget(sectionLabel(tr("Fill:"), state_, this));
        fill_ = new QSpinBox(this);
        fill_->setRange(0, 100);
        fill_->setValue(100);
        fill_->setSuffix(QStringLiteral("%"));
        fill_->setFixedWidth(62);
        locks->addWidget(fill_);
        column->addLayout(locks);

        // Layer stack.
        scroll_ = new QScrollArea(this);
        scroll_->setWidgetResizable(true);
        scroll_->setFrameShape(QFrame::NoFrame);
        stack_ = new QWidget(scroll_);
        stackLayout_ = new QVBoxLayout(stack_);
        stackLayout_->setContentsMargins(0, 0, 0, 0);
        stackLayout_->setSpacing(0);
        stackLayout_->addStretch(1);
        scroll_->setWidget(stack_);
        // Drops on the empty area below the last row land at the end of the
        // stack (the stretch, not a row, receives them).
        stack_->setAcceptDrops(true);
        stack_->installEventFilter(this);
        // Windowed rows: scrolling rebinds the pool to the new window.
        connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { layoutWindow(false); });
        column->addWidget(scroll_, 1);

        QWidget* footer = makePanelFooter(
            state_,
            {{QStringLiteral("link"), tr("Link layers")},
             {QStringLiteral("fx"), tr("Add a layer style")},
             {QStringLiteral("mask"), tr("Add a layer mask")},
             {QStringLiteral("adjustments"), tr("Create new fill or adjustment layer")},
             {QStringLiteral("group"), tr("Create a new group")},
             {QStringLiteral("newlayer"), tr("Create a new layer")},
             {QStringLiteral("trash"), tr("Delete layer")}},
            this, [this](const QString& id) { onFooter(id); });
        column->addWidget(footer);
        // Hero treatment for the fx button: same row height, but a wider
        // button with the 24px glyph so it reads at a glance. Footer buttons
        // are created in list order, so index 1 is fx.
        {
            const QList<QToolButton*> buttons =
                footer->findChildren<QToolButton*>();
            if (buttons.size() > 1) {
                QToolButton* fx = buttons.at(1);
                fx->setFixedSize(34, 22);
                fx->setIconSize(QSize(21, 21));
            }
        }

        connect(state_, &AppState::layersChanged, this, [this] { rebuild(); });
        // A fresh group header asks to be named right away: open the inline
        // editor on it (groupSelectedLayers emits this AFTER layersChanged, so
        // the rows have already been rebuilt when this handler runs).
        connect(state_, &AppState::groupCreated, this, [this](int index) {
            if (QWidget* w = rowWidget(index)) static_cast<LayerRow*>(w)->startRename();
        });
        // Selection-only change: repaint the rows (they draw the highlight from
        // live activeLayer) and sync the header spinboxes — no 4101-widget
        // rebuild, which would make clicking a layer cost milliseconds.
        connect(state_, &AppState::activeLayerChanged, this,
                [this] { syncActive(); });
        connect(state_, &AppState::activeDocumentChanged, this, [this] { rebuild(); });
        connect(state_, &AppState::themeChanged, this, [this] { rebuild(); });
        // A move/resize drag or a paint stroke marks the touched layer's cached
        // preview stale; repaint the rows so the thumbnail follows. Untouched
        // layers keep their cached image, so only the edited row re-samples.
        // One container repaint covers the visible rows: per-row update()
        // posts one paint event each (tens of thousands on huge documents),
        // flooding the event queue for identical pixels.
        connect(state_, &AppState::documentModified, this, [this](DocumentItem*) {
            if (stack_) stack_->update();
        });
        connect(opacitySlider_, &QSlider::valueChanged, this, [this](int v) {
            if (opacity_ && opacity_->value() != v) opacity_->setValue(v);
        });
        // A slider drag is one undo step: begin on press, live-write through
        // the spinbox handler below, commit on release. Without a press
        // (keyboard nudge) the handler wraps each change singly.
        connect(opacitySlider_, &QSlider::sliderPressed, this, [this] {
            if (state_->activeLayer() && !state_->undoStepActive()) {
                state_->beginUndoStep();
                opacityGesture_ = true;
                opacityStart_ = opacitySlider_->value();
            }
        });
        connect(opacitySlider_, &QSlider::sliderReleased, this, [this] {
            if (opacityGesture_) {
                opacityGesture_ = false;
                // A press without movement leaves no step behind.
                if (opacitySlider_->value() == opacityStart_)
                    state_->discardUndoStep();
                else
                    state_->commitUndoStep(tr("Layer Opacity"), QStringLiteral("opacity"));
            }
        });
        connect(opacity_, &QSpinBox::valueChanged, this, [this](int v) {
            if (opacitySlider_ && opacitySlider_->value() != v) {
                const QSignalBlocker b(opacitySlider_);
                opacitySlider_->setValue(v);
            }
        });
        connect(opacity_, &QSpinBox::valueChanged, this, [this](int v) {
            LayerItem* l = state_->activeLayer();
            if (!l || l->opacity == v) return;
            if (state_->undoStepActive()) {
                l->opacity = v;
            } else {
                state_->beginUndoStep();
                l->opacity = v;
                state_->commitUndoStep(tr("Layer Opacity"), QStringLiteral("opacity"));
            }
            if (DocumentItem* d = state_->activeDocument()) d->rebuildComposite();
            emit state_->documentModified(state_->activeDocument());
        });
        connect(fill_, &QSpinBox::valueChanged, this, [this](int v) {
            LayerItem* l = state_->activeLayer();
            if (!l || l->fill == v) return;
            state_->beginUndoStep();
            l->fill = v;
            state_->commitUndoStep(tr("Fill"), QStringLiteral("fill"));
            if (DocumentItem* d = state_->activeDocument()) d->rebuildComposite();
            emit state_->documentModified(state_->activeDocument());
        });
        connect(blendMode_, &QComboBox::currentTextChanged, this, [this](const QString& mode) {
            LayerItem* l = state_->activeLayer();
            if (!l || l->blendMode == mode) return;
            state_->beginUndoStep();
            l->blendMode = mode;
            state_->commitUndoStep(tr("Blend Mode"), QStringLiteral("blend"));
            if (DocumentItem* d = state_->activeDocument()) d->rebuildComposite();
            emit state_->documentModified(state_->activeDocument());
        });

        rebuild();
    }

  private:
    void onFooter(const QString& id) {
        DocumentItem* d = state_->activeDocument();
        if (!d) return;
        if (id == QLatin1String("newlayer")) {
            LayerItem layer;
            layer.name = tr("Layer %1").arg(d->layers.size() + 1);
            layer.swatch = QColor::fromHsv((d->layers.size() * 47) % 360, 120, 190);
            state_->beginUndoStep();
            state_->addLayer(layer);
            state_->commitUndoStep(tr("New Layer"), QStringLiteral("newlayer"));
        } else if (id == QLatin1String("group")) {
            LayerItem group;
            group.kind = LayerItem::Kind::Group;
            group.name = tr("Group %1").arg(d->layers.size());
            state_->beginUndoStep();
            state_->addLayer(group);
            state_->commitUndoStep(tr("New Group"), QStringLiteral("group"));
            // A brand-new group should be named right away (addLayer emitted
            // layersChanged, so the row for the new active layer exists now).
            if (QWidget* w = rowWidget(d->activeLayer))
                static_cast<LayerRow*>(w)->startRename();
        } else if (id == QLatin1String("mask")) {
            if (state_->addLayerMask(1.0f)) rebuild();
        } else if (id == QLatin1String("adjustments")) {
            LayerItem layer;
            layer.kind = LayerItem::Kind::Adjustment;
            layer.adjustmentType = QStringLiteral("Hue/Saturation");
            layer.name = layer.adjustmentType;
            layer.swatch = QColor(0x9a, 0x72, 0xd0);
            state_->beginUndoStep();
            state_->addLayer(layer);
            state_->commitUndoStep(tr("Adjustment Layer"), QStringLiteral("adjustments"));
        } else if (id == QLatin1String("fx")) {
            // The window owns the Layer Style dialog; the panel only asks.
            if (state_->activeLayer()) emit state_->layerStyleRequested(-1);
        } else if (id == QLatin1String("trash")) {
            state_->removeSelectedLayers();
        }
    }

    void syncActive() {
        // Rows read d->activeLayer live, so repainting is enough (Qt clips the
        // paint to the rows actually visible in the scroll area). One
        // container repaint, not one event per row: per-row update() posts
        // tens of thousands of paint events on huge documents.
        if (stack_) stack_->update();
        if (LayerItem* l = state_->activeLayer()) {
            QSignalBlocker b1(opacity_), b2(fill_), b3(blendMode_);
            const QSignalBlocker b1s(opacitySlider_);
            opacity_->setValue(l->opacity);
            if (opacitySlider_) opacitySlider_->setValue(l->opacity);
            fill_->setValue(l->fill);
            blendMode_->setCurrentText(l->blendMode);
            if (lockButtons_.size() == 5) {
                const QSignalBlocker b4(lockButtons_[4]);
                lockButtons_[4]->setChecked(l->locked);
            }
        }
        // Click-an-image-to-find-its-layer: the canvas selects the layer under
        // the cursor, and the panel follows by scrolling to its row.
        ensureActiveVisible();
    }

    // Lock-all toggle: flips LayerItem::locked on every selected layer as one
    // undo step (locking is interaction state — no recomposite needed, just a
    // row repaint for the badges).
    void setSelectedLocked(bool on) {
        DocumentItem* d = state_->activeDocument();
        if (!d || d->layers.isEmpty()) return;
        const QVector<int> sel = state_->selectedLayerIndices();
        bool any = false;
        for (int idx : sel) {
            if (idx < 0 || idx >= d->layers.size()) continue;
            if (d->layers[idx].locked != on) {
                any = true;
                break;
            }
        }
        if (!any) return;
        state_->beginUndoStep();
        for (int idx : sel) {
            if (idx < 0 || idx >= d->layers.size()) continue;
            d->layers[idx].locked = on;
        }
        state_->commitUndoStep(on ? tr("Lock Layers") : tr("Unlock Layers"),
                               QStringLiteral("lock"));
        emit state_->layersChanged();
    }

    // Scroll the stack so the active layer's row is visible, expanding any
    // collapsed ancestor groups that hide it. Called on every
    // activeLayerChanged (canvas picks, panel clicks, undo) and, via rebuild(),
    // when a structural change moves the selection. Pure view state: never
    // touches undo or the composite.
    void ensureActiveVisible() {
        DocumentItem* d = state_->activeDocument();
        if (!d || d->layers.isEmpty()) {
            lastDoc_ = d;
            lastActive_ = -1;
            return;
        }
        const int active =
            qBound(0, d->activeLayer, d->layers.size() - 1);
        lastDoc_ = d;
        lastActive_ = active;
        // A canvas pick inside a collapsed group would otherwise scroll to a
        // hidden row (a no-op): reveal the whole ancestor chain first, like
        // standard editors do.
        bool revealed = false;
        for (int g : d->enclosingGroups(active)) {
            if (g >= 0 && g < d->layers.size() &&
                d->layers[g].kind == LayerItem::Kind::Group &&
                !d->layers[g].groupExpanded) {
                d->layers[g].groupExpanded = true;
                revealed = true;
            }
        }
        if (revealed) rebuild();   // rebuilds rows; scrolls again below
        scrollToLayer(active);
    }

    // Scroll the stack so the given layer's row is visible. Pooled rows
    // only exist inside the window, so move the window first, then nudge
    // the scrollbar to the row's absolute position.
    void scrollToLayer(int index) {
        const int pos = orderPos(index);
        if (pos < 0 || !scroll_) return;
        const int y = pos * kRowHeight;
        const int vy = scroll_->verticalScrollBar()->value();
        const int vh = scroll_->viewport()->height();
        if (y < vy || y + kRowHeight > vy + vh) {
            scroll_->verticalScrollBar()->setValue(
                qMax(0, y - vh / 2));
        }
        layoutWindow(false);
        if (QWidget* w = rowWidget(index))
            scroll_->ensureWidgetVisible(w, 0, 40);
    }

    bool eventFilter(QObject* obj, QEvent* event) override {
        // Manual row layout: rows keep the stack width on panel resizes.
        // Rare (dock resize) and position-only, so a single linear pass.
        // A wider viewport may fit more rows: grow the pool, then rebind.
        if (obj == stack_ && event->type() == QEvent::Resize) {
            const int w = stack_->width();
            ensurePool(poolNeeded());
            for (LayerRow* row : rows_) {
                if (row->width() != w) row->resize(w, kRowHeight);
            }
            layoutWindow(false);
        }
        // End-of-list drops: a drag over the stack's empty area (below the last
        // row) shows a caret under the last row and appends on drop.
        if (obj == stack_ && event->type() == QEvent::DragEnter) {
            auto* e = static_cast<QDragEnterEvent*>(event);
            if (e->mimeData()->hasFormat(QLatin1String(kLayerDragMime))) {
                e->acceptProposedAction();
                return true;
            }
            return false;
        }
        if (obj == stack_ && event->type() == QEvent::DragMove) {
            auto* e = static_cast<QDragMoveEvent*>(event);
            if (!e->mimeData()->hasFormat(QLatin1String(kLayerDragMime))) return false;
            DocumentItem* d = state_->activeDocument();
            if (d) {
                const int last = d->layers.size() - 1;
                if (drop_.row != last || drop_.mode != LayerDropState::Below) {
                    if (QWidget* w = rowWidget(drop_.row)) w->update();
                    drop_.row = last;
                    drop_.mode = LayerDropState::Below;
                    if (QWidget* w = rowWidget(last)) w->update();
                }
            }
            e->acceptProposedAction();
            return true;
        }
        if (obj == stack_ && event->type() == QEvent::DragLeave) {
            clearDropCaret();
            return false;
        }
        if (obj == stack_ && event->type() == QEvent::Drop) {
            auto* e = static_cast<QDropEvent*>(event);
            if (!e->mimeData()->hasFormat(QLatin1String(kLayerDragMime))) return false;
            DocumentItem* d = state_->activeDocument();
            if (d) {
                const int last = d->layers.size() - 1;
                const int targetIndent =
                    last >= 0 ? d->layers[last].indent : 0;
                state_->reparentSelectedLayers(d->layers.size(), targetIndent);
            }
            clearDropCaret();
            e->acceptProposedAction();
            return true;
        }
        return QWidget::eventFilter(obj, event);
    }

    // The LayerRow widget currently bound to a document layer index, or
    // null when the index is off-screen (the pool only covers the visible
    // window, so off-screen rows have no widget by design).
    QWidget* rowWidget(int index) const {
        for (LayerRow* row : rows_) {
            if (row->layerIndex() == index) return row;
        }
        return nullptr;
    }

    // Position of a layer in the visible order, or -1 when hidden/unknown.
    int orderPos(int index) const {
        for (int p = 0; p < order_.size(); ++p) {
            if (order_[p] == index) return p;
        }
        return -1;
    }

    // Pool size for the current viewport: visible rows plus overscan above
    // and below so normal scrolling rebinds without creating widgets.
    // A small floor keeps tiny panels (and tests) on the single-pass path.
    int poolNeeded() const {
        const int vh = scroll_ && scroll_->viewport()
                           ? scroll_->viewport()->height()
                           : 0;
        const int vis = vh > 0 ? vh / kRowHeight + 1 : 64;
        return vis + 2 * kWindowOver;
    }

    void ensurePool(int need) {
        while (rows_.size() < need) {
            rows_.append(new LayerRow(state_, -1, &drop_, stack_, &rows_));
        }
    }

    // Bind the pool to the window around the scroll position. Absolute
    // y positions keep drag/drop, rename and paint code untouched: a pooled
    // row is indistinguishable from a dedicated one.
    void layoutWindow(bool force) {
        DocumentItem* d = state_->activeDocument();
        if (!d || order_.isEmpty()) return;
        const int vy = scroll_ ? scroll_->verticalScrollBar()->value() : 0;
        const int w = stack_->width();
        ensurePool(poolNeeded());
        const int maxBase = order_.size() <= rows_.size()
                                ? 0
                                : order_.size() - rows_.size();
        const int want =
            qBound(0, vy / kRowHeight - kWindowOver, maxBase);
        if (!force && want == base_) return;
        base_ = want;
        stack_->setUpdatesEnabled(false);
        for (int s = 0; s < rows_.size(); ++s) {
            LayerRow* row = rows_[s];
            const int pos = base_ + s;
            if (pos >= order_.size()) {
                row->hide();
                continue;
            }
            row->setIndex(order_[pos]);
            const int y = pos * kRowHeight;
            if (row->y() != y || row->width() != w)
                row->setGeometry(0, y, w, kRowHeight);
            row->show();
        }
        stack_->setUpdatesEnabled(true);
    }

    void rebuild() {
        clearDropCaret();
        DocumentItem* d = state_->activeDocument();
        // Rows are positioned manually (see below), never in the stack
        // layout: a QBoxLayout with tens of thousands of items spends
        // minutes in geometry passes (maximumSize/sizeHint per item, per
        // pass) on a real display. The layout keeps only its stretch.
        if (!d) {
            for (LayerRow* row : rows_) row->hide();
            stack_->setMinimumHeight(kEndSlack);
            order_.clear();
            base_ = -1;
            lastDoc_ = nullptr;
            lastActive_ = -1;
            return;
        }

        const auto rbT0 = std::chrono::steady_clock::now();
        // Row pool covers the visible window, not the whole stack: creating
        // one QWidget per layer costs ~60us each (5s+ on an 84k-layer map),
        // while only a few dozen rows are ever on screen. The visible order
        // (collapsed subtrees excluded, like before) sets the scrollbar
        // range; pooled rows bind to its window and carry absolute y
        // positions, so drag, rename and paint code is untouched.
        const int n = d->layers.size();
        order_.clear();
        order_.reserve(n);
        for (int i = 0; i < n; ++i) {
            if (!d->rowHiddenByCollapsedGroup(i)) order_.append(i);
        }
        int removed = 0, created = 0;
        const int poolWant = qMin(order_.size(), poolNeeded());
        while (rows_.size() > poolWant && rows_.size() > 64) {
            LayerRow* row = rows_.takeLast();
            row->hide();
            row->deleteLater();
            ++removed;
        }
        const int poolHave = rows_.size();
        ensurePool(poolWant);
        created = rows_.size() - poolHave;
        // Slack below the last row keeps the end-of-list drop zone.
        stack_->setMinimumHeight(order_.size() * kRowHeight + kEndSlack);
        base_ = -1;
        layoutWindow(true);
        ::pittore::core::log::log_info(
            "[panel][rows] total=%d created=%d removed=%d pool=%d ms=%.2f",
            n, created, removed, rows_.size(),
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rbT0)
                .count());

        if (LayerItem* l = state_->activeLayer()) {
            QSignalBlocker b1(opacity_), b2(fill_), b3(blendMode_);
            const QSignalBlocker b1s(opacitySlider_);
            opacity_->setValue(l->opacity);
            if (opacitySlider_) opacitySlider_->setValue(l->opacity);
            fill_->setValue(l->fill);
            blendMode_->setCurrentText(l->blendMode);
            if (lockButtons_.size() == 5) {
                const QSignalBlocker b4(lockButtons_[4]);
                lockButtons_[4]->setChecked(l->locked);
            }
        }
        stack_->update();
        // Follow structural selections (new layer, group, reparent, undo) to
        // the active row, but keep the scroll position when the active layer
        // did not change (e.g. collapsing a distant group must not yank the
        // viewport). Document switches always re-reveal.
        if (!d->layers.isEmpty()) {
            const int active =
                qBound(0, d->activeLayer, d->layers.size() - 1);
            if (d != lastDoc_ || active != lastActive_) {
                lastDoc_ = d;
                lastActive_ = active;
                scrollToLayer(active);
            }
        } else {
            lastDoc_ = d;
            lastActive_ = -1;
        }
    }

    void clearDropCaret() {
        drop_.row = -1;
        drop_.mode = LayerDropState::None;
    }

    AppState* state_;
    QScrollArea* scroll_ = nullptr;
    QWidget* stack_ = nullptr;
    QVBoxLayout* stackLayout_ = nullptr;
    QVector<LayerRow*> rows_;    // pooled rows for the visible window
    QVector<int> order_;        // visible layer indices in stack order
    int base_ = -1;             // order_ position bound to rows_[0]
    LayerDropState drop_;        // live drag-over caret shared with the rows
    QComboBox* blendMode_ = nullptr;
    QSpinBox* opacity_ = nullptr;
    QSlider* opacitySlider_ = nullptr;
    QSpinBox* fill_ = nullptr;
    // True while an opacity slider drag holds an open undo step, and the
    // slider position where it started (a press without movement discards).
    bool opacityGesture_ = false;
    int opacityStart_ = 0;
    QVector<QToolButton*> lockButtons_;
    // Last scrolled-to selection: rebuild() only auto-scrolls when the active
    // row actually moved (or the document changed), so expanding/collapsing a
    // distant group never yanks the viewport.
    DocumentItem* lastDoc_ = nullptr;
    int lastActive_ = -1;
};

}  // namespace

QWidget* createLayersPanel(AppState* state, QWidget* parent) {
    return new LayersPanel(state, parent);
}

}  // namespace pittore::ui
