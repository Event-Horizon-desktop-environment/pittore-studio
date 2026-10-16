// Canvas geometry: paper, resize, perspective crop, layer rotation.
//
// Split out of app_state.cpp; DocumentItem/AppState members, no extra
// declarations needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"

#include "engine/compute/layer_mask.h"

#include <QImage>
#include <QTransform>

#include <algorithm>
#include <cmath>
namespace pittore::ui {

// Rotate opaque-grey mask coverage through the same Qt transform a pixel
// rotation uses, so a linked mask keeps its orientation as well as its size.
std::shared_ptr<pittore::Image> rotatedMaskImage(const pittore::Image& mask,
                                                  const QTransform& t) {
    const int mw = static_cast<int>(mask.width());
    const int mh = static_cast<int>(mask.height());
    if (mw <= 0 || mh <= 0) return nullptr;
    QImage gray(mw, mh, QImage::Format_Grayscale8);
    if (gray.isNull()) return nullptr;
    for (int y = 0; y < mh; ++y) {
        uchar* row = gray.scanLine(y);
        for (int x = 0; x < mw; ++x) {
            const float c = std::clamp(
                mask.data()[static_cast<std::size_t>(y) * mw + x].r, 0.0f,
                1.0f);
            row[x] = static_cast<uchar>(
                qBound(0, static_cast<int>(std::lround(c * 255.0f)), 255));
        }
    }
    const QImage rotated = gray.transformed(t, Qt::SmoothTransformation);
    if (rotated.isNull()) return nullptr;
    const QImage conv = rotated.convertToFormat(QImage::Format_Grayscale8);
    if (conv.isNull()) return nullptr;
    auto out = std::make_shared<pittore::Image>(
        static_cast<std::uint32_t>(conv.width()),
        static_cast<std::uint32_t>(conv.height()));
    for (int y = 0; y < conv.height(); ++y) {
        const uchar* row = conv.constScanLine(y);
        pittore::RGBAf* dst =
            out->data() + static_cast<std::size_t>(y) * conv.width();
        for (int x = 0; x < conv.width(); ++x)
            dst[x] = pittore::compute::make_mask_pixel(row[x] / 255.0f);
    }
    return out;
}

void AppState::deriveCanvasPaper(DocumentItem& doc) {
    doc.canvasPaper = QColor(0xf2, 0xf2, 0xf2);
    doc.canvasTransparent = false;
    if (doc.layers.isEmpty()) return;
    const LayerItem& bg = doc.layers.last();
    if (!bg.pixels || bg.pixels->width() == 0 || bg.pixels->height() == 0)
        return;
    const pittore::RGBAf c = bg.pixels->data()[0];
    if (c.a < 0.5f) {
        doc.canvasTransparent = true;
    } else if (c.r < 0.5f && c.g < 0.5f && c.b < 0.5f) {
        doc.canvasPaper = QColor(0, 0, 0);
    } else if (c.r > 0.85f && c.g > 0.85f && c.b > 0.85f &&
               std::fabs(c.r - c.g) < 0.03f &&
               std::fabs(c.r - c.b) < 0.03f &&
               std::fabs(c.g - c.b) < 0.03f) {
        // Achromatic light gray (including the historic canvas gray):
        // keep the exact value so erasing restores the file's own
        // background. Chromatic corners fall through to white (the
        // conventional background-color behavior for photos).
        const int v = std::clamp(int(std::lround(c.r * 255.0f)), 0, 255);
        const int w = std::clamp(int(std::lround(c.g * 255.0f)), 0, 255);
        const int u = std::clamp(int(std::lround(c.b * 255.0f)), 0, 255);
        doc.canvasPaper = QColor(v, w, u);
    } else {
        doc.canvasPaper = QColor(255, 255, 255);
    }
}

bool AppState::resizeCanvas(const QPointF& newTopLeftDoc, const QSize& newSize) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    if (newSize.width() < 1 || newSize.height() < 1) return false;
    for (LayerItem& l : d->layers) {
        l.offset -= newTopLeftDoc;
        if (l.mask && !l.maskLinked) l.maskOffset -= newTopLeftDoc;
        l.thumbnail = QImage();
    }
    // The selection lives in document space, so it rides the same origin
    // shift as the layers: after a crop the selection still outlines the same
    // pixels, not a stale absolute rect. Its mask (document-sized) is rebased
    // by the same shift, and anything the resize pushed off-canvas is dropped
    // — it can no longer be reached by any tool.
    if (!d->selection.isEmpty()) {
        const QSize oldSize = d->size;
        const QRectF moved =
            d->selection.translated(-newTopLeftDoc).intersected(
                QRectF(QPointF(0, 0), QSizeF(newSize)));
        if (d->selectionIsMask && !d->selectionMask.isNull() &&
            d->selectionMask.size() == oldSize) {
            QImage rebased(newSize, QImage::Format_Grayscale8);
            rebased.fill(0);
            const int sx = qRound(-newTopLeftDoc.x());
            const int sy = qRound(-newTopLeftDoc.y());
            for (int y = 0; y < newSize.height(); ++y) {
                const int srcY = y + sy;
                if (srcY < 0 || srcY >= oldSize.height()) continue;
                uchar* row = rebased.scanLine(y);
                const uchar* src = d->selectionMask.constScanLine(srcY);
                for (int x = 0; x < newSize.width(); ++x) {
                    const int srcX = x + sx;
                    row[x] = (srcX >= 0 && srcX < oldSize.width()) ? src[srcX] : 0;
                }
            }
            d->selectionMask = std::move(rebased);
        }
        d->selection = moved;
        if (moved.isEmpty()) {
            d->selectionIsEllipse = false;
            d->selectionIsMask = false;
            d->selectionMask = QImage();
        }
        ++d->selectionStamp;
    }
    d->size = newSize;
    d->rebuildComposite();
    emit layersChanged();
    emit documentModified(d);
    return true;
}

