// HIP compute backend — the AMD native path (Pittore.md §9 / §0).
// HIP mirrors CUDA's API and syntax nearly 1:1, so this file is a direct port
// of cuda_backend.cu (which the kernel-parity tests share). It compiles only
// when ROCm/hipcc is present (-Dbackend-hip=enabled); on machines without an
// AMD GPU the backend reports zero devices and is never selected.
//
// This machine has no ROCm, so this file is NOT part of a default build — it
// is kept in lockstep with the CUDA backend and MUST be built+tested on AMD
// hardware before relying on it.

#include "engine/compute/hip_backend.h"

#if defined(PITTORE_HAS_HIP)

#include <hip/hip_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <typeinfo>

#include "engine/core/log.h"
#include "engine/core/pixel.h"
#include "engine/compute/adjust.h"
#include "engine/compute/dither.h"
#include "engine/compute/oklab.h"
#include "engine/compute/backend.h"

namespace pittore::compute {
namespace {

void check(hipError_t err, const char* what) {
    if (err != hipSuccess)
        throw std::runtime_error(std::string(what) + ": " + hipGetErrorString(err));
}

void check_nothrow(hipError_t err, const char* what) {
    if (err != hipSuccess)
        std::fprintf(stderr, "pittore-hip: %s: %s\n", what, hipGetErrorString(err));
}

constexpr std::size_t kBlock = 256;

constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

__global__ void k_grayscale(const float4* src, float4* dst, std::size_t n) {
    const std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float4 s = src[i];
    const float y = kLumaR * s.x + kLumaG * s.y + kLumaB * s.z;
    dst[i] = make_float4(y, y, y, s.w);
}

// Mirrors the host blend::pixel exactly, so every kernel variant (full, region,
// placed) blends bit-for-bit like the CPU reference. `mode` is a
// static_cast<int>(BlendMode); x/y are document coordinates (Dissolve reads
// them; every other mode ignores them).
__device__ __forceinline__ float4 k_blend_composite(float4 s, float4 d,
                                                    int mode, int x, int y) {
    float cr, cg, cb, ca;
    blend::pixel(static_cast<BlendMode>(mode), s.x, s.y, s.z, s.w,
                 d.x, d.y, d.z, d.w, x, y, cr, cg, cb, ca);
    return make_float4(cr, cg, cb, ca);
}

__global__ void k_composite(float4* dst, const float4* src, std::size_t n,
                            int w, int mode) {
    const std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    dst[i] = k_blend_composite(src[i], dst[i], mode, static_cast<int>(i % w),
                               static_cast<int>(i / w));
}

// Region-limited composite: identical blend math, applied only to the pixels
// [x0,x1) × [y0,y1) of a `w`-wide row-major image. The incremental repaint
// path a brush uses when only a small capsule changed.
__global__ void k_composite_region(float4* dst, const float4* src, int w, int x0,
                                   int y0, int x1, int y1, int mode) {
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= rw * rh) return;
    const int lx = i % rw;
    const int ly = i / rw;
    const std::size_t idx = static_cast<std::size_t>(y0 + ly) * w + (x0 + lx);
    dst[idx] = k_blend_composite(src[idx], dst[idx], mode, x0 + lx, y0 + ly);
}

// Device mirror of the host PremulAcc accumulator (see cuda_backend.cu /
// paint.cpp): premultiplied double-space taps, unpremultiplied once at the
// end, so placed-layer samples match the CPU reference bit-for-bit.
struct KPremulAcc {
    // Single-precision accumulation: matches the CPU double reference to
    // ~1e-7 (values in [0,1]), inside every parity tolerance, at full
    // float throughput (fp64 is 1/64 rate on consumer GPUs).
    float r = 0, g = 0, b = 0, a = 0, wsum = 0;

    __device__ void add(const float4* src, int sw, int sh, long long x,
                        long long y, double w) {
        const float wf = static_cast<float>(w);
        wsum += wf;  // total weight counts even outside taps (transparency),
        if (w == 0.0 || x < 0 || y < 0 || x >= sw || y >= sh)
            return;  // ...so a half-covered pixel gets half alpha, not full.
        const float4 s = src[static_cast<std::size_t>(y) * sw + x];
        r += s.x * s.w * wf;
        g += s.y * s.w * wf;
        b += s.z * s.w * wf;
        a += s.w * wf;
    }
    __device__ float4 done() const {
        if (wsum <= 0.0f || a <= 0.0f) return make_float4(0, 0, 0, 0);
        const float inv = 1.0f / wsum;
        const float aa = a * inv;
        if (aa <= 0.0f) return make_float4(0, 0, 0, 0);
        return make_float4((r * inv) / aa,
                           (g * inv) / aa,
                           (b * inv) / aa, aa);
    }
};

// Mirrors sample_placed_host: bilinear on upscale, box-average on downscale.
// doc = offset + scale·src; integer coords return the exact texel, so a pure
// translation samples identically to a memcpy blit.
__device__ float4 k_sample_placed(const float4* src, int sw, int sh,
                                  double docX, double docY, double ox,
                                  double oy, double sx, double sy) {
    const double fx = (docX - ox) / sx;
    const double fy = (docY - oy) / sy;

    if (sx >= 1.0 && sy >= 1.0) {
        // Upscale (or 1:1): bilinear over the 4 surrounding texels.
        if (fx < -1.0 || fy < -1.0 || fx > sw || fy > sh)
            return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        const long long x0 = static_cast<long long>(floor(fx));
        const long long y0 = static_cast<long long>(floor(fy));
        const double tx = fx - x0, ty = fy - y0;
        KPremulAcc acc;
        acc.add(src, sw, sh, x0, y0, (1 - tx) * (1 - ty));
        acc.add(src, sw, sh, x0 + 1, y0, tx * (1 - ty));
        acc.add(src, sw, sh, x0, y0 + 1, (1 - tx) * ty);
        acc.add(src, sw, sh, x0 + 1, y0 + 1, tx * ty);
        return acc.done();
    }

    // Downscale: box-average the covered source footprint, taps capped at 24
    // per axis (see paint.cpp).
    const double spanX = 1.0 / sx;
    const double spanY = 1.0 / sy;
    if (fx + spanX < 0.0 || fy + spanY < 0.0 || fx > sw || fy > sh)
        return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    constexpr int kMaxTaps = 24;
    const int nx = max(1, min(kMaxTaps, static_cast<int>(ceil(spanX))));
    const int ny = max(1, min(kMaxTaps, static_cast<int>(ceil(spanY))));
    KPremulAcc acc;
    for (int j = 0; j < ny; ++j) {
        const double tapY =
            (ny == 1) ? fy + spanY * 0.5 : fy + spanY * (j + 0.5) / ny;
        const long long yy = static_cast<long long>(floor(tapY));
        for (int i = 0; i < nx; ++i) {
            const double tapX =
                (nx == 1) ? fx + spanX * 0.5 : fx + spanX * (i + 0.5) / nx;
            acc.add(src, sw, sh, static_cast<long long>(floor(tapX)), yy, 1.0);
        }
    }
    return acc.done();
}

struct KPlacedParam {
    int sw, sh;           // layer native size
    int w;                // document row stride (accumulator)
    int x0, y0, x1, y1;   // region in document pixels
    double ox, oy, sx, sy;
    float fold;           // opacity × fill folded into sampled alpha
    int mode;             // 0 = Normal, 1 = Multiply
};

// Opaque-grey mask coverage (see layer_mask.h): the sampler averages
// premultiplied taps, so R is the correctly weighted mean. A null mask
// reveals everything.
__device__ float k_mask_coverage(const float4* mask, int msw, int msh,
                                 double docX, double docY, double mox,
                                 double moy, double msx, double msy) {
    if (!mask || msw <= 0 || msh <= 0 || msx <= 0.0 || msy <= 0.0)
        return 1.0f;
    const float4 m =
        k_sample_placed(mask, msw, msh, docX, docY, mox, moy, msx, msy);
    return m.x < 0.0f ? 0.0f : (m.x > 1.0f ? 1.0f : m.x);
}

// Fused placed-layer composite: sample → fold alpha → blend straight into the
// accumulator, one kernel in place of host staging + a separate composite.
__global__ void k_composite_placed(const float4* src, float4* dst, KPlacedParam p) {
    const int rw = p.x1 - p.x0;
    const int rh = p.y1 - p.y0;
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= rw * rh) return;
    const int lx = i % rw;
    const int ly = i / rw;
    const std::size_t idx = static_cast<std::size_t>(p.y0 + ly) * p.w + (p.x0 + lx);
    // 1:1 doc-aligned layer (the identity background): the sampler would return
    // the exact source texel, so read it directly — no coordinate math, no taps.
    // `sw == w && sh >= y1` proves every region pixel lands on a real source
    // texel of a w-wide buffer, so the flat `idx` read is in bounds.
    float4 s;
    if (p.sx == 1.0 && p.sy == 1.0 && p.ox == 0.0 && p.oy == 0.0 &&
        p.sw == p.w && p.sh >= p.y1) {
        s = src[idx];
    } else {
        s = k_sample_placed(src, p.sw, p.sh, static_cast<double>(p.x0 + lx),
                            static_cast<double>(p.y0 + ly), p.ox, p.oy, p.sx,
                            p.sy);
    }
    s.w *= p.fold;
    dst[idx] = k_blend_composite(s, dst[idx], p.mode, p.x0 + lx, p.y0 + ly);
}

// Batched k_composite_placed: every layer in one launch. The host hands over a
// compacted param array (empty windows dropped) plus, per document row, the
// bottom→top list of layer cells that cross that row. Threads own destination
// pixels — never layers — so a pixel covered by several layers is blended by
// exactly ONE thread walking its row list in layer order. Layers whose stamp
// is a pixel-aligned 1:1 translation take a direct source read instead of
// running the bilinear sampler, which would collapse to the very same texel.
struct KPlacedManyParam {
    const float4* src;
    int sw, sh, w;
    int x0, y0, x1, y1;
    double ox, oy, sx, sy;
    float fold;
    int mode;
    const float4* mask;
    int msw, msh;
    double mox, moy, msx, msy;
    int clipped;
    int clipBase;
    int isAdjustment;
    int adjKind;
    float adjP[16];
    const float* adjAux;
};

// One layer's footprint on one document row. Cells are appended bottom→top per
// row by the host, so scanning them in order IS the layer stack.
struct KRowCell {
    int x0, x1;   // column span [x0, x1) on this row
    int layer;    // index into the param array
};

// Sample one cell of a batched layer at a document pixel, with the same
// identity/translation fast paths the row walk uses for its own source.
__device__ float4 k_sample_cell(const KPlacedManyParam& prm, int x, int y,
                                std::size_t idx) {
    if (prm.sx == 1.0 && prm.sy == 1.0 && prm.ox == 0.0 && prm.oy == 0.0 &&
        prm.sw == prm.w && prm.sh >= prm.y1) {
        return prm.src[idx];
    }
    if (prm.sx == 1.0 && prm.sy == 1.0 &&
        prm.ox == static_cast<double>(static_cast<int>(prm.ox)) &&
        prm.oy == static_cast<double>(static_cast<int>(prm.oy))) {
        const int sx = x - static_cast<int>(prm.ox);
        const int sy = y - static_cast<int>(prm.oy);
        if (sx >= 0 && sy >= 0 && sx < prm.sw && sy < prm.sh)
            return prm.src[static_cast<std::size_t>(sy) * prm.sw + sx];
        return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return k_sample_placed(prm.src, prm.sw, prm.sh, static_cast<double>(x),
                           static_cast<double>(y), prm.ox, prm.oy, prm.sx,
                           prm.sy);
}

// Run a live adjustment over the composite-so-far. The adjustment reads the
// accumulator colour, keeps its alpha for the usual mask/fold/clip pipeline
// in the caller, and blends with the layer's own blend mode.
__device__ float4 k_apply_adjustment(const KPlacedManyParam& prm, float4 acc) {
    float r, g, b;
    adjust::apply(static_cast<AdjustmentKind>(prm.adjKind), prm.adjP,
                  prm.adjAux, acc.x, acc.y, acc.z, r, g, b);
    return make_float4(r, g, b, acc.w);
}

// Post-mask/fold alpha of one layer at a document pixel: the coverage a
// clipped layer above it uses. Each clipped layer samples its own base, so a
// base that does not reach the pixel contributes nothing instead of leaking
// the coverage of some lower layer.
__device__ float k_base_coverage(const KPlacedManyParam* params,
                                 const KPlacedManyParam& prm, int x, int y,
                                 std::size_t idx) {
    if (!prm.clipped || prm.clipBase < 0) return 0.0f;
    const KPlacedManyParam bprm = params[prm.clipBase];
    float4 b = k_sample_cell(bprm, x, y, idx);
    b.w *= k_mask_coverage(bprm.mask, bprm.msw, bprm.msh,
                           static_cast<double>(x), static_cast<double>(y),
                           bprm.mox, bprm.moy, bprm.msx, bprm.msy);
    b.w *= bprm.fold;
    return b.w;
}

__global__ void k_composite_many_rows(const KPlacedManyParam* params,
                                      const int* rowStart,
                                      const KRowCell* cells, float4* dst,
                                      int w, int rx0, int ry0, int rw,
                                      int rh) {
    const int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= rw * rh) return;
    const int x = rx0 + t % rw;
    const int y = ry0 + t / rw;
    const int idx = y * w + x;
    const int b = rowStart[y];
    const int n = rowStart[y + 1] - b;
    float4 acc = dst[idx];  // cleared accumulator → transparent outside cells
    for (int k = 0; k < n; ++k) {
        const KRowCell c = cells[b + k];
        if (x < c.x0 || x >= c.x1) continue;
        const KPlacedManyParam prm = params[c.layer];
        float4 s;
        if (prm.isAdjustment) {
            s = k_apply_adjustment(prm, acc);
        } else {
            s = k_sample_cell(prm, x, y, static_cast<std::size_t>(idx));
        }
        s.w *= k_mask_coverage(prm.mask, prm.msw, prm.msh,
                               static_cast<double>(x), static_cast<double>(y),
                               prm.mox, prm.moy, prm.msx, prm.msy);
        s.w *= prm.fold;
        if (prm.clipped)
            s.w *= k_base_coverage(params, prm, x, y,
                                   static_cast<std::size_t>(idx));
        acc = k_blend_composite(s, acc, prm.mode, x, y);
    }
    dst[idx] = acc;
}

