#include "engine/compute/cpu_backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/compute/paint.h"
#include "engine/compute/brushes/erase/erase.h"
#include "engine/compute/brushes/history/history_dab.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/adjust.h"
#include "engine/compute/dither.h"
#include "engine/compute/tone_blend.h"
#include "engine/core/log.h"
#include "engine/core/parallel.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

namespace {

// Reused full-frame scratch owned by the calling thread. parallel_rows
// workers write disjoint rows and join before return, so sharing is safe
// (no nesting: none of these helpers calls another). Never shrinks.
// Two slots: slot 0 is blur/chained scratch, slot 1 is the caller's output
// (e.g. sharpen's blur field while cpu_blur works in slot 0).
std::vector<RGBAf>& tls_frame(std::size_t n, int slot = 0) {
    thread_local std::vector<RGBAf> buf0;
    thread_local std::vector<RGBAf> buf1;
    std::vector<RGBAf>& buf = slot == 0 ? buf0 : buf1;
    if (buf.size() < n) buf.resize(n);
    return buf;
}

// Host-side buffer: the CPU backend's "device" is host memory, so upload and
// download are identity operations.
class CpuBuffer final : public Buffer {
  public:
    explicit CpuBuffer(std::size_t bytes) : size_(bytes), data_(bytes, 0) {}

    std::size_t size() const override { return size_; }
    void* host() override { return data_.data(); }
    const void* host() const override { return data_.data(); }
    void upload() const override {}
    void download() const override {}

  private:
    std::size_t size_;
    std::vector<unsigned char> data_;
};

constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

void cpu_grayscale(const RGBAf* src, RGBAf* dst, std::uint32_t w,
                     std::uint32_t h) {
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const std::size_t base = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                const RGBAf& s = src[base + x];
                const float yy = kLumaR * s.r + kLumaG * s.g + kLumaB * s.b;
                RGBAf& d = dst[base + x];
                d.r = d.g = d.b = yy;
                d.a = s.a;
            }
        }
    });
}

// Full-canvas composite: delegates to the shared blend module (blend.h/cpp),
// which every backend and the kernel-parity tests draw from.
void cpu_composite(RGBAf* dst, const RGBAf* src, std::size_t n, BlendMode mode,
                   std::size_t w) {
    blend::composite_buffer(dst, src, n, mode, w);
}

std::vector<float> make_kernel(float sigma, int& radius) {
    // Requires sigma > 0. Radius covers 3 sigma (99.7%) of the gaussian mass.
    radius = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float t = static_cast<float>(i) / sigma;
        const float v = std::exp(-0.5f * t * t);
        kernel[static_cast<std::size_t>(i + radius)] = v;
        sum += v;
    }
    const float inv = 1.0f / sum;
    for (float& k : kernel) k *= inv;
    return kernel;
}

void cpu_blur(const RGBAf* src, RGBAf* dst, std::uint32_t w, std::uint32_t h,
              float sigma) {
    if (sigma <= 0.0f) {
        if (src != dst) std::memcpy(dst, src, static_cast<std::size_t>(w) * h * sizeof(RGBAf));
        return;
    }

    int radius = 0;
    const std::vector<float> kernel = make_kernel(sigma, radius);
    const int r = radius;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    RGBAf* tmp = tls_frame(n).data();

    const auto hbound = static_cast<int>(w) - 1;
    const auto vbound = static_cast<int>(h) - 1;

    // Horizontal pass: src -> tmp. Rows disjoint: bit-identical.
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                RGBAf acc{};
                for (int j = -r; j <= r; ++j) {
                    const int xx = std::clamp(static_cast<int>(x) + j, 0, hbound);
                    const RGBAf& p = src[row + static_cast<std::size_t>(xx)];
                    const float k = kernel[static_cast<std::size_t>(j + r)];
                    acc.r += k * p.r;
                    acc.g += k * p.g;
                    acc.b += k * p.b;
                    acc.a += k * p.a;
                }
                tmp[row + x] = acc;
            }
        }
    });

    // Vertical pass: tmp -> dst.
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                RGBAf acc{};
                for (int j = -r; j <= r; ++j) {
                    const int yy = std::clamp(static_cast<int>(y) + j, 0, vbound);
                    const RGBAf& p = tmp[static_cast<std::size_t>(yy) * w + x];
                    const float k = kernel[static_cast<std::size_t>(j + r)];
                    acc.r += k * p.r;
                    acc.g += k * p.g;
                    acc.b += k * p.b;
                    acc.a += k * p.a;
                }
                dst[static_cast<std::size_t>(y) * w + x] = acc;
            }
        }
    });
}

}  // namespace

namespace {

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// --- look kernels -----------------------------------------------------------
// Pixel-exact ports of the CUDA/HIP kernels (cuda_backend.cu k_* / hip mirror)
// so cpu<->gpu parity holds everywhere these are exercised.
//
// Unsharp: out = src + amount * (src - blur). The gating magnitude is the
// signed mean difference, matching the GPU kernel exactly.
void cpu_sharpen(const RGBAf* src, RGBAf* dst, std::uint32_t w, std::uint32_t h,
                 float amount, float radius, float threshold) {
    const auto n = static_cast<std::size_t>(w) * h;
    RGBAf* blur = tls_frame(n, 1).data();
    cpu_blur(src, blur, w, h, radius);  // GPU sharpen passes radius as sigma
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const std::size_t base = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = base + x;
                const RGBAf& s = src[i];
                const RGBAf& b = blur[i];
                RGBAf d = s;
                const float diff = ((s.r - b.r) + (s.g - b.g) + (s.b - b.b)) / 3.0f;
                if (std::abs(diff) >= threshold) {
                    d.r = s.r + amount * (s.r - b.r);
                    d.g = s.g + amount * (s.g - b.g);
                    d.b = s.b + amount * (s.b - b.b);
                }
                d.r = clamp01(d.r);
                d.g = clamp01(d.g);
                d.b = clamp01(d.b);
                dst[i] = d;
            }
        }
    });
}

