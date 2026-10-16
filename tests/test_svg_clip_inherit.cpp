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

// display="none" culls the element and its subtree, visibility hidden (and
// inherited hidden) paints nothing, and display:none also arrives through
// the stylesheet pre-pass. Honored content: no rows, verdict stays clean.
QByteArray displayNoneSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<style>.gone { display: none; }</style>"
        "<g id=\"hidden-group\" display=\"none\">"
        "<rect id=\"h1\" x=\"0\" y=\"0\" width=\"120\" height=\"120\" "
        "fill=\"#ff0000\"/>"
        "</g>"
        "<g id=\"hgroup\" visibility=\"hidden\">"
        "<rect id=\"hchild\" x=\"0\" y=\"0\" width=\"60\" height=\"60\" "
        "fill=\"#ff0000\"/>"
        "</g>"
        "<rect id=\"shown\" x=\"0\" y=\"0\" width=\"60\" height=\"60\" "
        "fill=\"#00ff00\"/>"
        "<rect id=\"styled\" class=\"gone\" x=\"60\" y=\"60\" width=\"60\" "
        "height=\"60\" fill=\"#0000ff\"/>"
        "<rect id=\"invis\" x=\"0\" y=\"60\" width=\"60\" height=\"60\" "
        "fill=\"#ff00ff\" visibility=\"hidden\"/>"
        "</svg>");
}

void test_display_none_culls_subtree() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(displayNoneSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());
    CHECK(findLayer(r, QStringLiteral("hidden-group")) == nullptr);
    CHECK(findLayer(r, QStringLiteral("h1")) == nullptr);
    CHECK(findLayer(r, QStringLiteral("hgroup")) == nullptr);
    CHECK(findLayer(r, QStringLiteral("hchild")) == nullptr);
    CHECK(findLayer(r, QStringLiteral("styled")) == nullptr);
    CHECK(findLayer(r, QStringLiteral("invis")) == nullptr);
    const SvgPartLayer* shown = findLayer(r, QStringLiteral("shown"));
    CHECK(shown != nullptr);
    if (!shown) return;
    CHECK(painted(shown->pixels) > 1000);
    // Only the shown leaf survives as a pixel row.
    int leafRows = 0;
    for (const SvgPartLayer& l : r.layers)
        if (!l.group && !l.pixels.isNull()) ++leafRows;
    CHECK_EQ(leafRows, 1);
}

// clip-rule governs clip children (fill-rule is ignored there): an evenodd
//nested square knocks a hole, the same geometry as nonzero stays solid.
QByteArray clipRuleSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"250\" height=\"120\">"
        "<defs>"
        "<clipPath id=\"hole\"><path d=\"M10 10 H110 V110 H10 Z "
        "M40 40 H80 V80 H40 Z\" clip-rule=\"evenodd\"/></clipPath>"
        "<clipPath id=\"solid\"><path d=\"M140 10 H240 V110 H140 Z "
        "M170 40 H210 V80 H170 Z\" clip-rule=\"nonzero\"/></clipPath>"
        "</defs>"
        "<rect id=\"donut\" x=\"10\" y=\"10\" width=\"100\" height=\"100\" "
        "clip-path=\"url(#hole)\" fill=\"#c04030\"/>"
        "<rect id=\"full\" x=\"140\" y=\"10\" width=\"100\" height=\"100\" "
        "clip-path=\"url(#solid)\" fill=\"#3050c0\"/>"
        "</svg>");
}

void test_clip_rule_evenodd_cuts_hole() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(clipRuleSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());
    CHECK(findLayer(r, QStringLiteral("donut")) != nullptr);
    CHECK(findLayer(r, QStringLiteral("full")) != nullptr);
    const QImage img = composite(r);
    // Donut: painted rim, empty hole.
    CHECK(qAlpha(img.pixel(20, 20)) > 8);
    CHECK_EQ(qAlpha(img.pixel(60, 60)), 0);
    // Nonzero with same winding: the inner square stays painted.
    CHECK(qAlpha(img.pixel(190, 60)) > 8);
}

