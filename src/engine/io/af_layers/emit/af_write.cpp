#include "engine/io/af_layers/emit/af_write.h"

#include "engine/io/af_layers.h"
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/emit/af_emit.h"
#include "engine/io/af_layers/geom/af_geom.h"

#include <algorithm>
#include <ctime>
#include <random>

#include <zlib.h>
#ifdef PITTORE_AF
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

namespace pittore::io {
namespace af_detail {

namespace {

// ---------------------------------------------------------------------------
// Graph builder helpers
// ---------------------------------------------------------------------------

Node* newNode(Graph& g) {
    g.nodes.push_back(std::make_unique<Node>());
    return g.nodes.back().get();
}

}  // namespace

std::uint64_t Builder::rand() {
    // splitmix64.
    rng += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

Node* Builder::node31(const std::vector<std::pair<std::uint32_t, std::uint16_t>>& chain) {
    Node* n = newNode(*g);
    for (const auto& t : chain) n->types.emplace_back(t.first, t.second);
    for (std::size_t i = 0; i + 1 < chain.size(); ++i)
        n->sections.push_back({chain[i].first, chain[i].second, 0, 0, false});
    n->sections.push_back({chain.back().first, chain.back().second, 0, 0, true});
    n->endMark = 1;
    return n;
}

Node* Builder::nodeClosed(std::uint32_t tag, std::uint16_t ver) {
    Node* n = newNode(*g);
    n->types.emplace_back(tag, ver);
    n->sections.push_back({tag, ver, 0, 0, false});
    n->endMark = 2;
    return n;
}

Node* Builder::nodeTagged(std::uint32_t tag, std::uint16_t ver) {
    Node* n = newNode(*g);
    n->types.emplace_back(tag, ver);
    return n;
}

Value Builder::vI32(std::int32_t v) {
    Value x;
    x.k = Value::K::I32;
    x.u = static_cast<std::uint64_t>(static_cast<std::uint32_t>(v));
    x.wire = 0x07;
    return x;
}

Value Builder::vU8(std::uint8_t v) {
    Value x;
    x.k = Value::K::U8;
    x.u = v;
    x.wire = 0x01;
    return x;
}

Value Builder::vU32(std::uint32_t v) {
    Value x;
    x.k = Value::K::U32;
    x.u = v;
    x.wire = 0x03;
    return x;
}

Value Builder::vF32(float v) {
    Value x;
    x.k = Value::K::F32;
    x.f = v;
    x.wire = 0x09;
    return x;
}

Value Builder::vF64(double v) {
    Value x;
    x.k = Value::K::F64;
    x.f = v;
    x.wire = 0x0a;
    return x;
}

Value Builder::vBool(bool v) {
    Value x;
    x.k = Value::K::Bool;
    x.u = v ? 1 : 0;
    x.wire = 0x29;
    return x;
}

Value Builder::vEnum(std::uint16_t id, std::uint16_t ver) {
    Value x;
    x.k = Value::K::Enum;
    x.u = id;
    x.ver = ver;
    x.wire = 0x2a;
    return x;
}

Value Builder::vStr(const std::string& s) {
    Value x;
    x.k = Value::K::Str;
    x.s = s;
    x.wire = 0x2b;
    return x;
}

Value Builder::vVecI(const std::vector<std::int32_t>& v) {
    Value x;
    x.k = Value::K::VecI;
    x.vi = v;
    if (v.size() < 2 || v.size() > 6) fail("VecI width out of range for builder");
    x.wire = static_cast<std::uint8_t>(0x15 + (v.size() - 2));
    return x;
}

Value Builder::vVecD(const std::vector<double>& v) {
    Value x;
    x.k = Value::K::VecD;
    x.vd = v;
    if (v.size() < 2 || v.size() > 6) fail("VecD width out of range for builder");
    x.wire = static_cast<std::uint8_t>(0x24 + (v.size() - 2));
    return x;
}

Value Builder::vNull31() {
    Value x;
    x.k = Value::K::Class;
    x.cls = nullptr;
    x.wire = 0x31;
    return x;
}

Value Builder::vClass(Node* n) {
    Value x;
    x.k = Value::K::Class;
    x.cls = n;
    x.wire = 0x31;
    return x;
}

Value Builder::vArray(std::uint8_t wire) {
    Value x;
    x.k = Value::K::Array;
    x.wire = wire;
    return x;
}

Value Builder::vEmbedded(std::uint32_t tag, const std::string& name) {
    Value x;
    x.k = Value::K::Embedded;
    x.u = tag;
    x.s = name;
    x.wire = 0x33;
    return x;
}

void Builder::put(Node* n, const char* name, Value v) {
    n->fields.emplace_back(tag4(name), std::move(v));
}

void Builder::putArr(Node* n, const char* name, std::uint8_t wire, std::vector<Value> items) {
    Value v = vArray(wire);
    v.arr = std::move(items);
    put(n, name, std::move(v));
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> blendEnum(
    const std::string& name) {
    // Inverse of blendName(); modes with no Affinity equivalent are nullopt
    // (the caller falls back to Normal and reports).
    if (name == "Normal") return std::make_pair(0, 0);
    if (name == "Darken") return std::make_pair(1, 0);
    if (name == "Multiply") return std::make_pair(2, 0);
    if (name == "Darker Color") return std::make_pair(2, 1);
    if (name == "Color Burn") return std::make_pair(3, 0);
    if (name == "Lighten") return std::make_pair(4, 0);
    if (name == "Screen") return std::make_pair(5, 0);
    if (name == "Color Dodge") return std::make_pair(6, 0);
    if (name == "Lighter Color") return std::make_pair(6, 1);
    if (name == "Add") return std::make_pair(7, 0);
    if (name == "Overlay") return std::make_pair(8, 0);
    if (name == "Soft Light") return std::make_pair(9, 0);
    if (name == "Hard Light") return std::make_pair(10, 0);
    if (name == "Vivid Light") return std::make_pair(11, 0);
    if (name == "Pin Light") return std::make_pair(12, 0);
    if (name == "Hard Mix") return std::make_pair(13, 0);
    if (name == "Difference") return std::make_pair(14, 0);
    if (name == "Exclusion") return std::make_pair(15, 0);
    if (name == "Linear Light") return std::make_pair(15, 1);
    if (name == "Subtract") return std::make_pair(16, 0);
    if (name == "Hue") return std::make_pair(17, 0);
    if (name == "Saturation") return std::make_pair(18, 0);
    if (name == "Luminosity") return std::make_pair(19, 0);
    if (name == "Color") return std::make_pair(20, 0);
    return std::nullopt;
}

namespace {

// ---------------------------------------------------------------------------
// Tile encoder
// ---------------------------------------------------------------------------

void deltaU8(std::uint8_t* p, std::size_t n) {
    std::uint8_t prev = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t v = p[i];
        p[i] = static_cast<std::uint8_t>(v - prev);
        prev = v;
    }
}

bool deflateZlib(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    out.clear();
    z_stream zs{};
    if (deflateInit(&zs, Z_BEST_SPEED) != Z_OK) return false;
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

#ifdef PITTORE_AF
bool deflateZstd(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) {
    const std::size_t bound = ZSTD_compressBound(in.size());
    out.resize(bound);
    const std::size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), 1);
    if (ZSTD_isError(n)) {
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
}
#endif

// Compress one tile payload: byte-delta prediction, then zstd (0x42) or
// zlib (0x41); stored (0x00) when compression does not pay. The CRC covers
// the original (undeltad) tile, matching what the reader verifies.
struct PackedTile {
    std::uint8_t comp = 0;
    std::vector<std::uint8_t> bytes;  // bytes after #Fil
    std::uint32_t crc = 0;
};

PackedTile packTile(const std::vector<std::uint8_t>& tile) {
    PackedTile pt;
    pt.crc = crc32(0, tile.data(), static_cast<uInt>(tile.size()));
    std::vector<std::uint8_t> work = tile;
    deltaU8(work.data(), work.size());
    std::vector<std::uint8_t> comp;
    bool ok = false;
#ifdef PITTORE_AF
    ok = deflateZstd(work, comp);
    pt.comp = 0x42;
#else
    ok = deflateZlib(work, comp);
    pt.comp = 0x41;
#endif
    if (!ok || comp.size() >= tile.size()) {
        // Stored: raw bytes, no prediction pass on read.
        pt.comp = 0x00;
        pt.bytes = tile;
    } else {
        pt.bytes = std::move(comp);
    }
    return pt;
}

}  // namespace

TiledPlanes tileRgba8(Graph& g, const std::uint8_t* px, std::uint32_t w,
                      std::uint32_t h, int& entryBase) {
    // Deinterleave into 4 tight planes first.
    std::vector<std::vector<std::uint8_t>> tight(
        4, std::vector<std::uint8_t>(std::size_t(w) * h));
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                tight[c][std::size_t(y) * w + x] =
                    px[(std::size_t(y) * w + x) * 4 + c];
    TiledPlanes t;
    t.gridW = int((w + 255) / 256);
    t.gridH = int((h + 255) / 256);
    t.statuses.assign(4, {});
    t.blocks.assign(4, {});
    Builder b(&g, 0);
    for (int c = 0; c < 4; ++c) {
        for (int ty = 0; ty < t.gridH; ++ty) {
            for (int tx = 0; tx < t.gridW; ++tx) {
                const int vw = std::min<int>(256, int(w) - tx * 256);
                const int vh = std::min<int>(256, int(h) - ty * 256);
                std::vector<std::uint8_t> tile(0x10000, 0);
                bool all0 = true, allFF = true;
                for (int row = 0; row < vh; ++row) {
                    const std::uint8_t* src =
                        tight[c].data() + std::size_t(ty * 256 + row) * w + tx * 256;
                    std::memcpy(tile.data() + std::size_t(row) * 256, src, vw);
                    for (int x = 0; x < vw; ++x) {
                        all0 &= src[x] == 0;
                        allFF &= src[x] == 0xff;
                    }
                }
                if (all0) {
                    t.statuses[c].push_back(1);
                } else if (allFF) {
                    t.statuses[c].push_back(2);
                } else {
                    t.statuses[c].push_back(4);
                    PackedTile pt = packTile(tile);
                    const std::string name = "d/" + std::to_string(entryBase++);
                    Node* blck = b.nodeClosed(tag4("Blck"), 1);
                    if (vw < 256 || vh < 256) {
                        Value r;
                        r.k = Value::K::VecI;
                        r.vi = {0, 0, vw, vh};
                        r.wire = 0x17;
                        Builder::put(blck, "Rect", std::move(r));
                    }
                    Builder::put(blck, "Data", Builder::vEmbedded(tag4("DatI"), name));
                    OutEntry e;
                    e.name = name;
                    e.payload = std::move(pt.bytes);
                    e.plainSize = 0x10000;
                    e.comp = pt.comp;
                    e.crc = pt.crc;
                    t.entries.push_back(std::move(e));
                    t.blocks[c].push_back(blck);
                }
            }
        }
    }
    return t;
}

TiledPlanes tileMask8(Graph& g, const std::uint8_t* px, std::uint32_t w,
                      std::uint32_t h, int& entryBase) {
    std::vector<std::vector<std::uint8_t>> tight(
        1, std::vector<std::uint8_t>(std::size_t(w) * h));
    std::memcpy(tight[0].data(), px, std::size_t(w) * h);
    TiledPlanes t;
    t.gridW = int((w + 255) / 256);
    t.gridH = int((h + 255) / 256);
    t.statuses.assign(1, {});
    t.blocks.assign(1, {});
    Builder b(&g, 0);
    for (int ty = 0; ty < t.gridH; ++ty) {
        for (int tx = 0; tx < t.gridW; ++tx) {
            const int vw = std::min<int>(256, int(w) - tx * 256);
            const int vh = std::min<int>(256, int(h) - ty * 256);
            std::vector<std::uint8_t> tile(0x10000, 0);
            bool all0 = true, allFF = true;
            for (int row = 0; row < vh; ++row) {
                const std::uint8_t* src =
                    tight[0].data() + std::size_t(ty * 256 + row) * w + tx * 256;
                std::memcpy(tile.data() + std::size_t(row) * 256, src, vw);
                for (int x = 0; x < vw; ++x) {
                    all0 &= src[x] == 0;
                    allFF &= src[x] == 0xff;
                }
            }
            if (all0) {
                t.statuses[0].push_back(1);
            } else if (allFF) {
                t.statuses[0].push_back(2);
            } else {
                t.statuses[0].push_back(4);
                PackedTile pt = packTile(tile);
                const std::string name = "d/" + std::to_string(entryBase++);
                Node* blck = b.nodeClosed(tag4("Blck"), 1);
                if (vw < 256 || vh < 256) {
                    Value r;
                    r.k = Value::K::VecI;
                    r.vi = {0, 0, vw, vh};
                    r.wire = 0x17;
                    Builder::put(blck, "Rect", std::move(r));
                }
                Builder::put(blck, "Data", Builder::vEmbedded(tag4("DatI"), name));
                OutEntry e;
                e.name = name;
                e.payload = std::move(pt.bytes);
                e.plainSize = 0x10000;
                e.comp = pt.comp;
                e.crc = pt.crc;
                t.entries.push_back(std::move(e));
                t.blocks[0].push_back(blck);
            }
        }
    }
    return t;
}

Node* bitmapNode(Graph& g, Builder& b, int format, std::uint32_t w,
                 std::uint32_t h, const TiledPlanes& tiled) {
    Node* bitm = b.nodeClosed(tag4("DyBm"), 1);
    Builder::put(bitm, "Frmt", Builder::vEnum(static_cast<std::uint16_t>(format), 0));
    Builder::put(bitm, "BmpW", Builder::vI32(static_cast<std::int32_t>(w)));
    Builder::put(bitm, "BmpH", Builder::vI32(static_cast<std::int32_t>(h)));
    Builder::put(bitm, "DelA", Builder::vBool(true));
    Builder::put(bitm, "MipM", Builder::vEnum(4, 0));
    Builder::put(bitm, "LInf", Builder::vI32(0));
    Builder::put(bitm, "TInf", Builder::vI32(0));
    const int channels = format == 0 ? 4 : 1;
    for (int c = 0; c < channels; ++c) {
        const char twi[5] = {'T', 'W', 'i', static_cast<char>('1' + c), 0};
        const char thi[5] = {'T', 'H', 'i', static_cast<char>('1' + c), 0};
        const char idx[5] = {'I', 'd', 'x', static_cast<char>('1' + c), 0};
        const char sta[5] = {'S', 't', 'a', static_cast<char>('1' + c), 0};
        Builder::put(bitm, twi, Builder::vI32(tiled.gridW));
        Builder::put(bitm, thi, Builder::vI32(tiled.gridH));
        std::vector<Value> idxVals;
        for (Node* blck : tiled.blocks[c]) idxVals.push_back(Builder::vClass(blck));
        Builder::putArr(bitm, idx, 0xb1, std::move(idxVals));
        std::vector<Value> staVals;
        for (std::uint8_t s : tiled.statuses[c]) staVals.push_back(Builder::vU8(s));
        Builder::putArr(bitm, sta, 0x81, std::move(staVals));
    }
    (void)g;
    return bitm;
}

void evictBitmap(Node* dybm, std::uint32_t w, std::uint32_t h) {
    if (!dybm) return;
    const int gw = int((w + 255) / 256);
    const int gh = int((h + 255) / 256);
    const std::size_t n = std::size_t(gw) * gh;
    auto setI32 = [&](const char* name, std::int32_t v) {
        for (auto& f : dybm->fields)
            if (f.first == tag4(name) && f.second.k == Value::K::I32) {
                f.second.u = static_cast<std::uint64_t>(static_cast<std::uint32_t>(v));
                return;
            }
    };
    setI32("BmpW", static_cast<std::int32_t>(w));
    setI32("BmpH", static_cast<std::int32_t>(h));
    for (int c = 1; c <= 5; ++c) {
        char twi[5] = {'T', 'W', 'i', static_cast<char>('0' + c), 0};
        char thi[5] = {'T', 'H', 'i', static_cast<char>('0' + c), 0};
        char idx[5] = {'I', 'd', 'x', static_cast<char>('0' + c), 0};
        char sta[5] = {'S', 't', 'a', static_cast<char>('0' + c), 0};
        setI32(twi, gw);
        setI32(thi, gh);
        for (auto& f : dybm->fields) {
            if (f.first == tag4(idx) && f.second.k == Value::K::Array) {
                f.second.arr.clear();
            }
            if (f.first == tag4(sta) && f.second.k == Value::K::Array) {
                f.second.arr.clear();
                for (std::size_t i = 0; i < n; ++i) {
                    Value s;
                    s.k = Value::K::U8;
                    s.u = 0;
                    f.second.arr.push_back(std::move(s));
                }
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Minimal PNG encoder (RGBA8, Sub filter) for the thumbnail block.
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encodePngRgba8(const std::uint8_t* px, std::uint32_t w,
                                         std::uint32_t h) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&](const char tag[4], const std::vector<std::uint8_t>& data) {
        const std::uint32_t n = static_cast<std::uint32_t>(data.size());
        out.push_back(static_cast<std::uint8_t>(n >> 24));
        out.push_back(static_cast<std::uint8_t>(n >> 16));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(n));
        out.insert(out.end(), tag, tag + 4);
        out.insert(out.end(), data.begin(), data.end());
        uLong crc = crc32(0, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(tag), 4);
        if (!data.empty())
            crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()),
                        static_cast<uInt>(data.size()));
        out.push_back(static_cast<std::uint8_t>(crc >> 24));
        out.push_back(static_cast<std::uint8_t>(crc >> 16));
        out.push_back(static_cast<std::uint8_t>(crc >> 8));
        out.push_back(static_cast<std::uint8_t>(crc));
    };
    std::vector<std::uint8_t> ihdr;
    for (int i = 3; i >= 0; --i)
        ihdr.push_back(static_cast<std::uint8_t>((w >> (8 * i)) & 0xff));
    for (int i = 3; i >= 0; --i)
        ihdr.push_back(static_cast<std::uint8_t>((h >> (8 * i)) & 0xff));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type: RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk("IHDR", ihdr);
    std::vector<std::uint8_t> raw;
    raw.reserve(std::size_t(h) * (std::size_t(w) * 4 + 1));
    std::vector<std::uint8_t> prev(std::size_t(w) * 4, 0), cur(std::size_t(w) * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            for (int c = 0; c < 4; ++c) {
                const std::uint8_t a = (x == 0) ? 0 : cur[x * 4 + c - 4];
                cur[x * 4 + c] = px[(std::size_t(y) * w + x) * 4 + c];
                cur[x * 4 + c] = static_cast<std::uint8_t>(cur[x * 4 + c] - a);
            }
        }
        raw.push_back(1);  // Sub filter
        raw.insert(raw.end(), cur.begin(), cur.end());
        prev = cur;
    }
    std::vector<std::uint8_t> comp;
    if (!deflateZlib(raw, comp)) return {};
    chunk("IDAT", comp);
    chunk("IEND", {});
    return out;
}

// ---------------------------------------------------------------------------
// Fresh v12 container writer (single #FT4 savepoint).
// ---------------------------------------------------------------------------

namespace {

void putU16le(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v & 0xff));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}

