// Persona mapping tests: vector/pixel tool + panel sets (vector.md §6).
// Plus the vector re-rasterizer (ArtNode → trimmed pixels). QtCore/QtGui only
// — no widgets, no AppState — so this cannot regress the Pixel UI.
#include <QImage>
#include <QSet>
#include <QString>

#include "engine/vector/vector_art.h"
#include "test_util.h"
#include "ui/persona/persona.h"
#include "ui/persona/vector_node.h"
#include "ui/persona/vector_point_ops.h"
#include "ui/persona/vector_raster.h"
#include "ui/tool_registry.h"

using namespace pittore::ui;
using namespace pittore::vector;

namespace {

bool containsLeader(const std::vector<ToolId>& leaders, ToolId id) {
    for (ToolId l : leaders)
        if (l == id) return true;
    return false;
}

void run() {
    // Id round-trip.
    CHECK(personaName(Persona::Pixel) == QStringLiteral("Pixel"));
    CHECK(personaName(Persona::Vector) == QStringLiteral("Vector"));
    CHECK(personaName(Persona::Draw) == QStringLiteral("Draw"));
    CHECK(personaName(Persona::Color) == QStringLiteral("Color"));
    CHECK(personaId(Persona::Pixel) == QStringLiteral("pixel"));
    CHECK(personaId(Persona::Vector) == QStringLiteral("vector"));
    CHECK(personaId(Persona::Draw) == QStringLiteral("draw"));
    CHECK(personaId(Persona::Color) == QStringLiteral("color"));
    Persona out = Persona::Pixel;
    CHECK(personaFromId(QStringLiteral("vector"), &out) && out == Persona::Vector);
    CHECK(personaFromId(QStringLiteral("pixel"), &out) && out == Persona::Pixel);
    CHECK(personaFromId(QStringLiteral("draw"), &out) && out == Persona::Draw);
    CHECK(personaFromId(QStringLiteral("color"), &out) && out == Persona::Color);
    CHECK(!personaFromId(QStringLiteral("layout"), &out));

    // Artboard round-trip (structural persona).
    CHECK(personaName(Persona::Artboard) == QStringLiteral("Artboard"));
    CHECK(personaId(Persona::Artboard) == QStringLiteral("artboard"));
    CHECK(personaFromId(QStringLiteral("artboard"), &out) &&
          out == Persona::Artboard);

    // Artboard is structural + vector layout: paint, selection and retouch
    // groups stay hidden so no tool in this tab can touch pixels.
    {
        const QStringList hidden = hiddenToolsFor(Persona::Artboard);
        CHECK(!hidden.isEmpty());
        for (ToolId id : artboardHiddenLeaders())
            CHECK(hidden.contains(QString::number(static_cast<int>(id))));
        // hiddenToolsFor() carries GROUP LEADERS only: tools that ride in
        // another group (Eraser/Pencil/CloneStamp with Brush, Transparency
        // with Gradient, PointTransform with Node) are not listed under their
        // own id. So assert over allGroups(), not over raw ToolIds.
        const std::vector<ToolId> vis = artboardVisibleLeaders();
        int groups = 0;
        int visibleLeaders = 0;
        for (const ToolGroup& g : allGroups()) {
            ++groups;
            const bool visible =
                std::find(vis.begin(), vis.end(), g.leader) != vis.end();
            if (visible) ++visibleLeaders;
            CHECK(hidden.contains(QString::number(static_cast<int>(g.leader))) ==
                  !visible);
        }
        CHECK(visibleLeaders > 0);
        CHECK(hidden.size() == groups - visibleLeaders);
        // The layout set the persona exists for must survive as visible
        // leaders: drawing (Pen/Rectangle), editing (Node), Place and the
        // navigation/colour utilities.
        for (ToolId id : {ToolId::Move, ToolId::Pen, ToolId::Rectangle,
                          ToolId::Eyedropper, ToolId::Hand, ToolId::Zoom})
            CHECK(!hidden.contains(QString::number(static_cast<int>(id))));
    }

    // The Artboards panel is Artboard-exclusive: it must not appear anywhere
    // else, and must lead its own layout.
    CHECK(artboardOnlyPanels().contains(QStringLiteral("artboards")));
    CHECK(artboardPanels().first() == QStringLiteral("artboards"));
    CHECK(!vectorPanels().contains(QStringLiteral("artboards")));
    CHECK(!colorPanels().contains(QStringLiteral("artboards")));
    CHECK(!drawPanels().contains(QStringLiteral("artboards")));
    CHECK(!pixelFallbackPanels().contains(QStringLiteral("artboards")));

    // Pixel hides vector-exclusive tools only (photo bench keeps the rest).
    {
        const QStringList pixelHidden = hiddenToolsFor(Persona::Pixel);
        CHECK(!pixelHidden.isEmpty());
        for (ToolId id : pixelHiddenLeaders())
            CHECK(pixelHidden.contains(QString::number(static_cast<int>(id))));
        for (ToolId id :
             {ToolId::Move, ToolId::Brush, ToolId::Eraser, ToolId::Hand,
              ToolId::Zoom, ToolId::RectMarquee, ToolId::CloneStamp,
              ToolId::HorizontalType})
            CHECK(!pixelHidden.contains(
                QString::number(static_cast<int>(id))));
    }

    // Vector keeps the draw/select core and its shared tools.
    const std::vector<ToolId> visible = vectorVisibleLeaders();
    for (ToolId id :
         {ToolId::Move, ToolId::Eyedropper, ToolId::Gradient, ToolId::Pen,
          ToolId::AddAnchorPoint, ToolId::PathSelection, ToolId::Rectangle,
          ToolId::HorizontalType, ToolId::Hand, ToolId::Zoom, ToolId::NodeTool,
          ToolId::CornerTool, ToolId::VectorBrushTool,
          ToolId::VectorFloodFillTool, ToolId::VectorCropTool,
          ToolId::PlaceTool})
        CHECK(containsLeader(visible, id));

    // Vector hides the pixel benches (selection, crop, retouch, paint, generative).
    const QStringList hidden = hiddenToolsFor(Persona::Vector);
    CHECK(!hidden.isEmpty());
    for (ToolId id :
         {ToolId::RectMarquee, ToolId::Lasso, ToolId::QuickSelection, ToolId::Crop,
          ToolId::SpotHealing, ToolId::CloneStamp, ToolId::Eraser, ToolId::Brush,
          ToolId::Dodge, ToolId::ImportPhotos})
        CHECK(hidden.contains(QString::number(static_cast<int>(id))));

    // Visible + hidden is an exact partition of the group leaders.
    QSet<int> seen;
    // New shapes ride the Rectangle flyout: no strip or filter changes needed.
    for (ToolId id :
         {ToolId::RoundedRectangle, ToolId::Diamond, ToolId::Trapezoid,
          ToolId::DoubleStar, ToolId::SquareStar, ToolId::Arrow, ToolId::Donut,
          ToolId::Pie, ToolId::Segment, ToolId::Crescent, ToolId::Cog,
          ToolId::Cloud, ToolId::CalloutRect, ToolId::CalloutEllipse,
          ToolId::Tear, ToolId::Heart, ToolId::Spiral, ToolId::QRCode,
          ToolId::Cat, ToolId::Hexagon, ToolId::Octagon, ToolId::Cross,
          ToolId::RightTriangle, ToolId::Parallelogram, ToolId::Chevron,
          ToolId::DoubleArrow, ToolId::CircularArrow, ToolId::Sparkle,
          ToolId::Shield, ToolId::Ticket, ToolId::Sun}) {
        const ToolGroup* g = groupForTool(id);
        CHECK(g != nullptr);
        if (g) CHECK(g->leader == ToolId::Rectangle);
    }    for (ToolId id : visible) seen.insert(static_cast<int>(id));
    for (ToolId id : vectorHiddenLeaders()) CHECK(!seen.contains(static_cast<int>(id)));
    for (ToolId id : vectorHiddenLeaders()) seen.insert(static_cast<int>(id));
    QSet<int> leaders;
    for (const ToolGroup& group : allGroups()) leaders.insert(static_cast<int>(group.leader));
    CHECK(seen == leaders);

    // Draw keeps the paint family + shared utilities, nothing else.
    {
        const std::vector<ToolId> drawVisible = drawVisibleLeaders();
        for (ToolId id :
             {ToolId::Move, ToolId::Brush, ToolId::Eraser, ToolId::Blur,
              ToolId::Dodge, ToolId::HistoryBrush, ToolId::Gradient,
              ToolId::Eyedropper, ToolId::Hand, ToolId::Zoom})
            CHECK(containsLeader(drawVisible, id));
        const QStringList drawHidden = hiddenToolsFor(Persona::Draw);
        for (ToolId id :
             {ToolId::Pen, ToolId::RectMarquee, ToolId::CloneStamp,
              ToolId::HorizontalType, ToolId::VectorBrushTool,
              ToolId::Liquify, ToolId::ImportPhotos})
            CHECK(drawHidden.contains(QString::number(static_cast<int>(id))));
        // Draw visible + hidden partitions the leaders exactly.
        QSet<int> dseen;
        for (ToolId id : drawVisible) dseen.insert(static_cast<int>(id));
        for (ToolId id : drawHiddenLeaders()) {
            CHECK(!dseen.contains(static_cast<int>(id)));
            dseen.insert(static_cast<int>(id));
        }
        CHECK(dseen == leaders);
        // Pixel hides exactly the vector-exclusive set.
        QSet<int> pseen;
        for (ToolId id : pixelHiddenLeaders())
            pseen.insert(static_cast<int>(id));
        CHECK(hiddenToolsFor(Persona::Pixel).size() == pseen.size());
    }

    // Color keeps the grade-only tools + shared utilities, nothing else.
    {
        const std::vector<ToolId> colorVisible = colorVisibleLeaders();
        for (ToolId id :
             {ToolId::Move, ToolId::Eyedropper, ToolId::Dodge,
              ToolId::Gradient, ToolId::Hand, ToolId::Zoom})
            CHECK(containsLeader(colorVisible, id));
        const QStringList colorHidden = hiddenToolsFor(Persona::Color);
        CHECK(!colorHidden.isEmpty());
        for (ToolId id :
             {ToolId::Pen, ToolId::RectMarquee, ToolId::CloneStamp,
              ToolId::Brush, ToolId::HorizontalType, ToolId::VectorBrushTool,
              ToolId::Liquify, ToolId::ImportPhotos})
            CHECK(colorHidden.contains(QString::number(static_cast<int>(id))));
        // Grade tools stay visible: Dodge/Gradient/Eyedropper must not hide.
        for (ToolId id : {ToolId::Dodge, ToolId::Gradient, ToolId::Eyedropper})
            CHECK(!colorHidden.contains(QString::number(static_cast<int>(id))));
        // Color visible + hidden partitions the leaders exactly.
        QSet<int> cseen;
        for (ToolId id : colorVisible) cseen.insert(static_cast<int>(id));
        for (ToolId id : colorHiddenLeaders()) {
            CHECK(!cseen.contains(static_cast<int>(id)));
            cseen.insert(static_cast<int>(id));
        }
        CHECK(cseen == leaders);
    }

    // Panel sets.
    CHECK(vectorPanels().contains(QStringLiteral("layers")));
    CHECK(vectorPanels().contains(QStringLiteral("paths")));
    CHECK(vectorPanels().contains(QStringLiteral("color")));
    CHECK(vectorOnlyPanels().contains(QStringLiteral("stroke")));
    CHECK(vectorOnlyPanels().contains(QStringLiteral("paths")));
    for (const QString& id : vectorOnlyPanels())
        CHECK(vectorPanels().contains(id));
    CHECK(pixelOnlyPanels().contains(QStringLiteral("channels")));
    CHECK(pixelOnlyPanels().contains(QStringLiteral("adjustments")));
    for (const QString& id : pixelOnlyPanels()) CHECK(!vectorPanels().contains(id));
    CHECK(pixelFallbackPanels().contains(QStringLiteral("adjustments")));
    // Draw layout: paint panels, no photo-only or vector-only ones.
    CHECK(drawPanels().contains(QStringLiteral("brushes")));
    CHECK(drawPanels().contains(QStringLiteral("brushpreview")));
    CHECK(drawPanels().contains(QStringLiteral("navigator")));
    CHECK(drawPanels().contains(QStringLiteral("layers")));
    for (const QString& id : pixelOnlyPanels())
        CHECK(!drawPanels().contains(id));
    for (const QString& id : vectorOnlyPanels())
        CHECK(!drawPanels().contains(id));
    // Color layout: scopes-first grade panels; shares photo panels by design.
    CHECK(colorPanels().contains(QStringLiteral("histogram")));
    CHECK(colorPanels().contains(QStringLiteral("info")));
    CHECK(colorPanels().contains(QStringLiteral("color")));
    CHECK(colorPanels().contains(QStringLiteral("adjustments")));
    CHECK(colorPanels().contains(QStringLiteral("properties")));
    CHECK(colorPanels().contains(QStringLiteral("layers")));
    CHECK(colorPanels().contains(QStringLiteral("channels")));
    CHECK(colorFallbackPanels() == colorPanels());
    for (const QString& id : vectorOnlyPanels())
        CHECK(!colorPanels().contains(id));
}

// Rect 10,10 → 50,30 in node space, identity matrix (node == source pixels).
ArtNode rectNode() {
    ArtNode node;
    node.name = "rect";
    auto seg = [](Segment::Kind kind, float x, float y) {
        Segment s;
        s.kind = kind;
        s.x = x;
        s.y = y;
        return s;
    };
    node.segments = {seg(Segment::Kind::MoveTo, 10, 10),
                     seg(Segment::Kind::LineTo, 50, 10),
                     seg(Segment::Kind::LineTo, 50, 30),
                     seg(Segment::Kind::LineTo, 10, 30),
                     seg(Segment::Kind::Close, 0, 0)};
    node.paint.hasFill = true;
    node.paint.fill[0] = 255;
    node.paint.fill[1] = 0;
    node.paint.fill[2] = 0;
    node.paint.fill[3] = 255;
    return node;
}

void runAnchors() {
    auto countAnchors = [](const ArtNode& n) {
        int c = 0;
        for (const Segment& s : n.segments)
            if (s.kind == Segment::Kind::MoveTo ||
                s.kind == Segment::Kind::LineTo ||
                s.kind == Segment::Kind::CubicTo)
                ++c;
        return c;
    };
    // Insert on the top edge midpoint (30,10): new corner anchor.
    {
        ArtNode node = rectNode();
        int seg = -1;
        CHECK(insertAnchorPoint(node, QPointF(30, 10), 16.0, &seg));
        CHECK_EQ(countAnchors(node), 5);
        CHECK(seg >= 0);
        QPointF p(node.segments[std::size_t(seg)].x,
                  node.segments[std::size_t(seg)].y);
        CHECK(std::hypot(p.x() - 30.0, p.y() - 10.0) < 1.0);
    }
    // Far away: no span, unchanged.
    {
        ArtNode node = rectNode();
        const auto before = node.segments;
        int seg = -1;
        CHECK(!insertAnchorPoint(node, QPointF(200, 200), 16.0, &seg));
        CHECK(node.segments.size() == before.size());
    }
    // Near-corner clicks belong to the anchor tools (endpoint margin).
    {
        ArtNode node = rectNode();
        int seg = -1;
        CHECK(!insertAnchorPoint(node, QPointF(11, 10), 16.0, &seg));
    }
    // Insert into a cubic span preserves the curve (split halves).
    {
        ArtNode node = rectNode();
        node.segments[1].kind = Segment::Kind::CubicTo;
        node.segments[1].c1x = 20;
        node.segments[1].c1y = 0;
        node.segments[1].c2x = 40;
        node.segments[1].c2y = 0;
        int seg = -1;
        CHECK(insertAnchorPoint(node, QPointF(30, 2), 16.0, &seg));
        CHECK(node.segments[std::size_t(seg)].kind ==
              Segment::Kind::CubicTo);
        CHECK_EQ(countAnchors(node), 5);
    }
    // Delete a middle anchor: neighbours join straight.
    {
        ArtNode node = rectNode();
        CHECK(deleteAnchorPoint(node, 2));
        CHECK_EQ(countAnchors(node), 3);
        // The bridging span runs (50,10)->(10,30) as a clean line.
        CHECK(node.segments[2].kind == Segment::Kind::LineTo);
    }
    // Refusals: MoveTo start, minimum corners, tiny paths.
    {
        ArtNode node = rectNode();
        CHECK(!deleteAnchorPoint(node, 0));
        CHECK(!deleteAnchorPoint(node, -1));
        ArtNode tri = rectNode();
        CHECK(deleteAnchorPoint(tri, 2));  // rect -> triangle
        CHECK(!deleteAnchorPoint(tri, 1));  // triangle is minimal
    }
    // Smooth detection: straight rect corners are corners; converting one
    // to smooth grows handles, converting back retracts them.
    {
        ArtNode node = rectNode();
        CHECK(!isSmoothAnchor(node, 1));
        convertNodePoint(node, 1, true);
        CHECK(isSmoothAnchor(node, 1));
        convertNodePoint(node, 1, false);
        CHECK(!isSmoothAnchor(node, 1));
        CHECK(node.segments[1].kind == Segment::Kind::LineTo);
    }
}

void runRaster() {
    // Solid fill: trimmed image, opaque red heart, transparent margin.
    {
        ArtNode node = rectNode();
        QImage img;
        QPointF origin;
        CHECK(rasterizeArtNode(node, &img, &origin));
        CHECK(!img.isNull());
        CHECK(img.width() == 42);   // 40 + 1px AA margin each side
        CHECK(img.height() == 22);  // 20 + 1px AA margin each side
        const QColor heart = img.pixelColor(30 - int(origin.x()), 20 - int(origin.y()));
        CHECK(heart.red() > 200 && heart.alpha() == 255);
        CHECK(img.pixelColor(0, 0).alpha() < 16);
    }
    // Stroke grows the trim by half the width each side.
    {
        ArtNode node = rectNode();
        node.paint.hasStroke = true;
        node.paint.stroke[0] = 0;
        node.paint.stroke[1] = 0;
        node.paint.stroke[2] = 0;
        node.paint.stroke[3] = 255;
        node.paint.strokeWidth = 6.0;
        QImage img;
        QPointF origin;
        CHECK(rasterizeArtNode(node, &img, &origin));
        CHECK(img.width() == 48);   // 40 + (3 + 1) each side
        CHECK(img.height() == 28);  // 20 + (3 + 1) each side
    }
    // Linear gradient: red on the left, blue on the right.
    {
        ArtNode node = rectNode();
        node.paint.hasGradient = true;
        node.paint.gradient.radial = false;
        node.paint.gradient.x1 = 10;
        node.paint.gradient.y1 = 0;
        node.paint.gradient.x2 = 50;
        node.paint.gradient.y2 = 0;
        ArtStop a, b;
        a.pos = 0.0f;
        a.rgba[0] = 255;
        a.rgba[3] = 255;
        b.pos = 1.0f;
        b.rgba[2] = 255;
        b.rgba[3] = 255;
        node.paint.gradient.stops = {a, b};
        QImage img;
        QPointF origin;
        CHECK(rasterizeArtNode(node, &img, &origin));
        CHECK(!img.isNull());
        const int y = 20 - int(origin.y());
        const QColor left = img.pixelColor(20 - int(origin.x()), y);
        const QColor right = img.pixelColor(40 - int(origin.x()), y);
        CHECK(left.red() > left.blue());
        CHECK(right.blue() > right.red());
    }
    // Degenerate input renders nothing.
    {
        ArtNode empty;
        QImage img;
        QPointF origin;
        CHECK(!rasterizeArtNode(empty, &img, &origin));
        CHECK(img.isNull());
        ArtNode unpainted = rectNode();
        unpainted.paint.hasFill = false;
        CHECK(!rasterizeArtNode(unpainted, &img, &origin));
    }
}

}  // namespace

int main() {
    run();
    runRaster();
    runAnchors();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