// 3x3 homography inverse (adjugate / determinant). False when singular.
static bool invertHomography3(const double h[3][3], double inv[3][3]) {
    const double det =
        h[0][0] * (h[1][1] * h[2][2] - h[1][2] * h[2][1]) -
        h[0][1] * (h[1][0] * h[2][2] - h[1][2] * h[2][0]) +
        h[0][2] * (h[1][0] * h[2][1] - h[1][1] * h[2][0]);
    if (std::fabs(det) <= 1e-12) return false;
    inv[0][0] = (h[1][1] * h[2][2] - h[1][2] * h[2][1]) / det;
    inv[0][1] = (h[0][2] * h[2][1] - h[0][1] * h[2][2]) / det;
    inv[0][2] = (h[0][1] * h[1][2] - h[0][2] * h[1][1]) / det;
    inv[1][0] = (h[1][2] * h[2][0] - h[1][0] * h[2][2]) / det;
    inv[1][1] = (h[0][0] * h[2][2] - h[0][2] * h[2][0]) / det;
    inv[1][2] = (h[0][2] * h[1][0] - h[0][0] * h[1][2]) / det;
    inv[2][0] = (h[1][0] * h[2][1] - h[1][1] * h[2][0]) / det;
    inv[2][1] = (h[0][1] * h[2][0] - h[0][0] * h[2][1]) / det;
    inv[2][2] = (h[0][0] * h[1][1] - h[0][1] * h[1][0]) / det;
    return true;
}

// Apply a homography to (u, v); identity fallback on w == 0.
static void applyHomography3(const double h[3][3], double u, double v,
                             double& sx, double& sy) {
    const double w = h[2][0] * u + h[2][1] * v + h[2][2];
    if (std::fabs(w) <= 1e-12) {
        sx = u;
        sy = v;
        return;
    }
    sx = (h[0][0] * u + h[0][1] * v + h[0][2]) / w;
    sy = (h[1][0] * u + h[1][1] * v + h[1][2]) / w;
}

