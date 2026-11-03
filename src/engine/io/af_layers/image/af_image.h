#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

#include "engine/io/af_layers/graph/af_graph.h"
#include "engine/io/af_layers/geom/af_geom.h"

#ifdef PITTORE_JPEG
#include <csetjmp>
#include <jpeglib.h>
#endif
namespace pittore::io {
namespace af_detail {

// ---------------------------------------------------------------------------
// Bitmap decode (DyBm records)
// ---------------------------------------------------------------------------
using Bitmap = render::Rgba8Image;

enum class FK { Rgba, Gray, Cmyk, Lab, Mask };

struct Format {
    int bps;
    int channels;
    FK kind;
};

#ifdef PITTORE_JPEG
struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

#endif
struct Placed {
    IntRect rect;
    Bitmap img;
};

std::optional<Format> formatOf(std::uint16_t id);
void labToSrgb(float l, float a, float b, float& r, float& g, float& bl);
bool decodeImageBytes(const std::vector<std::uint8_t>& b, Bitmap& out, std::string& why);
std::string pathBasename(const std::string& p);
bool readFileBytes(const std::string& path, std::vector<std::uint8_t>& out);
std::optional<std::string> resolveLinkedPath(const std::string& raw,
                                             const std::string& baseDir);
std::string findLinkPath(const Graph& g);
std::vector<std::uint8_t> allStatuses(const Graph& g, const Node* bitm);
bool bitmapHasContent(const Graph& g, const Node* bitm);
std::vector<std::pair<std::size_t, std::size_t>> tileOffsets(std::size_t gridWidth,
                                                             std::size_t height);
void fillTile(std::vector<std::uint8_t>& plane, std::size_t pitch, std::size_t x, std::size_t y,
              const std::vector<std::uint8_t>& pattern);
void copySourceTile(std::vector<std::uint8_t>& plane, std::size_t pitch, std::size_t rows,
                    std::size_t bps, const Bitmap& source, std::size_t channel, std::size_t x,
                    std::size_t y);
std::optional<std::vector<std::uint8_t>> tilePayload(std::vector<std::uint8_t> data);
Bitmap interleave(const std::vector<std::vector<std::uint8_t>>& planes, std::size_t pitch,
                  std::size_t w, std::size_t h, std::size_t bps, FK kind);
std::optional<Placed> affineResample(const Bitmap& img, const Mat& map);
std::optional<Placed> placeRaster(const Mat& map, Bitmap img);
// Crop a bitmap that currently occupies `at` to the document-space box `box`.
// The returned rect is the intersection; a bitmap smaller than the box keeps
// its full extent. Returns nullopt when the two do not overlap or the source
// geometry is inconsistent.
std::optional<Placed> cropPlaced(Bitmap img, const IntRect& at, const IntRect& box);
#ifdef PITTORE_JPEG
void jpegErrorExit(j_common_ptr cinfo);
bool jpegDecode(const std::vector<std::uint8_t>& b, Bitmap& out, std::string& why);
#endif

}  // namespace af_detail
}  // namespace pittore::io
