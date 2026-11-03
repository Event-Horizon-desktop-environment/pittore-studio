#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/compute/warp.h"
#include "engine/core/pixel.h"
#include "engine/compute/blend.h"
#include "engine/compute/tone_blend.h"

namespace pittore::compute {

enum class BackendType { CPU, CUDA, HIP };

inline const char* to_string(BackendType t) {
    switch (t) {
        case BackendType::CPU: return "CPU";
        case BackendType::CUDA: return "CUDA";
        case BackendType::HIP: return "HIP";
    }
    return "unknown";
}

// One compute device a backend can use.
struct Device {
    BackendType type = BackendType::CPU;
    std::string name;
    std::uint64_t memory_mb = 0;     // 0 = unknown
    int compute_major = 0;           // GPU arch (compute capability / gfx family); 0 = n/a
    int compute_minor = 0;
};

// Blend modes + math live in blend.h (CPU reference and GPU kernels share it).

// Device memory with a host-visible staging pointer.
// upload()/download() cross the host/device line (no-ops on CPU).
class Buffer {
  public:
    virtual ~Buffer() = default;
    virtual std::size_t size() const = 0;   // bytes
    virtual void* host() = 0;               // host-visible staging (mutable)
    virtual const void* host() const = 0;   // read-only host access
    virtual void upload() const = 0;        // host -> device (no-op on CPU)
    virtual void download() const = 0;      // device -> host (no-op on CPU)

    // Small transfers for repaints: only [x0,x1) x [y0,y1) of a w-wide
    // RGBAf image crosses. No-op on CPU. `w` pins the row stride.
    virtual void upload_region(std::uint32_t /*w*/, std::uint32_t /*x0*/,
                               std::uint32_t /*y0*/, std::uint32_t /*x1*/,
                               std::uint32_t /*y1*/) const {}
    virtual void download_region(std::uint32_t /*w*/, std::uint32_t /*x0*/,
                                 std::uint32_t /*y0*/, std::uint32_t /*x1*/,
                                 std::uint32_t /*y1*/) const {}

    // Device copy: this := src (same size required). Default stages through
    // host (correct everywhere); GPU backends override with a D2D copy so
    // snapshots like the tone backdrop stop being recomposites.
    virtual void copy_from(const Buffer& src) {
        if (src.size() != size())
            throw std::invalid_argument("copy_from buffers differ in size");
        src.download();
        std::memcpy(host(), src.host(), size());
        upload();
    }

  protected:
    Buffer() = default;
};

// One layer for a batched composite: native pixels, stamp, and the
// doc-clipped area it paints. Optional grey mask scales alpha first;
// a clipped layer also scales by its base layer's post-mask alpha
// (`clipBase` indexes the same bottom->top array, -1 = no base, invisible).
// A clipped layer with no base anywhere becomes its own base (unclipped).
struct PlacedLayer {
    const Buffer* src = nullptr;       // sw×sh native pixels
    std::uint32_t sw = 0, sh = 0;
    double ox = 0.0, oy = 0.0;         // doc = offset + scale·src
    double sx = 1.0, sy = 1.0;
    std::uint32_t x0 = 0, y0 = 0;      // clipped doc region
    std::uint32_t x1 = 0, y1 = 0;
    float fold = 1.0f;                 // opacity × fill
    BlendMode mode = BlendMode::Normal;
    const Buffer* mask = nullptr;      // msw×msh opaque-grey coverage (R channel)
    std::uint32_t msw = 0, msh = 0;
    double mox = 0.0, moy = 0.0;       // doc = mask offset + mask scale·mask
    double msx = 1.0, msy = 1.0;
    bool clipped = false;              // clip to the base layer at clipBase
    int clipBase = -1;                 // index of the base layer, -1 = none
    // Live adjustment layer (no source): reads the accumulator, runs adjKind,
    // then blends as usual. adjP holds params (see adjust.h); adjAux is the
    // 256-float curve LUT for Curves, else null.
    bool isAdjustment = false;
    int adjKind = 0;
    float adjP[16] = {};
    const Buffer* adjAux = nullptr;    // 3x256 floats (R/G/B), Curves only
    // Solid fill (board backgrounds, D1): every window pixel samples the
    // constant `fillColor` instead of `src` (then null). Mask/fold/clip/blend
    // are unchanged, so a fill is just a layer whose source is one colour.
    bool isFill = false;
    pittore::RGBAf fillColor;
};

// Compute engine, one per physical device (see factory.h).
// Kernels work on RGBAf row-major buffers, linear light.
class ComputeBackend {
  public:
    virtual ~ComputeBackend() = default;
    virtual BackendType type() const = 0;
    virtual const std::string& name() const = 0;
    virtual const Device& device() const = 0;

