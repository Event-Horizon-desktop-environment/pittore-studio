// test_project_file.cpp — IFP own-format codec (engine/io/project.{h,cpp}),
// the PSD-inspired single-file project format: fixed big-endian header with
// length-prefixed metadata / layers / preview sections, per-layer deflated
// straight RGBA16 samples, and a scan path that skips layer pixels.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/io/project.h"
#include "test_util.h"

using pittore::io::ProjectFileDoc;
using pittore::io::ProjectLayerFile;

namespace {

ProjectFileDoc sampleProject() {
    ProjectFileDoc p;
    p.width = 320;
    p.height = 200;
    p.dpi = 150;
    p.colorMode = 3;
    p.name = "Round Trip";
    p.colorModeName = "RGB/8";
    p.background = "white";

    ProjectLayerFile bg;
    bg.name = "Background";
    bg.kind = 0;
    bg.visible = true;
    bg.locked = true;
    bg.width = 320;
    bg.height = 200;
    // Straight alpha: half-transparent center with strong RGB — any
    // premultiplication round trip would corrupt it (a=0.5,r=0.4 -> r=0.8).
    bg.rgba.assign(320u * 200u * 4u, 65535);
    const std::size_t c = (1u * 320u + 2u) * 4u;
    bg.rgba[c + 0] = 0x6666;  // r
    bg.rgba[c + 1] = 0x4CCC;  // g
    bg.rgba[c + 2] = 0x3333;  // b
    bg.rgba[c + 3] = 0x8000;  // a = 0.5

    ProjectLayerFile top;
    top.name = "Detail";
    top.kind = 0;
    top.visible = true;
    top.opacity = 80;
    top.blend = "Multiply";
    top.offsetX = 12.25;
    top.offsetY = -3.5;
    top.scaleX = 1.5;
    top.scaleY = 0.75;
    // metadata-only layer: no pixels
    top.width = 0;
    top.height = 0;

    p.layers.push_back(top);  // index 0 = top
    p.layers.push_back(bg);

    p.previewWidth = 64;
    p.previewHeight = 40;
    p.preview.assign(64u * 40u * 4u, 0xFFFF);
    return p;
}

}  // namespace

static void test_round_trip() {
    ProjectFileDoc p = sampleProject();
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    projectEncode error: %s\n", error.c_str());
        return;
    }
    CHECK(bytes->size() > 64);  // header + sections

    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back) {
        std::fprintf(stderr, "    projectDecode error: %s\n", error.c_str());
        return;
    }

    CHECK_EQ(back->width, 320u);
    CHECK_EQ(back->height, 200u);
    CHECK_EQ(back->dpi, 150u);
    CHECK_EQ(back->colorMode, 3u);
    CHECK(back->name == "Round Trip");
    CHECK(back->colorModeName == "RGB/8");
    CHECK(back->background == "white");
    CHECK_EQ(back->layers.size(), 2u);
    if (back->layers.size() != 2) return;

    // Metadata-only top layer preserved without pixels.
    CHECK(back->layers[0].name == "Detail");
    CHECK_EQ(back->layers[0].opacity, 80u);
    CHECK(back->layers[0].blend == "Multiply");
    CHECK_NEAR(back->layers[0].offsetX, 12.25, 1e-9);
    CHECK_NEAR(back->layers[0].offsetY, -3.5, 1e-9);
    CHECK_NEAR(back->layers[0].scaleX, 1.5, 1e-9);
    CHECK_NEAR(back->layers[0].scaleY, 0.75, 1e-9);
    CHECK_EQ(back->layers[0].width, 0u);
    CHECK_EQ(back->layers[0].rgba.size(), 0u);

    // Pixel layer: straight alpha must survive the deflate hop exactly.
    CHECK(back->layers[1].name == "Background");
    CHECK(back->layers[1].locked);
    CHECK_EQ(back->layers[1].width, 320u);
    CHECK_EQ(back->layers[1].height, 200u);
    CHECK_EQ(back->layers[1].rgba.size(), 320u * 200u * 4u);
    const std::size_t c = (1u * 320u + 2u) * 4u;
    CHECK_EQ(back->layers[1].rgba[c + 0], 0x6666u);
    CHECK_EQ(back->layers[1].rgba[c + 1], 0x4CCCu);
    CHECK_EQ(back->layers[1].rgba[c + 2], 0x3333u);
    CHECK_EQ(back->layers[1].rgba[c + 3], 0x8000u);
    // Background pixels that weren't touched should still be opaque white.
    CHECK_EQ(back->layers[1].rgba[0], 65535u);

    // Preview survived.
    CHECK_EQ(back->previewWidth, 64u);
    CHECK_EQ(back->previewHeight, 40u);
    CHECK_EQ(back->preview.size(), 64u * 40u * 4u);
}

