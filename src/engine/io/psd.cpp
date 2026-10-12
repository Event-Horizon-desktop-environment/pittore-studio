#include "engine/io/psd.h"

#include "engine/color/icc_convert.h"
#include "engine/color/separate.h"
#include "engine/io/icc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>
#include <zlib.h>

// Shared per-pixel math, header-only (no link dependency on the compute
// library): the adjustment transfers and the blend cores, so the exported
// preview matches the live engine bit-for-bit where the pixel path agrees.
#include "engine/compute/adjust.h"
#include "engine/compute/blend.h"
#include "engine/core/parallel.h"
#include "engine/core/tonal_ops.h"

namespace pittore::io {
namespace {

constexpr std::uint16_t kMaxSample = 65535;

// Bounds-checked big-endian reader over a byte vector.
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

    std::uint16_t u16be() {
        if (!ok || pos + 2 > b.size()) {
            ok = false;
            return 0;
        }
        const std::uint16_t v = static_cast<std::uint16_t>((b[pos] << 8) | b[pos + 1]);
        pos += 2;
        return v;
    }

    std::uint32_t u32be() {
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

    std::uint64_t u64be() {
        if (!ok || pos + 8 > b.size()) {
            ok = false;
            return 0;
        }
        const std::uint64_t v =
            (static_cast<std::uint64_t>(b[pos]) << 56) |
            (static_cast<std::uint64_t>(b[pos + 1]) << 48) |
            (static_cast<std::uint64_t>(b[pos + 2]) << 40) |
            (static_cast<std::uint64_t>(b[pos + 3]) << 32) |
            (static_cast<std::uint64_t>(b[pos + 4]) << 24) |
            (static_cast<std::uint64_t>(b[pos + 5]) << 16) |
            (static_cast<std::uint64_t>(b[pos + 6]) << 8) |
            static_cast<std::uint64_t>(b[pos + 7]);
        pos += 8;
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
};

std::nullopt_t fail(std::string* error, const char* why) {
    if (error) *error = why;
    return std::nullopt;
}

// PackBits-style PSD RLE decoder. Fills `out` with exactly `expected` bytes.
bool unpackRow(const std::uint8_t* src, std::size_t n, std::uint8_t* out,
               std::size_t expected) {
    std::size_t i = 0, o = 0;
    while (i < n && o < expected) {
        const std::uint8_t h = src[i++];
        if (h < 128) {
            const std::size_t c = static_cast<std::size_t>(h) + 1;
            if (i + c > n || o + c > expected) return false;
            std::memcpy(out + o, src + i, c);
            i += c;
            o += c;
        } else {
            const std::size_t c = 257u - h;
            if (i >= n || o + c > expected) return false;
            const std::uint8_t v = src[i++];
            for (std::size_t k = 0; k < c; ++k) out[o++] = v;
        }
    }
    return o == expected;
}

// Reads one planar scanline of `width` samples (byte-run expanded by `run` or
// taken raw) into 16-bit samples. Returns false on failure.
inline std::uint16_t f32beToU16(const std::uint8_t* p);
bool readPlaneRow(Reader& src, int width, int depth, bool rle,
                  std::uint32_t rowBytes, std::uint16_t* out) {
    const int bytesPerSample = depth == 32 ? 4 : (depth >= 16 ? 2 : 1);
    const std::uint32_t expected = static_cast<std::uint32_t>(width * bytesPerSample);
    if (rle) {
        const std::uint8_t* packed = src.take(rowBytes);
        if (!packed) return false;
        std::vector<std::uint8_t> raw(expected);
        if (!unpackRow(packed, rowBytes, raw.data(), expected)) return false;
        if (bytesPerSample == 1) {
            for (int x = 0; x < width; ++x)
                out[x] = static_cast<std::uint16_t>(raw[x]) * 257u;
        } else if (bytesPerSample == 4) {
            for (int x = 0; x < width; ++x) out[x] = f32beToU16(raw.data() + x * 4);
        } else {
            for (int x = 0; x < width; ++x)
                out[x] = static_cast<std::uint16_t>((raw[x * 2] << 8) | raw[x * 2 + 1]);
        }
        return true;
    }
    // Raw rows are padded to an even byte count.
    const std::uint32_t padded = (expected + 1u) & ~1u;
    if (padded != rowBytes) return false;
    const std::uint8_t* raw = src.take(padded);
    if (!raw) return false;
    if (bytesPerSample == 1) {
        for (int x = 0; x < width; ++x)
            out[x] = static_cast<std::uint16_t>(raw[x]) * 257u;
    } else if (bytesPerSample == 4) {
        for (int x = 0; x < width; ++x) out[x] = f32beToU16(raw + x * 4);
    } else {
        for (int x = 0; x < width; ++x)
            out[x] = static_cast<std::uint16_t>((raw[x * 2] << 8) | raw[x * 2 + 1]);
    }
    return true;
}

std::uint16_t clamp16(double v) {
    if (v <= 0.0) return 0;
    if (v >= 1.0) return kMaxSample;
    return static_cast<std::uint16_t>(v * kMaxSample + 0.5);
}

// Lab (D50) → sRGB. `L`, `a`, `b` are PSD samples (a/b biased by half-range).
void labToRgb(std::uint16_t L, std::uint16_t a, std::uint16_t b, double* r,
              double* g, double* bl) {
    constexpr double kScale = 1.0 / 65535.0;
    const double Lf = static_cast<double>(L) * kScale * 100.0;
    const double af = static_cast<double>(a) * kScale * 255.0 - 128.0;
    const double bf = static_cast<double>(b) * kScale * 255.0 - 128.0;

    auto finv = [](double t) -> double {
        constexpr double delta = 6.0 / 29.0;
        return t > delta ? t * t * t : 3.0 * delta * delta * (t - 4.0 / 29.0);
    };

    const double fy = (Lf + 16.0) / 116.0;
    const double fx = fy + af / 500.0;
    const double fz = fy - bf / 200.0;

    const double X = 0.95047 * finv(fx);
    const double Y = 1.00000 * finv(fy);
    const double Z = 1.08883 * finv(fz);

    const double rl = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    const double gl = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    const double bl_ = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;

    auto gamma = [](double c) -> double {
        c = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
        return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    };

    *r = gamma(rl);
    *g = gamma(gl);
    *bl = gamma(bl_);
}

// Packs one scanline of bytes into PSD PackBits tokens.
std::vector<std::uint8_t> packBitsRow(const std::uint8_t* src, std::size_t n) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && src[i + run] == src[i] && run < 128) ++run;
        if (run >= 3) {
            while (run > 0) {
                const std::size_t chunk = run < 128 ? run : 128;
                out.push_back(static_cast<std::uint8_t>(257 - chunk));
                out.push_back(src[i]);
                run -= chunk;
                i += chunk;
            }
            continue;
        }
        // Literal block: up to 128 bytes, ending before a 3+ repeat.
        std::size_t lit = 0;
        while (i + lit < n && lit < 128) {
            std::size_t lr = 1;
            while (i + lit + lr < n && src[i + lit + lr] == src[i + lit] && lr < 128) ++lr;
            if (lr >= 3) break;
            ++lit;
        }
        if (lit == 0) lit = 1;  // a lone byte that never forms a run
        out.push_back(static_cast<std::uint8_t>(lit - 1));
        for (std::size_t k = 0; k < lit; ++k) out.push_back(src[i + k]);
        i += lit;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Layered import/export (groups, blend modes, opacity, visibility, names)
// ---------------------------------------------------------------------------

struct BlendEntry {
    const char key[4];
    const char* name;
};

// PSD 4cc blend-mode keys → Pittore blend-mode names.
constexpr BlendEntry kBlendMap[] = {
    {{'n', 'o', 'r', 'm'}, "Normal"},              {{'d', 'i', 's', 's'}, "Dissolve"},
    {{'d', 'a', 'r', 'k'}, "Darken"},              {{'m', 'u', 'l', ' '}, "Multiply"},
    {{'i', 'd', 'i', 'v'}, "Color Burn"},          {{'l', 'b', 'r', 'n'}, "Linear Burn"},
    {{'d', 'k', 'C', 'l'}, "Darker Color"},        {{'l', 'i', 't', 'e'}, "Lighten"},
    {{'s', 'c', 'r', 'n'}, "Screen"},              {{'d', 'i', 'v', ' '}, "Color Dodge"},
    {{'l', 'd', 'd', 'g'}, "Linear Dodge (Add)"},  {{'l', 'g', 'C', 'l'}, "Lighter Color"},
    {{'o', 'v', 'e', 'r'}, "Overlay"},             {{'s', 'L', 'i', 't'}, "Soft Light"},
    {{'h', 'L', 'i', 't'}, "Hard Light"},          {{'v', 'L', 'i', 't'}, "Vivid Light"},
    {{'l', 'L', 'i', 't'}, "Linear Light"},        {{'p', 'L', 'i', 't'}, "Pin Light"},
    {{'h', 'M', 'i', 'x'}, "Hard Mix"},            {{'d', 'i', 'f', 'f'}, "Difference"},
    {{'s', 'm', 'u', 'd'}, "Exclusion"},           {{'f', 's', 'u', 'b'}, "Subtract"},
    {{'f', 'd', 'i', 'v'}, "Divide"},              {{'h', 'u', 'e', ' '}, "Hue"},
    {{'s', 'a', 't', ' '}, "Saturation"},          {{'c', 'o', 'l', 'r'}, "Color"},
    {{'l', 'u', 'm', ' '}, "Luminosity"},          {{'p', 'a', 's', 's'}, "Pass Through"},
};

std::string blendNameFromKey(const std::uint8_t* k) {
    for (const BlendEntry& e : kBlendMap)
        if (std::memcmp(k, e.key, 4) == 0) return e.name;
    return "Normal";
}

std::string blendKeyFromName(const std::string& name) {
    for (const BlendEntry& e : kBlendMap)
        if (name == e.name) return std::string(e.key, 4);
    return std::string("norm");
}

void append16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void append32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

// Layer samples are straight RGBA16 in the FULL 16-bit range regardless of the
// source depth; assemble one raw scanline of `bps`-byte samples into them.
// 32-bit samples arrive as big-endian floats in 0..1 and are scaled into that
// same range — NaN, negatives and +0 all fall to zero.
inline std::uint16_t f32beToU16(const std::uint8_t* p) {
    const std::uint32_t u = (static_cast<std::uint32_t>(p[0]) << 24) |
                            (static_cast<std::uint32_t>(p[1]) << 16) |
                            (static_cast<std::uint32_t>(p[2]) << 8) |
                            static_cast<std::uint32_t>(p[3]);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    if (!(f > 0.0f)) return 0;  // NaN, negative and +0 all map to 0
    if (f >= 1.0f) return kMaxSample;
    return static_cast<std::uint16_t>(f * kMaxSample + 0.5f);
}

void assembleRow(const std::uint8_t* raw, int width, int bps, std::uint16_t* out) {
    if (bps == 1) {
        for (int x = 0; x < width; ++x) out[x] = static_cast<std::uint16_t>(raw[x]) * 257u;
    } else if (bps == 4) {
        for (int x = 0; x < width; ++x) out[x] = f32beToU16(raw + x * 4);
    } else {
        for (int x = 0; x < width; ++x)
            out[x] = static_cast<std::uint16_t>((raw[x * 2] << 8) | raw[x * 2 + 1]);
    }
}

// Decodes one layer channel block (compression + payload) into a plane of
// width*height 16-bit samples. Supports Raw, RLE (the byte-count table is
// carried inside the block, as layer channels require) and deflate ZIP with or
// without prediction. RLE counts are u16 in PSD, u32 in PSB.
bool decodeLayerChannel(const std::vector<std::uint8_t>& block, int width, int height,
                        int depth, std::vector<std::uint16_t>& plane,
                        bool isPsb = false) {
    if (width <= 0 || height <= 0 || block.size() < 2) return false;
    const int compression =
        static_cast<int>((static_cast<unsigned>(block[0]) << 8) | block[1]);
    const std::uint8_t* data = block.data() + 2;
    const std::size_t dataLen = block.size() - 2;
    const int bps = depth == 32 ? 4 : (depth >= 16 ? 2 : 1);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * bps;
    plane.assign(static_cast<std::size_t>(width) * height, 0);

    if (compression == 0) {  // raw, even-padded rows
        const std::size_t padded = (rowBytes + 1u) & ~1u;
        if (dataLen != padded * static_cast<std::size_t>(height)) return false;
        for (int y = 0; y < height; ++y)
            assembleRow(data + static_cast<std::size_t>(y) * padded, width, bps,
                        plane.data() + static_cast<std::size_t>(y) * width);
        return true;
    }
    if (compression == 1) {  // RLE with an in-band row-length table
        const std::size_t countSize = isPsb ? 4 : 2;
        if (dataLen < countSize * static_cast<std::size_t>(height)) return false;
        std::vector<std::uint32_t> runs(height);
        std::size_t region = 0;
        for (int y = 0; y < height; ++y) {
            std::uint32_t v = 0;
            if (isPsb) {
                v = (static_cast<std::uint32_t>(data[4 * y]) << 24) |
                    (static_cast<std::uint32_t>(data[4 * y + 1]) << 16) |
                    (static_cast<std::uint32_t>(data[4 * y + 2]) << 8) |
                    static_cast<std::uint32_t>(data[4 * y + 3]);
            } else {
                v = static_cast<std::uint32_t>((data[2 * y] << 8) | data[2 * y + 1]);
            }
            runs[y] = v;
            region += v;
        }
        if (countSize * height + region != dataLen) return false;
        std::size_t cursor = countSize * height;
        std::vector<std::uint8_t> raw(rowBytes);
        for (int y = 0; y < height; ++y) {
            if (!unpackRow(data + cursor, runs[y], raw.data(), rowBytes)) return false;
            assembleRow(raw.data(), width, bps,
                        plane.data() + static_cast<std::size_t>(y) * width);
            cursor += runs[y];
        }
        return true;
    }
    if (compression == 2 || compression == 3) {  // ZIP (raw deflate) [+ prediction]
        const std::size_t expect = rowBytes * static_cast<std::size_t>(height);
        if (expect == 0 || expect > (1u << 30) || dataLen == 0) return false;
        std::vector<std::uint8_t> inflated(expect);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        zs.next_in = const_cast<Bytef*>(data);
        zs.avail_in = static_cast<uInt>(dataLen);
        zs.next_out = inflated.data();
        zs.avail_out = static_cast<uInt>(expect);
        const int zr = inflate(&zs, Z_FINISH);
        const bool ok = (zr == Z_STREAM_END && zs.total_out == expect);
        inflateEnd(&zs);
        if (!ok) return false;
        if (compression == 3) {
            std::vector<std::uint8_t> scratch;
            if (depth == 32) scratch.resize(rowBytes);
            for (int y = 0; y < height; ++y) {
                std::uint8_t* rowp = inflated.data() + static_cast<std::size_t>(y) * rowBytes;
                if (depth == 8) {
                    std::uint8_t acc = 0;
                    for (std::size_t x = 0; x < rowBytes; ++x) {
                        acc = static_cast<std::uint8_t>(acc + rowp[x]);
                        rowp[x] = acc;
                    }
                } else if (depth == 32) {
                    for (std::size_t x = 1; x < rowBytes; ++x)
                        rowp[x] = static_cast<std::uint8_t>(rowp[x] + rowp[x - 1]);
                    scratch.assign(rowp, rowp + rowBytes);
                    const int w = width;
                    for (int i = 0; i < w; ++i)
                        for (int k = 0; k < 4; ++k)
                            rowp[i * 4 + k] = scratch[k * w + i];
                } else {  // 16-bit: big-endian sample delta per row
                    for (int x = 1; x < width; ++x) {
                        const std::uint16_t a = static_cast<std::uint16_t>(
                            (rowp[x * 2] << 8) | rowp[x * 2 + 1]);
                        const std::uint16_t b = static_cast<std::uint16_t>(
                            (rowp[x * 2 - 2] << 8) | rowp[x * 2 - 1]);
                        const std::uint16_t s =
                            static_cast<std::uint16_t>(a + b);
                        rowp[x * 2] = static_cast<std::uint8_t>(s >> 8);
                        rowp[x * 2 + 1] = static_cast<std::uint8_t>(s & 0xff);
                    }
                }
            }
        }
        for (int y = 0; y < height; ++y)
            assembleRow(inflated.data() + static_cast<std::size_t>(y) * rowBytes, width,
                        bps, plane.data() + static_cast<std::size_t>(y) * width);
        return true;
    }
    return false;
}

// UTF-16BE (as stored in 'luni' blocks) → UTF-8.
std::string utf16beToUtf8(const std::uint8_t* p, std::size_t n) {
    std::string out;
    auto push = [&out](std::uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        std::uint32_t cp = (static_cast<std::uint32_t>(p[i]) << 8) | p[i + 1];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
            const std::uint32_t lo = (p[i + 2] << 8) | p[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            continue;
        }
        push(cp);
    }
    return out;
}

// UTF-8 → UTF-16BE bytes (as 'luni' wants).
std::vector<std::uint8_t> utf8ToUtf16be(const std::string& s) {
    std::vector<std::uint8_t> out;
    auto push = [&out](std::uint32_t cp) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            const std::uint32_t hi = 0xD800 + (cp >> 10);
            const std::uint32_t lo = 0xDC00 + (cp & 0x3FF);
            out.push_back(static_cast<std::uint8_t>(hi >> 8));
            out.push_back(static_cast<std::uint8_t>(hi & 0xFF));
            out.push_back(static_cast<std::uint8_t>(lo >> 8));
            out.push_back(static_cast<std::uint8_t>(lo & 0xFF));
        } else {
            out.push_back(static_cast<std::uint8_t>(cp >> 8));
            out.push_back(static_cast<std::uint8_t>(cp & 0xFF));
        }
    };
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0xFFFD;
        if (c < 0x80) { cp = c; ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                 ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); i += 4;
        } else ++i;
        push(cp);
    }
    return out;
}

