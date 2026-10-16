// Marquee/elliptical/arbitrary-shape selection: setting, masking, combining
// and inverting. Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/selection_mask.h"

#include <algorithm>
#include <utility>

namespace pittore::ui {

void AppState::setSelection(const QRectF& rect, bool ellipse) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    d->selection = rect;
    d->selectionIsEllipse = ellipse;
    // A rect/ellipse (marquee) selection replaces any arbitrary-shape mask.
    d->selectionIsMask = false;
    d->selectionMask = QImage();
    emit selectionChanged();
    if (!rect.isEmpty() && context_ != TaskContext::Crop && context_ != TaskContext::Text)
        setTaskContext(TaskContext::Selection);
    else if (rect.isEmpty() && context_ == TaskContext::Selection)
        setTaskContext(TaskContext::None);
}

// Bounding box of a grayscale mask at the standard 50% (127) threshold, in
// pixel coordinates; empty (maxX < 0) when nothing is selected.
QRect maskBbox(const QImage& mask) {
    if (mask.isNull()) return QRect();
    const int w = mask.width(), h = mask.height();
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        const uchar* row = mask.constScanLine(y);  // honours padded stride
        for (int x = 0; x < w; ++x) {
            if (row[x] > 127) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    return maxX < 0 ? QRect() : QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

void AppState::setSelectionMask(QImage mask) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    if (mask.isNull() || mask.width() != d->size.width() ||
        mask.height() != d->size.height()) {
        clearSelection();
        return;
    }
    if (mask.format() != QImage::Format_Grayscale8)
        mask = mask.convertToFormat(QImage::Format_Grayscale8);
    const QRect bbox = maskBbox(mask);
    if (bbox.isEmpty()) {
        clearSelection();
        return;
    }
    d->selectionMask = std::move(mask);
    d->selectionIsMask = true;
    d->selectionIsEllipse = false;
    // Pixel (x,y) of the mask is document pixel (x,y): the bbox is the
    // selection rect bbox-consumers rely on.
    d->selection = QRectF(bbox);
    ++d->selectionStamp;
    emit selectionChanged();
    if (context_ != TaskContext::Crop && context_ != TaskContext::Text)
        setTaskContext(TaskContext::Selection);
}

void AppState::clearSelection() { setSelection(QRectF(), false); }

void AppState::replaceSelectionMask(QImage mask, const QString& undoName,
                                    const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d) return;
    d->beginUndoAction();
    setSelectionMask(std::move(mask));
    d->commitUndoAction(undoName, iconKey);
    emit historyChanged();
    emit documentModified(d);
}

