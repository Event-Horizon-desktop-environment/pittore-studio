// test_project.cpp — own-format projects: single-file IFP round-trip
// (saveProjectFile → loadProjectFile), the scanProjects start-page listing, and
// the full AppState create → save → open cycle.
//
// Runs headless under QCoreApplication with the CPU backend. PITTORE_PROJECTS_DIR
// and XDG_CONFIG_HOME are pointed at scratch dirs by meson so the real
// ~/Pictures and Settings.toml are never touched.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include "engine/compute/adjust.h"
#include "engine/core/log.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/project_manager.h"

using namespace pittore::ui;

namespace {

// Build a straight-alpha RGBAf image of known values. The critical pixel is
// half-transparent with large RGB: any premultiple/divide round trip would
// corrupt it (a=0.5, r=0.4 would become r=0.8), so the byte-faithful path
// leaves it untouched.
std::shared_ptr<pittore::Image> straightImage() {
    auto img = std::make_shared<pittore::Image>(4, 2);
    const pittore::RGBAf center{0.4f, 0.3f, 0.2f, 0.5f};
    for (std::uint32_t y = 0; y < img->height(); ++y)
        for (std::uint32_t x = 0; x < img->width(); ++x)
            img->data()[y * img->width() + x] =
                (x == 2 && y == 1) ? center : pittore::RGBAf{1, 1, 1, 1};
    return img;
}

ProjectFileData sampleData() {
    ProjectFileData data;
    data.name = QStringLiteral("Round Trip");
    data.size = QSize(320, 200);
    data.dpi = 150;
    data.colorMode = QStringLiteral("RGB/8");
    // Opaque bytes, not a real profile: persistence must be byte-exact
    // regardless of what the bytes mean (the CMYK test covers usability).
    data.iccProfile = QByteArray("fake-destination-profile-bytes");
    data.background = QStringLiteral("white");

    ProjectLayerMeta bg;
    bg.name = QStringLiteral("Background");
    bg.kind = 0;  // int(LayerItem::Kind::Pixel)
    bg.visible = true;
    bg.locked = true;
    bg.pixels = imageToStraightRgba64(*straightImage());

    ProjectLayerMeta top;
    top.name = QStringLiteral("Detail");
    top.kind = 0;
    top.opacity = 80;
    top.blendMode = QStringLiteral("Multiply");
    top.offset = QPointF(12, 24);
    top.scaleX = 1.5;
    top.scaleY = 1.5;
    // Retained vector geometry (as an SVG import would leave behind).
    {
        using namespace pittore::vector;
        auto art = std::make_shared<ArtNode>();
        art->name = "rect";
        Segment s;
        s.kind = Segment::Kind::MoveTo;
        s.x = 0;
        s.y = 0;
        art->segments.push_back(s);
        s.kind = Segment::Kind::LineTo;
        s.x = 10;
        s.y = 0;
        art->segments.push_back(s);
        s = Segment{};
        s.kind = Segment::Kind::Close;
        art->segments.push_back(s);
        art->paint.hasFill = true;
        art->paint.fill[0] = 0x33;
        art->paint.fill[1] = 0x66;
        art->paint.fill[2] = 0xcc;
        art->paint.fill[3] = 0x80;
        art->paint.hasGradient = true;
        art->paint.gradient.radial = true;
        art->paint.gradient.stops = {{0.0f, {0, 0, 0, 255}},
                                     {1.0f, {255, 255, 255, 128}}};
        art->opacity = 0.75;
        art->evenOdd = true;
        top.art = art;
    }

    data.layers.append(top);  // index 0 = top
    data.layers.append(bg);

    data.preview = QImage(64, 40, QImage::Format_ARGB32);
    data.preview.fill(Qt::white);
    return data;
}

}  // namespace