// One parsed layer record, before group nesting is applied. The channel info
// (ids + block lengths) is kept so the outer walker can decode the channel
// image data that the format stores after all layer records.
struct ParsedLayer {
    PsdLayerFile file;
    int lsct = 0;  // 0 other, 1 open folder, 2 closed folder, 3 folder end
    struct Chan {
        std::int16_t id;
        std::uint64_t len;  // includes the 2-byte compression tag (u64 in PSB)
    };
    std::vector<Chan> chans;
    // First adjustment payload found ('brit', 'levl', 'curv', 'expA',
    // 'vibA', 'hust'/'hue2', 'nvrt', 'thrs', 'post'), copied verbatim.
    char adjKey[4] = {};
    bool hasAdjKey = false;
    std::vector<std::uint8_t> adjData;
    // Uninterpreted 8BIM blocks, preserved verbatim so round-trips don't
    // shed data we don't understand (effects, vector masks, ...). Blocks we
    // regenerate ourselves ('luni', 'lsct', masks, adjustments) are never
    // stored here.
    struct RawBlock {
        char sig[4] = {'8', 'B', 'I', 'M'};
        char key[4];
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> padding;
    };
    std::vector<RawBlock> rawBlocks;
};

// True for the additional-info keys that carry adjustment payloads. Both
// 'hust' and 'hue2' are accepted on read (same layout); only 'hust' is
// ever written.
bool isAdjustmentKey(const std::uint8_t* key) {
    static const char* const kKeys[] = {"brit", "levl", "curv", "expA",
                                        "vibA", "hust", "hue2", "nvrt",
                                        "thrs", "post"};
    for (const char* k : kKeys)
        if (std::memcmp(key, k, 4) == 0) return true;
    return false;
}

// Parse the layer mask/adjustment record carried in a layer's extra data.
// Only the mask rectangle, default handling and enable/relative flags are
// needed to keep the mask live; any vendor extensions after the base record
// are ignored. Returns false on truncation.
bool parseMaskRecord(const std::uint8_t* data, std::size_t n,
                     PsdLayerFile& file) {
    file.hasMask = false;
    file.maskEnabled = true;
    file.maskRelative = false;
    file.maskLeft = file.maskTop = 0;
    file.maskWidth = file.maskHeight = 0;
    if (n == 0) return true;
    if (n < 18) return false;
    const auto s32 = [&](std::size_t i) {
        return static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(data[i]) << 24) |
            (static_cast<std::uint32_t>(data[i + 1]) << 16) |
            (static_cast<std::uint32_t>(data[i + 2]) << 8) |
            static_cast<std::uint32_t>(data[i + 3]));
    };
    const std::int32_t top = s32(0);
    const std::int32_t left = s32(4);
    const std::int32_t bottom = s32(8);
    const std::int32_t right = s32(12);
    const std::uint8_t flags = data[17];
    if (right <= left || bottom <= top) return true;
    const std::uint64_t mw = static_cast<std::uint64_t>(right - left);
    const std::uint64_t mh = static_cast<std::uint64_t>(bottom - top);
    if (mw > (1u << 30) || mh > (1u << 30) || mw * mh > (1u << 30))
        return false;
    file.maskLeft = left;
    file.maskTop = top;
    file.maskWidth = static_cast<std::uint32_t>(mw);
    file.maskHeight = static_cast<std::uint32_t>(mh);
    file.maskRelative = (flags & 0x01) != 0;
    file.maskEnabled = (flags & 0x02) == 0;
    return true;
}

// ---------------------------------------------------------------------------
// Adjustment payloads ('brit', 'levl', 'curv', 'expA', 'vibA', 'hust'/'hue2',
// 'nvrt', 'thrs', 'post'). Layouts follow the long-documented legacy block
// shapes; 'expA'/'vibA' use a minimal versioned descriptor whose exact
// PSD fidelity is best-effort (see below) — our own reader parses our
// own writer exactly, and every block is length-prefixed so third parties
// skip safely regardless.
// ---------------------------------------------------------------------------

bool adjU16(const std::uint8_t* d, std::size_t n, std::size_t at,
            std::uint16_t& v) {
    if (at + 2 > n) return false;
    v = static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
    return true;
}

bool adjI16(const std::uint8_t* d, std::size_t n, std::size_t at,
            std::int16_t& v) {
    std::uint16_t u;
    if (!adjU16(d, n, at, u)) return false;
    v = static_cast<std::int16_t>(u);
    return true;
}

bool adjU32(const std::uint8_t* d, std::size_t n, std::size_t at,
            std::uint32_t& v) {
    if (at + 4 > n) return false;
    v = (static_cast<std::uint32_t>(d[at]) << 24) |
        (static_cast<std::uint32_t>(d[at + 1]) << 16) |
        (static_cast<std::uint32_t>(d[at + 2]) << 8) |
        static_cast<std::uint32_t>(d[at + 3]);
    return true;
}

bool adjF64(const std::uint8_t* d, std::size_t n, std::size_t at, double& v) {
    if (at + 8 > n) return false;
    std::uint64_t u = 0;
    for (int k = 0; k < 8; ++k) u = (u << 8) | d[at + k];
    std::memcpy(&v, &u, 8);
    return true;
}

// Minimal versioned-descriptor reader for the 'doub' items we write (and
// the spec writes for these keys): u16 + u32 version prefix, then name,
// class, item count, and key/type/value triples. Returns the key→value map,
// or nullopt when the shape is anything else (foreign descriptors degrade
// to a stand-in layer instead of misparsed pixels).
std::optional<std::map<std::string, double>> parseVersionedDoubles(
    const std::uint8_t* d, std::size_t n) {
    std::uint16_t v16;
    std::uint32_t v32;
    if (!adjU16(d, n, 0, v16) || !adjU32(d, n, 2, v32)) return std::nullopt;
    std::size_t at = 6;
    std::uint32_t nameLen;
    if (!adjU32(d, n, at, nameLen)) return std::nullopt;
    at += 4;
    if (nameLen > 256 || at + nameLen * 2 > n) return std::nullopt;
    at += nameLen * 2;
    std::uint32_t classLen;
    if (!adjU32(d, n, at, classLen)) return std::nullopt;
    at += 4;
    if (classLen == 0) {
        if (at + 4 > n) return std::nullopt;
        at += 4;
    } else {
        if (classLen > 64 || at + classLen > n) return std::nullopt;
        at += classLen;
    }
    std::uint32_t count;
    if (!adjU32(d, n, at, count)) return std::nullopt;
    at += 4;
    if (count > 64) return std::nullopt;
    std::map<std::string, double> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 8 > n) return std::nullopt;
        const std::string key(reinterpret_cast<const char*>(d + at), 4);
        const bool isDouble = std::memcmp(d + at + 4, "doub", 4) == 0;
        at += 8;
        if (!isDouble) return std::nullopt;
        double v;
        if (!adjF64(d, n, at, v)) return std::nullopt;
        at += 8;
        out[key] = v;
    }
    return out;
}

void appendF64(std::vector<std::uint8_t>& out, double v) {
    std::uint64_t u = 0;
    std::memcpy(&u, &v, 8);
    for (int k = 7; k >= 0; --k)
        out.push_back(static_cast<std::uint8_t>((u >> (k * 8)) & 0xff));
}

// Mirror of parseVersionedDoubles: u16 + u32 version prefix, empty name,
// 4-char class, then key/'doub'/BE-double items.
std::vector<std::uint8_t> versionedDescriptor(
    const char* classId,
    const std::vector<std::pair<std::string, double>>& items) {
    std::vector<std::uint8_t> out;
    append16(out, 16);
    append32(out, 16);
    append32(out, 0);  // empty name
    append32(out, 0);  // 4-char class follows
    out.insert(out.end(), classId, classId + 4);
    append32(out, static_cast<std::uint32_t>(items.size()));
    for (const auto& [key, v] : items) {
        out.insert(out.end(), key.begin(), key.begin() + 4);
        out.insert(out.end(), {'d', 'o', 'u', 'b'});
        appendF64(out, v);
    }
    return out;
}

// Decode one adjustment payload into a live PsdLayerFile adjustment. Returns
// false when the payload is malformed (the caller then keeps a stand-in).
bool parseAdjustmentBlock(const char key[4], const std::uint8_t* data,
                          std::size_t n, PsdLayerFile& file) {
    const auto same = [&](const char* k) { return std::memcmp(key, k, 4) == 0; };
    if (same("nvrt")) {
        file.adjustmentKind = 7;
        return true;
    }
    if (same("post")) {
        std::uint16_t levels;
        if (!adjU16(data, n, 0, levels) || levels < 2) return false;
        file.adjustmentKind = 9;
        file.adjustmentParams[0] = static_cast<float>(levels);
        return true;
    }
    if (same("thrs")) {
        std::uint16_t level;
        if (!adjU16(data, n, 0, level)) return false;
        file.adjustmentKind = 8;
        file.adjustmentParams[0] = static_cast<float>(level) / 255.0f;
        return true;
    }
    if (same("brit")) {
        std::int16_t b, c;
        if (!adjI16(data, n, 0, b) || !adjI16(data, n, 2, c)) return false;
        file.adjustmentKind = 1;
        file.adjustmentParams[0] = static_cast<float>(b) / 100.0f;
        file.adjustmentParams[1] = static_cast<float>(c) / 100.0f;
        return true;
    }
    if (same("levl")) {
        // A u16 version, then 29 records of 5 u16s each: input black point,
        // input white point, output black point, output white point and
        // gamma*100. Record 0 is the composite channel, which is all the
        // master-only Levels here reads; the per-channel records beyond
        // identity are a documented import limitation.
        std::uint16_t version;
        if (!adjU16(data, n, 0, version) || n < 2 + 4 * 10) return false;
        std::uint16_t v[5];
        for (int k = 0; k < 5; ++k)
            if (!adjU16(data, n, 2 + k * 2, v[k])) return false;
        file.adjustmentKind = 2;
        file.adjustmentParams[0] = static_cast<float>(v[0]) / 255.0f;
        file.adjustmentParams[1] = static_cast<float>(v[1]) / 255.0f;
        file.adjustmentParams[2] = static_cast<float>(v[4]) / 100.0f;
        file.adjustmentParams[3] = static_cast<float>(v[2]) / 255.0f;
        file.adjustmentParams[4] = static_cast<float>(v[3]) / 255.0f;
        return true;
    }
    if (same("curv")) {
        // Legacy shape: u8 padding, u16 version (1 or 4), u32 channel bitmap,
        // then per channel: u16 point count and (output, input) u16 pairs.
        // Bitmap bit 0 = RGB composite (master), bits 1..3 = R/G/B. Every
        // present channel lands in its own slot; a missing slot is identity.
        if (n < 7) return false;
        std::uint16_t version;
        if (!adjU16(data, n, 1, version) || (version != 1 && version != 4))
            return false;
        std::uint32_t bitmap;
        if (!adjU32(data, n, 3, bitmap) || bitmap == 0) return false;
        std::size_t at = 7;
        bool found = false;
        for (int ch = 0; ch < 4; ++ch) {
            if ((bitmap & (1u << ch)) == 0) continue;
            std::uint16_t count;
            if (!adjU16(data, n, at, count)) return false;
            at += 2;
            if (count < 2 || count > 32) {
                // Not our channel (or corrupt): skip what we can, else fail.
                if (at + static_cast<std::size_t>(count) * 4 > n) return false;
                at += static_cast<std::size_t>(count) * 4;
                continue;
            }
            std::vector<std::pair<double, double>> pts;
            bool bad = false;
            for (int k = 0; k < count; ++k) {
                std::uint16_t o, i;
                if (!adjU16(data, n, at, o) || !adjU16(data, n, at + 2, i)) {
                    bad = true;
                    break;
                }
                at += 4;
                pts.emplace_back(i / 255.0, o / 255.0);
            }
            if (bad) return false;
            if (ch == 0)
                file.adjustmentCurve = std::move(pts);
            else if (ch == 1)
                file.adjustmentCurveR = std::move(pts);
            else if (ch == 2)
                file.adjustmentCurveG = std::move(pts);
            else
                file.adjustmentCurveB = std::move(pts);
            found = true;
        }
        if (!found) return false;
        file.adjustmentKind = 3;
        return true;
    }
    if (same("hust") || same("hue2")) {
        // Version u16, colorize u16, then master hue/saturation/lightness
        // i16. Ranges and colorize have no engine counterpart (documented);
        // a colorized file still imports its master values.
        std::int16_t h, s, l;
        if (!adjI16(data, n, 4, h) || !adjI16(data, n, 6, s) ||
            !adjI16(data, n, 8, l))
            return false;
        file.adjustmentKind = 6;
        file.adjustmentParams[0] = static_cast<float>(h);
        file.adjustmentParams[1] = static_cast<float>(s) / 100.0f;
        file.adjustmentParams[2] = static_cast<float>(l) / 100.0f;
        return true;
    }
    if (same("expA")) {
        auto d = parseVersionedDoubles(data, n);
        if (!d) return false;
        const auto it = d->find("Exps");
        if (it == d->end()) return false;
        file.adjustmentKind = 4;
        file.adjustmentParams[0] = static_cast<float>(it->second);
        return true;
    }
    if (same("vibA")) {
        auto d = parseVersionedDoubles(data, n);
        if (!d) return false;
        const auto it = d->find("Vibr");
        if (it == d->end()) return false;
        file.adjustmentKind = 5;
        file.adjustmentParams[0] = static_cast<float>(it->second) / 100.0f;
        return true;
    }
    return false;
}

