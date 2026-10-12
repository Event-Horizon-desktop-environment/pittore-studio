#include "lab_engine.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "engine/ai/bg_remove.h"
#include "ui/selection_mask.h"

namespace pittore::lab {
namespace {

// Straight RGBA8888 row-major buffer from a QImage (for the AI entry points).
std::vector<std::uint8_t> rgbaBuffer(const QImage& img) {
    const QImage im = img.convertToFormat(QImage::Format_RGBA8888);
    const int w = im.width(), h = im.height();
    std::vector<std::uint8_t> buf(std::size_t(w) * std::size_t(h) * 4u);
    for (int y = 0; y < h; ++y) {
        const uchar* row = im.constScanLine(y);
        std::memcpy(&buf[std::size_t(y) * w * 4u], row, std::size_t(w) * 4u);
    }
    return buf;
}

// Grayscale8 matte from float alpha [0,1] with an optional level band around
// the 0.5 decision point (matches the model-alpha → mask step).
QImage matteFromAlpha(const std::vector<float>& alpha, int w, int h,
                      double lo, double hi) {
    QImage m(w, h, QImage::Format_Grayscale8);
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    for (int y = 0; y < h; ++y) {
        uchar* row = m.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double v = (alpha[std::size_t(y) * w + x] - lo) / span;
            row[x] = uchar(std::clamp(int(v * 255.0 + 0.5), 0, 255));
        }
    }
    return m;
}

std::vector<float> alphaFromMatte(const QImage& m) {
    const QImage g =
        m.format() == QImage::Format_Grayscale8 ? m
                                                : m.convertToFormat(
                                                      QImage::Format_Grayscale8);
    const int w = g.width(), h = g.height();
    std::vector<float> a(std::size_t(w) * std::size_t(h));
    for (int y = 0; y < h; ++y) {
        const uchar* row = g.constScanLine(y);
        for (int x = 0; x < w; ++x)
            a[std::size_t(y) * w + x] = row[x] * (1.0f / 255.0f);
    }
    return a;
}

// Border band %, exactly as RefineDialog::refineBorderPx.
int borderBandPx(const QImage& mask, int percent) {
    if (percent <= 0) return 0;
    const QRect b = pittore::ui::selectionMaskBbox(mask);
    const int m = std::min(b.width(), b.height());
    return std::clamp(int(percent / 100.0 * m / 2.0 + 0.5), 1, 256);
}

// Cutout = straight RGB with the matte as the alpha channel.
QImage makeCutout(const QImage& rgbStraight, const QImage& matte) {
    const QImage s = rgbStraight.convertToFormat(QImage::Format_RGBA8888);
    const QImage g =
        matte.format() == QImage::Format_Grayscale8
            ? matte
            : matte.convertToFormat(QImage::Format_Grayscale8);
    const int w = s.width(), h = s.height();
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const uchar* srow = s.constScanLine(y);
        const uchar* grow = g.constScanLine(y);
        QRgb* orow = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = grow[x];
            orow[x] = qRgba(srow[x * 4 + 0], srow[x * 4 + 1], srow[x * 4 + 2], a);
        }
    }
    return out;
}

}  // namespace

QImage LabEngine::segment(const QImage& img, const LabParams& p,
                          QString* error,
                          std::vector<float>* rawAlpha) const {
    if (img.isNull()) {
        if (error) *error = QStringLiteral("No image loaded.");
        return {};
    }
    if (p.modelPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("No model chosen — pick a .onnx first.");
        return {};
    }
    if (!pittore::ai::onnx_available()) {
        if (error)
            *error = QStringLiteral("This build links no ONNX Runtime.");
        return {};
    }
    const int w = img.width(), h = img.height();
    std::vector<std::uint8_t> buf = rgbaBuffer(img);
    pittore::ai::SegmentResult res;
    try {
        res = pittore::ai::segment_rgba8(buf.data(), w, h,
                                         p.modelPath.toStdString(),
                                         p.inputSize);
    } catch (const std::exception& e) {
        if (error) *error = QStringLiteral("Segment exception: %1").arg(e.what());
        return {};
    }
    if (!res.ok) {
        if (error) *error = QString::fromStdString(res.error);
        return {};
    }
    if (res.alpha.size() != std::size_t(w) * std::size_t(h)) {
        if (error) *error = QStringLiteral("Model returned a wrong-sized mask.");
        return {};
    }
    if (rawAlpha) *rawAlpha = res.alpha;
    return matteFromAlpha(res.alpha, w, h, p.segmentLevelLo, p.segmentLevelHi);
}

