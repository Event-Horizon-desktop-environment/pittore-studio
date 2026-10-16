#pragma once
// SVG → "pieces" import: parses an SVG into a tree of group + shape nodes and
// rasterizes each shape into its own document-space pixel layer, so opening an
// SVG yields the same small separate parts the file is made of (like a PSD
// with groups), instead of one flattened image.
//
// Built on QtCore/QtGui only (QXmlStreamReader + QPainter) — no QtSvg, so the
// same code runs under tests and in the app. Text is rendered through the font
// engine when a QGuiApplication exists and as a filled box approximation
// otherwise.

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QVector>
#include <QXmlStreamReader>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "engine/vector/vector_art.h"

namespace pittore::ui {

struct SvgGradient {
    bool radial = false;
    double x1 = 0.0, y1 = 0.0, x2 = 1.0, y2 = 0.0;  // linear (normalized 0..1 bbox)
    double cx = 0.5, cy = 0.5, r = 0.5;             // radial (normalized bbox)
    bool userSpace = false;                         // gradientUnits="userSpaceOnUse"
    struct Stop {
        double offset;
        QColor color;
    };
    std::vector<Stop> stops;
};

// One clip-path in effect on a node, resolved into the node's local space:
// `toNode` maps the clip's own user space (where the clipPath's shapes were
// authored) into the local space `path` lives in. Identity for the node's
// own clip-path; the map from the referencing ancestor's space otherwise,
// so ancestor transforms land correctly at paint time.
struct SvgClipRef {
    QString id;
    QTransform toNode;
};

// One parsed node: a group or a drawable shape. Transforms accumulate from the
// root; `path` and style live in user space.
struct SvgNode {
    QString name;
    bool group = false;
    // Flattened group: children paint as ONE shared raster row (see
    // flattenBigGroups). Panel shows a single row; art stays null.
    bool flattened = false;
    QTransform transform;  // user units → document units (root viewBox baked in)
    double opacity = 1.0;
    // shape fields (when !group):
    QPainterPath path;
    bool fromText = false;  // built from <text>/<flowRoot>: kept crisp and
                            // individually editable, never merged/flattened
    bool hasFill = false;
    QColor fill;
    std::shared_ptr<SvgGradient> gradient;
    bool hasStroke = false;
    QColor stroke;
    // stroke="url(#id)" resolved against the document's gradients: paints the
    // stroke where `stroke` only carries the flat fallback colour. Null for
    // ordinary solid strokes.
    std::shared_ptr<SvgGradient> strokeGradient;
    double strokeWidth = 0.0;
    // Object refs (empty = none): pattern/hatch fill, markers,
    // clipPath/mask/filter ids, and retained mesh + filter graphs.
    QString patternId;
    QString patternTransform;
    QString markerStart, markerMid, markerEnd;
    QString clipId, maskId, filterId;
    // Effective clips in paint order: every ancestor's clip-path (outermost
    // first), then this element's own. A clip-path on a group clips its whole
    // subtree, so each descendant rasterizes against the intersection. Empty
    // for unclipped leaves; `clipId` keeps the element's own ref alone (that
    // is what retained-art export rewrites).
    QVector<SvgClipRef> clipStack;
    std::shared_ptr<pittore::vector::MeshGradient> mesh;
    std::shared_ptr<pittore::vector::FilterGraph> filter;
    // Embedded raster (<image>): decoded bitmap painted into `path` (its
    // x/y/width/height placement rect). `imageFit` mirrors
    // preserveAspectRatio: 0 = meet (default), 1 = none, 2 = slice.
    std::shared_ptr<QImage> image;
    quint8 imageFit = 0;
    QVector<SvgNode> children;  // paint order
};

// Defs cells: pattern tiles, marker glyphs, clip/mask art, filter graphs,
// mesh gradients and rebuildable shape refs (for <textPath> targets).
struct SvgPatternCell {
    double x = 0.0, y = 0.0, w = 10.0, h = 10.0;
    QString xform;  // patternTransform, may be empty
    QVector<SvgNode> shapes;
};
struct SvgMarkerCell {
    double refX = 0.0, refY = 0.0, w = 3.0, h = 3.0, deg = 0.0;
    bool autoOrient = true;
    bool strokeUnits = true;
    QVector<SvgNode> shapes;
};
struct SvgClipCell {
    QVector<SvgNode> shapes;
};
struct SvgMaskCell {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;  // mask region (0 w/h = auto)
    QVector<SvgNode> shapes;
};
struct SvgShapeRef {
    QString tag;
    // Rebuildable attributes for <textPath> targets (d/points/geometry).
    QXmlStreamAttributes attrs;
};
struct SvgResources {
    QHash<QString, std::shared_ptr<SvgGradient>> grads;
    // Single-color paint servers (`<solidcolor>`): id to resolved flat
    // colour. Flattens at the fill site like a one-stop gradient.
    QHash<QString, QColor> solids;
    QHash<QString, SvgPatternCell> patterns;
    QHash<QString, SvgMarkerCell> markers;
    QHash<QString, SvgClipCell> clips;
    QHash<QString, SvgMaskCell> masks;
    QHash<QString, pittore::vector::FilterGraph> filters;
    QHash<QString, pittore::vector::MeshGradient> meshes;
    QHash<QString, SvgShapeRef> shapeRefs;
    // Decoded <image> bitmaps: tallied for the import report (shared by
    // <use> clones and pattern tiles, so counted once at decode).
    int images = 0, imagesDropped = 0;
    qint64 imageBytes = 0;
};

struct SvgSceneRoot {
    double widthPx = 0.0, heightPx = 0.0;      // explicit width/height in px (0 = absent)
    double vbX = 0.0, vbY = 0.0, vbW = 0.0, vbH = 0.0;  // viewBox (all 0 = absent)
    QVector<SvgNode> children;                  // top level, paint order
    SvgResources res;                           // defs: gradients, patterns,
                                                // markers, clips, masks,
                                                // filters, meshes, shape refs
};

// Parses a chunk of SVG XML into a scene tree. Returns false (with *error)
// when the XML is not parseable or nothing drawable was found.
bool svgPartsParse(const QByteArray& xml, SvgSceneRoot* out, QString* error);

// Per-import diagnostics: counts and capped issue maps that let the log say
// whether an SVG rendered faithfully. All increments are plain integers
// (issue keys allocate only for rare misses), so an 80k-leaf file pays
// nothing measurable. Emitted as one summary line plus warning lines.
struct SvgImportDiag {
    qint64 bytesIn = 0;
    qint64 bytesNorm = 0;
    // "not-needed" | "expanded" | "over-cap-skipped".
    QString normNote = QStringLiteral("not-needed");
    int preUses = 0, preSymbols = 0, preImages = 0, preStyles = 0;
    int images = 0;         // embedded rasters decoded
    int imagesDropped = 0;  // undecodable or over the image budget
    qint64 imageBytes = 0;  // decoded pixel weight (RGBA8)
    int leaves = 0;   // leaves pushed by the parts walker
    int groups = 0;   // group rows kept
    int cutAfterOverflow = 0;  // elements skipped once the budget tripped
    bool overflow = false;
    int overflowAt = 0;  // leaf count when the budget tripped
    qint64 geometryBytes = 0;   // retained weight walked
    qint64 geometryBudget = 0;  // machine-scaled cap in force
    bool rowCap = false;   // too many final rows: single-raster fallback
    bool areaCap = false;  // too much raster area: single-raster fallback
    int rowsCounted = 0;   // final rows after merging + flattening
    qint64 rasterArea = 0;  // estimated row raster pixels
    int texts = 0, textPaths = 0, flowTexts = 0, blankTexts = 0;
    bool fontGlyphs = false;  // true when the font engine shaped text
    int gradDefs = 0, patternDefs = 0, markerDefs = 0, clipDefs = 0;
    int maskDefs = 0, filterDefs = 0, meshDefs = 0;
    int gradUses = 0, patternUses = 0, clipUses = 0, maskUses = 0;
    int filterUses = 0, markerUses = 0, meshUses = 0;
    int skippedLeaves = 0;  // unsupported tags + undecodable images
    int emptyLeaves = 0;  // empty geometry (d="" etc): paints nothing, kept
                          // out of the degraded verdict but still reported
    // tag -> dropped count (unsupported elements only, first 16 tags).
    QHash<QString, int> skipTags;
    // "kind#id" -> count for refs that resolve to nothing (first 16).
    QHash<QString, int> missingRefs;
    int mergedFrom = 0, mergedTo = 0;  // top-level children, pre/post merge
    int flatRows = 0, flatMembers = 0;
    int sealedGroups = 0;  // groups sealed into shared rows (rescue path)
    int chunkedRuns = 0;   // loose-leaf runs chunked (rescue path)
    bool flattenFallback = false;  // single-raster overflow path taken
    int layersPixel = 0, layersGroup = 0, layersFlatArt = 0, layersShared = 0;
    int docW = 0, docH = 0, dpi = 96;
    qint64 normMs = 0, walkMs = 0, rasterMs = 0;
    QString error;
};

// One imported piece, already rasterized to document space. `pixels` is null
// for group rows.
struct SvgPartLayer {
    QString name;
    int depth = 0;
    bool group = false;
    QImage pixels;
    QPointF offset;  // document-space position of pixel (0,0)
    double opacity = 1.0;
    // The true vector geometry behind `pixels` (null for groups), kept so the
    // piece can be re-emitted as real SVG rather than an embedded bitmap.
    std::shared_ptr<pittore::vector::ArtNode> art;
    // Member geometry for flattened rows (empty otherwise): one node per
    // merged leaf in the row's source space, so a zoom-settle rebake can
    // re-rasterize the row at display density instead of upscaling the
    // document-resolution bake.
    std::vector<std::shared_ptr<pittore::vector::ArtNode>> flatArt;
    // Sealed group rows (null otherwise): the whole subtree, kept so zoom
    // can re-paint it exactly (all paints, clips, masks included) instead
    // of upscaling the shared raster.
    std::shared_ptr<SvgNode> shared;
    std::shared_ptr<SvgResources> sharedRes;
};

struct SvgImportResult {
    QSize docSize;
    QVector<SvgPartLayer> layers;  // index 0 = top (panel order)
    SvgImportDiag diag;            // what the log verdict is built from
};

// Cooperative progress/cancel for threaded import. All state is atomic: the
// worker writes, the progress dialog polls, and tests observe through
// onUpdate (invoked synchronously on the worker whenever percent advances;
// may set cancelled). Percent is monotonic by construction (phase-weighted,
// max-guarded); cancelled aborts at the next shape/job/phase boundary, and
// a cancelled import returns false with *error left EMPTY (a quiet abort,
// never a warning box). Null progress means legacy synchronous behavior.
enum class SvgImportPhase { Parse = 0, Walk = 1, Raster = 2, Done = 3 };

struct SvgImportProgress {
    std::atomic<int> percent{0};
    std::atomic<int> phase{0};  // SvgImportPhase, as int for atomicity
    std::atomic<bool> cancelled{false};
    std::function<void(int percent, int phase)> onUpdate;
};

// Parses + rasterizes an SVG into separate piece layers (group row per <g>,
// pixel layer per shape), sized from width/height/viewBox or the content.
// Returns false on a hard failure; *dpiOut defaults to 96.
bool svgPartsImport(const QByteArray& xml, SvgImportResult* out, int* dpiOut,
                    QString* error, SvgImportProgress* progress = nullptr);

// Whether a wxh canvas fits the flatten fallback's single raster under the
// given pixel budget: degenerate and over-wide (>16384) canvases never do.
// Pure predicate so the gate is unit-testable without allocating the image.
bool svgFlattenSizeOk(int iw, int ih, qint64 pixelBudget);

// SVG path-data → Qt path (shared by the walker, textPath rebuilds and the
// XML editor's d= writer). File-local parsers stay hidden; these wrappers
// carry the external linkage.
QPainterPath svgParsePathData(const QString& d);
// Qt path → engine segments (shared by the importer and the XML editor).
std::vector<pittore::vector::Segment> svgPathSegments(const QPainterPath& path);

struct LayerItem;

// Exact zoom rebake for sealed group rows (see SvgPartLayer::shared): paints
// the retained subtree at display density into the styled fields. `visible`
// culls to a document region (null = whole row) and is remembered in
// LayerItem::styledView so pans past it schedule a fresh bake. Same
// contract as the flattened-row rebake otherwise (pristine pixels only,
// bucket above 1, never composites or signals).
void rebakeSharedRow(LayerItem& l, double zoom,
                     const QRectF& visible = QRectF());

// Paints a retained subtree at display density (row-source → doc via
// `place`): union box through the placement, capped like the single-art
// dense bake, leaves framed into a box*density image with the exact import
// painter. `visible` culls to a document region (null = whole row); the
// baked doc box reports through `bakedBox`. Returns false when density 1
// suffices (base pixels already exact) or nothing paints.
bool sharedRowBake(const SvgNode& root, const SvgResources& res,
                   const QTransform& place, double zoom, const QRectF& visible,
                   QImage* img, QPointF* origin, double* resampleOut,
                   QRectF* bakedBox);

}  // namespace pittore::ui