void putU32le(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

void putU64le(std::vector<std::uint8_t>& o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

}  // namespace

std::vector<std::uint8_t> writeContainer(const std::vector<OutEntry>& entries,
                                         const std::vector<std::uint8_t>& thumbPng,
                                         std::uint64_t creationDate) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {0x00, 0xff, 0x4b, 0x41});
    putU16le(out, 12);  // unified .af container version
    putU16le(out, 0);   // flags
    out.insert(out.end(), {'P', 'r', 's', 'n'});
    out.insert(out.end(), {'#', 'I', 'n', 'f'});
    const std::size_t infAt = out.size();
    out.insert(out.end(), 8 * 5, 0);  // fat, thumb, length, unknown, date
    putU32le(out, 1);                 // revision
    putU32le(out, static_cast<std::uint32_t>(entries.size() + 1));  // num
    out.insert(out.end(), {'P', 'r', 'o', 't'});
    putU32le(out, 12);  // protocol revision

    // #Fil payloads (first directly after the header, rest with sentinel).
    std::vector<std::uint64_t> offsets;
    offsets.reserve(entries.size());
    std::uint64_t totalComp = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) out.insert(out.end(), 4, 0xff);
        offsets.push_back(out.size());
        out.insert(out.end(), {'#', 'F', 'i', 'l'});
        out.insert(out.end(), entries[i].payload.begin(), entries[i].payload.end());
        totalComp += entries[i].payload.size();
    }

    // Single #FT4 savepoint (the verified v12 directory layout).
    out.insert(out.end(), 4, 0xff);
    const std::uint64_t fatOffset = out.size();
    out.insert(out.end(), {'#', 'F', 'T', '4'});
    const std::size_t headAt = out.size();
    putU64le(out, 0);              // next FAT offset (single savepoint)
    putU64le(out, creationDate);   // savepoint
    out.insert(out.end(), 8, 0);   // thumbnail record offset (patched below)
    putU64le(out, totalComp);      // document extent (informational)
    putU64le(out, 0);              // unknown
    putU64le(out, static_cast<std::uint64_t>(entries.size()));  // row count
    const std::size_t tableSizeAt = out.size();
    putU32le(out, 0);  // directory size (patched below)
    putU32le(out, 0);  // family flags (informational)
    putU32le(out, 0);  // unknown
    putU32le(out, offsets.empty() ? 0 : static_cast<std::uint32_t>(offsets[0]));
    putU32le(out, 0);  // unknown
    // Rows: [uncompressed][compressed][crc32][compression byte | kind 0x21 |
    // u16][payload type u8][stored-bytes crc32][name]; every row but the last
    // carries a 13-byte tail naming the next row's record tag. Names are
    // always written here (the terminal row's name field is optional).
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const OutEntry& e = entries[i];
        putU64le(out, e.plainSize);
        putU64le(out, e.payload.size());
        putU32le(out, e.crc);
        out.push_back(e.comp);
        out.push_back(0x21);  // payload kind, as observed for named records
        putU16le(out, 0);
        out.push_back(0);  // payload type
        uLong ccrc = crc32(0, Z_NULL, 0);
        if (!e.payload.empty())
            ccrc = crc32(ccrc, e.payload.data(), static_cast<uInt>(e.payload.size()));
        putU32le(out, static_cast<std::uint32_t>(ccrc));
        putU16le(out, static_cast<std::uint16_t>(e.name.size()));
        out.insert(out.end(), e.name.begin(), e.name.end());
        if (i + 1 < entries.size()) {
            putU32le(out, static_cast<std::uint32_t>(i + 1));  // row id
            out.push_back(0);
            putU64le(out, offsets[i + 1]);  // next row's record tag
        }
    }
    // Trailing directory: one 14-byte summary per name prefix (c/ before d/).
    std::vector<std::pair<std::string, std::uint64_t>> dirs;
    for (const auto& e : entries) {
        const std::size_t slash = e.name.find('/');
        if (slash == std::string::npos) continue;
        const std::string prefix = e.name.substr(0, slash + 1);
        bool found = false;
        for (auto& d : dirs)
            if (d.first == prefix) {
                ++d.second;
                found = true;
            }
        if (!found) dirs.emplace_back(prefix, 1);
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        putU32le(out, 2);
        putU32le(out, static_cast<std::uint32_t>(d.second));
        putU32le(out, 0);
        out.push_back(d.first.size() > 0 ? d.first[0] : '\0');
        out.push_back(d.first.size() > 1 ? d.first[1] : '\0');
    }
    const std::uint32_t tableSize = static_cast<std::uint32_t>(out.size() - fatOffset - 59);
    out[tableSizeAt + 0] = static_cast<std::uint8_t>(tableSize & 0xff);
    out[tableSizeAt + 1] = static_cast<std::uint8_t>((tableSize >> 8) & 0xff);
    out[tableSizeAt + 2] = static_cast<std::uint8_t>((tableSize >> 16) & 0xff);
    out[tableSizeAt + 3] = static_cast<std::uint8_t>((tableSize >> 24) & 0xff);

    // Thumbnail block (offset points at the sentinel).
    std::uint64_t thumbOffset = 0;
    if (!thumbPng.empty()) {
        thumbOffset = out.size();
        out.insert(out.end(), 4, 0xff);
        out.insert(out.end(), {'T', 'h', 'm', 'b'});
        putU32le(out, 1);  // count
        putU32le(out, static_cast<std::uint32_t>(13 + thumbPng.size()));
        putU32le(out, 29);  // header constant, as observed
        putU32le(out, 0);
        putU32le(out, static_cast<std::uint32_t>(thumbPng.size()));
        out.push_back(1);
        out.insert(out.end(), thumbPng.begin(), thumbPng.end());
    }

    // Patch #Inf and the FAT's thumb mirror.
    auto patchU64 = [&](std::size_t at, std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            out[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xff);
    };
    patchU64(infAt + 0, fatOffset);
    patchU64(infAt + 8, thumbOffset);
    patchU64(infAt + 16, totalComp);
    patchU64(infAt + 32, creationDate);
    patchU64(headAt + 16, thumbOffset);
    return out;
}

