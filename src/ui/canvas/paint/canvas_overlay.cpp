#include "ui/canvas_view.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
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
#include "ui/persona/vector_edit.h"
#include "ui/persona/vector_node.h"
#include "ui/persona/vector_point_ops.h"
#include "ui/persona/vector_profile.h"
#include "ui/persona/vector_raster.h"
#include "ui/persona/vector_shapes.h"
#include "ui/selection_mask.h"
#include "ui/svg_parts.h"
#include "engine/ai/bg_remove.h"
#include "engine/compute/paint.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include "ui/canvas/shared/canvas_helpers.h"

namespace pittore::ui {


// Brush cursor + resize HUD: the tool's own pointer UI. Painted on
// every frame whether or not Extras are visible (hiding the cursor
// would blind the brush), while selection edges and the other
// helpers follow the Extras toggle. Overlay only — never document
// pixels.
void CanvasView::paintBrushCursor(QPainter& p) {
    const QTransform t = documentTransform();
    // Brush cursor: an outline ring the size of the brush follows the pointer
    // for every tool that strokes with a dab kernel — Brush/Pencil/Eraser/
    // Dodge/Burn/Sponge, Liquify, the other brush-preset tools, and the
    // vector brush. Shape (circle / nib preview / tilt tick) and behaviour
    // come from the Brush Cursor preferences; overlay only, never
    // composited into the document. Geometry matches brushCursorViewRect
    // exactly (shared cursorRingGeometry), so partial repaints always cover
    // the painted ring and no fragments pool behind it.
    if (brushCursorVisible() && !brushResizeActive_) {
        // One geometry function feeds both this paint and the dirty rect,
        // so the ring can never draw outside its invalidated area.
        double r, ratio, angleDeg;
        bool square;
        cursorRingGeometry(r, ratio, angleDeg, square);
        const int outline = state_->outlineShape();
        if (outline == 1) {  // circle: diameter only, nib shaping off
            ratio = 1.0;
            square = false;
        }
        const QPointF cView = t.map(cursorDoc_);
        const QPointF eView = t.map(cursorDoc_ + QPointF(r, 0.0));
        const double rv = std::hypot(eView.x() - cView.x(), eView.y() - cView.y());
        const double rvMinor = rv * ratio;
        if (rv > 0.5 || rvMinor > 0.5) {
            // A solid, thick ribbon (not a dotted segment ring): a dark 4 px
            // base with a white core, anti-aliased, so the ring reads on any
            // background and never flickers into dashes. The ribbon is the
            // cursor: the OS cursor is blanked app-wide while it draws.
            // Rotated to the tip angle so the ring previews the actual dab.
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(Qt::NoBrush);
            p.translate(cView);
            p.rotate(-angleDeg);
            if (square) {
                const QRectF rr(-rv, -rvMinor, 2.0 * rv, 2.0 * rvMinor);
                p.setPen(QPen(QColor(0, 0, 0, 230), 4));
                p.drawRect(rr);
                p.setPen(QPen(Qt::white, 1.5));
                p.drawRect(rr);
            } else {
                p.setPen(QPen(QColor(0, 0, 0, 230), 4));
                p.drawEllipse(QPointF(0, 0), rv, rvMinor);
                p.setPen(QPen(Qt::white, 1.5));
                p.drawEllipse(QPointF(0, 0), rv, rvMinor);
            }
            if (outline == 3 && std::hypot(tiltX_, tiltY_) > 2.0) {
                // Tilt tick: short line from the centre along the stylus
                // lean, so the direction reads without a second ring.
                const double len = std::hypot(tiltX_, tiltY_);
                const QPointF dir(tiltX_ / len * rv, tiltY_ / len * rv);
                p.setPen(QPen(QColor(0, 0, 0, 230), 3.5));
                p.drawLine(QPointF(0, 0), dir);
                p.setPen(QPen(Qt::white, 2));
                p.drawLine(QPointF(0, 0), dir);
            }
            if (state_->cursorShape() != 0) {
                // An OS cursor is showing: pin a 3px centre dot so the
                // exact dab hotspot stays readable inside the ring.
                p.resetTransform();
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::white);
                p.drawEllipse(cView, 1.5, 1.5);
                p.setBrush(QColor(0, 0, 0, 230));
                p.drawEllipse(cView, 0.75, 0.75);
            }
            p.restore();
        }
    }