static void test_file_round_trip() {
    // 1. Save → load must preserve every metadata field.
    const ProjectFileData data = sampleData();
    const QString path = projectPathForName(data.name);
    QString error;
    const bool saved = saveProjectFile(path, data, &error);
    if (!saved) std::fprintf(stderr, "    saveProjectFile error: %s\n", error.toLocal8Bit().constData());
    CHECK(saved);
    if (pittore_test::failures() > 0) return;

    // The whole project lives in ONE file: no project.toml, no preview.png and
    // no per-layer PNG sidecars (the old "could not write to layer image
    // layers/NNNN.png" failure path is gone structurally).
    CHECK(QFile::exists(path));
    CHECK(!QFile::exists(QFileInfo(path).absolutePath() +
                         QStringLiteral("/project.toml")));
    CHECK(!QFile::exists(QFileInfo(path).absolutePath() +
                         QStringLiteral("/preview.png")));
    CHECK(!QFile::exists(QFileInfo(path).absolutePath() +
                         QStringLiteral("/layers")));

    ProjectFileData loaded;
    CHECK(loadProjectFile(path, &loaded, &error));
    CHECK(loaded.name == data.name);
    CHECK(loaded.size == data.size);
    CHECK_EQ(loaded.dpi, data.dpi);
    CHECK(loaded.colorMode == data.colorMode);
    CHECK(loaded.iccProfile == data.iccProfile);
    CHECK(loaded.background == data.background);
    CHECK_EQ(loaded.layers.size(), 2);
    if (loaded.layers.size() != 2) return;

    CHECK(loaded.layers[0].name == QStringLiteral("Detail"));
    CHECK_EQ(loaded.layers[0].kind, 0);
    CHECK_EQ(loaded.layers[0].opacity, 80);
    CHECK(loaded.layers[0].blendMode == QStringLiteral("Multiply"));
    CHECK(loaded.layers[0].offset == QPointF(12, 24));
    CHECK_NEAR(loaded.layers[0].scaleX, 1.5, 1e-9);
    CHECK_NEAR(loaded.layers[0].scaleY, 1.5, 1e-9);
    // The metadata-only layer persists without any pixel payload.
    CHECK(loaded.layers[0].pixels.isNull());
    // Retained vector geometry survives the disk hop intact.
    CHECK(loaded.layers[0].art != nullptr);
    if (loaded.layers[0].art) {
        const auto& art = *loaded.layers[0].art;
        CHECK_EQ(art.segments.size(), 3u);
        CHECK_EQ(static_cast<int>(art.segments[1].kind),
                 static_cast<int>(pittore::vector::Segment::Kind::LineTo));
        CHECK(art.evenOdd);
        CHECK_NEAR(art.opacity, 0.75, 1e-9);
        CHECK(art.paint.hasFill);
        CHECK_EQ(static_cast<int>(art.paint.fill[0]), 0x33);
        CHECK_EQ(static_cast<int>(art.paint.fill[3]), 0x80);
        CHECK(art.paint.hasGradient);
        CHECK(art.paint.gradient.radial);
        CHECK_EQ(art.paint.gradient.stops.size(), 2u);
        CHECK_EQ(static_cast<int>(art.paint.gradient.stops[1].rgba[3]), 128);
    }

    CHECK(loaded.layers[1].name == QStringLiteral("Background"));
    CHECK(loaded.layers[1].locked);

    // 2. Straight alpha survives the disk hop: the 0.4/0.3/0.2@0.5 pixel must
    // come back as-is. Premultiplication would double every channel.
    const auto img = straightRgba64ToImage(loaded.layers[1].pixels);
    CHECK(img != nullptr);
    if (!img) return;
    const pittore::RGBAf c = img->data()[1 * img->width() + 2];
    CHECK_NEAR(c.r, 0.4f, 1e-3f);
    CHECK_NEAR(c.g, 0.3f, 1e-3f);
    CHECK_NEAR(c.b, 0.2f, 1e-3f);
    CHECK_NEAR(c.a, 0.5f, 1e-3f);

    // 3. The library scan lists the newly written project file.
    const QVector<ProjectEntry> entries = scanProjects();
    bool found = false;
    for (const ProjectEntry& e : entries) {
        if (e.name == data.name) {
            found = true;
            CHECK(QFileInfo(e.path).absoluteFilePath() ==
                  QFileInfo(path).absoluteFilePath());
            CHECK(!e.thumb.isNull());
        }
    }
    CHECK(found);
}

