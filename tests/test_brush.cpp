// Brush dab kernel: coverage, hardness, opacity, edge clipping, source-over.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/compute/brushes/blur/blur_dab.h"
#include "engine/compute/brushes/erase/erase.h"
#include "engine/compute/brushes/heal/heal.h"
#include "engine/compute/brushes/history/history_dab.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/compute/brushes/loaders/loaders.h"
#include "engine/compute/brushes/loaders/pat.h"
#include "engine/compute/brushes/mix/mix.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "engine/compute/factory.h"
#include "engine/compute/blend.h"
#include "engine/color/icc_convert.h"
#include "engine/compute/layer_mask.h"
#include "engine/compute/paint.h"
#include "engine/core/pixel.h"
#include "test_util.h"
#include "ui/brushes/sensor_drives.h"
#include "ui/canvas/shared/symmetry.h"

using pittore::RGBAf;

namespace {

void fill(std::vector<RGBAf>& v, std::uint32_t w, std::uint32_t h) {
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            v[static_cast<std::size_t>(y) * w + x] = RGBAf{0, 0, 0, 0};
}

}  // namespace

void test_brush() {
    auto backend = pittore::compute::make_default_backend();

    // 1. Hard dab, full opacity: opaque core, zero alpha outside the radius.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        fill(img, w, h);
        auto buf = backend->make_buffer(img.size() * sizeof(RGBAf));
        std::memcpy(buf->host(), img.data(), buf->size());
        buf->upload();
        backend->paint_dab(*buf, w, h, 8.0f, 8.0f, 4.0f, 1.0f, 1.0f, RGBAf{1, 0, 0, 0});
        const RGBAf* out = static_cast<const RGBAf*>(buf->host());

        CHECK_NEAR(out[8 * w + 8].r, 1.0f, 1e-6f);   // centre: red
        CHECK_NEAR(out[8 * w + 8].g, 0.0f, 1e-6f);
        CHECK_NEAR(out[8 * w + 8].a, 1.0f, 1e-6f);
        CHECK_NEAR(out[8 * w + 12].a, 0.0f, 1e-6f);  // outside radius +4
        CHECK_NEAR(out[0].a, 0.0f, 1e-6f);           // far corner untouched
    }

    // 2. Partial opacity stays partial (no build-up in a single dab).
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        fill(img, w, h);
        auto buf = backend->make_buffer(img.size() * sizeof(RGBAf));
        std::memcpy(buf->host(), img.data(), buf->size());
        buf->upload();
        backend->paint_dab(*buf, w, h, 8.0f, 8.0f, 4.0f, 1.0f, 0.4f, RGBAf{1, 0, 0, 0});
        const RGBAf* out = static_cast<const RGBAf*>(buf->host());
        CHECK_NEAR(out[8 * w + 8].a, 0.4f, 1e-5f);
        CHECK_NEAR(out[8 * w + 8].r, 1.0f, 1e-5f);
    }

    // 3. Software dab: alpha tapers toward the rim, reaches zero at radius.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        fill(img, w, h);
        auto buf = backend->make_buffer(img.size() * sizeof(RGBAf));
        std::memcpy(buf->host(), img.data(), buf->size());
        buf->upload();  // push staged data to the device (no-op on CPU)
        backend->paint_dab(*buf, w, h, 8.0f, 8.0f, 4.0f, 0.0f, 1.0f, RGBAf{0, 0, 1, 0});
        const RGBAf* out = static_cast<const RGBAf*>(buf->host());
        // Linear ramp from centre: at the centre pixel's own centre
        // (8.5,8.5), dist = sqrt(0.5), so coverage = 1 - sqrt(0.5)/4.
        CHECK_NEAR(out[8 * w + 8].a, 1.0f - std::sqrt(0.5f) / 4.0f, 1e-5f);
        CHECK_NEAR(out[8 * w + 12].a, 0.0f, 1e-6f);  // at radius 4
        // Midpoint ring (2 px from centre) is roughly ~0.5 alpha.
        CHECK_NEAR(out[8 * w + 10].a, 0.5f, 0.15f);
        // And the shaft interior is covered (bluish).
        CHECK_NEAR(out[8 * w + 9].b, 1.0f, 1e-6f);
    }

    // 4. Edge clipping: dab centred on the corner only affects in-bounds px.
    {
        constexpr std::uint32_t w = 8, h = 8;
        std::vector<RGBAf> img(w * h);
        fill(img, w, h);
        auto buf = backend->make_buffer(img.size() * sizeof(RGBAf));
        std::memcpy(buf->host(), img.data(), buf->size());
        buf->upload();
        backend->paint_dab(*buf, w, h, 0.0f, 0.0f, 4.0f, 1.0f, 1.0f, RGBAf{0, 1, 0, 0});
        const RGBAf* out = static_cast<const RGBAf*>(buf->host());
        CHECK_NEAR(out[0].a, 1.0f, 1e-6f);
        CHECK_NEAR(out[3].a, 1.0f, 1e-6f);   // in-range corner of the radius
        CHECK_NEAR(out[5].a, 0.0f, 1e-6f);   // outside
        CHECK_NEAR(out[7 * w + 7].a, 0.0f, 1e-6f);
        CHECK_NEAR(out[0].g, 1.0f, 1e-6f);   // painted green
    }

    // 5. Source-over onto existing content: dab over white at 50% = 50% grey.
    {
        constexpr std::uint32_t w = 8, h = 8;
        std::vector<RGBAf> img(w * h);
        fill(img, w, h);
        for (auto& p : img) p = RGBAf{1, 1, 1, 1};  // opaque white canvas
        auto buf = backend->make_buffer(img.size() * sizeof(RGBAf));
        std::memcpy(buf->host(), img.data(), buf->size());
        buf->upload();
        backend->paint_dab(*buf, w, h, 4.0f, 4.0f, 3.0f, 1.0f, 0.5f, RGBAf{0, 0, 0, 0});
        const RGBAf* out = static_cast<const RGBAf*>(buf->host());
        CHECK_NEAR(out[4 * w + 4].a, 1.0f, 1e-5f);  // stays opaque
        CHECK_NEAR(out[4 * w + 4].r, 0.5f, 1e-5f);  // 0.5 black over 1.0 white
        CHECK_NEAR(out[0].r, 1.0f, 1e-6f);          // untouched corner stays white
    }

    // 6. Host helper path agrees with the backend default (CPU parity).
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        fill(img, w, h);
        std::vector<RGBAf> direct = img;
        auto buf = backend->make_buffer(img.size() * sizeof(RGBAf));
        std::memcpy(buf->host(), img.data(), buf->size());
        buf->upload();
        backend->paint_dab(*buf, w, h, 5.0f, 5.0f, 3.0f, 0.5f, 0.7f, RGBAf{1, 0.5f, 0, 0});
        pittore::compute::paint_dab_host(direct.data(), w, h, 5.0f, 5.0f, 3.0f,
                                          0.5f, 0.7f, RGBAf{1, 0.5f, 0, 0});
        const RGBAf* via_backend = static_cast<const RGBAf*>(buf->host());
        for (std::size_t i = 0; i < img.size(); ++i) {
            CHECK_NEAR(via_backend[i].r, direct[i].r, 1e-6f);
            CHECK_NEAR(via_backend[i].a, direct[i].a, 1e-6f);
        }
    }

    // Native CMYK dab updates the retained inks and derives its RGB preview
    // from those exact device samples.
    {
        RGBAf pixel{0.5f, 0.5f, 0.5f, 1.0f};
        std::array<std::uint16_t, 4> inks{1000, 2000, 3000, 4000};
        pittore::color::CmykToSrgb preview(nullptr, 0);
        pittore::compute::NativeCmykPaint native;
        native.pixels = &inks;
        native.source = {50000, 10000, 20000, 30000};
        native.preview = &preview;
        pittore::compute::paint_dab_host(
            &pixel, 1, 1, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f,
            RGBAf{1, 0, 0, 1}, nullptr, nullptr, nullptr, nullptr, nullptr,
            pittore::compute::dabmix::DabMixOp::Normal, &native);
        CHECK(inks == native.source);
        float expected[3];
        preview.convert(inks[0] / 65535.0f, inks[1] / 65535.0f,
                        inks[2] / 65535.0f, inks[3] / 65535.0f, expected);
        CHECK_NEAR(pixel.r, expected[0], 1e-6f);
        CHECK_NEAR(pixel.g, expected[1], 1e-6f);
        CHECK_NEAR(pixel.b, expected[2], 1e-6f);
        CHECK_NEAR(pixel.a, 1.0f, 1e-6f);
    }
    {
        RGBAf pixel{0.5f, 0.5f, 0.5f, 1.0f};
        std::array<std::uint16_t, 4> inks{1000, 2000, 3000, 4000};
        pittore::color::CmykToSrgb preview(nullptr, 0);
        pittore::compute::NativeCmykPaint native;
        native.pixels = &inks;
        native.source = {50000, 10000, 20000, 30000};
        native.preview = &preview;
        pittore::compute::StampTip stamp;
        stamp.w = stamp.h = 1;
        stamp.alpha = {1.0f};
        pittore::compute::stamp_dab_host(
            &pixel, 1, 1, 0.5f, 0.5f, 1.0f, stamp,
            pittore::compute::StampMode::AlphaMask, 1.0f,
            RGBAf{1, 0, 0, 1}, 0.0f, nullptr, nullptr, nullptr, nullptr,
            0, 1, nullptr, nullptr, nullptr, 0.0f,
            pittore::compute::dabmix::DabMixOp::Normal, nullptr, nullptr,
            &native);
        CHECK(inks == native.source);
        float expected[3];
        preview.convert(inks[0] / 65535.0f, inks[1] / 65535.0f,
                        inks[2] / 65535.0f, inks[3] / 65535.0f, expected);
        CHECK_NEAR(pixel.r, expected[0], 1e-6f);
        CHECK_NEAR(pixel.g, expected[1], 1e-6f);
        CHECK_NEAR(pixel.b, expected[2], 1e-6f);
    }
    {
        const RGBAf match{0, 0, 0, 1};
        RGBAf pixel{0, 0, 0, 1};
        std::array<std::uint16_t, 4> inks{1000, 2000, 3000, 4000};
        pittore::color::CmykToSrgb preview(nullptr, 0);
        pittore::compute::NativeCmykPaint native;
        native.pixels = &inks;
        native.source = {50000, 10000, 20000, 30000};
        native.preview = &preview;
        int bbox[4]{};
        CHECK(pittore::compute::flood_fill_host(
            &match, &pixel, 1, 1, 0, 0, 0.0f, true, false, 1.0f,
            RGBAf{0, 1, 0, 1}, false, bbox, nullptr, &native));
        CHECK(inks == native.source);
        float expected[3];
        preview.convert(inks[0] / 65535.0f, inks[1] / 65535.0f,
                        inks[2] / 65535.0f, inks[3] / 65535.0f, expected);
        CHECK_NEAR(pixel.r, expected[0], 1e-6f);
        CHECK_NEAR(pixel.g, expected[1], 1e-6f);
        CHECK_NEAR(pixel.b, expected[2], 1e-6f);
    }
    {
        RGBAf pixel{1, 0, 0, 1};
        const RGBAf target{1, 0, 0, 1};
        std::array<std::uint16_t, 4> inks{};
        pittore::color::SrgbToCmyk separation(nullptr, 0);
        pittore::color::CmykToSrgb preview(nullptr, 0);
        float originalInk[4];
        separation.convert(1, 0, 0, originalInk);
        for (int c = 0; c < 4; ++c)
            inks[c] = static_cast<std::uint16_t>(
                std::lround(originalInk[c] * 65535.0f));
        pittore::compute::NativeCmykPaint native;
        native.pixels = &inks;
        native.preview = &preview;
        native.separation = &separation;
        int bbox[4]{};
        CHECK(pittore::compute::replace_color_dab_host(
            &pixel, 1, 1, 0.5f, 0.5f, 1.0f, 1.0f, &target, 1,
            RGBAf{0, 0, 1, 1}, 0.0f,
            pittore::compute::ReplaceMode::Color, 0, false, 0.0f, bbox,
            nullptr, &native));
        float nr, ng, nb;
        pittore::compute::blend::stitch_color(
            pittore::compute::BlendMode::Color, 1, 0, 0, 0, 0, 1,
            nr, ng, nb);
        float expected[4];
        separation.convert(nr, ng, nb, expected);
        for (int c = 0; c < 4; ++c)
            CHECK(std::abs(int(inks[c]) -
                           int(std::lround(expected[c] * 65535.0f))) <= 1);
        float expectedRgb[3];
        preview.convert(inks[0] / 65535.0f, inks[1] / 65535.0f,
                        inks[2] / 65535.0f, inks[3] / 65535.0f, expectedRgb);
        CHECK_NEAR(pixel.r, expectedRgb[0], 1e-6f);
        CHECK_NEAR(pixel.g, expectedRgb[1], 1e-6f);
        CHECK_NEAR(pixel.b, expectedRgb[2], 1e-6f);
    }

    // 7. Incremental region composite == full composite (Normal).
    //    Composite in two disjunct regions must equal one whole-buffer pass.
    {
        constexpr std::uint32_t w = 24, h = 20;
        std::vector<RGBAf> full(w * h), split(w * h), src(w * h);
        for (std::uint32_t i = 0; i < w * h; ++i) {
            full[i] = RGBAf{0.2f, 0.4f, 0.6f, 0.7f};   // some base underneath
            split[i] = full[i];
            src[i] = RGBAf{0.9f, 0.5f, 0.1f, 0.6f};    // the layer on top
        }
        std::vector<RGBAf> base = full;                 // untouched copy for parity
        auto bb = backend->make_buffer(full.size() * sizeof(RGBAf));
        auto sb = backend->make_buffer(src.size() * sizeof(RGBAf));
        std::memcpy(bb->host(), base.data(), bb->size());
        std::memcpy(sb->host(), src.data(), sb->size());
        bb->upload();
        sb->upload();
        backend->composite(*bb, *sb, w, h, pittore::compute::BlendMode::Normal);
        std::memcpy(full.data(), bb->host(), bb->size());

        // Region pass: left half then right half, same base/src.
        pittore::compute::composite_region_host(split.data(), src.data(), w, 0, 0,
                                                 w / 2, h,
                                                 pittore::compute::BlendMode::Normal);
        pittore::compute::composite_region_host(split.data(), src.data(), w, w / 2, 0,
                                                 w, h,
                                                 pittore::compute::BlendMode::Normal);
        for (std::size_t i = 0; i < full.size(); ++i) {
            CHECK_NEAR(split[i].r, full[i].r, 1e-6f);
            CHECK_NEAR(split[i].g, full[i].g, 1e-6f);
            CHECK_NEAR(split[i].b, full[i].b, 1e-6f);
            CHECK_NEAR(split[i].a, full[i].a, 1e-6f);
        }
    }

    // 9. Eraser on opaque canvas: full opacity → transparent inside the core.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        for (auto& p : img) p = RGBAf{1, 1, 1, 1};  // opaque white canvas
        pittore::compute::erase_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                          1.0f, 1.0f);
        CHECK_NEAR(img[8 * w + 8].a, 0.0f, 1e-6f);  // centre fully erased
        CHECK_NEAR(img[8 * w + 8].r, 1.0f, 1e-6f);  // rgb untouched
        CHECK_NEAR(img[8 * w + 12].a, 1.0f, 1e-6f); // outside radius
        CHECK_NEAR(img[0].a, 1.0f, 1e-6f);
    }

    // 10. Partial-opacity erase: alpha scales down, colour kept.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        for (auto& p : img) p = RGBAf{0.3f, 0.6f, 0.9f, 1.0f};
        pittore::compute::erase_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                          1.0f, 0.5f);
        CHECK_NEAR(img[8 * w + 8].a, 0.5f, 1e-5f);   // 1.0 * (1 - 0.5)
        CHECK_NEAR(img[8 * w + 8].r, 0.3f, 1e-6f);   // colour unchanged
        // Erasing twice at 50% accumulates: 0.5 -> 0.25.
        pittore::compute::erase_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                          1.0f, 0.5f);
        CHECK_NEAR(img[8 * w + 8].a, 0.25f, 1e-5f);
    }

    // 11. Soft eraser: alpha tapers toward the rim like the paint dab.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        for (auto& p : img) p = RGBAf{1, 1, 1, 1};
        pittore::compute::erase_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                          0.0f, 1.0f);
        CHECK_NEAR(img[8 * w + 8].a, 1.0f - (1.0f - std::sqrt(0.5f) / 4.0f), 1e-5f);
        CHECK_NEAR(img[8 * w + 12].a, 1.0f, 1e-6f);  // at radius: no erase
    }

    // 12. Eraser parity with paint dab: painting a semi-soft dab then erasing
    //     the same spot at full opacity restores full transparency at the core.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h);
        for (auto& p : img) p = RGBAf{1, 1, 1, 1};
        pittore::compute::paint_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                          0.4f, 0.6f, RGBAf{0, 0, 0, 0});
        // Erase the same geometry at opacity 1.: alpha × (1 - cov).
        pittore::compute::erase_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                          0.4f, 1.0f);
        CHECK_NEAR(img[8 * w + 8].a, 0.0f, 1e-6f);  // centre fully transparent again
        CHECK_NEAR(img[0].a, 1.0f, 1e-6f);          // untouched corner keeps alpha
        CHECK_NEAR(img[8 * w + 12].a, 1.0f, 1e-6f); // outside the radius
    }

    // 13. Mask dabs move opaque-grey coverage toward the brush value while
    //     keeping the mask opaque.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
        pittore::compute::mask_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                         1.0f, 1.0f, 0.0f);
        CHECK_NEAR(img[8 * w + 8].r, 0.0f, 1e-6f);
        CHECK_NEAR(img[8 * w + 8].g, 0.0f, 1e-6f);
        CHECK_NEAR(img[8 * w + 8].b, 0.0f, 1e-6f);
        CHECK_NEAR(img[8 * w + 8].a, 1.0f, 1e-6f);
        CHECK_NEAR(img[0].r, 1.0f, 1e-6f);
        pittore::compute::mask_dab_host(img.data(), w, h, 8.0f, 8.0f, 4.0f,
                                         1.0f, 0.5f, 1.0f);
        CHECK_NEAR(img[8 * w + 8].r, 0.5f, 1e-6f);
        CHECK_NEAR(img[8 * w + 8].a, 1.0f, 1e-6f);
    }

    // 14. Auto-tip round defaults reproduce the round kernel bit-exactly.
    {
        constexpr std::uint32_t w = 16, h = 16;
        std::vector<RGBAf> a(w * h), b(w * h);
        fill(a, w, h);
        fill(b, w, h);
        pittore::compute::paint_dab_host(a.data(), w, h, 8.0f, 8.0f, 4.0f,
                                         0.5f, 0.7f, RGBAf{1, 0.5f, 0, 0});
        pittore::compute::AutoTip tip;  // round, ratio 1, angle 0
        tip.hardness = 0.5f;
        pittore::compute::paint_tip_dab_host(b.data(), w, h, 8.0f, 8.0f, 4.0f,
                                             tip, 0.7f, RGBAf{1, 0.5f, 0, 0});
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK_NEAR(b[i].r, a[i].r, 1e-6f);
            CHECK_NEAR(b[i].a, a[i].a, 1e-6f);
        }
    }

    // 15. Ellipse: ratio 0.5 leaves the major (x) axis full and squeezes
    // the minor (y) axis, so angle 0 paints wide; angle 90 swaps them.
    // Positive angles turn counter-clockwise: at 45 the major runs from
    // lower-left to upper-right.
    {
        constexpr std::uint32_t w = 24, h = 24;
        auto inkAt = [&](float ratio, float angle, int x, int y) {
            std::vector<RGBAf> img(w * h);
            fill(img, w, h);
            pittore::compute::AutoTip tip;
            tip.ratio = ratio;
            tip.angleDeg = angle;
            tip.hardness = 1.0f;
            pittore::compute::paint_tip_dab_host(img.data(), w, h, 12.0f,
                                                 12.0f, 4.0f, tip, 1.0f,
                                                 RGBAf{1, 0, 0, 0});
            return img[static_cast<std::size_t>(y) * w + x].a;
        };
        // Major axis is x at angle 0: 3px along x paints, 3px up the minor
        // axis (radius 2) does not.
        CHECK_NEAR(inkAt(0.5f, 0.0f, 15, 12), 1.0f, 1e-6f);
        CHECK_NEAR(inkAt(0.5f, 0.0f, 12, 9), 0.0f, 1e-6f);
        // Rotated 90 degrees: the axes swap.
        CHECK_NEAR(inkAt(0.5f, 90.0f, 12, 9), 1.0f, 1e-6f);
        CHECK_NEAR(inkAt(0.5f, 90.0f, 15, 12), 0.0f, 1e-6f);
    }

    // 15b. Rotation direction: at 45 degrees counter-clockwise the major
    // axis runs lower-left to upper-right (a mirrored convention would
    // paint the opposite diagonal).
    {
        constexpr std::uint32_t w = 24, h = 24;
        auto inkAt = [&](int x, int y) {
            std::vector<RGBAf> img(w * h);
            fill(img, w, h);
            pittore::compute::AutoTip tip;
            tip.ratio = 0.5f;
            tip.angleDeg = 45.0f;
            tip.hardness = 1.0f;
            pittore::compute::paint_tip_dab_host(img.data(), w, h, 12.0f,
                                                 12.0f, 4.0f, tip, 1.0f,
                                                 RGBAf{1, 0, 0, 0});
            return img[static_cast<std::size_t>(y) * w + x].a;
        };
        CHECK_NEAR(inkAt(14, 10), 1.0f, 1e-6f);
        CHECK_NEAR(inkAt(14, 14), 0.0f, 1e-6f);
    }

    // 16. Square tip paints the corners the round tip skips.
    {
        constexpr std::uint32_t w = 24, h = 24;
        std::vector<RGBAf> round(w * h), square(w * h);
        fill(round, w, h);
        fill(square, w, h);
        pittore::compute::paint_dab_host(round.data(), w, h, 12.0f, 12.0f,
                                         4.0f, 1.0f, 1.0f, RGBAf{1, 0, 0, 0});
        pittore::compute::AutoTip tip;
        tip.silhouette = pittore::compute::TipSilhouette::Square;
        tip.hardness = 1.0f;
        pittore::compute::paint_tip_dab_host(square.data(), w, h, 12.0f,
                                             12.0f, 4.0f, tip, 1.0f,
                                             RGBAf{1, 0, 0, 0});
        // Corner 3px out diagonally: outside the circle, inside the square.
        CHECK_NEAR(round[15 * w + 15].a, 0.0f, 1e-6f);
        CHECK_NEAR(square[15 * w + 15].a, 1.0f, 1e-6f);
        // Centres agree.
        CHECK_NEAR(square[12 * w + 12].a, 1.0f, 1e-6f);
    }

    // 17. Spacing helper: % of diameter (15% reproduces the legacy step).
    {
        pittore::compute::AutoTip tip;
        tip.spacingPct = 15.0f;
        CHECK_NEAR(pittore::compute::tip_spacing(10.0f, tip), 3.0f, 1e-6f);
        tip.spacingPct = 100.0f;
        CHECK_NEAR(pittore::compute::tip_spacing(10.0f, tip), 20.0f, 1e-6f);
        tip.autoSpacing = true;
        tip.spacingPct = 15.0f;
        CHECK_NEAR(pittore::compute::tip_spacing(10.0f, tip), 0.5f, 1e-6f);
        tip.spacingPct = 100.0f;
        CHECK_NEAR(pittore::compute::tip_spacing(10.0f, tip), 20.0f, 1e-6f);
    }

    // 17c. Size-law auto spacing: stride follows the dab diameter
    // (linear sub-pixel, square-root above), ignoring the percentage.
    {
        using pittore::compute::auto_spacing_stride;
        CHECK_NEAR(auto_spacing_stride(64.0f, 0.12f), 0.96f, 1e-5f);
        CHECK_NEAR(auto_spacing_stride(0.5f, 0.12f), 0.06f, 1e-6f);
        CHECK_NEAR(auto_spacing_stride(-4.0f, 0.12f), 0.0f, 1e-6f);
        CHECK_NEAR(auto_spacing_stride(1.0f, 1.0f), 1.0f, 1e-6f);
        pittore::compute::AutoTip tip;
        tip.autoSpacing = true;
        tip.spacingAutoCoeff = 0.12f;
        tip.spacingPct = 10.0f;  // ignored under the size law
        CHECK_NEAR(pittore::compute::tip_spacing(32.0f, tip), 0.96f, 1e-5f);
        CHECK_NEAR(pittore::compute::tip_spacing(2.0f, tip), 0.5f, 1e-6f);
        tip.spacingAutoCoeff = 0.0f;  // back to legacy quadratic
        tip.spacingPct = 10.0f;
        CHECK_NEAR(pittore::compute::tip_spacing(32.0f, tip), 0.64f, 1e-5f);
        tip.autoSpacing = false;  // plain percentage
        CHECK_NEAR(pittore::compute::tip_spacing(32.0f, tip), 6.4f, 1e-5f);
        tip.spacingAutoCoeff = 99.0f;  // sanitize clamps to off
        tip.sanitize();
        CHECK_NEAR(tip.spacingAutoCoeff, 0.0f, 1e-6f);
        pittore::compute::StampTip stamp;
        stamp.w = 4;
        stamp.h = 4;
        stamp.alpha.assign(16, 1.0f);
        stamp.spacingAuto = true;
        stamp.spacingAutoCoeff = 0.12f;
        CHECK_NEAR(pittore::compute::stamp_spacing(32.0f, stamp), 0.96f,
                   1e-5f);
        stamp.spacingAuto = false;
        stamp.spacingPct = 10.0f;
        CHECK_NEAR(pittore::compute::stamp_spacing(32.0f, stamp), 6.4f,
                   1e-5f);
    }

    // 17b. New tip shapes: defaults off preserve legacy; each feature
    // visibly reshapes the dab when enabled.
    {
        constexpr std::uint32_t w = 24, h = 24;
        auto ink = [&](pittore::compute::AutoTip tip) {
            std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
            tip.sanitize();
            pittore::compute::paint_tip_dab_host(img.data(), w, h, 12.0f,
                                                 12.0f, 6.0f, tip, 1.0f,
                                                 RGBAf{1, 1, 1, 0});
            int n = 0;
            for (const auto& p : img)
                if (p.a > 0.5f) ++n;
            return n;
        };
        pittore::compute::AutoTip base;
        base.hardness = 0.0f;
        const int baseInk = ink(base);
        CHECK(baseInk > 20);
        // Spikes carve valleys out of a soft round tip.
        pittore::compute::AutoTip star = base;
        star.spikes = 5;
        star.ratio = 0.5f;
        CHECK(ink(star) < baseInk);
        CHECK(ink(star) > 0);
        // Gaussian falloff differs from linear at the same hardness.
        pittore::compute::AutoTip gauss = base;
        gauss.falloff = 1;
        int diff = 0;
        {
            std::vector<RGBAf> a(w * h, RGBAf{0, 0, 0, 0}),
                b(w * h, RGBAf{0, 0, 0, 0});
            base.sanitize();
            gauss.sanitize();
            pittore::compute::paint_tip_dab_host(a.data(), w, h, 12.0f, 12.0f,
                                                 6.0f, base, 1.0f,
                                                 RGBAf{1, 1, 1, 0});
            pittore::compute::paint_tip_dab_host(b.data(), w, h, 12.0f, 12.0f,
                                                 6.0f, gauss, 1.0f,
                                                 RGBAf{1, 1, 1, 0});
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i].a - b[i].a) > 1e-6f) ++diff;
        }
        CHECK(diff > 0);
        // Sharpness threshold cuts soft coverage into a hard core.
        pittore::compute::AutoTip sharp = base;
        sharp.sharpness = 0.8f;
        sharp.soften = 0.0f;
        CHECK(ink(sharp) < baseInk);
        CHECK(ink(sharp) > 0);
        // Soften reopens part of the cut edge.
        pittore::compute::AutoTip soft = sharp;
        soft.soften = 1.0f;
        {
            std::vector<RGBAf> a(w * h, RGBAf{0, 0, 0, 0}),
                b(w * h, RGBAf{0, 0, 0, 0});
            pittore::compute::paint_tip_dab_host(a.data(), w, h, 12.0f, 12.0f,
                                                 6.0f, sharp, 1.0f,
                                                 RGBAf{1, 1, 1, 0});
            pittore::compute::paint_tip_dab_host(b.data(), w, h, 12.0f, 12.0f,
                                                 6.0f, soft, 1.0f,
                                                 RGBAf{1, 1, 1, 0});
            int midA = 0, midB = 0;
            for (const auto& p : a)
                if (p.a > 0.05f && p.a < 0.95f) ++midA;
            for (const auto& p : b)
                if (p.a > 0.05f && p.a < 0.95f) ++midB;
            CHECK(midB >= midA);
        }
        // Aniso split slims one axis of a soft tip.
        pittore::compute::AutoTip aniso = base;
        aniso.fadeAniso = 1.0f;
        CHECK(ink(aniso) != baseInk);
        // Eraser twin follows the same shape (star erases less).
        {
            std::vector<RGBAf> a(w * h, RGBAf{1, 1, 1, 1}),
                b(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::erase_tip_dab_host(a.data(), w, h, 12.0f, 12.0f,
                                                 6.0f, base, 1.0f);
            pittore::compute::erase_tip_dab_host(b.data(), w, h, 12.0f, 12.0f,
                                                 6.0f, star, 1.0f);
            int keptA = 0, keptB = 0;
            for (const auto& p : a)
                if (p.a > 0.5f) ++keptA;
            for (const auto& p : b)
                if (p.a > 0.5f) ++keptB;
            CHECK(keptB > keptA);
        }
    }

    // 17c. Symmetry math: copy fan-out, position/offset mirroring,
    // direction and tip-angle sense.
    {
        namespace sym = pittore::ui::symmetry;
        CHECK(sym::copyCount(false, false) == 1);
        CHECK(sym::copyCount(true, false) == 2);
        CHECK(sym::copyCount(false, true) == 2);
        CHECK(sym::copyCount(true, true) == 4);
        // Positions mirror about the center; base copy is identity.
        CHECK_NEAR(sym::mirrorX(2.0, 10.0, false), 2.0, 1e-9);
        CHECK_NEAR(sym::mirrorX(2.0, 10.0, true), 18.0, 1e-9);
        CHECK_NEAR(sym::mirrorX(10.0, 10.0, true), 10.0, 1e-9);
        // Offsets negate per flipped axis (clone/heal source tracking).
        CHECK_NEAR(sym::mirrorComp(3.0, false), 3.0, 1e-9);
        CHECK_NEAR(sym::mirrorComp(3.0, true), -3.0, 1e-9);
        // Canvas-clockwise directions: vertical mirror flips heading.
        CHECK_NEAR(sym::mirrorDirDeg(0.0, true, false), 180.0, 1e-9);
        CHECK_NEAR(sym::mirrorDirDeg(90.0, true, false), 90.0, 1e-9);
        CHECK_NEAR(sym::mirrorDirDeg(90.0, false, true), -90.0, 1e-9);
        CHECK_NEAR(sym::mirrorDirDeg(0.0, true, true), 180.0, 1e-9);
        // Tip angles run counter-clockwise: one mirror negates, two keep.
        CHECK_NEAR(sym::mirrorTipAngle(30.0, false, false), 30.0, 1e-9);
        CHECK_NEAR(sym::mirrorTipAngle(30.0, true, false), -30.0, 1e-9);
        CHECK_NEAR(sym::mirrorTipAngle(30.0, false, true), -30.0, 1e-9);
        CHECK_NEAR(sym::mirrorTipAngle(30.0, true, true), 30.0, 1e-9);
    }

    // 17d. Sensor drives: empty is exactly 1.0; sensors normalize 0..1;
    // amounts are bipolar; shapers shape; names round-trip.
    {
        namespace sd = pittore::ui::sensordrive;
        sd::SensorState st;
        st.pressure = 0.5;
        st.hold = 1.0;
        st.tiltXDeg = 30.0;
        st.speed01 = 0.25;
        st.distPx = 250.0;
        st.timeSec = 2.5;
        st.fuzzyDab01 = 0.4;
        st.fuzzyStroke01 = 0.7;
        auto linear = [](const std::string&, double raw) { return raw; };
        auto doubler = [](const std::string&, double raw) { return raw * 2.0; };
        // Empty drive list: bit-exact 1.0, no sensor reads.
        CHECK(sd::driveFactor({}, "scatter", st, linear) == 1.0);
        CHECK(!sd::needsFuzzyDab({}, "scatter"));
        auto one = [&](const char* sensor, double amount,
                       const std::string& prop = "scatter") {
            sd::SensorDrive d;
            d.prop = prop;
            d.sensor = sd::sensorFromName(sensor);
            d.amount = amount;
            return sd::driveFactor({d}, prop, st, linear);
        };
        CHECK_NEAR(one("pressure", 100.0), 1.5, 1e-9);
        CHECK_NEAR(one("pressure", -50.0), 0.75, 1e-9);
        CHECK_NEAR(one("hold", 100.0), 2.0, 1e-9);
        CHECK_NEAR(one("tiltx", 100.0), 1.5, 1e-9);
        CHECK_NEAR(one("speed", 100.0), 1.25, 1e-9);
        CHECK_NEAR(one("distance", 100.0), 1.5, 1e-9);  // 250/500
        CHECK_NEAR(one("time", 100.0), 1.5, 1e-9);      // 2.5/5
        CHECK_NEAR(one("fuzzydab", 100.0), 1.4, 1e-9);
        CHECK_NEAR(one("fuzzystroke", 100.0), 1.7, 1e-9);
        // Wrong property: ignored.
        {
            sd::SensorDrive d;
            d.prop = "size";
            d.sensor = sd::Sensor::Pressure;
            d.amount = 100.0;
            CHECK(sd::driveFactor({d}, "scatter", st, linear) == 1.0);
        }
        // Zero amount: off, and never needs a random draw.
        {
            sd::SensorDrive d;
            d.prop = "scatter";
            d.sensor = sd::Sensor::FuzzyDab;
            d.amount = 0.0;
            CHECK(sd::driveFactor({d}, "scatter", st, linear) == 1.0);
            CHECK(!sd::needsFuzzyDab({d}, "scatter"));
        }
        {
            sd::SensorDrive d;
            d.prop = "scatter";
            d.sensor = sd::Sensor::FuzzyDab;
            d.amount = 25.0;
            CHECK(sd::needsFuzzyDab({d}, "scatter"));
        }
        // Shaper shapes the response (production passes the curve eval).
        {
            sd::SensorDrive d;
            d.prop = "scatter";
            d.sensor = sd::Sensor::Pressure;
            d.amount = 100.0;
            d.curve = "1|0,0;1,2";
            CHECK_NEAR(sd::driveFactor({d}, "scatter", st, doubler),
                       2.0, 1e-9);  // raw .5, doubled to 1.0
        }
        // Names round-trip; unknown names fall back to pressure.
        CHECK(std::string(sd::sensorName(sd::Sensor::Tangential)) ==
              "tangential");
        CHECK(sd::sensorFromName("lean") == sd::Sensor::Lean);
        CHECK(sd::sensorFromName("nope") == sd::Sensor::Pressure);
    }

    // 18. Tip eraser twin: ellipse removes along the major (x) axis only.
    {
        constexpr std::uint32_t w = 24, h = 24;
        std::vector<RGBAf> img(w * h);
        for (auto& p : img) p = RGBAf{1, 1, 1, 1};
        pittore::compute::AutoTip tip;
        tip.ratio = 0.5f;
        tip.hardness = 1.0f;
        pittore::compute::erase_tip_dab_host(img.data(), w, h, 12.0f, 12.0f,
                                             4.0f, tip, 1.0f);
        CHECK_NEAR(img[12 * w + 15].a, 0.0f, 1e-6f);  // major axis erased
        CHECK_NEAR(img[9 * w + 12].a, 1.0f, 1e-6f);   // minor axis kept
    }

    // -- Stamp tips: loaders (synthetic clean-room fixtures) ----------------
    namespace bl = pittore::compute::brushload;

    auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(std::uint8_t(x >> 24));
        v.push_back(std::uint8_t(x >> 16));
        v.push_back(std::uint8_t(x >> 8));
        v.push_back(std::uint8_t(x));
    };
    auto be16 = [](std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(std::uint8_t(x >> 8));
        v.push_back(std::uint8_t(x));
    };

    // 19. GBR v2 grayscale: dims, spacing, name, mask-convention coverage.
    {
        std::vector<std::uint8_t> f;
        const char name[] = {'a', 'b', '\0'};
        be32(f, 28 + 3);
        be32(f, 2);
        be32(f, 4);
        be32(f, 3);
        be32(f, 1);
        f.insert(f.end(), {'G', 'I', 'M', 'P'});
        be32(f, 30);
        f.insert(f.end(), std::begin(name), std::end(name));
        // 4x3 ramp: 0, 128, 255 across x, repeated rows.
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                f.push_back(std::uint8_t(x == 0 ? 0 : x == 1 ? 128 : 255));
        bl::LoadedTip t = bl::load_gbr(f.data(), f.size());
        CHECK(t.ok);
        CHECK(t.name == "ab");
        CHECK(t.tip.w == 4 && t.tip.h == 3);
        CHECK(!t.tip.color);
        CHECK_NEAR(t.tip.spacingPct, 30.0f, 1e-6f);
        CHECK_NEAR(t.tip.alpha[0], 0.0f, 1e-6f);
        CHECK_NEAR(t.tip.alpha[1], 128.0f / 255.0f, 1e-6f);
        CHECK_NEAR(t.tip.alpha[2], 1.0f, 1e-6f);
    }

    // 20. GBR v2 RGBA: color planes + alpha.
    {
        std::vector<std::uint8_t> f;
        be32(f, 28 + 2);
        be32(f, 2);
        be32(f, 2);
        be32(f, 1);
        be32(f, 4);
        f.insert(f.end(), {'G', 'I', 'M', 'P'});
        be32(f, 25);
        f.push_back('c');
        f.push_back('\0');
        // px0 red opaque, px1 green half.
        f.insert(f.end(), {255, 0, 0, 255, 0, 255, 0, 128});
        bl::LoadedTip t = bl::load_gbr(f.data(), f.size());
        CHECK(t.ok);
        CHECK(t.tip.color);
        CHECK_NEAR(t.tip.red[0], 1.0f, 1e-6f);
        CHECK_NEAR(t.tip.alpha[1], 128.0f / 255.0f, 1e-5f);
        CHECK_NEAR(t.tip.green[1], 1.0f, 1e-6f);
    }

    // 20b. GBR v1 (no magic/spacing): name + body parse, spacing defaults.
    {
        std::vector<std::uint8_t> f;
        be32(f, 20 + 3);
        be32(f, 1);
        be32(f, 2);
        be32(f, 2);
        be32(f, 1);
        f.push_back('v');
        f.push_back('\0');
        f.push_back('\0');  // 3 name bytes total
        f.insert(f.end(), {0, 255, 255, 0});
        bl::LoadedTip t = bl::load_gbr(f.data(), f.size());
        CHECK(t.ok);
        CHECK(t.name == "v");
        CHECK(t.tip.w == 2 && t.tip.h == 2);
        CHECK_NEAR(t.tip.spacingPct, 25.0f, 1e-6f);
        CHECK_NEAR(t.tip.alpha[0], 0.0f, 1e-6f);
        CHECK_NEAR(t.tip.alpha[1], 1.0f, 1e-6f);
    }

    // 21. GBR rejects: bad magic, truncation, bad dims, float depth.
    {        std::vector<std::uint8_t> f;
        be32(f, 28 + 2);
        be32(f, 2);
        be32(f, 2);
        be32(f, 1);
        be32(f, 1);
        f.insert(f.end(), {'X', 'X', 'X', 'X'});
        be32(f, 25);
        f.push_back('c');
        f.push_back('\0');
        f.insert(f.end(), {0, 0});
        CHECK(!bl::load_gbr(f.data(), f.size()).ok);  // magic
        CHECK(!bl::load_gbr(f.data(), 10).ok);        // truncated
        CHECK(!bl::load_gbr(nullptr, 0).ok);          // empty
        std::vector<std::uint8_t> g;
        be32(g, 28 + 2);
        be32(g, 2);
        be32(g, 0);
        be32(g, 0);
        be32(g, 1);
        g.insert(g.end(), {'G', 'I', 'M', 'P'});
        be32(g, 25);
        g.push_back('c');
        g.push_back('\0');
        CHECK(!bl::load_gbr(g.data(), g.size()).ok);  // zero dims
        std::vector<std::uint8_t> h;
        be32(h, 28 + 2);
        be32(h, 2);
        be32(h, 2);
        be32(h, 1);
        be32(h, 18);
        h.insert(h.end(), {'G', 'I', 'M', 'P'});
        be32(h, 25);
        h.push_back('c');
        h.push_back('\0');
        CHECK(!bl::load_gbr(h.data(), h.size()).ok);  // float16 depth
    }

    // 22. GIH: two cells, step + selection parsed.
    {
        auto tinyGbr = [&](std::uint8_t v) {
            std::vector<std::uint8_t> f;
            be32(f, 28 + 2);
            be32(f, 2);
            be32(f, 2);
            be32(f, 2);
            be32(f, 1);
            f.insert(f.end(), {'G', 'I', 'M', 'P'});
            be32(f, 25);
            f.push_back('x');
            f.push_back('\0');
            f.insert(f.end(), {v, v, v, v});
            return f;
        };
        const std::string hdr =
            "MyHose\n2 ncells:2 step:30 selection:random\n";
        std::vector<std::uint8_t> f(hdr.begin(), hdr.end());
        auto c0 = tinyGbr(0), c1 = tinyGbr(255);
        f.insert(f.end(), c0.begin(), c0.end());
        f.insert(f.end(), c1.begin(), c1.end());
        bl::LoadedHose hose = bl::load_gih(f.data(), f.size());
        CHECK(hose.ok);
        CHECK(hose.name == "MyHose");
        CHECK(hose.cells.size() == 2);
        CHECK(hose.step == 30);
        CHECK(hose.selection == "random");
        CHECK_NEAR(hose.cells[0].tip.alpha[0], 0.0f, 1e-6f);
        CHECK_NEAR(hose.cells[1].tip.alpha[0], 1.0f, 1e-6f);
        CHECK_NEAR(hose.cells[0].tip.spacingPct, 30.0f, 1e-6f);
        // Picker stays in range for both modes.
        for (std::size_t k = 0; k < 20; ++k) {
            CHECK(bl::hose_cell_index(hose, k, 12345) < 2);
        }
        bl::LoadedHose inc = hose;
        inc.selection = "incremental";
        CHECK(bl::hose_cell_index(inc, 0, 0) == 0);
        CHECK(bl::hose_cell_index(inc, 1, 0) == 1);
        CHECK(bl::hose_cell_index(inc, 2, 0) == 0);
        bl::LoadedHose weird = hose;
        weird.selection = "angular";
        CHECK(bl::hose_cell_index(weird, 3, 0, 0.0) == 0);
        CHECK(bl::hose_cell_index(weird, 3, 0, 3.141592653589793) == 1);
        bl::LoadedHose vel = hose;
        vel.selection = "velocity";
        CHECK(bl::hose_cell_index(vel, 0, 0, 0.0, 0.5, 0.0) == 0);
        CHECK(bl::hose_cell_index(vel, 0, 0, 0.0, 0.5, 1.0) == 1);
        bl::LoadedHose pres = hose;
        pres.selection = "pressure";
        CHECK(bl::hose_cell_index(pres, 0, 0, 0.0, 0.0) == 0);
        CHECK(bl::hose_cell_index(pres, 0, 0, 0.0, 1.0) == 1);
        bl::LoadedHose unknown = hose;
        unknown.selection = "diagonal-wobble";
        CHECK(bl::hose_cell_index(unknown, 3, 0) == 1);  // fallback
        CHECK(!bl::load_gih(f.data(), 4).ok);
    }

    // 23. ABR v1 uncompressed sampled tip.
    {
        std::vector<std::uint8_t> f;
        be16(f, 1);
        be16(f, 1);          // one brush
        be16(f, 2);          // sampled
        be32(f, 40);         // block size
        be32(f, 0);          // misc
        be16(f, 20);         // spacing
        f.push_back(0);      // antialias
        be16(f, 0);
        be16(f, 0);
        be16(f, 0);
        be16(f, 0);          // short bounds
        be32(f, 0);
        be32(f, 0);
        be32(f, 2);
        be32(f, 3);          // 3x2
        be16(f, 8);          // depth
        f.push_back(0);      // raw
        f.insert(f.end(), {0, 128, 255, 255, 64, 32});
        bl::LoadedAbr a = bl::load_abr(f.data(), f.size());
        CHECK(a.ok);
        CHECK(a.tips.size() == 1);
        CHECK(a.tips[0].tip.w == 3 && a.tips[0].tip.h == 2);
        CHECK_NEAR(a.tips[0].tip.spacingPct, 20.0f, 1e-6f);
        CHECK_NEAR(a.tips[0].tip.alpha[2], 1.0f, 1e-6f);
        CHECK_NEAR(a.tips[0].tip.alpha[4], 64.0f / 255.0f, 1e-6f);
    }

    // 24. ABR v1 RLE rows decode (repeat + literal runs).
    {
        std::vector<std::uint8_t> body;
        be32(body, 0);
        be16(body, 25);
        body.push_back(0);
        be16(body, 0);
        be16(body, 0);
        be16(body, 0);
        be16(body, 0);
        be32(body, 0);
        be32(body, 0);
        be32(body, 1);
        be32(body, 3);  // 3x1
        be16(body, 8);
        body.push_back(1);  // RLE
        be16(body, 2);      // row0: 2 compressed bytes
        body.push_back(std::uint8_t(-2));
        body.push_back(10);  // repeat 10 x3
        const std::uint32_t size = (std::uint32_t)body.size();
        std::vector<std::uint8_t> f;
        be16(f, 1);
        be16(f, 1);
        be16(f, 2);
        be32(f, size);
        f.insert(f.end(), body.begin(), body.end());
        bl::LoadedAbr a = bl::load_abr(f.data(), f.size());
        CHECK(a.ok);
        CHECK(a.tips.size() == 1);
        CHECK_NEAR(a.tips[0].tip.alpha[0], 10.0f / 255.0f, 1e-6f);
        CHECK_NEAR(a.tips[0].tip.alpha[2], 10.0f / 255.0f, 1e-6f);
    }

    // 24b. ABR v2 sampled tip carries its authored UCS2 name; the pixels
    // still decode at the right offsets (the old layout misaligned here).
    {
        std::vector<std::uint8_t> body;
        be32(body, 0);
        be16(body, 25);
        // UCS2-BE name "Hi!": u32 count + code units.
        be32(body, 3);
        body.insert(body.end(), {0, 'H', 0, 'i', 0, '!'});
        body.push_back(0);  // antialias
        be16(body, 0);
        be16(body, 0);
        be16(body, 0);
        be16(body, 0);  // short bounds
        be32(body, 0);
        be32(body, 0);
        be32(body, 1);
        be32(body, 2);  // 2x1
        be16(body, 8);
        body.push_back(0);  // raw
        body.insert(body.end(), {77, 88});
        const std::uint32_t size = (std::uint32_t)body.size();
        std::vector<std::uint8_t> f;
        be16(f, 2);  // version 2 => name present
        be16(f, 1);
        be16(f, 2);  // sampled
        be32(f, size);
        f.insert(f.end(), body.begin(), body.end());
        bl::LoadedAbr a = bl::load_abr(f.data(), f.size());
        CHECK(a.ok);
        CHECK(a.tips.size() == 1);
        CHECK(a.tips[0].name == "Hi!");
        CHECK(a.tips[0].nameAuthored);
        CHECK(a.tips[0].tip.w == 2 && a.tips[0].tip.h == 1);
        CHECK_NEAR(a.tips[0].tip.alpha[0], 77.0f / 255.0f, 1e-6f);
        CHECK_NEAR(a.tips[0].tip.alpha[1], 88.0f / 255.0f, 1e-6f);
    }

    // 25. Tagged-block sampled item (37-byte key + sub-version run).
    {
        std::vector<std::uint8_t> item;
        item.insert(item.end(), 37, 'k');  // key
        item.insert(item.end(), 10, 0);    // sub-version 1 run
        be32(item, 0);
        be32(item, 0);
        be32(item, 2);
        be32(item, 2);
        be16(item, 8);
        item.push_back(0);  // raw
        item.insert(item.end(), {1, 2, 3, 4});
        std::vector<std::uint8_t> samp;
        be32(samp, (std::uint32_t)item.size());
        samp.insert(samp.end(), item.begin(), item.end());
        std::vector<std::uint8_t> f;
        be16(f, 6);
        be16(f, 1);  // sub-version 1
        f.insert(f.end(), {'8', 'B', 'I', 'M'});
        f.insert(f.end(), {'s', 'a', 'm', 'p'});
        be32(f, (std::uint32_t)samp.size());
        f.insert(f.end(), samp.begin(), samp.end());
        while (f.size() % 4 != 0) f.push_back(0);
        bl::LoadedAbr a = bl::load_abr(f.data(), f.size());
        CHECK(a.ok);
        CHECK(a.tips.size() == 1);
        CHECK(a.tips[0].tip.w == 2 && a.tips[0].tip.h == 2);
        CHECK_NEAR(a.tips[0].tip.alpha[3], 4.0f / 255.0f, 1e-6f);
        // Unknown item variants skip by length instead of failing.
        std::vector<std::uint8_t> odd = f;
        // Corrupt the rectangle (bottom byte) so validation fails: the
        // file still parses, with zero tips.
        odd[77] = 200;
        bl::LoadedAbr b = bl::load_abr(odd.data(), odd.size());
        CHECK(b.ok);
        CHECK(b.tips.empty());
        // Unknown versions + garbage fail cleanly.
        std::vector<std::uint8_t> bad{0, 99, 0, 0};
        CHECK(!bl::load_abr(bad.data(), bad.size()).ok);
        CHECK(!bl::load_abr(nullptr, 0).ok);
    }

    // -- Stamp dab kernel ----------------------------------------------------
    // 26. Alpha-mask: solid tip tints the foreground, full coverage core.
    {
        constexpr std::uint32_t w = 8, h = 8;
        std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
        pittore::compute::StampTip tip;
        tip.w = 4;
        tip.h = 4;
        tip.alpha.assign(16, 1.0f);
        pittore::compute::stamp_dab_host(img.data(), w, h, 4.0f, 4.0f, 2.0f,
                                         tip,
                                         pittore::compute::StampMode::AlphaMask,
                                         1.0f, RGBAf{0, 0, 1, 0}, 0.0f);
        CHECK_NEAR(img[4 * w + 4].b, 1.0f, 1e-5f);  // blue over white = blue
        CHECK_NEAR(img[4 * w + 4].r, 0.0f, 1e-5f);
        CHECK_NEAR(img[4 * w + 4].a, 1.0f, 1e-5f);
        CHECK_NEAR(img[0].r, 1.0f, 1e-6f);  // corner untouched
    }

    // 27. Color-image: the tip's own RGBA lands as-is.
    {
        constexpr std::uint32_t w = 8, h = 8;
        std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
        pittore::compute::StampTip tip;
        tip.w = 4;
        tip.h = 4;
        tip.color = true;
        tip.alpha.assign(16, 1.0f);
        tip.red.assign(16, 1.0f);
        tip.green.assign(16, 0.0f);
        tip.blue.assign(16, 0.0f);
        pittore::compute::stamp_dab_host(img.data(), w, h, 4.0f, 4.0f, 2.0f,
                                         tip,
                                         pittore::compute::StampMode::ColorImage,
                                         1.0f, RGBAf{0, 0, 1, 0}, 0.0f);
        CHECK_NEAR(img[4 * w + 4].r, 1.0f, 1e-5f);  // red, not foreground blue
        CHECK_NEAR(img[4 * w + 4].b, 0.0f, 1e-5f);
    }

    // 27b. LightnessMap recolors FG around neutral; GradientMap blends
    // BG->FG by tip lightness. Neutral tip reproduces the foreground.
    {
        constexpr std::uint32_t w = 8, h = 8;
        // 4x4 color tip, uniform 2x2 quadrants: TL black, TR mid grey,
        // BL light grey, BR white. Alpha solid so coverage is full.
        auto quadTip = [] {
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.color = true;
            tip.alpha.assign(16, 1.0f);
            tip.red.assign(16, 0.0f);
            tip.green.assign(16, 0.0f);
            tip.blue.assign(16, 0.0f);
            auto quad = [&](int qx, int qy, float v) {
                for (int y = 0; y < 2; ++y)
                    for (int x = 0; x < 2; ++x) {
                        const std::size_t i =
                            std::size_t(qy * 2 + y) * 4 + (qx * 2 + x);
                        tip.red[i] = tip.green[i] = tip.blue[i] = v;
                    }
            };
            quad(0, 0, 0.0f);
            quad(1, 0, 0.5f);
            quad(0, 1, 0.75f);
            quad(1, 1, 1.0f);
            return tip;
        };
        const RGBAf fg{0.2f, 0.4f, 0.8f, 1.0f};
        const RGBAf bg{1.0f, 0.0f, 0.0f, 1.0f};
        // Radius 4 at (4,4): quadrant interiors land on pixels
        // (2,2)=black, (6,2)=mid, (6,6)=white.
        auto dabPx = [&](const pittore::compute::StampTip& tip,
                         pittore::compute::StampMode mode, int x, int y,
                         const RGBAf* bgPtr = nullptr) {
            std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 4.0f, tip, mode, 1.0f, fg,
                0.0f, nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr,
                bgPtr);
            return img[std::size_t(y) * w + x];
        };
        // LightnessMap: black tip darkens FG, white tip brightens it.
        const RGBAf dark = dabPx(quadTip(),
                                 pittore::compute::StampMode::LightnessMap,
                                 2, 2);
        const RGBAf light = dabPx(quadTip(),
                                  pittore::compute::StampMode::LightnessMap,
                                  6, 6);
        CHECK(dark.b < fg.b);
        CHECK(dark.r < fg.r);
        CHECK(light.b >= fg.b);
        CHECK(light.r >= fg.r);
        // Mid-grey tip (~0.5 luma) reproduces FG near-exactly.
        const RGBAf mid = dabPx(quadTip(),
                                pittore::compute::StampMode::LightnessMap,
                                6, 2);
        CHECK_NEAR(mid.r, fg.r, 0.06f);
        CHECK_NEAR(mid.g, fg.g, 0.06f);
        CHECK_NEAR(mid.b, fg.b, 0.06f);
        // GradientMap: black tip -> BG, white tip -> FG.
        const RGBAf g0 = dabPx(quadTip(),
                               pittore::compute::StampMode::GradientMap, 2,
                               2, &bg);
        const RGBAf g1 = dabPx(quadTip(),
                               pittore::compute::StampMode::GradientMap, 6,
                               6, &bg);
        CHECK_NEAR(g0.r, bg.r, 1e-5f);
        CHECK_NEAR(g0.g, bg.g, 1e-5f);
        CHECK_NEAR(g1.r, fg.r, 1e-5f);
        CHECK_NEAR(g1.b, fg.b, 1e-5f);
        // Grayscale tip: coverage-derived lightness still varies the ink.
        pittore::compute::StampTip gray;
        gray.w = 4;
        gray.h = 4;
        gray.alpha.assign(16, 1.0f);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) gray.alpha[y * 4 + x] = 1.0f;
        const RGBAf gd = dabPx(gray,
                               pittore::compute::StampMode::LightnessMap,
                               2, 2);
        CHECK(gd.b < fg.b);  // full-coverage dark end darkens
        // Legacy modes bit-exact: extra defaulted args change nothing.
        {
            std::vector<RGBAf> a(w * h, RGBAf{1, 1, 1, 1}),
                b(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.alpha.assign(16, 1.0f);
            pittore::compute::stamp_dab_host(
                a.data(), w, h, 4.0f, 4.0f, 2.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f,
                RGBAf{0, 0, 1, 0}, 0.0f);
            pittore::compute::stamp_dab_host(
                b.data(), w, h, 4.0f, 4.0f, 2.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f,
                RGBAf{0, 0, 1, 0}, 0.0f, nullptr, nullptr, nullptr,
                nullptr, 0, 1, nullptr, nullptr);
            for (std::size_t i = 0; i < a.size(); ++i) {
                CHECK_NEAR(a[i].r, b[i].r, 1e-7f);
                CHECK_NEAR(a[i].a, b[i].a, 1e-7f);
            }
        }
    }

    // 27d. Pigment-mix dabs: mean-family ops glaze instead of covering.
    // Mid-grey ink over white separates every mode at full and half alpha.
    {
        using pittore::compute::dabmix::DabMixOp;
        constexpr std::uint32_t w = 8, h = 8;
        auto dab = [&](DabMixOp mix, float opacity) {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.alpha.assign(16, 1.0f);
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 2.0f, tip,
                pittore::compute::StampMode::AlphaMask, opacity,
                RGBAf{0.5f, 0.5f, 0.5f, 1}, 0.0f, nullptr, nullptr, nullptr,
                nullptr, 0, 1, nullptr, nullptr, nullptr, 0.0f, mix);
            return img[4 * w + 4];
        };
        const RGBAf n = dab(DabMixOp::Normal, 1.0f);
        CHECK_NEAR(n.r, 0.5f, 1e-5f);
        const RGBAf g = dab(DabMixOp::GeometricMean, 1.0f);
        CHECK_NEAR(g.r, 0.70710678f, 1e-5f);
        const RGBAf p = dab(DabMixOp::Parallel, 1.0f);
        CHECK_NEAR(p.r, 0.6666667f, 1e-5f);
        const RGBAf al = dab(DabMixOp::Allanon, 1.0f);
        CHECK_NEAR(al.r, 0.75f, 1e-5f);
        CHECK_NEAR(dab(DabMixOp::Normal, 0.5f).r, 0.75f, 1e-5f);
        CHECK_NEAR(dab(DabMixOp::GeometricMean, 0.5f).r, 0.8535534f, 1e-5f);
        CHECK_NEAR(dab(DabMixOp::Parallel, 0.5f).r, 0.8333333f, 1e-5f);
        CHECK_NEAR(dab(DabMixOp::Allanon, 0.5f).r, 0.875f, 1e-5f);
        // Id mapping round-trips; unknown ids stay Normal.
        CHECK(pittore::compute::dabmix::mix_from_id("geometric_mean") ==
              DabMixOp::GeometricMean);
        CHECK(pittore::compute::dabmix::mix_from_id("parallel") ==
              DabMixOp::Parallel);
        CHECK(pittore::compute::dabmix::mix_from_id("allanon") ==
              DabMixOp::Allanon);
        CHECK(pittore::compute::dabmix::mix_from_id("bogus") ==
              DabMixOp::Normal);
        CHECK(pittore::compute::dabmix::mix_from_id(nullptr) ==
              DabMixOp::Normal);
    }

    // 27e. Pigment-mix on auto dabs: round + tip paths agree with the
    // stamp path per mode, and defaulted args stay bit-exact Normal.
    {
        using pittore::compute::dabmix::DabMixOp;
        constexpr std::uint32_t w = 8, h = 8;
        auto dabRound = [](DabMixOp mix, float opacity) {
            constexpr std::uint32_t W = 8, H = 8;
            std::vector<RGBAf> img(W * H, RGBAf{1, 1, 1, 1});
            pittore::compute::paint_dab_host(
                img.data(), W, H, 4.0f, 4.0f, 2.0f, 1.0f, opacity,
                RGBAf{0.5f, 0.5f, 0.5f, 1}, nullptr, nullptr, nullptr,
                nullptr, nullptr, mix);
            return img[4 * W + 4];
        };
        auto dabTip = [](DabMixOp mix, float opacity) {
            constexpr std::uint32_t W = 8, H = 8;
            std::vector<RGBAf> img(W * H, RGBAf{1, 1, 1, 1});
            pittore::compute::AutoTip tip;  // round, ratio 1, angle 0
            tip.hardness = 1.0f;
            pittore::compute::paint_tip_dab_host(
                img.data(), W, H, 4.0f, 4.0f, 2.0f, tip, opacity,
                RGBAf{0.5f, 0.5f, 0.5f, 1}, nullptr, nullptr, nullptr,
                nullptr, nullptr, mix);
            return img[4 * W + 4];
        };
        using DabFn = RGBAf (*)(DabMixOp, float);
        const DabFn fns[] = {dabRound, dabTip};
        for (DabFn dab : fns) {
            CHECK_NEAR(dab(DabMixOp::Normal, 1.0f).r, 0.5f, 1e-5f);
            CHECK_NEAR(dab(DabMixOp::GeometricMean, 1.0f).r, 0.70710678f,
                       1e-5f);
            CHECK_NEAR(dab(DabMixOp::Parallel, 1.0f).r, 0.6666667f, 1e-5f);
            CHECK_NEAR(dab(DabMixOp::Allanon, 1.0f).r, 0.75f, 1e-5f);
            CHECK_NEAR(dab(DabMixOp::Normal, 0.5f).r, 0.75f, 1e-5f);
            CHECK_NEAR(dab(DabMixOp::GeometricMean, 0.5f).r, 0.8535534f,
                       1e-5f);
        }
        // Defaulted tail args reproduce explicit Normal bit-exactly.
        {
            std::vector<RGBAf> a(w * h, RGBAf{1, 1, 1, 1}),
                b(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            pittore::compute::paint_tip_dab_host(
                a.data(), w, h, 4.0f, 4.0f, 2.0f, tip, 1.0f,
                RGBAf{0, 0, 1, 0});
            pittore::compute::paint_tip_dab_host(
                b.data(), w, h, 4.0f, 4.0f, 2.0f, tip, 1.0f,
                RGBAf{0, 0, 1, 0}, nullptr, nullptr, nullptr, nullptr,
                nullptr, DabMixOp::Normal);
            for (std::size_t i = 0; i < a.size(); ++i) {
                CHECK_NEAR(a[i].r, b[i].r, 1e-7f);
                CHECK_NEAR(a[i].a, b[i].a, 1e-7f);
            }
        }
    }

    // 27c. Relief: lightness dabs deposit scaled by thickness, pile up to
    // a max, erase carves, smudge transports, null planes stay legacy.
    {
        constexpr std::uint32_t w = 8, h = 8;
        auto quadTip = [] {
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.color = true;
            tip.alpha.assign(16, 1.0f);
            tip.red.assign(16, 0.0f);
            tip.green.assign(16, 0.0f);
            tip.blue.assign(16, 0.0f);
            // BR quadrant white, rest black.
            for (int y = 2; y < 4; ++y)
                for (int x = 2; x < 4; ++x) {
                    tip.red[y * 4 + x] = tip.green[y * 4 + x] =
                        tip.blue[y * 4 + x] = 1.0f;
                }
            return tip;
        };
        const RGBAf fg{0.5f, 0.5f, 0.5f, 1.0f};
        // Deposit scales with thickness; zero thickness leaves it flat.
        {
            std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
            std::vector<float> height(w * h, 0.0f);
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 4.0f, quadTip(),
                pittore::compute::StampMode::LightnessMap, 1.0f, fg, 0.0f,
                nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr, nullptr,
                height.data(), 1.0f);
            CHECK(height[6 * w + 6] > 0.5f);  // white quadrant piles up
            CHECK_NEAR(height[2 * w + 2], 0.0f, 1e-6f);  // black adds nothing
            CHECK_NEAR(height[0], 0.0f, 1e-6f);          // outside untouched
            std::vector<float> flat(w * h, 0.0f);
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 4.0f, quadTip(),
                pittore::compute::StampMode::LightnessMap, 1.0f, fg, 0.0f,
                nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr, nullptr,
                flat.data(), 0.0f);
            for (float v : flat) CHECK_NEAR(v, 0.0f, 1e-7f);
        }
        // Alpha-mask mode never deposits, even with thickness set.
        {
            std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
            std::vector<float> height(w * h, 0.0f);
            pittore::compute::StampTip tip = quadTip();
            tip.color = false;
            tip.red.clear();
            tip.green.clear();
            tip.blue.clear();
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 4.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f, fg, 0.0f,
                nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr, nullptr,
                height.data(), 1.0f);
            for (float v : height) CHECK_NEAR(v, 0.0f, 1e-7f);
            CHECK(img[4 * w + 4].a > 0.0f);  // paint still landed
        }
        // Relief never exceeds the deposit target (max-buildup).
        {
            std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
            std::vector<float> height(w * h, 0.0f);
            for (int k = 0; k < 4; ++k)
                pittore::compute::stamp_dab_host(
                    img.data(), w, h, 4.0f, 4.0f, 4.0f, quadTip(),
                    pittore::compute::StampMode::LightnessMap, 1.0f, fg,
                    0.0f, nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr,
                    nullptr, height.data(), 0.5f);
            for (float v : height) CHECK(v <= 0.5f + 1e-6f);
            CHECK(height[6 * w + 6] > 0.4f);
        }
        // Erase carves relief with the same mask.
        {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            std::vector<float> height(w * h, 1.0f);
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.alpha.assign(16, 1.0f);
            pittore::compute::stamp_erase_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 4.0f, tip, 1.0f, 0.0f,
                nullptr, nullptr, nullptr, nullptr, 0, 1, height.data());
            CHECK_NEAR(height[4 * w + 4], 0.0f, 1e-5f);
            CHECK_NEAR(height[0], 1.0f, 1e-6f);
        }
        // Smudge transports relief with its travelling patch; an unbound
        // plane leaves pixels untouched.
        {
            auto bump = [&] {
                std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
                std::vector<float> height(w * h, 0.0f);
                for (int y = 2; y < 6; ++y)
                    for (int x = 2; x < 4; ++x) height[y * w + x] = 1.0f;
                return std::make_pair(img, height);
            };
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            // Unbound vs neutral ctl: identical pixels when no relief rides.
            {
                auto [a, ha] = bump();
                auto [b, hb] = bump();
                pittore::compute::SmudgeCarry ca, cb;
                pittore::compute::smudge_tip_dab_host(a.data(), w, h, 5.0f,
                                                       4.0f, 3.0f, tip, 1.0f,
                                                       1.0f, ca);
                pittore::compute::SmudgeCtl ctl;
                pittore::compute::smudge_tip_dab_host(
                    b.data(), w, h, 5.0f, 4.0f, 3.0f, tip, 1.0f, 1.0f, cb,
                    nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                    pittore::compute::BlendMode::Normal, &ctl, nullptr);
                for (std::size_t i = 0; i < a.size(); ++i)
                    CHECK_NEAR(a[i].r, b[i].r, 1e-7f);
            }
            // Bound planes: a two-dab step rightward drags the bump with it
            // and loads the travelling relief patch.
            {
                auto [img, height] = bump();
                pittore::compute::SmudgeCarry carry;
                pittore::compute::SmudgeCtl ctl;
                pittore::compute::smudge_tip_dab_host(
                    img.data(), w, h, 4.0f, 4.0f, 3.0f, tip, 0.5f, 1.0f,
                    carry, nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                    pittore::compute::BlendMode::Normal, &ctl, height.data());
                pittore::compute::smudge_tip_dab_host(
                    img.data(), w, h, 6.0f, 4.0f, 3.0f, tip, 0.5f, 1.0f,
                    carry, nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                    pittore::compute::BlendMode::Normal, &ctl, height.data());
                CHECK(carry.hasHeight);
                bool loaded = false;
                for (float v : carry.height)
                    if (v > 0.0f) loaded = true;
                CHECK(loaded);  // picked up relief
                CHECK(height[4 * w + 5] < 1.0f);  // smeared down toward patch
                CHECK(height[4 * w + 5] > 0.0f);  // but not erased
            }
        }
    }

    // 28. Rotation mirrors an asymmetric tip; eraser twin removes coverage.
    {
        constexpr std::uint32_t w = 12, h = 12;
        auto dab = [&](float angle) {
            std::vector<RGBAf> img(w * h, RGBAf{0, 0, 0, 0});
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.alpha.assign(16, 0.0f);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 2; ++x) tip.alpha[y * 4 + x] = 1.0f;
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 6.0f, 6.0f, 2.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f,
                RGBAf{1, 1, 1, 0}, angle);
            return img;
        };
        auto a0 = dab(0.0f), a180 = dab(180.0f);
        // Left-heavy tip at 0° paints the left side; at 180° the right side.
        CHECK(a0[6 * w + 4].a > 0.9f);
        CHECK(a0[6 * w + 7].a < 0.1f);
        CHECK(a180[6 * w + 7].a > 0.9f);
        CHECK(a180[6 * w + 4].a < 0.1f);

        std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
        pittore::compute::StampTip tip;
        tip.w = 4;
        tip.h = 4;
        tip.alpha.assign(16, 1.0f);
        pittore::compute::stamp_erase_dab_host(img.data(), w, h, 6.0f, 6.0f,
                                               2.0f, tip, 1.0f, 0.0f);
        CHECK_NEAR(img[6 * w + 6].a, 0.0f, 1e-5f);
        CHECK_NEAR(img[0].a, 1.0f, 1e-6f);
        CHECK(pittore::compute::stamp_spacing(10.0f, tip) > 0.5f);
    }

    // -- Paper grain ---------------------------------------------------------
    // 29. texture_mult: strength mixes toward the grain, tiling wraps,
    // levels shape it, invert flips it.
    {
        using pittore::compute::PatternTex;
        // 2x2 checker: (0,0)=0, (1,0)=1, (0,1)=1, (1,1)=0.
        const float g[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        PatternTex t;
        t.gray = g;
        t.w = 2;
        t.h = 2;
        t.strength = 1.0f;
        CHECK_NEAR(pittore::compute::texture_mult(nullptr, 3, 4), 1.0f, 1e-6f);
        CHECK_NEAR(pittore::compute::texture_mult(&t, 0, 0), 0.0f, 1e-6f);
        CHECK_NEAR(pittore::compute::texture_mult(&t, 1, 0), 1.0f, 1e-6f);
        CHECK_NEAR(pittore::compute::texture_mult(&t, 2, 0), 0.0f, 1e-6f);  // wraps
        CHECK_NEAR(pittore::compute::texture_mult(&t, 0, 2), 0.0f, 1e-6f);
        t.strength = 0.5f;
        CHECK_NEAR(pittore::compute::texture_mult(&t, 0, 0), 0.5f, 1e-6f);
        CHECK_NEAR(pittore::compute::texture_mult(&t, 1, 0), 1.0f, 1e-6f);
        t.strength = 1.0f;
        t.invert = true;
        CHECK_NEAR(pittore::compute::texture_mult(&t, 0, 0), 1.0f, 1e-6f);
        t.invert = false;
        t.offsetX = 1.0f;  // origin shift moves the tile phase
        CHECK_NEAR(pittore::compute::texture_mult(&t, 0, 0), 1.0f, 1e-6f);
        t.offsetX = 0.0f;
        t.scale = 2.0f;  // 2 target px per pattern px
        CHECK_NEAR(pittore::compute::texture_mult(&t, 1, 0), 0.0f, 1e-6f);
        CHECK_NEAR(pittore::compute::texture_mult(&t, 2, 0), 1.0f, 1e-6f);
        t.scale = 1.0f;
        t.contrast = 0.0f;  // flat contrast collapses to neutral+brightness
        CHECK_NEAR(pittore::compute::texture_mult(&t, 0, 0), 0.5f, 1e-6f);
    }

    // 29b. New grain modes 3..6: full-strength shapes, soft vs hard split,
    // and backward compat for 0..2 (soft ignored there).
    {
        using pittore::compute::PatternTex;
        auto at = [](int mode, float grain, float strength, bool soft) {
            const float g[1] = {grain};
            PatternTex t;
            t.gray = g;
            t.w = 1;
            t.h = 1;
            t.strength = strength;
            t.mode = mode;
            t.soft = soft;
            return pittore::compute::texture_mult(&t, 0, 0);
        };
        // Full strength converges soft/hard and maps 0->0, 1->1.
        for (int m = 3; m <= 6; ++m) {
            CHECK_NEAR(at(m, 0.0f, 1.0f, false), 0.0f, 1e-6f);
            CHECK_NEAR(at(m, 0.0f, 1.0f, true), 0.0f, 1e-6f);
            CHECK_NEAR(at(m, 1.0f, 1.0f, false), 1.0f, 1e-6f);
            CHECK_NEAR(at(m, 1.0f, 1.0f, true), 1.0f, 1e-6f);
            CHECK_NEAR(at(m, 0.5f, 1.0f, false), at(m, 0.5f, 1.0f, true),
                       1e-6f);
        }
        // Mid grain shapes differ per mode at full strength.
        const float over = at(3, 0.25f, 1.0f, true);   // smoothstep ~0.156
        const float dodge = at(4, 0.25f, 1.0f, true);  // screen ~0.438
        const float burn = at(5, 0.25f, 1.0f, true);   // gamma 0.0625
        const float height = at(6, 0.25f, 1.0f, true);  // boosted 0.375
        CHECK_NEAR(over, 0.15625f, 1e-5f);
        CHECK_NEAR(dodge, 0.4375f, 1e-5f);
        CHECK_NEAR(burn, 0.0625f, 1e-5f);
        CHECK_NEAR(height, 0.375f, 1e-5f);
        CHECK(dodge > over && over > burn);
        CHECK(height > over);
        // Partial strength: soft fades to untextured, hard rubs out.
        CHECK_NEAR(at(3, 0.25f, 0.2f, true), 1.0f - 0.2f + 0.2f * over,
                   1e-6f);
        CHECK_NEAR(at(3, 0.25f, 0.2f, false),
                   std::clamp(over - 0.8f, 0.0f, 1.0f), 1e-6f);
        // Height allows fuller coverage than multiply at same strength.
        CHECK(at(6, 0.8f, 1.0f, true) >= at(0, 0.8f, 1.0f, true));
        // Legacy modes ignore soft (bit-exact).
        CHECK_NEAR(at(0, 0.25f, 0.5f, false), at(0, 0.25f, 0.5f, true),
                   1e-6f);
        CHECK_NEAR(at(1, 0.25f, 0.5f, false), at(1, 0.25f, 0.5f, true),
                   1e-6f);
        CHECK_NEAR(at(2, 0.25f, 0.5f, false), at(2, 0.25f, 0.5f, true),
                   1e-6f);
    }

    // 30. Grain breaks the dab up: a half-hole pattern roughly halves ink.
    {
        constexpr std::uint32_t w = 16, h = 16;
        auto stroke = [](const pittore::compute::PatternTex* tex) {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            pittore::compute::paint_tip_dab_host(
                img.data(), w, h, 8.0f, 8.0f, 5.0f, tip, 1.0f,
                RGBAf{0, 0, 0, 0}, nullptr, tex);
            int ink = 0;
            for (const auto& p : img)
                if (p.r < 0.5f) ++ink;
            return ink;
        };
        const int full = stroke(nullptr);
        CHECK(full > 50);
        const float g[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        pittore::compute::PatternTex t;
        t.gray = g;
        t.w = 2;
        t.h = 2;
        t.strength = 1.0f;
        const int grainy = stroke(&t);
        CHECK(grainy > 0);
        CHECK(grainy < full * 0.75);
    }

    // -- Smudge ----------------------------------------------------------------
    // 31. Smudge drags paint: finger-loaded red reddens the white side and
    // the travelling patch picks up canvas toward non-red.
    {
        constexpr std::uint32_t w = 24, h = 16;
        std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < 8; ++x)
                img[y * w + x] = RGBAf{0, 0, 0, 1};
        pittore::compute::AutoTip tip;
        tip.hardness = 1.0f;
        pittore::compute::SmudgeCarry carry;
        pittore::compute::SmudgeCtl ctl;
        ctl.fingerPaint = true;  // start from the foreground
        ctl.fg = RGBAf{1, 0, 0, 1};
        pittore::compute::smudge_tip_dab_host(img.data(), w, h, 10.0f, 8.0f,
                                              5.0f, tip, 1.0f, 1.0f, carry,
                                              nullptr, nullptr, nullptr,
                                              nullptr, 0, nullptr,
                                              pittore::compute::BlendMode::Normal,
                                              &ctl);
        // White side near the dab picks up red from the carried patch.
        CHECK(img[8 * w + 13].r > 0.7f);
        CHECK(img[8 * w + 13].g < 0.5f);
        // Carried patch drifted toward the canvas (picked up non-red).
        bool drifted = false;
        for (const auto& c : carry.color)
            if (c.g > 0.0f || c.b > 0.0f) drifted = true;
        CHECK(drifted);
        // Far field untouched.
        CHECK_NEAR(img[8 * w + 23].r, 1.0f, 1e-6f);
        CHECK_NEAR(img[8 * w + 0].r, 0.0f, 1e-6f);
    }

    // 32. Stamp smudge + rate: zero rate moves nothing, full rate smears.
    {
        constexpr std::uint32_t w = 24, h = 16;
        auto run = [&](float rate) {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < 8; ++x)
                    img[y * w + x] = RGBAf{0, 0, 0, 1};
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.alpha.assign(16, 1.0f);
            pittore::compute::SmudgeCarry carry;
            pittore::compute::SmudgeCtl ctl;
            ctl.fingerPaint = true;
            ctl.fg = RGBAf{0, 0, 1, 1};
            pittore::compute::smudge_stamp_dab_host(img.data(), w, h, 10.0f,
                                                    8.0f, 5.0f, tip, rate,
                                                    1.0f, 0.0f, carry,
                                                    nullptr, nullptr, nullptr,
                                                    nullptr, 0, 1, nullptr,
                                                    pittore::compute::BlendMode::Normal,
                                                    &ctl);
            return img[8 * w + 13].r;
        };
        CHECK_NEAR(run(0.0f), 1.0f, 1e-6f);  // untouched white
        CHECK(run(1.0f) < 0.5f);             // smeared toward blue paint
    }

    // 32b. Smudge controls: null ctl matches neutral ctl; color rate
    // reloads foreground; smear trails behind the stroke direction.
    {
        constexpr std::uint32_t w = 24, h = 16;
        auto edgeImg = [&] {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < 8; ++x)
                    img[y * w + x] = RGBAf{0, 0, 0, 1};
            return img;
        };
        // Null ctl == neutral ctl (exercises the default arg).
        {
            auto a = edgeImg(), b = edgeImg();
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            pittore::compute::SmudgeCarry ca, cb;
            pittore::compute::smudge_tip_dab_host(a.data(), w, h, 10.0f,
                                                   8.0f, 5.0f, tip, 1.0f,
                                                   1.0f, ca);
            pittore::compute::SmudgeCtl ctl;  // all neutral
            pittore::compute::smudge_tip_dab_host(b.data(), w, h, 10.0f,
                                                   8.0f, 5.0f, tip, 1.0f,
                                                   1.0f, cb, nullptr, nullptr,
                                                   nullptr, nullptr, 0,
                                                   nullptr,
                                                   pittore::compute::BlendMode::Normal,
                                                   &ctl);
            for (std::size_t i = 0; i < a.size(); ++i) {
                CHECK_NEAR(a[i].r, b[i].r, 1e-7f);
                CHECK_NEAR(a[i].g, b[i].g, 1e-7f);
            }
            CHECK(ca.side == cb.side);
            CHECK_NEAR(ca.color.front().r, cb.color.front().r, 1e-7f);
        }
        // Color rate 1 reloads carried paint to FG: white canvas turns blue.
        {
            auto img = edgeImg();
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            pittore::compute::SmudgeCarry carry;
            pittore::compute::SmudgeCtl ctl;
            ctl.colorRate = 1.0f;
            ctl.fg = RGBAf{0, 0, 1, 1};
            pittore::compute::smudge_tip_dab_host(img.data(), w, h, 14.0f,
                                                   8.0f, 4.0f, tip, 1.0f,
                                                   1.0f, carry, nullptr,
                                                   nullptr, nullptr, nullptr,
                                                   0, nullptr,
                                                   pittore::compute::
                                                       BlendMode::Normal,
                                                   &ctl);
            CHECK(img[8 * w + 14].b > 0.9f);  // blue laid on white
            CHECK(img[8 * w + 14].r < 0.5f);
            bool blue = false;
            for (const auto& c : carry.color)
                if (c.b > 0.99f) blue = true;
            CHECK(blue);  // carried patch reloaded
        }
        // Smear trails: pickup from behind drags the left color rightward
        // harder than dulling does (two-dab stroke: prime, then step right).
        {
            auto run = [&](bool smear) {
                auto img = edgeImg();
                pittore::compute::AutoTip tip;
                tip.hardness = 1.0f;
                pittore::compute::SmudgeCarry carry;
                pittore::compute::SmudgeCtl ctl;
                if (smear) {
                    ctl.mode = pittore::compute::SmudgeMode::Smear;
                    ctl.trailX = 4.0f;  // look 4px behind (+x stroke)
                }
                pittore::compute::smudge_tip_dab_host(
                    img.data(), w, h, 10.0f, 8.0f, 3.0f, tip, 1.0f, 1.0f,
                    carry, nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                    pittore::compute::BlendMode::Normal, &ctl);
                pittore::compute::smudge_tip_dab_host(
                    img.data(), w, h, 12.0f, 8.0f, 3.0f, tip, 1.0f, 1.0f,
                    carry, nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                    pittore::compute::BlendMode::Normal, &ctl);
                return img[8 * w + 13];
            };
            const RGBAf dull = run(false), smear = run(true);
            // Smear picked up the black block behind -> darker result.
            CHECK(smear.r < dull.r);
        }
    }

    // 33. Density: per-pixel dropout, stable per seed, full at 1, empty at 0.
    {
        constexpr std::uint32_t w = 16, h = 16;
        auto run = [&](float density, std::uint32_t seed) {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            pittore::compute::DabDensity den{density, seed};
            pittore::compute::paint_tip_dab_host(
                img.data(), w, h, 8.0f, 8.0f, 5.0f, tip, 1.0f,
                RGBAf{0, 0, 0, 0}, nullptr, nullptr, &den);
            int ink = 0;
            for (const auto& p : img)
                if (p.r < 0.5f) ++ink;
            return ink;
        };
        const int full = run(1.0f, 0);
        CHECK(full > 50);
        const int halfA = run(0.5f, 7);
        CHECK(halfA > 0);
        CHECK(halfA < full);
        CHECK(run(0.5f, 7) == halfA);  // same seed, same pixels
        CHECK(run(0.0f, 7) == 0);
    }

    // 34. ABR computed tips are counted, not sampled: a v1 file with one
    // computed (type 1) entry yields no bitmap but skippedComputed == 1,
    // and a mixed file keeps its sampled tip alongside the count.
    {
        std::vector<std::uint8_t> f;
        be16(f, 1);          // version 1
        be16(f, 2);          // two brushes
        be16(f, 1);          // computed
        be32(f, 8);          // small opaque block
        for (int i = 0; i < 8; ++i) f.push_back(0);
        be16(f, 2);          // sampled
        be32(f, 40);         // block size
        be32(f, 0);          // misc
        be16(f, 20);         // spacing
        f.push_back(0);      // antialias
        be16(f, 0);
        be16(f, 0);
        be16(f, 0);
        be16(f, 0);          // short bounds
        be32(f, 0);
        be32(f, 0);
        be32(f, 2);
        be32(f, 3);          // 3x2
        be16(f, 8);          // depth
        f.push_back(0);      // raw
        f.insert(f.end(), {0, 128, 255, 255, 64, 32});
        bl::LoadedAbr a = bl::load_abr(f.data(), f.size());
        CHECK(a.ok);
        CHECK(a.tips.size() == 1);
        CHECK(a.skippedComputed == 1);
    }

    // 35. Tip filter tier: nearest keeps hard texel edges, bilinear
    // blends them. A 2x2 checker magnified 4x differs between tiers.
    {
        constexpr std::uint32_t w = 8, h = 8;
        auto run = [&](int filter) {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::StampTip tip;
            tip.w = 2;
            tip.h = 2;
            tip.alpha = {1.0f, 0.0f, 0.0f, 1.0f};
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 4.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f,
                RGBAf{0, 0, 0, 0}, 0.0f, nullptr, nullptr, nullptr,
                nullptr, 0, filter);
            return img;
        };
        const auto smooth = run(1);
        const auto draft = run(0);
        int diff = 0;
        for (std::size_t i = 0; i < smooth.size(); ++i)
            if (std::fabs(smooth[i].r - draft[i].r) > 1e-6f) ++diff;
        CHECK(diff > 0);  // tiers really differ
        // Draft keeps texel values (the tip's own 0/1 land unblended).
        bool hasExact = false;
        for (const auto& p : draft)
            if (p.r == 0.0f || p.r == 1.0f) hasExact = true;
        CHECK(hasExact);
    }

    // 35b. PAT patterns decode: depth1 gray, depth2 gray+alpha (clear
    // reads white); bad magic and truncation fail cleanly.
    {
        namespace bl = pittore::compute::brushload;
        auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
            v.push_back((x >> 24) & 0xff);
            v.push_back((x >> 16) & 0xff);
            v.push_back((x >> 8) & 0xff);
            v.push_back(x & 0xff);
        };
        auto patFile = [&](std::uint32_t w, std::uint32_t h,
                            std::uint32_t bpp, const char* name,
                            std::vector<std::uint8_t> body) {
            std::vector<std::uint8_t> f;
            be32(f, std::uint32_t(24 + std::strlen(name) + 1));
            be32(f, 1);
            be32(f, w);
            be32(f, h);
            be32(f, bpp);
            f.insert(f.end(), {'G', 'P', 'A', 'T'});
            for (const char* c = name; *c; ++c) f.push_back(*c);
            f.push_back(0);
            f.insert(f.end(), body.begin(), body.end());
            return f;
        };
        {
            bl::PatImage img;
            auto f = patFile(2, 2, 1, "g", {0, 128, 255, 64});
            CHECK(bl::decode_pat(f.data(), f.size(), &img));
            CHECK(img.valid());
            CHECK(img.w == 2 && img.h == 2);
            CHECK(img.name == "g");
            CHECK_NEAR(img.gray[0], 0.0f, 1e-6f);
            CHECK_NEAR(img.gray[1], 128.0f / 255.0f, 1e-6f);
            CHECK_NEAR(img.gray[2], 1.0f, 1e-6f);
            CHECK_NEAR(img.gray[3], 64.0f / 255.0f, 1e-6f);
        }
        {
            bl::PatImage img;
            auto f = patFile(1, 2, 2, "n", {0, 0, 0, 255});
            CHECK(bl::decode_pat(f.data(), f.size(), &img));
            CHECK_NEAR(img.gray[0], 1.0f, 1e-6f);  // clear reads white
            CHECK_NEAR(img.gray[1], 0.0f, 1e-6f);
        }
        {
            bl::PatImage img;
            auto f = patFile(2, 2, 1, "g", {0, 128, 255, 64});
            f[20] = 'X';
            CHECK(!bl::decode_pat(f.data(), f.size(), &img));  // magic
            CHECK(!bl::decode_pat(f.data(), 10, &img));        // truncated
            std::vector<std::uint8_t> tiny{1, 2, 3};
            CHECK(!bl::decode_pat(tiny.data(), tiny.size(), &img));
            CHECK(!bl::decode_pat(nullptr, 0, &img));
        }
    }

    // 35c. Paper grain: levels pipeline order (invert, subtractive
    // brightness, mid-pivot contrast, neutral remap, cutoff window),
    // tiling wrap, and strength-exact combines.
    {
        using pittore::compute::papergrain::GrainMode;
        using pittore::compute::papergrain::PaperGrain;
        float tile[4] = {0.0f, 0.5f, 1.0f, 0.25f};
        auto grain = [&] {
            PaperGrain g;
            g.gray = tile;
            g.w = 2;
            g.h = 2;
            g.strength = 1.0f;
            g.scale = 1.0f;
            g.neutral = 0.5f;
            return g;
        };
        using pittore::compute::papergrain::grain_sample;
        {
            PaperGrain g = grain();
            CHECK_NEAR(grain_sample(&g, 0.0f, 0.0f), 0.0f, 1e-6f);
            CHECK_NEAR(grain_sample(&g, 1.0f, 0.0f), 0.5f, 1e-6f);
            CHECK_NEAR(grain_sample(&g, 0.0f, 1.0f), 1.0f, 1e-6f);
        }
        {
            PaperGrain g = grain();
            g.brightness = 0.3f;  // subtractive: larger darkens
            CHECK_NEAR(grain_sample(&g, 1.0f, 0.0f), 0.2f, 1e-5f);
            CHECK_NEAR(grain_sample(&g, 0.0f, 1.0f), 0.7f, 1e-5f);
        }
        {
            PaperGrain g = grain();
            g.contrast = 2.0f;  // pivots mid gray
            CHECK_NEAR(grain_sample(&g, 1.0f, 0.0f), 0.5f, 1e-5f);
            CHECK_NEAR(grain_sample(&g, 0.0f, 1.0f), 1.0f, 1e-5f);
            CHECK_NEAR(grain_sample(&g, 1.0f, 1.0f), 0.0f, 1e-5f);
        }
        {
            PaperGrain g = grain();
            g.neutral = 0.25f;
            CHECK_NEAR(grain_sample(&g, 0.0f, 1.0f), 1.0f, 1e-5f);
            float dark[4] = {0.125f, 0.125f, 0.125f, 0.125f};
            g.gray = dark;
            CHECK_NEAR(grain_sample(&g, 0.0f, 0.0f), 0.25f, 1e-5f);
        }
        {
            PaperGrain g = grain();
            g.cutoffPolicy = 1;
            g.cutLo = 0.4f;
            g.cutHi = 0.9f;
            CHECK_NEAR(grain_sample(&g, 0.0f, 0.0f), 0.0f, 1e-6f);
            CHECK_NEAR(grain_sample(&g, 1.0f, 0.0f), 0.5f, 1e-6f);
            g.cutoffPolicy = 2;
            CHECK_NEAR(grain_sample(&g, 0.0f, 0.0f), 1.0f, 1e-6f);
        }
        {
            PaperGrain g = grain();
            g.scale = 2.0f;  // tile repeats every 2 target px
            CHECK_NEAR(grain_sample(&g, 2.0f, 0.0f), 0.5f, 1e-5f);
            CHECK_NEAR(grain_sample(&g, 4.0f, 0.0f), 0.0f, 1e-5f);
        }
        using pittore::compute::papergrain::grain_combine;
        CHECK_NEAR(grain_combine(0.8f, 0.5f, 0.0f, GrainMode::Subtract), 0.8f,
                   1e-6f);
        CHECK_NEAR(grain_combine(0.8f, 0.5f, 1.0f, GrainMode::Subtract), 0.3f,
                   1e-6f);
        CHECK_NEAR(grain_combine(0.8f, 0.5f, 1.0f, GrainMode::Darken), 0.5f,
                   1e-6f);
        CHECK_NEAR(grain_combine(0.2f, 0.5f, 1.0f, GrainMode::Darken), 0.2f,
                   1e-6f);
        CHECK_NEAR(grain_combine(0.8f, 0.5f, 1.0f, GrainMode::Multiply), 0.4f,
                   1e-6f);
        using pittore::compute::papergrain::grain_mult;
        CHECK_NEAR(grain_mult(nullptr, 0.7f, 0.0f, 0.0f), 0.7f, 1e-6f);
        PaperGrain off = grain();
        off.strength = 0.0f;
        CHECK(!off.valid());
        CHECK_NEAR(grain_mult(&off, 0.7f, 0.0f, 0.0f), 0.7f, 1e-6f);
    }

    // 35d. Stamp sharpness threshold + grain integration through the dab.
    {
        // Threshold: at/above 1-strength goes opaque, below scales by
        // softness; off stays identical.
        using pittore::compute::StampSharp;
        using pittore::compute::sharpen_coverage;
        CHECK_NEAR(sharpen_coverage(0.5f, nullptr), 0.5f, 1e-6f);
        StampSharp off;
        CHECK_NEAR(sharpen_coverage(0.5f, &off), 0.5f, 1e-6f);
        StampSharp th;
        th.strength = 0.6f;
        th.softness = 1.0f;
        CHECK_NEAR(sharpen_coverage(0.9f, &th), 1.0f, 1e-6f);
        CHECK_NEAR(sharpen_coverage(0.2f, &th), 0.2f, 1e-6f);
        th.softness = 0.0f;
        CHECK_NEAR(sharpen_coverage(0.2f, &th), 0.0f, 1e-6f);
        CHECK_NEAR(sharpen_coverage(1.0f, &th), 1.0f, 1e-6f);
        // Uniform half grain halves a solid stamp dab (black -> mid gray
        // over white); no grain stays black.
        constexpr std::uint32_t w = 8, h = 8;
        auto dabGray = [&](const pittore::compute::papergrain::PaperGrain* kg) {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::StampTip tip;
            tip.w = 4;
            tip.h = 4;
            tip.alpha.assign(16, 1.0f);
            pittore::compute::StampSharp noSharp;
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 2.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f,
                RGBAf{0, 0, 0, 1}, 0.0f, nullptr, nullptr, nullptr, nullptr,
                0, 1, nullptr, nullptr, nullptr, 0.0f,
                pittore::compute::dabmix::DabMixOp::Normal, kg, &noSharp);
            return img[4 * w + 4].r;
        };
        CHECK_NEAR(dabGray(nullptr), 0.0f, 1e-5f);
        float half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        pittore::compute::papergrain::PaperGrain g;
        g.gray = half;
        g.w = 2;
        g.h = 2;
        g.strength = 1.0f;
        g.scale = 1.0f;
        g.neutral = 0.5f;
        CHECK_NEAR(dabGray(&g), 0.5f, 1e-4f);
        // Auto dab honors the same grain path.
        {
            std::vector<RGBAf> img(w * h, RGBAf{1, 1, 1, 1});
            pittore::compute::paint_dab_host(
                img.data(), w, h, 4.0f, 4.0f, 2.0f, 1.0f, 1.0f,
                RGBAf{0, 0, 0, 1}, nullptr, nullptr, nullptr, nullptr, &g);
            CHECK_NEAR(img[4 * w + 4].r, 0.5f, 1e-4f);
        }
    }

    // Blur / Sharpen dab core: flat no-ops, edge response, gates, determinism.
    {
        constexpr std::uint32_t w = 32, h = 32;
        auto flat = [&] {
            std::vector<RGBAf> v(w * h, {0.4f, 0.5f, 0.6f, 1.0f});
            return v;
        };
        auto halfHalf = [&] {
            std::vector<RGBAf> v(w * h);
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x)
                    v[std::size_t(y) * w + x] =
                        x < w / 2 ? RGBAf{0, 0, 0, 1} : RGBAf{1, 1, 1, 1};
            return v;
        };
        int bbox[4] = {0, 0, 0, 0};
        auto same = [&](const std::vector<RGBAf>& a,
                        const std::vector<RGBAf>& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(),
                               a.size() * sizeof(RGBAf)) == 0;
        };
        // Strength 0: no-op, no bbox.
        {
            auto v = halfHalf();
            const auto before = v;
            CHECK(!pittore::compute::blur_sharpen_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 0.0f, false,
                false, bbox));
            CHECK(same(v, before));
        }
        // Blur on flat: blur of flat is flat (bit-identical).
        {
            auto v = flat();
            const auto before = v;
            (void)pittore::compute::blur_sharpen_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, false,
                false, bbox);
            CHECK(same(v, before));
        }
        // Blur on a hard edge: edge pixels move toward mid-grey.
        {
            auto v = halfHalf();
            CHECK(pittore::compute::blur_sharpen_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, false,
                false, bbox));
            CHECK(v[16 * w + 15].r > 0.0f);  // black side lightened
            CHECK(v[16 * w + 16].r < 1.0f);  // white side darkened
            CHECK(bbox[2] > bbox[0] && bbox[3] > bbox[1]);
        }
        // Sharpen with Protect Detail on flat: gate holds, no change.
        {
            auto v = flat();
            const auto before = v;
            CHECK(!pittore::compute::blur_sharpen_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, true,
                true, bbox));
            CHECK(same(v, before));
        }
        // Sharpen on edge without protect: edge contrast grows. Mid-greys
        // (not pure black/white, where clamping eats the delta).
        {
            std::vector<RGBAf> v(w * h);
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x)
                    v[std::size_t(y) * w + x] =
                        x < w / 2 ? RGBAf{0.3f, 0.3f, 0.3f, 1} : RGBAf{0.7f, 0.7f, 0.7f, 1};
            const auto before = v;
            CHECK(pittore::compute::blur_sharpen_dab_host(
                v.data(), w, h, 14.0f, 16.0f, 6.0f, 1.0f, 1.0f, true,
                false, bbox));
            CHECK(!same(v, before));
        }
        // Deterministic: same dab twice from the same start is identical.
        {
            auto a = halfHalf(), b = halfHalf();
            int ba[4] = {0, 0, 0, 0}, bb[4] = {0, 0, 0, 0};
            pittore::compute::blur_sharpen_dab_host(a.data(), w, h, 16.0f,
                                                     16.0f, 8.0f, 0.5f, 0.5f,
                                                     false, false, ba);
            pittore::compute::blur_sharpen_dab_host(b.data(), w, h, 16.0f,
                                                     16.0f, 8.0f, 0.5f, 0.5f,
                                                     false, false, bb);
            CHECK(same(a, b));
            CHECK(std::equal(std::begin(ba), std::end(ba), std::begin(bb)));
        }
        // Kernel radius table is monotonic and bounded.
        CHECK(pittore::compute::blurDabKernelRadius(8.0f) == 1);
        CHECK(pittore::compute::blurDabKernelRadius(64.0f) == 4);
        CHECK(pittore::compute::blurDabKernelRadius(1000.0f) == 4);
    }

    // Background Eraser dab core: tolerance match, ramps, gates.
    {
        constexpr std::uint32_t w = 32, h = 32;
        auto white = [&] {
            return std::vector<RGBAf>(w * h, {1, 1, 1, 1});
        };
        auto same = [&](const std::vector<RGBAf>& a,
                        const std::vector<RGBAf>& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(),
                               a.size() * sizeof(RGBAf)) == 0;
        };
        int bbox[4] = {0, 0, 0, 0};
        const RGBAf paper{1, 1, 1, 1};
        const RGBAf red{1, 0, 0, 1};
        // Exact match on flat white: alpha drops in the dab.
        {
            auto v = white();
            CHECK(pittore::compute::background_erase_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, paper,
                0.5f, false, red, bbox));
            CHECK(v[16 * w + 16].a < 1.0f);  // centre erased
            CHECK(v[0].a == 1.0f);            // far corner untouched
            CHECK(bbox[2] > bbox[0] && bbox[3] > bbox[1]);
        }
        // Non-matching sample: no-op.
        {
            auto v = white();
            const auto before = v;
            CHECK(!pittore::compute::background_erase_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, red,
                0.1f, false, red, bbox));
            CHECK(same(v, before));
        }
        // Protect Foreground: pixel matching fg survives.
        {
            auto v = white();
            const auto before = v;
            CHECK(!pittore::compute::background_erase_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, paper,
                0.5f, true, paper, bbox));
            CHECK(same(v, before));
        }
        // Tolerance 0: exact matches only.
        {
            std::vector<RGBAf> v(w * h, {1, 1, 1, 1});
            v[16 * w + 16] = RGBAf{0.99f, 1.0f, 1.0f, 1.0f};
            CHECK(pittore::compute::background_erase_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 1.0f, 1.0f, paper,
                0.0f, false, red, bbox));
            CHECK(v[16 * w + 16].a == 1.0f);  // off-white survives tol 0
        }
        // RGB untouched, only alpha moves.
        {
            auto v = white();
            CHECK(pittore::compute::background_erase_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, paper,
                0.5f, false, red, bbox));
            CHECK(v[16 * w + 16].r == 1.0f && v[16 * w + 16].g == 1.0f &&
                  v[16 * w + 16].b == 1.0f);
        }
        // Deterministic rerun.
        {
            auto a = white(), b = white();
            int ba[4] = {0, 0, 0, 0}, bb[4] = {0, 0, 0, 0};
            pittore::compute::background_erase_dab_host(
                a.data(), w, h, 16.0f, 16.0f, 8.0f, 0.5f, 0.5f, paper,
                0.5f, false, red, ba);
            pittore::compute::background_erase_dab_host(
                b.data(), w, h, 16.0f, 16.0f, 8.0f, 0.5f, 0.5f, paper,
                0.5f, false, red, bb);
            CHECK(same(a, b));
            CHECK(std::equal(std::begin(ba), std::end(ba), std::begin(bb)));
        }
    }

    // PatternStamp tiles + stamp core.
    {
        constexpr std::uint32_t w = 32, h = 32;
        auto white = [&] {
            return std::vector<RGBAf>(w * h, {1, 1, 1, 1});
        };
        auto same = [&](const std::vector<RGBAf>& a,
                        const std::vector<RGBAf>& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(),
                               a.size() * sizeof(RGBAf)) == 0;
        };
        int bbox[4] = {0, 0, 0, 0};
        // Tiles are deterministic and distinct.
        {
            const auto t0a = pittore::compute::make_pattern_tile(0);
            const auto t0b = pittore::compute::make_pattern_tile(0);
            const auto t1 = pittore::compute::make_pattern_tile(1);
            CHECK(t0a.valid() && t0b.valid() && t1.valid());
            CHECK(same(t0a.px, t0b.px));
            CHECK(!same(t0a.px, t1.px));
            // Clamped ids stay in range.
            CHECK(pittore::compute::make_pattern_tile(99).id == 0);
        }
        // Stamp visibly changes white paper.
        {
            auto v = white();
            pittore::compute::PatternTile tile;
            tile.id = 2;
            tile.px = pittore::compute::make_pattern_tile(2).px;
            CHECK(pittore::compute::pattern_stamp_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 1.0f, tile,
                0.0f, 0.0f, bbox));
            CHECK(!same(v, white()));
            CHECK(bbox[2] > bbox[0] && bbox[3] > bbox[1]);
        }
        // Opacity 0: no-op.
        {
            auto v = white();
            const auto before = v;
            pittore::compute::PatternTile tile;
            tile.id = 0;
            tile.px = pittore::compute::make_pattern_tile(0).px;
            CHECK(!pittore::compute::pattern_stamp_dab_host(
                v.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f, 0.0f, tile,
                0.0f, 0.0f, bbox));
            CHECK(same(v, before));
        }
        // Aligned vs travelled origins differ.
        {
            auto a = white(), b = white();
            pittore::compute::PatternTile tile;
            tile.id = 3;
            tile.px = pittore::compute::make_pattern_tile(3).px;
            pittore::compute::pattern_stamp_dab_host(a.data(), w, h, 16.0f,
                                                     16.0f, 8.0f, 0.8f, 1.0f,
                                                     tile, 0.0f, 0.0f, bbox);
            pittore::compute::pattern_stamp_dab_host(b.data(), w, h, 16.0f,
                                                     16.0f, 8.0f, 0.8f, 1.0f,
                                                     tile, 5.0f, 7.0f, bbox);
            CHECK(!same(a, b));
        }
        // Deterministic rerun.
        {
            auto a = white(), b = white();
            pittore::compute::PatternTile tile;
            tile.id = 1;
            tile.px = pittore::compute::make_pattern_tile(1).px;
            int ba[4] = {0, 0, 0, 0}, bb[4] = {0, 0, 0, 0};
            pittore::compute::pattern_stamp_dab_host(a.data(), w, h, 16.0f,
                                                     16.0f, 8.0f, 0.5f, 0.5f,
                                                     tile, 0.0f, 0.0f, ba);
            pittore::compute::pattern_stamp_dab_host(b.data(), w, h, 16.0f,
                                                     16.0f, 8.0f, 0.5f, 0.5f,
                                                     tile, 0.0f, 0.0f, bb);
            CHECK(same(a, b));
            CHECK(std::equal(std::begin(ba), std::end(ba), std::begin(bb)));
        }
    }

    // History Brush dab core: snapshot source-over restore.
    {
        constexpr std::uint32_t w = 32, h = 32;
        auto paper = [&] {
            return std::vector<RGBAf>(w * h, {1, 1, 1, 1});
        };
        auto same = [&](const std::vector<RGBAf>& a,
                        const std::vector<RGBAf>& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(),
                               a.size() * sizeof(RGBAf)) == 0;
        };
        int bbox[4] = {0, 0, 0, 0};
        // Red dab restored toward white source.
        {
            std::vector<RGBAf> dst(w * h, {1, 0, 0, 1});
            const auto src = paper();
            CHECK(pittore::compute::history_brush_dab_host(
                dst.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f,
                1.0f, bbox));
            CHECK(dst[16 * w + 16].r > 0.0f);  // red lightened toward white
            CHECK(dst[16 * w + 16].g > 0.0f);
            CHECK(bbox[2] > bbox[0] && bbox[3] > bbox[1]);
        }
        // Identical src/dst: restores to (near-)identity — source-over of
        // identical texels is a float round-trip, so compare at 1e-5.
        {
            auto v = paper();
            const auto src = paper();
            (void)pittore::compute::history_brush_dab_host(
                v.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f,
                1.0f, bbox);
            float worst = 0.0f;
            for (std::size_t i = 0; i < v.size(); ++i) {
                worst = std::max(
                    worst, std::fabs(v[i].r - src[i].r));
                worst = std::max(
                    worst, std::fabs(v[i].g - src[i].g));
                worst = std::max(
                    worst, std::fabs(v[i].b - src[i].b));
                worst = std::max(
                    worst, std::fabs(v[i].a - src[i].a));
            }
            CHECK(worst <= 1e-5f);
        }
        // Opacity 0: no-op.
        {
            std::vector<RGBAf> dst(w * h, {1, 0, 0, 1});
            const auto before = dst;
            const auto src = paper();
            CHECK(!pittore::compute::history_brush_dab_host(
                dst.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f,
                0.0f, bbox));
            CHECK(same(dst, before));
        }
        // Deterministic rerun.
        {
            std::vector<RGBAf> a(w * h, {1, 0, 0, 1});
            std::vector<RGBAf> b(w * h, {1, 0, 0, 1});
            const auto src = paper();
            int ba[4] = {0, 0, 0, 0}, bb[4] = {0, 0, 0, 0};
            pittore::compute::history_brush_dab_host(
                a.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.5f,
                0.5f, ba);
            pittore::compute::history_brush_dab_host(
                b.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.5f,
                0.5f, bb);
            CHECK(same(a, b));
            CHECK(std::equal(std::begin(ba), std::end(ba), std::begin(bb)));
        }
    }

    // Healing Brush donor translate: offset sampling, clamped edges.
    {
        constexpr std::uint32_t w = 16, h = 16;
        auto ramp = [&] {
            std::vector<RGBAf> v(w * h);
            for (std::uint32_t y = 0; y < h; ++y)
                for (std::uint32_t x = 0; x < w; ++x)
                    v[std::size_t(y) * w + x] = RGBAf{
                        float(x) / float(w), float(y) / float(h), 0.5f,
                        1.0f};
            return v;
        };
        // Zero offset: identity copy.
        {
            const auto base = ramp();
            std::vector<RGBAf> donor(w * h);
            pittore::compute::translate_heal_donor_host(
                base.data(), donor.data(), w, h, 0.0f, 0.0f);
            CHECK(std::memcmp(base.data(), donor.data(),
                              base.size() * sizeof(RGBAf)) == 0);
        }
        // Positive offset shifts the donor window (donor[i] = base[i - o]).
        {
            const auto base = ramp();
            std::vector<RGBAf> donor(w * h);
            pittore::compute::translate_heal_donor_host(
                base.data(), donor.data(), w, h, 3.0f, 2.0f);
            CHECK(donor[5 * w + 6].r == base[3 * w + 3].r);
            CHECK(donor[5 * w + 6].g == base[3 * w + 3].g);
            // Clamped edge: donor(0,0) reads base(0,0).
            CHECK(donor[0].r == base[0].r && donor[0].g == base[0].g);
        }
        // Deterministic rerun.
        {
            const auto base = ramp();
            std::vector<RGBAf> a(w * h), b(w * h);
            pittore::compute::translate_heal_donor_host(
                base.data(), a.data(), w, h, 3.0f, 2.0f);
            pittore::compute::translate_heal_donor_host(
                base.data(), b.data(), w, h, 3.0f, 2.0f);
            CHECK(std::memcmp(a.data(), b.data(),
                              a.size() * sizeof(RGBAf)) == 0);
        }
        // spot_heal_host with an offset donor heals the blemish.
        {
            constexpr std::uint32_t hw = 64, hh = 64;
            std::vector<RGBAf> layer(hw * hh, {1, 1, 1, 1});
            for (std::uint32_t y = 24; y < 40; ++y)
                for (std::uint32_t x = 24; x < 40; ++x)
                    layer[std::size_t(y) * hw + x] = RGBAf{0, 0, 0, 1};
            std::vector<RGBAf> donor(hw * hh);
            pittore::compute::translate_heal_donor_host(
                layer.data(), donor.data(), hw, hh, -20.0f, -20.0f);
            int bbox[4] = {0, 0, 0, 0};
            CHECK(pittore::compute::spot_heal_host(
                layer.data(), hw, hh, 32.0f, 32.0f, 10.0f, 0.8f,
                donor.data(),
                pittore::compute::HealType::ContentAware, 5, bbox));
            CHECK(bbox[2] > bbox[0] && bbox[3] > bbox[1]);
            // The black core lightened (healed toward the white donor).
            CHECK(layer[32 * hw + 32].r > 0.0f);
        }
    }

    // Red-eye fix: red pupils desaturate, everything else survives.
    {
        constexpr std::uint32_t w = 32, h = 32;
        auto same = [&](const std::vector<RGBAf>& a,
                        const std::vector<RGBAf>& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(),
                               a.size() * sizeof(RGBAf)) == 0;
        };
        int bbox[4] = {0, 0, 0, 0};
        // Red pupil in the box is fixed.
        {
            std::vector<RGBAf> v(w * h, {1, 1, 1, 1});
            v[16 * w + 16] = RGBAf{0.9f, 0.1f, 0.1f, 1.0f};
            CHECK(pittore::compute::redeye_fix_host(v.data(), w, h, 8, 8,
                                                     24, 24, 1.0f, bbox));
            CHECK(v[16 * w + 16].r < 0.9f);  // desaturated + darkened
            CHECK(std::fabs(v[16 * w + 16].r - v[16 * w + 16].g) < 1e-5f);
        }
        // Non-red pixels untouched.
        {
            std::vector<RGBAf> v(w * h, {0.2f, 0.5f, 0.8f, 1.0f});
            const auto before = v;
            CHECK(!pittore::compute::redeye_fix_host(v.data(), w, h, 0, 0,
                                                      w, h, 1.0f, bbox));
            CHECK(same(v, before));
        }
        // Darken scales the correction.
        {
            std::vector<RGBAf> a(w * h, {1, 1, 1, 1});
            std::vector<RGBAf> b(w * h, {1, 1, 1, 1});
            a[16 * w + 16] = RGBAf{0.9f, 0.1f, 0.1f, 1.0f};
            b[16 * w + 16] = RGBAf{0.9f, 0.1f, 0.1f, 1.0f};
            pittore::compute::redeye_fix_host(a.data(), w, h, 8, 8, 24,
                                               24, 1.0f, bbox);
            pittore::compute::redeye_fix_host(b.data(), w, h, 8, 8, 24,
                                               24, 0.0f, bbox);
            CHECK(a[16 * w + 16].r < b[16 * w + 16].r);
            CHECK(b[16 * w + 16].r < 0.9f);  // still desaturated at 0
        }
        // Deterministic rerun.
        {
            std::vector<RGBAf> a(w * h, {1, 1, 1, 1});
            std::vector<RGBAf> b(w * h, {1, 1, 1, 1});
            a[10 * w + 10] = RGBAf{0.8f, 0.1f, 0.2f, 1.0f};
            b[10 * w + 10] = RGBAf{0.8f, 0.1f, 0.2f, 1.0f};
            pittore::compute::redeye_fix_host(a.data(), w, h, 0, 0, w, h,
                                               0.5f, bbox);
            pittore::compute::redeye_fix_host(b.data(), w, h, 0, 0, w, h,
                                               0.5f, bbox);
            CHECK(same(a, b));
        }
    }

    // Art History dab: styled history stamps with tolerance gate.
    {
        constexpr std::uint32_t w = 32, h = 32;
        auto paper = [&] {
            return std::vector<RGBAf>(w * h, {1, 1, 1, 1});
        };
        auto same = [&](const std::vector<RGBAf>& a,
                        const std::vector<RGBAf>& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(),
                               a.size() * sizeof(RGBAf)) == 0;
        };
        int bbox[4] = {0, 0, 0, 0};
        // Red field restored toward the white source (offset stamp).
        {
            std::vector<RGBAf> dst(w * h, {1, 0, 0, 1});
            const auto src = paper();
            CHECK(pittore::compute::arthistory_dab_host(
                dst.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f,
                1.0f, 6.0f, 0.0f, 12.0f, 0.0f, bbox));
            bool moved = false;
            for (const RGBAf& p : dst)
                if (p.g > 0.0f) {
                    moved = true;
                    break;
                }
            CHECK(moved);
        }
        // Strict tolerance gates the differing field out.
        {
            std::vector<RGBAf> dst(w * h, {1, 0, 0, 1});
            const auto before = dst;
            const auto src = paper();
            (void)pittore::compute::arthistory_dab_host(
                dst.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.8f,
                1.0f, 0.0f, 0.0f, 12.0f, 0.01f, bbox);
            CHECK(same(dst, before));
        }
        // Deterministic rerun.
        {
            std::vector<RGBAf> a(w * h, {1, 0, 0, 1});
            std::vector<RGBAf> b(w * h, {1, 0, 0, 1});
            const auto src = paper();
            int ba[4] = {0, 0, 0, 0}, bb[4] = {0, 0, 0, 0};
            pittore::compute::arthistory_dab_host(
                a.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.5f,
                0.5f, 6.0f, 0.0f, 12.0f, 0.0f, ba);
            pittore::compute::arthistory_dab_host(
                b.data(), src.data(), w, h, 16.0f, 16.0f, 8.0f, 0.5f,
                0.5f, 6.0f, 0.0f, 12.0f, 0.0f, bb);
            CHECK(same(a, b));
            CHECK(std::equal(std::begin(ba), std::end(ba), std::begin(bb)));
        }
    }

    // Per-dab blend modes (the painting tools' "Mode" combo): the W3C blend
    // shell replaces the source-over mix in every paint kernel. Multiply and
    // Screen back the Alt+Shift+M/S shortcuts.
    {
        constexpr std::uint32_t w = 16, h = 16;
        const RGBAf grey{0.5f, 0.5f, 0.5f, 1.0f};
        const RGBAf color{0.5f, 0.25f, 0.75f, 1.0f};
        auto roundBlend = [&](pittore::compute::BlendMode mode, RGBAf backdrop,
                              float opacity,
                              pittore::compute::NativeCmykPaint* cmyk = nullptr) {
            std::vector<RGBAf> img(w * h, backdrop);
            pittore::compute::paint_dab_host(
                img.data(), w, h, 8.0f, 8.0f, 4.0f, 1.0f, opacity, color,
                nullptr, nullptr, nullptr, nullptr, nullptr,
                pittore::compute::dabmix::DabMixOp::Normal, cmyk, mode);
            return img[8 * w + 8];
        };
        // Opaque backdrop at full opacity: the core texel is exactly B(Cb, Cs).
        {
            const RGBAf mul = roundBlend(pittore::compute::BlendMode::Multiply,
                                         grey, 1.0f);
            CHECK_NEAR(mul.r, 0.25f, 1e-5f);
            CHECK_NEAR(mul.g, 0.125f, 1e-5f);
            CHECK_NEAR(mul.b, 0.375f, 1e-5f);
            CHECK_NEAR(mul.a, 1.0f, 1e-6f);
            const RGBAf scr = roundBlend(pittore::compute::BlendMode::Screen,
                                         grey, 1.0f);
            CHECK_NEAR(scr.r, 0.75f, 1e-5f);
            CHECK_NEAR(scr.g, 0.625f, 1e-5f);
            CHECK_NEAR(scr.b, 0.875f, 1e-5f);
            CHECK_NEAR(scr.a, 1.0f, 1e-6f);
        }
        // Transparent backdrop: the shell falls back to the source colour and
        // keeps the source alpha.
        {
            const RGBAf onAir = roundBlend(pittore::compute::BlendMode::Multiply,
                                           RGBAf{0, 0, 0, 0}, 1.0f);
            CHECK_NEAR(onAir.r, color.r, 1e-5f);
            CHECK_NEAR(onAir.g, color.g, 1e-5f);
            CHECK_NEAR(onAir.b, color.b, 1e-5f);
            CHECK_NEAR(onAir.a, 1.0f, 1e-5f);
            const RGBAf halfAir =
                roundBlend(pittore::compute::BlendMode::Multiply,
                           RGBAf{0, 0, 0, 0}, 0.5f);
            CHECK_NEAR(halfAir.r, color.r, 1e-5f);
            CHECK_NEAR(halfAir.a, 0.5f, 1e-5f);
        }
        // An explicit mix op still wins over the blend mode.
        {
            std::vector<RGBAf> a(w * h, grey), b(w * h, grey);
            pittore::compute::paint_dab_host(
                a.data(), w, h, 8.0f, 8.0f, 4.0f, 1.0f, 1.0f, color, nullptr,
                nullptr, nullptr, nullptr, nullptr,
                pittore::compute::dabmix::DabMixOp::Allanon, nullptr,
                pittore::compute::BlendMode::Multiply);
            pittore::compute::paint_dab_host(
                b.data(), w, h, 8.0f, 8.0f, 4.0f, 1.0f, 1.0f, color, nullptr,
                nullptr, nullptr, nullptr, nullptr,
                pittore::compute::dabmix::DabMixOp::Allanon);
            CHECK(std::memcmp(a.data(), b.data(),
                              a.size() * sizeof(RGBAf)) == 0);
        }
        // The tail default stays Normal bit-for-bit.
        {
            std::vector<RGBAf> a(w * h, grey), b(w * h, grey);
            pittore::compute::paint_dab_host(a.data(), w, h, 8.0f, 8.0f, 4.0f,
                                             0.5f, 0.6f, color);
            pittore::compute::paint_dab_host(
                b.data(), w, h, 8.0f, 8.0f, 4.0f, 0.5f, 0.6f, color, nullptr,
                nullptr, nullptr, nullptr, nullptr,
                pittore::compute::dabmix::DabMixOp::Normal, nullptr,
                pittore::compute::BlendMode::Normal);
            CHECK(std::memcmp(a.data(), b.data(),
                              a.size() * sizeof(RGBAf)) == 0);
        }
        // Auto-tip dabs share the shell (round ratio-1 tip, hard core).
        {
            pittore::compute::AutoTip tip;
            tip.hardness = 1.0f;
            std::vector<RGBAf> img(w * h, grey);
            pittore::compute::paint_tip_dab_host(
                img.data(), w, h, 8.0f, 8.0f, 4.0f, tip, 1.0f, color, nullptr,
                nullptr, nullptr, nullptr, nullptr,
                pittore::compute::dabmix::DabMixOp::Normal, nullptr,
                pittore::compute::BlendMode::Multiply);
            CHECK_NEAR(img[8 * w + 8].r, 0.25f, 1e-5f);
            CHECK_NEAR(img[8 * w + 8].g, 0.125f, 1e-5f);
            CHECK_NEAR(img[8 * w + 8].b, 0.375f, 1e-5f);
        }
        // Stamp dabs share the shell (solid tip covers the core fully).
        {
            pittore::compute::StampTip tip;
            tip.w = tip.h = 8;
            tip.alpha.assign(64, 1.0f);
            std::vector<RGBAf> img(w * h, grey);
            pittore::compute::stamp_dab_host(
                img.data(), w, h, 8.0f, 8.0f, 4.0f, tip,
                pittore::compute::StampMode::AlphaMask, 1.0f, color, 0.0f,
                nullptr, nullptr, nullptr, nullptr, 0, 1, nullptr, nullptr,
                nullptr, 0.0f, pittore::compute::dabmix::DabMixOp::Normal,
                nullptr, nullptr, nullptr, pittore::compute::BlendMode::Screen);
            CHECK_NEAR(img[8 * w + 8].r, 0.75f, 1e-5f);
            CHECK_NEAR(img[8 * w + 8].g, 0.625f, 1e-5f);
            CHECK_NEAR(img[8 * w + 8].b, 0.875f, 1e-5f);
        }
        // Native-CMYK destinations re-separate the blended projection.
        {
            std::vector<std::array<std::uint16_t, 4>> inks(w * h);
            pittore::color::SrgbToCmyk separation(nullptr, 0);
            pittore::color::CmykToSrgb preview(nullptr, 0);
            pittore::compute::NativeCmykPaint native;
            native.pixels = inks.data();
            native.separation = &separation;
            native.preview = &preview;
            const RGBAf out = roundBlend(pittore::compute::BlendMode::Multiply,
                                         grey, 1.0f, &native);
            CHECK_NEAR(out.r, 0.25f, 1e-5f);
            CHECK_NEAR(out.g, 0.125f, 1e-5f);
            CHECK_NEAR(out.b, 0.375f, 1e-5f);
            CHECK_NEAR(out.a, 1.0f, 1e-5f);
            const std::array<std::uint16_t, 4>& core = inks[8 * w + 8];
            float expected[4];
            separation.convert(out.r, out.g, out.b, expected);
            for (int c = 0; c < 4; ++c)
                CHECK(std::abs(int(core[c]) -
                               int(std::lround(expected[c] * 65535.0f))) <= 1);
            float recombined[3];
            preview.convert(core[0] / 65535.0f, core[1] / 65535.0f,
                            core[2] / 65535.0f, core[3] / 65535.0f, recombined);
            CHECK_NEAR(out.r, recombined[0], 1e-6f);
            CHECK_NEAR(out.g, recombined[1], 1e-6f);
            CHECK_NEAR(out.b, recombined[2], 1e-6f);
        }
        // Without a separation there are no inks to maintain: plain RGB blend.
        {
            const std::array<std::uint16_t, 4> kept{1000, 2000, 3000, 4000};
            std::vector<std::array<std::uint16_t, 4>> inks(w * h, kept);
            pittore::color::CmykToSrgb preview(nullptr, 0);
            pittore::compute::NativeCmykPaint native;
            native.pixels = inks.data();
            native.preview = &preview;
            const RGBAf out = roundBlend(pittore::compute::BlendMode::Multiply,
                                         grey, 1.0f, &native);
            CHECK_NEAR(out.r, 0.25f, 1e-5f);
            CHECK(inks[8 * w + 8] == kept);
        }
        // Guard cases: the helper reports "unhandled" and leaves the texel.
        {
            RGBAf d{0.4f, 0.6f, 0.8f, 0.5f};
            const RGBAf before = d;
            std::array<std::uint16_t, 4> inks{};
            pittore::color::SrgbToCmyk separation(nullptr, 0);
            pittore::color::CmykToSrgb preview(nullptr, 0);
            pittore::compute::NativeCmykPaint native;
            CHECK(!pittore::compute::native_cmyk_blend_texel(
                &native, 0, d, pittore::compute::BlendMode::Multiply, 0.5f,
                0.5f, 0.5f, 1.0f, 0, 0));
            native.pixels = &inks;
            CHECK(!pittore::compute::native_cmyk_blend_texel(
                &native, 0, d, pittore::compute::BlendMode::Multiply, 0.5f,
                0.5f, 0.5f, 1.0f, 0, 0));
            native.separation = &separation;
            CHECK(!pittore::compute::native_cmyk_blend_texel(
                &native, 0, d, pittore::compute::BlendMode::Multiply, 0.5f,
                0.5f, 0.5f, 1.0f, 0, 0));
            CHECK(!pittore::compute::native_cmyk_blend_texel(
                nullptr, 0, d, pittore::compute::BlendMode::Multiply, 0.5f,
                0.5f, 0.5f, 1.0f, 0, 0));
            CHECK_NEAR(d.r, before.r, 1e-7f);
            CHECK_NEAR(d.g, before.g, 1e-7f);
            CHECK_NEAR(d.b, before.b, 1e-7f);
            CHECK_NEAR(d.a, before.a, 1e-7f);
            native.preview = &preview;
            CHECK(pittore::compute::native_cmyk_blend_texel(
                &native, 0, d, pittore::compute::BlendMode::Multiply, 0.5f,
                0.5f, 0.5f, 1.0f, 0, 0));
        }
    }
}

#ifndef PITTORE_TEST_NO_MAIN
TEST_MAIN_CALL(test_brush)
#endif