// out = clamp((src + brightness) * (1 + contrast)). Exact arithmetic.
void cpu_brightness_contrast(const RGBAf* src, RGBAf* dst, std::uint32_t w,
                             std::uint32_t h, float brightness, float contrast) {
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const std::size_t base = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                const RGBAf& s = src[base + x];
                RGBAf& d = dst[base + x];
                d.r = clamp01((s.r + brightness) * (1.0f + contrast));
                d.g = clamp01((s.g + brightness) * (1.0f + contrast));
                d.b = clamp01((s.b + brightness) * (1.0f + contrast));
                d.a = s.a;
            }
        }
    });
}

// HSL round-trip, normalized hue in [0,1) — mirrors the GPU kernels.
float hue_to_rgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void rgb_to_hsl(float r, float g, float b, float& h, float& s, float& l) {
    const float cmax = std::max(r, std::max(g, b));
    const float cmin = std::min(r, std::min(g, b));
    l = 0.5f * (cmax + cmin);
    if (cmax == cmin) {
        h = s = 0.0f;
        return;
    }
    const float d = cmax - cmin;
    s = d / (1.0f - std::abs(2.0f * l - 1.0f));
    // Matches adjust::rgb_to_hsl exactly (subtract keeps h in [0,6) since
    // |(g-b)|/d <= 1); no libm call.
    if (cmax == r) {
        h = (g - b) / d + 6.0f;
        if (h >= 6.0f) h -= 6.0f;
    }
    else if (cmax == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

void hsl_to_rgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s == 0.0f) {
        r = g = b = l;
        return;
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
}

void cpu_hue_saturation(const RGBAf* src, RGBAf* dst, std::uint32_t w,
                        std::uint32_t h, float hue_shift, float sat_adj,
                        float light_adj) {
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const std::size_t base = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                const RGBAf& s = src[base + x];
                float hh, sat, l;
                rgb_to_hsl(s.r, s.g, s.b, hh, sat, l);
                // Same domain as adjust::frac_unit (hue fraction in [0,1)
                // plus shift/360 in [-0.5,0.5] plus 1): branch subtracts are
                // correctly-rounded, matching fmod bit-for-bit here.
                hh = hh + hue_shift / 360.0f + 1.0f;
                if (hh >= 2.0f) hh -= 2.0f;
                else if (hh >= 1.0f) hh -= 1.0f;
                sat = clamp01(sat * (1.0f + sat_adj));
                l = clamp01(l + light_adj);
                float r, g, b;
                hsl_to_rgb(hh, sat, l, r, g, b);
                dst[base + x] = RGBAf{r, g, b, s.a};
            }
        }
    });
}

// 3x3 per-channel median, all four channels (GPU kernel sorts RGBA alike).
// Rows disjoint with stack-only scratch: bit-identical threaded.
void cpu_median_3x3(const RGBAf* src, RGBAf* dst, std::uint32_t w,
                    std::uint32_t h) {
    const auto hbound = static_cast<int>(w) - 1;
    const auto vbound = static_cast<int>(h) - 1;
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                float vals[4][9];
                int idx = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = std::clamp(static_cast<int>(x) + dx, 0, hbound);
                        const int yy = std::clamp(static_cast<int>(y) + dy, 0, vbound);
                        const RGBAf& p = src[static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx)];
                        vals[0][idx] = p.r;
                        vals[1][idx] = p.g;
                        vals[2][idx] = p.b;
                        vals[3][idx] = p.a;
                        ++idx;
                    }
                }
                for (int c = 0; c < 4; ++c) {
                    for (int i = 1; i < 9; ++i) {
                        const float key = vals[c][i];
                        int j = i - 1;
                        while (j >= 0 && vals[c][j] > key) { vals[c][j + 1] = vals[c][j]; --j; }
                        vals[c][j + 1] = key;
                    }
                }
                RGBAf& d = dst[row + x];
                d.r = vals[0][4];
                d.g = vals[1][4];
                d.b = vals[2][4];
                d.a = vals[3][4];
            }
        }
    });
}

}  // namespace

Device cpu_device() {
    Device d;
    d.type = BackendType::CPU;
    d.name = "CPU (" + std::to_string(std::thread::hardware_concurrency()) + " threads)";
    d.memory_mb = 0;  // system RAM; not a meaningful "device" budget
    return d;
}

CpuBackend::CpuBackend() : name_("CPU"), device_(cpu_device()) {}

std::unique_ptr<Buffer> CpuBackend::make_buffer(std::size_t bytes) {
    return std::make_unique<CpuBuffer>(bytes);
}

void CpuBackend::grayscale(Buffer& src, Buffer& dst, std::uint32_t w,
                           std::uint32_t h) {
    if (src.size() != dst.size())
        throw std::invalid_argument("grayscale buffers differ in size");
    cpu_grayscale(static_cast<const RGBAf*>(src.host()),
                  static_cast<RGBAf*>(dst.host()), w, h);
}