static void test_app_state_cycle() {
    // createNewProject persists a project file and leaves a live document;
    // saveProject writes the same file; openProject reloads it as a separate
    // document.
    AppState state;
    CHECK(state.documents().isEmpty());

    ProjectFileData data;
    data.name = QStringLiteral("New One");
    data.size = QSize(320, 200);
    data.dpi = 150;
    data.colorMode = QStringLiteral("RGB/8");
    data.background = QStringLiteral("white");
    QString error;
    DocumentItem* doc = state.createNewProject(data, &error);
    CHECK(doc != nullptr);
    if (!doc) return;

    CHECK_EQ(state.documents().size(), 1);
    CHECK_EQ(doc->layers.size(), 1);
    CHECK(doc->layers[0].name == QStringLiteral("Background"));
    CHECK(state.activeProjectPath().endsWith(QStringLiteral("/New One.psc")));
    // Persisted at creation time as a single file.
    CHECK(QFile::exists(projectPathForName(data.name)));

    // Mutate the document, then persist (now over the pre-existing file).
    QImage red(16, 16, QImage::Format_ARGB32_Premultiplied);
    red.fill(0xFFFF0000);
    state.placeImageLayer(red, QStringLiteral("red"), QPointF(160, 100), 1.0);
    CHECK_EQ(doc->layers.size(), 2);
    // The document's CMYK separation bytes ride the .psc round trip too.
    doc->iccProfile = QByteArray("state-round-trip-icc");
    const bool saved2 = state.saveProject(state.activeProjectPath(), &error);
    if (!saved2) std::fprintf(stderr, "    saveProject error: %s\n", error.toLocal8Bit().constData());
    CHECK(saved2);

    // Reopen from disk as a second document.
    const bool reopened0 = state.openProject(state.activeProjectPath(), &error);
    if (!reopened0) std::fprintf(stderr, "    openProject error: %s\n", error.toLocal8Bit().constData());
    CHECK(reopened0);
    CHECK_EQ(state.documents().size(), 2);
    DocumentItem* reopened = state.activeDocument();
    CHECK(reopened != nullptr);
    if (!reopened) return;
    CHECK(reopened->title == QStringLiteral("New One"));
    CHECK(reopened->size == QSize(320, 200));
    CHECK(reopened->colorMode == QStringLiteral("RGB/8"));
    CHECK(reopened->iccProfile == QByteArray("state-round-trip-icc"));
    CHECK_EQ(reopened->layers.size(), 2);
    CHECK(reopened->layers[0].name == QStringLiteral("red"));
    CHECK(reopened->layers[0].pixels != nullptr);
    CHECK_EQ(reopened->layers[0].pixels->width(), 16u);
    CHECK_EQ(reopened->layers[0].pixels->height(), 16u);
    // The placed red pixel survives the reopen.
    CHECK(reopened->composite.pixelColor(160, 100).red() > 250);
    CHECK(!reopened->dirty);
    CHECK(reopened->filePath == state.activeProjectPath());

    // Both project files ended up in the recent list, most recent first.
    const QStringList recent = state.settings().recentProjects;
    CHECK(recent.contains(QFileInfo(projectPathForName(data.name)).absoluteFilePath()));
}

