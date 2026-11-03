// af_container_probe.cpp — container-level .af inspector (NOT a test).
//
// The sibling probes answer "what document is in this file?" (af_open_probe)
// and "what records does this document hold?" (af_font_probe). This one
// answers the grammar question underneath both: how is the container laid
// out? It walks the bytes independently of Archive::parse so the two can be
// cross-checked, then reports:
//
//   72-byte tagged header  (#Inf fields, Prot, the byte-6 variant class)
//   #FT4 savepoint chain    (each block: offsets, counts, f52/f56/f60/f68)
//   v12 row directory       (rows, names, ids, 13-byte next-tag tails)
//   delta/checkpoint block  (kind-2 tokens, kind-1 inline structs, serial 1)
//   14-byte family trailers (prefix + cumulative counter)
//
// Subcommands:
//   head     <file>                header fields
//   walk     <file> [--json]       FT4 chain: blocks, rows/deltas, trailers
//   verify   <file> [--json]       walk + decode every head entry (CRC gate)
//   extract  <file> <name|id> [out]  one entry to stdout (or a file)
//   census   <dir>  [--json] [--layers]  every .af*: header + block summary
//   diff     <a> <b> [--json]      structural diff (header + named entries)
//   template [out]                 built-in export template doc.dat bytes
//   pack-template [out]            minimal .af around the built-in template
//   pack-doc <doc.dat> [out]       pack a raw docfat into a minimal .af
//   pack-layer [out]               one-pixel-layer .af through afEncodeLayers
//
// Exit status: 0 ok, 1 verify/census failure, 2 usage/IO. JSON output is
// stable-keyed for scripting; the human output is for eyeballing.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <zlib.h>
#ifdef PITTORE_AF
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

#include "engine/io/af_layers.h"
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/emit/af_write.h"

namespace fs = std::filesystem;
using pittore::io::AfLayersDoc;
using pittore::io::af_detail::Archive;
using pittore::io::af_detail::ArchiveEntry;

