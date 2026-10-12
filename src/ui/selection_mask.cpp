#include "ui/selection_mask.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pittore::ui {

QRect selectionMaskBbox(const QImage& mask) {
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

QImage selectionMaskLargestComponent(const QImage& mask) {
    if (mask.isNull()) return mask;
    const int w = mask.width(), h = mask.height();
    const QRect bbox = selectionMaskBbox(mask);
    if (bbox.isEmpty()) return mask;  // nothing selected


    // Flood-fill each blob, keep the biggest. Visited cells are consumed so
    // no region is scanned twice.
    QImage visited(w, h, QImage::Format_Grayscale8);
    visited.fill(0);
    std::vector<int> best;          // (x, y) pairs of the largest component
    std::vector<int> stack;
    for (int y0 = bbox.top(); y0 <= bbox.bottom(); ++y0) {
        for (int x0 = bbox.left(); x0 <= bbox.right(); ++x0) {
            if (mask.constScanLine(y0)[x0] <= 127 || visited.constScanLine(y0)[x0])
                continue;
            std::vector<int> comp;
            stack.clear();
            stack.push_back(x0);
            stack.push_back(y0);
            visited.scanLine(y0)[x0] = 1;
            while (!stack.empty()) {
                const int y = stack.back();
                stack.pop_back();
                const int x = stack.back();
                stack.pop_back();
                comp.push_back(x);
                comp.push_back(y);
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    uchar* vrow = visited.scanLine(ny);
                    const uchar* mrow = mask.constScanLine(ny);
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        if (nx < 0 || nx >= w || vrow[nx]) continue;
                        if (mrow[nx] <= 127) continue;
                        vrow[nx] = 1;
                        stack.push_back(nx);
                        stack.push_back(ny);
                    }
                }
            }
            if (comp.size() > best.size()) best.swap(comp);
        }
    }

    QImage kept(w, h, QImage::Format_Grayscale8);
    kept.fill(0);
    for (std::size_t i = 0; i + 1 < best.size(); i += 2)
        kept.scanLine(best[i + 1])[best[i]] = 255;
    return kept;
}

QImage selectionMaskRemoveSpeckles(const QImage& mask, int minPixels) {
    if (mask.isNull() || minPixels <= 0) return mask;
    const int w = mask.width(), h = mask.height();
    // A tiny selection may legitimately be all small islands: only clean
    // when the mask holds substantially more than the threshold, so there
    // is a clear main mass for specks to contrast against.
    long selected = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* row = mask.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (row[x]) ++selected;
    }
    if (selected < 16L * minPixels) return mask;
    QImage visited(w, h, QImage::Format_Grayscale8);
    visited.fill(0);
    QImage out = mask.copy();
    std::vector<int> comp;
    std::vector<int> stack;
    for (int y0 = 0; y0 < h; ++y0) {
        for (int x0 = 0; x0 < w; ++x0) {
            if (mask.constScanLine(y0)[x0] == 0 ||
                visited.constScanLine(y0)[x0])
                continue;
            comp.clear();
            stack.clear();
            stack.push_back(x0);
            stack.push_back(y0);
            visited.scanLine(y0)[x0] = 1;
            while (!stack.empty()) {
                const int y = stack.back();
                stack.pop_back();
                const int x = stack.back();
                stack.pop_back();
                comp.push_back(x);
                comp.push_back(y);
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    uchar* vrow = visited.scanLine(ny);
                    const uchar* mrow = mask.constScanLine(ny);
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        if (nx < 0 || nx >= w || vrow[nx]) continue;
                        if (mrow[nx] == 0) continue;
                        vrow[nx] = 1;
                        stack.push_back(nx);
                        stack.push_back(ny);
                    }
                }
            }
            if (comp.size() / 2 < std::size_t(minPixels)) {
                for (std::size_t i = 0; i + 1 < comp.size(); i += 2)
                    out.scanLine(comp[i + 1])[comp[i]] = 0;
            }
        }
    }
    return out;
}

// Fill small enclosed background holes via invert → RemoveSpeckles → invert
// (see the header for the contract). RemoveSpeckles drops small foreground
// islands; on the inverted matte the "foreground" is exactly the original
// background, so small enclosed background islands are dropped (→ 0) while
// the border-connected exterior survives, and inverting back turns the
// dropped islands to solid. Strands and the soft skirt are connected to the
// exterior through the inversion and are never touched.
QImage selectionMaskFillSmallHoles(const QImage& mask, int maxPixels) {
    if (mask.isNull() || maxPixels <= 0) return mask;
    const int w = mask.width(), h = mask.height();
    const QImage g = mask.format() == QImage::Format_Grayscale8
                         ? mask
                         : mask.convertToFormat(QImage::Format_Grayscale8);
    if (g.isNull()) return mask;
    QImage inv(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar* srow = g.constScanLine(y);
        uchar* drow = inv.scanLine(y);
        for (int x = 0; x < w; ++x)
            drow[x] = static_cast<uchar>(255 - srow[x]);
    }
    inv = selectionMaskRemoveSpeckles(inv, maxPixels);
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar* srow = inv.constScanLine(y);
        uchar* drow = out.scanLine(y);
        for (int x = 0; x < w; ++x)
            drow[x] = static_cast<uchar>(255 - srow[x]);
    }
    return out;
}


namespace {

// Binary morph with a square radius-`r` kernel, clipped to a padded bbox so
// small masks stay cheap. Separable: two passes make the full square.
QImage morphBinary(const QImage& mask, int r, bool dilate) {
    const int w = mask.width(), h = mask.height();
    const QRect bbox = selectionMaskBbox(mask).adjusted(-r, -r, r, r)
                           .intersected(QRect(0, 0, w, h));
    if (bbox.isEmpty()) return mask;

    QImage bin(w, h, QImage::Format_Grayscale8);
    bin.fill(0);
    for (int y = bbox.top(); y <= bbox.bottom(); ++y) {
        const uchar* srow = mask.constScanLine(y);
        uchar* brow = bin.scanLine(y);
        for (int x = bbox.left(); x <= bbox.right(); ++x)
            brow[x] = srow[x] > 127 ? 255 : 0;
    }

    QImage tmp(w, h, QImage::Format_Grayscale8);
    tmp.fill(0);
    for (int y = bbox.top(); y <= bbox.bottom(); ++y) {
        const uchar* brow = bin.constScanLine(y);
        uchar* trow = tmp.scanLine(y);
        for (int x = bbox.left(); x <= bbox.right(); ++x) {
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            bool any = false, all = true;
            for (int i = x0; i <= x1; ++i) {
                if (brow[i]) any = true;
                else all = false;
            }
            trow[x] = (dilate ? any : all) ? 255 : 0;
        }
    }
    QImage out(w, h, QImage::Format_Grayscale8);
    out.fill(0);
    for (int y = bbox.top(); y <= bbox.bottom(); ++y) {
        uchar* orow = out.scanLine(y);
        for (int x = bbox.left(); x <= bbox.right(); ++x) {
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            bool any = false, all = true;
            for (int yy = y0; yy <= y1; ++yy) {
                if (tmp.constScanLine(yy)[x]) any = true;
                else all = false;
            }
            orow[x] = (dilate ? any : all) ? 255 : 0;
        }
    }
    return out;
}

}  // namespace

QImage selectionMaskFillHoles(const QImage& mask, int bridgeRadius) {
    if (mask.isNull()) return mask;
    const int w = mask.width(), h = mask.height();
    const QRect bbox = selectionMaskBbox(mask);
    if (bbox.isEmpty()) return mask;  // nothing selected


    // `probe` only decides what's enclosed — never returned. Closing it first
    // seals narrow gaps (shadow under a rim, say) so they read as interior.
    // A plain dilate would fatten every edge instead.
    QImage probe = mask;
    if (bridgeRadius > 0) {
        const QImage closed =
            morphBinary(morphBinary(mask, bridgeRadius, /*dilate=*/true),
                        bridgeRadius, /*dilate=*/false);
        probe = closed;
        // Closing is extensive, but guard anyway: if any selected pixel were
        // dropped the flood would become more permissive, not less.
        for (int y = 0; y < h; ++y) {
            const uchar* mrow = mask.constScanLine(y);
            uchar* prow = probe.scanLine(y);
            for (int x = 0; x < w; ++x)
                if (mrow[x] > 127) prow[x] = 255;
        }
    }

    // Flood the background from the border; what it can't reach is interior.
    QImage reached(w, h, QImage::Format_Grayscale8);
    reached.fill(0);
    std::vector<int> stack;
    const auto push = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h) return;
        if (probe.constScanLine(y)[x] > 127) return;  // selected: not background
        if (reached.constScanLine(y)[x]) return;
        reached.scanLine(y)[x] = 1;
        stack.push_back(x);
        stack.push_back(y);
    };
    for (int x = 0; x < w; ++x) {
        push(x, 0);
        push(x, h - 1);
    }
    for (int y = 0; y < h; ++y) {
        push(0, y);
        push(w - 1, y);
    }
    while (!stack.empty()) {
        const int y = stack.back();
        stack.pop_back();
        const int x = stack.back();
        stack.pop_back();
        push(x - 1, y);
        push(x + 1, y);
        push(x, y - 1);
        push(x, y + 1);
    }

    // Fill unreachable background in the ORIGINAL mask; edges stay put.
    QImage out = mask;
    for (int y = 0; y < h; ++y) {
        const uchar* rrow = reached.constScanLine(y);
        const uchar* mrow = mask.constScanLine(y);
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < w; ++x)
            if (mrow[x] <= 127 && !rrow[x]) orow[x] = 255;
    }
    return out;
}

