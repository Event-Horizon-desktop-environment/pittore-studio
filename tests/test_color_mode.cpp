// test_color_mode.cpp — Image > Mode through AppState: the colorMode tag
// lifecycle, Rec.709 grayscale luma, the CMYK separation round trip,
// undo/redo restoring pixels AND the tag/icc fields, live-text colours,
// canvas paper, the 16-bit depth suffix and the no-op guard.
//
// Profile control is deterministic: handing the document its own ICC bytes
// (the system CMYK fixture, when present) pins the conversion to the
// profiled path; without the fixture the resolver falls back to a settings
// or system probe and then the naive full-GCR core — every assertion below
// holds for all three. Runs headless under QCoreApplication; the meson
// XDG_CONFIG_HOME sandbox keeps the real Settings.toml out of the way.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "engine/color/icc_convert.h"
#include "engine/core/log.h"
#include "engine/compute/warp.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/color_mode.h"

using namespace pittore::ui;

namespace {

QImage solid(int w, int h, QRgb c) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

bool isRed(const QColor& c) {
    return c.red() > 250 && c.green() < 5 && c.blue() < 5;
}

QByteArray fileBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// Usable = LCMS2 opens it as a CMYK profile whose encoder self-test passes.
bool usable(const QByteArray& bytes) {
    if (bytes.isEmpty()) return false;
    const pittore::color::SrgbToCmyk probe(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        static_cast<std::size_t>(bytes.size()));
    return probe.valid();
}

}  // namespace

