// Device composite for DocumentItem::rebuildComposite. Everything here
// moved out of app_state.cpp unchanged, wrapped so a device failure (OOM
// included) returns false and the caller falls through to the CPU
// reference path instead of terminating the app.

#include "ui/app_state_detail.h"

#include "engine/compute/backend.h"
#include "engine/core/log.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pittore::ui {

bool DocumentItem::rebuildCompositeGPU(pittore::compute::ComputeBackend& be,
                                       pittore::compute::Buffer& acc,
                                       std::vector<std::unique_ptr<pittore::compute::Buffer>>&
                                           toneScratch,
                                       std::uint32_t w, std::uint32_t h,
                                       bool toneActive) {
    const auto t0 = std::chrono::steady_clock::now();
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
    try {
        be.clear(acc);
        const auto tclear = std::chrono::steady_clock::now();
        // Gather every visible pixel layer's device source + clipped footprint,
        // then composite them ALL with one batched launch: a canvas holding
        // thousands of small parts must not launch thousands of kernels.
        std::vector<CompositedLayer> gathered;
        gatherCompositedLayers(QRect(QPoint(0, 0), size), gathered);
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
        if (!toneActive) {
            if (!placed.empty()) be.composite_many_into(acc, w, 0, 0, w,
                                                        h, placed.data(),
                                                        placed.size());
        } else {
            // Segmented composite: tone-blend spans composite their children
            // in isolation (temp buffer), blend against a snapshot of the
            // accumulator below them, and re-enter as one synthetic layer.
            // Clip bases index the gathered list, so slices remap them to
            // slice-relative positions (out-of-slice bases degrade to
            // unclipped, mirroring composite_many_host's own validation —
            // and cross-group clip pairs can never be valid, see
            // gatherCompositedLayers).
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
            // Gathered-index subrange whose entries fall in a panel range
            // (gather preserves panel order, so the slice is contiguous).
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
            std::function<void(pittore::compute::Buffer&, int, int, int)> compGPU =
                [&](pittore::compute::Buffer& acc, int top, int bottom,
                    int depth) {
                    int g = -1, gend = -1;
                    for (const ToneSpan& s : toneSpansIn(top, bottom)) {
                        // Skip headers whose children contribute nothing.
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
                            be.composite_many_into(acc, w, 0, 0, w, h,
                                                   slice.data(), slice.size());
                        return;
                    }
                    compGPU(acc, gend + 1, bottom, depth);  // everything below
                    // Depth-indexed scratch pair for this level (never shared
                    // with nested levels); grown lazily, persists in stage_.
                    const std::size_t need = std::size_t(2 * (depth + 1));
                    while (toneScratch.size() < need)
                        toneScratch.push_back(
                            be.make_buffer(static_cast<std::size_t>(w) * h *
                                           sizeof(pittore::RGBAf)));
                    pittore::compute::Buffer& grp =
                        *toneScratch[std::size_t(2 * depth)];
                    pittore::compute::Buffer& bdrop =
                        *toneScratch[std::size_t(2 * depth + 1)];
                    // Backdrop snapshot: device copy of the below-slice the
                    // line above just composited into acc (was: a second
                    // full composite of the same slice). Bit-identical,
                    // one D2D instead of a second kernel over all layers.
                    bdrop.copy_from(acc);
                    be.clear(grp);
                    compGPU(grp, g + 1, gend, depth + 1);  // children
                    const LayerItem& header = layers[g];
                    be.tone_blend(grp, bdrop, w, h, header.toneBlend);
                    // The blended group re-enters as one Normal layer with
                    // the header's own opacity/fill/mode (masks on group
                    // headers are engine-wide no-ops, matching groups).
                    pittore::compute::PlacedLayer synth;
                    synth.src = &grp;
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
                    be.composite_many_into(acc, w, 0, 0, w, h, &synth, 1);
                    compGPU(acc, top, g - 1, depth);  // everything above
                };
            compGPU(acc, 0, layers.size() - 1, 0);
        }
        const auto tplaced = std::chrono::steady_clock::now();
        // Layers that went away leave orphaned device copies behind; prune
        // them instead of dropping the whole cache, so unchanged layers keep
        // their uploaded pixels and a rebuild never does 4101 fresh cudaMalloc
        // + upload dances. placedSourceFor self-heals entries whose size or
        // stamp changed, so matching the ownership pointer is enough.
        prunePlacedSources();
        const auto tprune = std::chrono::steady_clock::now();
        be.blit_premul(acc, composite.bits(), w, 0, 0, w, h,
                       static_cast<std::size_t>(composite.bytesPerLine()));
        const auto tblit = std::chrono::steady_clock::now();
        drawVectorOverlays();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        PITTORE_LOG("[render][rebuild] w=%u h=%u layers=%d backend=%s ms=%.2f",
                     w, h, static_cast<int>(layers.size()),
                     backend ? backend->name().c_str() : "cpu", ms);
        // Breakdown only on demand: per-rebuild tracing doubles rebuild
        // cost with mutex + file I/O. Slow rebuilds always report.
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG(
                "[render][rebuild-breakdown] clear=%.3f gather+placed=%.3f "
                "prune=%.3f blit=%.3f overlay=%.3f stack=%s",
                std::chrono::duration<double, std::milli>(tclear - t0).count(),
                std::chrono::duration<double, std::milli>(tplaced - tclear).count(),
                std::chrono::duration<double, std::milli>(tprune - tplaced).count(),
                std::chrono::duration<double, std::milli>(tblit - tprune).count(),
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tblit).count(),
                describeStack(gathered).toLocal8Bit().constData());
        return true;
    } catch (const std::exception& e) {
        PITTORE_LOG("[render][rebuild] GPU composite failed (%s); CPU fallback",
                    e.what());
        return false;
    }
}