// Encode live adjustment fields back into an additional-info payload.
// Inverse of parseAdjustmentBlock for every kind we can write.
std::vector<std::uint8_t> encodeAdjustmentBlock(const PsdLayerFile& l,
                                                char keyOut[4]) {
    std::vector<std::uint8_t> out;
    const auto q255 = [](float v) {
        return static_cast<std::uint16_t>(
            std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    switch (l.adjustmentKind) {
        case 7:  // Invert: empty payload.
            std::memcpy(keyOut, "nvrt", 4);
            break;
        case 9: {  // Posterize: u16 levels.
            std::memcpy(keyOut, "post", 4);
            const auto lv = static_cast<std::uint16_t>(
                std::clamp(l.adjustmentParams[0], 2.0f, 255.0f) + 0.5f);
            append16(out, lv);
            break;
        }
        case 8: {  // Threshold: u16 level 0..255.
            std::memcpy(keyOut, "thrs", 4);
            append16(out, q255(l.adjustmentParams[0]));
            break;
        }
        case 1: {  // Brightness/Contrast: i16, i16, mean i16, lab u8.
            std::memcpy(keyOut, "brit", 4);
            const auto q = [](float v) {
                return static_cast<std::int16_t>(std::clamp(
                    std::lround(v * 100.0f), -32768L, 32767L));
            };
            const std::int16_t b = q(l.adjustmentParams[0]);
            const std::int16_t c = q(l.adjustmentParams[1]);
            append16(out, static_cast<std::uint16_t>(b));
            append16(out, static_cast<std::uint16_t>(c));
            append16(out, 0);  // mean
            out.push_back(0);  // lab flag
            break;
        }
        case 2: {  // Levels: version + 29 records of 5 u16s. Record 0 is
                   // ours; records 1..3 (per-channel) are identity — our
                   // Levels is master-only, matching what we can import.
            std::memcpy(keyOut, "levl", 4);
            append16(out, 2);  // version
            const auto rec = [&](float ib, float iw, float ob, float ow,
                                 float gm) {
                append16(out, q255(ib));
                append16(out, q255(iw));
                append16(out, q255(ob));
                append16(out, q255(ow));
                append16(out, static_cast<std::uint16_t>(std::clamp(
                                   gm * 100.0f, 1.0f, 1000.0f) +
                               0.5f));
            };
            rec(l.adjustmentParams[0], l.adjustmentParams[1],
                l.adjustmentParams[3], l.adjustmentParams[4],
                l.adjustmentParams[2]);
            for (int k = 0; k < 3; ++k) rec(0.0f, 1.0f, 0.0f, 1.0f, 1.0f);
            for (int k = 0; k < 25; ++k) rec(0.0f, 1.0f, 0.0f, 1.0f, 1.0f);
            break;
        }
        case 3: {  // Curves (legacy): RGB composite + R/G/B channels.
            std::memcpy(keyOut, "curv", 4);
            out.push_back(0);  // padding
            append16(out, 1);  // version
            // Bitmap over the non-empty channels (bit 0 = RGB composite).
            // An all-identity layer still writes the master pair so the
            // block stays readable as Curves.
            const std::vector<std::pair<double, double>>* ch[4] = {
                &l.adjustmentCurve, &l.adjustmentCurveR,
                &l.adjustmentCurveG, &l.adjustmentCurveB};
            std::uint32_t bitmap = 0;
            for (int c = 0; c < 4; ++c)
                if (ch[c]->size() >= 2) bitmap |= (1u << c);
            if (bitmap == 0) bitmap = 1;
            append32(out, bitmap);
            for (int c = 0; c < 4; ++c) {
                if ((bitmap & (1u << c)) == 0) continue;
                std::vector<std::pair<double, double>> pts(ch[c]->begin(),
                                                           ch[c]->end());
                if (pts.size() < 2) pts = {{0.0, 0.0}, {1.0, 1.0}};
                const std::size_t count = std::min<std::size_t>(pts.size(), 32);
                append16(out, static_cast<std::uint16_t>(count));
                for (std::size_t k = 0; k < count; ++k) {
                    append16(out, q255(static_cast<float>(pts[k].second)));
                    append16(out, q255(static_cast<float>(pts[k].first)));
                }
            }
            break;
        }
        case 6: {  // Hue/Saturation: version, colorize, h/s/l i16.
            std::memcpy(keyOut, "hust", 4);
            append16(out, 2);  // version
            append16(out, 0);  // colorize off (master-only engine)
            const auto qi = [](float v) {
                return static_cast<std::uint16_t>(static_cast<std::int16_t>(
                    std::clamp(std::lround(v), -32768L, 32767L)));
            };
            append16(out, qi(std::lround(l.adjustmentParams[0])));
            append16(out, qi(std::lround(l.adjustmentParams[1] * 100.0f)));
            append16(out, qi(std::lround(l.adjustmentParams[2] * 100.0f)));
            break;
        }
        case 4: {  // Exposure: versioned descriptor, stops as 'Exps'.
            std::memcpy(keyOut, "expA", 4);
            out = versionedDescriptor(
                "expA",
                {{"Exps", l.adjustmentParams[0]}, {"Ofst", 0.0}, {"Gmm ", 1.0}});
            break;
        }
        case 5: {  // Vibrance: versioned descriptor, 'Vibr' in -100..100.
            std::memcpy(keyOut, "vibA", 4);
            out = versionedDescriptor(
                "vibA", {{"Vibr", l.adjustmentParams[0] * 100.0},
                         {"Strt", 0.0}});
            break;
        }
        default:
            break;
    }
    return out;
}

// Reads one layer record (rect, channels + pixel data, blend/opacity/flags,
// extra data with mask/blending-ranges/name/'luni'/'lsct') from `in`, which is
// bounded by `limit`. Returns false on any structural error.
bool isPsbLongKey(const std::uint8_t* key) {
    static const char* const kLong[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16",
                                        "Mt32", "Mtrn", "Alph", "FMsk", "lnk2",
                                        "FEid", "FXid", "PxSD"};
    for (const char* k : kLong)
        if (std::memcmp(key, k, 4) == 0) return true;
    return false;
}

bool isValidBlockSig(const std::uint8_t* sig) {
    return sig && (std::memcmp(sig, "8BIM", 4) == 0 ||
                   std::memcmp(sig, "8B64", 4) == 0);
}

bool readLayerRecord(Reader& in, std::size_t limit, ParsedLayer& out,
                     bool isPsb = false) {
    const std::uint32_t top = in.u32be();
    const std::uint32_t left = in.u32be();
    const std::uint32_t bottom = in.u32be();
    const std::uint32_t right = in.u32be();
    if (!in.ok || in.pos > limit) return false;

    const std::uint64_t w =
        right > left ? static_cast<std::uint64_t>(right) - left : 0;
    const std::uint64_t h =
        bottom > top ? static_cast<std::uint64_t>(bottom) - top : 0;
    PsdLayerFile& file = out.file;
    file.left = left;
    file.top = top;
    file.width = static_cast<std::uint32_t>(w);
    file.height = static_cast<std::uint32_t>(h);

    const std::uint16_t nCh = in.u16be();
    if (!in.ok || in.pos > limit) return false;
    if (nCh > 16) return false;

    out.chans.clear();
    out.chans.reserve(nCh);
    std::uint64_t chanTotal = 0;
    for (std::uint16_t c = 0; c < nCh; ++c) {
        ParsedLayer::Chan ch;
        ch.id = static_cast<std::int16_t>(in.u16be());
        if (isPsb) {
            ch.len = in.u64be();
        } else {
            ch.len = in.u32be();
        }
        chanTotal += ch.len;
        if (!in.ok || in.pos > limit || chanTotal > (1u << 31)) return false;
        out.chans.push_back(ch);
    }

    // Blend signature + key, opacity, clipping, flags, filler, extra data.
    const std::uint8_t* blendSig = in.take(4);
    const std::uint8_t* blendKey = in.take(4);
    const std::uint8_t opacity = in.u8();
    const std::uint8_t clipping = in.u8();
    const std::uint8_t flags = in.u8();
    in.skip(1);  // filler
    const std::uint32_t extraLen = in.u32be();
    if (!in.ok || !isValidBlockSig(blendSig) ||
        in.pos + extraLen > limit) {
        return false;
    }

    file.opacity = opacity;
    file.clipped = clipping != 0;
    file.visible = (flags & 0x02) != 0;
    file.blend = blendKey ? blendNameFromKey(blendKey) : "Normal";

    // Extra data: layer mask, blending ranges, name, additional blocks.
    const std::size_t extraEnd = in.pos + extraLen;
    const std::uint32_t maskLen = in.u32be();
    if (!in.ok || in.pos + maskLen > extraEnd) return false;
    const std::uint8_t* maskData = in.take(maskLen);
    if (!maskData) return false;
    if (!parseMaskRecord(maskData, maskLen, file)) return false;
    const std::uint32_t rangesLen = in.u32be();
    if (!in.skip(rangesLen)) return false;
    if (!in.ok || in.pos > extraEnd) return false;

    // Pascal layer name (Latin-1 bytes; 'luni' overrides). Padded to a
    // multiple of 4 bytes including the length byte.
    std::string name;
    const std::uint8_t nameLen = in.u8();
    if (!in.ok || in.pos + nameLen > extraEnd) return false;
    if (nameLen > 0) {
        const std::uint8_t* np = in.take(nameLen);
        if (!np) return false;
        name.assign(reinterpret_cast<const char*>(np), nameLen);
    }
    const std::uint32_t namePad = (4u - (1u + nameLen) % 4u) % 4u;
    if (namePad) in.skip(namePad);

    // Additional layer-info blocks: 'luni' (unicode name) and 'lsct' (section
    // divider / group markers) are used; adjustment payloads are captured;
    // everything else is preserved verbatim. The empty 'norm' trailer that
    // follows an 'lsct' divider is deterministic (the emitter regenerates
    // it), so it is never captured — otherwise it would double on re-encode.
    bool prevWasLsct = false;
    while (in.ok && in.pos + 12 <= extraEnd) {
        const std::uint8_t* sig = in.take(4);
        const std::uint8_t* key = in.take(4);
        if (!sig || !key) break;
        // PSB uses 8-byte lengths for the long keys (LMsk/Lr16/...); the
        // block body follows, 2-byte padded inside layer records.
        std::uint64_t blkLen64 = 0;
        if (isPsb && isPsbLongKey(key)) {
            // 8-byte big-endian length for the long keys in PSB.
            if (in.pos + 8 > extraEnd) break;
            const std::uint32_t hi = in.u32be();
            const std::uint32_t lo = in.u32be();
            if (!in.ok) break;
            blkLen64 = (static_cast<std::uint64_t>(hi) << 32) | lo;
        } else {
            if (in.pos + 4 > extraEnd) break;
            blkLen64 = in.u32be();
        }
        if (!in.ok || blkLen64 > extraEnd - in.pos) break;
        const std::size_t blkLen = static_cast<std::size_t>(blkLen64);
        const std::uint8_t* data = in.take(blkLen);
        if (!data) break;
        // Inside layer records, additional-info blocks are 2-byte padded.
        // Kept verbatim for preserved blocks; other branches ignore it.
        std::vector<std::uint8_t> padBytes;
        if ((blkLen & 1u) && in.pos < extraEnd) {
            const std::uint8_t* pp = in.take(1);
            if (pp) padBytes.assign(pp, pp + 1);
        }
        const bool isLsct = isValidBlockSig(sig) && std::memcmp(key, "lsct", 4) == 0;
        if (isValidBlockSig(sig)) {
            if (std::memcmp(key, "luni", 4) == 0) {
                if (blkLen >= 4) {
                    const std::uint32_t n = (data[0] << 24) | (data[1] << 16) |
                                            (data[2] << 8) | data[3];
                    if (n <= 4096 && blkLen >= 4u + 2u * n) {
                        const std::string u = utf16beToUtf8(data + 4, 2u * n);
                        if (!u.empty()) name = u;
                    }
                }
            } else if (isLsct) {
                if (blkLen >= 4) {
                    const std::uint32_t t = (data[0] << 24) | (data[1] << 16) |
                                            (data[2] << 8) | data[3];
                    if (t <= 3) out.lsct = static_cast<int>(t);
                }
            } else if (!out.hasAdjKey && isAdjustmentKey(key) &&
                       blkLen <= (1u << 16)) {
                out.hasAdjKey = true;
                std::memcpy(out.adjKey, key, 4);
                out.adjData.assign(data, data + blkLen);
            } else if (blkLen <= (32u << 20) &&
                       !(prevWasLsct && blkLen == 0 &&
                         std::memcmp(key, "norm", 4) == 0)) {
                ParsedLayer::RawBlock rb;
                std::memcpy(rb.sig, sig, 4);
                std::memcpy(rb.key, key, 4);
                rb.data.assign(data, data + blkLen);
                rb.padding = std::move(padBytes);
                out.rawBlocks.push_back(std::move(rb));
            }
        }
        prevWasLsct = isLsct;
    }
    if (!in.ok || in.pos > extraEnd) return false;

    file.name = name.empty() ? "Layer" : name;
    return true;
}

// Stored (spec-inverted: 65535 = no ink) CMYK samples -> straight RGBA16.
// Profiled when `cmyk` is live, else the exact legacy integer math, so
// unprofiled output is bit-identical to before. Shared by the flat
// composite and the layered path so the two can never disagree again.
void convertCmykStored(const pittore::color::CmykToSrgb* cmyk,
                       std::uint32_t cs, std::uint32_t ms, std::uint32_t ys,
                       std::uint32_t ks, std::uint16_t* rgb /*[3]*/) {
    if (cmyk && cmyk->valid()) {
        constexpr float inv = 1.0f / 65535.0f;
        float out[3] = {0.0f, 0.0f, 0.0f};
        cmyk->convert(1.0f - cs * inv, 1.0f - ms * inv, 1.0f - ys * inv,
                      1.0f - ks * inv, out);
        rgb[0] = clamp16(out[0]);
        rgb[1] = clamp16(out[1]);
        rgb[2] = clamp16(out[2]);
        return;
    }
    rgb[0] = static_cast<std::uint16_t>(cs * ks / 65535u);
    rgb[1] = static_cast<std::uint16_t>(ms * ks / 65535u);
    rgb[2] = static_cast<std::uint16_t>(ys * ks / 65535u);
}

// Fills `file.rgba` (straight RGBA16, opaque when alpha is missing) from the
// decoded channel planes. `colorMode` is the document mode (1 gray, 3 RGB,
// 4 CMYK stored inverted, 9 Lab); layers are converted to RGB on import.
// When `cmyk` is live the CMYK conversion is profiled, otherwise the exact
// legacy integer math below, so unprofiled output is bit-identical.
void assembleLayerRgba(PsdLayerFile& file, int colorMode,
                       const std::vector<std::vector<std::uint16_t>>& planes,
                       const pittore::color::CmykToSrgb* cmyk) {
    const std::uint64_t n =
        static_cast<std::uint64_t>(file.width) * file.height;
    if (n == 0) {
        file.rgba.clear();
        return;
    }
    file.rgba.assign(static_cast<std::size_t>(n) * 4, 65535);
    const bool gray = colorMode == 1;
    if (colorMode == 4) {  // CMYK: C,M,Y,K in 0..3, alpha in 4 when present
        file.cmyk.resize(static_cast<std::size_t>(n) * 4);
        for (std::uint64_t i = 0; i < n; ++i) {
            const std::uint32_t c = planes[0].size() > i ? planes[0][i] : 65535;
            const std::uint32_t m = planes[1].size() > i ? planes[1][i] : 65535;
            const std::uint32_t y = planes[2].size() > i ? planes[2][i] : 65535;
            const std::uint32_t k = planes[3].size() > i ? planes[3][i] : 65535;
            const std::uint16_t a = planes.size() > 4 && planes[4].size() > i
                                        ? planes[4][i]
                                        : 65535;
            std::uint16_t rgb[3] = {0, 0, 0};
            convertCmykStored(cmyk, c, m, y, k, rgb);
            file.cmyk[i * 4 + 0] = static_cast<std::uint16_t>(65535 - c);
            file.cmyk[i * 4 + 1] = static_cast<std::uint16_t>(65535 - m);
            file.cmyk[i * 4 + 2] = static_cast<std::uint16_t>(65535 - y);
            file.cmyk[i * 4 + 3] = static_cast<std::uint16_t>(65535 - k);
            file.rgba[i * 4 + 0] = rgb[0];
            file.rgba[i * 4 + 1] = rgb[1];
            file.rgba[i * 4 + 2] = rgb[2];
            file.rgba[i * 4 + 3] = a;
        }
        return;
    }
    if (colorMode == 9) {  // Lab → sRGB per pixel
        for (std::uint64_t i = 0; i < n; ++i) {
            const std::uint16_t L = planes[0].size() > i ? planes[0][i] : 0;
            const std::uint16_t a = planes[1].size() > i ? planes[1][i] : 32768;
            const std::uint16_t b = planes[2].size() > i ? planes[2][i] : 32768;
            const std::uint16_t al = planes[3].size() > i ? planes[3][i] : 65535;
            double r, g, bl;
            labToRgb(L, a, b, &r, &g, &bl);
            file.rgba[i * 4 + 0] = clamp16(r);
            file.rgba[i * 4 + 1] = clamp16(g);
            file.rgba[i * 4 + 2] = clamp16(bl);
            file.rgba[i * 4 + 3] = al;
        }
        return;
    }
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint16_t v = gray ? (planes[0].size() > i ? planes[0][i] : 0) : 0;
        const std::uint16_t r =
            gray ? v : (planes[0].size() > i ? planes[0][i] : 0);
        const std::uint16_t g =
            gray ? v : (planes[1].size() > i ? planes[1][i] : 0);
        const std::uint16_t b =
            gray ? v : (planes[2].size() > i ? planes[2][i] : 0);
        const std::uint16_t a = planes[3].size() > i ? planes[3][i] : 65535;
        file.rgba[i * 4 + 0] = r;
        file.rgba[i * 4 + 1] = g;
        file.rgba[i * 4 + 2] = b;
        file.rgba[i * 4 + 3] = a;
    }
}

// Coverage of a user mask at a document pixel. Pixels outside the stored
// mask bounds reveal (1), matching the .af mask behaviour and the UI's
// layer-native mask conversion.
double psdMaskCoverage(const PsdLayerFile& file, std::uint32_t dx,
                       std::uint32_t dy) {
    if (!file.hasMask || !file.maskEnabled || file.mask.empty() ||
        file.maskWidth == 0 || file.maskHeight == 0)
        return 1.0;
    std::int64_t mx, my;
    if (file.maskRelative) {
        mx = static_cast<std::int64_t>(dx) -
             static_cast<std::int64_t>(file.left) - file.maskLeft;
        my = static_cast<std::int64_t>(dy) -
             static_cast<std::int64_t>(file.top) - file.maskTop;
    } else {
        mx = static_cast<std::int64_t>(dx) - file.maskLeft;
        my = static_cast<std::int64_t>(dy) - file.maskTop;
    }
    if (mx < 0 || my < 0 || static_cast<std::uint64_t>(mx) >= file.maskWidth ||
        static_cast<std::uint64_t>(my) >= file.maskHeight)
        return 1.0;
    const std::size_t i = static_cast<std::size_t>(my) * file.maskWidth +
                          static_cast<std::size_t>(mx);
    return static_cast<double>(file.mask[i]) / 65535.0;
}

// Source-over flatten used for the document composite written by
// psdEncodeLayers. Blend modes beyond Normal are approximated (a documented
// simplification); layer opacity and visibility are honoured.
void compositeLayers(const PsdLayersDoc& doc, std::vector<std::uint16_t>& out) {
    const std::size_t q = static_cast<std::size_t>(doc.width) * doc.height;
    out.assign(q * 4, 0);
    if (q == 0) return;
    constexpr double kMax = 65535.0;
    std::vector<double> dr(q, 0.0), dg(q, 0.0), db(q, 0.0), da(q, 0.0);
    std::vector<double> clip(q, 0.0);
    for (const PsdLayerFile& l : doc.layers) {
        if (l.isGroup || !l.visible) continue;
        if (l.isAdjustment) {
            // Live adjustment: transform the composite-so-far with the same
            // shared math as the engine, then blend with the layer's own
            // mode at mask×fold×clip alpha. Adjustments never establish clip
            // coverage (neutral, like the app compositor). Stand-ins
            // (kind 0) are no-ops.
            if (l.adjustmentKind == 0) continue;
            const auto kind = static_cast<pittore::compute::AdjustmentKind>(
                l.adjustmentKind);
            const auto mode =
                pittore::compute::blend::from_display_name(l.blend.c_str());
            const double fold = static_cast<double>(l.opacity) / 255.0;
            // Aux tables mirror the live compositor: 3x256 (R/G/B) for
            // Curves, 256 for Levels, none otherwise. adjust::apply samples
            // the table when non-null and evaluates params directly when
            // null, so both paths stay exact.
            float lut[768];
            const float* lutPtr = nullptr;
            if (kind == pittore::compute::AdjustmentKind::Levels) {
                pittore::buildLevelsLUT(l.adjustmentParams, lut);
                lutPtr = lut;
            }
            if (kind == pittore::compute::AdjustmentKind::Curves) {
                float master[256], ch[256];
                pittore::buildCurveLUT(l.adjustmentCurve, master);
                const std::vector<std::pair<double, double>>* chs[3] = {
                    &l.adjustmentCurveR, &l.adjustmentCurveG,
                    &l.adjustmentCurveB};
                for (int c = 0; c < 3; ++c) {
                    pittore::buildCurveLUT(*chs[c], ch);
                    float* dst = lut + 256 * c;
                    for (int i = 0; i < 256; ++i) {
                        const float v = std::clamp(ch[i], 0.0f, 1.0f);
                        dst[i] = master[std::clamp(
                            static_cast<int>(v * 255.0f + 0.5f), 0, 255)];
                    }
                }
                lutPtr = lut;
            }
            // Full canvas, not the stored rect: the format writes fill and
            // adjustment layers with null bounds (ImageMagick also rejects a
            // 0-channel record with a non-empty rect), so the rect is only a
            // hint. Coverage still comes from the mask descriptor. Rows are
            // independent (per-pixel accumulators), so this parallelizes.
            pittore::core::parallel_rows(
                doc.height, [&](std::uint32_t y0, std::uint32_t y1) {
                    for (std::uint32_t dy = y0; dy < y1; ++dy) {
                        for (std::uint32_t dx = 0; dx < doc.width; ++dx) {
                            const std::size_t didx =
                                static_cast<std::size_t>(dy) * doc.width + dx;
                            double cov = fold * psdMaskCoverage(l, dx, dy);
                            if (l.clipped) cov *= clip[didx];
                            if (cov <= 0.0 || da[didx] <= 0.0) continue;
                            float ar, ag, ab;
                            pittore::compute::adjust::apply(
                                kind, l.adjustmentParams, lutPtr,
                                static_cast<float>(dr[didx]),
                                static_cast<float>(dg[didx]),
                                static_cast<float>(db[didx]), ar, ag, ab);
                            float or_, og, ob, oa;
                            pittore::compute::blend::pixel(
                                mode, ar, ag, ab,
                                static_cast<float>(da[didx] * cov),
                                static_cast<float>(dr[didx]),
                                static_cast<float>(dg[didx]),
                                static_cast<float>(db[didx]),
                                static_cast<float>(da[didx]),
                                static_cast<int>(dx), static_cast<int>(dy),
                                or_, og, ob, oa);
                            dr[didx] = or_;
                            dg[didx] = og;
                            db[didx] = ob;
                            da[didx] = oa;
                        }
                    }
                });
            continue;
        }
        if (l.rgba.empty()) continue;
        const double srcOp = static_cast<double>(l.opacity) / 255.0;
        pittore::core::parallel_rows(
            l.height, [&](std::uint32_t y0, std::uint32_t y1) {
                for (std::uint32_t y = y0; y < y1; ++y) {
                    const std::uint32_t dy = l.top + y;
                    if (dy >= doc.height) continue;
                    for (std::uint32_t x = 0; x < l.width; ++x) {
                        const std::uint32_t dx = l.left + x;
                        if (dx >= doc.width) continue;
                        const std::size_t sidx =
                            (static_cast<std::size_t>(y) * l.width + x) * 4;
                        const std::size_t didx =
                            static_cast<std::size_t>(dy) * doc.width + dx;
                        const double sr = l.rgba[sidx] / kMax;
                        const double sg = l.rgba[sidx + 1] / kMax;
                        const double sb = l.rgba[sidx + 2] / kMax;
                        double sa = l.rgba[sidx + 3] / kMax * srcOp;
                        sa *= psdMaskCoverage(l, dx, dy);
                        if (l.clipped) sa *= clip[didx];
                        if (sa <= 0.0) continue;
                        const double od = da[didx];
                        const double oa = sa + od * (1.0 - sa);
                        if (oa <= 0.0) continue;
                        dr[didx] =
                            (sr * sa + dr[didx] * od * (1.0 - sa)) / oa;
                        dg[didx] =
                            (sg * sa + dg[didx] * od * (1.0 - sa)) / oa;
                        db[didx] =
                            (sb * sa + db[didx] * od * (1.0 - sa)) / oa;
                        da[didx] = oa;
                        if (!l.clipped) clip[didx] = sa;
                    }
                }
            });
    }
    pittore::core::parallel_rows(
        static_cast<std::uint32_t>(q), [&](std::uint32_t lo, std::uint32_t hi) {
            for (std::size_t i = lo; i < hi; ++i) {
                out[i * 4 + 0] = clamp16(dr[i]);
                out[i * 4 + 1] = clamp16(dg[i]);
                out[i * 4 + 2] = clamp16(db[i]);
                out[i * 4 + 3] = clamp16(da[i]);
            }
        });
}

// RLE-compressed block bytes for one layer channel of an interleaved RGBA16
// buffer (`rgba` holds width*height*4 samples; `ch` ∈ 0..3 selects the plane).
std::vector<std::uint8_t> rleChannelBlock(const std::uint16_t* rgba, int w, int h,
                                          int depth, int ch, int stride = 4) {
    const bool is16 = depth >= 16;
    // Row-parallel PackBits: rows are independent; indexed assignment keeps
    // byte-identical output.
    std::vector<std::vector<std::uint8_t>> packed_(static_cast<std::size_t>(h));
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + 2 * h + static_cast<std::size_t>(w) * h * (is16 ? 2 : 1));
    append16(bytes, 1);  // compression: RLE
    pittore::core::parallel_rows(
        static_cast<std::uint32_t>(h), [&](std::uint32_t lo, std::uint32_t hi) {
            std::vector<std::uint8_t> raw;
            raw.reserve(static_cast<std::size_t>(w) * (is16 ? 2 : 1));
            for (std::uint32_t y = lo; y < hi; ++y) {
                raw.clear();
                const std::uint16_t* src =
                    rgba + (static_cast<std::size_t>(y) * w + 0) * stride + ch;
                for (int x = 0; x < w; ++x, src += stride) {
                    if (is16) {
                        const std::uint16_t v = *src;
                        raw.push_back(static_cast<std::uint8_t>(v >> 8));
                        raw.push_back(static_cast<std::uint8_t>(v & 0xff));
                    } else {
                        raw.push_back(static_cast<std::uint8_t>(*src >> 8));
                    }
                }
                packed_[y] = packBitsRow(raw.data(), raw.size());
            }
        });
    for (int y = 0; y < h; ++y)
        append16(bytes, static_cast<std::uint16_t>(packed_[y].size()));
    for (const auto& row : packed_) bytes.insert(bytes.end(), row.begin(), row.end());
    return bytes;
}

