#include "engine/io/tiff.h"

#include "engine/color/convert.h"
#include "engine/color/icc_convert.h"
#include "engine/color/separate.h"
#include "engine/io/icc.h"

extern "C" {
#include <tiffio.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pittore::io {

namespace {

// In-memory file state for TIFFClientOpen. For reads `owned` is true and
// memClose destroys the state; for writes the caller owns the state and keeps
// it alive past TIFFClose so the finished bytes can be copied out.
struct MemState {
    std::vector<std::uint8_t> bytes;
    std::size_t pos = 0;
    bool owned = true;
};

tmsize_t memRead(thandle_t h, void* buf, tmsize_t size) {
    if (size <= 0) return 0;
    auto* s = static_cast<MemState*>(h);
    if (s->pos >= s->bytes.size()) return 0;
    const std::size_t avail = s->bytes.size() - s->pos;
    const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(size), avail);
    std::memcpy(buf, s->bytes.data() + s->pos, take);
    s->pos += take;
    return static_cast<tmsize_t>(take);
}

tmsize_t memWrite(thandle_t h, void* buf, tmsize_t size) {
    if (size <= 0) return 0;
    auto* s = static_cast<MemState*>(h);
    if (s->pos + static_cast<std::size_t>(size) > s->bytes.size())
        s->bytes.resize(s->pos + static_cast<std::size_t>(size));
    std::memcpy(s->bytes.data() + s->pos, buf, static_cast<std::size_t>(size));
    s->pos += static_cast<std::size_t>(size);
    return size;
}

toff_t memSeek(thandle_t h, toff_t off, int whence) {
    auto* s = static_cast<MemState*>(h);
    std::int64_t target = 0;
    switch (whence) {
        case SEEK_SET: target = static_cast<std::int64_t>(off); break;
        case SEEK_CUR:
            target = static_cast<std::int64_t>(s->pos) + static_cast<std::int64_t>(off);
            break;
        case SEEK_END:
            target = static_cast<std::int64_t>(s->bytes.size()) +
                     static_cast<std::int64_t>(off);
            break;
        default: return static_cast<toff_t>(-1);
    }
    if (target < 0) {
        s->pos = 0;
        return static_cast<toff_t>(-1);
    }
    s->pos = static_cast<std::size_t>(target);
    return static_cast<toff_t>(s->pos);
}

int memClose(thandle_t h) {
    auto* s = static_cast<MemState*>(h);
    if (s->owned) delete s;
    return 0;
}

toff_t memSize(thandle_t h) {
    auto* s = static_cast<MemState*>(h);
    return static_cast<toff_t>(s->bytes.size());
}

int memMap(thandle_t, void**, toff_t*) { return 0; }
void memUnmap(thandle_t, void*, toff_t) {}

TIFF* openForRead(const std::vector<std::uint8_t>& data) {
    auto* s = new MemState;
    s->bytes = data;
    s->pos = 0;
    s->owned = true;
    TIFF* tif = TIFFClientOpen("mem", "r", s, memRead, memWrite, memSeek, memClose,
                               memSize, memMap, memUnmap);
    if (!tif) {
        delete s;
        return nullptr;
    }
    return tif;
}

TIFF* openForWrite(MemState* s) {
    return TIFFClientOpen("mem", "w", s, memRead, memWrite, memSeek, memClose,
                          memSize, memMap, memUnmap);
}

// Pixel-format description for one TIFF directory, validated up front so
// exotic variants (palette/YCbCr/CMYK/separate-planar/rotated) decline fast.
struct BandedFormat {
    std::uint16_t bits = 0;
    std::uint16_t samples = 0;
    bool gray = false;
    bool cmyk = false;
    bool whiteIsZero = false;
    bool assocAlpha = false;
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    std::uint16_t planar = PLANARCONFIG_CONTIG;
};

// Embedded ICC profile bytes for the open directory (used to make the CMYK
// scanline conversion profiled); empty when the file carries none.
std::vector<std::uint8_t> iccOfOpenTiff(TIFF* tif) {
    std::uint32_t len = 0;
    void* ptr = nullptr;
    if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &len, &ptr) && ptr && len > 0 &&
        len < (1u << 24))
        return std::vector<std::uint8_t>(static_cast<std::uint8_t*>(ptr),
                                         static_cast<std::uint8_t*>(ptr) + len);
    return {};
}

