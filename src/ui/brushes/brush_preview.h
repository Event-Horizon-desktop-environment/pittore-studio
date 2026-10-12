#pragma once
// Brush preview rendering: honest WYSIWYG thumbnails and scratch strokes
// painted with the real dab kernels (auto tip + stamp), so the preview can
// never drift from what the canvas puts down. Our own design: a tip dab on
// a light checker plus a pressure-ramp stroke on paper white.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QWidget>

#include "engine/compute/brushes/dab/dab.h"
#include "engine/compute/brushes/smudge/smudge.h"
#include "engine/compute/brushes/stamp/stamp.h"
#include "ui/brushes/pressure_curve.h"
#include "engine/core/pixel.h"

namespace pittore::ui::brushpreview {

struct AutoParams {
    double ratio = 1.0;
    double angleDeg = 0.0;
    bool square = false;
    double hardness = 0.5;  // 0..1
    double spacingPct = 15.0;
    bool spacingAuto = false;
    double spacingAutoCoeff = 0.0;
    bool pressureSize = true;
    bool pressureOpacity = true;
    double opacity = 1.0;  // 0..1
    double flowPct = 1.0;  // 0..1
    // Authored pressure response ("value|x,y;..." or empty = built-in).
    QString sizeCurve, opacityCurve, flowCurve;
    int spikes = 0;
    double fadeAniso = 0.0;  // -100..100, 0 isotropic
    int falloff = 0;
    double sharpness = 0.0;  // 0..100
    double soften = 0.0;     // 0..100
    // Dab composite op for pigment-mix presets (normal default).
    pittore::compute::dabmix::DabMixOp mixOp =
        pittore::compute::dabmix::DabMixOp::Normal;
};

namespace detail {

inline void paintChecker(QImage& img) {
    QPainter p(&img);
    const int cell = 8;
    for (int y = 0; y < img.height(); y += cell)
        for (int x = 0; x < img.width(); x += cell)
            p.fillRect(x, y, cell, cell,
                       ((x / cell + y / cell) % 2) ? QColor(235, 235, 235)
                                                  : QColor(255, 255, 255));
}

// Float buffer -> QImage over an existing background (straight-alpha over).
// With keepAlpha the destination alpha is composited too, so callers can
// render onto transparency instead of paper.
inline void blitOver(const std::vector<pittore::RGBAf>& buf, int w, int h,
                     QImage& img, bool keepAlpha = false) {
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const pittore::RGBAf& s = buf[std::size_t(y) * w + x];
            const QColor bg = QColor::fromRgba(row[x]);
            const float a = std::clamp(s.a, 0.0f, 1.0f);
            const int r = int(s.r * a * 255.0f + bg.red() * (1.0f - a));
            const int g = int(s.g * a * 255.0f + bg.green() * (1.0f - a));
            const int b = int(s.b * a * 255.0f + bg.blue() * (1.0f - a));
            if (!keepAlpha) {
                row[x] = qRgb(std::clamp(r, 0, 255), std::clamp(g, 0, 255),
                              std::clamp(b, 0, 255));
                continue;
            }
            const float bgA = bg.alphaF();
            const float outA = a + bgA * (1.0f - a);
            if (outA <= 1e-6f) {
                row[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            row[x] = qRgba(
                std::clamp(int((s.r * a + bg.redF() * bgA * (1.0f - a)) /
                               outA * 255.0f),
                           0, 255),
                std::clamp(int((s.g * a + bg.greenF() * bgA * (1.0f - a)) /
                               outA * 255.0f),
                           0, 255),
                std::clamp(int((s.b * a + bg.blueF() * bgA * (1.0f - a)) /
                               outA * 255.0f),
                           0, 255),
                std::clamp(int(outA * 255.0f), 0, 255));
        }
    }
}

inline float sizeMult(double p, bool enabled) {
    if (!enabled) return 1.0f;
    const float pc = float(std::clamp(p, 0.0, 1.0));
    return 0.15f + 0.85f * pc * std::sqrt(pc);
}

// Authored curve or built-in fallback.
inline float curveOr(const pittore::ui::brushcurve::Curve& c, double p,
                     float legacy) {
    if (!c.has()) return legacy;
    return float(std::clamp(pittore::ui::brushcurve::eval(c, p), 0.0, 1.0));
}

inline pittore::compute::AutoTip toTip(const AutoParams& a) {
    pittore::compute::AutoTip tip;
    tip.silhouette = a.square ? pittore::compute::TipSilhouette::Square
                              : pittore::compute::TipSilhouette::Round;
    tip.ratio = float(std::clamp(a.ratio, 0.01, 1.0));
    tip.angleDeg = float(a.angleDeg);
    tip.hardness = float(std::clamp(a.hardness, 0.0, 1.0));
    tip.spacingPct = float(std::clamp(a.spacingPct, 1.0, 200.0));
    tip.spikes = std::clamp(a.spikes, 0, 12);
    tip.fadeAniso = float(std::clamp(a.fadeAniso / 100.0, -1.0, 1.0));
    tip.falloff = (a.falloff == 1) ? 1 : 0;
    tip.sharpness = float(std::clamp(a.sharpness / 100.0, 0.0, 1.0));
    tip.soften = float(std::clamp(a.soften / 100.0, 0.0, 1.0));
    tip.autoSpacing = a.spacingAuto;
    tip.spacingAutoCoeff = float(std::clamp(a.spacingAutoCoeff, 0.0, 8.0));
    tip.sanitize();
    return tip;
}

inline pittore::compute::StampMode toStampMode(int mode) {
    using pittore::compute::StampMode;
    if (mode == 1) return StampMode::ColorImage;
    if (mode == 2) return StampMode::LightnessMap;
    if (mode == 3) return StampMode::GradientMap;
    return StampMode::AlphaMask;
}

inline pittore::compute::StampLevels toStampLevels(double neutralPct,
                                                   double brightnessPct,
                                                   double contrastPct) {
    pittore::compute::StampLevels l;
    l.neutral = float(std::clamp(neutralPct / 100.0, 0.0, 1.0));
    l.brightness = float(std::clamp(brightnessPct / 100.0, -1.0, 1.0));
    l.contrast = float(std::clamp(contrastPct / 100.0, 0.0, 4.0));
    return l;
}

}  // namespace detail

// Stylus lean direction in degrees, measured from straight-down (pen toward
// the viewer): 0 when the pen is near-upright, otherwise the lean azimuth.
// Drives tip rotation for tools that opt in; mice sit at exactly 0.
inline double tiltRotationOffset(double xTiltDeg, double yTiltDeg) {
    if (std::hypot(xTiltDeg, yTiltDeg) < 3.0) return 0.0;
    double deg = std::atan2(xTiltDeg, yTiltDeg) * 180.0 / 3.141592653589793;
    while (deg > 180.0) deg -= 360.0;
    while (deg <= -180.0) deg += 360.0;
    return deg;
}

// One dab, longest side ~72% of the square, on a checker.
inline QImage tipThumbAuto(const AutoParams& a, int px = 56,
                         bool paper = true) {
    QImage img(px, px, QImage::Format_ARGB32);
    if (paper)
        detail::paintChecker(img);
    else
        img.fill(Qt::transparent);
    std::vector<pittore::RGBAf> buf(std::size_t(px) * px,
                                     pittore::RGBAf{0, 0, 0, 0});
    const auto tip = detail::toTip(a);
    pittore::compute::paint_tip_dab_host(
        buf.data(), px, px, px / 2.0f, px / 2.0f, px * 0.36f, tip, 1.0f,
        pittore::RGBAf{0, 0, 0, 1}, nullptr, nullptr, nullptr, nullptr,
        nullptr, a.mixOp);
    detail::blitOver(buf, px, px, img, !paper);
    return img;
}

inline QImage tipThumbStamp(const pittore::compute::StampTip& tip, int mode,
                            int px = 56, bool paper = true,
                            const QColor& bg = QColor(),
                            double neutralPct = 50.0,
                            double brightnessPct = 0.0,
                            double contrastPct = 100.0,
                            pittore::compute::dabmix::DabMixOp mix =
                                pittore::compute::dabmix::DabMixOp::Normal) {
    QImage img(px, px, QImage::Format_ARGB32);
    if (paper)
        detail::paintChecker(img);
    else
        img.fill(Qt::transparent);
    if (!tip.valid()) return img;
    std::vector<pittore::RGBAf> buf(std::size_t(px) * px,
                                     pittore::RGBAf{0, 0, 0, 0});
    const auto smode = detail::toStampMode(mode);
    const auto levels =
        detail::toStampLevels(neutralPct, brightnessPct, contrastPct);
    const pittore::RGBAf white{1, 1, 1, 1};
    const pittore::RGBAf bgC{float(bg.redF()), float(bg.greenF()),
                              float(bg.blueF()), 1.0f};
    pittore::compute::stamp_dab_host(
        buf.data(), px, px, px / 2.0f, px / 2.0f, px * 0.36f, tip, smode, 1.0f,
        pittore::RGBAf{0, 0, 0, 1}, 0.0f, nullptr, nullptr, nullptr,
        nullptr, 0, 1, &levels, bg.isValid() ? &bgC : &white, nullptr, 0.0f,
        mix);
    detail::blitOver(buf, px, px, img, !paper);
    return img;
}

// Scratch stroke: an S-curve with pressure swelling mid-stroke, the same
// size/opacity response the canvas uses. Paper white, foreground ink.
template <typename DabFn>
QImage strokeCanvas(int w, int h, DabFn&& dabAt, bool paper = true) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(paper ? Qt::white : Qt::transparent);
    std::vector<pittore::RGBAf> buf(std::size_t(w) * h,
                                     pittore::RGBAf{0, 0, 0, 0});
    const double amp = h * 0.22;
    double lastX = 8.0, lastY = h / 2.0;
    double since = 1e9;
    for (double x = 8.0; x <= w - 8.0; x += 1.0) {
        const double t = (x - 8.0) / (w - 16.0);
        const double y = h / 2.0 + amp * std::sin(t * 2.0 * 3.141592653589793);
        const double pressure = 0.12 + 0.88 * std::pow(std::sin(t * 3.141592653589793), 0.7);
        const double dist = std::hypot(x - lastX, y - lastY);
        since += dist;
        const float need = dabAt(buf.data(), w, h, float(x), float(y), pressure, true);
        if (since >= need) {
            dabAt(buf.data(), w, h, float(x), float(y), pressure, false);
            since = 0.0;
        }
        lastX = x;
        lastY = y;
    }
    detail::blitOver(buf, w, h, img, !paper);
    return img;
}

