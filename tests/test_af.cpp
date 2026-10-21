// Affinity (.af/.afphoto/.afdesign/.afpub) codec: probe, embedded-preview PNG
// decode (RGB/RGBA/gray/palette, 8+16-bit, Adam7), tail JSON metadata, and —
// when built with libzstd (PITTORE_AF) — the flat "#Fil" frame walk used by
// the scanner in af.cpp. The layered decoder (af_layers.cpp) is separate: it
// reads the container's FAT savepoints, parses the "doc.dat" object graph and
// rebuilds placed bitmap layers. It is exercised in testLayeredDecode against
// the external sample-document fixtures (PITTORE_AFDESIGN_DIR).
//
// Flattened-preview fixtures are synthesised in-test: PNGs built with zlib
// deflate inside an .af container assembled from the verified layout
// (header + "#Fil" zstd frames + cleartext previews + JSON tail).
#include "engine/io/af.h"
#include "engine/io/af_layers.h"
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/emit/af_write.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>
#ifdef PITTORE_AF
#include <zstd.h>
#endif

using pittore::io::AfDocument;
using pittore::io::AfImage;
using pittore::io::afDecode;
using pittore::io::afDecodeDocument;
using pittore::io::afProbe;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void put32be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void put32le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
}

std::vector<std::uint8_t> deflateBytes(const std::vector<std::uint8_t>& raw) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    deflateInit(&zs, Z_BEST_COMPRESSION);
    zs.next_in = const_cast<Bytef*>(raw.data());
    zs.avail_in = static_cast<uInt>(raw.size());
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 16384> buf{};
    int ret;
    do {
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        ret = deflate(&zs, Z_FINISH);
        out.insert(out.end(), buf.begin(), buf.begin() + (buf.size() - zs.avail_out));
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return out;
}

std::vector<std::uint8_t> pngChunk(const char type[4],
                                   const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> c;
    put32be(c, static_cast<std::uint32_t>(data.size()));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    const std::uint32_t crc =
        crc32(0, reinterpret_cast<const Bytef*>(c.data() + 4),
              static_cast<uInt>(c.size() - 4));
    put32be(c, crc);
    return c;
}

// Builds a PNG from per-scanline rows (each row = filter byte + pixels).
// `colorType`/`bitDepth` go into the IHDR; `extra` chunks (pHYs, PLTE, tRNS)
// are appended before IDAT.
std::vector<std::uint8_t> buildPng(std::uint32_t w, std::uint32_t h, int colorType,
                                   int bitDepth, int interlace,
                                   const std::vector<std::uint8_t>& scanlines,
                                   const std::vector<std::vector<std::uint8_t>>& extra = {}) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr.push_back(static_cast<std::uint8_t>(bitDepth));
    ihdr.push_back(static_cast<std::uint8_t>(colorType));
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(static_cast<std::uint8_t>(interlace));
    const auto ihdrChunk = pngChunk("IHDR", ihdr);
    out.insert(out.end(), ihdrChunk.begin(), ihdrChunk.end());
    for (const auto& e : extra) out.insert(out.end(), e.begin(), e.end());
    const auto idat = pngChunk("IDAT", deflateBytes(scanlines));
    out.insert(out.end(), idat.begin(), idat.end());
    const auto iend = pngChunk("IEND", {});
    out.insert(out.end(), iend.begin(), iend.end());
    return out;
}

// RGBA8 → colorType 6 (depth 8) with a pHYs of 3780 dpm (≈96 dpi).
std::vector<std::uint8_t> buildRgba8Png(std::uint32_t w, std::uint32_t h,
                                        const std::vector<std::uint8_t>& rgba,
                                        bool phys = true) {
    std::vector<std::uint8_t> rows;
    for (std::uint32_t y = 0; y < h; ++y) {
        rows.push_back(0);
        rows.insert(rows.end(), rgba.begin() + y * w * 4, rgba.begin() + (y + 1) * w * 4);
    }
    std::vector<std::vector<std::uint8_t>> extra;
    if (phys) {
        std::vector<std::uint8_t> ph;
        put32be(ph, 3780);
        put32be(ph, 3780);
        ph.push_back(1);  // metres
        extra.push_back(pngChunk("pHYs", ph));
    }
    return buildPng(w, h, 6, 8, 0, rows, extra);
}

std::vector<std::uint8_t> afHeaderTail(const std::string& jsonTail) {
    // Named spans, not braced insert lists: GCC's -Wstringop-overflow
    // heuristic misfires on vector::insert(initializer_list) for small
    // constant spans. Same bytes in the same order.
    static constexpr unsigned char kMagic[] = {0x00, 0xff, 0x4b, 0x41,
                                               0x0c, 0x00, 0x04, 0x02};
    static constexpr unsigned char kKind[] = {'n', 's', 'r', 'P',
                                              '#', 'I', 'n', 'f'};
    static constexpr unsigned char kMarker[] = {0x3a, 0, 0, 0};
    static constexpr unsigned char kProto[] = {'P', 'r', 'o', 't',
                                               'M', '5', 0, 0};
    std::vector<std::uint8_t> out;
    out.reserve(64 + jsonTail.size());
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    out.insert(out.end(), std::begin(kKind), std::end(kKind));
    out.insert(out.end(), 40, 0);                       // five u64 LE fields
    out.insert(out.end(), std::begin(kMarker), std::end(kMarker));
    out.insert(out.end(), std::begin(kProto), std::end(kProto));
    out.insert(out.end(), jsonTail.begin(), jsonTail.end());
    return out;
}

const std::string kJsonTail =
    R"({"document":{"artboardCount":0,"author":"","clientVersion":"3.3.0.4850",)"
    R"("hasLinkedResources":true,"isBeta":false,"pageCount":1,"tags":[],"title":"d"}})";

// ---------------------------------------------------------------------------
// PNG fixtures
// ---------------------------------------------------------------------------

// 2×2 RGBA: (10,20,30,40) (200,210,220,230) / (60,70,80,90) (100,110,120,130)
std::vector<std::uint8_t> tinyRgba() {
    const std::vector<std::uint8_t> px = {
        10, 20, 30, 40, 200, 210, 220, 230, 60, 70, 80, 90, 100, 110, 120, 130};
    return buildRgba8Png(2, 2, px);
}

