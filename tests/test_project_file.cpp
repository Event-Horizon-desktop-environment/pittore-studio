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

// Builds a raw §8.2 record `[type:4][length:u32][payload]` for an unknown type,
// so a test can hand one to the codec without the encoder knowing the type.
std::vector<std::uint8_t> rawRecord(const char type[4],
                                    const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> r(type, type + 4);
    const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
    r.push_back(static_cast<std::uint8_t>(n >> 24));
    r.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
    r.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
    r.push_back(static_cast<std::uint8_t>(n & 0xff));
    r.insert(r.end(), payload.begin(), payload.end());
    return r;
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

// Spacing recorded from imported formats (v6) round trips; a live-text layer
// without it stays on the old version so old readers still load the file.
static void test_v6_text_spacing() {
    std::string error;
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile& top = p.layers[0];
    top.hasTextSpec = true;
    top.text = "Hello";
    top.textFamily = "sans-serif";
    top.textSize = 48;
    top.textLineHeight = 1;
    top.textWrapWidth = 200;
    top.textEmLeading = 0.8;
    top.textParagraphGap = 25;
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v6 encode error: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(fileVersion(*bytes), 6u);
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back) {
        std::fprintf(stderr, "    v6 decode error: %s\n", error.c_str());
        return;
    }
    if (back->layers.size() != 2) return;
    CHECK(back->layers[0].hasTextSpec);
    CHECK_NEAR(back->layers[0].textEmLeading, 0.8, 1e-12);
    CHECK_NEAR(back->layers[0].textParagraphGap, 25.0, 1e-12);
    // The pixel layer carries no spec.
    CHECK(!back->layers[1].hasTextSpec);
    // Re-encode is byte-identical and the scan accepts v6.
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);
    auto info = pittore::io::projectScan(*bytes, &error);
    CHECK(info.has_value());
    if (info) CHECK(info->name == "Round Trip");

    // Plain live text with no recorded spacing stays v1.
    ProjectFileDoc plain = sampleProject();
    plain.layers[0].hasTextSpec = true;
    plain.layers[0].text = "Plain";
    auto old = pittore::io::projectEncode(plain, &error);
    CHECK(old.has_value());
    if (!old) return;
    CHECK_EQ(fileVersion(*old), 1u);
    auto plainBack = pittore::io::projectDecode(*old, &error);
    CHECK(plainBack.has_value());
    if (!plainBack) return;
    CHECK_NEAR(plainBack->layers[0].textEmLeading, 0.0, 1e-12);
    CHECK_NEAR(plainBack->layers[0].textParagraphGap, 0.0, 1e-12);
}

static void test_v7_artboards() {
    std::string error;
    ProjectFileDoc p = sampleProject();
    p.icc = {0, 1, 2, 3, 4, 5, 6, 7};  // counter must coexist with ICC
    p.boardIdCounter = 3;
    p.background = "white";

    // Board header row (indent 0) with one child (indent 1), then a second
    // board with a fill. Mirrors the runtime tree: structure rides `indent`.
    ProjectLayerFile board1;
    board1.name = "Board A";
    board1.kind = 1;
    board1.isBoard = true;
    board1.boardId = 1;
    board1.boardX = -100.5;
    board1.boardY = 40.25;
    board1.boardW = 800;
    board1.boardH = 600;
    board1.indent = 0;
    board1.opacity = 100;
    board1.locked = true;
    p.layers.insert(p.layers.begin(), board1);

    ProjectLayerFile child;
    child.name = "Child";
    child.kind = 0;
    child.indent = 1;  // metadata-only row under Board A
    p.layers.insert(p.layers.begin() + 1, child);

    ProjectLayerFile board2;
    board2.name = "Board B";
    board2.kind = 1;
    board2.isBoard = true;
    board2.boardId = 2;
    board2.boardX = 1000;
    board2.boardY = 0;
    board2.boardW = 512;
    board2.boardH = 256;
    board2.boardHasFill = true;
    board2.boardFillColor = 0x80ff8000u;  // ARGB
    board2.boardFillOpacity = 0.375f;     // exact in f32
    board2.indent = 0;
    p.layers.insert(p.layers.begin() + 3, board2);

    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v7 encode error: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(fileVersion(*bytes), 7u);

    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back) {
        std::fprintf(stderr, "    v7 decode error: %s\n", error.c_str());
        return;
    }
    CHECK_EQ(back->layers.size(), 5u);
    if (back->layers.size() != 5) return;

    const ProjectLayerFile& b1 = back->layers[0];
    CHECK(b1.isBoard);
    // On disk the row carries the Artboard kind byte; in memory it decodes to
    // a Group row (INV-011), so the runtime Kind enum is untouched.
    CHECK_EQ(b1.kind, pittore::io::kProjectKindGroup);
    CHECK_EQ(b1.boardId, 1u);
    CHECK_NEAR(b1.boardX, -100.5, 1e-12);
    CHECK_NEAR(b1.boardY, 40.25, 1e-12);
    CHECK_NEAR(b1.boardW, 800.0, 1e-12);
    CHECK_NEAR(b1.boardH, 600.0, 1e-12);
    CHECK(!b1.boardHasFill);
    CHECK(b1.locked);
    CHECK_EQ(b1.indent, 0);

    const ProjectLayerFile& c = back->layers[1];
    CHECK(!c.isBoard);  // children are not boards
    CHECK_EQ(c.indent, 1);

    const ProjectLayerFile& b2 = back->layers[3];
    CHECK(b2.isBoard);
    CHECK_EQ(b2.boardId, 2u);
    CHECK(b2.boardHasFill);
    CHECK_EQ(b2.boardFillColor, 0x80ff8000u);
    CHECK_NEAR(b2.boardFillOpacity, 0.375, 1e-6);
    CHECK_NEAR(b2.boardW, 512.0, 1e-12);

    // Counter and ICC both round-trip (they share the meta section).
    CHECK_EQ(back->boardIdCounter, 3u);
    CHECK(back->icc == p.icc);

    // Re-encode is byte-identical and the scan accepts v7.
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);
    auto info = pittore::io::projectScan(*bytes, &error);
    CHECK(info.has_value());
    if (info) CHECK(info->name == "Round Trip");
}