RunResult LabEngine::refine(const QImage& img, const QImage& seed,
                            const LabParams& p) const {
    RunResult r;
    if (img.isNull() || seed.isNull() ||
        img.size() != seed.size()) {
        r.error = QStringLiteral("Refine needs an image and a matching seed matte.");
        return r;
    }
    QElapsedTimer t;
    t.start();
    const QImage comp = img.convertToFormat(QImage::Format_ARGB32);
    const int w = comp.width(), h = comp.height();
    QImage work =
        seed.format() == QImage::Format_Grayscale8
            ? seed
            : seed.convertToFormat(QImage::Format_Grayscale8);

    auto capture = [&](const char* name, const QImage& m) {
        r.stages.append({QString::fromLatin1(name), m});
    };

    if (p.useSpeckles) {
        work = pittore::ui::selectionMaskRemoveSpeckles(work, p.speckleMinPx);
        capture("Remove speckles", work);
    }
    if (p.useDecay) {
        work = pittore::ui::selectionMaskDecayWash(work, comp,
                                                   p.decaySupportPx, p.decayTexTol);
        capture("Decay wash", work);
    }
    // AI refine runs BEFORE the edge solve: guided growth recovers faint
    // strands but also bleeds a soft skirt around solid edges, and the
    // colour-honest snap below trims exactly that skirt (background-coloured
    // pixels solve to ~0) while keeping hair strands (far from the backdrop
    // model). Snap-then-AI left the skirt as the last word on the edge.
    if (p.useRefineAi) {
        std::vector<std::uint8_t> buf = rgbaBuffer(img);
        std::vector<float> alpha = alphaFromMatte(work);
        pittore::ai::guided_refine_alpha(buf.data(), w, h, alpha,
                                         p.refineRadius,
                                         float(p.refineEps),
                                         float(p.refineLevelLo),
                                         float(p.refineLevelHi));
        work = matteFromAlpha(alpha, w, h, 0.0, 1.0);
        capture("AI refine (guided)", work);
    }
    if (p.useSmooth && p.smoothPasses() > 0) {
        work = pittore::ui::selectionMaskSmooth(work, p.smoothPasses());
        capture("Smooth", work);
    }
    if (p.useSnap) {
        const int band =
            p.snapBandPx > 0 ? p.snapBandPx : borderBandPx(work, p.borderPct);
        if (band > 0) {
            if (p.smartRadius && band >= 4)
                work = pittore::ui::selectionMaskSnapSmart(
                    comp, work, std::max(1, band / std::max(1, p.snapNarrowDiv)),
                    band, p.snapPasses);
            else
                work = pittore::ui::selectionMaskSnapToEdges(comp, work, band,
                                                             p.snapPasses);
            capture(p.smartRadius ? "Snap (smart radius)" : "Snap (edges)", work);
        }
    }
    if (p.useFillHoles) {
        work = pittore::ui::selectionMaskFillSmallHoles(work,
                                                        p.fillHolesMaxPx);
        capture("Fill small holes", work);
    }
    if (p.useFeather && p.featherPx > 0) {
        work = pittore::ui::selectionMaskFeather(work, p.featherPx);
        capture("Feather", work);
    }
    if (p.useShift && p.shiftPx() != 0) {
        work = pittore::ui::selectionMaskGrow(work, p.shiftPx());
        capture("Shift", work);
    }
    if (p.useRamp && p.rampPct != 0) {
        if (p.rampPct > 0) {
            work = pittore::ui::selectionMaskRamp(work, 1.0 + 3.0 * p.rampPct / 100.0);
        } else {
            const int growPx =
                std::max(1, int(-p.rampPct / 100.0 * 24 + 0.5));
            work = pittore::ui::selectionMaskGrow(work, growPx);
        }
        capture("Ramp", work);
    }

    // Decontaminate the RGB, then bind the matte as alpha.
    QImage rgb = img.convertToFormat(QImage::Format_RGBA8888);
    if (p.decontam) {
        pittore::ui::decontaminateStraightRgba(rgb, work, p.decontamRadius,
                                                       p.decontamAmount);
        capture("Decontam (RGB only)", work);
    }
    r.cutout = makeCutout(rgb, work);
    r.matte = work;
    r.original = img;
    r.ok = true;
    r.ms = t.elapsed() / 1000.0;
    return r;
}

}  // namespace pittore::lab