void CpuBackend::composite(Buffer& bottom, const Buffer& top, std::uint32_t w,
                           std::uint32_t h, BlendMode mode) {
    if (bottom.size() != top.size())
        throw std::invalid_argument("composite buffers differ in size");
    const auto t0 = std::chrono::steady_clock::now();
    cpu_composite(static_cast<RGBAf*>(bottom.host()),
                  static_cast<const RGBAf*>(top.host()),
                  static_cast<std::size_t>(w) * h, mode, w);
    PITTORE_LOG("[composite] cpu full w=%u h=%u mode=%d ms=%.3f", w, h,
                 static_cast<int>(mode),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void CpuBackend::composite_region(Buffer& bottom, const Buffer& top,
                                  std::uint32_t w, std::uint32_t /*h*/,
                                  std::uint32_t x0, std::uint32_t y0,
                                  std::uint32_t x1, std::uint32_t y1,
                                  BlendMode mode) {
    if (bottom.size() != top.size())
        throw std::invalid_argument("composite_region buffers differ in size");
    composite_region_host(static_cast<RGBAf*>(bottom.host()),
                          static_cast<const RGBAf*>(top.host()), w, x0, y0, x1, y1,
                          mode);
}

void CpuBackend::composite_placed(Buffer& bottom, const Buffer& src,
                                  std::uint32_t sw, std::uint32_t sh,
                                  double ox, double oy, double sx, double sy,
                                  std::uint32_t w, std::uint32_t x0,
                                  std::uint32_t y0, std::uint32_t x1,
                                  std::uint32_t y1, float alpha_fold,
                                  BlendMode mode) {
    composite_placed_host(static_cast<RGBAf*>(bottom.host()),
                          static_cast<const RGBAf*>(src.host()), sw, sh, ox, oy,
                          sx, sy, w, x0, y0, x1, y1, alpha_fold, mode);
}

void CpuBackend::gaussian_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                               std::uint32_t h, float sigma) {
    if (src.size() != dst.size())
        throw std::invalid_argument("gaussian_blur buffers differ in size");
    cpu_blur(static_cast<const RGBAf*>(src.host()),
             static_cast<RGBAf*>(dst.host()), w, h, sigma);
}

void CpuBackend::sharpen(Buffer& src, Buffer& dst, std::uint32_t w,
                          std::uint32_t h, float amount, float radius,
                          float threshold) {
    if (src.size() != dst.size())
        throw std::invalid_argument("sharpen buffers differ in size");
    cpu_sharpen(static_cast<const RGBAf*>(src.host()),
                static_cast<RGBAf*>(dst.host()), w, h, amount, radius, threshold);
}

void CpuBackend::brightness_contrast(Buffer& src, Buffer& dst, std::uint32_t w,
                                      std::uint32_t h, float brightness,
                                      float contrast) {
    if (src.size() != dst.size())
        throw std::invalid_argument("brightness_contrast buffers differ in size");
    cpu_brightness_contrast(static_cast<const RGBAf*>(src.host()),
                            static_cast<RGBAf*>(dst.host()), w, h, brightness,
                            contrast);
}

void CpuBackend::hue_saturation(Buffer& src, Buffer& dst, std::uint32_t w,
                                 std::uint32_t h, float hue_shift,
                                 float saturation, float lightness) {
    if (src.size() != dst.size())
        throw std::invalid_argument("hue_saturation buffers differ in size");
    cpu_hue_saturation(static_cast<const RGBAf*>(src.host()),
                       static_cast<RGBAf*>(dst.host()), w, h, hue_shift,
                       saturation, lightness);
}

void CpuBackend::median_filter(Buffer& src, Buffer& dst, std::uint32_t w,
                                std::uint32_t h) {
    if (src.size() != dst.size())
        throw std::invalid_argument("median_filter buffers differ in size");
    cpu_median_3x3(static_cast<const RGBAf*>(src.host()),
                   static_cast<RGBAf*>(dst.host()), w, h);
}

// Base-class default: a region composite without a device kernel degrades to
// the full composite (correct for every backend, just not incremental). CPU and
// GPU backends override with a true region path.
void ComputeBackend::composite_region(Buffer& bottom, const Buffer& top,
                                      std::uint32_t w, std::uint32_t h,
                                      std::uint32_t /*x0*/, std::uint32_t /*y0*/,
                                      std::uint32_t /*x1*/, std::uint32_t /*y1*/,
                                      BlendMode mode) {
    composite(bottom, top, w, h, mode);
}

// Base-class default: route the fused placed-layer composite through host
// staging (pull the accumulator back, sample + blend in host memory, push it
// up again). Correct for every backend; CPU overrides with the host-only path
// and GPU backends with device kernels.
void ComputeBackend::composite_placed(Buffer& bottom, const Buffer& src,
                                      std::uint32_t sw, std::uint32_t sh,
                                      double ox, double oy, double sx, double sy,
                                      std::uint32_t w, std::uint32_t x0,
                                      std::uint32_t y0, std::uint32_t x1,
                                      std::uint32_t y1, float alpha_fold,
                                      BlendMode mode) {
    if (bottom.size() < static_cast<std::size_t>(w) * (y1 - y0) * sizeof(RGBAf))
        throw std::invalid_argument("composite_placed buffer too small");
    bottom.download();
    composite_placed_host(static_cast<RGBAf*>(bottom.host()),
                          static_cast<const RGBAf*>(src.host()), sw, sh, ox, oy,
                          sx, sy, w, x0, y0, x1, y1, alpha_fold, mode);
    bottom.upload();
}

// Base-class default: no device kernel for the persistent path, so fall back
// to composite_placed (host staging). Correct everywhere; CPU ends up in
// composite_placed_host, GPU backends override with a kernel that skips the
// accumulator transfers.
void ComputeBackend::composite_placed_into(
    Buffer& bottom, const Buffer& src, std::uint32_t sw, std::uint32_t sh,
    double ox, double oy, double sx, double sy, std::uint32_t w,
    std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1,
    float alpha_fold, BlendMode mode) {
    composite_placed(bottom, src, sw, sh, ox, oy, sx, sy, w, x0, y0, x1, y1,
                     alpha_fold, mode);
}

// Base-class default: no device kernel for the batched persistent path either,
// so run the shared host row walk. Correct for every backend that does not
// override (CPU and generic fallbacks; HIP/CUDA override with a single fused
// kernel). The region bounds the destination pixels that may change; per-layer
// windows are already doc-clipped by the caller.
void ComputeBackend::composite_many_into(Buffer& bottom, std::uint32_t w,
                                         std::uint32_t rx0,
                                         std::uint32_t ry0,
                                         std::uint32_t rx1,
                                         std::uint32_t ry1,
                                         const PlacedLayer* layers,
                                         std::size_t count) {
    if (!layers || count == 0 || w == 0 || rx0 >= rx1 || ry0 >= ry1) return;
    if (bottom.size() == 0 ||
        bottom.size() % (static_cast<std::size_t>(w) * sizeof(RGBAf)) != 0)
        throw std::invalid_argument("composite_many_into buffer too small");
    const auto h = static_cast<std::uint32_t>(
        bottom.size() / (static_cast<std::size_t>(w) * sizeof(RGBAf)));
    bottom.download();

    std::vector<HostPlacedLayer> host;
    host.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const PlacedLayer& l = layers[i];
        if (l.x0 >= l.x1 || l.y0 >= l.y1) continue;
        const RGBAf* src = nullptr;
        if (!l.isAdjustment && !l.isFill) {
            if (!l.src || l.sw == 0 || l.sh == 0) continue;
            if (l.src->size() <
                static_cast<std::size_t>(l.sw) * l.sh * sizeof(RGBAf))
                throw std::invalid_argument("composite_many_into source too small");
            l.src->download();
            src = static_cast<const RGBAf*>(l.src->host());
        }
        const RGBAf* mask = nullptr;
        if (l.mask) {
            if (l.msw == 0 || l.msh == 0 ||
                l.mask->size() <
                    static_cast<std::size_t>(l.msw) * l.msh * sizeof(RGBAf))
                throw std::invalid_argument("composite_many_into mask too small");
            l.mask->download();
            mask = static_cast<const RGBAf*>(l.mask->host());
        }
        const float* aux = nullptr;
        if (l.adjAux) {
            // Curves carries 3x256 floats (R/G/B), Levels a single 256 table.
            const std::size_t need =
                (l.adjKind == static_cast<int>(AdjustmentKind::Curves)
                     ? 768
                     : 256) *
                sizeof(float);
            if (l.adjAux->size() < need)
                throw std::invalid_argument("composite_many_into curve LUT too small");
            l.adjAux->download();
            aux = static_cast<const float*>(l.adjAux->host());
        }
        HostPlacedLayer e;
        e.src = src;
        e.sw = l.sw;
        e.sh = l.sh;
        e.ox = l.ox;
        e.oy = l.oy;
        e.sx = l.sx;
        e.sy = l.sy;
        e.x0 = l.x0;
        e.y0 = l.y0;
        e.x1 = l.x1;
        e.y1 = l.y1;
        e.fold = l.fold;
        e.mode = l.mode;
        e.mask = mask;
        e.msw = l.msw;
        e.msh = l.msh;
        e.mox = l.mox;
        e.moy = l.moy;
        e.msx = l.msx;
        e.msy = l.msy;
        e.clipped = l.clipped;
        e.clipBase = l.clipBase;
        e.isAdjustment = l.isAdjustment;
        e.isFill = l.isFill;
        e.fillColor = l.fillColor;
        e.adjKind = l.adjKind;
        for (int k = 0; k < 16; ++k) e.adjP[k] = l.adjP[k];
        e.adjAux = aux;
        host.push_back(e);
    }
    composite_many_host(static_cast<RGBAf*>(bottom.host()), w, h, rx0, ry0,
                        rx1, ry1, host.data(), host.size());
    bottom.upload();
}