QImage selectionMaskFromLayerAlpha(const float* alpha, int pw, int ph,
                                   const QPointF& offset, double scaleX,
                                   double scaleY, const QSize& docSize) {
    QImage smask(docSize.width(), docSize.height(), QImage::Format_Grayscale8);
    smask.fill(0);
    if (!alpha || pw < 1 || ph < 1 || smask.isNull()) return smask;
    const double sx = std::max(scaleX, 1e-6);
    const double sy = std::max(scaleY, 1e-6);
    const int dw = docSize.width(), dh = docSize.height();
    // Bilinear resample: nearest-neighbour here imprints stair-steps on hair
    // strands whenever the layer scale differs from 1:1. At exactly 1:1 with
    // integer offset this collapses to an exact copy.
    for (int y = 0; y < dh; ++y) {
        uchar* row = smask.scanLine(y);
        for (int x = 0; x < dw; ++x) {
            const double lxf = (x + 0.5 - offset.x()) / sx - 0.5;
            const double lyf = (y + 0.5 - offset.y()) / sy - 0.5;
            const int x0 = std::clamp(int(std::floor(lxf)), 0, pw - 1);
            const int y0 = std::clamp(int(std::floor(lyf)), 0, ph - 1);
            const int x1 = std::clamp(x0 + 1, 0, pw - 1);
            const int y1 = std::clamp(y0 + 1, 0, ph - 1);
            const float wx = std::clamp(float(lxf - x0), 0.0f, 1.0f);
            const float wy = std::clamp(float(lyf - y0), 0.0f, 1.0f);
            const float v00 = alpha[std::size_t(y0) * pw + x0];
            const float v10 = alpha[std::size_t(y0) * pw + x1];
            const float v01 = alpha[std::size_t(y1) * pw + x0];
            const float v11 = alpha[std::size_t(y1) * pw + x1];
            const float v = v00 * (1 - wx) * (1 - wy) + v10 * wx * (1 - wy) +
                            v01 * (1 - wx) * wy + v11 * wx * wy;
            row[x] = static_cast<uchar>(
                std::clamp(int(v * 255.0f + 0.5f), 0, 255));
        }
    }
    return smask;
}

QImage selectionMaskFromPolygon(const std::vector<QPointF>& poly,
                                const QSize& docSize, bool antialias,
                                int featherPx) {
    const int w = docSize.width(), h = docSize.height();
    if (w < 1 || h < 1 || poly.size() < 3) return QImage();
    double bx0 = poly.front().x(), by0 = poly.front().y();
    double bx1 = bx0, by1 = by0;
    for (const QPointF& p : poly) {
        bx0 = std::min(bx0, p.x());
        by0 = std::min(by0, p.y());
        bx1 = std::max(bx1, p.x());
        by1 = std::max(by1, p.y());
    }
    const int ix0 = std::max(0, int(std::floor(bx0)) - 1);
    const int iy0 = std::max(0, int(std::floor(by0)) - 1);
    const int ix1 = std::min(w, int(std::ceil(bx1)) + 1);
    const int iy1 = std::min(h, int(std::ceil(by1)) + 1);
    if (ix1 <= ix0 || iy1 <= iy0) return QImage();
    QImage mask(w, h, QImage::Format_Grayscale8);
    if (mask.isNull()) return mask;
    mask.fill(0);
    const int sub = antialias ? 4 : 1;
    std::vector<double> xs;
    xs.reserve(poly.size());
    // Exact horizontal coverage with 4 sub-rows per pixel when antialiased
    // (even-odd): each sub-row contributes its covered width / sub.
    for (int y = iy0; y < iy1; ++y) {
        uchar* row = mask.scanLine(y);
        for (int x = ix0; x < ix1; ++x) {
            double acc = 0.0;
            for (int s = 0; s < sub; ++s) {
                const double yy = y + (antialias ? (s + 0.5) / sub : 0.5);
                xs.clear();
                for (std::size_t i = 0, j = poly.size() - 1;
                     i < poly.size(); j = i++) {
                    const double ay = poly[j].y(), by = poly[i].y();
                    if ((ay <= yy && by > yy) || (by <= yy && ay > yy)) {
                        const double ax = poly[j].x(), bx = poly[i].x();
                        xs.push_back(ax + (yy - ay) / (by - ay) *
                                              (bx - ax));
                    }
                }
                if (xs.empty()) continue;
                std::sort(xs.begin(), xs.end());
                for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                    const double x0 = xs[k], x1 = xs[k + 1];
                    if (antialias) {
                        const double lo = std::max(double(x), x0);
                        const double hi = std::min(double(x + 1), x1);
                        if (hi > lo) acc += (hi - lo) / sub;
                    } else {
                        const double ccx = x + 0.5;
                        if (ccx >= x0 && ccx < x1) acc += 1.0 / sub;
                    }
                }
            }
            if (acc > 1.0) acc = 1.0;
            row[x] = static_cast<uchar>(std::clamp(int(acc * 255.0 + 0.5),
                                                   0, 255));
        }
    }
    if (featherPx > 0) mask = selectionMaskFeather(mask, featherPx);
    return mask;
}

QPainterPath selectionOutlineFromMask(const QImage& mask, const QRect& region,
                                      int threshold) {
    QPainterPath path;
    const int w = mask.width(), h = mask.height();
    if (w < 1 || h < 1) return path;
    // Outside the mask counts as empty, and the contour still traces the
    // mask's own edges. Cells span corners -1..w/h.
    const QRect r = region.intersected(QRect(-1, -1, w + 2, h + 2));
    if (r.isEmpty()) return path;
    // Pad by one cell so edge contours are always traced.
    QRect cells = r.adjusted(-1, -1, 1, 1).intersected(QRect(-1, -1, w + 2, h + 2));
    const int x0 = cells.left(), y0 = cells.top(), x1 = cells.right(),
              y1 = cells.bottom();
    const auto sel = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;  // outside = empty
        return mask.constScanLine(y)[x] > threshold;
    };
    using Vec2 = std::pair<double, double>;
    struct Seg {
        Vec2 a, b;
    };
    std::vector<Seg> segs;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int code = (sel(x, y) ? 1 : 0) | (sel(x + 1, y) ? 2 : 0) |
                             (sel(x + 1, y + 1) ? 4 : 0) | (sel(x, y + 1) ? 8 : 0);
            if (code == 0 || code == 15) continue;
            const Vec2 mT{x + 0.5, y}, mR{x + 1.0, y + 0.5}, mB{x + 0.5, y + 1.0},
                mL{x, y + 0.5};
            switch (code) {
                case 1: case 14: segs.push_back({mL, mT}); break;  // TL corner
                case 2: case 13: segs.push_back({mT, mR}); break;  // TR corner
                case 3: case 12: segs.push_back({mL, mR}); break;  // top two
                case 4: case 11: segs.push_back({mR, mB}); break;  // BR corner
                case 6: case 9:  segs.push_back({mT, mB}); break;  // right two
                case 8: case 7:  segs.push_back({mB, mL}); break;  // BL corner
                case 5: segs.push_back({mL, mB}); segs.push_back({mT, mR}); break;
                case 10: segs.push_back({mT, mL}); segs.push_back({mR, mB}); break;
                default: break;
            }
        }
    }
    if (segs.empty()) return path;

    // Chain segments into loops. Endpoints sit on half-grid spots, so pack
    // them to int keys (2x, 2y) for matching.
    const auto key = [](const Vec2& v) {
        const std::int64_t ix = std::int64_t(std::lround(v.first * 2.0));
        const std::int64_t iy = std::int64_t(std::lround(v.second * 2.0));
        return (std::uint64_t(std::uint64_t(ix) << 32)) |
               (std::uint64_t(std::uint64_t(iy) & 0xffffffffu));
    };
    std::unordered_map<std::uint64_t, std::vector<int>> byStart, byEnd;
    for (int i = 0; i < int(segs.size()); ++i) {
        byStart[key(segs[i].a)].push_back(i);
        byEnd[key(segs[i].b)].push_back(i);
    }
    std::vector<std::uint8_t> used(segs.size(), 0);
    std::vector<Vec2> chain;
    for (int s = 0; s < int(segs.size()); ++s) {
        if (used[s]) continue;
        chain.clear();
        used[s] = 1;
        chain.push_back(segs[s].a);
        chain.push_back(segs[s].b);
        // Join at either end, either way round — neighbours can emit reversed.
        bool grew = true;
        while (grew) {
            grew = false;
            const auto k = key(chain.back());
            for (int j : byStart[k])
                if (!used[j]) { used[j] = 1; chain.push_back(segs[j].b); grew = true; break; }
            if (!grew)
                for (int j : byEnd[k])
                    if (!used[j]) { used[j] = 1; chain.push_back(segs[j].a); grew = true; break; }
            if (!grew) {
                const auto f = key(chain.front());
                for (int j : byEnd[f])
                    if (!used[j]) { used[j] = 1; chain.insert(chain.begin(), segs[j].a); grew = true; break; }
                if (!grew)
                    for (int j : byStart[f])
                        if (!used[j]) { used[j] = 1; chain.insert(chain.begin(), segs[j].b); grew = true; break; }
            }
        }
        if (chain.size() < 2) continue;
        path.moveTo(QPointF(chain.front().first, chain.front().second));
        for (std::size_t i = 1; i < chain.size(); ++i)
            path.lineTo(QPointF(chain[i].first, chain[i].second));
        path.closeSubpath();  // every contour is a closed loop
    }
    return path;
}