// RLE-compressed block bytes for one greyscale layer channel (a user mask):
// `gray` holds width*height 16-bit coverage samples.
std::vector<std::uint8_t> rleGrayChannelBlock(const std::uint16_t* gray, int w,
                                              int h, int depth) {
    const bool is16 = depth >= 16;
    std::vector<std::vector<std::uint8_t>> packed_(static_cast<std::size_t>(h));
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + 2 * h + static_cast<std::size_t>(w) * h * (is16 ? 2 : 1));
    append16(bytes, 1);  // compression: RLE
    pittore::core::parallel_rows(
        static_cast<std::uint32_t>(h), [&](std::uint32_t lo, std::uint32_t hi) {
            std::vector<std::uint8_t> raw;
            raw.reserve(static_cast<std::size_t>(w) * (is16 ? 2 : 1));
            for (std::uint32_t y = lo; y < hi; ++y) {
                raw.clear();
                const std::uint16_t* src = gray + static_cast<std::size_t>(y) * w;
                for (int x = 0; x < w; ++x, ++src) {
                    if (is16) {
                        raw.push_back(static_cast<std::uint8_t>(*src >> 8));
                        raw.push_back(static_cast<std::uint8_t>(*src & 0xff));
                    } else {
                        raw.push_back(static_cast<std::uint8_t>(*src >> 8));
                    }
                }
                packed_[y] = packBitsRow(raw.data(), raw.size());
            }
        });
    for (int y = 0; y < h; ++y)
        append16(bytes, static_cast<std::uint16_t>(packed_[y].size()));
    for (const auto& row : packed_) bytes.insert(bytes.end(), row.begin(), row.end());
    return bytes;
}

