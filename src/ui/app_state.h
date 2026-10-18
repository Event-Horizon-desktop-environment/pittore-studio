#pragma once
#include <QColor>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPainterPath>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "engine/core/image.h"
#include "engine/color/cmyk_image.h"
#include "engine/core/tonal_ops.h"
#include "engine/compute/brushes/loaders/loaders.h"
#include "engine/compute/brushes/smudge/smudge.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/compute/tone_blend.h"
#include "engine/compute/blend.h"
#include "ui/brushes/pressure_curve.h"
#include "engine/compute/warp.h"
#include "engine/render/layer_style.h"
#include "engine/text/text_engine.h"
#include "engine/vector/vector_art.h"
#include "engine/vector/boolean.h"
#include "ui/svg_parts.h"
#include "ui/project_manager.h"
#include "ui/settings.h"
#include "ui/color_mismatch.h"
#include "ui/theme.h"
#include "ui/tool_registry.h"

namespace pittore::compute {
class Buffer;
class ComputeBackend;
struct HostPlacedLayer;
struct PlacedLayer;
}  // namespace pittore::compute

namespace pittore::io {
struct PsdLayersDoc;
struct AfLayersDoc;
struct AfEncodeResult;
}

namespace pittore::ui {

// Forward-declared here so the class can reference layered-import results;
// the full definitions live in ui/svg_parts.h (included by app_state.cpp).
struct SvgImportResult;

// R20: the Contextual Task Bar reads *task* context while the Options bar reads
// *tool* context. This is the one enum both sides agree on.
enum class TaskContext {
    None,
    Selection,     // marching ants exist
    Text,          // a type layer is being edited
    Crop,          // crop tool is live
    Transform,     // free transform is live
    Path,          // a path is selected
    ShapeLayer,    // a live shape layer is selected
    GenerativeResult,
};

// Stable name for logs ("None", "Text", …) so a tool's own log can say which
// task context its setup ended up in.
const char* taskContextName(TaskContext c);

enum class ScreenMode { Standard, FullScreenWithMenuBar, FullScreen };

// Chrome visibility, driven by Tab (all) and Shift+Tab (panels only).
enum class ChromeVisibility { All, PanelsHidden, AllHidden };

// The 27 blend modes, in menu order with the separators the Layers
// panel draws between families.
const QStringList& blendModeNames();
bool blendModeIsSeparatorBefore(int index);

// Editable settings for a live text layer created by the Type tool. Mirrors
// pittore::text::TextSpec plus the document-space origin and ink colour; the
// layer's `pixels` are always the most recent render of this spec.
struct TextItem {
    QString text;
    QString family = QStringLiteral("sans-serif");
    bool bold = false;
    bool italic = false;
    double size = 48.0;      // em size, document pixels
    int align = 0;           // 0 = left, 1 = centre, 2 = right
    double lineHeight = 1.0; // multiple of the face's natural line step
    double emLeading = 0.0;  // > 0 = line advance as an em fraction, replaces
                             // the natural step entirely
    double paragraphGap = 0.0;  // distance after every paragraph break, px
    double tracking = 0.0;   // extra spacing per character, document pixels
    double wrapWidth = 0.0;  // > 0 = frame text that reflows, 0 = artistic
    double frameHeight = 0.0;  // frame text box height (overlay only)
    QPointF origin{0, 0};    // document-space top-left of the first line
    QColor color = QColor(0, 0, 0);

    // --- Character panel --------------------------------------------------
    int underline = 0;   // 0 none, 1 single, 2 double
    int strike = 0;
    // Decorations and the highlight box inherit the fill when the colour is
    // invalid or fully transparent.
    QColor underlineColor;
    QColor strikeColor;
    QColor backgroundColor;
    double baselineShift = 0.0;  // document pixels, positive = up
    double hScale = 100.0;       // percent; 100 = natural width
    double vScale = 100.0;       // percent; 100 = natural height
    int superSub = 0;            // -1 subscript, 0 none, +1 superscript
    bool allCaps = false;
    bool kerning = true;
    unsigned otFeatures = 0;     // pittore::text::OTF_* bits
};

struct LayerItem {
    enum class Kind { Pixel, Text, Shape, Adjustment, Group, SmartObject, Frame };

    QString name = QStringLiteral("Layer 1");
    Kind kind = Kind::Pixel;
    bool visible = true;
    bool locked = false;
    bool lockTransparency = false;
    bool clipped = false;          // clipped to the layer below
    int opacity = 100;
    int fill = 100;
    QString blendMode = QStringLiteral("Normal");
    bool hasMask = false;
    bool maskLinked = true;
    bool maskSelected = false;
    bool maskEnabled = true;
    // Mask-panel properties, finished on demand (see mask_finish.h)
    // so the backends keep sampling plain coverage: density multiplies
    // coverage (0..1, default 1) and feather blurs the mask (gaussian radius
    // in mask-native px, default 0). Both bake into PSD export and Apply.
    float maskDensity = 1.0f;
    float maskFeather = 0.0f;
    // A layer mask is opaque-grey coverage in layer-native pixels: r=g=b is
    // the reveal amount (0 hides, 1 reveals) and a stays 1. The sampler reads
    // coverage from R so resampling averages coverage rather than
    // coverage×coverage. `maskStamp` revisions the device cache.
    std::shared_ptr<pittore::Image> mask;
    QPointF maskOffset{0, 0};  // document coords of mask pixel (0,0)
    double maskScaleX = 1.0;   // document pixels per mask pixel (always > 0)
    double maskScaleY = 1.0;
    std::uint64_t maskStamp = 0;
    // Live adjustment (Kind::Adjustment only): `adjustmentKind` is an
    // pittore::compute::AdjustmentKind value (0 = None, i.e. a legacy
    // stand-in with no effect); `adjustmentParams` carries the kind's floats
    // (see adjust.h). `adjustStamp` revisions the device LUT cache.
    int adjustmentKind = 0;
    float adjustmentParams[16] = {};
    // Curves LUT: 3x256 floats (R, G, B back to back), each channel's own
    // curve pre-folded with the RGB composite (master). Empty unless Curves.
    std::vector<float> adjustmentLUT;
    // Live Tone Blend Group (Kind::Group + toneBlendGroup): the group's
    // composite is re-graded against the composited backdrop beneath it at
    // composite time (see DocumentItem::rebuildComposite). Params follow
    // research/R102 + docs/live-tone-blend-research.md; contentType is 0
    // image / 1 vector / 2 text (rasterization hint, auto-set from the first
    // child at creation). Rides undo snapshots with the layer vector, free.
    bool toneBlendGroup = false;
    pittore::compute::ToneBlendParams toneBlend;
    // Curves control points in normalized coords (input x, output y). The
    // LUT above is always derived from these by rebuildAdjustmentLUT, so the
    // points stay the editable source of truth for the curve editor and for
    // project persistence. `adjustmentCurve` is the RGB composite (master);
    // R/G/B hold per-channel curves, empty meaning identity.
    QVector<QPointF> adjustmentCurve;
    QVector<QPointF> adjustmentCurveR;
    QVector<QPointF> adjustmentCurveG;
    QVector<QPointF> adjustmentCurveB;
    std::uint64_t adjustStamp = 0;
    int indent = 0;                // group nesting depth
    bool groupExpanded = true;
    // --- Artboard payload (a board IS a Kind::Group row: D1, INV-011) -----
    // A board is a container group that additionally clips to `boardRect`
    // and renders a fill behind its children. It carries a nonzero boardId;
    // every ordinary Group has boardId == 0. Structure, visibility, opacity,
    // indent and the whole group machinery treat it as a group — that is the
    // point (§5.2 INV-011): no parallel tree, no second container kind, and
    // every one of the ~150 `kind == Group` container checks keeps working.
    // Board layer opacity reuses `opacity` (INV-022: it multiplies the
    // subtree, exactly like a group's); there is deliberately no
    // separate boardOpacity to desynchronise from it.
    std::uint64_t boardId = 0;     // 0 = not a board (INV-006: never reused)
    QRectF boardRect;              // global document coords (INV-001, D2)
    bool boardHasFill = false;     // false = transparent: the pasteboard
    QColor boardFill;              //   shows through (REQ-005)
    double boardFillOpacity = 1.0; // fill opacity, separate from `opacity`
    bool isBoard() const { return boardId != 0; }
    QColor swatch = QColor(0x6e, 0x6e, 0x6e);  // stand-in for the thumbnail
    QString adjustmentType;                     // when kind == Adjustment
    // The layer's pixels are rendered glyphs (a recovered TxtA/TxtF layer, or
    // a live text layer being edited). The Layers panel then shows the whole
    // text fitted inside its thumbnail and labels the row with the text, and
    // the canvas never rescales it as if it were a photo.
    bool isText = false;
    // A live, editable text layer (created by the Type tool) whose `textSpec`
    // is the source of truth. Recovered text is `isText` but not `liveText`:
    // its glyphs are all that survived the import.
    bool liveText = false;
    TextItem textSpec;
    // Layout metrics cached by the last text render, for the caret overlay.
    double textLayoutWidth = 0.0;
    double textFirstBaseline = 0.0;
    double textLineAdvance = 0.0;

    // Pixel content (Kind::Pixel). nullptr for non-pixel layers. The engine's
    // Image (straight-alpha RGBAf, host memory) is the source of truth; it is
    // composited into DocumentItem::composite by the compute backend.
    //
    // The image keeps its NATIVE pixels: a placed or dropped photo stays at
    // full camera resolution here no matter how it is moved or scaled, so
    // scaling down and back up loses nothing. `offset`/`scale` map source →
    // document (doc = offset + scale * src); identity (offset 0, scale 1 with
    // document-sized pixels) is the classic pixel layer. The compositor
    // resamples on the fly — bilinear on upscale, box-averaged on downscale.
    std::shared_ptr<pittore::Image> pixels;
    // Device CMYK ink samples retained for a CMYK document. RGB `pixels`
    // remain the display/render representation; native brush strokes update
    // both, while RGB-boundary operations refresh the sidecar from appearance.
    std::shared_ptr<pittore::color::CmykImage> cmykPixels;
    std::uint64_t cmykSourceStamp = 0;
    QPointF offset{0, 0};   // document coords of source pixel (0,0)
    double scaleX = 1.0;    // document pixels per source pixel (always > 0)
    double scaleY = 1.0;

    // Monotonic revision of `pixels`. Bumped whenever the native pixels
    // mutate in place (brush dab, erase); the compositor's device-side copy
    // (see DocumentItem::PaintStage::sources) keys on it so a paint stroke
    // never composites stale GPU memory.
    std::uint64_t sourceStamp = 0;

    // Packed native pixels STASHED for a layer imported hidden behind a
    // flattened base: straight RGBA8, deferredWidth*deferredHeight*4 bytes.
    // QByteArray is implicitly shared, so undo snapshots and panel copies stay
    // free — unlike `pixels`, which costs 16 B/px as RGBAf. A layer nobody has
    // inspected therefore keeps a quarter of the memory it used to, and only
    // expands (materializeLayerPixels) when it is shown, exported or painted.
    // The stash is the 8-bit source the decoder expanded to 16-bit, so
    // expanding and re-stashing is lossless. Empty for ordinary layers.
    QByteArray deferredRgba8;
    quint32 deferredWidth = 0;
    quint32 deferredHeight = 0;

    // Impasto relief 0..1 in layer-native px, parallel to `pixels`
    // (row-major, same dims). Null = flat. Shared with undo snapshots
    // exactly like pixels: clone before in-place edits (see
    // copyOnWriteActiveHeight). Deposited by lightness-map stamp dabs,
    // transported by smudge; shaded at composite time.
    std::shared_ptr<std::vector<float>> heightMap;

    // Live (Smart) filter: a non-destructive recipe over the native pixels.
    // The filtered render is cached like the styled raster and re-rendered
    // only when pixels, id, params or enabled change; disabling or removing
    // reveals the untouched natives. Composes under layer styles (the style
    // renders over the filtered base).
    bool hasLiveFilter = false;
    bool liveFilterEnabled = true;
    QString liveFilterId;
    std::vector<double> liveFilterParams;
    // Cached filtered render (same dims as `pixels`), or null when no live
    // filter applies. Rebuilt by ensureLayerFilter; never mutated in place.
    // `mutable`/GUI-thread only, like `styled`.
    mutable std::shared_ptr<pittore::Image> filtered;
    mutable std::uint64_t filteredStamp = 0;
    mutable std::string filteredId;
    mutable std::vector<double> filteredParams;
    mutable bool filteredValid = false;

    // Non-destructive layer style: the effect parameters the Layer Style dialog
    // edits. Rendered over `pixels` at composite time (see layerSource), never
    // baked into them, so switching an effect off or cancelling the dialog
    // leaves the layer's own content untouched.
    pittore::render::LayerStyle style;

    // Cached render of `style` over `pixels`, grown by the style's outset and
    // placed at `styledOffset` (the layer's own offset shifted by the outset's
    // scaled reach). Rebuilt by layerSource whenever the pixels or the style
    // change; `styled` is never mutated in place, so snapshots may share it.
    // `mutable`/GUI-thread only, like `thumbnail`.
    mutable std::shared_ptr<pittore::Image> styled;
    mutable QPointF styledOffset{0, 0};
    mutable std::uint64_t styledStamp = 0;
    mutable bool styledValid = false;
    // Revision of the effective (possibly styled) source: bumped whenever the
    // styled raster is rebuilt or dropped, including on a placement change that
    // leaves sourceStamp alone. The compositor's device cache keys on it so a
    // moved styled layer re-uploads instead of sampling a stale raster.
    mutable std::uint64_t styledRev = 0;
    // Placement the styled raster was built for; a scale invalidates it, while
    // a pure translation only moves `styledOffset` (the raster is anchored to
    // the layer, so moving it never re-renders the effects).
    mutable QPointF styledBaseOffset{0, 0};
    mutable double styledBaseScaleX = 1.0;
    mutable double styledBaseScaleY = 1.0;
    // Bake factor of the styled raster: effects are rendered into a raster
    // capped at `kMaxStyledEdge` pixels on the long edge (so a 48MP layer
    // never budgets a CPU effect pass at 48MP), and the compositor resamples
    // it back to document scale via layerDrawSource (scaleX = scaleY = 1/r).
    // 1.0 for rasters baked at their full document footprint.
    mutable double styledResample = 1.0;
    // Document region a culled dense bake covers (shared-row zoom rebakes
    // paint only the visible viewport). Empty = whole footprint. The
    // rebake is only reused while it still covers the viewport, so panning
    // past it schedules a fresh bake instead of showing stale tiles.
    mutable QRectF styledView;
    // Cached doc-space geometry footprint behind the last zoom-density
    // decision (single-art bake or flattened-row union box, WITHOUT the
    // stroke margin the bake framing adds). Lets the zoom-settle sweep
    // predict the density bucket without walking any QPainterPath: when the
    // stamp + placement key still matches, an unchanged bucket skips both
    // the geometry walk and the re-rasterize. Pinned by
    // styledFootprintStamp (sourceStamp for single-art rows, flatStamp for
    // flattened rows); the two row kinds never share a layer, and each
    // rebake path additionally requires its own geometry present, so a
    // stale box from the other kind can never validate.
    mutable QRectF styledFootprint;
    mutable std::uint64_t styledFootprintStamp = 0;

    // Cached live-draw paths for the canvas: the QPainterPath built from
    // QPainterPath built from `art` plus the variable-width stroke outline.
    // Geometry is immutable per ArtNode, so the cache keys on the node
    // pointer + sourceStamp (bumped by every art replacement, so a recycled
    // address can never validate stale geometry; undo snapshots share the
    // same node pointer and stamp, which validate correctly). Implicitly
    // shared: snapshots copy it for free. GUI-thread only, like `thumbnail`.
    mutable QPainterPath livePathCache;
    mutable QPainterPath liveStrokeCache;
    mutable bool liveStrokeBuilt = false;
    mutable const void* livePathKey = nullptr;
    mutable std::uint64_t livePathStamp = 0;

    // Cached Layers-panel preview (see layerThumbnail). Empty until the panel
    // first asks for it; the model clears it whenever the layer's pixel content
    // changes so a stale preview is never shown. `mutable` because the panel
    // builds it lazily through a const layer reference. GUI-thread only.
    mutable QImage thumbnail;

    // Cached combined-content preview for a Group row (see groupThumbnail):
    // the source-over composite of the group's visible pixel descendants,
    // stair-stepped with groupThumbnailStamp (a hash of every descendant's
    // content + placement + visibility). Cleared implicitly whenever the stamp
    // no longer matches; GUI-thread only, like `thumbnail`.
    mutable QImage groupThumbnailCache;
    mutable std::uint64_t groupThumbnailStamp = 0;