bool AppState::selectFromLayerAlpha(int layerIndex) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty() || layerIndex < 0 ||
        layerIndex >= d->layers.size())
        return false;
    const LayerItem& l = d->layers[layerIndex];

    // Sources whose alpha makes up the selection: the pixel layer itself, or
    // the visible pixel descendants of a group, bottom→top (the composite
    // order) so Ctrl+click on a group row outlines the whole assembled logo.
    QVector<int> sources;
    if (l.kind == LayerItem::Kind::Pixel) {
        sources.append(layerIndex);
    } else if (l.kind == LayerItem::Kind::Group) {
        sources = groupPixelDescendantIndices(layerIndex);
        if (sources.isEmpty()) return false;
        std::sort(sources.begin(), sources.end(),
                  [](int a, int b) { return a > b; });
    } else {
        return false;   // text/adjustment rows have no alpha to extract
    }

    const int dw = d->size.width(), dh = d->size.height();
    QImage mask(dw, dh, QImage::Format_Grayscale8);
    mask.fill(0);
    if (mask.isNull()) return false;

    // Each source's alpha is placed into document space with the same
    // nearest-neighbour lookup the AI/wand rasterisers use — but treating
    // out-of-footprint pixels as transparent (0), never clamping to the source
    // edge, so the mask is the layer's exact footprint. Iterating only each
    // source's own document footprint keeps the cost O(source pixels), not
    // O(doc × layers). For a single pixel layer this is a 1:1 alpha copy.
    for (int idx : sources) {
        const LayerItem& c = d->layers[idx];
        if (!c.visible || !c.pixels) continue;
        const int pw = static_cast<int>(c.pixels->width());
        const int ph = static_cast<int>(c.pixels->height());
        if (pw < 1 || ph < 1) continue;
        const double sx = std::max(c.scaleX, 1e-6);
        const double sy = std::max(c.scaleY, 1e-6);
        QRect foot = QRectF(c.offset, QSizeF(pw * sx, ph * sy))
                         .toAlignedRect()
                         .intersected(QRect(0, 0, dw, dh));
        if (foot.isEmpty()) continue;
        const pittore::RGBAf* src = c.pixels->data();
        for (int y = foot.top(); y <= foot.bottom(); ++y) {
            uchar* row = mask.scanLine(y);
            for (int x = foot.left(); x <= foot.right(); ++x) {
                const int lx = std::clamp(
                    static_cast<int>(std::lround((x + 0.5 - c.offset.x()) /
                                                 sx - 0.5)),
                    0, pw - 1);
                const int ly = std::clamp(
                    static_cast<int>(std::lround((y + 0.5 - c.offset.y()) /
                                                 sy - 0.5)),
                    0, ph - 1);
                const float a = src[std::size_t(ly) * pw + lx].a;
                // Source-over union for groups; single layers just copy their
                // alpha (first paint over the cleared mask).
                const float cur = row[x] / 255.0f;
                row[x] = static_cast<uchar>(std::clamp(
                    static_cast<int>((a + cur * (1.0f - a)) * 255.0f + 0.5f),
                    0, 255));
            }
        }
    }

    if (maskBbox(mask).isEmpty()) {
        setStatusHint(tr("Select Transparency: layer is fully transparent."));
        return false;
    }
    replaceSelectionMask(std::move(mask), tr("Select Transparency"),
                         QStringLiteral("select"));
    setStatusHint(tr("Selection loaded from \"%1\".")
                      .arg(d->layers[layerIndex].name));
    return true;
}

// The live selection as a document-resolution coverage mask: the stored mask
// when one exists, otherwise the rect/ellipse rasterised. Always returns a
// document-sized Grayscale8 image (all-zero when nothing is selected).
QImage selectionAsMask(const DocumentItem& d) {
    QImage m(d.size, QImage::Format_Grayscale8);
    if (m.isNull()) return m;
    m.fill(0);
    if (d.selection.isEmpty()) return m;
    if (d.selectionIsMask && !d.selectionMask.isNull()) {
        if (d.selectionMask.size() == m.size()) return d.selectionMask;
        return d.selectionMask.scaled(m.size(), Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_Grayscale8);
    }
    const QRectF sel = d.selection.normalized();
    const bool ellipse = d.selectionIsEllipse;
    const int w = m.width(), h = m.height();
    const double cx = sel.center().x(), cy = sel.center().y();
    const double rx = std::max(sel.width() * 0.5, 1e-9);
    const double ry = std::max(sel.height() * 0.5, 1e-9);
    for (int y = 0; y < h; ++y) {
        uchar* row = m.scanLine(y);
        for (int x = 0; x < w; ++x) {
            bool inside;
            if (ellipse) {
                const double nx = (x + 0.5 - cx) / rx;
                const double ny = (y + 0.5 - cy) / ry;
                inside = nx * nx + ny * ny <= 1.0;
            } else {
                inside = sel.contains(QPointF(x + 0.5, y + 0.5));
            }
            if (inside) row[x] = 255;
        }
    }
    return m;
}