// RGB 2×2 (colorType 2): (1,2,3) (4,5,6) / (7,8,9) (10,11,12)
std::vector<std::uint8_t> tinyRgb() {
    std::vector<std::uint8_t> rows;
    const std::uint8_t px[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    for (std::uint32_t y = 0; y < 2; ++y) {
        rows.push_back(0);
        for (std::uint32_t x = 0; x < 2; ++x) {
            const std::uint8_t* p = px + (y * 2 + x) * 3;
            rows.insert(rows.end(), p, p + 3);
        }
    }
    return buildPng(2, 2, 2, 8, 0, rows);
}

// RGB 3×2 (colorType 2): larger area than the 2×2 fixtures (6 > 4), used to
// prove the primary preview is the largest, not the last.
std::vector<std::uint8_t> tinyBig() {
    std::vector<std::uint8_t> rows;
    const std::uint8_t px[18] = {1, 2, 3, 4, 5, 6, 7, 8, 9,
                                 10, 11, 12, 13, 14, 15, 16, 17, 18};
    for (std::uint32_t y = 0; y < 2; ++y) {
        rows.push_back(0);
        for (std::uint32_t x = 0; x < 3; ++x) {
            const std::uint8_t* p = px + (y * 3 + x) * 3;
            rows.insert(rows.end(), p, p + 3);
        }
    }
    return buildPng(3, 2, 2, 8, 0, rows);
}

// Gray 2×2 (colorType 0, depth 8): 0, 85, 170, 255
std::vector<std::uint8_t> tinyGray() {
    std::vector<std::uint8_t> rows;
    const std::uint8_t px[4] = {0, 85, 170, 255};
    for (std::uint32_t y = 0; y < 2; ++y) {
        rows.push_back(0);
        rows.insert(rows.end(), px + y * 2, px + y * 2 + 2);
    }
    return buildPng(2, 2, 0, 8, 0, rows);
}

// Palette 2×2 (colorType 3): indices {1,0,2,1}, palette red/green/blue,
// tRNS alpha {0,128,255}.
std::vector<std::uint8_t> tinyPalette() {
    std::vector<std::uint8_t> plte;
    plte.insert(plte.end(), {255, 0, 0, 0, 255, 0, 0, 0, 255});
    std::vector<std::uint8_t> trns = {0, 128, 255};
    std::vector<std::uint8_t> rows;
    const std::uint8_t idx[4] = {1, 0, 2, 1};
    for (std::uint32_t y = 0; y < 2; ++y) {
        rows.push_back(0);
        rows.insert(rows.end(), idx + y * 2, idx + y * 2 + 2);
    }
    return buildPng(2, 2, 3, 8, 0, rows,
                    {pngChunk("PLTE", plte), pngChunk("tRNS", trns)});
}

// 16-bit RGBA 2×2 (colorType 6, depth 16): value v stored as v*(0x101/1).
std::vector<std::uint8_t> tinyRgba16() {
    std::vector<std::uint8_t> rows;
    const std::uint16_t px[16] = {0x0101, 0x0202, 0x0303, 0x0404, 0xfefe, 0xfdfd, 0xfcfc,
                                  0xfbfb, 0x1111, 0x2222, 0x3333, 0x4444, 0xaaaa, 0xbbbb,
                                  0xcccc, 0xdddd};
    for (std::uint32_t y = 0; y < 2; ++y) {
        rows.push_back(0);
        for (std::uint32_t x = 0; x < 2; ++x) {
            const std::uint16_t* p = px + (y * 2 + x) * 4;
            for (int c = 0; c < 4; ++c) {
                rows.push_back(static_cast<std::uint8_t>(p[c] >> 8));
                rows.push_back(static_cast<std::uint8_t>(p[c] & 0xff));
            }
        }
    }
    return buildPng(2, 2, 6, 16, 0, rows);
}

// 8×8 RGBA gradient; returns the pixel values too.
std::vector<std::uint8_t> pat8x8(std::vector<std::uint8_t>& rgba) {
    const std::uint32_t w = 8, h = 8;
    rgba.resize(w * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            rgba[(y * w + x) * 4 + 0] = static_cast<std::uint8_t>(x * 31);
            rgba[(y * w + x) * 4 + 1] = static_cast<std::uint8_t>(y * 31);
            rgba[(y * w + x) * 4 + 2] = static_cast<std::uint8_t>((x + y) * 15);
            rgba[(y * w + x) * 4 + 3] = 255;
        }
    std::vector<std::uint8_t> rows;
    for (std::uint32_t y = 0; y < h; ++y) {
        rows.push_back(0);
        rows.insert(rows.end(), rgba.begin() + y * w * 4, rgba.begin() + (y + 1) * w * 4);
    }
    return buildRgba8Png(w, h, rgba, false);
}

// Adam7-interlaced build of the same 8×8 pattern.
std::vector<std::uint8_t> pat8x8Interlaced(const std::vector<std::uint8_t>& rgba) {
    static const std::uint32_t ax[7] = {0, 4, 0, 2, 0, 1, 0};
    static const std::uint32_t ay[7] = {0, 0, 4, 0, 2, 0, 1};
    static const std::uint32_t adx[7] = {8, 8, 4, 4, 2, 2, 1};
    static const std::uint32_t ady[7] = {8, 8, 8, 4, 4, 2, 2};
    std::vector<std::uint8_t> rows;
    for (int p = 0; p < 7; ++p) {
        const std::uint32_t pw = (8 - ax[p] + adx[p] - 1) / adx[p];
        const std::uint32_t ph = (8 - ay[p] + ady[p] - 1) / ady[p];
        for (std::uint32_t py = 0; py < ph; ++py) {
            rows.push_back(0);
            for (std::uint32_t px = 0; px < pw; ++px) {
                const std::uint32_t x = ax[p] + px * adx[p];
                const std::uint32_t y = ay[p] + py * ady[p];
                rows.insert(rows.end(), rgba.begin() + (y * 8 + x) * 4,
                            rgba.begin() + (y * 8 + x) * 4 + 4);
            }
        }
    }
    return buildPng(8, 8, 6, 8, 1, rows);
}

// Invalid PNG: same layout but with a corrupted CRC in the IDAT chunk.
std::vector<std::uint8_t> corruptPng() {
    auto png = tinyRgba();
    // The IDAT chunk starts at offset 8 (IHDR: 8+8+13+4 = 33) → 33.
    const std::size_t idatAt = 33;
    png[idatAt + 8 + 4 + 2] ^= 0xff;  // flip a byte inside the IDAT payload
    const std::size_t clen =
        (std::uint32_t(png[idatAt]) << 24) | (std::uint32_t(png[idatAt + 1]) << 16) |
        (std::uint32_t(png[idatAt + 2]) << 8) | png[idatAt + 3];
    // recompute CRC over type+data
    const std::uint32_t crc = crc32(0, png.data() + idatAt + 4,
                                    static_cast<uInt>(8 + clen));
    const std::size_t crcAt = idatAt + 8 + clen;
    png[crcAt] = static_cast<std::uint8_t>(crc >> 24);
    png[crcAt + 1] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
    png[crcAt + 2] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
    png[crcAt + 3] = static_cast<std::uint8_t>(crc & 0xff);
    return png;  // deflate stream is now garbage → inflate fails
}

// ---------------------------------------------------------------------------
// .af container + zstd metadata fixtures
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> recordBlock() {
    // Mirrors the verified record grammar: [tag u8][name 4][payload].
    std::vector<std::uint8_t> v;
    const auto u32rec = [&](std::uint8_t tag, const char name[4], std::uint32_t val) {
        v.push_back(tag);
        v.insert(v.end(), name, name + 4);
        put32le(v, val);
    };
    const auto strRec = [&](std::uint8_t tag, const char name[4], const std::string& s) {
        v.push_back(tag);
        v.insert(v.end(), name, name + 4);
        put32le(v, static_cast<std::uint32_t>(s.size()));
        v.insert(v.end(), s.begin(), s.end());
    };
    u32rec(0x07, "nveR", 4850);
    u32rec(0x07, "rjaM", 3);
    u32rec(0x07, "rniM", 3);
    u32rec(0x07, "dliB", 0);
    strRec(0x2b, "NFRI", "Z:\\Assets\\photos\\aurora.jpg");
    u32rec(0x07, "WpmB", 3840);
    u32rec(0x07, "HpmB", 2160);
    u32rec(0x07, "IPDO", 96);
    // Internal block reference — must be ignored by the placement scanner.
    strRec(0x33, "ataD", "d/7f32");
    return v;
}

// --- layered-decode fixtures (af_layers) ---------------------------------

// The layered decoder is validated against real sample documents. They
// arrive as a container + FAT savepoints + a compressed "doc.dat" object
// graph + tile payloads, which cannot be synthesised here without
// reimplementing the encoder. Sample documents are external fixtures fetched
// separately (PITTORE_AFDESIGN_DIR); a checkout without them skips the
// layered test instead of failing. No fixture path is baked into the source.
std::string affinityFixture(const char* name) {
    const char* env = std::getenv("PITTORE_AFDESIGN_DIR");
    if (!env || !*env) return {};
    std::string dir = env;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    std::FILE* f = std::fopen((dir + name).c_str(), "rb");
    if (!f) return {};
    std::fclose(f);
    return dir + name;
}

bool readWholeFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::rewind(f);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<std::size_t>(n));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}