static void test_text_layer_round_trip() {
    AppState state;
    ProjectFileData data;
    data.name = QStringLiteral("Type Tool");
    data.size = QSize(256, 128);
    data.dpi = 72;
    data.colorMode = QStringLiteral("RGB/8");
    data.background = QStringLiteral("white");
    QString error;
    DocumentItem* doc = state.createNewProject(data, &error);
    CHECK(doc != nullptr);
    if (!doc) return;

    // The Type tool creates a live text layer inside the editing session's undo
    // step, so one Undo removes the layer and everything typed into it.
    state.beginUndoStep();
    const int index = state.addTextLayer(QPointF(20, 40), 0.0,
                                         QStringLiteral("Liberation Sans"), 32.0,
                                         QColor(0, 0, 0));
    CHECK(index >= 0);
    if (index < 0) return;
    CHECK_EQ(doc->layers.size(), 2);
    CHECK(doc->layers[index].liveText);
    CHECK(doc->layers[index].isText);
    CHECK_EQ(doc->layers[index].kind, LayerItem::Kind::Pixel);

    doc->layers[index].textSpec.text = QStringLiteral("Hello");
    // Character-panel attributes are part of the saved spec.
    doc->layers[index].textSpec.underline = 1;
    doc->layers[index].textSpec.strike = 2;
    doc->layers[index].textSpec.underlineColor = QColor(200, 40, 40);
    doc->layers[index].textSpec.strikeColor = QColor(30, 60, 200);
    doc->layers[index].textSpec.backgroundColor = QColor(255, 240, 150);
    doc->layers[index].textSpec.tracking = 3.0;
    doc->layers[index].textSpec.lineHeight = 1.25;
    doc->layers[index].textSpec.baselineShift = 4.0;
    doc->layers[index].textSpec.hScale = 120.0;
    doc->layers[index].textSpec.vScale = 90.0;
    doc->layers[index].textSpec.superSub = 1;
    doc->layers[index].textSpec.allCaps = true;
    doc->layers[index].textSpec.kerning = false;
    doc->layers[index].textSpec.otFeatures =
        pittore::text::OTF_Liga | pittore::text::OTF_Frac;
    const bool rendered = state.refreshTextLayer(index);
    CHECK(rendered);
    CHECK(doc->layers[index].pixels != nullptr);
    state.commitUndoStep(QStringLiteral("Text"), QStringLiteral("type"));

    // The rendered glyphs really landed in the composite (dark ink over the
    // opaque white background), unless the system has no usable font at all.
    if (doc->layers[index].pixels) {
        bool ink = false;
        for (int y = 0; y < doc->composite.height() && !ink; ++y)
            for (int x = 0; x < doc->composite.width(); ++x)
                if (doc->composite.pixelColor(x, y).red() < 128) {
                    ink = true;
                    break;
                }
        CHECK(ink);
    }

    // Save → reload keeps the editable spec, the live flag and the pixels.
    const QString path = state.activeProjectPath();
    const bool saved = state.saveProject(path, &error);
    if (!saved) std::fprintf(stderr, "    saveProject error: %s\n", error.toLocal8Bit().constData());
    CHECK(saved);
    const bool opened = state.openProject(path, &error);
    if (!opened) std::fprintf(stderr, "    openProject error: %s\n", error.toLocal8Bit().constData());
    CHECK(opened);
    DocumentItem* re = state.activeDocument();
    CHECK(re != nullptr);
    if (!re) return;
    const LayerItem* tl = nullptr;
    int ti = -1;
    for (int i = 0; i < re->layers.size(); ++i)
        if (re->layers[i].liveText) {
            tl = &re->layers[i];
            ti = i;
        }
    CHECK(tl != nullptr);
    if (!tl) return;
    CHECK(tl->textSpec.text == QStringLiteral("Hello"));
    CHECK(tl->textSpec.family == QStringLiteral("Liberation Sans"));
    CHECK_NEAR(tl->textSpec.size, 32.0, 1e-9);
    CHECK(tl->isText);
    CHECK(tl->pixels != nullptr);
    // The reloaded text is visible in the composite, not just present.
    if (tl->pixels) {
        bool ink = false;
        for (int y = 0; y < re->composite.height() && !ink; ++y)
            for (int x = 0; x < re->composite.width(); ++x)
                if (re->composite.pixelColor(x, y).red() < 128) {
                    ink = true;
                    break;
                }
        CHECK(ink);
    }
    // The Character-panel attributes survive the round trip.
    CHECK_EQ(tl->textSpec.underline, 1);
    CHECK_EQ(tl->textSpec.strike, 2);
    CHECK(tl->textSpec.underlineColor == QColor(200, 40, 40));
    CHECK(tl->textSpec.strikeColor == QColor(30, 60, 200));
    CHECK(tl->textSpec.backgroundColor == QColor(255, 240, 150));
    CHECK_NEAR(tl->textSpec.tracking, 3.0, 1e-9);
    CHECK_NEAR(tl->textSpec.lineHeight, 1.25, 1e-9);
    CHECK_NEAR(tl->textSpec.baselineShift, 4.0, 1e-9);
    CHECK_NEAR(tl->textSpec.hScale, 120.0, 1e-9);
    CHECK_NEAR(tl->textSpec.vScale, 90.0, 1e-9);
    CHECK_EQ(tl->textSpec.superSub, 1);
    CHECK(tl->textSpec.allCaps);
    CHECK(!tl->textSpec.kerning);
    CHECK_EQ(tl->textSpec.otFeatures,
             unsigned(pittore::text::OTF_Liga | pittore::text::OTF_Frac));

    // Resizing through the transform box re-sets the glyphs at the new point
    // size rather than scaling the old raster, so the run stays sharp and the
    // rendered pixels really do grow.
    if (ti >= 0 && re->layers[ti].pixels) {
        const std::uint32_t w0 = re->layers[ti].pixels->width();
        const QPointF origin = re->layers[ti].textSpec.origin;
        CHECK(state.setLiveTextSize(ti, 48.0, origin));
        CHECK_NEAR(re->layers[ti].textSpec.size, 48.0, 1e-9);
        CHECK(re->layers[ti].pixels != nullptr);
        if (re->layers[ti].pixels) CHECK(re->layers[ti].pixels->width() > w0);
        // Shrinking back is just as lossless: the spec is the source of truth.
        CHECK(state.setLiveTextSize(ti, 16.0, origin));
        CHECK_NEAR(re->layers[ti].textSpec.size, 16.0, 1e-9);
        CHECK(re->layers[ti].pixels != nullptr);
    }

    // A Character-panel edit is one undo step on the active text layer: Undo
    // restores the previous attribute exactly.
    if (ti >= 0) {
        state.setActiveLayerIndex(ti);
        CHECK(state.applyCharacterOption(QStringLiteral("tracking"), 7.0));
        CHECK_NEAR(re->layers[ti].textSpec.tracking, 7.0, 1e-9);
        CHECK(state.canUndo());
        state.undo();
        CHECK_NEAR(re->layers[ti].textSpec.tracking, 3.0, 1e-9);
        // A second identical request changes nothing and leaves no history.
        CHECK(!state.applyCharacterOption(QStringLiteral("tracking"), 3.0));
    }
}