void AppState::invertSelection() {
    DocumentItem* d = activeDocument();
    if (!d || d->selection.isEmpty() || d->size.isEmpty()) return;
    const QImage base = selectionAsMask(*d);
    if (base.isNull()) return;
    QImage inv(base.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < base.height(); ++y) {
        const uchar* s = base.constScanLine(y);
        uchar* o = inv.scanLine(y);
        for (int x = 0; x < base.width(); ++x) o[x] = uchar(255 - s[x]);
    }
    replaceSelectionMask(std::move(inv), tr("Inverse"),
                         QStringLiteral("marquee-rect"));
}

void AppState::combineSelection(QImage incoming, int mode,
                                const QString& undoName,
                                const QString& iconKey) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return;
    if (incoming.isNull() || incoming.size() != d->size) {
        if (mode == 0) replaceSelectionMask(QImage(), undoName, iconKey);
        return;
    }
    if (incoming.format() != QImage::Format_Grayscale8)
        incoming = incoming.convertToFormat(QImage::Format_Grayscale8);
    if (mode == 0) {
        replaceSelectionMask(std::move(incoming), undoName, iconKey);
        return;
    }
    const QImage base = selectionAsMask(*d);
    QImage out(base.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < base.height(); ++y) {
        const uchar* a = base.constScanLine(y);
        const uchar* b = incoming.constScanLine(y);
        uchar* o = out.scanLine(y);
        for (int x = 0; x < base.width(); ++x) {
            const int av = a[x], bv = b[x];
            int v;
            switch (mode) {
                case 1: v = std::max(av, bv); break;          // add / union
                case 2: v = (av * (255 - bv)) / 255; break;   // subtract
                case 3: v = std::min(av, bv); break;          // intersect
                default: v = bv; break;
            }
            o[x] = uchar(std::clamp(v, 0, 255));
        }
    }
    replaceSelectionMask(std::move(out), undoName, iconKey);
}


