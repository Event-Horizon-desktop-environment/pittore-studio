// Soft-proof manager over LCMS2: document -> proof -> display on float RGBA.
// Fixture profile is the vendored CC0 synthetic CMYK profile (see
// tests/proof_fixture.h); the test skips itself cleanly if it is absent.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/color/proof.h"
#include "proof_fixture.h"
#include "test_util.h"

using pittore::color::ProofIntent;
using pittore::color::ProofManager;

static const std::string kCmykProfile = pittore_test::cmykFixturePath();

static bool hasFixtureProfile() {
    return pittore_test::hasCmykFixture();
}

// 2x2 straight-alpha RGBA float tile: mid-gray, pure green (out of CMYK
// gamut), white, transparent black.
static std::vector<float> makeTile() {
    return {
        0.5f, 0.5f, 0.5f, 1.0f,  //
        0.0f, 1.0f, 0.0f, 1.0f,  //
        1.0f, 1.0f, 1.0f, 1.0f,  //
        0.0f, 0.0f, 0.0f, 0.0f,  //
    };
}

static void test_proof_unavailable_path() {
    ProofManager pm;
#ifndef PITTORE_LCMS2
    CHECK(!pm.available());
    CHECK_EQ(pm.cacheSize(), 0u);
    std::vector<float> tile = makeTile();
    CHECK(!pm.applyProof(tile.data(), tile.data(), 2, 2).empty());
    CHECK(!pm.setProofProfile(kCmykProfile).empty());
#else
    CHECK(pm.available());
#endif
}

#ifdef PITTORE_LCMS2
void test_proof_gray_stable() {
    if (!hasFixtureProfile()) {
        std::printf("  (skip: no fixture profile at %s)\n", kCmykProfile.c_str());
        return;
    }
    ProofManager pm;
    CHECK(pm.setProofProfile(kCmykProfile).empty());
    std::vector<float> tile = makeTile(), out = tile;
    CHECK(pm.applyProof(tile.data(), out.data(), 2, 2).empty());
    // In-gamut mid-gray survives the sRGB -> CMYK -> sRGB simulation close.
    CHECK_NEAR(out[0], 0.5f, 0.05f);
    CHECK_NEAR(out[1], 0.5f, 0.05f);
    CHECK_NEAR(out[2], 0.5f, 0.05f);
    CHECK_NEAR(out[3], 1.0f, 1e-6f);  // alpha passes through
    CHECK_NEAR(out[15], 0.0f, 1e-6f);
    CHECK_EQ(pm.cacheSize(), 1u);
    // Second run reuses the cached transform: no rebuild.
    CHECK(pm.applyProof(tile.data(), out.data(), 2, 2).empty());
    CHECK_EQ(pm.cacheSize(), 1u);
}

void test_proof_gamut_alarm() {
    if (!hasFixtureProfile()) {
        std::printf("  (skip: no fixture profile at %s)\n", kCmykProfile.c_str());
        return;
    }
    ProofManager pm;
    CHECK(pm.setProofProfile(kCmykProfile).empty());
    pm.setGamutCheck(true, 1.0f, 0.0f, 1.0f);  // magenta alarm
    std::vector<float> tile = makeTile(), out = tile;
    CHECK(pm.applyProof(tile.data(), out.data(), 2, 2).empty());
    // Pure sRGB green is out of CMYK gamut -> alarm color.
    CHECK_NEAR(out[4], 1.0f, 0.05f);
    CHECK_NEAR(out[5], 0.0f, 0.05f);
    CHECK_NEAR(out[6], 1.0f, 0.05f);
    // Same pixel without the alarm renders as some in-gamut color instead.
    pm.setGamutCheck(false);
    CHECK(pm.applyProof(tile.data(), out.data(), 2, 2).empty());
    const bool isAlarm =
        out[4] > 0.95f && out[5] < 0.05f && out[6] > 0.95f;
    CHECK(!isAlarm);
    for (int c = 4; c < 7; ++c) CHECK(out[c] >= 0.0f && out[c] <= 1.0f);
}

void test_proof_errors() {
    ProofManager pm;
    std::vector<float> tile = makeTile();
    CHECK(!pm.applyProof(tile.data(), tile.data(), 2, 2).empty());  // no proof
    CHECK(!pm.setProofProfile("/nonexistent/xx.icc").empty());
    CHECK(!pm.setSourceProfile("/nonexistent/xx.icc").empty());
    CHECK(!pm.applyProof(nullptr, tile.data(), 2, 2).empty());
    CHECK(!pm.applyProof(tile.data(), tile.data(), 0, 2).empty());
    // In-place (src == dst) is supported.
    if (hasFixtureProfile()) {
        CHECK(pm.setProofProfile(kCmykProfile).empty());
        CHECK(pm.applyProof(tile.data(), tile.data(), 2, 2).empty());
    }
}
#endif

static void test_proof() {
    test_proof_unavailable_path();
#ifdef PITTORE_LCMS2
    test_proof_gray_stable();
    test_proof_gamut_alarm();
    test_proof_errors();
#endif
}

#ifndef PITTORE_TEST_NO_MAIN
TEST_MAIN_CALL(test_proof)
#endif
