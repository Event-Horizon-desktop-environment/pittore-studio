// Liquify stroke, session and mask state.
//
// Split out of app_state.cpp; AppState members, no extra declarations needed.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"

#include "engine/compute/backend.h"
#include "engine/compute/warp.h"
#include "engine/core/log.h"

#include <algorithm>
#include <cmath>
#include <vector>
namespace pittore::ui {

// ---------------------------------------------------------------------------
// Liquify (displacement warp)
// ---------------------------------------------------------------------------
bool AppState::beginLiquifyStroke() {
    DocumentItem* d = activeDocument();
    if (!d || d->size.isEmpty()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !isPaintable(*layer)) return false;

    // One history entry for the whole stroke. The snapshot must be taken
    // BEFORE the layer is rebound: it shares the pre-stroke image, so undo
    // restores exactly what the warp started from. Use the gesture-scoped API
    // so the matching commitUndoStep()/discardUndoStep() from the stroke's end
    // actually promote/drop it (raw beginUndoAction would leave the gesture
    // depth at zero and the commit would be dropped).
    beginUndoStep();
    // beginUndoStep() COW-shares the layer vector with the snapshot, so the
    // pointer taken above goes stale the moment that vector detaches: re-resolve
    // it. This access detaches once; after it the pointer is stable for the
    // freeze/rebind below, and the snapshot keeps the untouched original.
    layer = activeLayer();
    ensureLayerPixels(*d, *layer);
    if (!layer->pixels || layer->pixels->width() == 0 ||
        layer->pixels->height() == 0) {
        discardUndoStep();
        return false;
    }

    // Freeze the pre-stroke pixels as the warp source and hand the live layer a
    // private copy to receive the resamples. dst never aliases src, and the
    // frozen image stays alive for the whole stroke (the undo snapshot shares
    // it, so undoing restores the original).
    liquifySrc_ = layer->pixels;
    liquifyCmykSrc_ = layer->cmykPixels;
    layer->pixels = std::make_shared<pittore::Image>(liquifySrc_->clone());
    ++layer->sourceStamp;
    layer->cmykSourceStamp = layer->cmykPixels
                                 ? layer->sourceStamp
                                 : 0;
    layer->thumbnail = QImage();

    // Stage the frozen source once: a displaced sample can land anywhere in it,
    // so it stays device-resident for the whole stroke (the same contract
    // composite_placed has for a placed layer).
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    const std::size_t bytes =
        static_cast<std::size_t>(w) * h * sizeof(pittore::RGBAf);
    if (!liquifySrcBuf_ || !liquifyDstBuf_ || liquifyBufW_ != w ||
        liquifyBufH_ != h) {
        pittore::compute::ComputeBackend& be = computeBackend();
        liquifySrcBuf_ = be.make_buffer(bytes);
        liquifyDstBuf_ = be.make_buffer(bytes);
        liquifyBufW_ = w;
        liquifyBufH_ = h;
    }
    std::memcpy(liquifySrcBuf_->host(), liquifySrc_->data(), bytes);
    liquifySrcBuf_->upload();
    PITTORE_LOG("[liquify] begin %ux%u backend=%s", w, h,
                 d->backend ? d->backend->name().c_str() : "cpu");
    return true;
}

bool AppState::liquifyDab(const pittore::compute::WarpMesh& mesh, int x0,
                          int y0, int x1, int y1) {
    DocumentItem* d = activeDocument();
    if (!d) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels || !liquifySrc_ || !liquifySrcBuf_ ||
        !liquifyDstBuf_)
        return false;

    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    if (liquifySrc_->width() != w || liquifySrc_->height() != h) return false;

    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(x1, static_cast<int>(w));
    y1 = std::min(y1, static_cast<int>(h));
    if (x0 >= x1 || y0 >= y1) return false;

    const pittore::compute::WarpSubgrid grid =
        pittore::compute::warp_subgrid(mesh, x0, y0, x1, y1);
    if (grid.empty()) return false;