inline QImage strokePreviewAuto(const AutoParams& a, const QColor& fg,
                                int w = 300, int h = 110,
                                const pittore::compute::PatternTex* tex = nullptr,
                                bool paper = true,
                                const pittore::compute::papergrain::PaperGrain* kgrain =
                                    nullptr) {
    const auto tip = detail::toTip(a);
    const float baseR = h * 0.20f;
    const pittore::RGBAf ink{float(fg.redF()), float(fg.greenF()),
                              float(fg.blueF()), 1.0f};
    const float op = float(std::clamp(a.opacity, 0.0, 1.0));
    const float flowBase = float(std::clamp(a.flowPct, 0.0, 1.0));
    const auto sizeC = pittore::ui::brushcurve::parseEncoded(a.sizeCurve);
    const auto opC = pittore::ui::brushcurve::parseEncoded(a.opacityCurve);
    const auto flowC = pittore::ui::brushcurve::parseEncoded(a.flowCurve);
    return strokeCanvas(w, h,
                        [&](pittore::RGBAf* buf, int bw, int bh, float x,
                            float y, double pressure, bool measure) {
                            const float pc = float(std::clamp(pressure, 0.0, 1.0));
                            const float r =
                                baseR * detail::curveOr(
                                            sizeC, pressure,
                                            detail::sizeMult(pressure,
                                                             a.pressureSize));
                            const float need =
                                pittore::compute::tip_spacing(r, tip);
                            if (measure) return need;
                            const float o =
                                op *
                                detail::curveOr(
                                    opC, pressure,
                                    a.pressureOpacity ? pc : 1.0f) *
                                flowBase * detail::curveOr(flowC, pressure, 1.0f);
                            pittore::compute::paint_tip_dab_host(
                                buf, bw, bh, x, y, r, tip, o, ink, nullptr,
                                tex, nullptr, nullptr, kgrain, a.mixOp);
                            return need;
                        }, paper);
}

