#include "engine/io/af_layers.h"
#include "engine/io/af.h"
#include "engine/core/parallel.h"
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

// ---------------------------------------------------------------------------
// Affine placement (port of import/resample.rs + raster.rs place_raster)
// ---------------------------------------------------------------------------

void mirrorBitmap(Bitmap& img, bool horizontal, bool vertical) {
    const std::size_t w = img.w, h = img.h;
    if (horizontal)
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w / 2; ++x)
                for (int c = 0; c < 4; ++c)
                    std::swap(img.px[(y * w + x) * 4 + c],
                              img.px[(y * w + (w - 1 - x)) * 4 + c]);
    if (vertical)
        for (std::size_t y = 0; y < h / 2; ++y)
            for (std::size_t x = 0; x < w * 4; ++x)
                std::swap(img.px[y * w * 4 + x], img.px[(h - 1 - y) * w * 4 + x]);
}


Bitmap resampleTo(Bitmap img, std::uint32_t dw, std::uint32_t dh) {
    if ((img.w == dw && img.h == dh) || dw == 0 || dh == 0) return img;
    const std::size_t sw = img.w, sh = img.h, dW = dw, dH = dh;
    struct Tap {
        std::size_t x0, x1;
        float w;
    };
    std::vector<Tap> xtaps(dW);
    for (std::size_t x = 0; x < dW; ++x) {
        const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) /
                             static_cast<float>(dW) - 0.5f;
        std::size_t x0 = static_cast<std::size_t>(std::max(0.0f, std::floor(fx)));
        if (x0 >= sw) x0 = sw - 1;
        const std::size_t x1 = std::min(x0 + 1, sw - 1);
        const float wx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
        xtaps[x] = Tap{x0, x1, wx};
    }
    Bitmap out;
    out.w = dw;
    out.h = dh;
    out.px.assign(dW * dH * 4, 0);
    // Each output row is written by exactly one thread from read-only taps and
    // source rows, so this is bit-identical to the serial sweep.
    auto row = [&](std::size_t y) {
        const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) /
                             static_cast<float>(dH) - 0.5f;
        std::size_t y0 = static_cast<std::size_t>(std::max(0.0f, std::floor(fy)));
        if (y0 >= sh) y0 = sh - 1;
        const std::size_t y1 = std::min(y0 + 1, sh - 1);
        const float wy = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        const std::uint8_t* row0 = img.px.data() + y0 * sw * 4;
        const std::uint8_t* row1 = img.px.data() + y1 * sw * 4;
        std::uint8_t* orow = out.px.data() + y * dW * 4;
        for (std::size_t x = 0; x < dW; ++x) {
            const Tap& t = xtaps[x];
            for (int c = 0; c < 4; ++c) {
                const float top = row0[t.x0 * 4 + c] * (1.0f - t.w) + row0[t.x1 * 4 + c] * t.w;
                const float bot = row1[t.x0 * 4 + c] * (1.0f - t.w) + row1[t.x1 * 4 + c] * t.w;
                orow[x * 4 + c] = static_cast<std::uint8_t>(top * (1.0f - wy) + bot * wy + 0.5f);
            }
        }
    };
    pittore::core::parallel_rows(
        static_cast<std::uint32_t>(dH), [&](std::uint32_t y0, std::uint32_t y1) {
            for (std::size_t y = y0; y < y1; ++y) row(y);
        });
    return out;
}