// PSD stores the CMYK complement (full ink = 0 in the file). Inverts the
// four ink planes of a separated buffer in place; alpha (the fifth sample)
// rides through untouched.
void invertInkPlanes(std::vector<std::uint16_t>& px, int stride, int inks) {
    const std::size_t n = px.size() / static_cast<std::size_t>(stride);
    for (std::size_t i = 0; i < n; ++i)
        for (int c = 0; c < inks; ++c)
            px[i * static_cast<std::size_t>(stride) + c] =
                static_cast<std::uint16_t>(
                    65535 - px[i * static_cast<std::size_t>(stride) + c]);
}

// Image-resource section payload carrying one ICC profile: '8BIM', spec
// resource id 0x040F, empty even-padded Pascal name, big-endian length,
// profile bytes (even-padded). Empty/absent profile -> no section at all.
std::vector<std::uint8_t> buildIccResource(const std::uint8_t* icc,
                                           std::size_t n) {
    std::vector<std::uint8_t> r;
    if (!icc || n == 0) return r;
    r.insert(r.end(), {'8', 'B', 'I', 'M'});
    append16(r, 0x040F);
    r.push_back(0);  // empty Pascal name (length byte)...
    r.push_back(0);  // ...padded to even
    append32(r, static_cast<std::uint32_t>(n));
    r.insert(r.end(), icc, icc + n);
    if (n & 1u) r.push_back(0);
    return r;
}

// Composite image data (RLE) for the flattened document: compression word, an
// external channel-major row-length table, then packed rows — the layout the
// composite decoder expects. `channels` is both the plane count and the
// interleaved stride: 4 for RGBA, 5 for the CMYK composite (C,M,Y,K,A).
void appendCompositeRle(std::vector<std::uint8_t>& out, const std::uint16_t* rgba,
                        std::uint32_t w, std::uint32_t h, int depth,
                        int channels = 4) {
    const bool is16 = depth >= 16;
    std::vector<std::vector<std::uint8_t>> rows(static_cast<std::size_t>(channels) * h);
    pittore::core::parallel_rows(
        h, [&](std::uint32_t lo, std::uint32_t hi) {
            std::vector<std::uint8_t> raw;
            raw.reserve(static_cast<std::size_t>(w) * (is16 ? 2 : 1));
            for (int ch = 0; ch < channels; ++ch) {
                for (std::uint32_t y = lo; y < hi; ++y) {
                    raw.clear();
                    const std::uint16_t* src =
                        rgba + (static_cast<std::size_t>(y) * w) * channels + ch;
                    for (std::uint32_t x = 0; x < w; ++x, src += channels) {
                        if (is16) {
                            const std::uint16_t v = *src;
                            raw.push_back(static_cast<std::uint8_t>(v >> 8));
                            raw.push_back(static_cast<std::uint8_t>(v & 0xff));
                        } else {
                            raw.push_back(static_cast<std::uint8_t>(*src >> 8));
                        }
                    }
                    rows[static_cast<std::size_t>(ch) * h + y] =
                        packBitsRow(raw.data(), raw.size());
                }
            }
        });
    append16(out, 1);  // compression: RLE
    for (const auto& row : rows) append16(out, static_cast<std::uint16_t>(row.size()));
    for (const auto& row : rows) out.insert(out.end(), row.begin(), row.end());
}

// Raw-deflate `in` (no zlib header) to match the decoder's -MAX_WBITS
// inflate. Used for ZIP (compression 2) channel blocks: no prediction,
// no row table, just the tag + stream.
bool deflateRaw(const std::vector<std::uint8_t>& in,
                std::vector<std::uint8_t>& out) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    std::array<std::uint8_t, 32768> buf{};
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        rc = deflate(&zs, Z_FINISH);
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
    }
    deflateEnd(&zs);
    return rc == Z_STREAM_END;
}

std::vector<std::uint8_t> zipChannelBlock(const std::uint16_t* rgba, int w, int h,
                                          int depth, int ch, int stride = 4) {
    const bool is16 = depth >= 16;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(w) * h * (is16 ? 2 : 1));
    for (int y = 0; y < h; ++y) {
        const std::uint16_t* src =
            rgba + (static_cast<std::size_t>(y) * w + 0) * stride + ch;
        for (int x = 0; x < w; ++x, src += stride) {
            if (is16) {
                const std::uint16_t v = *src;
                raw.push_back(static_cast<std::uint8_t>(v >> 8));
                raw.push_back(static_cast<std::uint8_t>(v & 0xff));
            } else {
                raw.push_back(static_cast<std::uint8_t>(*src >> 8));
            }
        }
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + raw.size() / 2);
    append16(bytes, 2);  // compression: ZIP without prediction
    std::vector<std::uint8_t> comp;
    if (deflateRaw(raw, comp))
        bytes.insert(bytes.end(), comp.begin(), comp.end());
    return bytes;
}

// ZIP-compressed block bytes for one greyscale layer channel (a user mask).
std::vector<std::uint8_t> zipGrayChannelBlock(const std::uint16_t* gray, int w,
                                              int h, int depth) {
    const bool is16 = depth >= 16;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(w) * h * (is16 ? 2 : 1));
    for (int y = 0; y < h; ++y) {
        const std::uint16_t* src = gray + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x, ++src) {
            if (is16) {
                raw.push_back(static_cast<std::uint8_t>(*src >> 8));
                raw.push_back(static_cast<std::uint8_t>(*src & 0xff));
            } else {
                raw.push_back(static_cast<std::uint8_t>(*src >> 8));
            }
        }
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(2 + raw.size() / 2);
    append16(bytes, 2);  // compression: ZIP without prediction
    std::vector<std::uint8_t> comp;
    if (deflateRaw(raw, comp))
        bytes.insert(bytes.end(), comp.begin(), comp.end());
    return bytes;
}

// Composite image data (ZIP without prediction) for the flattened document:
// compression word + one raw-deflate stream over all channel planes.
void appendCompositeZip(std::vector<std::uint8_t>& out, const std::uint16_t* rgba,
                        std::uint32_t w, std::uint32_t h, int depth,
                        int channels = 4) {
    const bool is16 = depth >= 16;
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(channels) * w * h * (is16 ? 2 : 1));
    for (int ch = 0; ch < channels; ++ch) {
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint16_t* src =
                rgba + (static_cast<std::size_t>(y) * w) * channels + ch;
            for (std::uint32_t x = 0; x < w; ++x, src += channels) {
                if (is16) {
                    const std::uint16_t v = *src;
                    raw.push_back(static_cast<std::uint8_t>(v >> 8));
                    raw.push_back(static_cast<std::uint8_t>(v & 0xff));
                } else {
                    raw.push_back(static_cast<std::uint8_t>(*src >> 8));
                }
            }
        }
    }
    append16(out, 2);  // compression: ZIP without prediction
    std::vector<std::uint8_t> comp;
    if (deflateRaw(raw, comp))
        out.insert(out.end(), comp.begin(), comp.end());
}

// A record waiting to be serialized: pixel layers carry their R,G,B,A channel
// image blocks (each block begins with its own 16-bit compression tag);
// adjustment layers carry an optional mask block plus their adjustment
// payload (emitted as the additional-info block); group, empty-layer and
// folder-end records carry none.
struct OutRec {
    const PsdLayerFile* layer = nullptr;
    bool endMarker = false;
    bool hasMask = false;
    bool hasAdj = false;
    char adjKey[4] = {};
    std::vector<std::uint8_t> adjData;
    std::vector<std::vector<std::uint8_t>> blocks;
};

// Writes one PSD layer record's metadata: rect, channel info (ids + block
// lengths), blend signature/key, opacity, clipping, flags, filler and extra
// data. Channel PIXEL data is not part of the record - the caller emits every
// record first and only then the channel blocks, whose sizes are written into
// the channel-info fields above. `endMarker` emits the folder-end divider
// used to close a group (lsct type 3, no pixels).
void emitLayerRecord(std::vector<std::uint8_t>& out, const OutRec& r,
                     bool cmyk) {
    const PsdLayerFile* l = r.layer;
    const bool isAdj = l && l->isAdjustment && !r.endMarker;
    if (r.endMarker || !l || l->isGroup || l->rgba.empty() || l->width == 0 ||
        l->height == 0) {
        if (isAdj) {
            // Adjustment rows keep their stored rect (full-doc from our
            // exporter) with zero pixel channels, or one mask channel.
            append32(out, l->top);
            append32(out, l->left);
            append32(out, l->top + l->height);
            append32(out, l->left + l->width);
            if (r.hasMask) {
                append16(out, 1);
                append16(out, static_cast<std::uint16_t>(-2));
                append32(out, static_cast<std::uint32_t>(r.blocks[0].size()));
            } else {
                append16(out, 0);  // no channels
            }
        } else {
            for (int i = 0; i < 4; ++i) append32(out, 0);  // zero rect
            append16(out, 0);                              // no channels
        }
    } else {
        append32(out, l->top);
        append32(out, l->left);
        append32(out, l->top + l->height);
        append32(out, l->left + l->width);
        // CMYK: C,M,Y,K,A[,mask]; RGB: R,G,B,A[,mask]. Transparency keeps
        // id -1 and the user mask -2 in both.
        const int idsRgb[6] = {0, 1, 2, -1, -2, 0};
        const int idsCmyk[6] = {0, 1, 2, 3, -1, -2};
        const int* ids = cmyk ? idsCmyk : idsRgb;
        const int colorPlanes = cmyk ? 4 : 3;
        const int channelCount = colorPlanes + 1 + (r.hasMask ? 1 : 0);
        append16(out, static_cast<std::uint16_t>(channelCount));
        for (int c = 0; c < channelCount; ++c) {
            append16(out, static_cast<std::uint16_t>(ids[c]));
            append32(out, static_cast<std::uint32_t>(r.blocks[c].size()));
        }
    }

    out.insert(out.end(), {'8', 'B', 'I', 'M'});
    const std::string blendKey =
        l ? blendKeyFromName(l->blend) : std::string("norm");
    out.insert(out.end(), blendKey.begin(), blendKey.end());
    out.push_back(l ? l->opacity : 255u);
    out.push_back(l && l->clipped ? 1 : 0);
    out.push_back(l && l->visible ? 0x02 : 0x00);
    out.push_back(0);  // filler

    // Extra data: mask (a real descriptor when the layer carries a user
    // mask), blending ranges (none), name, additional blocks.
    const std::string name = l ? l->name : std::string();
    std::vector<std::uint8_t> extra;
    if (l && r.hasMask && l->maskWidth > 0 && l->maskHeight > 0) {
        std::vector<std::uint8_t> maskData;
        maskData.reserve(20);
        const std::uint32_t mleft =
            static_cast<std::uint32_t>(l->maskLeft);
        const std::uint32_t mtop = static_cast<std::uint32_t>(l->maskTop);
        const std::uint32_t mright = mleft + l->maskWidth;
        const std::uint32_t mbottom = mtop + l->maskHeight;
        append32(maskData, mtop);
        append32(maskData, mleft);
        append32(maskData, mbottom);
        append32(maskData, mright);
        maskData.push_back(0);  // default color
        std::uint8_t maskFlags = 0;
        if (l->maskRelative) maskFlags |= 0x01;
        if (!l->maskEnabled) maskFlags |= 0x02;
        maskData.push_back(maskFlags);
        maskData.push_back(0);  // padding
        maskData.push_back(0);  // padding
        append32(extra, static_cast<std::uint32_t>(maskData.size()));
        extra.insert(extra.end(), maskData.begin(), maskData.end());
    } else {
        append32(extra, 0);  // layer mask data length
    }
    append32(extra, 0);  // blending ranges length
    std::string latin = name;
    for (char& c : latin)
        if (static_cast<unsigned char>(c) >= 0x80) c = '?';
    if (latin.size() > 255) latin.resize(255);
    extra.push_back(static_cast<std::uint8_t>(latin.size()));
    extra.insert(extra.end(), latin.begin(), latin.end());
    // Pascal name is padded to a multiple of 4 bytes (length byte included);
    // extra so far is [mask len][ranges len][name], all 4/4/1+name aligned.
    while (extra.size() & 3u) extra.push_back(0);

    const std::vector<std::uint8_t> uni = utf8ToUtf16be(name);
    if (uni.size() / 2 <= 4096 && !uni.empty()) {
        extra.insert(extra.end(), {'8', 'B', 'I', 'M', 'l', 'u', 'n', 'i'});
        append32(extra, static_cast<std::uint32_t>(4 + uni.size()));
        append32(extra, static_cast<std::uint32_t>(uni.size() / 2));
        extra.insert(extra.end(), uni.begin(), uni.end());
    }
    // Live adjustment payload, then any preserved foreign blocks verbatim
    // (each 2-byte padded like all in-record additional info; the stored
    // signature and pad bytes ride along so 8B64/odd-pad inputs re-emit
    // exactly).
    auto emitRaw = [&](const char* sig, const char* key,
                       const std::vector<std::uint8_t>& data,
                       const std::vector<std::uint8_t>& padding) {
        extra.insert(extra.end(), sig, sig + 4);
        extra.insert(extra.end(), key, key + 4);
        append32(extra, static_cast<std::uint32_t>(data.size()));
        extra.insert(extra.end(), data.begin(), data.end());
        if (!padding.empty()) {
            extra.insert(extra.end(), padding.begin(), padding.end());
        } else if (data.size() & 1u) {
            extra.push_back(0);
        }
    };
    auto emitAdj = [&](const char* key, const std::vector<std::uint8_t>& data) {
        emitRaw("8BIM", key, data, {});
    };
    if (r.hasAdj && !r.endMarker) emitAdj(r.adjKey, r.adjData);
    if (l && !r.endMarker) {
        for (const auto& rb : l->rawBlocks) emitRaw(rb.sig, rb.key, rb.data, rb.padding);
    }
    if (r.endMarker || (l && l->isGroup)) {
        extra.insert(extra.end(), {'8', 'B', 'I', 'M', 'l', 's', 'c', 't'});
        append32(extra, 8);  // type (4) + signature '8BIM' (4)
        const std::uint32_t type =
            r.endMarker ? 3u : (l->groupExpanded ? 1u : 2u);
        append32(extra, type);
        extra.insert(extra.end(), {'8', 'B', 'I', 'M'});
        // The writer usually follows the divider with an (empty) normal-blend
        // key block; it too needs the 4-byte length so parsers stay aligned.
        // Skip it when an identical preserved block already rode along above
        // (it would otherwise double on every re-encode).
        bool haveNorm = false;
        if (l) {
            for (const auto& rb : l->rawBlocks) {
                if (std::memcmp(rb.key, "norm", 4) == 0 && rb.data.empty()) {
                    haveNorm = true;
                    break;
                }
            }
        }
        if (!haveNorm) {
            extra.insert(extra.end(), {'8', 'B', 'I', 'M', 'n', 'o', 'r', 'm'});
            append32(extra, 0);
        }
    }
    append32(out, static_cast<std::uint32_t>(extra.size()));
    out.insert(out.end(), extra.begin(), extra.end());
}

}  // namespace

