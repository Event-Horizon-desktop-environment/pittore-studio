#include "ui/tools/table/tool_table.h"

#include <unordered_map>
#include <vector>

namespace pittore::ui::detail {

using S = ToolSection;

// The table below is research/R01 transcribed. Order inside a section is the
// order the tools appear in the toolbar; the first entry of each
// key-group is the group leader shown on the strip.
const std::vector<ToolDef> kToolTable = {
    // 1. Selection ---------------------------------------------------------
    {ToolId::Move,                 "Move",                    "move",        'V', S::Selection,    true,  "Move layers, selections and guides. Ctrl-drag from any paint tool."},
    {ToolId::Artboard,             "Artboard",                "artboard",    'V', S::Selection,    false, "Create and rearrange artboards."},
    {ToolId::RectMarquee,          "Rectangular Marquee",     "marquee-rect",'M', S::Selection,    true,  "Rectangular selection. Shift = square, Alt = from centre."},
    {ToolId::EllipseMarquee,       "Elliptical Marquee",      "marquee-ell", 'M', S::Selection,    false, "Elliptical selection. Shift = circle, Alt = from centre."},
    {ToolId::SingleRowMarquee,     "Single Row Marquee",      "marquee-row", 'M', S::Selection,    false, "Select a single pixel row."},
    {ToolId::SingleColumnMarquee,  "Single Column Marquee",   "marquee-col", 'M', S::Selection,    false, "Select a single pixel column."},
    {ToolId::Lasso,                "Lasso",                   "lasso",       'L', S::Selection,    true,  "Freehand selection."},
    {ToolId::PolygonalLasso,       "Polygonal Lasso",         "lasso-poly",  'L', S::Selection,    false, "Straight-segment selection."},
    {ToolId::MagneticLasso,        "Magnetic Lasso",          "lasso-mag",   'L', S::Selection,    false, "Edge-snapping freehand selection."},
    {ToolId::SelectionBrush,       "Selection Brush",         "sel-brush",   'L', S::Selection,    false, "Paint a selection directly; commit to marching ants."},
    {ToolId::QuickSelection,       "Quick Selection",         "quick-sel",   'W', S::Selection,    true,  "Drag to grow a selection along edges."},
    {ToolId::ObjectSelection,      "Object Selection",        "object-sel",  'W', S::Selection,    false, "Draw a region; the object inside is selected."},
    {ToolId::MagicWand,            "Magic Wand",              "wand",        'W', S::Selection,    false, "Select by colour similarity."},

    // 2. Crop, layout & measurement ----------------------------------------
    {ToolId::Crop,                 "Crop",                    "crop",        'C', S::CropMeasure,  true,  "Crop the document; content-aware fill available."},
    {ToolId::PerspectiveCrop,      "Perspective Crop",        "crop-persp",  'C', S::CropMeasure,  false, "Crop and correct perspective in one step."},
    {ToolId::Slice,                "Slice",                   "slice",       'C', S::CropMeasure,  false, "Divide the document into export slices."},
    {ToolId::SliceSelect,          "Slice Select",            "slice-sel",   'C', S::CropMeasure,  false, "Select and edit existing slices."},
    {ToolId::Frame,                "Frame",                   "frame",       'K', S::CropMeasure,  true,  "Create a placeholder frame for images."},
    {ToolId::Eyedropper,           "Eyedropper",              "eyedropper",  'I', S::CropMeasure,  true,  "Sample a colour. Alt = background colour."},
    {ToolId::ColorSampler,         "Color Sampler",           "sampler",     'I', S::CropMeasure,  false, "Pin up to four readouts into the Info panel."},
    {ToolId::Ruler,                "Ruler",                   "ruler",       'I', S::CropMeasure,  false, "Measure distance and angle."},
    {ToolId::Note,                 "Note",                    "note",        'I', S::CropMeasure,  false, "Attach a text annotation to the document."},
    {ToolId::Count,                "Count",                   "count",       'I', S::CropMeasure,  false, "Count items in the image."},
    {ToolId::StylePickerTool,      "Style Picker",            "style-picker", 0, S::CropMeasure,  false, "Sample style attributes and paint them on."},
    {ToolId::MeasureTool,          "Measure",                 "measure",     0,  S::CropMeasure,  false, "Measure with drawing scale."},
    {ToolId::AreaTool,             "Area",                    "area",        0,  S::CropMeasure,  false, "Measure area with drawing scale."},

    // 3. Retouching & painting ---------------------------------------------
    {ToolId::SpotHealing,          "Spot Healing Brush",      "spot-heal",   'J', S::RetouchPaint, true,  "Heal blemishes from surrounding texture."},
    {ToolId::Remove,               "Remove",                  "remove",      'J', S::RetouchPaint, false, "Paint over a distraction to remove it."},
    {ToolId::HealingBrush,         "Healing Brush",           "heal",        'J', S::RetouchPaint, false, "Alt-click a source, then paint to heal."},
    {ToolId::Patch,                "Patch",                   "patch",       'J', S::RetouchPaint, false, "Lasso a region and drag it onto clean pixels."},
    {ToolId::ContentAwareMove,     "Content-Aware Move",      "ca-move",     'J', S::RetouchPaint, false, "Move an object and fill what it left behind."},
    {ToolId::RedEye,               "Red Eye",                 "red-eye",     'J', S::RetouchPaint, false, "Remove red-eye from flash photographs."},
    {ToolId::CloneStamp,           "Clone Stamp",             "clone",       'S', S::RetouchPaint, true,  "Alt-click a source, then paint copies of it."},
    {ToolId::PatternStamp,         "Pattern Stamp",           "pattern",     'S', S::RetouchPaint, false, "Paint with a pattern."},
    {ToolId::Eraser,               "Eraser",                  "eraser",      'E', S::RetouchPaint, true,  "Erase to transparency or to the background colour."},
    {ToolId::BackgroundEraser,     "Background Eraser",       "eraser-bg",   'E', S::RetouchPaint, false, "Erase sampled colour while protecting edges."},
    {ToolId::MagicEraser,          "Magic Eraser",            "eraser-magic",'E', S::RetouchPaint, false, "Erase similarly-coloured pixels in one click."},
    {ToolId::Blur,                 "Blur",                    "blur",        0,   S::RetouchPaint, true,  "Soften detail by painting."},
    {ToolId::Sharpen,              "Sharpen",                 "sharpen",     0,   S::RetouchPaint, false, "Increase local contrast by painting."},
    {ToolId::Smudge,               "Smudge",                  "smudge",      0,   S::RetouchPaint, false, "Push pixels as if dragging wet paint."},
    {ToolId::Liquify,              "Liquify",                 "liquify",     0,   S::RetouchPaint, true,  "Warp pixels with a brush (Filter > Liquify)."},
    {ToolId::Dodge,                "Dodge",                   "dodge",       'O', S::RetouchPaint, true,  "Lighten by painting."},
    {ToolId::Burn,                 "Burn",                    "burn",        'O', S::RetouchPaint, false, "Darken by painting."},
    {ToolId::Sponge,               "Sponge",                  "sponge",      'O', S::RetouchPaint, false, "Saturate or desaturate by painting."},
    {ToolId::Brush,                "Brush",                   "brush",       'B', S::RetouchPaint, true,  "Paint with the foreground colour."},
    {ToolId::Pencil,               "Pencil",                  "pencil",      'B', S::RetouchPaint, false, "Paint hard-edged, aliased strokes."},
    {ToolId::ColorReplacement,     "Color Replacement",       "replace",     'B', S::RetouchPaint, false, "Replace colour while keeping texture. Alt-click or Lock Area locks the area colours."},
    {ToolId::MixerBrush,           "Mixer Brush",             "mixer",       'B', S::RetouchPaint, false, "Blend canvas colour into the brush load."},
    {ToolId::HistoryBrush,         "History Brush",           "history-br",  'Y', S::RetouchPaint, true,  "Paint back from a history state."},
    {ToolId::ArtHistoryBrush,      "Art History Brush",       "art-history", 'Y', S::RetouchPaint, false, "Paint stylised strokes from a history state."},
    {ToolId::Gradient,             "Gradient",                "gradient",    'G', S::RetouchPaint, true,  "Linear, radial, angle, reflected or diamond blend."},
    {ToolId::PaintBucket,          "Paint Bucket",            "bucket",      'G', S::RetouchPaint, false, "Fill similar pixels with the foreground colour."},
    // Its own group leader (not a Gradient rider): personas must be able to
    // show Transparency without also showing Gradient.
    {ToolId::TransparencyTool,     "Transparency",            "transparency", 0,  S::RetouchPaint, true,  "Apply transparency gradients to vector objects."},
    {ToolId::AdjustmentBrush,      "Adjustment Brush",        "adj-brush",   0,   S::RetouchPaint, true,  "Paint an adjustment; the mask is created for you."},

    // 4. Drawing & type -----------------------------------------------------
    {ToolId::Pen,                  "Pen",                     "pen",         'P', S::DrawType,     true,  "Draw precise Bezier paths."},
    {ToolId::FreeformPen,          "Freeform Pen",            "pen-free",    'P', S::DrawType,     false, "Draw paths freehand."},
    {ToolId::CurvaturePen,         "Curvature Pen",           "pen-curve",   'P', S::DrawType,     false, "Draw curves by clicking points."},
    {ToolId::ContentAwareTracing,  "Content-Aware Tracing",   "trace",       'P', S::DrawType,     false, "Hover an edge; commit it as a path or selection."},
    {ToolId::AddAnchorPoint,       "Add Anchor Point",        "anchor-add",  '+', S::DrawType,     true,  "Add a point to an existing path."},
    {ToolId::DeleteAnchorPoint,    "Delete Anchor Point",     "anchor-del",  '-', S::DrawType,     false, "Remove a point from a path."},
    {ToolId::ConvertPoint,         "Convert Point",           "anchor-conv", 0,   S::DrawType,     false, "Switch a point between smooth and corner."},
    {ToolId::StrokeWidthTool,      "Stroke Width",            "stroke-width", 0,  S::DrawType,     false, "Edit a curve's pressure profile on canvas."},
    {ToolId::KnifeTool,            "Knife",                   "knife",       0,   S::DrawType,     false, "Cut curves and shapes into fragments."},
    {ToolId::PathSelection,        "Path Selection",          "path-sel",    'A', S::DrawType,     true,  "Select and move whole paths."},
    {ToolId::DirectSelection,      "Direct Selection",        "direct-sel",  'A', S::DrawType,     false, "Select and move individual points and handles."},
    {ToolId::NodeTool,             "Node",                    "node",        0,   S::DrawType,     true,  "Edit curves at node and handle level."},
    {ToolId::PointTransformTool, "Point Transform",         "point-xform", 0,  S::DrawType,     false, "Scale and rotate from any node."},
    {ToolId::CornerTool,         "Corner",                  "corner",      0,   S::DrawType,     true,  "Round corners by dragging."},
    {ToolId::ContourTool,        "Contour",                 "contour",     0,   S::DrawType,     false, "Offset an outline inward or outward."},
    {ToolId::Rectangle,            "Rectangle",               "shape-rect",  'U', S::DrawType,     true,  "Draw a live rectangle; corner radius is editable."},
    {ToolId::Ellipse,              "Ellipse",                 "shape-ell",   'U', S::DrawType,     false, "Draw a live ellipse."},
    {ToolId::Triangle,             "Triangle",                "shape-tri",   'U', S::DrawType,     false, "Draw a live triangle."},
    {ToolId::Polygon,              "Polygon",                 "shape-poly",  'U', S::DrawType,     false, "Draw a live polygon."},
    {ToolId::Star,                 "Star",                    "shape-star",  'U', S::DrawType,     false, "Draw a live star."},
    {ToolId::Line,                 "Line",                    "shape-line",  'U', S::DrawType,     false, "Draw a line or arrow."},
    {ToolId::CustomShape,          "Custom Shape",            "shape-custom",'U', S::DrawType,     false, "Draw from the shape library."},
    {ToolId::RoundedRectangle,     "Rounded Rectangle",       "shape-rounded", 'U', S::DrawType,   false, "Draw a live rounded rectangle."},
    {ToolId::Diamond,              "Diamond",                 "shape-diamond", 'U', S::DrawType,   false, "Draw a live diamond."},
    {ToolId::Trapezoid,            "Trapezoid",               "shape-trapezoid", 'U', S::DrawType, false, "Draw a live trapezoid."},
    {ToolId::DoubleStar,           "Double Star",             "shape-doublestar", 'U', S::DrawType, false, "Draw a live double star."},
    {ToolId::SquareStar,           "Square Star",             "shape-squarestar", 'U', S::DrawType, false, "Draw a live square star."},
    {ToolId::Arrow,                "Arrow",                   "shape-arrow", 'U', S::DrawType,     false, "Draw a live arrow."},
    {ToolId::Donut,                "Donut",                   "shape-donut", 'U', S::DrawType,     false, "Draw a live donut."},
    {ToolId::Pie,                  "Pie",                     "shape-pie",   'U', S::DrawType,     false, "Draw a live pie."},
    {ToolId::Segment,              "Segment",                 "shape-segment", 'U', S::DrawType,   false, "Draw a live circle segment."},
    {ToolId::Crescent,             "Crescent",                "shape-crescent", 'U', S::DrawType,  false, "Draw a live crescent."},
    {ToolId::Cog,                  "Cog",                     "shape-cog",   'U', S::DrawType,     false, "Draw a live cog."},
    {ToolId::Cloud,                "Cloud",                   "shape-cloud", 'U', S::DrawType,     false, "Draw a live cloud."},
    {ToolId::CalloutRect,          "Callout Rounded Rectangle", "shape-callout-rect", 'U', S::DrawType, false, "Draw a rounded-rectangle callout."},
    {ToolId::CalloutEllipse,       "Callout Ellipse",         "shape-callout-ell", 'U', S::DrawType, false, "Draw an ellipse callout."},
    {ToolId::Tear,                 "Tear",                    "shape-tear",  'U', S::DrawType,     false, "Draw a live tear."},
    {ToolId::Heart,                "Heart",                   "shape-heart", 'U', S::DrawType,     false, "Draw a live heart."},
    {ToolId::Spiral,               "Spiral",                  "shape-spiral", 'U', S::DrawType,    false, "Draw a live spiral."},
    {ToolId::QRCode,               "QR Code",                 "shape-qr",    'U', S::DrawType,     false, "Draw a QR code. Needs a QR encoder."},
    {ToolId::Cat,                  "Cat",                     "shape-cat",   'U', S::DrawType,     false, "Draw a cat."},
    {ToolId::Hexagon,              "Hexagon",                 "shape-hexagon", 'U', S::DrawType,   false, "Draw a live hexagon."},
    {ToolId::Octagon,              "Octagon",                 "shape-octagon", 'U', S::DrawType,   false, "Draw a live octagon."},
    {ToolId::Cross,                "Cross",                   "shape-cross", 'U', S::DrawType,     false, "Draw a live cross."},
    {ToolId::RightTriangle,        "Right Triangle",          "shape-rtriangle", 'U', S::DrawType, false, "Draw a live right triangle."},
    {ToolId::Parallelogram,        "Parallelogram",           "shape-parallelogram", 'U', S::DrawType, false, "Draw a live parallelogram."},
    {ToolId::Chevron,              "Chevron",                 "shape-chevron", 'U', S::DrawType,   false, "Draw a live chevron."},
    {ToolId::DoubleArrow,          "Double Arrow",            "shape-doublearrow", 'U', S::DrawType, false, "Draw a live double-headed arrow."},
    {ToolId::CircularArrow,        "Circular Arrow",          "shape-circulararrow", 'U', S::DrawType, false, "Draw a live circular arrow."},
    {ToolId::Sparkle,              "Sparkle",                 "shape-sparkle", 'U', S::DrawType,   false, "Draw a live four-point sparkle."},
    {ToolId::Shield,               "Shield",                  "shape-shield", 'U', S::DrawType,    false, "Draw a live shield badge."},
    {ToolId::Ticket,               "Ticket",                  "shape-ticket", 'U', S::DrawType,    false, "Draw a live ticket."},
    {ToolId::Sun,                  "Sun",                     "shape-sun",   'U', S::DrawType,     false, "Draw a live sun."},
    {ToolId::HorizontalType,       "Horizontal Type",         "type",        'T', S::DrawType,     true,  "Set horizontal type."},
    {ToolId::VerticalType,         "Vertical Type",           "type-vert",   'T', S::DrawType,     false, "Set vertical type."},
    {ToolId::HorizontalTypeMask,   "Horizontal Type Mask",    "type-mask",   'T', S::DrawType,     false, "Create a selection shaped like horizontal type."},
    {ToolId::VerticalTypeMask,     "Vertical Type Mask",      "type-mask-v", 'T', S::DrawType,     false, "Create a selection shaped like vertical type."},
    {ToolId::VectorBrushTool,      "Vector Brush",            "vector-brush", 0,  S::DrawType,     true,  "Paint editable vector brush strokes."},
    {ToolId::VectorFloodFillTool,  "Vector Flood Fill",       "flood-vector", 0, S::DrawType,     true,  "Flood bounded areas with new shapes."},
    {ToolId::ShapeBuilderTool,     "Shape Builder",           "shape-builder", 0,S::DrawType,     false, "Combine shape areas into complex shapes."},
    {ToolId::VectorCropTool,       "Vector Crop",             "crop-vector", 0,  S::DrawType,     true,  "Non-destructively crop objects."},
    {ToolId::PlaceTool,            "Place",                   "place",       0,  S::DrawType,     true,  "Place images and documents."},
    {ToolId::CalligraphyTool,      "Calligraphy",             "calligraphy", 0,  S::DrawType,     true,  "Calligraphic nib strokes (angle/width/thinning)."},
    {ToolId::SprayTool,            "Spray",                   "spray",       0,  S::DrawType,     true,  "Scatter stamps with outline preview."},
    {ToolId::MeshTool,             "Mesh Gradient",           "mesh",        0,  S::DrawType,     true,  "Edit mesh-gradient patches."},
    {ToolId::ConnectorTool,        "Connector",               "connector",   0,  S::DrawType,     true,  "Diagram connectors with obstacle avoidance."},
    {ToolId::TweakTool,            "Tweak",                   "tweak",       0,  S::DrawType,     true,  "Push/shrink/grow vector art."},
    {ToolId::Box3DTool,            "3D Box",                  "box3d",       0,  S::DrawType,     true,  "Perspective boxes."},
    {ToolId::PagesTool,            "Pages",                   "pages",       0,  S::DrawType,     true,  "Multipage artboards."},
    {ToolId::LpeTool,              "Path Effects",            "lpe",         0,  S::DrawType,     true,  "Apply live path effects."},
    {ToolId::MarkerTool,           "Marker",                  "marker",      0,  S::DrawType,     true,  "Edit start/mid/end markers."},
    {ToolId::VectorEraserTool,     "Vector Eraser",           "eraser-vector", 0, S::DrawType,   true,  "Erase vector segments by width."},

    // 5. Navigation ---------------------------------------------------------
    {ToolId::Hand,                 "Hand",                    "hand",        'H', S::Navigation,   true,  "Pan the view. Hold Space from any tool."},
    {ToolId::RotateView,           "Rotate View",             "rotate-view", 'R', S::Navigation,   false, "Rotate the canvas view non-destructively."},
    {ToolId::Zoom,                 "Zoom",                    "zoom",        'Z', S::Navigation,   true,  "Zoom in; Alt to zoom out."},

    // 7. Content & generative ----------------------------------------------
    {ToolId::ImportPhotos,         "Import Photos",           "import",      0,   S::Generative,   true,  "Import images from a connected device."},
    {ToolId::GenerativeFill,       "Generative Fill",         "gen-fill",    0,   S::Generative,   false, "Describe what should fill the selection."},
    {ToolId::GenerateBackground,   "Generate Background",     "gen-bg",      0,   S::Generative,   false, "Replace the background from a description."},
};

std::vector<ToolGroup> buildToolGroups() {
    std::vector<ToolGroup> groups;
    for (const ToolDef& t : kToolTable) {
        if (t.groupLeader) {
            ToolGroup g;
            g.leader = t.id;
            g.key = t.key;
            g.section = t.section;
            g.members.push_back(t.id);
            groups.push_back(g);
        } else if (!groups.empty()) {
            groups.back().members.push_back(t.id);
        }
    }
    return groups;
}

const std::unordered_map<int, const ToolDef*>& toolTableIndex() {
    static const std::unordered_map<int, const ToolDef*> index = [] {
        std::unordered_map<int, const ToolDef*> m;
        for (const ToolDef& t : kToolTable) m.emplace(static_cast<int>(t.id), &t);
        return m;
    }();
    return index;
}

const std::vector<ToolDef>& toolTable() { return kToolTable; }

}  // namespace pittore::ui::detail