namespace {

// Split a polygon into loops (MoveTo starts, LineTo extends, Close ends).
// Curves shouldn't happen here — callers check first.
struct OutlineLoop {
    QVector<QPointF> pts;
    bool closed = false;
};

QVector<OutlineLoop> pathToLoops(const QPainterPath& p) {
    QVector<OutlineLoop> loops;
    const int n = p.elementCount();
    int i = 0;
    while (i < n) {
        if (p.elementAt(i).type != QPainterPath::MoveToElement) {
            ++i;
            continue;
        }
        OutlineLoop loop;
        loop.pts.push_back(QPointF(p.elementAt(i).x, p.elementAt(i).y));
        int j = i + 1;
        for (; j < n; ++j) {
            const QPainterPath::Element& el = p.elementAt(j);
            if (el.type == QPainterPath::MoveToElement) break;
            if (el.type != QPainterPath::LineToElement) break;
            loop.pts.push_back(QPointF(el.x, el.y));
        }
        i = j;
        // Qt turns closeSubpath() into a final line home; equal ends = closed.
        if (loop.pts.size() >= 3 && loop.pts.back() == loop.pts.front())
            loop.closed = true;
        if (loop.pts.size() >= 2) loops.push_back(std::move(loop));
    }
    return loops;
}

// Drop near-collinear vertices; closed-loop wraparound.
QVector<QPointF> collapseCollinear(const QVector<QPointF>& in) {
    const int m = in.size();
    if (m < 4) return in;
    QVector<QPointF> out;
    out.reserve(m);
    for (int i = 0; i < m; ++i) {
        const QPointF& prev = in[(i - 1 + m) % m];
        const QPointF& cur = in[i];
        const QPointF& next = in[(i + 1) % m];
        const double v0x = cur.x() - prev.x(), v0y = cur.y() - prev.y();
        const double v1x = next.x() - cur.x(), v1y = next.y() - cur.y();
        const double cross = v0x * v1y - v0y * v1x;
        const double denom = std::hypot(v0x, v0y) * std::hypot(v1x, v1y);
        if (denom > 0 && std::abs(cross) / denom < 1e-3) continue;  // straight
        out.push_back(cur);
    }
    return out;
}

// Catmull-Rom round of a closed loop, as cubics through every vertex.
QPainterPath catmullRomClosed(const QVector<QPointF>& pts) {
    QPainterPath path;
    const int m = pts.size();
    if (m < 3) return path;
    const auto P = [&](int i) -> QPointF { return pts[((i % m) + m) % m]; };
    path.moveTo(pts.front());
    for (int i = 0; i < m; ++i) {
        const QPointF p0 = P(i - 1), p1 = P(i), p2 = P(i + 1), p3 = P(i + 2);
        const QPointF c1 = p1 + (p2 - p0) / 6.0;
        const QPointF c2 = p2 - (p3 - p1) / 6.0;
        path.cubicTo(c1, c2, p2);
    }
    path.closeSubpath();
    return path;
}

}  // namespace

QPainterPath selectionOutlineSmoothed(const QPainterPath& raw) {
    if (raw.isEmpty()) return raw;
    const int n = raw.elementCount();
    for (int i = 0; i < n; ++i) {
        const QPainterPath::ElementType t = raw.elementAt(i).type;
        if (t == QPainterPath::CurveToElement ||
            t == QPainterPath::CurveToDataElement)
            return raw;  // already smooth — keep as-is
    }
    const QVector<OutlineLoop> loops = pathToLoops(raw);
    QPainterPath out;
    for (const OutlineLoop& loop : loops) {
        QVector<QPointF> p;
        p.reserve(loop.pts.size());
        for (const QPointF& q : loop.pts)
            if (p.isEmpty() || p.back() != q) p.push_back(q);
        if (loop.closed && p.size() > 2 && p.back() == p.front())
            p.pop_back();  // drop the explicit return-to-start
        if (p.size() < 3 || !loop.closed) {
            if (p.size() >= 2) {
                out.moveTo(p.front());
                for (int k = 1; k < p.size(); ++k) out.lineTo(p[k]);
                if (loop.closed) out.closeSubpath();
            }
            continue;
        }
        // Spike removal: a vertex doubling straight back is tracer noise.
        // A few passes settle cascades.
        for (int pass = 0; pass < 3; ++pass) {
            const int m = p.size();
            QVector<QPointF> q;
            q.reserve(m);
            bool changed = false;
            for (int k = 0; k < m; ++k) {
                const QPointF& prev = p[(k - 1 + m) % m];
                const QPointF& cur = p[k];
                const QPointF& next = p[(k + 1) % m];
                const double v0x = cur.x() - prev.x(), v0y = cur.y() - prev.y();
                const double v1x = next.x() - cur.x(), v1y = next.y() - cur.y();
                const double l0 = std::hypot(v0x, v0y), l1 = std::hypot(v1x, v1y);
                if (l0 > 0.25 && l1 > 0.25) {
                    const double dot =
                        (v0x * v1x + v0y * v1y) / (l0 * l1);
                    if (dot < -0.5) {  // doubles back >120°
                        changed = true;
                        continue;
                    }
                }
                q.push_back(cur);
            }
            p = q;
            if (!changed) break;
        }
        p = collapseCollinear(p);
        if (p.size() < 3) continue;
        if (p.size() < 5) {
            out.moveTo(p.front());
            for (int k = 1; k < p.size(); ++k) out.lineTo(p[k]);
            out.closeSubpath();
            continue;
        }
        out.addPath(catmullRomClosed(p));
    }
    return out;
}

namespace {

// Separable box blur, radius `r`. Uses scanLine so strides stay right.
QImage boxBlurPass(const QImage& in, int r) {
    if (r <= 0) return in;
    const int w = in.width(), h = in.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar* irow = in.constScanLine(y);
        uchar* trow = tmp.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            int acc = 0;
            for (int i = x0; i <= x1; ++i) acc += irow[i];
            trow[x] = static_cast<uchar>(acc / (x1 - x0 + 1));
        }
    }
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            int acc = 0;
            for (int yy = y0; yy <= y1; ++yy) acc += tmp.constScanLine(yy)[x];
            orow[x] = static_cast<uchar>(acc / (y1 - y0 + 1));
        }
    }
    return out;
}

// Grow (dilate) / shrink (erode) of the >127 region by a square kernel radius
// `r`, as a binary pass. Works only near the current selection bbox so tiny
// masks stay cheap even at full document resolution.
QImage morphRegion(const QImage& mask, int r, bool dilate) {
    const int w = mask.width(), h = mask.height();
    const QRect bbox = selectionMaskBbox(mask).adjusted(-r, -r, r, r)
                           .intersected(QRect(0, 0, w, h));
    if (bbox.isEmpty()) return mask;

    // Grayscale morphology (local max for dilate, local min for erode):
    // unlike a binarise-then-morph, this displaces soft gradation intact
    // instead of imprinting a hard rim that later stages must sand off.
    QImage tmp(w, h, QImage::Format_Grayscale8);
    tmp.fill(0);
    for (int y = bbox.top(); y <= bbox.bottom(); ++y) {
        const uchar* srow = mask.constScanLine(y);
        uchar* trow = tmp.scanLine(y);
        for (int x = bbox.left(); x <= bbox.right(); ++x) {
            const int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            int v = dilate ? 0 : 255;
            for (int i = x0; i <= x1; ++i)
                v = dilate ? std::max(v, int(srow[i]))
                           : std::min(v, int(srow[i]));
            trow[x] = static_cast<uchar>(v);
        }
    }
    QImage out(w, h, QImage::Format_Grayscale8);
    out.fill(0);
    for (int y = bbox.top(); y <= bbox.bottom(); ++y) {
        uchar* orow = out.scanLine(y);
        for (int x = bbox.left(); x <= bbox.right(); ++x) {
            const int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
            int v = dilate ? 0 : 255;
            for (int yy = y0; yy <= y1; ++yy) {
                const int t = tmp.constScanLine(yy)[x];
                v = dilate ? std::max(v, t) : std::min(v, t);
            }
            orow[x] = static_cast<uchar>(v);
        }
    }
    return out;
}

}  // namespace

QImage selectionMaskGrow(const QImage& mask, int radius) {
    if (mask.isNull() || radius == 0) return mask;
    QImage out = morphRegion(mask, std::abs(radius), radius > 0);
    // Re-soften proportionally to the displacement: a fixed 1px blur after a
    // multi-px morph leaves a hard rim (a visible contour where the edge
    // landed). The boundary stays approximately as soft as it was moved.
    const int soften =
        std::clamp((std::abs(radius) + 1) / 2, 1, 8);
    return boxBlurPass(out, soften);
}

QImage selectionMaskSmooth(const QImage& mask, int passes) {
    if (mask.isNull()) return mask;
    QImage out = mask;
    for (int i = 0; i < std::max(0, passes); ++i) out = boxBlurPass(out, 1);
    return out;
}