namespace {

int failures = 0;

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

struct Bytes {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    bool has(std::uint64_t at, std::size_t len) const {
        return at <= n && len <= n - static_cast<std::size_t>(at);
    }
    std::uint8_t u8(std::uint64_t at) const { return p[at]; }
    std::uint16_t u16(std::uint64_t at) const {
        return std::uint16_t(p[at]) | (std::uint16_t(p[at + 1]) << 8);
    }
    std::uint32_t u32(std::uint64_t at) const {
        return std::uint32_t(p[at]) | (std::uint32_t(p[at + 1]) << 8) |
               (std::uint32_t(p[at + 2]) << 16) | (std::uint32_t(p[at + 3]) << 24);
    }
    std::uint64_t u64(std::uint64_t at) const {
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[at + i];
        return v;
    }
    std::string tag(std::uint64_t at) const {
        std::string s;
        for (int i = 0; i < 4 && has(at + i, 1); ++i)
            s.push_back(static_cast<char>(p[at + i]));
        return s;
    }
};

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

struct Header {
    bool ok = false;
    std::string cls;            // class word at 8 ("nsrP" = reversed "Prsn")
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint8_t variant = 0;   // byte 6: the save-class byte (0 fresh, 4 append)
    std::uint8_t origin = 0;    // byte 7: second class byte (3 on v2→v3 converts)
    std::uint64_t o1 = 0, o2 = 0, o3 = 0, X = 0, mt = 0;
    std::uint32_t A = 0, B = 0, prot = 0;
};

Header parseHeader(const Bytes& b) {
    Header h;
    if (!b.has(0, 72)) return h;
    static const std::uint8_t kMagic[4] = {0x00, 0xff, 0x4b, 0x41};
    if (std::memcmp(b.p, kMagic, 4) != 0) return h;
    // Class tags are stored byte-reversed ("Prsn" reads as "nsrP"), so the
    // class word is reported, not matched; #Inf and Prot are literal.
    if (std::memcmp(b.p + 12, "#Inf", 4) != 0) return h;
    if (std::memcmp(b.p + 64, "Prot", 4) != 0) return h;
    h.version = b.u16(4);
    h.cls.assign(reinterpret_cast<const char*>(b.p + 8), 4);
    h.flags = b.u16(6);
    h.variant = b.u8(6);
    h.origin = b.u8(7);
    h.o1 = b.u64(16);
    h.o2 = b.u64(24);
    h.o3 = b.u64(32);
    h.X = b.u64(40);
    h.mt = b.u64(48);
    h.A = b.u32(56);
    h.B = b.u32(60);
    h.prot = b.u32(68);
    h.ok = h.version >= 7 && h.version <= 12;
    return h;
}

void printHeaderText(const Header& h, const std::string& name) {
    std::printf("%s\n", name.c_str());
    std::printf("  magic 00ff4b41  class=%s version=%u flags=0x%04x variant=%u origin=%u\n",
                h.cls.c_str(), h.version, h.flags, h.variant, h.origin);
    std::printf("  o1=%-10" PRIu64 " o2=%-10" PRIu64 " o3=%-10" PRIu64 "\n", h.o1, h.o2, h.o3);
    std::printf("  X=%-10" PRIu64 " mt=%-11" PRIu64 " A=%-6u B=%-6u prot=%u\n", h.X, h.mt,
                h.A, h.B, h.prot);
}

// ---------------------------------------------------------------------------
// FT4 chain: rows, delta elements, trailers
// ---------------------------------------------------------------------------

struct Row {
    std::uint64_t tag = 0, us = 0, cs = 0;
    std::uint32_t crc = 0, flags = 0, crc2 = 0;
    std::uint8_t comp = 0, pkind = 0, typ = 0;
    std::optional<std::string> name;
    std::uint32_t id = 0;
};

struct InlineStruct {
    std::uint32_t serial = 0;
    std::uint64_t off = 0, us = 0, cs = 0;
    std::uint32_t crcC = 0, mid = 0;
    std::uint8_t typ = 0;
    std::uint32_t crcP = 0;
};

struct Block {
    std::uint64_t off = 0, next = 0, savepoint = 0, blockEnd = 0, o3e = 0, X = 0;
    std::uint64_t rowCount = 0;
    std::uint32_t dirSize = 0, f56 = 0, f60 = 0, firstTag = 0, b68 = 0;
    enum class Kind { Rows, Delta, Bad } kind = Kind::Bad;
    std::vector<Row> rows;
    std::vector<InlineStruct> structs;
    std::vector<std::uint32_t> tokens;
    std::vector<std::pair<std::string, std::uint64_t>> trailers;
    bool blockEndInFile = false;
    bool rowsConsumeExactly = false;
};

// A trailer group is [u32 prefixLen (1..8)][u64 counter][prefix bytes].
// Returns the byte length of the whole run, or 0 when `at` is not a run.
std::size_t trailerRun(const Bytes& b, std::uint64_t off, std::uint64_t end,
                       std::vector<std::pair<std::string, std::uint64_t>>* out) {
    std::uint64_t p = off;
    while (p < end) {
        if (end - p < 12 || !b.has(p, 12)) return 0;
        const std::uint32_t plen = b.u32(p);
        if (plen < 1 || plen > 8 || end - p < 12 + plen) return 0;
        const std::uint64_t counter = b.u64(p + 4);
        std::string prefix;
        for (std::uint32_t k = 0; k < plen; ++k) {
            const std::uint8_t c = b.u8(p + 12 + k);
            if (c < 0x20 || c > 0x7e) return 0;
            prefix.push_back(static_cast<char>(c));
        }
        if (out) out->emplace_back(prefix, counter);
        p += 12 + plen;
    }
    return static_cast<std::size_t>(p - off);
}

bool walkBlock(const Bytes& b, std::uint64_t off, Block& blk) {
    blk.off = off;
    if (!b.has(off, 72) || b.tag(off) != "#FT4") return false;
    blk.next = b.u64(off + 4);
    blk.savepoint = b.u64(off + 12);
    blk.blockEnd = b.u64(off + 20);
    blk.o3e = b.u64(off + 28);
    blk.X = b.u64(off + 36);
    blk.rowCount = b.u64(off + 44);
    blk.dirSize = b.u32(off + 52);
    blk.f56 = b.u32(off + 56);
    blk.f60 = b.u32(off + 60);
    blk.firstTag = b.u32(off + 64);
    blk.b68 = b.u32(off + 68);
    const auto hasReceipt = [&](std::uint64_t at) {
        return b.has(at, 4) && b.u32(at) == 0xffffffffu;
    };
    blk.blockEndInFile = blk.blockEnd >= off && blk.blockEnd <= b.n &&
                         (blk.blockEnd == 0 || hasReceipt(blk.blockEnd));

    std::uint64_t end = blk.blockEnd;
    if (end < off || end > b.n) end = off + blk.dirSize + 59;
    if (end < off || end > b.n) end = b.n;

    // Row directory: rowCount x (29-byte core + optional name + 13-byte tail).
    {
        Block rows;
        rows = blk;
        rows.kind = Block::Kind::Rows;
        std::uint64_t p = off + 72;
        std::uint64_t tag = blk.firstTag;
        bool sane = true;
        for (std::uint64_t i = 0; i < blk.rowCount && sane; ++i) {
            if (end - p < 29 || !b.has(p, 29)) {
                sane = false;
                break;
            }
            Row r;
            r.us = b.u64(p);
            r.cs = b.u64(p + 8);
            r.crc = b.u32(p + 16);
            r.flags = b.u32(p + 20);
            r.comp = static_cast<std::uint8_t>(r.flags & 0xff);
            r.pkind = b.u8(p + 24);
            r.crc2 = b.u32(p + 25);
            r.tag = tag;
            p += 29;
            if (end - p >= 2 && b.has(p, 2)) {
                const std::uint16_t nl = b.u16(p);
                if (nl >= 1 && nl <= 64 && end - p >= 2 + std::uint64_t(nl) &&
                    b.has(p + 2, nl)) {
                    bool printable = true;
                    std::string nm;
                    for (std::uint16_t k = 0; k < nl; ++k) {
                        const std::uint8_t c = b.u8(p + 2 + k);
                        if (c < 0x20 || c > 0x7e) {
                            printable = false;
                            break;
                        }
                        nm.push_back(static_cast<char>(c));
                    }
                    if (printable) {
                        r.name = nm;
                        p += 2 + nl;
                    }
                }
            }
            if (i + 1 < blk.rowCount) {
                if (end - p < 13 || !b.has(p, 13)) {
                    sane = false;
                    break;
                }
                r.id = b.u32(p);
                tag = b.u64(p + 5);
                p += 13;
            }
            rows.rows.push_back(std::move(r));
        }
        if (sane && end >= p && (end - p) % 14 == 0) {
            const std::size_t run = trailerRun(b, p, end, &rows.trailers);
            if (run == end - p) {
                rows.rowsConsumeExactly = true;
                for (const Row& r : rows.rows)
                    if (!b.has(r.tag, 4) || b.tag(r.tag) != "#Fil") sane = false;
            } else {
                sane = false;
            }
        } else {
            sane = false;
        }
        if (sane) {
            blk.kind = Block::Kind::Rows;
            blk.rows = std::move(rows.rows);
            blk.trailers = std::move(rows.trailers);
            blk.rowsConsumeExactly = true;
            return true;
        }
    }

    // Delta/checkpoint element stream (kind byte says 1/2).
    if ((blk.b68 & 0xff) == 1 || (blk.b68 & 0xff) == 2) {
        Block d = blk;
        d.kind = Block::Kind::Delta;
        std::uint64_t p = off + 64;
        bool closed = false;
        while (p < end) {
            std::vector<std::pair<std::string, std::uint64_t>> runTrailers;
            const std::size_t run = trailerRun(b, p, end, &runTrailers);
            if (run != 0 && run == end - p) {
                d.trailers = std::move(runTrailers);
                closed = true;
                break;
            }
            if (end - p < 5 || !b.has(p, 5)) break;
            const std::uint32_t serial = b.u32(p);
            const std::uint8_t kind = b.u8(p + 4);
            if (kind == 2) {
                d.tokens.push_back(serial);
                p += 5;
                continue;
            }
            if (kind != 1 || end - p < 42) break;
            InlineStruct s;
            s.serial = serial;
            s.off = b.u64(p + 5);
            s.us = b.u64(p + 13);
            s.cs = b.u64(p + 21);
            s.crcC = b.u32(p + 29);
            s.mid = b.u32(p + 33);
            s.typ = b.u8(p + 37);
            s.crcP = b.u32(p + 38);
            d.structs.push_back(s);
            p += 42;
        }
        if (closed) {
            blk.kind = Block::Kind::Delta;
            blk.structs = std::move(d.structs);
            blk.tokens = std::move(d.tokens);
            blk.trailers = std::move(d.trailers);
            return true;
        }
    }
    blk.kind = Block::Kind::Bad;
    return false;
}

std::vector<Block> walkChain(const Bytes& b, const Header& h) {
    std::vector<Block> out;
    std::set<std::uint64_t> seen;
    std::uint64_t off = h.o1;
    while (off != 0 && seen.size() < 64) {
        if (!seen.insert(off).second) break;
        Block blk;
        if (!walkBlock(b, off, blk)) {
            Block bad;
            bad.off = off;
            bad.kind = Block::Kind::Bad;
            out.push_back(bad);
            break;
        }
        out.push_back(blk);
        off = blk.next;
    }
    return out;
}

const char* kindName(Block::Kind k) {
    switch (k) {
        case Block::Kind::Rows: return "rows";
        case Block::Kind::Delta: return "delta";
        default: return "bad";
    }
}

void printRowText(const Bytes& b, const Row& r) {
    std::string name = r.name ? *r.name : "(doc.dat?)";
    const bool fil = b.has(r.tag, 4) && b.tag(r.tag) == "#Fil";
    std::printf("      row tag=%-9" PRIu64 " %-10s us=%-8" PRIu64 " cs=%-8" PRIu64
                " crc=%08x flags=%02x/%02x comp=%02x name=%s\n",
                r.tag, fil ? "#Fil" : "NOT#Fil", r.us, r.cs, r.crc,
                (r.flags >> 8) & 0xff, r.flags & 0xff, r.comp, name.c_str());
}

void printBlockText(const Bytes& b, const Block& blk) {
    std::printf("  @%-10" PRIu64 " %-5s next=%-10" PRIu64 " savepoint=%" PRIu64
                " end=%-10" PRIu64 " rc=%-6" PRIu64 " dirSize=%-8u"
                " f56=%08x f60=%08x b68=%02x\n",
                blk.off, kindName(blk.kind), blk.next, blk.savepoint, blk.blockEnd,
                blk.rowCount, blk.dirSize, blk.f56, blk.f60, blk.b68 & 0xff);
    for (const Row& r : blk.rows) printRowText(b, r);
    for (const auto& t : blk.trailers)
        std::printf("      trailer %-4s counter=%" PRIu64 "\n", t.first.c_str(), t.second);
    if (blk.kind == Block::Kind::Delta) {
        std::printf("      delta: %zu tokens, %zu structs\n", blk.tokens.size(),
                    blk.structs.size());
        for (const InlineStruct& s : blk.structs) {
            const bool fil = b.has(s.off, 4) && b.tag(s.off) == "#Fil";
            std::printf("      struct serial=%u off=%" PRIu64 " %s us=%" PRIu64
                        " cs=%" PRIu64 " crc_c=%08x mid=%08x typ=%u crc_p=%08x\n",
                        s.serial, s.off, fil ? "#Fil" : "NOT#Fil", s.us, s.cs, s.crcC,
                        s.mid, s.typ, s.crcP);
        }
    }
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(c);
                }
        }
    }
    return o;
}