// Flat-stack fast path: every layer is plain Normal (no mask/clip/adjustment)
// and covers the region, so the per-pixel walk sheds the cell indirection,
// the ~150B param loads, the mode switch and all branches. Params ride in
// kernel arguments (constant cache), not global memory. Sampling reuses
// k_sample_placed for scaled layers; integer 1:1 layers take a direct read.
// The Normal blend is open-coded from blend::pixel (Bl(ackdrop)=Source):
//   ao = sa + da*(1-sa)  (always > 0: sa > 0 after the skip)
//   out = (s*sa + d*da*(1-sa)) / ao
// which matches the shared math to float rounding (~1e-7).
struct KFlatParam {
    const float4* src;
    int sw, sh;
    double ox, oy, sx, sy;  // kept double: identical taps to the generic walk
    float fold;
    int direct;  // 1:1 integer placement → direct texel read
    int iox, ioy;
    int x0, y0, x1, y1;  // doc-clipped window: out-of-window pixels skip the
                         // layer exactly like the generic cell walk does
};

constexpr int kFlatMax = 16;

// Resident in constant memory (broadcast to the whole launch): one small
// H2D per call instead of per-thread register file pressure. NOTE: an
// earlier revision passed this struct by value and a 1-layer full-frame
// Normal composite cost 10ms in spills — this is the fix.
__constant__ KFlatParam kFlatConst[kFlatMax];

__global__ void k_composite_flat_rows(float4* dst, int w, int rx0, int ry0,
                                      int rw, int rh, int n) {
    const int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= rw * rh) return;
    const int x = rx0 + t % rw;
    const int y = ry0 + t / rw;
    const std::size_t idx = static_cast<std::size_t>(y) * w + x;
    float4 acc = dst[idx];
    for (int j = 0; j < n; ++j) {
        const KFlatParam p = kFlatConst[j];
        if (x < p.x0 || x >= p.x1 || y < p.y0 || y >= p.y1) continue;
        float4 s;
        if (p.direct) {
            const int sx = x - p.iox;
            const int sy = y - p.ioy;
            if (sx < 0 || sy < 0 || sx >= p.sw || sy >= p.sh) continue;
            s = p.src[static_cast<std::size_t>(sy) * p.sw + sx];
        } else {
            s = k_sample_placed(p.src, p.sw, p.sh, static_cast<double>(x),
                                static_cast<double>(y), p.ox, p.oy, p.sx,
                                p.sy);
        }
        s.w *= p.fold;
        if (s.w <= 0.0f) continue;
        const float sa = s.w;
        const float da = acc.w;
        const float ao = sa + da * (1.0f - sa);
        const float inv = 1.0f / ao;
        const float trans = da * (1.0f - sa);
        acc.x = (s.x * sa + acc.x * trans) * inv;
        acc.y = (s.y * sa + acc.y * trans) * inv;
        acc.z = (s.z * sa + acc.z * trans) * inv;
        acc.w = ao;
    }
    dst[idx] = acc;
}

// Convert straight-alpha RGBAf to the premultiplied ARGB32 word QImage wants
// (byte order B,G,R,A on little-endian, i.e. 0xAARRGGBB). Ordered Bayer
// dither on absolute coords, shared with the host loop, so all backends
// agree bit-for-bit; exact integers pass through.
__global__ void k_premul_argb(const float4* src, unsigned int* dst, int w,
                              int x0, int y0, int rw, int rh) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= rw * rh) return;
    const int lx = i % rw;
    const int ly = i / rw;
    const float4 s = src[static_cast<std::size_t>(y0 + ly) * w + (x0 + lx)];
    const unsigned int ax = static_cast<unsigned int>(x0 + lx);
    const unsigned int ay = static_cast<unsigned int>(y0 + ly);
    const unsigned int ai =
        pittore::compute::dither::quantize(s.w, ax, ay);
    const unsigned int ri =
        pittore::compute::dither::quantize(s.x * s.w, ax, ay);
    const unsigned int gi =
        pittore::compute::dither::quantize(s.y * s.w, ax, ay);
    const unsigned int bi =
        pittore::compute::dither::quantize(s.z * s.w, ax, ay);
    dst[i] = bi | (gi << 8) | (ri << 16) | (ai << 24);
}

// bg_erase: discontiguous tolerance erase under the dab mask. Mirrors
// background_erase_dab_host exactly (max-channel metric, tolerance ramp,
// linear rim by hardness, double sqrt included). One thread per bbox pixel.
__global__ void k_bg_erase(float4* dst, int w, int x0, int y0, int bw,
                           int bh, float cx, float cy, float inv_r,
                           float hardness, float opacity, float sr, float sg,
                           float sb, float tol, int protectFg, float fr,
                           float fg, float fb) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= bw * bh) return;
    const int x = x0 + (i % bw);
    const int y = y0 + (i / bw);
    const float dx = static_cast<float>(x) + 0.5f - cx;
    const float dy = static_cast<float>(y) + 0.5f - cy;
    const float t = static_cast<float>(
        sqrt(static_cast<double>(dx * dx + dy * dy)) *
        static_cast<double>(inv_r));
    if (t >= 1.0f) return;
    float hard = hardness < 0.0f ? 0.0f : (hardness > 1.0f ? 1.0f : hardness);
    float cov = 1.0f;
    if (t > hard) {
        float span = 1.0f - hard;
        if (span < 1e-4f) span = 1e-4f;
        cov = (1.0f - t) / span;
    }
    if (!(cov > 0.0f)) return;
    float4 d = dst[static_cast<std::size_t>(y) * w + x];
    float dr = d.x > sr ? d.x - sr : sr - d.x;
    float dg = d.y > sg ? d.y - sg : sg - d.y;
    float db = d.z > sb ? d.z - sb : sb - d.z;
    float dist = dr > dg ? dr : dg;
    dist = dist > db ? dist : db;
    float f;
    if (tol <= 0.0f) {
        f = dist <= 1e-6f ? 1.0f : 0.0f;
    } else {
        if (dist > tol) return;
        f = (tol - dist) / (tol + 1e-6f);
    }
    if (protectFg) {
        float pr = d.x > fr ? d.x - fr : fr - d.x;
        float pg = d.y > fg ? d.y - fg : fg - d.y;
        float pb = d.z > fb ? d.z - fb : fb - d.z;
        float pd = pr > pg ? pr : pg;
        pd = pd > pb ? pd : pb;
        if (pd <= tol) return;
    }
    const float a = cov * f * opacity;
    if (!(a > 0.0f)) return;
    d.w *= (1.0f - a);
    dst[static_cast<std::size_t>(y) * w + x] = d;
}

// pattern_stamp: source-over the 64x64 procedural tile under the dab
// mask. Mirrors pattern_stamp_dab_host exactly (nearest tile texel,
// same mask geometry and op order, double sqrt included). One thread
// per bbox pixel.
__global__ void k_pattern_stamp(float4* dst, int w, int x0, int y0, int bw,
                                int bh, float cx, float cy, float inv_r,
                                float hardness, float opacity,
                                const float4* tile, float ox, float oy) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= bw * bh) return;
    const int x = x0 + (i % bw);
    const int y = y0 + (i / bw);
    const float dx = static_cast<float>(x) + 0.5f - cx;
    const float dy = static_cast<float>(y) + 0.5f - cy;
    const float t = static_cast<float>(
        sqrt(static_cast<double>(dx * dx + dy * dy)) *
        static_cast<double>(inv_r));
    if (t >= 1.0f) return;
    float hard = hardness < 0.0f ? 0.0f : (hardness > 1.0f ? 1.0f : hardness);
    float cov = 1.0f;
    if (t > hard) {
        float span = 1.0f - hard;
        if (span < 1e-4f) span = 1e-4f;
        cov = (1.0f - t) / span;
    }
    const float a = cov * opacity;
    if (!(a > 0.0f)) return;
    int tx = static_cast<int>(floorf(static_cast<float>(x) - ox)) % 64;
    int ty = static_cast<int>(floorf(static_cast<float>(y) - oy)) % 64;
    if (tx < 0) tx += 64;
    if (ty < 0) ty += 64;
    const float4 s = tile[ty * 64 + tx];
    float4 d = dst[static_cast<std::size_t>(y) * w + x];
    const float inv_a = 1.0f - a;
    const float out_a = a + d.w * inv_a;
    if (out_a > 0.0f) {
        d.x = (a * s.x + d.x * d.w * inv_a) / out_a;
        d.y = (a * s.y + d.y * d.w * inv_a) / out_a;
        d.z = (a * s.z + d.z * d.w * inv_a) / out_a;
        d.w = out_a;
        dst[static_cast<std::size_t>(y) * w + x] = d;
    }
}

// history_dab: source-over snapshot texels under the dab mask. Mirrors
// history_brush_dab_host exactly (same rim geometry, op order, double
// sqrt included). One thread per bbox pixel.
__global__ void k_history_dab(float4* dst, const float4* src, int w, int x0,
                              int y0, int bw, int bh, float cx, float cy,
                              float inv_r, float hardness, float opacity) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= bw * bh) return;
    const int x = x0 + (i % bw);
    const int y = y0 + (i / bw);
    const float dx = static_cast<float>(x) + 0.5f - cx;
    const float dy = static_cast<float>(y) + 0.5f - cy;
    const float t = static_cast<float>(
        sqrt(static_cast<double>(dx * dx + dy * dy)) *
        static_cast<double>(inv_r));
    if (t >= 1.0f) return;
    float hard = hardness < 0.0f ? 0.0f : (hardness > 1.0f ? 1.0f : hardness);
    float cov = 1.0f;
    if (t > hard) {
        float span = 1.0f - hard;
        if (span < 1e-4f) span = 1e-4f;
        cov = (1.0f - t) / span;
    }
    const float a = cov * opacity;
    if (!(a > 0.0f)) return;
    const std::size_t idx = static_cast<std::size_t>(y) * w + x;
    const float4 s = src[idx];
    float4 d = dst[idx];
    const float inv_a = 1.0f - a;
    const float out_a = a + d.w * inv_a;
    if (out_a > 0.0f) {
        d.x = (a * s.x + d.x * d.w * inv_a) / out_a;
        d.y = (a * s.y + d.y * d.w * inv_a) / out_a;
        d.z = (a * s.z + d.z * d.w * inv_a) / out_a;
        d.w = out_a;
        dst[idx] = d;
    }
}

// paint_dab: one round stamp, straight-alpha source-over. Mirrors
// paint_dab_host exactly (same types and op order, double sqrt included)
// so CPU/GPU strokes agree bit-for-bit. One thread per bbox pixel; no
// full-frame traffic unlike the base-class download+dab+upload fallback.
__global__ void k_paint_dab(float4* dst, int w, int x0, int y0, int bw, int bh,
                            float cx, float cy, float inv_r, float soft_span,
                            float op, float cr, float cg, float cb) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= bw * bh) return;
    const int x = x0 + (i % bw);
    const int y = y0 + (i / bw);
    const float dx = static_cast<float>(x) + 0.5f - cx;
    const float dy = static_cast<float>(y) + 0.5f - cy;
    const float t = static_cast<float>(
        sqrt(static_cast<double>(dx * dx + dy * dy)) *
        static_cast<double>(inv_r));
    if (t >= 1.0f) return;
    float cov = 1.0f;
    if (t > 1.0f - soft_span) cov = (1.0f - t) / soft_span;
    const float a = cov * op;
    if (a <= 0.0f) return;
    float4 d = dst[static_cast<std::size_t>(y) * w + x];
    const float inv_a = 1.0f - a;
    const float out_a = a + d.w * inv_a;
    if (out_a > 0.0f) {
        d.x = (a * cr + d.x * d.w * inv_a) / out_a;
        d.y = (a * cg + d.y * d.w * inv_a) / out_a;
        d.z = (a * cb + d.z * d.w * inv_a) / out_a;
        d.w = out_a;
        dst[static_cast<std::size_t>(y) * w + x] = d;
    }
}

__global__ void k_blur_h(const float4* src, float4* dst, int w, int h,
                         const float* kernel, int r) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const int high = w - 1;
    const int row = y * w;
    float4 acc = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = -r; j <= r; ++j) {
        const int xx = min(max(x + j, 0), high);
        const float4 p = src[row + xx];
        const float k = kernel[j + r];
        acc.x += k * p.x;
        acc.y += k * p.y;
        acc.z += k * p.z;
        acc.w += k * p.w;
    }
    dst[row + x] = acc;
}

__global__ void k_blur_v(const float4* src, float4* dst, int w, int h,
                         const float* kernel, int r) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const int high = h - 1;
    float4 acc = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = -r; j <= r; ++j) {
        const int yy = min(max(y + j, 0), high);
        const float4 p = src[yy * w + x];
        const float k = kernel[j + r];
        acc.x += k * p.x;
        acc.y += k * p.y;
        acc.z += k * p.z;
        acc.w += k * p.w;
    }
    dst[y * w + x] = acc;
}