    // True vector geometry when this layer came from an SVG import: the paths
    // and paint that produced its pixels. Shared and immutable, so undo
    // snapshots copy it for free and nothing can mutate it in place. Null for
    // every other layer (photos, paint, text, decoded formats). The SVG
    // exporter emits it verbatim and embeds `pixels` only as a fallback.
    std::shared_ptr<const pittore::vector::ArtNode> art;

    // Member geometry for flattened SVG rows (empty otherwise): the merged
    // leaves behind the row's shared raster, in the row's source space. Lets
    // a zoom-settle rebake re-rasterize the row at display density instead
    // of upscaling the document-resolution bake. Shared and immutable like
    // `art`. `flatStamp` pins the pixel content the geometry was baked from:
    // paint bumps sourceStamp, and the rebake refuses to run past it, so a
    // painted row keeps its pixels instead of reverting to geometry.
    std::vector<std::shared_ptr<const pittore::vector::ArtNode>> flatArt;
    std::uint64_t flatStamp = 0;
    // Sealed group rows (null otherwise): the whole subtree behind the
    // row's shared raster, with the resources it paints against. Same
    // zoom-rebake deal as flatArt (exact QPainter path, so clips, masks,
    // patterns and text survive), same flatStamp pin on the pixels.
    std::shared_ptr<SvgNode> sharedNode;
    std::shared_ptr<SvgResources> sharedRes;

    // Verbatim PSD additional-info blocks (TySh, SoLd, lfx2, vmsk, ...)
    // preserved from PSD import. Re-emitted on PSD export only when the
    // layer is untouched (kind, stamps and placement match the import
    // values below), so a painted/moved layer never ships stale
    // descriptors. Dropped by IFP save (native format keeps our model;
    // PSD→IFP→PSD loses foreign editability, pixels survive).
    struct PsdRawBlock {
        char sig[4] = {'8', 'B', 'I', 'M'};
        char key[4] = {};
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> padding;
    };
    std::vector<PsdRawBlock> psdRawBlocks;
    // Original channel compression seen at PSD import (0/1/2/3, -1 when
    // authored here): untouched ZIP-origin layers re-emit ZIP (see
    // buildLayeredPsdDoc) instead of churning methods on every save.
    int psdChannelMethod = -1;
    std::uint64_t psdRawSrc = 0;    std::uint64_t psdRawAdj = 0;
    std::uint64_t psdRawMask = 0;
    bool psdRawHadMask = false;
    Kind psdRawKind = Kind::Pixel;
    QPointF psdRawOffset{0, 0};
    double psdRawScaleX = 1.0;
    double psdRawScaleY = 1.0;
};

struct HistoryItem {
    QString name;
    QString iconKey;
    bool snapshot = false;
};

// A Note-tool annotation: a small marker at a document point carrying the
// author's text. View-side only — never touches a layer's pixels.
struct DocNote {
    QPointF pos;
    QString text;
    QString author;
    QColor color = QColor(0xff, 0xd2, 0x2b);
};

// A Count-tool marker. `group` is the Count Group it belongs to, so the tool
// can keep several independent counters in one document.
struct CountMarker {
    QPointF pos;
    int group = 1;
};

// The pixels the compositor should draw for a layer, with their placement: the
// layer's styled render (document-space, scale 1) when a style is active, else
// its own `pixels` with the layer's offset/scale. Builds (or reuses) the styled
// render, so this may be the expensive call; `img` is null when the layer has
// no pixel content yet.
struct LayerDrawSource {
    std::shared_ptr<pittore::Image> img;
    QPointF offset{0, 0};
    double scaleX = 1.0;
    double scaleY = 1.0;
};

// One pixel row selected for compositing, with its already-resolved mask and
// clipping state. `source` retains its shared pixels; `mask` borrows the
// layer's mask image for the duration of the composite. Adjustment rows
// carry no source: they transform the composite-so-far, with `adjAux`
// borrowing 256 curve-LUT floats for Curves.
struct CompositedLayer {
    int layerIndex = -1;
    LayerDrawSource source;
    const pittore::Image* mask = nullptr;
    QPointF maskOffset{0, 0};
    double maskScaleX = 1.0;
    double maskScaleY = 1.0;
    bool clipped = false;
    int clipBase = -1;
    bool isAdjustment = false;
    int adjKind = 0;
    float adjP[16] = {};
    const float* adjAux = nullptr;
    // Solid board fill (D1): constant `fillColor` over `window`, no `source`.
    bool isFill = false;
    pittore::RGBAf fillColor;
    float fold = 1.0f;
    QString blendMode = QStringLiteral("Normal");
    QRect window;  // already doc-clipped, may be empty
};

// One-line summary of a gathered stack for slow-rebuild attribution, e.g.
// "[Normal,Multiply+M,Hue+A3]": mode per layer, +M masked, +C clipped,
// +A<kind> live adjustment. Built per rebuild (O(layers), trivial) and
// printed only by the gated breakdown logs.
inline QString describeStack(const std::vector<CompositedLayer>& gathered) {
    // Capped: an 84k-layer stack dumps half a megabyte per slow rebuild.
    // Head + tail keeps attribution (top/bottom layers matter) while the
    // count preserves scale.
    constexpr std::size_t kCap = 12;
    QString out = QLatin1String("[");
    const std::size_t n = gathered.size();
    const std::size_t head = std::min(n, kCap);
    for (std::size_t i = 0; i < head; ++i) {
        if (i) out += QLatin1Char(',');
        const CompositedLayer& e = gathered[i];
        if (e.isAdjustment)
            out += QStringLiteral("A%1").arg(e.adjKind);
        else if (e.isFill)
            out += QStringLiteral("F");
        else
            out += e.blendMode;
        if (e.mask) out += QLatin1Char('M');
        if (e.clipped) out += QLatin1Char('C');
    }
    if (n > head)
        out += QStringLiteral("...(%1 more)").arg(n - head);
    out += QLatin1Char(']');
    return out;
}
// One open document. Pixel layers hold an engine Image and are composited by
// the compute backend into `composite` (the QImage the canvas draws). See
// CanvasView::paintDocument and DocumentItem::rebuildComposite for the seam.
class DocumentItem {
  public:
    DocumentItem(QString title, QSize size, int dpi);
    // Defined in app_state_composite.cpp (needs the PaintStage definition
    // there): the staging buffers die with the document.
    ~DocumentItem();

    QString title;
    QString filePath;
    // Source file an image document was imported from (flat image, layered
    // PSD/AF, SVG parts). Empty for native/new documents. Session restore
    // reopens through here when filePath (the native project) is empty;
    // proxies reuse it for the same reason. Never part of undo snapshots.
    QString importSourcePath;
    QSize size;
    int dpi = 300;
    QString colorMode = QStringLiteral("RGB/8");
    QString profile = QStringLiteral("sRGB IEC61966-2.1");
    // ICC bytes of the document's own CMYK separation (imported CMYK
    // source, or the destination picked at Image > Mode > CMYK). Empty for
    // RGB/Grayscale documents; feeds CMYK export so a round trip separates
    // through the same profile. Resolution order for export lives in
    // ui/color_mode.h (doc bytes > settings > system probe > naive).
    QByteArray iccProfile;
    bool dirty = false;
    // Canvas bottom: what a fresh document shows where nothing is painted.
    // Erasing the bottom-most (Background) layer restores this instead of
    // punching transparency; a transparent bottom erases to transparency.
    // Plain documents default to the historic canvas gray; the New Project
    // dialog sets white/black/transparent explicitly.
    QColor canvasPaper{0xf2, 0xf2, 0xf2};
    bool canvasTransparent = false;
    // Monotonic counter bumped by every composite (full rebuild or region
    // recomposite): a cheap generation stamp that lets the canvas cache
    // derived geometry (e.g. the Move gizmo bounds of a large group) instead
    // of walking the whole layer stack on every pointer move or repaint.
    std::uint64_t revision = 0;

    // View state (per-document, restored when the tab is reactivated).
    double zoom = 1.0;
    QPointF pan{0, 0};
    double rotation = 0.0;

    QVector<LayerItem> layers;   // index 0 = top, matching the Layers panel
    int activeLayer = 0;

    QVector<int> selectedLayers;

    // --- Artboards (D1) --------------------------------------------------
    // Ordered board index: the boardId of every board row, in panel and
    // export order. NOT a second source of membership — a board is a Group
    // row with boardId != 0, and this list is derived from the tree. It
    // exists so listing, export order and drag-reorder have a stable order
    // without scanning the whole stack (INV-007: it is a permutation of the
    // board rows). boardOrderIsPermutation() validates that in debug builds.
    QVector<std::uint64_t> boardOrder;
    // Board the view is pointed at (status bar suffix, zoom-to-board). This
    // is view state, distinct from the *selection* (selectedLayers can hold
    // board rows like any row). Captured by the snapshot so undoing "create
    // board" does not leave a dangling active board (INV-014).
    std::uint64_t activeBoard = 0;
    // Monotonic board-id counter: never reused within a document, including
    // across undo/redo and save/load (INV-006). Persisted in .psc v7 (§8.3
    // ABOR) so ids survive a round trip. 0 is reserved as "not a board".
    std::uint64_t boardIdCounter = 0;
    // REQ-028: dropping content over a board reparents it into that board.
    // A per-document toggle; Affinity has no such toggle and it is a
    // documented, requested defect there (§1.11), so we ship it in v1 as a
    // deliberate deviation.
    bool autoReparent = true;

    QVector<HistoryItem> history;
    int historyPosition = 0;

    QRectF selection;            // empty = no selection
    bool selectionIsEllipse = false;
    // Arbitrary-shape selection as a document-resolution grayscale channel
    // (0..255 coverage). Authoritative when selectionIsMask; `selection` then
    // holds the mask's bounding box for the bbox-driven consumers (crop,
    // export bounds, transform handles, copy source). selectionStamp is bumped
    // on every mask change so the canvas can cache the vector outline.
    QImage selectionMask;
    bool selectionIsMask = false;
    quint64 selectionStamp = 0;

    QVector<double> horizontalGuides;
    QVector<double> verticalGuides;

    // Annotation tools (Color Sampler / Note / Count / Ruler). Drawn only by
    // the canvas overlay and captured by the undo snapshot, so add/move/delete
    // is reversible for the life of the session.
    QVector<QPointF> colorSamples;   // Color Sampler pins, document space
    QVector<DocNote> notes;
    QVector<CountMarker> countMarkers;
    QPointF rulerStart{0, 0};
    QPointF rulerEnd{0, 0};
    bool rulerHasMeasurement = false;
    // Area tool: measured rectangle in document pixels (same undo and
    // annotation plumbing as the ruler line).
    QRectF areaRect;
    bool areaHasMeasurement = false;

    // Slice tool (R19): rectangular export regions in document pixels, drawn
    // and edited on the canvas, listed in the Slice Select options bar.
    QVector<QRect> slices;
    // Index of the slice Slice Select currently has selected (-1 = none). Not
    // part of the undo snapshot: a restored slice list simply drops it.
    int selectedSlice = -1;

    QImage composite;            // layered composite drawn by the canvas

    // Large-image proxy: when the source file's full dimensions exceed the
    // import pixel budget, the document opens at 1/proxyFactor scale with a
    // box-averaged proxy. fullSize/sourcePath record the true image for the
    // status bar and for future full-res tile paging; isProxy is false for
    // normal documents.
    bool isProxy = false;
    int proxyFactor = 1;
    QSize fullSize;
    QString sourcePath;

    QString statusText() const;

    // Full re-composite (structural changes: layer add/remove, rebuilds the
    // composite QImage and resets the persistent staging buffers).
    void rebuildComposite();

    // Region-limited re-composite for light edits (visibility toggles): only
    // `r` is recomposited from the visible stack; pixels outside are kept.
    // A canvas-sized toggled layer (the background) costs the same as a full
    // rebuild — correctness unaffected.
    void recompositeRegion(const QRect& r) { renderRegion(r); }

    // --- layer-stack queries (groups) --------------------------------------
    // The ancestor Group rows of `index`, innermost first (empty for a
    // top-level layer). A Group at g encloses every following row until one
    // with indent <= g.indent. Pure structure scan; O(depth) per call.
    QVector<int> enclosingGroups(int index) const;
    // Effective visibility for compositing: the row's own `visible` AND every
    // enclosing group's — a hidden group hides its whole subtree, exactly like
    // the group eye does. Collapse (groupExpanded) is UI-only and
    // never affects rendering.
    bool effectivelyVisible(int index) const;
    // Batch version of effectivelyVisible() + parentGroupIndex() for every
    // row in ONE linear pass (indent-stack sweep). Hot paths (gather,
    // view-order, paint) must use this: the per-index queries scan back
    // O(n) rows per call, turning every full sweep into O(n²) — ~14M
    // iterations per frame on a 5k-layer document. `parent[i]` is the
    // innermost enclosing group or -1, identical to parentGroupIndex().
    void effectiveVisibility(QVector<char>& vis, QVector<int>& parent) const;
    // Whether the Layers panel should draw a row for `index`: false when any
    // enclosing group is collapsed, so a collapsed group shows only its own
    // row. UI-only; has no effect on the composite.
    bool rowHiddenByCollapsedGroup(int index) const;

    // --- artboard queries (boards) -----------------------------------------
    // A board is a Group row with boardId != 0 (D1, INV-011). These resolve
    // between board rows, board ids and layer rows without scanning the stack.
    // Point queries are O(depth). DocumentItem is not a QObject, so these are
    // pure queries; the undoable mutations that emit live on AppState.
    // Row index of the board row carrying `id`, or -1 if absent (INV-007).
    int boardRowIndex(std::uint64_t id) const;
    // Owning board id for a layer row: the innermost enclosing board, or 0 for
    // pasteboard content (D3). O(depth).
    std::uint64_t boardAt(int layerRowIndex) const;
    // Layer rows inside the board at `boardRow` (its subtree, INV-011), in
    // stack order. O(subtree).
    QVector<int> boardChildRows(int boardRow) const;
    // True when `boardOrder` contains exactly the board rows, each once
    // (INV-007). Debug/restore validator; O(n).
    bool boardOrderIsPermutation() const;
    // Batch board-ownership sweep: for every row, the innermost enclosing
    // board's id, or 0 for pasteboard content (D3). ONE indent-stack pass,
    // mirroring effectiveVisibility — the paint path must not call the O(depth)
    // boardAt() per row or a full sweep goes O(n²) (§16.2). `board[i]` is
    // identical to boardAt(i).
    void boardOwnership(QVector<std::uint64_t>& board) const;

    // Incremental repaint: paintDab records only the touched pixel rect here;
    // a later flush composites + blits just that rect (persistent staging in
    // the .cpp). Empty = nothing pending.
    QRect paintDirty;

    // Wash-stroke overlay footprint (document space): union of the regions
    // folded from the wash scratch into the live composite. The tile
    // cache's simple tier blits own layer pixels (which a wash stroke
    // never touches until bake), so layers intersecting this rect must
    // fall back to the composite tier or the stroke flickers under ready
    // tiles. Set by flushPaint's fold, cleared by bake/endStrokeState.
    QRect washOverlayDirty;

    // The compute backend this document composites on. Set by AppState when the
    // document is created and when preferences change; nullptr until then (the
    // CPU reference backend is created lazily inside the .cpp).
    pittore::compute::ComputeBackend* backend = nullptr;

    // Switch to a different compute backend: discards the engine staging buffers
    // (they belong to the old backend) and re-composites from scratch. Callers
    // must not have dabs in flight when they call this.
    void setBackend(pittore::compute::ComputeBackend* be);

    // --- undo/redo ---------------------------------------------------------
    // A full-document snapshot. The layer stack shares its pixel Images via
    // shared_ptr, so a snapshot is cheap (one refcount bump per image); in-place
    // pixel edits copy-on-write the edited layer's Image BEFORE mutating (see
    // AppState::copyOnWriteActiveLayer) so past snapshots never observe later
    // strokes.
    struct DocumentSnapshot {
        QVector<LayerItem> layers;
        int activeLayer = 0;
        QVector<int> selectedLayers;
        QRectF selection;
        bool selectionIsEllipse = false;
        QImage selectionMask;
        bool selectionIsMask = false;
        quint64 selectionStamp = 0;
        QVector<HistoryItem> history;
        int historyPosition = 0;
        QVector<QPointF> colorSamples;
        QVector<DocNote> notes;
        QVector<CountMarker> countMarkers;
        QPointF rulerStart{0, 0};
        QPointF rulerEnd{0, 0};
        bool rulerHasMeasurement = false;
        QRectF areaRect;
        bool areaHasMeasurement = false;
        // Canvas size for crop/artboard undo (layers ride along via their
        // shared Images; offsets shift with the origin).
        QSize docSize;
        QColor canvasPaper;
        // Image > Mode retags these; undo must restore them or the tag
        // outlives the pixels it describes.
        QString colorMode;
        QByteArray iccProfile;
        QVector<QRect> slices;
        // --- Artboards (D7) ---
        // A board is a Group row, so its payload rides `layers` above for
        // free. These are the document-level board pieces that do not: the
        // ordered index, the view's active board, the id counter (so undo
        // never hands out an id that a snapshot still references, INV-006)
        // and the auto-reparent toggle.
        QVector<std::uint64_t> boardOrder;
        std::uint64_t activeBoard = 0;
        std::uint64_t boardIdCounter = 0;
        bool autoReparent = true;
    };