bool readBandedFormat(TIFF* tif, BandedFormat& fmt,
                      bool allowOrientation = false,
                      bool allowSeparateCmyk = false) {
    std::uint16_t bits = 0, samples = 0, photo = 0, planar = 0, orient = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orient);
    std::uint16_t extraCount = 0;
    std::uint16_t* extraTypes = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &extraCount, &extraTypes);
    const bool hasAlpha = extraCount > 0 && extraTypes &&
        (extraTypes[0] == EXTRASAMPLE_ASSOCALPHA ||
         extraTypes[0] == EXTRASAMPLE_UNASSALPHA);
    if (orient == 0) orient = ORIENTATION_TOPLEFT;
    if (allowOrientation) {
        if (orient < ORIENTATION_TOPLEFT || orient > ORIENTATION_LEFTBOT)
            return false;
    } else if (orient != ORIENTATION_TOPLEFT) {
        return false;
    }
    if (planar != PLANARCONFIG_CONTIG &&
        !(allowSeparateCmyk && photo == PHOTOMETRIC_SEPARATED &&
          planar == PLANARCONFIG_SEPARATE))
        return false;
    if (bits != 8 && bits != 16) return false;
    const bool gray = (photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE);
    const bool rgb = (photo == PHOTOMETRIC_RGB);
    const bool cmyk = (photo == PHOTOMETRIC_SEPARATED);
    if (!gray && !rgb && !cmyk) return false;
    if (gray && (samples != 1 && !(samples == 2 && hasAlpha))) return false;
    if (rgb && (samples != 3 && !(samples == 4 && hasAlpha))) return false;
    if (cmyk && (samples != 4 && !(samples == 5 && hasAlpha))) return false;
    fmt.bits = bits;
    fmt.samples = samples;
    fmt.gray = gray;
    fmt.cmyk = cmyk;
    fmt.whiteIsZero = gray && photo == PHOTOMETRIC_MINISWHITE;
    fmt.assocAlpha =
        extraCount > 0 && extraTypes && extraTypes[0] == EXTRASAMPLE_ASSOCALPHA;
    fmt.orientation = orient;
    fmt.planar = planar;
    return true;
}

