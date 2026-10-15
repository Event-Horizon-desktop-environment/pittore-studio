// clip-path on a <g>: the clip applies to the whole subtree, so descendant
// rasters must be cut by it (intersected with any clip of their own), and the
// retained art must carry the effective clip so the canvas live-repaint vetoes
// and keeps the clipped bake. Headless (offscreen), small synthetics only.
#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdio>

#include "test_util.h"
#include "ui/svg_parts.h"

using namespace pittore::ui;

namespace {

// A disc with four bars laid across it, each bar wider than the disc, all
// inside one clipped group. Unclipped, the bars stick out past the disc on
// both sides; clipped, nothing paints outside the circle.
QByteArray groupClipSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<defs>"
        "<clipPath id=\"discClip\"><circle cx=\"60\" cy=\"60\" r=\"40\"/></clipPath>"
        "<clipPath id=\"leftHalf\"><rect x=\"0\" y=\"0\" width=\"60\" "
        "height=\"120\"/></clipPath>"
        "</defs>"
        "<g clip-path=\"url(#discClip)\">"
        "<circle id=\"disc\" cx=\"60\" cy=\"60\" r=\"40\" fill=\"#3050c0\"/>"
        "<rect id=\"bar1\" x=\"0\" y=\"40\" width=\"120\" height=\"8\" "
        "fill=\"#202020\"/>"
        "<rect id=\"bar2\" x=\"0\" y=\"56\" width=\"120\" height=\"10\" "
        "fill=\"#202020\"/>"
        "<rect id=\"bar3\" x=\"0\" y=\"74\" width=\"120\" height=\"12\" "
        "fill=\"#202020\"/>"
        "<rect id=\"bar4\" x=\"0\" y=\"94\" width=\"120\" height=\"14\" "
        "fill=\"#202020\"/>"
        "</g>"
        "</svg>");
}

// A leaf carrying its own clip inside a clipped group: the two must intersect
// (group disc minus the leaf's left half = right half of the disc).
QByteArray nestedClipSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<defs>"
        "<clipPath id=\"discClip\"><circle cx=\"60\" cy=\"60\" r=\"40\"/></clipPath>"
        "<clipPath id=\"leftHalf\"><rect x=\"0\" y=\"0\" width=\"60\" "
        "height=\"120\"/></clipPath>"
        "</defs>"
        "<g clip-path=\"url(#discClip)\">"
        "<rect id=\"both\" x=\"0\" y=\"0\" width=\"120\" height=\"120\" "
        "clip-path=\"url(#leftHalf)\" fill=\"#c04030\"/>"
        "</g>"
        "</svg>");
}

// The group transform maps the clip's user space too: a disc clip inside a
// translate(40 0) group lands 40 doc-units right of where it is authored.
QByteArray transformedGroupClipSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"160\" height=\"120\">"
        "<defs>"
        "<clipPath id=\"discClip\"><circle cx=\"40\" cy=\"60\" r=\"30\"/></clipPath>"
        "</defs>"
        "<g clip-path=\"url(#discClip)\" transform=\"translate(40 0)\">"
        "<rect id=\"wide\" x=\"0\" y=\"0\" width=\"160\" height=\"120\" "
        "fill=\"#208040\"/>"
        "</g>"
        "</svg>");
}

int painted(const QImage& img) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) > 8) ++n;
    return n;
}

const SvgPartLayer* findLayer(const SvgImportResult& r, const QString& name) {
    for (const SvgPartLayer& l : r.layers)
        if (l.name == name && !l.group) return &l;
    return nullptr;
}