    // The undo engine: up to maxUndoSteps() pre-action snapshots plus a redo
    // tail. beginUndoAction() snapshots the current state; commitUndoAction()
    // promotes it to the undo stack, appends the display-history entry and
    // clears the redo tail; discardUndoAction() drops it (aborted gesture).
    void beginUndoAction();
    void commitUndoAction(const QString& name, const QString& iconKey);
    void discardUndoAction();
    // Undo depth cap (Settings > Performance > History). Process-wide;
    // AppState syncs it from Settings.toml on load and
    // on every applySettings. Defaults to kDefaultMaxUndoSteps.
    static int maxUndoSteps();
    static void setMaxUndoSteps(int n);
    static constexpr int kDefaultMaxUndoSteps = 100;
    bool canUndo() const { return !undoStack_.isEmpty(); }
    bool canRedo() const { return !redoStack_.isEmpty(); }
    int undoDepth() const { return undoStack_.size(); }
    int redoDepth() const { return redoStack_.size(); }
    void resetHistory();
    static constexpr int kMaxUndoSteps = 100;

  private:
    struct PaintStage;                                   // acc/work + engine backend ref
    std::unique_ptr<PaintStage> stage_;

    void ensurePainter();
    void drawVectorOverlays();
    void renderRegion(const QRect& region);
    void renderRegionTone(const QRect& region);

    // Cached device copy of a placed layer's native pixels (GPU backends).
    // Creates or refreshes the cache on demand and returns the buffer the
    // backend samples from in composite_placed.
    const pittore::compute::Buffer& placedSourceFor(const LayerItem& l);
    // Cached device copy of a layer mask's opaque-grey coverage. Null when
    // the layer has no live mask.
    const pittore::compute::Buffer* maskSourceFor(const LayerItem& l);
    // Finished (density/feather-baked) mask for compositing, or null when
    // the raw mask applies. Cached in the paint stage; shared by the device
    // upload path and the CPU reference path so both sample identical bytes.
    const pittore::Image* finishedMaskFor(const LayerItem& l);
    // Cached device copy of a Curves layer's 256-entry LUT. Null unless the
    // layer is a live Curves adjustment with a LUT.
    const pittore::compute::Buffer* adjAuxSourceFor(const LayerItem& l);
    // Bottom→top pixel rows touching `region`, with masks, folds and clip
    // groups already resolved. Shared by the CPU and GPU composite paths so
    // both see the same stack.
    void gatherCompositedLayers(const QRect& region,
                                std::vector<CompositedLayer>& out);
    // Drop device slots whose pixel/mask Images are no longer owned by any
    // layer. Called after a full rebuild.
    void prunePlacedSources();
    // Device composite moved out to composite_gpu.cpp. Returns false when a
    // device failure (OOM included) should fall through to the CPU path.
    // Takes the stage buffers by reference so the stage type itself stays
    // local to app_state.cpp.
    bool rebuildCompositeGPU(pittore::compute::ComputeBackend& be,
                             pittore::compute::Buffer& acc,
                             std::vector<std::unique_ptr<pittore::compute::Buffer>>&
                                 toneScratch,
                             std::uint32_t w, std::uint32_t h, bool toneActive);
    // Region twin of the above, moved out to composite_gpu.cpp for the same
    // guard. Region rect is passed through for attribution logging.
    bool renderRegionGPU(pittore::compute::ComputeBackend& be,
                         pittore::compute::Buffer& acc, int x0, int y0, int x1,
                         int y1, std::uint32_t w, std::uint32_t h,
                         const QRect& region);
    // Incremental device refresh after an in-place brush dab: copies only
    // `layerRect` (layer-native pixels of the draw source) into the staged
    // slot and pushes just that region to the device. Direct path only —
    // when the draw source is the native pixels (no live filter/style) and
    // a staged slot already exists; otherwise bumps the stamp so the next
    // gather takes the full refresh path. Keeps the cache coherent: the
    // slot's stamp is left valid because its bytes are current.
    void refreshPlacedRegion(LayerItem& l, const QRect& layerRect);
    // Mask twin of refreshPlacedRegion (mask-native pixels of the raw mask).
    // When density/feather apply, the finished cache is patched in place
    // (patchFinishedMaskRegion: halo-exact) and the staged finished bytes
    // are region-uploaded; only a size mismatch falls back to the stamp bump.
    void refreshMaskRegion(LayerItem& l, const QRect& maskRect);
    // Dab epilogue for pixel-target dabs: region-refresh for a dab at
    // (lcx,lcy)+lradius in a lw×lh target, then drop the panel thumbnail.
    // Replaces the stamp-bump + thumbnail-clear pair at every dab site.
    void touchPixelsAfterDab(LayerItem& l, float lcx, float lcy, float lradius,
                             std::uint32_t lw, std::uint32_t lh,
                             bool preserveInkColor = false);

    DocumentSnapshot captureSnapshot() const;
    void restoreSnapshot(const DocumentSnapshot& s);
    QVector<DocumentSnapshot> undoStack_;
    QVector<DocumentSnapshot> redoStack_;
    std::unique_ptr<DocumentSnapshot> pendingUndo_;

    friend class AppState;                               // drives incremental flushes
};

// Document-space bounds of a layer's content. Layers without pixels yet count
// as document-sized (the identity assumption ensureLayerPixels will realise).
QRectF layerBounds(const DocumentItem& d, const LayerItem& l);

// The live selection as a document-resolution Grayscale8 coverage mask:
// the stored mask when one exists, otherwise the rect/ellipse rasterised.
// All-zero when nothing is selected. Shared by selection ops and the
// mask-from-selection paths so both rasterise identically.
QImage selectionAsMask(const DocumentItem& d);

// True when a layer's verbatim PSD blocks may be re-emitted: the layer is
// untouched since import (kind, pixel/param/mask stamps and placement all
// match). Shared by PSD export and IFP save so both drop stale descriptors
// instead of describing pixels that no longer exist.
bool psdRawBlocksFresh(const LayerItem& l);
// Font family name for a Type-tool family-combo index (see the options bar's
// list). Returns an empty string when the index is out of range.
QString typeFamilyForIndex(int index);
int typeIndexForFamily(const QString& family);

// Map a live text layer's editable state onto the engine's spec, and lay it
// out. Shared by rendering and the editing session's caret/selection so the
// two can never disagree about where a character sits.
pittore::text::TextSpec textSpecFor(const TextItem& item);
pittore::text::TextLayout textLayoutFor(const TextItem& item);

LayerDrawSource layerDrawSource(const LayerItem& l);

// Styled-bake resolution cap (long edge, px). The fx dialog drops it while a
// drag storm is active so live previews bake a small proxy (~16x less work),
// then restores it so the resting state is always full-res. UI-thread only;
// the dialog is modal so no other writer can interleave. Default 2048.
void setStyledBakeCap(double maxEdge);

// Layer bounds grown by the sampler halo (see app_state.cpp) — the exact rect
// a visibility toggle or placement change must recomposite to stay correct.
QRectF stageBounds(const DocumentItem& d, const LayerItem& l);
// True when the layer needs no resampling (document-sized pixels at offset 0,
// scale 1): the compositor takes the memcpy fast path.
bool layerIsIdentity(const DocumentItem& d, const LayerItem& l);
// Display name for an AdjustmentKind value ("Curves", ...). Empty for None
// and unknown values.
QString adjustmentKindName(int kind);
// Properties-panel parameter descriptors in param-index order. Each maps a
// QSlider int range onto the stored real range linearly.
struct AdjustmentParamDesc {
    QString label;
    int sliderMin = 0;
    int sliderMax = 100;
    double realMin = 0.0;
    double realMax = 1.0;
};
QVector<AdjustmentParamDesc> adjustmentParamDescs(int kind);
// Rebuild a Curves layer's 256-entry LUT from its control points (identity
// when empty) and bump its adjust stamp. Call after any curve edit.
void rebuildAdjustmentLUT(LayerItem& l);
// A `box`×`box` premultiplied ARGB32 preview of a layer's own pixel content.
// Regular layers scale to COVER the square (uniform, overflow cropped); a text
// layer passes `contain`, which letterboxes the whole run so every glyph shows.
// — the Layers-panel thumbnail. Built lazily and cached on the layer itself;
// rebuilt only after the model invalidates it (pixel content change). Move/scale
// do not affect it, since it previews the content rather than its placement.
// --- stashed (deferred) layer pixels --------------------------------------
// A layer imported hidden behind a flattened base keeps its native pixels in
// LayerItem::deferredRgba8 (8-bit) instead of realising them as RGBAf. These
// four helpers are the only sanctioned way to cross between the two states.

// True when this layer's pixels are packed away rather than realised.
bool hasDeferredPixels(const LayerItem& l);
// Expand a stashed layer into `pixels`, clearing the stash. Lossless (the
// stash is the 8-bit source) and a no-op when there is nothing stashed.
void materializeLayerPixels(LayerItem& l);
// Realise every stashed layer of `doc`; returns the indices it touched.
// Callers that only need to READ the pixels (export) pass the result to
// stashDeferredPixels() afterwards so the document is not left inflated.
QVector<int> materializeDeferredPixels(DocumentItem& doc);
// Put back the layers materializeDeferredPixels() expanded that are still
// hidden. Never touches a layer the user has since made visible.
void stashDeferredPixels(DocumentItem& doc, const QVector<int>& expanded);

// Returns a null image when the layer has no pixel content. The tap grid is
// bounded, so the cost is O(box²) regardless of the layer's native resolution.
QImage layerThumbnail(const DocumentItem& d, const LayerItem& l, int box,
                      bool contain = false);
// A `box`×`box` grayscale preview of a layer's own mask coverage (white
// reveals, black conceals). Null when the layer has no mask. The preview is
// small and nearest-sampled, so it never rescans a large mask through a box
// filter. A disabled mask gets a red slash over the same coverage.
QImage layerMaskThumbnail(const LayerItem& l, int box);
// The Layers-panel rectangle a layer's mask thumbnail occupies. Shared by row
// painting, hit-testing and name layout so the three can never disagree.
QRect layerMaskThumbRect(const LayerItem& layer);
// A `box`×`box` premultiplied ARGB32 preview of a Group row's combined
// content: the source-over composite (bottom→top) of the group's visible
// pixel descendants with live adjustments applied in stack order, folded by
// each row's opacity×fill — what a group thumbnail shows by default. The
// whole group's bounds fit to COVER the square. Cached on the group's own LayerItem keyed by a content stamp
// (groupThumbnailStamp), so only a real descendant change re-samples. Null
// when the group has no visible pixel content.
QImage groupThumbnail(const DocumentItem& d, int groupIndex, int box);
// Average colour of the document composite over the sample window centred on
// `docPos`. `sampleSize` is the Color Sampler's combo index (0 Point, 1 3×3,
// 2 5×5, 3 11×11, 4 31×31, 5 51×51, 6 101×101). Returns an invalid colour when
// the document has no composite or the point is outside the canvas.
QColor sampleCompositeColor(const DocumentItem& d, const QPointF& docPos,
                            int sampleSize);
// Topmost (index 0 = top) visible pixel layer with realised pixels whose
// bounds contain docPos, or -1. The Move tool's Auto-Select uses this.
int topPixelLayerAt(const DocumentItem& d, const QPointF& docPos);

// The single source of truth the whole UI observes. Panels, the options bar,
// the tools panel and the contextual task bar all bind to these signals rather
// than to each other, which is what makes the layout freely re-dockable.
class AppState : public QObject {
    Q_OBJECT

  public:
    explicit AppState(QObject* parent = nullptr);
    ~AppState() override;

    // --- tools ------------------------------------------------------------
    ToolId activeTool() const { return activeTool_; }
    // `reason` only feeds the per-tool setup log ("switch", "cycle-group",
    // "temporary-push", "liquify-revert") so a log says why a tool changed.
    void setActiveTool(ToolId id, const char* reason = "switch");
    void cycleToolGroup(char key);          // Shift+<letter>
    void pushTemporaryTool(ToolId id);      // spring-loaded / Space / Ctrl
    void popTemporaryTool();
    bool hasTemporaryTool() const { return temporaryDepth_ > 0; }

    // Per-tool option values, keyed "<tool>/<option-id>".
    QVariant option(ToolId tool, const QString& id) const;
    void setOption(ToolId tool, const QString& id, const QVariant& value);
    // Store an option value without pushing it to the document: used when the
    // UI mirrors the active layer (e.g. entering a text session).
    void setOptionSilently(ToolId tool, const QString& id, const QVariant& value);
    QVariant option(const QString& id) const { return option(activeTool_, id); }

    // --- stamp-tip library (user-imported PNG/GBR/GIH/ABR tips) ------------
    // Stamps are keyed by an id (usually the file name under the config
    // brushes dir) and cached in memory; the Brushes panel owns persistence.
    void setBrushStamp(const QString& id,
                       const pittore::compute::StampTip& tip);
    const pittore::compute::StampTip* brushStamp(const QString& id) const;
    void clearBrushStamp(const QString& id);
    // Display name of the last applied brush preset (empty = none yet).
    // Set by preset application; manual slider tweaks keep the base name.
    void setActiveBrushPresetName(const QString& name) {
        brushPresetName_ = name;
    }
    QString activeBrushPresetName() const { return brushPresetName_; }

    // --- paper-grain pattern library ---------------------------------------
    // Grayscale tiles (0 = hole, 1 = full) keyed by pattern id; the Brushes
    // panel owns disk. Values live here so dab kernels can borrow them.
    struct PatternGray {
        std::uint32_t w = 0, h = 0;
        std::vector<float> gray;
        bool valid() const {
            return w > 0 && h > 0 && w <= 1024 && h <= 1024 &&
                   gray.size() == std::size_t(w) * h;
        }
    };
    void setBrushPattern(const QString& id, PatternGray pattern);
    const PatternGray* brushPattern(const QString& id) const;
    void clearBrushPattern(const QString& id);

    // --- hose library (multi-cell tips) ------------------------------------
    // Whole hoses keyed by hose id; the Brushes panel rebuilds the map from
    // presets on every load/mutation, so no per-hose delete is needed.
    void setBrushHose(const QString& id,
                      const pittore::compute::brushload::LoadedHose& hose);
    const pittore::compute::brushload::LoadedHose* brushHose(
        const QString& id) const;
    void clearBrushHoses();

    // --- per-stroke brush state ---------------------------------------------
    // One RNG + seed per stroke (scatter/density/hose-pick), the smudge
    // brush's held paint (reset to the foreground at stroke start), and the
    // paper-grain origin in document space (random per stroke when the
    // preset asks for it, else the stored offsets). Canvas press/release
    // bracket every pixel stroke with these; tools that skip the bracket
    // get a default-constructed state (seeded once, grain at origin).
    void beginStrokeState(ToolId tool, std::uint64_t seed = 0);
    void endStrokeState();
    // Pinned seed for deterministic replay (tests): when non-zero, strokes
    // use it instead of a random seed. Production leaves it at 0.
    void pinStrokeSeed(std::uint64_t seed) { pinnedSeed_ = seed; }
    double strokeRandom();  // uniform [0,1)
    std::uint64_t strokeSeed() const { return strokeSeed_; }
    pittore::compute::SmudgeCarry& smudgeCarry() { return smudgeCarry_; }
    QPointF strokePatternOrigin() const { return strokePatternOrigin_; }
    // Authored pressure response for the live stroke (parsed from the
    // tool's brush_*_curve options at stroke start; inactive = built-in).
    const pittore::ui::brushcurve::Curve& strokeSizeCurve() const {
        return strokeSizeCurve_;
    }
    const pittore::ui::brushcurve::Curve& strokeOpacityCurve() const {
        return strokeOpacityCurve_;
    }
    const pittore::ui::brushcurve::Curve& strokeFlowCurve() const {
        return strokeFlowCurve_;
    }
    const pittore::ui::brushcurve::Curve& strokeRotationCurve() const {
        return strokeRotationCurve_;
    }