// ---------------------------------------------------------------------------
// Template patch + fresh layer stack
// ---------------------------------------------------------------------------

namespace {

// Mutable field lookup by tag (null when absent or of another kind).
Value* mutField(Node* n, const char* name, Value::K kind) {
    if (!n) return nullptr;
    const std::uint32_t t = tag4(name);
    for (auto& f : n->fields)
        if (f.first == t && f.second.k == kind) return &f.second;
    return nullptr;
}

void setVecD(Node* n, const char* name, std::vector<double> v) {
    if (Value* f = mutField(n, name, Value::K::VecD)) f->vd = std::move(v);
}

void setVecI(Node* n, const char* name, std::vector<std::int32_t> v) {
    if (Value* f = mutField(n, name, Value::K::VecI)) f->vi = std::move(v);
}

struct TemplateRefs {
    Node* docN = nullptr;
    Node* spread = nullptr;
    Node* slcp = nullptr;
    Node* spmd = nullptr;
    Node* csel = nullptr;
};

bool findTemplate(Graph& g, TemplateRefs& r, std::string& why) {
    Node* root = g.root();
    Node* docN = root ? g.child(root, "DocR") : nullptr;
    if (!docN) {
        why = "template has no DocR";
        return false;
    }
    Node* spread = nullptr;
    for (Node* s : g.children(docN, "Chld")) {
        spread = s;
        break;
    }
    if (!spread) {
        why = "template has no spread";
        return false;
    }
    r.docN = docN;
    r.spread = spread;
    r.slcp = g.child(spread, "SlcP");
    r.spmd = g.child(spread, "SpMd");
    r.csel = g.child(root, "CSel");
    return true;
}

std::string uuid4(Builder& b) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08X-%04X-%04X-%04X-%012lX",
                  static_cast<std::uint32_t>(b.rand()),
                  static_cast<std::uint16_t>(b.rand()),
                  static_cast<std::uint16_t>(b.rand()),
                  static_cast<std::uint16_t>(b.rand()),
                  static_cast<unsigned long>(b.rand() & 0xFFFFFFFFFFFFull));
    return buf;
}

}  // namespace

