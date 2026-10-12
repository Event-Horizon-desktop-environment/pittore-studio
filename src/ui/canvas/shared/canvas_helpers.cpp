#include "ui/canvas/shared/canvas_helpers.h"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <cmath>

#include "ui/canvas_view.h"
#include "ui/selection_mask.h"

namespace pittore::ui {


// The cursor shown while a brush tool owns the pointer: invisible, so the
// painted brush-size ring is the only cursor. Plain Qt::BlankCursor is enough
// on X11, but on Wayland (Hyprland) with NVIDIA hardware cursors some
// compositors keep rendering a stale shape when a client blanks its cursor.
// An explicit 1x1 fully-transparent image cursor is a real, composited buffer
// and cannot fall back to the previous shape.
QCursor brushHiddenCursor() {
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
        QPixmap pm(1, 1);
        pm.fill(Qt::transparent);
        return QCursor(pm);
    }
    return QCursor(Qt::BlankCursor);
}

// The standard zoom ladder lives in canvas_helpers.h (zoomSteps): inline so
// link units that pull in only a subset of the UI sources still reach it.

bool isSelectionTool(ToolId id) {
    switch (id) {
        case ToolId::RectMarquee:
        case ToolId::EllipseMarquee:
        case ToolId::SingleRowMarquee:
        case ToolId::SingleColumnMarquee:
        case ToolId::Lasso:
        case ToolId::PolygonalLasso:
        case ToolId::MagneticLasso:
        case ToolId::QuickSelection:
        case ToolId::ObjectSelection:
        case ToolId::MagicWand:
        case ToolId::SelectionBrush:
            return true;
        default:
            return false;
    }
}

// Tools that stroke pixels with the dab kernel. Eraser shares the brush's
// stroke machinery; it just replaces source-over painting with alpha erase.
// Dodge/Burn/Sponge share it too: the dab mask drives a tonal operator instead.
bool isPaintTool(ToolId id) {
    switch (id) {
        case ToolId::Brush:
        case ToolId::Pencil:
        case ToolId::Eraser:
        case ToolId::Dodge:
        case ToolId::Burn:
        case ToolId::Sponge:
        case ToolId::Blur:
        case ToolId::Sharpen:
        case ToolId::BackgroundEraser:
        case ToolId::PatternStamp:
        case ToolId::HistoryBrush:
        case ToolId::HealingBrush:
        case ToolId::ArtHistoryBrush:
        case ToolId::MixerBrush:
        case ToolId::Remove:
        case ToolId::AdjustmentBrush:
        case ToolId::ColorReplacement:
        case ToolId::SpotHealing:
        case ToolId::CloneStamp:
        case ToolId::Smudge:
            return true;
        default:
            return false;
    }
}

// Type tools: press/drag creates a live text layer, or a click inside one
// resumes editing it.
bool isTypeTool(ToolId id) {
    return id == ToolId::HorizontalType || id == ToolId::VerticalType;
}

bool isShapeTool(ToolId id) {
    switch (id) {
        case ToolId::Rectangle:
        case ToolId::Ellipse:
        case ToolId::RoundedRectangle:
        case ToolId::Triangle:
        case ToolId::Diamond:
        case ToolId::Trapezoid:
        case ToolId::Polygon:
        case ToolId::Star:
        case ToolId::DoubleStar:
        case ToolId::SquareStar:
        case ToolId::Arrow:
        case ToolId::DoubleArrow:
        case ToolId::Donut:
        case ToolId::Pie:
        case ToolId::Segment:
        case ToolId::Crescent:
        case ToolId::Cog:
        case ToolId::Cloud:
        case ToolId::CalloutRect:
        case ToolId::CalloutEllipse:
        case ToolId::Tear:
        case ToolId::Heart:
        case ToolId::Spiral:
        case ToolId::QRCode:
        case ToolId::Cat:
        case ToolId::Hexagon:
        case ToolId::Octagon:
        case ToolId::Cross:
        case ToolId::RightTriangle:
        case ToolId::Parallelogram:
        case ToolId::Chevron:
        case ToolId::CircularArrow:
        case ToolId::Sparkle:
        case ToolId::Shield:
        case ToolId::Ticket:
        case ToolId::Sun:
        case ToolId::Line:
        case ToolId::CustomShape:
            return true;
        default:
            return false;
    }
}

bool isPenTool(ToolId id) {
    return id == ToolId::Pen || id == ToolId::FreeformPen ||
           id == ToolId::CurvaturePen;
}