    // --- colours ----------------------------------------------------------
    QColor foreground() const { return foreground_; }
    QColor background() const { return background_; }
    void setForeground(const QColor& c);
    void setBackground(const QColor& c);
    void swapColors();        // X
    void resetColors();       // D
    const QVector<QColor>& swatches() const { return swatches_; }
    void addSwatch(const QColor& c);

    // --- modes ------------------------------------------------------------
    bool quickMask() const { return quickMask_; }
    void setQuickMask(bool on);
    ScreenMode screenMode() const { return screenMode_; }
    void setScreenMode(ScreenMode m);
    void cycleScreenMode(bool reverse = false);
    ChromeVisibility chromeVisibility() const { return chrome_; }
    void setChromeVisibility(ChromeVisibility v);
    int surroundIndex() const { return surround_; }
    void cycleSurround(bool reverse = false);
    UiTheme theme() const { return theme_; }
    void setTheme(UiTheme t);

    // --- snapping ---------------------------------------------------------
    // Move-drag snapping: the canvas snaps layer edges/centers to guides, the
    // grid, slices, other layers and the document bounds. Target flags
    // combine; SnapTargetsAll enables every target. The menu owns the UI;
    // state lives here so the canvas and tests agree.
    enum SnapTarget {
        SnapGuides = 1,
        SnapGrid = 2,
        SnapDocumentBounds = 4,
        SnapSlices = 8,
        SnapLayers = 16,
        SnapTargetsAll = SnapGuides | SnapGrid | SnapDocumentBounds |
                         SnapSlices | SnapLayers,
    };
    bool snapEnabled() const { return snapEnabled_; }
    void setSnapEnabled(bool on);
    int snapTargets() const { return snapTargets_; }
    void setSnapTargets(int targets);
    void setSnapTarget(SnapTarget target, bool on);

    // --- canvas "Show" defaults -------------------------------------------
    // Persisted mirrors of the CanvasView visibility toggles (View menu +
    // Settings > Canvas). Writers update the live canvas themselves first;
    // these only keep Settings.toml in sync so the state survives restarts.
    // No signal: nothing observes these except startup + the dialog.
    void setShowRulers(bool on);
    void setShowGuides(bool on);
    void setShowGrid(bool on);
    void setShowSelectionEdges(bool on);
    void setShowSmartGuides(bool on);
    void setShowPixelGrid(bool on);
    void setShowExtras(bool on);

    // --- soft-proof view state --------------------------------------------
    // Session-only toggles (View menu, not persisted): the proof/gamut
    // setup itself (profile, intent, BPC) persists in Settings and is
    // applied through applySettings. proofChanged drives the canvas
    // repaint; nothing else observes these.
    bool proofEnabled() const { return proofEnabled_; }
    void setProofEnabled(bool on);
    bool proofGamut() const { return proofGamut_; }
    void setProofGamut(bool on);

    // --- preferences -------------------------------------------------------
    // Settings are owned here, loaded in the constructor and written back to
    // disk whenever they change (theme changes included). applySettings() swaps
    // the live compute backend and re-composites every open document.
    const AppSettings& settings() const { return settings_; }
    // Tablet mode (enlarged tool targets): persists via applySettings and
    // broadcasts settingsChanged for the tool strip/persona bar to rebuild.
    void setTabletMode(bool on) {
        if (settings_.tabletMode == on) return;
        AppSettings next = settings_;
        next.tabletMode = on;
        applySettings(next);
    }
    bool tabletMode() const { return settings_.tabletMode; }
    // Brush cursor preferences (cursor shape + outline split): same
    // persist-via-applySettings pattern as tablet mode. The canvas reads
    // these live; no recomposite needed, so setters skip the heavy path
    // by routing through applySettings like everything else.
    void setCursorShape(int v) {
        v = std::clamp(v, 0, 2);
        if (settings_.cursorShape == v) return;
        AppSettings next = settings_;
        next.cursorShape = v;
        applySettings(next);
    }
    int cursorShape() const { return settings_.cursorShape; }
    void setOutlineShape(int v) {
        v = std::clamp(v, 0, 3);
        if (settings_.outlineShape == v) return;
        AppSettings next = settings_;
        next.outlineShape = v;
        applySettings(next);
    }
    int outlineShape() const { return settings_.outlineShape; }
    void setShowOutlineWhilePainting(bool on) {
        if (settings_.showOutlineWhilePainting == on) return;
        AppSettings next = settings_;
        next.showOutlineWhilePainting = on;
        applySettings(next);
    }
    bool showOutlineWhilePainting() const {
        return settings_.showOutlineWhilePainting;
    }
    void setOutlineEffectiveSize(bool on) {
        if (settings_.outlineEffectiveSize == on) return;
        AppSettings next = settings_;
        next.outlineEffectiveSize = on;
        applySettings(next);
    }
    bool outlineEffectiveSize() const {
        return settings_.outlineEffectiveSize;
    }
    // The compute backend in active use (settings-driven; caller holds no ref).
    pittore::compute::ComputeBackend& computeBackend() const;
    // Apply new preferences: updates settings_, persists them, rebuilds the
    // backend if the compute section changed, re-creates paint staging on every
    // document and emits settingsChanged().
    void applySettings(const AppSettings& next);
    QString computeDeviceLabel() const;   // status-bar text for the live backend

    // --- documents --------------------------------------------------------
    const QVector<DocumentItem*>& documents() const { return documents_; }
    DocumentItem* activeDocument() const;
    int activeDocumentIndex() const { return activeDocument_; }
    void setActiveDocumentIndex(int index);
    DocumentItem* addDocument(const QString& title, QSize size, int dpi);
    void closeDocument(int index);

    // --- own-format projects ---------------------------------------------
    // Create a new project document (adds the document, fills its background
    // per `data.background` and persists the project file immediately).
    // Returns nullptr with *error set on failure (RAM limit gate, bad name).
    DocumentItem* createNewProject(const ProjectFileData& data,
                                   QString* error = nullptr);
    // Load a project file into a new document (own format, or anything
    // saveProjectFile produced). On success the new document becomes active
    // and the file is added to the recent list. Returns false with *error
    // when the file is not a valid project.
    bool openProject(const QString& path, QString* error = nullptr);
    // Canvas bottom for freshly opened files: buckets the bottom layer's
    // origin pixel (pristine at load) into transparent/black/white, the
    // same buckets the project saver writes, so erase-to-background
    // matches what the file shows. Photos land on white (never a sampled
    // sky-blue); transparent files punch through.
    void deriveCanvasPaper(DocumentItem& doc);
    // Open ANY supported image or project file into a new document. .psc loads
    // natively (layers preserved); every other format is decoded flattened —
    // the built-in PSD/XCF/KRA codecs plus Qt's image plugins (PNG/JPEG/TIFF/
    // WebP/GIF/BMP/… whatever the runtime provides) — and imported as a single
    // pixel layer. The source path is remembered by Open Recent but not set as
    // filePath, so Save prompts for the native format. Returns false with
    // *error set when the file cannot be decoded.
    bool openImageFile(const QString& path, QString* error = nullptr);
    // Embedded-profile policy for imports: the "embedded profile mismatch"
    // workflow. resolveImportedProfile maps an embedded profile name to the
    // tag the new document should carry, or nullopt when the user cancels
    // (callers then roll back the document and stay quiet: *error is left
    // empty on Cancel). Missing/empty embedded profiles are silently tagged
    // with the working space — the conventional default — while matching
    // families never prompt unless the policy is AlwaysAsk, under which any
    // embedded bytes prompt. Policies Ask/AlwaysAsk delegate to the mismatch
    // resolver (MainWindow shows the dialog); without one they fall back to
    // Convert.
    void setProfileMismatchResolver(
        std::function<ImportProfileChoice(const QString& embedded,
                                          const QString& working)>
            fn);
    std::optional<QString> resolveImportedProfile(const QString& embedded,
                                                DocumentItem* doc = nullptr);
    // Tags `doc` per resolveImportedProfile; false only on user Cancel.
    bool applyImportedProfile(DocumentItem& doc, const QString& embedded,
                              QString* error = nullptr);
    // Deferred questions: the image paints first, the dialog follows.
    // While deferral is on, the Asking policies store the
    // question instead of prompting and tag provisionally; the host drains
    // it after syncing + repainting, so the dialog opens over the visible
    // image. Silent policies resolve inline as usual.
    void setDeferMismatchDialogs(bool on);
    bool hasPendingProfileMismatch() const;
    // Ask the pending question now (no-op true when none). False means the
    // user cancelled: the document was already rolled back.
    bool resolvePendingProfileMismatch(QString* error = nullptr);
    // Layered-import helpers used by openImageFile: PSD/PSB layers+groups are
    // converted into LayerItems (groups from folder records, blends/opacity/
    // visibility, unicode names); SVG pieces become separate shape layers under
    // group rows. Both build a document, select the top layer and register the
    // file with Open Recent.
    bool openPsdLayers(const QString& path, const pittore::io::PsdLayersDoc& layers,
                       QString* error = nullptr,
                       const QString& embeddedProfile = {});
    // Layered PSD export (4A, defined in ui/psd_export.cpp): the inverse of
    // openPsdLayers — panel order becomes file order with group
    // opacity/visibility unfolded, raw native pixels, masks as coverage
    // grids, adjustments at full-doc rect. Returns the encoded file bytes.
    // Non-const because it realises stashed layers for the read (and packs
    // them back afterwards) — see materializeDeferredPixels.
    std::optional<std::vector<std::uint8_t>> exportLayeredPsd(
        DocumentItem& doc, QString* error = nullptr) const;
    // Layered Affinity export (defined in ui/af_export.cpp): like the PSD
    // path but targeting the .af graph model. Baked/lossy content is
    // reported in the result's skipped list for the dialog to show.
    std::optional<pittore::io::AfEncodeResult> exportAfLayers(
        DocumentItem& doc, QString* error = nullptr) const;
    bool openSvgParts(const QString& path, const SvgImportResult& svg, int dpi,
                      QString* error = nullptr);
    // Affinity (.af/.afphoto/.afdesign/.afpub): the layered codec (af_layers.h)
    // recovers the bitmap layers from the metadata tree + delta tile stream;
    // `canvasHint` is the flattened embedded-preview size used to size the
    // document when the tree's canvas is not mapped yet. `flattenedBase` — the
    // embedded preview raster, the known-good flattened render of the page —
    // becomes an opaque full-canvas base layer so the document shows the same
    // picture as the pre-layered import; the recovered layers stay listed but
    // are hidden by default (their placement transform is not mapped yet).
    // Takes the decode non-const: each layer's RGBA16 buffer is released as it
    // is consumed (see openAfLayers), which is what keeps a large document
    // from holding its decode and its panel model at the same time.
    bool openAfLayers(const QString& path, pittore::io::AfLayersDoc& layers,
                      const QSize& canvasHint, const QImage& flattenedBase,
                      QString* error = nullptr);
    // Serialize the active document into a project file. Sets the file as
    // the document's filePath, clears dirty and adds it to the recent list.
    // Returns false with *error set when saving fails.
    bool saveProject(const QString& path, QString* error = nullptr);
    // The active document's project file, or empty when it has none.
    QString activeProjectPath() const;
    // R98: write the active document to `path` as a recovery snapshot without
    // touching its filePath / dirty flag / recent list. Used by the crash-safety
    // autosave, so a partial save never masquerades as a real Save.
    bool writeRecoverySnapshot(const QString& path, QString* error = nullptr);
    // Prepend `path` to the most-recent project list (dedup, capped, saved).
    void noteRecentProject(const QString& path);
    // Session restore (reopen file-backed documents on startup): `file` is
    // JSON {"version":1,"documents":[absolute paths...],"active":index}.
    // saveSession records filePath, else importSourcePath, plus the active
    // index; false when nothing is restorable. restoreSession reopens each
    // existing file (.psc/.ifp via openProject, anything else via openImageFile),
    // skips missing files and garbage, clamps the active index, and returns
    // the restored count. Unsaved work is not preserved (matches quit).
    bool saveSession(const QString& file) const;
    int restoreSession(const QString& file);

    // --- task context -----------------------------------------------------
    TaskContext taskContext() const { return context_; }
    void setTaskContext(TaskContext c);

    // Convenience mutators that keep context + history + composite coherent.
    void pushHistory(const QString& name, const QString& iconKey);
    void setSelection(const QRectF& rect, bool ellipse);
    // Replace the selection with an arbitrary shape: a document-resolution
    // grayscale image (0..255 coverage). The bounding box (>127) becomes the
    // stored selection rect for bbox consumers. A null/empty mask deselects.
    void setSelectionMask(QImage mask);
    void clearSelection();
    // Replace the selection with `mask` as one undoable step (snapshot taken
    // before the change; the History-panel entry comes from the undo step
    // itself). Object Select, Select Subject and Refine Selection commit this
    // way — unlike pushHistory, Undo genuinely reverts the selection.
    void replaceSelectionMask(QImage mask, const QString& undoName,
                              const QString& iconKey);
    // Selection ▸ From Layer Transparency (Ctrl+click a layer's thumbnail in
    // the Layers panel): load the layer's own alpha channel as a doc-resolution
    // selection mask — soft edges kept (partial coverage), outline traced at
    // 50% opacity by the canvas's marching ants. Zero for pixel layers; groups
    // take the source-over union of their visible pixel descendants' alpha
    // (bottom→top), so the whole logo's outline loads from its group row. One
    // undoable step; returns false (with a status hint) when there is nothing
    // to select.
    bool selectFromLayerAlpha(int layerIndex);
    // Inverse selection (Select ▸ Inverse / Ctrl+Shift+I / task-bar Invert):
    // complement the live selection — an arbitrary-shape mask becomes
    // 255−coverage, a rect/ellipse selection is rasterised and complemented.
    // One undoable step; a no-op when nothing is selected.
    void invertSelection();
    // Combine `incoming` (a document-resolution coverage mask, from Magic Wand
    // or Quick Selection) with the live selection using a combine mode:
    // 0 = new (replace), 1 = add (union), 2 = subtract, 3 = intersect. One
    // undoable step. A null/size-mismatched incoming mask replaces with nothing
    // for mode 0 and is ignored otherwise.
    void combineSelection(QImage incoming, int mode, const QString& undoName,
                          const QString& iconKey);
    // Add a pixel layer from QImage pixels as one undoable step, placed
    // exactly at `offset`/scale (no centering — the caller guarantees
    // placement). Selects the new layer and re-composites.
    void addPixelLayerFromImage(const QImage& img, const QString& name,
                                const QPointF& offset, double scaleX,
                                double scaleY, const QString& undoName,
                                const QString& iconKey);
    void addLayer(LayerItem layer);
    // Remove a layer by index without touching history. Used to discard an
    // empty text layer when a Type-tool editing session ends.
    void removeLayerSilently(int index);
    // Place an image as a new pixel layer that keeps its native pixels
    // (lossless source): the layer is centred on centerDoc at `scale`
    // document pixels per image pixel. Selects the new layer, re-composites,
    // and pushes a Place history entry. No-op when there is no document or
    // the image is null.
    void placeImageLayer(const QImage& img, const QString& name,
                         const QPointF& centerDoc, double scale);
    // Place an already-parsed SVG into the CURRENT document, mirroring
    // placeImageLayer: the parts become shape/group layers centred on
    // `centerDoc` (fit-scaled down when the SVG is larger than the canvas so
    // it is never upscaled, never upscaled when smaller). With no document
    // open it falls back to openSvgParts. Returns false with *error set when
    // the SVG has no usable parts.
    bool placeSvgParts(const QString& path, const SvgImportResult& svg,
                       const QPointF& centerDoc, QString* error = nullptr);
    // Lift the active pixel layer's appearance inside `clip` (doc space;
    // empty = whole document) into a new QImage, scaling each pixel's alpha
    // by the coverage of `mask` at that position (`mask` null = no clipping).
    // `docRect` receives the lifted doc-space rect. Straight alpha: RGB is
    // preserved, alpha = source alpha * coverage. Null when there is no
    // paint-capable active layer or the lifted rect would be empty. Shared by
    // Copy/Paste, Layer via Copy and the Refine Selection outputs.
    QImage copyActiveLayerMasked(const QImage& mask, const QRectF& clip,
                                 QRectF* docRect = nullptr);
    // Refine output: duplicate the active pixel layer over the refined
    // `coverage` channel's bbox (doc-sized Grayscale8), unmixing background
    // fringe from semi-covered edge pixels (foreground decontamination).
    // Without `withMask` the matte is baked into the new layer's alpha — a
    // real cutout, background removed, like Remove Background. With
    // `withMask` the full pixels are kept and the channel rides along as a
    // live layer mask. One undo/history step. False when there is no
    // paint-capable active layer or the coverage is empty.
    bool newDecontaminatedLayer(const QImage& coverage, bool withMask);
    // Move/scale the active pixel layer, re-compositing only the union of the
    // old and new bounds. Scales clamp to [0.01, 100]. Returns false when
    // there is no movable active layer (none, locked, or not a pixel layer).
    bool setActiveLayerPlacement(const QPointF& offset, double scaleX,
                                 double scaleY);
    void removeActiveLayer();
    void setActiveLayerIndex(int index);
    LayerItem* activeLayer() const;