// Base-class default: host-side zero fill. GPU backends override with a device
// memset so no host/device transfer is needed.
void ComputeBackend::clear(Buffer& dst) {
    if (dst.size()) std::memset(dst.host(), 0, dst.size());
}

// Base-class default: pull the buffer back to host staging and convert there.
// The public preview canvas path always runs the CPU loop; GPU backends
// override with a device convert + DMA so the host never touches the pixels.
void ComputeBackend::blit_premul(const Buffer& src, std::uint8_t* dst,
                                 std::uint32_t w, std::uint32_t x0,
                                 std::uint32_t y0, std::uint32_t x1,
                                 std::uint32_t y1, std::size_t dst_pitch) {
    if (x0 >= x1 || y0 >= y1) return;
    const_cast<Buffer&>(src).download();
    const RGBAf* px = static_cast<const RGBAf*>(src.host());
    // Rows write disjoint dst rows (dst_pitch stride): bit-identical.
    pittore::core::parallel_rows(y1 - y0, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            std::uint8_t* out = dst + static_cast<std::size_t>(y) * dst_pitch;
            const RGBAf* row = px + static_cast<std::size_t>(y0 + y) * w;
            for (std::uint32_t x = x0; x < x1; ++x) {
                const std::uint32_t ax = x, ay = y0 + y;
                const RGBAf& s = row[x];
                out[0] = pittore::compute::dither::quantize(s.b * s.a, ax, ay);
                out[1] = pittore::compute::dither::quantize(s.g * s.a, ax, ay);
                out[2] = pittore::compute::dither::quantize(s.r * s.a, ax, ay);
                out[3] = pittore::compute::dither::quantize(s.a, ax, ay);
                out += 4;
            }
        }
    });
}