PenMode penModeFor(ToolId id) {
    if (id == ToolId::FreeformPen) return PenMode::Freehand;
    if (id == ToolId::CurvaturePen) return PenMode::Curvature;
    return PenMode::Bezier;
}

// One dab of a Liquify brush. The mesh
// is in layer native pixels; (dx, dy) is the drag delta in layer pixels
// (Forward Warp and Push Left deform along it), and the radial brushes read each
// vertex's position relative to the brush centre. Returns true when the dab can
// have changed the mesh, so a click with no drag does not fabricate an undo
// step for Forward Warp / Push Left.
bool applyLiquifyBrush(compute::WarpMesh& m, int mode, float cx, float cy,
                       float radius, float strength, float dx, float dy) {
    using compute::warp_mesh_for_each_near;
    using compute::warp_mesh_relax;
    switch (mode) {
        case 0:  // Forward Warp: fetch from behind the drag, so pixels follow it.
            if (dx == 0.0f && dy == 0.0f) return false;
            warp_mesh_for_each_near(m, cx, cy, radius,
                                    [=](float& ox, float& oy, float w, float, float) {
                                        ox -= dx * w * strength;
                                        oy -= dy * w * strength;
                                    });
            return true;
        case 1:  // Reconstruct: pull the offsets back towards the identity.
            if (strength <= 0.0f) return false;
            warp_mesh_relax(m, cx, cy, radius, strength * 0.5f);
            return true;
        case 2:    // Twirl CW
        case 3: {  // Twirl CCW
            const float step = strength * 0.25f * (mode == 2 ? 1.0f : -1.0f);
            if (step == 0.0f) return false;
            warp_mesh_for_each_near(m, cx, cy, radius,
                                    [=](float& ox, float& oy, float w, float rx, float ry) {
                                        const float a = step * w;
                                        const float s = std::sin(a), c = std::cos(a);
                                        const float fx = rx + ox, fy = ry + oy;
                                        ox = fx * c - fy * s - rx;
                                        oy = fx * s + fy * c - ry;
                                    });
            return true;
        }
        case 4:    // Pucker
        case 5: {  // Bloat
            const float step = strength * 0.15f * (mode == 4 ? 1.0f : -1.0f);
            if (step == 0.0f) return false;
            warp_mesh_for_each_near(m, cx, cy, radius,
                                    [=](float& ox, float& oy, float w, float rx, float ry) {
                                        const float fx = rx + ox, fy = ry + oy;
                                        ox += fx * step * w;
                                        oy += fy * step * w;
                                    });
            return true;
        }
        case 6: {  // Push Left: slide pixels perpendicular to the drag.
            const float len = std::hypot(dx, dy);
            if (len < 1e-4f) return false;
            const float px = dy / len, py = -dx / len;
            const float push = len * strength;
            warp_mesh_for_each_near(m, cx, cy, radius,
                                    [=](float& ox, float& oy, float w, float, float) {
                                        ox -= px * push * w;
                                        oy -= py * push * w;
                                    });
            return true;
        }
        default:
            return false;
    }
}


// Step a UTF-8 byte offset by one character, never splitting a multibyte
// sequence. Both clamp at the string ends.
std::size_t prevCharBoundary(const std::string& s, std::size_t at) {
    if (at == 0) return 0;
    if (at > s.size()) at = s.size();
    --at;
    while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
    return at;
}

std::size_t nextCharBoundary(const std::string& s, std::size_t at) {
    if (at >= s.size()) return s.size();
    ++at;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) ++at;
    return at;
}



// --- Object Select alpha restriction ---------------------------------------
// SAM returns a smooth blob with no notion of transparency, so on a cutout
// layer it happily selects the fully transparent background around the
// object — pixels that carry no image information and can never be part of
// the object. The layer's own alpha plane is ground truth for "pixels with
// information": restricting the selection to opaque content can never remove
// a real object pixel, and on a full photo (no transparency) the restriction
// is a no-op.

// Document-space channel of the layer's pixels with alpha > `floor`/255;
// `opaqueFraction` reports their share of the layer (1.0 = full photo).
QImage layerOpaqueDocMask(const LayerItem* layer, int floor,
                          const QSize& docSize, double& opaqueFraction) {
    opaqueFraction = 1.0;
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels)
        return {};
    auto* img = layer->pixels.get();
    const int pw = static_cast<int>(img->width());
    const int ph = static_cast<int>(img->height());
    const double th = double(floor) / 255.0;
    const std::size_t n = std::size_t(pw) * std::size_t(ph);
    std::vector<float> alpha(n, 0.0f);
    std::size_t opaque = 0;
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x)
            if (img->at(x, y).a > th) {
                alpha[std::size_t(y) * pw + x] = 1.0f;
                ++opaque;
            }
    opaqueFraction = double(opaque) / double(n);
    return selectionMaskFromLayerAlpha(alpha.data(), pw, ph, layer->offset,
                                       layer->scaleX, layer->scaleY, docSize);
}