static void test_cmyk_payload_round_trip() {
    ProjectFileDoc p;
    p.width = 2;
    p.height = 1;
    p.dpi = 300;
    p.colorMode = 4;
    p.colorModeName = "CMYK/16";
    p.name = "Native inks";
    p.icc = {0x00, 0x43, 0x4d, 0x59, 0xff};

    ProjectLayerFile layer;
    layer.name = "CMYK pixels";
    layer.width = 2;
    layer.height = 1;
    layer.rgba = {0x1111, 0x2222, 0x3333, 0xffff,
                  0xaaaa, 0xbbbb, 0xcccc, 0x8000};
    layer.cmyk = {0x0000, 0x1111, 0x2222, 0xffff,
                  0x3333, 0x4444, 0x5555, 0x6666};
    p.layers.push_back(layer);

    std::string error;
    const auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;
    const auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back) return;
    CHECK_EQ(back->colorMode, 4u);
    CHECK(back->colorModeName == "CMYK/16");
    CHECK(back->icc == p.icc);
    CHECK_EQ(back->layers.size(), 1u);
    if (back->layers.size() != 1) return;
    CHECK(back->layers[0].rgba == layer.rgba);
    CHECK(back->layers[0].cmyk == layer.cmyk);
}

static void test_scan_skips_layers() {
    ProjectFileDoc p = sampleProject();
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;

    auto info = pittore::io::projectScan(*bytes, &error);
    CHECK(info.has_value());
    if (!info) {
        std::fprintf(stderr, "    projectScan error: %s\n", error.c_str());
        return;
    }
    CHECK(info->name == "Round Trip");
    CHECK(info->width == 320);
    CHECK(info->height == 200);
    CHECK(info->previewWidth == 64);
    CHECK(info->previewHeight == 40);
    CHECK_EQ(info->preview.size(), 64u * 40u * 4u);
}

static void test_corrupt_inputs() {
    ProjectFileDoc p = sampleProject();
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;

    // Wrong magic → clean rejection.
    std::vector<std::uint8_t> badMagic = *bytes;
    badMagic[0] = 'X';
    CHECK(!pittore::io::projectDecode(badMagic, &error).has_value());
    CHECK(!error.empty());

    // Truncated payload → clean rejection, no crash.
    std::vector<std::uint8_t> truncated(bytes->begin(), bytes->begin() + 40);
    CHECK(!pittore::io::projectDecode(truncated, &error).has_value());

    // Empty buffer.
    CHECK(!pittore::io::projectDecode({}, &error).has_value());
}

static void test_black_background_round_trip() {
    ProjectFileDoc p = sampleProject();
    p.background = "black";
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (back) CHECK(back->background == "black");
}

static std::uint32_t fileVersion(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 12) return 0;
    return (static_cast<std::uint32_t>(bytes[8]) << 24) |
           (static_cast<std::uint32_t>(bytes[9]) << 16) |
           (static_cast<std::uint32_t>(bytes[10]) << 8) |
           static_cast<std::uint32_t>(bytes[11]);
}

static void test_plain_doc_stays_v1() {
    // No masks/clips/indents/locks: the file stays v1 so old readers work,
    // and v2 fields decode defaulted.
    ProjectFileDoc p = sampleProject();
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;
    CHECK_EQ(fileVersion(*bytes), 1u);
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back || back->layers.size() != 2) return;
    CHECK(!back->layers[0].hasMask);
    CHECK(!back->layers[0].clipped);
    CHECK(!back->layers[0].lockTransparency);
    CHECK_EQ(back->layers[0].indent, 0);
}

static void test_v2_mask_clip_indent_lock() {
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile& top = p.layers[0];
    top.clipped = true;
    top.lockTransparency = true;
    top.indent = 1;
    top.hasMask = true;
    top.maskEnabled = false;  // disabled masks persist disabled
    top.maskOffsetX = 12.25;
    top.maskOffsetY = -3.5;
    top.maskScaleX = 1.0;
    top.maskScaleY = 1.0;
    top.maskWidth = 4;
    top.maskHeight = 2;
    top.mask = {65535, 65535, 0, 0, 32768, 32768, 100, 200};
    top.maskDensity = 0.5;
    top.maskFeather = 2.0;
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v2 encode error: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(fileVersion(*bytes), 2u);
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back || back->layers.size() != 2) return;
    const ProjectLayerFile& l = back->layers[0];
    CHECK(l.clipped);
    CHECK(l.lockTransparency);
    CHECK_EQ(l.indent, 1);
    CHECK(l.hasMask);
    CHECK(!l.maskEnabled);
    CHECK_NEAR(l.maskOffsetX, 12.25, 1e-9);
    CHECK_NEAR(l.maskOffsetY, -3.5, 1e-9);
    CHECK_EQ(l.maskWidth, 4u);
    CHECK_EQ(l.maskHeight, 2u);
    CHECK(l.mask == top.mask);
    CHECK_NEAR(l.maskDensity, 0.5, 1e-9);
    CHECK_NEAR(l.maskFeather, 2.0, 1e-9);
    // Untouched layers stay v1-shaped.
    CHECK(!back->layers[1].hasMask);
    CHECK(!back->layers[1].clipped);
    CHECK_EQ(back->layers[1].indent, 0);
    // Re-encoding the decode is byte-identical.
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);
    // The scan path accepts v2 without decoding layers.
    auto info = pittore::io::projectScan(*bytes, &error);
    CHECK(info.has_value());
    if (info) CHECK(info->name == "Round Trip");
}

