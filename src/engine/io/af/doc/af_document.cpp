#include "engine/io/af.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <zlib.h>
#ifdef PITTORE_AF
// ZSTD_decompressBound lives in zstd's advanced API; the header only declares
// it when this macro is set first (see zstd.h). The symbol itself is exported
// by the shared library, so no static-link constraint is introduced.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

#include "engine/io/af/shared/af_preview.h"
#include "engine/io/af/png/af_png.h"

namespace pittore::io {

namespace {

std::uint64_t u64le(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// End of the last v12 directory block. The saved flattened preview sits after
// it; a PNG inside the data area is an embedded source stored in the clear
// (a placed logo sheet can be larger than the document preview), so the two
// regions must not compete by area.
std::uint64_t lastDirectoryEnd(const std::vector<std::uint8_t>& data) {
    if (data.size() < 24) return 0;
    std::uint64_t at = u64le(data.data() + 16);
    std::uint64_t end = 0;
    for (int guard = 0; at != 0 && guard < 10000; ++guard) {
        if (at + 24 > data.size()) break;
        if (std::memcmp(data.data() + at, "#FT4", 4) != 0) break;
        end = std::max(end, u64le(data.data() + at + 20));
        const std::uint64_t next = u64le(data.data() + at + 4);
        if (next == at) break;
        at = next;
    }
    return end;
}

}  // namespace


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool afProbe(const std::vector<std::uint8_t>& data) {
    using namespace af_preview;
    if (data.size() < 16) return false;
    if (data[0] != 0x00 || data[1] != 0xff || data[2] != 0x4b || data[3] != 0x41)
        return false;
    return hasBytes(data, reinterpret_cast<const std::uint8_t*>("nsrP"), 4, 8) &&
           hasBytes(data, reinterpret_cast<const std::uint8_t*>("#Inf"), 4, 12);
}


std::optional<AfDocument> afDecodeDocument(const std::vector<std::uint8_t>& data,
                                           std::string* error) {
    using namespace af_preview;
    const auto fail = [&](const char* why) -> std::optional<AfDocument> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (data.size() < 16) return fail("file too small to be an Affinity archive");
    if (!afProbe(data)) return fail("not an Affinity archive (bad header magic)");

    AfDocument doc;

    // 1. Embedded cleartext PNG previews (zlib-only).
    std::size_t at = 0;
    while (at + 8 <= data.size()) {
        std::size_t found = std::string::npos;
        for (std::size_t k = at; k + 8 <= data.size(); ++k) {
            if (std::memcmp(data.data() + k, kPngMagic, 8) == 0) {
                found = k;
                break;
            }
        }
        if (found == std::string::npos) break;
        PngInfo png;
        std::size_t end = 0;
        std::string why;
        if (!parsePng(data, found, png, &end, &why)) {
            at = found + 8;  // payload looked like a PNG but is not one
            continue;
        }
        AfPreview pv;
        if (!decodePngToRgba16(png, pv.rgba, &why)) {
            at = found + 8;
            continue;
        }
        pv.width = png.width;
        pv.height = png.height;
        pv.offset = found;
        pv.size = end - found;
        pv.dpi = png.dpmX > 0 ? int(std::llround(double(png.dpmX) * 0.0254)) : 0;
        doc.previews.push_back(std::move(pv));
        at = found + 8;
    }
    if (!doc.previews.empty()) {
        // Primary = the saved flattened preview, which the format writes after
        // the directory block. Clear-text PNGs inside the data area are placed
        // sources, not document renders. Among tail previews the largest wins:
        // some files carry a small thumbnail beside a larger render.
        const std::uint64_t tailStart = lastDirectoryEnd(data);
        std::size_t best = doc.previews.size();
        std::uint64_t bestArea = 0;
        for (std::size_t i = 0; i < doc.previews.size(); ++i) {
            if (tailStart != 0 && doc.previews[i].offset < tailStart) continue;
            const std::uint64_t area =
                std::uint64_t(doc.previews[i].width) * doc.previews[i].height;
            if (best == doc.previews.size() || area > bestArea) {
                best = i;
                bestArea = area;
            }
        }
        if (best == doc.previews.size()) {
            // Older archive layouts keep no preview region after the directory;
            // fall back to the largest embedded render.
            best = 0;
            for (std::size_t i = 1; i < doc.previews.size(); ++i) {
                const std::uint64_t area =
                    std::uint64_t(doc.previews[i].width) * doc.previews[i].height;
                const std::uint64_t chosen =
                    std::uint64_t(doc.previews[best].width) * doc.previews[best].height;
                if (area > chosen) best = i;
            }
        }
        doc.primary = best;
    }

    // 2. Tail manifest (title / client version / page count).
    doc.title = jsonTailString(data, "title");
    doc.clientVersion = jsonTailString(data, "clientVersion");
    doc.pageCount = jsonTailU32(data, "pageCount");

    // 3. Frame walk + object-tree scan (needs libzstd).
    walkFrames(data, doc);
    if (!doc.appVersion.empty() && doc.revision != 0)
        doc.appVersion += "." + std::to_string(doc.revision);

    return doc;
}

std::optional<AfImage> afDecode(const std::vector<std::uint8_t>& data, std::string* error) {
    using namespace af_preview;
    auto doc = afDecodeDocument(data, error);
    if (!doc) return std::nullopt;
    if (doc->previews.empty()) {
        if (error) *error = "no embedded document preview found in the Affinity archive";
        return std::nullopt;
    }
    const AfPreview& pv = doc->previews[doc->primary];
    AfImage img;
    img.width = pv.width;
    img.height = pv.height;
    img.depth = 8;
    img.colorMode = 3;
    img.dpi = pv.dpi;
    img.rgba = pv.rgba;
    return img;
}

bool afDecodePngRgba16(const std::uint8_t* data, std::size_t size,
                       std::uint32_t& width, std::uint32_t& height,
                       std::vector<std::uint16_t>& rgba, std::string* error) {
    using namespace af_preview;
    width = 0;
    height = 0;
    rgba.clear();
    if (!data || size < 8) {
        if (error) *error = "PNG: truncated buffer";
        return false;
    }
    // parsePng/decodePngToRgba16 live in this translation unit's anonymous
    // namespace; they take the whole buffer with `start` at the signature.
    std::vector<std::uint8_t> bytes(data, data + size);
    PngInfo png;
    std::size_t end = 0;
    std::string why;
    if (!parsePng(bytes, 0, png, &end, &why)) {
        if (error) *error = why;
        return false;
    }
    if (!decodePngToRgba16(png, rgba, &why)) {
        if (error) *error = why;
        return false;
    }
    width = png.width;
    height = png.height;
    return true;
}

}  // namespace pittore::io