    // --- Vector persona (own folder: ui/persona/vector_edit.cpp) --------------
    // Replace layer `index`'s vector paint and node opacity, re-rasterize its
    // retained geometry and recomposite, as one undoable step. `opacity` < 0
    // keeps the node's current opacity. Returns false when the layer has no
    // retained geometry or the re-raster fails (nothing is mutated).
    bool applyVectorPaint(int layerIndex,
                          const pittore::vector::ArtPaint& paint,
                          double opacity, const QString& undoName);
    // Clear the editable art layer's width profile back to a uniform
    // stroke (one undo step). False with a hint when nothing is editable
    // or the stroke is already uniform.
    bool resetStrokeProfile();    // Node-tool commit: replace the whole retained node (geometry + paint),
    // re-rasterize and recomposite as one undoable step. False when the layer
    // has no retained geometry or the re-raster fails (nothing is mutated).
    bool applyVectorNode(int layerIndex,
                         const pittore::vector::ArtNode& node,
                         const QString& undoName);
    // Shape creation (own folder: ui/persona/vector_shapes.cpp): build the
    // tool's live shape in `docRect` from its options-bar values and add it
    // as a retained-geometry layer, one undo step. False (with a status hint,
    // nothing mutated) for modes without a backend yet (Path/Pixels output,
    // boolean path ops) or invisible paint.
    bool addVectorShapeLayer(ToolId tool, const QRectF& docRect,
                             const QString& undoName);
    // Pen / Freehand / Curvature commit (same folder): `docSegments` is the
    // finished path in document coordinates; it is framed, rasterized and
    // added as a retained-geometry layer, one undo step. Open paths keep
    // stroke-only paint (like Line); closed paths keep the bar's fill.
    // False (with a status hint, nothing mutated) for degenerate paths or
    // invisible paint. The pen tool stays active for the next path.
    bool addVectorPathLayer(
        const std::vector<pittore::vector::Segment>& docSegments, ToolId tool,
        const QString& undoName);
    // Shared creation tail behind addVectorShapeLayer/addVectorPathLayer:
    // frame, rasterize, bake and add `node` (local coords) at `rect`, one
    // undo step. `stayInTool` keeps the creation tool active (Pen keeps
    // drawing); otherwise keep_selected decides between Move and staying.
    bool commitArtNodeLayer(std::shared_ptr<pittore::vector::ArtNode> node,
                            const QRectF& rect, ToolId tool,
                            const QString& undoName, bool stayInTool,
                            const QString& blendMode = QStringLiteral("Normal"));
    // Vector Brush commit (same folder): a filled ribbon outline from the
    // streamed centerline, one undo step. The brush stays active.
    bool addVectorBrushLayer(const std::vector<pittore::vector::Segment>& ribbon,
                             const QColor& color, double opacity,
                             const QString& blendMode, const QString& undoName);
    // Vector Flood Fill (own folder: ui/persona/vector_build.cpp): flood the
    // composite from `docPos`, trace the region into a vector outline layer.
    // Smart refill repaints the hit art instead; Knockout also deletes fully
    // covered art. One undo step; false with a hint when nothing fills.
    bool floodFillVectorArt(const QPointF& docPos);
    // Shape Builder (same folder): `action` 0 Add (combine intersected art
    // into one layer, paint order kept), 1 Delete (remove it), 2 Create
    // (needs booleans — refuses). Click picks topmost; drag uses the box.
    // One undo step; locked rows never join or leave.
    bool shapeBuilderAt(const QRectF& docRect, int action);
    // Text on a path (same folder): lay `text` along the editable art's
    // longest outline in `family` at `sizePt` (document points) and commit
    // the glyph outlines as one fill layer — real vector, baked (the text
    // is no longer editable as text afterwards). One undo step.
    bool textOnPath(const QString& text, const QString& family, double sizePt);
    // Text in a shape (same folder): flow `text` into the editable art's
    // bbox columns in `family` at `sizePt` and commit the glyph outlines as
    // one fill layer. One undo step.
    bool textInShape(const QString& text, const QString& family, double sizePt);
    // Boolean fold (same folder): combine `indices` (top-first art layers)
    // with `op`, replacing the first and removing the rest, one undo step.
    // `extra` appends one more doc-space operand (a just-created shape that
    // is not a layer yet). Paint and name come from the first layer.
    // False with a hint when fewer than two operands or the result is empty.
    bool booleanFoldLayers(const QVector<int>& indices,
                           pittore::vector::BoolOp op,
                           const std::vector<pittore::vector::BoolRing>* extra,
                           const QString& undoName);
    // Vector-direct refresh (own folder: ui/persona/vector_edit.cpp):
    // re-render the moved selection's retained-geometry layers from their
    // nodes at the live placement, so transforms never resample art. Scale
    // folds back to 1 with the offset anchoring the fresh trim (the
    // compositor then always samples art layers 1:1). Post-gesture only —
    // call after commit, never inside a snapshot. True when anything
    // re-rendered.
    bool refreshVectorArt();
    // Zoom-coupled dense bakes (same folder): re-render every art layer's
    // display bake at the current view zoom so zoomed-in art stays crisp.
    // Post-zoom/undo/redo only; true when anything re-baked.
    bool rezoomVectorArt();

    // --- Type tool (live text layers) ---------------------------------------
    // Create an empty live text layer. `origin` is the document-space top-left
    // of the first line; `wrapWidth > 0` makes it frame text that reflows to
    // that width, otherwise artistic (never wraps). Selects the layer and
    // returns its index, or -1 when there is no document.
    int addTextLayer(const QPointF& origin, double wrapWidth, const QString& family,
                     double size, const QColor& color);
    // Re-lay and re-render a live text layer from its spec, replacing its
    // pixels and recompositing the affected region. Returns false when `index`
    // is not a live text layer or no font is available.
    bool refreshTextLayer(int index);
    // Resize a live text layer by setting its point size and layout origin and
    // re-rendering at the new size (glyphs are re-set from their outlines, so a
    // transform-box drag never resamples the old raster). `index` is the layer.
    bool setLiveTextSize(int index, double size, const QPointF& origin);
    // Topmost visible live text layer whose bounds contain `docPos`, or -1.
    int textLayerAt(const QPointF& docPos) const;

    // --- undo/redo engine ---------------------------------------------------
    // Gesture-scoped history: beginUndoStep() when a gesture starts (stroke
    // press, move/scale drag press), then commitUndoStep() on release when the
    // gesture actually changed the document, or discardUndoStep() when it did
    // not. One-shot actions (add/delete layer, place, AI alpha) call both
    // begin+commit internally and never need the canvas.
    void beginUndoStep();
    void commitUndoStep(const QString& name, const QString& iconKey);
    void discardUndoStep();
    // Rewind a live-write gesture: restores the pre-begin snapshot (like
    // undo) instead of merely dropping it like discardUndoStep() does.
    // For modal sessions (Color Grade) whose Cancel must leave no trace.
    void cancelUndoStep();
    // True while a gesture step is open (between begin and commit/discard).
    // Panels use this to coalesce a slider drag into one step: begin on
    // press, live-write through the drag, commit on release.
    bool undoStepActive() const;
    // Rewind/restore to the previous/next snapshot (undo/redo up to 100 steps).
    // Rebuilds the composite and emits layersChanged + activeLayerChanged +
    // selectionChanged + historyChanged + documentModified. No-op with a status
    // hint when there is nothing to step to.
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
    QString undoStepName() const;   // most recent action to be undone
    QString redoStepName() const;   // the step an undo would re-apply
    int undoDepth() const;          // pending+committed steps (for tests/UI)
    int redoDepth() const;

    // --- layer selection ----------------------------------------------------
    // Ctrl+A: select every layer. Multi-selection moves and deletes together —
    // the Move tool drags all selected layers by the same delta and Delete
    // removes them all.
    void selectAllLayers();
    // Collapse the multi-selection back to the active layer only.
    void clearLayerSelection();
    // Shift+click: select the contiguous range from the anchor (active layer)
    // to `toIndex`. With `add` false the range replaces the selection (plain
    // Shift+click); with true it unions with it (Ctrl+Shift+click). The
    // clicked row becomes active. Emits activeLayerChanged.
    void selectLayerRange(int toIndex, bool add = false);
    // Bulk eye toggle: set `visible` on every layer in `indices` as one undo
    // step, re-compositing the union of their footprints. Returns false when
    // nothing changed. Used by the Layers panel eye when the clicked row
    // belongs to a multi-selection, so all selected layers flip together.
    bool setLayersVisible(const QVector<int>& indices, bool visible);
    // The effective selection: `selectedLayers` when non-empty, else
    // {activeLayer}. Clamped to the layer list.
    QVector<int> selectedLayerIndices() const;
    // Layer Style (fx) targets: the effective selection expanded so a
    // selected group folder contributes its whole subtree, filtered to
    // styleable rows (Pixel kind with raster — group headers have nothing
    // to render effects from). Sorted top-first; empty when nothing can
    // take a style. Shared by the fx button entry and the dialog.
    QVector<int> fxTargetLayers() const;
    // Delete every selected layer (refuses, with a status hint, when it would
    // empty the document). Records one undo/history step; emits layersChanged
    // + historyChanged + documentModified.
    void removeSelectedLayers();
    // Move every selected pixel layer to its press-time offset + `delta`
    // document pixels, re-compositing only the union of their old and new
    // bounds. `starts` holds each selected layer's offset captured at gesture
    // press (aligned with selectedLayerIndices()); it must be supplied by
    // interactive drags — without it the delta would accumulate onto an
    // already-moved layer on every mouse event and the image would outrun the
    // cursor. Returns false when nothing moved (no selections, locked/non-pixel
    // layers only, or a null delta).
    bool moveSelectedLayers(const QPointF& delta,
                            const QVector<QPointF>& starts = {});
    // Move the given layer indices by `delta` document pixels (gesture-baseline
    // compatible: `starts`, when aligned with `indices`, holds each layer's
    // press-time offset). Re-composites only the union of old/new bounds. Used
    // by the Move tool to translate a whole group's pixel descendants as a unit.
    bool moveLayersAt(const QVector<int>& indices, const QPointF& delta,
                      const QVector<QPointF>& starts = {});
    // Apply per-layer offset/scale updates to the given pixel layers in one
    // step (used by group scaling). The arrays must match `indices`. Every
    // offset/scale is clamped like setActiveLayerPlacement. Returns false when
    // nothing moved.
    bool setLayerPlacements(const QVector<int>& indices,
                            const QVector<QPointF>& offsets,
                            const QVector<double>& scaleXs,
                            const QVector<double>& scaleYs);

    // --- layer grouping and reordering --------------------------------------
    // Group the selected layers into a new group row. Subtrees of selected
    // group rows travel with them (a selected group is never orphaned). The new
    // group is selected and returned (or -1 when nothing could be grouped).
    int groupSelectedLayers();
    // Wrap the selection in a Live Tone Blend Group: a normal group (same
    // undo-safe path as groupSelectedLayers, so this costs a second history
    // step named for the blend) whose header carries default blend params
    // and whose content type is auto-detected from its first child (shape
    // → vector, live/recovered text → text, else image). A lone selected
    // (else active) layer is wrapped on its own, so one image re-grades
    // against the composite beneath it. Returns the header index, or -1
    // when nothing could be grouped.
    int makeToneBlendGroup();
    // Ungroup selected group rows — or, when no group row is selected, the
    // active layer's parent group. Group headers are removed and their children
    // promoted one level. The promoted rows become the selection. Returns true
    // when anything changed.
    bool ungroupSelectedLayers();
    // Move the selected layers' blocks (leaves and group subtrees) one slot in
    // the stack: step < 0 toward the top/front, step > 0 toward the
    // bottom/back. Respects group nesting (a block never escapes its container
    // past the parent header). Returns true when anything moved.
    bool moveSelectedLayersInStack(int step);
    // Jump the selected layers to the very front (top) / back (bottom) of the
    // stack, subtrees intact. Returns true when anything moved.
    bool bringSelectedLayersToFront();
    bool sendSelectedLayersToBack();
    // Re-parent the selected layers (subtrees intact) to `targetIndex` — the
    // panel slot, in the PRE-move index space, before which the rows should
    // land — at base indent `targetIndent`, preserving each row's depth
    // relative to the block's shallowest row. Used by Layers-panel drag-drop.
    // Returns true when anything moved.
    bool reparentSelectedLayers(int targetIndex, int targetIndent);
    // Duplicate the selected layers (group subtrees travel with their header)
    // in place: each contiguous run of selected rows is copied directly above
    // itself, so the copies sit right over their sources. Pixel content is
    // deep-copied, so painting the duplicate never touches the original;
    // cached previews are cleared so the rows re-render. The copies become the
    // new selection. One undo/history step. Returns false when there is
    // nothing to duplicate.
    bool duplicateSelectedLayers();

    // The pixel-layer indices nested under group `groupIndex` (top-first panel
    // order). The group itself is not included.
    QVector<int> groupPixelDescendantIndices(int groupIndex) const;
    // Union of the doc-space bounds of a group's pixel descendants (empty when
    // the group has none).
    QRectF groupBounds(int groupIndex) const;
    // The outermost group row that contains `index` (index itself included when
    // it is a group), or -1 when the row belongs to no group. Used by the Move
    // tool to select the whole unit when a grouped part is clicked on canvas.
    int outerGroupContaining(int index) const;
    // Toggle the expanded state of a group (click the disclosure chevron).
    // `subtree` = true when Alt/Option is held: recursively sets every group
    // inside this group's subtree. `allGroups` = true for Ctrl/Cmd+click:
    // toggles every group in the document. The change is NOT undoable (it is
    // a UI-state toggle, and doesn't alter content).
    // Emits layersChanged so the panel rebuilds the hidden row set.
    void toggleLayerGroupExpanded(int groupIndex, bool subtree, bool allGroups);
    // Set every group row to `expanded` (Expand/Collapse All Groups in the
    // row context menu). UI-state only; emits layersChanged when it changed.
    void setAllGroupsExpanded(bool expanded);
    // Deep-copy the active pixel layer's Image before an in-place edit so undo
    // snapshots keep the pre-edit pixels. Call once at stroke start, before the
    // first dab. Returns false when there is nothing to copy-on-write.
    bool copyOnWriteActiveLayer();
    // Mask twin of copyOnWriteActiveLayer: clone the active layer's mask
    // before a mask stroke so undo restores pre-stroke coverage.
    bool copyOnWriteActiveMask();
    // Height twin of copyOnWriteActiveLayer: clone the active layer's
    // relief plane before a stroke that deposits or moves relief.
    // No-op (false) when the layer carries no height yet.
    bool copyOnWriteActiveHeight();

    // --- layer masks and clipping ------------------------------------------
    // Add a reveal (`coverage` = 1) or hide (`coverage` = 0) mask to the
    // active pixel layer as one undoable step. An existing mask is selected
    // rather than replaced. Returns false with a status hint when the active
    // row cannot carry a mask.
    bool addLayerMask(float coverage = 1.0f);
    // Fill the active layer's mask with `coverage` (creating a mask when
    // needed) as one undoable step. Used by Reveal All/Hide All.
    bool fillLayerMask(float coverage);
    // Delete/invert/enable/link the active layer's mask, each as one
    // undoable content step (selection and linking only change UI state and
    // are not undoable).
    bool deleteLayerMask();
    bool invertLayerMask();
    bool setLayerMaskEnabled(bool enabled);
    bool setLayerMaskLinked(bool linked);
    bool setLayerMaskSelected(int index, bool selected);
    // Mask-panel properties (defined in ui/mask_finish.cpp): live, no undo
    // step, like the opacity slider. Density 0..1 (clamped), feather >= 0 px.
    bool setLayerMaskDensity(float density);
    bool setLayerMaskFeather(float px);
    // Bake the (finished) mask into pixel alpha and remove it. Pixel layers
    // only; one undoable step.
    bool applyLayerMask();
    // Selection <-> mask interchange (defined in ui/selection_ops.cpp).
    // Reveal/Hide paint the live selection into the active mask (created
    // when missing, intersected otherwise); From Transparency converts
    // pixel alpha; Load brings the finished mask back as a selection.
    // Modify ops wrap the selection_mask primitives. All undoable.
    bool maskRevealSelection();
    bool maskHideSelection();
    bool maskPaintSelection(bool hide);
    bool maskFromTransparency();
    bool loadMaskAsSelection();
    bool modifySelectionExpand(int px);
    bool modifySelectionContract(int px);
    bool modifySelectionFeather(int px);
    bool modifySelectionSmooth(int passes);
    bool modifySelectionBorder(int px);
    // Toggle whether the active pixel layer clips to the nearest base pixel
    // layer below it in the same group. One undoable step.
    bool toggleLayerClipped();

