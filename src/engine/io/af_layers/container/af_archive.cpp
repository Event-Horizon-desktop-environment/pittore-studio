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


#ifdef PITTORE_AF
std::vector<std::uint8_t> zstdDecode(const std::uint8_t* src, std::size_t srcSize,
                                     std::size_t limit) {
    std::vector<std::uint8_t> out(limit);
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!ctx) fail("zstd: cannot create decode context");
    ZSTD_inBuffer in{src, srcSize, 0};
    ZSTD_outBuffer ob{out.data(), limit, 0};
    std::size_t rem = 1;
    while (rem != 0) {
        const std::size_t beforeIn = in.pos, beforeOut = ob.pos;
        rem = ZSTD_decompressStream(ctx, &ob, &in);
        if (ZSTD_isError(rem)) {
            const std::string err = ZSTD_getErrorName(rem);
            ZSTD_freeDCtx(ctx);
            fail("zstd: " + err);
        }
        if (rem != 0 && in.pos == beforeIn && ob.pos == beforeOut) break;  // stalled
        if (ob.pos == ob.size) break;                                      // cap reached
    }
    ZSTD_freeDCtx(ctx);
    out.resize(ob.pos);
    return out;
}
#endif


void undoDeltaU8(std::uint8_t* p, std::size_t n) {
    std::uint8_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc = static_cast<std::uint8_t>(acc + p[i]);
        p[i] = acc;
    }
}

void undoDeltaU16(std::uint8_t* p, std::size_t n) {
    if (n % 2 != 0) fail("predicted u16 stream has an odd length");
    std::uint16_t acc = 0;
    for (std::size_t i = 0; i < n; i += 2) {
        const std::uint16_t v = std::uint16_t(p[i]) | (std::uint16_t(p[i + 1]) << 8);
        acc = static_cast<std::uint16_t>(acc + v);
        p[i] = static_cast<std::uint8_t>(acc & 0xff);
        p[i + 1] = static_cast<std::uint8_t>(acc >> 8);
    }
}

void undoDeinterleave16(std::uint8_t* p) {
    std::vector<std::uint8_t> out(0x10000);
    for (std::size_t k = 0; k < 0x4000; ++k) {
        out[4 * k + 1] = p[2 * k];
        out[4 * k + 3] = p[2 * k + 1];
        out[4 * k] = p[0x8000 + 2 * k];
        out[4 * k + 2] = p[0x8000 + 2 * k + 1];
    }
    std::memcpy(p, out.data(), out.size());
}