std::optional<PsdImage> psdDecode(const std::vector<std::uint8_t>& data,
                                  std::string* error,
                                  const std::vector<std::uint8_t>* embeddedIcc) {
    const pittore::color::CmykToSrgb conv(
        embeddedIcc && !embeddedIcc->empty() ? embeddedIcc->data() : nullptr,
        embeddedIcc ? embeddedIcc->size() : 0);
    Reader in(data);
    if (!in.ok || in.remaining() < 26) return fail(error, "PSD file too short");

    // --- header -----------------------------------------------------------
    const std::uint8_t* sig = in.take(4);
    if (!sig || std::memcmp(sig, "8BPS", 4) != 0)
        return fail(error, "not a PSD file (bad signature)");
    const std::uint16_t version = in.u16be();
    if (version != 1 && version != 2)
        return fail(error, "unsupported PSD version");
    const bool isPsb = (version == 2);
    if (!in.skip(6)) return fail(error, "truncated PSD header");
    const std::uint16_t channels = in.u16be();
    const std::uint32_t height = in.u32be();
    const std::uint32_t width = in.u32be();
    const std::uint16_t depth = in.u16be();
    const std::uint16_t colorMode = in.u16be();
    if (!in.ok || width == 0 || height == 0)
        return fail(error, "truncated PSD header");
    if (depth != 8 && depth != 16 && depth != 32)
        return fail(error, "unsupported PSD bit depth");
    if (channels == 0 || channels > 56)
        return fail(error, "invalid PSD channel count");
    if (width > 300000u || height > 300000u)
        return fail(error, "PSD dimensions too large");
    if (!isPsb && (width > 30000u || height > 30000u))
        return fail(error, "PSD dimensions exceed 30000; use PSB");

    const std::uint64_t q64 = static_cast<std::uint64_t>(width) * height;
    if (q64 > 268435456u)  // sanity cap ~268 MP
        return fail(error, "PSD dimensions too large");
    const std::uint32_t q = static_cast<std::uint32_t>(q64);

    // --- color mode data / image resources / layer & mask info ------------
    // colorModeData and resources stay u32 in both versions; only the
    // layer&mask section grows to u64 in PSB.
    const std::uint32_t colorModeLen = in.u32be();
    if (!in.skip(colorModeLen)) return fail(error, "truncated color-mode data");
    const std::uint32_t resourcesLen = in.u32be();
    if (!in.skip(resourcesLen)) return fail(error, "truncated image resources");
    std::uint64_t layerMaskLen = 0;
    if (isPsb) {
        layerMaskLen = in.u64be();
    } else {
        layerMaskLen = in.u32be();
    }
    if (!in.ok) return fail(error, "truncated layer/mask info");
    if (layerMaskLen > in.remaining()) return fail(error, "truncated layer/mask info");
    if (!in.skip(static_cast<std::size_t>(layerMaskLen)))
        return fail(error, "truncated layer/mask info");

    // --- composite compression + planes -----------------------------------
    if (in.remaining() < 2) return fail(error, "missing PSD image data");
    const std::uint16_t compression = in.u16be();
    if (compression > 3)
        return fail(error, "unsupported PSD compression (only Raw, RLE and ZIP)");

    // For RLE the byte-count table (channels × rows) precedes the data:
    // u16 entries in PSD, u32 in PSB.
    std::vector<std::uint32_t> rowBytes;
    if (compression == 1) {
        const std::uint64_t entries = static_cast<std::uint64_t>(channels) * height;
        if (entries > (1u << 28)) return fail(error, "PSD dimensions too large");
        if (isPsb) {
            if (in.remaining() < entries * 4)
                return fail(error, "truncated RLE byte-count table");
            rowBytes.resize(static_cast<std::size_t>(entries));
            for (std::uint64_t i = 0; i < entries; ++i) rowBytes[i] = in.u32be();
        } else {
            if (in.remaining() < entries * 2)
                return fail(error, "truncated RLE byte-count table");
            rowBytes.resize(static_cast<std::size_t>(entries));
            for (std::uint64_t i = 0; i < entries; ++i) rowBytes[i] = in.u16be();
        }
    }

    const int bytesPerSample = depth == 32 ? 4 : (depth >= 16 ? 2 : 1);
    const std::uint32_t rawRow = (static_cast<std::uint32_t>(width * bytesPerSample) + 1u) & ~1u;

    // planebuf[ch][y*w + x] — one plane per channel.
    std::vector<std::vector<std::uint16_t>> planes(channels);
    for (auto& p : planes) p.resize(q);

    if (compression == 2 || compression == 3) {
        // Merged ZIP: one zlib stream over all planes (no per-row table,
        // no even padding). Layout is planes × height rows of width*bps.
        const std::size_t rowBytesZip = static_cast<std::size_t>(width) * bytesPerSample;
        const std::size_t expect =
            static_cast<std::size_t>(channels) * height * rowBytesZip;
        if (expect == 0 || expect > (1u << 30))
            return fail(error, "PSD dimensions too large");
        const std::size_t compLen = in.remaining();
        if (compLen == 0) return fail(error, "missing PSD image data");
        const std::uint8_t* comp = in.take(compLen);
        if (!comp) return fail(error, "truncated PSD image data");
        std::vector<std::uint8_t> inflated(expect);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
            return fail(error, "corrupt PSD channel data");
        zs.next_in = const_cast<Bytef*>(comp);
        zs.avail_in = static_cast<uInt>(compLen);
        zs.next_out = inflated.data();
        zs.avail_out = static_cast<uInt>(expect);
        const int zr = inflate(&zs, Z_FINISH);
        const bool zok = (zr == Z_STREAM_END && zs.total_out == expect);
        inflateEnd(&zs);
        if (!zok) return fail(error, "corrupt PSD channel data");
        if (compression == 3) {
            const std::size_t rows =
                static_cast<std::size_t>(channels) * height;
            std::vector<std::uint8_t> scratch;
            if (depth == 32) scratch.resize(rowBytesZip);
            for (std::size_t r = 0; r < rows; ++r) {
                std::uint8_t* rowp = inflated.data() + r * rowBytesZip;
                if (depth == 8) {
                    std::uint8_t acc = 0;
                    for (std::size_t x = 0; x < rowBytesZip; ++x) {
                        acc = static_cast<std::uint8_t>(acc + rowp[x]);
                        rowp[x] = acc;
                    }
                } else if (depth == 32) {
                    // 32-bit: byte-delta first, then unshuffle the four
                    // byte planes (MSB plane first), per the PSD spec.
                    for (std::size_t x = 1; x < rowBytesZip; ++x)
                        rowp[x] = static_cast<std::uint8_t>(rowp[x] + rowp[x - 1]);
                    scratch.assign(rowp, rowp + rowBytesZip);
                    for (std::uint32_t i = 0; i < width; ++i)
                        for (int k = 0; k < 4; ++k)
                            rowp[i * 4 + k] = scratch[k * width + i];
                } else {  // 16-bit: big-endian sample delta per row
                    for (std::uint32_t x = 1; x < width; ++x) {
                        const std::uint16_t a = static_cast<std::uint16_t>(
                            (rowp[x * 2] << 8) | rowp[x * 2 + 1]);
                        const std::uint16_t b = static_cast<std::uint16_t>(
                            (rowp[x * 2 - 2] << 8) | rowp[x * 2 - 1]);
                        const std::uint16_t s =
                            static_cast<std::uint16_t>(a + b);
                        rowp[x * 2] = static_cast<std::uint8_t>(s >> 8);
                        rowp[x * 2 + 1] = static_cast<std::uint8_t>(s & 0xff);
                    }
                }
            }
        }
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint8_t* src = inflated.data() +
                    (static_cast<std::size_t>(ch) * height + y) * rowBytesZip;
                assembleRow(src, static_cast<int>(width), bytesPerSample,
                            planes[ch].data() + static_cast<std::uint64_t>(y) * width);
            }
        }
    } else {
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint32_t len = compression == 1 ? rowBytes[static_cast<std::uint32_t>(ch) * height + y] : rawRow;
                std::uint16_t* row = planes[ch].data() + static_cast<std::uint64_t>(y) * width;
                if (!readPlaneRow(in, static_cast<int>(width), depth, compression == 1, len, row))
                    return fail(error, "corrupt PSD channel data");
            }
        }
    }
    if (!in.ok) return fail(error, "corrupt PSD image data");

    PsdImage img;
    img.width = width;
    img.height = height;
    img.depth = depth;
    img.colorMode = colorMode;
    img.rgba.assign(q * 4, kMaxSample);

    auto put = [&](std::uint64_t i, std::uint16_t r, std::uint16_t g, std::uint16_t b,
                   std::uint16_t a) {
        img.rgba[i * 4 + 0] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = a;
    };

    const std::uint16_t opaque = kMaxSample;
    switch (colorMode) {
        case 3: {  // RGB
            const bool hasA = channels >= 4;
            const auto& R = planes[0], G = planes[1], B = planes[2];
            const auto& A = hasA ? planes[3] : planes[0];
            for (std::uint64_t i = 0; i < q; ++i)
                put(i, R[i], G[i], B[i], hasA ? A[i] : opaque);
            break;
        }
        case 1: {  // grayscale
            const bool hasA = channels >= 2;
            const auto& V = planes[0];
            const auto& A = hasA ? planes[1] : planes[0];
            for (std::uint64_t i = 0; i < q; ++i)
                put(i, V[i], V[i], V[i], hasA ? A[i] : opaque);
            break;
        }
        case 4: {  // CMYK (stored inverted: 255 = no ink)
            const bool hasA = channels >= 5;
            const auto& C = planes[0], M = planes[1], Y = planes[2], K = planes[3];
            const auto& A = hasA ? planes[4] : planes[0];
            img.cmyk.resize(static_cast<std::size_t>(q) * 4);
            for (std::uint64_t i = 0; i < q; ++i) {
                const std::uint32_t cc = C[i], mm = M[i], yy = Y[i], kk = K[i];
                std::uint16_t rgb[3] = {0, 0, 0};
                convertCmykStored(&conv, cc, mm, yy, kk, rgb);
                img.cmyk[i * 4 + 0] = static_cast<std::uint16_t>(65535 - cc);
                img.cmyk[i * 4 + 1] = static_cast<std::uint16_t>(65535 - mm);
                img.cmyk[i * 4 + 2] = static_cast<std::uint16_t>(65535 - yy);
                img.cmyk[i * 4 + 3] = static_cast<std::uint16_t>(65535 - kk);
                put(i, rgb[0], rgb[1], rgb[2], hasA ? A[i] : opaque);
            }
            break;
        }
        case 9: {  // Lab
            const bool hasA = channels >= 4;
            const auto& Lp = planes[0], ap = planes[1], bp = planes[2];
            const auto& A = hasA ? planes[3] : planes[0];
            for (std::uint64_t i = 0; i < q; ++i) {
                double r, g, b;
                labToRgb(Lp[i], ap[i], bp[i], &r, &g, &b);
                put(i, clamp16(r), clamp16(g), clamp16(b), hasA ? A[i] : opaque);
            }
            break;
        }
        default:
            return fail(error, "unsupported PSD color mode (only RGB, grayscale, CMYK, Lab)");
    }

    return img;
}