void jsonHeader(std::string& out, const Header& h) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "\"version\":%u,\"flags\":%u,\"variant\":%u,\"origin\":%u,"
                  "\"o1\":%" PRIu64 ",\"o2\":%" PRIu64 ",\"o3\":%" PRIu64 ","
                  "\"X\":%" PRIu64 ",\"mt\":%" PRIu64 ",\"A\":%u,\"B\":%u,\"prot\":%u",
                  h.version, h.flags, h.variant, h.origin, h.o1, h.o2, h.o3, h.X,
                  h.mt, h.A, h.B, h.prot);
    out += buf;
}

void jsonBlocks(std::string& out, const Bytes& b, const std::vector<Block>& blocks) {
    out += "[";
    bool firstBlock = true;
    for (const Block& blk : blocks) {
        if (!firstBlock) out += ",";
        firstBlock = false;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "{\"off\":%" PRIu64 ",\"kind\":\"%s\",\"next\":%" PRIu64 ","
                      "\"savepoint\":%" PRIu64 ",\"blockEnd\":%" PRIu64 ","
                      "\"rowCount\":%" PRIu64 ",\"dirSize\":%u,"
                      "\"f56\":%u,\"f60\":%u,\"b68\":%u,\"rows\":[",
                      blk.off, kindName(blk.kind), blk.next, blk.savepoint, blk.blockEnd,
                      blk.rowCount, blk.dirSize, blk.f56, blk.f60, blk.b68);
        out += buf;
        bool firstRow = true;
        for (const Row& r : blk.rows) {
            if (!firstRow) out += ",";
            firstRow = false;
            std::snprintf(buf, sizeof(buf),
                          "{\"tag\":%" PRIu64 ",\"fil\":%s,\"us\":%" PRIu64
                          ",\"cs\":%" PRIu64 ",\"crc\":%u,\"flags\":%u,"
                          "\"pkind\":%u,\"crc2\":%u,\"name\":%s}",
                          r.tag, (b.has(r.tag, 4) && b.tag(r.tag) == "#Fil") ? "true" : "false",
                          r.us, r.cs, r.crc, r.flags, r.pkind, r.crc2,
                          r.name ? ("\"" + jsonEscape(*r.name) + "\"").c_str() : "null");
            out += buf;
        }
        out += "],\"tokens\":[";
        for (std::size_t i = 0; i < blk.tokens.size(); ++i) {
            if (i) out += ",";
            out += std::to_string(blk.tokens[i]);
        }
        out += "],\"structs\":[";
        for (std::size_t i = 0; i < blk.structs.size(); ++i) {
            const InlineStruct& s = blk.structs[i];
            if (i) out += ",";
            std::snprintf(buf, sizeof(buf),
                          "{\"serial\":%u,\"off\":%" PRIu64 ",\"us\":%" PRIu64
                          ",\"cs\":%" PRIu64 ",\"crc_c\":%u,\"mid\":%u,\"typ\":%u,"
                          "\"crc_p\":%u}",
                          s.serial, s.off, s.us, s.cs, s.crcC, s.mid, s.typ, s.crcP);
            out += buf;
        }
        out += "],\"trailers\":{";
        bool firstT = true;
        for (const auto& t : blk.trailers) {
            if (!firstT) out += ",";
            firstT = false;
            out += "\"" + jsonEscape(t.first) + "\":" + std::to_string(t.second);
        }
        out += "}}";
    }
    out += "]";
}