Archive Archive::parse(const std::vector<std::uint8_t>& bytes) {
    Archive a;
    a.data = bytes.data();
    a.size = bytes.size();
    Cursor c(bytes.data(), bytes.size());
    const std::uint8_t* magic = c.take(4);
    if (!(magic[0] == 0x00 && magic[1] == 0xff && magic[2] == 0x4b && magic[3] == 0x41))
        fail("not an Affinity container (bad magic)");
    a.version = c.u16();
    const std::uint16_t flags = c.u16();
    c.u32();  // class tag ("Prsn")
    if (a.version < 7 || a.version > 12)
        fail("unsupported Affinity container version " + std::to_string(a.version));
    if (flags & 3) fail("unsupported container flags");
    if (c.u32() != letag4("#Inf")) fail("missing #Inf block");
    const std::uint64_t fatOffset = c.u64();
    const std::uint64_t thumbOffset = c.u64();
    const std::uint64_t length = c.u64();
    c.u64();  // unknown
    c.u64();  // creation date
    c.u32();  // revision
    c.u32();  // num
    if (a.version > 7) {
        if (c.u32() != letag4("Prot")) fail("missing Prot block");
        c.u32();  // protocol revision
    }
    if (fatOffset > bytes.size() || thumbOffset > bytes.size())
        fail("container pointers are out of range");
    (void)length;  // informational only (not the file length; ignored on read)

    std::size_t offset = static_cast<std::size_t>(fatOffset);
    std::set<std::size_t> visited;
    std::uint32_t blocks = 0;
    // Synthetic ids for directory entries that carry no on-disk id.
    std::uint32_t nextSyntheticId = 0x40000000u;
    while (offset != 0) {
        if (++blocks > 10000) fail("FAT chain does not terminate");
        if (offset > bytes.size() || bytes.size() - offset < 8) fail("FAT block is truncated");
        if (visited.count(offset)) break;
        visited.insert(offset);
        Cursor fc(bytes.data() + offset, bytes.size() - offset);
        const std::uint32_t fatTag = fc.u32();
        if (!(fatTag == letag4("#FAT") || fatTag == letag4("#FT2") || fatTag == letag4("#FT3") ||
              fatTag == letag4("#FT4")))
            fail("bad FAT block tag " + tagName(fatTag));
        const std::uint64_t nextOffset = fc.u64();
        const std::uint64_t savepoint = fc.u64();
        if (fatTag == letag4("#FT4")) {
            // v12 directory (verified against real files): three informational
            // u64s, a u64 row count, five u32 fields, then rows from +72.
            const std::uint64_t blockEnd = fc.u64();  // end of this directory
            fc.u64();                                 // document extent
            fc.u64();                                 // unknown
            const std::uint64_t rowCount = fc.u64();
            const std::uint32_t dirSize = fc.u32();   // block length - 59
            fc.u32();                                  // family flags
            fc.u32();                                  // unknown
            const std::uint32_t firstTag = fc.u32();   // first row's record tag
            const std::uint32_t firstKind = fc.u32();  // 0 = rows; low byte = delta element kind

            // Rows are [uncompressed][compressed][crc32][compression byte |
            // payload kind u8 | u16][payload type u8][stored-bytes crc32],
            // an optional name (1..64 printable bytes; the terminal document
            // row may omit it), and — for every row but the last — a 13-byte
            // tail naming the next row's record tag. Records are "#Fil" plus
            // the compressed payload at the tag. Parse into a scratch list so
            // a damaged table (crash leftovers) is dropped whole instead of
            // poisoning the archive.
            std::vector<ArchiveEntry> rows;
            bool sane = true;
            std::uint64_t tag = firstTag;
            for (std::uint64_t i = 0; i < rowCount && sane; ++i) {
                if (fc.remaining() < 29) {
                    sane = false;
                    break;
                }
                ArchiveEntry e;
                e.savepoint = savepoint;
                e.offset = tag;
                e.size = fc.u64();
                e.compressedSize = fc.u64();
                e.crc32 = fc.u32();
                const std::uint32_t rowFlags = fc.u32();
                e.compression = static_cast<std::uint8_t>(rowFlags & 0xff);
                fc.u8();   // payload kind
                fc.u32();  // checksum of the stored bytes
                if (fc.remaining() >= 2) {
                    const std::size_t save = fc.pos;
                    const std::uint16_t len = fc.u16();
                    if (len >= 1 && len <= 64 && fc.remaining() >= len) {
                        const std::uint8_t* nb = fc.take(len);
                        bool printable = true;
                        for (std::size_t k = 0; k < len; ++k)
                            if (nb[k] < 0x20 || nb[k] > 0x7e) {
                                printable = false;
                                break;
                            }
                        if (printable)
                            e.name = std::string(reinterpret_cast<const char*>(nb), len);
                    }
                    if (!e.name) fc.seek(save);
                }
                std::uint64_t nextTag = 0;
                if (i + 1 < rowCount) {
                    if (fc.remaining() < 13) {
                        sane = false;
                        break;
                    }
                    fc.u32();  // row id
                    fc.u8();   // unknown
                    nextTag = fc.u64();
                }
                rows.push_back(std::move(e));
                tag = nextTag;
            }
            if (sane) {
                // The block ends where its own directory records end: the
                // stored end first, the derived length as a backup. What
                // remains after the rows is a multiple of the 14-byte family
                // summaries (c/, d/, ...).
                std::uint64_t end = blockEnd;
                if (end < offset || end > bytes.size()) {
                    end = offset + dirSize + 59;
                    if (end < offset || end > bytes.size()) end = bytes.size();
                }
                if (end < offset + fc.pos || (end - offset - fc.pos) % 14 != 0) sane = false;
            }
            if (!sane && ((firstKind & 0xff) == 1 || (firstKind & 0xff) == 2)) {
                // Delta/checkpoint directory (tile-commit saves): after the
                // same 64-byte header comes a sequence of [u32 serial][u8 kind]
                // elements instead of rows. kind 2 marks an entry carried over
                // unchanged (its metadata lives in older blocks); kind 1 is an
                // inline struct for the entry — observed only for the document
                // (serial 1): [off u64][uncompressed][compressed][crc32]
                // [compression | payload kind | u16][payload type u8]
                // [stored-bytes crc32]. Family summaries close the block
                // exactly as in row directories. Parse into the same scratch
                // list so a damaged block is still dropped whole.
                rows.clear();
                sane = true;
                fc.seek(64);
                std::uint64_t end = blockEnd;
                if (end < offset || end > bytes.size()) {
                    end = offset + dirSize + 59;
                    if (end < offset || end > bytes.size()) end = bytes.size();
                }
                const std::size_t blockLen = static_cast<std::size_t>(end - offset);
                const auto u32At = [&](std::size_t rel) {
                    std::uint32_t v;
                    std::memcpy(&v, bytes.data() + offset + rel, 4);
                    return v;
                };
                // One or more family summaries: [u32 prefixLen][u64 counter]
                // [prefix bytes]. Returns their exact total length, or 0 if
                // the bytes at rel are not a run of summaries.
                const auto trailerRun = [&](std::size_t rel) -> std::size_t {
                    std::size_t p = rel;
                    while (p < blockLen) {
                        if (blockLen - p < 12) return 0;
                        const std::uint32_t plen = u32At(p);
                        if (plen < 1 || plen > 8 || blockLen - p < 12 + plen) return 0;
                        for (std::uint32_t k = 0; k < plen; ++k) {
                            const std::uint8_t ch = bytes[offset + p + 12 + k];
                            if (ch < 0x20 || ch > 0x7e) return 0;
                        }
                        p += 12 + plen;
                    }
                    return p - rel;
                };
                bool closed = false;
                while (fc.pos < blockLen) {
                    const std::size_t rem = blockLen - fc.pos;
                    const std::size_t run = trailerRun(fc.pos);
                    if (run != 0 && run == rem) {
                        closed = true;
                        break;
                    }
                    if (rem < 5) {
                        sane = false;
                        break;
                    }
                    const std::uint32_t serial = fc.u32();
                    const std::uint8_t kind = fc.u8();
                    if (kind == 2) continue;  // token: metadata lives in older blocks
                    if (kind != 1 || rem < 42) {
                        sane = false;
                        break;
                    }
                    ArchiveEntry e;
                    e.savepoint = savepoint;
                    e.offset = fc.u64();
                    e.size = fc.u64();
                    e.compressedSize = fc.u64();
                    e.crc32 = fc.u32();
                    const std::uint32_t rowFlags = fc.u32();
                    e.compression = static_cast<std::uint8_t>(rowFlags & 0xff);
                    fc.u8();   // payload kind
                    fc.u32();  // checksum of the stored bytes
                    if (serial == 1) rows.push_back(std::move(e));  // the document
                }
                if (!closed) sane = false;
            }
            for (std::size_t i = 0; sane && i < rows.size(); ++i) {
                if (rows[i].offset + 4 > bytes.size() ||
                    std::memcmp(bytes.data() + rows[i].offset, "#Fil", 4) != 0)
                    sane = false;
            }
            if (sane) {
                for (ArchiveEntry& e : rows) {
                    std::optional<std::string> nm = e.name;
                    if (!nm && (e.compression & 0x40) == 0) nm = std::string("doc.dat");
                    auto it = nm ? a.names.find(*nm) : a.names.end();
                    if (it != a.names.end()) {
                        e.id = it->second;
                    } else {
                        e.id = nextSyntheticId++;
                        if (nm) a.names.emplace(*nm, e.id);
                    }
                    e.name = nm;
                    e.flag = 0;
                    a.entries.push_back(std::move(e));
                }
            }
            offset = static_cast<std::size_t>(nextOffset);
            continue;
        }
        fc.u64();  // some offset
        fc.u64();  // some length
        fc.u64();  // unknown5
        const std::uint32_t filesCount = fc.u32();
        fc.u32();  // unknown7
        fc.u32();  // unknown8
        const std::uint16_t dirsCount = fc.u16();
        fc.u8();   // unknown10
        if (filesCount > fc.remaining()) fail("FAT file count exceeds the block");
        for (std::uint32_t i = 0; i < filesCount; ++i) {
            ArchiveEntry e;
            e.id = fc.u32();
            e.flag = fc.u8();
            e.savepoint = savepoint;
            if (e.flag > 2) fail("bad entry flag " + std::to_string(e.flag));
            if (e.flag != 2) {
                e.offset = fc.u64();
                e.size = fc.u64();
                e.compressedSize = fc.u64();
                e.crc32 = fc.u32();
                e.compression = fc.u8();
                if (fatTag != letag4("#FAT")) fc.u32();  // unknown6
                if (fatTag == letag4("#FT4")) fc.u32();  // unknown
                // Early FATs store an algorithm index; normalize to full form.
                if (fatTag == letag4("#FAT") || fatTag == letag4("#FT2")) {
                    switch (e.compression) {
                        case 0x01: e.compression = 0x01; break;
                        case 0x02: e.compression = 0x41; break;
                        case 0x03: e.compression = 0x81; break;
                        case 0x04: e.compression = 0xc1; break;
                        default: e.compression = 0;
                    }
                }
            }
            if (e.flag == 0) {
                const std::uint16_t len = fc.u16();
                const std::uint8_t* nb = fc.take(len);
                e.name = std::string(reinterpret_cast<const char*>(nb), len);
                auto it = a.names.find(*e.name);
                if (it != a.names.end() && it->second != e.id)
                    fail("entry name maps to two ids: " + *e.name);
                if (it == a.names.end()) a.names.emplace(*e.name, e.id);
            }
            a.entries.push_back(std::move(e));
        }
        for (std::uint16_t i = 0; i < dirsCount; ++i) {
            const std::uint16_t ln = fc.u16();
            const std::uint16_t some = fc.u16();
            fc.u64();
            fc.take(ln);
            if (some != 0) fail("unexpected directory payload");
        }
        offset = static_cast<std::size_t>(nextOffset);
    }
    // Head revision per id, preferring the later-listed savepoint on a tie.
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        const ArchiveEntry& e = a.entries[i];
        auto h = a.heads.find(e.id);
        if (h == a.heads.end()) {
            a.heads.emplace(e.id, i);
        } else if (e.savepoint >= a.entries[h->second].savepoint) {
            h->second = i;
        }
    }
    return a;
}


