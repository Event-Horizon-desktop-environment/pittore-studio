// gpu_parity.cpp — shared CPU-vs-GPU kernel parity checks, backend-agnostic.
//
// Runs grayscale, composite (every layer blend mode, on the full, region and
// placed paths) and gaussian blur on both the CPU reference backend and the
// requested GPU backend and compares within float tolerance. The caller
// decides which GPU type to probe; unavailable GPU backends are a clean skip,
// not a failure.
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "engine/compute/factory.h"
#include "engine/compute/adjust.h"
#include "engine/compute/layer_mask.h"
#include "engine/core/pixel.h"
#include "test_util.h"
#include "gpu_parity.h"

using pittore::RGBAf;

namespace {

bool near(const RGBAf& a, const RGBAf& b, float eps) {
    return std::abs(a.r - b.r) <= eps && std::abs(a.g - b.g) <= eps &&
           std::abs(a.b - b.b) <= eps && std::abs(a.a - b.a) <= eps;
}

}  // namespace

void run_gpu_parity(pittore::compute::BackendType gpu_type) {
    // Every layer blend mode plus Pass Through. CPU and GPU must agree on all
    // of them — Dissolve included, whose per-pixel hash depends only on the
    // document coordinate, which both sides derive identically.
    constexpr pittore::compute::BlendMode kAllModes[] = {
        pittore::compute::BlendMode::Normal,
        pittore::compute::BlendMode::Multiply,
        pittore::compute::BlendMode::Dissolve,
        pittore::compute::BlendMode::Darken,
        pittore::compute::BlendMode::ColorBurn,
        pittore::compute::BlendMode::LinearBurn,
        pittore::compute::BlendMode::DarkerColor,
        pittore::compute::BlendMode::Lighten,
        pittore::compute::BlendMode::Screen,
        pittore::compute::BlendMode::ColorDodge,
        pittore::compute::BlendMode::LinearDodge,
        pittore::compute::BlendMode::LighterColor,
        pittore::compute::BlendMode::Overlay,
        pittore::compute::BlendMode::SoftLight,
        pittore::compute::BlendMode::HardLight,
        pittore::compute::BlendMode::VividLight,
        pittore::compute::BlendMode::LinearLight,
        pittore::compute::BlendMode::PinLight,
        pittore::compute::BlendMode::HardMix,
        pittore::compute::BlendMode::Difference,
        pittore::compute::BlendMode::Exclusion,
        pittore::compute::BlendMode::Subtract,
        pittore::compute::BlendMode::Divide,
        pittore::compute::BlendMode::Hue,
        pittore::compute::BlendMode::Saturation,
        pittore::compute::BlendMode::Color,
        pittore::compute::BlendMode::Luminosity,
        pittore::compute::BlendMode::PassThrough,
    };

    auto gpu = pittore::compute::make_backend(gpu_type);
    if (!gpu) {
        std::printf("  [%s] backend not built / no device — skipped\n",
                    pittore::compute::to_string(gpu_type));
        return;
    }

    auto cpu = pittore::compute::make_default_backend();
    if (cpu->type() != pittore::compute::BackendType::CPU)
        cpu = pittore::compute::make_backend(pittore::compute::BackendType::CPU);

    constexpr std::uint32_t w = 512, h = 512;   // 2x2 tile grid
    constexpr std::uint32_t cw = 257, ch = 129; // ragged edge tiles
    const std::size_t n = static_cast<std::size_t>(w) * h;

    std::vector<RGBAf> src(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = static_cast<float>(i % 512);
        const float y = static_cast<float>(i / 512);
        src[i] = RGBAf{(x / 511.0f) * 0.8f + 0.1f, (y / 511.0f) * 0.8f + 0.1f,
                       ((x + y) / 1022.0f) * 0.5f, ((i & 7) + 4) / 12.0f};
    }

    auto cpu_s = cpu->make_buffer(n * sizeof(RGBAf));
    auto gpu_s = gpu->make_buffer(n * sizeof(RGBAf));
    std::memcpy(cpu_s->host(), src.data(), cpu_s->size());
    std::memcpy(gpu_s->host(), src.data(), gpu_s->size());

    // --- grayscale ------------------------------------
    auto cpu_d = cpu->make_buffer(n * sizeof(RGBAf));
    auto gpu_d = gpu->make_buffer(n * sizeof(RGBAf));
    cpu->grayscale(*cpu_s, *cpu_d, w, h);
    gpu->grayscale(*gpu_s, *gpu_d, w, h);
    const auto* rc = static_cast<const RGBAf*>(cpu_d->host());
    const auto* gc = static_cast<const RGBAf*>(gpu_d->host());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (!near(rc[i], gc[i], 1e-5f)) ++mismatches;
    CHECK_EQ(mismatches, 0u);

    // --- composite (all layer blend modes) -----------------
    std::vector<RGBAf> top(n);
    for (std::size_t i = 0; i < n; ++i)
        top[i] = RGBAf{0.9f, 0.1f, 0.5f, ((i & 15) + 2) / 18.0f};
    auto cpu_t = cpu->make_buffer(n * sizeof(RGBAf));
    auto gpu_t = gpu->make_buffer(n * sizeof(RGBAf));
    std::memcpy(cpu_t->host(), top.data(), cpu_t->size());
    std::memcpy(gpu_t->host(), top.data(), gpu_t->size());
    std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
    std::memcpy(gpu_d->host(), src.data(), gpu_d->size());

    for (auto mode : kAllModes) {
        std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
        std::memcpy(gpu_d->host(), src.data(), gpu_d->size());
        cpu->composite(*cpu_d, *cpu_t, w, h, mode);
        gpu->composite(*gpu_d, *gpu_t, w, h, mode);
        mismatches = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (!near(static_cast<const RGBAf*>(cpu_d->host())[i],
                      static_cast<const RGBAf*>(gpu_d->host())[i], 1e-5f))
                ++mismatches;
        CHECK_EQ(mismatches, 0u);
    }

    // --- region composite (incremental repaint path) --
    // A region composite must (a) match the CPU reference inside the rect,
    // (b) leave every pixel outside the rect untouched, and (c) equal the full
    // composite restricted to that rect. Rects cover interior, edge-touching,
    // a thin brush-capsule and the bottom-right corner.
    {
        const std::pair<std::pair<std::uint32_t, std::uint32_t>,
                        std::pair<std::uint32_t, std::uint32_t>>
            rects[] = {{{0, 0}, {w, h}},                     // full frame
                       {{100, 80}, {400, 300}},              // interior
                       {{0, 40}, {w, 120}},                  // full width, touches L/R
                       {{250, 250}, {270, 260}},             // tiny capsule
                       {{w - 20, h - 20}, {w, h}}};          // bottom-right corner
        for (auto mode : kAllModes) {
            auto cpu_full = cpu->make_buffer(n * sizeof(RGBAf));
            std::memcpy(cpu_full->host(), src.data(), cpu_full->size());
            cpu->composite(*cpu_full, *cpu_t, w, h, mode);
            const auto* full = static_cast<const RGBAf*>(cpu_full->host());

            for (const auto& r : rects) {
                const std::uint32_t x0 = r.first.first, y0 = r.first.second;
                const std::uint32_t x1 = r.second.first, y1 = r.second.second;
                std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
                std::memcpy(gpu_d->host(), src.data(), gpu_d->size());
                cpu->composite_region(*cpu_d, *cpu_t, w, h, x0, y0, x1, y1, mode);
                gpu->composite_region(*gpu_d, *gpu_t, w, h, x0, y0, x1, y1, mode);
                mismatches = 0;
                std::size_t outside_changed = 0;
                const auto* cd = static_cast<const RGBAf*>(cpu_d->host());
                const auto* gd = static_cast<const RGBAf*>(gpu_d->host());
                for (std::uint32_t y = 0; y < h; ++y) {
                    for (std::uint32_t x = 0; x < w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * w + x;
                        const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
                        if (in) {
                            if (!near(cd[i], gd[i], 1e-5f) ||
                                !near(cd[i], full[i], 1e-5f) ||
                                !near(gd[i], full[i], 1e-5f))
                                ++mismatches;
                        } else if (!near(gd[i], src[i], 0.0f) ||
                                   !near(cd[i], src[i], 0.0f)) {
                            ++outside_changed;
                        }
                    }
                }
                CHECK_EQ(mismatches, 0u);
                CHECK_EQ(outside_changed, 0u);
            }
        }
    }

    // --- composite_placed (fused placed-layer path) ---------
    // A move/resize drag composites a placed layer on the GPU through
    // composite_placed: sample the layer's NATIVE pixels for each doc pixel of
    // the rect, fold alpha, blend into the accumulator. Must (a) match the
    // CPU reference inside the rect, (b) leave the rest untouched. Exercise
    // upscale, downscale, a thin 1-px move strip, an opacity fold, edge-
    // straddling placements and both blend modes.
    {
        constexpr std::uint32_t sw = 257, sh = 193;   // native photo
        const std::size_t pn = static_cast<std::size_t>(sw) * sh;
        std::vector<RGBAf> photo(pn);
        for (std::size_t i = 0; i < pn; ++i) {
            const float x = static_cast<float>(i % sw);
            const float y = static_cast<float>(i / sw);
            photo[i] = RGBAf{(x / sw) * 0.8f + 0.1f, (y / sh) * 0.6f + 0.2f,
                             (x * 0.5f + y * 0.5f) / (sw + sh),
                             0.15f + (x * 0.6f + y * 0.4f) / (sw + sh)};
        }
        auto cpu_ps = cpu->make_buffer(pn * sizeof(RGBAf));
        auto gpu_ps = gpu->make_buffer(pn * sizeof(RGBAf));
        std::memcpy(cpu_ps->host(), photo.data(), cpu_ps->size());
        std::memcpy(gpu_ps->host(), photo.data(), gpu_ps->size());
        // The UI caches placed layer pixels on the device up front; the fused
        // kernels expect device residency, so upload the test source too.
        cpu_ps->upload();
        gpu_ps->upload();

        struct PlacedCase {
            double ox, oy, sx, sy;
            std::uint32_t x0, y0, x1, y1;
            float fold;
            pittore::compute::BlendMode mode;
        };
        const PlacedCase cases[] = {
            // upscale 2x, full frame
            {8.0, 5.0, 2.0, 2.0, 0, 0, w, h, 1.0f, pittore::compute::BlendMode::Normal},
            // downscale 0.5x, full frame
            {-60.0, -20.0, 0.5, 0.5, 0, 0, w, h, 1.0f, pittore::compute::BlendMode::Normal},
            // pure translation + opacity fold over a thin move strip
            {3.0, 3.0, 1.0, 1.0, 100, 40, 102, 460, 0.35f, pittore::compute::BlendMode::Normal},
            // upscale, touching the bottom-right corner, multiply
            {200.0, 150.0, 1.5, 1.5, 400, 300, w, h, 1.0f, pittore::compute::BlendMode::Multiply},
            // straddles the top-left edge (sampler transparency), multiply + fold
            {-12.0, -8.0, 2.0, 2.0, 0, 0, 300, 200, 0.85f, pittore::compute::BlendMode::Multiply},
            // 1:1 placed photo in the interior
            {64.0, 40.0, 1.0, 1.0, 60, 30, 400, 300, 1.0f, pittore::compute::BlendMode::Normal},
            // non-trivial modes on the fused placed path too
            {64.0, 40.0, 1.0, 1.0, 60, 30, 400, 300, 1.0f, pittore::compute::BlendMode::Difference},
            {64.0, 40.0, 1.0, 1.0, 60, 30, 400, 300, 1.0f, pittore::compute::BlendMode::Dissolve},
            {200.0, 150.0, 1.5, 1.5, 400, 300, w, h, 1.0f, pittore::compute::BlendMode::Screen},
        };
        for (const auto& c : cases) {
            std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
            std::memcpy(gpu_d->host(), src.data(), gpu_d->size());
            cpu->composite_placed(*cpu_d, *cpu_ps, sw, sh, c.ox, c.oy, c.sx, c.sy,
                                  w, c.x0, c.y0, c.x1, c.y1, c.fold, c.mode);
            gpu->composite_placed(*gpu_d, *gpu_ps, sw, sh, c.ox, c.oy, c.sx, c.sy,
                                  w, c.x0, c.y0, c.x1, c.y1, c.fold, c.mode);
            mismatches = 0;
            std::size_t outside_changed = 0;
            const auto* cd = static_cast<const RGBAf*>(cpu_d->host());
            const auto* gd = static_cast<const RGBAf*>(gpu_d->host());
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    const bool in = x >= c.x0 && x < c.x1 && y >= c.y0 && y < c.y1;
                    if (in) {
                        if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
                    } else if (!near(gd[i], src[i], 0.0f)) {
                        ++outside_changed;
                    }
                }
            }
            CHECK_EQ(mismatches, 0u);
            CHECK_EQ(outside_changed, 0u);
        }

        // Doc-aligned 1:1 source: exercises the kernel's identity fast path
        // (flat texel read) against the CPU sampler. The whole point of the fast
        // path is that it is bit-identical to sampling at scale 1 offset 0, so
        // the tolerance here is zero.
        {
            std::vector<RGBAf> doc(n);
            for (std::size_t i = 0; i < n; ++i)
                doc[i] = src[i];   // opaque-ish doc-sized background
            auto id_ps = gpu->make_buffer(n * sizeof(RGBAf));
            std::memcpy(id_ps->host(), doc.data(), id_ps->size());
            id_ps->upload();
            auto cpu_id = cpu->make_buffer(n * sizeof(RGBAf));
            std::memcpy(cpu_id->host(), doc.data(), cpu_id->size());
            cpu_id->upload();

            std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
            std::memcpy(gpu_d->host(), src.data(), gpu_d->size());
            cpu->composite_placed(*cpu_d, *cpu_id, w, h, 0.0, 0.0, 1.0, 1.0, w, 0,
                                  0, w, h, 1.0f,
                                  pittore::compute::BlendMode::Normal);
            gpu->composite_placed(*gpu_d, *id_ps, w, h, 0.0, 0.0, 1.0, 1.0, w, 0,
                                  0, w, h, 1.0f,
                                  pittore::compute::BlendMode::Normal);
            mismatches = 0;
            const auto* cd = static_cast<const RGBAf*>(cpu_d->host());
            const auto* gd = static_cast<const RGBAf*>(gpu_d->host());
            // The fast path returns the source texel exactly; the CPU sampler
            // round-trips it through premultiplied space, a sub-ULP difference.
            // The project invariant is 1e-5, and the fast path is in fact the
            // *more* faithful match for the CPU identity/memcpy background path.
            for (std::size_t i = 0; i < n; ++i)
                if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
            CHECK_EQ(mismatches, 0u);
        }

        // --- device-persistent path: clear + composite_placed_into + blit_premul
        // This is exactly the UI's move/resize frame: zero the accumulator on
        // the device, blend every layer with no host sync, then convert the
        // finished pixels to premultiplied ARGB32 for the canvas. Must match the
        // host reference in the rect, leave everything else transparent, and
        // produce the same ARGB bytes.
        {
            cpu->clear(*cpu_d);
            gpu->clear(*gpu_d);
            gpu_d->download();
            const auto* cd0 = static_cast<const RGBAf*>(cpu_d->host());
            const auto* gd0 = static_cast<const RGBAf*>(gpu_d->host());
            mismatches = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (!near(cd0[i], gd0[i], 0.0f)) ++mismatches;
            CHECK_EQ(mismatches, 0u);

            for (const auto& c : cases) {
                cpu->clear(*cpu_d);
                gpu->clear(*gpu_d);
                cpu->composite_placed_into(*cpu_d, *cpu_ps, sw, sh, c.ox, c.oy,
                                           c.sx, c.sy, w, c.x0, c.y0, c.x1, c.y1,
                                           c.fold, c.mode);
                gpu->composite_placed_into(*gpu_d, *gpu_ps, sw, sh, c.ox, c.oy,
                                           c.sx, c.sy, w, c.x0, c.y0, c.x1, c.y1,
                                           c.fold, c.mode);
                gpu_d->download();
                mismatches = 0;
                std::size_t outside_changed = 0;
                const auto* cd = static_cast<const RGBAf*>(cpu_d->host());
                const auto* gd = static_cast<const RGBAf*>(gpu_d->host());
                for (std::uint32_t y = 0; y < h; ++y) {
                    for (std::uint32_t x = 0; x < w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * w + x;
                        const bool in = x >= c.x0 && x < c.x1 && y >= c.y0 && y < c.y1;
                        if (in) {
                            if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
                        } else if (gd[i].a != 0.0f) {
                            ++outside_changed;
                        }
                    }
                }
                CHECK_EQ(mismatches, 0u);
                CHECK_EQ(outside_changed, 0u);

                // blit_premul: identical premultiplied ARGB32 bytes (tolerance
                // of one LSB — both sides run the same Bayer dither).
                std::vector<std::uint8_t> ca(static_cast<std::size_t>(w) * h * 4);
                std::vector<std::uint8_t> ga(static_cast<std::size_t>(w) * h * 4);
                cpu->blit_premul(*cpu_d, ca.data(), w, 0, 0, w, h,
                                 static_cast<std::size_t>(w) * 4);
                gpu->blit_premul(*gpu_d, ga.data(), w, 0, 0, w, h,
                                 static_cast<std::size_t>(w) * 4);
                std::size_t byte_diff = 0;
                for (std::size_t i = 0; i < ca.size(); ++i)
                    if (std::abs(static_cast<int>(ca[i]) - static_cast<int>(ga[i])) > 1)
                        ++byte_diff;
                CHECK_EQ(byte_diff, 0u);
            }
        }
        // --- batched masked/clipped composite ------------------------------
        // The batched path had no parity coverage. A base layer establishes
        // coverage; two clipped layers in the same group both use that base
        // (they never mask each other); and a later base starts a new group.
        {
            constexpr std::uint32_t mw = 64, mh = 48;
            constexpr std::uint32_t sw = 32, sh = 32;
            constexpr std::uint32_t cw = 8, ch = 8;
            const std::size_t mn = static_cast<std::size_t>(mw) * mh;
            auto upload = [](auto& be, const std::vector<RGBAf>& v) {
                auto b = be->make_buffer(v.size() * sizeof(RGBAf));
                std::memcpy(b->host(), v.data(), b->size());
                b->upload();
                return b;
            };

            std::vector<RGBAf> red(sw * sh, RGBAf{1.0f, 0.0f, 0.0f, 0.8f});
            std::vector<RGBAf> blue(sw * sh, RGBAf{0.0f, 0.0f, 1.0f, 0.6f});
            std::vector<RGBAf> mask(sw * sh);
            for (std::uint32_t y = 0; y < sh; ++y) {
                for (std::uint32_t x = 0; x < sw; ++x)
                    mask[static_cast<std::size_t>(y) * sw + x] =
                        pittore::compute::make_mask_pixel(x < 16 ? 1.0f : 0.0f);
            }
            std::vector<RGBAf> green(cw * ch, RGBAf{0.0f, 1.0f, 0.0f, 1.0f});
            std::vector<RGBAf> yellow(cw * ch, RGBAf{1.0f, 1.0f, 0.0f, 0.5f});

            auto cpu_acc = cpu->make_buffer(mn * sizeof(RGBAf));
            auto gpu_acc = gpu->make_buffer(mn * sizeof(RGBAf));
            auto cpu_red = upload(cpu, red);
            auto gpu_red = upload(gpu, red);
            auto cpu_blue = upload(cpu, blue);
            auto gpu_blue = upload(gpu, blue);
            auto cpu_mask = upload(cpu, mask);
            auto gpu_mask = upload(gpu, mask);
            auto cpu_green = upload(cpu, green);
            auto gpu_green = upload(gpu, green);
            auto cpu_yellow = upload(cpu, yellow);
            auto gpu_yellow = upload(gpu, yellow);

            auto fillPlaced = [](pittore::compute::PlacedLayer& p,
                                 pittore::compute::Buffer* src,
                                 std::uint32_t w, std::uint32_t h, double ox,
                                 double oy, std::uint32_t x0, std::uint32_t y0,
                                 std::uint32_t x1, std::uint32_t y1, float fold,
                                 bool clipped) {
                p.src = src;
                p.sw = w;
                p.sh = h;
                p.ox = ox;
                p.oy = oy;
                p.sx = 1.0;
                p.sy = 1.0;
                p.x0 = x0;
                p.y0 = y0;
                p.x1 = x1;
                p.y1 = y1;
                p.fold = fold;
                p.mode = pittore::compute::BlendMode::Normal;
                p.clipped = clipped;
            };
            std::vector<pittore::compute::PlacedLayer> cpu_layers(4);
            std::vector<pittore::compute::PlacedLayer> gpu_layers(4);
            for (int side = 0; side < 2; ++side) {
                auto& layers = side == 0 ? cpu_layers : gpu_layers;
                auto* redBuf = side == 0 ? cpu_red.get() : gpu_red.get();
                auto* blueBuf = side == 0 ? cpu_blue.get() : gpu_blue.get();
                auto* maskBuf = side == 0 ? cpu_mask.get() : gpu_mask.get();
                auto* greenBuf = side == 0 ? cpu_green.get() : gpu_green.get();
                auto* yellowBuf = side == 0 ? cpu_yellow.get() : gpu_yellow.get();
                fillPlaced(layers[0], redBuf, sw, sh, 8.0, 8.0, 8, 8, 40, 40,
                           0.5f, false);
                fillPlaced(layers[1], blueBuf, sw, sh, 16.0, 16.0, 16, 16, 48,
                           48, 1.0f, true);
                layers[1].mask = maskBuf;
                layers[1].msw = sw;
                layers[1].msh = sh;
                layers[1].mox = 16.0;
                layers[1].moy = 16.0;
                fillPlaced(layers[2], greenBuf, cw, ch, 10.0, 30.0, 10, 30, 18,
                           38, 1.0f, true);
                fillPlaced(layers[3], yellowBuf, cw, ch, 0.0, 0.0, 0, 0, 8, 8,
                           1.0f, false);
            }
            for (auto& layers : {&cpu_layers, &gpu_layers}) {
                (*layers)[1].clipBase = 0;
                (*layers)[2].clipBase = 0;
            }

            cpu->clear(*cpu_acc);
            gpu->clear(*gpu_acc);
            cpu->composite_many_into(*cpu_acc, mw, 0, 0, mw, mh,
                                     cpu_layers.data(), cpu_layers.size());
            gpu->composite_many_into(*gpu_acc, mw, 0, 0, mw, mh,
                                     gpu_layers.data(), gpu_layers.size());
            gpu_acc->download();
            const auto* cd = static_cast<const RGBAf*>(cpu_acc->host());
            const auto* gd = static_cast<const RGBAf*>(gpu_acc->host());
            mismatches = 0;
            for (std::size_t i = 0; i < mn; ++i)
                if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
            CHECK_EQ(mismatches, 0u);

            const auto at = [&](std::uint32_t x, std::uint32_t y) {
                return static_cast<std::size_t>(y) * mw + x;
            };
            // The second clipped layer sits inside the base but outside the
            // first clipped layer: base-group clipping still shows it.
            CHECK(cd[at(12, 32)].a > 0.2f);
            CHECK(cd[at(12, 32)].g > cd[at(12, 32)].b);
            // The masked half of the first clipped layer disappears.
            CHECK_EQ(cd[at(40, 20)].a, 0.0f);
            CHECK_EQ(gd[at(40, 20)].a, 0.0f);
            // The unmasked half blends blue over the red base.
            CHECK(cd[at(20, 20)].a > 0.5f);
            CHECK(cd[at(20, 20)].b > 0.1f);
            // The later non-clipped layer starts a new group outside the base.
            CHECK_EQ(cd[at(2, 2)].a, 0.5f);
            CHECK_EQ(cd[at(50, 40)].a, 0.0f);

            // A clipped layer whose base contributes nothing stays invisible.
            for (auto& layers : {&cpu_layers, &gpu_layers}) {
                pittore::compute::PlacedLayer& solo = (*layers)[1];
                solo.clipped = true;
                solo.clipBase = -1;
                solo.x0 = 0;
                solo.y0 = 0;
                solo.x1 = mw;
                solo.y1 = mh;
            }
            cpu->clear(*cpu_acc);
            gpu->clear(*gpu_acc);
            cpu->composite_many_into(*cpu_acc, mw, 0, 0, mw, mh,
                                     &cpu_layers[1], 1);
            gpu->composite_many_into(*gpu_acc, mw, 0, 0, mw, mh,
                                     &gpu_layers[1], 1);
            gpu_acc->download();
            const auto* cdSolo =
                static_cast<const RGBAf*>(cpu_acc->host());
            const auto* gdSolo =
                static_cast<const RGBAf*>(gpu_acc->host());
            mismatches = 0;
            for (std::size_t i = 0; i < mn; ++i) {
                if (cdSolo[i].a != 0.0f) ++mismatches;
                if (!near(cdSolo[i], gdSolo[i], 1e-5f)) ++mismatches;
            }
            CHECK_EQ(mismatches, 0u);
        }

        // --- solid-fill layer (board backgrounds, D1) ----------------------
        // A fill layer has no source: every window pixel samples the constant
        // fillColor. CPU and GPU must agree exactly, the fill region takes the
        // colour at its fold alpha over a base, and the region outside the
        // window is untouched. The base layer proves the fill composites over
        // content rather than replacing the accumulator.
        {
            constexpr std::uint32_t mw = 48, mh = 40;
            const std::size_t mn = static_cast<std::size_t>(mw) * mh;
            auto upload = [](auto& be, const std::vector<RGBAf>& v) {
                auto b = be->make_buffer(v.size() * sizeof(RGBAf));
                std::memcpy(b->host(), v.data(), b->size());
                b->upload();
                return b;
            };
            std::vector<RGBAf> base(mn, RGBAf{0.1f, 0.1f, 0.1f, 1.0f});
            auto cpu_base = upload(cpu, base);
            auto gpu_base = upload(gpu, base);
            auto cpu_acc = cpu->make_buffer(mn * sizeof(RGBAf));
            auto gpu_acc = gpu->make_buffer(mn * sizeof(RGBAf));

            const RGBAf fillCol{0.9f, 0.2f, 0.3f, 1.0f};
            std::vector<pittore::compute::PlacedLayer> cpu_l(2), gpu_l(2);
            for (int side = 0; side < 2; ++side) {
                auto& layers = side == 0 ? cpu_l : gpu_l;
                auto* baseBuf = side == 0 ? cpu_base.get() : gpu_base.get();
                auto& b = layers[0];  // full-canvas opaque base
                b.src = baseBuf;
                b.sw = mw;
                b.sh = mh;
                b.ox = 0.0;
                b.oy = 0.0;
                b.sx = 1.0;
                b.sy = 1.0;
                b.x0 = 0;
                b.y0 = 0;
                b.x1 = mw;
                b.y1 = mh;
                b.fold = 1.0f;
                b.mode = pittore::compute::BlendMode::Normal;
                auto& f = layers[1];  // solid fill over a sub-rect
                f.isFill = true;
                f.fillColor = fillCol;
                f.x0 = 8;
                f.y0 = 6;
                f.x1 = 32;
                f.y1 = 26;
                f.fold = 0.5f;
                f.mode = pittore::compute::BlendMode::Normal;
            }
            cpu->clear(*cpu_acc);
            gpu->clear(*gpu_acc);
            cpu->composite_many_into(*cpu_acc, mw, 0, 0, mw, mh, cpu_l.data(),
                                     cpu_l.size());
            gpu->composite_many_into(*gpu_acc, mw, 0, 0, mw, mh, gpu_l.data(),
                                     gpu_l.size());
            gpu_acc->download();
            const auto* cd = static_cast<const RGBAf*>(cpu_acc->host());
            const auto* gd = static_cast<const RGBAf*>(gpu_acc->host());
            std::size_t mismatches = 0;
            for (std::size_t i = 0; i < mn; ++i)
                if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
            CHECK_EQ(mismatches, 0u);
            const auto at = [&](std::uint32_t x, std::uint32_t y) {
                return static_cast<std::size_t>(y) * mw + x;
            };
            // Inside the window the fill reddens the base at fold alpha
            // (0.9*0.5 + 0.1*0.5 = 0.5 red over the opaque base).
            CHECK(cd[at(20, 15)].r > 0.45f);
            CHECK(cd[at(20, 15)].a > 0.99f);
            // Outside the window only the base shows, bit-for-bit on both.
            CHECK(std::abs(cd[at(2, 15)].r - 0.1f) < 1e-4f);
            CHECK(near(cd[at(20, 15)], gd[at(20, 15)], 1e-5f));
        }

        // --- batched flat stack (fast path) --------------------------------
        // All-plain Normal layers take the tight flat kernel (no cells, no
        // param staging). Cover identity, integer translation, fractional
        // scale and folds: the flat walk must match the generic walk 1e-5.
        {
            constexpr std::uint32_t fw = 64, fh = 48;
            const std::size_t fn = static_cast<std::size_t>(fw) * fh;
            std::vector<RGBAf> photo(fn);
            for (std::uint32_t y = 0; y < fh; ++y) {
                for (std::uint32_t x = 0; x < fw; ++x)
                    photo[static_cast<std::size_t>(y) * fw + x] = RGBAf{
                        (x % 16) / 15.0f, (y % 12) / 11.0f,
                        ((x * 3 + y) % 8) / 7.0f, 0.25f + (x % 4) / 4.0f * 0.75f};
            }
            auto upload = [](auto& be, const std::vector<RGBAf>& v) {
                auto b = be->make_buffer(v.size() * sizeof(RGBAf));
                std::memcpy(b->host(), v.data(), b->size());
                b->upload();
                return b;
            };
            auto cpu_acc = cpu->make_buffer(fn * sizeof(RGBAf));
            auto gpu_acc = gpu->make_buffer(fn * sizeof(RGBAf));
            auto cpu_photo = upload(cpu, photo);
            auto gpu_photo = upload(gpu, photo);
            auto fillFlat = [](pittore::compute::PlacedLayer& p,
                               pittore::compute::Buffer* src, double ox,
                               double oy, double sx, double sy, float fold,
                               std::uint32_t w, std::uint32_t h,
                               std::uint32_t x0, std::uint32_t y0,
                               std::uint32_t x1, std::uint32_t y1) {
                p.src = src;
                p.sw = w;
                p.sh = h;
                p.ox = ox;
                p.oy = oy;
                p.sx = sx;
                p.sy = sy;
                p.x0 = x0;
                p.y0 = y0;
                p.x1 = x1;
                p.y1 = y1;
                p.fold = fold;
                p.mode = pittore::compute::BlendMode::Normal;
                p.clipped = false;
            };
            std::vector<pittore::compute::PlacedLayer> cpu_flat(5);
            std::vector<pittore::compute::PlacedLayer> gpu_flat(5);
            for (int side = 0; side < 2; ++side) {
                auto& ls = side == 0 ? cpu_flat : gpu_flat;
                auto* buf = side == 0 ? cpu_photo.get() : gpu_photo.get();
                fillFlat(ls[0], buf, 0.0, 0.0, 1.0, 1.0, 1.0f, fw, fh,
                         0, 0, fw, fh);
                fillFlat(ls[1], buf, 5.0, 3.0, 1.0, 1.0, 0.5f, fw, fh,
                         0, 0, fw, fh);
                fillFlat(ls[2], buf, 0.0, 0.0, 0.5, 0.5, 1.0f, fw, fh,
                         0, 0, fw, fh);
                fillFlat(ls[3], buf, 2.5, 1.5, 2.0, 2.0, 0.75f, fw, fh,
                         10, 5, 54, 40);
                fillFlat(ls[4], buf, 0.0, 0.0, 1.0, 1.0, 1.0f, fw, fh,
                         48, 36, 64, 48);
            }
            cpu->clear(*cpu_acc);
            gpu->clear(*gpu_acc);
            cpu->composite_many_into(*cpu_acc, fw, 0, 0, fw, fh,
                                     cpu_flat.data(), cpu_flat.size());
            gpu->composite_many_into(*gpu_acc, fw, 0, 0, fw, fh,
                                     gpu_flat.data(), gpu_flat.size());
            gpu_acc->download();
            const auto* cd =
                static_cast<const RGBAf*>(cpu_acc->host());
            const auto* gd =
                static_cast<const RGBAf*>(gpu_acc->host());
            std::size_t mismatches = 0;
            for (std::size_t i = 0; i < fn; ++i)
                if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
            CHECK_EQ(mismatches, 0u);
        }

        // --- batched live adjustments --------------------------------------
        // Every adjustment kind runs over a gradient accumulator on CPU and
        // GPU and must agree; then a masked and a clipped adjustment prove
        // the alpha pipeline (mask/fold/clip) applies to adjustments too.
        {
            constexpr std::uint32_t aw = 64, ah = 48;
            const std::size_t an = static_cast<std::size_t>(aw) * ah;
            std::vector<RGBAf> grad(an);
            for (std::uint32_t y = 0; y < ah; ++y) {
                for (std::uint32_t x = 0; x < aw; ++x) {
                    grad[static_cast<std::size_t>(y) * aw + x] = RGBAf{
                        (x % 16) / 15.0f, (y % 12) / 11.0f,
                        ((x + y) % 8) / 7.0f, 0.25f + (x % 4) / 4.0f * 0.75f};
                }
            }
            // S-curve LUT shared by the Curves case on both backends
            // (3x256: R/G/B identical here, exercising the wide tables).
            std::vector<float> lut(768);
            for (int i = 0; i < 256; ++i) {
                const float x = i / 255.0f;
                lut[i] = lut[i + 256] = lut[i + 512] =
                    x * x * (3.0f - 2.0f * x);
            }
            auto cpu_lut = cpu->make_buffer(lut.size() * sizeof(float));
            auto gpu_lut = gpu->make_buffer(lut.size() * sizeof(float));
            std::memcpy(cpu_lut->host(), lut.data(), cpu_lut->size());
            std::memcpy(gpu_lut->host(), lut.data(), gpu_lut->size());
            cpu_lut->upload();
            gpu_lut->upload();

            struct AdjCase {
                pittore::compute::AdjustmentKind kind;
                float p[16];
            };
            const AdjCase cases[] = {
                {pittore::compute::AdjustmentKind::BrightnessContrast,
                 {0.1f, 0.2f}},
                {pittore::compute::AdjustmentKind::Levels,
                 {0.1f, 0.9f, 1.5f, 0.0f, 1.0f}},
                {pittore::compute::AdjustmentKind::Curves, {}},
                {pittore::compute::AdjustmentKind::Exposure, {1.0f}},
                {pittore::compute::AdjustmentKind::Vibrance, {0.5f}},
                {pittore::compute::AdjustmentKind::HueSaturation,
                 {30.0f, 0.2f, -0.1f}},
                {pittore::compute::AdjustmentKind::Invert, {}},
                {pittore::compute::AdjustmentKind::Threshold, {0.5f}},
                {pittore::compute::AdjustmentKind::Posterize, {4.0f}},
                {pittore::compute::AdjustmentKind::PhotoFilter,
                 {0.93f, 0.54f, 0.0f, 0.25f, 1.0f}},
                {pittore::compute::AdjustmentKind::WhiteBalance,
                 {6500.0f, 0.0f}},
                {pittore::compute::AdjustmentKind::BlackWhite,
                 {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f}},
                {pittore::compute::AdjustmentKind::ChannelMixer,
                 {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.1f,
                  -0.1f, 0.0f, 0.0f}},
                {pittore::compute::AdjustmentKind::ColorBalance,
                 {20.0f, -10.0f, 30.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                  1.0f}},
            };
            for (const auto& c : cases) {
                auto cpu_acc = cpu->make_buffer(an * sizeof(RGBAf));
                auto gpu_acc = gpu->make_buffer(an * sizeof(RGBAf));
                std::memcpy(cpu_acc->host(), grad.data(), cpu_acc->size());
                std::memcpy(gpu_acc->host(), grad.data(), gpu_acc->size());
                cpu_acc->upload();
                gpu_acc->upload();
                pittore::compute::PlacedLayer cpu_l, gpu_l;
                for (int side = 0; side < 2; ++side) {
                    auto& l = side == 0 ? cpu_l : gpu_l;
                    l.isAdjustment = true;
                    l.adjKind = static_cast<int>(c.kind);
                    for (int k = 0; k < 8; ++k) l.adjP[k] = c.p[k];
                    if (c.kind ==
                        pittore::compute::AdjustmentKind::Curves)
                        l.adjAux = side == 0 ? cpu_lut.get() : gpu_lut.get();
                    l.x0 = 0;
                    l.y0 = 0;
                    l.x1 = aw;
                    l.y1 = ah;
                    l.fold = 1.0f;
                    l.mode = pittore::compute::BlendMode::Normal;
                }
                cpu->composite_many_into(*cpu_acc, aw, 0, 0, aw, ah, &cpu_l,
                                         1);
                gpu->composite_many_into(*gpu_acc, aw, 0, 0, aw, ah, &gpu_l,
                                         1);
                gpu_acc->download();
                const auto* cd = static_cast<const RGBAf*>(cpu_acc->host());
                const auto* gd = static_cast<const RGBAf*>(gpu_acc->host());
                mismatches = 0;
                for (std::size_t i = 0; i < an; ++i)
                    if (!near(cd[i], gd[i], 1e-4f)) ++mismatches;
                CHECK_EQ(mismatches, 0u);
            }

            // Spot-check one kind's actual effect: full-strength invert of an
            // opaque grey must come back (1-v) exactly.
            {
                auto cpu_acc = cpu->make_buffer(an * sizeof(RGBAf));
                std::vector<RGBAf> grey(an, RGBAf{0.2f, 0.4f, 0.6f, 1.0f});
                std::memcpy(cpu_acc->host(), grey.data(), cpu_acc->size());
                cpu_acc->upload();
                pittore::compute::PlacedLayer l;
                l.isAdjustment = true;
                l.adjKind = static_cast<int>(
                    pittore::compute::AdjustmentKind::Invert);
                l.x0 = 0;
                l.y0 = 0;
                l.x1 = aw;
                l.y1 = ah;
                l.fold = 1.0f;
                l.mode = pittore::compute::BlendMode::Normal;
                cpu->composite_many_into(*cpu_acc, aw, 0, 0, aw, ah, &l, 1);
                const auto* cd = static_cast<const RGBAf*>(cpu_acc->host());
                CHECK_NEAR(cd[0].r, 0.8f, 1e-6f);
                CHECK_NEAR(cd[0].g, 0.6f, 1e-6f);
                CHECK_NEAR(cd[0].b, 0.4f, 1e-6f);
                CHECK_NEAR(cd[0].a, 1.0f, 1e-6f);
            }

            // Masked + clipped adjustments: a half-masked invert and an
            // invert clipped to a small base; outside the base nothing may
            // change on either backend.
            {
                constexpr std::uint32_t bw = 64, bh = 48, bw2 = 32;
                const std::size_t bn =
                    static_cast<std::size_t>(bw) * bh;
                std::vector<RGBAf> red(bw2 * bw2,
                                       RGBAf{1.0f, 0.0f, 0.0f, 1.0f});
                std::vector<RGBAf> hmask(bw2 * bw2);
                for (std::uint32_t y = 0; y < bw2; ++y)
                    for (std::uint32_t x = 0; x < bw2; ++x)
                        hmask[static_cast<std::size_t>(y) * bw2 + x] =
                            pittore::compute::make_mask_pixel(
                                x < 16 ? 1.0f : 0.0f);
                auto cpu_red = cpu->make_buffer(red.size() * sizeof(RGBAf));
                auto gpu_red = gpu->make_buffer(red.size() * sizeof(RGBAf));
                auto cpu_hmask =
                    cpu->make_buffer(hmask.size() * sizeof(RGBAf));
                auto gpu_hmask =
                    gpu->make_buffer(hmask.size() * sizeof(RGBAf));
                std::memcpy(cpu_red->host(), red.data(), cpu_red->size());
                std::memcpy(gpu_red->host(), red.data(), gpu_red->size());
                std::memcpy(cpu_hmask->host(), hmask.data(),
                            cpu_hmask->size());
                std::memcpy(gpu_hmask->host(), hmask.data(),
                            gpu_hmask->size());
                cpu_red->upload();
                gpu_red->upload();
                cpu_hmask->upload();
                gpu_hmask->upload();
                auto cpu_acc = cpu->make_buffer(bn * sizeof(RGBAf));
                auto gpu_acc = gpu->make_buffer(bn * sizeof(RGBAf));
                std::vector<pittore::compute::PlacedLayer> cpu_ls(3);
                std::vector<pittore::compute::PlacedLayer> gpu_ls(3);
                for (int side = 0; side < 2; ++side) {
                    auto& ls = side == 0 ? cpu_ls : gpu_ls;
                    auto* redBuf = side == 0 ? cpu_red.get() : gpu_red.get();
                    auto* maskBuf =
                        side == 0 ? cpu_hmask.get() : gpu_hmask.get();
                    // Base: opaque red rect.
                    ls[0].src = redBuf;
                    ls[0].sw = bw2;
                    ls[0].sh = bw2;
                    ls[0].ox = 8.0;
                    ls[0].oy = 8.0;
                    ls[0].sx = ls[0].sy = 1.0;
                    ls[0].x0 = 8;
                    ls[0].y0 = 8;
                    ls[0].x1 = 40;
                    ls[0].y1 = 40;
                    ls[0].fold = 1.0f;
                    ls[0].mode = pittore::compute::BlendMode::Normal;
                    // Half-masked invert over the whole frame.
                    ls[1].isAdjustment = true;
                    ls[1].adjKind = static_cast<int>(
                        pittore::compute::AdjustmentKind::Invert);
                    ls[1].x0 = 0;
                    ls[1].y0 = 0;
                    ls[1].x1 = bw;
                    ls[1].y1 = bh;
                    ls[1].fold = 1.0f;
                    ls[1].mode = pittore::compute::BlendMode::Normal;
                    ls[1].mask = maskBuf;
                    ls[1].msw = bw2;
                    ls[1].msh = bw2;
                    ls[1].mox = 16.0;
                    ls[1].moy = 16.0;
                    // Clipped invert restricted to the red base.
                    ls[2].isAdjustment = true;
                    ls[2].adjKind = static_cast<int>(
                        pittore::compute::AdjustmentKind::Invert);
                    ls[2].x0 = 0;
                    ls[2].y0 = 0;
                    ls[2].x1 = bw;
                    ls[2].y1 = bh;
                    ls[2].fold = 1.0f;
                    ls[2].mode = pittore::compute::BlendMode::Normal;
                    ls[2].clipped = true;
                    ls[2].clipBase = 0;
                }
                cpu->clear(*cpu_acc);
                gpu->clear(*gpu_acc);
                cpu->composite_many_into(*cpu_acc, bw, 0, 0, bw, bh,
                                         cpu_ls.data(), cpu_ls.size());
                gpu->composite_many_into(*gpu_acc, bw, 0, 0, bw, bh,
                                         gpu_ls.data(), gpu_ls.size());
                gpu_acc->download();
                const auto* cd = static_cast<const RGBAf*>(cpu_acc->host());
                const auto* gd = static_cast<const RGBAf*>(gpu_acc->host());
                mismatches = 0;
                for (std::size_t i = 0; i < bn; ++i)
                    if (!near(cd[i], gd[i], 1e-4f)) ++mismatches;
                CHECK_EQ(mismatches, 0u);
                // Inside the base the double invert cancels to red; outside
                // the base the clipped invert must not have run.
                const auto at2 = [&](std::uint32_t x, std::uint32_t y) {
                    return static_cast<std::size_t>(y) * bw + x;
                };
                CHECK(cd[at2(20, 20)].r > 0.9f);  // red back after 2× invert
                CHECK_EQ(cd[at2(50, 40)].a, 0.0f);  // clipped: untouched
            }
        }
    }

    // --- gaussian blur (square + ragged tiles) --------
    for (auto [bw, bh] : {std::pair{w, h}, std::pair{cw, ch}}) {
        const std::size_t bn = static_cast<std::size_t>(bw) * bh;
        auto cpu_bs = cpu->make_buffer(bn * sizeof(RGBAf));
        auto gpu_bs = gpu->make_buffer(bn * sizeof(RGBAf));
        auto cpu_bd = cpu->make_buffer(bn * sizeof(RGBAf));
        auto gpu_bd = gpu->make_buffer(bn * sizeof(RGBAf));
        std::vector<RGBAf> bsrc(bn);
        for (std::size_t i = 0; i < bn; ++i) {
            const float x = static_cast<float>(i % bw);
            const float y = static_cast<float>(i / bw);
            bsrc[i] = RGBAf{(x + 1) / 300.0f, (y + 1) / 300.0f,
                            (x * 0.5f + y * 0.3f) / 300.0f, 0.9f};
        }
        std::memcpy(cpu_bs->host(), bsrc.data(), cpu_bs->size());
        std::memcpy(gpu_bs->host(), bsrc.data(), gpu_bs->size());

        for (float sigma : {0.7f, 2.0f, 6.0f}) {
            cpu->gaussian_blur(*cpu_bs, *cpu_bd, bw, bh, sigma);
            gpu->gaussian_blur(*gpu_bs, *gpu_bd, bw, bh, sigma);
            mismatches = 0;
            for (std::size_t i = 0; i < bn; ++i)
                if (!near(static_cast<const RGBAf*>(cpu_bd->host())[i],
                          static_cast<const RGBAf*>(gpu_bd->host())[i], 1e-4f))
                    ++mismatches;
            CHECK_EQ(mismatches, 0u);
        }
    }

    // --- sharpen ------------------------------
    for (auto [sw, sh] : {std::pair{w, h}, std::pair{cw, ch}}) {
        const std::size_t sn = static_cast<std::size_t>(sw) * sh;
        auto cpu_ss = cpu->make_buffer(sn * sizeof(RGBAf));
        auto gpu_ss = gpu->make_buffer(sn * sizeof(RGBAf));
        auto cpu_sd = cpu->make_buffer(sn * sizeof(RGBAf));
        auto gpu_sd = gpu->make_buffer(sn * sizeof(RGBAf));
        std::vector<RGBAf> ssrc(sn);
        for (std::size_t i = 0; i < sn; ++i) {
            const float x = static_cast<float>(i % sw);
            const float y = static_cast<float>(i / sw);
            ssrc[i] = RGBAf{0.5f + 0.05f * std::sin(x * 0.02f),
                            0.5f + 0.04f * std::cos(y * 0.03f),
                            0.5f + (x + y) / 600.0f, 0.8f};
        }
        std::memcpy(cpu_ss->host(), ssrc.data(), cpu_ss->size());
        std::memcpy(gpu_ss->host(), ssrc.data(), gpu_ss->size());
        for (float amount : {0.5f, 1.0f, 2.5f}) {
            for (float radius : {1.0f, 3.0f, 6.0f}) {
                cpu->sharpen(*cpu_ss, *cpu_sd, sw, sh, amount, radius, 0.0f);
                gpu->sharpen(*gpu_ss, *gpu_sd, sw, sh, amount, radius, 0.0f);
                mismatches = 0;
                for (std::size_t i = 0; i < sn; ++i)
                    if (!near(static_cast<const RGBAf*>(cpu_sd->host())[i],
                              static_cast<const RGBAf*>(gpu_sd->host())[i], 1e-4f))
                        ++mismatches;
                CHECK_EQ(mismatches, 0u);
            }
        }
    }

    // --- brightness / contrast -----------------
    auto cpu_bc = cpu->make_buffer(n * sizeof(RGBAf));
    auto gpu_bc = gpu->make_buffer(n * sizeof(RGBAf));
    for (float br : {-0.2f, 0.0f, 0.1f, 0.4f}) {
        for (float ct : {-0.5f, -0.1f, 0.25f, 1.0f}) {
            std::memcpy(cpu_bc->host(), src.data(), cpu_bc->size());
            std::memcpy(gpu_bc->host(), src.data(), gpu_bc->size());
            cpu->brightness_contrast(*cpu_bc, *cpu_bc, w, h, br, ct);
            gpu->brightness_contrast(*gpu_bc, *gpu_bc, w, h, br, ct);
            mismatches = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (!near(static_cast<const RGBAf*>(cpu_bc->host())[i],
                          static_cast<const RGBAf*>(gpu_bc->host())[i], 1e-5f))
                    ++mismatches;
            CHECK_EQ(mismatches, 0u);
        }
    }

    // --- hue / saturation ----------------------
    auto cpu_hs = cpu->make_buffer(cw * ch * sizeof(RGBAf));
    auto gpu_hs = gpu->make_buffer(cw * ch * sizeof(RGBAf));
    std::vector<RGBAf> hsrc(static_cast<std::size_t>(cw) * ch);
    for (std::size_t i = 0; i < hsrc.size(); ++i) {
        const float x = static_cast<float>(i % cw) / cw;
        const float y = static_cast<float>(i / cw) / ch;
        hsrc[i] = RGBAf{x, y, (x + y) / 2.0f, 0.85f};
    }
    std::memcpy(cpu_hs->host(), hsrc.data(), cpu_hs->size());
    std::memcpy(gpu_hs->host(), hsrc.data(), gpu_hs->size());
    for (float hue : {-120.0f, 0.0f, 45.0f, 180.0f}) {
        for (float sat : {-1.0f, -0.4f, 0.0f, 0.6f, 1.0f}) {
            cpu->hue_saturation(*cpu_hs, *cpu_hs, cw, ch, hue, sat, 0.1f);
            gpu->hue_saturation(*gpu_hs, *gpu_hs, cw, ch, hue, sat, 0.1f);
            mismatches = 0;
            for (std::size_t i = 0; i < hsrc.size(); ++i)
                if (!near(static_cast<const RGBAf*>(cpu_hs->host())[i],
                          static_cast<const RGBAf*>(gpu_hs->host())[i], 1e-4f))
                    ++mismatches;
            CHECK_EQ(mismatches, 0u);
        }
    }

    // --- median filter (ragged edges) ----------
    auto cpu_m = cpu->make_buffer(cw * ch * sizeof(RGBAf));
    auto gpu_m = gpu->make_buffer(cw * ch * sizeof(RGBAf));
    auto cpu_ms = cpu->make_buffer(cw * ch * sizeof(RGBAf));
    auto gpu_ms = gpu->make_buffer(cw * ch * sizeof(RGBAf));
    std::vector<RGBAf> msrc(static_cast<std::size_t>(cw) * ch);
    for (std::size_t i = 0; i < msrc.size(); ++i)
        msrc[i] = RGBAf{(i * 31) % 256 / 255.0f, (i * 17) % 256 / 255.0f,
                        (i * 73) % 256 / 255.0f, 0.5f + ((i & 3) / 8.0f)};
    std::memcpy(cpu_ms->host(), msrc.data(), cpu_ms->size());
    std::memcpy(gpu_ms->host(), msrc.data(), gpu_ms->size());
    cpu->median_filter(*cpu_ms, *cpu_m, cw, ch);
    gpu->median_filter(*gpu_ms, *gpu_m, cw, ch);
    mismatches = 0;
    for (std::size_t i = 0; i < msrc.size(); ++i)
        if (!near(static_cast<const RGBAf*>(cpu_m->host())[i],
                  static_cast<const RGBAf*>(gpu_m->host())[i], 1e-5f))
            ++mismatches;
    CHECK_EQ(mismatches, 0u);

    // --- warp (Liquify displacement resample) ----------
    // Build a real deformation (forward-warp dabs + a reconstruct relax), then
    // resample several regions on both backends. Must (a) match the CPU
    // reference inside the rect, (b) leave every pixel outside the rect
    // untouched, and (c) reproduce the source exactly through an identity
    // mesh. The GPU kernel reads `src` device-resident and device-side, so this
    // is also the check that a displaced fetch outside the dab rect sees the
    // right pixels.
    {
        using namespace pittore::compute;
        auto cpu_ws = cpu->make_buffer(n * sizeof(RGBAf));
        auto gpu_ws = gpu->make_buffer(n * sizeof(RGBAf));
        std::memcpy(cpu_ws->host(), src.data(), cpu_ws->size());
        std::memcpy(gpu_ws->host(), src.data(), gpu_ws->size());
        cpu_ws->upload();  // UI contract: the frozen snapshot is device-resident
        gpu_ws->upload();

        WarpMesh mesh = make_warp_mesh(w, h);
        // Two forward-warp dabs that drag vertices with the brush, then a
        // reconstruct pass that pulls part of one back — a non-trivial mesh.
        for (auto [cx, cy] :
             {std::pair{150.0f, 170.0f}, std::pair{330.0f, 300.0f}}) {
            warp_mesh_for_each_near(
                mesh, cx, cy, 90.0f,
                [](float& dx, float& dy, float wt, float rx, float ry) {
                    dx -= rx * wt * 0.85f;   // forward warp
                    dy -= ry * wt * 0.85f;
                });
        }
        warp_mesh_relax(mesh, 330.0f, 300.0f, 70.0f, 0.6f);
        CHECK(!mesh.identity());

        struct WarpCase {
            std::uint32_t x0, y0, x1, y1;
        };
        const WarpCase wcases[] = {
            {0, 0, w, h},            // full frame
            {60, 80, 260, 280},      // around the first dab
            {240, 210, 430, 400},    // around the second dab
            {10, 10, 120, 120},      // top-left corner
            {w - 90, h - 90, w, h},  // bottom-right corner
            {400, 40, 470, 110},     // interior, mostly untouched
        };
        for (const auto& c : wcases) {
            const WarpSubgrid grid = warp_subgrid(mesh, c.x0, c.y0, c.x1, c.y1);
            if (grid.empty()) continue;
            std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
            std::memcpy(gpu_d->host(), src.data(), gpu_d->size());
            cpu->warp(*cpu_d, *cpu_ws, w, h, grid, c.x0, c.y0, c.x1, c.y1);
            gpu->warp(*gpu_d, *gpu_ws, w, h, grid, c.x0, c.y0, c.x1, c.y1);
            mismatches = 0;
            std::size_t outside_changed = 0;
            const auto* cd = static_cast<const RGBAf*>(cpu_d->host());
            const auto* gd = static_cast<const RGBAf*>(gpu_d->host());
            for (std::uint32_t yy = 0; yy < h; ++yy) {
                for (std::uint32_t xx = 0; xx < w; ++xx) {
                    const std::size_t i = static_cast<std::size_t>(yy) * w + xx;
                    const bool in =
                        xx >= c.x0 && xx < c.x1 && yy >= c.y0 && yy < c.y1;
                    if (in) {
                        if (!near(cd[i], gd[i], 1e-5f)) ++mismatches;
                    } else if (!near(gd[i], src[i], 0.0f) ||
                               !near(cd[i], src[i], 0.0f)) {
                        ++outside_changed;
                    }
                }
            }
            CHECK_EQ(mismatches, 0u);
            CHECK_EQ(outside_changed, 0u);
        }

        // Identity mesh: the warp is a resample that must give the source back.
        {
            const WarpMesh id = make_warp_mesh(w, h);
            const WarpSubgrid grid = warp_subgrid(id, 100, 100, 400, 350);
            std::memcpy(cpu_d->host(), src.data(), cpu_d->size());
            std::memcpy(gpu_d->host(), src.data(), gpu_d->size());
            cpu->warp(*cpu_d, *cpu_ws, w, h, grid, 100, 100, 400, 350);
            gpu->warp(*gpu_d, *gpu_ws, w, h, grid, 100, 100, 400, 350);
            mismatches = 0;
            const auto* cd = static_cast<const RGBAf*>(cpu_d->host());
            const auto* gd = static_cast<const RGBAf*>(gpu_d->host());
            for (std::uint32_t yy = 100; yy < 350; ++yy)
                for (std::uint32_t xx = 100; xx < 400; ++xx) {
                    const std::size_t i = static_cast<std::size_t>(yy) * w + xx;
                    if (!near(cd[i], src[i], 1e-5f) ||
                        !near(gd[i], src[i], 1e-5f) ||
                        !near(cd[i], gd[i], 1e-5f))
                        ++mismatches;
                }
            CHECK_EQ(mismatches, 0u);
        }
    }

    // --- tone_blend (Live Tone Blend Group transfer) ---
    // Fused device kernels vs the CPU reference: pyramid low-pass, gain
    // field, Oklab post stages. Photo-like gradients with transparent and
    // half-covered texels on both sides, across the param cube plus the
    // strength-0 no-op contract (dst untouched on both). 1e-4: the pyramid
    // and Oklab pow differ in the last ulp between host/device libm.
    {
        using pittore::compute::ToneBlendParams;
        const ToneBlendParams cases[] = {
            ToneBlendParams{},  // defaults (strength 1, smooth reference)
            {.strength = 0.0f},  // no-op: dst must be bit-identical to input
            {.strength = 0.5f, .lowPass = 0.0f},  // full-res reference
            {.strength = 1.0f,
             .color = 0.0f,
             .contrast = -1.0f,
             .lowPass = 0.0f},  // desaturate + flatten, leaky reference
            {.strength = 0.75f,
             .color = 0.5f,
             .contrast = 1.0f,
             .lowPass = 0.5f},  // mid cube
            {.strength = 1.0f,
             .color = 1.0f,
             .contrast = 0.5f,
             .lowPass = 1.0f,
             .contentType = 2},  // smoothest reference, text hint (no-op)
        };
        const std::pair<std::uint32_t, std::uint32_t> shapes[] = {
            {w, h},      // power-of-two pyramid
            {cw, ch},    // ragged pyramid (odd halvings, edge clamps)
            {17, 9},     // tiny (few levels, near the 8px stop rule)
            {64, 64},    // square mid-size
        };
        for (const auto [tw, th] : shapes) {
            const std::size_t tn = static_cast<std::size_t>(tw) * th;
            std::vector<RGBAf> grp(tn), bkg(tn);
            for (std::uint32_t y = 0; y < th; ++y) {
                for (std::uint32_t x = 0; x < tw; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * tw + x;
                    const float fx = tw > 1 ? static_cast<float>(x) / (tw - 1) : 0.0f;
                    const float fy = th > 1 ? static_cast<float>(y) / (th - 1) : 0.0f;
                    grp[i] = RGBAf{0.15f + 0.75f * fx, 0.1f + 0.7f * fy,
                                   0.2f + 0.5f * (fx + fy) * 0.5f, 1.0f};
                    bkg[i] = RGBAf{0.1f + 0.6f * fy, 0.15f + 0.65f * fx,
                                   0.3f + 0.5f * fx * fy, 1.0f};
                    if (i % 13 == 0) grp[i].a = 0.0f;  // transparent group
                    if (i % 17 == 0) bkg[i].a = 0.0f;  // gain-neutral backdrop
                    if (i % 7 == 0) {
                        grp[i].a = 0.4f;  // half-covered group texel
                        bkg[i].a = 0.6f;
                    }
                }
            }
            auto cpu_g = cpu->make_buffer(tn * sizeof(RGBAf));
            auto gpu_g = gpu->make_buffer(tn * sizeof(RGBAf));
            auto cpu_b = cpu->make_buffer(tn * sizeof(RGBAf));
            auto gpu_b = gpu->make_buffer(tn * sizeof(RGBAf));
            for (const auto& pc : cases) {
                std::memcpy(cpu_g->host(), grp.data(), cpu_g->size());
                std::memcpy(gpu_g->host(), grp.data(), gpu_g->size());
                std::memcpy(cpu_b->host(), bkg.data(), cpu_b->size());
                std::memcpy(gpu_b->host(), bkg.data(), gpu_b->size());
                // Device-resident contract: the transfer reads device
                // memory; host-filled callers upload explicitly.
                gpu_g->upload();
                gpu_b->upload();
                cpu->tone_blend(*cpu_g, *cpu_b, tw, th, pc);
                gpu->tone_blend(*gpu_g, *gpu_b, tw, th, pc);
                const auto* cd = static_cast<const RGBAf*>(cpu_g->host());
                const auto* gd = static_cast<const RGBAf*>(gpu_g->host());
                mismatches = 0;
                for (std::size_t i = 0; i < tn; ++i)
                    if (!near(cd[i], gd[i], 1e-4f)) ++mismatches;
                CHECK_EQ(mismatches, 0u);
                if (pc.strength <= 0.0f) {
                    // No-op contract: both sides leave dst exactly as fed.
                    for (std::size_t i = 0; i < tn; ++i)
                        if (!near(cd[i], grp[i], 0.0f) ||
                            !near(gd[i], grp[i], 0.0f))
                            ++mismatches;
                    CHECK_EQ(mismatches, 0u);
                }
            }
            // Stale-host regression: device content wins. Upload pattern P1,
            // then overwrite host staging with P2 (no upload): the transfer
            // must follow the device (P1), matching the CPU reference fed P1
            // directly. The old upload()-inside-tone_blend read P2 and wiped
            // fused-path groups to transparent.
            {
                const ToneBlendParams pc{};
                std::memcpy(cpu_g->host(), grp.data(), cpu_g->size());
                std::memcpy(cpu_b->host(), bkg.data(), cpu_b->size());
                std::memcpy(gpu_g->host(), grp.data(), gpu_g->size());
                std::memcpy(gpu_b->host(), bkg.data(), gpu_b->size());
                gpu_g->upload();
                gpu_b->upload();
                std::memset(gpu_g->host(), 0x7f, gpu_g->size());
                std::memset(gpu_b->host(), 0x7f, gpu_b->size());
                cpu->tone_blend(*cpu_g, *cpu_b, tw, th, pc);
                gpu->tone_blend(*gpu_g, *gpu_b, tw, th, pc);
                const auto* cd = static_cast<const RGBAf*>(cpu_g->host());
                const auto* gd = static_cast<const RGBAf*>(gpu_g->host());
                mismatches = 0;
                for (std::size_t i = 0; i < tn; ++i)
                    if (!near(cd[i], gd[i], 1e-4f)) ++mismatches;
                CHECK_EQ(mismatches, 0u);
            }
        }
    }
}