static void test_stale_text_origin_heals() {
    // User file shape: the spec origin went stale (garbage) while the live
    // offset stayed correct, so the text looks fine until reload re-renders
    // it off-canvas. Load must re-anchor from the saved placement.
    AppState state;
    ProjectFileData data;
    data.name = QStringLiteral("Stale Origin");
    data.size = QSize(256, 128);
    data.dpi = 72;
    data.colorMode = QStringLiteral("RGB/8");
    data.background = QStringLiteral("white");
    QString error;
    DocumentItem* doc = state.createNewProject(data, &error);
    CHECK(doc != nullptr);
    if (!doc) return;

    state.beginUndoStep();
    const int index = state.addTextLayer(QPointF(20, 40), 0.0,
                                         QStringLiteral("Liberation Sans"), 32.0,
                                         QColor(0, 0, 0));
    CHECK(index >= 0);
    if (index < 0) return;
    doc->layers[index].textSpec.text = QStringLiteral("Hi");
    CHECK(state.refreshTextLayer(index));
    CHECK(doc->layers[index].pixels != nullptr);
    state.commitUndoStep(QStringLiteral("Text"), QStringLiteral("type"));
    const QPointF goodOffset = doc->layers[index].offset;

    // Corrupt only the origin, the way stale session state does; the live
    // offset (what the user sees) stays put.
    doc->layers[index].textSpec.origin = QPointF(-157901, 264235);
    const QString path = state.activeProjectPath();
    CHECK(state.saveProject(path, &error));
    CHECK(state.openProject(path, &error));
    DocumentItem* re = state.activeDocument();
    CHECK(re != nullptr);
    if (!re) return;
    const LayerItem* tl = nullptr;
    for (int i = 0; i < re->layers.size(); ++i)
        if (re->layers[i].liveText) tl = &re->layers[i];
    CHECK(tl != nullptr);
    if (!tl) return;
    CHECK(tl->pixels != nullptr);
    // Healed back to the saved placement, not the stale origin.
    CHECK((tl->offset - goodOffset).manhattanLength() < 2.0);
    bool ink = false;
    for (int y = 0; y < re->composite.height() && !ink; ++y)
        for (int x = 0; x < re->composite.width(); ++x)
            if (re->composite.pixelColor(x, y).red() < 128) {
                ink = true;
                break;
            }
    CHECK(ink);
}

