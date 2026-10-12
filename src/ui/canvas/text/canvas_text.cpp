#include "ui/canvas_view.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPainterPath>
#include <QScrollBar>
#include <QDateTime>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui/ai_models.h"
#include "ui/contextual_task_bar.h"
#include "ui/icons.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


double CanvasView::textAlignShift() const {
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return 0.0;
    const TextItem& t = d->layers[textEditIndex_].textSpec;
    if (t.wrapWidth > 0.5) {
        const double lw = d->layers[textEditIndex_].textLayoutWidth;
        if (t.align == 1) return (t.wrapWidth - lw) * 0.5;
        if (t.align == 2) return t.wrapWidth - lw;
    }
    return 0.0;
}


QPointF CanvasView::textLayoutToDoc(const QPointF& p) const {
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return p;
    const TextItem& t = d->layers[textEditIndex_].textSpec;
    return QPointF(t.origin.x() + textAlignShift() + p.x(), t.origin.y() + p.y());
}


QPointF CanvasView::textDocToLayout(const QPointF& p) const {
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return p;
    const TextItem& t = d->layers[textEditIndex_].textSpec;
    return QPointF(p.x() - t.origin.x() - textAlignShift(), p.y() - t.origin.y());
}


void CanvasView::refreshTextLayout() {
    textLayoutValid_ = false;
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return;
    const LayerItem& l = d->layers[textEditIndex_];
    if (!l.liveText) return;
    textLayout_ = textLayoutFor(l.textSpec);
    textLayoutValid_ = true;
}


std::size_t CanvasView::textByteLength() const {
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return 0;
    return static_cast<std::size_t>(
        d->layers[textEditIndex_].textSpec.text.toUtf8().size());
}


std::size_t CanvasView::textOffsetAtDoc(const QPointF& docPoint) const {
    if (!textLayoutValid_) return textCaret_;
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size())
        return textCaret_;
    const TextItem& t = d->layers[textEditIndex_].textSpec;
    const QPointF p = textDocToLayout(docPoint);
    return pittore::text::hitTest(textSpecFor(t), textLayout_,
                                   static_cast<float>(p.x()),
                                   static_cast<float>(p.y()));
}


void CanvasView::selectWordAt(std::size_t byte) {
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return;
    const std::string s = d->layers[textEditIndex_].textSpec.text.toStdString();
    auto isWord = [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '\'';
    };
    // Bytes of a multibyte codepoint count as word characters, so a CJK
    // character selects whole (boundaries are snapped below).
    auto isWordByte = [&](std::size_t i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        return c >= 0x80 || isWord(c);
    };
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t lo = std::min(byte, s.size());
    std::size_t hi = lo;
    while (lo > 0 && isWordByte(lo - 1)) --lo;
    while (hi < s.size() && isWordByte(hi)) ++hi;
    if (lo == hi) {
        // No word under the cursor: select the whitespace run instead.
        while (lo > 0 && isSpace(static_cast<unsigned char>(s[lo - 1]))) --lo;
        while (hi < s.size() && isSpace(static_cast<unsigned char>(s[hi]))) ++hi;
    }
    // Snap to character boundaries so a multibyte sequence is never split.
    while (lo > 0 && (static_cast<unsigned char>(s[lo]) & 0xC0) == 0x80) --lo;
    while (hi < s.size() && (static_cast<unsigned char>(s[hi]) & 0xC0) == 0x80) ++hi;
    textAnchor_ = lo;
    textCaret_ = hi;
    textCaretOn_ = true;
    if (textCaretTimer_) textCaretTimer_->start();
    viewport()->update();
}


bool CanvasView::textPointInRun(const QPointF& docPoint) const {
    if (!textLayoutValid_ || textLayout_.lines.empty()) return false;
    const QPointF p = textDocToLayout(docPoint);
    for (const pittore::text::LineSpan& s : textLayout_.lines) {
        const double pad = std::max(2.0, static_cast<double>(s.height) * 0.2);
        if (p.y() < s.top - pad || p.y() > s.top + s.height + pad) continue;
        const double right = s.x + std::max(s.width, 1.0f) + pad;
        if (p.x() >= s.x - pad && p.x() <= right) return true;
    }
    return false;
}


double CanvasView::textLineX(const pittore::text::LineSpan& span,
                             std::size_t byte) const {
    if (byte >= span.end) return span.x + span.width;
    for (const pittore::text::TextLayout::CharPos& c : textLayout_.chars)
        if (c.byte == byte) return c.x;
    return span.x + span.width;
}


