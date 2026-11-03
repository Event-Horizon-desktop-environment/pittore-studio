// Document compositing: paint-stage caches, the gather walk, full and
// region rebuilds.
//
// Split out of app_state.cpp; everything here is a DocumentItem member (or
// its file-local helpers) so no extra declarations are needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"
#include "ui/live_filter.h"
#include "ui/mask_finish.h"

#include "engine/compute/adjust.h"
#include "engine/compute/backend.h"
#include "engine/compute/blend.h"
#include "engine/compute/brushes/dab/dab.h"
#include "engine/compute/dither.h"
#include "engine/compute/factory.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/tone_blend.h"
#include "engine/core/log.h"
#include "engine/core/parallel.h"

#include <QImage>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace pittore::ui {

namespace {
// sRGB (QColor) → linear-light RGBAf, matching how layer pixels are stored
// (straight-alpha linear). The board fill colour is sRGB; the compositor works
// in linear, so convert once at gather time. The QColor's own alpha rides
// through; the separate fill opacity folds in later as `fold`.
pittore::RGBAf boardFillLinear(const QColor& c) {
    const auto lin = [](int v) {
        const float s = static_cast<float>(v) / 255.0f;
        return s <= 0.04045f ? s / 12.92f
                             : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    return pittore::RGBAf{lin(c.red()), lin(c.green()), lin(c.blue()),
                          c.alphaF()};
}
}  // namespace

// Persistent per-document staging: the engine accumulator, allocated once per
// document size, plus the device-side source caches below. A brush stroke
// never reallocates per dab; renderRegion reuses the accumulator for
// incremental recomposites.
//
// `sources` is the device-side cache of placed layers' NATIVE pixels. GPU
// compositing samples moved/scaled layers straight from these (fused
// composite_many_into) instead of re-uploading the photo and re-sampling on
// the host every frame of a move/resize drag. Entries are keyed by the
// layer's pixels serial + sourceStamp + byte count so a brush dab or a
// reallocated Image is re-uploaded before it is sampled.
struct DocumentItem::PaintStage {
    struct PlacedSource {
        const void* key = nullptr;   // Image serial (see imageKey) or null
        std::uint64_t stamp = 0;     // LayerItem::sourceStamp at cache time
        std::uint64_t rev = 0;       // LayerItem::styledRev at cache time
        std::size_t bytes = 0;
        std::unique_ptr<pittore::compute::Buffer> dev;
    };
    std::unique_ptr<pittore::compute::Buffer> acc;
    // Live Tone Blend Group scratch pool: per-nesting-depth (group, backdrop)
    // buffer pairs. Recursion depth indexes it, so nested groups never share
    // temporaries. Grown lazily inside the rebuild; persists across rebuilds.
    std::vector<std::unique_ptr<pittore::compute::Buffer>> toneScratch;
    // Keyed by Image serial (see imageKey) so the per-layer device-texture
    // lookup is O(1) instead of a linear scan over 4k cached layers. Each
    // full-canvas recomposite was doing ~16.8M pointer compares (~4 ms)
    // hunting for its own layers — the dominant cost of a background/large-
    // layer toggle.
    std::unordered_map<const void*, PlacedSource> sources;
    // Parallel cache for opaque-grey mask coverage, keyed by the mask Image
    // serial plus LayerItem::maskStamp. Pruned alongside `sources`.
    std::unordered_map<const void*, PlacedSource> masks;
    // Finished (density/feather-baked) mask images, keyed by the raw mask
    // pointer. The device upload in `masks` above always copies from here
    // when present, so the backends sample finished coverage untouched.
    struct FinishedMask {
        std::shared_ptr<pittore::Image> img;
        std::uint64_t stamp = 0;
        float density = 1.0f;
        float feather = 0.0f;
    };
    std::unordered_map<const void*, FinishedMask> finishedMasks;
    // Parallel cache for Curves LUT uploads (under 3 KB), keyed by
    // l.adjustmentLUT.data() plus LayerItem::adjustStamp; the hit also
    // verifies content, small as the table is.
    std::unordered_map<const void*, PlacedSource> auxs;
};

pittore::compute::ComputeBackend& cpuBackend() {
    static std::unique_ptr<pittore::compute::ComputeBackend> backend =
        pittore::compute::make_backend(pittore::compute::BackendType::CPU);
    return *backend;
}

// Cache identity for an Image: its allocation serial, cast into the map key.
// The raw data address cannot serve as identity — a freed image's heap block
// can be handed to a later allocation of the same size, so a surviving stale
// slot would see the same address (and often the same stamp and byte count)
// while holding the previous image's bytes. Serials never repeat while a
// stale entry survives the prune.
const void* imageKey(const std::shared_ptr<pittore::Image>& img) {
    if (!img) return nullptr;
    return reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(img->serial()));
}

std::size_t layerBytes(const QSize& size) {
    return static_cast<std::size_t>(size.width()) * size.height() * sizeof(pittore::RGBAf);
}

// Halo-grown staging bounds of a layer for culling: identity layers are exact
// (a memcpy blit cannot bleed), while a resampled layer can colour document
// pixels up to one source texel past its bounds (the sampler footprint). This
// trades a tiny halo of rework for correctness when the toggled/edited layer
// is scaled (the sampler reaches one texel past the rect).
LayerDrawSource layerDrawSource(const LayerItem& l) {
    ensureLayerFilter(l);
    ensureLayerStyle(l);
    LayerDrawSource s;
    if (l.styled) {
        // The styled raster may be a capped bake (see ensureLayerStyle): scale
        // it back up to the layer's full document footprint so effects keep
        // their exact document size while the CPU/VRAM cost stays bounded.
        s.img = l.styled;
        s.offset = l.styledOffset;
        const double r = l.styledResample;
        s.scaleX = r > 0.0 ? 1.0 / r : 1.0;
        s.scaleY = r > 0.0 ? 1.0 / r : 1.0;
    } else {
        // Unstyled: composite the filtered render when a live filter
        // applies, else the native pixels (same placement either way —
        // the cache is layer-sized by construction).
        if (l.filtered && l.filteredValid)
            s.img = l.filtered;
        else
            s.img = l.pixels;
        s.offset = l.offset;
        s.scaleX = l.scaleX;
        s.scaleY = l.scaleY;
    }
    return s;
}

int topPixelLayerAt(const DocumentItem& d, const QPointF& docPos) {
    for (int i = 0; i < d.layers.size(); ++i) {
        const LayerItem& l = d.layers[i];
        if (!l.visible || l.kind != LayerItem::Kind::Pixel || !l.pixels)
            continue;
        if (layerBounds(d, l).contains(docPos)) return i;
    }
    return -1;
}

QColor sampleCompositeColor(const DocumentItem& d, const QPointF& docPos,
                            int sampleSize) {
    if (d.composite.isNull() || d.size.isEmpty()) return QColor();
    static const int kSizes[] = {1, 3, 5, 11, 31, 51, 101};
    const int n = kSizes[std::clamp(sampleSize, 0, 6)];
    const int cx = static_cast<int>(std::floor(docPos.x()));
    const int cy = static_cast<int>(std::floor(docPos.y()));
    if (!d.composite.rect().contains(QPoint(cx, cy))) return QColor();

    QImage img = d.composite;
    if (img.format() != QImage::Format_ARGB32_Premultiplied &&
        img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);

    const int half = n / 2;
    double sr = 0, sg = 0, sb = 0, sa = 0;
    int count = 0;
    for (int y = cy - half; y <= cy + half; ++y) {
        if (y < 0 || y >= img.height()) continue;
        for (int x = cx - half; x <= cx + half; ++x) {
            if (x < 0 || x >= img.width()) continue;
            const QRgb p = img.pixel(x, y);
            const int a = qAlpha(p);
            // Unpremultiply per sample so the average is of true colours, not
            // of premultiplied values (which would darken soft edges).
            if (a > 0) {
                sr += double(qRed(p)) * 255.0 / a;
                sg += double(qGreen(p)) * 255.0 / a;
                sb += double(qBlue(p)) * 255.0 / a;
            }
            sa += a;
            ++count;
        }
    }
    if (count == 0) return QColor();
    const int outA = qBound(0, int(std::lround(sa / count)), 255);
    return QColor(qBound(0, int(std::lround(sr / count)), 255),
                  qBound(0, int(std::lround(sg / count)), 255),
                  qBound(0, int(std::lround(sb / count)), 255), outA);
}