static void test_color_mode() {
    AppState state;
    DocumentItem* d =
        state.addDocument(QStringLiteral("mode"), QSize(32, 24), 300);
    CHECK(d != nullptr);
    if (!d) return;

    d->canvasPaper = QColor(255, 0, 0);
    state.placeImageLayer(solid(8, 8, 0xFFFF0000), QStringLiteral("red"),
                          QPointF(8, 8), 1.0);   // covers x4..11, y4..11
    state.placeImageLayer(solid(8, 8, 0xFFFFFFFF), QStringLiteral("white"),
                          QPointF(24, 8), 1.0);  // covers x20..27, y4..11
    CHECK_EQ(d->layers.size(), 3);
    CHECK(d->colorMode == QStringLiteral("RGB/8"));

    // --- Grayscale: Rec.709 luma into all three channels ------------------
    CHECK(state.convertDocumentMode(AppState::ModeTarget::Grayscale));
    CHECK(d->colorMode == QStringLiteral("Grayscale/8"));
    {
        const QColor g = d->composite.pixelColor(6, 6);
        CHECK(g.red() >= 52 && g.red() <= 56);  // 0.2126 * 255 ≈ 54
        CHECK(g.green() == g.red());
        CHECK(g.blue() == g.red());
        const QColor w = d->composite.pixelColor(22, 6);  // white stays white
        CHECK(w.red() >= 250 && w.green() >= 250 && w.blue() >= 250);
        const QColor p = d->canvasPaper;  // paper rides along
        CHECK(p.red() >= 52 && p.red() <= 56);
        CHECK(p.green() == p.red());
    }
    // Undo restores tag, pixels AND paper; redo re-applies the tag.
    state.undo();
    CHECK(d->colorMode == QStringLiteral("RGB/8"));
    CHECK(isRed(d->composite.pixelColor(6, 6)));
    CHECK_EQ(d->canvasPaper.red(), 255);
    state.redo();
    CHECK(d->colorMode == QStringLiteral("Grayscale/8"));
    state.undo();
    CHECK(d->colorMode == QStringLiteral("RGB/8"));

    // --- CMYK: separation round trip --------------------------------------
    const char* kGs = "/usr/share/ghostscript/iccprofiles/default_cmyk.icc";
    const bool fixture = QFileInfo::exists(kGs);
    if (fixture) {
        d->iccProfile = fileBytes(kGs);
        CHECK(usable(d->iccProfile));
    }
    CHECK(state.convertDocumentMode(AppState::ModeTarget::Cmyk));
    CHECK(d->colorMode == QStringLiteral("CMYK/8"));
    if (!d->iccProfile.isEmpty()) CHECK(usable(d->iccProfile));
    {
        const QColor w = d->composite.pixelColor(22, 6);
        CHECK(w.red() > 215 && w.green() > 215 && w.blue() > 215);
        const QColor r = d->composite.pixelColor(6, 6);
        CHECK(r.red() > r.green() + 90 && r.red() > r.blue() + 90);
        CHECK(d->canvasPaper.red() > d->canvasPaper.green() + 60);
    }

    // Normal build-up and wash brush strokes edit the device inks directly.
    // Finalizing before commit keeps their redo snapshots valid too.
    {
        state.setActiveTool(ToolId::Brush);
        for (const QString& paintMode : {QStringLiteral("buildup"),
                                         QStringLiteral("wash")}) {
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"), paintMode);
            state.beginStrokeState(ToolId::Brush, 17);
            state.beginUndoStep();
            state.copyOnWriteActiveLayer();
            LayerItem* painted = state.activeLayer();
            CHECK(painted && painted->cmykPixels && painted->cmykPixels->valid());
            const QPointF originalOffset = painted ? painted->offset : QPointF();
            if (painted && paintMode == QStringLiteral("wash"))
                painted->offset = QPointF(0, 0);  // identity target arms wash scratch
            const QPointF dabPosition = paintMode == QStringLiteral("wash")
                                            ? QPointF(2.5, 2.5)
                                            : QPointF(22.5, 6.5);
            CHECK(state.paintDab(dabPosition, 1.0, 1.0, 1.0,
                                 QColor(0, 0, 255)));
            state.flushPaint();
            if (paintMode == QStringLiteral("wash")) {
                state.bakeWashStroke();
                if (painted) painted->offset = originalOffset;
                d->rebuildComposite();
            }
            state.endStrokeState();
            state.commitUndoStep(QStringLiteral("CMYK brush"), QString());
            if (painted && painted->cmykPixels) {
                const auto profile = resolveCmykProfile(*d);
                pittore::color::SrgbToCmyk separation(
                    reinterpret_cast<const std::uint8_t*>(profile.constData()),
                    static_cast<std::size_t>(profile.size()));
                float expected[4];
                separation.convert(0.0f, 0.0f, 1.0f, expected);
                const auto& ink = painted->cmykPixels->pixels[
                    2 * painted->pixels->width() + 2];
                for (int c = 0; c < 4; ++c)
                    CHECK(std::abs(int(ink[c]) -
                                   int(std::lround(expected[c] * 65535.0f))) <= 2);
                CHECK(painted->cmykPixels->appearanceFingerprint ==
                      pittore::color::CmykImage::fingerprint(*painted->pixels));
            }
            state.undo();
            state.redo();
            painted = state.activeLayer();
            CHECK(painted && painted->cmykPixels &&
                  painted->cmykPixels->appearanceFingerprint ==
                      pittore::color::CmykImage::fingerprint(*painted->pixels));
            state.undo();  // later mode-history checks start at their baseline
        }
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_painting_mode"),
                        QStringLiteral("wash"));
        state.beginStrokeState(ToolId::Brush, 19);
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        LayerItem* painted = state.activeLayer();
        const QPointF originalOffset = painted->offset;
        painted->offset = QPointF(0, 0);
        pittore::compute::StampTip tip;
        tip.w = tip.h = 1;
        tip.alpha = {1.0f};
        tip.color = true;
        tip.red = {0.0f};
        tip.green = {1.0f};
        tip.blue = {0.0f};
        CHECK(state.stampTipDab(QPointF(2.5, 2.5), 1.0, 1, 1.0,
                                QColor(0, 255, 0), 0.0, tip));
        state.flushPaint();
        state.bakeWashStroke();
        painted->offset = originalOffset;
        d->rebuildComposite();
        state.endStrokeState();
        state.commitUndoStep(QStringLiteral("CMYK stamp"), QString());
        const auto profile = resolveCmykProfile(*d);
        pittore::color::SrgbToCmyk separation(
            reinterpret_cast<const std::uint8_t*>(profile.constData()),
            static_cast<std::size_t>(profile.size()));
        float expected[4];
        separation.convert(0.0f, 1.0f, 0.0f, expected);
        const auto& stampInk = painted->cmykPixels->pixels[
            2 * painted->pixels->width() + 2];
        for (int c = 0; c < 4; ++c)
            CHECK(std::abs(int(stampInk[c]) -
                           int(std::lround(expected[c] * 65535.0f))) <= 2);
        state.undo();

        state.setActiveTool(ToolId::PaintBucket);
        state.setForeground(QColor(255, 0, 255));
        state.setOption(ToolId::PaintBucket, QStringLiteral("tolerance"), 0.0);
        state.setOption(ToolId::PaintBucket, QStringLiteral("contiguous"), true);
        state.setOption(ToolId::PaintBucket, QStringLiteral("antialias"), false);
        state.setOption(ToolId::PaintBucket, QStringLiteral("opacity"), 100.0);
        CHECK(state.paintBucketAt(QPointF(22.5, 6.5)));
        painted = state.activeLayer();
        const auto bucketProfile = resolveCmykProfile(*d);
        pittore::color::SrgbToCmyk bucketSeparation(
            reinterpret_cast<const std::uint8_t*>(bucketProfile.constData()),
            static_cast<std::size_t>(bucketProfile.size()));
        bucketSeparation.convert(1.0f, 0.0f, 1.0f, expected);
        const auto& bucketInk = painted->cmykPixels->pixels[
            2 * painted->pixels->width() + 2];
        for (int c = 0; c < 4; ++c)
            CHECK(std::abs(int(bucketInk[c]) -
                           int(std::lround(expected[c] * 65535.0f))) <= 2);
        CHECK(painted->cmykPixels->appearanceFingerprint ==
              pittore::color::CmykImage::fingerprint(*painted->pixels));
        state.undo();

        state.setActiveTool(ToolId::ColorReplacement);
        state.setForeground(QColor(0, 255, 255));
        state.beginStrokeState(ToolId::ColorReplacement, 23);
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        painted = state.activeLayer();
        const std::size_t replaceIndex = 2 * painted->pixels->width() + 2;
        const auto originalInk = painted->cmykPixels->pixels[replaceIndex];
        const std::vector<pittore::RGBAf> targets{
            painted->pixels->data()[replaceIndex]};
        CHECK(state.replaceColorDab(QPointF(22.5, 6.5), 1.0, 1.0, 3,
                                    targets, 0.0, 0, false, 0.0));
        state.flushPaint();
        state.endStrokeState();
        state.commitUndoStep(QStringLiteral("CMYK replace"), QString());
        CHECK(painted->cmykPixels->pixels[replaceIndex] != originalInk);
        CHECK(painted->cmykPixels->appearanceFingerprint ==
              pittore::color::CmykImage::fingerprint(*painted->pixels));
        state.undo();

        // Fill Selection's source-over result must be represented in native
        // inks directly, with undo restoring the previous samples.
        painted = state.activeLayer();
        const std::size_t fillIndex = 2 * painted->pixels->width() + 2;
        const auto priorFillInk = painted->cmykPixels->pixels[fillIndex];
        CHECK(state.fillActiveSelectionWithColor(
            QColor(0, 128, 255), QStringLiteral("CMYK fill")));
        painted = state.activeLayer();
        const auto fillProfile = resolveCmykProfile(*d);
        pittore::color::SrgbToCmyk fillSeparation(
            reinterpret_cast<const std::uint8_t*>(fillProfile.constData()),
            static_cast<std::size_t>(fillProfile.size()));
        fillSeparation.convert(0.0f, 128.0f / 255.0f, 1.0f, expected);
        const auto& fillInk = painted->cmykPixels->pixels[fillIndex];
        for (int c = 0; c < 4; ++c)
            CHECK(std::abs(int(fillInk[c]) -
                           int(std::lround(expected[c] * 65535.0f))) <= 2);
        CHECK(painted->cmykPixels->appearanceFingerprint ==
              pittore::color::CmykImage::fingerprint(*painted->pixels));
        state.undo();
        painted = state.activeLayer();
        CHECK(painted && painted->cmykPixels->pixels[fillIndex] ==
                             priorFillInk);

        // RGB-domain tonal editing re-separates only the dab region, keeping
        // untouched imported device values intact.
        painted = state.activeLayer();
        const auto outsideToneInk = painted->cmykPixels->pixels[0];
        state.beginUndoStep();
        state.copyOnWriteActiveLayer();
        CHECK(state.toneDab(QPointF(22.5, 6.5), 2.0, 0.5, 0.7, 0, 2,
                            false, false));
        state.endToneStroke();
        painted = state.activeLayer();
        CHECK(painted->cmykPixels->appearanceFingerprint ==
              pittore::color::CmykImage::fingerprint(*painted->pixels));
        CHECK(painted->cmykPixels->pixels[0] == outsideToneInk);
        state.commitUndoStep(QStringLiteral("CMYK tone dab"), QString());
        state.undo();

        // Liquify resamples only its dab region; cancellation must restore the
        // exact original CMYK samples instead of converting RGB back to inks.
        painted = state.activeLayer();
        const auto priorLiquifyInks = painted->cmykPixels->pixels;
        CHECK(state.beginLiquifyStroke());
        pittore::compute::WarpMesh warp = pittore::compute::make_warp_mesh(
            painted->pixels->width(), painted->pixels->height());
        for (std::size_t i = 0; i < warp.offsets.size(); i += 2)
            warp.offsets[i] = 1.25f;
        CHECK(state.liquifyDab(warp, 0, 0,
                               static_cast<int>(painted->pixels->width()),
                               static_cast<int>(painted->pixels->height())));
        state.cancelLiquifyStroke();
        painted = state.activeLayer();
        CHECK(painted && painted->cmykPixels &&
              painted->cmykPixels->pixels == priorLiquifyInks);

        // A filter preview is reversible without throwing away imported or
        // previously edited device inks. Accepting one refreshes the native
        // separation to match the filtered appearance.
        painted = state.activeLayer();
        CHECK(painted && painted->cmykPixels);
        if (painted && painted->pixels && painted->cmykPixels) {
            const auto beforeAlphaInks = painted->cmykPixels->pixels;
            std::vector<float> alpha(painted->pixels->pixel_count(), 0.5f);
            CHECK(state.applyActiveLayerAlpha(
                alpha.data(), static_cast<int>(painted->pixels->width()),
                static_cast<int>(painted->pixels->height()),
                QStringLiteral("CMYK alpha"), QString()));
            painted = state.activeLayer();
            CHECK(painted && painted->cmykPixels &&
                  painted->cmykPixels->pixels == beforeAlphaInks);
            state.undo();
            painted = state.activeLayer();

            const auto originalInks = painted->cmykPixels;
            const auto originalPixel = painted->pixels->data()[0];
            CHECK(state.beginTonalEdit() != nullptr);
            painted = state.activeLayer();
            CHECK(state.applyFilterToActiveLayer(
                "gaussian_blur", {2.0}));
            state.cancelTonalEdit();
            painted = state.activeLayer();
            CHECK(painted && painted->cmykPixels &&
                  painted->cmykPixels->pixels == originalInks->pixels);
            if (painted && painted->pixels)
                CHECK(std::memcmp(&painted->pixels->data()[0], &originalPixel,
                                  sizeof(originalPixel)) == 0);

            CHECK(state.beginTonalEdit() != nullptr);
            CHECK(state.applyFilterToActiveLayer("gaussian_blur", {2.0}));
            state.commitTonalEdit(QStringLiteral("CMYK filter"), QString());
            painted = state.activeLayer();
            CHECK(painted && painted->cmykPixels &&
                  painted->cmykPixels->appearanceFingerprint ==
                      pittore::color::CmykImage::fingerprint(*painted->pixels));
            state.undo();
        }
    }

    // --- CMYK -> RGB: retag only (appearance pixels already ARE the RGB) --
    const QColor rAfterCmyk = d->composite.pixelColor(6, 6);
    CHECK(state.convertDocumentMode(AppState::ModeTarget::Rgb));
    CHECK(d->colorMode == QStringLiteral("RGB/8"));
    CHECK(d->iccProfile.isEmpty());  // a stale separation never rides along
    CHECK(d->composite.pixelColor(6, 6) == rAfterCmyk);

    // Undo the retag: back to CMYK with the profile bytes; undo the
    // separation itself: back to RGB with the pre-conversion bytes.
    state.undo();
    CHECK(d->colorMode == QStringLiteral("CMYK/8"));
    if (fixture) CHECK(usable(d->iccProfile));
    state.undo();
    CHECK(d->colorMode == QStringLiteral("RGB/8"));
    if (fixture)
        CHECK(!d->iccProfile.isEmpty());  // snapshot carried them
    else
        CHECK(d->iccProfile.isEmpty());
    CHECK(isRed(d->composite.pixelColor(6, 6)));
    CHECK(isRed(d->canvasPaper));

    // --- The depth suffix survives a conversion ---------------------------
    d->colorMode = QStringLiteral("RGB/16");
    CHECK(state.convertDocumentMode(AppState::ModeTarget::Grayscale));
    CHECK(d->colorMode == QStringLiteral("Grayscale/16"));
    state.undo();
    CHECK(d->colorMode == QStringLiteral("RGB/16"));
    // RGB/32 keeps its suffix on a no-op and clamps to /8 on CMYK (there is
    // no 32-bit CMYK tag, and /32 already exported at 8 anyway).
    d->colorMode = QStringLiteral("RGB/32");
    CHECK(!state.convertDocumentMode(AppState::ModeTarget::Rgb));
    CHECK(state.convertDocumentMode(AppState::ModeTarget::Cmyk));
    CHECK(d->colorMode == QStringLiteral("CMYK/8"));
    state.undo();
    CHECK(d->colorMode == QStringLiteral("RGB/32"));
    d->colorMode = QStringLiteral("RGB/8");

    // --- No-op guard: RGB -> RGB adds no history --------------------------
    const int depth = state.undoDepth();
    CHECK(!state.convertDocumentMode(AppState::ModeTarget::Rgb));
    CHECK_EQ(state.undoDepth(), depth);

    // --- Live text colours ride the conversion (value copies) -------------
    const int ti =
        state.addTextLayer(QPointF(4, 16), 0.0, QStringLiteral("Liberation Sans"),
                           12.0, QColor(255, 0, 0));
    CHECK(ti >= 0);
    if (ti >= 0) {
        CHECK(state.convertDocumentMode(AppState::ModeTarget::Grayscale));
        const QColor tc = d->layers[ti].textSpec.color;
        CHECK(tc.red() >= 52 && tc.red() <= 56);
        CHECK(tc.green() == tc.red());
        CHECK(tc.blue() == tc.red());
        state.undo();
        CHECK(isRed(d->layers[ti].textSpec.color));
        CHECK(d->colorMode == QStringLiteral("RGB/8"));
    }

    // --- Resolver sanity: whatever comes back is a live CMYK profile ------
    {
        const QByteArray bytes = resolveCmykProfile(*d);
        if (!bytes.isEmpty()) CHECK(usable(bytes));
        const QString path = defaultCmykProfilePath();
        if (!path.isEmpty()) CHECK(QFileInfo::exists(path));
    }

    // No active document -> no conversion, no crash.
    AppState bare;
    CHECK(!bare.convertDocumentMode(AppState::ModeTarget::Cmyk));
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-color-mode-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_color_mode();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