static void test_v2_curves_channels() {
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile& top = p.layers[0];
    top.hasAdjustment = true;
    top.adjustmentKind = 3;
    top.adjustmentCurve = {{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}};
    top.adjustmentCurveR = {{0.0, 0.0}, {1.0, 0.5}};
    // G/B stay empty (identity).
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v2 curves encode error: %s\n",
                     error.c_str());
        return;
    }
    CHECK_EQ(fileVersion(*bytes), 2u);
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back || back->layers.empty()) return;
    const ProjectLayerFile& l = back->layers[0];
    CHECK(l.hasAdjustment && l.adjustmentKind == 3);
    CHECK(l.adjustmentCurve == top.adjustmentCurve);
    CHECK(l.adjustmentCurveR == top.adjustmentCurveR);
    CHECK(l.adjustmentCurveG.empty());
    CHECK(l.adjustmentCurveB.empty());
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);
}

static void test_v3_wide_params() {
    // Params beyond the original 8 push the file to v3 and round-trip all
    // 16; v1/v2 files (8 params) still decode with the rest identity 0.
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile& top = p.layers[0];
    top.hasAdjustment = true;
    top.adjustmentKind = 13;  // future kind exercising the wide slots
    top.adjustmentParams[3] = 0.5;
    top.adjustmentParams[10] = 2.5;
    top.adjustmentParams[15] = -1.0;
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v3 encode error: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(fileVersion(*bytes), 3u);
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back || back->layers.empty()) return;
    const ProjectLayerFile& l = back->layers[0];
    CHECK(l.hasAdjustment && l.adjustmentKind == 13);
    CHECK_NEAR(l.adjustmentParams[3], 0.5, 1e-12);
    CHECK_NEAR(l.adjustmentParams[10], 2.5, 1e-12);
    CHECK_NEAR(l.adjustmentParams[15], -1.0, 1e-12);
    CHECK_NEAR(l.adjustmentParams[0], 0.0, 1e-12);
    CHECK_NEAR(l.adjustmentParams[8], 0.0, 1e-12);
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);
    // The scan path accepts v3 without decoding layers.
    auto info = pittore::io::projectScan(*bytes, &error);
    CHECK(info.has_value());
    if (info) CHECK(info->name == "Round Trip");
}

static void test_v4_psd_blocks() {
    // Foreign PSD blocks push the file to v4 and round-trip verbatim
    // (sig, key, odd-size data, stored pad); block-free files stay ≤v3.
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile::PsdBlock tysh;
    std::memcpy(tysh.sig, "8B64", 4);
    std::memcpy(tysh.key, "TySh", 4);
    tysh.data = {1, 2, 3, 4, 5, 6, 7};
    tysh.padding = {0};
    p.layers[1].psdBlocks.push_back(tysh);
    ProjectLayerFile::PsdBlock sold;
    std::memcpy(sold.sig, "8BIM", 4);
    std::memcpy(sold.key, "SoLd", 4);
    sold.data.assign(4096, 0xab);
    p.layers[1].psdBlocks.push_back(sold);
    std::string error;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v4 encode error: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(fileVersion(*bytes), 4u);
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back || back->layers.size() != 2) return;
    CHECK(back->layers[0].psdBlocks.empty());
    CHECK_EQ(back->layers[1].psdBlocks.size(), 2u);
    if (back->layers[1].psdBlocks.size() != 2) return;
    const auto& b0 = back->layers[1].psdBlocks[0];
    CHECK(std::memcmp(b0.sig, "8B64", 4) == 0);
    CHECK(std::memcmp(b0.key, "TySh", 4) == 0);
    CHECK(b0.data == tysh.data);
    CHECK(b0.padding == tysh.padding);
    CHECK(back->layers[1].psdBlocks[1].data.size() == 4096u);
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);
    auto info = pittore::io::projectScan(*bytes, &error);
    CHECK(info.has_value());
    // Truncated inside the block section → clean rejection.
    std::vector<std::uint8_t> cut(bytes->begin(), bytes->end() - 3);
    CHECK(!pittore::io::projectDecode(cut, &error).has_value());
}

int main() {
    test_round_trip();
    test_cmyk_payload_round_trip();
    test_scan_skips_layers();
    test_corrupt_inputs();
    test_black_background_round_trip();
    test_plain_doc_stays_v1();
    test_v2_mask_clip_indent_lock();
    test_v2_curves_channels();
    test_v3_wide_params();
    test_v4_psd_blocks();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