// Keep only the 8-connected >127 component containing `seed`; everything else
// becomes 0 (the input's own values survive inside the component). On a
// cutout layer the click lands on the object itself, so this component IS the
// object's silhouette — every one of its pixels, including tonal detail the
// model mistakes for background.
QImage cutoutComponentAt(const QImage& opaque, const QPoint& seed) {
    const int w = opaque.width(), h = opaque.height();
    if (seed.x() < 0 || seed.y() < 0 || seed.x() >= w || seed.y() >= h)
        return {};
    const int stride = opaque.bytesPerLine();
    const uchar* bits = opaque.constBits();
    if (bits[std::size_t(seed.y()) * stride + seed.x()] <= 127) return {};
    std::vector<std::uint8_t> seen(std::size_t(w) * h, 0);
    const auto seenAt = [&](int x, int y) -> std::uint8_t& {
        return seen[std::size_t(y) * w + x];
    };
    std::vector<int> stack;
    stack.reserve(1 << 16);
    stack.push_back(seed.y() * w + seed.x());
    seenAt(seed.x(), seed.y()) = 1;
    while (!stack.empty()) {
        const int idx = stack.back();
        stack.pop_back();
        const int x = idx % w, y = idx / w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                if (seenAt(nx, ny)) continue;
                if (bits[std::size_t(ny) * stride + nx] <= 127) continue;
                seenAt(nx, ny) = 1;
                stack.push_back(ny * w + nx);
            }
    }
    QImage out(opaque.size(), QImage::Format_Grayscale8);
    out.fill(0);
    for (int y = 0; y < h; ++y) {
        const uchar* src = bits + std::size_t(y) * stride;
        uchar* dst = out.scanLine(y);
        const std::uint8_t* srow = seen.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x)
            if (srow[x]) dst[x] = src[x];
    }
    return out;
}

// Keep only the 8-connected >127 component of `mask` containing `seed`;
// everything else becomes 0 (the input's own values survive inside the
// component). Returns `mask` unchanged when the seed itself is not
// selected. Used so "click this person" selects that person even when
// the full-frame mask also covers others (or a bigger stray blob).
QImage selectionComponentAt(const QImage& mask, const QPoint& seed) {
    const int w = mask.width(), h = mask.height();
    if (seed.x() < 0 || seed.y() < 0 || seed.x() >= w || seed.y() >= h)
        return mask;
    const int stride = mask.bytesPerLine();
    const uchar* bits = mask.constBits();
    if (bits[std::size_t(seed.y()) * stride + seed.x()] <= 127) return mask;
    std::vector<std::uint8_t> seen(std::size_t(w) * h, 0);
    const auto seenAt = [&](int x, int y) -> std::uint8_t& {
        return seen[std::size_t(y) * w + x];
    };
    std::vector<int> stack;
    stack.reserve(1 << 16);
    stack.push_back(seed.y() * w + seed.x());
    seenAt(seed.x(), seed.y()) = 1;
    while (!stack.empty()) {
        const int idx = stack.back();
        stack.pop_back();
        const int x = idx % w, y = idx / w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                if (seenAt(nx, ny)) continue;
                if (bits[std::size_t(ny) * stride + nx] <= 127) continue;
                seenAt(nx, ny) = 1;
                stack.push_back(ny * w + nx);
            }
    }
    QImage out(mask.size(), QImage::Format_Grayscale8);
    out.fill(0);
    for (int y = 0; y < h; ++y) {
        const uchar* src = bits + std::size_t(y) * stride;
        uchar* dst = out.scanLine(y);
        const std::uint8_t* srow = seen.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x)
            if (srow[x]) dst[x] = src[x];
    }
    return out;
}

// Element-wise max of two document-resolution coverage masks (union). `acc`
// grows to `m`'s size when empty; a size mismatch (shouldn't happen — both are
// document sized) restarts from `m`.
void unionMaskInto(QImage& acc, const QImage& m) {
    if (m.isNull()) return;
    if (acc.isNull() || acc.size() != m.size()) {
        acc = QImage(m.size(), QImage::Format_Grayscale8);
        if (acc.isNull()) return;
        acc.fill(0);
    }
    for (int y = 0; y < acc.height(); ++y) {
        const uchar* s = m.constScanLine(y);
        uchar* o = acc.scanLine(y);
        for (int x = 0; x < acc.width(); ++x)
            o[x] = std::max(o[x], s[x]);
    }
}