// One scanline of raw samples -> W*4 straight-alpha RGBA8. `src` holds
// w*samples*(bits/8) bytes previously read via TIFFReadScanline. For
// PHOTOMETRIC_SEPARATED rows the ink samples go through `cmykXform` when it
// is live (its own naive fallback covers an unusable profile); a null
// transform is the pure-naive conversion.
void convertScanlineToRgba8(const std::uint8_t* src, const BandedFormat& fmt,
                            std::uint32_t w, std::uint8_t* dst,
                            const pittore::color::CmykToSrgb* cmykXform = nullptr) {
    if (fmt.bits == 8) {
        const std::uint8_t* s = src;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t r, g, b, a;
            if (fmt.gray) {
                std::uint8_t v = s[0];
                if (fmt.whiteIsZero) v = static_cast<std::uint8_t>(255 - v);
                r = g = b = v;
                a = (fmt.samples == 2) ? s[1] : 255;
                s += fmt.samples;
            } else if (fmt.cmyk) {
                const float ci = s[0] / 255.0f, mi = s[1] / 255.0f;
                const float yi = s[2] / 255.0f, ki = s[3] / 255.0f;
                a = (fmt.samples == 5) ? s[4] : 255;
                s += fmt.samples;
                float t[3];
                if (cmykXform) {
                    cmykXform->convert(ci, mi, yi, ki, t);
                } else {
                    const pittore::RGBAf v =
                        pittore::color::cmykToRgb({ci, mi, yi, ki}, 1.0f);
                    t[0] = v.r;
                    t[1] = v.g;
                    t[2] = v.b;
                }
                r = static_cast<std::uint8_t>(std::lround(std::clamp(t[0], 0.0f, 1.0f) * 255.0f));
                g = static_cast<std::uint8_t>(std::lround(std::clamp(t[1], 0.0f, 1.0f) * 255.0f));
                b = static_cast<std::uint8_t>(std::lround(std::clamp(t[2], 0.0f, 1.0f) * 255.0f));
            } else {
                r = s[0];
                g = s[1];
                b = s[2];
                a = (fmt.samples == 4) ? s[3] : 255;
                s += fmt.samples;
            }
            if (fmt.assocAlpha && a != 0 && a != 255) {
                // Associated (premultiplied) -> straight.
                r = static_cast<std::uint8_t>((static_cast<int>(r) * 255 + a / 2) / a);
                g = static_cast<std::uint8_t>((static_cast<int>(g) * 255 + a / 2) / a);
                b = static_cast<std::uint8_t>((static_cast<int>(b) * 255 + a / 2) / a);
            } else if (fmt.assocAlpha && a == 0) {
                r = g = b = 0;
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    } else {
        const std::uint16_t* s =
            reinterpret_cast<const std::uint16_t*>(src);
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t r, g, b, a;
            if (fmt.gray) {
                std::uint16_t v = s[0] >> 8;
                if (fmt.whiteIsZero) v = static_cast<std::uint16_t>(255 - v);
                r = g = b = static_cast<std::uint8_t>(v);
                a = (fmt.samples == 2) ? static_cast<std::uint8_t>(s[1] >> 8) : 255;
                s += fmt.samples;
            } else if (fmt.cmyk) {
                const float ci = s[0] / 65535.0f, mi = s[1] / 65535.0f;
                const float yi = s[2] / 65535.0f, ki = s[3] / 65535.0f;
                a = (fmt.samples == 5) ? static_cast<std::uint8_t>(s[4] >> 8) : 255;
                s += fmt.samples;
                float t[3];
                if (cmykXform) {
                    cmykXform->convert(ci, mi, yi, ki, t);
                } else {
                    const pittore::RGBAf v =
                        pittore::color::cmykToRgb({ci, mi, yi, ki}, 1.0f);
                    t[0] = v.r;
                    t[1] = v.g;
                    t[2] = v.b;
                }
                r = static_cast<std::uint8_t>(std::lround(std::clamp(t[0], 0.0f, 1.0f) * 255.0f));
                g = static_cast<std::uint8_t>(std::lround(std::clamp(t[1], 0.0f, 1.0f) * 255.0f));
                b = static_cast<std::uint8_t>(std::lround(std::clamp(t[2], 0.0f, 1.0f) * 255.0f));
            } else {
                r = static_cast<std::uint8_t>(s[0] >> 8);
                g = static_cast<std::uint8_t>(s[1] >> 8);
                b = static_cast<std::uint8_t>(s[2] >> 8);
                a = (fmt.samples == 4) ? static_cast<std::uint8_t>(s[3] >> 8) : 255;
                s += fmt.samples;
            }
            if (fmt.assocAlpha && a != 0 && a != 255) {
                r = static_cast<std::uint8_t>((static_cast<int>(r) * 255 + a / 2) / a);
                g = static_cast<std::uint8_t>((static_cast<int>(g) * 255 + a / 2) / a);
                b = static_cast<std::uint8_t>((static_cast<int>(b) * 255 + a / 2) / a);
            } else if (fmt.assocAlpha && a == 0) {
                r = g = b = 0;
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
}

// Shared banded-scanline core: handles 8/16-bit contiguous grey/RGB/RGBA and
// CMYK(SEPARATED)/CMYKA, TOPLEFT only. Calls sink(y, rgba8) per row. `tif`
// must be open on the first directory; closed by the caller.
bool decodeBandedCore(TIFF* tif, std::uint32_t w, std::uint32_t h,
                      std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    BandedFormat fmt;
    if (!readBandedFormat(tif, fmt)) return false;

    // CMYK rows convert through the embedded profile when there is one (the
    // converter's own fallback keeps an unusable profile honest/naive).
    const std::vector<std::uint8_t> icc =
        fmt.cmyk ? iccOfOpenTiff(tif) : std::vector<std::uint8_t>{};
    const pittore::color::CmykToSrgb conv(icc.data(), icc.size());

    const std::size_t srcRowBytes =
        static_cast<std::size_t>(w) * fmt.samples * (fmt.bits / 8);
    std::vector<std::uint8_t> src(srcRowBytes);
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * 4);

    for (std::uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(tif, src.data(), y, 0) < 0) return false;
        convertScanlineToRgba8(src.data(), fmt, w, dst.data(),
                               fmt.cmyk ? &conv : nullptr);
        if (!sink(y, dst.data())) return false;
    }
    return true;
}

// Box-averaged subsample core: each factor×factor source block (edge blocks
// shrink) averages into one output pixel. `tif` open on first directory.
bool decodeSubsampledCore(TIFF* tif, std::uint32_t w, std::uint32_t h,
                          int factor, std::uint32_t wOut, std::uint32_t hOut,
                          std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    BandedFormat fmt;
    if (!readBandedFormat(tif, fmt)) return false;

    const std::vector<std::uint8_t> icc =
        fmt.cmyk ? iccOfOpenTiff(tif) : std::vector<std::uint8_t>{};
    const pittore::color::CmykToSrgb conv(icc.data(), icc.size());

    const std::size_t srcRowBytes =
        static_cast<std::size_t>(w) * fmt.samples * (fmt.bits / 8);
    std::vector<std::uint8_t> src(srcRowBytes);
    std::vector<std::uint8_t> convRow(static_cast<std::size_t>(w) * 4);
    std::vector<std::uint32_t> acc(static_cast<std::size_t>(wOut) * 4, 0);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(wOut) * 4, 0);

    for (std::uint32_t oy = 0; oy < hOut; ++oy) {
        const std::uint32_t y0 = oy * static_cast<std::uint32_t>(factor);
        std::uint32_t y1 = y0 + static_cast<std::uint32_t>(factor);
        if (y1 > h) y1 = h;
        std::fill(acc.begin(), acc.end(), 0);
        for (std::uint32_t y = y0; y < y1; ++y) {
            if (TIFFReadScanline(tif, src.data(), y, 0) < 0) return false;
            convertScanlineToRgba8(src.data(), fmt, w, convRow.data(),
                                   fmt.cmyk ? &conv : nullptr);
            for (std::uint32_t ox = 0; ox < wOut; ++ox) {
                const std::uint32_t x0 = ox * static_cast<std::uint32_t>(factor);
                std::uint32_t x1 = x0 + static_cast<std::uint32_t>(factor);
                if (x1 > w) x1 = w;
                for (std::uint32_t x = x0; x < x1; ++x) {
                    acc[ox * 4 + 0] += convRow[x * 4 + 0];
                    acc[ox * 4 + 1] += convRow[x * 4 + 1];
                    acc[ox * 4 + 2] += convRow[x * 4 + 2];
                    acc[ox * 4 + 3] += convRow[x * 4 + 3];
                }
            }
        }
        for (std::uint32_t ox = 0; ox < wOut; ++ox) {
            const std::uint32_t x0 = ox * static_cast<std::uint32_t>(factor);
            std::uint32_t x1 = x0 + static_cast<std::uint32_t>(factor);
            if (x1 > w) x1 = w;
            const std::uint32_t count = (x1 - x0) * (y1 - y0);
            out[ox * 4 + 0] = static_cast<std::uint8_t>((acc[ox * 4 + 0] + count / 2) / count);
            out[ox * 4 + 1] = static_cast<std::uint8_t>((acc[ox * 4 + 1] + count / 2) / count);
            out[ox * 4 + 2] = static_cast<std::uint8_t>((acc[ox * 4 + 2] + count / 2) / count);
            out[ox * 4 + 3] = static_cast<std::uint8_t>((acc[ox * 4 + 3] + count / 2) / count);
        }
        if (!sink(oy, out.data())) return false;
    }
    return true;
}

}  // namespace

