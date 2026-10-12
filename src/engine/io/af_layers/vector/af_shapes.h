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
namespace pittore::io {
namespace af_detail {

using ShapeSubPaths = std::vector<std::pair<std::vector<pittore::vector::Anchor>, bool>>;

struct ShapeGeometry {
    const char* name = "";
    ShapeSubPaths subpaths;
};

std::optional<ShapeGeometry> shapeGeometry(const Graph& g, const Node* shpe,
                                           float x0, float y0, float x1, float y1);
void transformAnchors(pittore::vector::VectorPath& path, const Mat& m);
// Free-path curve records ("PCrv" Crvs→Data, 18-byte f64/f64/marker records
// with a closed flag per subpath) as a transformed path. Shared by the shape
// emitter and vector-crop masks so both read the records identically.
std::optional<pittore::vector::VectorPath> pathFromCurveRecords(const Graph& g, const Node* node,
                                                                const Mat& lctm);
std::optional<pittore::vector::VectorShape> vectorPaint(
    const Graph& g, const Node* node, const Mat& ctm,
    pittore::vector::VectorPath path, bool evenOdd,
    std::optional<pittore::vector::GradientFill>& gradient);
std::string utf8Prefix(const std::string& s, std::size_t n);
std::string trimmed(const std::string& s);
std::size_t textLineCount(const std::string& s);

}  // namespace af_detail
}  // namespace pittore::io