// ---------------------------------------------------------------------------
// Entry CRC verification (engine path)
// ---------------------------------------------------------------------------

bool verifyEntries(const std::vector<std::uint8_t>& bytes, bool json) {
    int decodeFailures = 0;
    std::size_t checked = 0, skipped = 0;
    try {
        Archive ar = Archive::parse(bytes);
        if (!json) std::printf("  entries=%zu (heads %zu)\n", ar.entries.size(), ar.heads.size());
        for (const ArchiveEntry& e : ar.entries) {
            if (e.flag == 2) {
                ++skipped;
                continue;
            }
            ++checked;
            try {
                const std::vector<std::uint8_t> plain = ar.extract(e);
                if (!json)
                    std::printf("  %-24s off=%-9" PRIu64 " us=%-8zu cs=%-8" PRIu64
                                " comp=%02x ok\n",
                                e.name ? e.name->c_str() : "(unnamed)", e.offset,
                                plain.size(), e.compressedSize, e.compression);
            } catch (const std::exception& ex) {
                ++decodeFailures;
                std::printf("  FAIL %-22s off=%" PRIu64 ": %s\n",
                            e.name ? e.name->c_str() : "(unnamed)", e.offset, ex.what());
            }
        }
    } catch (const std::exception& ex) {
        ++decodeFailures;
        std::printf("  FAIL archive parse: %s\n", ex.what());
    }
    if (json)
        std::printf("{\"ok\":%s,\"checked\":%zu,\"skipped\":%zu,\"failures\":%d}\n",
                    decodeFailures ? "false" : "true", checked, skipped, decodeFailures);
    else
        std::printf("  verify: %zu checked, %zu deleted, %d failures\n", checked, skipped,
                    decodeFailures);
    failures += decodeFailures;
    return decodeFailures == 0;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int cmdHead(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    const Bytes b{bytes.data(), bytes.size()};
    const Header h = parseHeader(b);
    if (!h.ok) {
        std::printf("%s: not a v7..v12 Affinity container\n", path.c_str());
        return 2;
    }
    printHeaderText(h, path);
    return 0;
}

int cmdWalk(const std::string& path, bool json) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    const Bytes b{bytes.data(), bytes.size()};
    const Header h = parseHeader(b);
    if (!h.ok) {
        std::printf("%s: not a v7..v12 Affinity container\n", path.c_str());
        return 2;
    }
    const std::vector<Block> blocks = walkChain(b, h);
    if (json) {
        std::string out = "{\"file\":\"" + jsonEscape(path) + "\",";
        jsonHeader(out, h);
        out += ",\"blocks\":";
        jsonBlocks(out, b, blocks);
        out += "}\n";
        std::fputs(out.c_str(), stdout);
    } else {
        printHeaderText(h, path);
        for (const Block& blk : blocks) printBlockText(b, blk);
        std::printf("  chain: %zu block(s)\n", blocks.size());
    }
    return 0;
}

