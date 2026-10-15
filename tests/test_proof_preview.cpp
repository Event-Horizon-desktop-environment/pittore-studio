// test_proof_preview.cpp — canvas soft-proof bridge: null/error paths are
// deterministic everywhere; the rendered-proof assertions need the vendored
// CC0 CMYK fixture and skip cleanly without it (same gate as test_proof.cpp).
// No widgets, no config.
#include <cstdio>
#include <filesystem>

#include <QImage>

#include "proof_fixture.h"
#include "test_util.h"
#include "engine/color/proof.h"
#include "ui/proof_preview.h"

using pittore::color::ProofManager;
using pittore::ui::proofPreviewImage;

namespace {

const std::string kCmykProfile = pittore_test::cmykFixturePath();

QImage solidImage(int w, int h, QRgb c) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

}  // namespace

int main() {
    ProofManager pm;
    // Null source and unconfigured manager always fall back to null.
    CHECK(proofPreviewImage(QImage(), pm).isNull());
    CHECK(proofPreviewImage(solidImage(4, 4, 0xFF808080), pm).isNull());

    if (!std::filesystem::exists(kCmykProfile)) {
        std::printf("  (skip: no fixture profile at %s)\n", kCmykProfile.c_str());
    } else {
        CHECK(pm.setProofProfile(kCmykProfile).empty());
        // Mid-gray survives the round trip close (sRGB -> CMYK -> sRGB).
        const QImage gray =
            proofPreviewImage(solidImage(8, 8, 0xFF808080), pm);
        CHECK(!gray.isNull());
        if (!gray.isNull()) {
            const QColor c = gray.pixelColor(4, 4);
            CHECK(std::abs(c.red() - 128) <= 14);
            CHECK(std::abs(c.green() - 128) <= 14);
            CHECK(std::abs(c.blue() - 128) <= 14);
        }
        // Transparent texels stay transparent (alpha rides through).
        QImage withHole = solidImage(8, 8, 0xFF808080);
        withHole.setPixel(2, 2, 0x00000000);
        const QImage holed = proofPreviewImage(withHole, pm);
        CHECK(!holed.isNull());
        if (!holed.isNull()) CHECK(holed.pixelColor(2, 2).alpha() == 0);
        // Gamut alarm: sRGB green is out of CMYK gamut -> magenta.
        pm.setGamutCheck(true, 1.0f, 0.0f, 1.0f);
        const QImage alarmed =
            proofPreviewImage(solidImage(8, 8, 0xFF00FF00), pm);
        CHECK(!alarmed.isNull());
        if (!alarmed.isNull()) {
            const QColor c = alarmed.pixelColor(4, 4);
            CHECK(c.red() > 200 && c.blue() > 200 && c.green() < 60);
        }
    }

    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return pittore_test::failures() == 0 ? 0 : 1;
}
