#include "engine/io/af_layers.h"
#include "engine/io/af.h"
#include "engine/render/layer_style.h"
#include "engine/text/text_engine.h"
#include "engine/vector/vector_shape.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>
#ifdef PITTORE_AF
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif
#ifdef PITTORE_WEBP
#include "engine/io/webp.h"
#endif
#ifdef PITTORE_TIFF
#include "engine/io/tiff.h"
#endif
#ifdef PITTORE_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#endif
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/geom/af_geom.h"
#include "engine/io/af_layers/geom/af_homography.h"
#include "engine/io/af_layers/graph/af_graph.h"
#include "engine/io/af_layers/graph/af_graph_parser.h"
#include "engine/io/af_layers/image/af_image.h"
#include "engine/io/af_layers/filter/af_blur.h"
#include "engine/io/af_layers/filter/af_distort.h"
#include "engine/io/af_layers/filter/af_fx.h"
#include "engine/io/af_layers/filter/af_live.h"
#include "engine/io/af_layers/vector/af_shapes.h"
#include "engine/io/af_layers/walker/af_walker.h"

namespace pittore::io {
namespace af_detail {


std::optional<Format> formatOf(std::uint16_t id) {
    switch (id) {
        case 0: return Format{1, 4, FK::Rgba};
        case 1: return Format{2, 4, FK::Rgba};
        case 2: return Format{1, 2, FK::Gray};
        case 3: return Format{2, 2, FK::Gray};
        case 4: return Format{1, 5, FK::Cmyk};
        case 5: return Format{2, 4, FK::Lab};
        case 6: return Format{1, 1, FK::Mask};
        case 9: return Format{4, 4, FK::Rgba};
        default: return std::nullopt;
    }
}


void labToSrgb(float l, float a, float b, float& r, float& g, float& bl) {
    const float fy = (l + 16.0f) / 116.0f;
    const float fx = fy + a / 500.0f;
    const float fz = fy - b / 200.0f;
    auto finv = [](float t) {
        return t > 6.0f / 29.0f ? t * t * t
                                : 3.0f * (6.0f / 29.0f) * (6.0f / 29.0f) * (t - 4.0f / 29.0f);
    };
    const float xn = 0.9642f, yn = 1.0f, zn = 0.8251f;
    const float x = xn * finv(fx), y = yn * finv(fy), z = zn * finv(fz);
    const float rr = 3.133856f * x - 1.616867f * y - 0.490615f * z;
    const float gg = -0.978768f * x + 1.916141f * y + 0.033454f * z;
    const float bb = 0.071945f * x - 0.228991f * y + 1.405243f * z;
    auto enc = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    };
    r = enc(rr);
    g = enc(gg);
    bl = enc(bb);
}

#ifdef PITTORE_JPEG

void jpegErrorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(err->jump, 1);
}

bool jpegDecode(const std::vector<std::uint8_t>& b, Bitmap& out, std::string& why) {
    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        why = "JPEG: decode failed";
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, b.data(), static_cast<unsigned long>(b.size()));
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    out.w = cinfo.output_width;
    out.h = cinfo.output_height;
    out.px.assign(std::size_t(out.w) * out.h * 4, 255);
    std::vector<std::uint8_t> row(std::size_t(cinfo.output_width) * cinfo.output_components);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW p = row.data();
        jpeg_read_scanlines(&cinfo, &p, 1);
        const std::size_t y = cinfo.output_scanline - 1;
        for (std::size_t x = 0; x < out.w; ++x) {
            out.px[(y * out.w + x) * 4 + 0] = row[x * 3 + 0];
            out.px[(y * out.w + x) * 4 + 1] = row[x * 3 + 1];
            out.px[(y * out.w + x) * 4 + 2] = row[x * 3 + 2];
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
#endif


bool decodeImageBytes(const std::vector<std::uint8_t>& b, Bitmap& out, std::string& why) {
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint16_t> rgba;
        std::string err;
        if (!afDecodePngRgba16(b.data(), b.size(), w, h, rgba, &err)) {
            why = "PNG: " + err;
            return false;
        }
        out.w = w;
        out.h = h;
        out.px.resize(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < out.px.size(); ++i)
            out.px[i] = static_cast<std::uint8_t>(rgba[i] >> 8);
        return true;
    }
    if (b.size() >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
#ifdef PITTORE_JPEG
        return jpegDecode(b, out, why);
#else
        why = "embedded JPEG source needs libjpeg (PITTORE_JPEG)";
        return false;
#endif
    }
    if (b.size() >= 12 && b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
        b[8] == 'W' && b[9] == 'E' && b[10] == 'B' && b[11] == 'P') {
#ifdef PITTORE_WEBP
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint16_t> rgba;
        if (!webpDecodeRgba16(b, w, h, rgba) || w == 0 || h == 0) {
            why = "WebP: decode failed";
            return false;
        }
        out.w = w;
        out.h = h;
        out.px.resize(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < out.px.size(); ++i)
            out.px[i] = static_cast<std::uint8_t>(rgba[i] >> 8);
        return true;
#else
        why = "embedded WebP source needs libwebp (PITTORE_WEBP)";
        return false;
#endif
    }
    // Placed originals are often plain TIFFs (in-house scans, older photos).
    const bool tiffLittle = b.size() >= 4 && b[0] == 'I' && b[1] == 'I' &&
                            b[2] == 0x2a && b[3] == 0x00;
    const bool tiffBig = b.size() >= 4 && b[0] == 'M' && b[1] == 'M' &&
                         b[2] == 0x00 && b[3] == 0x2a;
    if (tiffLittle || tiffBig) {
#ifdef PITTORE_TIFF
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint16_t> rgba;
        int dpi = 0;
        if (!tiffDecodeRgba16(b, w, h, rgba, dpi) || w == 0 || h == 0) {
            why = "TIFF: decode failed";
            return false;
        }
        out.w = w;
        out.h = h;
        out.px.resize(std::size_t(w) * h * 4);
        for (std::size_t i = 0; i < out.px.size(); ++i)
            out.px[i] = static_cast<std::uint8_t>(rgba[i] >> 8);
        return true;
#else
        why = "embedded TIFF source needs libtiff (PITTORE_TIFF)";
        return false;
#endif
    }
    why = "unrecognised embedded source image";
    return false;
}