// Base-class default: pass the dab through host staging. Works for every
// backend; CpuBackend keeps it (host == device there), GPU backends that
// want native kernels can override later.
void ComputeBackend::paint_dab(Buffer& dst, std::uint32_t w, std::uint32_t h,
                               float cx, float cy, float radius,
                               float hardness, float opacity,
                               const RGBAf& color) {
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("paint_dab buffer too small");
    dst.download();
    paint_dab_host(static_cast<RGBAf*>(dst.host()), w, h, cx, cy, radius,
                   hardness, opacity, color);
    dst.upload();
}

void CpuBackend::paint_dab(Buffer& dst, std::uint32_t w, std::uint32_t h,
                           float cx, float cy, float radius, float hardness,
                           float opacity, const RGBAf& color) {
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("paint_dab buffer too small");
    paint_dab_host(static_cast<RGBAf*>(dst.host()), w, h, cx, cy, radius,
                   hardness, opacity, color);
}

// Base-class default: pass the dab through host staging. Works for every
// backend; CpuBackend keeps it (host == device there), GPU backends with
// native kernels override below.
void ComputeBackend::bg_erase(Buffer& dst, std::uint32_t w, std::uint32_t h,
                              float cx, float cy, float radius,
                              float hardness, float opacity,
                              const RGBAf& sample, float tolerance,
                              bool protectFg, const RGBAf& fg, int* bboxOut) {
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("bg_erase buffer too small");
    dst.download();
    background_erase_dab_host(static_cast<RGBAf*>(dst.host()), w, h, cx, cy,
                              radius, hardness, opacity, sample, tolerance,
                              protectFg, fg, bboxOut);
    dst.upload();
}

void CpuBackend::bg_erase(Buffer& dst, std::uint32_t w, std::uint32_t h,
                           float cx, float cy, float radius, float hardness,
                           float opacity, const RGBAf& sample, float tolerance,
                           bool protectFg, const RGBAf& fg, int* bboxOut) {
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("bg_erase buffer too small");
    background_erase_dab_host(static_cast<RGBAf*>(dst.host()), w, h, cx, cy,
                              radius, hardness, opacity, sample, tolerance,
                              protectFg, fg, bboxOut);
}

// Base-class default: pass the dab through host staging.
void ComputeBackend::pattern_stamp(Buffer& dst, std::uint32_t w,
                                   std::uint32_t h, float cx, float cy,
                                   float radius, float hardness, float opacity,
                                   const Buffer& tile, float ox, float oy,
                                   int* bboxOut) {
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("pattern_stamp buffer too small");
    if (tile.size() < 64u * 64u * sizeof(RGBAf))
        throw std::invalid_argument("pattern_stamp tile too small");
    dst.download();
    tile.download();
    pattern_stamp_dab_host(
        static_cast<RGBAf*>(dst.host()), w, h, cx, cy, radius, hardness,
        opacity,
        PatternTile{-1,
                    std::vector<RGBAf>(
                        static_cast<const RGBAf*>(tile.host()),
                        static_cast<const RGBAf*>(tile.host()) + 64u * 64u)},
        ox, oy, bboxOut);
    dst.upload();
}

void CpuBackend::pattern_stamp(Buffer& dst, std::uint32_t w, std::uint32_t h,
                                float cx, float cy, float radius,
                                float hardness, float opacity,
                                const Buffer& tile, float ox, float oy,
                                int* bboxOut) {
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("pattern_stamp buffer too small");
    if (tile.size() < 64u * 64u * sizeof(RGBAf))
        throw std::invalid_argument("pattern_stamp tile too small");
    pattern_stamp_dab_host(
        static_cast<RGBAf*>(dst.host()), w, h, cx, cy, radius, hardness,
        opacity,
        PatternTile{-1,
                    std::vector<RGBAf>(
                        static_cast<const RGBAf*>(tile.host()),
                        static_cast<const RGBAf*>(tile.host()) + 64u * 64u)},
        ox, oy, bboxOut);
}

// Base-class default: pass the dab through host staging.
void ComputeBackend::history_dab(Buffer& dst, std::uint32_t w,
                                 std::uint32_t h, float cx, float cy,
                                 float radius, float hardness, float opacity,
                                 const Buffer& src, int* bboxOut) {
    const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
    if (dst.size() < need || src.size() < need)
        throw std::invalid_argument("history_dab buffer too small");
    dst.download();
    src.download();
    history_brush_dab_host(
        static_cast<RGBAf*>(dst.host()),
        static_cast<const RGBAf*>(src.host()), w, h, cx, cy, radius,
        hardness, opacity, bboxOut);
    dst.upload();
}

void CpuBackend::history_dab(Buffer& dst, std::uint32_t w, std::uint32_t h,
                              float cx, float cy, float radius, float hardness,
                              float opacity, const Buffer& src,
                              int* bboxOut) {
    const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
    if (dst.size() < need || src.size() < need)
        throw std::invalid_argument("history_dab buffer too small");
    history_brush_dab_host(
        static_cast<RGBAf*>(dst.host()),
        static_cast<const RGBAf*>(src.host()), w, h, cx, cy, radius,
        hardness, opacity, bboxOut);
}