static void test_adjustment_round_trip() {
    // Live adjustments survive a save → open cycle with parameters, curve
    // points and the derived LUT intact, and keep affecting the composite.
    AppState state;
    ProjectFileData data;
    data.name = QStringLiteral("Adjustments");
    data.size = QSize(64, 48);
    data.dpi = 72;
    data.colorMode = QStringLiteral("RGB/8");
    data.background = QStringLiteral("white");
    QString error;
    DocumentItem* doc = state.createNewProject(data, &error);
    CHECK(doc != nullptr);
    if (!doc) return;

    QImage grey(16, 16, QImage::Format_ARGB32_Premultiplied);
    grey.fill(0xFF808080);
    state.placeImageLayer(grey, QStringLiteral("grey"), QPointF(32, 24), 1.0);
    const QColor before = doc->composite.pixelColor(32, 24);
    CHECK(qAbs(before.red() - 128) < 4);

    using pittore::compute::AdjustmentKind;
    CHECK(state.addAdjustmentLayer(
        static_cast<int>(AdjustmentKind::BrightnessContrast)));
    state.setActiveLayerIndex(0);
    CHECK(state.setAdjustmentParam(0, 0.2f));  // brightness +0.2
    const QColor brighter = doc->composite.pixelColor(32, 24);
    CHECK(brighter.red() > before.red() + 20);

    CHECK(state.addAdjustmentLayer(
        static_cast<int>(AdjustmentKind::Curves)));
    state.setActiveLayerIndex(0);
    CHECK(state.setAdjustmentCurve({{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}}));
    const QColor curved = doc->composite.pixelColor(32, 24);
    CHECK(curved.green() > brighter.green());

    CHECK(state.saveProject(state.activeProjectPath(), &error));
    CHECK(state.openProject(state.activeProjectPath(), &error));
    DocumentItem* re = state.activeDocument();
    CHECK(re != nullptr);
    if (!re) return;
    CHECK_EQ(re->layers.size(), 4);
    // Panel order top-first: curves, brightness, grey, Background.
    const LayerItem& curves = re->layers[0];
    const LayerItem& bright = re->layers[1];
    CHECK_EQ(curves.kind, LayerItem::Kind::Adjustment);
    CHECK_EQ(curves.adjustmentKind, static_cast<int>(AdjustmentKind::Curves));
    CHECK_EQ(curves.adjustmentCurve.size(), 3);
    CHECK_EQ(curves.adjustmentLUT.size(), 768u);  // 3x256 R/G/B, master folded
    CHECK_NEAR(curves.adjustmentCurve[1].y(), 0.75, 1e-9);
    CHECK_EQ(bright.adjustmentKind,
             static_cast<int>(AdjustmentKind::BrightnessContrast));
    CHECK_NEAR(bright.adjustmentParams[0], 0.2f, 1e-6f);
    // The reopened composite matches the pre-save one exactly.
    CHECK(re->composite.pixelColor(32, 24) == curved);
}

static void test_legacy_ifp_scan() {
    // Pre-.psc libraries still open: a legacy .ifp written by an old version
    // is listed by the library scan and loads through the same codec.
    ProjectFileData data = sampleData();
    data.name = QStringLiteral("Legacy Probe");
    const QString path =
        QDir(projectsRootDir()).filePath(data.name + QStringLiteral(".ifp"));
    QFile::remove(path);
    QString error;
    CHECK(saveProjectFile(path, data, &error));
    const QVector<ProjectEntry> entries = scanProjects();
    bool found = false;
    for (const ProjectEntry& e : entries) {
        if (e.name == data.name) {
            found = true;
            CHECK(QFileInfo(e.path).absoluteFilePath() ==
                  QFileInfo(path).absoluteFilePath());
        }
    }
    CHECK(found);
    ProjectFileData loaded;
    CHECK(loadProjectFile(path, &loaded, &error));
    CHECK(loaded.name == data.name);
    QFile::remove(path);
}