int cmdVerify(const std::string& path, bool json) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    const Bytes b{bytes.data(), bytes.size()};
    const Header h = parseHeader(b);
    if (!h.ok) {
        std::printf("%s: not a v7..v12 Affinity container\n", path.c_str());
        return 2;
    }
    if (!json) {
        printHeaderText(h, path);
        for (const Block& blk : walkChain(b, h)) printBlockText(b, blk);
    }
    verifyEntries(bytes, json);
    return failures ? 1 : 0;
}

int cmdExtract(const std::string& path, const std::string& what, const std::string& out) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes)) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    try {
        Archive ar = Archive::parse(bytes);
        const ArchiveEntry* e = ar.head(what);
        if (!e) {
            char* end = nullptr;
            const unsigned long long id = std::strtoull(what.c_str(), &end, 0);
            if (!end || *end) {
                std::printf("no entry named %s\n", what.c_str());
                return 2;
            }
            for (const ArchiveEntry& cand : ar.entries)
                if (cand.id == id && cand.flag != 2) e = &cand;
        }
        if (!e) {
            std::printf("no entry %s\n", what.c_str());
            return 2;
        }
        const std::vector<std::uint8_t> plain = ar.extract(*e);
        if (out.empty()) {
            std::fwrite(plain.data(), 1, plain.size(), stdout);
            std::fprintf(stderr, "  (id=%u off=%" PRIu64 " us=%zu cs=%" PRIu64 " comp=%02x)\n",
                         e->id, e->offset, plain.size(), e->compressedSize, e->compression);
        } else {
            std::ofstream f(out, std::ios::binary);
            f.write(reinterpret_cast<const char*>(plain.data()),
                    static_cast<std::streamsize>(plain.size()));
            std::printf("  wrote %s (id=%u off=%" PRIu64 " us=%zu cs=%" PRIu64 " comp=%02x)\n",
                        out.c_str(), e->id, e->offset, plain.size(), e->compressedSize,
                        e->compression);
        }
        return 0;
    } catch (const std::exception& ex) {
        std::printf("extract failed: %s\n", ex.what());
        return 1;
    }
}