// Base-class default: pass the transfer through host staging. Works for
// every backend; CpuBackend keeps it (host == device there), fused device
// kernels override per backend later.
void ComputeBackend::tone_blend(Buffer& dst, const Buffer& backdrop,
                                std::uint32_t w, std::uint32_t h,
                                const ToneBlendParams& p) {
    const std::size_t need = static_cast<std::size_t>(w) * h * sizeof(RGBAf);
    if (dst.size() < need || backdrop.size() < need)
        throw std::invalid_argument("tone_blend buffer too small");
    backdrop.download();
    dst.download();
    applyToneBlend(static_cast<RGBAf*>(dst.host()),
                   static_cast<const RGBAf*>(backdrop.host()),
                   static_cast<RGBAf*>(dst.host()), w, h, p);
    dst.upload();
}

void ComputeBackend::tone_blend_region(Buffer& dst, const Buffer& backdrop,
                                       std::uint32_t w, std::uint32_t h,
                                       std::uint32_t, std::uint32_t,
                                       std::uint32_t, std::uint32_t,
                                       const ToneBlendParams& p) {
    // Default: fall back to full-frame (correct but slower)
    tone_blend(dst, backdrop, w, h, p);
}

namespace {

inline std::uint32_t repClamp(int v, std::uint32_t hi) {
    if (v < 0) return 0;
    if (v >= static_cast<int>(hi)) return hi - 1;
    return static_cast<std::uint32_t>(v);
}

void hostBoxBlur(const RGBAf* src, RGBAf* dst, std::uint32_t w,
                 std::uint32_t h, int radius) {
    // Sliding clamped-edge window: identical taps to the naive box (each of
    // the 2rr+1 taps clamped into range, matching boxBlurInto and the GPU
    // k_box kernels), but O(1) per pixel instead of O(rr). Double running
    // sums keep rounding at the naive level; alpha passes through.
    const int rr = std::max(1, radius);
    if (w == 0 || h == 0) return;
    const double n = static_cast<double>(2 * rr + 1);
    const std::size_t np = static_cast<std::size_t>(w) * h;
    RGBAf* tmp = tls_frame(np).data();
    // H-pass: rows independent.
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            const RGBAf* row = src + static_cast<std::size_t>(y) * w;
            RGBAf* trow = tmp + static_cast<std::size_t>(y) * w;
            double ar = 0.0, ag = 0.0, ab = 0.0;
            for (int dx = -rr; dx <= rr; ++dx) {
                const RGBAf& p = row[repClamp(dx, w)];
                ar += p.r;
                ag += p.g;
                ab += p.b;
            }
            for (std::uint32_t x = 0; x < w; ++x) {
                const int xi = static_cast<int>(x);
                RGBAf& o = trow[x];
                o.r = static_cast<float>(ar / n);
                o.g = static_cast<float>(ag / n);
                o.b = static_cast<float>(ab / n);
                o.a = row[x].a;
                const RGBAf& out = row[repClamp(xi - rr, w)];
                const RGBAf& in = row[repClamp(xi + rr + 1, w)];
                ar += static_cast<double>(in.r) - out.r;
                ag += static_cast<double>(in.g) - out.g;
                ab += static_cast<double>(in.b) - out.b;
            }
        }
    });
    // V-pass: columns independent (own running sums per strip).
    pittore::core::parallel_cols(w, [&](std::uint32_t x0, std::uint32_t x1) {
        const std::size_t strip = static_cast<std::size_t>(x1 - x0);
        std::vector<double> ar(strip, 0.0), ag(strip, 0.0), ab(strip, 0.0);
        for (int dy = -rr; dy <= rr; ++dy) {
            const RGBAf* row =
                tmp + static_cast<std::size_t>(repClamp(dy, h)) * w + x0;
            for (std::uint32_t x = 0; x < strip; ++x) {
                ar[x] += row[x].r;
                ag[x] += row[x].g;
                ab[x] += row[x].b;
            }
        }
        for (std::uint32_t y = 0; y < h; ++y) {
            const int yi = static_cast<int>(y);
            RGBAf* orow = dst + static_cast<std::size_t>(y) * w + x0;
            const RGBAf* srow = src + static_cast<std::size_t>(y) * w + x0;
            for (std::uint32_t x = 0; x < strip; ++x) {
                orow[x].r = static_cast<float>(ar[x] / n);
                orow[x].g = static_cast<float>(ag[x] / n);
                orow[x].b = static_cast<float>(ab[x] / n);
                orow[x].a = srow[x].a;
            }
            const RGBAf* drop =
                tmp + static_cast<std::size_t>(repClamp(yi - rr, h)) * w + x0;
            const RGBAf* add =
                tmp + static_cast<std::size_t>(repClamp(yi + rr + 1, h)) * w + x0;
            for (std::uint32_t x = 0; x < strip; ++x) {
                ar[x] += static_cast<double>(add[x].r) - drop[x].r;
                ag[x] += static_cast<double>(add[x].g) - drop[x].g;
                ab[x] += static_cast<double>(add[x].b) - drop[x].b;
            }
        }
    });
}

}  // namespace

void ComputeBackend::box_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                              std::uint32_t h, int radius) {
    if (src.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf) ||
        dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("box_blur buffer too small");
    src.download();
    hostBoxBlur(static_cast<const RGBAf*>(src.host()),
                static_cast<RGBAf*>(dst.host()), w, h, radius);
    dst.upload();
}