bool AppState::magicWandSelectAt(const QPointF& docPos, int modeOverride) {
    const ToolId tool = activeTool();
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || layer->kind != LayerItem::Kind::Pixel || !layer->pixels) {
        setStatusHint(tr("Magic Wand: select a pixel layer first."));
        return false;
    }

    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const int lw = static_cast<int>(layer->pixels->width());
    const int lh = static_cast<int>(layer->pixels->height());
    const int sx = static_cast<int>(
        std::floor((docPos.x() - layer->offset.x()) / lsx));
    const int sy = static_cast<int>(
        std::floor((docPos.y() - layer->offset.y()) / lsy));
    if (lw <= 0 || lh <= 0 || sx < 0 || sy < 0 || sx >= lw || sy >= lh) {
        setStatusHint(tr("Magic Wand: click a pixel on the active layer."));
        return false;
    }

    // Colour the region is measured in: the active layer's own straight-alpha
    // pixels, or (Sample All Layers) the document composite read at each layer
    // texel's document position — the same buffer Paint Bucket floods.
    std::vector<pittore::RGBAf> matchBuf;
    const pittore::RGBAf* match = layer->pixels->data();
    if (option(tool, QStringLiteral("sample_all")).toBool() &&
        !d->composite.isNull()) {
        QImage comp = d->composite;
        if (comp.format() != QImage::Format_ARGB32_Premultiplied &&
            comp.format() != QImage::Format_ARGB32)
            comp = comp.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        matchBuf.resize(std::size_t(lw) * std::size_t(lh));
        for (int y = 0; y < lh; ++y) {
            const double docY = layer->offset.y() + (double(y) + 0.5) * lsy;
            const int py = std::clamp(static_cast<int>(std::floor(docY)), 0,
                                      comp.height() - 1);
            const QRgb* row =
                reinterpret_cast<const QRgb*>(comp.constScanLine(py));
            for (int x = 0; x < lw; ++x) {
                const double docX =
                    layer->offset.x() + (double(x) + 0.5) * lsx;
                const int px = std::clamp(static_cast<int>(std::floor(docX)), 0,
                                          comp.width() - 1);
                const QRgb c = row[px];
                const int a = qAlpha(c);
                pittore::RGBAf& dst = matchBuf[std::size_t(y) * lw + x];
                if (a <= 0) {
                    dst = pittore::RGBAf{0, 0, 0, 0};
                } else {
                    const float inv = 1.0f / static_cast<float>(a);
                    dst = pittore::RGBAf{qRed(c) * inv, qGreen(c) * inv,
                                          qBlue(c) * inv,
                                          static_cast<float>(a) / 255.0f};
                }
            }
        }
        match = matchBuf.data();
    }

    const float tol = static_cast<float>(std::clamp(
        option(tool, QStringLiteral("tolerance")).toDouble() / 255.0, 0.0, 1.0));
    const bool contiguous = option(tool, QStringLiteral("contiguous")).toBool();
    const bool antialias = option(tool, QStringLiteral("antialias")).toBool();

    // Reference colour: the average over the Sample Size window (Point Sample
    // reads the single pixel). Averaging keeps a noisy seed inside its region.
    static const int kSampleSizes[] = {1, 3, 5, 11, 31, 51, 101};
    const int si =
        std::clamp(option(tool, QStringLiteral("sample_size")).toInt(), 0, 6);
    const int half = kSampleSizes[si] / 2;
    double ar = 0, ag = 0, ab = 0, aa = 0;
    int cnt = 0;
    for (int y = std::max(0, sy - half); y <= std::min(lh - 1, sy + half); ++y)
        for (int x = std::max(0, sx - half); x <= std::min(lw - 1, sx + half); ++x) {
            const pittore::RGBAf& c = match[std::size_t(y) * lw + x];
            ar += c.r;
            ag += c.g;
            ab += c.b;
            aa += c.a;
            ++cnt;
        }
    if (cnt <= 0) return false;
    const pittore::RGBAf seed{static_cast<float>(ar / cnt),
                               static_cast<float>(ag / cnt),
                               static_cast<float>(ab / cnt),
                               static_cast<float>(aa / cnt)};
    // Same per-channel max metric as the Paint Bucket's flood fill.
    const auto within = [&](const pittore::RGBAf& c) {
        return std::max(std::max(std::fabs(c.r - seed.r), std::fabs(c.g - seed.g)),
                        std::max(std::fabs(c.b - seed.b),
                                 std::fabs(c.a - seed.a))) <= tol;
    };

    const std::size_t n = std::size_t(lw) * std::size_t(lh);
    std::vector<std::uint8_t> region(n, 0);
    const std::size_t seedIdx = std::size_t(sy) * lw + sx;
    if (contiguous) {
        // Iterative 4-connected flood; a vector is the stack so the depth stays
        // heap-bounded (mirrors flood_fill_host).
        std::vector<int> stack;
        stack.reserve(1024);
        region[seedIdx] = 1;
        stack.push_back(static_cast<int>(seedIdx));
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int x = idx % lw, y = idx / lw;
            const auto tryPush = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= lw || ny >= lh) return;
                const std::size_t ni = std::size_t(ny) * lw + nx;
                if (region[ni]) return;
                if (within(match[ni])) {
                    region[ni] = 1;
                    stack.push_back(static_cast<int>(ni));
                }
            };
            tryPush(x - 1, y);
            tryPush(x + 1, y);
            tryPush(x, y - 1);
            tryPush(x, y + 1);
        }
    } else {
        for (std::size_t i = 0; i < n; ++i)
            if (within(match[i])) region[i] = 1;
    }

    // Layer-space coverage: 1 inside the region, 0 outside. With Anti-alias a
    // 3x3 box feathers the boundary by one pixel — exactly the Paint Bucket's
    // edge treatment.
    std::vector<float> cov(n, 0.0f);
    if (!antialias) {
        for (std::size_t i = 0; i < n; ++i) cov[i] = region[i] ? 1.0f : 0.0f;
    } else {
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                float acc = 0.0f;
                int valid = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int cx = x + dx, cy = y + dy;
                        if (cx < 0 || cy < 0 || cx >= lw || cy >= lh) continue;
                        ++valid;
                        acc += region[std::size_t(cy) * lw + cx] ? 1.0f : 0.0f;
                    }
                cov[std::size_t(y) * lw + x] =
                    valid > 0 ? acc / static_cast<float>(valid) : 0.0f;
            }
    }

    QImage mask = pittore::ui::selectionMaskFromLayerAlpha(
        cov.data(), lw, lh, layer->offset, layer->scaleX, layer->scaleY,
        d->size);
    if (pittore::ui::selectionMaskBbox(mask).isEmpty()) {
        setStatusHint(tr("Magic Wand: nothing within tolerance here."));
        return false;
    }
    const int mode = modeOverride >= 0
                         ? modeOverride
                         : option(tool, QStringLiteral("selmode")).toInt();
    combineSelection(std::move(mask), mode, tr("Magic Wand"),
                     QStringLiteral("wand"));
    setStatusHint(tr("Magic Wand: selected by colour."));
    return true;
}