    // --- live adjustment layers --------------------------------------------
    // Create an adjustment layer of `kind` (an AdjustmentKind value) with
    // default parameters as one undoable step. Selects the new row.
    bool addAdjustmentLayer(int kind);
    // Live-edit one float parameter (param-index order follows
    // adjustmentParamDescs) with no undo step — same contract as the Layers
    // panel opacity slider: the composite rebuilds immediately.
    bool setAdjustmentParam(int index, float value);
    // Index-taking variant: edits any adjustment row without moving the
    // selection (the dialog grades several layers per gesture).
    bool setAdjustmentParamAt(int layerIndex, int paramIndex, float value);
    // Replace a Curves layer's control points (normalized), rebuild its LUT
    // and recomposite. Live, no undo step.
    bool setAdjustmentCurve(const QVector<QPointF>& points);
    // Replace one Curves channel (0 = R, 1 = G, 2 = B). Live, no undo step.
    bool setAdjustmentCurveForChannel(int ch, const QVector<QPointF>& points);
    // Restore the active adjustment layer to its creation defaults (params +
    // curves + LUT rebuild + recomposite). Live, no undo step — same contract
    // as the slider rows in the Properties panel.
    bool resetAdjustmentToDefaults();
    // Index-taking variant (same, no selection change).
    bool resetAdjustmentAt(int layerIndex);
    // Momentary before/after preview for the active adjustment layer
    // (press-hold eyeball in Properties): begin hides the row, end restores
    // its prior visibility. Recomposites immediately, takes no undo step and
    // leaves no persistence delta — strictly view-only. begin is a no-op
    // unless the active layer is a visible adjustment; end without begin is
    // a no-op. Emits documentModified only (never layersChanged) so the
    // Properties panel is not rebuilt out from under the held button.
    void beginAdjustmentPreview();
    void endAdjustmentPreview();

    // Paint one brush dab into the active layer at document space. radius,
    // hardness ∈ [0,1] and opacity ∈ [0,1]; color is straight RGB (alpha
    // ignored — per-dab opacity carries it). Returns false when nothing could
    // be painted (no doc, active layer not paint-able). The dab touches the
    // layer's pixels immediately but does NOT rebuild the composite; callers
    // call flushPaint() once per input event so a whole stroke segment updates
    // in one incremental composite. Each stroke should push ONE history entry
    // when the callers know the stroke is over.
    bool paintDab(const QPointF& docPos, double radius, double hardness,
                  double opacity, const QColor& color, double ratio = 1.0,
                  double angleDeg = 0.0, bool squareTip = false);

    // Eraser twin of paintDab: alpha is multiplied down by the dab mask instead
    // of source-over, so existing strokes are removed. Same dirty-rect /
    // flushPaint contract as paintDab.
    bool eraseDab(const QPointF& docPos, double radius, double hardness,
                  double opacity, double ratio = 1.0, double angleDeg = 0.0,
                  bool squareTip = false);

    // Stamp-tip twins of paintDab/eraseDab: the tip's longest side spans the
    // dab diameter. `stampId` must exist in the stamp library (brushStamp);
    // unknown ids are a silent no-op so a missing file can never break a
    // stroke. Hardness is unused for stamps (tips carry their own falloff).
    bool stampDab(const QPointF& docPos, double radius, int mode,
                  double opacity, const QColor& color, double angleDeg,
                  const QString& stampId);
    bool stampEraseDab(const QPointF& docPos, double radius, double opacity,
                       double angleDeg, const QString& stampId, int flip = 0,
                       int filter = 1);
    // Folds an armed wash stroke into the active layer at the tool opacity.
    // No-op without an armed stroke or with an empty scratch. Called on
    // release before the undo step commits.
    bool bakeWashStroke();
    // The active tool's "Mode" option as an engine blend mode when that
    // option is the painting tools' shared blend list (Multiply, Screen, …).
    // Normal when the option is unset or index 0, and for combo names the dab
    // kernels do not implement (Behind/Clear map to Normal, the same fallback
    // blend::from_display_name applies). Tools that reuse the "mode" id for
    // their own enum (Color Replacement's recolor modes, the Eraser's tip
    // shapes) always resolve to Normal.
    pittore::compute::BlendMode paintBlendFor(ToolId tool) const;
    // Select the tool's declared blend "Mode" option by display name
    // ("Multiply", "Screen", …) as a normal option change (options bar
    // follows). False when the tool declares no such blend option.
    bool setToolBlendMode(ToolId tool, const QString& name);
    // Direct-tip cores: stampDab/stampEraseDab resolve the id then call
    // these (hose cells already hold their tip, so they call these too).
    bool stampTipDab(const QPointF& docPos, double radius, int mode,
                     double opacity, const QColor& color, double angleDeg,
                     const pittore::compute::StampTip& tip, int flip = 0,
                     int filter = 1);
    bool stampTipEraseDab(const QPointF& docPos, double radius, double opacity,
                          double angleDeg,
                          const pittore::compute::StampTip& tip,
                          int flip = 0, int filter = 1);
    // Direct-tip cores: stampDab/stampEraseDab resolve the id then call
    // these (hose cells already hold their tip, so they call these too).
    // Smudge twins: dirty-brush smearing toward the stroke's held paint
    // (smudgePaint(), reset to the foreground at stroke start). `rate`
    // already folds tool opacity/pressure/flow; `radiusFrac` scales the
    // sampling footprint. `sampleAll` picks up from the composite instead
    // of the active layer; `blend` constrains the smear; `finger == false`
    // primes the held paint from the canvas on the first dab (pure smear),
    // while finger starts wet from the foreground. `smudgeMode` 0 dulls /
    // 1 smears, `colorRate` reloads foreground per dab, `trailX/Y` offset
    // the smear pickup in layer px (all neutral = legacy dulling).
    bool smudgeTipDab(const QPointF& docPos, double radius, double hardness,
                      double ratio, double angleDeg, bool squareTip,
                      double rate, double radiusFrac, bool sampleAll = false,
                      pittore::compute::BlendMode blend =
                          pittore::compute::BlendMode::Normal,
                      bool finger = true, int smudgeMode = 0,
                      double colorRate = 0.0, double trailX = 0.0,
                      double trailY = 0.0);
    bool smudgeStampDab(const QPointF& docPos, double radius, double angleDeg,
                        const pittore::compute::StampTip& tip, double rate,
                        double radiusFrac, int flip = 0, int filter = 1,
                        bool sampleAll = false,
                        pittore::compute::BlendMode blend =
                            pittore::compute::BlendMode::Normal,
                        bool finger = true, int smudgeMode = 0,
                        double colorRate = 0.0, double trailX = 0.0,
                        double trailY = 0.0);

    // Incrementally re-composite the dirty rect accumulated by paintDab since
    // the previous flush and emit documentModified once. Cheap no-op when no
    // dab was painted since the last call.
    void flushPaint();

    // Live dab pressure for the texture/mask resolvers (paintDabAt sets it
    // per dab before calling the dab cores below; defaults to 1.0 for
    // direct callers, which keeps their output bit-identical).
    void setDabPressure01(double p) { dabPressure01_ = p; }

    // --- tonal brushes / bucket fills ---------------------------------------
    // Dodge/Burn/Sponge twin of paintDab: the dab's coverage drives a tonal or
    // saturation operator instead of source-over. `op` is a
    // pittore::compute::ToneOp; `amount` ∈ [0,1]. Same dirty-rect /
    // flushPaint contract as paintDab.
    bool toneDab(const QPointF& docPos, double radius, double hardness,
                 double amount, int op, int range, bool protectTones,
                 bool vibrance);

    // End a tonal stroke (Dodge/Burn/Sponge): drop the accumulated coverage and
    // the pre-stroke snapshot. Safe to call when no stroke is active.
    void endToneStroke();

    // Mixer Brush twin of paintDab: wet-mix the loaded paint with the
    // canvas sample and paint the result. `load01` scales the initial
    // foreground load. CPU only (serial load feedback). Same dirty-rect /
    // flushPaint contract as paintDab.
    bool mixerBrushDab(const QPointF& docPos, double radius, double hardness,
                       double flow, double mix, double wet, double load01,
                       bool sampleAll);

    // Adjustment Brush twin of paintDab: apply one adjustment under the
    // dab mask, scaled by `strength`. `adjIndex` follows the options-bar
    // Adjustment combo; only Brightness/Contrast (0), Exposure (3) and
    // Hue/Saturation (5) are implemented (fixed documented strengths),
    // the rest honestly refuse. CPU runs the shared core; Brightness and
    // Hue ride the device backend on bbox buffers (Exposure is CPU-only).
    // Same dirty-rect / flushPaint contract as paintDab.
    bool adjustmentBrushDab(const QPointF& docPos, double radius,
                            double hardness, double strength, int adjIndex);

    // Remove-tool mark dab: accumulate stroke coverage for the release-time
    // healing fill. Always true when the dab lands on the layer.
    bool removeMarkDab(const QPointF& docPos, double radius, double hardness);
    // Release-time healing fill over the marked bbox (spot-heal grid,
    // donor = layer or composite). Consumes the marks when `consume`.
    // Owns no undo (the stroke opened it); returns whether pixels moved.
    bool removeHealMarked(bool consume, bool sampleAll, int diffusion);
    // End a remove stroke: drop the marks unless they accumulate.
    void endRemoveStroke(bool keep);

    // Blur/Sharpen twin of paintDab: each dab box-blurs its bbox and mixes
    // by the dab mask times `strength` (Sharpen unsharp-mixes with the
    // Protect Detail gate). CPU runs the shared blur_dab.h core; bboxes at
    // or above 96x96 on a GPU backend take the device box_blur fastpath.
    // Same dirty-rect / flushPaint contract as paintDab.
    bool blurSharpenDab(const QPointF& docPos, double radius, double hardness,
                        double strength, bool sharpen, bool protectDetail);

    // Background Eraser twin of paintDab: erase where the layer RGB is
    // within `tolerance01` of the sampled colour (ramped). `sampling` is 0
    // Continuous (resample the dab centre), 1 Once (first dab of the
    // stroke), 2 Background Swatch. `limits` is 0 Discontiguous, 1
    // Contiguous (bbox flood from the centre), 2 Find Edges (currently the
    // contiguous path). Discontiguous runs the shared host core or the
    // device bg_erase fastpath; contiguous runs the bbox flood on the host
    // (serial BFS, bbox-small by construction). Same dirty-rect /
    // flushPaint contract as paintDab.
    bool backgroundEraseDab(const QPointF& docPos, double radius,
                            double hardness, double opacity,
                            double tolerance01, int sampling, int limits,
                            bool protectFg);

    // PatternStamp twin of paintDab: source-over the procedural 64x64 tile
    // (pattern.h) under the dab mask. `patternId` selects the tile (cached
    // per id); `aligned` pins the tile to the document, otherwise to the
    // stroke's first dab; `impressionist` jitters the tile offset per dab
    // from the stroke RNG. Non-Normal modes are refused by the caller with
    // a status hint (same contract as Paint Bucket). CPU runs the shared
    // host core; GPU backends take the device pattern_stamp fastpath with
    // a tile buffer uploaded per dab (64KB). Same dirty-rect / flushPaint
    // contract as paintDab.
    bool patternStampDab(const QPointF& docPos, double radius, double hardness,
                         double opacity, int patternId, bool aligned,
                         bool impressionist);

    // History Brush twin of paintDab: source-over texels from the history
    // source (oldest undo snapshot, i.e. the document-open state) under
    // the dab mask. Non-Normal modes are refused by the caller with a
    // status hint. CPU runs the shared host core; GPU backends take the
    // device history_dab fastpath. Same dirty-rect / flushPaint contract
    // as paintDab.
    bool historyBrushDab(const QPointF& docPos, double radius, double hardness,
                         double opacity);

    // Art History twin of paintDab: stamps the history source with a
    // style-driven offset (curl/length per style index, deterministic per
    // stroke). `areaPx` is the stamp diameter, `tolerance01` gates
    // application to similar colours (<=0 paints everywhere). CPU only
    // (stamp-bounded serial core). Same dirty-rect / flushPaint contract.
    bool artHistoryDab(const QPointF& docPos, double radius, double hardness,
                       double opacity, int style, double areaPx,
                       double tolerance01);

    // --- clone stamp --------------------------------------------------------
    // Clone Stamp twin of paintDab: stamp the source at `offsetDoc` (a
    // document-space delta from dab to source, captured at stroke start)
    // onto the active layer. `opacity`/`flow` follow the usual model (flow builds
    // toward the opacity ceiling across overlapping dabs); `sampleMode` is 0
    // Current Layer, 1 Current & Below, 2 All Layers. The stroke reads a
    // frozen source snapshot so it can never feed its own output back.
    // Same dirty-rect / flushPaint contract as paintDab.
    bool cloneStampDab(const QPointF& docPos, double radius, double hardness,
                       double opacity, double flow, const QPointF& offsetDoc,
                       int sampleMode);
    // End a clone stroke: drop the accumulated coverage and the snapshots.
    // Safe to call when no stroke is active.
    void endCloneStroke();

    // Healing Brush twin of paintDab: heal the dab with the donor (the
    // stroke-frozen source translated by `offsetDoc`, or the tiled
    // `patternId` when `usePattern`). `sampleMode` is 0 Current Layer, 1
    // Current & Below, 2 All Layers (both composite modes read the
    // flattened composite like the Magic Wand measures). The donor is
    // translated once per offset/sample/pattern key — amortized over the
    // stroke's dabs. CPU only (serial donor search, like booleans).
    // Same dirty-rect / flushPaint contract as paintDab.
    bool healBrushDab(const QPointF& docPos, double radius, double hardness,
                      const QPointF& offsetDoc, int sampleMode, int diffusion,
                      bool usePattern, int patternId);
    // End a heal stroke: drop the translated donor. Safe when idle.
    void endHealStroke();

    // Red Eye fix in `boxDoc` (document space): red-dominant texels
    // desaturate and darken by `darken01`. Owns its undo step (click tool:
    // begin/commit inside, discard when nothing red matched).
    bool redEyeAt(const QRectF& boxDoc, double darken01);

    // Content-Aware Tracing: vectorize the committed selection boundary.
    // `output` 0 Path (work-path storage does not exist yet: honest
    // refuse), 1 Selection (simplified polygon filled back to a mask),
    // 2 Shape (polygon vector layer). `detail` 0..100 drives the
    // Douglas-Peucker epsilon (100 = full fidelity). Owns its undo step.
    // CPU only (bbox-sized walk).
    bool traceSelection(int output, int detail);

    // Resize the document canvas: the new top-left lands at `newTopLeftDoc`
    // (layer offsets shift by its negation, unlinked masks ride along) and
    // `newSize` becomes the canvas. No undo management (the caller opened
    // the step); content outside is clipped by the rebuild, new area shows
    // the paper.
    bool resizeCanvas(const QPointF& newTopLeftDoc, const QSize& newSize);

    // --- artboard mutations (undoable; one step each: INV-030) -------------
    // Defined in app_state_boards.cpp. Each targets the active document, does
    // its model work inside a snapshot step, and emits boardsChanged +
    // layersChanged on success. Return the new board's id, or 0/false on
    // failure (no document, invalid rect, locked board, cycle). The board
    // payload rides the DocumentItem's layer vector; DocumentItem is not a
    // QObject, so the signal emission lives here (AppState), matching how
    // grouping and resizeCanvas already work.
    std::uint64_t createBoard(const QRectF& rect, const QString& name);
    bool deleteBoard(std::uint64_t id, bool keepContent);
    bool renameBoard(std::uint64_t id, const QString& name);
    bool setBoardRect(std::uint64_t id, const QRectF& rect);
    bool moveBoardBy(std::uint64_t id, double dx, double dy);
    bool setBoardFill(std::uint64_t id, bool hasFill, const QColor& fill,
                      double fillOpacity);
    bool setBoardVisible(std::uint64_t id, bool visible);
    bool setBoardLocked(std::uint64_t id, bool locked);
    bool reorderBoards(int from, int to);
    bool setActiveBoard(std::uint64_t id);