void ComputeBackend::median_radius(Buffer& src, Buffer& dst, std::uint32_t w,
                                   std::uint32_t h, int radius) {
    if (src.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf) ||
        dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("median_radius buffer too small");
    src.download();
    const RGBAf* s = static_cast<const RGBAf*>(src.host());
    RGBAf* d = static_cast<RGBAf*>(dst.host());
    const int r = std::max(1, radius);
    const std::size_t k = static_cast<std::size_t>(2 * r + 1) * (2 * r + 1);
    const bool small = k <= 32;
    // Rows disjoint; each worker owns its window scratch: bit-identical.
    // Single window walk feeds all three channels (was 3x traffic).
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        std::vector<float> w0, w1, w2;
        w0.reserve(k);
        w1.reserve(k);
        w2.reserve(k);
        for (std::uint32_t y = lo; y < hi; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                w0.clear();
                w1.clear();
                w2.clear();
                for (int dy = -r; dy <= r; ++dy) {
                    const std::uint32_t sy = repClamp(static_cast<int>(y) + dy, h);
                    for (int dx = -r; dx <= r; ++dx) {
                        const RGBAf& t =
                            s[static_cast<std::size_t>(sy) * w + repClamp(static_cast<int>(x) + dx, w)];
                        w0.push_back(t.r);
                        w1.push_back(t.g);
                        w2.push_back(t.b);
                    }
                }
                RGBAf acc{0, 0, 0, 1.0f};
                float* wins[3] = {w0.data(), w1.data(), w2.data()};
                for (int c = 0; c < 3; ++c) {
                    float* v = wins[c];
                    if (small) {
                        for (std::size_t i = 1; i < k; ++i) {
                            const float key = v[i];
                            std::size_t j = i;
                            while (j > 0 && v[j - 1] > key) {
                                v[j] = v[j - 1];
                                --j;
                            }
                            v[j] = key;
                        }
                    } else {
                        std::nth_element(v, v + k / 2, v + k);
                    }
                    acc[c] = v[k / 2];
                }
                acc.a = s[static_cast<std::size_t>(y) * w + x].a;
                d[static_cast<std::size_t>(y) * w + x] = acc;
            }
        }
    });
    dst.upload();
}

void ComputeBackend::unsharp_box(Buffer& src, Buffer& dst, std::uint32_t w,
                                 std::uint32_t h, float amount, int radius,
                                 float threshold) {
    if (src.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf) ||
        dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("unsharp_box buffer too small");
    src.download();
    const std::size_t n = static_cast<std::size_t>(w) * h;
    RGBAf* blur = tls_frame(n, 1).data();
    hostBoxBlur(static_cast<const RGBAf*>(src.host()), blur, w, h, radius);
    const RGBAf* s = static_cast<const RGBAf*>(src.host());
    RGBAf* d = static_cast<RGBAf*>(dst.host());
    const double amt = std::clamp(static_cast<double>(amount), 0.0, 5.0);
    const double thr = std::clamp(static_cast<double>(threshold), 0.0, 1.0);
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                for (int c = 0; c < 3; ++c) {
                    const double diff = static_cast<double>(s[i][c]) - blur[i][c];
                    double v = s[i][c];
                    if (std::fabs(diff) > thr) v = s[i][c] + amt * diff;
                    const double cl = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
                    d[i][c] = static_cast<float>(cl);
                }
                d[i].a = s[i].a;
            }
        }
    });
    dst.upload();
}

void ComputeBackend::motion_blur(const Buffer& src, Buffer& dst,
                                 std::uint32_t w, std::uint32_t h,
                                 float angleDeg, int distance) {
    if (src.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf) ||
        dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("motion_blur buffer too small");
    src.download();
    const RGBAf* s = static_cast<const RGBAf*>(src.host());
    RGBAf* d = static_cast<RGBAf*>(dst.host());
    const double ang = angleDeg * 3.141592653589793 / 180.0;
    const double dx = std::cos(ang);
    const double dy = std::sin(ang);
    const int dist = std::max(1, distance);
    const double n = 2 * dist + 1;
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                double ar = 0, ag = 0, ab = 0;
                // Incremental stepping along the motion vector (one add per
                // tap per axis instead of two muls); bilinear weights hoisted
                // out of the per-channel products (same math as the GPU
                // k_motion_sample, within the 2e-4 filter reference).
                double sx = static_cast<double>(x) - dx * dist;
                double sy = static_cast<double>(y) - dy * dist;
                for (int i = -dist; i <= dist; ++i) {
                    const int x0 = static_cast<int>(std::floor(sx));
                    const int y0 = static_cast<int>(std::floor(sy));
                    const double fx = sx - x0;
                    const double fy = sy - y0;
                    const double w00 = (1 - fx) * (1 - fy);
                    const double w10 = fx * (1 - fy);
                    const double w01 = (1 - fx) * fy;
                    const double w11 = fx * fy;
                    const std::uint32_t yc0 = repClamp(y0, h);
                    const std::uint32_t yc1 = repClamp(y0 + 1, h);
                    const std::uint32_t xc0 = repClamp(x0, w);
                    const std::uint32_t xc1 = repClamp(x0 + 1, w);
                    const RGBAf& a = s[static_cast<std::size_t>(yc0) * w + xc0];
                    const RGBAf& b = s[static_cast<std::size_t>(yc0) * w + xc1];
                    const RGBAf& c = s[static_cast<std::size_t>(yc1) * w + xc0];
                    const RGBAf& e = s[static_cast<std::size_t>(yc1) * w + xc1];
                    ar += a.r * w00 + b.r * w10 + c.r * w01 + e.r * w11;
                    ag += a.g * w00 + b.g * w10 + c.g * w01 + e.g * w11;
                    ab += a.b * w00 + b.b * w10 + c.b * w01 + e.b * w11;
                    sx += dx;
                    sy += dy;
                }
                RGBAf& o = d[static_cast<std::size_t>(y) * w + x];
                o.r = static_cast<float>(ar / n);
                o.g = static_cast<float>(ag / n);
                o.b = static_cast<float>(ab / n);
                o.a = s[static_cast<std::size_t>(y) * w + x].a;
            }
        }
    });
    dst.upload();
}