static void test_v7_id_counter_rederived() {
    // A v7 file whose meta stops short of the counter (or a hand-built one)
    // still yields a counter above every loaded id, so a board created after
    // load never collides (INV-006).
    std::string error;
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile board;
    board.name = "B";
    board.kind = 1;
    board.isBoard = true;
    board.boardId = 9;
    board.boardW = 10;
    board.boardH = 10;
    p.layers.insert(p.layers.begin(), board);
    p.boardIdCounter = 0;  // deliberately stale/absent

    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back || back->layers.empty()) return;
    // Encoder wrote 0, decoder re-derives 9 from the loaded board.
    CHECK_EQ(back->boardIdCounter, 9u);
}

static void test_v7_unknown_records() {
    // §8.2 rules 2/5 + TEST-002/003: a record type we do not model is skipped
    // by length on read, retained verbatim, and re-emitted unchanged on save —
    // byte-identical round-trip, so an ordering or framing bug is visible.
    // The fixture is built in-test with a small binary writer (AGENTS.md: never
    // commit binary fixtures).
    std::string error;
    ProjectFileDoc p = sampleProject();
    ProjectLayerFile board;
    board.name = "B";
    board.kind = 1;
    board.isBoard = true;
    board.boardId = 1;
    board.boardW = 100;
    board.boardH = 50;
    p.layers.insert(p.layers.begin(), board);
    p.boardIdCounter = 1;

    // A future per-board record type ("GUID") we do not model.
    const std::vector<std::uint8_t> unknown = rawRecord("GUID", {9, 8, 7, 6, 5});
    p.unknownRecords = unknown;

    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) {
        std::fprintf(stderr, "    v7 unknown encode error: %s\n", error.c_str());
        return;
    }

    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(back.has_value());
    if (!back) {
        std::fprintf(stderr, "    v7 unknown decode error: %s\n", error.c_str());
        return;
    }
    // Skipped by length (rule 2), retained verbatim in order (rule 5).
    CHECK(back->unknownRecords == unknown);

    // Re-encode is byte-identical (TEST-002): the unknown record is re-emitted
    // unchanged and in place.
    auto again = pittore::io::projectEncode(*back, &error);
    CHECK(again.has_value() && *again == *bytes);

    // A second decode still retains it (TEST-003, rule 5).
    auto back2 = pittore::io::projectDecode(*again, &error);
    CHECK(back2.has_value() && back2->unknownRecords == unknown);

    // Multiple unknown records keep their original order across a round trip.
    p.unknownRecords.clear();
    const auto u1 = rawRecord("AAAA", {1});
    const auto u2 = rawRecord("BBBB", {2, 2});
    p.unknownRecords.insert(p.unknownRecords.end(), u1.begin(), u1.end());
    p.unknownRecords.insert(p.unknownRecords.end(), u2.begin(), u2.end());
    auto bytes2 = pittore::io::projectEncode(p, &error);
    auto back3 = bytes2 ? pittore::io::projectDecode(*bytes2, &error)
                        : std::nullopt;
    CHECK(back3.has_value() && back3->unknownRecords == p.unknownRecords);
}

static void test_future_version_refused_with_message() {
    // A file from a future version is refused, and the message names the
    // version found and the highest this build supports (§8.7), so the
    // refusal is actionable rather than a bare boolean failure.
    std::string error;
    ProjectFileDoc p = sampleProject();
    auto bytes = pittore::io::projectEncode(p, &error);
    CHECK(bytes.has_value());
    if (!bytes) return;
    (*bytes)[11] = 8;  // patch the big-endian version field (bytes 8..11)
    auto back = pittore::io::projectDecode(*bytes, &error);
    CHECK(!back.has_value());
    CHECK(error.find("file version 8") != std::string::npos);
    CHECK(error.find("up to version 7") != std::string::npos);
    // The scan path refuses it too, with the same message.
    std::string scanError;
    auto info = pittore::io::projectScan(*bytes, &scanError);
    CHECK(!info.has_value());
    CHECK(scanError.find("file version 8") != std::string::npos);
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
    test_v6_text_spacing();
    test_v7_artboards();
    test_v7_id_counter_rederived();
    test_v7_unknown_records();
    test_future_version_refused_with_message();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
