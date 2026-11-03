// Live adjustment math (engine/compute/adjust.h): hand-derived values per
// kind, identity cases, and cross-checks against the destructive tonal_ops
// implementations (Levels/Curves share semantics with the dialogs).
#include <cmath>
#include <cstring>
#include <vector>

#include "engine/compute/adjust.h"
#include "engine/compute/factory.h"
#include "engine/core/image.h"
#include "engine/core/tonal_ops.h"
#include "test_util.h"

using pittore::RGBAf;
using pittore::compute::AdjustmentKind;

namespace {

void applyOne(AdjustmentKind kind, const float* p, const float* lut, float r,
              float g, float b, float& or_, float& og, float& ob) {
    pittore::compute::adjust::apply(kind, p, lut, r, g, b, or_, og, ob);
}

}  // namespace

static void test_adjust() {
    float or_, og, ob;
    const float zero[8] = {};

    // --- Brightness/Contrast: out = clamp((v + b) * (1 + c)) ---------------
    applyOne(AdjustmentKind::BrightnessContrast, zero, nullptr, 0.5f, 0.5f,
             0.5f, or_, og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-6f);  // identity at rest
    const float bc[8] = {0.1f, 0.0f};
    applyOne(AdjustmentKind::BrightnessContrast, bc, nullptr, 0.5f, 0.5f, 0.5f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.6f, 1e-6f);
    const float full[8] = {0.0f, 1.0f};
    applyOne(AdjustmentKind::BrightnessContrast, full, nullptr, 0.5f, 0.5f,
             0.5f, or_, og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);  // 0.5 * 2 clamps to 1

    // --- Invert ------------------------------------------------------------
    applyOne(AdjustmentKind::Invert, zero, nullptr, 0.25f, 0.5f, 1.0f, or_, og,
             ob);
    CHECK_NEAR(or_, 0.75f, 1e-6f);
    CHECK_NEAR(og, 0.5f, 1e-6f);
    CHECK_NEAR(ob, 0.0f, 1e-6f);

    // --- Threshold is luma-based -------------------------------------------
    const float th[8] = {0.5f};
    applyOne(AdjustmentKind::Threshold, th, nullptr, 1.0f, 0.0f, 0.0f, or_, og,
             ob);  // red luma 0.2126 < 0.5
    CHECK_NEAR(or_, 0.0f, 1e-6f);
    CHECK_NEAR(og, 0.0f, 1e-6f);
    CHECK_NEAR(ob, 0.0f, 1e-6f);
    const float th2[8] = {0.2f};
    applyOne(AdjustmentKind::Threshold, th2, nullptr, 1.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);

    // --- Posterize ----------------------------------------------------------
    const float pz[8] = {2.0f};
    applyOne(AdjustmentKind::Posterize, pz, nullptr, 0.3f, 0.6f, 0.9f, or_, og,
             ob);
    CHECK_NEAR(or_, 0.0f, 1e-6f);  // floor(0.3+0.5) = 0
    CHECK_NEAR(og, 1.0f, 1e-6f);   // floor(0.6+0.5) = 1
    CHECK_NEAR(ob, 1.0f, 1e-6f);

    // --- Exposure: v * 2^stops ----------------------------------------------
    const float ex[8] = {1.0f};
    applyOne(AdjustmentKind::Exposure, ex, nullptr, 0.25f, 0.25f, 0.25f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-6f);
    const float exm[8] = {-1.0f};
    applyOne(AdjustmentKind::Exposure, exm, nullptr, 0.5f, 0.5f, 0.5f, or_, og,
             ob);
    CHECK_NEAR(or_, 0.25f, 1e-6f);

    // --- Levels identity + input range --------------------------------------
    const float lv[8] = {0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    applyOne(AdjustmentKind::Levels, lv, nullptr, 0.37f, 0.37f, 0.37f, or_, og,
             ob);
    CHECK_NEAR(or_, 0.37f, 1e-5f);
    const float lv2[8] = {0.5f, 1.0f, 1.0f, 0.0f, 1.0f};
    applyOne(AdjustmentKind::Levels, lv2, nullptr, 0.75f, 0.75f, 0.75f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-5f);  // (0.75-0.5)/0.5

    // --- Levels matches the destructive dialog implementation ----------------
    {
        pittore::Image img(4, 4);
        for (std::uint32_t i = 0; i < 16; ++i) {
            const float v = static_cast<float>(i) / 15.0f;
            img.data()[i] = RGBAf{v, 1.0f - v, 0.5f * v, 1.0f};
        }
        pittore::Image ref = img.clone();
        pittore::applyLevels(ref, 3, 0.25, 0.75, 2.0, 0.1, 0.9);
        const float lp[8] = {0.25f, 0.75f, 2.0f, 0.1f, 0.9f};
        for (std::uint32_t i = 0; i < 16; ++i) {
            const RGBAf& s = img.data()[i];
            applyOne(AdjustmentKind::Levels, lp, nullptr, s.r, s.g, s.b, or_,
                     og, ob);
            CHECK_NEAR(or_, ref.data()[i].r, 1e-5f);
            CHECK_NEAR(og, ref.data()[i].g, 1e-5f);
            CHECK_NEAR(ob, ref.data()[i].b, 1e-5f);
        }
    }

    // --- Levels table matches direct evaluation ------------------------------
    // Exact on the 256 grid points (bin i samples levels(i/255) by
    // construction); bounded off-grid by half a bin times local slope, so
    // the off-grid probe uses gentle params without cliff edges.
    {
        const float lp[8] = {0.0f, 1.0f, 1.5f, 0.0f, 1.0f};
        float lut[256];
        pittore::buildLevelsLUT(lp, lut);
        for (int i = 0; i < 256; ++i) {
            const float v = i / 255.0f;
            applyOne(AdjustmentKind::Levels, lp, lut, v, v, v, or_, og, ob);
            float dr, dg, db;
            applyOne(AdjustmentKind::Levels, lp, nullptr, v, v, v, dr, dg,
                     db);
            CHECK_NEAR(or_, dr, 1e-6f);
            CHECK_NEAR(og, dg, 1e-6f);
            CHECK_NEAR(ob, db, 2e-6f);
        }
        applyOne(AdjustmentKind::Levels, lp, lut, 0.123f, 0.456f, 0.789f, or_,
                 og, ob);
        float dr, dg, db;
        applyOne(AdjustmentKind::Levels, lp, nullptr, 0.123f, 0.456f, 0.789f,
                 dr, dg, db);
        CHECK_NEAR(or_, dr, 4e-3f);
        CHECK_NEAR(og, dg, 4e-3f);
        CHECK_NEAR(ob, db, 4e-3f);
    }

    // --- Curves LUT matches applyCurveLUT ------------------------------------
    {
        // 3x256 tables (R/G/B); the shared table here exercises the master
        // path on all three channels.
        float lut[768];
        pittore::buildCurveLUT({{0.0, 0.0}, {0.25, 0.1}, {1.0, 0.9}},
                                lut);
        for (int c = 1; c < 3; ++c)
            std::memcpy(lut + 256 * c, lut, 256 * sizeof(float));
        pittore::Image img(8, 2);
        for (std::uint32_t i = 0; i < 16; ++i) {
            const float v = static_cast<float>(i) / 15.0f;
            img.data()[i] = RGBAf{v, v, v, 1.0f};
        }
        pittore::Image ref = img.clone();
        pittore::applyCurveLUT(ref, 3, lut);
        for (std::uint32_t i = 0; i < 16; ++i) {
            const RGBAf& s = img.data()[i];
            applyOne(AdjustmentKind::Curves, zero, lut, s.r, s.g, s.b, or_, og,
                     ob);
            CHECK_NEAR(or_, ref.data()[i].r, 1e-6f);
        }
        // Null LUT is identity.
        applyOne(AdjustmentKind::Curves, zero, nullptr, 0.4f, 0.4f, 0.4f, or_,
                 og, ob);
        CHECK_NEAR(or_, 0.4f, 1e-6f);
    }

    // --- Curves per-channel tables -------------------------------------------
    {
        // R lifts shadows, G/B stay identity: only red moves.
        float lut[768];
        pittore::buildCurveLUT({{0.0, 0.5}, {1.0, 1.0}}, lut);
        pittore::buildCurveLUT({}, lut + 256);
        pittore::buildCurveLUT({}, lut + 512);
        applyOne(AdjustmentKind::Curves, zero, lut, 0.0f, 0.0f, 0.0f, or_, og,
                 ob);
        CHECK_NEAR(or_, 0.5f, 1e-6f);
        CHECK_NEAR(og, 0.0f, 1e-6f);
        CHECK_NEAR(ob, 0.0f, 1e-6f);
        applyOne(AdjustmentKind::Curves, zero, lut, 1.0f, 1.0f, 1.0f, or_, og,
                 ob);
        CHECK_NEAR(or_, 1.0f, 1e-6f);
        CHECK_NEAR(og, 1.0f, 1e-6f);
        CHECK_NEAR(ob, 1.0f, 1e-6f);
    }

    // --- Vibrance ------------------------------------------------------------
    const float vib0[8] = {0.0f};
    applyOne(AdjustmentKind::Vibrance, vib0, nullptr, 0.2f, 0.7f, 0.4f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.2f, 1e-6f);  // zero amount is identity
    CHECK_NEAR(og, 0.7f, 1e-6f);
    const float vib1[8] = {1.0f};
    applyOne(AdjustmentKind::Vibrance, vib1, nullptr, 0.5f, 0.5f, 0.5f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-6f);  // grey has no span: unchanged
    applyOne(AdjustmentKind::Vibrance, vib1, nullptr, 1.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);  // vivid stays vivid
    CHECK_NEAR(og, 0.0f, 1e-6f);
    const float vibm[8] = {-1.0f};
    applyOne(AdjustmentKind::Vibrance, vibm, nullptr, 1.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.2126f, 1e-5f);  // full negative desaturates to luma
    CHECK_NEAR(og, 0.2126f, 1e-5f);
    CHECK_NEAR(ob, 0.2126f, 1e-5f);

    // --- Hue/Saturation -------------------------------------------------------
    const float hs0[8] = {0.0f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::HueSaturation, hs0, nullptr, 0.3f, 0.6f, 0.1f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.3f, 1e-6f);  // zero shift is identity
    const float hsdesat[8] = {0.0f, -1.0f, 0.0f};
    applyOne(AdjustmentKind::HueSaturation, hsdesat, nullptr, 1.0f, 0.0f, 0.0f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-6f);  // red desaturates to HSL lightness 0.5
    CHECK_NEAR(og, 0.5f, 1e-6f);
    CHECK_NEAR(ob, 0.5f, 1e-6f);
    // Matches the engine's own hue_saturation op exactly.
    {
        auto backend = pittore::compute::make_backend(
            pittore::compute::BackendType::CPU);
        std::vector<RGBAf> src(16), dst(16);
        for (std::uint32_t i = 0; i < 16; ++i)
            src[i] = RGBAf{(i % 4) / 3.0f, ((i / 4) % 2) * 0.7f,
                           (i % 3) / 2.0f, 1.0f};
        auto sb = backend->make_buffer(16 * sizeof(RGBAf));
        auto db = backend->make_buffer(16 * sizeof(RGBAf));
        std::memcpy(sb->host(), src.data(), sb->size());
        sb->upload();
        backend->hue_saturation(*sb, *db, 4, 4, 45.0f, 0.3f, -0.1f);
        const auto* got = static_cast<const RGBAf*>(db->host());
        const float hp[8] = {45.0f, 0.3f, -0.1f};
        for (std::uint32_t i = 0; i < 16; ++i) {
            applyOne(AdjustmentKind::HueSaturation, hp, nullptr, src[i].r,
                     src[i].g, src[i].b, or_, og, ob);
            CHECK_NEAR(or_, got[i].r, 1e-6f);
            CHECK_NEAR(og, got[i].g, 1e-6f);
            CHECK_NEAR(ob, got[i].b, 1e-6f);
        }
    }

    // --- PhotoFilter ----------------------------------------------------------
    // Density 0 is identity whatever the filter color.
    const float pf0[8] = {1.0f, 0.55f, 0.0f, 0.0f, 1.0f};
    applyOne(AdjustmentKind::PhotoFilter, pf0, nullptr, 0.2f, 0.7f, 0.4f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.2f, 1e-6f);
    CHECK_NEAR(og, 0.7f, 1e-6f);
    CHECK_NEAR(ob, 0.4f, 1e-6f);
    // Grey filter is identity at any density.
    const float pfg[8] = {0.5f, 0.5f, 0.5f, 1.0f, 1.0f};
    applyOne(AdjustmentKind::PhotoFilter, pfg, nullptr, 0.3f, 0.6f, 0.1f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.3f, 1e-5f);
    CHECK_NEAR(og, 0.6f, 1e-5f);
    CHECK_NEAR(ob, 0.1f, 1e-5f);
    const float pfg2[8] = {0.5f, 0.5f, 0.5f, 1.0f, 0.0f};
    applyOne(AdjustmentKind::PhotoFilter, pfg2, nullptr, 0.3f, 0.6f, 0.1f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.3f, 1e-5f);
    CHECK_NEAR(og, 0.6f, 1e-5f);
    CHECK_NEAR(ob, 0.1f, 1e-5f);
    // Warming gel at 25%, preserve off: luma(filter) = .606, 25% mix.
    const float pfw[8] = {1.0f, 0.55f, 0.0f, 0.25f, 0.0f};
    applyOne(AdjustmentKind::PhotoFilter, pfw, nullptr, 0.5f, 0.5f, 0.5f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.58128f, 1e-4f);
    CHECK_NEAR(og, 0.48846f, 1e-4f);
    CHECK_NEAR(ob, 0.375f, 1e-4f);
    // Full density with preserve: saturated orange at source lightness.
    const float pfp[8] = {1.0f, 0.55f, 0.0f, 1.0f, 1.0f};
    applyOne(AdjustmentKind::PhotoFilter, pfp, nullptr, 0.5f, 0.5f, 0.5f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-5f);
    CHECK_NEAR(og, 0.5503f, 1e-3f);
    CHECK_NEAR(ob, 0.0f, 1e-5f);
    // Preserve on white must not NaN; white stays white.
    const float pfwhite[8] = {1.0f, 0.55f, 0.0f, 0.25f, 1.0f};
    applyOne(AdjustmentKind::PhotoFilter, pfwhite, nullptr, 1.0f, 1.0f, 1.0f,
             or_, og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);
    CHECK_NEAR(og, 1.0f, 1e-6f);
    CHECK_NEAR(ob, 1.0f, 1e-6f);

    // --- WhiteBalance ---------------------------------------------------------
    // 6500K is near-daylight: near identity on grey.
    const float wb6500[8] = {6500.0f, 0.0f};
    applyOne(AdjustmentKind::WhiteBalance, wb6500, nullptr, 0.5f, 0.5f, 0.5f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.49826f, 1e-3f);
    CHECK_NEAR(og, 0.5f, 1e-3f);
    CHECK_NEAR(ob, 0.50814f, 1e-3f);
    // 3200K corrects tungsten: red to ~0.72, green/blue clip at 1.
    const float wb3200[8] = {3200.0f, 0.0f};
    applyOne(AdjustmentKind::WhiteBalance, wb3200, nullptr, 1.0f, 1.0f, 1.0f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.72008f, 1e-3f);
    CHECK_NEAR(og, 1.0f, 1e-6f);
    CHECK_NEAR(ob, 1.0f, 1e-6f);
    // Full magenta tint halves green.
    const float wbt[8] = {6500.0f, 1.0f};
    applyOne(AdjustmentKind::WhiteBalance, wbt, nullptr, 1.0f, 1.0f, 1.0f, or_,
             og, ob);
    CHECK_NEAR(og, 0.5f, 1e-6f);

    // --- BlackWhite -----------------------------------------------------------
    // Neutrals survive any slider setting.
    const float bwmax[8] = {300.0f, -200.0f, 300.0f, -200.0f, 300.0f, -200.0f};
    applyOne(AdjustmentKind::BlackWhite, bwmax, nullptr, 0.5f, 0.5f, 0.5f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-6f);
    CHECK_NEAR(og, 0.5f, 1e-6f);
    CHECK_NEAR(ob, 0.5f, 1e-6f);
    // Defaults: red 0.4, yellow 0.6, green 0.4, cyan 0.6, blue 0.2,
    // magenta 0.8.
    const float bwdef[8] = {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
    applyOne(AdjustmentKind::BlackWhite, bwdef, nullptr, 1.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.4f, 1e-5f);
    CHECK_NEAR(og, 0.4f, 1e-5f);
    CHECK_NEAR(ob, 0.4f, 1e-5f);
    applyOne(AdjustmentKind::BlackWhite, bwdef, nullptr, 1.0f, 1.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.6f, 1e-5f);
    applyOne(AdjustmentKind::BlackWhite, bwdef, nullptr, 0.0f, 0.0f, 1.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.2f, 1e-5f);
    // Extremes clip.
    const float bwhi[8] = {300.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
    applyOne(AdjustmentKind::BlackWhite, bwhi, nullptr, 1.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);
    const float bwlo[8] = {-200.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f};
    applyOne(AdjustmentKind::BlackWhite, bwlo, nullptr, 1.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.0f, 1e-6f);

    // --- ChannelMixer ---------------------------------------------------------
    // Identity passes through.
    const float cmid[16] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::ChannelMixer, cmid, nullptr, 0.2f, 0.5f, 0.8f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.2f, 1e-6f);
    CHECK_NEAR(og, 0.5f, 1e-6f);
    CHECK_NEAR(ob, 0.8f, 1e-6f);
    // Channel rotate: Red<-Blue, Green<-Red, Blue<-Green.
    const float cmrot[16] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::ChannelMixer, cmrot, nullptr, 0.2f, 0.5f, 0.8f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.8f, 1e-6f);
    CHECK_NEAR(og, 0.2f, 1e-6f);
    CHECK_NEAR(ob, 0.5f, 1e-6f);
    // Constant offsets shift (clamped).
    const float cmc[16] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.1f, -0.1f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::ChannelMixer, cmc, nullptr, 0.2f, 0.5f, 0.95f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.3f, 1e-6f);
    CHECK_NEAR(og, 0.4f, 1e-6f);
    CHECK_NEAR(ob, 0.95f, 1e-6f);
    // Monochrome replicates the red row.
    const float cmmono[16] = {0.4f, 0.4f, 0.2f, 0.0f, 1.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    applyOne(AdjustmentKind::ChannelMixer, cmmono, nullptr, 0.2f, 0.5f, 0.8f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.44f, 1e-5f);
    CHECK_NEAR(og, 0.44f, 1e-5f);
    CHECK_NEAR(ob, 0.44f, 1e-5f);

    // --- ColorBalance ---------------------------------------------------------
    // Neutral balances are identity.
    const float cb0[16] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 1.0f};
    applyOne(AdjustmentKind::ColorBalance, cb0, nullptr, 0.3f, 0.6f, 0.1f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.3f, 1e-5f);
    CHECK_NEAR(og, 0.6f, 1e-5f);
    CHECK_NEAR(ob, 0.1f, 1e-5f);
    // Shadows Cyan-Red +100 with no preserve lifts black to red.
    const float cbs[16] = {100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::ColorBalance, cbs, nullptr, 0.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);
    CHECK_NEAR(og, 0.0f, 1e-6f);
    CHECK_NEAR(ob, 0.0f, 1e-6f);
    // Midtones +100 on 0.25 grey: the shadow zone weighs 0.5 but its
    // slider is 0.
    const float cbm[16] = {0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::ColorBalance, cbm, nullptr, 0.25f, 0.25f, 0.25f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.75f, 1e-5f);
    CHECK_NEAR(og, 0.25f, 1e-5f);
    CHECK_NEAR(ob, 0.25f, 1e-5f);
    // Highlights Yellow-Blue -50 on light grey.
    const float cbh[16] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, -50.0f, 0.0f};
    applyOne(AdjustmentKind::ColorBalance, cbh, nullptr, 0.6f, 0.6f, 0.6f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.6f, 1e-5f);
    CHECK_NEAR(og, 0.6f, 1e-5f);
    CHECK_NEAR(ob, 0.7f, 1e-5f);
    // Preserve on: midtones red +100 on mid grey lands on pure red.
    const float cbp[16] = {0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 1.0f};
    applyOne(AdjustmentKind::ColorBalance, cbp, nullptr, 0.5f, 0.5f, 0.5f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-4f);
    CHECK_NEAR(og, 0.0f, 1e-4f);
    CHECK_NEAR(ob, 0.0f, 1e-4f);

    // --- Recolour -------------------------------------------------------------
    // Grey keeps its HSL lightness and takes the target hue at full
    // saturation: HSL(120, 1, 0.5) is pure green.
    const float rec[16] = {120.0f, 1.0f, 0.0f};
    applyOne(AdjustmentKind::Recolour, rec, nullptr, 0.5f, 0.5f, 0.5f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.0f, 1e-5f);
    CHECK_NEAR(og, 1.0f, 1e-5f);
    CHECK_NEAR(ob, 0.0f, 1e-5f);
    // Black and white stay at the ends of the lightness ramp.
    applyOne(AdjustmentKind::Recolour, rec, nullptr, 0.0f, 0.0f, 0.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 0.0f, 1e-6f);
    CHECK_NEAR(og, 0.0f, 1e-6f);
    CHECK_NEAR(ob, 0.0f, 1e-6f);
    applyOne(AdjustmentKind::Recolour, rec, nullptr, 1.0f, 1.0f, 1.0f, or_,
             og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);
    CHECK_NEAR(og, 1.0f, 1e-6f);
    CHECK_NEAR(ob, 1.0f, 1e-6f);
    // Zero saturation is a grey at the source lightness, not black.
    const float rec_grey[16] = {120.0f, 0.0f, 0.0f};
    applyOne(AdjustmentKind::Recolour, rec_grey, nullptr, 0.25f, 0.25f, 0.25f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.25f, 1e-5f);
    CHECK_NEAR(og, 0.25f, 1e-5f);
    CHECK_NEAR(ob, 0.25f, 1e-5f);
    // Positive lightness blends the carried L toward white before the hue is
    // applied: L 0.5 + 0.5*(1-0.5) = 0.75 -> hsl(120, 1, 0.75) = (0.5, 1, 0.5).
    const float rec_light[16] = {120.0f, 1.0f, 0.5f};
    applyOne(AdjustmentKind::Recolour, rec_light, nullptr, 0.5f, 0.5f, 0.5f,
             or_, og, ob);
    CHECK_NEAR(or_, 0.5f, 1e-5f);
    CHECK_NEAR(og, 1.0f, 1e-5f);
    CHECK_NEAR(ob, 0.5f, 1e-5f);
    // Full lightness reaches white; negative blends toward black.
    const float rec_full[16] = {120.0f, 1.0f, 1.0f};
    applyOne(AdjustmentKind::Recolour, rec_full, nullptr, 0.5f, 0.5f, 0.5f,
             or_, og, ob);
    CHECK_NEAR(or_, 1.0f, 1e-6f);
    CHECK_NEAR(og, 1.0f, 1e-6f);
    CHECK_NEAR(ob, 1.0f, 1e-6f);

    // --- None is identity ------------------------------------------------------
    applyOne(AdjustmentKind::None, zero, nullptr, 0.1f, 0.2f, 0.3f, or_, og,
             ob);
    CHECK_NEAR(or_, 0.1f, 1e-6f);
    CHECK_NEAR(og, 0.2f, 1e-6f);
    CHECK_NEAR(ob, 0.3f, 1e-6f);
}

TEST_MAIN_CALL(test_adjust)