// The object-graph decoder is exercised against real sample documents when
// the external fixture directory is present (PITTORE_AFDESIGN_DIR). They
// cover the container/FAT savepoint walk, the zlib + zstd payload variants,
// the bitmap tile statuses, group nesting, and (through d.af) the layer-kind
// skip paths. Skipped when the fixtures are absent so a source-only checkout
// still runs.
void testLayeredDecode() {
#ifdef PITTORE_AF
    const std::string raster = affinityFixture("raster_test.afdesign");
    if (raster.empty()) {
        std::printf("test_af: sample-document fixtures absent — layered test skipped\n");
        check(true, "layered decode skipped (no fixtures)");
        return;
    }

    // raster_test: a group holding one 180×180 RGBA bitmap whose alpha is a
    // filled disc. The decoded pixels are pinned by FNV-1a so a regression in
    // the plane assembly or the tile status path is caught immediately.
    {
        std::vector<std::uint8_t> bytes;
        check(readWholeFile(raster, bytes), "read raster_test.afdesign");
        std::string err;
        auto doc = pittore::io::afDecodeLayers(bytes, &err);
        check(doc.has_value(), "afDecodeLayers parses raster_test.afdesign");
        if (doc) {
            check(doc->width == 180 && doc->height == 180,
                  "raster_test canvas from SprB");
            check(doc->layers.size() == 2, "raster_test: one group + one bitmap");
            check(doc->frameCount == 1, "raster_test decoded one bitmap frame");
            check(doc->complete, "raster_test has no skipped layers");
            check(doc->skippedLayers == 0, "raster_test skip count is zero");
            const pittore::io::AfLayer* pixel = nullptr;
            for (const auto& l : doc->layers)
                if (!l.isGroup && !l.rgba.empty()) pixel = &l;
            check(pixel != nullptr, "raster_test 'Pixel' raster recovered");
            if (pixel) {
                check(pixel->name == "Pixel", "raster_test layer name");
                check(pixel->width == 180 && pixel->height == 180, "Pixel dims");
                check(pixel->left == 0 && pixel->top == 0, "Pixel placement");
                check(pixel->rgba.size() == 180u * 180 * 4,
                      "Pixel RGBA8 expanded to 16-bit");
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (std::uint8_t b : pixel->rgba) {
                    h ^= b;
                    h *= 0x100000001b3ull;
                }
                check(h == 0x711adfba89c62931ull, "Pixel raster hash");
            }
        }
    }

    // layer_test: fifty nested groups with no leaf content at all. The walk
    // must still report the canvas and the tree and produce no frames; the UI
    // imports the group tree itself and drops the flattened preview.
    {
        const std::string path = affinityFixture("layer_test.afdesign");
        if (!path.empty()) {
            std::vector<std::uint8_t> bytes;
            check(readWholeFile(path, bytes), "read layer_test.afdesign");
            std::string err;
            auto doc = pittore::io::afDecodeLayers(bytes, &err);
            check(doc.has_value(), "afDecodeLayers parses layer_test.afdesign");
            if (doc) {
                check(doc->width == 180 && doc->height == 180, "layer_test canvas");
                check(doc->layers.size() == 50, "layer_test group tree recovered");
                check(doc->frameCount == 0, "layer_test has no bitmap frames");
                check(doc->complete && doc->skippedLayers == 0,
                      "layer_test has no leaf nodes to skip");
                bool allGroups = true, anyPixels = false;
                for (const auto& l : doc->layers) {
                    if (!l.isGroup) allGroups = false;
                    if (!l.rgba.empty()) anyPixels = true;
                }
                check(allGroups, "layer_test emits only group layers");
                check(!anyPixels, "layer_test layers carry no raster pixels");
            }
        }
    }
#else
    check(true, "layered decode skipped (no libzstd)");
#endif
}