void ComputeBackend::lens_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                               std::uint32_t h, int radius, int kind,
                               float brightness, float threshold, float noise) {
    if (src.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf) ||
        dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("lens_blur buffer too small");
    src.download();
    const RGBAf* s = static_cast<const RGBAf*>(src.host());
    RGBAf* d = static_cast<RGBAf*>(dst.host());
    const int rr = std::max(1, radius);
    const std::size_t n = static_cast<std::size_t>(w) * h;
    RGBAf* blur = tls_frame(n).data();
    const auto inside = [&](int dx, int dy) {
        const double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
        if (dist > rr) return false;
        if (kind == 0) return true;
        const int sides = kind - 10;
        if (sides < 3) return true;
        const double th = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
        const double seg = 6.283185307179586 / sides;
        double a = std::fmod(th, seg);
        if (a < 0) a += seg;
        const double sc = std::cos(3.141592653589793 / sides) / std::cos(a - seg * 0.5);
        return dist <= rr * sc;
    };
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                double ar = 0, ag = 0, ab = 0;
                int cnt = 0;
                for (int dy = -rr; dy <= rr; ++dy) {
                    for (int dx = -rr; dx <= rr; ++dx) {
                        if (!inside(dx, dy)) continue;
                        const RGBAf& p = s[static_cast<std::size_t>(repClamp(static_cast<int>(y) + dy, h)) * w + repClamp(static_cast<int>(x) + dx, w)];
                        ar += p.r;
                        ag += p.g;
                        ab += p.b;
                        ++cnt;
                    }
                }
                cnt = std::max(1, cnt);
                RGBAf& o = blur[static_cast<std::size_t>(y) * w + x];
                o.r = static_cast<float>(ar / cnt);
                o.g = static_cast<float>(ag / cnt);
                o.b = static_cast<float>(ab / cnt);
                o.a = s[static_cast<std::size_t>(y) * w + x].a;
            }
        }
    });
    const std::uint64_t st = 777;
    pittore::core::parallel_rows(h, [&](std::uint32_t lo, std::uint32_t hi) {
        for (std::uint32_t y = lo; y < hi; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
            const RGBAf& o = s[static_cast<std::size_t>(y) * w + x];
            const RGBAf& b = blur[static_cast<std::size_t>(y) * w + x];
            const double lum = 0.2126 * o.r + 0.7152 * o.g + 0.0722 * o.b;
            const double spec = std::clamp((lum - threshold) * 4.0 + 0.5, 0.0, 1.0);
            const double m = 0.35 + 0.65 * spec;
            RGBAf& t = d[static_cast<std::size_t>(y) * w + x];
            t.r = static_cast<float>(o.r * (1 - m) + b.r * m + spec * brightness * 0.4);
            t.g = static_cast<float>(o.g * (1 - m) + b.g * m + spec * brightness * 0.4);
            t.b = static_cast<float>(o.b * (1 - m) + b.b * m + spec * brightness * 0.4);
            t.a = o.a;
            if (noise > 0.01) {
                std::uint64_t z = static_cast<std::uint64_t>(int(x) * 374761393 + int(y) * 668265263) + st * 144665;
                z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
                z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
                z = z ^ (z >> 31);
                const double g = (z % 1000) / 1000.0 - 0.5;
                const auto cl = [](double v) { return v < 0.0 ? 0.0f : (v > 1.0 ? 1.0f : static_cast<float>(v)); };
                t.r = cl(t.r + g * 0.08 * noise);
                t.g = cl(t.g + g * 0.08 * noise);
                t.b = cl(t.b + g * 0.08 * noise);
            }
            }
        }
    });
    dst.upload();
}


// Base-class default: read the frozen snapshot from host staging, resample the
// region into the destination's host staging, then push only that region to the
// device. Correct for every backend; CpuBackend keeps it (host == device) and
// GPU backends override with a device kernel that reads `src` in place.
void ComputeBackend::warp(Buffer& dst, const Buffer& src, std::uint32_t w,
                          std::uint32_t h, const WarpSubgrid& grid,
                          std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                          std::uint32_t y1) {
    if (x0 >= x1 || y0 >= y1 || grid.empty()) return;
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("warp buffer too small");
    warp_into_host(static_cast<RGBAf*>(dst.host()),
                   static_cast<const RGBAf*>(src.host()), w, h, grid,
                   static_cast<int>(x0), static_cast<int>(y0),
                   static_cast<int>(x1), static_cast<int>(y1));
    dst.upload_region(w, x0, y0, x1, y1);
}

void CpuBackend::warp(Buffer& dst, const Buffer& src, std::uint32_t w,
                      std::uint32_t h, const WarpSubgrid& grid,
                      std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                      std::uint32_t y1) {
    if (x0 >= x1 || y0 >= y1 || grid.empty()) return;
    if (dst.size() < static_cast<std::size_t>(w) * h * sizeof(RGBAf))
        throw std::invalid_argument("warp buffer too small");
    warp_into_host(static_cast<RGBAf*>(dst.host()),
                   static_cast<const RGBAf*>(src.host()), w, h, grid,
                   static_cast<int>(x0), static_cast<int>(y0),
                   static_cast<int>(x1), static_cast<int>(y1));
}

}  // namespace pittore::compute