    // Perspective Crop commit: warp the quad (TL,TR,BR,BL document space)
    // onto an output rect and crop the canvas to it. Output size comes
    // from `outW/outH` when positive, else the quad bbox. Pixel layers
    // (and their masks) resample through the homography; vector/text
    // layers only reposition (hinted). Owns its undo step. CPU only.
    bool perspectiveCrop(const QPointF quad[4], int outW, int outH);

    // Generative Fill / Background entry: validates the request (prompt +
    // selection for fill; prompt for background) and refuses honestly —
    // no generative model is bundled yet. Returns true only when work was
    // queued. The audit treats a missing model as SKIP-MODEL (same id).
    static const char* generativeModelId();
    bool generativeFill(bool background);

    // Patch transfer: heal the committed selection from the donor area at
    // +`deltaDoc` (drop minus press, document space). `mode` 0 Normal
    // (feathered donor copy) or 1 Content-Aware (donor + low-frequency
    // lighting match scaled by `colorMix` 0..10); `featherPx` (Structure)
    // sets the edge feather; `transparent` transfers donor detail onto the
    // target base instead. Donor reads the active layer (or the flattened
    // composite when `sampleAll`); writes land on the active layer.
    // No undo management (the caller opened the step on press). CPU only.
    bool patchTransfer(const QPointF& deltaDoc, int mode, int featherPx,
                       int colorMix, bool sampleAll);

    // Content-Aware Move: paste the committed selection's content at
    // +`deltaDoc` (Move mode heals the vacated hole first via the shared
    // spot-heal grid; Extend mode duplicates, leaving the source intact).
    // `featherPx` (Structure) sets the paste edge; `colorMix` 0..10 scales
    // the low-frequency lighting match on the paste; `sampleAll` reads the
    // donor through the flattened composite. transform_on_drop is not
    // applied. No undo management (the caller opened the step on press).
    // CPU only.
    bool contentAwareMove(const QPointF& deltaDoc, bool extend, int featherPx,
                          int colorMix, bool sampleAll);

    // --- replace-color brush ------------------------------------------------
    // Color Replacement twin of paintDab: recolor the active layer's pixels
    // under the brush that match ANY of `targets` (straight RGB) within
    // `tolerance` ∈ [0,1], keeping each pixel's own shading. `mode` is 0 Hue,
    // 1 Saturation, 2 Color, 3 Luminosity (the W3C non-separable blends in
    // blend.h). `limits` is 0 Discontiguous (every match under the brush),
    // 1 Contiguous (the match region connected to the brush centre), 2 Find
    // Edges (contiguous, stopped at strong luminance edges). With
    // `antialias` the match-region boundary is feathered by one pixel, like
    // the Paint Bucket. `harmony` ∈ [0,1] adapts the transfer to the
    // surroundings (recolored local mean + the pixel's own deviation), so the
    // new colour beds into textured areas instead of flattening them. Alpha
    // is never touched. Same dirty-rect / flushPaint contract as paintDab
    // (no opacity: every dab applies fully; this tool takes no
    // opacity/flow control).
    bool replaceColorDab(const QPointF& docPos, double radius, double hardness,
                         int mode, const std::vector<pittore::RGBAf>& targets,
                         double tolerance, int limits, bool antialias,
                         double harmony);
    // Build the target-colour set for one dab: the averaged colour under the
    // brush centre plus ring samples at ~0.55×radius, clustered greedily (a
    // sample joins an existing target when within half the tolerance of it)
    // and capped at four — so a gradient under the brush matches along its
    // whole span without widening the tolerance into a bleed. Each sample
    // averages a `sampleSize` window (0 point, 1 3×3, 2 5×5, 3 11×11,
    // 4 31×31); transparent samples are skipped. Empty when nothing usable
    // sits under the brush.
    std::vector<pittore::RGBAf> replaceTargetsAt(const QPointF& docPos,
                                                 double radius, int sampleSize,
                                                 double tolerance) const;
    // Explicit target palette for Alt+click: a 9×9 grid over the brush disc
    // (each cell Sample-Size-averaged), clustered the same way but capped at
    // `maxTargets` (48 for the Alt+click gesture) — the whole area's colour
    // range becomes matchable, e.g. an object's full shading span. Empty when
    // the disc holds no visible pixels.
    std::vector<pittore::RGBAf> replacePaletteAt(const QPointF& docPos,
                                                 double radius, int sampleSize,
                                                 double tolerance,
                                                 int maxTargets) const;
    // Nearest active-layer texel under a document point (straight-alpha RGBAf
    // from the layer's native pixels). Used to sample the colour to replace.
    // *ok is false when there is no paintable active layer or the point falls
    // outside its pixels.
    pittore::RGBAf sampleActiveLayerAt(const QPointF& docPos, bool* ok) const;
    // Spot Healing Brush dab: remove the blemish under the brush by
    // texture-replacement (donor detail over destination colour — never a
    // plain copy). `type` follows the options-bar Type toggles (0
    // Content-Aware, 1 Create Texture, 2 Proximity Match), `diffusion` is
    // 1..7, and `sampleAll` measures donors through the composite instead of
    // the active layer (writes still land on the active layer). Same
    // dirty-rect / flushPaint contract as paintDab.
    bool spotHealDab(const QPointF& docPos, double radius, double hardness,
                     int type, int diffusion, bool sampleAll);
    // One-shot Paint Bucket: flood the active layer (or, with Sample All
    // Layers, the region measured through the composite) from `docPos` with the
    // foreground colour. Self-contained history step; flushes its own paint.
    // Returns false when nothing changed or the active layer is not paintable.
    bool paintBucketAt(const QPointF& docPos);

    // One-shot Magic Eraser: flood-down the active layer's alpha from `docPos`.
    // Same contract as paintBucketAt.
    bool magicEraseAt(const QPointF& docPos);

    // One-shot Magic Wand: replace/combine a coverage selection of the
    // contiguous (or global) pixels within Tolerance of the clicked colour.
    // `mode` follows combineSelection; -1 uses the tool's Selection Mode
    // option. Honours Sample All Layers, Anti-alias and Sample Size.
    bool magicWandSelectAt(const QPointF& docPos, int mode = -1);

    // Bake a rotation into the active pixel layer's own pixels (used by
    // Ruler ▸ Straighten Layer). The layer keeps its centre and placement; its
    // native resolution grows to the rotated bounds. One history step.
    bool rotateActiveLayer(double degrees);

    // --- tonal / filter edits (R49, R54) ------------------------------------
    // Interactive dialogs (Levels/Curves/Add Noise/Median/Unsharp Mask) edit
    // the active pixel layer's native Image in place with a live preview.
    // beginTonalEdit opens ONE undo step + copy-on-write and returns the
    // paintable active layer (nullptr when nothing can be edited). The dialog
    // mutates layer->pixels directly and calls applyTonalEditPreview() after
    // each change to recomposite. commitTonalEdit promotes the step;
    // cancelTonalEdit restores the pre-edit pixels and drops the step.
    LayerItem* beginTonalEdit();
    void applyTonalEditPreview();
    // Restore the pre-edit pixels (idempotent): previews always re-apply
    // from pristine, so a drag's endpoint never depends on its path.
    void resetTonalEditPixels();
    void commitTonalEdit(const QString& name, const QString& iconKey);
    void cancelTonalEdit();
    // One-shot analogue: apply `edit` to the active pixel layer as a single
    // undo step (Sharpen / Sharpen More / one-shot filters).
    bool applyLayerEditOneShot(const std::function<void(pittore::Image&)>& edit,
                               const QString& name, const QString& iconKey);
    // Filter-dialog apply path: runs `id` with `params` on the active pixel
    // layer, using device kernels when the active backend is a GPU and the
    // filter is in the GPU-routed set (exact-algorithm ports of the CPU
    // reference), falling back to the CPU engine otherwise and on any
    // device error. Returns false when there is no editable layer.
    bool applyFilterToActiveLayer(const std::string& id,
                                  const std::vector<double>& params);
    // Live (Smart) filters (defined in ui/live_filter.cpp): a recipe over
    // the native pixels, rendered on demand. Convert keeps pixels and sets
    // defaults (one undo step); param edits are live without undo steps
    // (slider contract); enable/disable/remove are undoable steps. Removing
    // reveals the never-mutated natives.
    bool convertToLiveFilter(const QString& filterId);
    bool convertToLiveFilter(const QString& filterId,
                             const std::vector<double>& params);
    bool setLiveFilterParam(int index, double value);
    bool setLiveFilterEnabled(bool enabled);
    bool removeLiveFilter();
    // Image Rotation / Flip Canvas (R22): rotate or flip every layer that has
    // pixels about the document centre, keeping each layer's centre fixed.
    // 90° rotations keep exact pixel grids (no resampling). One undo step.
    // `op` is one of "cw90", "ccw90", "180", "flipH", "flipV".
    bool transformDocumentImage(const QString& op);
    // Image > Mode: rewrite appearance pixels (and colour-bearing items)
    // into the target space, retag `colorMode`, keep the CMYK separation
    // bytes. RGB / Grayscale / CMYK only — the other Mode entries stay
    // disabled (they need native side data we don't store). One undo step.
    // Implementation in ui/color_mode.cpp.
    enum class ModeTarget { Rgb, Grayscale, Cmyk };
    bool convertDocumentMode(ModeTarget target);

    // R80: bin the active document's flattened composite into a 256-bin
    // histogram (per-channel + luminance). Sampling is capped so a large
    // document does not stall the UI thread. Returns false with no pixels.
    bool activeHistogram(pittore::Histogram256& out) const;

    // --- cursor readout -----------------------------------------------------
    // Last pointer position + colour, pushed by the window so the Info panel
    // can show a live readout without owning the canvas.
    QPointF cursorDocumentPos() const { return cursorPos_; }
    QColor cursorColor() const { return cursorColor_; }
    void setCursorInfo(const QPointF& pos, const QColor& color);

    // Announce an annotation edit (sample pin, note, count marker or ruler) so
    // the canvas and panels refresh. The caller has already mutated the active
    // document's annotation vectors (and owns the undo step).
    void markAnnotationsChanged();

    // Type tool: push one options-bar option through to the active live text
    // layer (family/style/size/align/colour) and re-render it. Returns true when
    // a live text layer changed. No-op when the active layer is not a live text
    // layer or the option is not typographic.
    bool applyTextOption(const QString& id, const QVariant& value);

    // Character panel: apply one typographic option to the active live text
    // layer as a single undo step, or (with no live text layer active) to the
    // Type tool's insertion attributes. Returns true when something changed.
    bool applyCharacterOption(const QString& id, const QVariant& value);
    // The Character panel's current values: the active live text layer's spec,
    // or the Type tool's insertion attributes when no text layer is active.
    TextItem activeTextSpec() const;

    // --- Liquify (displacement warp) ------------------------------------------
    // Begin a warp stroke on the active pixel layer. Takes one history snapshot
    // (BEFORE the layer is rebound), freezes the pre-stroke pixels as the warp
    // source, gives the live layer a private copy to receive the resamples, and
    // uploads the frozen source once into a device-resident staging buffer the
    // whole stroke fetches from. Returns false when there is no warppable active
    // layer (the snapshot is discarded in that case).
    bool beginLiquifyStroke();

    // Resample the [x0,x1) × [y0,y1) region of the active layer, in LAYER
    // pixels, from the frozen snapshot through `mesh`, on the live compute
    // backend (device kernel on a GPU build; the CPU reference otherwise). The
    // rebuilt region is copied back into the layer's host pixels and the
    // document-space dirty rect accumulated; callers call flushPaint() once per
    // input event. Returns false when the region cannot be warped.
    bool liquifyDab(const pittore::compute::WarpMesh& mesh, int x0, int y0,
                    int x1, int y1);

    // Drop the frozen snapshot. The staging buffers stay allocated (grown per
    // stroke) for reuse. The history step is owned by the caller
    // (beginUndoStep/commitUndoStep/discardUndoStep).
    void endLiquifyStroke();

    // Escape / tool-switch cancel: restore the pre-stroke pixels, drop the
    // snapshot and the pending history step, and re-composite the document.
    void cancelLiquifyStroke();

    bool beginLiquifySession();
    bool liquifySessionActive() const { return liquifySession_; }
    pittore::compute::WarpMesh& liquifySessionMesh() { return liquifyMesh_; }
    const pittore::compute::WarpMesh& liquifySessionMesh() const { return liquifyMesh_; }
    bool liquifySessionMoved() const { return liquifySessionMoved_; }
    void markLiquifySessionMoved() { liquifySessionMoved_ = true; }
    bool liquifyMirrorDab(float cx, float cy, float radius, float angle,
                          bool invert, float strength, float falloffExp);
    bool liquifyCloneSource(float x, float y);
    bool liquifyHasCloneSource() const { return liquifyHasClone_; }
    QPointF liquifyCloneSourcePoint() const { return liquifyClone_; }
    void paintLiquifyMask(float cx, float cy, float radius, float hardness,
                          bool freeze, float opacity);
    float liquifyMaskAt(float x, float y) const;
    bool liquifyMaskPresent() const;
    std::vector<float> liquifyMaskCopy() const { return liquifyMask_; }
    void restoreLiquifyMask(std::vector<float> m);
    bool liquifyMaskCombine(const std::vector<float>& src, int op);
    std::shared_ptr<pittore::Image> liquifySource() const { return liquifySrc_; }
    void clearLiquifyMask();
    void fillLiquifyMask(float v);
    void invertLiquifyMask();
    bool liquifyMaskFromSelection();
    bool renderLiquifyFull();
    void scaleLiquifyMesh(float f);
    void resetLiquifyMesh();
    void setLiquifyMesh(pittore::compute::WarpMesh mesh);
    bool saveLiquifyMesh(const QString& path) const;
    bool loadLiquifyMesh(const QString& path);
    void requestLiquifySaveMesh();
    void requestLiquifyLoadMesh();
    void endLiquifySession();
    void cancelLiquifySession();

    // Last headless-capable AI command report (Auto Refine today), readable
    // over the MCP bridge without touching log files — the production
    // equivalent of the refine lab's get_state (command, outcome, model,
    // timing, matte coverage, stages that ran).
    struct AiRunReport {
        QString command;
        bool ok = false;
        QString message = QStringLiteral("did not complete");
        QString model;
        double milliseconds = 0.0;
        double coverage = 0.0;  // fraction of layer pixels with alpha > 127
        QStringList stages;
    };
    const AiRunReport& lastAiRun() const { return lastAiRun_; }
    void setLastAiRun(AiRunReport report) { lastAiRun_ = std::move(report); }

    // Multiply the active pixel layer's alpha by an external soft mask laid over
    // the layer's NATIVE pixels (AI background removal). `mask` is width×height
    // floats in [0,1], row-major, in the layer's pixel space. Commits the edit
    // (sourceStamp++, thumbnail + composite invalidation, history entry) so
    // callers can run inference off-thread and apply here. Returns false when
    // there is no paint-capable active layer or the mask size mismatches.
    bool applyActiveLayerAlpha(const float* mask, int width, int height,
                               const QString& historyName, const QString& iconKey);

    // Replace the active pixel layer's full RGBA content from a straight-alpha
    // RGBA8888 image (width×height must match the layer's native pixels).
    // Commits the edit (sourceStamp++, thumbnail + composite invalidation,
    // history entry). Returns false when there is no paint-capable active
    // layer or the size mismatches.
    bool replaceActiveLayerRgba(const QImage& rgba,
                                const QString& historyName,
                                const QString& iconKey);

    // Delete: when the document holds an arbitrary-shape (mask) selection,
    // erase just the selected pixels from the active pixel layer (soft edges
    // follow the mask's coverage, copy-on-write, one undo step). Returns false
    // when there is no mask selection or no paint-capable active layer, so
    // callers fall back to removing the selected layer(s).
    bool eraseSelectionFromActiveLayer();
    // Fill (Alt+Backspace foreground / Ctrl+Backspace background): paint the
    // live selection with `color` on the active pixel layer — coverage from
    // the selection mask (feathered edges blend), whole layer when nothing is
    // selected. Copy-on-write, one undo step. False when no paint-capable
    // active layer.
    bool fillActiveSelectionWithColor(const QColor& color,
                                      const QString& undoName);
    // Invert image colors (Ctrl+I): complement the active pixel layer's RGB
    // in place (alpha untouched), clipped to the live selection when one
    // exists. Copy-on-write, one undo step. False when no paint-capable
    // active layer.
    bool invertActiveLayerPixels();
    // Number-key layer opacity (1..0): set the active layer's opacity 10..100%
    // as one undo step (the Layers-panel spinbox path is direct). False when
    // there is no active layer.
    bool setActiveLayerOpacity(int percent);
    // Blend-mode letter shortcuts (Alt+Shift+M/S) with the Move tool: set the
    // active layer's blend mode by display name as one undo step (mirrors the
    // Layers-panel combo; "Add" is accepted as an alias for Linear Dodge).
    // False when there is no active layer or the name is unknown.
    bool setActiveLayerBlendMode(const QString& name);