bool tiffDecodeRgba16(const std::vector<std::uint8_t>& data,
                      std::uint32_t& width, std::uint32_t& height,
                      std::vector<std::uint16_t>& rgba, int& dpi) {
    if (data.empty()) return false;
    TIFF* tif = openForRead(data);
    if (!tif) return false;

    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    float xres = 0.0f;
    TIFFGetFieldDefaulted(tif, TIFFTAG_XRESOLUTION, &xres);
    if (w == 0 || h == 0 || static_cast<std::uint64_t>(w) * h > (1ull << 30)) {
        TIFFClose(tif);
        return false;
    }
    std::uint16_t photo = PHOTOMETRIC_RGB;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photo);
    if (photo == PHOTOMETRIC_SEPARATED) {
        // libtiff's RGBA interface has no dependable CMYK path; decode the
        // ink rows through our own (profile-aware) scanline converter and
        // widen its 8-bit output the same way the RGBA path does.
        dpi = xres >= 1.0f ? static_cast<int>(xres + 0.5f) : 0;
        rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        bool ok = false;
        std::function<bool(std::uint32_t, const std::uint8_t*)> sink =
            [&](std::uint32_t y, const std::uint8_t* row) {
                std::uint16_t* dst =
                    rgba.data() + static_cast<std::size_t>(y) * w * 4;
                for (std::uint32_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = static_cast<std::uint16_t>(row[x * 4 + 0]) * 257u;
                    dst[x * 4 + 1] = static_cast<std::uint16_t>(row[x * 4 + 1]) * 257u;
                    dst[x * 4 + 2] = static_cast<std::uint16_t>(row[x * 4 + 2]) * 257u;
                    dst[x * 4 + 3] = static_cast<std::uint16_t>(row[x * 4 + 3]) * 257u;
                }
                return true;
            };
        ok = decodeBandedCore(tif, w, h, sink);
        TIFFClose(tif);
        if (!ok) return false;
        width = w;
        height = h;
        return true;
    }
    std::vector<std::uint32_t> raster(static_cast<std::size_t>(w) * h);
    // Convert any orientation to top-left raster order, like QImageReader does.
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT)) {
        TIFFClose(tif);
        return false;
    }
    TIFFClose(tif);

    dpi = xres >= 1.0f ? static_cast<int>(xres + 0.5f) : 0;
    const std::uint64_t n = static_cast<std::uint64_t>(w) * h;
    rgba.assign(static_cast<std::size_t>(n) * 4, 0);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint32_t v = raster[i];  // RGBA in host byte order
        rgba[i * 4 + 0] = static_cast<std::uint16_t>(v & 0xff) * 257u;
        rgba[i * 4 + 1] = static_cast<std::uint16_t>((v >> 8) & 0xff) * 257u;
        rgba[i * 4 + 2] = static_cast<std::uint16_t>((v >> 16) & 0xff) * 257u;
        rgba[i * 4 + 3] = static_cast<std::uint16_t>((v >> 24) & 0xff) * 257u;
    }
    width = w;
    height = h;
    return true;
}