std::string pathBasename(const std::string& p) {
    const std::size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}


bool readFileBytes(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    if (len < 0 || static_cast<std::uint64_t>(len) > kMaxEntrySize) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(len));
    if (len > 0) f.read(reinterpret_cast<char*>(out.data()), len);
    return f.good() || f.eof();
}


// The .af format records a linked image's path as the writing machine saw it.
// Windows documents (Plat == "Win32") use backslashes and drive letters; under
// Wine, Z: is the filesystem root, so "Z:\\home\\me\\pic.webp" is
// "/home/me/pic.webp". Other drive letters usually live under /mnt. The raw
// path, then the basename beside the .af itself, are the fallbacks.
std::optional<std::string> resolveLinkedPath(const std::string& raw,
                                             const std::string& baseDir) {
    if (raw.empty()) return std::nullopt;
    std::string unixPath = raw;
    std::replace(unixPath.begin(), unixPath.end(), '\\', '/');
    std::vector<std::string> candidates;
    if (unixPath[0] == '/') {
        candidates.push_back(unixPath);
    } else if (unixPath.size() >= 2 && unixPath[1] == ':') {
        const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(unixPath[0])));
        const std::string rest = unixPath.substr(2);
        const std::string tail = (!rest.empty() && rest[0] == '/') ? rest : "/" + rest;
        if (drive == 'z') candidates.push_back(tail);   // Wine's Z: is /
        candidates.push_back("/mnt/" + std::string(1, drive) + tail);
        candidates.push_back(std::string(1, drive) + ":" + tail);
    } else {
        candidates.push_back(unixPath);
    }
    const std::string base = pathBasename(unixPath);
    if (!base.empty() && !baseDir.empty()) candidates.push_back(baseDir + "/" + base);
    for (const std::string& cand : candidates) {
        std::vector<std::uint8_t> bytes;
        if (readFileBytes(cand, bytes) && !bytes.empty()) return cand;
    }
    return std::nullopt;
}


// The external path of a linked source entry: the graphics "Filn" field, or a
// "Path" string as a fallback. Both are plain strings on the entry's graph.
std::string findLinkPath(const Graph& g) {
    for (const auto& up : g.nodes)
        if (const Value* v = g.field(up.get(), "Filn"))
            if (v->k == Value::K::Str && !v->s.empty()) return v->s;
    for (const auto& up : g.nodes)
        if (const Value* v = g.field(up.get(), "Path"))
            if (v->k == Value::K::Str && !v->s.empty()) return v->s;
    return {};
}


std::vector<std::uint8_t> allStatuses(const Graph& g, const Node* bitm) {
    std::vector<std::uint8_t> out;
    for (const char* name : {"Sta1", "Sta2", "Sta3", "Sta4", "Sta5"}) {
        const Value* v = g.field(bitm, name);
        if (v && v->k == Value::K::Array)
            for (const Value& item : v->arr)
                if (item.k == Value::K::U8) out.push_back(static_cast<std::uint8_t>(item.u));
    }
    return out;
}


bool bitmapHasContent(const Graph& g, const Node* bitm) {
    for (std::uint8_t s : allStatuses(g, bitm))
        if (s > 1) return true;
    return false;
}

}  // namespace af_detail
}  // namespace pittore::io