__global__ void k_sharpen(const float4* src, const float4* blurred,
                          float4* dst, std::size_t n, float amount,
                          float threshold) {
    const std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float4 s = src[i];
    const float4 b = blurred[i];
    float4 out = s;
    float d = ((s.x - b.x) + (s.y - b.y) + (s.z - b.z)) / 3.0f;
    if (fabsf(d) >= threshold) {
        out.x = s.x + amount * (s.x - b.x);
        out.y = s.y + amount * (s.y - b.y);
        out.z = s.z + amount * (s.z - b.z);
    }
    out.x = fminf(fmaxf(out.x, 0.0f), 1.0f);
    out.y = fminf(fmaxf(out.y, 0.0f), 1.0f);
    out.z = fminf(fmaxf(out.z, 0.0f), 1.0f);
    dst[i] = out;
}

__global__ void k_brightness_contrast(const float4* src, float4* dst,
                                      std::size_t n, float brightness,
                                      float contrast) {
    const std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float4 s = src[i];
    float4 out;
    out.x = (s.x + brightness) * (1.0f + contrast);
    out.y = (s.y + brightness) * (1.0f + contrast);
    out.z = (s.z + brightness) * (1.0f + contrast);
    out.x = fminf(fmaxf(out.x, 0.0f), 1.0f);
    out.y = fminf(fmaxf(out.y, 0.0f), 1.0f);
    out.z = fminf(fmaxf(out.z, 0.0f), 1.0f);
    out.w = s.w;
    dst[i] = out;
}

__device__ float hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

__device__ float3 rgb_to_hsl(float3 rgb) {
    float cmax = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));
    float cmin = fminf(rgb.x, fminf(rgb.y, rgb.z));
    float l = (cmax + cmin) * 0.5f;
    if (cmax == cmin) return make_float3(0.0f, 0.0f, l);
    float d = cmax - cmin;
    float s = d / (1.0f - fabsf(2.0f * l - 1.0f));
    float h;
    if (cmax == rgb.x) h = fmodf((rgb.y - rgb.z) / d + 6.0f, 6.0f);
    else if (cmax == rgb.y) h = (rgb.z - rgb.x) / d + 2.0f;
    else h = (rgb.x - rgb.y) / d + 4.0f;
    h /= 6.0f;
    return make_float3(h, s, l);
}

__device__ float3 hsl_to_rgb(float3 hsl) {
    float h = hsl.x, s = hsl.y, l = hsl.z;
    if (s == 0.0f) return make_float3(l, l, l);
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    float3 rgb;
    rgb.x = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    rgb.y = hue_to_rgb(p, q, h);
    rgb.z = hue_to_rgb(p, q, h - 1.0f / 3.0f);
    return rgb;
}

__global__ void k_hue_saturation(const float4* src, float4* dst, std::size_t n,
                                 float hue_shift, float sat_adj, float light_adj) {
    const std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float4 s = src[i];
    float3 hsl = rgb_to_hsl(make_float3(s.x, s.y, s.z));
    hsl.x = fmodf(hsl.x + hue_shift / 360.0f + 1.0f, 1.0f);
    hsl.y = fminf(fmaxf(hsl.y * (1.0f + sat_adj), 0.0f), 1.0f);
    hsl.z = fminf(fmaxf(hsl.z + light_adj, 0.0f), 1.0f);
    float3 rgb = hsl_to_rgb(hsl);
    dst[i] = make_float4(rgb.x, rgb.y, rgb.z, s.w);
}

__global__ void k_median3x3(const float4* src, float4* dst, int w, int h) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    float vals[4][9];
    int idx = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int xx = min(max(x + dx, 0), w - 1);
            int yy = min(max(y + dy, 0), h - 1);
            const float4 p = src[yy * w + xx];
            vals[0][idx] = p.x;
            vals[1][idx] = p.y;
            vals[2][idx] = p.z;
            vals[3][idx] = p.w;
            ++idx;
        }
    }
    for (int c = 0; c < 4; ++c) {
        for (int i = 1; i < 9; ++i) {
            float key = vals[c][i];
            int j = i - 1;
            while (j >= 0 && vals[c][j] > key) { vals[c][j + 1] = vals[c][j]; --j; }
            vals[c][j + 1] = key;
        }
    }
    dst[y * w + x] = make_float4(vals[0][4], vals[1][4], vals[2][4], vals[3][4]);
}


__global__ void k_box_h(const float4* src, float4* dst, int w, int h, int r) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    const int high = w - 1;
    const int row = y * w;
    const float inv = 1.0f / float(2 * r + 1);
    float4 acc = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = -r; j <= r; ++j) {
        const float4 p = src[row + min(max(x + j, 0), high)];
        acc.x += p.x;
        acc.y += p.y;
        acc.z += p.z;
    }
    acc.x *= inv;
    acc.y *= inv;
    acc.z *= inv;
    acc.w = src[row + x].w;
    dst[row + x] = acc;
}

__global__ void k_box_v(const float4* src, float4* dst, int w, int h, int r) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    const int high = h - 1;
    const float inv = 1.0f / float(2 * r + 1);
    float4 acc = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = -r; j <= r; ++j) {
        const float4 p = src[min(max(y + j, 0), high) * w + x];
        acc.x += p.x;
        acc.y += p.y;
        acc.z += p.z;
    }
    acc.x *= inv;
    acc.y *= inv;
    acc.z *= inv;
    acc.w = src[y * w + x].w;
    dst[y * w + x] = acc;
}

__global__ void k_median_r(const float4* src, float4* dst, int w, int h, int r) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    float win[3][289];
    int n = 0;
    for (int dy = -r; dy <= r; ++dy) {
        const int yy = min(max(y + dy, 0), h - 1);
        for (int dx = -r; dx <= r; ++dx) {
            const int xx = min(max(x + dx, 0), w - 1);
            const float4 p = src[yy * w + xx];
            win[0][n] = p.x;
            win[1][n] = p.y;
            win[2][n] = p.z;
            ++n;
        }
    }
    const int mid = n / 2;
    float4 out = src[y * w + x];
    for (int c = 0; c < 3; ++c) {
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            float pivot = win[c][(lo + hi) / 2];
            int i = lo, j = hi;
            while (i <= j) {
                while (win[c][i] < pivot) ++i;
                while (win[c][j] > pivot) --j;
                if (i <= j) {
                    float t = win[c][i];
                    win[c][i] = win[c][j];
                    win[c][j] = t;
                    ++i;
                    --j;
                }
            }
            if (j < mid) lo = i;
            if (mid < i) hi = j;
        }
        if (c == 0) out.x = win[0][mid];
        else if (c == 1) out.y = win[1][mid];
        else out.z = win[2][mid];
    }
    dst[y * w + x] = out;
}

__global__ void k_unsharp_gate(const float4* src, const float4* blurred,
                               float4* dst, std::size_t n, float amount,
                               float threshold) {
    const std::size_t i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float4 s = src[i];
    const float4 b = blurred[i];
    float4 out = s;
    float dx = s.x - b.x;
    if (fabsf(dx) > threshold) out.x = fminf(fmaxf(s.x + amount * dx, 0.0f), 1.0f);
    float dy = s.y - b.y;
    if (fabsf(dy) > threshold) out.y = fminf(fmaxf(s.y + amount * dy, 0.0f), 1.0f);
    float dz = s.z - b.z;
    if (fabsf(dz) > threshold) out.z = fminf(fmaxf(s.z + amount * dz, 0.0f), 1.0f);
    dst[i] = out;
}

__device__ float4 k_motion_sample(const float4* src, int w, int h, float x, float y) {
    const int x0 = (int)floorf(x);
    const int y0 = (int)floorf(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const int xa = min(max(x0, 0), w - 1);
    const int xb = min(max(x0 + 1, 0), w - 1);
    const int ya = min(max(y0, 0), h - 1);
    const int yb = min(max(y0 + 1, 0), h - 1);
    const float4 a = src[ya * w + xa];
    const float4 b = src[ya * w + xb];
    const float4 c = src[yb * w + xa];
    const float4 d = src[yb * w + xb];
    const float iw = (1.0f - fx) * (1.0f - fy);
    const float jw = fx * (1.0f - fy);
    const float kw = (1.0f - fx) * fy;
    const float lw = fx * fy;
    return make_float4(a.x * iw + b.x * jw + c.x * kw + d.x * lw,
                       a.y * iw + b.y * jw + c.y * kw + d.y * lw,
                       a.z * iw + b.z * jw + c.z * kw + d.z * lw,
                       a.w * iw + b.w * jw + c.w * kw + d.w * lw);
}

__global__ void k_motion(const float4* src, float4* dst, int w, int h,
                         float dx, float dy, int dist) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    float4 acc = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = -dist; i <= dist; ++i) {
        const float4 s = k_motion_sample(src, w, h, x + dx * i, y + dy * i);
        acc.x += s.x;
        acc.y += s.y;
        acc.z += s.z;
    }
    const float n = float(2 * dist + 1);
    const float4 c = src[y * w + x];
    dst[y * w + x] = make_float4(acc.x / n, acc.y / n, acc.z / n, c.w);
}


__global__ void k_shaped(const float4* src, float4* dst, int w, int h, int r, int kind) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    float accx = 0.0f, accy = 0.0f, accz = 0.0f;
    int n = 0;
    for (int dy = -r; dy <= r; ++dy) {
        const int yy = min(max(y + dy, 0), h - 1);
        for (int dx = -r; dx <= r; ++dx) {
            const float dist = sqrtf(float(dx * dx + dy * dy));
            if (dist > r) continue;
            bool inside = true;
            if (kind > 10) {
                const int sides = kind - 10;
                if (sides >= 3) {
                    const double th = atan2(double(dy), double(dx));
                    const double seg = 6.283185307179586 / sides;
                    double a = fmod(th, seg);
                    if (a < 0) a += seg;
                    const double sc = cos(3.141592653589793 / sides) / cos(a - seg * 0.5);
                    inside = dist <= r * sc;
                }
            }
            if (!inside) continue;
            const float4 p = src[yy * w + min(max(x + dx, 0), w - 1)];
            accx += p.x;
            accy += p.y;
            accz += p.z;
            ++n;
        }
    }
    n = max(n, 1);
    const float4 c = src[y * w + x];
    dst[y * w + x] = make_float4(accx / n, accy / n, accz / n, c.w);
}

__device__ unsigned long long k_lens_hash(int x, int y, unsigned long long s) {
    unsigned long long z = (unsigned long long)(x * 374761393 + y * 668265263) + s * 144665ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

__global__ void k_lens_finish(const float4* orig, const float4* blurred,
                              float4* dst, int w, int h, float brightness,
                              float threshold, float noise) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    const float4 o = orig[y * w + x];
    const float4 b = blurred[y * w + x];
    const float lum = 0.2126f * o.x + 0.7152f * o.y + 0.0722f * o.z;
    float spec = (lum - threshold) * 4.0f + 0.5f;
    spec = fminf(fmaxf(spec, 0.0f), 1.0f);
    const float m = 0.35f + 0.65f * spec;
    float4 t;
    t.x = o.x * (1.0f - m) + b.x * m + spec * brightness * 0.4f;
    t.y = o.y * (1.0f - m) + b.y * m + spec * brightness * 0.4f;
    t.z = o.z * (1.0f - m) + b.z * m + spec * brightness * 0.4f;
    t.w = o.w;
    if (noise > 0.01f) {
        const unsigned long long hh = k_lens_hash(x, y, 777ULL);
        const float g = float(hh % 1000) / 1000.0f - 0.5f;
        t.x = fminf(fmaxf(t.x + g * 0.08f * noise, 0.0f), 1.0f);
        t.y = fminf(fmaxf(t.y + g * 0.08f * noise, 0.0f), 1.0f);
        t.z = fminf(fmaxf(t.z + g * 0.08f * noise, 0.0f), 1.0f);
    }
    dst[y * w + x] = t;
}

// --- Live Tone Blend Group transfer (fused device path, CUDA mirror) -------
// Same math as cuda_backend.cu's k_tone_* and tone_blend.cpp: alpha-weighted
// box pyramid down, bilinear upsample, pivot reduction, per-pixel gain +
// Oklab re-grade. Kernel parity is enforced by tests/gpu_parity.cpp.
struct KToneParam {
    float strength;
    float color;
    float contrast;
    float pivot;
};