bool tiffEncodeExport(std::uint32_t width, std::uint32_t height, int dpi,
                      const std::uint16_t* rgba, const TiffEncodeOptions& opt,
                      std::vector<std::uint8_t>& out) {
    const std::uint64_t n = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || n > (1ull << 30) || !rgba) return false;
    const int bits = (opt.bits == 16) ? 16 : 8;
    const bool gray = opt.grayscale;
    const bool cmyk = opt.cmyk && !gray;
    // CMYK separates the appearance once up front; the scanline loops then
    // just slice ink planes (ink coverage, TIFF convention) out of it.
    std::vector<std::uint16_t> planed;
    if (cmyk)
        pittore::color::separateRgba16(rgba, static_cast<std::size_t>(n),
                                        opt.icc.data(), opt.icc.size(),
                                        opt.withAlpha, planed);
    const std::uint16_t* base = cmyk ? planed.data() : rgba;
    const int stride = cmyk ? (opt.withAlpha ? 5 : 4) : 4;
    // Alpha rides along for 8-bit RGB(A) only; 16-bit alpha stays planar with
    // RGB (RGBA64) while gray+alpha is pre-flattened by the caller. CMYK
    // keeps alpha at both depths (5th sample, unassociated).
    const bool alpha = !cmyk && opt.withAlpha && !gray && bits == 8;
    const int samples = cmyk ? (opt.withAlpha ? 5 : 4)
                             : (gray ? 1 : (alpha || bits == 16 ? 4 : 3));
    const bool alpha16 = !cmyk && !gray && bits == 16;

    MemState s;
    s.owned = false;
    TIFF* tif = openForWrite(&s);
    if (!tif) return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                 gray ? PHOTOMETRIC_MINISBLACK
                 : cmyk ? PHOTOMETRIC_SEPARATED
                        : PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,
                 opt.compression == 2   ? COMPRESSION_DEFLATE
                 : opt.compression == 0 ? COMPRESSION_NONE
                                        : COMPRESSION_LZW);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
    if (alpha || alpha16 || (cmyk && opt.withAlpha)) {
        const std::uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
    }
    if (!opt.icc.empty())
        TIFFSetField(tif, TIFFTAG_ICCPROFILE,
                     static_cast<std::uint32_t>(opt.icc.size()),
                     opt.icc.data());
    if (dpi > 0) {
        TIFFSetField(tif, TIFFTAG_XRESOLUTION, static_cast<float>(dpi));
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, static_cast<float>(dpi));
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    }

    bool ok = true;
    if (bits == 8) {
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * samples);
        for (std::uint32_t y = 0; y < height && ok; ++y) {
            const std::uint16_t* src = base + (static_cast<std::uint64_t>(y) * width) * stride;
            for (std::uint32_t x = 0; x < width; ++x) {
                if (cmyk) {
                    const int cnt = samples;
                    for (int i = 0; i < cnt; ++i)
                        row[x * cnt + i] =
                            static_cast<std::uint8_t>(src[x * stride + i] >> 8);
                    continue;
                }
                const std::uint8_t r =
                    static_cast<std::uint8_t>(src[x * 4 + 0] >> 8);
                const std::uint8_t g =
                    static_cast<std::uint8_t>(src[x * 4 + 1] >> 8);
                const std::uint8_t b =
                    static_cast<std::uint8_t>(src[x * 4 + 2] >> 8);
                const std::uint8_t a =
                    static_cast<std::uint8_t>(src[x * 4 + 3] >> 8);
                if (gray) {
                    // Rec.709 luma, matching the dialog's grayscale preview.
                    row[x] = static_cast<std::uint8_t>(
                        (13933u * r + 46871u * g + 4732u * b + 32768u) >> 16);
                } else if (alpha) {
                    row[x * 4 + 0] = r;
                    row[x * 4 + 1] = g;
                    row[x * 4 + 2] = b;
                    row[x * 4 + 3] = a;
                } else {
                    row[x * 3 + 0] = r;
                    row[x * 3 + 1] = g;
                    row[x * 3 + 2] = b;
                }
            }
            if (TIFFWriteScanline(tif, row.data(), static_cast<std::uint32_t>(y),
                                  0) < 0)
                ok = false;
        }
    } else {
        std::vector<std::uint16_t> row(static_cast<std::size_t>(width) *
                                       samples);
        for (std::uint32_t y = 0; y < height && ok; ++y) {
            const std::uint16_t* src = base + (static_cast<std::uint64_t>(y) * width) * stride;
            for (std::uint32_t x = 0; x < width; ++x) {
                if (cmyk) {
                    const int cnt = samples;
                    for (int i = 0; i < cnt; ++i)
                        row[x * cnt + i] = src[x * stride + i];
                    continue;
                }
                const std::uint16_t r = src[x * 4 + 0];
                const std::uint16_t g = src[x * 4 + 1];
                const std::uint16_t b = src[x * 4 + 2];
                const std::uint16_t a = src[x * 4 + 3];
                if (gray) {
                    row[x] = static_cast<std::uint16_t>(
                        (13933u * r + 46871u * g + 4732u * b + 32768u) >> 16);
                } else {
                    row[x * 4 + 0] = r;
                    row[x * 4 + 1] = g;
                    row[x * 4 + 2] = b;
                    row[x * 4 + 3] = alpha16 ? a : 0xFFFF;
                }
            }
            if (TIFFWriteScanline(tif, row.data(), static_cast<std::uint32_t>(y),
                                  0) < 0)
                ok = false;
        }
    }
    TIFFClose(tif);  // finalizes the directory into the caller-owned buffer
    if (!ok) return false;
    out = std::move(s.bytes);
    return true;
}

