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


void CanvasView::layoutRulers() {
    if (!horizontalRuler_) return;
    const int offset = rulers_ ? kRulerSize : 0;
    rulerCorner_->setGeometry(0, 0, offset, offset);
    horizontalRuler_->setGeometry(offset, 0, width() - offset, offset);
    verticalRuler_->setGeometry(0, offset, offset, height() - offset);
}


void CanvasView::beginRulerGuide(Qt::Orientation orientation,
                                 const QPointF& rulerPos) {
    if (!doc()) return;
    rulerGuideDragging_ = true;
    rulerGuideHorizontal_ = (orientation == Qt::Horizontal);
    updateRulerGuide(orientation, rulerPos);
}


void CanvasView::updateRulerGuide(Qt::Orientation orientation,
                                const QPointF& rulerPos) {
    DocumentItem* d = doc();
    if (!d || !rulerGuideDragging_) return;
    // The ruler strip is off-document on its own axis; sample through the
    // viewport centre on that axis so the value stays exact under rotation.
    QPointF vp = QPointF(rulerPos);
    if (orientation == Qt::Horizontal)
        vp.setY(viewport()->height() / 2.0);
    else
        vp.setX(viewport()->width() / 2.0);
    rulerGuideDoc_ = viewToDocument(vp);
    viewport()->update();
}


void CanvasView::finishRulerGuide(bool commit) {
    DocumentItem* d = doc();
    const bool wasDragging = rulerGuideDragging_;
    rulerGuideDragging_ = false;
    if (!commit || !wasDragging || !d) {
        viewport()->update();
        return;
    }
    // Same storage as the New Guide dialog (and no undo step, like it).
    if (rulerGuideHorizontal_)
        d->horizontalGuides.append(rulerGuideDoc_.y());
    else
        d->verticalGuides.append(rulerGuideDoc_.x());
    emit state_->documentModified(d);
    viewport()->update();
}


void CanvasView::cancelRulerGuide() { finishRulerGuide(false); }


void CanvasView::paintRuler(QPainter& p, Qt::Orientation orientation,
                           const QRect& area) const {
    const ThemeColors c = colorsFor(state_->theme());
    p.fillRect(area, c.chrome);
    p.setPen(QPen(c.border, 1));
    if (orientation == Qt::Horizontal)
        p.drawLine(area.bottomLeft(), area.bottomRight());
    else
        p.drawLine(area.topRight(), area.bottomRight());

    DocumentItem* d = doc();
    if (!d) return;

    // Tick spacing: the first step on the ladder that leaves at least 48 device
    // pixels between labels at the current zoom (zoom is logical, so fold
    // the ruler's DPR in or labels go DPR× sparser on 150/200%).
    static const int ladder[] = {1, 2, 5, 10, 25, 50, 100, 250, 500,
                                 1000, 2500, 5000, 10000, 25000};
    const qreal rulerDpr =
        p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0;
    const double effZoom = d->zoom * (rulerDpr > 0.0 ? rulerDpr : 1.0);
    int step = ladder[0];
    for (int candidate : ladder) {
        step = candidate;
        if (candidate * effZoom >= 48) break;
    }

    QFont f = p.font();
    f.setPixelSize(9);
    p.setFont(f);
    p.setPen(c.textDim);

    const QTransform t = documentTransform();
    const int extent = orientation == Qt::Horizontal ? d->size.width() : d->size.height();
    const int limit = orientation == Qt::Horizontal ? area.width() : area.height();

    for (int value = 0; value <= extent; value += step) {
        const QPointF mapped =
            t.map(orientation == Qt::Horizontal ? QPointF(value, 0) : QPointF(0, value));
        const double pos = orientation == Qt::Horizontal ? mapped.x() : mapped.y();
        if (pos < -40 || pos > limit + 40) continue;

        if (orientation == Qt::Horizontal) {
            p.drawLine(QPointF(pos, area.height() - 7), QPointF(pos, area.height() - 1));
            p.drawText(QPointF(pos + 2, area.height() - 9), QString::number(value));
        } else {
            p.drawLine(QPointF(area.width() - 7, pos), QPointF(area.width() - 1, pos));
            p.save();
            p.translate(area.width() - 9, pos - 2);
            p.rotate(-90);
            p.drawText(QPointF(0, 0), QString::number(value));
            p.restore();
        }

        for (int i = 1; i < 5; ++i) {
            const double sub = value + step * i / 5.0;
            const QPointF minor =
                t.map(orientation == Qt::Horizontal ? QPointF(sub, 0) : QPointF(0, sub));
            const double mp = orientation == Qt::Horizontal ? minor.x() : minor.y();
            if (orientation == Qt::Horizontal)
                p.drawLine(QPointF(mp, area.height() - 4), QPointF(mp, area.height() - 1));
            else
                p.drawLine(QPointF(area.width() - 4, mp), QPointF(area.width() - 1, mp));
        }
    }

    // Cursor position marker.
    const QPointF cursorView = t.map(cursorDoc_);
    p.setPen(QPen(c.accent, 1));
    if (orientation == Qt::Horizontal)
        p.drawLine(QPointF(cursorView.x(), 0), QPointF(cursorView.x(), area.height()));
    else
        p.drawLine(QPointF(0, cursorView.y()), QPointF(area.width(), cursorView.y()));
}

}  // namespace pittore::ui