    computeBackend().warp(*liquifyDstBuf_, *liquifySrcBuf_, w, h, grid,
                          static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1),
                          static_cast<std::uint32_t>(y1));

    // The rebuilt region is in the destination buffer's host staging; copy it
    // into the layer's pixels, which stay the source of truth for undo, saving
    // and compositing.
    const pittore::RGBAf* src =
        static_cast<const pittore::RGBAf*>(liquifyDstBuf_->host());
    pittore::RGBAf* dst = layer->pixels->data();
    const std::size_t rowBytes =
        static_cast<std::size_t>(x1 - x0) * sizeof(pittore::RGBAf);
    for (int y = y0; y < y1; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * w + x0,
                    src + static_cast<std::size_t>(y) * w + x0, rowBytes);
    ++layer->sourceStamp;
    refreshLayerCmyk(*d, *layer, QRect(x0, y0, x1 - x0, y1 - y0));
    layer->thumbnail = QImage();
    // Dirty rect: the resampled layer rect mapped to document space, plus the
    // resampling halo (as in paintDab).
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const double dx0 = layer->offset.x() + x0 * lsx;
    const double dy0 = layer->offset.y() + y0 * lsy;
    const double dx1 = layer->offset.x() + x1 * lsx;
    const double dy1 = layer->offset.y() + y1 * lsy;
    const QRect dabRect(
        static_cast<int>(std::floor(dx0)) - 1,
        static_cast<int>(std::floor(dy0)) - 1,
        static_cast<int>(std::ceil(dx1 - dx0)) + 3,
        static_cast<int>(std::ceil(dy1 - dy0)) + 3);
    const QRect clipped = dabRect.intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty =
        d->paintDirty.isNull() ? clipped : d->paintDirty.united(clipped);
    return true;
}

void AppState::endLiquifyStroke() {
    // Keep the staging buffers (grown on demand, reused across strokes); only
    // the frozen snapshot is stroke-scoped.
    liquifySrc_.reset();
    liquifyCmykSrc_.reset();
}

void AppState::cancelLiquifyStroke() {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (d && layer && liquifySrc_ && layer->pixels &&
        layer->pixels->width() == liquifySrc_->width() &&
        layer->pixels->height() == liquifySrc_->height()) {
        layer->pixels = liquifySrc_;   // drop the partially warped copy
        ++layer->sourceStamp;
        layer->cmykPixels = liquifyCmykSrc_;
        layer->cmykSourceStamp = layer->cmykPixels
                                     ? layer->sourceStamp
                                     : 0;
        layer->thumbnail = QImage();
    }
    endLiquifyStroke();
    if (d) {
        discardUndoStep();
        d->paintDirty = QRect();
        d->rebuildComposite();
        emit regionModified(d, QRectF(QPoint(0, 0), d->size));
        emit documentModified(d);
    }
}

bool AppState::beginLiquifySession() {
    if (liquifySession_) return liquifySrc_ != nullptr;
    if (!beginLiquifyStroke()) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels) {
        cancelLiquifyStroke();
        return false;
    }
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    liquifyMesh_ = pittore::compute::make_warp_mesh(w, h);
    liquifyMask_.assign(static_cast<std::size_t>(w) * h, 0.0f);
    liquifyMaskW_ = w;
    liquifyMaskH_ = h;
    liquifySession_ = true;
    liquifySessionMoved_ = false;
    liquifyMeshStrength_ = 100.0f;
    liquifyHasClone_ = false;
    return true;
}

void AppState::endLiquifySession() {
    endLiquifyStroke();
    liquifySession_ = false;
    liquifySessionMoved_ = false;
    liquifyMask_.clear();
    liquifyMaskW_ = 0;
    liquifyMaskH_ = 0;
    liquifyHasClone_ = false;
    liquifyMeshStrength_ = 100.0f;
}

void AppState::cancelLiquifySession() {
    cancelLiquifyStroke();
    liquifySession_ = false;
    liquifySessionMoved_ = false;
    liquifyMask_.clear();
    liquifyMaskW_ = 0;
    liquifyMaskH_ = 0;
    liquifyHasClone_ = false;
    liquifyMeshStrength_ = 100.0f;
    liquifyMesh_ = pittore::compute::WarpMesh();
}

