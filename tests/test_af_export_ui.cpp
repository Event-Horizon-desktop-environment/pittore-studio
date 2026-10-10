// test_af_export_ui.cpp — Affinity export through the live UI model:
// buildAfLayersDoc structure + baked notes, then the full exportAfLayers
// path (built-in template, composite thumbnail) and a decode + reimport
// circle.
//
// AppState harness like test_psd_export (headless, scratch XDG_CONFIG_HOME).
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

#include <cmath>

#include "engine/compute/layer_mask.h"
#include "engine/core/log.h"
#include "engine/io/af_layers.h"
#include "engine/io/af_layers/emit/af_write.h"
#include "test_util.h"
#include "ui/af_export.h"
#include "ui/app_state.h"
#include "ui/app_state_detail.h"

using namespace pittore::ui;

namespace {

std::shared_ptr<pittore::Image> solidImg(int w, int h, float r, float g,
                                          float b, float a = 1.0f) {
    auto img = std::make_shared<pittore::Image>(
        static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (int i = 0; i < w * h; ++i) img->data()[i] = {r, g, b, a};
    return img;
}

LayerItem pixelLayer(const QString& name, std::shared_ptr<pittore::Image> px) {
    LayerItem l;
    l.name = name;
    l.kind = LayerItem::Kind::Pixel;
    l.pixels = std::move(px);
    l.offset = QPointF(0, 0);
    l.scaleX = l.scaleY = 1.0;
    if (l.pixels) l.sourceStamp = 1;
    return l;
}

}  // namespace

static void test_af_export_ui() {
    AppState state;
    DocumentItem* doc = state.addDocument(QStringLiteral("t"), QSize(8, 4), 72);
    CHECK(doc != nullptr);
    if (!doc) return;

    LayerItem clip = pixelLayer(QStringLiteral("clip"), solidImg(8, 4, 0, 0, 1));
    clip.clipped = true;
    {
        auto m = std::make_shared<pittore::Image>(8, 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 8; ++x)
                m->data()[y * 8 + x] =
                    pittore::compute::make_mask_pixel(x < 4 ? 1.0f : 0.0f);
        clip.mask = std::move(m);
        clip.hasMask = true;
        clip.maskEnabled = true;
        clip.maskOffset = QPointF(0, 0);
        clip.maskScaleX = clip.maskScaleY = 1.0;
    }

    LayerItem curves;
    curves.name = QStringLiteral("curves");
    curves.kind = LayerItem::Kind::Adjustment;
    curves.adjustmentKind = 3;
    curves.adjustmentCurve = {QPointF(0, 0), QPointF(0.5, 0.75), QPointF(1, 1)};

    LayerItem group;
    group.name = QStringLiteral("G");
    group.kind = LayerItem::Kind::Group;
    group.indent = 0;

    LayerItem child = pixelLayer(QStringLiteral("green"), solidImg(4, 4, 0, 1, 0));
    child.offset = QPointF(0, 0);
    child.indent = 1;

    LayerItem bg = pixelLayer(QStringLiteral("bg"), solidImg(8, 4, 1, 0, 0));

    // Panel order, top first.
    doc->layers = {clip, curves, group, child, bg};
    doc->rebuildComposite();

    // Structure: adjustments drop with a bake note, the rest lands bottom-first.
    AfExportDoc exp = buildAfLayersDoc(*doc);
    CHECK_EQ(exp.doc.width, 8u);
    CHECK_EQ(exp.doc.height, 4u);
    bool bakedCurves = false;
    for (const auto& b : exp.baked)
        if (b.find("curves") != std::string::npos) bakedCurves = true;
    CHECK(bakedCurves);
    // File order bottom -> top: bg, group, child, clip.
    CHECK_EQ(exp.doc.layers.size(), 4u);
    if (exp.doc.layers.size() != 4) return;
    CHECK(exp.doc.layers[0].name == "bg");
    CHECK(exp.doc.layers[1].isGroup);
    CHECK(exp.doc.layers[2].name == "green");
    CHECK_EQ(exp.doc.layers[2].indent, 1);
    CHECK(exp.doc.layers[3].name == "clip" && exp.doc.layers[3].clipped);
    CHECK_EQ(exp.doc.layers[3].masks.size(), 1u);

    // Full path: template + thumbnail + encode, then decode + reimport.
    QString error;
    auto enc = state.exportAfLayers(*doc, &error);
    if (!enc) {
        std::fprintf(stderr, "    export error: %s\n",
                      error.toLocal8Bit().constData());
        CHECK(false);
        return;
    }
    CHECK(!enc->bytes.empty());
    std::string codecError;
    auto dec = pittore::io::afDecodeLayers(enc->bytes, &codecError);
    CHECK(dec.has_value());
    if (!dec) {
        std::fprintf(stderr, "    decode error: %s\n", codecError.c_str());
        return;
    }
    CHECK_EQ(dec->layers.size(), 4u);

    const bool opened = state.openAfLayers(QStringLiteral("t.afphoto"), *dec,
                                           QSize(8, 4), QImage(), &error);
    CHECK(opened);
    if (opened) {
        DocumentItem* re = state.activeDocument();
        CHECK(re != nullptr);
        if (re) {
            CHECK_EQ(re->layers.size(), 4);
            bool haveBg = false, haveGreen = false;
            for (const LayerItem& l : re->layers) {
                if (l.name == QStringLiteral("bg")) haveBg = true;
                if (l.name == QStringLiteral("green")) haveGreen = true;
            }
            CHECK(haveBg && haveGreen);
        }
    }
}

// Paint, move, opacity, mask and new-layer edits all survive an
// export → decode round trip: saving edits to .af works.
static void test_af_edit_cycle() {
    AppState state;
    DocumentItem* doc = state.addDocument(QStringLiteral("e"), QSize(8, 4), 72);
    CHECK(doc != nullptr);
    if (!doc) return;
    LayerItem bg = pixelLayer(QStringLiteral("bg"), solidImg(8, 4, 0, 0, 1));
    doc->layers = {bg};
    doc->rebuildComposite();

    QString error;
    auto enc = state.exportAfLayers(*doc, &error);
    CHECK(enc.has_value());
    if (!enc) return;
    std::string codecError;
    auto dec = pittore::io::afDecodeLayers(enc->bytes, &codecError);
    CHECK(dec.has_value());
    if (!dec) return;
    CHECK(state.openAfLayers(QStringLiteral("e.afphoto"), *dec, QSize(8, 4), QImage(),
                             &error));
    DocumentItem* re = state.activeDocument();
    CHECK(re != nullptr && !re->layers.empty());
    if (!re || re->layers.empty()) return;

    // Edits: paint a pixel, move the layer, change opacity, mask one
    // pixel out, add a layer.
    LayerItem& l = re->layers[0];
    l.pixels->data()[0] = {1.0f, 1.0f, 1.0f, 1.0f};
    ++l.sourceStamp;
    l.offset = QPointF(1, 0);
    l.opacity = 50;
    {
        auto m = std::make_shared<pittore::Image>(8, 4);
        m->fill(pittore::compute::make_mask_pixel(1.0f));
        m->data()[2] = pittore::compute::make_mask_pixel(0.0f);
        l.mask = std::move(m);
        l.hasMask = true;
        l.maskEnabled = true;
        l.maskOffset = QPointF(0, 0);
        l.maskScaleX = l.maskScaleY = 1.0;
        ++l.maskStamp;
    }
    LayerItem extra = pixelLayer(QStringLiteral("extra"), solidImg(2, 2, 0, 1, 0));
    extra.offset = QPointF(6, 2);
    re->layers.append(extra);
    re->rebuildComposite();

    auto enc2 = state.exportAfLayers(*re, &error);
    CHECK(enc2.has_value());
    if (!enc2) return;
    auto dec2 = pittore::io::afDecodeLayers(enc2->bytes, &codecError);
    CHECK(dec2.has_value());
    if (!dec2 || dec2->layers.size() != 2) return;
    const auto& paint = dec2->layers[1];
    CHECK(paint.name == "bg");
    CHECK_EQ(paint.opacity, 128u);  // qRound(50 * 255 / 100)
    CHECK_EQ(paint.left, 1);
    CHECK_EQ(paint.top, 0);
    // Painted white pixel at doc (0,0) moved with the layer to (1,0);
    // the footprint clips to the canvas (7x4). The masked-out pixel at
    // doc (2,0) lands at footprint index 1.
    CHECK(paint.rgba.size() == 7u * 4u * 4u);
    if (paint.rgba.size() == 7u * 4u * 4u) {
        CHECK(paint.rgba[0] == 65535 && paint.rgba[1] == 65535 &&
              paint.rgba[2] == 65535);
        CHECK_EQ(paint.masks.size(), 1u);
        if (!paint.masks.empty()) CHECK(paint.masks[0].px[1] == 0);
    }
    CHECK(dec2->layers[0].name == "extra");
}

// saveProject() writes .af* through the exporter, adopts the path (so a
// later Save writes back), and re-saving after edits picks them up.
static void test_af_save_adopts() {
    AppState state;
    DocumentItem* doc = state.addDocument(QStringLiteral("s"), QSize(8, 4), 72);
    CHECK(doc != nullptr);
    if (!doc) return;
    doc->layers = {pixelLayer(QStringLiteral("bg"), solidImg(8, 4, 0, 0, 1))};
    doc->rebuildComposite();

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    if (!tmp.isValid()) return;
    const QString path = tmp.filePath(QStringLiteral("s.afphoto"));
    QString error;
    CHECK(state.saveProject(path, &error));
    CHECK(doc->filePath == QFileInfo(path).absoluteFilePath());
    CHECK(!doc->dirty);

    // Edit, then Save to the adopted path (the Ctrl+S route).
    doc->layers[0].pixels->data()[0] = {0.0f, 1.0f, 0.0f, 1.0f};
    ++doc->layers[0].sourceStamp;
    doc->dirty = true;
    CHECK(state.saveProject(doc->filePath, &error));
    CHECK(!doc->dirty);

    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    if (!f.isOpen()) return;
    const QByteArray raw = f.readAll();
    std::string codecError;
    auto dec = pittore::io::afDecodeLayers(
        std::vector<std::uint8_t>(raw.constBegin(), raw.constEnd()), &codecError);
    CHECK(dec.has_value());
    if (!dec || dec->layers.size() != 1) return;
    // Painted green pixel survived the save.
    CHECK(dec->layers[0].rgba[0] == 0 && dec->layers[0].rgba[1] == 65535 &&
          dec->layers[0].rgba[2] == 0);
}

// Layers imported hidden behind a flattened base keep their native pixels
// packed in LayerItem::deferredRgba8 (4 B/px) instead of realising them as
// RGBAf (16 B/px). Every consumer — save, export, the visibility toggle — has
// to reach the same pixels whichever state the layer is in, and crossing
// between the two states must not change a sample.
static void test_stashed_pixels() {
    AppState state;
    constexpr int kW = 4, kH = 2;
    QByteArray expect(kW * kH * 4, Qt::Uninitialized);
    for (int i = 0; i < expect.size(); ++i)
        expect[i] = static_cast<char>((i * 37 + 5) & 0xff);

    pittore::io::AfLayersDoc pd;
    pd.width = kW;
    pd.height = kH;
    pd.complete = false;  // something was skipped -> flattened base wins
    pittore::io::AfLayer l;
    l.name = "packed";
    l.width = kW;
    l.height = kH;
    l.rgba.resize(expect.size());
    for (int i = 0; i < expect.size(); ++i)
        l.rgba[static_cast<std::size_t>(i)] =
            static_cast<std::uint16_t>(static_cast<uchar>(expect[i])) * 257u;
    pd.layers.push_back(std::move(l));

    QImage flat(kW, kH, QImage::Format_RGBA64);
    flat.fill(Qt::white);
    QString error;
    CHECK(state.openAfLayers(QStringLiteral("st.af"), pd, QSize(kW, kH), flat,
                             &error));
    DocumentItem* d = state.activeDocument();
    CHECK(d != nullptr);
    if (!d) return;
    // Top-first panel: the recovered layer first, the flattened base below it.
    CHECK_EQ(d->layers.size(), 2);
    if (d->layers.size() != 2) return;
    // Visibility contract: recovered layers open with the file's own
    // visibility and realise from the packed stash on the first composite;
    // the flattened preview stays as the hidden reference copy below them.
    LayerItem& recovered = d->layers[0];
    CHECK(recovered.visible);
    CHECK(recovered.pixels != nullptr);
    CHECK(!hasDeferredPixels(recovered));
    CHECK(!d->layers[1].visible);
    // The decode buffer it came from was released as it was consumed.
    CHECK(pd.layers.empty() || pd.layers[0].rgba.empty());

    // Hide the layer to reach the packed state: the stash is exactly the
    // decoder's 8-bit source, u16>>8.
    recovered.visible = false;
    stashDeferredPixels(*d, QVector<int>{0});
    LayerItem& stashed = d->layers[0];
    CHECK(stashed.pixels == nullptr);
    CHECK(hasDeferredPixels(stashed));
    CHECK_EQ(int(stashed.deferredWidth), kW);
    CHECK_EQ(int(stashed.deferredHeight), kH);
    CHECK(stashed.deferredRgba8 == expect);

    // Save straight out of the packed state.
    const ProjectFileData saved = projectDataFromDocument(*d);
    CHECK_EQ(saved.layers.size(), 2);
    CHECK(!saved.layers[0].pixels.isNull());
    CHECK_EQ(saved.layers[0].pixels.width(), kW);
    CHECK_EQ(saved.layers[0].pixels.height(), kH);

    // Realise for the read; saving must produce byte-identical output either
    // way, then the layer packs straight back.
    const QVector<int> expanded = materializeDeferredPixels(*d);
    CHECK_EQ(expanded.size(), 1);
    CHECK(stashed.pixels != nullptr);
    CHECK(!hasDeferredPixels(stashed));
    CHECK_EQ(int(stashed.pixels->width()), kW);
    CHECK_EQ(int(stashed.pixels->height()), kH);
    if (stashed.pixels)
        CHECK(std::abs(stashed.pixels->data()[1].g -
                       static_cast<uchar>(expect[5]) / 255.0f) < 1e-6f);
    const ProjectFileData realised = projectDataFromDocument(*d);
    CHECK_EQ(saved.layers[0].pixels.sizeInBytes(),
             realised.layers[0].pixels.sizeInBytes());
    CHECK(saved.layers[0].pixels.sizeInBytes() > 0);
    CHECK(std::memcmp(saved.layers[0].pixels.constBits(),
                      realised.layers[0].pixels.constBits(),
                      saved.layers[0].pixels.sizeInBytes()) == 0);
    stashDeferredPixels(*d, expanded);
    CHECK(stashed.pixels == nullptr);
    CHECK(hasDeferredPixels(stashed));
    CHECK(stashed.deferredRgba8 == expect);

    // Export from the packed state: the encoder has to reach the pixels, and
    // the document must not be left inflated afterwards.
    QString exportError;
    auto enc = state.exportAfLayers(*d, &exportError);
    if (!enc)
        std::fprintf(stderr, "    export error: %s\n",
                     exportError.toLocal8Bit().constData());
    CHECK(enc.has_value());
    if (enc) {
        std::string codecError;
        auto dec = pittore::io::afDecodeLayers(enc->bytes, &codecError);
        CHECK(dec.has_value());
        bool sawLayer = false;
        if (dec) {
            // File order is bottom-first, so find ours by name rather than
            // by position (the flattened base is exported too).
            for (const auto& el : dec->layers) {
                if (el.name != "packed") continue;
                sawLayer = true;
                CHECK_EQ(static_cast<long long>(el.rgba.size()), expect.size());
                if (static_cast<long long>(el.rgba.size()) == expect.size())
                    CHECK_EQ(int(el.rgba[5]), int(static_cast<uchar>(expect[5])) * 257);
            }
        }
        CHECK(sawLayer);
    }
    CHECK(hasDeferredPixels(stashed));
    CHECK(stashed.deferredRgba8 == expect);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =        QDir::tempPath() + QStringLiteral("/pittore-af-export-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_af_export_ui();
    test_af_edit_cycle();
    test_af_save_adopts();
    test_stashed_pixels();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