// PSD/PSB, SVG and .af archives (.af/.afphoto/.afdesign/.afpub) are decoded by
// Pittore's own importers (AppState::openImageFile → openPsdLayers /
// openSvgParts / the built-in .af codec), not by Qt's QImageReader, so a
// system without those plugins would otherwise reject the drag entirely.
bool isCodecImportSuffix(const QByteArray& suffix) {
    return suffix == "psd" || suffix == "psb" || suffix == "svg" ||
           suffix == "af" || suffix == "afphoto" || suffix == "afdesign" ||
           suffix == "afpub";
}

bool dropMimeHasImage(const QMimeData* mime) {
    if (!mime) return false;
    if (mime->hasImage()) return true;
    if (!mime->hasUrls()) return false;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QByteArray suffix =
            QFileInfo(url.toLocalFile()).suffix().toLower().toLatin1();
        if (isCodecImportSuffix(suffix)) return true;
        if (QImageReader::supportedImageFormats().contains(suffix)) return true;
    }
    return false;
}


CanvasRuler::CanvasRuler(CanvasView* view, Qt::Orientation orientation, QWidget* parent)
    : QWidget(parent), view_(view), orientation_(orientation) {
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setCursor(orientation == Qt::Horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
}

void CanvasRuler::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    view_->paintRuler(p, orientation_, rect());
}


void CanvasRuler::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        view_->beginRulerGuide(orientation_, event->position());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}


void CanvasRuler::mouseMoveEvent(QMouseEvent* event) {
    view_->updateRulerGuide(orientation_, event->position());
    event->accept();
}


void CanvasRuler::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        view_->finishRulerGuide(true);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}


// --- Freehand / polygonal lasso + selection brush ---------------------------
// Clean-room selection gestures: freehand loops and click-click polygons
// rasterise to a coverage mask (even-odd, AA, feather) and combine through
// the tool's selmode. Magnetic snapping seeks the strongest local edge.

int CanvasView::lassoSelMode(Qt::KeyboardModifiers mods) const {
    const ToolId tool = state_->activeTool();
    int mode = state_->option(tool, QStringLiteral("selmode")).toInt();
    mode = std::clamp(mode, 0, 3);
    if (mods.testFlag(Qt::ShiftModifier)) mode = 1;
    else if (mods.testFlag(Qt::AltModifier)) mode = 2;
    return mode;
}

void CanvasView::lassoCommitPolygon(const std::vector<QPointF>& loop,
                                   Qt::KeyboardModifiers mods) {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty()) return;
    if (loop.size() < 3) return;
    const ToolId tool = state_->activeTool();
    const int feather =
        std::clamp(state_->option(tool, QStringLiteral("feather")).toInt(),
                   0, 1000);
    bool antialias = true;
    const QVariant aa = state_->option(tool, QStringLiteral("antialias"));
    if (aa.isValid()) antialias = aa.toBool();
    const int mode = lassoSelMode(mods);
    std::vector<QPointF> pts = loop;
    QImage mask = selectionMaskFromPolygon(pts, d->size, antialias, feather);
    if (mask.isNull() || selectionMaskBbox(mask).isEmpty()) {
        if (mode == 0) {
            state_->clearSelection();
            state_->pushHistory(toolName(tool),
                                QString::fromUtf8(toolDef(tool).iconKey));
        }
        return;
    }
    state_->combineSelection(std::move(mask), mode, toolName(tool),
                             QString::fromUtf8(toolDef(tool).iconKey));
}