// <a> renders its children like a group; linking has no raster effect.
QByteArray linkGroupSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<a id=\"link\" xlink:href=\"https://example.invalid/\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
        "<rect id=\"linked\" x=\"10\" y=\"10\" width=\"100\" height=\"100\" "
        "fill=\"#c04030\"/>"
        "</a>"
        "</svg>");
}

void test_link_group_renders_children() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(linkGroupSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());
    const SvgPartLayer* linked = findLayer(r, QStringLiteral("linked"));
    CHECK(linked != nullptr);
    if (!linked) return;
    CHECK(painted(linked->pixels) > 1000);
}

// Non-rendered and timed elements are honored silently: metadata, font and
// color definitions, base-frame animation, and the consumed <style> leave
// no skip or dangling counts behind.
QByteArray silentTagsSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<style>.t { fill: #3050c0; }</style>"
        "<title>silent</title>"
        "<desc>silent</desc>"
        "<metadata>silent</metadata>"
        "<rect id=\"box\" class=\"t\" x=\"10\" y=\"10\" width=\"100\" "
        "height=\"100\">"
        "<animate attributeName=\"x\" from=\"10\" to=\"20\" dur=\"1s\"/>"
        "</rect>"
        "<animate attributeName=\"opacity\" values=\"1;0\" dur=\"1s\"/>"
        "<cursor/>"
        "</svg>");
}

void test_silent_tags_stay_clean() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(silentTagsSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());
    CHECK(r.diag.skippedLeaves == 0);
    CHECK(r.diag.skipTags.isEmpty());
    CHECK(r.diag.missingRefs.isEmpty());
    CHECK_EQ(r.diag.imagesDropped, 0);
    const SvgPartLayer* box = findLayer(r, QStringLiteral("box"));
    CHECK(box != nullptr);
    if (!box) return;
    CHECK(painted(box->pixels) > 1000);
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

// <switch> walks the first child whose conditions hold; untaken siblings
// are alternatives, not loss. An all-failing switch is counted once.
QByteArray switchSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<switch id=\"sw\">"
        "<rect id=\"animated\" x=\"10\" y=\"10\" width=\"100\" height=\"100\" "
        "fill=\"#ff0000\" requiredFeatures=\"http://www.w3.org/TR/SVG11/"
        "feature#Animation\"/>"
        "<rect id=\"fallback\" x=\"10\" y=\"10\" width=\"100\" height=\"100\" "
        "fill=\"#00ff00\"/>"
        "</switch>"
        "<switch id=\"empty\">"
        "<rect id=\"never\" x=\"10\" y=\"10\" width=\"100\" height=\"100\" "
        "fill=\"#0000ff\" requiredFeatures=\"http://www.w3.org/TR/SVG11/"
        "feature#Script\"/>"
        "</switch>"
        "</svg>");
}

void test_switch_picks_first_passing_child() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(switchSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());
    CHECK(findLayer(r, QStringLiteral("animated")) == nullptr);
    const SvgPartLayer* fallback = findLayer(r, QStringLiteral("fallback"));
    CHECK(fallback != nullptr);
    if (!fallback) return;
    CHECK(painted(fallback->pixels) > 1000);
    CHECK(findLayer(r, QStringLiteral("never")) == nullptr);
    CHECK(r.diag.skipTags.contains(QStringLiteral("switch")));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    test_group_clip_cuts_descendants();
    test_leaf_clip_intersects_group_clip();
    test_group_transform_maps_clip();
    test_display_none_culls_subtree();
    test_clip_rule_evenodd_cuts_hole();
    test_link_group_renders_children();
    test_silent_tags_stay_clean();
    test_switch_picks_first_passing_child();
    test_unclipped_leaf_unaffected();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