QImage selectionMaskFeather(const QImage& mask, int radius) {
    if (mask.isNull()) return mask;
    return boxBlurPass(mask, std::max(0, radius));
}

QImage selectionMaskRamp(const QImage& mask, double ramp) {
    if (mask.isNull()) return mask;
    if (ramp <= 0.0 || std::abs(ramp - 1.0) < 1e-4) return mask;
    QImage out(mask.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar* irow = mask.constScanLine(y);
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            const double v = irow[x] / 255.0;
            orow[x] = static_cast<uchar>(std::clamp(
                int(std::pow(v, ramp) * 255.0 + 0.5), 0, 255));
        }
    }
    return out;
}

QImage selectionMaskBrushStamp(const QImage& mask, const QPointF& center,
                               double radius, double hardness, int value) {
    if (mask.isNull() || radius <= 0.0) return mask;
    QImage out = mask;
    const int w = mask.width(), h = mask.height();
    const int x0 = std::max(0, int(center.x() - radius - 2.0));
    const int x1 = std::min(w - 1, int(center.x() + radius + 2.0));
    const int y0 = std::max(0, int(center.y() - radius - 2.0));
    const int y1 = std::min(h - 1, int(center.y() + radius + 2.0));
    const double inner = radius * std::clamp(hardness, 0.0, 1.0);
    const double span = std::max(1e-6, radius - inner);
    for (int y = y0; y <= y1; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = x0; x <= x1; ++x) {
            const double dx = x + 0.5 - center.x(), dy = y + 0.5 - center.y();
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d > radius) continue;
            const double fall =
                d <= inner ? 1.0 : std::clamp((radius - d) / span, 0.0, 1.0);
            row[x] = static_cast<uchar>(std::clamp(
                int(value * fall + row[x] * (1.0 - fall) + 0.5), 0, 255));
        }
    }
    return out;
}


QImage selectionMaskFeatherStamp(const QImage& mask, const QPointF& center,
                                 double radius, double strength) {
    if (mask.isNull() || radius <= 0.0) return mask;
    strength = std::clamp(strength, 0.0, 1.0);
    if (strength <= 0.0) return mask;
    QImage out = mask;
    const int w = mask.width(), h = mask.height();
    const int x0 = std::max(0, int(center.x() - radius - 2.0));
    const int x1 = std::min(w - 1, int(center.x() + radius + 2.0));
    const int y0 = std::max(0, int(center.y() - radius - 2.0));
    const int y1 = std::min(h - 1, int(center.y() + radius + 2.0));
    // Mean of the 3x3 neighborhood, blended by a soft disc falloff.
    for (int y = y0; y <= y1; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = x0; x <= x1; ++x) {
            const double dx = x + 0.5 - center.x(), dy = y + 0.5 - center.y();
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d > radius) continue;
            const double fall =
                std::clamp(1.0 - d / radius, 0.0, 1.0) * strength;
            int sum = 0, n = 0;
            for (int yy = std::max(0, y - 1); yy <= std::min(h - 1, y + 1);
                 ++yy) {
                const uchar* srow = mask.constScanLine(yy);
                for (int xx = std::max(0, x - 1);
                     xx <= std::min(w - 1, x + 1); ++xx) {
                    sum += srow[xx];
                    ++n;
                }
            }
            const double mean = n > 0 ? double(sum) / n : row[x];
            row[x] = static_cast<uchar>(
                std::clamp(int(mean * fall + row[x] * (1.0 - fall) + 0.5), 0,
                           255));
        }
    }
    return out;
}


namespace {

// Square-kernel dilation of a binary (0/255) image, O(w*h) via a running
// window per row and then per column. The refine band may be hundreds of
// pixels wide on a big matte, so a nested per-pixel window scan would
// crawl; this stays a single pass over the pixels either way.
QImage dilateBinary(const QImage& bin, int r) {
    const int w = bin.width(), h = bin.height();
    QImage tmp(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const uchar* s = bin.constScanLine(y);
        uchar* d = tmp.scanLine(y);
        int cnt = 0, right = -1;
        for (int x = 0; x < w; ++x) {
            const int nhi = std::min(w - 1, x + r);
            while (right < nhi) cnt += s[++right] ? 1 : 0;
            const int nlo = x - r;
            if (nlo - 1 >= 0) cnt -= s[nlo - 1] ? 1 : 0;
            d[x] = cnt ? 255 : 0;
        }
    }
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int x = 0; x < w; ++x) {
        int cnt = 0, bottom = -1;
        for (int y = 0; y < h; ++y) {
            const int nhi = std::min(h - 1, y + r);
            while (bottom < nhi) {
                ++bottom;
                cnt += tmp.constScanLine(bottom)[x] ? 1 : 0;
            }
            const int nlo = y - r;
            if (nlo - 1 >= 0) cnt -= tmp.constScanLine(nlo - 1)[x] ? 1 : 0;
            out.scanLine(y)[x] = cnt ? 255 : 0;
        }
    }
    return out;
}

QImage invertBinary(const QImage& bin) {
    QImage out(bin.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < out.height(); ++y) {
        const uchar* s = bin.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x) d[x] = s[x] ? 0 : 255;
    }
    return out;
}

}  // namespace

QImage selectionMaskDecayWash(const QImage& mask, const QImage& guide,
                              int supportPx, double texTol) {
    if (mask.isNull() || guide.isNull() || mask.size() != guide.size())
        return mask;
    if (supportPx <= 0) return mask;
    const int w = mask.width(), h = mask.height();
    QImage fg(w, h, QImage::Format_Grayscale8);
    fg.fill(0);
    for (int y = 0; y < h; ++y) {
        const uchar* srow = mask.constScanLine(y);
        uchar* frow = fg.scanLine(y);
        for (int x = 0; x < w; ++x)
            if (srow[x] >= 200) frow[x] = 255;
    }
    if (selectionMaskBbox(fg).isEmpty()) return mask;  // nothing confident
    const QImage support = dilateBinary(fg, supportPx);
    // Distance outside the support (capped): wash fades over falloffPx
    // instead of zeroing outright, so no hard contour appears where the
    // decay ends. Same chamfer pattern as the edge snap.
    constexpr int kFall = 8;
    QVector<int> dist(w * h, INT_MAX);
    for (int y = 0; y < h; ++y) {
        const uchar* prow = support.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (prow[x]) dist[y * w + x] = 0;
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int d = dist[y * w + x];
            if (x > 0) d = std::min(d, dist[y * w + x - 1] + 1);
            if (y > 0) d = std::min(d, dist[(y - 1) * w + x] + 1);
            dist[y * w + x] = std::min(d, kFall + 1);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            int d = dist[y * w + x];
            if (x + 1 < w) d = std::min(d, dist[y * w + x + 1] + 1);
            if (y + 1 < h) d = std::min(d, dist[(y + 1) * w + x] + 1);
            dist[y * w + x] = std::min(d, kFall + 1);
        }
    QImage lum(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        const QRgb* grow =
            reinterpret_cast<const QRgb*>(guide.constScanLine(y));
        uchar* lrow = lum.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const QRgb px = grow[x];
            lrow[x] = static_cast<uchar>(
                (77 * qRed(px) + 150 * qGreen(px) + 29 * qBlue(px)) >> 8);
        }
    }
    QImage out = mask;
    // Radius-2 Laplacian of luma: hair strands and hard edges carry large
    // second-order energy; smooth glow wash and bilinear-upsample grid
    // plateaus sit near zero even where their first-order slope is not.
    const int R = 2;
    for (int y = 0; y < h; ++y) {
        const uchar* srow = mask.constScanLine(y);
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int v = srow[x];
            if (v <= 0 || v >= 255) continue;
            const double keep = dist[y * w + x] <= 0
                                    ? 1.0
                                    : std::max(
                                          0.0, 1.0 - dist[y * w + x] /
                                                         double(kFall));
            if (keep >= 1.0) continue;
            const int xm = x - R >= 0 ? x - R : 0;
            const int xp = x + R < w ? x + R : w - 1;
            const int ym = y - R >= 0 ? y - R : 0;
            const int yp = y + R < h ? y + R : h - 1;
            const uchar* lxm = lum.constScanLine(y);
            const uchar* lym = lum.constScanLine(ym);
            const uchar* lyc = lum.constScanLine(y);
            const uchar* lyp = lum.constScanLine(yp);
            const double lap =
                (double(lxm[xp]) + double(lxm[xm]) + double(lym[x]) +
                 double(lyp[x]) - 4.0 * double(lyc[x])) *
                0.25;
            if (lap < texTol && lap > -texTol) {
                // Featureless wash: attenuate with distance instead of
                // zeroing, so no contour appears where the decay ends.
                orow[x] = static_cast<uchar>(v * keep + 0.5);
            }
        }
    }
    return out;
}