// Layer-build context: tiled entries, skip notes, running d/N index.
struct BuildJobs {
    std::vector<OutEntry> entries;
    std::vector<std::string> skipped;
    int entryBase = 1;
    Builder* builder = nullptr;
};

namespace {

void putGooP(Graph& g, Builder& b, Node* n) {
    (void)g;
    const std::uint64_t a = b.rand();
    const std::uint64_t c = b.rand();
    Builder::put(n, "GooP",
                 Builder::vVecI({static_cast<std::int32_t>(a & 0xffffffffull),
                                 static_cast<std::int32_t>(a >> 32),
                                 static_cast<std::int32_t>(c & 0xffffffffull),
                                 static_cast<std::int32_t>(c >> 32)}));
}

// The member block every layer node opens with, in the order the reader walks
// it: transform defaults, then the two members that depend on the layer
// (blend and placement, each omitted when it holds the default), then the
// rest of the identity boilerplate. `ox`/`oy` relativize placement — a
// clipped child lives in its base's space — and `blendOverride` substitutes
// the parent's mode for such a child.
void putCommon(Graph& g, Builder& b, Node* n, const AfLayer& l, bool isGroup,
               int ox = 0, int oy = 0, const std::string* blendOverride = nullptr) {
    (void)g;
    (void)isGroup;
    const std::string& blend = blendOverride ? *blendOverride : l.blend;
    const bool moved = l.left != ox || l.top != oy;
    const float op = std::clamp(l.opacity / 255.0f, 0.0f, 1.0f);
    // A run of members that read the same for every freshly authored layer.
    auto constantRun =
        [&](std::initializer_list<std::pair<const char*, Value>> run) {
            for (const auto& [tag, value] : run) Builder::put(n, tag, value);
        };

    constantRun({{"TrCn", Builder::vI32(18)},
                 {"TrAn", Builder::vEnum(0, 0)},
                 {"TrFP", Builder::vVecD({0.0, 0.0})},
                 {"TrFV", Builder::vBool(false)}});

    // Neither of these is written when it matches the default the reader
    // falls back to: Normal blending, and no offset from the base's origin.
    if (blend != "Normal") {
        if (auto be = blendEnum(blend))
            Builder::put(n, "Blnd", Builder::vEnum(be->first, be->second));
    }
    if (moved) {
        Builder::put(n, "Xfrm",
                     Builder::vVecD({1.0, 0.0, double(l.left - ox), 0.0, 1.0,
                                     double(l.top - oy)}));
    }

    constantRun({{"SrBx", Builder::vVecD({0.0, 0.0, 0.0, 0.0})},
                 {"SrPB", Builder::vVecD({0.0, 0.0, 0.0, 0.0})}});

    // What says which layer this is and how it reads.
    Builder::put(n, "Desc", Builder::vStr(l.name));
    constantRun({{"TagC", Builder::vNull31()}});
    Builder::put(n, "Visi", Builder::vBool(l.visible));
    Builder::put(n, "Opac", Builder::vF32(op));
    Builder::put(n, "FOpc", Builder::vF32(op));

    Builder::putArr(n, "FiEf", 0xb1, {});
    constantRun({{"Edtb", Builder::vBool(true)},
                 {"MEtb", Builder::vBool(true)},
                 {"Data", Builder::vNull31()},
                 {"AncD", Builder::vNull31()}});
    putGooP(g, b, n);
    constantRun({{"deco", Builder::vBool(false)}});
    Builder::putArr(n, "Frst", 0xab, {});
    Builder::putArr(n, "Scnd", 0xab, {});
    constantRun({{"TWSt", Builder::vEnum(0, 0)},
                 {"TWBo", Builder::vVecD({0.0, 0.0, 0.0, 0.0})},
                 {"CLiT", Builder::vEnum(0, 0)},
                 {"avin", Builder::vU32(1073741823u)}});
}

Node* matxIdentity(Graph& g, Builder& b) {
    Node* m = b.nodeTagged(tag4("Matx"), 1);
    Builder::put(m, "Rows", Builder::vI32(3));
    Builder::put(m, "Cols", Builder::vI32(3));
    Value d;
    d.k = Value::K::Array;
    d.wire = 0x8a;
    for (double v : {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}) {
        Value e;
        e.k = Value::K::F64;
        e.f = v;
        d.arr.push_back(std::move(e));
    }
    Builder::put(m, "Data", std::move(d));
    (void)g;
    return m;
}

Node* perspUnit(Graph& g, Builder& b) {
    Node* q = b.nodeTagged(tag4("Quad"), 1);
    const char* xs[4] = {"X0  ", "X1  ", "X2  ", "X3  "};
    const char* ys[4] = {"Y0  ", "Y1  ", "Y2  ", "Y3  "};
    const double xv[4] = {0.0, 1.0, 1.0, 0.0};
    const double yv[4] = {0.0, 0.0, 1.0, 1.0};
    for (int i = 0; i < 4; ++i) Builder::put(q, xs[i], Builder::vF64(xv[i]));
    for (int i = 0; i < 4; ++i) Builder::put(q, ys[i], Builder::vF64(yv[i]));
    Node* p = b.nodeTagged(tag4("Per*"), 1);
    Value pa;
    pa.k = Value::K::Array;
    pa.wire = 0xb2;
    pa.refTag = tag4("Quad");
    pa.refId = 1;
    Value e;
    e.k = Value::K::Class;
    e.cls = q;
    e.wire = 0x32;
    pa.arr.push_back(std::move(e));
    Builder::put(p, "Pers", std::move(pa));
    Builder::put(p, "Curr", Builder::vI32(0));
    (void)g;
    return p;
}

void putRasterTail(Graph& g, Builder& b, Node* n, bool extE) {
    Builder::put(n, "CMsk", Builder::vI32(31));
    Builder::put(n, "ProT", Builder::vEnum(0, 0));
    Builder::put(n, "Unpr", Builder::vNull31());
    Value w;
    w.k = Value::K::Class;
    w.cls = matxIdentity(g, b);
    w.wire = 0x32;
    w.refTag = tag4("Matx");
    w.refId = 1;
    Builder::put(n, "WRot", std::move(w));
    Builder::put(n, "Lati", Builder::vF64(90.0));
    Builder::put(n, "Long", Builder::vF64(90.0));
    Builder::put(n, "Roll", Builder::vF64(0.0));
    Builder::put(n, "FOV ", Builder::vF64(75.0));
    Value pp;
    pp.k = Value::K::Class;
    pp.cls = perspUnit(g, b);
    pp.wire = 0x32;
    pp.refTag = tag4("Per*");
    pp.refId = 1;
    Builder::put(n, "Psp*", std::move(pp));
    Builder::put(n, "ExtE", Builder::vBool(extE));
}

// One combined doc-space coverage grid (layer extent, reveal outside)
// multiplied from the layer's masks.
std::vector<std::uint8_t> bakeMaskCoverage(const AfLayer& l) {
    std::vector<std::uint8_t> cov(std::size_t(l.width) * l.height, 255);
    for (std::uint32_t y = 0; y < l.height; ++y) {
        const std::int64_t docY = std::int64_t(l.top) + y;
        for (std::uint32_t x = 0; x < l.width; ++x) {
            const std::int64_t docX = std::int64_t(l.left) + x;
            std::uint32_t c = 255;
            for (const AfMask& m : l.masks) {
                if (m.width == 0 || m.height == 0) continue;
                if (m.px.size() != std::size_t(m.width) * m.height) continue;
                const std::int64_t mx = docX - m.left;
                const std::int64_t my = docY - m.top;
                if (mx < 0 || my < 0 || std::uint64_t(mx) >= m.width ||
                    std::uint64_t(my) >= m.height)
                    continue;
                c = c * m.px[std::size_t(my) * m.width + std::size_t(mx)] / 255u;
            }
            cov[std::size_t(y) * l.width + x] = static_cast<std::uint8_t>(c);
        }
    }
    return cov;
}

// Mask node (MRst) for a pre-baked coverage grid in a bitmap space placed
// at (left, top): layer masks use the layer's own bitmap space, group
// masks the full canvas.
Node* buildMask(Graph& g, Builder& b, const std::vector<std::uint8_t>& cov,
                std::uint32_t w, std::uint32_t h, int left, int top, BuildJobs& jobs) {
    TiledPlanes t = tileMask8(g, cov.data(), w, h, jobs.entryBase);
    for (auto& e : t.entries) jobs.entries.push_back(std::move(e));
    Node* bitm = bitmapNode(g, b, 6, w, h, t);
    Node* m = b.node31({{tag4("MRst"), 1}, {tag4("EncR"), 0}, {tag4("Rstr"), 1},
                       {tag4("Node"), 0}});
    Builder::put(m, "ComO", Builder::vEnum(0, 0));
    AfLayer ml;
    ml.name = "";
    ml.visible = true;
    ml.opacity = 255;
    ml.blend = "Normal";
    ml.left = left;
    ml.top = top;
    putCommon(g, b, m, ml, false);
    Builder::put(m, "Bitm", Builder::vClass(bitm));
    Builder::put(m, "BitR",
                 Builder::vVecI({0, 0, std::int32_t(w), std::int32_t(h)}));
    Builder::put(m, "BitI",
                 Builder::vVecI({0, 0, std::int32_t(w), std::int32_t(h)}));
    putRasterTail(g, b, m, false);
    return m;
}

// Canvas-space coverage from doc-space masks (reveal outside every mask).
std::vector<std::uint8_t> bakeMaskCanvas(const std::vector<AfMask>& masks, std::uint32_t w,
                                         std::uint32_t h) {
    std::vector<std::uint8_t> cov(std::size_t(w) * h, 255);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t c = 255;
            for (const AfMask& m : masks) {
                if (m.width == 0 || m.height == 0) continue;
                if (m.px.size() != std::size_t(m.width) * m.height) continue;
                const std::int64_t mx = std::int64_t(x) - m.left;
                const std::int64_t my = std::int64_t(y) - m.top;
                if (mx < 0 || my < 0 || std::uint64_t(mx) >= m.width ||
                    std::uint64_t(my) >= m.height)
                    continue;
                c = c * m.px[std::size_t(my) * m.width + std::size_t(mx)] / 255u;
            }
            cov[std::size_t(y) * w + x] = static_cast<std::uint8_t>(c);
        }
    }
    return cov;
}