inline QImage strokePreviewStamp(const pittore::compute::StampTip& tip,
                                 int mode, const QColor& fg, double angleDeg,
                                 int w = 300, int h = 110,
                                 const pittore::compute::PatternTex* tex = nullptr,
                                 const AutoParams& pr = AutoParams{},
                                 bool paper = true,
                                 const QColor& bg = QColor(),
                                 double neutralPct = 50.0,
                                 double brightnessPct = 0.0,
                                 double contrastPct = 100.0,
                                 const pittore::compute::papergrain::PaperGrain* kgrain =
                                     nullptr) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(paper ? Qt::white : Qt::transparent);
    if (!tip.valid()) return img;
    const auto smode = detail::toStampMode(mode);
    const auto levels =
        detail::toStampLevels(neutralPct, brightnessPct, contrastPct);
    const pittore::RGBAf ink{float(fg.redF()), float(fg.greenF()),
                              float(fg.blueF()), 1.0f};
    const QColor bgEff = bg.isValid() ? bg : fg;
    const pittore::RGBAf bgC{float(bgEff.redF()), float(bgEff.greenF()),
                              float(bgEff.blueF()), 1.0f};
    const float baseR = h * 0.20f;
    const auto sizeC = pittore::ui::brushcurve::parseEncoded(pr.sizeCurve);
    const auto opC = pittore::ui::brushcurve::parseEncoded(pr.opacityCurve);
    const auto flowC = pittore::ui::brushcurve::parseEncoded(pr.flowCurve);
    const float opBase = float(std::clamp(pr.opacity, 0.0, 1.0)) *
                         float(std::clamp(pr.flowPct, 0.0, 1.0));
    pittore::compute::StampSharp sharp;
    sharp.strength =
        float(std::clamp(pr.sharpness / 100.0, 0.0, 1.0));
    sharp.softness =
        float(std::clamp(pr.soften / 100.0, 0.0, 1.0));
    const pittore::compute::papergrain::PaperGrain* grainPtr =
        (kgrain && kgrain->valid()) ? kgrain : nullptr;
    return strokeCanvas(w, h,
                        [&](pittore::RGBAf* buf, int bw, int bh, float x,
                            float y, double pressure, bool measure) {
                            const float pc = float(std::clamp(pressure, 0.0, 1.0));
                            const float r =
                                baseR * detail::curveOr(
                                            sizeC, pressure,
                                            detail::sizeMult(pressure,
                                                             pr.pressureSize));
                            const float need =
                                pittore::compute::stamp_spacing(r, tip);
                            if (measure) return need;
                            const float o =
                                opBase *
                                detail::curveOr(
                                    opC, pressure,
                                    pr.pressureOpacity ? pc : 1.0f) *
                                detail::curveOr(flowC, pressure, 1.0f);
                            pittore::compute::stamp_dab_host(
                                buf, bw, bh, x, y, r, tip, smode, o, ink,
                                float(angleDeg), nullptr, tex, nullptr,
                                nullptr, 0, 1, &levels, &bgC, nullptr, 0.0f,
                                pr.mixOp, grainPtr, &sharp);
                            return need;
                        }, paper);
}

