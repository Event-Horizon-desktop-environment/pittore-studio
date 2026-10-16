// test_hidpi_render.cpp — fractional display scaling render check.
// Runs with QT_SCALE_FACTOR=1.5 (set by the suite, see tests/meson.build):
// proves the interface actually paints at fractional devicePixelRatio —
// canvas viewport grab lands at device-pixel size with real content, and
// glyphs bake at the live ratio instead of 1x. Runs headless
// (QT_QPA_PLATFORM=offscreen).
#include <cmath>
#include <cstdio>

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QThread>
#include <QWidget>

#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/canvas_view.h"
#include "ui/icons.h"
#include "ui/panels/shared/panel_helpers.h"
#include "ui/svg_parts.h"

using namespace pittore::ui;

namespace {

const char* kSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
    "<rect x=\"10\" y=\"10\" width=\"80\" height=\"80\" fill=\"#ff0000\"/>"
    "</svg>";

DocumentItem* openDoc(AppState& state) {
    SvgImportResult result;
    int dpi = 96;
    QString error;
    if (!svgPartsImport(QByteArray(kSvg), &result, &dpi, &error)) return nullptr;
    QString openError;
    if (!state.openSvgParts(QStringLiteral("red.svg"), result, dpi, &openError))
        return nullptr;
    return state.activeDocument();
}

void pump(int rounds = 60) {
    for (int i = 0; i < rounds; ++i) {
        qApp->processEvents();
        QThread::msleep(5);
    }
}

// Fraction of pixels that differ from the first pixel (flat-fill guard).
double varFraction(const QImage& img) {
    if (img.isNull() || img.width() < 2 || img.height() < 2) return 0.0;
    const QRgb bg = img.pixel(0, 0);
    long long diff = 0;
    const long long n = static_cast<long long>(img.width()) * img.height();
    const int step = qMax(1, int(n / 20000));
    long long sampled = 0;
    for (long long i = 0; i < n; i += step) {
        const int x = int(i % img.width()), y = int(i / img.width());
        if (img.pixel(x, y) != bg) ++diff;
        ++sampled;
    }
    return sampled ? double(diff) / double(sampled) : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    const QString scratch =
        QDir::tempPath() + QStringLiteral("/pittore-test-hidpi-render");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);
    qputenv("XDG_CONFIG_HOME", scratch.toLocal8Bit());
    QApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-hidpi-render-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    // The suite forces QT_SCALE_FACTOR=1.5: the viewport must report it.
    QWidget window;
    AppState state;
    auto* canvas = new CanvasView(&state, &window);
    window.resize(800, 600);
    canvas->setGeometry(0, 0, 800, 600);
    window.show();
    pump();
    const qreal dpr = canvas->viewport()->devicePixelRatioF();
    std::printf("hidpi: viewport dpr=%.3f\n", double(dpr));
    CHECK(std::fabs(dpr - 1.5) < 0.01);

    // Glyphs bake at the live ratio, not 1x.
    {
        const QPixmap pm =
            glyphPixmap(QStringLiteral("move"), 20, QColor(Qt::black), dpr);
        CHECK(std::fabs(pm.devicePixelRatioF() - dpr) < 0.01);
        CHECK(pm.width() == int(std::ceil(20.0 * double(dpr))));
        CHECK(varFraction(pm.toImage()) > 0.01);
    }

    // Swatch wells bake at explicit ratios with logical geometry intact.
    {
        const QPixmap pm = colorSwatchPixmap(QColor(200, 40, 40), dpr);
        CHECK(std::fabs(pm.devicePixelRatioF() - dpr) < 0.01);
        CHECK(pm.width() == int(std::ceil(18.0 * double(dpr))));
        CHECK(varFraction(pm.toImage()) > 0.01);
        const QPixmap pm1 = colorSwatchPixmap(QColor(200, 40, 40), 1.0);
        CHECK(pm1.width() == 18);
    }

    // The canvas paints at device resolution: grab size follows DPR and
    // carries real (non-flat) content.
    DocumentItem* doc = openDoc(state);
    CHECK(doc != nullptr);
    if (doc) {
        canvas->setZoom(2.0);
        pump(120);
        canvas->viewport()->update();
        pump();
        const QImage shot = canvas->viewport()->grab().toImage();
        const int wantW =
            int(std::ceil(double(canvas->viewport()->width()) * double(dpr)));
        const int wantH =
            int(std::ceil(double(canvas->viewport()->height()) * double(dpr)));
        std::printf("hidpi: viewport=%dx%d grab=%dx%d want=%dx%d\n",
                    canvas->viewport()->width(), canvas->viewport()->height(),
                    shot.width(), shot.height(), wantW, wantH);
        CHECK(std::abs(shot.width() - wantW) <= 1);
        CHECK(std::abs(shot.height() - wantH) <= 1);
        CHECK(varFraction(shot) > 0.01);
        shot.save(scratch + QStringLiteral("/hidpi-150-canvas.png"));
    }

    if (pittore_test::failures() == 0)
        std::printf("test_hidpi_render: all checks passed\n");
    else
        std::printf("test_hidpi_render: %d failures\n",
                    pittore_test::failures());
    return pittore_test::failures() ? 1 : 0;
}