std::vector<std::uint8_t> metadataTree(std::size_t minBytes) {
    const auto block = recordBlock();
    std::vector<std::uint8_t> tree;
    while (tree.size() < minBytes) tree.insert(tree.end(), block.begin(), block.end());
    return tree;
}

std::vector<std::uint8_t> buildAf(
    const std::vector<std::vector<std::uint8_t>>& pngs, const std::string& jsonTail,
    const std::vector<std::vector<std::uint8_t>>* frames = nullptr) {
    std::vector<std::uint8_t> out = afHeaderTail(jsonTail);
    if (frames) {
        for (const auto& raw : *frames) {
            out.insert(out.end(), {'#', 'F', 'i', 'l'});
#ifdef PITTORE_AF
            const std::size_t bound = ZSTD_compressBound(raw.size());
            std::vector<std::uint8_t> comp(bound);
            const std::size_t n =
                ZSTD_compress(comp.data(), comp.size(), raw.data(), raw.size(), 3);
            comp.resize(n);
            out.insert(out.end(), comp.begin(), comp.end());
#endif
            out.insert(out.end(), {0xff, 0xff, 0xff, 0xff});
        }
    }
    for (const auto& p : pngs) out.insert(out.end(), p.begin(), p.end());
    out.insert(out.end(), jsonTail.begin(), jsonTail.end());
    return out;
}

void testProbe() {
    const auto good = afHeaderTail(kJsonTail);
    check(afProbe(good), "afProbe on real header");
    check(!afProbe(std::vector<std::uint8_t>(16, 0x00)), "afProbe rejects zeros");
    std::vector<std::uint8_t> junk = {'8', 'B', 'P', 'S'};
    check(!afProbe(junk), "afProbe rejects PSD magic");
    std::vector<std::uint8_t> cut(good.begin(), good.begin() + 12);
    check(!afProbe(cut), "afProbe rejects truncated header");
}

void testPngDecodes() {
    // Primary = highest-fidelity preview: largest pixel area, not the last.
    {
        auto doc = afDecodeDocument(buildAf({tinyRgba(), tinyRgb()}, kJsonTail));
        check(doc.has_value(), "afDecodeDocument parses previews");
        if (doc) {
            check(doc->previews.size() == 2, "two previews found");
            check(doc->primary == 0, "equal-area tie → first preview is primary");
            const pittore::io::AfPreview& p = doc->previews[0];
            check(p.width == 2 && p.height == 2, "RGBA preview dimensions");
            check(p.rgba[0] == 10 * 257 && p.rgba[1] == 20 * 257 && p.rgba[2] == 30 * 257 &&
                      p.rgba[3] == 40 * 257,
                  "RGBA preview samples (×257)");
            check(p.rgba[4 * 3 + 0] == 100 * 257 && p.rgba[4 * 3 + 3] == 130 * 257,
                  "RGBA preview last pixel");
            check(p.dpi == 96, "pHYs dpi recovered (3780 dpm)");
        }
    }
    // A larger preview wins regardless of its position in the archive tail.
    {
        auto doc = afDecodeDocument(buildAf({tinyRgb(), tinyBig()}, kJsonTail));
        check(doc.has_value(), "largest-preview decode (big last)");
        if (doc) {
            check(doc->previews.size() == 2, "two previews found");
            check(doc->primary == 1, "3×2 preview beats the trailing 2×2");
            check(doc->previews[1].width == 3 && doc->previews[1].height == 2,
                  "largest preview dimensions");
        }
        auto doc2 = afDecodeDocument(buildAf({tinyBig(), tinyRgb()}, kJsonTail));
        check(doc2.has_value(), "largest-preview decode (big first)");
        if (doc2) check(doc2->primary == 0, "largest preview wins when first too");
    }
    // afDecode returns the primary (largest-area) preview.
    {
        auto img = afDecode(buildAf({tinyRgba(), tinyRgb()}, kJsonTail));
        check(img.has_value(), "afDecode succeeds");
        if (img) {
            check(img->width == 2 && img->height == 2, "afDecode picks largest preview dims");
            check(img->rgba[0] == 10 * 257 && img->rgba[5] == 210 * 257 &&
                      img->rgba[7] == 230 * 257,
                  "afDecode primary-preview pixels (first on tie)");
            check(img->dpi == 96, "RGBA preview pHYs dpi carried through");
        }
    }
    {
        auto img = afDecode(buildAf({tinyGray()}, kJsonTail));
        check(img.has_value(), "gray decode");
        if (img)
            check(img->rgba[0] == 0 && img->rgba[4 * 1] == 85 * 257 &&
                      img->rgba[4 * 2] == 170 * 257 && img->rgba[4 * 3] == 0xffff,
                  "gray → opaque RGBA16");
    }
    {
        auto img = afDecode(buildAf({tinyPalette()}, kJsonTail));
        check(img.has_value(), "palette decode");
        if (img) {
            // index1 = green with alpha 128; index0 = red alpha 0; index2 blue a255
            check(img->rgba[0] == 0x0000 && img->rgba[1] == 255 * 257 && img->rgba[2] == 0 &&
                      img->rgba[3] == 128 * 257,
                  "palette green + tRNS alpha");
            check(img->rgba[4 * 1 + 0] == 255 * 257 && img->rgba[4 * 1 + 3] == 0,
                  "palette red + transparent");
            check(img->rgba[4 * 3 + 1] == 255 * 257 && img->rgba[4 * 3 + 3] == 128 * 257,
                  "palette green pixel 3");
        }
    }
    {
        auto img = afDecode(buildAf({tinyRgba16()}, kJsonTail));
        check(img.has_value(), "16-bit decode");
        if (img)
            check(img->rgba[0] == 0x0101 && img->rgba[5] == 0xfdfd &&
                      img->rgba[14] == 0xcccc && img->rgba[15] == 0xdddd,
                  "16-bit samples pass through");
    }
    // Corrupt previews are skipped; a valid one still decodes.
    {
        auto img = afDecode(buildAf({corruptPng(), tinyRgb()}, kJsonTail));
        check(img.has_value(), "corrupt preview skipped, valid one decoded");
        if (img) check(img->width == 2 && img->rgba[0] == 1 * 257, "corrupt skip pixel");
    }
}