bool tiffEncodeRgba16(std::uint32_t width, std::uint32_t height, int dpi,
                      const std::uint16_t* rgba,
                      std::vector<std::uint8_t>& out) {
    const std::uint64_t n = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || n > (1ull << 30) || !rgba) return false;

    MemState s;
    s.owned = false;
    TIFF* tif = openForWrite(&s);
    if (!tif) return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
    const std::uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
    if (dpi > 0) {
        TIFFSetField(tif, TIFFTAG_XRESOLUTION, static_cast<float>(dpi));
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, static_cast<float>(dpi));
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    }

    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
    bool ok = true;
    for (std::uint32_t y = 0; y < height && ok; ++y) {
        const std::uint16_t* src = rgba + (static_cast<std::uint64_t>(y) * width) * 4;
        for (std::uint32_t x = 0; x < width; ++x) {
            row[x * 4 + 0] = static_cast<std::uint8_t>(src[x * 4 + 0] >> 8);
            row[x * 4 + 1] = static_cast<std::uint8_t>(src[x * 4 + 1] >> 8);
            row[x * 4 + 2] = static_cast<std::uint8_t>(src[x * 4 + 2] >> 8);
            row[x * 4 + 3] = static_cast<std::uint8_t>(src[x * 4 + 3] >> 8);
        }
        if (TIFFWriteScanline(tif, row.data(), static_cast<std::uint32_t>(y), 0) < 0)
            ok = false;
    }
    TIFFClose(tif);  // finalizes the directory into the caller-owned buffer
    if (!ok) return false;
    out = std::move(s.bytes);
    return true;
}

bool tiffProbeFile(const char* path, std::uint32_t& width,
                   std::uint32_t& height, int& dpi) {
    width = 0;
    height = 0;
    dpi = 0;
    if (!path || !*path) return false;
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return false;
    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    float xres = 0.0f;
    TIFFGetFieldDefaulted(tif, TIFFTAG_XRESOLUTION, &xres);
    TIFFClose(tif);
    if (w == 0 || h == 0) return false;
    width = w;
    height = h;
    dpi = xres >= 1.0f ? static_cast<int>(xres + 0.5f) : 0;
    return true;
}

bool tiffDecodeFileBanded(const char* path,
                          std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    if (!path || !*path) return false;
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return false;
    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    bool ok = false;
    if (w != 0 && h != 0 &&
        static_cast<std::uint64_t>(w) * h <= (1ull << 30)) {
        try {
            ok = decodeBandedCore(tif, w, h, sink);
        } catch (...) {
            ok = false;
        }
    }
    TIFFClose(tif);
    return ok;
}