QImage selectionMaskSnapToEdges(const QImage& guide, const QImage& mask,
                                int bandPx, int passes) {
    if (guide.isNull() || mask.isNull() || bandPx <= 0 || passes <= 0)
        return mask;
    if (guide.size() != mask.size()) return mask;
    bandPx = std::min(bandPx, 256);
    const int w = mask.width(), h = mask.height();
    // An ARGB guide solves in RGB colour space — the same affinity model the
    // matte brush uses — so a cool backdrop and a warm subject of equal tone
    // still separate (dark hair over a blue-grey backdrop). Anything else is
    // a tone guide and solves on luma alone.
    const bool isColor = guide.format() == QImage::Format_ARGB32 ||
                         guide.format() == QImage::Format_ARGB32_Premultiplied;
    QImage argb, luma;
    if (isColor) {
        argb = guide.format() == QImage::Format_ARGB32
                   ? guide
                   : guide.convertToFormat(QImage::Format_ARGB32);
        if (argb.isNull()) return mask;
    } else {
        luma = guide.format() == QImage::Format_Grayscale8
                   ? guide
                   : guide.convertToFormat(QImage::Format_Grayscale8);
        if (luma.isNull()) return mask;
    }
    // A flat guide carries no edge information: pass through untouched.
    if (isColor) {
        int rMin = 255, rMax = 0, gMin = 255, gMax = 0, bMin = 255, bMax = 0;
        for (int y = 0; y < h; ++y) {
            const QRgb* row =
                reinterpret_cast<const QRgb*>(argb.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb px = row[x];
                rMin = std::min(rMin, qRed(px));
                rMax = std::max(rMax, qRed(px));
                gMin = std::min(gMin, qGreen(px));
                gMax = std::max(gMax, qGreen(px));
                bMin = std::min(bMin, qBlue(px));
                bMax = std::max(bMax, qBlue(px));
            }
        }
        if (rMin == rMax && gMin == gMax && bMin == bMax) return mask;
    } else {
        int lumaMin = 255, lumaMax = 0;
        for (int y = 0; y < h; ++y) {
            const uchar* lrow = luma.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                if (lrow[x] < lumaMin) lumaMin = lrow[x];
                if (lrow[x] > lumaMax) lumaMax = lrow[x];
            }
        }
        if (lumaMin == lumaMax) return mask;
    }
    // The movable band — the ONLY pixels the solve may rewrite:
    //  - soft input: within bandPx of a transition (0<cov<255), so solid
    //    areas never wobble and a feathered edge can translate;
    //  - hard input (the usual marquee): a band straddling the boundary
    //    itself. A binary matte has no ramp to translate, so the solve
    //    re-derives the edge from the guide right where it sits — which is
    //    exactly what "Matte edges" is for.
    QImage trans(w, h, QImage::Format_Grayscale8);
    trans.fill(0);
    QImage bin(w, h, QImage::Format_Grayscale8);
    bin.fill(0);
    int inside = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* mrow = mask.constScanLine(y);
        uchar* trow = trans.scanLine(y);
        uchar* brow = bin.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int v = mrow[x];
            if (v > 0 && v < 255) trow[x] = 255;
            if (v > 127) {
                brow[x] = 255;
                ++inside;
            }
        }
    }
    if (inside == 0 || inside == w * h) return mask;  // empty / full mask
    QImage gate(w, h, QImage::Format_Grayscale8);
    gate.fill(0);
    if (!selectionMaskBbox(trans).isEmpty()) {
        gate = dilateBinary(trans, bandPx);
    } else {
        const QImage d = dilateBinary(bin, bandPx);
        const QImage e =
            invertBinary(dilateBinary(invertBinary(bin), bandPx));
        for (int y = 0; y < h; ++y) {
            const uchar* br = bin.constScanLine(y);
            const uchar* dr = d.constScanLine(y);
            const uchar* er = e.constScanLine(y);
            uchar* gr = gate.scanLine(y);
            for (int x = 0; x < w; ++x)
                gr[x] = ((br[x] && !er[x]) || (dr[x] && !br[x])) ? 255 : 0;
        }
    }
    // Loop bounds: the band's own bbox, so a small matte on a big image
    // stays cheap.
    int zx0 = w, zy0 = h, zx1 = -1, zy1 = -1;
    for (int y = 0; y < h; ++y) {
        const uchar* grow = gate.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (grow[x]) {
                if (x < zx0) zx0 = x;
                if (x > zx1) zx1 = x;
                if (y < zy0) zy0 = y;
                if (y > zy1) zy1 = y;
            }
        }
    }
    if (zx1 < zx0) return mask;  // nothing inside the band
    // Foreground/background models from confident pixels inside the loop
    // bounds (foreground >= 200 as in the matte brush solve; background <= 8
    // so only truly clear pixels vote — faint wisps would otherwise tint the
    // backdrop model toward hair).
    // A colour guide models all three channels, a tone guide models luma.
    // Distance-to-gate field (capped chamfer over the zone): model sampling
    // stays near the boundary it solves, so one subject region's colours
    // (coat, skin, hair) don't muddy another's. modelR tracks the band; if
    // nothing confident sits that close, the pass falls back to zone-wide
    // sampling below, so tight bands never starve outright.
    const int modelR = bandPx;
    QVector<int> dist((zy1 - zy0 + 1) * (zx1 - zx0 + 1), INT_MAX);
    auto distAt = [&](int x, int y) -> int& {
        return dist[(y - zy0) * (zx1 - zx0 + 1) + (x - zx0)];
    };
    for (int y = zy0; y <= zy1; ++y) {
        const uchar* grow = gate.constScanLine(y);
        for (int x = zx0; x <= zx1; ++x)
            if (grow[x]) distAt(x, y) = 0;
    }
    for (int y = zy0; y <= zy1; ++y)
        for (int x = zx0; x <= zx1; ++x) {
            int d = distAt(x, y);
            if (x > zx0) d = std::min(d, distAt(x - 1, y) + 1);
            if (y > zy0) d = std::min(d, distAt(x, y - 1) + 1);
            distAt(x, y) = std::min(d, modelR + 1);
        }
    for (int y = zy1; y >= zy0; --y)
        for (int x = zx1; x >= zx0; --x) {
            int d = distAt(x, y);
            if (x < zx1) d = std::min(d, distAt(x + 1, y) + 1);
            if (y < zy1) d = std::min(d, distAt(x, y + 1) + 1);
            distAt(x, y) = std::min(d, modelR + 1);
        }
    double fcr = 0, fcg = 0, fcb = 0, bcr = 0, bcg = 0, bcb = 0;
    auto models = [&](const QImage& m, double& fg, double& bg, int& fn,
                      int& bn, int maxDist) {
        double fs = 0, bs = 0;
        fcr = fcg = fcb = bcr = bcg = bcb = 0.0;
        fn = 0;
        bn = 0;
        for (int y = zy0; y <= zy1; ++y) {
            const uchar* mrow = m.constScanLine(y);
            const uchar* lrow = isColor ? nullptr : luma.constScanLine(y);
            const QRgb* crow = isColor
                                   ? reinterpret_cast<const QRgb*>(
                                         argb.constScanLine(y))
                                   : nullptr;
            for (int x = zx0; x <= zx1; ++x) {
                if (maxDist >= 0 && distAt(x, y) > maxDist) continue;
                const uchar v = mrow[x];
                if (v >= 200) {
                    if (isColor) {
                        const QRgb px = crow[x];
                        fcr += qRed(px);
                        fcg += qGreen(px);
                        fcb += qBlue(px);
                    } else {
                        fs += lrow[x];
                    }
                    ++fn;
                } else if (v <= 8) {
                    // Background samples must be truly clear: the old <=55 bar
                    // admitted faint wisps and skirt pixels (coverage 9..55),
                    // whose hair-mixed colours polluted the backdrop model and
                    // made real background read "far from bg" — arming the
                    // bg-proof that then protected wash, skirts and
                    // hallucinated domes from ever being solved down. Clear
                    // pixels keep the model honest; starvation falls back to
                    // the zone-wide/global means below as before.
                    if (isColor) {
                        const QRgb px = crow[x];
                        bcr += qRed(px);
                        bcg += qGreen(px);
                        bcb += qBlue(px);
                    } else {
                        bs += lrow[x];
                    }
                    ++bn;
                }
            }
        }
        fg = fn > 0 ? fs / fn : 0.0;
        bg = bn > 0 ? bs / bn : 0.0;
        if (isColor && fn > 0) {
            fcr /= fn;
            fcg /= fn;
            fcb /= fn;
        }
        if (isColor && bn > 0) {
            bcr /= bn;
            bcg /= bn;
            bcb /= bn;
        }
    };
    QImage work = mask;
    // Global fallback models: soft AI masks often hold too few confident
    // (fg >= 200 / truly-clear bg) samples inside the zone for the local
    // means, in which case every pass used to bail out and the dialog visibly
    // did nothing.
    // Project against whole-image means instead; only a fully soft mask
    // keeps its input.
    double gfg[3] = {}, gbg[3] = {};
    long gfn = 0, gbn = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* srow = mask.constScanLine(y);
        const QRgb* crow = isColor
                               ? reinterpret_cast<const QRgb*>(
                                     argb.constScanLine(y))
                               : nullptr;
        const uchar* lrow =
            isColor ? nullptr : luma.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const uchar v = srow[x];
            if (v >= 200) {
                if (isColor) {
                    const QRgb px = crow[x];
                    gfg[0] += qRed(px);
                    gfg[1] += qGreen(px);
                    gfg[2] += qBlue(px);
                } else {
                    gfg[0] += lrow[x];
                    gfg[1] += lrow[x];
                    gfg[2] += lrow[x];
                }
                ++gfn;
            } else if (v <= 8) {
                // Same truly-clear bar as the local models above: faint wisps
                // must not vote in the global backdrop mean either.
                if (isColor) {
                    const QRgb px = crow[x];
                    gbg[0] += qRed(px);
                    gbg[1] += qGreen(px);
                    gbg[2] += qBlue(px);
                } else {
                    gbg[0] += lrow[x];
                    gbg[1] += lrow[x];
                    gbg[2] += lrow[x];
                }
                ++gbn;
            }
        }
    }
    const bool gHaveBoth = gfn >= 8 && gbn >= 8;
    if (gHaveBoth) {
        for (int c = 0; c < 3; ++c) {
            gfg[c] /= gfn;
            gbg[c] /= gbn;
        }
    }
    for (int pass = 0; pass < passes; ++pass) {
        double fg = 0, bg = 0;
        int fn = 0, bn = 0;
        // Near-gate models first: boundary colours, not zone averages.
        models(work, fg, bg, fn, bn, modelR);
        if (fn < 8 || bn < 8)
            models(work, fg, bg, fn, bn, -1);  // whole zone, then global
        if (fn < 8 || bn < 8) {
            if (!gHaveBoth) return work;  // cannot model either side
            fcr = gfg[0];
            fcg = gfg[1];
            fcb = gfg[2];
            bcr = gbg[0];
            bcg = gbg[1];
            bcb = gbg[2];
            fg = 0.299 * gfg[0] + 0.587 * gfg[1] + 0.114 * gfg[2];
            bg = 0.299 * gbg[0] + 0.587 * gbg[1] + 0.114 * gbg[2];
            fn = bn = 8;
        }
        // Degenerate guide: foreground and background read the same colour
        // (flat paper under the edge, same-tone subject/background). There
        // is nothing to solve against, and dividing it out would erase the
        // whole band — keep the input matte instead.
        if (isColor) {
            const double sep = (fcr - bcr) * (fcr - bcr) +
                               (fcg - bcg) * (fcg - bcg) +
                               (fcb - bcb) * (fcb - bcb);
            if (sep < 16.0 * 16.0) return work;
        } else if (std::abs(fg - bg) < 8.0) {
            return work;
        }
        for (int y = zy0; y <= zy1; ++y) {
            uchar* orow = work.scanLine(y);
            const uchar* grow = gate.constScanLine(y);
            const uchar* lrow = isColor ? nullptr : luma.constScanLine(y);
            const QRgb* crow = isColor
                                   ? reinterpret_cast<const QRgb*>(
                                         argb.constScanLine(y))
                                   : nullptr;
            for (int x = zx0; x <= zx1; ++x) {
                if (!grow[x]) continue;  // outside the band: untouched
                double alpha;
                double dist2b;  // colour (or luma) distance to the bg mean
                if (isColor) {
                    const QRgb px = crow[x];
                    const double drf = qRed(px) - fcr;
                    const double dgf = qGreen(px) - fcg;
                    const double dbf = qBlue(px) - fcb;
                    const double drb = qRed(px) - bcr;
                    const double dgb = qGreen(px) - bcg;
                    const double dbb = qBlue(px) - bcb;
                    const double df = drf * drf + dgf * dgf + dbf * dbf;
                    const double db = drb * drb + dgb * dgb + dbb * dbb;
                    alpha = db / (df + db + 1e-6);
                    dist2b = db;
                } else {
                    const double l = lrow[x];
                    const double df = (l - fg) * (l - fg);
                    const double db = (l - bg) * (l - bg);
                    alpha = db / (df + db + 1e-6);
                    dist2b = db;
                }
                const double solved = alpha * 255.0;
                const double seed = orow[x];
                // Same asymmetric rule as the matte solve: coverage rises
                // freely, falls only with bg-colour proof (12 RMS of the
                // bg mean) — cast-coloured subject pixels on the rim must
                // not be dragged out of a solid selection.
                const double proof = isColor ? 3.0 * 12.0 * 12.0
                                             : 12.0 * 12.0;
                if (dist2b > proof && solved + 0.5 < seed) {
                    orow[x] = static_cast<uchar>(seed);
                } else {
                    orow[x] = static_cast<uchar>(std::clamp(
                        int(solved + 0.5), 0, 255));
                }
            }
        }
    }
    return work;
}


