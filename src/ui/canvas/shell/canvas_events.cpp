#include "ui/canvas_view.h"

#include <QApplication>
#include <QColorSpace>
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
#include <QTabletEvent>
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
#include "ui/embedded_icc.h"
#include "ui/icons.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


bool CanvasView::event(QEvent* event) {
    // The canvas itself is the focus widget while the Type tool edits (the
    // scroll area takes focus, with the viewport as its proxy), so a single-key
    // window shortcut such as X/F/D/Tab arrives here as ShortcutOverride before
    // it becomes a shortcut. Claim the keys the caret would consume, turning
    // them into ordinary key presses instead.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* e = static_cast<QKeyEvent*>(event);
        if (textKeyConsumes(e)) {
            e->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}


// Right-click with the Zoom tool opens the R14 navigation menu: zoom steps,
// fit modes, view rotation and the preset percentages — the conventional
// arrangement (in/out, fit family, rotate/reset, presets). Other tools keep
// the default (no) menu so their right-click stays free for later gestures.
void CanvasView::contextMenuEvent(QContextMenuEvent* event) {
    // The right-release ending an Alt+Right brush resize must not pop the
    // canvas menu.
    if (brushResizeSuppressMenu_) {
        brushResizeSuppressMenu_ = false;
        event->accept();
        return;
    }
    const ToolId tool = state_->activeTool();
    if (tool == ToolId::Zoom) {
        if (!doc()) return;
        QMenu menu(this);
        QAction* zoomInAction = menu.addAction(tr("Zoom In"));
        QAction* zoomOutAction = menu.addAction(tr("Zoom Out"));
        menu.addSeparator();
        QAction* fitAction = menu.addAction(tr("Fit"));
        QAction* fitWidthAction = menu.addAction(tr("Fit to Width"));
        menu.addSeparator();
        QAction* rotateLeftAction = menu.addAction(tr("Rotate Left"));
        QAction* rotateRightAction = menu.addAction(tr("Rotate Right"));
        QAction* resetAction = menu.addAction(tr("Reset"));
        menu.addSeparator();
        QAction* pct50 = menu.addAction(tr("50%"));
        QAction* pct100 = menu.addAction(tr("100%"));
        QAction* pct150 = menu.addAction(tr("150%"));
        QAction* pct200 = menu.addAction(tr("200%"));
        QAction* chosen = menu.exec(event->globalPos());
        if (!chosen) return;
        if (chosen == zoomInAction)
            zoomIn();
        else if (chosen == zoomOutAction)
            zoomOut();
        else if (chosen == fitAction)
            zoomToFit();
        else if (chosen == fitWidthAction)
            zoomToWidth();
        else if (chosen == rotateLeftAction)
            setRotation(doc()->rotation - 90.0);
        else if (chosen == rotateRightAction)
            setRotation(doc()->rotation + 90.0);
        else if (chosen == resetAction)
            resetRotation();
        else if (chosen == pct50)
            setZoom(0.5);
        else if (chosen == pct100)
            setZoom(1.0);
        else if (chosen == pct150)
            setZoom(1.5);
        else if (chosen == pct200)
            setZoom(2.0);
        return;
    }
    QMenu menu(this);
    QAction* selAll = menu.addAction(tr("Select All"));
    selAll->setShortcut(QKeySequence(QStringLiteral("Ctrl+A")));
    QAction* desel = menu.addAction(tr("Deselect"));
    desel->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    QAction* inverse = menu.addAction(tr("Inverse"));
    inverse->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+I")));
    menu.addSeparator();
    QAction* copyA = menu.addAction(tr("Copy"));
    copyA->setShortcut(QKeySequence(QStringLiteral("Ctrl+C")));
    QAction* pasteA = menu.addAction(tr("Paste"));
    pasteA->setShortcut(QKeySequence(QStringLiteral("Ctrl+V")));
    QAction* pastePlace = menu.addAction(tr("Paste in Place"));
    pastePlace->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    QAction* clearA = menu.addAction(tr("Clear"));
    menu.addSeparator();
    QAction* fitA = menu.addAction(tr("Fit on Screen"));
    fitA->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    QAction* actualA = menu.addAction(tr("Actual Pixels"));
    actualA->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    menu.addSeparator();
    QAction* swapA = menu.addAction(tr("Swap Colors"));
    swapA->setShortcut(QKeySequence(QStringLiteral("X")));
    QAction* resetA = menu.addAction(tr("Reset Colors"));
    resetA->setShortcut(QKeySequence(QStringLiteral("D")));
    menu.addSeparator();
    QAction* clearGuidesA = menu.addAction(tr("Clear Guides"));
    QAction* chosen = menu.exec(event->globalPos());
    if (!chosen) return;
    DocumentItem* d = doc();
    if (chosen == selAll) {
        if (d) state_->setSelection(QRectF(QPointF(0, 0), QSizeF(d->size)), false);
    } else if (chosen == desel) {
        state_->clearSelection();
    } else if (chosen == inverse) {
        state_->invertSelection();
    } else if (chosen == copyA) {
        emit copyRequested();
    } else if (chosen == pasteA) {
        emit pasteRequested();
    } else if (chosen == pastePlace) {
        emit pasteInPlaceRequested();
    } else if (chosen == clearA) {
        emit clearRequested();
    } else if (chosen == fitA) {
        zoomToFit();
    } else if (chosen == actualA) {
        zoomActualPixels();
    } else if (chosen == swapA) {
        state_->swapColors();
    } else if (chosen == resetA) {
        state_->resetColors();
    } else if (chosen == clearGuidesA) {
        if (d) {
            d->horizontalGuides.clear();
            d->verticalGuides.clear();
            emit state_->documentModified(d);
        }
    }
}


bool CanvasView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport()) {
        switch (event->type()) {
            case QEvent::TabletPress:
            case QEvent::TabletMove:
            case QEvent::TabletRelease:
            case QEvent::TabletLeaveProximity: {
                // Stylus input for pressure tools; anything unconsumed keeps
                // flowing so Qt synthesizes the usual mouse gesture.
                auto* e = static_cast<QTabletEvent*>(event);
                if (handleTabletEvent(e)) return true;
                break;
            }
            case QEvent::Enter:
                cursorInViewport_ = true;
                // A brush tool syncs the canvas cursor the moment the pointer
                // arrives, before the first move can enforce it.
                if (cursorToolActive())
                    viewport()->setCursor(canvasCursor());
                syncCursorOverride();
                break;
            case QEvent::Leave:
                // The brush ring belongs to the canvas the pointer just left:
                // erase it so the next overlay paint draws nothing, and pop the
                // app-wide blank cursor so panels regain their own cursors.
                cursorInViewport_ = false;
                syncCursorOverride();
                if (!brushCursorRect_.isEmpty()) {
                    viewport()->update(
                        brushCursorRect_.adjusted(-2, -2, 2, 2).toAlignedRect());
                    brushCursorRect_ = QRectF();
                }
                break;
            case QEvent::DragEnter:
            case QEvent::DragMove: {
                // QDragEnterEvent derives QDragMoveEvent, so one cast covers
                // both.
                auto* e = static_cast<QDragMoveEvent*>(event);
                if (dropMimeHasImage(e->mimeData())) {
                    e->acceptProposedAction();
                    dropActive_ = true;
                    viewport()->update();
                } else {
                    e->ignore();
                }
                return true;
            }
            case QEvent::DragLeave:
                dropActive_ = false;
                viewport()->update();
                return true;
            case QEvent::Drop: {
                auto* e = static_cast<QDropEvent*>(event);
                dropActive_ = false;
                viewport()->update();
                if (!dropMimeHasImage(e->mimeData())) {
                    e->ignore();
                    return true;
                }
                e->acceptProposedAction();
                const QPointF docPos = viewToDocument(e->position());
                if (e->mimeData()->hasImage()) {
                    const QImage img =
                        qvariant_cast<QImage>(e->mimeData()->imageData());
                    if (!img.isNull())
                        acceptDropImage(img, tr("Dropped Image"), docPos);
                }
                for (const QUrl& url : e->mimeData()->urls()) {
                    if (!url.isLocalFile()) continue;
                    const QString localPath = url.toLocalFile();
                    const QByteArray suffix =
                        QFileInfo(localPath).suffix().toLower().toLatin1();
                    if (isCodecImportSuffix(suffix)) {
                        // A dropped SVG joins the CURRENT document at the drop
                        // point as editable shape/group layers, like a placed
                        // photo — not a new project. PSD/PSB and .af keep
                        // opening the same way File > Open does (their own
                        // document with layers/groups intact) rather than
                        // flattening to one placed bitmap.
                        if (suffix == "svg") {
                            QFile f(localPath);
                            if (f.open(QIODevice::ReadOnly)) {
                                SvgImportResult svg;
                                int dpi = 96;
                                QString svgError;
                                if (svgPartsImport(f.readAll(), &svg, &dpi,
                                                   &svgError)) {
                                    QString placeError;
                                    if (!state_->placeSvgParts(
                                            localPath, svg, docPos,
                                            &placeError)) {
                                        state_->setStatusHint(
                                            placeError.isEmpty()
                                                ? tr("Could not place %1.")
                                                      .arg(QFileInfo(localPath)
                                                               .fileName())
                                                : placeError);
                                    } else {
                                        state_->setActiveTool(
                                            ToolId::Move);  // invite a resize
                                    }
                                } else {
                                    state_->setStatusHint(svgError);
                                }
                            } else {
                                state_->setStatusHint(
                                    tr("Could not read %1.")
                                        .arg(QFileInfo(localPath).fileName()));
                            }
                            continue;
                        }
                        // Open these the same way File > Open does: PSD/PSB
                        // as their own document with layers/groups intact, and
                        // .af via the built-in container codec, rather
                        // than flattening to one placed bitmap.
                        QString openError;
                        if (!state_->openImageFile(localPath, &openError)) {
                            state_->setStatusHint(
                                openError.isEmpty()
                                    ? tr("Could not read %1.")
                                          .arg(QFileInfo(localPath).fileName())
                                    : openError);
                        }
                        continue;
                    }
                    QImageReader reader(localPath);
                    reader.setAutoTransform(true);
                    const QImage img = reader.read();
                    if (img.isNull()) {
                        state_->setStatusHint(
                            tr("Could not read %1 as an image.")
                                .arg(QFileInfo(localPath).fileName()));
                        continue;
                    }
                    // Pasted pixels join the document's working space: run
                    // the same mismatch policy as File > Open (the standard
                    // "Ask When Pasting"). Cancel skips this file; anything
                    // else places as decoded — placed layers carry no
                    // profile tag of their own. embeddedProfileName falls
                    // back to the container scan when the decode reports no
                    // color space, so a profiled file still asks.
                    if (!state_->resolveImportedProfile(
                            embeddedProfileName(localPath, img)))
                        continue;
                    acceptDropImage(img, QFileInfo(localPath).baseName(), docPos);
                }
                return true;
            }
            case QEvent::ShortcutOverride: {
                // Claim single-key window shortcuts (F/X/D/Tab and the tool
                // letters) so they arrive as text while the caret is live.
                auto* e = static_cast<QKeyEvent*>(event);
                if (textKeyConsumes(e)) {
                    e->accept();
                    return true;
                }
                break;
            }
            case QEvent::KeyPress: {
                // The viewport holds focus while the Type tool is editing, so
                // typing lands here rather than on the scroll area.
                auto* e = static_cast<QKeyEvent*>(event);
                if (handleTextKey(e)) return true;
                break;   // everything else keeps its normal handling
            }
            default:
                break;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}


void CanvasView::acceptDropImage(const QImage& img, const QString& name,
                                 const QPointF& docPos) {
    DocumentItem* d = doc();
    if (!d) {
        // Nothing open: the drop becomes a new document at native size with
        // the image placed 1:1. The document switch arms the deferred fit.
        d = state_->addDocument(name.isEmpty() ? tr("Dropped Image") : name,
                                img.size(), 300);
        if (!d) return;
        state_->placeImageLayer(img, d->title,
                                QPointF(img.width() / 2.0, img.height() / 2.0),
                                1.0);
    } else {
        // Fit oversized images down, never upscale: a smaller photo lands at
        // 1:1 centred on the drop point.
        const double fit =
            qMin(1.0, qMin(double(d->size.width()) / qMax(1, img.width()),
                            double(d->size.height()) / qMax(1, img.height())));
        state_->placeImageLayer(img, name, docPos, fit);
    }
    state_->setActiveTool(ToolId::Move);  // transform controls invite a resize
}

}  // namespace pittore::ui
