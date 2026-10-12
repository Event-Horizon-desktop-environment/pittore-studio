// tests/test_tiff.cpp — TIFF probe + banded scanline decode.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "engine/io/tiff.h"

#ifdef PITTORE_TIFF
extern "C" {
#include <tiffio.h>
}
#endif

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";               \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

// Scratch files land in the system temp directory instead of a fixed path, so
// the test honours $TMPDIR and never assumes /tmp exists or is writable.
static std::string scratchPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// Optional coated CMYK profile for the embedded-profile round trip. No ICC
// binaries ship in the repo, so this takes the first path that exists: an
// override from the environment, then the profile Ghostscript installs. With
// none present the section prints a skip line instead of failing.
static const char* cmykFixturePath() {
    if (const char* env = std::getenv("PITTORE_CMYK_ICC")) return env;
    static const char* const kSystem[] = {
        "/usr/share/ghostscript/iccprofiles/default_cmyk.icc",
        "/usr/share/color/icc/ghostscript/default_cmyk.icc",
    };
    for (const char* c : kSystem) {
        if (std::FILE* f = std::fopen(c, "rb")) {
            std::fclose(f);
            return c;
        }
    }
    return kSystem[0];
}

int main() {
#ifndef PITTORE_TIFF
    std::cout << "SKIP (no PITTORE_TIFF)\n";
    return 0;
#else
    // 1. Encode a small 7x5 RGBA image, write to temp file.
    const std::uint32_t w = 7, h = 5;
    std::vector<std::uint16_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i + 0] = static_cast<std::uint16_t>((x * 37 + y * 11) % 256 * 257);
            rgba[i + 1] = static_cast<std::uint16_t>((x * 53 + y * 29) % 256 * 257);
            rgba[i + 2] = static_cast<std::uint16_t>((x * 17 + y * 71) % 256 * 257);
            rgba[i + 3] = 65535;
        }
    std::vector<std::uint8_t> enc;
    CHECK(pittore::io::tiffEncodeRgba16(w, h, 72, rgba.data(), enc));
    CHECK(!enc.empty());

    const std::string tmpPath = scratchPath("pittore_tiff_test.tif");
    {
        FILE* f = std::fopen(tmpPath.c_str(), "wb");
        CHECK(f != nullptr);
        if (f) {
            CHECK(std::fwrite(enc.data(), 1, enc.size(), f) == enc.size());
            std::fclose(f);
        }
    }

    // 2. Probe recovers dimensions + dpi without decoding.
    {
        std::uint32_t pw = 0, ph = 0;
        int dpi = 0;
        CHECK(pittore::io::tiffProbeFile(tmpPath.c_str(), pw, ph, dpi));
        CHECK(pw == w && ph == h);
        CHECK(dpi == 72);
    }
    {
        std::uint32_t pw = 0, ph = 0;
        int dpi = 0;
        CHECK(!pittore::io::tiffProbeFile(
            scratchPath("does-not-exist.tif").c_str(), pw, ph, dpi));
    }

    // 3. Banded file decode matches the buffered RGBA path (>>8 of x257).
    {
        std::vector<std::uint8_t> banded(static_cast<std::size_t>(w) * h * 4, 0);
        std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
            [&](std::uint32_t y, const std::uint8_t* row) -> bool {
            if (y >= h) return false;
            std::copy(row, row + static_cast<std::ptrdiff_t>(w) * 4,
                      banded.data() + static_cast<std::size_t>(y) * w * 4);
            return true;
        };
        CHECK(pittore::io::tiffDecodeFileBanded(tmpPath.c_str(), sink));
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                CHECK(banded[i + 0] == static_cast<std::uint8_t>(rgba[i + 0] >> 8));
                CHECK(banded[i + 1] == static_cast<std::uint8_t>(rgba[i + 1] >> 8));
                CHECK(banded[i + 2] == static_cast<std::uint8_t>(rgba[i + 2] >> 8));
                CHECK(banded[i + 3] == 255);
            }
    }

    // 4. Banded buffer decode matches too.
    {
        std::vector<std::uint8_t> banded(static_cast<std::size_t>(w) * h * 4, 0);
        std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
            [&](std::uint32_t y, const std::uint8_t* row) -> bool {
            if (y >= h) return false;
            std::copy(row, row + static_cast<std::ptrdiff_t>(w) * 4,
                      banded.data() + static_cast<std::size_t>(y) * w * 4);
            return true;
        };
        CHECK(pittore::io::tiffDecodeBufferBanded(enc, sink));
        CHECK(banded[0] == static_cast<std::uint8_t>(rgba[0] >> 8));
    }

    // 5. Buffered path still works on the same bytes.
    {
        std::uint32_t dw = 0, dh = 0;
        std::vector<std::uint16_t> out;
        int dpi = 0;
        CHECK(pittore::io::tiffDecodeRgba16(enc, dw, dh, out, dpi));
        CHECK(dw == w && dh == h && dpi == 72);
        CHECK(out.size() == rgba.size());
        for (std::size_t i = 0; i < out.size(); ++i) CHECK(out[i] == rgba[i]);
    }

    // 6. Grayscale MINISBLACK decodes to grey RGB + opaque alpha.
    {
        const std::string grayPath = scratchPath("pittore_tiff_gray.tif");
        TIFF* tif = TIFFOpen(grayPath.c_str(), "w");
        CHECK(tif != nullptr);
        if (tif) {
            const std::uint32_t gw = 4, gh = 3;
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, gw);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, gh);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, gh);
            std::uint8_t row[4] = {0, 85, 170, 255};
            for (std::uint32_t y = 0; y < gh; ++y)
                CHECK(TIFFWriteScanline(tif, row, y, 0) >= 0);
            TIFFClose(tif);
            std::vector<std::uint8_t> got(static_cast<std::size_t>(gw) * gh * 4, 0);
            std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
                [&](std::uint32_t y, const std::uint8_t* r) -> bool {
                std::copy(r, r + gw * 4, got.data() + static_cast<std::size_t>(y) * gw * 4);
                return true;
            };
            CHECK(pittore::io::tiffDecodeFileBanded(grayPath.c_str(), sink));
            CHECK(got[0] == 0 && got[1] == 0 && got[2] == 0 && got[3] == 255);
            CHECK(got[4] == 85 && got[5] == 85 && got[6] == 85 && got[7] == 255);
            CHECK(got[8] == 170 && got[11] == 255);
            CHECK(got[12] == 255 && got[15] == 255);
        }
    }

    // 7. Subsampled file decode box-averages each factor×factor block.
    // 7x5 at factor 2 -> 4x3; expected values recomputed independently
    // from the 8-bit source (rgba>>8), including shrunken edge blocks.
    {
        const int factor = 2;
        const std::uint32_t ow = (w + 1) / 2, oh = (h + 1) / 2;
        std::vector<std::uint8_t> sub(static_cast<std::size_t>(ow) * oh * 4, 0);
        std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
            [&](std::uint32_t y, const std::uint8_t* r) -> bool {
            if (y >= oh) return false;
            std::copy(r, r + static_cast<std::ptrdiff_t>(ow) * 4,
                      sub.data() + static_cast<std::size_t>(y) * ow * 4);
            return true;
        };
        std::uint32_t dw = 0, dh = 0;
        int ddpi = 0;
        CHECK(pittore::io::tiffDecodeFileSubsampled(tmpPath.c_str(), factor,
                                                     dw, dh, ddpi, sink));
        CHECK(dw == ow && dh == oh);
        CHECK(ddpi == 72);
        auto src8 = [&](std::uint32_t x, std::uint32_t y, int c) -> int {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            return (c == 3) ? 255 : static_cast<int>(rgba[i + c] >> 8);
        };
        for (std::uint32_t oy = 0; oy < oh; ++oy)
            for (std::uint32_t ox = 0; ox < ow; ++ox) {
                const std::uint32_t x0 = ox * 2, y0 = oy * 2;
                const std::uint32_t x1 = std::min(x0 + 2, w);
                const std::uint32_t y1 = std::min(y0 + 2, h);
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (std::uint32_t y = y0; y < y1; ++y)
                        for (std::uint32_t x = x0; x < x1; ++x)
                            sum += src8(x, y, c);
                    const int n = (x1 - x0) * (y1 - y0);
                    const int want = (sum + n / 2) / n;
                    const std::size_t i =
                        (static_cast<std::size_t>(oy) * ow + ox) * 4;
                    CHECK(sub[i + c] == want);
                }
            }
    }

    // 8. Subsampled buffer twin agrees; factor 1 delegates to banded;
    // factor 0 and oversized factors behave (false / 1x1).
    {
        const std::uint32_t ow = (w + 1) / 2, oh = (h + 1) / 2;
        std::uint32_t dw = 0, dh = 0;
        int ddpi = 0;
        std::vector<std::uint8_t> sub(static_cast<std::size_t>(ow) * oh * 4,
                                      0);
        std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
            [&](std::uint32_t y, const std::uint8_t* r) -> bool {
            if (y >= oh) return false;
            std::copy(r, r + static_cast<std::ptrdiff_t>(ow) * 4,
                      sub.data() + static_cast<std::size_t>(y) * ow * 4);
            return true;
        };
        CHECK(pittore::io::tiffDecodeBufferSubsampled(enc, 2, dw, dh, ddpi,
                                                       sink));
        CHECK(dw == ow && dh == oh);

        // factor 1 == banded output.
        std::vector<std::uint8_t> f1(static_cast<std::size_t>(w) * h * 4, 0);
        std::function<bool(std::uint32_t, const std::uint8_t*)> sink1 =
            [&](std::uint32_t y, const std::uint8_t* r) -> bool {
            std::copy(r, r + static_cast<std::ptrdiff_t>(w) * 4,
                      f1.data() + static_cast<std::size_t>(y) * w * 4);
            return true;
        };
        dw = 0;
        dh = 0;
        CHECK(pittore::io::tiffDecodeBufferSubsampled(enc, 1, dw, dh, ddpi,
                                                       sink1));
        CHECK(dw == w && dh == h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                CHECK(f1[i + 0] == static_cast<std::uint8_t>(rgba[i + 0] >> 8));
                CHECK(f1[i + 3] == 255);
            }

        // factor 0 rejected; factor 8 on 7x5 -> single average pixel.
        std::uint32_t zw = 0, zh = 0;
        int zdpi = 0;
        CHECK(!pittore::io::tiffDecodeBufferSubsampled(enc, 0, zw, zh, zdpi,
                                                        sink1));
        CHECK(!pittore::io::tiffDecodeBufferSubsampled(enc, -3, zw, zh, zdpi,
                                                        sink1));
        std::vector<std::uint8_t> one(4, 0);
        std::function<bool(std::uint32_t, const std::uint8_t*)> sinkOne =
            [&](std::uint32_t y, const std::uint8_t* r) -> bool {
            if (y != 0) return false;
            std::copy(r, r + 4, one.data());
            return true;
        };
        CHECK(pittore::io::tiffDecodeBufferSubsampled(enc, 8, zw, zh, zdpi,
                                                       sinkOne));
        CHECK(zw == 1 && zh == 1);
    }

    // 7. Embedded ICC profile description round-trips through the tag.
    {
        const std::string iccPath = scratchPath("pittore_tiff_icc.tif");
        // Minimal 'desc' profile: stock 128-byte header + one tag.
        std::vector<std::uint8_t> icc(128, 0);
        auto put32 = [&](std::uint32_t v) {
            icc.push_back(static_cast<std::uint8_t>(v >> 24));
            icc.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
            icc.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
            icc.push_back(static_cast<std::uint8_t>(v & 0xff));
        };
        const std::string desc = "Test TIFF";
        put32(1);
        icc.insert(icc.end(), {'d', 'e', 's', 'c'});
        put32(144);
        put32(static_cast<std::uint32_t>(12 + desc.size() + 1));
        icc.insert(icc.end(), {'d', 'e', 's', 'c'});
        put32(0);
        put32(static_cast<std::uint32_t>(desc.size()) + 1);
        icc.insert(icc.end(), desc.begin(), desc.end());
        icc.push_back(0);
        TIFF* tif = TIFFOpen(iccPath.c_str(), "w");
        CHECK(tif != nullptr);
        if (tif) {
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 1);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 1);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);
            TIFFSetField(tif, TIFFTAG_ICCPROFILE,
                         static_cast<std::uint32_t>(icc.size()), icc.data());
            std::uint8_t px = 0;
            CHECK(TIFFWriteScanline(tif, &px, 0, 0) >= 0);
            TIFFClose(tif);
            CHECK(pittore::io::tiffIccProfileName(iccPath.c_str()) ==
                  "Test TIFF");
            FILE* f = std::fopen(iccPath.c_str(), "rb");
            CHECK(f != nullptr);
            if (f) {
                std::fseek(f, 0, SEEK_END);
                const long len = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                std::vector<std::uint8_t> bytes(
                    static_cast<std::size_t>(len));
                CHECK(std::fread(bytes.data(), 1, bytes.size(), f) ==
                      bytes.size());
                std::fclose(f);
                CHECK(pittore::io::tiffIccProfileData(bytes) == "Test TIFF");
            }
            std::remove(iccPath.c_str());
        }
        // The tag-less file from section 1 reports nothing.
        CHECK(pittore::io::tiffIccProfileName(tmpPath.c_str()).empty());
    }

    // --- CMYK (SEPARATED) write -> read round trip --------------------------
    {
        const std::uint32_t cw = 8, ch = 8;
        std::vector<std::uint16_t> appear(static_cast<std::size_t>(cw) * ch * 4);
        for (std::uint32_t y = 0; y < ch; ++y)
            for (std::uint32_t x = 0; x < cw; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * cw + x) * 4;
                std::uint16_t r = 0, g = 0, b = 0;
                if (x < 4 && y < 4) {
                    r = 65535;  // red
                } else if (x >= 4 && y < 4) {
                    r = g = b = 65535;  // white
                } else if (x < 4) {
                    r = g = b = 32768;  // mid gray
                }  // else black
                appear[i + 0] = r;
                appear[i + 1] = g;
                appear[i + 2] = b;
                appear[i + 3] = (x == 0 && y == 7) ? 30000 : 65535;
            }

        // Naive separation, 8-bit, with alpha.
        pittore::io::TiffEncodeOptions opt;
        opt.bits = 8;
        opt.cmyk = true;
        std::vector<std::uint8_t> enc8;
        CHECK(pittore::io::tiffEncodeExport(cw, ch, 300, appear.data(), opt,
                                             enc8));
        CHECK(!enc8.empty());
        if (!enc8.empty()) {
            // Banded reader (ink rows -> appearance).
            std::vector<std::uint8_t> got(static_cast<std::size_t>(cw) * ch * 4,
                                          0);
            std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
                [&](std::uint32_t y, const std::uint8_t* row) {
                    std::memcpy(got.data() + static_cast<std::size_t>(y) * cw * 4,
                                row, static_cast<std::size_t>(cw) * 4);
                    return true;
                };
            bool ok = pittore::io::tiffDecodeBufferBanded(enc8, sink);
            CHECK(ok);
            const std::uint8_t* red = &got[0];
            CHECK(red[0] > 210 && red[1] < 60 && red[2] < 60);
            const std::uint8_t* wht = &got[4 * 4];
            CHECK(wht[0] > 210 && wht[1] > 210 && wht[2] > 210);
            const std::uint8_t* gry = &got[32 * 4];
            CHECK(gry[0] > 115 && gry[0] < 140 && std::abs(gry[0] - gry[1]) <= 2);
            const std::uint8_t* blk = &got[63 * 4];
            CHECK(blk[0] < 25 && blk[1] < 25 && blk[2] < 25);
            CHECK(got[4] == 255);              // red pixel is opaque
            const std::uint8_t* tr = &got[(7u * cw + 0) * 4];
            CHECK(tr[3] == static_cast<std::uint8_t>(30000 >> 8));  // alpha kept

            // RGBA16 entry routes the same CMYK rows too.
            std::vector<std::uint16_t> out16;
            std::uint32_t dw = 0, dh = 0;
            int dpi = 0;
            CHECK(pittore::io::tiffDecodeRgba16(enc8, dw, dh, out16, dpi));
            CHECK(dw == cw && dh == ch);
            CHECK(dpi == 300);
            if (out16.size() == static_cast<std::size_t>(cw) * ch * 4) {
                CHECK(out16[0] > 54000 && out16[1] < 15000);
                CHECK(std::abs(static_cast<int>(out16[32 * 4]) - 32768) < 1000);
                CHECK(out16[(7u * cw) * 4 + 3] < 31000 &&
                      out16[(7u * cw) * 4 + 3] > 29000);
            }

            // 16-bit separation, no alpha (4 samples).
            pittore::io::TiffEncodeOptions o16;
            o16.bits = 16;
            o16.cmyk = true;
            o16.withAlpha = false;
            std::vector<std::uint8_t> enc16;
            CHECK(pittore::io::tiffEncodeExport(cw, ch, 0, appear.data(), o16,
                                                 enc16));
            if (!enc16.empty()) {
                std::uint32_t w16 = 0, h16 = 0;
                std::vector<std::uint16_t> back16;
                int d16 = 0;
                CHECK(pittore::io::tiffDecodeRgba16(enc16, w16, h16, back16,
                                                     d16));
                CHECK(w16 == cw && h16 == ch);
                if (back16.size() == static_cast<std::size_t>(cw) * ch * 4) {
                    CHECK(back16[0] > 54000 && back16[1] < 15000);
                    CHECK(back16[(7u * cw) * 4 + 3] == 65535);  // alpha absent -> opaque
                }
            }
        }

        // Profiled write (fixture below): profile embedded + decode goes
        // through it rather than the naive core.
        FILE* pf = std::fopen(cmykFixturePath(), "rb");
        if (!pf) {
            std::printf("  (skip: no CMYK profile fixture for tiff)\n");
        } else {
            std::vector<std::uint8_t> icc;
            int c = 0;
            while ((c = std::fgetc(pf)) != EOF)
                icc.push_back(static_cast<std::uint8_t>(c));
            std::fclose(pf);
            pittore::io::TiffEncodeOptions po;
            po.bits = 8;
            po.cmyk = true;
            po.icc = icc;
            std::vector<std::uint8_t> penc;
            CHECK(pittore::io::tiffEncodeExport(cw, ch, 300, appear.data(), po,
                                                 penc));
            if (!penc.empty()) {
                CHECK(!pittore::io::tiffIccProfileData(penc).empty());
                std::vector<std::uint8_t> got(static_cast<std::size_t>(cw) *
                                                  ch * 4,
                                              0);
                std::function<bool(std::uint32_t, const std::uint8_t*)> psink =
                    [&](std::uint32_t y, const std::uint8_t* row) {
                        std::memcpy(
                            got.data() + static_cast<std::size_t>(y) * cw * 4,
                            row, static_cast<std::size_t>(cw) * 4);
                        return true;
                    };
                bool ok = pittore::io::tiffDecodeBufferBanded(penc, psink);
                CHECK(ok);
                const std::uint8_t* wht = &got[4 * 4];
                CHECK(wht[0] > 215 && wht[1] > 215 && wht[2] > 215);
                const std::uint8_t* blk = &got[63 * 4];
                CHECK(blk[0] < 90 && blk[1] < 90 && blk[2] < 90);
            }
        }
    }

    // Native CMYK decoding preserves the samples while applying the TIFF
    // orientation transform expected by the RGB preview import path.
    {
        const std::string rotatedPath =
            scratchPath("pittore_tiff_cmyk_rotated.tif");
        constexpr std::uint32_t rw = 3, rh = 2;
        TIFF* tif = TIFFOpen(rotatedPath.c_str(), "w");
        CHECK(tif != nullptr);
        if (tif) {
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, rw);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, rh);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
            TIFFSetField(tif, TIFFTAG_INKSET, INKSET_CMYK);
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_RIGHTTOP);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rh);
            for (std::uint32_t y = 0; y < rh; ++y) {
                std::uint8_t row[rw * 4]{};
                for (std::uint32_t x = 0; x < rw; ++x)
                    for (std::uint32_t c = 0; c < 4; ++c)
                        row[x * 4 + c] =
                            static_cast<std::uint8_t>(y * 60 + x * 20 + c);
                CHECK(TIFFWriteScanline(tif, row, y, 0) >= 0);
            }
            TIFFClose(tif);
            std::uint32_t dw = 0, dh = 0;
            std::vector<std::uint16_t> inks;
            CHECK(pittore::io::tiffDecodeCmykFile(
                rotatedPath.c_str(), dw, dh, inks));
            CHECK(dw == rh && dh == rw);
            CHECK(inks.size() == static_cast<std::size_t>(rw) * rh * 4);
            if (inks.size() == static_cast<std::size_t>(rw) * rh * 4) {
                // RIGHTTOP maps source (x=0,y=0) to destination (1,0),
                // source (2,1) to destination (0,2).
                const std::size_t first = (0u * dw + 1u) * 4;
                CHECK(inks[first] == 0 && inks[first + 3] == 3 * 257);
                const std::size_t last = (2u * dw + 0u) * 4;
                CHECK(inks[last] == 100 * 257 && inks[last + 3] == 103 * 257);
            }
        }
        std::remove(rotatedPath.c_str());
    }

    // Planar-separate CMYK stores one scanline per ink. Preserve those device
    // values too; libtiff still provides the profile/preview path separately.
    {
        const std::string planarPath =
            scratchPath("pittore_tiff_cmyk_planar.tif");
        constexpr std::uint32_t pw = 3, ph = 2;
        TIFF* tif = TIFFOpen(planarPath.c_str(), "w");
        CHECK(tif != nullptr);
        if (tif) {
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, pw);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, ph);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
            TIFFSetField(tif, TIFFTAG_INKSET, INKSET_CMYK);
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
            TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
            TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, ph);
            for (std::uint32_t c = 0; c < 4; ++c)
                for (std::uint32_t y = 0; y < ph; ++y) {
                    std::uint8_t row[pw]{};
                    for (std::uint32_t x = 0; x < pw; ++x)
                        row[x] = static_cast<std::uint8_t>(c * 40 + y * 10 + x);
                    CHECK(TIFFWriteScanline(tif, row, y, c) >= 0);
                }
            TIFFClose(tif);
            std::uint32_t dw = 0, dh = 0;
            std::vector<std::uint16_t> inks;
            CHECK(pittore::io::tiffDecodeCmykFile(
                planarPath.c_str(), dw, dh, inks));
            CHECK(dw == pw && dh == ph);
            CHECK(inks.size() == static_cast<std::size_t>(pw) * ph * 4);
            if (inks.size() == static_cast<std::size_t>(pw) * ph * 4) {
                const std::size_t pixel = (1u * pw + 2u) * 4;
                CHECK(inks[pixel] == 12 * 257);
                CHECK(inks[pixel + 1] == 52 * 257);
                CHECK(inks[pixel + 2] == 92 * 257);
                CHECK(inks[pixel + 3] == 132 * 257);
            }
        }
        std::remove(planarPath.c_str());
    }

    // Tiled TIFF stores complete fixed-size tile blocks instead of scanlines.
    {
        const std::string tiledPath =
            scratchPath("pittore_tiff_cmyk_tiled.tif");
        constexpr std::uint32_t tw = 16, th = 16;
        TIFF* tif = TIFFOpen(tiledPath.c_str(), "w");
        CHECK(tif != nullptr);
        if (tif) {
            TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, tw);
            TIFFSetField(tif, TIFFTAG_IMAGELENGTH, th);
            TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
            TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
            TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
            TIFFSetField(tif, TIFFTAG_INKSET, INKSET_CMYK);
            TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
            TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
            TIFFSetField(tif, TIFFTAG_TILEWIDTH, tw);
            TIFFSetField(tif, TIFFTAG_TILELENGTH, th);
            std::vector<std::uint16_t> tile(tw * th * 4);
            for (std::uint32_t y = 0; y < th; ++y)
                for (std::uint32_t x = 0; x < tw; ++x)
                    for (std::uint32_t c = 0; c < 4; ++c)
                        tile[(y * tw + x) * 4 + c] =
                            static_cast<std::uint16_t>(1000 + y * 100 + x * 4 + c);
            CHECK(TIFFWriteTile(tif, tile.data(), 0, 0, 0, 0) >= 0);
            TIFFClose(tif);
            std::uint32_t dw = 0, dh = 0;
            std::vector<std::uint16_t> inks;
            CHECK(pittore::io::tiffDecodeCmykFile(
                tiledPath.c_str(), dw, dh, inks));
            CHECK(dw == tw && dh == th);
            CHECK(inks.size() == static_cast<std::size_t>(tw) * th * 4);
            if (inks.size() == static_cast<std::size_t>(tw) * th * 4) {
                const std::size_t i = (7u * tw + 5u) * 4;
                CHECK(inks[i] == 1720);
                CHECK(inks[i + 1] == 1721);
                CHECK(inks[i + 2] == 1722);
                CHECK(inks[i + 3] == 1723);
            }
        }
        std::remove(tiledPath.c_str());
    }

    std::remove(tmpPath.c_str());
    std::remove(scratchPath("pittore_tiff_gray.tif").c_str());
    if (failures == 0) std::cout << "test_tiff OK\n";
    return failures == 0 ? 0 : 1;
#endif
}
