#pragma once
// PAT pattern file decoder: minimal clean-room reader from the format's
// layout (big-endian header, packed rows). Produces a coverage-neutral
// luma tile in [0,1] where transparent texels read as white, matching the
// engine's PatternGray convention. Returns false on any malformed input;
// never reads past the buffer.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pittore::compute::brushload {

struct PatImage {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<float> gray;  // w*h luma in [0,1], row-first
    std::string name;

    bool valid() const {
        return w > 0 && h > 0 && w <= 1024 && h <= 1024 &&
               gray.size() == std::size_t(w) * h;
    }
};

// Decode PAT bytes into a luma tile. Depth 1: gray; depth 2: gray+alpha
// (transparent reads as white); depth 3: RGB luma; depth 4: RGBA luma
// blended against white by alpha. Anything else (bad magic, truncated
// header/body, zero size, unknown depth) fails cleanly.
inline bool decode_pat(const std::uint8_t* data, std::size_t n,
                       PatImage* out) {
    if (!data || !out) return false;
    out->gray.clear();
    out->name.clear();
    out->w = out->h = 0;
    if (n < 24) return false;
    auto u32be = [&](std::size_t at) {
        return (std::uint32_t(data[at]) << 24) |
               (std::uint32_t(data[at + 1]) << 16) |
               (std::uint32_t(data[at + 2]) << 8) |
               std::uint32_t(data[at + 3]);
    };
    const std::uint32_t headerSize = u32be(0);
    // const std::uint32_t version = u32be(4);  // accepted, not interpreted
    const std::uint32_t w = u32be(8);
    const std::uint32_t h = u32be(12);
    const std::uint32_t bpp = u32be(16);
    const bool magic = data[20] == 'G' && data[21] == 'P' &&
                       data[22] == 'A' && data[23] == 'T';
    if (!magic) return false;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return false;
    if (bpp < 1 || bpp > 4) return false;
    if (headerSize < 24 || headerSize > n) return false;
    std::size_t nameLen = 0;
    for (std::size_t i = 24; i < headerSize && i < n; ++i) {
        if (data[i] == 0) break;
        ++nameLen;
    }
    out->name.assign(reinterpret_cast<const char*>(data + 24), nameLen);
    const std::size_t bodyOff = headerSize;
    const std::size_t bodyLen = std::size_t(w) * h * bpp;
    if (bodyOff + bodyLen > n || bodyOff + bodyLen < bodyOff) return false;
    out->w = w;
    out->h = h;
    out->gray.assign(std::size_t(w) * h, 1.0f);
    const std::uint8_t* src = data + bodyOff;
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        const std::uint8_t* px = src + i * bpp;
        float v;
        if (bpp == 1) {
            v = float(px[0]) / 255.0f;
        } else if (bpp == 2) {
            const float a = float(px[1]) / 255.0f;
            v = float(px[0]) / 255.0f * a + (1.0f - a);
        } else if (bpp == 3) {
            v = (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.0f;
        } else {
            const float a = float(px[3]) / 255.0f;
            const float luma =
                (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.0f;
            v = luma * a + (1.0f - a);
        }
        out->gray[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    return out->valid();
}

}  // namespace pittore::compute::brushload
