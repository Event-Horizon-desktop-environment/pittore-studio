#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pittore::io {

// .af archive (.af/.afphoto/.afdesign/.afpub). True pixels can't be rebuilt
// (tile mapping is internal), so this reads the embedded PNG previews: the tail
// holds the saved flattened render after the directory block; clear-text PNGs
// inside the data area are placed sources. We pick the tail render.
// Needs only zlib; with libzstd (PITTORE_AF) it also walks #Fil frames for
// version atoms + linked-resource list.

// Flattened .af preview: straight RGBA16 (8-bit preview x257, like PSD).
struct AfImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int depth = 8;      // source preview depth (always 8 today)
    int colorMode = 3;  // original PNG color type is promoted to RGB/RGBA
    int dpi = 0;        // pHYs dots-per-inch when the preview carries it
    std::vector<std::uint16_t> rgba;  // width*height*4
};

// One embedded PNG preview (file order).
struct AfPreview {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int dpi = 0;
    std::uint64_t offset = 0;  // byte range in the container (provenance)
    std::uint64_t size = 0;
    std::vector<std::uint16_t> rgba;
};

// A linked/placed image from the object tree.
struct AfPlacement {
    std::string path;   // as stored (often rewritten to Z:\…)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t dpi = 0;
};

struct AfDocument {
    std::string title;          // tail JSON "title"
    std::string clientVersion;  // tail JSON "clientVersion" of the app that saved it
    std::string appVersion;     // object-tree version atoms "Major.Minor.Build.Revision"
    std::uint32_t revision = 0; // highest "Revision" atom seen in the trees
    std::uint32_t pageCount = 0;
    std::uint64_t frameCount = 0;  // 0 without libzstd
    std::vector<AfPlacement> placements;
    std::vector<AfPreview> previews;  // file order; `primary` is the largest
    std::size_t primary = 0;
};

// True if `data` looks like an .af file (magic + header tags, all 1.x/2.x/3.x).
bool afProbe(const std::vector<std::uint8_t>& data);

// Reads the flattened composite (largest embedded PNG). Nullopt on failure.
std::optional<AfImage> afDecode(const std::vector<std::uint8_t>& data,
                                std::string* error = nullptr);

// Reads header, all previews, tail JSON, and (with PITTORE_AF) frame walk +
// object-tree scan. Nullopt on failure.
std::optional<AfDocument> afDecodeDocument(const std::vector<std::uint8_t>& data,
                                           std::string* error = nullptr);

// Reads an embedded PNG subset to RGBA16. Shared with af_layers for placed
// originals ("Bckg"). False + *error on failure.
bool afDecodePngRgba16(const std::uint8_t* data, std::size_t size,
                       std::uint32_t& width, std::uint32_t& height,
                       std::vector<std::uint16_t>& rgba, std::string* error = nullptr);

}  // namespace pittore::io