QImage selectionMaskBlendAiHair(const QImage& base, const QImage& aiMask,
                                int bandPx) {
    if (base.isNull() || aiMask.isNull()) return base;
    if (base.size() != aiMask.size()) return base;
    const QImage b = base.format() == QImage::Format_Grayscale8
                         ? base
                         : base.convertToFormat(QImage::Format_Grayscale8);
    QImage a = aiMask.format() == QImage::Format_Grayscale8
                   ? aiMask
                   : aiMask.convertToFormat(QImage::Format_Grayscale8);
    if (b.isNull() || a.isNull()) return base;
    const int w = b.width(), h = b.height();
    bandPx = std::clamp(bandPx, 1, 64);
    // Empty / full matte: nothing to enhance.
    int inside = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* row = b.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (row[x] > 127) ++inside;
    }
    if (inside == 0 || inside == w * h) return base;
    // Degenerate AI (all empty / all full): nothing to blend — the model found
    // no edge. Blending it would flood the band with one side.
    {
        int aiInside = 0;
        for (int y = 0; y < h; ++y) {
            const uchar* arow = a.constScanLine(y);
            for (int x = 0; x < w; ++x)
                if (arow[x] > 127) ++aiInside;
        }
        if (aiInside == 0 || aiInside == w * h) return base;
    }
    // Polarity: whichever of ai / inverted-ai agrees with base at >127 wins.
    long agree = 0, disagree = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* brow = b.constScanLine(y);
        const uchar* arow = a.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const bool bv = brow[x] > 127;
            const bool av = arow[x] > 127;
            if (bv == av) ++agree;
            else ++disagree;
        }
    }
    if (disagree > agree) {
        for (int y = 0; y < h; ++y) {
            uchar* arow = a.scanLine(y);
            for (int x = 0; x < w; ++x) arow[x] = uchar(255 - arow[x]);
        }
    }
    // Movable band around the base edge (same gate as the edge snap): soft
    // input dilates its transition, hard input straddles the boundary.
    QImage trans(w, h, QImage::Format_Grayscale8);
    trans.fill(0);
    QImage bin(w, h, QImage::Format_Grayscale8);
    bin.fill(0);
    for (int y = 0; y < h; ++y) {
        const uchar* mrow = b.constScanLine(y);
        uchar* trow = trans.scanLine(y);
        uchar* brow = bin.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int v = mrow[x];
            if (v > 0 && v < 255) trow[x] = 255;
            if (v > 127) brow[x] = 255;
        }
    }
    QImage gate(w, h, QImage::Format_Grayscale8);
    gate.fill(0);
    if (!selectionMaskBbox(trans).isEmpty()) {
        gate = dilateBinary(trans, bandPx);
    } else {
        const QImage d = dilateBinary(bin, bandPx);
        const QImage e = invertBinary(dilateBinary(invertBinary(bin), bandPx));
        for (int y = 0; y < h; ++y) {
            const uchar* br = bin.constScanLine(y);
            const uchar* dr = d.constScanLine(y);
            const uchar* er = e.constScanLine(y);
            uchar* gr = gate.scanLine(y);
            for (int x = 0; x < w; ++x)
                gr[x] = ((br[x] && !er[x]) || (dr[x] && !br[x])) ? 255 : 0;
        }
    }
    QImage out = b;
    for (int y = 0; y < h; ++y) {
        const uchar* grow = gate.constScanLine(y);
        const uchar* arow = a.constScanLine(y);
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < w; ++x)
            if (grow[x]) orow[x] = arow[x];
    }
    return out;
}


// Separable running-sum box mean over a w*h float plane (declared here so the
// decontamination below can reuse it for its colour diffusion).
static void boxMeanPlane(QVector<float>& data, int w, int h, int r);