void CanvasView::setTextCaret(std::size_t byte, bool extend) {
    textCaret_ = std::min(byte, textByteLength());
    if (!extend) textAnchor_ = textCaret_;
    textCaretOn_ = true;
    if (textCaretTimer_) textCaretTimer_->start();
    viewport()->update();
}


void CanvasView::replaceTextRange(std::size_t from, std::size_t to,
                                  const QString& insert) {
    DocumentItem* d = doc();
    const int index = textEditIndex_;
    if (!d || index < 0 || index >= d->layers.size() || !d->layers[index].liveText)
        return;
    QByteArray bytes = d->layers[index].textSpec.text.toUtf8();
    const QByteArray ins = insert.toUtf8();
    int f = qBound(0, static_cast<int>(std::min(from, to)), bytes.size());
    int t = qBound(0, static_cast<int>(std::max(from, to)), bytes.size());
    bytes.replace(f, t - f, ins);
    d->layers[index].textSpec.text = QString::fromUtf8(bytes);
    textCaret_ = static_cast<std::size_t>(f) + static_cast<std::size_t>(ins.size());
    textAnchor_ = textCaret_;
    textEditChanged_ = true;
    textCaretOn_ = true;
    if (textCaretTimer_) textCaretTimer_->start();
    state_->refreshTextLayer(index);
    refreshTextLayout();
    viewport()->update();
}


bool CanvasView::textCaretSegment(QPointF& top, QPointF& bottom) const {
    if (!textLayoutValid_) return false;
    DocumentItem* d = doc();
    if (!d || textEditIndex_ < 0 || textEditIndex_ >= d->layers.size()) return false;
    const TextItem& t = d->layers[textEditIndex_].textSpec;
    const pittore::text::Caret c =
        pittore::text::caretAt(textSpecFor(t), textLayout_, textCaret_);
    top = textLayoutToDoc(QPointF(c.x, c.top));
    bottom = textLayoutToDoc(QPointF(c.x, c.top + c.height));
    return true;
}


void CanvasView::startTextEdit(int index, bool undoBegan) {
    commitTextEdit();   // one session at a time; no-op when none is live
    DocumentItem* d = doc();
    if (!d || index < 0 || index >= d->layers.size() || !d->layers[index].liveText)
        return;
    if (!undoBegan) state_->beginUndoStep();
    textEditing_ = true;
    textEditIndex_ = index;
    textEditChanged_ = false;
    textCaretOn_ = true;
    // Mirror the layer into the Type tool's options so the options bar and the
    // Character panel show the run being edited. Silent: the values already
    // match the layer, so this must not write back to it.
    const TextItem& spec = d->layers[index].textSpec;
    const ToolId typeTool = state_->activeTool() == ToolId::VerticalType ? ToolId::VerticalType
                                                                        : ToolId::HorizontalType;
    const int famIndex = typeIndexForFamily(spec.family);
    if (famIndex >= 0) state_->setOptionSilently(typeTool, QStringLiteral("family"), famIndex);
    state_->setOptionSilently(typeTool, QStringLiteral("style"),
                              spec.bold && spec.italic ? 6 : spec.bold ? 4
                              : spec.italic             ? 1
                                                        : 0);
    state_->setOptionSilently(typeTool, QStringLiteral("size"), spec.size);
    state_->setOptionSilently(typeTool, QStringLiteral("align"), spec.align);
    state_->setOptionSilently(typeTool, QStringLiteral("color"), spec.color);
    refreshTextLayout();
    // A fresh session starts collapsed at the end of the run, so typing appends
    // (the .af reader keeps the caret where you clicked; that path sets it directly).
    textCaret_ = textAnchor_ = textByteLength();
    if (textCaretTimer_) textCaretTimer_->start();
    state_->setActiveLayerIndex(index);
    viewport()->setFocus(Qt::MouseFocusReason);
    viewport()->update();
}