bool AppState::perspectiveCrop(const QPointF quad[4], int outW, int outH) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    // Reject degenerate quads (zero area or bowtie): signed area must clear
    // a pixel.
    double area2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF& p = quad[i];
        const QPointF& q = quad[(i + 1) % 4];
        area2 += p.x() * q.y() - q.x() * p.y();
    }
    if (std::fabs(area2) < 2.0) {
        setStatusHint(tr("Perspective Crop: the quad is degenerate."));
        return false;
    }
    // Output size: explicit W/H win, else the quad bbox.
    double qx0 = quad[0].x(), qy0 = quad[0].y(), qx1 = quad[0].x(),
           qy1 = quad[0].y();
    for (int i = 1; i < 4; ++i) {
        qx0 = std::min(qx0, quad[i].x());
        qy0 = std::min(qy0, quad[i].y());
        qx1 = std::max(qx1, quad[i].x());
        qy1 = std::max(qy1, quad[i].y());
    }
    const int ow = outW > 0 ? outW : std::max(1, int(std::round(qx1 - qx0)));
    const int oh = outH > 0 ? outH : std::max(1, int(std::round(qy1 - qy0)));
    if (ow < 1 || oh < 1 || ow > 16000 || oh > 16000) {
        setStatusHint(tr("Perspective Crop: bad output size."));
        return false;
    }
    // Homography quad -> output rect, via DLT + partial-pivot Gaussian
    // elimination (double precision, deterministic).
    double H[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    {
        const double dst[4][2] = {{0, 0},
                                  {double(ow), 0},
                                  {double(ow), double(oh)},
                                  {0, double(oh)}};
        double m[8][9] = {};
        for (int i = 0; i < 4; ++i) {
            const double x = quad[i].x(), y = quad[i].y();
            const double u = dst[i][0], v = dst[i][1];
            m[2 * i][0] = x;
            m[2 * i][1] = y;
            m[2 * i][2] = 1;
            m[2 * i][6] = -u * x;
            m[2 * i][7] = -u * y;
            m[2 * i][8] = u;
            m[2 * i + 1][3] = x;
            m[2 * i + 1][4] = y;
            m[2 * i + 1][5] = 1;
            m[2 * i + 1][6] = -v * x;
            m[2 * i + 1][7] = -v * y;
            m[2 * i + 1][8] = v;
        }
        for (int c = 0; c < 8; ++c) {
            int piv = c;
            for (int r = c + 1; r < 8; ++r)
                if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
            if (std::fabs(m[piv][c]) < 1e-12) {
                setStatusHint(tr("Perspective Crop: the quad is degenerate."));
                return false;
            }
            if (piv != c)
                for (int k = c; k < 9; ++k) std::swap(m[c][k], m[piv][k]);
            const double div = m[c][c];
            for (int k = c; k < 9; ++k) m[c][k] /= div;
            for (int r = 0; r < 8; ++r) {
                if (r == c) continue;
                const double f = m[r][c];
                if (f == 0.0) continue;
                for (int k = c; k < 9; ++k) m[r][k] -= f * m[c][k];
            }
        }
        H[0][0] = m[0][8];
        H[0][1] = m[1][8];
        H[0][2] = m[2][8];
        H[1][0] = m[3][8];
        H[1][1] = m[4][8];
        H[1][2] = m[5][8];
        H[2][0] = m[6][8];
        H[2][1] = m[7][8];
        H[2][2] = 1.0;
    }
    auto sampleBilinear = [](const pittore::Image* img, double x, double y) {
        const std::uint32_t w = img->width(), h = img->height();
        const int x0 = std::clamp(int(std::floor(x)), 0, int(w) - 1);
        const int y0 = std::clamp(int(std::floor(y)), 0, int(h) - 1);
        const int x1 = std::min(x0 + 1, int(w) - 1);
        const int y1 = std::min(y0 + 1, int(h) - 1);
        const float fx = float(std::clamp(x - x0, 0.0, 1.0));
        const float fy = float(std::clamp(y - y0, 0.0, 1.0));
        const pittore::RGBAf& a = img->data()[std::size_t(y0) * w + x0];
        const pittore::RGBAf& b = img->data()[std::size_t(y0) * w + x1];
        const pittore::RGBAf& c = img->data()[std::size_t(y1) * w + x0];
        const pittore::RGBAf& e = img->data()[std::size_t(y1) * w + x1];
        pittore::RGBAf o;
        o.r = (a.r * (1 - fx) + b.r * fx) * (1 - fy) +
              (c.r * (1 - fx) + e.r * fx) * fy;
        o.g = (a.g * (1 - fx) + b.g * fx) * (1 - fy) +
              (c.g * (1 - fx) + e.g * fx) * fy;
        o.b = (a.b * (1 - fx) + b.b * fx) * (1 - fy) +
              (c.b * (1 - fx) + e.b * fx) * fy;
        o.a = (a.a * (1 - fx) + b.a * fx) * (1 - fy) +
              (c.a * (1 - fx) + e.a * fx) * fy;
        return o;
    };
    double invH[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (!invertHomography3(H, invH)) {
        setStatusHint(tr("Perspective Crop: the quad is degenerate."));
        return false;
    }
    beginUndoStep();
    bool vectorMoved = false;
    for (LayerItem& l : d->layers) {
        if (l.kind != LayerItem::Kind::Pixel || !l.pixels) {
            // Non-pixel art cannot warp through the homography: keep it
            // visible at the quad origin (hinted below).
            if ((l.art && !l.art->isEmpty()) || l.liveText) {
                l.offset = QPointF(0, 0);
                vectorMoved = true;
            }
            l.thumbnail = QImage();
            continue;
        }
        // Fresh output images: the undo snapshot keeps sharing the old
        // pixels untouched, so no copy-on-write dance is needed here.
        const double lsx = std::max(l.scaleX, 1e-6);
        const double lsy = std::max(l.scaleY, 1e-6);
        auto warped = std::make_shared<pittore::Image>(std::uint32_t(ow),
                                                        std::uint32_t(oh));
        // Inverse-map each output texel: output -> doc (inverse H) ->
        // layer px. Rows are disjoint: parallel-safe.
        pittore::core::parallel_rows(
            std::uint32_t(oh), [&](std::uint32_t y0, std::uint32_t y1) {
                for (std::uint32_t y = y0; y < y1; ++y) {
                    for (int x = 0; x < ow; ++x) {
                        const double u = x + 0.5, v = y + 0.5;
                        // Inverse-map the output texel (shared
                        // precomputed inverse).
                        double sx = 0.0, sy = 0.0;
                        applyHomography3(invH, u, v, sx, sy);
                        const double lx = (sx - l.offset.x()) / lsx;
                        const double ly = (sy - l.offset.y()) / lsy;
                        warped->data()[std::size_t(y) * ow + x] =
                            sampleBilinear(l.pixels.get(), lx - 0.5,
                                           ly - 0.5);
                    }
                }
            });
        l.pixels = std::move(warped);
        l.offset = QPointF(0, 0);
        l.scaleX = 1.0;
        l.scaleY = 1.0;
        if (l.mask && l.maskLinked) {
            // Linked masks ride the layer frame: same inverse map.
            auto wmask = std::make_shared<pittore::Image>(std::uint32_t(ow),
                                                           std::uint32_t(oh));
            pittore::core::parallel_rows(
                std::uint32_t(oh), [&](std::uint32_t y0, std::uint32_t y1) {
                    for (std::uint32_t y = y0; y < y1; ++y)
                        for (int x = 0; x < ow; ++x) {
                            const double u = x + 0.5, v = y + 0.5;
                            double sx = 0.0, sy = 0.0;
                            applyHomography3(invH, u, v, sx, sy);
                            const double lx =
                                (sx - l.offset.x()) / lsx - 0.5;
                            const double ly =
                                (sy - l.offset.y()) / lsy - 0.5;
                            wmask->data()[std::size_t(y) * ow + x] =
                                sampleBilinear(l.mask.get(), lx, ly);
                        }
                });
            l.mask = std::move(wmask);
        }
        // Unlinked masks keep their frozen placement (documented).
        l.thumbnail = QImage();
    }
    const int res =
        option(ToolId::PerspectiveCrop, QStringLiteral("res")).toInt();
    if (res > 0) d->dpi = res;
    resizeCanvas(QPointF(0, 0), QSize(ow, oh));
    commitUndoStep(tr("Perspective Crop"), QStringLiteral("crop"));
    if (vectorMoved)
        setStatusHint(tr("Perspective Crop: vector layers repositioned, "
                         "not warped."));
    else
        setStatusHint(tr("Perspective Crop applied."));
    return true;
}

bool AppState::rotateActiveLayer(double degrees) {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;
    // A rotation that is a no-op (or the 360° wrap) should not touch pixels.
    double deg = std::fmod(degrees, 360.0);
    if (std::fabs(deg) < 0.01 || std::fabs(deg) > 359.99) return false;

    beginUndoStep();
    copyOnWriteActiveLayer();
    // copyOnWriteActiveLayer detaches the layer vector, so the pointer taken
    // above may be stale: re-resolve before touching the pixels.
    layer = activeLayer();
    if (!layer) {
        discardUndoStep();
        return false;
    }
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels) {
        discardUndoStep();
        return false;
    }

    const QImage src = qImageFromImage(*layer->pixels)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (src.isNull()) {
        discardUndoStep();
        return false;
    }
    QTransform t;
    t.rotate(deg);
    const QImage rotated = src.transformed(t, Qt::SmoothTransformation);
    auto img = imageFromQImage(rotated);
    if (!img) {
        discardUndoStep();
        return false;
    }
    if (layer->mask && layer->maskLinked) {
        auto rotatedMask = rotatedMaskImage(*layer->mask, t);
        if (!rotatedMask) {
            discardUndoStep();
            return false;
        }
        layer->mask = std::move(rotatedMask);
        ++layer->maskStamp;
    }

    // Keep the layer's document-space centre fixed as its native bounds grow.
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const QPointF center(
        layer->offset.x() + layer->pixels->width() * lsx * 0.5,
        layer->offset.y() + layer->pixels->height() * lsy * 0.5);
    layer->pixels = img;
    layer->offset = QPointF(center.x() - img->width() * lsx * 0.5,
                            center.y() - img->height() * lsy * 0.5);
    syncLinkedMaskPlacement(*layer);
    ++layer->sourceStamp;
    layer->thumbnail = QImage();
    d->rebuildComposite();
    commitUndoStep(tr("Rotate Layer"), QStringLiteral("rotate"));
    return true;
}

}  // namespace pittore::ui
