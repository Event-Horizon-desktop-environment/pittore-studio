#include <QApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdio>
#include <cstdlib>

#include "lab_engine.h"
#include "lab_mainwindow.h"

namespace {

// Headless proof that the lab's glue (buffers, sizes, the real pipeline)
// actually runs: load the bundled portrait, segment with BiRefNet-portrait,
// refine with defaults, print stage list + matte stats, write a composite.
int selfTest() {
    using namespace pittore::lab;
    const QStringList imgs = {
        QStringLiteral("tests/Refine-app/Tests/d.png"),
        QStringLiteral("Tests/d.png"),
        QStringLiteral("/home/matt/Documents/GitHub/pittore-studio/tests/Refine-app/Tests/d.png"),
    };
    QString imgPath;
    for (const QString& c : imgs)
        if (QFileInfo::exists(c)) { imgPath = c; break; }
    if (imgPath.isEmpty()) {
        std::printf("SELFTEST: no d.png found\n");
        return 2;
    }
    QImage img(imgPath);
    if (img.isNull()) {
        std::printf("SELFTEST: could not load %s\n", qPrintable(imgPath));
        return 2;
    }
    // Find a portrait/birefnet model on disk.
    const QStringList dirs = {
        qEnvironmentVariable("PITTORE_MODELS_DIR"),
        QDir::homePath() + "/.local/share/InfinityPhoto/models",
        QDir::homePath() + "/.local/share/PittoreStudio/models",
    };
    QString model;
    for (const QString& d : dirs) {
        const QDir dir(d);
        if (!dir.exists()) continue;
        for (const QString& f : dir.entryList({"*birefnet*portrait*.onnx", "*portrait*.onnx"},
                                              QDir::Files)) {
            model = dir.filePath(f);
            break;
        }
        if (!model.isEmpty()) break;
    }
    if (model.isEmpty()) {
        std::printf("SELFTEST: no portrait model found (set PITTORE_MODELS_DIR)\n");
        return 2;
    }
    LabEngine eng;
    LabParams p;
    p.modelPath = model;
    p.inputSize = 1024;
    p.decontam = true;  // exercise the decontamination path too
    p.useRefineAi = true;
    QString err;
    std::printf("SELFTEST: image %dx%d, model %s\n", img.width(), img.height(),
                qPrintable(model));
    const QImage seed = eng.segment(img, p, &err);
    if (seed.isNull()) {
        std::printf("SELFTEST: segment FAILED: %s\n", qPrintable(err));
        return 1;
    }
    std::printf("SELFTEST: segment ok %dx%d\n", seed.width(), seed.height());
    const RunResult r = eng.refine(img, seed, p);
    if (!r.ok) {
        std::printf("SELFTEST: refine FAILED: %s\n", qPrintable(r.error));
        return 1;
    }
    std::printf("SELFTEST: refine ok in %.2fs, %d stages:\n", r.ms,
                int(r.stages.size()));
    for (const Stage& s : r.stages) std::printf("  - %s\n", qPrintable(s.first));
    // Matte coverage: fraction of pixels above 127.
    const QImage g = r.matte.convertToFormat(QImage::Format_Grayscale8);
    long sel = 0;
    for (int y = 0; y < g.height(); ++y) {
        const uchar* row = g.constScanLine(y);
        for (int x = 0; x < g.width(); ++x)
            if (row[x] >= 127) ++sel;
    }
    std::printf("SELFTEST: matte coverage %.1f%%\n",
                100.0 * sel / double(g.width() * g.height()));
    r.cutout.save("/tmp/refinelab_selftest_cutout.png");
    r.matte.save("/tmp/refinelab_selftest_matte.png");
    std::printf("SELFTEST: wrote /tmp/refinelab_selftest_{cutout,matte}.png\n");
    std::printf("SELFTEST: PASS\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--selftest") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app(argc, argv);
            return selfTest();
        }
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Refine Lab"));

    // CLI: --mcp-port N (default 8765, 0 = ephemeral, "off" = disabled),
    // --dark (start in dark mode), --open FILE.
    quint16 mcpPort = 8765;
    bool startDark = false;
    QString openFile;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a = argv[i];
        if (a == "--no-mcp") {
            mcpPort = 0xffff;
        } else if (a == "--mcp-port" && i + 1 < argc) {
            mcpPort = quint16(qsizetype(std::strtoul(argv[++i], nullptr, 10)));
        } else if (a == "--dark") {
            startDark = true;
        } else if (a == "--open" && i + 1 < argc) {
            openFile = QString::fromLocal8Bit(argv[++i]);
        }
    }
    if (const QByteArray env = qgetenv("PITTORE_REFINE_MCP_PORT"); !env.isEmpty())
        mcpPort = quint16(env.toUShort());

    pittore::lab::LabWindow w;
    if (startDark) w.setDarkMode(true);
    if (!openFile.isEmpty()) w.loadImageFile(openFile, nullptr);
    if (mcpPort != 0xffff) {
        QString err;
        if (!w.startMcp(mcpPort, &err))
            std::fprintf(stderr, "MCP: could not listen on port %u: %s\n",
                         unsigned(mcpPort), qPrintable(err));
        else
            std::fprintf(stderr, "MCP: http://127.0.0.1:%u/mcp\n",
                         unsigned(mcpPort));
    }
    w.show();
    return app.exec();
}