// Hose scratch stroke: cycles cells per dab (incremental) or hash-picks
// them (random, fixed seed so the preview is stable).
inline QImage strokePreviewHose(const std::vector<pittore::compute::StampTip>& cells,
                                const QString& selection,
                                std::uint64_t seed, int mode, const QColor& fg,
                                double angleDeg, int w = 300, int h = 110,
                                const pittore::compute::PatternTex* tex = nullptr,
                                const AutoParams& pr = AutoParams{},
                                bool paper = true,
                                const pittore::compute::papergrain::PaperGrain* kgrain =
                                    nullptr) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(paper ? Qt::white : Qt::transparent);
    if (cells.empty()) return img;
    const auto smode = detail::toStampMode(mode);
    const pittore::RGBAf ink{float(fg.redF()), float(fg.greenF()),
                              float(fg.blueF()), 1.0f};
    const float baseR = h * 0.20f;
    const bool random = selection == QStringLiteral("random");
    std::size_t dabNo = 0;
    // Step on the first cell (stable stride; cells vary the ink).
    const float stepSpacing = [&] {
        pittore::compute::StampTip t = cells.front();
        t.sanitize();
        return t.spacingPct;
    }();
    const auto sizeC = pittore::ui::brushcurve::parseEncoded(pr.sizeCurve);
    const auto opC = pittore::ui::brushcurve::parseEncoded(pr.opacityCurve);
    const auto flowC = pittore::ui::brushcurve::parseEncoded(pr.flowCurve);
    const float opBase = float(std::clamp(pr.opacity, 0.0, 1.0)) *
                         float(std::clamp(pr.flowPct, 0.0, 1.0));
    return strokeCanvas(w, h,
                        [&](pittore::RGBAf* buf, int bw, int bh, float x,
                            float y, double pressure, bool measure) {
                            const float pc = float(std::clamp(pressure, 0.0, 1.0));
                            const float r =
                                baseR * detail::curveOr(
                                            sizeC, pressure,
                                            detail::sizeMult(pressure,
                                                             pr.pressureSize));
                            const float need = std::max(
                                0.75f, 2.0f * r * stepSpacing / 100.0f);
                            if (measure) return need;
                            std::size_t idx = dabNo % cells.size();
                            if (random) {
                                std::uint64_t z =
                                    seed + dabNo * 0x9E3779B97F4A7C15ull;
                                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                                z ^= (z >> 31);
                                idx = std::size_t(z % cells.size());
                            }
                            ++dabNo;
                            const auto& tip = cells[idx];
                            if (!tip.valid()) return need;
                            const float o =
                                opBase *
                                detail::curveOr(
                                    opC, pressure,
                                    pr.pressureOpacity ? pc : 1.0f) *
                                detail::curveOr(flowC, pressure, 1.0f);
                            pittore::compute::StampSharp sharp;
                            sharp.strength = float(
                                std::clamp(pr.sharpness / 100.0, 0.0, 1.0));
                            sharp.softness = float(
                                std::clamp(pr.soften / 100.0, 0.0, 1.0));
                            const pittore::compute::papergrain::PaperGrain* grainPtr =
                                (kgrain && kgrain->valid()) ? kgrain : nullptr;
                            pittore::compute::stamp_dab_host(
                                buf, bw, bh, x, y, r, tip, smode, o, ink,
                                float(angleDeg), nullptr, tex, nullptr,
                                nullptr, 0, 1, nullptr, nullptr, nullptr,
                                0.0f, pr.mixOp, grainPtr, &sharp);
                            return need;
                        }, paper);
}