void testInterlace() {
    std::vector<std::uint8_t> rgba;
    const auto plain = pat8x8(rgba);
    const auto ilaced = pat8x8Interlaced(rgba);
    auto a = afDecode(buildAf({plain}, kJsonTail));
    auto b = afDecode(buildAf({ilaced}, kJsonTail));
    check(a.has_value() && b.has_value(), "interlaced decode succeeds");
    if (a && b)
        check(a->rgba == b->rgba, "Adam7 output identical to non-interlaced");
}

void testTailJson() {
    auto doc = afDecodeDocument(buildAf({tinyRgba()}, kJsonTail));
    check(doc.has_value(), "tail json decode");
    if (doc) {
        check(doc->title == "d", "title from tail JSON");
        check(doc->clientVersion == "3.3.0.4850", "clientVersion from tail JSON");
        check(doc->pageCount == 1, "pageCount from tail JSON");
    }
}

void testMetadataScan() {
#ifdef PITTORE_AF
    std::vector<std::vector<std::uint8_t>> frames = {metadataTree(210 * 1024),
                                                     std::vector<std::uint8_t>(65536, 0)};
    // falsify nothing: the 65536-byte "tile" frame must not be scanned as meta.
    auto doc = afDecodeDocument(buildAf({tinyRgba()}, kJsonTail, &frames));
    check(doc.has_value(), "metadata scan decode");
    if (doc) {
        check(doc->frameCount == 2, "frame walk counts both zstd frames");
        check(doc->revision == 4850, "revision atom read");
        check(doc->appVersion == "3.3.0.4850", "appVersion assembled");
        check(!doc->placements.empty(), "placements found");
        if (!doc->placements.empty()) {
            const auto& pl = doc->placements[0];
            check(pl.width == 3840 && pl.height == 2160, "placement dims");
            check(pl.path.find("aurora.jpg") != std::string::npos,
                  "placement path");
        }
        bool anyDpi = false;
        for (const auto& pl : doc->placements)
            if (pl.dpi == 96) anyDpi = true;
        check(anyDpi, "placement dpi (IPDO) attached");
        // Internal block references ("d/7f32") must not surface as placements.
        for (const auto& pl : doc->placements)
            check(pl.path.find("d/7f32") == std::string::npos,
                  "block references excluded");
    }
    // The cleartext tail must terminate the walk (non-zstd "#Fil" payload).
    {
        std::vector<std::vector<std::uint8_t>> f2 = {metadataTree(210 * 1024)};
        std::vector<std::uint8_t> buf = buildAf({tinyRgba()}, kJsonTail, &f2);
        buf.insert(buf.end(), {'#', 'F', 'i', 'l', 0x00, 0xff, 0x4b, 0x53});
        auto doc2 = afDecodeDocument(buf, nullptr);
        check(doc2.has_value() && doc2->frameCount == 1,
              "walk stops at the cleartext tail");
    }
#else
    check(true, "metadata scan skipped (no libzstd)");
#endif
}