// Composite every non-group layer of `r` at document scale onto one image, so
// the clip can be judged across layers (the reported bug split the clipped art
// over several rows, each rasterized on its own).
QImage composite(const SvgImportResult& r) {
    QImage out(r.docSize, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    for (int i = r.layers.size() - 1; i >= 0; --i) {
        const SvgPartLayer& l = r.layers[i];
        if (l.group || l.pixels.isNull()) continue;
        p.setOpacity(l.opacity);
        p.drawImage(l.offset, l.pixels);
    }
    p.end();
    return out;
}

// Count opaque pixels far outside the clip circle: at doc coords beyond
// r + margin from the centre, where the clip allows nothing.
int outsideClipPaint(const QImage& img, double cx, double cy, double r) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) <= 8) continue;
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            if (std::sqrt(dx * dx + dy * dy) > r + 2.0) ++n;
        }
    return n;
}

void test_group_clip_cuts_descendants() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(groupClipSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());

    const SvgPartLayer* bar = findLayer(r, QStringLiteral("bar1"));
    CHECK(bar != nullptr);
    if (!bar) return;
    // The clip cuts at paint time inside the bar's own bounds (bounds are
    // the path box, not clip-trimmed): the painted span must be the disc's
    // width, not the authored 120.
    const int barPx = painted(bar->pixels);
    CHECK(barPx > 300);
    CHECK(barPx < 800);  // full-width bar would paint 960
    // And the retained art must carry the clip so the canvas live-repaint
    // (which cannot re-apply a clip) stays vetoed.
    CHECK(bar->art != nullptr);
    if (bar->art) CHECK(!bar->art->paint.clipId.empty());

    // Across the whole composite, nothing may paint outside the clip circle:
    // the reported bug let the bars run past the disc on both sides.
    const QImage img = composite(r);
    CHECK_EQ(outsideClipPaint(img, 60.0, 60.0, 40.0), 0);
    // ...while the clipped art itself still paints plenty inside it.
    CHECK(painted(img) > 2000);
}

void test_leaf_clip_intersects_group_clip() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(nestedClipSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());

    const SvgPartLayer* leaf = findLayer(r, QStringLiteral("both"));
    CHECK(leaf != nullptr);
    if (!leaf) return;
    const QImage img = composite(r);
    // Outside the group's circle: nothing.
    CHECK_EQ(outsideClipPaint(img, 60.0, 60.0, 40.0), 0);
    // The leaf's own clip (left half rect) intersects the group's circle:
    // the left half of the disc paints, the right half stays empty.
    int leftHalf = 0, rightHalf = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) <= 8) continue;
            if (x < 60) ++leftHalf; else ++rightHalf;
        }
    CHECK_EQ(rightHalf, 0);
    CHECK(leftHalf > 1000);
}

void test_group_transform_maps_clip() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(transformedGroupClipSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());

    const QImage img = composite(r);
    // The clip is authored at (40,60) and rides the group's translate(40 0):
    // it must clip at (80,60), leaving the strip left of x=10 empty.
    CHECK_EQ(outsideClipPaint(img, 80.0, 60.0, 30.0), 0);
    int leftStrip = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < 10; ++x)
            if (qAlpha(img.pixel(x, y)) > 8) ++leftStrip;
    CHECK_EQ(leftStrip, 0);
    CHECK(painted(img) > 1000);
}

void test_unclipped_leaf_unaffected() {
    // Same shapes with no clip anywhere: the bars must reach the edges, so
    // the clip fix only bites when a clip is actually in effect.
    const QByteArray svg = QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<g>"
        "<rect id=\"bar\" x=\"0\" y=\"40\" width=\"120\" height=\"8\" "
        "fill=\"#202020\"/>"
        "</g>"
        "</svg>");
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(svg, &r, &dpi, &error));
    CHECK(error.isEmpty());
    const SvgPartLayer* bar = findLayer(r, QStringLiteral("bar"));
    CHECK(bar != nullptr);
    if (!bar) return;
    CHECK(bar->pixels.width() >= 120);
    CHECK(bar->art != nullptr);
    if (bar->art) CHECK(bar->art->paint.clipId.empty());
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    test_group_clip_cuts_descendants();
    test_leaf_clip_intersects_group_clip();
    test_group_transform_maps_clip();
    test_unclipped_leaf_unaffected();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