std::optional<Placed> affineResample(const Bitmap& img, const Mat& map) {
    Mat inv;
    if (!matInvert(map, inv)) return std::nullopt;
    const double sw = img.w, sh = img.h;
    const double cx[4] = {0.0, sw, 0.0, sw};
    const double cy[4] = {0.0, 0.0, sh, sh};
    double lox = std::numeric_limits<double>::infinity();
    double loy = std::numeric_limits<double>::infinity();
    double hix = -std::numeric_limits<double>::infinity();
    double hiy = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 4; ++i) {
        const auto [x, y] = matApply(map, cx[i], cy[i]);
        if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
        lox = std::min(lox, x);
        loy = std::min(loy, y);
        hix = std::max(hix, x);
        hiy = std::max(hiy, y);
    }
    if (std::max({std::abs(lox), std::abs(loy), std::abs(hix), std::abs(hiy)}) > double(1 << 24))
        return std::nullopt;
    IntRect rect{static_cast<int>(std::floor(lox)), static_cast<int>(std::floor(loy)),
                 static_cast<int>(std::ceil(hix)), static_cast<int>(std::ceil(hiy))};
    if (rect.empty()) return std::nullopt;
    const std::size_t dw = rect.width(), dh = rect.height();
    if (dw * dh > kMaxPixels) return std::nullopt;

    bool opaque = true;
    for (std::size_t i = 3; i < img.px.size(); i += 4)
        if (img.px[i] != 0xff) {
            opaque = false;
            break;
        }
    (void)opaque;

    const std::int64_t iw = img.w, ih = img.h;
    auto fetch = [&](std::int64_t x, std::int64_t y) -> std::array<float, 4> {
        if (x < 0 || y < 0 || x >= iw || y >= ih) return {0.0f, 0.0f, 0.0f, 0.0f};
        const std::size_t at = (static_cast<std::size_t>(y) * iw + static_cast<std::size_t>(x)) * 4;
        const float a = img.px[at + 3];
        return {img.px[at] * a, img.px[at + 1] * a, img.px[at + 2] * a, a};
    };
    Placed out;
    out.rect = rect;
    out.img.w = static_cast<std::uint32_t>(dw);
    out.img.h = static_cast<std::uint32_t>(dh);
    out.img.px.assign(dw * dh * 4, 0);
    for (std::size_t y = 0; y < dh; ++y) {
        const double py = rect.y0 + static_cast<double>(y) + 0.5;
        const double rowSx = inv.m[1] * py + inv.m[2] + inv.m[0] * (rect.x0 + 0.5);
        const double rowSy = inv.m[4] * py + inv.m[5] + inv.m[3] * (rect.x0 + 0.5);
        for (std::size_t x = 0; x < dw; ++x) {
            const double sx = rowSx + inv.m[0] * static_cast<double>(x);
            const double sy = rowSy + inv.m[3] * static_cast<double>(x);
            const double fx = sx - 0.5, fy = sy - 0.5;
            if (fx < -1.0 || fy < -1.0 || fx > sw || fy > sh) continue;
            const std::int64_t tx = static_cast<std::int64_t>(fx);
            const std::int64_t ty = static_cast<std::int64_t>(fy);
            const std::int64_t x0 = tx - (static_cast<double>(tx) > fx ? 1 : 0);
            const std::int64_t y0 = ty - (static_cast<double>(ty) > fy ? 1 : 0);
            const float wx = static_cast<float>(fx - static_cast<double>(x0));
            const float wy = static_cast<float>(fy - static_cast<double>(y0));
            const float wts[4] = {(1.0f - wx) * (1.0f - wy), wx * (1.0f - wy),
                                  (1.0f - wx) * wy, wx * wy};
            const std::int64_t dxs[4] = {0, 1, 0, 1};
            const std::int64_t dys[4] = {0, 0, 1, 1};
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const auto p = fetch(x0 + dxs[k], y0 + dys[k]);
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * wts[k];
            }
            const std::size_t at = (y * dw + x) * 4;
            if (acc[3] > 1e-6f) {
                const float un = 1.0f / acc[3];
                for (int c = 0; c < 3; ++c)
                    out.img.px[at + c] =
                        static_cast<std::uint8_t>(std::clamp(acc[c] * un + 0.5f, 0.0f, 255.0f));
                out.img.px[at + 3] =
                    static_cast<std::uint8_t>(std::clamp(acc[3] + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}


std::optional<Placed> placeRaster(const Mat& map, Bitmap img) {
    if (!matAxisAligned(map)) return affineResample(img, map);
    const auto [ax, ay] = matApply(map, 0.0, 0.0);
    const auto [bx, by] = matApply(map, static_cast<double>(img.w), static_cast<double>(img.h));
    auto sane = [](double v) { return std::isfinite(v) && std::abs(v) < double(1 << 24); };
    IntRect rect;
    if (sane(ax) && sane(ay) && sane(bx) && sane(by))
        rect = IntRect{static_cast<int>(std::lround(std::min(ax, bx))),
                       static_cast<int>(std::lround(std::min(ay, by))),
                       static_cast<int>(std::lround(std::max(ax, bx))),
                       static_cast<int>(std::lround(std::max(ay, by)))};
    if (rect.empty()) rect = IntRect{0, 0, static_cast<int>(img.w), static_cast<int>(img.h)};
    Bitmap r = resampleTo(std::move(img), static_cast<std::uint32_t>(rect.width()),
                          static_cast<std::uint32_t>(rect.height()));
    mirrorBitmap(r, map.m[0] < 0.0, map.m[4] < 0.0);
    if (r.px.size() != std::size_t(rect.width()) * rect.height() * 4) return std::nullopt;
    return Placed{rect, std::move(r)};
}

std::optional<Placed> cropPlaced(Bitmap img, const IntRect& at, const IntRect& box) {
    if (img.w == 0 || img.h == 0 ||
        img.px.size() != std::size_t(img.w) * img.h * 4)
        return std::nullopt;
    if (at.width() != static_cast<int>(img.w) ||
        at.height() != static_cast<int>(img.h))
        return std::nullopt;
    const int x0 = std::max(at.x0, box.x0);
    const int y0 = std::max(at.y0, box.y0);
    const int x1 = std::min(at.x1, box.x1);
    const int y1 = std::min(at.y1, box.y1);
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    if (x0 == at.x0 && y0 == at.y0 && x1 == at.x1 && y1 == at.y1)
        return Placed{at, std::move(img)};
    const int w = x1 - x0;
    const int h = y1 - y0;
    const std::size_t srcPitch = static_cast<std::size_t>(at.width()) * 4;
    Bitmap out;
    out.w = static_cast<std::uint32_t>(w);
    out.h = static_cast<std::uint32_t>(h);
    out.px.resize(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(&out.px[static_cast<std::size_t>(y) * w * 4],
                    &img.px[static_cast<std::size_t>(y0 - at.y0 + y) * srcPitch +
                            static_cast<std::size_t>(x0 - at.x0) * 4],
                    static_cast<std::size_t>(w) * 4);
    return Placed{IntRect{x0, y0, x1, y1}, std::move(out)};
}

}  // namespace af_detail
}  // namespace pittore::io