static void test_board_round_trip() {
    // Boards survive the .psc cycle: two boards (one with a fill) rebuild with
    // the same ids, rects, fill and panel order, and the id counter resumes
    // above every loaded board so a new board never collides (INV-006).
    AppState state;
    ProjectFileData data;
    data.name = QStringLiteral("Boards");
    data.size = QSize(400, 300);
    data.dpi = 96;
    data.colorMode = QStringLiteral("RGB/8");
    data.background = QStringLiteral("white");
    QString error;
    DocumentItem* doc = state.createNewProject(data, &error);
    CHECK(doc != nullptr);
    if (!doc) return;

    const std::uint64_t a = state.createBoard(QRectF(10, 20, 180, 120),
                                              QStringLiteral("Board A"));
    const std::uint64_t b = state.createBoard(QRectF(210, 20, 160, 120),
                                              QStringLiteral("Board B"));
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(state.setBoardFill(a, true, QColor(20, 120, 220), 0.5));
    CHECK_EQ(doc->boardOrder.size(), 2);

    const bool saved = state.saveProject(state.activeProjectPath(), &error);
    if (!saved)
        std::fprintf(stderr, "    saveProject error: %s\n",
                     error.toLocal8Bit().constData());
    CHECK(saved);

    const bool reopened0 = state.openProject(state.activeProjectPath(), &error);
    if (!reopened0)
        std::fprintf(stderr, "    openProject error: %s\n",
                     error.toLocal8Bit().constData());
    CHECK(reopened0);
    DocumentItem* r = state.activeDocument();
    CHECK(r != nullptr);
    if (!r) return;

    // createBoard prepends, so B is the top board (row 0) and A below it;
    // boardOrder lists them in that panel order.
    CHECK(r->boardOrderIsPermutation());
    CHECK_EQ(r->boardOrder.size(), 2);
    CHECK_EQ(r->boardOrder[0], b);
    CHECK_EQ(r->boardOrder[1], a);
    const int ra = r->boardRowIndex(a);
    const int rb = r->boardRowIndex(b);
    CHECK_EQ(rb, 0);
    CHECK_EQ(ra, 1);
    CHECK(ra >= 0 && rb >= 0);
    if (ra < 0 || rb < 0) return;

    CHECK(r->layers[ra].isBoard());
    CHECK_EQ(r->layers[ra].boardId, a);
    CHECK_NEAR(r->layers[ra].boardRect.x(), 10.0, 1e-9);
    CHECK_NEAR(r->layers[ra].boardRect.y(), 20.0, 1e-9);
    CHECK_NEAR(r->layers[ra].boardRect.width(), 180.0, 1e-9);
    CHECK_NEAR(r->layers[ra].boardRect.height(), 120.0, 1e-9);
    CHECK(r->layers[ra].boardHasFill);
    CHECK_EQ(r->layers[ra].boardFill.red(), 20);
    CHECK_EQ(r->layers[ra].boardFill.green(), 120);
    CHECK_EQ(r->layers[ra].boardFill.blue(), 220);
    CHECK_NEAR(r->layers[ra].boardFillOpacity, 0.5, 1e-6);

    CHECK(r->layers[rb].isBoard());
    CHECK_EQ(r->layers[rb].boardId, b);
    CHECK_NEAR(r->layers[rb].boardRect.x(), 210.0, 1e-9);
    CHECK(!r->layers[rb].boardHasFill);

    // The id counter resumed above both loaded boards, so a board created
    // after the reload gets a fresh id (INV-006).
    CHECK(r->boardIdCounter >= a && r->boardIdCounter >= b);
    const std::uint64_t c = state.createBoard(QRectF(0, 200, 60, 60),
                                              QStringLiteral("Board C"));
    CHECK(c != 0 && c != a && c != b);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir = QDir::tempPath() + QStringLiteral("/pittore-project-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    std::printf("  [project] projects root: %s\n",
                pittore::ui::projectsRootDir().toLocal8Bit().constData());
    test_file_round_trip();
    test_legacy_ifp_scan();
    test_app_state_cycle();
    test_text_layer_round_trip();
    test_board_round_trip();
    test_stale_text_origin_heals();
    test_adjustment_round_trip();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}