float AppState::liquifyMaskAt(float x, float y) const {
    if (liquifyMask_.empty() || liquifyMaskW_ < 2 || liquifyMaskH_ < 2)
        return 0.0f;
    const float fx = std::clamp(x, 0.0f, static_cast<float>(liquifyMaskW_ - 1));
    const float fy = std::clamp(y, 0.0f, static_cast<float>(liquifyMaskH_ - 1));
    const auto c0 = static_cast<std::uint32_t>(fx);
    const auto r0 = static_cast<std::uint32_t>(fy);
    const auto c1 = std::min(c0 + 1, liquifyMaskW_ - 1);
    const auto r1 = std::min(r0 + 1, liquifyMaskH_ - 1);
    const float tx = fx - c0;
    const float ty = fy - r0;
    const float a = liquifyMask_[static_cast<std::size_t>(r0) * liquifyMaskW_ + c0];
    const float b = liquifyMask_[static_cast<std::size_t>(r0) * liquifyMaskW_ + c1];
    const float c = liquifyMask_[static_cast<std::size_t>(r1) * liquifyMaskW_ + c0];
    const float d = liquifyMask_[static_cast<std::size_t>(r1) * liquifyMaskW_ + c1];
    return a + (b - a) * tx + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

void AppState::paintLiquifyMask(float cx, float cy, float radius,
                                float hardness, bool freeze, float opacity) {
    if (liquifyMask_.empty() || radius <= 0.0f || opacity <= 0.0f) return;
    const float core = std::clamp(hardness, 0.0f, 1.0f);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int x1 = std::min(static_cast<int>(liquifyMaskW_),
                            static_cast<int>(std::ceil(cx + radius)) + 1);
    const int y1 = std::min(static_cast<int>(liquifyMaskH_),
                            static_cast<int>(std::ceil(cy + radius)) + 1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float d = std::hypot(static_cast<float>(x) - cx,
                                       static_cast<float>(y) - cy);
            if (d >= radius) continue;
            const float t = d / radius;
            float f = 1.0f;
            if (t > core && core < 1.0f) {
                const float u = 1.0f - (t - core) / (1.0f - core);
                f = u * u * (3.0f - 2.0f * u);
            }
            float& m = liquifyMask_[static_cast<std::size_t>(y) * liquifyMaskW_ + x];
            const float w = f * opacity;
            m = freeze ? std::max(m, w) : std::min(m, 1.0f - w);
        }
    }
}

bool AppState::liquifyMaskPresent() const {
    for (float v : liquifyMask_) {
        if (v > 0.001f) return true;
    }
    return false;
}

void AppState::clearLiquifyMask() {
    std::fill(liquifyMask_.begin(), liquifyMask_.end(), 0.0f);
}

void AppState::fillLiquifyMask(float v) {
    std::fill(liquifyMask_.begin(), liquifyMask_.end(),
              std::clamp(v, 0.0f, 1.0f));
}

void AppState::invertLiquifyMask() {
    for (float& v : liquifyMask_) v = 1.0f - v;
}

bool AppState::liquifyMaskFromSelection() {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->pixels || liquifyMask_.empty()) return false;
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    bool any = false;
    for (std::uint32_t y = 0; y < liquifyMaskH_; ++y) {
        for (std::uint32_t x = 0; x < liquifyMaskW_; ++x) {
            const double dx = layer->offset.x() + (x + 0.5) * lsx;
            const double dy = layer->offset.y() + (y + 0.5) * lsy;
            float v = 0.0f;
            if (d->selectionIsMask && !d->selectionMask.isNull()) {
                const int sx = std::clamp(static_cast<int>(dx), 0,
                                          d->selectionMask.width() - 1);
                const int sy = std::clamp(static_cast<int>(dy), 0,
                                          d->selectionMask.height() - 1);
                v = qGray(d->selectionMask.pixel(sx, sy)) / 255.0f;
            } else if (!d->selection.isEmpty() && d->selection.contains(dx, dy)) {
                v = 1.0f;
            }
            if (v > 0.0f) any = true;
            liquifyMask_[static_cast<std::size_t>(y) * liquifyMaskW_ + x] =
                std::max(liquifyMask_[static_cast<std::size_t>(y) * liquifyMaskW_ + x], v);
        }
    }
    return any;
}

