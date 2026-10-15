#pragma once
#include <QAbstractScrollArea>
#include <QCursor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QTimer>
#include <QTransform>
#include <QPainter>

#include <cstddef>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>

#include "ui/app_state.h"
#include "ui/brushes/sensor_drives.h"
#include "ui/persona/vector_pen.h"

#include "engine/color/proof.h"
#include "engine/vector/path.h"

#include "engine/compute/warp.h"
#include "engine/text/text_engine.h"

class QKeyEvent;
class QTabletEvent;
class QTimer;

namespace pittore::ai {
struct SamEncodings;
}

namespace pittore::ui {

class ContextualTaskBar;
class CanvasRuler;

// The document view. Everything drawn here that is not document pixels lives in
// the overlay layer (R20): marching ants, crop rectangle, transform handles,
// guides, grid, gradient stops. The overlay is view-transformed with the
// document but never composited into it.
//
// The composite currently arrives as a QImage from DocumentItem. The Vulkan
// presentation path (R56/R91) replaces exactly one function — paintDocument —
// with a swapchain blit; the rest of this class is view maths and input.
class CanvasView final : public QAbstractScrollArea {
    Q_OBJECT

  public:
    explicit CanvasView(AppState* state, QWidget* parent = nullptr);

    double zoom() const;
    void setZoom(double zoom, QPointF viewportAnchor = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToFill();
    void zoomActualPixels();
    // Fit the document's width into the viewport (R14 "Fit to Width").
    void zoomToWidth();
    // Fit the active layer's document-space bounds on screen (R14 "Fit Layer
    // on Screen"); falls back to the document fit when nothing is selected.
    void zoomToActiveLayer();
    // 1 image pixel == 1 physical print pixel at the document's DPI (R14
    // "Print Size"): the zoom that maps image pixels to the screen's DPI.
    void zoomPrintSize();
    void setRotation(double degrees);
    void resetRotation();

    bool rulersVisible() const { return rulers_; }
    void setRulersVisible(bool on);
    // Ruler-drag guide creation (driven by the ruler strips): press begins
    // the pending guide, moves track it, release commits it to the document
    // (Escape abandons). The overlay draws the pending line dashed.
    void beginRulerGuide(Qt::Orientation orientation, const QPointF& rulerPos);
    void updateRulerGuide(Qt::Orientation orientation, const QPointF& rulerPos);
    void finishRulerGuide(bool commit);
    void cancelRulerGuide();
    // Crop tool: the live frame is view state until it is committed. Apply
    // crops the document in one undo step (canvas resizes, layer content
    // stays put); Cancel resets the frame to the whole document. Routed
    // from the contextual task bar via the window's command dispatcher.
    void commitCrop();
    void cancelCrop();
    // Node tool click-selection (convert/close/split/join/reverse target):
    // selected art layer and anchor segment, or -1 when nothing is selected.
    int selectedNodeLayer() const { return nodeSelLayer_; }
    int selectedNodeSeg() const { return nodeSelSeg_; }
    bool guidesVisible() const { return guides_; }
    void setGuidesVisible(bool on);
    bool gridVisible() const { return grid_; }
    void setGridVisible(bool on);
    // Stroke symmetry across the canvas-center axes (painting aid; mirrored
    // dabs land in the same undo step as the primary stroke).
    bool symmetryX() const { return symmetryX_; }
    bool symmetryY() const { return symmetryY_; }
    void setSymmetryX(bool on);
    void setSymmetryY(bool on);
    bool selectionEdgesVisible() const { return selectionEdges_; }
    void setSelectionEdgesVisible(bool on);
    bool smartGuidesVisible() const { return smartGuides_; }
    void setSmartGuidesVisible(bool on);
    bool pixelGridVisible() const { return pixelGrid_; }
    void setPixelGridVisible(bool on);
    // Document-space grid spacing both the overlay and move-snapping share.
    double gridStep() const;
    bool extrasVisible() const { return extras_; }
    void setExtrasVisible(bool on);

    ContextualTaskBar* taskBar() const { return taskBar_; }

    // True while a Type-tool editing session is live: the window's single-key
    // tool shortcuts must stand down so letters reach the canvas as text.
    bool textEditing() const { return textEditing_; }

    QPointF viewToDocument(QPointF viewPoint) const;
    QPointF documentToView(QPointF documentPoint) const;

    // Painted by the ruler strips that sit in the scroll area's margins.
    void paintRuler(QPainter& painter, Qt::Orientation orientation, const QRect& area) const;
    QPointF cursorDocumentPosition() const { return cursorDoc_; }
    // Brush cursor queries (public so tests can pin the paint ⊆ dirty-rect
    // invariant): cursorToolActive is the tool gate alone, brushCursorVisible
    // adds the outline preferences, cursorRingGeometry is the single geometry
    // source both the paint and the dirty rect share, brushCursorViewRect is
    // the invalidated box, canvasCursor the configured viewport cursor.
    bool cursorToolActive() const;
    bool brushCursorVisible() const;
    void cursorRingGeometry(double& rDoc, double& ratio, double& angleDeg,
                            bool& square) const;
    QRectF brushCursorViewRect() const;
    QCursor canvasCursor() const;
    // Move-drag smart guides. While a move drag is live a faint document-centre
    // crosshair is shown (green vertical, red horizontal) so the centre is a
    // live readout in both directions; the axis brightens when the drag snaps
    // to the centre, and any other snapped target gets its own line. Public
    // so tests can pin the target list (grid spacing included).
    struct MoveSnap {
        QPointF offset;
        bool x = false;
        double xPos = 0.0;
        bool xCenter = false;
        bool y = false;
        double yPos = 0.0;
        bool yCenter = false;
        // Which of the layer's three features won the snap on each axis:
        // 0 = left/top, 1 = centre, 2 = right/bottom, -1 = no snap. Without it
        // a trace shows the winning target but not which edge it pulled there,
        // so an off-canvas edge cannot be attributed to the snap or the cursor.
        int xFeat = -1;
        int yFeat = -1;
    };
    // Move-drag snapping (public so tests can pin the target list, grid
    // spacing included): snappedMoveOffset snaps a layer by its pixel
    // footprint, snappedMoveSize by an explicit doc-space footprint. Both
    // return the raw offset unchanged when nothing snaps, plus the guide
    // line(s) to draw (see implementation for the tolerance rules). `moving`
    // lists the layers the gesture itself drags: their own edges are never
    // snap targets, or every small per-event step would snap back onto where
    // the layer just was and the drag would crawl in lurches (and, once a
    // feature drifted within tolerance of another of its own edges, teleport
    // by half the layer's width).
    MoveSnap snappedMoveOffset(const QPointF& raw, const LayerItem& layer,
                               const QVector<int>& moving = {}) const;
    // Same snapping for a doc-space footprint of given size (used when the
    // moved unit is a group and has no single pixel footprint).
    MoveSnap snappedMoveSize(const QPointF& raw, const QSizeF& size,
                             const QVector<int>& moving = {}) const;

    // Liquify: an in-progress warp stroke. The main window calls
    // cancelLiquify() on Escape and commitLiquifyStrokeIfActive() when the tool
    // is left mid-stroke; both are no-ops when no warp stroke is live.
    bool liquifyStrokeActive() const { return liquifyActive_; }
    void cancelLiquify();
    void commitLiquifyStrokeIfActive();

    // Color Replacement: lock the colour palette under the brush cursor as
    // the match set (the Alt+click gesture calls this at the click point;
    // the options-bar Lock Area button calls it at the hover point, for
    // desktops whose window manager swallows Alt+click). No-op unless the
    // Replace brush is live and the pointer is over the canvas.
    void lockReplacePalette();
    // Airbrush tick: one rate dab at the cursor while a stroke is live.
    // Public so tests can drive it deterministically (the QTimer is the
    // normal caller).
    void airbrushTick();
    // Stylus response math: pure functions of the latched readings, public
    // so tests can pin them without a device. Lean is 0 upright, 1 at
    // full 60° tilt; the size factor grows with lean (>= 1), the opacity
    // factor shrinks (<= 1), and the wheel factor maps tangential
    // pressure (0.15..1). Amounts are 0..100; 0 is exactly 1.0.
    static double tiltLean(double tiltX, double tiltY);
    static double tiltSizeFactor(double lean01, double amtPct);
    static double tiltOpacityFactor(double lean01, double amtPct);
    static double tangentialFlowFactor(double tangential01);
    static double speedSizeFactor(double speed01, double amtPct);
    static double timeFadeFactorMs(quint64 elapsedMs, double lenSec);
    static double fuzzyFactor(double h01, double amtPct);
    static double hash01(std::uint64_t z);

  signals:
    void zoomChanged(double zoom);
    void cursorMoved(QPointF documentPosition);
    void colorSampled(const QColor& color, bool toBackground);
    void copyRequested();
    void pasteRequested();
    void pasteInPlaceRequested();
    void clearRequested();
    // Place tool click: the window opens the file dialog and places the image
    // at this document point (canvas never touches dialogs itself, except the
    // small QInputDialog prompts like the Note tool's).
    void placeRequested(QPointF docPos);
    // View rotation changed by the Rotate View tool (or the options bar); the
    // window mirrors it into the tool's angle field.
    void rotationChanged(double degrees);
    // Click-an-image-to-find-its-layer: emitted after a canvas pick selects a
    // layer (Move click with Auto-Select, Ctrl+click from any tool). The
    // window surfaces the Layers dock so the picked row is visible; the
    // Layers panel itself already highlights and scrolls to the row.
    void layerPickedFromCanvas();

  public slots:
    void refresh();

  public:
    // Installed on the viewport in the constructor; public so synthetic
    // drops/keys can drive it headless in tests.
    bool eventFilter(QObject* watched, QEvent* event) override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

  protected:

  private:
    void updateScrollRange();
    void applyToolCursor();
    void layoutRulers();
    void applyPendingFit();
    void paintDocument(QPainter& painter);
    // Region-bounded variant: only the damaged viewport rect is repainted
    // (checkerboard + composite source-rect), so a layer-eye toggle paints
    // the 60×60 window instead of the whole document.
    void paintDocument(QPainter& painter, const QRect& viewDirty);
    // Segmented paint: one edit-time composite blit plus crisp live draws
    // for simple vector layers with nothing stacked above them (fully
    // opaque, no overlapping pixel content above). Everything else keeps
    // its correctly blended composite raster. True when it painted; false
    // when no layer qualifies and the caller keeps the single flattened
    // blit. baseDrawn: the composite base is already on screen (the tiled
    // path put it there with its ready tiles), so re-blitting it here would
    // bury those tiles and undo the sharpening frame.
    bool paintSegmented(QPainter& painter, const QTransform& docToView,
                        const QRectF& docRect, const QRect& viewDirty,
                        bool baseDrawn = false);
    void paintOverlay(QPainter& painter);
    // Brush cursor + resize HUD: the tool's own pointer UI, painted on
    // every frame (hiding the cursor would blind the brush) while the
    // rest of the overlay follows the Extras toggle.
    void paintBrushCursor(QPainter& painter);
    // Annotation-tool overlay: Color Sampler pins, Note markers, Count markers
    // and the Ruler measurement. Drawn in the overlay layer so a note/count
    // never becomes document pixels.
    void paintAnnotationOverlay(QPainter& painter, const QTransform& t);
    // View-space hit tests for the annotation markers (7 px tolerance). Return
    // an index, or -1.
    int samplePinAtView(const QPointF& viewPos) const;
    int noteAtView(const QPointF& viewPos) const;
    int countMarkerAtView(const QPointF& viewPos) const;
    // Slice tool (R19): the slice under a document point (7 px tolerance in
    // view space; multiple intersections pick the smallest — topmost).
    int sliceAtView(const QPointF& viewPos) const;
    // Double-click-to-edit core shared by the manual second-press detector in
    // mousePressEvent and by mouseDoubleClickEvent: drops the caret into the
    // live text layer under docPos (word-select when the session is already
    // live there). A hit with the Move tool also promotes it to the Type
    // tool; returns true when a text session started or continued.
    bool handleTextDoubleClick(const QPointF& docPos);
    // Marquee rubber band with conventional modifiers: Alt draws from the
    // center, Shift constrains to a square (circle for the ellipse tool).
    // marqueeMods_ tracks the modifiers from the press/move/release event
    // stream (not the live keyboard state) so synthetic events, mid-drag key
    // changes, and the overlay all agree.
    QRectF marqueeLiveRect() const;
    QTransform documentTransform() const;
    QSizeF scaledDocumentSize() const;
    DocumentItem* doc() const { return state_->activeDocument(); }

    // Crop tool (own TU: canvas/tools/canvas_crop.cpp). The frame is a
    // document-space rect held in the view — never the document selection:
    // a crop rect is a pending canvas edit, not a pixel selection. Moving the
    // pointer over the crop tool arms a frame over the whole document; a press
    // picks the gesture from the hit test (a handle, inside the frame, or
    // outside it to rubber-band a new one), moves track it, and release keeps
    // the frame live for fine tuning until commitCrop().
    static constexpr int kCropMove = 8;   // drag inside repositions the frame
    static constexpr int kCropDraw = 9;   // drag outside rubber-bands a new frame
    QRectF cropOverlayRect() const;       // live frame, clamped to the document
    double cropRatio() const;             // active ratio constraint (0 = free)
    // Handle position for a frame rect (move-tool convention 0..7: TL, top,
    // TR, right, BR, bottom, BL, left — the opposite handle is (h + 4) % 8).
    static QPointF cropHandlePos(int handle, const QRectF& rect);
    int cropHandleAtView(const QPointF& viewPos) const;
    void cropArmForDocument();
    void cropPress(const QPointF& docPoint, const QPointF& viewPos);
    void cropDragTo(const QPointF& docPoint);
    void cropHoverUpdate(const QPointF& viewPos);
    QRectF cropRect_;                  // the live frame, document space
    int cropDragMode_ = -1;            // -1 idle, 0..7 handle, kCropMove, kCropDraw
    QPointF cropPressDoc_;             // press point (anchor / move delta base)
    QRectF cropPressRect_;             // frame at press (resize/move base)

    // Brush: paint one dab at docPos and mark the stroke dirty. Reads the
    // active tool's brush size/hardness/opacity options + foreground colour.
    void paintDabAt(const QPointF& docPos);
    // Symmetry fan-out: paints the dab plus its canvas-center mirrors in
    // one undo step (same hose cell, mirrored offsets/directions).
    void paintSymmetricAt(const QPointF& docPos);
    double strokeRadius() const;
    // Auto-tip shape for the active tool: ratio (minor/major), rotation
    // degrees, square flag. Defaults reproduce the legacy round dab.
    void brushTipShape(double& ratio, double& angleDeg, bool& square) const;
    // Rotation mode for a tool (0 off, 1 tilt, 2 drawing angle, 3 pressure,
    // 4 barrel, 5 fuzzy). Honors the legacy tilt toggle when no mode is set.
    int brushRotationMode(ToolId tool) const;
    // Pressure response multipliers for a tool at pressure p: the preset's
    // authored curve when one is live for the stroke, else the built-in
    // response (concave size, linear opacity, flat flow). Gates off = 1.
    // (Size/opacity gates now live in the per-move DabToolOpts cache;
    // flow has no gate and stays direct.)
    double pressureFlowMult(ToolId tool, double p,
                            const sensordrive::SensorState& st) const;
    // Per-move dab options: every tool option paintDabAt needs, resolved
    // once per input event instead of once per dab (fast strokes lay
    // hundreds of dabs per event; only pressure/position/latches vary per
    // dab). Refreshed at press, per move, and per airbrush tick; paintDabAt
    // re-refreshes on a tool mismatch as a safety net.
    struct DabToolOpts {
        ToolId tool = ToolId::Brush;
        double baseRadius = 0.0;
        double hardness = 50.0;
        double tipRatio = 1.0, tipAngle = 0.0;
        bool squareTip = false;
        double opacity01 = 1.0, flow01 = 1.0;
        bool wash = true;
        int rotationMode = 0, sourceMode = 0, flip = 0;
        double tiltSizeAmt = 0.0, tiltOpacityAmt = 0.0;
        bool tangentialOn = false;
        bool pressureSizeOn = true, pressureOpacityOn = true;
        double scatterPct = 0.0, scatterAx = 1.0, scatterAy = 1.0;
        double densityGate = 100.0;
        QString hoseId, stampId;
        int stampMode = 0;
        bool smudge = false;
        double smudgeRate = 0.7, smudgeRadiusFrac = 1.0;
        int smudgeMode = 0;  // 0 dulling, 1 smear
        double smudgeColorRate = 0.0;  // 0..1 FG reload per dab
        double smudgeLength = 1.0;     // smear trail in radii
        // Generic sensor drives (decoded per event; empty = legacy).
        std::vector<sensordrive::SensorDrive> sensorDrives;
        double fadeLen = 0.0, darkenAmt = 0.0, hueJitterAmt = 0.0;
        double satJitterAmt = 0.0, valJitterAmt = 0.0;
        bool pressureIn = false;
        double speedSizeAmt = 0.0;
        // Tilt-axis split (size): independent X/Y lean amounts. The
        // combined tilt amounts above behave as elevation response.
        double tiltXSizeAmt = 0.0, tiltYSizeAmt = 0.0;
        // Time fade (seconds, 0 = off): like fade but over stroke elapsed
        // time instead of travelled distance.
        double timeFadeSec = 0.0;
        // Fuzzy-stroke clock (0..100): one hash from the stroke seed scales
        // size/opacity for the whole stroke (no RNG draw, so reseeded
        // strokes replay bit-exactly).
        double fuzzySizeAmt = 0.0, fuzzyOpacityAmt = 0.0;
        // Perspective depth (0..100) around a vanishing point (doc px;
        // -1,-1 = canvas center): dabs shrink toward the point.
        double perspectiveAmt = 0.0, vpX = -1.0, vpY = -1.0;
        // Pressure followers (all off by default): grain strength, mask
        // ratio and smudge rate track the dab pressure. Gradient source
        // length in px (FG→BG over the travelled stroke).
        bool texturePressure = false, maskPressure = false;
        bool smudgePressure = false;
        double gradientLen = 500.0;
        // Tip filter tier: 1 bilinear (smooth stamps), 0 nearest (draft:
        // crisp pixel-art stamps, cheaper per texel). Auto tips are always
        // analytic; the filter governs bitmap stamps only.
        int tipFilter = 1;
        // Erase blend (drawing eraser): Brush/Pencil dabs erase with the
        // live tip instead of painting — same size, pressure and rotation.
        bool eraseBlend = false;
    };
    void refreshDabOpts();
    // Fade taper 0..1 from travelled distance (1 when off). Speed-size
    // factor 0.25..1 (pure; 1 at rest or when off). The dab size/opacity
    // factors combine pressure response with tilt, fade and speed exactly
    // as paintDabAt does, so gap-fill spacing strides on the dab it lays.
    double fadeFactor() const;
    double timeFadeFactor() const;
    // Perspective depth factor at a document point (pure math above).
    double perspectiveFactor(const QPointF& docPos) const;
    double dabSizeFactor(double pr,
                         const sensordrive::SensorState& st) const;
    double dabOpacityFactor(double pr,
                            const sensordrive::SensorState& st) const;
    // Per-dab sensor snapshot for the drive factors below. Draws one stroke
    // RNG value shared by all wired properties when any of them needs a
    // per-dab fuzzy draw (empty drive list draws nothing: legacy streams
    // never shift).
    sensordrive::SensorState dabSensorState(double pr);
    // Color Replacement palette lock at an explicit document point (shared by
    // the Alt+click gesture and the options-bar button).
    void lockReplacePaletteAt(const QPointF& docPos);
    // Measure readout: ruler length/angle through the drawing-scale units.
    // Shown while measuring; silent when there is no live measurement.
    void updateMeasureHint();
    void updateAreaHint();
    void syncCursorOverride();

    // Liquify (displacement warp). A stroke is
    // press → one mesh dab per move (re-rendering only the dab's footprint from
    // the frozen snapshot through the current mesh) → release, with one history
    // entry for the whole stroke. The mesh is in layer native pixel space.
    void beginLiquifyStroke(const QPointF& docPos);
    void liquifyDabAt(const QPointF& docPos);
    void liquifyDabPoint(const QPointF& docPos, float dxLayer, float dyLayer);
    void finishLiquifyStroke(bool commit);

    // Soft proof (View menu): when proofActive(), the blit below draws the
    // proof-rendered composite instead of the raw one (vector fast path is
    // bypassed so every pixel is proofed). The manager is GUI-thread-only
    // and reconfigured only when the setup fingerprint moves.
    bool proofActive() const;
    // Proof of `docRect` (empty = the whole document). Proofing the damaged
    // rect on every repaint is what made zoom lag with a profile applied, so
    // the result is cached against the document's revision and the proof
    // setup: a cache hit returns the cached image straight away, a miss
    // proofs (and caches) what is needed. *srcRect receives the sub-rect of
    // the returned image that maps onto the request; null skips that.
    QImage proofedComposite(const QRect& docRect, QRect* srcRect = nullptr) const;
    void syncProofConfig() const;
    // Move tool: the transform box is drawn for the active pixel layer while
    // the Move tool is live and its Show Transform Controls option is on.
    bool moveTransformVisible() const;
    // Document-space bounds of the active layer, or a null rect when there is
    // nothing transformable.
    QRectF activeLayerBounds() const;
    // Memoized computation behind moveTransformVisible()/activeLayerBounds():
    // for a group both are O(N) walks over the whole subtree, so hover moves
    // and every canvas repaint can't afford to recompute them for thousands of
    // SVG parts. Recomputes only when the active layer or the document's
    // composite revision changes.
    void ensureGizmoCache() const;
    // The 8 transform handles, clockwise from top-left: TL T TR R BR B BL L.
    QPointF handleDocPos(int handle, const QRectF& bounds) const;
    // Handle under a view-space point, or -1 (7 px tolerance).
    int handleAtView(const QPointF& viewPos) const;
    // Begin dragging the layer body (auto-selects under Auto-Select).
    bool beginMoveDrag(const QPointF& docPos);
    // Click-to-select: pick the topmost layer under a document point and reveal
    // it in the Layers panel (select + scroll to its row). Used by Ctrl+click
    // with any non-Move tool; the Move tool reaches the same selection through
    // beginMoveDrag. Returns true when a layer was picked.
    bool pickLayerAt(const QPointF& docPos);
    // True when a plain press lands inside the current selection (any
    // selected visible layer, groups by their bounds; the active layer when
    // the set is empty), so the drag keeps it instead of auto-selecting.
    bool pressHitsSelection(const QPointF& docPos) const;
    // Begin a scale drag from a handle; corners are proportional unless Shift
    // is held, edges scale one axis.
    bool beginScaleDrag(int handle, const QPointF& docPos);
    void updateScaleDrag(const QPointF& docPos, bool proportional);
    // Place one dropped image, opening a document for it when none is open.
    void acceptDropImage(const QImage& img, const QString& name,
                         const QPointF& docPos);

    // Type tool: an editing session owns one live text layer. `startTextEdit`
    // takes over the session (and the undo step, unless the caller already
    // began one). `commitTextEdit` ends it: an unchanged session discards the
    // undo step, and an empty live text layer is removed outright. `handleTextKey`
    // consumes a key press while editing; returns true when it did.
    void startTextEdit(int index, bool undoBegan = false);
    void commitTextEdit();
    bool handleTextKey(QKeyEvent* event);
    // True when a key press would be consumed by the live text session (used to
    // claim QEvent::ShortcutOverride so window-wide single-key shortcuts yield).
    bool textKeyConsumes(const QKeyEvent* event) const;
    void setTextContent(int index, const QString& text);

    // Text-editing session internals. The caret and anchor are UTF-8 byte
    // offsets into the live layer's run, matching the engine's layout.
    double textAlignShift() const;
    QPointF textLayoutToDoc(const QPointF& layoutPoint) const;
    QPointF textDocToLayout(const QPointF& docPoint) const;
    // Recompute the cached layout from the live layer's current spec.
    void refreshTextLayout();
    // Byte offset nearest a document point within the active run.
    std::size_t textOffsetAtDoc(const QPointF& docPoint) const;
    // True when a document point falls within the active run's line boxes.
    bool textPointInRun(const QPointF& docPoint) const;
    // Pen x of `byte` on `span` (line end when the byte is past it).
    double textLineX(const pittore::text::LineSpan& span, std::size_t byte) const;
    // Move the caret; `extend` keeps the anchor and grows the selection.
    void setTextCaret(std::size_t byte, bool extend);
    // Select the word (or whitespace run) containing `byte`, for double-click.
    void selectWordAt(std::size_t byte);
    // Replace the bytes [from, to) with `insert`; the caret lands after it.
    void replaceTextRange(std::size_t from, std::size_t to, const QString& insert);
    // The caret's top and bottom in document space (a vertical segment for the
    // horizontal runs we render). False when no live layer owns the session.
    bool textCaretSegment(QPointF& top, QPointF& bottom) const;
    std::size_t textByteLength() const;

    AppState* state_;
    ContextualTaskBar* taskBar_ = nullptr;
    CanvasRuler* horizontalRuler_ = nullptr;
    CanvasRuler* verticalRuler_ = nullptr;
    QWidget* rulerCorner_ = nullptr;
    QTimer* antsTimer_ = nullptr;
    int antsPhase_ = 0;

    // Object Selection (AI): precomputed SAM image embeddings, cached per
    // layer image — any pixel edit swaps the Image (copy-on-write) and the
    // pointer key changes, forcing a re-encode. Also keyed by model so a
    // different encoder never reuses another's embeddings.
    std::shared_ptr<const pittore::ai::SamEncodings> objectEncodings_;
    const void* objectEncodingsKey_ = nullptr;
    QString objectEncodingsModel_;
    // Full-frame segmentation cache (single-file person/person models):
    // segment_rgba8 returns the whole subject in one shot; cached per layer
    // image + model so hover moves and repeated clicks stay cheap.
    std::vector<float> segmentAlphaCache_;
    const void* segmentAlphaCacheKey_ = nullptr;
    QString segmentAlphaCacheModel_;
    // conventional Object Select: encode the active layer once, prompt the
    // SAM decoder at the document pixel, select the object as an
    // arbitrary-shape mask (drawn as a marching-ants outline). Returns false
    // when nothing was selected (e.g. the model is not a pair).
    bool runAiObjectSelect(const QPoint& docPixel);
    // Quick Selection: the AI segmentation is unioned under the brush stroke.
    // `from`/`to` are the press/release document points; up to a handful of
    // sample points along the segment are decoded with the shared SAM path and
    // their masks combined into the live selection with `mode` (see
    // AppState::combineSelection). One undoable step for the whole gesture.
    void runAiQuickSelect(const QPointF& from, const QPointF& to, int mode);
    // The shared decode path behind the tool: fills `maskOut` with the
    // doc-space selection channel (largest component kept) for the object the
    // decoder finds at `docPixel`. `status` carries a user-facing reason on
    // failure. Encodings are cached per layer image + model, so hover moves
    // stay cheap. `expand` runs the multi-point union pass — a single SAM
    // point prompt can stop at a sub-part of the object, so the committed
    // selection (and the settled hover preview) probe extra points inside the
    // mask and take the element-wise max; the live hover skips it for speed.
    bool objectSelectMaskAt(const QPoint& docPixel, QImage& maskOut,
                            float& iouOut, QString& status, bool expand = true);
    // Vector marching-ants outline of an arbitrary-shape (mask) selection,
    // cached until the mask changes (document selectionStamp).
    QPainterPath selectionOutlinePath();
    QPainterPath selectionOutline_;
    quint64 selectionOutlineStamp_ = 0;

    // Object Select hover preview: while the pointer hovers over a pixel layer
    // (tool active, Object Finder on, not dragging), the decoder runs at the
    // cursor and its mask is shown as a blue overlay; the click commits.
    QImage hoverPreviewMask_;
    QPoint hoverPreviewPoint_;
    bool hoverPreviewActive_ = false;
    bool hoverPreviewExpanded_ = false;
    qint64 hoverPreviewLastMs_ = 0;
    QTimer* hoverSettleTimer_ = nullptr;
    void onHoverSettle();

    // Zoom settle: wheel/pinch/scrub zoom re-bakes vector art at ceil(zoom)
    // density plus a full composite + re-upload per density step (see
    // bakeArtDense). Doing that synchronously in setZoom turned every wheel
    // tick into 25–65ms of gather+placed work. The timer coalesces the burst
    // into one rebake on settle; the frames themselves are one composite
    // blit plus direct geometry draws, so zooming stays smooth and lands
    // crisp.
    QTimer* zoomSettleTimer_ = nullptr;

    // A document opens at Fit on Screen whenever it is larger than
    // the window. The viewport has no useful size until the first resize, so
    // the fit is deferred rather than applied when the document arrives.
    bool pendingFit_ = true;

    bool rulers_ = true;
    bool rulerGuideDragging_ = false;
    bool rulerGuideHorizontal_ = true;
    QPointF rulerGuideDoc_;
    bool guides_ = true;
    bool grid_ = false;
    bool symmetryX_ = false;
    bool symmetryY_ = false;
    bool selectionEdges_ = true;
    bool smartGuides_ = true;
    bool pixelGrid_ = false;
    bool extras_ = true;

    // Interaction state.
    bool dragging_ = false;
    bool panning_ = false;
    QPointF dragStartDoc_;
    QPointF dragCurrentDoc_;
    // Manual double-click detector: some setups never deliver a real
    // MouseButtonDblClick to the canvas, so the second quick press on a text
    // layer starts editing directly instead of beginning a move/paint drag.
    // (When Qt does deliver the DblClick event afterwards, the live-session
    // branch of the shared core turns it into a word selection.)
    qint64 lastPressMs_ = 0;
    QPointF lastPressView_;
    bool lastPressOnCanvas_ = false;
    // Last viewport DPR seen by the paint path. Moving the window across
    // mixed-DPR screens changes devicePixelRatioF without any zoom change:
    // tile buckets key on zoomEff (zoom × DPR) so they miss and rebake,
    // and the checker key includes DPR — but scroll ranges and print-size
    // assumptions must be re-evaluated, so the paint path syncs on change.
    qreal lastPaintDpr_ = 0.0;
    // The tool that owned the previous press: switching tools between clicks
    // restarts the double-click window (a fresh gesture, not a second press).
    ToolId lastPressTool_ = ToolId::Move;
    // Marquee modifier state from the live gesture's event stream (see
    // marqueeLiveRect): set on press, refreshed on every move, read by the
    // overlay and the release commit.
    Qt::KeyboardModifiers marqueeMods_ = Qt::NoModifier;
    // Shape-drag press point. Shift/Alt recompute both drag ends absolutely
    // from this anchor every move event, so toggling keys mid-drag is stable.
    QPointF shapeAnchorDoc_;
    QPointF panStartView_;
    QPoint panStartScroll_;
    QPointF cursorDoc_;
    // Brush-size cursor: every tool that strokes with the dab kernel shows its
    // brush tip as an outline ring under the pointer (the brush
    // cursor). `brushCursorRect_` caches the last drawn view-space ring so
    // hover moves repaint only the old ∪ new strip;
    // `cursorInViewport_` stops the ring appearing at a stale cursor position
    // before the pointer enters (or once it leaves) the canvas.
    bool cursorInViewport_ = false;
    QRectF brushCursorRect_;
    // Whether the application-level blank cursor is currently pushed (the
    // pointer is over a brush tool's canvas). Keeps push/pop balanced.
    bool cursorOverrideActive_ = false;
    // Last rubber-band strip (view coords) repainted by a marquee drag, so the
    // next move can erase the previous band by repainting old ∪ new.
    QRectF liveViewRect_;

    // Brush stroke state (painting tools). A stroke = press → dabs on every
    // move (gap-filled) → release; one history entry per stroke.
    bool strokeActive_ = false;
    bool strokePainted_ = false;   // any dab landed since the drag began
    QPointF strokeLastDoc_;
    double strokeLastPressure_ = 1.0;  // pressure at strokeLastDoc_ (gap-fill ramp)
    std::size_t strokeDabCount_ = 0;   // dabs since press (hose cycling)
    // Stabilizer anchor + raw tracker + speed EMA for the live stroke.
    QPointF strokeSmoothDoc_;
    QPointF strokeLastRawDoc_;
    std::uint64_t strokeLastMoveMs_ = 0;
    // Color Replacement "Once" sampling: the target set locked at the
    // stroke's first dab, re-armed (empty + flag false) on every press.
    bool replaceStrokeHasTargets_ = false;
    std::vector<pittore::RGBAf> replaceStrokeTargets_;
    // Alt+Right-drag brush resize (conventional behaviour): drag left/right for
    // size, up/down for hardness. Writes brush_size / brush_hardness live.
    bool brushResizeActive_ = false;
    QPointF brushResizeStartView_;
    double brushResizeStartSize_ = 64.0;
    double brushResizeStartHardness_ = 50.0;
    // Set when a brush resize just ended: the right-release that ends it
    // would otherwise pop the canvas context menu, so the next
    // contextMenuEvent is swallowed instead.
    bool brushResizeSuppressMenu_ = false;
    // Scrubby zoom (Zoom tool drag): press-drag scrubs continuously; a press
    // without drag falls back to the classic step zoom on release.
    bool zoomScrubbing_ = false;
    QPointF zoomScrubAnchorView_;
    QPointF zoomScrubLastView_;
    double zoomScrubStart_ = 1.0;
    bool zoomScrubMoved_ = false;
    // Move Alt+drag duplicate: the press duplicated the selection, so the
    // drag moves the fresh copies (one extra history step for the copy).
    bool moveDuplicated_ = false;
    // Color Replacement Alt+click palette lock: the explicitly picked area
    // colours, used for every stroke until re-picked or the tool changes.
    bool replacePaletteLocked_ = false;
    std::vector<pittore::RGBAf> replacePaletteTargets_;
    // Clone Stamp source state: the Alt-pinned anchor (document coords) and
    // the live stroke offset (dab → source, document coords). A fresh
    // Alt-click clears the offset so the next press re-derives it.
    QPointF cloneAltPoint_;
    bool cloneHasAlt_ = false;
    QPointF cloneOffset_;
    bool cloneHasOffset_ = false;

    // Liquify stroke state. `liquifyMesh_` starts at the identity on press and
    // accumulates the brush dabs; it is in layer native pixel space.
    bool liquifyActive_ = false;
    bool liquifyMoved_ = false;   // the mesh actually deformed since press
    pittore::compute::WarpMesh liquifyMesh_;
    QPointF liquifyLastDoc_;

    // Soft-proof transform (GUI thread only): created on first proofed
    // paint, reconfigured when the setup fingerprint moves. Mutable: paint
    // is const and configuration rides the settings, like the gizmo cache.
    mutable std::unique_ptr<pittore::color::ProofManager> proofManager_;
    // The setup last attempted (profile/intent/bpc/gamut), compared piecewise
    // so an unchanged repaint does no formatting, and so a refused profile is
    // attempted once instead of once per paint.
    mutable bool proofAttempted_ = false;
    mutable QString proofProfile_;
    mutable int proofIntent_ = -1;
    mutable bool proofBpc_ = false;
    mutable bool proofGamut_ = false;
    // Proofed-composite cache keyed on the document, its revision and size
    // (the setup is cleared through syncProofConfig when it moves). The
    // document pointer is part of the key because revisions restart per
    // document: two tabs could otherwise hand each other's proofed pixels to
    // the blit. Proofing the visible document on every zoom frame cost a
    // float conversion plus a CMS transform per pixel per repaint; with this
    // the zoom frames are blits. Cleared on any composite change.
    mutable QImage proofCache_;
    mutable const DocumentItem* proofCacheDoc_ = nullptr;
    mutable QRect proofCacheRect_;        // document rect proofCache_ covers
    mutable QSize proofCacheDocSize_;     // document size it was built for
    mutable std::uint64_t proofCacheRev_ = ~std::uint64_t(0);

    // Memoized Move-gizmo geometry shared by moveTransformVisible() and
    // activeLayerBounds() (see ensureGizmoCache()). Mutable: both accessors
    // are const and run on hover/repaint hot paths.
    mutable const DocumentItem* gizmoCacheDoc_ = nullptr;
    mutable int gizmoCacheLayer_ = -1;
    mutable bool gizmoCacheValid_ = false;
    mutable std::uint64_t gizmoCacheRev_ = 0;
    mutable QRectF gizmoCacheBounds_;
    mutable bool gizmoCacheHasPixels_ = false;

    // Move-tool drag state: dragging the layer body, or scaling from one of
    // the 8 transform handles around the opposite anchor.
    bool moveDragging_ = false;
    bool scaleDragging_ = false;
    bool moveChanged_ = false;   // the layer actually moved since press
    int activeHandle_ = -1;
    QPointF moveGrabOffset_;       // press point minus layer offset
    // The active layer's offset at press: the baseline a move-drag delta (and
    // its snap) is measured against for moving the whole selection together.
    QPointF moveAnchorStartOffset_;
    // Every selected pixel layer's offset at press, aligned with
    // selectedLayerIndices() — moveSelectedLayers() places each layer at
    // start + delta so the drag never accumulates onto a moved layer.
    QVector<QPointF> moveLayerStarts_;
    // The layers this gesture moves/scales: the selection for a plain pixel
    // layer, or a group's pixel descendants when the active layer is a group
    // (so the whole group drags/resizes as one unit). Index-aligned with
    // moveLayerStarts_ / scaleStartOffsets_ etc.
    QVector<int> moveLayerSet_;
    QPointF scaleAnchorDoc_;       // fixed opposite point during a scale drag
    QPointF scaleStartOffset_;
    double scaleStartX_ = 1.0;
    double scaleStartY_ = 1.0;
    QRectF scaleStartBounds_;
    // Per-layer scale baselines for group scaling (one entry per
    // moveLayerSet_ entry): the press-time offset/scale of each descendant.
    QVector<QPointF> scaleStartOffsets_;
    QVector<double> scaleStartXs_, scaleStartYs_;
    // A live text layer scales through its point size: these are the press-time
    // size and layout origin the corner drag interpolates from.
    double scaleStartTextSize_ = 0.0;
    QPointF scaleStartTextOrigin_;

    // Drag-and-drop: a highlight while an image hovers the viewport.
    bool dropActive_ = false;

    // Type tool: the live editing session, and the drag that sizes a frame box.
    bool textEditing_ = false;
    int textEditIndex_ = -1;
    bool textEditChanged_ = false;
    // Type Mask mode: 0 none, 1 horizontal, 2 vertical. While set, the
    // live session commits into a selection mask (no text layer survives).
    int textMaskMode_ = 0;
    QPointF textEditOrigin_;
    bool textCreating_ = false;
    QPointF textCreateStartDoc_;
    double textCreateSize_ = 0.0;   // font size implied by the drag distance
    bool textCreateFrame_ = false;  // Shift-drag sizes a frame box instead
    bool textCaretOn_ = true;
    QTimer* textCaretTimer_ = nullptr;
    // Caret and selection in UTF-8 byte offsets, plus the cached layout of the
    // live run (rebuilt on every edit) so hit-testing never re-shapes per move.
    std::size_t textCaret_ = 0;
    std::size_t textAnchor_ = 0;
    bool textSelecting_ = false;
    bool textLayoutValid_ = false;
    pittore::text::TextLayout textLayout_;

    // Move-drag smart guides. While a move drag is live a faint document-centre
    // crosshair is shown (green vertical, red horizontal) so the centre is a
    // live readout in both directions; the axis brightens when the drag snaps
    // to the centre, and any other snapped target gets its own line.
    MoveSnap moveSnap_;

    // Rotate View: a drag rotates the canvas about the viewport centre, the
    // pointer's bearing from that centre driving the angle.
    bool rotating_ = false;
    double rotateStartPointerAngle_ = 0.0;  // degrees at press
    double rotateStartRotation_ = 0.0;      // document rotation at press

    // Ruler: press starts a measurement, move updates the far endpoint. The
    // Measure tool shares the gesture (same line, same undo); only its readout
    // converts through the drawing scale.
    bool rulerDragging_ = false;

    // Area: press starts a measured rectangle, move updates the far corner.
    // Same undo and annotation plumbing as the ruler line.
    bool areaDragging_ = false;
    QPointF areaAnchorDoc_;

    // Red Eye: press-drag boxes the pupil, release fixes it.
    bool redeyeDragging_ = false;
    QPointF redeyeAnchorDoc_;

    // Node tool: drag an ArtNode endpoint of the picked art layer. The working
    // copy previews in the overlay; release commits one re-raster step.
    bool nodeDragging_ = false;
    int nodeLayer_ = -1;
    int nodeSeg_ = -1;
    QPointF nodeGrabDelta_{0, 0};  // endpoint minus cursor, node space
    pittore::vector::ArtNode nodeWork_;
    bool nodeMoved_ = false;
    // Handle drag: anchor segment + side (0 in, 1 out); -1 means the drag is
    // an endpoint drag. Mirror preserves smoothness unless Alt is held.
    int nodeHandleSeg_ = -1;
    int nodeHandleSide_ = 0;
    bool nodeHandleMirror_ = false;
    QPointF nodeHandleGrabDelta_{0, 0};
    // Click-selected anchor for the Node convert/close actions.
    int nodeSelLayer_ = -1;
    int nodeSelSeg_ = -1;
    // Pen / Freehand / Curvature: the in-progress path in document coords.
    // Clicks add anchors (drag shapes Bezier handles); double-click, Enter
    // or click-to-close finishes one layer; Escape cancels. Freehand commits
    // on release instead. The overlay previews the working path.
    bool penActive_ = false;
    ToolId penTool_ = ToolId::Pen;
    PenPath penPath_;
    bool penPressed_ = false;
    QPointF penPressDoc_;
    bool penShaping_ = false;  // press-drag is shaping handles, not a click
    QPointF penHoverDoc_;      // rubber-band cursor, document coords
    // Finishes the working path as one layer (open unless closed), then
    // clears it. False when there is nothing committable (path kept).
    bool finishPenPath();
    void cancelPenPath();
    // Magnetic drawing: snap a document point to a nearby art endpoint.
    // True + `*out` when one lands within tolDoc.
    bool snapPenToArt(const QPointF& docPos, double tolDoc, QPointF* out);

    // Vector Brush: streamed centerline + per-dab widths, committed as a
    // filled ribbon on release. The overlay previews the working ribbon.
    bool vbrushActive_ = false;
    BrushStroke vbrushStroke_;
    // Tablet stylus state for pressure-driven width (controller Pressure):
    // pressure is only valid between TabletPress and TabletRelease.
    bool tabletDown_ = false;
    double tabletPressure_ = -1.0;
    // Pixel-brush pressure latch (size+opacity response). Written
    // from real stylus events only; a mouse (or untouched run) sits at
    // exactly 1.0, so non-tablet behaviour is bit-identical.
    double pixelPressure_ = 1.0;
    // Stylus tilt latch, degrees (-60..60 each axis, 0/0 for a mouse).
    // Drives tip rotation when the tool opts in; otherwise ignored.
    double tiltX_ = 0.0;
    double tiltY_ = 0.0;
    // Barrel rotation latch, degrees (0 for a mouse). Drives tip rotation
    // in that rotation mode; otherwise ignored.
    double barrelRotation_ = 0.0;
    // Wheel (tangential pressure) latch, 0..1 (0 for a mouse or a device
    // without a wheel). Scales flow when the tool opts in; else ignored.
    double tangentialPressure_ = 0.0;
    // Symmetry copy flags for the dab in flight: paintSymmetricAt sets them
    // per mirror so angled tips negate correctly; exactly one flip negates.
    bool symFlipX_ = false;
    bool symFlipY_ = false;
    // Eraser-end auto-switch: the pixel Eraser stands in for the stroke and the
    // previous tool returns on release (when still active).
    bool stylusSwitched_ = false;
    ToolId stylusReturnTool_ = ToolId::Brush;
    // Eraser end on a Brush/Pencil keeps the live tip and flips erase
    // blend instead of swapping tools; cleared on release when we set it.
    bool stylusEraseBlend_ = false;
    // Smoothed pointer speed, px/sec EMA. Feeds hose velocity indexing;
    // mice and instant synthetic events read ~0 (no-op) by construction.
    double strokeSpeed_ = 0.0;
    // Travelled stroke length, doc px (fade taper), and running-maximum
    // pressure (PressureIn hold). Re-armed at press, extended per move.
    double strokeDist_ = 0.0;
    double strokeMaxP_ = 1.0;
    // Stroke clock: press-event timestamp (ms) and elapsed ms updated per
    // move (time fade). Fuzzy-stroke hash from the stroke seed.
    quint64 strokePressMs_ = 0;
    quint64 strokeElapsedMs_ = 0;
    double fuzzyStrokeH01_ = 0.5;
    DabToolOpts dabOpts_;
    // Airbrush metronome: while a stroke is live with the tool's airbrush
    // toggle on, ticks lay dabs at the cursor on the tool's rate.
    QTimer* airbrushTimer_ = nullptr;
    void startAirbrush(ToolId tool);
    // Width for a dab at `docPos` from the bar's Width/Controller (velocity
    // thins fast spans; pressure scales when a tablet stroke is live).
    double vbrushWidthAt(const QPointF& docPos) const;
    // Nib shape for the bar's tip options (roundness/angle/square): the
    // calligraphic nib buildRibbon sweeps. Defaults reproduce the round
    // ribbon exactly.
    void vbrushNib(double& ratio, double& angleDeg, bool& square) const;
    // Stylus input for the vector brush (other tools ignore tablet events
    // so Qt synthesizes the usual mouse gesture). True when consumed.
    bool handleTabletEvent(QTabletEvent* event);
    // Returns the pre-eraser tool after an eraser-end stroke (see above).
    void restoreStylusTool();

    // Gradient / Transparency on art: press-drag defines the axis in
    // document coords; release maps it to node space and commits one paint
    // step. The overlay previews the live axis.
    bool vgradActive_ = false;
    ToolId vgradTool_ = ToolId::Gradient;
    int vgradLayer_ = -1;
    QPointF vgradStartDoc_;
    QPointF vgradCurDoc_;
    // Map a document point into an art layer's node space. False when the
    // layer has no usable geometry/placement.
    bool docToArtNode(int layerIndex, const QPointF& docPos,
                      QPointF* nodePos);

    // Knife: press-drag draws the cut line; release cuts the art beneath.
    bool knifeActive_ = false;
    QPointF knifeStartDoc_;
    QPointF knifeCurDoc_;
    // Stroke Width: drag shapes the width profile at the cursor (Shift
    // scales the base width uniformly instead). The overlay previews the
    // working paint; release commits one step. Double-click resets uniform.
    bool swActive_ = false;
    int swLayer_ = -1;
    double swStartWidth_ = 1.0;
    double swLiveWidth_ = 1.0;
    QPointF swPressDoc_;
    pittore::vector::ArtPaint swStartPaint_;
    pittore::vector::ArtPaint swPreviewPaint_;
    bool swHasPreview_ = false;
    // Flattened centerlines (node space) + per-subpath lengths for the
    // profile t lookup while dragging.
    pittore::vector::Path swFlat_;
    std::vector<double> swTotals_;
    // Point Transform: drag an anchor freely, or Shift-drag to scale /
    // Alt-drag to rotate all anchors about their centroid. Working copy
    // previews; release commits one step.
    bool ptDragging_ = false;
    int ptLayer_ = -1;
    int ptSeg_ = -1;
    int ptMode_ = 0;  // 0 move, 1 scale, 2 rotate
    QPointF ptCentre_;
    pittore::vector::ArtNode ptWork_;
    pittore::vector::ArtNode ptOrig_;
    QPointF ptPressDoc_;
    QPointF ptGrabDelta_{0, 0};  // anchor minus cursor, node space
    bool ptMoved_ = false;

    // Shape Builder: press-drag marquees art to combine/delete (a click
    // picks the topmost). Vector Flood Fill commits on press instead.
    bool builderActive_ = false;
    QPointF builderStartDoc_;
    QPointF builderCurDoc_;

    // Vector drag tools (own TU: canvas/pen/canvas_vector_tools):
    // press-drag-release creation gestures over addVectorPathLayer, with
    // modify-in-place upgrades when existing art is hit (tweak/eraser/LPE).
    bool inkActive_ = false;
    ToolId inkTool_ = ToolId::Move;
    QPointF inkStartDoc_;
    QPointF inkCurDoc_;
    std::vector<std::pair<double, double>> inkSpine_;  // calligraphy stream
    bool inkPress(const QPointF& docPoint);
    bool inkMove(const QPointF& docPoint);
    bool inkRelease(const QPointF& docPoint);
    // Drag-preview for the vector tools (own TU): spray cone, tweak ring,
    // LPE chord, connector rubber-band. No-op otherwise.
    void paintInkPreview(QPainter& painter, const QTransform& viewMap);

    // Style Picker: sampled paint waiting for its target layer. Value-kept so
    // layer add/remove between pick and apply cannot dangle.
    std::optional<pittore::vector::ArtPaint> stylePicked_;
    double stylePickedOpacity_ = 1.0;

    // Count: dragging an existing marker.
    bool countDragging_ = false;
    bool countDragged_ = false;
    int countDragIndex_ = -1;
    QPointF countDragGrab_;

    // Note hover index, so the viewport tooltip can show the note's text.
    int noteHover_ = -1;
    // Note: pressing an existing note grabs it. A drag moves it (one history
    // step on release, like the reference); a click with no movement opens its
    // text for editing instead.
    bool noteDragging_ = false;
    bool noteDragged_ = false;
    int noteDragIndex_ = -1;
    QPointF noteDragGrab_;
    QPointF noteDragOrig_;

    // Slice tool (R19): press-drag draws a new export rectangle; the live
    // rectangle is shown dashed until release. Slice Select drags an existing
    // slice (document->selectedSlice follows the press hit); released as one
    // history step.
    bool sliceDragging_ = false;
    QPointF sliceDragAnchorDoc_;
    QPointF sliceDragCurrentDoc_;
    bool sliceSelectDragging_ = false;
    bool sliceSelectMoved_ = false;
    int sliceSelectIndex_ = -1;
    QPointF sliceSelectGrabDoc_;
    QPointF sliceSelectStartOffset_;
    // Frame: press-drag draws the placeholder box; release commits it as a
    // vector shape layer (Rectangle/Ellipse by the shape toggle).
    bool frameDragging_ = false;
    QPointF frameAnchorDoc_;

    // Artboard: press-drag resizes the canvas to the dragged rect; a click
    // resizes to the W/H option size at the current origin.
    bool artboardDragging_ = false;
    QPointF artboardAnchorDoc_;

    // Perspective Crop: corner-draggable quad + double-click commit.
    // pcropQuad_ holds TL,TR,BR,BL in document space when armed.
    bool pcropArmed_ = false;
    int pcropCorner_ = -1;
    QPointF pcropQuad_[4];
    QPointF pcropAnchorDoc_;

    // Generative Fill/Background: press-drag marks the fill area as a
    // selection (the Generate button consumes it); release commits it.
    bool genMarking_ = false;

    // Content-Aware Tracing: press-drag marquees the area to vectorize;
    // release commits the selection and traces it per the output option.
    bool traceMarking_ = false;
    // Patch: Select phase draws a marquee into a new selection; Drag phase
    // moves the committed selection's outline to the donor spot (release
    // heals the original area from the drop area). patchPhase_ 0 = idle,
    // 1 = selecting, 2 = dragging. Reuses dragging_/dragStartDoc_/
    // dragCurrentDoc_ so the marquee overlay and release plumbing apply.
    int patchPhase_ = 0;
    QPointF patchAnchorDoc_;
    QPointF patchPressDoc_;
    bool patchMoved_ = false;
    // ContentAwareMove: phase 1 marquees a selection, phase 2 drags its
    // content to the drop point (hole heals on release). Same plumbing.
    int camPhase_ = 0;
    QPointF camPressDoc_;
    bool camMoved_ = false;

    // Freehand lasso (Lasso + Magnetic Lasso): press-drag streams document
    // points, release closes the loop into a polygon selection. Magnetic
    // snaps each point to the strongest nearby edge.
    bool lassoLive_ = false;
    std::vector<QPointF> lassoStroke_;
    // Polygonal lasso: click-click anchors, rubber-banded until double-click
    // or Enter commits, Escape cancels. Hover holds the live cursor end.
    std::vector<QPointF> polyPts_;
    QPointF polyHover_;
    bool polyHoverOn_ = false;
    // Selection brush: press-drag paints an incoming coverage mask (white =
    // brushed) with a soft round tip; release combines it with the live
    // selection through the tool's selmode.
    bool selBrushLive_ = false;
    QImage selBrushMask_;
    QPointF selBrushLast_;
    // Lasso helpers (own TU pieces live in the mouse handlers; small pure
    // helpers stay here so press/move/release/overlay agree).
    int lassoSelMode(Qt::KeyboardModifiers mods) const;
    void lassoCommitPolygon(const std::vector<QPointF>& loop,
                            Qt::KeyboardModifiers mods);
    QPointF magneticSnap(const QPointF& docPos) const;
    void selBrushPaintTo(const QPointF& docPos, double radius, double hardness,
                         double opacity01);
    QPainterPath lassoLivePath() const;
    QPainterPath polyLivePath() const;
};

}  // namespace pittore::ui