__global__ void k_tone_down(const float4* prev, float4* next, int pw, int ph,
                            int nw, int nh) {
    const int n = nw * nh;
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int x = i % nw;
    const int y = i / nw;
    // Single-precision accumulation (values in [0,1]): matches the CPU
    // double reference to ~1e-7, inside every parity tolerance.
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    for (int sy = 0; sy < 2; ++sy)
        for (int sx = 0; sx < 2; ++sx) {
            int px = 2 * x + sx;
            if (px > pw - 1) px = pw - 1;
            int py = 2 * y + sy;
            if (py > ph - 1) py = ph - 1;
            const float4 c = prev[static_cast<std::size_t>(py) * pw + px];
            r += c.x * c.w;
            g += c.y * c.w;
            b += c.z * c.w;
            a += c.w;
        }
    float4 o;
    if (a > 0.0f) {
        o.x = r / a;
        o.y = g / a;
        o.z = b / a;
        o.w = a / 4.0f;
    } else {
        o = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    next[i] = o;
}

__global__ void k_tone_up(const float4* lv, float4* out, int lw, int lh, int w,
                          int h) {
    const int n = w * h;
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int x = i % w;
    const int y = i / w;
    const double fy = (static_cast<double>(y) + 0.5) * lh / h - 0.5;
    long long y0 = static_cast<long long>(floor(fy));
    if (y0 < 0) y0 = 0;
    if (y0 > lh - 1) y0 = lh - 1;
    long long y1 = y0 + 1;
    if (y1 > lh - 1) y1 = lh - 1;
    double ty = fy - static_cast<double>(y0);
    if (ty < 0.0) ty = 0.0;
    if (ty > 1.0) ty = 1.0;
    const double fx = (static_cast<double>(x) + 0.5) * lw / w - 0.5;
    long long x0 = static_cast<long long>(floor(fx));
    if (x0 < 0) x0 = 0;
    if (x0 > lw - 1) x0 = lw - 1;
    long long x1 = x0 + 1;
    if (x1 > lw - 1) x1 = lw - 1;
    double tx = fx - static_cast<double>(x0);
    if (tx < 0.0) tx = 0.0;
    if (tx > 1.0) tx = 1.0;
    const float4 a = lv[static_cast<std::size_t>(y0) * lw + x0];
    const float4 b = lv[static_cast<std::size_t>(y0) * lw + x1];
    const float4 c = lv[static_cast<std::size_t>(y1) * lw + x0];
    const float4 d = lv[static_cast<std::size_t>(y1) * lw + x1];
    const float w00 = static_cast<float>((1.0 - tx) * (1.0 - ty));
    const float w10 = static_cast<float>(tx * (1.0 - ty));
    const float w01 = static_cast<float>((1.0 - tx) * ty);
    const float w11 = static_cast<float>(tx * ty);
    float4 o;
    o.x = a.x * w00 + b.x * w10 + c.x * w01 + d.x * w11;
    o.y = a.y * w00 + b.y * w10 + c.y * w01 + d.y * w11;
    o.z = a.z * w00 + b.z * w10 + c.z * w01 + d.z * w11;
    o.w = a.w * w00 + b.w * w10 + c.w * w01 + d.w * w11;
    out[i] = o;
}

struct KTonePivot {
    double sum;
    unsigned int count;
};

__global__ void k_tone_pivot(const float4* bLow, KTonePivot* out,
                             std::size_t n) {
    __shared__ double s_sum[kBlock];
    __shared__ unsigned int s_cnt[kBlock];
    double sum = 0.0;
    unsigned int cnt = 0;
    const std::size_t total = (n + 15) / 16;
    for (std::size_t j = threadIdx.x; j < total; j += blockDim.x) {
        const float4 c = bLow[j * 16];
        if (c.w > 0.0f) {
            float L, a, b;
            oklab::rgb_to_oklab(c.x, c.y, c.z, L, a, b);
            sum += static_cast<double>(L);
            ++cnt;
        }
    }
    s_sum[threadIdx.x] = sum;
    s_cnt[threadIdx.x] = cnt;
    __syncthreads();
    for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s) {
            s_sum[threadIdx.x] += s_sum[threadIdx.x + s];
            s_cnt[threadIdx.x] += s_cnt[threadIdx.x + s];
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        out->sum = s_sum[0];
        out->count = s_cnt[0];
    }
}

__global__ void k_tone_final(const float4* grp, const float4* gLow,
                             const float4* bLow, float4* dst, std::size_t n,
                             KToneParam p) {
    const std::size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float4 g = grp[i];
    if (g.w <= 0.0f) {
        dst[i] = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const float4 gl = gLow[i];
    const float4 bl = bLow[i];
    float kr = 1.0f, kg = 1.0f, kb = 1.0f;
    if (bl.w > 0.0f) {
        kr = fminf(fmaxf(bl.x / (gl.x + 1e-4f), 1.0f / 256.0f), 256.0f);
        kg = fminf(fmaxf(bl.y / (gl.y + 1e-4f), 1.0f / 256.0f), 256.0f);
        kb = fminf(fmaxf(bl.z / (gl.z + 1e-4f), 1.0f / 256.0f), 256.0f);
    }
    float L, a, b;
    oklab::rgb_to_oklab(g.x * kr, g.y * kg, g.z * kb, L, a, b);
    a *= p.color;
    b *= p.color;
    L = p.pivot + (L - p.pivot) * (1.0f + p.contrast);
    float r, gg, bb;
    oklab::oklab_to_rgb(L, a, b, r, gg, bb);
    oklab::gamut_map(r, gg, bb);
    float4 o;
    o.x = g.x + (r - g.x) * p.strength;
    o.y = g.y + (gg - g.y) * p.strength;
    o.z = g.z + (bb - g.z) * p.strength;
    o.w = g.w;
    dst[i] = o;
}

// --- Liquify displacement resample (CUDA mirror) ---------------------------
// Byte-for-byte the same math as cuda_backend.cu and warp.cpp: bilinear subgrid
// sample at the pixel centre, premultiplied bilinear colour fetch, transparent
// outside the layer.
struct KWarpParam {
    int w, h;            // layer size / row stride
    int x0, y0, x1, y1;  // region to resample
    float ox, oy;        // subgrid origin in layer pixels
    int cols, rows;      // subgrid vertex counts
};

__device__ float2 k_warp_displace(const float* ofs, int cols, int rows,
                                  float ox, float oy, float x, float y) {
    float2 d = make_float2(0.0f, 0.0f);
    if (cols < 2 || rows < 2) return d;
    const float fx = fminf(fmaxf((x - ox) / 4.0f, 0.0f), (float)(cols - 1));
    const float fy = fminf(fmaxf((y - oy) / 4.0f, 0.0f), (float)(rows - 1));
    const int c0 = (int)fx;
    const int r0 = (int)fy;
    const int c1 = min(c0 + 1, cols - 1);
    const int r1 = min(r0 + 1, rows - 1);
    const float tx = fx - c0;
    const float ty = fy - r0;
    const int i00 = (r0 * cols + c0) * 2;
    const int i10 = (r0 * cols + c1) * 2;
    const int i01 = (r1 * cols + c0) * 2;
    const int i11 = (r1 * cols + c1) * 2;
    const float ax = ofs[i00], ay = ofs[i00 + 1];
    const float bx = ofs[i10], by = ofs[i10 + 1];
    const float cx = ofs[i01], cy = ofs[i01 + 1];
    const float dx = ofs[i11], dy = ofs[i11 + 1];
    const float topx = ax + (bx - ax) * tx;
    const float topy = ay + (by - ay) * tx;
    const float botx = cx + (dx - cx) * tx;
    const float boty = cy + (dy - cy) * tx;
    d.x = topx + (botx - topx) * ty;
    d.y = topy + (boty - topy) * ty;
    return d;
}

__device__ float4 k_warp_fetch(const float4* src, int w, int h, int x, int y,
                               float dx, float dy) {
    const float ox = floorf(dx);
    const float oy = floorf(dy);
    const float tx = dx - ox;
    const float ty = dy - oy;
    const int x0 = x + (int)ox;
    const int y0 = y + (int)oy;
    float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f;
    const float ws[4] = {(1.0f - tx) * (1.0f - ty), tx * (1.0f - ty),
                         (1.0f - tx) * ty, tx * ty};
    const int tap[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int t = 0; t < 4; ++t) {
        const float wt = ws[t];
        if (wt <= 0.0f) continue;
        const int sx = x0 + tap[t][0];
        const int sy = y0 + tap[t][1];
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;  // transparent
        const float4 p = src[static_cast<std::size_t>(sy) * w + sx];
        ar += (p.x * p.w) * wt;
        ag += (p.y * p.w) * wt;
        ab += (p.z * p.w) * wt;
        aa += p.w * wt;
    }
    if (aa <= 1e-6f) return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    return make_float4(ar / aa, ag / aa, ab / aa, aa);
}

__global__ void k_warp(const float4* src, float4* dst, const float* ofs,
                       KWarpParam p) {
    const int rw = p.x1 - p.x0;
    const int rh = p.y1 - p.y0;
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= rw * rh) return;
    const int x = p.x0 + i % rw;
    const int y = p.y0 + i / rw;
    const float2 d = k_warp_displace(ofs, p.cols, p.rows, p.ox, p.oy,
                                     static_cast<float>(x) + 0.5f,
                                     static_cast<float>(y) + 0.5f);
    dst[static_cast<std::size_t>(y) * p.w + x] =
        k_warp_fetch(src, p.w, p.h, x, y, d.x, d.y);
}

class HipBuffer final : public Buffer {
  public:
    explicit HipBuffer(std::size_t bytes) : size_(bytes) {
        check(hipHostMalloc(&host_, bytes ? bytes : 1), "hipHostMalloc");
        check(hipMalloc(&dev_, bytes ? bytes : 1), "hipMalloc");
        std::memset(host_, 0, bytes);
    }
    ~HipBuffer() override {
        check_nothrow(hipFreeHost(host_), "hipFreeHost");
        check_nothrow(hipFree(dev_), "hipFree");
    }

    std::size_t size() const override { return size_; }
    void* host() override { return host_; }
    const void* host() const override { return host_; }
    float4* dev() const { return static_cast<float4*>(dev_); }
    void* raw_dev() const { return dev_; }

    // Device-side snapshot (tone backdrop): no host roundtrip.
    void copy_from(const Buffer& src) override {
        auto& s = dynamic_cast<const HipBuffer&>(src);
        if (s.size() != size_)
            throw std::invalid_argument("copy_from buffers differ in size");
        check(hipMemcpy(dev_, s.dev_, size_, hipMemcpyDeviceToDevice),
              "hipMemcpy D2D copy_from");
    }