bool AppState::liquifyMirrorDab(float cx, float cy, float radius,
                                float angle, bool invert, float strength,
                                float falloffExp) {
    DocumentItem* d = activeDocument();
    LayerItem* layer = activeLayer();
    if (!d || !layer || !layer->pixels || !liquifySrc_) return false;
    if (radius <= 0.0f || strength <= 0.0f) return false;
    const std::uint32_t w = layer->pixels->width();
    const std::uint32_t h = layer->pixels->height();
    float ux = std::cos(angle);
    float uy = std::sin(angle);
    if (invert) {
        const float tx = ux;
        ux = -uy;
        uy = tx;
    }
    const auto sample = [&](float x, float y) {
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float tx = x - x0;
        const float ty = y - y0;
        float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
        const int xs[2] = {x0, x0 + 1};
        const int ys[2] = {y0, y0 + 1};
        const float ws[2] = {1.0f - tx, tx};
        const float hs[2] = {1.0f - ty, ty};
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const float wt = ws[i] * hs[j];
                if (wt <= 0.0f) continue;
                if (xs[i] < 0 || ys[j] < 0 || xs[i] >= static_cast<int>(w) ||
                    ys[j] >= static_cast<int>(h))
                    continue;
                const pittore::RGBAf& p =
                    layer->pixels->at(static_cast<std::uint32_t>(xs[i]),
                                      static_cast<std::uint32_t>(ys[j]));
                ar += p.r * p.a * wt;
                ag += p.g * p.a * wt;
                ab += p.b * p.a * wt;
                aa += p.a * wt;
            }
        }
        if (aa <= 1e-6f) return pittore::RGBAf{0, 0, 0, 0};
        return pittore::RGBAf{ar / aa, ag / aa, ab / aa, aa};
    };
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int x1 = std::min(static_cast<int>(w),
                            static_cast<int>(std::ceil(cx + radius)) + 1);
    const int y1 = std::min(static_cast<int>(h),
                            static_cast<int>(std::ceil(cy + radius)) + 1);
    bool changed = false;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dist = std::hypot(dx, dy);
            if (dist >= radius) continue;
            const float t = 1.0f - dist / radius;
            const float base = t * t * (3.0f - 2.0f * t);
            const float wt = std::pow(std::max(base, 0.0f), falloffExp) *
                             strength * (1.0f - liquifyMaskAt(static_cast<float>(x),
                                                             static_cast<float>(y)));
            if (wt <= 0.0f) continue;
            const float proj = dx * ux + dy * uy;
            const float qx = cx + proj * ux - (dx - proj * ux);
            const float qy = cy + proj * uy - (dy - proj * uy);
            const pittore::RGBAf s = sample(qx, qy);
            pittore::RGBAf& dst = layer->pixels->at(static_cast<std::uint32_t>(x),
                                                     static_cast<std::uint32_t>(y));
            const float da = dst.a;
            const float outA = da * (1.0f - wt) + s.a * wt;
            if (outA > 1e-6f) {
                dst.r = (dst.r * da * (1.0f - wt) + s.r * s.a * wt) / outA;
                dst.g = (dst.g * da * (1.0f - wt) + s.g * s.a * wt) / outA;
                dst.b = (dst.b * da * (1.0f - wt) + s.b * s.a * wt) / outA;
            } else {
                dst.r = dst.g = dst.b = 0.0f;
            }
            dst.a = outA;
            changed = true;
        }
    }
    if (!changed) return false;
    d->refreshPlacedRegion(*layer, QRect(x0, y0, x1 - x0, y1 - y0));
    refreshLayerCmyk(*d, *layer, QRect(x0, y0, x1 - x0, y1 - y0));
    layer->thumbnail = QImage();
    const double lsx = std::max(layer->scaleX, 1e-6);
    const double lsy = std::max(layer->scaleY, 1e-6);
    const QRect dabRect(
        static_cast<int>(std::floor(layer->offset.x() + x0 * lsx)) - 1,
        static_cast<int>(std::floor(layer->offset.y() + y0 * lsy)) - 1,
        (x1 - x0) + 3, (y1 - y0) + 3);
    const QRect clipped = dabRect.intersected(QRect(QPoint(0, 0), d->size));
    d->paintDirty =
        d->paintDirty.isNull() ? clipped : d->paintDirty.united(clipped);
    if (liquifySession_) liquifySessionMoved_ = true;
    return true;
}