// Smudge scratch: a dark block on the left gives the stroke something to
// drag across the paper, showing the smear honestly. `dab` receives the
// evolving carry: (buf, w, h, x, y, pressure, carry, measure) -> stride.
template <typename DabFn>
QImage strokePreviewSmudgeImpl(const QColor& fg, DabFn&& dab, int w = 300,
                               int h = 110) {
    (void)fg;  // foreground reaches the dab through its own SmudgeCtl
    std::vector<pittore::RGBAf> buf(std::size_t(w) * h,
                                     pittore::RGBAf{0, 0, 0, 0});
    // Seed ink: dark block at the stroke start, on opaque paper.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            buf[std::size_t(y) * w + x] = pittore::RGBAf{1, 1, 1, 1};
    {
        pittore::compute::AutoTip block;
        block.hardness = 1.0f;
        pittore::compute::paint_tip_dab_host(
            buf.data(), w, h, 30.0f, h / 2.0f, h * 0.30f, block, 1.0f,
            pittore::RGBAf{0, 0, 0, 1});
    }
    pittore::compute::SmudgeCarry carry;
    const double amp = h * 0.18;
    double lastX = 8.0, lastY = h / 2.0, since = 1e9;
    for (double x = 8.0; x <= w - 8.0; x += 1.0) {
        const double t = (x - 8.0) / (w - 16.0);
        const double y = h / 2.0 + amp * std::sin(t * 2.0 * 3.141592653589793);
        const double pressure = 0.35 + 0.65 * std::sin(t * 3.141592653589793);
        since += std::hypot(x - lastX, y - lastY);
        const float need = dab(nullptr, 0, 0, 0, 0, pressure, carry, true);
        if (since >= need) {
            dab(buf.data(), w, h, float(x), float(y), pressure, carry, false);
            since = 0.0;
        }
        lastX = x;
        lastY = y;
    }
    QImage out(w, h, QImage::Format_ARGB32);
    out.fill(Qt::white);
    detail::blitOver(buf, w, h, out);
    return out;
}