QPointF CanvasView::magneticSnap(const QPointF& docPos) const {
    DocumentItem* d = doc();
    if (!d || d->size.isEmpty() || d->composite.isNull()) return docPos;
    const ToolId tool = state_->activeTool();
    const int width = std::clamp(
        state_->option(tool, QStringLiteral("width")).toInt(), 1, 256);
    const int contrast = std::clamp(
        state_->option(tool, QStringLiteral("contrast")).toInt(), 1, 100);
    const QImage& comp = d->composite;
    const int cw = comp.width(), ch = comp.height();
    const int cx = std::clamp(int(std::floor(docPos.x())), 0, cw - 1);
    const int cy = std::clamp(int(std::floor(docPos.y())), 0, ch - 1);
    QImage src = comp;
    if (src.format() != QImage::Format_ARGB32 &&
        src.format() != QImage::Format_ARGB32_Premultiplied &&
        src.format() != QImage::Format_RGB32)
        src = src.convertToFormat(QImage::Format_ARGB32);
    auto lumaAt = [&](int x, int y) -> double {
        x = std::clamp(x, 0, cw - 1);
        y = std::clamp(y, 0, ch - 1);
        const QRgb c =
            reinterpret_cast<const QRgb*>(src.constScanLine(y))[x];
        return 0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c);
    };
    const double thresh = 2.0 + (contrast / 100.0) * 28.0;
    double best = thresh;
    QPointF bestPt = docPos;
    const int r = width;
    for (int y = std::max(0, cy - r); y <= std::min(ch - 1, cy + r); ++y) {
        for (int x = std::max(0, cx - r); x <= std::min(cw - 1, cx + r); ++x) {
            const double gx =
                (lumaAt(x + 1, y) - lumaAt(x - 1, y)) * 0.5;
            const double gy =
                (lumaAt(x, y + 1) - lumaAt(x, y - 1)) * 0.5;
            const double g = std::hypot(gx, gy);
            if (g <= best) continue;
            const double dist = std::hypot(double(x - cx), double(y - cy));
            if (dist > r + 1e-9) continue;
            best = g;
            bestPt = QPointF(x + 0.5, y + 0.5);
        }
    }
    return bestPt;
}

void CanvasView::selBrushPaintTo(const QPointF& docPos, double radius,
                                 double hardness, double opacity01) {
    if (selBrushMask_.isNull()) return;
    const int w = selBrushMask_.width(), h = selBrushMask_.height();
    if (w < 1 || h < 1 || radius <= 0.0) return;
    const double op = std::clamp(opacity01, 0.0, 1.0);
    if (op <= 0.0) return;
    const double hard = std::clamp(hardness, 0.0, 1.0);
    const double inner = radius * hard;
    const double span = std::max(1e-6, radius - inner);
    // Gap-fill from the last dab so fast drags never dot. Spacing is % of
    // the tip diameter (default 25 reproduces a 0.5-radius step).
    const ToolId tool = state_->activeTool();
    double spacingPct =
        state_->option(tool, QStringLiteral("spacing")).toDouble();
    if (!(spacingPct >= 1.0)) spacingPct = 25.0;
    spacingPct = std::clamp(spacingPct, 1.0, 1000.0);
    const double dist = QLineF(selBrushLast_, docPos).length();
    const double spacingPx =
        std::max(1.0, 2.0 * radius * spacingPct / 100.0);
    const int steps = std::max(1, int(std::ceil(dist / spacingPx)));
    for (int s = 0; s <= steps; ++s) {
        const double t = steps == 0 ? 1.0 : double(s) / double(steps);
        const QPointF p = selBrushLast_ + (docPos - selBrushLast_) * t;
        const int x0 = std::max(0, int(std::floor(p.x() - radius - 1.0)));
        const int x1 = std::min(w - 1, int(std::ceil(p.x() + radius + 1.0)));
        const int y0 = std::max(0, int(std::floor(p.y() - radius - 1.0)));
        const int y1 = std::min(h - 1, int(std::ceil(p.y() + radius + 1.0)));
        for (int y = y0; y <= y1; ++y) {
            uchar* row = selBrushMask_.scanLine(y);
            for (int x = x0; x <= x1; ++x) {
                const double dx = x + 0.5 - p.x();
                const double dy = y + 0.5 - p.y();
                const double dd = std::hypot(dx, dy);
                if (dd > radius) continue;
                const double fall = dd <= inner
                                        ? 1.0
                                        : std::clamp((radius - dd) / span,
                                                     0.0, 1.0);
                const double k = std::clamp(fall * op, 0.0, 1.0);
                if (k <= 0.0) continue;
                row[x] = static_cast<uchar>(std::clamp(
                    int(255.0 * k + row[x] * (1.0 - k) + 0.5), 0, 255));
            }
        }
    }
    selBrushLast_ = docPos;
}

QPainterPath CanvasView::lassoLivePath() const {
    QPainterPath path;
    if (lassoStroke_.size() < 2) return path;
    path.moveTo(lassoStroke_.front());
    for (std::size_t i = 1; i < lassoStroke_.size(); ++i)
        path.lineTo(lassoStroke_[i]);
    path.closeSubpath();
    return path;
}

QPainterPath CanvasView::polyLivePath() const {
    QPainterPath path;
    if (polyPts_.empty()) return path;
    path.moveTo(polyPts_.front());
    for (std::size_t i = 1; i < polyPts_.size(); ++i)
        path.lineTo(polyPts_[i]);
    if (polyHoverOn_) path.lineTo(polyHover_);
    return path;
}

}  // namespace pittore::ui