Node* buildRstr(Graph& g, Builder& b, const AfLayer& l,
                const std::vector<const AfLayer*>& clips, BuildJobs& jobs, int ox = 0,
                int oy = 0) {
    // Unknown blends fold to Normal here (covering bases and clips alike)
    // so the file never claims an effect it cannot render.
    std::string blend = l.blend;
    if (blend != "Normal" && !blendEnum(blend)) {
        jobs.skipped.push_back("blend of '" + l.name + "' has no Affinity equivalent");
        blend = "Normal";
    }
    // Pixels: RGBA16 straight to 8-bit per channel sample.
    std::vector<std::uint8_t> px(std::size_t(l.width) * l.height * 4);
    for (std::size_t i = 0, n = std::size_t(l.width) * l.height; i < n; ++i) {
        for (int c = 0; c < 4; ++c) {
            const std::uint32_t v = l.rgba[i * 4 + c];
            px[i * 4 + c] = static_cast<std::uint8_t>((v * 255u + 32767u) / 65535u);
        }
    }
    TiledPlanes t = tileRgba8(g, px.data(), l.width, l.height, jobs.entryBase);
    for (auto& e : t.entries) jobs.entries.push_back(std::move(e));
    Node* bitm = bitmapNode(g, b, 0, l.width, l.height, t);

    Node* n = b.node31({{tag4("Rstr"), 1}, {tag4("Node"), 0}});
    putCommon(g, b, n, l, false, ox, oy, &blend);
    if (!clips.empty()) {
        std::vector<Value> items;
        for (const AfLayer* c : clips) {
            // Clipped children live in the base's space: placement relative
            // to the base origin (both are axis-aligned placed rects).
            Node* cn = buildRstr(g, b, *c, {}, jobs, l.left, l.top);
            if (cn) items.push_back(Builder::vClass(cn));
        }
        Builder::putArr(n, "Chld", 0xb1, std::move(items));
    }
    if (!l.masks.empty()) {
        std::vector<std::uint8_t> cov = bakeMaskCoverage(l);
        Node* mn = buildMask(g, b, cov, l.width, l.height, l.left, l.top, jobs);
        std::vector<Value> items;
        items.push_back(Builder::vClass(mn));
        Builder::putArr(n, "AdCh", 0xb1, std::move(items));
    }
    Builder::put(n, "Bitm", Builder::vClass(bitm));
    Builder::put(n, "BitR",
                 Builder::vVecI({0, 0, std::int32_t(l.width), std::int32_t(l.height)}));
    Builder::put(n, "BitI",
                 Builder::vVecI({0, 0, std::int32_t(l.width), std::int32_t(l.height)}));
    putRasterTail(g, b, n, true);
    return n;
}