bool tiffDecodeBufferBanded(const std::vector<std::uint8_t>& data,
                            std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    if (data.empty()) return false;
    TIFF* tif = openForRead(data);
    if (!tif) return false;
    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    bool ok = false;
    if (w != 0 && h != 0 &&
        static_cast<std::uint64_t>(w) * h <= (1ull << 30)) {
        try {
            ok = decodeBandedCore(tif, w, h, sink);
        } catch (...) {
            ok = false;
        }
    }
    TIFFClose(tif);
    return ok;
}

namespace {

bool decodeSubsampledOnOpenTiff(TIFF* tif, int factor, std::uint32_t& wOut,
                                std::uint32_t& hOut, int& dpi,
                                std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    wOut = 0;
    hOut = 0;
    dpi = 0;
    if (factor < 1) return false;
    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    float xres = 0.0f;
    TIFFGetFieldDefaulted(tif, TIFFTAG_XRESOLUTION, &xres);
    if (w == 0 || h == 0 ||
        static_cast<std::uint64_t>(w) * h > (1ull << 30))
        return false;
    const std::uint32_t f = static_cast<std::uint32_t>(factor);
    const std::uint32_t ow = (w + f - 1) / f;
    const std::uint32_t oh = (h + f - 1) / f;
    if (ow == 0 || oh == 0) return false;
    bool ok = false;
    try {
        if (factor == 1)
            ok = decodeBandedCore(tif, w, h, sink);
        else
            ok = decodeSubsampledCore(tif, w, h, factor, ow, oh, sink);
    } catch (...) {
        ok = false;
    }
    if (!ok) return false;
    wOut = ow;
    hOut = oh;
    dpi = xres >= 1.0f ? static_cast<int>(xres + 0.5f) : 0;
    return true;
}

}  // namespace

bool tiffDecodeFileSubsampled(const char* path, int factor,
                              std::uint32_t& wOut, std::uint32_t& hOut,
                              int& dpi,
                              std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    wOut = 0;
    hOut = 0;
    dpi = 0;
    if (!path || !*path || factor < 1) return false;
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return false;
    const bool ok = decodeSubsampledOnOpenTiff(tif, factor, wOut, hOut, dpi, sink);
    TIFFClose(tif);
    return ok;
}

bool tiffDecodeBufferSubsampled(const std::vector<std::uint8_t>& data,
                                int factor, std::uint32_t& wOut,
                                std::uint32_t& hOut, int& dpi,
                                std::function<bool(std::uint32_t, const std::uint8_t*)>& sink) {
    wOut = 0;
    hOut = 0;
    dpi = 0;
    if (data.empty() || factor < 1) return false;
    TIFF* tif = openForRead(data);
    if (!tif) return false;
    const bool ok = decodeSubsampledOnOpenTiff(tif, factor, wOut, hOut, dpi, sink);
    TIFFClose(tif);
    return ok;
}

namespace {

std::string iccFromOpenTiff(TIFF* tif) {
    if (!tif) return "";
    std::uint32_t size = 0;
    void* raw = nullptr;
    if (!TIFFGetField(tif, TIFFTAG_ICCPROFILE, &size, &raw) || !raw ||
        size == 0 || size > (1u << 24))
        return "";
    return iccProfileDescription(static_cast<const std::uint8_t*>(raw), size);
}

}  // namespace

std::string tiffIccProfileData(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return "";
    TIFF* tif = openForRead(data);
    if (!tif) return "";
    const std::string out = iccFromOpenTiff(tif);
    TIFFClose(tif);
    return out;
}