int cmdCensus(const std::string& dir, bool json, bool layers) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file()) continue;
        const std::string ext = ent.path().extension().string();
        if (ext == ".af" || ext == ".afdesign" || ext == ".afphoto" || ext == ".afpub")
            files.push_back(ent.path().string());
    }
    std::sort(files.begin(), files.end());
    if (json) std::printf("[");
    bool first = true;
    for (const std::string& f : files) {
        std::vector<std::uint8_t> bytes;
        if (!readFile(f, bytes)) continue;
        const Bytes b{bytes.data(), bytes.size()};
        const Header h = parseHeader(b);
        const std::vector<Block> blocks = walkChain(b, h);
        bool layersOk = false;
        std::size_t layerCount = 0, skipped = 0;
        std::uint64_t frameCount = 0;
        if (layers) {
            auto doc = pittore::io::afDecodeLayers(bytes, nullptr);
            layersOk = doc.has_value();
            if (doc) {
                layerCount = doc->layers.size();
                skipped = doc->skippedLayers;
                frameCount = doc->frameCount;
            }
        }
        if (json) {
            if (!first) std::printf(",");
            first = false;
            std::string out = "{\"file\":\"" + jsonEscape(f) + "\",\"size\":" +
                              std::to_string(bytes.size()) + ",";
            jsonHeader(out, h);
            out += ",\"blocks\":";
            jsonBlocks(out, b, blocks);
            if (layers) {
                out += ",\"layers\":{\"ok\":" + std::string(layersOk ? "true" : "false") +
                       ",\"count\":" + std::to_string(layerCount) +
                       ",\"skipped\":" + std::to_string(skipped) +
                       ",\"frames\":" + std::to_string(frameCount) + "}";
            }
            out += "}";
            std::fputs(out.c_str(), stdout);
        } else {
            std::printf("%s (%zu B)\n", f.c_str(), bytes.size());
            if (h.ok) printHeaderText(h, "  header");
            for (const Block& blk : blocks) printBlockText(b, blk);
            if (layers)
                std::printf("  layers: %s count=%zu skipped=%zu frames=%" PRIu64 "\n",
                            layersOk ? "ok" : "FAILED", layerCount, skipped, frameCount);
        }
    }
    if (json) std::printf("]\n");
    return failures ? 1 : 0;
}

