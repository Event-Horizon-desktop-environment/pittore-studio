#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/text/text_engine.h"
#include "engine/vector/vector_art.h"

namespace pittore::io {

// Layered Affinity decode, separate from the flat preview codec.
// The file is a small versioned filesystem (#FAT chain; entries like doc.dat,
// tiles d/1.., originals c/1..), zlib (v1) or zstd (v2), CRC-checked. doc.dat
// is an object graph; bitmaps ("DyBm") index tiles; placement is the "Xfrm"
// chain; canvas is the union of the spreads' page rects ("PagR" rctp, else
// "SprB", else "DfSz"), stacked vertically with a gap. Multi-spread documents
// keep every spread (Affinity's multi-canvas): single-spread files are
// unchanged.
// Masks stay live, effects/filters bake into pixels, shapes/paths/text rebuild
// from geometry. Adjustments are skipped + logged; the flat preview still shows
// them. Needs only zlib; v2 tiles need PITTORE_AF, source images need
// PITTORE_WEBP/JPEG — else that layer is skipped with a reason.
// Masks stay live, effects/filters bake into pixels, shapes/paths/text rebuild
// from geometry. Adjustments are skipped + logged; the flat preview still shows
// them. Needs only zlib; v2 tiles need PITTORE_AF, source images need
// PITTORE_WEBP/JPEG — else that layer is skipped with a reason.

// One mask in doc space: 8-bit coverage, 255 = show. Outside its rect it shows.
struct AfMask {
    int left = 0, top = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> px;
};

// One layer, already placed on canvas. Pixels are straight RGBA16 (8-bit x257).
// Empty for groups / skipped layers. UI overlays with unit scale.
struct AfLayer {
    int left = 0, top = 0;                // placed origin in document space
    std::uint32_t width = 0, height = 0;  // placed extent
    std::uint16_t opacity = 255;          // 0..255
    bool visible = true;
    bool clipped = false;
    int indent = 0;             // group nesting depth (0 = top level)
    bool isGroup = false;       // group folders (children follow, indented)
    bool groupExpanded = true;
    // Rebuilt glyphs from a text layer, not a raster. UI shows text thumb + label.
    bool isText = false;
    // Live text spec for plain (unrotated) layers: double-click keeps editing
    // via fonts. False when rotated or layout failed.
    bool hasText = false;
    pittore::text::TextSpec textSpec;
    float textOriginX = 0.0f;   // document-space top-left of the first line
    float textOriginY = 0.0f;
    float textFrameHeight = 0.0f;
    float textColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // straight RGBA, 0..1
    std::string name;
    std::string blend = "Normal";
    std::string sourcePath;     // placed-image original path, when present
    // True vector shape/path in layer source space. Null for rasters or when
    // effects/masks changed pixels (geometry alone would lie).
    std::shared_ptr<const pittore::vector::ArtNode> art;
    // Masks stay unbaked: UI folds them into an editable layer mask.
    std::vector<AfMask> masks;
    // width*height*4 RGBA16 (empty for groups / skipped).
    std::vector<std::uint16_t> rgba;
};

// One page (one spread's trim) in source coords. The stacked import keeps
// every spread: the canvas is the vertical stack of these rects with a gap.
struct AfPage {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    std::string source;  // "PagR", "SprB" or "DfSz"
};

// Layered doc in FILE order (bottom -> top). UI reverses for its panel.
struct AfLayersDoc {
    std::uint32_t width = 0, height = 0;  // stacked canvas (pages + gaps)
    int depth = 8;                        // tiles are 8-bit, expanded to 16
    std::vector<AfLayer> layers;
    // One entry per spread, in file order, in final canvas coords.
    std::vector<AfPage> pages;
    // DyBm bitmap count, kept as `frameCount` for old log callers.
    std::uint64_t frameCount = 0;
    // True when all non-group layers made pixels. Else UI trusts flat preview.
    bool complete = false;
    std::size_t skippedLayers = 0;
    // Diagnostics for the UI log: entry list, bitmap summaries, skip reasons.
    std::vector<std::string> log;
};

// Reads layers/bitmaps. Nullopt if unparsable. Empty-but-parseable still
// returns a doc (complete=false + log).
//
// `baseDir` is where the .af lives. Linked images keep only an outside path
// (often Windows-style); when set we try to load it, else we just report it.
std::optional<AfLayersDoc> afDecodeLayers(const std::vector<std::uint8_t>& data,
                                          std::string* error = nullptr,
                                          const std::string& baseDir = {});

}  // namespace pittore::io