    virtual std::unique_ptr<Buffer> make_buffer(std::size_t bytes) = 0;

    // Rec.709 gray; alpha kept. src and dst may match.
    virtual void grayscale(Buffer& src, Buffer& dst, std::uint32_t w,
                           std::uint32_t h) = 0;

    // Blend `top` INTO `bottom` in place. SVG spec, straight alpha.
    virtual void composite(Buffer& bottom, const Buffer& top, std::uint32_t w,
                           std::uint32_t h, BlendMode mode) = 0;

    // Blend only [x0,x1) x [y0,y1). Rest untouched. Lets a small brush
    // repaint move just that region across the bus. Default falls back
    // to full composite; CPU/GPU override with a real region path.
    virtual void composite_region(Buffer& bottom, const Buffer& top,
                                  std::uint32_t w, std::uint32_t h,
                                  std::uint32_t x0, std::uint32_t y0,
                                  std::uint32_t x1, std::uint32_t y1,
                                  BlendMode mode);

    // Fused sample + blend for one placed layer over a region:
    // doc = offset + scale*src, alpha_fold into sampled alpha, blend in place.
    // What sample_placed_host + composite_region do, in one call (what a
    // move/resize drag uses). GPU keeps src resident, so only the acc
    // region crosses; default stages through host.
    virtual void composite_placed(Buffer& bottom, const Buffer& src,
                                  std::uint32_t sw, std::uint32_t sh,
                                  double ox, double oy, double sx, double sy,
                                  std::uint32_t w, std::uint32_t x0,
                                  std::uint32_t y0, std::uint32_t x1,
                                  std::uint32_t y1, float alpha_fold,
                                  BlendMode mode);

    // Device-persistent composite_placed: `bottom` stays on-device
    // (cleared with clear(), read back with blit_premul), so no acc
    // transfer. Default forwards to composite_placed.
    virtual void composite_placed_into(Buffer& bottom, const Buffer& src,
                                       std::uint32_t sw, std::uint32_t sh,
                                       double ox, double oy, double sx, double sy,
                                       std::uint32_t w, std::uint32_t x0,
                                       std::uint32_t y0, std::uint32_t x1,
                                       std::uint32_t y1, float alpha_fold,
                                       BlendMode mode);

    // Batched composite_placed_into: every layer in one call, one launch
    // + one param upload for big multi-part docs. Empty windows skipped;
    // rx/ry bound the output that must be right. GPU runs one fused kernel;
    // default runs the host row walk (masks scale alpha, clipped layers
    // use the nearest base below).
    virtual void composite_many_into(Buffer& bottom, std::uint32_t w,
                                     std::uint32_t rx0, std::uint32_t ry0,
                                     std::uint32_t rx1, std::uint32_t ry1,
                                     const PlacedLayer* layers,
                                     std::size_t count);

    // Zero a buffer. GPU does a device memset (host staging goes stale;
    // device-persistent callers read back via blit_premul).
    virtual void clear(Buffer& dst);

    // Convert [x0,x1) x [y0,y1) of w-wide `src` to premul ARGB32 bytes
    // (QImage Format_ARGB32_Premultiplied) at `dst` (rows `dst_pitch` apart).
    // Canvas upload; GPU converts on-device + DMAs so the host skips pixels.
    virtual void blit_premul(const Buffer& src, std::uint8_t* dst,
                             std::uint32_t w, std::uint32_t x0,
                             std::uint32_t y0, std::uint32_t x1,
                             std::uint32_t y1, std::size_t dst_pitch);