int cmdDiff(const std::string& pa, const std::string& pb, bool json) {
    std::vector<std::uint8_t> ba, bb;
    if (!readFile(pa, ba) || !readFile(pb, bb)) {
        std::printf("cannot read inputs\n");
        return 2;
    }
    const Bytes A{ba.data(), ba.size()}, B{bb.data(), bb.size()};
    const Header ha = parseHeader(A), hb = parseHeader(B);
    std::vector<std::string> diffs;
    auto field = [&](const char* name, std::uint64_t a, std::uint64_t b) {
        if (a != b) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %" PRIu64 " -> %" PRIu64 " (%+lld)", name, a,
                          b, static_cast<long long>(b) - static_cast<long long>(a));
            diffs.emplace_back(buf);
        }
    };
    field("o1", ha.o1, hb.o1);
    field("o2", ha.o2, hb.o2);
    field("o3", ha.o3, hb.o3);
    field("X", ha.X, hb.X);
    field("mt", ha.mt, hb.mt);
    field("A", ha.A, hb.A);
    field("B", ha.B, hb.B);
    field("prot", ha.prot, hb.prot);
    field("variant", ha.variant, hb.variant);
    field("size", ba.size(), bb.size());

    std::map<std::string, const ArchiveEntry*> ea, eb;
    std::string parseA, parseB;
    try {
        for (const auto& e : Archive::parse(ba).entries)
            if (e.name) ea[*e.name] = &e;
    } catch (const std::exception& ex) { parseA = ex.what(); }
    try {
        for (const auto& e : Archive::parse(bb).entries)
            if (e.name) eb[*e.name] = &e;
    } catch (const std::exception& ex) { parseB = ex.what(); }

    std::vector<std::string> added, removed;
    for (const auto& kv : eb)
        if (!ea.count(kv.first)) added.push_back(kv.first);
    for (const auto& kv : ea)
        if (!eb.count(kv.first)) removed.push_back(kv.first);
    std::size_t changed = 0;
    for (const auto& kv : ea) {
        auto it = eb.find(kv.first);
        if (it == eb.end()) continue;
        const ArchiveEntry& x = *kv.second;
        const ArchiveEntry& y = *it->second;
        if (x.size != y.size || x.compressedSize != y.compressedSize || x.crc32 != y.crc32 ||
            x.compression != y.compression) {
            ++changed;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "entry %-24s us %" PRIu64 "->%" PRIu64 " cs %" PRIu64
                          "->%" PRIu64 " crc %08x->%08x comp %02x->%02x",
                          kv.first.c_str(), x.size, y.size, x.compressedSize, y.compressedSize,
                          x.crc32, y.crc32, x.compression, y.compression);
            diffs.emplace_back(buf);
        }
    }
    if (json) {
        std::printf("{\"a\":\"%s\",\"b\":\"%s\",\"diffs\":[", jsonEscape(pa).c_str(),
                    jsonEscape(pb).c_str());
        for (std::size_t i = 0; i < diffs.size(); ++i)
            std::printf("%s\"%s\"", i ? "," : "", jsonEscape(diffs[i]).c_str());
        std::printf("],\"added\":[");
        for (std::size_t i = 0; i < added.size(); ++i)
            std::printf("%s\"%s\"", i ? "," : "", jsonEscape(added[i]).c_str());
        std::printf("],\"removed\":[");
        for (std::size_t i = 0; i < removed.size(); ++i)
            std::printf("%s\"%s\"", i ? "," : "", jsonEscape(removed[i]).c_str());
        std::printf("],\"changed\":%zu,\"parseErrors\":[\"%s\",\"%s\"]}\n",
                    changed, jsonEscape(parseA).c_str(), jsonEscape(parseB).c_str());
    } else {
        std::printf("%s vs %s\n", pa.c_str(), pb.c_str());
        for (const std::string& d : diffs) std::printf("  %s\n", d.c_str());
        for (const std::string& a : added) std::printf("  + %s\n", a.c_str());
        for (const std::string& r : removed) std::printf("  - %s\n", r.c_str());
        if (diffs.empty() && added.empty() && removed.empty())
            std::printf("  (no structural differences)\n");
    }
    return 0;
}

void usage() {
    std::printf(
        "usage: af_container_probe <cmd> [args]\n"
        "  head    <file.af>\n"
        "  walk    <file.af> [--json]\n"
        "  verify  <file.af> [--json]\n"
        "  extract <file.af> <name|id> [outfile]\n"
        "  census  <dir> [--json] [--layers]\n"
        "  diff    <a.af> <b.af> [--json]\n"
        "  template [outfile]   built-in export template doc.dat\n"
        "  pack-template [outfile]  minimal .af around that template\n"
        "  pack-doc <doc.dat> [outfile]  pack a raw docfat into a minimal .af\n"
        "  pack-layer [outfile]     one-pixel-layer .af via afEncodeLayers\n");
}

int cmdTemplate(const std::string& out) {
    const std::vector<std::uint8_t> bytes = pittore::io::afBuildTemplateDoc();
    if (out.empty()) {
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
        std::fprintf(stderr, "  (template doc.dat, %zu bytes)\n", bytes.size());
        return 0;
    }
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        std::printf("cannot write %s\n", out.c_str());
        return 2;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    std::printf("  wrote %s (template doc.dat, %zu bytes)\n",
                out.c_str(), bytes.size());
    return 0;
}