Node* buildGrup(Graph& g, Builder& b, const AfLayer& l, std::vector<Value> children,
                const AfLayersDoc& doc, BuildJobs& jobs) {
    Node* n = b.node31({{tag4("Grup"), 1}, {tag4("LogN"), 0}});
    putCommon(g, b, n, l, true);
    if (!l.masks.empty() && doc.width > 0 && doc.height > 0 &&
        std::uint64_t(doc.width) * doc.height <= kMaxPixels) {
        // Group masks cover the canvas (the group has no bitmap of its own).
        std::vector<std::uint8_t> cov = bakeMaskCanvas(l.masks, doc.width, doc.height);
        Node* mn = buildMask(g, b, cov, doc.width, doc.height, 0, 0, jobs);
        std::vector<Value> items;
        items.push_back(Builder::vClass(mn));
        Builder::putArr(n, "AdCh", 0xb1, std::move(items));
    }
    Builder::putArr(n, "Chld", 0xb1, std::move(children));
    Builder::put(n, "ComO", Builder::vEnum(0, 0));
    return n;
}

// Recursive descent over file order (bottom -> top): groups nest by indent,
// clipped runs fold into their base's Chld. Returns file-order nodes.
std::vector<Value> buildLevel(Graph& g, Builder& b, const std::vector<AfLayer>& layers,
                              std::size_t& pos, int level, BuildJobs& jobs,
                              const AfLayersDoc& doc) {
    std::vector<Value> out;
    while (pos < layers.size()) {
        const AfLayer& l = layers[pos];
        if (l.indent < level) break;
        // Clamp malformed indent jumps onto the current level.
        if (l.isGroup) {
            ++pos;
            std::vector<Value> kids = buildLevel(g, b, layers, pos, level + 1, jobs, doc);
            Node* gn = buildGrup(g, b, l, std::move(kids), doc, jobs);
            out.push_back(Builder::vClass(gn));
            continue;
        }
        // Pixel base: collect its clipped run (clips sit one level deep).
        const AfLayer& base = layers[pos];
        ++pos;
        std::vector<const AfLayer*> clips;
        while (pos < layers.size() && layers[pos].clipped && !layers[pos].isGroup &&
               layers[pos].indent == level) {
            clips.push_back(&layers[pos]);
            ++pos;
        }
        if (base.width == 0 || base.height == 0 || base.rgba.empty()) {
            if (!clips.empty()) {
                // Orphaned clips stay visible rather than vanishing: unclip.
                for (const AfLayer* c : clips) {
                    AfLayer u = *c;
                    u.clipped = false;
                    Node* un = buildRstr(g, b, u, {}, jobs);
                    if (un) out.push_back(Builder::vClass(un));
                }
                jobs.skipped.push_back("clipped layers of empty '" + base.name +
                                       "' unclipped to stay visible");
            } else {
                jobs.skipped.push_back("layer '" + base.name + "' has no pixels");
            }
            continue;
        }
        if (std::uint64_t(base.width) * base.height > kMaxPixels) {
            jobs.skipped.push_back("layer '" + base.name + "' exceeds the pixel cap");
            continue;
        }
        Node* rn = buildRstr(g, b, base, clips, jobs);
        if (rn) out.push_back(Builder::vClass(rn));
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (c == '\n') {
            o += "\\n";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            // Control bytes have no place in the tail manifest.
            o += ' ';
        } else {
            o += c;
        }
    }
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoder core (af_detail) + public entry point below.
// ---------------------------------------------------------------------------

std::optional<std::vector<std::uint8_t>> encodeLayersImpl(
    const AfLayersDoc& doc, const std::vector<std::uint8_t>& templateDocDat,
    const AfThumb& thumb, const std::string& title, std::uint64_t creationDate,
    std::vector<std::string>& skipped, std::string& error) {
    const std::uint32_t w = doc.width;
    const std::uint32_t h = doc.height;
    if (w == 0 || h == 0 || w > (1u << 20) || h > (1u << 20)) {
        error = "implausible canvas for Affinity export";
        return std::nullopt;
    }
    if (std::uint64_t(w) * h > kMaxPixels) {
        error = "canvas over the pixel cap";
        return std::nullopt;
    }

    Graph g;
    try {
        g = parseGraph(templateDocDat);
    } catch (const AfError& e) {
        error = std::string("export template is not a graph document: ") + e.msg;
        return std::nullopt;
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }

    TemplateRefs t;
    {
        std::string why;
        if (!findTemplate(g, t, why)) {
            error = why;
            return std::nullopt;
        }
    }

    Builder b(&g, (std::uint64_t(w) << 32) | h);
    BuildJobs jobs;
    jobs.builder = &b;

    // Canvas geometry.
    setVecD(t.docN, "DfSz", {double(w), double(h)});
    Node* spread = t.spread;
    setVecD(spread, "SprB", {0.0, 0.0, double(w), double(h)});
    if (t.slcp) setVecI(t.slcp, "SRct", {0, 0, std::int32_t(w), std::int32_t(h)});
    if (t.spmd) {
        for (Node* page : g.children(t.spmd, "PagR"))
            setVecD(page, "rctp", {0.0, 0.0, double(w), double(h)});
    }
    // Fresh spread identity + evicted composite caches.
    Builder::put(spread, "MiID", Builder::vStr(uuid4(b)));
    for (const char* cache : {"RasS", "Ras2"}) {
        if (Node* srst = g.child(spread, cache)) {
            if (g.child(srst, "Bitm")) {
                Node* fresh = b.nodeClosed(tag4("DyBm"), 1);
                Builder::put(fresh, "Frmt", Builder::vEnum(6, 0));
                Builder::put(fresh, "BmpW", Builder::vI32(std::int32_t(w)));
                Builder::put(fresh, "BmpH", Builder::vI32(std::int32_t(h)));
                Builder::put(fresh, "DelA", Builder::vBool(true));
                Builder::put(fresh, "MipM", Builder::vEnum(4, 0));
                Builder::put(fresh, "LInf", Builder::vI32(0));
                Builder::put(fresh, "TInf", Builder::vI32(0));
                Builder::put(fresh, "TWi1",
                             Builder::vI32(std::int32_t((w + 255) / 256)));
                Builder::put(fresh, "THi1",
                             Builder::vI32(std::int32_t((h + 255) / 256)));
                Builder::putArr(fresh, "Idx1", 0xb1, {});
                std::vector<Value> st;
                const std::size_t n =
                    std::size_t((w + 255) / 256) * ((h + 255) / 256);
                for (std::size_t i = 0; i < n; ++i) st.push_back(Builder::vU8(0));
                Builder::putArr(fresh, "Sta1", 0x81, std::move(st));
                // Swap the bitmap child in place.
                for (auto& f : srst->fields)
                    if (f.first == tag4("Bitm") && f.second.k == Value::K::Class) {
                        f.second.cls = fresh;
                        break;
                    }
            }
            if (Value* bi = mutField(srst, "BitI", Value::K::VecI))
                bi->vi = {0, 0, std::int32_t(w), std::int32_t(h)};
        }
    }
    // Fresh layer stack + cleared selection.
    {
        std::size_t pos = 0;
        std::vector<Value> stack = buildLevel(g, b, doc.layers, pos, 0, jobs, doc);
        Value arr;
        arr.k = Value::K::Array;
        arr.arr = std::move(stack);
        // Keep the template's array wire form.
        if (Value* old = mutField(spread, "Chld", Value::K::Array))
            arr.wire = old->wire ? old->wire : 0xb1;
        else
            arr.wire = 0xb1;
        for (auto& f : spread->fields)
            if (f.first == tag4("Chld") && f.second.k == Value::K::Array) {
                f.second = std::move(arr);
                break;
            }
    }
    if (t.csel) {
        for (auto& f : t.csel->fields)
            if (f.first == tag4("Itms") && f.second.k == Value::K::Array) {
                f.second.arr.clear();
                break;
            }
    }

    std::vector<std::uint8_t> docDat;
    try {
        emitGraph(g, docDat);
    } catch (const AfError& e) {
        error = std::string("export graph serialization failed: ") + e.msg;
        return std::nullopt;
    }

    std::vector<OutEntry> entries;
    {
        OutEntry e;
        e.name = "doc.dat";
        e.plainSize = docDat.size();
#ifdef PITTORE_AF
        std::vector<std::uint8_t> comp;
        if (deflateZstd(docDat, comp) && comp.size() < docDat.size()) {
            e.payload = std::move(comp);
            e.comp = 0x02;
        } else {
            e.payload = docDat;
            e.comp = 0x00;
        }
#else
        std::vector<std::uint8_t> comp;
        if (deflateZlib(docDat, comp) && comp.size() < docDat.size()) {
            e.payload = std::move(comp);
            e.comp = 0x01;
        } else {
            e.payload = docDat;
            e.comp = 0x00;
        }
#endif
        e.crc = crc32(0, docDat.data(), static_cast<uInt>(docDat.size()));
        entries.push_back(std::move(e));
    }
    for (auto& e : jobs.entries) entries.push_back(std::move(e));

    std::vector<std::uint8_t> thumbPng;
    if (thumb.width > 0 && thumb.height > 0 &&
        thumb.rgba.size() == std::size_t(thumb.width) * thumb.height * 4)
        thumbPng = encodePngRgba8(thumb.rgba.data(), thumb.width, thumb.height);

    std::vector<std::uint8_t> file = writeContainer(entries, thumbPng, creationDate);
    // Tail manifest (title for browsers and our own metadata scan).
    const std::string json = "{\"document\":{\"title\":\"" + jsonEscape(title) +
                             "\",\"pageCount\":1}}";
    file.insert(file.end(), json.begin(), json.end());

    skipped = std::move(jobs.skipped);
    return file;
}

// ---------------------------------------------------------------------------
// Export template, built rather than bundled.
//
// afEncodeLayers() needs a document to patch: findTemplate() walks root ->
// DocR -> Chld[0], and encodeLayersImpl() then writes DfSz (DocR), SprB /
// MiID / Chld (spread), and clears CSel.Itms. Everything a real .af carries
// beyond that — style lists, swatches, asset libraries, the example layers
// the format's own files ship with — is never read, so it is not built here.
// That keeps the export path free of any third-party document and cuts about
// 12 KB of carried-along chrome from every export (measured across three
// fixtures: 16877->4661, 19704->7472, 39592->27349 bytes).
//
// SpMd (per-page geometry) and RasS/Ras2 (composite caches) are absent on
// purpose: both are null-checked by findTemplate()/the encoder, so the first
// export starts with no page or cache to invalidate.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> buildTemplateDoc() {
    Graph g;
    g.fileVersion = 2;
    g.headerExtra = 0;
    Builder b(&g, 0x9E3779B97F4A7C15ull);  // splitmix64: no randomness needed

    Node* root = b.node31({{tag4("Pers"), 1}});

    Node* docN = b.nodeClosed(tag4("DocN"), 0);
    Builder::put(docN, "DfSz", Builder::vVecD({512.0, 512.0}));
    Builder::put(docN, "TrcT", Builder::vI32(18));  // document type/depth

    Node* spread = b.node31({{tag4("Sprd"), 1}, {tag4("Node"), 0}});
    Builder::put(spread, "SprB", Builder::vVecD({0.0, 0.0, 512.0, 512.0}));
    Builder::put(spread, "MiID",
                 Builder::vStr("00000000-0000-4000-8000-000000000000"));
    Builder::put(spread, "TcpN", Builder::vI32(1));
    Builder::put(spread, "OpsF", Builder::vI32(0));
    // Empty layer stack: encodeLayersImpl() replaces this wholesale from the
    // AfLayersDoc, but the field must exist as an array for the swap to land.
    Builder::putArr(spread, "Chld", 0xb1, {});

    Builder::putArr(docN, "Chld", 0xb1, {Builder::vClass(spread)});
    Builder::put(root, "DocR", Builder::vClass(docN));

    // Optional; the encoder null-checks it, then drops any stale selection.
    Node* csel = b.nodeClosed(tag4("Sele"), 0);
    Builder::putArr(csel, "Itms", 0xb1, {});
    Builder::put(root, "CSel", Builder::vClass(csel));

    std::vector<std::uint8_t> out;
    emitGraph(g, out);
    return out;
}

}  // namespace af_detail

std::vector<std::uint8_t> afBuildTemplateDoc() {
    return af_detail::buildTemplateDoc();
}

std::optional<AfEncodeResult> afEncodeLayers(
    const AfLayersDoc& doc, const std::vector<std::uint8_t>& templateDocDat,
    const AfThumb& thumb, const std::string& title, std::uint64_t creationDate,
    std::string* error) {
    std::vector<std::string> skipped;
    std::string err;
    auto bytes = af_detail::encodeLayersImpl(doc, templateDocDat, thumb, title,
                                            creationDate, skipped, err);
    if (!bytes) {
        if (error) *error = err;
        return std::nullopt;
    }
    AfEncodeResult r;
    r.bytes = std::move(*bytes);
    r.skipped = std::move(skipped);
    return r;
}

}  // namespace pittore::io