    QString statusHint() const { return statusHint_; }
    void setStatusHint(const QString& hint);

    // Region-edit latency instrumentation: stamp a cheap region edit so the
    // canvas paint that renders it first can log click→paint delay.
    void noteRegionEdit(const QRectF& docRect);
    QRectF lastRegionEditRect() const { return regionEditRect_; }
    // Age (µs) of the pending region-edit stamp, or -1 when none/expired.
    // Consumes the stamp so only the first paint that covers the region reports.
    qint64 takeRegionEditAgeUs();

  signals:
    void toolChanged(ToolId id);
    void optionChanged(ToolId tool, const QString& id, const QVariant& value);
    void colorsChanged();
    void swatchesChanged();
    void quickMaskChanged(bool on);
    void screenModeChanged(ScreenMode m);
    void chromeVisibilityChanged(ChromeVisibility v);
    void surroundChanged(int index);
    void themeChanged(UiTheme t);
    void settingsChanged();
    void documentsChanged();
    void activeDocumentChanged(DocumentItem* doc);
    void documentModified(DocumentItem* doc);
    // Region-scoped edit notice: only `docRect` (document coordinates) needs a
    // visible refresh. Emitted by cheap edits like a layer visibility toggle so
    // the canvas can repaint just that window instead of a full-viewport
    // update + repaint (which is the perceived "lag" on every eye click).
    void regionModified(DocumentItem* doc, const QRectF& docRect);
    void layersChanged();
    // Cheap selection-only broadcast: the active LayerItem changed but the
    // layer list itself is untouched. Consumers repaint (rows, canvas gizmo)
    // instead of rebuilding the whole panel — thousands of small SVG rows make
    // a full layersChanged rebuild far too slow for a plain click.
    void activeLayerChanged();
    // A group header was created by grouping selected layers; the index is the
    // new header's position. The Layers panel uses it to start an inline rename
    // right after creation (emitted after layersChanged so rows are current).
    void groupCreated(int index);
    // The board list, order or a board's properties changed (create, delete,
    // reorder, rename, fill, opacity, visibility). Carries the document,
    // because a restore can change the whole list. The Layers panel does not
    // need it (boards are rows); the Artboards panel (§10.2) does.
    void boardsChanged(DocumentItem* doc);
    // A Live Tone Blend Group asks for its settings dialog (Layers panel
    // icon click, or right after creation). Carries the header index.
    void toneBlendRequested(int groupIndex);
    void historyChanged();
    void selectionChanged();
    void taskContextChanged(TaskContext c);
    void statusHintChanged(const QString& hint);
    void snapChanged();
    // Soft-proof toggles or the persisted proof setup changed: the canvas
    // repaints (reconfiguring its proof transform when the setup moved).
    void proofChanged();
    // Live pointer readout for the Info panel (document position + sampled
    // colour). Emitted on every pointer move over the canvas.
    void cursorInfoChanged(const QPointF& pos, const QColor& color);
    // An annotation (sample pin, note, count marker, ruler) changed.
    void annotationsChanged();
    // The Layers panel's fx button (and the Layer Style menu) asks for the
    // non-destructive Layer Style dialog on the active layer; the window owns
    // the dialog, panels only request it so they stay decoupled from it.
    void layerStyleRequested(int effectIndex);
    void liquifySaveMeshRequested();
    void liquifyLoadMeshRequested();

  private:
    // Ensure a layer's native CMYK samples match its current RGB appearance
    // and profile, then resolve the foreground into this document's inks.
    bool prepareNativeCmykPaint(DocumentItem& doc, LayerItem& layer,
                                const QColor& color,
                                pittore::compute::NativeCmykPaint& paint,
                                bool trackStroke = true);
    // Shared Paint Bucket / Magic Eraser flood. `erase` picks the operation
    // (paint the foreground vs multiply alpha down).
    bool floodEdit(const QPointF& docPos, bool erase);
    // Spot-heal grid fill over a bbox, shared by Remove (marked area) and
    // ContentAwareMove (vacated hole). `cover` gates dabs (null = whole
    // bbox). Reports per-dab regions for the incremental composite.
    static bool healFillGrid(DocumentItem& d, LayerItem& layer,
                             const std::vector<pittore::RGBAf>& donor,
                             int bx0, int by0, int bx1, int by1,
                             const float* cover, int diff);
    // History source lookup shared by the History and Art History Brushes:
    // the oldest undo snapshot's matching layer (document-open state).
    // Null when no usable history state exists yet.
    static const LayerItem* historySourceFor(DocumentItem* d, int activeLayer,
                                             std::uint32_t lw,
                                             std::uint32_t lh);
    // Paper grain for a dab's target space (invalid/disabled when none).
    // pressure01 scales strength when the texture-pressure toggle is on.
    pittore::compute::PatternTex resolveTexture(
        ToolId tool, double lsx, double lsy, const QPointF& targetOffset,
        double pressure01 = 1.0) const;
    // Paper-grain evaluator for imported-bundle textures (invalid/disabled
    // when the preset keeps the legacy combine or no pattern applies).
    pittore::compute::papergrain::PaperGrain resolvePaperGrain(
        ToolId tool, double lsx, double lsy, const QPointF& targetOffset,
        double pressure01 = 1.0) const;
    // Masked second tip for a tool (invalid/disabled when none set).
    // pressure01 shrinks the mask when the mask-pressure toggle is on.
    pittore::compute::MaskTip resolveMaskTip(ToolId tool,
                                              double pressure01 = 1.0) const;
    // Per-pixel density for a dab (1.0 = solid). Advances the stroke's dab
    // counter, so reseeded strokes replay bit-exactly.
    pittore::compute::DabDensity resolveDensity(ToolId tool);
    // Live dab pressure for the resolvers above (paintDabAt sets it per
    // dab; defaults to 1.0 for direct kernel/test callers).

    ToolId activeTool_ = ToolId::Move;
    QVector<ToolId> toolStack_;
    int temporaryDepth_ = 0;
    QHash<QString, QVariant> options_;
    // Display name of the last applied brush preset (see setter above).
    QString brushPresetName_;
    // User-imported stamp tips, keyed by stamp id (Brushes panel owns disk).
    QHash<QString, pittore::compute::StampTip> stamps_;
    // Paper-grain patterns + multi-cell hoses (same ownership).
    QHash<QString, PatternGray> patterns_;
    QHash<QString, pittore::compute::brushload::LoadedHose> hoses_;
    // Per-stroke brush state (see beginStrokeState).
    std::mt19937 strokeRng_;
    std::uint64_t strokeSeed_ = 0;
    std::uint64_t pinnedSeed_ = 0;  // test hook: fixed seed instead of random
    std::uint32_t dabCount_ = 0;  // dabs since stroke start (density seed)
    bool strokeStateLive_ = false;
    // Wash painting mode: dabs accumulate in a target-space scratch buffer
    // and composite over the canvas at the stroke opacity; the bake folds
    // them into the layer on release. Armed per stroke for paint tools.
    bool washArmed_ = false;
    pittore::Image washScratch_{1, 1};
    std::vector<std::array<std::uint16_t, 4>> washScratchCmyk_;
    QRect washDirty_;
    pittore::ui::brushcurve::Curve strokeSizeCurve_;
    pittore::ui::brushcurve::Curve strokeOpacityCurve_;
    pittore::ui::brushcurve::Curve strokeFlowCurve_;
    pittore::ui::brushcurve::Curve strokeRotationCurve_;
    pittore::compute::SmudgeCarry smudgeCarry_;
    // Finger-paint priming: with finger off, the first dab of the stroke
    // loads the travelling patch from the canvas under the dab (pure smear);
    // with finger on, the stroke starts from the foreground (wet brush).
    // Empty carry = unprimed; re-arms per stroke.
    // Art History dab index: style curl offsets advance per dab;
    // re-arms per stroke (deterministic reruns).
    int arthDabIndex_ = 0;
    // Background Eraser Once sampling: the first dab of the stroke loads
    // the sampled colour; later dabs reuse it. Re-arms per stroke.
    bool bgEraseSampled_ = false;
    pittore::RGBAf bgEraseSample_{0, 0, 0, 1};
    // Mixer Brush load: the brush-held paint, relaxed toward the canvas
    // by every dab (serial feedback, hence CPU-only). Loaded from the
    // foreground at stroke start unless load_after is off and a load
    // survives; cleaned after the stroke when clean_after is on.
    pittore::RGBAf mixerLoaded_{0, 0, 0, 0};
    bool mixerLoadedValid_ = false;
    // Remove-tool marks: per-texel coverage painted by the stroke,
    // healed on release (checked strokes). Survives across strokes while
    // remove_after_stroke is off; consumed by the healing fill.
    std::vector<float> removeCoverage_;
    std::uint32_t removeW_ = 0;
    std::uint32_t removeH_ = 0;
    // PatternStamp tile cache (procedural 64x64, rebuilt on pattern change)
    // + stroke origin for non-aligned mode (first dab latches it).
    std::vector<pittore::RGBAf> patternTilePx_;
    int patternTileId_ = -1;
    bool patternOriginArmed_ = true;
    QPointF patternStrokeStart_{0, 0};
    QPointF strokePatternOrigin_{0, 0};
    // Per-stroke selection mask: layerSelectionMask() depends only on the
    // selection + target geometry (never the dab position), so one build
    // serves the whole stroke instead of one resample per dab. Keyed on
    // layer identity + selection stamp + geometry; armed by
    // beginStrokeState, cleared by endStrokeState.
    pittore::compute::SelectionMask strokeSelCache_;
    const LayerItem* strokeSelLayer_ = nullptr;
    quint64 strokeSelStamp_ = 0;
    QPointF strokeSelOffset_;
    double strokeSelSx_ = 0.0, strokeSelSy_ = 0.0;
    const pittore::compute::SelectionMask* strokeSelectionMask(
        DocumentItem& d, LayerItem& layer,
        const pittore::Image* targetImage, const QPointF& targetOffset,
        double sx, double sy);
    // Live dab pressure for the texture/mask resolvers above.
    double dabPressure01_ = 1.0;
    // Native CMYK dabs keep the ink sidecar authoritative through a stroke;
    // the RGB appearance fingerprint is refreshed once on pointer-up.
    DocumentItem* nativeCmykStrokeDoc_ = nullptr;
    LayerItem* nativeCmykStrokeLayer_ = nullptr;

    // Depth of nested begin/commit undo gestures. A Character-panel edit inside
    // a live Type session joins the session's step instead of starting its own.
    int undoGestureDepth_ = 0;

    // Tonal-stroke accumulation (Dodge/Burn/Sponge): the pre-stroke image and
    // the per-pixel max coverage over the stroke. A dab raises coverage and
    // re-renders from the pre-stroke pixels, so overlapping dabs do not
    // compound. Empty when no tonal stroke is active.
    bool toneStrokeActive_ = false;
    std::vector<pittore::RGBAf> toneStrokePre_;
    std::vector<float> toneStrokeCoverage_;
    std::uint32_t toneStrokeW_ = 0;
    std::uint32_t toneStrokeH_ = 0;

    // Clone-stroke accumulation (Clone Stamp): the pre-stroke image, the
    // per-pixel max coverage over the stroke, and the source snapshot the
    // stamp reads (the layer's own pre-stroke pixels, or the composite
    // caught at stroke start for the composite sample modes — sampling live
    // pixels would feed the stroke's own output back into itself). Empty
    // when no clone stroke is active.
    bool cloneStrokeActive_ = false;
    std::vector<pittore::RGBAf> cloneStrokePre_;
    std::vector<pittore::RGBAf> cloneStrokeSrc_;
    std::vector<float> cloneStrokeCoverage_;
    std::uint32_t cloneStrokeW_ = 0;
    std::uint32_t cloneStrokeH_ = 0;

    // Healing Brush donor session: the translated donor (sample offset
    // applied to the stroke-frozen source, or the tiled pattern) rebuilt
    // when the offset/sample/pattern key changes. Keyed on quantized
    // offset + sample mode + pattern id + layer size; empty when idle.
    // Freed with the stroke (see endHealStroke, called from the same
    // release path as endCloneStroke).
    std::vector<pittore::RGBAf> healDonor_;
    std::uint32_t healDonorW_ = 0;
    std::uint32_t healDonorH_ = 0;
    float healDonorOx_ = 0.0f;
    float healDonorOy_ = 0.0f;
    int healDonorSample_ = -1;
    int healDonorPattern_ = -1;
    std::uint64_t healDonorSeed_ = 0;
    // One-shot fill scratch (Remove release, ContentAwareMove drop): full-
    // layer donor staging, grow-only across calls so steady state pays the
    // copy but never the allocation + first-touch faults.
    std::vector<pittore::RGBAf> healFillScratch_;

    // Tonal/filter edit session state (beginTonalEdit..commit/cancel). The
    // pre-edit clone restores the pixels on cancel; the undo snapshot opened
    // by beginTonalEdit covers the accepted edit.
    bool tonalEditActive_ = false;
    std::shared_ptr<pittore::Image> tonalEditPre_;
    std::shared_ptr<pittore::color::CmykImage> tonalEditCmykPre_;

    // Momentary adjustment preview (begin/endAdjustmentPreview): stashed row
    // index + its prior visibility while hidden. -1 when idle.
    int previewAdjustmentIndex_ = -1;
    bool previewWasVisible_ = true;

    // Insertion attributes the Character panel edits when no live text layer is
    // active; the next text layer starts from these.
    TextItem characterDefaults_;

    QColor foreground_ = Qt::black;
    QColor background_ = Qt::white;
    QVector<QColor> swatches_;

    bool quickMask_ = false;
    bool snapEnabled_ = true;
    int snapTargets_ = SnapTargetsAll;
    bool proofEnabled_ = false;
    bool proofGamut_ = false;
    ScreenMode screenMode_ = ScreenMode::Standard;
    ChromeVisibility chrome_ = ChromeVisibility::All;
    int surround_ = 0;
    UiTheme theme_ = UiTheme::DarkGray;
    AppSettings settings_;
    // Import profile-mismatch resolver (MainWindow shows the dialog; empty
    // means headless/tests: Ask falls back to Convert).
    std::function<ImportProfileChoice(const QString&, const QString&)>
        mismatchResolver_;
    // Deferred mismatch dialog (see setDeferMismatchDialogs). Documents live
    // on the heap, so the pointer stays valid across UI sync calls.
    bool deferMismatch_ = false;
    struct PendingMismatch {
        DocumentItem* doc = nullptr;
        QString embedded;
    };
    std::optional<PendingMismatch> pendingMismatch_;
    std::unique_ptr<pittore::compute::ComputeBackend> backend_;

    QVector<DocumentItem*> documents_;
    int activeDocument_ = -1;

    // Liquify stroke state (see beginLiquifyStroke). `liquifySrc_` is the frozen
    // pre-stroke image the warp samples from; the buffers are w×h RGBAf staging
    // pairs reused across strokes and grown when the layer size changes.
    std::shared_ptr<pittore::Image> liquifySrc_;
    std::shared_ptr<pittore::color::CmykImage> liquifyCmykSrc_;
    std::unique_ptr<pittore::compute::Buffer> liquifySrcBuf_;
    std::unique_ptr<pittore::compute::Buffer> liquifyDstBuf_;
    std::uint32_t liquifyBufW_ = 0;
    std::uint32_t liquifyBufH_ = 0;
    bool liquifySession_ = false;
    bool liquifySessionMoved_ = false;
    pittore::compute::WarpMesh liquifyMesh_;
    std::vector<float> liquifyMask_;
    std::uint32_t liquifyMaskW_ = 0;
    std::uint32_t liquifyMaskH_ = 0;
    float liquifyMeshStrength_ = 100.0f;
    bool liquifyHasClone_ = false;
    QPointF liquifyClone_;

    TaskContext context_ = TaskContext::None;
    QString statusHint_;
    AiRunReport lastAiRun_;

    // Last pointer position + sampled colour (see setCursorInfo).
    QPointF cursorPos_{0, 0};
    QColor cursorColor_;

    // Region-edit latency instrumentation: the last cheap region edit
    // (visibility toggle, move/scale frame, stroke flush) stamps its doc rect
    // + monotonic time so the canvas paint that renders it first can log the
    // true click→paint latency.
    QRectF regionEditRect_;
    qint64 regionEditTimeUs_ = -1;
};

}  // namespace pittore::ui