bool AppState::traceSelection(int output, int detail) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    if (d->selection.isEmpty() && !d->selectionIsMask) {
        setStatusHint(tr("Tracing: select an area first."));
        return false;
    }
    if (std::clamp(output, 0, 2) == 0) {
        setStatusHint(tr("Tracing: work-path storage is planned."));
        return false;
    }
    // Source polygon in document pixels: rect corners, a 64-gon ellipse,
    // or the largest marching-squares loop of the mask channel.
    std::vector<QPointF> poly;
    if (d->selectionIsMask && !d->selectionMask.isNull()) {
        const QPainterPath outline = pittore::ui::selectionOutlineFromMask(
            d->selectionMask,
            d->selection.toAlignedRect().adjusted(-1, -1, 1, 1));
        double bestArea = -1.0;
        for (const QPolygonF& sub : outline.toSubpathPolygons()) {
            if (sub.size() < 3) continue;
            const QRectF bb = sub.boundingRect();
            const double area = bb.width() * bb.height();
            if (area > bestArea) {
                bestArea = area;
                poly.assign(sub.begin(), sub.end());
            }
        }
    } else if (d->selectionIsEllipse) {
        const QPointF ctr = d->selection.center();
        const double rx = d->selection.width() / 2.0;
        const double ry = d->selection.height() / 2.0;
        if (rx > 0.0 && ry > 0.0) {
            for (int i = 0; i < 64; ++i) {
                const double a = 2.0 * 3.14159265358979323846 * i / 64.0;
                poly.emplace_back(ctr.x() + rx * std::cos(a),
                                  ctr.y() + ry * std::sin(a));
            }
        }
    } else {
        const QRectF& r = d->selection;
        poly = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    }
    if (poly.size() < 3) {
        setStatusHint(tr("Tracing: nothing traceable selected."));
        return false;
    }
    // Douglas-Peucker simplification, explicit stack (no recursion depth).
    // Epsilon scales with (100 - detail): 100 keeps every vertex.
    const double eps = (100.0 - std::clamp(detail, 0, 100)) * 0.05;
    if (eps > 1e-9 && poly.size() > 4) {
        std::vector<char> keep(poly.size(), 0);
        keep.front() = 1;
        keep.back() = 1;
        std::vector<std::pair<int, int>> stack{{0, int(poly.size()) - 1}};
        while (!stack.empty()) {
            const auto [s, e] = stack.back();
            stack.pop_back();
            const QPointF a = poly[std::size_t(s)], b = poly[std::size_t(e)];
            const double dx = b.x() - a.x(), dy = b.y() - a.y();
            const double len = std::hypot(dx, dy);
            double dmax = -1.0;
            int imax = -1;
            for (int i = s + 1; i < e; ++i) {
                const double dist =
                    len > 1e-12
                        ? std::fabs((poly[std::size_t(i)].x() - a.x()) * dy -
                                    (poly[std::size_t(i)].y() - a.y()) * dx) /
                              len
                        : std::hypot(poly[std::size_t(i)].x() - a.x(),
                                     poly[std::size_t(i)].y() - a.y());
                if (dist > dmax) {
                    dmax = dist;
                    imax = i;
                }
            }
            if (imax >= 0 && dmax > eps) {
                keep[std::size_t(imax)] = 1;
                stack.emplace_back(s, imax);
                stack.emplace_back(imax, e);
            }
        }
        std::vector<QPointF> simp;
        for (std::size_t i = 0; i < poly.size(); ++i)
            if (keep[i]) simp.push_back(poly[i]);
        if (simp.size() >= 3) poly = std::move(simp);
    }
    const bool closePath =
        option(ToolId::ContentAwareTracing, QStringLiteral("close_path"))
            .toBool();
    if (output == 2) {
        // Polygon vector layer (fill only; stroke comes from the bar).
        auto node = std::make_shared<pittore::vector::ArtNode>();
        node->name = "Trace";
        bool first = true;
        for (const QPointF& p : poly) {
            pittore::vector::Segment s;
            s.kind = first ? pittore::vector::Segment::Kind::MoveTo
                           : pittore::vector::Segment::Kind::LineTo;
            s.x = float(p.x());
            s.y = float(p.y());
            node->segments.push_back(s);
            first = false;
        }
        if (closePath) {
            pittore::vector::Segment c;
            c.kind = pittore::vector::Segment::Kind::Close;
            node->segments.push_back(c);
        }
        const QColor fg = foreground();
        node->paint.hasFill = true;
        node->paint.fill[0] = std::uint8_t(fg.red());
        node->paint.fill[1] = std::uint8_t(fg.green());
        node->paint.fill[2] = std::uint8_t(fg.blue());
        node->paint.fill[3] = 255;
        double bx0 = poly.front().x(), by0 = poly.front().y();
        double bx1 = bx0, by1 = by0;
        for (const QPointF& p : poly) {
            bx0 = std::min(bx0, p.x());
            by0 = std::min(by0, p.y());
            bx1 = std::max(bx1, p.x());
            by1 = std::max(by1, p.y());
        }
        return commitArtNodeLayer(
            std::move(node), QRectF(QPointF(bx0, by0), QPointF(bx1, by1)),
            ToolId::ContentAwareTracing, tr("Trace"), true,
            QStringLiteral("Normal"));
    }
    // Output 1: even-odd fill of the (implicitly closed) polygon back
    // into a document mask.
    QImage mask(QSize(d->size.width(), d->size.height()),
                QImage::Format_Grayscale8);
    mask.fill(0);
    double bx0 = poly.front().x(), by0 = poly.front().y();
    double bx1 = bx0, by1 = by0;
    for (const QPointF& p : poly) {
        bx0 = std::min(bx0, p.x());
        by0 = std::min(by0, p.y());
        bx1 = std::max(bx1, p.x());
        by1 = std::max(by1, p.y());
    }
    const int ix0 = std::max(0, int(std::floor(bx0)));
    const int iy0 = std::max(0, int(std::floor(by0)));
    const int ix1 = std::min(mask.width(), int(std::ceil(bx1)));
    const int iy1 = std::min(mask.height(), int(std::ceil(by1)));
    auto inside = [&](double x, double y) {
        bool in = false;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size();
             j = i++) {
            const QPointF& a = poly[i];
            const QPointF& b = poly[j];
            if ((a.y() > y) != (b.y() > y) &&
                x < (b.x() - a.x()) * (y - a.y()) / (b.y() - a.y()) + a.x())
                in = !in;
        }
        return in;
    };
    for (int y = iy0; y < iy1; ++y) {
        uchar* row = mask.scanLine(y);
        for (int x = ix0; x < ix1; ++x)
            if (inside(x + 0.5, y + 0.5)) row[x] = 255;
    }
    replaceSelectionMask(std::move(mask), tr("Trace"),
                         QStringLiteral("trace"));
    return true;
}

}  // namespace pittore::ui