inline QImage strokePreviewSmudgeAuto(const AutoParams& a, double rate,
                                      const QColor& fg, int w = 300,
                                      int h = 110,
                                      const pittore::compute::PatternTex* tex = nullptr,
                                      int smudgeMode = 0,
                                      double colorRate01 = 0.0,
                                      double trailRadii = 0.0) {
    const auto tip = detail::toTip(a);
    const float baseR = h * 0.16f;
    const float rt = float(std::clamp(rate, 0.0, 1.0));
    const float opBase = float(std::clamp(a.opacity, 0.0, 1.0)) *
                         float(std::clamp(a.flowPct, 0.0, 1.0));
    const auto sizeC = pittore::ui::brushcurve::parseEncoded(a.sizeCurve);
    const auto opC = pittore::ui::brushcurve::parseEncoded(a.opacityCurve);
    const auto flowC = pittore::ui::brushcurve::parseEncoded(a.flowCurve);
    return strokePreviewSmudgeImpl(
        fg,
        [&](pittore::RGBAf* buf, int bw, int bh, float x, float y,
            double pressure, pittore::compute::SmudgeCarry& carry,
            bool measure) {
            const float pc = float(std::clamp(pressure, 0.0, 1.0));
            const float r =
                baseR * detail::curveOr(sizeC, pressure,
                                        detail::sizeMult(pressure,
                                                         a.pressureSize));
            const float need =
                pittore::compute::tip_spacing(r, tip);
            if (measure || !buf) return need;
            const float o =
                opBase *
                detail::curveOr(opC, pressure,
                                a.pressureOpacity ? pc : 1.0f) *
                detail::curveOr(flowC, pressure, 1.0f);
            pittore::compute::SmudgeCtl ctl;
            ctl.mode = smudgeMode == 1
                           ? pittore::compute::SmudgeMode::Smear
                           : pittore::compute::SmudgeMode::Dulling;
            ctl.colorRate = float(std::clamp(colorRate01, 0.0, 1.0));
            ctl.fg = pittore::RGBAf{float(fg.redF()), float(fg.greenF()),
                                     float(fg.blueF()), 1.0f};
            ctl.trailX = float(-trailRadii * r);
            pittore::compute::smudge_tip_dab_host(buf, bw, bh, x, y, r, tip,
                                                  rt * o, 1.0f, carry,
                                                  nullptr, tex, nullptr,
                                                  nullptr, 0, nullptr,
                                                  pittore::compute::BlendMode::
                                                      Normal,
                                                  &ctl);
            return need;
        },
        w, h);
}

inline QImage strokePreviewSmudgeStamp(const pittore::compute::StampTip& tip,
                                       double rate, double angleDeg,
                                       const QColor& fg, int w = 300,
                                       int h = 110,
                                       const pittore::compute::PatternTex* tex = nullptr,
                                       const AutoParams& pr = AutoParams{},
                                       int smudgeMode = 0,
                                       double colorRate01 = 0.0,
                                       double trailRadii = 0.0) {
    const float baseR = h * 0.16f;
    const float rt = float(std::clamp(rate, 0.0, 1.0));
    const auto sizeC = pittore::ui::brushcurve::parseEncoded(pr.sizeCurve);
    const auto opC = pittore::ui::brushcurve::parseEncoded(pr.opacityCurve);
    return strokePreviewSmudgeImpl(
        fg,
        [&](pittore::RGBAf* buf, int bw, int bh, float x, float y,
            double pressure, pittore::compute::SmudgeCarry& carry,
            bool measure) {
            const float pc = float(std::clamp(pressure, 0.0, 1.0));
            const float r =
                baseR * detail::curveOr(sizeC, pressure,
                                        detail::sizeMult(pressure,
                                                         pr.pressureSize));
            const float need = pittore::compute::stamp_spacing(r, tip);
            if (measure || !buf || !tip.valid()) return need;
            const float o = detail::curveOr(
                opC, pressure, pr.pressureOpacity ? pc : 1.0f);
            pittore::compute::SmudgeCtl ctl;
            ctl.mode = smudgeMode == 1
                           ? pittore::compute::SmudgeMode::Smear
                           : pittore::compute::SmudgeMode::Dulling;
            ctl.colorRate = float(std::clamp(colorRate01, 0.0, 1.0));
            ctl.fg = pittore::RGBAf{float(fg.redF()), float(fg.greenF()),
                                     float(fg.blueF()), 1.0f};
            ctl.trailX = float(-trailRadii * r);
            pittore::compute::smudge_stamp_dab_host(buf, bw, bh, x, y, r,
                                                    tip, rt * o, 1.0f,
                                                    float(angleDeg), carry,
                                                    nullptr, tex, nullptr,
                                                    nullptr, 0, 1, nullptr,
                                                    pittore::compute::BlendMode::
                                                        Normal,
                                                    &ctl);
            return need;
        },
        w, h);
}

