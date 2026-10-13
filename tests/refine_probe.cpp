// refine_probe.cpp — temporary (NOT a suite test). Stage-by-stage harshness
// measurement for the Refine pipeline: builds a synthetic hair phantom with
// analytic ground-truth alpha, runs each stage, and reports max adjacent step
// (stair detector) plus MAE vs ground truth. Purpose: find which stage
// imprints blocky edges.
//
// Usage: refine_probe
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ui/selection_mask.h"

namespace {
using namespace pittore::ui;

// Phantom: vertical soft edge (sigmoid, ~5px transition) + two 1px diagonal
// strands, dark hair over teal backdrop. Ground truth = analytic coverage.
struct Phantom {
    QImage guide;  // ARGB32
    QImage input;  // Grayscale8: the edge as a WIDE soft ramp (sloppy AI)
    QImage gt;     // Grayscale8: the true tight edge
    int w = 0, h = 0;
};

Phantom makePhantom() {
    Phantom p;
    p.w = 160;
    p.h = 120;
    p.guide = QImage(p.w, p.h, QImage::Format_ARGB32);
    p.input = QImage(p.w, p.h, QImage::Format_Grayscale8);
    p.gt = QImage(p.w, p.h, QImage::Format_Grayscale8);
    for (int y = 0; y < p.h; ++y) {
        QRgb* grow = reinterpret_cast<QRgb*>(p.guide.scanLine(y));
        uchar* irow = p.input.scanLine(y);
        uchar* trow = p.gt.scanLine(y);
        for (int x = 0; x < p.w; ++x) {
            // True edge: sigmoid centred at x=80, width ~5px.
            const double t = (x - 80.0) / 2.5;
            const double cov = 1.0 / (1.0 + std::exp(t));
            // Strand: 1px diagonal x-y=40, soft profile.
            const double d = std::abs(x - y - 40);
            const double strand = std::clamp(1.0 - d / 1.5, 0.0, 1.0);
            const double hair = std::clamp(cov + strand * (1.0 - cov), 0.0, 1.0);
            const int v = int(hair * 255.0 + 0.5);
            // Input: WIDE sloppy ramp (what SAM hands over), strands dropped.
            const double wide = 1.0 / (1.0 + std::exp((x - 80.0) / 9.0));
            const int wv = int(wide * 255.0 + 0.5);
            // Guide colours: dark hair vs teal backdrop.
            const int r = int(25 + (60 - 25) * (1.0 - cov) + 0.5);
            const int g = int(25 + (95 - 25) * (1.0 - cov) + 0.5);
            const int b = int(30 + (105 - 30) * (1.0 - cov) + 0.5);
            grow[x] = qRgba(r, g, b, 255);
            irow[x] = static_cast<uchar>(wv);
            trow[x] = static_cast<uchar>(v);
        }
    }
    return p;
}

double maxStepAt(const QImage& m, int& ax, int& ay, int& av, int& bv) {
    double mx = 0.0;
    ax = ay = av = bv = 0;
    const int w = m.width(), h = m.height();
    // Skip the 2px image border: clamped neighborhoods there are degenerate
    // for every solver and don't represent real edge behavior.
    for (int y = 2; y < h - 2; ++y) {
        const uchar* row = m.constScanLine(y);
        for (int x = 3; x < w - 2; ++x) {
            const double d = double(std::abs(int(row[x]) - int(row[x - 1])));
            if (d > mx) {
                mx = d;
                ax = x;
                ay = y;
                av = row[x - 1];
                bv = row[x];
            }
        }
    }
    for (int y = 3; y < h - 2; ++y) {
        const uchar* a = m.constScanLine(y - 1);
        const uchar* b = m.constScanLine(y);
        for (int x = 2; x < w - 2; ++x) {
            const double d = double(std::abs(int(b[x]) - int(a[x])));
            if (d > mx) {
                mx = d;
                ax = x;
                ay = y;
                av = a[x];
                bv = b[x];
            }
        }
    }
    return mx;
}

double mae(const QImage& m, const QImage& gt) {
    double sum = 0.0;
    const int w = m.width(), h = m.height();
    for (int y = 0; y < h; ++y) {
        const uchar* a = m.constScanLine(y);
        const uchar* b = gt.constScanLine(y);
        for (int x = 0; x < w; ++x) sum += std::abs(int(a[x]) - int(b[x]));
    }
    return sum / (w * h);
}

void report(const char* stage, const QImage& m, const QImage& gt) {
    int ax, ay, av, bv;
    const double ms = maxStepAt(m, ax, ay, av, bv);
    std::printf("%-14s maxStep=%5.0f @(%d,%d %d->%d) mae=%6.2f\n", stage, ms,
                ax, ay, av, bv, mae(m, gt));
}
}  // namespace

int main() {
    Phantom p = makePhantom();
    report("input", p.input, p.gt);
    QImage m = selectionMaskRemoveSpeckles(p.input, 8);
    report("despeckle", m, p.gt);
    m = selectionMaskDecayWash(m, p.guide, 24);
    report("decay", m, p.gt);
    m = selectionMaskSnapToEdges(p.guide, m, 24, 3);
    report("snap", m, p.gt);
    m = selectionMaskSmooth(m, 2);
    report("smooth", m, p.gt);
    m = selectionMaskMatteSolve(p.guide, m, QPointF(60, 60), 12.0);
    report("matte-brush", m, p.gt);
    QImage base = m;
    m = selectionMaskGrow(base, -4);
    report("shift-4", m, p.gt);
    m = selectionMaskGrow(base, 4);
    report("shift+4", m, p.gt);
    return 0;
}