bool AppState::liquifyCloneSource(float x, float y) {
    liquifyClone_ = QPointF(x, y);
    liquifyHasClone_ = true;
    return true;
}

void AppState::restoreLiquifyMask(std::vector<float> m) {
    if (m.size() != liquifyMask_.size()) return;
    liquifyMask_ = std::move(m);
}

bool AppState::liquifyMaskCombine(const std::vector<float>& src, int op) {
    if (liquifyMask_.empty() || src.size() != liquifyMask_.size()) return false;
    if (op == 4) {
        invertLiquifyMask();
        return true;
    }
    for (std::size_t i = 0; i < liquifyMask_.size(); ++i) {
        const float s = std::clamp(src[i], 0.0f, 1.0f);
        float& m = liquifyMask_[i];
        if (op == 0) m = s;
        else if (op == 1) m = std::max(m, s);
        else if (op == 2) m = m * (1.0f - s);
        else if (op == 3) m = m * s;
    }
    return true;
}

bool AppState::renderLiquifyFull() {
    if (!liquifySession_ || liquifyMesh_.cols < 2) return false;
    LayerItem* layer = activeLayer();
    if (!layer || !layer->pixels) return false;
    const int w = static_cast<int>(layer->pixels->width());
    const int h = static_cast<int>(layer->pixels->height());
    if (!liquifyDab(liquifyMesh_, 0, 0, w, h)) return false;
    flushPaint();
    return true;
}

void AppState::scaleLiquifyMesh(float f) {
    if (!liquifySession_) return;
    for (float& v : liquifyMesh_.offsets) v *= f;
}

void AppState::resetLiquifyMesh() {
    if (!liquifySession_) return;
    if (!liquifyMesh_.identity()) {
        std::fill(liquifyMesh_.offsets.begin(), liquifyMesh_.offsets.end(),
                  0.0f);
        liquifyMeshStrength_ = 100.0f;
        if (renderLiquifyFull()) liquifySessionMoved_ = true;
    } else {
        liquifyMeshStrength_ = 100.0f;
    }
}

void AppState::setLiquifyMesh(pittore::compute::WarpMesh mesh) {
    liquifyMesh_ = std::move(mesh);
    liquifyMeshStrength_ = 100.0f;
}

bool AppState::saveLiquifyMesh(const QString& path) const {
    if (!liquifySession_ || liquifyMesh_.cols < 2) return false;
    return pittore::compute::warp_mesh_write(path.toStdString(),
                                              liquifyMesh_);
}

bool AppState::loadLiquifyMesh(const QString& path) {
    if (!liquifySession_) return false;
    pittore::compute::WarpMesh tmp;
    if (!pittore::compute::warp_mesh_read(path.toStdString(), tmp))
        return false;
    if (tmp.cols != liquifyMesh_.cols || tmp.rows != liquifyMesh_.rows)
        return false;
    liquifyMesh_ = std::move(tmp);
    liquifyMeshStrength_ = 100.0f;
    if (renderLiquifyFull()) liquifySessionMoved_ = true;
    return true;
}

void AppState::requestLiquifySaveMesh() {
    emit liquifySaveMeshRequested();
}

void AppState::requestLiquifyLoadMesh() {
    emit liquifyLoadMeshRequested();
}

}  // namespace pittore::ui