void CanvasView::commitTextEdit() {
    if (!textEditing_) return;
    textEditing_ = false;
    textSelecting_ = false;
    textLayoutValid_ = false;
    textCaret_ = textAnchor_ = 0;
    if (textCaretTimer_) textCaretTimer_->stop();
    const int index = textEditIndex_;
    const bool changed = textEditChanged_;
    textEditIndex_ = -1;
    textEditChanged_ = false;

    DocumentItem* d = doc();
    const bool live = d && index >= 0 && index < d->layers.size() &&
                      d->layers[index].liveText;
    const bool empty = live && d->layers[index].textSpec.text.isEmpty();
    // Type Mask commit: rasterize the typed run into a selection mask;
    // no text layer survives. The open undo step (begun with the layer)
    // is committed over the selection change.
    if (textMaskMode_ != 0 && live && !empty) {
        const TextItem spec = d->layers[index].textSpec;
        const auto raster = pittore::text::rasterize(textSpecFor(spec));
        state_->removeLayerSilently(index);
        textMaskMode_ = 0;
        if (raster && !raster->isEmpty()) {
            QImage mask(QSize(d->size.width(), d->size.height()),
                        QImage::Format_Grayscale8);
            mask.fill(0);
            const QPointF topLeft = textLayoutToDoc(
                QPointF(float(raster->bounds.left), float(raster->bounds.top)));
            const int bw = raster->bounds.width();
            for (int y = 0; y < raster->bounds.height(); ++y) {
                const int dy = qRound(topLeft.y()) + y;
                if (dy < 0 || dy >= mask.height()) continue;
                uchar* row = mask.scanLine(dy);
                for (int x = 0; x < bw; ++x) {
                    const int dx = qRound(topLeft.x()) + x;
                    if (dx < 0 || dx >= mask.width()) continue;
                    row[dx] = raster->coverage[std::size_t(y) * bw + x];
                }
            }
            state_->setSelectionMask(mask);
            state_->commitUndoStep(tr("Type Mask"), QStringLiteral("type"));
        } else {
            state_->discardUndoStep();
            state_->setStatusHint(tr("Type Mask: no text to select."));
        }
        viewport()->update();
        return;
    }
    textMaskMode_ = 0;
    // An empty text layer never survives an editing session: remove it and drop
    // the undo step that would otherwise resurrect a blank layer.
    if (empty) state_->removeLayerSilently(index);
    if (live && !empty && changed)
        state_->commitUndoStep(tr("Text"), QStringLiteral("type"));
    else
        state_->discardUndoStep();
    viewport()->update();
}


bool CanvasView::textKeyConsumes(const QKeyEvent* event) const {
    if (!textEditing_) return false;
    switch (event->key()) {
        case Qt::Key_Escape:
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return true;
        case Qt::Key_A:
            // Ctrl+A selects the whole run; a plain "a" is text.
            return event->modifiers().testFlag(Qt::ControlModifier);
        default:
            break;
    }
    // Leave every other Ctrl/Alt/Meta chord (undo, save, …) to the window; plain
    // printable input is text.
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    const QString text = event->text();
    return !text.isEmpty() && text.at(0).isPrint();
}