void testRealFile() {
    const char* env = std::getenv("PITTORE_AF_FIXTURE");
    const std::string path = (env && *env) ? env : "";
    std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("test_af: real-file test skipped (PITTORE_AF_FIXTURE %s)\n",
                    path.empty() ? "unset" : "absent");
        check(true, "real-file fixture absent — skipped");
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::rewind(f);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(n));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        check(false, "real-file fixture read");
        return;
    }
    std::fclose(f);

    auto doc = afDecodeDocument(data, nullptr);
    check(doc.has_value(), "real d.af decodes");
    if (doc) {
        check(doc->previews.size() == 2, "d.af has two embedded previews");
        check(doc->title == "d" && doc->clientVersion == "3.3.0.4850" &&
                  doc->pageCount == 1,
              "d.af tail JSON metadata");
        if (!doc->previews.empty()) {
            const auto& p = doc->previews[doc->primary];
            check(p.width == 2559 && p.height == 1439,
                  "d.af primary preview is the 2559×1439 render (largest)");
        }
#ifdef PITTORE_AF
        check(doc->frameCount == 3285, "d.af zstd frame walk count");
        check(doc->revision == 4646, "d.af revision atom");
        check(doc->appVersion == "3.2.3.4646", "d.af appVersion (atoms + revision)");
        check(doc->placements.size() >= 2, "d.af linked placements found");
        bool hasWide = false;
        for (const auto& pl : doc->placements)
            if (pl.width == 3840 && pl.height == 2160) hasWide = true;
        check(hasWide, "d.af 3840×2160 screenshot placement dims");
#endif
    }

    // Layered decode: the container object graph is walked and every placed
    // bitmap layer is rebuilt. d.af holds ten embedded raster layers plus one
    // empty group in a 3840×2160 page. Its "linked" placed images keep only an
    // external path (a Windows path for files written by the Windows app); when
    // that file is present (Wine's Z: is /) it decodes too, so the counts below
    // are lower bounds. Text and shape layers are re-built in place; the
    // remaining adjustment kinds are skipped and logged. The flattened
    // preview is not imported as a panel row either way, so skipped pixels
    // exist only in the log.
    auto layers = pittore::io::afDecodeLayers(data, nullptr);
    check(layers.has_value(), "d.af layered decode");
    if (layers) {
#ifdef PITTORE_AF
        check(layers->frameCount >= 10, "d.af decoded at least ten bitmap frames");
#endif
        check(layers->width == 3840 && layers->height == 2160,
              "d.af canvas is the 3840×2160 page (not a layer extent)");
        check(layers->layers.size() >= 11, "d.af one group + at least ten raster layers");
        check(!layers->complete, "d.af reports skipped linked/vector layers");
        check(layers->skippedLayers > 0, "d.af skip count is non-zero");
        int rasters = 0, groups = 0, offCanvas = 0;
        bool overCap = false, loggedLinked = false, webpLayer = false;
        for (const auto& l : layers->layers) {
            if (l.isGroup) {
                ++groups;
                continue;
            }
            if (l.rgba.empty()) continue;
            ++rasters;
            if (l.left < 0 || l.top < 0) ++offCanvas;
            if (std::uint64_t(l.width) * l.height > (1ull << 28)) overCap = true;
            if (l.name.size() >= 5 && l.name.compare(l.name.size() - 5, 5, ".webp") == 0)
                webpLayer = true;
        }
        check(rasters >= 10, "d.af at least ten raster layers carry pixels");
        check(groups == 1, "d.af empty group layer recovered");
        // Text and shape layers are re-built rather than skipped; only linked
        // sources and the remaining adjustment kinds are reported.
        bool vectorSkipped = false;
        for (const std::string& l : layers->log)
            if (l.rfind("skip", 0) == 0 &&
                (l.find("text") != std::string::npos ||
                 l.find("path has no geometry") != std::string::npos ||
                 l.find("vector layer") != std::string::npos))
                vectorSkipped = true;
        check(!vectorSkipped, "d.af text and shape layers are rebuilt");
        check(!overCap, "d.af no bitmap over the decoder pixel cap");
        // Placement is canvas-relative and signed: the wallpaper layers are
        // placed at negative origins and must keep them.
        check(offCanvas >= 3, "d.af negative layer origins preserved");
        for (const std::string& l : layers->log)
            if (l.find("source entry") != std::string::npos) loggedLinked = true;
        check(loggedLinked, "d.af linked sources are logged as skipped");
        // When the linked WebP beside the document is present, the link resolver
        // must have decoded it into a real layer. This is the whole point of
        // resolving "Link" source entries rather than skipping them.
        const std::size_t slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
        const std::string linked = dir + "/1.webp";
        std::FILE* webp = std::fopen(linked.c_str(), "rb");
        if (webp) {
            std::fclose(webp);
            check(webpLayer, "d.af linked WebP decodes into a layer");
        }
    }

    auto img = afDecode(data, nullptr);
    check(img.has_value(), "d.af flattened preview decode");
    if (img)
        check(img->width == 2559 && img->height == 1439,
              "d.af flattened dims use the largest preview 2559×1439");
}

// ---------------------------------------------------------------------------
// v12 FT4 grammar fixtures
// ---------------------------------------------------------------------------
//
// Real containers are external fixtures; the reader's v12 branch (row
// directories, delta/checkpoint element streams, damaged-block fallback) is
// pinned here on synthesised bytes. Storage is comp=0 (stored) throughout so
// the fixtures need only zlib. af_container_probe walks the same bytes from
// the CLI against the research corpus.

void put16le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}

