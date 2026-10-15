// SVG paint-server strokes: `stroke="url(#id)"` must paint its gradient into
// the piece raster (and into the retained vector geometry), plain strokes keep
// painting, and `fill="none"` shapes with no stroke stay empty. Headless
// (offscreen), small synthetics only.
#include <QApplication>
#include <QColor>
#include <QImage>

#include <cstdint>
#include <string>
#include <vector>

#include "test_util.h"
#include "ui/svg_parts.h"

using namespace pittore::ui;
using pittore::vector::ArtDocument;
using pittore::vector::ArtElement;
using pittore::vector::artDocumentToSvg;
using pittore::vector::decodeArtNode;
using pittore::vector::encodeArtNode;

namespace {

// Two concentric circles, matching the reported badge: a filled disc and a
// gradient-stroked ring whose two ends land on opposite colours of the axis.
QByteArray gradientStrokeSvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
        "<defs>"
        "<linearGradient id=\"ring\" gradientUnits=\"userSpaceOnUse\" "
        "x1=\"0\" y1=\"0\" x2=\"100\" y2=\"100\">"
        "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
        "<stop offset=\"1\" stop-color=\"#0000ff\"/>"
        "</linearGradient>"
        "</defs>"
        "<circle id=\"disc\" cx=\"50\" cy=\"50\" r=\"48\" fill=\"#101010\"/>"
        "<circle id=\"ring\" cx=\"50\" cy=\"50\" r=\"40\" fill=\"none\" "
        "stroke=\"url(#ring)\" stroke-width=\"8\"/>"
        "<path id=\"flat\" d=\"M10 90 L90 90\" fill=\"none\" "
        "stroke=\"#00ff00\" stroke-width=\"4\"/>"
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

// Painted pixels in either corner of the gradient axis: red near (0,0),
// blue near (100,100).
bool hasRed(const QImage& img) {
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.alpha() > 8 && c.red() > c.blue() + 60) return true;
        }
    return false;
}
bool hasBlue(const QImage& img) {
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.alpha() > 8 && c.blue() > c.red() + 60) return true;
        }
    return false;
}

void test_gradient_stroke_paints() {
    SvgImportResult r;
    int dpi = 96;
    QString error;
    CHECK(svgPartsImport(gradientStrokeSvg(), &r, &dpi, &error));
    CHECK(error.isEmpty());

    const SvgPartLayer* disc = findLayer(r, QStringLiteral("disc"));
    const SvgPartLayer* ring = findLayer(r, QStringLiteral("ring"));
    const SvgPartLayer* flat = findLayer(r, QStringLiteral("flat"));
    CHECK(disc != nullptr);
    CHECK(ring != nullptr);
    CHECK(flat != nullptr);
    if (!disc || !ring || !flat) return;

    // The filled disc always painted.
    CHECK(painted(disc->pixels) > 1000);
    // The gradient-stroked ring must paint too: a ring of width 8 around
    // r=40 covers roughly 2*pi*40*8 ~ 2000 doc pixels.
    CHECK(painted(ring->pixels) > 500);
    // ...and it must carry the gradient, not a flat stand-in.
    CHECK(hasRed(ring->pixels));
    CHECK(hasBlue(ring->pixels));
    // A plain stroke still paints.
    CHECK(painted(flat->pixels) > 100);

    // Retained vector geometry keeps the stroke so the vector persona and
    // the SVG writer see the same paint as the raster.
    CHECK(ring->art != nullptr);
    CHECK(disc->art != nullptr);
    if (!ring->art || !disc->art) return;
    CHECK(ring->art->paint.hasStroke);
    CHECK(ring->art->paint.strokeWidth > 0.0);
    CHECK(ring->art->paint.hasStrokeGradient);
    CHECK_EQ(ring->art->paint.strokeGradient.stops.size(), 2u);
    CHECK(disc->art->paint.hasFill);

    // The project encoding carries the stroke gradient as an end-appended
    // tail: round-tripping must not silently flatten the ring.
    const std::vector<std::uint8_t> blob = encodeArtNode(*ring->art);
    const auto back = decodeArtNode(blob, nullptr);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->paint.hasStroke);
        CHECK(back->paint.hasStrokeGradient);
        CHECK_EQ(back->paint.strokeGradient.stops.size(), 2u);
        CHECK(!back->paint.strokeGradient.radial);
    }

    // SVG export writes the paint server back as a reference, not a flat
    // stand-in colour.
    ArtDocument doc;
    doc.width = 100.0;
    doc.height = 100.0;
    ArtElement el;
    el.node = *ring->art;
    doc.elements.push_back(el);
    const std::string out = artDocumentToSvg(doc);
    CHECK(out.find("stroke=\"url(#grad") != std::string::npos);
    CHECK(out.find("<linearGradient id=\"grad0") != std::string::npos);
    CHECK(out.find("stop-color=\"#ff0000\"") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    test_gradient_stroke_paints();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
