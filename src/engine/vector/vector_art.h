#pragma once
// Retained vector art and the SVG writer.
//
// The editor's document is raster, so a layer only has true vector geometry
// when the app kept it: SVG imports retain the parsed paths here (attached to
// the layer) and the composer emits them verbatim. Everything that was never
// vector (photos, painted pixels, rasterised filters) is embedded as a PNG, so
// an exported SVG is real vector for the parts that have geometry and a raster
// fallback for the rest.
//
// This file is toolkit-free: `Segment` comes from path.h, and the writer only
// produces text. Callers hand in already-encoded PNG bytes for raster pieces.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/vector/filter_fe.h"
#include "engine/vector/mesh.h"
#include "engine/vector/path.h"

namespace pittore::vector {

// One stop of a gradient fill.
struct ArtStop {
    float pos = 0.0f;
    std::uint8_t rgba[4] = {0, 0, 0, 255};
};

// A gradient fill. Coordinates are in the node's own space, matching the
// `segments`, so the writer can emit them with gradientUnits="userSpaceOnUse"
// inside the same transform as the path.
struct ArtGradient {
    bool radial = false;
    double x1 = 0.0, y1 = 0.0, x2 = 1.0, y2 = 0.0;  // linear axis
    double cx = 0.5, cy = 0.5, r = 0.5;             // radial centre + radius
    std::vector<ArtStop> stops;
};

// One width-profile control point: absolute width multiplier at normalized
// arclength t (0..1, per subpath).
struct ArtWidthPoint {
    float t = 0.0f;
    float w = 1.0f;
};

// How a shape is painted: a fill (flat or gradient) and/or a stroke.
struct ArtPaint {
    bool hasFill = false;
    std::uint8_t fill[4] = {0, 0, 0, 255};
    bool hasGradient = false;
    ArtGradient gradient;

    bool hasStroke = false;
    std::uint8_t stroke[4] = {0, 0, 0, 255};
    // Stroke paint server (SVG stroke="url(#id)"): when set it paints the
    // stroke instead of `stroke`, which stays as the flat fallback colour
    // for panels and writers that only carry a solid. Coordinates live in
    // node space like `gradient`.
    bool hasStrokeGradient = false;
    ArtGradient strokeGradient;
    double strokeWidth = 1.0;
    int cap = 0;   // 0 butt, 1 round, 2 square
    int join = 0;  // 0 miter, 1 round, 2 bevel
    // Dash pattern in stroke-width units (SVG dasharray semantics): empty or
    // hasDash=false draws solid. {0, gap} with round caps draws dots.
    bool hasDash = false;
    std::vector<float> dash;
    float dashOffset = 0.0f;
    // Variable-width profile: multipliers over normalized arclength (per
    // subpath), interpolated linearly. Empty or hasProfile=false draws the
    // uniform strokeWidth. Zero pinches the outline to a point.
    bool hasProfile = false;
    std::vector<ArtWidthPoint> profile;
    // Object refs (all optional, all round-trip through SVG + IFP):
    // pattern/hatch fill id, marker ids, clipPath/mask ids, live mesh paint,
    // and a retained SVG filter graph (applied post-raster by the importer,
    // re-emitted verbatim by the writer).
    std::string patternId;
    // Per-node pattern space override (patternTransform matrix, QTransform
    // order); identity + hasPatternXform=false means "use the def as-is".
    bool hasPatternXform = false;
    double patternXform[6] = {1, 0, 0, 1, 0, 0};
    std::string markerStart, markerMid, markerEnd;
    std::string clipId, maskId;
    bool hasMesh = false;
    MeshGradient mesh;
    bool hasFilter = false;
    FilterGraph filter;
};

// One retained vector shape. `segments` are in the node's own coordinates and
// `matrix` (a b c d e f, QTransform order) maps them into the layer's source
// space. `evenOdd` mirrors the rasteriser's fill rule.
struct ArtNode {
    std::string name;
    std::vector<Segment> segments;
    double matrix[6] = {1, 0, 0, 1, 0, 0};
    double opacity = 1.0;
    bool evenOdd = false;
    ArtPaint paint;

    bool isEmpty() const { return segments.empty(); }
};

// A raster piece to embed as <image>. `base64Png` is the raw base64 payload
// (no "data:" prefix); `mime` selects the data-URI type ("image/png" or
// "image/jpeg"). x/y/w/h place it in document space.
struct ArtRaster {
    std::string name;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    double opacity = 1.0;
    std::string base64Png;
    std::string mime = "image/png";
};

// One drawable in paint order. `raster` selects which payload is used.
// `blendCss` is an optional CSS mix-blend-mode value ("multiply", "screen"…);
// empty means normal.
struct ArtElement {
    bool raster = false;
    ArtNode node;
    ArtRaster image;
    std::string blendCss;
};

// A whole document: viewport size plus viewBox (viewX/Y/W/H; an empty viewBox
// falls back to 0,0,width,height).
struct ArtDocument {
    double width = 0.0, height = 0.0;
    double viewX = 0.0, viewY = 0.0, viewW = 0.0, viewH = 0.0;
    std::vector<ArtElement> elements;  // element 0 is painted first (bottom)
};

// Serializer options for artDocumentToSvg(). The defaults reproduce the
// historic output byte-for-byte; the Export dialog drives the rest.
struct ArtSvgOptions {
    int decimals = 4;      // 0-6 significant decimals for coordinates
    bool setViewbox = true;
    bool lineBreaks = true;
};

// Serialize to a standalone SVG document. Returns an empty string when the
// document has no size.
std::string artDocumentToSvg(const ArtDocument& doc);
std::string artDocumentToSvg(const ArtDocument& doc, const ArtSvgOptions& opt);

// The decoder-rebuilt shape/paint pair (vector_shape.h). Forward-declared so
// this file stays independent of the shape builders.
struct VectorShape;
struct GradientFill;

// Convert a decoder-rebuilt shape into retained art, so a native .af shape
// exports as real vector geometry instead of an embedded bitmap. The shape's
// anchors are expected in document space; `tx`/`ty` shift them into the layer's
// source space (normally -left, -top) so the layer's own placement keeps
// applying. The stroke is round-capped and round-joined to match the
// rasteriser. Returns null when the shape has no subpaths.
std::shared_ptr<ArtNode> artNodeFromShape(const VectorShape& shape,
                                          const GradientFill* gradient = nullptr,
                                          double tx = 0.0, double ty = 0.0);

// Expand a profiled stroke into filled outline segments (MoveTo/LineTo/Close
// chains, wound for nonzero fill), in node coordinates. Empty when the node
// has no usable profiled stroke. Shared by the SVG writer and the Qt
// rasterizers so all three draw the same outline.
std::vector<Segment> profiledStrokeSegments(const ArtNode& node);

// Standard base64 (no line breaks). Empty input yields an empty string.
std::string base64Encode(const std::uint8_t* data, std::size_t size);

// Binary round-trip used by the IFP project format's per-layer vector block.
// The encoding is little-endian and self-describing; `decodeArtNode` returns
// nullopt on any truncation or unknown tag, and reports how many bytes it
// consumed through `consumed` when non-null.
std::vector<std::uint8_t> encodeArtNode(const ArtNode& node);
std::optional<ArtNode> decodeArtNode(const std::vector<std::uint8_t>& bytes,
                                     std::size_t* consumed = nullptr);

}  // namespace pittore::vector
