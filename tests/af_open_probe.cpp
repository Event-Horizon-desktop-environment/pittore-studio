// af_open_probe.cpp — open .af files through the app's real import path
// (AppState::openImageFile -> afDecodeLayers -> openAfLayers), i.e. the same
// code File > Open runs, and report the document each one produces.
// Diagnostic only, NOT a registered test.
//
//   af_open_probe <file.af> [more.af ...]
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdio>
#include <string>

#include <sys/resource.h>

#include "engine/core/log.h"
#include "ui/app_state.h"

using pittore::ui::AppState;
using pittore::ui::LayerItem;
using pittore::ui::DocumentItem;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-af-open-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    if (argc < 2) {
        std::printf("usage: af_open_probe <file.af> [more.af ...]\n");
        return 2;
    }

    AppState state;
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        std::printf("=== %s\n", argv[i]);

        QString error;
        const bool ok = state.openImageFile(path, &error);
        DocumentItem* d = state.activeDocument();

        if (!ok || !d) {
            std::printf("    OPEN FAILED: %s\n",
                        error.isEmpty() ? "(no message)" : qPrintable(error));
            ++failures;
            continue;
        }

        // Every document this open produced, not just the active one.
        const int docsAdded = static_cast<int>(state.documents().size());
        for (int di = 0; di < docsAdded; ++di) {
            DocumentItem* doc = state.documents()[di];
            std::printf("    doc %d  \"%s\" %dx%d layers=%d\n", di,
                        qPrintable(doc->title), doc->size.width(),
                        doc->size.height(), static_cast<int>(doc->layers.size()));
            for (const auto& l : doc->layers) {
                std::printf("      %-32s %6.1f,%.1f  %5dx%-5d %s\n",
                            qPrintable(l.name), l.offset.x(), l.offset.y(),
                            l.deferredWidth > 0 ? int(l.deferredWidth)
                                                : (l.pixels ? int(l.pixels->width()) : 0),
                            l.deferredHeight > 0 ? int(l.deferredHeight)
                                                 : (l.pixels ? int(l.pixels->height()) : 0),
                            l.isText ? "text" : (l.kind == LayerItem::Kind::Group
                                                     ? "group"
                                                     : "pixel"));
            }
            const QString png =
                QStringLiteral("/tmp/opencode/af-open-doc-%1-%2.png").arg(di);
            doc->composite.save(png, "PNG");
            std::printf("      composite -> %s\n", qPrintable(png));
        }
        break;

        std::printf("    ok  \"%s\" %dx%d layers=%d\n", qPrintable(d->title),
                    d->size.width(), d->size.height(),
                    static_cast<int>(d->layers.size()));
        double realized = 0.0, stashed = 0.0;
        for (const auto& l : d->layers) {
            // Resident bytes per layer: realised pixels cost RGBAf (16 B/px),
            // a layer packed behind the flattened base costs RGBA8 (4 B/px).
            const int lw = l.pixels ? static_cast<int>(l.pixels->width()) : 0;
            const int lh = l.pixels ? static_cast<int>(l.pixels->height()) : 0;
            double mib =
                static_cast<double>(lw) * lh * 16.0 / (1024.0 * 1024.0);
            int sw = 0, sh = 0;
            const bool packed = hasDeferredPixels(l);
            if (packed) {
                sw = static_cast<int>(l.deferredWidth);
                sh = static_cast<int>(l.deferredHeight);
                mib = static_cast<double>(l.deferredRgba8.size()) /
                      (1024.0 * 1024.0);
                stashed += mib;
            } else {
                realized += mib;
            }
            std::printf("      %7.1f MiB  %-30s %5dx%-5d %s/%d%s%s%s\n", mib,
                        qPrintable(l.name), packed ? sw : lw,
                        packed ? sh : lh, qPrintable(l.blendMode), l.opacity,
                        l.visible ? "" : " HIDDEN", l.hasMask ? " +mask" : "",
                        packed ? " PACKED" : "");
        }
        std::printf("    pixels: realized %.1f MiB + packed %.1f MiB = %.1f MiB\n",
                    realized, stashed, realized + stashed);
    }

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    std::printf("\nopened %d/%d\npeak RSS %.1f MiB\n", argc - 1 - failures,
                argc - 1, ru.ru_maxrss / 1024.0);
    return failures ? 1 : 0;
}