std::vector<std::uint8_t> Archive::extract(const ArchiveEntry& e) const {
    if (e.size == 0) fail("empty entry");
    if (e.size > kMaxEntrySize) fail("entry declares more than the size limit");
    Cursor c(data, size);
    c.seek(static_cast<std::size_t>(e.offset));
    if (c.u32() != letag4("#Fil")) fail("entry does not point at a #Fil block");

    const std::uint8_t algorithm = e.compression & 3;
    std::uint8_t predictFlag = (e.compression >> 5) & 1;
    std::uint8_t predictType = 0;
    switch (e.compression & 0xc0) {
        case 0x40: predictFlag = 0; predictType = 1; break;
        case 0x80: predictType = 2; break;
        case 0xc0: predictType = 3; break;
        default: predictFlag = 0; predictType = 0;
    }

    std::vector<std::uint8_t> plain;
    if (algorithm == 1) {
        const std::uint8_t* raw = c.take(static_cast<std::size_t>(e.compressedSize));
        plain.resize(static_cast<std::size_t>(e.size));
        uLongf destLen = static_cast<uLongf>(e.size);
        const int rc =
            uncompress(plain.data(), &destLen, raw, static_cast<uLong>(e.compressedSize));
        if (rc != Z_OK) fail("zlib: entry did not inflate");
        plain.resize(destLen);
    } else if (algorithm == 2) {
#ifdef PITTORE_AF
        const std::uint8_t* raw = c.take(static_cast<std::size_t>(e.compressedSize));
        plain = zstdDecode(raw, static_cast<std::size_t>(e.compressedSize),
                           static_cast<std::size_t>(e.size) + 1);
#else
        fail("entry needs zstd, which this build lacks (PITTORE_AF)");
#endif
    } else {
        const std::uint8_t* raw = c.take(static_cast<std::size_t>(e.size));
        plain.assign(raw, raw + e.size);
    }
    if (plain.size() != e.size)
        fail("entry decompressed to " + std::to_string(plain.size()) + " bytes, expected " +
             std::to_string(e.size));

    if (predictFlag == 0) {
        if (predictType == 1) undoDeltaU8(plain.data(), plain.size());
        if (predictType == 2) undoDeltaU16(plain.data(), plain.size());
    } else if (predictType == 2) {
        undoDeltaU8(plain.data(), plain.size());
        if (plain.size() == 0x10000) undoDeinterleave16(plain.data());
    }

    const uLong crc =
        crc32(0, reinterpret_cast<const Bytef*>(plain.data()), static_cast<uInt>(plain.size()));
    if (static_cast<std::uint32_t>(crc) != e.crc32) fail("CRC mismatch on entry");
    return plain;
}

}  // namespace af_detail
}  // namespace pittore::io