    void upload() const override {
        const auto t0 = std::chrono::steady_clock::now();
        check(hipMemcpy(dev_, host_, size_, hipMemcpyHostToDevice), "hipMemcpy H2D");
        if (::pittore::core::log::bulkTrace()) PITTORE_LOG("[gpu][upload] bytes=%zu ms=%.3f", size_,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    void download() const override {
        const auto t0 = std::chrono::steady_clock::now();
        check(hipMemcpy(host_, dev_, size_, hipMemcpyDeviceToHost), "hipMemcpy D2H");
        if (::pittore::core::log::bulkTrace()) PITTORE_LOG("[gpu][download] bytes=%zu ms=%.3f", size_,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // Region-limited transfers: move only the pixels [x0,x1) × [y0,y1) of a
    // `w`-pixel-wide row-major RGBAf image. Rows are tightly packed, so the
    // pitch is w * sizeof(float4) and hipMemcpy2D carries the sub-rect.
    void upload_region(std::uint32_t w, std::uint32_t x0, std::uint32_t y0,
                       std::uint32_t x1, std::uint32_t y1) const override {
        if (x0 >= x1 || y0 >= y1) return;
        const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(float4);
        const std::size_t wb = static_cast<std::size_t>(x1 - x0) * sizeof(float4);
        const auto t0 = std::chrono::steady_clock::now();
        check(hipMemcpy2D(static_cast<char*>(dev_) + y0 * pitch + x0 * sizeof(float4),
                          pitch,
                          static_cast<const char*>(host_) + y0 * pitch + x0 * sizeof(float4),
                          pitch, wb, y1 - y0, hipMemcpyHostToDevice),
              "hipMemcpy2D H2D region");
        if (::pittore::core::log::transferTrace()) PITTORE_LOG("[gpu][upload] region=(%u,%u,%u,%u) bytes=%zu ms=%.3f", x0, y0,
                     x1, y1, wb * (y1 - y0),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    void download_region(std::uint32_t w, std::uint32_t x0, std::uint32_t y0,
                         std::uint32_t x1, std::uint32_t y1) const override {
        if (x0 >= x1 || y0 >= y1) return;
        const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(float4);
        const std::size_t wb = static_cast<std::size_t>(x1 - x0) * sizeof(float4);
        const auto t0 = std::chrono::steady_clock::now();
        check(hipMemcpy2D(static_cast<char*>(host_) + y0 * pitch + x0 * sizeof(float4),
                          pitch,
                          static_cast<const char*>(dev_) + y0 * pitch + x0 * sizeof(float4),
                          pitch, wb, y1 - y0, hipMemcpyDeviceToHost),
              "hipMemcpy2D D2H region");
        if (::pittore::core::log::transferTrace()) PITTORE_LOG("[gpu][download] region=(%u,%u,%u,%u) bytes=%zu ms=%.3f", x0, y0,
                     x1, y1, wb * (y1 - y0),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

  private:
    std::size_t size_;
    void* host_ = nullptr;
    void* dev_ = nullptr;
};

class HipBackend final : public ComputeBackend {
  public:
    explicit HipBackend(int ordinal) : ordinal_(ordinal) {
        check(hipSetDevice(ordinal), "hipSetDevice");
        hipDeviceProp_t prop{};
        check(hipGetDeviceProperties(&prop, ordinal), "hipGetDeviceProperties");
        name_ = "HIP " + std::string(prop.name);
        device_.type = BackendType::HIP;
        device_.name = prop.name;
        device_.memory_mb = prop.totalGlobalMem >> 20;
        device_.compute_major = prop.major;
        device_.compute_minor = prop.minor;
    }

    ~HipBackend() override { check_nothrow(hipSetDevice(ordinal_), "hipSetDevice"); }

    BackendType type() const override { return BackendType::HIP; }
    const std::string& name() const override { return name_; }
    const Device& device() const override { return device_; }

    std::unique_ptr<Buffer> make_buffer(std::size_t bytes) override {
        return std::make_unique<HipBuffer>(bytes);
    }

    void grayscale(Buffer& src, Buffer& dst, std::uint32_t w,
                   std::uint32_t h) override {
        if (src.size() != dst.size())
            throw std::invalid_argument("grayscale buffers differ in size");
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        const std::size_t n = static_cast<std::size_t>(w) * h;
        k_grayscale<<<(n + kBlock - 1) / kBlock, kBlock>>>(s.dev(), d.dev(), n);
        check(hipGetLastError(), "grayscale launch");
        d.download();
    }

    void composite(Buffer& bottom, const Buffer& top, std::uint32_t w,
                   std::uint32_t h, BlendMode mode) override {
        if (bottom.size() != top.size())
            throw std::invalid_argument("composite buffers differ in size");
        const auto t0 = std::chrono::steady_clock::now();
        auto& b = dynamic_cast<HipBuffer&>(bottom);
        auto& t = dynamic_cast<const HipBuffer&>(top);
        t.upload();
        b.upload();
        const std::size_t n = static_cast<std::size_t>(w) * h;
        k_composite<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            b.dev(), t.dev(), n, static_cast<int>(w), static_cast<int>(mode));
        check(hipGetLastError(), "composite launch");
        b.download();
        PITTORE_LOG("[composite] gpu full w=%u h=%u mode=%d bytes=%zu ms=%.3f", w, h,
                     static_cast<int>(mode), bottom.size(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    void composite_region(Buffer& bottom, const Buffer& top, std::uint32_t w,
                          std::uint32_t /*h*/, std::uint32_t x0, std::uint32_t y0,
                          std::uint32_t x1, std::uint32_t y1,
                          BlendMode mode) override {
        if (bottom.size() != top.size())
            throw std::invalid_argument("composite_region buffers differ in size");
        if (x0 >= x1 || y0 >= y1) return;
        const auto t0 = std::chrono::steady_clock::now();
        auto& b = dynamic_cast<HipBuffer&>(bottom);
        auto& t = dynamic_cast<const HipBuffer&>(top);
        t.upload_region(w, x0, y0, x1, y1);
        b.upload_region(w, x0, y0, x1, y1);
        const int rw = static_cast<int>(x1 - x0);
        const int rh = static_cast<int>(y1 - y0);
        const int n = rw * rh;
        k_composite_region<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            b.dev(), t.dev(), static_cast<int>(w), static_cast<int>(x0),
            static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1),
            static_cast<int>(mode));
        check(hipGetLastError(), "composite_region launch");
        b.download_region(w, x0, y0, x1, y1);
        PITTORE_LOG("[composite] gpu region=(%u,%u,%u,%u) w=%u mode=%d ms=%.3f",
                     x0, y0, x1, y1, w, static_cast<int>(mode),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // Fused placed-layer composite: the layer's native pixels are already
    // device-resident (the UI caches them per layer), so the sampler reads
    // them straight from device memory and only the accumulator region crosses
    // the host/device boundary — one upload + one kernel + one download for the
    // whole fused sample/fold/blend step, no work-buffer staging per layer.
    void composite_placed(Buffer& bottom, const Buffer& src,
                          std::uint32_t sw, std::uint32_t sh,
                          double ox, double oy, double sx, double sy,
                          std::uint32_t w, std::uint32_t x0, std::uint32_t y0,
                          std::uint32_t x1, std::uint32_t y1,
                          float alpha_fold, BlendMode mode) override {
        if (x0 >= x1 || y0 >= y1) return;
        const auto t0 = std::chrono::steady_clock::now();
        auto& b = dynamic_cast<HipBuffer&>(bottom);
        auto& s = dynamic_cast<const HipBuffer&>(src);
        b.upload_region(w, x0, y0, x1, y1);
        const KPlacedParam p{static_cast<int>(sw), static_cast<int>(sh),
                             static_cast<int>(w), static_cast<int>(x0),
                             static_cast<int>(y0), static_cast<int>(x1),
                             static_cast<int>(y1), ox, oy, sx, sy, alpha_fold,
                             static_cast<int>(mode)};
        const int n = (static_cast<int>(x1) - static_cast<int>(x0)) *
                      (static_cast<int>(y1) - static_cast<int>(y0));
        k_composite_placed<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), b.dev(), p);
        check(hipGetLastError(), "composite_placed launch");
        b.download_region(w, x0, y0, x1, y1);
        PITTORE_LOG("[composite] gpu placed region=(%u,%u,%u,%u) src=%ux%u scale=(%.2f,%.2f) fold=%.3f mode=%d ms=%.3f",
                     x0, y0, x1, y1, sw, sh, sx, sy, alpha_fold,
                     static_cast<int>(mode),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }

    // Device-persistent fused composite: `bottom` and `src` are both already
    // device-resident, so this is a bare kernel launch — no accumulator
    // transfer at all. blit_premul pulls the finished frame back once.
    void composite_placed_into(Buffer& bottom, const Buffer& src,
                               std::uint32_t sw, std::uint32_t sh,
                               double ox, double oy, double sx, double sy,
                               std::uint32_t w, std::uint32_t x0,
                               std::uint32_t y0, std::uint32_t x1,
                               std::uint32_t y1, float alpha_fold,
                               BlendMode mode) override {
        if (x0 >= x1 || y0 >= y1) return;
        auto& b = dynamic_cast<HipBuffer&>(bottom);
        auto& s = dynamic_cast<const HipBuffer&>(src);
        const KPlacedParam p{static_cast<int>(sw), static_cast<int>(sh),
                             static_cast<int>(w), static_cast<int>(x0),
                             static_cast<int>(y0), static_cast<int>(x1),
                             static_cast<int>(y1), ox, oy, sx, sy, alpha_fold,
                             static_cast<int>(mode)};
        const int n = (static_cast<int>(x1) - static_cast<int>(x0)) *
                      (static_cast<int>(y1) - static_cast<int>(y0));
        k_composite_placed<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), b.dev(), p);
        check(hipGetLastError(), "composite_placed_into launch");
    }

    // One launch for every layer instead of one per layer. The param array and
    // the per-row bottom→top cell lists are staged on the device (grown on
    // demand like argb_), so a multi-part document pays a single upload +
    // launch.
    void composite_many_into(Buffer& bottom, std::uint32_t w,
                             std::uint32_t rx0, std::uint32_t ry0,
                             std::uint32_t rx1, std::uint32_t ry1,
                             const PlacedLayer* layers,
                             std::size_t count) override {
        if (!layers || count == 0 || rx0 >= rx1 || ry0 >= ry1) return;
        const auto t0 = std::chrono::steady_clock::now();
        auto& b = dynamic_cast<HipBuffer&>(bottom);
        const std::size_t elem = static_cast<std::size_t>(w) * sizeof(float4);
        const int h = b.size() % elem == 0
                          ? static_cast<int>(b.size() / elem)
                          : static_cast<int>(b.size() / elem) + 1;
        // Flat-stack fast path: every kept layer is plain Normal covering
        // the region (no mask/clip/adjustment). One tight kernel with params
        // in flight — no cell tables, no staging uploads. Falls through to
        // the generic walk on the first exotic layer.
        {
            KFlatParam flat[kFlatMax];
            int nflat = 0;
            bool flatOk = true;
            // Coverage heuristic: the flat walk samples every kept layer at
            // every region pixel, so it only wins when layers are dense over
            // the region (full-frame photos qualify; sparse thumbnails take
            // the generic cell walk). Correctness never depends on this: the
            // per-layer window check above skips exactly like cells do.
            std::uint64_t winArea = 0;
            const std::uint64_t regionArea =
                static_cast<std::uint64_t>(rx1 - rx0) * (ry1 - ry0);
            for (std::size_t i = 0; i < count && flatOk; ++i) {
                const PlacedLayer& in = layers[i];
                if (in.x0 >= in.x1 || in.y0 >= in.y1) continue;
                if (in.y1 > static_cast<std::uint32_t>(h)) continue;
                bool plain = !in.isAdjustment && !in.clipped && !in.mask &&
                             in.mode == BlendMode::Normal && in.src &&
                             in.sw > 0 && in.sh > 0 && in.sx > 0.0 &&
                             in.sy > 0.0 && nflat < kFlatMax;
                if (!plain) {
                    flatOk = false;
                    break;
                }
                winArea += static_cast<std::uint64_t>(in.x1 - in.x0) *
                           (in.y1 - in.y0);
                auto& s = dynamic_cast<const HipBuffer&>(*in.src);
                KFlatParam p{};
                p.src = s.dev();
                p.sw = static_cast<int>(in.sw);
                p.sh = static_cast<int>(in.sh);
                p.ox = in.ox;
                p.oy = in.oy;
                p.sx = in.sx;
                p.sy = in.sy;
                p.fold = in.fold;
                p.direct = (in.sx == 1.0 && in.sy == 1.0 &&
                            in.ox == static_cast<double>(static_cast<int>(in.ox)) &&
                            in.oy == static_cast<double>(static_cast<int>(in.oy)))
                               ? 1
                               : 0;
                p.iox = static_cast<int>(in.ox);
                p.ioy = static_cast<int>(in.oy);
                p.x0 = static_cast<int>(in.x0);
                p.y0 = static_cast<int>(in.y0);
                p.x1 = static_cast<int>(in.x1);
                p.y1 = static_cast<int>(in.y1);
                flat[nflat++] = p;
            }
            if (flatOk && nflat > 0 &&
                winArea * 2 >= static_cast<std::uint64_t>(nflat) * regionArea) {
                const int rw = static_cast<int>(rx1 - rx0);
                const int rh = static_cast<int>(ry1 - ry0);
                check(hipMemcpyToSymbol(kFlatConst, flat,
                                        static_cast<std::size_t>(nflat) *
                                            sizeof(KFlatParam),
                                        0, hipMemcpyHostToDevice),
                      "composite_many_into flat params");
                k_composite_flat_rows<<<(static_cast<std::size_t>(rw) * rh + kBlock - 1) /
                                           kBlock,
                                       kBlock>>>(
                    b.dev(), static_cast<int>(w), static_cast<int>(rx0),
                    static_cast<int>(ry0), rw, rh, nflat);
                check(hipGetLastError(), "composite_many_into flat launch");
                // Same fence as the generic path: the launch is async, so
                // synchronize here — otherwise this kernel's time leaks into
                // whatever syncs next (tone pivot, blit DMA) and the logs lie.
                const auto thost = std::chrono::steady_clock::now();
                check(hipDeviceSynchronize(), "composite_many_into flat sync");
                const auto tdev = std::chrono::steady_clock::now();
                PITTORE_LOG(
                    "[composite] gpu placed-many flat layers=%d region=%ux%u "
                    "host=%.3f kernel=%.3f",
                    nflat, rw, rh,
                    std::chrono::duration<double, std::milli>(thost - t0).count(),
                    std::chrono::duration<double, std::milli>(tdev - thost).count());
                return;
            }
        }
        compHost_.clear();
        compKept_.clear();
        if (compHost_.capacity() < count) {
            compHost_.reserve(count);
            compKept_.reserve(count);
        }
        auto& host = compHost_;
        auto& kept = compKept_;
        compPer_.assign(static_cast<std::size_t>(h) + 1, 0);
        auto& per = compPer_;
        std::size_t totalCells = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const PlacedLayer& in = layers[i];
            if (in.x0 >= in.x1 || in.y0 >= in.y1) continue;
            if (in.y1 > static_cast<std::uint32_t>(h)) continue;
            const float4* srcDev = nullptr;
            int sw = 0, sh = 0;
            if (!in.isAdjustment) {
                if (!in.src) continue;
                auto& s = dynamic_cast<const HipBuffer&>(*in.src);
                srcDev = s.dev();
                sw = static_cast<int>(in.sw);
                sh = static_cast<int>(in.sh);
            }
            const float4* maskDev = nullptr;
            int msw = 0, msh = 0;
            if (in.mask) {
                auto& m = dynamic_cast<const HipBuffer&>(*in.mask);
                maskDev = m.dev();
                msw = static_cast<int>(in.msw);
                msh = static_cast<int>(in.msh);
            }
            const float* auxDev = nullptr;
            if (in.adjAux) {
                auto& a = dynamic_cast<const HipBuffer&>(*in.adjAux);
                auxDev = reinterpret_cast<const float*>(a.dev());
            }
            KPlacedManyParam p{};
            p.src = srcDev;
            p.sw = sw;
            p.sh = sh;
            p.w = static_cast<int>(w);
            p.x0 = static_cast<int>(in.x0);
            p.y0 = static_cast<int>(in.y0);
            p.x1 = static_cast<int>(in.x1);
            p.y1 = static_cast<int>(in.y1);
            p.ox = in.ox;
            p.oy = in.oy;
            p.sx = in.sx;
            p.sy = in.sy;
            p.fold = in.fold;
            p.mode = static_cast<int>(in.mode);
            p.mask = maskDev;
            p.msw = msw;
            p.msh = msh;
            p.mox = in.mox;
            p.moy = in.moy;
            p.msx = in.msx;
            p.msy = in.msy;
            p.clipped = in.clipped ? 1 : 0;
            p.clipBase = in.clipBase;
            p.isAdjustment = in.isAdjustment ? 1 : 0;
            p.adjKind = in.adjKind;
            for (int k = 0; k < 16; ++k) p.adjP[k] = in.adjP[k];
            p.adjAux = auxDev;
            host.push_back(p);
            kept.push_back(in);
            for (std::uint32_t yy = in.y0; yy < in.y1; ++yy) {
                ++per[yy];
                ++totalCells;
            }
        }
        const int ncount = static_cast<int>(host.size());
        if (ncount == 0) return;
        for (auto& p : host) {
            if (!p.clipped) {
                p.clipBase = -1;
                continue;
            }
            const bool valid =
                p.clipBase == -1 ||
                (p.clipBase >= 0 && p.clipBase < ncount &&
                 !host[static_cast<std::size_t>(p.clipBase)].clipped &&
                 !host[static_cast<std::size_t>(p.clipBase)].isAdjustment);
            if (!valid) {
                p.clipped = 0;
                p.clipBase = -1;
            }
        }
        compRow_.resize(static_cast<std::size_t>(h) + 1);
        auto& rowStart = compRow_;
        rowStart[0] = 0;
        for (int yy = 1; yy <= h; ++yy)
            rowStart[yy] = rowStart[yy - 1] + per[yy - 1];
        compCurs_ = rowStart;
        auto& curs = compCurs_;
        compCells_.clear();
        compCells_.resize(totalCells);
        auto& cells = compCells_;
        for (std::size_t j = 0; j < kept.size(); ++j) {
            const PlacedLayer& in = kept[j];
            const KRowCell c{static_cast<int>(in.x0), static_cast<int>(in.x1),
                             static_cast<int>(j)};
            for (std::uint32_t yy = in.y0; yy < in.y1; ++yy)
                cells[curs[yy]++] = c;
        }

        const std::size_t paramBytes = host.size() * sizeof(KPlacedManyParam);
        if (!many_ || many_->size() < paramBytes)
            many_ = std::make_unique<HipBuffer>(paramBytes);
        const std::size_t rowBytes = rowStart.size() * sizeof(int);
        if (!cum_ || cum_->size() < rowBytes)
            cum_ = std::make_unique<HipBuffer>(rowBytes);
        const std::size_t cellBytes = cells.size() * sizeof(KRowCell);
        if (!rows_ || rows_->size() < cellBytes)
            rows_ = std::make_unique<HipBuffer>(cellBytes);
        check(hipMemcpy(many_->raw_dev(), host.data(), paramBytes,
                        hipMemcpyHostToDevice),
              "composite_many_into params");
        check(hipMemcpy(cum_->raw_dev(), rowStart.data(), rowBytes,
                        hipMemcpyHostToDevice),
              "composite_many_into row-start");
        if (cellBytes)
            check(hipMemcpy(rows_->raw_dev(), cells.data(), cellBytes,
                            hipMemcpyHostToDevice),
                  "composite_many_into cells");
        const int rw = static_cast<int>(rx1 - rx0);
        const int rh = static_cast<int>(ry1 - ry0);
        k_composite_many_rows<<<(static_cast<std::size_t>(rw) * rh + kBlock - 1) /
                                    kBlock,
                                kBlock>>>(
            static_cast<const KPlacedManyParam*>(many_->raw_dev()),
            static_cast<const int*>(cum_->raw_dev()),
            static_cast<const KRowCell*>(rows_->raw_dev()), b.dev(),
            static_cast<int>(w), static_cast<int>(rx0), static_cast<int>(ry0),
            rw, rh);
        check(hipGetLastError(), "composite_many_into launch");
        const auto thost = std::chrono::steady_clock::now();
        check(hipDeviceSynchronize(), "composite_many_into sync");
        const auto tdev = std::chrono::steady_clock::now();
        PITTORE_LOG(
            "[composite] gpu placed-many rows layers=%d cells=%zu region=%ux%u "
            "bytes=%zu host=%.3f kernel=%.3f",
            ncount, totalCells, rw, rh,
            paramBytes + rowBytes + cellBytes,
            std::chrono::duration<double, std::milli>(thost - t0).count(),
            std::chrono::duration<double, std::milli>(tdev - thost).count());
    }

    void clear(Buffer& dst) override {
        auto& d = dynamic_cast<HipBuffer&>(dst);
        if (d.size()) check(hipMemset(d.raw_dev(), 0, d.size()), "hipMemset");
    }

    // In-place round stamp on device-resident pixels: bbox-sized launch, no
    // full-frame traffic (the base-class download+dab+upload costs ~1.2ms
    // per dab on 1080p). Math mirrors paint_dab_host exactly.
    void paint_dab(Buffer& dst, std::uint32_t w, std::uint32_t h, float cx,
                   float cy, float radius, float hardness, float opacity,
                   const RGBAf& color) override {
        const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
        if (dst.size() < need)
            throw std::invalid_argument("paint_dab buffer too small");
        if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
        auto& d = dynamic_cast<HipBuffer&>(dst);
        const float hard = std::clamp(hardness, 0.0f, 1.0f);
        const float op = std::clamp(opacity, 0.0f, 1.0f);
        const int x0 =
            std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int y0 =
            std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int x1 = std::min(
            static_cast<int>(w) - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y1 = std::min(
            static_cast<int>(h) - 1, static_cast<int>(std::ceil(cy + radius)));
        if (x0 > x1 || y0 > y1) return;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        const int n = bw * bh;
        const float inv_r = 1.0f / radius;
        const float soft_span = std::max(1.0f - hard, 1e-4f);
        k_paint_dab<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            d.dev(), static_cast<int>(w), x0, y0, bw, bh, cx, cy, inv_r,
            soft_span, op, color.r, color.g, color.b);
        check(hipGetLastError(), "paint_dab launch");
        // Keep host staging current for the touched bbox (the old
        // download+dab+upload contract), without full-frame traffic.
        d.download_region(w, static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1 + 1),
                          static_cast<std::uint32_t>(y1 + 1));
    }

    // bg_erase: discontiguous tolerance erase (kernel + bbox-only
    // download). Contiguous limits run on the host core instead.
    void bg_erase(Buffer& dst, std::uint32_t w, std::uint32_t h, float cx,
                  float cy, float radius, float hardness, float opacity,
                  const RGBAf& sample, float tolerance, bool protectFg,
                  const RGBAf& fg, int* bboxOut) override {
        const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
        if (dst.size() < need)
            throw std::invalid_argument("bg_erase buffer too small");
        if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
        auto& d = dynamic_cast<HipBuffer&>(dst);
        const float hard = std::clamp(hardness, 0.0f, 1.0f);
        const float op = std::clamp(opacity, 0.0f, 1.0f);
        const float tol = std::clamp(tolerance, 0.0f, 1.0f);
        const int x0 =
            std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int y0 =
            std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int x1 = std::min(
            static_cast<int>(w) - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y1 = std::min(
            static_cast<int>(h) - 1, static_cast<int>(std::ceil(cy + radius)));
        if (x0 > x1 || y0 > y1) return;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        const int n = bw * bh;
        k_bg_erase<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            d.dev(), static_cast<int>(w), x0, y0, bw, bh, cx, cy,
            1.0f / radius, hard, op, sample.r, sample.g, sample.b, tol,
            protectFg ? 1 : 0, fg.r, fg.g, fg.b);
        check(hipGetLastError(), "bg_erase launch");
        d.download_region(w, static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1 + 1),
                          static_cast<std::uint32_t>(y1 + 1));
        if (bboxOut) {
            bboxOut[0] = x0;
            bboxOut[1] = y0;
            bboxOut[2] = x1 + 1;
            bboxOut[3] = y1 + 1;
        }
    }

    // pattern_stamp: source-over the procedural tile (kernel + bbox-only
    // download). Tile crosses as a 64x64 device buffer.
    void pattern_stamp(Buffer& dst, std::uint32_t w, std::uint32_t h,
                       float cx, float cy, float radius, float hardness,
                       float opacity, const Buffer& tile, float ox, float oy,
                       int* bboxOut) override {
        const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
        if (dst.size() < need)
            throw std::invalid_argument("pattern_stamp buffer too small");
        if (tile.size() < 64u * 64u * sizeof(RGBAf))
            throw std::invalid_argument("pattern_stamp tile too small");
        if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
        auto& d = dynamic_cast<HipBuffer&>(dst);
        auto& t = dynamic_cast<const HipBuffer&>(tile);
        const float hard = std::clamp(hardness, 0.0f, 1.0f);
        const float op = std::clamp(opacity, 0.0f, 1.0f);
        const int x0 =
            std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int y0 =
            std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int x1 = std::min(
            static_cast<int>(w) - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y1 = std::min(
            static_cast<int>(h) - 1, static_cast<int>(std::ceil(cy + radius)));
        if (x0 > x1 || y0 > y1) return;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        const int n = bw * bh;
        k_pattern_stamp<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            d.dev(), static_cast<int>(w), x0, y0, bw, bh, cx, cy,
            1.0f / radius, hard, op,
            reinterpret_cast<const float4*>(t.dev()), ox, oy);
        check(hipGetLastError(), "pattern_stamp launch");
        d.download_region(w, static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1 + 1),
                          static_cast<std::uint32_t>(y1 + 1));
        if (bboxOut) {
            bboxOut[0] = x0;
            bboxOut[1] = y0;
            bboxOut[2] = x1 + 1;
            bboxOut[3] = y1 + 1;
        }
    }

    // history_dab: source-over snapshot texels (kernel + bbox-only
    // download). `src` must match dst dimensions.
    void history_dab(Buffer& dst, std::uint32_t w, std::uint32_t h, float cx,
                     float cy, float radius, float hardness, float opacity,
                     const Buffer& src, int* bboxOut) override {
        const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
        if (dst.size() < need || src.size() < need)
            throw std::invalid_argument("history_dab buffer too small");
        if (radius <= 0.0f || opacity <= 0.0f || w == 0 || h == 0) return;
        auto& d = dynamic_cast<HipBuffer&>(dst);
        auto& s = dynamic_cast<const HipBuffer&>(src);
        const float hard = std::clamp(hardness, 0.0f, 1.0f);
        const float op = std::clamp(opacity, 0.0f, 1.0f);
        const int x0 =
            std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int y0 =
            std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int x1 = std::min(
            static_cast<int>(w) - 1, static_cast<int>(std::ceil(cx + radius)));
        const int y1 = std::min(
            static_cast<int>(h) - 1, static_cast<int>(std::ceil(cy + radius)));
        if (x0 > x1 || y0 > y1) return;
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        const int n = bw * bh;
        k_history_dab<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            d.dev(), reinterpret_cast<const float4*>(s.dev()),
            static_cast<int>(w), x0, y0, bw, bh, cx, cy, 1.0f / radius,
            hard, op);
        check(hipGetLastError(), "history_dab launch");
        d.download_region(w, static_cast<std::uint32_t>(x0),
                          static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1 + 1),
                          static_cast<std::uint32_t>(y1 + 1));
        if (bboxOut) {
            bboxOut[0] = x0;
            bboxOut[1] = y0;
            bboxOut[2] = x1 + 1;
            bboxOut[3] = y1 + 1;
        }
    }

    void blit_premul(const Buffer& src, std::uint8_t* dst, std::uint32_t w,
                     std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                     std::uint32_t y1, std::size_t dst_pitch) override {
        if (x0 >= x1 || y0 >= y1) return;
        auto& s = dynamic_cast<const HipBuffer&>(src);
        const int rw = static_cast<int>(x1 - x0);
        const int rh = static_cast<int>(y1 - y0);
        const std::size_t need =
            static_cast<std::size_t>(rw) * rh * sizeof(unsigned int);
        if (!argb_ || argb_->size() < need) argb_ = std::make_unique<HipBuffer>(need);
        const int n = rw * rh;
        k_premul_argb<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), static_cast<unsigned int*>(argb_->raw_dev()),
            static_cast<int>(w), static_cast<int>(x0), static_cast<int>(y0), rw,
            rh);
        check(hipGetLastError(), "blit_premul launch");
        check(hipMemcpy2D(dst, dst_pitch, argb_->raw_dev(),
                          static_cast<std::size_t>(rw) * sizeof(unsigned int),
                          static_cast<std::size_t>(rw) * sizeof(unsigned int), rh,
                          hipMemcpyDeviceToHost),
              "blit_premul copy");
    }

    // Resident core: same kernels, no transfers (see CUDA backend).
    void gaussian_blur_resident(const HipBuffer& s, HipBuffer& d,
                                std::uint32_t w, std::uint32_t h,
                                float sigma) {
        if (sigma <= 0.0f) {
            check(hipMemcpy(d.dev(), s.dev(), s.size(), hipMemcpyDeviceToDevice),
                  "hipMemcpy D2D");
            return;
        }

        const int r = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
        std::vector<float> kernel(static_cast<std::size_t>(2 * r + 1));
        float sum = 0.0f;
        for (int i = -r; i <= r; ++i) {
            const float t = static_cast<float>(i) / sigma;
            const float v = std::exp(-0.5f * t * t);
            kernel[static_cast<std::size_t>(i + r)] = v;
            sum += v;
        }
        const float inv = 1.0f / sum;
        for (float& k : kernel) k *= inv;

        HipBuffer& kc = blurKernel(kernel.size() * sizeof(float));
        check(hipMemcpy(kc.dev(), kernel.data(), kernel.size() * sizeof(float),
                        hipMemcpyHostToDevice),
              "hipMemcpy kernel");
        HipBuffer& tmp = blurFrame(static_cast<std::size_t>(w) * h * sizeof(float4));

        const dim3 block(256, 4);
        const dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);

        k_blur_h<<<grid, block>>>(s.dev(), tmp.dev(), static_cast<int>(w),
                                  static_cast<int>(h),
                                  static_cast<float*>(kc.raw_dev()), r);
        k_blur_v<<<grid, block>>>(tmp.dev(), d.dev(), static_cast<int>(w),
                                  static_cast<int>(h),
                                  static_cast<float*>(kc.raw_dev()), r);
        check(hipGetLastError(), "blur launch");
    }

    void gaussian_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                       std::uint32_t h, float sigma) override {
        if (src.size() != dst.size())
            throw std::invalid_argument("gaussian_blur buffers differ in size");
        auto& s = dynamic_cast<const HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        gaussian_blur_resident(s, d, w, h, sigma);
        d.download();
    }

    void gaussian_blur_resident(const Buffer& src, Buffer& dst,
                                std::uint32_t w, std::uint32_t h,
                                float sigma) override {
        auto& s = dynamic_cast<const HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        gaussian_blur_resident(s, d, w, h, sigma);
    }

    void sharpen(Buffer& src, Buffer& dst, std::uint32_t w, std::uint32_t h,
                 float amount, float radius, float threshold) override {
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        // Second pool slot: the resident blur below reuses slot A internally.
        HipBuffer& tmp = blurFrame(s.size(), true);
        gaussian_blur_resident(s, tmp, w, h, radius);  // no second upload
        const std::size_t n = static_cast<std::size_t>(w) * h;
        k_sharpen<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), tmp.dev(), d.dev(), n, amount, threshold);
        check(hipGetLastError(), "sharpen launch");
        d.download();
    }

    void brightness_contrast(Buffer& src, Buffer& dst, std::uint32_t w,
                             std::uint32_t h, float brightness,
                             float contrast) override {
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        const std::size_t n = static_cast<std::size_t>(w) * h;
        k_brightness_contrast<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), d.dev(), n, brightness, contrast);
        check(hipGetLastError(), "brightness_contrast launch");
        d.download();
    }

    void hue_saturation(Buffer& src, Buffer& dst, std::uint32_t w,
                        std::uint32_t h, float hue_shift, float saturation,
                        float lightness) override {
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        const std::size_t n = static_cast<std::size_t>(w) * h;
        k_hue_saturation<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), d.dev(), n, hue_shift, saturation, lightness);
        check(hipGetLastError(), "hue_saturation launch");
        d.download();
    }

    void median_filter(Buffer& src, Buffer& dst, std::uint32_t w,
                       std::uint32_t h) override {
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        k_median3x3<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16)>>>(
            s.dev(), d.dev(), static_cast<int>(w), static_cast<int>(h));
        check(hipGetLastError(), "median_filter launch");
        d.download();
    }

    void box_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                  std::uint32_t h, int radius) override {
        auto& s = dynamic_cast<const HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        if (s.size() != d.size())
            throw std::invalid_argument("box_blur buffers differ in size");
        s.upload();
        box_blur_resident(s, d, w, h, radius);
        d.download();
    }

    void box_blur_resident(const Buffer& src, Buffer& dst, std::uint32_t w,
                           std::uint32_t h, int radius) override {
        auto& s = dynamic_cast<const HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        if (s.size() != d.size())
            throw std::invalid_argument("box_blur buffers differ in size");
        const int r = std::max(1, radius);
        HipBuffer& tmp = blurFrame(s.size());
        const dim3 block(256, 4);
        const dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
        k_box_h<<<grid, block>>>(s.dev(), tmp.dev(), static_cast<int>(w),
                                 static_cast<int>(h), r);
        k_box_v<<<grid, block>>>(tmp.dev(), d.dev(), static_cast<int>(w),
                                 static_cast<int>(h), r);
        check(hipGetLastError(), "box_blur launch");
    }

    void median_radius(Buffer& src, Buffer& dst, std::uint32_t w,
                       std::uint32_t h, int radius) override {
        const int r = std::max(1, radius);
        if (r > 8) {
            ComputeBackend::median_radius(src, dst, w, h, r);
            return;
        }
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        s.upload();
        k_median_r<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16)>>>(
            s.dev(), d.dev(), static_cast<int>(w), static_cast<int>(h), r);
        check(hipGetLastError(), "median_radius launch");
        d.download();
    }

    void lens_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                   std::uint32_t h, int radius, int kind, float brightness,
                   float threshold, float noise) override {
        auto& s = dynamic_cast<const HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        if (s.size() != d.size())
            throw std::invalid_argument("lens_blur buffers differ in size");
        s.upload();
        const int r = max(1, radius);
        HipBuffer& tmp = blurFrame(s.size());
        // Small blocks on purpose: k_shaped evaluates the polygon aperture
        // in double precision, which costs 2 registers per value plus the
        // transcendental calls — a 1024-thread block exceeds the per-block
        // register file ("too many resources requested for launch"). The
        // math is untouched, so CPU/GPU parity is unaffected.
        const dim3 block(128, 2);
        const dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
        k_shaped<<<grid, block>>>(s.dev(), tmp.dev(), static_cast<int>(w),
                                  static_cast<int>(h), r, kind);
        k_lens_finish<<<grid, block>>>(s.dev(), tmp.dev(), d.dev(),
                                       static_cast<int>(w), static_cast<int>(h),
                                       brightness, threshold, noise);
        check(hipGetLastError(), "lens_blur launch");
        d.download();
    }

    void unsharp_box(Buffer& src, Buffer& dst, std::uint32_t w,
                     std::uint32_t h, float amount, int radius,
                     float threshold) override {
        auto& s = dynamic_cast<HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        if (s.size() != d.size())
            throw std::invalid_argument("unsharp_box buffers differ in size");
        s.upload();
        const int r = std::max(1, radius);
        HipBuffer& tmp = blurFrame(s.size());
        const dim3 block(256, 4);
        const dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
        k_box_h<<<grid, block>>>(s.dev(), tmp.dev(), static_cast<int>(w),
                                 static_cast<int>(h), r);
        k_box_v<<<grid, block>>>(tmp.dev(), d.dev(), static_cast<int>(w),
                                 static_cast<int>(h), r);
        const std::size_t n = static_cast<std::size_t>(w) * h;
        k_unsharp_gate<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), d.dev(), d.dev(), n, amount, threshold);
        check(hipGetLastError(), "unsharp_box launch");
        d.download();
    }

    void motion_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                     std::uint32_t h, float angleDeg, int distance) override {
        auto& s = dynamic_cast<const HipBuffer&>(src);
        auto& d = dynamic_cast<HipBuffer&>(dst);
        if (s.size() != d.size())
            throw std::invalid_argument("motion_blur buffers differ in size");
        s.upload();
        const double ang = angleDeg * 3.141592653589793 / 180.0;
        const int dist = std::max(1, distance);
        k_motion<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16)>>>(
            s.dev(), d.dev(), static_cast<int>(w), static_cast<int>(h),
            static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)),
            dist);
        check(hipGetLastError(), "motion_blur launch");
        d.download();
    }

    // Liquify resample (CUDA mirror): the frozen snapshot is device-resident
    // for the stroke, so only the dab's subgrid is uploaded and the rebuilt
    // region downloaded.
    void warp(Buffer& dst, const Buffer& src, std::uint32_t w, std::uint32_t h,
              const WarpSubgrid& grid, std::uint32_t x0, std::uint32_t y0,
              std::uint32_t x1, std::uint32_t y1) override {
        if (x0 >= x1 || y0 >= y1 || grid.empty()) return;
        const auto t0 = std::chrono::steady_clock::now();
        auto& d = dynamic_cast<HipBuffer&>(dst);
        auto& s = dynamic_cast<const HipBuffer&>(src);
        const std::size_t bytes = grid.offsets.size() * sizeof(float);
        if (!warp_ || warp_->size() < bytes)
            warp_ = std::make_unique<HipBuffer>(bytes ? bytes : sizeof(float));
        if (bytes)
            check(hipMemcpy(warp_->raw_dev(), grid.offsets.data(), bytes,
                            hipMemcpyHostToDevice),
                  "warp offsets");
        const KWarpParam p{static_cast<int>(w),  static_cast<int>(h),
                           static_cast<int>(x0), static_cast<int>(y0),
                           static_cast<int>(x1), static_cast<int>(y1),
                           static_cast<float>(grid.left),
                           static_cast<float>(grid.top),
                           static_cast<int>(grid.cols),
                           static_cast<int>(grid.rows)};
        const int n = (static_cast<int>(x1) - static_cast<int>(x0)) *
                      (static_cast<int>(y1) - static_cast<int>(y0));
        k_warp<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            s.dev(), d.dev(), static_cast<const float*>(warp_->raw_dev()), p);
        check(hipGetLastError(), "warp launch");
        d.download_region(w, x0, y0, x1, y1);
        PITTORE_LOG("[warp] hip region=(%u,%u,%u,%u) grid=%ux%u ms=%.3f", x0,
                     y0, x1, y1, grid.cols, grid.rows,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
    }

    // Live Tone Blend Group transfer, fused on-device (CUDA mirror): pyramid
    // low-pass for both fields, bilinear upsample, pivot reduction, then the
    // per-pixel gain + Oklab re-grade. Same math as applyToneBlend.
    void tone_blend(Buffer& dst, const Buffer& backdrop, std::uint32_t w,
                    std::uint32_t h, const ToneBlendParams& p) override {
        const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(float4);
        if (dst.size() < need || backdrop.size() < need)
            throw std::invalid_argument("tone_blend buffers too small");
        // CPU contract: strength <= 0 (or an empty frame) is a no-op that
        // leaves dst untouched; the fused path must also touch nothing.
        const float strength = std::clamp(p.strength, 0.0f, 1.0f);
        if (strength <= 0.0f || w == 0 || h == 0) return;
        const auto t0 = std::chrono::steady_clock::now();
        auto& d = dynamic_cast<HipBuffer&>(dst);
        auto& b = dynamic_cast<const HipBuffer&>(backdrop);
        // Device-resident inputs (fused-path contract): the caller owns
        // residency — composite_many_into leaves its outputs on device, and
        // an upload() here would clobber them with stale host staging
        // (wiping the group to transparent). Host-filled callers upload
        // explicitly before calling.

        // Pyramid shapes on the host (same stop rule as buildPyramid).
        struct Dim {
            std::uint32_t w, h;
        };
        std::vector<Dim> dims;
        dims.push_back({w, h});
        while (!(dims.back().w <= 8 && dims.back().h <= 8) &&
               !(dims.back().w <= 1 && dims.back().h <= 1)) {
            dims.push_back({std::max<std::uint32_t>(1, dims.back().w / 2),
                            std::max<std::uint32_t>(1, dims.back().h / 2)});
        }
        const std::size_t depth = dims.size();
        const float lp = std::clamp(p.lowPass, 0.0f, 1.0f);
        const std::size_t li = std::min<std::size_t>(
            static_cast<std::size_t>(std::lround(lp * (depth - 1))), depth - 1);

        // Scratch: downsample levels (level 0 aliases the inputs) for both
        // fields, plus the two full-res upsampled fields. Pooled per canvas
        // size like the CUDA path (per-call hipMallocs would dwarf the
        // kernels).
        if (tonePoolW_ != w || tonePoolH_ != h || tonePool_.empty()) {
            tonePool_.clear();
            const std::size_t np = static_cast<std::size_t>(w) * h;
            for (std::size_t k = 1; k < depth; ++k) {
                const std::size_t bytes =
                    static_cast<std::size_t>(dims[k].w) * dims[k].h *
                    sizeof(float4);
                tonePool_.push_back(std::make_unique<HipBuffer>(bytes));
                tonePool_.push_back(std::make_unique<HipBuffer>(bytes));
            }
            tonePool_.push_back(std::make_unique<HipBuffer>(np * sizeof(float4)));
            tonePool_.push_back(std::make_unique<HipBuffer>(np * sizeof(float4)));
            tonePool_.push_back(
                std::make_unique<HipBuffer>(sizeof(KTonePivot)));
            tonePoolW_ = w;
            tonePoolH_ = h;
        }
        auto poolAt = [&](std::size_t k) -> HipBuffer* {
            return tonePool_[k].get();
        };
        auto levelG = [&](std::size_t k) -> float4* {
            return k == 0 ? d.dev() : poolAt(2 * (k - 1))->dev();
        };
        auto levelB = [&](std::size_t k) -> const float4* {
            return k == 0 ? b.dev() : poolAt(2 * (k - 1) + 1)->dev();
        };
        for (std::size_t k = 1; k < depth; ++k) {
            const int nn = static_cast<int>(dims[k].w) * static_cast<int>(dims[k].h);
            k_tone_down<<<(nn + kBlock - 1) / kBlock, kBlock>>>(
                levelG(k - 1), levelG(k), static_cast<int>(dims[k - 1].w),
                static_cast<int>(dims[k - 1].h), static_cast<int>(dims[k].w),
                static_cast<int>(dims[k].h));
            check(hipGetLastError(), "tone_blend down G launch");
            k_tone_down<<<(nn + kBlock - 1) / kBlock, kBlock>>>(
                levelB(k - 1), poolAt(2 * (k - 1) + 1)->dev(),
                static_cast<int>(dims[k - 1].w),
                static_cast<int>(dims[k - 1].h),
                static_cast<int>(dims[k].w),
                static_cast<int>(dims[k].h));
            check(hipGetLastError(), "tone_blend down B launch");
        }

        const std::size_t n = static_cast<std::size_t>(w) * h;
        HipBuffer* gLow = poolAt(2 * (depth - 1));
        HipBuffer* bLow = poolAt(2 * (depth - 1) + 1);
        k_tone_up<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            levelG(li), gLow->dev(), static_cast<int>(dims[li].w),
            static_cast<int>(dims[li].h), static_cast<int>(w), static_cast<int>(h));
        check(hipGetLastError(), "tone_blend up G launch");
        k_tone_up<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            levelB(li), bLow->dev(), static_cast<int>(dims[li].w),
            static_cast<int>(dims[li].h), static_cast<int>(w), static_cast<int>(h));
        check(hipGetLastError(), "tone_blend up B launch");

        HipBuffer* piv = poolAt(2 * (depth - 1) + 2);
        k_tone_pivot<<<1, kBlock>>>(bLow->dev(),
                                    static_cast<KTonePivot*>(piv->raw_dev()), n);
        check(hipGetLastError(), "tone_blend pivot launch");
        piv->download();
        const auto* pp = static_cast<const KTonePivot*>(piv->host());
        const float pivot = pp->count > 0
                                ? static_cast<float>(pp->sum / pp->count)
                                : 0.5f;
        tonePivot_ = pivot;
        tonePivotValid_ = true;
        const KToneParam kp{strength, std::clamp(p.color, 0.0f, 1.0f),
                            std::clamp(p.contrast, -1.0f, 1.0f), pivot};
        // Same-thread read-then-write: dst aliases the group input, which is
        // safe because each thread reads grp[i] before writing dst[i].
        k_tone_final<<<(n + kBlock - 1) / kBlock, kBlock>>>(
            d.dev(), gLow->dev(), bLow->dev(), d.dev(), n, kp);
        check(hipGetLastError(), "tone_blend final launch");
        PITTORE_LOG("[tone_blend] hip w=%u h=%u levels=%zu pick=%zu ms=%.3f", w,
                     h, depth, li,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
    }

    void tone_blend_region(Buffer& dst, const Buffer& backdrop,
                           std::uint32_t w, std::uint32_t h,
                           std::uint32_t rx0, std::uint32_t ry0,
                           std::uint32_t rx1, std::uint32_t ry1,
                           const ToneBlendParams& p) override {
        // Clamp region to frame
        rx0 = std::min(rx0, w);
        ry0 = std::min(ry0, h);
        rx1 = std::min(rx1, w);
        ry1 = std::min(ry1, h);
        if (rx0 >= rx1 || ry0 >= ry1) return;

        const float strength = std::clamp(p.strength, 0.0f, 1.0f);
        if (strength <= 0.0f || w == 0 || h == 0) return;
        const auto t0 = std::chrono::steady_clock::now();
        auto& d = dynamic_cast<HipBuffer&>(dst);
        auto& b = dynamic_cast<const HipBuffer&>(backdrop);

        // Halo must cover the footprint of one selected-level texel at full
        // resolution (see CUDA backend for the full rationale).
        struct Dim0 { std::uint32_t w, h; };
        std::vector<Dim0> fullDims;
        fullDims.push_back({w, h});
        while (!(fullDims.back().w <= 8 && fullDims.back().h <= 8) &&
               !(fullDims.back().w <= 1 && fullDims.back().h <= 1)) {
            fullDims.push_back({std::max<std::uint32_t>(1, fullDims.back().w / 2),
                                std::max<std::uint32_t>(1, fullDims.back().h / 2)});
        }
        const std::size_t fullDepth = fullDims.size();
        const float lp0 = std::clamp(p.lowPass, 0.0f, 1.0f);
        const std::size_t li0 = std::min<std::size_t>(
            static_cast<std::size_t>(std::lround(lp0 * (fullDepth - 1))), fullDepth - 1);
        const std::uint32_t lw0 = fullDims[li0].w, lh0 = fullDims[li0].h;
        const int halo = static_cast<int>(
            2 * std::max(w / std::max<std::uint32_t>(1, lw0),
                         h / std::max<std::uint32_t>(1, lh0)) + 8);
        const std::uint32_t x0 = (rx0 > static_cast<std::uint32_t>(halo)) ? rx0 - halo : 0;
        const std::uint32_t y0 = (ry0 > static_cast<std::uint32_t>(halo)) ? ry0 - halo : 0;
        const std::uint32_t x1 = std::min(rx1 + static_cast<std::uint32_t>(halo), w);
        const std::uint32_t y1 = std::min(ry1 + static_cast<std::uint32_t>(halo), h);
        const std::uint32_t rw = x1 - x0;
        const std::uint32_t rh = y1 - y0;

        // If region+halo covers >75% of frame, just do full-frame (simpler)
        const std::uint64_t regionArea = static_cast<std::uint64_t>(rw) * rh;
        const std::uint64_t frameArea = static_cast<std::uint64_t>(w) * h;
        if (regionArea * 4 >= frameArea * 3) {
            tone_blend(dst, backdrop, w, h, p);
            return;
        }

        // Pitches for 2D copies (staging copies happen below, after the
        // pooled scratch check so sizes are known to match).
        const std::size_t pitch = static_cast<std::size_t>(w) * sizeof(float4);
        const std::size_t regionPitch = static_cast<std::size_t>(rw) * sizeof(float4);

        // Region pyramid, extended to the full frame's depth when the region
        // alone would stop shallower (see CUDA backend): the selected level
        // must cover the same blur footprint as full frame's (li0).
        struct Dim { std::uint32_t w, h; };
        std::vector<Dim> dims;
        dims.push_back({rw, rh});
        while (dims.size() < fullDepth &&
               !(dims.back().w <= 1 && dims.back().h <= 1)) {
            dims.push_back({std::max<std::uint32_t>(1, dims.back().w / 2),
                            std::max<std::uint32_t>(1, dims.back().h / 2)});
        }
        const std::size_t depth = dims.size();
        const std::size_t li = std::min(li0, depth - 1);

        // Pooled region scratch (see CUDA backend for rationale).
        const std::size_t np = static_cast<std::size_t>(rw) * rh;
        if (toneRegionW_ != rw || toneRegionH_ != rh ||
            toneRegionDepth_ != depth || toneRegionPool_.empty()) {
            toneRegionPool_.clear();
            for (std::size_t k = 1; k < depth; ++k) {
                const std::size_t bytes =
                    static_cast<std::size_t>(dims[k].w) * dims[k].h *
                    sizeof(float4);
                toneRegionPool_.push_back(std::make_unique<HipBuffer>(bytes));
                toneRegionPool_.push_back(std::make_unique<HipBuffer>(bytes));
            }
            toneRegionPool_.push_back(std::make_unique<HipBuffer>(np * sizeof(float4)));
            toneRegionPool_.push_back(std::make_unique<HipBuffer>(np * sizeof(float4)));
            toneRegionPool_.push_back(std::make_unique<HipBuffer>(sizeof(KTonePivot)));
            const std::size_t regionBytes = np * sizeof(float4);
            toneRegionGrp_ = std::make_unique<HipBuffer>(regionBytes);
            toneRegionBdrop_ = std::make_unique<HipBuffer>(regionBytes);
            toneRegionW_ = rw;
            toneRegionH_ = rh;
            toneRegionDepth_ = depth;
        }

        auto rpoolAt = [&](std::size_t k) -> HipBuffer* {
            return toneRegionPool_[k].get();
        };
        check(hipMemcpy2D(toneRegionGrp_->raw_dev(), regionPitch,
                           static_cast<const char*>(d.raw_dev()) + static_cast<std::size_t>(y0) * pitch + static_cast<std::size_t>(x0) * sizeof(float4),
                           pitch, regionPitch, rh,
                           hipMemcpyDeviceToDevice),
              "tone_blend_region stage group");
        check(hipMemcpy2D(toneRegionBdrop_->raw_dev(), regionPitch,
                           static_cast<const char*>(b.raw_dev()) + static_cast<std::size_t>(y0) * pitch + static_cast<std::size_t>(x0) * sizeof(float4),
                           pitch, regionPitch, rh,
                           hipMemcpyDeviceToDevice),
              "tone_blend_region stage backdrop");
        float4* rgDev = static_cast<float4*>(toneRegionGrp_->raw_dev());
        const float4* rbDev = static_cast<const float4*>(toneRegionBdrop_->raw_dev());

        auto rlevelG = [&](std::size_t k) -> float4* {
            return k == 0 ? rgDev : static_cast<float4*>(rpoolAt(2 * (k - 1))->raw_dev());
        };
        auto rlevelB = [&](std::size_t k) -> const float4* {
            return k == 0 ? rbDev : static_cast<const float4*>(rpoolAt(2 * (k - 1) + 1)->raw_dev());
        };
        for (std::size_t k = 1; k < depth; ++k) {
            const int nn = static_cast<int>(dims[k].w) * static_cast<int>(dims[k].h);
            k_tone_down<<<(nn + kBlock - 1) / kBlock, kBlock>>>(
                rlevelG(k - 1), static_cast<float4*>(rpoolAt(2 * (k - 1))->raw_dev()),
                static_cast<int>(dims[k - 1].w),
                static_cast<int>(dims[k - 1].h),
                static_cast<int>(dims[k].w),
                static_cast<int>(dims[k].h));
            check(hipGetLastError(), "tone_blend_region down G launch");
            k_tone_down<<<(nn + kBlock - 1) / kBlock, kBlock>>>(
                rlevelB(k - 1), static_cast<float4*>(rpoolAt(2 * (k - 1) + 1)->raw_dev()),
                static_cast<int>(dims[k - 1].w),
                static_cast<int>(dims[k - 1].h),
                static_cast<int>(dims[k].w),
                static_cast<int>(dims[k].h));
            check(hipGetLastError(), "tone_blend_region down B launch");
        }

        float4* rgLow = static_cast<float4*>(rpoolAt(2 * (depth - 1))->raw_dev());
        float4* rbLow = static_cast<float4*>(rpoolAt(2 * (depth - 1) + 1)->raw_dev());
        k_tone_up<<<(np + kBlock - 1) / kBlock, kBlock>>>(
            rlevelG(li), rgLow, static_cast<int>(dims[li].w),
            static_cast<int>(dims[li].h), static_cast<int>(rw), static_cast<int>(rh));
        check(hipGetLastError(), "tone_blend_region up G launch");
        k_tone_up<<<(np + kBlock - 1) / kBlock, kBlock>>>(
            rlevelB(li), rbLow, static_cast<int>(dims[li].w),
            static_cast<int>(dims[li].h), static_cast<int>(rw), static_cast<int>(rh));
        check(hipGetLastError(), "tone_blend_region up B launch");

        // Cached global pivot (see CUDA backend): skips a device sync and
        // avoids regional-pivot seams when contrast != 0.
        float pivot = tonePivotValid_ ? tonePivot_ : 0.5f;
        if (!tonePivotValid_) {
            HipBuffer* rpiv = rpoolAt(2 * (depth - 1) + 2);
            k_tone_pivot<<<1, kBlock>>>(rbLow,
                                        static_cast<KTonePivot*>(rpiv->raw_dev()), np);
            check(hipGetLastError(), "tone_blend_region pivot launch");
            rpiv->download();
            const auto* rpp = static_cast<const KTonePivot*>(rpiv->host());
            pivot = rpp->count > 0
                        ? static_cast<float>(rpp->sum / rpp->count)
                        : 0.5f;
        }
        const KToneParam kp{strength, std::clamp(p.color, 0.0f, 1.0f),
                            std::clamp(p.contrast, -1.0f, 1.0f), pivot};
        k_tone_final<<<(np + kBlock - 1) / kBlock, kBlock>>>(
            rgDev, rgLow, rbLow, rgDev, np, kp);
        check(hipGetLastError(), "tone_blend_region final launch");

        // Copy the INNER region (without halo) back to dst.
        const std::uint32_t ix0 = rx0 - x0;
        const std::uint32_t iy0 = ry0 - y0;
        const std::uint32_t iw = rx1 - rx0;
        const std::uint32_t ih = ry1 - ry0;
        check(hipMemcpy2D(static_cast<char*>(d.raw_dev()) + static_cast<std::size_t>(ry0) * pitch + static_cast<std::size_t>(rx0) * sizeof(float4),
                           pitch,
                           static_cast<const char*>(toneRegionGrp_->raw_dev()) + static_cast<std::size_t>(iy0) * regionPitch + static_cast<std::size_t>(ix0) * sizeof(float4),
                           regionPitch,
                           static_cast<std::size_t>(iw) * sizeof(float4), ih,
                           hipMemcpyDeviceToDevice),
              "tone_blend_region writeback");
        // Sync for honest timing (matches full-frame path).
        check(hipDeviceSynchronize(), "tone_blend_region sync");
        PITTORE_LOG("[tone_blend] hip region=(%u,%u,%u,%u) halo=%d ms=%.3f",
                     rx0, ry0, rx1, ry1, halo,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
    }

  private:
    int ordinal_;
    std::string name_;
    Device device_;
    std::unique_ptr<HipBuffer> argb_;
    std::unique_ptr<HipBuffer> many_;
    std::unique_ptr<HipBuffer> cum_;
    std::unique_ptr<HipBuffer> rows_;
    // Host-side scratch for the composite_many_into pass (param array, kept
    // layers, tallies, prefix starts, cursors, cells). Cleared per frame,
    // never freed in steady state.
    std::vector<KPlacedManyParam> compHost_;
    std::vector<PlacedLayer> compKept_;
    std::vector<int> compPer_;
    std::vector<int> compRow_;
    std::vector<int> compCurs_;
    std::vector<KRowCell> compCells_;
    std::unique_ptr<HipBuffer> warp_;  // Liquify subgrid staging (grown on demand)
    // Blur scratch pool (frame temps + gaussian kernel consts), grown on
    // demand and reused across calls. Two frame slots: slot A for direct
    // blur temps, slot B for sharpen's temp (gaussian_blur reuses slot A
    // internally, so sharing one slot would alias).
    std::vector<std::unique_ptr<HipBuffer>> blurPool_;
    std::unique_ptr<HipBuffer> blurKernel_;
    HipBuffer& blurFrame(std::size_t bytes, bool second = false) {
        const std::size_t idx = second ? 1 : 0;
        if (blurPool_.size() < 2) blurPool_.resize(2);
        auto& slot = blurPool_[idx];
        if (!slot || slot->size() != bytes)
            slot = std::make_unique<HipBuffer>(bytes);
        return *slot;
    }
    HipBuffer& blurKernel(std::size_t bytes) {
        if (!blurKernel_ || blurKernel_->size() != bytes)
            blurKernel_ = std::make_unique<HipBuffer>(bytes);
        return *blurKernel_;
    }
    // Tone-blend scratch pool (pyramid levels, upsampled fields, pivot
    // buffer), sized for the last (w, h); see the CUDA path for the layout.
    std::vector<std::unique_ptr<HipBuffer>> tonePool_;
    std::uint32_t tonePoolW_ = 0;
    std::uint32_t tonePoolH_ = 0;
    // Last global pivot from a full-frame tone_blend (see CUDA backend).
    float tonePivot_ = 0.5f;
    bool tonePivotValid_ = false;
    // Region tone-blend scratch: single-slot cache keyed by (rw, rh, depth).
    std::vector<std::unique_ptr<HipBuffer>> toneRegionPool_;
    std::uint32_t toneRegionW_ = 0;
    std::uint32_t toneRegionH_ = 0;
    std::size_t toneRegionDepth_ = 0;
    std::unique_ptr<HipBuffer> toneRegionGrp_;
    std::unique_ptr<HipBuffer> toneRegionBdrop_;
};

}  // namespace

std::vector<Device> hip_devices() {
    int count = 0;
    if (hipGetDeviceCount(&count) != hipSuccess || count < 0) count = 0;
    std::vector<Device> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        hipDeviceProp_t prop{};
        if (hipGetDeviceProperties(&prop, i) != hipSuccess) continue;
        Device d;
        d.type = BackendType::HIP;
        d.name = prop.name;
        d.memory_mb = prop.totalGlobalMem >> 20;
        d.compute_major = prop.major;
        d.compute_minor = prop.minor;
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<ComputeBackend> make_hip_backend() {
    auto devices = hip_devices();
    if (devices.empty()) return nullptr;
    return std::make_unique<HipBackend>(0);
}

}  // namespace pittore::compute

#endif  // PITTORE_HAS_HIP