    // Separable gaussian, sigma in px, clamped edges. src/dst may match.
    virtual void gaussian_blur(const Buffer& src, Buffer& dst, std::uint32_t w,
                               std::uint32_t h, float sigma) = 0;

    // Unsharp: src + amount * (src - blurred). Threshold skips calm areas.
    virtual void sharpen(Buffer& /*src*/, Buffer& /*dst*/,
                         std::uint32_t /*w*/, std::uint32_t /*h*/,
                         float /*amount*/, float /*radius*/,
                         float /*threshold*/) {}

    // Brightness/contrast, both in [-1,1].
    virtual void brightness_contrast(Buffer& /*src*/, Buffer& /*dst*/,
                                     std::uint32_t /*w*/, std::uint32_t /*h*/,
                                     float /*brightness*/, float /*contrast*/) {}

    // HSL: hue_shift in [-180,180], saturation/lightness in [-1,1].
    virtual void hue_saturation(Buffer& /*src*/, Buffer& /*dst*/,
                                std::uint32_t /*w*/, std::uint32_t /*h*/,
                                float /*hue_shift*/, float /*saturation*/,
                                float /*lightness*/) {}

    // 3x3 median (kills salt-and-pepper noise).
    virtual void median_filter(Buffer& /*src*/, Buffer& /*dst*/,
                               std::uint32_t /*w*/, std::uint32_t /*h*/) {}

    // Separable box blur, edges replicate. radius < 1 acts as 1.
    virtual void box_blur(const Buffer& /*src*/, Buffer& /*dst*/,
                          std::uint32_t /*w*/, std::uint32_t /*h*/,
                          int /*radius*/);

    // Device-resident box blur: `src` is already on-device, `dst` is left
    // on-device (no transfers). Lets callers chain N blurs with one upload
    // + one download (the Phase-3 resident pipeline in miniature). Default
    // forwards to box_blur (correct everywhere); GPU backends override.
    virtual void box_blur_resident(const Buffer& src, Buffer& dst,
                                   std::uint32_t w, std::uint32_t h,
                                   int radius) {
        box_blur(src, dst, w, h, radius);
    }

    // Device-resident gaussian: same contract as box_blur_resident.
    // Default forwards to gaussian_blur; GPU backends override.
    virtual void gaussian_blur_resident(const Buffer& src, Buffer& dst,
                                        std::uint32_t w, std::uint32_t h,
                                        float sigma) {
        gaussian_blur(src, dst, w, h, sigma);
    }

    // Square per-channel median, edges replicate, alpha passes through.
    // Radii over 8 use the host fallback.
    virtual void median_radius(Buffer& /*src*/, Buffer& /*dst*/,
                               std::uint32_t /*w*/, std::uint32_t /*h*/,
                               int /*radius*/);

    // Unsharp over a box-blur base: clamp(orig + amount*diff) per channel
    // where |diff| beats threshold.
    virtual void unsharp_box(Buffer& /*src*/, Buffer& /*dst*/,
                             std::uint32_t /*w*/, std::uint32_t /*h*/,
                             float /*amount*/, int /*radius*/,
                             float /*threshold*/);

    // Motion blur along (cos angle, sin angle), bilinear, clamped edges.
    // distance < 1 acts as 1. Alpha passes through.
    virtual void motion_blur(const Buffer& /*src*/, Buffer& /*dst*/,
                             std::uint32_t /*w*/, std::uint32_t /*h*/,
                             float /*angleDeg*/, int /*distance*/);

    // Lens blur: shaped-kernel blur (0 = disc) mixed by specular response,
    // plus hashed grain.
    virtual void lens_blur(const Buffer& /*src*/, Buffer& /*dst*/,
                           std::uint32_t /*w*/, std::uint32_t /*h*/,
                           int /*radius*/, int /*kind*/, float /*brightness*/,
                           float /*threshold*/, float /*noise*/);