    // Clone-source cross (issue #8): a tiny view-sized cross at the point
    // the next dab samples from — the Alt-pinned anchor before the first
    // stroke, then tracking the cursor by the stroke offset. Same dark-base
    // + white-core ribbon as the ring, so it reads on any background.
    // Overlay only, never document pixels.
    if (cloneSourceVisible()) {
        const QPointF sView = t.map(cloneSourceDoc());
        static constexpr double kArm = 7.0;
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 230), 4));
        p.drawLine(sView + QPointF(-kArm, 0), sView + QPointF(kArm, 0));
        p.drawLine(sView + QPointF(0, -kArm), sView + QPointF(0, kArm));
        p.setPen(QPen(Qt::white, 1.5));
        p.drawLine(sView + QPointF(-kArm, 0), sView + QPointF(kArm, 0));
        p.drawLine(sView + QPointF(0, -kArm), sView + QPointF(0, kArm));
        p.restore();
    }

    // Alt+Right resize HUD (conventional behaviour): accent size + hardness rings
    // around the pointer plus a px/% chip, all reading the same live options
    // the drag writes — no need to glance at the status bar.
    if (brushResizeActive_ && cursorInViewport_ && doc()) {
        const ToolId tool = state_->activeTool();
        const QVariant sv = state_->option(tool, QStringLiteral("brush_size"));
        const double size = (sv.isValid() && sv.toDouble() > 0.0)
                                ? sv.toDouble()
                                : brushResizeStartSize_;
        const QVariant hv = state_->option(tool, QStringLiteral("brush_hardness"));
        const double hard = hv.isValid() ? hv.toDouble() : brushResizeStartHardness_;
        const QPointF cView = t.map(cursorDoc_);
        const QPointF eView =
            t.map(cursorDoc_ + QPointF(std::max(0.5, size * 0.5), 0.0));
        const double rv = std::max(1.5, std::hypot(eView.x() - cView.x(),
                                                   eView.y() - cView.y()));
        static const QColor kHud(0x3d, 0xa5, 0xff);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 230), 4));
        p.drawEllipse(cView, rv, rv);
        p.setPen(QPen(kHud, 2));
        p.drawEllipse(cView, rv, rv);
        if (hard > 0.5 && hard < 99.5) {
            p.setPen(QPen(kHud.lighter(130), 1.5));
            p.drawEllipse(cView, rv * hard / 100.0, rv * hard / 100.0);
        }
        // Readout chip tucked outside the ring, clamped into the viewport.
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        const QString text =
            tr("%1 px · %2%").arg(qRound(size)).arg(qRound(hard));
        const QFontMetrics fm(f);
        const int pad = 6;
        const QSize chip(fm.horizontalAdvance(text) + pad * 2,
                         fm.height() + pad);
        QPointF anchor(cView.x() + rv + 10.0, cView.y() - chip.height() - 8.0);
        const QRect view = viewport()->rect();
        if (anchor.x() + chip.width() > view.right() - 4)
            anchor.setX(cView.x() - rv - 10.0 - chip.width());
        if (anchor.y() < 4) anchor.setY(cView.y() + rv + 10.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 22, 26, 230));
        p.drawRoundedRect(QRectF(anchor, chip), 4, 4);
        p.setPen(Qt::white);
        p.drawText(QRectF(anchor, chip), Qt::AlignCenter, text);
        p.restore();
    }
}

