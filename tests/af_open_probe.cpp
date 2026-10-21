// af_open_probe.cpp — open .af files through the app's real import path
// (AppState::openImageFile -> afDecodeLayers -> openAfLayers), i.e. the same
// code File > Open runs, and report the document each one produces.
// Diagnostic only, NOT a registered test.
//
//   af_open_probe <file.af> [more.af ...]
//   af_open_probe --json <file.af> [more.af ...]
//
// The default output is for eyeballing (per-layer name/offset/size, packed
// bytes, composite PNGs under /tmp/opencode). --json prints one JSON array
// with the same fields so the research corpus can diff opens between builds.
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

namespace {

std::string jsonEscape(const QString& s) {
    const std::string in = s.toStdString();
    std::string out;
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

const char* kindOf(const LayerItem& l) {
    if (l.isText) return "text";
    if (l.kind == LayerItem::Kind::Group) return "group";
    return "pixel";
}

int widthOf(const LayerItem& l) {
    return l.deferredWidth > 0 ? int(l.deferredWidth)
                               : (l.pixels ? int(l.pixels->width()) : 0);
}

int heightOf(const LayerItem& l) {
    return l.deferredHeight > 0 ? int(l.deferredHeight)
                                : (l.pixels ? int(l.pixels->height()) : 0);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-af-open-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());

    bool json = false;
    int first = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            json = true;
            ++first;
        } else {
            break;
        }
    }
    if (argc - first < 1) {
        std::printf("usage: af_open_probe [--json] <file.af> [more.af ...]\n");
        return 2;
    }

    AppState state;
    int failures = 0;
    if (json) std::printf("[");
    bool firstJson = true;
    for (int i = first; i < argc; ++i) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        if (!json) std::printf("=== %s\n", argv[i]);

        QString error;
        const bool ok = state.openImageFile(path, &error);
        DocumentItem* d = state.activeDocument();

        if (!ok || !d) {
            ++failures;
            if (json) {
                std::printf("%s{\"file\":\"%s\",\"ok\":false,\"error\":\"%s\"}",
                            firstJson ? "" : ",", jsonEscape(path).c_str(),
                            jsonEscape(error).c_str());
                firstJson = false;
            } else {
                std::printf("    OPEN FAILED: %s\n",
                            error.isEmpty() ? "(no message)" : qPrintable(error));
            }
            continue;
        }

        // Every document this open produced, not just the active one.
        const int docsAdded = static_cast<int>(state.documents().size());
        if (json) std::printf("%s{\"file\":\"%s\",\"ok\":true,\"documents\":[",
                              firstJson ? "" : ",", jsonEscape(path).c_str());
        firstJson = false;
        for (int di = 0; di < docsAdded; ++di) {
            DocumentItem* doc = state.documents()[di];
            if (json) {
                std::printf("%s{\"title\":\"%s\",\"width\":%d,\"height\":%d,\"layers\":[",
                            di ? "," : "", jsonEscape(doc->title).c_str(),
                            doc->size.width(), doc->size.height());
                for (std::size_t li = 0;
                     li < static_cast<std::size_t>(doc->layers.size()); ++li) {
                    const LayerItem& l = doc->layers[li];
                    std::printf(
                        "%s{\"name\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"w\":%d,\"h\":%d,"
                        "\"kind\":\"%s\",\"visible\":%s,\"blend\":\"%s\",\"opacity\":%d,"
                        "\"mask\":%s,\"packed\":%s}",
                        li ? "," : "", jsonEscape(l.name).c_str(),
                        l.offset.x(), l.offset.y(), widthOf(l), heightOf(l), kindOf(l),
                        l.visible ? "true" : "false", jsonEscape(l.blendMode).c_str(),
                        l.opacity, l.hasMask ? "true" : "false",
                        pittore::ui::hasDeferredPixels(l) ? "true" : "false");
                }
                std::printf("]}");
            } else {
                std::printf("    doc %d  \"%s\" %dx%d layers=%d\n", di,
                            qPrintable(doc->title), doc->size.width(),
                            doc->size.height(), static_cast<int>(doc->layers.size()));
                for (const auto& l : doc->layers) {
                    std::printf("      %-32s %6.1f,%.1f  %5dx%-5d %s\n",
                                qPrintable(l.name), l.offset.x(), l.offset.y(),
                                widthOf(l), heightOf(l), kindOf(l));
                }
                const QString png =
                    QStringLiteral("/tmp/opencode/af-open-doc-%1-%2.png").arg(di);
                doc->composite.save(png, "PNG");
                std::printf("      composite -> %s\n", qPrintable(png));

                double realized = 0.0, stashed = 0.0;
                for (const auto& l : d->layers) {
                    // Resident bytes per layer: realised pixels cost RGBAf (16 B/px),
                    // a layer packed behind the flattened base costs RGBA8 (4 B/px).
                    const int lw = l.pixels ? static_cast<int>(l.pixels->width()) : 0;
                    const int lh = l.pixels ? static_cast<int>(l.pixels->height()) : 0;
                    double mib = static_cast<double>(lw) * lh * 16.0 / (1024.0 * 1024.0);
                    int sw = 0, sh = 0;
                    const bool packed = pittore::ui::hasDeferredPixels(l);
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
                                qPrintable(l.name), packed ? sw : lw, packed ? sh : lh,
                                qPrintable(l.blendMode), l.opacity,
                                l.visible ? "" : " HIDDEN",
                                l.hasMask ? " +mask" : "", packed ? " PACKED" : "");
                }
                std::printf(
                    "    pixels: realized %.1f MiB + packed %.1f MiB = %.1f MiB\n",
                    realized, stashed, realized + stashed);
            }
        }
        if (json) std::printf("]}");
    }
    if (json) std::printf("]\n");

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    if (!json)
        std::printf("\nopened %d/%d\npeak RSS %.1f MiB\n", argc - first - failures,
                    argc - first, ru.ru_maxrss / 1024.0);
    return failures ? 1 : 0;
}
