// test_psd_export.cpp — layered PSD export (Phase 4A): DocumentItem model to
// PSD file bytes and back.
//
// Builds a small document with a group (folded opacity), a clipped layer, a
// live Curves adjustment, a mask and a hidden layer; exports via
// AppState::exportLayeredPsd; then verifies the decode (unfolded records),
// the full circle through openPsdLayers (refolded panel values), and the
// file structure through ImageMagick's `identify` (when available).
//
// Runs headless under QCoreApplication like test_place_ui.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/compute/layer_mask.h"
#include "engine/color/cmyk_image.h"
#include "engine/core/log.h"
#include "engine/io/psd.h"
#include "test_util.h"
#include "ui/app_state.h"
#include "ui/color_mode.h"
#include "ui/psd_export.h"

using namespace pittore::ui;

namespace {

std::shared_ptr<pittore::Image> solidImg(int w, int h, float r, float g,
                                          float b, float a = 1.0f) {
    auto img = std::make_shared<pittore::Image>(
        static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (int i = 0; i < w * h; ++i) img->data()[i] = {r, g, b, a};
    return img;
}

LayerItem pixelLayer(const QString& name,
                     std::shared_ptr<pittore::Image> px) {
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

// Foreign PSD blocks (TySh, SoLd, ...) survive a UI export untouched, and
// are dropped the moment the layer is painted: stale descriptors must
// never describe new pixels.
static void test_rawblock_carry() {
    AppState state;
    DocumentItem* doc = state.addDocument(QStringLiteral("r"), QSize(4, 2), 72);
    CHECK(doc != nullptr);
    if (!doc) return;

    LayerItem l = pixelLayer(QStringLiteral("type-ish"), solidImg(4, 2, 1, 0, 0));
    LayerItem::PsdRawBlock rb;
    std::memcpy(rb.sig, "8BIM", 4);
    std::memcpy(rb.key, "TySh", 4);
    rb.data = {9, 8, 7, 6, 5};  // odd size: exercises the pad path
    l.psdRawBlocks.push_back(rb);
    l.psdChannelMethod = 2;  // ZIP origin: kept under default RLE encode
    l.psdRawSrc = l.sourceStamp;
    l.psdRawAdj = l.adjustStamp;
    l.psdRawMask = l.maskStamp;
    l.psdRawHadMask = l.hasMask;
    l.psdRawKind = l.kind;
    l.psdRawOffset = l.offset;
    l.psdRawScaleX = l.scaleX;
    l.psdRawScaleY = l.scaleY;
    doc->layers = {l};

    QString error;
    auto bytes = state.exportLayeredPsd(*doc, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;
    {
        std::string codecError;
        auto dec = pittore::io::psdDecodeLayers(*bytes, &codecError);
        CHECK(dec.has_value());
        if (!dec) return;
        bool kept = false;
        bool zipKept = false;
        for (const auto& layer : dec->layers) {
            for (const auto& b : layer.rawBlocks)
                if (std::memcmp(b.key, "TySh", 4) == 0 && b.data == rb.data)
                    kept = true;
            if (layer.channelMethod == 2) zipKept = true;
        }
        CHECK(kept);
        CHECK(zipKept);
    }
    // Simulate a paint dab: the stamp moves, the block must go.
    doc->layers[0].sourceStamp = l.sourceStamp + 1;
    auto bytes2 = state.exportLayeredPsd(*doc, &error);
    CHECK(bytes2.has_value());
    if (!bytes2) return;
    {
        std::string codecError;
        auto dec = pittore::io::psdDecodeLayers(*bytes2, &codecError);
        CHECK(dec.has_value());
        if (!dec) return;
        bool kept = false;
        for (const auto& layer : dec->layers)
            for (const auto& b : layer.rawBlocks)
                if (std::memcmp(b.key, "TySh", 4) == 0) kept = true;
        CHECK(!kept);
    }
}

static void test_layered_export() {
    AppState state;
    DocumentItem* doc = state.addDocument(QStringLiteral("t"), QSize(8, 4), 72);
    CHECK(doc != nullptr);
    if (!doc) return;

    // Panel order, top first: hidden, clipped+masked, curves, group, child
    // (folded to 50 by the group), background.
    LayerItem hidden = pixelLayer(QStringLiteral("hidden"),
                                  solidImg(8, 4, 1, 1, 1));
    hidden.visible = false;

    LayerItem clip = pixelLayer(QStringLiteral("clip"),
                                solidImg(8, 4, 0, 0, 1));
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
    curves.adjustmentKind = 3;  // Curves
    curves.adjustmentCurve = {QPointF(0, 0), QPointF(0.5, 0.75),
                              QPointF(1, 1)};

    LayerItem group;
    group.name = QStringLiteral("G");
    group.kind = LayerItem::Kind::Group;
    group.opacity = 50;
    group.indent = 0;

    // Panel children carry FOLDED values (what the importer produces): the
    // group halves this layer, so 100 folds to 50.
    LayerItem child = pixelLayer(QStringLiteral("green"),
                                 solidImg(4, 4, 0, 1, 0));
    child.offset = QPointF(0, 0);
    child.opacity = 50;
    child.indent = 1;

    LayerItem bg = pixelLayer(QStringLiteral("bg"),
                              solidImg(8, 4, 1, 0, 0));

    doc->layers = {hidden, clip, curves, group, child, bg};

    QString error;
    auto bytes = state.exportLayeredPsd(*doc, &error);
    if (!bytes)
        std::fprintf(stderr, "    export error: %s\n",
                     error.toLocal8Bit().constData());
    CHECK(bytes.has_value());
    if (!bytes) return;

    std::string codecError;
    auto dec = pittore::io::psdDecodeLayers(*bytes, &codecError);
    CHECK(dec.has_value());
    if (!dec) return;
    CHECK_EQ(dec->width, 8u);
    CHECK_EQ(dec->height, 4u);
    // File order bottom -> top: bg, group, child, curves, clip, hidden.
    // (Each group record precedes its children, as the decoder expects.)
    CHECK_EQ(dec->layers.size(), 6u);
    if (dec->layers.size() != 6) return;
    const auto& L = dec->layers;
    CHECK(L[0].name == "bg" && !L[0].isGroup && !L[0].isAdjustment);
    CHECK(L[0].rgba.size() == 8u * 4u * 4u);
    CHECK(L[0].rgba[0] == 65535 && L[0].rgba[1] == 0 &&
          L[0].rgba[2] == 0 && L[0].rgba[3] == 65535);
    // Unfolded: the group keeps its own 50, the child leaves as 100 (255).
    CHECK(L[1].name == "G" && L[1].isGroup &&
          L[1].opacity == 128);  // qRound(50 * 255 / 100)
    CHECK(L[2].name == "green" && L[2].indent == 1 &&
          L[2].opacity == 255);
    CHECK(L[2].rgba.size() == 4u * 4u * 4u);
    // Null bounds (as written for 0-channel records), params live
    // in the descriptor block.
    CHECK(L[3].isAdjustment && L[3].adjustmentKind == 3 &&
          L[3].width == 0 && L[3].height == 0);
    CHECK(L[3].adjustmentCurve.size() == 3);
    CHECK(L[4].name == "clip" && L[4].clipped && L[4].hasMask);
    bool maskOk = L[4].mask.size() == 8u * 4u;
    for (std::size_t k = 0; maskOk && k < L[4].mask.size(); ++k) {
        const bool left = (k % 8) < 4;
        if (L[4].mask[k] != (left ? 65535u : 0u)) maskOk = false;
    }
    CHECK(maskOk);
    CHECK(L[5].name == "hidden" && !L[5].visible);

    // Re-encoding the decode is byte-identical.
    auto again = pittore::io::psdEncodeLayers(*dec, &codecError);
    CHECK(again.has_value() && *again == *bytes);

    // Full circle through the real importer: the group refolds the child.
    const bool opened = state.openPsdLayers(QStringLiteral("t.psd"), *dec,
                                            &error);
    CHECK(opened);
    if (opened) {
        DocumentItem* re = state.activeDocument();
        CHECK(re != nullptr);
        if (re) {
            CHECK_EQ(re->layers.size(), 6);
            int gi = -1, ci = -1;
            for (int i = 0; i < re->layers.size(); ++i) {
                if (re->layers[i].name == QStringLiteral("G")) gi = i;
                if (re->layers[i].name == QStringLiteral("green")) ci = i;
            }
            CHECK(gi >= 0 && ci >= 0);
            if (gi >= 0 && ci >= 0) {
                CHECK_EQ(re->layers[gi].opacity, 50);
                CHECK_EQ(re->layers[ci].opacity, 50);  // refolded
                CHECK_EQ(re->layers[ci].indent, 1);
            }
        }
    }

    // Structural check with ImageMagick when it is installed: every record
    // (plus the composite) shows up as a scene at canvas geometry.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    if (tmp.isValid()) {
        const QString path = tmp.filePath(QStringLiteral("export.psd"));
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        if (f.isOpen()) {
            f.write(reinterpret_cast<const char*>(bytes->data()),
                    static_cast<qint64>(bytes->size()));
            f.close();
            const QString magick = QStringLiteral("/usr/sbin/magick");
            if (QFile::exists(magick)) {
                QProcess proc;
                proc.start(magick, {QStringLiteral("identify"),
                                    QStringLiteral("-format"),
                                    QStringLiteral("%g:%s\\n"), path});
                CHECK(proc.waitForFinished(30000));
                CHECK_EQ(proc.exitCode(), 0);
                const QString out =
                    QString::fromLocal8Bit(proc.readAllStandardOutput());
                const QStringList lines =
                    out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                // Composite + the 4 pixel layers; the 0-channel group and
                // adjustment rows are structure, not scenes. Scene 0 is the
                // 8x4 composite; the 4x4 child reports its own geometry.
                CHECK_EQ(lines.size(), 5);
                CHECK(lines[0].startsWith(QStringLiteral("8x4")));
                int small = 0;
                for (const QString& line : lines) {
                    if (line.startsWith(QStringLiteral("4x4"))) ++small;
                    CHECK(line.startsWith(QStringLiteral("8x4")) ||
                          line.startsWith(QStringLiteral("4x4")));
                }
                CHECK_EQ(small, 1);
            } else {
                std::printf("    magick missing, scene check skipped\\n");
            }
        }
    }
}

static void test_cmyk_profile_guard() {
    AppState state;
    DocumentItem* doc =
        state.addDocument(QStringLiteral("inks"), QSize(2, 1), 300);
    CHECK(doc != nullptr);
    if (!doc) return;
    doc->colorMode = QStringLiteral("CMYK/16");
    LayerItem layer = pixelLayer(QStringLiteral("inks"),
                                 solidImg(2, 1, 0.2f, 0.4f, 0.6f));
    auto inks = std::make_shared<pittore::color::CmykImage>();
    inks->width = 2;
    inks->height = 1;
    inks->pixels = {{{1000, 2000, 3000, 4000}},
                    {{5000, 6000, 7000, 8000}}};
    inks->appearanceFingerprint =
        pittore::color::CmykImage::fingerprint(*layer.pixels);
    const QByteArray profile = resolveCmykProfile(*doc);
    inks->profileFingerprint = pittore::color::CmykImage::fingerprint(
        reinterpret_cast<const std::uint8_t*>(profile.constData()),
        static_cast<std::size_t>(profile.size()));
    layer.cmykPixels = inks;
    doc->layers = {layer};

    const auto preserved = buildLayeredPsdDoc(*doc);
    CHECK_EQ(preserved.layers.size(), 1u);
    if (preserved.layers.size() == 1)
        CHECK(preserved.layers[0].cmyk ==
              std::vector<std::uint16_t>({1000, 2000, 3000, 4000,
                                          5000, 6000, 7000, 8000}));

    layer.cmykPixels = std::make_shared<pittore::color::CmykImage>(*inks);
    layer.cmykPixels->profileFingerprint ^= 0x1u;
    doc->layers = {layer};
    const auto separated = buildLayeredPsdDoc(*doc);
    CHECK_EQ(separated.layers.size(), 1u);
    if (separated.layers.size() == 1)
        CHECK(separated.layers[0].cmyk.empty());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString logDir =
        QDir::tempPath() + QStringLiteral("/pittore-psd-export-log");
    QDir().mkpath(logDir);
    ::pittore::core::log::set_log_dir(logDir.toStdString().c_str());
    test_layered_export();
    test_rawblock_carry();
    test_cmyk_profile_guard();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