// Decontaminate a straight (unpremultiplied) RGBA cutout: remove the background
// colour that bleeds into the semi-transparent edge / hair-wisp band.
//
// Follows the industry approach (Photoshop/Affinity "decontaminate colours"):
// edge pixels take the colour of NEARBY CONFIDENT FOREGROUND instead of trying
// to unmix the background out. Unmixing (fg = (observed - bg*(1-a))/a) is
// ill-conditioned where alpha is small (1/a noise gain → rainbow speckle) and
// wrong wherever alpha itself is overstated (it then faithfully preserves the
// contaminant); replacement needs no division and no background estimate, so
// neither failure mode exists. A weighted colour diffusion (push-pull
// inpainting) propagates confident subject colour outward; each edge pixel
// blends toward it by `amount` — full blend through most of the band, fading
// to zero at the confident interior so solid detail is untouched and no seam
// appears. Alpha is never modified; only colour is corrected.
bool decontaminateStraightRgba(QImage& straight, const QImage& coverage,
                               int radius, double amount) {
    if (straight.isNull() || coverage.isNull()) return false;
    if (straight.size() != coverage.size()) return false;
    if (straight.format() != QImage::Format_RGBA8888)
        straight = straight.convertToFormat(QImage::Format_RGBA8888);
    if (straight.isNull()) return false;
    // Clamp generously: callers pass proxy-relative radii (tuned at proxyCap,
    // scaled by 1/proxyScale for full-res delivery), so the ceiling must clear
    // ~3x the tuned proxy value. Cost is radius-independent (running sums).
    radius = std::clamp(radius, 2, 128);
    amount = std::clamp(amount, 0.0, 1.0);
    const int w = straight.width(), h = straight.height();
    const QImage cov =
        coverage.format() == QImage::Format_Grayscale8
            ? coverage
            : coverage.convertToFormat(QImage::Format_Grayscale8);

    const int n = w * h;
    constexpr int kKnown = 230;  // confident-foreground coverage threshold
    // Foreground estimate sources (confident subject) for the edge fill.
    QVector<float> wr(n), rr(n), gr(n), br(n);
    bool anyKnown = false;
    bool anyEdge = false;
    for (int y = 0; y < h; ++y) {
        const uchar* crow = cov.constScanLine(y);
        const uchar* srow = straight.constScanLine(y);
        const int row = y * w;
        for (int x = 0; x < w; ++x) {
            const int i = row + x;
            const int c = crow[x];
            if (c >= kKnown) {
                wr[i] = 1.0f;
                rr[i] = static_cast<float>(srow[x * 4 + 0]);
                gr[i] = static_cast<float>(srow[x * 4 + 1]);
                br[i] = static_cast<float>(srow[x * 4 + 2]);
                anyKnown = true;
            } else if (c > 0) {
                anyEdge = true;
            }
        }
    }
    if (!anyKnown || !anyEdge) return false;

    // Diffuse the known colours outward. Two blur scales: a small radius for
    // fine strands and a larger one to carry colour across wider gaps, summed
    // so short strands keep detail while isolated wisps still get filled.
    const int r1 = std::clamp(radius / 2, 2, 12);
    const int r2 = std::clamp(radius, 6, 32);
    // Blur weight and each colour channel by r1, accumulate, then by r2.
    boxMeanPlane(wr, w, h, r1);
    boxMeanPlane(rr, w, h, r1);
    boxMeanPlane(gr, w, h, r1);
    boxMeanPlane(br, w, h, r1);
    // (boxMeanPlane is a plain mean; wr now holds mean-weight in [0,1].)
    // Second, wider pass on top for robustness across broader wisps.
    boxMeanPlane(wr, w, h, r2 - r1 > 0 ? r2 - r1 : 1);
    boxMeanPlane(rr, w, h, r2 - r1 > 0 ? r2 - r1 : 1);
    boxMeanPlane(gr, w, h, r2 - r1 > 0 ? r2 - r1 : 1);
    boxMeanPlane(br, w, h, r2 - r1 > 0 ? r2 - r1 : 1);

    // Write back: recolour the edge band from the diffused foreground.
    // Full `amount` blend through most of the band, fading linearly to zero
    // at the confident interior — solid detail is never touched and no seam
    // appears where the correction ends.
    constexpr int kFull = 190;  // full blend at/below this coverage
    bool touched = false;
    for (int y = 0; y < h; ++y) {
        const uchar* crow = cov.constScanLine(y);
        uchar* srow = straight.scanLine(y);
        const int row = y * w;
        for (int x = 0; x < w; ++x) {
            const int c = crow[x];
            if (c <= 0 || c >= kKnown) continue;  // background or confident fg
            const int i = row + x;
            const float wv = wr[i];
            if (wv <= 1e-3f) continue;  // no known colour reached here
            double t = amount;
            if (c > kFull)
                t *= double(kKnown - c) / double(kKnown - kFull);
            uchar* px = srow + x * 4;
            px[0] = static_cast<uchar>(std::clamp(
                int(px[0] + t * (rr[i] / wv - px[0]) + 0.5), 0, 255));
            px[1] = static_cast<uchar>(std::clamp(
                int(px[1] + t * (gr[i] / wv - px[1]) + 0.5), 0, 255));
            px[2] = static_cast<uchar>(std::clamp(
                int(px[2] + t * (br[i] / wv - px[2]) + 0.5), 0, 255));
            touched = true;
        }
    }
    return touched;
}


// Separable running-sum box mean over a w*h float plane, in place: each
// pixel ends up holding the average of its +-r neighbourhood (edges clamp).
static void boxMeanPlane(QVector<float>& data, int w, int h, int r) {
    if (r <= 0 || w <= 0 || h <= 0) return;
    QVector<float> tmp(w * h);
    for (int y = 0; y < h; ++y) {
        const int row = y * w;
        double sum = 0.0;
        for (int i = 0; i <= std::min(r, w - 1); ++i) sum += data[row + i];
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0, x - r);
            const int hi = std::min(w - 1, x + r);
            tmp[row + x] = static_cast<float>(sum / (hi - lo + 1));
            if (x >= r) sum -= data[row + (x - r)];
            if (x + r + 1 < w) sum += data[row + x + r + 1];
        }
    }
    for (int x = 0; x < w; ++x) {
        double sum = 0.0;
        for (int i = 0; i <= std::min(r, h - 1); ++i) sum += tmp[i * w + x];
        for (int y = 0; y < h; ++y) {
            const int lo = std::max(0, y - r);
            const int hi = std::min(h - 1, y + r);
            data[y * w + x] = static_cast<float>(sum / (hi - lo + 1));
            if (y >= r) sum -= tmp[(y - r) * w + x];
            if (y + r + 1 < h) sum += tmp[(y + r + 1) * w + x];
        }
    }
}