std::optional<std::vector<std::uint8_t>> psdEncodeRgba(
    std::uint32_t width, std::uint32_t height, int depth,
    const std::uint16_t* rgba, int colorMode, const std::uint8_t* icc,
    std::size_t iccLen) {
    if (width == 0 || height == 0 || (depth != 8 && depth != 16) || !rgba)
        return std::nullopt;
    const std::uint64_t q = static_cast<std::uint64_t>(width) * height;
    if (q > 268435456u) return std::nullopt;

    // Planes: R, G, B, A (channel-major rows) — or C, M, Y, K, A when the
    // caller asked for a CMYK separation (appearance in, inks inverted out).
    const bool cmyk = colorMode == 4;
    std::vector<std::uint16_t> planed;
    if (cmyk) {
        pittore::color::separateRgba16(rgba, static_cast<std::size_t>(q),
                                        icc, iccLen, true, planed);
        invertInkPlanes(planed, 5, 4);
    }
    const std::uint16_t* src = cmyk ? planed.data() : rgba;
    const int stride = cmyk ? 5 : 4;
    const std::uint32_t channels = cmyk ? 5u : 4u;
    const std::uint16_t max = depth >= 16 ? kMaxSample : 255u;

    std::vector<std::vector<std::uint8_t>> planes(channels);
    std::vector<std::vector<std::vector<std::uint8_t>>> packed(channels);  // per-row tokens

    const int bytesPerSample = depth >= 16 ? 2 : 1;
    const std::uint32_t rowBytes = width * bytesPerSample;

    for (int ch = 0; ch < static_cast<int>(channels); ++ch) {
        planes[ch].resize(static_cast<std::size_t>(q) * bytesPerSample);
        const std::uint16_t* in = src + ch;
        std::uint8_t* outp = planes[ch].data();
        for (std::uint64_t i = 0; i < q; ++i, in += stride) {
            std::uint16_t v = *in;
            if (v > max) v = max;
            if (bytesPerSample == 1) {
                *outp++ = static_cast<std::uint8_t>(v);
            } else {
                *outp++ = static_cast<std::uint8_t>(v >> 8);
                *outp++ = static_cast<std::uint8_t>(v & 0xff);
            }
        }
        packed[ch].reserve(height);
        for (std::uint32_t y = 0; y < height; ++y)
            packed[ch].push_back(packBitsRow(planes[ch].data() +
                                                 static_cast<std::size_t>(y) * rowBytes,
                                             rowBytes));
    }

    std::vector<std::uint8_t> out;
    out.reserve(1024 + q * 4);
    auto put16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v & 0xff));
    };
    auto put32 = [&out](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v >> 24));
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(v & 0xff));
    };

    // Header.
    out.insert(out.end(), {'8', 'B', 'P', 'S'});
    put16(1);      // version
    out.insert(out.end(), 6, 0);
    put16(static_cast<std::uint16_t>(channels));
    put32(height);
    put32(width);
    put16(static_cast<std::uint16_t>(depth));
    put16(static_cast<std::uint16_t>(cmyk ? 4 : 3));  // color mode: CMYK / RGB
    put32(0);      // color mode data length
    const std::vector<std::uint8_t> resources = buildIccResource(icc, iccLen);
    put32(static_cast<std::uint32_t>(resources.size()));  // image resources
    out.insert(out.end(), resources.begin(), resources.end());
    put32(0);      // layer + mask info length
    put16(1);      // compression: RLE

    // RLE byte-count table (channel-major).
    for (int ch = 0; ch < static_cast<int>(channels); ++ch)
        for (std::uint32_t y = 0; y < height; ++y)
            put16(static_cast<std::uint16_t>(packed[ch][y].size()));

    for (int ch = 0; ch < static_cast<int>(channels); ++ch)
        for (const auto& row : packed[ch]) out.insert(out.end(), row.begin(), row.end());

    return out;
}

std::optional<PsdLayersDoc> psdDecodeLayers(const std::vector<std::uint8_t>& data,
                                            std::string* error,
                                            const std::vector<std::uint8_t>* embeddedIcc);
namespace layered_detail {
// Parses one layer-info body (count + records + channel image data) from
// `in`, bounded by `bodyEnd`. Used for the main section and, when the main
// section is empty, for a lifted Lr16/Lr32/Layr global block.
bool parseLayersBody(Reader& in, std::size_t bodyEnd, bool isPsb, int depth,
                     int colorMode, std::vector<ParsedLayer>& parsed,
                     const pittore::color::CmykToSrgb* cmyk,
                     std::string* error) {
    if (in.pos + 2 > bodyEnd) {
        if (error) *error = "truncated layer info";
        return false;
    }
    const std::int16_t rawCount = static_cast<std::int16_t>(in.u16be());
    const std::uint32_t count =
        rawCount < 0 ? static_cast<std::uint32_t>(-static_cast<std::int32_t>(rawCount))
                     : static_cast<std::uint32_t>(rawCount);
    if (!in.ok) {
        if (error) *error = "truncated layer info";
        return false;
    }
    if (count == 0) {
        if (error) *error = "PSD layer list empty";
        return false;
    }
    if (count > 8192) {
        if (error) *error = "PSD layer count too large";
        return false;
    }
    parsed.clear();
    parsed.reserve(count);
    std::uint64_t pixelBudget = 0;
    for (std::uint32_t i = 0; i < count && in.ok && in.pos < bodyEnd; ++i) {
        ParsedLayer p;
        if (!readLayerRecord(in, bodyEnd, p, isPsb)) {
            if (error) *error = in.ok ? "malformed layer record" : "truncated layer record";
            return false;
        }
        pixelBudget += static_cast<std::uint64_t>(p.file.width) * p.file.height;
        pixelBudget += static_cast<std::uint64_t>(p.file.maskWidth) *
                       p.file.maskHeight;
        pixelBudget += p.adjData.size();
        for (auto& rb : p.rawBlocks) pixelBudget += rb.data.size();
        if (pixelBudget > (1u << 30)) {
            if (error) *error = "layer pixel budget exceeded";
            return false;
        }
        parsed.push_back(std::move(p));
    }
    for (auto& p : parsed) {
        const std::uint64_t w = p.file.width;
        const std::uint64_t h = p.file.height;
        const bool raster = w > 0 && h > 0 && w * h <= (1u << 30);
        // 5 slots so CMYK keeps C,M,Y,K plus alpha (-1 → 4).
        std::vector<std::vector<std::uint16_t>> planes(5);
        std::vector<std::uint16_t> maskPlane;
        const bool isCmyk = (colorMode == 4);
        for (const ParsedLayer::Chan& c : p.chans) {
            if (c.len < 2 || c.len > bodyEnd - in.pos) {
                if (error) *error = "layer channel data out of range";
                return false;
            }
            const std::size_t len = static_cast<std::size_t>(c.len);
            const std::uint8_t* block0 = in.take(len);
            if (!block0) {
                if (error) *error = "truncated layer channel data";
                return false;
            }
            const std::vector<std::uint8_t> block(block0, block0 + len);
            const int tag = (static_cast<int>(block[0]) << 8) | block[1];
            int slot = -1;
            if (isCmyk) {
                if (c.id == 0) slot = 0;  // C
                else if (c.id == 1) slot = 1;  // M
                else if (c.id == 2) slot = 2;  // Y
                else if (c.id == 3) slot = 3;  // K
                else if (c.id == -1) slot = 4;  // alpha
            } else {
                if (c.id == 0) slot = 0;  // R (the single grey plane when grayscale)
                else if (c.id == 1) slot = 1;
                else if (c.id == 2) slot = 2;
                else if (c.id == -1) slot = 3;  // alpha
                else if (c.id == 3 && colorMode != 4) slot = -1;
            }
            if (slot >= 0 && p.file.channelMethod < 0 && tag >= 0 && tag <= 3)
                p.file.channelMethod = tag;
            if (slot >= 0 && raster && planes[slot].empty() &&
                !decodeLayerChannel(block, static_cast<int>(w), static_cast<int>(h),
                                    depth, planes[slot], isPsb)) {
                if (error) *error = "corrupt layer channel data";
                return false;
            }
            if ((c.id == -2 || c.id == -3) && maskPlane.empty() &&
                p.file.maskWidth > 0 && p.file.maskHeight > 0 &&
                static_cast<std::uint64_t>(p.file.maskWidth) *
                        p.file.maskHeight <=
                    (1u << 30) &&
                !decodeLayerChannel(block, static_cast<int>(p.file.maskWidth),
                                    static_cast<int>(p.file.maskHeight), depth,
                                    maskPlane, isPsb)) {
                if (error) *error = "corrupt layer mask channel data";
                return false;
            }
        }
        if (p.hasAdjKey) {
            bool hasPixels = false;
            for (const ParsedLayer::Chan& c : p.chans) {
                if (c.id == 0 || c.id == 1 || c.id == 2 || c.id == 3 || c.id == -1) {
                    hasPixels = true;
                    break;
                }
            }
            if (!hasPixels) {
                p.file.isAdjustment = true;
                if (!parseAdjustmentBlock(p.adjKey, p.adjData.data(),
                                          p.adjData.size(), p.file)) {
                    p.file.adjustmentKind = 0;
                    for (int k = 0; k < 16; ++k)
                        p.file.adjustmentParams[k] = 0.0f;
                    p.file.adjustmentCurve.clear();
                }
            }
        }
        if (raster && !p.file.isAdjustment)
            assembleLayerRgba(p.file, colorMode, planes, cmyk);
        if (!maskPlane.empty() &&
            maskPlane.size() == static_cast<std::size_t>(p.file.maskWidth) *
                                    p.file.maskHeight) {
            p.file.mask = std::move(maskPlane);
            p.file.hasMask = true;
        } else {
            p.file.mask.clear();
            p.file.hasMask = false;
        }
    }
    if (in.pos > bodyEnd) {
        if (error) *error = "layer info overrun";
        return false;
    }
    return true;
}
}  // namespace layered_detail

std::optional<PsdLayersDoc> psdDecodeLayers(const std::vector<std::uint8_t>& data,
                                            std::string* error,
                                            const std::vector<std::uint8_t>* embeddedIcc) {
    const pittore::color::CmykToSrgb conv(
        embeddedIcc && !embeddedIcc->empty() ? embeddedIcc->data() : nullptr,
        embeddedIcc ? embeddedIcc->size() : 0);
    Reader in(data);
    if (!in.ok || in.remaining() < 26) return fail(error, "PSD file too short");

    // --- header -----------------------------------------------------------
    const std::uint8_t* sig = in.take(4);
    if (!sig || std::memcmp(sig, "8BPS", 4) != 0)
        return fail(error, "not a PSD file (bad signature)");
    const std::uint16_t version = in.u16be();
    if (version != 1 && version != 2)
        return fail(error, "unsupported PSD version");
    const bool isPsb = (version == 2);
    if (!in.skip(6)) return fail(error, "truncated PSD header");
    in.u16be();  // channels
    const std::uint32_t height = in.u32be();
    const std::uint32_t width = in.u32be();
    const std::uint16_t depth = in.u16be();
    const std::uint16_t colorMode = in.u16be();
    if (!in.ok || width == 0 || height == 0) return fail(error, "truncated PSD header");
    if (depth != 8 && depth != 16 && depth != 32)
        return fail(error, "unsupported PSD bit depth for layered import");
    if (colorMode != 3 && colorMode != 1 && colorMode != 4 && colorMode != 9)
        return fail(error, "layered import supports RGB/grayscale/CMYK/Lab documents");
    if (width > 300000u || height > 300000u)
        return fail(error, "PSD dimensions too large");
    if (!isPsb && (width > 30000u || height > 30000u))
        return fail(error, "PSD dimensions exceed 30000; use PSB");
    const std::uint64_t docPixels = static_cast<std::uint64_t>(width) * height;
    if (docPixels > 268435456u) return fail(error, "PSD dimensions too large");

    const std::uint32_t colorModeLen = in.u32be();
    if (!in.skip(colorModeLen)) return fail(error, "truncated color-mode data");
    const std::uint32_t resourcesLen = in.u32be();
    if (!in.skip(resourcesLen)) return fail(error, "truncated image resources");

    // --- layer & mask info ------------------------------------------------
    // layer&mask + layer-info lengths are u64 in PSB, u32 in PSD.
    std::uint64_t layerMaskLen = 0;
    if (isPsb) {
        layerMaskLen = in.u64be();
    } else {
        layerMaskLen = in.u32be();
    }
    if (!in.ok) return fail(error, "truncated layer/mask info");
    if (layerMaskLen == 0) return fail(error, "PSD has no layer section");
    if (layerMaskLen > in.remaining()) return fail(error, "truncated layer/mask info");
    const std::size_t layerMaskEnd = in.pos + static_cast<std::size_t>(layerMaskLen);

    std::uint64_t layerInfoLen = 0;
    if (isPsb) {
        if (in.pos + 8 > layerMaskEnd) return fail(error, "truncated layer info");
        const std::uint32_t hi = in.u32be();
        const std::uint32_t lo = in.u32be();
        if (!in.ok) return fail(error, "truncated layer info");
        layerInfoLen = (static_cast<std::uint64_t>(hi) << 32) | lo;
    } else {
        if (in.pos + 4 > layerMaskEnd) return fail(error, "truncated layer info");
        layerInfoLen = in.u32be();
    }
    if (!in.ok) return fail(error, "truncated layer info");

    std::vector<ParsedLayer> parsed;
    if (layerInfoLen > 0) {
        if (layerInfoLen > layerMaskEnd - in.pos)
            return fail(error, "truncated layer info");
        const std::size_t layerInfoEnd = in.pos + static_cast<std::size_t>(layerInfoLen);
        // Channel image data follows ALL records inside this same body:
        // each block starts with its compression tag; RLE counts are
        // u32 in PSB, u16 in PSD.
        if (!layered_detail::parseLayersBody(in, layerInfoEnd, isPsb, depth, colorMode,
                                             parsed, &conv, error))
            return std::nullopt;
        if (in.pos > layerInfoEnd) return fail(error, "layer info overrun");
        in.pos = layerInfoEnd;
    } else {
        // 16/32-bit PSD files keep layers in a global Lr16/Lr32/Layr
        // block with the main layer info left empty. Scan the remainder
        // of the section (global mask + tagged blocks) for it and parse
        // its data as the layer-info body.
        if (in.pos + 4 > layerMaskEnd) return fail(error, "PSD has no layer records");
        const std::uint32_t globalMaskLen = in.u32be();
        if (!in.ok || in.pos + globalMaskLen > layerMaskEnd)
            return fail(error, "truncated layer/mask info");
        if (!in.skip(globalMaskLen)) return fail(error, "truncated layer/mask info");
        std::vector<std::uint8_t> lifted;
        bool found = false;
        while (in.ok && in.pos + 12 <= layerMaskEnd) {
            const std::uint8_t* bsig = in.take(4);
            const std::uint8_t* bkey = in.take(4);
            if (!bsig || !bkey) break;
            if (!isValidBlockSig(bsig)) break;
            std::uint64_t blkLen = 0;
            if (isPsb && isPsbLongKey(bkey)) {
                if (in.pos + 8 > layerMaskEnd) break;
                const std::uint32_t hi = in.u32be();
                const std::uint32_t lo = in.u32be();
                if (!in.ok) break;
                blkLen = (static_cast<std::uint64_t>(hi) << 32) | lo;
            } else {
                if (in.pos + 4 > layerMaskEnd) break;
                blkLen = in.u32be();
            }
            if (!in.ok || blkLen > layerMaskEnd - in.pos) break;
            const std::uint8_t* bdata = in.take(static_cast<std::size_t>(blkLen));
            if (!bdata) break;
            if ((blkLen & 1u) && in.pos < layerMaskEnd) in.skip(1);
            if (!found &&
                (std::memcmp(bkey, "Lr16", 4) == 0 ||
                 std::memcmp(bkey, "Lr32", 4) == 0 ||
                 std::memcmp(bkey, "Layr", 4) == 0)) {
                lifted.assign(bdata, bdata + static_cast<std::size_t>(blkLen));
                found = true;
            }
        }
        if (!found) return fail(error, "PSD has no layer records");
        Reader lin(lifted);
        if (!layered_detail::parseLayersBody(lin, lifted.size(), isPsb, depth, colorMode,
                                             parsed, &conv, error))
            return std::nullopt;
        if (lin.pos > lifted.size()) return fail(error, "layer info overrun");
        in.pos = layerMaskEnd;
    }
    if (in.pos < layerMaskEnd && !in.skip(layerMaskEnd - in.pos))
        return fail(error, "truncated layer/mask tail");

    // --- group nesting ----------------------------------------------------
    PsdLayersDoc doc;
    doc.width = width;
    doc.height = height;
    doc.depth = depth;
    // Keep the header's mode id for the caller (1 gray, 3 RGB, 4 CMYK,
    // 9 Lab — import metadata only; the writer always emits 3/4) and the
    // profile the layers were just decoded through, so a CMYK source can
    // be re-exported through the same separation.
    doc.colorMode = colorMode;
    if (embeddedIcc && !embeddedIcc->empty()) doc.icc = *embeddedIcc;
    int level = 0;
    for (auto& p : parsed) {
        if (p.lsct == 3) {  // folder-end divider: closes the current group
            if (level > 0) --level;
            continue;
        }
        p.file.indent = level;
        if (p.lsct == 1 || p.lsct == 2) {
            p.file.isGroup = true;
            p.file.groupExpanded = (p.lsct == 1);
            if (level < 64) ++level;
        }
        for (auto& rb : p.rawBlocks) {
            PsdLayerFile::RawBlock out;
            std::memcpy(out.sig, rb.sig, 4);
            std::memcpy(out.key, rb.key, 4);
            out.data = std::move(rb.data);
            out.padding = std::move(rb.padding);
            p.file.rawBlocks.push_back(std::move(out));
        }
        doc.layers.push_back(std::move(p.file));
    }
    return doc;
}