// Live widget: fixed-height strip that repaints from cached params.
class BrushPreviewWidget final : public QWidget {
  public:
    explicit BrushPreviewWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(96);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setObjectName(QStringLiteral("brushPreview"));
    }

    // Owned grain tile: the widget keeps its own copy so library churn can
    // never dangle the preview.
    struct OwnedPattern {
        std::vector<float> gray;
        pittore::compute::PatternTex tex;
        std::vector<float> grainGray;
        pittore::compute::papergrain::PaperGrain grain;
        bool grainOn = false;
    };

    void setExpandable(bool on) {
        setSizePolicy(on ? QSizePolicy::Expanding : QSizePolicy::Fixed,
                      on ? QSizePolicy::Expanding : QSizePolicy::Fixed);
        if (on) setMinimumHeight(200);
    }

    void setAuto(const AutoParams& a, const QColor& fg,
                 const OwnedPattern* pattern = nullptr) {
        auto_ = a;
        fg_ = fg;
        setPattern(pattern);
        isStamp_ = false;
        isHose_ = false;
        isSmudge_ = false;
        refresh();
    }
    void setStamp(const pittore::compute::StampTip& tip, int mode,
                  double angleDeg, const QColor& fg,
                  const OwnedPattern* pattern = nullptr,
                  const AutoParams& pr = AutoParams{},
                  const QColor& bg = QColor(), double neutralPct = 50.0,
                  double brightnessPct = 0.0, double contrastPct = 100.0) {
        stamp_ = tip;
        stampMode_ = mode;
        stampAngle_ = angleDeg;
        fg_ = fg;
        bg_ = bg;
        neutralPct_ = neutralPct;
        brightnessPct_ = brightnessPct;
        contrastPct_ = contrastPct;
        setPattern(pattern);
        pressure_ = pr;
        isStamp_ = true;
        isHose_ = false;
        isSmudge_ = false;
        refresh();
    }
    void setHose(const std::vector<pittore::compute::StampTip>& cells,
                 const QString& selection, std::uint64_t seed, int mode,
                 double angleDeg, const QColor& fg,
                 const OwnedPattern* pattern = nullptr,
                 const AutoParams& pr = AutoParams{}) {
        hoseCells_ = cells;
        hoseSelection_ = selection;
        hoseSeed_ = seed;
        stampMode_ = mode;
        stampAngle_ = angleDeg;
        fg_ = fg;
        setPattern(pattern);
        pressure_ = pr;
        isStamp_ = false;
        isHose_ = true;
        isSmudge_ = false;
        refresh();
    }
    void setSmudgeAuto(const AutoParams& a, double rate, const QColor& fg,
                       const OwnedPattern* pattern = nullptr,
                       int smudgeMode = 0, double colorRate01 = 0.0,
                       double trailRadii = 0.0) {
        auto_ = a;
        smudgeRate_ = rate;
        fg_ = fg;
        smudgeMode_ = smudgeMode;
        smudgeColorRate_ = colorRate01;
        smudgeTrail_ = trailRadii;
        setPattern(pattern);
        isStamp_ = false;
        isHose_ = false;
        isSmudge_ = true;
        smudgeIsStamp_ = false;
        refresh();
    }
    void setSmudgeStamp(const pittore::compute::StampTip& tip, double rate,
                        double angleDeg, const QColor& fg,
                        const OwnedPattern* pattern = nullptr,
                        const AutoParams& pr = AutoParams{},
                        int smudgeMode = 0, double colorRate01 = 0.0,
                        double trailRadii = 0.0) {
        stamp_ = tip;
        smudgeRate_ = rate;
        stampAngle_ = angleDeg;
        fg_ = fg;
        smudgeMode_ = smudgeMode;
        smudgeColorRate_ = colorRate01;
        smudgeTrail_ = trailRadii;
        setPattern(pattern);
        pressure_ = pr;
        isStamp_ = false;
        isHose_ = false;
        isSmudge_ = true;
        smudgeIsStamp_ = true;
        refresh();
    }
    void clear() {
        isStamp_ = false;
        isHose_ = false;
        isSmudge_ = false;
        auto_ = AutoParams{};
        pressure_ = AutoParams{};
        refresh();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        if (cache_.isNull()) refresh();
        if (!cache_.isNull()) {
            // Fit width, center vertically.
            const int dw = width() - 16;
            const int dh = dw * cache_.height() / cache_.width();
            const int y = (height() - dh) / 2;
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(QRect(8, y, dw, dh), cache_);
        }
        p.setPen(QPen(QColor(0, 0, 0, 40), 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

  private:
    void setPattern(const OwnedPattern* pattern) {
        pattern_ = OwnedPattern{};
        if (!pattern) return;
        if (pattern->tex.valid() && !pattern->gray.empty()) {
            pattern_.gray = pattern->gray;
            pattern_.tex = pattern->tex;
            pattern_.tex.gray = pattern_.gray.data();
        }
        if (pattern->grainOn && pattern->grain.valid() &&
            !pattern->grainGray.empty()) {
            pattern_.grainGray = pattern->grainGray;
            pattern_.grain = pattern->grain;
            pattern_.grain.gray = pattern_.grainGray.data();
            pattern_.grainOn = true;
        }
    }

    const pittore::compute::PatternTex* texPtr() const {
        return pattern_.tex.valid() ? &pattern_.tex : nullptr;
    }

    const pittore::compute::papergrain::PaperGrain* grainPtr() const {
        return (pattern_.grainOn && pattern_.grain.valid()) ? &pattern_.grain
                                                            : nullptr;
    }

    void refresh() {
        if (isSmudge_ && !smudgeIsStamp_)
            cache_ = strokePreviewSmudgeAuto(auto_, smudgeRate_, fg_, 300,
                                             110, texPtr(), smudgeMode_,
                                             smudgeColorRate_, smudgeTrail_);
        else if (isSmudge_ && smudgeIsStamp_ && stamp_.valid())
            cache_ = strokePreviewSmudgeStamp(stamp_, smudgeRate_, stampAngle_,
                                              fg_, 300, 110, texPtr(),
                                              pressure_, smudgeMode_,
                                              smudgeColorRate_, smudgeTrail_);
        else if (isHose_ && !hoseCells_.empty())
            cache_ = strokePreviewHose(hoseCells_, hoseSelection_, hoseSeed_,
                                       stampMode_, fg_, stampAngle_, 300, 110,
                                       texPtr(), pressure_, true, grainPtr());
        else if (isStamp_ && stamp_.valid())
            cache_ = strokePreviewStamp(stamp_, stampMode_, fg_, stampAngle_,
                                        300, 110, texPtr(), pressure_, true,
                                        bg_, neutralPct_, brightnessPct_,
                                        contrastPct_, grainPtr());
        else if (!isStamp_ && !isHose_ && !isSmudge_)
            cache_ = strokePreviewAuto(auto_, fg_, 300, 110, texPtr(), true,
                                       grainPtr());
        else
            cache_ = QImage();
        update();
    }

    AutoParams auto_;
    AutoParams pressure_;  // response curves/gates for stamp/hose previews
    pittore::compute::StampTip stamp_;
    std::vector<pittore::compute::StampTip> hoseCells_;
    QString hoseSelection_ = QStringLiteral("incremental");
    std::uint64_t hoseSeed_ = 0;
    int stampMode_ = 0;
    double stampAngle_ = 0.0;
    double smudgeRate_ = 0.7;
    int smudgeMode_ = 0;
    double smudgeColorRate_ = 0.0;
    double smudgeTrail_ = 0.0;
    bool smudgeIsStamp_ = false;
    QColor fg_ = Qt::black;
    QColor bg_;
    double neutralPct_ = 50.0;
    double brightnessPct_ = 0.0;
    double contrastPct_ = 100.0;
    bool isStamp_ = false;
    bool isHose_ = false;
    bool isSmudge_ = false;
    OwnedPattern pattern_;
    QImage cache_;
};

}  // namespace pittore::ui::brushpreview