void put64le(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

void patch64le(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
}

void patch32le(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
}

std::uint32_t crc32Of(const std::vector<std::uint8_t>& b) {
    return static_cast<std::uint32_t>(crc32(0, b.data(), static_cast<uInt>(b.size())));
}

// 72-byte tagged header: magic, version 12, Prsn, #Inf (five u64), revision,
// num, Prot. The #Inf fields are patched by the fixture that owns the file.
std::vector<std::uint8_t> afContainerHeaderPrelude() {
    std::vector<std::uint8_t> out = {0x00, 0xff, 0x4b, 0x41, 0x0c, 0x00, 0x00, 0x00};
    out.insert(out.end(), {'P', 'r', 's', 'n'});
    out.insert(out.end(), {'#', 'I', 'n', 'f'});
    out.insert(out.end(), 40, 0);  // fat, thumb, length, unknown, date
    put32le(out, 1);               // revision
    put32le(out, 3);               // num
    out.insert(out.end(), {'P', 'r', 'o', 't'});
    put32le(out, 12);              // protocol revision
    return out;
}

std::size_t addRecord(std::vector<std::uint8_t>& out,
                      const std::vector<std::uint8_t>& payload, bool sentinel) {
    if (sentinel) out.insert(out.end(), 4, 0xff);
    const std::size_t tag = out.size();
    out.insert(out.end(), {'#', 'F', 'i', 'l'});
    out.insert(out.end(), payload.begin(), payload.end());
    return tag;
}

// One v12 row: [us][cs][crc][comp|kind|u16][payload kind][stored crc]
// [u16 name len + name][13-byte tail unless last].
void appendRow(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& payload,
               const char* name, std::uint64_t nextTag, std::uint32_t id, bool last) {
    put64le(out, payload.size());        // uncompressed
    put64le(out, payload.size());        // compressed (stored)
    put32le(out, crc32Of(payload));      // crc of the final bytes
    put32le(out, 0);                     // compression 0 (stored) | kind | u16
    out.push_back(0x21);                 // payload kind: named record
    put32le(out, crc32Of(payload));      // crc of the stored bytes
    const std::size_t nl = std::strlen(name);
    put16le(out, static_cast<std::uint16_t>(nl));
    out.insert(out.end(), name, name + nl);
    if (!last) {
        put32le(out, id);                // row id
        out.push_back(0);
        put64le(out, nextTag);           // next row's record tag
    }
}

// [u32 prefixLen][u64 counter][prefix bytes] — one family summary.
void appendFooter(std::vector<std::uint8_t>& out, const char* prefix,
                  std::uint64_t count) {
    const std::size_t nl = std::strlen(prefix);
    put32le(out, static_cast<std::uint32_t>(nl));
    put64le(out, count);
    out.insert(out.end(), prefix, prefix + nl);
}

// A #FT4 row directory: doc.dat + d/1, stored, one "d/" trailer. This is the
// grammar every fresh export uses.
std::vector<std::uint8_t> synthRowDirectory() {
    std::vector<std::uint8_t> out = afContainerHeaderPrelude();
    const std::vector<std::uint8_t> docPayload = {'d', 'o', 'c', '!', 1, 2, 3};
    const std::vector<std::uint8_t> tilePayload = {'t', 'i', 'l', 'e', 9, 9};
    const std::size_t docTag = addRecord(out, docPayload, false);
    const std::size_t tileTag = addRecord(out, tilePayload, true);

    out.insert(out.end(), 4, 0xff);
    const std::size_t fatOff = out.size();
    out.insert(out.end(), {'#', 'F', 'T', '4'});
    put64le(out, 0);             // next
    put64le(out, 1000);          // savepoint
    const std::size_t endAt = out.size();
    put64le(out, 0);             // block end, patched
    put64le(out, 0);             // document extent
    put64le(out, 0);             // unknown
    put64le(out, 2);             // row count
    const std::size_t dirSizeAt = out.size();
    put32le(out, 0);             // directory size, patched
    put32le(out, 0x01020001);    // f56: compact row directory
    put32le(out, 0x00000000);    // f60
    put32le(out, static_cast<std::uint32_t>(docTag));  // first tag
    put32le(out, 0);             // b68: row directory (no delta)
    appendRow(out, docPayload, "doc.dat", tileTag, 40, false);
    appendRow(out, tilePayload, "d/1", 0, 0, true);
    appendFooter(out, "d/", 2);
    const std::size_t end = out.size();
    patch64le(out, endAt, end);
    patch32le(out, dirSizeAt, static_cast<std::uint32_t>(end - fatOff - 59));
    patch64le(out, 16, fatOff);  // o1
    patch64le(out, 24, end);     // o2
    return out;
}

// A delta/checkpoint block: one kind-2 token then the serial-1 document
// struct (42 B), closed by the same family trailer.
std::vector<std::uint8_t> synthDeltaBlock() {
    std::vector<std::uint8_t> out = afContainerHeaderPrelude();
    const std::vector<std::uint8_t> payload = {'d', 'e', 'l', 't', 'a'};
    const std::size_t tag = addRecord(out, payload, false);

    out.insert(out.end(), 4, 0xff);
    const std::size_t fatOff = out.size();
    out.insert(out.end(), {'#', 'F', 'T', '4'});
    put64le(out, 0);             // next
    put64le(out, 2000);          // savepoint
    const std::size_t endAt = out.size();
    put64le(out, 0);             // block end, patched
    put64le(out, 0);             // document extent
    put64le(out, 0);             // unknown
    put64le(out, 0);             // row count (unused by the delta path)
    const std::size_t dirSizeAt = out.size();
    put32le(out, 0);             // directory size, patched
    put32le(out, 0xe7000001);    // f56: tile-delta class
    put32le(out, 0x02000004);    // f60
    // Delta blocks are 64 B of header: the element stream starts at +64, so
    // the bytes a row directory spends on firstTag/f68 are payload here and
    // the +68 byte doubles as the discriminator (first element kind).
    put32le(out, 235);           // element: kind-2 token, serial 235
    out.push_back(2);
    put32le(out, 1);             // element: kind-1 struct, serial 1 = the doc
    out.push_back(1);
    put64le(out, tag);           // record tag position (payload at +4)
    put64le(out, payload.size());
    put64le(out, payload.size());
    put32le(out, crc32Of(payload));  // crc of final bytes
    put32le(out, 0);                 // mid: compression low byte 0 (stored)
    out.push_back(0);                // payload type
    put32le(out, crc32Of(payload));  // crc of stored bytes
    appendFooter(out, "d/", 1);
    const std::size_t end = out.size();
    patch64le(out, endAt, end);
    patch32le(out, dirSizeAt, static_cast<std::uint32_t>(end - fatOff - 59));
    patch64le(out, 16, fatOff);  // o1
    patch64le(out, 24, end);     // o2
    return out;
}

// Two-block chain: a damaged newest block (row count 5, 16 body bytes) whose
// next pointer reaches a healthy older row directory. The reader must drop
// the damaged block whole and resolve the document from the older one.
std::vector<std::uint8_t> synthDamagedChain() {
    std::vector<std::uint8_t> out = afContainerHeaderPrelude();
    const std::vector<std::uint8_t> payload = {'o', 'l', 'd', 'd', 'o', 'c'};
    const std::size_t tag = addRecord(out, payload, false);

    out.insert(out.end(), 4, 0xff);
    const std::size_t oldFat = out.size();
    out.insert(out.end(), {'#', 'F', 'T', '4'});
    put64le(out, 0);             // next: end of chain
    put64le(out, 1000);          // savepoint
    const std::size_t oldEndAt = out.size();
    put64le(out, 0);             // block end, patched
    put64le(out, 0);
    put64le(out, 0);
    put64le(out, 1);             // row count
    const std::size_t oldDirSizeAt = out.size();
    put32le(out, 0);             // directory size, patched
    put32le(out, 0x01000001);
    put32le(out, 0x01000000);
    put32le(out, static_cast<std::uint32_t>(tag));
    put32le(out, 0);
    appendRow(out, payload, "doc.dat", 0, 0, true);
    appendFooter(out, "d/", 1);
    const std::size_t oldEnd = out.size();
    patch64le(out, oldEndAt, oldEnd);
    patch32le(out, oldDirSizeAt, static_cast<std::uint32_t>(oldEnd - oldFat - 59));

    // Damaged newest block: correct header, nonsensical body (too short for
    // its row count), pointing back at the healthy block.
    out.insert(out.end(), 4, 0xff);
    const std::size_t badFat = out.size();
    out.insert(out.end(), {'#', 'F', 'T', '4'});
    put64le(out, oldFat);        // next: the healthy older block
    put64le(out, 2000);          // savepoint (newer)
    const std::size_t badEndAt = out.size();
    put64le(out, 0);             // block end, patched
    put64le(out, 0);
    put64le(out, 0);
    put64le(out, 5);             // row count larger than the body
    const std::size_t badDirSizeAt = out.size();
    put32le(out, 0);             // directory size, patched
    put32le(out, 0xc0000001);
    put32le(out, 0x00000003);
    put32le(out, 0);             // first tag
    put32le(out, 0);             // b68: claims a row directory
    out.insert(out.end(), 16, 0);
    const std::size_t badEnd = out.size();
    patch64le(out, badEndAt, badEnd);
    patch32le(out, badDirSizeAt, static_cast<std::uint32_t>(badEnd - badFat - 59));
    patch64le(out, 16, badFat);  // o1: newest (damaged) block first
    patch64le(out, 24, badEnd);  // o2
    return out;
}

void testV12Grammar() {
    using pittore::io::af_detail::Archive;
    using pittore::io::af_detail::ArchiveEntry;

    const auto expectDoc = [](const Archive& ar, const char* what, std::size_t wantEntries,
                              const std::vector<std::uint8_t>& wantPayload) {
        check(ar.entries.size() == wantEntries, what);
        const ArchiveEntry* doc = ar.head("doc.dat");
        check(doc != nullptr, "v12: doc.dat resolves");
        if (doc) {
            const auto plain = ar.extract(*doc);
            check(plain == wantPayload, "v12: doc.dat payload round-trips");
        }
    };

    // Row directory: two rows, names, tail chain, trailer.
    {
        const std::vector<std::uint8_t> docPayload = {'d', 'o', 'c', '!', 1, 2, 3};
        const std::vector<std::uint8_t> tilePayload = {'t', 'i', 'l', 'e', 9, 9};
        const auto bytes = synthRowDirectory();
        try {
            const Archive ar = Archive::parse(bytes);
            check(ar.entries.size() == 2, "v12 row directory: two entries");
            const ArchiveEntry* doc = ar.head("doc.dat");
            check(doc != nullptr, "v12 row directory: doc.dat head");
            if (doc) check(ar.extract(*doc) == docPayload, "v12 row directory: doc payload");
            const ArchiveEntry* tile = ar.head("d/1");
            check(tile != nullptr, "v12 row directory: d/1 head");
            if (tile)
                check(ar.extract(*tile) == tilePayload, "v12 row directory: tile payload");
            check(ar.names.count("doc.dat") == 1 && ar.names.count("d/1") == 1,
                  "v12 row directory: name map built");
        } catch (const std::exception& e) {
            std::printf("  row directory threw: %s\n", e.what());
            check(false, "v12 row directory parses");
        }
    }

    // Delta/checkpoint block: token skipped, serial-1 struct resolves the doc.
    {
        const std::vector<std::uint8_t> docPayload = {'d', 'e', 'l', 't', 'a'};
        const auto bytes = synthDeltaBlock();
        try {
            const Archive ar = Archive::parse(bytes);
            expectDoc(ar, "v12 delta: one entry", 1, docPayload);
        } catch (const std::exception& e) {
            std::printf("  delta block threw: %s\n", e.what());
            check(false, "v12 delta block parses");
        }
    }

    // Damaged newest block: dropped whole, older block still resolves.
    {
        const std::vector<std::uint8_t> docPayload = {'o', 'l', 'd', 'd', 'o', 'c'};
        const auto bytes = synthDamagedChain();
        try {
            const Archive ar = Archive::parse(bytes);
            check(ar.entries.size() == 1, "v12 damaged block: entries from the old block only");
            const ArchiveEntry* doc = ar.head("doc.dat");
            if (doc) check(doc->savepoint == 1000, "v12 damaged block: old savepoint wins");
            expectDoc(ar, "v12 damaged block: one entry", 1, docPayload);
        } catch (const std::exception& e) {
            std::printf("  damaged chain threw: %s\n", e.what());
            check(false, "v12 damaged block falls back");
        }
    }

    // Writer round trip: the fresh-export container must parse back with the
    // same names and payloads (the reader/writer grammar contract).
    {
        using pittore::io::af_detail::OutEntry;
        using pittore::io::af_detail::writeContainer;
        std::vector<OutEntry> entries;
        const std::vector<std::uint8_t> docPayload = {'e', 'x', 'p', 'o', 'r', 't'};
        const std::vector<std::uint8_t> tilePayload = {'t', 'i', 'l', 'e', '0', '1'};
        for (const auto& p : {docPayload, tilePayload}) {
            OutEntry e;
            e.payload = p;
            e.plainSize = p.size();
            e.comp = 0;
            e.crc = crc32Of(p);
            entries.push_back(std::move(e));
        }
        entries[0].name = "doc.dat";
        entries[1].name = "d/1";
        const auto bytes = writeContainer(entries, {}, 1234);
        try {
            const Archive ar = Archive::parse(bytes);
            check(ar.entries.size() == 2, "writer round trip: two entries");
            const ArchiveEntry* doc = ar.head("doc.dat");
            check(doc != nullptr && ar.extract(*doc) == docPayload,
                  "writer round trip: doc payload");
            const ArchiveEntry* tile = ar.head("d/1");
            check(tile != nullptr && ar.extract(*tile) == tilePayload,
                  "writer round trip: tile payload");
        } catch (const std::exception& e) {
            std::printf("  writer round trip threw: %s\n", e.what());
            check(false, "writer round trip parses");
        }
    }
}

}  // namespace

int main() {
    testProbe();
    testPngDecodes();
    testInterlace();
    testTailJson();
    testMetadataScan();
    testLayeredDecode();
    testRealFile();
    testV12Grammar();

    if (failures == 0) {
        std::printf("test_af: all checks passed\n");
        return 0;
    }
    std::printf("test_af: %d failures\n", failures);
    return 1;
}