void DocumentItem::gatherCompositedLayers(const QRect& region,
                                          std::vector<CompositedLayer>& out) {
    out.clear();
    if (region.isEmpty() || size.isEmpty()) return;
    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(size.width(), region.left() + region.width());
    const int y1 = qMin(size.height(), region.top() + region.height());
    if (x0 >= x1 || y0 >= y1) return;
    out.reserve(static_cast<std::size_t>(layers.size()));

    // Linear visibility + parent-group sweep shared by every row below.
    // Per-index effectivelyVisible()/parentGroupIndex() scan back O(n) rows
    // per call, turning this loop quadratic on multi-thousand-layer docs.
    QVector<char> effVis;
    QVector<int> effParent;
    effectiveVisibility(effVis, effParent);

    // Board clip (D1): resolve each row's owning board and this region's board
    // rects once, so the per-row narrowing below is an O(1) lookup rather than
    // a per-row ancestor scan (which would go O(n²); see §16.2).
    QVector<std::uint64_t> boardOwn;
    boardOwnership(boardOwn);
    const std::unordered_map<std::uint64_t, QRectF> boardRects =
        boardClipRects(*this, region);

    // Board fill (D1): a board paints a solid colour behind its children. The
    // fill is emitted as its own layer at the board's bottom-most subtree row
    // (see the trigger in the loop below), so it composites below every child
    // but above whatever sits below the board. It carries no source, mask or
    // clip: just a constant colour over the board rect, with the board's fill
    // opacity folded into alpha.
    std::unordered_set<std::uint64_t> fillDone;
    auto emitFill = [&](std::uint64_t bid) {
        const int br = boardRowIndex(bid);
        if (br < 0 || !effVis[br]) return;
        const LayerItem& bl = layers[br];
        if (!bl.boardHasFill) return;
        const QRect brect = boardPaintRect(boardRects, bid);
        if (brect.isEmpty()) return;
        CompositedLayer e;
        e.layerIndex = br;
        e.isFill = true;
        e.fillColor = boardFillLinear(bl.boardFill);
        e.fold = static_cast<float>(bl.boardFillOpacity);
        e.blendMode = QStringLiteral("Normal");
        e.window = brect;
        out.push_back(std::move(e));
    };

    // Standard clipping groups: a clipped layer uses the nearest
    // included non-clipped pixel layer in the same parent scope as its base.
    // A base that exists but contributes nothing here (hidden or outside the
    // region) leaves the clipped layer invisible; only a layer with no base
    // anywhere acts as its own base.
    int baseOutPos = -1;
    int baseParent = -2;
    bool baseExists = false;
    for (int i = layers.size() - 1; i >= 0; --i) {
        LayerItem& l = layers[i];
        const int parent = effParent[i];
        // Board fill (D1): emit each board's fill once, at its bottom-most
        // subtree row, so it lands below every child but above whatever sits
        // below the board. boardOwn is self-owned on the header and inherited
        // by every descendant, so the first downward hit is that bottom row.
        if (boardOwn[i] != 0 && !fillDone.contains(boardOwn[i])) {
            fillDone.insert(boardOwn[i]);
            emitFill(boardOwn[i]);
        }
        const bool isAdjustment =
            l.kind == LayerItem::Kind::Adjustment && l.adjustmentKind != 0;
        if (l.kind != LayerItem::Kind::Pixel && !isAdjustment) {
            if (l.kind != LayerItem::Kind::Group &&
                effVis[i] && parent == baseParent) {
                baseOutPos = -1;
                baseExists = false;
            }
            continue;
        }
        if (!effVis[i]) {
            // A hidden pixel base still blocks the chain (its clipped
            // followers stay invisible); a hidden adjustment is simply absent.
            if (!isAdjustment && parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        // Board clip (D1): narrow this row's paint bounds to its owning
        // board's rect (already intersected with the region). A row entirely
        // outside its board takes the same skip path as a hidden one, so an
        // alpha-clip base inside the board still blocks its followers.
        int ix0 = x0, iy0 = y0, ix1 = x1, iy1 = y1;
        if (boardOwn[i] != 0) {
            const QRect br = boardPaintRect(boardRects, boardOwn[i]);
            if (br.isEmpty()) {
                ix0 = ix1;
                iy0 = iy1;  // board misses the region: nothing paints here
            } else {
                ix0 = std::max(x0, br.left());
                iy0 = std::max(y0, br.top());
                ix1 = std::min(x1, br.right() + 1);
                iy1 = std::min(y1, br.bottom() + 1);
            }
        }
        if (ix0 >= ix1 || iy0 >= iy1) {
            if (!isAdjustment && parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        if (isAdjustment) {
            // Live adjustments transform the composite-so-far across the
            // whole region. They never establish or break clip coverage, but
            // a clipped adjustment still resolves to the pixel base below it.
            bool clipped = false;
            int clipBase = -1;
            if (l.clipped) {
                if (baseOutPos >= 0 && baseParent == parent) {
                    clipped = true;
                    clipBase = baseOutPos;
                } else if (baseExists && baseParent == parent) {
                    clipped = true;
                    clipBase = -1;
                }
            }
            const pittore::Image* mask = effectiveMaskImage(size, l);
            QPointF maskOffset{0, 0};
            double maskScaleX = 1.0, maskScaleY = 1.0;
            if (mask) maskTransformFor(l, maskOffset, maskScaleX, maskScaleY);
            CompositedLayer e;
            e.layerIndex = i;
            e.mask = mask;
            e.maskOffset = maskOffset;
            e.maskScaleX = maskScaleX;
            e.maskScaleY = maskScaleY;
            e.clipped = clipped;
            e.clipBase = clipBase;
            e.isAdjustment = true;
            e.adjKind = l.adjustmentKind;
            for (int k = 0; k < 16; ++k)
                e.adjP[k] = l.adjustmentParams[k];
            e.adjAux = l.adjustmentLUT.size() == adjustmentAuxSize(l) &&
                             adjustmentAuxSize(l) != 0
                         ? l.adjustmentLUT.data()
                         : nullptr;
            e.fold = (l.opacity / 100.0f) * (l.fill / 100.0f);
            e.blendMode = l.blendMode;
            e.window = QRect(ix0, iy0, ix1 - ix0, iy1 - iy0);
            out.push_back(std::move(e));
            continue;
        }
        ensureLayerPixels(*this, l);
        if (!l.pixels || !effVis[i]) {
            if (parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        std::uint32_t rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
        if (!gpuCompositeWindow(*this, l, static_cast<std::uint32_t>(ix0),
                                static_cast<std::uint32_t>(iy0),
                                static_cast<std::uint32_t>(ix1),
                                static_cast<std::uint32_t>(iy1), &rx0, &ry0,
                                &rx1, &ry1)) {
            if (parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        const LayerDrawSource src = layerDrawSource(l);
        if (!src.img) {
            if (parent == baseParent) {
                baseOutPos = -1;
                baseExists = true;
            }
            continue;
        }
        bool clipped = false;
        int clipBase = -1;
        if (l.clipped) {
            if (baseOutPos >= 0 && baseParent == parent) {
                clipped = true;
                clipBase = baseOutPos;
            } else if (baseExists && baseParent == parent) {
                clipped = true;
                clipBase = -1;
            } else {
                baseOutPos = static_cast<int>(out.size());
                baseParent = parent;
                baseExists = true;
            }
        } else {
            baseOutPos = static_cast<int>(out.size());
            baseParent = parent;
            baseExists = true;
        }
        const pittore::Image* mask = effectiveMaskImage(size, l);
        QPointF maskOffset{0, 0};
        double maskScaleX = 1.0, maskScaleY = 1.0;
        if (mask) maskTransformFor(l, maskOffset, maskScaleX, maskScaleY);
        CompositedLayer e;
        e.layerIndex = i;
        e.source = src;
        e.mask = mask;
        e.maskOffset = maskOffset;
        e.maskScaleX = maskScaleX;
        e.maskScaleY = maskScaleY;
        e.clipped = clipped;
        e.clipBase = clipBase;
        e.fold = (l.opacity / 100.0f) * (l.fill / 100.0f);
        e.blendMode = l.blendMode;
        e.window = QRect(static_cast<int>(rx0), static_cast<int>(ry0),
                         static_cast<int>(rx1 - rx0),
                         static_cast<int>(ry1 - ry0));
        out.push_back(std::move(e));
    }
}

void DocumentItem::rebuildComposite() {
    if (size.isEmpty()) return;
    ++revision;   // any composite changes the canvas's derived-geometry caches
    // --- Live Tone Blend Group scaffolding (shared by both backends) ------
    // A tone-blend header + its indent subtree form a span. Spans nest;
    // callers recurse so inner groups blend against the outer group's
    // work-in-progress (the correct nesting semantic). Headers that are
    // effectively invisible blend nothing (their children are hidden too).
    struct ToneSpan {
        int header = -1;
        int end = -1;
    };
    auto toneTokenEnd = [&](int start) {
        if (start < 0 || start >= layers.size()) return start;
        const int base = layers[start].indent;
        int end = start;
        while (end + 1 < layers.size() && layers[end + 1].indent > base) ++end;
        return end;
    };
    auto toneSpansIn = [&](int top, int bottom) {
        std::vector<ToneSpan> spans;
        for (int i = top; i <= bottom && i < layers.size();) {
            const LayerItem& l = layers[i];
            if (l.kind == LayerItem::Kind::Group && l.toneBlendGroup &&
                effectivelyVisible(i)) {
                const int end = toneTokenEnd(i);
                spans.push_back(ToneSpan{i, end});
                i = end + 1;
                continue;
            }
            ++i;
        }
        return spans;
    };
    auto hasVisibleToneBlendGroup = [&]() {
        bool any = false;
        for (const LayerItem& l : layers) {
            if (l.kind == LayerItem::Kind::Group && l.toneBlendGroup) {
                any = true;
                break;
            }
        }
        if (!any) return false;
        QVector<char> tv;
        QVector<int> tp;
        effectiveVisibility(tv, tp);
        for (int i = 0; i < layers.size(); ++i)
            if (layers[i].kind == LayerItem::Kind::Group &&
                layers[i].toneBlendGroup && tv[i])
                return true;
        return false;
    };
    const bool toneActive = hasVisibleToneBlendGroup();
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint32_t w = static_cast<std::uint32_t>(size.width());
    const std::uint32_t h = static_cast<std::uint32_t>(size.height());

    // Reuse the canvas across rebuilds (33MB at 4K): only realloc when the
    // size/format actually changed. Both paths overwrite every pixel.
    if (composite.size() != size ||
        composite.format() != QImage::Format_ARGB32_Premultiplied)
        composite = QImage(size, QImage::Format_ARGB32_Premultiplied);

    // Pixel layers are composited by the engine, bottom→top, straight alpha.
    // The accumulator starts fully transparent; each layer's opacity × fill is
    // folded into its alpha before the blend. Text / Shape / Adjustment layers
    // remain QPainter stand-ins drawn above the flattened pixels.
    ensurePainter();
    const bool gpu =
        backend && backend->type() != pittore::compute::BackendType::CPU;

    // GPU path: the accumulator lives on the device end to end. Clear it there,
    // sample/fold/blend EVERY pixel layer with the fused kernel — the 1:1
    // background included, since the sampler reads an integer-aligned texel
    // exactly — then hand the finished frame to the canvas with one device
    // convert + DMA. No host resample, no work buffer, no per-layer transfer.
    // Device failures (OOM included) fall through to the CPU reference
    // path below instead of terminating: a huge import on a small GPU
    // still opens.
    if (gpu && rebuildCompositeGPU(*backend, *stage_->acc, stage_->toneScratch,
                                   w, h, toneActive))
        return;

    // CPU reference path: host-authoritative accumulator. This uses the same
    // shared bottom→top row walk as the GPU batched kernel (masks, folds and
    // clip groups included), sampled straight from the live layer Images.
    composite.fill(Qt::transparent);
    auto* acc = static_cast<pittore::RGBAf*>(stage_->acc->host());
    std::fill_n(acc, static_cast<std::size_t>(w) * h, pittore::RGBAf{});
    const auto tfill = std::chrono::steady_clock::now();

    std::vector<CompositedLayer> gathered;
    gatherCompositedLayers(QRect(QPoint(0, 0), size), gathered);
    // Gathered-index slice for a panel range (gather preserves panel
    // order, so the slice is contiguous).
    auto gatheredRangeForPanels = [&](int plo, int phi) {
        std::size_t a = gathered.size(), b = 0;
        for (std::size_t j = 0; j < gathered.size(); ++j) {
            const int li = gathered[j].layerIndex;
            if (li >= plo && li <= phi) {
                a = std::min(a, j);
                b = std::max(b, j + 1);
            }
        }
        if (b <= a) return std::pair<std::size_t, std::size_t>{0, 0};
        return std::pair<std::size_t, std::size_t>{a, b};
    };
    // Slice converter with clipBase remapped to slice-relative positions
    // (out-of-slice bases degrade to unclipped, mirroring
    // composite_many_host's own validation).
    auto sliceViews = [&](std::size_t glo, std::size_t ghi) {
        std::vector<pittore::compute::HostPlacedLayer> views;
        if (ghi > glo) views.reserve(ghi - glo);
        for (std::size_t j = glo; j < ghi && j < gathered.size(); ++j) {
            const CompositedLayer& e = gathered[j];
            pittore::compute::HostPlacedLayer v;
            if (e.isFill) {
                v.isFill = true;
                v.fillColor = e.fillColor;
            } else if (e.isAdjustment) {
                v.isAdjustment = true;
                v.adjKind = e.adjKind;
                for (int k = 0; k < 16; ++k) v.adjP[k] = e.adjP[k];
                v.adjAux = e.adjAux;
            } else {
                v.src = e.source.img->data();
                v.sw = e.source.img->width();
                v.sh = e.source.img->height();
                v.ox = e.source.offset.x();
                v.oy = e.source.offset.y();
                v.sx = e.source.scaleX;
                v.sy = e.source.scaleY;
            }
            // Finished coverage when density/feather apply, so the CPU
            // reference matches the device path byte-for-byte.
            const pittore::Image* fin =
                finishedMaskFor(layers[e.layerIndex]);
            const pittore::Image* mv = fin ? fin : e.mask;
            v.mask = mv ? mv->data() : nullptr;
            v.msw = mv ? mv->width() : 0;
            v.msh = mv ? mv->height() : 0;
            v.mox = e.maskOffset.x();
            v.moy = e.maskOffset.y();
            v.msx = e.maskScaleX;
            v.msy = e.maskScaleY;
            v.x0 = static_cast<std::uint32_t>(e.window.left());
            v.y0 = static_cast<std::uint32_t>(e.window.top());
            v.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
            v.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
            v.fold = e.fold;
            v.mode = engineBlendMode(e.blendMode);
            v.clipped = e.clipped;
            v.clipBase = e.clipBase;
            if (v.clipped && v.clipBase >= 0) {
                if (static_cast<std::size_t>(v.clipBase) >= glo &&
                    static_cast<std::size_t>(v.clipBase) < ghi)
                    v.clipBase -= static_cast<int>(glo);
                else {
                    v.clipped = false;
                    v.clipBase = -1;
                }
            }
            views.push_back(v);
        }
        return views;
    };
    if (!toneActive) {
        auto views = sliceViews(0, gathered.size());
        if (!views.empty())
            pittore::compute::composite_many_host(acc, w, h, 0, 0, w, h,
                                                   views.data(), views.size());
    } else {
        // Segmented composite mirroring the GPU path: tone-blend spans
        // composite their children into local temps (nesting-safe: every
        // recursion level owns its vectors), blend against a snapshot of
        // the accumulator below, and re-enter as one synthetic layer.
        const std::size_t n = static_cast<std::size_t>(w) * h;
        // Depth-indexed scratch pair per recursion level (mirrors the GPU
        // toneScratch): grown on first use, reused by sibling groups, never
        // freed in steady state. bdrop is fully overwritten by the acc copy;
        // grp is zeroed before each children composite (the walk blends over
        // it, so stale bytes would leak through).
        std::vector<std::vector<pittore::RGBAf>> toneHost;
        auto toneBuf = [&](std::size_t idx) -> pittore::RGBAf* {
            if (toneHost.size() <= idx) toneHost.resize(idx + 1);
            auto& v = toneHost[idx];
            if (v.size() != n) v.assign(n, pittore::RGBAf{});
            return v.data();
        };
        std::function<void(pittore::RGBAf*, int, int, int)> compCPU =
            [&](pittore::RGBAf* dst, int top, int bottom, int depth) {
                int g = -1, gend = -1;
                for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                    const auto cr = gatheredRangeForPanels(s.header + 1, s.end);
                    if (cr.second <= cr.first) continue;
                    if (s.header > g) {
                        g = s.header;
                        gend = s.end;
                    }
                }
                if (g < 0) {
                    const auto r = gatheredRangeForPanels(top, bottom);
                    if (r.second <= r.first) return;
                    auto slice = sliceViews(r.first, r.second);
                    if (!slice.empty())
                        pittore::compute::composite_many_host(
                            dst, w, h, 0, 0, w, h, slice.data(), slice.size());
                    return;
                }
                compCPU(dst, gend + 1, bottom, depth);  // everything below
                pittore::RGBAf* bdrop = toneBuf(std::size_t(2 * depth));
                std::memcpy(bdrop, acc, n * sizeof(pittore::RGBAf));  // backdrop
                pittore::RGBAf* grp = toneBuf(std::size_t(2 * depth + 1));
                std::fill_n(grp, n, pittore::RGBAf{});  // children
                compCPU(grp, g + 1, gend, depth + 1);
                const LayerItem& header = layers[g];
                pittore::compute::applyToneBlend(
                    grp, bdrop, grp, w, h,
                    header.toneBlend);
                pittore::compute::HostPlacedLayer synth;
                synth.src = grp;
                synth.sw = w;
                synth.sh = h;
                synth.ox = 0.0;
                synth.oy = 0.0;
                synth.sx = 1.0;
                synth.sy = 1.0;
                synth.x0 = 0;
                synth.y0 = 0;
                synth.x1 = w;
                synth.y1 = h;
                synth.fold = (header.opacity / 100.0f) *
                             (header.fill / 100.0f);
                synth.mode = engineBlendMode(header.blendMode);
                pittore::compute::composite_many_host(dst, w, h, 0, 0, w, h,
                                                       &synth, 1);
                compCPU(dst, top, g - 1, depth);  // everything above
            };
        compCPU(acc, 0, layers.size() - 1, 0);
    }
    const auto tcomp = std::chrono::steady_clock::now();

    blitRGBAfToPremul(acc, composite.bits(), 0, 0, w,
                        static_cast<std::uint32_t>(size.height()));
    const auto tblit = std::chrono::steady_clock::now();

    // (overlay QPainter pass shared with renderRegion)
    drawVectorOverlays();
    const auto tdone = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(tdone - t0)
                          .count();
    PITTORE_LOG("[render][rebuild] w=%u h=%u layers=%d backend=%s ms=%.2f", w, h,
                 static_cast<int>(layers.size()),
                 backend ? backend->name().c_str() : "cpu", ms);
    if (pittore::core::log::strokeTrace() ||
        ms >= pittore::core::log::slowEventMs())
        PITTORE_LOG(
            "[render][rebuild-breakdown] fill=%.3f composite=%.3f "
            "blit=%.3f overlay=%.3f stack=%s",
            std::chrono::duration<double, std::milli>(tfill - t0).count(),
            std::chrono::duration<double, std::milli>(tcomp - tfill).count(),
            std::chrono::duration<double, std::milli>(tblit - tcomp).count(),
            std::chrono::duration<double, std::milli>(tdone - tblit).count(),
            describeStack(gathered).toLocal8Bit().constData());
}

// Re-composite only the changed rect through the same shared bottom→top
// stack walk the full rebuild uses (masks, folds and clip groups included),
// then blit just those rows into the composite QImage. Overlay
// (Text/Shape/etc.) layers are redrawn over the region too, so the result is
// pixel-identical to a full rebuild wherever anything repainted.
void DocumentItem::renderRegion(const QRect& region) {
    if (region.isEmpty() || size.isEmpty()) return;
    if (composite.isNull()) {            // nothing rendered yet → full rebuild
        rebuildComposite();
        return;
    }
    // Live Tone Blend Groups read the backdrop through a blur with finite
    // footprint: expand the dirty rect by a halo and run a region-bounded
    // tone blend. Falls back to full rebuild only for huge regions.
    bool toneVisible = false;
    for (int i = 0; i < layers.size(); ++i) {
        if (layers[i].kind == LayerItem::Kind::Group &&
            layers[i].toneBlendGroup && effectivelyVisible(i)) {
            toneVisible = true;
            break;
        }
    }
    if (toneVisible) {
        renderRegionTone(region);
        return;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(size.width());
    const std::uint32_t h = static_cast<std::uint32_t>(size.height());
    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(static_cast<int>(w), region.left() + region.width());
    const int y1 = qMin(static_cast<int>(h), region.top() + region.height());
    if (x0 >= x1 || y0 >= y1) return;
    ++revision;   // a region edit changes the pixels the canvas draws
    const auto t0 = std::chrono::steady_clock::now();

    ensurePainter();
    const bool gpu =
        backend && backend->type() != pittore::compute::BackendType::CPU;

    // GPU path — the per-frame move/resize drag. The accumulator stays on the
    // device: clear it there, then sample + fold + blend every layer that
    // touches the dirty rect (the 1:1 background included) with the fused
    // kernel, and hand just the dirty rows to the canvas with a device convert
    // + DMA. The host never resamples a photo and never stages the work buffer.
    // Same guard as the full rebuild: device failure falls through to
    // the CPU path below instead of terminating.
    if (gpu && renderRegionGPU(*backend, *stage_->acc, x0, y0, x1, y1, w, h,
                                region))
        return;

    // CPU reference path: host-authoritative accumulator. Like the full
    // rebuild, this walks the same shared bottom→top stack instead of
    // staging one layer at a time.
    auto* acc = static_cast<pittore::RGBAf*>(stage_->acc->host());

    // Clear only the region of the accumulator.
    for (int y = y0; y < y1; ++y) {
        pittore::RGBAf* row = acc + static_cast<std::size_t>(y) * w + x0;
        std::fill(row, row + static_cast<std::size_t>(x1 - x0), pittore::RGBAf{});
    }
    const auto tclear = std::chrono::steady_clock::now();

    std::vector<CompositedLayer> gathered;
    gatherCompositedLayers(QRect(x0, y0, x1 - x0, y1 - y0), gathered);
    std::vector<pittore::compute::HostPlacedLayer> views;
    views.reserve(gathered.size());
    for (const CompositedLayer& e : gathered) {
        pittore::compute::HostPlacedLayer v;
        if (e.isFill) {
            v.isFill = true;
            v.fillColor = e.fillColor;
        } else if (e.isAdjustment) {
            v.isAdjustment = true;
            v.adjKind = e.adjKind;
            for (int k = 0; k < 16; ++k) v.adjP[k] = e.adjP[k];
            v.adjAux = e.adjAux;
        } else {
            v.src = e.source.img->data();
            v.sw = e.source.img->width();
            v.sh = e.source.img->height();
            v.ox = e.source.offset.x();
            v.oy = e.source.offset.y();
            v.sx = e.source.scaleX;
            v.sy = e.source.scaleY;
        }
        // Finished coverage when density/feather apply, so the CPU
        // reference matches the device path byte-for-byte.
        const pittore::Image* fin = finishedMaskFor(layers[e.layerIndex]);
        const pittore::Image* mv = fin ? fin : e.mask;
        v.mask = mv ? mv->data() : nullptr;
        v.msw = mv ? mv->width() : 0;
        v.msh = mv ? mv->height() : 0;
        v.mox = e.maskOffset.x();
        v.moy = e.maskOffset.y();
        v.msx = e.maskScaleX;
        v.msy = e.maskScaleY;
        v.x0 = static_cast<std::uint32_t>(e.window.left());
        v.y0 = static_cast<std::uint32_t>(e.window.top());
        v.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
        v.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
        v.fold = e.fold;
        v.mode = engineBlendMode(e.blendMode);
        v.clipped = e.clipped;
        v.clipBase = e.clipBase;
        views.push_back(v);
    }
    pittore::compute::composite_many_host(
        acc, w, static_cast<std::uint32_t>(size.height()),
        static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
        static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(y1),
        views.data(), views.size());
    const auto tcomp = std::chrono::steady_clock::now();

    // Blit only the updated rows.
    for (int y = y0; y < y1; ++y) {
        uchar* dst = composite.bits() +
                     static_cast<qsizetype>(y) * composite.bytesPerLine() +
                     static_cast<qsizetype>(x0) * 4;
        blitRGBAfToPremul(acc + static_cast<std::size_t>(y) * w + x0, dst,
                          static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y),
                          static_cast<std::uint32_t>(x1 - x0), 1);
    }
    const auto tblit = std::chrono::steady_clock::now();

    drawVectorOverlays();
    const auto tdone = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(tdone - t0)
                          .count();
    if (pittore::core::log::strokeTrace() ||
        ms >= pittore::core::log::slowEventMs())
        PITTORE_LOG(
            "[render][region] incremental backend=%s region=(%d,%d,%d,%d) "
            "layers=%d clear=%.2f composite=%.2f blit=%.2f overlay=%.2f ms=%.2f",
            backend ? backend->name().c_str() : "cpu", region.x(), region.y(),
            region.width(), region.height(),
            static_cast<int>(layers.size()),
            std::chrono::duration<double, std::milli>(tclear - t0).count(),
            std::chrono::duration<double, std::milli>(tcomp - tclear).count(),
            std::chrono::duration<double, std::milli>(tblit - tcomp).count(),
            std::chrono::duration<double, std::milli>(tdone - tblit).count(),
            ms);
}

// Region-bounded tone composite for incremental repaints (erase/paint dabs
// inside a tone-blend group). Expands the dirty rect by a halo covering the
// tone pyramid blur footprint, composites below-slice + children over just
// that expanded region, runs tone_blend_region, and blits only the inner
// dirty rect. Falls back to full rebuild when the expanded region would
// cover most of the frame anyway.
void DocumentItem::renderRegionTone(const QRect& region) {
    if (region.isEmpty() || size.isEmpty()) return;
    if (composite.isNull()) {
        rebuildComposite();
        return;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(size.width());
    const std::uint32_t h = static_cast<std::uint32_t>(size.height());
    const int x0 = qMax(0, region.left());
    const int y0 = qMax(0, region.top());
    const int x1 = qMin(static_cast<int>(w), region.left() + region.width());
    const int y1 = qMin(static_cast<int>(h), region.top() + region.height());
    if (x0 >= x1 || y0 >= y1) return;
    ++revision;
    const auto t0 = std::chrono::steady_clock::now();

    ensurePainter();
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    const bool gpu =
        backend && backend->type() != pittore::compute::BackendType::CPU;

    // Halo must cover the tone pyramid blur footprint at full resolution.
    // Must match the halo in tone_blend_region (32px).
    constexpr int kToneHalo = 32;
    const int hx0 = qMax(0, x0 - kToneHalo);
    const int hy0 = qMax(0, y0 - kToneHalo);
    const int hx1 = qMin(static_cast<int>(w), x1 + kToneHalo);
    const int hy1 = qMin(static_cast<int>(h), y1 + kToneHalo);

    // If expanded region covers most of frame, full rebuild is simpler.
    const std::uint64_t expArea =
        static_cast<std::uint64_t>(hx1 - hx0) * (hy1 - hy0);
    const std::uint64_t frameArea = static_cast<std::uint64_t>(w) * h;
    if (expArea * 4 >= frameArea * 3) {
        rebuildComposite();
        return;
    }

    // Tone scaffolding (mirrors rebuildComposite).
    struct ToneSpan {
        int header = -1;
        int end = -1;
    };
    auto toneTokenEnd = [&](int start) {
        if (start < 0 || start >= layers.size()) return start;
        const int base = layers[start].indent;
        int end = start;
        while (end + 1 < layers.size() && layers[end + 1].indent > base) ++end;
        return end;
    };
    auto toneSpansIn = [&](int top, int bottom) {
        std::vector<ToneSpan> spans;
        for (int i = top; i <= bottom && i < layers.size();) {
            const LayerItem& l = layers[i];
            if (l.kind == LayerItem::Kind::Group && l.toneBlendGroup &&
                effectivelyVisible(i)) {
                const int end = toneTokenEnd(i);
                spans.push_back(ToneSpan{i, end});
                i = end + 1;
                continue;
            }
            ++i;
        }
        return spans;
    };

    if (gpu) {
        const auto tprep0 = std::chrono::steady_clock::now();
        // Clear the accumulator like the non-tone region path does. Only the
        // dirty region is blitted back to canvas, so pixels outside keep
        // their existing canvas content. Starting transparent is required
        // because composite_many_into blends over existing device bytes.
        be.clear(*stage_->acc);
        std::vector<CompositedLayer> gathered;
        gatherCompositedLayers(QRect(hx0, hy0, hx1 - hx0, hy1 - hy0), gathered);
        std::vector<pittore::compute::PlacedLayer> placed;
        placed.reserve(gathered.size());
        for (const CompositedLayer& e : gathered) {
            const LayerItem& l = layers[e.layerIndex];
            pittore::compute::PlacedLayer p;
            if (e.isFill) {
                p.isFill = true;
                p.fillColor = e.fillColor;
            } else if (e.isAdjustment) {
                p.isAdjustment = true;
                p.adjKind = e.adjKind;
                for (int k = 0; k < 16; ++k) p.adjP[k] = e.adjP[k];
                p.adjAux = adjAuxSourceFor(l);
            } else {
                p.src = &placedSourceFor(l);
                p.sw = e.source.img->width();
                p.sh = e.source.img->height();
                p.ox = e.source.offset.x();
                p.oy = e.source.offset.y();
                p.sx = e.source.scaleX;
                p.sy = e.source.scaleY;
            }
            const pittore::compute::Buffer* maskBuf = maskSourceFor(l);
            p.mask = maskBuf;
            p.msw = e.mask ? static_cast<std::uint32_t>(e.mask->width()) : 0u;
            p.msh = e.mask ? static_cast<std::uint32_t>(e.mask->height()) : 0u;
            p.mox = e.maskOffset.x();
            p.moy = e.maskOffset.y();
            p.msx = e.maskScaleX;
            p.msy = e.maskScaleY;
            p.x0 = static_cast<std::uint32_t>(e.window.left());
            p.y0 = static_cast<std::uint32_t>(e.window.top());
            p.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
            p.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
            p.fold = e.fold;
            p.mode = engineBlendMode(e.blendMode);
            p.clipped = e.clipped;
            p.clipBase = e.clipBase;
            placed.push_back(p);
        }
        auto remapPlaced = [&](std::size_t glo, std::size_t ghi) {
            std::vector<pittore::compute::PlacedLayer> slice;
            if (ghi > glo) slice.reserve(ghi - glo);
            for (std::size_t j = glo; j < ghi && j < placed.size(); ++j) {
                auto p = placed[j];
                if (p.clipped && p.clipBase >= 0) {
                    if (static_cast<std::size_t>(p.clipBase) >= glo &&
                        static_cast<std::size_t>(p.clipBase) < ghi)
                        p.clipBase -= static_cast<int>(glo);
                    else {
                        p.clipped = false;
                        p.clipBase = -1;
                    }
                }
                slice.push_back(p);
            }
            return slice;
        };
        auto placedRangeForPanels = [&](int plo, int phi) {
            std::size_t a = placed.size(), b = 0;
            for (std::size_t j = 0; j < gathered.size() && j < placed.size();
                 ++j) {
                const int li = gathered[j].layerIndex;
                if (li >= plo && li <= phi) {
                    a = std::min(a, j);
                    b = std::max(b, j + 1);
                }
            }
            if (b <= a) return std::pair<std::size_t, std::size_t>{0, 0};
            return std::pair<std::size_t, std::size_t>{a, b};
        };
        // Region-bounded recursive composite. Unlike rebuildComposite's
        // compGPU (full frame), every composite_many_into and tone_blend
        // call here is bounded to [ux0,ux1) x [uy0,uy1). Scratch buffers
        // are full-frame (reused from stage_) but only the region is
        // written; the final synth composite targets just the inner dirty
        // rect of stage_->acc, preserving everything outside it.
        const std::uint32_t ux0 = static_cast<std::uint32_t>(hx0);
        const std::uint32_t uy0 = static_cast<std::uint32_t>(hy0);
        const std::uint32_t ux1 = static_cast<std::uint32_t>(hx1);
        const std::uint32_t uy1 = static_cast<std::uint32_t>(hy1);
        std::function<void(pittore::compute::Buffer&, int, int, int)> compGPURegion =
            [&](pittore::compute::Buffer& acc, int top, int bottom,
                int depth) {
                int g = -1, gend = -1;
                for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                    const auto cr =
                        placedRangeForPanels(s.header + 1, s.end);
                    if (cr.second <= cr.first) continue;
                    if (s.header > g) {
                        g = s.header;
                        gend = s.end;
                    }
                }
                if (g < 0) {
                    const auto r = placedRangeForPanels(top, bottom);
                    if (r.second <= r.first) return;
                    auto slice = remapPlaced(r.first, r.second);
                    if (!slice.empty())
                        be.composite_many_into(acc, w, ux0, uy0, ux1, uy1,
                                               slice.data(), slice.size());
                    return;
                }
                compGPURegion(acc, gend + 1, bottom, depth);
                const std::size_t need = std::size_t(2 * (depth + 1));
                while (stage_->toneScratch.size() < need)
                    stage_->toneScratch.push_back(
                        be.make_buffer(static_cast<std::size_t>(w) * h *
                                       sizeof(pittore::RGBAf)));
                pittore::compute::Buffer& grp =
                    *stage_->toneScratch[std::size_t(2 * depth)];
                pittore::compute::Buffer& bdrop =
                    *stage_->toneScratch[std::size_t(2 * depth + 1)];
                // Backdrop = below-slice composited over the expanded
                // region into scratch. Clear the region first since the
                // walk blends over existing device bytes.
                be.clear(bdrop);
                {
                    const auto r = placedRangeForPanels(gend + 1, bottom);
                    if (r.second > r.first) {
                        auto slice = remapPlaced(r.first, r.second);
                        if (!slice.empty())
                            be.composite_many_into(bdrop, w, ux0, uy0, ux1, uy1,
                                                   slice.data(), slice.size());
                    }
                }
                be.clear(grp);
                compGPURegion(grp, g + 1, gend, depth + 1);
                const LayerItem& header = layers[g];
                // Region tone transfer over the expanded rect (halo absorbs
                // pyramid edge effects); inner dirty rect is exact.
                be.tone_blend_region(grp, bdrop, w, h, ux0, uy0, ux1, uy1,
                                     header.toneBlend);
                pittore::compute::PlacedLayer synth;
                synth.src = &grp;
                synth.sw = w;
                synth.sh = h;
                synth.ox = 0.0;
                synth.oy = 0.0;
                synth.sx = 1.0;
                synth.sy = 1.0;
                synth.x0 = ux0;
                synth.y0 = uy0;
                synth.x1 = ux1;
                synth.y1 = uy1;
                synth.fold = (header.opacity / 100.0f) *
                             (header.fill / 100.0f);
                synth.mode = engineBlendMode(header.blendMode);
                // Composite the toned region back into the accumulator.
                // The region path preserves acc outside [x0,x1) x [y0,y1).
                be.composite_placed_into(acc, grp, w, h, 0.0, 0.0, 1.0, 1.0,
                                         w, ux0, uy0, ux1, uy1,
                                         synth.fold, synth.mode);
                compGPURegion(acc, top, g - 1, depth);
            };
        compGPURegion(*stage_->acc, 0, layers.size() - 1, 0);
        prunePlacedSources();
        // Blit just the inner dirty region (not halo) to canvas.
        uchar* dst = composite.bits() +
                     static_cast<qsizetype>(y0) * composite.bytesPerLine() +
                     static_cast<qsizetype>(x0) * 4;
        const auto tprep1 = std::chrono::steady_clock::now();
        be.blit_premul(*stage_->acc, dst, w, static_cast<std::uint32_t>(x0),
                       static_cast<std::uint32_t>(y0),
                       static_cast<std::uint32_t>(x1),
                       static_cast<std::uint32_t>(y1),
                       static_cast<std::size_t>(composite.bytesPerLine()));
        const auto tblit1 = std::chrono::steady_clock::now();
        drawVectorOverlays();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG(
                "[render][region-tone] backend=%s dirty=(%d,%d,%d,%d) "
                "layers=%d prep=%.2f blit=%.2f ms=%.2f",
                backend ? backend->name().c_str() : "cpu", x0, y0,
                x1 - x0, y1 - y0, static_cast<int>(layers.size()),
                std::chrono::duration<double, std::milli>(tprep1 - tprep0).count(),
                std::chrono::duration<double, std::milli>(tblit1 - tprep1).count(),
                ms);
        return;
    }

    // CPU reference path: region-bounded tone composite on the host
    // accumulator, mirroring compGPURegion above (same recursion, same
    // expanded-region contract) instead of a full rebuild. Scratch buffers
    // are full-frame but only the expanded region is written; only the inner
    // dirty rect is blitted back, so the canvas outside keeps its pixels.
    {
        const auto tprep0 = std::chrono::steady_clock::now();
        auto* acc = static_cast<pittore::RGBAf*>(stage_->acc->host());
        // Clear only the expanded region: the walk blends over existing
        // bytes, so the region must start transparent.
        for (int y = hy0; y < hy1; ++y)
            std::fill_n(acc + static_cast<std::size_t>(y) * w + hx0,
                        static_cast<std::size_t>(hx1 - hx0),
                        pittore::RGBAf{});
        std::vector<CompositedLayer> gathered;
        gatherCompositedLayers(QRect(hx0, hy0, hx1 - hx0, hy1 - hy0),
                               gathered);
        // Gathered-index slice for a panel range (gather preserves panel
        // order, so the slice is contiguous) — same as rebuildComposite.
        auto gatheredRangeForPanels = [&](int plo, int phi) {
            std::size_t a = gathered.size(), b = 0;
            for (std::size_t j = 0; j < gathered.size(); ++j) {
                const int li = gathered[j].layerIndex;
                if (li >= plo && li <= phi) {
                    a = std::min(a, j);
                    b = std::max(b, j + 1);
                }
            }
            if (b <= a) return std::pair<std::size_t, std::size_t>{0, 0};
            return std::pair<std::size_t, std::size_t>{a, b};
        };
        // Slice converter with clipBase remapped to slice-relative positions
        // — same as rebuildComposite (finished coverage when density/feather
        // apply, so the CPU reference matches the device path byte-for-byte).
        auto sliceViews = [&](std::size_t glo, std::size_t ghi) {
            std::vector<pittore::compute::HostPlacedLayer> views;
            if (ghi > glo) views.reserve(ghi - glo);
            for (std::size_t j = glo; j < ghi && j < gathered.size(); ++j) {
                const CompositedLayer& e = gathered[j];
                pittore::compute::HostPlacedLayer v;
                if (e.isFill) {
                    v.isFill = true;
                    v.fillColor = e.fillColor;
                } else if (e.isAdjustment) {
                    v.isAdjustment = true;
                    v.adjKind = e.adjKind;
                    for (int k = 0; k < 16; ++k) v.adjP[k] = e.adjP[k];
                    v.adjAux = e.adjAux;
                } else {
                    v.src = e.source.img->data();
                    v.sw = e.source.img->width();
                    v.sh = e.source.img->height();
                    v.ox = e.source.offset.x();
                    v.oy = e.source.offset.y();
                    v.sx = e.source.scaleX;
                    v.sy = e.source.scaleY;
                }
                const pittore::Image* fin =
                    finishedMaskFor(layers[e.layerIndex]);
                const pittore::Image* mv = fin ? fin : e.mask;
                v.mask = mv ? mv->data() : nullptr;
                v.msw = mv ? mv->width() : 0;
                v.msh = mv ? mv->height() : 0;
                v.mox = e.maskOffset.x();
                v.moy = e.maskOffset.y();
                v.msx = e.maskScaleX;
                v.msy = e.maskScaleY;
                v.x0 = static_cast<std::uint32_t>(e.window.left());
                v.y0 = static_cast<std::uint32_t>(e.window.top());
                v.x1 = static_cast<std::uint32_t>(e.window.right() + 1);
                v.y1 = static_cast<std::uint32_t>(e.window.bottom() + 1);
                v.fold = e.fold;
                v.mode = engineBlendMode(e.blendMode);
                v.clipped = e.clipped;
                v.clipBase = e.clipBase;
                if (v.clipped && v.clipBase >= 0) {
                    if (static_cast<std::size_t>(v.clipBase) >= glo &&
                        static_cast<std::size_t>(v.clipBase) < ghi)
                        v.clipBase -= static_cast<int>(glo);
                    else {
                        v.clipped = false;
                        v.clipBase = -1;
                    }
                }
                views.push_back(v);
            }
            return views;
        };
        const std::uint32_t ux0 = static_cast<std::uint32_t>(hx0);
        const std::uint32_t uy0 = static_cast<std::uint32_t>(hy0);
        const std::uint32_t ux1 = static_cast<std::uint32_t>(hx1);
        const std::uint32_t uy1 = static_cast<std::uint32_t>(hy1);
        const std::size_t n = static_cast<std::size_t>(w) * h;
        // Depth-indexed scratch pair per recursion level (mirrors the full
        // rebuild's toneHost): grown on first use, reused by sibling groups.
        // Only the expanded region is written; grp is zeroed over the region
        // before each children composite (the walk blends over it, so stale
        // bytes would leak through).
        std::vector<std::vector<pittore::RGBAf>> toneHost;
        auto toneBuf = [&](std::size_t idx) -> pittore::RGBAf* {
            if (toneHost.size() <= idx) toneHost.resize(idx + 1);
            auto& v = toneHost[idx];
            if (v.size() != n) v.assign(n, pittore::RGBAf{});
            return v.data();
        };
        auto clearRegion = [&](pittore::RGBAf* buf) {
            for (int y = hy0; y < hy1; ++y)
                std::fill_n(buf + static_cast<std::size_t>(y) * w + hx0,
                            static_cast<std::size_t>(hx1 - hx0),
                            pittore::RGBAf{});
        };
        std::function<void(pittore::RGBAf*, int, int, int)> compCPURegion =
            [&](pittore::RGBAf* dst, int top, int bottom, int depth) {
                int g = -1, gend = -1;
                for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                    const auto cr =
                        gatheredRangeForPanels(s.header + 1, s.end);
                    if (cr.second <= cr.first) continue;
                    if (s.header > g) {
                        g = s.header;
                        gend = s.end;
                    }
                }
                if (g < 0) {
                    const auto r = gatheredRangeForPanels(top, bottom);
                    if (r.second <= r.first) return;
                    auto slice = sliceViews(r.first, r.second);
                    if (!slice.empty())
                        pittore::compute::composite_many_host(
                            dst, w, h, ux0, uy0, ux1, uy1, slice.data(),
                            slice.size());
                    return;
                }
                compCPURegion(dst, gend + 1, bottom, depth);  // below
                pittore::RGBAf* bdrop = toneBuf(std::size_t(2 * depth));
                clearRegion(bdrop);
                {
                    const auto r = gatheredRangeForPanels(gend + 1, bottom);
                    if (r.second > r.first) {
                        auto slice = sliceViews(r.first, r.second);
                        if (!slice.empty())
                            pittore::compute::composite_many_host(
                                bdrop, w, h, ux0, uy0, ux1, uy1, slice.data(),
                                slice.size());
                    }
                }
                pittore::RGBAf* grp = toneBuf(std::size_t(2 * depth + 1));
                clearRegion(grp);
                compCPURegion(grp, g + 1, gend, depth + 1);  // children
                const LayerItem& header = layers[g];
                // Region tone transfer over the expanded rect (the cached
                // level + pivot make the inner dirty rect exact).
                pittore::compute::applyToneBlendRegion(
                    grp, bdrop, grp, w, h, ux0, uy0, ux1, uy1,
                    header.toneBlend);
                pittore::compute::HostPlacedLayer synth;
                synth.src = grp;
                synth.sw = w;
                synth.sh = h;
                synth.ox = 0.0;
                synth.oy = 0.0;
                synth.sx = 1.0;
                synth.sy = 1.0;
                synth.x0 = ux0;
                synth.y0 = uy0;
                synth.x1 = ux1;
                synth.y1 = uy1;
                synth.fold = (header.opacity / 100.0f) *
                             (header.fill / 100.0f);
                synth.mode = engineBlendMode(header.blendMode);
                // Composite the toned region back; acc outside the expanded
                // region is preserved.
                pittore::compute::composite_many_host(dst, w, h, ux0, uy0,
                                                       ux1, uy1, &synth, 1);
                compCPURegion(dst, top, g - 1, depth);  // above
            };
        compCPURegion(acc, 0, layers.size() - 1, 0);
        // Blit just the inner dirty region (not halo) to canvas.
        const auto tprep1 = std::chrono::steady_clock::now();
        for (int y = y0; y < y1; ++y) {
            uchar* d = composite.bits() +
                       static_cast<qsizetype>(y) * composite.bytesPerLine() +
                       static_cast<qsizetype>(x0) * 4;
            blitRGBAfToPremul(acc + static_cast<std::size_t>(y) * w + x0, d,
                              static_cast<std::uint32_t>(x0),
                              static_cast<std::uint32_t>(y),
                              static_cast<std::uint32_t>(x1 - x0), 1);
        }
        const auto tblit1 = std::chrono::steady_clock::now();
        drawVectorOverlays();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG(
                "[render][region-tone] backend=cpu dirty=(%d,%d,%d,%d) "
                "layers=%d prep=%.2f blit=%.2f ms=%.2f",
                x0, y0, x1 - x0, y1 - y0, static_cast<int>(layers.size()),
                std::chrono::duration<double, std::milli>(tprep1 - tprep0).count(),
                std::chrono::duration<double, std::milli>(tblit1 - tprep1).count(),
                ms);
        return;
    }
}

void AppState::flushPaint() {
    DocumentItem* d = activeDocument();
    if (!d || d->paintDirty.isNull()) return;
    const QRect dirty = d->paintDirty;
    d->paintDirty = QRect();
    const auto t0 = std::chrono::steady_clock::now();
    d->renderRegion(dirty);
    // Wash strokes live in the scratch buffer: fold the dirty region over
    // the fresh composite at the stroke opacity so the stroke previews
    // live (the bake makes it permanent on release). Both sides stay in
    // premultiplied space: the composite is ARGB32_Premultiplied and the
    // straight scratch is premultiplied on the fly.
    if (washArmed_ && !washDirty_.isNull() && !d->composite.isNull() &&
        int(washScratch_.width()) == d->composite.width() &&
        int(washScratch_.height()) == d->composite.height()) {
        const QVariant o =
            option(activeTool_, QStringLiteral("opacity"));
        const double op =
            o.isValid() ? std::clamp(o.toDouble() / 100.0, 0.0, 1.0) : 1.0;
        if (op > 0.0) {
            const QRect region =
                dirty.intersected(washDirty_).intersected(
                    QRect(QPoint(0, 0), d->size));
            const std::string mixId =
                option(activeTool_, QStringLiteral("brush_composite"))
                    .toString()
                    .toStdString();
            const auto mixOp =
                pittore::compute::dabmix::mix_from_id(mixId.c_str());
            const bool useMix =
                mixOp != pittore::compute::dabmix::DabMixOp::Normal;
            // The tool's blend mode applies to the whole wash at once (the
            // scratch accumulated at Normal), mirroring the release bake.
            const pittore::compute::BlendMode washBlend =
                paintBlendFor(activeTool_);
            const bool useBlend =
                !useMix && washBlend != pittore::compute::BlendMode::Normal;
            const pittore::RGBAf* src = washScratch_.data();
            const std::uint32_t w = washScratch_.width();
            for (int y = region.top(); y <= region.bottom(); ++y) {                QRgb* row = reinterpret_cast<QRgb*>(
                    d->composite.scanLine(y));
                for (int x = region.left(); x <= region.right(); ++x) {
                    const pittore::RGBAf& s =
                        src[std::size_t(y) * w + x];
                    const float sa =
                        std::clamp(s.a * float(op), 0.0f, 1.0f);
                    if (sa <= 0.0f) continue;
                    if (useMix) {
                        // Pigment-mix preview: run the dab color through
                        // the mean shell, then re-premultiply into the
                        // composite row (which stores premultiplied, so
                        // un-premultiply first).
                        const QRgb dst = row[x];
                        const float da = qAlpha(dst) / 255.0f;
                        float dr = 0.0f, dg = 0.0f, db = 0.0f;
                        if (da > 1e-6f) {
                            dr = qRed(dst) / 255.0f / da;
                            dg = qGreen(dst) / 255.0f / da;
                            db = qBlue(dst) / 255.0f / da;
                        }
                        float or_, og, ob, oa;
                        pittore::compute::dabmix::mix_pixel(
                            mixOp, s.r, s.g, s.b, sa, dr, dg, db, da, or_,
                            og, ob, oa);
                        row[x] = qRgba(
                            std::clamp(int(or_ * oa * 255.0f), 0, 255),
                            std::clamp(int(og * oa * 255.0f), 0, 255),
                            std::clamp(int(ob * oa * 255.0f), 0, 255),
                            std::clamp(int(oa * 255.0f), 0, 255));
                        continue;
                    }
                    if (useBlend) {
                        // Blend-mode preview: unpremultiply the backdrop,
                        // run the shared W3C shell, re-premultiply into the
                        // composite row.
                        const QRgb dst = row[x];
                        const float da = qAlpha(dst) / 255.0f;
                        float dr = 0.0f, dg = 0.0f, db = 0.0f;
                        if (da > 1e-6f) {
                            dr = qRed(dst) / 255.0f / da;
                            dg = qGreen(dst) / 255.0f / da;
                            db = qBlue(dst) / 255.0f / da;
                        }
                        float or_, og, ob, oa;
                        pittore::compute::blend::pixel(
                            washBlend, s.r, s.g, s.b, sa, dr, dg, db, da, x,
                            y, or_, og, ob, oa);
                        row[x] = qRgba(
                            std::clamp(int(or_ * oa * 255.0f), 0, 255),
                            std::clamp(int(og * oa * 255.0f), 0, 255),
                            std::clamp(int(ob * oa * 255.0f), 0, 255),
                            std::clamp(int(oa * 255.0f), 0, 255));
                        continue;
                    }
                    const QRgb dst = row[x];
                    const float inv = 1.0f - sa;
                    const float out_a =
                        sa + (qAlpha(dst) / 255.0f) * inv;
                    if (out_a <= 0.0f) continue;
                    const float r =
                        (s.r * sa + (qRed(dst) / 255.0f) * inv) / out_a;
                    const float g =
                        (s.g * sa + (qGreen(dst) / 255.0f) * inv) / out_a;
                    const float b =
                        (s.b * sa + (qBlue(dst) / 255.0f) * inv) / out_a;
                    row[x] = qRgba(std::clamp(int(r * out_a * 255.0f), 0, 255),
                                   std::clamp(int(g * out_a * 255.0f), 0, 255),
                                   std::clamp(int(b * out_a * 255.0f), 0, 255),
                                   std::clamp(int(out_a * 255.0f), 0, 255));
                }
            }
            // Track the overlay footprint so the tile cache falls back to
            // the composite tier under live wash (simple-tier tiles blit
            // own layer pixels, which carry no wash ink until bake).
            if (!region.isEmpty())
                d->washOverlayDirty = d->washOverlayDirty.united(region);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    // Per input event: trace-gated like the dab logs (mutex + file write per
    // event dominated fast strokes).
    if (pittore::core::log::strokeTrace())
        PITTORE_LOG("[flush] dirty=(%d,%d,%d,%d) backend=%s ms=%.2f", dirty.x(), dirty.y(),
                     dirty.width(), dirty.height(),
                     d->backend ? d->backend->name().c_str() : "cpu", ms);
    // Region notice: the canvas repaints only the stroke's touching rect —
    // a full-frame refresh per brush move was the lag in the paint logs.
    noteRegionEdit(QRectF(dirty));
    emit regionModified(d, QRectF(dirty));
}

void AppState::noteRegionEdit(const QRectF& docRect) {
    regionEditRect_ = docRect;
    regionEditTimeUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
}

qint64 AppState::takeRegionEditAgeUs() {
    if (regionEditTimeUs_ < 0) return -1;
    const qint64 ageUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count() -
                         regionEditTimeUs_;
    regionEditTimeUs_ = -1;  // consumed by the first paint after the edit
    return (ageUs < 0 || ageUs >= 1000000) ? -1 : ageUs;
}


void DocumentItem::ensurePainter() {
    if (stage_ && stage_->acc) return;
    const std::size_t bytes = layerBytes(size);
    pittore::compute::ComputeBackend& be =
        backend ? *backend : cpuBackend();
    if (!stage_) stage_ = std::make_unique<PaintStage>();
    stage_->acc = std::move(be.make_buffer(bytes));
}

// Cached device copy of a placed layer's native pixels (see PaintStage::sources).
// Returns a buffer the GPU backend samples in composite_placed; the host copy
// is uploaded once and re-uploaded only when the layer's pixels change.
const pittore::compute::Buffer& DocumentItem::placedSourceFor(const LayerItem& l) {
    ensurePainter();
    const LayerDrawSource s = layerDrawSource(l);
    // Keyed on the layer's own pixels (stable across style edits) even when the
    // uploaded bytes are the styled render, so a dragged slider refreshes one
    // device slot instead of accumulating a new one per frame.
    const void* key = imageKey(l.pixels);
    const std::size_t nbytes =
        s.img ? static_cast<std::size_t>(s.img->width()) * s.img->height() *
                    sizeof(pittore::RGBAf)
              : 0;
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    auto it = stage_->sources.find(key);
    if (it != stage_->sources.end()) {
        PaintStage::PlacedSource& e = it->second;
        if (e.dev && e.stamp == l.sourceStamp && e.bytes == nbytes &&
            e.rev == l.styledRev)
            return *e.dev;
        // Same source, new revision: refresh the existing slot instead of
        // pushing a new one, so a paint stroke never accumulates whole-layer
        // device buffers (a fresh Image replaces the slot outright).
        if (!e.dev || e.bytes != nbytes) e.dev = be.make_buffer(nbytes);
        e.stamp = l.sourceStamp;
        e.rev = l.styledRev;
        e.bytes = nbytes;
        if (key && s.img) std::memcpy(e.dev->host(), s.img->data(), nbytes);
        // Buffers are created zeroed; push the current pixels (no-op on CPU).
        e.dev->upload();
        // Full-layer refresh: rare (edits go through refreshPlacedRegion),
        // so this stays as the churn signal when fallbacks fire per event.
        if (pittore::core::log::strokeTrace())
            ::pittore::core::log::log_info(
                "[render][placed] source refreshed key=%p bytes=%zu backend=%s", key,
                nbytes, be.name().c_str());
        return *e.dev;
    }
    auto dev = be.make_buffer(nbytes);
    if (key && s.img) std::memcpy(dev->host(), s.img->data(), nbytes);
    // Buffers are created zeroed; push the current pixels (no-op on CPU).
    dev->upload();
    // Cache fills are per-layer traffic on thousand-layer documents (one
    // line per layer per cold composite); only an explicit bulk trace pays
    // for it. The refresh path just above is rare churn and stays on.
    if (pittore::core::log::bulkTrace())
        ::pittore::core::log::log_info(
            "[render][placed] source cached key=%p bytes=%zu backend=%s", key,
            nbytes, be.name().c_str());
    auto ins = stage_->sources.emplace(
        key, PaintStage::PlacedSource{key, l.sourceStamp, l.styledRev, nbytes,
                                      std::move(dev)});
    return *ins.first->second.dev;
}

// Cached device copy of a layer mask's opaque-grey coverage. The host copy
// is uploaded once and re-uploaded only when the mask image changes.
const pittore::Image* DocumentItem::finishedMaskFor(const LayerItem& l) {
    const pittore::Image* mask = effectiveMaskImage(size, l);
    if (!mask || (l.maskFeather <= 0.0f && l.maskDensity == 1.0f))
        return nullptr;
    ensurePainter();
    const void* fkey = imageKey(l.mask);
    auto fit = stage_->finishedMasks.find(fkey);
    if (fit != stage_->finishedMasks.end() &&
        fit->second.stamp == l.maskStamp &&
        fit->second.density == l.maskDensity &&
        fit->second.feather == l.maskFeather)
        return fit->second.img.get();
    auto finished = finishMaskImage(*mask, l.maskDensity, l.maskFeather);
    const pittore::Image* out = finished.get();
    stage_->finishedMasks[fkey] = {std::move(finished), l.maskStamp,
                                   l.maskDensity, l.maskFeather};
    return out;
}

const pittore::compute::Buffer* DocumentItem::maskSourceFor(const LayerItem& l) {
    const pittore::Image* mask = effectiveMaskImage(size, l);
    if (!mask) return nullptr;
    ensurePainter();
    // Density/feather finish on the host (cached; see finishedMaskFor). The
    // device upload below copies the finished bytes, so every backend
    // samples identical coverage.
    const pittore::Image* upload = mask;
    if (const pittore::Image* fin = finishedMaskFor(l)) upload = fin;
    // Keyed by the raw mask (stamp-invalidated); the uploaded bytes are the
    // finished image when density/feather apply (identical dimensions).
    const void* key = imageKey(l.mask);
    const std::size_t nbytes = static_cast<std::size_t>(mask->width()) *
                               mask->height() * sizeof(pittore::RGBAf);
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    auto it = stage_->masks.find(key);
    if (it != stage_->masks.end()) {
        PaintStage::PlacedSource& e = it->second;
        if (e.dev && e.stamp == l.maskStamp && e.bytes == nbytes)
            return e.dev.get();
        if (!e.dev || e.bytes != nbytes) e.dev = be.make_buffer(nbytes);
        e.stamp = l.maskStamp;
        e.rev = 0;
        e.bytes = nbytes;
        std::memcpy(e.dev->host(), upload->data(), nbytes);
        e.dev->upload();
        return e.dev.get();
    }
    auto dev = be.make_buffer(nbytes);
    std::memcpy(dev->host(), upload->data(), nbytes);
    dev->upload();
    auto ins = stage_->masks.emplace(
        key, PaintStage::PlacedSource{key, l.maskStamp, 0, nbytes,
                                      std::move(dev)});
    return ins.first->second.dev.get();
}

void DocumentItem::refreshPlacedRegion(LayerItem& l, const QRect& layerRect) {
    if (!l.pixels) {
        ++l.sourceStamp;
        return;
    }
    ensurePainter();
    // Direct iff the compositor samples the native pixels themselves (no
    // live filter/style render in between). Object comparison is exact:
    // filtered/styled renders are separate shared Images, both alive here.
    const LayerDrawSource s = layerDrawSource(l);  // cheap on valid stamps
    if (!s.img || s.img.get() != l.pixels.get()) {
        ++l.sourceStamp;  // filtered/styled: full refresh on next gather
        return;
    }
    const std::uint32_t w = s.img->width();
    const std::uint32_t h = s.img->height();
    const int x0 = std::max(0, layerRect.left());
    const int y0 = std::max(0, layerRect.top());
    const int x1 = std::min<std::uint32_t>(w, static_cast<std::uint32_t>(
        std::max(0, layerRect.left() + layerRect.width())));
    const int y1 = std::min<std::uint32_t>(h, static_cast<std::uint32_t>(
        std::max(0, layerRect.top() + layerRect.height())));
    if (x0 >= x1 || y0 >= y1) return;  // dab missed: pixels provably untouched
    const void* key = imageKey(l.pixels);
    const std::size_t nbytes =
        static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    auto it = stage_->sources.find(key);
    if (it == stage_->sources.end() || !it->second.dev ||
        it->second.bytes != nbytes || it->second.stamp != l.sourceStamp ||
        it->second.rev != l.styledRev) {
        ++l.sourceStamp;  // no usable slot: full refresh on next gather
        return;
    }
    // Slot bytes are current except this rect: patch the staged rows and push
    // only the region (no-op on CPU). The stamp stays valid — no churn.
    PaintStage::PlacedSource& e = it->second;
    const auto* src = s.img->data();
    auto* dst = static_cast<char*>(e.dev->host());
    const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(pittore::RGBAf);
    const std::size_t rowBytes =
        static_cast<std::size_t>(x1 - x0) * sizeof(pittore::RGBAf);
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * pitch +
                        static_cast<std::size_t>(x0) * sizeof(pittore::RGBAf),
                    src + static_cast<std::size_t>(y) * w + x0, rowBytes);
    e.dev->upload_region(w, static_cast<std::uint32_t>(x0),
                         static_cast<std::uint32_t>(y0),
                         static_cast<std::uint32_t>(x1),
                         static_cast<std::uint32_t>(y1));
}

void DocumentItem::touchPixelsAfterDab(LayerItem& l, float lcx, float lcy,
                                       float lradius, std::uint32_t lw,
                                       std::uint32_t lh,
                                       bool preserveInkColor) {
    const int x0 = std::max(0, static_cast<int>(std::floor(lcx - lradius - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(lcy - lradius - 1.0f)));
    const int x1 = std::min(static_cast<int>(lw),
                            static_cast<int>(std::ceil(lcx + lradius + 1.0f)));
    const int y1 = std::min(static_cast<int>(lh),
                            static_cast<int>(std::ceil(lcy + lradius + 1.0f)));
    refreshPlacedRegion(l, QRect(x0, y0, x1 - x0, y1 - y0));
    if (!preserveInkColor)
        refreshLayerCmyk(*this, l, QRect(x0, y0, x1 - x0, y1 - y0));
    l.thumbnail = QImage();  // stale panel preview
}

void DocumentItem::refreshMaskRegion(LayerItem& l, const QRect& maskRect) {
    if (!l.mask) {
        ++l.maskStamp;
        return;
    }
    ensurePainter();
    const pittore::Image* mask = effectiveMaskImage(size, l);
    if (!mask || mask != l.mask.get()) {
        ++l.maskStamp;
        return;
    }
    // The staged slot holds finished bytes when density/feather apply:
    // patch the finished cache in place instead of re-baking it whole.
    const pittore::Image* upload = mask;
    if (const pittore::Image* fin = finishedMaskFor(l)) {
        if (!patchFinishedMaskRegion(*mask, l.maskDensity, l.maskFeather,
                                     maskRect.left(), maskRect.top(),
                                     maskRect.left() + maskRect.width(),
                                     maskRect.top() + maskRect.height(),
                                     *const_cast<pittore::Image*>(fin))) {
            ++l.maskStamp;  // size mismatch: full re-bake on next gather
            return;
        }
        upload = fin;
    }
    const std::uint32_t w = upload->width();
    const std::uint32_t h = upload->height();
    const int x0 = std::max(0, maskRect.left());
    const int y0 = std::max(0, maskRect.top());
    const int x1 = std::min<std::uint32_t>(w, static_cast<std::uint32_t>(
        std::max(0, maskRect.left() + maskRect.width())));
    const int y1 = std::min<std::uint32_t>(h, static_cast<std::uint32_t>(
        std::max(0, maskRect.top() + maskRect.height())));
    if (x0 >= x1 || y0 >= y1) return;
    const void* key = imageKey(l.mask);
    const std::size_t nbytes =
        static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    auto it = stage_->masks.find(key);
    if (it == stage_->masks.end() || !it->second.dev ||
        it->second.bytes != nbytes || it->second.stamp != l.maskStamp) {
        ++l.maskStamp;
        return;
    }
    PaintStage::PlacedSource& e = it->second;
    const auto* src = upload->data();
    auto* dst = static_cast<char*>(e.dev->host());
    const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(pittore::RGBAf);
    const std::size_t rowBytes =
        static_cast<std::size_t>(x1 - x0) * sizeof(pittore::RGBAf);
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * pitch +
                        static_cast<std::size_t>(x0) * sizeof(pittore::RGBAf),
                    src + static_cast<std::size_t>(y) * w + x0, rowBytes);
    e.dev->upload_region(w, static_cast<std::uint32_t>(x0),
                         static_cast<std::uint32_t>(y0),
                         static_cast<std::uint32_t>(x1),
                         static_cast<std::uint32_t>(y1));
}

// Cached device copy of an adjustment layer's aux table: 3x256 floats for
// Curves (R/G/B), 256 for Levels (see PaintStage::auxs).
const pittore::compute::Buffer* DocumentItem::adjAuxSourceFor(
    const LayerItem& l) {
    const std::size_t want = adjustmentAuxSize(l);
    if (want == 0 || l.adjustmentLUT.size() != want) return nullptr;
    ensurePainter();
    // Keyed by the LUT bytes' address, whose vector block can be handed to a
    // later same-size table after a layer goes away; the stamp does not always
    // differ between the two. Content settles it: the slot's host copy is the
    // staged truth, and the whole table is under 3 KB.
    const void* key = static_cast<const void*>(l.adjustmentLUT.data());
    const std::size_t nbytes = want * sizeof(float);
    pittore::compute::ComputeBackend& be = backend ? *backend : cpuBackend();
    auto it = stage_->auxs.find(key);
    if (it != stage_->auxs.end()) {
        PaintStage::PlacedSource& e = it->second;
        if (e.dev && e.stamp == l.adjustStamp && e.bytes == nbytes &&
            std::memcmp(e.dev->host(), l.adjustmentLUT.data(), nbytes) == 0)
            return e.dev.get();
        if (!e.dev || e.bytes != nbytes) e.dev = be.make_buffer(nbytes);
        e.stamp = l.adjustStamp;
        e.rev = 0;
        e.bytes = nbytes;
        std::memcpy(e.dev->host(), l.adjustmentLUT.data(), nbytes);
        e.dev->upload();
        return e.dev.get();
    }
    auto dev = be.make_buffer(nbytes);
    std::memcpy(dev->host(), l.adjustmentLUT.data(), nbytes);
    dev->upload();
    auto ins = stage_->auxs.emplace(
        key, PaintStage::PlacedSource{key, l.adjustStamp, 0, nbytes,
                                      std::move(dev)});
    return ins.first->second.dev.get();
}

void DocumentItem::prunePlacedSources() {
    if (!stage_) return;
    std::vector<const void*> ownedPixels;
    std::vector<const void*> ownedMasks;
    ownedPixels.reserve(layers.size());
    ownedMasks.reserve(layers.size());
    for (const LayerItem& l : layers) {
        if (l.kind == LayerItem::Kind::Pixel && l.pixels)
            ownedPixels.push_back(imageKey(l.pixels));
        if (effectiveMaskImage(size, l))
            ownedMasks.push_back(imageKey(l.mask));
    }
    std::sort(ownedPixels.begin(), ownedPixels.end());
    std::sort(ownedMasks.begin(), ownedMasks.end());
    for (auto it = stage_->sources.begin(); it != stage_->sources.end();) {
        if (std::binary_search(ownedPixels.begin(), ownedPixels.end(),
                               it->first))
            ++it;
        else
            it = stage_->sources.erase(it);
    }
    for (auto it = stage_->masks.begin(); it != stage_->masks.end();) {
        if (std::binary_search(ownedMasks.begin(), ownedMasks.end(), it->first))
            ++it;
        else
            it = stage_->masks.erase(it);
    }
    for (auto it = stage_->finishedMasks.begin();
         it != stage_->finishedMasks.end();) {
        if (std::binary_search(ownedMasks.begin(), ownedMasks.end(), it->first))
            ++it;
        else
            it = stage_->finishedMasks.erase(it);
    }
    std::vector<const void*> ownedAux;
    ownedAux.reserve(layers.size());
    for (const LayerItem& l : layers) {
        if (adjustmentAuxSize(l) != 0 &&
            l.adjustmentLUT.size() == adjustmentAuxSize(l))
            ownedAux.push_back(
                static_cast<const void*>(l.adjustmentLUT.data()));
    }
    std::sort(ownedAux.begin(), ownedAux.end());
    for (auto it = stage_->auxs.begin(); it != stage_->auxs.end();) {
        if (std::binary_search(ownedAux.begin(), ownedAux.end(), it->first))
            ++it;
        else
            it = stage_->auxs.erase(it);
    }
}



// ---------------------------------------------------------------------------
// DocumentItem
// ---------------------------------------------------------------------------
DocumentItem::DocumentItem(QString t, QSize s, int d)
    : title(std::move(t)), size(s), dpi(d) {
    LayerItem bg;
    bg.name = QStringLiteral("Background");
    bg.locked = true;
    bg.swatch = QColor(0xf2, 0xf2, 0xf2);  // matches default canvasPaper
    layers.append(bg);

    history.append({QStringLiteral("New"), QStringLiteral("newlayer"), true});
    historyPosition = 0;
    rebuildComposite();
}

DocumentItem::~DocumentItem() = default;

AppState::~AppState() { qDeleteAll(documents_); }

void DocumentItem::setBackend(pittore::compute::ComputeBackend* be) {
    backend = be;
    stage_.reset();               // staging buffers belong to the old backend
    rebuildComposite();
}

}  // namespace pittore::ui



