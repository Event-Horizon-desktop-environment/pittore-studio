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
#include "engine/io/af_layers/container/af_archive.h"
#include "engine/io/af_layers/graph/af_graph.h"
#include "engine/io/af_layers/geom/af_geom.h"
#include "engine/io/af_layers/image/af_image.h"

namespace pittore::io {
namespace af_detail {

// The reconstructed text of a "TxtA"/"TxtF" layer, handed to `finishImageLayer`
// so the UI can keep the layer live instead of treating its glyphs as a bake.
struct TextPayload {
    pittore::text::TextSpec spec;
    float originX = 0.0f;
    float originY = 0.0f;
    float frameHeight = 0.0f;
    std::array<std::uint8_t, 4> color{0, 0, 0, 255};
};
struct Walker {
    const Archive& ar;
    const Graph& g;
    AfLayersDoc& doc;
    std::string baseDir;   // directory of the .af, for linked-source resolution
    std::unordered_map<std::string, std::optional<Bitmap>> sourceCache;
    // Count of enclosing groups with live masks. Group masks are stored on
    // their group rows (not baked), but a masked ancestor still means vector
    // geometry alone cannot reproduce a descendant.
    int maskedAncestors = 0;
    Mat nodeCtm(const Node* node, const Mat& parent) const;
    std::optional<Bitmap> sourceImage(const Node* bitm, std::uint32_t w, std::uint32_t h,
                                  std::string& why);
    std::optional<std::vector<std::uint8_t>> loadPlane(const Node* bitm, const char* sta,
                                                   const char* idx, std::size_t gridWidth,
                                                   std::size_t pitch, std::size_t rows,
                                                   std::size_t height, std::size_t bps,
                                                   const Bitmap* source, std::size_t channel,
                                                   std::string& why);
    std::optional<Bitmap> decodeBitmap(const Node* bitm, std::string& why);
    void noteSkip(const Node* node, const std::string& why);
    void applyCommon(const Node* node, AfLayer& layer);
    std::vector<AfMask> placeMasks(const Node* node, const Mat& ctm,
                              const IntRect* pad = nullptr);
    // Vector-crop shapes ("ShpN") and free paths ("PCrv") nested in a
    // layer's `AdCh` list, rasterized to live coverage masks. `pad`
    // optionally widens each mask to a footprint (the photo's placed
    // rect): masks reveal outside their own rect, so an unpadded crop
    // would show the photo past its frame.
    std::vector<AfMask> cropMasks(const Node* node, const Mat& base,
                                  const IntRect* pad = nullptr);
    void finishImageLayer(const Node* node, const Mat& ctm, int indent, bool clipped,
                      const std::string& display, const std::string& irfn,
                      render::Rgba8Image img, IntRect rect, bool isText = false,
                      const TextPayload* text = nullptr,
                      std::shared_ptr<const pittore::vector::ArtNode> art = nullptr);
    void emitRaster(const Node* node, const Mat& ctm, int indent, bool clipped);
    void emitFill(const Node* node, const Mat& ctm, int indent, bool clipped);
    void emitShape(const Node* node, const Mat& ctm, int indent, bool clipped);
    void emitText(const Node* node, const Mat& ctm, int indent, bool clipped);
    void emit(const Node* node, const Mat& ctm, int indent, bool clipped);
};

}  // namespace af_detail
}  // namespace pittore::io