bool DocumentItem::renderRegionGPU(pittore::compute::ComputeBackend& be,
                                   pittore::compute::Buffer& acc, int x0,
                                   int y0, int x1, int y1, std::uint32_t w,
                                   std::uint32_t, const QRect& region) {
    const auto t0 = std::chrono::steady_clock::now();
    try {
        const auto tprep0 = std::chrono::steady_clock::now();
        be.clear(acc);
        // A layer missing the dirty region (halo included) stages nothing —
        // skipping it is exactly compositing transparency. This is what
        // keeps multi-part documents interactive: a move touches the moved
        // layer plus whatever sits under/over it, not the whole stack.
        // Everything that DOES touch the region is composited with one
        // batched launch, so the dirty-rect path never pays per-layer
        // launch overhead either.
        std::vector<CompositedLayer> gathered;
        gatherCompositedLayers(QRect(x0, y0, x1 - x0, y1 - y0), gathered);
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
        if (!placed.empty())
            be.composite_many_into(acc, w,
                                   static_cast<std::uint32_t>(x0),
                                   static_cast<std::uint32_t>(y0),
                                   static_cast<std::uint32_t>(x1),
                                   static_cast<std::uint32_t>(y1),
                                   placed.data(), placed.size());
        uchar* dst = composite.bits() +
                     static_cast<qsizetype>(y0) * composite.bytesPerLine() +
                     static_cast<qsizetype>(x0) * 4;
        // The launches above are async, so the work lands here: blit_premul
        // waits on the stream when it DMAs the converted rows back.
        const auto tprep1 = std::chrono::steady_clock::now();
        be.blit_premul(acc, dst, w, static_cast<std::uint32_t>(x0),
                       static_cast<std::uint32_t>(y0),
                       static_cast<std::uint32_t>(x1),
                       static_cast<std::uint32_t>(y1),
                       static_cast<std::size_t>(composite.bytesPerLine()));
        const auto tblit1 = std::chrono::steady_clock::now();
        drawVectorOverlays();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        // Per-event log: gated like the dab logs, but slow regions always
        // report with their prep/blit split so real slowness is attributable.
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG(
                "[render][region] incremental backend=%s region=(%d,%d,%d,%d) "
                "layers=%d prep=%.2f blit=%.2f ms=%.2f",
                backend ? backend->name().c_str() : "cpu", region.x(), region.y(),
                region.width(), region.height(), static_cast<int>(layers.size()),
                std::chrono::duration<double, std::milli>(tprep1 - tprep0).count(),
                std::chrono::duration<double, std::milli>(tblit1 - tprep1).count(),
                ms);
        return true;
    } catch (const std::exception& e) {
        PITTORE_LOG("[render][region] GPU composite failed (%s); CPU fallback",
                    e.what());
        return false;
    }
}

}  // namespace pittore::ui
