#include "engine/io/project.h"

#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include <zlib.h>

namespace pittore::io {
namespace {

// Sanity cap on total pixel count (mirrors the PSD codec's ~268 MP limit).
constexpr std::uint32_t kMaxPixels = 268435456u;

std::nullopt_t fail(std::string* error, const char* msg) {
    if (error) *error = msg;
    return std::nullopt;
}

// ── writers (big-endian) ────────────────────────────────────────────────────
void putBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    putU16(out, static_cast<std::uint16_t>(v >> 16));
    putU16(out, static_cast<std::uint16_t>(v & 0xffff));
}

void putF64(std::vector<std::uint8_t>& out, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    putU16(out, static_cast<std::uint16_t>(bits >> 48));
    putU16(out, static_cast<std::uint16_t>((bits >> 32) & 0xffff));
    putU16(out, static_cast<std::uint16_t>((bits >> 16) & 0xffff));
    putU16(out, static_cast<std::uint16_t>(bits & 0xffff));
}

void putStr16(std::vector<std::uint8_t>& out, const std::string& s,
              std::size_t maxLen) {
    const std::size_t n = s.size() > maxLen ? maxLen : s.size();
    putU16(out, static_cast<std::uint16_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// Straight RGBA16 samples → big-endian byte stream (the on-disk wording).
std::vector<std::uint8_t> samplesToBytes(const std::vector<std::uint16_t>& samples) {
    std::vector<std::uint8_t> out;
    out.reserve(samples.size() * 2);
    for (std::uint16_t s : samples) {
        out.push_back(static_cast<std::uint8_t>(s >> 8));
        out.push_back(static_cast<std::uint8_t>(s & 0xff));
    }
    return out;
}

// Big-endian byte stream → host samples.
std::vector<std::uint16_t> bytesToSamples(const std::uint8_t* p, std::size_t nBytes) {
    std::vector<std::uint16_t> out(nBytes / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::uint16_t>((p[i * 2] << 8) | p[i * 2 + 1]);
    return out;
}

std::vector<std::uint8_t> deflate(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return {};
    const uLong bound = compressBound(static_cast<uLong>(bytes.size()));
    std::vector<std::uint8_t> out(bound);
    uLongf dstLen = bound;
    const int rc = compress2(out.data(), &dstLen, bytes.data(),
                             static_cast<uLong>(bytes.size()),
                             Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return {};
    out.resize(dstLen);
    return out;
}

// ── reader ──────────────────────────────────────────────────────────────────
struct Reader {
    const std::vector<std::uint8_t>& b;
    std::size_t pos = 0;
    bool ok = true;

    explicit Reader(const std::vector<std::uint8_t>& data) : b(data) {}

    std::size_t remaining() const { return b.size() - pos; }

    bool skip(std::size_t n) {
        if (n > remaining()) {
            ok = false;
            return false;
        }
        pos += n;
        return true;
    }

    std::uint8_t u8() {
        if (!ok || pos + 1 > b.size()) {
            ok = false;
            return 0;
        }
        return b[pos++];
    }

    std::uint16_t u16() {
        if (!ok || pos + 2 > b.size()) {
            ok = false;
            return 0;
        }
        const std::uint16_t v = static_cast<std::uint16_t>((b[pos] << 8) | b[pos + 1]);
        pos += 2;
        return v;
    }

    std::uint32_t u32() {
        if (!ok || pos + 4 > b.size()) {
            ok = false;
            return 0;
        }
        const std::uint32_t v = (static_cast<std::uint32_t>(b[pos]) << 24) |
                                (static_cast<std::uint32_t>(b[pos + 1]) << 16) |
                                (static_cast<std::uint32_t>(b[pos + 2]) << 8) |
                                static_cast<std::uint32_t>(b[pos + 3]);
        pos += 4;
        return v;
    }

    double f64() {
        const std::uint32_t hi = u32();
        const std::uint32_t lo = u32();
        std::uint64_t bits = (static_cast<std::uint64_t>(hi) << 32) | lo;
        double v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }

    const std::uint8_t* take(std::size_t n) {
        if (n > remaining()) {
            ok = false;
            return nullptr;
        }
        const std::uint8_t* p = b.data() + pos;
        pos += n;
        return p;
    }

    std::string str() {
        const std::uint16_t n = u16();
        const std::uint8_t* p = take(n);
        if (!p) return {};
        return std::string(reinterpret_cast<const char*>(p), n);
    }
};

// Decompress `src` and validate the result has exactly `expected` bytes.
std::vector<std::uint8_t> inflateExpected(const std::vector<std::uint8_t>& src,
                                          std::size_t expected) {
    if (expected == 0) return {};
    std::vector<std::uint8_t> out(expected);
    uLongf dstLen = static_cast<uLongf>(expected);
    const int rc = uncompress(out.data(), &dstLen, src.data(),
                              static_cast<uLong>(src.size()));
    if (rc != Z_OK || dstLen != expected) return {};
    return out;
}

bool readPixelBlock(Reader& in, std::size_t sampleCount,
                    std::vector<std::uint16_t>* outSamples, const char* what,
                    std::string* error) {
    const std::uint8_t compId = in.u8();
    const std::uint32_t dataLen = in.u32();
    if (!in.ok || compId > 1) {
        if (error) *error = what;
        return false;
    }
    const std::uint8_t* data = in.take(dataLen);
    if (!data) {
        if (error) *error = what;
        return false;
    }
    if (compId == 0) {
        // Stored raw big-endian samples.
        if (dataLen != sampleCount * 2) {
            if (error) *error = what;
            return false;
        }
        *outSamples = bytesToSamples(data, dataLen);
        return true;
    }
    const std::vector<std::uint8_t> raw = inflateExpected(
        std::vector<std::uint8_t>(data, data + dataLen), sampleCount * 2);
    if (raw.empty()) {
        if (error) *error = what;
        return false;
    }
    *outSamples = bytesToSamples(raw.data(), raw.size());
    return true;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> projectEncode(const ProjectFileDoc& p,
                                                       std::string* error) {
    const std::uint64_t q = static_cast<std::uint64_t>(p.width) * p.height;
    if (p.width == 0 || p.height == 0 || q > kMaxPixels)
        return fail(error, "project dimensions out of range");

    // v2 when any layer needs it; v3 for widened adjustment params; v4 for
    // foreign PSD blocks; v5 for native CMYK layer samples.
    bool v2 = false;
    bool v3 = false;
    bool v4 = false;
    bool v5 = false;
    std::uint64_t psdBlockBytes = 0;
    for (const ProjectLayerFile& l : p.layers) {
        if (l.hasMask || l.clipped || l.lockTransparency || l.indent != 0 ||
            l.hasLiveFilter ||
            (l.hasAdjustment && (!l.adjustmentCurveR.empty() ||
                                 !l.adjustmentCurveG.empty() ||
                                 !l.adjustmentCurveB.empty()))) {
            v2 = true;
        }
        if (l.hasAdjustment) {
            for (int k = 8; k < 16; ++k) {
                if (l.adjustmentParams[k] != 0.0) {
                    v3 = true;
                    break;
                }
            }
        }
        if (!l.psdBlocks.empty()) {
            if (l.psdBlocks.size() > 64)
                return fail(error, "too many foreign PSD blocks");
            for (const auto& b : l.psdBlocks) {
                if (b.data.size() > (32u << 20) || b.padding.size() > 4)
                    return fail(error, "foreign PSD block too large");
                psdBlockBytes += b.data.size() + b.padding.size();
                if (psdBlockBytes > (256u << 20))
                    return fail(error, "foreign PSD blocks exceed budget");
            }
            v4 = true;
        }
        if (!l.cmyk.empty()) v5 = true;
        if (v2 && v3 && v4 && v5) break;
    }
    v4 = v4 || v5;
    v3 = v3 || v4;  // v4 is a strict superset of v3 below.
    v2 = v2 || v3;  // v3 is a strict superset of v2 below.

    std::vector<std::uint8_t> out;
    // ── header ──────────────────────────────────────────────────────────────
    putBytes(out, "INFIPROJ", 8);
    putU32(out, v5 ? 5 : (v4 ? 4 : (v3 ? 3 : (v2 ? 2 : 1))));  // version
    putU32(out, p.width);
    putU32(out, p.height);
    putU16(out, 4);                  // channels (RGBA)
    putU16(out, 16);                 // depth
    putU16(out, p.colorMode);
    putU16(out, 0);                  // reserved
    putU32(out, p.dpi);
    putU16(out, p.background == "black" ? 1 : p.background == "transparent" ? 2 : 0);
    putU16(out, 0);                  // reserved

    // Build all three sections first; the header carries their lengths up front
    // (PSD-style length markers), then payloads follow in a fixed order.
    std::vector<std::uint8_t> meta;
    putStr16(meta, p.name, 65535);
    putStr16(meta, p.colorModeName, 65535);
    if (!p.icc.empty()) {
        putU32(meta, static_cast<std::uint32_t>(p.icc.size()));
        putBytes(meta, p.icc.data(), p.icc.size());
    }

    // Parallel per-layer deflate: each layer's pixel + mask blocks are
    // independent. Encoded into side buffers here, concatenated in order
    // below, so output bytes are bit-identical to the serial loop.
    struct LayerComp {
        std::vector<std::uint8_t> pix;
        std::vector<std::uint8_t> mask;
        std::vector<std::uint8_t> cmyk;
    };
    std::vector<LayerComp> comps(p.layers.size());
    {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        const std::size_t nl = p.layers.size();
        const unsigned threads =
            static_cast<unsigned>(std::min<std::size_t>(nl == 0 ? 1 : nl, hw));
        std::vector<std::thread> workers;
        workers.reserve(threads > 0 ? threads - 1 : 0);
        auto encodeRange = [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) {
                const ProjectLayerFile& l = p.layers[i];
                LayerComp& c = comps[i];
                if (l.width != 0 && l.height != 0) {
                    const std::uint64_t px =
                        static_cast<std::uint64_t>(l.width) * l.height;
                    // Validation (dims/size match) stays in the serial loop
                    // below, which fails exactly as before; here encode
                    // whatever bytes are present (never crashes on mismatch).
                    if (px <= kMaxPixels && l.rgba.size() == px * 4)
                        c.pix = deflate(samplesToBytes(l.rgba));
                    if (v5 && px <= kMaxPixels && l.cmyk.size() == px * 4)
                        c.cmyk = deflate(samplesToBytes(l.cmyk));
                }
                if (v2 && l.hasMask) {
                    const std::uint64_t mp =
                        static_cast<std::uint64_t>(l.maskWidth) * l.maskHeight;
                    if (mp != 0 && mp <= kMaxPixels && l.mask.size() == mp)
                        c.mask = deflate(samplesToBytes(l.mask));
                }
            }
        };
        if (threads <= 1 || nl <= 1) {
            encodeRange(0, nl);
        } else {
            const std::size_t base = nl / threads;
            const std::size_t rest = nl % threads;
            std::size_t lo = 0;
            for (unsigned t = 0; t + 1 < threads; ++t) {
                const std::size_t n = base + (t < rest ? 1 : 0);
                workers.emplace_back([&, lo, n] { encodeRange(lo, lo + n); });
                lo += n;
            }
            encodeRange(lo, nl);
            for (auto& th : workers) th.join();
        }
    }

    std::vector<std::uint8_t> layers;
    putU32(layers, static_cast<std::uint32_t>(p.layers.size()));
    std::size_t li = 0;
    for (const ProjectLayerFile& l : p.layers) {
        putStr16(layers, l.name, 65535);
        layers.push_back(l.kind);
        layers.push_back(static_cast<std::uint8_t>((l.visible ? 1 : 0) |
                                                   (l.locked ? 2 : 0) |
                                                   (l.isText ? 4 : 0) |
                                                   (l.hasTextSpec ? 8 : 0) |
                                                   (l.hasTextExtras ? 16 : 0) |
                                                   (l.hasVector ? 32 : 0) |
                                                   (l.hasAdjustment ? 64 : 0) |
                                                   (l.hasToneBlend ? 128 : 0)));
        // The v1 flags byte is full, so v2 appends a second one plus the
        // nesting depth right here (fixed positions, v2 only).
        if (v2) {
            layers.push_back(static_cast<std::uint8_t>(
                (l.hasMask ? 1 : 0) | (l.clipped ? 2 : 0) |
                (l.lockTransparency ? 4 : 0) | (l.hasLiveFilter ? 8 : 0)));
            putU32(layers, static_cast<std::uint32_t>(l.indent));
        }
        putU16(layers, l.opacity);
        putU16(layers, l.fill);
        putStr16(layers, l.blend, 65535);
        putF64(layers, l.offsetX);
        putF64(layers, l.offsetY);
        putF64(layers, l.scaleX);
        putF64(layers, l.scaleY);
        putU32(layers, l.width);
        putU32(layers, l.height);
        if (l.width != 0 && l.height != 0) {
            const std::uint64_t px = static_cast<std::uint64_t>(l.width) * l.height;
            if (px > kMaxPixels)
                return fail(error, "layer dimensions out of range");
            if (l.rgba.size() != px * 4)
                return fail(error, "layer pixel buffer size mismatch");
            // Encoded in parallel above; identical bytes to inline deflate.
            // (Empty here means deflate failed, as before: zlib never emits
            // empty output for non-empty input.)
            const std::vector<std::uint8_t>& comp = comps[li].pix;
            if (comp.empty())
                return fail(error, "layer compression failed");
            layers.push_back(1);  // compression: deflate
            putU32(layers, static_cast<std::uint32_t>(comp.size()));
            layers.insert(layers.end(), comp.begin(), comp.end());
        } else {
            layers.push_back(0);  // compression: none
            putU32(layers, 0);
        }
        if (l.hasTextSpec) {
            putStr16(layers, l.text, 65535);
            putStr16(layers, l.textFamily, 65535);
            layers.push_back(static_cast<std::uint8_t>((l.textBold ? 1 : 0) |
                                                       (l.textItalic ? 2 : 0)));
            layers.push_back(l.textAlign);
            putF64(layers, l.textSize);
            putF64(layers, l.textLineHeight);
            putF64(layers, l.textTracking);
            putF64(layers, l.textWrapWidth);
            putF64(layers, l.textFrameHeight);
            putF64(layers, l.textOriginX);
            putF64(layers, l.textOriginY);
            putU32(layers, l.textColor);
        }
        if (l.hasTextExtras) {
            layers.push_back(l.textUnderline);
            layers.push_back(l.textStrike);
            putU32(layers, l.textUnderlineColor);
            putU32(layers, l.textStrikeColor);
            putU32(layers, l.textBackgroundColor);
            putF64(layers, l.textBaselineShift);
            putF64(layers, l.textHScale);
            putF64(layers, l.textVScale);
            layers.push_back(static_cast<std::uint8_t>(l.textSuperSub));
            layers.push_back(static_cast<std::uint8_t>((l.textAllCaps ? 1 : 0) |
                                                       (l.textKerning ? 2 : 0)));
            putU32(layers, l.textOtFeatures);
        }
        if (l.hasVector) {
            putU32(layers, static_cast<std::uint32_t>(l.vectorData.size()));
            layers.insert(layers.end(), l.vectorData.begin(),
                          l.vectorData.end());
        }
        if (l.hasAdjustment) {
            layers.push_back(l.adjustmentKind);
            // v3 carries the widened 16 params; v1/v2 keep the original 8.
            const int nparams = v3 ? 16 : 8;
            for (int k = 0; k < nparams; ++k)
                putF64(layers, l.adjustmentParams[k]);
            if (l.adjustmentCurve.size() > 65535)
                return fail(error, "adjustment curve too large");
            putU16(layers,
                   static_cast<std::uint16_t>(l.adjustmentCurve.size()));
            for (const auto& pt : l.adjustmentCurve) {
                putF64(layers, pt.first);
                putF64(layers, pt.second);
            }
            // v2 per-channel curves (always present in v2, possibly empty).
            // v1 files predate them and decode with identity channels.
            if (v2) {
                const std::vector<std::pair<double, double>>* chs[3] = {
                    &l.adjustmentCurveR, &l.adjustmentCurveG,
                    &l.adjustmentCurveB};
                for (int c = 0; c < 3; ++c) {
                    if (chs[c]->size() > 65535)
                        return fail(error, "adjustment curve too large");
                    putU16(layers, static_cast<std::uint16_t>(chs[c]->size()));
                    for (const auto& pt : *chs[c]) {
                        putF64(layers, pt.first);
                        putF64(layers, pt.second);
                    }
                }
            }
        }
        if (l.hasToneBlend) {
            for (int k = 0; k < 5; ++k) putF64(layers, l.toneBlend[k]);
        }
        // v2 live-filter block: recipe id, enabled flag, param vector.
        if (v2 && l.hasLiveFilter) {
            putStr16(layers, l.liveFilterId, 4096);
            layers.push_back(l.liveFilterEnabled ? 1 : 0);
            if (l.liveFilterParams.size() > 1024)
                return fail(error, "live filter params too large");
            putU16(layers,
                   static_cast<std::uint16_t>(l.liveFilterParams.size()));
            for (double v : l.liveFilterParams) putF64(layers, v);
        }
        // v2 mask block (coverage grid, deflated like the pixel blocks).
        if (v2 && l.hasMask) {
            layers.push_back(l.maskEnabled ? 1 : 0);
            putF64(layers, l.maskOffsetX);
            putF64(layers, l.maskOffsetY);
            putF64(layers, l.maskScaleX);
            putF64(layers, l.maskScaleY);
            putF64(layers, l.maskDensity);
            putF64(layers, l.maskFeather);
            putU32(layers, l.maskWidth);
            putU32(layers, l.maskHeight);
            const std::uint64_t mp = static_cast<std::uint64_t>(l.maskWidth) *
                                     l.maskHeight;
            if (mp > kMaxPixels)
                return fail(error, "layer mask dimensions out of range");
            if (l.mask.size() != mp)
                return fail(error, "layer mask buffer size mismatch");
            if (mp != 0) {
                // Encoded in parallel above; identical bytes to inline deflate.
                const std::vector<std::uint8_t>& comp = comps[li].mask;
                if (comp.empty())
                    return fail(error, "layer mask compression failed");
                layers.push_back(1);  // compression: deflate
                putU32(layers, static_cast<std::uint32_t>(comp.size()));
                layers.insert(layers.end(), comp.begin(), comp.end());
            } else {
                layers.push_back(0);  // compression: none
                putU32(layers, 0);
            }
        }
        // v4 foreign-PSD block section (every layer in v4 files, possibly
        // empty): sig + key + data + stored pad bytes, verbatim.
        if (v4) {
            if (l.psdBlocks.size() > 64)
                return fail(error, "too many foreign PSD blocks");
            putU32(layers, static_cast<std::uint32_t>(l.psdBlocks.size()));
            for (const auto& b : l.psdBlocks) {
                layers.insert(layers.end(), b.sig, b.sig + 4);
                layers.insert(layers.end(), b.key, b.key + 4);
                putU32(layers, static_cast<std::uint32_t>(b.data.size()));
                layers.insert(layers.end(), b.data.begin(), b.data.end());
                putU32(layers, static_cast<std::uint32_t>(b.padding.size()));
                layers.insert(layers.end(), b.padding.begin(),
                              b.padding.end());
            }
        }
        if (v5) {
            const std::uint64_t cmykSamples =
                static_cast<std::uint64_t>(l.width) * l.height * 4;
            if (!l.cmyk.empty() && l.cmyk.size() != cmykSamples)
                return fail(error, "layer CMYK buffer size mismatch");
            if (!l.cmyk.empty() && comps[li].cmyk.empty())
                return fail(error, "layer CMYK compression failed");
            if (l.cmyk.empty()) {
                layers.push_back(0);
                putU32(layers, 0);
            } else {
                layers.push_back(1);
                putU32(layers,
                       static_cast<std::uint32_t>(comps[li].cmyk.size()));
                layers.insert(layers.end(), comps[li].cmyk.begin(),
                              comps[li].cmyk.end());
            }
        }
        ++li;
    }

    std::vector<std::uint8_t> preview;
    putU32(preview, p.previewWidth);
    putU32(preview, p.previewHeight);
    const std::uint64_t px = static_cast<std::uint64_t>(p.previewWidth) *
                             p.previewHeight;
    if (px > kMaxPixels) return fail(error, "preview dimensions out of range");
    if (px != 0) {
        if (p.preview.size() != px * 4)
            return fail(error, "preview pixel buffer size mismatch");
        const std::vector<std::uint8_t> comp = deflate(samplesToBytes(p.preview));
        if (comp.empty()) return fail(error, "preview compression failed");
        preview.push_back(1);  // compression: deflate
        putU32(preview, static_cast<std::uint32_t>(comp.size()));
        preview.insert(preview.end(), comp.begin(), comp.end());
    } else {
        preview.push_back(0);  // compression: none
        putU32(preview, 0);
    }

    // Section lengths, then the payloads, in matching order.
    putU32(out, static_cast<std::uint32_t>(meta.size()));
    putU32(out, static_cast<std::uint32_t>(layers.size()));
    putU32(out, static_cast<std::uint32_t>(preview.size()));
    out.insert(out.end(), meta.begin(), meta.end());
    out.insert(out.end(), layers.begin(), layers.end());
    out.insert(out.end(), preview.begin(), preview.end());

    return out;
}

std::optional<ProjectFileDoc> projectDecode(const std::vector<std::uint8_t>& b,
                                            std::string* error) {
    Reader in(b);
    if (!in.ok || in.remaining() < 8) return fail(error, "project file too short");
    const std::uint8_t* magic = in.take(8);
    if (!magic || std::memcmp(magic, "INFIPROJ", 8) != 0)
        return fail(error, "not a Pittore project (bad magic)");

    const std::uint32_t version = in.u32();
    const std::uint32_t width = in.u32();
    const std::uint32_t height = in.u32();
    const std::uint16_t channels = in.u16();
    const std::uint16_t depth = in.u16();
    const std::uint16_t colorMode = in.u16();
    in.u16();  // reserved
    const std::uint32_t dpi = in.u32();
    const std::uint16_t bg = in.u16();
    in.u16();  // reserved
    const std::uint32_t metaLen = in.u32();
    const std::uint32_t layersLen = in.u32();
    const std::uint32_t previewLen = in.u32();
    if (!in.ok) return fail(error, "truncated project header");
    if (version < 1 || version > 5)
        return fail(error, "unsupported project version");
    if (channels != 4 || depth != 16)
        return fail(error, "unsupported project channel layout");
    if (in.remaining() < static_cast<std::size_t>(metaLen) + layersLen + previewLen)
        return fail(error, "truncated project sections");

    ProjectFileDoc doc;
    doc.width = width;
    doc.height = height;
    doc.dpi = dpi;
    doc.colorMode = colorMode;
    doc.background = bg == 1 ? "black" : bg == 2 ? "transparent" : "white";

    // Metadata.
    {
        Reader m(in.b);
        m.pos = in.pos;
        const std::size_t metaEnd = in.pos + metaLen;
        doc.name = m.str();
        doc.colorModeName = m.str();
        // Optional trailing ICC block (u32 BE length + bytes); files written
        // before it existed end here, so bounds-check against metaEnd.
        if (m.ok && m.pos + 4 <= metaEnd) {
            const std::uint32_t n = m.u32();
            if (m.ok && n <= metaEnd - m.pos) {
                const std::uint8_t* p = m.take(n);
                if (p) doc.icc.assign(p, p + n);
            }
        }
        if (!m.ok) return fail(error, "corrupt project metadata");
        in.pos += metaLen;
    }

    // Layer records.
    std::vector<ProjectLayerFile> layers;
    {
        Reader l(in.b);
        l.pos = in.pos;
        const std::uint32_t count = l.u32();
        if (!l.ok) return fail(error, "corrupt project layer table");
        if (count > 65536)
            return fail(error, "project layer count out of range");
        layers.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ProjectLayerFile layer;
            layer.name = l.str();
            layer.kind = l.u8();
            const std::uint8_t flags = l.u8();
            layer.visible = (flags & 1) != 0;
            layer.locked = (flags & 2) != 0;
            layer.isText = (flags & 4) != 0;
            layer.hasTextSpec = (flags & 8) != 0;
            layer.hasTextExtras = (flags & 16) != 0;
            layer.hasVector = (flags & 32) != 0;
            layer.hasAdjustment = (flags & 64) != 0;
            layer.hasToneBlend = (flags & 128) != 0;
            if (version >= 2) {
                const std::uint8_t flags2 = l.u8();
                layer.hasMask = (flags2 & 1) != 0;
                layer.clipped = (flags2 & 2) != 0;
                layer.lockTransparency = (flags2 & 4) != 0;
                layer.hasLiveFilter = (flags2 & 8) != 0;
                layer.indent = static_cast<std::int32_t>(l.u32());
            }
            layer.opacity = l.u16();
            layer.fill = l.u16();
            layer.blend = l.str();
            layer.offsetX = l.f64();
            layer.offsetY = l.f64();
            layer.scaleX = l.f64();
            layer.scaleY = l.f64();
            layer.width = l.u32();
            layer.height = l.u32();
            if (!l.ok) return fail(error, "corrupt layer record");
            const std::uint64_t px = static_cast<std::uint64_t>(layer.width) *
                                     layer.height;
            if (px > kMaxPixels)
                return fail(error, "layer dimensions out of range");
            if (layer.width != 0 && layer.height != 0) {
                if (!readPixelBlock(l, px * 4, &layer.rgba,
                                    "corrupt layer pixel data", error))
                    return std::nullopt;
            } else {
                if (!readPixelBlock(l, 0, &layer.rgba,
                                    "corrupt layer record", error))
                    return std::nullopt;
            }
            if (layer.hasTextSpec) {
                layer.text = l.str();
                layer.textFamily = l.str();
                const std::uint8_t style = l.u8();
                layer.textBold = (style & 1) != 0;
                layer.textItalic = (style & 2) != 0;
                layer.textAlign = l.u8();
                layer.textSize = l.f64();
                layer.textLineHeight = l.f64();
                layer.textTracking = l.f64();
                layer.textWrapWidth = l.f64();
                layer.textFrameHeight = l.f64();
                layer.textOriginX = l.f64();
                layer.textOriginY = l.f64();
                layer.textColor = l.u32();
                if (!l.ok) return fail(error, "corrupt layer text record");
            }
            if (layer.hasTextExtras) {
                layer.textUnderline = l.u8();
                layer.textStrike = l.u8();
                layer.textUnderlineColor = l.u32();
                layer.textStrikeColor = l.u32();
                layer.textBackgroundColor = l.u32();
                layer.textBaselineShift = l.f64();
                layer.textHScale = l.f64();
                layer.textVScale = l.f64();
                layer.textSuperSub = static_cast<std::int8_t>(l.u8());
                const std::uint8_t ex = l.u8();
                layer.textAllCaps = (ex & 1) != 0;
                layer.textKerning = (ex & 2) != 0;
                layer.textOtFeatures = l.u32();
                if (!l.ok) return fail(error, "corrupt layer text extras");
            }
            if (layer.hasVector) {
                const std::uint32_t len = l.u32();
                if (!l.ok) return fail(error, "corrupt layer vector record");
                const std::uint8_t* bytes = l.take(len);
                if (!bytes) return fail(error, "corrupt layer vector record");
                layer.vectorData.assign(bytes, bytes + len);
            }
            if (layer.hasAdjustment) {
                layer.adjustmentKind = l.u8();
                // v3+ carries 16 params; v1/v2 carry 8 with the rest identity 0.
                const int nparams = version >= 3 ? 16 : 8;
                for (int k = 0; k < nparams; ++k)
                    layer.adjustmentParams[k] = l.f64();
                for (int k = nparams; k < 16; ++k)
                    layer.adjustmentParams[k] = 0.0;
                const std::uint32_t npts = l.u16();
                if (!l.ok || npts > 1024)
                    return fail(error, "corrupt layer adjustment record");
                layer.adjustmentCurve.reserve(npts);
                for (std::uint32_t k = 0; k < npts; ++k) {
                    const double x = l.f64();
                    const double y = l.f64();
                    if (!l.ok)
                        return fail(error, "corrupt layer adjustment record");
                    layer.adjustmentCurve.emplace_back(x, y);
                }
                if (version >= 2) {
                    std::vector<std::pair<double, double>>* chs[3] = {
                        &layer.adjustmentCurveR, &layer.adjustmentCurveG,
                        &layer.adjustmentCurveB};
                    for (int c = 0; c < 3; ++c) {
                        const std::uint32_t n = l.u16();
                        if (!l.ok || n > 1024)
                            return fail(error,
                                        "corrupt layer adjustment record");
                        chs[c]->reserve(n);
                        for (std::uint32_t k = 0; k < n; ++k) {
                            const double x = l.f64();
                            const double y = l.f64();
                            if (!l.ok)
                                return fail(error,
                                            "corrupt layer adjustment record");
                            chs[c]->emplace_back(x, y);
                        }
                    }
                }
            }
            if (layer.hasToneBlend) {
                for (int k = 0; k < 5; ++k) layer.toneBlend[k] = l.f64();
                if (!l.ok)
                    return fail(error, "corrupt layer tone blend record");
            }
            if (version >= 2 && layer.hasLiveFilter) {
                layer.liveFilterId = l.str();
                layer.liveFilterEnabled = l.u8() != 0;
                const std::uint32_t npar = l.u16();
                if (!l.ok || npar > 1024)
                    return fail(error, "corrupt live filter record");
                layer.liveFilterParams.reserve(npar);
                for (std::uint32_t k = 0; k < npar; ++k) {
                    layer.liveFilterParams.push_back(l.f64());
                    if (!l.ok)
                        return fail(error, "corrupt live filter record");
                }
            }
            if (version >= 2 && layer.hasMask) {
                layer.maskEnabled = l.u8() != 0;
                layer.maskOffsetX = l.f64();
                layer.maskOffsetY = l.f64();
                layer.maskScaleX = l.f64();
                layer.maskScaleY = l.f64();
                layer.maskDensity = l.f64();
                layer.maskFeather = l.f64();
                layer.maskWidth = l.u32();
                layer.maskHeight = l.u32();
                if (!l.ok) return fail(error, "corrupt layer mask record");
                const std::uint64_t mp =
                    static_cast<std::uint64_t>(layer.maskWidth) *
                    layer.maskHeight;
                if (mp > kMaxPixels)
                    return fail(error, "layer mask dimensions out of range");
                if (!readPixelBlock(l, mp, &layer.mask,
                                    "corrupt layer mask data", error))
                    return std::nullopt;
            }
            if (version >= 4) {
                const std::uint32_t nblocks = l.u32();
                if (!l.ok || nblocks > 64)
                    return fail(error, "corrupt foreign PSD block table");
                layer.psdBlocks.reserve(nblocks);
                std::uint64_t blockBytes = 0;
                for (std::uint32_t k = 0; k < nblocks; ++k) {
                    ProjectLayerFile::PsdBlock b;
                    const std::uint8_t* sig = l.take(4);
                    const std::uint8_t* key = l.take(4);
                    if (!sig || !key)
                        return fail(error, "corrupt foreign PSD block record");
                    std::memcpy(b.sig, sig, 4);
                    std::memcpy(b.key, key, 4);
                    const std::uint32_t dlen = l.u32();
                    if (!l.ok || dlen > (32u << 20))
                        return fail(error, "corrupt foreign PSD block record");
                    const std::uint8_t* data = l.take(dlen);
                    if (!data)
                        return fail(error, "corrupt foreign PSD block record");
                    b.data.assign(data, data + dlen);
                    const std::uint32_t plen = l.u32();
                    if (!l.ok || plen > 4)
                        return fail(error, "corrupt foreign PSD block record");
                    const std::uint8_t* pad = l.take(plen);
                    if (!pad)
                        return fail(error, "corrupt foreign PSD block record");
                    b.padding.assign(pad, pad + plen);
                    blockBytes += dlen + plen;
                    if (blockBytes > (256u << 20))
                        return fail(error, "foreign PSD blocks exceed budget");
                    layer.psdBlocks.push_back(std::move(b));
                }
            }
            if (version >= 5) {
                const std::uint64_t cp =
                    static_cast<std::uint64_t>(layer.width) * layer.height;
                if (cp > kMaxPixels ||
                    !readPixelBlock(l, cp * 4, &layer.cmyk,
                                    "corrupt layer CMYK data", error))
                    return std::nullopt;
            }
            layers.push_back(std::move(layer));
        }
        if (!l.ok) return fail(error, "corrupt layer table");
        in.pos += layersLen;
    }
    doc.layers = std::move(layers);

    // Preview.
    {
        Reader p(in.b);
        p.pos = in.pos;
        doc.previewWidth = p.u32();
        doc.previewHeight = p.u32();
        if (!p.ok) return fail(error, "corrupt project preview");
        const std::uint64_t px = static_cast<std::uint64_t>(doc.previewWidth) *
                                 doc.previewHeight;
        if (px > kMaxPixels)
            return fail(error, "preview dimensions out of range");
        if (!readPixelBlock(p, px * 4, &doc.preview, "corrupt project preview data",
                            error))
            return std::nullopt;
    }

    return doc;
}

std::optional<ProjectScanInfo> projectScan(const std::vector<std::uint8_t>& b,
                                           std::string* error) {
    Reader in(b);
    if (!in.ok || in.remaining() < 8) return fail(error, "project file too short");
    const std::uint8_t* magic = in.take(8);
    if (!magic || std::memcmp(magic, "INFIPROJ", 8) != 0)
        return fail(error, "not a Pittore project (bad magic)");

    const std::uint32_t version = in.u32();
    const std::uint32_t width = in.u32();
    const std::uint32_t height = in.u32();
    in.u16();  // channels
    in.u16();  // depth
    in.u16();  // colorMode
    in.u16();  // reserved
    in.u32();  // dpi
    const std::uint16_t bg = in.u16();
    in.u16();  // reserved
    const std::uint32_t metaLen = in.u32();
    const std::uint32_t layersLen = in.u32();
    const std::uint32_t previewLen = in.u32();
    if (!in.ok) return fail(error, "truncated project header");
    if (version < 1 || version > 5)
        return fail(error, "unsupported project version");
    if (in.remaining() < static_cast<std::size_t>(metaLen) + layersLen + previewLen)
        return fail(error, "truncated project sections");

    ProjectScanInfo info;
    info.width = width;
    info.height = height;
    info.background = bg == 1 ? "black" : bg == 2 ? "transparent" : "white";

    {
        Reader m(in.b);
        m.pos = in.pos;
        info.name = m.str();
        info.colorModeName = m.str();
        if (!m.ok) return fail(error, "corrupt project metadata");
        in.pos += metaLen;
    }
    in.skip(layersLen);  // no layer decode when scanning.

    {
        Reader p(in.b);
        p.pos = in.pos;
        info.previewWidth = p.u32();
        info.previewHeight = p.u32();
        if (!p.ok) return fail(error, "corrupt project preview");
        const std::uint64_t px = static_cast<std::uint64_t>(info.previewWidth) *
                                 info.previewHeight;
        if (px > kMaxPixels)
            return fail(error, "preview dimensions out of range");
        if (!readPixelBlock(p, px * 4, &info.preview,
                            "corrupt project preview data", error))
            return std::nullopt;
    }
    return info;
}

}  // namespace pittore::io