QImage selectionMaskMatteSolve(const QImage& color, const QImage& mask,
                               const QPointF& center, double radius) {
    if (color.isNull() || mask.isNull() || radius <= 0.0) return mask;
    if (color.size() != mask.size()) return mask;
    const QImage argb =
        color.format() == QImage::Format_ARGB32
            ? color
            : color.convertToFormat(QImage::Format_ARGB32);
    if (argb.isNull()) return mask;
    const int w = mask.width(), h = mask.height();
    // Every pixel gets its OWN foreground/background models: box means over
    // the confident (>=200 / <=55) guide pixels in a 1.5x radius
    // neighbourhood. A pixel with only one side in range skips itself —
    // deep interior is never stamped uncertain and a loose overshoot near
    // the edge still reaches the far side without widening the search.
    // 1.5x the brush radius, capped at 16 doc px: the local fg/bg models
    // must stay local. An uncapped box on a big brush mixes the whole
    // stroke neighbourhood into the means and the projection answer stops
    // meaning anything.
    const int sampleR =
        std::max(1, std::min(int(std::ceil(radius * 1.5)), 16));
    const double window = double(2 * sampleR + 1) * double(2 * sampleR + 1);
    const int rx0 = std::max(0, int(center.x() - radius - sampleR - 1.0));
    const int rx1 = std::min(w - 1, int(center.x() + radius + sampleR + 1.0));
    const int ry0 = std::max(0, int(center.y() - radius - sampleR - 1.0));
    const int ry1 = std::min(h - 1, int(center.y() + radius + sampleR + 1.0));
    const int rw = rx1 - rx0 + 1, rh = ry1 - ry0 + 1;
    if (rw <= 0 || rh <= 0) return mask;
    // Planes: [0]=fg coverage mean, [1]=bg coverage mean, [2..4]=fg colour,
    // [5..7]=bg colour. Colour planes are the sum of confident-pixel values,
    // so dividing by the matching coverage mean recovers the local mean
    // colour of that side's samples only.
    auto build = [&](const QImage& src, QVector<float> p[8]) {
        for (int i = 0; i < 8; ++i) p[i].fill(0.0f, rw * rh);
        for (int y = ry0; y <= ry1; ++y) {
            const uchar* srow = src.constScanLine(y);
            const QRgb* crow =
                reinterpret_cast<const QRgb*>(argb.constScanLine(y));
            const int base = (y - ry0) * rw;
            for (int x = rx0; x <= rx1; ++x) {
                const QRgb px = crow[x];
                if (qAlpha(px) < 16) continue;  // transparent: no model vote
                const int i = base + (x - rx0);
                const uchar m = srow[x];
                if (m >= 200) {
                    p[0][i] = 1.0f;
                    p[2][i] = static_cast<float>(qRed(px));
                    p[3][i] = static_cast<float>(qGreen(px));
                    p[4][i] = static_cast<float>(qBlue(px));
                } else if (m <= 55) {
                    p[1][i] = 1.0f;
                    p[5][i] = static_cast<float>(qRed(px));
                    p[6][i] = static_cast<float>(qGreen(px));
                    p[7][i] = static_cast<float>(qBlue(px));
                }
            }
        }
        for (int i = 0; i < 8; ++i) boxMeanPlane(p[i], rw, rh, sampleR);
    };
    const int x0 = std::max(0, int(center.x() - radius - 1.0));
    const int x1 = std::min(w - 1, int(center.x() + radius + 1.0));
    const int y0 = std::max(0, int(center.y() - radius - 1.0));
    const int y1 = std::min(h - 1, int(center.y() + radius + 1.0));
    // Coverage = projection of the pixel colour onto the local
    // foreground/background colour line (color-line alpha): 1 at the
    // foreground mean, 0 at the background mean, linear in between. Pixels
    // whose box lacks either side, or whose two mean colours sit too close
    // together to tell apart, keep their current coverage — with no
    // trustworthy model the honest result is the seed itself, never an
    // uncertainty stamp.
    QImage trial = mask;
    // Nearest-confident search: when the local box starves, walk square
    // rings outward to the closest >=200 / <=55 samples instead of projecting
    // against whole-image averages (which smear coat, skin, hair and backdrop
    // into mud). Bounded so vast soft regions bail out and keep their seed.
    auto nearestModels = [&](int x, int y, int rMax, double f[3],
                             double b[3]) {
        double fs[3] = {}, bs[3] = {};
        int fn = 0, bn = 0;
        const int w = mask.width(), h = mask.height();
        auto visit = [&](int nx, int ny) {
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) return;
            const QRgb px = reinterpret_cast<const QRgb*>(
                argb.constScanLine(ny))[nx];
            if (qAlpha(px) < 16) return;
            const uchar m = trial.constScanLine(ny)[nx];
            if (m >= 200 && fn < 16) {
                fs[0] += qRed(px);
                fs[1] += qGreen(px);
                fs[2] += qBlue(px);
                ++fn;
            } else if (m <= 55 && bn < 16) {
                bs[0] += qRed(px);
                bs[1] += qGreen(px);
                bs[2] += qBlue(px);
                ++bn;
            }
        };
        for (int r = 1; r <= rMax && (fn < 8 || bn < 8); ++r) {
            for (int dx = -r; dx <= r && (fn < 8 || bn < 8); ++dx) {
                visit(x + dx, y - r);
                visit(x + dx, y + r);
            }
            for (int dy = -r + 1; dy <= r - 1 && (fn < 8 || bn < 8);
                 ++dy) {
                visit(x - r, y + dy);
                visit(x + r, y + dy);
            }
        }
        if (fn < 8 || bn < 8) return false;
        for (int c = 0; c < 3; ++c) {
            f[c] = fs[c] / fn;
            b[c] = bs[c] / bn;
        }
        return true;
    };
    const int rMaxSeek = std::max(16, std::min(int(radius * 2 + 16), 64));
    auto solve = [&](const QVector<float> p[8]) {
        for (int y = y0; y <= y1; ++y) {
            const QRgb* crow =
                reinterpret_cast<const QRgb*>(argb.constScanLine(y));
            uchar* row = trial.scanLine(y);
            for (int x = x0; x <= x1; ++x) {
                const double dx = x + 0.5 - center.x();
                const double dy = y + 0.5 - center.y();
                if (dx * dx + dy * dy > radius * radius) continue;
                const int i = (y - ry0) * rw + (x - rx0);
                const double fn = p[0][i], bn = p[1][i];
                double fR, fG, fB, bR, bG, bB;
                if (fn * window >= 8.0 && bn * window >= 8.0) {
                    fR = p[2][i] / fn;
                    fG = p[3][i] / fn;
                    fB = p[4][i] / fn;
                    bR = p[5][i] / bn;
                    bG = p[6][i] / bn;
                    bB = p[7][i] / bn;
                } else {
                    double nf[3], nb[3];
                    if (!nearestModels(x, y, rMaxSeek, nf, nb)) continue;
                    fR = nf[0];
                    fG = nf[1];
                    fB = nf[2];
                    bR = nb[0];
                    bG = nb[1];
                    bB = nb[2];
                }
                const QRgb px = crow[x];
                const double lR = fR - bR, lG = fG - bG, lB = fB - bB;
                const double sep = lR * lR + lG * lG + lB * lB;
                if (sep < 12.0 * 12.0) continue;  // colours too close to tell
                const double cr = qRed(px) - bR, cg = qGreen(px) - bG,
                             cb = qBlue(px) - bB;
                const double dist2b = cr * cr + cg * cg + cb * cb;
                const double dot = cr * lR + cg * lG + cb * lB;
                const double alpha = std::clamp(dot / (sep + 1e-9), 0.0, 1.0);
                const double solved = alpha * 255.0;
                const double seed = row[x];
                // Asymmetric evidence: coverage may RISE freely (wisps pull
                // in) but may only FALL when the pixel actually reads as
                // background — its colour must sit within 12 RMS of the
                // local bg mean. Two failure modes this blocks: model noise
                // punching holes in solid interiors (stippled faces), and
                // subject pixels that wear the backdrop's rim/cast colour
                // (backlit ears, wet hair, coat edges) reading as wall.
                // Soft seeds get the same protection: a feathered edge used
                // to be fully re-solved by colour and eroded away.
                if (dist2b > 3.0 * 12.0 * 12.0 && solved + 0.5 < seed) {
                    row[x] = static_cast<uchar>(int(seed));
                } else {
                    row[x] = static_cast<uchar>(int(solved + 0.5));
                }
            }
        }
    };
    QVector<float> planes[8];
    build(mask, planes);
    solve(planes);
    // Re-estimate from the trial's own confident pixels: once the first
    // round has cleared an overshoot the models lose their pollution, so
    // the second pass separates hair from backdrop cleanly.
    build(trial, planes);
    solve(planes);
    // Blend by the soft disc falloff so strokes never leave rims.
    QImage out = mask;
    for (int y = y0; y <= y1; ++y) {
        const uchar* trow = trial.constScanLine(y);
        uchar* row = out.scanLine(y);
        for (int x = x0; x <= x1; ++x) {
            const double dx = x + 0.5 - center.x();
            const double dy = y + 0.5 - center.y();
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d > radius) continue;
            const double fall = std::clamp(1.0 - d / radius, 0.0, 1.0);
            row[x] = static_cast<uchar>(std::clamp(
                int(trow[x] * fall + row[x] * (1.0 - fall) + 0.5), 0, 255));
        }
    }
    return out;
}

namespace {

// Softness of the mask's edge for the smart-radius blend. Transition pixels
// (0 < v < 255) score the local width of their ramp — the fraction of
// mid-tones in a small box, high over hair/fur, ~0 at a hard step. The best
// score propagates outward up to `reach` px (plateau, then a linear fade at
// the end), so the wide treatment is licensed near a soft edge and never
// near a hard one. (A purely local score dies a few px past the narrow gate
// and the wide band would never fire; propagation is the same chamfer
// pattern the wash decay uses.)
QImage edgeSoftnessMap(const QImage& mask, int reach) {
    constexpr int kRampBox = 4;
    const int w = mask.width(), h = mask.height();
    QVector<float> f(w * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        const uchar* srow = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const int v = srow[x];
            f[y * w + x] = (v > 8 && v < 247) ? 1.0f : 0.0f;
        }
    }
    boxMeanPlane(f, w, h, kRampBox);
    // Seeds are the ramp's own mid-tone pixels; the box density scores how
    // wide the ramp is (a 1px-antialiased step → ~0 → stays hard; a real
    // hair ramp → 1). Seeding non-ramp pixels instead would let their low
    // local density cap the score right where the ramp ends — precisely
    // where the wide band is needed.
    const int cap = reach + 1;
    QVector<int> dist(w * h, cap);
    QVector<float> src(w * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        const uchar* srow = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (srow[x] <= 8 || srow[x] >= 247) continue;
            const int i = y * w + x;
            dist[i] = 0;
            src[i] = static_cast<float>(
                std::clamp((f[i] - 0.1) / 0.4, 0.0, 1.0));
        }
    }
    // Chamfer propagation of (distance to the nearest ramp pixel, that
    // pixel's score), capped at reach + 1.
    auto relax = [&](int cur, int nb) {
        const int nd = dist[nb] + 1;
        if (nd < dist[cur]) {
            dist[cur] = nd;
            src[cur] = src[nb];
        }
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (x > 0) relax(i, i - 1);
            if (y > 0) relax(i, i - w);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const int i = y * w + x;
            if (x + 1 < w) relax(i, i + 1);
            if (y + 1 < h) relax(i, i + w);
        }
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            // Plateau then fade: full score across the first 2/3 of the
            // reach (the wide band must actually fire just past the ramp,
            // where the narrow gate stops), linear fade over the last third
            // so no step appears where the influence ends.
            const double fall =
                std::clamp(3.0 * (reach - dist[i]) / reach, 0.0, 1.0);
            orow[x] = static_cast<uchar>(
                std::clamp(double(src[i]) * fall, 0.0, 1.0) * 255.0 + 0.5);
        }
    }
    return out;
}

}  // namespace

QImage selectionMaskSnapSmart(const QImage& guide, const QImage& mask,
                              int narrowPx, int widePx, int passes) {
    if (guide.isNull() || mask.isNull() || guide.size() != mask.size())
        return mask;
    if (narrowPx <= 0) return mask;
    if (widePx <= narrowPx)
        return selectionMaskSnapToEdges(guide, mask, narrowPx, passes);
    const QImage narrow = selectionMaskSnapToEdges(guide, mask, narrowPx, passes);
    const QImage wide = selectionMaskSnapToEdges(guide, mask, widePx, passes);
    const QImage soft = edgeSoftnessMap(mask, widePx);
    const int w = mask.width(), h = mask.height();
    QImage out = narrow;
    for (int y = 0; y < h; ++y) {
        const uchar* srow = soft.constScanLine(y);
        const uchar* wrow = wide.constScanLine(y);
        const uchar* nrow = narrow.constScanLine(y);
        uchar* orow = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int s = srow[x];
            orow[x] = static_cast<uchar>(
                (wrow[x] * s + nrow[x] * (255 - s) + 127) / 255);
        }
    }
    return out;
}

}  // namespace pittore::ui