    // One round dab INTO dst in place, straight-alpha source-over.
    // hardness 0 = soft, opacity in [0,1]. Default goes via host staging,
    // so every backend gets a brush for free.
    virtual void paint_dab(Buffer& dst, std::uint32_t w, std::uint32_t h,
                           float cx, float cy, float radius, float hardness,
                           float opacity, const RGBAf& color);

    // Background Eraser dab (discontiguous): erase in the dab circle where
    // RGB is within `tolerance` of `sample` (ramped); `protectFg` skips
    // pixels also matching `fg`. RGB untouched, alpha multiplied down.
    // Bbox written to bboxOut (exclusive). Default goes via host staging;
    // CUDA/HIP run the bbox kernel with bbox-only download (same contract
    // as paint_dab).
    virtual void bg_erase(Buffer& dst, std::uint32_t w, std::uint32_t h,
                          float cx, float cy, float radius, float hardness,
                          float opacity, const RGBAf& sample, float tolerance,
                          bool protectFg, const RGBAf& fg, int* bboxOut);

    // Pattern stamp dab: source-over the 64x64 procedural tile under the
    // dab mask. `tile` is a 64x64 RGBA device/host buffer, `ox`/`oy` the
    // layer-space tile origin. Default goes via host staging; CUDA/HIP
    // run the bbox kernel with bbox-only download.
    virtual void pattern_stamp(Buffer& dst, std::uint32_t w, std::uint32_t h,
                               float cx, float cy, float radius,
                               float hardness, float opacity,
                               const Buffer& tile, float ox, float oy,
                               int* bboxOut);

    // History Brush dab: source-over snapshot texels (`src`, same dims as
    // dst) under the dab mask. Default goes via host staging; CUDA/HIP run
    // the bbox kernel with bbox-only download.
    virtual void history_dab(Buffer& dst, std::uint32_t w, std::uint32_t h,
                             float cx, float cy, float radius, float hardness,
                             float opacity, const Buffer& src, int* bboxOut);

    // Liquify resample: rebuild [x0,x1) x [y0,y1) of live `dst` from frozen
    // `src` (same size) through `grid`. dst must not alias src. GPU reads
    // `src` on-device (uploaded once per stroke); only the dst region
    // crosses. Default goes via host staging.
    virtual void warp(Buffer& dst, const Buffer& src, std::uint32_t w,
                      std::uint32_t h, const WarpSubgrid& grid,
                      std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                      std::uint32_t y1);

    // Live Tone Blend Group transfer: re-grade `dst` (w×h, the isolated
    // group composite) against `backdrop` (same size, the composite beneath
    // the group) in place, per ToneBlendParams. Reading `backdrop` while
    // writing `dst` is safe. Inputs are DEVICE-RESIDENT (fused-path
    // contract, like composite_many_into): the caller uploads host-filled
    // buffers explicitly; this call never uploads, so device-side outputs
    // of a previous launch survive. The result is downloaded to host
    // staging on the way out for host readers. Default goes via host
    // staging (download both, run the CPU reference, upload), so every
    // backend is correct without a device kernel; fused kernels override
    // per backend.
    virtual void tone_blend(Buffer& dst, const Buffer& backdrop,
                            std::uint32_t w, std::uint32_t h,
                            const ToneBlendParams& p);

    // Region-bounded tone blend: only re-process [rx0,rx1) x [ry0,ry1)
    // of the frame. The halo extends the region by 3*sigma for the
    // pyramid blur. Default falls back to full-frame; GPU overrides
    // with a tight region kernel.
    virtual void tone_blend_region(Buffer& dst, const Buffer& backdrop,
                                   std::uint32_t w, std::uint32_t h,
                                   std::uint32_t rx0, std::uint32_t ry0,
                                   std::uint32_t rx1, std::uint32_t ry1,
                                   const ToneBlendParams& p);

  protected:
    ComputeBackend() = default;
};

}  // namespace pittore::compute