int writePackedDoc(const std::vector<std::uint8_t>& doc, const std::string& out) {
    using pittore::io::af_detail::OutEntry;
    using pittore::io::af_detail::writeContainer;
    OutEntry e;
    e.name = "doc.dat";
    e.payload = doc;
    e.plainSize = doc.size();
    e.comp = 0;  // stored: no compressor dependency for this path
    e.crc = static_cast<std::uint32_t>(
        crc32(0, doc.data(), static_cast<uInt>(doc.size())));
    const std::vector<std::uint8_t> file =
        writeContainer({e}, {}, 1234567890);
    if (out.empty()) {
        std::fwrite(file.data(), 1, file.size(), stdout);
        std::fprintf(stderr, "  (packed doc.dat %zu B, container %zu B)\n",
                     doc.size(), file.size());
        return 0;
    }
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        std::printf("cannot write %s\n", out.c_str());
        return 2;
    }
    f.write(reinterpret_cast<const char*>(file.data()),
            static_cast<std::streamsize>(file.size()));
    std::printf("  wrote %s (packed doc.dat %zu B, container %zu B)\n",
                out.c_str(), doc.size(), file.size());
    return 0;
}

int cmdPackTemplate(const std::string& out) {
    return writePackedDoc(pittore::io::afBuildTemplateDoc(), out);
}

int cmdPackDoc(const std::string& in, const std::string& out) {
    std::vector<std::uint8_t> doc;
    if (!readFile(in, doc)) {
        std::printf("cannot read %s\n", in.c_str());
        return 2;
    }
    return writePackedDoc(doc, out);
}

int cmdPackLayer(const std::string& out) {
    using namespace pittore::io;
    AfLayersDoc doc;
    doc.width = 8;
    doc.height = 8;
    doc.depth = 8;
    AfLayer layer;
    layer.left = 0;
    layer.top = 0;
    layer.width = 8;
    layer.height = 8;
    layer.opacity = 255;
    layer.visible = true;
    layer.indent = 0;
    layer.name = "bg";
    layer.rgba.assign(std::size_t(8) * 8 * 4, 0);
    for (std::size_t i = 0; i < std::size_t(8) * 8; ++i) {
        layer.rgba[i * 4 + 0] = 200 * 257;
        layer.rgba[i * 4 + 1] = 10 * 257;
        layer.rgba[i * 4 + 2] = 20 * 257;
        layer.rgba[i * 4 + 3] = 255 * 257;
    }
    doc.layers.push_back(layer);
    std::string error;
    const auto enc = afEncodeLayers(doc, afBuildTemplateDoc(), AfThumb{},
                                    "minimal-layer", 1234567890, &error);
    if (!enc) {
        std::printf("encode failed: %s\n", error.c_str());
        return 1;
    }
    if (out.empty()) {
        std::fwrite(enc->bytes.data(), 1, enc->bytes.size(), stdout);
        std::fprintf(stderr,
                     "  (one-layer container, %zu bytes, %zu skipped)\n",
                     enc->bytes.size(), enc->skipped.size());
        return 0;
    }
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        std::printf("cannot write %s\n", out.c_str());
        return 2;
    }
    f.write(reinterpret_cast<const char*>(enc->bytes.data()),
            static_cast<std::streamsize>(enc->bytes.size()));
    std::printf("  wrote %s (one-layer container, %zu bytes, %zu skipped)\n",
                out.c_str(), enc->bytes.size(), enc->skipped.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    bool json = false, layers = false;
    std::vector<std::string> rest;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") json = true;
        else if (arg == "--layers") layers = true;
        else rest.push_back(arg);
    }
    if (cmd == "head" && rest.size() == 1) return cmdHead(rest[0]);
    if (cmd == "walk" && rest.size() == 1) return cmdWalk(rest[0], json);
    if (cmd == "verify" && rest.size() == 1) return cmdVerify(rest[0], json);
    if (cmd == "extract" && (rest.size() == 2 || rest.size() == 3))
        return cmdExtract(rest[0], rest[1], rest.size() == 3 ? rest[2] : std::string());
    if (cmd == "census" && rest.size() == 1) return cmdCensus(rest[0], json, layers);
    if (cmd == "diff" && rest.size() == 2) return cmdDiff(rest[0], rest[1], json);
    if (cmd == "template" && rest.size() <= 1)
        return cmdTemplate(rest.empty() ? std::string() : rest[0]);
    if (cmd == "pack-template" && rest.size() <= 1)
        return cmdPackTemplate(rest.empty() ? std::string() : rest[0]);
    if (cmd == "pack-doc" && (rest.size() == 1 || rest.size() == 2))
        return cmdPackDoc(rest[0], rest.size() == 2 ? rest[1] : std::string());
    if (cmd == "pack-layer" && rest.size() <= 1)
        return cmdPackLayer(rest.empty() ? std::string() : rest[0]);
    usage();
    return 2;
}
