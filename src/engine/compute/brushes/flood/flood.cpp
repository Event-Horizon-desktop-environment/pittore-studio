#include "engine/compute/brushes/flood/flood.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "engine/core/pixel.h"
#include "engine/color/icc_convert.h"

namespace pittore::compute {

namespace {

// Largest per-channel difference between two straight-alpha colours. Flood
// tolerance is measured this way (rather than Euclidean) so the same numeric
// tolerance behaves the same on any one channel.
inline float colour_distance(const RGBAf& a, const RGBAf& b) {
    return std::max(std::max(std::fabs(a.r - b.r), std::fabs(a.g - b.g)),
                    std::max(std::fabs(a.b - b.b), std::fabs(a.a - b.a)));
}

}  // namespace

bool flood_fill_host(const RGBAf* match, RGBAf* dst, std::uint32_t w,
                     std::uint32_t h, int seedX, int seedY, float tolerance,
                     bool contiguous, bool antialias, float opacity,
                     const RGBAf& color, bool erase, int* bbox,
                     const SelectionMask* selection,
                     NativeCmykPaint* nativeCmyk) {
    if (!match || !dst || w == 0 || h == 0) return false;
    if (seedX < 0 || seedY < 0 || seedX >= static_cast<int>(w) ||
        seedY >= static_cast<int>(h))
        return false;
    // A fill is confined to the selection, seed included.
    if (selection && selection->coverage(seedX, seedY) <= 0.0f) return false;

    const std::size_t n = static_cast<std::size_t>(w) * h;
    const std::size_t seed = static_cast<std::size_t>(seedY) * w +
                             static_cast<std::size_t>(seedX);
    const RGBAf seedColor = match[seed];
    const float tol = std::clamp(tolerance, 0.0f, 1.0f);
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    if (op <= 0.0f) return false;

    std::vector<std::uint8_t> region(n, 0);
    if (contiguous) {
        // Iterative 4-connected scanline-free flood: push neighbours that are
        // within tolerance. A vector is the stack so the depth is heap-bounded.
        std::vector<int> stack;
        stack.reserve(1024);
        region[seed] = 1;
        stack.push_back(static_cast<int>(seed));
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int x = idx % static_cast<int>(w);
            const int y = idx / static_cast<int>(w);
            auto tryPush = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(w) ||
                    ny >= static_cast<int>(h))
                    return;
                if (selection && selection->coverage(nx, ny) <= 0.0f) return;
                const std::size_t ni = static_cast<std::size_t>(ny) * w +
                                       static_cast<std::size_t>(nx);
                if (region[ni]) return;
                if (colour_distance(match[ni], seedColor) <= tol) {
                    region[ni] = 1;
                    stack.push_back(static_cast<int>(ni));
                }
            };
            tryPush(x - 1, y);
            tryPush(x + 1, y);
            tryPush(x, y - 1);
            tryPush(x, y + 1);
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            if (selection &&
                selection->coverage(static_cast<int>(i % w),
                                    static_cast<int>(i / w)) <= 0.0f)
                continue;
            if (colour_distance(match[i], seedColor) <= tol) region[i] = 1;
        }
    }

    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    bool any = false;
    auto applyPixel = [&](int x, int y, float coverage) {
        float a = coverage * op;
        if (selection) a *= selection->coverage(x, y);
        if (a <= 0.0f) return;
        RGBAf& d = dst[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
        const RGBAf before = d;
        std::array<std::uint16_t, 4> beforeInk{};
        const bool hasNativeInk = !erase && nativeCmyk && nativeCmyk->pixels &&
                                  nativeCmyk->preview;
        if (hasNativeInk)
            beforeInk = nativeCmyk->pixels[static_cast<std::size_t>(y) * w + x];
        if (erase) {
            if (d.a <= 0.0f) return;
            d.a *= (1.0f - a);
        } else {
            const float inv_a = 1.0f - a;
            const float out_a = a + d.a * inv_a;
            if (out_a <= 0.0f) return;
            if (nativeCmyk && nativeCmyk->pixels && nativeCmyk->preview) {
                auto& ink = nativeCmyk->pixels[std::size_t(y) * w + x];
                float values[4];
                for (int c = 0; c < 4; ++c) {
                    const float mixed =
                        (a * (nativeCmyk->source[c] / 65535.0f) +
                         d.a * (ink[c] / 65535.0f) * inv_a) / out_a;
                    ink[c] = static_cast<std::uint16_t>(std::clamp(
                        std::lround(mixed * 65535.0f), 0L, 65535L));
                    values[c] = ink[c] / 65535.0f;
                }
                float rgb[3];
                nativeCmyk->preview->convert(values[0], values[1], values[2],
                                             values[3], rgb);
                d.r = rgb[0]; d.g = rgb[1]; d.b = rgb[2];
            } else {
                d.r = (a * color.r + d.r * d.a * inv_a) / out_a;
                d.g = (a * color.g + d.g * d.a * inv_a) / out_a;
                d.b = (a * color.b + d.b * d.a * inv_a) / out_a;
            }
            d.a = out_a;
        }
        // Only count a pixel that actually moved, so a no-op fill does not
        // fabricate an undo step.
        const bool inkChanged = hasNativeInk &&
            beforeInk != nativeCmyk->pixels[static_cast<std::size_t>(y) * w + x];
        if (colour_distance(before, d) < 1e-6f && !inkChanged) return;
        if (!any) {
            rx0 = rx1 = x;
            ry0 = ry1 = y;
            any = true;
        } else {
            rx0 = std::min(rx0, x);
            ry0 = std::min(ry0, y);
            rx1 = std::max(rx1, x);
            ry1 = std::max(ry1, y);
        }
    };

    // Region bounding box (used to bound the apply pass).
    int bx0 = static_cast<int>(w), by0 = static_cast<int>(h), bx1 = -1, by1 = -1;
    for (std::size_t i = 0; i < n; ++i) {
        if (!region[i]) continue;
        const int x = static_cast<int>(i % w);
        const int y = static_cast<int>(i / w);
        bx0 = std::min(bx0, x);
        by0 = std::min(by0, y);
        bx1 = std::max(bx1, x);
        by1 = std::max(by1, y);
    }
    if (bx1 < 0) return false;

    // Without antialiasing every region pixel is fully covered. With it, the
    // binary region is feathered by a 3x3 box so the boundary fades by one
    // pixel on both sides (a pixel outside the region may still catch a little
    // fill, which is exactly what a soft edge looks like). Each target pixel is
    // visited once so source-over is applied exactly once.
    const int margin = antialias ? 1 : 0;
    for (int y = std::max(0, by0 - margin); y <= std::min(static_cast<int>(h) - 1, by1 + margin); ++y) {
        for (int x = std::max(0, bx0 - margin); x <= std::min(static_cast<int>(w) - 1, bx1 + margin); ++x) {
            float cov;
            if (!antialias) {
                if (!region[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]) continue;
                cov = 1.0f;
            } else {
                // Feather by a 3x3 box. Normalise by the number of in-bounds
                // neighbours so a region touching the image border keeps full
                // coverage there; only a genuine region boundary fades.
                cov = 0.0f;
                int valid = 0;
                for (int sy = -1; sy <= 1; ++sy)
                    for (int sx = -1; sx <= 1; ++sx) {
                        const int cx = x + sx, cy = y + sy;
                        if (cx < 0 || cy < 0 || cx >= static_cast<int>(w) ||
                            cy >= static_cast<int>(h))
                            continue;
                        ++valid;
                        if (region[static_cast<std::size_t>(cy) * w +
                                   static_cast<std::size_t>(cx)])
                            cov += 1.0f;
                    }
                if (valid > 0) cov /= static_cast<float>(valid);
                if (cov <= 0.0f) continue;
            }
            applyPixel(x, y, cov);
        }
    }

    if (!any) return false;
    if (bbox) {
        bbox[0] = rx0;
        bbox[1] = ry0;
        bbox[2] = rx1 + 1;  // half-open
        bbox[3] = ry1 + 1;
    }
    return true;
}

}  // namespace pittore::compute