bool tiffDecodeCmykFile(const char* path, std::uint32_t& width,
                        std::uint32_t& height,
                        std::vector<std::uint16_t>& cmyk,
                        std::vector<std::uint8_t>* icc, int* bitDepth) {
    if (!path || !*path) return false;
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return false;
    std::uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    BandedFormat fmt;
    if (w == 0 || h == 0 || !readBandedFormat(tif, fmt, true, true) || !fmt.cmyk ||
        static_cast<std::uint64_t>(w) * h > (1ull << 30)) {
        TIFFClose(tif);
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<std::uint16_t> decoded(n * 4);
    const std::size_t sampleBytes = fmt.bits / 8;
    const bool separate = fmt.planar == PLANARCONFIG_SEPARATE;
    const bool tiled = TIFFIsTiled(tif) != 0;
    auto storeSample = [&](std::uint32_t x, std::uint32_t y, int c,
                           std::uint16_t value) {
        std::uint32_t dx = x, dy = y;
        switch (fmt.orientation) {
            case ORIENTATION_TOPRIGHT: dx = w - 1 - x; break;
            case ORIENTATION_BOTRIGHT:
                dx = w - 1 - x; dy = h - 1 - y; break;
            case ORIENTATION_BOTLEFT: dy = h - 1 - y; break;
            case ORIENTATION_LEFTTOP: dx = y; dy = x; break;
            case ORIENTATION_RIGHTTOP: dx = h - 1 - y; dy = x; break;
            case ORIENTATION_RIGHTBOT:
                dx = h - 1 - y; dy = w - 1 - x; break;
            case ORIENTATION_LEFTBOT: dx = y; dy = w - 1 - x; break;
            default: break;
        }
        const std::uint32_t outW =
            fmt.orientation >= ORIENTATION_LEFTTOP ? h : w;
        decoded[(static_cast<std::size_t>(dy) * outW + dx) * 4 + c] = value;
    };
    auto sampleAt = [&](const std::uint8_t* row,
                        std::size_t sample) -> std::uint16_t {
        if (fmt.bits == 8)
            return static_cast<std::uint16_t>(row[sample] * 257u);
        return reinterpret_cast<const std::uint16_t*>(row)[sample];
    };
    if (tiled) {
        std::uint32_t tw = 0, th = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        const tmsize_t tileBytes = TIFFTileSize(tif);
        const tmsize_t tileRowBytes = TIFFTileRowSize(tif);
        if (!tw || !th || tileBytes <= 0 || tileRowBytes <= 0) {
            TIFFClose(tif);
            return false;
        }
        std::vector<std::uint8_t> tile(static_cast<std::size_t>(tileBytes));
        std::vector<std::uint8_t> plane;
        if (separate) plane.resize(static_cast<std::size_t>(tileBytes));
        for (std::uint32_t ty = 0; ty < h; ty += th) {
            for (std::uint32_t tx = 0; tx < w; tx += tw) {
                for (std::uint16_t c = 0; c < (separate ? 4 : 1); ++c) {
                    std::uint8_t* src = separate ? plane.data() : tile.data();
                    if (TIFFReadTile(tif, src, tx, ty, 0, c) < 0) {
                        TIFFClose(tif);
                        return false;
                    }
                    const std::uint32_t rows = std::min(th, h - ty);
                    const std::uint32_t cols = std::min(tw, w - tx);
                    for (std::uint32_t iy = 0; iy < rows; ++iy) {
                        const std::uint8_t* row = src +
                            static_cast<std::size_t>(iy) * tileRowBytes;
                        for (std::uint32_t ix = 0; ix < cols; ++ix) {
                            const std::size_t base = static_cast<std::size_t>(ix) *
                                (separate ? 1 : fmt.samples);
                            const std::uint32_t x = tx + ix, y = ty + iy;
                            if (separate) {
                                storeSample(x, y, c, sampleAt(row, base));
                            } else {
                                for (int ink = 0; ink < 4; ++ink)
                                    storeSample(x, y, ink,
                                                sampleAt(row, base + ink));
                            }
                        }
                    }
                }
            }
        }
    } else {
        const std::size_t rowBytes = static_cast<std::size_t>(w) *
            (separate ? 1 : fmt.samples) * sampleBytes;
        std::vector<std::uint8_t> row(rowBytes);
        std::vector<std::uint8_t> plane;
        if (separate) plane.resize(rowBytes);
        for (std::uint32_t y = 0; y < h; ++y) {
            if (!separate && TIFFReadScanline(tif, row.data(), y, 0) < 0) {
                TIFFClose(tif);
                return false;
            }
            if (separate) {
                for (std::uint16_t c = 0; c < 4; ++c) {
                    if (TIFFReadScanline(tif, plane.data(), y, c) < 0) {
                        TIFFClose(tif);
                        return false;
                    }
                    for (std::uint32_t x = 0; x < w; ++x)
                        storeSample(x, y, c, sampleAt(plane.data(), x));
                }
            } else {
                for (std::uint32_t x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c)
                        storeSample(x, y, c,
                                    sampleAt(row.data(), x * fmt.samples + c));
            }
        }
    }
    if (icc) *icc = iccOfOpenTiff(tif);
    if (bitDepth) *bitDepth = fmt.bits;
    TIFFClose(tif);
    width = fmt.orientation >= ORIENTATION_LEFTTOP ? h : w;
    height = fmt.orientation >= ORIENTATION_LEFTTOP ? w : h;
    cmyk = std::move(decoded);
    return true;
}

std::string tiffIccProfileName(const char* path) {
    if (!path || !*path) return "";
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return "";
    const std::string out = iccFromOpenTiff(tif);
    TIFFClose(tif);
    return out;
}

}  // namespace pittore::io