std::optional<std::vector<std::uint8_t>> psdEncodeLayers(
    const PsdLayersDoc& doc, std::string* error,
    PsdLayerCompression compression) {
    if (doc.width == 0 || doc.height == 0) return fail(error, "PSD document has no size");
    if (doc.depth != 8 && doc.depth != 16)
        return fail(error, "PSD depth must be 8 or 16");
    if (doc.layers.empty()) return fail(error, "PSD document has no layers");
    if (doc.layers.size() > 8192) return fail(error, "too many layers");
    const std::uint64_t docPixels = static_cast<std::uint64_t>(doc.width) * doc.height;
    if (docPixels > 268435456u) return fail(error, "PSD dimensions too large");
    const bool useZip = compression == PsdLayerCompression::Zip;
    // Mode 4 separates each appearance plane set (and the composite) into
    // inks at write time; everything else stays the RGB writer of old.
    const bool cmyk = doc.colorMode == 4;
    auto pixBlock = [&](const std::uint16_t* rgba, int w, int h, int ch,
                        bool zip, int stride = 4) {
        return zip ? zipChannelBlock(rgba, w, h, doc.depth, ch, stride)
                   : rleChannelBlock(rgba, w, h, doc.depth, ch, stride);
    };
    auto maskBlock = [&](const std::uint16_t* gray, int w, int h, bool zip) {
        return zip ? zipGrayChannelBlock(gray, w, h, doc.depth)
                   : rleGrayChannelBlock(gray, w, h, doc.depth);
    };
    std::uint64_t pixelBudget = docPixels;
    for (const PsdLayerFile& l : doc.layers) {
        const std::uint64_t n = static_cast<std::uint64_t>(l.width) * l.height;
        if (!l.rgba.empty() && l.rgba.size() != n * 4)
            return fail(error, "layer pixel count mismatch");
        const std::uint64_t mn =
            static_cast<std::uint64_t>(l.maskWidth) * l.maskHeight;
        if (!l.mask.empty() && l.mask.size() != mn)
            return fail(error, "layer mask size mismatch");
        pixelBudget += n + (l.hasMask ? mn : 0);
        if (pixelBudget > (1u << 31)) return fail(error, "layer pixel budget exceeded");
    }

    // --- layer & mask section ---------------------------------------------
    // Real PSD layout: every layer record (metadata only, no pixels) is
    // written first, then all channel image data follows. `recs` captures the
    // records and their blocks so channel-info lengths are exact.
    std::vector<OutRec> recs;
    int level = 0;
    for (const PsdLayerFile& l : doc.layers) {
        while (level > l.indent && level > 0) {
            recs.push_back({});
            recs.back().endMarker = true;
            --level;
        }
        OutRec r;
        r.layer = &l;
        // Untouched ZIP-origin layers keep their method even under a global
        // RLE encode (mixed methods are legal PSD); everything else follows
        // the requested compression.
        const bool lz = useZip || l.preferZip;
        if (!l.isGroup && !l.rgba.empty()) {
            r.blocks.reserve(6);
            if (cmyk) {
                // Separate once per layer: appearance planes in, five
                // interleaved C,M,Y,K,A planes out (inks inverted for the
                // file), then emit each plane with its stride. A fresh
                // native separation is used directly when its dimensions
                // match; otherwise appearance pixels are separated using
                // the document profile.
                std::vector<std::uint16_t> planed;
                const std::size_t n = l.rgba.size() / 4;
                if (l.cmyk.size() == n * 4) {
                    planed.resize(n * 5);
                    for (std::size_t i = 0; i < n; ++i) {
                        for (int c = 0; c < 4; ++c)
                            planed[i * 5 + c] = l.cmyk[i * 4 + c];
                        planed[i * 5 + 4] = l.rgba[i * 4 + 3];
                    }
                } else {
                    pittore::color::separateRgba16(
                        l.rgba.data(), n, doc.icc.data(), doc.icc.size(),
                        true, planed);
                }
                invertInkPlanes(planed, 5, 4);
                for (int c = 0; c < 5; ++c)
                    r.blocks.push_back(pixBlock(
                        planed.data(), static_cast<int>(l.width),
                        static_cast<int>(l.height), c, lz, 5));
            } else {
                for (int c = 0; c < 4; ++c)
                    r.blocks.push_back(pixBlock(
                        l.rgba.data(), static_cast<int>(l.width),
                        static_cast<int>(l.height), c, lz));
            }
            if (l.hasMask && !l.mask.empty() && l.maskWidth > 0 &&
                l.maskHeight > 0) {
                r.hasMask = true;
                r.blocks.push_back(maskBlock(
                    l.mask.data(), static_cast<int>(l.maskWidth),
                    static_cast<int>(l.maskHeight), lz));
            }
        } else if (!l.isGroup && l.isAdjustment) {
            // Adjustment rows carry no pixel channels, but may carry a mask
            // channel. Stand-ins (kind 0) are skipped: they are foreign
            // no-ops with no faithful PSD representation, and emitting them
            // as blank pixels would corrupt the canvas on reimport.
            if (l.adjustmentKind == 0) continue;
            if (l.hasMask && !l.mask.empty() && l.maskWidth > 0 &&
                l.maskHeight > 0) {
                r.hasMask = true;
                r.blocks.push_back(maskBlock(
                    l.mask.data(), static_cast<int>(l.maskWidth),
                    static_cast<int>(l.maskHeight), lz));
            }
            r.adjData = encodeAdjustmentBlock(l, r.adjKey);
            r.hasAdj = true;
        }
        recs.push_back(std::move(r));
        if (l.isGroup) ++level;
    }
    while (level > 0) {
        recs.push_back({});
        recs.back().endMarker = true;
        --level;
    }

    std::vector<std::uint8_t> layerInfo;
    layerInfo.reserve(1024 + pixelBudget * 2);
    append32(layerInfo, 0);  // length patched below
    // The layer count covers EVERY record, including the lsct folder-end
    // dividers the writer inserts to close each group.
    append16(layerInfo, static_cast<std::uint16_t>(recs.size()));
    for (const OutRec& r : recs) emitLayerRecord(layerInfo, r, cmyk);
    // Channel image data: compression tag + payload per channel, in record
    // order and then channel-info order, as other editors write it.
    for (const OutRec& r : recs)
        for (const auto& b : r.blocks) layerInfo.insert(layerInfo.end(), b.begin(), b.end());
    const std::uint32_t layerInfoLen =
        static_cast<std::uint32_t>(layerInfo.size() - 4);
    layerInfo[0] = static_cast<std::uint8_t>(layerInfoLen >> 24);
    layerInfo[1] = static_cast<std::uint8_t>(layerInfoLen >> 16);
    layerInfo[2] = static_cast<std::uint8_t>(layerInfoLen >> 8);
    layerInfo[3] = static_cast<std::uint8_t>(layerInfoLen);

    std::vector<std::uint8_t> layerMask = std::move(layerInfo);
    append32(layerMask, 0);  // global layer mask data (none)

    // --- flattened composite ----------------------------------------------
    // Composite in appearance space (the engine's blend maths is RGB), then
    // separate the finished picture — flatten first, separate second, the
    // standard RGB-workflow order.
    std::vector<std::uint16_t> composite;
    compositeLayers(doc, composite);
    int compChannels = 4;
    if (cmyk) {
        std::vector<std::uint16_t> planed;
        pittore::color::separateRgba16(
            composite.data(), composite.size() / 4, doc.icc.data(),
            doc.icc.size(), true, planed);
        invertInkPlanes(planed, 5, 4);
        composite = std::move(planed);
        compChannels = 5;
    }
    std::vector<std::uint8_t> compositeData;
    if (useZip)
        appendCompositeZip(compositeData, composite.data(), doc.width,
                           doc.height, doc.depth, compChannels);
    else
        appendCompositeRle(compositeData, composite.data(), doc.width,
                           doc.height, doc.depth, compChannels);

    // --- assemble ---------------------------------------------------------
    std::vector<std::uint8_t> out;
    out.reserve(1024 + layerMask.size() + compositeData.size());
    out.insert(out.end(), {'8', 'B', 'P', 'S'});
    append16(out, 1);  // version
    out.insert(out.end(), 6, 0);
    append16(out, static_cast<std::uint16_t>(cmyk ? 5 : 4));  // channels: CMYK+A / RGBA
    append32(out, doc.height);
    append32(out, doc.width);
    append16(out, static_cast<std::uint16_t>(doc.depth));
    append16(out, static_cast<std::uint16_t>(cmyk ? 4 : 3));  // color mode: CMYK / RGB
    append32(out, 0);  // color mode data
    const std::vector<std::uint8_t> resources =
        buildIccResource(doc.icc.data(), doc.icc.size());
    append32(out, static_cast<std::uint32_t>(resources.size()));  // image resources
    out.insert(out.end(), resources.begin(), resources.end());
    append32(out, static_cast<std::uint32_t>(layerMask.size()));
    out.insert(out.end(), layerMask.begin(), layerMask.end());
    out.insert(out.end(), compositeData.begin(), compositeData.end());
    return out;
}

std::vector<std::uint8_t> psdEmbeddedIcc(
    const std::vector<std::uint8_t>& data) {
    const std::uint8_t* p = data.data();
    const std::size_t n = data.size();
    if (n < 30) return {};
    if (std::memcmp(p, "8BPS", 4) != 0) return {};
    // Header is 26 bytes; color-mode-data length follows at [26, 30).
    std::uint32_t colorModeLen = 0, resourcesLen = 0;
    if (!icc_detail::peekU32(p, n, 26, &colorModeLen)) return {};
    std::size_t pos = 30 + colorModeLen;
    if (pos + 4 > n) return {};
    if (!icc_detail::peekU32(p, n, pos, &resourcesLen)) return {};
    pos += 4;
    if (resourcesLen > n - pos) return {};
    const std::size_t end = pos + resourcesLen;
    // Image resource blocks: '8BIM' + u16 id + Pascal name (even-padded) +
    // u32 size + data (even-padded). The ICC profile is resource 0x040F by
    // spec; 0x0422 is accepted as a legacy fallback (files this writer
    // emitted before the spec id was straightened out) — the spec id wins
    // if a file somehow carries both.
    std::vector<std::uint8_t> legacy;
    while (pos + 12 <= end) {
        if (std::memcmp(p + pos, "8BIM", 4) != 0) return {};
        std::uint16_t id = 0;
        if (!icc_detail::peekU16(p, n, pos + 4, &id)) return {};
        const std::size_t nameLen = p[pos + 6];
        const std::size_t nameField = ((1 + nameLen) + 1) & ~std::size_t(1);
        std::uint32_t size = 0;
        if (!icc_detail::peekU32(p, n, pos + 6 + nameField, &size)) return {};
        const std::size_t body = pos + 6 + nameField + 4;
        if (size > end - body) return {};
        if (id == 0x040F)
            return std::vector<std::uint8_t>(p + body, p + body + size);
        if (id == 0x0422 && legacy.empty())
            legacy.assign(p + body, p + body + size);
        pos = body + ((size + 1) & ~std::size_t(1));
    }
    return legacy;
}

std::string psdIccProfileName(const std::vector<std::uint8_t>& data) {
    const std::vector<std::uint8_t> icc = psdEmbeddedIcc(data);
    if (icc.empty()) return "";
    return iccProfileDescription(icc.data(), icc.size());
}

}  // namespace pittore::io
