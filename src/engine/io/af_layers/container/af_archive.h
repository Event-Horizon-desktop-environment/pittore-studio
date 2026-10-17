#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/io/af_layers.h"
#include "engine/render/layer_style.h"
#include "engine/text/text_engine.h"
#include "engine/vector/vector_art.h"
#include "engine/vector/vector_shape.h"

namespace pittore::io {
namespace af_detail {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------
struct AfError : std::exception {
    std::string msg;
    explicit AfError(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};
[[noreturn]] inline void fail(const std::string& m) { throw AfError(m); }

// Class/field tags are stored reversed on disk, so a little-endian read of
// the bytes equals the big-endian packing of the name ("Frmt" is read from
// "tmrF"). Container tags, by contrast, are stored literally.
constexpr std::uint32_t tag4(const char* s) {
    return (std::uint32_t(std::uint8_t(s[0])) << 24) |
           (std::uint32_t(std::uint8_t(s[1])) << 16) |
           (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}
constexpr std::uint32_t letag4(const char* s) {
    return std::uint32_t(std::uint8_t(s[0])) | (std::uint32_t(std::uint8_t(s[1])) << 8) |
           (std::uint32_t(std::uint8_t(s[2])) << 16) | (std::uint32_t(std::uint8_t(s[3])) << 24);
}

inline std::string tagName(std::uint32_t t) {
    std::string s(4, '?');
    s[0] = static_cast<char>((t >> 24) & 0xff);
    s[1] = static_cast<char>((t >> 16) & 0xff);
    s[2] = static_cast<char>((t >> 8) & 0xff);
    s[3] = static_cast<char>(t & 0xff);
    return s;
}

constexpr std::uint64_t kMaxEntrySize = 1ull << 30;  // one entry's plaintext
constexpr std::uint64_t kMaxPixels = 1ull << 28;     // one image's pixel count
constexpr std::uint32_t kMaxArray = 1u << 24;        // decoded array elements

// Bounded little-endian reader.
struct Cursor {
    const std::uint8_t* b = nullptr;
    std::size_t n = 0;
    std::size_t pos = 0;
    Cursor() = default;
    Cursor(const std::uint8_t* data, std::size_t size) : b(data), n(size) {}

    std::size_t remaining() const { return n - pos; }
    const std::uint8_t* take(std::size_t k) {
        if (k > n - pos) fail("truncated input");
        const std::uint8_t* p = b + pos;
        pos += k;
        return p;
    }
    void seek(std::size_t p) {
        if (p > n) fail("seek past end of input");
        pos = p;
    }
    std::uint8_t u8() { return take(1)[0]; }
    std::uint16_t u16() {
        const std::uint8_t* p = take(2);
        return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
    }
    std::uint32_t u32() {
        const std::uint8_t* p = take(4);
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
               (std::uint32_t(p[3]) << 24);
    }
    std::uint64_t u64() {
        const std::uint8_t* p = take(8);
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
        return v;
    }
};

// ---------------------------------------------------------------------------
// Container (00 FF 4B 41): #Inf header + #FAT/#FT2/#FT3/#FT4 savepoint chain
// ---------------------------------------------------------------------------

struct ArchiveEntry {
    std::uint32_t id = 0;
    std::uint64_t savepoint = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t compressedSize = 0;
    std::uint32_t crc32 = 0;
    std::uint8_t compression = 0;
    std::uint8_t flag = 0;  // 0 named, 1 revision, 2 deleted
    std::optional<std::string> name;
};

struct Archive {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint16_t version = 0;
    std::vector<ArchiveEntry> entries;
    std::unordered_map<std::string, std::uint32_t> names;  // name -> id
    std::unordered_map<std::uint32_t, std::size_t> heads;  // id -> best entry index

    static Archive parse(const std::vector<std::uint8_t>& bytes);
    const ArchiveEntry* head(const std::string& name) const {
        auto n = names.find(name);
        if (n == names.end()) return nullptr;
        auto h = heads.find(n->second);
        if (h == heads.end()) return nullptr;
        const ArchiveEntry& e = entries[h->second];
        return e.flag == 2 ? nullptr : &e;
    }
    std::vector<std::uint8_t> extract(const ArchiveEntry& e) const;
};

#ifdef PITTORE_AF
std::vector<std::uint8_t> zstdDecode(const std::uint8_t* src, std::size_t srcSize,
                                     std::size_t limit);
#endif
void undoDeltaU8(std::uint8_t* p, std::size_t n);
void undoDeltaU16(std::uint8_t* p, std::size_t n);
void undoDeinterleave16(std::uint8_t* p);

}  // namespace af_detail
}  // namespace pittore::io