bool CanvasView::handleTextKey(QKeyEvent* event) {
    if (!textEditing_) return false;
    DocumentItem* d = doc();
    const int index = textEditIndex_;
    if (!d || index < 0 || index >= d->layers.size() || !d->layers[index].liveText) {
        commitTextEdit();
        return false;
    }
    const std::string bytes = d->layers[index].textSpec.text.toStdString();
    const std::size_t len = bytes.size();
    const std::size_t selLo = std::min(textCaret_, textAnchor_);
    const std::size_t selHi = std::max(textCaret_, textAnchor_);
    const bool hasSel = selHi > selLo;
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);

    // Caret on the line holding `textCaret_`, used by Home/End and Up/Down.
    auto caretLine = [this]() -> const pittore::text::LineSpan* {
        if (!textLayoutValid_ || textLayout_.lines.empty()) return nullptr;
        const pittore::text::LineSpan* best = &textLayout_.lines.front();
        for (const pittore::text::LineSpan& s : textLayout_.lines) {
            if (s.start <= textCaret_) best = &s;
        }
        return best;
    };

    switch (event->key()) {
        case Qt::Key_Escape:
            commitTextEdit();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Type Mask tools commit on Enter (a newline is meaningless
            // for a selection); Type tools insert a line break.
            if (textMaskMode_ != 0) {
                commitTextEdit();
                return true;
            }
            replaceTextRange(selLo, selHi, QStringLiteral("\n"));
            return true;
        case Qt::Key_Backspace: {
            if (hasSel) {
                replaceTextRange(selLo, selHi, QString());
                return true;
            }
            if (textCaret_ == 0) return true;
            replaceTextRange(prevCharBoundary(bytes, textCaret_), textCaret_, QString());
            return true;
        }
        case Qt::Key_Delete: {
            if (hasSel) {
                replaceTextRange(selLo, selHi, QString());
                return true;
            }
            if (textCaret_ >= len) return true;
            replaceTextRange(textCaret_, nextCharBoundary(bytes, textCaret_), QString());
            return true;
        }
        case Qt::Key_Left: {
            if (ctrl) return true;   // word-wise motion is not modelled yet
            const std::size_t to =
                hasSel && !shift ? selLo : prevCharBoundary(bytes, textCaret_);
            setTextCaret(to, shift);
            return true;
        }
        case Qt::Key_Right: {
            if (ctrl) return true;
            const std::size_t to =
                hasSel && !shift ? selHi : nextCharBoundary(bytes, textCaret_);
            setTextCaret(to, shift);
            return true;
        }
        case Qt::Key_Up:
        case Qt::Key_Down: {
            const pittore::text::LineSpan* line = caretLine();
            if (!line) return true;
            int cur = 0;
            for (int i = 0; i < static_cast<int>(textLayout_.lines.size()); ++i)
                if (&textLayout_.lines[i] == line) cur = i;
            const int want = cur + (event->key() == Qt::Key_Down ? 1 : -1);
            if (want < 0 || want >= static_cast<int>(textLayout_.lines.size()))
                return true;
            const pittore::text::LineSpan& to = textLayout_.lines[want];
            const double x = textLineX(*line, textCaret_);
            const TextItem& t = d->layers[index].textSpec;
            setTextCaret(pittore::text::hitTest(
                             textSpecFor(t), textLayout_, static_cast<float>(x),
                             static_cast<float>(to.top + to.height * 0.5)),
                         shift);
            return true;
        }
        case Qt::Key_Home: {
            const pittore::text::LineSpan* line = caretLine();
            setTextCaret(line ? line->start : 0, shift);
            return true;
        }
        case Qt::Key_End: {
            const pittore::text::LineSpan* line = caretLine();
            setTextCaret(line ? line->end : len, shift);
            return true;
        }
        case Qt::Key_A:
            if (ctrl) {
                textAnchor_ = 0;
                textCaret_ = len;
                textCaretOn_ = true;
                if (textCaretTimer_) textCaretTimer_->start();
                viewport()->update();
                return true;
            }
            break;
        // Tab is swallowed while typing so it cannot toggle the app chrome.
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return true;
        default:
            break;
    }

    // Plain printable input replaces the selection (or inserts at the caret);
    // chords and the delete key are left to the window's own handling.
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    const QString text = event->text();
    if (text.isEmpty() || !text.at(0).isPrint()) return false;
    replaceTextRange(selLo, selHi, text);
    return true;
}


void CanvasView::keyPressEvent(QKeyEvent* event) {
    if (handleTextKey(event)) {
        event->accept();
        return;
    }
    // A ruler guide in flight abandons on Escape like any drag preview.
    if (rulerGuideDragging_ && event->key() == Qt::Key_Escape) {
        cancelRulerGuide();
        event->accept();
        return;
    }
    // Polygonal lasso: Escape abandons the loop, Enter commits it.
    if (state_->activeTool() == ToolId::PolygonalLasso &&
        !polyPts_.empty()) {
        if (event->key() == Qt::Key_Escape) {
            polyPts_.clear();
            polyHoverOn_ = false;
            state_->setStatusHint(tr("Polygonal Lasso cancelled."));
            viewport()->update();
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return ||
             event->key() == Qt::Key_Enter) &&
            polyPts_.size() >= 3) {
            lassoCommitPolygon(polyPts_, event->modifiers());
            polyPts_.clear();
            polyHoverOn_ = false;
            event->accept();
            viewport()->update();
            refresh();
            return;
        }
    }
    // Freehand lasso / selection brush: Escape abandons the live stroke.
    if ((lassoLive_ || selBrushLive_) && event->key() == Qt::Key_Escape) {
        lassoLive_ = false;
        lassoStroke_.clear();
        selBrushLive_ = false;
        selBrushMask_ = QImage();
        dragging_ = false;
        viewport()->update();
        event->accept();
        return;
    }
    // Pen-family gestures: Escape abandons the working path, Enter/Return
    // finishes it as a layer. Freehand commits on release instead.
    if (penActive_ && isPenTool(state_->activeTool())) {
        if (event->key() == Qt::Key_Escape) {
            cancelPenPath();
            state_->setStatusHint(tr("Pen path cancelled."));
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            penTool_ != ToolId::FreeformPen) {
            finishPenPath();
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}

}  // namespace pittore::ui