void CanvasView::paintOverlay(QPainter& p) {
    DocumentItem* d = doc();
    const QTransform t = documentTransform();
    const ThemeColors c = colorsFor(state_->theme());
    // Legibility is a device-px question: at DPR 2 a 4-logical-px gap is 8
    // device px, so grid/ruler/pixel-grid gates use zoom × DPR.
    const qreal overlayDpr =
        p.device() ? qreal(p.device()->devicePixelRatioF()) : 1.0;
    const double effZoom =
        d->zoom * (overlayDpr > 0.0 ? overlayDpr : 1.0);

    // Grid (document-space multiples of the spacing setting, so lines stay
    // glued to the same coordinates at any zoom or rotation; view-space
    // stride keeps them legible). Loops run over the visible slice only,
    // indexed by multiple (no error accumulation across a huge document).
    if (grid_) {
        const QColor ink = state_->settings().gridColor;
        p.setPen(QPen(ink, 1));
        const double step = gridStep();
        if (step * effZoom > 4) {
            const QRectF vis =
                t.inverted().mapRect(QRectF(viewport()->rect()));
            const long long kx0 =
                std::max(0LL, (long long)std::floor(vis.left() / step));
            const long long kx1 =
                (long long)std::ceil(vis.right() / step);
            for (long long k = kx0; k <= kx1; ++k) {
                const double gx = k * step;
                if (gx > d->size.width()) break;
                p.drawLine(t.map(QPointF(gx, 0)),
                           t.map(QPointF(gx, d->size.height())));
            }
            const long long ky0 =
                std::max(0LL, (long long)std::floor(vis.top() / step));
            const long long ky1 =
                (long long)std::ceil(vis.bottom() / step);
            for (long long k = ky0; k <= ky1; ++k) {
                const double gy = k * step;
                if (gy > d->size.height()) break;
                p.drawLine(t.map(QPointF(0, gy)),
                           t.map(QPointF(d->size.width(), gy)));
            }
            // Subdivisions (Settings > Canvas): fainter lines between the
            // majors, gated by the same legibility rule. Major lines redraw
            // over them, so no doubling where they coincide.
            const int subdiv =
                qBound(1, state_->settings().gridSubdivisions, 16);
            const double sub = step / subdiv;
            if (subdiv > 1 && sub * effZoom > 4) {
                p.setPen(QPen(QColor(ink.red(), ink.green(), ink.blue(),
                                     ink.alpha() / 2),
                               1));
                const QRectF vis2 =
                    t.inverted().mapRect(QRectF(viewport()->rect()));
                for (long long k = static_cast<long long>(
                         std::floor(vis2.left() / sub));
                     k * sub <= vis2.right() + sub; ++k) {
                    const double gx = k * sub;
                    if (gx < 0 || gx > d->size.width()) continue;
                    p.drawLine(t.map(QPointF(gx, 0)),
                               t.map(QPointF(gx, d->size.height())));
                }
                for (long long k = static_cast<long long>(
                         std::floor(vis2.top() / sub));
                     k * sub <= vis2.bottom() + sub; ++k) {
                    const double gy = k * sub;
                    if (gy < 0 || gy > d->size.height()) continue;
                    p.drawLine(t.map(QPointF(0, gy)),
                               t.map(QPointF(d->size.width(), gy)));
                }
                p.setPen(QPen(ink, 1));
                for (long long k = kx0; k <= kx1; ++k) {
                    const double gx = k * step;
                    if (gx > d->size.width()) break;
                    p.drawLine(t.map(QPointF(gx, 0)),
                               t.map(QPointF(gx, d->size.height())));
                }
                for (long long k = ky0; k <= ky1; ++k) {
                    const double gy = k * step;
                    if (gy > d->size.height()) break;
                    p.drawLine(t.map(QPointF(0, gy)),
                               t.map(QPointF(d->size.width(), gy)));
                }
            }
        }
    }

    // Pixel grid: document-pixel boundaries, only where a pixel covers
    // enough screen to read (device px, so HiDPI shows it at the same
    // physical density instead of one zoom stop late).
    if (pixelGrid_ && effZoom >= 4.0) {
        p.setPen(QPen(QColor(255, 255, 255, 22), 1));
        const QRectF vis = t.inverted().mapRect(QRectF(viewport()->rect()));
        const int x0 = std::max(0, int(std::floor(vis.left())));
        const int x1 = std::min(d->size.width(), int(std::ceil(vis.right())));
        for (int x = x0; x <= x1; ++x)
            p.drawLine(t.map(QPointF(x, 0)),
                       t.map(QPointF(x, d->size.height())));
        const int y0 = std::max(0, int(std::floor(vis.top())));
        const int y1 =
            std::min(d->size.height(), int(std::ceil(vis.bottom())));
        for (int y = y0; y <= y1; ++y)
            p.drawLine(t.map(QPointF(0, y)),
                       t.map(QPointF(d->size.width(), y)));
    }

    // Guides.
    if (guides_) {
        const QColor guideInk = state_->settings().guideColor;
        p.setPen(QPen(guideInk, 1));
        for (double y : d->horizontalGuides) {
            const QPointF a = t.map(QPointF(0, y));
            const QPointF b = t.map(QPointF(d->size.width(), y));
            p.drawLine(a, b);
        }
        for (double x : d->verticalGuides) {
            const QPointF a = t.map(QPointF(x, 0));
            const QPointF b = t.map(QPointF(x, d->size.height()));
            p.drawLine(a, b);
        }
        // A ruler drag in flight previews dashed until release commits it.
        if (rulerGuideDragging_) {
            p.setPen(QPen(guideInk, 1, Qt::DashLine));
            if (rulerGuideHorizontal_) {
                const double y = rulerGuideDoc_.y();
                p.drawLine(t.map(QPointF(0, y)),
                           t.map(QPointF(d->size.width(), y)));
            } else {
                const double x = rulerGuideDoc_.x();
                p.drawLine(t.map(QPointF(x, 0)),
                           t.map(QPointF(x, d->size.height())));
            }
        }
    }

    // Stroke symmetry axes through the canvas center (dashed, always on
    // top of grid/guides so the mirror is visible while painting).
    if ((symmetryX_ || symmetryY_) && d->size.width() > 0 &&
        d->size.height() > 0) {
        const QColor guideInk = state_->settings().guideColor;
        p.setPen(QPen(guideInk, 1, Qt::DashLine));
        const double cx = d->size.width() / 2.0;
        const double cy = d->size.height() / 2.0;
        if (symmetryX_)
            p.drawLine(t.map(QPointF(cx, 0)),
                       t.map(QPointF(cx, d->size.height())));
        if (symmetryY_)
            p.drawLine(t.map(QPointF(0, cy)),
                       t.map(QPointF(d->size.width(), cy)));
    }

    // The selection in progress, then the committed selection's marching ants.
    // Quick Selection has no rectangular preview — its mask is committed on
    // release from the AI segmentation, so don't draw a misleading marquee.
    // Lasso tools preview their loop, the selection brush previews its paint.
    QRectF live;
    const ToolId liveTool = state_->activeTool();
    const bool lassoPreview =
        (liveTool == ToolId::Lasso || liveTool == ToolId::MagneticLasso) &&
        lassoLive_ && lassoStroke_.size() >= 2;
    const bool polyPreview = liveTool == ToolId::PolygonalLasso &&
                             (!polyPts_.empty());
    const bool selBrushPreview = liveTool == ToolId::SelectionBrush &&
                                 selBrushLive_ && !selBrushMask_.isNull();
    if (dragging_ &&
        (isSelectionTool(liveTool) || liveTool == ToolId::VectorCropTool ||
         liveTool == ToolId::Patch || liveTool == ToolId::RedEye ||
         liveTool == ToolId::Frame || liveTool == ToolId::GenerativeFill ||
         liveTool == ToolId::GenerateBackground ||
         liveTool == ToolId::ContentAwareMove ||
         liveTool == ToolId::PerspectiveCrop ||
         liveTool == ToolId::Artboard ||
         liveTool == ToolId::ContentAwareTracing) &&
        liveTool != ToolId::QuickSelection &&
        liveTool != ToolId::Lasso && liveTool != ToolId::MagneticLasso &&
        liveTool != ToolId::PolygonalLasso &&
        liveTool != ToolId::SelectionBrush)
        live = isSelectionTool(liveTool)
                   ? marqueeLiveRect()
                   : QRectF(dragStartDoc_, dragCurrentDoc_).normalized();

    auto drawAnts = [&](const QPainterPath& docPath) {
        if (docPath.isEmpty()) return;
        const QPainterPath mapped = t.map(docPath);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 1));
        p.drawPath(mapped);
        QPen dashed(Qt::black, 1, Qt::CustomDashLine);
        dashed.setDashPattern({4, 4});
        dashed.setDashOffset(antsPhase_);
        p.setPen(dashed);
        p.drawPath(mapped);
    };

    // Committed selection: an arbitrary-shape mask paints its true outline
    // (marching-squares boundary); rect/ellipse selections draw as before.
    // Hidden entirely when Selection Edges is off.
    QPainterPath committed;
    if (d->selectionIsMask)
        committed = selectionOutlinePath();
    else if (d->selectionIsEllipse)
        committed.addEllipse(d->selection);
    else
        committed.addRect(d->selection);
    if (selectionEdges_) drawAnts(committed);

    QPainterPath livePath;
    if (!live.isEmpty()) {
        if (state_->activeTool() == ToolId::EllipseMarquee)
            livePath.addEllipse(live);
        else
            livePath.addRect(live);
        if (selectionEdges_) drawAnts(livePath);
    }
    // Freehand loop + closing rubber.
    if (lassoPreview && selectionEdges_) drawAnts(lassoLivePath());
    // Polygonal anchors + rubber band (also when not dragging).
    if (polyPreview) {
        if (selectionEdges_) drawAnts(polyLivePath());
        p.save();
        p.setBrush(Qt::white);
        p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
        for (const QPointF& a : polyPts_) {
            const QPointF v = t.map(a);
            p.drawRect(QRectF(v.x() - 3, v.y() - 3, 6, 6));
        }
        p.restore();
    }
    // Selection-brush paint: blue tint + ants of the brushed area.
    if (selBrushPreview) {
        const QRect bbox = selectionMaskBbox(selBrushMask_);
        if (!bbox.isEmpty()) {
            const QPainterPath prev =
                selectionOutlineFromMask(selBrushMask_, bbox.adjusted(-1, -1, 1, 1));
            if (selectionEdges_) drawAnts(prev);
            QImage tint(selBrushMask_.size(), QImage::Format_ARGB32_Premultiplied);
            tint.fill(Qt::transparent);
            for (int yy = 0; yy < tint.height(); ++yy) {
                const uchar* srow = selBrushMask_.constScanLine(yy);
                QRgb* trow = reinterpret_cast<QRgb*>(tint.scanLine(yy));
                for (int xx = 0; xx < tint.width(); ++xx)
                    trow[xx] = qRgba(0, 130, 255, srow[xx] * 90 / 255);
            }
            p.drawImage(t.mapRect(QRectF(0, 0, tint.width(), tint.height())),
                        tint);
        }
    }

    // Perspective Crop quad + rule-of-thirds grid while armed (or while
    // the arming drag is in flight, which the marquee rect above covers).
    if (pcropArmed_ && state_->activeTool() == ToolId::PerspectiveCrop) {
        QPainterPath quad;
        quad.moveTo(t.map(pcropQuad_[0]));
        for (int i = 1; i < 4; ++i) quad.lineTo(t.map(pcropQuad_[i]));
        quad.closeSubpath();
        drawAnts(quad);
        if (state_->option(ToolId::PerspectiveCrop, QStringLiteral("show_grid"))
                .toBool()) {
            auto lerp = [&](const QPointF& a, const QPointF& b,
                            double f) {
                return t.map(a + (b - a) * f);
            };
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 255, 255, 160), 1));
            for (double f : {1.0 / 3.0, 2.0 / 3.0}) {
                QPainterPath gl;
                gl.moveTo(lerp(pcropQuad_[0], pcropQuad_[3], f));
                gl.lineTo(lerp(pcropQuad_[1], pcropQuad_[2], f));
                p.drawPath(gl);
                QPainterPath gv;
                gv.moveTo(lerp(pcropQuad_[0], pcropQuad_[1], f));
                gv.lineTo(lerp(pcropQuad_[3], pcropQuad_[2], f));
                p.drawPath(gv);
            }
            for (int i = 0; i < 4; ++i) {
                const QPointF h = t.map(pcropQuad_[i]);
                p.setPen(QPen(Qt::white, 2));
                p.drawRect(QRectF(h.x() - 4, h.y() - 4, 8, 8));
            }
        }
    }

    // Shape drag preview: the live outline in document coords, translucent
    // fill from the bar's colour. Builders are anchor-cheap; no raster here.
    if (dragging_ && isShapeTool(liveTool) && liveTool != ToolId::QRCode) {
        const QRectF box = QRectF(dragStartDoc_, dragCurrentDoc_).normalized();
        if (box.width() >= 1.0 && box.height() >= 1.0) {
            auto preview = makeShapeArt(
                liveTool, QRectF(0, 0, box.width(), box.height()),
                shapeStyleFor(state_, liveTool));
            if (preview) {
                const QPainterPath viewPath =
                    t.map(artNodePath(*preview).translated(box.x(), box.y()));
                QColor wash(preview->paint.fill[0], preview->paint.fill[1],
                            preview->paint.fill[2], 48);
                if (!preview->paint.hasFill) wash = QColor(0x3d, 0xa5, 0xff, 36);
                p.setBrush(wash);
                p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
                p.drawPath(viewPath);
            }
        }
    }

    // Node tool: outline plus endpoints of the editable shape (the working
    // copy mid-drag, with the grabbed point filled). Squares stay view-sized.
    if (liveTool == ToolId::NodeTool) {
        const int layerIndex = nodeDragging_ ? nodeLayer_ : vectorEditableLayer(state_);
        const LayerItem* nl = (layerIndex >= 0 && layerIndex < d->layers.size())
                                  ? &d->layers[layerIndex]
                                  : nullptr;
        if (nl && nl->art && !nl->art->isEmpty() && nl->scaleX > 0.0 &&
            nl->scaleY > 0.0) {
            const pittore::vector::ArtNode& node =
                nodeDragging_ ? nodeWork_ : *nl->art;
            // Node → document: art matrix, then layer scale, then offset
            // (Qt composes left-first).
            const QTransform docT =
                QTransform(nl->art->matrix[0], nl->art->matrix[1],
                           nl->art->matrix[2], nl->art->matrix[3],
                           nl->art->matrix[4], nl->art->matrix[5]) *
                QTransform().scale(nl->scaleX, nl->scaleY) *
                QTransform().translate(nl->offset.x(), nl->offset.y());
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
            p.drawPath(t.map(docT.map(artNodePath(node))));
            const std::vector<QPointF> pts = nodeEndpoints(node);
            for (const QPointF& np : pts) {
                const QPointF v = t.map(docT.map(np));
                p.setBrush(Qt::white);
                p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
                p.drawRect(QRectF(v.x() - 4, v.y() - 4, 8, 8));
            }
            // Bezier handles: arm lines plus round tips. The selected
            // anchor draws filled so convert/close has a visible target.
            for (const NodeHandle& h : nodeHandles(node)) {
                const auto& as = node.segments[static_cast<std::size_t>(h.anchorSeg)];
                const QPointF a = t.map(docT.map(QPointF(as.x, as.y)));
                const QPointF b = t.map(docT.map(h.pos));
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.0));
                p.drawLine(a, b);
                p.setBrush(Qt::white);
                p.drawEllipse(b, 3.0, 3.0);
            }
            if (nodeSelLayer_ == layerIndex && nodeSelSeg_ >= 0 &&
                nodeSelSeg_ < static_cast<int>(node.segments.size())) {
                const auto& ss =
                    node.segments[static_cast<std::size_t>(nodeSelSeg_)];
                const QPointF v = t.map(docT.map(QPointF(ss.x, ss.y)));
                p.setBrush(QColor(0x3d, 0xa5, 0xff));
                p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
                p.drawRect(QRectF(v.x() - 4, v.y() - 4, 8, 8));
            }
            if (nodeDragging_ && nodeSeg_ >= 0 &&
                nodeSeg_ < static_cast<int>(node.segments.size())) {
                QPointF ep;
                const auto& s = node.segments[static_cast<std::size_t>(nodeSeg_)];
                if (s.kind == pittore::vector::Segment::Kind::MoveTo ||
                    s.kind == pittore::vector::Segment::Kind::LineTo ||
                    s.kind == pittore::vector::Segment::Kind::CubicTo)
                    ep = QPointF(s.x, s.y);
                else if (!pts.empty())
                    ep = pts.front();
                else
                    ep = QPointF();
                const QPointF v = t.map(docT.map(ep));
                p.setBrush(QColor(0x3d, 0xa5, 0xff));
                p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
                p.drawRect(QRectF(v.x() - 4, v.y() - 4, 8, 8));
            }
        }
    }

    // Pen / Freehand / Curvature: working path, rubber band, anchors and
    // live Bezier handles. Squares stay view-sized; document coords.
    if (penActive_ && isPenTool(liveTool)) {
        const PenMode mode = penModeFor(penTool_);
        pittore::vector::ArtNode tmp;
        tmp.segments = penPath_.toSegments(mode);
        const QColor blue(0x3d, 0xa5, 0xff);
        if (!tmp.segments.empty()) {
            QPainterPath docPath = artNodePath(tmp);
            const QVariant rbOpt =
                state_->option(penTool_, QStringLiteral("rubberband"));
            const bool rubber = rbOpt.isValid() ? rbOpt.toBool() : true;
            if (rubber && !penPath_.closed() && !penPath_.isEmpty() &&
                mode != PenMode::Freehand) {
                QPainterPath band = docPath;
                const QPointF last =
                    penPath_.points().back().anchor;
                if (band.isEmpty())
                    band.moveTo(last);
                else
                    band.lineTo(last);
                band.lineTo(penHoverDoc_);
                docPath = band;
            }
            QColor wash = QColor(blue.red(), blue.green(), blue.blue(), 36);
            if (penPath_.closed()) {
                const ShapeStyle st = shapeStyleFor(state_, penTool_);
                if (st.hasFill) wash = QColor(st.fill.red(), st.fill.green(),
                                             st.fill.blue(), 48);
            }
            p.setBrush(wash);
            p.setPen(QPen(blue, 1.5));
            p.drawPath(t.map(docPath));
            const double closeTol = 8.0 / qMax(0.01, d->zoom);
            const bool closing =
                penPath_.closeHit(penHoverDoc_, closeTol);
            int ai = 0;
            for (const PenPoint& pp : penPath_.points()) {
                if (mode == PenMode::Bezier) {
                    p.setBrush(Qt::NoBrush);
                    p.setPen(QPen(blue, 1.0));
                    for (const auto& h :
                         {std::make_pair(pp.hasIn, pp.inHandle),
                          std::make_pair(pp.hasOut, pp.outHandle)}) {
                        if (!h.first) continue;
                        const QPointF a = t.map(pp.anchor);
                        const QPointF b = t.map(h.second);
                        p.drawLine(a, b);
                        p.setBrush(Qt::white);
                        p.drawEllipse(b, 3.0, 3.0);
                        p.setBrush(Qt::NoBrush);
                    }
                }
                const QPointF v = t.map(pp.anchor);
                const bool first = ai == 0;
                p.setBrush((first && closing) ? blue : Qt::white);
                p.setPen(QPen(blue, 1.5));
                const double s = (first && closing) ? 5.0 : 4.0;
                p.drawRect(QRectF(v.x() - s, v.y() - s, 2 * s, 2 * s));
                ++ai;
            }
        }
    }
    // Vector Brush: working ribbon outline in the bar colour.
    if (vbrushActive_ && liveTool == ToolId::VectorBrushTool &&
        !vbrushStroke_.isEmpty()) {
        pittore::vector::ArtNode tmp;
        double nibRatio, nibAngle;
        bool nibSquare;
        vbrushNib(nibRatio, nibAngle, nibSquare);
        tmp.segments = vbrushStroke_.buildRibbon(nibRatio, nibAngle,
                                                 nibSquare);
        if (!tmp.segments.empty()) {
            QColor color = state_->option(ToolId::VectorBrushTool,
                                          QStringLiteral("color"))
                               .value<QColor>();
            if (!color.isValid()) color = state_->foreground();
            const QColor blue(0x3d, 0xa5, 0xff);
            p.setBrush(QColor(color.red(), color.green(), color.blue(), 48));
            p.setPen(QPen(blue, 1.5));
            p.drawPath(t.map(artNodePath(tmp)));
        }
    }

    // Shape Builder: marquee preview.
    if (builderActive_ && liveTool == ToolId::ShapeBuilderTool) {
        const QRectF box =
            QRectF(builderStartDoc_, builderCurDoc_).normalized();
        if (box.width() > 1.0 || box.height() > 1.0) {
            p.setBrush(QColor(0x3d, 0xa5, 0xff, 36));
            p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.0));
            p.drawRect(t.map(box).boundingRect());
        }
    }

    // Knife: the cut line. Stroke Width: live width readout at the press
    // point. Point Transform: editable endpoints plus centroid.
    if (knifeActive_ && liveTool == ToolId::KnifeTool) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 60, 60), 1.5));
        p.drawLine(t.map(knifeStartDoc_), t.map(knifeCurDoc_));
    }
    if (swActive_ && liveTool == ToolId::StrokeWidthTool) {
        const QPointF v = t.map(swPressDoc_) + QPointF(12, -12);
        p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.0));
        p.drawText(v, tr("%1 px").arg(swLiveWidth_, 0, 'f', 1));
        // Working outline from the preview paint, plus the committed
        // profile points as dots on the centerline.
        if (swLayer_ >= 0 && swLayer_ < d->layers.size()) {
            const LayerItem* nl = &d->layers[swLayer_];
            if (nl->art && !nl->art->isEmpty()) {
                const QTransform docT =
                    QTransform(nl->art->matrix[0], nl->art->matrix[1],
                               nl->art->matrix[2], nl->art->matrix[3],
                               nl->art->matrix[4], nl->art->matrix[5]) *
                    QTransform().scale(nl->scaleX, nl->scaleY) *
                    QTransform().translate(nl->offset.x(), nl->offset.y());
                if (swHasPreview_) {
                    pittore::vector::ArtNode tmp = *nl->art;
                    tmp.paint = swPreviewPaint_;
                    const QPainterPath preview = expandStrokePath(tmp);
                    if (!preview.isEmpty()) {
                        p.setBrush(Qt::NoBrush);
                        p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.0));
                        p.drawPath(t.map(docT.map(preview)));
                    }
                }
                if (nl->art->paint.hasProfile) {
                    const pittore::vector::Path flat =
                        pittore::vector::flattenSegments(nl->art->segments,
                                                          0.25f);
                    p.setBrush(Qt::white);
                    p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.0));
                    for (std::size_t si = 0; si < flat.subpaths.size(); ++si) {
                        const auto& sub = flat.subpaths[si];
                        double total = 0.0;
                        std::vector<double> cum(sub.size(), 0.0);
                        for (std::size_t k = 1; k < sub.size(); ++k) {
                            total += std::hypot(sub[k].first - sub[k - 1].first,
                                                sub[k].second - sub[k - 1].second);
                            cum[k] = total;
                        }
                        for (const auto& wp : nl->art->paint.profile) {
                            const double target =
                                std::clamp((double)wp.t, 0.0, 1.0) * total;
                            std::size_t k = 1;
                            while (k + 1 < sub.size() && cum[k] < target) ++k;
                            const double span = cum[k] - cum[k - 1];
                            const double f =
                                (span > 1e-9) ? (target - cum[k - 1]) / span : 0.0;
                            const QPointF np(
                                sub[k - 1].first +
                                    (sub[k].first - sub[k - 1].first) *
                                        std::clamp(f, 0.0, 1.0),
                                sub[k - 1].second +
                                    (sub[k].second - sub[k - 1].second) *
                                        std::clamp(f, 0.0, 1.0));
                            const QPointF vv = t.map(docT.map(np));
                            p.drawEllipse(vv, 3.0, 3.0);
                        }
                    }
                }
            }
        }
    }
    if (liveTool == ToolId::PointTransformTool) {
        const int layerIndex = ptDragging_ ? ptLayer_ : vectorEditableLayer(state_);
        const LayerItem* nl = (layerIndex >= 0 && layerIndex < d->layers.size())
                                  ? &d->layers[layerIndex]
                                  : nullptr;
        if (nl && nl->art && !nl->art->isEmpty() && nl->scaleX > 0.0 &&
            nl->scaleY > 0.0) {
            const pittore::vector::ArtNode& node =
                ptDragging_ ? ptWork_ : *nl->art;
            const QTransform docT =
                QTransform(nl->art->matrix[0], nl->art->matrix[1],
                           nl->art->matrix[2], nl->art->matrix[3],
                           nl->art->matrix[4], nl->art->matrix[5]) *
                QTransform().scale(nl->scaleX, nl->scaleY) *
                QTransform().translate(nl->offset.x(), nl->offset.y());
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.5));
            p.drawPath(t.map(docT.map(artNodePath(node))));
            for (const QPointF& np : nodeEndpoints(node)) {
                const QPointF v = t.map(docT.map(np));
                p.setBrush(Qt::white);
                p.drawRect(QRectF(v.x() - 4, v.y() - 4, 8, 8));
            }
            const QPointF c = t.map(docT.map(nodeAnchorCentroid(node)));
            p.setPen(QPen(QColor(0x3d, 0xa5, 0xff), 1.0));
            p.drawLine(c + QPointF(-6, 0), c + QPointF(6, 0));
            p.drawLine(c + QPointF(0, -6), c + QPointF(0, 6));
        }
    }

    // Vector drag tools: spray cone, tweak ring, LPE chord,
    // connector rubber-band (own TU canvas_vector_tools; no-op otherwise).
    if (inkActive_) paintInkPreview(p, t);

    // Gradient / Transparency: live axis preview.
    if (vgradActive_ &&
        (liveTool == ToolId::Gradient ||
         liveTool == ToolId::TransparencyTool)) {
        const QColor blue(0x3d, 0xa5, 0xff);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(blue, 1.5));
        p.drawLine(t.map(vgradStartDoc_), t.map(vgradCurDoc_));
        p.setBrush(blue);
        p.drawEllipse(t.map(vgradStartDoc_), 4.0, 4.0);
        p.setBrush(Qt::white);
        p.drawEllipse(t.map(vgradCurDoc_), 4.0, 4.0);
    }

    // Object Select hover preview: the blue overlay of the object the decoder
    // currently finds under the pointer, before the click commits it.
    if (hoverPreviewActive_ && !hoverPreviewMask_.isNull()) {
        const QImage& m = hoverPreviewMask_;
        const QRectF docRect(0, 0, double(m.width()), double(m.height()));
        // Cap the preview channel at ~2k on the long edge: the overlay is
        // translucent colour only, so a downscale keeps hover moves cheap.
        const double scale =
            std::min(1.0, 2048.0 / std::max(m.width(), m.height()));
        QImage tint =
            (scale < 1.0 ? m.scaled(m.size() * scale, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation)
                         : m);
        QImage overlay(tint.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        for (int yy = 0; yy < tint.height(); ++yy) {
            const uchar* srow = tint.constScanLine(yy);
            QRgb* trow = reinterpret_cast<QRgb*>(overlay.scanLine(yy));
            for (int xx = 0; xx < tint.width(); ++xx)
                trow[xx] = qRgba(0, 130, 255, srow[xx] * 130 / 255);
        }
        p.drawImage(t.mapRect(docRect), overlay);
    }

    // Crop overlay: the live frame with a darkened surround, the guides the
    // tool's Overlay option asks for, eight resize handles and a live size
    // readout. The frame is view state — the document is untouched until the
    // crop commits (Enter, double-click, or the task bar's Apply). Gated on
    // the tool itself, so Perspective Crop draws only its own quad.
    if (state_->activeTool() == ToolId::Crop &&
        state_->taskContext() == TaskContext::Crop) {
        const QRectF box = cropOverlayRect();
        if (!box.isEmpty()) {
            const QRectF mapped = t.map(QPolygonF(box)).boundingRect();
            QPainterPath outside;
            outside.addRect(QRectF(viewport()->rect()));
            QPainterPath inside;
            inside.addPolygon(t.map(QPolygonF(box)));
            p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 140));

            // Overlay guides per the tool option. "Never Show Overlay" (6)
            // keeps only the frame and handles; the drawn geometries are the
            // thirds, the sixths grid and the golden section — the option
            // values this build renders.
            const int overlay =
                state_->option(ToolId::Crop, QStringLiteral("overlay")).toInt();
            if (overlay != 6) {
                p.setPen(QPen(QColor(255, 255, 255, 120), 1));
                if (overlay == 1) {
                    for (int i = 1; i < 6; ++i) {
                        const double x = mapped.left() + mapped.width() * i / 6.0;
                        const double y = mapped.top() + mapped.height() * i / 6.0;
                        p.drawLine(QPointF(x, mapped.top()), QPointF(x, mapped.bottom()));
                        p.drawLine(QPointF(mapped.left(), y), QPointF(mapped.right(), y));
                    }
                } else if (overlay == 4) {
                    static const double kGolden[2] = {0.382, 0.618};
                    for (const double f : kGolden) {
                        const double x = mapped.left() + mapped.width() * f;
                        const double y = mapped.top() + mapped.height() * f;
                        p.drawLine(QPointF(x, mapped.top()), QPointF(x, mapped.bottom()));
                        p.drawLine(QPointF(mapped.left(), y), QPointF(mapped.right(), y));
                    }
                } else {
                    for (int i = 1; i < 3; ++i) {
                        const double x = mapped.left() + mapped.width() * i / 3.0;
                        const double y = mapped.top() + mapped.height() * i / 3.0;
                        p.drawLine(QPointF(x, mapped.top()), QPointF(x, mapped.bottom()));
                        p.drawLine(QPointF(mapped.left(), y), QPointF(mapped.right(), y));
                    }
                }
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::white, 1));
            p.drawRect(mapped);

            // Corner and edge handles: the same 0..7 order the press hit
            // test uses, so what is painted is exactly what is grabbable.
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            for (int h = 0; h < 8; ++h) {
                const QPointF hp = t.map(cropHandlePos(h, box));
                p.drawRect(QRectF(hp.x() - 3.5, hp.y() - 3.5, 7, 7));
            }

            // Live size readout on a chip along the frame's bottom edge.
            const QString label = QStringLiteral("%1 × %2")
                                      .arg(qRound(box.width()))
                                      .arg(qRound(box.height()));
            const QFontMetrics fm(p.font());
            const double cw = fm.horizontalAdvance(label) + 12.0;
            const double ch = fm.height() + 6.0;
            const QRectF chip(QPointF(mapped.center().x() - cw * 0.5,
                                      mapped.bottom() - ch - 8.0),
                              QSizeF(cw, ch));
            p.fillRect(chip, QColor(0, 0, 0, 160));
            p.setPen(Qt::white);
            p.drawText(chip, Qt::AlignCenter, label);
        }
    }

    // Transform handles on the active layer bounds.
    if (state_->taskContext() == TaskContext::Transform) {
        const QRectF bounds(d->size.width() * 0.2, d->size.height() * 0.2,
                            d->size.width() * 0.6, d->size.height() * 0.6);
        const QRectF mapped = t.map(QPolygonF(bounds)).boundingRect();
        p.setPen(QPen(c.accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(mapped);
        p.setBrush(Qt::white);
        p.setPen(QPen(c.accent, 1));
        for (QPointF handle : {mapped.topLeft(), mapped.topRight(), mapped.bottomLeft(),
                               mapped.bottomRight(), mapped.center()})
            p.drawRect(QRectF(handle.x() - 3.5, handle.y() - 3.5, 7, 7));
    }

    // Move-tool transform controls: the active layer's bounds with 8
    // drag handles. Overlay only — never composited into the document.
    if (moveTransformVisible()) {
        const QRectF bounds = activeLayerBounds();
        const QRectF mapped = t.map(QPolygonF(bounds)).boundingRect();
        p.setPen(QPen(c.accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(mapped);
        p.setBrush(Qt::white);
        p.setPen(QPen(c.accent, 1));
        for (int h = 0; h < 8; ++h) {
            const QPointF hp = t.map(handleDocPos(h, bounds));
            p.drawRect(QRectF(hp.x() - 3.5, hp.y() - 3.5, 7, 7));
        }
    }

    // Move-drag smart guides: the document centre is a live readout in both
    // directions (green vertical, red horizontal), brightening on a centre
    // snap; any other snapped target gets its own line. Gated on the
    // Smart Guides toggle.
    if (moveDragging_ && d && smartGuides_ && state_->snapEnabled() &&
        state_->snapTargets()) {
        const double cx = d->size.width() / 2.0;
        const double cy = d->size.height() / 2.0;
        const bool xHot = moveSnap_.x && moveSnap_.xCenter;
        const bool yHot = moveSnap_.y && moveSnap_.yCenter;
        p.setPen(QPen(QColor(0x2e, 0xc4, 0x4e, xHot ? 255 : 70), xHot ? 2 : 1));
        p.drawLine(t.map(QPointF(cx, 0)), t.map(QPointF(cx, d->size.height())));
        p.setPen(QPen(QColor(0xe7, 0x3c, 0x3c, yHot ? 255 : 70), yHot ? 2 : 1));
        p.drawLine(t.map(QPointF(0, cy)), t.map(QPointF(d->size.width(), cy)));
        if (moveSnap_.x && !moveSnap_.xCenter) {
            p.setPen(QPen(QColor(0x2e, 0xc4, 0x4e), 1));
            p.drawLine(t.map(QPointF(moveSnap_.xPos, 0)),
                       t.map(QPointF(moveSnap_.xPos, d->size.height())));
        }
        if (moveSnap_.y && !moveSnap_.yCenter) {
            p.setPen(QPen(QColor(0xe7, 0x3c, 0x3c), 1));
            p.drawLine(t.map(QPointF(0, moveSnap_.yPos)),
                       t.map(QPointF(d->size.width(), moveSnap_.yPos)));
        }
    }

    // Drop highlight while an image hovers the viewport.
    if (dropActive_) {
        p.setPen(QPen(Qt::white, 2, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 24));
        p.drawRect(viewport()->rect().adjusted(8, 8, -8, -8));
    }

    // Brush cursor + resize HUD (always visible): see paintBrushCursor.

    // Type tool overlay: the frame box being dragged out, the live editing
    // frame, and the caret.
    if (isTypeTool(state_->activeTool())) {
        if (textCreating_) {
            const ToolId tool = state_->activeTool();
            const double optionSize =
                state_->option(tool, QStringLiteral("size")).toDouble();
            const QColor ink = state_->option(tool, QStringLiteral("color")).value<QColor>();
            if (textCreateFrame_) {
                const QPointF a = t.map(textCreateStartDoc_);
                const QPointF b = t.map(cursorDoc_);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
                p.drawRect(QRectF(a, b).normalized());
            } else {
                // Draw the preview "A" at the size the drag is dialling in: the
                // letter grows under the cursor, then becomes the caret.
                const double size = textCreateSize_ > 4.0
                                        ? textCreateSize_
                                        : (optionSize > 0 ? optionSize : 36.0);
                p.save();
                p.setTransform(t);
                QFont f = p.font();
                f.setPixelSize(std::max(1, int(std::round(size))));
                const int style =
                    state_->option(tool, QStringLiteral("style")).toInt();
                f.setBold(style >= 3);
                f.setItalic(style == 1);
                p.setFont(f);
                p.setPen(QPen(ink.isValid() ? ink : QColor(Qt::black), 0));
                // Keep the glyph's top near the click, where the text layer's
                // first line will actually start.
                p.drawText(QPointF(textCreateStartDoc_.x(),
                                   textCreateStartDoc_.y() + size * 0.8),
                           QStringLiteral("A"));
                p.restore();
            }
        }
        DocumentItem* dd = doc();
        if (textEditing_ && dd && textEditIndex_ >= 0 &&
            textEditIndex_ < dd->layers.size()) {
            const LayerItem& l = dd->layers[textEditIndex_];
            if (l.liveText) {
                const TextItem& tp = l.textSpec;
                if (tp.wrapWidth > 0.5 && tp.frameHeight > 0.0) {
                    const QRectF frame(tp.origin, QSizeF(tp.wrapWidth, tp.frameHeight));
                    p.setBrush(Qt::NoBrush);
                    p.setPen(QPen(c.accent, 1));
                    p.drawRect(t.map(QPolygonF(frame)).boundingRect());
                }
                // Selection highlight: one quad per line, from the anchor to
                // the caret, mapped through the document transform.
                if (textLayoutValid_ && textAnchor_ != textCaret_) {
                    const std::size_t lo = std::min(textAnchor_, textCaret_);
                    const std::size_t hi = std::max(textAnchor_, textCaret_);
                    QColor hl = c.accent;
                    hl.setAlpha(90);
                    p.setPen(Qt::NoPen);
                    p.setBrush(hl);
                    for (const pittore::text::LineSpan& span : textLayout_.lines) {
                        if (span.end <= lo || span.start >= hi) continue;
                        const std::size_t a = std::max(lo, span.start);
                        const std::size_t b = std::min(hi, span.end);
                        const double x0 = textLineX(span, a);
                        const double x1 = b > a ? textLineX(span, b) : x0;
                        if (x1 <= x0) continue;
                        const QRectF r(textLayoutToDoc(QPointF(x0, span.top)),
                                       textLayoutToDoc(QPointF(x1, span.top + span.height)));
                        p.drawPolygon(t.map(QPolygonF(r.normalized())));
                    }
                }
                if (textCaretOn_) {
                    QPointF top, bot;
                    if (textCaretSegment(top, bot)) {
                        const QPointF a = t.map(top);
                        const QPointF b = t.map(bot);
                        // Black beneath white so the caret reads on any document.
                        p.setPen(QPen(QColor(0, 0, 0, 180), 1));
                        p.drawLine(a, b);
                        p.setPen(QPen(QColor(255, 255, 255, 230), 1));
                        p.drawLine(a + QPointF(1, 0), b + QPointF(1, 0));
                    }
                }
            }
        }
    }

    // Annotation tools: sample pins, notes, count markers, ruler.
    paintAnnotationOverlay(p, t);

    // Artboard chrome: board outlines plus each board's name above its
    // top-left corner. Last, so the label never falls under a selection
    // marquee or a tool overlay.
    paintArtboardOverlay(p, t);
}

}  // namespace pittore::ui
