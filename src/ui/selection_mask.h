#pragma once
// Freeform selections: the marching-ants outline of a grayscale channel,
// plus the shared 50% bbox test. Canvas draws it, AppState stores it.
// Split out so the outline math is testable without a widget.

#include <QImage>
#include <QPointF>
#include <QPainterPath>
#include <QRect>
#include <QSize>

#include <vector>

namespace pittore::ui {

// Marching-ants outline of a mask (> threshold = selected), as closed loops
// in mask pixels. Clipped to `region`; empty when nothing is selected.
QPainterPath selectionOutlineFromMask(const QImage& mask, const QRect& region,
                                      int threshold = 127);

// Smoothed display version of the raw outline: drops pixel spikes, merges
// straight runs, rounds corners. Passes curves through untouched.
QPainterPath selectionOutlineSmoothed(const QPainterPath& raw);

// Bbox of a mask at 50% (127); null when nothing is selected.
QRect selectionMaskBbox(const QImage& mask);

// Keep only the biggest 8-connected blob; clears stray specks. Null passes through.
QImage selectionMaskLargestComponent(const QImage& mask);

// Drop isolated 8-connected islands smaller than `minPixels`, keeping every
// other pixel byte-identical. Refine input from AI masks is often sprinkled
// with detached specks that no edge-band solver reaches (they sit outside
// any boundary band); without this they survive to the committed result.
// Connected soft edges are one mass with the subject, so only truly
// detached islands go. Null passes through.
QImage selectionMaskRemoveSpeckles(const QImage& mask, int minPixels = 32);

// Fill small enclosed background holes: non-solid regions (coverage < 255)
// fully enclosed by solid coverage (255) and smaller than `maxPixels` are set
// to 255. This cleans snap-punched texture dots and sub-strand specks when
// the edge solve runs last. Implemented as invert → RemoveSpeckles → invert:
// exterior background and the soft edge skirt stay connected through the
// inversion (kept); only truly enclosed islands are removed, so larger
// enclosed gaps (real see-through hair) and floating strands survive. Edges
// never move — only enclosed interiors change, and only upward.
QImage selectionMaskFillSmallHoles(const QImage& mask, int maxPixels = 16);

// Decay broad low-confidence wash far from the subject: partial coverage
// (0 < v < 255) outside `supportPx` of any confident (>=200) pixel, in a
// low-TEXTURE image region, is AI haze rather than hair — hair and strands
// carry fine-scale Laplacian energy while glow wash is smooth second-order
// flat (a plain gradient gate is fooled by the glow's own slope). Solid and
// near-edge coverage is untouched. `guide` is an ARGB32 image the same size
// (luma texture used). Null passes through.
QImage selectionMaskDecayWash(const QImage& mask, const QImage& guide,
                              int supportPx = 32, double texTol = 8.0);

// Fill interior holes (background the flood from the border can't reach).
// `bridgeRadius` seals narrow gaps first (scratch-only, edges never move).
// Null passes through.
QImage selectionMaskFillHoles(const QImage& mask, int bridgeRadius = 0);

// Layer alpha (floats 0..1) sampled into a doc-size selection channel.
QImage selectionMaskFromLayerAlpha(const float* alpha, int pw, int ph,
                                   const QPointF& offset, double scaleX,
                                   double scaleY, const QSize& docSize);

// Freehand / polygonal lasso raster: even-odd fill of the implicitly closed
// loop `poly` (document coords) into a document-sized coverage channel.
// `antialias` supersamples 4 sub-rows per pixel row for soft edges;
// `featherPx` (>0) softens the finished edge. Null when fewer than 3 points
// or the document size is empty.
QImage selectionMaskFromPolygon(const std::vector<QPointF>& poly,
                                const QSize& docSize, bool antialias,
                                int featherPx);

// Refine Selection helpers. Each takes a Grayscale8 channel, returns a new one.
// Grow/shrink re-softens with a 3x3 blur; null passes through unchanged.

// Grow (+) / shrink (−) the edge by `radius` px. 0 = unchanged.
QImage selectionMaskGrow(const QImage& mask, int radius);
// A few box-blur passes to even out the outline.
QImage selectionMaskSmooth(const QImage& mask, int passes);
// Feather the hard edge outward by `radius` px, keeping soft coverage.
QImage selectionMaskFeather(const QImage& mask, int radius);
// Remap coverage; >1 hardens, <1 softens (1 = unchanged).
QImage selectionMaskRamp(const QImage& mask, double ramp);
// Soft stamp toward `value` (0 bg, 255 fg, ~128 matte); hardness 1 = hard edge.
QImage selectionMaskBrushStamp(const QImage& mask, const QPointF& center,
                               double radius, double hardness, int value);
// Locally soften toward the neighborhood mean inside the disc (the Refine
// adjustment brush in Feather mode). `strength` 0..1 blends by brush falloff.
QImage selectionMaskFeatherStamp(const QImage& mask, const QPointF& center,
                                 double radius, double strength = 1.0);
// Local matting solve for the Refine adjustment brush in Matte mode. Every
// pixel gets its own foreground/background models: box means of the
// confident (coverage >= 200 / <= 55) guide pixels in a 1.5x radius
// neighbourhood. Coverage is the projection of the pixel colour onto the
// local foreground/background colour line; pixels whose box lacks either
// side, or whose mean colours sit closer than 12 per channel of RGB
// separation, keep their current coverage (never stamped uncertain). The
// models are re-estimated once from the trial result so a polluted loose
// matte converges, then the result is blended by a soft disc falloff so
// strokes never leave rims. `color` is an ARGB32 guide of the same size.
QImage selectionMaskMatteSolve(const QImage& color, const QImage& mask,
                               const QPointF& center, double radius);
// Edge-snapped refinement (the Refine dialog's Matte edges): within `bandPx`
// of the current edge, coverage is attracted toward the image's own edge from
// `guide`, so hair/soft detail lands on real image boundaries. An ARGB guide
// solves in RGB colour space (fg/bg colour models — cool backdrop vs warm
// subject of the same tone still separate); a grayscale guide solves on luma
// alone. Passes run the attraction repeatedly; 0 disables.
QImage selectionMaskSnapToEdges(const QImage& guide, const QImage& mask,
                                int bandPx, int passes = 3);
// Smart-radius snap: a narrow band pass everywhere plus a wide band pass
// accepted only where the mask edge is soft (high local alpha variance, i.e.
// hair/fur ramps rather than hard boundaries). Mirrors Photoshop's Smart
// Radius: hard edges keep a tight treatment while soft zones get the wide
// one, in a single run. `widePx` should exceed `narrowPx`; 0 disables.
QImage selectionMaskSnapSmart(const QImage& guide, const QImage& mask,
                              int narrowPx, int widePx, int passes = 3);
// AI hair enhance blend (the Refine dialog's Enhance Edges button): take the
// portrait-matting AI's soft `aiMask` only inside `bandPx` of the current
// `base` edge, keep `base` everywhere else. `aiMask` polarity is auto-fixed
// (whichever of ai / inverted-ai agrees with `base` at >127 wins), so a model
// that returns background instead of subject can never flip the matte. Null
// / size mismatch passes `base` through; empty or full `base` is returned
// unchanged (nothing to enhance).
QImage selectionMaskBlendAiHair(const QImage& base, const QImage& aiMask,
                                int bandPx);
// Foreground decontamination for straight-alpha RGBA8888 `straight`: removes
// the backdrop colour that bleeds into the semi-transparent edge / hair-wisp
// band (0 < coverage < 255 from `coverage`). Industry approach
// (Photoshop/Affinity "decontaminate colours"): edge pixels take the colour
// of nearby confident foreground instead of unmixing the background out —
// unmixing is ill-conditioned at low alpha and wrong wherever alpha itself is
// overstated. Confident subject colour is propagated outward by weighted
// diffusion; each edge pixel blends toward it by `amount` (full blend through
// most of the band, fading to zero at the confident interior). `radius`
// sets the diffusion reach. Alpha is preserved; fully covered/empty pixels
// and the rest of the image are untouched. Returns false when sizes mismatch.
bool decontaminateStraightRgba(QImage& straight, const QImage& coverage,
                               int radius = 12, double amount = 0.85);

}  // namespace pittore::ui